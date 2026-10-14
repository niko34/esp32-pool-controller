# ADR-0108 — Basculer le mode de régulation est une commande d'accueil ; injecter manuellement reste un geste de réglage

- **Statut** : Accepté
- **Date** : 2026-10-13
- **Décideurs** : utilisateur (décision produit), gate `pool-chemistry` (GO sous 10 conditions puis
  arbitrages après revue), `ux-ui-designer`, `architect`, `web-ui-developer`, `code-reviewer`
- **Spec(s) liée(s)** : [`feature-161`](../../specs/features/done/feature-161-choisir-le-mode-depuis-les-cartes.md)

## Contexte

`feature-161` ajoute, sur les cartes pH et ORP du tableau de bord, un sélecteur qui démarre ou
arrête la régulation automatique **en un clic**, sans quitter l'accueil. C'est un changement de
nature : jusque-là, changer le mode de dosage exigeait d'ouvrir la page de réglage dédiée.

Le gate `pool-chemistry` a dû trancher **deux questions distinctes**, souvent confondues dans la
discussion initiale : (1) le **changement de mode** (démarrer/arrêter la régulation automatique)
doit-il être accessible depuis l'accueil, et avec quelle friction ? (2) **l'injection manuelle**
(un geste de dosage immédiat et discret) doit-elle suivre la même règle, maintenant qu'elle n'est
plus conditionnée à un mode « Manuelle » qui la cachait de fait ?

Le gate a répondu différemment aux deux : le mode se change depuis l'accueil, **sans**
confirmation systématique (C5) ; l'injection manuelle reste **exclue** de l'accueil, même dans un
monde où elle est désormais possible dans les trois modes (C8 : « un geste qui dose ne doit pas
être à un clic de l'écran d'accueil »). Une revue a ensuite étendu la seule confirmation du lot
(interrompre un secours en cours, C4) à **toutes** les surfaces qui partagent le même chemin
d'écriture, pas seulement celle d'où le gate l'avait d'abord exigée.

## Décision

1. **Un contrôle qui bascule un état réversible et en permanence visible** (le mode de régulation :
   Auto / Programme / Arrêt) **peut** vivre sur une carte du tableau de bord. Sa protection n'est
   **pas** une confirmation de clic mais la **visibilité permanente** du mode dans l'en-tête de la
   carte, redondante avec l'état du sélecteur — l'utilisateur voit toujours ce qui tourne, avant
   même de regarder le sélecteur.
2. **Un contrôle qui déclenche une action de dosage discrète et immédiate** (injection manuelle)
   **ne vit jamais sur une carte du tableau de bord**, quel que soit le mode actif. Il reste
   confiné à la page de réglage dédiée, qui demande une navigation explicite.
3. **Une confirmation de clic est réservée aux transitions rares et dangereuses** (interrompre une
   chloration de secours en cours), et s'attache à **l'action**, pas à la **surface** d'où part le
   clic : si deux surfaces partagent le même chemin d'écriture, elles partagent la **même**
   protection — jamais une friction sur l'une et pas sur l'autre.

## Alternatives considérées

- **Option A** (rejetée) — poser le sélecteur de mode **et** l'injection manuelle sur la carte du
  tableau de bord, comme le proposait la demande initiale et les maquettes fournies. Rejetée par le
  gate (C8) : un geste qui dose ne doit pas être à un clic de l'écran d'accueil, même si les gardes
  de sécurité (watchdog, présence d'eau, limites) continuent de s'appliquer. La proximité physique
  du geste avec l'écran le plus consulté est elle-même un facteur de risque, indépendant des gardes
  logicielles.
- **Option B** (rejetée) — exiger une confirmation modale sur **toute** transition de mode (Auto↔
  Arrêt, Auto↔Programme…), par symétrie avec les contrôles de dosage. Rejetée (C5) : une friction
  **uniforme** sur un geste fréquent s'apprend à cliquer sans lire — leçon tirée de feature-159.
  La visibilité permanente du mode protège déjà contre l'arrêt accidentel, sans coût d'usage.
- **Option C** (retenue) — distinguer **la nature du contrôle** (bascule d'état réversible vs.
  action de dosage immédiate) plutôt que la fréquence ou la gravité perçue, et attacher la
  confirmation à l'**action** dangereuse identifiée (interruption d'un secours), sur **toutes**
  les surfaces qui y mènent.

## Conséquences

### Positives
- Les cartes du tableau de bord gagnent un contrôle utile (changer de mode sans naviguer) sans
  exposer le geste le plus sensible (injecter).
- La protection contre une interruption accidentelle de secours ne dépend plus de la surface
  d'où part le clic — fermant une classe de contournement (« la garde existe sur une page, pas sur
  l'autre »).
- Le principe « les plafonds protègent, le reste informe » (mémoire du projet) trouve ici une
  formulation UI reproductible pour de futures cartes.

### Négatives / dette assumée
- La distinction « bascule d'état » vs « action de dosage » doit être réexpliquée à chaque nouvelle
  carte candidate à un contrôle direct (ex. une future carte Éclairage) — ce n'est pas un réflexe
  acquis, c'est une classification à refaire consciemment.
- « Programme » désigne deux valeurs internes différentes selon la carte (`manual` en Filtration,
  `scheduled` en pH/ORP) — assumé par écrit, invisible à l'écran, mais une confusion possible pour
  quiconque lit un journal ou une trame brute sans ce contexte.

### Ce que ça verrouille
- Toute future carte de tableau de bord qui expose un contrôle de régulation doit être classée
  explicitement avant implémentation : **bascule d'état réversible** (éligible à la carte,
  protégée par visibilité permanente) ou **action de dosage discrète** (jamais sur la carte,
  confinée à la page de réglage).
- Une confirmation de clic protège désormais **l'action**, jamais la **surface** : tout futur
  chemin d'écriture partagé entre plusieurs surfaces doit porter sa protection au même niveau sur
  toutes, via une fonction mutualisée — pas une copie par surface.
- L'exclusion de la carte Éclairage de ce lot (`feature-162`) suit directement cette doctrine :
  poser un sélecteur sur un affichage qui ment déjà aurait transformé un défaut d'affichage en
  commande fausse, avant même de se poser la question de la classification ci-dessus.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h) / [`.cpp`](../../src/dosing_logic.cpp)
  (garde rang 4 `AutomaticInjectInProgress`, C1) ·
  [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) ·
  [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (verrou étalonnage, C2) ·
  [`data/app.js`](../../data/app.js) (`applyRegulationMode()`, `confirmOrpRescueInterruption()`,
  `resolveSelectorMode()`)
- Spec : [`specs/features/done/feature-161-choisir-le-mode-depuis-les-cartes.md`](../../specs/features/done/feature-161-choisir-le-mode-depuis-les-cartes.md)
- Doc externe : [`docs/features/page-dashboard.md`](../features/page-dashboard.md#sélecteur-de-mode-depuis-les-cartes-ph--orp--filtration-feature-161-v2770) ·
  [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#garde-rang-4--injection-automatique-en-cours-bug-161-gate-c1)
