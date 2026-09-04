# Subsystem — `history`

- **Fichiers** : [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp)
- **Singleton** : `extern HistoryManager history;`
- **Partition** : `history` LittleFS **64 KB** (séparée de `spiffs`) — voir [`partitions.csv`](../../partitions.csv) et [ADR-0009](../adr/0009-partition-coredump.md).

## Rôle

Enregistre des snapshots des valeurs (pH, ORP, température, filtration active, dosing pH/ORP) toutes les 5 minutes. Consolide progressivement les données anciennes pour garder un historique utilisable sur **~82 jours** (7 jours de données horaires + 75 jours de données journalières) sans saturer la flash. ⚠️ Objectif théorique : l'étage journalier est actuellement affamé — voir le *Suivi connu* de la section [Consolidation par créneau complet](#consolidation-par-créneau-complet--déduplication-v2181-bug-history-doublons-troncature).

## Granularités

```cpp
enum Granularity : uint8_t {
  RAW    = 0,   // 5 min, gardés ~7h (créneau complet) → 96 points max = 8h (kMaxRawDataPoints, v2.18.1)
  HOURLY = 1,   // moyenne 1h → 168 points max = 7j (kMaxHourlyDataPoints) ; HOURLY_MAX_AGE = 15j (⚠️ voir Suivi connu)
  DAILY  = 2    // moyenne 1j, gardée 90j (DAILY_MAX_AGE) → 75 points max (kMaxDailyDataPoints)
};
```

Voir [`constants.h:49`](../../src/constants.h:49) et [`history.h:30`](../../src/history.h:30).

## Cycle de vie d'un point

