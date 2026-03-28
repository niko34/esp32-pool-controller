# Guide de compilation

## Table des partitions

Le projet utilise une table de partitions personnalisée pour ESP32 4MB:

| Partition | Offset     | Taille  | Usage |
|-----------|------------|---------|-------|
| nvs       | 0x9000     | 20KB    | Configuration NVS |
| otadata   | 0xE000     | 8KB     | Métadonnées OTA |
| app0      | 0x10000    | 1344KB  | Firmware slot 0 (OTA) |
| app1      | 0x160000   | 1344KB  | Firmware slot 1 (OTA) |
| spiffs    | 0x2B0000   | 1216KB  | Fichiers web (LittleFS) |
| history   | 0x3E0000   | 128KB   | Historique séparé (préservé lors des mises à jour) |

**Total utilisé**: ~4060KB sur 4096KB (4MB)

## Compilation

### 1. Compiler le firmware

```bash
pio run
```

### 2. Construire le filesystem LittleFS

⚠️ **Important**: N'utilisez PAS `pio run -t buildfs` car il utilise une mauvaise taille (128KB au lieu de 1216KB).

Utilisez plutôt le script fourni:

```bash
./build_fs.sh
```

Ce script:
1. **Minifie** automatiquement les fichiers HTML, CSS et JS (économie ~60KB)
2. **Construit** le filesystem avec la bonne taille (1216KB) qui correspond à la partition spiffs dans `partitions.csv`

Les fichiers sources restent dans `data/`, les fichiers minifiés sont générés dans `data-build/` (ignoré par git).

### 3. Uploader sur l'ESP32

#### Upload du firmware

```bash
pio run -t upload
```

#### Upload du filesystem

⚠️ **Important**: N'utilisez PAS `pio run -t uploadfs` car il reconstruit le filesystem avec la mauvaise taille.

Utilisez plutôt `esptool.py` directement pour uploader le fichier construit par `build_fs.sh`:

```bash
~/.platformio/penv/bin/esptool.py \
  --chip esp32 \
  --port /dev/cu.usbserial-0001 \
  --baud 115200 \
  write_flash 0x2B0000 .pio/build/esp32dev/littlefs.bin
```

**Note**: `0x2B0000` est l'offset de la partition littlefs défini dans `partitions.csv`.

#### Déploiement complet

Pour tout uploader en une seule fois:

```bash
# 1. Build firmware
pio run

# 2. Build filesystem
./build_fs.sh

# 3. Upload firmware
pio run -t upload

# 4. Upload filesystem
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-0001 --baud 115200 \
  write_flash 0x2B0000 .pio/build/esp32dev/littlefs.bin
```

### Compilation complète en une commande

Pour compiler firmware + filesystem et copier les deux `.bin` à la racine du projet (utile avant une mise à jour OTA manuelle) :

```bash
./build_all.sh
```

Ce script enchaîne `pio run`, `build_fs.sh`, puis copie `firmware.bin` et `littlefs.bin` à la racine. Ces fichiers peuvent ensuite être uploadés via l'interface web (Système → Mise à jour OTA).

## Structure des fichiers

- `partitions.csv` - Table de partitions personnalisée
- `build_all.sh` - Script tout-en-un : compile firmware + filesystem, copie les `.bin` à la racine
- `build_fs.sh` - Script pour construire LittleFS (avec minification)
- `minify.js` - Script de minification HTML/CSS/JS (Node.js)
- `data/` - Fichiers web sources (HTML, CSS, JS, images)
- `data-build/` - Fichiers web minifiés (généré automatiquement, ignoré par git)
- `src/` - Code source C++

## Notes

- L'historique est stocké dans une partition séparée (`history`) qui n'est PAS effacée lors des mises à jour OTA du filesystem
- Le fichier d'historique sera `/history/history.json` sur la partition dédiée
- Si la partition history n'est pas disponible, le système utilise `/history.json` sur littlefs en fallback
