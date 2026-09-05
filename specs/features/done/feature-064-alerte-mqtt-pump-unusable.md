## Métadonnées
- **ID** : feature-064
- **Slug** : alerte-mqtt-pump-unusable
- **Type** : feature
- **Statut** : done — implémentation livrée (double GO pool-chemistry P1+P2, revue Approved, builds core 2/core 3 + 415 tests natifs verts, docs à jour) ; en attente de commit
- **Priorité** : haute (prérequis de la porte de bascule core 3 — ADR-0034)
- **Version cible** : 2.16.0
- **Dernière mise à jour** : 2026-08-04
- **Spec(s) liée(s)** : feature-063 (exécution migration core 3), ADR-0034

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (zone pump_controller, exigence issue de P1 feature-063)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Publier une alerte MQTT `pump_unusable` lorsqu'une pompe doseuse a été déclarée
inutilisable au boot (échec d'attache LEDC sous core Arduino 3.x, flag
`pumpUsable[]` fail-closed livré en feature-063). Exigence pool-chemistry P1 :
condition BLOQUANTE avant toute production core 3 (porte de bascule ADR-0034).

## Contexte
Depuis feature-063 (v2.15.0), sous core 3, `ledcAttachChannel()` peut échouer au
boot ; la pompe concernée est alors mise en sécurité (`pumpUsable[i] = false`,
duty 0 à vie, log CRITICAL). Mais ce log est local : vu de Home Assistant, la
régulation « compterait » des cycles sans injection réelle → dérive pH/ORP
silencieuse. Pool-chemistry (verdict P1, feature-063) a accepté le log seul pour
le firmware de banc, et exige une alerte MQTT avant toute bascule production.
Le patron existe déjà : alerte différée `history_partition_reset` publiée dans
`setup()` après `mqttManager.begin()` (`src/main.cpp:231-234`), car l'événement
survient avant que MQTT n'existe.

## Comportement attendu
- Au boot, si au moins une pompe a échoué son attache LEDC (core 3 uniquement),
  publier via `mqttManager.publishAlert("pump_unusable", ...)` après
  `mqttManager.begin()`, un message **en français** identifiant la ou les pompes
  (numéro, GPIO, canal LEDC) et précisant que la pompe est hors service jusqu'au
  reboot.
- Une alerte par pompe inutilisable (ou une alerte agrégée — arbitrage architect).
- Sous core 2 (production actuelle), le chemin compile mais ne peut jamais se
  déclencher : l'accesseur retourne « utilisable » inconditionnellement.
- Le comportement de mise en sécurité lui-même (feature-063) est INCHANGÉ :
  cette spec ajoute uniquement de la **visibilité**, en lecture seule du flag.

### Cas limites
- WiFi indisponible au boot (mode AP secours) : `mqttManager.begin()` n'est pas
  appelé → alerte perdue jusqu'au prochain boot. Même limitation assumée que
  `history_partition_reset` — à confirmer par pool-chemistry (question Q1
  ci-dessous).
- Les deux pompes inutilisables : les deux doivent être signalées.
- MQTT configuré mais broker injoignable au boot : comportement de la outQueue
  existante de `publishAlert` (pas de mécanique nouvelle).

### Questions ouvertes (pour architect / pool-chemistry)
- **Q1** : une publication unique au boot suffit-elle (patron
  `history_partition_reset`), ou faut-il une re-publication à chaque
  (re)connexion MQTT pour survivre à un broker absent au boot ? La pompe reste
  inutilisable TOUTE la vie du firmware — un rappel périodique ou
  à-la-reconnexion est défendable pour une alerte de sécurité chimique.

## Comportement actuel
`pumpUsable[2]` (public, `src/pump_controller.h:66`) est posé à `false` dans
`PumpControllerClass::begin()` (`src/pump_controller.cpp:91`) avec log CRITICAL,
sous garde `#if ESP_ARDUINO_VERSION_MAJOR >= 3`. Aucune remontée MQTT ; les
refus de `setManualPump` sont throttlés en log local uniquement.

## Contraintes techniques
- ESP32, offline first
- `pump_controller.begin()` s'exécute AVANT `mqttManager.begin()` dans `setup()`
  → publication différée obligatoire (patron feature-059)
- Le flag est `volatile bool[2]` posé une seule fois au boot, lu ensuite —
  pas de nouvelle mutation, pas de mutex nécessaire (à confirmer code-reviewer)
