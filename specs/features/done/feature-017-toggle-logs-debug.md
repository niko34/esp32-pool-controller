## Métadonnées
- **ID** : feature-017
- **Slug** : toggle-logs-debug
- **Type** : feature
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 1.0.7
- **Dernière mise à jour** : 2026-04-30
- **Spec(s) liée(s)** : aucune (mais s'inspire du pattern existant `sensorLogsEnabled` introduit pour les logs des sondes)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (nouveau champ dans `/get-config`, `/save-config`, payload WS)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (NVS : nouvelle clé booléenne)
- [x] Frontend (`data/`) — nouveau switch dans Paramètres Avancé

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non applicable (aucun chemin de dosage touché, pas de calibration)
- [ ] ux-ui-designer — non requis (un switch standard sous un autre switch existant, pas d'arbitrage UX nouveau)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Ajouter un paramètre **« Logs DEBUG activés »** dans Paramètres → Avancé, juste sous le switch existant **« Log des sondes »**. Quand ce paramètre est désactivé (par défaut), les messages de niveau `DEBUG` ne sont **ni produits côté firmware** (économie CPU/RAM/flash logs) **ni affichés côté UI**. Quand activé, les messages `DEBUG` sont produits, persistés dans le buffer de logs, et affichés dans la page Logs comme aujourd'hui.

L'objectif est d'**alléger les logs en fonctionnement nominal** (production), tout en permettant d'**activer les logs DEBUG ponctuellement** lors d'une session de diagnostic, sans nécessiter de re-flasher le firmware.

## Contexte

Aujourd'hui, le firmware produit en permanence un volume non négligeable de logs `DEBUG` :
- `Diagnostic publié` (toutes les 5 min) dans `mqtt_manager.cpp`
- `Consolidation terminée: N points` et `Historique sauvegardé (N points)` dans `history.cpp`
- `Config produits sauvegardée` après chaque save MQTT
- `MQTT publish drop: <topic>` (depuis feature-014 IT4) en cas de coupure réseau
- Etc.

Ces logs saturent rapidement le buffer NVS (200 entrées max — `kMaxLogEntries`) et la fenêtre Logs de l'UI, rendant la lecture difficile pendant un diagnostic. Le filtre `#log_level_debug` côté UI permet déjà de masquer l'affichage, mais le firmware continue de les produire — donc ils occupent de la place dans le buffer rotatif et sont push via WebSocket.

Le pattern existant `authCfg.sensorLogsEnabled` (booléen NVS, exposé dans `/get-config`, modifiable via le switch « Log des sondes ») est le modèle à suivre.

## Comportement attendu

### Côté UI (Paramètres → Avancé)

- Nouveau switch **« Logs DEBUG activés »** placé **immédiatement sous** le switch existant « Log des sondes »
- Libellé descriptif court sous le switch (ex. « Active les messages de diagnostic détaillé. À activer uniquement pour le diagnostic. »)
- Quand l'utilisateur active/désactive le switch et clique « Enregistrer », la valeur est persistée en NVS et l'effet est immédiat (pas de redémarrage requis)
- Filtre `#log_level_debug` de la page Logs reste indépendant : il filtre l'affichage. Mais quand `debugLogsEnabled = false`, le firmware ne produit plus ces messages, donc le filtre n'a plus rien à afficher.

### Côté firmware

- Nouvelle option de config : `authCfg.debugLogsEnabled` (booléen, default `false`), persistée en NVS sous la clé `debug_logs`
- `Logger::debug(const String&)` (ligne ~75 de `logger.cpp`) doit court-circuiter immédiatement si `authCfg.debugLogsEnabled == false` (early return, pas d'allocation, pas de push WS, pas d'écriture buffer)
- Les autres niveaux (`info`, `warning`, `error`, `critical`) ne sont **pas affectés**
- Le changement de la valeur est effectif sans redémarrage : la prochaine appel à `Logger::debug()` lira la nouvelle valeur

### Cas limites

- WebSocket déconnecté : pas d'impact (pas de push de log si pas produit)
- Buffer NVS saturé : moins probable car volume de logs réduit
- Activation à chaud : la pile d'exécution courante peut produire un dernier log `DEBUG` avant que la nouvelle valeur ne soit lue (race bénigne, pas de problème)
- Désactivation à chaud : symétrique
- Default `false` : par défaut après un flash neuf ou une mise à jour OTA, les logs DEBUG sont désactivés (économie immédiate)
- Migration : les utilisateurs qui mettent à jour vers cette version verront leurs logs DEBUG désactivés sans action ; ils devront aller activer le switch s'ils les veulent

## Comportement actuel

- Les `Logger::debug(...)` produisent tous leurs messages, sont persistés dans le buffer rotatif (200 entrées), poussés via WebSocket à la page Logs, et écrits dans le fichier persistant.
- Le filtre `#log_level_debug` côté UI permet de **masquer** l'affichage, mais ne réduit pas la production firmware.
- Aucune option utilisateur ne permet de désactiver la production des logs DEBUG.

## Contraintes techniques

- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB — vérifier `./build_fs.sh`
- AsyncWebServer : aucun handler bloquant > 50 ms
- Concurrence : `authCfg.debugLogsEnabled` est un `bool` lu depuis multiples tâches (loopTask, mqttTask, etc.). Une lecture booléenne sans mutex est OK sur ESP32 (atomicité naturelle des lectures alignées) — pas de barrier requise pour ce cas
- Pattern à suivre exactement : `authCfg.sensorLogsEnabled` (cf. `src/config.h:119`, `src/config.cpp:166,319`, `src/web_routes_config.cpp:167,492-494`, `src/ws_manager.cpp:277`)
- NVS : ajouter une clé courte (≤ 15 caractères) dans la même `Preferences` namespace que `sensor_logs`
- Le early-return dans `Logger::debug()` doit être en TOUT DÉBUT de fonction, AVANT toute allocation `String` ou tout calcul

## Fichiers potentiellement concernés

- `src/config.h` — ajouter `bool debugLogsEnabled = false;` dans la struct AuthConfig
- `src/config.cpp` — `saveAuthConfig()` ligne ~166 (write NVS), `loadAuthConfig()` ligne ~319 (read NVS)
- `src/logger.cpp` — `Logger::debug()` ligne ~75, ajouter early return si `!authCfg.debugLogsEnabled`
- `src/logger.h` — pas de changement attendu
- `src/web_routes_config.cpp` — ligne ~167 (exposer dans `/get-config`), ligne ~492-494 (lire dans `/save-config`)
- `src/ws_manager.cpp` — ligne ~277 (exposer dans `_buildConfigJson()` pour push WS broadcast config)
- `data/index.html` — nouveau switch « Logs DEBUG activés » dans la card Avancé, juste sous « Log des sondes »
- `data/app.js` — `applyConfig()` (~ligne 1340+) lire `cfg.debug_logs_enabled`, `collectAuthConfig()` ou équivalent envoyer le champ
- `docs/features/page-settings.md` — mention du nouveau switch dans la section Avancé
- `docs/subsystems/logger.md` — comportement filtré
- `docs/UPDATE_GUIDE.md` — note de migration : par défaut les logs DEBUG sont désactivés après update
- `CHANGELOG.md` — entrée Firmware + Frontend
- `src/constants.h` — pas attendu (booléen NVS, pas de constante de seuil)

## Livrables — check-list

- [x] A1 — Ajout `bool debugLogsEnabled = false;` dans `AuthConfig` (`src/config.h`)
- [x] A2 — Persistence NVS : write dans `saveAuthConfig()` + read dans `loadAuthConfig()` avec clé `debug_logs` et default `false`
- [x] A3 — Early return dans `Logger::debug()` si `!authCfg.debugLogsEnabled` (en tout début de fonction)
- [x] A4 — Champ `debug_logs_enabled` ajouté dans la réponse `/get-config` (`web_routes_config.cpp:167`)
- [x] A5 — Champ `debug_logs_enabled` lu dans `/save-config` (`web_routes_config.cpp:492+`) avec log info `"Logs DEBUG: activés/désactivés"`
- [x] A6 — Champ `debug_logs_enabled` ajouté dans `_buildConfigJson()` (`ws_manager.cpp:277`)
- [x] B1 — Nouveau switch HTML « Logs DEBUG activés » dans `data/index.html`, immédiatement sous « Log des sondes » dans la card Avancé
- [x] B2 — Texte descriptif court en français sous le switch
- [x] B3 — `data/app.js` : `applyConfig()` met à jour le checkbox depuis `cfg.debug_logs_enabled`
- [x] B4 — `data/app.js` : la fonction qui envoie la config inclut le champ `debug_logs_enabled` (suivre le pattern de `sensor_logs_enabled`)
- [ ] D1 — Test manuel : activer le switch, sauvegarder, vérifier que des logs DEBUG apparaissent. Désactiver, sauvegarder, vérifier qu'aucun nouveau log DEBUG n'apparaît — **délégué à l'humain post-flash**
- [ ] D2 — Test manuel : redémarrer l'ESP32, vérifier que la valeur est bien persistée — **délégué à l'humain post-flash**
- [x] D3 — `pio run` SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning
- [x] D4 — `./build_fs.sh` SUCCESS, partition 1088 KB OK
- [x] C1 — `docs/features/page-settings.md` : section card Logs mentionne le nouveau switch « Logs DEBUG activés »
- [x] C2 — `docs/subsystems/logger.md` : `Logger::debug()` est conditionné par `authCfg.debugLogsEnabled` (nouvelle section « Toggle DEBUG runtime »)
- [x] C3 — `docs/UPDATE_GUIDE.md` : note de migration ajoutée (default false après update + procédure d'activation)
- [x] C4 — `CHANGELOG.md` : entrée Firmware + Frontend + Documentation sous `[Unreleased] - 2026-04-30`

## Nouvelles signatures / API

- Endpoints HTTP : aucun nouveau (champs ajoutés à `/get-config` et `/save-config` existants)
- Champs WebSocket nouveaux : `debug_logs_enabled` (booléen) dans la payload `config`
- Topics MQTT : aucun nouveau
- Fonctions publiques : aucune nouvelle
- Constantes : aucune nouvelle

## Sécurité chimique

- Dosage impliqué : non
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : aucun (option d'observabilité pure)

## ADR attendu ?

- **Probable** : non
- **Critère(s) candidat(s)** : aucun (extension d'un pattern existant `sensorLogsEnabled`, pas de décision structurante, pas d'alternative crédible à arbitrer)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-config` — nouveau champ `debug_logs_enabled` (bool)
- `POST /save-config` — nouveau champ accepté `debug_logs_enabled` (bool)

### Champs WebSocket
- `config.debug_logs_enabled` (bool) — exposé dans la payload `config` push WS via `_buildConfigJson()`

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-settings.md` — section Avancé : nouveau switch « Logs DEBUG activés », sous « Log des sondes »
- `docs/subsystems/logger.md` — `Logger::debug()` filtré par `authCfg.debugLogsEnabled` ; `info/warning/error/critical` non affectés

#### Docs transverses
- `docs/API.md` — si la liste des champs `/get-config` y est listée, ajouter `debug_logs_enabled`
- `docs/UPDATE_GUIDE.md` — note de migration (default `false` après update)
- `CHANGELOG.md` — entrée sous la version cible

#### Docs immuables
- aucun

## Critères d'acceptation

- [x] AC1 — Quand le switch est désactivé (default), aucun log de niveau DEBUG n'apparaît dans la page Logs ni dans le buffer NVS, et `Logger::debug()` retourne immédiatement (vérifié par lecture du code — early return en première ligne dans `src/logger.cpp`)
- [ ] AC2 — Quand le switch est activé, les logs DEBUG sont produits et visibles comme avant — **délégué à l'humain post-flash**
- [ ] AC3 — La valeur du switch est persistée en NVS et survit à un redémarrage de l'ESP32 — **délégué à l'humain post-flash**
- [x] AC4 — Les niveaux INFO, WARN, ERROR, CRITICAL ne sont PAS affectés par ce switch (vérifié par lecture du code — seul `debug()` court-circuité)
- [x] AC5 — Le filtre `#log_level_debug` de la page Logs continue de fonctionner indépendamment (aucune modification du filtre UI)
- [x] AC6 — Tous les libellés utilisateur sont en français
- [x] AC7 — `pio run` compile sans nouveau warning
- [x] AC8 — `./build_fs.sh` passe sans dépasser la partition `spiffs` 1088 KB
- [x] AC9 — Le default `false` s'applique après un flash neuf ou une mise à jour OTA (vérifié par lecture du code : `bool debugLogsEnabled = false;` dans `AuthConfig` + default `false` au read NVS)

## Travail réalisé

### Firmware (5 fichiers)
- `src/config.h` : ajout `bool debugLogsEnabled = false;` dans la struct `AuthConfig`
- `src/config.cpp` : write/read NVS clé `debug_logs` (default `false`) dans `saveAuthConfig()` / `loadAuthConfig()`
- `src/logger.cpp` : `#include "config.h"` + early return `if (!authCfg.debugLogsEnabled) return;` en TOUTE PREMIÈRE LIGNE de `Logger::debug()`. Niveaux `info/warning/error/critical` non touchés
- `src/web_routes_config.cpp` : champ `debug_logs_enabled` exposé dans `/get-config` et accepté dans `/save-config` (avec log INFO FR « Logs DEBUG: activés » / « Logs DEBUG: désactivés »)
- `src/ws_manager.cpp` : champ `debug_logs_enabled` ajouté dans la payload WS `config` (pour broadcast cohérent à tous les clients connectés)

### Frontend (2 fichiers)
- `data/index.html` : nouveau switch `#debug_logs_enabled` dans la card Logs (Paramètres → Avancé), placé immédiatement sous `#sensor_logs_enabled`. Libellé « Logs DEBUG activés », description en français : « Active les messages de diagnostic détaillé. À activer uniquement pour le diagnostic. »
- `data/app.js` : lecture dans `applyConfig` (avec `=== true` pour respecter le default `false`), écriture dans `collectAuthConfig`, event listener autosave miroir de `sensor_logs_enabled`

### Documentation (4 fichiers)
- `docs/features/page-settings.md` : section card Logs étoffée
- `docs/subsystems/logger.md` : nouvelle section « Toggle DEBUG runtime »
- `docs/API.md` : payload `/get-config` + table des champs notables
- `docs/UPDATE_GUIDE.md` : note de migration en tête de section

### CHANGELOG
- `CHANGELOG.md` : nouvelle entrée sous `[Unreleased] - 2026-04-30` (sections Firmware, Frontend, Documentation)

## Fichiers modifiés

```
src/config.h
src/config.cpp
src/logger.cpp
src/web_routes_config.cpp
src/ws_manager.cpp
data/index.html
data/app.js
docs/features/page-settings.md
docs/subsystems/logger.md
docs/API.md
docs/UPDATE_GUIDE.md
CHANGELOG.md
specs/features/doing/feature-017-toggle-logs-debug.md → specs/features/done/feature-017-toggle-logs-debug.md
```

## Build

- `pio run` : **SUCCESS**
- RAM : **16.4 %** (inchangée)
- Flash : **97.8 %** (inchangée)
- Warnings : **0 nouveau**
- `./build_fs.sh` : **SUCCESS**, partition `spiffs` 1088 KB OK

## Tests dynamiques restants

À faire après flash USB ou OTA :

- **D1** — Activer le switch, sauvegarder, vérifier que des logs DEBUG apparaissent (ex. `DEBUG : Diagnostic publié` toutes les 5 min ou `DEBUG : Consolidation terminée: N points` toutes les heures). Désactiver, sauvegarder, vérifier qu'aucun nouveau log DEBUG n'apparaît dans la page Logs (attendre 5–10 min).
- **D2** — Activer le switch, redémarrer l'ESP32 (Paramètres → Système → Redémarrer). Au redémarrage, vérifier que le switch est toujours activé et que les logs DEBUG sont produits (persistance NVS).
- **AC2** / **AC3** dépendent de D1 et D2.

## Plan de test manuel

1. **Default après flash neuf / OTA** : vérifier que le switch « Logs DEBUG activés » est désactivé. Aller sur la page Logs, attendre 1-2 minutes, vérifier qu'aucun nouveau log DEBUG n'apparaît (sauf déjà présents avant la mise à jour).
2. **Activation** : cocher le switch, cliquer Enregistrer. Aller sur la page Logs avec auto-refresh. Vérifier qu'on voit apparaître `DEBUG : Diagnostic publié` (toutes les 5 min) ou `DEBUG : Consolidation terminée: N points` (toutes les heures) selon le timing.
3. **Désactivation** : décocher le switch, cliquer Enregistrer. Attendre 5-10 min. Vérifier qu'aucun nouveau log DEBUG n'apparaît dans la page Logs.
4. **Persistance** : activer, redémarrer l'ESP32 (Paramètres → Système → Redémarrer). Au redémarrage, vérifier que le switch est toujours activé et que les logs DEBUG sont produits.
5. **Filtre UI indépendant** : avec switch activé et logs DEBUG produits, jouer avec le filtre `#log_level_debug` côté UI : décocher → les logs DEBUG sont masqués mais d'autres niveaux restent ; recocher → les DEBUG réapparaissent. Le filtre UI agit uniquement sur l'affichage.
6. **Régression niveaux autres** : avec switch désactivé, vérifier que INFO, WARN, ERROR, CRITICAL apparaissent toujours normalement (ex. déclencher une alerte de capteur anormal pour générer un WARN).

## Notes pour agent

- **Pattern existant** `sensorLogsEnabled` à mimer scrupuleusement (5 fichiers à modifier en parallèle : `config.h/cpp`, `web_routes_config.cpp`, `ws_manager.cpp`, et la lecture côté code firmware concerné).
- **Pour la lecture côté `Logger::debug()`** : `authCfg` est une variable globale. Lire `authCfg.debugLogsEnabled` au début de la fonction. Pas de mutex requis (lecture booléenne).
- **Default `false`** : choix volontaire pour la sobriété. L'utilisateur active explicitement quand il a besoin.
- **NVS clé `debug_logs`** : 11 caractères, sous la limite 15. Cohérent avec `sensor_logs`.
- **Veiller à NE PAS toucher** au filtre UI `#log_level_debug` qui reste un filtre d'affichage indépendant. Les deux mécanismes sont complémentaires :
  - Switch « Logs DEBUG activés » → décide si le firmware **produit** les DEBUG
  - Filtre `#log_level_debug` → décide si l'UI **affiche** les DEBUG produits
- **Migration UX** : ajouter une note dans `docs/UPDATE_GUIDE.md` pour informer les utilisateurs existants que leurs logs DEBUG sont désormais désactivés par défaut après update, et qu'ils peuvent les réactiver via Paramètres → Avancé → « Logs DEBUG activés ».
- **Performance** : le early return dans `Logger::debug()` fait économiser potentiellement plusieurs centaines de bytes par log non produit (allocation `String`, lock mutex log buffer, push WebSocket, écriture fichier). Gain mesurable en production.

## Hors périmètre

- Pas de changement du niveau de log par défaut côté firmware (toujours `INFO`)
- Pas de filtre granulaire par catégorie/module (juste un on/off global pour DEBUG)
- Pas de mécanisme de niveaux configurables par module (ex. activer DEBUG seulement pour `mqtt_manager`)
- Pas de conditions temporelles (ex. activer DEBUG pendant 1h puis désactiver auto)
- Pas de désactivation des autres niveaux (INFO/WARN/ERROR/CRITICAL restent toujours actifs)
- Pas de modification du filtre UI `#log_level_debug` (reste indépendant, c'est volontaire)
