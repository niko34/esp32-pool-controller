# Guide de compilation

## Table des partitions

Le projet utilise une table de partitions personnalisée pour ESP32 4MB:

| Partition | Taille  | Usage |
|-----------|---------|-------|
| nvs       | 20KB    | Configuration NVS |
| otadata   | 8KB     | Métadonnées OTA |
| app0      | 1280KB  | Firmware slot 0 (OTA) |
| app1      | 1280KB  | Firmware slot 1 (OTA) |
| littlefs  | 1344KB  | Fichiers web |
| history   | 128KB   | Historique séparé (préservé lors des mises à jour) |

**Total**: 4096KB (4MB)

## Compilation

### 1. Compiler le firmware

```bash
pio run
```

### 2. Construire le filesystem LittleFS

⚠️ **Important**: N'utilisez PAS `pio run -t buildfs` car il utilise une mauvaise taille (128KB au lieu de 1344KB).

Utilisez plutôt le script fourni:

```bash
./build_fs.sh
```

Ce script construit le filesystem avec la bonne taille (1344KB) qui correspond à la partition littlefs dans `partitions.csv`.

### 3. Uploader sur l'ESP32

#### Upload du firmware

```bash
pio run -t upload
```

#### Upload du filesystem

⚠️ **Important**: N'utilisez PAS `pio run -t uploadfs` car il reconstruit le filesystem avec la mauvaise taille.

Utilisez plutôt `esptool.py` directement pour uploader le fichier construit par `build_fs.sh`:

```bash
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32 \
  --port /dev/cu.usbserial-210 \
  --baud 115200 \
  write_flash 0x290000 .pio/build/esp32dev/littlefs.bin
```

**Note**: `0x290000` est l'offset de la partition littlefs défini dans `partitions.csv`.

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
  --chip esp32 --port /dev/cu.usbserial-210 --baud 115200 \
  write_flash 0x290000 .pio/build/esp32dev/littlefs.bin
```

## Structure des fichiers

- `partitions.csv` - Table de partitions personnalisée
- `build_fs.sh` - Script pour construire LittleFS avec la bonne taille
- `data/` - Fichiers web (HTML, CSS, JS, images)
- `src/` - Code source C++

## Notes

- L'historique est stocké dans une partition séparée (`history`) qui n'est PAS effacée lors des mises à jour OTA du filesystem
- Le fichier d'historique sera `/history/history.json` sur la partition dédiée
- Si la partition history n'est pas disponible, le système utilise `/history.json` sur littlefs en fallback
