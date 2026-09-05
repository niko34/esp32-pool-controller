## Métadonnées
- **ID** : feature-032
- **Slug** : decoupage-god-objects-firmware
- **Type** : feature
- **Statut** : **clôturée — écartée** (2026-07-06, décision utilisateur sur analyse)
- **Priorité** : normale (à la rédaction — caduque à la clôture)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-06

## Note de clôture (2026-07-06)

Spec issue de la revue de code du 2026-06-20, **écartée** après audit contre v2.12.0 :

1. **Volet `pump_controller` : réglé autrement et mieux.** La spec le déléguait à la feature-029, supersédée par la campagne ADR-0017 (036-041) : toute la logique critique vit dans `dosing_logic` (324 l., 100 % couvert, 74 tests). La coquille restante (1530 l.) est de l'I/O sans logique risquée. La douleur d'origine des « god objects » — logique critique intestable — est éteinte.
2. **Volet `mqtt_manager` (1523 l., +225 depuis juin) : constat exact, mais pire ratio bénéfice/risque du backlog.** Le sous-système le plus sensible à l'intégration (double queue inter-core, dédup, 34 blocs discovery, reconnexion), une preuve d'iso-comportement reposant sur des captures MQTT on-target, un bénéfice purement esthétique pour un projet mono-mainteneur sans incident jamais causé par cette dette.
3. **Volet `web_routes_config` + couche de validation centralisée : spéculatif** (fichier stable à ~1000 l., aucune douleur constatée).

**Porte de sortie** : si `mqtt_manager` continue de grossir ou qu'une vraie douleur de maintenance apparaît, rouvrir en **version light** : pur déplacement de code des blocs discovery + sérialiseurs par domaine vers `mqtt_discovery.cpp`/`mqtt_serializers.cpp`, SANS toucher au transport/queues/concurrence (vérifiable compilateur, risque faible). Ne jamais rouvrir le volet validation sans douleur démontrée.

