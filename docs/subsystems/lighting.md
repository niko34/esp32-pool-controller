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

## Logique d'horaire pure (`schedule_logic`)

[feature-040](../../specs/features/doing/feature-040-eclairage-horaire-testable.md) applique le pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), **pas de nouvel ADR**) à la décision d'horaire de l'éclairage, en **réutilisant** le module pur [`src/schedule_logic.h`](../../src/schedule_logic.h) / [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) créé pour la filtration (feature-038). C'est un *characterization refactor* : **aucun comportement observable n'a changé** (mêmes décisions allumage/extinction pour les mêmes entrées). `lighting.cpp::update()` devient une **coquille mince** qui délègue à `decideLightingOn(...)` ; RTC/`getLocalTime`, `millis()` (timeout manuel), `digitalWrite`, `publishState()`/MQTT restent dans la coquille.

### Fonctions pures utilisées

| Fonction | Rôle |
|----------|------|
| `int timeStringToMinutes(const char* hhmm)` | Parse `"HH:MM"` → minutes depuis minuit (réutilisée à l'identique de la filtration). |
| `bool isMinutesInRange(int now, int start, int end, bool equalMeansAlways = false)` | Appartenance à `[start, end)`. L'éclairage passe **`equalMeansAlways = true`**. |
| `bool decideLightingOn(manualOverride, enabledFlag, scheduleEnabled, haveTime, nowMin, startMin, endMin, currentlyOn)` | Décision marche/arrêt de l'éclairage (spécifique éclairage). |

`decideLightingOn` reproduit exactement la décision d'origine :

- `manualOverride` actif → suit `enabledFlag` (quel que soit l'horaire) ;
- sinon `scheduleEnabled` → `isMinutesInRange(nowMin, startMin, endMin, true)` si `haveTime`, **sinon conserve `currentlyOn`** (pas de faux toggle pendant un OTA / perte RTC) ;
- sinon → suit `enabledFlag`.

### Divergence intentionnelle `start == end`

Le paramètre `equalMeansAlways` de `isMinutesInRange` distingue les deux domaines sur la plage dégénérée `start == end` :

| Domaine | `equalMeansAlways` | `start == end` signifie |
|---------|--------------------|--------------------------|
| **Filtration** | `false` (défaut) | plage invalide → **jamais** (relais OFF) |
| **Éclairage** | `true` | **allumé toute la journée** |

Cette divergence est **voulue** et ne doit pas être unifiée. La filtration n'est **pas régressée** : le paramètre par défaut `false` préserve strictement son comportement (les tests natifs feature-038 restent verts).

### Testabilité native

`schedule_logic.cpp` est couvert à **100 % des lignes** par la suite Unity native, avec **11 tests dédiés** à l'éclairage ajoutés en feature-040 (équivalence stricte de `decideLightingOn`, divergence `start==end`, fenêtre simple + wrap minuit, priorités manuel/horaire, conservation d'état sans heure valide) — **93 tests au total**. Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Cas limites

- **`feature_enabled = false`** : le relais est forcé à LOW, UI masquée.
- **Programmation `start == end`** : **allumé toute la journée** (`isMinutesInRange(..., equalMeansAlways=true)`) — divergence voulue avec la filtration.
- **Plage traversant minuit** (`start > end`, ex. 22:00 → 06:00) : gérée.
- **Heure indisponible sous programmation** (`haveTime=false`) : conserve l'état courant.

## Fichiers liés

- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — logique d'horaire pure partagée filtration/éclairage

- [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- [`src/constants.h`](../../src/constants.h) — `kLightingRelayPin = 27` (PCB v2)
- [`src/config.h:101`](../../src/config.h:101) — struct `LightingConfig`
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [page-lighting.md](../features/page-lighting.md) — UI correspondante
