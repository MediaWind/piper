#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

#include <sys/inotify.h> 
#include <unistd.h> 

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "json.hpp"
#include "piper.hpp"

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

enum OutputType { OUTPUT_FILE, OUTPUT_DIRECTORY, OUTPUT_STDOUT, OUTPUT_RAW, OUTPUT_PLAY };

struct RunConfig {
  // Path to .onnx voice file
  filesystem::path modelPath;

  // Path to JSON voice config file
  filesystem::path modelConfigPath;

  // Type of output to produce.
  // Default is to write a WAV file in the current directory.
  OutputType outputType = OUTPUT_DIRECTORY;

  // Path for output
  optional<filesystem::path> outputPath = filesystem::path("out/");

  // Numerical id of the default speaker (multi-speaker voices)
  optional<piper::SpeakerId> speakerId;

  // Amount of noise to add during audio generation
  optional<float> noiseScale;

  // Speed of speaking (1 = normal, < 1 is faster, > 1 is slower)
  optional<float> lengthScale;

  // Variation in phoneme lengths
  optional<float> noiseW;

  // Seconds of silence to add after each sentence
  optional<float> sentenceSilenceSeconds;

  // Path to espeak-ng data directory (default is next to piper executable)
  optional<filesystem::path> eSpeakDataPath;

  // Path to libtashkeel ort model
  // https://github.com/mush42/libtashkeel/
  optional<filesystem::path> tashkeelModelPath;

  // stdin input is lines of JSON instead of text with format:
  // {
  //   "text": str,               (required)
  //   "speaker_id": int,         (optional)
  //   "speaker": str,            (optional)
  //   "output_file": str,        (optional)
  // }
  bool jsonInput = false;

  // Seconds of extra silence to insert after a single phoneme
  optional<std::map<piper::Phoneme, float>> phonemeSilenceSeconds;

  // true to use CUDA execution provider
  bool useCuda = false;
};

struct JobItem {
  std::string voice;
  std::string text;
};

void parseArgs(int argc, char *argv[], RunConfig &runConfig);
void rawOutputProc(vector<int16_t> &sharedAudioBuffer, mutex &mutAudio,
  condition_variable &cvAudio, bool &audioReady,
  bool &audioFinished);

struct VoiceEntry {
  std::string key;
  fs::path onnx;
  fs::path json;
};


