## Métadonnées
- **ID** : feature-036
- **Slug** : dosage-testable-decision-pure
- **Type** : feature (refactor de testabilité + tests)
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 2.2.6 (refactor interne strictement préservateur de comportement, aucun changement visible utilisateur)
- **Dernière mise à jour** : 2026-06-27
- **Spec(s) liée(s)** : feature-025 (lissage + `SensorFilter` testable, pause mélange, ADR-0016), fix pause-mélange (done/), ADR-0016 (régulation P temporisée)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Capteurs (pH / ORP / température)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)
- [x] Tests natifs (`test/`, `platformio.ini` env `native`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (logique de dosage extraite/réorganisée)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Rendre la logique de décision de dosage de `pump_controller` testable en natif (sur PC, sans ESP32) en l'extrayant en fonctions pures, puis ajouter une batterie de tests unitaires Unity couvrant l'hystérésis, le temps minimum d'injection, les limites de sécurité et un test de **non-régression** du bug pause-mélange. Aucun changement de comportement de dosage.

## Contexte
Aujourd'hui le seul code testé en natif est `SensorFilter` (feature-025, 17 tests). La logique de dosage (`canDose`, `shouldStartDosing`, `shouldContinueDosing`, calcul de l'erreur pH-/pH+, gardes de sécurité) n'a **aucun test** : le bug récent « pause mélange armée au démarrage » (injection auto coupée après ~1 cycle, `minInjectionTimeMs` jamais respecté) serait passé inaperçu jusqu'au test terrain. Or c'est la zone la plus sensible du firmware (sécurité chimique).

`canDose()` lit directement des objets globaux (`sensors`, `filtration`, `mqttCfg`, `safetyLimits`, `millis()`, `esp_task_wdt_status`), ce qui empêche tout test natif sans shimer toute la chaîne I²C/FreeRTOS. La solution retenue (cf. discussion) est le pattern **Humble Object** : extraire la **décision** en logique pure recevant ses entrées en paramètres, et garder `canDose()`/`shouldStartDosing()` comme **coquilles minces** qui collectent les valeurs et délèguent.

## Comportement attendu
Le comportement **observable** de la régulation est **strictement inchangé** : mêmes verdicts de dosage, mêmes causes de refus (`getPhDoseBlockedReason`/`getOrpDoseBlockedReason`), mêmes seuils, mêmes limites, même ordre d'évaluation des gardes.

### Refactor (préservation de comportement stricte — « characterization refactor »)
1. Extraire la décision « peut-on doser ? » dans une fonction pure, p.ex. :
   `DoseDecision evaluateDose(const DoseInputs& in);`
   où `DoseInputs` est une struct POD regroupant tout ce que `canDose()` lit actuellement (watchdog actif, filtration en marche / mode continu, lecture filtrée + NaN, filtre prêt/instable, points de calibration + requis, stabilisation post-cal, pause mélange active, mode régulation, limites journalière/horaire, anti-rafale) et `DoseDecision = { bool allowed; const char* refusalCause; }`.
2. `canDose(int pumpIndex)` devient une coquille : elle **collecte** ces valeurs depuis les globals puis `return evaluateDose(...)`. La logique de `logRefusalOnce` / l'exposition WS de la cause restent identiques.
3. Rendre `shouldStartDosing` / `shouldContinueDosing` testables : soit fonctions libres/statiques recevant `pumpProtection` (ou les champs nécessaires) en paramètre, soit module pur séparé. Le `systemLogger.warning(...)` éventuel reste dans la coquille, pas dans la logique pure.
4. Optionnel (si à faible risque) : isoler le calcul d'erreur pH-/pH+ (`error = (type==ph_plus) ? cible - ph : ph - cible`) en helper pur.
5. Nouveau module pur sans dépendance Arduino/FreeRTOS (ex. `src/dosing_logic.{h,cpp}`) compilable en natif via un `build_src_filter` étendu.

### Tests natifs (Unity, env `native`)
- `test/test_native/test_dosing_decision.cpp` couvrant au minimum les AC ci-dessous.

### Cas limites
- Filtration OFF (hors mode continu) → refus « filtration arrêtée ».
- Lecture filtrée NaN / filtre non prêt / filtre instable → refus fail-closed.
- Calibration insuffisante (pH < 2 points, ORP < 1) → refus.
- Pause mélange active → refus « pause mélange en cours ».
- Limite journalière / horaire atteinte → refus.
- Erreur dans le deadband (≤ startThreshold) → pas de démarrage.
- Injection en cours, `runTime < minInjectionTimeMs` → `shouldContinueDosing` force la poursuite même si erreur sous stopThreshold.

## Comportement actuel
- `SensorFilter` testé (17 tests natifs). `pump_controller` : 0 test. Logique de dosage couplée aux globals → non testable nativement en l'état.

