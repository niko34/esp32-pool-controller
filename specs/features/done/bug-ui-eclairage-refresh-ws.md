# Bug — Champs horaires éclairage non rafraîchis en temps réel (WS config)

- **Type** : bug simple (finition de feature-052)
- **Statut** : done
- **Version cible** : 2.17.1
- **Créée** : 2026-07-06 (rapport utilisateur)
- **Domaine** : UI (data/app.js) — aucun firmware

## Symptôme

Changer les heures / le mode de programmation éclairage depuis Home Assistant : l'écran Éclairage de l'UI web ne met pas à jour ses champs en temps réel (contrairement à l'écran Filtration). Il faut quitter/revenir sur l'écran.

## Cause

`loadConfig()` (déclenché par le message WS `config`) repeuple directement les champs filtration (`#filtration_mode/start/end`, app.js:1619-1629, sous garde `!filtrationDirty`) mais **pas** les champs éclairage. Ces derniers ne sont peuplés que par `loadLightingConfig()` (app.js:6147), appelée à l'ouverture de l'écran, pas sur un message WS. Le firmware applique bien le changement (broadcast WS OK) ; c'est un défaut de rafraîchissement visuel du formulaire.

## Correctif

Miroir exact du mécanisme filtration, dans `data/app.js` :
1. Déclarer `let lightingDirty = false;` (près de `filtrationDirty`, l.1731).
2. Le passer à `true` sur les `change` de `#lighting_schedule_mode`, `#lighting_start_time`, `#lighting_end_time` (ajouter les listeners start/end manquants).
3. Le remettre à `false` après une sauvegarde réussie (bindLightingManualSave).
4. Dans `loadConfig()`, ajouter un bloc (près du bloc filtration) : si `!lightingDirty`, repeupler `#lighting_schedule_mode` (`enabled`/`disabled` selon `cfg.lighting_schedule_enabled`), `#lighting_start_time`, `#lighting_end_time`, et régler l'affichage de `#lighting-schedule-settings`.

## Critères d'acceptation

- [ ] Changer les heures éclairage depuis HA → l'écran Éclairage se met à jour ≤ 5 s sans quitter l'écran (on-target).
- [x] Une saisie éclairage en cours (non sauvegardée) n'est PAS écrasée par un message WS (garde lightingDirty), comme pour la filtration.
- [x] Aucun firmware touché ; `node --check data/app.js` OK, `./build_fs.sh` OK.

## Notes d'implémentation

- **Miroir de `filtrationDirty`** : introduction de `lightingDirty`, listeners `change` sur `#lighting_schedule_mode` / `#lighting_start_time` / `#lighting_end_time` marquant dirty, reset à `false` après sauvegarde réussie. Bloc de repeuplement des 3 champs éclairage ajouté dans `loadConfig()` sous garde `!lightingDirty` (miroir du bloc filtration).
- **Garde anti-écrasement** : une saisie éclairage en cours n'est pas écrasée par un message WS `config` tant que la sauvegarde n'a pas eu lieu.
- **Périmètre** : `data/app.js` seul (+ `version.h` bumpé en 2.17.1 pour cohérence d'affichage). Aucun firmware modifié.
- **Revue** : `code-reviewer` Approved. Réserve mineure cosmétique assumée (déréférencement sans null-check) : calque fidèle du code filtration existant, la SPA conserve les vues dans le DOM → éléments toujours présents, pas de risque.
- **Reste on-target** : valider en conditions réelles que changer les heures éclairage depuis HA met bien à jour l'écran.
