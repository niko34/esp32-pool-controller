# Fix — Pause mélange armée au démarrage au lieu de l'arrêt (régulation pH/ORP auto)

## Type
Bug fix (sécurité/efficacité dosage) — modèle bug.

## Symptôme
Première régulation auto pH+ : log `Démarrage dosage pH (auto): pH=6.99 cible=7.20 erreur=0.213 (cycle 1/20)` immédiatement suivi de `[Dosage pH] Refus : pause mélange en cours`, **jamais** de `Arrêt dosage pH`. Le pH ne remonte pas vers la cible. Chip filtre « Mélange en cours ».

## Cause racine (confirmée par debugger + pool-chemistry)
`notifyPhDose()` / `notifyOrpDose()` arment la pause mélange (`kPhMixingDelayMs`=15min / `kOrpMixingDelayMs`=20min) **au démarrage** de l'injection (pump_controller.cpp ~L781 / ~L930). Or `canDose()` re-garde l'entrée de la branche auto à chaque cycle `update()` (sans throttle, main.cpp:185) et refuse si pause mélange active (garde #5b ~L383). Dès le cycle `update()` suivant, la branche est sautée → `desiredDuty=0` → pompe coupée après ~1 cycle loop (qq ms). `minInjectionTimeMs` (30s) et `shouldContinueDosing` deviennent du code mort sur le chemin auto.

## Gravité
FAIL-SAFE : sous-dosage (régulation inefficace), **aucun risque de surdosage**. Anti-cycling renforcé, pas affaibli.

## Correctif — Piste A
Déplacer (pas dupliquer) `notifyPhDose()` / `notifyOrpDose()` du chemin de **démarrage** vers le chemin d'**arrêt** de l'injection (~L800-801 / ~L948-949, bloc `if (phDosingState.active)` où `lastStopTime` est déjà posé). La pause s'arme une fois la dose réellement versée → réactive `minInjectionTimeMs` + `shouldContinueDosing`.

## Domaines touchés
- `src/pump_controller.cpp` (pH + ORP). Pas d'API/route/WS/MQTT nouvelle.

## Sécurité chimique
Validation `pool-chemistry` sur le diff obligatoire. Invariants à préserver : filtration/eau, watchdog, limites jour/heure, anti-rafale 1min/15min, deadband 0.05/0.01, gel intégrale, fail-closed, calibration. Pause mélange TOUJOURS armée une fois l'injection terminée ; jamais armée si aucune injection n'a démarré.

## Critères d'acceptation
- AC1 : injection auto pH dure ≥ `minInjectionTimeMs` (log `Arrêt dosage pH … durée=≥30s` présent avec volume).
- AC2 : `phMixingDelayActive` passe ON à l'**arrêt**, pas au démarrage ; bloque le cycle suivant 15 min.
- AC3 : ORP idem (20 min).
- AC4 : pas de fausse pause sans injection (aucun `notifyDose` si `shouldDose` n'a jamais démarré).
- AC5 : `pio run` SUCCESS, pas de nouveau warning. Anti-rafale non régressé.

## Statut
doing — analyse faite (debugger + pool-chemistry), implémentation en cours.
