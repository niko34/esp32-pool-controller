# Bug — Libellés du mode filtration trompeurs dans Home Assistant

- **Type** : bug simple (format minimal)
- **Statut** : done
- **Version cible** : 2.15.0
- **Créée** : 2026-07-06 (rapport utilisateur on-target, v2.14.1)
- **Domaine** : MQTT/HA (discovery) — aucun chemin de dosage

## Symptôme

Le select « Mode Filtration » dans HA expose les valeurs brutes `auto`/`manual`/`force`/`off`, alors que l'UI web affiche `Auto`/`Programmation`/`Manuel`/`Désactivé`. Pire qu'un simple écart de langue : `manual` correspond à « Programmation » et `force` à « Manuel » — l'inverse de l'intuition (un utilisateur qui choisit « manual » active en réalité le mode programmé).

## Correctif (option 1A — templates HA, protocole inchangé)

Dans le bloc discovery `select/{id}_filtration_mode` (mqtt_manager.cpp ~l.1322) :
- `options` = libellés français : `Auto`, `Programmation`, `Manuel`, `Désactivé`.
- `value_template` : mappe l'état brut reçu (`auto`/`manual`/`force`/`off`) vers le libellé affiché.
- `command_template` : mappe le libellé sélectionné vers la valeur brute envoyée sur `command_topic`.

Le topic `filtration_mode` (état) continue de publier les valeurs brutes, et le handler `drainCommandQueue` continue de recevoir/valider les valeurs brutes → **contrat MQTT sur le fil strictement inchangé**, aucune automatisation existante cassée. Seul l'affichage HA devient clair. Les autres selects (ph/orp_regulation_mode) restent en valeurs brutes anglaises (cohérence : « automatic/scheduled/manual » ne sont pas trompeurs). Hors périmètre : exposition des heures début/fin (demande séparée).

Rappel sémantique (pour la doc) : Auto = créneau calculé par température (heures en lecture seule) ; Programmation (`manual`) = créneau à heures fixées par l'utilisateur ; Manuel (`force`) = contrôle ON/OFF manuel sans planning ; Désactivé (`off`).

## Critères d'acceptation

- [ ] HA affiche « Auto / Programmation / Manuel / Désactivé » pour le mode filtration (on-target).
- [ ] Sélectionner un libellé dans HA applique le bon mode (command_template → valeur brute) ; l'état HA reflète le mode courant (value_template) (on-target).
- [x] Topics MQTT (état + commande) inchangés sur le fil ; handler drainCommandQueue inchangé.
- [x] `pio run` sans warning (185 tests natifs verts — validation test-engineer).


## Notes d'implémentation

- **Correctif** : `src/mqtt_manager.cpp` (bloc discovery `select/{id}_filtration_mode`, ~l.1329-1345) + `src/version.h` → 2.15.0.
- **Templates HA** : `options` = `Auto`/`Programmation`/`Manuel`/`Désactivé` ; `value_template` mappe l'état brut (`auto`/`manual`/`force`/`off`) → libellé avec `.get(value, value)` (état inattendu passe sans casser) ; `command_template` mappe le libellé → valeur brute sur `.../set`.
- **Wire inchangé** (vérifié) : `filtrationModeState` publie toujours `filtrationCfg.mode` brut (l.462, 573) ; handler `drainCommandQueue` case `FiltrationMode` (l.957-981) valide toujours `auto`/`manual`/`force`/`off` brut. Les templates sont le SEUL point de traduction, côté HA.
- **Anti-fuite** : `publishConfig()` fait `doc.clear()` (l.1262) → les templates ne contaminent pas les blocs `ph`/`orp_regulation_mode` suivants (grep : `value_template`/`command_template` présents uniquement l.1340/1342).
- **Encodage** : fichier confirmé UTF-8 ; « Désactivé » cohérent avec les autres libellés accentués.
- **Verdict revue code-reviewer** : ✅ Approved (aucune issue bloquante). Sécurité chimique non applicable (aucun chemin de dosage touché).
- **Build** : `pio run` SUCCESS, 0 warning.
- **Reste on-target** : les 2 AC d'affichage/commande HA nécessitent une validation sur matériel (broker + HA réels), à cocher après déploiement.
