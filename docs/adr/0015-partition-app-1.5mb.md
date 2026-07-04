# ADR-0015 — Partitions app à 1.5 MB (layout v2)

> **Statut : Superseded by [ADR-0019](0019-partition-app-1664k.md)** (2026-07-04)
> Le layout v2 (app 1536 KB / spiffs 832 KB) est remplacé par le **layout v3**
> (app 1664 KB / spiffs 576 KB), rendu possible par la migration uPlot
> ([ADR-0018](0018-migration-uplot.md), payload FS 449 KB) et déclenché par
> l'occupation firmware à 90,8 %. `nvs`, `history` et `coredump` restent inchangés.
> Voir feature-044 et [ADR-0019](0019-partition-app-1664k.md).
> Contenu ci-dessous conservé à titre historique.

- **Statut** : Superseded by [ADR-0019](0019-partition-app-1664k.md)
- **Date** : 2026-05-08
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : feature-024 (déclencheur), feature-021 (origine indirecte)

## Contexte

L'implémentation de feature-024 (pente sonde pH Atlas EZO) a fait franchir au firmware la limite de la partition `app0/app1` (1408 KB = 0x160000) :

- Firmware avec feature-024 : 1 446 KB de binaire `.bin` (sections code + data + headers ESP32)
- Partition disponible : 1 442 KB
- Dépassement : ~4 KB → image tronquée à l'écriture flash → bootloader détecte CRC invalide → **boot loop SW_RESET**

Le rapport `pio run` annonçait Flash 99.8% (1 439 137 / 1 441 792) car il ne mesure que les sections du `.elf`. Le `.bin` final ajoute le header d'image ESP32, padding 4K et checksums, ce qui pousse au-delà de la partition.

**Cause racine** : la table de partitions actuelle (ADR-0007) date d'avant l'arrivée de :
- feature-019 (PCB v2 + isolation galvanique)
- feature-020 (multi-sondes DS18B20)
- feature-021 (Atlas EZO + 17 entités HA discovery)
- feature-024 (pente sonde pH + 3 sensors HA supplémentaires)

Cumul de ces features = ~80 KB de code en plus. La marge initiale (~50 KB) est consommée.

