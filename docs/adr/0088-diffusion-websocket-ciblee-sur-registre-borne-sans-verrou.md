# ADR-0088 — Diffusion WebSocket ciblée sur un registre borné sans verrou

- **Statut** : Accepté
- **Date** : 2026-10-08
- **Décideurs** : `architect`, `security-reviewer`, `code-reviewer` (gate bug-081)
- **Spec(s) liée(s)** : [bug-081](../../specs/features/done/bug-081-websocket-diffuse-aux-clients-non-authentifies.md)

## Contexte

Jusqu'à la v2.56.1, `WsManager::update()` ne vérifiait que « au moins un client
authentifié existe » (`_authenticatedClients.empty()`) avant d'appeler
`_ws->textAll(...)` pour les trois broadcasts (`sensor_data`, `config`, `log`).
`textAll()` diffuse à **tous** les clients connectés à `/ws`, authentifiés ou
non : un client qui ne s'authentifiait jamais recevait donc toutes les trames
dès qu'un *autre* client légitime était connecté — 92 champs en clair (pH, ORP,
volumes injectés, états de dosage). `_authenticatedClients` existait, était
tenu à jour correctement, mais ne servait **jamais** à cibler l'envoi — c'est
le défaut bug-081.

Le correctif doit passer d'une diffusion « à tous les connectés » à une
diffusion « à un ensemble d'identifiants que nous détenons ». La question
structurante n'est pas *s'il faut cibler* (évident) mais **quelle structure de
données porte cette liste**, parce que la boucle d'envoi tourne dans
`loopTask` et que sa radiation (`erase()`) peut être appelée de façon
**ré-entrante, depuis cette même boucle** :

`_queueMessage` (file de la socket pleine, `setCloseClientOnQueueFull(true)`
posé en v2.11.1) → `lock.unlock(); c->close();` → `AsyncClient::_close()`
(`_discard_cb` **synchrone**) → `_handleDisconnect` → destructeur du client →
événement `WS_EVT_DISCONNECT` → notre `_onEvent()` → notre propre
`erase()` — **toujours dans la pile d'appel de la boucle d'envoi qui a
déclenché la file pleine**.

## Décision

Le registre des clients authentifiés est un **tableau borné d'atomiques**
(`WsClientRegistry<N>`, [`src/ws_client_registry_logic.h`](../../src/ws_client_registry_logic.h)),
muté uniquement par `compare_exchange` (`insert`/`erase`) ou par des stores
atomiques simples (`clear`) — **jamais** par un verrou applicatif (mutex
FreeRTOS). La capacité est fixée à la compilation
(`kWsRegistrySlots = kWsMaxClients + 1`, [`src/constants.h`](../../src/constants.h)).

La cible de chaque broadcast est un **snapshot** de ce registre pris sur la
pile de `loopTask`, avant la boucle d'envoi. Chaque identifiant du snapshot est
servi via `AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)` — une
surcharge du fork ESP32Async qui prend son propre verrou privé
(`_ws_clients_lock`, `std::recursive_mutex`) et résout `client(id)`
**sous ce verrou**, en renvoyant `false` si le client a disparu entre-temps.

## Alternatives considérées

- **`std::set<uint32_t>` protégé par un mutex FreeRTOS** (rejetée) — c'est la
  structure qui précédait le correctif (déjà en place, jamais utilisée pour
  cibler). Un mutex applicatif **non récursif** pris pendant l'itération
  d'envoi et repris par `erase()` lors de la ré-entrance décrite ci-dessus
  serait un **auto-interblocage certain**, pas hypothétique : le chemin
  `_queueMessage` → `close()` → `WS_EVT_DISCONNECT` → `erase()` est emprunté
  dès la première file saturée pendant un push, ce qui arrive en usage normal
  (client zombie, cf. v2.11.1).
