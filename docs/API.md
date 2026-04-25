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

### GET /auth/ap-password — CRITICAL

Retourne le mot de passe WiFi du point d'accès `PoolControllerAP`. Utile si l'étiquette collée sur le boîtier est illisible ou perdue.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/auth/ap-password
```

```json
{
  "ap_ssid": "PoolControllerAP",
  "ap_password": "XXXXXXXX"
}
```

> Le mot de passe AP est généré aléatoirement au premier boot et ne change pas lors des mises à jour OTA ni des factory resets (bouton physique). Pour en générer un nouveau, utiliser `./deploy.sh factory`.

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

Paramètre `range` : `24h` (défaut), `3d`, `7d`, `30d`, `all`.

Paramètre optionnel `?since=TIMESTAMP` pour récupération incrémentale.

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

Retourne les 200 derniers logs système. Paramètre optionnel `?since=TIMESTAMP` pour récupération incrémentale (ne retourne que les entrées postérieures à ce timestamp).

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/get-logs
curl -u admin:monmotdepasse "http://poolcontroller.local/get-logs?since=14400000"
```

```json
{
  "logs": [
    { "timestamp": 14400000, "level": "INFO", "message": "Capteurs initialisés" },
    { "timestamp": 14405000, "level": "WARNING", "message": "pH hors limites: 7.8" }
  ]
}
```

> `timestamp` est exprimé en millisecondes depuis le démarrage de l'ESP32 (`millis()`).

---

### GET /download-logs — WRITE

Télécharge les logs système sous forme de fichier texte (`pool_logs.txt`).

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/download-logs -o pool_logs.txt
```

```
# Pool Controller — Journal système
# Exporté le boot+3600s | Entrées: 42
# Format: [+Xs] NIVEAU : message

