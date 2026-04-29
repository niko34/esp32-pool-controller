# ADR-0011 — MQTT déplacé dans une tâche FreeRTOS dédiée

- **Statut** : Accepté
- **Date** : 2026-04-27
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : [`feature-014-mqtt-task-dediee`](../../specs/features/done/feature-014-mqtt-task-dediee.md)

## Contexte

[ADR-0010](0010-stabilite-mqtt-reseau.md) a documenté cinq mesures synchrones (D1–D5) pour stabiliser MQTT sans tâche dédiée, en plafonnant le pire blocage à ~7 s par tentative de connexion. Cette borne suffisait sur le réseau LAN du domicile testé. **Elle n'a pas tenu en production.**

Trois crashes successifs ont été enregistrés sur l'installation cible, tous avec la même signature dans le coredump :

```
loopTask: loop() → publishAllStates() → publishProductState()
       → PubSubClient::publish("OFF", "pool/sensors/ph_stock_low", retain=true)
       → WiFiClient::write(size=33) bloqué dans lwip_select > 30 s
       → Watchdog ISR → PANIC
```

Le 3ᵉ crash (le plus court à reproduire) a duré **33 octets de payload** bloquants > 30 s. La cause racine est l'environnement réseau réel :

- **Liaison CPL bruyante** entre l'ESP32 et le routeur (compteur électrique perturbant le signal porteur).
- **TCP send window saturée** par les retransmits lwip qui s'empilent quand le CPL drop des paquets.
- **`PubSubClient::publish()` bloque sur `WiFiClient::write()`** qui appelle `lwip_select(write_set)` — pas de timeout côté Arduino-ESP32 sur cette opération.

Les mesures d'ADR-0010 (`setSocketTimeout(2)` notamment) couvrent le **connect TCP** et le **DNS**, pas la publication. La pile lwip ne propage pas le `socketTimeout` PubSubClient à `lwip_write`. Donc dès que le réseau commence à dropper des ACKs, **chaque `publish()` peut bloquer plusieurs dizaines de secondes**, gelant `loopTask` (régulation pH/ORP, filtration, watchdog).

Le keepalive 60 s mitige les déconnexions logiques de session MQTT, pas les blocages individuels d'un `write()`. Aucune option synchrone supplémentaire ne peut résoudre ça : tant que `mqtt.publish()` s'exécute dans `loopTask`, la régulation est suspendue.

## Décision

**MQTT vit désormais dans une tâche FreeRTOS dédiée `mqttTask`**, pinnée sur le core 0 (priorité 2, stack 8 KB), distincte de `loopTask` (core 1).

Architecture producer/consumer :

```
loopTask (core 1)                         mqttTask (core 0, prio 2, stack 8 KB)
─────────────────                        ─────────────────────────────────────
publishXxx()       → outQueue ─────────→ drainOutQueue() → mqtt.publish()
publishAllStates() → flag atomique ────→ snapshot sous mutex + ~15 publish
publishDiagnostic()→ flag atomique ────→ JSON ~400 c + 1 publish
                                         mqtt.connect() + mqtt.loop()
                                         messageCallback()  ← broker
drainCommandQueue()← inQueue   ←──────── enqueue InboundCmd
```

Implémentation côté firmware :

1. **Deux queues FreeRTOS** allouées au démarrage et conservées toute la vie du firmware :
   - `outQueue` (32 entrées × `OutboundMsg{topic[64], payload[128], retain}` = ~6.3 KB)
   - `inQueue` (16 entrées × `InboundCmd{type, payload[64]}` = ~1 KB)

2. **API publique inchangée** : `mqttManager.publishXxx()` deviennent des **producteurs non-bloquants** (enqueue + drop si plein). Aucun call site externe (`main.cpp`, `filtration.cpp`, `lighting.cpp`) ne change.

