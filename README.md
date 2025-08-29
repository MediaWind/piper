# Piper 🎵

Piper est un petit programme permettant de générer et de jouer de l’audio à partir de texte, avec prise en charge de plusieurs voix via des modèles **ONNX**.

---

## 🚀 Utilisation

### Lancer le programme

```bash
./piper
```

👉 Dans ce cas, la sortie sera automatiquement générée en **fichier `.wav`** dans le dossier **`out/`**.

### Jouer directement un morceau

```bash
./piper --play
```

---

## 📝 Créer un job (texte à voix)

Pour transformer du texte en voix, il faut créer un fichier JSON dans le dossier **`jobs/`** avec la structure suivante :

```json
[
    {
        "text": "Le texte à prononcer",
        "voice": "fr"
    },
    {
        "text": "blablabla",
        "voice": "nl"
    }
]
```

* **text** : le contenu à lire
* **voice** : le code de la voix (exemple : `"fr"`, `"nl"`)

👉 Le fichier sera automatiquement lu, converti en audio, joué et sauvegardé en `.wav` dans le dossier **`out/`**.

---

## 🎙️ Ajouter de nouvelles voix

Pour ajouter une voix :

1. Déposer les fichiers du modèle (`.onnx`) et de configuration (`.onnx.json`) dans le dossier **`voices/`**.

---

## Paramètres

```bash
./piper --play --length_scale 2 --espeak_data /var/www/data/piper/espeak-ng-data --voices_dir /var/www/data/piper/voices
```

--length_scale: Speed of speaking (1 = normal, < 1 is faster, > 1 is slower)
--espeak_data: espeak_data direcotry
--voices_dir: voices directory

## 📂 Organisation du projet

* `piper` → exécutable principal
* `voices/` → modèles de voix (`.onnx` + `.json`)
* `jobs/` → fichiers JSON décrivant les textes à lire
* `out/` → fichiers audio générés (`.wav`)
* `main.cpp` → point d’entrée du programme et configuration des voix

---


## Build for CMP4
1. build

```bash
make docker
```

2. Unzip tar.gz

```bash
tar -xvzf dist/piper_armv7.tar.gz
```

3. Move file to right place
a. put `piper` in /skeleton


👉 Résumé :

* **Sans option** → génère du `.wav` dans `out/`
* **Avec `--play`** → lecture directe
* **Avec fichier `jobs/*.json`** → lecture + génération automatique depuis du texte
