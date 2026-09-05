## Métadonnées
- **ID** : feature-014
- **Slug** : mqtt-task-dediee
- **Type** : feature
- **Statut** : done (Itération 5 livrée — 2026-04-30 — build vert, revue OK, tests dynamiques AC-IT5-3 / AC-IT5-4 délégués à l'humain post-flash)
- **Priorité** : critique
- **Version cible** : 1.0.5 (fiabilisation keepalive — incrément CORRECTIF)
- **Dernière mise à jour** : 2026-04-30 (Itération 5 livrée — `O_NONBLOCK` IT4 remplacé par `SO_SNDTIMEO=500 ms`, le PINGREQ keepalive PubSubClient redevient fiable, Mosquitto ne devrait plus couper sur `exceeded timeout`)
- **Spec(s) liée(s)** : ADR-0010 (Stabilité MQTT et résilience réseau) — alternative écartée reprise et retenue par ADR-0011 ; ADR-0011 section « Évolutions » → Itérations 2, 3, 4

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (effet indirect : moins de contention mutex pendant publish)
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non applicable (pas de dosage, pas de calibration)
- [ ] ux-ui-designer — non applicable (pas de surface UI)
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Déplacer toute la logique MQTT (`MqttManager::update()`, `connect()`, `publish*()`, `messageCallback()`) dans une tâche FreeRTOS dédiée, distincte de la loop principale (`loopTask`). Aucun changement de comportement observable côté utilisateur — la tâche dédiée publie/souscrit aux mêmes topics avec les mêmes payloads. L'objectif est purement architectural : **isoler les blocages réseau MQTT (CPL lossy, broker silencieux, TCP retransmits) de la régulation pH/ORP, de la filtration et du watchdog principal**.

## Contexte

ADR-0010 a documenté l'alternative « tâche FreeRTOS dédiée pour MQTT » et l'a écartée à l'époque, jugeant que les correctifs synchrones (`setSocketTimeout(2)`, pré-résolution DNS, court-circuit IP, suppression du `requestReconnect()` périodique, keepalive 60s) suffisaient en pratique.

**Symptômes observés depuis qui justifient de revenir sur cette décision :**

1. **Blocage de la loop principale pendant les publications MQTT** : `publishAllStates()` enchaîne ~15 `mqtt.publish()` synchrones. En cas de TCP lossy (CPL bruyant), chaque publication peut bloquer jusqu'à `setSocketTimeout * 1000 ms` (2s). Cumul théorique 30s, suffisant pour déclencher le watchdog.
2. **`AbortError: Fetch is aborted` côté UI** : le frontend a observé des timeouts 10s sur `GET /data` parce que le handler async devait prendre un mutex de configuration tenu par la loop principale en train de publier MQTT.
3. **Régulation pH/ORP gelée** : pendant la même fenêtre, `PumpController::update()` n'est pas appelé → pas de check des limites journalières / horaires, pas d'ajustement PID.
4. **Risque PANIC watchdog 30s** déjà observé en session lorsqu'une résolution DNS interne lwip combinée à la contention bloquait la loop > 30s.

Le keepalive 60s mitige les déconnexions, pas les blocages individuels. La tâche dédiée est la seule solution structurelle.

## Comportement attendu

### Côté utilisateur final
**Aucun changement observable** :
- Mêmes topics MQTT publiés (sensors, status, alerts, diagnostic, discovery HA)
- Mêmes payloads (JSON identique)
- Mêmes intervalles (publishAllStates toutes les `kMqttPublishIntervalMs`, diagnostic toutes les `kDiagnosticPublishIntervalMs`)
- Même latence perçue (≤ quelques secondes entre événement et publish)
- Même comportement de reconnexion automatique (rate-limit, backoff, court-circuit IP)

### Côté firmware
- Une nouvelle tâche FreeRTOS `mqttTask` créée au démarrage par `MqttManager::begin()`, pinnée sur le **core 0** (différent du `loopTask` sur core 1) pour répartir la charge réseau
- Stack size dimensionné pour supporter `mqtt.connect()` + handshake CONNACK + 1024 octets de buffer + JSON sérialisation (~ 6-8 KB)
- Priorité FreeRTOS basse (1 ou 2), inférieure à `tiT` (lwip) et `async_tcp` mais supérieure à `IDLE`
- Communication entre `loopTask` et `mqttTask` via :
  - **Une queue FreeRTOS** pour les publications sortantes (snapshot des données à publier, format compact)
  - **Un mutex existant ou nouveau** pour les états partagés que `mqttTask` doit lire (sensors, config, dosing) — exposition claire et minimale
  - **Une variable atomique** pour `mqtt.connected()` exposé à `loopTask` (lecture seule pour les `isConnected()` callers)
- `MqttManager::publish*()` appelées depuis `loopTask` deviennent des **producteurs sur la queue** (non-bloquants, retournent immédiatement après enqueue ou drop si queue pleine)
- La consommation de la queue, le `mqtt.loop()`, la reconnexion, et les retries TCP se passent **uniquement dans `mqttTask`**
- `messageCallback()` (souscriptions HA → ESP32) doit poster ses commandes vers `loopTask` via une queue d'entrée pour éviter d'agir directement sur les actuateurs depuis `mqttTask`

### Cas limites
- **Queue pleine** (publish plus rapide que ce que la tâche peut écouler) : drop des messages les plus anciens (semantique "best effort"), log `WARN` edge-triggered
- **Watchdog** : `mqttTask` doit appeler `esp_task_wdt_add(NULL)` au démarrage et `esp_task_wdt_reset()` à chaque itération avant de bloquer dans la queue
- **Arrêt propre OTA** : avant un redémarrage post-OTA, `loopTask` doit demander à `mqttTask` de publier un `status=offline` synchrone (avec timeout court ~1s) puis terminer la tâche proprement
- **`MqttManager::update()` appelée depuis `loopTask`** (legacy code path) : conserver la signature mais la rendre **no-op** (la tâche fait tout) ; éviter les appels redondants
- **Réception MQTT depuis HA** (commande `filtration/set`, `lighting/set`, `ph_target/set`, `orp_target/set`) : la queue d'entrée doit être consommée par `loopTask` à chaque tour pour respecter les contraintes thread-safety des modules `filtration`, `lighting`, `pump_controller`
- **Heap fragmentation** : la nouvelle tâche alloue son stack une fois ; aucune allocation dynamique récurrente attendue

## Comportement actuel

`MqttManager::update()` est appelée depuis `loop()` dans `main.cpp` ligne 157. Cette méthode :
1. Vérifie `mqtt.connected()`
2. Si déconnecté : appelle `connect()` (qui peut bloquer jusqu'à 2s pour le TCP + 5s pour le DNS hostname)
3. Si connecté : appelle `mqtt.loop()` (lit/écrit sur la socket TCP, peut bloquer si la socket est saturée)

`publishAllStates()` est appelée depuis `loop()` toutes les `kMqttPublishIntervalMs`. Elle enchaîne ~15 `mqtt.publish()` synchrones.

`messageCallback()` (souscriptions HA) est appelé depuis `mqtt.loop()` (donc depuis `loopTask`), et agit directement sur les actuateurs (filtration, lighting, pump_controller) — déjà thread-safe via mutex existants.

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB — non concerné (pas d'ajout frontend)
- AsyncWebServer : aucun handler bloquant > 50 ms (cette feature DEVRAIT améliorer cette contrainte en éliminant la contention mutex)
- WebSocket pour toute donnée temps réel ; HTTP authentifié pour les actions
- Watchdog 30s respecté par toutes les tâches subscribers
- Pas d'allocation dynamique récurrente dans `mqttTask` (snapshots passés par valeur ou pointeur dans la queue)
- Compatible avec OTA en cours (la nouvelle tâche doit pouvoir être terminée proprement avant `ESP.restart()`)
- Heap budget : la nouvelle tâche consomme ~6-8 KB de stack ; vérifier `ESP.getFreeHeap()` après ajout
- Compatible avec le coredump (la nouvelle tâche apparaîtra dans le dump avec son nom)

## Fichiers potentiellement concernés
- `src/mqtt_manager.h` — déclaration de la tâche, queue, signatures publiques inchangées (publishXxx() deviennent producteurs)
- `src/mqtt_manager.cpp` — refonte interne : tâche FreeRTOS, queue producteur/consommateur, dispatch des commandes entrantes
- `src/main.cpp` — `mqttManager.update()` retiré ou rendu no-op
- `src/constants.h` — nouvelles constantes : `kMqttTaskStackSize`, `kMqttTaskPriority`, `kMqttTaskCore`, `kMqttQueueLength`
- `src/web_routes_data.cpp`, `src/web_routes_control.cpp` — vérifier les call sites de `mqttManager.publish*()` (toujours OK car les méthodes deviennent non-bloquantes)
- `docs/subsystems/mqtt-manager.md` — refonte majeure de la section architecture
- `docs/adr/NNNN-mqtt-task-dediee.md` — nouvel ADR (revient sur l'alternative écartée d'ADR-0010)

## Livrables — check-list
- [x] A1 — Queue FreeRTOS sortante créée (`outQueue`, 32 entrées × `OutboundMsg{topic[64], payload[128], retain}` ≈ 6.3 KB)
- [x] A2 — Queue FreeRTOS entrante créée (`inQueue`, 16 entrées × `InboundCmd{type, payload[64]}` ≈ 1 KB)
- [x] A3 — Tâche `mqttTask` créée dans `MqttManager::begin()` via `xTaskCreatePinnedToCore` (stack `kMqttTaskStackSize=8192`, prio `kMqttTaskPriority=2`, core `kMqttTaskCore=0`)
- [x] A4 — Boucle `taskLoop()` : reset wdt → reconnect/connect → mqtt.loop() → flags atomiques (publishStates/Diag) → drainOutQueue() → xQueuePeek timeout 100 ms
- [x] A5 — Connect/reconnect/backoff entièrement dans `connectInTask()` (rate-limit 5 s, backoff 5→120 s, court-circuit IP, pré-DNS lwip)
- [x] A6 — `publishXxx()` = producteurs non-bloquants via `enqueueOutbound()` (xQueueSend timeout 0, drop oldest si plein, WARN edge-triggered fenêtre 5 s)
- [x] A7 — `messageCallback()` enfile uniquement `InboundCmd` dans `inQueue`, jamais d'action directe sur les actuateurs depuis `mqttTask`
- [x] A8 — `mqttManager.drainCommandQueue()` ajouté dans `loop()` après `mqttManager.update()` ; pop jusqu'à 8 cmds/tour, applique sous `configMutex`
- [x] A9 — `mqttTask` lit `productCfg`/`mqttCfg` sous `configMutex` (existant), capteurs via getters atomiques (`sensors.getPh()` etc.) ; pas de copie locale en RAM
- [x] A10 — `mqttManager.update()` rendu no-op (commentaire explicatif renvoyant à ADR-0011) ; conservé pour préserver les call sites historiques
- [x] A11 — Watchdog : `esp_task_wdt_add(NULL)` au démarrage de `mqttTaskFunction`, `esp_task_wdt_reset()` en début de `taskLoop()` + avant `mqtt.connect()` + entre les blocs `publishDiscovery/States/Diagnostic`
- [x] A12 — `shutdownForRestart()` créée : enqueue `status=offline` + flush 1 s + `taskShouldStop=true` ; appelée depuis les 4 sites de `ESP.restart()` (web_server.cpp ×2, web_routes_config.cpp, main.cpp factory reset)
- [ ] D1 — Test manuel sur matériel réel (déconnexion broker 60 s) — **délégué à l'humain** (nécessite broker Mosquitto sur LAN). À retester depuis IT4 : timeline attendue ~60 s pour `-4` ou quasi-immédiat pour `-3` selon mode d'arrêt
- [x] D2 — **VALIDÉ 2026-04-29 (4ᵉ tentative humaine, IT4)** — câble Ethernet HA débranché, `WARN: MQTT déconnecté détecté — état=-4` à 14:12:43 (~60 s après début drops à 14:11:53), reconnexion auto à 14:15:13. Aucun crash watchdog
- [ ] D3 — Test manuel rafale 50 cmds HA — **délégué à l'humain** (nécessite Home Assistant)
- [x] D4 — `pio run --target size` exécuté : RAM 16.4 % (53896/327680 octets), **identique** au baseline avant feature ; +~16 KB heap runtime (queues + stack mqttTask) — sous le critère AC10 (< 18 %)
- [x] C1 — `docs/subsystems/mqtt-manager.md` refondu : section « Architecture producer/consumer » avec diagramme, règles d'or, paramètres `mqttTask`, refonte gestion commandes HA, troubleshooting drops `outQueue`
- [x] C2 — `docs/adr/0011-mqtt-task-dediee.md` créé (critères ADR 1+3+4 déclenchés)
- [x] C3 — `docs/adr/0010-stabilite-mqtt-reseau.md` mis à jour : note de mise à jour en préambule + biffage `(rejetée, à reconsidérer)` → `(retenue par ADR-0011, Superseded for MQTT decoupling)`
- [x] C4 — `CHANGELOG.md` `[Unreleased]` 2026-04-27 : entrée Firmware « MQTT déplacé dans une tâche FreeRTOS dédiée » + entrée « Arrêt propre OTA » ; entrées Documentation pour ADR-0011, MAJ ADR-0010, refonte mqtt-manager.md, note UPDATE_GUIDE.md
- [x] C5 — `docs/UPDATE_GUIDE.md` : note user-facing « Stabilité réseau MQTT — tâche dédiée — depuis 2026-04-27 » dans la section Notes de migration

## Nouvelles signatures / API
- Endpoints HTTP : aucun nouveau (`mqttManager.publishXxx()` reste appelée mais devient non-bloquante)
- Champs WebSocket : aucun nouveau
- Topics MQTT : aucun nouveau (mêmes topics, mêmes payloads)
- Fonctions publiques nouvelles dans `src/mqtt_manager.h` :
  - aucune nouvelle signature publique idéalement — l'API externe reste identique pour minimiser l'impact sur les call sites
  - méthodes internes privées : `static void mqttTaskFunction(void* pvParameters);`, `void processOutgoingQueue();`, `void processIncomingQueue();` (consommé par `loopTask`)
- Constantes nouvelles dans `src/constants.h` :
  - `constexpr uint32_t kMqttTaskStackSize = 8192;`
  - `constexpr UBaseType_t kMqttTaskPriority = 2;`
  - `constexpr BaseType_t kMqttTaskCore = 0;`
  - `constexpr UBaseType_t kMqttOutQueueLength = 32;`
  - `constexpr UBaseType_t kMqttInQueueLength = 16;`

## Sécurité chimique
- Dosage impliqué : non
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : aucun direct, mais **bénéfice indirect** : la régulation pH/ORP n'est plus gelée pendant les blocages MQTT, donc les checks de limites de sécurité (`pump_controller.cpp`) ne sont plus suspendus

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte structurante : nouvelle tâche FreeRTOS, nouveau pattern producer/consumer pour MQTT) + 3 (alternatives crédibles déjà documentées dans ADR-0010 — il faut tracer le revirement) + 4 (verrouillage long terme : tout futur module qui voudra publier MQTT devra passer par la queue, plus le pattern synchrone direct)
- **Sujet pressenti** : `0011-mqtt-task-dediee.md` (revient sur l'alternative écartée d'ADR-0010 et la documente comme retenue)

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun nouveau (mêmes topics existants)

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/subsystems/mqtt-manager.md` — refonte majeure : architecture producer/consumer, queues, tâche dédiée, points de synchronisation
- `docs/subsystems/<composant>.md` — aucun autre composant touché (pump_controller, sensors, filtration restent inchangés côté firmware ; juste plus stables grâce à l'isolation MQTT)

#### Docs transverses
- `docs/API.md` — aucun (pas de nouveau endpoint)
- `docs/MQTT.md` — aucun changement de contrat externe
- `docs/BUILD.md` — aucun
- `docs/UPDATE_GUIDE.md` — note user-facing : « Stabilité réseau améliorée — la régulation continue de tourner même en cas de microcoupures broker MQTT »
- `README.md` — aucun
- `CHANGELOG.md` — entrée Firmware sous la version cible : « MQTT déplacé dans une tâche FreeRTOS dédiée pour isoler les blocages réseau de la régulation et du watchdog »

#### Docs immuables
- `docs/adr/0011-mqtt-task-dediee.md` — à créer
- `docs/adr/0007-table-partitions-custom.md` — non concerné
- `docs/adr/0010-stabilite-mqtt-reseau.md` — ajouter une note de mise à jour : la décision « tâche dédiée écartée » de l'époque est maintenant supersédée par ADR-0011

## Critères d'acceptation
- [x] AC1 — Vérifié par construction : `mqtt.publish()`, `mqtt.connect()`, `mqtt.loop()` ne s'exécutent plus jamais depuis `loopTask`. `PumpController::update()` est appelé inconditionnellement à chaque tour de `loop()` indépendamment de l'état MQTT. Test dynamique D1 délégué à l'humain
- [x] AC2 — Vérifié par construction : aucune route HTTP n'appelle MQTT. `configMutex` n'est plus pris simultanément à un blocage réseau. Test dynamique délégué à l'humain
- [x] AC3 — Vérifié par revue de code : `enqueueOutbound()` = 2 strlcpy + 1 xQueueSend timeout 0. `publishAllStates/Diagnostic` = 1 atomic store. < 50 µs typique, < 5 ms worst-case (queue pleine = +1 xQueueReceive timeout 0)
- [x] AC4 — Vérifié : log `mqttTask démarrée (core=X prio=Y)` ajouté dans `mqttTaskFunction` au start, et nom "mqttTask" passé à `xTaskCreatePinnedToCore`
- [x] AC5 — Vérifié par construction : nom de tâche enregistré dans le TCB FreeRTOS, automatiquement présent dans `esp_core_dump_*`. Test dynamique délégué à l'humain
- [ ] AC6 — Test manuel rafale 50 cmds HA — délégué à l'humain. Capacité `inQueue=16` dimensionnée pour 16 cmds simultanées + drainage 8 cmds/tour de loop. WARN explicite si saturation
- [x] AC7 — Vérifié par revue : `mqttTask` reset wdt en début de `taskLoop()`, avant `mqtt.connect()`, et entre les blocs longs (`publishDiscovery`/`AllStates`/`Diagnostic`). `loopTask` reset wdt inchangé en début de `loop()` et plus aucun bloc MQTT bloquant
- [x] AC8 — `pio run` SUCCESS, **0 nouveau warning** (les 4 warnings `trigraph` dans `logger.cpp:43,171` sont préexistants hors-scope)
- [x] AC9 — `./build_fs.sh` non impacté (aucune modif `data/`)
- [x] AC10 — `pio run --target size` : **RAM 16.4 %** (53896 / 327680 octets), identique au baseline. Sous le seuil 18 %. +~16 KB heap runtime (mesurable via `mqtt_task_stack_hwm` exposé dans payload diagnostic)
- [x] Tous les libellés utilisateur en français (non concerné, pas de surface UI)
- [ ] Comportement nominal testé manuellement — **délégué à l'humain** (matériel réel + Mosquitto + HA)
- [x] ADR-0011 publié, ADR-0010 mis à jour avec note + Superseded for MQTT decoupling sur l'alternative écartée

## Plan de test manuel

> **Tous les tests sont exécutables sans connexion USB**, depuis l'app web : Paramètres → Avancé → card **Logs** avec **Auto (5s)** activé.

### Préparation
1. Flasher : `./deploy.sh ota-firmware` (ou USB si OTA indisponible)
2. Une fois rebooté, ouvrir l'app web → Paramètres → Avancé → card **Logs** → activer **Auto (5s)**
3. Télécharger les logs (bouton "Télécharger") et vérifier la présence de :
   ```
   INFO: mqttTask démarrée (core=0 prio=2)
   INFO: Gestionnaire MQTT initialisé (mqttTask core=0 prio=2 stack=8192)
   ```
   → confirme que la nouvelle architecture est active (**AC4** validé)

### AC4 — `mqttTask` démarrée au boot
**Procédure** : étape 3 de la préparation ci-dessus.
**Succès** : les deux lignes `INFO: mqttTask démarrée` et `Gestionnaire MQTT initialisé (mqttTask core=...)` apparaissent dans le log de boot.

### D1 — Broker arrêté 60 secondes
**Objectif** : la régulation continue de tourner pendant que le broker est inaccessible.
1. Côté HA : **Settings → Add-ons → Mosquitto broker → Stop** (ou `ha addons stop core_mosquitto` dans le terminal)
2. Démarrer un chrono **2–3 minutes** (la détection prend désormais ~60 s en mode non-bloquant IT4 ; le délai 2–3 min couvre les cas tordus où lwip abandonne avant le keepalive)
3. Pendant la coupure, observer dans l'UI logs ESP32 :
   - Logs continuent d'apparaître (`Consolidation`, capteurs, `tickDailyRollover` si minuit, etc.)
   - **Aucun** `--- DÉMARRAGE ---` (sinon = PANIC watchdog → échec)
   - **~60–90 s** après l'arrêt : apparition de `WARN: MQTT déconnecté détecté — état=-4` (`MQTT_CONNECTION_TIMEOUT`, keepalive PubSubClient). Variante minoritaire `état=-3` (`MQTT_CONNECTION_LOST`) possible si l'add-on s'est fermé proprement et que lwip a reçu un `FIN`/`RST` (timing dépendant)
   - L'UI reste fluide (pas d'`AbortError` dans la console navigateur)
4. **Relancer Mosquitto** : Add-ons → Mosquitto broker → Start
5. Vérifier reconnexion automatique : `INFO: Tentative connexion MQTT...` puis `INFO: MQTT connecté !`

**Succès** : aucun crash, UI fluide pendant toute la coupure, reconnexion automatique au retour.

> **Note sur le délai et l'état observés (post-IT4)** — Avec le mode non-bloquant introduit en IT4 (`fcntl(F_SETFL, O_NONBLOCK)` sur la socket TCP), `mqtt.loop()` continue à tourner même quand le send buffer TCP est plein (les writes retournent `EAGAIN` immédiatement au lieu de bloquer). Le keepalive applicatif PubSubClient (60 s sans `PINGRESP`) devient donc le mécanisme dominant : après ~60 s `_client->stop()` est appelé en interne et `_state = MQTT_CONNECTION_TIMEOUT` (`-4`). Au tour suivant de `taskLoop()`, `mqtt.connected()` retourne `false` et le WARN est émis avec `état=-4`.
>
> Avant IT4 (mode bloquant), le keepalive ne s'exécutait pas correctement (les publishes restaient bloqués dans `lwip_select`), c'était l'abandon TCP lwip (90–180 s) qui détectait via un `connected() == false` figeant l'état sur `-3`. Cette dominance est **inversée depuis IT4**.
>
> Sur une fermeture **propre** TCP (broker `SIGTERM` qui envoie un `FIN`/`RST`), la détection est quasi-immédiate et l'état observé est `-3` (la socket est invalidée par la pile TCP avant que le keepalive applicatif n'ait le temps d'agir).

### D2 — Coupure réseau côté HA (variante CPL — Option 2)
**Note** : impossible de débrancher la prise CPL côté ESP32 (ça couperait l'UI). On simule donc la coupure de chemin réseau côté serveur HA, équivalent du point de vue de l'ESP32.

**Statut** : **VALIDÉ en IT4 — 4ᵉ tentative humaine, 2026-04-29 14:12:43** — aucun crash watchdog, `mqttTask` ne bloque plus, `WARN: MQTT déconnecté détecté — état=-4` détecté à T+60 s après début des drops (logs fournis dans l'analyse IT4).

1. Identifier la connexion réseau du serveur Home Assistant (Ethernet ou Wi-Fi)
2. **Débrancher le câble Ethernet** du serveur HA (ou couper son Wi-Fi si HA est en Wi-Fi)
3. Démarrer un chrono **2–3 minutes**
4. Pendant la coupure, observer dans l'UI logs ESP32 :
   - Logs continuent d'apparaître régulièrement
   - **Aucun** `--- DÉMARRAGE ---`
   - L'UI reste responsive (charge des pages, navigation, refresh des capteurs WS)
   - Apparition de `WARN: MQTT déconnecté détecté — état=-4` après **~60–90 s** (keepalive PubSubClient timeout — mécanisme dominant depuis le mode non-bloquant IT4). Variante minoritaire `état=-3` possible si l'AP redémarre proprement et envoie un `FIN`/`RST`. Puis tentatives `code=-2` avec backoff 5→10→20→40→80→120 s
   - Drops `MQTT publish drop: <topic>` éventuels pendant la fenêtre de coupure (mode non-bloquant : `safePublish()` retourne `false` silencieusement quand la socket TCP a son send buffer plein)
5. **Rebrancher le câble** (ou rallumer le Wi-Fi HA)
6. Vérifier dans les logs : reconnexion MQTT automatique en quelques secondes

**Succès** : aucun crash, UI ESP32 reste fluide pendant la coupure totale du chemin vers le broker.

**Logs observés en validation 2026-04-29 (extrait, IT4)** :
```
14:11:43 DEBUG : Diagnostic publié          ← connexion OK, coupure Ethernet juste après
14:11:53 DEBUG : MQTT publish drop: pool/sensors/alerts   ← IT4 drop silencieux fonctionne
14:12:43 WARN  : MQTT déconnecté détecté — état=-4        ← ~60s après début drops = keepalive PubSubClient
14:12:43 INFO  : Tentative connexion MQTT (délai=5s)...
... backoff 5→10→20→40→80s ...
14:15:13 INFO  : MQTT connecté !            ← reconnexion automatique au retour Ethernet
```

### D3 / AC6 — Rafale de commandes HA
**Objectif** : aucune commande perdue lors d'un afflux brutal.
1. Côté HA, créer une automation qui envoie 50 commandes en 5 secondes :
   ```yaml
   service: switch.toggle
   target:
     entity_id: switch.poolcontroller_filtration_switch
   ```
   Avec `repeat: count: 50` et `delay: '00:00:00.1'` entre chaque.
2. Pendant l'envoi, observer les logs ESP32 :
   - Chaque commande reçue → log `INFO: Filtration ...` côté firmware
   - **Aucun** `WARN: MQTT inQueue saturée` (= drop par débordement queue)
3. Compter à la fin combien de commandes ont été appliquées (état final cohérent avec parité de la séquence ON/OFF).

**Succès** : toutes les commandes traitées, état final correct, aucun warning de saturation queue.

### AC5 — `mqttTask` dans le coredump (bonus)
**Pas besoin de provoquer un crash réel**.
1. Si un coredump existe déjà : Paramètres → Avancé → card **Diagnostic crash** → Télécharger
2. Décoder localement : `./tools/decode_coredump.sh coredump.bin`
3. Section `THREADS INFO` : vérifier la ligne `0x3ffXXXXX  mqttTask  2/2  ...`

**Succès** : `mqttTask` apparaît dans la liste des threads.

### OTA — Arrêt propre pendant trafic MQTT
**Objectif** : `shutdownForRestart()` est invoqué et publie `status=offline` avant le redémarrage.
1. Lancer un OTA firmware ou filesystem depuis l'app web
2. Pendant le restart, observer côté HA / Mosquitto les logs ou le topic `pool/sensors/status`
3. Vérifier que la valeur passe à `offline` avant que le client se déconnecte côté broker

**Succès** : `status=offline` reçu côté broker juste avant le restart.

### Mémoire (vérification optionnelle)
1. Avant : noter `ESP.getFreeHeap()` (visible dans le diagnostic MQTT publié toutes les 5 min, ou via la card Infos système)
2. Après plusieurs heures de fonctionnement avec la nouvelle archi : vérifier l'écart < 10 KB

**Succès** : pas de fuite mémoire détectable.

## Notes pour agent
- **Synchronisation est le piège n°1** : `mqttTask` doit lire les états partagés via les mutex existants (`configMutex`, `i2cMutex`, etc.). Ne pas dupliquer les structures.
- **Ne pas appeler de méthode `Async*` depuis `mqttTask`** : la tâche `async_tcp` doit rester libre.
- **`mqttTask` ne doit jamais appeler `applyPumpDuty()`, `filtration.start()`, etc.** directement — ces actions doivent passer par la queue d'entrée, traitée par `loopTask`.
- **Stack size 8192** est une estimation prudente : `mqtt.connect()` fait des allocations stack pour le buffer JSON et la handshake CONNACK. À mesurer avec `uxTaskGetStackHighWaterMark()` après quelques heures de fonctionnement.
- **Compatibilité ADR-0010** : la pré-résolution DNS, le court-circuit IP, le `setSocketTimeout(2)`, le keepalive 60s restent dans la nouvelle tâche.
- Avant d'invoquer `embedded-developer`, le `architect` doit produire un schéma producer/consumer clair avec la liste exhaustive des call sites de `mqttManager.publish*()` à auditer.

## Hors périmètre
- Pas de changement de protocole MQTT (toujours QoS 0, retain selon topic)
- Pas de modification des topics, payloads, ou de l'auto-discovery HA
- Pas de tâche dédiée pour AsyncWebServer (déjà géré par la lib)
- Pas de migration vers un client MQTT plus moderne (esp-mqtt, async-mqtt-client) — on reste sur PubSubClient pour minimiser le scope
- Pas de gestion de plusieurs brokers (un seul broker)
- Pas de TLS (hors scope, à traiter dans une autre spec si besoin)

---

## Suivi / itérations

### Itération 1 — 2026-04-27 — Implémentation initiale

**Statut** : `done`

**Contexte** : 3ᵉ crash production confirmant que `setSocketTimeout(2)` (ADR-0010) ne couvre pas `lwip_write` sur réseau lossy. Coredump : `loopTask → publishProductState → WiFiClient::write(33 octets)` bloqué dans `lwip_select` > 30 s → PANIC watchdog. Priorité élevée de `haute` à `critique` au début du cycle.

**Travail réalisé** :

- **Architecture producer/consumer** : tâche FreeRTOS `mqttTask` (core 0, prio 2, stack 8 KB) + 2 queues (`outQueue` 32×196 octets, `inQueue` 16×65 octets).
- **API publique préservée** : tous les call sites externes (`main.cpp`, `filtration.cpp`, `lighting.cpp`, web routes) inchangés. Les méthodes `publishXxx()` deviennent producteurs non-bloquants.
- **Optimisation `publishAllStates`/`publishDiagnostic`** : flag atomique posé depuis `loopTask`, snapshot + sérialisation + publish dans `mqttTask` sous `configMutex`. Évite que `loopTask` prenne tous les mutex de capteurs pendant la publication des ~15 états.
- **Compatibilité ADR-0010 totale** : pré-résolution DNS, court-circuit IP, `setSocketTimeout(2)`, keepalive 60 s, backoff 5→120 s — tous préservés intégralement, mais s'exécutant désormais dans `mqttTask`.
- **Arrêt propre OTA** : `shutdownForRestart()` instrumenté sur les 4 sites de `ESP.restart()` (web_server.cpp ×2, web_routes_config.cpp, main.cpp factory reset).
- **Watchdog** : `mqttTask` enregistré via `esp_task_wdt_add(NULL)`, reset à chaque itération + avant chaque opération longue (connect, publishDiscovery, publishAllStates, publishDiagnostic).

**Fichiers modifiés** :

- `src/constants.h` — +7 constantes (`kMqttTaskStackSize=8192`, `kMqttTaskPriority=2`, `kMqttTaskCore=0`, `kMqttOutQueueLength=32`, `kMqttInQueueLength=16`, `kMqttTaskLoopTimeoutMs=100`, `kMqttOfflineFlushMs=1000`).
- `src/mqtt_manager.h` — déclaration tâche/queues, types `OutboundMsg`/`InboundCmd`, `shutdownForRestart()`, `drainCommandQueue()`.
- `src/mqtt_manager.cpp` — refonte interne complète (~750 lignes). API publique inchangée.
- `src/main.cpp` — ajout `mqttManager.drainCommandQueue()` dans `loop()` ; `mqttManager.shutdownForRestart()` avant le `ESP.restart()` du factory reset bouton.
- `src/web_server.cpp` — `#include "mqtt_manager.h"` + `mqttManager.shutdownForRestart()` avant les 2 sites de `ESP.restart()` (OTA, mode AP).
- `src/web_routes_config.cpp` — `mqttManager.shutdownForRestart()` avant le `ESP.restart()` du toggle disableApOnBoot.
- `docs/adr/0011-mqtt-task-dediee.md` — créé (ADR critères 1+3+4 déclenchés).
- `docs/adr/0010-stabilite-mqtt-reseau.md` — note de mise à jour + biffage de l'alternative écartée.
- `docs/subsystems/mqtt-manager.md` — refonte majeure (architecture producer/consumer, règles d'or, troubleshooting).
- `docs/UPDATE_GUIDE.md` — note user-facing « Stabilité réseau MQTT — tâche dédiée ».
- `CHANGELOG.md` — entrée Firmware « MQTT déplacé dans une tâche FreeRTOS dédiée » + entrée Documentation pour ADR-0011.

**Build** :

- `pio run` SUCCESS, **0 nouveau warning**.
- RAM static : **16.4 %** (53896 / 327680 octets), identique au baseline.
- Flash : 97.7 % (inchangé).
- `./build_fs.sh` non impacté.

**Tests dynamiques (D1, D2, D3, AC6, comportement nominal) délégués à l'humain** : nécessitent matériel réel + broker Mosquitto + Home Assistant + installation CPL pour reproduction. Tous les autres critères AC1–AC5, AC7–AC10, AC11 sont validés par construction et revue de code.

**Notes pour la prochaine itération éventuelle** :

- Surveiller `mqtt_task_stack_hwm` (champ ajouté au payload diagnostic) sur plusieurs jours. Si HWM stable > 4000, on peut réduire `kMqttTaskStackSize` à 6144 voire 4096 pour libérer ~4 KB de heap.
- Si les drops `outQueue` apparaissent en log régulier, considérer (a) augmenter `kMqttOutQueueLength` à 64, (b) augmenter `kMaxPublishPerIter` à 16.
- Si une commande HA est régulièrement perdue (`MQTT inQueue saturée`), augmenter `kMqttInQueueLength` à 32.
- Migrer éventuellement vers `esp-mqtt` (client IDF natif) reste une piste si PubSubClient montre d'autres limites — hors scope ici.

### Itération 1bis — 2026-04-28 — Plan de test concretisé pour validation sans USB

**Statut** : `done`

**Contexte** : l'utilisateur n'a pas d'accès USB facile à l'ESP32 (le contrôleur est déjà en place dans son installation piscine). Les tests dynamiques D1/D2/D3/AC5/AC6 ont été retravaillés pour être 100 % exécutables depuis l'app web (Paramètres → Avancé → card Logs avec Auto 5s).

**Modifications de la spec** :

- D1 : procédure web-UI détaillée (stop add-on Mosquitto côté HA, observation logs ESP32, succès = aucun crash + UI fluide + reconnexion auto)
- D2 : reformulé en **Option 2** = débrancher le câble réseau du serveur HA pendant 2 minutes (impossible de couper le CPL côté ESP32 sans perdre l'accès UI)
- D3 / AC6 : automation HA `repeat: count: 50, delay: 0.1s`, observation log `WARN: MQTT inQueue saturée` côté ESP32
- AC5 : utilisation d'un coredump déjà existant (Paramètres → Avancé → Diagnostic crash → Télécharger), pas besoin de provoquer un nouveau crash
- Ajout d'une procédure **OTA** (vérifier `status=offline` côté broker juste avant restart)
- Ajout d'une procédure **mémoire** (suivi `getFreeHeap` sur la durée)

**Aucun changement de code** — uniquement la section « Plan de test manuel » de cette spec.

**Notes** :

- Le plan reste valable pour les futures itérations qui toucheraient `mqtt_manager.*` ou la régulation de la loop principale.
- Si un nouveau cycle modifie la queue ou la logique de drainage, ajouter ici les procédures de test correspondantes.

### Itération 2 — 2026-04-29 — Durcissement watchdog sur broker injoignable

**Statut** : `done` — tests verts (build SUCCESS, code-review OK), re-test dynamique D1/D2 délégué à l'humain après flash OTA

**Contexte** :

Pendant les tests manuels D1/D2 sur matériel réel, un crash watchdog a été observé pendant la procédure D2 (câble Ethernet du serveur Home Assistant débranché). Le coredump confirme que `mqttTask` était bloquée dans `WiFiClient::connect()` → `lwip_select()` lors d'un `mqtt.connect()` sur broker injoignable, > 30 s sans `esp_task_wdt_reset()`, déclenchant le PANIC sur le core 0.

Analyse :
- Le `setSocketTimeout(2)` ne borne **que les `read`** (cf. PubSubClient).
- Le `wifiClient.setTimeout(5000)` actuel ne suffit pas à couvrir tout le cycle `connect + CONNACK` quand le SYN TCP retransmet : `TCP_SYNMAXRTX` par défaut = 6, soit ~75 s cumulés en backoff exponentiel.
- Conséquence : entre le `esp_task_wdt_reset()` posé juste avant `mqtt.connect()` (ligne 204) et le prochain reset (à l'entrée du `if (connected)` ligne 237 ou en début de `taskLoop()` ligne 125), `mqttTask` peut rester bloquée 30+ secondes dans le connect — wdt PANIC garanti dès que la fenêtre dépasse 30 s.

D'autres chemins MQTT longs (boucles `publishDiscovery` ~17 publish, `publishAllStatesInternal` ~15 publish) sont aujourd'hui resetés **entre les blocs**, pas **entre les publish individuels** : un seul `mqtt.publish()` bloquant > 30 s suffit pour PANIC dans le pire cas (CPL très lossy).

Enfin, le `mqttManager.requestReconnect()` appelé depuis `setup()` (`main.cpp:134`) provoque une race avec le démarrage de `mqttTask` (qui se connecte déjà toute seule au premier tour de `taskLoop()` quand `!mqtt.connected() && mqttCfg.enabled`). Résultat : 1ʳᵉ connexion par mqttTask → flag posé par setup → mqttTask déconnecte → reconnecte → 2× discovery (32 publish au lieu de 17). Bénin en condition normale, mais double l'exposition aux blocages réseau juste avant les coupures.

**Travail à réaliser** (5 fixes) :

1. **`src/mqtt_manager.cpp:34`** — `wifiClient.setTimeout(5000)` → `wifiClient.setTimeout(2000)` (2 s au lieu de 5 s). Vérifier l'unité attendue par l'API Arduino-ESP32 (ms vs s) et **logger un commentaire explicite dans le code**. Introduire `kMqttClientConnectTimeoutMs` dans `constants.h` si nécessaire.
2. **`src/mqtt_manager.cpp:222`** (juste après `bool connected = ...mqtt.connect(...)`, avant le `if (connected)`) — ajouter un `esp_task_wdt_reset()` pour borner explicitement le pire cas connect, qu'il réussisse ou échoue.
3. **`src/mqtt_manager.cpp` `publishDiscovery()` (~694) et `publishAllStatesInternal()` (~430)** — ajouter `esp_task_wdt_reset()` **entre chaque** `mqtt.publish()` (ou tous les 4-5 au minimum). Approche élégante retenue : modifier le helper `publishConfig` (~708) pour qu'il appelle `esp_task_wdt_reset()` après chaque publish, et insérer un reset après chaque `mqtt.publish()` direct dans `publishAllStatesInternal()`.
4. **`src/main.cpp:134`** — retirer `mqttManager.requestReconnect()`. Raison : `MqttManager::begin()` démarre déjà `mqttTask` qui gère la connexion initiale. Vérifier la cohérence du commentaire `// Connexion MQTT initiale`.
5. **`src/mqtt_manager.cpp` `taskLoop()` (~123)** — ajouter `connectedAtomic.store(mqtt.connected(), std::memory_order_relaxed)` en début de chaque tour, **avant** la branche `if (!mqtt.connected() ...)`. Single source of truth pour l'UI (qui lit `mqttManager.isConnected()` = `connectedAtomic`) et le WARN (qui lit `mqtt.connected()` directement). Évite la divergence observée pendant D2 où l'UI affichait « Déconnecté » sans WARN dans les logs. Conserver les `connectedAtomic.store(true/false)` existants aux transitions ou factoriser — à arbitrer par architect/code-reviewer.

**Hors scope** :
- Pas de changement de protocole MQTT, topics, payloads, auto-discovery
- Pas de modification de `TCP_SYNMAXRTX` dans sdkconfig (peut être proposé comme follow-up)
- Pas de migration vers `esp-mqtt`
- Pas de re-test D1/D2 par l'agent — re-test délégué à l'humain après flash

**Critères d'acceptation spécifiques à cette itération** :

- [x] AC-IT2-1 — Fix F1 appliqué, unité du timeout WiFiClient documentée dans le code (commentaire explicite). **Découverte clé** : `WiFiClient::setTimeout()` d'Arduino-ESP32 6.9.0 attend des **secondes** (`_timeout = seconds * 1000` dans `WiFiClient.cpp:327`), pas des ms. Le code historique `setTimeout(5000)` programmait 5000 secondes (~83 min) — bug latent qui rendait le timeout côté client TCP totalement inopérant. Corrigé à `setTimeout(kMqttClientConnectTimeoutSec)` avec `kMqttClientConnectTimeoutSec = 2` dans `constants.h` (commentaire d'unité explicite).
- [x] AC-IT2-2 — Fix F2 appliqué, `esp_task_wdt_reset()` ajouté juste après le retour de `mqtt.connect()` dans `connectInTask()` (couvre le cas SYN TCP retransmis sur broker injoignable jusqu'à ~75 s avant abandon lwip).
- [x] AC-IT2-3 — Fix F3 appliqué : helper `publishConfig` instrumenté (reset après chaque publish individuel d'auto-discovery, 17 messages) + reset au milieu de `publishAllStatesInternal()` (≤ 5 publish entre 2 resets). Vérification visuelle : aucun bloc > 5 × `setSocketTimeout(2)` = 10 s ne reste possible entre deux resets.
- [x] AC-IT2-4 — Fix F4 appliqué, `mqttManager.requestReconnect()` retiré de `setup()` dans `main.cpp`. `mqttTask` se connecte au premier tour de `taskLoop()` quand `!mqtt.connected() && mqttCfg.enabled`.
- [x] AC-IT2-5 — Fix F5 appliqué, `connectedAtomic.store(mqtt.connected(), std::memory_order_relaxed)` posé en début de chaque tour de `taskLoop()` (single source of truth). Suppression de 3 stores intermédiaires redondants. Transitions explicites conservées en `connectInTask()` (succès) et `disconnect()` (depuis `loopTask`).
- [x] AC-IT2-6 — `pio run` SUCCESS, 0 nouveau warning, RAM 16.4 % / Flash 97.7 % (inchangé baseline).
- [x] AC-IT2-7 — Watchdog jamais désactivé, mutex `configMutex` symétriques, API publique `MqttManager` inchangée (vérifié par `code-reviewer`).
- [x] AC-IT2-8 — `docs/subsystems/mqtt-manager.md` mis à jour (nouvelle section « Watchdog dans `mqttTask` » avec tableau des 10 emplacements ; nouvelle section « Timeout client TCP — UNITÉ EN SECONDES » avec AVERTISSEMENT visible ; section « Détection des déconnexions » étendue avec le store canonique ; section « Reconnexion » étendue avec « Connexion initiale au boot »).
- [x] AC-IT2-9 — `CHANGELOG.md` `[Unreleased]` 2026-04-29 ouvert : entrée Firmware « Durcissement watchdog mqttTask sur broker injoignable » avec sous-bullets, entrée Documentation pour MAJ doc subsystem + section « Évolutions » de l'ADR-0011.
- [ ] AC-IT2-10 — Re-test D1/D2 par l'humain : **délégué** — à exécuter après flash OTA pour confirmer qu'aucun PANIC n'apparaît pendant 3 min de broker injoignable.

**Travail réalisé** :

- **F1 — Timeout client TCP corrigé** : `wifiClient.setTimeout(5000)` → `wifiClient.setTimeout(kMqttClientConnectTimeoutSec)` avec `kMqttClientConnectTimeoutSec = 2` dans `constants.h`. Commentaire d'unité explicite ajouté en regard de la constante. Bug latent documenté dans la doc subsystem (encadré AVERTISSEMENT).
- **F2 — Reset post-`mqtt.connect()`** : `esp_task_wdt_reset()` inséré juste après le retour de `mqtt.connect()` dans `connectInTask()`, qu'il réussisse ou échoue.
- **F3 — Resets granulaires sur les chemins longs** : `esp_task_wdt_reset()` au milieu de `publishAllStatesInternal()` (entre les 2 moitiés des ~15 publish d'états) ; `esp_task_wdt_reset()` après chaque publish individuel dans le helper `publishConfig` lambda de `publishDiscovery()` (17 messages auto-discovery) — cadence garantie ≤ 5 publish entre 2 resets.
- **F4 — Suppression race au boot** : `mqttManager.requestReconnect()` retiré de `setup()` dans `main.cpp`. La 1ʳᵉ connexion est gérée par `mqttTask` au premier tour de `taskLoop()`. Plus de double publish d'auto-discovery au démarrage (32 → 17).
- **F5 — `connectedAtomic` single source of truth** : `connectedAtomic.store(mqtt.connected(), std::memory_order_relaxed)` posé en début de chaque tour de `taskLoop()` avant toute branche de reconnect/loop/drain. Transitions explicites conservées en `connectInTask()` (succès) et `disconnect()`. **3 stores intermédiaires redondants supprimés** dans `taskLoop()` post-publish (qui créaient une fenêtre de divergence UI/WARN).

**Fichiers modifiés** (3 fichiers, diff minimal) :

- `src/constants.h` — ajout de `constexpr uint32_t kMqttClientConnectTimeoutSec = 2;` avec commentaire d'unité explicite.
- `src/mqtt_manager.cpp` — `setTimeout(kMqttClientConnectTimeoutSec)` ; `esp_task_wdt_reset()` après `mqtt.connect()` ; reset dans le lambda `publishConfig` de `publishDiscovery` ; reset au milieu de `publishAllStatesInternal` ; store canonique `connectedAtomic.store(mqtt.connected())` en début de `taskLoop()` ; suppression des 3 stores redondants post-publish.
- `src/main.cpp` — suppression du `mqttManager.requestReconnect()` dans `setup()` (race au boot avec `mqttTask`).

> **Note** : les autres modifications visibles dans `git diff` au moment de l'itération (`WiFi.setSleep`, `WiFi.onEvent`, refonte WARN capteurs edge-triggered, suppression `requestReconnect()` dans `checkSystemHealth`, `kMaxHourlyDataPoints` 360 → 168, partition coredump, etc.) sont **préexistantes** et hors-scope IT2.

**Build** :

- `pio run` SUCCESS, **0 nouveau warning**.
- RAM static : **16.4 %** (53896 / 327680 octets), identique au baseline.
- Flash : **97.7 %** (inchangé).
- `./build_fs.sh` non impacté (aucune modif `data/`).

**Tests dynamiques restants** :

- **D1 — Broker arrêté 60 s** : à re-tester par l'humain après flash OTA. Critère : aucun crash watchdog, UI fluide, reconnexion automatique au retour.
- **D2 — Câble Ethernet HA débranché 2–3 min** : **scénario qui a déclenché le PANIC original**. À re-tester par l'humain après flash OTA pour confirmer que les fixes F1+F2 bornent bien `mqtt.connect()` sous le watchdog 30 s. Critère : aucun `--- DÉMARRAGE ---` pendant la coupure, `WARN: MQTT déconnecté détecté — état=-3` dans les 100–180 s, UI ESP32 et régulation pH/ORP non gelées.

**Notes pour la prochaine itération éventuelle** :

- Si malgré les fixes F1+F2 un blocage `WiFiClient::connect()` > 2 s persiste (cas extrêmes de SYN TCP qui retransmet), envisager **abaisser `TCP_SYNMAXRTX`** dans `sdkconfig` (par défaut 6, on pourrait le passer à 3 pour borner ~10 s au lieu de ~75 s avant abandon lwip). Hors scope IT2, à reconsidérer si le re-test D2 expose toujours un timing limite.
- **Surveiller `mqtt_task_stack_hwm`** sur plusieurs jours via le payload diagnostic MQTT — si le HWM reste stable > 4000, on peut réduire `kMqttTaskStackSize` à 6144 voire 4096 pour libérer ~4 KB de heap.
- **Surveiller la cadence des `WARN: MQTT déconnecté détecté — état=-3`** sur plusieurs jours en condition réelle — si la cadence est élevée alors que HA est sain, c'est probablement le CPL. Mitigation longue : Ethernet via bridge AP ou meilleur emplacement WiFi (cf. doc subsystem section troubleshooting).
- Si malgré tous ces durcissements le client `PubSubClient` montre encore d'autres limites (ex. blocages dans `lwip_write` durant un publish), la migration vers **`esp-mqtt`** (client IDF natif, tâche dédiée, callbacks event-based) reste l'évolution suivante envisagée — hors scope ici.

### Itération 3 — 2026-04-29 — Borne TCP write côté lwip + bail-out anticipé

**Statut** : `done` — build SUCCESS, code-review verdict APPROVE, re-test D2 humain (3ᵉ tentative) délégué après flash OTA

**Contexte** : re-test D2 par l'humain APRÈS flash IT2. Nouveau crash watchdog confirmé par coredump-5.bin. Le point de blocage a bougé : avant IT2 c'était `mqtt.connect()` (résolu par F1+F2) ; maintenant c'est `mqtt.publish()` dans `publishAllStatesInternal()`.

Backtrace `mqttTask` au moment du PANIC :

```
WiFiClient::write (size=33)
  → lwip_select
  → PubSubClient::write
  → PubSubClient::publish (payload="OFF", retained=true)
  → MqttManager::publishAllStatesInternal (mqtt_manager.cpp:158)
  → MqttManager::taskLoop
```

Diagnostic : un `mqtt.publish()` (33 octets, payload "OFF" — probablement `filtration_state` ou `lighting_state`) bloque dans `WiFiClient::write` parce que le send buffer TCP est plein, lwip retransmet, la socket reste `ESTABLISHED`. `setTimeout(2)` (F1) borne `SO_SNDTIMEO`/`SO_RCVTIMEO` côté API socket mais ne borne PAS la durée totale d'une retransmission TCP côté pile IP.

**Pourquoi IT2 n'a pas suffi** :

- F2 (wdt reset après `mqtt.connect()`) a couvert le crash en phase connect (validé par coredump-4 vs coredump-5)
- F3 (wdt reset entre groupes de publish) ne suffit pas : entre deux resets dans `publishAllStatesInternal`, il y a 5–7 publish. Un seul publish bloquant > 30 s suffit pour PANIC malgré les resets périphériques
- F1 (`setTimeout(2)`) ne couvre pas la retransmission TCP en write loop

**Travail à réaliser** :

- **F6** — `platformio.ini` : `build_flags += -DCONFIG_LWIP_TCP_MAXRTX=5` (au lieu de 12 par défaut). Après ~5–6 retransmissions infructueuses (~10 s cumulés), lwip ferme la socket → `mqtt.connected()` retourne false → publishs suivants skip immédiatement.
- **F7** — `mqtt_manager.cpp` `connectInTask()` après `mqtt.connect()` réussi : ajouter `setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &kMqttTcpUserTimeoutMs, sizeof(...))` à 5000 ms. Option lwip qui borne le temps total de retransmission par segment **avant abandon de la connexion**. Récupérer le `fd` natif depuis `wifiClient` via `wifiClient.fd()` ou équivalent.
- **F8** — `mqtt_manager.cpp` `publishAllStatesInternal()` : remplacer les 2 resets actuels (après les 3 capteurs + au milieu) par un `esp_task_wdt_reset()` **avant chaque** `mqtt.publish()` direct. Ajouter un `if (!mqtt.connected()) return;` après chaque ~3 publish pour bail-out rapide une fois la socket fermée par lwip (F6+F7).
- **F9** — `mqtt_manager.cpp` `publishDiscovery()` lambda `publishConfig` : ajouter aussi le check `if (!mqtt.connected()) return;` avant le `mqtt.publish()` (fail fast si déco au milieu des 17 publish).
- **F10** — Nouvelle constante `src/constants.h` : `constexpr int kMqttTcpUserTimeoutMs = 5000;` avec commentaire d'unité explicite (option socket lwip TCP_USER_TIMEOUT).

**Pourquoi cette combinaison** : avec F6+F7, un publish ne peut bloquer plus de ~10 s avant que lwip n'abandonne et propage l'erreur jusqu'à `WiFiClient::write` qui retournera 0/erreur. F8+F9 garantissent que dès que la socket est fermée, on stoppe net les publish suivants au lieu d'enchaîner les 14 restants.

**Hors scope IT3** :

- Pas de migration vers `esp-mqtt`
- Pas de routage de `publishAllStatesInternal` via `outQueue` (changement architectural plus lourd, à reconsidérer si IT3 ne suffit pas)
- Pas de modification du protocole, des topics, ni des payloads

**Travail réalisé** :

- **F6 — `CONFIG_LWIP_TCP_MAXRTX=5` global** : ajout de `-DCONFIG_LWIP_TCP_MAXRTX=5` dans `build_flags` de `platformio.ini` avec commentaire référence feature-014 IT3 / ADR-0011. Après ~5 retransmissions infructueuses (≈10 s cumulés en backoff exponentiel, contre ~75 s avec la valeur par défaut 12), lwip abandonne la socket → `mqtt.connected()` retourne `false` → bail-out F8/F9 court-circuitent les publish suivants. **Trade-off accepté** : paramètre global à toute la pile lwip → impact aussi AsyncWebServer / OTA HTTP / NTP (abandon socket ~10 s vs ~75 s). Imperceptible en LAN sain, mais OTA en réseau très dégradé peut être avorté plus tôt et nécessiter un retry humain.
- **F8 — Cadence wdt 1:1 + bail-out dans `publishAllStatesInternal()`** : 20 `esp_task_wdt_reset()` posés **avant chaque** `mqtt.publish()` (vs 2 resets par groupes en IT2) — garantit qu'au pire un seul publish reste à l'intérieur de la fenêtre wdt 30 s. 5 `if (!mqtt.connected()) return;` répartis tous les ~3-4 publish stoppent net les publish restants dès que lwip a fermé la socket.
- **F9 — Bail-out fail-fast dans `publishDiscovery()` lambda `publishConfig`** : `if (!mqtt.connected()) { doc.clear(); return; }` en tête du helper. Même logique appliquée à la salve des 17 publish d'auto-discovery.
- **F7 et F10 ANNULÉS** : le plan IT3 initial prévoyait `setsockopt(TCP_USER_TIMEOUT, 5000)` (option lwip RFC 5482, par-socket) avec une nouvelle constante `kMqttTcpUserTimeoutMs` dans `constants.h`. **Vérification faite : `TCP_USER_TIMEOUT` n'est pas supporté par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). Le `setsockopt` retournerait `EINVAL` sans effet. F7 et F10 ont donc été abandonnés. Le bornage TCP repose **uniquement sur F6** (paramètre global lwip).

**Fichiers modifiés** (2 fichiers, diff minimal) :

- `platformio.ini` — ajout de `-DCONFIG_LWIP_TCP_MAXRTX=5` dans `build_flags` avec commentaire de référence (feature-014 IT3 / ADR-0011).
- `src/mqtt_manager.cpp` — `publishAllStatesInternal()` : 20 `esp_task_wdt_reset()` AVANT chaque `mqtt.publish()` (cadence 1:1) + 5 `if (!mqtt.connected()) return;` répartis tous les ~3-4 publish ; `publishDiscovery()` lambda `publishConfig` : `if (!mqtt.connected()) { doc.clear(); return; }` ajouté en tête.

**Build** :

- `pio run` SUCCESS, **0 nouveau warning imprévu**. Les 2 warnings `redefined` sur `CONFIG_LWIP_TCP_MAXRTX` et `CONFIG_LWIP_MAX_SOCKETS` sont **attendus** — conséquence voulue du `-D` override sdkconfig (le compilateur signale qu'on remplace la valeur par défaut sdkconfig par la valeur build_flags).
- RAM static : **16.4 %** (53896 / 327680 octets), identique au baseline.
- Flash : **97.8 %** (mineure augmentation due à l'ajout des bail-out + resets supplémentaires).
- `./build_fs.sh` non impacté (aucune modif `data/`).

**Tests dynamiques restants** :

- **D2 — Câble Ethernet HA débranché 2–3 min** : à re-tester par l'humain (3ᵉ test) après flash OTA pour confirmer que F6+F8+F9 bornent bien `mqtt.publish()` sous le watchdog 30 s. Critère : aucun `--- DÉMARRAGE ---` pendant la coupure, `WARN: MQTT déconnecté détecté — état=-3` dans les ~10 s suivant la coupure (au lieu de 100–180 s avec lwip default), UI ESP32 et régulation pH/ORP non gelées, reconnexion automatique au retour du réseau.

**Notes pour la prochaine itération éventuelle** :

- Si malgré F6+F8+F9 un blocage `WiFiClient::write()` > 10 s persiste (cas extrêmes de send buffer TCP coincé par AsyncWebServer simultanément), envisager **router `publishAllStatesInternal` via `outQueue`** au lieu de publier directement depuis `mqttTask` — consommation par drainOutQueue() avec rate-limit. Changement architectural plus lourd, à reconsidérer uniquement si IT3 expose toujours un timing limite.
- Une migration future vers ESP-IDF 5.x (Arduino-ESP32 3.x ou pioarduino) apporterait potentiellement le support de `TCP_USER_TIMEOUT` (RFC 5482), ce qui permettrait de **ré-isoler le bornage à la socket MQTT** et restaurer le default lwip (12 retries) globalement. À reconsidérer dans le cadre d'une éventuelle migration plateforme.
- Migrer éventuellement vers **`esp-mqtt`** (client IDF natif, tâche dédiée, callbacks event-based) reste l'évolution suivante envisagée — hors scope ici.

### Itération 4 — 2026-04-29 — Wrapper `safePublish()` + socket non-bloquante (pivot)

**Statut** : `done` — build SUCCESS, code-review verdict APPROVE, re-test D2 humain (4ᵉ tentative) délégué après flash OTA

**Contexte** : 3ᵉ re-test D2 par l'humain APRÈS flash IT3. Nouveau crash watchdog confirmé par coredump-6.bin. Le point de blocage a encore bougé : avant IT3 c'était `publishAllStatesInternal()` (résolu par F8) ; maintenant c'est `drainOutQueue()` qui est passé à travers les mailles.

Backtrace `mqttTask` au moment du PANIC :

```
WiFiClient::write (size=110)
  → lwip_select
  → PubSubClient::publish (payload="{...orp_limit...}", retained=false, plength=87)
  → MqttManager::drainOutQueue (mqtt_manager.cpp:270)
  → MqttManager::taskLoop (mqtt_manager.cpp:165)
```

**Diagnostic en deux temps** :

1. **Oubli IT3** : `drainOutQueue()` n'a PAS été instrumenté par F8/F9. Cette fonction consomme la queue de tous les `publishAlert()`/`publishStatus()`/`publishLog()`/`publishFiltrationState()` etc. Elle fait `mqtt.publish()` en bloquant sans `esp_task_wdt_reset()` entre les publish ni bail-out granulaire.

2. **F6 (`CONFIG_LWIP_TCP_MAXRTX=5`) borne moins fort que prévu** : avec `TCP_RTO_INITIAL=3000ms` (default lwip) et backoff exponentiel × 2 :
   - retrans 1 : T+3s, retrans 2 : T+9s, retrans 3 : T+21s, retrans 4 : T+45s, retrans 5 : T+93s
   - **Pire cas réel : ~93s avant abandon socket par lwip**, bien au-delà du watchdog 30s.

L'analyse architect IT3 avait sous-estimé ce point. Le `MAXRTX` seul ne suffit pas. Il faudrait coupler avec un `TCP_RTO_INITIAL` réduit, mais ça impacte les transferts normaux.

**Approche IT4 : éviter d'entrer dans `mqtt.publish()` quand la socket ne peut pas écrire**

Wrapper unique `safePublish(topic, payload, retain, len)` utilisé partout dans `mqttTask` :

```cpp
bool MqttManager::safePublish(const char* topic, const char* payload, bool retain, size_t payloadLen) {
  esp_task_wdt_reset();
  if (!mqtt.connected()) return false;
  // SO_SNDBUF par défaut ESP32 ~5760 octets, headers MQTT ~16 octets max pour les topics courts
  if (wifiClient.availableForWrite() < (int)(payloadLen + kMqttPublishHeadersOverhead)) {
    return false;  // socket en retransmission ou send buffer plein → skip silencieux
  }
  return mqtt.publish(topic, payload, retain);
}
```

**Pourquoi `availableForWrite()` est non-bloquant** : implémentation Arduino-ESP32 lit `SO_SNDBUF` via `getsockopt` (instantané) puis bytes en queue via `ioctl(TIOCOUTQ)` (instantané, pas de syscall réseau). Si la socket est en retransmission TCP, le buffer reste plein → retourne 0 → on skip → pas de blocage.

**Travail à réaliser** :

- **F11** — `src/mqtt_manager.h` : déclarer `bool safePublish(const char* topic, const char* payload, bool retain, size_t payloadLen);` privé.
- **F12** — `src/constants.h` : `constexpr size_t kMqttPublishHeadersOverhead = 16;` (commentaire : « Marge MQTT 3.1.1 fixed header + variable header pour topics courts <80c. À augmenter si on étend les topics. »)
- **F13** — `src/mqtt_manager.cpp` : implémenter `safePublish()` selon le pseudo-code ci-dessus.
- **F14** — `src/mqtt_manager.cpp` `drainOutQueue()` (~ligne 252-273) : remplacer le `mqtt.publish(...)` direct par `safePublish(...)` avec calcul de `strlen(msg.payload)`. **Fix l'oubli IT3, c'est le crash actuel**.
- **F15** — `src/mqtt_manager.cpp` `publishAllStatesInternal()` : remplacer les 20 `mqtt.publish()` directs par `safePublish()`. Supprimer les 20 `esp_task_wdt_reset()` manuels (le wrapper le fait). Supprimer les 5 bail-out `if (!mqtt.connected()) return;` manuels (le wrapper retourne false silencieusement).
- **F16** — `src/mqtt_manager.cpp` lambda `publishConfig` dans `publishDiscovery()` : remplacer `mqtt.publish()` par `safePublish()`. Supprimer les checks IT3 redondants.

**Bénéfice annexe** : le code devient drastiquement plus lisible. ~50 lignes supprimées (les checks et wdt resets éparpillés deviennent un appel unique).

**Hors scope IT4** :

- Pas de modification de `CONFIG_LWIP_TCP_MAXRTX` (laissé à 5, ne nuit pas)
- Pas de modification de `TCP_RTO_INITIAL` (impact transferts normaux)
- Pas de migration vers `esp-mqtt`
- Pas de routage de `publishAllStatesInternal` via `outQueue` (plus complexe, le wrapper résout déjà)
- Pas de modification du protocole, des topics, ni des payloads

**Découvertes IT4** (deux découvertes techniques majeures) :

1. **`WiFiClient::availableForWrite()` retourne TOUJOURS 0 dans Arduino-ESP32 6.9.0**. La méthode héritée de `Print::availableForWrite()` n'est **pas surchargée** par `WiFiClient`, donc renvoie systématiquement la valeur par défaut `0`. Le check envisagé en F12 (`availableForWrite() < payloadLen + kMqttPublishHeadersOverhead`) aurait court-circuité 100 % des publishes. **F12 a donc été annulé** au profit du pivot socket non-bloquante via `fcntl(O_NONBLOCK)`. La constante `kMqttPublishHeadersOverhead` n'a pas été créée.

2. **`CONFIG_LWIP_TCP_MAXRTX=5` borne à ~93 s en pratique, pas 10 s**. Avec `TCP_RTO_INITIAL=3000 ms` (default lwip) et backoff exponentiel × 2, la séquence des retransmits est T+3 s, T+9 s, T+21 s, T+45 s, **T+93 s**. L'analyse architect IT3 estimait ~10 s cumulés, mais c'est en réalité ~93 s — bien au-delà du watchdog 30 s. `MAXRTX=5` seul ne protège pas `mqttTask`. Le vrai mécanisme de protection est le mode non-bloquant. Le bornage `MAXRTX` est conservé comme garde-fou complémentaire (il accélère la fermeture de socket côté lwip pour que `mqtt.connected()` retourne `false` plus tôt).

**Travail réalisé** :

- **F11** — `src/mqtt_manager.h` : déclaration privée `bool safePublish(const char* topic, const char* payload, bool retain)` ligne 102.
- **F-NEW (pivot non-bloquant)** — `src/mqtt_manager.cpp` `connectInTask()` lignes 230-236 : passage de la socket TCP en mode non-bloquant via `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` après chaque `mqtt.connect()` réussi, avant `subscribe()`. Includes `<fcntl.h>` et `<errno.h>` ajoutés en tête de fichier.
- **F13** — `src/mqtt_manager.cpp` lignes 270-274 : implémentation de `safePublish()` (un `esp_task_wdt_reset()` + check `mqtt.connected()` + délégation à `mqtt.publish()`).
- **F14** — `src/mqtt_manager.cpp` `drainOutQueue()` : `mqtt.publish(...)` direct → `safePublish(...)`. **Fix l'oubli IT3, c'est le point de blocage du coredump-6.**
- **F15** — `src/mqtt_manager.cpp` `publishAllStatesInternal()` : 20 `mqtt.publish()` → `safePublish()`. **Suppression** des 20 `esp_task_wdt_reset()` IT3 manuels (factorisés dans le wrapper) et des 5 bail-out `if (!mqtt.connected()) return;` IT3 (le wrapper retourne `false` silencieusement).
- **F16** — `src/mqtt_manager.cpp` lambda `publishConfig` dans `publishDiscovery()` : `mqtt.publish()` → `safePublish()`. **Suppression** du bail-out `if (!mqtt.connected()) { doc.clear(); return; }` IT3 redondant.
- **24 call sites au total** couverts par `safePublish()` : status `online` au connect, `drainOutQueue` (1), `publishAllStatesInternal` (20), `publishDiagnosticInternal` (1), lambda `publishConfig` (1 site appelé 17 fois pour les 17 entités HA d'auto-discovery).
- **F12 ANNULÉ** : la constante `kMqttPublishHeadersOverhead` n'a pas été créée (cf. Découvertes — `availableForWrite()` cassé). Le pivot architectural socket non-bloquante remplace ce mécanisme sans constante supplémentaire.

**Fichiers modifiés** (2 fichiers) :

- `src/mqtt_manager.h` — ajout de la déclaration privée `bool safePublish(const char*, const char*, bool)`.
- `src/mqtt_manager.cpp` — includes `<fcntl.h>` + `<errno.h>` ajoutés ; `connectInTask()` enrichie du passage en `O_NONBLOCK` post-connect ; implémentation de `safePublish()` ; remplacement des 24 `mqtt.publish()` directs par `safePublish()` ; suppression des 20 `esp_task_wdt_reset()` IT3 dans `publishAllStatesInternal` et des 6 bail-out `if (!mqtt.connected()) return;` IT3 (5 dans `publishAllStatesInternal` + 1 dans la lambda `publishConfig`).

**Build** :

- `pio run` SUCCESS, **0 nouveau warning**.
- RAM static : **16.4 %** (53896 / 327680 octets), identique au baseline.
- Flash : **97.8 %** (légère baisse vs IT3 grâce aux ~50 lignes supprimées par la factorisation).
- `./build_fs.sh` non impacté (aucune modif `data/`).

**Tests dynamiques restants** :

- **D2 — Câble Ethernet HA débranché 2–3 min** : **VALIDÉ — 2026-04-29, 4ᵉ tentative humaine = PASS**. Aucun PANIC watchdog, `mqttTask` ne bloque plus, `WARN: MQTT déconnecté détecté — état=-4` détecté à 14:12:43 (~60 s après le début des drops à 14:11:53). Reconnexion automatique à 14:15:13 au retour de l'Ethernet HA. **Observation** : l'état dominant est désormais `-4` (`MQTT_CONNECTION_TIMEOUT`, keepalive PubSubClient applicatif) au lieu de `-3` (`MQTT_CONNECTION_LOST`, abandon TCP lwip) — c'est un **bénéfice indirect** du mode non-bloquant : `mqtt.loop()` continue à tourner et le keepalive 60 s gagne sur le timeout TCP 90–180 s. Détection plus précoce et plus propre. Doc D1/D2 et `mqtt-manager.md` mises à jour pour refléter cette dominance.
- **D1 — Broker arrêté 60 s** : à re-tester par l'humain. Devrait donner `-3` (fermeture propre Mosquitto avec `FIN`/`RST`) ou `-4` (kill brutal) selon la procédure d'arrêt côté HA. Plus de blocage attendu côté `mqttTask`.

**Notes pour la prochaine itération éventuelle** :

- Si malgré le pivot non-bloquant un blocage subtil persiste (cas extrêmes où `mqtt.connect()` lui-même bloque longuement avant que `O_NONBLOCK` soit posé), envisager **passer la socket en `O_NONBLOCK` en amont du `mqtt.connect()`** — nécessite de gérer le `EINPROGRESS` du connect, plus invasif. Hors scope IT4.
- Une migration future vers ESP-IDF 5.x (Arduino-ESP32 3.x ou pioarduino) apporterait potentiellement `WiFiClient::availableForWrite()` correctement override + le support de `TCP_USER_TIMEOUT` (RFC 5482), permettant des choix plus fins. À reconsidérer dans le cadre d'une éventuelle migration plateforme.
- Migrer éventuellement vers **`esp-mqtt`** (client IDF natif, tâche dédiée, callbacks event-based) reste l'évolution suivante envisagée — hors scope ici.

### Itération 5 — 2026-04-30 — Fiabilisation keepalive PINGREQ via `SO_SNDTIMEO` (au lieu de `O_NONBLOCK`)

**Statut** : `done` — build SUCCESS (RAM 16.4 %, Flash 97.8 %, 0 nouveau warning), code-review verdict APPROVE, AC-IT5-3 et AC-IT5-4 délégués à l'humain après flash OTA

**Contexte** : observation utilisateur de déconnexions MQTT ponctuelles non systématiques après IT4. Logs Mosquitto et ESP32 corrélés :

```
Mosquitto :
  13:55:57 Client ESP32PoolController disconnected: exceeded timeout.
ESP32 :
  13:56:05 WARN: MQTT déconnecté détecté — état=-3
  13:56:05 INFO: Tentative connexion MQTT (délai=5s)...
  ... backoff 5→10→20→40→80s ...
```

`exceeded timeout` côté broker = aucun PINGREQ reçu pendant `keepAlive × 1.5 = 90 s`. Le client ESP32 le détecte ~8 s plus tard via le FIN TCP envoyé par Mosquitto (état=-3).

**Cause racine** : side-effect d'IT4. Le socket TCP est mis en `O_NONBLOCK` après chaque `mqtt.connect()` réussi. PubSubClient `loop()` envoie le PINGREQ (2 octets) toutes les `keepAlive = 60 s` via `_client->write(buf, 2)` **sans vérifier le retour** ; il met immédiatement `lastOutActivity = t` et `pingOutstanding = true`. En mode non-bloquant, si le send buffer TCP est plein à cet instant précis (publish en cours, retransmission, latence), `lwip_send()` retourne `EAGAIN` et **les 2 octets ne partent jamais**. Le broker n'entend rien pendant 90 s → coupe la session.

C'est exactement le contre-pied de ce qu'IT4 cherchait à éviter (blocage long), mais le pivot total `O_NONBLOCK` a fragilisé le keepalive.

**Approche IT5** : remplacer `O_NONBLOCK` par **`SO_SNDTIMEO` à 500 ms**.

- Socket reste **bloquante** mais avec un **timeout d'écriture borné**
- Le PINGREQ (2 octets) a largement le temps de passer en < 100 ms sur réseau normal
- Un publish massif est borné par appel à 500 ms (vs blocage > 30 s avant IT4)
- Pire cas `publishDiscovery` (17 publish enchaînés) : 17 × 500 ms = 8.5 s, sous le watchdog 30 s avec marge
- mqttTask reste protégée du blocage long (objectif initial d'IT4 préservé)
- Le keepalive PubSubClient redevient fiable (le PINGREQ a le temps de partir, et même s'il rate, le broker timeout sera suivi d'une reconnexion automatique au prochain tour de `taskLoop`)

**Travail à réaliser** :

- **F17** — `src/mqtt_manager.cpp` `connectInTask()` : remplacer le bloc `fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)` par `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))` avec `struct timeval tv = {0, 500*1000}` (500 ms). Conserver la garde `if (fd >= 0)` et le log warning sur échec.
- **F18** — `src/constants.h` : nouvelle constante `kMqttSocketSendTimeoutMs = 500` avec commentaire d'unité (ms).
- **F19** — `src/mqtt_manager.cpp` : retirer les includes `<fcntl.h>` (s'ils ne servent plus) ; ajouter `<sys/socket.h>` ou `<lwip/sockets.h>` pour `SO_SNDTIMEO` et `struct timeval`. À vérifier par architect/embedded-developer.
- **F20** — `safePublish()` reste **inchangé** : le check `mqtt.connected()` + `esp_task_wdt_reset()` est toujours pertinent. Le `mqtt.publish()` interne sera désormais borné par `SO_SNDTIMEO` au lieu d'un retour immédiat `EAGAIN`. Drop silencieux préservé : si `mqtt.publish` retourne false (write timeout), `safePublish` retourne false comme avant.

**Hors scope IT5** :

- Pas de changement de l'API externe ni des topics MQTT
- Pas de routage de `publishAllStatesInternal` via `outQueue`
- Pas de migration vers `esp-mqtt`
- Pas de modification du keepalive PubSubClient (60 s) ni de `setSocketTimeout` (2 s)
- Pas de modif de `CONFIG_LWIP_TCP_MAXRTX=5` (laissé à 5, ne nuit pas)

**Critères d'acceptation IT5** :

- [x] AC-IT5-1 — Build `pio run` SUCCESS, RAM/Flash inchangés (±0.1 %), 0 nouveau warning imprévu
- [x] AC-IT5-2 — Vérification par lecture de code : `O_NONBLOCK` plus utilisé dans `mqtt_manager.cpp` ; `SO_SNDTIMEO` posé à 500 ms après chaque `mqtt.connect()` réussi
- [ ] AC-IT5-3 — Test manuel humain : sur plusieurs heures de fonctionnement, **aucune déconnexion `exceeded timeout` côté Mosquitto** (à valider sur les logs broker, pas seulement côté ESP32) — **délégué à l'humain post-flash OTA**
- [ ] AC-IT5-4 — Re-test D2 (Ethernet HA débranché 2-3 min) toujours sans crash watchdog (non-régression IT4) — **délégué à l'humain post-flash OTA**
- [x] AC-IT5-5 — Tous les libellés utilisateur restent en français (non concerné, pas d'UI)

**Travail réalisé** :

- **F17** — `src/mqtt_manager.cpp` `connectInTask()` : bloc `fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)` remplacé par `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))` avec `tv = {0, 500_000 µs}`. Garde `fd >= 0` conservée, log warning sur échec `setsockopt` avec `errno`.
- **F18** — `src/constants.h` : ajout `kMqttSocketSendTimeoutMs = 500` (ms) avec commentaire d'unité explicite et référence ADR-0011 IT5.
- **F19** — `src/mqtt_manager.cpp` includes : `<fcntl.h>` retiré (plus utilisé), `<lwip/sockets.h>` ajouté pour `SO_SNDTIMEO`, `SOL_SOCKET`, `struct timeval`.
- **F20** — `safePublish()` runtime inchangé. Commentaire d'en-tête (lignes ~269–274 de `mqtt_manager.cpp`) reformulé pour refléter le mécanisme `SO_SNDTIMEO` au lieu du `O_NONBLOCK` d'IT4.

**Fichiers modifiés** :

- `src/constants.h` (1 nouvelle constante)
- `src/mqtt_manager.cpp` (includes, `connectInTask()`, commentaire `safePublish()`)

**Build** : `pio run` SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning. Aucun changement d'API publique, aucun changement de topics MQTT, aucun changement de contrat externe.

**Tests dynamiques restants** :

- AC-IT5-3 (validation Mosquitto sur plusieurs heures sans `exceeded timeout`) : à valider par l'humain après flash OTA, en consultant les logs broker pendant ≥ 6 h de fonctionnement nominal.
- AC-IT5-4 (re-test D2, Ethernet HA débranché 2–3 min) : à rejouer par l'humain après flash OTA pour confirmer la non-régression IT4 — pas de PANIC watchdog, détection en ~60 s via keepalive applicatif PubSubClient.

**Documentation mise à jour** :

- `CHANGELOG.md` (entrée 2026-04-30, section Firmware + Documentation)
- `docs/subsystems/mqtt-manager.md` (section « Garde-fou » renommée IT5, tableau paramètres + sections Keepalive, Bornage TCP, Bascule -3/-4)
- `docs/adr/0011-mqtt-task-dediee.md` (sous-section « Itération 5 — 2026-04-30 » ajoutée dans « Évolutions »)
