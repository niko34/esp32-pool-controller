# ADR-0021 — Répartition du volume quotidien scheduled par fenêtres de 15 min alignées horloge

- **Statut** : Accepté
- **Date** : 2026-07-05
- **Décideurs** : architect + pool-chemistry (GO pré-implémentation sous 5 conditions, toutes levées post-implémentation, feature-011)
- **Spec(s) liée(s)** : feature-011 (répartition 24 h du mode Programmée)

## Contexte

Le mode Programmée ([ADR-0002](0002-mode-programmee-volume-quotidien.md)) injectait le volume quotidien **d'un bloc** : dès le début de la filtration, la pompe tournait au débit effectif jusqu'au quota (`ph/orp_daily_target_ml`) ou à la limite horaire. Conséquence chimique : un pic de concentration localisé en début de plage, aucune répartition sur la journée.

Contraintes à concilier :
- `minInjectionTimeMs` = 30 s (anti short-cycling, pompe péristaltique) ;
- budget horaire **partagé** auto + manuel + test + UART ([ADR-0020](0020-budget-horaire-dosage-unique.md)) ;
- une injection par fenêtre de 15 min sur 8–12 h de filtration = 30–45 démarrages/jour > `maxCyclesPerDay` (20) — tension à arbitrer ;
- les compteurs journaliers se réinitialisent à minuit local ([ADR-0008](0008-persistance-cumuls-journaliers-nvs.md)) : un horizon de répartition qui franchirait minuit serait incohérent ;
- une seule plage de filtration quotidienne (`start`/`end`, éventuellement à cheval sur minuit) ;
- interdiction du PID imbriqué (spec : prévisibilité et testabilité).

## Décision

La répartition est calculée par une **fonction pure** `evaluateScheduledDose()` ([`src/dosing_logic.h`](../../src/dosing_logic.h), pattern [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)), la branche `scheduled` de `pump_controller` n'étant qu'une coquille de collecte/application. Règles retenues :

1. **Fenêtres de 15 min alignées sur l'horloge murale** (`kScheduledWindowMinutes`) : `windowIndex = nowMin / 15` (0..95). L'index absolu rend la répartition **idempotente après redémarrage** (pas de dérive de phase).
2. **Recalcul à chaque nouvelle fenêtre** depuis l'état courant : `v = remaining / nWin` avec `remaining = min(cible, plafond journalier) − injecté du jour` (injections manuelles incluses) et `nWin = horizon / 15` (plancher 1). Auto-correcteur : changement de cible, injection manuelle, retard subi et reboot sont absorbés sans code dédié.
3. **Snapshot `stopTargetMl`** à l'entrée de fenêtre (`injecté + v`) : la cible d'arrêt cumulée est **figée** pour la durée de la fenêtre. `doseNow` reste réévalué à chaque tick contre `min(stopTargetMl, cible effective)` — une baisse de cible en cours de fenêtre arrête l'injection immédiatement.
4. **Report anti short-cycling** : si la durée d'injection de `v` est < `minInjectionTimeMs` (30 s), la fenêtre n'injecte rien ; le volume se reporte mécaniquement (moins de fenêtres restantes → doses plus longues).
5. **Bornage par le budget horaire partagé** (ADR-0020) : `v` est plafonné par le budget restant (`hourlyLimitMs − usedMs`) converti en mL via le débit effectif ; l'excédent se reporte.
6. **Horizon borné à minuit, pas de rattrapage J+1** : le reliquat non injecté à minuit est **perdu** (log `info`). En mode `regulationMode == "continu"`, l'horizon est `1440 − nowMin` (fin de journée).
7. **Exemption `cyclesToday` conservée** (arbitrage R4 validé pool-chemistry) : les démarrages scheduled ne comptent pas dans `maxCyclesPerDay`, car ils sont **bornés structurellement** (≤ 4/h par le cadencement) et le **ring anti-rafale partagé est consulté avant tout démarrage** (6/min, 20/15 min — mêmes seuils que `canDose`). Chaque démarrage est quand même enregistré dans le ring.
8. **Fail-closed** : watchdog inactif, heure locale invalide, horizon ≤ 0 ou débit ≤ 0 → aucune injection.

## Alternatives considérées

