# ADR-0103 — Les gestes d'entretien quittent la vue courante

- **Statut** : Accepté
- **Date** : 2026-10-12
- **Décideurs** : `architect` (feature-156), proposition initiale de l'utilisateur, confirmée par `ux-ui-designer` (mandat complet)
- **Spec(s) liée(s)** : [`feature-156`](../../specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md)

## Contexte

La page pH portait cinq cartes : débit d'injection, régulation, correction
manuelle de la mesure, historique, calibration. Trois servent à consulter et
piloter ; deux sont des gestes d'**entretien** — rares, délibérés, qu'on ne
fait pas en passant, et qui sont justement les **deux seuls gestes qui
touchent à la justesse de la mesure** (calibration de la sonde, correction
manuelle). Noyés parmi des cartes de pilotage courant, ils ne se lisaient pas
comme un couple cohérent.

L'utilisateur a demandé la même chose pour l'ORP et la Température : les deux
pages ont chacune un bouton « Calibrer la sonde » qui affichait sa carte **au
milieu** des autres cartes de la page (motif déjà en place depuis feature-034,
jamais généralisé jusqu'à cette décision). **Trois usagers confirmés** pour un
même besoin : masquer le reste de la page, afficher le ou les gestes
d'entretien seuls, offrir un chemin de retour.

## Décision

**Un geste d'entretien quitte la vue courante** : au clic sur son point
d'entrée, la page bascule dans un **mode réglage** où tout le reste est
masqué (`hidden`, jamais `style.display` inline — un style inline écraserait
la règle CSS du mode sans effet visible), remplacé par la ou les carte(s)
d'entretien et un lien de retour **en haut et en bas**. Sur la page pH, deux
cartes (calibration puis correction) ; sur l'ORP et la Température, une seule
(calibration).

Le mécanisme est **générique** : une seule classe CSS (`is-flow-maintenance`
posée sur la vue), une seule paire de fonctions paramétrées
(`enterMaintenance(page)` / `exitMaintenance(page)`), une table de
configuration (`MAINTENANCE_PAGES`) qui déclare pour chaque page ses
identifiants (vue, cartes, liens de retour, sous-titre d'entête). **Pas de
sous-route, pas d'implémentation jumelle** : ajouter une quatrième page au
mode coûte quelques lignes de données dans `MAINTENANCE_PAGES`, pas une
nouvelle paire de fonctions `show*CalibrationCard()` / `hide*CalibrationCard()`.

## Alternatives considérées

- **Une implémentation par page** (rejetée — c'était l'état de fait avant ce
  lot : `showPhCalibrationCard()` / `hidePhCalibrationCard()`,
  `showTempCalibrationCard()` / `hideTempCalibrationCard()`, chacune avec son
  propre style inline). Chaque implémentation avait divergé sur un détail (une
  seule avait pensé à annuler l'attente de calibration en vol à la fermeture) —
  exactement le défaut que cette feature a dû corriger après coup pour la
  version générique.
- **Une sous-route dédiée** (`/ph/maintenance`, rejetée) — introduirait un
  état d'URL à synchroniser avec le routeur SPA existant, pour un gain nul :
  le mode réglage est un état d'affichage transitoire, pas une destination
  qu'on partage ou qu'on recharge.
- **Mécanisme générique unique, paramétré par page** (retenue) — un seul
  endroit à faire évoluer pour les trois pages, un seul endroit où corriger un
  défaut (cf. leçon ci-dessous), extensible à une quatrième page sans
  dupliquer de logique.

## Conséquences

### Positives
- Les deux seuls gestes qui touchent à la justesse de la mesure pH sont
  désormais visuellement adjacents (calibration juste au-dessus de la
  correction), ce qui rend le cas « sonde non étalonnée, corrigez d'abord
  l'étalonnage » lisible sans texte supplémentaire.
- Un seul mécanisme à maintenir pour les trois pages ; un correctif
  (accessibilité, scroll, calibration en vol) profite aux trois en un seul
  endroit.
- Le point d'entrée pH (« Calibration et correction de la mesure ») rend la
  correction manuelle **trouvable** depuis un geste qui, avant ce lot,
  annonçait uniquement « Calibrer la sonde » — motif direct de l'incident
  d'origine (une fonction existante devenue introuvable parce que rangée sous
  un intitulé qui ne l'annonçait pas).

### Négatives / dette assumée
- Généraliser un mécanisme à partir de trois implémentations spécifiques a un
  coût de revue réel : deux défauts ont été trouvés **après** la première
  implémentation (voir « Leçon » ci-dessous), tous deux de la même nature —
  un geste que la version spécifique faisait en plus, perdu dans la
  généralisation.
- La page pH porte deux noms différents pour le même mécanisme (« Calibration
  et correction de la mesure » vs « Calibrer la sonde » sur ORP/Température) :
  assumé, le nom suit le **contenu** (deux destinations sur le pH, une seule
  ailleurs), pas une incohérence à corriger.

### Ce que ça verrouille
- Toute future carte « geste d'entretien, rare et délibéré » sur une
  quatrième page (débit de la pompe, diagnostic réseau…) doit passer par
  `MAINTENANCE_PAGES` et les fonctions génériques, pas par une implémentation
  dédiée.
- Un changement de vue (clic menu) doit systématiquement neutraliser tout
  mode réglage resté ouvert (`_exitAllMaintenanceModes()`, appelée depuis
  `showView()`) — un point d'entrée unique pour cette garde, jamais une
  vérification par page.

### Leçon — généraliser un mécanisme en oubliant ce qu'une version spécifique faisait en plus

Ce lot a produit **un défaut et son frère**, tous deux invisibles en relisant
une seule moitié du code :

1. **Quitter une page en mode réglage par le menu** (sans cliquer le lien de
   retour) **laissait la page amputée au retour** : la classe
   `is-flow-maintenance` n'était jamais retirée, donc le débit d'injection, la
   régulation et l'historique restaient masqués.
2. **Son frère**, trouvé en revue juste après le premier correctif : la même
   sortie par le menu laissait **tourner le minuteur d'une calibration en
   vol**, parce que l'ancienne fonction spécifique (`hidePhCalibrationCard()`)
   appelait `_calEndAwait()` pour l'annuler — un geste que la généralisation
   avait **perdu en chemin**, puisque `exitMaintenance()` ne le faisait pas et
   que seul `requestExitMaintenance()` (le lien de retour, pas le menu) y
   pensait.

**Racine commune : généraliser un mécanisme en oubliant ce que la version
spécifique faisait en plus**, pas seulement ce qu'elle affichait. Un troisième
symptôme de la même famille a été trouvé en revue, de nature différente mais
du même registre (« le code a changé sans que le commentaire suive ») :
**quatre commentaires affirmaient encore « pas d'estampillage sur
l'effacement de calibration »** après que le gate `pool-chemistry` eut
renversé cette décision (`ClearPhCal` estampille désormais, lui aussi, cf.
[ADR-0102](0102-ordre-des-gestes-de-justesse-par-compteur.md)) — un
contributeur qui aurait « réconcilié » le code sur la foi de ces commentaires
aurait **annulé un arbitrage de sécurité**.

## Références

- Code : [`data/app.js`](../../data/app.js) (`MAINTENANCE_PAGES`,
  `enterMaintenance()`, `exitMaintenance()`, `_exitAllMaintenanceModes()`,
  `requestExitMaintenance()`), [`data/index.html`](../../data/index.html)
  (sections `#view-ph`, `#view-orp`, `#view-temperature`), [`data/app.css`](../../data/app.css)
  (`.is-flow-maintenance`, `.flow-maintenance`)
- Spec : [`specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md`](../../specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md)
- Doc externe : [docs/features/page-ph.md](../features/page-ph.md), [docs/features/page-orp.md](../features/page-orp.md), [docs/features/page-temperature.md](../features/page-temperature.md)
