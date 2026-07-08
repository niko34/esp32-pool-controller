# Page Filtration — `/filtration`

- **Fichier UI** : [`data/index.html:332`](../../data/index.html:332) (section `#view-filtration`)
- **URL** : `http://poolcontroller.local/#/filtration`

## Rôle

Configurer et piloter la pompe de filtration. Trois modes coexistent :

- **Automatique** — horaires calculés à partir de la température de l'eau (algorithme autour du pivot `kFiltrationPivotHour = 13h`, voir [`constants.h:78`](../../src/constants.h:78)).
- **Manuel** — l'utilisateur règle un horaire fixe `start` / `end`.
- **Off** — la filtration est désactivée.

Un **forçage temporaire** (ON ou OFF) permet de déroger à la planification sans modifier la config ; il est perdu au redémarrage.

## Structure

> ⚠️ **Visibilité conditionnée au mode d'installation** (feature-056, v2.19.0). La page n'est pleinement active qu'en mode d'installation **`managed`** (PoolController pilote la filtration). En **`powered`** (alimenté par le circuit de filtration) et **`external`** (filtration tierce signalée), la programmation et le contrôle du relais sont **masqués** : le PoolController ne pilote pas le relais, une note « filtration non pilotée par le PoolController » est affichée. Le mode se règle dans Paramètres → Installation (voir [page-settings.md](page-settings.md#installation-panel-install-feature-056) et [ADR-0026](../adr/0026-mode-installation.md)). L'ancien toggle « Gérer la filtration » (`#filtration_enabled`) est **supprimé** (fusionné dans `install_mode`).

- **Carte Contrôle manuel** (`#filtration-manual-card`) — boutons Démarrer / Arrêter. Le **badge d'état (Marche / Arrêt)** est placé à droite du titre dans le `card__head` (frère direct du `<h2>`, classes `pill ok` ou `pill bad`) — voir feature-001. La ligne « État actuel » du body, devenue redondante, a été supprimée. Masquée hors mode `managed`.
- **Carte Configuration** — sélecteur de mode, heure de début, heure de fin, bouton Sauvegarder. Masquée hors mode `managed`.

Les autres cartes (graphe, statistiques) peuvent être ajoutées au fil du temps ; se référer à `#view-filtration` pour l'état courant.

## Données consommées (WebSocket `/ws`)

- `install_mode` (`managed` / `powered` / `external`) — pilote la visibilité de la page (feature-056). `filtration_enabled` **retiré**.
- `filtration_running` (bool — relais fermé, pertinent en `managed`)
- `filtration_mode` (`auto` / `manual` / `force` / `off`)
- `filtration_start`, `filtration_end` (strings `HH:MM`)
- `filtration_force_on`, `filtration_force_off` (bool — forçage temporaire)

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Démarrer manuellement | `POST /filtration/on` | WRITE |
| Arrêter manuellement | `POST /filtration/off` | WRITE |
| Sauvegarder mode/horaires | `POST /save-config` (champs `filtration_mode`, `filtration_start`, `filtration_end` — `filtration_enabled` retiré, feature-056) | CRITICAL |

La fonction `saveFiltration()` côté JS sauvegarde uniquement les champs filtration, indépendamment des autres saves (autosave dédié).

## Règles firmware

- Mode `auto` : l'horaire est recalculé à chaque `computeAutoSchedule()` ([`filtration.cpp`](../../src/filtration.cpp)) en fonction de la température mesurée, avec une **deadband de 1°C** (`_lastScheduledTemp`) pour éviter les recalculs inutiles. Rafraîchissement possible après 5 min d'eau en circulation.
- Mode `manual` : l'horaire est respecté tel qu'entré par l'utilisateur.
- Mode `off` : le relais est forcé à LOW en permanence.
- **Relais piloté uniquement en mode d'installation `managed`** (feature-056) : en `powered` / `external`, `filtration.update()` retourne de façon anticipée, aucune commande relais, timer de stabilisation au démarrage filtration non armé.
- Interaction régulation : voir [page pH](page-ph.md), [page ORP](page-orp.md) et [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md). En mode `managed`, le démarrage de la filtration arme le timer de stabilisation (voir `armStabilizationTimer()` dans [`pump_controller.h:112`](../../src/pump_controller.h:112)).

## Interaction MQTT / Home Assistant

| Entité HA | Topic état | Topic commande |
|-----------|------------|----------------|
| `switch.*_filtration_switch` | `{base}/filtration_state` (`ON`/`OFF`) | `{base}/filtration/set` |
| `select.*_filtration_mode` | `{base}/filtration_mode` (`auto`/`manual`/`force`/`off`) | `{base}/filtration_mode/set` |
| `binary_sensor.*_filtration` | `{base}/filtration_state` | — |

Voir [`docs/MQTT.md`](../MQTT.md) pour l'auto-discovery complet.

## Cas limites

- **WebSocket déconnecté** : les valeurs affichent `--:--` ; les boutons Démarrer/Arrêter restent cliquables et produisent une erreur si l'API HTTP est aussi injoignable.
- **OTA en cours** : le contrôleur n'accepte plus de changements d'état, `PumpController.setOtaInProgress(true)` stoppe également les pompes doseuses ([`pump_controller.cpp`](../../src/pump_controller.cpp)).
- **Horaire de fin avant le début** : la logique `isMinutesInRange()` gère les plages qui franchissent minuit ([`filtration.cpp`](../../src/filtration.cpp)).

## Fichiers

- [`data/index.html:332`](../../data/index.html:332)
- [`data/app.js`](../../data/app.js) — `saveFiltration()`, `updateFiltrationBadges()` (split détail = `pill ok/bad/mid` dans `card__head` ; dashboard = `state-badge--*` inchangé), `getFiltrationState()` qui expose `pillClass` (mapping `warn → mid`)
- [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp) — logique firmware
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/filtration/*`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — persistance config
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — `publishFiltrationState()`

## Documentation liée

- [docs/subsystems/filtration.md](../subsystems/filtration.md) — règles de comportement et interaction filtration ↔ régulation.
- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — gardes (`canDose()` requiert la présence d'eau `resolveWaterPresence()`).
- [ADR-0026](../adr/0026-mode-installation.md) — mode d'installation (3 archétypes de câblage) + résolution de la présence d'eau.
