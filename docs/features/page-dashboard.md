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
5. **Carte Température** (`#dashboard-temperature-card`) — valeur courante en °C, badge `#temperature-sensor-badge` (« Capteur indisponible » v2.10.5 + segments d'état mesure feature-061 itération 2), mini-graphique. Lien vers `/temperature`.
6. **Carte Boost** (`#dashboard-boost-card`) — activation / désactivation du Mode Boost, heure d'expiration quand il est actif (feature-053, v2.18.0, voir [Carte Boost](#carte-boost-feature-053)).

Chaque mini-graphique est rendu par **uPlot** (usine `createMiniLineChart`, feature-043 — remplace Chart.js) dans un conteneur dédié, alimenté par l'historique des 3 derniers jours (`GET /get-history?range=3d`, points bruts + moyennes horaires selon l'ancienneté). La coloration de la courbe est **conditionnelle par segment** (vert dans la tolérance, interpolation continue vers le rouge au-delà, gris si donnée absente — `getMiniChartRGB`, app.js).

Un **chip `Calibration nécessaire`** (`#calib-chip`) s'affiche en haut de page si pH **ou** ORP est flagué non calibré.

## Badge multi-états des cartes pH / ORP (`#ph-sensor-badge`, `#orp-sensor-badge`)

Chaque carte pH et ORP porte un badge composé (`composeSensorBadge()`, app.js — feature-012) qui agrège jusqu'à cinq états candidats, par ordre de priorité décroissante :

| Priorité | Segment | Variant | Condition |
|---|---|---|---|
| 0 | `Capteur indisponible` | `danger` | valeur `ph` / `orp` `null` ou NaN dans le WS (capteur stale ou figé — feature-022, v2.10.0) — **gaté par `measures_valid`** (feature-061, voir ci-dessous) |
| 1 | `Filtration arrêtée` | `info` | `water_present === false` sans staleness (feature-061, v2.13.0 — voir [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)) |
| 1 | `Signal filtration périmé` | `warning` | `water_present === false` et `filtration_state_stale === true` (feature-061) |
| 1 | `Stabilisation : ~N min` | `info` | `water_present === true` et `measures_valid === false` (purge post-reprise, feature-061) |
| 1 | `Trop bas (X)` / `Trop élevé (X)` | `danger` | valeur hors plage (cible ± 0,2 pH / ± 150 mV) — **gaté par `measures_valid`** (feature-061) |
| 2 | `Limite journalière atteinte` | `warning` | `ph_limit_reached` / `orp_limit_reached` |
| 3 | `Cumul XX %` | `warning` | cumul journalier ≥ 75 % du quota |
| 4 | `Stock faible (X,X L)` | `warning` | `*_remaining_ml` ≤ `*_alert_threshold_ml` (seuil > 0) |

**Règle de composition** :
- **Maximum 2 segments** affichés simultanément, séparés par « · ». Au-delà de deux candidats, seuls les **deux plus prioritaires** sont retenus (ex. « hors plage + limite atteinte + stock faible » → `Trop élevé (7.8) · Limite journalière atteinte`, le stock est tronqué).
- Variant du badge : hiérarchie **`danger` > `warning` > `info`** (feature-061 — avant : `danger` sinon `warning`). La variante `info` (`.sensor-badge--info`, fond bleu) signale un **état normal informatif** — « attendre », par opposition au rouge « agir ».
- « Hors plage » et « Limite journalière atteinte » simultanés affichent désormais **les deux segments** (avant v2.9.0 : seul « hors plage » était visible).

