# Subsystem — `mqtt_manager`

- **Fichiers** : [`src/mqtt_manager.h`](../../src/mqtt_manager.h), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)
- **Singleton** : `extern MqttManager mqttManager;`
- **Lib** : [PubSubClient v2.8](https://github.com/knolleary/pubsubclient)
- **Tâche FreeRTOS dédiée** : `mqttTask` (core 0, priorité 2, stack 8 KB) — voir [ADR-0011](../adr/0011-mqtt-task-dediee.md)

## Rôle

Client MQTT avec **auto-discovery Home Assistant**. Publie les états des capteurs, relais, pompes et reçoit les commandes HA (set target pH/ORP, filtration on/off, etc.). Support Last Will & Testament.

Depuis [ADR-0011](../adr/0011-mqtt-task-dediee.md), toute l'activité réseau MQTT (`connect`, `loop`, `publish`, callback) s'exécute dans une **tâche FreeRTOS dédiée**, isolée de `loopTask`. Aucune publication ne peut bloquer la régulation pH/ORP, la filtration, ou le watchdog principal.

## Architecture producer/consumer (depuis ADR-0011)

```
loopTask (core 1)                         mqttTask (core 0, prio 2, stack 8 KB)
─────────────────                        ─────────────────────────────────────
publishXxx(topic, payload)               drainOutQueue()
  → enqueueOutbound() ─→ outQueue ────→    pop OutboundMsg → mqtt.publish()
                          (32 entrées)
publishAllStates() / publishDiagnostic()
  → flag atomique ──────────────────→    snapshot sous configMutex
                                          → ~15 publish (states) ou JSON (diag)

                                          mqtt.connect() / mqtt.loop()
                                          messageCallback(topic, payload)
                                            → enqueueInbound(InboundCmd)
drainCommandQueue()                                     │
  ← inQueue (16 entrées) ←──────────────────────────────┘
  → applique sous configMutex
    (filtration, lighting, ph_target, orp_target)
```

### Règles d'or

1. **Aucune méthode `mqtt.publish()` / `mqtt.connect()` / `mqtt.loop()`** ne s'exécute jamais depuis `loopTask`, ni depuis un handler HTTP, ni depuis aucune autre tâche FreeRTOS — uniquement depuis `mqttTask`.
2. **`mqttTask` n'agit JAMAIS directement sur les actuateurs** (`pump_controller`, `filtration`, `lighting`, `safetyLimits`). Toute commande HA reçue passe par `inQueue` puis `drainCommandQueue()` exécuté par `loopTask` sous `configMutex`.
3. **Aucun appel `Async*`** (`AsyncWebServer*`, `AsyncTCP*`) depuis `mqttTask` — la tâche `async_tcp` reste dédiée à son rôle.
4. **API publique inchangée** : les call sites historiques (`main.cpp`, `filtration.cpp`, `lighting.cpp`) appellent les méthodes `publishXxx()` exactement comme avant. Elles deviennent simplement non-bloquantes.

## API publique

```cpp
void begin();                       // Crée queues + mqttTask. Ne se connecte pas — voir requestReconnect().
void update();                      // No-op (legacy). La logique vit dans mqttTask. Call sites historiques préservés.
void connect();                     // Délègue à mqttTask via requestReconnect()
void disconnect();                  // Marque la déconnexion ; effective au prochain tick mqttTask
void requestReconnect();            // Pose un flag consommé par mqttTask
bool isConnected();                 // Lecture atomique (std::atomic<bool>) — pas de mutex requis

// Producteurs non-bloquants — appelables depuis loopTask, retournent en < 50 µs.
// La sérialisation et la publication réelle sont faites par mqttTask.
void publishSensorState(const String& topic, const String& payload, bool retain = true);
void publishAllStates();            // = pose un flag atomique ; mqttTask snapshot+publish
void publishFiltrationState();
void publishLightingState();
void publishDosingState();
void publishProductState();        // snapshot productCfg sous configMutex avant enqueue
void publishTargetState();         // snapshot mqttCfg sous configMutex avant enqueue
void publishAlert(...);            // sérialise JSON court → enqueue
void publishLog(...);
void publishStatus(...);
void publishDiagnostic();          // = pose un flag atomique ; JSON sérialisé dans mqttTask

// Drainage commandes HA — à appeler depuis loopTask à chaque tour de loop().
void drainCommandQueue();

// Arrêt propre avant ESP.restart() — flush "status=offline" puis stop mqttTask.
void shutdownForRestart();
```

## Tâche dédiée — paramètres

Configurés dans [`src/constants.h`](../../src/constants.h) :

| Constante | Valeur | Rôle |
|---|---|---|
| `kMqttTaskStackSize` | `8192` | Stack FreeRTOS (couvre `mqtt.connect()` + handshake CONNACK + JSON discovery) |
| `kMqttTaskPriority` | `2` | Bas, > IDLE, < `tiT` (lwip) et `async_tcp` |
| `kMqttTaskCore` | `0` | `loopTask` est sur core 1 — répartit la charge réseau |
| `kMqttOutQueueLength` | `32` | File sortante (~3 s de débit nominal) |
| `kMqttInQueueLength` | `16` | File entrante (commandes HA) |
| `kMqttTaskLoopTimeoutMs` | `100` | Timeout `xQueuePeek` dans `mqttTask` |
| `kMqttOfflineFlushMs` | `1000` | Timeout flush `status=offline` avant restart |
| `kMqttClientConnectTimeoutSec` | `2` | **Timeout en SECONDES** passé à `WiFiClient::setTimeout()` — borne le SYN/CONNACK TCP côté broker injoignable. Voir avertissement ci-dessous |

Le high-water-mark de la stack est exposé dans le payload `diagnostic` MQTT (champ `mqtt_task_stack_hwm`) — utile pour réduire `kMqttTaskStackSize` si on observe un large headroom après plusieurs jours.

## Topics

Structure complète dans [`MqttTopics`](../../src/mqtt_manager.h). Résumé :

```
{base}/temperature
{base}/ph, {base}/ph_target, {base}/ph_target/set, {base}/ph_dosing, {base}/ph_limit
{base}/orp, {base}/orp_target, {base}/orp_target/set, {base}/orp_dosing, {base}/orp_limit
{base}/ph_regulation_mode, {base}/ph_daily_target_ml
{base}/orp_regulation_mode, {base}/orp_daily_target_ml
{base}/ph_remaining_ml, {base}/ph_stock_low
{base}/orp_remaining_ml, {base}/orp_stock_low
{base}/filtration_state, {base}/filtration/set
{base}/filtration_mode, {base}/filtration_mode/set
{base}/lighting_state, {base}/lighting/set
{base}/status                 (LWT : "online" / "offline")
{base}/alerts
{base}/logs
{base}/diagnostic
```

Voir [`docs/MQTT.md`](../MQTT.md) pour la liste exhaustive avec les entités HA correspondantes.

## Auto-discovery Home Assistant

`publishDiscovery()` publie 17 messages `retain=true` sur `homeassistant/.../config` pour déclarer automatiquement les entités. **Exécutée uniquement depuis `mqttTask`** lors d'une connexion réussie (lambda interne, pas appelable de l'extérieur). Publié **une seule fois** par session (`discoveryPublished` guard).

