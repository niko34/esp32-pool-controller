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
| 409  | Conflit d'état — typiquement injection manuelle refusée car filtration arrêtée (sécurité chimique). Voir [`POST /ph/inject/start`](#post-phinjectstart--write). |
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
  "ph": 7.234,
  "temperature": 24.5,
  "temperature_circuit": 28.1,
  "phCalPoints": 2,
  "orpCalPoints": 1,
  "phRaw": 7.241,
  "phMedian": 7.236,
  "phFiltered": 7.234,
  "phFilterReady": true,
  "phFilterUnstable": false,
  "phRejectedCount": 0,
  "orpRaw": 722,
  "orpMedian": 720,
  "orpFiltered": 720,
  "orpFilterReady": true,
  "orpFilterUnstable": false,
  "orpRejectedCount": 0,
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

> **feature-021** : `ph` est désormais publié avec **3 décimales** (vs 1 décimale en v1.x). Champs `phCalPoints` (`-1..3`) et `orpCalPoints` (`-1..1`) ajoutés. Champs **supprimés** de la réponse `/data` : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw` (la notion de « valeur brute » n'a pas de sens côté Atlas EZO — la valeur est déjà calibrée par le module). Voir [ADR-0014](adr/0014-migration-atlas-ezo.md).
>
> **feature-025** : `ph` / `orp` correspondent désormais à la valeur **filtrée** (médiane + EMA), avec fallback sur le brut tant que le filtre n'est pas amorcé. Champs `phRaw/phMedian/phFiltered/phFilterReady/phFilterUnstable/phRejectedCount` (+ équivalents `orp*`) ajoutés. Les champs flottants valent `null` si la mesure est indisponible (stale / non amorcé). Mêmes champs côté WS. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#filtrage-des-mesures-phorp--feature-025).

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

### DELETE /logs — WRITE

Efface intégralement les logs côté ESP32 : buffer RAM circulaire, tampon de flush en attente, fichier persistant `/system.log` et son éventuel `.tmp` de rotation.

```bash
curl -u admin:monmotdepasse -X DELETE http://poolcontroller.local/logs
```

```json
{ "success": true }
```

Une entrée `INFO : Logs effacés (RAM + fichier persistant)` est écrite immédiatement après l'effacement pour tracer l'action.

> ⚠️ Action irréversible. Pour ne vider que la vue navigateur sans toucher au firmware, utiliser le bouton « Effacer (écran) » côté UI.

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
  "time_current": "2026-03-28T14:30:45",
  "sensor_logs_enabled": false,
  "debug_logs_enabled": false
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
| `sensor_logs_enabled` | boolean | Verbosité des logs capteurs (default `false`). Modifiable via Paramètres → Avancé → « Log des sondes ». |
| `debug_logs_enabled` | boolean | Active la production des logs de niveau `DEBUG` côté firmware (default `false`). Quand `false`, `Logger::debug()` court-circuite immédiatement (early return). Les niveaux `INFO`/`WARN`/`ERROR`/`CRITICAL` ne sont pas affectés. Modifiable via Paramètres → Avancé → « Logs DEBUG activés ». Persisté en NVS sous la clé `debug_logs`. Effet immédiat, pas de redémarrage requis. |

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
  "ph": 7.234,
  "orp": 718,
  "temperature": 24.5,
  "temperature_circuit": 28.1,
  "phCalPoints": 2,
  "orpCalPoints": 1,
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

> **feature-021** : `ph` 3 décimales, ajout de `phCalPoints` (`-1..3`) et `orpCalPoints` (`-1..1`). Champs **supprimés** de la payload WS : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw`. Voir [ADR-0014](adr/0014-migration-atlas-ezo.md).
>
> **feature-025** : `ph` / `orp` = valeur **filtrée** (fallback brut si non amorcé). Champs de filtrage ajoutés (voir tableau dédié ci-dessous).

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
| `orp_cal_valid` | boolean | `true` si une calibration ORP a été enregistrée (depuis 2.0.0 : miroir de `orpCalPoints >= 1`) |

