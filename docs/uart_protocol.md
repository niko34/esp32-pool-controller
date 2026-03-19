# Protocole UART — ESP32 Pool Controller

## Configuration physique

| Paramètre | Valeur |
|-----------|--------|
| Interface | Serial2 (UART2) |
| RX | GPIO16 |
| TX | GPIO17 |
| Baud | 115200 |
| Format | 8N1 |
| Terminateur | `\n` |

> Serial (USB) reste disponible pour les logs debug. Ne jamais envoyer de commandes sur Serial.

---

## Format général

Chaque message est une ligne JSON sur une seule ligne, terminée par `\n`.

- **Requête** (écran → contrôleur) : `{"cmd":"<commande>","data":{...}}`
- **Réponse** (contrôleur → écran) : `{"type":"<type>","data":{...}}`
- **Événement asynchrone** (contrôleur → écran) : `{"type":"event"|"alarm","event":"<nom>","data":{...}}`

---

## Commandes disponibles

### `ping`
```json
→ {"cmd":"ping"}
← {"type":"pong"}
```

### `get_info`
```json
→ {"cmd":"get_info"}
← {"type":"info","data":{"firmware":"2.5.5","build_date":"Mar 10 2026","build_time":"12:00:00","board":"esp32_pool_controller","uptime_s":3600,"free_heap":102400}}
```

### `get_status`
```json
→ {"cmd":"get_status"}
← {
    "type": "status",
    "data": {
      "ph": 7.2,
      "orp": 650,
      "temperature": 25.5,
      "filtration_running": true,
      "filtration_mode": "auto",
      "ph_dosing": false,
      "orp_dosing": false,
      "ph_daily_ml": 125,
      "orp_daily_ml": 80,
      "ph_limit_reached": false,
      "orp_limit_reached": false,
      "lighting_on": false,
      "wifi_connected": true,
      "wifi_ssid": "MonReseau",
      "wifi_ip": "192.168.1.100",
      "wifi_rssi": -55,
      "mqtt_connected": false,
      "time_synced": true,
      "time_current": "2026-03-10T14:30:00"
    }
  }
```

> Les valeurs capteurs sont `null` si le capteur n'est pas disponible.

### `get_config`
```json
→ {"cmd":"get_config"}
← {
    "type": "config",
    "data": {
      "ph_target": 7.2,
      "orp_target": 650.0,
      "ph_enabled": true,
      "orp_enabled": true,
      "ph_pump": 1,
      "orp_pump": 2,
      "regulation_mode": "pilote",
      "ph_correction_type": "ph_minus",
      "ph_injection_limit_s": 60,
      "orp_injection_limit_s": 60,
      "filtration_enabled": true,
      "filtration_mode": "auto",
      "filtration_start": "08:00",
      "filtration_end": "20:00",
      "lighting_feature_enabled": true,
      "lighting_enabled": false,
      "lighting_brightness": 255,
      "lighting_schedule_enabled": false,
      "lighting_start_time": "20:00",
      "lighting_end_time": "23:00",
      "max_ph_ml_per_day": 500.0,
      "max_chlorine_ml_per_day": 300.0,
      "time_use_ntp": true,
      "ntp_server": "pool.ntp.org",
      "timezone_id": "europe_paris",
      "ph_calibration_date": "2026-03-01T10:30:00",
      "orp_calibration_date": "",
      "orp_calibration_offset": 0.0,
      "orp_calibration_slope": 1.0,
      "temp_calibration_date": "",
      "temp_calibration_offset": 0.0,
      "temperature_enabled": true
    }
  }
```

### `get_alarms`
```json
→ {"cmd":"get_alarms"}
← {"type":"alarms","data":[
    {"code":"PH_LIMIT","message":"Limite journalière pH atteinte"},
    {"code":"ORP_ABNORMAL","message":"Valeur ORP anormale: 350 mV"}
  ]}
```

> Renvoie un tableau vide si aucune alarme active : `{"type":"alarms","data":[]}`

**Codes d'alarme possibles :**

| Code | Déclenchement |
|------|--------------|
| `PH_LIMIT` | Limite journalière pH atteinte |
| `ORP_LIMIT` | Limite journalière ORP/chlore atteinte |
| `PH_ABNORMAL` | pH < 5.0 ou > 9.0 |
| `ORP_ABNORMAL` | ORP < 400 ou > 900 mV |
| `TEMP_ABNORMAL` | Température < 5 °C ou > 40 °C |

### `get_network_status`
```json
→ {"cmd":"get_network_status"}
← {"type":"network_status","data":{"wifi_mode":"STA","connected":true,"ssid":"MonReseau","ip":"192.168.1.100","rssi":-55,"mqtt_connected":false}}
```

### `set_config`
Modifie un ou plusieurs champs de configuration. Seuls les champs présents sont mis à jour.

```json
→ {"cmd":"set_config","data":{"ph_target":7.3,"orp_target":680}}
← {"type":"ack","cmd":"set_config"}
```

**Champs modifiables :**

