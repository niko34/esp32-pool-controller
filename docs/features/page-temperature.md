# Page Température — `/temperature`

- **Fichier UI** : [`data/index.html:487`](../../data/index.html:487) (section `#view-temperature`)
- **URL** : `http://poolcontroller.local/#/temperature`

## Rôle

Afficher la température courante et son historique, offrir une calibration par offset (si la sonde mesure légèrement à côté).

## Structure

- **Toggle « Gérer la température »** (`temperature_enabled`) — permet de masquer la fonctionnalité si la sonde n'est pas installée.
- **Valeur courante** — température en °C avec 1 décimale.
- **Graphique historique** — Chart.js, plages `24h` / `7j` / `30j` / `Tout`.
- **Carte Calibration** — saisie d'une valeur de référence (ex. thermomètre externe) + bouton Calibrer qui calcule l'offset = référence − valeur brute, et le persiste.
- **Date de calibration** — affichée à côté du bouton.

## Données consommées (WebSocket `/ws`)

- `temperature` (float, °C) — température calibrée (brute + `tempCalibrationOffset`)
- `temperature_raw` (float, °C) — valeur brute DS18B20 avant offset
- `temperature_ambient`, `temperature_ambient_raw` — si un capteur ambiant est ajouté (présent dans la structure WS, mais pas forcément dans l'UI par défaut)

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Calibrer la température | `POST /save-config` avec `temp_calibration_offset` et `temp_calibration_date` calculés côté client | CRITICAL |

Comme pour la calibration ORP (voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md)), **le calcul se fait côté JS** : l'utilisateur saisit la valeur réelle, le JS calcule `offset = ref - raw` et envoie via `POST /save-config`.

## Règles firmware

- Sonde DS18B20 1-Wire lue toutes les **2 secondes** (`kTempSensorIntervalMs = 2000`, voir [`constants.h:22`](../../src/constants.h:22)).
- Formule appliquée : `temp_final = temp_raw + tempCalibrationOffset` (voir [`config.h:87`](../../src/config.h:87)).
- Pas de compensation pour la température ambiante ni pour la profondeur de la sonde.

## Cas limites

- **Sonde déconnectée** : `temperature` reste à NAN → affichage `--` + badge d'erreur.
- **Calibration aberrante** : pas de garde-fou côté firmware si l'utilisateur entre un offset de 20 °C. À valider côté UI avant l'envoi.

## Fichiers

- [`data/index.html:487`](../../data/index.html:487)
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — `DallasTemperature`
- [`src/config.h:87`](../../src/config.h:87) — champs `tempCalibrationOffset`, `tempCalibrationDate`, `temperatureEnabled`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — persistance

## Specs historiques

Aucune spec dédiée à date.