## Intervalles

- Publication d'état périodique : **10 s** (`kMqttPublishIntervalMs` [`constants.h:17`](../../src/constants.h:17)). Déclenchée depuis `loopTask` par un simple flag atomique — la publication réelle se fait dans `mqttTask`.
- Publication diagnostic : **5 min** (`kDiagnosticPublishIntervalMs` [`constants.h:19`](../../src/constants.h:19)). Idem.

## Keepalive

`mqtt.setKeepAlive(60)` dans `MqttManager::begin()` ([`mqtt_manager.cpp:33`](../../src/mqtt_manager.cpp:33)) — PubSubClient envoie un `PINGREQ` après 60 s d'inactivité côté client si aucun publish n'a circulé.

Conséquence côté broker : Mosquitto (et la plupart des brokers MQTT) applique `1.5 × keepalive` comme délai max sans paquet reçu avant de fermer la session. Avec un keepalive de 60 s, **la tolérance broker est de 90 s** (contre 45 s avec le précédent réglage de 30 s).

**Trade-off** : une vraie déconnexion (broker arrêté, lien WiFi coupé) est détectée en 90 s au lieu de 45 s. Acceptable pour la régulation pH/ORP qui ne dépend pas d'une latence sub-minute. En contrepartie, le client tolère sans broncher des microcoupures réseau jusqu'à ~60 s — utile sur les chemins instables (CPL/Powerline, WiFi à RSSI marginal).

## Reconnexion

Backoff exponentiel : 5 s → 10 s → 20 s → ... → **120 s max** (`_reconnectDelay` [`mqtt_manager.h`](../../src/mqtt_manager.h)).

