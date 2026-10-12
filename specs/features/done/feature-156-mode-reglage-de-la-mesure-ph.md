## Métadonnées
- **ID** : feature-156
- **Slug** : mode-reglage-de-la-mesure-ph
- **Type** : feature
- **Statut** : **livré et clôturé** — implémentation, tests et documentation livrés en v2.72.0.
  Les deux critères qui exigeaient un jugement humain ont été rendus par l'utilisateur le
  2026-10-12, **après** le déploiement de la v2.73.0 : le libellé du point d'entrée est trouvé
  par qui cherche à corriger sa mesure, et le rendu à 390 px est conforme sur appareil réel.
  ⚠️ Ces deux cases sont restées ouvertes 1 jour **volontairement** : aucun agent ne pouvait les
  cocher — c'est l'utilisateur qui avait signalé le malentendu d'origine, et le CSS ne prouve
  pas un rendu.
- **Priorité** : normale
- **Version cible** : 2.72.0
- **Dernière mise à jour** : 2026-10-12
- **Spec(s) liée(s)** : feature-155 (correction manuelle de la mesure — la carte déplacée),
  feature-154 (pastilles de santé de la sonde), feature-034 (bouton « Calibrer la sonde » et
  carte de calibration déjà masquée par défaut)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — ⚠️ **OUI, pour UN point précis** : la spec ajoute le mot
      « **Recommandé** » au titre de la correction. Or cette correction **engage le dosage**
      (feature-155) et une valeur mal indiquée **déplace le pH réel du bassin**. Recommander un
      geste qui peut nuire demande un arbitrage chimique, pas un choix de rédaction. Aucune
      ligne de dosage n'est modifiée par ailleurs.
- [x] **ux-ui-designer — MANDAT COMPLET** : c'est **le** lot d'ergonomie. Il arrête le nom du
      point d'entrée, celui des liens de retour, la disposition du mode, et **tout autre choix
      visuel que ce lot impose, même non demandé**.
- [x] **embedded-developer — REQUIS** (décision utilisateur du 2026-10-12) : deux horodatages à poser et à persister. L'estimation initiale « aucun firmware » est **périmée**.
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Sortir de la page pH les **deux gestes d'entretien** — étalonnage de la sonde et correction
manuelle de la mesure — pour les réunir dans un **écran dédié**, atteint depuis un point
d'entrée unique et quitté par un lien de retour placé **en haut et en bas**. La page pH
courante n'affiche plus ni l'un ni l'autre.

## Contexte

**Proposition de l'utilisateur, 2026-10-12** :

> « Dans l'écran de pH, je propose que quand on clique sur Calibrer la sonde, on masque toutes
> les informations de l'écran. On affiche le cadre de Calibration et dessous on affiche le cadre
> de correction de la mesure. En haut et en bas on aurait un lien de retour. Le nom de ce lien
> est à déterminer par l'ux. Sur l'écran de pH normal, on ne verrait ni le cadre de calibration
> ni le cadre de correction. »

### Pourquoi c'est juste

La page pH porte aujourd'hui **cinq cartes** : débit d'injection, régulation, correction
manuelle de la mesure, historique, calibration. **Trois servent à consulter et piloter** ; **deux
sont des gestes d'entretien** — rares, délibérés, qu'on ne fait pas en passant.

Et les réunir a un sens plus fort qu'un rangement : **ce sont les deux seuls gestes qui touchent
à la JUSTESSE de la mesure.** Côte à côte dans un espace dédié, ils deviennent **comparables**,
au lieu de se confondre en étant noyés parmi des cartes sans rapport. C'est le meilleur remède
au malentendu que ce projet combat depuis l'incident du 2026-08-25 (une sonde **saine** déclarée
à remplacer, cause réelle = décalage **externe**).

### Ce n'est pas une invention : le motif existe déjà

La carte de calibration est **déjà masquée par défaut** et s'affiche au clic sur « Calibrer la
sonde » (`data/index.html:679`, feature-034). Ce lot **étend** ce motif : masquer **le reste**,
et y **joindre** la correction.

