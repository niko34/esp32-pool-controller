## Métadonnées
- **ID** : feature-029
- **Slug** : tests-testabilite-dosage
- **Type** : feature
- **Statut** : **clôturée — supersédée** (2026-07-06, réalisée par les features 036-041 sans référence à cette spec)
- **Priorité** : haute (à la rédaction — satisfaite)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-06

## Note de clôture (2026-07-06)

Spec issue de la revue de code du 2026-06-20, **intégralement supersédée** : la campagne de testabilité des features 036-041 (fin juin, v2.2.6-2.2.11) est littéralement sa réalisation, en plus large :

| Exigence spec | Réalisation v2.11.2 |
|---|---|
| Unité pure `dosing_policy` (`DosingInputs`/`DosingDecision`/`evaluateDosing`) | ✅ `src/dosing_logic.*` (036) : `DoseInputs` → `evaluateDose()` — **15 gardes** (10 à la rédaction ; le module a absorbé les gardes ajoutées depuis) |
| Refactor à comportement constant validé | ✅ Characterization refactor, pool-chemistry GO + code-reviewer Approved (cycle 036) — AC3 mot pour mot |
| Anti-rafale 1 min/15 min | ✅ `countCyclesInWindow`/`recordCycleTimestamp` (039), frontières testées |
| Rollover minuit, `now` injecté | ✅ `shouldRolloverByDate`/`shouldRolloverByMillis` (039) |
| Wrap `millis()` 32 bits | ✅ Testé (039) |
| AC2 (filtration OFF/NaN/filtre non prêt/cal incomplète/limites/anti-rafale/rollover) | ✅ Tous couverts — **74 tests dosing, 100 % lignes**, 185 tests en CI |
| ADR « politique de dosage testable » | ✅ **ADR-0017** (Humble Object) — le sujet pressenti exact |
| BUILD.md + pump-controller.md | ✅ Faits (036-041) |

Livré au-delà de la spec : PID pur (`computePidPure` — optionnel dans la spec), gardes d'injection manuelle (006), répartition scheduled (011), détection capteur figé (022) — tous nés testables dans le même module. **Aucun résidu à reporter.** Aucun code modifié au titre de cette spec. Contenu conservé ci-dessous pour traçabilité.
- **Spec(s) liée(s)** : `specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md` (a introduit `test/test_native/` + `env:native`)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — la logique de dosage est extraite/refactorée pour être testable : risque de régression sécurité
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Rendre la **logique critique de dosage testable hors matériel** et la couvrir par des tests unitaires : extraire les décisions de `canDose()`, l'anti-cycling/anti-rafale et le rollover journalier derrière une interface pure (sans dépendances Arduino/globals), puis ajouter une suite de tests dans l'environnement `native` existant.

## Contexte
La revue de code (2026-06-20) a relevé que la testabilité est quasi nulle : **un seul module testé** (`test/test_native/test_sensor_filter.cpp`), précisément parce que c'est le seul sans dépendance Arduino/globals. La logique la plus critique du système — `canDose()` (10 gardes fail-closed), anti-cycling, anti-rafale 1 min/15 min, limites jour/heure, rollover journalier — n'est **pas vérifiable hors matériel**. Pour un système de **dosage chimique**, l'absence de filet de sécurité automatisé sur ces chemins est un risque élevé : toute évolution future (cf. feature-025 qui a modifié l'entrée PID) repose sur de la relecture manuelle. L'infra de test native existe déjà (`env:native`, feature-025) — il faut l'étendre.

## Comportement attendu
- La logique de décision de dosage est isolée dans une (ou des) unité(s) **pure(s)** : entrées = état explicite (mesures, limites, timestamps, flags filtration/calibration/filtre), sortie = décision + raison. Pas d'accès direct aux globals ni au matériel.
- `pump_controller` consomme cette unité ; le comportement runtime reste **strictement identique** (refactor sans changement fonctionnel).
- Une suite de tests `native` couvre les chemins critiques (voir livrables), exécutable via `pio test -e native`.
- La couverture inclut explicitement les invariants de sécurité chimique (fail-closed).

### Cas limites
- Rollover à minuit (réinitialisation des cumuls journaliers) testé sans dépendre de l'horloge réelle (injection d'un `now` simulé).
- Débordement `millis()` (wrap 32 bits) sur les fenêtres anti-rafale.
- Filtration OFF / capteur NaN / filtre non prêt / calibration incomplète → dosage refusé avec la bonne raison.