3. **`publishAllStates()` et `publishDiagnostic()` depuis `loopTask` = simple flag atomique** (`std::atomic<bool>`). `mqttTask` détecte le flag, prend les snapshots sous `configMutex`, et publie. Cela évite que `loopTask` prenne tous les mutex de capteurs pendant qu'on publie ~15 messages.

4. **`messageCallback()` (commandes HA reçues) s'exécute dans `mqttTask`** via `mqtt.loop()` mais **n'agit jamais directement sur les actuateurs** (`pump_controller`, `filtration`, `lighting`). Il poste une `InboundCmd` dans `inQueue`. `loopTask` consomme `inQueue` à chaque tour de `loop()` via `mqttManager.drainCommandQueue()` et applique l'action sous `configMutex`.

5. **Connect/reconnect/backoff entièrement dans `mqttTask`** : la pré-résolution DNS, le court-circuit IP, `setSocketTimeout(2)`, le keepalive 60 s et le backoff exponentiel d'ADR-0010 restent intégralement appliqués — mais dans la tâche dédiée, donc **leurs blocages n'affectent plus `loopTask`**.

6. **Watchdog** : `mqttTask` s'enregistre via `esp_task_wdt_add(NULL)` au démarrage et reset son watchdog à chaque itération (avant les opérations longues), respectant ainsi le timeout 30 s côté tâche.

7. **Arrêt propre OTA** : `MqttManager::shutdownForRestart()` est appelée depuis `web_server.cpp` (et tous les autres sites de `ESP.restart()`) avant le reboot. Elle enfile `status=offline` et laisse jusqu'à 1 s à `mqttTask` pour drainer la queue et publier le LWT proprement.

## Alternatives considérées

- **Garder le pattern synchrone d'ADR-0010** (rejetée) — c'est l'option qui a échoué en production trois fois. Aucune extension synchrone ne peut isoler `lwip_write` du `loopTask`.
- **Augmenter le watchdog à 60 s** (rejetée) — masque le problème, retarde la détection sans le corriger. Et insuffisant : le 3ᵉ crash était à > 33 s, on aurait juste retardé.
- **Migrer vers `esp-mqtt` (client MQTT IDF natif)** (rejetée pour ce cycle) — utilise déjà une tâche dédiée mais nécessite une migration complète du client (callbacks, auto-discovery, gestion de session). Hors scope, à reconsidérer si `PubSubClient` montre d'autres limites.
- **Désactiver la publication retain** (rejetée) — n'élimine pas le problème (le drop CPL se produit aussi sur des publish QoS 0 sans retain). Et casserait l'auto-discovery HA.
- **Tâche dédiée pour AsyncWebServer aussi** (hors scope) — déjà géré nativement par la lib AsyncTCP via `async_tcp` task. Le problème ne s'y manifeste pas.

## Conséquences

### Positives

- **`loopTask` jamais bloquée par `mqtt.publish()`** : `enqueueOutbound()` retourne en < 50 µs (mesure typique). La régulation pH/ORP, la filtration, le check des limites de sécurité et le watchdog continuent indépendamment de l'état du réseau.
- **Régulation pH/ORP préservée** pendant les microcoupures CPL : les compteurs `dailyXxxInjectedMl` continuent de s'incrémenter, les checks horaires/journaliers restent évalués, le PID continue.
- **WebSocket `/data` reste réactive** pendant qu'une tentative de connexion MQTT bloque dans `mqttTask` : la contention sur `configMutex` n'est plus simultanée à un blocage réseau.
- **Stack high-water-mark exposé** dans le payload `diagnostic` (`mqtt_task_stack_hwm`) pour pouvoir réduire `kMqttTaskStackSize` si nécessaire après collecte.
- **Arrêt propre OTA** : le LWT `status=offline` est désormais publié avant le reboot, plus seulement après timeout broker côté serveur.

### Négatives / dette assumée

