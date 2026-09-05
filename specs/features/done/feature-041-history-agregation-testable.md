## Métadonnées
- **ID** : feature-041
- **Slug** : history-agregation-testable
- **Type** : feature (refactor de testabilité + tests)
- **Statut** : todo
- **Priorité** : basse (historique : justesse des moyennes ; aucun impact chimie/eau)
- **Version cible** : N/A — refactor interne strictement préservateur de comportement (bump CORRECTIF optionnel à la release)
- **Dernière mise à jour** : 2026-06-27
- **Spec(s) liée(s)** : feature-036/037/038/039/040 (modules purs testables, ADR-0017 Humble Object)

## Domaines touchés
- [x] Historique / persistance
- [x] Tests natifs (`test/`, env `native` / `native_coverage`)
- [ ] Régulation / Capteurs / Filtration / Web / MQTT

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **NON applicable** (historique = aucun impact dosage/chimie/eau)
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Extraire la **math scalaire d'agrégation** de l'historique (calcul de bucket temporel, prédicat d'ancienneté, moyenne, règles majorité / « au moins un ») de `history.cpp::consolidateData()` vers un module pur testable en natif, puis ajouter des tests Unity. Aucun changement de comportement.

## Contexte
`history.cpp` agrège les points bruts (5 min) en moyennes horaires puis quotidiennes (`consolidateData()` L440-665) et n'a **0 test natif**. Contrainte forte : l'env de test natif **n'a pas de libc++** (les modules purs n'utilisent que des headers C). Or `history.cpp` repose massivement sur `std::vector`/`std::map`/`std::sort`/`std::remove_if` → **le regroupement n'est PAS testable nativement** et reste dans la coquille.

En revanche, la **math scalaire** sous-jacente est pure et testable, et c'est là que se logent les bugs (off-by-one de bucket, division par zéro, mauvais seuil de majorité) :
- bucket temporel : `(timestamp / kSecondsPerHour) * kSecondsPerHour` (L479) et l'équivalent quotidien ;
- ancienneté : `age = now - timestamp; age > MAX_AGE` (L462, L476-477, L532-533) ;
- moyenne : `sum / validCount` (L516-519), avec `validCount==0` → point ignoré ;
- majorité : `filtrationActive = (filtrationCount > total/2)` (L520) ;
- « au moins un » : `phDosing = (phDosingCount > 0)` (L521-522).

## Comportement attendu
Comportement **observable strictement inchangé** : mêmes moyennes, mêmes buckets, mêmes flags agrégés, mêmes purges.

### Refactor (characterization, préservation stricte)
Créer un module pur `src/history_logic.{h,cpp}` (sans Arduino/FreeRTOS/`<vector>`/`<map>` ; `<stdint.h>`/`<math.h>` seulement) :
- `uint32_t bucketTimestamp(uint32_t ts, uint32_t bucketSeconds);` → `(ts / bucketSeconds) * bucketSeconds` (garde `bucketSeconds>0`).
- `bool isOlderThan(uint32_t now, uint32_t ts, uint32_t maxAgeSeconds);` → `(now - ts) > maxAgeSeconds` (uint32).
- `float finalizeMean(float sum, int count);` → `count > 0 ? sum / count : NAN`.
- `bool isMajority(int trueCount, int total);` → `trueCount > total / 2` (division entière, reproduit L520).
- `bool anyTrue(int count);` → `count > 0`.

`consolidateData()` (et les passes raw→horaire et horaire→quotidien) utilisent ces helpers ; le regroupement `std::map`/`std::vector`, les `std::remove_if`/`std::sort`, la persistance et l'horloge restent dans la coquille.

### Cas limites
- `bucketTimestamp` : ts pile sur une frontière (reste identique) ; ts = bucket+1 (tronque au bucket) ; bucket horaire (3600) vs quotidien (86400).
- `isOlderThan` : age == maxAge → false (frontière stricte `>`) ; age == maxAge+1 → true ; wrap uint32.
- `finalizeMean` : count==0 → NaN ; count>0 → sum/count exact.
- `isMajority` : 3/5 → true ; 2/4 → false (2 > 2 faux) ; 3/4 → true ; total impair/pair.
- `anyTrue` : 0 → false ; ≥1 → true.

## Comportement actuel
Math inline dans `consolidateData()` (L440-665), couplée `std::vector`/`std::map`/FS/horloge. 0 test natif.