## Décisions utilisateur — 2026-10-12

1. ⚠️ **PRÉCISION DE L'UTILISATEUR, 2026-10-12 — J'AVAIS COMPRIS TROP LARGE.** Une version
   antérieure de cette spec écrivait que « l'asymétrie avec l'ORP est assumée » et plaçait **la
   page ORP entière hors périmètre**. **C'EST FAUX.** L'utilisateur :
   > « Tu me confirmes que l'ORP est traité comme le pH pour la calibration ? Quand j'ai dit
   > qu'on gardait l'asymétrie, c'était uniquement pour indiquer qu'il n'y avait pas de
   > correction pour le capteur. »
   ⇒ **L'ORP REÇOIT LE MÊME MODE DÉDIÉ** : son écran de calibration quitte lui aussi la vue
   courante, avec liens de retour en haut et en bas.
   ⇒ **L'asymétrie porte UNIQUEMENT sur l'absence de carte de correction** côté ORP — le module
   EZO ORP se calibre en 1 point et n'expose ni pente ni zéro.
   ⇒ **Conséquence heureuse sur les NOMS** : l'asymétrie devient **cohérente**, pas subie. Sur
   le pH le point d'entrée annonce **deux** destinations (« Calibration et correction de la
   mesure ») parce qu'il y en a deux ; sur l'ORP il peut rester « **Calibrer la sonde** »
   parce qu'il n'y en a qu'une. Le nom suit le contenu.
   **Structure ORP vérifiée** : 4 cartes (débit d'injection, régulation, historique,
   calibration), `#orp-card-calibration` **déjà masquée par défaut**
   (`style="display:none"`), `#orp_cal_trigger_btn`. Même motif que le pH, réutilisable tel quel.
   ✅ **LA PAGE TEMPÉRATURE SUIT AUSSI** — question posée à l'utilisateur le 2026-10-12,
   réponse : **oui**. ⇒ **TROIS pages** adoptent le mode dédié : pH, ORP, Température.
   Structure vérifiée : `#temperature-card-calibration` **déjà masquée par défaut**
   (`style="display:none"`, `index.html:680`), `#temp_cal_trigger_btn` (`:640`). Même motif.
   ⇒ **Trois usagers confirmés** : l'argument du **mécanisme générique** (une seule classe, pas
   trois implémentations jumelles) et celui de l'**ADR** en sortent renforcés.
2. ⚠️ **Le terme « calibrer » ou « calibration » DOIT être conservé dans le titre** du point
   d'entrée. C'est une contrainte, pas une préférence.
3. ⚠️ **Le mot « Recommandé » est ajouté au titre de la correction**, pour signaler qu'après un
   étalonnage il est de bonne pratique de vérifier avec un test externe.

## ✅ VERDICT `pool-chemistry` + DÉCISIONS UTILISATEUR — 2026-10-12

### 🔴 « Recommandé » dans le titre : REFUSÉ par le gate, décision utilisateur = SUIVRE LE GATE

**La décision initiale de l'utilisateur (« ajouter Recommandé au titre ») est ÉCARTÉE** — par
lui-même, après l'argument chimique. Ne pas la réintroduire.

**Pourquoi** : un étalonnage EZO sur tampons est l'opération **la plus juste de toute la
chaîne** (tampons ±0,01-0,02). Le vérifier derrière à la bandelette (**±0,5**, soit l'amplitude
**complète** de `kPhOffsetMax`) revient à arbitrer une référence avec un instrument **25× plus
grossier**. La correction reste légitime — elle existe pour un décalage **externe** réel
(courants de fuite, incident 2026-08-25) — mais ce n'est **pas** une étape de contrôle
post-étalonnage.

