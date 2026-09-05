## Métadonnées
- **ID** : feature-037
- **Slug** : dosage-proportionnel-testable
- **Type** : feature (refactor de testabilité + tests)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : N/A — refactor interne strictement préservateur de comportement (bump CORRECTIF optionnel à la release)
- **Dernière mise à jour** : 2026-06-27
- **Spec(s) liée(s)** : feature-036 (décision de dosage pure + tests, ADR-0017), feature-025 (anti-windup B6, pause mélange), ADR-0016 (régulation P temporisée)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Capteurs (pH / ORP / température)
- [ ] Filtration / Éclairage
- [ ] Web / WS / MQTT
- [x] Tests natifs (`test/`, env `native` / `native_coverage`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** (calcul de dosage / régulation)
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Extraire le **calcul du débit proportionnel à l'erreur** (le « P » de la régulation P-temporisée) de `pump_controller` vers la logique pure testable, puis ajouter des tests Unity vérifiant que le dosage **varie proportionnellement** à l'écart à la cible, qu'il est **borné** (min/max) et que l'**anti-windup** gèle l'intégrale à saturation. Aucun changement de comportement.

## Contexte
feature-036 a extrait et testé la **décision** de dosage (`evaluateDose`, `shouldStart/ContinueDosingPure` — usage booléen de l'erreur : « faut-il doser ? »). Mais le **calcul de la quantité** (« combien doser ? ») est resté dans `pump_controller.cpp` et n'a **aucun test** :
- `computePID()` (pump_controller.cpp ~L436) — sortie proportionnelle `kp * error` (+ termes Ki/Kd à 0 en P-temporisée, ADR-0016) + gel d'intégrale anti-windup ;
- `computeFlowFromError()` (~L490) ;
- bornage `constrain(pidOutput, minFlowMlPerMin, maxFlowMlPerMin)` (~L775/L924) ;
- détection de saturation `kp*error >= maxFlow` → `freezeIntegral` (feature-025 B6, ~L771/L920).

C'est le cœur de la justesse du dosage (proportionnalité, bornage, anti-windup) et il est aujourd'hui validé uniquement par revue, jamais par test automatique.

## Comportement attendu
Comportement **observable strictement inchangé** : pour des entrées identiques (kp, error, deadband, min/max flow, état intégral, dt, freezeIntegral), la sortie débit et l'évolution de l'intégrale sont **identiques** à l'implémentation actuelle.

### Refactor (characterization, préservation stricte)
- Extraire dans la logique pure (`src/dosing_logic.*` ou module pur dédié, sans Arduino/FreeRTOS) :
  - le calcul proportionnel + bornage : une fonction pure renvoyant le débit à partir de `kp`, `error`, `deadband`, `minFlow`, `maxFlow` (et, pour rester fidèle, la part Ki/Kd si conservée — à 0 en P-temporisée) ;
  - la mise à jour d'intégrale **anti-windup** : fonction pure recevant l'intégrale courante, `error`, `dt`, `ki`, `freezeIntegral` (saturation) et renvoyant la nouvelle intégrale + la sortie (l'état PID est passé en paramètre, pas lu en global ; cf. pattern temps injecté de feature-036).
- `computePID()` / `computeFlowFromError()` de `pump_controller` deviennent des coquilles qui collectent l'état PID + les params pompe et délèguent à la logique pure. Le `constrain` final et l'écriture du duty PWM restent dans la coquille (couche hardware).

### Cas limites
- `error <= deadband` → sortie nulle (pas de dosage).
- `error` très grand → débit **borné à maxFlow**.
- `error` juste au-dessus du deadband → débit **≥ minFlow** (pas de flow=0 à la première itération, cf. commentaire L794 actuel).
- Saturation (`kp*error >= maxFlow`) → **intégrale gelée** (n'augmente pas).
- `error` négative (pH au-delà de la cible côté opposé) → pas de dosage / reset intégrale (comportement actuel L812).
- P-temporisée (Ki=Kd=0) → sortie = `kp*error` bornée, intégrale reste 0.

## Comportement actuel
Le calcul proportionnel + anti-windup vit dans `pump_controller.cpp` (computePID L436, computeFlowFromError L490), couplé à l'état `PIDController` et aux `PumpControlParams`. Aucun test natif ne l'exerce. feature-036 ne couvre que la décision booléenne.

## Contraintes techniques
- Logique pure : pas de `<Arduino.h>` réel, pas de FreeRTOS, pas de `<vector>` ; `<stdint.h>`/`<math.h>` comme dosing_logic existant (compat libc++-absent du host CI).
- État PID (intégrale) passé en paramètre / renvoyé, jamais lu en global depuis la logique pure.
- Aucun gain (`kp`, `ki`, `kd`), seuil (deadband 0.05 / stop 0.01) ou débit (`minFlowMlPerMin`, `maxFlowMlPerMin`) modifié.
- Watchdog, gardes `canDose`, pause mélange : hors périmètre, inchangés.

## Fichiers potentiellement concernés
- `src/dosing_logic.{h,cpp}` — ajout des fonctions pures proportionnelles + anti-windup (ou nouveau module `src/regulation_math.{h,cpp}` si plus clair — arbitrage architect).
- `src/pump_controller.cpp` — `computePID`/`computeFlowFromError` deviennent des coquilles.
- `src/pump_controller.h` — signatures éventuelles.
- `test/test_native_dosing/test_dosing_decision.cpp` (ou nouveau `test/test_native_regulation/`) — tests de proportionnalité.
- `platformio.ini` — `build_src_filter` si nouveau module pur.
- `docs/subsystems/pump-controller.md`, `CHANGELOG.md`.

## Nouvelles signatures / API (indicatives — architect arbitre)
- `float computeProportionalFlow(float kp, float error, float deadband, float minFlow, float maxFlow);`
- `struct PidStep { float output; float integral; };`
- `PidStep computePidPure(float kp, float ki, float kd, float error, float prevError, float integral, float dtSec, float deadband, float minFlow, float maxFlow, bool freezeIntegral);`
- Aucun endpoint HTTP / champ WS / topic MQTT.

## Sécurité chimique (obligatoire)
- Dosage impliqué : **oui** (calcul de la quantité injectée) — pH-/pH+/chlore.
- Limites journalières/horaires : inchangées (hors périmètre, gérées par `evaluateDose`/coquille).
- Filtration requise : inchangé.
- Calibration : non.
- Algorithme PID touché : **oui (extraction, sans changement de gains ni de formule)**.
- Risque identifié : **régression silencieuse du débit** (mauvais bornage, anti-windup cassé → sur/sous-dosage). Mitigation : équivalence stricte validée `pool-chemistry` (mêmes sorties pour un jeu d'entrées de référence), tests verrouillant proportionnalité/bornage/gel intégrale, aucun gain/seuil modifié. Le pire cas reste borné par `maxFlow` + limites horaire/journalière de feature-036 (inchangées).

## ADR attendu ?
- **Probable** : non — réutilise le pattern Humble Object déjà acté en **ADR-0017**. À confirmer par `architect` ; si rien de structurant nouveau → « aucun des 4 critères rempli ».

## Éléments visibles (pour doc-writer)
### Endpoints / WS / MQTT
- aucun (inchangés)
### Documentation
- `docs/subsystems/pump-controller.md` — section calcul proportionnel pur + anti-windup, testabilité.
- `CHANGELOG.md` — entrée (refactor interne + couverture du calcul de dosage).
- ADR : seulement si `architect`/`pool-chemistry` le justifient.

## Critères d'acceptation
- [ ] AC1 — Équivalence stricte : pour un jeu d'entrées de référence, la fonction pure renvoie le **même débit** et la **même intégrale** que `computePID`/`computeFlowFromError` actuels (table validée `pool-chemistry`).
- [ ] AC2 — Proportionnalité : le débit **croît avec l'erreur** au-dessus du deadband (monotone) ; `flow(error_petit) < flow(error_grand)`.
- [ ] AC3 — Bornage haut : une erreur largement supérieure à `maxFlow/kp` → débit = `maxFlow`.
- [ ] AC4 — Bornage bas : une erreur juste au-dessus du deadband → débit **≥ minFlow** (jamais 0 hors deadband).
- [ ] AC5 — Deadband : `error <= deadband` → débit **0**.
- [ ] AC6 — Anti-windup : quand `freezeIntegral` (saturation), l'intégrale renvoyée **n'augmente pas**.
- [ ] AC7 — P-temporisée (Ki=Kd=0) : sortie = `kp*error` bornée, intégrale reste 0.
- [ ] AC8 — `pio test -e native` tous PASSED ; couverture des nouvelles fonctions ≥ seuil ; `pio run` esp32dev SUCCESS sans warning.
- [ ] AC9 — Aucun gain/seuil/débit modifié (diff = déplacement pur).
- [ ] Libellés / logs inchangés.

## Plan de test manuel
1. `pio test -e native` → tous verts ; `./tools/coverage.sh` → fonctions proportionnelles couvertes.
2. `pio run` → SUCCESS sans warning.
3. On-target : régulation pH auto avec une **grosse** erreur (cible loin du pH) → vérifier dans les logs un débit proche de `maxFlow` ; puis cible proche → débit moindre. Confirme la proportionnalité réelle.

## Notes pour agent
- **Invariant directeur** : characterization refactor — aucune modification de la formule, des gains, des seuils ou des bornes. On déplace pour tester.
- Attention à l'**état PID** : le rendre explicite (paramètre + retour), ne jamais le lire en global depuis la logique pure.
- Réutiliser l'env `native`/`native_coverage` et le pattern de tests de feature-036.
- Le calcul `dt` (temps écoulé) reste dans la coquille (millis) ; la logique pure reçoit `dtSec`.

## Hors périmètre
- Modification des gains PID, du deadband, des débits min/max, des durées de mélange.
- Réactivation de Ki/Kd (la P-temporisée reste le défaut, ADR-0016).
- Extraction de `update()` au-delà du strict nécessaire.
- Tests on-target automatisés.

---

## Suivi / itérations (clôture)

**Statut : livré — tests verts, revue Approved, pool-chemistry GO (équivalence stricte, 2 passages).** Version 2.2.7.

### Décisions
- Extraction du cœur PID dans `computePidPure` (src/dosing_logic.{h,cpp}), pur/POD, `<stdint.h>`+`<math.h>` (clamp maison `clampf`).
- **Option Y** : le bornage final `[minFlow, maxFlow]` est intégré au pur ; le `constrain` externe des chemins pH/ORP (ex-L775/L924) est supprimé. Équivalent au comportement d'origine (validé sur source).
- État PID via `struct PidResult { flow; integral; lastError; }` (paramètre + retour, jamais global). `computePID` coquille : dt/millis, gardes `lastTime==0` et `dt<=0||dt>10`, flag `freezeIntegral`, reset négatif intégrale → restent coquille.
- `computeFlowFromError` confirmée **code mort** → marquée DEAD, non extraite.
- ADR : réutilise **ADR-0017** (Humble Object), aucun nouvel ADR.

### Équivalence (pool-chemistry)
- Table de référence chiffrée (pH kp=8, ORP kp=0.3, integralMax=50, deadband 0.05/15, minFlow=5.2, maxFlow=90). 10 cas équivalents.
- **Cas erreur négative** : table d'origine indiquait flow=0 par erreur (constrain oublié) ; l'original ET le refactor donnent **5.2** (plancher minFlow) → équivalence préservée. Cas purement défensif : `shouldStartDosing` exige `error > startThreshold` positif, donc jamais atteint en régulation réelle.
- Aucun gain/seuil/débit modifié.

### Incident traité
- `src/config.h` portait une modification manuelle **non commitée** (plafonds journaliers 300→1000 / 500→1500) — détectée par code-reviewer (Blocked sécurité). Confirmée « essai manuel » par l'utilisateur → **rétablie à 300/500** (valeurs doc/main), exclue de ce commit.

### Tests
- `test/test_native_dosing/test_dosing_decision.cpp` : +9 tests (AC1 table pH+ORP, AC2 proportionnalité monotone, AC3 bornage haut, AC4 bornage bas, AC5 deadband strict, AC6 anti-windup, AC7 P-temporisée, erreur négative).
- `pio test -e native` : **51 PASSED**. `computePidPure` 100% lignes. `pio run` esp32dev SUCCESS.

### Fichiers
- `src/dosing_logic.{h,cpp}`, `src/pump_controller.{cpp,h}`, `test/test_native_dosing/test_dosing_decision.cpp`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`, `src/version.h` (2.2.7).

### Reste (non automatisable CI)
- Validation on-target (AC : proportionnalité réelle) : grosse erreur → débit proche de maxFlow ; petite erreur → débit moindre.
