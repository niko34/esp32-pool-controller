# Guide de compilation et déploiement

Guide **opérationnel** : quelles commandes lancer pour compiler et déployer. Pour la structure des partitions et le pourquoi du layout courant (v3), voir [ADR-0019](adr/0019-partition-app-1664k.md) et la source de vérité [`partitions.csv`](../partitions.csv).

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
2. Construit `littlefs.bin` avec la **taille exacte de la partition `spiffs`** (589 824 octets = 576 KB, layout v3 — voir [ADR-0019](adr/0019-partition-app-1664k.md)).

⚠️ **Ne pas utiliser `pio run -t buildfs`** : PlatformIO recalcule une taille incorrecte (128 KB), ce qui produit un `littlefs.bin` non conforme à la partition `spiffs`.

### Tout en une commande

```bash
./build_all.sh
```

Enchaîne `pio run` puis `build_fs.sh`.

## Tests natifs (hors matériel)

L'environnement `native` (`platform = native` dans [`platformio.ini`](../platformio.ini)) compile et exécute la **logique pure** du firmware sur le PC, sans ESP32. Il ne se lance qu'explicitement :

```bash
pio test -e native
```

PlatformIO compile **un binaire par dossier `test/test_*`** ; `pio test -e native` couvre donc désormais **deux suites** :

| Dossier | Couvre | Source testée |
|---------|--------|---------------|
| `test/test_native_sensor_filter/` | filtrage médiane + EMA, warmup, rejets (feature-025) | `src/sensor_filter.cpp` |
| `test/test_native_dosing/` | décision de dosage (`evaluateDose`, hystérésis start/stop, non-régression pause-mélange) (feature-036) | `src/dosing_logic.cpp` |

Le `build_src_filter` de l'env `native` inclut les deux modules purs :

```ini
build_src_filter = +<sensor_filter.cpp> +<dosing_logic.cpp>
```

Ces deux sources ne dépendent **ni d'Arduino, ni de FreeRTOS, ni d'I²C** ; seul un shim minimal (`test/native_shim/`) fournit `NAN` / `isnan` / types entiers. Voir [ADR-0017](adr/0017-logique-metier-pure-humble-object-testabilite.md) pour la convention « logique pure séparée de la couche hardware ».

### Couverture de tests

Le script [`tools/coverage.sh`](../tools/coverage.sh) mesure la couverture des classes pures (`sensor_filter`, `dosing_logic`) via l'env dédié `native_coverage` (env `native` + instrumentation `--coverage`) et **gcovr** :

```bash
./tools/coverage.sh          # résumé console (lignes + branches) + rapport HTML
./tools/coverage.sh --open   # idem + ouvre coverage/index.html (macOS)
```

Prérequis : `gcovr` dans le venv PlatformIO (`~/.platformio/penv/bin/pip install gcovr`) et `/usr/bin/gcov` (Command Line Tools sur macOS — Apple LLVM, compatible). Le rapport HTML est généré dans `coverage/` (ignoré par git).

> **Portée** : seules les **classes pures** sont mesurées. La coquille `pump_controller.cpp` (collecte des globals + mapping énum→chaîne) est **exclue du build natif** — son équivalence est validée par revue, pas par couverture. Voir [ADR-0017](adr/0017-logique-metier-pure-humble-object-testabilite.md).

### Validation on-target (manuelle)

Les tests natifs ne couvrent que la **logique pure**. L'**intégration matériel + réseau** (I²C, PWM, RTC, LittleFS, WiFi, WebSocket, MQTT) et les **scénarios complets de régulation** se valident à la main sur la carte, via la check-list [`docs/test-on-target.md`](test-on-target.md) — à dérouler après un `./deploy.sh` touchant la régulation, la filtration, les capteurs ou la persistance.

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
- **Partition `history` préservée** : l'upload filesystem OTA n'écrase que la partition `spiffs`. L'historique des mesures n'est **pas** perdu à la mise à jour UI (voir [ADR-0019](adr/0019-partition-app-1664k.md) et [history.md](subsystems/history.md)).
- **Changement de table de partitions** : un nouveau layout (ex. v2 → v3 en v2.4.0) exige un **flash USB une fois** — l'OTA ne réécrit pas la table. Voir [UPDATE_GUIDE.md](UPDATE_GUIDE.md#migration-layout-v2--v3--v240).

