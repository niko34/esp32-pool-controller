# Subsystem — `ws_manager`

- **Fichiers** : [`src/ws_manager.h`](../../src/ws_manager.h), [`src/ws_manager.cpp`](../../src/ws_manager.cpp)
- **Singleton** : `extern WsManager wsManager;`
- **Endpoint** : `ws://poolcontroller.local/ws`

## Rôle

Bus de diffusion temps réel vers tous les clients UI connectés. Push **toutes les 5 s** des données capteur + config + logs. Remplace le polling HTTP. Voir [ADR-0005](../adr/0005-websocket-push-sans-polling.md).

## API publique

```cpp
void begin(AsyncWebServer* server);
void update();                         // cleanup + push capteurs 5s
void broadcastSensorData();            // push immédiat, toutes données
void broadcastConfig();                // push immédiat, config actuelle
void broadcastLog(const LogEntry&);    // push un log
bool hasClients() const;
```

## Timing

- Intervalle de push périodique : **5000 ms** (`kSensorPushIntervalMs` [`ws_manager.h:25`](../../src/ws_manager.h:25)).
- Push **immédiat** sur événement :
  - Changement d'état filtration (démarrage / arrêt)
  - Démarrage / arrêt injection pH ou ORP
  - Changement de mode (régulation / filtration / lighting)
  - Sauvegarde config (`POST /save-config`)
  - Commande HA modifiant la config via MQTT (`drainCommandQueue`, v2.14.1)
  - Nouveau log (callback `systemLogger.setLogCallback()`)

## Authentification

Le WebSocket exige un token valide. Architecture :
1. Client appelle `GET /auth/token` (HTTP, avec Basic Auth) pour obtenir un token court.
2. Client ouvre `ws://.../ws?token=<token>` (ou envoie `{"type":"auth","token":"..."}` après connexion).
3. `_authenticatedClients` (std::set<uint32_t>) garde les client IDs validés.
4. Les messages des clients non-authentifiés sont ignorés.

⚠️ La vérification du token dans `_onData()` passe par `authManager.secureTokenEquals()` — **comparaison à temps constant**, même exigence que l'auth HTTP (v2.11.2, feature-028 ; jamais de `==` / `!=` direct sur le token, voir [auth.md](auth.md#comparaison-de-token-à-temps-constant-v2112-feature-028)). Token rejeté → log `[WS] Token rejeté` + fermeture de la connexion.

Voir [`ws_manager.cpp`](../../src/ws_manager.cpp) `_onClientConnect()` et `_onData()`.

## Format des messages push

JSON avec un champ `type` :
- `type: "sensor_data"` → payload identique à `/data` (voir [docs/API.md](../API.md))
- `type: "config"` → payload identique à `/get-config`
- `type: "log"` → `{timestamp, level, message}`

