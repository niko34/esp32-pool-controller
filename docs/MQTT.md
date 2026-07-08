# MQTT - ESP32 Pool Controller

## Configuration

| Paramètre | Description | Défaut |
|-----------|-------------|--------|
| Serveur | Adresse du broker MQTT | — |
| Port | Port du broker | 1883 |
| Topic de base | Préfixe de tous les topics | `pool/sensors` |
| Utilisateur / Mot de passe | Authentification broker (optionnel) | — |

Configuration via **Paramètres → MQTT** dans l'interface web ou via `POST /save-config`.

---

## Topics publiés

Tous les topics utilisent le préfixe configurable (ex: `pool/sensors`). Les valeurs sont publiées avec **rétention** (retain=true) sauf indication contraire.

### Capteurs

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/temperature` | `24.5` | Température de l'eau (°C) |
| `{base}/ph` | `7.234` | Valeur pH **filtrée** (médiane + EMA) depuis 2.2.0 — fallback brut si filtre non amorcé (**3 décimales** depuis 2.0.0 — voir [ADR-0014](adr/0014-migration-atlas-ezo.md)). Topics brut/médiane séparés ci-dessous (feature-025). |
| `{base}/orp` | `720` | Valeur ORP **filtrée** (mV) depuis 2.2.0 — fallback brut si filtre non amorcé |
| `{base}/ph_cal_points` | `2` | Points de calibration EZO pH (entier `-1..3`, `-1` = EZO injoignable). Retain. Voir [feature-021](../specs/features/done/feature-021-migration-atlas-ezo.md). |
| `{base}/orp_cal_points` | `1` | Points de calibration EZO ORP (entier `-1..1`, `-1` = EZO injoignable). Retain. |
| `{base}/ph_slope_acid` | `99.7` | Pente acide sonde pH EZO en % (1 décimale). Retain. Edge-triggered ([feature-024](../specs/features/done/feature-024-pente-sonde-ph.md)). |
| `{base}/ph_slope_base` | `100.3` | Pente base sonde pH EZO en % (1 décimale). Retain. Edge-triggered. |
| `{base}/ph_slope_zero` | `-0.89` | Décalage zéro sonde pH EZO en mV (2 décimales). Retain. Non publié si firmware EZO ancien. |

### Filtration

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/filtration_state` | `ON` / `OFF` | État du relais de filtration |
| `{base}/filtration_mode` | `auto` / `manual` / `force` / `off` | Mode de filtration courant (valeurs brutes sur le fil ; le select HA les affiche traduites — voir ci-dessous). Sémantique : `auto` = créneau calculé selon la température (heures en lecture seule) ; `manual` = **Programmation** (créneau à heures fixées par l'utilisateur) ; `force` = **Manuel** (contrôle ON/OFF sans planning) ; `off` = **Désactivé**. |
| `{base}/filtration_start` | `HH:MM` (ex. `08:00`) | Heure de début du créneau de filtration (feature-051, v2.16.0). Retain. En mode `auto`, recalculée selon la température (lecture seule) ; en mode `manual` (**Programmation**), heure fixée par l'utilisateur. |
| `{base}/filtration_end` | `HH:MM` (ex. `20:00`) | Heure de fin du créneau de filtration (feature-051, v2.16.0). Retain. Même sémantique. |

### Éclairage

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/lighting_state` | `ON` / `OFF` | État du relais d'éclairage (contrôle manuel) |

### Boost (feature-053, v2.18.0)

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/boost` | `ON` / `OFF` | État du **Mode Boost** (surchloration temporaire du jour). Retain. `ON` tant que le boost est actif ; retombe automatiquement à `OFF` au prochain minuit local (expiration). Voir [ADR-0025](adr/0025-mode-boost.md). |
| `{base}/lighting_schedule` | `ON` / `OFF` | Programmation horaire de l'éclairage active/inactive (feature-052, v2.17.0). Retain. Reflète le booléen `lightingCfg.scheduleEnabled`. Indépendant du relais manuel. Exposé en HA comme `select` « Mode Éclairage » (Programmation/Désactivé) via templates — le fil reste `ON`/`OFF` (bug-ha-eclairage-select, v2.17.2). |
| `{base}/lighting_start` | `HH:MM` (ex. `20:00`) | Heure de début du créneau d'éclairage (feature-052, v2.17.0). Retain. |
| `{base}/lighting_end` | `HH:MM` (ex. `23:30`) | Heure de fin du créneau d'éclairage (feature-052, v2.17.0). Retain. |

### Dosage

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_dosage` | `45.2` | Volume dosé pH aujourd'hui (ml) |
| `{base}/orp_dosage` | `120.5` | Volume dosé ORP/chlore aujourd'hui (ml) |
| `{base}/ph_dosing` | `ON` / `OFF` | Pompe doseuse pH en cours d'injection |
| `{base}/orp_dosing` | `ON` / `OFF` | Pompe doseuse ORP en cours d'injection |
| `{base}/ph_limit` | `ON` / `OFF` | Limite journalière pH atteinte |
| `{base}/orp_limit` | `ON` / `OFF` | Limite journalière ORP atteinte |
| `{base}/ph_stock_low` | `ON` / `OFF` | Volume pH restant sous le seuil d'alerte |
| `{base}/orp_stock_low` | `ON` / `OFF` | Volume chlore restant sous le seuil d'alerte |
| `{base}/ph_remaining_ml` | `1500` | Volume de produit pH restant dans le bidon (ml) |
| `{base}/orp_remaining_ml` | `3200` | Volume de produit chlore restant dans le bidon (ml) |
| `{base}/ph_daily_ml` | `45.2` | Cumul journalier injecté pH (mL) — retain, dédup au cycle 10 s, retombe à 0 à minuit (feature-050). Alimente l'entité HA `sensor` « Dosage pH aujourd'hui » |
| `{base}/orp_daily_ml` | `120.5` | Cumul journalier injecté chlore (mL) — retain, dédup au cycle 10 s, retombe à 0 à minuit (feature-050). Alimente l'entité HA `sensor` « Dosage Chlore aujourd'hui » |

### Consignes

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_target` | `7.2` | Consigne pH cible |
| `{base}/orp_target` | `700` | Consigne ORP cible (mV) |
| `{base}/ph_regulation_mode` | `automatic` / `scheduled` / `manual` | Mode de régulation pH actif — commandable via `{base}/ph_regulation_mode/set` depuis v2.7.0 (feature-009) |
| `{base}/ph_daily_target_ml` | `150` | Volume quotidien programmé pH (mL) — mode Programmée uniquement. Commandable via `{base}/ph_daily_target_ml/set` depuis v2.14.0 (feature-050) |
| `{base}/orp_regulation_mode` | `automatic` / `scheduled` / `manual` | Mode de régulation ORP actif — commandable via `{base}/orp_regulation_mode/set` depuis v2.7.0 |
| `{base}/orp_daily_target_ml` | `200` | Volume quotidien programmé chlore (mL) — mode Programmée uniquement. Commandable via `{base}/orp_daily_target_ml/set` depuis v2.14.0 (feature-050) |

### Système

| Topic | Payload | Rétention | Description |
|-------|---------|:---------:|-------------|
| `{base}/status` | `online` / `offline` | Oui | Disponibilité (LWT) |
| `{base}/alerts` | JSON | Non | Alertes en temps réel |
| `{base}/logs` | Texte | Non | Messages de log |
| `{base}/diagnostic` | JSON | Oui | Snapshot complet du système |

> **`reset_reason` (raison du dernier reboot) :** ce champ est disponible uniquement via le **WebSocket** (`/ws`, champ `reset_reason` dans le message `sensor_data`). Il n'est pas publié via MQTT. Voir [`docs/API.md`](API.md#ws-ws--write) pour les valeurs possibles.

---

## Topics de commande (souscription)

| Topic | Payload accepté | Action |
|-------|----------------|--------|
| `{base}/filtration/set` | `ON` / `OFF` | Force marche/arrêt filtration |
| `{base}/filtration_mode/set` | `auto` / `manual` / `force` / `off` | Change le mode de filtration (valeurs brutes attendues par le handler ; depuis HA le `command_template` retraduit le libellé français choisi vers ces valeurs) |
| `{base}/filtration_start/set` | `HH:MM` (ex. `07:30`) | Change l'heure de début du créneau (feature-051, v2.16.0). Validée `HH:MM` via `timeStringToMinutes` (format invalide → ignoré + `warning` + resync HA sur la valeur réelle). Efface les overrides `forceOn`/`forceOff`, applique via `filtration.update()`, persisté NVS, republié. **Effet réel en mode Programmation** ; en mode **Auto** l'heure est recalculée par la température au prochain `update()` (saisie écrasée, consultation) |
| `{base}/filtration_end/set` | `HH:MM` (ex. `20:30`) | Change l'heure de fin du créneau (symétrique, feature-051, v2.16.0) |
| `{base}/lighting/set` | `ON` / `OFF` | Allume/éteint l'éclairage (contrôle manuel du relais) |
| `{base}/boost/set` | `ON` / `OFF` | Active / désactive le **Mode Boost** (feature-053, v2.18.0). `ON` → active la surchloration temporaire du jour (filtration forcée + cible/limite chlore effectives relevées, cf. [ADR-0025](adr/0025-mode-boost.md)) ; `OFF` → désactive. **L'activation est refusée si l'heure n'est pas synchronisée** (l'expiration à minuit ne peut être calculée) → aucun changement + resync HA sur l'état réel. Payload invalide ignoré. Expire automatiquement au prochain minuit local. |
| `{base}/lighting_schedule/set` | `ON` / `OFF` | Active/désactive la programmation horaire (feature-052, v2.17.0). Payload validé `ON`/`OFF` (insensible à la casse) → `lightingCfg.scheduleEnabled` ; invalide → ignoré + `warning` + resync HA. Persisté NVS, appliqué via `lighting.update()`, republié. Broadcast WS config vers l'UI web |
| `{base}/lighting_start/set` | `HH:MM` (ex. `20:00`) | Change l'heure de début du créneau (feature-052, v2.17.0). Validée `HH:MM` via `timeStringToMinutes` (format invalide → ignoré + `warning` + resync HA). Pilote `lightingCfg.startTime`, persisté NVS, appliqué, republié |
| `{base}/lighting_end/set` | `HH:MM` (ex. `23:30`) | Change l'heure de fin du créneau (symétrique, `lightingCfg.endTime`, feature-052, v2.17.0) |
| `{base}/ph_target/set` | `7.2` (6.0 – 8.5) | Change la consigne pH |
| `{base}/orp_target/set` | `700` (400 – 900) | Change la consigne ORP (mV) |
| `{base}/ph_regulation_mode/set` | `automatic` / `scheduled` / `manual` | Change le mode de régulation pH (feature-009, v2.7.0). Insensible à la casse. Valeur hors enum → ignorée + log `warning` |
| `{base}/orp_regulation_mode/set` | `automatic` / `scheduled` / `manual` | Change le mode de régulation ORP (idem) |
| `{base}/ph_daily_target_ml/set` | `150` (0 – 2000 mL, entier) | Change le volume quotidien programmé pH (feature-050, v2.14.0). Payload non numérique → ignoré + log `warning` ; négatif → clampé à 0 ; **valeur > limite journalière VIVE `maxPhMlPerDay` → refusée** + `warning` + resync HA. Persisté NVS, republié |
| `{base}/orp_daily_target_ml/set` | `500` (0 – 2000 mL, entier) | Change le volume quotidien programmé chlore (symétrique pH). Refus si > limite journalière VIVE `maxChlorineMlPerDay` |
| `{base}/reboot/set` | `PRESS` (tout payload accepté) | Redémarre le contrôleur (feature-050, v2.14.0). Non retain. Redémarrage **différé propre** : flush MQTT `offline` puis restart, même séquence que la route `POST /reboot` |

