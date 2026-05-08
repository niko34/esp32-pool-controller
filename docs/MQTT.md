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
| `{base}/ph` | `7.234` | Valeur pH (**3 décimales** depuis 2.0.0 — voir [ADR-0014](adr/0014-migration-atlas-ezo.md)) |
| `{base}/orp` | `720` | Valeur ORP (mV) |
| `{base}/ph_cal_points` | `2` | Points de calibration EZO pH (entier `-1..3`, `-1` = EZO injoignable). Retain. Voir [feature-021](../specs/features/done/feature-021-migration-atlas-ezo.md). |
| `{base}/orp_cal_points` | `1` | Points de calibration EZO ORP (entier `-1..1`, `-1` = EZO injoignable). Retain. |
| `{base}/ph_slope_acid` | `99.7` | Pente acide sonde pH EZO en % (1 décimale). Retain. Edge-triggered ([feature-024](../specs/features/done/feature-024-pente-sonde-ph.md)). |
| `{base}/ph_slope_base` | `100.3` | Pente base sonde pH EZO en % (1 décimale). Retain. Edge-triggered. |
| `{base}/ph_slope_zero` | `-0.89` | Décalage zéro sonde pH EZO en mV (2 décimales). Retain. Non publié si firmware EZO ancien. |

### Filtration

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/filtration_state` | `ON` / `OFF` | État du relais de filtration |
| `{base}/filtration_mode` | `auto` / `manual` / `force` / `off` | Mode de filtration courant |

### Éclairage

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/lighting_state` | `ON` / `OFF` | État du relais d'éclairage |

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

### Consignes

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_target` | `7.2` | Consigne pH cible |
| `{base}/orp_target` | `700` | Consigne ORP cible (mV) |
| `{base}/ph_regulation_mode` | `automatic` / `scheduled` / `manual` | Mode de régulation pH actif |
| `{base}/ph_daily_target_ml` | `150` | Volume quotidien programmé pH (mL) — mode Programmée uniquement |
| `{base}/orp_regulation_mode` | `automatic` / `scheduled` / `manual` | Mode de régulation ORP actif |
| `{base}/orp_daily_target_ml` | `200` | Volume quotidien programmé chlore (mL) — mode Programmée uniquement |

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
| `{base}/filtration_mode/set` | `auto` / `manual` / `force` / `off` | Change le mode de filtration |
| `{base}/lighting/set` | `ON` / `OFF` | Allume/éteint l'éclairage |
| `{base}/ph_target/set` | `7.2` (6.0 – 8.5) | Change la consigne pH |
| `{base}/orp_target/set` | `700` (400 – 900) | Change la consigne ORP (mV) |

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
| `ph_limit` | Limite journalière de dosage pH atteinte |
| `orp_limit` | Limite journalière de dosage ORP atteinte |
| `ph_abnormal` | pH < 5.0 ou pH > 9.0 |
| `orp_abnormal` | ORP < 400 mV ou ORP > 900 mV |
| `temp_abnormal` | Température < 5°C ou > 40°C |
| `low_memory` | Mémoire heap disponible sous le seuil |

### Alertes dédiées (feature-021, retain, edge-triggered)

Ces deux topics sont **retain** : le dernier état persiste sur le broker même après un reboot. Une **payload vide** publiée en retain efface l'alerte (clear) — utile pour HA qui peut alors retirer le badge automatiquement.

| Topic | Payload alerte | Payload clear | Condition de bascule |
|-------|----------------|---------------|----------------------|
| `{base}/alerts/calibration_required` | JSON `{"type":"calibration_required","phCalPoints":<int>,"orpCalPoints":<int>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : `phCalPoints < 2` OU `orpCalPoints < 1`. Bascule **clear** : les deux capteurs OK. |
| `{base}/alerts/sensor_stale` | JSON `{"type":"sensor_stale","phStale":<bool>,"orpStale":<bool>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : `getPh()` ou `getOrp()` retourne NaN (lecture > `kSensorStaleTimeoutMs = 20 s`). Bascule **clear** : les deux lectures redeviennent valides. |

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
| Sensor | Volume pH Restant | `{base}/ph_remaining_ml` | — |
| Sensor | Volume Chlore Restant | `{base}/orp_remaining_ml` | — |
| Binary Sensor | Contrôleur Status | `{base}/status` | — |
| Select | Mode Filtration | `{base}/filtration_mode` | `{base}/filtration_mode/set` |
| Switch | Filtration Marche/Arrêt | `{base}/filtration_state` | `{base}/filtration/set` |
| Switch | Éclairage Piscine | `{base}/lighting_state` | `{base}/lighting/set` |
| Number | Consigne pH | `{base}/ph_target` | `{base}/ph_target/set` |
| Number | Consigne ORP | `{base}/orp_target` | `{base}/orp_target/set` |

> **Compatibilité `orp_enabled` :** le champ `orp_enabled` (booléen) est maintenu comme miroir de `orp_regulation_mode` (`true` si mode ≠ `manual`). Les automations Home Assistant qui testent la valeur de ce champ continuent de fonctionner sans modification. Le topic `{base}/orp_regulation_mode` est la source de vérité pour le mode actif.

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
