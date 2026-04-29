# ADR-0009 — Partition coredump dédiée + redimensionnement history (128 KB → 64 KB)

- **Statut** : Accepté
- **Date** : 2026-04-27
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : aucune (décision infrastructure)

## Contexte

Les crashes firmware de type `PANIC` (exception Xtensa : `StoreProhibited`, `LoadProhibited`, `IllegalInstruction`, etc.) produisent un backtrace dans le moniteur série, mais celui-ci est inutilisable en production :

- L'USB n'est pas connecté au boîtier installé en local technique.
- Le web server ne démarre pas encore quand le crash se produit en boot → les logs ne sont pas accessibles via `/download-logs`.
- La raison du reboot (`PANIC`) est visible dans le champ WS `reset_reason`, mais sans adresse PC ni nom de tâche, le diagnostic est impossible à distance.

ESP-IDF intègre un mécanisme natif de coredump : au moment du crash, le firmware sauvegarde automatiquement l'état de la RAM (registres, stack de chaque tâche) dans une partition flash dédiée (`coredump`). Le dump persiste jusqu'à effacement explicite. Il peut ensuite être téléchargé et décodé hors ligne avec `esp_coredump`.

La contrainte est la taille totale disponible : la flash est de 4 MB et la table de partitions existante (ADR-0007) ne prévoit pas de partition `coredump`. La seule partition réductible sans impact fonctionnel majeur est `history` (128 KB) — les données brutes et horaires sont en RAM, le fichier `history.bin` peut être réduit si l'on diminue la rétention horaire.

## Décision

La table de partitions est modifiée dans [`partitions.csv`](../../partitions.csv) :

| Name     | Type | SubType  | Offset    | Size      | Taille |
|----------|------|----------|-----------|-----------|--------|
| nvs      | data | nvs      | 0x9000    | 0x5000    | 20 KB  |
| otadata  | data | ota      | 0xE000    | 0x2000    | 8 KB   |
| app0     | app  | ota_0    | 0x10000   | 0x160000  | 1408 KB |
| app1     | app  | ota_1    | 0x170000  | 0x160000  | 1408 KB |
| spiffs   | data | spiffs   | 0x2D0000  | 0x110000  | 1088 KB |
| history  | data | spiffs   | 0x3E0000  | 0x10000   | **64 KB** |
| coredump | data | coredump | 0x3F0000  | 0x10000   | **64 KB** |

Points clés :

- La partition `history` est réduite de 128 KB à **64 KB** (offset inchangé, taille divisée par deux).
- Une nouvelle partition `coredump` de **64 KB** est ajoutée à `0x3F0000`, en fin de flash.
- La rétention des données horaires (`kMaxHourlyDataPoints`) est réduite de 360 à **168 points** (15 jours → 7 jours) pour tenir dans le nouveau budget ([`constants.h`](../../src/constants.h)).
- Le logger réduit la taille max de son fichier de 32 KB à **16 KB** (rotation au-delà, garde 12 KB) pour que le budget total de la partition `history` reste cohérent : `history.bin ~24 KB + system.log ~16 KB + tmp rotation ~12 KB = 52 KB < 56 KB utiles`.
- `HistoryManager::begin()` détecte un redimensionnement de partition via NVS (`hist_meta/part_sz`) et efface le filesystem avant montage si la taille a changé — évite un crash `IntegerDivideByZero` dans `lfs_alloc` au premier boot après migration.
- Un script de décodage [`tools/decode_coredump.sh`](../../tools/decode_coredump.sh) est fourni pour décoder le dump hors ligne avec `xtensa-esp32-elf-gdb`. Le script accepte deux arguments : `./tools/decode_coredump.sh [coredump.bin] [firmware.elf]`. Le deuxième argument est optionnel (défaut : `.pio/build/esp32dev/firmware.elf`) et permet de cibler un ELF archivé. En cas de SHA mismatch, le script détecte automatiquement la divergence, extrait le SHA du coredump et liste les ELF disponibles dans `builds/` avec la commande exacte à relancer.
- `deploy.sh` archive automatiquement l'ELF à chaque build firmware réussi dans `builds/firmware_YYYYMMDD_HHMMSS.elf` (rétention : 10 archives, dossier ignoré par git). Cette archive permet de retrouver l'ELF exact en cas de mismatch sans nécessiter un re-flash pour reproduire le crash.
- Trois endpoints HTTP sont exposés : `GET /coredump/info`, `GET /coredump/download`, `DELETE /coredump` (tous WRITE).

