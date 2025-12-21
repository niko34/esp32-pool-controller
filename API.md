# API Documentation - ESP32 Pool Controller

## Endpoints Disponibles

### 1. Informations Système

**GET** `/get-system-info`

Retourne les informations complètes sur le système ESP32.

**Exemple de requête :**
```bash
curl http://poolcontroller.local/get-system-info
```

**Réponse (JSON) :**
```json
{
  "firmware_version": "2025.1.1",
  "build_date": "Dec 21 2025",
  "build_time": "14:30:45",
  "chip_model": "ESP32-D0WDQ6",
  "chip_revision": 1,
  "chip_cores": 2,
  "cpu_freq_mhz": 240,
  "free_heap": 123456,
  "heap_size": 327680,
  "free_psram": 0,
  "psram_size": 0,
  "flash_size": 4194304,
  "flash_speed": 40000000,
  "ota_partition": "app0",
  "ota_partition_size": 1310720,
  "fs_total_bytes": 1507328,
  "fs_used_bytes": 245760,
  "fs_free_bytes": 1261568,
  "wifi_ssid": "MonWiFi",
  "wifi_rssi": -45,
  "wifi_ip": "192.168.1.100",
  "wifi_mac": "AA:BB:CC:DD:EE:FF",
  "uptime_seconds": 86400,
  "uptime_days": 1,
  "uptime_hours": 0,
  "uptime_minutes": 0
}
```

**Utilisation avec jq :**
```bash
# Version du firmware
curl -s http://poolcontroller.local/get-system-info | jq -r '.firmware_version'

# Mémoire libre en KB
curl -s http://poolcontroller.local/get-system-info | jq '.free_heap / 1024'

# Adresse IP
curl -s http://poolcontroller.local/get-system-info | jq -r '.wifi_ip'

# Uptime formaté
curl -s http://poolcontroller.local/get-system-info | jq -r '"\(.uptime_days)j \(.uptime_hours)h \(.uptime_minutes)min"'
```

---

### 2. Mise à Jour OTA

**POST** `/update`

Permet de mettre à jour le firmware ou le système de fichiers via HTTP multipart/form-data.

**Détection automatique du type :**
- Fichier `.bin` → Firmware
- Fichier `.littlefs.bin` ou `.fs.bin` → Filesystem

**Exemple de requête :**
```bash
# Mise à jour du firmware
curl -X POST -F "update=@firmware.bin" http://poolcontroller.local/update

# Mise à jour du filesystem
curl -X POST -F "update=@littlefs.bin" http://poolcontroller.local/update
```

**Réponse :**
- `200 OK` : Mise à jour réussie, l'ESP32 va redémarrer
- `500 Internal Server Error` : Erreur lors de la mise à jour

**Notes importantes :**
- L'ESP32 redémarre automatiquement après la mise à jour
- Attendre environ 30 secondes avant d'envoyer une nouvelle requête
- Ne PAS couper l'alimentation pendant la mise à jour

---

### 3. Configuration

**GET** `/get-config`

Retourne la configuration actuelle du système.

**Exemple de requête :**
```bash
curl http://poolcontroller.local/get-config
```

**Réponse (JSON) :**
```json
{
  "server": "mqtt.example.com",
  "port": 1883,
  "topic": "pool/controller",
  "username": "user",
  "password": "******",
  "enabled": true,
  "ph_target": 7.2,
  "orp_target": 700,
  "ph_enabled": true,
  "ph_pump": 1,
  "orp_enabled": true,
  "orp_pump": 2,
  "ph_limit_seconds": 300,
  "orp_limit_seconds": 300,
  "time_use_ntp": true,
  "ntp_server": "pool.ntp.org",
  "timezone_id": "europe_paris",
  "filtration_mode": "auto",
  "filtration_start": "08:00",
  "filtration_end": "20:00",
  "wifi_ssid": "MonWiFi",
  "wifi_ip": "192.168.1.100",
  "max_ph_ml_per_day": 1000,
  "max_chlorine_ml_per_day": 1000,
  "time_current": "2025-12-21T14:30:45"
}
```

---

### 4. Données Capteurs

**GET** `/data`

Retourne les valeurs actuelles des capteurs.

**Exemple de requête :**
```bash
curl http://poolcontroller.local/data
```

**Réponse (JSON) :**
```json
{
  "orp": 720,
  "ph": 7.3,
  "orp_raw": 715,
  "ph_raw": 7.28,
  "temperature": 24.5,
  "filtration_running": true,
  "ph_dosing": false,
  "orp_dosing": false,
  "ph_daily_ml": 45.2,
  "orp_daily_ml": 120.5,
  "ph_limit_reached": false,
  "orp_limit_reached": false
}
```

**Monitoring avec script :**
```bash
#!/bin/bash
# monitor.sh - Affiche les valeurs en temps réel

while true; do
    DATA=$(curl -s http://poolcontroller.local/data)
    PH=$(echo $DATA | jq -r '.ph')
    ORP=$(echo $DATA | jq -r '.orp')
    TEMP=$(echo $DATA | jq -r '.temperature')

    clear
    echo "=== Pool Monitor ==="
    echo "pH:          $PH"
    echo "ORP:         $ORP mV"
    echo "Température: $TEMP °C"
    echo ""
    echo "$(date)"

    sleep 5
done
```

