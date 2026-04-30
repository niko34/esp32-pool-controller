# Page Paramètres — `/settings`

- **Fichier UI** : [`data/index.html:1065`](../../data/index.html:1065) (section `#view-settings`)
- **URL** : `http://poolcontroller.local/#/settings`

## Rôle

Page de configuration système. Structurée en **8 onglets segmentés** ([`data/index.html:1072`](../../data/index.html:1072)) qui pilotent chacun une zone distincte de la configuration firmware. Aucun onglet n'interagit directement avec les capteurs ou les pompes en production — tout passe par `POST /save-config` (CRITICAL) ou des endpoints dédiés.

## Structure — 8 onglets

| Onglet | ID panel | Rôle |
|--------|----------|------|
| Wi-Fi | `panel-wifi` | Statut SSID/IP/mDNS + lien vers l'assistant de reconfiguration |
| MQTT | `panel-mqtt` | Activation + broker (host/port/topic/auth) |
| Heure | `panel-time` | NTP ou heure manuelle, timezone, serveur NTP |
| Sécurité | `panel-security` | Changement mot de passe admin + CORS |
| Régulation pH / ORP | `panel-regulation` | Mode pilote/continu, vitesse PID, limites horaires/journalières, délai stabilisation |
| Système | `panel-system` | Version, OTA GitHub, OTA manuel, export/import historique, reboot, factory reset |
| Avancé | `panel-dev` | Affectation pompes, puissance max, débit nominal, tests pompe, infos système, logs |
| À propos | `panel-about` | Liens projet + versions des libs — contenu statique |

## Données consommées (`GET /get-config` + WebSocket `/ws` + `GET /get-system-info`)

### Wi-Fi (`panel-wifi`)
- Depuis `GET /wifi/status` : `ssid`, `ip`, `mode` (STA/AP), `mdns_hostname`.

### MQTT (`panel-mqtt`)
- `mqtt_enabled`, `mqtt_server`, `mqtt_port`, `mqtt_topic`, `mqtt_username`, `mqtt_password`
- Statut dynamique via WS : pill `mqtt_status_pill` alimentée par `mqtt_connected`. Le badge est **temps réel** (feature-015) — il bascule en moins de 5 s suivant la détection firmware d'une coupure broker, sans nécessiter de recharger la page. Le champ `mqtt_connected` est poussé à chaque tick `sensor_data` (5 s) en plus du snapshot `config` au load.
- **Placement DOM** (feature-001) : le badge `mqtt_status_pill` est placé comme frère direct du `<h2>` à l'intérieur du `card__head` (sorti du titre), pour cohérence inter-pages avec les cards Filtration et Éclairage. Une règle CSS de garde `.card__head .pill { flex-shrink: 0; white-space: nowrap; }` garantit qu'il ne se comprime pas.

### Heure (`panel-time`)
- `time_use_ntp` (bool), `time_timezone` (enum — voir [`config.h:169`](../../src/config.h:169) `TIMEZONES`), `time_ntp_server`
- Heure courante : `GET /time-now` pour pré-remplir le champ manuel.

### Sécurité (`panel-security`)
- Password admin : endpoint dédié `/auth/change-password` (ne passe **pas** par `/save-config`).
- CORS : `auth_cors_origins` (string vide / `*` / liste) — persisté par `/save-config` et **nécessite un reboot**.

