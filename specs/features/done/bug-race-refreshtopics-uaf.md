# Bug — Course inter-tâches sur `refreshTopics()` : lecture de `String` en cours de réassignation (UAF potentiel)

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : done — implémenté, testé (316/316 natifs dont 27 nouveaux `test_native_mqtt_topics`), revue OK, documenté ; en attente de commit
- **Version cible** : 2.10.0 (même lot que feature-059, non déployée)
- **Créée** : 2026-07-22 (découvert par analyse statique pendant l'investigation de `bug-spam-sensor-problem-serial.md` — bug latent, aucun crash attribué à ce jour)
- **Domaine** : MQTT (concurrence) — aucun chemin de dosage touché
- **Priorité suggérée** : haute (UAF = corruption heap possible ; le projet a un historique de gels/crashs réseau inexpliqués, cf. ADR-0029/0030)

## Symptôme observé

Aucun symptôme directement attribué. Bug latent identifié par lecture de code. Candidat crédible comme contributeur aux instabilités heap/réseau historiques (les gels « appareil injoignable » traités par containment en 2.9.1/2.9.2).

## Cause racine (analyse statique 2026-07-22)

`refreshTopics()` (mqtt_manager.cpp:279 → 101) **réassigne ~70 membres `String` de la struct `topics`** depuis **mqttTask (core 0)** à chaque tentative de connexion MQTT, pendant que **loopTask (core 1)** peut lire ces mêmes `topics.*` concurremment — ex. `publishFiltrationState()` appelé depuis `filtration.cpp:252`.

Une réassignation de `String` Arduino libère/réalloue le buffer interne : un lecteur concurrent peut déréférencer un pointeur libéré (**use-after-free**) ou lire un buffer en cours de copie. Conséquences possibles : topic tronqué/corrompu publié, corruption de heap silencieuse, crash apériodique très difficile à reproduire — d'autant plus fréquent que le broker flappe (chaque tentative de reconnexion réassigne les 70 String).

## Correctif envisagé (à arbitrer par l'architect)

Options par ordre de préférence :
1. **Ne réassigner que si changement réel** : `refreshTopics()` ne fait rien si le préfixe/config topics n'a pas changé depuis le dernier appel (cas de très loin le plus fréquent : reconnexions sans changement de config) — supprime ~100 % des réassignations en pratique sans toucher au modèle de concurrence.
2. Protéger `topics` par mutex (lourd : ~70 String, lecteurs multiples dans plusieurs fichiers).
3. Double buffer + pointeur atomique vers la génération courante.

L'option 1 est probablement suffisante seule ; à confirmer en vérifiant tous les chemins qui modifient la config des topics (changement de préfixe MQTT via UI → passe par où ?).

## Critères d'acceptation

- [x] Aucune réassignation des `String` de `topics` pendant qu'un lecteur peut y accéder (par élimination des réassignations répétées et/ou synchronisation) — options 1 + 2 combinées : early-return si préfixe normalisé inchangé + `topicsMutex` (writer = `refreshTopics()`/mqttTask, lecture cross-task = `enqueueOutbound()` seule).
- [x] Le changement de configuration MQTT (préfixe/topics) à chaud reste fonctionnel — `POST /save-config` → `requestReconnect()` (feature-018) → `refreshTopics()` au prochain connect détecte `base != topics.base` et reconstruit.
- [x] Aucun chemin de dosage touché.
- [x] `pio run` sans warning nouveau, suites natives vertes (316/316, dont 27 nouveaux dans `test/test_native_mqtt_topics/` pour le module pur `mqtt_topics_logic`).

## Notes

- Découverte documentée dans la section Investigation de `bug-spam-sensor-problem-serial.md` (hypothèse 5, « bug latent distinct »).
- Cosmétique à corriger au passage : commentaire mqtt_manager.cpp:450 (« kMaxPayloadLen=384 » vs constante réelle 128).

## Notes d'implémentation (v2.10.0, 2026-07-23)

- **Cycle** : architect → web-ui-developer → tests → code-reviewer (**Approved** sans changement requis) → doc-writer, une itération.
- **Approche retenue (architect)** : option 1 de la spec (rebuild uniquement si le préfixe normalisé change) **+** fermeture complète de la fenêtre résiduelle par `topicsMutex`, rendue triviale par une découverte clé : toutes les lectures cross-task de `topics` passent par l'unique entonnoir `enqueueOutbound()` (les autres lecteurs tournent dans mqttTask, séquentiels avec le writer). Options 2 (mutex chez tous les lecteurs) et 3 (double buffer, +4 Ko heap) écartées.
- **Fichiers créés** : `src/mqtt_topics_logic.{h,cpp}` (module pur `normalizeMqttBase` sur buffers C — le stub `String` natif est inutilisable ; comportement historique conservé, y compris la non-idempotence assumée « pool / » → « pool␣ », sans effet : le guard ne re-normalise jamais sa sortie), `test/test_native_mqtt_topics/` (27 tests : trim, slashes, fallback, déterminisme, bornes canari).
- **Fichiers modifiés** : `src/mqtt_manager.h` (`topicsMutex` + contrat), `src/mqtt_manager.cpp` (mutex créé dans `begin()` avant la tâche ; `refreshTopics()` : snapshot `mqttCfg.topic` sous configMutex — 2ᵉ course corrigée au passage — relâché avant topicsMutex, jamais imbriqués ; early-return si base inchangée ; rebuild sous topicsMutex, log hors section critique ; `enqueueOutbound()` : strlcpy du topic sous topicsMutex, timeout → drop + `warnTopicsMutexTimeout` anti-ré-entrance ; commentaire kMaxTopicLen=64/kMaxPayloadLen=128 corrigé), `platformio.ini` (module pur en build natif).
- **Revue** : deadlock impossible (ordre config→topics unidirectionnel vérifié ligne par ligne) ; 2 points mineurs acceptés (lecture `topic.length()` hors mutex — atomique, pire cas 1 message droppé ; timeout configMutex au 1ᵉʳ refreshTopics → LWT vide sur la 1ʳᵉ tentative, auto-réparé). Hors périmètre noté pour plus tard : `reconnectRequested` en `std::atomic<bool>` (race bénigne préexistante).
- **Bénéfice collatéral** : plus aucune réallocation des ~90 `String` sur reconnexion sans changement de config (anti-fragmentation heap sur broker instable).
- **Validation** : `pio run` SUCCESS 0 warning, 316/316 tests natifs (10 suites). Docs : `docs/subsystems/mqtt-manager.md` (section « Reconstruction des topics »), `CHANGELOG.md` [2.10.0]. Pas d'ADR (correctif local au modèle ADR-0011).
- **Reste on-target** : changer le préfixe topic via l'UI broker connecté (reconnexion + nouveaux topics dans HA) ; vérifier l'absence du log « Topics MQTT reconstruits » sur reconnexions sans changement.
