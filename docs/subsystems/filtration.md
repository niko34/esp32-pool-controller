# Subsystem — `filtration`

- **Fichiers** : [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- **Singleton** : `extern FiltrationManager filtration;`
- **GPIO relais** : `kFiltrationRelayPin = 26` (PCB v2, [`constants.h`](../../src/constants.h)) — actif haut. Voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md).

## Rôle

Pilote le relais de la pompe de filtration. **Le mode affiché est la seule autorité** qui
décide si la pompe tourne (feature-146, [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) :

| Mode | Jeton protocole (inchangé) | Comportement |
|---|---|---|
| **Auto** | `auto` | créneau calculé depuis la température de l'eau, recalculé quand elle change (planning figé à l'arrêt du cycle, promu à minuit — voir plus bas) |
| **Programme** | `manual` | créneau fixé par l'utilisateur, **jamais** recalculé |
| **Marche** | `force` | la pompe tourne — **sans expiration**, jusqu'au prochain changement de mode |
| **Arrêt** | `off` | la pompe est arrêtée — **sans expiration**, jusqu'au prochain changement de mode |

Les jetons sur le fil (NVS/MQTT/HTTP) sont **conservés tels quels** (zéro migration) : la
correspondance jeton → sens vit **exclusivement** dans `parseFiltrationMode()`
([`src/schedule_logic.h`](../../src/schedule_logic.h)/`.cpp`) — c'est le seul endroit du code
métier qui compare un littéral de mode.

⚠️ **L'« ordre direct » (`forceOn`/`forceOff`, minuteur 4 h) est SUPPRIMÉ** (feature-146,
v2.63.0). Avant ce lot, un second système de décision, évalué **avant** le mode dans
`decideFiltrationRun()`, pouvait rendre un changement de mode sans effet (le mode `auto` ne
pouvait rien démarrer tant qu'un `forceOff` MQTT restait actif — défaut de production du
2026-10-09, racine de ce lot). Les champs `FiltrationConfig::forceOn/forceOff` et
`FiltrationRuntime::forceOnStartMs/forceOffStartMs` n'existent plus. Les boutons **Démarrer** /
**Arrêter** de l'interface changent désormais le **mode** (→ Marche / → Arrêt), rien d'autre.

