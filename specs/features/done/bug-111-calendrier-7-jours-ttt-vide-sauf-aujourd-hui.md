# bug-111 — Le calendrier 7 jours du temps d'atteinte n'affiche qu'aujourd'hui

## Métadonnées
| | |
|---|---|
| Type | bug (interface) |
| Statut | done |
| Priorité | Moyenne — l'indicateur est inexploitable, aucun effet sur la régulation |
| Version cible | 2.45.1 |
| Signalé par | utilisateur, 2026-09-07 |

## Domaines touchés
- [x] Frontend (`data/`) — `data/app.js`, rendu de la bande 7 jours
- [ ] Régulation — [ ] MQTT — [ ] Web (routes) — [ ] Capteurs — [ ] Filtration — [ ] OTA

## Agents requis
- [x] web-ui-developer · [x] doc-writer
- [ ] pool-chemistry — **sans objet** : aucun chemin de dosage, aucune décision, aucun
  seuil. Le défaut est un rendu qui n'atteint pas les données qu'on lui a passées.

## Symptôme
Sur les écrans pH **et** ORP, la bande « temps d'atteinte de la cible » des 7 derniers
jours n'affiche une durée que pour **aujourd'hui**. Les six autres pastilles montrent
`—` en gris (état « en cours »), alors que l'appareil possède bien les journées
archivées.

## Cause
`GET /get-kpi` sert les entrées sous la forme :

```json
{"d":20260906,"ph":{"s":2,"m":356,"e0":-0.02,"f":0},"orp":{"s":2,"m":120,…}}
```

soit une sous-structure **par régulation**. Or `renderTttDays()` (`data/app.js:4674`)
lit `e.s`, `e.m`, `e.e0`, `e.f` **au premier niveau** de l'entrée, et reçoit `_kpiDays`
brut depuis `renderTttChannel()` (`data/app.js:4846`). La sous-structure n'est jamais
extraite : `e.s` vaut `undefined`, aucune branche de la chaîne
(`s === 2` / `3` / `1`) ne correspond, et le rendu tombe dans le `else` final —
classe `ttt__day--pending`, texte `—`.

**Pourquoi le défaut a survécu à sa revue** : la case d'aujourd'hui est **écrasée**
juste avant la boucle par l'état vivant issu de `/data`, par un choix délibéré et
commenté sur place (`data/app.js:4720-4729`). Cette pastille-là affiche donc la bonne
valeur, ce qui donne à la bande l'apparence de fonctionner. Le seul jour observable
sans historique était aussi le seul jour correct.

## Correctif
Aplatir l'entrée pour la voie rendue, au site d'appel où `prefix` est connu.
Deux invariants à ne pas casser :

1. **`null` doit rester `null`.** `_kpiDays` vaut `null` après un échec de relecture,
   et `renderTttDays()` s'en sert pour écrire « Historique indisponible » puis sortir.
   Le transformer en tableau vide ferait lire « aucune journée enregistrée » — un
   mensonge. Un `.map()` non gardé produirait exactement cette régression.
2. **Une entrée sans la sous-structure de la voie reste un trou.** Un firmware qui
   servirait `ph` sans `orp` doit donner une pastille absente (`·`), pas une pastille
   « en cours ».

## Critères d'acceptation
- [x] Les journées archivées affichent leur durée sur les deux écrans
- [x] Une journée manquante reste un trou **à sa place** (la bande est datée, un
      décalage annoncerait la mauvaise date — cf. commentaire `app.js:4690`)
- [x] Échec de `/get-kpi` → toujours « Historique indisponible », jamais une bande vide
- [x] `node --check data/app.js` et `./build_fs.sh` passent

## Plan de test manuel
1. Ouvrir l'écran pH : les journées présentes dans `/get-kpi` affichent leur durée.
2. Idem sur l'écran ORP, avec des valeurs **différentes** de celles du pH — c'est ce
   qui prouve que la bonne sous-structure est lue, et non un aplatissement au hasard.
3. Couper le réseau et recharger : légende « Historique indisponible ».

## Hors périmètre
- L'écrasement de la case du jour par l'état vivant : comportement voulu, documenté.
- L'absence de test automatique du rendu DOM : le projet n'a pas de harnais JS
  (les 36 suites natives sont du C++ Unity). Validation par lecture et manipulation.

## Notes de clôture (2026-09-07, v2.45.1)
Correctif : helper `tttDaysForChannel(days, prefix)` inséré avant `renderTttChannel()`,
qui passe désormais `tttDaysForChannel(_kpiDays, prefix)`. `renderTttDays()` n'est pas
touchée. Les deux invariants tiennent **par construction** et non par convention :
`if (!Array.isArray(days)) return days;` fait traverser `null` intact, et une voie
absente n'est pas poussée (aucun objet vide fabriqué), donc l'emplacement reste un trou
à sa date.

Simulation Node sur la charge réelle de l'appareil : les deux voies rendent des valeurs
**différentes** (356 min pour le pH contre 120 pour l'ORP au 06/09) — c'est la preuve
que la bonne sous-structure est lue, et non un aplatissement au hasard. Voie absente →
liste vide (trou). `null` → `null`.

**Découverte annexe, et vraie cause amont** : `docs/API.md` décrivait les clés de
`/get-kpi` dans un tableau **plat** alors que son propre exemple JSON était imbriqué.
Un développeur qui lisait le tableau écrivait exactement le code fautif. Corrigé
(colonne « Niveau », lignes `ph`/`orp`, avertissement explicite). En revanche
`docs/features/page-ph.md` et `page-orp.md` étaient **justes** : la doc tenait la
promesse que le code ne tenait pas. Aucun ADR (lecture erronée d'une charge déjà
spécifiée, aucune contrainte nouvelle).

Pas de test automatique : le projet n'a pas de harnais JS. Validation par simulation
Node de la transformation, `node --check`, et `./build_fs.sh` (charge utile 189 711 o
sur 524 288).
