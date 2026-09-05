## Métadonnées
- **ID** : feature-039
- **Slug** : anti-rafale-rollover-testable
- **Type** : feature (refactor de testabilité + tests)
- **Statut** : todo
- **Priorité** : haute (sécurité dosage : anti-emballement + quotas journaliers)
- **Version cible** : N/A — refactor interne strictement préservateur de comportement (bump CORRECTIF optionnel à la release)
- **Dernière mise à jour** : 2026-06-27
- **Spec(s) liée(s)** : feature-036/037 (dosing_logic, pattern Humble Object, ADR-0017), feature-025 (anti-rafale Pass 3.5), feature-038 (schedule_logic)

## Domaines touchés
- [x] Régulation (pump_controller) → **passage obligatoire par `pool-chemistry`** (anti-rafale = garde de sécurité dosage)
- [x] Tests natifs (`test/`, env `native` / `native_coverage`)
- [ ] Capteurs / Filtration / Web / MQTT / UART / OTA

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (anti-rafale anti-emballement + reset des quotas journaliers)
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Extraire la logique pure de l'**anti-rafale** (comptage de cycles de dosage sur fenêtres glissantes 1 min / 15 min, gestion du wrap `millis()`) et des **déclencheurs de rollover journalier** (date NTP changée / fallback 24 h) de `pump_controller.cpp` vers la logique pure testable, puis ajouter des tests Unity. Aucun changement de comportement.

## Contexte
feature-036/037 ont couvert la décision et le calcul de dosage. Restent **non testés** dans `pump_controller.cpp`, et 🔴 critiques pour la sécurité :
- **`countRecentDosingCycles(pumpIndex, windowMs)`** (L291) — compte les timestamps de démarrage de cycle dans une fenêtre glissante, ring buffer `_dosingCycleHistory` (taille `kDosingCycleHistorySize=20`), **gère le wrap `millis()`** (arithmétique non signée). Alimente l'anti-rafale de `canDose()` (refus si ≥ `kMaxDosingCyclesPerMinute=6` sur 1 min OU ≥ `kMaxDosingCyclesPer15Min=20` sur 15 min). C'est le garde-fou anti-emballement de la pompe.
- **`recordDosingCycleStart(pumpIndex)`** (L~280) — écrit `millis()` dans le ring buffer + avance l'index circulaire.
- **`tickDailyRollover()`** (L502) — réinitialise les quotas journaliers (`dailyPhInjectedMl`/`dailyOrpInjectedMl`) à minuit (date locale NTP) ou via fallback 24 h (`millis()`), arme le timer de stabilisation (mitigation double-quota au passage de minuit).

Un bug ici = soit emballement de la pompe (anti-rafale cassé), soit double dosage / quota jamais réinitialisé (rollover cassé). Aucun test natif aujourd'hui.

## Comportement attendu
Comportement **observable strictement inchangé** : mêmes comptages, mêmes décisions de reset, mêmes seuils.