Champs ajoutés en feature-021 (calibration EZO) :

| Champ | Type | Description |
|-------|------|-------------|
| `phCalPoints` | integer | Points de calibration EZO pH. `-1` = module injoignable / bus dégradé, `0` = non calibré, `1` = mid seul, `2` = mid + low (calibration nominale), `3` = mid + low + high. La régulation pH automatique est inhibée tant que la valeur est `< 2`. |
| `orpCalPoints` | integer | Points de calibration EZO ORP. `-1` = module injoignable, `0` = non calibré, `1` = calibré (Atlas ORP n'a qu'un seul point de calibration). La régulation ORP automatique est inhibée tant que la valeur est `< 1`. |

Champs ajoutés en feature-024 (pente sonde pH) :

| Champ | Type | Description |
|-------|------|-------------|
| `phSlopeAcid` | float \| null | Pente acide EZO en % (1 décimale, idéal 100). `null` si jamais lu, bus dégradé ou EZO injoignable. |
| `phSlopeBase` | float \| null | Pente base EZO en % (1 décimale, idéal 100). `null` mêmes conditions. |
| `phSlopeZero` | float \| null | Décalage zéro EZO en mV (2 décimales, idéal 0). `null` si firmware EZO ancien ne le rapporte pas. |
| `phSlopeAgeMs` | integer \| null | Millisecondes depuis la dernière query `Slope,?` réussie. `null` si jamais lue depuis le boot (cohérent avec `phSlope*` nullables). L'UI considère l'état stale au-delà de 36 h. |

Champs ajoutés en feature-025 (lissage mesures + régulation P temporisée) :

| Champ | Type | Description |
|-------|------|-------------|
| `phRaw` | float \| null | Dernière mesure pH **brute** Atlas (3 décimales). `null` si stale / jamais lue. |
| `phMedian` | float \| null | Médiane glissante pH (fenêtre 7). `null` si pas de donnée. |
| `phFiltered` | float \| null | pH **filtré** (médiane + EMA) — valeur utilisée par le PID et affichée en principal. `null` si non amorcé. |
| `phFilterReady` | boolean | `true` après warmup (≥ 5 mesures valides) **et** dernière mesure récente. Le dosage pH est bloqué tant que `false`. |
| `phFilterUnstable` | boolean | `true` si trop de rejets consécutifs (≥ 10) → capteur déclaré instable, dosage bloqué. |
| `phRejectedCount` | integer | Nombre de mesures pH rejetées (compteur glissant 0..255). |
| `orpRaw` / `orpMedian` / `orpFiltered` | float \| null | Équivalents ORP (mV, sans décimale). |
| `orpFilterReady` / `orpFilterUnstable` | boolean | Équivalents ORP. |
| `orpRejectedCount` | integer | Équivalent ORP. |
| `phMixingDelayActive` | boolean | `true` pendant la pause mélange hydraulique pH (15 min après injection). |
| `orpMixingDelayActive` | boolean | `true` pendant la pause mélange hydraulique ORP (20 min après injection). |
| `phDoseBlockedReason` | string \| null | Dernière cause de refus de `canDose(0)` (ex `"filtre capteur non prêt (warmup / EZO injoignable)"`, `"pause mélange en cours"`). `null` si dosage autorisé. |
| `orpDoseBlockedReason` | string \| null | Équivalent ORP. |

> Le WebSocket pousse la configuration complète à la connexion initiale ; les mises à jour suivantes sont différentielles (seuls les champs modifiés sont inclus).

---

## Contrôle

### POST /pump1/on, /pump1/off — WRITE

