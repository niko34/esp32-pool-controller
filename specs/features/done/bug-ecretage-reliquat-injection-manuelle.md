# Bug — Refus d'injection manuelle du reliquat exact du quota journalier

- **Type** : bug simple (format minimal `specs/modele.md`)
- **Statut** : done
- **Version cible** : 2.9.2
- **Créée** : 2026-07-05 (rapport utilisateur on-target, v2.9.1)
- **Domaine** : dosage → **pool-chemistry + code-reviewer obligatoires**
- **Spec liée** : `done/bug-souscompte-injection-manuelle.md` (même famille : fractions invisibles vs arithmétique exacte des gardes)

## Symptôme observé

Limite journalière 50 mL, cumul affiché 39 → demande d'injection manuelle de 11 mL refusée « Limite journalière atteinte — reste disponible aujourd'hui : 11 mL » (le message promet 11 et refuse 11).

## Cause racine

La garde accepte bien l'égalité (`cumul + demandé > limite` → refus, `dosing_logic.cpp` l.215, frontière `==` verrouillée par test natif). Mais :
1. le cumul réel est fractionnaire (intégration débit×temps) : 39,x affiché « 39 » ;
2. le volume évalué n'est pas la demande mais `effectiveMl = débit × durationS/60` avec `durationS` arrondi à la seconde **au plus proche** → jusqu'à ±0,5 s d'excès ;
3. donc 39,4 + 11,2 = 50,6 > 50 → refus, `remaining = 10,6` ;
4. le message arrondit le reliquat **au plus proche** (`lroundf`) → « reste 11 mL » : promesse intenable.

## Correctif

**Écrêtage automatique au reliquat** dans les handlers `/ph|orp/inject/start` (la garde pure ne change pas) :
1. `remaining = maxDailyMl − dailyInjectedMl` (si `maxDailyMl > 0`). Si `volumeMl > remaining` et `remaining > 0` :
   - `durationS = floor(remaining / débit × 60)` (arrondi **vers le bas** : le volume effectif ne re-déborde jamais) ;
   - si `durationS < 1` → laisser le refus `daily_limit` (reliquat trop petit pour être injectable) ;
   - sinon écrêter : log `[Injection] volume écrêté de X à Y mL (reliquat journalier)` ;
   - **`creditMl = remaining`** (pas `effectiveMl`) : le crédit plancher de fin d'injection (v2.9.1) porte alors le cumul exactement à la limite → latch `*_limit_reached` + badge « Limite journalière atteinte ». Sur-compte < 1 s de débit, conservateur.
2. Chemin legacy `?duration=` : même écrêtage de la durée.
3. Message et champ `remaining_ml` du refus `daily_limit` : arrondi **vers le bas** (`floorf`) — ne plus jamais promettre plus que disponible.

Propriétés : on n'injecte jamais plus que le reliquat (plus strict qu'avant) ; le registre de sécurité ne sous-compte jamais (héritée v2.9.1).

## Critères d'acceptation

- [ ] Scénario utilisateur : limite 50, cumul ~39, demande 11 → injection démarrée (écrêtée au reliquat), log d'écrêtage, et à la fin cumul = 50,0 → `*_limit_reached` + badge.
- [ ] Reliquat < 1 s de pompe → refus `daily_limit` avec reliquat affiché arrondi vers le bas.
- [ ] Demande ≤ reliquat → comportement inchangé (pas d'écrêtage).
- [x] Symétrie ORP + chemin `?duration=`.
- [x] `pio run` sans warning, 152 tests natifs verts (garde pure inchangée).

## Notes d'implémentation (v2.9.2)

- **Fichiers modifiés** : `src/web_routes_control.cpp` (écrêtage dans les 2 handlers `/ph|orp/inject/start`, `floorf` sur le refus `daily_limit`, `creditMl = remaining` si écrêté sans re-plafonnement / `effectiveMl` sinon), `src/version.h` (2.9.2).
- **Verdicts** : pool-chemistry **GO**, code-reviewer **Approved**, `pio run` SUCCESS, **152/152** tests natifs verts. Reco cosmétique post-revue appliquée : logs de démarrage d'injection à 1 décimale.
- **Garde pure inchangée** : `evaluateManualInject()` / `dosing_logic.cpp` non touchés — l'écrêtage est fait en amont dans les handlers ; frontière `==` toujours verrouillée par test natif.
- **Reste on-target** : les 3 premiers critères (scénario utilisateur écrêtage + latch, refus reliquat < 1 s avec arrondi bas, demande ≤ reliquat inchangée) — non vérifiables sans matériel.
- **Docs** : `docs/API.md`, `docs/subsystems/pump-controller.md`, `docs/features/page-ph.md`, `page-orp.md`, `CHANGELOG.md` [2.9.2].