- **+~16 KB heap runtime** : 8 KB de stack `mqttTask` + ~6.3 KB `outQueue` + ~1 KB `inQueue` + TCB. Marginal sur 320 KB total. RAM statique reste à 16.4 % (mesure `pio run --target size`).
- **Latence de publication +~50 ms** worst-case : un message produit par `loopTask` peut attendre la prochaine itération de `mqttTask` (timeout `xQueuePeek` = 100 ms). Imperceptible utilisateur.
- **Drops silencieux possibles** sur saturation `outQueue` (32 entrées). Logged en WARN edge-triggered (un seul log par fenêtre 5 s pour éviter le spam). En pratique, 32 entrées correspondent à ~3 s de débit nominal — il faudrait une saturation broker prolongée pour les voir.
- **Une commande HA peut être perdue** si `inQueue` (16 entrées) est saturée ET `loopTask` ne draine pas assez vite. WARN explicite dans les logs si ça arrive. Pas critique fonctionnellement (HA peut renvoyer la commande).
- **Complexité accrue du modèle de threading** : il faut désormais raisonner « depuis quelle tâche s'exécute ce code » à chaque modification de `mqtt_manager.*`. Mitigé par les commentaires explicites dans les méthodes (`// Exécutée UNIQUEMENT depuis mqttTask`).

### Ce que ça verrouille

- **Tout futur module qui voudra publier MQTT doit passer par `outQueue`** (via les méthodes `publishXxx()` existantes ou en ajoutant une nouvelle méthode qui appelle `enqueueOutbound`). **Plus jamais de `mqtt.publish()` direct depuis `loopTask` ou un handler HTTP.**
- **`mqttTask` ne doit JAMAIS appeler directement** `applyPumpDuty()`, `filtration.start()`, `lighting.setManualOn()`, `safetyLimits.*`, etc. Toute action sur les actuateurs depuis une réception MQTT passe par `inQueue` → `drainCommandQueue()` exécuté dans `loopTask` sous `configMutex`.
- **`mqttTask` n'appelle jamais d'API `Async*`** (`AsyncWebServer*`, `AsyncTCP*`). La tâche `async_tcp` reste dédiée à son rôle.
- **L'API publique `MqttManager::publishXxx()` doit rester non-bloquante**. Toute future méthode ajoutée doit conserver le pattern producteur (enqueue + retour < 1 ms).
- **Cette décision supersède l'alternative écartée d'ADR-0010** : « Tâche FreeRTOS dédiée pour MQTT (rejetée, à reconsidérer) ». La version courte d'ADR-0010 a été annotée en conséquence.

## Références

