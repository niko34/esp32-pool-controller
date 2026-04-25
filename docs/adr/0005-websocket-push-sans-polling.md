# ADR-0005 — WebSocket push pour le temps réel, pas de polling périodique

- **Statut** : Accepté
- **Date** : 2025 (introduction de `ws_manager`)
- **Doc(s) liée(s)** : [ws-manager.md](../subsystems/ws-manager.md), [API.md](../API.md), [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md)

## Contexte

Chaque page de l'UI (dashboard, pH, ORP, dosages) doit afficher des valeurs qui évoluent dans le temps : pH courant, ORP courant, température, état des pompes, cumul journalier, temps restant d'injection manuelle, délai de stabilisation, etc.

Un client web a deux façons de récupérer ça :

1. **Polling HTTP** — le client appelle `GET /data` toutes les X secondes.
2. **WebSocket push** — le serveur pousse un message JSON dès qu'un état change, + un résumé périodique.

Avec 4 à 8 onglets ouverts sur le réseau local (PC, tablette, téléphone, Home Assistant), le polling multiplie les requêtes HTTP authentifiées sur un ESP32 qui a autre chose à faire (boucle de régulation, mesures capteurs, MQTT, UART).

## Décision

Le contrôleur expose un **WebSocket `/ws`** authentifié qui pousse :

- un **message initial** à la connexion (config complète + dernières mesures)
- un **push périodique** toutes les **5 s** (cadence `kSensorPushIntervalMs`, voir [`ws_manager.h`](../../src/ws_manager.h))
- des **push événementiels** à chaque changement significatif (changement de mode, changement de config, début/fin d'injection, log)

Le frontend (`data/app.js`) s'abonne au WebSocket au chargement et ne **polle jamais** les endpoints HTTP pour récupérer l'état. Les endpoints HTTP sont réservés aux **actions** (save-config, inject/start, calibrate, reboot, …).

L'endpoint `GET /data` est conservé pour les clients scriptés (Home Assistant alternatif, script Python) mais l'UI web ne l'utilise pas.

## Alternatives considérées

- **Polling HTTP toutes les 5 s** (rejeté) — charge CPU et heap sur l'ESP32, latence moyenne de 2,5 s sur chaque changement d'état, multiplicateur par nombre d'onglets ouverts.
- **Server-Sent Events (SSE)** (rejeté) — moins de support natif dans la stack `AsyncWebServer`, pas de canal retour client → serveur (utile si on veut envoyer plus tard des commandes par la même socket).
- **MQTT depuis le navigateur** (rejeté) — nécessiterait un broker MQTT-over-WebSocket et couplerait l'UI locale à la présence d'un broker externe, à l'opposé du mode « offline first ».

## Conséquences

### Positives
- Charge réseau et CPU réduite : un push toutes les 5 s par client au lieu d'une requête HTTP complète.
- Latence < 1 s sur les événements importants (début d'injection visible en quasi temps réel sur toutes les pages ouvertes).
- Une seule source d'état côté JS : `latestSensorData` alimentée par le WebSocket.

### Négatives / dette assumée
- Le client doit gérer reconnexion automatique + état « déconnecté » (badge visuel sur les pages pH / ORP : blocs Statistiques grisés quand WS KO).
- La validation de l'authentification WebSocket est custom (cookie ou token), voir `ws_manager.cpp` — pas la même stack que HTTP Basic Auth.
- Scripts externes qui voudraient lire l'état doivent passer par `GET /data` : le WebSocket reste le canal principal.

### Ce que ça verrouille
- La cadence de 5 s est un compromis : descendre plus bas (1 s) surchargerait l'ESP32 avec plusieurs clients ; monter plus haut (10 s) rendrait l'UI moins réactive pendant une injection courte (30 s min).

## Références

- Code : [`src/ws_manager.h`](../../src/ws_manager.h) (`kSensorPushIntervalMs = 5000`)
- Code : [`src/ws_manager.cpp`](../../src/ws_manager.cpp) `broadcastSensorData()`, `broadcastConfig()`, `broadcastLog()`
- Code : [`data/app.js`](../../data/app.js) gestion de `latestSensorData` et reconnexion
- Doc régulation : [pump-controller.md](../subsystems/pump-controller.md) — consommatrice principale
- API : [`docs/API.md`](../API.md) section « WebSocket temps réel »