Démarre ou arrête la pompe doseuse 1 (pH) manuellement (test bench).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/off
```

> ⚠️ **Garde filtration (v2.1.2)** : `/pump1/on` retourne **`409 Conflict`** si la filtration est arrêtée et que `regulationMode != "continu"`. `/pump1/off` reste inconditionnel (pouvoir arrêter en toute circonstance).

---

### POST /pump2/on, /pump2/off — WRITE

Démarre ou arrête la pompe doseuse 2 (ORP/chlore) manuellement (test bench).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump2/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump2/off
```

> ⚠️ **Garde filtration (v2.1.2)** : mêmes règles que `/pump1/on` ci-dessus.

---

### POST /pump1/duty/:duty, /pump2/duty/:duty — WRITE

Règle la puissance PWM d'une pompe (0–255).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/duty/128
```

---

### POST /ph/inject/start — WRITE

Démarre une injection manuelle pH à la puissance configurée (`pump1_max_duty_pct` ou `pump2_max_duty_pct` selon `ph_pump`). La durée est calculée à partir du volume demandé et du débit nominal de la pompe.

> ⚠️ **Garde filtration (v2.1.2)** : l'injection est **refusée HTTP 409** si la filtration est arrêtée et que `regulationMode != "continu"` (corps texte : « filtration arrêtée — injection refusée pour sécurité chimique »). Si la filtration s'arrête **pendant** l'injection, celle-ci est interrompue automatiquement (< 100 ms après détection) et une alerte MQTT `ph_injection_aborted` est publiée sur `{base}/alerts`. Pas de reprise automatique : l'utilisateur doit relancer manuellement après reprise filtration.
>
> **Limites volumétriques toujours non gardées** : l'injection manuelle ignore la limite horaire (`ph_limit_minutes`), la limite journalière (`max_ph_ml_per_day`), le délai de stabilisation et le mode de régulation. Le volume injecté **est compté** dans le cumul journalier (`ph_daily_ml`) et peut donc le dépasser. Responsabilité opérateur. Voir [docs/subsystems/pump-controller.md](subsystems/pump-controller.md) et [docs/features/page-ph.md](features/page-ph.md).

Paramètres (querystring, exclusifs) :

| Paramètre | Type | Validation |
|-----------|------|------------|
| `volume` | float (mL) | 1 – 2000 (mode préféré) |
| `duration` | integer (secondes) | fallback legacy, borné **1 – 600** (`kManualInjectMaxDurationS`, abaissé de 3600 → 600 en v2.1.2 — voir CHANGELOG) |

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/ph/inject/start?volume=15"
```

Réponses :
- `200 OK` — injection démarrée.
- `400` — paramètre manquant ou hors plage.
- `409 Conflict` — filtration arrêtée (sauf mode `continu`). Corps : texte explicite.

---

### POST /ph/inject/stop — WRITE

Arrête immédiatement l'injection manuelle pH en cours.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/ph/inject/stop
```

---

### POST /orp/inject/start — WRITE

Identique à `/ph/inject/start` pour la pompe ORP (`orp_pump`). Mêmes paramètres et contraintes.

> ⚠️ **Garde filtration (v2.1.2)** : refus **HTTP 409** si filtration arrêtée (sauf mode `continu`). Arrêt cyclique automatique si la filtration tombe en cours d'injection, alerte MQTT `orp_injection_aborted` publiée sur `{base}/alerts`. Bornage `duration` à 600 s (idem `/ph/inject/start`).
>
> **Limites volumétriques toujours non gardées** : `orp_limit_minutes`, `max_chlorine_ml_per_day`, stabilisation et mode de régulation ne sont PAS vérifiés. Le volume est compté dans `orp_daily_ml` et peut dépasser `max_chlorine_ml_per_day`. Voir [docs/subsystems/pump-controller.md](subsystems/pump-controller.md) et [docs/features/page-orp.md](features/page-orp.md).

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

## Calibration (Atlas EZO — feature-021)

Toutes les routes de calibration retournent **immédiatement** (`< 1 ms`) en mettant la commande dans la queue FreeRTOS de `SensorManager`. La commande s'exécute dans `loopTask` en ~900 ms (transaction I²C bloquante avec le module EZO). L'UI observe l'avancement via les champs WS `phCalPoints` / `orpCalPoints` rafraîchis automatiquement.

### POST /calibrate_ph — WRITE

Calibration EZO pH — déclenche `Cal,mid,7.00` (point milieu) ou `Cal,low,4.00` (point bas).

**Payload JSON :**

```json
{ "step": "mid" }    // ou { "step": "low" }
```

**Réponse 200** :

```json
{ "success": true, "queued": true, "step": "mid" }
```

**Erreurs** :
- `400 step must be 'mid' or 'low'` si payload invalide.
- `503 calibration queue saturée — réessayer dans 1s` si la queue (4 slots) est pleine.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"step":"mid"}' \
  http://poolcontroller.local/calibrate_ph
```

