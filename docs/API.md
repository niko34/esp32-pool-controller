# API Documentation - ESP32 Pool Controller

## Authentification

Tous les endpoints nécessitent une authentification sauf ceux marqués **public**.

### Méthodes d'Authentification

#### HTTP Basic Auth

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/get-config
```

#### Token API (header)

```bash
curl -H "X-Auth-Token: abc123def456..." http://poolcontroller.local/get-config
```

Le token API est généré automatiquement au premier démarrage. Il est visible dans **Paramètres → Système → Token API** et peut être régénéré via `/auth/regenerate-token`.

### Niveaux de protection

| Niveau | Description |
|--------|-------------|
| Public | Accessible sans authentification |
| WRITE  | Authentification requise |
| CRITICAL | Authentification requise — opérations sensibles |

### Recommandations de sécurité

- Utiliser un réseau WiFi sécurisé
- Ne pas exposer l'ESP32 directement sur Internet
- Changer le mot de passe admin par défaut
- Utiliser le token API plutôt que le mot de passe dans les scripts automatisés

---

## Codes d'erreur

| Code | Description |
|------|-------------|
| 200  | Succès |
| 400  | Requête invalide (paramètre manquant ou JSON invalide) |
| 401  | Authentification requise ou mot de passe incorrect |
| 403  | Accès refusé (ex: wizard déjà complété) |
| 404  | Endpoint introuvable |
| 429  | Trop de requêtes (rate limiting) |
| 500  | Erreur serveur |

---

## Authentification et Wizard

### GET /auth/status — public

Retourne l'état d'authentification du système.

```bash
curl http://poolcontroller.local/auth/status
```

```json
{
  "firstBoot": false,
  "authEnabled": true,
  "forceWifiConfig": false
}
```

---

### POST /auth/login — public

Authentifie l'utilisateur et retourne le token API.

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"monmotdepasse"}' \
  http://poolcontroller.local/auth/login
```

```json
{
  "success": true,
  "token": "abc123def456...",
  "username": "admin"
}
```

> ⚠️ Bloqué si `firstBoot=true` — le mot de passe doit d'abord être changé via `/auth/change-password`.

---

### POST /auth/change-password — public (premier démarrage) / WRITE (ensuite)

Change le mot de passe admin. Accessible sans authentification lors du premier démarrage.

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"currentPassword":"admin","newPassword":"nouveaumotdepasse"}' \
  http://poolcontroller.local/auth/change-password
```

```json
{
  "success": true,
  "token": "abc123def456...",
  "message": "Password changed successfully"
}
```

Règles : minimum 8 caractères, différent du mot de passe actuel.

---

### POST /auth/complete-wizard — public (premier démarrage uniquement)

Marque le wizard de configuration initiale comme complété.

```bash
curl -X POST http://poolcontroller.local/auth/complete-wizard
```

---

### GET /auth/token — CRITICAL

Retourne le token API courant.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/auth/token
```

```json
{ "token": "abc123def456..." }
```

---

### POST /auth/regenerate-token — CRITICAL

Régénère un nouveau token API (invalide l'ancien).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/auth/regenerate-token
```

```json
{
  "success": true,
  "token": "nouveautoken...",
  "message": "API token regenerated"
}
```

---

## Données et Historique

### GET /data — WRITE

Retourne les valeurs actuelles des capteurs.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/data
```

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
  "orp_limit_reached": false,
  "ph_tracking_enabled": true,
  "ph_remaining_ml": 1500,
  "ph_alert_threshold_ml": 500,
  "orp_tracking_enabled": true,
  "orp_remaining_ml": 3200,
  "orp_alert_threshold_ml": 1000
}
```

---

### GET /get-history — WRITE

Retourne l'historique des mesures.

```bash
curl -u admin:monmotdepasse "http://poolcontroller.local/get-history?range=24h"
```

Paramètre `range` : `24h` (défaut), `7d`, `30d`, `all`.

---

### POST /history/clear — CRITICAL

Efface tout l'historique des mesures.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/history/clear
```

---

### POST /history/import — WRITE

Importe un lot de points d'historique.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '[{"timestamp":1700000000,"ph":7.2,"orp":720,"temperature":24.5}]' \
  http://poolcontroller.local/history/import
```

---

### GET /get-logs — WRITE

Retourne les 50 derniers logs système. Paramètre optionnel `?since=TIMESTAMP`.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/get-logs
```

```json
{
  "logs": [
    { "timestamp": "14:30:45", "level": "INFO", "message": "Capteurs initialisés" },
    { "timestamp": "14:30:50", "level": "WARNING", "message": "pH hors limites: 7.8" }
  ]
}
```

---

## Configuration

### GET /get-config — WRITE

Retourne la configuration complète du système.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/get-config
```

```json
{
  "server": "mqtt.example.com",
  "port": 1883,
  "topic": "pool/controller",
  "enabled": true,
  "regulation_mode": "pilote",
  "ph_correction_type": "ph_minus",
  "ph_target": 7.2,
  "orp_target": 700,
  "ph_regulation_enabled": true,
  "ph_pump": 1,
  "orp_regulation_enabled": true,
  "orp_pump": 2,
  "ph_limit_seconds": 300,
  "orp_limit_seconds": 300,
  "time_use_ntp": true,
  "ntp_server": "pool.ntp.org",
  "timezone_id": "europe_paris",
  "filtration_enabled": true,
  "filtration_mode": "auto",
  "filtration_start": "08:00",
  "filtration_end": "20:00",
  "lighting_feature_enabled": true,
  "lighting_enabled": false,
  "lighting_schedule_enabled": true,
  "lighting_start": "20:00",
  "lighting_end": "23:00",
  "temperature_enabled": true,
  "wifi_ssid": "MonWiFi",
  "wifi_ip": "192.168.1.100",
  "max_ph_ml_per_day": 1000,
  "max_chlorine_ml_per_day": 1000,
  "time_current": "2026-03-28T14:30:45"
}
```

---

### POST /save-config — CRITICAL

Sauvegarde la configuration complète. Corps JSON identique à la réponse de `/get-config`.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"ph_target":7.2,"orp_target":700}' \
  http://poolcontroller.local/save-config
```

