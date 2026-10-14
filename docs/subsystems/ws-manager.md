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
bool broadcastSensorData();            // push immédiat, toutes données — cible UNIQUEMENT les clients authentifiés (bug-081)
bool broadcastConfig();                // push immédiat, config actuelle — idem
bool broadcastLog(const LogRecord&);   // push un log — CODE MORT, voir ci-dessous (type renommé LogEntry → LogRecord en feature-129)
void revokeAllAuthenticated();         // bug-081 : révoque tous les clients authentifiés (bascule auth_enabled, rotation de token)
bool hasClients() const;
size_t clientCount() const;            // nb clients connectés (diagnostic, v2.8.1)
uint32_t skippedPushCount() const;     // cumul des pushes sautés : garde heap OU bad_alloc rattrapé (v2.11.1 / v2.14.0)
uint32_t oversizedFrameCount() const;  // cumul des trames > kWsFrameBufBytes — pushes sautés, JAMAIS tronqués (v2.14.0)
uint32_t registryFullCount() const;    // bug-081 : cumul des connexions refusées (registre authentifié saturé, kWsMaxClients)
```

> **bug-081 (v2.57.0)** : les trois `broadcastXxx()` renvoient désormais `bool` (envoi
> réussi vers au moins un client) — valeur informative, aucun appelant existant ne
> l'exploite aujourd'hui. Voir [la section dédiée](#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall).

> ⚠️ **Code mort — `broadcastLog()` / `Logger::setLogCallback()`** (constat investigation churn-heap, 2026-07-28, **toujours vrai après feature-129**) : le hook `systemLogger.setLogCallback()` ([`src/logger.h`](../../src/logger.h)) n'a **aucun appelant** depuis son introduction en 2.7.0 (commit `aedd007` — ajouté sans jamais être branché). `broadcastLog()` n'est donc **jamais exécuté** : aucun message `type: "log"` n'est émis, le handler UI correspondant (`data/app.js`) ne reçoit rien, et la page Journal utilise le polling HTTP `/get-logs`. La purge (ou le branchement volontaire avec garde anti-récursion) est **reportée** à un futur cycle — en attendant, `broadcastLog()` porte quand même la garde heap v2.11.1 par cohérence, et son **type de paramètre a été maintenu à jour** (`LogEntry` → `LogRecord`, feature-129) malgré l'absence d'appelant : voir ci-dessous.

### `broadcastLog()` et le type `LogRecord` (feature-129)

Le journal a remplacé son ancien type `LogEntry` par deux types distincts,
`LogRecord` et `LogArenaView` (voir [`docs/subsystems/logger.md`](logger.md#arène-circulaire-du-journal-feature-129)).
`broadcastLog(const LogRecord&)` est le **seul** consommateur de `LogRecord`
côté `ws_manager` — signature mise à jour en même temps que `logger.h`, même
si aucun appelant ne l'invoque encore.

⚠️ **Si ce hook est un jour branché, retenir un point de sémantique** :
`LogRecord::message` pointe vers le tampon de l'**appelant** de `log()` (la
chaîne **intégrale**, avant toute troncature de stockage), jamais vers
`sLogArena`. La copie éventuellement stockée dans l'arène, elle, peut avoir
été tronquée à `kLogMessageMaxBytes` (768 o) et porter `kLogFlagTruncated`.
**`broadcastLog()` ne doit jamais propager ce drapeau** : le flux WS refléterait
alors un message complet comme « tronqué », ce qui est trompeur — précisément
sur un refus d'injection de sécurité, où faire croire qu'il manque une
information qui est en fait entièrement présente serait le pire des deux
sens d'erreur. C'est `/get-logs` (`trunc` par entrée, lu depuis l'arène) qui
porte cette information, pas le canal temps réel. Voir
[`docs/API.md`](../API.md#get-get-logs--write).

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

## Deux trames, deux gabarits — un seul plafond DUR (à lire AVANT tout chiffrage)

⚠️ Cette section existe parce que la confusion entre ces deux notions a produit **quatre chiffrages de budget faux d'affilée** pendant feature-092. Elle est **normative** : tout bloc de budget plus bas dans ce fichier s'y réfère.

Il y a **deux** trames JSON sortantes, donc **deux** gabarits `StaticJson<N>` distincts :

| Trame | Fabricant | Gabarit documentaire |
|---|---|---|
| `sensor_data` | `_buildSensorJson()` — [`src/ws_manager.cpp:303`](../../src/ws_manager.cpp) | `StaticJson<3584>` (**3456** de feature-092 à la v2.47.0, **3072** avant) |
| `config` | `_buildConfigJson()` — [`src/ws_manager.cpp:979`](../../src/ws_manager.cpp) | `StaticJson<3008>` |

**Le gabarit n'est PAS une limite.** En ArduinoJson 7, `StaticJson<N>` dérive de `JsonDocument` et `N` n'est plus que la valeur rendue par `capacity()` — le pool est alloué dynamiquement. **Le bumper coûte 0 octet de RAM.** Un bump refusé « pour économiser la RAM » serait donc un **mauvais arbitrage** : il n'économise rien. Le gabarit ne documente que l'ordre de grandeur attendu de la trame ; c'est un repère de lecture, pas un garde-fou.

**Le seul plafond DUR des deux chemins est `kWsFrameBufBytes = 3584` o** ([`src/constants.h`](../../src/constants.h)), taille du buffer statique `sWsFrameBuf` en `.bss`. Celui-là se paie : le relever coûte autant de `.bss`, donc autant de RAM en moins pour le tas.

**Mode de défaillance** : un dépassement **saute le push** (garde `measureJson(doc) + 1 > cap`, compteur `_oversizedFrames`, WARN throttlé) — il ne **tronque jamais**. Une trame trop grosse est une trame **entièrement perdue**, pas une trame amputée.

### La méthode de chiffrage (normative)

> On compte la valeur au **PIRE CAS STRUCTUREL** — la borne du tampon ou du type qui la porte — **jamais** une valeur relevée sur une trame vivante.

L'erreur d'origine de feature-092 tient en une phrase : la chaîne de commentaires de budget partait d'une **mesure vivante de 2 479 o** (v2.25.1) sur laquelle **huit deltas pire-cas** ont ensuite été empilés, version après version. Le total héritait donc, à chaque étape, du sous-comptage de son point de départ.

Cas d'école, rectifié dans ce cycle. Les deux causes de refus de dosage sont bornées par `kDoseBlockedReasonMaxLen = 96` (95 caractères + 2 guillemets = **97 o de valeur**) :

| Clé | Sérialisation pire cas | Octets | Était comptée |
|---|---|---:|---:|
| `phDoseBlockedReason` | `"phDoseBlockedReason":"<95 car.>",` | **120** | 62 |
| `orpDoseBlockedReason` | `"orpDoseBlockedReason":"<95 car.>",` | **121** | 64 |

**+115 o de correction sans qu'aucun champ n'ait été ajouté.** En fonctionnement normal ces deux clés pèsent ~8 o chacune — d'où le piège. Et ce pire cas n'est **pas théorique** : les causes de refus les plus longues sont celles de l'**escalade de sécurité pH**, c'est-à-dire le moment précis où perdre la trame coûte le plus cher. Même coïncidence que `ph_auto_manual_reason` sur la trame `config` (voir [budget de trame `config`](#budget-de-trame--décompte-rectifié-en-revue)).

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
3. `_authRegistry` (`WsClientRegistry<kWsRegistrySlots>`, [`ws_client_registry_logic.h`](../../src/ws_client_registry_logic.h)) garde les client IDs validés.
4. Les messages des clients non authentifiés sont ignorés.

⚠️ La vérification du token dans `_onData()` passe par `authManager.secureTokenEquals()` — **comparaison à temps constant**, même exigence que l'auth HTTP (v2.11.2, feature-028 ; jamais de `==` / `!=` direct sur le token, voir [auth.md](auth.md#comparaison-de-token-à-temps-constant-v2112-feature-028)). Token rejeté → log `[WS] Token rejeté` + fermeture de la connexion.

⚠️ **Avant bug-081 (v2.57.0), `_authenticatedClients` (`std::set<uint32_t>`) ne servait
qu'à décider « au moins un client authentifié existe-t-il ? » (garde de non-vacuité avant
`textAll()`) — il n'était jamais consulté pour choisir *qui* reçoit la trame. `textAll()`
diffusait aux clients connectés à `/ws`, authentifiés ou non. Voir la section dédiée
[bug-081](#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall) ci-dessous.**

Voir [`ws_manager.cpp`](../../src/ws_manager.cpp) `_onClientConnect()` et `_onData()`.

## bug-081 — diffusion ciblée sur registre, au lieu de `textAll()`

**Le défaut (jusqu'à la v2.56.1 incluse)** : `WsManager::update()` ne vérifiait que
« `_authenticatedClients` n'est pas vide » avant d'appeler `_ws->textAll(...)` — qui diffuse
à **tous** les clients connectés à la socket `/ws`, authentifiés ou non. Un client qui ne
s'authentifiait jamais recevait donc toutes les trames (`sensor_data`, `config`) dès qu'**un
autre** client légitime était connecté : mesures pH/ORP, états de dosage, volumes injectés,
causes de refus — 92 champs en clair. Capturé et documenté par la spec
[bug-081](../../specs/features/done/bug-081-websocket-diffuse-aux-clients-non-authentifies.md).
Corrigé en v2.57.0.

### Ciblage par identifiant : `AsyncWebSocket::text(uint32_t id, ...)`

Les trois broadcasts (`broadcastSensorData`, `broadcastConfig`, `broadcastLog`) passent
désormais par `WsManager::_sendToAuthenticated(buf, len)`, qui :
1. prend un **snapshot** des ids de `_authRegistry` sur la **pile** de `loopTask` (tableau
   `uint32_t ids[kWsRegistrySlots]`) ;
2. construit **un seul** `std::shared_ptr<std::vector<uint8_t>>` pour toute la trame ;
3. appelle `_ws->text(id, sharedBuffer)` pour chaque id du snapshot.

**Pourquoi `AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)` est sûr depuis
`loopTask`, là où itérer `getClients()` est un UB** (piège déjà documenté plus haut,
v2.11.1) : cette surcharge prend le verrou privé du fork `_ws_clients_lock` et résout
`client(id)` **sous ce verrou**, en rendant `false` si le client a disparu entre le
snapshot et l'envoi (déconnexion concurrente). Elle n'expose donc **jamais** la liste
interne à l'appelant — contrairement à une itération manuelle de `getClients()`, qui reste
interdite.

### Pourquoi le registre `_authRegistry` est SANS verrou applicatif (ne pas « améliorer »)

`_queueMessage` du fork, avec `_closeWhenFull = true` (`setCloseClientOnQueueFull`, posé à
la connexion, v2.11.1) : file saturée → `lock.unlock(); c->close();` → `AsyncClient::_close()`
(`_discard_cb` **synchrone**) → `_handleDisconnect` → destructeur du client → événement
`WS_EVT_DISCONNECT` → notre `_onEvent()` → **notre propre `_authRegistry.erase()`**, le tout
**dans la même pile d'appel que la boucle d'envoi de `_sendToAuthenticated`, depuis
`loopTask`**. Le fork y survit parce que son verrou est un `std::recursive_mutex`
(réentrant) ; un mutex FreeRTOS applicatif à nous (non récursif) s'auto-bloquerait sur ce
même chemin — interblocage garanti dès la première file saturée pendant un push.

D'où le choix : `_authRegistry` (`WsClientRegistry<N>`, [`ws_client_registry_logic.h`](../../src/ws_client_registry_logic.h))
n'est qu'un tableau fixe de `std::atomic<uint32_t>` (0 = slot libre), muté uniquement par
`compare_exchange` (`insert`/`erase`) ou par stores atomiques simples (`clear`). Le snapshot
pris sur la pile **avant** la boucle d'envoi est une **copie** : une mutation du registre
pendant l'itération (y compris la ré-entrance ci-dessus) ne la perturbe pas. Interdiction
absolue de tenir un verrou applicatif pendant cette boucle.

### ⚠️ Profil mémoire : ÉGAL à l'ancien, pas meilleur

`textAll(const char*, len)` **déléguait déjà** à `textAll(makeSharedBuffer(...))` dans le
fork ESP32Async — donc **un seul** buffer partagé entre tous les clients. Le nouveau code
(`_sendToAuthenticated`) fait de même : **un seul** `make_shared` par push, envoyé par
référence à `_ws->text(id, shared)` pour chaque destinataire. **Le profil mémoire de
bug-081 est donc identique à l'ancien, pas une amélioration.** Ce qui aurait coûté **N**
copies de ~3,5 Ko (une par client) est la surcharge `AsyncWebSocket::text(id, const char*,
len)` — qui appelle `makeSharedBuffer` à **chaque** appel — et elle a été **écartée** pour
cette raison précise en revue. Ne pas documenter ni commenter ce correctif comme un gain
mémoire : ce serait faux.

### Révocation : `WsManager::revokeAllAuthenticated()`

Appelée depuis deux handlers HTTP (jamais depuis loopTask) :
- `src/web_routes_config.cpp` — bascule `auth_enabled` **faux→vrai** ;
- `src/web_routes_auth.cpp` — `POST /auth/regenerate-token`.

Sans cette révocation, un invité auto-inscrit en mode ouvert (`auth_enabled = false`)
continuait de recevoir les pushes **après** l'activation de l'authentification — bug-081
rouvert par un autre chemin, puisque le registre ne se vide pas de lui-même à la bascule de
config. Même raisonnement pour une rotation de token : un client déjà authentifié avec
l'**ancien** token continuait sinon de recevoir indéfiniment (le keepalive maintient la
socket vivante).

Mécanique en deux temps, sûre depuis un contexte `async_tcp` :
1. `revokeAllAuthenticated()` vide `_authRegistry` **immédiatement**, par stores atomiques
   seuls (`WsClientRegistry::clear()`) — aucune section critique, appelable depuis
   n'importe quel contexte.
2. La fermeture effective des sockets (`_ws->closeAll()`) est **différée** : un flag
   `_pendingRevokeClose` (atomic bool) est posé, consommé au prochain passage de `update()`
   (`loopTask`). Jamais de `closeAll()` directement depuis le handler HTTP.

### `kWsMaxClients` / `kWsRegistrySlots`

- `kWsMaxClients = 4` ([`constants.h`](../../src/constants.h)) — nomme le littéral `4`
  auparavant en dur dans `_ws->cleanupClients(4)` (« Max 4 clients WS simultanés pour
  préserver les sockets lwIP », v2.11.1).
- `kWsRegistrySlots = kWsMaxClients + 1` — capacité du registre PUR. Le slot de rab absorbe
  la fenêtre entre l'accept `async_tcp` d'un Nᵉ+1 client **transitoire** et le prochain
  passage de `cleanupClients()` dans `loopTask` (qui n'a pas encore eu la main) : sans lui,
  cette fenêtre ferait refuser une authentification légitime en pur cas de bord de
  capacité. Le tampon de snapshot sur la pile (`_sendToAuthenticated`, `update()`) est
  dimensionné exactement sur cette constante (`static_assert` bornant `kWsRegistrySlots`
  entre 2 et 16 — le tampon vit sur la pile de `loopTask`).
- Un client en excès (registre saturé, `WsRegistryInsert::kFull`) est **fermé**
  (`client->close()`) plutôt que servi en silence ; chaque refus incrémente
  `registryFullCount()` et déclenche un WARN throttlé (30 s) dans `update()`.

### Compteur d'époque `_authEpoch` / `_servedEpoch` — remplace `_pendingInitialPush`

L'ancien booléen `_pendingInitialPush` était posé à `true` par **toute** insertion réussie
dans le registre et remis à `false` au prochain `update()` — une seule insertion entre deux
tours de `update()` déclenchait bien le push initial, mais rien ne garantissait qu'une
**seconde** insertion survenue avant que le premier push n'ait eu lieu ne soit pas perdue
(le booléen écrase, il ne compte pas). Remplacé par deux compteurs atomiques :
- `_authEpoch` — incrémenté (`fetch_add`, `memory_order_release`) à chaque insertion
  **réussie** (`kInserted`, jamais `kAlreadyPresent` ni `kFull`) dans `_onClientConnect()` ou
  `_onData()` ;
- `_servedEpoch` — lu (`memory_order_acquire`) **avant** le snapshot dans `update()` ;
  `initialPush = (authEpoch != servedEpoch)`, puis avancé **avant** l'envoi réel (dès que le
  registre était non vide à cet instant, même si la garde heap ou la saturation font sauter
  l'émission plus bas — sans quoi une famine mémoire prolongée ferait reconstruire
  `_buildSensorJson()` à chaque tour de `loop()` au lieu d'attendre le prochain cycle de 5 s).

L'ordre `release`/`acquire` s'apparie : en `relaxed`, l'époque pourrait devenir visible
**avant** l'écriture du slot du registre — `update()` verrait la nouvelle époque, prendrait
un snapshot **sans** ce client, puis avancerait `_servedEpoch` : le push initial serait perdu
pour lui, sans rattrapage possible (l'époque ne redeviendrait jamais différente pour ce
client tant qu'aucune autre insertion ne survient).

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

- **Pas de client AUTHENTIFIÉ connecté** : `broadcastSensorData()` ne fait rien (gain CPU). Depuis bug-081 (v2.57.0), la garde est `_authRegistry.empty()` — un client connecté mais **non authentifié** n'est **jamais** compté « présent » pour cette garde ni pour aucune des trois broadcasts : il ne reçoit rien, même si `_ws->count() > 0`. `hasClients()` reste basé sur `_ws->count()` (compte **toutes** les connexions, authentifiées ou non — diagnostic uniquement, ne pilote plus aucun envoi).
- **Client déconnecté sans close** : nettoyé par `AsyncWebSocket::cleanupClients()` appelé dans `update()` ; depuis v2.11.1, aussi récolté par saturation de file (fermeture par le fork) ou timeout de PING keepalive. Dans tous les cas, `WS_EVT_DISCONNECT` retire aussi l'id de `_authRegistry` (`_onEvent`).
- **Registre authentifié saturé** (`kWsMaxClients` dépassé, bug-081) : la connexion ou l'authentification excédentaire est **fermée** (`client->close()`) plutôt que servie en silence — compteur `registryFullCount()` + WARN throttlé 30 s dans `update()`. Jamais de log depuis le contexte `async_tcp` qui détecte la saturation (pile réduite).
- **Push pendant write en cours** : `AsyncWebSocket` gère la file d'attente (bornée à 5 trames depuis v2.11.1), pas de blocage du loop.
- **Heap fragmenté** (plus gros bloc **8 bits** `< kWsPushMinAllocBytes = 6144` depuis v2.33.0, [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)) : pushes **sautés** (garde v2.11.1), compteur `skippedPushCount()` + WARN throttlé 30 s dans `update()` (libellé `maxBloc8=`). Course résiduelle (TOCTOU) : `std::bad_alloc` **rattrapé** autour de l'envoi ciblé (`_sendToAuthenticated`, depuis bug-081 — anciennement `textAll`) → push sauté, même compteur, jamais d'`abort()` (v2.14.0).
- **Trame plus grande que le buffer statique** (`kWsFrameBufBytes = 3584` depuis v2.21.1, était 3072) : push **sauté** (jamais tronqué), compteur `oversizedFrameCount()` + WARN throttlé dédié dans `update()` — signale qu'il faut agrandir la constante (v2.14.0).
- **Bascule `auth_enabled` faux→vrai ou rotation de token** : révocation immédiate de tous les clients authentifiés — voir [`revokeAllAuthenticated()`](#révocation--wsmanagerrevokeallauthenticated).

## Dépendances

- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) **v3.11.2** (fork ESP32Async) — classe `AsyncWebSocket`.
- [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) **v3.4.10** (fork ESP32Async) — couche TCP asynchrone sous-jacente.

> **Fork ESP32Async — pourquoi (v2.9.0)** : le projet consommait auparavant les libs **originales** `me-no-dev` (`AsyncTCP` / `ESPAsyncWebServer`) via des dépôts Git **non épinglés**, aujourd'hui non maintenus. Un coredump a mis en évidence un **crash récurrent** (`panic → reboot`, appareil injoignable) déclenché au **timeout/close d'un client WebSocket** :
> `AsyncWebSocketClient::_onTimeout` → `AsyncClient::close()` → `_tcp_recved(pcb, len corrompu)` → `tcp_recved` (lwIP) → **assert `new_rcv_ann_wnd <= 0xffff`** (`tcp_update_rcv_ann_wnd`, `lwip/src/core/tcp.c`) → panic.
> C'est un **bug de course connu** des libs me-no-dev. La migration vers le fork maintenu **ESP32Async**, avec **versions épinglées** (`AsyncTCP@3.4.10`, `ESPAsyncWebServer@3.11.2`), corrige le mode de défaillance **sans changement de code applicatif** (fork rétrocompatible). *(Contexte de l'époque, v2.9.0 : la plateforme était `espressif32@6.9.0`, core 2.0.x / IDF 4.4, et elle n'a pas été touchée par cette migration de libs. Le projet est passé en core 3 / IDF 5 en v2.20.4, et le repli core 2 a été retiré en v2.42.1 — [ADR-0069](../adr/0069-retrait-repli-core-2.md) —, le couple épinglé restant le même.)* Voir [ADR-0028](../adr/0028-migration-fork-esp32async.md). C'est la 2ᵉ cause de crash de la pile web traitée par coredump ; la 1ʳᵉ (`std::bad_alloc` sur `/logs`) l'a été en 2.8.2.

## Fichiers liés

- [`src/ws_manager.h`](../../src/ws_manager.h), [`src/ws_manager.cpp`](../../src/ws_manager.cpp)
- [`src/ws_client_registry_logic.h`](../../src/ws_client_registry_logic.h) — registre PUR des clients authentifiés (bug-081, module sans Arduino.h/FreeRTOS, ADR-0017), testé par `test/test_native_ws_registry/`
- [`tools/check_ws_broadcast_targeting.sh`](../../tools/check_ws_broadcast_targeting.sh) — interdit tout `textAll`/`binaryAll`/`printfAll` dans `src/` (bug-081), lancé par `hygiene-specs.yml`
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

Valeurs lues via les getters `PumpController.isBoostActive()` / `getBoostUntilEpoch()`. Un changement d'état (activation via `POST /boost/start`, désactivation via `/boost/stop` ou commande HA `{base}/boost/set`, **ou expiration à minuit**) déclenche un push WS immédiat. Voir [pump-controller.md §Mode Boost](pump-controller.md#mode-boost-feature-053), [features/page-dashboard.md](../features/page-dashboard.md#carte-modes-temporaires-feature-140) et [ADR-0025](../adr/0025-mode-boost.md).

## Champs `sensor_data` ajoutés en feature-055 (effet Boost persistant, v2.18.2)

| Champ | Type | Description |
|-------|------|-------------|
| `boost_filtration_extended` | bool | `true` ssi la filtration est gérée par PoolController (= `filtrationCfg.enabled`) : le levier « filtration prolongée » du Boost s'applique effectivement. |
| `boost_chlorine_boosted` | bool | `true` ssi la régulation ORP est en mode `automatic` (= `orpRegulationMode == "automatic"`) : le levier « surchloration » (cible/limite chlore relevées) s'applique effectivement. |

Ces deux booléens reflètent les **leviers réellement actifs** du Mode Boost et sont **calculés au vol** dans `_buildSensorJson()` à chaque cycle (indépendants de `boost_active`) : ils sont donc valides après une activation depuis Home Assistant **et** après un rechargement de page — contrairement aux booléens `filtration_extended` / `chlorine_boosted` de la seule réponse HTTP `POST /boost/start` (feature-054). L'UI (`updateBoostCard`, [page-dashboard.md](../features/page-dashboard.md#carte-modes-temporaires-feature-140)) les utilise pour afficher une ligne « Effet » persistante quand le Boost est actif. Aucune logique de dosage n'est touchée.

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

## ~~Champs `sensor_data` ajoutés en v2.20.3 (alarme filtre bloqué)~~ — RETIRÉS en v2.68.0

⚠️ **`phFilterAlarm`/`orpFilterAlarm` (et `phFilterReady`/`orpFilterReady`,
`phFilterUnstable`/`orpFilterUnstable`) sont RETIRÉS en v2.68.0** (feature-152) — le mécanisme
« filtre non prêt durablement » et son verrou ont disparu (verdict `pool-chemistry` : ce
mécanisme se posait sur du bruit normal, aucun cas où il a démontrablement évité un dommage).
`isnan(ph)`/`isnan(orp)` sur les champs `ph`/`orp` eux-mêmes porte désormais toute
l'information : il n'y a plus d'état « prêt »/« instable »/« alarme » séparé de la valeur.

Les **six** booléens retirés (`phFilterReady`, `phFilterUnstable`, `phFilterAlarm`,
`orpFilterReady`, `orpFilterUnstable`, `orpFilterAlarm`) allègent `_buildSensorJson()` de
**141 octets** au pire cas (recompté, ADR-0073) : `"phFilterReady":false,` = 22 o,
`"phFilterUnstable":false,` = 25 o, `"phFilterAlarm":false,` = 22 o,
`"orpFilterReady":false,` = 23 o, `"orpFilterUnstable":false,` = 26 o,
`"orpFilterAlarm":false,` = 23 o. Pire cas de trame recalculé : **3356 o**, sous
`kWsFrameBufBytes = 3584 o` — **marge résiduelle 228 o**. Voir le décompte complet en
commentaire de [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`_buildSensorJson()`).

Section conservée pour archive (description v2.20.3 à v2.67.x) :

<details>
<summary>Description historique (archivée)</summary>

`phFilterAlarm`/`orpFilterAlarm` (booléens) passaient à `true` quand le filtre restait non
prêt plus de 5 min alors que la filtration tournait et que les mesures étaient représentatives
— plus aucune régulation pH, ni chlore. Source : `sensors.isPhFilterNotReadyAlarm()` /
`isOrpFilterNotReadyAlarm()`. Mêmes champs et mêmes valeurs dans `GET /data`. Distinction
documentée à l'époque : `phFilterReady = false` était banal (warmup, purge),
`phFilterAlarm = true` était une anomalie durable. Budget de payload d'alors : 2112 → 2176
octets côté WS, 1280 → 1344 octets côté `/data`.

</details>

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

> ⚠️ **v2.36.0 (feature-094, [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) — le champ `ph_dosing` change de SENS** (aucun champ ajouté, aucun renommé). Il disait « un débit a été commandé » ; il dit désormais « **une session de dosage est ouverte** » : le verrou de session pH suit la **porte** (décidée sur une moyenne glissante de 10 min de la filtrée) et non plus le débit instantané. Pendant une **prolongation** — durée minimale de 30 min murales, débit plafonné au feedforward puis nul — **`ph_dosing` reste `true` pompe à l'arrêt**, jusqu'à 3 h au pire cas. `orp_dosing` est **inchangé**. La distinction « commandé / effectif » posée ci-dessus reste donc valide et devient **indispensable** : « pompe alimentée » se lit **uniquement** sur `ph_pulse_phase_ms < ph_pulse_ms`. Parité stricte avec `/data` et MQTT (même accesseur `isPhDosing()`).

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
- **Rétrocompat client** : clé `ph_ff_status` **absente** = firmware < 2.28.0 → l'UI masque la ligne entière (afficher « inactif » mentirait sur un firmware sans feedforward). Voir [page-ph.md](../features/page-ph.md#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460).
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

> ⚠️ **Écart de parité PRÉEXISTANT relevé au passage** (non introduit ni corrigé par la feature-088) : la trame `config` ne porte **ni** `regulation_speed`, **ni** l'objet `regulation_defaults` servi par `/get-config`. Suivi dans [bug-089](../../specs/features/done/bug-089-parite-ws-config-champs-manquants.md).
>
> **RÉSOLU en v2.39.0** : `regulation_speed` est **dans la trame** ; `regulation_defaults` reste volontairement HTTP-seul, **dérivable** côté client depuis `pool_size` (désormais poussé) et le cache `regulation_defaults_by_size`. Voir [§ bug-089](#champs-config-ajoutés-en-bug-089--parité-de-la-trame-v2390) et [ADR-0065](../adr/0065-trame-websocket-etat-mutable-pas-constantes.md). Le client de la feature-088 s'en protège de deux façons : `loadConfig()` **frais avant** d'ouvrir le dialogue (les valeurs « actuelles » ne viennent jamais d'une trame partielle) et cache `_regulationDefaults` réinjecté dans une config partielle (le bouton ne clignote pas — leçon du motif « champ WS-seul qui clignote », plusieurs récidives dans ce projet). Détail côté UI : [page-settings.md](../features/page-settings.md#bouton--restaurer-les-valeurs-par-défaut--feature-088-v2310).
>
> **v2.34.0** : `stabilization_delay_min` figurait aussi dans cette liste ; le réglage ayant été **retiré de `/get-config`** (feature-091), il ne relève plus d'un écart de parité — il n'existe plus nulle part.

## Champs `sensor_data` ajoutés en feature-091 — purge adaptative (v2.34.0)

La purge des mesures pH/ORP n'est plus une minuterie mais un **verdict de stabilité** encadré par un plancher de 10 min et un plafond de 90 min ([ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md), détail complet dans [measure-validity.md](measure-validity.md)). Conséquence sur la trame : `measures_stab_remaining_s` **disparaît** (plus de fin datée à l'avance) et **quatre** champs le remplacent.

| Champ | Type | Description |
|-------|------|-------------|
| `measures_stab_elapsed_s` | uint32 | Secondes **écoulées** dans la purge courante ; `0` hors purge. Remplace `measures_stab_remaining_s`. ⚠️ Nom toujours **distinct** de `stabilization_remaining_s` (sas post-**calibration des pompes**). |
| `measures_purge_last_duration_s` | uint32 | Durée de la **dernière purge terminée** (s). `0` si aucune depuis le boot, ou si elle a été dispensée. |
| `measures_purge_last_end` | uint8 | Motif de fin de la dernière purge : `0` aucune, `1` stabilité, `3` dispensée (arrêt d'eau court). ⚠️ **Depuis la v2.62.0** (feature-139, voir section dédiée plus bas) : `2` (plafond, alerte) et `4` (re-purge) ne sont **plus jamais émis** — valeurs réservées, non réattribuées. Valeurs figées par `enum class PurgeEnd` ([`src/measure_validity.h`](../../src/measure_validity.h)). |
| `measures_purge_count` | uint8 | Nombre de **re-purges** (refermetures C1) du cycle d'eau courant ; remis à zéro au front descendant d'eau. Saturé à 255. |

- **Parité `/data` ↔ WS obligatoire** : les cinq champs (`measures_valid` + les quatre ci-dessus) sont écrits dans le **même ordre** et depuis la **même source** (`measureValidity.getSnapshot()`) par [`ws_manager.cpp:649-653`](../../src/ws_manager.cpp) et [`web_routes_data.cpp:194-200`](../../src/web_routes_data.cpp). Motif récurrent du projet (**3 récidives**) : un champ présent d'un seul côté ne disparaît pas, il **clignote** — `latestSensorData` est remplacé par la charge `/data` toutes les 30 s, puis re-rempli par le push WS 5 s plus tard.
- Le champ `circulationSettled` du snapshot n'est **pas** publié (usage firmware seul : autorisation de la branche Programmée, C4).
- Consommateurs UI (à l'époque de la v2.34.0) : segments d'état « Stabilisation / Re-stabilisation depuis N min » ([page-dashboard.md](../features/page-dashboard.md)), ligne « Dernière purge » de Réglages → Régulation ([page-settings.md](../features/page-settings.md)), bandeaux de plafond des pages [pH](../features/page-ph.md) et [ORP](../features/page-orp.md). ⚠️ **v2.62.0 (feature-139)** : les segments « Stabilisation / Re-stabilisation depuis N min » et les bandeaux de plafond sont **retirés** (vocabulaire jugé incompréhensible par l'utilisateur, et le motif `2` qu'ils affichaient n'est plus jamais émis) — voir la section dédiée ci-dessous.

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

## Champ `sensor_data` retiré / ajouté en feature-139 — retrait de l'analyse de stabilité (v2.62.0)

Le gate `pool-chemistry` du 2026-10-08 a établi que le détecteur de stabilité de `measure_validity` (fenêtre glissante de 10 statistiques de bloc, seuils 0,15 pH / 15 mV, re-fermeture C1, plafond 90 min) était un **filtre de transitoire** — un détecteur d'étendue structurellement aveugle à une mesure *durablement* fausse — et non une couche de sécurité. Il est **retiré** ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'[ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)). Conséquence sur la trame :

- **`measures_purge_count` RETIRÉ** — le mécanisme de re-purge supprimé côté firmware rendait ce champ **structurellement constant à 0** ; le conserver figé aurait publié une forme stable qui ment. Seul consommateur du dépôt (`data/app.js`) utilisait déjà `|| 0`, dégradation propre garantie. Octet sérialisé au pire cas retiré : `"measures_purge_count":255,` = **−27 o**. **Marge résiduelle sous `kWsFrameBufBytes` (3584 o) portée de 39 à 66 o.**
- **`measures_purge_last_end` domaine réduit** à `{0, 1, 3}` — `2` (plafond) et `4` (re-purge) ne sont **plus jamais émis** (valeurs réservées, non réattribuées).
- **`measures_dose_available_in_s` AJOUTÉ** (uint32, secondes) — seul endroit de vérité pour le délai avant que le dosage automatique redevienne possible (`PumpController.getDoseAvailableInS()`), remplace un calcul **côté client** faux d'un facteur 10 (il ne regardait que le reste de purge et ignorait le plancher de gel du filtre capteur). Méthode de chiffrage (ADR-0073) : borne **structurelle** (`kPurgeFloorMs` assertée dans [5 ; 30] min, soit jusqu'à 1800 s), pas la valeur observée (600) : `"measures_dose_available_in_s":1800,` = **+36 o**. **Marge résiduelle portée de 66 à 30 o** — ÉTROITE : tout nouvel ajout à cette trame exige désormais un retrait compensatoire ou un relèvement de `kWsFrameBufBytes` (décision d'ADR, pas de routine).
- **Parité `/data` ↔ WS obligatoire**, mêmes clés, même source (`measureValidity.getSnapshot()` + `PumpController.getDoseAvailableInS()`) — voir [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) et [`src/ws_manager.cpp`](../../src/ws_manager.cpp).
- Détail du calcul et des deux mises en garde d'usage (ne promet pas un dosage, omis filtration à l'arrêt) : [measure-validity.md](measure-validity.md#délai-avant-dosage-annoncé-à-lutilisateur-dose_available_logic-feature-139).
- L'alerte MQTT `measures_purge_timeout` est **retirée** en conséquence (voir [MQTT.md](../MQTT.md#alertes)) — le motif `2` qu'elle signalait ne se produit plus.

## Champs `sensor_data` ajoutés en feature-096 — temps d'atteinte de cible (v2.37.0)

**Huit** champs (4 par régulation) exposant le chronomètre du jour de l'indicateur [time-to-target](time-to-target.md) ([ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)). Source unique : `timeToTarget.getSnapshot()` — un instantané mémorisé publié sous spinlock `portMUX`, jamais l'accesseur de `pump_controller`.

| Champ | Type | Description |
|-------|------|-------------|
| `ph_ttt_state` / `orp_ttt_state` | uint8 | `0` pas de départ aujourd'hui, `1` en cours, `2` atteinte, `3` non atteinte. **4-255 réservés** (un firmware plus récent doit rester lisible). |
| `ph_ttt_min` / `orp_ttt_min` | uint16 | **Minutes de circulation**, bornées à 1440 par le module pur. ⚠️ **Sens variable** : état `1` = minutes écoulées, `2` = temps d'atteinte, `3` = durée totale. |
| `ph_ttt_e0` / `orp_ttt_e0` | float \| `null` | Écart **initial signé** à t0. pH **2 décimales**, ORP **1**. `null` = pas de départ — jamais `0` (le firmware transporte `NAN`, `nullptr` à la sérialisation). |
| `ph_ttt_flag` / `orp_ttt_flag` | uint8 | **Champ de bits** : `0x01` consigne modifiée, `0x02` boost, `0x04` stabilisation non atteinte, `0x08` coupure secteur, `0x10` régulation non automatique. Encodage **figé** (il est aussi archivé en NVS). Un client teste des **bits**, jamais l'égalité. |

- **Parité `/data` ↔ WS obligatoire** : les huit champs sont écrits dans le **même ordre**, avec les **mêmes arrondis**, depuis la **même source**, par [`ws_manager.cpp:694-702`](../../src/ws_manager.cpp) et [`web_routes_data.cpp:427-435`](../../src/web_routes_data.cpp). Motif récurrent du projet (**3 récidives**) : un champ servi d'un seul côté ne disparaît pas, il **clignote**.
- La **mémoire 7 jours** n'est **pas** poussée en WS : elle vient de [`GET /get-kpi`](../API.md#get-get-kpi--write), relue à l'entrée dans les vues pH/ORP puis au changement d'état ou de jour — jamais en polling (elle n'est écrite que deux fois par jour et par régulation).
- Consommateurs UI : ligne « Atteinte de la cible » et bande de 7 pastilles des pages [pH](../features/page-ph.md#atteinte-de-la-cible-feature-096-v2370) et [ORP](../features/page-orp.md#atteinte-de-la-cible-feature-096-v2370). Le **tableau de bord n'est pas touché**.

**Budget de payload** : **+150 o** au pire cas, décompté clé par clé (guillemets + deux-points + valeur + virgule). Décompte **identique** à celui de `web_routes_data.cpp` — mêmes clés.

| Champ | Sérialisation pire cas | Octets |
|-------|------------------------|--------|
| `ph_ttt_state` | `"ph_ttt_state":3,` | 17 |
| `ph_ttt_min` | `"ph_ttt_min":1440,` | 18 |
| `ph_ttt_e0` | `"ph_ttt_e0":-14.00,` | 19 |
| `ph_ttt_flag` | `"ph_ttt_flag":255,` | 18 |
| `orp_ttt_state` | `"orp_ttt_state":3,` | 18 |
| `orp_ttt_min` | `"orp_ttt_min":1440,` | 19 |
| `orp_ttt_e0` | `"orp_ttt_e0":-1000.0,` | 21 |
| `orp_ttt_flag` | `"orp_ttt_flag":255,` | 20 |

Pire cas de trame **2 842 → 2 992 o**, au-dessus du gabarit documentaire 2944 → le gabarit `StaticJson<>` de `_buildSensorJson()` passe de **2944 à 3072**. **Marge résiduelle sous le seul plafond DUR** — `sWsFrameBuf`, `kWsFrameBufBytes` = **3584** o : **3584 − 2992 = 592 o** (elle était de 742 o avant cet ajout). **Aucun bump de `kWsFrameBufBytes` n'est requis.** Si un jour la trame dépassait, la garde `measureJson(doc) + 1 > cap` **saute** le push et incrémente `_oversizedFrames` (warn throttlé) — elle ne **tronque** jamais.

> Côté `/data`, les mêmes champs tiennent **sans bump** : ce chemin n'a aucun plafond dur (`sendJsonResponse` dimensionne la `String` à la demande). Rappel ArduinoJson 7 : `StaticJson<N>` dérive de `JsonDocument`, `N` n'est plus qu'une valeur documentaire (`capacity()`).

## Champs `config` ajoutés en bug-089 — parité de la trame (v2.39.0)

`/get-config` servait **89** clés, la trame WebSocket `config` seulement **66**. Après un changement de configuration, les **autres** clients connectés ne voyaient donc pas ces champs se rafraîchir avant leur prochain `/get-config`. C'est la **4ᵉ occurrence documentée** du motif de parité dans ce projet.

**Le correctif évident aurait été pire que le bug.** Ajouter les 23 clés manquantes coûtait **+1 365 o** sur une trame dont le plafond est **dur** : un dépassement fait **sauter le push** — la garde `measureJson(doc) + 1 > cap` ne tronque pas, elle **abandonne** la trame. On aurait remplacé des champs manquants par **une trame absente pour tous les clients**.

### Le critère de partage (il contraint tous les ajouts futurs)

> **La trame porte l'état mutable à l'exécution et non dérivable ; tout le reste est HTTP-seul et déclaré.**

Voir [ADR-0065](../adr/0065-trame-websocket-etat-mutable-pas-constantes.md). Application : **15 clés ajoutées**, **8 déclarées en exception**.

### Les 15 clés ajoutées à `_buildConfigJson()`

| Champ | Type | Pourquoi il est mutable à l'exécution |
|-------|------|----------------------------------------|
| `max_cycles_per_day` | integer | Garde anti-cycling (feature-060), réglable depuis l'onglet Régulation |
| `regulation_speed` | string | Vitesse de correction — réglage utilisateur |
| `pool_size` | string | Taille de bassin (feature-090) ; c'est aussi la clé qui rend `regulation_defaults` **dérivable** côté client |
| `ph_auto_manual_epoch` | integer (epoch) | Instant de la bascule **automatique** de la régulation pH en manuel (escalade de sécurité C5, v2.20.3) — écrit **par le firmware**, sans action utilisateur |
| `ph_auto_manual_reason` | string | Motif littéral de cette bascule, produit par `setPhAutoManualTrace()` ([`src/pump_controller.cpp`](../../src/pump_controller.cpp)) — **jamais** une saisie utilisateur |
| `ph_tracking_enabled` / `orp_tracking_enabled` | boolean | Suivi de produit activé |
| `ph_container_ml` / `orp_container_ml` | float | Volume du bidon |
| `ph_total_injected_ml` / `orp_total_injected_ml` | float | **Avance à chaque injection** — l'état le plus mutable de toute la config |
| `ph_remaining_ml` / `orp_remaining_ml` | float | `max(0, container − total_injected)`, calculé au vol |
| `ph_alert_threshold_ml` / `orp_alert_threshold_ml` | float | Seuil d'alerte de bidon bas |

Sans ces clés, la **jauge de bidon** et l'**escalade de sécurité pH** des clients tiers restaient gelées jusqu'à leur prochain `/get-config`.

### Les 8 exceptions déclarées (clés HTTP-seules)

Chacune est inscrite dans la table `kHttpOnlyKeys` de la sentinelle **avec un motif écrit**, et le motif est **asserté** (≥ 20 caractères).

> Depuis feature-110 (v2.45.0), la table compte **12 entrées** : les 8 ci-dessous plus les 4 compteurs d'hygiène du relevé de bidon — voir [feature-110 — compteurs de démarrages de pompe](#feature-110-v2450--compteurs-de-démarrages-de-pompe-ph_starts--orp_starts).

| Clé HTTP-seule | Motif déclaré |
|----------------|---------------|
| `ph_target_min` / `ph_target_max` / `ph_target_step` | **Constantes de compilation** (`kPhTarget*`) : immuables à l'exécution ; `app.js` conserve la dernière borne reçue |
| `ph_offset_min` / `ph_offset_max` / `ph_offset_step` | **Constantes de compilation** (`kPhOffset*`), même raison |
| `regulation_defaults_by_size` | **Table de compilation** (`kRegulationDefaultsBySize`, 3 colonnes) : immuable **et** ~525 o de trame **par push** |
| `regulation_defaults` | **Dérivable côté client** : `pool_size` est désormais dans la trame et `by_size` est en cache (`syncRegulationDefaults()`, [`data/app.js`](../../data/app.js)). L'extraire de `fillRegulationDefaults` coûterait **+703 o** de trame pour une valeur calculable |

> Le client reste **inerte** quand une de ces clés manque : `_applyPhBounds()` ignore **en bloc** un lot de bornes absent ou incohérent et conserve les bornes déjà connues (asymétrie déjà documentée en [v2.25.0](#champ-config-ajouté-en-feature-067-offset-logiciel-ph-v2200)). `syncRegulationDefaults()` **reconstruit** la colonne courante depuis `pool_size` + cache `by_size` au lieu de resservir un cache périmé après un changement de taille reçu par WebSocket.

### Budget de trame — décompte rectifié en revue

Octets **sérialisés** au pire cas, comptés clé par clé (guillemets + deux-points + valeur + virgule) :

| Champ | Sérialisation pire cas | Octets |
|-------|------------------------|--------|
| `max_cycles_per_day` | `"max_cycles_per_day":60,` | 24 |
| `regulation_speed` | `"regulation_speed":"normal",` | 28 |
| `pool_size` | `"pool_size":"medium",` | 21 |
| `ph_auto_manual_epoch` | `"ph_auto_manual_epoch":4294967295,` | 34 |
| `ph_auto_manual_reason` | `"ph_auto_manual_reason":"…"` (427 o) | **452** |
| `ph_tracking_enabled` | `"ph_tracking_enabled":false,` | 28 |
| `ph_container_ml` | `"ph_container_ml":123456.78,` | 28 |
| `ph_total_injected_ml` | `"ph_total_injected_ml":123456.78,` | 33 |
| `ph_remaining_ml` | `"ph_remaining_ml":123456.78,` | 28 |
| `ph_alert_threshold_ml` | `"ph_alert_threshold_ml":123456.78,` | 34 |
| `orp_tracking_enabled` | `"orp_tracking_enabled":false,` | 29 |
| `orp_container_ml` | `"orp_container_ml":123456.78,` | 29 |
| `orp_total_injected_ml` | `"orp_total_injected_ml":123456.78,` | 34 |
| `orp_remaining_ml` | `"orp_remaining_ml":123456.78,` | 29 |
| `orp_alert_threshold_ml` | `"orp_alert_threshold_ml":123456.78,` | 35 |
| **Sous-total bug-089 (15 clés)** | | **+866** |
| `ph_starts` (feature-110, v2.45.0) | `"ph_starts":4294967295,` | 23 |
| `orp_starts` (feature-110, v2.45.0) | `"orp_starts":4294967295,` | 24 |
| **Total** | | **+913** |

Trame complète (**83 clés**, format compact) : **2 100 o en nominal**, **≈ 2 995 o au pire cas** → **marge ≈ 589 o** sous le seul plafond **dur**, `sWsFrameBuf` (`kWsFrameBufBytes = 3584` o). Gabarit documentaire `StaticJson<>` porté **2560 → 2944** (bug-089), puis **2944 → 3008** (feature-110) ; rappel ArduinoJson 7 : `N` n'est plus qu'une valeur documentaire (`capacity()`), ce bump coûte **0 octet de RAM**. **Aucun dépassement, aucun bump de `kWsFrameBufBytes`.**

> Chiffres **courants** (v2.45.0). Les valeurs de la v2.39.0 — 81 clés, 2 053 o nominal, ≈ 2 948 o au pire cas, marge ≈ 636 o — sont **périmées** : feature-110 a ajouté `ph_starts` (23 o) et `orp_starts` (24 o), soit **+47 o**. La ligne à lire pour arbitrer un ajout est **marge ≈ 589 o**.

> ⚠️ **Chiffre rectifié en revue — facteur 2,7.** Le bloc de dimensionnement annonçait **167 o** pour `ph_auto_manual_reason` (« ~140 caractères »). Les chaînes réelles ont été reconstruites depuis leur **unique écrivain**, `setPhAutoManualTrace()` : la branche « Diagnostic sonde EZO NORMAL » fait **411 caractères / 427 octets** UTF-8, la branche « hors norme » 325 caractères / 340 o. Coût réel **452 o**, total **+866 o**, marge **636 o** — et non 921. Le chiffre compte : c'est lui que lira le prochain contributeur pour arbitrer son ajout ; avec l'ancienne valeur il aurait cru disposer de 285 o qui n'existent pas. Et **le pire cas de longueur coïncide avec l'escalade de sécurité pH** — précisément le moment où perdre la trame coûterait le plus cher. Ne jamais estimer une longueur ici : la seule source de vérité est le littéral du firmware.
>
> **Réserve connue** : `server`, `topic`, `username` et `wifi_ssid` ne sont bornés **nulle part** (`/save-config` ne valide aucune longueur) ; la marge est prouvée sous l'hypothèse d'environ 64 o par champ. Surveiller `oversizedFrameCount()`.

### La sentinelle de parité — `test/test_native_ws_config_parity/`

Le motif en est à sa 4ᵉ occurrence. À chaque fois, la leçon retenue était « penser à vérifier des deux côtés » : une **discipline orale**, donc oubliée quatre fois. La sentinelle la remplace par une garde qui ne dépend de la vigilance de personne. Coût : **zéro octet de flash** (test natif, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).

Ce n'est pas un test de comportement mais un test **structurel** : il lit le **texte source** des deux fichiers entre des **marqueurs commentés** — `>>> PARITE-CONFIG:DEBUT` et `<<< PARITE-CONFIG:FIN` — et non des bornes de fonction, qui casseraient au premier refactor. Convention normative lue telle quelle : receveur `doc[` dans [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp), receveur `d[` dans [`src/ws_manager.cpp`](../../src/ws_manager.cpp). **Une clé posée hors des marqueurs est invisible pour la sentinelle.**

Quatre assertions :

1. **Clé HTTP non couverte → échec.** Toute clé servie par `/get-config` est soit dans la trame WS, soit déclarée dans `kHttpOnlyKeys`.
2. **Clé WS-seule → échec.** C'est le sens **dangereux** du motif : un champ WS-seul ne disparaît pas, il **clignote** — la charge `/get-config` remplace périodiquement l'objet de config côté client et l'efface, la trame WS le remet. Une clé HTTP-seule peut être déclarée ; une clé WS-seule, **jamais**.
3. **Marqueur absent → échec explicite.** Sans marqueur, aucune clé n'est comparée et le test devient aveugle : le message dit « restaurer le marqueur, **ne pas désactiver le test** ». Un plancher de 60 clés extraites de chaque côté sert de contrôle négatif (un extracteur qui ne trouve rien passerait toutes les autres assertions).
4. **Exception morte → échec.** Une clé exemptée qui n'existe plus côté HTTP fait échouer le test. **C'est elle qui empêche la liste de pourrir** : sans elle, la table serait devenue en un an un cimetière de justifications périmées. Corollaire symétrique : une exception **contredite** (clé finalement poussée en WS) échoue aussi.

Deux gardes complémentaires : chaque exception porte un **motif écrit d'au moins 20 caractères** (asserté, doublons interdits), et le décompte des clés HTTP-seules constatées dans le code doit être **exactement** celui de la table.

> ⚠️ **Angle mort assumé et documenté** : l'extraction est **textuelle**. Une clé mise en **commentaire** entre les marqueurs reste comptée comme présente. La sentinelle vise **la distraction, pas la malveillance** — c'est le bon périmètre, et parser du C++ ici serait un remède pire que le mal.

**Que faire quand elle casse** : ajouter la clé du côté manquant **entre les marqueurs** (en vérifiant la marge de trame), ou l'inscrire dans `kHttpOnlyKeys` avec un motif écrit. Jamais la désactiver.

## feature-162 (v2.79.0) — `lighting_mode`, seule autorité de l'éclairage

Une clé ajoutée à la trame `config`, **entre les marqueurs de parité** :

| Champ | Type | Pourquoi il est mutable à l'exécution |
|-------|------|----------------------------------------|
| `lighting_mode` | string (`on`/`off`/`schedule`) | Remplace le triplet `enabled`/`scheduleEnabled`/l'ancien forçage manuel **caché** (jamais publié avant ce lot) — seule autorité de l'état d'éclairage, réglable depuis la carte d'accueil **et** la page de réglage. |

`lighting_enabled` et `lighting_schedule_enabled` restent servis des **deux côtés** (déjà présents dans la trame avant ce lot) mais changent d'**expression** : dérivés de `lighting_mode` (`mode == "on"` / `mode == "schedule"`) plutôt que de deux booléens séparés. ⚠️ **C'est exactement l'angle mort qu'une comparaison par simple présence de clé ne voit pas** (4ᵉ récidive du motif de parité, dont une forme déjà invisible au grep sur `sensor_data`) : une clé présente des deux côtés mais calculée différemment ferait **osciller** l'affichage au rythme des deux cadences de rafraîchissement (`/get-config` périodique vs push WS), sans qu'aucune charge isolée ne paraisse fausse. Couvert par une sentinelle **dédiée**, vérifiant l'**expression**, pas seulement la clé : `test_h_lighting_expression_partagee_identique_des_deux_cotes` (`test/test_native_ws_config_parity/test_ws_config_parity.cpp`), même technique d'extraction de texte source que la sentinelle générique ci-dessus mais restreinte à la région des trois clés citées.

Détail firmware complet : [docs/subsystems/lighting.md](lighting.md).

## feature-093 (v2.34.0) — persistance de l'intégrale pH : AUCUN champ nouveau

La restauration de l'intégrale pH au démarrage ([pump-controller.md](pump-controller.md#3-persistance-nvs-conditionnelle-r1-r7)) ne publie **rien de neuf** : les champs `ph_integral`, `ph_integral_term`, `ph_integral_pct` et `ph_integral_saturated` (v2.26.0) suffisent — une valeur restaurée s'y lit comme n'importe quelle autre. Toute la traçabilité (motif de rejet, âge en temps de filtration, motif d'écriture) passe par le **journal système**, donc par `broadcastLog`.

## feature-110 (v2.45.0) — compteurs de démarrages de pompe (`ph_starts` / `orp_starts`)

Deux clés ajoutées à la trame `config`, **entre les marqueurs de parité** :

| Champ | Type | Pourquoi il est mutable à l'exécution |
|-------|------|----------------------------------------|
| `ph_starts` | `uint32` | Démarrages de pompe pH **crédités** depuis le dernier « Nouveau bidon ». Avance en continu **à côté** de `ph_total_injected_ml` (même branche de code, même garde) |
| `orp_starts` | `uint32` | Idem pour la pompe chlore |

Elles satisfont le critère de partage ([ADR-0065](../adr/0065-trame-websocket-etat-mutable-pas-constantes.md)) : état mutable à l'exécution, non dérivable côté client. Coût **+47 o** au pire cas (uint32 saturé) — voir le [budget de trame](#budget-de-trame--décompte-rectifié-en-revue).

### Les 4 compteurs d'hygiène restent HTTP-seuls

`ph_anom`, `orp_anom`, `ph_anom_ml` et `orp_anom_ml` sont **délibérément absents** de la trame et **déclarés** dans `kHttpOnlyKeys` (motif écrit, asserté) : ils qualifient un **relevé de bidon**, lu **une fois par campagne**, et ne pilotent aucun affichage continu. Les pousser toutes les 5 s coûterait ~120 o de trame pour zéro usage temps réel.

Conséquence pour un client : ces quatre clés ne s'obtiennent que par [`GET /get-config`](../API.md#get-get-config--write). C'est pourquoi le bouton « Nouveau bidon » de la [page Produits](../features/page-dosages.md) **recharge la configuration par HTTP** avant d'afficher sa confirmation — `window._config` peut venir d'une trame WS, où l'hygiène n'est jamais présente.

**Ce que la sentinelle garantit ici** : `ph_starts` / `orp_starts` ne peuvent pas devenir WS-seules (sens dangereux, elles **clignoteraient**), et les 4 exceptions ne peuvent pas survivre à la disparition de leur clé HTTP (assertion « exception morte »).

## Champs `sensor_data` ajoutés en feature-092 — composition du débit d'injection (v2.46.0)

> ⚠️ Lire d'abord [Deux trames, deux gabarits — un seul plafond DUR](#deux-trames-deux-gabarits--un-seul-plafond-dur-à-lire-avant-tout-chiffrage). Les chiffres ci-dessous sont des **pires cas structurels**, jamais des mesures vivantes.

**Huit** champs exposant l'équation que la régulation applique déjà — *demande = dose de base apprise + écart courant + rattrapage accumulé, bornée* — servis **à l'identique** sur `GET /data` et la trame `sensor_data`, dans le **même passage** de code, entre les marqueurs `PARITE-DATA`. Source unique : les [accesseurs de composition](pump-controller.md#composition-du-débit-dinjection--accesseurs-de-lecture-seule-feature-092-v2460) de `pump_controller`, copiés sous spinlock `portMUX`.

| Champ | Type | Description |
|-------|------|-------------|
| `ph_ff_learned_ml_per_min` | float \| `null` | Dose de base **apprise** = `min(charge médiane 7 j, 60 % du soutenable)`. Calculée **à la lecture**, donc vraie hors session — contrairement à `ph_ff_ml_per_min`, qui vaut 0 hors session. `null` sous 3 journées exploitables ou pompe non caractérisée ; `0,00` avec 3 journées sans injection est une **observation**. |
| `ph_p_term_ml_per_min` | float \| `null` | Écart courant `kp·e`, **SIGNÉ** (négatif = mesure sous la consigne), `0` dans la bande morte, **jamais borné**. `null` si la mesure n'est pas représentative, ou si `kp = 0`. |
| `ph_i_term_ml_per_min` | float \| `null` | Rattrapage **servi** (`0` dans la bande morte). **Distinct** de `ph_integral_term`, terme brut et contrat externe historique. |
| `ph_demand_ml_per_min` | float \| `null` | Demande **totale bornée** = `clamp(PI servi + FF, 0, débit d'amplitude)`. |
| `orp_p_term_ml_per_min` | float \| `null` | Idem pH côté chlore. |
| `orp_demand_ml_per_min` | float \| `null` | Demande ORP. Régulation **P-pure** (Ki = 0, aucun feedforward) : la demande **vaut** le terme proportionnel borné. |
| `orp_sustainable_ml_per_min` | float \| `null` | Plafond de débit moyen de la voie ORP imposé par sa limite horaire — miroir de `ph_sustainable_ml_per_min`. |
| `orp_limited_by_hourly_budget` | bool | **Verdict** firmware « le chlore est borné par sa limite horaire en ce moment » — homologue d'`ph_limited_by_hourly_budget`. Jamais `null`, jamais une comparaison laissée au client. |

⚠️ **Il n'existe AUCUN champ « appliqué ».** Voir [API.md](../API.md#champs-de-composition-du-débit-dinjection-feature-092-v2460) : le tick borne à l'**amplitude de la pompe**, jamais au débit soutenable.

### Budget de trame — décompte rectifié (v2.40.0 ; la marge de 137 o est retombée à **67 o** puis **43 o** en v2.47.1)

Octets **sérialisés au pire cas structurel**, comptés clé par clé (guillemets + deux-points + valeur + virgule). Valeurs signées comptées sur 8 caractères (`-1234.56`) : les termes P et I ne sont **pas** bornés, c'est leur raison d'être ; grandeurs ≥ 0 sur 6 (`123.45`) ; `null` = 4 o, donc jamais dimensionnant.

| Champ | Sérialisation pire cas | Octets |
|-------|------------------------|-------:|
| `ph_ff_learned_ml_per_min` | `"ph_ff_learned_ml_per_min":123.45,` | 34 |
| `ph_p_term_ml_per_min` | `"ph_p_term_ml_per_min":-1234.56,` | 32 |
| `ph_i_term_ml_per_min` | `"ph_i_term_ml_per_min":-1234.56,` | 32 |
| `ph_demand_ml_per_min` | `"ph_demand_ml_per_min":123.45,` | 30 |
| `orp_p_term_ml_per_min` | `"orp_p_term_ml_per_min":-1234.56,` | 33 |
| `orp_demand_ml_per_min` | `"orp_demand_ml_per_min":123.45,` | 31 |
| `orp_sustainable_ml_per_min` | `"orp_sustainable_ml_per_min":123.45,` | 36 |
| `orp_limited_by_hourly_budget` | `"orp_limited_by_hourly_budget":false,` | 37 |
| **Sous-total — 8 champs de composition** | | **+265** |
| `ph_pump_flow_uncharacterized` | `"ph_pump_flow_uncharacterized":false,` | 37 |
| `orp_pump_flow_uncharacterized` | `"orp_pump_flow_uncharacterized":false,` | 38 |
| **Sous-total — correctif de parité (2 booléens)** | | **+75** |
| **Correction de méthode** — `phDoseBlockedReason` / `orpDoseBlockedReason` recomptés au **pire cas structurel** | (aucun champ ajouté) | **+115** |
| **Total** | | **+455** |

Les **4** champs de pause mélange (`phMixingDelayActive` 28 o, `orpMixingDelayActive` 29 o, `ph_mix_remaining_s` 32 o, `orp_mix_remaining_s` 33 o = **122 o**, `uint32` comptés sur 10 chiffres — borne du **type**, pas la borne fonctionnelle de 900 s) **n'ajoutent rien à cette trame** : ils y existaient déjà depuis la v2.19.1. C'est `/data` qui rattrape (voir l'axe de parité ci-dessous).

**Pire cas de trame : 2 992 (décompte v2.37.0) + 455 = 3 447 o**, au-dessus du gabarit documentaire 3072 → gabarit porté **3072 → 3456** (8 o sous le gabarit ; **0 octet de RAM**, cf. section normative).

> ⚠️ **MARGE SOUS LE SEUL PLAFOND DUR** — `sWsFrameBuf`, `kWsFrameBufBytes = 3584` o : **3584 − 3 447 = 137 o** (⚠️ **valeur de la v2.40.0** ; `bug-082` a dépensé 70 o de plus, la marge courante est de **67 o** — voir la [section v2.47.1](#champs-sensor_data-ajoutés-en-v2471--parité-de-flow_assist-bug-082)). C'est **sous la barre des 150 o**, et c'est dit ici plutôt qu'absorbé : le décompte v2.37.0 annonçait 592 o de marge, dont **115 n'ont jamais existé**. **137 o, c'est quatre clés flottantes.** Tout nouveau champ de cette trame exige désormais un **retrait compensatoire** OU un **relèvement de `kWsFrameBufBytes`** — décision d'ADR, pas de routine.

**Chiffres courants** — **mesurés** le 2026-09-07 sur la v2.47.1 (extraction du receveur `d[` dans `_buildSensorJson` et `doc[` dans `handleGetData`, corps de fonction entiers, même convention que la sentinelle) : **121** clés dans `sensor_data`, **97** dans `GET /data`, **26** WS-seules, **2** `/data`-seules. L'écart avec la ligne précédente (v2.46.0 : 119 / 96 / 26 / 3) n'a **pas** été imputé clé par clé ; seule la baisse des `/data`-seules l'est : `flow_assist` est devenue **bilatérale** (`bug-082`). Ne pas recopier ces nombres ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md)) — la sentinelle les remesure à chaque `pio test`. **Remesuré le 2026-09-07 après feature-068**, même méthode (corps de `_buildSensorJson` et de `handleGetData`, code **dépouillé de ses commentaires**, clés uniques) : **121 / 97 / 26 / 2**. ⚠️ Le même comptage sur l'arbre **d'avant** feature-068 donne **120 / 96** : le quadruplet ci-dessus décrit donc l'état **avec** `tempFrozenAlarm`, et l'attribution « v2.47.1 / bug-082 » de la phrase précédente est à lire comme « v2.47.1, lot complet ». `tempFrozenAlarm` étant **bilatéral**, il ne déplace ni les WS-seules ni les `/data`-seules.

### Élagage chiffré, NON appliqué — et pourquoi il a été refusé

`ph_integral` (pH·s) et `ph_integral_term` n'ont plus **aucun consommateur côté client** depuis feature-092 (la carte lit `ph_integral_pct` et `ph_i_term_ml_per_min`). Les retirer de **cette trame seulement** — en les gardant sur `/data`, sur MQTT et donc dans Home Assistant, où ils sont un contrat externe — rendrait :

| Clé | Sérialisation pire cas | Octets |
|---|---|---:|
| `ph_integral` | `"ph_integral":-43200.0,` | 23 |
| `ph_integral_term` | `"ph_integral_term":-6.000,` | 26 |
| **Total rendu** | | **49** |

soit une marge portée de 137 à **186 o** — ⚠️ **base périmée depuis `bug-082`** (v2.47.1, même journée) : la marge de départ est désormais de **67 o**, et le levier la porterait à **116 o**. **Refusé le 07/09/2026**, pour deux motifs à lire **avant toute nouvelle tentative** :

1. les deux clés deviendraient `/data`-seules, donc **deux exceptions de plus** dans la sentinelle de parité — sur l'axe précis qui vient de produire un défaut réel. Payer en exceptions ce que l'on gagne en octets, **le jour même où l'on installe la garde**, est un mauvais échange ;
2. l'autre voie — relever `kWsFrameBufBytes` — coûterait **+512 o de `.bss`**, donc autant de RAM en moins pour le tas, pendant que [bug-080](../../specs/features/done/bug-080-redemarrage-panic-et-fragmentation-heap.md) documente une famine mémoire **active**.

Le levier reste **valable** pour une future feature qui manquera de place : les **49 o** et les deux clés concernées sont consignés ici pour que le chiffre soit déjà disponible.

### Champs `sensor_data` ajoutés en v2.47.1 — parité de `flow_assist` (bug-082)

L'objet `flow_assist` n'existait que dans [`GET /data`](../API.md#get-data--write) : la trame WS, poussée toutes les 5 s, **remplace `latestSensorData` en entier** (aucune fusion côté client), donc ses champs repassaient à `undefined` en quelques secondes et l'estimation de durée d'injection retombait **en permanence** sur son repli client — **61 / 67 mL/min au lieu des 74 servis**.

**Deux champs seulement** passent en parité, dans un receveur imbriqué nommé **`faObj` des deux côtés** (nom repris à l'identique **exprès** : la sentinelle extrait les champs imbriqués avec **un seul** extracteur textuel) :

| Champ | Type | Règle |
|---|---|---|
| `aborted` | bool | Dernière marche interrompue **par le serveur** (perte de filtration). Jamais `null`. |
| `flow255` | array[2] | Débit majorant à duty 255, `[0]` = pompe 1. **Tableau de 2, jamais un scalaire** : un scalaire retomberait **silencieusement** sur le repli client. |

Les **sept** champs de session (`active`, `pump`, `phase`, `remaining_s`, `purges_left`, `purge_s`, `measure_s`) restent **`/data`-seuls** et déclarés dans `kFlowAssistDataOnly` (voir l'axe de parité ci-dessous). Contrepartie **livrée avec eux** : un `GET /data` **forcé à l'ouverture de l'assistant** — sans lui, `purge_s` / `measure_s` / `purges_left` restaient adossés à des **miroirs client en dur 60 / 120 / 3**, soit le **4ᵉ cas** du même motif de parité réintroduit par le correctif du 5ᵉ.

> ⚠️ **`aborted` n'est pas un affichage, c'est un garde-fou de dosage** — et il était **mort en pratique**. Un débit mesuré sous-estimé agit **DEUX FOIS DANS LE MÊME SENS** : `planPulse` ([`src/pulse_dosing_logic.cpp:45`](../../src/pulse_dosing_logic.cpp)) calcule `pulseMs = periodMs × meanFlow / ampFlow`, donc un `ampFlow` trop petit **allonge le pouls** (sur-injection **réelle**) ; et `pulseDeltaVolumeMl` ([`:161`](../../src/pulse_dosing_logic.cpp)) crédite le plateau au **même** `ampFlow`, donc `dailyInjectedMl` **sous-compte** d'autant et la garde `maxDailyMl` est **desserrée du même facteur**. **La sécurité volumique se ment à elle-même.** Le filet en aval n'est que **partiel** (bornes `0 < flow ≤ 200` : 12 mL/min au lieu de 26 passe sans bruit) ; l'avertissement de vraisemblance 20-35 mL/min ([`data/app.js:9617`](../../data/app.js)) **n'est pas bloquant** — le rendre bloquant est une recommandation du visa `pool-chemistry`, **restée ouverte**.

**Le repli client 61 / 67 reste distinct de 74, et c'est verrouillé par une assertion** (`test_p_…_reste_distinct_de_la_constante_firmware`), pour **deux** motifs :

1. sur firmware **ancien**, la pompe débite réellement 61 / 67 : annoncer 74 **raccourcirait l'estimation de 18 à 21 %** sous la marche réelle → l'utilisateur croit à un défaut et **relance**, la sur-injection venant de la **répétition** ;
2. sur firmware **à jour**, aligner le repli sur 74 rendrait la **présence** du champ **indiscernable de son absence** : ce bug serait invisible pour toujours et une 6ᵉ récidive muette. **Le repli distinct EST le détecteur.**

#### Budget de payload — méthode ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md))

**Pire cas structurel : 70 o.** Les deux bornes sont **vérifiées**, non déduites :

- un élément de `flow255` est un `float` sérialisé par ArduinoJson 7. **Balayage exhaustif des 2³¹ motifs de bits** de `float` finis ≥ 0 (harnais hôte, ArduinoJson des `libdeps` du projet) : longueur maximale **12 o**, atteinte en notation exponentielle (`1.40129846e-45`). Ce n'est **pas** « 9 chiffres significatifs + point » (10 o) : ArduinoJson en rend **7**, mais l'exposant reprend les octets économisés. Le même balayage restreint à la borne fonctionnelle du débit (`0 < f ≤ 200`) donne **le même** résultat ;
- un booléen vaut **`false` au pire** (5 o), plus long que `true` (4 o).

| Fragment sérialisé | Octets |
|---|---:|
| `"flow_assist":{` | 15 |
| `"aborted":false,` | 16 |
| `"flow255":[1.401298e-45,1.401298e-45]` | 37 |
| `},` | 2 |
| **Total pire cas** | **70** |

**50 o en fonctionnement** (`74` = 2 o par élément) — valeur **exacte** et non estimée : `kPumpFlow255MlPerMin` est une `constexpr float`, sa sérialisation est figée à la compilation. On budgète quand même les **70 o** : ADR-0073 impose la borne du **type**, et cette constante est un paramètre de caractérisation de pompe qu'un relevé au banc peut changer.

**Pire cas de trame : 3 447 + 70 = 3 517 o** → gabarit documentaire porté **3456 → 3584** (0 octet de RAM, cf. section normative). **Marge résiduelle sous le seul plafond DUR** (`sWsFrameBuf`, `kWsFrameBufBytes = 3584` o) : **67 o**, contre 137 o avant cet ajout. **67 o, c'est deux clés flottantes** : tout nouveau champ de cette trame exige un **retrait compensatoire** ou un relèvement de `kWsFrameBufBytes` (décision d'ADR, pas de routine). La [réserve de 49 o](#élagage-chiffré-non-appliqué--et-pourquoi-il-a-été-refusé) (`ph_integral`, `ph_integral_term`) **n'est pas dépensée**.

> ⚠️ **Le 3 447 hérité n'a PAS été re-dérivé dans ce cycle** : il est repris tel quel du décompte v2.40.0. C'est **exactement** le genre d'héritage qui a produit quatre chiffrages faux pendant feature-092 — dit ici plutôt qu'absorbé. Un futur cycle qui touche ce budget doit **recompter la trame entière**, pas empiler un delta de plus.

### Champ `sensor_data` ajouté en v2.47.1 — sonde de température figée (feature-068)

| Champ | Type | Description |
|---|---|---|
| `tempFrozenAlarm` | bool | `true` quand la sonde DS18B20 « eau » est **figée** : `kTempFrozenSamples` = **43 200** lectures valides consécutives dans une bande < `kTempFrozenEpsilonC` = **0,03 °C**, soit ≈ **24 h** à 2 s (bug-142, v2.56.1 — était 900 / 30 min). Reflet direct de `sensors.isTemperatureFrozen()`, **valeur instantanée**. |

**En parité stricte avec `GET /data`** — aucune exception déclarée dans la sentinelle. Le champ est **informatif pour le dosage** : il n'inhibe ni le dosage, ni la compensation `RT` ([ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md) — l'erreur de pH induite vaut ≈ 0,001). Il **gate en revanche le planning de filtration** (`filtration.cpp:162` et `:405`, bug-142) : pas une alarme chimique, mais pas inerte non plus.

**Ce qui N'ENTRE PAS dans cette trame, et pourquoi** : les diagnostics de la cascade de compensation (`ezo_comp_t`, `ezo_comp_stage`) partent sur le topic MQTT **retenu** `<base>/diagnostic`, qui a son propre budget. Ce sont des grandeurs **relues à froid**, plusieurs jours après un épisode ; aucun écran ne les lit en direct, et les mettre ici aurait coûté du budget sur le **seul plafond dur** du projet en ouvrant une question de parité pour rien.

#### Budget de payload — méthode ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md))

Borne **structurelle du type**, refaite et non recopiée. Le type est `bool` ; la sérialisation la plus longue d'un booléen ArduinoJson est `false` (**5 o**), pas `true` (4 o). Le nom de clé est un littéral de **15** caractères, compté caractère par caractère (t-e-m-p-F-r-o-z-e-n-A-l-a-r-m) :

| Fragment sérialisé | Décompte | Octets |
|---|---|---:|
| `"tempFrozenAlarm":false,` | 1+15+1+1+5+1 | **24** |

**Contre-mesure** (harnais hôte, ArduinoJson des `libdeps` du projet, clé insérée **entre deux clés factices** pour capter la virgule de séparation — méthode de `bug-082`) : **24 o** avec `false`, 23 o avec `true`. Décompte **vérifié**, pas déduit.

**Pire cas de trame : 3 517 + 24 = 3 541 o.** Le gabarit `StaticJson<3584>` reste suffisant, **aucun bump**. ⚠️ Seul le **delta** a été recompté : le 3 517 hérité de `bug-082` (lui-même adossé au 3 447 de la v2.40.0, **non re-dérivé**) est repris tel quel — même réserve que ci-dessus.

> ⚠️ **MARGE RÉSIDUELLE : 43 o** sous `kWsFrameBufBytes = 3584` (elle était de 67 o). **C'est UNE clé booléenne de plus, pas deux.** Acceptable pour ce lot ; **pas pour le suivant** : le prochain champ de cette trame devra **consommer la réserve de 49 o** (`ph_integral` + `ph_integral_term`, sans consommateur client depuis feature-092 — arbitrage exigeant une exception motivée dans la sentinelle de parité, **toujours pas pris**) **ou** relever `kWsFrameBufBytes` par **ADR**. Rappel du mode de défaillance : le dépassement **saute** le push (garde `measureJson(doc) + 1 > cap`, `_oversizedFrames` + warn throttlé), il ne **tronque** jamais — et le pire cas de longueur **coïncide** avec l'escalade de sécurité pH, donc avec le moment où perdre la trame coûte le plus cher.

### Champs `sensor_data` ajoutés en v2.54.0 — durée du refus de dosage (feature-138), et élagage de `ph_integral`/`ph_integral_term`

Incident fondateur (2026-10-02) : après des pluies diluviennes, les deux voies pH et ORP ont été déclarées instables et **tout dosage automatique s'est arrêté**, chlore compris, au pire moment. Le code de refus (`*DoseBlockedCode`) était déjà publié, mais sa **durée** ne l'était pas — « bloqué » et « bloqué depuis 9 h » n'appellent pas la même réaction, et un relevé sans date avait déjà produit un diagnostic faux côté UI (voir [page-dashboard.md](../features/page-dashboard.md#bandeau--dosage-bloqué--feature-138-v2540)).

| Champ | Type | Description |
|---|---|---|
| `phDoseBlockedForS` | uint32 \| null | Secondes depuis lesquelles `phDoseBlockedCode` est inchangé. `null` **identique à la règle du code** — aucun refus en cours. `0` est une durée valide (refus qui vient de commencer). Clampé à `kDoseBlockedForMaxS` = **86400** (24 h). Calculé par le module pur `doseBlockElapsedS()` ([`src/dose_block_duration_logic.h`](../../src/dose_block_duration_logic.h), ADR-0017) — arithmétique non signée, exacte à travers le repliement de `millis()` (24,8 j). |
| `orpDoseBlockedForS` | uint32 \| null | Équivalent ORP. |

Source : `PumpController.getPhDoseBlockedForS()` / `getOrpDoseBlockedForS()`, lecture seule d'un `uint32_t` membre (`_refusalCodeSinceMs[]`) écrit par un **seul** point d'écriture, `setRefusalCode()` — voir [pump-controller.md](pump-controller.md#durée-du-refus-courant-par-voie-feature-138-v2540). ⚠️ `0` dans `_refusalCodeSinceMs[]` n'est **pas** une sentinelle « pas d'épisode » (contrairement à `_sensorFaultSinceMs`) : la validité est portée par `*DoseBlockedCode != kDoseBlockedCodeNone`, jamais par cette valeur elle-même — 0 est une milliseconde parfaitement normale au boot et après repliement.

#### Budget de payload — méthode ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md))

Borne **structurelle du CLAMP applicatif** (`kDoseBlockedForMaxS = 86400`, 5 chiffres décimaux), **pas** la borne du type C (`uint32_t` irait à 4294967295, 10 chiffres) — c'est ce clamp, pas le type, qu'il faut relire au prochain recomptage :

| Fragment sérialisé | Décompte | Octets |
|---|---|---:|
| `"phDoseBlockedForS":86400,` | 1+17+1+1+5+1 | **26** |
| `"orpDoseBlockedForS":86400,` | 1+18+1+1+5+1 | **27** |

Pire cas d'ajout : **53 o**. `null` (4 o) est plus court, donc jamais dimensionnant.

**Élagage simultané, dans la même trame** (décision utilisateur du 2026-10-04, option (a) — voir [ADR-0086](../adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md)) : `ph_integral` (23 o) et `ph_integral_term` (26 o) sont retirés de **cette seule trame**, soit **−49 o**. Plus aucun consommateur côté client depuis feature-092 (grep vérifié : les clés client sont `ph_integral_pct`, `ph_integral_saturated`, l'id DOM `ph_integral_warning` — trois clés distinctes). Les deux champs restent publiés intacts sur `/data`, MQTT et Home Assistant, où ils sont un contrat externe — exception motivée dans la sentinelle de parité (`test_ws_data_parity`, deux entrées `kDataOnly`, voir plus bas).

**Pire cas de trame : 3541 (hérité de v2.47.1) − 49 (élagage) + 53 (durée) = 3545 o.** Gabarit `StaticJson<3584>` inchangé, **aucun bump**. **Marge résiduelle sous le seul plafond DUR** (`sWsFrameBuf`, `kWsFrameBufBytes = 3584` o) : **3584 − 3545 = 39 o** (elle était de 43 o). L'élagage compense presque entièrement l'ajout, à **4 o** près. Côté `/data`, les deux durées **ne sont pas élaguées** (`ph_integral` / `ph_integral_term` y restent) : charge pire cas 2912 + 53 = 2965 o, gabarit `StaticJson<>` porté de **2944 à 3072** (aucun plafond dur sur ce chemin, marge purement documentaire).

> ⚠️ **Marge résiduelle : 39 o, sous le seul plafond DUR.** C'est **moins d'une clé courte supplémentaire** : le prochain champ de cette trame exige un retrait compensatoire ou un relèvement de `kWsFrameBufBytes` — décision d'**ADR**, pas de routine ([ADR-0086](../adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md) pose désormais explicitement que la trame WS n'est **pas** un miroir de `/data`). Rappel du mode de défaillance : le dépassement **saute** le push (garde `measureJson(doc) + 1 > cap`, `_oversizedFrames` + warn throttlé), il ne **tronque** jamais.
>
> ⚠️ **Correction d'une affirmation fausse portée par le commentaire de `kWsFrameBufBytes`** (`constants.h`) depuis le bump v2.21.1 : il justifiait l'augmentation par « +512 o en `.bss` statique — **hors heap 8 bits**, aucun impact sur la famine mémoire ». C'est **inexact** : le `.bss` est en DRAM, et le tas 8 bits n'est jamais que le **reliquat** de cette même DRAM après soustraction du `.bss`/`.data`. Le `.bss` ne **fragmente** pas (aucun impact sur `max_alloc_8bit`), mais il **réduit bien le total disponible** — c'est ce raisonnement, et non une distinction heap/`.bss`, qui faisait passer l'option « relever `kWsFrameBufBytes` » pour gratuite dans les arbitrages précédents. Commentaire corrigé dans le code ; voir [ADR-0086](../adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md) pour le raisonnement complet.

### Champs `sensor_data` ajoutés en v2.55.0 — Chloration de secours (feature-140), et migration de `filtration_state_source`/`filtration_ext_on` vers `/data`-seul

Incident fondateur identique à la v2.54.0 ci-dessus (pluies diluviennes du 2026-10-02) : le mode Programmée existant permet de continuer à doser le chlore aveuglément, mais ce n'était accessible qu'en mutant `orp_regulation_mode` en NVS, sans borne de temps ni rappel de défaire ce changement. La Chloration de secours (feature-140) en fait une surcouche temporaire auto-expirante, décalquée du Mode Boost — voir [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md) et [pump-controller.md §Chloration de secours](pump-controller.md#chloration-de-secours-feature-140).

| Champ | Type | Description |
|---|---|---|
| `rescue_until` | integer (epoch) \| `0` | Échéance de la Chloration de secours (48 h glissantes depuis l'activation, **pas** minuit local). `0` si inactif. Même convention que `boost_until` : le client calcule le temps restant. |
| `rescue_dosing` | boolean | Verdict **dérivé** « le secours dose-t-il réellement maintenant ? » = `isRescueChlorineActive() && !isOrpPhInhibited()` — **pas** un miroir de `rescue_until > 0` : une activation suspendue par un pH > 7,60 (condition #8 du gate) ne dose rien, et la ligne d'effet combiné du widget « Modes temporaires » doit pouvoir le dire. |
| `rescue_lift` | integer (0-3) | Raison de la **dernière fin** du mode (`RescueLiftReason`, [`config.h`](../../src/config.h)), fenêtre de visibilité glissante de 90 s (`kRescueLiftReasonVisibleMs`, 3× la cadence `/data` — un client qui rafraîchit toutes les ~30 s est garanti de la voir au moins une fois). `0` = aucune fin récente ; `1` = levée automatique (30 min de mesures ORP fiables — **seul** cas où l'annonce « les mesures sont redevenues fiables » est vraie) ; `2` = expiration des 48 h ; `3` = arrêt manuel. Calculée à **chaque** lecture (aucun minuteur dédié à armer/désarmer), retombe à `0` d'elle-même. |

**Élagage simultané, dans la même trame** (même patron qu'en v2.54.0, [ADR-0086](../adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md)) : `filtration_state_source` et `filtration_ext_on` sont retirés de **cette seule trame** — **aucun consommateur dans `data/`** (vérifié par grep). Ils migrent **`/data`-seuls** ([`src/web_routes_data.cpp`](../../src/web_routes_data.cpp)), dérivés de `mqttCfg.installMode` seul et de `filtration.getExternalState()`, **sans** rappeler `filtration.resolveWaterPresence()` en contexte `async_tcp` (lecture non protégée de `state.running`, proscrite sur ce chemin). `wp.source` n'est donc plus lu dans `_buildSensorJson()` ; `wp` reste nécessaire pour `water_present`/`filtration_state_stale`.

#### Budget de payload — méthode ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md))

| Fragment sérialisé | Octets | Sens |
|---|---:|---|
| `"filtration_state_source":"commanded",` (plus longue valeur : `"commanded"`, 9 car.) | **38** | retrait |
| `"filtration_ext_on":false,` | **26** | retrait |
| `"rescue_until":1790000000,` (epoch 10 chiffres, borne structurelle `time_t` 32 bits) | **26** | ajout |
| `"rescue_dosing":false,` | **22** | ajout |

Retrait **64 o**, ajout **48 o**, net **−16 o**. Pire cas de trame : **3545 (hérité de v2.54.0) − 64 + 48 = 3529 o**, sous le gabarit `StaticJson<3584>` (aucun bump) — **marge portée de 39 à 55 o**.

**Complément** (même cycle, retour `web-ui-developer`) : `+1` champ `rescue_lift` — `"rescue_lift":1,` = **16 o** (clé "rescue_lift" = 11 caractères, **pas** 12 ; plage 0-3 tient sur 1 chiffre, jamais dimensionnant). Pire cas de trame : **3529 + 16 = 3545 o**, toujours sous `kWsFrameBufBytes = 3584` (aucun bump).

> **MARGE RÉSIDUELLE SOUS LE SEUL PLAFOND DUR** (`sWsFrameBuf`, `kWsFrameBufBytes = 3584` o) : **3584 − 3545 = 39 o** (elle était de 55 o avant ce complément, 39 o avant toute la feature-140 — l'élagage de 64 o avait plus que compensé les 48 o de `rescue_until`/`rescue_dosing`, et `rescue_lift` consomme exactement ce gain). Tout nouveau champ de cette trame exige un **retrait compensatoire** ou un relèvement de `kWsFrameBufBytes` — décision d'**ADR**, pas de routine.

Côté `/data`, les deux champs migrés **s'ajoutent** (ils n'y existaient pas) et les trois champs secours sont servis à parité stricte — charge pire cas portée de 2965 à 3093 o, gabarit `StaticJson<>` porté de **3072 à 3200** (aucun plafond dur sur ce chemin, marge purement documentaire). Voir [API.md](../API.md#champs-ajoutés-en-feature-140-chloration-de-secours).

**Cliquet `kWsOnlyLiveCeiling` abaissé 18 → 16** ([`test/test_native_ws_data_parity/test_ws_data_parity.cpp`](../../test/test_native_ws_data_parity/test_ws_data_parity.cpp)) : `filtration_state_source` et `filtration_ext_on` quittent `kWsOnlyLive` (la dette inventoriée se réduit, le plafond la suit — il ne peut **jamais** augmenter, seulement baisser quand une clé WS-seule migre ailleurs). `kDataOnly` passe de **4 à 6** entrées : les deux migrées s'ajoutent aux deux `ph_integral`/`ph_integral_term` de la v2.54.0.

### Champs `sensor_data` retirés en v2.63.0 — fin de l'ordre direct de filtration (feature-146)

`filtration_force_on` et `filtration_force_off` **disparaissent** de la trame (et du firmware
entier) avec l'« ordre direct » de filtration (`forceOn`/`forceOff`, minuteur 4 h) qu'ils
reflétaient — voir [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md) et
[filtration.md](filtration.md). **Aucune migration** : contrairement aux élagages précédents
(v2.54.0, v2.55.0), ces deux clés ne se déplacent pas vers `/data`, elles n'ont plus de raison
d'exister nulle part — c'était l'écho d'un état qui n'existe plus.

Octets SÉRIALISÉS au pire cas retirés (méthode [ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md)) :

```
"filtration_force_on":false,  → 28 o
"filtration_force_off":false, → 29 o
```

Retrait total **57 o**. Pire cas de trame : **3554 (décompte v2.62.0) − 57 = 3497 o**, sous le
gabarit documentaire 3584 (aucun bump) et sous le seul plafond dur `kWsFrameBufBytes = 3584` o.

**MARGE RÉSIDUELLE SOUS LE SEUL PLAFOND DUR, `sWsFrameBuf`** : 3584 − 3497 = **87 o** (elle était
de 30 o avant ce retrait). Tout nouveau champ de cette trame exige toujours un retrait
compensatoire OU un relèvement de `kWsFrameBufBytes` (décision d'ADR, pas de routine).

Ces deux clés quittent aussi `kWsOnlyLive` dans la sentinelle de parité (`kWsOnlyLiveCeiling`
abaissé **16 → 14**) — voir le tableau des quatre catégories d'exception plus haut.

### Le second axe de parité — `/data` ↔ `sensor_data` (`PARITE-DATA`)

[ADR-0071](../adr/0071-parite-data-trame-capteurs.md) étend au **second axe** le principe posé par [ADR-0065](../adr/0065-trame-websocket-etat-mutable-pas-constantes.md) pour l'axe `config`. Deux différences de fond :

- **le critère de partage diffère** : la trame capteurs porte de la **mesure**, pas de la configuration. « Mutable à l'exécution et non dérivable » ne discrimine rien ici — tout y est mutable ;
- **le sens du danger est symétrique.** Sur l'axe config, c'est la charge `/get-config` qui écrase un champ WS-seul. Sur l'axe capteurs, le **push WS efface aussi un champ `/data`-seul** : `latestSensorData` est **remplacé en entier** par la dernière charge reçue, sans fusion.

Marqueurs commentés, lus tels quels par la sentinelle : `>>> PARITE-DATA:DEBUT` / `<<< PARITE-DATA:FIN`, dans `handleGetData` ([`src/web_routes_data.cpp:425-510`](../../src/web_routes_data.cpp)) et `_buildSensorJson` ([`src/ws_manager.cpp:879-963`](../../src/ws_manager.cpp)). Receveurs : `doc[` côté HTTP, `d[` côté WS. Receveurs **imbriqués exclus** (`faObj[`, objet `flow_assist`). Le bloc porte **14** clés, ensembles **identiques** des deux côtés (assertion dans les deux sens) : 4 de pause mélange + 8 de composition + 2 de caractérisation de pompe.

**Quatre catégories d'exception** depuis la v2.47.1, et elles sont distinctes pour de bonnes raisons :

| Catégorie | Compte | Règle |
|---|---:|---|
| `kWsOnlyLive` — état vivant servi par **aucune** route HTTP | **14** (était 16) | **Dette inventoriée** (antérieure à feature-092), déclarée pour rendre la sentinelle utilisable, pas pour valider le motif. Sous **cliquet** `kWsOnlyLiveCeiling = 14` (abaissé de 16 en v2.63.0, feature-146) : le compte ne peut **jamais augmenter**, seulement baisser quand une clé WS-seule migre vers `/data` **ou disparaît**. Cette fois, `filtration_force_on`/`filtration_force_off` ne migrent nulle part : ils **disparaissent** du firmware avec l'ordre direct qu'ils portaient — l'exception est retirée de la liste plutôt que neutralisée. |
| `kWsPlusOtherRoute` — clés **dual-sourcées** | **6** (était 8) | Absentes de `/data` mais servies par une **autre** route HTTP, dont le **nom est obligatoire** et asserté non vide : **5** par `GET /get-config` (`lighting_enabled`, `boost_active`, `boost_until`, `install_mode`, `mqtt_connected`) et **1** par `GET /get-logs` (`uptime_ms` — la table de la sentinelle abrège la route en « GET /logs »). Catégorie qui **n'existe pas** sur l'axe config : une règle « aucune clé WS-seule » les signalerait à tort. |
| `kDataOnly` — clés servies par `/data` seul | **6** (était 4) | `ph_pump_amplitude_duty`, `orp_pump_amplitude_duty` (grandeurs de **calibration**, lues dans des vues de diagnostic) ; **+2 en v2.54.0** (feature-138) : `ph_integral`, `ph_integral_term` — élaguées de la **seule** trame WS pour financer `*DoseBlockedForS` ; **+2 en v2.55.0** (feature-140) : `filtration_state_source`, `filtration_ext_on` — élaguées pour financer `rescue_until`/`rescue_dosing` (même méthode, [ADR-0086](../adr/0086-trame-websocket-sous-ensemble-choisi-de-data.md)). Les quatre restent un contrat externe sur `/data`/MQTT/HA. La 3ᵉ entrée d'origine — l'objet `flow_assist` — a été **retirée** en v2.47.1 : elle est devenue bilatérale (`bug-082`), et c'est la sentinelle qui l'a exigé. |
| `kFlowAssistDataOnly` — champs **imbriqués** de `flow_assist` servis par `/data` seul | **7** | Second axe, v2.47.1 (`bug-082`) : `active`, `pump`, `phase`, `remaining_s`, `purges_left`, `purge_s`, `measure_s` — champs de **session**, lus à l'ouverture de l'assistant après un `GET /data` **forcé**. Motif écrit obligatoire, **cliquet à 7**, et **`aborted` / `flow255` y sont formellement interdits**. |

Décompte **vérifié par mesure** et non supposé : les **20** clés WS-seules constatées (v2.63.0, était 22) = 14 « état vivant » + 6 dual-sourcées.

> **`ph_container_ml` / `orp_container_ml` sorties de `kWsPlusOtherRoute` en v2.59.0
> (feature-143).** Elles n'étaient déclarées dual-sourcées (`GET /get-config`) que
> parce que `/data` ne les servait pas encore — **4ᵉ récidive** du motif de parité : le
> raisonnement « c'est un réglage, `/get-config` suffit » ignorait que la page Produits
> recharge sa carte à **chaque** charge `/data`, pas seulement au chargement de la
> config. `/data` les sert désormais directement ([`src/web_routes_data.cpp`](../../src/web_routes_data.cpp)) :
> elles sont captées par la vérification générique (a)-(d) et ne figurent **plus**
> dans aucune table d'exception. Voir [docs/API.md](../API.md#get-data--write) et
> [docs/features/page-dosages.md](../features/page-dosages.md).

#### Le second axe **imbriqué** (v2.47.1, `bug-082`)

`flow_assist` est devenue **bilatérale**, mais son **contenu** ne l'est pas : la trame ne pousse que **2** de ses **9** champs. La parité de premier niveau était donc devenue **aveugle à l'intérieur de l'objet** — et c'est précisément là que se joue le garde-fou `aborted`. Les cas (j) à (p) diffent donc le contenu du receveur `faObj[` **des deux côtés**, avec **un seul** extracteur, sous les mêmes règles que l'axe de premier niveau : motif écrit obligatoire, **cliquet à 7**, exception morte = échec — **plus une interdiction** : `aborted` et `flow255` ne peuvent **jamais** être inscrits dans `kFlowAssistDataOnly`. Le cas (o) vérifie en outre que `flow255` est un **tableau de 2** des deux côtés, et le cas (p) que le repli client reste distinct de la constante firmware.

> **La sentinelle déclarait légitime le bug qu'elle devait signaler.** Livrée le **matin même** ([ADR-0071](../adr/0071-parite-data-trame-capteurs.md)), elle portait `flow_assist` dans `kDataOnly` comme une exception `/data`-seule **légitime** — erreur de l'agent principal du cycle. Quand le firmware a corrigé la parité, elle est passée **rouge d'elle-même** : son mécanisme « **exception morte = échec** » s'est déclenché **pour la première fois**, et son message disait quoi faire (« SUPPRIMER l'entree de kDataOnly »). C'est la deuxième fois en deux cycles que ce mécanisme paie.
>
> **Propriété vérifiée par contre-mesure en revue** : retirer `aborted` de la trame, **le déclarer** en exception **et relever le cliquet** produit **trois** échecs. **On ne peut donc plus détourner la garde pour légitimer le bug.** C'est la leçon transférable de ce cycle : une table d'exceptions n'est une garde que si **ses propres entrées** sont elles aussi sous contrainte.

**Seize cas** (a → p), dont sept sur l'axe de premier niveau : marqueurs présents (échec explicite sinon, message « restaurer le marqueur, **ne pas désactiver le test** ») · jeux de clés identiques entre les marqueurs, dans les deux sens · toute clé WS couverte ou déclarée · toute clé `/data` couverte ou déclarée · **aucune exception morte ni contredite** · motif écrit de **≥ 20 caractères** par exception, sans doublon · **cliquet** sur `kWsOnlyLive` ; les autres couvrent l'exclusion des receveurs imbriqués du diff de premier niveau, les champs de la carte débit, et le **second axe imbriqué** ci-dessus. Coût : **zéro octet de flash** (test natif, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).

> **La sentinelle a attrapé un défaut RÉEL pendant son propre cycle, avant la revue.** Le pied « pompe non caractérisée » de la nouvelle carte **clignotait** : `ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized` n'étaient servis que par `GET /data`, donc valaient `undefined` après chaque push WS (~5 s) et le pied restait masqué (`!== true`) jusqu'à la charge `/data` suivante (~30 s). C'est le **piège n° 1 du projet dans son sens inverse**, et il avait été introduit par une demande de l'agent principal du cycle. Le correctif est la **parité** (migration des deux booléens dans le bloc `PARITE-DATA`), pas une mémorisation côté client. La sentinelle est alors **repassée au rouge une seconde fois**, sur les deux exceptions devenues **mortes** : le mécanisme prévu pour empêcher la table de pourrir a fonctionné du premier coup, sur un vrai changement (`kDataOnly` : 5 → 3 entrées).

**Angle mort assumé** (identique à l'axe config) : l'extraction est **textuelle**. Une clé mise en **commentaire** entre les marqueurs reste comptée comme présente. La sentinelle vise **la distraction, pas la malveillance**.

**Que faire quand elle casse** : ajouter la clé du côté manquant **entre les marqueurs** (en vérifiant la **marge courante de 67 o**), ou l'inscrire dans la table adéquate avec un motif écrit. Jamais la désactiver.

## Champ `sensor_data` ajouté en feature-155 — `phValueRaw`, référence de l'accord pH (v2.71.0)

| Champ | Type | Description |
|---|---|---|
| `phValueRaw` | float \| null | Valeur **officielle** pH (même lissage que `ph`/`phMedian`/`phFiltered` — médiane + EMA), mais en **échelle BRUTE** : `sensors.getPhValueRaw()`, **avant** l'offset logiciel. Arrondi ×1000, `null` si NaN. |

⚠️ **Ce n'est pas `phRaw`.** `phRaw` est la dernière mesure Atlas EZO **instantanée**, non lissée (dispersion ~0,36 pH sur ce bassin, jusqu'à ±0,18 d'écart avec la valeur lissée — deux pas de réglage). `phValueRaw` porte le **même lissage** que `ph`, simplement sans la correction en sortie. C'est la valeur que le firmware utilise comme référence pour calculer l'écart d'un accord sur test manuel ([`POST /tune-ph-offset`](../API.md#post-tune-ph-offset--critical)) — servie ici pour que l'**aperçu client** (`evaluatePhTunePreview()`, `data/app.js`) travaille sur la **même** référence que le calcul serveur, jamais sur `ph`/`phFiltered`/`phMedian` (déjà corrigés de l'offset courant : partir de l'un d'eux ajouterait l'écart demandé à un écart déjà appliqué — défaut invisible à offset 0,00, cf. [ADR-0101](../adr/0101-la-correction-nait-d-une-mesure-jamais-d-un-ecart.md)).

**Parité stricte avec `GET /data`** (même passage, mêmes règles de null — voir [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) et [`src/ws_manager.cpp`](../../src/ws_manager.cpp)) : un champ servi d'un seul côté ne disparaîtrait pas, il **clignoterait** (3 récidives déjà documentées de ce défaut sur cette trame — voir la section « La sentinelle a attrapé un défaut RÉEL… » plus haut).

#### Budget de payload — méthode ([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md))

Borne **structurelle** de la plage physique de la sonde EZO pH (0,001–14,000 — 6 caractères de valeur, jamais plus), `null` (4 o) étant plus court et donc jamais dimensionnant. Clé de 10 caractères :

| Fragment sérialisé | Décompte | Octets |
|---|---|---:|
| `"phValueRaw":14.000,` | 1+10+1+1+6+1 | **20** |

Pire cas de trame WS porté à **3376 o**, sous `kWsFrameBufBytes = 3584` o — marge résiduelle **208 o** (était 228 o avant ce champ). Côté `/data` (gabarit documentaire, pas de plafond dur) : charge portée à **3122 o**, sous le gabarit `StaticJson<3200>` — marge **78 o**.

## Deux nouvelles clés HTTP-seules — `ph_cal_points_min` / `ph_tune_min_filtration_ms` (feature-155, v2.71.0)

`GET /get-config` sert deux nouveaux champs lecture seule consommés par l'**aperçu** de l'accord pH sur un test manuel (`data/app.js`) : `ph_cal_points_min` (`kPhTuneCalPointsMin`, défaut 2) et `ph_tune_min_filtration_ms` (`kPhTuneMinFiltrationMs`, défaut 600000). **Volontairement absents de la trame WebSocket** — même règle, et même motif, que les six bornes `ph_target_*`/`ph_offset_*` et les trois seuils de santé pH (v2.70.0) : ce sont des **constantes de compilation**, invariantes pour un firmware donné, et la marge de trame (208 o, cf. ci-dessus) est mieux dépensée ailleurs. Inscrites dans `kHttpOnlyKeys` de la sentinelle de parité (`test/test_native_ws_config_parity/test_ws_config_parity.cpp`), avec un motif écrit — même traitement que les clés qui les précèdent dans cette table.

Le client conserve la dernière valeur connue entre deux appels à `/get-config`, sans jamais retomber sur un défaut inventé : un repli trop permissif sur ces deux seuils affaiblirait un refus de sécurité (filtration insuffisamment établie, sonde non étalonnée), pas seulement un affichage.

## Champ `sensor_data` documenté a posteriori — `filtration_elapsed_s` (dette soldée, feature-157/feature-158)

| Champ | Type | Description |
|---|---|---|
| `filtration_elapsed_s` | uint32 | Durée écoulée du cycle de filtration courant, en secondes. `0` si à l'arrêt. |

⚠️ **Ce champ existe depuis la v2.73.0 (feature-157), mais n'avait jamais été
documenté** ni ici ni dans [API.md](../API.md#get-data--write) — né dans un
correctif post-livraison sans passage `doc-writer`. Servi à l'identique par
`GET /data` (même accesseur `FiltrationManager::filtrationElapsedS()`, même
valeur — parité par construction, puisque les deux routes lisent le **même**
getter).

## Champs `sensor_data` — bascule de calibration (bug-160)

⚠️ **REFONTE du 2026-10-13 (décision utilisateur)** : un premier mécanisme (bail de suspension,
trois champs `cal_hold_remaining_s` / `cal_hold_product` / `cal_hold_refractory_remaining_s`,
piloté par `POST /calibration/hold`) a été conçu puis **abandonné avant tout déploiement**, au
profit d'une bascule explicite de régulation en `manual` — voir
[pump-controller.md](pump-controller.md#bascule-de-régulation-en-manuel-pendant-une-calibration-bug-160-volet-2)
et [ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md). Les
trois champs `cal_hold_*` ci-dessus n'ont **jamais existé** dans une trame servie — ils
disparaissent de cette doc sans jamais avoir eu besoin d'une dépréciation.

**Deux champs NEUFS**, à parité stricte `/data` ↔ WebSocket (même accesseur `mqttCfg.calSwitch*`
des deux côtés — `src/web_routes_data.cpp` et `src/ws_manager.cpp`) :

| Champ | Type | Description |
|---|---|---|
| `cal_switch_active` | boolean | Une session de bascule est **ouverte** : la régulation pH et/ou ORP a été mise en `manual` à l'entrée d'un écran de calibration et n'a pas encore été restaurée par `POST /calibration/exit`. |
| `cal_switch_epoch` | integer (epoch) | Instant de l'entrée qui a ouvert la session courante. `0` si inactive, ou si l'horloge n'était pas synchronisée à l'entrée (jamais un 1970 trompeur — patron `phAutoManualEpoch`). |

Pilotés par [`POST /calibration/enter`](../API.md#post-calibrationenter--write) /
[`POST /calibration/exit`](../API.md#post-calibrationexit--write). Le mode `manual` lui-même est
**déjà** visible via `phRegulationMode`/`orpRegulationMode` (champs existants) — ces deux champs
ne font que dater et qualifier la bascule.

#### Budget de payload — dette de discipline (le décompte avait décroché pendant quatre lots)

⚠️ **Ce qui s'est réellement passé, à écrire comme un défaut de process, pas comme un détail de
dimensionnement.** La dernière marge **vérifiée dans le code source**
([`src/ws_manager.cpp`](../../src/ws_manager.cpp), commentaires « MARGE RÉSIDUELLE ») datait de
la feature-155 (v2.71.0, 208 o restants sous l'ancien `kWsFrameBufBytes` = 3584 o). **Sept champs
ont été ajoutés à la trame depuis, sans qu'aucun ne soit recompté** : trois pour feature-156
(`ph_offset_epoch`, `ph_cal_epoch`, `ph_offset_stale`), un pour feature-158
(`filtration_elapsed_s`), et les trois `cal_hold_*` envisagés pour `bug-160` (ci-dessus, jamais
livrés). Recomptés **a posteriori**, caractère par caractère
([ADR-0073](../adr/0073-chiffre-consigne-porte-sa-methode.md)) : +213 o au total — soit un pire
cas de trame à **3589 o, 5 octets AU-DESSUS** de l'ancien plafond de 3584.

**Un dépassement ne tronque pas, il fait SAUTER le push en silence** (garde
`measureJson(doc) + 1 > cap` en fin de fonction) : le temps réel aurait cessé de fonctionner
**sans aucune erreur visible**, avec pour seul signe un avertissement throttlé noyé dans le
journal. Cinq octets ont suffi à frôler cet incident, et rien dans le processus de revue des
quatre lots précédents ne l'a intercepté — le commentaire « MARGE RÉSIDUELLE » existe précisément
pour empêcher ça, et il a été **ignoré**, pas absent.

⇒ `kWsFrameBufBytes` porté 3584 → **4096** (`constants.h`) — une marge large, délibérément, pour
ne pas reproduire ce schéma au prochain champ oublié.

**Puis la refonte du même jour** a retiré les trois `cal_hold_*` (jamais servis) au profit des
deux `cal_switch_*` ci-dessus : delta net −100 + 58 = **−42 o**. Pire cas de trame final :
3589 − 42 = **3547 o**, sous les 4096 o (marge résiduelle **549 o**, confortable après le
relèvement — mais la prochaine fois que ce commentaire ne sera plus tenu à jour, la marge réelle
sera à nouveau inconnue, pas forcément confortable).

**La leçon à retenir n'est pas la valeur de la constante** — elle est déjà correcte. C'est que
**« recompter au prochain ajout » n'a pas tenu quatre lots de suite**, en dépit d'un avertissement
explicite dans le code lui-même. Tout ajout futur à cette trame doit relire le commentaire
« MARGE RÉSIDUELLE » le plus récent de [`src/ws_manager.cpp`](../../src/ws_manager.cpp) **et
vérifier sa date/version de référence** avant de supposer qu'il est à jour.

**Ce n'est pas l'uptime de l'appareil**, même si avant la v2.74.0
(feature-158) il s'y confondait de fait : la durée repartait de zéro à chaque
redémarrage logiciel, y compris quand la pompe n'avait jamais cessé de
tourner (1 619 s annoncés contre 1 644 s d'uptime mesurés en production, pour
~5 h de circulation réelle). Depuis la v2.74.0, la durée **persiste à travers
un redémarrage logiciel court** pendant lequel l'eau n'a pas cessé de
circuler — voir [filtration.md](filtration.md#reprise-de-la-durée-de-filtration-à-travers-un-redémarrage-feature-158-v2740)
et [ADR-0104](../adr/0104-la-continuite-de-l-eau-a-un-producteur-unique-et-des-consommateurs.md).
Alimente la garde des 10 minutes de [`POST /tune-ph-offset`](../API.md#post-tune-ph-offset--critical) (refus n°2).
