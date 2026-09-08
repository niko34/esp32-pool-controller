# Subsystem — `history`

- **Fichiers** : [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp)
- **Singleton** : `extern HistoryManager history;`
- **Partition** : `history` LittleFS **256 KB** à l'offset `0x7B0000` (séparée de `spiffs`) — layout v5, voir [`partitions.csv`](../../partitions.csv), [ADR-0068](../adr/0068-partitions-layout-v5-flash-8-mo.md) et [ADR-0009](../adr/0009-partition-coredump.md). ⚠️ **Était 64 KB à `0x3E0000` jusqu'au 2026-09-05** — tous les budgets chiffrés « x/16 blocs » ci-dessous datent de cette époque et sont désormais **très conservateurs** (la partition compte 64 blocs).


> ⚠️ **Accès fichiers — 5 des 14 sites `fopen` du firmware sont ici**
> (compte recompté le 2026-09-08 : ce bandeau annonçait **6 sites** et des
> numéros de ligne d'avant la v2.43.0). `history.cpp` ouvre `/history.json` aux
> lignes **620**, **651** et **716**, et `/.fscheck` aux lignes **343** et
> **347**. Le 6ᵉ — l'`exists()` invisible à tout `grep` — a été **supprimé** au
> cycle 2, pas gardé (cf. [plus bas](#loadfromfile--lexists-a-été-supprimé-pas-gardé)).
> Chaque ouverture alloue un verrou newlib dont l'échec appelle **`abort()`** :
> c'est le crash du 05/09
> ([`docs/incidents/2026-09-05-panic-fopen-historique.txt`](../incidents/2026-09-05-panic-fopen-historique.txt)).
> Stratégie retenue : [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) ;
> recensement : [acces-fichiers.md](acces-fichiers.md). ✅ **Gardes posées depuis
> la v2.43.0** (cycle 2 de `bug-100`).

## Rôle

Enregistre des snapshots des valeurs (pH, ORP, température, filtration active, **masque de voies de dosage** pH/ORP — voir [Masque de voies de dosage](#masque-de-voies-de-dosage-bug-098-v2470)) toutes les 5 minutes. Depuis la **v2.38.0** (feature-095), chaque point `RAW` porte **en plus** la mesure **lissée** pH/ORP — celle sur laquelle la régulation décide — voir [Mesure lissée historisée](#mesure-lissée-historisée-feature-095-v2380). Consolide progressivement les données anciennes pour garder un historique utilisable sur **~82 jours** (7 jours de données horaires + 75 jours de données journalières) sans saturer la flash. ⚠️ Objectif théorique : l'étage journalier est actuellement affamé — voir le *Suivi connu* de la section [Consolidation par créneau complet](#consolidation-par-créneau-complet--déduplication-v2181-bug-history-doublons-troncature).

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
| `historySaveAdmit(freeBytes, oldFileBytes, jsonBytes, marginBytes)` | Admission d'une écriture de `/history.json` : vrai si `libre + taille de l'ancien fichier ≥ JSON + marge` ([ADR-0033](../adr/0033-budget-espace-partition-history.md), v2.11.0) | Frontière **large** (égalité exacte → admis). Anti-overflow : soustractions gardées, aucune addition — correct jusqu'à `SIZE_MAX` sur chaque paramètre. **Logique NOUVELLE** (pas de contrainte characterization) |
| `historyBucketConsolidable(now, bucketStart, bucketLen, maxAgeSeconds)` | Créneau consolidable si **complet et ancien** : sa **dernière seconde** (`bucketStart + bucketLen − 1`) dépasse `maxAge` (v2.18.1, bug-history-doublons-troncature A1) | Même convention stricte qu'`isOlderThan` (`== maxAge` → `false`). `bucketLen == 0` → prédicat sur `bucketStart` seul (défensif). **Durcie v2.18.4** : dernière seconde dans le futur (`> now`) → **jamais consolidable** (voir [Garde anti-futur](#garde-anti-futur--purge-des-points-futurs-v2184-bug-points-futurs-et-ux-courbes)). **Logique NOUVELLE** |
| `historyMergeMeanFinite(a, b)` | Fusion **équipondérée** de deux valeurs d'un même champ lors de la déduplication : fini+fini → moyenne, un seul fini → lui, aucun → `NaN` (v2.18.1, A2) | Équipondéré car les effectifs d'origine ne sont pas stockés (approximation assumée pour les doublons hérités ≤ 2.18.0). **Logique NOUVELLE** |
| `historyIsExpired(now, ts, maxAgeSeconds)` | Ancienneté **future-safe** : `ts <= now && isOlderThan(...)` — un point horodaté dans le futur n'est **jamais** « expiré » (v2.18.4, bug-points-futurs-et-ux-courbes) | Utilisée par la purge 90 j de `consolidateData()`. L'invariant de wrap d'`isOlderThan` (documenté, verrouillé par tests) reste inchangé — la garde est posée **au-dessus**. **Logique NOUVELLE** |

Les deux passes de `consolidateData()` (raw → horaire, horaire → quotidien) **délèguent** à ces fonctions. *Characterization refactor* : la math reproduit **exactement** l'ancien comportement inline (frontières strictes, divisions entières, wrap `uint32`) — **aucun changement de comportement**. Ne pas « corriger » ces frontières (exception : `historySaveAdmit`, logique nouvelle).

> ⚠️ **`anyTrue(count)` a été RETIRÉE du module en v2.47.0** (bug-098), avec ses **trois** tests. L'union **bit à bit** des masques de dosage (`doseMaskUnion |= p.doseMask`) l'a remplacée dans les deux boucles de `consolidateData()`, **voie par voie**, et son dernier appelant (`anyTrue(phDosingCount)`) a disparu avec les deux booléens de `DataPoint`. Ses tests ont été supprimés **dans le même diff** : des assertions vertes sur du code mort font croire à un invariant vivant et maintiennent une couverture de 100 % trompeuse.

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

## Masque de voies de dosage (bug-098, v2.47.0)

`DataPoint` portait **deux** booléens, `phDosing` et `orpDosing`, mais la sérialisation les **fusionnait** :

```cpp
obj["d"] = point.phDosing || point.orpDosing;   // écriture — la voie disparaît
dp.phDosing = point["d"];                        // relecture — elle réapparaît en pH
```

Sur tout point rechargé depuis `/history.json` — donc **après chaque redémarrage** — une injection de **chlore seul** se relisait en **dosage pH**. Le firmware ne perdait pas l'information, il en **fabriquait une fausse**, puis `consolidateData()` la figeait dans les agrégats **DAILY** pour 75 à 90 jours. Les points encore en mémoire vive, eux, disaient vrai : l'historique était **incohérent avec lui-même selon l'âge du point**.

Les deux booléens sont remplacés par **un champ de bits** `uint8_t DataPoint::doseMask` ([`src/history.h`](../../src/history.h)), conformément à l'idiome énoncé par l'[ADR-0072](../adr/0072-absence-de-valeur-hors-du-domaine.md) — *le domaine d'une valeur ne porte jamais l'absence de valeur*.

| Bit | Constante | Sens |
|---|---|---|
| `0x01` | `kHistDosePh` | pompe **pH** active à l'instant du point |
| `0x02` | `kHistDoseOrp` | pompe **ORP (chlore)** active |
| `0x04` | `kHistDoseUnknownChannel` | **dosage, voie indéterminée** — point écrit par un firmware ≤ 2.46 |

- `0` = aucun dosage. Bits **3-7 libres** : une voie de plus ne coûtera **pas un octet** de `DataPoint` (`static_assert(sizeof(DataPoint) == 28)` inchangé, voir [Export chunked](#export-chunked-get-history-v2140-refondu-v2182) qui en dépend pour son allocation contiguë).
- Les valeurs de bits sont un **CONTRAT** : persistées dans `/history.json` (clé courte `d`) **et** servies par `GET /get-history` (clé `dosing`). Elles ne se renumérotent pas.
- **Le troisième état est le cœur du correctif.** Relire un booléen `true` en `kHistDosePh` **inventerait** un dosage pH sur une injection ORP seule. On dit donc « dosage, voie **indéterminée** », jamais `false` — ni `kHistDosePh`.

### Le discriminant de format : le TYPE JSON, pas une clé de version

`loadFromFile()` sépare les deux écritures possibles **par le type de la valeur**, sans numéro de schéma ([`src/history.cpp`](../../src/history.cpp)) :

| Valeur de `"d"` | Écrite par | Relue en |
|---|---|---|
| `true` | firmware **≤ 2.46** | `kHistDoseUnknownChannel` (`0x04`) |
| `false` | firmware **≤ 2.46** | `0` |
| `0` … `7` | firmware **≥ 2.47** | le masque, réduit à `0x07` |
| **absente** | — | `0` |

⚠️ **Le masque de réduction est `0x07`, PAS `0x03`** — et c'est un piège rencontré pendant le cycle, pas une précaution théorique. Avec `& (kHistDosePh | kHistDoseOrp)`, le **premier** cycle sauvegarde/relecture qui suit l'OTA — **cinq minutes** après la mise à jour — écrivait `"d":4` puis relisait `0` : le mensonge favorable revenait **par la porte de derrière, réintroduit par le correctif lui-même**. Le masque inclut donc `kHistDoseUnknownChannel` aux **trois** sites de lecture (`loadFromFile`, `POST /history/import`, et le codec de test qui les modélise).

⚠️ **Limite assumée du discriminant** : un `"d"` **flottant** (`3.0`) ou entier **64 bits hors plage `int`** ne satisfait **ni** `is<bool>()` **ni** `is<int>()` — il retombe donc sur `0`, « aucun dosage », c'est-à-dire le mensonge favorable. Ni le firmware ni l'export ne produisent ces formes : il faut un **fichier édité à la main** ou un export étranger. Non traité pour ne pas élargir le discriminant à des types qui rendraient la séparation des deux formats moins nette.

### Rétrogradation : sûre par construction

Un firmware **≤ 2.46** qui relit un fichier écrit par la 2.47 lit `"d":3` avec `dp.phDosing = point["d"]`. ArduinoJson convertit alors l'entier non nul en `true` — soit **exactement le comportement actuel** de cette version : « il y a eu un dosage ». La rétrogradation ne produit ni erreur, ni perte supplémentaire par rapport à ce que ce firmware savait déjà faire. `"d":0` se lit `false`, également correct.

### Agrégation et déduplication : union BIT À BIT

Les deux passes de `consolidateData()` (raw → horaire, horaire → quotidien) accumulent un `doseMaskUnion` :

```cpp
doseMaskUnion = (uint8_t)(doseMaskUnion | p.doseMask);   // « au moins un », voie par voie
```

C'est ce qui a **retiré son dernier appelant** à `anyTrue()` (voir [Math d'agrégation pure](#math-dagrégation-pure-history_logic)). Même règle à la fusion des doublons (`dedupAggregatedPoints()`) : `dup->doseMask |= p.doseMask`.

Conséquence à connaître : un agrégat **horaire** peut porter `0x05` ou `0x07` — le `0x04` d'un point ancien **cumulé** avec le `0x01`/`0x02` de points récents. Ce n'est pas une incohérence, c'est la lecture littérale du créneau : « il y a eu du pH, et il y a eu du dosage dont on ne connaît pas la voie ».

### Budget du format persisté : il RÉTRÉCIT

L'ancienne écriture coûtait `,"d":false` (**10 o**) ou `,"d":true` (**9 o**) ; la nouvelle coûte `,"d":0` … `,"d":7` (**6 o**). Sur la charge de référence — les plafonds de [`constants.h`](../../src/constants.h), 96 RAW + 168 HOURLY + 75 DAILY = **339 points** — la clé `d` passe de **3 147 à 2 034 octets**, soit **−1 113 o**.

**Méthode** (cf. [ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md)) : décompte structurel des longueurs de clé, régime réaliste (les points RAW ne dosent pas la plupart du temps, les agrégats portent au moins une voie), **asserté** par `test_budget_charge_realiste_de_339_points_retrecit` dans [`test/test_native_dose_mask/test_dose_mask.cpp`](../../test/test_native_dose_mask/test_dose_mask.cpp). Le test vérifie aussi que **même le pire cas hors contrat** (`,"d":255`, 8 o) reste plus court que `,"d":false`.

⚠️ **Le modèle `historyPointPersistedLen` de [`test/test_native_history/test_history_logic.cpp`](../../test/test_native_history/test_history_logic.cpp) n'a pas été mis à jour** : il modélise encore `"d":true/false` et une partition de 64 Ko (256 Ko depuis l'[ADR-0068](../adr/0068-partitions-layout-v5-flash-8-mo.md)). Il se trompe **du côté sûr** — il annonce le fichier plus **gros** qu'il n'est, de ces mêmes 1 113 o — donc ses tests restent verts et pertinents comme **majorant**. Le corriger propage sur une dizaine de constantes asservies et sur tout le récit de budget : **spec de suivi nécessaire**.

### Pire cas d'un point exporté : 161 o absolus, 157 atteignables

`formatHistoryPointJson()` ([`src/history_export_chunk.h`](../../src/history_export_chunk.h)) sert le masque en **entier** (`"dosing":%u`), **verbatim** — aucune traduction entre le fichier interne et le fil HTTP.

- **161 o** : pire cas **absolu** du format, les deux `%u` saturés à `"255"`. Mesuré comme le **maximum d'un balayage exhaustif** des 65 536 couples (masque, granularité), pas postulé.
- **157 o** : pire cas **atteignable** — le masque ne dépasse pas `7` et la granularité est bornée à `DAILY = 2`, soit **un** caractère chacun.
- `kHistoryPointJsonMaxBytes = 192` laisse donc **31 o** (absolu) et **35 o** (atteignable) de marge. **Le changement de bug-098 AGRANDIT la marge de 2 o**, il ne la consomme pas.
- Le balayage exhaustif garde le **mode de défaillance silencieux** : un point qui ne tient pas dans le report est **sauté** par la phase `kPoints` — le trou de courbe qui en résulte est indiscernable d'une panne de sonde. Aucun couple ne le déclenche. Voir aussi [Points omis de `/get-history`](#points-omis-de-get-history--la-perte-nest-plus-silencieuse).

### Surfaces impactées

| Surface | Effet |
|---|---|
| `/history.json` (persisté) | clé `d` **entière** ; relecture typée tolérante aux deux formats |
| `GET /get-history` | clé `dosing` **entière** — voir [API.md](../API.md#dosing--un-masque-de-voies-entier-bug-098-v2470) |
| `POST /history/import` | lecture **typée** ; ⚠️ `item["dosing"] \| false` aurait rendu `false` sur un entier (l'opérateur `\|` d'ArduinoJson exige la **concordance de type**) et effacé tout masque relu d'un export neuf |
| Export CSV de l'interface | colonne **`dosing_mask`** ajoutée, `dosing` conservée en booléen d'héritage |
| Mini-graphiques pH / ORP | marqueurs de dosage **par voie** — voir [page-dashboard.md](../features/page-dashboard.md) |

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

**Budget pire cas résultant** (avant : 2 méta + 12 history COW ×2 + 5 log + 3 tmp = **22 blocs > 16**) : 2 méta + 6 history + 3 log (≤ 8 Ko) + 2 tmp (6 Ko) = **13 blocs ≤ 16, marge 3 blocs**. Les plafonds log réduits font partie du budget — voir [logger.md](logger.md#persistance-sur-littlefs-partition-history). ⚠️ Toute nouvelle écriture sur cette partition doit passer par la garde ou démontrer le respect du budget. Décompte réactualisé en v2.38.0 : [Budget de partition, avant / après](#budget-de-partition--avant--après-feature-095).

### Le mode de défaillance du budget est le SILENCE, pas la corruption ⚠️

C'est l'information la plus importante de cette page, et la moins intuitive. Un dépassement de budget **ne corrompt rien** et **ne casse rien de visible** :

1. `historySaveAdmit()` **refuse** l'écriture **avant** de toucher au filesystem — l'ancien `/history.json` reste intact ;
2. un **WARNING throttlé 1/h** est journalisé (jamais ERROR : un ERROR déclencherait un flush disque immédiat sur un FS déjà plein, toutes les 5 min) ;
3. l'UI **continue d'afficher** l'historique, qui vit en RAM (`memoryBuffer`).

Conséquence réelle : **l'historique cesse simplement d'être persisté**, et personne ne s'en aperçoit — la perte n'apparaît qu'au **reboot suivant**, sous la forme d'un historique tronqué à la dernière sauvegarde réussie. C'est pourquoi toute spec qui ajoute un champ au point persisté doit **chiffrer** l'occupation résultante (et non l'estimer), et pourquoi le critère d'acceptation « aucun WARNING *Partition history pleine* après 24 h » est **bloquant**. Le compteur cumulé `nospc_cnt` et l'epoch du dernier save réussi (`last_ok`) sont persistés en NVS `hist_meta` — ce sont eux qui permettent de constater a posteriori qu'on a franchi la ligne.

## Sauvegarde `saveToFile()` — garde MÉMOIRE (v2.43.0, bug-100 / [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md))

La garde d'espace ci-dessus protège d'une **partition pleine**. Celle-ci protège
d'une **famine mémoire**, qui est une panne **différente** et bien plus brutale :
sur ESP32, `fopen()` fait allouer à newlib un verrou récursif de **92 o** ; si ce
`malloc` échoue, `lock_init_generic` appelle **`abort()`** — il n'y a **aucun
code de retour à tester**. C'est le PANIC de production du **05/09/2026**,
`loopTask`, sur `open("/history.json", "r")`.

Depuis la v2.43.0, aucune ouverture de fichier de `history.cpp` n'est tentée sans
admission préalable. Recensement complet et règles :
[`acces-fichiers.md`](acces-fichiers.md).

### Deux admissions dans `saveToFile()`, et aucune n'est redondante

| # | Emplacement | Seuil | Ce qu'elle protège |
|---|---|---|---|
| 1 | **tête** de `saveToFile()` ([`history.cpp:532`](../../src/history.cpp:532)) | `kStaticFileMinAllocBytes` = **8 192** (classe A) | évite de **payer** la sérialisation en famine ; couvre la **non-atomicité** (le `remove()` précède l'`open("w")` : une garde sur l'ouverture échangerait l'`abort()` contre la **destruction** du fichier) ; précède la lecture d'`oldSize`, que `historySaveAdmit()` consomme |
| 2 | **après `measureJson()`** ([`history.cpp:591`](../../src/history.cpp:591)) | idem | garantit que la mesure qui **autorise le `fopen`** est **postérieure au pic d'allocation** |

> ⚠️ **La leçon à retenir de ce correctif.** La première version ne posait que la
> garde nº 1 — et elle était **insuffisante**. Entre l'admission en tête de
> fonction et le `fopen`, `saveToFile()` construit le `JsonDocument` et ses
> `String` : **elle dépense la marge qu'elle vient de valider.** L'ouverture
> s'exécutait donc sur une mesure **devenue fausse**, c'est-à-dire sur une
> **autorisation périmée** — exactement ce que l'asymétrie mesure/décision existe
> pour interdire. Règle générale :
> [acces-fichiers.md § Piège de placement nº 2](acces-fichiers.md#-piège-de-placement-nº-2--avant-louverture-ne-suffit-pas--il-faut-avant-le-pic-dallocation).

**Classe A et non B** : ce chemin est atteignable depuis `POST /history/import`
(→ `importData()` → `saveToFile()`) sur la tâche `async_tcp`, donc avec des pbuf
lwip en vol.

**Une fois admis**, les deux ouvertures (`open("r")` pour `oldSize`,
`open("w")` pour l'écriture) passent par le helper avec
`kFsGuardAlreadyAdmitted` : **une** mesure du tas pour la sauvegarde entière.

### Report, retentative, et jamais de silence

Sur refus : **rien n'est touché sur le filesystem**, `saveToFile()` retourne, et
la sauvegarde est **retentée au prochain cycle `SAVE_INTERVAL`, dans 5 min**.
C'est un report, pas une perte : l'historique vit en RAM (`memoryBuffer`), l'UI
continue de l'afficher.

Observabilité, dans l'ordre de fiabilité décroissante :

| Canal | Contenu | Durabilité |
|---|---|---|
| Compteur **`hdef`** | refus cumulés (sauvegardes + tripwires) dans le topic **retenu** `<base>/diagnostic` | **survit au reboot** |
| **WARN throttlé 1/h** | `Famine memoire : sauvegarde de l'historique sautee (maxBloc8=X o < 8192 o) — nouvelle tentative dans 5 min` — variante `Famine memoire apres serialisation : …` pour la garde nº 2 | bornée (`_persistBuffer` = 100 entrées FIFO) |

**Jamais `ERROR`, jamais `CRITICAL`** : un `ERROR` déclencherait le flush disque
synchrone du logger, donc un `fopen`, donc l'`abort()` qu'on prévient.

Si le refus a été servi par le **cache du refus** (aucune mesure prise), le
message le dit — `maxBloc8 non mesure (refus en cache) < 8192 o` — au lieu
d'afficher un chiffre périmé comme s'il datait de l'instant.

⚠️ **`hdef` et `nospc_cnt` ne se remplacent pas** : « partition pleine » et
« famine mémoire » sont deux pannes distinctes, avec deux libellés distincts.
Les confondre ferait chercher de l'espace disque pendant une pénurie de RAM.

### Tripwire de boot — classe B, seuil 5 120

Au démarrage, **« reporter » n'a pas de sens** : il n'y a pas de plus tard. Deux
sites de `begin()`/`loadFromFile()` basculent donc sur un **tripwire** au lieu
d'un report, sur le **7ᵉ barreau** `kInternalFopenMinAllocBytes` = **5 120**
(l'absence de contention TCP est prouvée par l'ordre de `setup()` :
`history.begin()` à `main.cpp:271` précède `mqttManager.begin()` 301 et
`webServer.begin()` 317) :

| Site | Sous le seuil |
|---|---|
| [`history.cpp:317`](../../src/history.cpp:317) — avant le test d'intégrité `/.fscheck` | `historyEnabled = false` pour ce boot, `writeHistoryCrashCount(0)` (sortie **propre**, le drapeau du disjoncteur est levé) |
| [`history.cpp:703`](../../src/history.cpp:703) — avant le chargement de `/history.json` | `historyEnabled = false` pour ce boot |

**Conséquence utilisateur** : entre 4 096 (auto-guérison) et 5 120, **l'appareil
démarre sans historique persistant mais en dosant**. C'est un gain de
disponibilité de la régulation, pas un confort — l'alternative était l'`abort()`,
donc un boot loop.

Le tripwire **incrémente `hdef`** : c'est la dégradation la plus sévère du lot,
elle doit être visible dans le canal **retenu**, qui survit au reboot. Son WARN,
lui, sort par le haut du `_persistBuffer` sous famine prolongée.

> ⚠️ **L'observabilité ne doit pas dépendre du sous-système qu'elle garde.** Sous
> le tripwire de `begin()`, le montage a **réussi** — ce n'est pas la partition
> qui manque, c'est la mémoire. `setPersistenceFs(&historyFs)` est donc appelé
> **avant** de sortir : on ne perd que l'historique, **pas le journal persistant
> du boot entier**. L'appel est lui-même gardé (classe A) : sous le seuil il
> échoue proprement.

#### `setPersistenceFs()` réconcilie le `.tmp` du journal — sur les DEUX chemins (`bug-099`, v2.48.1)

Depuis la v2.48.1, `setPersistenceFs()` commence par **réconcilier le `.tmp` de
rotation** de `/system.log` : promotion du survivant, ou effacement de
l'orphelin ([ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md),
détail dans [logger.md](logger.md#reprise-dune-rotation-interrompue-au-démarrage-bug-099-v2481)).
`history.cpp` étant **le seul appelant**, cela concerne ses **deux** chemins :

| Appel | Contexte | Ce qui s'y passe |
|---|---|---|
| [`history.cpp:332`](../../src/history.cpp:332) | chemin **tripwire** — atteint **alors que l'admission mémoire a déjà été refusée** (l. 317) | La réconciliation redemande une admission au **même** seuil (5 120) et se la voit refuser à son tour : **l'ABSTENTION est ici le comportement normal**, pas une anomalie. Aucun `remove`, aucun `rename`, aucune ligne de journal. |
| [`history.cpp:377`](../../src/history.cpp:377) | chemin **nominal**, après le test d'intégrité (et l'éventuel reformatage) | Réconciliation effective : `Promote`, `Discard` ou silence. |

**Ordre garanti** : dans les deux cas, la réconciliation a lieu **avant** la
première admission d'**écriture** d'historique — `historySaveAdmit()`
([`history.cpp:630`](../../src/history.cpp:630)) — donc la marge d'espace est
calculée **après** l'éventuelle libération des deux blocs du `.tmp`, jamais
avant. Sous le layout v5 (256 Ko) cet ordre ne change **aucune** décision
d'admission : c'est une propriété qu'on préserve, pas un gain qu'on mesure.

### `loadFromFile()` — l'`exists()` a été supprimé, pas gardé

`loadFromFile()` appelait `historyStore->exists()` **juste avant** d'ouvrir le
même fichier. Or `VFSImpl::exists()` **ne fait pas de `stat()`** : il construit un
`VFSFileImpl(this, fpath, "r")`, donc un `fopen` complet. L'appel était
**strictement redondant** tout en **doublant** l'exposition à l'`abort()` — il a
été supprimé.

⚠️ **Perte de distinction assumée** : « fichier absent » (cas nominal au premier
boot) et « fichier illisible » ne sont **plus discernables**, l'API `fs::`
n'offrant aucun `stat()` pour les départager sans réintroduire le site fermé. Le
libellé est rendu **explicitement non discriminant**, en `INFO` :

```
Aucun historique exploitable (fichier absent ou illisible)
```

### La mesure a lieu **sous `_mutex` tenu**, et c'est accepté

`saveToFile()` est appelée avec `_mutex` tenu depuis `consolidateData()` **et**
depuis `importData()` — ce n'est pas un cas de bord évitable. Un parcours TLSF
coûte ~10² µs contre 10¹-10² ms pour l'écriture LittleFS **déjà payée sous ce
même verrou** ; le coût réel (« interruptions coupées ») est **identique** avec
ou sans mutex ; et le verrou du tas est **terminal**, donc aucune inversion de
priorité n'est possible. Détail de l'argument :
[acces-fichiers.md](acces-fichiers.md#la-mesure-sous-mutex-tenu-est-acceptée-et-cest-un-constat-mesuré).

## Mesure lissée historisée (feature-095, v2.38.0)

`recordDataPoint()` enregistre, en plus du brut (`ph` = brut + offset, `orp` = brut), la mesure **lissée** pH/ORP (`sensors.getPhFiltered()` / `getOrpFiltered()`) — **les mêmes accesseurs que `/data`**, donc exactement la valeur sur laquelle la régulation a décidé à l'instant de l'enregistrement.

- **Aucun repli sur le brut** : filtre non amorcé, sonde muette ⇒ `NAN`. « Pas de lissée » doit rester **observable**, pas maquillé.
- **Même sort que le brut quand la mesure n'est pas représentative** : `measureValidity.measuresValid() == false` (filtration arrêtée, purge post-reprise) force `phFiltered = orpFiltered = NAN` dans le même bloc que `ph`/`orp`/`temperature` — une mesure non représentative lissée reste une mesure non représentative.
- **`DataPoint` porte des initialiseurs `= NAN`** sur ces deux champs. Non optionnel : la structure est construite **sans initialisation explicite** dans `/history/import` et dans `avgPoint` de `consolidateData()` ; sans initialiseur on persisterait de la mémoire non initialisée, affichée ensuite comme une mesure réelle.
- **`sizeof(DataPoint)` est verrouillé par `static_assert` à 28 o** (20 auparavant). Ce n'est pas cosmétique : c'est ce chiffre qui dimensionne l'allocation **contiguë** de `snapshotFiltered()` pour `/get-history` — 339 points × 28 = **9 492 o d'un seul bloc**, demandés en contexte `async_tcp` sur un appareil dont le plus gros bloc libre tourne autour de 28-30 Ko (bug-080, famine 8 bits). Tout champ ajouté à `DataPoint` doit faire recompter ce budget, en compilation plutôt qu'en production.

### La lissée n'est historisée QUE sur les points RAW

`saveToFile()` ne sérialise `pf` / `of` **que si `point.granularity == RAW`** (5 min, ~8 h de profondeur). Au-delà — moyennes **horaires** et **journalières** — la lissée est **absente du fichier** et vaut `NaN` en mémoire, donc `null` sur `/get-history`.

Deux raisons, dans cet ordre :

1. **Budget de partition, mesuré et non supposé.** Historiser la lissée à **toutes** les granularités coûtait **+7 119 o** (339 points × 21 o) et portait l'occupation à **16 blocs sur 16** : `historySaveAdmit()` aurait alors **refusé la sauvegarde**, avec le mode de défaillance silencieux décrit [ci-dessus](#le-mode-de-défaillance-du-budget-est-le-silence-pas-la-corruption-). En RAW seul : **12/16 blocs**, résiduel **8 102 o**, et la profondeur horaire (7 j) reste intacte — aucune réduction de `kMaxHourlyDataPoints` n'a été nécessaire.
2. **La série aurait été redondante.** La lissée est un signal **passe-bas** : sa moyenne horaire **coïncide** avec celle du brut. On aurait payé ~5,1 Ko pour une seconde série indiscernable de la première.

Décision et alternatives écartées : [ADR-0064](../adr/0064-la-courbe-suit-le-nombre-affiche.md).

### Règle d'agrégation : la lissée n'est JAMAIS moyennée

| Chemin | Traitement de `phFiltered` / `orpFiltered` |
|---|---|
| `consolidateData()` RAW → HOURLY | `avgPoint.phFiltered = avgPoint.orpFiltered = NAN`, **explicitement** — jamais accumulées, jamais divisées |
| `consolidateData()` HOURLY → DAILY | idem |
| `dedupAggregatedPoints()` | **volontairement non fusionnées** (pas d'appel à `historyMergeMeanFinite`) : ce chemin ne traite que des agrégats, qui ne portent jamais de lissée. Ne rien écrire garantit qu'aucune lissée n'est **inventée** |
| `loadFromFile()` | `point["pf"] \| NAN` — tolérant à l'absence, aucune migration |

> ⚠️ **Le `validCount` partagé est intact.** Les compteurs d'échantillons valides de la consolidation restent ceux du brut/de la température : la lissée ne les incrémente ni ne les décrémente. C'est un invariant de *characterization* (les moyennes horaires et journalières produisent **exactement** les mêmes valeurs qu'avant la v2.38.0) — ne pas le « corriger ».

### Point persisté vs point exporté : deux formats distincts (feature-095)

La page confondait jusqu'ici ces deux formats. Ils ne sont **pas** interchangeables, et l'asymétrie est **voulue** :

| | `/history.json` (persisté) | `/get-history` (fil HTTP) |
|---|---|---|
| Nommage | clés **courtes** : `t p o T pf of f d g` | clés **longues** : `timestamp ph orp temperature phFiltered orpFiltered filtration dosing granularity` |
| Clé de dosage | `d` — **entier** depuis la v2.47.0 (`"d":3`) | `dosing` — **entier**, masque servi **verbatim** |
| Valeur absente | **clé omise** | clé **présente**, valeur **`null`** |
| Lissée sur un agrégat | jamais écrite | `null` |
| Producteur | `saveToFile()` (`history.cpp`) | `formatHistoryPointJson()` ([`history_export_chunk.h`](../../src/history_export_chunk.h)) |
| Consommateur de relecture | `loadFromFile()` | `POST /history/import` |

- **Dans le fichier, chaque octet comptait** : la partition faisait 64 Ko et son dépassement est silencieux. D'où les clés de 2 caractères et l'omission pure et simple d'une clé non finie. *(Depuis le 05/09 la partition fait 256 Ko — le format reste tel quel, mais ce n'est plus une contrainte qui doit dicter un arbitrage : voir [Le budget a changé d'ordre de grandeur](#le-budget-a-changé-dordre-de-grandeur-le-2026-09-05).)*
- **Sur le fil, la stabilité du contrat prime** : `ph` / `orp` / `temperature` sont **toujours** présents et disent `null` quand la mesure manque (contrat existant depuis feature-061). `phFiltered` / `orpFiltered` suivent **la même règle** — un client n'a jamais à distinguer « clé absente » de « valeur nulle ».
- ⚠️ **`POST /history/import` lit les clés LONGUES**, celles de l'export — pas les clés courtes du fichier persisté. Ne pas confondre les deux formats en écrivant un outil de restauration.

### Fraîcheur de la mesure lissée : nuance connue, aucun changement

`sensors.getPhFiltered()` **n'a pas de garde d'âge** : si le filtre est amorcé mais que la dernière mesure valide date, l'accesseur rend la dernière valeur lissée connue — et elle serait historisée comme une mesure fraîche.

**La revue a conclu de ne rien changer**, délibérément :

- c'est **exactement** la valeur que `/data`, le WebSocket, la page pH et la régulation utilisent **au même instant** — l'historique dit donc la vérité sur ce que l'appareil a fait ;
- ajouter une garde d'âge **seulement** dans l'historique créerait une **divergence** entre la courbe et le nombre affiché : précisément le défaut que la feature-095 corrige.

Le garde-fou réel est ailleurs et il est déjà là : `measureValidity` (filtration arrêtée / purge) et l'alarme de filtre bloqué (`phFilterAlarm` / `orpFilterAlarm`, > 5 min) annulent le point ou alertent l'utilisateur. La nuance est documentée ici pour qu'elle ne soit pas re-découverte comme un bug.

### Budget de partition — avant / après (feature-095)

Chiffres **mesurés**, pas estimés (bloc LittleFS = 4 096 o) :

| | Avant (v2.37.0) | Après (v2.38.0) | Δ |
|---|---|---|---|
| Point `RAW` persisté (pire cas) | 68 o | **90 o** | +21 o mesurés par point |
| `/history.json` pire cas (339 points) | 23 497 o | **25 513 o** | **+2 016 o** (96 points RAW seulement) |
| Blocs occupés par `/history.json` | 6 | **7** | +1 |
| Occupation de la partition en régime établi | 11/16 | **12/16** | +1 bloc |
| Résiduel disponible | — | **8 102 o** | — |

**Hypothèse écartée, chiffrée** : lissée à toutes les granularités ⇒ +7 119 o, `/history.json` ~30,6 Ko, **16/16 blocs** ⇒ `historySaveAdmit()` refuse, historique **plus jamais persisté**. Ce n'est pas une marge « mince », c'est un franchissement.

### `kHistoryPointJsonMaxBytes` : 128 → 192 o

La constante ([`constants.h`](../../src/constants.h)) borne le **carry** (`LogLineCarry<N>`) de la réponse chunked `/get-history` : un point entier — ou l'en-tête, ou le pied — doit y tenir, **jamais** de JSON partiel émis. Les deux clés longues faisaient sauter l'ancienne valeur de 128. Décompte du pire cas **réaliste** :

| Poste | Octets |
|---|---|
| Clés + ponctuation fixes (virgule de séparation comprise) | 114 |
| `timestamp` (10 chiffres) | 10 |
| `ph` + `temperature` (`%.1f`) | 6 + 6 |
| `phFiltered` (`%.2f`) | 7 |
| `orp` + `orpFiltered` (entiers) | 5 + 5 |
| `"false"` × 2 + granularité | 10 + 3 |
| **Total** | **161** (+ NUL) |

192 laisse donc **≥ 31 o de marge**. Coût du relèvement : **+64 o** d'état par requête `/get-history` (un seul carry vivant à la fois par requête) ; `/download-logs` n'est **pas** concerné (`kLogExportLineMaxBytes` lui est propre).

> **Pire cas théorique, à connaître** : si les cinq conversions saturaient leurs tampons de 15 caractères, le point atteindrait **213 o** et dépasserait 192. Inatteignable avec des mesures plausibles (un pH ne fait pas 15 chiffres) — on documente la borne au lieu de surdimensionner le tampon, **parce que le chemin de saut n'est plus muet** (ci-dessous). Toute nouvelle clé au format impose de **refaire ce décompte**.

### Points omis de `/get-history` : la perte n'est plus silencieuse

Quand `formatHistoryPointJson()` retourne 0 (le point ne tient pas dans le carry), le point est **sauté**. Jusqu'en v2.37.0, il l'était **sans un mot** : la réponse restait un JSON parfaitement valide, et le trou de courbe qui en résultait était **indiscernable d'une panne de capteur**. C'est un mode de panne de la même famille que le budget de partition — muet.

Depuis la v2.38.0 :

- les omissions sont **comptées** dans l'état de réponse (`HistoryDownloadState::skipped`) — jamais journalisées à l'unité (un export en sauterait des centaines) ;
- **un seul WARNING** est émis **à la clôture** de la réponse, throttlé par `kHistorySkipWarnThrottleMs` = **1 h** toutes réponses confondues (le dashboard rappelle la route à chaque rafraîchissement). Message **actionnable** : combien de points, pourquoi, et quoi faire —

  `get-history : N point(s) omis de la réponse — JSON du point au-dessus de kHistoryPointJsonMaxBytes (192 o) ; courbes trouées, relever la constante`

- construit par `snprintf` sur un tampon de pile : aucune `String` fabriquée dans le callback chunké.

La trace de clôture DEBUG existante (`get-history clos: emis=…`) est inchangée.

## Mode « compléter » — `appendData()` + `trimToCaps()` (bug-101, v2.48.0)

`POST /history/import` a désormais **deux** modes (champ `mode`, défaut `replace` —
voir [API.md](../API.md#post-historyimport--write)) :

| Mode | Fonction | Effet sur `memoryBuffer` |
|---|---|---|
| `replace` | `importData()` | `memoryBuffer = dataPoints;` — **substitution totale**, puis `dedupAggregatedPoints()` |
| `append` | `appendData()` | **fusion en place** créneau par créneau, puis `std::sort`, puis `trimToCaps()` |

C'est `append` qui rend une restauration de **taille quelconque** possible sans toucher
au budget mémoire : le client découpe, et chaque lot s'ajoute au précédent au lieu de
l'écraser. Décision et alternatives écartées (streaming firmware, format compact) :
[ADR-0075](../adr/0075-decoupage-client-contre-streaming-firmware.md).

### Règle de collision : le point entrant ÉCRASE

À **`(timestamp, granularité)` identique**, le point entrant remplace le point vivant
(`historyImportSameSlot()`, [`src/history_import_logic.h`](../../src/history_import_logic.h)).
Deux précisions qui ne sont pas des détails :

1. **RAW comprise.** ⚠️ **Ne pas espérer que `dedupAggregatedPoints()` couvre ce
   chemin** : il commence par `if (p.granularity != RAW)`
   ([`src/history.cpp:1248`](../../src/history.cpp)) et ignore donc les points RAW —
   dont un export porte jusqu'à `kMaxRawDataPoints` = **96**. Sans la règle appliquée
   *dans* `appendData()`, un lot RAW réenvoyé (reprise après coupure) **doublait en
   silence**.
2. **TOUTES les occurrences du créneau, pas seulement la première.** Un buffer chargé
   depuis un fichier écrit par une version **≤ 2.18.0** peut porter des doublons
   d'agrégats. N'en écraser qu'un laisserait l'autre **périmé**, que
   `dedupAggregatedPoints()` **moyennerait** ensuite avec le neuf : l'**idempotence**
   tomberait. Écraser les deux la conserve, `historyMergeMeanFinite(x, x) == x`.

**Pourquoi l'écrasement et non la moyenne** — c'est le cœur de la décision. Une moyenne
équipondérée fabriquerait une valeur **qui n'a jamais été mesurée** ; sur une reprise
après coupure, ce qu'on veut est la valeur de la **sauvegarde, telle quelle**. Et
l'écrasement donne l'**idempotence exacte** : réenvoyer *n* fois le même lot donne le
même historique qu'une fois. C'est cette propriété — et rien d'autre — qui rend le
bouton « Reprendre l'import » sûr.

> ⚠️ **`dedupAggregatedPoints()` n'est PAS appelé par `appendData()`, et c'est un
> choix.** Il moyennerait précisément les agrégats que l'écrasement vient de remplacer.
> La fusion en place garantit déjà l'unicité de `(timestamp, granularité)` pour tout ce
> que le lot a touché ; le reste relève de `consolidateData()`.

### Fusion en place : zéro tampon supplémentaire

La recherche est **linéaire dans `memoryBuffer`**, sans structure auxiliaire — ni
`std::map`, ni vecteur trié. Coût : `|pts| × |buffer|`, soit au pire ≈ 45 × 339
≈ **15 000 comparaisons de deux scalaires** par lot (le plafond de corps borne `|pts|`).
Compromis **délibérément inverse** de l'usuel : sur ce chemin, la ressource rare est la
**mémoire**, pas le CPU.

**Le seul contigu du chemin reste le vecteur existant** :
`(kMaxRawDataPoints + kMaxHourlyDataPoints + kMaxDailyDataPoints) × sizeof(DataPoint)`
= 339 × 28 = **9 492 octets**, verrouillé par `static_assert` (et `sizeof(DataPoint) == 28`
l'est aussi). C'est le chiffre à confronter au relevé `max_alloc_8bit` de
`GET /get-system-info` avant/après un import : l'invariant est que **l'interne ne bouge
pas de plus de 9 492 o** et que la **PSRAM absorbe** les tampons de corps
([ADR-0061](../adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)).

### `trimToCaps()` — bornage du mode « ajouter »

Ramène `memoryBuffer` sous les plafonds de [`constants.h`](../../src/constants.h),
**granularité par granularité**, en évinçant les **plus anciens**. Le plan d'éviction
est calculé par la fonction pure `historyImportTrimPlan(raw, hourly, daily)`, puis
appliqué en **un seul** passage `erase`/`remove_if` (O(n) au total).

- **Pré-requis** : mutex **déjà pris** par l'appelant, et buffer **trié par timestamp**
  — l'ordre du vecteur *est* l'ordre chronologique, donc « les premiers rencontrés » =
  « les plus anciens ».
- **Appelé uniquement par `appendData()`, et ce n'est pas un oubli.** Les deux sites de
  bornage existants sont **inchangés** : `recordDataPoint()` pour RAW, le pas 4 de
  `consolidateData()` pour HOURLY/DAILY. Le mode `replace` reste borné par le plafond
  de corps de la route (`kHistoryImportMaxBodyBytes`) et n'a donc pas besoin de ce
  filet. Seul l'ajout par lots pouvait franchir un plafond sans passer par aucun des
  deux chemins existants.
- **Ce que `trimToCaps()` retire n'est pas ce que le lot apportait.** Le compteur rendu
  au client (`dropped`) compte les **évincés de l'historique**, jamais les entrants
  refusés — cf. l'avertissement de [API.md](../API.md#post-historyimport--write).

### Atomicité et journal

`appendData()` prend le mutex historique **une seule fois** (idiome d'`importData()`) :
la séquence **fusion → tri → bornage → sauvegarde** est atomique, un lot ne peut pas
s'entrelacer avec le `recordDataPoint()` de la boucle. Comme `importData()`, il remet
`legacyHistoryPending = false` / `legacyMaxTimestamp = 0` — sans quoi la migration
d'historique hérité se réveillerait au prochain `consolidateData()` et **réécrirait
par-dessus** le lot.

Une ligne `INFO` par lot, émise **après** la libération du mutex (un `WARNING` ou pire
flusherait le journal en synchrone, donc un `fopen`, **sous** le mutex historique) :

```
Historique : lot ajouté (N nouveaux, M écrasés, D évincés, total T)
```

### 📋 Défauts ouverts sur ce chemin (consignés, non traités)

- **Une réécriture complète de `history.json` PAR LOT.** `appendData()` appelle
  `saveToFile()` à chaque appel : restaurer 339 points en 8 lots produit **8
  réécritures intégrales** du fichier, depuis la tâche `async_tcp` — latence perçue et
  usure de la partition. Un `flush` explicite en fin de séquence supposerait une notion
  de session côté appareil, qui n'existe pas (chaque lot est autonome, c'est ce qui rend
  la reprise possible). **Préexistant au mode `append`** (`importData()` faisait déjà
  une sauvegarde par appel), **atténué** par le filtre 503 de la garde de corps qui
  refuse les lots en famine.
- **`saveToFile()` est `void`.** Elle peut sauter la sauvegarde (admission d'espace ou
  de mémoire refusée) **sans que `appendData()` le sache** : la route répond alors
  `200` avec un `total` juste **en RAM**, mais le fichier n'a pas été écrit. Le point
  est visible dans le journal et par `historyFopenDeferralCount()` / `nospc_cnt`, jamais
  dans la réponse HTTP. **Préexistant.**

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
// bug-101 (v2.48.0) : mode « ajouter » de POST /history/import — fusion EN PLACE
// d'un lot dans l'historique existant, puis bornage par trimToCaps().
// outTotal   = points DÉTENUS après le lot ; outDropped = points ÉVINCÉS par
// trimToCaps() pendant cet appel (les plus anciens), JAMAIS « entrants refusés ».
// Les deux sorties sont écrites AVANT tout retour d'échec (jamais non initialisées).
// false = historique désactivé, lot vide, ou mutex non pris → RIEN n'a été modifié.
bool appendData(const std::vector<DataPoint>& pts, size_t* outTotal, size_t* outDropped);
bool clearHistory();   // false si le mutex n'a pas pu être pris (historique occupé) — v2.11.1
```

Fonction libre (`history.h`, v2.43.0, bug-100) :

```cpp
// Refus d'ouverture de fichier pour FAMINE MÉMOIRE, cumulés depuis le boot :
// sauvegardes sautées + tripwires de boot. Publié en `hdef` dans le payload
// RETENU <base>/diagnostic. ⚠️ DISTINCT de nospc_cnt (« partition pleine ») :
// deux pannes différentes, deux compteurs.
uint32_t historyFopenDeferralCount();
```

## Export chunked `/get-history` (v2.14.0, refondu v2.18.2)

Jusqu'en v2.13.x, `/get-history` copiait **tout** le buffer (`getLastHours`/`getAllData`) puis concaténait la réponse dans une `String` contiguë pré-réservée (~40 Ko épinglés pendant tout le transfert à chaque ouverture du dashboard) — **générateur de fond n°3 (G3)** de la fragmentation heap identifié par l'investigation churn-heap du 24/07. Depuis la v2.14.0, la route est **chunked** ([ADR-0032 §Extension 2026-07-30](../adr/0032-reponses-http-bornees.md#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140), patron `/download-logs`) ; depuis la v2.18.2, elle streame depuis un **instantané figé** :

- **`snapshotFiltered(out, cutoff, since)`** (v2.18.2) : copie de l'instantané **filtré** du buffer en **UNE** prise de mutex à l'ouverture de la requête — timeout dédié court `kHistorySnapshotTimeoutMs = 100 ms` (le timeout standard 2 s bloquerait la tâche `async_tcp`). Refus (rare) → la route répond **`503` `{"error":"historique occupé, réessayez"}`**. Coût borné : ≤ `kMaxRawDataPoints + kMaxHourlyDataPoints + kMaxDailyDataPoints` points — depuis la v2.38.0, 339 × 28 o ≈ **9,5 Ko d'un seul bloc contigu** (`reserve` dimensionné sur la taille du buffer : aucune réallocation possible), libérés en fin de réponse. Le callback chunked ne prend ensuite **aucun mutex** : aucun timeout, aucun glissement d'index possible (l'instantané est immuable → plus de doublon/saut best-effort).
- **Filtres `range`/`since` appliqués à la copie** (fonction pure `historyRangeCutoff`, sémantique exacte de l'ancien `getLastHours`) ; formatage d'un point par la fonction pure `formatHistoryPointJson` ([`src/history_export_chunk.h`](../../src/history_export_chunk.h)). Le format était **octet pour octet identique** à l'ancienne concaténation jusqu'à la v2.37.0 ; la **v2.38.0 y insère `phFiltered` et `orpFiltered`** (ajout **additif**, tout le reste inchangé — y compris la clé `count` émise deux fois, voir [API.md](../API.md#get-get-history--write)). Les deux paramètres ont une valeur par défaut `NAN` : un appelant qui n'a pas de lissée produit `null`.
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
| Importer un lot de points | `POST /history/import` (JSON, `mode` `replace`/`append`, ≤ **8 192 o** de corps) | WRITE |
| Purger l'historique | `POST /history/clear` | CRITICAL |

Voir [`web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303). Colonnes CSV de l'export de l'interface (v2.47.0) : `datetime, ph, orp, temperature, filtration, dosing, dosing_mask, granularity` — `dosing` reste le **booléen d'héritage** `0`/`1`, `dosing_mask` porte le [masque de voies](#masque-de-voies-de-dosage-bug-098-v2470). ⚠️ **Le nom `dosing_mask` est neuf à dessein** : réutiliser `dosing` aurait rendu un `1` **indécidable** — pH, ou voie inconnue ? — les exports ≤ 2.46 écrivant déjà `1` pour dire « il y a eu dosage ». À l'import, `dosing_mask` a **priorité** sur `dosing` quand les deux colonnes sont présentes.

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

La partition `history` (**256 KB, offset `0x7B0000`**, layout v5) est **séparée** de `spiffs` pour que les updates OTA du filesystem (qui réécrivent `spiffs`) ne touchent pas à l'historique. Voir [ADR-0009](../adr/0009-partition-coredump.md) et [ADR-0068](../adr/0068-partitions-layout-v5-flash-8-mo.md).

### Le budget a changé d'ordre de grandeur le 2026-09-05

La partition est passée de **64 à 256 KB** (16 → **64 blocs** LittleFS de 4 096 o) avec le layout v5. Ce n'est pas un confort marginal : le budget de cette partition était une **contrainte active**, et sa violation était **silencieuse** (voir [Le mode de défaillance du budget est le SILENCE](#le-mode-de-défaillance-du-budget-est-le-silence-pas-la-corruption-)).

| | v4 (jusqu'au 05/09) | v5 |
|---|---|---|
| Taille de partition | 64 KB | **256 KB** |
| Blocs LittleFS (4 096 o) | 16 | **64** |
| Occupation en régime établi (feature-095) | 12/16 | **12/64** |
| Pire cas budgété (2 méta + 6 history + 3 log + 2 tmp) | 13/16 — **marge 3 blocs** | 13/64 — **marge 51 blocs** |

**Conséquences directes sur les specs ouvertes :**

- **`bug-099` (`.tmp` de rotation jamais nettoyé au boot) est dissous.** La marge de croissance tombait à **90 octets** dans le pire cas (`.tmp` orphelin + tampon de log plein) ; elle se compte maintenant en dizaines de Ko. Le nettoyage du `.tmp` reste souhaitable **par hygiène**, il n'est plus **structurel**.
- **`bug-098` (drapeau de dosage ORP perdu à l'écriture) devient trivial à financer.** L'octet supplémentaire par point — **339 o au total** — était un arbitrage de budget ; ce n'en est plus un.

⚠️ **Ce qui ne change pas** : la garde d'admission `historySaveAdmit()` ([ADR-0033](../adr/0033-budget-espace-partition-history.md)) reste en place et reste obligatoire pour toute nouvelle écriture sur cette partition. Un budget confortable n'est pas un budget infini, et le mode de panne — le silence — est inchangé.

**Budget nominal des fichiers :**

| Fichier | Taille max |
|---------|-----------|
| `history.json` (données brutes + horaires + journalières) | ~30 KB |
| `system.log` (logs firmware persistés) | 16 KB |
| Fichier temporaire de rotation logs | 12 KB |
| **Total** | **~58 KB ≪ 256 KB** |

### Protection au redimensionnement de partition

`HistoryManager::begin()` compare la taille réelle de la partition `history` (lue via l'API IDF) avec la taille stockée en NVS sous la clé `hist_meta/part_sz`. Si les tailles diffèrent (ou si la clé est absente = premier boot), la partition est effacée via `esp_partition_erase_range` avant montage LittleFS.

Cette protection évite un crash `IntegerDivideByZero` dans `lfs_alloc` lorsque LittleFS tente de monter un filesystem dont la géométrie ne correspond plus à la taille physique de la partition — situation qui se produit lors d'un redimensionnement flash (ex. migration ADR-0007 → ADR-0009).

> ⚠️ Le premier boot après un changement de table de partitions **efface l'historique**. C'est intentionnel et documenté dans [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md).
>
> **Cas réel, 2026-09-05** : le passage au layout v5 a fait passer la partition de 64 à 256 KB — cette garde s'est donc déclenchée au premier boot et a effacé l'historique, comme prévu. C'est pour cette raison que la sauvegarde de `/history.json` **avant le flash, appareil encore en ligne**, est une étape de la procédure de migration et non une précaution optionnelle. ⚠️ La restauration se heurte ensuite à la limite de **16 384 octets de corps** de `POST /history/import` (≈145 points par lot) — voir [API.md](../API.md#post-historyimport--write).

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
| Corrompu | Erase de la partition + remount + reformatage LittleFS. Chiffre mesuré **avec la partition de 64 KB** : ~100 ms d'erase, 300-500 ms au total. ⚠️ **Non remesuré depuis le passage à 256 KB** (layout v5) : l'erase étant proportionnel à la taille, attendre un ordre de grandeur **~4×** supérieur. |

> Cette validation s'exécute **après** le test de redimensionnement de partition décrit ci-dessus. Les deux mécanismes sont indépendants : le premier détecte un changement de géométrie matérielle, le second détecte une corruption logique du filesystem monté avec succès.

> ⚠️ **Depuis la v2.43.0, cette validation elle-même est gardée** : les deux
> ouvertures de `/.fscheck` sont précédées d'une admission classe B
> ([`history.cpp:316`](../../src/history.cpp:316), seuil 5 120). Sous le seuil,
> le test n'est **pas** exécuté et `historyEnabled` passe à `false` pour ce boot
> — on ne conclut **pas** à une corruption faute de mémoire, et on ne reformate
> donc **pas** une partition saine. Cf. [Tripwire de boot](#tripwire-de-boot--classe-b-seuil-5-120).

## Cas limites

- **Partition pleine** : écriture impossible → log ERROR, `historyEnabled` peut être basculé à `false` manuellement depuis l'UI Avancé.
- **Famine mémoire en fonctionnement** (v2.43.0) : sauvegarde **reportée** de 5 min, ancien fichier intact, `hdef` incrémenté, WARN throttlé 1/h. Jamais d'`abort()`.
- **Famine mémoire au boot** (v2.43.0) : `historyEnabled = false` pour ce boot — l'appareil **démarre sans historique persistant mais en dosant**. Le journal persistant, lui, reste armé.
- **Migration d'une ancienne version** : `migrateLegacyHistory()` lit les fichiers au format précédent si `legacyHistoryPending = true`.
- **Boot sans heure** : les points sont enregistrés avec un timestamp uptime, corrigés à la synchro NTP.
- **Import d'un CSV malformé** : ligne rejetée individuellement, le reste est importé.

## Fichiers liés

- [`src/history.h`](../../src/history.h), [`src/history.cpp`](../../src/history.cpp)
- [`src/constants.h:49`](../../src/constants.h:49) — limites (`kMaxHourlyDataPoints = 168`)
- [`partitions.csv`](../../partitions.csv)
- [`src/history_logic.h`](../../src/history_logic.h), [`src/history_logic.cpp`](../../src/history_logic.cpp) — math d'agrégation pure
- [`src/history_export_chunk.h`](../../src/history_export_chunk.h) — logique pure de l'export chunked `/get-history` (v2.14.0) ; carry partagé avec [`src/logs_export_chunk.h`](../../src/logs_export_chunk.h)
- [ADR-0064](../adr/0064-la-courbe-suit-le-nombre-affiche.md) — la mesure lissée est la référence des courbes 5 min ; lissée non historisée sur les agrégats (feature-095)
- [`src/fs_guard.h`](../../src/fs_guard.h), [`src/fopen_guard_logic.h`](../../src/fopen_guard_logic.h) — point d'admission unique des `fopen` internes (v2.43.0)
- [acces-fichiers.md](acces-fichiers.md) — recensement des 14 sites, classes de contention, pièges de placement
- [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) — garde mémoire avant tout `fopen` interne, classes A/B, tripwire de boot
- [ADR-0033](../adr/0033-budget-espace-partition-history.md) — budget d'espace de la partition `history` (garde d'admission)
- [ADR-0009](../adr/0009-partition-coredump.md) — table de partitions courante
- [ADR-0007](../adr/0007-table-partitions-custom.md) — supersedé
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — logique métier pure / Humble Object
