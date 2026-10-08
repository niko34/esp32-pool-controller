# bug-081 — Le WebSocket diffuse à tous les clients, authentifiés ou non

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug de sécurité** — divulgation de données |
| Statut | implémenté — tests natifs verts, revue de sécurité OK ; preuves on-target C1/C2/C3 restent à exécuter par un humain (voir Notes d'implémentation) |
| Priorité | Haute pour la flotte, modérée en usage domestique |
| Découvert | 29/08/2026, incidemment — un agent a capturé les trames en clair sans s'authentifier |
| Version en cours | 2.26.0 (déployée) → corrigé en 2.57.0 |

## Domaines touchés

- `src/ws_manager.cpp` — `_broadcastSensorData()`, `broadcastConfig()`, tout appel à `textAll()`
- `docs/subsystems/ws-manager.md`, `docs/API.md`

## Sécurité chimique

**Non — aucune commande n'est exposée.** Les actions (dosage, calibration,
configuration) passent par HTTP authentifié. C'est une fuite de **lecture**, pas
une prise de contrôle.

## Le défaut

```cpp
if (_authenticatedClients.empty()) return;   // ws_manager.cpp:114
...
_ws->textAll(sWsFrameBuf, len);              // ws_manager.cpp:244
```

La garde vérifie qu'**au moins un** client est authentifié. `textAll()` diffuse
ensuite à **tous les clients connectés**, y compris ceux qui n'ont jamais envoyé
`{"type":"auth","token":"..."}`.

`_authenticatedClients` est donc tenu à jour correctement (insertion sur jeton
valide, suppression à la déconnexion, comparaison à temps constant — feature-028)
mais **n'est jamais utilisé pour cibler l'envoi**. Il ne sert que de booléen
« y a-t-il au moins un client légitime ».

## Preuve

Un agent s'est connecté à `/ws` **sans s'authentifier** et a capturé les trames
en clair pendant que l'interface légitime était ouverte : 92 champs, toutes les
5 s, dont `ph`, `orp`, températures, volumes injectés, états de dosage, causes de
refus. Reproductible tant qu'une session authentifiée est active.

Corollaire : le défaut est **invisible en usage normal**, puisqu'un client seul
et non authentifié ne reçoit rien (la garde de non-vacuité le protège). Il ne se
manifeste qu'en présence d'un client légitime — c'est-à-dire dès que le
propriétaire consulte son interface.

## Correctif attendu

Cibler l'envoi sur `_authenticatedClients` au lieu de `textAll()`.

⚠️ **Piège documenté à ne pas rouvrir** — `ws_manager.cpp:70` : « itérer
`getClients()` depuis loopTask est un UB — `_handleDisconnect` (async_tcp) efface
la liste sous le verrou PRIVÉ du fork, inaccessible côté appelant ». La v2.11.1 a
déjà payé cette leçon. Le correctif doit donc envoyer par `client->text()` sur
des identifiants, en gérant proprement le cas d'un client disparu entre-temps —
sans itérer la liste interne du serveur depuis la boucle principale.

À vérifier aussi : le même défaut sur `broadcastConfig()` et sur tout autre
`textAll()` du fichier.

## Question de conception à trancher

Le mode « authentification désactivée » (`authCfg.enabled == false`) insère
automatiquement tout client dans `_authenticatedClients` (`ws_manager.cpp:203`).
Ce comportement doit être **préservé** : un appareil volontairement ouvert doit
continuer de fonctionner. Le correctif ne doit pas transformer ce choix en panne.

## Critères de clôture

- Un client non authentifié ne reçoit **aucune** trame, même quand un client
  légitime est connecté — vérifié par capture réseau.
- Le mode sans authentification continue de fonctionner à l'identique.
- Aucune itération de `getClients()` depuis loopTask (le piège de la v2.11.1).
- Pas de régression sur la limite de 4 clients simultanés ni sur la fermeture
  automatique en cas de file saturée.

## Notes d'implémentation (v2.57.0)

### Ce qui a été fait

