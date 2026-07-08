# ADR-0024 — Partitions app à 1792 KB (layout v4)

- **Statut** : Accepté
- **Date** : 2026-07-06
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : feature-049 (déclencheur), feature-048 (prérequis — gzip des assets), feature-044 (origine du layout v3)
- **Supersède** : [ADR-0019](0019-partition-app-1664k.md) (layout v3, app 1664 KB / spiffs 576 KB)

## Contexte

Le layout v3 (ADR-0019) avait dimensionné la partition `spiffs` à 576 KB pour un payload FS de ~449 KB (post-uPlot). La pré-compression gzip des assets (feature-048) a fait tomber ce payload de 443 à **155 KB** : la partition `spiffs` n'était plus occupée qu'à **27 %** — 400 KB de flash dormaient dans une partition surdimensionnée, alors que la marge firmware (264 KB par slot) reste la ressource qui se consomme à chaque feature.

Comme pour la migration v2 → v3, on repartitionne **pendant que c'est confortable** plutôt que d'attendre le prochain dépassement : chaque changement de table exige un flash USB (accès physique au boîtier), autant le grouper avec une release.

## Décision

**Passer au layout v4 : `app0`/`app1` portées de 1664 à 1792 KB (+128 KB chacune), pris sur `spiffs` qui passe de 576 à 320 KB (−256 KB).** `nvs`, `otadata`, `history` et `coredump` sont strictement inchangés (offsets et tailles) : config, historique et coredump sont préservés.

### Table layout v4 ([`partitions.csv`](../../partitions.csv))

| Partition | Offset | Taille | Δ vs v3 |
|---|---|---|---|
| nvs | 0x9000 | 0x5000 (20 KB) | inchangé — **config préservée** |
| otadata | 0xE000 | 0x2000 (8 KB) | inchangé |
| **app0** | 0x10000 | **0x1C0000 (1792 KB)** | **+128 KB** |
| **app1** | **0x1D0000** | **0x1C0000 (1792 KB)** | **+128 KB**, offset décalé |
| **spiffs** | **0x390000** | **0x50000 (320 KB)** | **−256 KB**, offset décalé |
| history | 0x3E0000 | 0x10000 (64 KB) | inchangé — **historique préservé** |
| coredump | 0x3F0000 | 0x10000 (64 KB) | inchangé |

Vérifications d'arithmétique (refaites indépendamment en revue) : `app1` à 0x1D0000 aligné 64 KB ✓ ; fin de `app1` = 0x390000 = début de `spiffs` ✓ ; fin de `spiffs` = 0x3E0000 = début de `history` ✓ ; total = 0x400000 (4 MB) ✓.

### Mesures après migration (v2.13.0)

- **Firmware** : occupation **≈78,5 %** du slot 1792 KB → marge par slot portée de **264 à ~392 KB**.
- **FS** : payload gzippé 155 KB → **≈48 %** de la partition 320 KB → marge ~165 KB.

## Alternatives considérées

- **Option A (rejetée) — Statu quo (layout v3)**.
  La marge firmware de 264 KB est suffisante aujourd'hui. Mais elle se consomme à chaque feature, et le flash USB de la migration est de toute façon groupable avec la release courante — attendre ne fait que reporter un flash USB inévitable, potentiellement au pire moment (partition pleine).
- **Option B (rejetée) — Palier conservateur +64 KB** (app 1728 KB / spiffs 448 KB).
  Demi-mesure sans bénéfice de simplicité : le coût de migration (flash USB) est identique, et garder 448 KB de FS pour un payload de 155 KB reste surdimensionné. Autant prendre le palier le plus large en une seule migration.
- **Option C (rejetée) — Reprendre aussi la partition `history`**.
  Aurait libéré 64 KB de plus, mais `history` contient des **données à préserver** (déplacer son offset les perdrait ou exigerait une migration de données) et ses 64 KB suffisent au besoin actuel (brut + horaire + quotidien).
