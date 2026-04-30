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
  - Sauvegarde config
  - Nouveau log (callback `systemLogger.setLogCallback()`)

## Authentification

Le WebSocket exige un token valide. Architecture :
1. Client appelle `GET /auth/token` (HTTP, avec Basic Auth) pour obtenir un token court.
2. Client ouvre `ws://.../ws?token=<token>` (ou envoie `{"type":"auth","token":"..."}` après connexion).
3. `_authenticatedClients` (std::set<uint32_t>) garde les client IDs validés.
4. Les messages des clients non-authentifiés sont ignorés.

Voir [`ws_manager.cpp`](../../src/ws_manager.cpp) `_onClientConnect()` et `_onData()`.

## Format des messages push

JSON avec un champ `type` :
- `type: "sensor_data"` → payload identique à `/data` (voir [docs/API.md](../API.md))
- `type: "config"` → payload identique à `/get-config`
- `type: "log"` → `{timestamp, level, message}`

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
