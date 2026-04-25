# ADR-0003 — Calibration ORP calculée côté client, pas par le firmware

- **Statut** : Accepté
- **Date** : antérieur à 2026-04 (comportement déjà en place lors de la refonte de la page ORP)
- **Doc(s) liée(s)** : [page-orp.md](../features/page-orp.md), [sensors.md](../subsystems/sensors.md)

## Contexte

La calibration pH est prise en charge par la bibliothèque `DFRobot_PH` : elle expose des endpoints firmware (`/calibrate_ph_neutral`, `/calibrate_ph_acid`, `/clear_ph_calibration`) qui déclenchent une écriture EEPROM côté sonde.

La calibration ORP, elle, n'a pas de primitive équivalente : aucune librairie tiers ne la gère. Il fallait donc décider **où** faire le calcul `offset` (1 point) ou `offset + slope` (2 points).

## Décision

La calibration ORP est **calculée côté client** (navigateur) à partir :
- de la tension brute courante du capteur (champ WebSocket `orp_voltage_mv` ou équivalent),
- de la ou des valeurs de référence saisies par l'utilisateur (solution d'étalonnage).

Les résultats (`orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_date`, `orp_calibration_reference`) sont persistés via l'endpoint générique **`POST /save-config`**. Aucun endpoint firmware dédié.

Le firmware applique la formule `ORP_final = ORP_brut × slope + offset` à chaque lecture ([`sensors.cpp`](../../src/sensors.cpp)).

## Alternatives considérées

- **Endpoints firmware dédiés** (type `POST /calibrate_orp_point1`, rejeté) — surface API plus large, code C++ dupliquant une logique triviale (deux moyennes et une division), et surtout plus difficile à itérer côté UI (chaque changement de protocole nécessite un upload firmware + filesystem).
- **Calibration côté firmware déclenchée par une commande unique** (`POST /calibrate_orp?ref=470`, rejeté) — idem, et empêche de prévisualiser la valeur calculée avant persistance.

## Conséquences

### Positives
- Le protocole d'assistant (1 point, 2 points, étapes guidées) est **entièrement en JS** : itérations rapides, pas de rebuild firmware.
- Le firmware reste simple : il ne connaît que la formule finale, pas la procédure.
- L'utilisateur voit la valeur brute en temps réel pendant la calibration (via WebSocket) sans endpoint spécifique.

### Négatives / dette assumée
- Si le client perd la connexion en plein milieu du calcul, la calibration est perdue (pas de reprise côté firmware).
- Un client malveillant peut envoyer un couple `(offset, slope)` arbitraire via `POST /save-config` (auth CRITICAL requise toutefois).
- La formule de calibration est **implicite au protocole** : si le protocole change (ex. 3 points), il faut mettre à jour le JS **et** documenter le changement dans la spec.

### Ce que ça verrouille
- Le firmware ne peut pas rejeter une calibration invalide sur la base du protocole : il accepte tout couple `(offset, slope)` cohérent avec les types. La validation métier est côté client.

## Références

- Code : [`src/sensors.cpp`](../../src/sensors.cpp) application de `orpCalibrationOffset` et `orpCalibrationSlope`
- Code : [`src/config.h`](../../src/config.h) struct `MqttConfig` champs ORP calibration
- Doc UI ORP : [page-orp.md](../features/page-orp.md) — protocole de calibration côté client
- API : [`docs/API.md`](../API.md) — il n'y a **pas** d'endpoint `/calibrate_orp_*` (différence avec pH qui en a)
