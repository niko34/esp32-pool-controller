# ADR-0141 — L'appareil est témoin du froid, jamais agent

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : utilisateur, `pool-chemistry` (NO GO sur l'option 2), `architect`, `ux-ui-designer`
- **Spec(s) liée(s)** : [bug-197](../../specs/features/done/bug-197-le-libelle-hors-gel-annonce-une-protection-qui-n-existe-pas.md), [feature-195](../../specs/features/done/feature-195-langage-visuel-du-tableau-de-bord.md)

## Contexte

La carte Filtration affichait une pastille de froid calculée dans le navigateur (`temperature < 5.0` et pompe en marche). Elle laissait croire que l'appareil protège l'installation du gel. Il ne le fait pas : le firmware se contente de journaliser « Hivernage » sous `kFiltrationRefTempMinC` (4,0 °C) et de conserver le planning. Le libellé disparaissait quand la pompe s'arrêtait, c'est-à-dire quand le risque est maximal, et le guide client parlait d'une « pause anti-gel » qui n'a jamais existé. L'appareil est vendu : un propriétaire rassuré à tort ne fait rien, et les canalisations éclatent.

Deux options : (1) dire la vérité, (2) faire ce que le libellé promettait (démarrer la pompe sous un seuil). Les gates ne gardant aucune mémoire entre deux passages, la question reviendrait au premier hiver sans trace écrite.

## Décision

**L'appareil est témoin du froid, jamais agent.** Il relève et affiche une température ; il ne commande rien pour cause de gel et n'affirme aucune protection.

- Aucune logique de commande liée au froid (option 2 close).
- Le froid s'affiche dans une **bande** (carte Filtration, page Filtration, segment sur la carte Température), pas dans la pastille d'état. Texte : « Eau à X °C (sous 4 °C). » / « Eau des tuyaux à X °C (sous 4 °C). » + « Le gel reste à votre charge. ».
- **Seuil unique 4,0 °C** (`TEMP_WINTER_MIN_C` côté navigateur = `kFiltrationRefTempMinC`), à la place du 5,0 écrit en dur.
- Bande **absente** si valeur nulle/`NaN`, sondes non identifiées (la sonde pourrait être le coffret), hors ligne, ou sonde figée pompe en marche. Jamais conditionnée à `measuresOk`.
- Vocabulaire interdit partout (interface, journal, MQTT, guides) : tout mot qui fait de l'appareil un agent du gel.

## Alternatives considérées

- **Option 2 — commander la pompe sous un seuil** (rejetée, NO GO franc, trois raisons indépendantes) :
  1. **Mauvaise grandeur** : le gel casse les tuyaux quand l'*air* passe sous 0 °C ; l'appareil ne mesure que l'eau, qui perd ~0,5 °C par nuit pour 30-50 m³. Air à −6 °C avec eau à 6 °C ne déclencherait rien. Une sonde d'air est interdite (rien à installer chez le client).
  2. **Marche à sec** : condition de déclenchement et de destruction sont la même (eau froide = saison d'hivernage, circuit vidangé). La garniture de pompe est lubrifiée par l'eau et meurt en quelques minutes ; l'appareil n'a ni débitmètre ni pressostat et ne voit pas une vanne fermée (incident du 23/08/2026).
  3. **Réouverture du dosage à 2 °C** : en mode `Managed`, la présence d'eau est l'état commandé de la filtration ; une marche forcée ouvrirait la garde de présence d'eau sans autre garde de température (`dosing_logic.h` n'en porte aucune). Surdosage par construction sous ~10 °C, et risque de produits non dilués dans un tronçon mort.
- **Option 2 « activable en configuration »** (rejetée) : une configuration chez le client est interdite par la contrainte absolue du projet.
- **Servir le seuil par l'API** (rejetée pour ce lot) : ajouterait un champ à `/data` et au WebSocket (quatre sites de parité) pour un chiffre qui ne change pas. Le seuil est dupliqué côté navigateur ; le garde-fou est le commentaire croisé, pas la CI.
- **Garder la pastille de froid, la reformuler** (rejetée) : elle *remplaçait* l'état de la pompe au lieu de s'y ajouter.

## Conséquences

### Positives
- Plus aucune promesse de protection ; le cas « eau froide et pompe arrêtée » parle enfin.
- Un seul seuil, un seul calcul partagé (`coldWaterState()`), plus de priorité de teinte à arbitrer.

### Négatives / dette assumée
- Le seuil 4 existe en deux endroits (`constants.h`, `app.js`).
- Pompe arrêtée, la sonde lit un tronçon d'aspiration stagnant : la bande le dit (« Eau des tuyaux »).

### Ce que ça verrouille
- Toute demande de « protection antigel » par pilotage de la pompe doit **d'abord superseder cet ADR**, avec réponse aux trois raisons ci-dessus.
- Le guide client doit continuer à dire que le gel est à la charge du propriétaire.

## Références

- Code : `data/app.js` (`coldWaterState`, `renderColdWaterNotice`, `isWaterRoleUnknown`, `TEMP_WINTER_MIN_C`), `src/constants.h` (`kFiltrationRefTempMinC`), `src/filtration.cpp`
- Spec : [bug-197](../../specs/features/done/bug-197-le-libelle-hors-gel-annonce-une-protection-qui-n-existe-pas.md)
- Doc : [`docs/features/page-dashboard.md`](../features/page-dashboard.md), [`docs/DESIGN_SYSTEM.md`](../DESIGN_SYSTEM.md) §5.9, [`docs/subsystems/filtration.md`](../subsystems/filtration.md)