## Contraintes techniques
- `history_logic` pur : `<stdint.h>`/`<math.h>` uniquement (PAS de `<vector>`/`<map>`/Arduino/FreeRTOS — incompatibles avec l'env natif sans libc++).
- `std::vector`/`std::map`/`std::sort`/`std::remove_if`, persistance FS, horloge restent dans la coquille `consolidateData`.
- Aucune constante modifiée (`kSecondsPerHour`, `RAW_MAX_AGE`, `DAILY_MAX_AGE`, seuils d'âge).
- Préserver EXACTEMENT les divisions entières et le seuil de majorité (`> total/2`).

## Fichiers potentiellement concernés
- `src/history_logic.{h,cpp}` — [nouveau] helpers scalaires purs.
- `src/history.cpp` — `consolidateData` (et passes d'agrégation) utilisent les helpers.
- `test/test_native_history/test_history_logic.cpp` — [nouveau] tests Unity (1 dossier = 1 binaire).
- `platformio.ini` — `build_src_filter` de `[env:native]` (+ `history_logic.cpp`).
- `tools/coverage.sh` — ajouter le filtre `history_logic`.
- `docs/subsystems/` (history), `CHANGELOG.md`.

## Nouvelles signatures / API (indicatives — architect arbitre)
- `uint32_t bucketTimestamp(uint32_t ts, uint32_t bucketSeconds);`
- `bool isOlderThan(uint32_t now, uint32_t ts, uint32_t maxAgeSeconds);`
- `float finalizeMean(float sum, int count);`
- `bool isMajority(int trueCount, int total);`
- `bool anyTrue(int count);`
- Aucun endpoint HTTP / champ WS / topic MQTT.

## ADR attendu ?
- **Probable** : non — réutilise ADR-0017 (Humble Object).

## Éléments visibles (pour doc-writer)
### Endpoints / WS / MQTT
- aucun (inchangés)
### Documentation
- `docs/subsystems/` (history) : section math d'agrégation pure (bucket, ancienneté, moyenne, majorité), testabilité native ; noter que le regroupement std::vector/std::map reste hors tests natifs (libc++ absent).
- `CHANGELOG.md` — entrée (refactor interne + couverture de la math d'agrégation historique).

## Critères d'acceptation
- [ ] AC1 — `bucketTimestamp` : troncature au bucket (frontière exacte inchangée, bucket+1 → bucket) pour 3600 et 86400.
- [ ] AC2 — `isOlderThan` : age==maxAge → false ; age==maxAge+1 → true ; wrap uint32.
- [ ] AC3 — `finalizeMean` : count==0 → NaN ; count>0 → sum/count exact.
- [ ] AC4 — `isMajority` : reproduit `> total/2` (2/4→false, 3/4→true, 3/5→true) ; `anyTrue` : 0→false, ≥1→true.
- [ ] AC5 — Équivalence : `consolidateData` produit les mêmes moyennes/buckets/flags qu'avant (vérifié par lecture + non-régression des autres tests).
- [ ] AC6 — `pio test -e native` tous PASSED ; couverture des nouvelles fonctions ≥ seuil.
- [ ] AC7 — `pio run` (esp32dev) SUCCESS sans nouveau warning.
- [ ] AC8 — Aucune constante modifiée (diff = déplacement pur).
- [ ] Libellés / logs inchangés.

## Plan de test manuel
1. `pio test -e native` → tous verts ; `./tools/coverage.sh` → history_logic couvert.
2. `pio run` → SUCCESS sans warning.
3. On-target (optionnel) : laisser tourner > 6h → vérifier que les moyennes horaires apparaissent dans l'historique (cohérence visuelle).

## Notes pour agent
- **Invariant directeur** : characterization refactor — aucune modification des formules/seuils. On extrait la math scalaire ; le regroupement reste coquille.
- **Contrainte libc++** : `history_logic` NE DOIT PAS inclure `<vector>`/`<map>`/`<cstdint>` (env natif local sans libc++). Headers C uniquement.
- Reproduire EXACTEMENT la division entière `total/2` et la frontière `>` (majorité, ancienneté).
- Nouveau dossier de tests `test/test_native_history/` (1 binaire) ; étendre `platformio.ini` build_src_filter + `coverage.sh`.

## Hors périmètre
- Tester le regroupement `std::map`/`std::vector` ou les `std::sort`/`std::remove_if` (non testable nativement sans libc++).
- Refonte de la persistance FS ou du format de fichier historique.
- Modifier les âges de rétention ou la granularité.

---

## Suivi / itérations (clôture)

**Statut : livré — tests verts, revue Approved (pas de pool-chemistry : historique = zéro impact chimie/eau).** Version 2.2.11.

### Décisions
- Module pur `src/history_logic.{h,cpp}` (`<stdint.h>`/`<math.h>` seulement) : `bucketTimestamp`, `isOlderThan`, `finalizeMean`, `isMajority`, `anyTrue`.
- Signatures en `uint32_t` (reproduit l'arithmétique 32 bits ESP32 ET compile en natif x86_64 où `unsigned long`=64 bits casserait le wrap) ; la coquille caste `(uint32_t)`.
- `consolidateData` (2 passes raw→horaire et horaire→quotidien) délègue ; l'accumulation (`sum`, `validCount` sous `!isnan`), la garde `if(validCount>0){push}`, le regroupement `std::map`/`std::vector`, `std::remove_if`/`std::sort` et la persistance restent coquille (non testables nativement, libc++ absent).
- `bucketTimestamp` unifie horaire (`kSecondsPerHour`) et quotidien (`86400`) via argument ; garde `bucketSeconds==0 → ts` (enrichissement défensif, appelants toujours non nuls → équivalence runtime stricte).
- ADR-0017 réutilisé.

### Contrainte libc++
Seule la **math scalaire** est testée nativement ; le regroupement `std::vector`/`std::map` reste hors tests (env natif sans libc++).

### Tests
- `test/test_native_history/test_history_logic.cpp` : 25 tests (buckets/frontières, ancienneté + wrap uint32, moyenne count==0→NaN, majorité `>total/2`, anyTrue).
- `pio test -e native` : **118 PASSED** (4 binaires). `history_logic.cpp` **100 % lignes**. `pio run` esp32dev SUCCESS.
- `tools/coverage.sh` + `platformio.ini` (build_src_filter) étendus.

### Fichiers
- `src/history_logic.{h,cpp}` (nouveau), `src/history.cpp`, `platformio.ini`, `test/test_native_history/test_history_logic.cpp` (nouveau), `tools/coverage.sh`, `docs/subsystems/history.md` (nouveau), `CHANGELOG.md`, `src/version.h` (2.2.11).