- Code : [`src/mqtt_manager.h`](../../src/mqtt_manager.h) — déclaration tâche, queues, types `OutboundMsg` / `InboundCmd`
- Code : [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — `mqttTaskFunction`, `taskLoop`, `connectInTask`, `drainOutQueue`, `drainCommandQueue`, `shutdownForRestart`
- Code : [`src/main.cpp`](../../src/main.cpp) — `mqttManager.update()` rendu no-op, ajout `drainCommandQueue()` dans `loop()`
- Code : [`src/web_server.cpp`](../../src/web_server.cpp) — `shutdownForRestart()` avant tous les `ESP.restart()`
- Code : [`src/constants.h`](../../src/constants.h) — `kMqttTaskStackSize`, `kMqttTaskPriority`, `kMqttTaskCore`, `kMqttOutQueueLength`, `kMqttInQueueLength`, `kMqttTaskLoopTimeoutMs`, `kMqttOfflineFlushMs`, `kMqttClientConnectTimeoutSec` (ajouté itération 2)
- Spec : [`specs/features/done/feature-014-mqtt-task-dediee.md`](../../specs/features/done/feature-014-mqtt-task-dediee.md)
- ADR superseded (partiellement) : [ADR-0010](0010-stabilite-mqtt-reseau.md) — alternative « Tâche FreeRTOS dédiée » remplacée par cette décision
- Doc subsystem : [`docs/subsystems/mqtt-manager.md`](../subsystems/mqtt-manager.md) — refonte producer/consumer
- CHANGELOG `[Unreleased]` 2026-04-27 — section Firmware « Tâche MQTT dédiée »
- CHANGELOG `[Unreleased]` 2026-04-29 — section Firmware « Durcissement watchdog mqttTask sur broker injoignable » (itération 2)

## Évolutions

### Itération 2 — 2026-04-29 — Durcissement watchdog sur broker injoignable

Pendant le test manuel D2 (câble Ethernet HA débranché 2–3 min) prévu par la spec feature-014, un PANIC watchdog a été observé sur le core 0. Le coredump confirme que `mqttTask` était bloquée dans `WiFiClient::connect()` → `lwip_select()` lors d'un `mqtt.connect()` sur broker injoignable, > 30 s sans `esp_task_wdt_reset()`. La cause racine identifiée : aucun reset n'était posé **après** le `mqtt.connect()`, et le timeout client TCP n'était en réalité jamais armé (cf. bug latent ci-dessous).

**La décision principale de cet ADR n'est PAS inversée** — la tâche dédiée reste retenue comme structurelle. Cette itération est un durcissement post-test interne à `mqttTask` qui ne change ni l'API publique, ni les topics, ni le contrat externe.

5 fixes appliqués :

1. **Timeout client TCP correct** : `wifiClient.setTimeout(kMqttClientConnectTimeoutSec = 2)` au lieu du `setTimeout(5000)` historique. L'API `WiFiClient::setTimeout()` d'Arduino-ESP32 6.9.0 ([`WiFiClient.cpp:327`](https://github.com/espressif/arduino-esp32/blob/2.0.x/libraries/WiFi/src/WiFiClient.cpp), `_timeout = seconds * 1000`) attend des **secondes** — le code original programmait donc 5000 secondes (~83 minutes), bug latent qui rendait le timeout inopérant.
2. **`esp_task_wdt_reset()` post-`mqtt.connect()`** ajouté pour borner le pire cas connect/CONNACK même quand `connect()` retourne `false` (broker injoignable).
3. **`esp_task_wdt_reset()` granulaire** dans `publishDiscovery()` (helper `publishConfig` reset après chaque publish individuel) et au milieu de `publishAllStatesInternal()` (≤ 5 publish entre 2 resets, contre 15 auparavant).
4. **Suppression du `mqttManager.requestReconnect()` au boot** dans `setup()` ([`main.cpp`](../../src/main.cpp)). `mqttTask` se connecte déjà toute seule au premier tour de `taskLoop()` — élimine la race au boot qui causait un double publish d'auto-discovery (32 messages au lieu de 17).
5. **`connectedAtomic` single source of truth** : store canonique `connectedAtomic.store(mqtt.connected())` posé en début de chaque tour de `taskLoop()`, complété par les transitions explicites en `connectInTask()` (succès) et `disconnect()` (depuis `loopTask`). Suppression de 3 stores intermédiaires redondants qui créaient une fenêtre de divergence UI/WARN observée pendant D2.

**Fichiers touchés** : `src/constants.h`, `src/mqtt_manager.cpp`, `src/main.cpp` (3 fichiers, le diff est minimal — toutes les autres modifications visibles dans `git diff` au moment de l'itération sont préexistantes hors-scope).

**Build** : `pio run` SUCCESS, RAM 16.4 %, flash 97.7 %, 0 nouveau warning.

**Re-test D1/D2** délégué à l'humain après flash OTA pour confirmer l'absence de PANIC pendant 3 min de broker injoignable.

### Itération 3 — 2026-04-29 — Borne TCP write côté lwip + bail-out anticipé

Le re-test D2 par l'humain **après le flash IT2** a déclenché un **nouveau crash watchdog** sur `mqttTask`. Le coredump-5 confirme que le point de blocage a bougé : avant IT2 c'était `mqtt.connect()` (résolu par F1+F2) ; cette fois c'est `mqtt.publish()` :

```
WiFiClient::write (size=33)
  → lwip_select
  → PubSubClient::write
  → PubSubClient::publish (payload="OFF", retained=true)
  → MqttManager::publishAllStatesInternal
  → MqttManager::taskLoop
```

Cause racine : un `mqtt.publish()` (33 octets, payload "OFF" — probablement `filtration_state` ou `lighting_state`) bloque dans `WiFiClient::write` parce que le send buffer TCP est plein, lwip retransmet, la socket reste `ESTABLISHED`. `setTimeout(2)` (F1) borne `SO_SNDTIMEO`/`SO_RCVTIMEO` côté API socket mais ne borne PAS la durée totale d'une retransmission TCP côté pile IP. Le reset par groupes de 5 publish d'IT2 ne suffit pas : un seul publish > 30 s dans le pire cas suffit pour PANIC.

**La décision principale de cet ADR n'est PAS inversée** — la tâche dédiée reste retenue. Cette itération est un durcissement supplémentaire sur la phase publish, sans changement d'API ni de contrat externe.

3 fixes appliqués (F6, F8, F9) :

1. **F6 — `CONFIG_LWIP_TCP_MAXRTX=5` global** : ajout de `-DCONFIG_LWIP_TCP_MAXRTX=5` dans `build_flags` de `platformio.ini` (avec commentaire référence feature-014 IT3 / ADR-0011). Après ~5 retransmissions infructueuses (≈10 s cumulés), lwip abandonne la socket → `mqtt.connected()` retourne `false` → publish suivants court-circuités par F8/F9. Valeur lwip par défaut (12 retries, ~75 s) jugée trop laxiste pour un firmware temps réel.
2. **F8 — Cadence wdt 1:1 + bail-out dans `publishAllStatesInternal()`** : `esp_task_wdt_reset()` posé **avant chaque** `mqtt.publish()` (20 resets, contre 2 resets par groupes en IT2). 5 `if (!mqtt.connected()) return;` répartis tous les ~3-4 publish stoppent net les 14 publish restants dès que lwip a fermé la socket.
3. **F9 — Bail-out fail-fast dans `publishDiscovery()` lambda `publishConfig`** : `if (!mqtt.connected()) { doc.clear(); return; }` en tête du helper. Même logique pour la salve des 17 publish d'auto-discovery.

**F7 et F10 annulés** : le plan IT3 initial prévoyait aussi `setsockopt(TCP_USER_TIMEOUT, 5000)` (option lwip RFC 5482, par-socket) avec une nouvelle constante `kMqttTcpUserTimeoutMs`. Vérification faite : **`TCP_USER_TIMEOUT` n'est pas supporté par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). Le `setsockopt` retournerait `EINVAL` sans effet. F7 et F10 ont donc été abandonnés. Le bornage TCP repose **uniquement sur F6** (paramètre global lwip).

**Trade-off documenté** : `CONFIG_LWIP_TCP_MAXRTX=5` est **global à toute la pile lwip** — il s'applique aussi à AsyncWebServer, à OTA HTTP, et au client NTP. L'impact est :
- AsyncWebServer : abandon plus rapide d'un client web mal connecté (~10 s au lieu de ~75 s). Imperceptible en LAN sain.
- OTA HTTP : un push OTA en réseau dégradé (CPL très lossy, RSSI marginal) **peut être avorté plus tôt**. Acceptable car OTA est supervisé interactivement par l'humain et toujours rejouable.
- NTP : abandon plus rapide, retry applicatif déjà en place, sans impact.

Si une migration future vers ESP-IDF 5.x apporte le support de `TCP_USER_TIMEOUT`, on pourra ré-isoler le bornage à la socket MQTT et restaurer le default lwip global.

**Fichiers touchés** : `platformio.ini`, `src/mqtt_manager.cpp` (2 fichiers).

**Build** : `pio run` SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning imprévu (les 2 warnings `redefined` sur `CONFIG_LWIP_TCP_MAXRTX` et `CONFIG_LWIP_MAX_SOCKETS` sont attendus, conséquence voulue du `-D` override sdkconfig).

**Re-test D2 humain** (3ᵉ tentative) délégué après flash OTA pour confirmer l'absence de PANIC sur la phase publish.

### Itération 4 — 2026-04-29 — Wrapper `safePublish()` + socket non-bloquante

Le 3ᵉ re-test D2 par l'humain **après le flash IT3** a déclenché un nouveau crash watchdog (coredump-6). Le point de blocage a encore migré : avant IT3 c'était `publishAllStatesInternal()` (résolu par F8) ; cette fois c'est `drainOutQueue()`, fonction qui consomme `outQueue` (alertes, status, logs, états relais asynchrones publiés via `publishAlert()` / `publishStatus()` / `publishLog()` / `publishFiltrationState()` etc.). Backtrace `mqttTask` :

```
WiFiClient::write (size=110)
  → lwip_select
  → PubSubClient::publish (payload="{...orp_limit...}", retained=false)
  → MqttManager::drainOutQueue
  → MqttManager::taskLoop
```

**Diagnostic en deux temps** :

1. **Oubli IT3** : `drainOutQueue()` n'avait PAS été instrumenté par F8/F9. Aucun `esp_task_wdt_reset()` granulaire entre deux `mqtt.publish()` consommés depuis `outQueue`, aucun bail-out sur `mqtt.connected()`. Sur send buffer TCP saturé, un seul publish ~110 octets pouvait bloquer > 30 s.
2. **`CONFIG_LWIP_TCP_MAXRTX=5` borne moins fort que prévu** : avec `TCP_RTO_INITIAL=3000 ms` (default lwip) et backoff exponentiel × 2, la séquence des retransmits est T+3 s, T+9 s, T+21 s, T+45 s, **T+93 s** — pas ~10 s comme estimé en IT3. Le pire cas réel est ~93 s avant abandon de socket par lwip, bien au-delà du watchdog 30 s. Le bornage `MAXRTX` seul ne suffit pas à protéger `mqttTask`.

**Découverte technique majeure** : `WiFiClient::availableForWrite()` retourne **toujours 0** dans Arduino-ESP32 6.9.0 — la méthode héritée de `Print::availableForWrite()` n'est pas surchargée par `WiFiClient`, donc renvoie systématiquement la valeur par défaut. Le plan IT4 initial prévoyait un check `availableForWrite()` avant chaque publish (F12 — `kMqttPublishHeadersOverhead`), cette piste a dû être **annulée** au profit d'un pivot architectural.

**La décision principale de cet ADR n'est PAS inversée** — la tâche dédiée reste retenue. Cette itération introduit deux mécanismes complémentaires sur la phase publish, sans changement d'API ni de contrat externe.

**Pivot architectural — socket TCP non-bloquante** :

Après chaque `mqtt.connect()` réussi, dans `connectInTask()`, la socket TCP sous-jacente est passée en mode non-bloquant via :

```cpp
int fd = wifiClient.fd();
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

**Effet** : tout `WiFiClient::write()` ultérieur retourne immédiatement avec `EAGAIN` si le send buffer TCP est plein (au lieu de bloquer dans `lwip_select`). `PubSubClient::publish()` propage cette erreur en retournant `false` (sans retry interne — confirmé `PubSubClient.cpp:599`). C'est ce mécanisme — et non `CONFIG_LWIP_TCP_MAXRTX=5` — qui protège réellement `mqttTask` des blocages > watchdog.

**Wrapper unique `safePublish()`** :

```cpp
bool MqttManager::safePublish(const char* topic, const char* payload, bool retain) {
  esp_task_wdt_reset();              // cadence wdt 1:1 garantie
  if (!mqtt.connected()) return false;  // bail-out fail-fast
  return mqtt.publish(topic, payload, retain);  // jamais bloquant : socket O_NONBLOCK
}
```

Tous les `mqtt.publish()` directs dans `mqttTask` ont été remplacés par `safePublish()` — **24 call sites** : `connectInTask()` (status `online`), `drainOutQueue()`, `publishAllStatesInternal()` (20 publishes d'états), `publishDiagnosticInternal()`, lambda `publishConfig` de `publishDiscovery()` (17 publishes auto-discovery HA).

**Fixes appliqués (F11, F13, F14, F15, F16) + pivot non-bloquant** :

- **F11** — `src/mqtt_manager.h` : déclaration privée `bool safePublish(const char*, const char*, bool)`.
- **Pivot non-bloquant** — `src/mqtt_manager.cpp` `connectInTask()` : `fcntl(fd, F_SETFL, O_NONBLOCK)` après chaque connect réussi (avant subscribe). Includes `<fcntl.h>` et `<errno.h>`.
- **F13** — `src/mqtt_manager.cpp` : implémentation `safePublish()` (wdt reset + check connected + publish).
- **F14** — `src/mqtt_manager.cpp` `drainOutQueue()` : `mqtt.publish()` direct → `safePublish()`. **Fix l'oubli IT3.**
- **F15** — `src/mqtt_manager.cpp` `publishAllStatesInternal()` : 20 `mqtt.publish()` → `safePublish()`. **Suppression** des 20 `esp_task_wdt_reset()` IT3 manuels (factorisés dans le wrapper) et des 5 bail-out `if (!mqtt.connected()) return;` IT3 (le wrapper retourne `false`).
- **F16** — `src/mqtt_manager.cpp` lambda `publishConfig` : `mqtt.publish()` → `safePublish()`. **Suppression** du bail-out IT3 redondant.

**F12 ANNULÉ** : la constante `kMqttPublishHeadersOverhead = 16` envisagée pour borner via `availableForWrite()` est **inutile** avec le pivot non-bloquant. `availableForWrite()` retournant toujours 0 dans Arduino-ESP32 6.9.0, le check aurait court-circuité 100 % des publishes. Le mode non-bloquant via `fcntl()` couvre le besoin sans constante supplémentaire.

**Trade-off documenté** : drop silencieux des publish quand le send buffer TCP est plein. Pas de retry, pas de reput dans `outQueue`. Acceptable parce que :
- Les **états retain** sont republiés au prochain `publishAllStatesInternal()` post-reconnect (cadence 10 s).
- Les **alertes ponctuelles** (`publishAlert`) **peuvent être perdues** — c'était déjà le cas avec `PubSubClient` en bloquant qui timeoutait sur send buffer plein avant ADR-0011. IT4 ne dégrade pas cette propriété, il rend le drop instantané et silencieux au lieu de bloquer 30 s puis dropper.
- L'auto-discovery HA est republiée à chaque reconnect (`discoveryPublished` reset à la déconnexion).

**Bénéfice annexe** : le code devient drastiquement plus lisible — ~50 lignes de `esp_task_wdt_reset()` et `if (!mqtt.connected()) return;` éparpillés ont été supprimées au profit d'un appel unique au wrapper.

**Fichiers touchés** : `src/mqtt_manager.h`, `src/mqtt_manager.cpp` (2 fichiers).

**Build** : `pio run` SUCCESS, RAM 16.4 %, Flash 97.8 % (légère baisse vs IT3 grâce aux ~50 lignes supprimées), 0 nouveau warning.

**Re-test D2 humain** (4ᵉ tentative) délégué après flash OTA pour confirmer l'absence de PANIC pendant 3 min de broker injoignable, cette fois sur la phase `drainOutQueue()`.