---

### GET /get-system-info — WRITE

Retourne les informations système de l'ESP32.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/get-system-info
```

```json
{
  "firmware_version": "1.0.3",
  "build_date": "Mar 28 2026",
  "chip_model": "ESP32-D0WDQ6",
  "cpu_freq_mhz": 240,
  "free_heap": 123456,
  "heap_size": 327680,
  "flash_size": 4194304,
  "ota_partition": "app0",
  "fs_total_bytes": 1507328,
  "fs_used_bytes": 245760,
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

---

### GET /time-now — WRITE

Retourne l'heure système courante.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/time-now
```

```json
{
  "time": "2026-03-28T14:30:45",
  "time_use_ntp": true,
  "timezone_id": "europe_paris"
}
```

---

## WiFi

### GET /wifi/status — public

Retourne l'état de la connexion WiFi.

```bash
curl http://poolcontroller.local/wifi/status
```

```json
{
  "mode": "STA",
  "connected": true,
  "ssid": "MonWiFi",
  "ap_ssid": "PoolControllerAP",
  "ap_ip": "192.168.4.1"
}
```

---

### GET /wifi/scan — public (mode AP) / WRITE (mode STA)

Scanne les réseaux WiFi disponibles.

```bash
curl http://poolcontroller.local/wifi/scan
```

```json
{
  "networks": [
    { "ssid": "MonWiFi", "rssi": -45, "channel": 6, "secure": true }
  ]
}
```

---

### POST /wifi/connect — public (mode AP) / WRITE (mode STA)

Se connecte à un réseau WiFi.

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"ssid":"MonWiFi","password":"wifipassword"}' \
  http://poolcontroller.local/wifi/connect
```

```json
{
  "accepted": true,
  "message": "WiFi connection request accepted, connecting asynchronously"
}
```

---

### POST /wifi/disconnect — WRITE

Déconnecte du WiFi et efface les credentials.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/wifi/disconnect
```

---

### POST /wifi/ap/disable — CRITICAL

Désactive le mode point d'accès.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/wifi/ap/disable
```

---

## Contrôle

### POST /pump1/on, /pump1/off — WRITE

Démarre ou arrête la pompe doseuse 1 (pH) manuellement.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/off
```

---

### POST /pump2/on, /pump2/off — WRITE

Démarre ou arrête la pompe doseuse 2 (ORP/chlore) manuellement.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump2/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump2/off
```

---

### POST /pump1/duty/:duty, /pump2/duty/:duty — WRITE

Règle la puissance PWM d'une pompe (0–255).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/duty/128
```

---

### POST /lighting/on, /lighting/off — WRITE

Allume ou éteint l'éclairage (relais).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/lighting/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/lighting/off
```

---

## Calibration

### POST /calibrate_ph_neutral — CRITICAL

Calibration pH point neutre (sonde dans solution pH 7.0).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/calibrate_ph_neutral
```

---

### POST /calibrate_ph_acid — CRITICAL

Calibration pH point acide (sonde dans solution pH 4.0).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/calibrate_ph_acid
```

---

### POST /clear_ph_calibration — CRITICAL

Efface la calibration pH.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/clear_ph_calibration
```

---

## Mises à jour OTA

### POST /update — CRITICAL

Met à jour le firmware ou le filesystem via multipart/form-data.

```bash
# Firmware
curl -u admin:monmotdepasse -X POST \
  -F "update_type=firmware" -F "update=@.pio/build/esp32dev/firmware.bin" \
  http://poolcontroller.local/update

# Filesystem
curl -u admin:monmotdepasse -X POST \
  -F "update_type=filesystem" -F "update=@.pio/build/esp32dev/littlefs.bin" \
  http://poolcontroller.local/update
```

> ⚠️ `update_type` doit être envoyé **avant** le fichier dans le multipart.

Réponse : `200 OK` (corps `OK`) ou `200 OK` (corps `FAIL`).

L'ESP32 redémarre automatiquement après une mise à jour réussie.

---

### GET /check-update — WRITE

Vérifie la dernière release disponible sur GitHub.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/check-update
```

```json
{
  "current_version": "1.0.3",
  "latest_version": "1.0.4",
  "update_available": true,
  "firmware_url": "https://github.com/.../firmware.bin",
  "filesystem_url": "https://github.com/.../littlefs.bin"
}
```

---

### POST /download-update — CRITICAL

Télécharge et installe une mise à jour depuis une URL GitHub.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"url":"https://github.com/.../firmware.bin"}' \
  http://poolcontroller.local/download-update
```

> Seuls les hôtes `github.com`, `api.github.com` et `objects.githubusercontent.com` sont autorisés.

---

## Système

### POST /reboot — CRITICAL

Redémarre l'ESP32.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/reboot
```

---

### POST /reboot-ap — CRITICAL

Efface les credentials WiFi et redémarre en mode point d'accès.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/reboot-ap
```

---

### POST /factory-reset — CRITICAL

Réinitialisation d'usine complète (efface la partition NVS).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/factory-reset
```

> Efface : mot de passe, token API, WiFi, MQTT, calibrations. Préserve l'historique et les fichiers LittleFS.
