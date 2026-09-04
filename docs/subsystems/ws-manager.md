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

- `_heapAllowsPush()` est appelé en tête des **3** broadcasts (`broadcastSensorData`, `broadcastConfig`, `broadcastLog`), **avant** toute construction de JSON : si `heapLargestFreeBlock8Bit() < kWsPushMinAllocBytes` (**6 144 o en métrique 8 bits depuis v2.33.0** — ≈ 2,4× la trame pire cas 2 552 o, au-dessus du palier auto-heal 4 096 ; était 16 384 sur la mesure **brute**, dont le plancher ~27 636 o rendait la garde **incapable de sauter un push**, même en famine réelle — [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)), le push est **sauté** — l'UI rate un cycle de 5 s, sans conséquence, et le débounce de l'auto-guérison ([ADR-0029 §Amendement](../adr/0029-auto-guerison-heap.md#amendement-2026-07-28--échantillonnage-adaptatif-v2111)) se joue.
- Chaque saut incrémente `_skippedPushes` (**atomic relaxed**, pattern `connectedAtomic`), exposé par `skippedPushCount()` (diagnostic).
- **INVARIANT anti-ré-entrance : `_heapAllowsPush()` ne logge JAMAIS.** Chaîne de récursion sinon : `logger` → callback (futur branchement) → `broadcastLog` → `_heapAllowsPush` → log → récursion (`heapLargestFreeBlock8Bit()` est elle-même sans log ni allocation, contrat [`heap_metrics.h`](../../src/heap_metrics.h) — l'invariant tient). Le WARN « N pushes WS sautés (heap fragmenté, maxBloc8=…) » — la **même métrique que celle qui décide**, réserve du contre-visa bug-080 — est émis **exclusivement depuis `update()`** (contexte `loopTask`), throttlé à `kWsSkipWarnThrottleMs` (**30 s**), et placé **avant** le court-circuit « aucun client » pour tracer aussi les sauts survenus juste avant une déconnexion.

## Angle mort TOCTOU comblé + filet bad_alloc + buffer statique (v2.14.0, bug-heap-toctou)

2ᵉ coredump `bad_alloc` dans `textAll` (30/07, v2.13.0) **malgré la garde v2.11.1** : la garde a été passée puis prise de vitesse. Trois durcissements complémentaires :

### Garde relevée : 8192 → 16384 (fin de la zone grise)

L'ancien seuil `kWsPushMinAllocBytes = 8192` était **égal** au seuil de restart auto-guérison `kRestartMaxAllocHeapBytes` (8192) → **zone grise 8-16 Ko** de maxBloc où les pushes restaient autorisés **et** l'auto-guérison ne s'armait pas. Dans cette zone, le TOCTOU (voir ci-dessous) suffisait à provoquer le crash. Le seuil passe à **16384** (marge 2× au-dessus du seuil de restart) : les pushes s'arrêtent **avant** la zone rouge, et l'auto-guérison garde sa fenêtre d'action. Voir [`constants.h`](../../src/constants.h) et [ADR-0032 §Extension 2026-07-30](../adr/0032-reponses-http-bornees.md#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140).

> **v2.33.0 — garde basculée en métrique 8 bits, seuil 16 384 → 6 144** ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md), bug-080). La mesure brute `ESP.getMaxAllocHeap()` compte l'IRAM 32 bits (plancher ~27 636 o) : cette garde n'a en réalité **jamais sauté un push**, même pendant la famine du 02/09. Elle lit désormais `heapLargestFreeBlock8Bit()` ; le principe « pushes coupés AVANT la zone rouge » est conservé dans la nouvelle échelle : 6 144 > 4 096 (auto-heal), et ≈ 2,4× la trame pire cas mesurée (2 552 o).

### Filet absolu : `try/catch (std::bad_alloc)` autour des 3 `textAll`

La garde reste un **check-then-act** (TOCTOU) : entre `getMaxAllocHeap()` et l'allocation du buffer de trame par le fork, la tâche `async_tcp` peut consommer le bloc mesuré (réponse HTTP, page `/get-logs`…). Le filet : chaque `textAll` des 3 broadcasts est enveloppé dans un `try/catch (std::bad_alloc)`. C'est **sûr** car l'exception part de `makeSharedBuffer` (`make_shared<vector<uint8_t>>`) **avant toute mutation de file** dans le fork → catch propre = push sauté, compté dans `_skippedPushes` (même compteur que la garde, même WARN throttlé d'`update()`). **Toute course résiduelle = un push raté, plus jamais un `abort()`.**

### Buffer statique de trame `.bss` — `sWsFrameBuf` (pattern « réserver une fois, réutiliser toujours »)

Les trames WS sortantes sont construites dans un buffer **statique** `sWsFrameBuf[kWsFrameBufBytes]` (**3584** o depuis v2.21.1 — 3072 de v2.14.0 à v2.21.0 —, réservé au link en `.bss`, **hors heap**, zéro fragmentation) réutilisé à chaque push — **plus aucune String temporaire par push** (supprime le churn G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s). `_buildSensorJson(buf, cap)` / `_buildConfigJson(buf, cap)` sérialisent directement dedans (`measureJson` d'abord : si le JSON + NUL ne tient pas → retour **0**, push **sauté**, jamais de troncature) ; `broadcastLog` utilise le même buffer avec le même contrat. Chaque dépassement incrémente `_oversizedFrames` (atomic relaxed, exposé par `oversizedFrameCount()`), avec un WARN throttlé **dédié** dans `update()` (« N trame(s) WS > `kWsFrameBufBytes` o (buffer statique) — pushes sautés, agrandir kWsFrameBufBytes ») — même invariant anti-ré-entrance que le warn des pushes sautés. `kWsSensorJsonReserveBytes` est **supprimée** (plus de `reserve()` de String).

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
- **Heap fragmenté** (plus gros bloc **8 bits** `< kWsPushMinAllocBytes = 6144` depuis v2.33.0, [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : pushes **sautés** (garde v2.11.1), compteur `skippedPushCount()` + WARN throttlé 30 s dans `update()` (libellé `maxBloc8=`). Course résiduelle (TOCTOU) : `std::bad_alloc` **rattrapé** autour de `textAll` → push sauté, même compteur, jamais d'`abort()` (v2.14.0).
- **Trame plus grande que le buffer statique** (`kWsFrameBufBytes = 3584` depuis v2.21.1, était 3072) : push **sauté** (jamais tronqué), compteur `oversizedFrameCount()` + WARN throttlé dédié dans `update()` — signale qu'il faut agrandir la constante (v2.14.0).

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
- [`src/constants.h`](../../src/constants.h) — `kWsPushMinAllocBytes` (6144 en 8 bits depuis v2.33.0, était 16384 en brute), `kWsFrameBufBytes` (3584 depuis v2.21.1), `kWsSkipWarnThrottleMs`, `kWsKeepAlivePeriodS` (`kWsSensorJsonReserveBytes` supprimée en v2.14.0)
- [`src/heap_metrics.h`](../../src/heap_metrics.h) — `heapLargestFreeBlock8Bit()`, mesure lue par `_heapAllowsPush()` ([ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md))
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

> ⚠️ **`measures_stab_remaining_s` est SUPPRIMÉ depuis la v2.34.0** (feature-091) : la purge n'a plus de fin connue à l'avance, il n'y a donc plus de compte à rebours à publier. Il est remplacé par quatre champs — voir [feature-091 (v2.34.0)](#champs-sensor_data-ajoutés-en-feature-091--purge-adaptative-v2340) ci-dessous. La description de `measures_valid` ci-dessus reste vraie **sauf** la mention du réglage `stabilizationDelayMin`, lui aussi retiré.

Les deux champs sont lus depuis le **cache** du singleton `measureValidity` (`getSnapshot()`, snapshot cohérent sous spinlock — jamais d'appel direct à `resolveWaterPresence()` hors `loopTask`). Voir [measure-validity.md](measure-validity.md). Le champ `water_present` (feature-056, ci-dessus) complète le triptyque : l'UI combine `water_present` + `filtration_state_stale` + `measures_valid` + l'état de purge pour les badges « Filtration arrêtée » / « Signal filtration périmé » / « Stabilisation depuis N min » ([page-dashboard.md](../features/page-dashboard.md#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)).

**Les champs live `ph` / `orp` / `phRaw` / `orpRaw` / … restent publiés tels quels** quand les mesures sont invalides (page calibration et onglet Avancé intacts) — c'est l'UI qui arbitre l'affichage via `measures_valid`. L'état de purge est **recalculé côté firmware** et rafraîchi à chaque push (5 s) : l'UI n'a aucun tick JS.

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

## Parité `/data` ↔ `sensor_data` — indicateurs de garde pH (v2.25.0)

Les quatre champs ci-dessous existaient **uniquement** sur le canal WebSocket, alors que l'UI les consomme depuis les **deux** sources : `loadSensorData()` charge `/data` et le traite comme une trame `sensor_data`. Ils ont donc été ajoutés à `/data` en v2.25.0, en **parité stricte**.

| Champ | Type | Description |
|-------|------|-------------|
| `phDoseBlockedReason` | string \| null | Dernière cause de refus du dosage pH — `canDose(0)` **ou** une garde de sécurité pH. `null` si autorisé. |
| `orpDoseBlockedReason` | string \| null | Équivalent ORP. |
| `phRawGuardBlocked` | bool | `true` quand une **butée de sécurité sur le pH BRUT** mord pour le produit pH déclaré (`6,20` en `ph_minus`, `8,10` en `ph_plus`). |
| `orpPhRawGuardBlocked` | bool | `true` quand la butée haute sur le pH BRUT (`8,10`) inhibe le dosage chlore. |

Source : `PumpController.copyPhDoseBlockedReason()` / `copyOrpDoseBlockedReason()` (copie dans un tampon fourni — les ex-getters rendant une `String` allouaient sur le tas dans la tâche `async_tcp` à chaque appel) et `isPhRawGuardBlocked()` / `isOrpPhRawGuardBlocked()`.

**Budget de payload** : les deux booléens ajoutés à la trame `sensor_data` coûtent 55 o au pire cas (`"phRawGuardBlocked":false,` = 26 o, `"orpPhRawGuardBlocked":false,` = 29 o).

### Code de cause de refus — parité d'emblée (v2.25.1)

Deux champs s'ajoutent, cette fois **sur les deux canaux dans le même passage** (`_buildSensorJson()` pour le WS, `web_routes_data.cpp` pour `/data`) — la parité n'est plus rattrapée après coup :

| Champ | Type | Description |
|-------|------|-------------|
| `phDoseBlockedCode` | integer \| null | **Code machine** de la cause de refus du dosage pH. `null` si aucun refus (`kDoseBlockedCodeNone` = 0 n'est jamais publié en tant que `0`). Valeurs = `enum class DoseRefusal`, **contrat de sérialisation figé** ; sentinelle `250` = cause de garde de sécurité. Table complète : [API.md](../API.md#code-de-cause-de-refus-de-dosage-v2251). |
| `orpDoseBlockedCode` | integer \| null | Équivalent ORP. |

Source : `PumpController.getPhDoseBlockedCode()` / `getOrpDoseBlockedCode()` (lecture d'un `uint8_t` membre, aucune allocation). **Même règle de nullité que le texte** : `code == kDoseBlockedCodeNone` ⇒ `null` explicite. Le code est écrit **avant** le texte côté firmware (`logRefusalOnce()`, `setGuardRefusalCause()`), de sorte qu'un lecteur asynchrone ne puisse jamais associer un texte de garde à un code de `canDose()` : au pire il voit le nouveau code avec l'ancien texte, et le **code fait foi**.

**Budget de payload** : **51 o** au pire cas (`"phDoseBlockedCode":null,` = 25 o, `"orpDoseBlockedCode":null,` = 26 o).

> Pourquoi un entier et non une chaîne d'identifiant stable (`"filter_frozen"`) : 1 à 3 octets au lieu de 10 à 20 sur un budget de trame contraint, et surtout un entier ne se compare **que par égalité** — une chaîne invite au `startsWith` / `includes`, c'est-à-dire au procédé même que ce champ retire. Voir [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md).

> ⚠️ **Exigence désormais explicite ([ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md))** : tout champ consommé par l'interface doit exister sur **tous** les canaux qui le poussent, avec le même nom et la même valeur ; et le client doit rester **inerte** quand il manque — clé absente ≠ `false`. Côté client, `rawGuardBites()` distingue explicitement trois états (`true` / `false` / **information absente**) et n'affiche aucun avertissement dans le troisième cas : un encart affiché « par précaution » quand l'info manque serait ignoré et détruirait la valeur de signal de l'encart réel.
>
> C'est la **troisième récidive** de ce motif : pentes de sonde en 2.21.2, calibration de pompe en 2.23.0 (voir plus bas), indicateurs de garde ici.

> ⚠️ Ces champs **gèlent** sur leur dernière valeur pendant les sorties anticipées de la boucle de dosage (filtration arrêtée, OTA, mode maintenance, capteurs non initialisés, stabilisation post-calibration, assistant de mesure du débit) : la surveillance n'y tourne pas, car en eau stagnante la mesure n'est pas représentative du bassin. Voir [pump-controller.md](pump-controller.md#surveillance-inconditionnelle-séparée-de-la-décision).

## Champ `config` ajouté en feature-067 (offset logiciel pH, v2.20.0)

| Champ | Type | Message | Description |
|-------|------|---------|-------------|
| `ph_offset` | float | **`config`** (pas `sensor_data`) | Offset logiciel de mesure pH, tel que persisté en NVS. Plage `[−0.50, +0.50]`, défaut `0`. `pH corrigé = pH brut EZO + ph_offset`. Miroir exact de `GET /get-config` — c'est un **paramètre**, pas une mesure : il n'a rien à faire dans le push temps réel 5 s. |

Émis par `_buildConfigJson()` ([`ws_manager.cpp`](../../src/ws_manager.cpp)) depuis `mqttCfg.phOffset`. Le broadcast `config` est déclenché par `POST /save-config` : l'UI voit donc la nouvelle valeur **sans reload**, et le champ de saisie `#ph_offset` est re-synchronisé — **sauf s'il a le focus**, pour ne pas écraser une saisie en cours ([page-ph.md](../features/page-ph.md#carte--correction-de-la-mesure-ph--feature-067-v2200)).

**Aucun champ `sensor_data` n'est ajouté.** En revanche la **valeur** de trois champs existants intègre désormais l'offset — `ph`, `phMedian`, `phFiltered` sont **corrigés**, `phRaw` reste **brut EZO** (règle : seuls les champs `*Raw` sont bruts, [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Le repli de warmup de `ph` passe par la valeur **corrigée**, jamais par le brut : on ne mélange jamais brut et corrigé sur un même champ.

Marge du buffer `_buildConfigJson()` portée à **2368 octets** (+1 champ ≈ 24 octets).

> ⚠️ **Asymétrie assumée (v2.25.0)** : les six bornes `ph_target_min/_max/_step` et `ph_offset_min/_max/_step` sont servies par `GET /get-config` mais **PAS** par la trame `config` du WebSocket — ce sont des constantes de compilation, invariantes pour un firmware donné, et le budget de trame est mieux dépensé ailleurs. C'est une **exception documentée** à la règle de parité ci-dessus, tolérable uniquement parce que le client la connaît : `_applyPhBounds()` ignore **en bloc** un lot absent ou incohérent (borne inversée, pas nul) et **conserve les bornes déjà connues**. Sans cette précaution, chaque push `config` ferait retomber l'UI sur ses valeurs de repli entre deux appels à `/get-config`.

## Champs `config` ajoutés en v2.23.0 — caractérisation des pompes (feature-074, correctif on-target)

**Bug on-target corrigé** : la trame `config` WS ne portait **pas** les champs de calibration des pompes — au premier push (≤ 5 s après `/save-config`), `updatePumpCalibrationDisplay()` traitait les clés absentes comme « zéro » et **écrasait** le pré-remplissage / la ligne lecture seule avec « débit non caractérisé » alors que la calibration existait. Correctif double :

1. **Parité STRICTE `/get-config` ↔ trame `config`** — 6 champs ajoutés à `_buildConfigJson()` ([`ws_manager.cpp`](../../src/ws_manager.cpp)), mêmes noms que `/get-config` :

| Champ | Type | Description |
|-------|------|-------------|
| `pump1_calibrated_duty` / `pump2_calibrated_duty` | integer | Duty calibré (0 = fail-closed) — voir [API.md](../API.md#get-config--read) |
| `pump1_pulse_flow_ml_per_min` / `pump2_pulse_flow_ml_per_min` | float | Débit au duty calibré (défaut de type 28,0, [ADR-0047](../adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)) |
| `pump1_flow_weighed` / `pump2_flow_weighed` | boolean | Provenance : `false` = valeur usine, `true` = pesée individuelle (informatif) |

Marge du buffer `_buildConfigJson()` portée à **2560 octets** (+6 champs ≈ 194 o au pire) ; trame `config` réelle ≈ **2,56 Ko** pour `kWsFrameBufBytes = 3584` — marge OK.

2. **Côté client, clé absente = pas d'information** — `updatePumpCalibrationDisplay()` fait un early-return si `pumpN_pulse_flow_ml_per_min` est `undefined` (payload d'un firmware plus ancien) : on ne touche à rien, **jamais** d'interprétation « absent = 0 ». Même leçon que la parité `/data` des pentes de sonde en 2.21.2 : tout champ affiché par l'UI doit exister sur **tous** les canaux qui poussent la config, et le client doit rester inerte quand il manque.

## Champs `sensor_data` ajoutés en v2.21.1 — calendrier d'impulsion (ADR-0045)

Itération 3 de bug-dosage-fantome (défaut n°3) : distinguer « **session de dosage ouverte** » (commandé — c'est ce que disent déjà `ph/orp_dosing`) de « **pompe alimentée maintenant** » (effectif — pouls en cours du [dosage en proportionnel de temps](pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210)). Le firmware expose la **géométrie du train d'impulsions**, le client anime localement entre deux trames ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md) — rejet du booléen instantané : aliasing à 5 s de push pour des impulsions de 5-60 s).

| Champ | Type | Unité | Description |
|-------|------|-------|-------------|
| `pulse_period_ms` | uint32 | ms | Période **fixe** du train d'impulsions (= `kPulsePeriodMs` = 60000). Constante de contrat, publiée pour que le client ne la code pas en dur. |
| `ph_pulse_session` | bool | — | Séance de dosage **ouverte** pour le pH (auto **OU** scheduled) — `PumpController.isPulseSessionActive(0)`. C'est l'état « commandé », pas la pompe. |
| `orp_pulse_session` | bool | — | Idem ORP (`isPulseSessionActive(1)`). |
| `ph_pulse_ms` | uint32 | ms | Durée d'impulsion **planifiée** pour la période courante (`getPulseMs(0)`). **`0` = deadband** : la période ne contient aucun pouls (demande sous le pouls minimal `kPulseMinMs` — **10 s depuis la v2.25.3**, 5 s avant) — la session reste ouverte, la pompe ne tournera pas cette période. La demande n'est pas perdue : elle est **accumulée** ([ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md)). |
| `orp_pulse_ms` | uint32 | ms | Idem ORP. |
| `ph_pulse_phase_ms` | uint32 | ms | Position **RELATIVE** dans la période courante (`getPulsePhaseMs(0, nowMs)`), clampée **[0..`pulse_period_ms`]** — jamais un `millis()` absolu : le client l'ancre sur SA propre horloge à la réception (`dosingAnchor`, app.js). `0` si session inactive. **Pompe alimentée ⇔ `phase < pulse_ms`.** |
| `orp_pulse_phase_ms` | uint32 | ms | Idem ORP. |

- **Mêmes 7 champs, mêmes noms, mêmes valeurs dans `GET /data`** ([API.md](../API.md#get-data--write)) — contrat architect, aucune asymétrie WS/HTTP.
- Getters `PumpController` **lecture seule** (aucun setter, aucun effet dosage) ; course bénigne assumée écriture loopTask / lecture async_tcp (lectures 8/32 bits atomiques sur Xtensa) ; index invalide → `false`/`0` (fail-closed). Voir [pump-controller.md](pump-controller.md#exposition-du-calendrier-dimpulsion-itération-3-v2211).
- **Rétrocompat client** : champs absents (firmware < 2.21.1) → l'UI retombe sur le comportement historique (`ph/orp_dosing` seuls).

**Budget de payload** : décompte au pire cas des octets sérialisés = **169 o** (7 champs, valeurs max `60000`/`false`) + pool ArduinoJson 7 nœuds ≈ 112 o → `_buildSensorJson()` bumpe de **2176 → 2432 octets**. **`kWsFrameBufBytes` passe de 3072 → 3584** (trame réelle mesurée ~2,2 Ko / 3584 — marge restaurée après plusieurs ajouts de champs successifs).

Consommateur UI : statut dosage des cartes pH/ORP du tableau de bord — 6 priorités d'affichage, compte à rebours local resynchronisé à chaque trame ([page-dashboard.md](../features/page-dashboard.md#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211)).

## Champs `sensor_data` ajoutés en v2.26.0 — terme intégral pH (ADR-0052)

La [levée de `Ki = 0`](pump-controller.md#terme-intégral-ph--levée-de-ki--0-v2260) sur la régulation pH rend le terme intégral **porteur de toute la charge à l'équilibre** (là où le proportionnel s'annule). Sans ces champs, une intégrale saturée resterait **invisible** — la classe de défaut historique du projet.

| Champ | Type | Unité | Description |
|-------|------|-------|-------------|
| `ph_integral` | float \| null | pH·s | Intégrale accumulée (`getPhIntegral()`), **signée**, arrondie à 1 décimale. Négative = accumulation de l'autre côté de la consigne, ce n'est pas une anomalie. `NAN` firmware → `null` explicite. |
| `ph_integral_term` | float \| null | mL/min | Contribution au débit moyen demandé (`getPhIntegralTerm()` = `ki × intégrale`), 3 décimales. Bornée par l'**autorité** de 6,0 mL/min quelle que soit la vitesse. |
| `ph_integral_pct` | float \| null | % | Remplissage de l'anti-windup (`getPhIntegralPct()`), **valeur absolue**, 1 décimale. `null` si `integralMax ≤ 0`. |

- **Parité stricte d'emblée avec `/data`** (`_buildSensorJson()` et `web_routes_data.cpp` **dans le même passage**) : mêmes noms, mêmes arrondis, `NaN` → `null` explicite. C'est la **quatrième application** du contrat de parité après les pentes de sonde (v2.21.2), les champs de calibration (v2.23.0) et l'état de blocage de dosage (v2.25.0). Le même trio est également publié sur MQTT ([MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-v2260-terme-intégral-ph)).
- **Aucun équivalent ORP** : `Ki_ORP` reste 0 — asymétrie voulue, hors périmètre de l'ADR-0052.
- Getters `PumpController` **lecture seule**, aucun setter, aucune influence sur une décision de dosage ; course bénigne assumée écriture `loopTask` / lecture `async_tcp` (lectures 32 bits atomiques sur Xtensa).
- **Rétrocompat client** : clé **absente** (firmware < 2.26.0) ≠ `null` ≠ `0`. L'UI masque le bloc entier quand la clé est absente — afficher « 0 % » mentirait sur une régulation dont on ne sait rien. Voir [page-ph.md](../features/page-ph.md#rattrapage-accumulé-par-la-régulation-ph-v2260).

**Budget de payload** : **73 o** au pire cas (`"ph_integral":-43200.0,` = 23 o, `"ph_integral_term":-6.000,` = 26 o, `"ph_integral_pct":100.0,` = 24 o). Pire cas de trame **2 479 → 2 552 o** ; le gabarit documentaire `StaticJson<>` de `_buildSensorJson()` passe de **2560 à 2688** pour rester franchement au-dessus, la marge sous `kWsFrameBufBytes` (3584 o) restant de **1 032 o**. Aucune troncature possible : la garde `measureJson(doc) + 1 > cap` **saute** le push au lieu de tronquer.

## Champs `sensor_data` ajoutés en v2.28.0 — feedforward pH (ADR-0055)

La [dose de base apprise](pump-controller.md#dose-de-base-apprise--feedforward-ph-v2280) ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)) est la seule source de dosage **aveugle à l'erreur mesurée** : sans observabilité, personne ne pourrait constater qu'elle dose (ou pourquoi elle ne dose pas).

| Champ | Type | Unité | Description |
|-------|------|-------|-------------|
| `ph_ff_ml_per_min` | float \| null | mL/min | Débit de base appliqué à la session automatique en cours (`getPhFeedforwardFlow()`), **2 décimales**, `0` hors session. `NaN` firmware → `null` explicite. |
| `ph_ff_status` | int | — | Code `PhFfStatus` (`getPhFeedforwardStatus()`), **contrat figé 0-4** : `0` inactif · `1` actif · `2` latch de sécurité · `3` historique < 3 j · `4` quota ≥ 60 %. Les états 2-4 sont servis même hors session. |

- **Parité stricte d'emblée avec `/data`** (mêmes noms, mêmes arrondis, `NaN` → `null`, même passage de code) — la parité `/data` ↔ WS est un motif à **5 récidives** dans ce projet, le critère de la spec l'exigeait explicitement. Le même duo est publié sur MQTT ([MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-v2280-dose-de-base-apprise--feedforward-ph)).
- **Rétrocompat client** : clé `ph_ff_status` **absente** = firmware < 2.28.0 → l'UI masque la ligne entière (afficher « inactif » mentirait sur un firmware sans feedforward). Voir [page-ph.md](../features/page-ph.md#ligne--dose-de-base-apprise--feature-085-v2280).
- Getters `PumpController` **lecture seule**, aucun setter, aucune influence sur une décision de dosage ; course bénigne assumée écriture `loopTask` / lecture `async_tcp`.

**Budget de payload** : **44 o** au pire cas (`"ph_ff_ml_per_min":123.45,` = 26 o, `"ph_ff_status":4,` = 18 o). Pire cas de trame **2 582 → 2 626 o**, sous le gabarit documentaire **2688** (marge 62 o) et sous `kWsFrameBufBytes` (3584 o, marge **958 o**) — **aucun bump**. Aucune troncature possible : la garde `measureJson(doc) + 1 > cap` **saute** le push au lieu de tronquer.

## Champs `sensor_data` ajoutés en v2.29.0 — limite d'actionneur pH (feature-086)

L'[exposition de la limite d'actionneur pH](pump-controller.md#limite-dactionneur-ph--exposition-et-alerte-de-sous-dimensionnement-feature-086-v2290) ([ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) rend visible la **conséquence** de la limite horaire d'injection : un plafond de débit moyen, donc un maximum journalier — l'élément qui explique un pH qui ne descend pas.

| Champ | Type | Unité | Description |
|-------|------|-------|-------------|
| `ph_sustainable_ml_per_min` | float \| null | mL/min | Débit moyen soutenable (`getPhSustainableMlPerMin()`), **2 décimales**. `NAN` firmware (pompe non caractérisée) → `null` explicite. |
| `ph_daily_max_ml` | int \| null | mL/j | Maximum journalier délivrable (`getPhDailyMaxMl()`, arrondi à l'entier) = soutenable × minutes **programmées** de filtration. `NAN` (soutenable indisponible ou planning inexploitable : mode `off`/`force`, plage invalide) → `null`. |
| `ph_limited_by_hourly_budget` | bool | — | Verdict « borné par la limite horaire » (`isPhLimitedByHourlyBudget()`) : épisode de saturation C5 ouvert **ET** mesure pH valide. |
| `ph_undersized` | bool | — | Sous-dimensionnement suspecté (`isPhUndersizedSuspected()`) : série ≥ 3 journées consécutives saturées **ET** anomalie C5 ouverte **ET** mesure pH valide. |

- **Parité stricte d'emblée avec `/data`** (mêmes noms, mêmes arrondis, `NaN` → `null`, même passage de code) — motif à **5 récidives** dans ce projet, exigé explicitement par la spec. **Pas de publication MQTT** (hors périmètre du cycle, budget flash).
- **Rétrocompat client** : `ph_limited_by_hourly_budget` **absent** (non booléen) = firmware < 2.29.0 → l'UI masque capacités **et** bandeaux. Voir [page-ph.md](../features/page-ph.md#capacités-de-dosage-et-bandeaux-de-limite-dactionneur-feature-086-v2290).
- Accesseurs `PumpController` **lecture seule**, aucun setter, aucune influence sur une décision de dosage ; course bénigne assumée écriture `loopTask` / lecture `async_tcp` (lectures 8/32 bits atomiques sur Xtensa).

**Budget de payload** : **119 o** au pire cas (`"ph_sustainable_ml_per_min":123.45,` = 35 o, `"ph_daily_max_ml":1439986,` = 26 o, `"ph_limited_by_hourly_budget":false,` = 36 o, `"ph_undersized":false,` = 22 o). Pire cas de trame **2 626 → 2 745 o** ; le gabarit documentaire `StaticJson<>` de `_buildSensorJson()` passe de **2688 à 2816** pour rester franchement au-dessus, la marge sous `kWsFrameBufBytes` (3584 o) restant de **839 o**. Aucune troncature possible : la garde `measureJson(doc) + 1 > cap` **saute** le push au lieu de tronquer.

## feature-088 (v2.31.0) — restauration des défauts de régulation : AUCUN champ nouveau

**Rien n'est ajouté à la trame `config` ni à `sensor_data`** — vérifié par grep de `_buildConfigJson()` : ni `regulation_defaults`, ni aucun champ propre à la feature n'y figure. La route [`POST /reset-regulation-defaults`](../API.md#post-reset-regulation-defaults--critical) **réutilise le mécanisme existant** `requestConfigBroadcast()` ([`ws_manager.h`](../../src/ws_manager.h)) : broadcast `config` **différé à la main loop** (patron `/save-config`, jamais de `_buildConfigJson()` en tâche AsyncTCP) — les autres clients connectés voient les valeurs restaurées sans reload.

> ⚠️ **Écart de parité PRÉEXISTANT relevé au passage** (non introduit ni corrigé par la feature-088) : la trame `config` ne porte **ni** `regulation_speed`, **ni** l'objet `regulation_defaults` servi par `/get-config`. Suivi dans [bug-089](../../specs/features/todo/bug-089-parite-ws-config-champs-manquants.md). Le client de la feature-088 s'en protège de deux façons : `loadConfig()` **frais avant** d'ouvrir le dialogue (les valeurs « actuelles » ne viennent jamais d'une trame partielle) et cache `_regulationDefaults` réinjecté dans une config partielle (le bouton ne clignote pas — leçon du motif « champ WS-seul qui clignote », plusieurs récidives dans ce projet). Détail côté UI : [page-settings.md](../features/page-settings.md#bouton--restaurer-les-valeurs-par-défaut--feature-088-v2310).
>
> **v2.34.0** : `stabilization_delay_min` figurait aussi dans cette liste ; le réglage ayant été **retiré de `/get-config`** (feature-091), il ne relève plus d'un écart de parité — il n'existe plus nulle part.

## Champs `sensor_data` ajoutés en feature-091 — purge adaptative (v2.34.0)

La purge des mesures pH/ORP n'est plus une minuterie mais un **verdict de stabilité** encadré par un plancher de 10 min et un plafond de 90 min ([ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md), détail complet dans [measure-validity.md](measure-validity.md)). Conséquence sur la trame : `measures_stab_remaining_s` **disparaît** (plus de fin datée à l'avance) et **quatre** champs le remplacent.

| Champ | Type | Description |
|-------|------|-------------|
| `measures_stab_elapsed_s` | uint32 | Secondes **écoulées** dans la purge courante ; `0` hors purge. Remplace `measures_stab_remaining_s`. ⚠️ Nom toujours **distinct** de `stabilization_remaining_s` (sas post-**calibration des pompes**). |
| `measures_purge_last_duration_s` | uint32 | Durée de la **dernière purge terminée** (s). `0` si aucune depuis le boot, ou si elle a été dispensée. |
| `measures_purge_last_end` | uint8 | Motif de fin de la dernière purge : `0` aucune, `1` stabilité, `2` **plafond** (alerte), `3` dispensée (coupure ≤ 10 s), `4` re-purge terminée. Valeurs figées par `enum class PurgeEnd` ([`src/measure_validity.h`](../../src/measure_validity.h)). |
| `measures_purge_count` | uint8 | Nombre de **re-purges** (refermetures C1) du cycle d'eau courant ; remis à zéro au front descendant d'eau. Saturé à 255. |

- **Parité `/data` ↔ WS obligatoire** : les cinq champs (`measures_valid` + les quatre ci-dessus) sont écrits dans le **même ordre** et depuis la **même source** (`measureValidity.getSnapshot()`) par [`ws_manager.cpp:649-653`](../../src/ws_manager.cpp) et [`web_routes_data.cpp:194-200`](../../src/web_routes_data.cpp). Motif récurrent du projet (**3 récidives**) : un champ présent d'un seul côté ne disparaît pas, il **clignote** — `latestSensorData` est remplacé par la charge `/data` toutes les 30 s, puis re-rempli par le push WS 5 s plus tard.
- Le champ `circulationSettled` du snapshot n'est **pas** publié (usage firmware seul : autorisation de la branche Programmée, C4).
- Consommateurs UI : segments d'état « Stabilisation / Re-stabilisation depuis N min » ([page-dashboard.md](../features/page-dashboard.md)), ligne « Dernière purge » de Réglages → Régulation ([page-settings.md](../features/page-settings.md)), bandeaux de plafond des pages [pH](../features/page-ph.md) et [ORP](../features/page-orp.md).

**Budget de payload** : **+136 o** ajoutés et **−39 o** retirés au pire cas, soit **+97 o**.

| Champ | Sérialisation pire cas | Octets |
|-------|------------------------|--------|
| `measures_stab_remaining_s` (retiré) | `"measures_stab_remaining_s":4294967295,` | −39 |
| `measures_stab_elapsed_s` | `"measures_stab_elapsed_s":4294967295,` | +37 |
| `measures_purge_last_duration_s` | `"measures_purge_last_duration_s":4294967295,` | +44 |
| `measures_purge_last_end` | `"measures_purge_last_end":4,` | +28 |
| `measures_purge_count` | `"measures_purge_count":255,` | +27 |

Pire cas de trame **2 745 → 2 842 o** ; le gabarit documentaire `StaticJson<>` de `_buildSensorJson()` passe de **2816 à 2944** pour rester franchement au-dessus, la marge sous `kWsFrameBufBytes` (3584 o) restant de **742 o**. Aucune troncature possible : la garde `measureJson(doc) + 1 > cap` **saute** le push au lieu de tronquer. Rappel ArduinoJson 7 : `StaticJson<N>` dérive de `JsonDocument`, `N` n'est plus qu'une valeur documentaire (`capacity()`) — le seul plafond **dur** de ce chemin reste `kWsFrameBufBytes`.

> Côté `/data`, les mêmes champs tiennent **sans bump** : le gabarit documentaire reste **2304** o (charge pire cas ≈ 2 236 o), et ce chemin n'a de toute façon aucun plafond dur (`sendJsonResponse` dimensionne la `String` à la demande).

## feature-093 (v2.34.0) — persistance de l'intégrale pH : AUCUN champ nouveau

La restauration de l'intégrale pH au démarrage ([pump-controller.md](pump-controller.md#3-persistance-nvs-conditionnelle-r1-r7)) ne publie **rien de neuf** : les champs `ph_integral`, `ph_integral_term`, `ph_integral_pct` et `ph_integral_saturated` (v2.26.0) suffisent — une valeur restaurée s'y lit comme n'importe quelle autre. Toute la traçabilité (motif de rejet, âge en temps de filtration, motif d'écriture) passe par le **journal système**, donc par `broadcastLog`.