---

## Alertes

Topic : `{base}/alerts` — QoS 0, sans rétention.

```json
{
  "type": "ph_abnormal",
  "message": "pH=4.8",
  "timestamp": 12345678
}
```

| Type | Condition |
|------|-----------|
| `ph_limit` | Limite journalière de dosage pH atteinte — émise **une seule fois, à la transition** vers la limite (v2.10.1 ; avant : répétée à chaque health check de 60 s). Événement non-retained : l'**état permanent** pour HA est porté par le binary_sensor `{base}/ph_limit` (retain). Libellé dynamique « pH+ » / « pH- » selon le produit configuré (`phCorrectionType`). Après un reboot avec limite déjà latchée : une alerte unique au premier health check (~60 s), comportement voulu. |
| `orp_limit` | Limite journalière de dosage ORP atteinte — même sémantique de transition que `ph_limit` (v2.10.1) ; état permanent : binary_sensor `{base}/orp_limit` (retain). |
| `ph_abnormal` | pH < 5.0 ou pH > 9.0 |
| `orp_abnormal` | ORP < 400 mV ou ORP > 900 mV |
| `temp_abnormal` | Température < 5°C ou > 40°C |
| `low_memory` | Mémoire heap disponible sous le seuil |
| `ph_injection_aborted` | Injection manuelle pH interrompue par la sécurité chimique : la filtration s'est arrêtée pendant l'injection (v2.1.2). Pas de reprise auto, l'utilisateur doit relancer manuellement après reprise filtration. |
| `orp_injection_aborted` | Injection manuelle ORP/chlore interrompue par la sécurité chimique (mêmes conditions que `ph_injection_aborted`). |

