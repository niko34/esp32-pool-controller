# ADR-0019 — Partitions app à 1664 KB (layout v3)

> **Statut : Superseded by [ADR-0024](0024-partitions-layout-v4.md)** (2026-07-06)
> Le layout v3 (app 1664 KB / spiffs 576 KB) est remplacé par le **layout v4**
> (app 1792 KB / spiffs 320 KB), rendu possible par la pré-compression gzip des
> assets (feature-048, payload FS 155 KB) qui laissait la partition `spiffs`
> occupée à 27 %. `nvs`, `history` et `coredump` restent inchangés.
> Voir feature-049 et [ADR-0024](0024-partitions-layout-v4.md).
> Contenu ci-dessous conservé à titre historique.

- **Statut** : Superseded by [ADR-0024](0024-partitions-layout-v4.md)
- **Date** : 2026-07-04
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : feature-044 (déclencheur), feature-043 (prérequis — migration uPlot), feature-024 (origine du layout v2)
- **Supersède** : [ADR-0015](0015-partition-app-1.5mb.md) (layout v2, app 1536 KB / spiffs 832 KB)

## Contexte

Le layout v2 (ADR-0015) donnait 1536 KB par slot app. Avant les optimisations récentes, le firmware occupait **90,8 %** de cette partition — la même trajectoire qui avait provoqué le boot loop de feature-024 (image tronquée au flash) se reproduisait, avec un repartitionnement d'urgence en perspective.

Deux évolutions ont changé la donne :

- **`CORE_DEBUG_LEVEL=0`** (commit `485b78c`) : −33 KB de Flash firmware ;
- **Migration Chart.js → uPlot** (feature-043, [ADR-0018](0018-migration-uplot.md)) : payload FS réduit de 601 054 à **449 177 octets** (−148,3 KB), ce qui rend viable une partition `spiffs` de **576 KB** (l'obstacle qui avait fait rejeter l'option B « spiffs 384 KB » de l'ADR-0015 est levé pour un palier intermédiaire).

Plutôt que d'attendre le prochain dépassement (chaque changement de table exige un flash USB, donc un accès physique au boîtier), on repartitionne **maintenant**, pendant que la migration est confortable.

## Décision

**Passer au layout v3 : `app0`/`app1` portées de 1536 à 1664 KB (+128 KB chacune), pris sur `spiffs` qui passe de 832 à 576 KB (−256 KB).** `nvs`, `otadata`, `history` et `coredump` sont strictement inchangés (offsets et tailles) : config, historique et coredump sont préservés.

### Table layout v3 ([`partitions.csv`](../../partitions.csv))

| Partition | Offset | Taille | Δ vs v2 |
|---|---|---|---|
| nvs | 0x9000 | 0x5000 (20 KB) | inchangé — **config préservée** |
| otadata | 0xE000 | 0x2000 (8 KB) | inchangé |
| **app0** | 0x10000 | **0x1A0000 (1664 KB)** | **+128 KB** |
| **app1** | **0x1B0000** | **0x1A0000 (1664 KB)** | **+128 KB**, offset décalé |
| **spiffs** | **0x350000** | **0x90000 (576 KB)** | **−256 KB**, offset décalé |
| history | 0x3E0000 | 0x10000 (64 KB) | inchangé — **historique préservé** |
| coredump | 0x3F0000 | 0x10000 (64 KB) | inchangé |

Vérifications d'arithmétique : `app1` à 0x1B0000 aligné 64 KB ✓ ; fin de `spiffs` = 0x3E0000 = début de `history` ✓ ; total = 0x400000 (4 MB) ✓.

### Mesures après migration (v2.4.0)

- **Firmware** : 1 427 753 / 1 703 936 octets = **83,8 %** (était 90,8 % sur 1536 KB) → marge **~270 KB**.
- **FS** : payload 449 177 octets → **~82 %** de la partition 576 KB (métadonnées LittleFS incluses) → marge **~104 KB**.

## Alternatives considérées

- **Option A (rejetée) — Palier conservateur +64 KB** (app 1600 KB / spiffs 704 KB).
  Garde plus de marge FS, mais la marge firmware (~140 KB) ne repousse le prochain repartitionnement que de quelques features. Or chaque changement de table coûte un **flash USB physique** : autant prendre le palier le plus large possible en une seule migration.
- **Option B (rejetée) — +192 KB** (app 1728 KB / spiffs 448 KB).
  Impossible : le payload FS actuel (449 177 o) remplirait la partition à ~100 %, aucune marge pour la moindre évolution frontend.
- **Option C (retenue) — +128 KB** (app 1664 KB / spiffs 576 KB).
  Meilleur compromis : marge firmware ~270 KB (comparable au budget consommé par ~2 ans de features) et marge FS ~104 KB suffisante pour des évolutions UI raisonnables.

## Conséquences

### Positives

- **~270 KB de marge firmware** (occupation 83,8 %) — le développement peut reprendre sans surveiller chaque KB.
- **Aucune donnée perdue** : `nvs` (config, calibrations, WiFi), `history` (historique des mesures) et `coredump` conservent offsets et tailles.
- Un seul flash USB pour la migration ; l'OTA firmware + FS refonctionne normalement ensuite (nouvelles bornes prises en compte automatiquement — `history.cpp` trouve sa partition par label, l'OTA FS passe par `Update/U_SPIFFS`).