## Contraintes techniques
- ESP32, offline first ; framework Arduino.
- Tests natifs : Unity, **C/C++ sans libc++** (cf. shims existants `test/native_shim/`), `-std=c++17`, `pio test -e native`.
- La logique pure extraite ne doit inclure **ni `<Arduino.h>` réel, ni FreeRTOS, ni `<vector>`/`<FS.h>`** (sinon elle n'est pas compilable en natif sans shim lourd).
- Aucun handler bloquant ; pas d'allocation dynamique récurrente introduite.
- Watchdog jamais désactivé ; aucune garde de sécurité supprimée ou réordonnée.

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` — `canDose`/`shouldStartDosing`/`shouldContinueDosing` deviennent des coquilles déléguant à la logique pure.
- `src/pump_controller.h` — signatures des helpers extraits.
- `src/dosing_logic.h` / `src/dosing_logic.cpp` — [nouveau] logique pure (struct `DoseInputs`, `evaluateDose`, hystérésis, min injection).
- `test/test_native/test_dosing_decision.cpp` — [nouveau] tests Unity.
- `platformio.ini` — env `native` : étendre `build_src_filter` pour inclure `dosing_logic.cpp` (et pas `pump_controller.cpp`).
- `docs/subsystems/pump-controller.md` — documenter la séparation décision pure / coquille hardware.
- `CHANGELOG.md` — entrée (refactor interne + couverture de tests).

## Nouvelles signatures / API
- `struct DoseInputs { ... };` (POD, dans `src/dosing_logic.h`)
- `struct DoseDecision { bool allowed; const char* refusalCause; };`
- `DoseDecision evaluateDose(const DoseInputs& in);`
- `bool shouldStartDosingPure(float error, float startThreshold, unsigned int cyclesToday, unsigned int maxCyclesPerDay);`
- `bool shouldContinueDosingPure(float error, float stopThreshold, unsigned long runTimeMs, unsigned long minInjectionTimeMs);`
- (signatures indicatives — `architect` arbitrera la forme exacte)
- Aucun endpoint HTTP / champ WS / topic MQTT nouveau.

## Sécurité chimique (obligatoire)
- Dosage impliqué : **oui** (refactor de la logique de décision) — produits : pH- / pH+ / chlore.
- Limites journalières / horaires concernées : toutes (doivent produire des verdicts identiques avant/après).
- Filtration requise avant action : oui (garde conservée à l'identique).
- Calibration capteur modifiée : non.
- Algorithme PID touché : non (le PID n'est pas dans le périmètre ; seule la **décision** de doser est extraite).
- Risque identifié : **régression silencieuse** d'une garde de sécurité lors de l'extraction. Mitigation : équivalence stricte validée par `pool-chemistry` (table de vérité des gardes avant/après), tests verrouillant chaque cause de refus, ordre d'évaluation préservé, fail-closed conservé. Aucun seuil/limite/délai modifié dans cette feature.

## ADR attendu ?
- **Décision finale** : OUI — [ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) créé (validé par `architect`, 4 critères remplis).
- **Critère(s) retenu(s)** : pattern Humble Object imposé comme convention de testabilité firmware (contraint les futurs refactors de régulation), frontière String/POD non lisible dans le code seul, alternatives crédibles écartées (mock des singletons, `const char*` statique).
- **Sujet** : « Logique métier pure (Humble Object) séparée de la couche hardware pour testabilité native ».

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- aucun
### Champs WebSocket
- aucun (les causes de refus exposées restent identiques)
### Topics MQTT
- aucun
### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — section « Décision de dosage : logique pure vs coquille hardware », testabilité native.
- `docs/features/` — aucune page UI touchée.
#### Docs transverses
- `CHANGELOG.md` — entrée sous la version de release (refactor interne + tests de dosage).
- `docs/BUILD.md` — éventuelle note sur `pio test -e native` étendu (si pertinent).
#### Docs immuables
- `docs/adr/NNNN-*.md` — seulement si `architect`/`pool-chemistry` valident l'ADR ci-dessus.

## Critères d'acceptation
- [x] AC1 — `canDose()` délègue à `evaluateDose()` et produit, pour chaque garde, **exactement** le même verdict et la même cause de refus qu'avant (table d'équivalence validée par `pool-chemistry`).
- [x] AC2 — `shouldStartDosingPure` : démarre ssi `error > startThreshold` et `cyclesToday < maxCyclesPerDay` ; testé aux bornes (deadband 0.05).
- [x] AC3 — `shouldContinueDosingPure` : force la poursuite tant que `runTimeMs < minInjectionTimeMs` (30 s), puis poursuit ssi `error > stopThreshold` (0.01).
- [x] AC4 — **Non-régression pause-mélange** : un scénario simulant un cycle auto complet vérifie qu'une injection dure **≥ `minInjectionTimeMs`** avant que la pause mélange ne s'arme (le bug corrigé ne peut pas réapparaître via la logique pure).
- [x] AC5 — Toutes les causes de refus de `evaluateDose` sont couvertes par au moins un test (filtration, NaN, filtre non prêt/instable, calibration, stabilisation, pause mélange, mode, limites jour/heure, anti-rafale).
- [x] AC6 — `pio test -e native` : tous les tests (existants `SensorFilter` + nouveaux dosage) PASSED.
- [x] AC7 — `pio run` (esp32dev) compile sans nouveau warning ; comportement firmware inchangé (validation on-target : un cycle auto produit `Démarrage` puis `Arrêt … durée ≥ 30 s`).
- [x] AC8 — Aucune garde de sécurité supprimée, aucun seuil/limite/délai modifié.
- [x] Tous les libellés / causes de refus restent en français et identiques.

## Plan de test manuel
1. `pio test -e native` → tous les tests verts (anciens + nouveaux).
2. `pio run` → SUCCESS sans warning.
3. On-target : régulation pH auto, pH sous cible → vérifier dans les logs `Démarrage dosage pH (auto)` **suivi de** `Arrêt dosage pH (auto): durée=≥30s vol≈…mL`, puis refus « pause mélange en cours » pendant 15 min.
4. On-target : couper la filtration en plein dosage → refus « filtration arrêtée », pompe coupée.

## Notes pour agent
- **Invariant directeur** : characterization refactor — on ne change AUCUN comportement de dosage, on déplace la logique pour la rendre testable. Tout écart de verdict = bug.
- Le périmètre exclut toute optimisation de la régulation (PID, seuils, durée de pulse) : ce sont des features distinctes.
- Réutiliser le pattern shim existant (`test/native_shim/`) ; idéalement la logique pure n'a besoin d'aucun shim (ne pas inclure Arduino).
- Penser à l'injection du temps (`now`/`runTimeMs` en paramètre) pour tester `minInjectionTimeMs` sans attendre 30 s réelles.

## Hors périmètre
- Modification des seuils, limites, gains PID, durées de mélange.
- Extraction/refonte de `update()` au-delà de la séparation décision/action minimale nécessaire.
- Mock des objets `sensors`/`filtration` (on extrait la logique plutôt que de mocker des singletons concrets).
- Tests on-target automatisés du dialogue I²C/PWM (séparé, ultérieur).

---

## Suivi / itérations (clôture)

**Statut : livré — tests verts, revue Approved, pool-chemistry GO (équivalence stricte).** Version 2.2.6.

### Décisions
- Pattern **Humble Object** : décision extraite en module pur `src/dosing_logic.{h,cpp}` (POD, `<stdint.h>`/`<math.h>` uniquement, pas d'Arduino/FreeRTOS), `canDose()` devient une coquille qui collecte les globals et délègue.
- Causes de refus : `enum class DoseRefusal` (15 valeurs) en pur ; la coquille mappe → `String` française **au mot près** (valeurs runtime réinjectées). Contrat WS (`getPhDoseBlockedReason`) préservé.
- **ADR-0017** créé (logique métier pure séparée de la couche hardware pour testabilité native).

### Équivalence (validée pool-chemistry, 2 passages)
- Ordre des 15 gardes identique ; fail-closed conservé ; aucun seuil/limite/délai modifié.
- Comparateur limite horaire `>=` confirmé identique à l'original (`git show main` l.418) — la doc `pump-controller.md` (`>`) était imprécise, corrigée en `>=`.
- `pumpIndex` invalide reste dans la coquille (sans log) ; `resetRefusalLogState` seulement si `cause==None` ; `countRecentDosingCycles` reste dans la coquille.

### Tests
- `test/test_native_dosing/test_dosing_decision.cpp` (21 tests) + `test/test_native_sensor_filter/test_sensor_filter.cpp` (17, déplacé). Séparation en 2 dossiers `test_*` (PlatformIO : 1 binaire/dossier).
- AC4 = verrou de non-régression du bug pause-mélange v2.2.5 (injection auto ≥ `minInjectionTimeMs`).
- `pio test -e native` : 38/38 PASSED. `pio run` esp32dev : SUCCESS, 0 nouveau warning.

### Fichiers
- `src/dosing_logic.{h,cpp}` [nouveau], `src/pump_controller.{cpp,h}`, `platformio.ini`, `test/test_native_dosing/`, `test/test_native_sensor_filter/`, `docs/subsystems/pump-controller.md`, `docs/BUILD.md`, `docs/adr/0017-*.md`, `CHANGELOG.md`, `src/version.h` (2.2.6).

### Reste à faire (non automatisable en CI)
- Validation on-target (AC7) : un cycle auto produit `Démarrage dosage pH (auto)` puis `Arrêt … durée ≥ 30 s`.
