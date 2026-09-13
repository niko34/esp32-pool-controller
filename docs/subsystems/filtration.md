# Subsystem — `filtration`

- **Fichiers** : [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- **Singleton** : `extern FiltrationManager filtration;`
- **GPIO relais** : `kFiltrationRelayPin = 26` (PCB v2, [`constants.h`](../../src/constants.h)) — actif haut. Voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md).

## Rôle

Pilote le relais de la pompe de filtration. Trois modes de **planification** coexistent : `auto` (horaire calculé selon température de l'eau), `manual` (horaire fixe), `off` (arrêt permanent). Un **forçage temporaire** (`force_on` / `force_off`) permet de déroger à la planification sans modifier la config ; il est perdu au redémarrage.

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

// feature-056 — mode d'installation ExternalFiltration
void setExternalState(bool running);                              // signal externe (HTTP/MQTT) — non bloquant (portMUX)
void getExternalState(bool& on, uint32_t& lastMs, bool& known) const;  // copie le triplet sous lock
WaterPresence resolveWaterPresence() const;                        // SOURCE UNIQUE présence d'eau (toutes gardes de dosage)
```

## Algorithme auto — planning figé à l'arrêt, promu à minuit (bug-126, [ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md))

⚠️ **Le planning auto n'est plus jamais écrit pendant que la pompe tourne.** Avant bug-126, `computeAutoSchedule()` était rappelée 5 min après le démarrage du cycle ; une température en baisse pouvait rétracter la fenêtre et arrêter le cycle qui venait de produire la mesure (`decideFiltrationRun()` étant une décision de **niveau**, pas de front). Deux moments seulement modifient désormais l'état :

1. **Pendant le cycle**, `sampleCycleTemperature()` échantillonne la température toutes les `kFiltrationTempSampleIntervalMs` (1 min) et l'intègre dans une **moyenne incrémentale** (`CycleTempAccumulator`, [`schedule_logic.h`](../../src/schedule_logic.h)) — **jamais une somme** (`mean += (x - mean) / count`), pour rester stable en mode `PoweredByFiltration` (eau présente 24/7). Un échantillon n'est retenu que si les **trois conditions cumulatives** de `isSampleEligible()` tiennent : purge terminée (`measureValidity.isCirculationSettled()`), horaire en plage (ferme un `forceOn` nocturne sans code dédié), sonde eau non figée (`!sensors.isTemperatureFrozen()`). La source est **exclusivement** `sensors.getWaterTemperature()` (offset de calibration borné appliqué, bug-114), avec un repli de tolérance `selectFiltrationRefTemp()` pour un appareil neuf sans rôle de sonde attribué — **jamais** `sensors.getTemperature()`, qui retomberait sur une sonde `Circuit` (boîtier électronique).
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

## État interne

```cpp
struct FiltrationRuntime {
  bool running = false;
  unsigned long startedAtMs = 0;
  unsigned long lastStoppedAtMs = 0;   // 0 si jamais arrêtée
  unsigned long forceOnStartMs = 0;    // 0 si inactif
  unsigned long forceOffStartMs = 0;
};
```

`scheduleComputedThisCycle` (recalcul en marche) est **retiré** de cette structure par bug-126 — devenu sans objet, le recalcul en marche ayant disparu.

## Interaction avec les autres composants

- **`pump_controller`** :
  - Démarrage filtration → `PumpController.armStabilizationTimer()` (timer configurable, défaut 5 min) → bloque les injections pendant la stabilisation. **Uniquement en mode `managed`** : en `powered` / `external` le relais n'est pas piloté, donc le timer de stabilisation au démarrage filtration n'est jamais armé (feature-056).
  - Arrêt filtration → `PumpController.clearStabilizationTimer()`.
  - Présence d'eau : `pump_controller` consomme désormais `filtration.resolveWaterPresence().waterPresent` (**source unique**, feature-056), et non plus `filtration.isRunning()` directement. En mode `scheduled`, l'horizon de répartition vaut `1440 − nowMin` hors `managed` (voir [pump-controller.md](pump-controller.md#mode-scheduled)).
- **`mqtt_manager`** : `publishFiltrationState()` publie `{base}/filtration_state` (`ON`/`OFF`) et `{base}/filtration_mode`.

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
| `bool decideFiltrationRun(mode, forceOn, forceOff, haveTime, nowMin, startMin, endMin, currentlyRunning)` | Décision marche/arrêt | priorités **`forceOn` > `forceOff` > plage horaire** ; `mode` (en minuscules) `manual`/`auto` → `isMinutesInRange(...)` si `haveTime`, sinon **conserve `currentlyRunning`** ; autre mode → `false` |
| `int remainingRangeMinutes(int nowMin, int startMin, int endMin)` (feature-011, v2.8.0) | Minutes restantes de la plage courante, **bornées à minuit** | hors plage ou plage invalide (`-1`, `start==end`) → `0` (délègue la garde à `isMinutesInRange`) ; `start<end` → `end − nowMin` ; plage à cheval sur minuit : partie du soir (`nowMin >= start`) → `1440 − nowMin` (**borné à minuit** : les compteurs journaliers se réinitialisent à minuit, l'horizon de répartition ne le franchit jamais), partie du matin (`nowMin < end`) → `end − nowMin` ; toujours ≥ 1 quand `nowMin` est dans la plage (fin exclusive) |

> `remainingRangeMinutes` sert d'**horizon de répartition** au mode `scheduled` des pompes doseuses ([ADR-0021](../adr/0021-repartition-scheduled.md), [pump-controller.md §Mode scheduled](pump-controller.md#mode-scheduled)) — la coquille `filtration.cpp` n'est **pas** modifiée par feature-011.

### Décision et temps indisponible

`decideFiltrationRun` applique les priorités dans l'ordre : un **forçage `force_on`** force la marche, sinon un **forçage `force_off`** force l'arrêt, sinon la **plage horaire** (modes `manual`/`auto`) décide. Lorsque l'heure n'est pas disponible (`haveTime=false`, avant NTP/RTC, pendant OTA…) et sans forçage actif, la fonction **renvoie `currentlyRunning` tel quel** : pas de faux démarrage/arrêt, l'état du relais est conservé jusqu'à resynchronisation. Le `mode` doit arriver en minuscules — la coquille applique `toLowerCase()` avant délégation.

### Fenêtre auto centrée sur le pivot

`computeAutoWindow` reproduit le calcul historique : durée = température / 2 (heures), bornée à `[1, 24]`, **centrée** sur `kFiltrationPivotHour = 13.0f` ([`constants.h:78`](../../src/constants.h:78)). Les deux bornes sont **wrappées dans `[0, 24)`**, ce qui peut produire une fenêtre traversant minuit (gérée par `isMinutesInRange`). La conversion heure flottante → minutes reproduit l'arrondi et le carry (`m>=60`) de l'implémentation d'origine.

### Coquille `filtration.cpp`

`begin()`, `update()` et `computeAutoSchedule()` deviennent des **coquilles minces** : elles collectent les entrées (heure RTC, `millis()`, config NVS, température), délèguent aux fonctions pures, puis appliquent les effets de bord. Restent **exclusivement dans la coquille** : lecture RTC/heure système, `millis()`, persistance NVS, **deadband 1 °C**, **timeout 4 h**, temporisation de stabilisation, commande relais (`digitalWrite`) et `publishState()`. La garde « présence d'eau » (`canDose()` exige `filtration.isRunning()`) est **strictement préservée**.

### Testabilité native

`schedule_logic.cpp` est couvert à **100 % des lignes** par la suite Unity native (19 tests dédiés, **70 tests au total** à la feature-038). Le temps, la température et l'état courant étant injectés en paramètres, chaque branche (parsing invalide, wrap minuit, durée bornée, priorités de forçage, conservation d'état sans heure) est exercée sans matériel ni attente réelle. feature-011 ajoute **5 tests `remainingRangeMinutes`** ([`test/test_native_schedule/test_schedule_logic.cpp`](../../test/test_native_schedule/test_schedule_logic.cpp)) — 152 tests au total en v2.8.0. Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

> ⚠️ **Invariant** : toute évolution future de la décision d'horaire **passe par `schedule_logic`** et doit conserver l'équivalence stricte verrouillée par les tests natifs.

## Cas limites

- **Sans heure système valide** (avant NTP/RTC) : `getCurrentMinutesOfDay()` échoue → pas de démarrage automatique. La filtration reste à l'état où le relais l'a laissé. Récupération dès que l'heure se synchronise.
- **Mode `off`** : le relais est forcé à LOW, aucune dérogation possible.
- **Forçage expiré au reboot** : `force_on` / `force_off` ne sont pas persistés.

## Endpoints HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Démarrer manuellement | `POST /filtration/on` | WRITE |
| Arrêter manuellement | `POST /filtration/off` | WRITE |
| Signaler l'état d'une filtration externe | `POST /filtration/external-state?running=…` | WRITE |
| Sauvegarder mode/horaires + `install_mode` | `POST /save-config` | CRITICAL |

Voir [`web_routes_control.cpp`](../../src/web_routes_control.cpp). `POST /filtration/external-state` accepte `running` en corps de formulaire OU en query (pas de JSON) — voir [API.md](../API.md#post-filtrationexternal-state--write).

## Fichiers liés

- [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- [`src/constants.h`](../../src/constants.h) — `kFiltrationRelayPin = 26` (PCB v2)
- [`src/constants.h:78`](../../src/constants.h:78) — `kFiltrationPivotHour`
- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — logique d'horaire pure (feature-038), générique filtration/éclairage
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — `InstallMode`, `resolveWaterPresent`, `migrateInstallMode` (feature-056)
- [`src/constants.h`](../../src/constants.h) — `kExternalFiltrationStaleMs = 180000` (fraîcheur signal externe)
- [ADR-0026](../adr/0026-mode-installation.md) — mode d'installation (3 archétypes) + résolution de la présence d'eau
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — logique métier pure (Humble Object) pour testabilité native
- [ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md) — planning auto figé à l'arrêt du cycle, promu à minuit (bug-126)
- [ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md) — sonde de température figée, statu quo instrumenté (amendé par bug-126)
- [page-filtration.md](../features/page-filtration.md) — UI correspondante
- [pump-controller.md](pump-controller.md) — interaction régulation (`canDose()` requiert filtration active)
