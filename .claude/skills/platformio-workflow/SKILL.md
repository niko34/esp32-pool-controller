---
name: platformio-workflow
description: Workflow et commandes PlatformIO pour ESP32 Pool Controller. À charger dès qu'il faut compiler, flasher, déployer le firmware ou le filesystem, ou lancer des tests embarqués/natifs. Couvre pio run, upload USB, OTA, build FS LittleFS 512KB, monitor série et décodage backtrace.
---

# PlatformIO — workflow ESP32 Pool Controller

## Configuration

- `platformio.ini` : env matériel unique `[env:esp32dev_core3]`, plateforme **pioarduino épinglée `55.03.37`** (core Arduino 3.3.7 / IDF 5.5.0), `framework = arduino`. L'ancien env de repli core 2 `[env:esp32dev]` (`espressif32@6.9.0`) a été **retiré en v2.42.1** — voir [ADR-0069](../../../docs/adr/0069-retrait-repli-core-2.md). ⚠️ La plateforme `espressif32@6.9.0` reste **installée** (elle fournit `tool-mklittlefs`, référencé en dur par `build_fs.sh`) : ne pas la désinstaller.
- ⚠️ **Ne jamais lancer `pio pkg install` / `pio pkg uninstall` sans nécessité** : la commande réécrit `platformio.ini` en supprimant tous ses commentaires.
- Partitions personnalisées : `partitions.csv` — **layout v5, flash 8 Mo** ([ADR-0068](../../../docs/adr/0068-partitions-layout-v5-flash-8-mo.md)). Ne jamais modifier sans flash USB (l'OTA n'écrit ni le bootloader ni la table).
- Filesystem : LittleFS, deux partitions :
  - `spiffs` : **512 KB** (524 288 octets = 0x80000, offset 0x730000) — contient `data/` (frontend web)
  - `history` : **256 KB** (0x40000, offset 0x7B0000) — contient `/history.json` et `/system.log`
- ⚠️ Taille différente de la valeur par défaut PlatformIO — **utiliser `./build_fs.sh`**, pas `pio run -t buildfs`

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
pio test -e native             # Tests natifs (PC, rapide) — 33 suites, 1291 cas
pio test -f test_xxx           # Filtrer une suite
```

⚠️ **Il n'existe pas d'environnement de test sur cible.** `pio test -e esp32dev` n'a jamais correspondu à un env déclaré, et `esp32dev` n'existe plus du tout depuis la v2.42.1. Toute la logique testable est extraite en modules purs ([ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) et jouée en natif :

```
test/
  test_native_<domaine>/   # Logique pure portable — 33 suites
```

## Debug

```bash
# Décoder une backtrace manuellement (adresses collées depuis un ancien log)
pio pkg exec -- xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/esp32dev_core3/firmware.elf <addr>
```

## Pièges connus

1. **Taille FS** : toujours `./build_fs.sh`, jamais `pio run -t buildfs` pour ce projet
2. **Port série pris** : fermer le moniteur avant `-t upload`
3. **Bibliothèques Git** : AsyncTCP et ESPAsyncWebServer sont épinglés sur des forks précis ; ne pas mettre à jour à la légère
4. **Node modules** : `node_modules/` sert uniquement à `minify.js` (et éventuel tooling frontend)
5. **Warnings gcc** : suivre l'évolution, un nouveau warning signale souvent un bug latent