Aucun code modifié au titre de cette spec. Contenu conservé ci-dessous pour traçabilité.
- **Spec(s) liée(s)** : `specs/features/todo/feature-029-tests-testabilite-dosage.md` (extrait déjà une partie de `pump_controller`) — coordonner l'ordre

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — uniquement si le périmètre touche `pump_controller` (sinon non)
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Réduire la dette structurelle des **god objects** firmware en séparant les responsabilités : extraire la **sérialisation métier MQTT** du transport dans `mqtt_manager.cpp`, et dégrouper `web_routes_config.cpp` (point d'entrée unique surchargé). Refactor à **comportement constant**.

## Contexte
La revue de code (2026-06-20) a identifié plusieurs fichiers god objects :
- `mqtt_manager.cpp` (1298 l.) mélange transport, double queue, dédup, auto-discovery HA **et** sérialisation métier de tous les domaines (il `#include` sensors/filtration/lighting/pump) → couplage fort.
- `web_routes_config.cpp` (1008 l.) concentre un point d'entrée unique surchargé, touchant ~110× les structs config globales sans couche de validation centralisée.
- `pump_controller.cpp` (1195 l.) — partiellement traité par **feature-029** (extraction de la politique de dosage) ; cette feature ne le re-couvre que si feature-029 ne suffit pas.

Le but est de rendre ces modules plus lisibles, testables et moins couplés, **sans changer le comportement observable** (mêmes topics MQTT, mêmes endpoints, mêmes payloads).

## Comportement attendu
- **MQTT** : séparer le **transport** (connexion, queues, reconnexion, dédup, publication) de la **sérialisation métier** (construction des payloads par domaine + auto-discovery HA). Le transport ne dépend plus des managers métier ; la sérialisation est regroupée par domaine dans des unités dédiées.
- **web_routes_config** : dégrouper par sous-domaine de configuration et/ou introduire une couche de validation/désérialisation centralisée des structs config, réduisant la duplication.
- **Aucun changement de contrat externe** : topics MQTT, auto-discovery HA, endpoints HTTP, formats de payload **strictement identiques**.
- Réduction mesurable de la taille des fichiers et du couplage (moins d'`#include` croisés).

### Cas limites
- Reconnexion MQTT, dédup anti-spam, retain HA → comportement préservé après séparation.
- Sauvegarde/chargement config (mutex récursif `config.cpp:24`) → inchangé.
- Auto-discovery HA au boot → mêmes entités publiées.

## Comportement actuel
- `mqtt_manager.cpp`, `web_routes_config.cpp` (et `pump_controller.cpp`) sont des fichiers volumineux multi-responsabilités, avec couplage par globals (~25 `extern`, `config.h:165-185`).

## Contraintes techniques
- ESP32 / FreeRTOS — préserver le modèle de concurrence existant (queues MQTT inter-core, `configMutex` récursif).
- Refactor à **iso-comportement** : aucun changement de topic/endpoint/payload.
- Pas de régression mémoire (surveiller heap/flash après découpage).
- Respecter `kConstants`, `systemLogger`, ArduinoJson 7.
- S'appuyer sur les tests (feature-029) et idéalement étendre la couverture là où c'est possible.

## Fichiers potentiellement concernés
- `src/mqtt_manager.*` — extraction transport vs sérialisation [modif majeure]
- `src/mqtt_serializers.*` ou `src/mqtt_discovery.*` — [nouveaux] sérialisation/auto-discovery par domaine
- `src/web_routes_config.cpp` — dégroupage par sous-domaine [modif]
- `src/config.*` — éventuelle couche de validation centralisée [modif]
- `docs/subsystems/mqtt-manager.md` — architecture après découpage [doc]

## Nouvelles signatures / API (optionnelle)
- Fonctions internes de sérialisation par domaine (ex. `void serializeSensorsState(JsonObject&);`)
- Aucune API publique externe nouvelle (contrats inchangés).

## Sécurité chimique (optionnelle)
- Dosage impliqué : non, **sauf** si le périmètre inclut `pump_controller` → alors `pool-chemistry` obligatoire et comportement de dosage strictement préservé.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 3 (séparation transport/sérialisation = choix structurant ; alternatives crédibles)
- **Sujet pressenti** : architecture MQTT (séparation transport / sérialisation métier / auto-discovery).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Routes de `web_routes_config.cpp` — **inchangées** (refactor interne)

### Champs WebSocket
- aucun

### Topics MQTT
- **inchangés** (mêmes topics, même auto-discovery HA) — non-régression à vérifier

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/mqtt-manager.md` — nouvelle architecture interne
- `docs/subsystems/config.md` — couche de validation (si introduite)

#### Docs transverses
- `CHANGELOG.md` — entrée dette technique (interne, sans impact utilisateur)

#### Docs immuables
- `docs/adr/NNNN-architecture-mqtt.md` — si ADR confirmé

## Critères d'acceptation
- [ ] AC1 — Le transport MQTT ne dépend plus directement des managers métier (sensors/filtration/lighting/pump) ; la sérialisation est isolée.
- [ ] AC2 — Topics MQTT, auto-discovery HA, endpoints HTTP et payloads **identiques** avant/après (diff réseau à blanc).
- [ ] AC3 — Taille et couplage de `mqtt_manager.cpp` et `web_routes_config.cpp` réduits de façon mesurable.
- [ ] AC4 — Aucune régression de reconnexion / dédup / retain MQTT.
- [ ] `pio run` compile sans nouveau warning ; flash et heap surveillés (pas de régression).

## Plan de test manuel
1. Démarrage → mêmes entités auto-discovery dans Home Assistant qu'avant.
2. Coupure/reprise broker → reconnexion et republication identiques.
3. Modification config via UI → persistance et payloads identiques.
4. Comparaison capture MQTT (mosquitto_sub) avant/après → diff nul.

## Notes pour agent
- **Refactor à iso-comportement** : la non-régression des contrats MQTT/HTTP est l'invariant central ; capturer le trafic MQTT avant/après pour le prouver.
- Faire **après** feature-029 (qui allège déjà `pump_controller`) pour éviter les conflits ; décider du périmètre `pump_controller` avec l'architect.
- Préserver strictement le modèle de concurrence (queues, mutex récursif).

## Hors périmètre
- Suppression du couplage par globals à l'échelle du projet (chantier trop large ; se limiter aux fichiers ciblés).
- Changement de bibliothèque MQTT.
- Modification des contrats externes (topics, endpoints).
