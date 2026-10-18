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
| 409  | Conflit d'état — typiquement injection manuelle refusée car filtration arrêtée (sécurité chimique, voir [`POST /ph/inject/start`](#post-phinjectstart--write)), ou **réactivation de la régulation pH sans acquittement** de la bascule de sécurité (**v2.25.0**, voir [`POST /save-config`](#post-save-config--critical)). |
| 429  | Trop de requêtes (rate limiting) |
| 500  | Erreur serveur |
| 503  | **Mémoire insuffisante** — réponse bornée refusée faute de tas contigu. Deux familles : réponses JSON volumineuses ([`GET /get-logs`](#get-get-logs--write), [`GET /debug/sensor_trace`](#get-debugsensor_trace--pas-dauth-cohérent-avec-autres-debug), [ADR-0032](adr/0032-reponses-http-bornees.md)) et, **depuis la v2.41.0**, **tout GET servant un fichier statique ou une page HTML** (voir ci-dessous). |

---

## 503 sur les fichiers statiques et les pages HTML (v2.41.0, [ADR-0067](adr/0067-garde-503-avant-fopen-handler-prioritaire.md))

**Tout `GET` servant un fichier statique ou une page HTML peut répondre `503`** quand le plus gros bloc libre du tas 8 bits passe sous **8 192 octets** (`kStaticFileMinAllocBytes`).

```http
HTTP/1.1 503 Service Unavailable
Content-Type: text/plain
Retry-After: 5

Memoire insuffisante, reessayez
```

Ce n'est pas une dégradation de confort : ouvrir un fichier demande à la libc un verrou interne dont l'échec d'allocation provoque un **`abort()` sans code de retour à tester** (redémarrage de l'appareil). Le `503` remplace un crash.

**Routes concernées** — les 7 pages HTML (`/`, `/index.html`, `/login.html`, `/wifi.html`, `/wizard.html`, `/config`, `/config.html`) et **tous** les assets servis par `serveStatic` (`.css`, `.js`, images, polices).

**Ce qui reste servi pendant la famine** — c'est cette liste qui permet de diagnostiquer l'appareil quand son interface est refusée :

| Toujours servi | Détail |
|---|---|
| Diagnostic / lecture | [`/data`](#get-data--write), [`/get-config`](#get-get-config--write), [`/get-logs`](#get-get-logs--write), [`/download-logs`](#get-download-logs--write), [`/get-history`](#get-get-history--write), [`/get-kpi`](#get-get-kpi--write), [`/get-system-info`](#get-get-system-info--write), [`/time-now`](#get-time-now--write) |
| Authentification | [`/auth/status`](#get-authstatus--public), [`/auth/token`](#get-authtoken--critical), [`/auth/ap-password`](#get-authap-password--critical) |
| Réseau | [`/wifi/status`](#get-wifistatus--public), [`/wifi/scan`](#get-wifiscan--public-mode-ap--write-mode-sta) |
| Coredump | [`/coredump/info`](#get-coredumpinfo--write), [`/coredump/download`](#get-coredumpdownload--write) |
| OTA | [`/check-update`](#get-check-update--write) et **`POST /update`** (méthode POST → hors de la garde) |
| Debug | [`/debug/sensor_filter_state`](#get-debugsensor_filter_state--pas-dauth-cohérent-avec-autres-debug), [`/debug/sensor_trace`](#get-debugsensor_trace--pas-dauth-cohérent-avec-autres-debug) |
| Temps réel | **WebSocket [`/ws`](#ws-ws--write)** et **MQTT** — inchangés |
| Actions | **toutes les routes `POST`**, dont le contrôle des pompes, de la filtration et de l'éclairage |

⚠️ **Conséquences à connaître** : en famine, `/wifi.html` et `/` étant refusés, le **portail AP de secours** et la **page OTA** ne sont plus atteignables **par le navigateur**. Les chemins de secours réels restent ouverts : `POST /update`, `./deploy.sh ota-firmware`, et les routes de diagnostic ci-dessus.

**Côté intégrateur** : traiter `503` sur ces routes comme **temporaire** et respecter `Retry-After` (5 s). Ne pas relancer en boucle serrée — c'est précisément le martèlement que l'en-tête existe pour éviter pendant que la mémoire se résorbe.

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
  "phSlopeAcid": 101.0,
  "phSlopeBase": 100.0,
  "phSlopeZero": -1.03,
  "phSlopeAgeMs": 42000,
  "phRaw": 7.241,
  "phMedian": 7.236,
  "phFiltered": 7.234,
  "phValueRaw": 7.241,
  "phRejectedCount": 0,
  "orpRaw": 722,
  "orpMedian": 720,
  "orpFiltered": 720,
  "orpRejectedCount": 0,
  "tempFrozenAlarm": false,
  "filtration_running": true,
  "filtration_elapsed_s": 1800,
  "water_present": true,
  "measures_valid": true,
  "measures_stab_elapsed_s": 0,
  "measures_purge_last_duration_s": 2820,
  "measures_purge_last_end": 1,
  "measures_dose_available_in_s": 0,
  "ph_dosing": false,
  "orp_dosing": false,
  "ph_pump_flow_uncharacterized": false,
  "orp_pump_flow_uncharacterized": true,
  "ph_pump_amplitude_duty": 127,
  "orp_pump_amplitude_duty": 127,
  "pulse_period_ms": 60000,
  "ph_pulse_session": false,
  "orp_pulse_session": false,
  "ph_pulse_ms": 0,
  "orp_pulse_ms": 0,
  "ph_pulse_phase_ms": 0,
  "orp_pulse_phase_ms": 0,
  "ph_integral": 7450.2,
  "ph_integral_term": 2.759,
  "ph_integral_pct": 46.0,
  "ph_daily_ml": 45.2,
  "orp_daily_ml": 120.5,
  "ph_limit_reached": false,
  "orp_limit_reached": false,
  "flow_assist": {
    "active": false,
    "pump": 0,
    "phase": "",
    "remaining_s": 0,
    "aborted": false,
    "purges_left": 3,
    "purge_s": 60,
    "measure_s": 120,
    "flow255": [74.0, 74.0]
  },
  "ph_tracking_enabled": true,
  "ph_container_ml": 5000,
  "ph_remaining_ml": 1500,
  "ph_alert_threshold_ml": 500,
  "orp_tracking_enabled": true,
  "orp_container_ml": 5000,
  "orp_remaining_ml": 3200,
  "orp_alert_threshold_ml": 1000
}
```

> **`ph_container_ml` / `orp_container_ml` ajoutés à `/data` en v2.59.0 (feature-143).**
> Ces deux clés existaient déjà dans `GET /get-config` et dans la trame WebSocket
> `config` — elles manquaient seulement à `/data`. C'était la **4ᵉ récidive** du motif
> de parité `/data` ↔ trame WS documenté plus haut : la sentinelle de parité
> (`test/test_native_ws_data_parity/`) les déclarait comme **exception dual-sourcée**
> légitime (« RÉGLAGE, servi par `/get-config` »), un raisonnement juste sur le papier
> et **faux en pratique**, parce que l'interface recharge la carte Produits à **chaque**
> charge `/data` (toutes les 30 s), pas seulement au chargement de la configuration.
> L'exception a été **retirée** de la sentinelle plutôt que contournée — c'est le
> mécanisme de détection qui a permis de la repérer, pas un oubli de garde. Voir
> [docs/subsystems/ws-manager.md](subsystems/ws-manager.md#le-second-axe-de-parité--data--sensor_data-parite-data)
> et [docs/features/page-dosages.md](features/page-dosages.md).

> **v2.21.0 ([ADR-0043](adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) — état de caractérisation des pompes**, 4 champs ajoutés (par **chimie** — le firmware résout l'affectation `ph_pump`/`orp_pump`) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `ph_pump_flow_uncharacterized` | boolean | `true` = la pompe affectée au pH ne peut pas doser en connaissance de débit (jamais pesée, débit implausible, amplitude ≠ duty calibré, ou amplitude sous le plancher de rotation) → **dosage automatique/programmé refusé**, badge UI « Débit non caractérisé — pesée requise ». |
> | `orp_pump_flow_uncharacterized` | boolean | Idem pour la pompe affectée à l'ORP/chlore. |
> | `ph_pump_amplitude_duty` | integer | Amplitude courante du plateau d'impulsion (duty commandé pendant un pouls) de la pompe pH : `pumpN_max_duty_pct × 255/100`. |
> | `orp_pump_amplitude_duty` | integer | Idem pompe ORP. |
>
> **v2.21.1 ([ADR-0045](adr/0045-etat-impulsion-expose-en-calendrier.md)) — calendrier d'impulsion**, 7 champs ajoutés, **identiques dans le push WS `sensor_data`** (5 s — mêmes noms, mêmes valeurs). Ils distinguent la **session de dosage ouverte** (commandé — ce que disent déjà `ph/orp_dosing`) de la **pompe alimentée maintenant** (effectif — pouls en cours) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `pulse_period_ms` | uint32 | Période fixe du train d'impulsions (`kPulsePeriodMs` = 60000 ms), publiée pour que le client ne la code pas en dur. |
> | `ph_pulse_session` / `orp_pulse_session` | boolean | Séance de dosage ouverte (auto **ou** scheduled) pour la chimie. |
> | `ph_pulse_ms` / `orp_pulse_ms` | uint32 | Durée d'impulsion **planifiée** (ms) pour la période courante. **`0` = deadband** (période sans pouls : demande sous le pouls minimal `kPulseMinMs` — **10 s depuis la v2.25.3**, 5 s avant). Depuis la v2.22.0 la demande n'est plus refusée mais **accumulée** ([ADR-0046](adr/0046-accumulateur-demande-sous-pouls-minimal.md)) : la séance reste ouverte, la pompe ne tourne simplement pas cette période. |
> | `ph_pulse_phase_ms` / `orp_pulse_phase_ms` | uint32 | Position **relative** dans la période, clampée `[0..pulse_period_ms]` — jamais un `millis()` absolu (le client l'ancre sur sa propre horloge). `0` si session inactive. **Pompe alimentée ⇔ `phase < pulse_ms`.** |
>
> Getters `PumpController` lecture seule, fail-closed sur index invalide — aucun chemin de dosage touché. Buffer `/data` porté de 1536 à **1792** octets. Consommateur : statut dosage des cartes pH/ORP du tableau de bord ([page-dashboard.md](features/page-dashboard.md#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211)).
>
> **v2.21.2 (bug-slope-boot-perdue) — pente sonde pH, parité de transport** : les 4 champs `phSlopeAcid` / `phSlopeBase` / `phSlopeZero` / `phSlopeAgeMs` sont **ajoutés à `/data`**. La trame WS `sensor_data` les portait **déjà** depuis feature-024 — c'est `/data` qui rattrape : un client sans WS (poll seul) ne pouvait jamais afficher la santé de sonde. Mêmes règles que le WS (voir le tableau feature-024 de la section [WS /ws](#ws-ws--write)) : pentes arrondies à **1 décimale**, zéro à **2 décimales** (mV), `null` si jamais lu ou invalidé (NaN) ; `phSlopeAgeMs` `null` si aucune `Slope,?` n'a jamais réussi. Lecture **cache uniquement** (zéro I²C dans le chemin async). Buffer `/data` porté de 1792 à **1920** octets.
>
> **feature-075 (v2.24.0) — objet `flow_assist`** : instantané **lecture seule** de la session de l'[assistant de mesure du débit](#assistant-de-mesure-du-débit-des-pompes-feature-075-v2240). Buffer `/data` porté de 1920 à **2304** octets.

#### Champs ajoutés en feature-140 (Chloration de secours, v2.55.0)

**Deux champs MIGRÉS depuis la trame WebSocket** ([ADR-0086](adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md), aucune nouvelle grandeur — ils existaient déjà côté firmware, servis jusqu'ici seulement sur `/ws`) :

| Champ | Type | Description |
|-------|------|-------------|
| `filtration_state_source` | string | Origine de `water_present` : `"commanded"` (managed), `"powered"` (powered), `"external"` (external). **`/data`-seul depuis la v2.55.0** — voir [WS /ws](#ws-ws--write). |
| `filtration_ext_on` | boolean | Dernier état signalé par la filtration externe (`ON`/`OFF`). **`/data`-seul depuis la v2.55.0** — voir [WS /ws](#ws-ws--write). |

**Trois champs NEUFS**, à parité stricte `/data` ↔ WebSocket (mêmes clés, même calcul) :

| Champ | Type | Description |
|-------|------|-------------|
| `rescue_until` | integer (epoch) | Instant d'expiration de la **Chloration de secours** (48 h glissantes depuis l'activation — **pas** un ancrage sur minuit local, voir [ADR-0087](adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md)). `0` si le mode est inactif. Piloté par [`POST /rescue/start`](#post-rescuestart--write) \| [`/rescue/stop`](#post-rescuestop--write), jamais par `/save-config`. Persisté en NVS (survit à un reboot). |
| `rescue_dosing` | boolean | Verdict **dérivé** « le secours dose-t-il réellement maintenant ? » = actif **et** pas suspendu par l'inhibition ORP-sur-pH (pH connu et > 7,60). **Pas** un simple miroir de `rescue_until > 0` : une activation armée peut être suspendue sans que `rescue_until` le dise (filtration à l'arrêt, pH haut, plafond journalier atteint — ces trois causes sont dérivées côté client d'autres champs `/data` déjà existants, voir [page-dashboard.md](features/page-dashboard.md#carte-modes-temporaires-feature-140)). |
| `rescue_lift` | integer (0-3) | Raison de **fin récente** du mode, fenêtre de visibilité glissante de 90 s (`kRescueLiftReasonVisibleMs`) : `0` = aucune fin récente, `1` = levée automatique (30 min de mesures ORP de nouveau fiables — **seul** cas où l'annonce « les mesures sont redevenues fiables » est vraie), `2` = expiration naturelle des 48 h (la régulation automatique a déjà repris, rien ne prouve que l'eau est fiable — contrôle au réactif DPD recommandé), `3` = arrêt manuel. Retombe à `0` de lui-même, **pas** un événement à acquitter. |

**Le pH n'est jamais dosé par ce mode** (condition #11 du gate `pool-chemistry`) : aucun champ pH n'est affecté par la Chloration de secours. Voir [MQTT.md](MQTT.md#topic-et-entité-ajoutés-en-feature-140-chloration-de-secours-v2550) pour les topics MQTT équivalents, et [pump-controller.md](subsystems/pump-controller.md#chloration-de-secours-feature-140) pour le détail firmware. Buffer `/data` porté à **3200** octets (chemin sans plafond dur, gabarit purement documentaire — le seul plafond dur du projet reste `kWsFrameBufBytes` côté WebSocket, voir [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2550--chloration-de-secours-feature-140-et-migration-de-filtration_state_sourcefiltration_ext_on-vers-data-seul)).

#### Champs ajoutés en bug-160 (bascule de régulation en Manuel pendant une calibration, volet 2)

⚠️ **REFONTE du 2026-10-13 (décision utilisateur)** : le mécanisme initialement documenté ici
(bail auto-expirant, trois champs `cal_hold_*`, `POST /calibration/hold`) a été **ABANDONNÉ avant
tout déploiement** — remplacé par une bascule explicite de la régulation pH+ORP en `manual` à
l'entrée de l'écran de calibration, restaurée par un geste humain (bouton « Fermer »). Voir
[ADR-0107](adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md) et
[pump-controller.md](subsystems/pump-controller.md#bascule-de-régulation-en-manuel-pendant-une-calibration-bug-160-volet-2).

**Deux champs NEUFS**, à parité stricte `/data` ↔ WebSocket (accesseur unique côté firmware,
mêmes clés, même calcul — voir [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data--bascule-de-calibration-bug-160)) :

| Champ | Type | Description |
|-------|------|-------------|
| `cal_switch_active` | boolean | Une session de bascule est **ouverte** : la régulation pH et/ou ORP a été mise en `manual` à l'entrée d'un écran de calibration et n'a **pas encore** été restaurée par `POST /calibration/exit`. |
| `cal_switch_epoch` | integer (epoch) | Instant de l'**entrée** dans l'écran de calibration qui a ouvert la session courante. `0` si inactive, ou si l'horloge n'était pas synchronisée à l'entrée (jamais un 1970 trompeur — même patron que `phAutoManualEpoch`). |

Pilotés par [`POST /calibration/enter`](#post-calibrationenter--write) et
[`POST /calibration/exit`](#post-calibrationexit--write) — ⛔ **ni bail ni minuteur de relâche** :
la session reste ouverte tant que `/calibration/exit` n'a pas été appelé, **y compris si le
navigateur est fermé** — c'est la restauration qui dépend d'un geste humain, pas la suspension
du dosage (coupée dès l'entrée, et le restant immédiatement visible via `phRegulationMode` /
`orpRegulationMode` = `"manual"`, déjà servis par `/data`).
>
> ⚠️ **Corrigé en v2.47.1 (`bug-082`) — la mention d'origine « pas de champ WS » était un défaut, pas un choix.** `flow_assist` était servi par `/data` **seul** alors que la trame `sensor_data` **remplace `latestSensorData` en entier** toutes les 5 s : ses champs repassaient donc à `undefined` en quelques secondes. **Deux** champs sont désormais servis **des deux côtés**, avec les mêmes règles null/arrondi : **`aborted`** et **`flow255`** (tableau de 2, jamais un scalaire). Les **sept** autres restent `/data`-seuls — ils ne pilotent l'écran de l'assistant que **pendant** une session, et le client force un `GET /data` à son ouverture. Parité **assertée en CI** (sentinelle `test_native_ws_data_parity`, second axe imbriqué — [ADR-0071](adr/0071-parite-data-trame-capteurs.md)) ; budget de trame et méthode de chiffrage : [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2471--parité-de-flow_assist-bug-082).
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `active` | boolean | Session d'assistant ouverte → dosage automatique **suspendu** et injections manuelles classiques refusées (`409 assist_active`). |
> | `pump` | integer | Pompe **physique** en assistant (1\|2), `0` si aucune session. |
> | `phase` | string | Marche en cours : `"purge"` \| `"measure"`, `""` pompe à l'arrêt. |
> | `remaining_s` | integer | Secondes restantes de la marche en cours (`0` sinon) — resynchronisation du compte à rebours client. |
> | `aborted` | boolean | La **dernière** marche a été interrompue **par le serveur** (perte de filtration). L'UI ne doit **jamais** router une mesure avortée vers la saisie du volume (débit sous-estimé → sur-injection). Réarmé à `false` au `start` suivant. **Servi aussi sur la trame WS depuis la v2.47.1** : c'est un **garde-fou de dosage**, il ne peut pas dépendre d'un seul transport. |
> | `purges_left` | integer | Purges restantes dans la session (max 3, `kFlowAssistMaxPurgesPerSession`). |
> | `purge_s` / `measure_s` | integer | Durées **servies** (60 s / 120 s) — l'UI paramètre ses libellés dessus, pas de miroir en dur. |
> | `flow255` | array[2] | **Servi aussi sur la trame WS depuis la v2.47.1.** Débit **majorant** à duty 255 (ml/min, `[0]` = pompe 1, `[1]` = pompe 2) — permet à l'UI d'annoncer le coût budget des marches et la durée d'une injection volumée **sans constante dupliquée**. ⚠️ **v2.25.2 ([ADR-0050](adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md))** : la valeur est un **majorant unique de flotte** (`kPumpFlow255MlPerMin` = **74,0**), **identique pour les deux pompes** — elle ne décrit plus l'exemplaire câblé en n°1 ou n°2 (61,0 / 67,0 avant la v2.25.2). Le tableau **reste de taille 2** : le contrat client est inchangé. Un client doit traiter « clé absente », « tableau vide » et « 0 » comme **trois cas distincts** — seul un nombre fini > 0 est une information. |
>
> **feature-021** : `ph` est désormais publié avec **3 décimales** (vs 1 décimale en v1.x). Champs `phCalPoints` (`-1..3`) et `orpCalPoints` (`-1..1`) ajoutés. Champs **supprimés** de la réponse `/data` : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw` (la notion de « valeur brute » n'a pas de sens côté Atlas EZO — la valeur est déjà calibrée par le module). Voir [ADR-0014](adr/0014-migration-atlas-ezo.md).
>
> **feature-025** : `ph` / `orp` correspondent à la valeur filtrée (médiane + EMA). Champs
> `phRaw/phMedian/phFiltered/phRejectedCount` (+ équivalents `orp*`) ajoutés. Les champs
> flottants valent `null` si la mesure est indisponible. Mêmes champs côté WS.
> ⚠️ **`phFilterReady`/`phFilterUnstable` (+ `orp*`) sont RETIRÉS en v2.68.0** — voir
> l'amendement feature-152 ci-dessous, qui remplace intégralement ce paragraphe sur ce point.
> Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#chaîne-de-mesure-à-5-étages-feature-152).
>
> **feature-061 (v2.13.0)** : champs de **validité des mesures pH/ORP** ajoutés (mêmes champs côté WS `sensor_data`, même source : cache du singleton `measureValidity`) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `water_present` | boolean | Présence d'eau résolue (source unique feature-056 — déjà présent en WS, **ajouté à `/data`** ici). |
> | `measures_valid` | boolean | `true` = mesures pH/ORP représentatives du bassin. `false` = eau absente **ou** purge post-reprise en cours. |
> | `measures_stab_remaining_s` | integer | Secondes restantes de purge (arrondi seconde sup) ; `0` si valide ou eau absente. ⚠️ **Champ SUPPRIMÉ en v2.34.0** — voir le bloc feature-091 ci-dessous. |
>
> **feature-091 (v2.34.0) — purge adaptative** ([ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md)) : la purge se termine sur la **stabilité des mesures brutes**, encadrée par un plancher de **10 min** et un plafond de **90 min** (`kPurgeFloorMs` / `kPurgeCapMs`). Il n'y a donc **plus de fin connue à l'avance** : `measures_stab_remaining_s` est **retiré** et remplacé par quatre champs. **Mêmes champs, mêmes valeurs côté WS `sensor_data`** (parité obligatoire — un champ servi d'un seul côté *clignote* au lieu de disparaître) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `measures_stab_elapsed_s` | uint32 | Secondes **écoulées** dans la purge courante ; `0` hors purge. Nom toujours **distinct** de `stabilization_remaining_s` (sas post-calibration des pompes). |
> | `measures_purge_last_duration_s` | uint32 | Durée de la **dernière purge terminée** (s) ; `0` si aucune depuis le boot ou si elle a été dispensée. |
> | `measures_purge_last_end` | uint8 | Motif de fin : `0` aucune, `1` stabilité, `3` dispensée (arrêt d'eau court). ⚠️ **Depuis la v2.62.0** (feature-139, voir amendement plus bas) : `2` (plafond, qui déclenchait l'alerte MQTT `measures_purge_timeout` — **retirée**) et `4` (re-purge) ne sont **plus jamais émis** — valeurs réservées, non réattribuées. Valeurs figées (`enum class PurgeEnd`, [`src/measure_validity.h`](../src/measure_validity.h)). |
> | `measures_purge_count` | uint8 | Nombre de **re-purges** du cycle d'eau courant (refermeture sur mesure redevenue instable) ; remis à zéro à l'arrêt de la circulation. |
>
> ⚠️ Le motif `2` **ouvre** la porte (`measures_valid` repasse à `true`) : c'est un fail-safe de disponibilité de la régulation, pas un blocage — les gardes pH/ORP habituelles restent seules en vigueur. Un client ne doit donc **pas** en déduire « mesures invalides », mais « mesures à surveiller ».
>
> Rétrocompat : un client antérieur qui lit `measures_stab_remaining_s` ne le trouve plus (clé absente) — il doit traiter l'absence comme « pas de compte à rebours disponible ». Buffer `/data` **inchangé** (gabarit documentaire 2304 o : +97 o net au pire cas ; ce chemin n'a de toute façon aucun plafond dur, `sendJsonResponse` dimensionne la `String` à la demande). Détail : [`docs/subsystems/measure-validity.md`](subsystems/measure-validity.md).
>
> **feature-139 (v2.62.0) — retrait de l'analyse de stabilité** ([ADR-0092](adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'[ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md)) : la purge ne juge plus la **stabilité** de la mesure (fenêtre de 10 statistiques de bloc, seuils 0,15 pH / 15 mV), seulement le **temps** écoulé depuis l'arrêt d'eau — motif établi par le gate `pool-chemistry` : ce détecteur n'était qu'un filtre de transitoire, structurellement aveugle à une mesure durablement fausse, et ses seuils n'avaient jamais été calés que sur un seul bassin. Conséquences sur ce bloc de champs :
>
> | Champ | Changement |
> |-------|------------|
> | `measures_purge_count` | **Retiré.** Le mécanisme de re-purge (refermeture C1) n'existe plus — ce champ serait devenu structurellement constant à 0. |
> | `measures_purge_last_end` | **Domaine réduit à `{0, 1, 3}`.** `2` (plafond) et `4` (re-purge) ne sont **plus jamais émis** — valeurs **réservées, non réattribuées** : un consommateur qui testait `=== 2` cesse de matcher, sans faux-positif. |
> | `measures_stab_elapsed_s`, `measures_purge_last_duration_s` | **Inchangés.** |
> | `measures_dose_available_in_s` | **Nouveau.** uint32, secondes avant que le dosage automatique redevienne possible (max du reste de plancher de purge et du reste de plancher de gel du filtre capteur — remplace un calcul **côté client** faux d'un facteur 10). **0** si déjà disponible. ⚠️ Ce nombre promet uniquement la **fin de l'attente d'eau neuve** — jamais « dosage garanti » : plafonds horaire/journalier, anti-rafale, pause de mélange, présence d'eau et watchdog restent en dehors. Le champ est **omis** quand la filtration est à l'arrêt (délai alors inconnu). Détail : [`docs/subsystems/measure-validity.md`](subsystems/measure-validity.md#délai-avant-dosage-annoncé-à-lutilisateur-dose_available_logic-feature-139). |
>
> L'alerte MQTT `measures_purge_timeout` est **retirée** (voir [MQTT.md](MQTT.md#alertes)) — plus aucune émission possible, le motif `2` ne se produisant plus.
>
> **feature-067 (v2.20.0) — sémantique brut / corrigé des valeurs pH.** Aucun champ n'est ajouté ni renommé dans `/data`, mais la **valeur** de trois d'entre eux intègre désormais l'offset logiciel `ph_offset` :
>
> | Champ | Contenu | Offset appliqué ? |
> |-------|---------|-------------------|
> | `ph` | valeur officielle pH (médiane + lissage) **corrigée** — `null` pendant la montée en régime depuis v2.68.0, **plus de repli** sur une autre valeur (voir l'amendement feature-152 plus haut) | ✅ oui |
> | `phMedian` | médiane glissante **corrigée** | ✅ oui |
> | `phFiltered` | pH lissé (médiane + EMA) **corrigé** | ✅ oui |
> | `phRaw` | dernière mesure **brute Atlas EZO** | ❌ non |
>
> Règle mnémotechnique côté firmware comme côté JSON : **seuls les champs suffixés `Raw` sont bruts**. Le filtre pH continue de travailler sur le signal brut ; l'offset est appliqué **en sortie**, donc un changement est visible immédiatement — cette règle de composition **survit telle quelle** à la refonte de la chaîne de mesure (feature-152, v2.68.0), seuls les étages internes du filtre ont changé. `NaN` en entrée ⇒ `null` en sortie : l'offset ne fabrique jamais une mesure. Voir [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#offset-logiciel-de-mesure-ph--feature-067).

> **feature-155 (v2.71.0) — champ `phValueRaw` ajouté.** Nouveau champ, **échelle brute**, arrondi à 3 décimales, `null` si NaN : `sensors.getPhValueRaw()`, c'est-à-dire la valeur **officielle** issue du filtre pH (médiane + lissage) mais **sans** l'offset logiciel. ⚠️ **Ce n'est pas un synonyme de `phRaw`** : `phRaw` est la dernière mesure Atlas EZO **instantanée** (dispersion ~0,36 sur un bassin typique de ce projet, jusqu'à ±0,18 d'écart avec la valeur lissée — deux pas de réglage), alors que `phValueRaw` porte le **même lissage** que `ph`/`phMedian`/`phFiltered`, simplement **avant** l'application de l'offset. C'est la valeur que [`POST /tune-ph-offset`](#post-tune-ph-offset--critical) utilise comme référence pour calculer l'écart d'un accord sur test manuel — servie ici pour que l'aperçu côté client utilise la **même** référence que le calcul serveur, et jamais `ph`/`phFiltered`/`phMedian` (déjà corrigés : partir de l'un d'eux ajouterait l'écart à un écart déjà appliqué). Même champ, mêmes règles, servi à l'identique par la trame WebSocket `sensor_data` — voir [docs/subsystems/ws-manager.md](subsystems/ws-manager.md#champ-sensor_data-ajouté-en-feature-155--phvalueraw-référence-de-laccord-ph-v2710).

> ⚠️ **`filtration_elapsed_s` — dette documentaire soldée (feature-157/feature-158, v2.73.0/v2.74.0).** Ce champ (uint32, secondes) existe depuis la v2.73.0 (feature-157, servi également par la trame WebSocket `sensor_data`), mais n'avait **jamais** été documenté ici — né dans un correctif post-livraison sans passage `doc-writer`. **Ce n'est pas l'uptime de l'appareil**, même si avant la v2.74.0 (feature-158) il s'y confondait de fait : un défaut constaté en production (1 619 s annoncés contre 1 644 s d'uptime, pour ~5 h de circulation réelle) a montré que la durée repartait de zéro à chaque redémarrage logiciel, y compris quand la pompe n'avait jamais cessé de tourner. Depuis la v2.74.0, la durée **persiste à travers un redémarrage logiciel court pendant lequel l'eau n'a pas cessé de circuler** (verdict de continuité de `measure_validity`, consommé — jamais recalculé — par `filtration`) : voir [docs/subsystems/filtration.md](subsystems/filtration.md#reprise-de-la-durée-de-filtration-à-travers-un-redémarrage-feature-158-v2740) et [ADR-0104](adr/0104-la-continuite-de-l-eau-a-un-producteur-unique-et-des-consommateurs.md). Alimente la garde des 10 minutes de [`POST /tune-ph-offset`](#post-tune-ph-offset--critical) (refus n°2). `0` si la filtration est à l'arrêt.

> ~~**v2.20.3 — alarme « filtre bloqué »** (`phFilterAlarm`/`orpFilterAlarm`)~~ — **RETIRÉS en
> v2.68.0** (feature-152) : le mécanisme « filtre non prêt durablement » et son verrou ont
> disparu, voir l'amendement ci-dessous.
>
> **feature-152 (v2.68.0) — refonte de la chaîne de mesure pH/ORP, 5 étages au lieu de 14**
> ([ADR-0097 « le filtre lisse, le dosage juge »](adr/0097-le-filtre-lisse-le-dosage-juge.md)).
> Récapitulatif des champs `/data` et WS `sensor_data` (parité stricte, mêmes valeurs des deux côtés) :
>
> | Champ | Changement |
> |-------|------------|
> | `ph` / `orp` | **Portent désormais directement la VALEUR OFFICIELLE** (médiane 9 + lissage τ=45 s, pH corrigé). `null` pendant la montée en régime, sonde figée ou mesure périmée — **plus de repli silencieux** sur le brut ou la dernière mesure. Avant ce lot, l'écran pouvait afficher une valeur pendant que le dosage refusait sur une valeur absente (défaut constaté le 2026-10-11). |
> | `phFilterReady` / `orpFilterReady` | **Retirés.** `isnan(ph)` / `isnan(orp)` suffit désormais — il n'y a plus d'état « prêt » distinct de la valeur. |
> | `phFilterUnstable` / `orpFilterUnstable` | **Retirés.** La médiane écarte les aberrations par construction (sans seuil réglable) ; il n'y a plus de jugement d'instabilité séparé. |
> | `phFilterAlarm` / `orpFilterAlarm` | **Retirés.** L'alarme « filtre non prêt durablement » et son verrou ont disparu — verdict `pool-chemistry` : ce mécanisme ne protégeait contre aucun dommage démontré. |
> | `phMedian` / `orpMedian`, `phRejectedCount` / `orpRejectedCount` | **Conservés, reclassés diagnostic seul** — ne conditionnent plus aucune décision de dosage. |
> | `measures_valid` | **Inchangé.** Reste la validité de la **chambre de mesure seule** (filtration arrêtée / purge) — une version intermédiaire de ce lot l'avait couplé à `!isnan(ph) && !isnan(orp)`, **retirée en revue** : côté interface, `measuresAreValid()` gate aussi la **température** (chaîne saine, hors périmètre) et la **voie opposée** — une sonde ORP figée aurait donc masqué le pH **et** la température, et fait annoncer « Reprise automatique imminente » sur une **panne matérielle**. Le remplissage de la fenêtre (montée en régime après front montant d'eau) est porté **par voie**, par le code de refus `15` (« Les mesures se mettent en place ») — voir la table des codes ci-dessous — pas par `measures_valid`. |
>
> Buffer `/data` et trame WS **allégés** (6 booléens retirés, 141 o au pire cas sur la trame WS —
> voir [`docs/subsystems/ws-manager.md`](subsystems/ws-manager.md)). Reprise manuelle :
> [`POST /reset-sensor-filter`](#post-reset-sensor-filter--critical), inchangée dans son contrat
> HTTP (son effet interne change : vidage de la fenêtre médiane, plus de levée de latch — il n'y
> en a plus). Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#chaîne-de-mesure-à-5-étages-feature-152).

> **feature-068 (v2.47.1) — sonde de température d'eau figée** (champ **en parité stricte** `/data` ↔ WS `sensor_data`, mêmes valeurs, valeur **instantanée** des deux côtés) :
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `tempFrozenAlarm` | boolean | `true` quand la sonde DS18B20 « eau » a rendu `kTempFrozenSamples` = **43 200** lectures valides consécutives dans une bande < `kTempFrozenEpsilonC` = **0,03 °C** (≈ **24 h** à 2 s, bug-142 — était 900 / 30 min). Miroir direct de `SensorManager::isTemperatureFrozen()`. |
>
> ⚠️ **Ce drapeau est informatif pour le DOSAGE** : il n'inhibe **ni** le dosage, **ni** la compensation `RT` de l'EZO pH — l'erreur de pH induite par une température figée est de **≈ 0,001 pH**, soit 2 à 3 ordres de grandeur sous le premier seuil de régulation. Il **gate en revanche le planning automatique de filtration** (bug-142) : `filtration.cpp:162` bloque tout recalcul pendant l'épisode, `:405` rejette les échantillons de T°. Ne pas l'utiliser comme condition de sécurité chimique côté intégrateur. Motif complet et chiffrage : [ADR-0074](adr/0074-temperature-figee-statu-quo-instrumente.md).
>
> **Levée immédiate**, sans hystérésis, dès la première lecture hors bande. Le seul anti-battement du projet sur cet état est un **maintien de 60 s à la publication MQTT** (`<base>/temp_sensor_frozen`) — il ne s'applique **pas** à `/data` ni au WebSocket. Coût de trame WS : **24 o** au pire cas structurel (`"tempFrozenAlarm":false,`), marge résiduelle **43 o** sous `kWsFrameBufBytes = 3584` (voir [`ws-manager.md`](subsystems/ws-manager.md)).

> **feature-094 (v2.36.0) — changement de SENS de `ph_dosing`** ([ADR-0062](adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)). **Aucun champ ajouté, aucun renommé, aucun retiré** — mais la valeur ne veut plus dire la même chose :
>
> | Champ | Avant (≤ 2.35.0) | Depuis 2.36.0 |
> |-------|------------------|---------------|
> | `ph_dosing` | « un débit a été commandé » (≈ la pompe tourne) | « une **session de dosage est ouverte** » — **la pompe peut être à l'arrêt** |
>
> Le verrou de session pH suit désormais la **porte** (décidée sur une moyenne glissante de 10 min de la mesure filtrée) et non plus le débit instantané : pendant une **prolongation** — durée minimale de session de 30 min murales, débit plafonné au feedforward puis nul — `ph_dosing` **reste `true` pompe à l'arrêt**, jusqu'à `kPhSessionMaxWallMs` = 3 h au pire cas. Même sens pour le champ WS `ph_dosing` (parité stricte), pour MQTT (`{base}/ph_dosing`, `ph_dosing_active`) et pour `history.phDosing`. **`orp_dosing` est inchangé.**
>
> ⚠️ Un client qui veut « **la pompe est alimentée maintenant** » ne doit pas lire `ph_dosing` : c'est le rôle du **calendrier d'impulsion** ([ADR-0045](adr/0045-etat-impulsion-expose-en-calendrier.md)) — pompe alimentée ⇔ `ph_pulse_phase_ms < ph_pulse_ms`. Détail : [pump-controller.md](subsystems/pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360).

> **feature-096 (v2.37.0) — temps d'atteinte de cible** ([ADR-0063](adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)) : **8 champs** décrivant, pour chaque régulation, le chronomètre du jour. **Mêmes champs, mêmes valeurs, même passage côté WS `sensor_data`** (parité obligatoire). Source unique : `timeToTarget.getSnapshot()` — un instantané mémorisé sous spinlock, jamais l'accesseur de `pump_controller`.
>
> | Champ | Type | Description |
> |-------|------|-------------|
> | `ph_ttt_state` / `orp_ttt_state` | uint8 | `0` pas encore de départ aujourd'hui, `1` en cours, `2` cible atteinte, `3` non atteinte. **Valeurs 4-255 réservées** — un client doit les traiter comme « état inconnu », jamais comme un voisin. |
> | `ph_ttt_min` / `orp_ttt_min` | uint16 | **Minutes de circulation**, bornées à 1440. ⚠️ **Le sens dépend de l'état** (voir ci-dessous). |
> | `ph_ttt_e0` / `orp_ttt_e0` | float \| `null` | Écart **initial signé** (valeur − consigne) à t0. pH à **2 décimales**, ORP à **1**. `null` = aucun départ, ou écart indisponible — **jamais `0`**. |
> | `ph_ttt_flag` / `orp_ttt_flag` | uint8 | **CHAMP DE BITS**, jamais une énumération (voir table des bits). |
>
> ⚠️ **`*_ttt_min` a un sens VARIABLE selon `*_ttt_state`** — le lire sans lire l'état donne un chiffre faux :
>
> | `state` | Ce que porte `*_ttt_min` |
> |---|---|
> | `1` en cours | minutes **écoulées** depuis t0 |
> | `2` atteinte | **temps d'atteinte** (`0` = « déjà à la cible au départ ») |
> | `3` non atteinte | **durée totale** de circulation de la journée |
> | `0` | `0` (aucune signification) |
>
> **Champ de bits `*_ttt_flag`** — **encodage figé dès le premier déploiement** (il est aussi archivé en NVS). Une journée peut cumuler plusieurs circonstances ; les bits 5-7 sont réservés. **Tester des bits, jamais l'égalité** :
>
> | Bit | Valeur | Sens |
> |---|---|---|
> | 0 | `0x01` | consigne **configurée** modifiée dans la journée |
> | 1 | `0x02` | **boost** ORP actif dans la journée (un boost ne lève **pas** le bit 0) |
> | 2 | `0x04` | purge de mesure terminée au **plafond** (stabilisation non atteinte, feature-091) |
> | 3 | `0x08` | reprise après **coupure secteur** |
> | 4 | `0x10` | **régulation non automatique** (manuel, maintenance, assistant de débit) — journée à **exclure des moyennes** de comparaison de versions |
>
> Rappel de la règle de partage du bit 4 ([ADR-0063](adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)) : *ce que le régulateur **choisit** compte ; ce qu'on lui **impose** de l'extérieur porte le bit*. Un quota journalier atteint, une limite horaire ou un actionneur sous-dimensionné ne posent **aucun bit** — ce sont de vraies non-atteintes, et c'est ce que la métrique doit voir.
>
> Rétrocompat : un firmware antérieur à la 2.37.0 ne sert **aucune** de ces clés. Un client doit traiter l'absence de `*_ttt_state` comme « indicateur inexistant » (bloc masqué), **jamais** comme l'état `0`. Gabarit documentaire `/data` **inchangé** ; ce chemin n'a de toute façon aucun plafond dur. Détail : [`docs/subsystems/time-to-target.md`](subsystems/time-to-target.md).

> **feature-092 (v2.46.0) — composition du débit d'injection** : **8** champs (`ph_ff_learned_ml_per_min`, `ph_p_term_ml_per_min`, `ph_i_term_ml_per_min`, `ph_demand_ml_per_min`, `orp_p_term_ml_per_min`, `orp_demand_ml_per_min`, `orp_sustainable_ml_per_min`, `orp_limited_by_hourly_budget`), plus **4** champs de **pause mélange** (`phMixingDelayActive`, `orpMixingDelayActive`, `ph_mix_remaining_s`, `orp_mix_remaining_s`) qui étaient WS-seuls, sont désormais servis **des deux côtés**, dans le même passage de code. Réciproquement, `ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized`, jusqu'ici `/data`-seuls, rejoignent la trame WS. ⚠ **Il n'existe aucun champ « appliqué »** : `ph_applied_ml_per_min` n'existe pas. Tableau complet, règles de nullité et budget : [Champs de composition du débit d'injection](#champs-de-composition-du-débit-dinjection-feature-092-v2460).

> Les champs live `ph` / `orp` / `phRaw` / … restent publiés tels quels quand `measures_valid` est `false` (calibration et diagnostics intacts) — c'est le **client** qui arbitre l'affichage. ⚠️ **Repli HTTP** : `/data` n'expose pas `filtration_state_stale` (présent en WS uniquement) — un client en polling HTTP dégrade « Signal filtration périmé » en « Filtration arrêtée » (asymétrie préexistante, à harmoniser). Voir [`docs/subsystems/measure-validity.md`](subsystems/measure-validity.md).

---

### GET /get-history — WRITE

Retourne l'historique des mesures.

```bash
curl -u admin:monmotdepasse "http://poolcontroller.local/get-history?range=24h"
```

Paramètre `range` : `24h` (défaut), `3d`, `7d`, `30d`, `all`. Une valeur **inconnue** est traitée comme `all` — et depuis la v2.14.0, seul le **token reconnu** est échoé dans le champ `range` de la réponse (jamais la valeur brute du client, qui pourrait malformer le JSON).

Paramètre optionnel `?since=TIMESTAMP` pour récupération incrémentale.

**Format de réponse** :

```json
{"range":"24h","count":0,"history":[{"timestamp":1753860000,"ph":7.2,"phFiltered":7.23,"orp":650,"orpFiltered":649,"temperature":24.6,"filtration":true,"dosing":2,"granularity":0}, "…"],"count":123}
```

> ⚠️ La clé `count` apparaît **deux fois** : `0` dans l'en-tête, la valeur réelle dans le pied. Bizarrerie historique **conservée volontairement** pour compatibilité (les parseurs JSON gardent la dernière occurrence).

> **feature-061 (v2.13.0)** : les points enregistrés pendant un arrêt de filtration (ou pendant la purge post-reprise) ont `ph` et `orp` à **`null`** (contrat existant, identique à un capteur absent) — `temperature`, `filtration` et `dosing` restent renseignés. Voir [`docs/subsystems/history.md`](subsystems/history.md#exclusion-des-mesures-phorptempérature-sans-circulation-deau-feature-061-v2130).

#### `dosing` : un MASQUE DE VOIES entier (bug-098, v2.47.0) ⚠️ changement de type

**Jusqu'à la v2.46 incluse, `dosing` était un booléen** : `true` disait « une pompe doseuse tournait à cet instant », sans dire **laquelle**. Depuis la **v2.47.0**, c'est un **entier**, champ de bits des voies réellement dosées ([ADR-0072](adr/0072-absence-de-valeur-hors-du-domaine.md)) :

| Bit | Valeur | Sens |
|---|---:|---|
| `kHistDosePh` | `0x01` | pompe **pH** active à l'instant du point |
| `kHistDoseOrp` | `0x02` | pompe **ORP (chlore)** active |
| `kHistDoseUnknownChannel` | `0x04` | **dosage, voie indéterminée** — point écrit par un firmware **≤ 2.46**, qui ne persistait pas la voie |

- **Valeurs observables** : `0`, `1`, `2`, `3`, `4` (jusqu'à `7` en théorie — un agrégat horaire peut cumuler `0x04` d'un point ancien et `0x01`/`0x02` de points récents). Les bits 3-7 sont **réservés**.
- **Tester des bits, jamais l'égalité.** `dosing & 1` pour « dosage pH », `dosing & 2` pour « dosage chlore ». Un `=== 1` casserait dès qu'un bit inconnu apparaît.
- **`0x04` ne se lit JAMAIS « pH ».** Un booléen ne dit pas la voie : la déduire inventerait une information absente. C'est exactement le défaut corrigé par bug-098, du côté firmware **comme** du côté client.
- **Un client qui lisait `dosing` en booléen JavaScript ne change pas de verdict** : `0` est *falsy*, `1`/`2`/`3`/`4` sont *truthy*. La clé **reste toujours présente** ; seul son type change.
- **Les deux surfaces portent les mêmes trois bits.** `GET /get-history` sert le masque **verbatim** ([`web_routes_data.cpp:1046`](../src/web_routes_data.cpp:1046) passe `p.doseMask` à `formatHistoryPointJson`) — aucune traduction, aucune perte entre le fichier interne [`/history.json`](subsystems/history.md) et le fil HTTP.
- **`POST /history/import` accepte les deux formats**, distingués par le **type JSON** de la valeur : booléen ⇒ export ≤ 2.46 ⇒ `0x04` (voie indéterminée) ; entier ⇒ masque, réduit à `0x07` ; clé absente ⇒ `0` (aucun dosage). Détail et limite du discriminant : [history.md](subsystems/history.md).
- **Export CSV de l'interface** : la colonne historique `dosing` est **conservée** en `0`/`1`, et une colonne **`dosing_mask`** est **ajoutée** à côté. Réutiliser le nom `dosing` aurait rendu un `1` **indécidable** — pH, ou voie inconnue ? — les anciens exports écrivant déjà `1` pour dire « il y a eu dosage ».

#### Mesure lissée : `phFiltered` / `orpFiltered` (feature-095, v2.38.0)

Chaque point porte, à côté de la mesure **brute** (`ph` = brut + offset, `orp` = brut), la mesure **lissée** — celle sur laquelle la régulation décide (médiane + EMA, exactement la valeur servie par `ph` / `orp` de [`/data`](#get-data--write) au même instant).

| Clé | Type | Arrondi | Description |
|-----|------|---------|-------------|
| `phFiltered` | float \| `null` | **2 décimales** (`%.2f`) | pH lissé vu par la régulation. Le dixième du brut masquerait l'essentiel de sa dynamique (la régulation décide au centième). |
| `orpFiltered` | int \| `null` | **entier** | ORP lissé (mV), mêmes règles d'arrondi que `/data`. |

**Règles de lecture** — trois points, dans cet ordre d'importance :

1. **Les clés sont TOUJOURS présentes**, comme `ph` / `orp` / `temperature` : l'absence de mesure se dit **`null`**, jamais par omission de clé. Ne pas confondre avec le **fichier persisté** `/history.json`, qui lui utilise des clés courtes (`pf` / `of`) et **omet** la clé quand la valeur n'est pas finie — voir [`docs/subsystems/history.md`](subsystems/history.md#point-persisté-vs-point-exporté--deux-formats-distincts-feature-095).
2. **La lissée n'existe QUE sur les points RAW** (`granularity: 0`, points 5 min, ~8 h de profondeur). Sur les **moyennes horaires** (`1`) et **journalières** (`2`), `phFiltered` et `orpFiltered` valent **toujours `null`** — elle n'est ni persistée ni agrégée à ces granularités (décision de budget de partition, [ADR-0064](adr/0064-la-courbe-suit-le-nombre-affiche.md)). Un client qui trace la lissée doit donc gérer une série **plus courte** que celle du brut sur une fenêtre 24 h.
3. **`null` ne vaut pas 0.** Filtre non amorcé, porte de mesure fermée (filtration arrêtée, purge), agrégat, ou point enregistré par un firmware antérieur à la 2.38 : dans tous ces cas la valeur est absente, jamais repliée sur le brut.

**Rétrocompatibilité** : ajout **purement additif**. Un client antérieur ignore les clés qu'il ne connaît pas ; un firmware antérieur à la 2.38.0 ne sert aucune des deux clés (le client doit traiter leur absence comme « lissée inexistante »). Les points enregistrés **avant** la 2.38 restent dans l'historique avec `phFiltered` / `orpFiltered` à `null` — aucune migration, aucune purge.

**Aller-retour** : [`POST /history/import`](#post-historyimport--write) relit ces deux clés **longues** (celles de l'export, pas les clés courtes du fichier persisté) — restaurer une sauvegarde `/get-history` restitue la lissée à l'identique, sans perte silencieuse.

> ⚠️ **Points omis (rare, mais désormais audible)** : si le JSON d'un point dépasse `kHistoryPointJsonMaxBytes` (**192 o** depuis la v2.38.0, pire cas réaliste **161 o**), le point est **sauté** — jamais de JSON partiel. La réponse reste valide, mais la courbe est trouée. Depuis la v2.38.0 ces omissions sont **comptées** et un **WARNING throttlé 1/h** est journalisé (« *N point(s) omis de la réponse…* ») : un trou de courbe n'est plus indiscernable d'une panne de sonde.

**Transfert chunked (v2.14.0, [ADR-0032 §Extension 2026-07-30](adr/0032-reponses-http-bornees.md#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140))** : la réponse est émise en `Transfer-Encoding: chunked` (mécanisme (b), patron `/download-logs`) — plus aucune `String` contiguë ~40 Ko épinglée pendant le transfert, et le **503** « heap fragmenté » de la v2.10.4 **n'existe plus sur cette route** (aucune garde heap nécessaire en chunked). Transparent pour `fetch()`/curl : contrat JSON identique.

**Instantané sous mutex unique (v2.18.2)** : le serveur copie un **instantané filtré** de l'historique en **une seule** prise de mutex (100 ms) à l'ouverture de la requête, puis streame **sans verrou**. Une réponse `200` est donc un JSON **complet et cohérent** (aucun glissement de buffer possible : l'instantané est figé, ~11 Ko transitoires libérés en fin de réponse).

**Erreur `503` (rare)** : si le mutex est occupé au moment de la requête (consolidation/sauvegarde de l'historique en cours, ~1,5 s toutes les 5 min) :

```json
{"error":"historique occupé, réessayez"}
```

Le client réessaie simplement (le dashboard re-sollicite la route au refresh suivant).

> ℹ️ **Note historique** : entre la v2.14.0 et la v2.18.1, **toutes** les réponses `/get-history` étaient tronquées (pied `],"count":N}` jamais émis — bug de machine à états corrigé en v2.18.2). La note v2.18.1 sur les « espaces de maintien de flux sous contention » relevait d'un diagnostic **erroné** ; ce mécanisme a été retiré en v2.18.2 — le corps ne contient plus de whitespace entre les tokens.

---

### GET /get-kpi — WRITE

Mémoire **7 jours** du [temps d'atteinte de cible](subsystems/time-to-target.md) des deux régulations (feature-096, v2.37.0). Sert la bande de pastilles des pages pH et ORP, et toute analyse externe.

**Authentification** : `RouteProtection::WRITE` — la **même classe que `/data`**. `NONE` exposerait sans authentification le comportement du bassin (heures de circulation, réactivité de la régulation, jours de maintenance) et rejouerait bug-081.

```bash
curl -u admin:monmotdepasse "http://poolcontroller.local/get-kpi"
```

**Réponse 200**

```json
{
  "days": [
    { "d": 20260904, "ph": { "s": 2, "m": 223, "e0": 0.18, "f": 0 },
                     "orp": { "s": 2, "m": 303, "e0": -45.0, "f": 0 } },
    { "d": 20260903, "ph": { "s": 3, "m": 612, "e0": 0.24, "f": 16 } }
  ]
}
```

⚠️ **La charge est imbriquée sur deux niveaux** : une entrée de `days` ne porte au premier niveau que `d`, et **une sous-structure par régulation** (`ph`, `orp`). Les quatre valeurs `s` / `m` / `e0` / `f` sont **à l'intérieur** de `ph` / `orp`, **jamais** au premier niveau de l'entrée. Un client qui lit `entrée.s` obtient `undefined` pour **toutes** les journées (`bug-111`, v2.45.1).

| Clé | Niveau | Type | Description |
|-----|--------|------|-------------|
| `d` | entrée | uint32 | Jour civil `AAAAMMJJ`. |
| `ph`, `orp` | entrée | objet, **omis** si absent | Sous-structure de la régulation pour cette journée. Une clé absente = **pas d'entrée** pour cette voie ce jour-là. |
| `s` | dans `ph` / `orp` | uint8 | État de la journée — même codage que `*_ttt_state` de `/data` (`0`-`3`, **4-255 réservés**). |
| `m` | dans `ph` / `orp` | uint16 | **Minutes de circulation** (≤ 1440) — **sens variable selon `s`**, cf. [`/data`](#get-data--write). |
| `e0` | dans `ph` / `orp` | float \| `null` | Écart initial signé à t0. pH **2 décimales**, ORP **1** — mêmes arrondis que `/data` et le WS. `null` = journée sans départ exploitable. |
| `f` | dans `ph` / `orp` | uint8 | **Champ de bits** — même encodage figé que `*_ttt_flag`, cf. [`/data`](#get-data--write). |

**Règles de lecture** :

- Le jour **le plus récent est en tête** ; au plus **7 entrées** (`kTimeToTargetHistoryDays`).
- Les deux voies sont archivées **indépendamment** : leurs dates peuvent différer. La réponse est l'**union** des dates, triée décroissant.
- Une voie **sans entrée** pour une date donnée voit sa clé **omise** — « absent » n'est pas « état 0 ». Un client doit distinguer les deux.
- Clés courtes (`s`/`m`/`e0`/`f`) : la charge est lue par la machine, et 7 jours × 2 voies multiplient chaque octet de nom par 14.
- **Non bloquant** : aucune lecture NVS, aucune E/S. La réponse est construite depuis un miroir RAM de 188 octets recopié sous spinlock `portMUX` (~800 o de JSON au pire cas).

**Erreurs**

- `401` : authentification manquante / invalide.
- `503 {"error":"Mémoire des indicateurs indisponible"}` : la NVS `pool-kpi` a été refusée, l'indicateur est **suspendu**. ⚠️ Ce cas ne renvoie **jamais** un tableau vide, qui se lirait « aucune journée enregistrée » alors que la mémoire est en panne. L'UI remplace alors la légende par « Historique indisponible » et **vide** la bande, sans toucher à la ligne de valeur (qui vient de `/data`).

> **Fréquence d'appel** : cette mémoire n'est écrite **que deux fois par jour et par régulation**. L'interface la relit à l'entrée dans les vues pH/ORP, puis uniquement au changement d'état ou de jour civil — **jamais en polling**.

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
  -d '{"history":[{"timestamp":1700000000,"ph":7.2,"phFiltered":7.23,"orp":720,"orpFiltered":719,"temperature":24.5}]}' \
  http://poolcontroller.local/history/import
```

⚠️ Le corps est un **objet** portant le tableau sous la clé `history` (un tableau nu
reçoit `400 « champ history manquant »`), plus le champ optionnel `mode` (section ci-dessous).

Les clés attendues sont celles **de l'export `/get-history`** (clés longues), pas celles du fichier persisté `/history.json`. Depuis la **v2.38.0**, `phFiltered` et `orpFiltered` sont relues : l'aller-retour `/get-history` → `/history/import` → `/get-history` restitue la [mesure lissée](#mesure-lissée--phfiltered--orpfiltered-feature-095-v2380) à l'identique. Clé **absente ou `null`** → mesure lissée absente (`null`), jamais 0 ni repli sur le brut.

**Clé `dosing` — les deux formats sont acceptés (bug-098, v2.47.0).** La lecture est **typée**, sans clé de version : un **booléen** est un export **≤ 2.46** et vaut `0x04` (« dosage, voie indéterminée ») ; un **entier** est un [masque de voies](#dosing--un-masque-de-voies-entier-bug-098-v2470) et est réduit à `0x07` ; une clé **absente** vaut `0`. Un booléen `true` ne se relit **jamais** « pH » — ce serait inventer la voie. ⚠️ **Limite assumée** : un `dosing` **flottant** (`3.0`) ou entier **64 bits hors plage `int`** ne satisfait aucun des deux cas et retombe sur `0` ; ni le firmware ni l'export de l'interface ne produisent ces formes.

#### Champ `mode` — « remplacer » ou « compléter » (bug-101, v2.48.0)

| `mode` | Effet | Fonction |
|---|---|---|
| `"replace"` | **Remplace** tout l'historique par le lot (comportement historique de la route) | `HistoryManager::importData()` |
| `"append"` | **Fusionne** le lot dans l'historique existant, créneau par créneau | `HistoryManager::appendData()` |
| **absent** | `"replace"` | — |
| valeur **inconnue** (`"merge"`, `42`, `null`…) | `"replace"` | — |

⚠️ **Le défaut est `replace`, et c'est délibéré** : tout client antérieur à la v2.48.0
postait un remplacement total sans le savoir. Son comportement est donc **inchangé**.
Une valeur inconnue retombe elle aussi sur `replace` — jamais une fusion silencieuse
sur un client qu'on n'a pas compris (`historyImportModeIsReplace()`,
[`src/history_import_logic.cpp`](../src/history_import_logic.cpp)).

**Règle de collision en mode `append`** : à `(timestamp, granularité)` identique, le
point **entrant écrase** le point vivant — **toutes granularités, RAW comprise** — et
**toutes** les occurrences du créneau, pas seulement la première. Conséquence
utilisable : **réenvoyer deux fois le même lot est exactement idempotent**, ce qui rend
une reprise après coupure réseau sûre. Détail et motif :
[`docs/subsystems/history.md`](subsystems/history.md).

```bash
# lot 1 — repart de zéro
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"mode":"replace","history":[{"timestamp":1700000000,"ph":7.2,"temperature":24.5}]}' \
  http://poolcontroller.local/history/import

# lots suivants — s'ajoutent au précédent
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"mode":"append","history":[{"timestamp":1700001800,"ph":7.3,"temperature":24.6}]}' \
  http://poolcontroller.local/history/import
```

**Réponse 200**

```json
{ "status": "success", "count": 145, "total": 339, "dropped": 12 }
```

| Champ | Sens |
|---|---|
| `count` | points **retenus** de *ce* lot (désérialisés avec succès) |
| `total` | points **détenus par l'appareil** après ce lot. En mode `replace`, c'est le nombre de points **soumis** : la fusion de doublons agrégés opérée par `importData()` peut le réduire, et ce n'est pas remesuré (il faudrait recopier ~11 Ko sur `async_tcp`). Le client conclut donc sur le **dernier** lot, qui est un `append`, dont le total est authentique. |
| `dropped` | points **évincés de l'historique** par `trimToCaps()` pendant cet appel. Toujours `0` en mode `replace`. |

> ⚠️ **`dropped` ne compte PAS les entrants refusés.** Ce sont les points **les plus
> anciens de chaque granularité** qui sortent pour tenir sous les plafonds mémoire.
> Contre-exemple à garder en tête : importer **20 points récents** dans un historique
> plein rend `dropped = 20`, mais ce sont **20 points anciens déjà présents** qui
> partent — la sauvegarde envoyée, elle, est bien entrée. Un client qui traduirait
> `dropped` par « points de votre fichier rejetés » mentirait à l'utilisateur.
>
> 📋 **Ne pas en conclure « aucun entrant n'est jamais écarté »** : la boucle de
> désérialisation fait un `continue` sur un point sans `timestamp` exploitable
> ([`src/web_routes_data.cpp:1181`](../src/web_routes_data.cpp)), et ces points ne
> sont comptés **nulle part** — `count` est le nombre de points **retenus**, pas le
> nombre de points soumis. Défaut **préexistant**, hors périmètre de `bug-101`, et
> **ouvert**.

#### ⚠️ Limite dure : 8 192 octets de corps (bug-113, v2.48.0)

**Un corps de plus de `8 192` octets reçoit `413` et n'est jamais traité**
(`kHistoryImportMaxBodyBytes`, [`src/constants.h`](../src/constants.h)). La comparaison
est **stricte** : 8 192 octets exactement passent, 8 193 non — même sémantique que la
bibliothèque (`AsyncJson.cpp:158`), pour que garde et bibliothèque ne divergent pas
d'un octet.

**Le plafond a été ABAISSÉ**, il n'a pas été relevé (il valait 16 384, le défaut jamais
surchargé de `AsyncCallbackJsonWebHandler`). Motif : le corps est **tamponné en entier
avant toute authentification** (`calloc(total + 1)`), sur **trois** routes. Le total
réclamable sans identifiants passe de **49 152** à **9 472 octets** (−80,73 %) :

| Route | Plafond | Constante |
|---|---|---|
| `POST /history/import` | **8 192 o** | `kHistoryImportMaxBodyBytes` |
| `POST /wifi/connect` | **1 024 o** | `kWifiConnectMaxBodyBytes` |
| `POST /sensors/onewire/identify` | **256 o** | `kSensorIdentifyMaxBodyBytes` |

Ces trois valeurs et leur total sont **assertés à la compilation** : une révision casse
le build au lieu de périmer cette page. Mécanisme du refus :
[`docs/subsystems/web-server.md`](subsystems/web-server.md).

**Restaurer un historique de taille quelconque** ne demande donc **aucune** capacité
supplémentaire à l'appareil : le client **découpe** et pose `mode:"append"` sur les
lots suivants. L'interface le fait seule (budget de **7 168 o** par lot,
`kHistoryImportChunkBudgetBytes`, soit 1 024 o de marge sous le plafond) ; un client
tiers doit faire de même.

> ⚠️ **Ne pas « optimiser » en découpant plus fin.** Un corps **≤ au seuil de bascule
> PSRAM** repartirait dans le **tas 8 bits interne**, celui qui est en famine : seul ce
> qui **dépasse** ce seuil voit `heap_caps_malloc_default` préférer la **PSRAM**
> ([ADR-0061](adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md),
> révisé par [ADR-0078](adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md) — seuil
> **4 095 o** jusqu'en v2.49.3, **1 024 o** depuis la **v2.50.0**).
> Le budget de 7 168 o est **au-dessus à dessein**, et cet invariant est asserté.

> **Poids d'un point, avec sa méthode** ([ADR-0073](adr/0073-chiffre-consigne-porte-sa-methode.md)) :
> **148 octets** mesurés le 2026-09-07 en exécutant `parseHistoryCsv()` et
> `chunkHistoryPoints()` de [`data/app.js`](../data/app.js) sur
> [`test/fixtures/history-worst-bytes.csv`](../test/fixtures/history-worst-bytes.csv)
> (16 chiffres significatifs, 7 clés émises, **sans** `phFiltered`/`orpFiltered`) ;
> **105-109 o** pour un point typique (`null`, une décimale) ; **172 o** au maximum
> **théorique** — squelette de clés 97 o plus trois nombres de 25 o, 25 étant la
> longueur JSON maximale d'un `double` (`-0.0000012345678901234567`, atteinte dès
> qu'une valeur passe sous `1e-6` : un ORP qui traverse zéro, une température proche
> de 0 °C).
>
> Le découpage client se fait par **octets mesurés** (`jsonByteLength`), jamais par
> nombre de points — et c'est ce qui l'a rendu indifférent à **quatre** chiffres
> successivement erronés : 211-213 o (supposait les colonnes filtrées), 149 o (un cas
> particulier, l'ORP négatif), et 157 o, qui était présenté ici comme une « borne
> conservatrice » alors qu'il s'agissait du pire cas de l'**export firmware** — une
> autre quantité, que le maximum théorique de 172 o dépasse. Vérifié par exécution :
> 200 points au pire cas donnent 5 lots, charge maximale 7 085 o, aucun 413.
>
> 🛑 **Le contournement « découper à la main, les lots se cumulent » qui figurait ici
> était FAUX ET DESTRUCTEUR** (retiré le 2026-09-07). Avant la v2.48.0, chaque lot
> **écrasait** le précédent (`memoryBuffer = dataPoints;`) : qui suivait ce conseil ne
> gardait que le **dernier** morceau, sans aucun message. Depuis la v2.48.0 le
> découpage est **sûr**, mais **uniquement** avec `mode:"append"` sur les lots suivants.

> ⚠️ **Effet de bord d'une interface en cache** : une page chargée avant la v2.48.0
> ignore le découpage et peut POSTer ~130 points (**~14 Ko**), volume qui passait sous
> l'ancien plafond de 16 384 o. Elle reçoit désormais un **413**. **Rechargez la page**
> (Ctrl-Maj-R) après la mise à jour du filesystem.

> Cette limite conditionne toute restauration après un changement de table de partitions
> (qui efface la partition `history`) — voir [ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md)
> et [UPDATE_GUIDE.md](UPDATE_GUIDE.md).

**Erreurs**

- `400` : champ `history` manquant / tableau vide / aucun point valide / JSON illisible
- `401` : authentification WRITE manquante ou invalide
- `413 {"error":"Corps JSON trop volumineux : N octets reçus, plafond 8192 octets pour cette route."}` :
  émis par la **garde maison** enregistrée avant le handler JSON, donc **en JSON** et
  non plus par la page d'erreur nue de la bibliothèque. ⚠️ Il reste rendu **avant
  `REQUIRE_AUTH`** — c'est structurel (le `Content-Length` est connu en fin d'en-têtes,
  l'authentification est vérifiée dans le callback de corps) ; le message est donc
  **générique et ne divulgue rien**. Le refus est **total**, jamais partiel : aucun
  point n'est importé.
- `503 {"error":"Mémoire insuffisante, réessayez dans quelques secondes"}` + en-tête
  `Retry-After: 5` : famine mémoire au moment de la requête (plus gros bloc 8 bits sous
  `kStaticFileMinAllocBytes`, [ADR-0066](adr/0066-sonde-du-tas-au-moment-du-refus.md)).
  Rejouer le lot tel quel — l'idempotence du mode `append` le permet.
- `500` : `importData()` / `appendData()` a échoué (historique désactivé, mutex
  historique non obtenu — dans ce cas **rien n'a été modifié**, le lot est rejouable)

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
    { "timestamp": 14400000, "level": "INFO", "message": "Capteurs initialisés", "trunc": false },
    { "timestamp": 14405000, "level": "WARNING", "message": "pH hors limites: 7.8", "trunc": false }
  ],
  "truncated": false
}
```

> `timestamp` est exprimé en millisecondes depuis le démarrage de l'ESP32 (`millis()`).
>
> **`trunc`** (booléen, ajouté en v2.52.1, feature-129) : `true` si le **message**
> de cette entrée a été tronqué à l'écriture, faute de tenir dans
> `kLogMessageMaxBytes` (768 o) — la fin du texte porte alors le marqueur
> `"... (tronque)"`. Distinct de `truncated` (niveau document, pagination) :
> `trunc` décrit **une entrée**, `truncated` décrit **la page**. Un message de
> garde chimique tronqué à 768 o reste rarissime (pire cas mesuré : ≈ 630 o) —
> voir [`docs/subsystems/logger.md`](subsystems/logger.md#arène-circulaire-du-journal-feature-129).
> ⚠️ Le flux **WebSocket** temps réel (page Logs) ne porte **jamais** ce drapeau :
> il diffuse le message intégral reçu par `log()`, avant toute troncature de
> stockage — voir [`docs/subsystems/ws-manager.md`](subsystems/ws-manager.md).

**Pagination** (v2.10.4) :

- Page de **30 entrées** (`kLogsMaxEntriesPerPage`, ~4 Ko de JSON au pire), extensible jusqu'au **plafond dur de 60** (`kLogsPageHardCap`) **uniquement** pour inclure la queue d'entrées partageant la même milliseconde que la dernière entrée incluse — le curseur `?since=` étant strictement supérieur, les exclure les perdrait définitivement entre deux pages (règles d'admission : [`src/logs_page.h`](../src/logs_page.h)).
- `truncated: true` → il reste des entrées au-delà de cette page ; le client reboucle avec `?since=<timestamp de la dernière entrée reçue>` jusqu'à `truncated: false`. Un client ancien qui ignore `truncated` reste fonctionnel (il voit simplement la première page).
- Le document JSON de la page est construit en **une seule passe sous le mutex logger**, l'échappement est délégué à `serializeJson` (l'échappement manuel a été supprimé).

**Réponse 503** (`{"error":"heap fragmenté, réessayez"}`) : la taille **réelle** de la page (`measureJson`) + `kHttpHeapMarginBytes` (4 Ko) est comparée au plus gros bloc **réellement allouable** (`heapLargestFreeBlock8Bit()` depuis v2.33.0 — l'ancienne `ESP.getMaxAllocHeap()` comptait l'IRAM 32 bits, plancher ~27 636 o, et rendait ce 503 préventif inatteignable ; [ADR-0058](adr/0058-gardes-heap-metrique-8-bits.md)) **avant** toute allocation ; sinon le stream est pré-dimensionné exactement (aucun `resize()`). Remplace la garde v2.8.2 à seuil fixe `kMinHeapForLogsResponse` (24 Ko), supprimée — un seuil constant indépendant de la taille réelle de la réponse ne protégeait pas (2 coredumps, 2026-07-23 et 2026-07-24).

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

**Réponse en flux chunked, zéro accumulation RAM** (v2.10.4, [ADR-0032](adr/0032-reponses-http-bornees.md)) : l'export est produit par une machine à états (`beginChunkedResponse`, patron `/coredump/download`) qui remplit directement le buffer TCP borné — d'abord le fichier persistant `/system.log` (lu par blocs), puis les entrées de l'arène circulaire (copiées une à une sous mutex via `Logger::copyEntryByIndex`, renommée depuis `getLogEntryByIndex` en feature-129/v2.52.1). Aucune garde heap n'est nécessaire (rien de proportionnel à la taille de l'export n'est alloué) : **plus de réponse 503** sur cette route.

Limites assumées de l'export (best-effort, route de diagnostic) :

- Lignes formatées > **256 octets** (`kLogExportLineMaxBytes`) tronquées avec suffixe « … ».
- Le ring RAM peut tourner entre deux chunks (doublon ou saut d'entrée possibles) ; un timeout du mutex logger termine la section RAM sans erreur.
- **Sous famine mémoire, l'export peut être TRONQUÉ — et il le dit** (v2.43.0, [ADR-0070](adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)). L'ouverture de `/system.log` est désormais gardée : si la mémoire ne permet pas de l'ouvrir, la route **répond toujours 200** et sert la session courante seule, précédée d'un en-tête explicite au lieu du `# === Historique persistant ===` habituel :

  ```
  # === Historique persistant INDISPONIBLE ===
  # (fichier absent, ou memoire insuffisante pour l'ouvrir :
  #  export TRONQUE a la session courante)
  ```

  Le repli existait déjà, mais il était **silencieux** — rien ne distinguait un export complet d'un export amputé. Le compteur MQTT [`xdef`](MQTT.md#hdef--ldef--xdef--refus-daccès-fichier-pour-famine-mémoire-v2430) recense ces exports tronqués. ⚠️ Un fichier **absent** produit le même en-tête qu'un **refus** : les deux ne sont pas distinguables côté route.

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
  "pool_size": "medium",
  "ph_correction_type": "ph_minus",
  "ph_target": 7.2,
  "orp_target": 700,
  "ph_regulation_enabled": true,
  "ph_pump": 1,
  "pump1_max_duty_pct": 50,
  "orp_regulation_enabled": true,
  "orp_pump": 2,
  "pump2_max_duty_pct": 50,
  "pump1_calibrated_duty": 127,
  "pump1_pulse_flow_ml_per_min": 28.0,
  "pump1_flow_weighed": false,
  "pump2_calibrated_duty": 0,
  "pump2_pulse_flow_ml_per_min": 0,
  "pump2_flow_weighed": false,
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
  "lighting_mode": "schedule",
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
  "ph_target_min": 7.2,
  "ph_target_max": 7.5,
  "ph_target_step": 0.1,
  "ph_offset_step": 0.1,
  "ph_offset_unusual_abs": 0.5,
  "ph_offset_confirm_interval_days": 30,
  "ph_offset_impasse_threshold": 1.2,
  "ph_offset_impasse_threshold_high": -0.9,
  "ph_raw_floor_absolute": 5.5,
  "ph_raw_ceiling_absolute": 8.5,
  "ph_cal_points_min": 2,
  "ph_tune_min_filtration_ms": 600000,
  "ph_offset_epoch": 0,
  "ph_cal_epoch": 0,
  "ph_offset_stale": false,
  "ph_offset_stale_cause": "",
  "ph_impasse_low": false,
  "ph_impasse_high": false,
  "boost_active": false,
  "boost_until": 0,
  "rescue_ml": 1500,
  "regulation_defaults": {
    "ph_target": 7.3,
    "orp_target": 600,
    "ph_limit_minutes": 10,
    "orp_limit_minutes": 10,
    "ph_daily_target_ml": 0,
    "orp_daily_target_ml": 0,
    "regulation_speed": "normal"
  },
  "regulation_defaults_by_size": {
    "small":  { "ph_target": 7.3, "orp_target": 600, "ph_limit_minutes": 5,  "orp_limit_minutes": 5,  "ph_daily_target_ml": 0, "orp_daily_target_ml": 0, "regulation_speed": "slow" },
    "medium": { "ph_target": 7.3, "orp_target": 600, "ph_limit_minutes": 10, "orp_limit_minutes": 10, "ph_daily_target_ml": 0, "orp_daily_target_ml": 0, "regulation_speed": "normal" },
    "large":  { "ph_target": 7.3, "orp_target": 600, "ph_limit_minutes": 15, "orp_limit_minutes": 15, "ph_daily_target_ml": 0, "orp_daily_target_ml": 0, "regulation_speed": "fast" }
  }
}
```

> ⚠️ **v2.34.0 (feature-091) — `stabilization_delay_min` RETIRÉ de `/get-config`.** Le « délai de stabilisation avant injection » n'est plus un réglage : la purge des mesures se termine sur leur **stabilité** (plancher 10 min, plafond 90 min — constantes firmware, [ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md)). Conséquences sur le contrat :
>
> - la réponse expose **7** réglages de régulation au lieu de 8 ; les objets `regulation_defaults` et `regulation_defaults_by_size` perdent la même clé (voir [`src/regulation_defaults.h`](../src/regulation_defaults.h), `struct RegulationDefaults` = 7 champs) ;
> - en écriture, [`POST /save-config`](#post-save-config--critical) **accepte silencieusement** la clé `stabilization_delay_min` d'un client ancien (ni erreur, ni effet) — même traitement que `screen_enabled` en v2.17.1 ;
> - le champ NVS `stab_delay` continue d'être lu et écrit mais reste **inerte** ([`src/config.cpp:187,463`](../src/config.cpp)) : aucune migration à faire, aucune valeur perdue si un retour arrière était nécessaire.

---

Champs notables de la réponse :

| Champ | Type | Description |
|-------|------|-------------|
| `install_mode` | string | **Mode d'installation** (feature-056, v2.19.0). `"managed"` (PoolController pilote la filtration : relais GPIO 26 piloté, programmation active, eau présente = filtration commandée ON), `"powered"` (contrôleur alimenté par le circuit de filtration : relais inerte, programmation masquée, eau présumée présente en permanence — ex-`continu`), `"external"` (filtration tierce signalée : relais inerte, eau présente seulement si dernier signal ON reçu < 180 s, sinon dosage suspendu). Remplace `regulation_mode` **et** `filtration_enabled`. Voir [ADR-0026](adr/0026-mode-installation.md). |
| `regulation_speed` | string | `"slow"` / `"normal"` / `"fast"` — préréglages PID. **v2.32.0 (feature-090)** : le champ est **inchangé dans l'API** (lecture, écriture, NVS, MQTT) mais **masqué dans l'UI** quand le firmware expose `pool_size` — la vitesse dérive de la taille du bassin et reste visible dans le dialogue de changement de taille ([ADR-0057](adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)). |
| `pool_size` | string | **v2.32.0 (feature-090)** — **Taille du bassin** : `"small"` (< 20 m³) / `"medium"` (20-60 m³) / `"large"` (> 60 m³). Sert **uniquement à présélectionner** les défauts de régulation ([ADR-0057](adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md) : elle n'entre **jamais** dans un calcul de dosage, débit ou quota). Persisté NVS (`pool_size`) ; clé absente ou illisible ⇒ **`medium`**, la colonne des défauts v2.30.0 — **migration sans effet** pour la flotte existante (aucun réglage modifié par la mise à jour seule). Champ absent sur un firmware < 2.32.0 : le client masque le sélecteur et ré-affiche la vitesse. Pas de nouvelle entité HA. |
| `regulation_defaults_by_size` | object | **v2.32.0 (feature-090)** — Les **trois colonnes** de défauts (`small` / `medium` / `large`), mêmes clés que `regulation_defaults` (**7 depuis la v2.34.0**, `stabilization_delay_min` retiré), sérialisées depuis la table unique [`src/regulation_defaults.h`](../src/regulation_defaults.h) (`kRegulationDefaultsBySize`). Lecture seule. Sert au **dialogue avant/après du changement de taille** — l'UI n'a jamais à deviner une colonne côté JS. |
| `ph_target_min` / `ph_target_max` / `ph_target_step` | float | **v2.25.0** — Plage et pas de la **consigne pH**, dérivés de `kPhTargetMin` / `kPhTargetMax` / `kPhTargetStep` ([`constants.h`](../src/constants.h)). **Lecture seule**, jamais acceptés en écriture. Servis pour que le frontend construise son contrôle segmenté **sans coder aucune borne en dur** : élargir la plage ou passer de 4 à 5 positions se fait côté firmware seul. Un lot incohérent ou absent est ignoré **en bloc** par le client, qui conserve les bornes déjà connues. |
| `ph_offset_step` | float | **v2.25.0** — Pas du pas-à-pas `−` / valeur / `+` de la carte « Correction manuelle de la mesure » (`kPhOffsetStep`, réaffecté en v2.71.0, feature-155 — il n'affiche plus un écart mais la valeur amenée par l'utilisateur). |
| `ph_offset_min` / `ph_offset_max` | — | ⛔ **SUPPRIMÉS en v2.82.0 (feature-168, décision utilisateur)** : la correction de mesure pH n'a plus de borne de réglage. Un client qui lisait ces deux champs pour construire des bornes de saisie doit les retirer — ils n'existent plus dans la réponse. |
| `ph_offset_unusual_abs` | float | **v2.82.0 (feature-168)** — `kPhOffsetUnusualAbs` (défaut **0,50**) : ancienne borne de réglage, conservée comme **SIGNAL** d'un défaut matériel possible, pas comme une limite. Au-delà, l'interface affiche un avertissement **non bloquant** ([ADR-0114](adr/0114-correction-mesure-ph-sans-borne-de-reglage.md)). |
| `ph_offset_impasse_threshold` / `ph_offset_impasse_threshold_high` | float | **v2.82.0 (feature-168)** — Seuils, **calculés** (`kPhFloorCorrected − kPhRawFloorAbsolute` = 1,20 ; `kPhCeilingCorrected − kPhRawCeilingAbsolute` = −0,90), au-delà desquels le filet sur le pH **brut** (fixe) mord **avant** que la régulation n'atteigne son seuil chimique corrigé : la régulation est dite en **impasse**, l'appareil le signale (alerte `ph_impasse`) au lieu de se fermer en silence. Voir [ADR-0113](adr/0113-filets-ph-bruts-absolus-independants-de-loffset.md). ⚠️ **Ces deux champs restent servis arrondis à l'affichage** — ne JAMAIS les recomparer à l'offset côté client pour décider d'un verdict : lire `ph_impasse_low` / `ph_impasse_high` ci-dessous à la place (correctif 2026-10-17). |
| `ph_impasse_low` / `ph_impasse_high` | boolean | **Correctif 2026-10-17** — **Verdict** d'impasse de régulation pour l'offset (`ph_offset`) **en vigueur**, calculé **par le firmware seul** (`isPhRegulationImpasseLow()` / `isPhRegulationImpasseHigh()`, [`src/ph_guard_logic.h`](../src/ph_guard_logic.h), tolérance `kPhImpasseEpsilon` comprise) et **jamais recalculé côté client** — même principe que `ph_offset_stale`, [ADR-0100](adr/0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md). Avant ces deux champs, l'écran recomparait l'offset au seuil **arrondi** servi par `ph_offset_impasse_threshold*` ci-dessus : une fausse alerte `CRITICAL` côté firmware (offset +1,20, bruit de représentation flottante de la constante dérivée) coexistait avec un simple « Avertissement » côté écran, qui comparait `1.2 > 1.2` (faux). `ph_impasse_low` porte sur `ph_minus`, `ph_impasse_high` sur `ph_plus` — un seul des deux peut être vrai selon le produit déclaré. Servi **à l'identique** par `GET /get-config` et la trame WS `config` (contrairement aux seuils bruts ci-dessus, HTTP-seuls : ce verdict change avec l'offset en vigueur, pas seulement à la compilation). |
| `ph_raw_floor_absolute` / `ph_raw_ceiling_absolute` | float | **v2.82.0 (feature-168)** — Les deux filets de sécurité pH sur le pH **brut** (`kPhRawFloorAbsolute` = **5,50**, `kPhRawCeilingAbsolute` = **8,50**), constantes chimiques **absolues** qui ne dérivent plus d'aucune borne de réglage de l'offset ([ADR-0113](adr/0113-filets-ph-bruts-absolus-independants-de-loffset.md)). Cités par les textes d'avertissement de l'écran de réglage (ADR-0100 : un seuil cité dans un texte d'interface est **servi**, jamais recopié en dur). |
| `ph_cal_points_min` | integer | **v2.71.0 (feature-155)** — Nombre minimal de points de calibration pH requis pour accepter un accord sur test manuel (`kPhTuneCalPointsMin`, défaut **2**). Consommé par l'aperçu client de `POST /tune-ph-offset` — un offset est une constante, une erreur d'étalonnage est proportionnelle : sans 2ᵉ point, elle serait figée dans la correction. **Lecture seule**, **absent de la trame WebSocket** (constante de compilation, même règle que les six bornes ci-dessus). |
| `ph_tune_min_filtration_ms` | integer | **v2.71.0 (feature-155)** — Délai minimal (ms) de filtration continue avant qu'un accord sur test manuel soit accepté (`kPhTuneMinFiltrationMs`, défaut **600000** = 10 min) : le temps nécessaire pour que l'eau testée soit celle qui passe devant la sonde. **Lecture seule**, **absent de la trame WebSocket**. Alimente le décompte affiché par la carte « Correction manuelle de la mesure » pendant le refus « filtration pas encore établie ». |
| `ph_offset_epoch` / `ph_cal_epoch` | integer (uint32) | **v2.72.0 (feature-156)** — Horodatage Unix (secondes) du dernier événement de justesse : respectivement l'instant où la correction manuelle du pH en vigueur (`ph_offset`) a été posée, et l'instant où le dernier étalonnage de la sonde pH a été validé (`CalibratePhMid`, `CalibratePhLow` **ou** `ClearPhCal` — un effacement de calibration rend la correction tout aussi caduque qu'un étalonnage classique). **`0`** = date inconnue (horloge non synchronisée au moment de l'événement, même convention que `ph_auto_manual_epoch`). ⚠️ **Ces deux champs servent À L'AFFICHAGE SEUL** — ils ne décident jamais de l'ordre entre les deux événements : c'est un compteur interne non exposé (`phOffsetSeq`/`phCalSeq`) qui fait foi, voir [ADR-0102](adr/0102-ordre-des-gestes-de-justesse-par-compteur.md). Servis **à l'identique** par `GET /get-config` et la trame WS `config`. |
| `ph_offset_stale` | boolean | **v2.72.0 (feature-156), étendu v2.82.0 (feature-168)** — **Verdict** : `true` si l'une des **trois** causes ci-dessous tient. Calculé **par le firmware seul** (accesseur unique `phOffsetStaleNow()`, [`src/config.cpp`](../src/config.cpp)), **jamais recalculé côté client** — même principe que les seuils de santé pH, [ADR-0100](adr/0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md). Pilote la bannière « Correction à revoir » de la carte « Correction manuelle de la mesure », voir [page-ph.md](features/page-ph.md#correction-devenue-caduque-ph_tune_stale_banner). Servi **à l'identique** par `GET /get-config` et la trame WS `config`. |
| `ph_offset_stale_cause` | string | **v2.82.0 (feature-168)** — N'a de sens **que si** `ph_offset_stale === true`. Trois valeurs possibles, **texte choisi par la cause** côté client (un bandeau commun avec un message différent — « les trois causes appellent trois gestes différents ») : `"calibration"` (correction antérieure au dernier étalonnage réussi, feature-156), `"expiration"` (correction inhabituelle — `\|ph_offset\| > ph_offset_unusual_abs` — non reconfirmée depuis plus de 30 jours, `kPhOffsetConfirmIntervalDays`), `"boot"` (correction inhabituelle relue en NVS au démarrage **sans confirmation utilisateur depuis** — signal de mémoire potentiellement altérée, prioritaire sur les deux autres causes). Chaîne vide si `ph_offset_stale === false`. |
| `ph_slope_healthy_min_pct` / `ph_slope_healthy_max_pct` / `ph_zero_healthy_max_mv` | float | **v2.70.0 (feature-154)** — Fenêtre de **diagnostic** EZO (défauts `90` / `110` / `15`), dérivée de `kPhSlopeHealthyMinPct` / `kPhSlopeHealthyMaxPct` / `kPhZeroHealthyMaxMv` ([`constants.h`](../src/constants.h)) — **mêmes constantes** que celles consommées par `isPhProbeHealthy()` côté firmware, qui nuance le message d'escalade de la bascule de sécurité pH. **Lecture seule**, jamais acceptées en écriture. Servies pour que l'écran « État sonde pH » calcule ses **trois pastilles** (pente acide, pente base, décalage zéro) **sans coder aucun seuil en dur** côté client — plus aucun littéral `85`/`90`/`95`/`15`/`30` dans `data/app.js` (vérifié par grep, et par un harnais de test qui assertit l'absence de tout littéral numérique hors `0`/`1` dans le texte extrait de `classifyPhCriterion()`, voir [ADR-0100](adr/0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)). ⚠️ **Purement diagnostiques — aucune de ces trois valeurs ne conditionne un dosage** : la seule décision de sécurité qui s'appuie sur cette fenêtre est `isPhProbeHealthy()`, inchangée par cette feature. **VOLONTAIREMENT ABSENTES de la trame WebSocket** (même motif que les six bornes ci-dessus) : ce sont des constantes de compilation, et `kWsFrameBufBytes` (3584 o) n'a que ~571 o de marge au pire cas — un pire cas qui **coïncide précisément** avec l'escalade de sécurité pH (trace de bascule automatique + raison en texte). Le client conserve donc les dernières bornes reçues entre deux appels à `/get-config`, sans jamais retomber sur un défaut inventé (un repli sur un seuil de *santé* innocenterait une sonde — contrairement au repli d'IHM des six bornes ci-dessus). ⚠️ **Divergence d'arrondi, bornée et non fermée davantage** (condition C4 du gate `pool-chemistry`) : la pastille juge la valeur **arrondie SERVIE** par `/data` (pentes à 1 décimale, zéro à 2 décimales — `web_routes_data.cpp`), **pas** la valeur interne lue par `querySlope()` (parsée par `strtof`, sans arrondi). Divergence théorique bornée à **< 0,05 %** (pentes) et **< 0,005 mV** (zéro) — à ces ordres de grandeur, un verdict ne peut basculer qu'à une fraction de poil du seuil exact. Ce qui a été **vérifié** : le chemin de parsing (`atlas_ezo.cpp:239-294`), le point d'arrondi unique (`web_routes_data.cpp:345-347`), le format documenté (1/2 décimales, conforme aux valeurs réelles relevées). Ce qui **ne l'a pas été** : la fiche technique Atlas Scientific, et une trame brute à d'autres ordres de grandeur. Fermeture retenue : vecteurs de test aux bornes exactes (89,9 / 90,0 / 110,0 / 110,1) sur la valeur **servie**, pas une preuve théorique supplémentaire. |
| `ph_limit_minutes` | integer | Durée max d'injection pH par fenêtre glissante d'1 h (minutes, 1–60) |
| `orp_limit_minutes` | integer | Durée max d'injection ORP par fenêtre glissante d'1 h (minutes, 1–60) |
| `max_cycles_per_day` | integer | Limite anti-cycling (garde 9, feature-060, v2.12.0) : démarrages de dosage max **par pompe et par jour**, compteur partagé régulation auto + injections manuelles. Défaut **24** (était 48 — abaissé en v2.20.5, cf. [pump-controller.md](subsystems/pump-controller.md#limite-de-cyclesjour-configurable-garde-9-feature-060-v2120)). Bornes dures **10–60** clampées côté serveur (voir la table de validation `/save-config` ci-dessous). Persisté en NVS (`max_cycles_day`) — le nouveau défaut **ne s'applique donc pas** à un appareil déjà provisionné, aucune migration n'écrase la valeur enregistrée. |
| `pump1_max_duty_pct` | integer | **v2.21.0** ([ADR-0043](adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) : **amplitude** du plateau d'impulsion pompe 1 (duty commandé pendant un pouls = `pct × 255/100`), borné **[40–100] %** côté serveur (`kPumpAmpMinPct` — sous 40 % la pompe cale). N'est plus un clamp PWM appliqué en sortie de régulation. **v2.23.0** ([ADR-0047](adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)) : **gelé à 50 % côté UI** (curseur retiré, le frontend ne l'envoie plus) — modifiable **par API seule**. |
| `pump2_max_duty_pct` | integer | Idem pompe 2. |
| `pump1_calibrated_duty` | integer | **v2.21.0** — duty auquel le débit de la pompe 1 s'applique. **v2.23.0** : le défaut usine n'est plus `0` mais **127** (calibration de type, `kPumpTypeCalDuty`) — `0` ne subsiste que par **effacement explicite** ou **clé NVS héritée à 0** et reste fail-closed (**dosage automatique/programmé REFUSÉ**). Persisté NVS `p1_cal_duty` (clé absente = défaut de type). |
| `pump1_pulse_flow_ml_per_min` | float | **v2.21.0** — débit (mL/min) au duty calibré, pompe 1. **v2.23.0** : défaut usine **28.0** (calibration de type, `kPumpTypePulseFlowMlPerMin`) ; une pesée individuelle (120 s recommandée) le remplace. Persisté NVS `p1_pulse_flow`. |
| `pump2_calibrated_duty` / `pump2_pulse_flow_ml_per_min` | int / float | Idem pompe 2 (NVS `p2_cal_duty` / `p2_pulse_flow`). |
| `pump1_flow_weighed` / `pump2_flow_weighed` | boolean | **v2.23.0** ([ADR-0047](adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)) — provenance du débit : `false` = **valeur usine** (calibration de type du modèle de pompe), `true` = **pesée individuelle** de CETTE pompe. **Purement informatif** (indication « valeur usine / pesé » de l'UI) — aucun gate de dosage ne s'y fie. Persisté NVS `p1/p2_flow_weighed` ; rabattu à `false` par l'effacement et par la garde de plausibilité au chargement NVS. **Parité trame WS `config` (correctif on-target v2.23.0)** : les 6 champs de caractérisation (`pumpN_calibrated_duty`, `pumpN_pulse_flow_ml_per_min`, `pumpN_flow_weighed`) sont présents **à l'identique** dans la trame WebSocket `config` — ils en étaient absents à la livraison initiale et le premier push écrasait l'affichage de la calibration côté UI (clé absente interprétée comme zéro). Voir [ws-manager.md](subsystems/ws-manager.md#champs-config-ajoutés-en-v2230--caractérisation-des-pompes-feature-074-correctif-on-target). |
| `ph_regulation_mode` | string | Mode de régulation pH : `"automatic"` (PID vers ph_target), `"scheduled"` (volume quotidien réparti sur 24 h), `"manual"` (aucune régulation automatique) |
| `ph_daily_target_ml` | integer | Volume quotidien cible en mL pour le mode Programmée pH (0 = désactivé, borné par `max_ph_ml_per_day`) |
| `ph_enabled` | boolean | Miroir dérivé de `ph_regulation_mode` : `true` si mode ≠ `"manual"`. Maintenu pour compatibilité MQTT / HA. ⚠️ **v2.25.0 — LECTURE SEULE** : le champ n'est **plus accepté en écriture** par `/save-config` (une clé envoyée est ignorée silencieusement). Il ouvrait un second chemin capable de rouvrir la régulation en contournant le gate d'acquittement, et de désynchroniser le couple mode/enabled en NVS. |
| `orp_regulation_mode` | string | Mode de régulation ORP : `"automatic"` (PID vers orp_target), `"scheduled"` (volume quotidien de chlore, aveugle au capteur), `"manual"` (aucune régulation automatique). ⚠️ **v2.55.0 (feature-140)** : cette valeur est **toujours la configuration NVS brute**, jamais affectée par la Chloration de secours — pendant que ce mode est actif, la **décision de dosage** peut différer de cette valeur publiée (mode ORP *effectif*, voir [ADR-0087](adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md)) ; `rescue_until`/`rescue_dosing` (`GET /data`) disent ce qui se passe réellement. |
| `orp_daily_target_ml` | integer | Volume quotidien cible en mL pour le mode Programmée ORP (0 = désactivé, borné par `max_orp_ml_per_day`) |
| `orp_enabled` | boolean | Miroir dérivé de `orp_regulation_mode` : `true` si mode ≠ `"manual"`. Maintenu pour compatibilité MQTT / HA. ⚠️ **v2.77.0 (bug-161, gate `pool-chemistry` C2) — LECTURE SEULE** : le champ n'est **plus accepté en écriture** par `/save-config` (une clé envoyée est ignorée silencieusement) — alignement d'un **écart** qui durait depuis v2.25.0 (`ph_enabled` avait fermé ce chemin, `orp_enabled` ne l'avait jamais été). Écrivable, il ouvrait un second chemin capable de réarmer `orp_enabled` en contournant le gate d'étalonnage et de désynchroniser le couple mode/enabled en NVS, symétriquement au défaut corrigé sur `ph_enabled` en v2.25.0. |
| `max_orp_ml_per_day` | float | Limite journalière ORP configurée (mL) — alias de `max_chlorine_ml_per_day` |
| `orp_cal_valid` | boolean | `true` si une calibration ORP a déjà été enregistrée (date non vide) |
| `sensor_logs_enabled` | boolean | Verbosité des logs capteurs (default `false`). Modifiable via Paramètres → Avancé → « Log des sondes ». |
| `debug_logs_enabled` | boolean | Active la production des logs de niveau `DEBUG` côté firmware (default `false`). Quand `false`, `Logger::debug()` court-circuite immédiatement (early return). Les niveaux `INFO`/`WARN`/`ERROR`/`CRITICAL` ne sont pas affectés. Modifiable via Paramètres → Avancé → « Logs DEBUG activés ». Persisté en NVS sous la clé `debug_logs`. Effet immédiat, pas de redémarrage requis. |
| `boost_active` | boolean | Mode Boost actif (feature-053, v2.18.0). `true` tant que la surchloration temporaire du jour est en cours. Piloté par `POST /boost/start` \| `/boost/stop` (jamais par `/save-config`). Persisté en NVS (survit à un reboot dans la journée), expire automatiquement au prochain minuit local. Voir [ADR-0025](adr/0025-mode-boost.md). |
| `boost_until` | integer (epoch) | Instant d'expiration du Boost (epoch UNIX, prochain minuit local). `0` si le Boost est inactif. Permet à l'UI d'afficher l'heure de fin. |
| `rescue_ml` | integer (mL) | **v2.56.0 (feature-141)** — Volume quotidien de la **Chloration de secours** : **ARMÉ** si le mode est actif, **PROPOSÉ** (chaîne de pré-remplissage) sinon. **Jamais `0`** et jamais la constante `1500` en dur — résolu par `rescueDailyMlForDisplay()` ([pump-controller.md](subsystems/pump-controller.md#chloration-de-secours-feature-140)), chaîne : `orp_daily_target_ml` (si > 0) → défaut par taille de bassin → repli final 1500 mL, puis borné par les mêmes clamps que l'activation. Pré-remplit le champ du dialogue d'activation et sert de seuil pour les libellés L5a/L5b du bandeau actif. **Parité stricte `/get-config` ↔ trame WS `config`** (même clé, même calcul, un seul lecteur firmware) — ne JAMAIS servir ce champ d'un seul côté (voir [ws-manager.md](subsystems/ws-manager.md)). |
| `ph_auto_manual_epoch` | integer (epoch) | **v2.20.3** — Instant (epoch UNIX) de la dernière **bascule automatique** de la régulation pH en mode `manual` par le filet de sécurité « refus de garde pH répétés en mode `automatic` » (feature-067, refondu en v2.25.0). `0` = aucune bascule en attente d'acquittement. Vaut `0` si l'horloge n'était pas synchronisée au moment de la bascule (la `reason` reste renseignée). Persisté en NVS (`ph_am_epoch`). **Lecture seule** : jamais accepté en écriture par `/save-config`. |
| `ph_auto_manual_reason` | string | **v2.20.3** — Cause en clair (français) de cette bascule, ex. `"3 refus de garde pH en 24 h — régulation pH basculée en manuel. Diagnostic sonde EZO NORMAL (…) : la sonde n'est pas en cause. …"` (message conditionné au diagnostic EZO depuis la v2.25.0). Chaîne vide si `ph_auto_manual_epoch` vaut `0`. Persisté en NVS (`ph_am_reason`). |

> **Trace de bascule automatique pH (v2.20.3)** — le journal système est un ring buffer : après quelques heures il ne restait **aucune** trace de l'escalade de sécurité, et il devenait impossible de dire si le mode `manual` venait du firmware ou d'une action humaine (incident du 2026-08-16). Les deux champs ci-dessus sont donc **persistés en NVS**, dans la **même transaction** que le mode lui-même.
>
> **Acquittement (v2.25.0 — fail-closed)** : la trace n'est **plus** effacée par un changement de mode. Tant qu'elle existe, `/save-config` **refuse en 409** toute sortie du mode `manual`, et la commande MQTT équivalente est refusée sans jamais effacer la trace. Le seul chemin de sortie est le champ **`ph_auto_manual_ack`** ci-dessous, envoyé par le bouton d'acquittement de la [page pH](features/page-ph.md). Clés NVS absentes sur un appareil mis à jour ⇒ `0` / `""` : rétrocompatible. Voir [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#acquittement-fail-closed-de-la-bascule-v2250).
| `ph_offset` | float | **Offset logiciel de mesure pH** (feature-067, v2.20.0). `pH corrigé = pH brut EZO + ph_offset`. Plage **[−0.50, +0.50]**, **pas de 0,10 depuis la v2.25.0** (était 0,01), défaut **0** (inerte). Persisté en NVS (clé `ph_offset`). **Ne touche jamais la calibration interne du module Atlas EZO** : c'est une correction en aval de la lecture, destinée à compenser un écart d'implantation de la sonde. Voir [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md). |
| `regulation_defaults` | object | **v2.31.0 (feature-088)** — **Valeurs d'usine** des réglages de tuning de la régulation, sérialisées depuis la source unique [`src/regulation_defaults.h`](../src/regulation_defaults.h) (mêmes clés que les champs de config correspondants). **v2.34.0 (feature-091)** : **7 clés** — `ph_target`, `orp_target`, `ph_limit_minutes`, `orp_limit_minutes`, `ph_daily_target_ml`, `orp_daily_target_ml`, `regulation_speed` (`stabilization_delay_min` retiré). **Lecture seule**. Sert au dialogue « actuel → usine » du bouton de restauration ([page Paramètres](features/page-settings.md#bouton--restaurer-les-valeurs-par-défaut--feature-088-v2310)) — le frontend ne code **aucune** de ces valeurs en dur. Objet **absent** sur un firmware < 2.31.0 : le client masque alors le bouton. **v2.32.0 (feature-090)** : le **contrat 088 est intact** (mêmes clés, à plat) mais l'objet reflète désormais la colonne de la **taille de bassin courante** (`mqttCfg.poolSize`) — le bouton de restauration remet donc les défauts **de la taille sélectionnée**. Voir [`POST /reset-regulation-defaults`](#post-reset-regulation-defaults--critical). |

#### Suivi produit — relevé du biais de comptage (feature-110, v2.45.0)

Six clés **en lecture seule** s'ajoutent aux 10 clés de suivi produit déjà servies (`*_tracking_enabled`, `*_container_ml`, `*_total_injected_ml`, `*_remaining_ml`, `*_alert_threshold_ml`). Ces six compteurs sont **remis à zéro en même temps que `*_total_injected_ml`**, dans le même bloc `*_reset_container` — jamais séparément : le rapport `V/N` n'a de sens que si les deux grandeurs couvrent la même campagne.

| Champ | Type | Description |
|-------|------|-------------|
| `ph_starts` | `uint32` | Nombre de **démarrages de pompe pH crédités** depuis le dernier « Nouveau bidon ». C'est le **`N`** de `V_compté = V_réel + N·δ` : relevé **avec** `ph_total_injected_ml`, il départage un biais de comptage **multiplicatif** d'un **déficit fixe par démarrage**. Incrément **saturant** (reste à `4294967295`, ne reboucle jamais). |
| `orp_starts` | `uint32` | Idem pour la pompe chlore. |
| `ph_anom` | `uint32` | **Hygiène du relevé** : parmi ces crédits, nombre de ticks de comptabilité **anormalement longs** (delta crédité > `kAcctDeltaAnomalyMs` = 5 000 ms). Incrément saturant. |
| `orp_anom` | `uint32` | Idem pour la pompe chlore. |
| `ph_anom_ml` | float (mL) | Volume crédité **par** ces ticks anormaux. Sert à **disqualifier** un relevé pollué, jamais à corriger le volume. |
| `orp_anom_ml` | float (mL) | Idem pour la pompe chlore. |

> ⚠️ **Les 4 clés d'anomalie sont HTTP-seules.** Elles sont **délibérément absentes** de la trame WebSocket `config` et **déclarées comme telles** dans la table `kHttpOnlyKeys` de la sentinelle de parité `test/test_native_ws_config_parity/`, avec un motif écrit et asserté : elles sont lues **une fois par campagne** (au changement de bidon) et pousser ~120 o toutes les 5 s pour zéro usage temps réel dépenserait le budget de trame ([plafond dur `kWsFrameBufBytes = 3584` o](subsystems/ws-manager.md#budget-de-trame--décompte-rectifié-en-revue)).
>
> `ph_starts` et `orp_starts`, eux, **sont** dans la trame (ils avancent en continu à côté du volume). Un client qui veut le relevé **complet** doit donc appeler `/get-config` : voir [ws-manager.md](subsystems/ws-manager.md#feature-110-v2450--compteurs-de-démarrages-de-pompe-ph_starts--orp_starts).
>
> **Aucune correction n'est appliquée par cette version** : ces compteurs **mesurent**. Le mode d'emploi du relevé — quoi noter, quand, et comment reconnaître un relevé qui ne conclut rien — est sur la [page Produits](features/page-dosages.md#relevé-du-biais-de-comptage--mode-demploi-feature-110-v2450).

**`POST /save-config` — champs pH spécifiques :**

| Champ | Type | Validation |
|-------|------|------------|
| `ph_regulation_mode` | string | Valeurs acceptées : `"automatic"`, `"scheduled"`, `"manual"`. Toute autre valeur est ignorée. Met à jour `ph_enabled` automatiquement. ⚠️ **v2.25.0** : si une trace de bascule automatique est active, toute sortie du mode `"manual"` est **refusée en 409** sans `ph_auto_manual_ack: true` (voir ci-dessous). |
| `ph_auto_manual_ack` | boolean | **v2.25.0** — Acquittement **délibéré** de la bascule de sécurité pH. **Booléen STRICT** : seule la valeur JSON `true` compte ; une chaîne (`"true"`, `"false"`, `"1"`) est **rejetée**, car ArduinoJson convertit toute chaîne en `true` et un client maladroit acquitterait une sécurité chimique par accident. Envoyé **seul** ⇒ efface la trace et **laisse le mode en manuel** (acquitter ≠ réarmer). Envoyé **avec** `ph_regulation_mode` ⇒ autorise en plus la transition. Sans effet s'il n'y a pas de trace. |
| `ph_target` | float | **v2.25.0** — Doit être dans **[7.20 ; 7.50]** (`kPhTargetMin` / `kPhTargetMax` de [`constants.h`](../src/constants.h)), **mêmes bornes pour `ph_minus` et `ph_plus`**. Hors plage, non finie ou non numérique → **HTTP 400** avec message français, **aucun** champ de la requête n'est appliqué (contrôle placé avant toute mutation). Jamais d'écrêtage silencieux. Dans la plage, la valeur est **alignée sur `kPhTargetStep` = 0.10**. Champ **absent** → consigne inchangée. Voir [Bornes de la consigne pH et de l'offset](#bornes-de-la-consigne-ph-et-de-loffset-v2250). |
| `ph_daily_target_ml` | integer | Doit être ≥ 0 et ≤ `max_ph_ml_per_day` (si configuré). Retourne HTTP 400 si la limite est dépassée. |
| `ph_offset` | float | ⚠️ **v2.82.0 (feature-168, [ADR-0114](adr/0114-correction-mesure-ph-sans-borne-de-reglage.md)) : PLUS DE BORNE DE RÉGLAGE.** Accepte **toute valeur finie de l'échelle pH** (`isfinite(offset) && \|offset\| <= 14,0`, `kPhOffsetScaleSpan`) — seule une **salubrité de domaine** est refusée (`NaN`, infini, valeur aberrante type `1e30`), pas une plage. Hors salubrité → **HTTP 400**, **aucun** champ de la requête n'est appliqué. Valeur saine → **arrondie au plus proche multiple de `kPhOffsetStep` = 0.10** (`roundPhOffsetToStep()`, **sans écrêtage** depuis feature-168). Champ **absent** ou **non numérique** → paramètre inchangé (patron des autres champs). Prise en compte **immédiate** (pas de reboot, pas d'attente de la fenêtre de lissage). Chaque changement effectif est journalisé en `INFO` : `Offset pH modifié: ancien=X.XX nouveau=Y.YY` ; au-delà de `ph_offset_unusual_abs` (0,50), un second log `CRITICAL` + alerte MQTT `ph_offset_unusual` signale une correction inhabituelle (pas un refus). |

**Corps de la réponse 400 (`ph_offset` non sain)** :

```json
{"error":"Offset pH invalide","message":"Offset pH invalide : la valeur doit être un nombre fini de l'échelle pH (±14,00). Aucune borne de réglage ne s'applique sinon — vérifiez la valeur avant de recommencer."}
```

> ⚠️ **Le message a changé de NATURE en v2.82.0** : avant feature-168, ce refus citait une **plage de réglage** (±0,50) ; il cite désormais un refus de **salubrité** (±14,00, l'échelle pH entière). Un client qui affichait l'ancien texte mot pour mot doit être mis à jour.
>
> ⚠️ **Exception assumée au patron `/save-config`** : `ph_offset` reste le **premier** champ de cette route à **refuser** la requête plutôt qu'à écrêter silencieusement la valeur (cf. `max_cycles_per_day`, clampé). Un offset écrêté serait un **mensonge de mesure**. ⛔ Depuis feature-168, `roundPhOffsetToStep()` n'écrête **plus du tout** (elle arrondissait et clampait avant ; elle arrondit seulement désormais) — réintroduire un clamp romprait silencieusement la décision utilisateur (une correction de +1,00 redeviendrait +0,50 au premier redémarrage).
>
> Effets de bord volontairement **absents** : enregistrer un offset ne réinitialise **aucun** compteur de dosage (cycles du jour, cumul journalier, pause anti-cycling). Sinon des enregistrements répétés permettraient de contourner les limites journalières. Un enregistrement pendant une injection en cours ne l'interrompt pas : le nouvel offset s'applique au prochain cycle d'évaluation.

**`POST /save-config` — champs ORP spécifiques :**

| Champ | Type | Validation |
|-------|------|------------|
| `orp_regulation_mode` | string | Valeurs acceptées : `"automatic"`, `"scheduled"`, `"manual"`. Toute autre valeur est ignorée. Met à jour `orp_enabled` automatiquement. |
| `orp_target` | float | **v2.80.0 (feature-164)** — Doit être dans **[300 ; 950] mV** (`kOrpTargetPlausibleMin` / `kOrpTargetPlausibleMax` de [`constants.h`](../src/constants.h)). Hors plage, non finie ou non numérique → **HTTP 400**, **aucun** champ de la requête n'est appliqué (contrôle placé avant toute mutation). **Jamais d'écrêtage silencieux** — même patron que `ph_target`. ⚠️ **Avant ce lot, ce champ n'était validé nulle part sur ce chemin** : une chaîne JSON se convertissait en `0` et était persistée telle quelle (chloration éteinte sans avertissement). Dans la plage, la valeur est **arrondie à l'entier le plus proche** (`kOrpTargetStep` = 1 mV — ⚠️ pas une grille de calage comme `kPhTargetStep` : un seul arrondi, aucune position imposée). Voir [Bornes de la consigne ORP](#bornes-de-la-consigne-orp-v2800). |
| `orp_daily_target_ml` | integer | Doit être ≥ 0 et ≤ `max_orp_ml_per_day` (si configuré). Retourne HTTP 400 si la limite est dépassée. |

#### Bornes de la consigne ORP (v2.80.0)

`orp_target` est validé **côté serveur** contre `kOrpTargetPlausibleMin` = **300 mV** et `kOrpTargetPlausibleMax` = **950 mV**, arrondie à l'entier (`kOrpTargetStep` = 1). **Mêmes bornes sur les deux chemins d'écriture** (`POST /save-config` et la commande MQTT `{base}/orp_target/set`, voir [MQTT.md](MQTT.md)) — avant ce lot, le chemin MQTT était **plus strict** ([400 ; 900], littéraux en dur), un resserrement sans justification propre : *une fenêtre resserrée interdirait la recalibration légitime après un changement de stabilisant (acide cyanurique)*.

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"orp_target":200}' http://poolcontroller.local/save-config
```

```json
{
  "error": "Consigne ORP hors plage autorisée",
  "message": "Consigne ORP hors plage autorisée (300 à 950 mV). En dessous, la désinfection peut devenir insuffisante ; au-dessus, le risque de sur-chloration et d'irritation augmente."
}
```

**Les bornes sont servies au client par `GET /get-config`** : `orp_target_min`, `orp_target_max`, `orp_target_step` (dérivées des mêmes constantes firmware, ADR-0100) — le frontend n'en code aucune en dur.

### POST /save-config — horaires de filtration (feature-164, v2.80.0)

| Champ | Type | Validation |
|-------|------|------------|
| `filtration_start` / `filtration_end` | string `HH:MM` | **Nouveau en v2.80.0** — avant ce lot, ces deux champs étaient affectés en **String brute**, sans passer par `timeStringToMinutes` ni aucune règle de cohérence (contrairement à `filtration_mode`, validé depuis feature-146). Format invalide → **HTTP 400**. Évaluées sur les valeurs **effectives** : un enregistrement qui ne modifie que l'un des deux champs est jugé contre l'autre, déjà en vigueur. |

- **`début == fin` → refus explicite HTTP 400 (C11, bloquant)** : cette plage faisait tourner la filtration **zéro heure par jour**, **silencieusement**, sans aucun refus ni message jusqu'à ce lot (`isMinutesInRange` avec `equalMeansAlways=false` renvoie `false` pour `start==end`).
- ⛔ **`fin < début` reste ACCEPTÉ** : `schedule_logic.cpp` (`isMinutesInRange`) traite cette plage comme **enjambant minuit** (`now >= start || now < end`) — cas d'usage réel (filtration de nuit). Ne jamais le refuser.
- **Durée programmée courte, sous 120 min (C12, non bloquant)** : **avertissement seulement**, l'enregistrement réussit. Formule tolérante au passage de minuit : `((fin − début) + 1440) % 1440`. Même texte à l'écran et au journal (`warning`).

**Corps de la réponse 400 (C11, texte imposé mot pour mot)** :

```json
{
  "error": "Heure de début et de fin identiques",
  "message": "Heure de début et heure de fin identiques (08:00) : la filtration ne démarrerait jamais. Saisissez deux heures différentes — par exemple 00:00 et 23:59 pour filtrer en continu. Une plage à cheval sur minuit (fin avant début) est en revanche acceptée."
}
```

**Avertissement C12 (journal, niveau `warning`, et affiché sur les cartes Filtration)** :

> « Filtration programmée : {X} min par jour. Sous 120 min, la circulation est trop courte pour que la régulation pH/ORP atteigne sa consigne — l'eau restera durablement à côté de la valeur visée. »

**`POST /save-config` — mode d'installation :**

| Champ | Type | Validation |
|-------|------|------------|
| `install_mode` | string | Valeurs acceptées : `"managed"`, `"powered"`, `"external"`. Toute autre valeur est ignorée (mode courant conservé). |

**`POST /save-config` — taille du bassin (feature-090, v2.32.0) :**

| Champ | Type | Validation |
|-------|------|------------|
| `pool_size` | string | Valeurs acceptées : `"small"`, `"medium"`, `"large"`. Valeur inconnue ou **égale à la taille courante** : aucune modification. Si la taille **change**, les **7 réglages de tuning** (8 avant la v2.34.0) reçoivent les défauts de la nouvelle taille (colonne de [`src/regulation_defaults.h`](../src/regulation_defaults.h)) par le **même chemin** que la restauration 088 : écritures sous `configMutex`, persistance NVS en fin de handler (clé `pool_size` incluse), retune PID **différé** via `applyRegulationSpeed()` (jamais d'écriture de coefficients depuis la tâche web ; changement effectif de vitesse ⇒ purge de l'intégrale, [ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md)). |

- **Journal avant → après** : un message compact `[Config] Taille de bassin X → Y : pH a→b ; ORP … mV ; limites … min/h ; prog. … mL/j ; vitesse …` — le segment `stab. …` a **disparu en v2.34.0** avec le réglage. Niveau **`warning`** avec mention `(dosage AUGMENTÉ)` si la nouvelle colonne **augmente la capacité de dosage** (consigne ORP ou limite horaire qui monte — passage à une taille supérieure : 5 → 10 → 15 min/h), `info` sinon (`regulationDefaultsRaiseDosing()`, prédicat 088 généralisé).
- **POST combiné** : le bloc `pool_size` est évalué **avant** le parse des clés individuelles — un corps `{ "pool_size": "large", "ph_limit_minutes": 12 }` applique d'abord les défauts de la taille **puis** les clés individuelles, qui les raffinent (sémantique de fusion JSON). C'est le chemin utilisé par l'assistant de premier démarrage (`{install_mode, pool_size}`).
- **INVARIANT** ([ADR-0057](adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)) : la taille **présélectionne** des réglages modifiables — elle n'entre **jamais** dans un calcul de dosage, de débit ou de quota.

**`POST /save-config` — protection pompes (feature-060, v2.12.0) :**

| Champ | Type | Validation |
|-------|------|------------|
| `max_cycles_per_day` | integer | Toute valeur hors bornes est **silencieusement clampée côté serveur** dans **[10, 60]** (`clampCyclesPerDay`, bornes `kCyclesPerDayMin` / `kCyclesPerDayMax` de [`constants.h`](../src/constants.h)) : la requête retourne 200, la valeur **clampée** est appliquée et persistée, et un `WARN` est journalisé (`max_cycles_per_day hors bornes : X → Y`). Le pré-clamp `min`/`max` du champ UI est purement indicatif — le serveur fait autorité. Toute modification effective est également journalisée en `WARN` (ancienne → nouvelle valeur). |

**`POST /save-config` — calibration des pompes doseuses (v2.21.0, [ADR-0043](adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md) ; sémantique v2.23.0, [ADR-0047](adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)) :**

| Champ | Type | Validation |
|-------|------|------------|
| `pump1_max_duty_pct` / `pump2_max_duty_pct` | integer | Clampés côté serveur dans **[40, 100]** (`kPumpAmpMinPct` — le plateau d'impulsion ne descend jamais sous le seuil de rotation). **Résidu rétrocompat v2.23.0** : le frontend embarqué **n'envoie plus** ces clés (amplitude gelée, curseur retiré, champ purgé des DEUX payloads de save — immunité anti-onglets-périmés), mais le serveur les accepte toujours d'un client périmé ou d'un appel API direct. |
| `pump1_calibrated_duty` / `pump2_calibrated_duty` | integer | **`0`** (= effacer la calibration : le débit associé est remis à 0, `weighed` repasse à `false`, la pompe redevient non caractérisée, dosage auto refusé) **ou [102–255]** (102 = 40 % × 255). Hors plage → **HTTP 400** explicite (patron `ph_offset` : **jamais d'écrêtage silencieux** — écrêter une pesée fausserait la comptabilité de volume). Corps : `{"error":"Duty calibré invalide : 0 (effacer la calibration) ou une valeur entre 102 et 255"}`. **v2.23.0** : le frontend n'envoie plus cette clé (formulaire à champ unique) — **résidu rétrocompat** : un duty explicite non nul reste accepté (vieux client) et pose **aussi** `weighed = true` (assumé : le drapeau est informatif, aucun gate ne s'y fie). |
| `pump1_pulse_flow_ml_per_min` / `pump2_pulse_flow_ml_per_min` | float | **]0 – 200]** mL/min (`kPulseFlowMaxMlPerMin`, plausibilité d'une doseuse de piscine — même garde qu'au chargement NVS). Hors plage / non fini → **HTTP 400** : `{"error":"Débit mesuré invalide : la valeur doit être supérieure à 0 et au plus 200 ml/min"}`. Ignoré si `pN_calibrated_duty: 0` est envoyé dans la même requête (l'effacement prime). **v2.23.0** : un débit valide reçu **SANS** clé duty est une **pesée à l'amplitude courante** — le serveur déduit `cal_duty = pumpN_max_duty_pct × 255 / 100` (∈ [102..255] par construction, l'amplitude étant clampée [40..100] plus haut dans le même handler) et pose `pumpN_flow_weighed = true`. |

Journal : pesée avec duty explicite → `INFO Pesée pompe N enregistrée : duty X, débit Y ml/min` ; pesée à duty déduit (v2.23.0) → `INFO Pesée pompe N enregistrée : duty X (déduit de l'amplitude P %), débit Y ml/min` ; effacement → `WARN Calibration pompe N effacée — pompe non caractérisée (dosage auto refusé)`.

> ⚠️ **Retiré en v2.21.0** : le champ `pump_max_flow_ml_per_min` **n'existe plus** dans `/get-config` ni `/save-config` (suppression du modèle linéaire duty↔débit, [ADR-0043](adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)). Un client qui l'envoie encore est **ignoré silencieusement** (aucun champ cible). La clé NVS `pump_max_flow` est purgée à la première sauvegarde, **sans conversion** vers les nouvelles clés : aucune valeur issue du modèle faux n'est reprise. **v2.23.0 ([ADR-0047](adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md))** : un appareil **vierge** (clés NVS absentes) reçoit la **calibration de type** (127 / 28,0 mL/min) et dose au premier flash ; seul `pumpN_calibrated_duty` **présent à 0** (effacement explicite / NVS hérité jamais calibré) reste fail-closed — **dosage automatique/programmé refusé** (badge « Débit non caractérisé — pesée requise » + `CRITICAL` throttlé).

> ⚠️ **Retiré en v2.34.0 (feature-091)** : le champ `stabilization_delay_min` **n'est plus servi** par `/get-config` (ni dans `regulation_defaults` / `regulation_defaults_by_size`). En écriture, `/save-config` l'**accepte silencieusement sans effet et sans erreur** — un client ancien, ou un assistant de premier démarrage non mis à jour, continue de fonctionner ([`src/web_routes_config.cpp:850`](../src/web_routes_config.cpp)). La clé NVS `stab_delay` reste lue et écrite mais **inerte** (précédent `screen_enabled`, feature-065). Le délai avant injection est désormais décidé par la **stabilité des mesures** : plancher 10 min, plafond 90 min ([ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md), [`docs/subsystems/measure-validity.md`](subsystems/measure-validity.md)).

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

#### Authentification du corps de requête (durcissement v2.25.0)

⚠️ **Correctif de sécurité — trou préexistant sur TOUTE la route** (pas seulement le pH). Le contrôle `REQUIRE_AUTH` n'était posé que dans le handler de **complétion**, qui s'exécute **après** le handler de corps : un `POST` **anonyme** voyait donc son corps intégralement **appliqué** (mutations de la config RAM **et** écriture NVS) avant que le `401` ne parte. Le refus n'était qu'un affichage.

Depuis la v2.25.0, le handler de **corps** vérifie l'authentification **avant** d'accumuler ou d'appliquer quoi que ce soit : un corps non autorisé est **ignoré en totalité** (aucune modification, aucun buffer alloué), un `warning` journalise l'IP d'origine, et la complétion émet le `401` comme avant.

- L'**exemption « premier démarrage »** (assistant de configuration initiale) est conservée à l'identique.
- Le quota `kMaxRequestsPerMinute` reste compté **une seule fois** par requête (le contrôle du corps ne rejoue pas le rate-limiting).

#### Codes de réponse

| Code | Cas |
|------|-----|
| `200` | Configuration enregistrée |
| `400` | Valeur invalide (**`ph_target` hors [7.20 ; 7.50]** — v2.25.0, `ph_offset` non sain (NaN/infini/\|valeur\| > 14, v2.82.0), débit de pesée hors plage, cible journalière > limite, JSON invalide…). **Aucun** champ n'est appliqué. |
| `401` | Non authentifié — **aucune** modification appliquée depuis la v2.25.0 |
| `409` | **v2.25.0** — Bascule de sécurité pH non acquittée : la requête tente de sortir du mode `manual` alors qu'une trace existe, sans `ph_auto_manual_ack: true`. **Aucune** mutation n'est appliquée (le gate est le premier bloc du handler) et la trace **survit**. ⚠️ **v2.80.0** — même code pour un changement de mode **ou de consigne** pH/ORP pendant une session d'étalonnage ouverte (voir [verrou étalonnage](#payload-mixte--quel-code-sort)). |

#### Bornes de la consigne pH et de l'offset (v2.25.0)

`ph_target` est validé **côté serveur** contre `kPhTargetMin` = **7.20** et `kPhTargetMax` = **7.50**, par pas de `kPhTargetStep` = **0.10** (4 positions), bornes **identiques** pour `ph_minus` et `ph_plus`. Avant la v2.25.0, ce champ n'était validé **nulle part** sur ce chemin (le champ UI acceptait 0-14 et la valeur était persistée telle quelle) tandis que la commande MQTT équivalente appliquait ses propres littéraux `6.0 – 8.5`.

- **Refus `400`, jamais d'écrêtage** : écrêter en silence ferait croire que la consigne saisie est appliquée alors que la régulation viserait une autre valeur.
- Le contrôle est placé **avant toute mutation** de la configuration : un refus ne laisse jamais la RAM divergente de la NVS.
- Une valeur **non numérique** est refusée elle aussi (correctif v2.25.0 : `ph_target` était lu sans conversion explicite — une chaîne JSON valait `0` et était persistée).
- **Dans** la plage, une valeur non alignée sur le pas est **alignée au plus proche** (log `info` si l'alignement change quelque chose) : l'interface ne produit que des multiples du pas, mais l'API reste ouverte.
- Une consigne hors bornes déjà présente en NVS est **clampée au démarrage** et réécrite immédiatement.

⚠️ **Changement de contrat visible** : la borne haute passe de **7.60 à 7.50**. À 7.60 pile, la consigne atteindrait le seuil d'inhibition ORP (`kPhOrpInhibit`) et le chlore serait suspendu **en permanence** dès que la régulation tient sa consigne. Une installation persistée à 7.60 est clampée à 7.50 au premier démarrage, avec avertissement journalisé. Voir [ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

```bash
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"ph_target":6.8}' http://poolcontroller.local/save-config
```

```json
{
  "error": "Consigne pH hors plage autorisée",
  "message": "Consigne pH hors plage autorisée (7,20 à 7,50). En dessous, l'eau devient agressive pour le liner, les joints et les parties métalliques ; au-dessus, la régulation du chlore serait inhibée en permanence."
}
```

⚠️ **L'offset pH n'a PLUS de borne de réglage depuis la v2.82.0 (feature-168, décision utilisateur — [ADR-0114](adr/0114-correction-mesure-ph-sans-borne-de-reglage.md)).** `ph_offset` accepte désormais **toute valeur finie de l'échelle pH** : seul un refus de **salubrité de domaine** subsiste (`isPhOffsetValid()` : `NaN`/infini/\|valeur\| > 14 pH), pas une plage de réglage. Dans la plage de salubrité, la valeur est **arrondie au plus proche multiple du pas** (`roundPhOffsetToStep()`, **0,10**, pas de clamp). Réponse 400 sur refus :

```json
{
  "error": "Offset pH invalide",
  "message": "Offset pH invalide : la valeur doit être un nombre fini de l'échelle pH (±14,00). Aucune borne de réglage ne s'applique sinon — vérifiez la valeur avant de recommencer."
}
```

⚠️ **Le message a changé de NATURE** : ce n'est plus « hors plage de réglage » (une consigne qu'on pourrait élargir), c'est un refus de **salubrité** — valeur non finie ou tellement extravagante (ex. `1e30`) qu'elle ne peut être qu'une corruption. Les installations ayant enregistré une valeur non alignée sur le pas (0.47) sont migrées au chargement NVS ; l'écart maximal introduit par l'arrondi est de 0,05 pH, soit le bruit résiduel du filtre.

**Les bornes sont servies au client par `GET /get-config`** — `ph_target_min`, `ph_target_max`, `ph_target_step`, `ph_offset_step` restent en lecture seule, dérivés des constantes firmware. ⛔ `ph_offset_min`/`ph_offset_max` **n'existent plus** (v2.82.0). Les seuils liés à la correction de mesure pH (`ph_offset_unusual_abs`, `ph_offset_confirm_interval_days`, `ph_offset_impasse_threshold`, `ph_offset_impasse_threshold_high`, `ph_raw_floor_absolute`, `ph_raw_ceiling_absolute`) sont servis séparément — voir le tableau de `GET /get-config` ci-dessus — et pilotent l'avertissement et l'alerte d'impasse de l'écran de réglage, jamais écrits en dur côté client (ADR-0100). ⚠️ **v2.80.0 : trois champs ORP équivalents** s'y ajoutent — `orp_target_min`, `orp_target_max`, `orp_target_step` (voir [Bornes de la consigne ORP](#bornes-de-la-consigne-orp-v2800)). Le frontend n'en code aucune en dur : élargir la plage ou changer le pas se fait **côté firmware seul**.

#### Payload mixte : quel code sort ?

Si une même requête cumule plusieurs motifs de refus, l'ordre d'évaluation du handler est **déterministe** et documenté :

1. authentification du corps (`401`) ;
2. **gate d'acquittement de la bascule pH** (`409`) ;
3. **verrou étalonnage** (`409`), si une session de calibration est ouverte (`mqttCfg.calSwitchActive`) — étendu en **v2.80.0 (feature-164)** : refuse désormais un changement **réel** de mode **OU de consigne** (pH ou ORP). ⚠️ **L'étalonnage d'UNE SEULE sonde verrouille les DEUX consignes** — l'appareil ne distingue pas les canaux (`calSwitchActive` est un drapeau global), acceptée par le gate sans spec de suivi (session de quelques minutes, bornée par l'alerte d'oubli) ;
4. bornes de la consigne pH (`400`) ;
5. **bornes de la consigne ORP (`400`, v2.80.0)** ;
6. **cohérence des horaires de filtration (`400`, v2.80.0)** — `début == fin` ;
7. autres validations de champs (`400`).

Donc un payload contenant **à la fois** un changement de mode refusé **et** une consigne hors bornes — par exemple `{"ph_regulation_mode":"automatic","ph_target":6.8}` sous trace de sécurité active — répond **`409`**, pas `400` : le gate de mode est évalué en premier. Dans tous les cas, **aucun** champ n'est appliqué.

**Corps de la réponse 409 (acquittement pH)** :

```json
{
  "error": "Bascule de sécurité pH non acquittée",
  "message": "Bascule de sécurité pH non acquittée — acquittez l'alerte sur la page pH avant de réactiver la régulation."
}
```

**Corps de la réponse 409 (verrou étalonnage, texte v2.80.0, imposé par le gate)** :

```json
{
  "error": "Étalonnage en cours",
  "message": "Étalonnage en cours — le mode et les consignes sont verrouillés. Le mode est rétabli à la fin de l'étalonnage, et les consignes redeviennent modifiables au même moment."
}
```

⚠️ **Texte CHANGÉ en v2.80.0** — l'ancien (« Étalonnage en cours — le mode est rétabli à la fin de l'étalonnage. », v2.77.0, ne mentionnait pas les consignes) est **périmé** : ne plus le citer, nulle part. **Un seul texte**, qu'il s'agisse d'un refus de mode ou de consigne — le refus décrit un **état** (une session ouverte), pas une action ; seul le préfixe du log (canal concerné, IP cliente) varie.

⚠️ Ce verrou **refuse**, il ne **réconcilie** jamais : il ne met **jamais** à jour la mémoire de mode antérieur à l'étalonnage (`ph/orpCalSwitchPrevMode`) — un second chemin d'écriture vers cette mémoire de sécurité serait exactement le piège qu'il ferme. Détail : [pump-controller.md](subsystems/pump-controller.md#verrou-étalonnage-sur-le-changement-de-mode-bug-161-gate-c2).

✅ **Couvert sur les QUATRE chemins d'écriture depuis v2.80.0** : `/save-config` (HTTP), les commandes MQTT du **mode** (feature-161 lot C), les commandes MQTT des **consignes**, et le recalcul induit par un changement de **taille de bassin** (`pool_size` → `applyRegulationDefaults()`). Même verrou, **même texte** partout.

⚠️ **Le refus ne s'arme que sur un changement RÉEL** — jamais sur la simple présence de la clé. L'interface renvoie systématiquement les valeurs courantes : tester la présence refuserait **toute** sauvegarde de réglages pendant un étalonnage, soit *une garde qui bloque plus qu'elle ne protège*.

⚠️ **À lire avant d'ajouter un cinquième écrivain** de `phTarget`/`orpTarget` : la garde avait d'abord été posée sur le **gestionnaire HTTP** plutôt que sur **le point où la valeur change**, et les quatre chemins ont été découverts **l'un après l'autre**. Voir [MQTT.md](MQTT.md).

Acquitter puis réarmer (deux requêtes, patron de l'interface web) :

```bash
# 1. Acquitter (efface la trace, reste en manuel)
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"ph_auto_manual_ack":true}' http://poolcontroller.local/save-config

# 2. Choisir le mode
curl -u admin:monmotdepasse -X POST -H "Content-Type: application/json" \
  -d '{"ph_regulation_mode":"automatic"}' http://poolcontroller.local/save-config
```

> Cette route journalise l'**IP d'origine** de tout changement du mode de régulation pH, sur les deux issues (accepté / refusé en 409).

---

### POST /tune-ph-offset — CRITICAL

**v2.71.0 (feature-155)** — Accorde l'appareil sur un test manuel (bandelette, PoolLab, pH-mètre) : remplace le geste « régler un écart » par « amener un nombre à la valeur lue sur son test ». Corps :

```json
{ "ph_reference": 7.45 }
```

`ph_reference` est la **valeur amenée** par l'utilisateur — **jamais un écart déjà calculé**. C'est le **firmware** qui calcule l'écart, avec **sa propre mesure** (`sensors.getPhValueRaw()`) à l'**instant de la réception** de la requête :

```
offset = roundPhOffsetToStep(ph_reference − getPhValueRaw())
```

#### Pourquoi une route dédiée plutôt qu'un champ de plus dans `POST /save-config`

1. **Sémantique différente** du champ `ph_offset` existant, qui reste le contrat de **stockage** (déjà calculé, écrit directement, par `POST /save-config`). Fusionner les deux sous la même clé ferait dériver le sens du champ selon l'appelant, invérifiable en lecture de code.
2. **Quatre refus ordonnés**, avec des textes **imposés mot pour mot**, incompatibles avec le patron différé de `/save-config` (une erreur parmi N champs, réponse posée par la complétion) : ici la réponse est **immédiate** et porte exactement le texte du refus qui s'applique.
3. **Aucun appel** à `requestReset()` / `resetDosingStates()` / `resetPhPauseGuard()` sur ce chemin : l'offset ne touche aucun compteur de dosage, aucune limite journalière ou horaire n'est contournable par des accords répétés.

#### ⚠️ Pourquoi le calcul se fait côté firmware, jamais côté navigateur

Le plan initial prévoyait un calcul côté client (`écart = ph_reference − valeur_affichée`), le navigateur envoyant directement `ph_offset`. **Refusé par le gate `pool-chemistry`** : un écart calculé par le navigateur sur une référence qui a **vieilli pendant le geste** (l'utilisateur regarde l'écran, compare à sa bandelette, hésite, clique) est **inauditable** — impossible de savoir, après coup, quelle mesure a réellement servi de base au calcul. En calculant côté serveur avec `getPhValueRaw()` lu à l'instant de la réception, le calcul est **reproductible** et sa trace de journal fait foi. L'affichage côté client (les trois nombres de la carte « Correction manuelle de la mesure ») n'est donc qu'un **APERÇU**, jamais la décision — voir [docs/features/page-ph.md](features/page-ph.md#enregistrement--post-tune-ph-offset).

⚠️ **Piège fermé par ce choix** : `ph`, `phFiltered` et `phMedian` (servis par `/data` et le WebSocket) sont **déjà corrigés** de l'offset courant. Un calcul qui partirait de l'une de ces valeurs ajouterait l'écart à un écart déjà appliqué — défaut **invisible à offset 0,00**, faux dès que l'offset ne l'est plus. Le firmware part donc exclusivement de `getPhValueRaw()` (voir le champ `phValueRaw` ci-dessous), **jamais** de `ph`/`phFiltered`/`phMedian`.

#### Les quatre refus, dans l'ordre d'évaluation (jamais réordonnés)

| Ordre | Condition | Code | Corps de la réponse |
|---:|---|---|---|
| 1 | `isnan(getPhValueRaw())` — mesure de référence indisponible (remplissage de fenêtre, sonde figée, porte fermée, péremption) | `400` | `"L'appareil n'a pas de mesure assez récente. Attendez qu'une valeur s'affiche, puis recommencez."` |
| 2 | Filtration arrêtée, **ou** en marche depuis moins de `kPhTuneMinFiltrationMs` = **600 000 ms (10 min)** — ⚠️ depuis la v2.74.0 (feature-158), cette durée peut inclure un report recrédité à travers un redémarrage logiciel court, voir [filtration.md](subsystems/filtration.md#reprise-de-la-durée-de-filtration-à-travers-un-redémarrage-feature-158-v2740) | `400` | `"La filtration doit tourner depuis 10 minutes pour que l'eau testée soit celle qui passe devant la sonde. Possible dans N min."` (N = minutes restantes, arrondi supérieur) |
| 3 | `calPoints < kPhTuneCalPointsMin` (2) — sonde non étalonnée | `400` | `"La sonde n'a pas encore ses deux points de référence. Faites d'abord l'étalonnage (tampon 7 puis tampon 4), puis revenez ici."` |
| 4 | ⚠️ **v2.82.0 (feature-168)** : Écart **demandé** (avant arrondi) hors **salubrité** de domaine (`NaN`, infini, ou \|écart\| > `kPhOffsetScaleSpan` = **14,0** pH, l'échelle pH entière) — ⛔ **ce n'est plus une borne de réglage** (`kPhOffsetMin`/`kPhOffsetMax` n'existent plus) | `400` | `"Trop loin de ce que l'appareil mesure. Refaites le test (bandelettes non périmées, lecture à la bonne échelle) et vérifiez l'étalonnage de la sonde avant de recommencer."` |

Aucune mise en cause de la sonde dans ces textes pour un écart excessif (refus 4) : le message oriente vers le test ou l'étalonnage, jamais vers un remplacement de pièce (leçon de l'incident du 2026-08-25, voir [docs/features/page-ph.md](features/page-ph.md#chip-détat-sonde-feature-024)).

⚠️ **Le refus 4 porte sur l'écart DEMANDÉ, pas sur l'écart arrondi** : `roundPhOffsetToStep()` **n'arrondit que** depuis feature-168 (elle clampait avant), donc tester la salubrité sur sa sortie la rendrait structurellement toujours vraie. Le test doit porter sur `ph_reference − getPhValueRaw()` **avant** tout arrondi.

⚠️ **Ce refus est devenu un cas extrême, quasi inatteignable en usage réel** (il fallait un écart dépassant l'ancienne borne ±0,50, condition fréquente avant feature-168 ; il faut désormais un écart dépassant ±14,0 ou non fini). L'écran de réglage ne l'affiche plus comme un refus **bloquant** côté client (il n'en reste que **trois**, voir [page-ph.md](features/page-ph.md#les-refus-bloquants-de-laccord)) : un écart dans `]0,50 ; 14,0]` est désormais **accepté**, assorti d'un avertissement non bloquant (`ph_offset_unusual_abs`, voir `GET /get-config`) — jamais d'un refus 400. Voir [ADR-0114](adr/0114-correction-mesure-ph-sans-borne-de-reglage.md).

#### Réponse `200` (accordé)

```json
{
  "success": true,
  "ph_offset": 0.3,
  "ph_value_raw": 7.13,
  "ph_reference": 7.45,
  "ph_reached": 7.43
}
```

`ph_offset` est l'écart **réellement appliqué** (après arrondi au pas de `kPhOffsetStep` = 0,10) — pas l'écart brut demandé. `ph_reached` est la valeur que l'appareil va **réellement** atteindre (`ph_value_raw + ph_offset`), **jamais** `ph_reference` : amener le nombre à 7,45 quand l'appareil lit 7,13 donne un écart brut de 0,32, arrondi à 0,30 — l'appareil atteint **7,43**, pas 7,45.

Effet immédiat (ADR-0039, inchangé) : l'offset est appliqué en sortie des accesseurs, sans attendre le remplissage d'une fenêtre de mesure. Journalisé en `info` à l'acceptation (valeur amenée, lecture brute, offset avant/après) et en `warning` à chaque refus.

#### Authentification

Même garde que `POST /save-config` : le corps est vérifié **en première instruction** du handler (`saveConfigBodyAuthorized`) — un corps non authentifié n'applique **rien**, même si le `401` global n'est émis qu'à la complétion.

#### Voir aussi

- [ADR-0101](adr/0101-la-correction-nait-d-une-mesure-jamais-d-un-ecart.md) — pourquoi l'utilisateur indique une mesure plutôt qu'un écart, et pourquoi le calcul part de la valeur brute servie par le firmware.
- [docs/features/page-ph.md](features/page-ph.md#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710) — la carte côté interface.

---

### POST /reset-regulation-defaults — CRITICAL

**v2.31.0 (feature-088)** — Restaure les réglages de tuning de la régulation pH/ORP aux **valeurs d'usine**, puis persiste en NVS. Périmètre **arbitré par `pool-chemistry`**, **7 champs depuis la v2.34.0** : `ph_target`, `orp_target`, `ph_limit_minutes`, `orp_limit_minutes`, `ph_daily_target_ml`, `orp_daily_target_ml`, `regulation_speed` (`stabilization_delay_min` retiré par la feature-091 — la purge des mesures n'est plus réglable). **v2.32.0 (feature-090)** : les valeurs restaurées sont celles de la **taille de bassin courante** (`mqttCfg.poolSize` — la taille elle-même n'est **pas** modifiée) ; sur `medium` (défaut de migration), ce sont exactement les valeurs v2.30.0 de la feature-087.

**Corps vide** — aucune valeur ne transite par le client, donc **aucun parsing JSON** : les valeurs appliquées viennent exclusivement de la source unique [`src/regulation_defaults.h`](../src/regulation_defaults.h) et ne peuvent pas diverger de celles annoncées par l'objet `regulation_defaults` de `/get-config` (qui reflète la même colonne).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/reset-regulation-defaults
```

**Réponse 200** — les valeurs appliquées (mêmes clés que l'objet `regulation_defaults` de `/get-config`) :

```json
{
  "success": true,
  "regulation_defaults": {
    "ph_target": 7.3,
    "orp_target": 600,
    "ph_limit_minutes": 10,
    "orp_limit_minutes": 10,
    "ph_daily_target_ml": 0,
    "orp_daily_target_ml": 0,
    "regulation_speed": "normal"
  }
}
```

**Codes de réponse**

| Code | Cas |
|------|-----|
| `200` | Restauration appliquée et persistée. Retune PID différé à la main loop (même hook que `regulation_speed` via `/save-config` — invariant v2.26.0), broadcast WS `config` différé (les autres clients voient les nouvelles valeurs sans reload). |
| `401` | Non authentifié — aucune modification. |
| `503` | **Mutex config occupé** (timeout) : `{"success":false,"error":"Configuration occupée, réessayez"}`. **RIEN n'est alors modifié** — la restauration est annulée **en bloc**, jamais d'application partielle. Correctif de revue v2.31.0 : la première implémentation répondait `success:true` sur ce chemin, un mensonge inacceptable sur une route CRITICAL de régulation. |

**Ce que la route ne touche JAMAIS** (invariants du visa `pool-chemistry`) : modes de régulation (`ph/orp_regulation_mode`), miroirs `ph/orp_enabled`, type de produit (`ph_correction_type`), offset pH, trace `ph_auto_manual_*`, calibration et sélection des pompes (`pumpN_*`, `ph_pump`, `orp_pump`), limites de sécurité journalières (`max_ph_ml_per_day`, `max_chlorine_ml_per_day`), `max_cycles_per_day`. Ces champs décrivent un **état physique** ou une **sécurité**, pas un réglage.

**Journal** : IP d'origine (`info`), puis **un message compact avant → après sur les 7 champs** (`[Config] Défauts de régulation (taille X) restaurés : pH a→b ; ORP … mV ; limites … min/h ; prog. … mL/j ; vitesse …`) — niveau **`warning`** si la restauration **augmente la capacité de dosage** (consigne ORP qui monte, limite horaire pH/ORP qui monte), `info` sinon. L'état complet d'avant restauration reste reconstituable depuis le journal. Voir [pump-controller.md](subsystems/pump-controller.md#restauration-des-valeurs-dusine-de-régulation-feature-088-v2310).

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
  "oom_reserve_bytes": 12288,
  "oom_reserve_internal": true,
  "oom_probe_enabled": true,
  "oom_probe_count": 11,
  "oom_probe_max_us": 84,
  "oom_probe_last_caps": 2060,
  "heap_size": 327680,
  "psram_size": 8388608,
  "psram_free": 8257536,
  "flash_size": 8388608,
  "ota_partition": "app0",
  "ota_partition_size": 3735552,
  "sketch_size": 1825717,
  "fs_total_bytes": 524288,
  "fs_used_bytes": 221184,
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

#### `oom_reserve_bytes` / `oom_reserve_internal` — état RÉEL de la réserve du parachute (v2.35.0, additifs)

| Champ | Type | Signification |
|---|---|---|
| `oom_reserve_bytes` | `uint32` | Taille **réellement armée** de la réserve d'urgence, en octets. `0` = aucune réserve |
| `oom_reserve_internal` | booléen | `true` si cette réserve réside bien en **RAM interne** (`esp_ptr_internal()` sur le pointeur publié) |

**Pourquoi deux champs et pas un** : depuis la bascule PSRAM ([ADR-0061](adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)), « la réserve vit en RAM interne » n'est plus une évidence de code mais un **invariant à vérifier** — une réserve tombée en PSRAM ne sauverait **rien** d'une famine de RAM interne, et le parachute serait décoratif sans que rien ne le signale. Mais `oom_reserve_internal` **seul est ambigu** : `false` vaut aussi bien « aucune réserve armée » que « réserve en PSRAM ». La taille rend le contrat **total et auto-portant** :

| `oom_reserve_bytes` | `oom_reserve_internal` | Lecture |
|---|---|---|
| `0` | `false` | **Aucune réserve armée** — parachute non armé (voir le journal : `Parachute OOM SANS RÉSERVE…` après 5 min, alerte MQTT `oom_reserve_absent`) |
| `12288` | `true` | **Nominal, sain** — état attendu en régime normal |
| `1024` | `true` | **Palier de repli** — le nominal a été refusé ; surclassement automatique dès que le tas le permet (WARN `Parachute OOM DÉCLASSÉ`) |
| ≠ `0` | `false` | ⚠️ **ANOMALIE** — réserve en PSRAM : **le parachute est inerte** face à une famine de RAM interne. Ne devrait jamais arriver (les trois sites d'allocation passent par `MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT` explicites) |

> ⚠️ **Croiser avec `oom_handler_registered` NE SUFFIT PAS.** Ce miroir peut transitoirement valoir `true` alors que la réserve est **nulle** (entrelacement documenté dans `oom_guard.cpp` — l'état « miroir `false` / handler présent » est auto-réparé au tick suivant, mais la fenêtre inverse existe le temps d'un tick). Pour un superviseur, la **source de vérité de l'armement** est le couple `oom_reserve_bytes` / `oom_reserve_internal`.

- **Coût** : lecture d'un atomique + une comparaison d'adresse *inline* — ni allocation, ni log, ni parcours de tas.
- **Ajout purement additif** : budget JSON `/get-system-info` mesuré à **~778 octets** pires cas (748 avant). Absents d'un firmware ≤ 2.34.0 → traiter leur absence comme « information indisponible ».

#### `oom_probe_*` — observabilité de la sonde du refus (v2.40.0, additifs)

Quatre champs **purement additifs** ([ADR-0066](adr/0066-sonde-du-tas-au-moment-du-refus.md), bug-080 lot B-2, condition 4 du gate `pool-chemistry`). Ils décrivent la **sonde du tas prise au moment exact d'un refus d'allocation** — la mesure sur laquelle repose désormais le verdict post-mortem au démarrage.

| Champ | Type | Signification |
|---|---|---|
| `oom_probe_enabled` | booléen | `false` = sonde **désactivée définitivement** (un couple de parcours a dépassé `kOomProbeMaxDurationUs` = 1 500 µs). Les trois autres champs sont alors **figés** |
| `oom_probe_count` | `uint32` | Nombre de sondes prises depuis le boot. `0` = aucun refus sondé (pas nécessairement aucun refus : ISR, plancher temporel, jeton non réarmé) |
| `oom_probe_max_us` | `uint32` | **Durée maximale observée** du couple des **deux** parcours, en µs |
| `oom_probe_last_caps` | `uint32` | Masque `heap_caps` de la dernière sonde. `2060` = `0x080C` = `MALLOC_CAP_8BIT \| MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL` |

**Protocole de lecture** — ces champs servent trois vérifications précises :

1. **La sonde ne coûte pas cher** : `oom_probe_max_us < 300` (le seuil de WARN `kOomProbeWarnDurationUs`). C'est la mesure qui valide en vol l'innocuité d'un parcours de tas exécuté **interruptions coupées** depuis un chemin d'échec.
2. **La sonde vit toujours** : `oom_probe_enabled = true`. Un `false` signifie que l'auto-limitation a frappé — chercher le WARN `Sonde OOM DÉSACTIVÉE définitivement` dans le journal, **et cesser d'interpréter les trois autres champs**.
3. **Sur quelles capacités les échecs tombent** : `oom_probe_last_caps`. `0x080C` est la signature des **11 échecs du relevé du 05/09/2026** (1 622 o refusés en `8BIT|DMA|INTERNAL`), contrainte **strictement plus restrictive** que `INTERNAL|8BIT` — c'est-à-dire une famine de **pool restreint**, et non une famine générale ni une corruption.

- **Coût** : quatre lectures d'atomiques — ni allocation, ni log, ni parcours de tas.
- **Ajout purement additif** : budget JSON `/get-system-info` mesuré à **~930 octets** pires cas (~805 avant ce quatuor). Absents d'un firmware ≤ 2.39.0 → traiter leur absence comme « information indisponible ».

> **Ce n'est pas une métrique de supervision.** Contrairement à `max_alloc_8bit`, ces quatre champs ne décrivent pas la santé du tas mais le **fonctionnement de l'instrument**. Ne pas construire d'alarme sur `oom_probe_count`, qui croît normalement en régime dégradé.

#### `log_*` — observabilité de l'arène circulaire du journal (v2.52.1, feature-129, additifs)

Six champs **purement additifs**, lecture de six atomiques `relaxed` — ni allocation, ni log (ferme la récursion `systemLogger → broadcastLog → _heapAllowsPush → systemLogger`), ni parcours de tas. Ils décrivent l'état de l'arène circulaire qui a remplacé le `std::vector<LogEntry>` de `String` du journal ([ADR-0082](adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md), [`docs/subsystems/logger.md`](subsystems/logger.md#arène-circulaire-du-journal-feature-129)).

```json
{
  "log_entries_current": 187,
  "log_arena_bytes_used": 11392,
  "log_arena_truncations": 0,
  "log_arena_evictions": 4021,
  "log_persist_overrun": 0,
  "log_arena_overlap_guard_trips": 0
}
```

| Champ | Type | Signification | Une valeur non nulle signifie |
|---|---|---|---|
| `log_entries_current` | `uint32` | Nombre d'entrées **actuellement vivantes** dans l'arène (≤ `kMaxLogEntries` = 200) | Rien d'anormal en soi — reflète simplement l'activité récente du journal |
| `log_arena_bytes_used` | `uint32` | Octets **actuellement occupés** dans `sLogArena` (≤ `kLogArenaBytes` = 16 384) | Idem — grandeur d'occupation, pas d'alarme |
| `log_arena_truncations` | `uint32` | Messages **tronqués** (dépassement de `kLogMessageMaxBytes` = 768 o) depuis le boot | Un message a dépassé le plafond — vérifier la source si le compteur croît vite ; le texte tronqué porte le marqueur `"... (tronque)"` et le champ `trunc` de `/get-logs` |
| `log_arena_evictions` | `uint32` | Créneaux évincés de l'arène depuis le boot, **tous motifs confondus** (rotation normale du ring à 200 entrées, ou pression mémoire) | Croît normalement en régime nominal (ring plein) — pas une alarme à elle seule |
| `log_persist_overrun` | `uint32` | Créneaux évincés **avant** d'avoir été persistés alors qu'ils **auraient dû l'être** — distinct de `log_arena_evictions` (condition C6, `pool-chemistry`) | Une ligne `WARNING` a été perdue pour `/system.log` sous rafale d'écriture avant que le flush périodique n'ait pu la sauver. `ERROR`/`CRITICAL` ne sont **jamais** concernés (flush synchrone) |
| `log_arena_overlap_guard_trips` | `uint32` | Déclenchements du détecteur d'invariant `logArenaWriteOverlapsLive()`, filet indépendant exécuté juste avant tout `memcpy` dans l'arène | **DOIT rester à 0 en exploitation.** Toute valeur non nulle signale que `logArenaPlan()`/`logRingEvictCount()` ont laissé passer un recouvrement de créneau vivant que ce filet a rattrapé en évinçant **tout** l'anneau — c'est une régression de la comptabilité de l'arène, à investiguer immédiatement, pas un phénomène tolérable en continu |

**Compatibilité** : absents d'un firmware ≤ 2.51.x → traiter l'absence comme « information indisponible ».

#### `net` — observabilité du watchdog de connectivité (v2.53.1, bug-134, additif)

Cinq champs **purement additifs**, regroupés dans un objet `net` — lectures
d'atomiques/`volatile` uniquement, ni allocation, ni parcours de tas. Ils
décrivent le watchdog de connectivité ([ADR-0030](adr/0030-watchdog-connectivite.md),
amendé par [ADR-0084](adr/0084-ce-qui-prouve-qu-un-reseau-est-vivant.md)) :
avant cette version, la sonde ICMP ne journalisait rien tant qu'aucune alerte
ne se déclenchait, ce qui a rendu le diagnostic de l'incident du 19/09
déductif plutôt que factuel.

```json
{
  "net": {
    "icmpEchoes": 4231,
    "msSinceEcho": 8214,
    "msSinceNetOk": 8214,
    "selfRearms": 0,
    "probeAlive": true
  }
}
```

| Champ | Type | Signification |
|---|---|---|
| `icmpEchoes` | `uint32` | Nombre total d'échos ICMP reçus de la passerelle depuis le boot |
| `msSinceEcho` | `uint32` | ms écoulées depuis le dernier écho ICMP reçu — indépendant de `msSinceNetOk` (que d'autres signaux réarment aussi) |
| `msSinceNetOk` | `uint32` | ms écoulées depuis le dernier réarmement de l'horloge de stall, toutes sources confondues (externes et auto-produites, dans la limite de leur crédit) |
| `selfRearms` | `uint8` | Crédit de réarmements **auto-produits** actuellement consommé (`kNetworkSelfRearmMax = 2`) — une preuve externe (écho ICMP, trafic applicatif) le remet à 0 |
| `probeAlive` | booléen | `true` si une session `esp_ping` est actuellement active vers la passerelle |

**Lecture** : `msSinceEcho` proche de `msSinceNetOk` et croissant sans
retomber signale une sonde qui ne reçoit plus rien — comparer au journal
(WARN « Sonde ICMP muette… » / INFO « Sonde ICMP rétablie… »). `selfRearms`
à son plafond signifie que le crédit de confiance accordé à la récupération
elle-même est épuisé : seule une preuve externe (ou un vrai reboot)
réarmera la suite. `probeAlive = false` avec WiFi associé signale une sonde
qui n'a pas encore pu démarrer (passerelle pas encore connue par DHCP).

**Compatibilité** : absent d'un firmware ≤ 2.53.0 → traiter l'absence comme
« information indisponible ».

#### Paramètre `hf` — planchers mémoire multi-masques (v2.49.0, additif conditionnel)

Le paramètre **`?hf=1`** ajoute un objet **`hf`** à la réponse (feature-122, lot
C-1a de `bug-080`). C'est un **instrument de diagnostic** : il publie les
**planchers glissants** de cinq pools de tas, l'événement qui tenait la machine
au creux, et le résultat du témoin de bascule PSRAM. **Aucune garde du
firmware ne lit ces champs** — elles restent sur `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`
(`0x0804`), cf. [ADR-0077](adr/0077-metrique-de-garde-exclut-les-pools-inatteignables.md).

```bash
curl -s -u admin:monmotdepasse 'http://poolcontroller.local/get-system-info?hf=1'
```

**Pourquoi un paramètre et pas un ajout inconditionnel** : la réponse est
sérialisée dans une `String`, donc elle réclame un **bloc contigu** du pool
8 bits. Le 08/09/2026, cette route répondait encore **200** alors que la page
statique tombait en **503** (964 o passaient avec un `maxBloc8` à 5 108 o), et le
`maxBloc8` observé descend jusqu'à **852 o**. Publier 1 741 o par défaut créerait
une bande de fragmentation où **la route de diagnostic échouerait alors qu'elle
passait avant** — l'instrument indisponible dans le régime qu'il sert à étudier.

**Deux budgets JSON, tous deux MESURÉS** avec `measureJson()` sur des valeurs
pires cas, le 2026-09-08 :

| Requête | Charge utile pire cas | Note |
|---|---|---|
| `GET /get-system-info` | **964 o** | **identique à l'octet près** à l'avant-lot (comparaison de chaînes) — aucune clé `hf`, pas même un objet vide. **Toujours 964 o en v2.51.0** : le lot B2 ci-dessous n'ajoute rien hors de `hf` |
| `GET /get-system-info?hf=1` | **1 741 o** (v2.49.1) | dont **777 o** pour `hf` — chiffre **DÉRIVÉ**, cf. l'avertissement ci-dessous. Était **1 694 o** / 730 o en v2.49.0. **Périmé depuis la v2.51.0**, cf. §Re-mesure v2.51.0 ci-dessous |

> ⚠️ **Le 777 est une somme, pas une mesure** (ADR-0073 : un chiffre consigné
> porte sa méthode). Il vaut `730 + 47`, où **730 o** est la mesure héritée de la
> v2.49.0 et **47 o** le delta des trois clés neuves, celui-là mesuré. Or une
> **reproduction indépendante** du pire cas absolu de `hf`, faite le 08/09 avec un
> harnais distinct, donne **774 o** — soit **3 o de moins**.
>
> L'écart n'est pas tranché, et **c'est la base de 730 qui est suspecte** : la
> mesure de la v2.49.0 semble avoir posé des valeurs non saturées sur au moins un
> champ. Les deux chiffres sont donc à lire ainsi : **774 o est la seule mesure
> directe** du pire cas absolu, **777 o est un majorant dérivé**. Aucun invariant
> ne dépend de l'un ni de l'autre — le plafond de route est à 8 192 o, la marge
> reste de plus de 6 Ko — mais **ne pas citer 777 comme une mesure**, et
> re-mesurer la base avant de s'en servir pour un dimensionnement.
>
> Quatrième chiffre contesté de ce lot, après 148/149 o par point d'historique,
> les ~84 µs par parcours de tas démentis à 516, et les 730 o eux-mêmes. Le motif
> est constant : **une somme héritée présentée comme un relevé**.

**Re-mesure de la v2.49.1** : les trois clés neuves (`ctxb`, `wmax`, `wmed`)
ajoutent **+47 o** pire cas, mesurés avec `measureJson()` sur des compteurs
32 bits saturés — `"ctxb":255` = 11 o, `"wmax":4294967295` = 18 o,
`"wmed":4294967295` = 18 o. **Ne pas ajouter de champ à `hf` sans re-mesurer** :
la marge entre cette charge et le pire `maxBloc8` observé (**852 o**) est ce qui
décide si la route de diagnostic reste disponible en régime dégradé.

#### Re-mesure v2.51.0 — lot B2 (fragmentation, `fb`/`ab`)

Deux clés neuves par emplacement, `fb` (`freeBlocks`) et `ab`
(`allocatedBlocks`) — voir la table des champs ci-dessous. **Méthode**
(ADR-0073, un chiffre porte sa méthode) : `measureJson()` sur un document hôte
qui reproduit fidèlement le comportement **réel** des helpers `putHeap*OrNull`
— en particulier `null` pour les masques `def`/`spi` (chemin O(régions), qui ne
mesure aucun bloc), et les octets de contexte/témoin bornés à leur largeur
réelle plutôt qu'à `0xFFFFFFFF` partout, ce qui gonflerait artificiellement une
mesure qui l'ignore.

| Requête | Charge utile pire cas | Méthode |
|---|---|---|
| sans `?hf=1` | **~964 o** | **inchangé par ce lot** (aucune ligne touchée hors du bloc `hf`) |
| avec `?hf=1` | **~1 870 à 1 876 o**, selon le harnais de mesure | `hf` seul mesure **~906 o** dans un des deux harnais |
| delta `fb`/`ab` | **+136 o** pire cas | mesuré **par différence directe** : objet `hf` avec vs sans `fb`/`ab`, toutes les autres valeurs inchangées — 3 masques sur 5 (`i8`, `dma`, `i8d`) rendent un nombre à 10 chiffres pour les deux champs, 2 masques (`def`, `spi`) rendent `null` |

L'écart de quelques octets entre les deux harnais (~1 870 o contre ~1 876 o)
tient aux **chaînes de test** utilisées par chacun (libellés, tailles de SSID
simulées) — ce n'est **pas** un désaccord sur le delta `fb`/`ab`, qui est lui
mesuré par différence directe et donc insensible à ce choix. Comme pour le
777/774 ci-dessus : ne pas trancher pour un chiffre unique faussement exact au
seul motif qu'un intervalle est moins satisfaisant à citer. Le delta `fb`/`ab`
(+136 o) est la seule des trois valeurs mesurée par différence plutôt que par
reconstruction, et c'est celle sur laquelle s'appuyer pour juger l'effet du
lot B2.

Le rapport à la marge de survie ne change pas de conclusion : le pire `maxBloc8`
observé sur le terrain reste **852 o**, très en dessous de ~1 870-1 876 o —
`?hf=1` reste un instrument de confort de temps calme, jamais un chemin
garanti en famine. C'est précisément pour cette raison que `fb`/`ab` ne sont
**pas** publiés sur les deux lignes de journal `Heap masq …` (dernier recours en
pleine famine, `snprintf` sur pile) mais seulement sur `b804` de la **référence**
`0x0804`, cf. [subsystems/system-health.md](subsystems/system-health.md#les-deux-lignes-de-journal-agrégées).

Une valeur quelconque suffit (`?hf=1`, `?hf`) : seule la **présence** du
paramètre est testée. La route reste en `RouteProtection::WRITE` — **aucune
surface non authentifiée n'est élargie**.

**Structure de `hf`** — cinq emplacements (`i8`, `dma`, `def`, `i8d`, `spi`) plus
huit champs de tête :

| Clé | Masque | Ce que le pool voit |
|---|---|---|
| `i8` | `0x0804` `INTERNAL\|8BIT` | métrique **actuelle** des gardes — **voit** une réserve DMA ⚠️ |
| `dma` | `0x080C` `INTERNAL\|DMA\|8BIT` | **classe d'échec observée** en production (ce que le WiFi demande) |
| `def` | `0x1000` `DEFAULT` | voit **aussi la PSRAM** ⚠️ — pas de « plus gros bloc » |
| `i8d` | `0x1804` `INTERNAL\|8BIT\|DEFAULT` | ignore PSRAM **et** réserve — candidate de `feature-123` |
| `spi` | `0x0400` `MALLOC_CAP_SPIRAM` | tas PSRAM — pas de « plus gros bloc » |

Champs de chaque emplacement (clés courtes : chaque champ est une allocation sur
le pool que cette route sert à diagnostiquer) :

| Clé | Type | Signification |
|---|---|---|
| `f` | `uint32` \| `null` | **libre** au dernier échantillon |
| `b` | `uint32` \| `null` | **plus gros bloc contigu** au dernier échantillon |
| `mf` / `mb` | `uint32` \| `null` | planchers **depuis le démarrage** (libre / plus gros bloc) |
| `wf` / `wb` | `uint32` \| `null` | planchers **de fenêtre** (réarmés à chaque trace agrégée de 30 min) |
| `cf` / `cb` | `uint8` | bits de **contexte au creux** de `mf` / `mb` (voir table ci-dessous) |
| `n` | `uint32` | échantillons pris pour **ce** masque (diffère d'un masque à l'autre : tour de rôle — `dma` et `i8d` sont visités **un échantillon sur quatre** depuis la v2.49.1, soit **une résolution de 8 s**) |
| `fb` / `ab` | `uint32` \| `null` | (v2.51.0, lot B2) **nombre de blocs libres / alloués** au **dernier échantillon** — pas un plancher, **aucun minimum** n'est tenu pour ces deux compteurs. `f / fb` donne la taille **moyenne** d'un trou : la grandeur qui manquait pour distinguer, à `f` constant, un morcellement modéré (peu de gros trous) d'une pulvérisation (beaucoup de petits) |
| `a` | `uint32` \| `null` | (feature-127, R1) **octets réellement alloués** (`total_allocated_bytes`) au **dernier échantillon** — même discipline que `fb`/`ab` : instantané, aucun plancher tenu, sous-produit du **même** `heap_caps_get_info()` (aucun parcours de tas supplémentaire). `null` pour `def`/`spi`, même cause O(régions) que `fb`/`ab` |

> ⚠️ **Convention de nommage des clés `hf.<masque>.*`** : **lettre seule = un
> compte d'OCTETS** (`f`, `a`) ; **lettre + `b` = un compte de BLOCS** (`fb`,
> `ab`). `a` a été choisi pour fermer cette symétrie implicite (jusque-là
> seulement entre `f`/libre et `fb`/blocs-libres). Tout champ futur de `hf` doit
> respecter cette règle. Détail : [`docs/subsystems/system-health.md`](subsystems/system-health.md#ce-qui-est-retenu-par-emplacement).

**Pourquoi `a` existe** : la fenêtre de mesure du 12-13/09/2026 a montré qu'à
**nombre** de blocs quasi constant (883 → 907 sur 28 relevés, oscillant), le
**libre** a perdu ~10 400 o qui ne sont jamais revenus — signe que les blocs
alloués pèsent en moyenne **plus lourd** qu'au départ, sans pouvoir dire si
c'est un jeu de travail légitimement plus gros ou une fuite en octets.
`total_allocated_bytes` était déjà calculé par `heap_caps_get_info()` et jeté,
exactement le défaut que le lot B2 avait corrigé un champ plus loin (`fb`/`ab`).
`libre + alloué` reste stable à l'échelle du tas, aux métadonnées de
l'allocateur près : c'est le calcul de contrôle de toute lecture de `a`. Voir
[`docs/incidents/2026-09-10-recidive-fragmentation-v2500.md`](incidents/2026-09-10-recidive-fragmentation-v2500.md).

**Budget JSON, MESURÉ** (2026-09-14, `measureJson()` par différence directe,
objet `hf` avec vs sans `a`, toutes les autres valeurs inchangées) : le champ
`a` ajoute **+63 o** pire cas à `hf` seul (3 masques sur 5 rendent un nombre à
10 chiffres, 2 masques `def`/`spi` rendent `null`). Sans `?hf=1` : **964 o**,
**inchangé à l'octet près** par cet ajout (aucune ligne touchée hors du bloc
`hf`). ⚠️ Le chiffre antérieur du lot B2 (906 o pour `hf` seul, `fb`/`ab`
compris) n'a pas été recopié : le même harnais a été ré-exécuté pour ce lot et
restitue 906 o avant `a`, 969 o après — le delta mesuré est donc **+63 o**, pas
une estimation (ADR-0073).

**Compatibilité** : la clé **`a`** est **absente d'un firmware ≤ 2.51.0** —
même traitement que `fb`/`ab` : un client doit lire son absence comme
« information indisponible », à ne pas confondre avec un `null` de masque
`def`/`spi` (mesurée, et le masque ne peut pas la rendre).

Champs de tête :

| Clé | Type | Signification |
|---|---|---|
| `n` | `uint32` | échantillons multi-masques depuis le démarrage |
| `pred` | `uint16` | 12 bits de la table de sélection, **attendu `749` (0x02ED)** — ⚠️ voir l'avertissement ci-dessous |
| `ctx` | `uint8` | **OU** des contextes vus **dans la fenêtre courante** — ⚠️ **réarmé toutes les 30 min**, cf. ci-dessous |
| `ctxb` | `uint8` | **OU** des contextes vus **depuis le démarrage** — **jamais réarmé** (v2.49.1) |
| `wmax` | `uint32` | **pire** durée observée d'un parcours de tas `0x0804`, en µs, depuis le démarrage (v2.49.1) |
| `wmed` | `uint32` | médiane **APPROCHÉE** de cette durée, en µs — à lire « **≤ wmed** », cf. ci-dessous (v2.49.1) |
| `hg` | `uint32` | évaluations du filtre de la garde 503 (≈ GET servis), monotone |
| `w` | `uint8` | bits **bruts** du témoin de bascule PSRAM |
| `w96` / `w95` | booléen \| `null` | verdict **tri-état** : `true` = allocation tombée en **RAM externe**, `false` = **interne**, `null` = **aucun verdict**. ⚠️ **Sémantique RELATIVE au seuil depuis la v2.50.0**, voir ci-dessous |
| `wdis` | booléen \| `null` | les deux API de classement d'adresse se contredisent |

> ⚠️ **`w96` / `w95` : les clés ne bougent pas, leur sémantique devient RELATIVE
> au seuil** (v2.50.0, feature-124 / [ADR-0078](adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md)).
>
> | Clé | Ce qu'elle rapporte | Taille allouée | Attendu |
> |---|---|---|---|
> | `w96` | la **plus petite classe déportée** par la politique | `kPsramExtmemThresholdBytes + 1` (**1 025 o**) | `true` (PSRAM) |
> | `w95` | le témoin de **contrôle** | `kPsramExtmemThresholdBytes` (**1 024 o**) | `false` (interne) — un `true` est une **anomalie à instruire** |
>
> Les noms `w96` / `w95` datent du seuil de 4 095 o, où ces deux tailles valaient
> **4 096** et **4 095** par coïncidence de calibrage : l'ancien couple est le **cas
> particulier** de la forme relative, à l'ancien seuil.
>
> **Pourquoi les clés sont CONSERVÉES malgré des noms devenus trompeurs** : le relevé
> de référence a été pris **le 09/09/2026, avant** le changement de seuil. Renommer les
> clés — ou déplacer les bits de `w` — rendrait les relevés **avant / après**
> incomparables **sans rien casser par ailleurs**, c'est-à-dire de la façon la plus
> silencieuse possible. La comparabilité de la mesure prime donc sur l'élégance du nom ;
> un renommage serait un changement d'API, hors du périmètre de ce lot. Les sept
> positions de bits de `w` et le câblage des deux clés sont épinglés par un test natif.
>
> ⚠️ **`w96 = true` ne prouve pas que la politique agit sous charge** : le témoin
> s'exécute **au démarrage, PSRAM quasi vide**. Il dit où tombe une allocation à cet
> instant-là, pas où elle tombe en pleine rafale, quand la préférence `extmem` peut se
> replier silencieusement en interne.

Bits de contexte (`cf`, `cb`, `ctx`, `ctxb`) — **purement observationnels**,
aucune décision du firmware ne les relit :

| Bit | Événement |
|---|---|
| `0x01` | au moins un client WebSocket connecté |
| `0x02` | des GET servis depuis l'échantillon précédent |
| `0x04` | au moins une pompe doseuse sous tension |
| `0x08` | rafale de découverte Home Assistant en cours ou récente (fenêtre 60 s) |

**Deux `null` à ne pas confondre** ([ADR-0072](adr/0072-absence-de-valeur-hors-du-domaine.md)
— l'absence de valeur ne se représente jamais par une valeur du domaine) :

| Cas | Champs concernés | Lecture |
|---|---|---|
| **Aucune mesure** | n'importe lequel | ce plancher n'a pas encore d'échantillon (`n = 0`, ou masque jamais visité par le tour de rôle) |
| **Masque incapable de rendre un bloc** | `b`, `mb`, `wb` de `def` et `spi` **uniquement** | ces masques prennent la branche O(régions) : leur « plus gros bloc » n'est **pas mesuré**. Il ne vaut **jamais** `0`, et jamais 4 194 240 — la sentinelle interne n'est pas publiée |
| **Masque incapable de rendre un nombre de blocs** (v2.51.0) | `fb`, `ab` de `def` et `spi` **uniquement** | même branche O(régions) que ci-dessus, mais **sentinelle distincte** : `heap_caps_get_free_size()` ne calcule aucun compte de blocs. La sentinelle interne (`kOomProbeBlockCountUnknown` = `0xFFFFFFFF`) n'est **jamais** publiée telle quelle et ne vaut **jamais** `0` non plus — un `0` se lirait comme « aucun trou », un mensonge favorable |
| **Masque incapable de rendre des octets alloués** (feature-127) | `a` de `def` et `spi` **uniquement** | même branche O(régions), même sentinelle que `fb`/`ab` (elle marque la cause — le chemin O(régions) — pas le type de grandeur publiée) |
| **Aucun verdict de témoin** | `w96`, `w95`, `wdis` | pas de PSRAM, `malloc()` refusé, ou témoin jamais exécuté. Un `false` y serait un verdict inventé |

> ⚠️ **`ctx` est une FENÊTRE, `ctxb` est un CUMUL DEPUIS LE DÉMARRAGE — et la
> confusion a déjà produit un faux résultat.** Au relevé du 08/09/2026, `ctx`
> valait **1** après **8 h 49** de fonctionnement, ce qui se lisait comme « un
> seul régime jamais rencontré en 8 h 49 ». C'était **faux** : `ctx` est remis à
> zéro par le réarmement de fenêtre (toutes les 30 min, à chaque trace agrégée)
> et ne portait donc que la fenêtre courante. `ctxb`, ajouté en v2.49.1, ne se
> réarme **jamais**.
> **Pour « quels régimes cet appareil a-t-il connus », lire `ctxb`. Pour « que
> s'est-il passé depuis le dernier report », lire `ctx`.**

> ⚠️ **`wmed` est une BORNE SUPÉRIEURE, pas une médiane exacte.** La durée des
> parcours est classée dans un histogramme à **7 seaux dyadiques** (32 / 64 / 128
> / 256 / 512 / 1024 µs, puis hors échelle = `65535`), sans tri ni allocation. La
> valeur rendue est la **borne supérieure du seau contenant le rang médian** :
> elle **surestime**, d'un biais borné par la largeur du seau. À écrire
> « ≤ `wmed` », jamais « = ». `wmax`, lui, est exact.
> Repère relevé le 08/09/2026 (12 673 échantillons) : `wmax = 516`,
> `wmed = 512`. Ces deux champs étaient **mesurés depuis la v2.49.0 mais non
> publiés** par la route — ils n'étaient lisibles que dans le journal.

> ⚠️ **`pred` NE PORTE AUCUNE INFORMATION D'EXÉCUTION.** La valeur est
> *constant-folded* par le compilateur (l'objet contient littéralement
> `movi a8, 0x2ed`) : elle est **constante par build**. Ne **jamais** la lire
> comme une observation du tas — ce qui la protège est un
> `static_assert(heapFloorsPredMask() == 0x02ED)` côté firmware, c'est-à-dire le
> **build**, pas la réponse HTTP. Un `pred ≠ 749` signifie seulement qu'on
> interroge un firmware dont la table de masques a changé.

- **Coût côté firmware** : `hf` ne lit **que des atomiques déjà rangées** —
  aucun parcours de tas sur le chemin de la requête (ce serait poser quatre
  mesures interruptions coupées sur `async_tcp`, au cœur de la famine étudiée).
- **Ce n'est pas une métrique de supervision.** Ne construire aucune alarme sur
  ces champs : ils décrivent l'**instrument** et le régime de fragmentation, pas
  la santé courante. Pour la supervision, `max_alloc_8bit` reste la grandeur de
  référence.
- **Compatibilité** : le paramètre `hf` et l'objet `hf` sont **absents d'un
  firmware ≤ 2.48.1** — un client doit traiter cette absence comme
  « information indisponible ». Sans le paramètre, la réponse d'un firmware
  2.49.0 est **rigoureusement identique** à celle de la version précédente.
  Les clés **`ctxb`, `wmax` et `wmed`** sont **absentes d'un firmware ≤ 2.49.0** :
  leur absence signifie « information indisponible », et **`ctx` ne doit pas y
  être lu comme un cumul depuis le démarrage** — il n'en existe pas.
  Les clés **`fb` / `ab`** (lot B2, fragmentation) sont **absentes d'un firmware
  ≤ 2.50.0** : même traitement, « information indisponible » — ne pas les
  confondre avec un `null` de masque `def`/`spi`, qui, lui, signifie « mesurée,
  et le masque ne peut pas la rendre ».
  La clé **`a`** (feature-127) est **absente d'un firmware ≤ 2.51.0** : même
  traitement que `fb`/`ab`.
- **Protocole de relevé** (quoi déclencher, dans quel ordre, comment lire chaque
  champ, et les deux réserves de lecture) :
  [subsystems/system-health.md](subsystems/system-health.md#protocole-de-relevé-sur-cible).
  Le **chemin de dernier recours en pleine famine** n'est pas cette route mais
  les deux lignes de journal `Heap masq …` (`snprintf` sur pile, aucun bloc
  contigu requis).

> **⚠️ `psram_free` CHANGE DE SENS en v2.35.0.** Jusqu'ici la PSRAM était libre à ~99 % en permanence et sa valeur ne bougeait pas. Depuis la bascule des allocations de plus de 4095 o ([ADR-0061](adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md)), **la PSRAM est un pool de travail** : `psram_free` **varie avec la charge web** (chaque fichier statique servi y prend son tampon stdio de 4096 o). Une baisse de plusieurs dizaines de Ko pendant un chargement d'interface est le comportement **attendu**, pas une anomalie. Corollaire pour un superviseur : ne pas alarmer sur une baisse de `psram_free` ; la garde TLS sortante, elle, exige depuis la v2.81.0 un **plus gros bloc PSRAM contigu** ≥ 40 960 o (`kOtaTlsMinPsramLargestBlockBytes`, [ADR-0112](adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md) — amende [ADR-0036](adr/0036-psram-requise-mise-a-jour-en-ligne.md), qui exigeait 1 Mo de PSRAM libre **totale**, un critère aveugle à la fragmentation et jamais réellement exploité par mbedtls avant ce correctif).

> `sketch_size` (octets, `ESP.getSketchSize()`) et `ota_partition_size` (octets, taille de la partition app active) permettent de calculer l'occupation flash du firmware — affichée sur la ligne « Firmware (flash) » de la carte Infos système (v2.5.2). `sketch_size` ajouté en v2.5.2 (feature-047).
>
> **Valeurs de référence depuis le layout v5** (v2.42.0, [ADR-0068](adr/0068-partitions-layout-v5-flash-8-mo.md)) : `flash_size = 8388608`, `ota_partition_size = 3735552` (3648 Ko), `fs_total_bytes = 524288` (512 Ko). Ces trois champs sont le **moyen de vérifier depuis l'extérieur qu'un appareil est bien passé en table v5** : un `flash_size` à `4194304` ou un `ota_partition_size` à `1835008` signale une table v4 encore active — c'est-à-dire un flash USB qui n'a pas été fait.
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

**Plafond de corps : 1 024 octets** (`kWifiConnectMaxBodyBytes`, bug-113, v2.48.0) —
au-delà, `413` en JSON maison, **avant** toute authentification, comme sur
`/history/import` (section « Limite dure : 8 192 octets de corps »). Dimensionné
sur la charge réelle : `ssid` ≤ 32 o (802.11) + `password` ≤ 63 o (WPA2-PSK), soit
**58 o** pour un corps nominal et **595 o** au pire cas *tout échappé* en `\uXXXX`
(×6 par octet, clés et ponctuation comprises) — **1,7×** de marge. ⚠️ **Pas 512 o** :
cette route porte le flux de **premier démarrage**, celui qu'on ne peut pas contourner,
et un SSID UTF-8 pathologique y serait refusé.

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

> ⚠️ **Authentification — aucune trame avant authentification réussie (bug-081, v2.57.0)**.
> Quand `auth_enabled = true`, la connexion s'ouvre mais **aucun message n'est émis** tant
> que le client n'a pas envoyé `{"type":"auth","token":"..."}` avec un token valide (comparé
> à temps constant, voir [auth.md](subsystems/auth.md)) — ni `sensor_data`, ni `config`, ni
> `log`. Jusqu'en v2.56.x, un client connecté sans s'authentifier recevait quand même toutes
> les trames dès qu'**un autre** client légitime était connecté (divulgation en lecture :
> mesures pH/ORP, états de dosage, volumes injectés — voir [CHANGELOG](../CHANGELOG.md) et
> [ws-manager.md](subsystems/ws-manager.md#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall)).
> **Comportement préservé quand `auth_enabled = false`** : tout client connecté à `/ws` est
> autorisé d'emblée, comme avant.
>
> **Changement observable pour un client en excès** : au-delà de `kWsMaxClients` clients
> *authentifiés* simultanés, la connexion est **fermée** par le firmware plutôt que servie
> en silence — voir [ws-manager.md](subsystems/ws-manager.md#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall).

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

Champs ajoutés en feature-024 (pente sonde pH) — depuis la v2.21.2, les 4 mêmes champs (mêmes règles) sont aussi dans [`GET /data`](#get-data--write) :

| Champ | Type | Description |
|-------|------|-------------|
| `phSlopeAcid` | float \| null | Pente acide EZO en % (1 décimale, idéal 100). `null` si jamais lu, bus dégradé ou EZO injoignable. |
| `phSlopeBase` | float \| null | Pente base EZO en % (1 décimale, idéal 100). `null` mêmes conditions. |
| `phSlopeZero` | float \| null | Décalage zéro EZO en mV (2 décimales, idéal 0). `null` si firmware EZO ancien ne le rapporte pas. |
| `phSlopeAgeMs` | integer \| null | Millisecondes depuis la dernière query `Slope,?` réussie. `null` si jamais lue depuis le boot (cohérent avec `phSlope*` nullables). L'UI considère l'état stale au-delà de 36 h. |

Champs ajoutés en feature-025 (lissage mesures + régulation P temporisée), **mis à jour en
v2.68.0 (feature-152)** — voir l'amendement complet plus haut :

| Champ | Type | Description |
|-------|------|-------------|
| `phRaw` | float \| null | Dernière mesure pH **brute** Atlas (3 décimales). `null` si stale / jamais lue. |
| `phMedian` | float \| null | Médiane glissante pH — **diagnostic seul** depuis v2.68.0. `null` si pas de donnée. |
| `phFiltered` | float \| null | VALEUR OFFICIELLE pH (médiane 9 + lissage τ=45 s) — identique au champ `ph`, exposée aussi ici pour diagnostic. `null` pendant la montée en régime, sonde figée ou mesure périmée. |
| ~~`phFilterReady`~~ | ~~boolean~~ | **RETIRÉ en v2.68.0** — `isnan(phFiltered)` suffit. |
| ~~`phFilterUnstable`~~ | ~~boolean~~ | **RETIRÉ en v2.68.0** — la médiane écarte les aberrations sans jugement séparé. |
| `phRejectedCount` | integer | Nombre de mesures pH rejetées (compteur glissant 0..255) — **diagnostic seul**, ne décide plus rien. |
| `orpRaw` / `orpMedian` / `orpFiltered` | float \| null | Équivalents ORP (mV, sans décimale). |
| ~~`orpFilterReady`~~ / ~~`orpFilterUnstable`~~ | ~~boolean~~ | **RETIRÉS en v2.68.0** — équivalents ORP. |
| `orpRejectedCount` | integer | Équivalent ORP, diagnostic seul. |
| `phFilterAlarm` / `orpFilterAlarm` | boolean | **v2.20.3** — `true` quand le filtre est resté non prêt > 5 min alors que la filtration tournait **et** que les mesures étaient représentatives → plus aucune régulation. Jamais armé pendant une purge, un warmup ou une filtration à l'arrêt. Voir [`ws-manager.md`](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2203-alarme-filtre-bloqué). |
| `tempFrozenAlarm` | boolean | **v2.47.1** (feature-068), fenêtre portée à 24 h en **v2.56.1** (bug-142) — `true` quand la sonde de température « eau » est **figée** (43 200 lectures valides dans une bande < 0,03 °C, ≈ 24 h ; était 900 / 30 min). **Informatif pour le dosage** : aucun effet sur le dosage ni sur la compensation `RT` ([ADR-0074](adr/0074-temperature-figee-statu-quo-instrumente.md)). Gate en revanche le **planning de filtration** (`filtration.cpp:162` et `:405`). Levée immédiate. |
| `phMixingDelayActive` | boolean | `true` pendant la pause mélange hydraulique pH (15 min après injection). |
| `orpMixingDelayActive` | boolean | `true` pendant la pause mélange hydraulique ORP (20 min après injection). |
| `phDoseBlockedReason` | string \| null | Dernière cause de refus de `canDose(0)` **ou d'une garde de sécurité pH**, en **texte français** destiné au journal et à l'affichage (ex `"filtre capteur en cours de remplissage (stabilisation)"`, `"pause mélange en cours"`, `"pH sous le plancher de sécurité (6.7) — dosage acide suspendu."`). **v2.65.0** (feature-149) : les causes de garde pH citent désormais le **seuil** (constante, une décimale), jamais la mesure — une cause latchée se fige à l'instant du déclenchement et une mesure y deviendrait fausse ; voir [`pump-controller.md`](subsystems/pump-controller.md#règle--une-cause-latchée-ne-porte-jamais-une-mesure-vive-v2571). `null` si dosage autorisé. **v2.25.0** : champ ajouté à `/data`, en **parité stricte** avec le WebSocket (voir ci-dessous). ⚠️ **Ne jamais analyser ce texte pour en déduire une catégorie** — utiliser `phDoseBlockedCode` (voir [Code de cause de refus](#code-de-cause-de-refus-de-dosage-v2251)). |
| `orpDoseBlockedReason` | string \| null | Équivalent ORP (ex `"pH brut en butée haute (butée 8.1) — dosage chlore suspendu"`, `"pH trop élevé (seuil 7.6) — dosage chlore suspendu"`). **v2.65.1** (feature-150) : les deux causes de garde ORP qui citent un seuil **pH** passent elles aussi à une décimale — même règle que `phDoseBlockedReason` ci-dessus, désormais identique sur les deux voies (`kPhCeilingRawHard` s'affichait `8.10` côté ORP et `8.1` côté pH pour la même constante). |
| `phDoseBlockedCode` | integer \| null | **v2.25.1** — **CODE MACHINE** de la cause de refus du dosage pH. `null` = aucun refus. Table des valeurs et sémantique : [Code de cause de refus](#code-de-cause-de-refus-de-dosage-v2251). |
| `orpDoseBlockedCode` | integer \| null | **v2.25.1** — Équivalent ORP. |
| `phDoseBlockedForS` | integer \| null | **v2.54.0** (feature-138) — **DURÉE**, en **secondes**, depuis laquelle `phDoseBlockedCode` est inchangé. `null` **identique à la règle du code** : aucun refus en cours — ce qui couvre aussi bien « plus de blocage » que « jamais dosé depuis le boot ». `0` est une valeur **valide** (un refus qui vient de commencer), à ne pas confondre avec `null`. Clampée à **86400** (24 h) : au-delà, la durée réelle n'est plus portée par ce champ, l'interface affiche « ≥ 24 h ». Remise à zéro à **tout** changement du code, y compris un retour à « aucun refus ». |
| `orpDoseBlockedForS` | integer \| null | **v2.54.0** — Équivalent ORP. |
| `phRawGuardBlocked` | bool | **v2.25.0** — `true` quand une **butée de sécurité sur le pH BRUT** mord pour le produit pH déclaré (`6,20` en `ph_minus`, `8,10` en `ph_plus`). Consommé par l'UI pour avertir **avant** une injection manuelle, qui contourne délibérément ces butées. |
| `orpPhRawGuardBlocked` | bool | **v2.25.0** — `true` quand la butée haute sur le pH BRUT (`8,10`) inhibe le dosage chlore. |

> ⚠️ **Exigence de PARITÉ STRICTE `/data` ↔ WebSocket.** Ces huit champs existent sur les **deux** canaux, avec le même nom, la même valeur et les mêmes règles de nullité. Les quatre premiers ont été ajoutés à `/data` en v2.25.0 (ils n'existaient que sur le WebSocket, alors que l'UI les consomme depuis les deux sources : `loadSensorData()` charge `/data` comme une trame WS) ; les deux `*DoseBlockedCode` sont publiés **d'emblée sur les deux canaux** en v2.25.1, dans le même passage de sérialisation ; les deux `*DoseBlockedForS` rejoignent la règle en v2.54.0, également publiés d'emblée des deux côtés. C'était la **troisième récidive** du même motif dans le projet — pentes de sonde en 2.21.2, calibration de pompe en 2.23.0, indicateurs de garde en 2.25.0. Règle désormais explicite ([ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md), rappelée par [ADR-0049](adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md)) : **tout champ consommé par l'interface doit exister sur les deux canaux**, et le client doit rester inerte quand il manque (clé absente ≠ `false`, clé absente ≠ `null`). Voir [ws-manager.md](subsystems/ws-manager.md).
>
> ⚠️ **`phDoseBlockedForS` / `orpDoseBlockedForS` sont `/data`-seuls sur `ph_integral` / `ph_integral_term`, pas l'inverse** — précision utile pour qui lit ce paragraphe vite : les deux durées ci-dessus sont bien servies **des deux côtés**. Ce sont `ph_integral` et `ph_integral_term` (voir la trame `sensor_data`) qui, eux, ont été retirés de la **seule** trame WebSocket pour financer le budget d'octets de cet ajout ; ils restent publiés sur `/data`, MQTT et Home Assistant. Détail et chiffrage : [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2540--durée-du-refus-de-dosage-feature-138-et-élagage-de-ph_integralph_integral_term), [ADR-0086](adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md).
>
> ⚠️ Les quatre champs de garde (`*DoseBlockedReason`, `*RawGuardBlocked`) **gèlent** sur leur dernière valeur pendant les sorties anticipées de la boucle de dosage (filtration arrêtée, OTA, mode maintenance, stabilisation post-calibration) : la surveillance n'y tourne pas, car en eau stagnante la mesure n'est pas représentative. Voir [pump-controller.md](subsystems/pump-controller.md#surveillance-inconditionnelle-séparée-de-la-décision).

#### Code de cause de refus de dosage (v2.25.1)

`phDoseBlockedCode` / `orpDoseBlockedCode` transportent la cause de refus sous forme d'**entier**, à côté du texte. Motif : l'interface web déduisait auparavant la catégorie du refus en cherchant des **fragments de texte** dans la phrase française. Deux défauts de production en sont résultés — « EZO indisponible » affiché pendant ~6 min au démarrage sur une sonde saine (le mot « indispo » du libellé de warmup correspondait), et une **vraie panne d'EZO** affichée « Stabilisation du filtre » (le libellé `FilterNotReady` mélangeait « warmup » et « EZO injoignable » dans une seule phrase). Voir [ADR-0049](adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md).

> ⚠️ **CONTRAT DE SÉRIALISATION FIGÉ.** Ces valeurs sont celles de `enum class DoseRefusal` ([`src/dosing_logic.h`](../src/dosing_logic.h)), écrites **explicitement** dans l'énum. Elles ne sont **jamais renumérotées** et **jamais insérées au milieu** : toute cause nouvelle s'ajoute **à la fin**, et une cause retirée du firmware reste **réservée**. **19** `static_assert` (**18** avant feature-163 ; **23 avant v2.68.0** — six retirés avec les codes `4`, `5`, `19`-`22`, un ajouté par feature-163) verrouillent la correspondance valeur par valeur dans [`src/pump_controller.cpp`](../src/pump_controller.cpp).

| Code | Cause | Registre |
|---:|---|---|
| `null` / `0` | Aucun refus en cours — dosage autorisé | — |
| `1` | Watchdog inactif | panne |
| `2` | Pas de présence d'eau (`resolveWaterPresent`, selon le mode d'installation) | normal |
| `3` | Valeur officielle NaN, **état de filtre indéterminé** (libellé mis à jour en v2.68.0 — « lecture filtrée » devient « valeur officielle », même code, même sémantique) | indéterminé |
| ~~`4`~~ | **RETIRÉ en v2.68.0** (feature-152) — la garde séparée « filtre non prêt » a disparu, le code `3` qualifié par `15`-`18` suffit. Jamais recyclé. | — |
| ~~`5`~~ | **RETIRÉ en v2.68.0** — « capteur instable » (rejets consécutifs / latch EMI) : le mécanisme a disparu, verdict `pool-chemistry` (ce latch se posait sur du bruit normal). Jamais recyclé. | — |
| `6` | Calibration insuffisante | panne |
| `7` | Stabilisation post-calibration en cours | normal |
| `8` | Pause mélange en cours | normal |
| `9` | Mode de régulation ≠ `automatic` | normal |
| `10` | Limite journalière atteinte | normal |
| `11` | Limite horaire atteinte | normal |
| `12` | Limite cycles/jour atteinte | normal |
| `13` | Anti-rafale (1 min) | normal |
| `14` | Anti-rafale (15 min) | normal |
| `15` | Valeur officielle NaN — **fenêtre de lissage en cours** | normal |
| `16` | Valeur officielle NaN — **sonde figée** | panne |
| `17` | Valeur officielle NaN — **plus aucune mesure acceptée** | panne |
| `18` | Valeur officielle NaN — **mesures non représentatives** (purge / eau à l'arrêt) | normal |
| ~~`19`~~-~~`22`~~ | **RETIRÉS en v2.68.0** — qualifiaient la garde séparée « filtre non prêt » (code `4`), qui a disparu avec elle. Jamais recyclés. | — |
| ~~`23`~~ | **N'A JAMAIS ÉTÉ LIVRÉ** — envisagé pour `bug-160` (volet 2) au moment du mécanisme de bail, abandonné avant tout déploiement. Jamais recyclé. | — |
| `24` | **AJOUTÉ en v2.83.0 (feature-163)** — session de calibration ouverte (`calibration_session_open`). Voir [tableau des codes de refus manuel](#codes-de-refus-409-v260) et [pump-controller.md](subsystems/pump-controller.md#garde-rang-4--session-détalonnage-ouverte-tous-modes-feature-163-adr-0115). Prochaine valeur disponible : `25`. | normal |
| `250` | Cause posée par une **garde de sécurité pH/ORP** ou par l'escalade | panne |

Les codes `15`-`18` **qualifient** la cause générique `3` : quatre états physiquement distincts
derrière un même `isnan(valeur officielle)`, discriminés sur l'**état interne du filtre**
(jamais sur la fraîcheur de la lecture brute, qui reste fraîche sur une sonde figée). Avant la
v2.68.0, les codes `19`-`22` qualifiaient de façon symétrique une garde séparée « filtre non
prêt » (code `4`) ; cette garde a disparu avec la refonte de la chaîne de mesure (feature-152) —
**une seule** garde (`isnan`) suffit désormais, donc une seule famille de qualifications. Le
code `3` subsiste et signifie explicitement « le firmware ne sait pas » (état non collecté). Le
code `250` (`kDoseBlockedCodeGuard`) couvre les messages porteurs de **valeurs runtime** (seuils,
mesures), non énumérables : le client se rabat alors sur `*DoseBlockedReason`.

**Règles de consommation client** (sémantique stricte : clé absente ≠ `null` ≠ `0`) :

| État du champ | Interprétation attendue |
|---|---|
| `0` ou `null` | Aucun refus, **affirmé** par le firmware |
| clé **absente** | Information indisponible (firmware antérieur à la v2.25.1) → **seul** cas de repli sur `*DoseBlockedReason`, affiché tel quel, sans déduire de catégorie de son contenu |
| `250` | Citer le texte de `*DoseBlockedReason` |
| code **non répertorié** | Afficher un aveu d'ignorance (« Cause inconnue (code N) ») — **jamais** le libellé voisin |

Champs ajoutés en feature-011 (répartition scheduled, v2.8.0 — [ADR-0021](adr/0021-repartition-scheduled.md)) :

| Champ | Type | Description |
|-------|------|-------------|
| `ph_scheduled_flow_ml_per_min` | float \| null | Débit moyen **planifié** restant du mode Programmée pH (mL/min, 1 décimale) = volume restant à injecter / minutes de filtration restantes (bornées à minuit). `null` si le mode pH n'est pas `scheduled`, hors plage de filtration, ou heure locale ESP32 invalide — l'UI affiche alors « — ». |
| `orp_scheduled_flow_ml_per_min` | float \| null | Équivalent ORP (chlore). Mêmes conditions de nullité. |

#### Champs du terme intégral pH (v2.26.0)

Ajoutés par la **levée de `Ki = 0`** sur la régulation pH ([ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md)). Publiés en **parité stricte** avec le WebSocket (mêmes noms, mêmes arrondis, mêmes règles de nullité) et avec MQTT.

| Champ | Type | Description |
|-------|------|-------------|
| `ph_integral` | float \| null | Intégrale accumulée par la régulation pH, en **pH·s**, **signée** (1 décimale). Une valeur négative n'est pas une anomalie : elle indique de quel côté de la consigne l'accumulation se trouve. `null` si la valeur n'est pas finie. |
| `ph_integral_term` | float \| null | Contribution du terme intégral au **débit moyen demandé**, en mL/min (3 décimales) = `ki × ph_integral`. Bornée par construction à l'**autorité** `kPhIntegralAuthorityMlPerMin` = **6,0 mL/min**, quelle que soit la vitesse de régulation. Vaut `0` tant que `Ki` est nul. |
| `ph_integral_pct` | float \| null | Remplissage de l'anti-windup, en **% de `integralMax`** (1 décimale), **valeur absolue** — une saturation négative est tout aussi anormale. `null` si `integralMax ≤ 0`. |

**Aucun équivalent ORP** : `Ki_ORP` reste à 0 (asymétrie voulue, hors périmètre de l'ADR-0052).

> ⚠️ **Ne pas recalculer `ph_integral_pct` côté client.** `integralMax` **varie avec la vitesse de régulation** (43 200 / 16 200 / 7 200 pH·s pour `slow` / `normal` / `fast`) et n'est pas publié : le firmware sert le rapport déjà calculé. Le recalculer exigerait de recopier une constante firmware dans le client — motif éliminé quatre fois dans ce projet.

**Règles de consommation client** (sémantique stricte : clé absente ≠ `null` ≠ `0`) :

| État du champ | Interprétation attendue |
|---|---|
| nombre fini, `0` compris | Valeur mesurée, **affirmée**. `0 %` signifie « réserve vide », pas « inconnu ». |
| `null` | Le firmware dit explicitement « non calculable » (terme intégral inactif) → afficher « — », **jamais** un zéro. |
| clé **absente** | Information indisponible (firmware antérieur à la v2.26.0) → masquer l'affichage entier. Afficher « 0 % » mentirait sur une régulation dont on ne sait rien. |

Interprétation : voir [pump-controller.md — Terme intégral pH](subsystems/pump-controller.md#terme-intégral-ph--levée-de-ki--0-v2260). Un `ph_integral_pct` proche de 100 % **durablement** signale que la régulation demande son maximum sans atteindre la consigne — l'alerte MQTT `ph_integral_saturated` en est la forme autoritative (elle seule tient compte de la **durée**).

> **Budget de payload** : +**73 o** au pire cas sur `/data` (`"ph_integral":-43200.0,` 23 o + `"ph_integral_term":-6.000,` 26 o + `"ph_integral_pct":100.0,` 24 o). Charge pire cas portée à **1 947 o**, gabarit documentaire `StaticJson<2304>` inchangé. Côté WebSocket, le gabarit passe de 2560 à **2688** (voir [ws-manager.md](subsystems/ws-manager.md)).

#### Champs du feedforward pH (feature-085, v2.28.0)

Ajoutés par la **dose de base apprise** ([ADR-0055](adr/0055-feedforward-appris-additif-plafonne.md)). Publiés en **parité stricte** sur `/data` **ET** sur la trame WebSocket `sensor_data` (mêmes noms, mêmes arrondis, mêmes règles de nullité — même passage de code), ainsi que sur MQTT.

| Champ | Type | Description |
|-------|------|-------------|
| `ph_ff_ml_per_min` | float \| null | Débit de base appliqué à la session de dosage pH **automatique** en cours, en mL/min (**2 décimales**). Vaut `0` hors session (le feedforward ne vit que sous une session ouverte). `null` si la valeur n'est pas finie (indisponible). |
| `ph_ff_status` | int | Code d'état `PhFfStatus`, **contrat figé 0-4** (jamais renuméroté) : `0` inactif (aucune session automatique) · `1` actif · `2` retenu par le latch de sécurité (pH sous consigne, mesure invalide ou réglages modifiés) · `3` en apprentissage (historique de charge < 3 jours exploitables) · `4` quota journalier (≥ 60 % de la limite consommés). Les états 2-4 sont servis **même hors session** : ils disent pourquoi le feedforward ne s'appliquera pas. |

**Règles de consommation client** : clé `ph_ff_status` **absente** = firmware antérieur à la v2.28.0 → masquer l'affichage entier. Un code **non répertorié** (firmware plus récent) → affichage neutre (« — »), jamais le libellé voisin. La valeur `ph_ff_ml_per_min` ne se lit qu'au statut `1` (partout ailleurs elle vaut 0 par construction).

Interprétation : [pump-controller.md — Dose de base apprise](subsystems/pump-controller.md#dose-de-base-apprise--feedforward-ph-v2280). Affichage : [page-ph.md](features/page-ph.md#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460).

> **Budget de payload** : +**43 o** au pire cas sur `/data` (`"ph_ff_ml_per_min":123.45,` 26 o + `"ph_ff_status":4,` 17 o). Charge pire cas portée à **2 020 o** (1 977 mesurés v2.26.0 + 43), marge 284 o sous le gabarit documentaire `StaticJson<2304>`, inchangé. Côté WebSocket : voir [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2280--feedforward-ph-adr-0055).

#### Champs de la limite d'actionneur pH (feature-086, v2.29.0)

Ajoutés par l'**exposition de la limite d'actionneur pH** ([ADR-0056](adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)). Publiés en **parité stricte** sur `/data` **ET** sur la trame WebSocket `sensor_data` (mêmes noms, mêmes arrondis, mêmes règles de nullité — même passage de code). **Lecture seule stricte** : aucun de ces champs n'influence une décision de dosage. MQTT hors périmètre de ce cycle (budget flash).

| Champ | Type | Description |
|-------|------|-------------|
| `ph_sustainable_ml_per_min` | float \| null | Débit moyen **soutenable** de la voie pH (mL/min, **2 décimales**) = débit d'amplitude calibré × limite horaire configurée. `null` si la pompe n'est pas caractérisée ou la valeur non calculable — l'UI affiche l'indisponibilité, rien n'est inventé. |
| `ph_daily_max_ml` | int \| null | Maximum journalier **délivrable** (mL/j, entier) = soutenable × minutes **programmées** de filtration du jour. Modes Auto/Programme (`auto`/`manual`) : minutes de la plage `start`/`end`. Mode Marche (`force`, **feature-146, v2.63.0**) : 1440 (la pompe tourne en permanence). `null` si le soutenable est indisponible **ou** si le planning ne fait pas foi (mode Arrêt `off`, plage invalide). |
| `ph_limited_by_hourly_budget` | bool | La régulation pH est **bornée par la limite horaire en ce moment** : verdict de saturation C5 (épisode ouvert, hystérésis firmware sur la **demande**) **ET** mesure pH valide. La conjonction évite de servir un verdict **figé** sur panne de sonde — « borné par la limite » reste distinct de « bidon vide » et de « panne de sonde ». |
| `ph_undersized` | bool | **Sous-dimensionnement suspecté** de l'actionneur : ≥ 3 journées calendaires **consécutives exploitables** délivrées à ≥ 80 % du maximum délivrable **ET** anomalie C5 encore ouverte **ET** mesure pH valide (même garde que le champ précédent). Information de **diagnostic d'installation**, pas une anomalie de fonctionnement. |

**Règles de consommation client** : `ph_limited_by_hourly_budget` **absent** (non booléen) = firmware antérieur à la v2.29.0 → masquer l'affichage entier (capacités et bandeaux). Les quatre champs sont servis dans le **même passage** des deux côtés : une trame ne peut pas être partielle sur ces clés, donc aucun clignotement possible.

Interprétation : [pump-controller.md — Limite d'actionneur pH](subsystems/pump-controller.md#limite-dactionneur-ph--exposition-et-alerte-de-sous-dimensionnement-feature-086-v2290). Affichage : [page-ph.md](features/page-ph.md#capacités-de-dosage-et-bandeaux-de-limite-dactionneur-feature-086-v2290).

> **Budget de payload** : +**119 o** au pire cas sur `/data` (`"ph_sustainable_ml_per_min":123.45,` 35 o + `"ph_daily_max_ml":1439986,` 26 o + `"ph_limited_by_hourly_budget":false,` 36 o + `"ph_undersized":false,` 22 o). Charge pire cas portée à **2 139 o** (2 020 v2.28.0 + 119), marge 165 o sous le gabarit documentaire `StaticJson<2304>`, inchangé. Côté WebSocket : voir [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2290--limite-dactionneur-ph-feature-086).

#### Champs de composition du débit d'injection (feature-092, v2.46.0)

**Huit** champs rendant lisible l'équation que la régulation applique déjà — *demande = dose de base apprise + écart courant + rattrapage accumulé, bornée* — plus **quatre** champs de **pause mélange** et **deux** booléens de **caractérisation de pompe** qui n'étaient servis que d'un seul côté et le sont désormais **des deux**. Tous sont publiés en **parité stricte** `/data` ↔ trame WebSocket `sensor_data` (mêmes noms, mêmes arrondis, mêmes règles de nullité, **même passage** de code, entre les marqueurs `PARITE-DATA` — [ADR-0071](adr/0071-parite-data-trame-capteurs.md)). **Lecture seule stricte** : aucun de ces champs n'influence une décision de dosage.

| Champ | Type | Description |
|-------|------|-------------|
| `ph_ff_learned_ml_per_min` | float \| null | Dose de base **apprise** (mL/min, **2 décimales**) = `min(charge médiane 7 j, 60 % du débit soutenable)`. Calculée **à la lecture**, donc vraie **hors session** — à ne pas confondre avec `ph_ff_ml_per_min`, qui vaut `0` hors session. `null` sous 3 journées exploitables ou pompe non caractérisée. `0,00` avec 3 journées sans injection est une **observation légitime**, pas une indisponibilité. |
| `ph_p_term_ml_per_min` | float \| null | Écart courant `kp·e` (mL/min), **SIGNÉ** — négatif = mesure sous la consigne. Vaut `0` dans la bande morte, et n'est **jamais borné** : c'est une grandeur pédagogique. `null` si la mesure n'est pas représentative (filtration arrêtée, purge, NaN) **ou** si `kp = 0` (régulateur inerte : un `0,00` s'y lirait « pH à la consigne » sur une régulation morte). |
| `ph_i_term_ml_per_min` | float \| null | Rattrapage **servi** (mL/min), `0` dans la bande morte. |
| `ph_demand_ml_per_min` | float \| null | Demande **totale bornée** (mL/min) = `clamp(PI servi + FF, 0, débit d'amplitude de la pompe)`. La borne basse porte sur le **couple** `p + i`, jamais sur la somme des trois termes. |
| `orp_p_term_ml_per_min` | float \| null | Écart courant chlore, mêmes règles. |
| `orp_demand_ml_per_min` | float \| null | Demande ORP. Régulation **P-pure** (`Ki = 0`, aucun feedforward) : la demande **vaut** le terme proportionnel borné. Les deux champs sont servis malgré la redondance apparente — c'est ce qui rend la carte ORP lisible avec le même composant que la carte pH. |
| `orp_sustainable_ml_per_min` | float \| null | Débit moyen **soutenable** de la voie ORP (mL/min, 2 décimales) = débit d'amplitude calibré × limite horaire ORP. Miroir de `ph_sustainable_ml_per_min`. `null` si la pompe n'est pas caractérisée. |
| `orp_limited_by_hourly_budget` | bool | **Verdict** firmware « la voie chlore est bornée par sa limite horaire **en ce moment** » — homologue d'`ph_limited_by_hourly_budget`, qui n'avait pas d'équivalent ORP alors que c'est la voie qui rencontre le cas **tous les jours** sur cet appareil. Vrai si la fenêtre horaire est **épuisée** *ou* si la demande atteint le soutenable (même phénomène, constaté plus tôt). Faux si la limite est désactivée (≤ 0 ou ≥ 60 min/h) et faux quand la mesure n'est pas représentative (même fail-closed que le pH). Jamais `null`. |

> **`ph_i_term_ml_per_min` est la contribution réellement servie (bande morte appliquée) ; `ph_integral_term` est le terme PI brut, conservé pour MQTT et l'historique. Les deux diffèrent exactement dans la bande morte.**

**Quatre champs de pause mélange, désormais servis sur `/data`** (ils étaient **WS-seuls**, donc un affichage qui s'en sert **clignotait**) : `phMixingDelayActive`, `orpMixingDelayActive` (bool) et `ph_mix_remaining_s`, `orp_mix_remaining_s` (uint32, secondes restantes). Sémantique **inchangée** côté WebSocket.

**Deux booléens de caractérisation de pompe, désormais servis sur la trame WS** : `ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized` étaient `/data`-**seuls**, et le push WebSocket **effaçait** donc la clé de l'objet client (`latestSensorData` est remplacé en entier, sans fusion). Sémantique inchangée côté `/data`.

**Sémantique de `ph_ff_status = 3` précisée** : le code `3` (« en apprentissage, historique < 3 jours ») n'est **jamais** servi si l'anneau de charge porte **≥ 3 journées exploitables**. Il est alors traduit en `0` (inactif) par l'accesseur, **avant** toute autre garde. Le défaut corrigé : entre l'archivage de minuit et le premier tick de feedforward du matin, l'interface annonçait « en apprentissage » pendant sept heures alors que la dose **était** apprise.

> ⚠️ **Il n'existe AUCUN champ « appliqué », et il n'en existera pas.** `ph_applied_ml_per_min` / `orp_applied_ml_per_min` **n'existent pas**. Un client qui veut `min(demande, soutenable)` peut le reconstruire — **mais ce nombre n'est pas ce que la pompe délivre.** Le tick borne la demande à l'**amplitude de la pompe** (26,0 mL/min sur l'appareil de référence), **jamais** au débit soutenable ; la limite horaire agit **en aval**, par **refus**. Avec la configuration vivante (10 min/h), une demande de 5,20 mL/min est réellement servie **à 5,20 mL/min de moyenne jusqu'à épuisement du budget horaire**, puis 0. Afficher `min(demande, soutenable)` comme un débit servi **sous-estimait de 17 %** ce que le bassin reçoit dans les dix minutes suivantes. C'est pourquoi le total de la carte est la **Demande**, et le plafond descend en note sous son vrai nom (« apport moyen possible … **en moyenne sur l'heure** »).

> **`orp_limited_by_hourly_budget` est un DIAGNOSTIC, pas une alarme.** Verdict `pool-chemistry` : à CYA **35 mg/L**, une boucle chlore qui sature son quota quotidien est **cohérente avec le coût de la protection UV** (chlore libre visé 2,5-3 mg/L). Ce n'est **pas** une anomalie à corriger en montant la consigne ou le gain — la tentation naturelle devant ce champ est exactement l'inverse de la bonne réponse. Un écart résiduel permanent est normal sur une régulation **P-pure**.

**Règles de consommation client** : `ph_demand_ml_per_min` **absent** (`undefined`) = firmware antérieur à la v2.46.0 → masquer l'affichage entier, ne rien inventer. Un `null` se lit « le firmware dit explicitement *non calculable* » → afficher « — », **jamais** un zéro. Les grandeurs **lentes** (`ph_ff_learned_ml_per_min`, `*_sustainable_ml_per_min`) restent servies quand la mesure n'est pas représentative : elles sont vraies même quand l'eau ne circule pas.

**Fenêtre résiduelle connue** : `update()` a **trois** sorties anticipées — OTA en cours, mode maintenance, assistant de mesure du débit — qui reviennent **avant** le point de publication de la composition **sans** invalider `measures_valid`. Pendant ces trois états, `/data`, le WebSocket et MQTT servent la **dernière** composition connue avec des mesures déclarées valides. Fenêtre **bornée et déclenchée par l'opérateur** (aucune n'est spontanée) — documentée, non corrigée dans ce cycle. Voir [pump-controller.md](subsystems/pump-controller.md#composition-du-débit-dinjection--accesseurs-de-lecture-seule-feature-092-v2460).

Interprétation firmware : [pump-controller.md](subsystems/pump-controller.md#composition-du-débit-dinjection--accesseurs-de-lecture-seule-feature-092-v2460). Affichage : [page-ph.md](features/page-ph.md#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460), [page-orp.md](features/page-orp.md#carte--débit-dinjection-orp--orp-card-dosing-feature-092-v2460). MQTT : [MQTT.md](MQTT.md#topics-et-entités-ajoutés-en-v2460-composition-du-débit-dinjection).

> **Budget de payload** : +**265 o** au pire cas structurel sur `/data` pour les 8 champs de composition, +**122 o** pour les 4 champs de pause mélange. Ce chemin n'a **aucun plafond dur** (`sendJsonResponse` dimensionne la `String` à la demande) ; le gabarit documentaire reste indicatif. **Côté WebSocket, la marge sous le seul plafond dur `kWsFrameBufBytes = 3584` o tombe à 137 o** — puis à **67 o** en v2.47.1 (`bug-082`, +70 o) : c'est là que se joue tout ajout futur, voir [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2471--parité-de-flow_assist-bug-082).

Champs ajoutés en feature-056 (mode d'installation, v2.19.0 — [ADR-0026](adr/0026-mode-installation.md)) :

| Champ | Type | Description |
|-------|------|-------------|
| `install_mode` | string | Mode d'installation actif : `"managed"` / `"powered"` / `"external"`. |
| `water_present` | boolean | Présence d'eau résolue (`resolveWaterPresent`), **source unique** des gardes de dosage. `managed` → filtration commandée ON ; `powered` → toujours `true` ; `external` → signal ON récent (< 180 s). |
| `filtration_state_stale` | boolean | Mode `external` uniquement : `true` si un signal a déjà été reçu mais qu'il est périmé (> 180 s) → dosage suspendu (fail-safe). |
| `filtration_ext_known` | boolean | Mode `external` : `true` dès qu'au moins un signal externe a été reçu depuis le boot (`false` = « aucun signal », fail-safe OFF). |
| `filtration_ext_age_s` | integer | Âge en secondes du dernier signal externe (`0` si `filtration_ext_known` est `false`). |

> ⚠️ **v2.55.0 (feature-140) — `filtration_state_source` et `filtration_ext_on` RETIRÉS de cette trame WebSocket.** Aucun consommateur dans `data/` (vérifié par grep), ils financent le budget d'octets de la Chloration de secours ([ADR-0086](adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md)) et migrent **`/data`-seuls** — voir [`GET /data`](#champs-ajoutés-en-feature-140-chloration-de-secours). **Changement observable pour tout client WebSocket tiers** qui lirait ces deux clés : il faut désormais les lire via `GET /data`.

#### Objet `flow_assist` — deux champs en parité (bug-082, v2.47.1)

La trame `sensor_data` porte désormais un objet `flow_assist` **réduit à deux champs**, aux **mêmes noms, mêmes types et mêmes règles** que sur [`GET /data`](#get-data--write) :

```json
"flow_assist": { "aborted": false, "flow255": [74.0, 74.0] }
```

- `aborted` (bool) — **garde-fou de dosage**, pas un affichage : la dernière marche de mesure a été interrompue **par le serveur** (perte de filtration). Il était **neutralisé en pratique** avant la v2.47.1, la trame WS écrasant `latestSensorData` avant la saisie du volume.
- `flow255` (array[2]) — débit majorant à duty 255. **Tableau de 2, jamais un scalaire** : un client doit traiter « clé absente », « tableau vide » et « 0 » comme **trois cas distincts**, et un scalaire comme une **absence**.

**Les sept champs de session** (`active`, `pump`, `phase`, `remaining_s`, `purges_left`, `purge_s`, `measure_s`) restent servis par **`/data` seul** — l'UI force un `GET /data` à l'ouverture de l'assistant, seul moment où ils sont utiles. Un client **ne doit pas** déduire d'un `flow_assist` sans `active` que la session est terminée : **clé absente ≠ session finie**.

> **Budget** : +**70 o** au pire cas structurel (méthode et décompte : [ws-manager.md](subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2471--parité-de-flow_assist-bug-082)), 50 o en fonctionnement. La marge sous le seul plafond dur `kWsFrameBufBytes = 3584` o tombe de **137 à 67 o**.

### Message `config` — contrat de parité avec `/get-config` (v2.39.0, bug-089)

Le message `type: "config"` est un **snapshot complet et idempotent** — il n'est jamais différentiel, et il n'est pas non plus un miroir exact de [`GET /get-config`](#get-get-config--write).

> **Règle ([ADR-0065](adr/0065-trame-websocket-etat-mutable-pas-constantes.md))** : la trame porte l'**état mutable à l'exécution et non dérivable** ; tout le reste est **HTTP-seul et déclaré**.

Jusqu'en v2.38.0, `/get-config` servait **89** clés et la trame **66** : les clients déjà connectés ne voyaient pas certains champs se rafraîchir avant leur prochain `/get-config`. La v2.39.0 ajoute **15 clés** (`max_cycles_per_day`, `regulation_speed`, `pool_size`, `ph_auto_manual_epoch`, `ph_auto_manual_reason`, et les 10 clés de suivi produit pH/ORP) — mêmes noms et mêmes valeurs que sur `/get-config`.

**12 clés restent volontairement HTTP-seules** (8 avant la v2.82.0 : `ph_offset_min`/`_max` **supprimées**, 6 nouvelles constantes de seuil **ajoutées**, feature-168), chacune déclarée avec un motif écrit :

| Clé HTTP-seule | Motif |
|----------------|-------|
| `ph_target_min` / `_max` / `_step`, `ph_offset_step` | **Constantes de compilation** : immuables pour un firmware donné, un client les lit une fois |
| `ph_offset_unusual_abs`, `ph_offset_confirm_interval_days`, `ph_offset_impasse_threshold` / `_high`, `ph_raw_floor_absolute`, `ph_raw_ceiling_absolute` | **v2.82.0 (feature-168)** — Constantes de compilation citées par les textes d'avertissement de l'écran de réglage et par la péremption à 30 jours ; servies **à la demande** (consultées en ouvrant l'écran), pas besoin d'un push temps réel |
| `regulation_defaults_by_size` | **Table de compilation** (3 colonnes), ~525 o de trame **par push** |
| `regulation_defaults` | **Dérivable** côté client depuis `pool_size` (dans la trame) et le cache `regulation_defaults_by_size` |

**Un intégrateur qui consomme la trame doit donc continuer d'appeler `/get-config`** pour ces 12 clés, et rester **inerte** quand elles manquent (clé absente ≠ `0`, ≠ `false`, ≠ `null`).

> ⚠️ **Pourquoi ce n'est pas une parité intégrale** : les 23 clés manquantes coûtaient **+1 365 o** sur une trame construite dans un buffer statique plafonné **dur** à `kWsFrameBufBytes = 3584` o, dont la garde **saute le push** au lieu de tronquer. La parité brute aurait remplacé des champs manquants par **une trame absente pour tous les clients**. Trame pire cas actuelle ≈ **2 948 o**, marge **636 o**. Détail du décompte : [ws-manager.md](subsystems/ws-manager.md#champs-config-ajoutés-en-bug-089--parité-de-la-trame-v2390).
>
> Une **sentinelle de test natif** (`test/test_native_ws_config_parity/`, coût flash nul) casse le build dès qu'une clé est ajoutée d'un seul côté — y compris dans le sens **WS-seul**, où un champ ne disparaît pas mais **clignote** (la charge `/get-config` l'écrase périodiquement côté client).

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

> **Motif** : ce paramètre sert à **caractériser au banc** le duty en dessous duquel la pompe cesse réellement de tourner, et à peser le débit à un duty fixe. C'est lui qui a produit la mesure du 2026-08-23 (Dmin = 82 ; 61/67 mL/min à 255) fondant le modèle en proportionnel de temps ([ADR-0043](adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)). Voir [pump-controller.md — Dosage en proportionnel de temps](subsystems/pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210).
>
> ⚠️ Il n'est **volontairement pas** borné par `MIN_ACTIVE_DUTY` (98 depuis v2.21.0) : sonder **sous** le plancher est précisément l'objet de la mesure. Sûreté : le paramètre ne peut que **réduire** la puissance par rapport au 255 déjà atteignable sans lui, et **toutes** les gardes restent en amont (auth `WRITE`, présence d'eau, inhibition maintenance). Sans le paramètre, le comportement est **strictement inchangé**. **Comptabilité (v2.21.0)** : le volume d'une marche manuelle est crédité au **majorant** débit-255 (`kPumpFlow255MlPerMin` — **74,0 mL/min, unique pour les deux pompes** depuis la v2.25.2, [ADR-0050](adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)), sauf si `duty` égale le duty calibré (crédit au débit mesuré) — jamais zéro sur une pompe alimentée.
>
> Journal : `[Test] Pompe 1 démarrée en mode manuel (duty=N)`.

> ⚠️ **Garde filtration (v2.1.2)** : `/pump1/on` retourne **`409 Conflict`** si la filtration est arrêtée et que `regulationMode != "continu"`. `/pump1/off` reste inconditionnel (pouvoir arrêter en toute circonstance).
>
> **v2.6.0** : le corps du 409 passe du texte brut au **JSON structuré** `{"error":"filtration_off","message":"…"}` — même format que les refus de `/ph/inject/start` (voir [tableau des codes](#codes-de-refus-409-v260)). Les routes de test `/pumpN/on` ne vérifient **que** la filtration (pas les limites volumétriques : pas de volume demandé), mais le temps de marche **consomme le budget horaire partagé** ([ADR-0020](adr/0020-budget-horaire-dosage-unique.md)).
>
> ⚠️ **v2.83.0 (feature-163)** : `/pump1/on` et `/pump2/on` partagent désormais `injectionAllowedOrReject()` avec `/ph|orp/inject/start` — elles refusent aussi en **409 `calibration_session_open`** si une session de calibration est ouverte, **avant** même la vérification de filtration. Voir [tableau des codes](#codes-de-refus-409-v260).

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

> 🔴 **`bug-170` (ouvert, critique, non corrigé)** : contrairement à **toutes** les autres routes
> de démarrage de pompe de cette page (`/pumpN/on`, `/ph|orp/inject/start`), ces deux routes
> appellent `setManualPump()` **directement**, après la seule authentification. ⛔ **Aucune garde
> de présence d'eau, aucune limite journalière ou horaire, aucun plafond de débit, et aucune garde
> d'étalonnage** (`calibration_session_open`, v2.83.0) — une pompe doseuse peut démarrer à un
> rapport cyclique arbitraire **sans circulation d'eau**, ce que `injectionAllowedOrReject()`
> refuse partout ailleurs. Voir
> [`bug-170`](../specs/features/todo/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md)
> et [ADR-0115](adr/0115-une-garde-sur-etat-partage-vit-au-point-dusage-final.md) (ce chemin en est
> l'exemple-même de « chemin pas encore fermé »). **Ne pas utiliser ces routes sur une
> installation en service** tant que ce bug reste ouvert.

---

### POST /ph/inject/start — WRITE

Démarre une injection manuelle pH. **v2.21.0 ([ADR-0043](adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md))** : la pompe tourne au **duty calibré** (mesure de CETTE pompe) si l'amplitude configurée y correspond, **sinon à 255** (seul autre débit connu, `kPumpFlow255MlPerMin` — **majorant unique de flotte, 74,0 mL/min** depuis la v2.25.2, [ADR-0050](adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md) : sur ce chemin **dégradé** la durée calculée est donc légèrement **courte**, jamais longue) — plus jamais un duty interpolé. La durée est calculée depuis le volume demandé et ce débit **connu** ; si le duty calibré est posé mais le débit pesé invalide (état incohérent), la route refuse en **409 `flow_uncharacterized`**. Le log de démarrage mentionne le duty : `[Injection] pH démarrée Xs pour Y mL (débit=Z mL/min, duty=D, pompe N)`.

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
| `calibration_session_open` | **v2.83.0 (feature-163)** — une session de calibration est **ouverte** (`mqttCfg.calSessionOpen`, posée inconditionnellement par `POST /calibration/enter`, quel que soit le mode de régulation). Garde **rang 3** — derrière Watchdog/Présence d'eau, **devant** la stabilisation post-cal et tout le reste (voir [pump-controller.md](subsystems/pump-controller.md#garde-rang-4--session-détalonnage-ouverte-tous-modes-feature-163-adr-0115)). Refus **inconditionnel**, aucune exception (y compris chloration de secours). Émet un `critical` `[Sécurité] Dosage refusé : étalonnage en cours (session de calibration ouverte) — aucune injection tant que la session n'est pas fermée`. Message : « Étalonnage en cours — injection refusée tant que la session de calibration est ouverte. » | — |
| `stabilization_in_progress` | Stabilisation post-calibration en cours **pour la pompe visée** | `seconds_remaining` (int) |
| `automatic_inject_in_progress` | **v2.77.0 (bug-161, gate `pool-chemistry` C1)** — une injection **automatique** est en cours sur cette pompe (`PumpController.isPhDosing()` / `isOrpDosing()`). Garde **rang 5** (était rang 4 avant v2.83.0, décalée par l'insertion de `calibration_session_open` en rang 3) — derrière Watchdog/Présence d'eau/session d'étalonnage/Stabilisation, **devant** le double démarrage manuel et les quotas. Ce trou était inatteignable tant que le bouton d'injection manuelle restait masqué hors du mode `manual` (feature-161 lève ce masquage) — voir [pump-controller.md](subsystems/pump-controller.md#garde-rang-5--injection-automatique-en-cours-bug-161-gate-c1). Message : « Une injection automatique est en cours — réessayez dans un instant. » | — |
| `already_injecting` | Une injection manuelle est déjà en cours sur cette pompe (double start) | — |
| `daily_limit` | Reliquat journalier **nul ou < 1 s de pompe** (sinon la demande est **écrêtée** au reliquat et acceptée — v2.9.2 ; frontière `==` **acceptée** ; limite ≤ 0 = illimité) | `remaining_ml` (int, reliquat arrondi **vers le bas** depuis v2.9.2 — le message ne promet jamais plus que disponible) |
| `hourly_limit` | `usedMs + durée demandée > *_limit_minutes × 60000` (budget **partagé** auto + manuel ; frontière `==` acceptée ; limite 0 = illimité) | — |
| `max_cycles` | Cycles/jour atteints (`maxCyclesPerDay = 20`, compteur partagé auto + manuel) | — |
| `burst_limit` | Anti-rafale : ≥ 6 démarrages/min OU ≥ 20 démarrages/15 min (ring partagé auto + manuel) | — |
| `flow_uncharacterized` | **v2.21.0** — aucun débit **connu** pour cette injection volumée : duty calibré posé mais débit pesé invalide (état incohérent, normalement impossible — gardes `/save-config` + assainissement NVS). Évalué **avant** les gardes `evaluateManualInject`. Message : « Débit de la pompe non caractérisé — effectuez la pesée (Réglages → Avancé → Configuration des pompes) avant une injection volumée. » Émet un `critical` `[Sécurité] Injection pH\|ORP manuelle refusée : débit non caractérisé (pesée requise)`. | — |
| `assist_active` | **v2.24.0 (feature-075, contre-visite gate)** — une session de l'[assistant de mesure du débit](#assistant-de-mesure-du-débit-des-pompes-feature-075-v2240) est ouverte : les injections manuelles classiques sont refusées sur **les deux chimies** tant qu'elle dure. Motif : sous suspension flow-assist, l'intégration du volume par tick est sautée — une injection classique interrompue par perte de filtration serait créditée **0** au registre journalier (violation P4). Refus **précoce** (avant toute mutation, avant même `planManualInject`). Message : « Mesure du débit en cours — réessayez après la fin de l'assistant. » Les routes de test `/pumpN/on?duty=` ne sont **pas** concernées (pas de crédit plancher). | — |

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

⚠️ **v2.79.0 (feature-162) — le sens change, les routes restent.** Avant ce lot, ces routes
posaient un **forçage manuel temporaire** (`state.manualOverride`, jamais persisté ni publié) qui
**neutralisait** la programmation en silence — la carte continuait d'annoncer « Programmation
activée » alors qu'elle ne faisait plus rien. Elles **choisissent désormais un mode** parmi les
trois seuls qui existent (`on` / `off` / `schedule`, voir [docs/subsystems/lighting.md](subsystems/lighting.md)) :
`/lighting/on` équivaut à `POST /save-config {"lighting_mode":"on"}`, `/lighting/off` à
`{"lighting_mode":"off"}`. **Persisté en NVS et publié en MQTT** — ce qu'elles ne faisaient pas
avant. Aucun état caché ne peut plus subsister derrière un appel à ces routes.

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

Active le **Mode Boost** (feature-053, v2.18.0) : surchloration temporaire du jour, auto-expirant au prochain minuit local. Tant qu'il est actif : créneau de filtration étendu jusqu'à minuit + cible ORP effective relevée (`min(orp_target + 60, 850)` mV) + limite journalière chlore effective relevée (`min(max_chlorine_ml_per_day × 1,5, 1000)` mL). **L'effet chlore ne s'applique qu'en mode de régulation ORP `automatic`** ; en Manuel / Programmé, seule la filtration s'étend. Aucun autre garde-fou n'est relâché. Voir [ADR-0093](adr/0093-le-mode-de-filtration-est-la-seule-autorite.md) (remplace [ADR-0025](adr/0025-mode-boost.md)).

⚠️ **Disponible dans tous les modes de filtration SAUF Arrêt** (feature-146, v2.63.0) : le Boost
surchlore une eau qui doit circuler, et le mode Arrêt ne consulte de toute façon pas le créneau.
Passer en mode Arrêt pendant que le Boost est actif **l'annule** (`stopBoost()`, pas une mise en
veille) — journalisé en `warning`, état republié.

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
| `filtration_extended` | boolean | `true` si la filtration est gérée par PoolController (`install_mode == "managed"`) → le Boost étend effectivement le créneau. `false` si filtration non gérée (levier inerte : le créneau n'est jamais consulté). |
| `chlorine_boosted` | boolean | `true` si `orp_regulation_mode == "automatic"` → le Boost relève effectivement la cible/limite chlore. `false` en Manuel / Programmé (levier inerte : le chlore n'est pas relevé). |

Quand un levier est inerte, `startBoost()` logue en plus un `warning` (couvre l'activation HTTP **et** MQTT/HA — voir [pump-controller.md §Mode Boost](subsystems/pump-controller.md#mode-boost-feature-053)). L'activation n'est jamais bloquée par un levier inerte (on informe, on n'empêche pas) — à la différence du refus **mode Arrêt** ci-dessous, qui lui bloque.

**Erreur 409** — deux causes possibles, distinguées dans le corps (feature-146, v2.63.0) :

- heure non synchronisée : sans horloge valide, l'expiration à minuit ne peut être calculée, l'activation est refusée (pas de boost « sans fin »)

  ```json
  { "error": "time_not_synced", "message": "Heure non synchronisée — Boost indisponible." }
  ```

- mode de filtration **Arrêt** : pas de circulation, pas de surchloration possible

  ```json
  { "error": "filtration_stopped", "message": "Boost impossible : filtration à l'arrêt (mode Arrêt)." }
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

### POST /rescue/start — WRITE

Active la **Chloration de secours** (feature-140, v2.55.0) : surcouche temporaire non destructive qui emprunte la branche de dosage « Programmée » existante pour continuer à injecter du chlore (volume fixe, aveugle à l'ORP) quand les sondes sont déclarées non fiables. **Ne modifie jamais `orp_regulation_mode` en NVS.** Auto-expirant 48 h après l'activation (epoch, **glissant** — pas un ancrage sur minuit local, voir [ADR-0087](adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md)). Décision structurante et libellés imposés : voir la spec [feature-140](../specs/features/done/feature-140-chloration-de-secours-et-widget-des-modes-temporaires.md).

**Paramètre `ml` (query, optionnel) — v2.56.0 (feature-141)** : volume quotidien **saisi** par l'utilisateur dans le dialogue d'activation, en mL entiers. **Absent ou non numérique** ⇒ la valeur est résolue par la **chaîne de pré-remplissage** (`orp_daily_target_ml` si > 0, sinon défaut par taille de bassin, sinon 1500 mL de repli). **Fourni** ⇒ seulement **borné** côté serveur, jamais réinterprété ni refusé : plancher **100 mL/j** (sous ce plancher, l'illusion d'une désinfection sans la fournir est jugée pire que ne pas armer le mode), plafond `min(limite journalière utilisateur si > 0, 2000 mL/j)`. **Le clamp serveur fait foi** — l'UI n'est qu'un confort, jamais une garde. Une valeur `> 1500` est acceptée : 1500 n'est plus qu'un **défaut**, pas un plafond (le bénéfice au-delà n'est simplement pas démontré — voir le libellé d'avertissement dans [page-dashboard.md](features/page-dashboard.md#chaînage-depuis-le-bandeau--dosage-bloqué--b4-feature-140)).

```bash
curl -u admin:monmotdepasse -X POST "http://poolcontroller.local/rescue/start?ml=1200"
```

**Réponse 200** :

```json
{ "rescue_active": true, "rescue_until": 1790150400, "rescue_ml": 1200 }
```

`rescue_ml` est le volume **réellement armé** après clamp serveur — peut différer de `ml` envoyé si hors bornes (ex. `ml=10` ⇒ `rescue_ml: 100`). C'est sur cette valeur que le client doit se recaler (`window._config.rescue_ml`), pas sur la saisie brute.

**Erreur 409** — mode de régulation ORP en Manuel (condition #5 du gate `pool-chemistry` : l'utilisateur dose déjà lui-même, l'activer en plus doserait deux fois) :

```json
{ "error": "mode_manual", "message": "Chloration de secours indisponible : la régulation du chlore est en mode Manuel. Repassez en Automatique ou Programmée pour l'utiliser." }
```

**Erreur 409** — heure non synchronisée (l'échéance 48 h serait indéterminable, même politique de sécurité que `POST /boost/start`) :

```json
{ "error": "time_not_synced", "message": "Chloration de secours impossible : horloge non synchronisée (échéance 48h indéterminable)." }
```

---

### POST /rescue/stop — WRITE

Désactive la Chloration de secours immédiatement. Retour au comportement normal selon la configuration réelle de l'utilisateur (`orp_regulation_mode` en NVS n'ayant jamais changé). Inconditionnel (jamais refusé).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/rescue/stop
```

**Réponse 200** :

```json
{ "rescue_active": false, "rescue_until": 0 }
```

---

### POST /reset-sensor-filter — CRITICAL

Réarmement **manuel** du filtre de mesure d'un capteur, introduit en v2.20.3 comme sortie de secours face au filtre de tendance centrale (ADR-0027/ADR-0041), qui pouvait rester verrouillé indéfiniment sur une médiane périmée.

⚠️ **État depuis la refonte de la chaîne de mesure (feature-152, v2.68.0, [ADR-0097](adr/0097-le-filtre-lisse-le-dosage-juge.md))** : cette route est **doublement orpheline**, mais **conservée en l'état**, délibérément. (1) Le **bouton qui l'appelait a été retiré** de l'interface (`data/` ne la référence plus) — elle n'est donc plus déclenchable qu'en appel direct (`curl`). (2) Sa raison d'être a disparu : la chaîne de mesure ne compte plus aucun verrou collant (2 → 0), et toute cause de blocage se résorbe désormais d'elle-même (la médiane traverse un échelon légitime en (N+1)/2 échantillons, quoi qu'il arrive). La route reste un usage légitime de **diagnostic** : vider la fenêtre et la détection de sonde figée sans redémarrer l'appareil. Détail et motif complet : commentaire au-dessus du handler, [`src/web_routes_control.cpp`](../src/web_routes_control.cpp).

Le filtre visé est vidé (médiane + lissage exponentiel) et la **détection de sonde figée est effacée** : le filtre repart en warmup complet (`kSensorFilterMedianWindow` = 9 mesures, ~45 s à 5 s/cycle) pendant lequel le dosage automatique reste bloqué. Il n'y a plus de « latch EMI » à lever — ce mécanisme a disparu avec la refonte.

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

## Assistant de mesure du débit des pompes (feature-075, v2.24.0)

Trois routes servent l'assistant guidé « Mesure du débit » (Réglages → Avancé, voir [page-settings.md](features/page-settings.md#assistant--mesure-du-débit--feature-075-v2240)). Principes de sécurité **non négociables** :

- **Minuterie côté serveur** : chaque marche de pompe (purge, mesure) est bornée par le firmware — si l'onglet se ferme ou le WiFi tombe, la pompe s'arrête seule à l'échéance. L'arrêt ne dépend **jamais** du navigateur.
- **Duty et durée fixés serveur, jamais acceptés du client** : purge à duty **255** (seul débit connu hors calibration — borne le volume résiduel poussé au bassin) pendant `kFlowAssistPurgeDurationS` = 60 s ; mesure à l'**amplitude exacte d'exploitation** (`getPumpAmplitudeDuty`) pendant `kFlowAssistMeasureDurationS` = 120 s.
- **Suspension du dosage automatique et programmé** (les deux chimies) pendant la session, rétablie à la sortie — avec **timeout serveur** `kFlowAssistSuspendTimeoutMs` = **15 min sans requête** (réconciliation fail-closed au tick, voir [pump-controller.md](subsystems/pump-controller.md#suspension--assistant-de-mesure-du-débit--feature-075-v2240)).
- **Toutes les gardes d'injection manuelle s'appliquent** (`evaluateManualInject` : watchdog, présence d'eau, stabilisation, double start, limites journalière/horaire, cycles/jour, anti-rafale) avec le volume **majorant** `flow255 × durée`. La marche est **créditée** au registre journalier (crédit-plancher à échéance ; arrêt anticipé ou perte de filtration → crédit **prorata au majorant**, jamais zéro sur une pompe alimentée — P4). Chaque marche compte un cycle (`requestManualCycleRecord`).
- **Une seule pompe en assistant à la fois** ; le véhicule d'exécution est le chemin `manualInject*` existant — aucune nouvelle mécanique de pompe.

### POST /flow-assist/start — WRITE

Ouvre (ou rejoint) la session d'assistant et lance une marche minutée serveur. Paramètres en corps `form-urlencoded` **ou** query string :

| Paramètre | Valeurs | Rôle |
|-----------|---------|------|
| `pump` | `1` \| `2` | Pompe **physique** |
| `phase` | `purge` \| `measure` | Type de marche (duty/durée déduits côté serveur — voir ci-dessus) |

```bash
curl -u admin:monmotdepasse -X POST -d "pump=1&phase=purge" \
  http://poolcontroller.local/flow-assist/start
```

**Réponse 200**

```json
{ "duration_s": 60, "phase": "purge", "purges_left": 2 }
```

**Erreurs**

- `400` : `{"error":"invalid_param"}` — `pump` ou `phase` absent/invalide
- `401` : token manquant / invalide
- `409` — corps JSON structuré `{"error":"…","message":"…"}` :

| `error` | Déclencheur |
|---------|-------------|
| `assist_busy` | Une session est déjà ouverte sur **l'autre** pompe (une seule pompe à la fois) |
| `purge_limit` | Quota de purges de la session atteint (`kFlowAssistMaxPurgesPerSession` = 3) |
| `already_injecting` | Une marche de pompe est déjà en cours sur cette chimie |
| `invalid_amplitude` | Amplitude d'exploitation nulle (`phase=measure` uniquement) — mesure impossible |
| *(codes des gardes manuelles)* | `watchdog_inactive`, `filtration_off`, `stabilization_in_progress`, `daily_limit`, `hourly_limit`, `max_cycles`, `burst_limit`, `maintenance_mode` — mêmes codes que [`/ph/inject/start`](#codes-de-refus-409-v260), évalués sur le volume majorant |

Chaque requête `/flow-assist/*` **réarme le timeout de session** (15 min). Un `start` accepté réarme aussi le drapeau `aborted` de `/data` à `false`.

### POST /flow-assist/stop — WRITE

Arrêt **anticipé** de la marche en cours (coupe-circuit opérateur — jamais gardé). La **session est conservée** : suspension du dosage maintenue, l'utilisateur reste dans l'assistant. Le volume est crédité **prorata au majorant** (`flow255 × temps écoulé`). Idempotent (aucune marche → `stopped: false`).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/flow-assist/stop
```

**Réponse 200**

```json
{ "stopped": true, "purges_left": 2 }
```

### POST /flow-assist/exit — WRITE

Sortie de l'assistant (Annuler, Terminer, sortie normale) : arrêt de la marche éventuelle (crédit prorata), **fermeture de la session puis levée de la suspension** — dans cet ordre, invariant anti-course (voir [pump-controller.md](subsystems/pump-controller.md#suspension--assistant-de-mesure-du-débit--feature-075-v2240)). Idempotent : sans session, ne fait que confirmer le rétablissement.

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/flow-assist/exit
```

**Réponse 200**

```json
{ "exited": true }
```

> L'**état de la session** est exposé en lecture seule par l'objet [`flow_assist` de `GET /data`](#get-data--write) (phase, secondes restantes, purges restantes, drapeau `aborted`, durées et débits majorants servis). L'**enregistrement du débit mesuré** ne passe pas par ces routes : l'UI convertit le volume lu en ml/min et l'envoie par le chemin `POST /save-config` existant (clé `pumpN_pulse_flow_ml_per_min`, feature-074 — duty déduit côté serveur, drapeau « mesuré » posé).

---

## Calibration (Atlas EZO — feature-021)

Toutes les routes de calibration retournent **immédiatement** (`< 1 ms`) en mettant la commande dans la queue FreeRTOS de `SensorManager`. La commande s'exécute dans `loopTask` en ~900 ms (transaction I²C bloquante avec le module EZO). L'UI observe l'avancement via les champs WS `phCalPoints` / `orpCalPoints` rafraîchis automatiquement.

⚠️ **Authentification du corps (v2.46.1)** — pour les trois routes ci-dessous, le **corps est refusé AVANT toute mise en file** : la garde `calibrationBodyAuthorized()` est la première instruction du handler de corps, exécutée avant le parse JSON et avant `enqueue*()`. Le `401` reste produit par le handler de **requête** (`REQUIRE_AUTH(WRITE)`), qui s'exécute **après** le corps — la réponse et le décompte de quota sont inchangés, seul l'effet de bord disparaît. Jusqu'en 2.46.0, un `POST` **anonyme** obtenait bien son `401` mais la commande EZO partait quand même, et `loopTask` l'exécutait ~900 ms plus tard : une calibration est **irréversible**. Une tentative non authentifiée est journalisée `WARNING` **au plus 1 fois/60 s et par route**. Détail et portée chimique : [`docs/subsystems/sensors.md`](subsystems/sensors.md#authentification-du-corps-des-routes-de-calibration-v2461).

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

---

### POST /calibration/enter — WRITE

Bascule la régulation **pH ET ORP en `manual`** à l'entrée dans l'écran de calibration (bug-160,
volet 2, REFONTE du 2026-10-13 — voir
[pump-controller.md](subsystems/pump-controller.md#bascule-de-régulation-en-manuel-pendant-une-calibration-bug-160-volet-2)
et [ADR-0107](adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)). Mémorise
le mode **antérieur** de chaque boucle (pour une restauration fidèle par
`POST /calibration/exit`), interrompt **immédiatement** toute injection manuelle en cours, et
demande l'interruption d'une session automatique/programmée déjà ouverte. Idempotente : un
second appel pendant une session déjà ouverte ne réécrit pas le mode antérieur mémorisé.

⚠️ **Exception de sécurité (gate `pool-chemistry`, condition C3)** : si la régulation pH est déjà
en `manual` sous l'effet d'une **escalade de sécurité** (`phAutoManualEpoch`/`phAutoManualReason`,
feature-067, refus répétés de plancher acide), le pH n'est **pas** touché — il est **déjà**
verrouillé en Manuel pour une raison plus grave que la calibration, et cette route ne mémorise
jamais ce `manual` comme un mode à restaurer plus tard (effacer l'escalade serait un nouveau trou
de sécurité). Aucun verrou équivalent n'existe côté ORP.

⚠️ **Mode courant déjà `manual`, par voie** (correctif 2026-10-13, v2.76.2) : indépendamment du
verrou de sécurité ci-dessus, **aucune bascule** n'a lieu non plus si la boucle (pH **ou** ORP) est
**déjà** en `manual` — avant ce correctif, l'ORP était toujours déclaré basculé même sans rien
changer. Si **ni** le pH **ni** l'ORP ne basculent (les deux étaient déjà en `manual`), aucune
session de bascule ne s'ouvre : pas de bandeau côté client, pas d'alerte d'oubli armée.

⚠️ **v2.83.0 (feature-163)** — **indépendamment de tout ce qui précède**, cette route ouvre
**inconditionnellement** la garde de dosage `mqttCfg.calSessionOpen` (persistée, NVS
`cal_sess_open`) : aucune injection (automatique, programmée, manuelle, de secours, ou test de
pompe) ne passe plus tant qu'elle est ouverte, **y compris dans le cas ci-dessus** où ni le pH ni
l'ORP ne basculent de mode — c'est précisément le cas que la bascule de mode seule ne protégeait
pas. Voir [tableau des codes 409](#codes-de-refus-409-v260) (`calibration_session_open`) et
[pump-controller.md](subsystems/pump-controller.md#garde-rang-4--session-détalonnage-ouverte-tous-modes-feature-163-adr-0115).

**Payload** : aucun (POST sans corps).

**Réponse 200** :

```json
{ "success": true, "ph_switched": true, "orp_switched": true }
```

`ph_switched` vaut `false` si le pH est déjà verrouillé par l'escalade de sécurité **ou** déjà en
`manual` par choix de l'utilisateur. `orp_switched` vaut `false` dans ce dernier cas aussi
(⚠️ n'est **plus** toujours `true` depuis le correctif du 2026-10-13 — v2.76.2).

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/calibration/enter
```

---

### POST /calibration/exit — WRITE

Restaure le mode **antérieur** de chaque boucle (pH, ORP), mémorisé par
`POST /calibration/enter` — ⛔ jamais `"automatic"` en dur : un mode `"scheduled"` redevient
`"scheduled"`. C'est l'action du bouton **« Fermer et rétablir la régulation »** de l'écran de
calibration (libellé dynamique : simplement **« Fermer »** si aucune bascule n'a eu lieu à
l'entrée — cas de l'exception de sécurité ci-dessus).

⚠️ **N'acquitte jamais** `phAutoManualEpoch`/`phAutoManualReason` (gate `pool-chemistry`,
condition C2, piège central de la refonte) : si cette trace de sécurité est active — qu'elle
l'ait déjà été à l'entrée, ou qu'elle soit apparue **pendant** la session de calibration (par
exemple une escalade de plancher acide déclenchée pendant que l'écran était ouvert) — le pH reste
en Manuel et seul l'ORP est restauré.

**Payload** : aucun (POST sans corps). Idempotente : appelée sans session ouverte (double clic,
session jamais ouverte), répond 200 sans rien muter.

⚠️ **v2.83.0 (feature-163)** — referme **inconditionnellement** `mqttCfg.calSessionOpen`, **que**
`calSwitchActive` soit actif ou non. C'est le seul chemin de fermeture (aucune expiration, aucun
mécanisme de relâche automatique) — une session laissée ouverte gèle **tout** dosage, secours
compris, jusqu'à cet appel.

**Réponse 200** (session ouverte, restauration effectuée) :

```json
{ "success": true, "restored": true, "ph_mode": "scheduled", "orp_mode": "manual" }
```

**Réponse 200** (aucune session ouverte) :

```json
{ "success": true, "restored": false }
```

```bash
curl -u admin:monmotdepasse -X POST http://poolcontroller.local/calibration/exit
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

<a id="authentification-de-lupload-v2462"></a>

⚠️ **Authentification de l'UPLOAD (v2.46.2, `bug-117`)** — jusqu'en 2.46.1 incluse, le **corps** de cette requête était traité **sans aucune vérification d'authentification** : `REQUIRE_AUTH(req, RouteProtection::CRITICAL)` n'existait que dans le handler de **réponse**, exécuté **après** un handler d'upload qui appelait `Update.begin()` puis `Update.end(true)`. Un client **non authentifié** du réseau local pouvait donc faire **flasher une image arbitraire** ; le `401` partait après le flash.

Depuis la v2.46.2, la garde `otaUploadAuthorized()` est la **première instruction** du handler d'upload, **avant** le verrou anti-uploads concurrents et avant tout effet de bord : un corps non autorisé est **ignoré en totalité** — aucun `Update.begin()`, **aucune écriture de partition**, pompes doseuses jamais inhibées, LittleFS jamais démonté. Un **`401` est donc désormais un résultat possible de cette route**, et il reste produit par le handler de **requête** (`REQUIRE_AUTH`) : la réponse, son corps et le décompte de quota sont inchangés, seul l'effet de bord disparaît.

**Aucun client légitime n'est affecté** — équivalence vérifiée dans les deux sens sur ESPAsyncWebServer 3.11.2 (`_parseReqHeader()` renseigne `_authorization` **et** empile tous les en-têtes avant le premier fragment, et rien ne les purge en cours de requête). Les **trois** appelants passent : `ota_update.sh` en authentification basique (`curl -u admin:<mdp>`), le même en jeton (`-H 'X-Auth-Token: …'`), et la page web d'upload (XHR + `X-Auth-Token`).

Une tentative non authentifiée est journalisée `WARNING` **au plus 1 fois/60 s**, avec l'IP d'origine — et **une seule fois par upload** (au premier fragment). Détail, motif et limites : [`docs/subsystems/ota-manager.md`](subsystems/ota-manager.md#authentification-du-handler-dupload-v2462).

**Champ POST `sha256` — OBLIGATOIRE depuis la v2.67.0** (v2.11.0, [ADR-0022](adr/0022-verification-integrite-ota.md) ; rendu obligatoire en v2.67.0, feature-118 lot B, [ADR-0096](adr/0096-empreinte-sha256-obligatoire-sur-post-update.md)) : empreinte SHA-256 de l'image, avec ou sans préfixe `sha256:`. **Une requête sans ce champ est désormais refusée** — `400`, **avant tout effet de bord** sur la partition (avant `g_uploadOwner = request`, avant `Update.begin()`, avant le démontage de LittleFS). Même place et même motif que le refus « malformée » introduit par le lot A.

> ⚠️ **ORDRE DES PARTIES DU MULTIPART — déterminant.** Le champ `sha256` doit **précéder** la partie fichier (`update_file`) dans le formulaire. ESPAsyncWebServer ne connaît un champ non-fichier qu'une fois sa partie du corps atteinte : un `sha256` envoyé **après** le fichier n'est pas encore lu à `index==0` et se voit comme **absent**, même si le client l'a bien envoyé — refus garanti. Les deux appelants officiels respectent cet ordre : `ota_update.sh` place `-F "sha256=…"` avant `-F "update=…"` dans la commande `curl`, et la page web ajoute `sha256` au `FormData` avant `update_file`.

> ⚠️ **Ce que cette empreinte prouve, et ce qu'elle ne prouve pas.** L'empreinte est **calculée par l'appelant** : elle prouve que l'image écrite est celle qu'il a envoyée, **pas qui l'a produite** — même devenue obligatoire. Pour le **firmware**, l'IDF vérifie déjà l'image à l'activation (`Update.end(true)` → `esp_ota_set_boot_partition()`) ; l'empreinte ajoute la détection du **mauvais fichier**. Pour le **filesystem**, elle est la **seule** vérification d'intégrité. **La règle de sécurité n° 6 de `CLAUDE.md` n'est TOUJOURS PAS satisfaite** ; elle le sera par une signature à clé publique, voir la spec de suivi [feature-151](../specs/features/todo/feature-151-signature-de-l-image-ota.md). Le terme à employer reste **« intégrité du transfert »** — jamais « sécurité ».
>
> `POST /download-update` n'est pas mieux armé : son paramètre `digest` est, lui aussi, **fourni par l'appelant**, et `isUrlAllowed()` ne contrôle que l'**hôte** (n'importe quel dépôt GitHub passe). Les deux routes sont désormais au **même niveau de preuve ET à la même obligation**.

```bash
curl -u admin:monmotdepasse -X POST \
  -F "update_type=firmware" -F "sha256=sha256:<64 hex>" \
  -F "update=@.pio/build/esp32dev_core3/firmware.bin" \
  http://poolcontroller.local/update
```

**Message de refus « absente »** (`kOtaSha256MissingMessage`, [`src/constants.h`](../src/constants.h)), **verrouillé mot à mot par une garde de test** — il couvre **deux** causes distinctes, dans cet ordre délibéré (la première est celle qu'un utilisateur non technique rencontrera réellement) :

```
FAIL: empreinte SHA-256 absente — si l'envoi vient de la page web, elle est
obsolète (en cache) : rechargez-la (Ctrl+Maj+R, ou videz le cache du site)
puis recommencez ; si l'envoi vient d'un script, le champ "sha256" doit
PRÉCÉDER la partie fichier dans le formulaire multipart — l'ordre des champs
est déterminant
```

Cause la plus fréquente en pratique : un onglet de l'interface **déjà ouvert avant le déploiement** du filesystem a chargé l'ancien `data/app.js`, sans le calcul SHA-256. Un onglet rechargé après le déploiement, ou ouvert après coup, n'y est pas exposé.

<a id="reponses-post-update-v2201"></a>

#### Réponses (v2.20.1)

> ⚠️ **Changement de contrat** : jusqu'à la v2.20.0 incluse, **tout** échec était servi en `200 OK` avec un corps `FAIL`. Un client qui testait le code HTTP voyait donc un succès. Depuis la v2.20.1, le code HTTP porte le verdict.

| Cas | Statut | Corps |
|---|---|---|
| Succès | `200` | `OK` **exactement** — inchangé (le front et `ota_update.sh` en dépendent) |
| **Non authentifié** (v2.46.2) | **`401`** | `{"error":"Authentication required"}` — corps d'upload **refusé avant toute écriture de partition** ([détail](#authentification-de-lupload-v2462)) |
| Upload concurrent | `409` | `Upload refusé: une mise à jour OTA est déjà en cours` — inchangé |
| Refus de type (octet magique) | `400` | `FAIL: <cause>` |
| `sha256` **absent** (v2.67.0) | `400` | `FAIL: empreinte SHA-256 absente — si l'envoi vient de la page web, elle est obsolète (en cache)…` (texte complet ci-dessous) |
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

#### ⚠️ Contrat d'erreur enrichi (`bug-165`, v2.80.x) — `step` et `detail`

Avant ce lot, **tout** code HTTP négatif (échec côté client — résolution de nom, TCP refusé,
handshake TLS…) produisait le **même** corps `{"error":"GitHub API error","code":-1}` : la cause
racine du bug-165 lui-même (mbedtls 3.x refusant une ancre de confiance cross-signée, voir
[docs/subsystems/ota-manager.md](subsystems/ota-manager.md#règle--une-ancre-de-confiance-est-toujours-la-variante-auto-signée-bug-checkupdate-tls-core3-bug-165-adr-0111))
n'était donc **diagnosticable qu'au journal**, jamais depuis la réponse HTTP elle-même. Le corps
`500` porte désormais deux champs supplémentaires :

```json
{
  "error": "GitHub API error",
  "code": -1,
  "step": "tls_handshake",
  "detail": "unable to verify the first certificate (-9984)"
}
```

| `step` | Condition | `detail` |
|---|---|---|
| `tls_handshake` | Code HTTP négatif **avec** une erreur TLS (`WiFiClientSecure::lastError() != 0`) — chaîne de certificats refusée | Message mbedtls réel (identique à celui déjà journalisé) |
| `connection` | Code HTTP `-1` **sans** erreur TLS associée | « Connexion impossible (résolution de nom ou TCP refusé) » |
| `http` | Tout autre code HTTPClient négatif (délai dépassé, encodage…) | `HTTPClient::errorToString(code)` |

⚠️ **`code` reste le contrat stable** (déjà le cas) ; `step`/`detail` sont des **champs
diagnostiques**, pas un contrat figé — le texte de `detail` peut varier selon la bibliothèque TLS.

⚠️ **Le 404 « aucune release publiée » est désormais nettement distinguable d'un échec réseau** —
avant que le correctif TLS ne rétablisse la connexion, **les deux cas produisaient le même
`code: -1`** : la connexion échouait systématiquement avant même d'atteindre GitHub, donc un vrai
404 (dépôt sans release) n'était jamais reçu pour être distingué. Le cas 404 reste intercepté
**avant** la classification ci-dessus et répond en **`200`**, pas en erreur :

```json
{
  "current_version": "2.80.0",
  "latest_version": "2.80.0",
  "update_available": false,
  "no_release": true,
  "message": "Aucune release disponible sur GitHub"
}
```

<a id="garde-tls-sortante-psram-libre--heap-v2186"></a>

#### Garde TLS sortante — plus gros bloc PSRAM + heap interne 8 bits (v2.18.6 ; **refondue v2.81.0**, [ADR-0036](adr/0036-psram-requise-mise-a-jour-en-ligne.md) + [ADR-0112](adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md))

🚨 **Les deux critères ci-dessous ont changé de nature en v2.81.0 (bug-165/bug-108)** — la cause
racine réelle n'était **pas** un problème de certificats (huit hypothèses l'ont supposé, toutes
écartées) mais mbedtls 3.6.5 **forçant** ses allocations en RAM interne
(`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1`), consommant ~56 Ko de tas interne pendant un handshake —
`maxAlloc8` mesuré à **3 316 o** en pleine vérification de certificat, pendant que 4,17 Mo de
PSRAM dormaient. Corrigé par `mbedtls_platform_set_calloc_free()`
([`src/mbedtls_psram_alloc.cpp`](../src/mbedtls_psram_alloc.cpp)) : mbedtls obéit désormais à la
politique PSRAM déjà posée par le projet. Détail complet :
[subsystems/ota-manager.md](subsystems/ota-manager.md) et
[subsystems/oom-guard.md](subsystems/oom-guard.md#-cause-racine-réelle-bug-165-2026-10-15--mbedtls-forçait-ses-allocations-en-ram-interne).

Avant la création de `WiFiClientSecure`, la route évalue le prédicat pur partagé `oomTlsGateReason()` ([subsystems/oom-guard.md](subsystems/oom-guard.md)), qui retourne un **motif** (il remplace le booléen `oomTlsGateOk` de la v2.18.5). **Signature et logique du prédicat inchangées** — seule la **sémantique des entrées** et les **seuils** ont changé. Deux critères, évalués **dans cet ordre** :

1. **Plus gros bloc PSRAM contigu** (`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` — ⚠️ **plus** `ESP.getFreePsram()`, qui mesure le total et non la contiguïté) < `kOtaTlsMinPsramLargestBlockBytes` (40 960 o = 2×16 384 tampons SSL + 8 192 marge JSON) → refus. Un total élevé mais fragmenté n'accueillerait pas un tampon TLS de 16 Ko.
2. Sinon, bicritère heap **interne 8 bits** (ADR-0058, ⚠️ **plus** `ESP.getFreeHeap()`/`getMaxAllocHeap()`, IRAM-inclusives) : `heapFree8Bit()` < `kCheckUpdateMinFreeHeapBytes` (32 768 o) **OU** `heapLargestFreeBlock8Bit()` < `kCheckUpdateMinMaxAllocBytes` (24 576 o) → refus **temporaire**.

🚨 **Pourquoi ce changement de métrique était nécessaire, pas cosmétique** : la garde v2.18.6-v2.80.x lisait `ESP.getFreeHeap()` = **82 796 o** au moment d'un refus réel et **autorisait** (seuil 65 536) — pendant que le plus gros bloc 8 bits réellement allouable valait **3 316 o**. *Une garde qui mesure la mauvaise grandeur ne protège de rien. Celle-ci a lu 82 796 octets libres et autorisé une connexion, pendant que le bloc réellement utilisable valait 3 316.*

**Trois refus possibles** (v2.18.7, inchangés par la refonte v2.81.0), tous en `503 application/json` et produits par le **helper partagé** `refuseIfTlsGateClosed` — donc **identiques au caractère près sur `/check-update` et `/download-update`** :

| `code` | Condition (v2.81.0) | Nature | `error` (texte utilisateur) | Effet interface |
|---|---|---|---|---|
| `psram_required` | Plus gros bloc PSRAM **absent** (`== 0`) | **Permanent** (matériel) | « La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à jour manuelle. » | Bouton **neutralisé** + callout permanent dans la carte GitHub ; repli : [upload de fichier](#post-update--critical) ou `deploy.sh ota-firmware` |
| `psram_low` | Plus gros bloc PSRAM présent mais **< 40 960 o** (fragmentée ou saturée) | **Temporaire**, réessayable | « La mise à jour en ligne est momentanément indisponible : la mémoire PSRAM est saturée. Réessayez dans quelques minutes ou utilisez la mise à jour manuelle. » | Toast **informatif**, bouton **non** neutralisé, rafraîchissement de l'état via `/get-system-info` |
| `tls_low_memory` | heap interne 8 bits libre < `kCheckUpdateMinFreeHeapBytes` **OU** plus gros bloc interne 8 bits < `kCheckUpdateMinMaxAllocBytes` (motif `kOomTlsGateLowMemory`) | **Temporaire**, réessayable | « Mémoire insuffisante pour TLS, réessayez » (texte inchangé depuis la v2.18.5) | Toast **informatif**, bouton **non** neutralisé, rafraîchissement via `/get-system-info` |

La discrimination `psram_required` / `psram_low` est faite **par la route** (simple test de nullité du plus gros bloc PSRAM déjà mesuré, aucun seuil dupliqué) : le prédicat pur `oomTlsGateReason()` regroupe les deux sous le motif `kOomTlsGateNoPsram` ([subsystems/oom-guard.md](subsystems/oom-guard.md#garde-tls-sortante-partagée--oomtlsgatereason-v2186--refondue-v2810-bug-165bug-108)).

> ⚠️ **Le champ `code` est le seul contrat machine** : il est présent sur les **trois** refus (depuis la v2.18.7 — auparavant seul le refus PSRAM en portait un) et c'est le discriminant stable pour un client. Le champ `error` est un **texte destiné à l'utilisateur** et **peut être reformulé sans préavis ni changement de version majeure** — il l'a été en v2.18.7 (formulation neutre, vocabulaire aligné sur la carte « Mise à jour manuelle » de l'interface). Ne jamais tester le contenu de `error` : tester `code`. Un client doit traiter `psram_required` comme **définitif** et `psram_low` / `tls_low_memory` comme **réessayables**. Le statut `503` et la valeur `psram_required` sont, eux, inchangés depuis la v2.18.6.

Les **logs** du firmware (WARN de refus, INFO nominal) restent volontairement techniques : ils sont destinés au diagnostic, pas à l'utilisateur, et ne suivent donc pas les reformulations du champ `error`.

Chaque refus émet un **WARN** auto-explicatif : motif, plus gros bloc PSRAM, les deux mesures heap interne 8 bits, leurs trois seuils et l'**IP du client** :

```
WARN  /check-update refusé (mémoire interne 8 bits insuffisante pour TLS) : plus gros bloc PSRAM=4194304 o (min 40960), heap interne 8 bits libre=28672 o (min 32768), plus gros bloc interne 8 bits=20480 o (min 24576), client=192.168.1.15
```

> ⚠️ **Mise à jour en ligne indisponible sans PSRAM** (rectifie la note « limitation core Arduino 3 » des versions antérieures — l'attribution correcte, établie par la mesure terrain du 2026-08-12, est l'**absence de PSRAM**, pas le core Arduino) : sur un module **WROOM** (sans PSRAM), la session TLS GitHub réclame plusieurs blocs contigus d'environ 16 Ko que la RAM interne ne fournit plus de façon fiable après quelques minutes de fonctionnement. Depuis la v2.18.6, `/check-update` **et** `/download-update` répondent donc `503 psram_required` **immédiatement**, sans ouvrir de session. Le module du BOM PCB v2 (**ESP32-DevKitC-VE / WROVER-E, 8 Mo de PSRAM**) place désormais **réellement** ces allocations en PSRAM (depuis le correctif mbedtls v2.81.0 — avant lui, le module avait la PSRAM mais mbedtls ne l'utilisait pas).

> **Note seuils (révisée v2.81.0)** : `kOtaTlsMinPsramLargestBlockBytes` = 40 960 o est désormais **mesuré sur WROVER réel** (plus gros bloc PSRAM observé très au-dessus, ~4 Mo) — l'ancien seuil `kOtaTlsMinPsramBytes` = 1 Mo portait la PSRAM libre **totale**, jamais mesurée sur WROVER réel, et reposait sur l'hypothèse (fausse) que mbedtls utilisait déjà la PSRAM. Les seuils heap portent désormais la métrique **8 bits** (ADR-0058), dérivation écrite dans `constants.h` et verrouillée par `static_assert`.

#### Nouveau champ `/get-system-info` : `mbedtls_psram_alloc` (v2.81.0)

Témoin anti-décoratif de l'allocateur PSRAM de mbedtls (ajout **purement additif**) :

| Valeur | Signification |
|---|---|
| `0` | Crochet jamais posé (pas de PSRAM) ou refusé par mbedtls |
| `1` | Crochet posé **et** confirmé actif en PSRAM (témoin réel) — régime nominal attendu |
| `2` | 🚨 **ANOMALIE** — crochet posé mais le témoin est retombé en RAM interne : la famine du handshake TLS persiste malgré le correctif |

Absent d'un firmware ≤ 2.80.x → traiter l'absence comme « information indisponible » (même convention que les autres champs additifs de cette route).

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
| `503` | `psram_low` | PSRAM présente mais plus gros bloc contigu **< 40 960 o** (fragmentée ou saturée, v2.81.0 — était < 1 Mo libre total) — **réessayable** |
| `503` | `tls_low_memory` | Heap interne **8 bits** momentanément dégradé (v2.81.0 — était la métrique IRAM-inclusive) — **réessayable** |

Les corps des **trois** refus sont produits par le **même helper** (`refuseIfTlsGateClosed`) que ceux de `/check-update` : les deux routes renvoient donc, au caractère près, les mêmes textes et les mêmes valeurs de `code` — voir le [tableau détaillé plus haut](#garde-tls-sortante-psram-libre--heap-v2186). Là encore, seul `code` fait contrat ; le champ `error` est un texte utilisateur reformulable.

<a id="download-update-aucun-handler-de-corps"></a>

✅ **Audité SAIN vis-à-vis de `bug-117` — pour une raison structurelle** (v2.46.2) : cette route est enregistrée en `server->on(url, HTTP_POST, lambda)` à **trois arguments**, donc **sans aucun handler de corps ni handler d'upload**. Il n'existe pas de point d'entrée exécuté avant le lambda de requête : `REQUIRE_AUTH(req, RouteProtection::CRITICAL)` s'exécute **nécessairement** avant tout accès à `Update`. Le défaut de `POST /update` ne peut pas s'y produire. Consigné ici pour ne pas re-auditer la même route : la question ne se rouvre que si un 4ᵉ ou 5ᵉ argument (corps / upload) est ajouté à cet enregistrement.

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

Réponse : `{ "success": true }`. Erreurs : 400 si adresse hex invalide ou role hors `"water"|"circuit"` ; 404 si l'adresse n'est pas présente sur le bus ; **413** si le corps dépasse **256 octets** (`kSensorIdentifyMaxBodyBytes`, bug-113, v2.48.0 — corps réel ≈ **45 o**, soit 5,7× de marge ; refus en JSON maison **avant** authentification, cf. `/history/import` (section « Limite dure : 8 192 octets de corps »)).

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

> ⚠️ **Cette route n'est plus ouverte.** Jusqu'en v2.20.2 elle était accessible **sans authentification**, comme les autres `/debug/*`. Elle effectue exactement la même action destructrice que [`POST /reset-sensor-filter`](#post-reset-sensor-filter--critical) (effacement de la détection de sonde figée) et contournait donc toutes les gardes posées sur celle-ci : elle exige désormais `RouteProtection::CRITICAL`. Les routes `/debug/*` voisines (`ph_slope_refresh`, `sensor_filter_state`, `sensor_trace`, `sensor_trace_clear`) restent **sans auth**.
>
> ⚠️ **État depuis la refonte de la chaîne de mesure (feature-152, v2.68.0)** : même situation que `POST /reset-sensor-filter` — la route reste fonctionnelle mais n'a plus de raison d'être qui la justifierait seule (plus de verrou collant à casser), elle reste un usage de diagnostic. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#chaîne-de-mesure-à-5-étages-feature-152).

Réinitialise un filtre (médiane + lissage exponentiel). Le filtre repasse en **warmup** → le dosage automatique est bloqué jusqu'à ce que la **fenêtre médiane** soit de nouveau pleine (`kSensorFilterMedianWindow` = 9 nouvelles mesures valides, ≈ 45 s à 5 s/cycle) avant de produire une valeur exploitable. Il n'y a plus de « latch EMI » à lever — ce mécanisme a disparu avec la refonte. Utile après un débranchement EZO, un test, ou une recalibration manuelle hors workflow.

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

**Sémantique `queued`** : la réponse signifie **acceptée**, pas **exécutée**. Le handler ne touche jamais le filtre (contexte `async_tcp`) : il pose un drapeau consommé en tête de `SensorManager::update()`, donc le reset a lieu au **prochain tour de boucle** (< 100 ms). L'état détruit (médiane avant/après, rejets, « prêt » avant reset, sonde figée) est journalisé à ce moment-là, en `warning` — ou en `critical` si une détection de sonde figée vient d'être effacée.

**Erreurs** : `400` (`sensor` invalide) · `401`/`403` (auth CRITICAL manquante).

### `GET /debug/sensor_filter_state` — pas d'auth (cohérent avec autres `/debug/*`)

Retourne l'état brut des deux filtres pour diagnostic. Lecture lock-free des getters `SensorManager` (valeurs scalaires).

```bash
curl http://poolcontroller.local/debug/sensor_filter_state
```

**Réponse 200**

```json
{
  "ph":  { "raw": 7.241, "median": 7.236, "filtered": 7.234, "state": "ready", "rejected": 0 },
  "orp": { "raw": 722, "median": 720, "filtered": 720, "state": "ready", "rejected": 0 }
}
```

Les champs `raw` / `median` / `filtered` valent `null` si la mesure est indisponible. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#chaîne-de-mesure-à-5-étages-feature-152).

> ℹ️ **Depuis la v2.69.0 (feature-153), cette route perd son dernier consommateur dans `data/`** — la carte « Signaux capteurs » affichait les lignes « État filtre pH/ORP » qu'elle alimentait ; elles sont retirées (voir [page-settings.md](features/page-settings.md#card-signaux-capteurs-feature-153-v2690)). **La route reste exposée** (diagnostic `curl`) : son retrait éventuel est une décision produit distincte, non prise ici.

#### Champ `state` (v2.68.0, feature-152 — remplace `ready`/`unstable`/`dispersion`/`dispersion_max`/`latch_cycles`/`clean_streak`)

⚠️ **Retiré par la refonte de la chaîne de mesure.** Jusqu'en v2.67.x, cette route exposait `ready` (bool), `unstable` (bool) et quatre champs de dispersion (`dispersion`, `dispersion_max`, `latch_cycles`, `clean_streak`) qui pilotaient un **latch EMI** — mécanisme retiré, il se posait sur du bruit de mesure normal ([ADR-0097](adr/0097-le-filtre-lisse-le-dosage-juge.md)). Il n'existe plus d'état « instable » séparé de la valeur : la médiane absorbe les aberrations par construction.

| Champ | Type | Description |
|---|---|---|
| `state` | string | **Même discriminant** que celui qui qualifie les causes de refus de dosage (`DoseFilterState`, `src/dosing_logic.h`) — dit **pourquoi** `filtered` est `null` quand c'est le cas, sans dupliquer de logique. Valeurs : `ready` (valeur officielle disponible), `warmup` (fenêtre de lissage en remplissage), `frozen` (sonde figée), `stale` (plus aucune mesure acceptée), `gate_closed` (mesures non représentatives — purge / eau à l'arrêt), `unknown` (repli, ne devrait pas survenir). |
| `rejected` | integer | Compteur glissant de rejets (étage 2 : NaN / hors plage physique) — **diagnostic seul**, ne décide plus rien (plus de seuil d'instabilité consécutif). Sature à 255. |

**Échelle** : `median` / `filtered` de cette route sont **corrigés** côté pH (offset appliqué, [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) ; `raw` reste **brut**, sans offset.

## Trace des signaux capteurs (feature-057, v2.7.0)

Réintroduction **fidèle** (haute cadence) de la trace des signaux capteurs retirée en v2.5.0 (feature-045), **étendue à l'ORP** (l'original ne couvrait que le pH) puis à la **médiane** (feature-153, v2.69.0). Un ring buffer firmware de `kSensorTraceBufferSize = 200` échantillons (était 300 avant la v2.69.0 — gain d'information payé en réduisant la durée du tampon, pas en l'agrandissant) enregistre les valeurs brutes, **médianes** et lissées à **chaque cycle capteur** (~5 s), cadence bien supérieure aux 5 s du WebSocket sur toute la fenêtre. Buffer **volatil** (perdu au reboot). Détail firmware : [`docs/subsystems/sensors.md`](subsystems/sensors.md#trace-des-signaux-capteurs--feature-057).

### `GET /debug/sensor_trace` — pas d'auth (cohérent avec autres `/debug/*`)

Dump JSON **paginé** du ring buffer, du plus ancien au plus récent. Handler **non bloquant** (< 50 ms) : lecture lock-free du buffer, réponse pré-dimensionnée (`beginResponseStream(type, taille)` — une seule allocation exacte, aucun `resize()` en cours de sérialisation), aucune allocation dans la loop.

> ⚠️ **Pagination obligatoire depuis la v2.10.1** (bug-crash-sensor-trace-badalloc) : la route sérialisait auparavant les échantillons en une seule réponse (~20 Ko contigus dans le `cbuf` d'`AsyncResponseStream`) — sur heap fragmenté (cf. [ADR-0029](adr/0029-auto-guerison-heap.md), le plus gros bloc libre peut descendre sous 8 Ko), l'allocation levait un `std::bad_alloc` non rattrapé sur la tâche `async_tcp` → `abort()` → **reboot** (coredump v2.10.0 du 2026-07-23, déclenché par l'ouverture de l'onglet Avancé). Chaque page est désormais bornée à `kSensorTraceMaxSamplesPerPage = 38` échantillons depuis la v2.69.0 (feature-153 ; était 50) — mesure réelle **avant** ce lot, le 2026-10-11 : **5 905 o** pour 50 échantillons (`curl /debug/sensor_trace?offset=0 | wc -c`), pas les ≈ 4,5 Ko/3,5 Ko longtemps recopiés sans remesurer (ADR-0073). Les deux champs `phMedian`/`orpMedian` coûtent **+34,7 o/échantillon** (mesurés par réinjection dans la page réelle) ; 38 échantillons/page → **5 775 o MESURÉS** le 2026-10-11 sur le firmware v2.69.0 déployé (projection `~5 825 o` juste à 0,9 % près). La page a donc **rétréci** (5 905 → 5 775 o). ⚠️ Mesurer sur une page **sans champ `null`** (`offset=20`, pas `offset=0`) : au démarrage les premiers échantillons portent des médianes nulles et `null` est plus court qu'une valeur — la page à `offset=0` ne pesait que 5 722 o et sous-estimait le pire cas.

```bash
curl "http://poolcontroller.local/debug/sensor_trace?offset=0&count=50"
```

**Paramètres de query** (optionnels — sans paramètre, la route retourne la **première page**) :

| Paramètre | Défaut | Description |
|-----------|--------|-------------|
| `offset` | `0` | Index du premier échantillon (chronologique, 0 = le plus ancien). |
| `count` | `kSensorTraceMaxSamplesPerPage` (38) | Nombre d'échantillons demandés. `0` ou valeur invalide → défaut. |

**Clamp silencieux** (jamais de `400`, calcul pur `computeTracePage()` dans [`src/sensor_trace_page.h`](../src/sensor_trace_page.h)) :
- `offset > count total` → page vide (`returned: 0`) ;
- `count > 38` → borné à 38 ;
- `offset + count` déborde → tronqué aux échantillons restants. Invariants garantis quel que soit l'input : `offset ≤ total`, `returned ≤ 38`, `offset + returned ≤ total`.

**Réponse 200**

```json
{
  "count": 200,
  "offset": 0,
  "returned": 38,
  "interval_ms": 5000,
  "now": 3600000,
  "samples": [
    { "t": 3595000, "phRaw": 7.241, "phCorrected": 7.191, "phMedian": 7.236, "phFiltered": 7.234, "orpRaw": 722.0, "orpMedian": 720.0, "orpFiltered": 720.0, "tempEau": 24.5 },
    { "t": 3600000, "phRaw": 7.238, "phCorrected": 7.188, "phMedian": 7.235, "phFiltered": 7.235, "orpRaw": 721.0, "orpMedian": 720.0, "orpFiltered": 720.0, "tempEau": 24.5 }
  ]
}
```

| Champ | Type | Description |
|-------|------|-------------|
| `count` | integer | Nombre **total** d'échantillons présents dans le buffer (0..200, depuis v2.69.0 — était 300) — permet au client d'enchaîner les pages. |
| `offset` | integer | Offset **effectif** de la page (après clamp). |
| `returned` | integer | Nombre d'échantillons dans `samples` (0..38, depuis v2.69.0 — était 50). |
| `interval_ms` | integer | Période nominale d'enregistrement (`kPhOrpSensorIntervalMs`, ~5000 ms). |
| `now` | integer | `millis()` courant — permet à l'UI de reconstruire l'axe temps relatif (`t − now`). |
| `samples[].t` | integer | Timestamp de l'échantillon en ms (`millis()` au moment de l'enregistrement). |
| `samples[].phRaw` / `phFiltered` | float \| null | pH brut / lissé, **tous deux SANS offset** (mesure capteur telle quelle, arrondi ×1000). `null` si NaN (lecture échouée / filtre non amorcé). |
| `samples[].phCorrected` | float \| null | **feature-067 (v2.20.0)** — pH brut **+ offset logiciel** (`ph_offset`, arrondi ×1000). `null` si le brut est NaN (l'offset ne fabrique jamais une mesure). À offset nul, égal à `phRaw`. |
| `samples[].phMedian` | float \| null | **feature-153 (v2.69.0)** — médiane glissante pH, **échelle BRUTE** (SANS offset, `_phFilter.median()` — c'est le navigateur qui ajoute l'offset à l'affichage, cf. note ci-dessous). `null` si fenêtre pas encore pleine (warmup) ou brut NaN. |
| `samples[].orpRaw` / `orpFiltered` | float \| null | ORP brut / lissé en mV (arrondi ×10). `null` si NaN. |
| `samples[].orpMedian` | float \| null | **feature-153 (v2.69.0)** — équivalent ORP (mV, arrondi ×10, échelle brute — l'ORP n'a pas d'offset logiciel). |
| `samples[].tempEau` | float \| null | T° eau envoyée à l'EZO pour compensation (°C, arrondi ×10). `null` si sonde absente. |

> ⚠️ **Exception à la règle « seuls les `*Raw` sont bruts »** : dans cette trace de **diagnostic**, `phFiltered` **et** `phMedian` sont la sortie du filtre **sans offset** (le filtre travaille sur le brut, [ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Seul `phCorrected` porte l'offset. L'objectif est de pouvoir dire à tout instant **ce qui vient de l'EZO** et **ce qui vient de la correction**. Ailleurs (`/data`, WebSocket, MQTT), `phFiltered` est **corrigé**. **Côté navigateur** (carte « Signaux capteurs », [page-settings.md](features/page-settings.md#card-signaux-capteurs-feature-153-v2690)), l'offset pH est ajouté aux trois courbes de la Haute cadence **à l'affichage**, pour que cette vue, la vue Journée et le tableau de bord restent sur la même échelle. Coût mémoire : l'échantillon pèse désormais **36 octets** (buffer de 200 points = 7 200 o RAM, depuis v2.69.0 — était 28 octets × 300 = 8 400 o) et une page de 38 échantillons pèse **5 775 o de JSON, mesurés** le 2026-10-11 sur firmware déployé (référence avant ce lot : 5 905 o pour 50 échantillons de 28 o).

**Erreurs**

- `503` `{"error":"heap fragmenté, réessayez"}` — garde-fou défensif : si la taille mesurée de la page (`measureJson`) + marge (`kSensorTraceHeapMarginBytes = 4096`) dépasse le plus gros bloc **réellement allouable** (`heapLargestFreeBlock8Bit()` depuis la **v2.40.0**), la route répond proprement au lieu de crasher. Réessayer plus tard (l'auto-guérison heap, ADR-0029, finit par restaurer la situation).

  > **v2.40.0 (bug-080 lot B-2, item A16)** : c'était la **dernière** garde 503 restée sur `ESP.getMaxAllocHeap()`, qui compte l'**IRAM 32 bits** inutilisable par le `new` du `cbuf` d'`AsyncResponseStream` ([ADR-0038](adr/0038-metrique-memoire-allouable-8-bits.md), [ADR-0058](adr/0058-gardes-heap-metrique-8-bits.md)). **Le seuil est inchangé — on corrige la mesure, pas la marge.** Comportement quasi identique en régime normal (**1 024 o** d'écart mesuré le 05/09/2026), mais la mesure brute **reste figée en famine** pendant que le 8 bits s'effondre : la garde cessait alors de protéger tout en paraissant fonctionner. Pour un client, **rien ne change** : mêmes code, message et sémantique de réessai.

> ℹ️ **Glissement du ring entre deux pages** : le buffer continue d'avancer (~1 échantillon toutes les 5 s) pendant qu'un client enchaîne les pages — les bornes ne sont pas ancrées sur un identifiant d'échantillon, un décalage d'~1 échantillon entre pages est donc possible et **toléré** (route de diagnostic, contrat lock-free `SensorTraceSample` inchangé).

Usage : diagnostiquer le **bruit** ou une **oscillation** de la chaîne de mesure en comparant brut, médiane et lissé sur une fenêtre continue, depuis l'onglet **Avancé** — l'UI agrège les pages de façon transparente (voir [`docs/features/page-settings.md`](features/page-settings.md#card-signaux-capteurs-feature-153-v2690)).

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

## Recensement du tas et lecture de contenu (feature-128, [ADR-0081](adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md))

Instrumentation de diagnostic née de l'incident
[`2026-09-10-recidive-fragmentation-v2500.md`](incidents/2026-09-10-recidive-fragmentation-v2500.md) :
la fenêtre du 12-13/09 a établi **que** le plus gros bloc contigu du tas
s'effondre en quatre marches discrètes, sans établir **qui** fait ces
découpes. `heap_caps_walk()` recense le tas interne 8 bits **bloc par bloc**
(adresse, taille, occupé/libre) et permet de lire le **contenu** d'un bloc
désigné — une `String` de journal se lit alors directement en clair, ce
qu'aucun agrégat (`/get-system-info?hf=1`) ne peut donner.

⚠️ **Ces quatre routes sont les premières `/debug/*` de ce dépôt à exiger
`RouteProtection::CRITICAL` ET à refuser explicitement 403 en mode
`auth_enabled = false`.** Toutes les autres routes `/debug/*` documentées
ci-dessus sont **sans authentification** « par cohérence avec les routes
voisines » — une convention héritée qui n'a jamais été remise en cause parce
qu'aucune ne lit la mémoire du processus. `src/web_routes_heap.cpp` porte
donc une convention **délibérément inverse**, écrite en tête de fichier :
chaque handler commence par

```cpp
if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
  sendJsonError(req, 403, "forbidden");
  return;
}
REQUIRE_AUTH(req, RouteProtection::CRITICAL);
```

**Pourquoi ce refus explicite est nécessaire, et pas seulement `CRITICAL`
seul** : dans ce projet, `RouteProtection::CRITICAL` est **décoratif**
(`src/auth.cpp:204`, `checkAuth`, ne teste `level` que contre `NONE` — donc
`CRITICAL` se comporte exactement comme `WRITE`), et `src/auth.cpp:199` fait
passer **toutes** les routes en clair dès que l'authentification est
désactivée (`auth_enabled = false`, réglable depuis `POST /save-config`) ou
au premier démarrage, avant que l'assistant de configuration n'ait tourné.
Sans le refus 403 **explicite et antérieur** ci-dessus, une route qui lit la
mémoire du processus serait ouverte à quiconque dès que l'authentification
est coupée — ce qui est déjà vrai de toute route `CRITICAL` de ce dépôt
aujourd'hui, mais que ce lot refuse d'hériter. C'est cette garde, pas
l'annotation `CRITICAL`, qui rend la protection réelle.

### Protocole en deux temps — le parcours du tas n'a jamais lieu sur `async_tcp`

`heap_caps_walk()` s'exécute interruptions masquées pendant tout le
parcours (spinlock inter-cœurs, établi par désassemblage — voir l'ADR).
L'exécuter directement dans un handler HTTP masquerait les interruptions de
la tâche qui sert le WiFi et l'ensemble des requêtes. Les quatre routes
fonctionnent donc en **deux temps**, à l'idiome de `POST /debug/sensor_filter_reset`
(section « Endpoints ajoutés en feature-024 » plus haut dans ce document) :

1. **`POST`** pose une demande, consommée par la **loop** (jamais par
   `async_tcp`) au tour suivant (< 100 ms) — réponse **`202`** = accepté,
   pas exécuté.
2. **`GET`** lit l'instantané **gelé** produit par ce parcours.

Un `GET` avant que la loop n'ait traité la demande rend `409 {"error":"not_ready"}`.

### `POST /debug/heap_census?used_from=&used_to=` — CRITICAL

Demande un recensement : les **N plus gros blocs libres** (top-N approché) et
les blocs **occupés** dont l'adresse tombe dans la fenêtre `[used_from,
used_to)`.

```bash
curl -u admin:monmotdepasse -X POST \
  "http://poolcontroller.local/debug/heap_census?used_from=0x3FFB0000&used_to=0x3FFB4000"
```

**Paramètres** (query string, base 0 — `0x…` ou décimal) :

| Paramètre | Défaut | Description |
|---|---|---|
| `used_from` | `0` | Borne basse (incluse) de la fenêtre de blocs **occupés** à détailler |
| `used_to` | `0` | Borne haute (exclue) |

⚠️ **`used_to == 0` demande une fenêtre VIDE, pas « toute la mémoire ».** C'est
contre-intuitif et délibéré (`heapCensusInWindow()`, `src/heap_census_logic.h` :
`hi == 0` rend toujours `false`) : **omettre les deux paramètres** — le cas le
plus fréquent d'un premier appel exploratoire — recense quand même le tas
(les **agrégats** `free_block_count`/`used_block_count` du `GET` sont
toujours remplis), mais **aucun** bloc occupé n'est publié en détail tant
qu'une fenêtre explicite n'est pas fournie. Pour explorer, fixer `used_to` à
une adresse plausible (ex. `heap_end` d'un premier recensement) puis affiner.

**Réponse 202**

```json
{ "queued": true, "request_id": 3, "build": { "census": 1, "peek": 0 } }
```

`request_id` est un compteur **bookkeeping** de ce fichier de routes,
distinct du `seq` firmware publié par le `GET` (généré par la loop à chaque
recensement **abouti**) : il permet de corréler une réponse `202` avec les
`GET` suivants, y compris quand la demande est encore en attente.

**Erreurs**

| Code | `error` | Cause |
|---|---|---|
| `403` | `forbidden` | authentification désactivée ou premier démarrage détecté (garde ci-dessus, avant même `REQUIRE_AUTH`) |
| `409` | `busy` | un cycle recensement/lecture est déjà en cours |
| `429` | `too_soon` | plancher `kHeapCensusMinIntervalMs` (5 s, **partagé** avec `/debug/heap_peek`) non écoulé — en-tête `Retry-After` posé |
| `410` | `disabled` | l'instrument s'est **auto-désactivé définitivement** (un parcours a dépassé `kHeapCensusMaxDurationUs`, 1,5 ms — jamais réarmé) |

### `GET /debug/heap_census?kind=free|used&offset=&count=` — CRITICAL

Lit l'instantané figé du dernier recensement abouti, paginé.

```bash
curl -u admin:monmotdepasse \
  "http://poolcontroller.local/debug/heap_census?kind=used&offset=0&count=24"
```

**Paramètres** : `kind` (`free` par défaut, ou `used`), `offset`/`count`
(pagination, `count` défaut = page pleine, `kHeapCensusPageEntries` = 24).

**Réponse 200**

```json
{
  "seq": 3,
  "took_at_ms": 512340,
  "heap_start": 1073889280,
  "heap_end": 1073938432,
  "used_from": 1073889280,
  "used_to": 1073893376,
  "free_block_count": 118,
  "used_block_count": 892,
  "used_in_window_count": 7,
  "used_truncated": false,
  "floor_free_blocks": 118,
  "floor_used_blocks": 892,
  "concordance": "match",
  "kind": "used",
  "offset": 0,
  "returned": 7,
  "total": 7,
  "truncated": false,
  "entries": [
    { "addr": 1073889312, "size": 48, "alloc_size": 36 }
  ],
  "walk_us": 812,
  "max_walk_us": 1104,
  "disabled": false,
  "build": { "census": 1, "peek": 0 }
}
```

| Champ | Type | Signification |
|---|---|---|
| `seq` | `uint32` | numéro de génération de ce recensement (1 = premier depuis le boot) |
| `took_at_ms` | `uint32` | `millis()` loop au moment du parcours |
| `heap_start` / `heap_end` | `uint32` | enveloppe (min/max) de **toutes** les régions parcourues — **pas nécessairement contiguë** si plusieurs tas portent le même masque de capacités |
| `used_from` / `used_to` | `uint32` | fenêtre **demandée** (écho, pour diagnostic) |
| `free_block_count` / `used_block_count` | `uint32` | agrégats **totaux** du parcours — à comparer à `fb`/`ab` de `/get-system-info?hf=1` (masque `i8`, `0x0804`) : c'est le **test de cohérence de l'instrument lui-même** |
| `used_in_window_count` | `uint32` | nombre de blocs occupés dont l'adresse tombe dans `[used_from, used_to)` |
| `used_truncated` | booléen | `true` si `used_in_window_count` dépasse `kHeapCensusMaxUsedBlocks` (96) — troncature du **parcours**, jamais silencieuse |
| `floor_free_blocks` / `floor_used_blocks` | `uint32` | écho de `fb`/`ab` **déjà échantillonnés** par le tour de rôle des planchers glissants (aucun second `heap_caps_get_info()` sur ce chemin) |
| `concordance` | `"match"` \| `"mismatch"` \| `"undetermined"` | verdict de cohérence entre les compteurs de **ce** recensement et `floor_*` — `undetermined` si `floor_*` valent `null` (masque jamais échantillonné) |
| `kind` | `"free"` \| `"used"` | quelle liste `entries` porte |
| `offset` / `returned` / `total` | `uint32` | pagination — `total` = taille de la liste **retenue** (top-N approché pour `free`, fenêtre demandée pour `used`), pas le compte brut du parcours |
| `truncated` | booléen | troncature de **pagination** (`offset + returned < total`) — distincte de `used_truncated` (troncature du **parcours**, côté loop) |
| `entries[].addr` / `.size` | `uint32` | adresse et taille TLSF (en-tête compris) du bloc |
| `entries[].alloc_size` | `uint32` \| sentinelle | **`used` uniquement** — octets réellement demandés (`poison_head_t.alloc_size`), ou `kHeapCensusAllocSizeUnknown` (`0xFFFFFFFF`, [ADR-0072](adr/0072-absence-de-valeur-hors-du-domaine.md)) si le canari de poisoning n'est pas conforme |
| `walk_us` / `max_walk_us` / `disabled` | `uint32` / `uint32` / booléen | voir « Champs d'observabilité C4 » ci-dessous |
| `build.census` / `build.peek` | `0` \| `1` | voir « Marqueur de build » ci-dessous |

**Aucun champ de `entries[]` ne porte d'octet de contenu** — adresses et
tailles seulement (R2). Lire le contenu d'un bloc exige `/debug/heap_peek`
(R3, ci-dessous).

**Erreurs** (toutes portent `walk_us`/`max_walk_us`/`disabled`, y compris en
échec — c'est précisément au moment d'un `410` que l'opérant a besoin de
savoir si l'instrument s'est auto-désactivé) :

| Code | `error` | Cause |
|---|---|---|
| `403` | `forbidden` | authentification désactivée ou premier démarrage détecté |
| `409` | `not_ready` | toujours `IDLE`/`REQUESTED` : aucune demande aboutie à lire |
| `409` | `busy` | une lecture concurrente est déjà en cours (course improbable, un seul appelant `async_tcp`) |
| `410` | `stale` | l'instantané a dépassé `kHeapCensusStaleMs` (5 min) sans avoir été lu — reposer une demande |
| `503` | — | garde mémoire (`heapLargestFreeBlock8Bit()` insuffisant pour sérialiser la réponse) — réessayer |

### `POST /debug/heap_peek?addr=&len=` — CRITICAL, **non livrée en production** ([ADR-0081](adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md))

Demande la lecture des premiers octets d'un bloc **occupé**, désigné par son
adresse (obtenue par un `GET /debug/heap_census?kind=used` préalable).

⚠️ **Cette route n'existe dans le binaire que si le firmware a été compilé
avec `PLATFORMIO_BUILD_FLAGS=-DPOOL_HEAP_PEEK=1` — jamais dans un build de
release.** Sur un firmware de production, elle répond **404** (route jamais
enregistrée). Voir l'ADR pour le mécanisme complet de non-livraison,
vérifié à la fois sur l'arbre (CI) et sur l'artefact publié (`/release`).

```bash
curl -u admin:monmotdepasse -X POST \
  "http://poolcontroller.local/debug/heap_peek?addr=0x3FFB0210&len=64"
```

**Paramètres** (obligatoires) : `addr`, `len` (≤ `kHeapPeekMaxBytes` = 128
octets par requête ; clampé à la taille réellement allouée du bloc désigné).

**Réponse 202**

```json
{ "queued": true, "build": { "census": 1, "peek": 1 } }
```

**Erreurs**

| Code | `error` | Cause |
|---|---|---|
| `403` | `forbidden` | authentification désactivée ou premier démarrage détecté |
| `400` | `missing_param` | `addr` ou `len` absent |
| `400` | `invalid_len` | `len == 0` |
| `409` | `busy` | une lecture est déjà en cours |
| `429` | `too_soon` | plancher `kHeapCensusMinIntervalMs` non écoulé — **partagé** avec `/debug/heap_census` (même coût O(blocs), même horodatage de dernier parcours) |
| `410` | `disabled` | instrument auto-désactivé définitivement |
| `403` | `budget_exhausted` | budget cumulé `kHeapPeekMaxBytesPerBoot` (8 192 o depuis le boot) épuisé — refus jusqu'au redémarrage. ⚠️ Ce budget borne un **vidage en masse**, pas la divulgation d'un secret isolé (un mot de passe WiFi, des identifiants MQTT ou un jeton tiennent en moins de 200 o) : voir l'ADR |

### `GET /debug/heap_peek` — CRITICAL, **non livrée en production**

Lit le résultat de la dernière demande aboutie : hexadécimal **et** ASCII
imprimable des octets copiés.

```bash
curl -u admin:monmotdepasse "http://poolcontroller.local/debug/heap_peek"
```

**Réponse 200**

```json
{
  "seq": 12,
  "took_at_ms": 519880,
  "addr": 1073889808,
  "alloc_size": 42,
  "len": 42,
  "found": true,
  "canary_valid": true,
  "stable": true,
  "budget_clamped": false,
  "budget_remaining": 8024,
  "hex": "323032362d30392d31342031323a30303a3030205b494e464f5d20486561703a...",
  "ascii": "2026-09-14 12:00:00 [INFO] Heap:...",
  "build": { "census": 1, "peek": 1 }
}
```

| Champ | Type | Signification |
|---|---|---|
| `addr` / `alloc_size` / `len` | `uint32` | adresse demandée, octets réellement alloués du bloc, octets **effectivement copiés** (`len ≤` `alloc_size` et `≤ 128`) |
| `found` | booléen | `false` si aucun bloc **occupé** ne commence exactement à `addr` sur le parcours **frais** de cette lecture (jamais un instantané vieillissant) |
| `canary_valid` | booléen | canari de poisoning conforme **avant** la copie — `false` ⇒ `hex`/`ascii` vides, aucune lecture hors du domaine du bloc |
| `stable` | booléen | canari re-testé **après** la copie — `false` signale un bloc modifié pendant la lecture (structurel uniquement, ne relit jamais le contenu) |
| `budget_clamped` | booléen | la longueur copiée a été réduite par le budget cumulé `kHeapPeekMaxBytesPerBoot` — jamais silencieux |
| `budget_remaining` | `uint32` | octets restants avant épuisement du budget de ce boot |
| `hex` / `ascii` | `string` | contenu des `len` octets copiés (à partir de `addr + 8`, l'en-tête `poison_head_t` sauté) |

⚠️ **`hex`/`ascii` ne sont ni journalisés ni destinés à être versionnés.** Voir
la note d'incident : adresse, taille et identification se consignent,
**jamais le contenu**.

**Erreurs** : `403 forbidden` (auth désactivée/premier démarrage) ·
`409 not_ready` · `409 busy` · `410 stale` · `503` (garde mémoire).

### Champs d'observabilité C4 — `walk_us` / `max_walk_us` / `disabled`

**Partagés entre R2 et R3** (même chronomètre, même coût O(blocs)), publiés
sur `GET /debug/heap_census`, succès **et** erreur :

| Champ | Signification |
|---|---|
| `walk_us` | durée du **dernier** parcours de tas, en µs |
| `max_walk_us` | pire parcours observé depuis le boot |
| `disabled` | `true` si l'instrument s'est auto-désactivé **définitivement** (C1, `pool-chemistry`) : un parcours a dépassé `kHeapCensusMaxDurationUs` (1,5 ms). La base de calibrage étant **mouvante** (`wmax` de référence : 516 µs en v2.49.0, 711 µs six jours plus tard, +38 %), l'instrument se coupe lui-même plutôt que de se fier à une estimation périmée — jamais réarmé |

### Marqueur de build — `build.census` / `build.peek`

Posé sur **toute** réponse JSON de ces quatre routes, succès ou erreur (coût
négligeable, ~30 o) : `census` vaut toujours `1` (R2 compilé
inconditionnellement), `peek` vaut `1` si et seulement si le binaire qui
répond a été compilé avec `POOL_HEAP_PEEK`. Permet de prouver, **a
posteriori** et à partir d'une seule réponse archivée (y compris une erreur),
quel binaire tournait pendant une fenêtre d'observation.

---

## Routes de debug supprimées (v2.5.0, feature-045)

Les routes suivantes, ajoutées pour la campagne de diagnostic d'oscillation pH (2026-05/06), ont été retirées et répondent désormais **404** :

- `GET /debug/ph_trace` et `POST /debug/ph_trace_clear` (trace ring buffer pH) — **réintroduites en v2.7.0** sous `GET /debug/sensor_trace` + `POST /debug/sensor_trace_clear` (pH **et** ORP ; voir section ci-dessus).
- `GET /debug/wifi_pause` et `POST /debug/wifi_pause` (pause WiFi temporaire) — **non réintroduites**.
- `POST /debug/ezo_command` et `POST /debug/ezo_factory` (diagnostic Atlas EZO) — **non réintroduites**.

Le code est récupérable via `git revert` du commit de la feature-045 ; l'état complet avec ces outils est figé au tag `v2.4.0`. Voir [feature-045](../specs/features/done/feature-045-retrait-outils-debug.md).