## Alternatives considérées

- **Réduire `spiffs` (UI web)** (rejeté) — la partition `spiffs` est déjà ajustée au plus juste (1088 KB pour Chart.js + JS minifié + HTML + CSS). Une réduction forcerait à retirer des assets ou à diminuer le cache navigateur.
- **Réduire `app0` / `app1`** (rejeté) — le binaire actuel approche 1 MB. Réduire les slots OTA à 1280 KB laisserait une marge insuffisante pour les évolutions futures.
- **Stocker le coredump sur `spiffs`** (rejeté) — `spiffs` est écrasé à chaque OTA filesystem, ce qui perdrait le dump. La partition dédiée survit aux OTA.
- **Ne rien faire** (rejeté) — les crashes en production sont actuellement indéboggables à distance.
- **Utiliser `esp_core_dump_to_uart()`** (rejeté) — envoie le dump sur la liaison série uniquement ; inaccessible sans USB connecté.

## Conséquences

### Positives

- Les crashes `PANIC` en production sont désormais déboggables : le dump persiste jusqu'à téléchargement explicite via l'UI ou l'API.
- Aucun changement d'offset pour `spiffs` ni pour `app0`/`app1` → les OTA existants continuent de fonctionner.
- La partition `coredump` est gérée nativement par ESP-IDF (pas de code de capture à maintenir).
- Un SHA mismatch (reflash entre le crash et l'analyse) est **récupérable** grâce aux archives ELF horodatées dans `builds/` et à la détection automatique dans `decode_coredump.sh`.

### Négatives / dette assumée

- La **première mise à jour** après ce changement de table de partitions **ne peut pas se faire via OTA** : la partition table est en dehors du périmètre OTA standard. Un flash USB initial est obligatoire (voir [`docs/BUILD.md`](../BUILD.md)).
- L'historique horaire est réduit de 15 à **7 jours** — les utilisateurs qui consultent des tendances au-delà de 7 jours verront des données journalières (moins précises) pour les périodes antérieures.
- Le `coredump.bin` peut atteindre 64 KB ; le téléchargement est streamé sans allocation côté firmware, mais le navigateur doit traiter un fichier binaire opaque (décodage hors ligne requis).

### Ce que ça verrouille

- **Flash USB obligatoire** pour la migration depuis une version utilisant ADR-0007. Une note de migration doit figurer dans `docs/UPDATE_GUIDE.md` pour toute release incluant cette table.
- La partition `coredump` est à `0x3F0000` — cet offset est désormais réservé et ne peut pas être réutilisé pour une autre partition sans un nouvel ADR.
- Le mécanisme de protection au redimensionnement (`hist_meta/part_sz` en NVS) est couplé à `HistoryManager::begin()` : tout changement futur de la taille de `history` doit s'assurer que la clé NVS est mise à jour en conséquence.

## Références

- Code : [`partitions.csv`](../../partitions.csv)
- Code : [`src/history.cpp`](../../src/history.cpp) — `begin()`, protection redimensionnement NVS
- Code : [`src/logger.cpp`](../../src/logger.cpp) — `kFlushIntervalMs`, `kMaxLogFileBytes`, `kRotateKeepBytes`
- Code : [`src/constants.h`](../../src/constants.h) — `kMaxHourlyDataPoints`
- Code : [`src/web_routes_coredump.h`](../../src/web_routes_coredump.h), [`src/web_routes_coredump.cpp`](../../src/web_routes_coredump.cpp)
- Script : [`tools/decode_coredump.sh`](../../tools/decode_coredump.sh)
- Doc : [`docs/API.md`](../API.md) — endpoints `/coredump/*`
- Doc : [`docs/subsystems/history.md`](../subsystems/history.md)
- Doc : [`docs/subsystems/logger.md`](../subsystems/logger.md)
- ADR supersedé : [ADR-0007](0007-table-partitions-custom.md)
