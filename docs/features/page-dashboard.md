# Page Tableau de bord — `/dashboard`

- **Fichier UI** : [`data/index.html:162`](../../data/index.html:162) (section `#view-dashboard`)
- **URL** : `http://poolcontroller.local/#/dashboard` (page d'accueil par défaut)

## Rôle

Vue d'ensemble de la piscine en temps réel. C'est la page que l'utilisateur voit en arrivant sur l'interface. Elle doit donner **en un coup d'œil** l'état de l'installation et permettre les actions fréquentes (allumer/éteindre filtration et éclairage) sans navigation.

## Structure

La page est une **grille de six cartes** (classe `grid--status-cards`) :

1. **Carte Filtration** (`#dashboard-filtration-card`) — état, mode, horaires, boutons Démarrer / Arrêter. Un lien engrenage amène vers `/filtration`.
2. **Carte Éclairage** (`#dashboard-lighting-card`) — état, mode (programmation on/off), horaires, boutons Allumer / Éteindre. Lien vers `/lighting`.
3. **Carte pH** (`#dashboard-ph-card`) — valeur pH courante, cible, badge dosage actif, mini-graphique. Bandeau « Calibration requise » si la sonde n'a jamais été calibrée. Lien vers `/ph`.
4. **Carte ORP** (`#dashboard-orp-card`) — valeur ORP courante (mV), cible, badge dosage actif, mini-graphique. Bandeau calibration. Lien vers `/orp`.
5. **Carte Température** (`#dashboard-temperature-card`) — valeur courante en °C, mini-graphique. Lien vers `/temperature`.
6. **Carte Boost** (`#dashboard-boost-card`) — activation / désactivation du Mode Boost, heure d'expiration quand il est actif (feature-053, v2.18.0, voir [Carte Boost](#carte-boost-feature-053)).

Chaque mini-graphique est rendu par **uPlot** (usine `createMiniLineChart`, feature-043 — remplace Chart.js) dans un conteneur dédié, alimenté par l'historique des 3 derniers jours (`GET /get-history?range=3d`, points bruts + moyennes horaires selon l'ancienneté). La coloration de la courbe est **conditionnelle par segment** (vert dans la tolérance, interpolation continue vers le rouge au-delà, gris si donnée absente — `getMiniChartRGB`, app.js).

Un **chip `Calibration nécessaire`** (`#calib-chip`) s'affiche en haut de page si pH **ou** ORP est flagué non calibré.

## Badge multi-états des cartes pH / ORP (`#ph-sensor-badge`, `#orp-sensor-badge`)

Chaque carte pH et ORP porte un badge composé (`composeSensorBadge()`, app.js — feature-012) qui agrège jusqu'à cinq états candidats, par ordre de priorité décroissante :

| Priorité | Segment | Variant | Condition |
|---|---|---|---|
| 0 | `Capteur indisponible` | `danger` | valeur `ph` / `orp` `null` ou NaN dans le WS (capteur stale ou figé — feature-022, v2.10.0) |
| 1 | `Trop bas (X)` / `Trop élevé (X)` | `danger` | valeur hors plage (cible ± 0,2 pH / ± 150 mV) |
| 2 | `Limite journalière atteinte` | `warning` | `ph_limit_reached` / `orp_limit_reached` |
| 3 | `Cumul XX %` | `warning` | cumul journalier ≥ 75 % du quota |
| 4 | `Stock faible (X,X L)` | `warning` | `*_remaining_ml` ≤ `*_alert_threshold_ml` (seuil > 0) |

**Règle de composition** :
- **Maximum 2 segments** affichés simultanément, séparés par « · ». Au-delà de deux candidats, seuls les **deux plus prioritaires** sont retenus (ex. « hors plage + limite atteinte + stock faible » → `Trop élevé (7.8) · Limite journalière atteinte`, le stock est tronqué).
- Variant du badge : `danger` si au moins un segment danger est retenu, sinon `warning`.
- « Hors plage » et « Limite journalière atteinte » simultanés affichent désormais **les deux segments** (avant v2.9.0 : seul « hors plage » était visible).

