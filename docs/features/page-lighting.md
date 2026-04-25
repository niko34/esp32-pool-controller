# Page Éclairage — `/lighting`

- **Fichier UI** : [`data/index.html:411`](../../data/index.html:411) (section `#view-lighting`)
- **URL** : `http://poolcontroller.local/#/lighting`

## Rôle

Contrôler l'éclairage de la piscine (relais GPIO26). Permet un allumage manuel immédiat ou une programmation horaire.

## Structure

- **Toggle « Gérer l'éclairage »** — masque / démasque la carte si la fonctionnalité n'est pas désirée (équivalent `lighting_feature_enabled` dans [`config.h:103`](../../src/config.h:103)).
- **Carte Contrôle manuel** — boutons Allumer / Éteindre, état actuel.
- **Carte Programmation** — toggle programmation + heures de début et de fin (format `HH:MM`) + bouton Sauvegarder.

## Données consommées (WebSocket `/ws`)

- `lighting_feature_enabled` (bool)
- `lighting_enabled` (bool — état relais)
- `lighting_schedule_enabled` (bool — programmation active)
- `lighting_start`, `lighting_end` (strings `HH:MM`)

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Allumer | `POST /lighting/on` | WRITE |
| Éteindre | `POST /lighting/off` | WRITE |
| Sauvegarder programmation | `POST /save-config` (champs `lighting_*`) | CRITICAL |

## Interaction MQTT / Home Assistant

| Entité HA | Topic état | Topic commande |
|-----------|------------|----------------|
| `switch.*_lighting` | `{base}/lighting_state` | `{base}/lighting/set` |

## Règles firmware

- Le relais est piloté en tout-ou-rien (LOW / HIGH). Un champ `brightness` (PWM 0-255) existe dans la struct [`LightingConfig`](../../src/config.h:101) pour un futur support de dimming, mais il n'est pas exposé dans l'UI à date.
- La programmation horaire est vérifiée à chaque `loop()` par [`lighting.cpp`](../../src/lighting.cpp) ; elle respecte les plages qui franchissent minuit.

## Cas limites

- **Pas d'éclairage câblé** : désactiver `lighting_feature_enabled` masque toute la carte sur le dashboard et la page `/lighting`.
- **Programmation activée sans heures cohérentes** : la logique `isMinutesInRange()` tolère start == end (aucun allumage).

## Fichiers

- [`data/index.html:411`](../../data/index.html:411)
- [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- [`src/config.h:101`](../../src/config.h:101) struct `LightingConfig`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/lighting/*`

## Specs historiques

Aucune spec dédiée à date.