### Alertes dédiées (feature-021 / feature-022, retain, edge-triggered)

Ces topics sont **retain** : le dernier état persiste sur le broker même après un reboot. Une **payload vide** publiée en retain efface l'alerte (clear) — utile pour HA qui peut alors retirer le badge automatiquement.

| Topic | Payload alerte | Payload clear | Condition de bascule |
|-------|----------------|---------------|----------------------|
| `{base}/alerts/calibration_required` | JSON `{"type":"calibration_required","phCalPoints":<int>,"orpCalPoints":<int>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : `phCalPoints < 2` OU `orpCalPoints < 1`. Bascule **clear** : les deux capteurs OK. |
| `{base}/alerts/sensor_stale` | JSON `{"type":"sensor_stale","phStale":<bool>,"orpStale":<bool>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : `getPh()` ou `getOrp()` retourne NaN (lecture > `kSensorStaleTimeoutMs = 20 s`). Bascule **clear** : les deux lectures redeviennent valides. |
| `{base}/alerts/sensor_frozen` | JSON `{"type":"sensor_frozen","phFrozen":<bool>,"orpFrozen":<bool>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : capteur pH ou ORP **figé** (variance nulle sur 30 lectures — feature-022, v2.10.0). Bascule **clear** : les deux capteurs redeviennent vivants. La température figée (warning-only) n'est **pas** dans ce payload. |

Publication **edge-triggered** : un message n'est émis qu'à la transition (entrée ou sortie de l'état d'alerte), pas à chaque cycle MQTT. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md) et [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md).

---

## Diagnostic

Topic : `{base}/diagnostic` — publié au démarrage et à chaque reconnexion.

```json
{
  "uptime_ms": 123456789,
  "uptime_min": 2057,
  "free_heap": 45230,
  "wifi_ssid": "MonWiFi",
  "wifi_rssi": -65,
  "ip_address": "192.168.1.100",
  "sensors_initialized": true,
  "ph_value": 7.2,
  "orp_value": 720,
  "temperature": 24.5,
  "ph_dosing_active": false,
  "orp_dosing_active": false,
  "ph_daily_ml": 45.2,
  "orp_daily_ml": 120.5,
  "ph_limit_reached": false,
  "orp_limit_reached": false,
  "filtration_running": true,
  "filtration_mode": "auto",
  "ph_target": 7.2,
  "orp_target": 700.0,
  "firmware_version": "1.0.3"
}
```

---

## Home Assistant Auto-Discovery

Le contrôleur publie automatiquement sa configuration pour Home Assistant au démarrage.

**Préfixe discovery :** `homeassistant/`
**Device ID :** `poolcontroller`

| Type | Nom dans HA | Topic état | Topic commande |
|------|-------------|-----------|----------------|
| Sensor | Piscine Température | `{base}/temperature` | — |
| Sensor | Piscine pH | `{base}/ph` | — |
| Sensor | Piscine ORP | `{base}/orp` | — |
| Sensor | Piscine pH Points Calibrés | `{base}/ph_cal_points` | — |
| Sensor | Piscine ORP Points Calibrés | `{base}/orp_cal_points` | — |
| Binary Sensor | Filtration Active | `{base}/filtration_state` | — |
| Binary Sensor | Dosage pH Actif | `{base}/ph_dosing` | — |
| Binary Sensor | Dosage Chlore Actif | `{base}/orp_dosing` | — |
| Binary Sensor | Limite Journalière pH | `{base}/ph_limit` | — |
| Binary Sensor | Limite Journalière Chlore | `{base}/orp_limit` | — |
| Binary Sensor | Stock pH Faible | `{base}/ph_stock_low` | — |
| Binary Sensor | Stock Chlore Faible | `{base}/orp_stock_low` | — |
| Binary Sensor | Capteur pH — problème | `{base}/ph_sensor_problem` | — |
| Binary Sensor | Capteur ORP — problème | `{base}/orp_sensor_problem` | — |
| Sensor | Volume pH Restant | `{base}/ph_remaining_ml` | — |
| Sensor | Volume Chlore Restant | `{base}/orp_remaining_ml` | — |
| Binary Sensor | Contrôleur Status | `{base}/status` | — |
| Select | Mode Filtration | `{base}/filtration_mode` | `{base}/filtration_mode/set` |
| Text | Filtration début | `{base}/filtration_start` | `{base}/filtration_start/set` |
| Text | Filtration fin | `{base}/filtration_end` | `{base}/filtration_end/set` |
| Select | Mode Régulation pH | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` |
| Select | Mode Régulation ORP | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` |
| Switch | Filtration Marche/Arrêt | `{base}/filtration_state` | `{base}/filtration/set` |
| Switch | Éclairage Piscine | `{base}/lighting_state` | `{base}/lighting/set` |
| Switch | Boost | `{base}/boost` | `{base}/boost/set` |
| Select | Mode Éclairage | `{base}/lighting_schedule` | `{base}/lighting_schedule/set` |
| Text | Éclairage début | `{base}/lighting_start` | `{base}/lighting_start/set` |
| Text | Éclairage fin | `{base}/lighting_end` | `{base}/lighting_end/set` |
| Number | Consigne pH | `{base}/ph_target` | `{base}/ph_target/set` |
| Number | Consigne ORP | `{base}/orp_target` | `{base}/orp_target/set` |
| Sensor | Dosage pH aujourd'hui | `{base}/ph_daily_ml` | — |
| Sensor | Dosage Chlore aujourd'hui | `{base}/orp_daily_ml` | — |
| Number | Volume quotidien pH | `{base}/ph_daily_target_ml` | `{base}/ph_daily_target_ml/set` |
| Number | Volume quotidien Chlore | `{base}/orp_daily_target_ml` | `{base}/orp_daily_target_ml/set` |
| Button | Redémarrer | — | `{base}/reboot/set` |

> **Compatibilité `orp_enabled` :** le champ `orp_enabled` (booléen) est maintenu comme miroir de `orp_regulation_mode` (`true` si mode ≠ `manual`). Les automations Home Assistant qui testent la valeur de ce champ continuent de fonctionner sans modification. Le topic `{base}/orp_regulation_mode` est la source de vérité pour le mode actif.

> **Libellés du select « Mode Filtration » (bug-ha-filtration-mode-labels, v2.15.0) :** dans Home Assistant, ce select expose les libellés français **Auto / Programmation / Manuel / Désactivé** (alignés sur l'UI web), et non plus les valeurs brutes anglaises. La traduction est purement côté HA via templates de la discovery : `value_template` mappe l'état brut publié (`auto`/`manual`/`force`/`off`) vers le libellé affiché (avec `.get(value, value)` → un état inattendu passe sans casser), `command_template` mappe le libellé choisi vers la valeur brute envoyée sur `.../set`. **Le protocole MQTT sur le fil reste strictement `auto`/`manual`/`force`/`off`** (état publié comme commande attendue par `drainCommandQueue`) : aucune automatisation existante n'est cassée. Levée d'ambiguïté : `manual` = « Programmation » (créneau à heures fixées), `force` = « Manuel » (contrôle ON/OFF sans planning). Les selects `ph`/`orp_regulation_mode` affichent depuis v2.17.3 les libellés français **Automatique / Programmée / Manuelle** via la même technique (le fil reste `automatic`/`scheduled`/`manual`, voir bug-ha-regulation-mode-labels ci-dessous).

> **Select « Mode Éclairage » (bug-ha-eclairage-select, v2.17.2) :** la programmation horaire de l'éclairage était exposée en `switch` ON/OFF. Elle est désormais un `select` « Mode Éclairage » avec les libellés **Programmation / Désactivé**, pour cohérence visuelle avec le select « Mode Filtration ». Même technique que ce dernier : `value_template` (`{'ON':'Programmation','OFF':'Désactivé'}.get(value, value)`) et `command_template` (`{'Programmation':'ON','Désactivé':'OFF'}`) traduisent l'affichage HA. **Le protocole MQTT sur le fil reste strictement `ON`/`OFF`** ; le booléen `lightingCfg.scheduleEnabled` et le handler `lighting_schedule` (reçoit toujours ON/OFF) sont **inchangés**. `unique_id` conservé (`poolcontroller_lighting_schedule`). Migration : un payload retain vide est publié une fois sur l'ancien topic `homeassistant/switch/{HA_DEVICE_ID}_lighting_schedule/config` pour retirer le switch orphelin de HA.

## Topic et entité ajoutés en feature-020 (PCB v2)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/temperature_circuit` | T° de la sonde DS18B20 « circuit électronique » (NaN/null si non identifiée) | true | `sensor` "Piscine Température Circuit", `device_class: temperature`, `unit: °C`, `state_class: measurement` |

Le topic `{base}/temperature` (eau piscine) et son entité « Piscine Température » restent **inchangés** (rétrocompat HA).

Le bus OneWire (GPIO 5) supporte 2 sondes DS18B20 sur le PCB v2. Chaque sonde a un rôle (eau/circuit) identifié via Paramètres → Avancé. Voir [ADR-0013](adr/0013-identification-sondes-onewire.md).

## Topics et entités ajoutés en feature-021 (Atlas EZO pH/ORP, PCB v2)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/ph_cal_points` | Nombre de points calibrés EZO pH (-1 = injoignable, 0..3 sinon) | true | `sensor` "Piscine pH Points Calibrés" — `unique_id: poolcontroller_ph_cal_points`, `icon: mdi:numeric` |
| `{base}/orp_cal_points` | Nombre de points calibrés EZO ORP (-1 = injoignable, 0..1 sinon) | true | `sensor` "Piscine ORP Points Calibrés" — `unique_id: poolcontroller_orp_cal_points`, `icon: mdi:numeric` |
| `{base}/alerts/calibration_required` | Alerte calibration EZO incomplète. JSON ou payload vide (clear). | true | aucun (pour automation HA personnalisée) |
| `{base}/alerts/sensor_stale` | Alerte lecture pH/ORP stale (NaN > 20 s). JSON ou payload vide (clear). | true | aucun |

**Précision pH** : le topic `{base}/ph` publie désormais avec **3 décimales** (vs 1 décimale en v1.x). Tout consommateur HA qui parsait `int()` doit basculer sur `float()` ; les sensors HA standards (`device_class: ph`) gèrent cela nativement.

**Topics inchangés** (rétrocompat HA) : `{base}/orp`, `{base}/ph_target`, `{base}/orp_target`, `{base}/ph_dosing`, `{base}/orp_dosing`, `{base}/ph_limit`, `{base}/orp_limit`, `{base}/ph_regulation_mode`, `{base}/orp_regulation_mode`, etc. Les topics et entités HA de calibration ORP héritées (notamment `orp_cal_valid`) restent diffusés pour compatibilité, mais leur source de vérité côté firmware est désormais le module EZO (`orp_cal_points >= 1`).

Voir [ADR-0014](adr/0014-migration-atlas-ezo.md) (décision migration) et [`docs/subsystems/sensors.md`](subsystems/sensors.md) (détails techniques EZO + cache cal_points).

## Topics et entités ajoutés en feature-024 (pente sonde pH)

Diagnostic d'usure de la sonde pH via la commande Atlas `Slope,?`. Toutes les valeurs sont **strictement diagnostiques** — elles n'affectent ni `canDose()` ni le PID. L'évaluation des seuils (sonde excellente / correcte / usée / à remplacer) est faite côté UI, pas en firmware.

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/ph_slope_acid` | Pente acide en % (1 décimale, idéal 100 %) | true | `sensor` "Piscine pH Pente Acide" — `unique_id: poolcontroller_ph_slope_acid`, `unit: %`, `icon: mdi:angle-acute`, `state_class: measurement` |
| `{base}/ph_slope_base` | Pente base en % (1 décimale, idéal 100 %) | true | `sensor` "Piscine pH Pente Base" — `unique_id: poolcontroller_ph_slope_base`, `unit: %`, `icon: mdi:angle-obtuse`, `state_class: measurement` |
| `{base}/ph_slope_zero` | Décalage zéro en mV (2 décimales, idéal 0). Non publié tant que NaN — peut rester absent sur firmware EZO ancien qui ne renvoie que 2 floats. | true | `sensor` "Piscine pH Décalage Zéro" — `unique_id: poolcontroller_ph_slope_zero`, `unit: mV`, `icon: mdi:sine-wave`, `state_class: measurement` |

**Publication edge-triggered** : un message n'est émis qu'à la transition de la valeur **arrondie** (1 décimale pour les pentes, 2 pour le zéro). Pas de spam à chaque cycle — la query `Slope,?` n'est elle-même rafraîchie qu'au boot, après calibration EZO et toutes les 24 h.

**Pas de `binary_sensor` "à remplacer"** côté firmware : l'utilisateur peut le créer en automation HA depuis les 3 sensors selon ses propres seuils (par défaut UI : pente min ≥ 95 % et |zéro| ≤ 15 mV → vert ; < 85 % ou |zéro| > 30 mV → rouge).

Voir [`docs/features/page-ph.md`](features/page-ph.md#chip-détat-sonde-feature-024) (chip + modal UI) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#pente-sonde-ph--feature-024) (détails firmware : cache, fail streak, refresh policy).

## Topics et entités ajoutés en feature-025 (lissage mesures pH/ORP)

Lissage logiciel des mesures (médiane 7 + EMA). Le topic `{base}/ph` / `{base}/orp` publie désormais la valeur **filtrée** (cf. tableau Capteurs ci-dessus) ; les topics ci-dessous exposent en plus le brut, la médiane et l'état du filtre pour diagnostic EMI. Tous **retain**.

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_raw` | `7.46` | Dernière mesure pH **brute** Atlas (diagnostic EMI, 3 décimales) |
| `{base}/ph_median` | `7.43` | Médiane glissante pH (fenêtre 7) |
| `{base}/ph_filtered` | `7.43` | Mesure pH **filtrée** (EMA) — valeur de régulation |
| `{base}/ph_filter_ready` | `ON`/`OFF` | Filtre pH prêt (warmup terminé + mesure récente) |
| `{base}/ph_filter_unstable` | `ON`/`OFF` | Capteur pH instable (trop de rejets consécutifs) |
| `{base}/ph_rejected_count` | `3` | Nombre de mesures pH rejetées (glissant, 0..255) |
| `{base}/orp_raw` | `690` | Dernière mesure ORP **brute** Atlas (mV) |
| `{base}/orp_median` | `682` | Médiane glissante ORP (mV) |
| `{base}/orp_filtered` | `682` | Mesure ORP **filtrée** (mV) — valeur de régulation |
| `{base}/orp_filter_ready` | `ON`/`OFF` | Filtre ORP prêt |
| `{base}/orp_filter_unstable` | `ON`/`OFF` | Capteur ORP instable |
| `{base}/orp_rejected_count` | `2` | Nombre de mesures ORP rejetées |
| `{base}/ph_mixing_delay_active` | `ON`/`OFF` | Pause mélange pH active (15 min après injection) |
| `{base}/orp_mixing_delay_active` | `ON`/`OFF` | Pause mélange ORP active (20 min après injection) |

### Auto-discovery HA (feature-025)

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Piscine pH Brut | `sensor` | `{base}/ph_raw` | `poolcontroller_ph_raw` | `unit: pH`, `icon: mdi:water-outline`, `state_class: measurement` |
| Piscine pH Filtré | `sensor` | `{base}/ph_filtered` | `poolcontroller_ph_filtered` | `unit: pH`, `icon: mdi:water-check`, `state_class: measurement` |
| Piscine ORP Brut | `sensor` | `{base}/orp_raw` | `poolcontroller_orp_raw` | `unit: mV`, `icon: mdi:flash-outline`, `state_class: measurement` |
| Piscine ORP Filtré | `sensor` | `{base}/orp_filtered` | `poolcontroller_orp_filtered` | `unit: mV`, `icon: mdi:flash-alert`, `state_class: measurement` |
| Piscine Filtre pH Prêt | `binary_sensor` | `{base}/ph_filter_ready` | `poolcontroller_ph_filter_ready` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:filter-check` |
| Piscine Filtre ORP Prêt | `binary_sensor` | `{base}/orp_filter_ready` | `poolcontroller_orp_filter_ready` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:filter-check` |

> Le préfixe réel `unique_id` est `HA_DEVICE_ID` (`poolcontroller`). Les topics `median`, `filter_unstable`, `rejected_count` et `mixing_delay_active` sont publiés en retain mais **sans** entité auto-discovery dédiée (l'utilisateur peut créer des sensors HA manuels s'il le souhaite). Voir [`src/mqtt_manager.cpp`](../src/mqtt_manager.cpp) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#filtrage-des-mesures-phorp--feature-025).

## Entités ajoutées en feature-009 (modes de régulation commandables, v2.7.0)

Deux entités HA `select` permettent de changer le mode de régulation pH / ORP depuis Home Assistant (automatisation « hivernage », détection d'absence, dashboard HA, etc.) :

| Entité | Discovery topic | `state_topic` | `command_topic` | Détails |
|--------|-----------------|---------------|-----------------|---------|
| Mode Régulation pH | `homeassistant/select/poolcontroller_ph_regulation_mode/config` | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` | `unique_id: poolcontroller_ph_regulation_mode`, `icon: mdi:ph`, `options: ["Automatique","Programmée","Manuelle"]` + templates (voir note ci-dessous) |
| Mode Régulation ORP | `homeassistant/select/poolcontroller_orp_regulation_mode/config` | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` | `unique_id: poolcontroller_orp_regulation_mode`, `icon: mdi:flash`, mêmes options + templates |

> **Libellés français des selects Mode Régulation pH/ORP (bug-ha-regulation-mode-labels, v2.17.3) :** dans Home Assistant, ces deux selects exposent les libellés français **Automatique / Programmée / Manuelle** (libellés exacts de l'UI web), et non plus les valeurs brutes anglaises. Même technique que les selects « Mode Filtration » et « Mode Éclairage » : `value_template` (`{'automatic':'Automatique','scheduled':'Programmée','manual':'Manuelle'}.get(value, value)`) mappe l'état brut publié vers le libellé, `command_template` (`{'Automatique':'automatic','Programmée':'scheduled','Manuelle':'manual'}[value]`) retraduit le libellé choisi vers la valeur brute envoyée sur `.../set`. **Le protocole MQTT sur le fil reste strictement `automatic` / `scheduled` / `manual`** (topics d'état et handlers `PhRegulationMode` / `OrpRegulationMode` inchangés) : aucune automatisation Home Assistant existante n'est cassée.

Comportement :

- **Topics state inchangés** (retain, format existant) — rétrocompat totale des dashboards HA existants. Les topics de commande `.../set` sont **non retain**.
- **Validation stricte** de l'enum (comparaison après normalisation en minuscules) : toute valeur hors `automatic` / `scheduled` / `manual` est **ignorée** avec un log `warning` (« Mode régulation pH/ORP invalide (MQTT): ... »), aucun republish.
- **Même voie que l'UI web** : application sous `configMutex`, miroir booléen `ph_enabled` / `orp_enabled` mis à jour (`true` si mode ≠ `manual`, voir [ADR-0004](adr/0004-mode-regulation-enum-3-valeurs.md)), persistance NVS **uniquement si changement réel**. Republication immédiate de l'état (feedback HA).
- **Changement pendant une injection en cours** : l'injection se termine naturellement, le nouveau mode prend effet au cycle de régulation suivant.
- Pas de `switch` HA séparé pour `ph_enabled` / `orp_enabled` : le `select` est la source de vérité (les miroirs booléens restent publiés pour rétrocompat).

Voir [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md#gestion-des-commandes-ha-reçues) (handler) et [`docs/features/page-ph.md`](features/page-ph.md#interaction-mqtt) / [`page-orp.md`](features/page-orp.md#interaction-mqtt).

## Topics et entités ajoutés en feature-022 (problème capteur pH/ORP, v2.10.0)

Observabilité de l'indisponibilité capteur pour Home Assistant : un état binaire **par capteur**, directement consommable par un `binary_sensor` HA sans parser les JSON d'alerte.

### États binaires `{base}/ph_sensor_problem` / `{base}/orp_sensor_problem`

| Topic | Payload | Retain | Condition |
|-------|---------|:------:|-----------|
| `{base}/ph_sensor_problem` | `ON` / `OFF` | oui | `ON` si le capteur pH est **stale** (lecture NaN > 20 s, cf. `alerts/sensor_stale`) **OU figé** (variance nulle, cf. `alerts/sensor_frozen`). `OFF` sinon. |
| `{base}/orp_sensor_problem` | `ON` / `OFF` | oui | Symétrique ORP. |

Publication **dédupliquée** : le message n'est émis qu'au changement d'état (cache mémorisé, première publication forcée après connexion). Retain → HA retrouve l'état après un reboot du contrôleur ou de HA.

### Auto-discovery HA

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Capteur pH — problème | `binary_sensor` | `{base}/ph_sensor_problem` | `poolcontroller_ph_sensor_problem` | `device_class: problem`, `payload_on: ON`, `payload_off: OFF`, `icon: mdi:alert` |
| Capteur ORP — problème | `binary_sensor` | `{base}/orp_sensor_problem` | `poolcontroller_orp_sensor_problem` | `device_class: problem`, mêmes payloads/icône |

### Alerte `{base}/alerts/sensor_frozen`

Calquée sur `alerts/sensor_stale` (retain, edge-triggered, clear = payload vide) — voir le tableau [Alertes dédiées](#alertes-dédiées-feature-021--feature-022-retain-edge-triggered). Publiée à la transition **figé** d'au moins un capteur pH/ORP :

```json
{ "type": "sensor_frozen", "phFrozen": true, "orpFrozen": false, "timestamp": 12345678 }
```

Un capteur figé (30 lectures acceptées dans une bande < ½ LSB, ≈ 2,5 min) rend le filtre non prêt → **dosage automatique bloqué fail-closed** (garde `FilterNotReady` existante). La **température figée** (détection warning-only, aucun impact dosage) n'apparaît **ni** dans cette alerte **ni** dans les états `*_sensor_problem`. Détails firmware : [`docs/subsystems/sensors.md`](subsystems/sensors.md#détection-capteur-figé--feature-022).

## Topics et entités ajoutés en feature-050 (complétude MQTT/HA, v2.14.0)

Issus d'un audit de couverture web/API vs MQTT/HA (périmètre « rentable »). Trois compléments : cumuls journaliers exposés, volumes quotidiens programmés commandables, bouton redémarrage.

### Cumuls journaliers injectés (P1)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/ph_daily_ml` | Cumul journalier injecté pH en mL (`safetyLimits.dailyPhInjectedMl`), publié au cycle 10 s avec dédup, retombe à 0 à minuit | true | `sensor` « Dosage pH aujourd'hui » — `unique_id: poolcontroller_ph_daily_ml`, `unit: mL`, `state_class: measurement` |
| `{base}/orp_daily_ml` | Cumul journalier injecté chlore en mL (`safetyLimits.dailyOrpInjectedMl`), idem | true | `sensor` « Dosage Chlore aujourd'hui » — `unique_id: poolcontroller_orp_daily_ml`, `unit: mL`, `state_class: measurement` |

Complètent les `binary_sensor` « Limite Journalière pH/Chlore » existants (état atteint/non atteint) par la valeur numérique du cumul.

### Volume quotidien programmé commandable (P2)

Les topics d'état `{base}/ph_daily_target_ml` / `{base}/orp_daily_target_ml` (mode Programmée) deviennent commandables via `.../set` + 2 entités HA `number`.

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Volume quotidien pH | `number` | `{base}/ph_daily_target_ml` | `{base}/ph_daily_target_ml/set` | `poolcontroller_ph_daily_target` | min 0 / max 2000 / step 10, `unit: mL` |
| Volume quotidien Chlore | `number` | `{base}/orp_daily_target_ml` | `{base}/orp_daily_target_ml/set` | `poolcontroller_orp_daily_target` | min 0 / max 2000 / step 10, `unit: mL` |

Validation du handler (`drainCommandQueue`, sous `configMutex`) : payload non numérique → ignoré + `warning` ; négatif → clampé à 0 ; **valeur strictement supérieure à la limite journalière VIVE (`maxPhMlPerDay` / `maxChlorineMlPerDay`) lue au moment du drain → refusée** + `warning` + resync HA sur la valeur réelle (condition posée par `pool-chemistry`). Persistance NVS **si changement réel**, republication immédiate, log `info` « ... (MQTT) ». La borne discovery (2000 mL) est indicative ; la limite vive reste le garde-fou effectif.

### Bouton Redémarrer (P3)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/reboot/set` | Tout payload accepté (le bouton HA envoie `PRESS`). Redémarrage **différé propre** : flag consommé en tête de `drainCommandQueue()` au prochain tour de loop, après `kRestartApModeDelayMs`, puis `shutdownForRestart()` (flush `status=offline`, ADR-0011) + `ESP.restart()` | non | `button` « Redémarrer » — `unique_id: poolcontroller_reboot`, `device_class: restart` |

Jamais de `ESP.restart()` direct au moment du drain — même séquence que la route `POST /reboot`.

---

## Topics et entités ajoutés en feature-051 (heures de filtration éditables, v2.16.0)

Expose les heures du créneau de filtration en entités HA `text` **éditables** (composant `text/`), complétant le select « Mode Filtration ».

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Filtration début | `text` | `{base}/filtration_start` | `{base}/filtration_start/set` | `poolcontroller_filtration_start` | `pattern` `^([01][0-9]|2[0-3]):[0-5][0-9]$`, min 5 / max 5, `icon: mdi:clock-start` |
| Filtration fin | `text` | `{base}/filtration_end` | `{base}/filtration_end/set` | `poolcontroller_filtration_end` | idem, `icon: mdi:clock-end` |

Les états `{base}/filtration_start` / `{base}/filtration_end` (retain) sont publiés par `publishFiltrationState()` (valeurs `filtrationCfg.start` / `.end`) — au boot lors de la (re)connexion, à chaque changement, et au cycle périodique de `publishAllStatesInternal()`.

**Sémantique** (identique à l'UI web) : les heures ont un effet réel en mode **Programmation** (`manual`). En mode **Auto**, elles sont recalculées par la température au prochain `filtration.update()` → une saisie HA en mode auto est écrasée et l'état republié montre la valeur recalculée (consultation, auto-corrigé). Le `pattern` de la discovery valide le format côté HA ; le handler revalide `HH:MM` via `timeStringToMinutes` (défense en profondeur). **Aucun chemin de dosage touché** : le dosage teste l'état live `filtration.isRunning()`, pas le planning.

---

## Topics et entités ajoutés en feature-052 (planning éclairage HA, v2.17.0)

Miroir de la feature-051 pour l'éclairage : expose le planning horaire (activation + heures) en entités HA éditables, complétant le switch « Éclairage Piscine » (contrôle manuel du relais, **inchangé**).

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Mode Éclairage | `select` | `{base}/lighting_schedule` | `{base}/lighting_schedule/set` | `poolcontroller_lighting_schedule` | Options **Programmation / Désactivé** ; templates ↔ `ON`/`OFF` sur le fil → booléen `lightingCfg.scheduleEnabled`. `switch` à l'origine (feature-052), migré en `select` en bug-ha-eclairage-select (v2.17.2) |
| Éclairage début | `text` | `{base}/lighting_start` | `{base}/lighting_start/set` | `poolcontroller_lighting_start` | `pattern` `^([01][0-9]|2[0-3]):[0-5][0-9]$`, min 5 / max 5, `icon: mdi:clock-start` |
| Éclairage fin | `text` | `{base}/lighting_end` | `{base}/lighting_end/set` | `poolcontroller_lighting_end` | idem, `icon: mdi:clock-end` |

Les états `{base}/lighting_schedule` / `{base}/lighting_start` / `{base}/lighting_end` (retain) sont publiés par `publishLightingState()` enrichie (valeurs `lightingCfg.scheduleEnabled` / `.startTime` / `.endTime`) — au boot lors de la (re)connexion, à chaque changement, et au cycle périodique de `publishAllStatesInternal()`.

**Choix booléen conservé** (cf. feature-028) : `scheduleEnabled` reste un booléen côté firmware et dans `/get-config` (`lighting_schedule_enabled`) ; l'exposition HA a d'abord été un `switch` (ON/OFF), migrée en `select` « Mode Éclairage » en v2.17.2 (le fil reste ON/OFF, voir la note ci-dessus). Un payload invalide (ni `ON` ni `OFF`, ou `HH:MM` mal formé) est ignoré + `warning` + resync HA sur la valeur réelle. Le broadcast WS config (bug-sync, v2.14.1) propage tout changement HA vers l'UI web sans rechargement. **Aucun chemin de dosage touché.**

---

## Topic et entité ajoutés en feature-053 (Mode Boost, v2.18.0)

Le **Mode Boost** est une surchloration temporaire du jour (filtration prolongée + cible ORP relevée), auto-expirant au prochain minuit local. Détails firmware : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#mode-boost-feature-053). Décision structurante : [ADR-0025](adr/0025-mode-boost.md).

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Boost | `switch` | `{base}/boost` | `{base}/boost/set` | `poolcontroller_boost` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:flash` |

L'état `{base}/boost` (retain) est publié à la (re)connexion, à chaque changement (activation / désactivation / expiration) et au cycle périodique de publication. Le switch retombe **automatiquement** sur `OFF` à minuit local sans commande HA (l'expiration côté firmware republie l'état).

**Activation refusée sans heure synchronisée** : si `{base}/boost/set` reçoit `ON` alors que l'horloge n'est pas synchronisée, la commande est **ignorée** (l'expiration à minuit serait incalculable) et l'état réel (`OFF`) est republié → le switch HA se resynchronise. Même politique de sécurité que la route HTTP `POST /boost/start` (`409 time_not_synced`, voir [API.md](API.md#post-booststart--write)).

> **Effet chlore gaté au mode ORP `automatic`** : le relèvement de la cible ORP et de la limite journalière chlore ne s'applique qu'en mode de régulation ORP `automatic`. En mode Manuel / Programmé, l'activation du Boost n'étend **que la filtration** (l'injection manuelle reste bornée à la limite normale). Voir [ADR-0025](adr/0025-mode-boost.md).

---

## Exclusions volontaires (politique MQTT/HA)

Cette section documente ce qui **n'est délibérément PAS exposé** en MQTT / Home Assistant. C'est une **politique assumée**, pas un backlog : l'audit de couverture (feature-050) a tranché chaque cas ci-dessous. Une ré-exposition future doit être justifiée par un besoin constaté, pas par principe de complétude.

| Non exposé | Pourquoi |
|-----------|----------|
| **Injections manuelles** (déclenchement direct d'un dosage pH/chlore) | Sécurité chimique : aucun dosage ne doit pouvoir être déclenché par une automatisation sans supervision humaine. Une injection reste une action locale (UI web / écran), sous présence de l'opérateur. |
| **Limites journalières / horaires de dosage** (configurables) | Ce sont des **garde-fous**, pas des préférences d'usage. Les rendre commandables à distance affaiblirait la barrière de sécurité qu'ils constituent (une automatisation HA pourrait relever le plafond avant de doser). Exposition écartée dès la feature-013. |
| **Calibration EZO pH/ORP** | Requiert la présence physique (solutions étalon, sonde plongée). Action non automatisable à distance. |
| **OTA / mise à jour firmware** | Flux supervisé interactivement, avec vérification CRC/signature. Pas de déclenchement fire-and-forget. |
| **Factory reset** | Action destructive irréversible (efface config/NVS). Jamais exposée à une automatisation. |
| **Wi-Fi (scan, connexion, changement)** | Configuration réseau sensible ; couper le Wi-Fi via MQTT couperait le canal MQTT lui-même. Présence physique ou UI locale. |
| **Authentification (token, mot de passe)** | Secret ; jamais transporté ni modifiable via MQTT. |
| **Pompes en mode test** | Actionnement direct des pompes pour maintenance — présence physique requise, potentiellement destructif (dosage à sec). |
| **Transients d'affichage** : délai de stabilisation restant, `inject_remaining`, **cause de blocage dosage** | États de transition à durée de vie très courte, pertinents seulement pour l'UI temps réel (WebSocket). La cause de blocage dosage a été explicitement écartée en feature-022, sur besoin constaté (bruit pour HA, pas de valeur d'automatisation). |
| **Historique** (brut / horaire / quotidien) | Home Assistant dispose de son propre recorder / long-term statistics qui agrège les `state_class: measurement` déjà publiés. Dupliquer l'historique firmware vers MQTT serait redondant. |