- **PID « lent » imbriqué** (rejetée) — asservir un débit continu sur l'écart au quota est imprévisible, difficile à borner chimiquement et intestable simplement ; la spec l'excluait explicitement.
- **Fenêtres relatives au début de plage** (rejetée) — la phase se perd au redémarrage ou au changement de plage ; l'alignement horloge murale donne des fenêtres stables et un `windowIndex` reconstructible à tout instant.
- **Rattrapage J+1 du reliquat** (rejetée) — accumule une « dette de dosage » qui peut se libérer d'un coup le lendemain (surdose) ; décision produit déjà actée par ADR-0002 (journée manquée = perdue).
- **Fenêtre de 30 min** (rejetée) — doses par fenêtre deux fois plus grosses (pics locaux plus marqués) sans bénéfice : le report < 30 s traite déjà les petits volumes avec des fenêtres de 15 min.
- **Limite de cycles dédiée au scheduled** (rejetée) — un compteur supplémentaire serait redondant avec le bornage structurel du cadencement + le ring anti-rafale partagé ; complexité sans gain de sécurité.
- **Zéro état (recalcul continu, sans snapshot)** (rejetée) — recalculer `v` à chaque tick **pendant** l'injection fait tendre le volume de fenêtre vers zéro au fur et à mesure que `remaining` diminue : la dose devient asymptotique et la fenêtre n'atteint jamais sa part. Le snapshot `stopTargetMl` fige la cible d'arrêt à l'entrée de fenêtre.

## Conséquences

### Positives
- Lissage garanti : aucune fenêtre de 15 min n'injecte plus que `remaining / nWin` (borné en plus par le budget horaire) — plus de pic de concentration en début de filtration.
- Auto-correction sans état persistant : cible modifiée à midi, injection manuelle, limite horaire subie ou reboot sont rattrapés au recalcul de la fenêtre suivante.
- Testable en natif : 12 tests dédiés `evaluateScheduledDose` + 5 tests `remainingRangeMinutes` (152 tests au total), fail-closed verrouillé.

### Négatives / dette assumée
- Reliquat perdu si la filtration est trop courte ou en panne (assumé — cohérent ADR-0002).
- **Pompe rapide + petite cible** : les premières fenêtres sont reportées (`v` < équivalent 30 s) ; les injections ne démarrent que quand l'horizon se resserre et que `v` grossit. C'est le fonctionnement voulu du report anti short-cycling, mais l'utilisateur peut être surpris de ne rien voir en début de plage.
- Les démarrages scheduled n'alimentent pas `cyclesToday` : `maxCyclesPerDay` ne reflète que l'automatique + le manuel (assumé, bornage structurel documenté).
- Horloge locale requise : heure invalide (avant NTP/RTC) → dosage scheduled suspendu (warning unique).

### Ce que ça verrouille
- Toute évolution de la répartition scheduled passe par `evaluateScheduledDose()` (logique pure, ADR-0017) — pas de logique de décision dans la coquille.
- La fenêtre de 15 min alignée horloge (`kScheduledWindowMinutes`) et l'horizon **borné à minuit** sont figés ; les changer exige un nouvel ADR.
- Pas de rattrapage J+1 ni de compteur de cycles dédié scheduled sans nouvel ADR.
- Le bornage par le budget horaire partagé (ADR-0020) s'applique au scheduled comme à toute source d'activation.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h) / [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) (`evaluateScheduledDose`), [`src/schedule_logic.h`](../../src/schedule_logic.h) (`remainingRangeMinutes`), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (branches scheduled pH/ORP, `tickDailyRollover`), [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`ph/orp_scheduled_flow_ml_per_min`), [`src/constants.h`](../../src/constants.h) (`kScheduledWindowMinutes`)
- Spec : `specs/features/doing/feature-011-repartition-24h-programmee.md`
- Doc : [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#mode-scheduled), [docs/features/page-ph.md](../features/page-ph.md), [docs/features/page-orp.md](../features/page-orp.md), [docs/API.md](../API.md#websocket-temps-réel)
- ADR liés : [ADR-0002](0002-mode-programmee-volume-quotidien.md) (volume quotidien en mL), [ADR-0008](0008-persistance-cumuls-journaliers-nvs.md) (reset minuit), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure), [ADR-0020](0020-budget-horaire-dosage-unique.md) (budget horaire partagé)
