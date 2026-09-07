# Specs terminées — et ce qui, dedans, ne l'est pas tout à fait

Ce dossier archive les demandes livrées. Il porte le **pourquoi** des décisions :
quel seuil a été retenu et sur quelle mesure, quelle alternative a été écartée,
quel incident a motivé quel correctif. Le code dit ce que fait le programme ;
ces fichiers disent pourquoi il le fait ainsi.

## ⚠️ Sept specs y sont rangées avec des vérifications on-target non faites

Constaté au ménage du **06/09/2026** (six entrées), plus `bug-101` archivée le
**07/09** avec sa recette on-target explicitement non jouée. Elles ont été classées « livré » sur la foi
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
| `bug-101-import-historique-plafonne-a-16-ko` (07/09) | **coupure Wi-Fi au 3ᵉ lot sur 6** → historique partiel et cohérent, reprise sans doublon ; relevé `psram_free` / `max_alloc_8bit` avant/après (invariant : l'interne ne bouge pas de plus de **9 492 o**, la PSRAM absorbe les tampons). Étape **humaine** par nature — c'est la mesure qui confirmera le désassemblage de l'allocateur sur l'appareil |

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