**Le risque est chimique, pas cosmétique** (chiffré par le gate) : une correction de bruit de
**+0,3** fait viser au régulateur un pH réel de **7,5-7,8** au lieu de 7,20-7,50. À pKa 7,5,
HOCl passe de **52 %** (pH 7,50) à **41 %** (7,70) : la boucle ORP compense par **surdosage**,
puis `kPhOrpInhibit` (7,60 corrigé) **coupe le chlore**. *Un mot d'interface déplace la chimie
du bassin.*

**⇒ RETENU : titre INCHANGÉ** (« Correction manuelle de la mesure », **pas de badge**), et une
**ligne contextuelle affichée UNIQUEMENT APRÈS UN ÉTALONNAGE RÉUSSI**, dans la carte du dessus.
**Texte imposé par le gate** :

> « Étalonnage terminé. Vérifiez avec un test **fiable** (gouttes ou testeur électronique) : si
> un écart **≥ 0,3 pH** se confirme sur **deux mesures** à des moments différents, corrigez
> ci-dessous. Une **bandelette (±0,5)** ne suffit pas à justifier une correction. »

⚠️ Conditionné **au moment** (après étalonnage) **ET au résultat** (écart ≥ 0,3 confirmé deux
fois). Les deux conditions sont dans le texte, ne pas en retirer une.

### 🔴 DÉFAUT TROUVÉ PAR LE GATE, HORS PÉRIMÈTRE INITIAL — INTÉGRÉ AU LOT

**Un étalonnage modifie le zéro ET la pente de la sonde : la correction antérieure devient
CADUQUE.** Elle compensait un état de la sonde qui **n'existe plus**. Aujourd'hui, elle continue
pourtant de s'appliquer **en silence** après chaque étalonnage — sur une valeur qui engage le
dosage.

**Décision utilisateur : la SIGNALER et PROPOSER la remise à zéro.** Pas de remise à zéro
automatique — « rien dans son dos sur une valeur qui engage le dosage ». L'utilisateur décide.

