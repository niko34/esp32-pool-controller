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
| `{base}/ph` | `7.2` | Valeur pH (1 décimale) |
| `{base}/orp` | `720` | Valeur ORP (mV) |

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

### Consignes

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_target` | `7.2` | Consigne pH cible |
| `{base}/orp_target` | `700` | Consigne ORP cible (mV) |

### Système

| Topic | Payload | Rétention | Description |
|-------|---------|:---------:|-------------|
| `{base}/status` | `online` / `offline` | Oui | Disponibilité (LWT) |
| `{base}/alerts` | JSON | Non | Alertes en temps réel |
| `{base}/logs` | Texte | Non | Messages de log |
| `{base}/diagnostic` | JSON | Oui | Snapshot complet du système |

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
| Binary Sensor | Filtration Active | `{base}/filtration_state` | — |
| Binary Sensor | Dosage pH Actif | `{base}/ph_dosing` | — |
| Binary Sensor | Dosage Chlore Actif | `{base}/orp_dosing` | — |
| Binary Sensor | Limite Journalière pH | `{base}/ph_limit` | — |
| Binary Sensor | Limite Journalière Chlore | `{base}/orp_limit` | — |
| Binary Sensor | Contrôleur Status | `{base}/status` | — |
| Select | Mode Filtration | `{base}/filtration_mode` | `{base}/filtration_mode/set` |
| Switch | Filtration Marche/Arrêt | `{base}/filtration_state` | `{base}/filtration/set` |
| Switch | Éclairage Piscine | `{base}/lighting_state` | `{base}/lighting/set` |
| Number | Consigne pH | `{base}/ph_target` | `{base}/ph_target/set` |
| Number | Consigne ORP | `{base}/orp_target` | `{base}/orp_target/set` |