## Comportement actuel
- Seul `sensor_filter` est testé.
- `canDose()` et la logique associée sont imbriqués dans `pump_controller.cpp` (1195 l.) avec dépendances aux globals et au matériel → non testables hors carte.

## Contraintes techniques
- ESP32 / PlatformIO — réutiliser `env:native` + Unity (déjà en place, feature-025).
- Le refactor ne doit **rien changer** au comportement de dosage (validé `pool-chemistry` + `code-reviewer`).
- Pas d'allocation dynamique récurrente introduite ; déterminisme (timestamps injectés, pas de `millis()` réel dans l'unité pure).
- Respecter `kConstants`, `systemLogger`.

## Fichiers potentiellement concernés
- `src/dosing_policy.*` — [nouveau] unité pure de décision (nom à valider par architect)
- `src/pump_controller.*` — délégation vers l'unité pure, sans changement fonctionnel [modif]
- `test/test_native/test_dosing_policy.cpp` — [nouveau] suite de tests
- `platformio.ini` — env native déjà présent ; ajuster `build_src_filter` si besoin [modif]
- `docs/subsystems/pump-controller.md` — note testabilité / architecture [doc]

## Nouvelles signatures / API (optionnelle)
- `struct DosingInputs { ... };` / `struct DosingDecision { bool allowed; Reason reason; };`
- `DosingDecision evaluateDosing(const DosingInputs&);` dans `src/dosing_policy.h`

## Sécurité chimique (optionnelle — obligatoire)
- Dosage impliqué : oui — produit : pH- (acide), chlore liquide.
- Limites journalières / horaires concernées : oui — toutes (la logique extraite les évalue).
- Filtration requise avant action : oui (inchangé).
- Calibration capteur modifiée : non.
- Algorithme PID touché : non (seule la décision canDose est extraite ; le PID reste tel quel, sauf si l'architect décide d'inclure son anti-windup dans l'unité pure).
- Risque identifié : régression silencieuse lors de l'extraction → **mitigé par tests + double validation pool-chemistry/code-reviewer**.

## ADR attendu ?
- **Probable** : oui (faible)
- **Critère(s) candidat(s)** : 1 (interface de décision de dosage = contrainte structurante pour les évolutions futures)
- **Sujet pressenti** : isolation de la politique de dosage derrière une interface pure testable.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun (comportement runtime inchangé)

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — architecture de la politique de dosage + testabilité

#### Docs transverses
- `docs/BUILD.md` — comment lancer `pio test -e native` (si pas déjà documenté)
- `CHANGELOG.md` — entrée qualité/tests

#### Docs immuables
- `docs/adr/NNNN-politique-dosage-testable.md` — si ADR confirmé

## Critères d'acceptation
- [ ] AC1 — La logique `canDose()` (10 gardes), anti-rafale et rollover journalier est évaluée par une unité pure sans dépendance matériel/globals.
- [ ] AC2 — `pio test -e native` exécute une suite couvrant : filtration OFF, NaN, filtre non prêt, calibration incomplète, limite journalière atteinte, limite horaire, anti-rafale 1 min/15 min, rollover minuit, wrap `millis()`.
- [ ] AC3 — Le comportement runtime de dosage est inchangé (revue `pool-chemistry` GO + `code-reviewer` Approved).
- [ ] AC4 — Tous les tests passent.
- [ ] `pio run` compile sans nouveau warning.

## Plan de test manuel
1. `pio test -e native` → tous verts.
2. Comparaison avant/après sur carte : un cycle de dosage nominal se comporte identiquement.
3. Scénario limite journalière atteinte → refus identique à l'existant.

## Notes pour agent
- C'est un **refactor à comportement constant** : la valeur est dans les tests, pas dans un changement fonctionnel. Toute divergence de comportement est un bug, pas une amélioration.
- `architect` : décider du périmètre exact extrait (canDose seul vs canDose + anti-windup PID).
- Réutiliser le shim `test/native_shim/Arduino.h` de feature-025 si nécessaire.

## Hors périmètre
- Tests d'intégration matériels (HIL).
- Refonte du PID (les coefficients/anti-windup restent ceux de feature-025).
- Découpage complet de `pump_controller` (cf. feature god objects).