[+01:00:00] INFO : Démarrage dosage pH: pH=7.85 cible=7.20 erreur=+0.65
[+01:00:45] INFO : Arrêt dosage pH: durée=45s vol≈3.5mL total jour=3.5/300mL — pause 30min
[+01:31:00] INFO : Démarrage dosage pH: pH=7.78 cible=7.20 erreur=+0.58
```

> Les logs enrichis incluent les événements de dosage (démarrage, arrêt, paramètres) ainsi que les alertes de limites horaires/journalières.

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
  "regulation_speed": "normal",
  "stabilization_delay_min": 5,
  "ph_correction_type": "ph_minus",
  "ph_target": 7.2,
  "orp_target": 700,
  "ph_regulation_enabled": true,
  "ph_pump": 1,
  "pump1_max_duty_pct": 50,
  "orp_regulation_enabled": true,
  "orp_pump": 2,
  "pump2_max_duty_pct": 50,
  "pump_max_flow_ml_per_min": 90,
  "ph_limit_minutes": 5,
  "orp_limit_minutes": 10,
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

Champs notables de la réponse :

| Champ | Type | Description |
|-------|------|-------------|
| `regulation_mode` | string | `"pilote"` (suit la filtration) ou `"continu"` |
| `regulation_speed` | string | `"slow"` / `"normal"` / `"fast"` — préréglages PID |
| `stabilization_delay_min` | integer | Délai de stabilisation capteurs après démarrage filtration (0–60 min) |
| `ph_limit_minutes` | integer | Durée max d'injection pH par fenêtre glissante d'1 h (minutes, 1–60) |
| `orp_limit_minutes` | integer | Durée max d'injection ORP par fenêtre glissante d'1 h (minutes, 1–60) |
| `pump1_max_duty_pct` | integer | Puissance maximale pompe 1 en régulation (0–100 %) |
| `pump2_max_duty_pct` | integer | Puissance maximale pompe 2 en régulation (0–100 %) |
| `pump_max_flow_ml_per_min` | float | Débit nominal des pompes à 100 % de puissance (mL/min) |
| `ph_regulation_mode` | string | Mode de régulation pH : `"automatic"` (PID vers ph_target), `"scheduled"` (volume quotidien réparti sur 24 h), `"manual"` (aucune régulation automatique) |
| `ph_daily_target_ml` | integer | Volume quotidien cible en mL pour le mode Programmée pH (0 = désactivé, borné par `max_ph_ml_per_day`) |
| `ph_enabled` | boolean | Miroir dérivé de `ph_regulation_mode` : `true` si mode ≠ `"manual"`. Maintenu pour compatibilité MQTT / HA. |
| `orp_regulation_mode` | string | Mode de régulation ORP : `"automatic"` (PID vers orp_target), `"scheduled"` (volume quotidien de chlore, aveugle au capteur), `"manual"` (aucune régulation automatique) |
| `orp_daily_target_ml` | integer | Volume quotidien cible en mL pour le mode Programmée ORP (0 = désactivé, borné par `max_orp_ml_per_day`) |
| `orp_enabled` | boolean | Miroir dérivé de `orp_regulation_mode` : `true` si mode ≠ `"manual"`. Maintenu pour compatibilité MQTT / HA. |
| `max_orp_ml_per_day` | float | Limite journalière ORP configurée (mL) — alias de `max_chlorine_ml_per_day` |
| `orp_cal_valid` | boolean | `true` si une calibration ORP a déjà été enregistrée (date non vide) |

**`POST /save-config` — champs pH spécifiques :**

| Champ | Type | Validation |
|-------|------|------------|
| `ph_regulation_mode` | string | Valeurs acceptées : `"automatic"`, `"scheduled"`, `"manual"`. Toute autre valeur est ignorée. Met à jour `ph_enabled` automatiquement. |
| `ph_daily_target_ml` | integer | Doit être ≥ 0 et ≤ `max_ph_ml_per_day` (si configuré). Retourne HTTP 400 si la limite est dépassée. |

**`POST /save-config` — champs ORP spécifiques :**

| Champ | Type | Validation |
|-------|------|------------|
| `orp_regulation_mode` | string | Valeurs acceptées : `"automatic"`, `"scheduled"`, `"manual"`. Toute autre valeur est ignorée. Met à jour `orp_enabled` automatiquement. |
| `orp_daily_target_ml` | integer | Doit être ≥ 0 et ≤ `max_orp_ml_per_day` (si configuré). Retourne HTTP 400 si la limite est dépassée. |

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

## WebSocket temps réel

### WS /ws — WRITE

Flux temps réel basse latence. L'ESP32 pousse un message JSON à chaque changement d'état significatif (mesures capteurs, dosage, configuration).

**Connexion**

```js
const ws = new WebSocket('ws://poolcontroller.local/ws');
```

**Message d'état courant** (poussé à la connexion puis à chaque changement)

```json
{
  "ph": 7.31,
  "orp": 718,
  "temperature": 24.5,
  "filtration_running": true,
  "ph_dosing": false,
  "orp_dosing": false,
  "ph_daily_ml": 45.2,
  "orp_daily_ml": 120.5,
  "ph_regulation_mode": "automatic",
  "ph_daily_target_ml": 0,
  "orp_regulation_mode": "automatic",
  "orp_daily_target_ml": 0,
  "max_orp_ml_per_day": 1000,
  "orp_cal_valid": true,
  "reset_reason": "POWER_ON"
}
```

Champs notables liés à la régulation pH :

| Champ | Type | Description |
|-------|------|-------------|
| `ph_regulation_mode` | string | Mode actif : `"automatic"`, `"scheduled"` ou `"manual"` |
| `ph_daily_target_ml` | integer | Volume quotidien programmé (mL) — pertinent uniquement en mode `"scheduled"` |

Champ système :

| Champ | Type | Description |
|-------|------|-------------|
| `reset_reason` | string | Raison du dernier reboot ESP32. Constant pendant tout le runtime. Valeurs possibles : `"POWER_ON"` (mise sous tension), `"SW_RESET"` (reboot logiciel, inclut les OTA), `"WATCHDOG"` (timeout watchdog matériel ou tâche), `"BROWNOUT"` (sous-tension), `"PANIC"` (exception / crash firmware), `"DEEP_SLEEP"` (réveil depuis veille profonde), `"EXTERNAL"` (signal RESET externe), `"UNKNOWN"` (autre). |

> Le champ `reset_reason` est absent des versions antérieures à la v2.5.x. L'UI se comporte gracieusement si le champ est absent (aucun toast affiché).

Champs notables liés à la régulation ORP :

| Champ | Type | Description |
|-------|------|-------------|
| `orp_regulation_mode` | string | Mode actif : `"automatic"`, `"scheduled"` ou `"manual"` |
| `orp_daily_target_ml` | integer | Volume quotidien programmé de chlore (mL) — pertinent uniquement en mode `"scheduled"` |
| `max_orp_ml_per_day` | float | Limite journalière ORP (mL) — utilisée pour borner la saisie côté UI |
| `orp_cal_valid` | boolean | `true` si une calibration ORP a été enregistrée |

> Le WebSocket pousse la configuration complète à la connexion initiale ; les mises à jour suivantes sont différentielles (seuls les champs modifiés sont inclus).

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

### POST /ph/inject/start — WRITE

Démarre une injection manuelle pH à la puissance configurée (`pump1_max_duty_pct` ou `pump2_max_duty_pct` selon `ph_pump`). La durée est calculée à partir du volume demandé et du débit nominal de la pompe.

> ⚠️ **L'injection manuelle NE VÉRIFIE PAS les gardes de régulation** : elle ignore la limite horaire (`ph_limit_minutes`), la limite journalière (`max_ph_ml_per_day`), le délai de stabilisation, l'état de la filtration et le mode de régulation. Le volume injecté **est compté** dans le cumul journalier (`ph_daily_ml`) et peut donc le dépasser. Responsabilité opérateur. Voir [docs/subsystems/pump-controller.md](subsystems/pump-controller.md) et [docs/features/page-ph.md](features/page-ph.md).

Paramètres (querystring, exclusifs) :

| Paramètre | Type | Validation |
|-----------|------|------------|
| `volume` | float (mL) | 1 – 2000 (mode préféré) |
| `duration` | integer (secondes) | fallback legacy, borné 1 – 3600 |

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/ph/inject/start?volume=15"
```

Réponse : `200 OK` / `400` si le paramètre est manquant ou hors plage.

---

### POST /ph/inject/stop — WRITE

Arrête immédiatement l'injection manuelle pH en cours.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/ph/inject/stop
```

---

### POST /orp/inject/start — WRITE

Identique à `/ph/inject/start` pour la pompe ORP (`orp_pump`). Mêmes paramètres et contraintes.

> ⚠️ **Même avertissement que `/ph/inject/start`** : les limites horaire / journalière / stabilisation / filtration ne sont PAS vérifiées. Le volume est compté dans `orp_daily_ml` et peut dépasser `max_chlorine_ml_per_day`. Voir [docs/subsystems/pump-controller.md](subsystems/pump-controller.md) et [docs/features/page-orp.md](features/page-orp.md).

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/orp/inject/start?volume=50"
```

---

### POST /orp/inject/stop — WRITE

Arrête immédiatement l'injection manuelle ORP en cours.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/orp/inject/stop
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

> Efface : mot de passe admin, token API, WiFi, MQTT, calibrations. Préserve : historique, fichiers LittleFS, **mot de passe AP WiFi** (l'étiquette sur le boîtier reste valide).
