# ADR-0004 — Sélecteur de mode à 3 valeurs au lieu de booléens `ph_enabled` / `orp_enabled`

- **Statut** : Accepté
- **Date** : 2026-04-24 (CHANGELOG [Unreleased])
- **Doc(s) liée(s)** : [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md), [pump-controller.md](../subsystems/pump-controller.md)

## Contexte

Avant la refonte des pages pH et ORP, la régulation chimique était pilotée par deux booléens :

- `ph_enabled` : régulation pH active (PID) vs. inactive
- `orp_enabled` : idem pour l'ORP

L'ajout du mode Programmée (voir [ADR-0002](0002-mode-programmee-volume-quotidien.md)) introduit un **troisième état** : ni PID, ni désactivé, mais « injecte X mL/j ». Un booléen ne suffit plus.

Par ailleurs, l'intégration Home Assistant auto-discovery publie un `switch.*_ph_enabled` et un `switch.*_orp_enabled` : supprimer ces champs casserait les automations HA existantes chez les utilisateurs.

## Décision

Le firmware remplace les booléens par un **enum string à 3 valeurs** :

```
ph_regulation_mode  ∈ { "automatic", "scheduled", "manual" }
orp_regulation_mode ∈ { "automatic", "scheduled", "manual" }
```

Les anciens booléens `ph_enabled` / `orp_enabled` sont **conservés comme miroirs dérivés** :
- `ph_enabled = (ph_regulation_mode != "manual")`
- `orp_enabled = (orp_regulation_mode != "manual")`

Ils sont publiés sur MQTT et exposés dans `/get-config` pour compatibilité, mais la **source de vérité** est `*_regulation_mode`.

Une migration NVS automatique s'exécute au premier boot post-upgrade :
- `ph_enabled=true`  → `ph_regulation_mode = "automatic"`
- `ph_enabled=false` → `ph_regulation_mode = "manual"`
- idem ORP

## Alternatives considérées

- **Trois booléens** (`ph_auto`, `ph_scheduled`, `ph_manual`, rejeté) — état illégal possible (deux vrais à la fois), logique défensive nécessaire partout.
- **Casser la compatibilité HA** (supprimer `ph_enabled` du discovery, rejeté) — impact utilisateur réel, nécessiterait une migration dans toutes les automations HA configurées.
- **Entier** (`mode = 0|1|2`, rejeté) — moins lisible dans les payloads MQTT et WebSocket, pas plus compact en pratique (ArduinoJson sérialise les deux).

## Conséquences

### Positives
- Un seul champ par régulation, état non ambigu.
- Migration transparente : les utilisateurs existants conservent leur comportement au redémarrage.
- Les automations HA qui testaient `switch.*_ph_enabled == on` continuent de fonctionner.

### Négatives / dette assumée
- Double écriture : chaque changement de mode met à jour **deux** champs (`ph_regulation_mode` et `ph_enabled`). Risque de désynchro si on modifie le code en oubliant le miroir.
- Le miroir `ph_enabled` ne distingue plus `automatic` de `scheduled` : les utilisateurs HA qui voulaient « régulation automatique PID » récupèrent aussi « régulation programmée » comme `on`.
- Plus de champs à publier sur MQTT (ajout de `ph_regulation_mode` et `ph_daily_target_ml`, idem ORP).

### Ce que ça verrouille
- Tant que `ph_enabled` / `orp_enabled` existent comme miroirs, les renommer ou les supprimer est un *breaking change* HA.
- Ajouter un 4ᵉ mode (ex. `expert`) nécessitera une décision sur la valeur du miroir booléen.

## Références

- Code : [`src/config.h`](../../src/config.h) struct `MqttConfig` champs `phRegulationMode`, `orpRegulationMode`, `phEnabled`, `orpEnabled`
- Code : [`src/config.cpp`](../../src/config.cpp) migration NVS au boot
- Code : [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) validation enum
- API : [`docs/API.md`](../API.md) champs de `/get-config` / `/save-config`
- MQTT : [`docs/MQTT.md`](../MQTT.md) note « Compatibilité `orp_enabled` » en fin de fichier
- CHANGELOG [Unreleased] 2026-04-24