---

### POST /calibrate_orp — WRITE

Calibration EZO ORP — déclenche `Cal,<reference>` avec la référence en mV.

**Payload JSON :**

```json
{ "reference": 470 }
```

**Plage acceptée** : `0..1000` mV (couvre les standards usuels 225, 470, 650 mV).

**Réponse 200** :

```json
{ "success": true, "queued": true, "reference": 470 }
```

**Erreurs** :
- `400 reference must be 0..1000 mV` si valeur hors plage.
- `503 calibration queue saturée — réessayer dans 1s`.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"reference":470}' \
  http://poolcontroller.local/calibrate_orp
```

---

### POST /calibrate_clear — WRITE

Efface complètement la calibration EZO mémorisée dans le module (`Cal,clear`). À utiliser pour repartir d'une calibration vierge avant de refaire le workflow complet.

**Payload JSON :**

```json
{ "sensor": "ph" }    // ou { "sensor": "orp" }
```

**Réponse 200** :

```json
{ "success": true, "queued": true, "sensor": "ph" }
```

**Erreurs** :
- `400 sensor must be 'ph' or 'orp'`.
- `503 calibration queue saturée`.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"sensor":"ph"}' \
  http://poolcontroller.local/calibrate_clear
```

> **Routes legacy supprimées** (404 désormais) : `POST /calibrate_ph_neutral`, `POST /calibrate_ph_acid`, `POST /clear_ph_calibration`. Remplacées par les 3 routes ci-dessus. Voir [ADR-0014](adr/0014-migration-atlas-ezo.md).

---

## Diagnostic Atlas EZO (v2.1.1)

Deux endpoints permettent d'interroger directement les modules Atlas EZO sur le bus I²C, sans passer par les caches `Sensors`. Utilisés par la carte **Diagnostic EZO** de la page Paramètres → Avancé (voir [`docs/features/page-settings.md`](features/page-settings.md#card-diagnostic-ezo)) et par les agents (RMA Atlas, validation empirique d'une réponse module).

> Ces endpoints prennent le mutex `i2cMutex` (timeout `kI2cMutexTimeoutMs = 2000 ms`) et bloquent le handler HTTP pendant `delay_ms` ms (50-5000 ms). Ils ne passent **pas** par la queue `_ezoQueue` — l'opération est synchrone côté serveur web. Le rate-limit s'applique normalement.

### Codes de statut Atlas EZO

| Code | Libellé | Signification |
|------|---------|---------------|
| `1` | success | Commande exécutée, payload présent (si attendu) |
| `2` | syntax error | Commande non reconnue par le module |
| `254` | not ready | Réponse pas encore disponible — rallonger le délai |
| `255` | no data | Pas de données à retourner |
| `0` | no response | Le module n'a rien retourné (timeout I²C) |

---

### POST /debug/ezo_command — pas d'auth (cohérent avec autres `/debug/*`)

Envoie une commande Atlas EZO arbitraire et retourne la réponse parsée (status code + texte + bytes hex bruts).

**Body JSON** :

