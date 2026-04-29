# Subsystem — `history`

- **Fichiers** : [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp)
- **Singleton** : `extern HistoryManager history;`
- **Partition** : `history` LittleFS **64 KB** (séparée de `spiffs`) — voir [`partitions.csv`](../../partitions.csv) et [ADR-0009](../adr/0009-partition-coredump.md).

## Rôle

Enregistre des snapshots des valeurs (pH, ORP, température, filtration active, dosing pH/ORP) toutes les 5 minutes. Consolide progressivement les données anciennes pour garder un historique utilisable sur **~82 jours** (7 jours de données horaires + 75 jours de données journalières) sans saturer la flash.

## Granularités

```cpp
enum Granularity : uint8_t {
  RAW    = 0,   // 5 min, gardés 6h    → 72 points max  (kMaxRawDataPoints)
  HOURLY = 1,   // moyenne 1h, gardée 7j → 168 points max (kMaxHourlyDataPoints)
  DAILY  = 2    // moyenne 1j, gardée 75j → 75 points max  (kMaxDailyDataPoints)
};
```

Voir [`constants.h:49`](../../src/constants.h:49) et [`history.h:30`](../../src/history.h:30).

## Cycle de vie d'un point

1. `recordDataPoint()` toutes les **5 min** (`RECORD_INTERVAL = 300000`) — ajout `RAW` dans `memoryBuffer`.
2. `consolidateData()` toutes les **5 min** (`SAVE_INTERVAL = 300000`) :
   - Points `RAW` plus anciens que 6 h → agrégés en moyenne horaire (`HOURLY`).
   - Points `HOURLY` plus anciens que 7 j → agrégés en moyenne journalière (`DAILY`).
   - Points `DAILY` plus anciens que 75 j → supprimés.
   - Appelle **`saveToFile()` en interne** à la fin du cycle de consolidation. Ne pas rappeler `saveToFile()` séparément depuis `update()` — c'est exactement le bug qui causait deux écritures flash + deux logs `Historique sauvegardé` toutes les 5 min.
3. Au boot : `loadFromFile()` restaure `memoryBuffer` depuis le fichier.

> Niveau de log : la trace `Consolidation terminée: N points` est en **DEBUG** (n'apparaît pas dans la persistance par défaut, niveau `INFO` minimum sur le fichier). Le marqueur antérieur `DEBUG: Début consolidation historique` a été supprimé.

## Pré-NTP handling

Si un snapshot est pris **avant** que l'heure soit synchronisée (timestamp uptime < `kMinValidEpoch`), il est marqué via `_preNtpPending = true`. Dès la synchro NTP réussie, `_applyPreNtpCorrection(ntpEpoch, uptimeSec)` re-date les points en calculant `epoch = ntpEpoch − (uptimeSec − pointUptime)`.

## API publique

```cpp
void begin();
void update();
void recordDataPoint();
std::vector<DataPoint> getLastHours(int hours);
std::vector<DataPoint> getLastDay();
std::vector<DataPoint> getAllData();
bool importData(const std::vector<DataPoint>& dataPoints);
void clearHistory();
```

## Endpoints HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Récupérer l'historique | `GET /get-history?range={24h|7d|30d|3d|all}` | READ |
| Importer un CSV | `POST /history/import` (multipart) | CRITICAL |
| Purger l'historique | `POST /history/clear` | CRITICAL |

Voir [`web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303). Colonnes CSV : `datetime, ph, orp, temperature, filtration, dosing, granularity`.

## Concurrence

Un mutex FreeRTOS (`_mutex`, `SemaphoreHandle_t`) protège `memoryBuffer` contre les accès simultanés entre la tâche de loop et les handlers web asynchrones.

## Partition dédiée

La partition `history` (64 KB, offset `0x3E0000`) est **séparée** de `spiffs` pour que les updates OTA du filesystem (qui réécrivent `spiffs`) ne touchent pas à l'historique. Voir [ADR-0009](../adr/0009-partition-coredump.md).

**Budget de la partition (64 KB utiles ≈ 56 KB après réserves LittleFS) :**

| Fichier | Taille max |
|---------|-----------|
| `history.bin` (données brutes + horaires + journalières) | ~24 KB |
| `system.log` (logs firmware persistés) | 16 KB |
| Fichier temporaire de rotation logs | 12 KB |
| **Total** | **~52 KB < 56 KB** |

### Protection au redimensionnement de partition

`HistoryManager::begin()` compare la taille réelle de la partition `history` (lue via l'API IDF) avec la taille stockée en NVS sous la clé `hist_meta/part_sz`. Si les tailles diffèrent (ou si la clé est absente = premier boot), la partition est effacée via `esp_partition_erase_range` avant montage LittleFS.

Cette protection évite un crash `IntegerDivideByZero` dans `lfs_alloc` lorsque LittleFS tente de monter un filesystem dont la géométrie ne correspond plus à la taille physique de la partition — situation qui se produit lors d'un redimensionnement flash (ex. migration ADR-0007 → ADR-0009).

> ⚠️ Le premier boot après un changement de table de partitions **efface l'historique**. C'est intentionnel et documenté dans [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md).

## Cas limites

- **Partition pleine** : écriture impossible → log ERROR, `historyEnabled` peut être basculé à `false` manuellement depuis l'UI Avancé.
- **Migration d'une ancienne version** : `migrateLegacyHistory()` lit les fichiers au format précédent si `legacyHistoryPending = true`.
- **Boot sans heure** : les points sont enregistrés avec un timestamp uptime, corrigés à la synchro NTP.
- **Import d'un CSV malformé** : ligne rejetée individuellement, le reste est importé.

## Fichiers liés

- [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp)
- [`src/constants.h:49`](../../src/constants.h:49) — limites (`kMaxHourlyDataPoints = 168`)
- [`partitions.csv`](../../partitions.csv)
- [ADR-0009](../adr/0009-partition-coredump.md) — table de partitions courante
- [ADR-0007](../adr/0007-table-partitions-custom.md) — supersedé
