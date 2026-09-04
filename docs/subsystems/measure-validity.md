# Subsystem — `measure_validity`

- **Fichiers** : [`src/measure_validity.h`](../../src/measure_validity.h), [`src/measure_validity.cpp`](../../src/measure_validity.cpp) (coquille impure) + [`src/measure_validity_logic.h`](../../src/measure_validity_logic.h), [`src/measure_validity_logic.cpp`](../../src/measure_validity_logic.cpp) (machine d'état pure) + [`src/purge_stability_logic.h`](../../src/purge_stability_logic.h), [`src/purge_stability_logic.cpp`](../../src/purge_stability_logic.cpp) (détecteur de stabilité pur)
- **Singleton** : `extern MeasureValidityManager measureValidity;`
- **Introduit par** : feature-061 (v2.13.0) ; étendu à la température en itération 2 (v2.13.0) ; porte d'alimentation des filtres en v2.20.3 ; **purge adaptative par la feature-091 (v2.34.0)** — [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)

## Rôle

Décide si les mesures **pH/ORP/température sont représentatives de l'eau du bassin**. Les sondes sont montées dans le circuit de filtration : filtration arrêtée = eau stagnante autour des sondes → la valeur dérive localement et ne reflète plus le bassin. À la reprise de la circulation, une **purge** est appliquée avant de redéclarer les mesures valides : l'eau stagnante de la chambre de mesure doit être chassée, **puis le transitoire de rinçage doit s'éteindre**.

Depuis la v2.34.0, la purge **n'est plus une minuterie** (`stabilizationDelayMin`, réglage utilisateur retiré) : elle se termine quand les **bruts pH et ORP sont stables**, encadrée par un **plancher de 10 min** et un **plafond de 90 min**. Motif : le rinçage du matin dessine un « V » de ~45-50 min (relevés des 02/09 et 04/09/2026 : pH brut 6,78 → 5,97 → 7,3) et la minuterie fixe, même portée à 15 min, ouvrait la porte **au fond du V** — d'où trois alertes pH à tort et une intégrale pH remise à zéro chaque matin.

Le verdict est consommé par :
- **`history.recordDataPoint()`** — point 5 min : pH/ORP/température forcés à `NaN` si invalide ; voir [history.md](history.md) ;
- **WS `sensor_data` + `GET /data`** — champs `water_present` / `measures_valid` / `measures_stab_elapsed_s` / `measures_purge_last_duration_s` / `measures_purge_last_end` / `measures_purge_count` (parité obligatoire) ;
- **`pump_controller`** — `measuresValid()` gate l'intégrale, le feedforward et les alertes système pH/ORP ; `isCirculationSettled()` gate la branche **Programmée** (C4) ;
- **`sensor_filter`** — porte d'alimentation des filtres pH/ORP (`setContext`), et depuis la v2.34.0 `ready()` est **faux porte fermée** (C6) — voir [sensors.md](sensors.md#porte-dalimentation-du-filtre-v2203).

**La chaîne de mesure interne ne change PAS** : les sondes continuent d'être lues et publiées (champs live `ph`/`orp`/`phRaw`/…). MQTT : seul ajout, l'alerte `measures_purge_timeout` (voir [MQTT.md](../MQTT.md)). Mode Auto de la filtration : inchangé (lit `sensors.getTemperature()` en direct).

## Architecture (Humble Object, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md))

| Module | Rôle | Testé en natif |
|---|---|---|
| `purge_stability_logic` (pur) | Médianes par minute, fenêtre glissante, verdict `stable` / `unstable` | `test/test_native_purge_stability/` (25 tests : rejeux 02/09 et 04/09, dérives lentes, ORP en pente, démonstration W = 8 à tort) |
| `measure_validity_logic` (pur) | Machine d'état porte/purge (`measureValidityTick`), circulation établie (C4), décision de dispense après coupure courte (C3, `evaluateOutageContinuity`) | `test/test_native_measure_validity/` |
| `measure_validity` (coquille) | Collecte : `resolveWaterPresence()`, `millis()`, bruts EZO, `esp_reset_reason()`, mémoire RTC ; journal ; alerte MQTT ; cache sous spinlock | — |

### Détecteur de stabilité (`purge_stability_logic`)

Par voie (pH brut, ORP brut — **sans** offset pH, [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) :

1. les lectures EZO (cadence `kPhOrpSensorIntervalMs` = 5 s) sont accumulées dans un tampon de minute ; une « minute » est définie en **nombre de lectures valides** (`kPurgeSamplesPerMin` = 12, NaN ignorés), pas en temps mural ;
2. à chaque minute pleine, la **médiane** est poussée dans un anneau de `kPurgeWindowMin` = 10 médianes ;
3. **`stable`** = anneau plein **ET** étendue (max − min des 10 médianes) ≤ `kPurgeStablePhRange` 0,15 pH **ET** ≤ `kPurgeStableOrpRange` 15 mV ;
4. **`unstable`** (refermeture C1) = étendue > `kPurgeRecloseRangePh` 0,30 pH ou > `kPurgeRecloseRangeOrp` 30 mV (= 2 × seuil, verrouillé par `static_assert`).

Agrégation des deux voies (`purgeStabilityEvaluate`) : une voie **jamais lue** est ignorée ; une voie lue mais dont l'anneau n'est **pas plein** bloque l'ouverture sans provoquer de refermeture ; aucune voie lue ⇒ ni stable ni instable (attente du plafond). Le verdict porte aussi `phRange` / `orpRange` pour le journal.

**Pourquoi W = 10** : le rinçage est un V (descente ≈ −0,047 pH/min, remontée ≈ +0,043 pH/min le 04/09). Le fond du V occupe une demi-fenêtre de chaque côté, donc la pente détectable vaut **seuil / ((W − 1) / 2)** : 0,10 pH/min à W = 4 (aveugle au V, ouverture à t+7 à pH 5,6), 0,043 à W = 8 (ouverture à t+13 à pH 5,71, 5 millièmes sous le seuil, puis refermeture à t+17), **0,033 à W = 10** (ouverture sur le plateau, pH 6,88, à t+47 sans bruit / t+46-62 avec bruit). 10 est la capacité de l'anneau (`kPurgeStabilityMaxWindowMin`). Détail : [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md).

### Machine d'état (`measureValidityTick`, une transition par tick, priorité décroissante)

| # | Condition | Effet | Événement |
|---|---|---|---|
| 1 | Eau absente | porte **fermée**, pas de purge ; au front descendant : `recloseArmed`, `waivedCycle`, `repurgeCount` remis à zéro | `kWaterDrop` |
| 2 | Front montant d'eau | purge démarre (`purgeStartMs = waterRiseMs = now`, `recloseArmed = true`). Si une **dispense** est présentée (`purgeWaived`, C3) : porte **ouverte** immédiatement, motif `kWaived`, durée 0, `waivedCycle = true` | `kPurgeStarted` |
| 3 | Purge en cours | **ouverture** si `écoulé ≥ kPurgeFloorMs` (10 min) **et** `stable` → motif `kStability` (ou `kRepurge` si `repurgeCount > 0`) ; sinon **ouverture** si `écoulé ≥ kPurgeCapMs` (90 min) → motif `kCap`, **et `recloseArmed = false`** (C2 : plus de refermeture jusqu'au prochain front d'eau) | `kOpened` |
| 4 | Porte ouverte, `recloseArmed`, `unstable` | **refermeture** (C1) : nouvelle purge (`purgeStartMs = now`), `repurgeCount++` (saturé 255), `valid = false` | `kReclosed` |

Toutes les soustractions de temps sont non signées (repliement de `millis()` sûr). La dispense n'est honorée qu'au front montant du tick où elle est présentée : la coquille ne la présente qu'au **premier tick après le boot**.

**Important** : le plafond **ouvre** la porte (fail-safe côté disponibilité de la régulation, les gardes pH/ORP habituelles restent actives) — il ne la laisse pas fermée. Un plafond atteint est donc signalé par une **alerte** (journal WARNING + MQTT `measures_purge_timeout` + bannières UI), pas par le badge « Stabilisation ».

### Constantes (`src/constants.h`)

| Constante | Valeur | Rôle |
|---|---|---|
| `kPurgeWindowMin` | 10 | Fenêtre de médianes (min) — `static_assert` ∈ [2 ; 10] |
| `kPurgeSamplesPerMin` | 12 | Lectures EZO par minute (5 s) — `static_assert` ∈ [3 ; 60] |
| `kPurgeStablePhRange` / `kPurgeStableOrpRange` | 0,15 pH / 15 mV | Étendue max sur la fenêtre pour « stable » |
| `kPurgeRecloseRangePh` / `kPurgeRecloseRangeOrp` | 0,30 pH / 30 mV | Seuil de refermeture C1 (= 2 × stable, `static_assert`) |
| `kPurgeFloorMs` | 600 000 (10 min) | Plancher — `static_assert` ≥ `kPurgeWindowMin` × 60 s |
| `kPurgeCapMs` | 5 400 000 (90 min) | Plafond — `static_assert` ≥ 1 h et > plancher |
| `kShortOutageS` | 10 s | Durée max d'une coupure « courte » (dispense C3) — `static_assert` ≤ 30 |
| `kOutageWaiverMinIntervalS` | 3 600 s | Au plus une dispense par heure — `static_assert` ≥ 600 |

### Dispense de purge après coupure courte (C3)

Un reboot logiciel de quelques secondes pendant une filtration active ne vide pas la conduite : re-purger 10 min serait du temps de régulation perdu. La coquille maintient en **RAM RTC** (`RTC_NOINIT_ATTR OutageRtcRecord`, 20 o, magic `"PURG"`, FNV-1a — survit à un reset logiciel/panic/watchdog, **pas** à une coupure secteur) un enregistrement `{heure vive, eau présente, dernière dispense}` rafraîchi ~1 s (`kOutageRtcRefreshMs`) **seulement si l'heure système est valide**.

`begin()` (appelé **après** `rtcManager.applyToSystem()`, **avant** le premier `update()`) évalue `evaluateOutageContinuity()` dans cet ordre, **fail-closed** :

| Ordre | Refus (`OutageDeny`) | Libellé journal |
|---|---|---|
| 1 | mémoire RTC absente ou corrompue (`kMemory`) | `mémoire RTC absente ou corrompue` |
| 2 | motif de reset hors liste blanche SW / PANIC / INT_WDT / TASK_WDT (`kResetKind`) | `reset matériel (coupure secteur possible)` |
| 3 | heure invalide au boot, ou antérieure à l'heure vive (`kClock`) | `heure invalide ou incohérente` |
| 4 | coupure > `kShortOutageS` (`kTooLong`) | `coupure trop longue` |
| 5 | eau absente avant la coupure (`kNoWater`) | `eau absente avant la coupure` |
| 6 | dispense accordée il y a < `kOutageWaiverMinIntervalS` (`kRateLimited`) | `dispense déjà accordée il y a moins d'une heure` |
| — | accordée (`kNone`) | `accordée` |

Si accordée, `lastWaiverEpoch` est horodaté et la dispense est présentée au premier tick : la porte s'ouvre motif `kWaived` (durée 0), et `isCirculationSettled()` est vrai pour tout le cycle d'eau. Les filtres pH/ORP repartent quand même de zéro (ré-amorçage ~65 s).

### Circulation établie (C4) — `isCirculationSettled()`

`measureValidityCirculationSettled()` : eau absente → `false` ; cycle dispensé → `true` ; sinon `now − waterRiseMs ≥ kPurgeFloorMs`. C'est la condition d'autorisation de la branche **Programmée** (aveugle aux mesures) : le produit injecté doit être emporté au bassin par une conduite dont l'eau a été renouvelée. Elle **ne dit rien** de la validité des mesures.

## API publique

```cpp
void begin();                                 // APRÈS rtcManager.applyToSystem(), AVANT le 1er update()
void update();                                // loopTask, AVANT history.update() (le point 5 min lit le cache)
MeasureValiditySnapshot getSnapshot() const;  // snapshot cohérent (1 section critique)
bool waterPresent() const;                    // getters unitaires (cohérence non garantie ENTRE eux)
bool measuresValid() const;
bool isCirculationSettled() const;            // C4 : eau présente ≥ kPurgeFloorMs, ou cycle dispensé
```

```cpp
struct MeasureValiditySnapshot {
  bool waterPresent;             // eau présente (source unique feature-056, en cache)
  bool measuresValid;            // porte OUVERTE : mesures représentatives du bassin
  bool circulationSettled;       // C4
  uint32_t stabElapsedS;         // secondes écoulées dans la purge courante (0 hors purge)
  uint32_t lastPurgeDurationS;   // durée de la dernière purge terminée (s)
  uint8_t lastPurgeEnd;          // PurgeEnd : 0 aucune / 1 stabilité / 2 plafond / 3 dispensée / 4 re-purge
  uint8_t purgeCount;            // re-purges du cycle d'eau courant (C1)
};
```

## Concurrence

Le cache est lu depuis **deux contextes** : `loopTask` (history, push WS, pump_controller) et `async_tcp` (`GET /data`) → snapshot sous spinlock `portMUX` (section critique minimale, **jamais de mutex bloquant en contexte async**). Les consommateurs lisent le **cache**, jamais `filtration.resolveWaterPresence()` directement ; l'état interne (`_state`, `_stab`, mémoire RTC) appartient à `loopTask` seule.

## Contrat des champs (WS `sensor_data` + `GET /data`, parité obligatoire)

| Champ | Type | Sémantique |
|-------|------|------------|
| `water_present` | bool | Présence d'eau résolue (source unique feature-056). |
| `measures_valid` | bool | `true` = mesures pH/ORP/température représentatives (porte ouverte). `false` = eau absente **ou** purge / re-purge en cours. `false` au boot tant qu'`update()` n'a pas tourné. |
| `measures_stab_elapsed_s` | uint | Secondes **écoulées** dans la purge courante ; `0` hors purge. Remplace l'ancien `measures_stab_remaining_s` (il n'y a plus de fin prévisible). |
| `measures_purge_last_duration_s` | uint | Durée de la **dernière purge terminée** (s). `0` si aucune depuis le boot ou si dispensée. |
| `measures_purge_last_end` | uint8 | Motif de fin de la dernière purge : `0` aucune, `1` stabilité, `2` **plafond** (alerte), `3` dispensée (coupure ≤ 10 s), `4` re-purge terminée par stabilité. Valeurs figées (`enum PurgeEnd`). |
| `measures_purge_count` | uint8 | Nombre de **re-purges** (refermetures C1) du cycle d'eau courant ; remis à zéro au front descendant d'eau. |

⚠️ Ne pas confondre avec `ph_stab_remaining_s` / `orp_stab_remaining_s` (sas post-calibration des **pompes**, inchangé — [pump-controller.md](pump-controller.md)).

## Consommateurs du verdict

| Consommateur | Usage |
|---|---|
| `history` | exclusion des points pH/ORP/température du point 5 min |
| UI (WS + `/data`) | badges « Filtration arrêtée » / « Stabilisation depuis N min » / « Re-stabilisation depuis N min » ; ligne « Dernière purge » (Réglages) ; bannières plafond (pages pH et ORP) |
| `pump_controller` | `measuresValid()` : intégrale pH, feedforward, alertes système pH/ORP ; `isCirculationSettled()` : branche Programmée (C4) |
| `sensor_filter` (v2.20.3, C6 v2.34.0) | porte d'alimentation des filtres pH/ORP ; `ready()` faux porte fermée |

### La porte d'alimentation des filtres (v2.20.3, durcie en v2.34.0)

Jusqu'en v2.20.2, le verdict suspendait l'**injection** mais pas l'**ingestion** : les filtres pH/ORP avalaient les mesures de stagnation, leur médiane acquérait autorité et rejetait ensuite les mesures représentatives (incident du 2026-08-16). `SensorManager::_updateFilterGates()` lit le **cache** et le transmet via `SensorFilter::setContext(waterPresent, measuresValid, nowMs)` avant `addSample()`. Porte fermée ⇒ l'échantillon n'entre nulle part ; réouverture ⇒ ré-amorçage.

**C6 (v2.34.0)** : `SensorFilter::ready()` retourne aussi **faux tant que la porte est fermée**. Avant, une **refermeture** (C1) laissait `ready()` vrai jusqu'à 20 s (âge du dernier échantillon) : P, I, feedforward et gardes d'alerte pouvaient consommer une dernière mesure de rinçage. Le plancher de gel de 60 s (`kSensorFilterGateFloorMs`) appliqué par le filtre après tout front montant est conservé et indépendant. Décision : [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md).

## Comportement au boot

Eau présente au premier `update()` → front montant vu au boot → **purge complète** (plancher 10 min + stabilité), **sauf** dispense C3 (coupure ≤ 10 s, cf. ci-dessus). Le sas de stabilisation des **pompes** (`armStabilizationTimer`, 5 min pH / 3 min ORP) n'est **plus** armé au boot ni au démarrage de la filtration : il ne couvre que la post-calibration EZO et le double quota de minuit ([pump-controller.md](pump-controller.md#stabilisation-post-calibration-et-passage-de-minuit-v2340)).

## Logs (préfixe `[STAB]`)

| Événement | Niveau | Message (forme) |
|---|---|---|
| Boot, dispense accordée | INFO | `[STAB] Coupure N s (reset logiciel, eau présente avant) → purge de reprise DISPENSÉE` |
| Boot, mémoire RTC absente | INFO | `[STAB] Démarrage à froid (mémoire RTC absente) → purge complète à la reprise d'eau` |
| Boot, dispense refusée | INFO | `[STAB] Coupure <N s \| de durée inconnue> (<motif>) → purge complète à la reprise d'eau` |
| Front montant d'eau | INFO | `[STAB] Reprise de circulation — purge des mesures pH/ORP (plancher 10 min, fin sur stabilité, plafond 90 min) ; bruts : pH=6.957 ORP=556.0 mV (eau stagnante)` |
| Fin de purge par stabilité | INFO | `[STAB] Fin de purge par stabilité (X min Y s, pH a→b ; ORP a→b mV) — mesures pH/ORP représentatives` |
| Fin de purge au plafond | **WARNING** | `[STAB] Purge des mesures ouverte au PLAFOND (X min Y s) sans stabilité (pH a→b ; ORP a→b mV) — sonde bruyante ou transitoire long ; régulation reprise, à surveiller` + `mqttManager.publishAlert("measures_purge_timeout", msg)` |
| Purge dispensée | INFO | `[STAB] Purge dispensée (coupure N s) — mesures pH/ORP représentatives immédiatement` |
| Refermeture C1 | **WARNING** | `[STAB] Re-purge n°N — mesures redevenues instables après ouverture (étendue pH … / ORP … mV) : porte refermée jusqu'à nouvelle stabilité` |
| Fin de re-purge | INFO | `[STAB] Fin de re-purge n°N (X min Y s, pH … ; ORP … mV) — mesures de nouveau représentatives` |

`a→b` = bruts min → max observés pendant la purge (`rangeOrNa`) ; une mesure absente s'écrit `n/a`, jamais `nan` ni `0.00`. Une retombée d'eau pendant la purge n'émet rien côté `[STAB]` (le gel des filtres est journalisé par `sensors`, voir [sensors.md](sensors.md)).

## Limites connues (assumées, GO `pool-chemistry` option (a), fail-closed)

- Une **dérive lente ≤ 0,017 pH/min** est jugée stable (0,15 pH sur 9 min d'écart entre médianes extrêmes).
- L'**ORP seul** ne détecte pas une pente ≤ 1,6 mV/min (voie secondaire : seuil 15 mV ≈ 13,5 mV de bruit).
- Sur **eau plate avec bruit ±0,18 pH**, l'étendue de 10 médianes ≈ 0,16 ≈ seuil : ouverture au plancher dans 12 graines sur 21, **queue possible jusqu'à t+29** (`test_E_rejeu_0409_5_graines_ouvrent_ou_plafond`). Élargir le seuil à 0,20 (option (b)) a été **refusé** ; des médianes sur 2 min (option (c)) sont la piste préférée → [feature-097](../../specs/features/todo/feature-097-seuil-de-stabilite-sur-donnees-terrain.md), décision sur ≥ 5 matins de télémétrie `measures_purge_last_*` (au plus tôt le 10/09/2026).
- Coût nominal : ≈ 10 min + 65 s de ré-amorçage des filtres après une longue coupure (contre 15 min de minuterie avant), mais ~47 min un matin de rinçage réel — c'est le prix d'une ouverture **sur le plateau** au lieu du fond du V.

## Cas limites

- **`millis() == 0` au front montant** : géré par la machine d'état (temps non signés).
- **Deux refermetures puis plafond** : figé par test natif (`test_native_measure_validity`) — après le plafond, plus aucune refermeture jusqu'au prochain front d'eau.
- **Repli HTTP `/data`** : ne porte pas `filtration_state_stale` (asymétrie préexistante) — l'UI dégrade « Signal filtration périmé » en « Filtration arrêtée » ; voir [API.md](../API.md#get-data--write).

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `measureValidity.begin()` après `rtcManager.applyToSystem()`, `update()` dans la loop **avant** `history.update()`
- [`src/history.cpp`](../../src/history.cpp) — exclusion pH/ORP/température du point 5 min
- [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — `setContext()`, `ready()` (C6)
- [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `measuresValid()` (intégrale, FF, alertes), `isCirculationSettled()` (branche Programmée pH et ORP)
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — publication des 5 champs
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — `publishAlert("measures_purge_timeout", …)`
- [`test/test_native_purge_stability/`](../../test/test_native_purge_stability/), [`test/test_native_measure_validity/`](../../test/test_native_measure_validity/)
- [filtration.md](filtration.md) — `resolveWaterPresence()`, source unique ([ADR-0026](../adr/0026-mode-installation.md))
- [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md) — purge adaptative, retrait du réglage utilisateur
