# ADR-0110 — Régler une consigne est une commande différée : éligible à la carte, jamais en enregistrement implicite

- **Statut** : Accepté — complète [ADR-0108](0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md)
- **Date** : 2026-10-14
- **Décideurs** : utilisateur (décision produit), gate `pool-chemistry` (conditions C1-C12 et deux
  précisions en contre-vérification), `ux-ui-designer`, `architect`, `web-ui-developer`,
  `code-reviewer`
- **Spec(s) liée(s)** : [`feature-164`](../../specs/features/done/feature-164-regler-les-valeurs-depuis-les-cartes.md)

## Contexte

[ADR-0108](0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md) distingue deux
catégories de gestes sur les cartes du tableau de bord : **basculer un mode** (réversible, en
permanence visible — éligible à la carte, protégé par la seule visibilité) et **injecter** (action
de dosage immédiate et discrète — jamais sur l'accueil).

`feature-164` introduit un troisième geste : **régler une consigne** (pH, ORP) ou un **horaire**
depuis la carte. Ce geste ne rentre dans aucune des deux catégories existantes : ce n'est pas une
action immédiate comme l'injection, mais ce n'est pas non plus un simple basculement d'état comme
le mode — **modifier la consigne ORP commande une chloration future**, et la régulation en tiendra
compte au prochain cycle. Deux questions, à alternative crédible, devaient être tranchées avant
l'implémentation.

### (a) Les bornes doivent-elles être plus serrées depuis l'accueil que depuis la page de réglage ?

Une garde resserrée sur la carte (par rapport à la page de réglage) aurait pu sembler prudente :
un geste plus rapide mériterait une marge plus étroite. Mais le principe déjà posé par ce projet
(« les plafonds protègent, le reste informe ») met en garde contre l'excès inverse : une garde qui
bloque plus qu'elle ne protège est un défaut.

### (b) Le geste doit-il être validé par une confirmation modale, ou par un bouton explicite ?

Les gestes de mode (ADR-0108) n'ont **aucune** confirmation — la visibilité permanente suffit.
Les gestes d'injection sont **confirmés par nature** (un clic sur un bouton d'action dédié, hors de
l'accueil). Un réglage de consigne n'est ni l'un ni l'autre : fallait-il une modale de confirmation
à chaque changement, ou suffisait-il d'un geste de validation explicite (bouton Sauvegarder) ?

## Décision

1. **Les bornes sont identiques partout** — carte d'accueil, page de réglage, commande MQTT.
   Aucune fenêtre resserrée spécifique à l'accueil. (Conséquence concrète : la fenêtre ORP, plus
   stricte côté MQTT qu'côté HTTP avant ce lot, est au contraire **élargie** aux mêmes bornes des
   deux côtés — voir « Ce que ça verrouille ».)
2. **Aucune confirmation modale sur un changement de consigne** — un **bouton Sauvegarder
   explicite**, jamais sur la même ligne qu'un champ, matérialise le geste de validation. ⛔
   **L'enregistrement n'est jamais implicite** (pas d'autosave au changement de valeur) : c'est le
   bouton, pas une modale, qui porte la friction minimale nécessaire.
3. **Seule exception à l'absence de confirmation** : une transition **rare et dangereuse**
   (abaisser la consigne ORP pendant une chloration de secours active) reste protégée par la
   confirmation **déjà posée** par ADR-0108 pour le mode — mutualisée, pas dupliquée.
4. **Le verrou d'étalonnage porte sur `calActive` seul**, jamais sur l'escalade de sécurité pH :
   une escalade force déjà le mode en Arrêt, verrouiller aussi la consigne n'y protégerait rien et
   ôterait à l'utilisateur le moyen de déplacer la cible pour en sortir.

## Alternatives considérées

### (a) Bornes

- **Option A (rejetée)** — bornes **resserrées** sur l'accueil (ex. ORP [450 ; 850] au lieu de
  [300 ; 950]), au nom d'une prudence accrue pour un geste « plus rapide ». Rejetée : la rapidité
  d'un geste n'est pas une mesure de risque chimique — le risque est le même quelle que soit la
  page d'où part la requête HTTP, et une fenêtre resserrée aurait **interdit la recalibration
  légitime** (ex. après un changement de stabilisant) depuis l'accueil sans raison chimique propre.
