# Subsystem — `ws_manager`

- **Fichiers** : [`src/ws_manager.h`](../../src/ws_manager.h), [`src/ws_manager.cpp`](../../src/ws_manager.cpp)
- **Singleton** : `extern WsManager wsManager;`
- **Endpoint** : `ws://poolcontroller.local/ws`

## Rôle

Bus de diffusion temps réel vers tous les clients UI connectés. Push **toutes les 5 s** des données capteur + config. Remplace le polling HTTP. Voir [ADR-0005](../adr/0005-websocket-push-sans-polling.md).

## API publique

```cpp
void begin(AsyncWebServer* server);
void update();                         // cleanup + push capteurs 5s + warn throttlé pushes sautés
void broadcastSensorData();            // push immédiat, toutes données
void broadcastConfig();                // push immédiat, config actuelle
void broadcastLog(const LogEntry&);    // push un log — CODE MORT, voir ci-dessous
bool hasClients() const;
size_t clientCount() const;            // nb clients connectés (diagnostic, v2.8.1)
uint32_t skippedPushCount() const;     // cumul des pushes sautés : garde heap OU bad_alloc rattrapé (v2.11.1 / v2.14.0)
uint32_t oversizedFrameCount() const;  // cumul des trames > kWsFrameBufBytes — pushes sautés, JAMAIS tronqués (v2.14.0)
```

> ⚠️ **Code mort — `broadcastLog()` / `Logger::setLogCallback()`** (constat investigation churn-heap, 2026-07-28) : le hook `systemLogger.setLogCallback()` ([`src/logger.h:58`](../../src/logger.h)) n'a **aucun appelant** depuis son introduction en 2.7.0 (commit `aedd007` — ajouté sans jamais être branché). `broadcastLog()` n'est donc **jamais exécuté** : aucun message `type: "log"` n'est émis, le handler UI correspondant (`data/app.js`) ne reçoit rien, et la page Journal utilise le polling HTTP `/get-logs`. La purge (ou le branchement volontaire avec garde anti-récursion) est **reportée** à un futur cycle — en attendant, `broadcastLog()` porte quand même la garde heap v2.11.1 par cohérence.

## Garde heap des broadcasts (v2.11.1, bug-badalloc-ws-broadcast)

