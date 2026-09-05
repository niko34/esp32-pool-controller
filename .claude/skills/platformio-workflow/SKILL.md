---
name: platformio-workflow
description: Workflow et commandes PlatformIO pour ESP32 Pool Controller. À charger dès qu'il faut compiler, flasher, déployer le firmware ou le filesystem, ou lancer des tests embarqués/natifs. Couvre pio run, upload USB, OTA, build FS LittleFS 1088KB, monitor série et décodage backtrace.
---

# PlatformIO — workflow ESP32 Pool Controller

## Configuration

- `platformio.ini` : env unique `[env:esp32dev]` avec `platform = espressif32@6.9.0`, `framework = arduino`
- Partitions personnalisées : `partitions.csv` (ne jamais modifier sans migration OTA)
- Filesystem : LittleFS, deux partitions :
  - `spiffs` : **1088 KB** (1 114 112 octets = 0x110000) — contient `data/` (frontend web)
  - `history` : **128 KB** (0x20000) — contient les fichiers d'historique
- ⚠️ Taille différente de la valeur par défaut PlatformIO — **utiliser `./build_fs.sh`**, pas `pio run -t buildfs`
- ⚠️ Le commentaire `1344KB` présent dans `platformio.ini` est **obsolète/erroné** — se fier à `build_fs.sh` et `partitions.csv`

## Compilation & upload USB

```bash
pio run                        # Compile (vérifie que ça build avant d'upload)
pio run -t upload              # Upload firmware via USB (port /dev/cu.usbserial-0001)
pio run -t clean               # Clean build
pio run --target size          # Taille flash / RAM
```

## Filesystem (données frontend dans data/)

```bash
./build_fs.sh                  # Build LittleFS avec bonne taille 1088KB
./build_fs.sh --upload         # Build + upload FS
# NE PAS utiliser : pio run -t buildfs (utilise mauvaise taille)
```

## Deploy (script tout-en-un)

```bash
./deploy.sh all                # Firmware + FS via USB
./deploy.sh firmware           # Firmware USB
./deploy.sh fs                 # FS USB
./deploy.sh ota-all            # Tout via WiFi OTA
./deploy.sh ota-firmware       # Firmware OTA
./deploy.sh ota-fs             # FS OTA
```

## Moniteur série

```bash
pio device monitor -b 115200                                  # Monitor standard
pio device monitor -b 115200 --filter esp32_exception_decoder # Décode les backtraces auto
```

Raccourcis utiles dans le monitor :
- `Ctrl+C` : quitter
- `Ctrl+T` puis `Ctrl+H` : aide

## OTA

- Mot de passe OTA stocké dans la config (pas en dur)
- Première install **obligatoirement en USB** (bootloader + partitions)
- Après, OTA via `./deploy.sh ota-*`

## Tests

```bash
pio test                       # Tous les tests (si dossier test/ existe)
pio test -e native             # Tests natifs (PC, rapide)
pio test -e esp32dev           # Tests sur cible (ESP32 connecté)
pio test -f test_xxx           # Filtrer un test
```

Organisation recommandée :
```
test/
  test_native/          # Logique pure portable
  test_embedded/        # Tests nécessitant HAL Arduino
```

## Debug

```bash
# Décoder une backtrace manuellement (adresses collées depuis un ancien log)
pio pkg exec -- xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/esp32dev/firmware.elf <addr>
```

## Pièges connus

1. **Taille FS** : toujours `./build_fs.sh`, jamais `pio run -t buildfs` pour ce projet
2. **Port série pris** : fermer le moniteur avant `-t upload`
3. **Bibliothèques Git** : AsyncTCP et ESPAsyncWebServer sont épinglés sur des forks précis ; ne pas mettre à jour à la légère
4. **Node modules** : `node_modules/` sert uniquement à `minify.js` (et éventuel tooling frontend)
5. **Warnings gcc** : suivre l'évolution, un nouveau warning signale souvent un bug latent
