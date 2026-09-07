# Page Température — `/temperature`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-temperature`)
- **URL** : `http://poolcontroller.local/#/temperature`

## Rôle

Afficher la température courante de l'eau et son historique, et offrir une calibration **par offset** (1 point) si la sonde mesure légèrement à côté d'une référence connue.

## Structure

Depuis feature-035, l'écran adopte le **même agencement que pH/ORP** : un bloc « stats » en haut visible en permanence, et une carte de calibration masquée qui s'affiche au clic.

En mode nominal, trois zones :

1. **Bloc Statistiques** (`#temperature-card-stats`, sans titre) — **valeur de température courante** en grand (1 décimale, ex. `25.4 °C`), une rangée de chips contenant **un seul chip d'état de calibration** (`#temp-cal-chip`, non cliquable), un **badge d'état de la sonde** (`#temperature-page-badge`, v2.47.1 — masqué par défaut) et **sous la rangée** un bouton **« Calibrer la sonde »** (`#temp_cal_trigger_btn`) toujours accessible.
   - Il n'y a **pas de chip filtre/mesure** ni de chip sonde : la température n'a ni filtre médiane/EMA ni diagnostic de pente (contrairement à pH/ORP).
2. **Carte Activation** (`#temperature-card-enable`) — toggle `temperature_enabled` pour masquer la fonctionnalité si aucune sonde n'est installée.
3. **Carte Historique** (`#temperature-card-history`) — graphique uPlot (feature-043, ex-Chart.js), plages `Tout` / `30j` / `7j` / `24h`.

La **carte Calibration** (`#temperature-card-calibration`) **remplace** les cartes Activation + Historique pendant une session de calibration (le bloc Statistiques reste visible).

## Chip d'état de calibration

Le chip `#temp-cal-chip` reflète l'état de calibration. Il est mis à jour **côté UI** par `updateTempCalChip()` ([`data/app.js`](../../data/app.js)) à partir de `window._config.temp_calibration_date` et `temp_calibration_offset` (chargés via `GET /get-config`). L'ancienneté est calculée côté JS.

| Condition | Libellé chip | Variante CSS / couleur |
|---|---|---|
| `temp_calibration_date` absente / vide | « Non calibré » | `chip--probe-unknown` (gris) |
| Calibré il y a < 1 j | « Calibré · aujourd'hui » | `chip--probe-good` (vert) |
| Calibré il y a 1–30 j | « Calibré · il y a N j » | `chip--probe-good` (vert) |
| Calibré il y a > 30 j (≤ 6 mois) | « Calibré · le JJ/MM » | `chip--probe-good` (vert) |
| Calibré il y a > 6 mois | « Calibré · ancien (il y a N mois) » | `chip--probe-warn` (ambré) |

> Le chip remplace les anciens callouts `#temp_cal_date_header` / `#temp_calibrated_status`, supprimés. L'en-tête de la carte de calibration (`#temp_cal_status_header`) reprend la même information en détail (« Calibré · JJ/MM/AAAA hh:mm (offset +0.3 °C) »).

## Badge d'état de la sonde (`#temperature-page-badge`, v2.47.1, feature-068)

Calqué sur `#ph-page-badge` (aucune classe CSS nouvelle) et alimenté par **exactement la même composition** que le badge de la carte Température du tableau de bord (`composeSensorBadge()`) : la page et le tableau de bord ne peuvent pas dire deux choses différentes. Il n'y a **rien à retirer** de la composition, contrairement aux pages pH/ORP — la température n'a pas de segment dosage ou stock.

| Condition | Libellé | Variante | Priorité |
|---|---|---|---|
| `temperature` `null` / NaN | « Capteur indisponible » | `danger` | 0 |
| Rôle « eau » non identifié (`sondes_identified === false` **et** `sondes_detected >= 1`), valeur présente | « Sonde eau non identifiée » | `danger` | 0 |
| `tempFrozenAlarm === true`, rôle « eau » identifié, valeur présente | « Sonde température figée » | `warning` | 1 |
| Mesures non représentatives (`measures_valid === false`) | « Filtration arrêtée » / « Stabilisation depuis N min » / « Signal filtration périmé » | `info` / `warning` | — |