---

### 5. Logs Système

**GET** `/get-logs`

Retourne les 50 derniers logs du système.

**Exemple de requête :**
```bash
curl http://poolcontroller.local/get-logs
```

**Réponse (JSON) :**
```json
{
  "logs": [
    {
      "timestamp": "14:30:45",
      "level": "INFO",
      "message": "Capteurs initialisés"
    },
    {
      "timestamp": "14:30:50",
      "level": "WARNING",
      "message": "pH hors limites: 7.8"
    }
  ]
}
```

---

### 6. Export CSV

**GET** `/export-csv`

Exporte l'historique des mesures au format CSV.

**Exemple de requête :**
```bash
curl -o pool_history.csv http://poolcontroller.local/export-csv
```

**Format CSV :**
```csv
timestamp,ph,orp,temperature
2025-12-21T14:30:00,7.2,720,24.5
2025-12-21T14:40:00,7.3,715,24.6
```

---

### 7. Heure Actuelle

**GET** `/time-now`

Retourne l'heure actuelle du système.

**Exemple de requête :**
```bash
curl http://poolcontroller.local/time-now
```

**Réponse (JSON) :**
```json
{
  "time": "2025-12-21T14:30:45",
  "time_use_ntp": true,
  "timezone_id": "europe_paris"
}
```

---

## Exemples d'Utilisation Avancés

### Monitoring Prometheus

Créer un exporter pour Prometheus :

```python
#!/usr/bin/env python3
# prometheus_exporter.py

import requests
import time
from prometheus_client import start_http_server, Gauge

# Métriques
ph_gauge = Gauge('pool_ph', 'pH de la piscine')
orp_gauge = Gauge('pool_orp_mv', 'ORP en mV')
temp_gauge = Gauge('pool_temperature_celsius', 'Température en °C')

def collect_metrics():
    try:
        r = requests.get('http://poolcontroller.local/data')
        data = r.json()

        if data['ph'] is not None:
            ph_gauge.set(data['ph'])
        if data['orp'] is not None:
            orp_gauge.set(data['orp'])
        if data['temperature'] is not None:
            temp_gauge.set(data['temperature'])

    except Exception as e:
        print(f"Erreur: {e}")

if __name__ == '__main__':
    start_http_server(8000)
    while True:
        collect_metrics()
        time.sleep(10)
```

### Alertes Slack

```bash
#!/bin/bash
# check_and_alert.sh

SLACK_WEBHOOK="https://hooks.slack.com/services/YOUR/WEBHOOK/URL"

DATA=$(curl -s http://poolcontroller.local/data)
PH=$(echo $DATA | jq -r '.ph')
ORP=$(echo $DATA | jq -r '.orp')

# Vérifier le pH
if (( $(echo "$PH < 7.0 || $PH > 7.6" | bc -l) )); then
    curl -X POST -H 'Content-type: application/json' \
        --data "{\"text\":\"⚠️ pH anormal: $PH\"}" \
        $SLACK_WEBHOOK
fi

# Vérifier l'ORP
if (( $(echo "$ORP < 650 || $ORP > 800" | bc -l) )); then
    curl -X POST -H 'Content-type: application/json' \
        --data "{\"text\":\"⚠️ ORP anormal: $ORP mV\"}" \
        $SLACK_WEBHOOK
fi
```

### Dashboard Grafana

Configuration de datasource dans Grafana avec SimpleJSON :

```bash
# Installer le plugin SimpleJSON
grafana-cli plugins install grafana-simple-json-datasource

# Créer un proxy vers l'ESP32
# server.js (Node.js)
const express = require('express');
const axios = require('axios');
const app = express();

app.use(express.json());

app.all('/api/*', async (req, res) => {
    const espUrl = 'http://poolcontroller.local' + req.path.replace('/api', '');
    const response = await axios.get(espUrl);
    res.json(response.data);
});

app.listen(3000);
```

---

## Authentification

Actuellement, l'API ne nécessite **pas d'authentification**. Il est recommandé de :

1. Utiliser un réseau WiFi sécurisé
2. Ne pas exposer l'ESP32 sur Internet directement
3. Utiliser un VPN pour l'accès distant
4. Configurer un firewall sur votre routeur

---

## Limitations

- **Rate Limiting** : Pas de limitation, mais éviter les requêtes trop fréquentes
- **Timeout** : 30 secondes pour les mises à jour OTA
- **Taille maximale** : ~1.5 MB pour le firmware, ~1.5 MB pour le filesystem
- **Concurrent Updates** : Une seule mise à jour à la fois

---

## Code d'Erreur

| Code | Description |
|------|-------------|
| 200  | Succès |
| 400  | Requête invalide |
| 404  | Endpoint introuvable |
| 500  | Erreur serveur |
