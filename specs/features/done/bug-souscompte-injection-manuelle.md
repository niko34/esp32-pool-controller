# Bug — Sous-compte du cumul journalier en fin d'injection manuelle volumée

- **Type** : bug simple (format minimal `specs/modele.md`)
- **Statut** : done (2026-07-05 — reste validation on-target, voir Notes d'implémentation)
- **Version cible** : 2.9.1
- **Créée** : 2026-07-05 (rapport utilisateur on-target, v2.9.0)
- **Domaine** : dosage → **pool-chemistry + code-reviewer obligatoires**

## Symptôme observé

Limite journalière pH = 50 mL, injection manuelle demandée = 50 mL (frontière `==` acceptée par `evaluateManualInject`, feature-006). À la fin de l'injection : cumul affiché **49 mL**, badge dashboard « Cumul 98 % » figé, le drapeau `ph_limit_reached` ne se déclenche jamais (le badge « Limite journalière atteinte » n'apparaît pas).

## Cause racine

Asymétrie entre la garde d'acceptation (raisonne en **volume exact** `effectiveMl`) et la comptabilité (**intègre débit × temps** avec pertes aux bornes) :
1. `durationS` arrondi à la seconde entière (`web_routes_control.cpp` l.264) → ±0,5 s ;
2. tranche entre le démarrage réel (handler HTTP async) et le premier tick de comptage non comptée ;
3. **dernière tranche jamais comptée** : `updateManualInject()` (loopTask, avant `PumpController.update()` dans la loop) coupe la pompe à l'expiration → au tick suivant `phManualActive` est déjà faux → le delta final est jeté (`lastSafetyTimestamp = 0` sans intégration, `pump_controller.cpp` l.1392-1394).

Résultat : intégré ≈ 49,x < 50 → `dailyInjectedMl >= maxPhMlPerDay` jamais vrai → limite jamais latchée.

## Correctif (validé pool-chemistry avant merge)

**Crédit plancher à la fin naturelle** d'une injection manuelle volumée :
- Au démarrage accepté : mémoriser `startCumulMl` (cumul au départ) et `effectiveMl` dans `ManualInjectState`.
- À la fin **naturelle** (expiration de durée, cas 1 de `updateManualInject()`) : appeler `PumpController.creditManualInjectionFloor(idx, startCumulMl, effectiveMl)` :
  - cas nominal (`dailyInjectedMl >= startCumulMl`, pas de rollover pendant l'injection) : `dailyInjectedMl = max(dailyInjectedMl, startCumulMl + effectiveMl)` ;
  - cas rollover minuit pendant l'injection (`dailyInjectedMl < startCumulMl`) : `dailyInjectedMl = max(dailyInjectedMl, effectiveMl)` — crédite le volume entier au jour nouveau, choix conservateur.
- Arrêts **anticipés** (stop manuel `/inject/stop`, interruption filtration) : PAS de crédit — l'intégration réelle fait foi (compte moins car injecté moins).
- Propriété directrice : **le registre ne sous-compte jamais** un volume promis et injecté en entier (fail-safe chimique) ; il peut sur-compter de quelques dixièmes de mL (conservateur).

## Critères d'acceptation

- [ ] Scénario utilisateur : limite 50, injection 50 → à la fin, cumul = 50,0, `ph_limit_reached = true`, badge « Limite journalière atteinte » (le segment « Cumul » disparaît).
- [ ] Stop manuel à mi-course : cumul = intégration réelle (pas de crédit).
- [ ] Interruption filtration : idem, pas de crédit.
- [x] Symétrie ORP.
- [x] `pio run` sans warning, 152 tests natifs verts (aucun module pur touché a priori).

## Notes d'implémentation (2026-07-05, v2.9.1)

- **Fichiers modifiés** :
  - `src/pump_controller.h` / `src/pump_controller.cpp` : nouvelle méthode `creditManualInjectionFloor(int idx, float startCumulMl, float creditMl)` (`pump_controller.cpp:675`) — plancher du registre de sécurité, gestion rollover minuit, `_dailyCountersDirty` armé, log info `[Sécurité] Compteur journalier ... ajusté de X à Y mL (crédit fin d'injection manuelle)`. Ne touche PAS au suivi produit (`productCfg.*TotalInjectedMl`).
  - `src/web_routes_control.cpp` : `ManualInjectState` enrichi (`startCumulMl`, `creditMl` mémorisés à l'acceptation, l.309-310 / 369-370) ; crédit appelé uniquement au chemin fin naturelle d'`updateManualInject()` (l.178, 207) ; champs remis à zéro sur les 3 chemins d'arrêt ; log de fin de durée enrichi (« cumul crédité à X mL »).
  - `src/version.h` : 2.9.1.
- **Verdicts** : pool-chemistry **GO** (propriété fail-safe : jamais de sous-compte du registre de sécurité, sur-compte de quelques dixièmes de mL toléré ; pas de crédit sur arrêt anticipé) ; code-reviewer **Approved**. `pio run` SUCCESS, 152/152 tests natifs verts.
- **Mineure notée par le reviewer** (bénin, non corrigé) : dans `updateManualInject()`, `active = false` est écrit avant la remise à zéro de `startCumulMl`/`creditMl` — sans conséquence car tout s'exécute en `loopTask` et le crédit lit les valeurs avant leur reset.
- **Reste à valider on-target** (critères 1 à 3 non cochés) : scénario limite 50 / injection 50 → latch `ph_limit_reached` + badge « Limite journalière atteinte » ; stop manuel à mi-course → pas de crédit ; interruption filtration → pas de crédit.
- **Docs** : `docs/subsystems/pump-controller.md` (section « Crédit plancher à la fin naturelle » + API publique), `CHANGELOG.md` [2.9.1]. Pas d'ADR (correctif de comptabilité suivant la propriété fail-safe existante, aucun des 4 critères ADR rempli).
