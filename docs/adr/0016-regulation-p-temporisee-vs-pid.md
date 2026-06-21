# ADR-0016 — Régulation proportionnelle temporisée par défaut, pas de PID complet

- **Statut** : Accepté
- **Date** : 2026-06-16
- **Décideurs** : pool-chemistry, architect
- **Spec(s) liée(s)** : [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md)

## Contexte

La feature-025 introduit un lissage logiciel des mesures pH/ORP (médiane + EMA). À partir du moment où la régulation consomme une mesure **filtrée** (et non plus brute) sur un système physiquement **lent** (homogénéisation d'un bassin de plusieurs dizaines de m³ après injection d'une pompe doseuse), l'infrastructure PID héritée du firmware v1 devient inadaptée :

- Le **terme dérivé (`Kd`)** réagit à la pente de l'erreur. Sur une mesure filtrée le bruit résiduel est faible mais non nul ; un `Kd` non nul l'amplifierait, sans aucun bénéfice (le système n'a pas de dynamique rapide à anticiper).
- Le **terme intégral (`Ki`)** accumule l'erreur tant que la cible n'est pas atteinte. Sur un système où l'effet d'une injection ne devient mesurable qu'après plusieurs minutes de mélange, l'intégrale s'emballe (windup) et provoque un **surdosage** avant que la mesure ne reflète l'injection déjà faite.
- Les coefficients `slow/normal/fast` de v1 (Kp 3-12, Ki 0.05-0.2, Kd 4-12) ont été calibrés sur une mesure brute et un dosage continu — hypothèses caduques.

Il faut trancher la stratégie de régulation par défaut **maintenant**, car elle conditionne les gardes `canDose()`, l'anti-windup, la zone morte et la pause mélange introduits dans la même feature.

## Décision

La régulation par défaut est une **régulation proportionnelle pure temporisée** :

- **`Kd = 0` impératif** pour les deux boucles (pH et ORP).
- **`Ki = 0`** : l'intégrale est inerte. L'`integralMax = 50` reste dans le code pour une réactivation terrain éventuelle, mais sans effet.
- **`Kp` seul** module l'amplitude : `Kp = 8` (pH), `Kp = 0.3` (ORP, plus conservateur car l'ORP dépend du pH, de la température et du stabilisant).
- La **temporisation** est assurée hors PID, par trois mécanismes complémentaires :
  - **pause mélange** post-injection (`kPhMixingDelayMs = 15 min`, `kOrpMixingDelayMs = 20 min`) — bloque toute nouvelle décision le temps de l'homogénéisation ;
  - **zone morte** = seuil de démarrage existant (`phStartThreshold = 0.05`, `orpStartThreshold = 15 mV`) — sortie forcée à 0 dans la zone morte ;
  - **anti-windup strict** — l'intégrale (si un jour réactivée) est gelée dès que le filtre n'est pas prêt, que `canDose()` refuse, que la pause mélange est active, que la sortie sature, ou que la mesure est rejetée/instable.

L'infrastructure `PIDController` est **conservée** (struct, `computePID()`) pour permettre une réactivation progressive de `Ki` après validation terrain, sans réécriture.

## Alternatives considérées

- **PID complet retuné** (rejetée) — calibrer de nouveaux jeux `Kp/Ki/Kd` adaptés au système lent. Rejetée : le tuning d'un PID sur un système à grand retard pur est délicat, exige des campagnes de mesure sur bassin réel, et le terme dérivé reste contre-indiqué quelle que soit la valeur. Risque de surdosage pendant la phase de tuning — inacceptable côté sécurité chimique.
- **Tout-ou-rien (bang-bang) avec hystérésis** (rejetée) — simple, mais ne module pas l'amplitude du dosage selon l'écart à la cible : un grand écart et un petit écart déclencheraient le même pulse, gaspillant du produit ou réagissant trop lentement.
- **P temporisée (retenue)** — proportionnelle pour moduler l'amplitude selon l'erreur, temporisée par la pause mélange pour respecter la dynamique hydraulique. Comportement déterministe, prévisible, et sûr par construction (un seul terme, pas d'accumulation cachée). Réactivation de `Ki` possible plus tard sans changement d'architecture.

## Conséquences

### Positives

- **Pas de windup possible** par défaut (`Ki = 0`) → pas de surdosage par accumulation.
- **Pas d'amplification de bruit** (`Kd = 0`).
- Comportement **prévisible et auditable** : la sortie ne dépend que de l'erreur courante et des gardes temporelles.
- La pause mélange aligne la cadence de décision sur la **physique réelle** du bassin.
- Migration douce : l'infra PID reste en place pour réactiver `Ki` après mesures terrain.

### Négatives / dette assumée

- Une régulation P pure laisse une **erreur statique résiduelle** possible (pas de terme intégral pour l'annuler). Acceptable : la zone morte autour de la consigne (±0.05 pH / ±15 mV) absorbe cette erreur, et un dépassement faible et stable est préférable à une oscillation.
- Les coefficients (`Kp`, durées de mélange, zone morte) sont des **valeurs initiales** à valider sur bassin réel ; ils peuvent nécessiter un ajustement.

### Ce que ça verrouille

- `Kd = 0` est une **invariant de sécurité** : toute réintroduction d'un terme dérivé exige un nouvel ADR.
- L'entrée de la régulation est la mesure **filtrée et prête** (`getPhFiltered()` + `isPhFilterReady()`), jamais la brute — verrouillé conjointement par feature-025.
- La pause mélange est une garde `canDose()` **prioritaire** : aucun chemin de dosage automatique ne peut la contourner.

## Références

- Code : [`src/pump_controller.h`](../../src/pump_controller.h) (`PIDController`, défauts `Kp/Ki/Kd`), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`computePID`, `canDose`, `notifyPhDose/notifyOrpDose`), [`src/constants.h`](../../src/constants.h) (`kPhMixingDelayMs`, `kOrpMixingDelayMs`)
- Spec : [`specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md`](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md)
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md), [`docs/subsystems/sensors.md`](../subsystems/sensors.md)