> ⚠️ **Le pilotage du relais est conditionné au mode d'installation** (feature-056, [ADR-0026](../adr/0026-mode-installation.md)). Le relais n'est piloté **que** si `mqttCfg.installMode == InstallMode::ManagedFiltration` (`managed`). En `powered` (contrôleur alimenté par le circuit de filtration) et `external` (filtration tierce), `update()` **retourne de façon anticipée** ([`filtration.cpp:125`](../../src/filtration.cpp:125)) : aucune commande relais, planification masquée côté UI. Voir [Mode d'installation et présence d'eau](#mode-dinstallation-et-présence-deau-feature-056).

## API publique

```cpp
void begin();
void update();              // appelé dans loop()
bool isRunning() const;
bool getRelayState() const;
void computeAutoSchedule(); // mise en service uniquement (bascule vers "auto") — voir bug-126 ci-dessous
void ensureTimesValid();
void publishState();

// feature-155 : instant millis() de démarrage du cycle courant, 0 si à l'arrêt.
unsigned long filtrationStartedAtMs() const;

// feature-158 (condition 7 du gate pool-chemistry) : ACCESSEUR UNIQUE de la durée
// écoulée du cycle courant — inclut le report recrédité après redémarrage
// (voir « Reprise de la durée de filtration » ci-dessous). 0 si à l'arrêt.
// Les TROIS sites consommateurs (web_routes_data.cpp, ws_manager.cpp,
// web_routes_config.cpp) appellent CET accesseur, aucun ne recalcule le ternaire.
unsigned long filtrationElapsedMs() const;
uint32_t filtrationElapsedS() const;  // même accesseur, en secondes (/data, WebSocket)

// feature-056 — mode d'installation ExternalFiltration
void setExternalState(bool running);                              // signal externe (HTTP/MQTT) — non bloquant (portMUX)
void getExternalState(bool& on, uint32_t& lastMs, bool& known) const;  // copie le triplet sous lock
WaterPresence resolveWaterPresence() const;                        // SOURCE UNIQUE présence d'eau (toutes gardes de dosage)
```

## Algorithme auto — planning figé à l'arrêt, promu à minuit (bug-126, [ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md))

⚠️ **Le planning auto n'est plus jamais écrit pendant que la pompe tourne.** Avant bug-126, `computeAutoSchedule()` était rappelée 5 min après le démarrage du cycle ; une température en baisse pouvait rétracter la fenêtre et arrêter le cycle qui venait de produire la mesure (`decideFiltrationRun()` étant une décision de **niveau**, pas de front). Deux moments seulement modifient désormais l'état :

1. **Pendant le cycle**, `sampleCycleTemperature()` échantillonne la température toutes les `kFiltrationTempSampleIntervalMs` (1 min) et l'intègre dans une **moyenne incrémentale** (`CycleTempAccumulator`, [`schedule_logic.h`](../../src/schedule_logic.h)) — **jamais une somme** (`mean += (x - mean) / count`), pour rester stable en mode `PoweredByFiltration` (eau présente 24/7). Un échantillon n'est retenu que si les **trois conditions cumulatives** de `isSampleEligible()` tiennent : purge terminée (`measureValidity.isCirculationSettled()`), horaire en plage de **base** (ferme un mode Marche nocturne sans code dédié — et jamais le créneau élargi par le Boost, [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)), sonde eau non figée (`!sensors.isTemperatureFrozen()`). La source est **exclusivement** `sensors.getWaterTemperature()` (offset de calibration borné appliqué, bug-114), avec un repli de tolérance `selectFiltrationRefTemp()` pour un appareil neuf sans rôle de sonde attribué — **jamais** `sensors.getTemperature()`, qui retomberait sur une sonde `Circuit` (boîtier électronique).
2. **À l'arrêt du cycle**, `writePendingSchedule()` fige la moyenne dans un **planning en attente** 100 % RAM (`PendingSchedule`, ci-dessous) — jamais dans `filtrationCfg` directement.
3. **À minuit**, `promotePendingScheduleIfDue()` (appelée en tête d'`update()`) bascule le planning en attente vers `filtrationCfg.start/end` si les conditions de promotion sont réunies — **une seule écriture NVS par jour**, zéro si la fenêtre calculée est identique à la fenêtre courante.

`computeAutoSchedule()` **subsiste** comme voie « mise en service » : application **immédiate** d'un planning, appelée quand le mode bascule vers `auto` (`web_routes_config.cpp`, `mqtt_manager.cpp`) — pas le chemin emprunté pendant la marche normale.

### `PendingSchedule` et les gardes de promotion

```cpp
struct PendingSchedule {
  bool  valid = false;
  int   startMin = -1;
  int   endMin = -1;
  float refTempC = NAN;                // journal
  uint16_t sampleCount = 0;            // journal
  float refMinC = NAN, refMaxC = NAN;  // journal (dispersion de la moyenne)
  char  computedOnDate[9] = {};        // "YYYYMMDD" local de l'arrêt producteur
  // Instantané de BASE : état du planning courant à l'instant du calcul.
  char  basisStart[6] = {};
  char  basisEnd[6] = {};
  char  basisMode[8] = {};
};
```

La promotion est gouvernée par la fonction pure `decideSchedulePromotion()` ([`schedule_logic.h`](../../src/schedule_logic.h)), ordre imposé et cumulatif :

| Verdict | Effet côté coquille |
|---|---|
| `Promote` | écrit `filtrationCfg.start/end`, `_pending.valid = false`, `saveMqttConfig()`, `publishState()`, log |
| `PumpRunning` | pending **conservé** — promotion **différée** au premier tour où la pompe est à l'arrêt (jamais sautée, jamais forcée) |
| `NotNewDay` | conserve, silencieux |
| `BasisChanged`, `NotAuto` | **jette** le pending (`valid = false`) + log `info` du motif |
| `AlreadyPromoted`, `NoPending` | rien |

`BasisChanged` compare `basisStart`/`basisEnd`/`basisMode` à `filtrationCfg` courant : un **instantané**, pas une liste d'appels à invalider — il attrape tout chemin d'écriture externe (`web_routes_config.cpp`, `mqtt_manager.cpp`), y compris ceux non énumérés. **Ni l'un ni l'autre de ces deux fichiers n'a été modifié par bug-126** pour invalider le pending explicitement.

`resolveReferenceTemp()` rend un `RefTempVerdict` (`Ok` / `NoSample` / `TooShort` / `TooCold` / `Implausible`) sur la moyenne du cycle, bornes `[kFiltrationRefTempMinC = 4 °C ; kFiltrationRefTempMaxC = 40 °C]` — voir [Bornes de plausibilité](#bornes-de-plausibilité-de-la-température-de-référence) plus bas. `TooCold` (hivernage) et `Implausible` (panne de sonde présumée) sont **traités différemment** : le premier en `info` sans alerte, le second en `critical` + alerte MQTT `filtration_temp_implausible`.

⚠️ **Tout est perdu au redémarrage** ([ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) : `_pending`, l'accumulateur en cours et le drapeau d'amorçage `_haveRepresentativeMeasure` sont en RAM, jamais persistés. Coût assumé : un jour de planning périmé au pire, auto-réparateur au cycle suivant.

### Bornes de plausibilité de la température de référence

`kFiltrationRefTempMinC = 4.0f` / `kFiltrationRefTempMaxC = 40.0f` ([`constants.h`](../../src/constants.h:2352)) ferment deux cas dégénérés : à 44 °C la fenêtre calculée commence à franchir minuit, à 48 °C elle sature à 24 h (`start == end`) et arrêterait la filtration **pour toujours en silence**. Deux `static_assert` de `constants.h` garantissent à la compilation que la fenêtre auto ne peut jamais franchir minuit tant que `kFiltrationRefTempMaxC` reste ≤ 40 °C.

### Réparation d'une plage nulle en mode auto (`ensureTimesValid()`)

Si `filtrationCfg.mode == "auto"` et `start == end` (NVS corrompue, ancien défaut, saisie incohérente), la plage est réparée sur `08:00`-`20:00`, avec `systemLogger.critical()` immédiat et une alerte MQTT `filtration_schedule_invalid` (différée jusqu'à la première connexion MQTT si l'appel a lieu au boot, avant `mqttManager.begin()`). **Mode `auto` uniquement** — en `manual`, une plage nulle peut être voulue par l'utilisateur.

⚠️ **Mode inconnu (feature-146)** : `ensureTimesValid()` appelle désormais `parseFiltrationMode()` — un jeton non reconnu (config corrompue, client legacy envoyant une valeur hors `auto|manual|force|off`) est journalisé (`systemLogger.warning`) puis **replié sur `auto`**, au lieu d'être silencieusement ignoré. `update()` journalise en plus un `critical` throttlé s'il atteint malgré tout un mode `Inconnu` après cette coercition (course entre lecture et coercition) — voir [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md).

## État interne

```cpp
struct FiltrationRuntime {
  bool running = false;
  unsigned long startedAtMs = 0;
  unsigned long lastStoppedAtMs = 0;   // 0 si jamais arrêtée
  unsigned long carryMs = 0;           // feature-158 : secondes CRÉDITÉES par une reprise après redémarrage
};
```

`scheduleComputedThisCycle` (recalcul en marche) est **retiré** de cette structure par bug-126 — devenu sans objet, le recalcul en marche ayant disparu. `forceOnStartMs`/`forceOffStartMs` sont **retirés** par feature-146 avec le reste de l'ordre direct (v2.63.0). `carryMs` est **ajouté** par feature-158 (v2.74.0) — voir la section dédiée ci-dessous.

## Reprise de la durée de filtration à travers un redémarrage (feature-158, v2.74.0)

> Décision architecturale associée : [ADR-0104](../adr/0104-la-continuite-de-l-eau-a-un-producteur-unique-et-des-consommateurs.md) — la continuité de l'eau a un producteur unique et des consommateurs.

### ⚠️ `filtration_elapsed_s` N'EST PLUS (et n'a jamais dû être) l'uptime

Avant ce lot, `state.startedAtMs = millis()` était reposé **sans condition** au
boot dès que la filtration devait rester active, y compris quand la pompe
n'avait jamais cessé de tourner — la durée servie (`filtrationElapsedS()`,
exposée par `/data` et la trame WebSocket sous `filtration_elapsed_s`)
**mesurait alors l'appareil, pas la filtration**. Mesuré en production le
2026-10-12 : 1 619 s de durée annoncée contre 1 644 s d'**uptime** — un écart
de 25 s (le délai d'init), pour une circulation réelle d'environ **5 h**.
Conséquence chimique directe : la garde des 10 minutes de la carte
« Correction manuelle de la mesure » pH ([page-ph.md](../features/page-ph.md#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710),
`kPhTuneMinFiltrationMs`) se réarmait à chaque déploiement OTA, même après des
heures de filtration continue.

**Depuis ce lot**, `filtrationElapsedMs()`/`filtrationElapsedS()` additionnent
l'écoulé mesuré en RAM depuis le dernier démarrage (`millis() − startedAtMs`,
wrap-safe) **et** un report (`state.carryMs`) crédité **si et seulement si**
le redémarrage qui vient de se produire est un redémarrage logiciel court
pendant lequel l'eau n'a pas cessé de circuler. `carryMs` n'est **jamais**
fusionné dans `startedAtMs` : les deux grandeurs restent observables
séparément (arithmétique extraite dans `computeFiltrationElapsedMs()`,
[`src/filtration_resume_logic.{h,cpp}`](../../src/filtration_resume_logic.h),
module pur, prouvée par vecteurs natifs).

### La frontière

`measure_validity` reste le **producteur unique** du verdict de continuité de
l'eau (`OutageContinuityVerdict`, feature-139/[ADR-0089](../adr/0089-ce-qu-un-redemarrage-a-le-droit-doublier.md)) —
voir [measure-validity.md](measure-validity.md#dispense-de-purge-après-arrêt-deau-court-c3).
`filtration` en est un **consommateur** : elle persiste sa propre **donnée**
(des secondes **réellement mesurées** de filtration), jamais un second
verdict, et ne recalcule **aucune** comparaison de durée de coupure.
**Test de non-régression de cette frontière** : un `grep` de `kWaterStopGraceS`,
`outageS` et `graceS` dans `src/filtration.*` doit rendre **zéro occurrence**
hors commentaire explicatif.

### Mémoire RTC scellée propre à la filtration — `FiltrationRtcRecord`

Même patron que `OutageRtcRecord` (`measure_validity.cpp`) : `RTC_NOINIT_ATTR`,
16 o, magic **`"FLTR"`** (⚠️ distinct de `"PURG"` : deux enregistrements, deux
producteurs), scellement FNV-1a. Survit à un reset logiciel/panic/watchdog,
**pas** à une coupure secteur (RAM RTC aléatoire ⇒ magic/CRC faux ⇒
fail-closed).

```cpp
struct FiltrationRtcRecord {
  uint32_t magic;              // kFiltrationRtcMagic ("FLTR")
  uint32_t carryS;              // secondes RÉELLEMENT mesurées, pompe commandée ON, appareil vivant — JAMAIS la durée de la coupure
  uint8_t  filtrationRunning;   // 1 = pompe COMMANDÉE ON au dernier rafraîchissement — PROPRE à la filtration, jamais déduit de waterPresent
  uint8_t  installMode;         // InstallMode au dernier rafraîchissement
  uint8_t  reserved[2];
  uint32_t crc;                 // FNV-1a
};
```

Rafraîchi (`refreshFiltrationRtcRecord()`) aux transitions marche/arrêt et à
une cadence ~1 s pendant la marche (`kFiltrationRtcRefreshMs`) — une simple
photo de l'état courant, **jamais une décision**.

### Décision de recrédit — `evaluateFiltrationResume()`

Fonction pure (`src/filtration_resume_logic.{h,cpp}`, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)),
consommée **une seule fois** par `FiltrationManager::begin()`, et seulement si
ce boot vient de décider de redémarrer la filtration (`initialState == true`)
— c'est cette condition qui **exclut le Boost par construction** : il ne
repasse jamais par `begin()`. Motifs (`FiltrationResumeMotif`) et refus, dans
l'ordre évalué :

| Motif | Condition | `carryS` recrédité |
|---|---|---:|
| `kVerdictNotReady` | `measureValidity.bootContinuityVerdict()` n'a pas encore produit son verdict (ordre d'initialisation invalide) | 0 |
| `kNoRecord` | mémoire RTC absente ou corrompue (démarrage à froid) | 0 |
| `kNotRunning` | la pompe n'était pas commandée ON avant la coupure | 0 |
| `kNotManaged` | mode d'installation (mémorisé ou courant) non géré | 0 |
| `kNotInitialRun` | ce n'est pas le tout premier démarrage de l'uptime (protège un futur appelant — inatteignable depuis `begin()` aujourd'hui) | 0 |
| `kDenied` | `measure_validity` a refusé la dispense (`verdict.waive == false`) | 0 |
| `kImplausible` | report au-delà de `kFiltrationResumeMaxPlausibleS` (24 h — **retour à ZÉRO, jamais écrêtage** : un report dont on doute n'est pas réduit, il est jeté) | 0 |
| `kResumed` | toutes les conditions réunies | `record.carryS` |

⚠️ **La coupure n'est JAMAIS créditée** : `carryS` ne porte que des secondes
réellement mesurées — `evaluateFiltrationResume()` ne fait **jamais**
`carryS + verdict.outageS`. Sous-estimer est sans danger, sur-estimer lève une
garde de dosage.

### Ordre d'écriture dans `begin()` — pourquoi il compte

```cpp
state.carryMs = resume.carryS * 1000UL;
state.startedAtMs = millis();
if (state.startedAtMs == 0) state.startedAtMs = 1;  // protège la sentinelle "arrêt"
state.running = true;  // SEULEMENT MAINTENANT
```

Un lecteur concurrent (handler HTTP async) qui verrait `running == true` avant
que `carryMs`/`startedAtMs` ne soient posés obtiendrait `millis() − 0`, soit
**l'uptime** — exactement le défaut corrigé par ce lot, revenu par la petite
porte. Même ordre aux trois sites qui touchent ce triplet (`begin()`,
démarrage en `update()`, arrêt en `update()` — les deux chemins d'arrêt,
`managed` et non-`managed`) : écrire `running` **en dernier** au démarrage,
**en premier** à l'arrêt.

### ⚠️ Dépendance d'ordre dans `main.cpp` — `measureValidity.begin()` AVANT `filtration.begin()`

`filtration.begin()` consomme le verdict produit par `measureValidity.begin()`
— cet ordre est commenté **des deux côtés** dans `main.cpp`. Si inversé par
erreur, le drapeau « verdict produit » de `MeasureValidityManager` reste
`false` : `filtration.begin()` refuse le recrédit et journalise un
`critical`, plutôt que de deviner un verdict. Voir [measure-validity.md](measure-validity.md#api-publique).

### Logs (préfixe `[Filtration]`, feature-158)

| Événement | Niveau | Message (forme) |
|---|---|---|
| Reprise accordée | INFO | `[Filtration] Reprise après redémarrage : N s de filtration recréditées (coupure M s, dispensée) — garde pH de 10 min NON réarmée` |
| Reprise refusée | INFO | `[Filtration] Reprise à zéro (<motif>) — la garde pH de 10 min s'applique` |
| Ordre d'initialisation invalide | CRITICAL | `[Filtration] Ordre d'initialisation invalide : measureValidity.begin() n'a pas encore produit son verdict de continuité — reprise refusée (0 s recrédités)` |

Une succession de lignes « Reprise après redémarrage » au journal est un
**signal à regarder** (boucle de redémarrages rapprochés), pas une garde
automatique — le gate `pool-chemistry` a **examiné et refusé** un plafond du
nombre de reports consécutifs : il bloquerait le cas légitime (plusieurs OTA
dans le même cycle de filtration), même travers que le quota « une dispense
par heure » retiré par feature-139.

### Dépendance *load-bearing* côté garde pH

Depuis ce lot, le refus n°2 (« filtration pas encore établie ») de
`evaluatePhTune()` peut être franchi dès les premières secondes après un boot
si un report est accordé. Le refus n°1 (`isnan(phValueRaw)`, mesure
indisponible) devient donc la **seule** protection des ~45 premières secondes
d'un boot pour cette garde — l'ordre des deux refus dans
[`src/ph_tune_logic.h`](../../src/ph_tune_logic.h) est **load-bearing** et ne
doit jamais être inversé. Voir [page-ph.md](../features/page-ph.md#les-cinq-états--prêt-ou-lun-des-quatre-refus).

### Cas limites

- **Modes « Marche » (forcé) et Boost** : la reprise **s'applique** aussi —
  tranché **contre** la proposition initiale de la spec par le gate
  `pool-chemistry` : « l'enregistrement porte une durée **mesurée** de
  circulation, pas une inférence administrative ; la physique ne lit pas le
  libellé du mode. » Mais le Boost, lui, ne peut **jamais** consommer de
  report : il ne repasse pas par `begin()` (condition 3, `kNotInitialRun`).
- **Démarrage à froid** (coupure secteur, mémoire RTC absente) ou **arrêt
  d'eau plus long que la grâce** (`kWaterStopGraceS`) : comportement
  **inchangé** — la durée repart de zéro, la garde pH s'applique pleinement.
- **Modes Powered/External** : aucun crédit (`kNotManaged`), que le mode
  mémorisé ou le mode courant ait changé.
- **Repliement de `millis()` à 2³² ms (~49,7 jours)** : `computeFiltrationElapsedMs()`
  reste wrap-safe par soustraction non signée 32 bits — jamais protégée par un
  `if (nowMs < startedAtMs)`, qui détruirait le repliement au lieu de le
  gérer.

### ⚠️ Piège de recette — le premier redémarrage après l'OTA ne peut pas bénéficier du correctif

La zone `RTC_NOINIT_ATTR` de `FiltrationRtcRecord` est **neuve** à la livraison
de ce lot : au premier redémarrage qui suit le déploiement, magic/CRC sont
invalides ⇒ `kNoRecord` ⇒ fail-closed, « Reprise à zéro (mémoire de
filtration absente ou corrompue) ». **Ce n'est pas un échec, c'est la preuve
du fail-closed** — la vérification se fait au **second** redémarrage.

## Interaction avec les autres composants

- **`pump_controller`** :
  - Démarrage filtration → `PumpController.armStabilizationTimer()` (timer configurable, défaut 5 min) → bloque les injections pendant la stabilisation. **Uniquement en mode `managed`** : en `powered` / `external` le relais n'est pas piloté, donc le timer de stabilisation au démarrage filtration n'est jamais armé (feature-056).
  - Arrêt filtration → `PumpController.clearStabilizationTimer()`.
  - Présence d'eau : `pump_controller` consomme désormais `filtration.resolveWaterPresence().waterPresent` (**source unique**, feature-056), et non plus `filtration.isRunning()` directement. En mode `scheduled`, l'horizon de répartition (`filtrationDosingHorizonMinutes()`, feature-146) vaut `1440 − nowMin` hors `managed`, **et aussi en mode Marche** (le créneau start/end ne décide plus rien dans ce mode — voir [pump-controller.md](pump-controller.md#mode-scheduled)).
- **`mqtt_manager`** : `publishFiltrationState()` publie `{base}/filtration_state` (`ON`/`OFF`) et `{base}/filtration_mode`. Le switch HA `{base}/filtration/set` (ON/OFF) pilote désormais le **mode** (Marche/Arrêt), plus un ordre direct séparé (feature-146).

## Gestion des horaires traversant minuit

`isMinutesInRange(now, start, end)` tolère les plages `start > end` (ex. 22:00 → 06:00).

## Mode d'installation et présence d'eau (feature-056)

Le **mode d'installation** (`InstallMode`, [ADR-0026](../adr/0026-mode-installation.md)) décrit le câblage réel et pilote la présence d'eau, le relais et l'horizon de dosage. Il remplace les anciens `regulationMode` (`pilote`/`continu`) **et** `filtrationCfg.enabled` (fusion).

### Résolution de la présence d'eau — source unique

`resolveWaterPresence()` est la **source unique** consommée par toutes les gardes de dosage (`canDose`, injection manuelle, monitor d'injection en cours). La coquille collecte les entrées (mode, `state.running`, triplet externe lu sous lock, âge wrap-safe, `kExternalFiltrationStaleMs`) et délègue à la fonction **pure** `resolveWaterPresent(WaterPresenceInputs)` ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp), pattern Humble Object [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)). **Fail-closed strict** :

| Mode | Sérialisé | Eau présente si… | Relais piloté |
|------|-----------|------------------|---------------|
| `ManagedFiltration` | `managed` | `filtration.isRunning()` (état commandé par PC) | oui (GPIO 26) |
| `PoweredByFiltration` | `powered` | toujours `true` (PC vivant ⟺ eau) | non |
| `ExternalFiltration` | `external` | `externalKnown && age ≤ 180 s && externalOn` | non |

Mode inconnu → `false` (fail-closed).

### État de la filtration externe (mode `external`)

Le signal externe est stocké dans le triplet `{_externalOn, _externalLastMs, _externalKnown}`, protégé par un **portMUX** (`portENTER_CRITICAL` / `portEXIT_CRITICAL`) — écrivable sans risque depuis un handler HTTP (`POST /filtration/external-state`) ou un callback MQTT async (`{base}/filtration_external_state/set`), tous deux hors `loopTask`.

- `setExternalState(bool)` : horodate (`millis()`) avant la section critique, écrit le triplet sous lock, logue après (`info` `[Filtration externe] État signalé : ON|OFF`). Le flag `_externalKnown` passe à `true` au premier signal.
- `getExternalState(on, lastMs, known)` : copie minimale sous lock ; l'âge (`now − lastMs`, wrap-safe) est calculé **hors** lock par `resolveWaterPresence()`.
- **Aucune persistance NVS** : au boot, `_externalKnown == false` → présence d'eau `false` (fail-safe OFF). Au-delà de `kExternalFiltrationStaleMs = 180 000 ms` (3 min) sans nouveau signal → périmé (`stale`) → présence d'eau `false`. C'est la condition de sécurité clé (pool-chemistry) qui évite l'injection dans une eau stagnante.

### Migration

`migrateInstallMode(regMode, filtrationEnabled)` (one-shot au boot, [`config.cpp`](../../src/config.cpp)) : `reg_mode == "continu"` → `powered`, sinon → `managed`. `external` n'est jamais produit par migration. **`filt_enabled` est ignoré** — voir le cas de bord relais dans [UPDATE_GUIDE.md](../UPDATE_GUIDE.md).

## Logique d'horaire pure (`schedule_logic`)

[feature-038](../../specs/features/done/feature-038-filtration-horaire-testable.md) applique le pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), **pas de nouvel ADR**) à la décision d'horaire. Toute l'arithmétique de planning est extraite de `filtration.cpp` vers un module **pur** [`src/schedule_logic.h`](../../src/schedule_logic.h) / [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — sans `<Arduino.h>`, sans RTC, sans `millis()`, sans NVS, sans FreeRTOS, sans `String` (en-têtes C `<stdint.h>`/`<math.h>` pour compiler en natif). C'est un *characterization refactor* : **aucun comportement n'a changé** (équivalence stricte validée par `pool-chemistry`, dont la préservation de la garde « présence d'eau » de `canDose()` côté `pump_controller`).

Module **générique** (pas spécifique filtration) : conçu pour être réutilisé par l'éclairage ou tout autre planning horaire ultérieur.

### Les fonctions pures

| Fonction | Rôle | Sémantique |
|----------|------|------------|
| `int timeStringToMinutes(const char* hhmm)` | Parse `"HH:MM"` → minutes depuis minuit | longueur < 5 ou `[2] != ':'` → `-1` ; `hh` hors `[0,23]` ou `mm` hors `[0,59]` → `-1` ; sinon `hh*60+mm` |
| `bool isMinutesInRange(int now, int start, int end, bool equalMeansAlways = false)` | Appartenance à `[start, end)` | `start`/`end` à `-1` → `false` (garde **avant** le test `start==end`) ; `start==end` → `equalMeansAlways` (filtration `false`, éclairage `true` — voir feature-040) ; `start<end` → `now>=start && now<end` ; `start>end` (wrap minuit) → `now>=start \|\| now<end` |
| `ScheduleWindow computeAutoWindow(float tempC, float pivotHour)` | Créneau auto selon température | `tempC<0` ramené à 0 ; `durationHours = tempC/2` borné `[1,24]` ; centré sur `pivotHour` (`start = pivot - duration/2`, `end = start + duration`) ; **wrap des deux bornes dans `[0,24)`** ; conversion heure→minutes avec arrondi/carry identique à l'origine |
| `FiltrationMode parseFiltrationMode(const char* token)` (feature-146) | **Unique** site de correspondance jeton → sens | `"auto"`→`Auto`, `"manual"`→`Programme`, `"force"`→`Marche`, `"off"`→`Arret`, tout autre (ou `nullptr`) → `Inconnu` ; `token` doit arriver en minuscules |
| `RunWindow effectiveRunWindow(ScheduleWindow base, bool boostActive, int nowMin)` (feature-146) | Créneau **effectif** consommé par `decideFiltrationRun` | `!boostActive` → `base` inchangé ; `boostActive` → `{nowMin, 1440}` (élargi jusqu'au prochain minuit, expiration exacte du Boost). Type `RunWindow` **distinct** de `ScheduleWindow` : ne sert **qu'à** `decideFiltrationRun`, jamais à l'échantillonnage de température ni au planning annoncé ([ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) |
| `bool decideFiltrationRun(FiltrationMode mode, bool haveTime, int nowMin, int startMin, int endMin, bool currentlyRunning)` | Décision marche/arrêt — `switch` **exhaustif sans `default`** (feature-146) | `Auto`/`Programme` → `isMinutesInRange(...)` si `haveTime`, sinon **conserve `currentlyRunning`** ; `Marche` → `true` (sans expiration) ; `Arret` → `false` (sans expiration) ; `Inconnu` → `currentlyRunning` (fail-safe de décision) |
| `int remainingRangeMinutes(int nowMin, int startMin, int endMin)` (feature-011, v2.8.0) | Minutes restantes de la plage courante, **bornées à minuit** | hors plage ou plage invalide (`-1`, `start==end`) → `0` (délègue la garde à `isMinutesInRange`) ; `start<end` → `end − nowMin` ; plage à cheval sur minuit : partie du soir (`nowMin >= start`) → `1440 − nowMin` (**borné à minuit** : les compteurs journaliers se réinitialisent à minuit, l'horizon de répartition ne le franchit jamais), partie du matin (`nowMin < end`) → `end − nowMin` ; toujours ≥ 1 quand `nowMin` est dans la plage (fin exclusive) |

> `remainingRangeMinutes` sert d'**horizon de répartition** au mode `scheduled` des pompes doseuses ([ADR-0021](../adr/0021-repartition-scheduled.md), [pump-controller.md §Mode scheduled](pump-controller.md#mode-scheduled)) — la coquille `filtration.cpp` n'est **pas** modifiée par feature-011. feature-146 ajoute `filtrationDosingHorizonMinutes()` dans `pump_controller.cpp`, qui factorise l'appel et traite le mode Marche comme « journée entière » (même traitement que hors mode `managed`) — voir [pump-controller.md](pump-controller.md#mode-scheduled).

### Décision et temps indisponible

`decideFiltrationRun` traite les **quatre modes** explicitement, sans priorité à négocier entre eux : `Marche` → marche sans condition, `Arrêt` → arrêt sans condition, `Auto`/`Programme` → la **plage horaire** décide. Lorsque l'heure n'est pas disponible (`haveTime=false`, avant NTP/RTC, pendant OTA…), la fonction **renvoie `currentlyRunning` tel quel** en `Auto`/`Programme` (pas de faux démarrage/arrêt, l'état du relais est conservé jusqu'à resynchronisation) — et aussi en mode `Inconnu` (fail-safe de décision, cas normalement inatteignable après `ensureTimesValid()`). Le `mode` doit arriver en minuscules — la coquille applique `toLowerCase()` avant délégation à `parseFiltrationMode()`.

⚠️ **Le Mode Boost (feature-053) ne participe plus à cette décision** (feature-146,
[ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) : il modifie le créneau
**en amont**, via `effectiveRunWindow()`, résolu par la coquille `filtration.cpp` avant l'appel.
Disponible dans tous les modes **sauf Arrêt** (surchloration : il faut de l'eau qui circule) —
passer en mode Arrêt **efface** un Boost actif (`stopBoost()`), ce n'est pas une simple mise en
veille.

### Fenêtre auto centrée sur le pivot

`computeAutoWindow` reproduit le calcul historique : durée = température / 2 (heures), bornée à `[1, 24]`, **centrée** sur `kFiltrationPivotHour = 13.0f` ([`constants.h:78`](../../src/constants.h:78)). Les deux bornes sont **wrappées dans `[0, 24)`**, ce qui peut produire une fenêtre traversant minuit (gérée par `isMinutesInRange`). La conversion heure flottante → minutes reproduit l'arrondi et le carry (`m>=60`) de l'implémentation d'origine.

### Coquille `filtration.cpp`

`begin()`, `update()` et `computeAutoSchedule()` deviennent des **coquilles minces** : elles collectent les entrées (heure RTC, `millis()`, config NVS, température), délèguent aux fonctions pures, puis appliquent les effets de bord. Restent **exclusivement dans la coquille** : lecture RTC/heure système, `millis()`, persistance NVS, **deadband 1 °C**, temporisation de stabilisation, commande relais (`digitalWrite`) et `publishState()`. La garde « présence d'eau » (`canDose()` exige `filtration.isRunning()`) est **strictement préservée**. ⚠️ Le **timeout 4 h** de l'ancien ordre direct a **disparu** avec lui (feature-146, v2.63.0) — Marche et Arrêt n'expirent plus.

### Testabilité native

`schedule_logic.cpp` est couvert à **100 % des lignes** par la suite Unity native. Le temps, la température et l'état courant étant injectés en paramètres, chaque branche (parsing invalide, wrap minuit, durée bornée, quatre modes, conservation d'état sans heure) est exercée sans matériel ni attente réelle. feature-146 (v2.63.0) remplace les anciens tests de priorité `forceOn`/`forceOff`/`boostForce` par une **matrice complète** sur `decideFiltrationRun` (4 modes × dans/hors créneau × heure connue/inconnue × état courant), plus des tests dédiés `parseFiltrationMode` et `effectiveRunWindow` ([`test/test_native_schedule/test_schedule_logic.cpp`](../../test/test_native_schedule/test_schedule_logic.cpp)). Voir [BUILD.md](../BUILD.md) pour `pio test -e native` (nombre de suites et de cas : mesurer, ne pas recopier un chiffre périmé — voir `CLAUDE.md`).

> ⚠️ **Invariant** : toute évolution future de la décision d'horaire **passe par `schedule_logic`** et doit conserver l'équivalence stricte verrouillée par les tests natifs.

## Cas limites

- **Sans heure système valide** (avant NTP/RTC) : `getCurrentMinutesOfDay()` échoue → pas de démarrage/arrêt automatique en Auto/Programme. La filtration reste à l'état où le relais l'a laissé. Récupération dès que l'heure se synchronise.
- **Mode Arrêt** : le relais est forcé à LOW, aucune dérogation possible — y compris pour le Mode Boost, qui s'efface en entrant dans ce mode.
- **Marche et Arrêt ne sont plus temporaires** (feature-146, v2.63.0) : ils **survivent à un redémarrage** et durent jusqu'au prochain changement de mode explicite — pas de minuteur, pas de retour automatique. ⚠️ Un appareil laissé en Marche consomme en continu ; un appareil laissé en Arrêt pendant l'hivernage peut priver de protection hors-gel un montage où la pompe de filtration est le seul moyen de faire circuler l'eau.
- **Mode inconnu** (config corrompue, client legacy) : replié sur Auto par `ensureTimesValid()`, journalisé — voir plus haut.
- **Migration `force` → Marche** (feature-146) : un appareil qui redémarre avec `mode = "force"` passait auparavant par le `return false` par défaut de l'ancien `decideFiltrationRun()` (pompe **arrêtée**, un bug) ; il bascule désormais en **Marche** (pompe **tournante**) — `begin()` journalise un `warning` et publie une alerte MQTT `filtration_mode_force_migrated` au premier tour connecté.

## Endpoints HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Changer de mode (Auto/Programme/Marche/Arrêt), dont Démarrer/Arrêter | `POST /save-config` `{filtration_mode: "auto"\|"manual"\|"force"\|"off"}` (feature-146 — plus de route `/filtration/on`/`/off` dédiée) | CRITICAL |
| Signaler l'état d'une filtration externe | `POST /filtration/external-state?running=…` | WRITE |
| Sauvegarder mode/horaires + `install_mode` | `POST /save-config` | CRITICAL |

Voir [`web_routes_control.cpp`](../../src/web_routes_control.cpp) (`/filtration/external-state`) et [`web_routes_config.cpp`](../../src/web_routes_config.cpp) (`/save-config`, validation serveur du mode via `parseFiltrationMode()` — un jeton non reconnu est refusé, `warning`, le reste de la requête s'applique). `POST /filtration/external-state` accepte `running` en corps de formulaire OU en query (pas de JSON) — voir [API.md](../API.md#post-filtrationexternal-state--write).

## Fichiers liés

- [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- [`src/filtration_resume_logic.h`](../../src/filtration_resume_logic.h), [`src/filtration_resume_logic.cpp`](../../src/filtration_resume_logic.cpp) — reprise de la durée de filtration, logique pure (feature-158)
- [`src/constants.h`](../../src/constants.h) — `kFiltrationRelayPin = 26` (PCB v2)
- [`src/constants.h:78`](../../src/constants.h:78) — `kFiltrationPivotHour`
- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — logique d'horaire pure (feature-038), générique filtration/éclairage
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — `InstallMode`, `resolveWaterPresent`, `migrateInstallMode` (feature-056)
- [`src/ph_tune_logic.h`](../../src/ph_tune_logic.h) — ordre des refus de la correction pH, *load-bearing* depuis feature-158
- [`src/constants.h`](../../src/constants.h) — `kExternalFiltrationStaleMs = 180000` (fraîcheur signal externe)
- [ADR-0104](../adr/0104-la-continuite-de-l-eau-a-un-producteur-unique-et-des-consommateurs.md) — la continuité de l'eau a un producteur unique (measure_validity) et des consommateurs (filtration), feature-158, v2.74.0
- [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md) — le mode de filtration est la seule autorité (feature-146, v2.63.0, remplace l'ADR-0025 Mode Boost)
- [ADR-0026](../adr/0026-mode-installation.md) — mode d'installation (3 archétypes) + résolution de la présence d'eau
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — logique métier pure (Humble Object) pour testabilité native
- [ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md) — planning auto figé à l'arrêt du cycle, promu à minuit (bug-126)
- [ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md) — sonde de température figée, statu quo instrumenté (amendé par bug-126)
- [page-filtration.md](../features/page-filtration.md) — UI correspondante
- [page-ph.md](../features/page-ph.md) — carte « Correction manuelle de la mesure », garde des 10 min consommant `filtrationElapsedMs()`
- [pump-controller.md](pump-controller.md) — interaction régulation (`canDose()` requiert filtration active)
- [measure-validity.md](measure-validity.md) — producteur du verdict de continuité de l'eau consommé par `filtration.begin()`
- [`test/test_native_filtration_resume/`](../../test/test_native_filtration_resume/) — tests natifs de `filtration_resume_logic`