## Dépendances PlatformIO

Liste des libs déclarées dans `platformio.ini` (`lib_deps`) — voir le fichier source pour les versions épinglées.

| Lib | Usage |
|-----|-------|
| `AsyncTCP` | TCP asynchrone (base de ESPAsyncWebServer) |
| `ESPAsyncWebServer` | Serveur HTTP / WebSocket non bloquant |
| `ESPAsyncWiFiManager` | Wizard Wi-Fi premier boot |
| `ArduinoJson` (v7) | Sérialisation JSON |
| `PubSubClient` | Client MQTT |
| `OneWire` + `DallasTemperature` | Bus 1-Wire pour DS18B20 (eau + circuit) |
| `RTClib` | DS3231 RTC |

> **Libs supprimées en feature-021 (v2.0.0)** : `Adafruit ADS1X15` et `DFRobot_PH` ont été retirées des `lib_deps`. La chaîne pH/ORP est désormais entièrement portée par la mini-classe maison [`AtlasEzoSensor`](../src/atlas_ezo.h) qui pilote les modules Atlas EZO Embedded en I²C natif (`Wire`). Voir [ADR-0014](adr/0014-migration-atlas-ezo.md). Cette suppression libère ~12 KB de flash mais l'ajout de la queue + des routes de calibration consomme environ autant — le build tenait alors à 98.8 % flash (layout v1). Depuis le layout v3 ([ADR-0019](adr/0019-partition-app-1664k.md)), l'occupation firmware est redescendue à ~83,8 % (marge ~270 KB).

## Bibliothèque de graphiques uPlot (frontend)

Les 6 graphiques de l'UI (3 mini-charts du dashboard, 3 historiques détail pH / ORP / Température) sont rendus par **uPlot** depuis feature-043, qui remplace Chart.js v4.5.1 (`chart.umd.min.js`, ~208 KB, **supprimé**). Voir [ADR-0018](adr/0018-migration-uplot.md). Un 7ᵉ graphique (debug oscillation pH du panel Avancé) a été retiré en v2.5.0 (feature-045).

### Provenance et version

- Paquet npm **`uplot@1.6.32`** — version **FIGÉE**, récupérée une fois puis committée. Aucune dépendance npm au build ni au runtime (offline-first, pas de CDN).
- Fichiers committés dans `data/` : [`uPlot.iife.min.js`](../data/uPlot.iife.min.js) (~50 KB) et [`uPlot.min.css`](../data/uPlot.min.css) (~2 KB) — copies de `dist/uPlot.iife.min.js` et `dist/uPlot.min.css` du paquet.
- Chargés par [`data/index.html`](../data/index.html) via `<script>` + `<link>` — 100 % vanilla, **aucune étape de build ajoutée** au déploiement (cohérent [ADR-0006](adr/0006-frontend-vanilla-js.md)).

### Mise à jour manuelle (si nécessaire)

```bash
npm pack uplot@<version>     # télécharge uplot-<version>.tgz
tar -xzf uplot-<version>.tgz
cp package/dist/uPlot.iife.min.js package/dist/uPlot.min.css data/
```

Puis `./build_fs.sh` et **vérification de parité en navigateur** (aucun test automatisé frontend — dashboard, détail pH/ORP/Température, console sans erreur).

### Gain mesuré (feature-043)

Suppression de `chart.umd.min.js` : payload FS 601 054 → 449 177 octets (**−148,3 KB**), soit ≈ 449 KB — gain qui a rendu possible la réduction de la partition `spiffs` à 576 KB au profit des slots app (layout v3, [ADR-0019](adr/0019-partition-app-1664k.md)).

> **Options non reprises** : l'ancienne usine Chart.js `createLineChart` exposait des options **mortes** (`hideYAxis`, `showYAxisGrid`, `extraPlugins`, `annotation`, `backgroundColor`), inutilisées aux call-sites. L'usine uPlot ([`data/app.js`](../data/app.js)) ne les reprend **pas** — réduction de périmètre assumée (feature-043).

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
