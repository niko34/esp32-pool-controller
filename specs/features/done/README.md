# Specs terminées — et ce qui, dedans, ne l'est pas tout à fait

Ce dossier archive les demandes livrées. Il porte le **pourquoi** des décisions :
quel seuil a été retenu et sur quelle mesure, quelle alternative a été écartée,
quel incident a motivé quel correctif. Le code dit ce que fait le programme ;
ces fichiers disent pourquoi il le fait ainsi.

## ⚠️ Six specs y sont rangées avec des vérifications on-target non faites

Constaté au ménage du **06/09/2026**. Elles ont été classées « livré » sur la foi
des tests natifs et de la revue, mais **des critères exigeant l'appareil réel sont
restés ouverts**. Ce n'est pas un défaut de code : c'est une dette de
*vérification*, et elle est invisible tant qu'on ne rouvre pas les fichiers.

| Spec | Ce qui n'a pas été vérifié sur l'appareil |
|---|---|
| `bug-echec-ota-indechiffrable-et-noyade-du-journal` | 3 critères en `[~]` : image FS envoyée en type « firmware », préservation du journal antérieur, message d'erreur nommant la cause |
| `bug-famine-8bits-parachute-mort-et-gardes-aveugles` | parachute réarmé et `new_handler` présent en permanence ; non-saturation du journal ; nom de l'appelant dans les WARN |
| `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue` | alerte de filtre non prêt ; bascule automatique en manuel ; **origine de l'excursion à ~4,8 non tranchée** |
| `bug-slope-boot-perdue-et-data-incomplet` | retour à l'état nominal en < 2 min après reboot ; visibilité au journal d'un échec `Slope,?` ; parité `/data` ↔ WS |
| `feature-071-carte-signaux-dispersion-drapeau` | 5 points du plan de test manuel (dispersion affichée, tiret filtration arrêtée, état INSTABLE…) |
| `feature-072-courbe-dispersion` | 5 points du plan de test manuel (oscillation en régime établi, échelle Y, trou sur transitoire…) |

**`feature-073-accumulateur-dosage-sous-plancher` porte même encore le statut
« doing »** dans son propre fichier, alors qu'il est rangé ici — son critère
on-target (« le pH franchit l'ancien point de stagnation ») n'a pas été confirmé.

## Ce que cela ne remet pas en cause

Toutes ces specs ont leur **code livré, testé en natif et revu**. Plusieurs
tournent en production depuis des semaines sans incident imputable. Les laisser
ici est défendable ; **prétendre qu'elles sont intégralement vérifiées ne l'est
pas.**

## À faire

Soit reprendre ces vérifications on-target une par une, soit **acter
explicitement** dans chaque fichier qu'elles ne seront pas faites et pourquoi.
Ce qui n'est pas tenable, c'est de laisser des cases vides sans décision : dans
six mois, personne ne saura si elles ont été oubliées ou volontairement écartées.

*(Le ménage du 06/09 a par ailleurs supprimé 9 doublons résiduels de `doing/` :
des copies antérieures laissées en place lors du classement. Chaque cas a été
vérifié ligne à ligne — aucun contenu n'existait dans `doing/` qui ne fût déjà
dans la version archivée ici.)*