| Champ | Type | Validation |
|-------|------|------------|
| `addr` | int | `8..119` décimal (= `0x08..0x77`). Adresses Atlas par défaut : `98` = `0x62` (ORP), `99` = `0x63` (pH). |
| `cmd` | string | 1-30 caractères ASCII (ex. `I`, `Status`, `R`, `Cal,?`, `Slope,?`, `RT,25.0`). |
| `delay_ms` | int | 50-5000 ms. Délai d'attente entre l'envoi et la lecture. Défaut côté UI : 900 ms (commandes longues `R`/`Cal,*`), 300 ms (commandes courtes `I`/`Status`/`?`). |

**Réponse 200** :

```json
{
  "success": true,
  "addr": "0x62",
  "cmd": "R",
  "status_code": 1,
  "status_label": "success",
  "response": "-369.2",
  "raw_hex": ["01", "2D", "33", "36", "39", "2E", "32", "00"],
  "delay_ms": 900
}
```

**Erreurs** :
- `400` — JSON invalide, `addr` hors plage, `cmd` vide ou > 30 chars, `delay_ms` hors plage.
- `500` — Échec transmission I²C (`endTransmission != 0`) — typiquement aucun module à cette adresse.
- `503` — Bus I²C occupé (acquisition mutex échouée dans les 2 s).

**Exemple curl** :

```bash
# Lire le pH brut sur l'EZO pH
curl -X POST -H "Content-Type: application/json" \
  -d '{"addr":99,"cmd":"R","delay_ms":900}' \
  http://poolcontroller.local/debug/ezo_command

# Diagnostiquer un module silencieux (Status doit retourner ?STATUS,P,...)
curl -X POST -H "Content-Type: application/json" \
  -d '{"addr":98,"cmd":"Status","delay_ms":300}' \
  http://poolcontroller.local/debug/ezo_command
```

---

### POST /debug/ezo_factory — pas d'auth (cohérent avec autres `/debug/*`)

Restaure les paramètres usine d'un module Atlas EZO via la commande `Factory`. Calibration effacée, adresse I²C par défaut (`0x63` pH / `0x62` ORP), baud rate UART par défaut, compensation T° remise à zéro.

> ⚠️ **Le mode de communication (I²C vs UART) n'est PAS modifié** par cette commande. Le firmware EZO n'est pas touché. Après l'appel : couper / rallumer l'alimentation ESP32, puis recalibrer le module via la page de calibration.

**Paramètre query** :

| Paramètre | Type | Validation |
|-----------|------|------------|
| `addr` | int | `8..119` décimal. |

**Réponse 200** :

```json
{
  "success": true,
  "addr": "0x62",
  "note": "power-cycle ESP32 then recalibrate"
}
```

**Erreurs** :
- `400` — Paramètre `addr` manquant ou hors plage.
- `500` — Échec transmission I²C (aucun module à cette adresse).
- `503` — Bus I²C occupé.

**Exemple curl** :

```bash
# Factory reset de l'EZO ORP (0x62 = 98 décimal)
curl -X POST "http://poolcontroller.local/debug/ezo_factory?addr=98"
```

> Un log `warning` est émis côté firmware : `[Debug] Commande Factory envoyée à 0xNN — couper/rallumer l'alim puis recalibrer`.

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

## Diagnostic crash (coredump)

Ces trois endpoints permettent de consulter, télécharger et effacer le coredump ESP-IDF persisté dans la partition flash dédiée (`coredump`, 64 KB). Le dump est produit automatiquement par le firmware lors d'un crash de type `PANIC` (exception Xtensa) et persiste jusqu'à effacement explicite.

> Le contenu de la partition `coredump` **n'est pas effacé par les OTA** (firmware ou filesystem) ni par un factory reset.

### GET /coredump/info — WRITE