| Champ | Type | Validation |
|-------|------|------------|
| `ph_target` | float | [5.0 – 9.0] |
| `orp_target` | float | [200 – 900] |
| `ph_enabled` | bool | — |
| `orp_enabled` | bool | — |
| `regulation_mode` | string | `"continu"` ou `"pilote"` |
| `ph_correction_type` | string | `"ph_minus"` ou `"ph_plus"` |
| `filtration_mode` | string | `"auto"`, `"manual"`, `"off"` |
| `filtration_enabled` | bool | — |
| `filtration_start` | string | `"HH:MM"` |
| `filtration_end` | string | `"HH:MM"` |
| `lighting_enabled` | bool | — |
| `lighting_brightness` | int | [0 – 255] |
| `lighting_schedule_enabled` | bool | — |
| `lighting_start_time` | string | `"HH:MM"` |
| `lighting_end_time` | string | `"HH:MM"` |
| `max_ph_ml_per_day` | float | > 0 |
| `max_chlorine_ml_per_day` | float | > 0 |

### `save_config`
Force la sauvegarde de la configuration sur le filesystem.

```json
→ {"cmd":"save_config"}
← {"type":"ack","cmd":"save_config"}
```

### `run_action`

#### `pump_test` — test manuel d'une pompe
```json
→ {"cmd":"run_action","data":{"action":"pump_test","pump":1,"duty":100}}
← {"type":"ack","cmd":"run_action"}
```
- `pump` : 1 ou 2
- `duty` : 0–255 (optionnel, défaut 255)

#### `pump_stop` — arrêt pompe
```json
→ {"cmd":"run_action","data":{"action":"pump_stop","pump":1}}
← {"type":"ack","cmd":"run_action"}
```

#### `lighting_on` / `lighting_off`
```json
→ {"cmd":"run_action","data":{"action":"lighting_on"}}
← {"type":"ack","cmd":"run_action"}
```

#### `filtration_mode` — changement de mode
```json
→ {"cmd":"run_action","data":{"action":"filtration_mode","mode":"manual"}}
← {"type":"ack","cmd":"run_action"}
← {"type":"event","event":"mode_changed","data":{"mode":"manual"}}
```
- `mode` : `"auto"`, `"manual"`, `"off"`

#### `calibrate_ph_neutral` — calibration pH 7.0
```json
→ {"cmd":"run_action","data":{"action":"calibrate_ph_neutral"}}
← {"type":"ack","cmd":"run_action"}
← {"type":"event","event":"calibration_done","data":{"sensor":"ph_neutral"}}
```

#### `calibrate_ph_acid` — calibration pH 4.0
```json
→ {"cmd":"run_action","data":{"action":"calibrate_ph_acid"}}
← {"type":"ack","cmd":"run_action"}
← {"type":"event","event":"calibration_done","data":{"sensor":"ph_acid"}}
```

#### `clear_ph_calibration` — effacement calibration pH
```json
→ {"cmd":"run_action","data":{"action":"clear_ph_calibration"}}
← {"type":"ack","cmd":"run_action"}
```

#### `ack_alarm` — acquittement d'alarme
```json
→ {"cmd":"run_action","data":{"action":"ack_alarm","code":"PH_LIMIT"}}
← {"type":"ack","cmd":"run_action"}
← {"type":"alarm","event":"cleared","data":{"code":"PH_LIMIT"}}
```
- `code` : `"PH_LIMIT"` ou `"ORP_LIMIT"`

---

## Réponses d'erreur

```json
{"type":"error","message":"invalid json"}
{"type":"error","message":"missing cmd field"}
{"type":"error","cmd":"run_action","message":"unknown action: foo"}
{"type":"error","cmd":"set_config","message":"ph_target hors plage [5.0-9.0]"}
{"type":"error","cmd":"run_action","message":"calibrate_ph_neutral: bus I2C occupé"}
```

---

## Événements asynchrones (push)

Ces messages sont envoyés spontanément par le contrôleur sans requête préalable.

### Changement d'état filtration
```json
{"type":"event","event":"filtration_changed","data":{"running":true}}
```

### Changement d'état dosage
```json
{"type":"event","event":"dosing_changed","data":{"ph":true,"orp":false}}
```

### Changement de mode filtration
```json
{"type":"event","event":"mode_changed","data":{"mode":"auto"}}
```

### Fin de calibration
```json
{"type":"event","event":"calibration_done","data":{"sensor":"ph_neutral"}}
{"type":"event","event":"calibration_done","data":{"sensor":"ph_acid"}}
```

### Alarme levée
```json
{"type":"alarm","event":"raised","data":{"code":"PH_LIMIT","message":"Limite journalière pH atteinte"}}
{"type":"alarm","event":"raised","data":{"code":"ORP_LIMIT","message":"Limite journalière ORP/chlore atteinte"}}
```

### Alarme effacée
```json
{"type":"alarm","event":"cleared","data":{"code":"PH_LIMIT"}}
{"type":"alarm","event":"cleared","data":{"code":"ORP_LIMIT"}}
```

---

## Recommandations pour l'application écran LVGL

1. Au démarrage : envoyer `get_status` + `get_config` pour initialiser l'affichage
2. Polling capteurs : `get_status` toutes les 5 secondes
3. Écouter en permanence les lignes entrantes (événements push)
4. Implémenter un timeout de réponse (~5s) pour détecter la perte de communication
5. Le protocole est **stateless** : pas de session, pas d'authentification UART
6. En cas de redémarrage du contrôleur, relancer `get_status` + `get_config`
7. Privilégier `run_action` pour les actions opérationnelles, `set_config` pour la configuration persistée