1. `recordDataPoint()` toutes les **5 min** (`RECORD_INTERVAL = 300000`) — ajout `RAW` dans `memoryBuffer`.
2. `consolidateData()` toutes les **5 min** (`SAVE_INTERVAL = 300000`) — depuis la v2.18.1, par **créneau complet** (voir section dédiée ci-dessous) :
   - Créneaux horaires `RAW` **complets** (dernière seconde de l'heure plus ancienne que `RAW_MAX_AGE` = 6 h, soit ~7 h après le début de l'heure) → **un** agrégat horaire (`HOURLY`).
   - Créneaux journaliers `HOURLY` **complets** (dernière seconde du jour plus ancienne que `HOURLY_MAX_AGE` = 15 j) → **un** agrégat journalier (`DAILY`). ⚠️ Voir *Suivi connu* : ce chemin est actuellement affamé.
   - Points `DAILY` plus anciens que 90 j (`DAILY_MAX_AGE`) → supprimés (prédicat **future-safe** `historyIsExpired` depuis v2.18.4 — un point horodaté dans le futur n'est jamais purgé comme « ancien »).
   - Après tri : `dedupAggregatedPoints()` (fusion des agrégats dupliqués de même créneau — ceinture-bretelles).
   - Appelle **`saveToFile()` en interne** à la fin du cycle de consolidation. Ne pas rappeler `saveToFile()` séparément depuis `update()` — c'est exactement le bug qui causait deux écritures flash + deux logs `Historique sauvegardé` toutes les 5 min.
3. Au boot : `loadFromFile()` restaure `memoryBuffer` depuis le fichier — avec, depuis la v2.18.4, **purge des points horodatés dans le futur** (voir [Garde anti-futur](#garde-anti-futur--purge-des-points-futurs-v2184-bug-points-futurs-et-ux-courbes)).

> Niveau de log : la trace `Consolidation terminée: N points` est en **DEBUG** (n'apparaît pas dans la persistance par défaut, niveau `INFO` minimum sur le fichier). Le marqueur antérieur `DEBUG: Début consolidation historique` a été supprimé.

## Math d'agrégation pure (`history_logic`)

- **Fichiers** : [`src/history_logic.h`](../../src/history_logic.h), [`src/history_logic.cpp`](../../src/history_logic.cpp)

Depuis **feature-041** (*characterization refactor*), la math **scalaire** d'agrégation est extraite de `history.cpp::consolidateData()` vers le module **pur** `history_logic` (pattern *Humble Object*, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)). Headers C uniquement (`<stdint.h>`/`<math.h>`), sans `<vector>`/`<map>`/Arduino/FreeRTOS — donc **testable en natif** sans libc++.

| Fonction | Rôle | Frontières / particularités |
|---|---|---|
| `bucketTimestamp(ts, bucketSeconds)` | Tronque un timestamp au début de son bucket (horaire/quotidien) : `(ts / bucketSeconds) * bucketSeconds` | `bucketSeconds == 0` → renvoie `ts` tel quel (garde anti-division par zéro) |
| `isOlderThan(now, ts, maxAgeSeconds)` | Prédicat d'ancienneté `(now - ts) > maxAgeSeconds` | Frontière **stricte** (`>`) : `age == maxAge` → `false`. Arithmétique `uint32`, wrap (`ts > now`) conservé tel quel |
| `finalizeMean(sum, count)` | Moyenne `sum/count` | `count == 0` → **`NaN`** (le point est ignoré par la coquille) |
| `isMajority(trueCount, total)` | Majorité stricte `trueCount > total/2` | Division **entière** : `2/4` → `false`, `3/4` → `true`, `3/5` → `true` |
| `anyTrue(count)` | « Au moins un » : `count > 0` | — |
| `historySaveAdmit(freeBytes, oldFileBytes, jsonBytes, marginBytes)` | Admission d'une écriture de `/history.json` : vrai si `libre + taille de l'ancien fichier ≥ JSON + marge` ([ADR-0033](../adr/0033-budget-espace-partition-history.md), v2.11.0) | Frontière **large** (égalité exacte → admis). Anti-overflow : soustractions gardées, aucune addition — correct jusqu'à `SIZE_MAX` sur chaque paramètre. **Logique NOUVELLE** (pas de contrainte characterization) |
| `historyBucketConsolidable(now, bucketStart, bucketLen, maxAgeSeconds)` | Créneau consolidable si **complet et ancien** : sa **dernière seconde** (`bucketStart + bucketLen − 1`) dépasse `maxAge` (v2.18.1, bug-history-doublons-troncature A1) | Même convention stricte qu'`isOlderThan` (`== maxAge` → `false`). `bucketLen == 0` → prédicat sur `bucketStart` seul (défensif). **Durcie v2.18.4** : dernière seconde dans le futur (`> now`) → **jamais consolidable** (voir [Garde anti-futur](#garde-anti-futur--purge-des-points-futurs-v2184-bug-points-futurs-et-ux-courbes)). **Logique NOUVELLE** |
| `historyMergeMeanFinite(a, b)` | Fusion **équipondérée** de deux valeurs d'un même champ lors de la déduplication : fini+fini → moyenne, un seul fini → lui, aucun → `NaN` (v2.18.1, A2) | Équipondéré car les effectifs d'origine ne sont pas stockés (approximation assumée pour les doublons hérités ≤ 2.18.0). **Logique NOUVELLE** |
| `historyIsExpired(now, ts, maxAgeSeconds)` | Ancienneté **future-safe** : `ts <= now && isOlderThan(...)` — un point horodaté dans le futur n'est **jamais** « expiré » (v2.18.4, bug-points-futurs-et-ux-courbes) | Utilisée par la purge 90 j de `consolidateData()`. L'invariant de wrap d'`isOlderThan` (documenté, verrouillé par tests) reste inchangé — la garde est posée **au-dessus**. **Logique NOUVELLE** |

Les deux passes de `consolidateData()` (raw → horaire, horaire → quotidien) **délèguent** à ces fonctions. *Characterization refactor* : la math reproduit **exactement** l'ancien comportement inline (frontières strictes, divisions entières, wrap `uint32`) — **aucun changement de comportement**. Ne pas « corriger » ces frontières (exception : `historySaveAdmit`, logique nouvelle).

> ⚠️ **Limite de testabilité** : seule la math scalaire est couverte en natif. Le **regroupement** par bucket (`std::map`/`std::vector`), l'accumulation des sommes/compteurs, la boucle de fusion `dedupAggregatedPoints()` et la **persistance** (`saveToFile`) restent dans la coquille `history.cpp` et **hors tests natifs** (contrainte libc++ absente de l'environnement natif). 48 tests Unity natifs (dont 10 pour `historySaveAdmit`, 8 pour `historyBucketConsolidable`/`historyMergeMeanFinite`, 5 pour la garde anti-futur — `historyIsExpired` passé/futur/frontières + bucket futur/en cours refusé) ; `history_logic.cpp` couvert à **100 % des lignes**.

## Consolidation par créneau complet + déduplication (v2.18.1, bug-history-doublons-troncature)

Jusqu'en v2.18.0, la consolidation appliquait le prédicat d'âge **par point** : les points d'une même heure franchissant `RAW_MAX_AGE` **progressivement** (un toutes les 5 min), chaque passage de consolidation créait un agrégat partiel supplémentaire au même timestamp — jusqu'à **12 doublons/heure** (idem HOURLY→DAILY), persistés dans `/history.json`, gonflant le buffer et rallongeant les consolidations (mutex tenu plus longtemps → contention sur `/get-history`).

**A1 — Créneau complet** : un créneau (heure ou jour) n'est converti que lorsque sa **dernière seconde** dépasse l'âge limite (`historyBucketConsolidable`, prédicat pur). Le prédicat est appliqué **à l'identique** au groupement ET à la suppression des points sources (sinon perte ou re-conversion). Résultat : **un seul** agrégat par créneau, calculé sur le créneau entier.

- Conséquence temporelle : un point RAW enregistré à `h:00` n'est agrégé que lorsque `h:59:59` dépasse `RAW_MAX_AGE` (6 h) — soit **~7 h** après le début de l'heure (contre 6 h par point avant). C'est ce qui impose `kMaxRawDataPoints = 96` (8 h) : à 72 (6 h pile), l'éviction par capacité supprimait les points RAW **avant** la complétude de leur créneau (affamage de l'agrégation horaire). Impact budget partition : +2 Ko de fichier, pire cas 14/16 blocs — note additive du 2026-08-09 dans [ADR-0033](../adr/0033-budget-espace-partition-history.md), garde `historySaveAdmit` inchangée.

**A2 — Déduplication/fusion** (`dedupAggregatedPoints()`, privée) : fusionne les agrégats de même `(timestamp, granularité ≠ RAW)` dans `memoryBuffer` — champs moyennés équipondérés (`historyMergeMeanFinite` : fini+fini → moyenne, un seul fini → lui, sinon NaN), booléens en **OR**. Pré-requis : buffer trié par timestamp (le tri n'étant pas stable, la recherche du doublon balaie toute la **plage d'ex æquo** de même timestamp). **Trois points d'appel** :

| Site | Rôle |
|---|---|
| `consolidateData()` (après tri) | Ceinture-bretelles derrière A1 + rattrapage d'un éventuel import |
| `loadFromFile()` (après tri défensif) | **Assainit en une fois, au premier boot**, les doublons persistés par les versions ≤ 2.18.0 — sans effacement d'historique |
| `importData()` (après tri) | Un CSV exporté d'une version ≤ 2.18.0 peut porter des doublons |

Quand des doublons sont fusionnés : log INFO `Historique : N agrégats dupliqués fusionnés`. La fusion équipondérée est une **approximation assumée** pour les doublons hérités (les effectifs d'origine ne sont pas stockés) — hors périmètre de correction, cf. spec.

> ⚠️ **Suivi connu (préexistant, hors lot — à traiter dans une spec future)** : `kMaxHourlyDataPoints` (168 points = **7 j**) < `HOURLY_MAX_AGE` (**15 j**) — les points HOURLY sont évincés par capacité **avant** d'atteindre l'âge de consolidation en DAILY : **les agrégats journaliers ne se forment jamais** (affamage structurel du 3ᵉ étage, antérieur à la v2.18.1). La rétention effective est donc ~7 j de données horaires (+ d'éventuels points DAILY historiques).

## Garde anti-futur + purge des points futurs (v2.18.4, bug-points-futurs-et-ux-courbes)

**Anomalie de production (2026-08-10)** : `/get-history` contenait un agrégat **journalier** daté du créneau du jour courant — doublement impossible (l'affamage DAILY documenté ci-dessus empêche leur formation, et un journalier du jour courant ne devrait naître que 15 j plus tard). Mécanisme : des points horodatés dans le **futur** (excursion d'horloge pendant une session de reboots) + wrap `uint32` d'`isOlderThan` (un point futur paraît vieux de ~136 ans) → consolidation immédiate HOURLY→DAILY avec un bucket futur. Le point fantôme polluait le bord droit des courbes.

Deux gardes, complémentaires :

1. **Prédicats d'âge future-safe** (`history_logic`, pur + tests) : la purge 90 j utilise `historyIsExpired(now, ts, maxAge)` = `ts <= now && isOlderThan(...)` ; `historyBucketConsolidable` est durcie de même (dernière seconde du bucket dans le futur → **jamais** consolidable). L'invariant de wrap d'`isOlderThan` lui-même est **inchangé** (documenté, verrouillé par tests — cf. tableau ci-dessus) : la garde est posée au-dessus, pas dedans.
2. **Assainissement au chargement** (`loadFromFile()`, après tri/dédup) : purge des points `ts > now + kHistoryFutureToleranceSeconds` (**3600 s** — absorbe un décalage d'horloge bénin sans laisser passer les excursions franches, [`constants.h`](../../src/constants.h)), **uniquement si l'horloge est disponible** (`getCurrentEpoch() > 0` — RTC/NTP ; sans heure, on ne juge pas). Log **INFO** `Historique : N point(s) horodaté(s) dans le futur purgé(s)` — le point fantôme disparaît au premier boot.

> La garde 1 empêche la **formation** de nouveaux agrégats fantômes (même si des points futurs entrent dans le buffer) ; la garde 2 **assainit** ceux déjà persistés. Les points futurs encore présents en RAM entre deux boots ne sont plus ni purgés ni consolidés — ils redeviennent des points normaux quand l'horloge les rattrape.

## Exclusion des mesures pH/ORP/température sans circulation d'eau (feature-061, v2.13.0)

`recordDataPoint()` force **`point.ph = NAN`, `point.orp = NAN` et `point.temperature = NAN`** (même bloc) quand `measureValidity.measuresValid()` est faux — c'est-à-dire quand l'eau ne circule pas (filtration arrêtée, signal externe périmé) **ou** pendant la **purge** post-reprise — l'eau stagnante de la chambre de mesure doit être chassée, puis le transitoire de rinçage doit s'éteindre. Voir [measure-validity.md](measure-validity.md).

> **v2.34.0 (feature-091)** : la purge n'est plus une minuterie réglable (`stabilizationDelayMin`, retiré) mais s'arrête **sur la stabilité des bruts**, entre un plancher de 10 min et un plafond de 90 min. Conséquence pour l'historique : le nombre de points `NaN` du matin n'est plus prévisible à l'avance et varie d'un jour à l'autre (typiquement 30 à 50 min de rinçage). C'est voulu — les points exclus sont exactement ceux qui n'étaient pas représentatifs. Voir [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md).

> La température a rejoint pH/ORP en **itération 2** de feature-061 : la sonde DS18B20 est dans la canalisation → à l'arrêt, l'eau du tuyau dérive vers la température ambiante du local (plateaux nocturnes non représentatifs du bassin). Même drapeau `measures_valid`, pas de validité séparée.

- Le NaN est sérialisé **`null`** dans `/history.json` et `/get-history` (contrat existant, identique à un capteur absent) → **trous dans les courbes** côté UI.
- **État filtration et flags dosage restent enregistrés** tels quels (l'état filtration est nécessaire aux courbes).
- **Agrégation** : les NaN sont ignorés par la consolidation (accumulation dans la coquille ; un bucket sans aucune valeur → `finalizeMean(sum, 0)` = NaN → point `null`). Un jour entièrement à l'arrêt produit un point `DAILY` avec pH/ORP/température `null`.
- **Pas de purge rétroactive** : les points enregistrés avant la v2.13.0 pendant des arrêts de filtration restent dans l'historique tels quels — seuls les nouveaux points sont exclus.

L'exclusion s'applique **au point enregistré** (dans `recordDataPoint()`), pas en post-traitement — `measureValidity.update()` est appelé dans la loop **avant** `history.update()`.

## Pré-NTP handling

Si un snapshot est pris **avant** que l'heure soit synchronisée (timestamp uptime < `kMinValidEpoch`), il est marqué via `_preNtpPending = true`. Dès la synchro NTP réussie, `_applyPreNtpCorrection(ntpEpoch, uptimeSec)` re-date les points en calculant `epoch = ntpEpoch − (uptimeSec − pointUptime)`.

## Sauvegarde `saveToFile()` — garde d'espace (v2.11.0, [ADR-0033](../adr/0033-budget-espace-partition-history.md))

Cause racine des 2 « corruptions » de juillet 2026 : **saturation structurelle** de la partition (pire cas 19-22 blocs demandés vs 16) déclenchant un div/0 `lfs_alloc` sur FS plein — bug littlefs amont ([#1063](https://github.com/littlefs-project/littlefs/issues/1063), [esp_littlefs #183](https://github.com/joltwallet/esp_littlefs/issues/183)), version figée par `espressif32@6.9.0`. Depuis la v2.11.0, `saveToFile()` suit la séquence **mesure → garde → remove → écriture → vérification** (l'ancien code ouvrait `"w"` d'entrée, tronquant l'ancien fichier avant toute vérification) :

1. **Mesure** : le JSON est construit en RAM et mesuré (`measureJson`) **avant** de toucher au FS.
2. **Garde d'admission** : `historySaveAdmit(libre, tailleAncienFichier, tailleJson, kHistorySaveSpaceMarginBytes = 8192)` (module pur `history_logic`). **Refus** → save sauté, **l'ancien fichier reste intact** (mieux vaut un historique périmé de 5 min qu'un fichier tronqué), retentative naturelle au cycle suivant.
3. **`remove()` de l'ancien fichier AVANT réécriture** : divise le pic copy-on-write par 2 (ancien + nouveau ne coexistent plus). Trade-off acté : un crash dans la fenêtre remove→write perd l'historique persisté — borné par la cadence 5 min et couvert par le disjoncteur ([ADR-0031](../adr/0031-disjoncteur-bootloop-history.md)).
4. **Vérification d'écriture partielle** : le retour de `serializeJson` doit être **exactement** la taille mesurée ; sinon le fichier partiel est **supprimé** (jamais de JSON tronqué silencieux).

**Chemins d'échec (refus, échec d'`open`, écriture partielle)** — tous identiques :

- log **WARNING** (jamais ERROR/CRITICAL : un ERROR déclencherait un flush disque immédiat sur le FS déjà plein toutes les 5 min — anti-flush-storm), **throttlé à 1/h** (`kHistorySaveWarnThrottleMs`, statique par site) ;
- `recordHistorySaveFailureNvs()` : incrément du compteur cumulé `nospc_cnt` + persistance de `last_ok` (epoch du dernier save réussi, tenu en RAM `_lastOkEpoch`) en NVS `hist_meta`. Écriture NVS **uniquement sur échec**, jamais à chaque save réussi (usure NVS).

Sur save réussi : `_lastOkEpoch` mis à jour en RAM + INFO d'occupation FS `Partition history : used/total octets utilisés`, throttlé 1/h.

**Budget pire cas résultant** (avant : 2 méta + 12 history COW ×2 + 5 log + 3 tmp = **22 blocs > 16**) : 2 méta + 6 history + 3 log (≤ 8 Ko) + 2 tmp (6 Ko) = **13 blocs ≤ 16, marge 3 blocs**. Les plafonds log réduits font partie du budget — voir [logger.md](logger.md#persistance-sur-littlefs-partition-history). ⚠️ Toute nouvelle écriture sur cette partition doit passer par la garde ou démontrer le respect du budget.

## API publique

```cpp
void begin();
void update();
void recordDataPoint();
std::vector<DataPoint> getLastHours(int hours);
std::vector<DataPoint> getLastDay();
std::vector<DataPoint> getAllData();
// v2.18.2 (bug-history-doublons-troncature it.2) : instantané FILTRÉ du buffer
// (timestamp >= cutoff, > since ; 0 = désactivé) copié en UNE prise de mutex
// (kHistorySnapshotTimeoutMs = 100 ms) pour la réponse chunked /get-history —
// le callback ne prend ensuite AUCUN mutex. Coût borné ~11 Ko, libéré en fin
// de réponse. false = mutex indisponible (rare) → la route répond 503.
bool snapshotFiltered(std::vector<DataPoint>& out, unsigned long cutoff, unsigned long since);
bool importData(const std::vector<DataPoint>& dataPoints);
bool clearHistory();   // false si le mutex n'a pas pu être pris (historique occupé) — v2.11.1
```

## Export chunked `/get-history` (v2.14.0, refondu v2.18.2)

Jusqu'en v2.13.x, `/get-history` copiait **tout** le buffer (`getLastHours`/`getAllData`) puis concaténait la réponse dans une `String` contiguë pré-réservée (~40 Ko épinglés pendant tout le transfert à chaque ouverture du dashboard) — **générateur de fond n°3 (G3)** de la fragmentation heap identifié par l'investigation churn-heap du 24/07. Depuis la v2.14.0, la route est **chunked** ([ADR-0032 §Extension 2026-07-30](../adr/0032-reponses-http-bornees.md#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140), patron `/download-logs`) ; depuis la v2.18.2, elle streame depuis un **instantané figé** :

- **`snapshotFiltered(out, cutoff, since)`** (v2.18.2) : copie de l'instantané **filtré** du buffer en **UNE** prise de mutex à l'ouverture de la requête — timeout dédié court `kHistorySnapshotTimeoutMs = 100 ms` (le timeout standard 2 s bloquerait la tâche `async_tcp`). Refus (rare) → la route répond **`503` `{"error":"historique occupé, réessayez"}`**. Coût borné : ≤ `kMaxRawDataPoints + kMaxHourlyDataPoints + kMaxDailyDataPoints` points ≈ **11 Ko transitoires**, libérés en fin de réponse. Le callback chunked ne prend ensuite **aucun mutex** : aucun timeout, aucun glissement d'index possible (l'instantané est immuable → plus de doublon/saut best-effort).
- **Filtres `range`/`since` appliqués à la copie** (fonction pure `historyRangeCutoff`, sémantique exacte de l'ancien `getLastHours`) ; formatage d'un point par la fonction pure `formatHistoryPointJson` ([`src/history_export_chunk.h`](../../src/history_export_chunk.h)) — format JSON **octet pour octet identique** à l'ancienne concaténation (y compris la clé `count` émise deux fois, voir [API.md](../API.md#get-get-history--write)).
- **Cause racine C — pied jamais émis (bug v2.14.0 → v2.18.1, corrigé v2.18.2)** : dans la machine à états du callback, le cas `kFooter` chargeait le pied `],"count":N}` dans le tampon de report (carry) **et** passait en phase `kDone` dans la **même itération** ; or la condition de boucle (`phase != kDone` seul) sortait **avant** d'émettre le carry → le pied n'était **JAMAIS envoyé** → JSON invalide sur **toutes** les réponses → graphiques morts depuis le déploiement v2.14 (05/08). Démontré au flux chunked brut sur l'appareil (chunks reçus = exactement l'émis, sans pied). **Fix** : la boucle continue tant que le carry n'est pas vide, **même en `kDone`** ([`src/web_routes_data.cpp:464`](../../src/web_routes_data.cpp)). Validé sur l'appareil (pied présent, JSON valide).
- ⚠️ **Diagnostic v2.18.1 invalidé** : la théorie « troncature par `RESPONSE_TRY_AGAIN`/timeout mutex en cours de flux » (correctif « espace de maintien de flux ») était **fausse** — le mécanisme complet a été **retiré** en v2.18.2 (`getPointByIndex`, `kHistoryPointReadTimeoutMs`, `kHistoryChunkMaxRetries` supprimés) ; plus aucun espace n'est émis dans le corps.
- **Trace de clôture (DEBUG)** : à la fin du flux, `get-history clos: emis=N o, points=N, idx=N, dernierAppel=N o` — l'émis réel de la machine à états, à confronter aux octets reçus côté client en cas de doute.
- ⚠️ **Note** : `migrateLegacyHistory()` n'est **plus appelé** sur ce chemin (l'ancien `getLastHours` le déclenchait) — la migration legacy reste couverte par `update()`/`loadFromFile()`/`importData()`.

Tests natifs : suite dédiée `test/test_native_history_export/` (cutoff `range`, format point, arrondis, NaN → `null`, virgules, capacité).

> 📋 **Dette (recommandation de revue v2.18.2)** : extraire la **machine à états chunked** de `web_routes_data.cpp` en logique pure testable en natif ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)), avec au minimum un test « **le flux se termine par le pied `],"count":N}`** ». C'est précisément l'absence de ce test qui a laissé le bug du pied jamais émis passer **5 jours en production et 2 revues de code** (seules les fonctions de formatage étaient couvertes, pas la boucle d'émission).

## Endpoints HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Récupérer l'historique | `GET /get-history?range={24h|7d|30d|3d|all}` | READ |
| Importer un CSV | `POST /history/import` (multipart) | CRITICAL |
| Purger l'historique | `POST /history/clear` | CRITICAL |

Voir [`web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303). Colonnes CSV : `datetime, ph, orp, temperature, filtration, dosing, granularity`.

