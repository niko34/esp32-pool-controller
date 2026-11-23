# ADR-0146 — Un plafond de volume plutôt qu'un terme intégral ou une anticipation côté chlore

- **Statut** : Accepté
- **Date** : 2026-11-23
- **Décideurs** : Nicolas Philippe, `pool-chemistry`, `architect`
- **Spec(s) liée(s)** : [feature-203](../../specs/features/done/feature-203-la-dose-de-chlore-se-cale-sur-le-rendement-observe.md) (prérequis : [ADR-0145](0145-la-pause-melange-orp-s-apprend-seulement-vers-le-prudent.md))

## Contexte

La commande chlore est purement proportionnelle (« loin donc fort ») ; rien ne relie un **volume** injecté à un **effet** mesuré. Le 18/11, 456 mL ont été injectés pour un manque de 51 mV alors qu'environ 220 mL suffisaient (le rendement observé était de 0,23 mV/mL). L'injection s'est arrêtée sur le plafond horaire, pas sur la cible. Il faut que l'appareil apprenne ce que son chlore rapporte sur **son** bassin, sans créer un second régulateur sur une voie dont la chimie est sensible.

## Décision

L'appareil mesure le **rendement** (mV par mL) et en tire un **plafond de volume par épisode** : `budget = marge × manque / rendement`. Le budget **ne peut qu'arrêter une injection plus tôt**, jamais la prolonger.

1. **Plafond, pas régulateur.** Un régulateur (terme intégral, anticipation) pourrait **augmenter** la dose. `computePID()` reste intouché, `orpPID.ki` reste à 0. Aucun nouveau chemin vers la pompe.
2. **Couplage à feature-202.** L'épisode se ferme sur le **verdict** de l'observation de 202 (`kClosed`, plateau = `refOrp`). `isOrpMixingDelayLearned()` n'est **jamais** consulté, et il n'y a **pas** de seconde détection de fin de montée : la mécanique de 202 avance par jalons constants jusqu'à 6×, elle voit donc le plateau réel dès le premier épisode. Un détecteur unique vaut mieux que deux détecteurs à synchroniser.
3. **Marge de 2** (`kOrpYieldBudgetMargin`). Le rendement appris est une moyenne sur toute la montée ; un petit manque signifie qu'on part près de la consigne, où les millivolts coûtent le plus cher (rendement ∝ 1/C). Sans marge, budget trop petit près de la cible.
4. **Domaine d'application sans nombre.** Le budget n'est appliqué que si `manque ≤ médiane(manques observés)` : on n'extrapole jamais hors du domaine mesuré. Cela a remplacé un seuil chiffré.
5. **Cycle de vie.** L'épisode vit exactement aussi longtemps que l'observation qui le porte : il ne s'ouvre que sur une séance **automatique**, ne se ré-ouvre pas en cours de séance, et est jeté avec l'observation pour toutes ses causes.
6. **Budget recalculé à chaque tick, jamais figé.** Un budget figé sur un épisode qui ne se ferme pas aurait bloqué la chloration automatique en permanence : le pire scénario sanitaire du lot.
7. **Alerte ET désarmement.** « Ne peut que réduire » est sûr pour un épisode, pas pour une suite infinie. Après 3 arrêts d'affilée sur le budget sans atteindre la consigne, `orp_yield_undersupply` s'affiche **et** la condition « budget » seule cesse de s'appliquer ; elle se réarme sur un épisode clos ≥ consigne (ou une remise à zéro de l'apprentissage). L'alerte seule laisserait tourner l'état fautif (travers de bug-200).
8. **Biais de comptage.** Le même compteur sert de dénominateur d'apprentissage et de volume comparé au budget : un biais de +6 % donne un budget 1,06×, du côté généreux, donc prudent.

## Alternatives considérées

- **Terme intégral côté chlore** (rejetée) — peut augmenter la dose ; windup, nouveau chemin vers la pompe, validation chimique lourde.
- **Anticipation (feedforward, comme le pH, feature-085)** (rejetée) — idem : pilote la dose vers le haut, et la relation chlore/mV est logarithmique.
- **Seconde détection de fin de montée propre à 203** (rejetée) — deux détecteurs à garder synchronisés ; divergence possible.
- **Fermer l'épisode sur la convergence de 202** (rejetée) — retarderait l'apprentissage de plusieurs épisodes sans gain.
- **Seuil chiffré de manque maximal** (rejetée) — nombre calé sur un bassin ; remplacé par la médiane des manques observés.
- **Plafond de volume appris** (retenue) — seul mécanisme qui ne peut que réduire.

## Conséquences

### Positives
- Dose plus juste, sans nouveau chemin vers la pompe ; sans apprentissage, comportement strictement celui d'avant (état sûr).

### Négatives / dette assumée
- L'appareil reste **lent à combler un grand manque** (relation logarithmique), voulu.
- Le chlore injecté **à la main n'est ni compté ni appris** ; l'apprentissage peut ne jamais converger sur un bassin qui réinjecte souvent, avec repli sur le comportement d'avant.
- L'empreinte de contexte ne couvre **ni la concentration du produit, ni la chimie de l'eau** (stabilisant, température, charge de baigneurs). L'empreinte ne porte que `{schéma, pompe, consigne}` ; le bouton **« nouveau bidon »** (`requestOrpYieldBottleReset()`) est le **rattrapage explicite** : seul signal de changement de produit dont l'appareil dispose, il remet tout l'apprentissage à zéro, même si le produit est le même (mieux vaut réapprendre pour rien que sous-doser). Le **désarmement**, borné à 3 épisodes, reste le filet pour tout le reste (stabilisant, température, baigneurs). Choix assumé, pas un oubli.
- **Nuance sur l'écrêtage** : `kOrpYieldMaxMvPerMl` ≈ 52,7 mV/mL, soit environ 226 fois le 0,23 mV/mL observé. L'écrêtage de plausibilité est un garde-fou structurel **quasi inerte en pratique** ; le vrai remède au sous-dosage est **le désarmement**. Croire l'écrêtage protecteur, c'est se tromper de mécanisme.

### Ce que ça verrouille
- Toute évolution du dosage chlore reste du côté « arrêter plus tôt » ; une anticipation à la hausse exigerait un nouvel ADR.
- La fermeture de l'épisode dépend de l'observation de 202 : modifier celle-ci impacte 203.

## Références

- Code : `src/orp_yield_learn_logic.h`, `src/pump_controller.{h,cpp}` (`tickOrpLearn`), `src/constants.h` (`kOrpYield*`)
- Tests : `test/test_native_orp_yield_learn/`
- Doc : [pump-controller](../subsystems/pump-controller.md), [page ORP](../features/page-orp.md)
