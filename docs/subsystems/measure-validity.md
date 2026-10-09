# Subsystem — `measure_validity`

- **Fichiers** : [`src/measure_validity.h`](../../src/measure_validity.h), [`src/measure_validity.cpp`](../../src/measure_validity.cpp) (coquille impure) + [`src/measure_validity_logic.h`](../../src/measure_validity_logic.h), [`src/measure_validity_logic.cpp`](../../src/measure_validity_logic.cpp) (machine d'état pure) + [`src/dose_available_logic.h`](../../src/dose_available_logic.h) (délai avant dosage, pur)
- **Singleton** : `extern MeasureValidityManager measureValidity;`
- **Introduit par** : feature-061 (v2.13.0) ; étendu à la température en itération 2 (v2.13.0) ; porte d'alimentation des filtres en v2.20.3 ; purge adaptative par la feature-091 (v2.34.0) — [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md) (**Superseded by ADR-0092**) ; estimateur de bloc remplacé par la moyenne interquartile en feature-097 (v2.54+) ; **analyse de stabilité RETIRÉE par la feature-139 (v2.62.0)** — [ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md)

## Rôle

Décide si les mesures **pH/ORP/température sont représentatives de l'eau du bassin**. Les sondes sont montées dans le circuit de filtration : filtration arrêtée = eau stagnante autour des sondes → la valeur dérive localement et ne reflète plus le bassin. À la reprise de la circulation, une **attente** est appliquée avant de redéclarer les mesures valides : l'eau stagnante de la chambre de mesure doit être chassée par de l'eau fraîche du bassin.

### ⚠️ Retrait de l'analyse de stabilité (feature-139, v2.62.0)

De la v2.34.0 à la v2.61.x, cette attente se terminait sur un **verdict de stabilité** : les bruts pH/ORP devaient cesser de bouger (fenêtre glissante de 10 statistiques de bloc, seuils 0,15 pH / 15 mV), avec une **re-fermeture** (C1) si la mesure recommençait à bouger et un **plafond** fail-safe de 90 min. Ce mécanisme a été **retiré**, motif établi par le gate `pool-chemistry` du 2026-10-08 ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md)) :

- c'est un détecteur d'**étendue** — il ne se ferme que pendant que la mesure *bouge*. Une mesure **durablement fausse** (une sonde qui lit faux de façon stable) ne produit aucune étendue et le traverse sans être vue : il est structurellement aveugle au cas même qui motivait sa conservation ;
- ses seuils n'avaient été calés qu'une fois, sur un seul bassin, avec une marge mesurée de 1,5× seulement — non universels par construction ;
- chiffrage : une heure de dosage entièrement faux coûte **0,07 pH** (pire cas universel), contre un **doublement de l'écart résiduel** mesuré pour 52 % de temps porte fermée sur une régulation P pure.

