## Métadonnées
- **ID** : feature-038
- **Slug** : filtration-horaire-testable
- **Type** : feature (refactor de testabilité + tests)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : N/A — refactor interne strictement préservateur de comportement (bump CORRECTIF optionnel à la release)
- **Dernière mise à jour** : 2026-06-27
- **Spec(s) liée(s)** : feature-036 / feature-037 (pattern Humble Object, tests natifs, ADR-0017), ADR-0016 (régulation — la présence d'eau via filtration conditionne le dosage)

## Domaines touchés
- [x] Filtration / Éclairage
- [x] Tests natifs (`test/`, env `native` / `native_coverage`)
- [ ] Régulation (pump_controller) — non modifié, MAIS `filtration.isRunning()` est la garde « présence d'eau » de `canDose()` → invariant à préserver
- [ ] Capteurs / Web / MQTT / UART / OTA

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **revue légère** : confirmer que la garde « présence d'eau avant injection » (`canDose` #2) n'est pas affaiblie (la filtration commande l'eau qui circule devant les sondes et permet le dosage)
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Extraire la **logique d'horaire de la filtration** (décision marche/arrêt selon mode + plage horaire + forçages, parsing "HH:MM", appartenance à une fenêtre franchissant minuit, calcul de la plage auto selon la température) de `filtration.cpp` vers un module pur testable en natif, puis ajouter des tests Unity. Aucun changement de comportement.

## Contexte
Suite des features 036/037 (dosage testable). La filtration est la **garde de sécurité « présence d'eau »** : `canDose()` (#2) refuse toute injection si la filtration ne tourne pas (hors mode continu). Or toute la décision d'horaire vit dans `filtration.cpp::update()`, couplée à `digitalWrite`, `millis()`, RTC et config — **aucun test**. Un bug d'horaire (mauvaise plage, fenêtre à cheval sur minuit mal gérée, plage invalide) pourrait faire tourner/arrêter la filtration à tort, donc autoriser un dosage sans circulation d'eau ou priver le bassin de filtration.

La logique est déjà bien isolée en helpers quasi-purs dans `filtration.cpp` :
- `timeStringToMinutes(const String&)` (L121) — parse "HH:MM" → minutes, -1 si invalide ;
- `isMinutesInRange(now, start, end)` (L129) — appartenance à une fenêtre, gère start>end (minuit), start==-1/end==-1/start==end → false ;
- `computeAutoSchedule()` (L72) — durée = temp/2 bornée [1h, 24h], centrée sur `kFiltrationPivotHour`, wrap 0-24, format "HH:MM" ;
- la **décision** dans `update()` (L186-201) : priorité forceOn > forceOff > (mode manual/auto : in-range) ; temps indisponible → conserver l'état courant.

## Comportement attendu
Comportement **observable strictement inchangé** : mêmes décisions marche/arrêt pour les mêmes (mode, forçages, heure, plage, état courant) ; mêmes plages auto pour la même température.

### Refactor (characterization, préservation stricte)
- Créer un module pur (ex. `src/filtration_logic.{h,cpp}` — ou `src/schedule_logic.*` réutilisable pour l'éclairage, arbitrage architect ; sans Arduino/FreeRTOS/<vector>, `<stdint.h>`/`<math.h>`, String autorisé via shim si besoin pour le parsing).
- Y déplacer (déplacement pur, fonctions libres/statiques) :
  - `timeStringToMinutes` (parsing + validation) ;
  - `isMinutesInRange` (appartenance fenêtre, gestion minuit, plages invalides) ;
  - le calcul de fenêtre auto : `computeAutoWindow(tempC, pivotHour) → { startMin, endMin }` (durée temp/2 bornée [1,24], centrage, wrap) — SÉPARÉ de la lecture `sensors.getTemperature()`, du deadband 1°C, de l'écriture config et du `saveMqttConfig()`/NVS (qui restent dans la coquille) ;
  - la décision run : `decideFiltrationRun(mode, forceOn, forceOff, haveTime, nowMin, startMin, endMin, currentlyRunning) → bool runTarget`.
- `filtration.cpp::update()` devient une coquille qui collecte (RTC/millis/config/sensors), appelle les fonctions pures, applique `digitalWrite`/timers/`publishState`/événements UART/stabilisation. Les forçages + timeout 4h, l'armement du timer de stabilisation, le recalcul auto à +5 min, la persistance restent dans la coquille.

### Cas limites
- Fenêtre simple (start < end) : in/out aux bornes (now==start → in ; now==end → out).
- Fenêtre à cheval sur minuit (start > end) : ex. 22:00-06:00.
- Plage invalide : start==end → jamais ; start==-1 ou end==-1 → jamais.
- Parsing "HH:MM" : valides (00:00, 23:59), invalides (longueur, séparateur, hh>23, mm>59) → -1.
- forceOn prioritaire sur tout ; forceOff prioritaire sur la plage ; forceOn > forceOff (ordre actuel L190-193).
- Temps indisponible (NTP/RTC KO) → conserver l'état courant (pas de faux start/stop).
- Auto : temp froide → durée plancher 1h ; temp très chaude → plafond 24h ; centrage sur pivot ; wrap minuit.

## Comportement actuel
Toute la logique est dans `filtration.cpp` (helpers L72-135 + décision L186-201), couplée GPIO/RTC/millis/config. 0 test natif.

## Contraintes techniques
- Module pur sans `<Arduino.h>` réel / FreeRTOS / `<vector>` ; types POD ; `String` seulement si nécessaire (shim natif existant) — préférer `const char*`/int si possible pour le parsing.
- Aucun seuil/constante modifié (`kFiltrationPivotHour`, timeout forçage 4h, deadband 1°C, durée temp/2, bornes 1h/24h).
- `millis()` et le temps courant (RTC) restent dans la coquille ; le pur reçoit `nowMin`, `tempC`, etc.
- Invariant sécurité : la filtration reste la garde « présence d'eau » de `canDose()` — comportement marche/arrêt strictement identique.

## Fichiers potentiellement concernés
- `src/filtration_logic.{h,cpp}` (ou `schedule_logic.*`) — [nouveau] fonctions pures.
- `src/filtration.{cpp,h}` — `update()` + helpers deviennent coquilles déléguant.
- `test/test_native_filtration/` (ou `test_native_schedule/`) — [nouveau] tests Unity (1 dossier = 1 binaire).
- `platformio.ini` — `build_src_filter` de `[env:native]` (+ hérité par `native_coverage`).
- `docs/subsystems/filtration.md` (ou page concernée), `CHANGELOG.md`.

## Nouvelles signatures / API (indicatives — architect arbitre)
- `int timeStringToMinutes(const char* hhmm);` (ou `const String&`)
- `bool isMinutesInRange(int nowMin, int startMin, int endMin);`
- `struct FiltrationWindow { int startMin; int endMin; };`
- `FiltrationWindow computeAutoWindow(float tempC, float pivotHour);`
- `bool decideFiltrationRun(const char* mode, bool forceOn, bool forceOff, bool haveTime, int nowMin, int startMin, int endMin, bool currentlyRunning);`
- Aucun endpoint HTTP / champ WS / topic MQTT.

## Sécurité chimique (revue légère pool-chemistry)
- Dosage impliqué : non directement. MAIS la filtration est la **garde présence d'eau** de `canDose()` (#2) : un défaut d'horaire pourrait autoriser un dosage sans circulation. → confirmer que la décision marche/arrêt est strictement préservée (équivalence), et que la garde côté `canDose` n'est pas touchée.
- Limites de dosage / PID : non touchés.
- Risque identifié : régression silencieuse de la décision d'horaire (fenêtre minuit, forçages, plage invalide). Mitigation : équivalence stricte + tests exhaustifs des cas limites.

## ADR attendu ?
- **Probable** : non — réutilise ADR-0017 (Humble Object). Si l'architect crée un module générique `schedule_logic` réutilisable (filtration + éclairage), le mentionner ; cela ne justifie pas un ADR nouveau a priori.

## Éléments visibles (pour doc-writer)
### Endpoints / WS / MQTT
- aucun (inchangés)
### Documentation
- `docs/subsystems/filtration.md` — section logique d'horaire pure (décision + fenêtre auto), testabilité native.
- `CHANGELOG.md` — entrée (refactor interne + couverture de la décision d'horaire filtration).
- ADR : seulement si l'architect le justifie (sinon « réutilise ADR-0017 »).

## Critères d'acceptation
- [ ] AC1 — Équivalence stricte : pour tout (mode, forceOn, forceOff, haveTime, nowMin, start, end, currentlyRunning), `decideFiltrationRun` renvoie le même `runTarget` que la logique actuelle de `update()` (L186-201).
- [ ] AC2 — `isMinutesInRange` : fenêtre simple (now==start → true, now==end → false), fenêtre minuit (start>end), plages invalides (start==end, -1) → false.
- [ ] AC3 — `timeStringToMinutes` : "00:00"→0, "23:59"→1439 ; invalides (mauvaise longueur, séparateur ≠ ':', hh>23, mm>59) → -1.
- [ ] AC4 — `computeAutoWindow` : durée = clamp(temp/2, 1h, 24h), fenêtre centrée sur le pivot, wrap minuit ; temp 0/négative → 1h ; temp ≥ 48 → 24h.
- [ ] AC5 — Priorités : forceOn → run (quel que soit le reste) ; forceOff (sans forceOn) → stop ; temps indisponible (mode auto/manual) → conserve `currentlyRunning`.
- [ ] AC6 — `pio test -e native` tous PASSED ; couverture des nouvelles fonctions ≥ seuil (`./tools/coverage.sh`).
- [ ] AC7 — `pio run` (esp32dev) SUCCESS sans nouveau warning.
- [ ] AC8 — Aucun seuil/constante modifié ; garde `canDose` #2 (filtration) inchangée (diff = déplacement pur).
- [ ] Libellés / logs inchangés.

## Plan de test manuel
1. `pio test -e native` → tous verts ; `./tools/coverage.sh` → nouvelles fonctions couvertes.
2. `pio run` → SUCCESS sans warning.
3. On-target : plage 22:00-06:00 (à cheval sur minuit) → filtration ON la nuit, OFF le jour ; forceOn pendant la plage OFF → ON ; mode auto à 2 températures → 2 plages cohérentes (chaude = plus longue).

## Notes pour agent
- **Invariant directeur** : characterization refactor — aucune modification de la formule auto, des seuils, des priorités. On déplace pour tester.
- Le temps (RTC/millis), la persistance NVS, le deadband 1°C de recalcul, le timeout forçage 4h, l'armement stabilisation, `publishState`/UART restent dans la coquille.
- Réutiliser env `native`/`native_coverage` + pattern de tests de feature-036/037.
- Envisager un module `schedule_logic` réutilisable pour l'éclairage (Tier 3 suivant) — à arbitrer par l'architect (peut rester `filtration_logic` si plus simple).

## Hors périmètre
- Modification de la règle auto (temp/2), du pivot, des timeouts.
- Refonte de `update()` au-delà de la séparation décision/action.
- Extraction de l'éclairage (`lighting.cpp`) — feature distincte ultérieure (mais module réutilisable bienvenu).
- Tests on-target automatisés du relais GPIO.

---

## Suivi / itérations (clôture)

**Statut : livré — tests verts, revue Approved, pool-chemistry GO (revue légère, conditions respectées).** Version 2.2.8.

### Décisions
- Module pur **`schedule_logic.{h,cpp}`** (générique, réutilisable pour l'éclairage) : `timeStringToMinutes(const char*)`, `isMinutesInRange`, `computeAutoWindow(tempC, pivot)→{startMin,endMin}`, `decideFiltrationRun(...)`. Includes `<stdint.h>/<math.h>/<string.h>`, zéro Arduino/FreeRTOS/String.
- `filtration.cpp::update()`/`begin()`/`computeAutoSchedule()` deviennent des coquilles : RTC/millis/NVS/deadband 1°C/timeout 4h/stabilisation/relais/publishState/UART restent coquille ; `mode.toLowerCase()` avant `decideFiltrationRun` ; parsing via `.c_str()`.
- `computeAutoWindow` reproduit EXACTEMENT l'arrondi d'origine (float→`round((h-int(h))*60)`+carry→minutes) pour ne pas décaler la fenêtre. Pivot réel = **13.0**.
- ADR : réutilise **ADR-0017** (Humble Object), aucun nouvel ADR.

### Sécurité (pool-chemistry, revue légère)
- Décision marche/arrêt strictement préservée (priorités forceOn>forceOff>plage ; `haveTime=false` sans forçage → conserve `currentlyRunning`).
- Garde « présence d'eau » de `canDose()` #2 (`pump_controller.cpp:326/643` via `filtration.isRunning()`) **inchangée**.
- Aucun seuil/constante modifié (pivot 13, timeout 4h, deadband 1°C, temp/2, bornes 1h/24h).

### Tests
- `test/test_native_schedule/test_schedule_logic.cpp` : 19 tests (AC2 fenêtre/minuit/invalides, AC3 parsing valides/invalides, AC4 fenêtre auto froide/chaude/plafond+wrap, AC5 priorités + temps indisponible).
- `tools/coverage.sh` étendu (filtre `schedule_logic`). `pio test -e native` : **70 PASSED**. `schedule_logic.cpp` **100% lignes**. `pio run` esp32dev SUCCESS.

### Fichiers
- `src/schedule_logic.{h,cpp}` [nouveau], `src/filtration.{cpp,h}`, `platformio.ini`, `test/test_native_schedule/test_schedule_logic.cpp` [nouveau], `tools/coverage.sh`, `docs/subsystems/filtration.md`, `CHANGELOG.md`, `src/version.h` (2.2.8).

### Reste (non automatisable CI)
- Validation on-target : plage à cheval sur minuit (22:00-06:00) ON la nuit / OFF le jour ; forceOn pendant plage OFF → ON ; auto à 2 températures → 2 plages cohérentes.
