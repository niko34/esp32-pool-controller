# Page Éclairage — `/lighting`

- **Fichier UI** : [`data/index.html:411`](../../data/index.html:411) (section `#view-lighting`)
- **URL** : `http://poolcontroller.local/#/lighting`

## Rôle

Contrôler l'éclairage de la piscine (relais GPIO26). Permet un allumage manuel immédiat ou une programmation horaire.

## Structure

- **Toggle « Gérer l'éclairage »** — masque / démasque la carte si la fonctionnalité n'est pas désirée (équivalent `lighting_feature_enabled` dans [`config.h:103`](../../src/config.h:103)).
- **Carte Contrôle manuel** — boutons Allumer / Éteindre. Le **badge d'état (Allumé / Éteint)** est placé à droite du titre dans le `card__head` (frère direct du `<h2>`, classes `pill ok` ou `pill bad`) — voir feature-001. La ligne « État actuel » du body, devenue redondante, a été supprimée.
- **Carte Programmation** — alignée sur la carte Programmation de la filtration (feature-052) : un select **« Mode d'éclairage »** avec deux options **« Programmation »** (value `enabled`) et **« Désactivé »** (value `disabled`), suivi des heures de **Début** / **Fin** (format `HH:MM`) affichées en grille 2 colonnes lorsque le mode « Programmation » est actif, puis bouton Sauvegarder. Les `value` du select mappent toujours le booléen `lighting_schedule_enabled` de `/get-config` / `/save-config` (backend inchangé) ; seuls les libellés affichés ont changé. Le **contrôle manuel** (Allumer / Éteindre) reste totalement indépendant de ce planning.

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
| `switch.*_lighting` (Éclairage Piscine — relais manuel) | `{base}/lighting_state` | `{base}/lighting/set` |
| `switch.*_lighting_schedule` (Programmation éclairage) | `{base}/lighting_schedule` | `{base}/lighting_schedule/set` |
| `text.*_lighting_start` (Éclairage début) | `{base}/lighting_start` | `{base}/lighting_start/set` |
| `text.*_lighting_end` (Éclairage fin) | `{base}/lighting_end` | `{base}/lighting_end/set` |

Depuis feature-052 (v2.17.0), le planning éclairage (activation + heures) est pilotable depuis Home Assistant, en miroir du planning de filtration (feature-051). Une modification depuis HA est appliquée par le firmware (validation ON/OFF ou `HH:MM`, resync sur invalide), persistée, et propagée à l'UI web via le broadcast WebSocket `config`. Le switch « Éclairage Piscine » (relais manuel) reste inchangé. Voir [`docs/MQTT.md`](../MQTT.md#topics-et-entités-ajoutés-en-feature-052-planning-éclairage-ha-v2170).

## Règles firmware

- Le relais est piloté en tout-ou-rien (LOW / HIGH). Un champ `brightness` (PWM 0-255) existe dans la struct [`LightingConfig`](../../src/config.h:101) pour un futur support de dimming, mais il n'est pas exposé dans l'UI à date.
- La programmation horaire est vérifiée à chaque `loop()` par [`lighting.cpp`](../../src/lighting.cpp) ; elle respecte les plages qui franchissent minuit.

## Cas limites

- **Pas d'éclairage câblé** : désactiver `lighting_feature_enabled` masque toute la carte sur le dashboard et la page `/lighting`.
- **Programmation activée sans heures cohérentes** : la logique `isMinutesInRange()` tolère start == end (aucun allumage).

## Fichiers

- [`data/index.html:411`](../../data/index.html:411)
- [`data/app.js`](../../data/app.js) — `updateLightingStatus()` (split détail = `pill ok/bad` dans `card__head` ; dashboard = `state-badge--*` inchangé)
- [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- [`src/config.h:101`](../../src/config.h:101) struct `LightingConfig`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/lighting/*`

## Specs historiques

- **feature-052** (v2.17.0) : uniformisation de la carte Programmation avec la filtration (libellé « Mode d'éclairage », options « Programmation »/« Désactivé », heures en grille 2 colonnes) + exposition du planning éclairage à Home Assistant (switch + 2 entités `text`).