- Aucun changement du binaire core 2 côté COMPORTEMENT (le code peut différer,
  contrairement à feature-063 : l'accesseur et l'appel `setup()` compilent des
  deux côtés)
- Pas de nouveau topic : réutiliser le canal `publishAlert` existant

## Fichiers potentiellement concernés
- `src/pump_controller.h` / `.cpp` — accesseur public en lecture seule
  (ex. `bool isPumpUsable(int index) const`), bi-core (retourne `true`
  inconditionnellement en core 2)
- `src/main.cpp` — publication différée après `mqttManager.begin()`, à côté du
  bloc `history_partition_reset`
- `src/mqtt_manager.*` — a priori INTOUCHÉ (`publishAlert` existe) ; ne toucher
  que si Q1 impose une re-publication à la reconnexion
- `docs/MQTT.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`,
  `docs/adr/0034-migration-core3-idf5.md` (cocher l'item de la porte de bascule)

## Livrables — check-list
- [x] A1 — Accesseur public lecture seule de l'état d'utilisabilité pompe dans
      `pump_controller.h/.cpp`, bi-core (core 2 : toujours utilisable) —
      `bool isPumpUsable(int index) const`, index hors bornes → false (fail-closed)
- [x] A2 — Publication `pump_unusable`, message français avec pompe/GPIO/canal —
      **implémentée dans `connectInTask()` (mqtt_manager.cpp) et non dans
      `setup()`** : conséquence de l'arbitrage Q1 (voir A3), aucun bloc main.cpp
- [x] A3 — Réponse à Q1 implémentée : **re-publication à CHAQUE (re)connexion
      MQTT RETENUE** (décision pool-chemistry P1) au lieu du one-shot boot
      (patron `history_partition_reset` écarté) — `publishAlert` est
      non-retained et l'état est permanent jusqu'au reboot : broker absent au
      boot ne perd plus l'alerte. Limitation résiduelle assumée : HA qui
      redémarre sans reconnexion ESP32 ne revoit pas l'alerte.
- [x] D1 — `pio run` (core 2) sans nouveau warning
- [x] D2 — `~/.platformio/penv/bin/pio run -e esp32dev_core3` SUCCESS
- [x] D3 — `pio test -e native` : zéro régression (415 tests)
- [x] C1 — `docs/MQTT.md` : type d'alerte `pump_unusable` documenté
- [x] C2 — `docs/subsystems/pump-controller.md` : accesseur + visibilité MQTT
      (+ `docs/subsystems/mqtt-manager.md` : hook `connectInTask()`)
- [x] C3 — `CHANGELOG.md` : entrée 2.16.0
- [x] C4 — ADR-0034 : item « alerte MQTT pump_unusable » de la porte de bascule
      marqué livré

## Nouvelles signatures / API
- Fonctions publiques : `bool isPumpUsable(int index) const;` dans
  `src/pump_controller.h` (nom définitif : architect)
- Topics MQTT nouveaux : aucun — nouvelle valeur `alertType` = `pump_unusable`
  sur le canal d'alerte existant

## Sécurité chimique
- Dosage impliqué : non directement — aucun chemin d'injection modifié, lecture
  seule du flag de mise en sécurité feature-063
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : N/A (pas d'action physique)
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : régression involontaire dans `pump_controller` (zone
  sensible) → validation pool-chemistry du diff obligatoire ; l'accesseur ne
  doit JAMAIS servir à réactiver une pompe (lecture seule stricte)

## ADR attendu ?
- **Probable** : non — implémentation d'une exigence déjà consignée dans
  l'ADR-0034 (accepté). Simple mise à jour de sa check-list.
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- Nouvelle valeur d'alerte `pump_unusable` sur le canal `publishAlert` existant
  (pas de nouveau topic, pas de nouvelle entité auto-discovery HA — à confirmer
  par architect si une entité binary_sensor dédiée serait plus utile dans HA)

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — accesseur + remontée MQTT
#### Docs transverses
- `docs/MQTT.md` — alerte `pump_unusable`
- `CHANGELOG.md` — entrée 2.16.0
#### Docs immuables
- `docs/adr/0034-migration-core3-idf5.md` — cocher l'item de la porte de bascule

## Critères d'acceptation
- [x] Sous core 3, si `ledcAttachChannel` échoue pour une pompe au boot, une
      alerte MQTT `pump_unusable` est publiée après l'init MQTT, en français,
      identifiant la pompe (et re-publiée à chaque reconnexion — arbitrage Q1)
- [x] Sous core 2, aucune alerte ne peut être émise et le comportement est
      strictement inchangé (`isPumpUsable()` retourne inconditionnellement true)
- [x] Aucun chemin d'écriture nouveau vers `pumpUsable[]` ni vers les pompes
      (accesseur const, lecture seule stricte, aucun setter — validé
      pool-chemistry + code-reviewer)
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` compile sans nouveau warning
- [x] `pio run -e esp32dev_core3` compile SUCCESS
- [x] `pio test -e native` : zéro régression

## Plan de test manuel
1. Nominal core 2 : boot normal → aucune alerte `pump_unusable`, pompes OK.
2. Core 3 banc (2ᵉ ESP32, quand disponible) : forcer un échec d'attache (canal
   invalide en debug) → log CRITICAL + alerte MQTT visible sur le broker.
3. Broker MQTT coupé au boot puis rétabli : vérifier le comportement retenu
   pour Q1 (perte assumée ou re-publication).

## Notes pour agent
- NE PAS modifier la logique de mise en sécurité feature-063 (gardes
  `applyPumpDuty`, `setManualPump`) — visibilité uniquement.
- Patron de référence : `history.partitionWasAutoErased()` +
  `publishAlert("history_partition_reset", ...)` dans `src/main.cpp:228-234`.
- La production reste en core 2 : ce code est dormant en prod, actif sur le banc.

## Hors périmètre
- Alarme écran UART pour ce cas (peut venir plus tard)
- Entité auto-discovery HA dédiée si l'arbitrage architect conclut que le canal
  d'alerte existant suffit
- Toute logique de « réactivation » d'une pompe sans reboot

## Notes d'implémentation (clôture 2026-08-04)
- **Cycle** : orchestrator → spec-reader → architect → pool-chemistry P1
  (GO SOUS CONDITIONS) → embedded-developer → pool-chemistry P2 (GO) →
  test-engineer (D1/D2/D3 verts) → code-reviewer (Approved) → doc-writer.
- **Q1 tranché (pool-chemistry, décisionnel)** : re-publication à CHAQUE
  (re)connexion MQTT, hook dans `connectInTask()` branche connectée après
  `publishDiagnosticInternal()`. Le bloc « publication au boot dans setup() »
  du plan initial (A2) a été SUPPRIMÉ : la 1ʳᵉ connexion couvre le boot, les
  reconnexions couvrent le reste (publishAlert non-retained + état permanent).
  A3 = implémenté (main.cpp intouché, mqtt_manager.cpp porte le hook).
  Limitation résiduelle assumée : HA redémarré sans reconnexion ESP32 ne
  revoit pas l'alerte.
- **Accesseur définitif** : `bool isPumpUsable(int index) const`
  (`pump_controller.h` section getters). Bi-core : core 3 reflète
  `pumpUsable[]` ; core 2 toujours true pour 0/1 ; hors bornes → false
  (fail-closed). Aucun setter.
- **Conditions P1 satisfaites (vérifié P2)** : hook branche connectée seule ✓,
  rôle chimique dans le message (« Pompe 1 (pH) » / « Pompe 2 (Chlore/ORP) ») ✓,
  test broker coupé/rétabli consigné au plan de test manuel ✓.
- **Concurrence (confirmé code-reviewer)** : lecture `pumpUsable` depuis
  mqttTask sûre sans mutex — écrit une fois dans begin() (loopTask) AVANT
  xTaskCreatePinnedToCore(mqttTask), happens-before par création de tâche +
  volatile.
- **Builds** : `pio run` SUCCESS 0 warning (Flash 82,2 %) ;
  `esp32dev_core3` SUCCESS 0 warning (Flash 92,3 %) ; 415/415 tests natifs.
  Pas de test natif nouveau (aucune logique pure au sens ADR-0017 : garde de
  compilation + flag matériel + PubSubClient).
- **Nit revue (non bloquant, non appliqué)** : `pumpPins`/`pumpChannels`
  locaux pourraient être `static constexpr` — laissé tel quel pour ne pas
  invalider le hunk validé P2 (coût négligeable, 2 tableaux de 2 éléments
  sur la pile, chemin froid).
- **Collatéral** : `src/idf_component.yml{,.orig}` générés par pioarduino
  (builds core 3) ajoutés au `.gitignore` (artefacts, pas du code projet).
- **Reste à l'humain** : plan de test manuel sur banc core 3 (échec d'attache
  forcé → alerte broker ; broker coupé au boot → alerte à la reconnexion),
  prérequis porte de bascule ADR-0034.