static bool parseJobJson(const fs::path &file, std::vector<JobItem> &outJobs) {
  try {
    std::ifstream in(file);
    if (!in.good()) return false;

    nlohmann::json j; 
    in >> j;

    auto parseOne = [](const nlohmann::json& x) -> JobItem {
      JobItem it;
      it.text  = x.at("text").get<std::string>();
      it.voice = x.value("voice", std::string("fr"));  // default = "fr"
      return it;
    };

    outJobs.clear();
    if (j.is_array()) {
      outJobs.reserve(j.size());
      for (const auto& x : j) outJobs.push_back(parseOne(x));
    } else if (j.is_object()) {
      outJobs.push_back(parseOne(j));
    } else {
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

static void applyOverridesToVoice(piper::Voice& v, const RunConfig& runConfig) {
  if (runConfig.noiseScale)   v.synthesisConfig.noiseScale   = runConfig.noiseScale.value();
  if (runConfig.lengthScale)  v.synthesisConfig.lengthScale  = runConfig.lengthScale.value();
  if (runConfig.noiseW)       v.synthesisConfig.noiseW       = runConfig.noiseW.value();
  if (runConfig.sentenceSilenceSeconds)
    v.synthesisConfig.sentenceSilenceSeconds = runConfig.sentenceSilenceSeconds.value();
  if (runConfig.phonemeSilenceSeconds) {
    if (!v.synthesisConfig.phonemeSilenceSeconds) {
      v.synthesisConfig.phonemeSilenceSeconds = runConfig.phonemeSilenceSeconds;
    } else {
      for (const auto& [ph, sec] : *runConfig.phonemeSilenceSeconds)
        v.synthesisConfig.phonemeSilenceSeconds->try_emplace(ph, sec);
    }
  }
}

static piper::Voice& getOrLoadVoice(const std::string& key,
  piper::PiperConfig& cfg,
  std::map<std::string, piper::Voice>& voices,
  const RunConfig& runConfig)
{
  // 1) Already cached?
  if (auto it = voices.find(key); it != voices.end()) {
    spdlog::debug("Voice [{}] already cached", key);
    return it->second;
  }

  // 2) Build paths directly from key
  fs::path onnxPath = "./voices/" + key + ".onnx";
  fs::path jsonPath = "./voices/" + key + ".onnx.json";

  if (!fs::exists(onnxPath) || !fs::exists(jsonPath)) {
    spdlog::error("Missing files for [{}]: {} / {}", key, onnxPath.string(), jsonPath.string());
    onnxPath = "./voices/fr_siwis.onnx";
    jsonPath = "./voices/fr_siwis.onnx.json";
  }

  // 3) Load
  spdlog::info("Loading [{}]: {} (config={})", key, onnxPath.string(), jsonPath.string());
  auto t0 = std::chrono::steady_clock::now();

  piper::Voice v;
  auto sid = runConfig.speakerId;
  loadVoice(cfg, onnxPath.string(), jsonPath.string(), v, sid, runConfig.useCuda);

  auto t1 = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(t1 - t0).count();

  applyOverridesToVoice(v, runConfig);
  spdlog::info("Voice [{}] loaded in {:.3f} seconds", key, elapsed);

  // 4) Insert & return
  auto [it2, inserted] = voices.emplace(key, std::move(v));
  return it2->second;
}

// Watcher: call onJob(voice, text) when a new job is added
static void watchDir(
  const fs::path &dir,
  const std::function<void(const std::string&, const std::string&)> &onJob)
{
  int fd = inotify_init1(IN_NONBLOCK);
  if (fd < 0) throw std::runtime_error("inotify_init1 failed");

  int wd = inotify_add_watch(fd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
  if (wd < 0) { close(fd); throw std::runtime_error("inotify_add_watch failed"); }

  spdlog::info(dir.c_str());

  std::vector<char> buf(16 * 1024);
  while (true) {
    int len = read(fd, buf.data(), (int)buf.size());
    if (len < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
    int i = 0;
    while (i < len) {
      auto *ev = reinterpret_cast<inotify_event*>(&buf[i]);
      if (ev->len > 0 && !(ev->mask & IN_ISDIR)) {
        std::string name(ev->name);
        if (name.size() >= 5 && name.substr(name.size()-5) == ".json" &&
            (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)))
        {
          fs::path job = dir / name;
          std::vector<JobItem> items;
          if (fs::exists(job) && parseJobJson(job, items)) {
            // we execute all items in the job
            for (const auto& it : items) {
              onJob(it.voice, it.text);
            }
          }
          std::error_code ec; fs::remove(job, ec); // remove the job file
        }
      }
      i += sizeof(inotify_event) + ev->len;
    }
  }

  inotify_rm_watch(fd, wd);
  close(fd);
}

static void playTextNow(piper::PiperConfig& cfg, piper::Voice& voice, const std::string& text) {
  const int rate = voice.synthesisConfig.sampleRate;   // e.g., 22050
  const int ch   = voice.synthesisConfig.channels;     // e.g., 1

  // You can also tune ALSA buffering:
  // -B = buffer time, -F = period time (in µs). Example: 200ms / 20ms
  std::string cmd = "aplay -q -f S16_LE -c " + std::to_string(ch) +
                    " -r " + std::to_string(rate) +
                    " -B 200000 -F 20000 -";

  FILE* pipe = popen(cmd.c_str(), "w");
  if (!pipe) { spdlog::error("popen aplay failed"); return; }

  // Disable stdio buffering: write() directly to the pipe
  setvbuf(pipe, nullptr, _IONBF, 0);

  // 1) Startup silence (~200 ms)
  const int start_silence_ms = 200;
  const size_t start_samples = (size_t)((start_silence_ms / 1000.0) * rate * ch);
  std::vector<int16_t> zeros(start_samples, 0);
  fwrite(zeros.data(), sizeof(int16_t), zeros.size(), pipe);

  std::vector<int16_t> chunk;           // filled by piper::textToAudio
  piper::SynthesisResult res{};

  // 2) Callback: COPY the chunk then write the copy
  auto onChunk = [&]() {
    if (!chunk.empty()) {
      std::vector<int16_t> local(chunk.begin(), chunk.end());
      size_t n = fwrite(local.data(), sizeof(int16_t), local.size(), pipe);
      (void)n;
      // No fflush: _IONBF and aplay read quickly; avoids the cost.
    }
  };

  // 3) Stream TTS
  piper::textToAudio(cfg, voice, text, chunk, res, onChunk);

  // 4) Ending silence (~100 ms) to avoid abrupt cut
  const int end_silence_ms = 100;
  const size_t end_samples = (size_t)((end_silence_ms / 1000.0) * rate * ch);
  if (end_samples > 0) {
    std::vector<int16_t> tail(end_samples, 0);
    fwrite(tail.data(), sizeof(int16_t), tail.size(), pipe);
  }

  // Finish
  fflush(pipe);
  pclose(pipe);

  spdlog::info("RTF={} (infer={}s, audio={}s)", res.realTimeFactor, res.inferSeconds, res.audioSeconds);
}

// ----------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  spdlog::set_default_logger(spdlog::stderr_color_st("piper"));

  RunConfig runConfig;
  parseArgs(argc, argv, runConfig);

#ifdef _WIN32
  // Required on Windows to show IPA symbols
  SetConsoleOutputCP(CP_UTF8);
#endif

  piper::PiperConfig piperConfig;
  std::map<std::string, piper::Voice> voices;

  // Get the path to the piper executable so we can locate espeak-ng-data, etc.
  // next to it.
#ifdef _MSC_VER
  auto exePath = []() {
    wchar_t moduleFileName[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, moduleFileName, std::size(moduleFileName));
    return filesystem::path(moduleFileName);
  }();
#else
#ifdef __APPLE__
  auto exePath = []() {
    char moduleFileName[PATH_MAX] = {0};
    uint32_t moduleFileNameSize = std::size(moduleFileName);
    _NSGetExecutablePath(moduleFileName, &moduleFileNameSize);
    return filesystem::path(moduleFileName);
  }();
#else
  auto exePath = filesystem::canonical("/proc/self/exe");
#endif
#endif


  piperConfig.useESpeak = true;
  if (runConfig.eSpeakDataPath) {
    piperConfig.eSpeakDataPath = runConfig.eSpeakDataPath->string();
  } else {
    std::vector<std::string> tryPaths = {
      (exePath.parent_path() / "espeak-ng-data").string(),
      "/usr/share/espeak-ng-data"
    };
    for (const auto& p : tryPaths) {
      if (!p.empty() && fs::exists(fs::path(p) / "phontab")) { piperConfig.eSpeakDataPath = p; break; }
    }
    if (piperConfig.eSpeakDataPath.empty()) {
      spdlog::error("eSpeak data introuvable (pas de 'phontab'). Passe --espeak_data <chemin>.");
      return 1;
    }
  }
  spdlog::info("eSpeak data: {}", piperConfig.eSpeakDataPath);

  piper::initialize(piperConfig);


  if (runConfig.outputType == OUTPUT_DIRECTORY) {
    runConfig.outputPath = filesystem::absolute(runConfig.outputPath.value());
    spdlog::info("Output directory: {}", runConfig.outputPath.value().string());
  }

  fs::path watchPath = "./jobs";
  fs::path outDir   = (runConfig.outputPath ? runConfig.outputPath.value() : fs::path("./out"));
  std::error_code mkec;
  fs::create_directories(watchPath, mkec);
  fs::create_directories(outDir, mkec);
  spdlog::info("Watch dir: {}", watchPath.string());
  spdlog::info("Out dir  : {}", outDir.string());

  auto now_ts_ns = [](){
    const auto now = chrono::system_clock::now();
    return std::to_string(chrono::duration_cast<chrono::nanoseconds>(now.time_since_epoch()).count());
  };

  piper::SynthesisResult result;
  watchDir(watchPath, [&](const std::string &voiceKey, const std::string &text){
    piper::Voice& sel = getOrLoadVoice(voiceKey, piperConfig, voices, runConfig);

    
    if (runConfig.outputType == OUTPUT_PLAY) {      // <-- Play immediately
      spdlog::info("Playing text now (voice={})", voiceKey);
      playTextNow(piperConfig, sel, text);
      return;
    }
    
    // Synthesize to WAV file
    const auto ts = now_ts_ns();
    filesystem::path outputPath = outDir / (voiceKey + "_" + ts + ".wav");
    ofstream audioFile(outputPath.string(), ios::binary);
    result = {};
    piper::textToWavFile(piperConfig, sel, text, audioFile, result);
    cout << outputPath.string() << endl;
    spdlog::info("Real-time factor: {} (infer={} sec, audio={} sec)",
      result.realTimeFactor, result.inferSeconds, result.audioSeconds);
  });

  piper::terminate(piperConfig);

  return EXIT_SUCCESS;
}

// ----------------------------------------------------------------------------

void rawOutputProc(vector<int16_t> &sharedAudioBuffer, mutex &mutAudio,
  condition_variable &cvAudio, bool &audioReady,
  bool &audioFinished) {
  vector<int16_t> internalAudioBuffer;
  while (true) {
    {
      unique_lock lockAudio{mutAudio};
      cvAudio.wait(lockAudio, [&audioReady] { return audioReady; });

      if (sharedAudioBuffer.empty() && audioFinished) {
        break;
      }

      copy(sharedAudioBuffer.begin(), sharedAudioBuffer.end(), back_inserter(internalAudioBuffer));

      sharedAudioBuffer.clear();

      if (!audioFinished) {
        audioReady = false;
      }
    }

    cout.write((const char *)internalAudioBuffer.data(),
               sizeof(int16_t) * internalAudioBuffer.size());
    cout.flush();
    internalAudioBuffer.clear();
  }

} // rawOutputProc

// ----------------------------------------------------------------------------

void printUsage(char *argv[]) {
  cerr << endl;
  cerr << "usage: " << argv[0] << " [options]" << endl;
  cerr << endl;
  cerr << "options:" << endl;
  cerr << "   -h        --help              show this message and exit" << endl;
  cerr << "   -m  FILE  --model       FILE  path to onnx model file" << endl;
  cerr << "   -c  FILE  --config      FILE  path to model config file "
          "(default: model path + .json)"
       << endl;
  cerr << "   -f  FILE  --output_file FILE  path to output WAV file ('-' for "
          "stdout)"
       << endl;
  cerr << "   -d  DIR   --output_dir  DIR   path to output directory (default: "
          "cwd)"
       << endl;
  cerr << "   --output_raw                  output raw audio to stdout as it "
          "becomes available"
       << endl;
  cerr << "   -s  NUM   --speaker     NUM   id of speaker (default: 0)" << endl;
  cerr << "   --noise_scale           NUM   generator noise (default: 0.667)"
       << endl;
  cerr << "   --length_scale          NUM   phoneme length (default: 1.0)"
       << endl;
  cerr << "   --noise_w               NUM   phoneme width noise (default: 0.8)"
       << endl;
  cerr << "   --sentence_silence      NUM   seconds of silence after each "
          "sentence (default: 0.2)"
       << endl;
  cerr << "   --espeak_data           DIR   path to espeak-ng data directory"
       << endl;
  cerr << "   --tashkeel_model        FILE  path to libtashkeel onnx model "
          "(arabic)"
       << endl;
  cerr << "   --json-input                  stdin input is lines of JSON "
          "instead of plain text"
       << endl;
  cerr << "   --use-cuda                    use CUDA execution provider"
       << endl;
  cerr << "   --debug                       print DEBUG messages to the console"
       << endl;
  cerr << "   -q       --quiet              disable logging" << endl;
  cerr << endl;
}

void ensureArg(int argc, char *argv[], int argi) {
  if ((argi + 1) >= argc) {
    printUsage(argv);
    exit(0);
  }
}

// Parse command-line arguments
void parseArgs(int argc, char *argv[], RunConfig &runConfig) {
  optional<filesystem::path> modelConfigPath;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-m" || arg == "--model") {
      ensureArg(argc, argv, i);
      runConfig.modelPath = filesystem::path(argv[++i]);
    } else if (arg == "-c" || arg == "--config") {
      ensureArg(argc, argv, i);
      modelConfigPath = filesystem::path(argv[++i]);
    } else if (arg == "-f" || arg == "--output_file" || arg == "--output-file") {
      ensureArg(argc, argv, i);
      std::string filePath = argv[++i];
      if (filePath == "-") {
        runConfig.outputType = OUTPUT_STDOUT;
        runConfig.outputPath = nullopt;
      } else {
        runConfig.outputType = OUTPUT_FILE;
        runConfig.outputPath = filesystem::path(filePath);
      }
    } else if (arg == "-d" || arg == "--output_dir" || arg == "output-dir") {
      ensureArg(argc, argv, i);
      runConfig.outputType = OUTPUT_DIRECTORY;
      runConfig.outputPath = filesystem::path(argv[++i]);
    } else if (arg == "--output_raw" || arg == "--output-raw") {
      runConfig.outputType = OUTPUT_RAW;
    } else if (arg == "--output_raw" || arg == "--play") {
      runConfig.outputType = OUTPUT_PLAY;
    } else if (arg == "-s" || arg == "--speaker") {
      ensureArg(argc, argv, i);
      runConfig.speakerId = (piper::SpeakerId)stol(argv[++i]);
    } else if (arg == "--noise_scale" || arg == "--noise-scale") {
      ensureArg(argc, argv, i);
      runConfig.noiseScale = stof(argv[++i]);
    } else if (arg == "--length_scale" || arg == "--length-scale") {
      ensureArg(argc, argv, i);
      runConfig.lengthScale = stof(argv[++i]);
    } else if (arg == "--noise_w" || arg == "--noise-w") {
      ensureArg(argc, argv, i);
      runConfig.noiseW = stof(argv[++i]);
    } else if (arg == "--sentence_silence" || arg == "--sentence-silence") {
      ensureArg(argc, argv, i);
      runConfig.sentenceSilenceSeconds = stof(argv[++i]);
    } else if (arg == "--phoneme_silence" || arg == "--phoneme-silence") {
      ensureArg(argc, argv, i);
      ensureArg(argc, argv, i + 1);
      auto phonemeStr = std::string(argv[++i]);
      if (!piper::isSingleCodepoint(phonemeStr)) {
        std::cerr << "Phoneme '" << phonemeStr
                  << "' is not a single codepoint (--phoneme_silence)"
                  << std::endl;
        exit(1);
      }

      if (!runConfig.phonemeSilenceSeconds) {
        runConfig.phonemeSilenceSeconds.emplace();
      }

      auto phoneme = piper::getCodepoint(phonemeStr);
      (*runConfig.phonemeSilenceSeconds)[phoneme] = stof(argv[++i]);
    } else if (arg == "--espeak_data" || arg == "--espeak-data") {
      ensureArg(argc, argv, i);
      runConfig.eSpeakDataPath = filesystem::path(argv[++i]);
    } else if (arg == "--tashkeel_model" || arg == "--tashkeel-model") {
      ensureArg(argc, argv, i);
      runConfig.tashkeelModelPath = filesystem::path(argv[++i]);
    } else if (arg == "--json_input" || arg == "--json-input") {
      runConfig.jsonInput = true;
    } else if (arg == "--use_cuda" || arg == "--use-cuda") {
      runConfig.useCuda = true;
    } else if (arg == "--version") {
      std::cout << piper::getVersion() << std::endl;
      exit(0);
    } else if (arg == "--debug") {
      // Set DEBUG logging
      spdlog::set_level(spdlog::level::debug);
    } else if (arg == "-q" || arg == "--quiet") {
      // diable logging
      spdlog::set_level(spdlog::level::off);
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv);
      exit(0);
    }
  }

  // Verify model file exists
  // ifstream modelFile(runConfig.modelPath.c_str(), ios::binary);
  // if (!modelFile.good()) {
  //   throw runtime_error("Model file doesn't exist");
  // }

  // if (!modelConfigPath) {
  //   runConfig.modelConfigPath =
  //       filesystem::path(runConfig.modelPath.string() + ".json");
  // } else {
  //   runConfig.modelConfigPath = modelConfigPath.value();
  // }

  // // Verify model config exists
  // ifstream modelConfigFile(runConfig.modelConfigPath.c_str());
  // if (!modelConfigFile.good()) {
  //   throw runtime_error("Model config doesn't exist");
  // }
}