Les trois broadcasts (`broadcastSensorData`, `broadcastConfig`, `broadcastLog`)
ne passent plus par `_ws->textAll(...)` mais par `WsManager::_sendToAuthenticated()` :
snapshot des ids authentifiés sur la pile de `loopTask`, un seul
`std::make_shared<std::vector<uint8_t>>` pour toute la trame, puis
`_ws->text(id, sharedBuffer)` par identifiant du snapshot. Le registre
`_authenticatedClients` (`std::set<uint32_t>`) est remplacé par
`_authRegistry`, instance de `WsClientRegistry<kWsRegistrySlots>`
(`src/ws_client_registry_logic.h`, nouveau module pur ADR-0017). Détail complet,
y compris le raisonnement de sûreté sans verrou : [ws-manager.md](../../docs/subsystems/ws-manager.md#bug-081--diffusion-ciblée-sur-registre-au-lieu-de-textall).

### API du fork retenue

`AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)` — prend le
verrou privé du fork (`_ws_clients_lock`, récursif) et résout `client(id)`
**sous ce verrou**, renvoyant `false` si le client a disparu entre le snapshot
et l'envoi. C'est ce qui permet d'éviter le piège documenté dans ce fichier
(itérer `getClients()` depuis `loopTask` = UB) **sans** réintroduire de verrou
applicatif.

### Défaut de concurrence préexistant, corrigé au passage

L'ancien booléen `_pendingInitialPush` était posé par **toute** insertion
réussie et remis à `false` au prochain `update()` : si une **seconde**
insertion survenait avant que le push déclenché par la première n'ait eu lieu,
elle n'était distinguée en rien — un cas limite pouvait perdre le push initial
d'un client sans rattrapage. Remplacé par un compteur d'époque
(`_authEpoch`/`_servedEpoch`, atomics `release`/`acquire`) qui ne peut pas
« écraser » une insertion non encore servie.

### Lacune de test/lint trouvée en revue et comblée

Le contrôle CI initial (`tools/check_ws_broadcast_targeting.sh`) interdisait
les appels `->textAll(`/`.textAll(` mais se laissait contourner par un appel
**qualifié** `AsyncWebSocket::textAll(...)` (ni `->` ni `.`). Un second motif a
été ajouté pour fermer cette porte avant la clôture.

### Conditions bloquantes du gate `security-reviewer` — toutes couvertes

1. Idempotence de `WsClientRegistry::insert()` (un id déjà présent ne
   consomme pas de second slot).
2. Prise/libération de slot **toujours** par CAS, jamais par store simple
   (insert et erase peuvent viser le même slot en concurrence).
3. Révocation immédiate (`revokeAllAuthenticated()`) sur bascule
   `auth_enabled` faux→vrai et sur rotation de token.
4. Ordre `release`/`acquire` de l'époque d'authentification, avancée **avant**
   l'envoi réel pour ne jamais reconstruire le JSON en boucle serrée en
   famine mémoire.

### Preuves on-target restantes (humain, non automatisables en CI)

- **C1** — capture réseau : un client connecté à `/ws` sans envoyer
  `{"type":"auth",...}` ne reçoit **aucune** trame pendant qu'un client
  légitime authentifié est actif (critère de clôture n°1).
- **C2** — bascule `auth_enabled = false` sur un appareil réel : tout client
  connecté reste servi sans régression (critère de clôture n°2).
- **C3** — saturation réelle : connecter `kWsMaxClients + 1` clients et
  vérifier que l'excédentaire est fermé proprement, sans effet sur les
  `kWsMaxClients` clients déjà authentifiés (critère de clôture n°4).

### Corrections apportées après la revue de code (agent principal, 2026-10-08)

1. **Lacune de test réelle, comblée et prouvée.** La suite initiale ne couvrait l'idempotence
   que lorsque le doublon occupait le PREMIER slot — cas où le filet de rattrapage de la
   passe 2 l'absorbait. Le `code-reviewer` a reproduit l'agencement manquant :
   `insert(1); insert(2); erase(1); insert(2)` ⇒ `kInserted` et `count() == 2`. En production :
   trame envoyée **deux fois** au même client (2 des 5 slots de `WS_MAX_QUEUED_MESSAGES`) et
   `erase()` n'en libérant qu'un, **slot fuité définitivement** jusqu'au redémarrage.
   Test `test_insert_idempotent_with_free_slot_before_duplicate` ajouté, puis **mutant reposé
   pour vérifier qu'il meurt** : il meurt sur ce test et sur lui seul. Module restauré,
   empreinte `md5 3ebdd3c4...` identique à l'original.

2. **Ordonnancement mémoire réparé.** `_authEpoch.fetch_add` était en `relaxed` face à un
   `load(acquire)` : un acquire sans release en face ne s'apparie avec rien. L'époque pouvait
   devenir visible **avant** l'écriture du slot ⇒ `update()` voyait la nouvelle époque, prenait
   un snapshot SANS ce client, puis avançait `_servedEpoch` ⇒ **push initial (capteurs ET
   config) perdu définitivement** pour ce client. Les deux sites passés en `release`.

3. **`static_assert` tautologique remplacé.** `kWsRegistrySlots == kWsMaxClients + 1`
   restituait la ligne au-dessus : il ne pouvait pas échouer, tout en donnant une fausse
   assurance sur le bornage du tampon de pile. Remplacé par une borne réelle
   (`>= 2 && <= 16`), motivée par la pile de loopTask.

4. **Script CI durci.** Le motif `(->|\.)` se laissait contourner par un appel QUALIFIÉ
   `AsyncWebSocket::textAll(...)`. Second motif ajouté, **vérifié par l'épreuve** : un fichier
   sonde contenant un appel qualifié fait bien sortir le script en erreur. `pingAll` reste hors
   liste à dessein — trame de contrôle sans donnée applicative.

5. **Commentaire mémoire FAUX corrigé** (condition du `security-reviewer` P2). Le code
   affirmait que `textAll` aurait alloué « N buffers, un par client ». Vérifié dans le fork :
   `textAll(const char*, len)` délègue à `textAll(makeSharedBuffer(...))`, donc **UN buffer
   partagé entre tous**. Le profil mémoire du correctif est **ÉGAL à l'ancien, pas meilleur**.
   Ce qui aurait fait N copies de ~3,5 Ko, c'est la surcharge `text(id, const char*, len)` —
   écartée pour cette raison précise.

### Ce que le cycle a établi au passage

- **`textAll()` était déjà un comportement indéfini en production** : `textAll(SharedBuffer)`
  prend le verrou puis fait `for (auto &c : _clients)` ; le verrou étant **récursif**, une
  fermeture déclenchée en cours de boucle (file pleine) efface un élément **sous l'itérateur**
  sans que rien ne s'y oppose. Le correctif supprime donc un UB existant en plus de la fuite.
- **Deux sites seulement écrivent `authCfg.enabled`** (vérifié) : `config.cpp:565` (chargement
  NVS au démarrage, aucun client WS n'existe alors) et `web_routes_config.cpp:1091` (couvert
  par la révocation). Pas de troisième porte.

### Résultats mesurés
- `pio run -e esp32dev_core3` : SUCCESS, zéro nouveau warning — RAM 29,5 %, Flash 50,1 %
  (+568 o de flash, +24 o de RAM)
- `pio test -e native` : **2 064 cas / 2 064**, **58 suites**
- Mutation : 5 mutants du rapport initial + celui de la revue, **tous tués**
- Hygiène : `check_specs`, `check_doc_links`, `check_fopen_guard`, `check_memory_discipline`,
  `check_ws_broadcast_targeting` — tous verts
- `security-reviewer` P1 **et** P2 : GO · `code-reviewer` : Approved après corrections

### ⚠️ Reste à faire — étape humaine
Les trois preuves on-target **n'ont pas été exécutées** (pas de port série dans ce conteneur) :
- **C1** — client non authentifié connecté pendant qu'une session légitime reçoit ⇒ AUCUNE trame
- **C2** — mode `authCfg.enabled == false` ⇒ le client sans auth reçoit comme avant
- **C3** — révocation : bascule auth off→on, puis `/auth/regenerate-token`
Les commandes exactes sont dans le rapport de test. **Tant qu'elles ne sont pas jouées, la
fermeture de la fuite est prouvée par le code et les tests, pas par l'observation.**