- **Option D (retenue) — +128 KB** (app 1792 KB / spiffs 320 KB).
  Marge firmware ~392 KB, FS à 48 % avec ~165 KB de marge pour les évolutions UI (le pipeline gzip amortit les ajouts futurs), `history`/`coredump` intouchées.

## Conséquences

### Positives

- **~392 KB de marge firmware par slot** (occupation ≈78,5 %) — était 264 KB en v3.
- **Aucune donnée perdue** : `nvs` (config, calibrations, WiFi), `history` (historique des mesures) et `coredump` conservent offsets et tailles ; la détection de redimensionnement de `HistoryManager` ne se déclenche pas.
- Un seul flash USB pour la migration ; l'OTA firmware + FS refonctionne normalement ensuite (nouvelles bornes prises en compte automatiquement).
- `deploy.sh` **reconstruit automatiquement** une image FS dont la taille ne correspond pas à 327 680 octets — pas de flash d'une image v3 par inadvertance via le script.

### Négatives / dette assumée

- **Flash USB obligatoire une fois** : l'OTA ne réécrit ni la table de partitions ni le bootloader. Accès physique au connecteur USB requis pour la migration v3 → v4. Voir [UPDATE_GUIDE.md](../UPDATE_GUIDE.md#migration-layout-v3--v4-v2130--depuis-2026-07-06).
- ⚠️ **Une image FS v4 (320 KB) ne doit JAMAIS être poussée en OTA sur un appareil encore en table v3** : la table active resterait v3, et un FS construit pour 320 KB monté sur une partition de 576 KB présente des métadonnées LittleFS discordantes. La release qui embarque le v4 se flashe par USB (`./deploy.sh all`), point.
- **Frontend contraint à 320 KB** : toute évolution UI doit rester compatible avec le pipeline gzip ; une grosse dépendance JS future devra repasser par une analyse type feature-043/048.
- **Granularité 64 KB des slots** (alignement 0x10000 obligatoire pour les partitions `app`) : les futurs ajustements de table se font par paliers de 64 KB minimum.

### Ce que ça verrouille

- L'app peut grossir jusqu'à ~1792 KB ; le FS étant déjà réduit au plancher raisonnable, il n'y a **plus de palier disponible** sur une flash 4 MB — au-delà, le passage à un module **8/16 MB** (PCB v3) devient la seule issue (déjà identifié dans les ADR-0015 et 0019).
- Le layout v4 devient la référence pour le quatuor `partitions.csv`, `platformio.ini` (`filesystem_size = 327680`), `build_fs.sh` (`-s 327680`) et `deploy.sh` (`LITTLEFS_OFFSET=0x390000`, contrôle de taille 327 680) — les 4 références doivent rester alignées.
- [ADR-0019](0019-partition-app-1664k.md) passe en `Superseded by ADR-0024`.

## Migration

1. Compiler firmware + FS (`pio run` + `./build_fs.sh`).
2. **Flash USB obligatoire** : `./deploy.sh all` (réécrit bootloader + table + firmware, puis FS à 0x390000). **Ne pas utiliser `./deploy.sh factory`** (efface la NVS → perte config + régénération du mot de passe AP).
3. NVS, historique et coredump préservés ; OTA de nouveau disponible dès le premier boot.

Procédure détaillée : [UPDATE_GUIDE.md — Migration layout v3 → v4](../UPDATE_GUIDE.md#migration-layout-v3--v4-v2130--depuis-2026-07-06).

## Références

- Code : [`partitions.csv`](../../partitions.csv), [`platformio.ini`](../../platformio.ini), [`build_fs.sh`](../../build_fs.sh), [`deploy.sh`](../../deploy.sh)
- Spec : `specs/features/doing/feature-049-partitions-layout-v4.md`
- ADR antérieurs : [ADR-0019](0019-partition-app-1664k.md) (layout v3, supersédé), [ADR-0009](0009-partition-coredump.md) (partition coredump), feature-048 (gzip — prérequis, voir [BUILD.md](../BUILD.md#pré-compression-gzip-des-assets-feature-048-v2121))
- Doc : [BUILD.md](../BUILD.md), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