**Contrainte produit** : impossible de retirer feature-024 ni de revenir à ADS1115 (le PCB v2 n'a plus le chip). Il faut soit redimensionner les partitions, soit supprimer du code.

## Décision

**Repartitionner la flash 4 MB en faisant passer `app0/app1` de 1408 KB à 1536 KB (1.5 MB).** L'espace est pris sur la partition `spiffs` (LittleFS UI) qui passe de 1088 KB à 832 KB.

### Nouveau layout (v2)

| Partition | Adresse | Taille v1 | Taille v2 | Δ |
|---|---|---|---|---|
| nvs | 0x9000 | 20 KB | 20 KB | — |
| otadata | 0xE000 | 8 KB | 8 KB | — |
| **app0** | 0x10000 | 1408 KB | **1536 KB** | **+128 KB** |
| **app1** | 0x190000 | 1408 KB | **1536 KB** | **+128 KB** |
| **spiffs** | 0x310000 | 1088 KB | **832 KB** | **−256 KB** |
| history | 0x3E0000 | 64 KB | 64 KB | — |
| coredump | 0x3F0000 | 64 KB | 64 KB | — |

**Marge nouveau firmware** : 1536 KB − 1446 KB = **+90 KB** confortable.
**Marge spiffs** : FS actuel ~660 KB / 832 KB = **21% libre** (vs 39% avant). Suffisant.

## Alternatives considérées

- **Option A (rejetée) — Optimiser le code pour rester sous 1408 KB**.
  Économies possibles : retirer `web_routes_debug.cpp` (-6-10 KB), refactoriser `MqttManager::publishDiscovery` (-1-2 KB), activer LTO (-50-100 KB potentiel). Total atteignable : ~50 KB.
  Rejet : (1) gains incertains, (2) freine le développement futur, (3) `web_routes_debug` est un outil utile pour diagnostiquer les sondes, (4) LTO peut introduire des bugs subtils.

- **Option B (rejetée) — App à 1.75 MB / spiffs 384 KB**.
  Donnerait +384 KB de marge mais le FS actuel (~660 KB) ne tiendrait plus dans 384 KB.
  Rejet : impossible sans refonte UI agressive (suppression Chart.js, minification additionnelle), trop coûteux pour un gain disproportionné.

- **Option C (rejetée) — Une seule partition app à 3 MB, suppression de l'OTA**.
  Donnerait +1.6 MB de marge.
  Rejet : OTA est critique pour une piscine en exploitation (mise à jour USB nécessite démontage du boîtier). Non négociable produit.

- **Option D (retenue) — App à 1.5 MB / spiffs 832 KB**.
  Marge confortable +90 KB pour les futures features, FS actuel passe sans changement, OTA conservée.

- **Option E (rejetée) — Firmware compressé en flash via app_image (ESP-IDF avancé)**.
  Pas standard avec Arduino-ESP32, refactor lourd. À considérer dans une éventuelle migration future vers ESP-IDF natif.

## Conséquences

### Positives

- **+90 KB de marge** dans la partition app après feature-024
- Repartitionnement effectué une fois, valide pour les ~5-10 prochaines features moyennes
- Pas de perte de fonctionnalité : OTA conservée, history et coredump conservés, tous les capteurs et UI fonctionnels

### Négatives / dette assumée

- **Flash USB obligatoire pour la migration** (le partition table change → l'OTA Arduino classique ne reflashe pas la partition table). Un accès physique au connecteur USB du boîtier est requis une fois.
- **NVS préservé** mais **OTA history perdu** au moment du flash (otadata reset). Le firmware redémarre proprement sur app0 vierge.
- **Spiffs passe de 1088 → 832 KB**. Marge réduite à 21%. Une refonte UI massive future (ex. ajout Three.js pour visualisation 3D) ne tiendrait pas — il faudrait minifier davantage ou changer de partitionnement.
- **Documentation à mettre à jour** : `docs/adr/0007-table-partitions-custom.md` annoté « Superseded by ADR-0015 ».

### Ce que ça verrouille

- L'app peut grossir jusqu'à ~1500 KB (laisse 36 KB de header/padding) — au-delà, repartitionnement obligatoire (option B ou retrait fonctionnalités)
- Spiffs limité à 832 KB → toute augmentation du frontend doit rester sous cette limite (FS actuel = 660 KB, marge 172 KB = ~26%)
- Le passage à un module ESP32 avec 8 MB ou 16 MB de flash deviendrait pertinent pour libérer toutes les contraintes — à considérer pour PCB v3 si le besoin se confirme

## Migration

1. **Compiler** firmware + FS avec la nouvelle config (`pio run` + `./build_fs.sh`)
2. **Flash USB obligatoire** (bootloader + partition table + firmware + FS) :
   ```bash
   ./deploy.sh all
   ```
   Le script `deploy.sh` flashe partition table + bootloader + app + FS à leurs nouvelles adresses.
3. **NVS préservé** : la partition `nvs` (0x9000–0xE000) ne bouge pas → calibrations, config WiFi, identification sondes, NVS dosage = tout conservé.
4. **OTA disponible** dès le 1er flash : prochaines mises à jour via WiFi.

## Références

- Code : `partitions.csv`, `platformio.ini`, `deploy.sh`, `build_fs.sh`
- Doc : `docs/BUILD.md` (à mettre à jour avec nouvelles adresses)
- Spec : `specs/features/done/feature-024-pente-sonde-ph.md` (déclencheur)
- ADR antérieur : ADR-0007 (table de partitions custom v1) — annoté « Superseded by ADR-0015 »
