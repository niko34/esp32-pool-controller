## Métadonnées
- **ID** : feature-040
- **Slug** : eclairage-horaire-testable
- **Type** : feature (refactor de testabilité + tests)
- **Statut** : todo
- **Priorité** : basse (éclairage : aucun impact chimie/eau ; complète la couverture d'horaire)
- **Version cible** : N/A — refactor interne strictement préservateur de comportement (bump CORRECTIF optionnel à la release)
- **Dernière mise à jour** : 2026-06-27
- **Spec(s) liée(s)** : feature-038 (`schedule_logic`, module générique d'horaire), ADR-0017 (Humble Object)

## Domaines touchés
- [x] Filtration / Éclairage
- [x] Tests natifs (`test/`, env `native` / `native_coverage`)
- [ ] Régulation / Capteurs / Web / MQTT / UART / OTA

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **NON applicable** (l'éclairage n'a aucun impact dosage/chimie/présence d'eau ; c'est un simple relais de confort)
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Extraire la décision d'horaire de l'éclairage de `lighting.cpp` vers le module pur `schedule_logic` (créé en feature-038, générique et réutilisable), puis ajouter des tests Unity. Aucun changement de comportement.

## Contexte
feature-038 a créé `src/schedule_logic.{h,cpp}` (module pur générique d'horaire) pour la filtration. L'éclairage (`lighting.cpp`) a la **même nature de logique** (parsing "HH:MM", appartenance à une fenêtre, décision marche/arrêt selon horaire/manuel) mais **0 test natif**. C'est le complément naturel de feature-038, à faible coût.

⚠️ **Différence importante à préserver** : l'éclairage et la filtration ne traitent PAS `start == end` de la même façon :
- `filtration` (`schedule_logic::isMinutesInRange`) : `start == end` → **false** (plage invalide → jamais).
- `lighting` (`lighting.cpp::isMinutesInRange` L49) : `start == end` → **true** (éclairage toute la journée).

Le refactor ne doit PAS unifier ce comportement : chaque domaine garde sa sémantique.

Détail des éléments réutilisables / spécifiques :
- `lighting.cpp::timeStringToMinutes` (L39-45) — **identique** à `schedule_logic::timeStringToMinutes` → réutilisable tel quel.
- `lighting.cpp::isMinutesInRange` (L47-54) — diffère sur `start==end` (true) → variante à gérer (paramètre ou fonction dédiée).
- décision dans `update()` (L85-108) : manualOverride → `enabled` ; sinon scheduleEnabled → in-range (ou conserve l'état si temps indispo) ; sinon → `enabled`. **Spécifique éclairage** → nouvelle fonction pure.

## Comportement attendu
Comportement **observable strictement inchangé** : mêmes décisions allumage/extinction pour les mêmes entrées.

### Refactor (characterization, préservation stricte)
- Réutiliser `schedule_logic::timeStringToMinutes` (identique).
- Gérer la variante `start==end` : soit un paramètre `bool equalMeansAlways` sur `isMinutesInRange` (défaut `false` = comportement filtration inchangé ; l'éclairage passe `true`), soit une fonction dédiée `isMinutesInRangeInclusive` — arbitrage architect. **Invariant : le comportement filtration existant (start==end → false) ne doit PAS changer.**
- Créer `decideLightingOn(manualOverride, enabledFlag, scheduleEnabled, haveTime, nowMin, startMin, endMin, currentlyOn) → bool` dans `schedule_logic` (copie exacte de la décision L85-108).
- `lighting.cpp::update()`/`begin()`/`ensureTimesValid()` deviennent des coquilles déléguant ; `millis()`/RTC/`digitalWrite`/`publishState`/MQTT restent dans la coquille.

### Cas limites
- `start == end` → **true** (éclairage allumé toute la journée) — divergence à préserver.
- Fenêtre simple (start < end) et fenêtre franchissant minuit (start > end) — ex. allumage 20:00→23:00, ou 22:00→02:00.
- `manualOverride` actif → suit `enabled` (quel que soit l'horaire).
- Pas de manualOverride, `scheduleEnabled=false` → suit `enabled`.
- `scheduleEnabled=true`, temps indisponible → conserve l'état courant (pas de faux toggle).
- Parsing "HH:MM" invalide → -1 (via timeStringToMinutes réutilisé).

## Comportement actuel
Logique dans `lighting.cpp` (helpers L39-54, décision L85-108), couplée GPIO/RTC/millis/MQTT. 0 test natif.

## Contraintes techniques
- `schedule_logic` reste pur (`<stdint.h>`/`<math.h>`/`<string.h>`, pas d'Arduino/FreeRTOS/`<vector>`).
- `millis()`, RTC (`getLocalTime`), `digitalWrite`, `publishState`/MQTT, `manualSetAtMs` restent dans la coquille.
- Aucune constante modifiée (heures par défaut "20:00"/"23:00" restent dans `ensureTimesValid` coquille).
- **Le comportement de la filtration (feature-038) ne doit subir AUCUN changement** (régression interdite sur `isMinutesInRange` filtration).

## Fichiers potentiellement concernés
- `src/schedule_logic.{h,cpp}` — ajout `decideLightingOn` + variante range inclusive.
- `src/lighting.{cpp,h}` — `update()`/`begin()`/helpers deviennent coquilles déléguant.
- `test/test_native_schedule/test_schedule_logic.cpp` (étendu) — tests éclairage.
- `docs/subsystems/` (lighting ou filtration/schedule), `CHANGELOG.md`.
- (Pas de changement attendu sur `platformio.ini`/`coverage.sh` : `schedule_logic` déjà inclus/filtré.)

## Nouvelles signatures / API (indicatives — architect arbitre)
- `bool isMinutesInRange(int now, int start, int end, bool equalMeansAlways = false);` (paramètre par défaut → filtration inchangée) — OU `bool isMinutesInRangeInclusive(...)`.
- `bool decideLightingOn(bool manualOverride, bool enabledFlag, bool scheduleEnabled, bool haveTime, int nowMin, int startMin, int endMin, bool currentlyOn);`
- Aucun endpoint HTTP / champ WS / topic MQTT.

## ADR attendu ?
- **Probable** : non — réutilise ADR-0017. À confirmer par l'architect (la variante `start==end` est un micro-arbitrage local, pas une décision structurante).

## Éléments visibles (pour doc-writer)
### Endpoints / WS / MQTT
- aucun (inchangés)
### Documentation
- `docs/subsystems/` : section logique d'horaire éclairage pure (décision + range), testabilité native ; noter la divergence `start==end` filtration/éclairage.
- `CHANGELOG.md` — entrée (refactor interne + couverture décision d'horaire éclairage).

## Critères d'acceptation
- [x] AC1 — Équivalence stricte : `decideLightingOn` renvoie la même décision que `update()` L85-108 pour tout tuple d'entrée.
- [x] AC2 — `start == end` : décision éclairage → **true** (toute la journée) ; et la filtration reste **false** (non régressée).
- [x] AC3 — Fenêtre simple + fenêtre franchissant minuit correctes (réutilise la logique range).
- [x] AC4 — Priorités : manualOverride → `enabled` ; sans manuel + scheduleEnabled=false → `enabled` ; scheduleEnabled=true + temps indispo → conserve `currentlyOn`.
- [x] AC5 — `pio test -e native` tous PASSED ; couverture des nouvelles fonctions ≥ seuil.
- [x] AC6 — `pio run` (esp32dev) SUCCESS sans nouveau warning.
- [x] AC7 — Aucune régression de la filtration (tests feature-038 toujours verts) ; aucune constante modifiée (diff = déplacement pur).
- [x] Libellés / logs inchangés.

## Plan de test manuel
1. `pio test -e native` → tous verts (filtration + éclairage) ; `./tools/coverage.sh` → nouvelles fonctions couvertes.
2. `pio run` → SUCCESS sans warning.
3. On-target : programmation 20:00-23:00 → ON le soir ; manuel ON/OFF prioritaire ; start==end → allumé toute la journée.

## Notes pour agent
- **Invariant directeur** : characterization refactor — aucune modification de comportement, ni éclairage ni filtration. La divergence `start==end` (lighting true / filtration false) est INTENTIONNELLE et doit être préservée.
- `millis()`/RTC/`digitalWrite`/MQTT restent dans la coquille ; le pur reçoit `nowMin`/flags.
- Réutiliser `schedule_logic` + le dossier `test/test_native_schedule/` (même binaire).

## Hors périmètre
- Unifier le comportement `start==end` entre filtration et éclairage (interdit — divergence voulue).
- Modifier les heures par défaut, le timeout manuel, ou la logique MQTT.
- Tests on-target automatisés du relais GPIO.

---

## Suivi / itérations (clôture)

**Statut : livré — tests verts, revue Approved (pas de pool-chemistry : éclairage = zéro impact chimie/eau).** Version 2.2.10.

### Décisions
- Réutilise `schedule_logic` (feature-038). Ajout `decideLightingOn(...)` (copie exacte de update() L85-108) ; `isMinutesInRange` gagne `bool equalMeansAlways = false`.
- **Divergence start==end préservée** : filtration → `false` (défaut, appels 3 args inchangés) ; éclairage → `true` (passe `equalMeansAlways=true`). Garde `-1` conservée AVANT le test `start==end`.
- `timeStringToMinutes` réutilisé tel quel. `ensureTimesValid` reste coquille. ADR-0017 réutilisé.
- `lighting.cpp::update()` devient coquille (collecte flags/RTC/relayState, délègue) ; helpers dupliqués supprimés.

### Non-régression
- Filtration intacte par construction (défaut `false`). Tests feature-038 toujours verts.

### Tests
- `test/test_native_schedule/test_schedule_logic.cpp` : +11 tests (start==end true/false + garde -1 ; fenêtres simple/minuit ; table `decideLightingOn` : manualOverride, scheduleEnabled off, haveTime=false conserve l'état).
- `pio test -e native` : **93 PASSED**. `schedule_logic.cpp` **100 % lignes**. `pio run` esp32dev SUCCESS.

### Fichiers
- `src/schedule_logic.{h,cpp}`, `src/lighting.{cpp,h}`, `test/test_native_schedule/test_schedule_logic.cpp`, `docs/subsystems/lighting.md` (nouveau), `docs/subsystems/filtration.md`, `CHANGELOG.md`, `src/version.h` (2.2.10).
