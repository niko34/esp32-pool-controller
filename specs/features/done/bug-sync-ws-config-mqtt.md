# Bug — L'UI web ne se met pas à jour quand la config change via MQTT/HA

- **Type** : bug simple (format minimal)
- **Statut** : done — tests verts (185/185 natifs), `pio run` 0 warning, revue code-reviewer Approved
- **Version cible** : 2.14.1
- **Créée** : 2026-07-06 (rapport utilisateur on-target, v2.14.0)
- **Domaine** : synchronisation WebSocket (UI) — aucun chemin de dosage touché

## Symptôme

Modifier la consigne pH ou ORP depuis Home Assistant (`.../ph_target/set`, `.../orp_target/set`) : la cible affichée sur le tableau de bord de l'UI web ne change pas (attente de 5 s inutile). Un rechargement de page corrige (donc la valeur est bien persistée et renvoyée par `/get-config`).

## Cause racine

L'UI n'apprend un changement de config que par un message WebSocket `type:"config"`, déclenché par le flag `_pendingConfigBroadcast` (ws_manager). Ce flag n'est activé qu'à un seul endroit : la route HTTP `/save-config` (`web_routes_config.cpp:818`, `wsManager.requestConfigBroadcast()`). Les handlers MQTT de `MqttManager::drainCommandQueue()` modifient `mqttCfg`, persistent et republient l'état MQTT, mais **n'appellent jamais `requestConfigBroadcast()`** → les clients WS connectés ne sont pas notifiés jusqu'au reload.

Périmètre réel : tous les handlers MQTT qui changent une config exposée dans `/get-config` — consignes pH/ORP (rapporté), modes de régulation (009), volumes quotidiens (050), mode filtration, éclairage. (Filtration ON/OFF et éclairage se reflètent partiellement via `sensor_data`.)

## Correctif

`drainCommandQueue()` s'exécute sur la loopTask, comme la consommation du flag WS → poser le flag y est sûr (écriture d'un booléen). Solution : un flag local `needConfigBroadcast` positionné pour toute commande traitée sauf `Reboot`, et `wsManager.requestConfigBroadcast()` appelé une fois en fin de drain. Le `return` interne (file vide) devient `break` pour un point de sortie unique. `#include "ws_manager.h"` ajouté.

Le prochain cycle WS (≤ 5 s) diffuse alors la config à jour à tous les clients. Broadcast idempotent (coalescé par le booléen), aucun contrat modifié.

## Critères d'acceptation

- [ ] Changer la consigne pH/ORP depuis HA → cible mise à jour dans l'UI web sous ≤ 5 s sans reload (on-target).
- [ ] Idem modes de régulation, volumes quotidiens, mode filtration via HA (on-target).
- [x] Aucun chemin de dosage touché ; mqtt_manager.cpp uniquement.
- [x] `pio run` sans warning, 185 tests natifs verts.

## Notes d'implémentation

- **Mécanisme** : `drainCommandQueue()` transforme le `return` interne (file vide) en `break` pour obtenir un point de sortie unique ; un flag local `needConfigBroadcast` est positionné pour toute commande traitée **sauf `Reboot`**, et `wsManager.requestConfigBroadcast()` est appelé **une seule fois** en fin de drain (`#include "ws_manager.h"` ajouté).
- Broadcast **unique et idempotent** (coalescé par le flag `_pendingConfigBroadcast` de `ws_manager`), consommé sur `loopTask` au prochain cycle WS (≤ 5 s). Aucun contrat modifié, second déclencheur du message `config` à côté de `POST /save-config`.
- **Revue** : code-reviewer **Approved**. Réserve mineure notée : le broadcast est posé même sur une commande **rejetée** (payload invalide, valeur refusée par la limite vive) — jugé idempotent/inoffensif, la config n'ayant alors pas changé et le snapshot renvoyé restant cohérent.
- **Fichiers** : `src/mqtt_manager.cpp` (+ include), `src/version.h` (2.14.1).
- **Reste on-target** : valider qu'un changement de consigne depuis HA se reflète dans l'UI sous 5 s sans reload.
