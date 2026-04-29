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

## Archivage automatique du firmware ELF

À chaque compilation réussie du firmware (commandes `firmware`, `all`, `factory`, `ota-firmware`, `ota-all`), `deploy.sh` archive le fichier `.pio/build/esp32dev/firmware.elf` dans le dossier `builds/` :

```
builds/firmware_20260427_143022.elf
builds/firmware_20260427_082810.elf
...
```

- **Rétention** : 10 archives au maximum — les plus anciennes sont supprimées automatiquement.
- **Dossier `builds/`** : exclu de git (`.gitignore`), local uniquement.
- **Pourquoi** : le décodage d'un coredump exige l'ELF **exact** qui tournait au moment du crash. Sans archive, si un reflash a eu lieu entre le crash et l'analyse, `esp_coredump` échoue avec une erreur SHA mismatch.

## Décodage d'un coredump

Workflow complet pour diagnostiquer un crash `PANIC` en production.

### 1. Récupérer le token d'authentification

Dans la console du navigateur (page ouverte sur l'interface du contrôleur) :

```javascript
sessionStorage.getItem('authToken')
```

Ou via l'API :

```bash
curl -s -X POST http://<ip>/auth/login \
  -H "Content-Type: application/json" \
  -d '{"password":"<mot-de-passe>"}' | jq -r '.token'
```

### 2. Télécharger le coredump

```bash
curl -H "X-Auth-Token: <token>" \
  http://<ip>/coredump/download -o coredump.bin
```

### 3. Décoder

```bash
./tools/decode_coredump.sh coredump.bin
```

Un deuxième argument optionnel permet de spécifier l'ELF à utiliser (utile si l'ELF courant ne correspond pas au firmware crashé) :

```bash
./tools/decode_coredump.sh coredump.bin builds/firmware_20260427_082810.elf
```

### 4. Récupérer d'un SHA mismatch

Si le coredump a été capturé avec un firmware différent de l'ELF courant, le script détecte le mismatch et affiche automatiquement les archives disponibles avec la commande exacte à relancer :

```
─────────────────────────────────────────────────────
Le coredump a été capturé avec un firmware différent.
SHA du coredump : 07b12255d2bba9c6

ELF archivés disponibles (du plus récent au plus ancien) :
  builds/firmware_20260427_143022.elf
  builds/firmware_20260427_082810.elf

Relancer avec l'ELF correspondant :
  ./tools/decode_coredump.sh coredump.bin builds/firmware_20260427_082810.elf
─────────────────────────────────────────────────────
```

Identifier l'archive dont la date/heure est antérieure au crash (visible dans les logs ou dans `reset_reason` WS), relancer avec cet ELF.

Voir aussi [ADR-0009](adr/0009-partition-coredump.md) pour la décision architecturale sur la partition `coredump`.

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
- [`deploy.sh`](../deploy.sh) — wrapper unique pour upload USB ou OTA ; archive l'ELF à chaque build firmware.
- [`ota_update.sh`](../ota_update.sh) — utilitaire OTA bas niveau (appelé par `deploy.sh`).
- [`tools/decode_coredump.sh`](../tools/decode_coredump.sh) — décodage d'un coredump avec `xtensa-esp32-elf-gdb`.
- `data/` — sources UI (HTML / CSS / JS / images).
- `data-build/` — UI minifiée (généré automatiquement, ignoré par git).
- `.pio/build/esp32dev/` — artéfacts de build (`firmware.bin`, `firmware.elf`, `littlefs.bin`).
- `builds/` — archives ELF horodatées (généré automatiquement, ignoré par git).
