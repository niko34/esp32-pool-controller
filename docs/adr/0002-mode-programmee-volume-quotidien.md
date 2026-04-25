# ADR-0002 — Mode Programmée exprimé en volume quotidien (mL), pas en cadence

- **Statut** : Accepté
- **Date** : 2026-04 (CHANGELOG [Unreleased])
- **Doc(s) liée(s)** : [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md), [pump-controller.md](../subsystems/pump-controller.md)

## Contexte

Le firmware supporte deux régulations chimiques : pH et ORP (chlore). Certains utilisateurs ne veulent **pas** d'asservissement PID sur la valeur mesurée — parce que la sonde est en bout de course, parce qu'ils n'ont pas installé la sonde ORP, ou simplement parce qu'ils préfèrent une logique de dosage par habitude. Il fallait donc un « mode programmé » tiers entre l'automatique PID et le manuel pur.

Se posait la question : **comment l'utilisateur exprime-t-il sa demande ?**

## Décision

Le mode Programmée est paramétré par un **volume quotidien en mL** (`ph_daily_target_ml`, `orp_daily_target_ml`). Le firmware injecte ce volume pendant les plages de filtration, jusqu'à atteindre le quota journalier. La seule barrière temporelle est la **limite horaire** d'injection (`ph_limit_minutes` / `orp_limit_minutes`).

Le champ est borné côté UI et côté firmware par la limite journalière de sécurité (`max_ph_ml_per_day`, `max_orp_ml_per_day`).

## Alternatives considérées

- **Cadence horaire** (X mL toutes les Y heures, rejeté) — force à recalculer côté UI quand l'utilisateur change la plage de filtration, et induit des injections en dehors de la filtration (sauf à ajouter un couplage complexe).
- **Répartition stricte sur 24 h** (X mL/j répartis linéairement, rejeté lors de la refonte) — incompatible avec la contrainte « régulation autorisée seulement pendant la filtration », et crée des pauses forcées peu naturelles pour une pompe péristaltique (cycles courts, usure).
- **Volume par cycle de filtration** (rejeté) — trop abstrait pour l'utilisateur, qui raisonne en « combien je consomme par jour ».

## Conséquences

### Positives
- Le réglage parle à l'utilisateur : « mon bidon de 20 L dure 60 jours à 333 mL/j ».
- L'injection respecte **automatiquement** la limite journalière (le quota ≤ la limite, par construction).
- Pas de dépendance à la météo ni au résultat de la mesure : utile quand la sonde ORP est absente ou peu fiable.

### Négatives / dette assumée
- Si la filtration est trop courte un jour donné (ex. panne), le quota n'est **pas rattrapé** le lendemain : la journée manquée est perdue.
- L'utilisateur peut régler un volume trop faible et avoir une eau sous-désinfectée : aucun garde-fou qualitatif, seulement un garde-fou quantitatif (limite max).
- Le mode Programmée ORP est **aveugle au capteur** : il n'empêche pas un surdosage localement si le chlore libre est déjà élevé.

### Ce que ça verrouille
- La saisie UI est en mL, pas en % de la limite ni en cadence. Si un futur produit veut raisonner par tranches horaires, il faudra un autre mode (ne pas modifier celui-ci).

## Références

- Code : [`src/config.h`](../../src/config.h) (`phDailyTargetMl`, `orpDailyTargetMl`, `phRegulationMode`, `orpRegulationMode`)
- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp) branche mode `scheduled`
- Doc UI pH : [page-ph.md](../features/page-ph.md)
- Doc UI ORP : [page-orp.md](../features/page-orp.md)
- Doc régulation : [pump-controller.md](../subsystems/pump-controller.md)
- CHANGELOG [Unreleased] 2026-04-24