> **Déclencheurs du message `config`** — le broadcast `config` (via le flag `_pendingConfigBroadcast` posé par `requestConfigBroadcast()`, consommé sur `loopTask`) a **deux** origines :
> 1. `POST /save-config` (`web_routes_config.cpp`) — sauvegarde de la config depuis l'UI web.
> 2. `MqttManager::drainCommandQueue()` (v2.14.1, bug-sync-ws-config-mqtt) — après application d'une commande HA modifiant la config (hors `Reboot`), pour que l'UI web reflète sous ≤ 5 s un changement fait depuis Home Assistant sans reload. Voir [mqtt-manager.md](mqtt-manager.md#notification-ui-temps-réel--broadcast-ws-config-bug-sync-ws-config-mqtt-v2141).

### Champs notables de `sensor_data`

Au-delà des mesures capteur (pH, ORP, température…) et des états de contrôle (filtration, dosage, éclairage), la payload `sensor_data` transporte aussi quelques champs d'observabilité :

| Champ | Type | Source | Notes |
|-------|------|--------|-------|
| `time_synced` | bool | `time(nullptr) >= kMinValidEpoch` | Vrai dès que NTP/RTC a fourni une heure valide |
| `uptime_ms` | uint | `millis()` | Permet au client de calculer le timestamp boot |
| `reset_reason` | string | `esp_reset_reason()` | Voir tableau ci-dessous |
| `mqtt_connected` | bool | `mqttManager.isConnected()` | État de connexion broker en temps réel — voir ci-dessous |

#### Champ `mqtt_connected` (sensor_data)

Ajouté par feature-015 pour rafraîchir le badge UI Paramètres → MQTT sans nécessiter de reload page. Lit la single source of truth `connectedAtomic` du `MqttManager` (introduit par feature-014 IT2 — atomic relaxed, pas de mutex). Permet à l'UI de basculer le badge en moins de 5 s après la détection firmware d'une coupure broker.

> Le champ `mqtt_connected` est aussi présent dans la payload `config` ([`_buildConfigJson()`](../../src/ws_manager.cpp:219)) — doublon volontaire : `sensor_data` est le canal **temps réel** (push 5 s), `config` est le **snapshot stable** broadcast à la transition (save HTTP `/save-config`, ouverture de page via `/get-config`). Les deux pointent vers la même source `mqttManager.isConnected()`.

#### Champ `reset_reason` (sensor_data)

Calculé à chaque push via `esp_reset_reason()` ([`ws_manager.cpp:17`](../../src/ws_manager.cpp:17)) — la valeur reflète la cause du **dernier reboot** et reste constante pendant toute la session.

| Valeur | Cause |
|--------|-------|
| `POWER_ON` | Mise sous tension (alim ou bouton) |
| `SW_RESET` | Reboot logiciel volontaire (`ESP.restart()`, OTA, factory reset) |
| `WATCHDOG` | Task WDT, Int WDT ou WDT générique — **anormal** |
| `BROWNOUT` | Tension d'alim trop basse — **anormal** |
| `PANIC` | Crash logiciel (exception, abort) — **anormal** |
| `DEEP_SLEEP` | Sortie de deep-sleep (non utilisé sur ce projet) |
| `EXTERNAL` | Reset par broche EN |
| `UNKNOWN` | Cause non reconnue par l'IDF |

Côté UI : un toast est affiché une seule fois par session si la valeur ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`}.

## Cas limites

- **Pas de client connecté** : `broadcastSensorData()` ne fait rien (gain CPU). `hasClients()` court-circuite la construction JSON.
- **Client déconnecté sans close** : nettoyé par `AsyncWebSocket::cleanupClients()` appelé dans `update()`.
- **Push pendant write en cours** : `AsyncWebSocket` gère la file d'attente, pas de blocage du loop.
- **Heap bas** (`< kMinFreeHeapBytes = 10000`) : le push continue mais les logs WARN peuvent être générés (voir `logger.cpp`).

## Dépendances

- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) v3.6.0 — classe `AsyncWebSocket`.

## Fichiers liés

- [`src/ws_manager.h`](../../src/ws_manager.h), [`src/ws_manager.cpp`](../../src/ws_manager.cpp)
- [`src/web_server.cpp`](../../src/web_server.cpp) — instanciation du serveur
- [`src/logger.h:55`](../../src/logger.h:55) — `setLogCallback()`
- [ADR-0005](../adr/0005-websocket-push-sans-polling.md)

## Champs `sensor_data` ajoutés en feature-020 (PCB v2)

| Champ | Type | Description |
|-------|------|-------------|
| `temperature_circuit` | float \| null | T° de la sonde DS18B20 « circuit électronique » (NaN/null si non identifiée ou en erreur) |
| `sondes_identified` | bool | true ssi les 2 sondes DS18B20 sont identifiées (eau + circuit) |
| `sondes_detected` | int (0..2) | Nombre de sondes DS18B20 physiquement détectées sur le bus OneWire |

Buffer `_buildSensorJson()` agrandi de **832 → 896 octets** pour absorber ces champs (marge ~30 octets sur le payload réel mesuré).

Les champs `sondes_identified` et `sondes_detected` pilotent la chip de notification ambré sur le Dashboard côté UI (visible tant que l'identification n'est pas faite). Voir `data/app.js` `_updateSondesChip()`.

## Champs `sensor_data` ajoutés en feature-006 (injection manuelle gardée)

| Champ | Type | Description |
|-------|------|-------------|
| `ph_stab_remaining_s` | uint | Secondes restantes de stabilisation post-calibration pour la pompe **pH** (index logique 0) |
| `orp_stab_remaining_s` | uint | Secondes restantes de stabilisation post-calibration pour la pompe **ORP** (index logique 1) |

Ces champs sont le miroir exact de la garde firmware **par pompe** (`manualInjectGuardOrReject` → `getStabilizationRemainingS(0/1)`) : l'UI désactive le bouton « Injecter » d'un produit uniquement si **sa** pompe est en stabilisation (une calibration ORP ne bloque pas le bouton pH, et inversement). Le champ global `stabilization_remaining_s` (max des 2 pompes) est **conservé** pour compatibilité (badge global, anciens clients) ; `data/app.js` `getInjectBlockReason()` retombe dessus si les champs par pompe sont absents (ancien firmware).

Buffer `_buildSensorJson()` bumpe de **1408 → 1472 octets**.

## Champs `sensor_data` ajoutés en feature-011 (répartition scheduled, v2.8.0)

| Champ | Type | Description |
|-------|------|-------------|
| `ph_scheduled_flow_ml_per_min` | float \| null | Débit moyen **planifié** restant du mode Programmée pH (mL/min, arrondi 1 décimale) = volume restant / minutes de filtration restantes (bornées à minuit). `NAN` firmware → `null` explicite (hors mode `scheduled`, hors plage de filtration, heure locale invalide) — l'UI affiche « — ». |
| `orp_scheduled_flow_ml_per_min` | float \| null | Équivalent ORP (chlore). |

Valeurs lues via `PumpController.getPhScheduledPlannedFlow()` / `getOrpScheduledPlannedFlow()` (rafraîchies à chaque tour d'`update()` en loopTask). Voir [pump-controller.md §Mode scheduled](pump-controller.md#mode-scheduled) et [ADR-0021](../adr/0021-repartition-scheduled.md).

Buffer `_buildSensorJson()` bumpe de **1472 → 1536 octets** (+ `out.reserve` 1200 → 1280).

## Champs `sensor_data` ajoutés en feature-053 (Mode Boost, v2.18.0)

| Champ | Type | Description |
|-------|------|-------------|
| `boost_active` | bool | `true` tant que le **Mode Boost** (surchloration temporaire du jour) est actif. Retombe automatiquement à `false` au prochain minuit local (expiration côté firmware) → l'UI voit la fin du boost sous ≤ 5 s sans reload. |
| `boost_until` | int (epoch) | Instant d'expiration du Boost (epoch UNIX, prochain minuit local). `0` si le Boost est inactif. Permet à l'UI (carte Boost du dashboard) d'afficher l'heure de fin. |

Valeurs lues via les getters `PumpController.isBoostActive()` / `getBoostUntilEpoch()`. Un changement d'état (activation via `POST /boost/start`, désactivation via `/boost/stop` ou commande HA `{base}/boost/set`, **ou expiration à minuit**) déclenche un push WS immédiat. Voir [pump-controller.md §Mode Boost](pump-controller.md#mode-boost-feature-053), [features/page-dashboard.md](../features/page-dashboard.md#carte-boost-feature-053) et [ADR-0025](../adr/0025-mode-boost.md).

## Champs `sensor_data` ajoutés en feature-055 (effet Boost persistant, v2.18.2)

| Champ | Type | Description |
|-------|------|-------------|
| `boost_filtration_extended` | bool | `true` ssi la filtration est gérée par PoolController (= `filtrationCfg.enabled`) : le levier « filtration prolongée » du Boost s'applique effectivement. |
| `boost_chlorine_boosted` | bool | `true` ssi la régulation ORP est en mode `automatic` (= `orpRegulationMode == "automatic"`) : le levier « surchloration » (cible/limite chlore relevées) s'applique effectivement. |

Ces deux booléens reflètent les **leviers réellement actifs** du Mode Boost et sont **calculés au vol** dans `_buildSensorJson()` à chaque cycle (indépendants de `boost_active`) : ils sont donc valides après une activation depuis Home Assistant **et** après un rechargement de page — contrairement aux booléens `filtration_extended` / `chlorine_boosted` de la seule réponse HTTP `POST /boost/start` (feature-054). L'UI (`updateBoostCard`, [page-dashboard.md](../features/page-dashboard.md#carte-boost-feature-053)) les utilise pour afficher une ligne « Effet » persistante quand le Boost est actif. Aucune logique de dosage n'est touchée.

Buffer `_buildSensorJson()` bumpe de **1600 → 1664 octets**.