**Précédence, et pourquoi elle est ainsi** : « sonde absente ou invalide » **prime** sur « figée » (une sonde qui ne mesure plus rien ne peut pas être figée — c'est aussi ce qui retire le badge « figée » si la sonde est débranchée en cours d'épisode), et « **sonde eau non identifiée** » n'est **jamais** confondu avec « **eau figée** » : deux états distincts, dont l'un (l'identification) est actionnable et l'autre non. Détail complet et limites de la condition : [page-dashboard.md](page-dashboard.md#badge-de-la-carte-température-temperature-sensor-badge-v2105-étendu-feature-061-itération-2-puis-feature-068).

⚠️ « Sonde température figée » est **informatif** : ni le dosage, ni la compensation de température du pH, ni le planning automatique de filtration ne changent. L'erreur de pH induite vaut **≈ 0,001** — voir [ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md). Le badge est en `warning` et non en `danger` pour cette raison. Tous les segments sont gatés par `measuresAreValid()` : filtration arrêtée, l'eau immobile dans la canalisation peut légitimement rendre la même valeur pendant une demi-heure.

## Carte de calibration (offset 1 point)

Affichée **au clic** sur « Calibrer la sonde » (`showTempCalibrationCard()` masque Activation + Historique et désactive le bouton déclencheur), elle propose :

- une **lecture live** de la température (`#temp_cal_live_value`, alimentée par le champ WS `temperature` à chaque message, pour juger la stabilité avant de calibrer — **pas d'indicateur de stabilité** car la température évolue lentement) ;
- un **stepper 3 étapes** (`#temp_step1/2/3`, classes `.calibration-steps` / `.step`, états `is-active` / `is-completed`) piloté par `updateTempCalibrationSteps()` :

| Idx | Étape | Action UI |
|---|---|---|
| 1 | Plonger la sonde dans un liquide à température connue | bouton « Étape suivante » |
| 2 | Attendre la stabilisation + saisir la température connue (`#temp_reference_value`) | bouton « Étape suivante » |
| 3 | Calibrer | bouton « Calibrer » |

- boutons **« Commencer la calibration » / « Étape suivante » / « Calibrer »** (`#temp_cal_start_btn`, libellé adapté à l'étape), **« Annuler »** (`#temp_cal_cancel_btn`, revient à l'idle sans modifier l'offset), et **« Fermer »** (`#temp_cal_close_btn`, `hideTempCalibrationCard()` restaure l'écran nominal et redonne le focus au bouton déclencheur).

### Calcul de l'offset (inchangé, côté client)

Comme pour la calibration ORP (voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md)), le calcul se fait **côté JS** :

```
offset = température de référence saisie − temperature_raw
```

`temperature_raw` (champ WS, sans offset) est utilisé en priorité ; repli sur `temperature − offset_précédent` si le champ est absent (firmware antérieur). Le résultat est persisté via `POST /save-config` (`temp_calibration_offset` + `temp_calibration_date` ISO 8601).

> ⚠️ **Depuis la v2.47.1 (`bug-114`), l'offset est ÉCRÊTÉ à ±5 °C côté firmware.** Une référence saisie très loin de la mesure (les bornes de saisie du champ `#temp_reference_value` vont de −10 à 50 °C) produit donc un offset **ramené à ±5 °C**, sans message d'erreur dans la réponse mais avec un `warning` au journal nommant la valeur demandée, la valeur retenue et l'IP cliente. Motif : un DS18B20 dérive de **dixièmes** de degré ; et cet offset alimente la commande `RT,<tempC>` de compensation de l'EZO pH, où un offset de +1000 vaut **≈ +0,15 pH** d'erreur et un offset très négatif une erreur **non bornée**. Détail des quatre points d'écrêtage : [`sensors.md`](../subsystems/sensors.md#les-quatre-points-de-clamp-de-loffset-de-calibration).

Après succès, l'UI met à jour optimiste la valeur affichée, recharge la config (`loadConfig()`), passe le chip à « Calibré », puis revient à l'écran nominal (`hideTempCalibrationCard()`).

## Données consommées (WebSocket `/ws`)

- `temperature` (float, °C) — température **calibrée** (brute + `tempCalibrationOffset` **écrêté**), affichée en grand et en lecture live
- `temperature_raw` (float, °C) — valeur brute DS18B20 avant offset, utilisée pour le calcul de calibration. **WS-seul** (absent de `/data`)
- `tempFrozenAlarm` (bool, v2.47.1) — sonde « eau » figée ; alimente le badge. **En parité** `/data` ↔ WS
- `sondes_identified` (bool) / `sondes_detected` (int) — servis des **deux** côtés ; discriminant de « Sonde eau non identifiée »

## Données consommées (`GET /get-config`)

- `temp_calibration_date` (ISO 8601 ou vide) — pilote l'état du chip
- `temp_calibration_offset` (float, °C) — affiché dans l'en-tête de la carte de calibration

## Actions

| Action | Endpoint | Payload | Auth |
|---|---|---|---|
| Calibrer la température (offset) | `POST /save-config` | config complète avec `temp_calibration_offset` + `temp_calibration_date` | CRITICAL |
| Activer / désactiver la mesure | `POST /save-config` | `temperature_enabled` | CRITICAL |

> feature-035 n'a créé aucun endpoint / topic / message WS. **feature-068 (v2.47.1)** ajoute en revanche le champ `tempFrozenAlarm` (`/data` + WS) et le topic MQTT retenu `{base}/temp_sensor_frozen` — voir [`docs/MQTT.md`](../MQTT.md#topic-et-entité-ajoutés-en-feature-068-sonde-de-température-figée-v2471).

## Règles firmware

- Sonde DS18B20 1-Wire lue toutes les **2 secondes** (`kTempSensorIntervalMs`, voir [`constants.h`](../../src/constants.h)).
- Formule appliquée : `temp_final = temp_raw + clampTempCalOffset(tempCalibrationOffset)` — offset borné à `[−5 ; +5]` °C depuis la v2.47.1 (`kTempCalOffsetMin/Max`).
- Lectures brutes bornées à `(−55 ; 125)` °C, avec rejet de `85.0` (power-on reset du DS18B20) et de `DEVICE_DISCONNECTED_C`.
- La température **envoyée à l'EZO pH** (`RT,<tempC>`) passe en plus par une fenêtre `[0 ; 40]` °C et une **cascade de repli** (mesure → dernière valeur plausible ≤ 20 s → 25 °C), journalisée. ⚠️ La borne haute de 40 °C est une borne de **prudence**, **non confirmée par la datasheet Atlas**. Voir [`sensors.md`](../subsystems/sensors.md#cascade-de-résolution-de-rt-bug-114).
- **Détection « sonde figée »** : 900 lectures valides consécutives dans une bande < 0,03 °C (≈ 30 min). **Aucun effet** sur le dosage ni sur le planning — uniquement l'exposition ([ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md)).
- Pas de filtre médiane/EMA, pas de compensation profondeur. La calibration reste **1 point par offset** (pas d'EZO, pas de pente).

## Cas limites

- **Sonde absente / NaN** : valeur et lecture live affichées `--`, chip neutre « Non calibré », calibration accessible (mais le bouton « Calibrer » alerte si aucune donnée capteur n'est disponible).
- **WebSocket déconnecté** : la valeur reste figée / `--`, pas de crash.
- **Jamais calibré** : chip gris « Non calibré », bouton « Calibrer la sonde » bien visible.
- **Annulation de calibration** : retour à l'état idle sans modifier l'offset.
- **Calibration aberrante** : bornes UI sur `#temp_reference_value` (`min=-10`, `max=50`) **et**, depuis la v2.47.1, **écrêtage firmware de l'offset à ±5 °C** aux quatre points de la chaîne (écriture, chargement NVS, émission de la mesure, émission `RT`) — une NVS déjà polluée par une version antérieure est donc également corrigée au boot, ce qui garantit que `/get-config` expose la valeur **réellement appliquée**.
- **Sonde figée** : badge « Sonde température figée », valeur **conservée** (plausible, seulement immobile), aucun effet métier.
- **Aucun rôle « eau » identifié** : badge « Sonde eau non identifiée » — jamais « eau figée ».

## Fichiers

- [`data/index.html`](../../data/index.html) — section `#view-temperature` (bloc stats + carte calibration masquée)
- [`data/app.js`](../../data/app.js) — `updateTempCalChip()`, `showTempCalibrationCard()` / `hideTempCalibrationCard()`, `updateTempCalibrationSteps()`, `bindTempCalibration()`
- [`data/app.css`](../../data/app.css) — `.ph-stats--single`, `.chip--cal`, `.cal-trigger-row` (réutilisés de feature-034)
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — `DallasTemperature`
- [`src/config.h`](../../src/config.h) — champs `tempCalibrationOffset`, `tempCalibrationDate`, `temperatureEnabled`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — persistance `/save-config`

## Specs liées

- feature-068 — exposition de la sonde de température figée (badge, `/data`/WS, MQTT) sans aucun effet métier (v2.47.1). Voir [ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md).
- bug-114 — écrêtage de l'offset de calibration à ±5 °C, fenêtre et cascade de la compensation `RT` (v2.47.1).
- feature-035 — uniformisation de l'écran Température avec pH/ORP (bloc stats + chip + carte de calibration masquée).
- feature-034 — patterns réutilisés (chip, carte masquée, bouton « Calibrer la sonde » sous les chips). Voir [page-ph.md](page-ph.md) / [page-orp.md](page-orp.md).
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calcul de calibration côté client.