### Négatives / dette assumée

- **Flash USB obligatoire une fois** : l'OTA ne réécrit pas la table de partitions (ni le bootloader). Accès physique au connecteur USB requis pour la migration v2 → v3. Voir [UPDATE_GUIDE.md](../UPDATE_GUIDE.md).
- **Contenu `spiffs` réécrit** lors de la migration (le FS est re-flashé au nouvel offset 0x350000) — sans impact, il ne contient que l'UI.
- **Frontend contraint à 576 KB** : marge ~104 KB. Toute grosse dépendance JS future devra repasser par une analyse type feature-043.
- Résidus inertes de l'ancienne `app1` (0x190000, layout v2) en flash — réécrits au premier OTA firmware.

### Ce que ça verrouille

- L'app peut grossir jusqu'à ~1664 KB ; au-delà, plus de palier disponible sur une flash 4 MB sans sacrifier le FS → le passage à un module **8/16 MB** (PCB v3) devient la seule issue (déjà identifié dans l'ADR-0015).
- Le layout v3 devient la référence pour `partitions.csv`, `platformio.ini` (`filesystem_size = 589824`), `build_fs.sh` (`-s 589824`) et `deploy.sh` (`LITTLEFS_OFFSET=0x350000`) — les 4 références doivent rester alignées.
- [ADR-0015](0015-partition-app-1.5mb.md) passe en `Superseded by ADR-0019`.

## Migration

1. Compiler firmware + FS (`pio run` + `./build_fs.sh`).
2. **Flash USB obligatoire** : `./deploy.sh all` (réécrit bootloader + table + firmware, puis FS à 0x350000). **Ne pas utiliser `./deploy.sh factory`** (efface la NVS → perte config + régénération du mot de passe AP).
3. NVS, historique et coredump préservés ; OTA de nouveau disponible dès le premier boot.

Procédure détaillée : [UPDATE_GUIDE.md — Migration layout v2 → v3](../UPDATE_GUIDE.md#migration-layout-v2--v3--v240).

## Références

- Code : [`partitions.csv`](../../partitions.csv), [`platformio.ini`](../../platformio.ini), [`build_fs.sh`](../../build_fs.sh), [`deploy.sh`](../../deploy.sh)
- Spec : `specs/features/doing/feature-044-repartitionnement-app-1664k.md`
- ADR antérieurs : [ADR-0015](0015-partition-app-1.5mb.md) (layout v2, supersédé), [ADR-0018](0018-migration-uplot.md) (uPlot — prérequis), [ADR-0009](0009-partition-coredump.md) (partition coredump)
- Doc : [BUILD.md](../BUILD.md), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