### Régulation (`panel-regulation`)
- `regulation_mode` (`pilote` | `continu`) — détermine si la régulation suit la filtration ([`pump_controller.cpp`](../../src/pump_controller.cpp)).
- `regulation_speed` (`slow` | `normal` | `fast`) — sélectionne le jeu de paramètres PID. Gains exacts détaillés dans [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#algorithme-résumé).
- `ph_limit` / `orp_limit` (min/h) — mapping firmware : `ph_limit_minutes`, `orp_limit_minutes` (CHANGELOG 2026-04-24).
- `ph_daily_limit` / `orp_daily_limit` (mL) — mapping : `max_ph_ml_per_day` / `max_chlorine_ml_per_day` ([`config.h:141`](../../src/config.h:141) `SafetyLimits`).
- `stabilization_delay_min` — voir [`pump_controller.h`](../../src/pump_controller.h) `stabilizationDelayMs`.

> ⚠️ **Paramètres de sécurité NON exposés dans l'UI** (en dur dans le firmware, décision volontaire — voir [docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif](../subsystems/pump-controller.md#paramètres-en-dur-récapitulatif)) :
>
> | Paramètre | Valeur | Rôle |
> |-----------|--------|------|
> | `PH_DEADBAND` | 0.01 | Zone morte du PID pH |
> | `ORP_DEADBAND` | 2.0 mV | Zone morte du PID ORP |
> | `phStartThreshold` / `phStopThreshold` | 0.05 / 0.01 | Hystérésis démarrage/arrêt pH |
> | `orpStartThreshold` / `orpStopThreshold` | 10 / 2 mV | Hystérésis démarrage/arrêt ORP |
> | `minInjectionTimeMs` | 30 000 ms | Durée minimale d'une injection (protection pompe) |
> | `maxCyclesPerDay` | 20 | Nombre max de démarrages / 24 h glissantes |
> | `MIN_ACTIVE_DUTY` | 80 | PWM minimum pour qu'une pompe tourne effectivement |
> | `integralMax` | 50 | Anti-windup PID |
> | `kPhMaxError` / `kOrpMaxError` | 1.0 / 200 mV | Normalisation erreur PID |
>
> Toute modification de ces valeurs exige un nouveau build firmware + passage obligatoire par l'agent `pool-chemistry`.

### Système (`panel-system`)
- Version locale : `sys_current_firmware_version` (depuis `FIRMWARE_VERSION` dans [`version.h`](../../src/version.h)).
- OTA GitHub : `/check-update` → info release, `/download-update` → téléchargement + flash.
- OTA manuel : `POST /update` (multipart `.bin`).
- Historique : `/get-history?range=all` (export CSV), `/history/import` (upload CSV), `/history/clear` (DELETE all).
- Infos runtime : `GET /get-system-info` (uptime, heap, flash, mac, rssi, fs usage).

### Avancé (`panel-dev`)
- `ph_pump` / `orp_pump` (1 ou 2) — affectation des deux pompes doseuses.
- `pump1_max_duty` / `pump2_max_duty` — puissance max en régulation (%, défaut 50).
- `pump_max_flow_ml_per_min` — débit nominal pour calcul volume injecté (défaut `kPumpMaxFlowMlPerMin = 90.0` [`constants.h`](../../src/constants.h)).
- Tests pompe : `POST /pump1/on`, `/pump1/off`, `/pump2/on`, `/pump2/off` — arrêt auto après 10 s côté firmware.
- `sensor_logs_enabled` (bool) — verbosité logs capteurs pour diagnostic.
- **Card "Diagnostic crash"** — voir section dédiée ci-dessous.
- **Card "Logs"** — voir section dédiée ci-dessous.

#### Card Diagnostic crash

Positionnée entre la card "Infos système" et la card "Logs" dans le panneau Avancé. Chargée au démarrage de la page via `GET /coredump/info`.

**Contenu affiché :**

| Élément | Description |
|---------|-------------|
| Statut | "Disponible" (badge vert) ou "Aucun coredump" (badge gris) |
| Tâche | Nom FreeRTOS de la tâche crashée (`task`) — affiché si disponible |
| Exception | Code cause + libellé (`exc_cause` / `exc_cause_str`) — affiché si disponible |
| Adresse PC | Valeur hexadécimale du Program Counter (`pc`) — affichée si disponible |

**Boutons :**

| Bouton | Endpoint | Actif quand |
|--------|----------|-------------|
| Actualiser | `GET /coredump/info` | Toujours |
| Télécharger | `GET /coredump/download` → téléchargement de `coredump.bin` | Coredump disponible |
| Effacer | `DELETE /coredump` avec dialogue de confirmation | Coredump disponible |

**Workflow de décodage (hint affiché dans la card) :**

```bash
./tools/decode_coredump.sh coredump.bin
```

Le script utilise `xtensa-esp32-elf-gdb` et `esp_coredump` du penv PlatformIO pour produire un backtrace lisible avec noms de fonctions et numéros de ligne.

Voir [`docs/API.md#diagnostic-crash-coredump`](../API.md#diagnostic-crash-coredump) pour les détails des endpoints.

#### Card Logs

Liste défilante des derniers logs poussés via WebSocket + filtres par niveau (DEBUG/INFO/WARN/ERROR/CRITICAL). Quatre boutons dans la barre d'action :

| Bouton | Style | Action |
|--------|-------|--------|
| Actualiser | ghost | Force un rechargement complet via `GET /get-logs` |
| Effacer (écran) | ghost | Vide **uniquement la vue navigateur locale** (`#logs_content`, `allLogEntries`, `lastLogTimestamp`). Les logs côté ESP32 sont intacts ; un rechargement les fait réapparaître. Tooltip : *« Vide uniquement la vue actuelle, les logs restent côté ESP32 »*. |
| Télécharger | ghost | Téléchargement de `pool_logs.txt` via `GET /download-logs` |
| Effacer (firmware) | **danger (rouge)** | Dialogue `confirm()` natif puis `DELETE /logs` (cf. [`docs/API.md`](../API.md#delete-logs--write)). Vide RAM + buffer pending + supprime `/system.log` côté ESP32, vide aussi la vue locale, toast de succès `Logs effacés (RAM + fichier)`. Tooltip : *« Vide la mémoire et supprime le fichier persistant côté ESP32 »*. |

Toggles complémentaires : `Auto (5s)` (rafraîchissement automatique), `Scroll auto` (suivi de fin), `sensor_logs_enabled` (verbosité capteurs).

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Sauvegarder config (tous champs sauf password) | `POST /save-config` | CRITICAL |
| Changer mot de passe admin | `POST /auth/change-password` (body `{current, new}`) | CRITICAL |
| Scanner Wi-Fi | `POST /wifi/scan` | WRITE |
| Déconnecter Wi-Fi | `POST /wifi/disconnect` | CRITICAL |
| Vérifier une update | `GET /check-update` | READ |
| Installer une update GitHub | `POST /download-update` | CRITICAL |
| Update manuelle (fichier .bin) | `POST /update` (multipart) | CRITICAL |
| Test pompe 1/2 | `POST /pump1/on` / `/pump2/on` | WRITE |
| Arrêt test pompe | `POST /pump1/off` / `/pump2/off` | WRITE |
| Export historique CSV | `GET /get-history?range=all` | READ |
| Import historique CSV | `POST /history/import` (multipart) | CRITICAL |
| Purger historique | `POST /history/clear` | CRITICAL |
| Redémarrer | `POST /reboot` | CRITICAL |
| Réinitialisation d'usine | `POST /factory-reset` | CRITICAL |
| Lire heure serveur | `GET /time-now` | READ |
| Statut coredump | `GET /coredump/info` | WRITE |
| Télécharger coredump | `GET /coredump/download` | WRITE |
| Effacer coredump | `DELETE /coredump` | WRITE |
| Effacer logs (firmware) | `DELETE /logs` | WRITE |
| Télécharger logs | `GET /download-logs` | WRITE |

Auth = le niveau minimum requis, voir [`docs/API.md`](../API.md).

## Règles firmware

- **Reboot obligatoire** pour appliquer : CORS, bascule Wi-Fi STA ↔ AP, changement de timezone (nouvel env TZ).
- **Factory reset** ([`web_routes_config.cpp`](../../src/web_routes_config.cpp) `/factory-reset`) : efface NVS + config LittleFS + calibrations, redémarre en mode AP `PoolControllerAP` sur `192.168.4.1`.
- **OTA** : partition active préservée, partitions app0/app1 = 1408 KB chacune, historique isolé sur une partition dédiée `history` 64 KB, coredump sur partition dédiée 64 KB — voir [ADR-0009](../adr/0009-partition-coredump.md) et [`partitions.csv`](../../partitions.csv).
- **Mot de passe** : hashé par `hashPassword()` avec salt ([`auth_*.cpp`](../../src/)), stocké en NVS, jamais renvoyé par l'API.
- **CORS** : chaîne vide = désactivé ; `*` = wildcard ; liste = origines autorisées séparées par `,`.

## Cas limites

- **Mot de passe < 8 car. / sans chiffre / sans spécial** : bloqué côté UI ([`data/index.html:1241`](../../data/index.html:1241)) — aucun appel réseau.
- **OTA en cours** : `PumpController.setOtaInProgress(true)` arrête les pompes doseuses (voir [`pump_controller.cpp`](../../src/pump_controller.cpp)) et la filtration reste à son état courant.
- **Import CSV malformé** : ligne rejetée individuellement, le reste est importé. Colonnes attendues : `datetime, ph, orp, temperature, filtration, dosing, granularity` — voir [`web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303).
- **Test pompe** : timeout 10 s côté firmware ; si la connexion réseau meurt, la pompe s'arrête quand même.
- **Factory reset pendant une injection** : l'injection est interrompue par le reboot, les cumuls NVS sont effacés.

## Interaction MQTT / Home Assistant

Aucune entité HA pour les paramètres — la page Settings est purement admin (config ESP32).
Le champ `mqtt_enabled` pilote directement la présence / l'absence du client MQTT dans `loop()` ([`mqtt_manager.cpp`](../../src/mqtt_manager.cpp)).

## Fichiers

- [`data/index.html:1065`](../../data/index.html:1065) — structure HTML (tabs + panels)
- [`data/app.js`](../../data/app.js) — handlers des 8 onglets (`setupWifi`, `setupMqtt`, `setupTime`, `setupSecurity`, `setupRegulation`, `setupSystem`, `setupDev`)
- [`src/config.h`](../../src/config.h) — structs `MqttConfig`, `AuthConfig`, `PumpControlParams`, `SafetyLimits`, `PumpProtection`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — `/save-config`, `/reboot`, `/reboot-ap`, `/factory-reset`
- [`src/web_routes_auth.cpp`](../../src/) / [`src/auth_manager.cpp`](../../src/) — endpoints `/auth/*`
- [`src/web_routes_wifi.cpp`](../../src/) — `/wifi/scan`, `/wifi/status`, `/wifi/disconnect`, `/wifi/ap/disable`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — `/pump1/*`, `/pump2/*`
- [`src/web_routes_data.cpp:303`](../../src/web_routes_data.cpp:303) — historique (`/get-history`, `/history/import`, `/history/clear`)
- [`src/ota.cpp`](../../src/) / [`src/update.cpp`](../../src/) — `/update`, `/check-update`, `/download-update`

## Specs historiques

Aucune spec dédiée. Les changements récents (renommage `ph_limit_minutes`, suppression `min_pause`, ajout `stabilization_delay_min`) sont tracés dans [`CHANGELOG.md`](../../CHANGELOG.md).