Reset sur appel explicite à `requestReconnect()` (après changement de config broker, username, etc.). **Le flag est consommé par `mqttTask` à la prochaine itération** (< 100 ms après l'appel).

> ⚠️ La reconnexion est **entièrement pilotée par `mqttTask`** — rate-limit + backoff internes. **Aucun appel externe à `requestReconnect()` ne doit être fait en boucle**, sous peine de réinitialiser le backoff et de générer une tentative toutes les 5 s en permanence sur broker injoignable. Voir [ADR-0010](../adr/0010-stabilite-mqtt-reseau.md).

### Connexion initiale au boot

Aucun appel à `mqttManager.requestReconnect()` n'est fait depuis `setup()` ([`main.cpp`](../../src/main.cpp)). Dès que `MqttManager::begin()` a démarré `mqttTask`, le premier tour de `taskLoop()` détecte `!mqtt.connected() && mqttCfg.enabled` et lance `connectInTask()` tout seul. Cela évite la **race observée avant ADR-0011 itération 2** : la 1ʳᵉ connexion réussissait via `mqttTask`, le `requestReconnect()` posé par `setup()` la marquait pour reconnexion, `mqttTask` se déconnectait/reconnectait → 2× publication d'auto-discovery (32 publishes au lieu de 17), donc 2× l'exposition aux blocages réseau juste après le boot.

## Pattern de connexion (DNS séparé du TCP)

`MqttManager::connectInTask()` ([`mqtt_manager.cpp`](../../src/mqtt_manager.cpp)) sépare explicitement la résolution DNS de la connexion TCP. **Cette logique reste valide depuis ADR-0010**, mais elle s'exécute désormais dans `mqttTask`, donc ses blocages éventuels n'affectent plus `loopTask`.

1. **Court-circuit IP littérale** : `IPAddress::fromString(mqttCfg.server)` — si la valeur configurée est déjà une IP, on l'utilise directement, **aucun appel DNS**.
2. **Sinon, résolution DNS** : `WiFi.hostByName(server, brokerIp)` (timeout lwip ~5 s). En cas d'échec, doublement du backoff et retour immédiat.
3. `mqtt.setServer(brokerIp, port)` — passe l'**`IPAddress`** déjà résolue à PubSubClient.
4. `esp_task_wdt_reset()` — reset WDT juste avant le connect bloquant (depuis `mqttTask`, dont le watchdog est aussi à 30 s).
5. `mqtt.connect(...)` — TCP only (pas de DNS interne puisque l'IP est déjà fournie).

Comportement résumé selon la valeur de `mqttCfg.server` :

| Valeur configurée | Chemin emprunté |
|---|---|
| IP littérale (`192.168.1.10`, `10.0.0.5`, ...) | skip lwip — `setServer(IPAddress, port)` direct |
| Hostname (`mosquitto.local`, `homeassistant.lan`, ...) | `WiFi.hostByName()` puis `setServer(IPAddress, port)` |

Sans cette séparation, `WiFiClient::connect(hostname, port)` enchaîne DNS + TCP dans un seul appel synchrone. `mqtt.setSocketTimeout(2)` borne en plus chaque tentative TCP à 2 s max.

> **Pourquoi cela ne suffisait pas avant ADR-0011** : `setSocketTimeout(2)` couvre le **connect TCP**, pas `lwip_write` qui peut bloquer indéfiniment quand le réseau est lossy (CPL bruyant en particulier). Trois crashes production ont eu pour cause une publication de 33 octets bloquant > 30 s dans `lwip_write`. La tâche dédiée isole ces blocages de `loopTask`.

### Timeout client TCP — UNITÉ EN SECONDES

> ⚠️ **AVERTISSEMENT — `WiFiClient::setTimeout()` attend des SECONDES, pas des millisecondes.**
> Dans Arduino-ESP32 6.9.0 ([`WiFiClient.cpp:327`](https://github.com/espressif/arduino-esp32/blob/2.0.x/libraries/WiFi/src/WiFiClient.cpp)), la méthode est définie comme `setTimeout(uint32_t seconds)` avec en interne `_timeout = seconds * 1000`. Le code historique appelait `wifiClient.setTimeout(5000)`, ce qui programmait **5000 secondes (~83 minutes)** au lieu des 5 s attendues — bug latent qui rendait le timeout côté client TCP totalement inopérant pendant un connect sur broker injoignable. La constante `kMqttClientConnectTimeoutSec = 2` ([`constants.h`](../../src/constants.h)) documente l'unité explicitement et fixe la borne à 2 s. **Ne JAMAIS remettre une valeur en ms ici.**

Cette borne couvre le pire cas d'un SYN TCP qui retransmet sur broker injoignable (sans elle, l'ordre `TCP_SYNMAXRTX` × backoff exponentiel de lwip cumulait jusqu'à ~75 s avant abandon — assez pour faire PANIC le watchdog 30 s même depuis `mqttTask`).

## Watchdog dans `mqttTask`

`mqttTask` est enregistrée auprès du watchdog ESP-IDF dès son démarrage :

```cpp
esp_task_wdt_add(NULL);   // mqtt_manager.cpp:114, début de mqttTaskFunction
```

Le timeout watchdog par tâche reste celui de `kWatchdogTimeoutSec = 30 s`. Pour rester sous cette borne quelles que soient les conditions réseau, des `esp_task_wdt_reset()` sont insérés sur tous les chemins potentiellement bloquants.

Depuis IT4 (cf. ADR-0011 « Évolutions »), **les `esp_task_wdt_reset()` granulaires de IT3 (un reset avant chaque `mqtt.publish()`) ont été factorisés dans le wrapper `safePublish()`** — voir section « Garde-fou : `safePublish()` + socket non-bloquant (IT4) » ci-dessous. Tous les call sites `mqtt.publish(...)` directs ont été remplacés par `safePublish(...)`, et les checks `if (!mqtt.connected()) return;` répétés ont été supprimés (le wrapper retourne `false` silencieusement). Les emplacements résiduels visibles dans le code source sont :

| # | Emplacement (`mqtt_manager.cpp`) | Rôle |
|---|---|---|
| 1 | `taskLoop()` début | Reset à chaque tour, **avant** toute opération réseau |
| 2 | `connectInTask()` juste avant `mqtt.connect()` | Borne le SYN TCP + handshake CONNACK |
| 3 | `connectInTask()` juste après `mqtt.connect()` | Borne le pire cas connect/CONNACK même quand `connect()` retourne `false` (broker injoignable, retransmits SYN cumulés) |
| 4 | Branche `if (connected)` après reconnexion réussie | Avant `subscribe()` et `publishDiscovery()` |
| 5 | `safePublish()` (wrapper, ~ligne 270) | Reset **avant chaque** appel `mqtt.publish()` — couvre les **24 call sites** : `drainOutQueue`, `publishAllStatesInternal` (20 publishes), lambda `publishConfig` de `publishDiscovery`, `publishDiagnosticInternal`, status `online` au connect |

### Cadence garantie

- **`safePublish()`** : tout `mqtt.publish()` est précédé d'un `esp_task_wdt_reset()` puis d'un check `mqtt.connected()`. Cadence wdt **1:1** quel que soit l'appelant. Si la socket est fermée par lwip, le wrapper retourne `false` immédiatement et l'appelant enchaîne sur le publish suivant — les salves de 17 (auto-discovery) ou 20 (states) sont court-circuitées en quelques millisecondes au lieu de continuer à appeler `mqtt.publish()` sur une socket morte.
- **`connectInTask()`** : reset avant ET après `mqtt.connect()` (#2 et #3) — couvre le cas où `connect()` peut bloquer ~75 s sur broker injoignable si `WiFiClient::setTimeout()` n'était pas posé (cf. avertissement section précédente).

> **Rationnel des resets #2 et #3** : avant ADR-0011 itération 2, seul un reset existait avant `mqtt.connect()`. Pendant le test D2 (câble Ethernet HA débranché), `mqttTask` est restée bloquée dans `WiFiClient::connect()` → `lwip_select` plus de 30 s sur le SYN TCP retransmis, déclenchant un PANIC watchdog avant qu'aucun reset ultérieur ne soit atteint. L'ajout du reset post-`connect()` (#3) borne le pire cas même quand `connect()` retourne sans appeler aucun autre code (échec immédiat ou abandon TCP).
>
> **Rationnel wrapper `safePublish()` (IT4)** : le 3ᵉ re-test D2 humain APRÈS le flash IT3 a déclenché un nouveau PANIC, cette fois dans `drainOutQueue()` (publish d'un message ~110 octets `orp_limit`) — fonction qui n'avait pas été instrumentée par IT3 (oubli) et qui consomme `outQueue` (alertes, status, logs, états relais). En parallèle, l'analyse a révélé que `CONFIG_LWIP_TCP_MAXRTX=5` borne en réalité à ~93 s (et non ~10 s) à cause du `TCP_RTO_INITIAL=3 s` × backoff exponentiel — bien au-delà du watchdog 30 s. Le pivot architectural d'IT4 est donc de **passer la socket TCP en mode non-bloquant** (cf. section suivante), puis d'imposer un **wrapper unique `safePublish()`** sur les 24 call sites de `mqtt.publish()` dans `mqttTask`.

## Garde-fou : `safePublish()` + socket non-bloquant (IT4)

> ⚠️ **AVERTISSEMENT — `WiFiClient::availableForWrite()` retourne TOUJOURS 0 dans Arduino-ESP32 6.9.0.**
> La méthode héritée de `Print::availableForWrite()` n'est **pas surchargée** par `WiFiClient`, donc elle renvoie systématiquement la valeur par défaut `0`. **Ne JAMAIS l'utiliser** pour décider si la socket peut accepter un `write()`. C'est cette découverte qui a fait pivoter le plan IT4 : la constante `kMqttPublishHeadersOverhead` envisagée (F12) a été annulée, et le bornage repose désormais sur le **mode non-bloquant** via `fcntl()`. Référence : implémentation `WiFiClient::write()` qui appelle `Print::availableForWrite()` sans override.

### Mode non-bloquant via `fcntl()`

Après chaque `mqtt.connect()` réussi, dans `connectInTask()`, la socket TCP sous-jacente est passée en mode non-bloquant :

```cpp
// Récupère le fd natif du WiFiClient et active O_NONBLOCK
int fd = wifiClient.fd();
int flags = fcntl(fd, F_GETFL, 0);
if (flags >= 0) {
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // socket non-bloquante : write retourne EAGAIN si buffer plein
}
```

**Effet** : tout `WiFiClient::write()` ultérieur retourne immédiatement avec `EAGAIN` si le send buffer TCP est plein (au lieu de bloquer dans `lwip_select`). `PubSubClient::publish()` propage cette erreur en retournant `false` (sans retry interne — confirmé `PubSubClient.cpp:599`). `safePublish()` retourne alors `false` aussi → drop silencieux du message.

### Wrapper `safePublish()`

```cpp
// mqtt_manager.cpp, ~ligne 270
bool MqttManager::safePublish(const char* topic, const char* payload, bool retain) {
  // Borne le watchdog — un seul reset par publish, cadence 1:1 garantie
  esp_task_wdt_reset();
  // Bail-out fail-fast : socket fermée par lwip ou jamais connectée
  if (!mqtt.connected()) return false;
  // mqtt.publish() ne peut plus bloquer : socket en O_NONBLOCK,
  // retourne false si write retourne EAGAIN (buffer TCP plein)
  return mqtt.publish(topic, payload, retain);
}
```

Trois garanties combinées :
1. **Watchdog reset** systématique avant chaque publish.
2. **Court-circuit** si la socket n'est plus connectée (au lieu de subir un timeout TCP).
3. **Aucun blocage** dans `lwip_write` — la socket non-bloquante propage `EAGAIN` instantanément.

### Call sites couverts (24 au total)

Tous les `mqtt.publish(...)` directs dans `mqttTask` ont été remplacés par `safePublish(...)` :

| Site | Rôle |
|---|---|
| `connectInTask()` — status `online` au connect | LWT `online` après handshake réussi |
| `drainOutQueue()` | Consomme `outQueue` (alertes, status, logs, états relais asynchrones) |
| `publishAllStatesInternal()` | **20 publishes** des états périodiques (température, pH, ORP, targets, dosing, mode régulation, daily, remaining, stock_low, filtration, lighting) |
| `publishDiagnosticInternal()` | Snapshot diagnostic (heap, RSSI, uptime, hwm, etc.) |
| `publishDiscovery()` lambda `publishConfig` | **17 publishes** d'auto-discovery HA `homeassistant/.../config` |

Les `esp_task_wdt_reset()` IT3 et les bail-out `if (!mqtt.connected()) return;` IT3 répartis dans `publishAllStatesInternal()` et la lambda `publishConfig` ont été **supprimés** : ils sont devenus redondants avec le wrapper et alourdissaient la lecture (~50 lignes supprimées).

### Trade-off accepté

- **Drop silencieux des publish quand le send buffer TCP est plein** : pas de retry, pas de reput dans `outQueue`. Acceptable parce que :
  - Les **états retain** (température, pH, ORP, targets, …) seront republiés au prochain `publishAllStatesInternal()` post-reconnect (cadence 10 s).
  - Les **alertes ponctuelles** (`publishAlert`) **peuvent être perdues**. C'était déjà le cas avec `PubSubClient` en mode bloquant qui timeoutait sur send buffer plein avant ADR-0011 — IT4 ne dégrade pas cette propriété, il rend juste le drop instantané et silencieux au lieu de bloquer 30 s puis dropper.
  - L'auto-discovery HA est republiée à chaque reconnect (`discoveryPublished` reset à la déconnexion) → un drop pendant la salve initiale est rattrapé au cycle suivant.

## Bornage TCP côté lwip

Depuis IT3 d'ADR-0011, `platformio.ini` impose un override global de la pile lwip :

```ini
build_flags =
  ...
  -DCONFIG_LWIP_TCP_MAXRTX=5    ; feature-014 IT3 / ADR-0011 — borne le nombre
                                ; de retransmissions TCP avant abandon de socket
```

**Effet escompté à l'origine d'IT3** : après ~5 retransmissions infructueuses d'un segment TCP, la pile abandonne la socket → `mqtt.connected()` retourne `false` → les publish suivants sont court-circuités par les bail-out installés dans `publishAllStatesInternal()` et `publishDiscovery()`.

> ⚠️ **Découverte IT4 — le bornage réel est ~93 s, pas ~10 s.** L'analyse IT3 estimait ~10 s cumulés sur 5 retransmissions, mais avec `TCP_RTO_INITIAL=3000 ms` (default lwip) et backoff exponentiel × 2, la séquence des retransmits est en réalité :
>
> - retrans 1 : T+3 s
> - retrans 2 : T+9 s (3+6)
> - retrans 3 : T+21 s (9+12)
> - retrans 4 : T+45 s (21+24)
> - retrans 5 : T+93 s (45+48)
>
> **Pire cas réel ≈ 93 s avant abandon de socket par lwip**, bien au-delà du watchdog 30 s. `CONFIG_LWIP_TCP_MAXRTX=5` **seul ne suffit pas** à protéger `mqttTask`. Le vrai mécanisme de protection contre les blocages `lwip_write` est le **mode non-bloquant** via `fcntl(F_SETFL, O_NONBLOCK)` introduit en IT4 (cf. section « Garde-fou : `safePublish()` + socket non-bloquant »). Le bornage TCP est conservé comme **garde-fou complémentaire** : il accélère la fermeture propre de la socket côté lwip une fois qu'elle est en retransmit prolongé, ce qui permet à `mqtt.connected()` de retourner `false` plus rapidement.

> ⚠️ **Trade-off — ce paramètre est GLOBAL à toute la pile lwip.** Il s'applique **à toutes les sockets TCP** ouvertes par le firmware, pas uniquement à MQTT :
> - **AsyncWebServer** : un client web mal connecté verra sa socket fermée plus rapidement (~93 s vs bien au-delà avec la valeur lwip par défaut de 12). Imperceptible en LAN sain.
> - **OTA HTTP** : un push OTA en réseau dégradé (CPL très lossy, RSSI marginal) peut être avorté plus tôt — l'humain doit relancer manuellement. Acceptable car OTA est déjà supervisé interactivement.
> - **NTP** : abandon plus rapide d'une requête NTP qui ne répond pas. NTP a son propre retry applicatif, sans impact.
>
> **Pourquoi pas un timeout par-socket ?** L'option lwip `TCP_USER_TIMEOUT` (RFC 5482), qui aurait permis de borner uniquement la socket MQTT et pas les autres, **n'est pas supportée par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). On retombe donc sur le paramètre global. Si une migration future vers ESP-IDF 5.x apporte le support de `TCP_USER_TIMEOUT`, on pourra ré-isoler le bornage à la socket MQTT et restaurer le default lwip global.

## Diagnostic — Codes d'état PubSubClient

Au front de transition « connecté → déconnecté », `mqttTask` émet :

```
WARN: MQTT déconnecté détecté — état=N
```

où `N` est le code retourné par `PubSubClient::state()`. Le log est **edge-triggered** (une seule ligne par perte de connexion, pas de spam pendant la phase de reconnexion). Codes utiles pour le diagnostic :

| Code | Constante | Cause typique |
|---|---|---|
| `-4` | `MQTT_CONNECTION_TIMEOUT` | **Code dominant depuis IT4** sur perte de connexion réseau (câble Ethernet HA débranché, panne de chemin) — keepalive PubSubClient (60 s) qui timeout sans `PINGRESP`, `_client->stop()` interne pose `_state = MQTT_CONNECTION_TIMEOUT`. Détection en **~60 s** après le début de la coupure. Voir section « Détection des déconnexions » plus bas |
| `-3` | `MQTT_CONNECTION_LOST` | socket TCP invalide — broker arrêté/redémarré, câble débranché, NAT timeout, réseau coupé. **Minoritaire depuis IT4** — observé essentiellement quand le broker se ferme proprement et envoie un `FIN`/`RST`, ou quand lwip abandonne ses retransmissions (90–180 s). Avant IT4 (mode bloquant) c'était le code dominant |
| `-2` | `MQTT_CONNECT_FAILED` | TCP refusé au moment du connect — broker injoignable (port fermé, firewall) |
| `-1` | `MQTT_DISCONNECTED` | déconnexion propre côté client (`disconnect()` ou `requestReconnect()`) |

À distinguer du log `ERROR: MQTT échec, code=N` émis dans la branche `connect()` lorsque le **handshake CONNECT applicatif** échoue (codes ≥ 1 : auth refusée, version protocole non supportée, etc.).

## Détection des déconnexions

Dans `MqttManager::taskLoop()` ([`mqtt_manager.cpp:141`](../../src/mqtt_manager.cpp:141)), le test `mqtt.connected()` est appelé **avant** `mqtt.loop()`. Cet ordre détermine quel code d'état PubSubClient est observable à l'arrivée :

```cpp
// Single source of truth pour l'UI et le diag — mis à jour à chaque tour, AVANT toute branche
connectedAtomic.store(mqtt.connected(), std::memory_order_relaxed);

if (!mqtt.connected() && mqttCfg.enabled) {
  // -> état = -3 (MQTT_CONNECTION_LOST) ou -4 (MQTT_CONNECTION_TIMEOUT)
  //    selon le mécanisme de détection (cf. tableau ci-dessous)
  ...
} else if (mqtt.connected()) {
  mqtt.loop();  // n'est appelé que si la socket est encore vivante
}
```

### Bascule de dominance entre `-3` et `-4` depuis IT4

> **Avant IT4 (mode bloquant)** : `mqtt.publish()` bloquait dans `WiFiClient::write` → `lwip_select` quand le send buffer TCP était plein. `mqtt.loop()` n'avait alors plus le temps de gérer le keepalive applicatif. C'était lwip qui finissait par fermer la socket après ses retransmissions TCP (~90–180 s), `mqtt.connected()` retournait `false`, et l'état observable se figeait sur **`-3` (`MQTT_CONNECTION_LOST`)**. Le code `-4` était théorique et jamais atteint.
>
> **Depuis IT4 (mode non-bloquant via `fcntl(O_NONBLOCK)`)** : tout `WiFiClient::write()` retourne immédiatement avec `EAGAIN` si le send buffer est plein. `mqtt.loop()` continue donc à tourner régulièrement et **le keepalive applicatif PubSubClient (60 s) devient le mécanisme de détection effectif** : un `PINGREQ` est posté (qu'il passe ou non n'a pas d'importance), aucun `PINGRESP` ne revient, et après ~60 s sans `PINGRESP` PubSubClient pose `_state = MQTT_CONNECTION_TIMEOUT` via un `_client->stop()` interne. Au tour suivant de `taskLoop()`, `mqtt.connected()` retourne `false` et l'état observable est désormais **`-4` (`MQTT_CONNECTION_TIMEOUT`)**.

**Conséquence pratique** : le code `-4` est désormais **dominant** lors d'une coupure réseau (ex. câble Ethernet HA débranché). `-3` reste possible mais devient **minoritaire** — typiquement quand le broker se ferme proprement (envoi `FIN`/`RST` reçu par lwip) ou quand lwip abandonne ses retransmissions avant le keepalive (cas tordus de timing).

**Bénéfice indirect d'IT4** : la détection est désormais **plus précoce** (~60 s, borné par le keepalive applicatif) et **plus propre** (timeout applicatif vs socket arrachée par la pile TCP) qu'avant. Avant IT4, on dépendait du mécanisme d'abandon TCP de lwip qui pouvait prendre 90 à 180 s selon le `RTO_INITIAL` et le nombre de retransmissions.

### `connectedAtomic` — single source of truth

`MqttManager::isConnected()` (lue par l'UI via `/data` et par le payload diagnostic MQTT) ne lit **pas** directement `mqtt.connected()` — elle lit `connectedAtomic.load(std::memory_order_relaxed)`. La cohérence repose sur **un seul store canonique** posé en début de chaque tour de `taskLoop()` (cf. snippet ci-dessus), avant toute branche de reconnect/loop/drain.

Deux transitions explicites complètent ce store canonique :
- `connectInTask()` après un `mqtt.connect()` réussi → `connectedAtomic.store(true)` immédiatement (sans attendre le prochain tour de `taskLoop()`).
- `disconnect()` (appelable depuis `loopTask`) → `connectedAtomic.store(false)` avant `mqtt.disconnect()`.

Les stores intermédiaires redondants présents avant ADR-0011 itération 2 (3 emplacements supplémentaires dans `taskLoop()` post-publish) ont été supprimés : ils créaient une fenêtre de divergence où l'UI affichait « Déconnecté » pendant que `mqtt.connected()` retournait encore `true` côté `mqttTask` (et inversement, pas de `WARN: MQTT déconnecté détecté` parce que la transition `mqtt.connected()` n'avait pas encore été observée). Cette divergence a été rapportée pendant le test manuel D2 — supprimée par cette refactorisation.

`PubSubClient::connected()` interroge la socket TCP sous-jacente. Avec le mode non-bloquant introduit en IT4, `mqtt.loop()` reste appelé tant que la socket n'a pas été abattue, ce qui laisse le keepalive applicatif arriver au bout : après ~60 s sans `PINGRESP`, `PubSubClient::loop()` détecte le timeout, exécute `_client->stop()` en interne (qui invalide la socket TCP) et pose `_state = MQTT_CONNECTION_TIMEOUT` (`-4`). Au tour suivant de `taskLoop()`, `mqtt.connected()` retourne `false` et le WARN edge-triggered est émis avec `état=-4`. Le code `-3` (`MQTT_CONNECTION_LOST`) reste possible quand la socket est invalidée par un autre chemin (FIN/RST reçu, abandon retransmissions lwip avant le keepalive), mais devient minoritaire.

### Bail-out fail-fast pendant les salves de publish (IT3 → IT4)

L'IT3 d'ADR-0011 introduisait des bail-out `if (!mqtt.connected()) return;` répartis dans `publishAllStatesInternal()` (5 sites) et en tête de la lambda `publishConfig` de `publishDiscovery()`. **Ces bail-out manuels ont été supprimés en IT4** au profit du wrapper `safePublish()` qui intègre la même logique de manière centralisée (cf. section « Garde-fou : `safePublish()` + socket non-bloquant (IT4) » plus haut). Tout `mqtt.publish()` direct passe désormais par `safePublish()`, qui retourne `false` si la socket est fermée — l'appelant enchaîne sur le publish suivant sans surcoût mesurable.

Le rôle fonctionnel est identique (réduire la durée totale d'une salve dégradée une fois que lwip a abandonné la socket), mais la mécanique est désormais portée par le wrapper unique au lieu d'être éparpillée.

### États observés selon le scénario (depuis IT4)

| Scénario | État typique | Délai de détection |
|---|---|---|
| Câble Ethernet débranché côté serveur ou switch HA | `-4` | **~60 s** — keepalive PubSubClient sans `PINGRESP` ; `_client->stop()` interne pose `MQTT_CONNECTION_TIMEOUT`. **Validé en test D2 humain (4ᵉ tentative, 2026-04-29)** |
| Broker tué brutalement (`docker stop`, kill `-9`, panne) | `-4` (dominant) | **~60 s** — même mécanisme, pas de `FIN`/`RST` propre côté broker → keepalive applicatif gagne sur l'abandon TCP lwip |
| Broker tué brutalement (variante minoritaire) | `-3` | **90–180 s** — si lwip abandonne ses retransmissions avant le keepalive (timing tordu, dépend du `RTO_INITIAL` et de la charge réseau au moment du dernier publish) |
| Fermeture TCP propre (broker `SIGTERM`, sessions fermées proprement) | `-3` | quasi-immédiat (lwip reçoit le `FIN`/`RST`) |
| Broker injoignable au moment du connect (port fermé, firewall) | `-2` | détecté par `setSocketTimeout(2)` au prochain `connect()` |
| `disconnect()` ou `requestReconnect()` côté firmware | `-1` | immédiat |

> **Cohérence avec les tests manuels feature-014 (D1/D2)** — la timeline ~60 s observée en test D2 IT4 reflète le keepalive applicatif PubSubClient (60 s sans `PINGRESP`) qui devient le mécanisme dominant grâce au mode non-bloquant. C'est **plus précoce et plus propre** qu'avant IT4 (où le mécanisme d'abandon TCP lwip mettait 90–180 s à détecter). La régulation pH/ORP, la filtration et le watchdog continuent indépendamment pendant toute la fenêtre de détection grâce à l'architecture `mqttTask` (cf. [ADR-0011](../adr/0011-mqtt-task-dediee.md)).

## Gestion des commandes HA reçues

`messageCallback()` s'exécute dans `mqttTask` (via `mqtt.loop()`). Il **ne fait rien d'autre** que :

1. Identifier le topic reçu et déterminer le `InboundCmdType` correspondant (`FiltrationMode`, `FiltrationOnOff`, `Lighting`, `PhTarget`, `OrpTarget`).
2. Copier le payload (≤ 64 octets) dans une struct `InboundCmd`.
3. `xQueueSend(inQueue, &cmd, 0)` — non-bloquant. Si la queue est saturée, un WARN est loggé et la commande est abandonnée.

L'application réelle se fait dans `drainCommandQueue()`, **appelé depuis `loopTask` à chaque tour de `loop()`**. Cette méthode :
- Pop jusqu'à 8 commandes par tour pour éviter de saturer `loopTask` en cas de rafale.
- Applique sous `configMutex` (`saveMqttConfig()`, mise à jour `mqttCfg`/`filtrationCfg`).
- Appelle `publishXxx()` pour publier le nouvel état (qui ré-enfile dans `outQueue` → `mqttTask` → broker).

| Topic reçu | Action `loopTask` (sous configMutex) |
|---|---|
| `{base}/filtration_mode/set` | `filtrationCfg.mode = "auto"|"manual"|"force"|"off"` + `filtration.computeAutoSchedule()` si auto + `saveMqttConfig()` + `publishFiltrationState()` |
| `{base}/filtration/set` | `filtrationCfg.forceOn/forceOff` ; `filtration.update()` republie après changement réel du relais |
| `{base}/lighting/set` | `lighting.setManualOn/Off()` + `publishLightingState()` |
| `{base}/ph_target/set` | `mqttCfg.phTarget = value` (clamp 6.0–8.5) + `saveMqttConfig()` + `publishTargetState()` |
| `{base}/orp_target/set` | `mqttCfg.orpTarget = value` (clamp 400–900 mV) + `saveMqttConfig()` + `publishTargetState()` |

## LWT / Status

Connexion avec `willTopic = {base}/status`, `willMessage = "offline"`, `willRetain = true`. Dès la connexion réussie, publication de `"online"`.

**Arrêt propre OTA / factory reset** : `MqttManager::shutdownForRestart()` est appelée depuis tous les sites de `ESP.restart()` (`web_server.cpp`, `web_routes_config.cpp`, `main.cpp` factory reset). Elle :
1. Enfile `status=offline` dans `outQueue`.
2. Attend jusqu'à `kMqttOfflineFlushMs` (1 s) que `mqttTask` draine la queue.
3. Pose le flag `taskShouldStop` ; `mqttTask` sort proprement de `taskLoop()` et appelle `vTaskDelete(NULL)`.

Cela permet aux clients HA de voir le passage à `offline` immédiatement, sans attendre les 90 s de timeout broker.

## Cas limites

- **MQTT désactivé** (`mqtt_enabled = false`) : `begin()` crée la tâche et les queues mais `connectInTask()` n'agit pas tant que le toggle reste off. Les producteurs (`publishXxx`) continuent d'enfiler dans `outQueue`, qui sera draina-silencieusement-droppée tant que pas connecté (pas d'accumulation indéfinie).
- **WiFi disponible mais broker injoignable** : backoff exponentiel dans `mqttTask`, **aucun blocage de `loopTask`**.
- **Broker accepte puis refuse auth** : déconnexion, tentative de reconnexion avec le backoff, log WARN.
- **Queue `outQueue` saturée** (publish plus rapide que ce que `mqttTask` peut écouler) : drop best-effort du message le plus ancien, log WARN edge-triggered (`MQTT outQueue saturée — N message(s) abandonné(s)` une fois par fenêtre 5 s). En pratique, 32 entrées correspondent à ~3 s de débit nominal — il faudrait une saturation broker prolongée pour les voir.
- **Queue `inQueue` saturée** (rafale de commandes HA) : la commande la plus récente est abandonnée, log WARN. HA peut renvoyer la commande, pas critique.
- **Crash dans `mqttTask`** : la tâche est nommée `mqttTask`, elle apparaît dans le coredump (`GET /coredump/info`) avec sa backtrace propre, distincte de `loopTask`.

## Troubleshooting — reconnexions répétées

En cas de reconnexion MQTT répétée, suivre ce workflow :

1. **Repérer le log de cause** : `WARN: MQTT déconnecté détecté — état=N` est émis une fois par perte de connexion. Le code `N` oriente immédiatement le diagnostic (voir tableau « Codes d'état PubSubClient » plus haut).
2. **Corréler avec les logs WiFi** (section « Logs WiFi » de [`docs/subsystems/logger.md`](logger.md)) :
   - Présence de lignes `WARN: WiFi déconnecté (reason=...)` synchrones avec les reconnexions MQTT → drop Wi-Fi sous-jacent (AP, RSSI, DHCP renew). Le client MQTT subit la perte de transport, ce n'est pas un problème de broker. Typiquement vu avec `état=-3` ou `état=-4`.
   - Aucun log WiFi mais `mqtt_manager` enchaîne les tentatives → cause côté broker ou réseau applicatif :
     - `état=-4` (timeout keepalive) → **cas le plus fréquent depuis IT4** : coupure réseau côté HA (câble débranché, AP HS), broker tué brutalement sans `FIN`/`RST`, packet loss prolongé. Détection en ~60 s.
     - `état=-3` (connection lost) → fermeture TCP propre (broker `SIGTERM`, NAT timeout du routeur émettant un `RST`), ou abandon retransmissions lwip avant le keepalive (90–180 s, minoritaire).
     - `état=-2` (connect failed) → broker injoignable (port, firewall) ou échec DNS — vérifier alors le log `ERROR: MQTT échec DNS pour '...'`.
     - `état=-1` (disconnected propre) → déclenché par un `requestReconnect()` côté firmware (changement de config broker, etc.).
3. **Si WiFi et broker sont innocentés** (aucun log `WARN: WiFi déconnecté`, broker stable côté HA/Mosquitto, RSSI correct côté ESP32), envisager le **chemin physique entre l'ESP32 et le routeur** :
   - **CPL/Powerline** : les prises CPL souffrent de microcoupures provoquées par le bruit électrique secteur (alimentations à découpage, moteurs, variateurs). Quelques paquets perdus suffisent à dépasser la tolérance broker (90 s avec keepalive 60 s) et à déclencher un `état=-4`. Symptôme typique : déconnexions toutes les 10–30 min sans aucun log WiFi côté ESP32 ni côté broker. **Depuis ADR-0011, ces microcoupures n'affectent plus la régulation pH/ORP** — `mqttTask` peut bloquer 30 s sans conséquence sur `loopTask`.
   - **WiFi à RSSI marginal** (< -75 dBm) ou interférences 2.4 GHz : packet loss similaire, mais souvent corrélé à des `WARN: WiFi déconnecté reason=200` (BEACON_TIMEOUT) ou `reason=8` (ASSOC_LEAVE).
   - Mitigation : déplacer l'ESP32 vers un lien Ethernet (via bridge AP) ou un meilleur emplacement WiFi.

## Troubleshooting — drops `outQueue` répétés

Si le log `WARN: MQTT outQueue saturée — N message(s) abandonné(s)` apparaît régulièrement :

1. **Vérifier le débit du broker** : un broker surchargé ou une session keepalive proche du timeout peut ralentir les `mqtt.publish()` côté `mqttTask` qui n'arrive plus à écouler `outQueue`.
2. **Vérifier `mqtt_task_stack_hwm` dans le payload diagnostic** : si le HWM est très bas (<1000), `mqttTask` peut se bloquer dans une opération longue (DNS, connect TCP), ce qui ralentit le drain.
3. **Augmenter `kMqttOutQueueLength`** si le pic de drops correspond à des phases d'alertes simultanées (`publishAlert` × N). Coût RAM marginal (chaque entrée = ~196 octets).

## Fichiers liés

- [`src/mqtt_manager.h`](../../src/mqtt_manager.h), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)
- [`src/config.h`](../../src/config.h) — struct `MqttConfig`
- [`src/constants.h`](../../src/constants.h) — paramètres `kMqttTask*`, `kMqttOutQueueLength`, etc.
- [`src/main.cpp`](../../src/main.cpp) — `mqttManager.update()` (no-op) et `drainCommandQueue()` dans `loop()`
- [`src/web_server.cpp`](../../src/web_server.cpp) — `shutdownForRestart()` avant `ESP.restart()`
- [docs/MQTT.md](../MQTT.md) — topics complets + entités HA
- [ADR-0010](../adr/0010-stabilite-mqtt-reseau.md) — stabilité MQTT/réseau (WiFi sans power save, DNS séparé, backoff non réinitialisé) — fixes synchrones préservés dans `mqttTask`
- [ADR-0011](../adr/0011-mqtt-task-dediee.md) — déplacement de toute la logique MQTT dans `mqttTask` (cette doc reflète l'architecture post-ADR-0011)
