## Métadonnées
- **ID** : feature-006
- **Slug** : injection-manuelle-gardee
- **Type** : feature
- **Statut** : done
- **Priorité** : haute (sécurité chimique — dernier écart entre injection manuelle et gardes de l'auto)
- **Version cible** : 2.6.0
- **Dernière mise à jour** : 2026-07-04 — **RÉVISION 2** (remplace la rédaction du 2026-04-24, largement périmée : une partie était implémentée depuis, et l'architecture a changé — cf. « Historique » en bas)
- **Spec(s) liée(s)** : feature-036/039 (`dosing_logic` : gardes pures testables, anti-rafale, ADR-0017), feature-025 (stabilisation post-cal), feature-002 (affichage cumul journalier — done)

## Domaines touchés
- [x] Régulation (pump_controller / dosing_logic) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP (`web_routes_control.cpp`)
- [x] Frontend (`data/`)
- [x] Tests natifs (`test/test_native_dosing/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire, double passage** (design puis code réel, comme feature-036/039)
- [ ] ux-ui-designer — non (toasts + bouton désactivé, patterns existants)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer — **oui** : la logique de garde manuelle doit être PURE et testée en natif
- [x] code-reviewer
- [x] doc-writer

## Description
Appliquer aux injections manuelles (`POST /ph/inject/start`, `POST /orp/inject/start`) les gardes de sécurité **encore manquantes** : limite journalière (avec reliquat), stabilisation post-calibration, limite horaire, cycles/jour et anti-rafale, refus de double démarrage — via une **fonction pure testable** dans `dosing_logic`, sur le modèle des features 036/039. L'opérateur garde la main (stop toujours libre), mais ne peut plus dépasser le budget chimique par inadvertance.

## État actuel (audité sur le code v2.5.2 — NE PAS réimplémenter)
Déjà en place depuis la rédaction initiale :
- ✅ **Garde filtration au démarrage** : `injectionAllowedOrReject()` (web_routes_control.cpp l.20-27) → **409** + log critique, appliquée aux 2 injections ET aux pompes test ; exemption mode « continu ».
- ✅ **Interruption en cours d'injection** si la filtration s'arrête (`updateManualInject()` l.56-95) + alerte MQTT `ph/orp_injection_aborted`.
- ✅ Bornage : volume ≤ 2000 mL/requête, durée plafonnée `kManualInjectMaxDurationS` (600 s).
- ✅ `inject/stop` sans garde (coupe-circuit opérateur).
- ✅ Volume manuel compté dans `ph_daily_ml`/`orp_daily_ml` (suivi `updateSafetyTracking` via duty).
- ✅ UI : 409 intercepté (app.js ~l.4950) avec message.

## Manques à combler (périmètre de cette feature)
1. **Limite journalière** : `cumul + volume demandé > max_*_ml_per_day` → non refusé aujourd'hui (risque chimique principal restant). Refus avec `remaining_ml`.
2. **Stabilisation post-calibration** : injection manuelle possible pendant la fenêtre → à refuser avec `seconds_remaining`.
3. **Limite horaire** (`usedMs` vs `*_limit_minutes`) : ni vérifiée ni **incrémentée** par le manuel.
4. **Cycles/jour + anti-rafale** (fenêtres 1 min/15 min) : ni vérifiés ni **enregistrés** (utiliser `recordCycleTimestamp` existant, feature-039).
5. **Double démarrage** : 2ᵉ `start` pendant une injection → 409 `already_injecting`.
6. **Format d'erreur structuré** : 409 en **JSON** `{"error":"<code>", "message":"<français>", "seconds_remaining"?:N, "remaining_ml"?:N}` (aujourd'hui text/plain). Codes : `filtration_off` (garde existante à migrer vers ce format), `daily_limit`, `hourly_limit`, `stabilization_in_progress`, `max_cycles`, `burst_limit`, `already_injecting`.
7. **UI proactive** : bouton « Injecter » désactivé (avec raison en hint) quand l'état WS connaît déjà un blocage (limite atteinte, stabilisation, filtration OFF) ; toast rouge français sur 409 (tous codes).

## Approche technique imposée (pattern ADR-0017, cohérence 036/039)
- **Fonction PURE** dans `src/dosing_logic.{h,cpp}` :
  `ManualInjectDecision evaluateManualInject(const ManualInjectInputs& in);`
  avec `ManualInjectInputs` (POD) : `filtrationOk, stabilizationActive, stabilizationRemainingS, alreadyInjecting, requestedMl, dailyInjectedMl, maxDailyMl, usedMs, hourlyLimitMs, requestedDurationMs, cyclesToday, maxCyclesPerDay, cyclesLastMin, maxCyclesPerMin, cyclesLast15Min, maxCyclesPer15Min` — et `ManualInjectDecision { bool allowed; ManualInjectRefusal cause; float remainingMl; }`.
  Ordre des gardes aligné sur `evaluateDose` (filtration → stabilisation → déjà en cours → journalier → horaire → cycles → anti-rafale). Pas de garde « mode » ni « filtre capteur » (le manuel est aveugle à la mesure — voulu).
- **Coquille** : la route collecte les valeurs (comme `canDose()`), appelle la fonction pure, formate le JSON 409. Le helper `injectionAllowedOrReject` actuel est absorbé/remplacé (la garde filtration passe dans la fonction pure ; l'exemption « continu » reste côté collecte).
- **Enregistrement** : au démarrage manuel accepté → `recordCycleTimestamp` (ring anti-rafale partagé), incrément cycles, mise à jour `usedMs` pendant l'injection, flush NVS des compteurs (cohérence avec l'auto).
- **Décision à trancher par l'architect** : `usedMs` manuel — compteur partagé avec l'auto (recommandé : budget horaire unique) ou séparé.

## Tests natifs (OBLIGATOIRE — étendre `test/test_native_dosing/test_dosing_decision.cpp`)
- Un test par cause de refus d'`evaluateManualInject` (7 causes) + cas nominal.
- **Reliquat** : `dailyInjectedMl=290, max=300, requested=30` → refus `daily_limit` avec `remaining_ml=10` ; `requested=10` → accepté.
- Frontières : `cumul+volume == max` → accepté (ou refusé — à figer par pool-chemistry, puis verrouillé par test) ; `usedMs + durée == limite` idem.
- Ordre de priorité des gardes (entrées multiples en échec).
- Anti-rafale : réutiliser les helpers 039 (`countCyclesInWindow`) — vérifier qu'une injection manuelle enregistrée compte dans la fenêtre vue par l'AUTO (garde partagée).
- Non-régression : les 118 tests existants restent verts ; couverture `dosing_logic` reste 100 % lignes (`./tools/coverage.sh`).

## Cas limites
- WS déconnecté : bouton reste cliquable (le firmware décide) ; message d'erreur affiché.
- Volume > reliquat : l'UI peut proposer le reliquat (optionnel) ; le firmware refuse de toute façon.
- Stabilisation expirant pendant l'injection : sans effet.
- Mode régulation `manual` : toutes les gardes chimiques s'appliquent (y compris filtration — déjà le cas aujourd'hui via `injectionAllowedOrReject` ; ne PAS l'affaiblir).

## Contraintes techniques
- Fonction pure : headers C uniquement (pas d'Arduino/String — codes d'erreur en enum, formatage français dans la coquille, comme le mapping `DoseRefusal`).
- Handlers AsyncWebServer < 50 ms ; pas de compat ascendante (l'ancien comportement est un bug de sécurité).
- Budget FS : partition **576 KB** (layout v3, ADR-0019) — impact frontend minime.
- `pio run` + `pio test -e native` + `node --check` + `./build_fs.sh` verts.

## Livrables — check-list
- [x] A1 — `evaluateManualInject` + types dans `dosing_logic.{h,cpp}` (pur, ordre des gardes aligné, commentaires frontières)
- [x] A2 — Coquille `/ph/inject/start` : collecte + JSON 409 structuré + enregistrement (cycles/anti-rafale/usedMs/NVS)
- [x] A3 — `/orp/inject/start` symétrique
- [x] A4 — Garde filtration existante migrée dans le flux pur (comportement identique, format JSON — `injectionAllowedOrReject` conservé pour `/pumpN/on` seulement, même format JSON)
- [x] A5 — 409 `already_injecting` sur double start
- [x] B1 — UI : toasts français par code d'erreur (+ `seconds_remaining`/`remaining_ml` affichés)
- [x] B2 — UI : bouton Injecter désactivé + hint selon l'état WS (limite/stabilisation/filtration)
- [x] D1 — Tests natifs : 7 causes (+ watchdog = 9 gardes) + nominal + reliquat + frontières + ordre + partage anti-rafale avec l'auto (17 tests `test_F006_*`)
- [x] D2 — `pio test -e native` tous verts (135) ; couverture dosing_logic 100 % maintenue
- [x] C1 — `docs/API.md` : codes 409 JSON des endpoints `/inject/*` (tableau 8 codes + note `/pumpN/on`)
- [x] C2/C3 — `docs/features/page-ph.md` / `page-orp.md` : ⚠️ « limites volumétriques non gardées » retirés, gardes + toasts + blocage proactif documentés
- [x] C4 — `docs/subsystems/pump-controller.md` : section « Gardes des injections manuelles (feature-006) »
- [x] C5 — `CHANGELOG.md` 2.6.0 : « Sécurité : injections manuelles soumises aux limites journalières/horaires, stabilisation et anti-rafale »

## Sécurité chimique
- Dosage impliqué : **oui** (pH-/pH+/chlore). Double passage pool-chemistry : (1) valider l'ordre des gardes, les frontières (`==` limite) et le partage du budget horaire/anti-rafale avec l'auto ; (2) équivalence sur code réel avant revue.
- Risque traité : dépassement silencieux du budget journalier / injection pendant stabilisation — dernier écart manuel/auto.
- Invariants : gardes de l'AUTO inchangées ; `inject/stop` jamais gardé ; garde filtration jamais affaiblie.

## Critères d'acceptation
- [x] AC1 — `POST /ph/inject/start?volume=30` avec cumul 290/300 → **409 JSON** `daily_limit`, `remaining_ml:10`, pompe à l'arrêt ; `volume=10` → 200 et cumul 300 + `ph_limit_reached`.
- [x] AC2 — Pendant stabilisation → 409 `stabilization_in_progress` + `seconds_remaining` ; filtration OFF (mode piloté) → 409 `filtration_off` (JSON).
- [x] AC3 — Limite horaire / 20 cycles jour / rafale → 409 (`hourly_limit`/`max_cycles`/`burst_limit`) ; l'injection manuelle acceptée est VISIBLE par l'anti-rafale de l'auto (ring partagé).
- [x] AC4 — Double start → 409 `already_injecting` ; stop toujours OK, même pendant stabilisation.
- [x] AC5 — UI : bouton désactivé + hint quand blocage connu ; toast français avec la raison sur 409.
- [x] AC6 — Tests natifs D1 verts ; 118 tests existants non régressés (135 au total) ; couverture 100 % maintenue.
- [x] AC7 — `pio run`/`build_fs`/`node --check` verts ; docs C1-C5 à jour (+ ADR-0020 créé).
- [x] AC8 — Validation on-target (plan de test manuel ci-dessous).

## Plan de test manuel (on-target)
1. Cumul 290/300 : injection 30 mL → refus + toast ; 10 mL → OK, `ph_limit_reached` levé, dosage auto suspendu.
2. Filtration OFF (piloté) : refus ; filtration ON pendant stabilisation post-cal : refus avec compte à rebours ; après expiration : OK.
3. Double start → refus ; stop → OK.
4. Enchaîner 7 injections manuelles d'1 s en < 1 min → refus `burst_limit` à la 7ᵉ (seuil 6/min partagé).

## Hors périmètre
- Bouton « injecter le reliquat » (optionnel post-livraison) ; journal par injection (feature-008) ; ergonomie du bloc injection ; entités HA.

---

## Historique
- **2026-07-04 — implémentation livrée (v2.6.0)** : pool-chemistry GO pré + post ; 17 tests natifs `test_F006_*` (135 total, dosing_logic 100 %). Décisions figées : watchdog en 1ʳᵉ garde (via `loopTaskHandle` — async_tcp non abonnée TWDT), frontières `==` acceptées (journalier/horaire), budget horaire **partagé** auto+manuel (**ADR-0020** — prédicat safety-tracking : pompes test/UART comptent aussi), ring anti-rafale/cyclesToday partagés via pending atomique consommé en loopTask, volume évalué post-clamp. Bypass UART `pump_test` assumé (écran non déployé, condition pool-chemistry #4 — documenté). Docs C1-C5 + ADR-0020 livrés. **Reste : AC8 (validation on-target)** — la spec reste en `doing/`.
- **2026-04-24 — rédaction initiale** : partait du constat « l'injection manuelle contourne tout » (aucune garde au démarrage).
- **2026-07-04 — révision 2 (audit code v2.5.2)** : la garde filtration (démarrage + interruption en cours + alerte MQTT), les bornes volume/durée et la gestion 409 de base existent déjà (implémentées au fil des features sécurité). Périmètre recentré sur les gardes manquantes (journalier/stabilisation/horaire/cycles/anti-rafale/double-start), approche réécrite sur le pattern `dosing_logic` pur + tests natifs (ADR-0017, features 036/039 — inexistants en avril), références mises à jour (FS 576 KB layout v3, version cible 2.6.0, anti-rafale `recordCycleTimestamp`).

---

## Clôture (2026-07-04)
- **AC8 validé on-target par l'utilisateur** : gardes journalière/stabilisation/double-start/anti-rafale vérifiées ; scénario « augmentation de limite en cours de journée » testé pH et ORP.
- **Fix découvert pendant AC8** : le drapeau `ph/orp_limit_reached` était latché jusqu'à minuit — augmenter la limite débloquait le dosage réel mais pas l'UI/alarme. `checkSafetyLimits()` dé-latche désormais dès que `cumul < limite` (validé pool-chemistry : GO, drapeau = miroir, gardes réelles déjà vives). L'anomalie ORP rapportée s'est révélée transitoire (retest OK).
- Bilan cycle : GO pool-chemistry ×3 (pré, post, fix), code-reviewer Approved (+3 mineures corrigées dont stabilisation par pompe dans le WS et garde flow≤0), 17 tests natifs (135 total, dosing_logic 100 %), ADR-0020 (budget horaire unique).
- Livré en **v2.6.0**, commit local SANS push (consigne utilisateur : release groupée plus tard).
