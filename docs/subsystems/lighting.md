# Subsystem — `lighting`

- **Fichiers** : [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- **Singleton** : `extern LightingManager lighting;`
- **GPIO relais** : `kLightingRelayPin = 27` (PCB v2, [`constants.h`](../../src/constants.h)) — actif haut. Voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md).

## Rôle

Pilote le relais de l'éclairage de la piscine en **tout-ou-rien** (LOW / HIGH). Supporte un allumage/extinction manuel ou une programmation horaire.

## API publique

```cpp
void begin();
void update();              // appelé dans loop()
bool isOn() const;
bool getRelayState() const;
void setManualOn();
void setManualOff();
void clearManualOverride();
void ensureTimesValid();
void publishState();
```

## État interne

```cpp
struct LightingRuntime {
  bool manualOverride = false;
  unsigned long manualSetAtMs = 0;
};
```

Le `manualOverride` prend le pas sur la programmation horaire jusqu'à sa réinitialisation (reboot ou nouvelle commande manuelle dans la plage programmée).

## Config (`LightingConfig` — [`config.h:101`](../../src/config.h:101))

- `feature_enabled` — active / masque la fonctionnalité.
- `schedule_enabled` — programmation horaire active.
- `start` / `end` (HH:MM) — plage d'allumage.
- `brightness` (0-255, PWM) — **présent mais non exposé dans l'UI à date** (relais tout-ou-rien).

## Endpoints HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Allumer | `POST /lighting/on` | WRITE |
| Éteindre | `POST /lighting/off` | WRITE |
| Sauvegarder programmation | `POST /save-config` | CRITICAL |

## MQTT / Home Assistant

Entité : `switch.*_lighting` — topic état `{base}/lighting_state`, commande `{base}/lighting/set`.

## Cas limites

- **`feature_enabled = false`** : le relais est forcé à LOW, UI masquée.
- **Programmation `start == end`** : aucun allumage (voir `isMinutesInRange()` dans [`lighting.cpp`](../../src/lighting.cpp)).
- **Plage traversant minuit** (`start > end`, ex. 22:00 → 06:00) : gérée.

## Fichiers liés

- [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- [`src/constants.h`](../../src/constants.h) — `kLightingRelayPin = 27` (PCB v2)
- [`src/config.h:101`](../../src/config.h:101) — struct `LightingConfig`
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [page-lighting.md](../features/page-lighting.md) — UI correspondante