Retourne un résumé JSON du dernier coredump disponible.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/coredump/info
```

**Si un coredump est disponible :**

```json
{
  "available": true,
  "task": "loopTask",
  "pc": 1074038456,
  "exc_cause": 29,
  "exc_cause_str": "StoreProhibited",
  "exc_vaddr": 0
}
```

**Si aucun coredump n'est présent :**

```json
{
  "available": false,
  "partition_found": true
}
```

| Champ | Type | Description |
|-------|------|-------------|
| `available` | boolean | `true` si un coredump valide a été trouvé dans la partition |
| `task` | string | Nom de la tâche FreeRTOS qui a crashé |
| `pc` | integer | Adresse du Program Counter au moment du crash (décimale) |
| `exc_cause` | integer | Code cause d'exception Xtensa (ex. 29 = `StoreProhibited`) |
| `exc_cause_str` | string | Libellé lisible de la cause d'exception |
| `exc_vaddr` | integer | Adresse mémoire ayant déclenché l'exception (accès invalide) |
| `partition_found` | boolean | `false` si la partition `coredump` est absente de la table de partitions |

---

### GET /coredump/download — WRITE

Télécharge le coredump brut sous forme de fichier binaire. Streamé via `AsyncCallbackResponse` sans allocation de 64 KB côté firmware.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/coredump/download -o coredump.bin
```

- **Content-Type** : `application/octet-stream`
- **Content-Disposition** : `attachment; filename="coredump.bin"`
- Retourne `404` si aucun coredump n'est disponible.

Pour décoder le fichier obtenu :

```bash
./tools/decode_coredump.sh coredump.bin
```

---

### DELETE /coredump — WRITE

Efface la partition `coredump` pour permettre l'enregistrement d'un prochain crash.

```bash
curl -u admin:monmotdepasse -X DELETE http://poolcontroller.local/coredump
```

```json
{ "success": true }
```

> ⚠️ L'effacement est irréversible. Télécharger le dump avant d'effacer.

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

## Endpoints ajoutés en feature-020 (identification 2 sondes DS18B20, PCB v2)

Tous les endpoints `/sensors/onewire/*` requièrent l'auth (HTTP Basic + token).

### `GET /sensors/onewire/scan`

Liste les sondes DS18B20 actuellement détectées sur le bus OneWire (GPIO 5) avec leurs adresses ROM, T° brutes lues du cache (lecture rapide non bloquante) et leur rôle assigné.

```json
{
  "sondes": [
    { "address": "28FF1A2B3C4D5E6F", "temperature": 18.4, "role": "water" },
    { "address": "28FF9988776655AA", "temperature": 24.1, "role": "unknown" }
  ],
  "identified_count": 1,
  "detected_count": 2
}
```

`role` ∈ `"water" | "circuit" | "unknown"`. La T° peut être périmée jusqu'à `kPhOrpSensorIntervalMs` (~5 s) car le handler lit le cache mis à jour par `Sensors::update()` — pas de `requestTemperatures()` synchrone (qui prendrait 750 ms en 12-bit, > timeout 50 ms d'AsyncWebServer).

### `POST /sensors/onewire/identify`

Assigne une sonde à un rôle. **Auto-permutation activée** : si une autre sonde avait déjà le rôle demandé, elle bascule automatiquement à l'autre rôle.

Payload :

```json
{ "address": "28FF1A2B3C4D5E6F", "role": "water" }
```

Réponse : `{ "success": true }`. Erreurs : 400 si adresse hex invalide ou role hors `"water"|"circuit"` ; 404 si l'adresse n'est pas présente sur le bus.

### `POST /sensors/onewire/reset`

Efface l'identification persistée NVS (clés `ow_water_addr` et `ow_circuit_addr`). L'utilisateur devra refaire le workflow d'identification.

Payload : `{}` (objet vide). Réponse : `{ "success": true }`.

Voir [ADR-0013](adr/0013-identification-sondes-onewire.md) pour la décision d'identification + alternatives écartées.

## Endpoints ajoutés en feature-024 (pente sonde pH)

### `POST /debug/ph_slope_refresh` — pas d'auth (cohérent avec autres `/debug/*`)