Coredump 2026-07-28 (v2.11.0) : `AsyncWebSocket::textAll()` alloue un buffer partagé (`makeSharedBuffer`, `make_shared<vector<uint8_t>>`) **sans rattraper** `std::bad_alloc` → sur tas fragmenté (plus gros bloc < ~1,8 Ko), `abort()` et reboot brutal. Correctif — extension de la philosophie [ADR-0032](../adr/0032-reponses-http-bornees.md#extension-2026-07-28--émissions-websocket-v2111) à la voie WS :

- `_heapAllowsPush()` est appelé en tête des **3** broadcasts (`broadcastSensorData`, `broadcastConfig`, `broadcastLog`), **avant** toute construction de JSON : si `ESP.getMaxAllocHeap() < kWsPushMinAllocBytes` (**16384** o depuis v2.14.0 — était 8192, voir la [section v2.14.0](#angle-mort-toctou-comblé--filet-bad_alloc--buffer-statique-v2140-bug-heap-toctou) ci-dessous), le push est **sauté** — l'UI rate un cycle de 5 s, sans conséquence, et le débounce de l'auto-guérison ([ADR-0029 §Amendement](../adr/0029-auto-guerison-heap.md#amendement-2026-07-28--échantillonnage-adaptatif-v2111)) se joue.
- Chaque saut incrémente `_skippedPushes` (**atomic relaxed**, pattern `connectedAtomic`), exposé par `skippedPushCount()` (diagnostic).
- **INVARIANT anti-ré-entrance : `_heapAllowsPush()` ne logge JAMAIS.** Chaîne de récursion sinon : `logger` → callback (futur branchement) → `broadcastLog` → `_heapAllowsPush` → log → récursion. Le WARN « N pushes WS sautés (heap fragmenté, maxBloc=…) » est émis **exclusivement depuis `update()`** (contexte `loopTask`), throttlé à `kWsSkipWarnThrottleMs` (**30 s**), et placé **avant** le court-circuit « aucun client » pour tracer aussi les sauts survenus juste avant une déconnexion.

## Angle mort TOCTOU comblé + filet bad_alloc + buffer statique (v2.14.0, bug-heap-toctou)

2ᵉ coredump `bad_alloc` dans `textAll` (30/07, v2.13.0) **malgré la garde v2.11.1** : la garde a été passée puis prise de vitesse. Trois durcissements complémentaires :

### Garde relevée : 8192 → 16384 (fin de la zone grise)

L'ancien seuil `kWsPushMinAllocBytes = 8192` était **égal** au seuil de restart auto-guérison `kRestartMaxAllocHeapBytes` (8192) → **zone grise 8-16 Ko** de maxBloc où les pushes restaient autorisés **et** l'auto-guérison ne s'armait pas. Dans cette zone, le TOCTOU (voir ci-dessous) suffisait à provoquer le crash. Le seuil passe à **16384** (marge 2× au-dessus du seuil de restart) : les pushes s'arrêtent **avant** la zone rouge, et l'auto-guérison garde sa fenêtre d'action. Voir [`constants.h`](../../src/constants.h) et [ADR-0032 §Extension 2026-07-30](../adr/0032-reponses-http-bornees.md#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140).

### Filet absolu : `try/catch (std::bad_alloc)` autour des 3 `textAll`

La garde reste un **check-then-act** (TOCTOU) : entre `getMaxAllocHeap()` et l'allocation du buffer de trame par le fork, la tâche `async_tcp` peut consommer le bloc mesuré (réponse HTTP, page `/get-logs`…). Le filet : chaque `textAll` des 3 broadcasts est enveloppé dans un `try/catch (std::bad_alloc)`. C'est **sûr** car l'exception part de `makeSharedBuffer` (`make_shared<vector<uint8_t>>`) **avant toute mutation de file** dans le fork → catch propre = push sauté, compté dans `_skippedPushes` (même compteur que la garde, même WARN throttlé d'`update()`). **Toute course résiduelle = un push raté, plus jamais un `abort()`.**

### Buffer statique de trame `.bss` — `sWsFrameBuf` (pattern « réserver une fois, réutiliser toujours »)

Les trames WS sortantes sont construites dans un buffer **statique** `sWsFrameBuf[kWsFrameBufBytes]` (**3072** o, réservé au link en `.bss`, **hors heap**, zéro fragmentation) réutilisé à chaque push — **plus aucune String temporaire par push** (supprime le churn G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s). `_buildSensorJson(buf, cap)` / `_buildConfigJson(buf, cap)` sérialisent directement dedans (`measureJson` d'abord : si le JSON + NUL ne tient pas → retour **0**, push **sauté**, jamais de troncature) ; `broadcastLog` utilise le même buffer avec le même contrat. Chaque dépassement incrémente `_oversizedFrames` (atomic relaxed, exposé par `oversizedFrameCount()`), avec un WARN throttlé **dédié** dans `update()` (« N trame(s) WS > 3072 o (buffer statique) — pushes sautés, agrandir kWsFrameBufBytes ») — même invariant anti-ré-entrance que le warn des pushes sautés. `kWsSensorJsonReserveBytes` est **supprimée** (plus de `reserve()` de String).

> ⚠️ **INVARIANT BLOQUANT — `sWsFrameBuf` est réservé à `loopTask`.** Les 3 broadcasts sont tous appelés depuis `update()`/loop aujourd'hui. Tout futur câblage **multi-tâche** de `broadcastLog` (ex. callback logger depuis `async_tcp`/`mqttTask`) devra utiliser un buffer **dédié** — jamais celui-ci, sous peine de trames entrelacées. Voir le commentaire sur place ([`src/ws_manager.cpp`](../../src/ws_manager.cpp)).

## Récolte des clients zombies (v2.11.1, investigation churn-heap §G1)

Générateur n°1 identifié de la fragmentation : un client WS **zombie** (téléphone verrouillé, fenêtre TCP gelée) conserve connexion ouverte une file d'envoi de jusqu'à **32 buffers ~1,8 Ko** (défaut fork) = **~58 Ko épinglés** en blocs dispersés — queue pleine en ~160 s au rythme d'un push / 5 s. Deux mécanismes complémentaires, appliqués à la connexion (`_onClientConnect`) :

- **File bornée + fermeture à saturation** : `-DWS_MAX_QUEUED_MESSAGES=5` ([`platformio.ini`](../../platformio.ini)) + `client->setCloseClientOnQueueFull(true)` → le **fork ferme lui-même** le client dont la file atteint 5 trames (~25 s de snapshots non consommés = client gelé), **sous SON verrou**. La boucle manuelle initiale (itérer `getClients()` depuis `update()` et fermer sur `queueLen()`) a été **rejetée en revue** : `_handleDisconnect` (tâche `async_tcp`) érase la liste sous le verrou **privé** du fork, inaccessible côté appelant → itération concurrente = UB. L'état capteurs étant idempotent (chaque push est un snapshot complet), aucune perte fonctionnelle.
- **Keepalive** : `client->keepAlivePeriod(kWsKeepAlivePeriodS)` (**30 s**, unité : secondes — ×1000 en interne par le fork) → PING automatique, les zombies meurent par timeout ACK. **Nuance** (revue 2.11.1) : le fork n'envoie le PING que si la file du client est **vide** → le keepalive ne récolte que les zombies **inactifs** (aucun push en attente) ; les zombies sous flux de pushes sont couverts par la saturation de file ci-dessus.

## ~~Pré-dimensionnement du JSON `sensor_data`~~ (v2.11.1, churn-heap Q3 — **obsolète depuis v2.14.0**)

Le `out.reserve(kWsSensorJsonReserveBytes = 2048)` de la String de `_buildSensorJson()` est **supprimé avec la String elle-même** : depuis v2.14.0 la trame est sérialisée directement dans le buffer statique `sWsFrameBuf` (voir la [section v2.14.0](#angle-mort-toctou-comblé--filet-bad_alloc--buffer-statique-v2140-bug-heap-toctou)).

## Timing

- Intervalle de push périodique : **5000 ms** (`kSensorPushIntervalMs` [`ws_manager.h:25`](../../src/ws_manager.h:25)).
- Push **immédiat** sur événement :
  - Changement d'état filtration (démarrage / arrêt)
  - Démarrage / arrêt injection pH ou ORP
  - Changement de mode (régulation / filtration / lighting)
  - Sauvegarde config (`POST /save-config`)
  - Commande HA modifiant la config via MQTT (`drainCommandQueue`, v2.14.1)

> Le push « nouveau log » (callback `systemLogger.setLogCallback()` → `broadcastLog`) n'a **jamais été branché** — code mort, voir l'encadré de la section API publique.

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
- `type: "log"` → `{timestamp, level, message}` — **jamais émis en pratique** (`broadcastLog` = code mort, cf. section API publique)

> **Déclencheurs du message `config`** — le broadcast `config` (via le flag `_pendingConfigBroadcast` posé par `requestConfigBroadcast()`, consommé sur `loopTask`) a **deux** origines :
> 1. `POST /save-config` (`web_routes_config.cpp`) — sauvegarde de la config depuis l'UI web.
> 2. `MqttManager::drainCommandQueue()` (v2.14.1, bug-sync-ws-config-mqtt) — après application d'une commande HA modifiant la config (hors `Reboot`), pour que l'UI web reflète sous ≤ 5 s un changement fait depuis Home Assistant sans reload. Voir [mqtt-manager.md](mqtt-manager.md#notification-ui-temps-réel--broadcast-ws-config-bug-sync-ws-config-mqtt-v2141).

### Champs notables de `sensor_data`

Au-delà des mesures capteur (pH, ORP, température…) et des états de contrôle (filtration, dosage, éclairage), la payload `sensor_data` transporte aussi quelques champs d'observabilité :

> **Contrat mesures capteur `null`** : `ph`, `orp` et `temperature` sont publiés `null` quand la valeur firmware est NaN (sonde absente, lecture stale, bus dégradé — même contrat que `GET /data`). **Représentation UI** (v2.10.5) : toute valeur `null` s'affiche « `--` » sur tous les sites d'affichage numérique (jamais de conservation de la dernière valeur) et déclenche le segment « Capteur indisponible » du badge de carte (pH/ORP : feature-022 ; température : `#temperature-sensor-badge`). Voir [page-dashboard.md](../features/page-dashboard.md#convention-----quand-un-capteur-est-indisponible-v2105).

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
- **Client déconnecté sans close** : nettoyé par `AsyncWebSocket::cleanupClients()` appelé dans `update()` ; depuis v2.11.1, aussi récolté par saturation de file (fermeture par le fork) ou timeout de PING keepalive.
- **Push pendant write en cours** : `AsyncWebSocket` gère la file d'attente (bornée à 5 trames depuis v2.11.1), pas de blocage du loop.
- **Heap fragmenté** (plus gros bloc `< kWsPushMinAllocBytes = 16384` depuis v2.14.0) : pushes **sautés** (garde v2.11.1), compteur `skippedPushCount()` + WARN throttlé 30 s dans `update()`. Course résiduelle (TOCTOU) : `std::bad_alloc` **rattrapé** autour de `textAll` → push sauté, même compteur, jamais d'`abort()` (v2.14.0).
- **Trame plus grande que le buffer statique** (`kWsFrameBufBytes = 3072`) : push **sauté** (jamais tronqué), compteur `oversizedFrameCount()` + WARN throttlé dédié dans `update()` — signale qu'il faut agrandir la constante (v2.14.0).

## Dépendances

- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) **v3.11.2** (fork ESP32Async) — classe `AsyncWebSocket`.
- [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) **v3.4.10** (fork ESP32Async) — couche TCP asynchrone sous-jacente.

> **Fork ESP32Async — pourquoi (v2.9.0)** : le projet consommait auparavant les libs **originales** `me-no-dev` (`AsyncTCP` / `ESPAsyncWebServer`) via des dépôts Git **non épinglés**, aujourd'hui non maintenus. Un coredump a mis en évidence un **crash récurrent** (`panic → reboot`, appareil injoignable) déclenché au **timeout/close d'un client WebSocket** :
> `AsyncWebSocketClient::_onTimeout` → `AsyncClient::close()` → `_tcp_recved(pcb, len corrompu)` → `tcp_recved` (lwIP) → **assert `new_rcv_ann_wnd <= 0xffff`** (`tcp_update_rcv_ann_wnd`, `lwip/src/core/tcp.c`) → panic.
> C'est un **bug de course connu** des libs me-no-dev. La migration vers le fork maintenu **ESP32Async**, avec **versions épinglées** (`AsyncTCP@3.4.10`, `ESPAsyncWebServer@3.11.2`), corrige le mode de défaillance **sans changement de code applicatif** (fork rétrocompatible). Plateforme inchangée (`espressif32@6.9.0`, core 2.0.x / IDF 4.4). Voir [ADR-0028](../adr/0028-migration-fork-esp32async.md). C'est la 2ᵉ cause de crash de la pile web traitée par coredump ; la 1ʳᵉ (`std::bad_alloc` sur `/logs`) l'a été en 2.8.2.

## Fichiers liés

- [`src/ws_manager.h`](../../src/ws_manager.h), [`src/ws_manager.cpp`](../../src/ws_manager.cpp)
- [`src/web_server.cpp`](../../src/web_server.cpp) — instanciation du serveur
- [`src/logger.h:58`](../../src/logger.h) — `setLogCallback()` (**code mort**, jamais branché — cf. section API publique)
- [`src/constants.h`](../../src/constants.h) — `kWsPushMinAllocBytes` (16384), `kWsFrameBufBytes` (3072), `kWsSkipWarnThrottleMs`, `kWsKeepAlivePeriodS` (`kWsSensorJsonReserveBytes` supprimée en v2.14.0)
- [`platformio.ini`](../../platformio.ini) — `-DWS_MAX_QUEUED_MESSAGES=5`
- [ADR-0005](../adr/0005-websocket-push-sans-polling.md), [ADR-0029 §Amendement](../adr/0029-auto-guerison-heap.md#amendement-2026-07-28--échantillonnage-adaptatif-v2111), [ADR-0032 §Extensions](../adr/0032-reponses-http-bornees.md#extension-2026-07-28--émissions-websocket-v2111) (v2.11.1 et v2.14.0)

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
| `ph_mix_remaining_s` | uint | Secondes restantes de **pause mélange** hydraulique post-injection pour la pompe **pH** (v2.19.1, `getMixingRemainingS(0)`) |
| `orp_mix_remaining_s` | uint | Secondes restantes de **pause mélange** hydraulique post-injection pour la pompe **ORP** (v2.19.1, `getMixingRemainingS(1)`) |

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

## Champs `sensor_data` ajoutés en feature-056 (Mode d'installation, v2.19.0)

| Champ | Type | Description |
|-------|------|-------------|
| `install_mode` | string | Mode d'installation actif : `"managed"` / `"powered"` / `"external"` (`installModeToString(mqttCfg.installMode)`). |
| `water_present` | bool | Présence d'eau résolue par `filtration.resolveWaterPresence().waterPresent` — **source unique** consommée par toutes les gardes de dosage. `managed` → filtration commandée ON ; `powered` → toujours `true` ; `external` → dernier signal ON reçu il y a < `kExternalFiltrationStaleMs` (180 s). |
| `filtration_state_source` | string | Origine de `water_present` : `"commanded"` (managed), `"powered"` (powered), `"external"` (external) — mappé depuis `WaterSource`. |
| `filtration_state_stale` | bool | `WaterPresence.stale` : mode `external`, `true` si un signal a déjà été reçu mais est périmé (> 180 s) → dosage suspendu (fail-safe). |
| `filtration_ext_known` | bool | Mode `external` : `true` dès qu'au moins un signal externe a été reçu depuis le boot. `false` = « aucun signal » (fail-safe OFF au boot). |
| `filtration_ext_on` | bool | Dernier état signalé par la filtration externe (`ON`/`OFF`), lu via `filtration.getExternalState()`. |
| `filtration_ext_age_s` | int | Âge en secondes du dernier signal externe (`0` si `filtration_ext_known == false`). |

Ces champs sont **calculés au vol** dans `_buildSensorJson()` à chaque cycle. La présence d'eau vient d'un appel unique à `filtration.resolveWaterPresence()` (qui délègue à la fonction pure `resolveWaterPresent()`, [ADR-0026](../adr/0026-mode-installation.md)) ; le triplet `{on, lastMs, known}` du signal externe est lu sous portMUX via `getExternalState()`. Le champ `boost_filtration_extended` (feature-055) est désormais dérivé de `installMode == ManagedFiltration` (et non plus de `filtrationCfg.enabled`, retiré). Les champs `regulation_mode` / `filtration_enabled` ont disparu de la payload `config`.

Buffer `_buildSensorJson()` bumpe de **1664 → 1920 octets**.

## Champs `sensor_data` ajoutés en feature-061 (validité des mesures pH/ORP, v2.13.0)

| Champ | Type | Description |
|-------|------|-------------|
| `measures_valid` | bool | `true` = les mesures pH/ORP sont **représentatives de l'eau du bassin**. `false` = eau absente (filtration arrêtée / signal externe périmé) **ou** délai de purge post-reprise en cours (`stabilizationDelayMin`, défaut 5 min). |
| `measures_stab_remaining_s` | uint | Secondes restantes du délai de purge (arrondi seconde sup). `0` si valide **ou** eau absente. ⚠️ Nom volontairement **distinct** de `stabilization_remaining_s` (stabilisation post-**calibration des pompes**, feature-006) : ici il s'agit de la **purge de la chambre de mesure**. |

Les deux champs sont lus depuis le **cache** du singleton `measureValidity` (`getSnapshot()`, snapshot cohérent sous spinlock — jamais d'appel direct à `resolveWaterPresence()` hors `loopTask`). Voir [measure-validity.md](measure-validity.md). Le champ `water_present` (feature-056, ci-dessus) complète le triptyque : l'UI combine `water_present` + `filtration_state_stale` + `measures_valid` + `measures_stab_remaining_s` pour les badges « Filtration arrêtée » / « Signal filtration périmé » / « Stabilisation : ~N min » ([page-dashboard.md](../features/page-dashboard.md#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)).

**Les champs live `ph` / `orp` / `phRaw` / `orpRaw` / … restent publiés tels quels** quand les mesures sont invalides (page calibration et onglet Avancé intacts) — c'est l'UI qui arbitre l'affichage via `measures_valid`. Le compte à rebours est **recalculé côté firmware** et rafraîchi à chaque push (5 s) : l'UI n'a aucun tick JS.

Buffer `_buildSensorJson()` bumpe de **2048 → 2112 octets** (l'étape intermédiaire 1920 → 2048 venait des champs `ph/orp_mix_remaining_s`, v2.19.1).

## Champs `sensor_data` ajoutés en v2.20.3 (alarme filtre bloqué)

| Champ | Type | Description |
|-------|------|-------------|
| `phFilterAlarm` | bool | `true` quand le filtre pH est resté **non prêt > 5 min** (`kSensorFilterNotReadyAlarmMs`) **alors que** la filtration tournait **et** que les mesures étaient déclarées représentatives → plus **aucune** régulation pH, ni chlore (fail-closed feature-067 sur pH brut indisponible). |
| `orpFilterAlarm` | bool | Symétrique ORP. |

Source : `sensors.isPhFilterNotReadyAlarm()` / `isOrpFilterNotReadyAlarm()` — simple reflet d'un drapeau, toute l'hystérésis est côté `SensorManager`. **Mêmes champs et mêmes valeurs dans `GET /data`** (aucune asymétrie WS/HTTP ici, contrairement à `filtration_state_stale`).

⚠️ **Distinction à ne pas perdre** : `phFilterReady = false` est **banal** (warmup, purge, plusieurs fois par jour) ; `phFilterAlarm = true` est une **anomalie durable**. La purge de la chambre de mesure, le warmup et la filtration à l'arrêt ne l'arment jamais. C'est ce champ que consomment le chip d'état filtre (label « Régulation à l'arrêt », prioritaire sur tous les autres états, y compris « EZO indisponible ») et la bannière du tableau de bord.

**Budget de payload** : `_buildSensorJson()` passe de **2112 → 2176 octets**. Décompte exact des octets sérialisés ajoutés (pire cas, la valeur `false` étant la plus longue) : `"phFilterAlarm":false,` = 22 o, `"orpFilterAlarm":false,` = 23 o, soit **45 o**. `StaticJson` dimensionne le **pool ArduinoJson** (nœuds), pas la chaîne : 2 nœuds booléens ≈ 32 o, clés non copiées (littéraux). Marge de 64 o retenue pour couvrir pool **et** alignement. `sWsFrameBuf` reste vérifié indépendamment par la garde `measureJson(doc) + 1 > sizeof(sWsFrameBuf)`. Côté `/data`, buffer porté de **1280 → 1344** octets.

Voir [sensors.md](sensors.md#alarme--filtre-non-prêt-durablement--v2203), [page-ph.md](../features/page-ph.md#bouton--réarmer-le-filtre-ph--v2203) et [page-dashboard.md](../features/page-dashboard.md#bannière--régulation-à-larrêt--v2203).

## Champ `config` ajouté en feature-067 (offset logiciel pH, v2.20.0)

| Champ | Type | Message | Description |
|-------|------|---------|-------------|
| `ph_offset` | float | **`config`** (pas `sensor_data`) | Offset logiciel de mesure pH, tel que persisté en NVS. Plage `[−0.50, +0.50]`, défaut `0`. `pH corrigé = pH brut EZO + ph_offset`. Miroir exact de `GET /get-config` — c'est un **paramètre**, pas une mesure : il n'a rien à faire dans le push temps réel 5 s. |

Émis par `_buildConfigJson()` ([`ws_manager.cpp`](../../src/ws_manager.cpp)) depuis `mqttCfg.phOffset`. Le broadcast `config` est déclenché par `POST /save-config` : l'UI voit donc la nouvelle valeur **sans reload**, et le champ de saisie `#ph_offset` est re-synchronisé — **sauf s'il a le focus**, pour ne pas écraser une saisie en cours ([page-ph.md](../features/page-ph.md#carte--correction-de-la-mesure-ph--feature-067-v2200)).

**Aucun champ `sensor_data` n'est ajouté.** En revanche la **valeur** de trois champs existants intègre désormais l'offset — `ph`, `phMedian`, `phFiltered` sont **corrigés**, `phRaw` reste **brut EZO** (règle : seuls les champs `*Raw` sont bruts, [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Le repli de warmup de `ph` passe par la valeur **corrigée**, jamais par le brut : on ne mélange jamais brut et corrigé sur un même champ.

Marge du buffer `_buildConfigJson()` portée à **2368 octets** (+1 champ ≈ 24 octets).
