# feature-073 — Accumulateur de dosage sous le pouls minimal (suppression de la zone morte)

## Métadonnées
- **ID** : feature-073
- **Slug** : accumulateur-dosage-sous-plancher
- **Type** : feature (évolution du modèle de dosage v2.21.0)
- **Statut** : doing — implémentation livrée (tests verts 649/649, revue Approved, gate `pool-chemistry` GO + GO contre-visite, docs/ADR-0046 à jour) ; reste E4 on-target à vérifier post-OTA
- **Priorité** : normale
- **Version cible** : 2.22.0 (changement de comportement de régulation)
- **Dernière mise à jour** : 2026-08-23
- **Spec(s) liée(s)** : bug-dosage-fantome (itération 2, ADR-0043), ADR-0044

## Domaines touchés
- [ ] Capteurs
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket (à confirmer : les champs calendrier existants suffisent a priori)
- [ ] MQTT / Home Assistant
- [ ] OTA / Authentification / Historique
- [ ] Frontend (`data/`) (à confirmer : « Dosage en attente » couvre déjà l'état, libellé à ajuster ?)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — OBLIGATOIRE (modifie le refus seuil C de l'ADR-0043, chemin de dosage)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer (seulement si un libellé/champ bouge)
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Sous le pouls minimal de 5 s, la régulation refuse aujourd'hui d'injecter (seuil C, ADR-0043) : toute demande < `Famp × 5/60` (~2,1 ml/min à l'amplitude 50 %) est ignorée. Conséquence de terrain (23/08/2026) : **le pH stagne à ~cible + 0,26** (kp=8) et n'atteint jamais la consigne. L'accumulateur remplace ce refus : la demande sous le plancher est **cumulée de période en période**, et dès que le cumul vaut le volume d'un pouls minimal, une impulsion de 5 s part — une demande de 0,7 ml/min devient une impulsion de 5 s toutes les ~3 minutes. La zone morte disparaît, le pouls minimal et toutes les limites restent respectés.

## Contexte
Observé en production le 23/08 pendant le rattrapage : impulsions raccourcissant vers le plancher de 5 s à mesure que l'erreur diminue, arrêt prévu vers pH 7,46 pour une cible à 7,20. L'utilisateur veut atteindre la cible. Le raffinement avait été identifié dès la conception (proposition faite au gate le 23/08) et écarté du premier cycle pour limiter le périmètre. Décision utilisateur explicite : accumulateur SEUL, la période reste à 60 s (l'allongement à 120 s proposé pour ménager les démarrages est refusé pour ce cycle).

## Comportement attendu
- Demande ≥ plancher : comportement actuel strictement inchangé (impulsion proportionnelle par période).
- Demande > 0 mais < plancher, séance ouverte : cumul du volume demandé (`demande × période`) dans un accumulateur par produit ; quand le cumul ≥ volume du pouls minimal (`Famp × kPulseMinMs`), une impulsion de 5 s (pouls minimal exactement) part dans la période courante et le cumul est décrémenté du volume tiré.
- Le volume compté reste `débit pesé × temps de pouls` (kick compris) — l'accumulateur ne crédite RIEN par lui-même, il ne fait que déclencher des impulsions réelles.
- L'accumulateur est **borné** (jamais plus d'un pouls minimal d'avance, pas de rafale de rattrapage) et **remis à zéro** à la fermeture de séance, au reboot (pas de persistance), et sur tout refus de gate (calibration invalide, B2, limites).
- Toutes les gardes restent évaluées à CHAQUE période avant de tirer : présence d'eau, limites journalière/horaire/cycles, calibration, drapeau unstable. Un pouls d'accumulateur n'a aucun passe-droit.
- Cas UI : l'état existant « Dosage en attente : Ns » reste correct entre deux pouls d'accumulateur (échéance = fin de période) ; si l'architect juge un libellé dédié utile, périmètre mini.

### Cas limites
- Erreur < seuil de déclenchement (0,05) : pas de séance, pas de cumul (inchangé).
- Demande nulle (pH sous la cible) : cumul figé ou remis à zéro (à trancher par le gate — recommandation : remise à zéro, une demande disparue ne doit pas laisser de dette).
- Séance fermée par limite atteinte : cumul perdu (voulu).
- La demande repasse au-dessus du plancher avec un cumul non nul : le cumul est abandonné ou fusionné (à trancher par le gate — recommandation : abandonné, le régime proportionnel reprend ses droits).

## Comportement actuel
`planPulse` (src/pulse_dosing_logic.cpp) retourne `refused=true` quand `periodMs × meanFlow / ampFlow < minPulseMs` (seuil C, ADR-0043). `pump_controller` journalise en info throttlé et ne tire pas. Zone morte = `Famp × 5/60 ÷ kp` en pH.

## Contraintes techniques
- Logique 100 % PURE dans pulse_dosing_logic (ADR-0017) : l'accumulateur est un paramètre d'entrée/sortie de la planification, testable en natif.
- Aucune modification de la comptabilité (`pulseDeltaVolumeMl`), du kick, des getters calendrier v2.21.1 (le pouls d'accumulateur EST un pouls normal de 5 s — les champs `pulse_ms/phase` le décrivent déjà).
- Invariants ADR-0043/0044 : période fixe 60 s (décision utilisateur), pouls min 5 s, kick 400 ms, usedMs en pompe-ON, cycle = séance.

## Fichiers potentiellement concernés
- `src/pulse_dosing_logic.{h,cpp}` — extension de la planification (accumulateur en entrée/sortie)
- `src/pump_controller.{h,cpp}` — état accumulateur par produit, remises à zéro, log
- `src/constants.h` — borne de l'accumulateur si constante dédiée
- `test/test_native_dosing/` — tests de l'accumulateur
- `src/version.h` — 2.22.0
- `docs/subsystems/pump-controller.md`, `docs/adr/` (amendement ou successeur de l'ADR-0043), `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Logique pure d'accumulateur (planification étendue), bornée, sans passe-droit sur les gardes (`planPulseWithCarry`/`PulseCarryPlan`, `src/pulse_dosing_logic.{h,cpp}`)
- [x] A2 — Intégration pump_controller pH ET ORP, remises à zéro (ouverture ET fermeture de séance — fermeture inconditionnelle à chaque tick sans débit, couvre tous les refus de gate —, reboot non persistant)
- [x] A3 — Log : première impulsion d'accumulateur d'une séance journalisée en info (`_carryPulseLogged`, pas de spam) ; anciens logs « injection différée (seuil C) » supprimés
- [x] A4 — version.h → 2.22.0
- [x] D1 — Tests natifs : 9 tests `test_F073_*` (cumul exact, tir à égalité incluse, résidu + invariant balayé 200 périodes, scénario long 0,7 mL/min, F2 bit-identique, entrées invalides, carry suspect, domaine du pouls, balayage 255 demandes ≥ plancher)
- [x] D2 — pio run 0 warning + pio test -e native sans régression (649/649, base 640 + 9)
- [x] C1 — docs (pump-controller.md, logger.md, CHANGELOG, ADR-0046 + amendement croisé ADR-0043)

## Sécurité chimique (obligatoire)
- Dosage impliqué : OUI — pH− (acide) et chlore
- Limites journalières/horaires : inchangées, évaluées avant chaque pouls d'accumulateur
- Filtration requise avant action : inchangée
- Calibration modifiée : non
- Algorithme PID touché : non (kp intact) — seule la RESTITUTION des faibles demandes change
- Risque identifié : sur-dosage si l'accumulateur n'était pas borné ou survivait aux refus → bornes et remises à zéro exigées ci-dessus, à valider par le gate

## ADR attendu ?
- **Probable** : oui — critère 3 (alternatives crédibles : refus/deadband ADR-0043 vs accumulateur vs kp plus fort) et critère 1 (le seuil C de l'ADR-0043 change de sémantique)
- **Sujet pressenti** : accumulateur de demande sous le pouls minimal (amende le refus seuil C de l'ADR-0043)
- ✅ **Créé** : [ADR-0046](../../../docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md) (note d'amendement croisée ajoutée dans l'ADR-0043)

## Critères d'acceptation
- [x] En natif : une demande constante de 0,7 ml/min (amplitude 25 ml/min) produit une impulsion de 5 s à cadence régulière, volume moyen restitué = demande à ±5 % (`test_F073_scenario_long_0_7_ml_min_volume_et_cadence`, 60 périodes, 18 tirs). *Précision corrigée au gate : la cadence réelle est **3-4 périodes** (intervalle moyen V_pouls/0,7 ≈ 3,32), pas « toutes les 3 » — le libellé initial supposait le volume nominal, or V_pouls est **kick-inclus** (≈ 2,32 mL, décision gate n°1).*
- [x] Jamais d'impulsion < 5 s ; jamais plus d'une impulsion par période ; cumul jamais ≥ volume d'un pouls min en sortie (invariant balayé sur 200 périodes + tir à égalité exacte incluse)
- [x] Refus de gate ou fin de séance → cumul à zéro (F1/F2 testés en pur ; côté coquille, la fermeture de séance est inconditionnelle à chaque tick sans débit et remet `_pulseCarryMl` à zéro — vérifié en revue, Approved)
- [ ] On-target : le pH franchit l'ancien point de stagnation (~cible + 0,26) et continue vers la cible — **à vérifier post-OTA (E4, seul critère restant — la spec reste en `doing/` en attendant)**
- [x] `pio run` 0 warning, natifs sans régression (649/649 : 640 + 9)

## Hors périmètre
- Allongement de la période (120 s) — refusé explicitement par l'utilisateur pour ce cycle
- Toute modification de kp / regulation_speed
- Persistance de l'accumulateur au reboot

## Notes d'implémentation (2026-08-23, v2.22.0)

**Verdicts** : gate `pool-chemistry` GO amont (sous 5 conditions) + GO en contre-visite (conditions vérifiées ligne par ligne) ; revue `code-reviewer` : **Approved** ; tests : **649/649** (640 base + 9 nouveaux `test_F073_*`).

**Décisions du gate** (verrouillées dans [ADR-0046](../../../docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md)) :
- **n°1 — V_pouls kick-inclus** : seuil de tir ET décrément = `pulseDeltaVolumeMl(0, kick, 0, poulsMin, flow255, ampFlow)` (volume RÉEL), jamais le nominal `ampFlow × 5 s` (sinon +10-12 % de sur-livraison).
- **n°2 — séance unique** : l'accumulation vit dans une séance ordinaire OUVERTE (jamais une séance par pouls — ~480 séances/j auto-bloquantes par `maxCyclesPerDay`). L'UI existante « Dosage en attente : Ns » couvre l'état (G1 confirmé — zéro modif web).
- **n°3 — aucune dette héritée** : cumul remis à zéro à chaque ouverture de séance.
- **Cas limites tranchés** : F1 — demande nulle → cumul remis à zéro (une dette sans demande vivante n'a pas de justification chimique) ; F2 — retour ≥ plancher → cumul abandonné, délégation bit-identique à `planPulse` (fusionner = compter deux fois la même erreur) ; F3 — tir à cumul ≥ V_pouls (égalité incluse), résidu conservé, ≤ 1 pouls/période, invariant de sortie `cumul < V_pouls`.

**Choix de l'implémenteur** :
- `flow255` passé à `planPulseWithCarry` = celui de la **pompe affectée** (`pumpFlow255(phPumpIdx/orpPumpIdx)`), cohérent avec la comptabilité par pompe.
- **Ouverture de séance déplacée AVANT le plan** dans `update()` : la séance s'ouvre aussi en régime d'accumulation (nécessaire pour maintenir le latch actif) ; `phStartDecided`/`orpStartDecided` couvrent la reprise après suspension hors-branche.
- Fermeture de séance **inconditionnelle** à chaque tick sans `flow > 0` (bloc `!phActive`/`!orpActive` en fin d'`update()`) → couvre TOUS les refus (gate, `canDose`, limites, suspension) sans énumération ; y remet `_pulseCarryMl` à zéro et réarme `_carryPulseLogged`.
- Comportement ≥ plancher **bit-identique** vérifié en natif sur un balayage de 255 demandes.

**Issue mineure de revue (assumée, cosmétique)** : le « X mL cumulés » du log A3 (`résidu + V_pouls`) peut être légèrement faux si l'écrêtage défensif (`carry ≥ V_pouls` en entrée) a joué — le volume **crédité**, lui, reste exact (chemin standard `pulseDeltaVolumeMl`). Documenté dans pump-controller.md et ADR-0046.

**Fichiers modifiés** : `src/pulse_dosing_logic.{h,cpp}`, `src/pump_controller.{h,cpp}`, `src/version.h` (2.22.0), `test/test_native_dosing/test_dosing_decision.cpp` (+270 lignes). Docs : `docs/adr/0046-*.md` (+ amendement 0043, index README ADR, docs/README.md), `docs/subsystems/pump-controller.md`, `docs/subsystems/logger.md`, `CHANGELOG.md`.

**Reste ouvert** : E4 on-target (franchissement du point de stagnation ~cible + 0,26) — à vérifier post-OTA ; la spec passe en `done/` à ce moment-là.