Force une nouvelle interrogation `Slope,?` sur l'EZO pH sans attendre le cycle automatique 24 h ni une recalibration. Utile pour valider la chaîne de mesure ou une nouvelle calibration.

```bash
curl -X POST http://poolcontroller.local/debug/ph_slope_refresh
```

**Réponse 200** :

```json
{ "success": true, "queued": true }
```

**Réponse 503** (queue EZO pleine OU query déjà en attente) :

```json
{ "error": "queue full or already pending" }
```

L'UI page `/ph` (chip d'état sonde) utilise cet endpoint via le bouton « Rafraîchir » du modal détails. La nouvelle valeur arrive dans le payload WS dès que le handler EZO traite la commande (~1-2 s côté `loopTask`). Voir [feature-024](../specs/features/done/feature-024-pente-sonde-ph.md) et [`docs/features/page-ph.md`](features/page-ph.md#chip-détat-sonde-feature-024).

### `POST /debug/sensor_filter_reset` — pas d'auth (cohérent avec autres `/debug/*`)

Réinitialise **les deux** filtres pH et ORP (médiane + EMA). Les filtres repassent en **warmup** → le dosage automatique est bloqué jusqu'à ce que chaque filtre ait reçu `kSensorFilterWarmupSamples` (= 5) nouvelles mesures valides. Utile après un débranchement EZO, un test, ou une recalibration manuelle hors workflow.

```bash
curl -X POST http://poolcontroller.local/debug/sensor_filter_reset
```

**Réponse 200**

```json
{ "success": true, "reset": ["ph", "orp"] }
```

### `GET /debug/sensor_filter_state` — pas d'auth (cohérent avec autres `/debug/*`)

Retourne l'état brut des deux filtres pour diagnostic. Lecture lock-free des getters `SensorManager` (valeurs scalaires).

```bash
curl http://poolcontroller.local/debug/sensor_filter_state
```

**Réponse 200**

```json
{
  "ph":  { "raw": 7.241, "median": 7.236, "filtered": 7.234, "ready": true, "unstable": false, "rejected": 0 },
  "orp": { "raw": 722, "median": 720, "filtered": 720, "ready": true, "unstable": false, "rejected": 0 }
}
```

Les champs `raw` / `median` / `filtered` valent `null` si la mesure est indisponible. Voir [feature-025](../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#reset-manuel--diagnostic).

### `GET /debug/ph_trace` — pas d'auth (cohérent avec autres `/debug/*`)

Retourne le ring buffer de diagnostic d'oscillation pH (~25 min d'historique, 1 échantillon par cycle capteur). Consommé par la card « Debug oscillation pH » du panel **Avancé** (voir [`docs/features/page-settings.md`](features/page-settings.md)).

```bash
curl http://poolcontroller.local/debug/ph_trace
```

**Réponse 200**

```json
{
  "count": 2,
  "interval_ms": 5000,
  "now": 1234567,
  "samples": [
    { "t": 1229000, "ph": 7.241, "phFiltered": 7.234, "orp": 720, "tempC": 24.5 },
    { "t": 1234000, "ph": 7.255, "phFiltered": 7.236, "orp": 721, "tempC": 24.5 }
  ]
}
```

- `t` : horodatage `millis()` de l'échantillon (à comparer à `now`).
- `ph` : pH **brut** (`_lastPh`), arrondi 3 décimales, `null` si indisponible.
- `phFiltered` : pH **lissé** (médiane + EMA, `_phFilter.filtered()`), arrondi 3 décimales, `null` si le filtre n'est pas amorcé. Permet de comparer visuellement l'effet du filtre feature-025 face au brut.
- `orp` : ORP brut (arrondi 0,1), `null` si indisponible.
- `tempC` : température envoyée à la compensation (arrondi 0,1), `null` si indisponible.

### `POST /debug/ph_trace_clear` — pas d'auth (cohérent avec autres `/debug/*`)

Vide le ring buffer de diagnostic d'oscillation pH. Réponse : `{ "success": true }`.