**Segment « Capteur indisponible »** (feature-022, v2.10.0) :
- Priorité la plus haute (0) : un capteur mort passe toujours devant les autres états.
- Piloté par les **signaux WS existants** (`ph` / `orp` à `null` quand le firmware ne dispose plus d'une valeur valide — stale ou figé) : aucun champ WS nouveau.
- **Garde anti-faux-positif au boot** : le segment n'est produit que si `latestSensorData !== null` (au moins un message WS reçu) — `updateSensorBadges()` étant aussi appelé depuis `loadConfig()` avant le premier push WS.
- « Capteur indisponible » et « hors plage » sont mutuellement exclusifs (une valeur `null` n'est pas comparable à la cible).
- **Garde anti-faux-positif « mesures invalides » (feature-061)** : le segment n'est produit que si `measuresAreValid(latestSensorData)` — on ne peut pas juger une sonde (ni une valeur hors plage) quand l'eau ne circule pas. Filtration arrêtée → seul le segment d'état mesure s'affiche, jamais un faux « Capteur indisponible » rouge.

**Segment « Cumul XX % »** :
- Affiché à partir de **75 %** du quota journalier (`ph_daily_ml / max_ph_ml_per_day`, `orp_daily_ml / max_chlorine_ml_per_day`), même arithmétique que la barre de cumul des pages `/ph` et `/orp` (arrondi des entrées, pourcentage plafonné à 100).
- Masqué quand la limite est atteinte (le segment « Limite journalière atteinte » prend le relais, pas de redondance), quand la limite n'est pas configurée (quota ≤ 0), ou quand les données sont indisponibles.

## Badge de la carte Température (`#temperature-sensor-badge`, v2.10.5, étendu feature-061 itération 2)

La carte Température porte un badge calqué sur les badges pH/ORP :

- **« Capteur indisponible »** (variant `danger`, v2.10.5) : affiché quand `temperature` est `null` ou NaN dans le WS (sonde DS18B20 débranchée, non identifiée ou en erreur). Même **garde anti-faux-positif au boot** (badge produit uniquement si `latestSensorData !== null`) et, depuis l'itération 2 de feature-061, même **garde anti-faux-positif « mesures invalides »** que pH/ORP (segment gaté par `measuresAreValid()` — on ne juge pas la sonde quand l'eau ne circule pas).
- **Segments d'état mesure** (feature-061 itération 2) : mêmes états que pH/ORP via `measureStateSegments()` — « Filtration arrêtée » (info bleu), « Signal filtration périmé » (warning), « Stabilisation : ~N min » (info). Voir la [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130).
- Masqué dès que la valeur redevient valide (disparition au prochain push WS, sans reload).

Pas de segments dosage/limites/calibration ici : la température n'a ni dosage, ni limites, ni calibration.

## Convention « `--` » quand un capteur est indisponible (v2.10.5)

Toute valeur capteur `null`/NaN s'affiche « **`--`** » sur **tous** les sites d'affichage numérique — jamais de conservation silencieuse de la dernière valeur connue (bug corrigé en v2.10.5 : le pattern « skip si null » figeait indéfiniment l'ancienne valeur à l'écran quand une sonde était débranchée) :

- **Métriques du tableau de bord** (`updateDashboardMetrics`, app.js) : `#m-temp`, `#m-ph`, `#m-orp` ;
- **Widgets compacts** : `#compact-ph`, `#compact-orp`, `#compact-temperature` — l'unité est conservée dans la structure (ex. `--<span class="compact-unit">°C</span>`) pour ne pas casser la mise en page ;
- **Sections détaillées** des cartes.

Cette convention est celle déjà utilisée par les pages Réglages/Calibration. **Exceptions volontaires** : les graphiques et mini-graphiques affichent des **trous** sur les points `null` (comportement correct — depuis feature-061 les `null` sont poussés dans la série pour que uPlot lève le crayon, au lieu d'être filtrés). Retour du capteur → les valeurs réapparaissent automatiquement au prochain push WS (≤ 1 cycle de rafraîchissement).

**Extension feature-061 (v2.13.0)** : la convention « `--` » s'applique aussi quand la mesure est **présente mais non représentative** (`measures_valid === false` — filtration arrêtée ou purge post-reprise). La distinction est portée par le badge : bleu « Filtration arrêtée » / « Stabilisation » (état normal) vs rouge « Capteur indisponible » (anomalie).

## Mesures pH/ORP non représentatives — filtration arrêtée / stabilisation (feature-061, v2.13.0)

Les sondes pH/ORP étant montées dans le circuit de filtration, **filtration arrêtée = eau stagnante** : la valeur lue dérive localement et ne reflète plus le bassin. Depuis l'**itération 2** de feature-061, la **température** est traitée à l'identique (sonde DS18B20 dans la canalisation → dérive vers la température ambiante du local à l'arrêt) — même drapeau `measures_valid`, mêmes badges, mêmes trous dans les courbes. Le firmware publie le verdict (`measures_valid`, voir [measure-validity.md](../subsystems/measure-validity.md)) ; l'UI l'applique :

**Segments d'état mesure** (`measureStateSegments()`, app.js — partagés dashboard / pages pH-ORP / carte et compact température, priorité 1, variant selon le cas) :

| Condition | Segment (libellé exact) | Variant | Sens |
|---|---|---|---|
| `water_present === false`, pas de staleness | `Filtration arrêtée` | `info` (bleu) | État **normal** — attendre la reprise, rien à faire |
| `water_present === false` et `filtration_state_stale === true` | `Signal filtration périmé` | `warning` (ambré) | Anomalie du signal externe — vérifier le câblage/l'émetteur |
| `water_present === true` et `measures_valid === false` | `Stabilisation : ~N min` (arrondi minute **sup** ; `Stabilisation : < 1 min` sous 60 s) | `info` (bleu) | Purge de la chambre de mesure en cours — attendre |

- **Nouvelle variante CSS `.sensor-badge--info`** (fond bleu `--accent-light`, texte `#1e40af`, contraste AA) : distingue visuellement l'**état normal** (bleu = attendre) de l'**anomalie** (rouge « Capteur indisponible » = agir).
- Le compte à rebours « Stabilisation : ~N min » est **recalculé à chaque push WS** (`measures_stab_remaining_s`, décrémenté côté firmware) — aucun tick JS.
- **Valeurs « `--` »** : `#compact-ph` / `#compact-orp` / `#compact-temperature` (et les valeurs des pages pH/ORP, la carte température) affichent `--` quand `measuresAreValid()` est faux — la ligne « Cible » reste affichée.
- **Trous dans les courbes** : les points d'historique avec pH/ORP/température `null` sont **poussés `null`** dans les séries uPlot (lever de crayon = trou visible), y compris sur le chemin incrémental (`appendMiniChart`) et sur l'agrégation par jour (un jour sans mesure valide reste dans la série avec `null`, au lieu d'être sauté). La série température a rejoint ce comportement en itération 2 (mini-charts + page historique) — elle en était exclue en itération 1. Le point « Aujourd'hui » injecté depuis le temps réel n'est pas alimenté par une mesure invalide (la dernière mesure valide du jour reste).
- **Dé-duplication avec le statut dosage** (`dedupDosingReason()`, app.js) : quand le badge affiche déjà « Filtration arrêtée » / « Signal filtration périmé », le motif équivalent du `dosing-status` est masqué — le badge fait foi. Les autres motifs (stabilisation pompe, pause mélange, limites…) sont inchangés.
- **Rétrocompat** : champs absents (firmware < 2.13.0) → `measuresAreValid()` retourne `true`, rien ne change ; pas de badge avant le premier message WS (pas de faux positif au boot).
- Périmètre : la page **Calibration** et l'onglet **Avancé** ne sont **pas** gatés (lectures live nécessaires — sonde hors de l'eau / diagnostic). MQTT/HA inchangé.

Le statut `#ph-dosing-status` / `#orp-dosing-status` de chaque carte (`getDosingStopReason()`, app.js) affiche notamment « Stabilisation : X min YY s » pendant la stabilisation post-calibration. Depuis v2.9.0, il consomme les champs **par pompe** `ph_stab_remaining_s` / `orp_stab_remaining_s` (feature-006) avec repli sur le champ global `stabilization_remaining_s` (compatibilité firmware plus ancien) : chaque carte affiche la stabilisation de **sa** pompe — une calibration ORP n'affiche plus « Stabilisation » sur la carte pH, et inversement.

Depuis **v2.19.1**, le statut affiche aussi la **pause mélange hydraulique** post-injection (« Pause mélange : X min YY s ») via `ph_mix_remaining_s` / `orp_mix_remaining_s` (repli sur le booléen `ph/orp MixingDelayActive` → « Pause mélange en cours » sans compte à rebours). Avant cette version, la pause mélange n'était visible que dans le modal « État filtre » des pages pH/ORP, d'où une impression de dosage bloqué sans explication sur le tableau de bord. Ordre d'affichage dans `getDosingStopReason()` : régulation désactivée → stabilisation → **pause mélange** → présence d'eau → limites.

## Données consommées (WebSocket `/ws`)

La carte pH lit :
- `ph`, `ph_target`, `ph_dosing`, `ph_cal_valid`
- Badge multi-états : `ph_limit_reached`, `ph_daily_ml`, `ph_remaining_ml`, `ph_alert_threshold_ml` (+ `max_ph_ml_per_day` de la config)
- Stabilisation : `ph_stab_remaining_s` (repli `stabilization_remaining_s`)
- Pause mélange : `ph_mix_remaining_s` (repli booléen `phMixingDelayActive`)

La carte ORP lit :
- `orp`, `orp_target`, `orp_dosing`, `orp_cal_valid`
- Badge multi-états : `orp_limit_reached`, `orp_daily_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml` (+ `max_chlorine_ml_per_day` de la config)
- Stabilisation : `orp_stab_remaining_s` (repli `stabilization_remaining_s`)
- Pause mélange : `orp_mix_remaining_s` (repli booléen `orpMixingDelayActive`)

Les cartes pH et ORP lisent aussi (feature-061) :
- `water_present`, `measures_valid`, `measures_stab_remaining_s`, `filtration_state_stale` — segments d'état mesure + gate « Capteur indisponible » / « hors plage » + valeurs « `--` »

La carte Température lit :
- `temperature`
- `water_present`, `measures_valid`, `measures_stab_remaining_s`, `filtration_state_stale` — segments d'état mesure + gate « Capteur indisponible » + valeur « `--` » (feature-061, itération 2)

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
- **Capteur pH ou ORP en erreur** : segment « Capteur indisponible » (danger, priorité 0) dans le badge multi-états (voir [Badge multi-états](#badge-multi-états-des-cartes-ph--orp-ph-sensor-badge-orp-sensor-badge)), valeur `--` (voir [Convention « `--` »](#convention-----quand-un-capteur-est-indisponible-v2105)).
- **Sonde de température en erreur** (v2.10.5) : badge « Capteur indisponible » (`#temperature-sensor-badge`), valeur `--` partout où la température s'affiche.
- **Filtration arrêtée / purge post-reprise** (feature-061) : valeurs pH/ORP et — depuis l'itération 2 — température à `--`, badge bleu « Filtration arrêtée » ou « Stabilisation : ~N min » sur les trois cartes (voir [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)).
- **Sonde non calibrée** : bandeau `calibration-alert` + chip global en haut de page.
- **Dosage en cours** : badge `dosing-status` affiché dans la carte concernée (motif filtration masqué si le badge capteur le porte déjà — dé-duplication feature-061).

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
- `bug-ui-valeurs-figees-capteur-absent` — convention « `--` » généralisée à tous les sites d'affichage numérique (fin du pattern « skip si null ») + badge « Capteur indisponible » température (v2.10.5)
- `feature-061-mesures-ph-orp-filtration-arretee` — mesures pH/ORP non représentatives sans circulation d'eau : badges info bleus, valeurs `--`, trous dans les courbes, dé-duplication dosing-status (v2.13.0) ; itération 2 : extension du même traitement à la température (carte/compact `--`, badges, courbe avec trous)