- **Option B (retenue)** — bornes **identiques partout**, dérivées des mêmes constantes firmware
  (`kOrpTargetPlausibleMin/Max`, `kPhTargetMin/Max`), servies au client par `/get-config`
  (ADR-0100). Corollaire : la fenêtre MQTT ORP, plus stricte qu'HTTP avant ce lot sans
  justification propre, est **élargie** pour converger — pas resserrée pour « protéger » l'accueil.

### (b) Confirmation vs bouton explicite

- **Option A (rejetée)** — une confirmation modale systématique à chaque changement de consigne.
  Rejetée : la leçon de feature-159 (une friction qui se répète s'apprend à cliquer sans lire)
  s'applique directement — un réglage de consigne est un geste **fréquent**, le rendre
  systématiquement confirmé l'aurait vidé de son sens protecteur en quelques usages.
- **Option B (rejetée)** — enregistrement **implicite** (autosave au changement de valeur), comme
  pour le mode. Rejetée : `bindAutosave()` exclut déjà explicitement les champs numériques et
  horaires dans tout le projet (« pas d'auto-save, bouton Sauvegarder uniquement ») — y déroger
  pour ce geste aurait été un écart délibéré, non assumé comme tel, et aurait cassé la cohérence
  déjà posée entre tous les champs de réglage du projet.
- **Option C (retenue)** — **bouton Sauvegarder explicite**, par bloc, jamais sur la même ligne
  qu'un champ (règle née du défaut de la page Produits, 2026-10-09). La friction nécessaire existe
  déjà (un clic distinct du changement de valeur) sans ajouter de modale.

## Conséquences

### Positives
- Un principe clair et réutilisable pour tout futur geste de réglage sur une carte : bornes
  identiques partout, validation par bouton explicite, confirmation réservée aux transitions
  rares et dangereuses (mutualisée, jamais dupliquée par surface).
- La fenêtre ORP élargie corrige au passage une incohérence héritée (MQTT plus strict qu'HTTP
  sans raison chimique propre).

### Négatives / dette assumée
- Chaque nouveau champ de réglage en carte (feature-164 ouvre la voie à feature-164bis/
  feature-166... pour d'autres cartes) doit répéter ce même triptyque (bornes servies, bouton
  explicite, verrou `calActive` le cas échéant) — pas de composant générique unique, une
  composition documentée (`DESIGN_SYSTEM.md` §5.20).
- Un étalonnage verrouille les deux consignes (pH et ORP) même si une seule sonde est concernée —
  accepté sans spec de suivi, mais c'est une limite connue à ne pas oublier si le modèle d'état
  d'étalonnage évolue vers une distinction par canal.

### Ce que ça verrouille
- Toute future consigne ou valeur réglable depuis une carte d'accueil suit le même triptyque :
  **bornes identiques à toutes les surfaces d'écriture** (HTTP, MQTT, carte, page de réglage),
  **bouton Sauvegarder explicite** (jamais d'autosave sur un champ numérique ou horaire), et
  **confirmation réservée aux transitions rares et dangereuses**, mutualisée entre toutes les
  surfaces qui partagent le même chemin d'écriture (pas une copie par surface — ADR-0108).
- Le verrou d'étalonnage d'une consigne suit toujours `calActive` seul, jamais l'escalade de
  sécurité propre à une voie — sauf nouvelle instruction explicite du gate `pool-chemistry`.

## Références

- Code : [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (validation ORP/horaires,
  verrou étendu) · [`src/pump_controller.{h,cpp}`](../../src/pump_controller.h) (purge
  d'intégrale, `requestPhTargetChange()`) · [`src/constants.h`](../../src/constants.h)
  (`kOrpTargetPlausibleMin/Max`, `kOrpTargetStep`) · [`data/app.js`](../../data/app.js)
  (`buildPhTargetSegments()` généralisée, `bindDashboardPhTargetSave()`,
  `bindDashboardOrpTargetSave()`, `confirmOrpRescueInterruption()` étendue)
- Spec : [`specs/features/done/feature-164-regler-les-valeurs-depuis-les-cartes.md`](../../specs/features/done/feature-164-regler-les-valeurs-depuis-les-cartes.md)
- Doc externe : [`docs/features/page-dashboard.md`](../features/page-dashboard.md#régler-la-consigne-depuis-la-carte-feature-164-v2800) ·
  [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#verrou-étendu-aux-consignes-feature-164-gate-c5c6) ·
  [`docs/DESIGN_SYSTEM.md`](../DESIGN_SYSTEM.md#520-modifier-une-valeur-dans-une-carte--une-composition-pas-une-classe-nouvelle)