**Segment « Capteur indisponible »** (feature-022, v2.10.0) :
- Priorité la plus haute (0) : un capteur mort passe toujours devant les autres états.
- Piloté par les **signaux WS existants** (`ph` / `orp` à `null` quand le firmware ne dispose plus d'une valeur valide — stale ou figé) : aucun champ WS nouveau.
- **Garde anti-faux-positif au boot** : le segment n'est produit que si `latestSensorData !== null` (au moins un message WS reçu) — `updateSensorBadges()` étant aussi appelé depuis `loadConfig()` avant le premier push WS.
- « Capteur indisponible » et « hors plage » sont mutuellement exclusifs (une valeur `null` n'est pas comparable à la cible).

**Segment « Cumul XX % »** :
- Affiché à partir de **75 %** du quota journalier (`ph_daily_ml / max_ph_ml_per_day`, `orp_daily_ml / max_chlorine_ml_per_day`), même arithmétique que la barre de cumul des pages `/ph` et `/orp` (arrondi des entrées, pourcentage plafonné à 100).
- Masqué quand la limite est atteinte (le segment « Limite journalière atteinte » prend le relais, pas de redondance), quand la limite n'est pas configurée (quota ≤ 0), ou quand les données sont indisponibles.

## Statut dosage et stabilisation par pompe

Le statut `#ph-dosing-status` / `#orp-dosing-status` de chaque carte (`getDosingStopReason()`, app.js) affiche notamment « Stabilisation : X min YY s » pendant la stabilisation post-calibration. Depuis v2.9.0, il consomme les champs **par pompe** `ph_stab_remaining_s` / `orp_stab_remaining_s` (feature-006) avec repli sur le champ global `stabilization_remaining_s` (compatibilité firmware plus ancien) : chaque carte affiche la stabilisation de **sa** pompe — une calibration ORP n'affiche plus « Stabilisation » sur la carte pH, et inversement.

## Données consommées (WebSocket `/ws`)

La carte pH lit :
- `ph`, `ph_target`, `ph_dosing`, `ph_cal_valid`
- Badge multi-états : `ph_limit_reached`, `ph_daily_ml`, `ph_remaining_ml`, `ph_alert_threshold_ml` (+ `max_ph_ml_per_day` de la config)
- Stabilisation : `ph_stab_remaining_s` (repli `stabilization_remaining_s`)

La carte ORP lit :
- `orp`, `orp_target`, `orp_dosing`, `orp_cal_valid`
- Badge multi-états : `orp_limit_reached`, `orp_daily_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml` (+ `max_chlorine_ml_per_day` de la config)
- Stabilisation : `orp_stab_remaining_s` (repli `stabilization_remaining_s`)

La carte Température lit :
- `temperature`

La carte Filtration lit :
- `filtration_running`, `filtration_mode`, `filtration_start`, `filtration_end`
- Flag d'override optimiste côté JS : `filtrationRunningOverride` (voir mémoire projet)

La carte Éclairage lit :
- `lighting_enabled`, `lighting_schedule_enabled`, `lighting_start`, `lighting_end`

La carte Boost lit :
- `boost_active` (bool), `boost_until` (epoch d'expiration) — voir [Carte Boost](#carte-boost-feature-053)
- `boost_filtration_extended`, `boost_chlorine_boosted` (bool, feature-055) — leviers réellement actifs, pilotent la ligne « Effet »

Les mini-graphiques consomment `GET /get-history?range=3d` au chargement, puis se rafraîchissent par **polling HTTP incrémental toutes les 5 minutes** (`GET /get-history?range=3d&since=<dernier timestamp>`, ajout des nouveaux points en fin de série) — pas via WebSocket (`loadMiniChartData`, app.js).

## Carte Boost (feature-053)

La carte **Boost** (`#dashboard-boost-card`, v2.18.0) permet d'activer d'un geste le **Mode Boost** : surchloration temporaire du jour (filtration prolongée + cible ORP relevée) après une forte fréquentation. Décision et bornes de sécurité : [ADR-0025](../adr/0025-mode-boost.md).

- **Boost inactif** : bouton **Activer le Boost**. Au clic → `POST /boost/start`. À l'activation, un **toast adaptatif** (v2.18.1, feature-054) informe selon les leviers réellement actifs, à partir des booléens `filtration_extended` / `chlorine_boosted` renvoyés par la réponse 200 :

  | `filtration_extended` | `chlorine_boosted` | Toast |
  |---|---|---|
  | ✅ | ✅ | *info* — « Boost activé — vérifiez le taux de chlore avant la baignade. » (message nominal, le contrôleur ne mesure pas le chlore libre ppm) |
  | ❌ | ✅ | *info* — « Boost activé (surchloration seule) : la filtration n'est pas gérée par PoolController. Vérifiez le taux de chlore avant la baignade. » |
  | ✅ | ❌ | *warning* — « Boost activé (filtration prolongée seule) : la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé. » |
  | ❌ | ❌ | *warning* — « Boost sans effet : filtration non gérée et régulation ORP non automatique. » |
- **Boost actif** (`boost_active === true`) : la carte affiche l'**heure d'expiration** dérivée de `boost_until` (prochain minuit local), une **ligne « Effet » persistante** (`#detail-boost-effect-row` / `#detail-boost-effect`, feature-055, v2.18.2 — voir ci-dessous) et un bouton **Arrêter le Boost** → `POST /boost/stop`.

**Ligne « Effet » persistante (feature-055)** — complément **persistant** du toast fugace de la feature-054 (le toast n'apparaît qu'au clic UI, jamais lors d'une activation depuis Home Assistant, et disparaît au rechargement). La ligne indique en permanence les leviers réellement actifs, à partir des booléens WS `boost_filtration_extended` / `boost_chlorine_boosted` (calculés au vol, donc valides après activation HA **et** après reload — voir [ws-manager.md §feature-055](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-feature-055-effet-boost-persistant-v2182)) :

  | `boost_filtration_extended` | `boost_chlorine_boosted` | Libellé « Effet » | Badge |
  |---|---|---|---|
  | ✅ | ✅ | « Filtration prolongée + surchloration » | `--ok` |
  | ❌ | ✅ | « Surchloration seule » | `--ok` |
  | ✅ | ❌ | « Filtration prolongée seule » | `--ok` |
  | ❌ | ❌ | « Sans effet (filtration non gérée, ORP non automatique) » | `--warn` (orange, attire l'œil) |

  Quand le Boost est **inactif**, la ligne « Effet » est masquée (badge de carte `--off`). Au **clic sur Activer**, `updateBoostCard()` mappe `filtration_extended` / `chlorine_boosted` de la réponse `POST /boost/start` vers `latestSensorData.boost_filtration_extended` / `boost_chlorine_boosted` pour un affichage immédiat, sans attendre le prochain push WS (~5 s).

  **Texte d'aide cohérent (`#detail-boost-hint`, v2.18.3)** — le hint sous le widget ne promet plus systématiquement l'effet nominal. Quand le Boost est **actif**, il est masqué (la ligne « Effet » fait foi, plus de contradiction). Quand il est **inactif**, il devient **prospectif** (« À l'activation : … ») et reflète les mêmes booléens WS que la ligne « Effet » : filtration prolongée + cible ORP relevée / cible ORP relevée seule / filtration prolongée seule / aucun effet.
- **Expiration automatique** : le firmware désactive le Boost à minuit local et pousse `boost_active: false` en WS → la carte repasse à l'état inactif sous ≤ 5 s, sans reload.
- **Heure non synchronisée** : `POST /boost/start` renvoie `409 time_not_synced` (l'expiration à minuit serait incalculable) → l'UI affiche un message d'indisponibilité et ne bascule pas la carte.

> **Effet chlore gaté au mode ORP `automatic`** : en mode de régulation ORP Manuel / Programmé, le Boost n'étend que la filtration (la cible et la limite chlore ne sont pas relevées). Comportement porté par le firmware ([pump-controller.md §Mode Boost](../subsystems/pump-controller.md#mode-boost-feature-053)). De même, si la filtration n'est pas gérée par PoolController, le Boost ne prolonge pas la filtration. Le toast adaptatif ci-dessus signale ces cas ; côté firmware, `startBoost()` logue un `warning` par levier inerte (couvre aussi l'activation via Home Assistant).

## Actions

| Bouton | Endpoint HTTP |
|--------|--------------|
| `#detail-filtration-start` | `POST /filtration/on` (ou équivalent) |
| `#detail-filtration-stop` | `POST /filtration/off` |
| `#detail-lighting-on` | `POST /lighting/on` |
| `#detail-lighting-off` | `POST /lighting/off` |
| Boost — Activer | `POST /boost/start` (toast rappel baignade) |
| Boost — Arrêter | `POST /boost/stop` |
| Liens engrenage | Navigation SPA (fragment `#/ph`, etc.) |

La fonction `updateFiltrationBadges()` côté JS met à jour simultanément `#detail-filtration-status` (dashboard) et `#filtration-current-status` (page filtration) pour rester cohérent.

## Cas limites

- **WebSocket déconnecté** : les valeurs affichent `--`. Les mini-graphiques conservent la dernière série.
- **Capteur pH ou ORP en erreur** : segment « Capteur indisponible » (danger, priorité 0) dans le badge multi-états (voir [Badge multi-états](#badge-multi-états-des-cartes-ph--orp-ph-sensor-badge-orp-sensor-badge)), valeur `--`.
- **Sonde non calibrée** : bandeau `calibration-alert` + chip global en haut de page.
- **Dosage en cours** : badge `dosing-status` affiché dans la carte concernée.

Le détail complet du cumul journalier (barre de progression, mL consommés / quota) reste sur les pages `/ph` et `/orp` ; le dashboard n'en montre que le segment « Cumul XX % » à partir de 75 %.

## Fichiers

- [`data/index.html:162`](../../data/index.html:162) — structure HTML du dashboard
- [`data/app.js`](../../data/app.js) — `latestSensorData`, `updateFiltrationBadges()`, `composeSensorBadge()`, `dailyCumulSegment()`, `getDosingStopReason()`, rendu uPlot (`createMiniLineChart`)
- [`data/uPlot.iife.min.js`](../../data/uPlot.iife.min.js), [`data/uPlot.min.css`](../../data/uPlot.min.css) — bibliothèque uPlot 1.6.32 (voir [BUILD.md](../BUILD.md#bibliothèque-de-graphiques-uplot-frontend))
- [`data/app.css`](../../data/app.css) — classes `.card--status`, `.compact-value`, `.dosing-status`, `.sensor-badge`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — `broadcastSensorData()` pousse les champs consommés ici
- [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — `GET /data`, `GET /get-history`

## Specs historiques

- `feature-012-badges-dashboard-cumul-stabilisation` — segment « Cumul XX % », règle de composition du badge multi-états (max 2 segments), stabilisation par pompe (v2.9.0, périmètre réduit sur avis UX)
- `feature-022-observabilite-dosage` — segment « Capteur indisponible » (priorité 0, danger) piloté par `ph`/`orp` null dans le WS, garde anti-faux-positif au boot (v2.10.0)
