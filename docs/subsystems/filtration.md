# Subsystem — `filtration`

- **Fichiers** : [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- **Singleton** : `extern FiltrationManager filtration;`
- **GPIO relais** : `kFiltrationRelayPin = 26` (PCB v2, [`constants.h`](../../src/constants.h)) — actif haut. Voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md).

## Rôle

Pilote le relais de la pompe de filtration. Trois modes coexistent : `auto` (horaire calculé selon température de l'eau), `manual` (horaire fixe), `off` (arrêt permanent). Un **forçage temporaire** (`force_on` / `force_off`) permet de déroger à la planification sans modifier la config ; il est perdu au redémarrage.

## API publique

```cpp
void begin();
void update();              // appelé dans loop()
bool isRunning() const;
bool getRelayState() const;
void computeAutoSchedule(); // recalcul horaire auto selon température
void ensureTimesValid();
void publishState();
```

## Algorithme auto

`computeAutoSchedule()` calcule `filtration_start` / `filtration_end` à partir de :
- la température de l'eau mesurée (`sensors.getTemperature()`),
- le pivot horaire `kFiltrationPivotHour = 13.0f` ([`constants.h:78`](../../src/constants.h:78)),
- une formule température → durée (voir [`filtration.cpp`](../../src/filtration.cpp)).

**Deadband 1 °C** : `_lastScheduledTemp` évite les recalculs pour une variation < 1 °C. Le rafraîchissement peut survenir après 5 min d'eau en circulation (laisse la température se stabiliser).

## État interne

```cpp
struct FiltrationRuntime {
  bool running = false;
  bool scheduleComputedThisCycle = false;
  unsigned long startedAtMs = 0;
  unsigned long lastStoppedAtMs = 0;   // 0 si jamais arrêtée
  unsigned long forceOnStartMs = 0;    // 0 si inactif
  unsigned long forceOffStartMs = 0;
};
```

## Interaction avec les autres composants

- **`pump_controller`** :
  - Démarrage filtration → `PumpController.armStabilizationTimer()` (timer configurable, défaut 5 min) → bloque les injections pendant la stabilisation.
  - Arrêt filtration → `PumpController.clearStabilizationTimer()`.
  - Mode `scheduled` : c'est le `pump_controller` qui vérifie `filtration.isRunning()` pour décider d'injecter.
- **`mqtt_manager`** : `publishFiltrationState()` publie `{base}/filtration_state` (`ON`/`OFF`) et `{base}/filtration_mode`.

## Gestion des horaires traversant minuit

`isMinutesInRange(now, start, end)` tolère les plages `start > end` (ex. 22:00 → 06:00).

## Logique d'horaire pure (`schedule_logic`)

[feature-038](../../specs/features/doing/feature-038-filtration-horaire-testable.md) applique le pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), **pas de nouvel ADR**) à la décision d'horaire. Toute l'arithmétique de planning est extraite de `filtration.cpp` vers un module **pur** [`src/schedule_logic.h`](../../src/schedule_logic.h) / [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — sans `<Arduino.h>`, sans RTC, sans `millis()`, sans NVS, sans FreeRTOS, sans `String` (en-têtes C `<stdint.h>`/`<math.h>` pour compiler en natif). C'est un *characterization refactor* : **aucun comportement n'a changé** (équivalence stricte validée par `pool-chemistry`, dont la préservation de la garde « présence d'eau » de `canDose()` côté `pump_controller`).

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

`begin()`, `update()` et `computeAutoSchedule()` deviennent des **coquilles minces** : elles collectent les entrées (heure RTC, `millis()`, config NVS, température), délèguent aux fonctions pures, puis appliquent les effets de bord. Restent **exclusivement dans la coquille** : lecture RTC/heure système, `millis()`, persistance NVS, **deadband 1 °C**, **timeout 4 h**, temporisation de stabilisation, commande relais (`digitalWrite`), `publishState()` et événements UART. La garde « présence d'eau » (`canDose()` exige `filtration.isRunning()`) est **strictement préservée**.

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
| Sauvegarder mode/horaires | `POST /save-config` | CRITICAL |

Voir [`web_routes_control.cpp`](../../src/web_routes_control.cpp).

## Fichiers liés

- [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- [`src/constants.h`](../../src/constants.h) — `kFiltrationRelayPin = 26` (PCB v2)
- [`src/constants.h:78`](../../src/constants.h:78) — `kFiltrationPivotHour`
- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — logique d'horaire pure (feature-038), générique filtration/éclairage
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — logique métier pure (Humble Object) pour testabilité native
- [page-filtration.md](../features/page-filtration.md) — UI correspondante
- [pump-controller.md](pump-controller.md) — interaction régulation (`canDose()` requiert filtration active)
