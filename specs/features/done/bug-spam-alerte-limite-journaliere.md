# Bug — Alerte « limite journalière atteinte » répétée toutes les minutes

- **Type** : bug simple (format minimal `specs/modele.md`)
- **Statut** : done
- **Version cible** : 2.10.1
- **Créée** : 2026-07-06 (rapport utilisateur on-target, v2.10.0)
- **Domaine** : notification (MQTT/logs) — aucun chemin de dosage touché

## Symptôme observé

Après le latch de la limite journalière pH (fonctionnement correct : crédit plancher → CRIT unique), le log affiche `WARN: Alerte: ph_limit - Limite journalière pH- atteinte` **toutes les minutes**, indéfiniment. De plus le libellé dit « pH- » alors que la régulation est configurée en pH+.

## Cause racine

Dans le health check de `main.cpp` (période `kHealthCheckIntervalMs` = 60 s), `mqttManager.publishAlert("ph_limit", …)` est appelé **sans condition** tant que `safetyLimits.phLimitReached` est vrai (l.560-561) — le flag d'edge `lastPhLimitReached` ne protège que l'alarme écran UART. `publishAlert` émet un événement MQTT non-retained ET un log WARN à chaque appel → spam. Symétrique ORP (l.570-571). Libellé « pH- » codé en dur (l.561) alors que `mqttCfg.phCorrectionType` peut valoir `ph_plus`.

## Correctif

1. Déplacer `publishAlert` sous la condition d'edge `!lastPhLimitReached` (une seule alerte à la transition, comme l'alarme UART). L'état persistant pour HA reste porté par le binary_sensor `ph_limit` retain (inchangé) — l'alerte est un événement de transition, pas un état.
2. Libellé dynamique : `pH+` / `pH-` selon `mqttCfg.phCorrectionType` (même règle que le log CRIT de pump_controller).
3. Symétrique ORP (libellé « chlore » inchangé, toujours exact).
4. La levée de limite reste tracée par le log existant de dé-latch (`checkSafetyLimits`, v2.6.0) + clear UART — pas de nouvel événement.

## Critères d'acceptation

- [ ] Limite atteinte → UNE seule ligne `WARN: Alerte: ph_limit …` (à la transition), plus aucune répétition. *(à vérifier on-target à la prochaine limite atteinte)*
- [ ] Libellé conforme au produit configuré (pH+ dans le cas rapporté). *(à vérifier on-target)*
- [x] Binary_sensor HA `ph_limit` inchangé (retain, état permanent).
- [x] Aucun chemin de dosage touché (main.cpp uniquement).
- [x] `pio run` sans warning, 163 tests natifs verts.

## Notes d'implémentation (v2.10.1)

- **Fichiers modifiés** : `src/main.cpp` (health check : `publishAlert` déplacé sous l'edge `!lastPhLimitReached` / `!lastOrpLimitReached`, libellé pH dynamique selon `mqttCfg.phCorrectionType`), `src/version.h` (2.10.1).
- **Validation** : `pio run` SUCCESS sans warning nouveau, 163/163 tests natifs verts, code-reviewer **Approved**.
- **Mineure notée (hors périmètre)** : le message d'alarme écran UART reste générique (non affecté par le libellé dynamique pH+/pH-).
- **Reste on-target** : à la prochaine limite atteinte, vérifier UNE seule ligne WARN (pas de répétition à 60 s) et le libellé « pH+ » en configuration pH+.
- **Docs** : `docs/MQTT.md` (sémantique de transition `ph_limit`/`orp_limit`), `CHANGELOG.md` ([2.10.1]).
