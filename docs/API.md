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

### Politique d'origine — pas de CORS

Le contrôleur applique une **politique même-origine stricte** ([ADR-0023](adr/0023-politique-cors-retrait.md), v2.11.2) : aucun en-tête `Access-Control-Allow-*` n'est émis, aucun preflight `OPTIONS` n'est traité. L'UI est servie par l'ESP32 lui-même — un navigateur ne peut pas appeler cette API depuis une page hébergée sur une autre origine. Les clients non-navigateur (`curl`, scripts, intégrations serveur) ne sont pas concernés. Pour une intégration cross-origin navigateur, passer par un reverse proxy.

Détails techniques :
- La comparaison du token API est à **temps constant** (HTTP et WebSocket) — voir [docs/subsystems/auth.md](subsystems/auth.md).
- Le token n'est accepté **que** via le header `X-Auth-Token` (jamais en query param `?token=...`).

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
  "phFilterAlarm": false,
  "orpRaw": 722,
  "orpMedian": 720,
  "orpFiltered": 720,
  "orpFilterReady": true,
  "orpFilterUnstable": false,
  "orpRejectedCount": 0,
  "orpFilterAlarm": false,
  "filtration_running": true,
  "water_present": true,
  "measures_valid": true,
  "measures_stab_remaining_s": 0,
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
>
> **feature-061 (v2.13.0)** : champs de **validité des mesures pH/ORP** ajoutés (mêmes champs côté WS `sensor_data`, même source : cache du singleton `measureValidity`) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `water_present` | boolean | Présence d'eau résolue (source unique feature-056 — déjà présent en WS, **ajouté à `/data`** ici). |
> | `measures_valid` | boolean | `true` = mesures pH/ORP représentatives du bassin. `false` = eau absente **ou** délai de purge post-reprise en cours (`stabilizationDelayMin`, défaut 5 min). |
> | `measures_stab_remaining_s` | integer | Secondes restantes de purge (arrondi seconde sup) ; `0` si valide ou eau absente. Nom **distinct** de `stabilization_remaining_s` (stabilisation post-calibration des pompes). |
>
> **feature-067 (v2.20.0) — sémantique brut / corrigé des valeurs pH.** Aucun champ n'est ajouté ni renommé dans `/data`, mais la **valeur** de trois d'entre eux intègre désormais l'offset logiciel `ph_offset` :
>
> | Champ | Contenu | Offset appliqué ? |
> |-------|---------|-------------------|
> | `ph` | pH filtré **corrigé** (repli sur le pH courant corrigé tant que le filtre n'est pas amorcé — jamais sur le brut) | ✅ oui |
> | `phMedian` | médiane glissante **corrigée** | ✅ oui |
> | `phFiltered` | pH lissé (médiane + EMA) **corrigé** | ✅ oui |
> | `phRaw` | dernière mesure **brute Atlas EZO** | ❌ non |
>
> Règle mnémotechnique côté firmware comme côté JSON : **seuls les champs suffixés `Raw` sont bruts**. Le filtre pH continue de travailler sur le signal brut (latch EMI et seuils de rejet inchangés) ; l'offset est appliqué **en sortie**, donc un changement est visible immédiatement. `NaN` en entrée ⇒ `null` en sortie : l'offset ne fabrique jamais une mesure. Voir [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#offset-logiciel-de-mesure-ph--feature-067).

> **v2.20.3 — alarme « filtre bloqué »** (mêmes champs côté WS `sensor_data`, mêmes valeurs) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `phFilterAlarm` | boolean | `true` quand le filtre pH est resté **non prêt plus de 5 min** (`kSensorFilterNotReadyAlarmMs`) **alors que** la filtration tournait **et** que les mesures étaient déclarées représentatives → plus **aucune** régulation pH (ni chlore, par fail-closed sur pH brut). Une purge de la chambre de mesure (`measures_valid = false`), un warmup ou une filtration à l'arrêt ne l'arment **jamais**. |
> | `orpFilterAlarm` | boolean | Idem pour le filtre ORP. |
>
> Ces drapeaux sont **edge-triggered côté firmware** (un `critical` + une alerte MQTT à la pose, répétition toutes les 30 min, `info` à la levée) et pilotent le chip d'état filtre + la bannière « Régulation à l'arrêt » du tableau de bord. Reprise : [`POST /reset-sensor-filter`](#post-reset-sensor-filter--critical). Buffer `/data` porté de 1280 à **1344** octets. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#alarme--filtre-non-prêt-durablement--v2203).

> Les champs live `ph` / `orp` / `phRaw` / … restent publiés tels quels quand `measures_valid` est `false` (calibration et diagnostics intacts) — c'est le **client** qui arbitre l'affichage. ⚠️ **Repli HTTP** : `/data` n'expose pas `filtration_state_stale` (présent en WS uniquement) — un client en polling HTTP dégrade « Signal filtration périmé » en « Filtration arrêtée » (asymétrie préexistante, à harmoniser). Voir [`docs/subsystems/measure-validity.md`](subsystems/measure-validity.md).

---

### GET /get-history — WRITE

Retourne l'historique des mesures.

```bash
curl -u admin:monmotdepasse "http://poolcontroller.local/get-history?range=24h"
```

Paramètre `range` : `24h` (défaut), `3d`, `7d`, `30d`, `all`. Une valeur **inconnue** est traitée comme `all` — et depuis la v2.14.0, seul le **token reconnu** est échoé dans le champ `range` de la réponse (jamais la valeur brute du client, qui pourrait malformer le JSON).

Paramètre optionnel `?since=TIMESTAMP` pour récupération incrémentale.

**Format de réponse** (inchangé octet pour octet depuis les versions antérieures) :

```json
{"range":"24h","count":0,"history":[{"timestamp":1753860000,"ph":7.2,"orp":650,"temperature":24.6,"filtration":true,"dosing":false,"granularity":0}, "…"],"count":123}
```

> ⚠️ La clé `count` apparaît **deux fois** : `0` dans l'en-tête, la valeur réelle dans le pied. Bizarrerie historique **conservée volontairement** pour compatibilité (les parseurs JSON gardent la dernière occurrence).

> **feature-061 (v2.13.0)** : les points enregistrés pendant un arrêt de filtration (ou pendant la purge post-reprise) ont `ph` et `orp` à **`null`** (contrat existant, identique à un capteur absent) — `temperature`, `filtration` et `dosing` restent renseignés. Voir [`docs/subsystems/history.md`](subsystems/history.md#exclusion-des-mesures-phorptempérature-sans-circulation-deau-feature-061-v2130).

**Transfert chunked (v2.14.0, [ADR-0032 §Extension 2026-07-30](adr/0032-reponses-http-bornees.md#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140))** : la réponse est émise en `Transfer-Encoding: chunked` (mécanisme (b), patron `/download-logs`) — plus aucune `String` contiguë ~40 Ko épinglée pendant le transfert, et le **503** « heap fragmenté » de la v2.10.4 **n'existe plus sur cette route** (aucune garde heap nécessaire en chunked). Transparent pour `fetch()`/curl : contrat JSON identique.

**Instantané sous mutex unique (v2.18.2)** : le serveur copie un **instantané filtré** de l'historique en **une seule** prise de mutex (100 ms) à l'ouverture de la requête, puis streame **sans verrou**. Une réponse `200` est donc un JSON **complet et cohérent** (aucun glissement de buffer possible : l'instantané est figé, ~11 Ko transitoires libérés en fin de réponse).

**Erreur `503` (rare)** : si le mutex est occupé au moment de la requête (consolidation/sauvegarde de l'historique en cours, ~1,5 s toutes les 5 min) :

```json
{"error":"historique occupé, réessayez"}
```

Le client réessaie simplement (le dashboard re-sollicite la route au refresh suivant).

> ℹ️ **Note historique** : entre la v2.14.0 et la v2.18.1, **toutes** les réponses `/get-history` étaient tronquées (pied `],"count":N}` jamais émis — bug de machine à états corrigé en v2.18.2). La note v2.18.1 sur les « espaces de maintien de flux sous contention » relevait d'un diagnostic **erroné** ; ce mécanisme a été retiré en v2.18.2 — le corps ne contient plus de whitespace entre les tokens.

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

Retourne les logs système par **pages bornées** (v2.10.4, [ADR-0032](adr/0032-reponses-http-bornees.md)). Paramètre optionnel `?since=TIMESTAMP` : ne retourne que les entrées dont le timestamp est **strictement supérieur** — c'est à la fois le curseur de pagination et le mécanisme de récupération incrémentale.

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/get-logs
curl -u admin:monmotdepasse "http://poolcontroller.local/get-logs?since=14400000"
```

```json
{
  "uptime_ms": 18000000,
  "logs": [
    { "timestamp": 14400000, "level": "INFO", "message": "Capteurs initialisés" },
    { "timestamp": 14405000, "level": "WARNING", "message": "pH hors limites: 7.8" }
  ],
  "truncated": false
}
```

> `timestamp` est exprimé en millisecondes depuis le démarrage de l'ESP32 (`millis()`).

**Pagination** (v2.10.4) :

- Page de **30 entrées** (`kLogsMaxEntriesPerPage`, ~4 Ko de JSON au pire), extensible jusqu'au **plafond dur de 60** (`kLogsPageHardCap`) **uniquement** pour inclure la queue d'entrées partageant la même milliseconde que la dernière entrée incluse — le curseur `?since=` étant strictement supérieur, les exclure les perdrait définitivement entre deux pages (règles d'admission : [`src/logs_page.h`](../src/logs_page.h)).
- `truncated: true` → il reste des entrées au-delà de cette page ; le client reboucle avec `?since=<timestamp de la dernière entrée reçue>` jusqu'à `truncated: false`. Un client ancien qui ignore `truncated` reste fonctionnel (il voit simplement la première page).
- Le document JSON de la page est construit en **une seule passe sous le mutex logger**, l'échappement est délégué à `serializeJson` (l'échappement manuel a été supprimé).

**Réponse 503** (`{"error":"heap fragmenté, réessayez"}`) : la taille **réelle** de la page (`measureJson`) + `kHttpHeapMarginBytes` (4 Ko) est comparée au plus gros bloc contigu libre (`ESP.getMaxAllocHeap()`) **avant** toute allocation ; sinon le stream est pré-dimensionné exactement (aucun `resize()`). Remplace la garde v2.8.2 à seuil fixe `kMinHeapForLogsResponse` (24 Ko), supprimée — un seuil constant indépendant de la taille réelle de la réponse ne protégeait pas (2 coredumps, 2026-07-23 et 2026-07-24).

---

### GET /download-logs — WRITE

Télécharge les logs système sous forme de fichier texte (`pool_logs.txt`).

```bash
curl -u admin:monmotdepasse http://poolcontroller.local/download-logs -o pool_logs.txt
```

```
# Pool Controller — Journal système
# Exporté le : 2026-07-24 09:41:00
# Format: [YYYY-MM-DD HH:MM:SS] NIVEAU : message

# === Historique persistant ===
[2026-07-23 08:12:04] INFO : Démarrage dosage pH: pH=7.85 cible=7.20 erreur=+0.65
# === Fin historique persistant ===

# === Session courante ===
[2026-07-24 09:31:00] INFO : Démarrage dosage pH: pH=7.78 cible=7.20 erreur=+0.58
```

> Heure non synchronisée → timestamps relatifs `[+HH:MM:SS]` depuis le boot. Les logs enrichis incluent les événements de dosage (démarrage, arrêt, paramètres) ainsi que les alertes de limites horaires/journalières.

**Réponse en flux chunked, zéro accumulation RAM** (v2.10.4, [ADR-0032](adr/0032-reponses-http-bornees.md)) : l'export est produit par une machine à états (`beginChunkedResponse`, patron `/coredump/download`) qui remplit directement le buffer TCP borné — d'abord le fichier persistant `/system.log` (lu par blocs), puis les entrées du buffer RAM (copiées une à une sous mutex via `Logger::getLogEntryByIndex`). Aucune garde heap n'est nécessaire (rien de proportionnel à la taille de l'export n'est alloué) : **plus de réponse 503** sur cette route.

Limites assumées de l'export (best-effort, route de diagnostic) :

- Lignes formatées > **256 octets** (`kLogExportLineMaxBytes`) tronquées avec suffixe « … ».
- Le ring RAM peut tourner entre deux chunks (doublon ou saut d'entrée possibles) ; un timeout du mutex logger termine la section RAM sans erreur.

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
  "install_mode": "managed",
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
  "max_cycles_per_day": 24,
  "time_use_ntp": true,
  "ntp_server": "pool.ntp.org",
  "timezone_id": "europe_paris",
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
  "debug_logs_enabled": false,
  "ph_offset": 0,
  "ph_auto_manual_epoch": 0,
  "ph_auto_manual_reason": "",
  "boost_active": false,
  "boost_until": 0
}
```

---

Champs notables de la réponse :

| Champ | Type | Description |
|-------|------|-------------|
| `install_mode` | string | **Mode d'installation** (feature-056, v2.19.0). `"managed"` (PoolController pilote la filtration : relais GPIO 26 piloté, programmation active, eau présente = filtration commandée ON), `"powered"` (contrôleur alimenté par le circuit de filtration : relais inerte, programmation masquée, eau présumée présente en permanence — ex-`continu`), `"external"` (filtration tierce signalée : relais inerte, eau présente seulement si dernier signal ON reçu < 180 s, sinon dosage suspendu). Remplace `regulation_mode` **et** `filtration_enabled`. Voir [ADR-0026](adr/0026-mode-installation.md). |
| `regulation_speed` | string | `"slow"` / `"normal"` / `"fast"` — préréglages PID |
| `stabilization_delay_min` | integer | Délai de stabilisation capteurs après démarrage filtration (0–60 min) |
| `ph_limit_minutes` | integer | Durée max d'injection pH par fenêtre glissante d'1 h (minutes, 1–60) |
| `orp_limit_minutes` | integer | Durée max d'injection ORP par fenêtre glissante d'1 h (minutes, 1–60) |
| `max_cycles_per_day` | integer | Limite anti-cycling (garde 9, feature-060, v2.12.0) : démarrages de dosage max **par pompe et par jour**, compteur partagé régulation auto + injections manuelles. Défaut **24** (était 48 — abaissé en v2.20.5, cf. [pump-controller.md](subsystems/pump-controller.md#limite-de-cyclesjour-configurable-garde-9-feature-060-v2120)). Bornes dures **10–60** clampées côté serveur (voir la table de validation `/save-config` ci-dessous). Persisté en NVS (`max_cycles_day`) — le nouveau défaut **ne s'applique donc pas** à un appareil déjà provisionné, aucune migration n'écrase la valeur enregistrée. |
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
| `boost_active` | boolean | Mode Boost actif (feature-053, v2.18.0). `true` tant que la surchloration temporaire du jour est en cours. Piloté par `POST /boost/start` \| `/boost/stop` (jamais par `/save-config`). Persisté en NVS (survit à un reboot dans la journée), expire automatiquement au prochain minuit local. Voir [ADR-0025](adr/0025-mode-boost.md). |
| `boost_until` | integer (epoch) | Instant d'expiration du Boost (epoch UNIX, prochain minuit local). `0` si le Boost est inactif. Permet à l'UI d'afficher l'heure de fin. |
| `ph_auto_manual_epoch` | integer (epoch) | **v2.20.3** — Instant (epoch UNIX) de la dernière **bascule automatique** de la régulation pH en mode `manual` par le filet de sécurité « refus de plancher acide répétés » (feature-067). `0` = aucune bascule en attente d'acquittement. Vaut `0` si l'horloge n'était pas synchronisée au moment de la bascule (la `reason` reste renseignée). Persisté en NVS (`ph_am_epoch`). **Lecture seule** : jamais accepté en écriture par `/save-config`. |
| `ph_auto_manual_reason` | string | **v2.20.3** — Cause en clair (français) de cette bascule, ex. `"3 refus de plancher pH en 24 h — sonde pH à recalibrer ou remplacer ; régulation pH basculée en manuel"`. Chaîne vide si `ph_auto_manual_epoch` vaut `0`. Persisté en NVS (`ph_am_reason`). |

> **Trace de bascule automatique pH (v2.20.3)** — le journal système est un ring buffer : après quelques heures il ne restait **aucune** trace de l'escalade de sécurité, et il devenait impossible de dire si le mode `manual` venait du firmware ou d'une action humaine (incident du 2026-08-16). Les deux champs ci-dessus sont donc **persistés en NVS**, dans la **même transaction** que le mode lui-même.
>
> **Acquittement** : la trace est effacée dès qu'un `POST /save-config` (ou une commande MQTT) provoque un **changement réel** du champ `ph_regulation_mode`. Renvoyer le mode courant à l'identique — ce que fait l'interface à chaque sauvegarde des réglages — **n'acquitte rien**. Clés NVS absentes sur un appareil mis à jour ⇒ `0` / `""` : rétrocompatible. Voir [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#persistance-de-la-bascule-automatique-en-manuel-v2203).
| `ph_offset` | float | **Offset logiciel de mesure pH** (feature-067, v2.20.0). `pH corrigé = pH brut EZO + ph_offset`. Plage **[−0.50, +0.50]**, pas de 0,01, défaut **0** (inerte). Persisté en NVS (clé `ph_offset`). **Ne touche jamais la calibration interne du module Atlas EZO** : c'est une correction en aval de la lecture, destinée à compenser un écart d'implantation de la sonde. Voir [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md). |

**`POST /save-config` — champs pH spécifiques :**

| Champ | Type | Validation |
|-------|------|------------|
| `ph_regulation_mode` | string | Valeurs acceptées : `"automatic"`, `"scheduled"`, `"manual"`. Toute autre valeur est ignorée. Met à jour `ph_enabled` automatiquement. |
| `ph_daily_target_ml` | integer | Doit être ≥ 0 et ≤ `max_ph_ml_per_day` (si configuré). Retourne HTTP 400 si la limite est dépassée. |
| `ph_offset` | float | Doit être dans **[−0.50, +0.50]** (`kPhOffsetMin` / `kPhOffsetMax` de [`constants.h`](../src/constants.h)). Hors plage → **HTTP 400**, **aucun** champ de la requête n'est appliqué. Champ **absent** ou **non numérique** → paramètre inchangé (patron des autres champs). Prise en compte **immédiate** (pas de reboot, pas d'attente de la fenêtre de lissage). Chaque changement effectif est journalisé en `INFO` : `Offset pH modifié: ancien=X.XX nouveau=Y.YY`. |

**Corps de la réponse 400 (`ph_offset` hors plage)** :

```json
{"error":"Offset pH invalide : la valeur doit être comprise entre -0.50 et +0.50 pH"}
```

> ⚠️ **Exception assumée au patron `/save-config`** : `ph_offset` est le **premier** champ de cette route à **refuser** la requête plutôt qu'à écrêter silencieusement la valeur (cf. `max_cycles_per_day`, clampé). Un offset écrêté serait un **mensonge de mesure** : l'utilisateur croirait avoir enregistré ±0,80 alors que la régulation appliquerait ±0,50. L'incohérence de style est délibérée.
>
> Effets de bord volontairement **absents** : enregistrer un offset ne réinitialise **aucun** compteur de dosage (cycles du jour, cumul journalier, pause anti-cycling). Sinon des enregistrements répétés permettraient de contourner les limites journalières. Un enregistrement pendant une injection en cours ne l'interrompt pas : le nouvel offset s'applique au prochain cycle d'évaluation.

**`POST /save-config` — champs ORP spécifiques :**

| Champ | Type | Validation |
|-------|------|------------|
| `orp_regulation_mode` | string | Valeurs acceptées : `"automatic"`, `"scheduled"`, `"manual"`. Toute autre valeur est ignorée. Met à jour `orp_enabled` automatiquement. |
| `orp_daily_target_ml` | integer | Doit être ≥ 0 et ≤ `max_orp_ml_per_day` (si configuré). Retourne HTTP 400 si la limite est dépassée. |

**`POST /save-config` — mode d'installation :**

| Champ | Type | Validation |
|-------|------|------------|
| `install_mode` | string | Valeurs acceptées : `"managed"`, `"powered"`, `"external"`. Toute autre valeur est ignorée (mode courant conservé). |

**`POST /save-config` — protection pompes (feature-060, v2.12.0) :**

| Champ | Type | Validation |
|-------|------|------------|
| `max_cycles_per_day` | integer | Toute valeur hors bornes est **silencieusement clampée côté serveur** dans **[10, 60]** (`clampCyclesPerDay`, bornes `kCyclesPerDayMin` / `kCyclesPerDayMax` de [`constants.h`](../src/constants.h)) : la requête retourne 200, la valeur **clampée** est appliquée et persistée, et un `WARN` est journalisé (`max_cycles_per_day hors bornes : X → Y`). Le pré-clamp `min`/`max` du champ UI est purement indicatif — le serveur fait autorité. Toute modification effective est également journalisée en `WARN` (ancienne → nouvelle valeur). |

> ⚠️ **Retirés en v2.19.0 (feature-056)** : les champs `regulation_mode` et `filtration_enabled` **n'existent plus** dans `/get-config` ni `/save-config`. Ils sont fusionnés dans `install_mode` (voir [ADR-0026](adr/0026-mode-installation.md)). La migration NVS est automatique — voir [UPDATE_GUIDE.md](UPDATE_GUIDE.md).

> ⚠️ **Retiré en v2.17.1** : le champ `screen_enabled` **n'existe plus** dans `/get-config` ni `/save-config` (retrait du réglage devenu sans objet après la suppression du support écran LVGL en v2.17.0, feature-065). Compatibilité legacy : un client qui envoie encore `screen_enabled` dans `/save-config` est **ignoré silencieusement** (aucune erreur, les autres champs sont traités normalement). La clé NVS associée n'est plus écrite ni lue (clé orpheline inoffensive sur les appareils existants).

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
  "free_heap_8bit": 52384,
  "max_alloc_8bit": 17420,
  "max_alloc_internal": 27584,
  "oom_handler_registered": true,
  "heap_size": 327680,
  "psram_size": 8388608,
  "psram_free": 8257536,
  "flash_size": 4194304,
  "ota_partition": "app0",
  "ota_partition_size": 1703936,
  "sketch_size": 1427753,
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

#### Champs mémoire — deux familles à ne pas confondre (v2.19.0)

Trois champs **additifs** ont été ajoutés en v2.19.0 ([ADR-0038](adr/0038-metrique-memoire-allouable-8-bits.md)). Les champs existants (`free_heap`, `heap_size`) conservent **exactement** leur sémantique d'origine : aucun client existant n'est cassé.

| Champ | Source | Signification |
|---|---|---|
| `free_heap` | `ESP.getFreeHeap()` = `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` | **Mesure brute historique.** Pool interne **total**, IRAM accessible en 32 bits comprise → **surestime** la mémoire disponible |
| `free_heap_8bit` | `heap_caps_get_free_size(MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` | **Mémoire réellement allouable** par un `malloc()` / `new` |
| `max_alloc_8bit` | `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` | **Plus gros bloc contigu réellement allouable** — la grandeur qui décide si un `new` de 356 o passe ou non |
| `max_alloc_internal` | `ESP.getMaxAllocHeap()` | Plus gros bloc du pool interne **total** (mesure brute) — publié pour rendre l'**écart** visible |

**Pourquoi les deux familles sont publiées** : `MALLOC_CAP_INTERNAL` inclut de l'IRAM qui n'est accessible qu'en accès 32 bits alignés et qu'aucune allocation ordinaire ne peut utiliser. Sur l'appareil de référence, `max_alloc_internal` est resté **figé à ~27 584 o pendant des jours** (reliquat d'IRAM) pendant que des allocations de 92 octets échouaient réellement. L'écart `max_alloc_internal − max_alloc_8bit` **chiffre ce piège** ; le publier évite qu'il se referme lors d'un futur cycle.

**Pour un intégrateur** : superviser `free_heap_8bit` et surtout `max_alloc_8bit`. `free_heap` ne doit **jamais** servir de critère d'alarme.

**Compatibilité** : les trois champs sont absents d'un firmware **≤ 2.18.9** — un client doit traiter leur absence comme « information indisponible » et retomber sur `free_heap` (c'est ce que fait l'interface web). Budget JSON mesuré avec `measureJson()` sur des valeurs pires cas : **716 octets** (628 avant l'ajout).

#### `oom_handler_registered` — témoin du parachute OOM (v2.20.2, additif)

| Champ | Type | Signification |
|---|---|---|
| `oom_handler_registered` | booléen | `true` si le `std::set_new_handler` du [parachute OOM](subsystems/oom-guard.md) est **actuellement inscrit** |

**Pourquoi ce champ existe** : le `new_handler` **se désinscrit lui-même** quand la réserve d'urgence est déjà consommée, et jusqu'à la v2.20.2 sa ré-inscription se trouvait derrière un `return`. L'appareil de production tournait donc **sans filet anti-`bad_alloc` ~2 h après chaque boot, sans que rien ne le signale** — c'est la cause directe de quatre crashs `abort()` sur la tâche `async_tcp`. Le journal, lui, s'écrasait en 25-35 min : il ne pouvait pas servir de témoin.

- **Lecture attendue en régime sain** : `true`. Un `false` **persistant** signifie que le parachute est absent **et** qu'aucune réserve n'est armée (la ré-inscription est conditionnée à la présence d'une réserve) — donc que le tas est durablement dégradé.
- **Coût** : lecture d'un miroir atomique — ni allocation, ni log, ni parcours de tas.
- **Ajout purement additif** : budget JSON `/get-system-info` mesuré à **748 octets** pires cas (716 avant). Absent d'un firmware ≤ 2.20.1 → traiter l'absence comme « information indisponible ».

> `sketch_size` (octets, `ESP.getSketchSize()`) et `ota_partition_size` (octets, taille de la partition app active) permettent de calculer l'occupation flash du firmware — affichée sur la ligne « Firmware (flash) » de la carte Infos système (v2.5.2). `sketch_size` ajouté en v2.5.2 (feature-047).
>
> `psram_size` / `psram_free` (octets, `ESP.getPsramSize()` / `ESP.getFreePsram()`, ajoutés en v2.18.0, feature-066) : PSRAM du module (WROVER-E). **`psram_size = 0` signifie PSRAM absente** (module WROOM) — l'UI affiche alors « absente » sur la ligne PSRAM de la card Infos système. Depuis la v2.18.7, `psram_size === 0` sert aussi de **condition d'indisponibilité de la mise à jour en ligne** côté interface (bouton « Vérifier » neutralisé + explication permanente dans la carte GitHub, voir [features/page-settings.md](features/page-settings.md)) ; si le champ est **absent** de la réponse, l'UI ne modifie rien (compatibilité ascendante).

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
| `phFilterAlarm` / `orpFilterAlarm` | boolean | **v2.20.3** — `true` quand le filtre est resté non prêt > 5 min alors que la filtration tournait **et** que les mesures étaient représentatives → plus aucune régulation. Jamais armé pendant une purge, un warmup ou une filtration à l'arrêt. Voir [`ws-manager.md`](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2203-alarme-filtre-bloqué). |
| `phMixingDelayActive` | boolean | `true` pendant la pause mélange hydraulique pH (15 min après injection). |
| `orpMixingDelayActive` | boolean | `true` pendant la pause mélange hydraulique ORP (20 min après injection). |
| `phDoseBlockedReason` | string \| null | Dernière cause de refus de `canDose(0)` (ex `"filtre capteur non prêt (warmup / EZO injoignable)"`, `"pause mélange en cours"`). `null` si dosage autorisé. |
| `orpDoseBlockedReason` | string \| null | Équivalent ORP. |

Champs ajoutés en feature-011 (répartition scheduled, v2.8.0 — [ADR-0021](adr/0021-repartition-scheduled.md)) :

| Champ | Type | Description |
|-------|------|-------------|
| `ph_scheduled_flow_ml_per_min` | float \| null | Débit moyen **planifié** restant du mode Programmée pH (mL/min, 1 décimale) = volume restant à injecter / minutes de filtration restantes (bornées à minuit). `null` si le mode pH n'est pas `scheduled`, hors plage de filtration, ou heure locale ESP32 invalide — l'UI affiche alors « — ». |
| `orp_scheduled_flow_ml_per_min` | float \| null | Équivalent ORP (chlore). Mêmes conditions de nullité. |

Champs ajoutés en feature-056 (mode d'installation, v2.19.0 — [ADR-0026](adr/0026-mode-installation.md)) :

| Champ | Type | Description |
|-------|------|-------------|
| `install_mode` | string | Mode d'installation actif : `"managed"` / `"powered"` / `"external"`. |
| `water_present` | boolean | Présence d'eau résolue (`resolveWaterPresent`), **source unique** des gardes de dosage. `managed` → filtration commandée ON ; `powered` → toujours `true` ; `external` → signal ON récent (< 180 s). |
| `filtration_state_source` | string | Origine de `water_present` : `"commanded"` (managed), `"powered"` (powered), `"external"` (external). |
| `filtration_state_stale` | boolean | Mode `external` uniquement : `true` si un signal a déjà été reçu mais qu'il est périmé (> 180 s) → dosage suspendu (fail-safe). |
| `filtration_ext_known` | boolean | Mode `external` : `true` dès qu'au moins un signal externe a été reçu depuis le boot (`false` = « aucun signal », fail-safe OFF). |
| `filtration_ext_on` | boolean | Dernier état signalé par la filtration externe (`ON`/`OFF`). |
| `filtration_ext_age_s` | integer | Âge en secondes du dernier signal externe (`0` si `filtration_ext_known` est `false`). |

> Le WebSocket pousse la configuration complète à la connexion initiale ; les mises à jour suivantes sont différentielles (seuls les champs modifiés sont inclus).

---

## Contrôle

### POST /pump1/on, /pump1/off — WRITE

Démarre ou arrête la pompe doseuse 1 (pH) manuellement (test bench).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/off
```

**Paramètre optionnel (v2.20.5)**

| Paramètre | Type | Défaut | Description |
|---|---|---|---|
| `duty` | entier | `255` (`MAX_PWM_DUTY`) | Rapport cyclique PWM appliqué à la pompe, borné `[0, 255]`. `duty=0` équivaut à `/pump1/off`. Accepté en query-string ou en corps de formulaire. |

```bash
# Balayage du seuil de rotation réel : 96/255 pendant 120 s
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/pump1/on?duty=96"
sleep 120
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/off
```

> **Motif** : ce paramètre sert à **caractériser au banc** le duty en dessous duquel la pompe cesse réellement de tourner. Le régulateur commande des duty au ras de `MIN_ACTIVE_DUTY` (82/255 pour une erreur de 0,8 pH) où la pompe ne tourne que par intermittence, alors que le modèle `duty ↔ débit` la croit à 6,4 mL/min — le volume est donc compté sur un produit qui ne sort pas. Voir [pump-controller.md — Conversion duty ↔ débit](subsystems/pump-controller.md#conversion-duty--débit).
>
> ⚠️ Il n'est **volontairement pas** borné par `MIN_ACTIVE_DUTY` (80) : sonder **sous** le plancher est précisément l'objet de la mesure. Sûreté : le paramètre ne peut que **réduire** la puissance par rapport au 255 déjà atteignable sans lui, et **toutes** les gardes restent en amont (auth `WRITE`, présence d'eau, inhibition maintenance). Sans le paramètre, le comportement est **strictement inchangé**.
>
> Journal : `[Test] Pompe 1 démarrée en mode manuel (duty=N)`.

> ⚠️ **Garde filtration (v2.1.2)** : `/pump1/on` retourne **`409 Conflict`** si la filtration est arrêtée et que `regulationMode != "continu"`. `/pump1/off` reste inconditionnel (pouvoir arrêter en toute circonstance).
>
> **v2.6.0** : le corps du 409 passe du texte brut au **JSON structuré** `{"error":"filtration_off","message":"…"}` — même format que les refus de `/ph/inject/start` (voir [tableau des codes](#codes-de-refus-409-v260)). Les routes de test `/pumpN/on` ne vérifient **que** la filtration (pas les limites volumétriques : pas de volume demandé), mais le temps de marche **consomme le budget horaire partagé** ([ADR-0020](adr/0020-budget-horaire-dosage-unique.md)).

---

### POST /pump2/on, /pump2/off — WRITE

Démarre ou arrête la pompe doseuse 2 (ORP/chlore) manuellement (test bench).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump2/on
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump2/off
```

> ⚠️ **Garde filtration (v2.1.2)** : mêmes règles que `/pump1/on` ci-dessus.
>
> **Paramètre optionnel `?duty=N` (v2.20.5)** : identique à `/pump1/on` — borné `[0, 255]`, défaut `255`, pas de plancher `MIN_ACTIVE_DUTY`, comportement inchangé sans le paramètre. Journal : `[Test] Pompe 2 démarrée en mode manuel (duty=N)`.

---

### POST /pump1/duty/:duty, /pump2/duty/:duty — WRITE

Règle la puissance PWM d'une pompe (0–255).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/pump1/duty/128
```

---

### POST /ph/inject/start — WRITE

Démarre une injection manuelle pH à la puissance configurée (`pump1_max_duty_pct` ou `pump2_max_duty_pct` selon `ph_pump`). La durée est calculée à partir du volume demandé et du débit nominal de la pompe.

> ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : le démarrage passe par les gardes de sécurité chimique de `evaluateManualInject()` ([`src/dosing_logic.h`](../src/dosing_logic.h)) — watchdog, filtration, stabilisation post-calibration, double démarrage, limite journalière **prédictive** (cumul + volume demandé), limite horaire (budget **partagé** avec la régulation auto, [ADR-0020](adr/0020-budget-horaire-dosage-unique.md)), cycles/jour et anti-rafale (ring partagé avec l'auto). Tout refus retourne **`409 Conflict`** avec un corps **JSON structuré** (voir [tableau des codes](#codes-de-refus-409-v260)). Le mode de régulation n'est volontairement **pas** vérifié (le manuel est disponible dans tous les modes), ni la mesure capteur (le manuel est aveugle à la mesure — assumé).
>
> Si la filtration s'arrête **pendant** l'injection, celle-ci est interrompue automatiquement (< 100 ms après détection) et une alerte MQTT `ph_injection_aborted` est publiée sur `{base}/alerts`. Pas de reprise automatique : l'utilisateur doit relancer manuellement après reprise filtration. `POST /ph/inject/stop` n'est **jamais** gardé (coupe-circuit opérateur).
>
> ✅ **Écrêtage automatique au reliquat journalier (v2.9.2)** : si le volume demandé (ou équivalent au `duration` legacy) **dépasse le reliquat** du quota journalier (`max_ph_ml_per_day − cumul`), la demande n'est plus refusée mais **écrêtée au reliquat** — durée recalculée arrondie **vers le bas** (le volume effectif ne re-déborde jamais la limite), log `info` `[Injection] pH : volume écrêté de X à Y mL (reliquat journalier)`, réponse `200 OK` normale. En fin naturelle, le crédit plancher (v2.9.1) porte le cumul **exactement à la limite** → latch `ph_limit_reached` + badge « Limite journalière atteinte ». Le refus `daily_limit` ne subsiste que si le reliquat est **nul ou < 1 s de pompe**. Cas double-clamp (reliquat > 10 min de pompe) : le plafond `kManualInjectMaxDurationS` gagne et le crédit retombe sur le volume effectif. Détail : [docs/subsystems/pump-controller.md](subsystems/pump-controller.md).

Paramètres (querystring, exclusifs) :

| Paramètre | Type | Validation |
|-----------|------|------------|
| `volume` | float (mL) | 1 – 2000 (mode préféré) |
| `duration` | integer (secondes) | fallback legacy, borné **1 – 600** (`kManualInjectMaxDurationS`, abaissé de 3600 → 600 en v2.1.2 — voir CHANGELOG) |

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/ph/inject/start?volume=15"
```

Réponses :
- `200 OK` — injection démarrée (corps texte `OK`).
- `400` — paramètre manquant ou hors plage (corps texte).
- `409 Conflict` — garde de sécurité chimique déclenchée. Corps JSON structuré (ci-dessous).

#### Codes de refus 409 (v2.6.0)

Format du corps (`application/json`) :

```json
{ "error": "daily_limit", "message": "Limite journalière atteinte — reste disponible aujourd'hui : 10 mL.", "remaining_ml": 10 }
```

| `error` | Déclencheur (évalué dans cet ordre) | Champ additionnel |
|---------|-------------------------------------|-------------------|
| `watchdog_inactive` | Watchdog inactif sur la tâche de dosage (`loopTask`) — règle absolue | — |
| `filtration_off` | Filtration arrêtée et `regulationMode != "continu"` | — |
| `stabilization_in_progress` | Stabilisation post-calibration en cours **pour la pompe visée** | `seconds_remaining` (int) |
| `already_injecting` | Une injection manuelle est déjà en cours sur cette pompe (double start) | — |
| `daily_limit` | Reliquat journalier **nul ou < 1 s de pompe** (sinon la demande est **écrêtée** au reliquat et acceptée — v2.9.2 ; frontière `==` **acceptée** ; limite ≤ 0 = illimité) | `remaining_ml` (int, reliquat arrondi **vers le bas** depuis v2.9.2 — le message ne promet jamais plus que disponible) |
| `hourly_limit` | `usedMs + durée demandée > *_limit_minutes × 60000` (budget **partagé** auto + manuel ; frontière `==` acceptée ; limite 0 = illimité) | — |
| `max_cycles` | Cycles/jour atteints (`maxCyclesPerDay = 20`, compteur partagé auto + manuel) | — |
| `burst_limit` | Anti-rafale : ≥ 6 démarrages/min OU ≥ 20 démarrages/15 min (ring partagé auto + manuel) | — |

Le volume/durée évalués sont ceux **réellement injectés** (post-plafonnement `kManualInjectMaxDurationS`), pas la demande brute. Chaque refus émet un log `critical` `[Sécurité] Injection pH|ORP manuelle refusée : …`. Détail des gardes : [docs/subsystems/pump-controller.md](subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006).

---

### POST /ph/inject/stop — WRITE

Arrête immédiatement l'injection manuelle pH en cours.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/ph/inject/stop
```

---

### POST /orp/inject/start — WRITE

Identique à `/ph/inject/start` pour la pompe ORP (`orp_pump`). Mêmes paramètres et contraintes.

> ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : mêmes gardes et même format de refus **`409` JSON** que `/ph/inject/start` — voir [tableau des codes](#codes-de-refus-409-v260). Limites appliquées côté ORP : `max_chlorine_ml_per_day` (journalière prédictive), `orp_limit_minutes` (horaire partagée avec l'auto), cycles/jour et anti-rafale (compteurs partagés), stabilisation post-cal ORP. Arrêt cyclique automatique si la filtration tombe en cours d'injection, alerte MQTT `orp_injection_aborted` publiée sur `{base}/alerts`. Bornage `duration` à 600 s (idem `/ph/inject/start`). **Écrêtage automatique au reliquat journalier (v2.9.2)** symétrique à la route pH (log `[Injection] ORP : volume écrêté de X à Y mL (reliquat journalier)`). `POST /orp/inject/stop` jamais gardé.

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

### POST /filtration/external-state — WRITE

Signale au contrôleur l'état réel d'une **filtration externe** (feature-056, v2.19.0). N'a d'effet utile qu'en mode d'installation `external` : il alimente la garde « présence d'eau » du dosage. Handler **non bloquant**, **aucune persistance** (au boot, état inconnu → dosage suspendu, fail-safe OFF).

Le paramètre `running` est accepté en **corps de formulaire** (`application/x-www-form-urlencoded`) OU en **query string** — valeurs acceptées : `true`/`false`/`1`/`0`/`on`/`off`. **Pas de corps JSON.**

```bash
# corps de formulaire
curl -u admin:monmotdepasse -X POST -d "running=true" \
  http://poolcontroller.local/filtration/external-state
# ou query string
curl -u admin:monmotdepasse -X POST \
  "http://poolcontroller.local/filtration/external-state?running=false"
```

**Réponse 200**

```json
{ "external_state": true }
```

**Erreurs**
- `400` : `{"error":"missing_param","message":"Paramètre 'running' requis (true/false)."}`
- `401` : token manquant / invalide

> Équivalent MQTT : publier `ON`/`OFF` sur `{base}/filtration_external_state/set` (voir [MQTT.md](MQTT.md)). Les deux canaux mettent à jour l'état de façon identique. Au-delà de `kExternalFiltrationStaleMs = 180 s` sans nouveau signal, l'état est considéré périmé → présence d'eau `false`. Voir [ADR-0026](adr/0026-mode-installation.md).

---

### POST /boost/start — WRITE

Active le **Mode Boost** (feature-053, v2.18.0) : surchloration temporaire du jour, auto-expirant au prochain minuit local. Tant qu'il est actif : filtration forcée en marche + cible ORP effective relevée (`min(orp_target + 60, 850)` mV) + limite journalière chlore effective relevée (`min(max_chlorine_ml_per_day × 1,5, 1000)` mL). **L'effet chlore ne s'applique qu'en mode de régulation ORP `automatic`** ; en Manuel / Programmé, seule la filtration s'étend. Aucun autre garde-fou n'est relâché. Voir [ADR-0025](adr/0025-mode-boost.md).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/boost/start
```

**Réponse 200** :

```json
{ "success": true, "boost_active": true, "boost_until": 1751846400, "filtration_extended": true, "chlorine_boosted": true }
```

Champs additifs v2.18.1 (feature-054) — indiquent quels leviers du Boost s'appliquent réellement selon la config :

| Champ | Type | Description |
|-------|------|-------------|
| `filtration_extended` | boolean | `true` si la filtration est gérée par PoolController (`filtration_enabled`) → le Boost prolonge effectivement la filtration. `false` si filtration non gérée (levier inerte : le forçage Boost n'est jamais atteint). |
| `chlorine_boosted` | boolean | `true` si `orp_regulation_mode == "automatic"` → le Boost relève effectivement la cible/limite chlore. `false` en Manuel / Programmé (levier inerte : le chlore n'est pas relevé). |

Quand un levier est inerte, `startBoost()` logue en plus un `warning` (couvre l'activation HTTP **et** MQTT/HA — voir [pump-controller.md §Mode Boost](subsystems/pump-controller.md#mode-boost-feature-053)). L'activation n'est jamais bloquée (on informe, on n'empêche pas).

**Erreur 409** — heure non synchronisée : sans horloge valide, l'expiration à minuit ne peut être calculée, l'activation est refusée (pas de boost « sans fin »).

```json
{ "error": "time_not_synced", "message": "Heure non synchronisée — Boost indisponible." }
```

---

### POST /boost/stop — WRITE

Désactive le Mode Boost immédiatement. Retour intégral au comportement normal (cible ORP, limite journalière et filtration selon la configuration). Inconditionnel (jamais refusé).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/boost/stop
```

**Réponse 200** :

```json
{ "success": true, "boost_active": false }
```

---

### POST /reset-sensor-filter — CRITICAL

Réarmement **manuel** du filtre de mesure d'un capteur, **hors des routes de debug** (v2.20.3). C'est la sortie de secours exposée à l'utilisateur quand un filtre reste bloqué en `ready = false` : dans ce cas il n'y a plus **aucune** régulation sur le paramètre concerné (et, côté pH, plus de régulation chlore non plus — le dosage ORP est fail-closed sur pH brut indisponible).

Le filtre visé est vidé (médiane + EMA), le latch EMI est levé et la **détection de sonde figée est effacée** : le filtre repart en warmup complet (≈ 13 mesures, ~65 s) pendant lequel le dosage automatique reste bloqué.

**Authentification** : `RouteProtection::CRITICAL` (administrateur)

**Paramètre** `sensor` (corps `form-urlencoded` **ou** query string) : `ph` ou `orp`. **Obligatoire** — pas de valeur par défaut : réparer le pH ne doit jamais sacrifier un filtre ORP sain.

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/reset-sensor-filter?sensor=ph"
```

**Réponse 200**

```json
{ "queued": true, "sensor": "ph" }
```

`queued` = **accepté**, pas exécuté : le handler n'écrit jamais dans le filtre (contexte `async_tcp`), il pose une demande consommée en tête de `SensorManager::update()` — reset effectif au prochain tour de boucle (< 100 ms), avec journalisation de l'état détruit.

**Erreurs**

- `400` : `{"error":"invalid_sensor"}` — paramètre `sensor` absent ou différent de `ph`/`orp`
- `401` / `403` : authentification CRITICAL manquante ou insuffisante
- `429` : `{"error":"too_soon","retry_after_s":<s>,"message":"…"}` — **un seul réarmement par capteur toutes les 10 minutes** (`kSensorFilterUiResetMinIntervalMs`). Le cooldown est par capteur : un `429` sur le pH n'empêche pas un réarmement ORP.
- `500` : `{"error":"queue_failed"}` — aucun filtre demandé (ne devrait pas survenir)

Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#réarmement-manuel-par-capteur-v2203) et [`docs/features/page-ph.md`](features/page-ph.md#bouton--réarmer-le-filtre-ph--v2203).

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

## Mises à jour OTA

### POST /update — CRITICAL

Met à jour le firmware ou le filesystem via multipart/form-data.

```bash
# Firmware
curl -u admin:monmotdepasse -X POST \
  -F "update_type=firmware" -F "update=@.pio/build/esp32dev_core3/firmware.bin" \
  http://poolcontroller.local/update

# Filesystem
curl -u admin:monmotdepasse -X POST \
  -F "update_type=filesystem" -F "update=@.pio/build/esp32dev_core3/littlefs.bin" \
  http://poolcontroller.local/update
```

> ⚠️ `update_type` doit être envoyé **avant** le fichier dans le multipart.

**Champ POST optionnel `sha256`** (v2.11.0, [ADR-0022](adr/0022-verification-integrite-ota.md)) : empreinte SHA-256 attendue de l'image, avec ou sans préfixe `sha256:`. Si elle est fournie et ne correspond pas à l'empreinte calculée du flux reçu (ou si elle est malformée), le flash est **refusé** (`Update.abort()`, corps `FAIL`, log `CRITICAL`). Sans ce champ, l'empreinte calculée est simplement **loggée** (`info`) — flash autorisé (upload manuel, utilisateur présent).

```bash
curl -u admin:monmotdepasse -X POST \
  -F "update_type=firmware" -F "sha256=sha256:<64 hex>" \
  -F "update=@.pio/build/esp32dev_core3/firmware.bin" \
  http://poolcontroller.local/update
```

<a id="reponses-post-update-v2201"></a>

#### Réponses (v2.20.1)

> ⚠️ **Changement de contrat** : jusqu'à la v2.20.0 incluse, **tout** échec était servi en `200 OK` avec un corps `FAIL`. Un client qui testait le code HTTP voyait donc un succès. Depuis la v2.20.1, le code HTTP porte le verdict.

| Cas | Statut | Corps |
|---|---|---|
| Succès | `200` | `OK` **exactement** — inchangé (le front et `ota_update.sh` en dépendent) |
| Upload concurrent | `409` | `Upload refusé: une mise à jour OTA est déjà en cours` — inchangé |
| Refus de type (octet magique) | `400` | `FAIL: <cause>` |
| `sha256` malformé, ou empreinte non conforme | `400` | `FAIL: <cause>` |
| Échec `begin` / écriture / finalisation | `500` | `FAIL: <cause> (<errorString>)` |

> ⚠️ **Le contrat machine est le code HTTP** — même règle que pour [`/check-update`](#garde-tls-sortante-psram-libre--heap-v2186). Le **texte** de la cause après `FAIL: ` est destiné à l'**utilisateur** (français, affiché tel quel dans l'interface) et **peut être reformulé sans préavis**. Ne jamais le parser : tester le statut. Le seul contenu de corps stable est le `OK` du succès.

**Refus de type — pré-contrôle de l'octet magique** (v2.20.1) : si `update_type=firmware` et que le **premier octet reçu** n'est pas `0xE9` (`ESP_IMAGE_HEADER_MAGIC`), l'image est refusée en `400` **avant tout effet de bord** — avant l'inhibition des pompes doseuses, avant le démontage de LittleFS, avant `Update.begin()`. Corps type :

```
FAIL: l'image reçue n'est pas un firmware ESP32 (octet magique 0xE1 au lieu de 0xE9) — type « Système de fichiers » attendu ?
```

L'ensemble des images **acceptées** est **inchangé** : ce contrôle est équivalent à celui qu'`UpdateClass::_writeBuffer()` appliquait déjà, il ne fait que le déplacer en amont pour produire une cause exploitable. Le cas **inverse** (image d'allure firmware déclarée `filesystem`) n'est **pas** refusé : il émet un `WARN` et poursuit — LittleFS n'a pas d'octet magique fiable à l'offset 0, un refus produirait des faux positifs. Voir [subsystems/ota-manager.md](subsystems/ota-manager.md#politique-de-refus--asymetrie-assumee-v2201).

**Une seule ligne de journal par session** (v2.20.1) : un échec d'upload produit **exactement une** entrée (`ERROR` ou `CRITICAL` pour un refus d'intégrité) nommant la cause réelle. Auparavant, chaque chunk restant re-journalisait la même erreur (~200 lignes) et vidait tout l'anneau de journal.

**Détection du type par nom de fichier** (repli quand `update_type` est absent) : reconnaît `littlefs.bin`, `spiffs.bin`, `filesystem.bin`, `fs.bin` ainsi que leurs variantes préfixées (`v2.20.1.littlefs.bin`, `pool-fs.bin`), insensible à la casse. Le nom **nu** `littlefs.bin` produit par `build_fs.sh` est reconnu depuis la v2.20.1 (l'ancienne règle exigeait un point devant). Tout autre `.bin` est présumé firmware. Logique pure [`src/ota_type_logic.h`](../src/ota_type_logic.h), 29 tests natifs (`test/test_native_ota_type/`).

L'ESP32 redémarre automatiquement après une mise à jour réussie — **uniquement** sur un `200`/`OK` réel (un refus de type ne déclenche aucun redémarrage).

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
  "filesystem_url": "https://github.com/.../littlefs.bin",
  "firmware_digest": "sha256:ab12…",
  "filesystem_digest": "sha256:cd34…"
}
```

Champs additifs v2.11.0 : `firmware_digest` / `filesystem_digest` — empreintes SHA-256 des assets (champ `digest` de l'API GitHub, format `sha256:<64 hex>`, chaîne vide si la release ne le fournit pas). L'UI les relaie en paramètre `digest=` de `/download-update` et **désactive le bouton Installer** si l'une des deux est vide (fail-closed).

La réponse contient aussi `release_name`, `published_at` et `release_notes` (corps de la release GitHub). Depuis la v2.10.4 ([ADR-0032](adr/0032-reponses-http-bornees.md)), `release_notes` est **tronqué à 2048 caractères** (`kOtaReleaseNotesMaxChars`) avec suffixe « … » : le corps d'une release n'est pas borné côté GitHub, la troncature borne la réponse HTTP construite en RAM. Depuis la v2.16.1, la coupe respecte les **frontières UTF-8** (un accent/emoji coupé en deux produisait un JSON invalide côté navigateur).

Depuis la v2.16.1 ([ADR-0032, extension 2026-08-04](adr/0032-reponses-http-bornees.md#extension-2026-08-04--check-update-parse-en-flux-entrant-v2161)), la réponse GitHub est parsée **en flux** (filtre limité aux champs consommés + budget mémoire `kOtaCheckJsonBudgetBytes` = 8192 o) au lieu d'être chargée entière en RAM. **Le contrat de la route est inchangé** (mêmes champs). Nouveau cas d'erreur :

| Code | Corps | Cas |
|---|---|---|
| `500` | `{"error":"GitHub response too large"}` | Corps de release GitHub dépassant le budget JSON (8 Ko) — refus propre, pas de consommation mémoire non bornée |

**Diagnostics (v2.16.2)** : chaque appel logge l'état heap **avant** l'ouverture de la session TLS ; un échec côté client (code HTTP négatif) logge le détail mbedtls (`lastError`, message + code) et le libellé `errorToString` au lieu d'un « -1 » muet. Depuis la v2.18.5, le log INFO nominal inclut aussi l'**IP du client** (attribution des appels TLS — le crash OOM du 11/08 avait un profil TLS compatible mais l'appelant était invérifiable) ; depuis la v2.18.6 il inclut la **PSRAM libre**, qui conditionne l'autorisation :

```
INFO  Vérification des mises à jour GitHub... (PSRAM libre=… o, heap libre=… o, plus gros bloc=… o, client=192.168.1.15)
```

<a id="garde-tls-sortante-psram-libre--heap-v2186"></a>

#### Garde TLS sortante — PSRAM libre + heap (v2.18.6, [ADR-0036](adr/0036-psram-requise-mise-a-jour-en-ligne.md))

Avant la création de `WiFiClientSecure`, la route évalue le prédicat pur partagé `oomTlsGateReason()` ([subsystems/oom-guard.md](subsystems/oom-guard.md)), qui retourne un **motif** (il remplace le booléen `oomTlsGateOk` de la v2.18.5). Deux critères, évalués **dans cet ordre** :

1. **PSRAM libre** (`ESP.getFreePsram()`) < `kOtaTlsMinPsramBytes` (1 048 576 o = 1 Mo) → refus. Le critère porte sur la PSRAM **LIBRE**, pas sur la taille installée : une PSRAM saturée ferait retomber mbedTLS sur la RAM interne, exactement le régime de crash.
2. Sinon, bicritère heap **inchangé** : heap libre < `kCheckUpdateMinFreeHeapBytes` (65 536 o) **OU** plus gros bloc contigu < `kCheckUpdateMinMaxAllocBytes` (20 480 o) → refus **temporaire**.

**Trois refus possibles** (v2.18.7), tous en `503 application/json` et produits par le **helper partagé** `refuseIfTlsGateClosed` — donc **identiques au caractère près sur `/check-update` et `/download-update`** :

| `code` | Condition | Nature | `error` (texte utilisateur) | Effet interface |
|---|---|---|---|---|
| `psram_required` | PSRAM **absente** (`ESP.getFreePsram() == 0`) | **Permanent** (matériel) | « La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à jour manuelle. » | Bouton **neutralisé** + callout permanent dans la carte GitHub ; repli : [upload de fichier](#post-update--critical) ou `deploy.sh ota-firmware` |
| `psram_low` | PSRAM présente mais **libre < 1 Mo** | **Temporaire**, réessayable | « La mise à jour en ligne est momentanément indisponible : la mémoire PSRAM est saturée. Réessayez dans quelques minutes ou utilisez la mise à jour manuelle. » | Toast **informatif**, bouton **non** neutralisé, rafraîchissement de l'état via `/get-system-info` |
| `tls_low_memory` | heap libre < `kCheckUpdateMinFreeHeapBytes` **OU** maxBloc < `kCheckUpdateMinMaxAllocBytes` (motif `kOomTlsGateLowMemory`) | **Temporaire**, réessayable | « Mémoire insuffisante pour TLS, réessayez » (texte inchangé depuis la v2.18.5) | Toast **informatif**, bouton **non** neutralisé, rafraîchissement via `/get-system-info` |

La discrimination `psram_required` / `psram_low` est faite **par la route** (simple test de nullité de la PSRAM libre déjà mesurée, aucun seuil dupliqué) : le prédicat pur `oomTlsGateReason()` regroupe les deux sous le motif `kOomTlsGateNoPsram` ([subsystems/oom-guard.md](subsystems/oom-guard.md#garde-tls-sortante-partagée--oomtlsgatereason-v2186)).

> ⚠️ **Le champ `code` est le seul contrat machine** : il est présent sur les **trois** refus (depuis la v2.18.7 — auparavant seul le refus PSRAM en portait un) et c'est le discriminant stable pour un client. Le champ `error` est un **texte destiné à l'utilisateur** et **peut être reformulé sans préavis ni changement de version majeure** — il l'a été en v2.18.7 (formulation neutre, vocabulaire aligné sur la carte « Mise à jour manuelle » de l'interface). Ne jamais tester le contenu de `error` : tester `code`. Un client doit traiter `psram_required` comme **définitif** et `psram_low` / `tls_low_memory` comme **réessayables**. Le statut `503` et la valeur `psram_required` sont, eux, inchangés depuis la v2.18.6.

Les **logs** du firmware (WARN de refus, INFO nominal) restent volontairement techniques : ils sont destinés au diagnostic, pas à l'utilisateur, et ne suivent donc pas les reformulations du champ `error`.

Chaque refus émet un **WARN** auto-explicatif : motif, PSRAM libre, les deux mesures heap, leurs trois seuils et l'**IP du client** :

```
WARN  /check-update refusé (PSRAM libre absente/insuffisante) : PSRAM libre=0 o (min 1048576), heap libre=87708 o (min 65536), plus gros bloc=34804 o (min 20480), client=192.168.1.15
```

> ⚠️ **Mise à jour en ligne indisponible sans PSRAM** (rectifie la note « limitation core Arduino 3 » des versions antérieures — l'attribution correcte, établie par la mesure terrain du 2026-08-12, est l'**absence de PSRAM**, pas le core Arduino) : sur un module **WROOM** (sans PSRAM), la session TLS GitHub réclame plusieurs blocs contigus d'environ 16 Ko que la RAM interne ne fournit plus de façon fiable après quelques minutes de fonctionnement. Le handshake échouait **après** avoir affamé le tas (allocation de 100 o refusée alors que 87 Ko restaient libres = fenêtre du crash `bad_alloc` du 11/08). Depuis la v2.18.6, `/check-update` **et** `/download-update` répondent donc `503 psram_required` **immédiatement**, sans ouvrir de session. Le module du BOM PCB v2 (**ESP32-DevKitC-VE / WROVER-E, 8 Mo de PSRAM**) place ces allocations en PSRAM.

> **Note seuils** : `kOtaTlsMinPsramBytes` = 1 Mo n'a **jamais été mesuré sur WROVER réel** (valeur volontairement large, la session TLS y consomme surtout de la PSRAM) — à confirmer à la mise en service du premier module PSRAM. Les seuils heap restent dimensionnés pour la RAM interne d'un WROOM, réserve du parachute OOM déduite ([ADR-0035](adr/0035-parachute-oom-new-handler.md)).

---

### POST /download-update — CRITICAL

Télécharge et installe une mise à jour depuis une URL GitHub, avec vérification d'intégrité SHA-256 **obligatoire** (v2.11.0, [ADR-0022](adr/0022-verification-integrite-ota.md)).

```bash
curl -u admin:monmotdepasse -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "url=https://github.com/.../firmware.bin&digest=sha256:<64 hex>&restart=true" \
  http://poolcontroller.local/download-update
```

> Seuls les hôtes `github.com`, `api.github.com` et `objects.githubusercontent.com` sont autorisés.

Paramètres POST :

| Paramètre | Requis | Description |
|---|---|---|
| `url` | oui | URL de l'asset (`firmware.bin` ou `littlefs.bin`, hôtes whitelistés) |
| `digest` | **oui** (v2.11.0) | Empreinte SHA-256 attendue, format `sha256:<64 hex>` (relayée depuis `/check-update`) |
| `restart` | non | `false` pour ne pas redémarrer après une installation filesystem réussie |

Erreurs d'intégrité (fail-closed) :

| Code | `error` | Cas |
|---|---|---|
| `400` | `integrity_digest_missing` | Paramètre `digest` absent — refus **avant** toute connexion |
| `400` | `integrity_digest_invalid` | Empreinte malformée (format attendu `sha256:<64 hex>`) |
| `422` | `integrity_mismatch` | Empreinte calculée du flux ≠ empreinte attendue (image corrompue ou tronquée, coupure WiFi incluse). Corps : `{"error","expected","computed","message"}`. **Firmware** : `Update.abort()` avant activation — version courante conservée. **Filesystem** : pas de redémarrage, partition FS à réinstaller (firmware intact). |

Chaque refus émet un log `CRITICAL` avec empreintes attendue/calculée.

**Garde TLS sortante (v2.18.6)** — cette route ouvre la **même** session TLS que `/check-update` et n'avait, jusqu'à la v2.18.5, **aucune garde mémoire** : un `POST` direct contournait celle de `/check-update`, avec un flash à la clé. Elle applique désormais le **même prédicat** `oomTlsGateReason()` ([détail des critères et des corps de réponse](#garde-tls-sortante-psram-libre--heap-v2186), [ADR-0036](adr/0036-psram-requise-mise-a-jour-en-ligne.md)) :

| Code | `code` | Cas |
|---|---|---|
| `503` | `psram_required` | Module **sans** PSRAM — indisponibilité **permanente**, utiliser la **mise à jour manuelle** |
| `503` | `psram_low` | PSRAM présente mais **saturée** (< 1 Mo libre) — **réessayable** |
| `503` | `tls_low_memory` | Heap interne momentanément dégradé — **réessayable** |

Les corps des **trois** refus sont produits par le **même helper** (`refuseIfTlsGateClosed`) que ceux de `/check-update` : les deux routes renvoient donc, au caractère près, les mêmes textes et les mêmes valeurs de `code` — voir le [tableau détaillé plus haut](#garde-tls-sortante-psram-libre--heap-v2186). Là encore, seul `code` fait contrat ; le champ `error` est un texte utilisateur reformulable.

**Position de la garde** (importante) : après la validation `url` / allowlist / `digest` (une requête malformée reste un `400`/`403`) et **avant** `WiFiClientSecure`, `setOtaInProgress()`, `LittleFS.end()` et tout effacement de partition — un refus n'a donc **aucun effet de bord** : pompes non gelées, filesystem non démonté, aucune partition touchée. Le WARN de refus est identique à celui de `/check-update`, préfixé `/download-update`.

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

### `POST /debug/sensor_filter_reset?sensor=ph|orp` — CRITICAL (⚠️ **changement en v2.20.3**)

> ⚠️ **Cette route n'est plus ouverte.** Jusqu'en v2.20.2 elle était accessible **sans authentification**, comme les autres `/debug/*`. Elle effectue exactement la même action destructrice que [`POST /reset-sensor-filter`](#post-reset-sensor-filter--critical) (effacement de la détection de sonde figée et du latch EMI) et contournait donc toutes les gardes posées sur celle-ci : elle exige désormais `RouteProtection::CRITICAL`. Les routes `/debug/*` voisines (`ph_slope_refresh`, `sensor_filter_state`, `sensor_trace`, `sensor_trace_clear`) restent **sans auth**.

Réinitialise un filtre (médiane longue + EMA) et **lève le latch EMI**. Le filtre repasse en **warmup** → le dosage automatique est bloqué jusqu'à ce que la **fenêtre centrale** soit de nouveau pleine (`kSensorFilterCentralWindow` = 13 nouvelles mesures valides, ≈ 65 s à 5 s/cycle) avant de produire une valeur exploitable. Utile après un débranchement EZO, un test, ou une recalibration manuelle hors workflow.

**Paramètre** (corps `form-urlencoded` **ou** query string) :

| `sensor` | Effet |
|---|---|
| `ph` | filtre **pH seul** (v2.20.3) |
| `orp` | filtre **ORP seul** (v2.20.3) |
| *absent* | **les deux** filtres — comportement historique, réponse inchangée |
| autre valeur | `400 {"error":"invalid_sensor"}` |

La sélectivité est un correctif : lors du déblocage en production du 2026-08-16, cette route a sacrifié le filtre ORP qui était parfaitement sain.

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/debug/sensor_filter_reset?sensor=ph"
```

**Réponse 200**

```json
{ "success": true, "queued": true, "reset": ["ph"] }
```

**Sémantique `queued`** : la réponse signifie **acceptée**, pas **exécutée**. Le handler ne touche jamais le filtre (contexte `async_tcp`) : il pose un drapeau consommé en tête de `SensorManager::update()`, donc le reset a lieu au **prochain tour de boucle** (< 100 ms). L'état détruit (médiane avant/après, rejets, latch, sonde figée) est journalisé à ce moment-là, en `warning` — ou en `critical` si une détection de sonde figée vient d'être effacée.

**Erreurs** : `400` (`sensor` invalide) · `401`/`403` (auth CRITICAL manquante).

### `GET /debug/sensor_filter_state` — pas d'auth (cohérent avec autres `/debug/*`)

Retourne l'état brut des deux filtres pour diagnostic. Lecture lock-free des getters `SensorManager` (valeurs scalaires).

```bash
curl http://poolcontroller.local/debug/sensor_filter_state
```

**Réponse 200**

```json
{
  "ph":  { "raw": 7.241, "median": 7.236, "filtered": 7.234, "ready": true, "unstable": false, "rejected": 0,
           "dispersion": 0.043, "dispersion_max": 0.5, "latch_cycles": 3, "clean_streak": 12 },
  "orp": { "raw": 722, "median": 720, "filtered": 720, "ready": true, "unstable": false, "rejected": 0,
           "dispersion": 11.0, "dispersion_max": 100.0, "latch_cycles": 3, "clean_streak": 12 }
}
```

Les champs `raw` / `median` / `filtered` valent `null` si la mesure est indisponible. Voir [feature-025](../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#reset-manuel--diagnostic).

#### Marge d'instabilité (v2.20.6)

| Champ | Type | Description |
|---|---|---|
| `dispersion` | float \| null | **Amplitude (`max − min`) de la fenêtre médiane PLEINE** — la grandeur qui **décide** du drapeau `unstable`, donc de l'inhibition du dosage automatique. `null` si la fenêtre n'est pas pleine (warmup, ou **porte d'alimentation fermée**). Arrondi à 3 décimales (pH) / 1 décimale (mV). |
| `dispersion_max` | float | Seuil de dépassement : `kPhFilterMaxDispersion` = **0.5** pH, `kOrpFilterMaxDispersion` = **100** mV. |
| `latch_cycles` | integer | Nombre de fenêtres pleines **consécutives** en dépassement avant pose du **latch EMI** (`kSensorFilterDispersionLatchCycles` = **3**, ≈ 15 s). |
| `clean_streak` | integer | Fenêtres pleines **et propres** consécutives — progression vers l'**auto-levée** du latch (**12** nécessaires, `kSensorFilterLatchClearCycles`). |

**Pourquoi ces champs existent** : `dispersion` pilotait l'inhibition du dosage sans être exposée par **aucune** route — ni interface, ni API. Pour évaluer la marge au seuil pendant une campagne de mesure, il fallait la **reconstruire de l'extérieur** par un minorant (crête-à-crête sur des relevés espacés de 30 s, alors que le critère porte sur 13 mesures à 5 s), avec une incertitude qu'aucune analyse ne pouvait lever. `dispersion_max` et `latch_cycles` sont publiés **à côté** de la valeur pour que la marge se lise sans connaître les constantes du firmware : `dispersion / dispersion_max` donne directement le taux d'occupation du seuil.

> ⚠️ **`dispersion: null` avec la filtration arrêtée n'est pas un défaut.** La **porte d'alimentation** (v2.20.3, [ADR-0041](adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)) gèle l'ingestion quand les mesures ne sont pas représentatives (log `Filtres pH/ORP : alimentation GELÉE — mesures non représentatives (circulation arrêtée)`). Fenêtre vide ⇒ pas de dispersion ⇒ pas de jugement d'instabilité.
>
> **Corollaire à ne pas confondre** : filtration arrêtée, `unstable: false` ne signifie **pas** « la mesure est stable ». Il signifie « **aucun jugement rendu**, faute de données ». Prendre le premier pour le second fait conclure à tort qu'une configuration est saine — c'est un piège de lecture réel, rencontré en analyse. Pour juger de la stabilité, n'exploiter cette route que **circulation en marche et `dispersion` non nulle**.

**Échelle** : `dispersion` est une **amplitude** calculée sur le buffer interne en échelle **brute**. Une amplitude est de toute façon **insensible à l'offset pH** (constante additive : `max(x+c) − min(x+c) = max(x) − min(x)`), contrairement à `median` et `filtered` de la même route qui sont **corrigés** ([ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Aucune correction à appliquer avant comparaison à `dispersion_max`.

## Trace des signaux capteurs (feature-057, v2.7.0)

Réintroduction **fidèle** (haute cadence) de la trace des signaux capteurs retirée en v2.5.0 (feature-045), **étendue à l'ORP** (l'original ne couvrait que le pH). Un ring buffer firmware de `kSensorTraceBufferSize = 300` échantillons enregistre les valeurs brutes **et** lissées à **chaque cycle capteur** (~5 s), cadence bien supérieure aux 5 s du WebSocket sur toute la fenêtre. Buffer **volatil** (perdu au reboot). Détail firmware : [`docs/subsystems/sensors.md`](subsystems/sensors.md#trace-des-signaux-capteurs--feature-057).

### `GET /debug/sensor_trace` — pas d'auth (cohérent avec autres `/debug/*`)

Dump JSON **paginé** du ring buffer, du plus ancien au plus récent. Handler **non bloquant** (< 50 ms) : lecture lock-free du buffer, réponse pré-dimensionnée (`beginResponseStream(type, taille)` — une seule allocation exacte, aucun `resize()` en cours de sérialisation), aucune allocation dans la loop.

> ⚠️ **Pagination obligatoire depuis la v2.10.1** (bug-crash-sensor-trace-badalloc) : la route sérialisait auparavant les 300 échantillons en une seule réponse (~20 Ko contigus dans le `cbuf` d'`AsyncResponseStream`) — sur heap fragmenté (cf. [ADR-0029](adr/0029-auto-guerison-heap.md), le plus gros bloc libre peut descendre sous 8 Ko), l'allocation levait un `std::bad_alloc` non rattrapé sur la tâche `async_tcp` → `abort()` → **reboot** (coredump v2.10.0 du 2026-07-23, déclenché par l'ouverture de l'onglet Avancé). Chaque page est désormais bornée à `kSensorTraceMaxSamplesPerPage = 50` échantillons (~3,5 Ko).

```bash
curl "http://poolcontroller.local/debug/sensor_trace?offset=0&count=50"
```

**Paramètres de query** (optionnels — sans paramètre, la route retourne la **première page**) :

| Paramètre | Défaut | Description |
|-----------|--------|-------------|
| `offset` | `0` | Index du premier échantillon (chronologique, 0 = le plus ancien). |
| `count` | `kSensorTraceMaxSamplesPerPage` (50) | Nombre d'échantillons demandés. `0` ou valeur invalide → défaut. |

**Clamp silencieux** (jamais de `400`, calcul pur `computeTracePage()` dans [`src/sensor_trace_page.h`](../src/sensor_trace_page.h)) :
- `offset > count total` → page vide (`returned: 0`) ;
- `count > 50` → borné à 50 ;
- `offset + count` déborde → tronqué aux échantillons restants. Invariants garantis quel que soit l'input : `offset ≤ total`, `returned ≤ 50`, `offset + returned ≤ total`.

**Réponse 200**

```json
{
  "count": 300,
  "offset": 0,
  "returned": 50,
  "interval_ms": 5000,
  "now": 3600000,
  "samples": [
    { "t": 3595000, "phRaw": 7.241, "phCorrected": 7.191, "phFiltered": 7.234, "orpRaw": 722.0, "orpFiltered": 720.0, "tempEau": 24.5 },
    { "t": 3600000, "phRaw": 7.238, "phCorrected": 7.188, "phFiltered": 7.235, "orpRaw": 721.0, "orpFiltered": 720.0, "tempEau": 24.5 }
  ]
}
```

| Champ | Type | Description |
|-------|------|-------------|
| `count` | integer | Nombre **total** d'échantillons présents dans le buffer (0..300) — permet au client d'enchaîner les pages. |
| `offset` | integer | Offset **effectif** de la page (après clamp). |
| `returned` | integer | Nombre d'échantillons dans `samples` (0..50). |
| `interval_ms` | integer | Période nominale d'enregistrement (`kPhOrpSensorIntervalMs`, ~5000 ms). |
| `now` | integer | `millis()` courant — permet à l'UI de reconstruire l'axe temps relatif (`t − now`). |
| `samples[].t` | integer | Timestamp de l'échantillon en ms (`millis()` au moment de l'enregistrement). |
| `samples[].phRaw` / `phFiltered` | float \| null | pH brut / lissé, **tous deux SANS offset** (mesure capteur telle quelle, arrondi ×1000). `null` si NaN (lecture échouée / filtre non amorcé). |
| `samples[].phCorrected` | float \| null | **feature-067 (v2.20.0)** — pH brut **+ offset logiciel** (`ph_offset`, arrondi ×1000). `null` si le brut est NaN (l'offset ne fabrique jamais une mesure). À offset nul, égal à `phRaw`. |
| `samples[].orpRaw` / `orpFiltered` | float \| null | ORP brut / lissé en mV (arrondi ×10). `null` si NaN. |
| `samples[].tempEau` | float \| null | T° eau envoyée à l'EZO pour compensation (°C, arrondi ×10). `null` si sonde absente. |

> ⚠️ **Exception à la règle « seuls les `*Raw` sont bruts »** : dans cette trace de **diagnostic**, `phFiltered` est la sortie du filtre **sans offset** (le filtre travaille sur le brut, [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Seul `phCorrected` porte l'offset. L'objectif est de pouvoir dire à tout instant **ce qui vient de l'EZO** et **ce qui vient de la correction**. Ailleurs (`/data`, WebSocket, MQTT), `phFiltered` est **corrigé**. Coût mémoire : le champ ajouté porte l'échantillon à 28 octets (buffer ≈ 8,4 Ko RAM) et une page de 50 échantillons à ≈ 4,5 Ko de JSON.

**Erreurs**

- `503` `{"error":"heap fragmenté, réessayez"}` — garde-fou défensif : si la taille mesurée de la page (`measureJson`) + marge (`kSensorTraceHeapMarginBytes = 4096`) dépasse le plus gros bloc libre (`ESP.getMaxAllocHeap()`), la route répond proprement au lieu de crasher. Réessayer plus tard (l'auto-guérison heap, ADR-0029, finit par restaurer la situation).

> ℹ️ **Glissement du ring entre deux pages** : le buffer continue d'avancer (~1 échantillon toutes les 5 s) pendant qu'un client enchaîne les pages — les bornes ne sont pas ancrées sur un identifiant d'échantillon, un décalage d'~1 échantillon entre pages est donc possible et **toléré** (route de diagnostic, contrat lock-free `SensorTraceSample` inchangé).

Usage : diagnostiquer le **bruit** ou une **oscillation** de la chaîne de mesure en comparant brut vs lissé sur une fenêtre continue, depuis l'onglet **Avancé** — l'UI agrège les pages de façon transparente (voir [`docs/features/page-settings.md`](features/page-settings.md#card-signaux-capteurs-brut--lissé-feature-057)).

### `POST /debug/sensor_trace_clear` — pas d'auth (cohérent avec autres `/debug/*`)

Vide le ring buffer (démarre une fenêtre d'observation propre).

```bash
curl -X POST http://poolcontroller.local/debug/sensor_trace_clear
```

**Réponse 200**

```json
{ "success": true }
```

---

## Routes de debug supprimées (v2.5.0, feature-045)

Les routes suivantes, ajoutées pour la campagne de diagnostic d'oscillation pH (2026-05/06), ont été retirées et répondent désormais **404** :

- `GET /debug/ph_trace` et `POST /debug/ph_trace_clear` (trace ring buffer pH) — **réintroduites en v2.7.0** sous `GET /debug/sensor_trace` + `POST /debug/sensor_trace_clear` (pH **et** ORP ; voir section ci-dessus).
- `GET /debug/wifi_pause` et `POST /debug/wifi_pause` (pause WiFi temporaire) — **non réintroduites**.
- `POST /debug/ezo_command` et `POST /debug/ezo_factory` (diagnostic Atlas EZO) — **non réintroduites**.

Le code est récupérable via `git revert` du commit de la feature-045 ; l'état complet avec ces outils est figé au tag `v2.4.0`. Voir [feature-045](../specs/features/done/feature-045-retrait-outils-debug.md).
