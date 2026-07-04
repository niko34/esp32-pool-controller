# Page Tableau de bord — `/dashboard`

- **Fichier UI** : [`data/index.html:162`](../../data/index.html:162) (section `#view-dashboard`)
- **URL** : `http://poolcontroller.local/#/dashboard` (page d'accueil par défaut)

## Rôle

Vue d'ensemble de la piscine en temps réel. C'est la page que l'utilisateur voit en arrivant sur l'interface. Elle doit donner **en un coup d'œil** l'état de l'installation et permettre les actions fréquentes (allumer/éteindre filtration et éclairage) sans navigation.

## Structure

La page est une **grille de cinq cartes** (classe `grid--status-cards`) :

1. **Carte Filtration** (`#dashboard-filtration-card`) — état, mode, horaires, boutons Démarrer / Arrêter. Un lien engrenage amène vers `/filtration`.
2. **Carte Éclairage** (`#dashboard-lighting-card`) — état, mode (programmation on/off), horaires, boutons Allumer / Éteindre. Lien vers `/lighting`.
3. **Carte pH** (`#dashboard-ph-card`) — valeur pH courante, cible, badge dosage actif, mini-graphique. Bandeau « Calibration requise » si la sonde n'a jamais été calibrée. Lien vers `/ph`.
4. **Carte ORP** (`#dashboard-orp-card`) — valeur ORP courante (mV), cible, badge dosage actif, mini-graphique. Bandeau calibration. Lien vers `/orp`.
5. **Carte Température** (`#dashboard-temperature-card`) — valeur courante en °C, mini-graphique. Lien vers `/temperature`.

Chaque mini-graphique est rendu par **uPlot** (usine `createMiniLineChart`, feature-043 — remplace Chart.js) dans un conteneur dédié, alimenté par l'historique des 3 derniers jours (`GET /get-history?range=3d`, points bruts + moyennes horaires selon l'ancienneté). La coloration de la courbe est **conditionnelle par segment** (vert dans la tolérance, interpolation continue vers le rouge au-delà, gris si donnée absente — `getMiniChartRGB`, app.js).

Un **chip `Calibration nécessaire`** (`#calib-chip`) s'affiche en haut de page si pH **ou** ORP est flagué non calibré.

## Données consommées (WebSocket `/ws`)

La carte pH lit :
- `ph`, `ph_target`, `ph_dosing`, `ph_cal_valid`

La carte ORP lit :
- `orp`, `orp_target`, `orp_dosing`, `orp_cal_valid`

La carte Température lit :
- `temperature`

La carte Filtration lit :
- `filtration_running`, `filtration_mode`, `filtration_start`, `filtration_end`
- Flag d'override optimiste côté JS : `filtrationRunningOverride` (voir mémoire projet)

La carte Éclairage lit :
- `lighting_enabled`, `lighting_schedule_enabled`, `lighting_start`, `lighting_end`

Les mini-graphiques consomment `GET /get-history?range=3d` au chargement, puis se rafraîchissent par **polling HTTP incrémental toutes les 5 minutes** (`GET /get-history?range=3d&since=<dernier timestamp>`, ajout des nouveaux points en fin de série) — pas via WebSocket (`loadMiniChartData`, app.js).

## Actions

| Bouton | Endpoint HTTP |
|--------|--------------|
| `#detail-filtration-start` | `POST /filtration/on` (ou équivalent) |
| `#detail-filtration-stop` | `POST /filtration/off` |
| `#detail-lighting-on` | `POST /lighting/on` |
| `#detail-lighting-off` | `POST /lighting/off` |
| Liens engrenage | Navigation SPA (fragment `#/ph`, etc.) |

La fonction `updateFiltrationBadges()` côté JS met à jour simultanément `#detail-filtration-status` (dashboard) et `#filtration-current-status` (page filtration) pour rester cohérent.

## Cas limites

- **WebSocket déconnecté** : les valeurs affichent `--`. Les mini-graphiques conservent la dernière série.
- **Capteur pH ou ORP en erreur** : badge `sensor-badge` affiché (`#ph-sensor-badge`, `#orp-sensor-badge`), valeur `--`.
- **Sonde non calibrée** : bandeau `calibration-alert` + chip global en haut de page.
- **Dosage en cours** : badge `dosing-status` affiché dans la carte concernée.

Aucune limite journalière ni temps de stabilisation n'est affiché sur le dashboard — ces informations sont sur les pages `/ph` et `/orp`.

## Fichiers

- [`data/index.html:162`](../../data/index.html:162) — structure HTML du dashboard
- [`data/app.js`](../../data/app.js) — `latestSensorData`, `updateFiltrationBadges()`, rendu uPlot (`createMiniLineChart`)
- [`data/uPlot.iife.min.js`](../../data/uPlot.iife.min.js), [`data/uPlot.min.css`](../../data/uPlot.min.css) — bibliothèque uPlot 1.6.32 (voir [BUILD.md](../BUILD.md#bibliothèque-de-graphiques-uplot-frontend))
- [`data/app.css`](../../data/app.css) — classes `.card--status`, `.compact-value`, `.dosing-status`, `.sensor-badge`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — `broadcastSensorData()` pousse les champs consommés ici
- [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — `GET /data`, `GET /get-history`

## Specs historiques

Aucune spec dédiée au dashboard à date (l'évolution se fait au fil des specs des autres pages).