⇒ Après un étalonnage réussi, si une correction non nulle est en vigueur : le dire, donner sa
valeur, et offrir **deux** issues explicites (l'annuler / la garder). ⚠️ `ux-ui-designer` arrête
la forme ; `pool-chemistry` revalidera le texte au gate 2.

### 🔴 DÉCISION UTILISATEUR — HORODATER LA CORRECTION. **Le lot inclut désormais du FIRMWARE.**

L'`ux-ui-designer` a signalé honnêtement une **limite** de sa proposition : détectée côté
navigateur, l'alerte « correction caduque » **ne survivrait pas à un rechargement de page**.
Étalonner puis recharger ⇒ alerte perdue, et **la correction périmée continue de s'appliquer en
silence**. Le défaut n'aurait été fermé **qu'à moitié**.

**Décision utilisateur : horodater.** L'appareil retient **quand** la correction a été posée et
le compare à la **date du dernier étalonnage**. Antérieure ⇒ **caduque**, et ce verdict reste
vrai après un rechargement, un redémarrage, des semaines plus tard.

⚠️ **CONSÉQUENCE : la spec disait « aucun changement firmware attendu » — C'EST FAUX
DÉSORMAIS.** `embedded-developer` est requis. Vérifié : **aucun horodatage n'existe aujourd'hui**
(grep : ni `phOffsetSetAt`, ni `offsetEpoch`, ni équivalent).

⚠️ **POINT DUR POUR L'`architect`** : `phSlopeAgeMs` est l'âge de la dernière **LECTURE** de
pente, **pas** la date de l'étalonnage. Il faut donc **DEUX** horodatages, pas un :
l'instant où la correction est posée, **et** l'instant où un étalonnage est validé. Le second
n'existe pas plus que le premier.

## Comportement attendu

### Les deux états de la page pH

**État courant** : débit d'injection, régulation, historique. **Ni** calibration, **ni**
correction. Un **point d'entrée** mène au second état.

**État « réglage »** : tout le reste est masqué. On voit, dans cet ordre :
1. un **lien de retour** ;
2. la carte **Calibration de la sonde pH** ;
3. la carte **Correction manuelle de la mesure**, titre portant « **Recommandé** » ;
4. un **lien de retour**.

### ⚠️ LE POINT QUI DÉCIDE DU SUCCÈS OU DE L'ÉCHEC : le nom du point d'entrée

Si le bouton reste « **Calibrer la sonde** », **la correction devient INTROUVABLE**. Quelqu'un
qui veut ajuster sa mesure sur une bandelette ne cliquera **jamais** sur « Calibrer » : il
cherchera sur la page, ne trouvera rien, et conclura que la fonction a disparu. **On aurait
rangé une fonction en la rendant invisible** — l'exact contraire du but.

⇒ Le titre doit **à la fois** contenir « calibration »/« calibrer » (**contrainte utilisateur**)
**et** laisser deviner que la correction est derrière. `ux-ui-designer` tranche la formulation
exacte ; **ces deux exigences sont cumulatives, pas alternatives.**

### Cas limites

- **Arrivée dans le mode avec la correction bloquée** (filtration < 10 min, sonde non étalonnée,
  mesure indisponible — feature-155) : l'utilisateur doit comprendre **pourquoi** en arrivant,
  sans croire à une panne. ⚠️ Le cas « **sonde non étalonnée** » est particulier et **favorable** :
  les deux cartes sont désormais **l'une au-dessus de l'autre**, donc le geste à faire d'abord
  est **juste au-dessus**. À exploiter.
- **Retour** : on revient à la page pH dans l'état où on l'a quittée (position de défilement,
  sections dépliées) — ne pas renvoyer en haut de page.
- **Navigation directe** (rechargement, lien profond, retour arrière du navigateur) : décider si
  le mode survit ou non. **Décision `architect`.**
- **Pendant une calibration en cours** : le lien de retour doit-il rester actif ? Une calibration
  interrompue laisse la sonde dans un état intermédiaire — à traiter explicitement.
- **Écran court** (390 px) : le lien du bas n'est atteint qu'après avoir fait défiler les deux
  cartes. C'est **voulu** (c'est sa raison d'être), mais vérifier qu'il reste visible et non
  collé au dernier élément.

## Comportement actuel

Les cinq cartes cohabitent sur la même page. La calibration est masquée jusqu'au clic sur
« Calibrer la sonde », puis s'affiche **au milieu des autres**. La correction est **toujours
visible**.

## Contraintes techniques

- **SPA, toutes les vues dans le DOM** (`is-active` en CSS) : le mode est un **état d'affichage**,
  pas une page nouvelle. Aucune route HTTP, aucun champ, aucun changement firmware attendu.
- ⚠️ **RESPONSIVE 390 px d'abord**, caractères comptés pour le point d'entrée et les liens de
  retour.
- ⚠️ **Accessibilité** : masquer des cartes ne doit pas laisser de pièges au clavier ni au
  lecteur d'écran (éléments masqués encore focalisables, titres orphelins). Le changement d'état
  doit être **annoncé**.
- ⚠️ **Ne pas casser les points d'entrée existants** : le chip d'état de la sonde ouvre un modal,
  le bouton « Aller à l'étalonnage » de la correction (feature-155) fait défiler vers la carte
  de calibration — **ce dernier n'a plus de sens si les deux cartes sont déjà côte à côte**, à
  réexaminer.
- Vanilla JS, français, aucune bibliothèque nouvelle.

## ✅ VERDICT `pool-chemistry` GATE 2 — GO SOUS CONDITIONS, toutes levées

### 🔴 BLOQUANT — LE TITRE DE LA BANNIÈRE MENTAIT

Le brouillon s'intitulait « **Correction désactivée par l'étalonnage** ». **Faux** :
`applyPhOffset` continue d'ajouter la correction à **chaque lecture** tant que l'utilisateur
n'a pas tranché. Elle **décide donc des injections d'acide** pendant tout le temps de
l'hésitation. Un utilisateur rassuré par « désactivée » pouvait laisser la bannière **des
semaines**.

**Texte imposé, appliqué mot pour mot** :
> **Correction à revoir après l'étalonnage**
> Une correction de **{valeur} pH** **reste appliquée à la mesure** : elle continue d'orienter
> le dosage tant que vous n'avez pas choisi. Elle a été posée **avant** l'étalonnage qui vient
> d'avoir lieu et compensait un réglage de sonde qui a changé — elle n'est peut-être plus
> justifiée.

⛔ « **reste appliquée** » et « **continue d'orienter le dosage** » sont **non négociables**.

### Estampiller aussi `ClearPhCal` — LE GATE A REFUSÉ NOTRE RAISONNEMENT

Le lot n'estampillait que `CalibratePhMid`/`Low`, au motif qu'« un effacement est toujours suivi
d'un étalonnage ». **Refusé** : c'est une **hypothèse**, pas une garantie. Rien n'oblige à
enchaîner, et un effacement seul laisserait la sonde **sans points** *et* la correction périmée
**non signalée**. Or après un `Cal,clear`, zéro et pente **ont changé** (l'EZO repart sur ses
valeurs d'usine) : la correction est caduque au sens chimique, exactement comme après un
étalonnage. ⇒ **Estampillage ajouté** — il ne peut rendre le verdict que **plus conservateur**.

### Autres points — validés tels quels

- **Conseil post-étalonnage** : repris mot pour mot, bon endroit, transitoire. **Conforme.**
- **« Garder » re-estampille** : **acceptable**, le dommage reste borné (refus hors ±0,50
  maintenu, action tracée). **Condition mineure appliquée** : le message de confirmation dit
  désormais « Correction conservée — **vérifiez-la par un test fiable** ».
- **Rétrocompatibilité** : compteurs à 0 ⇒ verdict faux ⇒ **silence, pas fausse alerte**.
  Validé **tel quel** : « une alarme sur tout le parc au premier démarrage apprendrait à
  l'ignorer. Le silence est le repli sûr. » ⚠️ **Conséquence à assumer** : une correction posée
  avant un étalonnage **antérieur à cette version** ne sera signalée qu'au **prochain**
  étalonnage.
- **« Annuler » via `/save-config`** sans les gardes de `/tune-ph-offset` : **confirmé** —
  « exiger 10 min de filtration pour **retirer** une correction serait une garde qui bloque plus
  qu'elle ne protège ».
- **Invariants intacts** : bornes ±0,50 (refus, pas écrêtage), `roundPhOffsetToStep`,
  ADR-0039, NaN-in/NaN-out. Aucun `requestReset`/`resetDosingStates`/`resetPhPauseGuard`.

**Portée du contrôle, dite par le gate** : il n'a **pas** relu la boucle de dosage, les pages
ORP/Température, ni l'accessibilité/responsive.

## Livrables — check-list

- [x] B1 — Deux états de la page pH ; l'état courant n'affiche **ni** calibration **ni**
      correction.
- [x] B2 — Point d'entrée dont le titre contient « calibration »/« calibrer » **ET** laisse
      deviner la correction. **Les deux, pas l'un ou l'autre.** (« Calibration et correction de
      la mesure ») — ⚠️ le jugement final sur la findabilité reste à faire par l'utilisateur
      (cf. Critères d'acceptation ci-dessous), pas par un agent.
- [x] B3 — Lien de retour **en haut ET en bas**, nom arrêté par `ux-ui-designer` (« ← Retour à
      la page pH/ORP/Température »).
- [x] B4 — Dans l'état réglage : calibration **puis** correction, dans cet ordre (DOM réordonné
      physiquement, pas d'`order` CSS).
- [x] B5 — ⚠️ **TITRE INCHANGÉ, pas de badge « Recommandé »** (décision utilisateur après
      refus du gate). À la place : **ligne contextuelle affichée uniquement APRÈS un étalonnage
      réussi**, texte **imposé mot pour mot** par le gate (cf. section « VERDICT » ci-dessus).
- [x] A1 — **Horodater la correction** (instant de pose) **et l'étalonnage** (instant de
      validation), persistés (`phOffsetEpoch`/`phOffsetSeq`/`phCalEpoch`/`phCalSeq`, NVS). ⚠️
      `phSlopeAgeMs` évité à dessein : c'est l'âge de la dernière **lecture** de pente, pas la
      date de l'étalonnage.
- [x] A2 — Verdict **« correction caduque »** (`ph_offset_stale`) servi au client : vrai si la
      correction est **antérieure** au dernier étalonnage **et** non nulle (comparaison par
      compteur, pas par horloge — [ADR-0102](../../../docs/adr/0102-ordre-des-gestes-de-justesse-par-compteur.md)).
      Survit au rechargement **et au redémarrage**.
- [x] B11 — **Correction caduque après étalonnage** : si une correction non nulle est en vigueur
      à la fin d'un étalonnage réussi, le **signaler** (avec sa valeur) et proposer **deux
      issues explicites** — l'annuler, ou la garder. ⛔ **PAS de remise à zéro automatique** :
      rien dans le dos de l'utilisateur sur une valeur qui engage le dosage.
- [x] B6 — Les blocages de la correction (feature-155) sont compréhensibles **à l'arrivée**.
      Exploiter la proximité : « sonde non étalonnée » ⇒ la carte à utiliser est juste au-dessus.
- [x] B7 — Retour **sans perte de contexte** (défilement restauré par `requestAnimationFrame`
      après repaint ; la page pH elle-même n'a pas de section dépliable à préserver au-delà du
      scroll).
- [x] B8 — **Accessibilité** : rien de masqué ne reste focalisable (`hidden` natif, pas
      `style.display`/`visibility` — sort aussi de l'ordre de tabulation et de l'arbre
      d'accessibilité), changement d'état annoncé (région `role="status" aria-live="polite"`),
      ordre de tabulation cohérent (DOM réordonné physiquement).
- [x] B9 — Responsive **vérifié à 390 px**, caractères comptés. Vérifié d'abord par lecture de
      code (`.maintenance-back-link` : `width: 100%`, jamais partagée avec un autre élément sur
      la même ligne — donc aucun `flex-wrap` possible).
      ✅ **CONTRÔLE VISUEL SUR APPAREIL RÉEL FAIT PAR L'UTILISATEUR LE 2026-10-12**, après le
      déploiement de la v2.73.0. Quatre points regardés sur la page pH en mode entretien :
      atteignabilité du lien de retour BAS sous les deux cartes empilées, tenue du pas-à-pas sur
      une ligne, **séparation effective du bouton destructif** d'avec « Corriger » (les 12 px de
      `.action-destructive` livrés en feature-157), et absence de débordement ou de coupure de
      texte. Verdict : conforme.
      ⚠️ Conditions du contrôle, pour que la portée soit lisible : correction `ph_offset` = +0,50
      en vigueur, donc le bouton destructif **était** affiché — le point le plus récent est bien
      celui qui a été vu, pas supposé.
- [x] B13 — **MÊME MODE DÉDIÉ SUR LA PAGE TEMPÉRATURE** (`#temperature-card-calibration`,
      `#temp_cal_trigger_btn`). Point d'entrée **« Calibrer la sonde » conservé**. Mécanisme
      **GÉNÉRIQUE** livré : une seule classe (`is-flow-maintenance`), une seule paire de
      fonctions paramétrées (`enterMaintenance`/`exitMaintenance`, table `MAINTENANCE_PAGES`) —
      pas trois implémentations jumelles (les anciennes `show/hideTempCalibrationCard()` sont
      retirées).
- [x] B12 — **MÊME MODE DÉDIÉ SUR LA PAGE ORP** : `#orp-card-calibration` quitte la vue
      courante, liens de retour en haut et en bas, même mécanisme. Point d'entrée **« Calibrer
      la sonde » conservé** — une seule destination, donc le nom n'a rien d'autre à annoncer.
      Aucune carte de correction côté ORP (asymétrie assumée, le module EZO ORP n'a ni pente ni
      zéro).
- [x] B10 — Le bouton « Aller à l'étalonnage » de feature-155 (`#ph_tune_goto_cal_btn`) est
      **supprimé** : il faisait défiler vers une carte lointaine, elle est désormais juste
      au-dessus.
- [x] C1 — `docs/features/page-ph.md`, `docs/features/page-orp.md`,
      `docs/features/page-temperature.md`, `docs/API.md`, `docs/subsystems/sensors.md`,
      `CHANGELOG.md`, `docs/user/src/guide-utilisation.html` mis à jour (doc-writer) — **le
      chemin pour atteindre ces deux gestes change sur trois pages**.
- [x] D1 — La bascule d'état (mode réglage) est de la logique **client (JS)**, rien d'extractible
      côté firmware pour elle : couverte par le **plan de test manuel** ci-dessous, pas par
      `pio test -e native`. En revanche la logique de justesse (A1/A2), elle, **est** pure côté
      firmware et **est** testée en natif : `isPhOffsetStale()` / `nextJustnessSeq()`, 7 cas
      neufs dans `test/test_native_ph_offset/test_ph_offset.cpp` (section D4).

## Sécurité chimique

- Dosage impliqué : **non directement** — aucune ligne de `pump_controller`, aucune route, aucun
  seuil touché. Ce lot **déplace** des cartes.
- ⚠️ **MAIS UN POINT EXIGE UN ARBITRAGE** : le mot « **Recommandé** » au titre de la correction.
  La valeur indiquée dans cette carte **engage le dosage** (feature-155) : une valeur mal
  indiquée **déplace le pH réel du bassin**, et le gate avait imposé l'avertissement « *c'est
  elle qui décidera des injections d'acide* ». **Recommander** un geste qui peut nuire n'est pas
  un choix de rédaction.
  **Questions au gate** : (a) « Recommandé » est-il justifié — après un étalonnage, vérifier
  avec un test externe est-il une **bonne pratique** ou une **source d'erreur supplémentaire**,
  sachant que l'utilisateur emploie des **bandelettes à ±0,5 pH** ? (b) Le mot risque-t-il de
  pousser à corriger **alors qu'aucun écart réel n'existe** ? (c) Si « Recommandé » est retenu,
  faut-il le conditionner (recommandé **après une calibration**, pas en permanence) ? (d) La
  formulation exacte.
- Risque identifié : **un geste facultatif présenté comme attendu**. Un utilisateur qui « suit
  les recommandations » pourrait corriger sur un test imprécis et **dégrader** une mesure juste.

## ADR attendu ?

- **Probable** : non. ⚠️ Si l'`architect` juge que « les gestes d'entretien quittent la vue
  courante » est un motif à généraliser (la page ORP, la page Température ont les mêmes boutons
  de calibration), qu'il le dise — ce serait le critère 1.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- **aucun**

### Documentation à produire / mettre à jour
- `docs/features/page-ph.md` — les deux états et le chemin d'accès.
- `docs/user/src/guide-utilisation.html` — ⚠️ **le chemin pour atteindre l'étalonnage et la
  correction change** : c'est le document qui décrit ces gestes. Ne régénérer **aucun PDF**.
- `CHANGELOG.md` — entrée 2.72.0.

## Critères d'acceptation

- [x] Sur la page pH courante, **aucune** des deux cartes n'est visible.
- [x] Le point d'entrée contient « calibration »/« calibrer » **et** un utilisateur qui cherche
      à corriger sa mesure le trouve. ⚠️ **À faire juger par l'utilisateur**, pas par un agent :
      c'est lui qui a signalé le malentendu d'origine.
      ✅ **VALIDÉ PAR L'UTILISATEUR LE 2026-10-12.** Question posée telle quelle : « sur la page
      pH, en lisant *Calibration et correction de la mesure*, est-ce que vous iriez cliquer là
      pour corriger votre mesure ? » — réponse : « *Oui, c'est ok pour moi.* »
      ⇒ Le libellé est **confirmé**, et le risque identifié en conception (le mot « Calibration »
      en tête qui ferait chercher ailleurs la correction) **ne s'est pas matérialisé**. C'est un
      jugement de lecture, pas un test d'usage prolongé : si un malentendu apparaissait plus
      tard, c'est ce libellé qu'il faudrait rouvrir en premier.
- [x] Dans l'état réglage, les deux cartes sont visibles **dans l'ordre** calibration puis
      correction, avec un lien de retour **en haut et en bas**.
- [x] ~~Le titre de la correction porte « Recommandé » dans la forme validée par le gate.~~
      **CORRIGÉ (relevé en revue, doc-writer) : ce critère contredisait B5 et la décision
      finale.** Le gate `pool-chemistry` a **refusé** le mot « Recommandé » au titre (risque
      chimique chiffré : une correction de bruit de +0,3 pH pousse le régulateur à viser un pH
      réel de 7,5-7,8 au lieu de 7,20-7,50). ⇒ **Titre inchangé** (« Correction manuelle de la
      mesure », pas de badge), remplacé par une **ligne contextuelle affichée uniquement après
      un étalonnage réussi**, texte imposé par le gate. Critère réellement livré : le conseil
      post-étalonnage (`#ph_cal_final_advice`) affiche le texte imposé mot pour mot — voir
      [docs/features/page-ph.md](../../../docs/features/page-ph.md#conseil-post-étalonnage-ph_cal_final_advice-feature-156-v2720).
- [x] Le retour ne perd pas le contexte de la page (défilement restauré).
- [x] Rien de masqué n'est atteignable au clavier (`hidden` natif, retiré de l'ordre de
      tabulation et de l'arbre d'accessibilité).
- [x] Vérifié **à 390 px** — lecture du CSS **puis** contrôle visuel sur appareil réel par
      l'utilisateur le 2026-10-12 (détail et portée exacte du contrôle : B9 ci-dessus).
- [x] `./build_fs.sh` dans les 512 Ko · `pio test -e native` zéro régression. Référence mesurée
      au moment de cette documentation : `pio run` SUCCESS (Flash 50,0 %), **68 suites / 2 121
      cas**, 14 contrôles d'hygiène (ADR-0073 : à re-mesurer, ne pas recopier ce chiffre tel
      quel dans un cycle futur).

## Plan de test manuel
1. Page pH : vérifier qu'aucune des deux cartes n'apparaît.
2. Entrer dans le mode : calibration en haut, correction dessous, liens de retour aux deux bouts.
3. Entrer avec la filtration à l'arrêt : le blocage de la correction est compréhensible.
4. Entrer avec une sonde non étalonnée : le lien entre les deux cartes est évident.
5. Revenir : la page pH est dans l'état où on l'a quittée.
6. Parcours **au clavier seul** : rien de masqué n'est atteignable.
7. Téléphone, 390 px : les deux liens de retour sont utilisables.

## Hors périmètre
- ⚠️ **LA PAGE ORP N'EST PLUS HORS PÉRIMÈTRE** — correction du 2026-10-12, cf. décision 1.
  Elle reçoit le **même mode dédié**, avec sa seule carte de calibration. Seule l'**absence de
  carte de correction** y reste asymétrique.
- ⚠️ **LA PAGE TEMPÉRATURE N'EST PLUS HORS PÉRIMÈTRE** (décision utilisateur du 2026-10-12) :
  elle reçoit le même mode dédié, avec sa seule carte de calibration. Point d'entrée
  « Calibrer la sonde » **conservé** — une seule destination, comme l'ORP.
- **Le contenu des deux cartes** : feature-154 et feature-155 viennent de les livrer. Ce lot les
  **déplace**, il ne les réécrit pas.
- **Tout changement firmware** : à confirmer par l'`architect`, mais rien n'est attendu.