**Ce qui reste** : un **plancher fixe** de 10 min (`kPurgeFloorMs`, physique de renouvellement d'eau en conduite, universel), les **butées chimiques** du pH (instantanées, insensibles au bruit), et les **plafonds horaire/journalier** de dosage — c'est désormais sur ces derniers que repose tout le risque résiduel qu'un mécanisme de stabilité aurait pu couvrir.

Le seul paramètre qui demeure est la durée d'**arrêt d'eau** en-deçà de laquelle ce plancher est sauté (`kWaterStopGraceS`, voir plus bas) : un arrêt court (nettoyage rapide du bassin) laisse l'eau de la conduite en place, attendre de nouveau 10 min serait punitif sans raison physique.

**La chaîne de mesure interne ne change PAS** : les sondes continuent d'être lues et publiées (champs live `ph`/`orp`/`phRaw`/…). Mode Auto de la filtration : inchangé (lit `sensors.getTemperature()` en direct).

## Architecture (Humble Object, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md))

| Module | Rôle | Testé en natif |
|---|---|---|
| ~~`purge_stability_logic`~~ (pur) | **SUPPRIMÉ en v2.62.0** (feature-139) — statistique de bloc, fenêtre glissante, verdict stable/instable. Historique : amendement 2026-09-20 de l'[ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md) (superseded). | ~~`test/test_native_purge_stability/`~~ (supprimé) |
| `measure_validity_logic` (pur) | Machine d'état porte/attente (`measureValidityTick`), datation de l'arrêt d'eau courant (C2), circulation établie (C4), décision de dispense à travers un redémarrage (C3, `evaluateOutageContinuity`) | `test/test_native_measure_validity/` |
| `dose_available_logic` (pur, feature-139) | Délai avant dosage automatique annoncé à l'utilisateur — max du reste de plancher de purge et du reste de plancher de gel du filtre capteur (`computeDoseAvailableInS`) | `test/test_native_dose_available/` |
| `measure_validity` (coquille) | Collecte : `resolveWaterPresence()`, `millis()`, bruts EZO, `esp_reset_reason()`, mémoire RTC ; journal ; cache sous spinlock | — |

### Machine d'état (`measureValidityTick`, une transition par tick, priorité décroissante)

| # | Condition | Effet | Événement |
|---|---|---|---|
| 1 | Eau absente | porte **fermée**, pas de purge. Datation de l'arrêt (C2, feature-139) : au front descendant, `waterStopMs = ce tick` ; à chaque tick suivant, si l'arrêt **atteint ou dépasse** `graceMs`, LATCH `stopGraceExceeded` (monotone — ne redescend jamais tant que l'eau reste absente, rend le repliement de `millis()` inoffensif) | `kWaterDrop` |
| 2 | Front montant d'eau | **purge** démarre (`purgeStartMs = waterRiseMs = now`). Si l'arrêt qui vient de se terminer était **datable ET court** (`haveWaterStop && !stopGraceExceeded`) : porte **ouverte** immédiatement, motif `kWaived`, durée 0, `waivedCycle = true` | `kPurgeStarted` |
| 3 | Purge en cours | **ouverture** si `écoulé ≥ floorMs` (10 min), motif `kStability`. Plus de condition d'étendue, plus de re-fermeture, plus de plafond (feature-139) | `kOpened` |

Toutes les soustractions de temps sont non signées (repliement de `millis()` sûr). La frontière de grâce est **`>=`, pas `>`** : à durée == `kWaterStopGraceS` **pile**, c'est déjà un arrêt **long** (sens prudent, fail-closed) — et cette frontière **doit** rester alignée entre `measureValidityTick()` (C2, en millisecondes) et `evaluateOutageContinuity()` (C3, en secondes), sous peine de faire diverger le verdict selon qu'un redémarrage s'est intercalé ou non (défaut réellement trouvé et corrigé pendant ce lot, voir plus bas).

**Un seul chemin de dispense** (arbitrage `pool-chemistry`, feature-139) : que l'arrêt d'eau ait traversé un redémarrage ou non, c'est **toujours** la même comparaison « durée d'absence < `kWaterStopGraceS` » qui décide. La mémoire RTC de la coquille (C3) ne fait que **porter** `waterStopMs` à travers un reboot dans le référentiel `millis()` post-boot — ce n'est pas une branche de décision séparée. Avant ce lot, deux chemins cohabitaient (un quota « une dispense par heure » côté redémarrage, une re-fermeture indépendante côté marche continue) : c'est exactement cette cohabitation qui produisait le défaut observé au journal du 2026-10-08 (dispense accordée pour une coupure de 8 s, puis annulée de fait par le démarrage filtration suivant, hors du chemin de dispense d'alors — 20 min de porte fermée après un arrêt de 10 s).

⚠️ **Défaut trouvé et corrigé en revue (code-reviewer, 2ᵉ passage)** : le latch `stopGraceExceeded` n'est **remesuré** qu'en règle 1 (eau absente). Entre `begin()` — qui le fige au boot sur un `outageS` daté **avant** le setup Wi-Fi/NTP/FS — et le tout premier `update()`, des dizaines de secondes peuvent s'écouler **sans qu'aucun tick « eau absente » ne passe dessus** (l'eau est déjà présente au tout premier tick vu par la machine d'état). Un arrêt de 175 s suivi de 30 s de démarrage aurait donc été dispensé à tort en ne faisant confiance qu'au latch. Corrigé en **remesurant aussi** au front montant (règle 2), en plus du latch — jamais à sa place : le latch reste ce qui rend un arrêt de plus de 49,7 jours inoffensif face au repliement de `millis()`, la remesure seule ne le garantirait pas.

### Constantes (`src/constants.h`)

| Constante | Valeur | Rôle |
|---|---|---|
| `kPurgeFloorMs` | 600 000 (10 min) | Plancher d'attente — **inchangé** par feature-139 (verdict A5 du gate : physique de renouvellement d'eau en conduite, universel). `static_assert` : reste dans [5 ; 30] min |
| `kWaterStopGraceS` (ex-`kShortOutageS`, **renommé** feature-139) | 180 (3 min) | Durée d'arrêt d'eau strictement en-deçà de laquelle le plancher de reprise est **sauté**, que l'appareil ait redémarré ou non. Justification **physique**, pas statistique (gate `pool-chemistry`, 2026-10-08, ADR-0073) : renouvellement au droit des sondes majoré à 71 s (60 m de DN50 = 118 L à 6 m³/h) ; en 180 s d'eau immobile ΔpH ≈ 0 (pas d'échange CO₂ en conduite fermée) et ΔORP ≪ 1 mV (décroissance chlore ≤ 0,5 mg/L/h) ; marge ≥ 3 à 5× avant le premier effet mesurable. `static_assert` : > 0, strictement < `kPurgeFloorMs` (sinon un arrêt « court » court-circuiterait tout ce que le plancher protège), ≤ 5 min (au-delà, le calcul de décroissance du chlore ne borne plus le ΔORP à une fraction de mV) |

**Constantes retirées par feature-139** (plus dans `constants.h`) : `kPurgeStablePhRange` / `kPurgeStableOrpRange` (seuil de stabilité), `kPurgeRecloseRangePh` / `kPurgeRecloseRangeOrp` (seuil de re-fermeture C1), `kPurgeCapMs` (plafond 90 min), `kPurgeNoiseFloorPh` / `kPurgeNoiseFloorOrp` (plancher de bruit mesuré), `kPurgeMinDetectedDriftPhPerMin`, `kPurgeWindowMin`, `kPurgeSamplesPerMin` (fenêtre et cadence du détecteur de stabilité), `kOutageWaiverMinIntervalS` (quota « une dispense par heure » — devenu inutile : le critère de durée explicite suffit, le risque résiduel d'un détecteur d'eau qui bat est désormais borné par les plafonds horaire/journalier de dosage, pas par une garde ici).

### Dispense de purge après arrêt d'eau court (C3)

Un arrêt d'eau court (nettoyage rapide du bassin, redémarrage logiciel de quelques secondes) ne renouvelle pas l'eau de la conduite : attendre de nouveau 10 min serait du temps de régulation perdu. La coquille maintient en **RAM RTC** (`RTC_NOINIT_ATTR OutageRtcRecord`, 16 o, magic `"PURG"`, FNV-1a — survit à un reset logiciel/panic/watchdog, **pas** à une coupure secteur) un enregistrement `{heure vive, eau présente}` rafraîchi ~1 s (`kOutageRtcRefreshMs`), **seulement si l'heure système est valide**.

`begin()` (appelé **après** `rtcManager.applyToSystem()`, **avant** le premier `update()`) évalue `evaluateOutageContinuity()` dans cet ordre, **fail-closed** :

| Ordre | Refus (`OutageDeny`) | Libellé journal |
|---|---|---|
| 1 | mémoire RTC absente ou corrompue (`kMemory`) | `mémoire RTC absente ou corrompue` |
| 2 | motif de reset hors liste blanche SW / PANIC / INT_WDT / TASK_WDT (`kResetKind`) | `reset matériel (coupure secteur possible)` |
| 3 | heure invalide au boot, ou antérieure à l'heure vive (`kClock`) | `heure invalide ou incohérente` |
| 4 | coupure **≥** `kWaterStopGraceS` (`kTooLong`, frontière alignée sur le latch de `measureValidityTick`) | `coupure trop longue` |
| 5 | eau absente avant la coupure (`kNoWater`) | `eau absente avant la coupure` |
| — | accordée (`kNone`) | — |

⚠️ **Le quota « une dispense par heure » a été retiré** (feature-139) : avec un critère de durée explicite, il ne bloquait plus que le cas légitime (deux arrêts d'entretien dans la même heure).

Si accordée, la coquille pré-sème `waterStopMs`/`haveWaterStop` du module pur dans le référentiel `millis()` post-boot : la **même** comparaison « durée < `kWaterStopGraceS` » s'applique ensuite, qu'il y ait eu reboot ou non — ce n'est **pas** une branche de décision séparée (arbitrage `pool-chemistry`). La porte s'ouvre au premier tick, motif `kWaived` (durée 0), et `isCirculationSettled()` est vrai pour tout le cycle d'eau. Les filtres pH/ORP repartent quand même de zéro (ré-amorçage ~65 s).

### Circulation établie (C4) — `isCirculationSettled()`

`measureValidityCirculationSettled()` : eau absente → `false` ; cycle dispensé → `true` ; sinon `now − waterRiseMs ≥ kPurgeFloorMs`. C'est la condition d'autorisation de la branche **Programmée** (aveugle aux mesures) : le produit injecté doit être emporté au bassin par une conduite dont l'eau a été renouvelée. Elle **ne dit rien** de la validité des mesures. Inchangée par feature-139.

## Délai avant dosage annoncé à l'utilisateur (`dose_available_logic`, feature-139)

`PumpControllerClass::getDoseAvailableInS()` (coquille mince, `pump_controller.cpp`) expose, en secondes, **le délai avant que le dosage automatique redevienne possible** — c'est le **seul endroit de vérité** pour cette grandeur, consommé par `/data`, le WebSocket et les textes de l'UI (remplace un calcul **côté client** qui ne regardait que le reste de purge et ignorait le plancher de gel du filtre capteur, faux d'un facteur 10 sur le cas le plus fréquent : un cycle **dispensé**).

```cpp
// src/dose_available_logic.h
uint32_t computeDoseAvailableInS(bool measuresValid, uint32_t stabElapsedS,
                                  uint32_t floorMs, uint32_t filterRemainingMs);
```

Deux plancher **indépendants** peuvent dominer le délai :
- le reste du **plancher de purge** de `measure_validity` (`kPurgeFloorMs`, 10 min) — pertinent **uniquement** tant que la porte n'est pas encore ouverte (`measuresValid == false`) ;
- le reste du **plancher de gel du filtre** capteur (`kSensorFilterGateFloorMs`, 60 s, `sensor_filter.{h,cpp}`) — armé **inconditionnellement** à chaque front montant d'eau, y compris quand `measure_validity` dispense sa propre purge. C'est pourquoi les deux planchers sont indépendants : l'un peut déjà être satisfait (cycle dispensé) pendant que l'autre reste actif.

Le délai annoncé est le **max** des deux restes, jamais négatif (arithmétique non signée avec clamp explicite). **0** si le dosage est déjà possible.

⚠️ **Deux défauts trouvés par le test au moment de l'extraction** (même lot, gate `pool-chemistry` 2ᵉ passage) :
1. la première version ne passait **pas** `measuresValid` à la formule — elle calculait `floorS − stabElapsedS` sans jamais regarder si `measure_validity` avait déjà ouvert sa porte. Or `measureValidityElapsedS()` renvoie **toujours** 0 dès que la porte est ouverte (dispensée ou par plancher) : le terme de purge valait donc `floorS` complet (10 min) **en permanence**, y compris sur un cycle dispensé — recréant exactement le défaut d'affichage d'un facteur 10 que ce lot corrigeait. Corrigé en clampant le terme de purge à 0 dès que `measuresValid` est vrai ;
2. une **asymétrie d'une seconde** entre le chemin de dispense « en marche » (C2, millisecondes) et le chemin « à travers un redémarrage » (C3, secondes, arrondi) faisait diverger le verdict selon qu'un reboot s'était intercalé à l'instant pile de la frontière — corrigée, et épinglée par un test qui asserte l'**égalité** des deux chemins, pas seulement leurs valeurs.

### Contrat `/data` + WebSocket — `measures_dose_available_in_s`

| Champ | Type | Sémantique |
|---|---|---|
| `measures_dose_available_in_s` | uint32 (secondes) | Délai avant que le dosage automatique redevienne possible. **0** si déjà disponible. |

⚠️ **Deux mises en garde, imposées par le gate `pool-chemistry`** :
1. Ce nombre promet **uniquement** la fin de l'attente d'eau neuve (planchers de purge et de gel du filtre). Il **ne promet pas** qu'un dosage aura lieu : plafonds horaire et journalier, anti-rafale, pause de mélange, stabilisation post-calibration, présence d'eau et watchdog restent **en dehors** de ce calcul. Vocabulaire imposé : « fin de l'attente d'eau neuve », jamais « dosage garanti ».
2. Le champ est **omis** quand la filtration est à l'arrêt (le délai est alors **inconnu** : rien ne dit quand l'eau reviendra). Il n'est significatif que lorsque l'eau circule. Le client distingue alors deux causes d'absence : `water_present === false` (filtration arrêtée, délai réellement inconnu) vs. champ absent avec `water_present` vrai ou absent (firmware antérieur à ce champ) — voir [`data/app.js`](../../data/app.js) `_measuresWaitText()`.

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
  uint8_t lastPurgeEnd;          // PurgeEnd : 0 aucune / 1 stabilité / 3 dispensée (2 et 4 RETIRÉS v2.62, réservés)
  // purgeCount SUPPRIMÉ en v2.62.0 (feature-139) : le mécanisme de re-purge n'existe plus.
};
```

## Concurrence

Le cache est lu depuis **deux contextes** : `loopTask` (history, push WS, pump_controller) et `async_tcp` (`GET /data`) → snapshot sous spinlock `portMUX` (section critique minimale, **jamais de mutex bloquant en contexte async**). Les consommateurs lisent le **cache**, jamais `filtration.resolveWaterPresence()` directement ; l'état interne (`_state`, mémoire RTC) appartient à `loopTask` seule.

## Contrat des champs (WS `sensor_data` + `GET /data`, parité obligatoire)

| Champ | Type | Sémantique |
|-------|------|------------|
| `water_present` | bool | Présence d'eau résolue (source unique feature-056). |
| `measures_valid` | bool | `true` = mesures pH/ORP/température représentatives (porte ouverte). `false` = eau absente **ou** attente en cours. `false` au boot tant qu'`update()` n'a pas tourné. |
| `measures_stab_elapsed_s` | uint | Secondes **écoulées** dans la purge courante ; `0` hors purge. |
| `measures_purge_last_duration_s` | uint | Durée de la **dernière purge terminée** (s). `0` si aucune depuis le boot ou si dispensée. |
| `measures_purge_last_end` | uint8 | Motif de fin de la dernière purge : `0` aucune, `1` stabilité (= plancher atteint), `3` dispensée (arrêt d'eau court). **Domaine réduit depuis v2.62.0** : `2` (plafond) et `4` (re-purge) sont **retirés, plus jamais émis** — valeurs **réservées, non réattribuées** : un consommateur externe qui testait `=== 2` cesse simplement de matcher, sans faux-positif. |
| ~~`measures_purge_count`~~ | — | **Retiré en v2.62.0** (feature-139) : le mécanisme de re-purge a disparu, ce champ serait devenu structurellement constant à 0 — le conserver aurait publié une forme stable qui ment. |
| `measures_dose_available_in_s` | uint32 | **Nouveau en v2.62.0** — voir section dédiée ci-dessus. |

## Consommateurs du verdict

| Consommateur | Usage |
|---|---|
| `history` | exclusion des points pH/ORP/température du point 5 min |
| UI (WS + `/data`) | textes déterministes « Reprise automatique dans N min » (remplace les anciens badges « Stabilisation depuis N min » / « Re-stabilisation depuis N min », vocabulaire jugé incompréhensible par l'utilisateur) ; ligne « Dernière purge » (Réglages) |
| `pump_controller` | `measuresValid()` : intégrale pH, feedforward, alertes système pH/ORP ; `isCirculationSettled()` : branche Programmée (C4) ; `getDoseAvailableInS()` : délai annoncé |
| `sensor_filter` (v2.20.3, C6 v2.34.0) | porte d'alimentation des filtres pH/ORP ; `ready()` faux porte fermée |

### La porte d'alimentation des filtres (v2.20.3, durcie en v2.34.0)

Jusqu'en v2.20.2, le verdict suspendait l'**injection** mais pas l'**ingestion** : les filtres pH/ORP avalaient les mesures de stagnation, leur médiane acquérait autorité et rejetait ensuite les mesures représentatives (incident du 2026-08-16). `SensorManager::_updateFilterGates()` lit le **cache** et le transmet via `SensorFilter::setContext(waterPresent, measuresValid, nowMs)` avant `addSample()`. Porte fermée ⇒ l'échantillon n'entre nulle part ; réouverture ⇒ ré-amorçage.

**C6 (v2.34.0)** : `SensorFilter::ready()` retourne aussi **faux tant que la porte est fermée**. Le plancher de gel de 60 s (`kSensorFilterGateFloorMs`) appliqué par le filtre après tout front montant est conservé et indépendant — c'est lui qui alimente le second terme de `computeDoseAvailableInS()`. Décision : [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md).

## Comportement au boot

Eau présente au premier `update()` → front montant vu au boot → **attente complète** (plancher 10 min), **sauf** dispense C3 (arrêt d'eau court, cf. ci-dessus). Le sas de stabilisation des **pompes** (`armStabilizationTimer`, 5 min pH / 3 min ORP) n'est **pas** armé au boot ni au démarrage de la filtration : il ne couvre que la post-calibration EZO et le double quota de minuit ([pump-controller.md](pump-controller.md#stabilisation-post-calibration-et-passage-de-minuit-v2340)).

## Logs (préfixe `[STAB]`)

| Événement | Niveau | Message (forme) |
|---|---|---|
| Boot, dispense accordée | INFO | `[STAB] Arrêt d'eau N s (reset logiciel, eau présente avant) → purge de reprise DISPENSÉE` |
| Boot, mémoire RTC absente | INFO | `[STAB] Démarrage à froid (mémoire RTC absente) → purge complète à la reprise d'eau` |
| Boot, dispense refusée | INFO | `[STAB] Arrêt d'eau <N s \| de durée inconnue> (<motif>) → purge complète à la reprise d'eau` |
| Front montant d'eau | INFO | `[STAB] Reprise de circulation — purge des mesures pH/ORP (plancher 10 min)` |
| Fin de purge (plancher atteint) | INFO | `[STAB] Fin de purge (X min Y s) — mesures pH/ORP représentatives` |
| Purge dispensée | INFO | `[STAB] Purge dispensée — mesures pH/ORP représentatives immédiatement` |
| Reprise après arrêt daté (C5, feature-139) | INFO | `[STAB] Reprise après arrêt de N s — attente <dispensée \| complète>` — journalisé à **chaque** reprise d'eau datable, dispense ou non ; c'est la grandeur qui a permis de calibrer `kWaterStopGraceS` sur des données réelles |

⚠️ **Retirés en v2.62.0** (plus aucun code ne les émet) : le `WARNING` de plafond atteint, l'alerte MQTT `measures_purge_timeout`, et le `WARNING` de refermeture C1 / fin de re-purge.

## Limites connues

- **Un arrêt d'eau de durée inconnue (coupure secteur) vaut toujours un arrêt long** (fail-closed) : la mémoire RTC ne survit pas à une coupure secteur, donc une coupure de 30 s traversant une coupure secteur coûte une attente complète de 10 min — assumé, c'est la même doctrine que l'ADR-0059 d'origine.
- **Il n'existe plus aucun filet qui détecte une mesure pH/ORP durablement fausse mais stable**, en dehors des butées chimiques pH et des plafonds de dosage. C'est un choix assumé par le gate `pool-chemistry` (voir [ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md)) : l'ancien mécanisme ne couvrait de toute façon pas ce cas (détecteur d'étendue). La voie **ORP** reste, en plus, sans aucune butée de plausibilité sur la mesure **brute** — manque préexistant, documenté comme suite distincte : [feature-145](../../specs/features/todo/feature-145-butee-de-plausibilite-sur-l-orp-brut.md).
- **La frontière de grâce (`kWaterStopGraceS`) est stricte** : à durée == 180 s pile, aucune dispense. Un arrêt de 179 s et un arrêt de 181 s produisent des comportements radicalement différents (0 vs 10 min d'attente) — c'est voulu (fail-closed), pas un artefact d'arrondi.

## Cas limites

- **`millis() == 0` au front montant** : géré par la machine d'état (temps non signés).
- **Repliement de `millis()` pendant un arrêt d'eau très long** : couvert par le latch `stopGraceExceeded`, qui une fois posé ne redescend jamais tant que l'eau reste absente (sans lui, un arrêt > 49,7 jours repasserait pour « court »).
- **Un reboot pile à la frontière de grâce** : les deux chemins de dispense (C2 en marche, C3 à travers un reboot) doivent rendre le **même** verdict — assertée par un test dédié (pas seulement les valeurs, l'égalité des deux chemins).
- **Repli HTTP `/data`** : ne porte pas `filtration_state_stale` (asymétrie préexistante) — l'UI dégrade « Signal filtration périmé » en « Filtration arrêtée » ; voir [API.md](../API.md#get-data--write).

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `measureValidity.begin()` après `rtcManager.applyToSystem()`, `update()` dans la loop **avant** `history.update()`
- [`src/history.cpp`](../../src/history.cpp) — exclusion pH/ORP/température du point 5 min
- [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — `setContext()`, `ready()` (C6)
- [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `measuresValid()` (intégrale, FF, alertes), `isCirculationSettled()` (branche Programmée pH et ORP), `getDoseAvailableInS()` (coquille de `dose_available_logic.h`)
- [`src/dose_available_logic.h`](../../src/dose_available_logic.h) — délai avant dosage, module pur (feature-139)
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — publication des champs
- [`test/test_native_measure_validity/`](../../test/test_native_measure_validity/), [`test/test_native_dose_available/`](../../test/test_native_dose_available/)
- [filtration.md](filtration.md) — `resolveWaterPresence()`, source unique ([ADR-0026](../adr/0026-mode-installation.md))
- [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md) — purge adaptative d'origine, **superseded by** [ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md) — retrait de l'analyse de stabilité
