# bug-081 — Le WebSocket diffuse à tous les clients, authentifiés ou non

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug de sécurité** — divulgation de données |
| Statut | todo |
| Priorité | Haute pour la flotte, modérée en usage domestique |
| Découvert | 29/08/2026, incidemment — un agent a capturé les trames en clair sans s'authentifier |
| Version en cours | 2.26.0 (déployée) |

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
