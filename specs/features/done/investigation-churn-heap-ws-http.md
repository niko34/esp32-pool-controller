# Investigation — Fragmentation heap par churn WebSocket/HTTP (cause racine)

- **Type** : investigation (analyse statique + plan d'instrumentation)
- **Statut** : done
- **Version cible** : à déterminer
- **Créée** : 2026-07-28
- **Domaine** : réseau/mémoire (ws_manager, web_server, AsyncTCP/ESPAsyncWebServer fork, mqtt) — aucun chemin de dosage
- **Specs liées** : bug-badalloc-ws-broadcast (symptôme confiné en 2.11.1), ADR-0028 (migration fork), ADR-0029 (auto-guérison heap — « containment, pas cause racine », CHANGELOG 2.9.2)

## Faits

1. Coredump 2026-07-28 (v2.11.0) : plus gros bloc libre < ~1,8 Ko en fonctionnement normal — effondrement plus rapide que la fenêtre d'échantillonnage 60 s de l'auto-guérison.
2. Historique : gels lwIP + fragmentation récurrents (2.9.0-2.9.2), traités par containment (auto-guérison mémoire + watchdog connectivité). La piste « churn WebSocket/HTTP » était notée à traiter ultérieurement.
3. Payload WS capteurs : 1 777 octets (a grossi au fil des features — champs filtre, sondes, dosage).
4. Le push WS de logs envoie UNE trame par ligne de log à chaque client connecté (callback logger→WS).

## Questions

1. **Qui alloue/libère quoi, à quelle cadence** : cycle de vie des buffers de trame WS dans le fork (makeSharedBuffer par message × N clients ?), churn des AsyncClient (connexions/déconnexions navigateur, keep-alive HTTP), String temporaires des JSON WS (_buildSensorJson), publishs MQTT (OutboundMsg fixes — a priori sains), timers/lambdas alloués par requête.
2. **Profil de fragmentation** : quelles séquences produisent des trous (alloc long-vécu au milieu d'allocs courtes — ex. un client WS qui reste connecté pendant un churn de requêtes HTTP) ?
3. **Scénario de l'effondrement rapide** : UI ouverte (WS + polling /data 30 s + page Journal paginée) + MQTT + mDNS — quel enchaînement peut faire chuter maxBloc de > 30 Ko à < 2 Ko entre deux ticks de 60 s ?
4. **Instrumentation minimale** pour la prochaine occurrence : historiser maxBloc à haute fréquence dans un ring RAM (dumpé au prochain boot ou sur alerte), compter les allocs de trames WS, tracer le nombre de clients WS/HTTP actifs au moment des chutes.

## Investigation (2026-07-28, analyse statique v2.11.0 — agent debugger)

### Correction d'un fait : le push WS de logs N'EXISTE PAS

Le fait n°4 est **faux dans le firmware livré**. `Logger::setLogCallback()` (`src/logger.h:58`)
n'a **aucun appelant** (`grep -rn setLogCallback src/` → seule la définition ; confirmé aussi
sur HEAD via `git grep`). `WsManager::broadcastLog()` (`src/ws_manager.cpp:148`) est du code
mort depuis son introduction en 2.7.0 (`git log -S setLogCallback` → seul commit `aedd007`,
qui ajoute le hook sans jamais le brancher). Le handler UI `msg.type === 'log'`
(`data/app.js:82`) ne reçoit donc jamais rien — la page Journal utilise le polling HTTP
`/get-logs`. **La boucle d'amplification « heap dégradé → WARN → trame WS par client » n'existe
pas.** La seule boucle liée aux logs est bornée : « Mémoire faible » CRITICAL à chaque tick 60 s
sans debounce (`src/main.cpp:770`) → `flushToDisk()` immédiat (`src/logger.cpp:72-74`) → allocs
LittleFS modestes 1×/min.

### Générateurs identifiés (classés par contribution probable)

**G1 — Épinglage par client WS lent/mort (candidat n°1 pour l'effondrement < 60 s).**
- `textAll` partage UN buffer entre clients (`makeSharedBuffer`, fork `AsyncWebSocket.cpp:861-865` :
  `make_shared<vector<uint8_t>>(len)` = 1 alloc ~1,8 Ko par push) mais chaque client garde une
  référence dans sa `_messageQueue` (deque, `AsyncWebSocket.cpp:490`).
- `WS_MAX_QUEUED_MESSAGES = 32` (fork `AsyncWebSocket.h:17`), `_closeWhenFull = false` par défaut
  (`AsyncWebSocket.h:228`) → queue pleine = messages **jetés**, connexion **conservée**
  (`AsyncWebSocket.cpp:483-485`).
- Aucun mécanisme ne récolte un client gelé : `setRxTimeout(0)` (`AsyncWebSocket.cpp:252`),
  keepalive ping désactivé (`_keepAlivePeriod(0)`, ligne 250), et `cleanupClients(4)`
  (`src/ws_manager.cpp:56`) ne ferme que si count > 4 (`AsyncWebSocket.cpp:1072-1086`).
- Chiffrage : téléphone verrouillé / WiFi en veille → fenêtre TCP gelée → 1 buffer ~1,8 Ko
  épinglé toutes les 5 s, **queue pleine en 160 s** = ~58 Ko épinglés en 32 blocs dispersés,
  entrelacés avec les allocs transitoires (pbufs lwIP, requêtes HTTP) → le libre restant est
  découpé en trous < 2 Ko. Cinétique compatible avec « chute entre deux ticks de 60 s » et avec
  le bad_alloc de 1777 o dans `textAll` (c'est `make_shared` qui throw, non rattrapé).

**G2 — Churn du push capteurs 5 s (bruit de fond permanent, ~1,8 Ko/bloc).**
Par push et par client : (a) `_buildSensorJson` : `out.reserve(1600)` (`src/ws_manager.cpp:344`)
alors que le payload réel fait **1777 o** — le doc a été bumpé à 2048 (ligne 177) mais pas le
reserve → ~6 réallocations de ~1,7 Ko pendant `serializeJson` ; (b) `makeSharedBuffer` 1,8 Ko ;
(c) copie dans les pbufs lwIP via `client->add()` (`AsyncWebSocket.cpp:105-125`, 2 segments TCP)
+ malloc/free d'en-tête. ≈ 10-15 allocs / ~15 Ko de trafic allocateur par push → ~180 Ko/min/client
de blocs ~1,8 Ko. C'est le « sable » qui remplit les trous entre les blocs épinglés de G1.

**G3 — Grosses String contiguës HTTP épinglées pendant le transfert.**
`/get-history?range=all` (chargé à CHAQUE ouverture du dashboard, `data/app.js:6553`) :
315 points max (72+168+75, `src/constants.h:109-111`) × 128 o → `String::reserve` ~40 Ko
(`src/web_routes_data.cpp:380`) + copie `std::vector<DataPoint>` (~7 Ko), puis le
`shared_ptr<String>` reste vivant pendant tout le transfert async (`src/web_helpers.cpp:10-22`)
— plusieurs secondes sur WiFi lent, pendant que G1/G2 allouent autour. Idem mini-charts 3d
(~30 Ko initial, `app.js:6557`).

**G4 — Polling Journal** : `loadLogs` toutes les 5 s page Journal ouverte (`app.js:4824`),
paginé (≤8 fetches en rechargement complet) — objets requête (std::list de String d'en-têtes,
fork `ESPAsyncWebServer.h:459-462`), `JsonDocument` heap + réponse ~4 Ko. Contributeur moyen.

**G5 — mDNS/MQTT : écartés comme déclencheurs.** MQTT = queue fixe d'`OutboundMsg`
(`src/mqtt_manager.cpp:95`), ré-annonce mDNS événementielle avec debounce (`src/main.cpp:47-53`).
Churn résiduel faible et constant.

### Scénario d'effondrement le plus crédible

UI ouverte sur téléphone → l'écran se verrouille SANS fermer le TCP (WiFi power-save) → le
client WS devient zombie ; l'utilisateur rouvre l'appli → **2e client WS** + rechargement
dashboard (`/get-history all` = 40 Ko contigus + mini-charts 30 Ko). Pendant ce temps le zombie
épingle 12 blocs de 1,8 Ko/min (jusqu'à 32) au milieu du churn G2/G3/G4. En 2-3 min, maxBloc
passe de dizaines de Ko à < 1,8 Ko → `make_shared` throw dans `textAll` → abort. Plus rapide
que le debounce auto-guérison (3 ticks × 60 s, `kAutoHealDebounceChecks`).

### Recommandations hiérarchisées

Quick wins (futur cycle, périmètre ws_manager + fork API publique) :
1. Dans `WsManager::update()` : fermer tout client dont `queueLen()` dépasse ~4 (l'état capteurs
   est idempotent, inutile d'empiler 32 snapshots) et/ou `client->setCloseClientOnQueueFull(true)`
   (`AsyncWebSocket.h:305`) à la connexion. Tue G1.
2. `client->keepAlivePeriod(30)` à la connexion → PING auto, les zombies meurent par timeout ACK.
3. `out.reserve(1600)` → 2048 dans `_buildSensorJson` (`src/ws_manager.cpp:344`). Tue les
   réallocations G2a.
4. Ne pas enqueuer vers les clients non authentifiés (textAll arrose tous les WS_CONNECTED).
5. Debouncer le CRITICAL « Mémoire faible » (`src/main.cpp:770`).

Refonte (si récidive après quick wins) :
6. `/get-history` en réponse chunked (machine à états comme `/download-logs`) → supprime la
   String contiguë 40 Ko (G3).
7. Supprimer `broadcastLog`/`setLogCallback` (code mort) ou les brancher volontairement avec
   garde anti-récursion — décision produit.

### Instrumentation minimale (prochaine occurrence)

- **Ring RTC noinit** : `RTC_NOINIT_ATTR` (survit à panic/restart, pas au power-on),
  128 entrées × 8 o = 1 Ko : `{uptime_s:u16, maxBloc/64:u16, freeHeap/256:u8, wsClients:u4,
  maxQueueLen:u4, httpEnCours:u8}` + magic. Échantillonné toutes les 2 s depuis la loop
  (coût : `heap_caps_get_largest_free_block` ≈ µs). Au boot suivant, si magic valide et
  reset != POWERON → dump dans system.log. Donne la **cinétique fine** (4 min d'historique)
  que le tick 60 s rate.
- **Compteur queue WS** : max de `client->queueLen()` sur les clients à chaque push + WARN
  one-shot si > 8. Discrimine directement G1 (queue qui monte AVANT la chute de maxBloc)
  vs churn pur (queues à 0-1 pendant que maxBloc s'érode).
- Coût total : ~1 Ko RTC RAM + ~30 lignes ; aucun impact chemin dosage.

## Critères de clôture

- [x] Générateur(s) de fragmentation identifié(s) avec preuves code (fichier:ligne) OU plan d'instrumentation embarqué prêt à spécifier.
- [x] Recommandations hiérarchisées (quick wins vs refonte) pour un futur cycle.

## Clôture (2026-07-28)

- [x] Générateurs identifiés avec preuves (G1 zombie WS — traité en v2.11.1 ; G2 churn push — atténué reserve 2048 ; G3 String /get-history 40 Ko épinglée — reporté ; G4 polling Journal — moyen ; G5 mDNS/MQTT — écartés). Boucle d'amplification logs→WS réfutée (code mort).
- [x] Recommandations hiérarchisées : quick wins Q1-Q4 repliés dans bug-badalloc-ws-broadcast (v2.11.1) ; reportés si récidive : /get-history chunked, purge broadcastLog/setLogCallback, instrumentation ring RTC noinit (spec prête à écrire), push WS limité aux clients authentifiés.