- **Conteneur dynamique protégé par un mutex récursif** (rejetée) — lève
  l'interblocage mais introduit une **allocation** (`std::set::insert`,
  rééquilibrage d'arbre) potentiellement déclenchée depuis un callback
  `async_tcp` (`_onClientConnect`, `_onData`), contexte à pile réduite où le
  projet s'est déjà fait surprendre par des allocations imprévues
  (bug-badalloc-ws-broadcast, bug-heap-toctou). Rejetée par la même logique
  que l'interdiction d'allouer dans le chemin de journalisation
  (`logger.{h,cpp}`, CLAUDE.md).
- **Itérer `_ws->getClients()` directement, sans registre séparé** (rejetée
  d'emblée, piège déjà documenté v2.11.1) — `_handleDisconnect` (tâche
  `async_tcp`) efface la liste interne sous le verrou **privé** du fork,
  inaccessible côté appelant : itération concurrente = comportement indéfini.
- **Tableau borné d'atomiques, radiation par CAS, snapshot sur la pile**
  (retenue) — aucune section critique applicative donc aucun interblocage
  possible ; header-only, zéro allocation (contrainte module pur ADR-0017) ;
  le snapshot pris avant la boucle d'envoi est une copie immunisée à toute
  mutation concurrente du registre pendant l'itération, y compris la
  ré-entrance qui a motivé ce choix.

## Conséquences

### Positives
- Élimine la divulgation en lecture (bug-081) : un client non authentifié ne
  reçoit plus aucune trame, quel que soit le nombre de clients légitimes
  connectés.
- Aucun risque d'interblocage sur le chemin de fermeture de file saturée,
  contrairement à l'alternative mutex.
- Zéro allocation dans les callbacks `async_tcp` (`insert`/`erase` sont des
  opérations sur tableau fixe).
- Révocation immédiate (`revokeAllAuthenticated()`) possible depuis un handler
  HTTP sans risque de blocage, par simples stores atomiques.

### Négatives / dette assumée
- Le profil mémoire du correctif est **égal** à l'ancien, pas meilleur : un
  seul buffer partagé (`make_shared`) était déjà la stratégie de `textAll()`
  dans le fork — aucun gain de ce côté, à ne jamais présenter comme tel.
- Code mort partiel : `broadcastLog()` reste sans appelant (inchangé par cet
  ADR, cf. [ws-manager.md](../subsystems/ws-manager.md)).

### Ce que ça verrouille
- **Plafond de clients figé à la compilation** (`kWsRegistrySlots`) : relever
  `kWsMaxClients` au-delà de 15 exige de revoir le dimensionnement du tampon de
  snapshot sur la pile de `loopTask`, pas de forcer le `static_assert`.
- **Un client en excès est fermé, jamais mis en file d'attente ni servi en
  dégradé** : il n'y a pas de mécanisme de priorité ou de file d'attente de
  clients WS, et il ne doit pas y en avoir sans réviser ce registre.
- Toute future évolution du registre des clients authentifiés doit respecter
  l'invariant « pas de verrou applicatif tenu pendant une boucle d'envoi
  WebSocket » — un futur refactor qui réintroduirait un mutex sur ce chemin
  reproduirait l'interblocage écarté ici.

## Références

- Code : [`src/ws_client_registry_logic.h`](../../src/ws_client_registry_logic.h), [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/ws_manager.h`](../../src/ws_manager.h), [`src/constants.h`](../../src/constants.h)
- Tests : `test/test_native_ws_registry/`
- Garde CI : [`tools/check_ws_broadcast_targeting.sh`](../../tools/check_ws_broadcast_targeting.sh) (`.github/workflows/hygiene-specs.yml`, job « Diffusion WebSocket ciblée »)
- Spec : [bug-081](../../specs/features/done/bug-081-websocket-diffuse-aux-clients-non-authentifies.md)
- Doc : [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall), [`docs/subsystems/auth.md`](../subsystems/auth.md), [`docs/API.md`](../API.md#ws-ws--write)