`POST /history/clear` répond **`503` « Historique occupé — réessayer »** si `clearHistory()` retourne `false` (mutex non obtenu dans le délai — voir section Concurrence).

## Concurrence

Un mutex FreeRTOS (`_mutex`, `SemaphoreHandle_t`) protège `memoryBuffer` contre les accès simultanés entre la tâche de loop et les handlers web asynchrones.

### Timeouts mutex bornés (feature-027, v2.11.1)

Les **6 prises de mutex** de `history.cpp` sont bornées par `kHistoryMutexTimeoutMs = 2000 ms` ([`constants.h`](../../src/constants.h)) — dimensionné sur le **pire détenteur légitime** : la consolidation + `saveToFile()` LittleFS (~1–1,5 s mesuré). Chaque timeout émet un `WARN` `[History] <site>: timeout mutex — opération sautée` **throttlé à 1/min/site** (`kMutexTimeoutWarnThrottleMs = 60000`, statique locale par site).

Politique d'échec par site :

| Site | Sur timeout |
|---|---|
| `update()` → `recordDataPoint()` | Point sauté, **`lastRecord` NON avancé** → retry naturel au tour de loop suivant (pas de trou de 5 min) |
| `update()` → `consolidateData()` | Consolidation sautée, **`lastSave` NON avancé** → retry naturel au tour suivant |
| `getLastHours()` | Vecteur vide (le client HTTP réessaiera). ⚠️ Un handler `async_tcp` peut désormais attendre **au plus 2 s** sur ce chemin — contre une attente *infinie* avant v2.11.1 (relevé en revue, amélioration nette) |
| `getAllData()` | Vecteur vide |
| `importData()` | `false` **avant toute modification** de `memoryBuffer` — la route teste le retour |
| `clearHistory()` | `false` **sans supprimer le fichier** (la RAM n'a pas été vidée → pas d'état incohérent) ; la route répond `503` |
| `snapshotFiltered()` (v2.18.2) | ⚠️ Timeout **dédié court** `kHistorySnapshotTimeoutMs = 100 ms` (pas 2 s : appelé depuis le handler HTTP sur `async_tcp`) — retour `false`, la route `/get-history` répond **`503` « historique occupé, réessayez »** (rare : la fenêtre de contention est la consolidation + save, ~1,5 s toutes les 5 min). **Aucun warn** (contention attendue et gérée). Le callback chunked, lui, ne prend **aucun** mutex |

