# Guide de compilation et déploiement

Guide **opérationnel** : quelles commandes lancer pour compiler et déployer. Pour la structure des partitions et le pourquoi du layout, voir [ADR-0007](adr/0007-table-partitions-custom.md) et la source de vérité [`partitions.csv`](../partitions.csv).

## Compilation

### Firmware seul

```bash
pio run
```

Génère `.pio/build/esp32dev/firmware.bin`.

### Filesystem (LittleFS) seul

```bash
./build_fs.sh
```

Ce script :
1. Minifie HTML / CSS / JS (via `minify.js`, sortie dans `data-build/`, ignoré par git) ;
2. Construit `littlefs.bin` avec la **taille exacte de la partition `spiffs`** (1 114 112 octets = 1088 KB).

⚠️ **Ne pas utiliser `pio run -t buildfs`** : PlatformIO recalcule une taille incorrecte (128 KB), ce qui produit un `littlefs.bin` non conforme à la partition `spiffs`.

### Tout en une commande

```bash
./build_all.sh
```

Enchaîne `pio run` puis `build_fs.sh`.

## Upload / Déploiement

Le script [`deploy.sh`](../deploy.sh) est l'entrée unique pour tous les modes de déploiement. Lancer sans argument pour l'aide :

```bash
./deploy.sh
```

### Modes USB (câble série)

| Commande | Action |
|----------|--------|
| `./deploy.sh firmware` | Compile + upload firmware uniquement |
| `./deploy.sh fs` | Build + upload filesystem uniquement |
| `./deploy.sh all` *(défaut)* | Firmware + filesystem |
| `./deploy.sh factory` | Efface toute la flash, puis upload complet. **Génère un nouveau mot de passe WiFi AP** (NVS effacée) |

### Modes OTA (WiFi)

| Commande | Action |
|----------|--------|
| `./deploy.sh ota-firmware` | Firmware uniquement en OTA |
| `./deploy.sh ota-fs` | Filesystem uniquement en OTA |
| `./deploy.sh ota-all` | Firmware + filesystem en OTA |

Les modes OTA utilisent par défaut `poolcontroller.local` (mDNS). Pour cibler une IP directe :

```bash
./ota_update.sh both 192.168.1.42 monmotdepasse
```

Voir aussi [UPDATE_GUIDE.md](UPDATE_GUIDE.md) pour la procédure détaillée.

## Pièges connus

- **`pio run -t buildfs`** : ne génère pas un `littlefs.bin` de la bonne taille → utiliser `./build_fs.sh`.
- **`pio run -t uploadfs`** : reconstruit le filesystem à la mauvaise taille avant upload → utiliser `./deploy.sh fs` ou `./deploy.sh ota-fs`.
- **Partition `history` préservée** : l'upload filesystem OTA n'écrase que la partition `spiffs`. L'historique des mesures n'est **pas** perdu à la mise à jour UI (voir [ADR-0007](adr/0007-table-partitions-custom.md) et [history.md](subsystems/history.md)).

## Structure des fichiers de build

- [`partitions.csv`](../partitions.csv) — source de vérité pour les offsets et tailles.
- [`platformio.ini`](../platformio.ini) — plateforme, libs, flags de compilation.
- [`build_all.sh`](../build_all.sh) — firmware + filesystem en une commande.
- [`build_fs.sh`](../build_fs.sh) — filesystem uniquement (minification + taille correcte).
- [`minify.js`](../minify.js) — minification HTML / CSS / JS.
- [`deploy.sh`](../deploy.sh) — wrapper unique pour upload USB ou OTA.
- [`ota_update.sh`](../ota_update.sh) — utilitaire OTA bas niveau (appelé par `deploy.sh`).
- `data/` — sources UI (HTML / CSS / JS / images).
- `data-build/` — UI minifiée (généré automatiquement, ignoré par git).
- `.pio/build/esp32dev/` — artéfacts de build (`firmware.bin`, `littlefs.bin`).
