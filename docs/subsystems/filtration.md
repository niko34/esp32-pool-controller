# Subsystem — `filtration`

- **Fichiers** : [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp)
- **Singleton** : `extern FiltrationManager filtration;`
- **GPIO relais** : `FILTRATION_RELAY_PIN = 25` ([`config.h:19`](../../src/config.h:19))

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
- [`src/constants.h:78`](../../src/constants.h:78) — `kFiltrationPivotHour`
- [page-filtration.md](../features/page-filtration.md) — UI correspondante
- [pump-controller.md](pump-controller.md) — interaction régulation (`canDose()` requiert filtration active)