Comportement nominal (mutex libre) strictement inchangé.

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

### Détection de corruption LittleFS au boot (v2.1.1)

Le mécanisme précédent (vérification de taille) ne couvre **pas** les corruptions internes du filesystem (bit flips, écritures interrompues, métadonnées LittleFS incohérentes). LittleFS peut **monter sans erreur un FS corrompu** puis crasher en `IntegerDivideByZero` à la première écriture — symptôme observé sur PCB v1 après une longue inactivité ESP32 (boot loop sans message après le log « Partition historique dédiée montée »).

Depuis le commit `3c8b657` (v2.1.1), `HistoryManager::begin()` exécute, **après** un mount LittleFS réussi et **avant** tout autre accès (notamment `setPersistenceFs` qui déclenche le 1ᵉʳ printf vers le filesystem) :

1. **Test write** : ouverture en écriture du fichier témoin `/.fscheck`, écriture des 2 octets `"ok"`, fermeture.
2. **Test read** : ouverture en lecture, comparaison du contenu lu avec `"ok"`.
3. **Cleanup** : suppression de `/.fscheck`.

Si **toute** étape échoue (`open` retourne null, `print` retourne ≠ 2 octets, ou contenu lu ≠ `"ok"`), le firmware considère la partition corrompue :

- Log `warning` : `Partition history corrompue détectée — reformatage automatique`.
- Démontage LittleFS (`historyFs.end()`), effacement intégral via `esp_partition_erase_range(histPart, 0, histPart->size)`, remontage avec `formatOnFail=true`.
- Si le remontage post-erase échoue à son tour : log `error` + `historyEnabled = false` (le système boote, mais sans persistance d'historique).
- Sinon : log `info` : `Partition history reformatée et remontée`. L'historique persistant **est perdu** — c'est jugé acceptable face à un boot loop infini.

| Cas | Coût |
|-----|------|
| Nominal (FS sain) | 4 opérations FS + cleanup ≈ 10 ms au boot, ~500 B flash ajoutés |
| Corrompu | Erase 64 KB ≈ 100 ms + remount + reformatage LittleFS ≈ 300-500 ms total |

> Cette validation s'exécute **après** le test de redimensionnement de partition décrit ci-dessus. Les deux mécanismes sont indépendants : le premier détecte un changement de géométrie matérielle, le second détecte une corruption logique du filesystem monté avec succès.

## Cas limites

- **Partition pleine** : écriture impossible → log ERROR, `historyEnabled` peut être basculé à `false` manuellement depuis l'UI Avancé.
- **Migration d'une ancienne version** : `migrateLegacyHistory()` lit les fichiers au format précédent si `legacyHistoryPending = true`.
- **Boot sans heure** : les points sont enregistrés avec un timestamp uptime, corrigés à la synchro NTP.
- **Import d'un CSV malformé** : ligne rejetée individuellement, le reste est importé.

## Fichiers liés

- [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp)
- [`src/constants.h:49`](../../src/constants.h:49) — limites (`kMaxHourlyDataPoints = 168`)
- [`partitions.csv`](../../partitions.csv)
- [`src/history_logic.h`](../../src/history_logic.h), [`src/history_logic.cpp`](../../src/history_logic.cpp) — math d'agrégation pure
- [`src/history_export_chunk.h`](../../src/history_export_chunk.h) — logique pure de l'export chunked `/get-history` (v2.14.0) ; carry partagé avec [`src/logs_export_chunk.h`](../../src/logs_export_chunk.h)
- [ADR-0009](../adr/0009-partition-coredump.md) — table de partitions courante
- [ADR-0007](../adr/0007-table-partitions-custom.md) — supersedé
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — logique métier pure / Humble Object