### Refactor (characterization, préservation stricte)
Étendre `src/dosing_logic.{h,cpp}` (anti-rafale = sécurité dosage, même domaine) avec des fonctions pures :
- **`int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs);`** — copie exacte de `countRecentDosingCycles` L291-301 : ignore les slots à 0, compte si `(now - ts) <= windowMs` (arithmétique non signée → wrap-safe). `now` (millis) injecté en paramètre.
- **`void recordCycleTimestamp(uint32_t* history, size_t& idx, size_t size, uint32_t now);`** — écrit `now` à `idx`, avance `idx = (idx+1) % size`. (Ou un retour du nouvel index si l'architect préfère éviter la ref.)
- **Rollover** : déclencheurs purs séparés de la mutation/NVS/log/stabilisation :
  - `bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr);` → `strlen(currentDayDate)>0 && strcmp(currentDayDate, todayStr)!=0`.
  - `bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now);` → `now - dayStartMs >= 86400000UL`.
  - (Le `strftime`/`localtime_r`, l'écriture `safetyLimits`, `saveDailyCounters`, `armStabilizationTimer`, les logs restent dans la coquille `tickDailyRollover`.)

Les méthodes `countRecentDosingCycles`/`recordDosingCycleStart`/`tickDailyRollover` deviennent des coquilles : elles fournissent `millis()`/`time()`/les buffers membres et délèguent aux fonctions pures.

### Cas limites
- `countCyclesInWindow` : buffer vide (tous 0) → 0 ; tous récents → size ; **wrap millis** (`now` juste après 0, `ts` proche `0xFFFFFFFF` → `(now-ts)` petit → compté) ; ts hors fenêtre → ignoré ; ts == now → compté (delta 0).
- `recordCycleTimestamp` : avance circulaire (idx repasse à 0 après size-1) ; écrase le plus ancien.
- `shouldRolloverByDate` : currentDayDate vide → false (première init gérée par coquille) ; même date → false ; date différente → true.
- `shouldRolloverByMillis` : `now - dayStart` exactement 86400000 → true ; juste en dessous → false ; wrap millis géré (non signé).

## Comportement actuel
Logique dans `pump_controller.cpp` (L280-301 anti-rafale, L502+ rollover), couplée `millis()`/`time()`/membres/NVS. 0 test natif.

## Contraintes techniques
- Logique pure dans `dosing_logic` : `<stdint.h>`/`<math.h>`/`<string.h>` (pour `strcmp`/`strlen`), pas d'Arduino/FreeRTOS/`<vector>`.
- `millis()`/`time()`/`localtime_r`/`strftime` restent dans la coquille ; le pur reçoit `now`/`todayStr`.
- Aucun seuil/constante modifié (`kMaxDosingCyclesPerMinute=6`, `kMaxDosingCyclesPer15Min=20`, `kDosingCycleHistorySize=20`, fenêtres 60000/900000 ms, 86400000 ms).
- Comportement de `canDose()` (anti-rafale #14/#15) strictement préservé.

## Fichiers potentiellement concernés
- `src/dosing_logic.{h,cpp}` — ajout des fonctions pures.
- `src/pump_controller.{cpp,h}` — `countRecentDosingCycles`/`recordDosingCycleStart`/`tickDailyRollover` deviennent coquilles.
- `test/test_native_dosing/test_dosing_decision.cpp` (étendu) — tests anti-rafale + rollover.
- `docs/subsystems/pump-controller.md`, `CHANGELOG.md`.
- (Pas de changement attendu sur `platformio.ini`/`coverage.sh` : `dosing_logic` déjà inclus/filtré.)

## Nouvelles signatures / API (indicatives — architect arbitre)
- `int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs);`
- `void recordCycleTimestamp(uint32_t* history, size_t& idx, size_t size, uint32_t now);`
- `bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr);`
- `bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now);`
- Aucun endpoint HTTP / champ WS / topic MQTT.

## Sécurité chimique (obligatoire)
- Dosage impliqué : **oui** (anti-rafale = garde anti-emballement de `canDose()` ; rollover = reset des quotas journaliers).
- Limites concernées : fenêtres anti-rafale 1 min/15 min, quotas journaliers (reset).
- Filtration / PID / calibration : non touchés.
- Risque identifié : (1) anti-rafale cassé → emballement pompe ; (2) rollover cassé → double dosage à minuit OU quota jamais réinitialisé. Mitigation : équivalence stricte validée `pool-chemistry` + tests des cas limites (wrap millis, frontière 86400000, comptage fenêtre). Aucun seuil modifié.

## ADR attendu ?
- **Probable** : non — réutilise ADR-0017 (Humble Object). À confirmer par l'architect.

## Éléments visibles (pour doc-writer)
### Endpoints / WS / MQTT
- aucun (inchangés)
### Documentation
- `docs/subsystems/pump-controller.md` — section anti-rafale + rollover : logique pure testable, gestion du wrap millis.
- `CHANGELOG.md` — entrée (refactor interne + couverture anti-rafale/rollover).
- ADR : seulement si l'architect le justifie (sinon « réutilise ADR-0017 »).

## Critères d'acceptation
- [x] AC1 — Équivalence stricte : `countCyclesInWindow` renvoie le même comptage que `countRecentDosingCycles` (L291-301) pour des buffers/fenêtres identiques.
- [x] AC2 — `countCyclesInWindow` : buffer vide → 0 ; slots à 0 ignorés ; ts dans/hors fenêtre ; ts==now compté ; **wrap millis** (now post-wrap, ts pré-wrap → compté correctement).
- [x] AC3 — `recordCycleTimestamp` : écriture + avance circulaire (idx wrap à size), écrase le plus ancien ; après `size` écritures la fenêtre reflète les `size` derniers.
- [x] AC4 — `shouldRolloverByDate` : vide → false ; même date → false ; date différente → true.
- [x] AC5 — `shouldRolloverByMillis` : == 86400000 → true ; juste en dessous → false ; wrap millis géré.
- [x] AC6 — Scénario anti-rafale combiné : N enregistrements puis comptage 1 min / 15 min cohérent avec les seuils (6 / 20).
- [x] AC7 — `pio test -e native` tous PASSED ; couverture des nouvelles fonctions ≥ seuil.
- [x] AC8 — `pio run` (esp32dev) SUCCESS sans nouveau warning.
- [x] AC9 — Aucun seuil/constante modifié ; comportement `canDose` anti-rafale inchangé (diff = déplacement pur).
- [x] Libellés / logs inchangés.

## Plan de test manuel
1. `pio test -e native` → tous verts ; `./tools/coverage.sh` → nouvelles fonctions couvertes.
2. `pio run` → SUCCESS sans warning.
3. On-target (si reproductible) : forcer plusieurs cycles rapprochés → vérifier le refus anti-rafale (`[Dosage] Refus : anti-rafale …`) au-delà de 6/min ou 20/15 min ; passage de minuit → log « Reset journalier ».

## Notes pour agent
- **Invariant directeur** : characterization refactor — aucune modification des seuils/fenêtres/formules. On déplace pour tester.
- Le **wrap `millis()`** (arithmétique non signée) est le piège principal : reproduire exactement `(now - ts) <= windowMs` en `uint32_t` ; tester explicitement le cas post-wrap.
- `millis()`/`time()`/`localtime_r`/`strftime`/`saveDailyCounters`/`armStabilizationTimer`/logs restent dans la coquille.
- Réutiliser `dosing_logic` + le dossier de tests `test/test_native_dosing/` (même binaire).

## Hors périmètre
- Modification des seuils anti-rafale, des fenêtres, du délai de rollover.
- Refonte de la persistance NVS des compteurs journaliers.
- Extraction d'autres parties de `pump_controller` (déjà couvertes ou hors sujet).

---

## Suivi / itérations (clôture)

**Statut : livré — tests verts, revue Approved, pool-chemistry GO (équivalence stricte, 2 passages).** Version 2.2.9.

### Décisions
- 4 fonctions pures ajoutées à `dosing_logic.{h,cpp}` : `countCyclesInWindow` (comptage fenêtre glissante, wrap millis uint32, frontière `<=`, slot 0 ignoré), `recordCycleTimestamp` (écrit + **renvoie** le nouvel index `(idx+1)%size` — cohérent avec le style du module, pas de mutation par référence), `shouldRolloverByDate` (`strlen>0 && strcmp!=0`), `shouldRolloverByMillis` (`(now-dayStart)>=86400000`, frontière `>=`). `<string.h>` ajouté.
- Coquilles `recordDosingCycleStart`/`countRecentDosingCycles`/`tickDailyRollover` : fournissent `millis()`/`time()`/buffers, gardes pumpIndex conservées. `time()/localtime_r/strftime`/NVS/`armStabilizationTimer`/logs/branche première-init restent coquille.
- ADR : réutilise **ADR-0017**.

### Conditions pool-chemistry respectées
1. `_dosingCycleHistoryIdx[pumpIndex] = recordCycleTimestamp(...)` (retour réassigné → index non figé, pas d'emballement).
2. Branche première-init (`strlen==0`, reset `dayStartTimestamp`) + fallback init restent coquille.
3. Cast explicite `(uint32_t)` sur l'appel `shouldRolloverByMillis`.
Aucun seuil modifié (6/20/20/60000/900000/86400000) ; garde `canDose` #14/#15 intacte.

### Tests
- `test/test_native_dosing/test_dosing_decision.cpp` : +15 tests (comptage vide/fenêtre/frontière/wrap millis ; avance circulaire/écrasement ; rollover date ; rollover millis frontière+wrap ; scénario anti-rafale 6/min, 20/15min).
- `pio test -e native` : **85 PASSED** (4 suites). `dosing_logic.cpp` **100 % lignes/fonctions**. `pio run` esp32dev SUCCESS.

### Fichiers
- `src/dosing_logic.{h,cpp}`, `src/pump_controller.cpp`, `test/test_native_dosing/test_dosing_decision.cpp`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`, `src/version.h` (2.2.9).
