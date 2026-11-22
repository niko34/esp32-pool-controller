## Métadonnées
- **ID** : bug-201
- **Slug** : l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation
- **Type** : bug
- **Statut** : **todo — BLOQUÉ par [feature-170](../doing/feature-170-retirer-la-confirmation-de-correction-ph.md)** (arbitrage utilisateur du 2026-11-21)
- **Priorité** : **haute**
- **Version cible** : 2.97.0
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [bug-200](../done/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md) (même famille) · [feature-168](../done/feature-168-correction-ph-sans-borne.md) (le gate C12 dont la contrepartie n'a jamais été payée)

## Domaines touchés
- [x] Frontend (`data/`)
- [x] Régulation — **consommée**, pas modifiée → `pool-chemistry` pour avis
- [ ] Web — routes HTTP / WebSocket *(rien à ajouter : les deux clés sont déjà servies)*

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry
- [x] ux-ui-designer
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
L'alerte « correction de mesure pH à confirmer » est écrite, juste, et servie par le firmware —
mais **le seul écran qui l'affiche est inatteignable sans suspendre la régulation**. Le
propriétaire ne peut pas la lire sans geler son dosage.

## Contexte
Signalé par l'utilisateur le 2026-11-18, mot pour mot : **« je ne vois pas l'alerte »**. Sa
machine est dans cet état en ce moment (`ph_offset = 1.2`, `ph_offset_stale = true`,
`ph_offset_stale_cause = "boot"`, lu sur `/get-config` de l'appareil).

### Faits vérifiés dans le code (relus un par un)
| Fait | Emplacement |
|---|---|
| La bannière existe et son texte est juste | `data/app.js:2522` (`renderPhOffsetStaleBanner`) |
| Elle vit dans `#ph_tune_stale_banner` | `data/index.html:1358` |
| ⛔ Qui est **à l'intérieur** de `#ph-card-tune`, marqué `hidden` | `data/index.html:1347` |
| ⛔ Lequel n'est dévoilé que par le flux maintenance | `data/app.js:10524` (`MAINTENANCE_PAGES.ph.cardIds`) |
| ⛔ Dont l'entrée émet `POST /calibration/enter`, **qui bascule la régulation en Manuel** | `data/app.js:10443` + commentaire `:10529-10531` |

### ⭐ La contradiction, et elle est écrite dans le fichier lui-même
`data/index.html:1337-1340` commente la carte ainsi :
> « **Toujours visible** (quel que soit le mode de régulation) car elle corrige la **MESURE**, pas
> la régulation. »

**Le commentaire est vrai. Le code ne l'est pas.** La carte porte `hidden` et figure dans la liste
des cartes de maintenance. ⇒ Personne n'a comparé l'intention écrite au comportement rendu.

### Le principe violé, nommé
Les gates de ce projet posent la même règle sous quatre formes (C3 segments `disabled` ⛔ jamais
`hidden` · C5 la visibilité permanente du mode **est** la protection · C10 consigne désactivée
⛔ jamais masquée · `index.html:183-186` « ni masquable ni repliable ») :

> ⭐ **Quand un gate renonce à BLOQUER, la contrepartie qu'il achète est la VISIBILITÉ PERMANENTE.**

Or feature-168 (C12) a **explicitement renoncé à bloquer** — décision utilisateur du 2026-10-16,
« je ne veux pas bloquer » — en échange d'un bandeau. ⇒ **La contrepartie n'a jamais été payée** :
« visible » a été implémenté comme « présent dans le DOM d'un sous-écran de maintenance », pas
comme « vu ». C'est un **défaut d'exécution de C12**, pas une demande de confort.

⚠️ La seconde voie de sortie est également défaillante : l'alerte MQTT de C12 est émise sous
`if (wifiConnected)` dans `setup()` (`src/main.cpp:491` englobant `:515-524`). Un démarrage hors
ligne ⇒ **aucune trace** sauf le journal, qui tient une minute.

## Comportement attendu
1. **`#ph-card-tune` est visible en permanence sur `#/ph`**, hors flux maintenance. Elle corrige la
   **mesure**, pas la sonde : elle ne justifie aucun gel de dosage. C'est ce que son propre
   commentaire annonce déjà.
2. ⛔ **La carte d'étalonnage EZO (`#ph-card-calibration`) reste, elle, dans le flux maintenance** —
   elle touche la sonde, son gel est légitime. ⚠️ Ne pas sortir les deux d'un bloc.
3. Les deux boutons « Annuler la correction » / « Garder » restent le **seul** chemin qui éteint le
   drapeau (`setPhOffsetStamped()` → `phOffsetBootUnconfirmed = false`, `src/config.cpp:196`).
   ⛔ Ne rien en retirer : les supprimer rendrait l'alerte **ineffaçable**.
4. Les trois textes de C15 (`boot` / `calibration` / `expiration`) restent **mot pour mot**.

### Cas limites
- `ph_offset` absent ou `NaN` ⇒ ⛔ aucune bannière. La garde `isFinite` n'est **pas** redondante
  avec `typeof === "number"` : `typeof NaN === "number"`.
- `ph_offset_stale` à `1` et non `true` ⇒ ⛔ aucune bannière (comparaison `=== true` stricte).
- `ph_offset === 0` avec `stale = true` ⇒ ⛔ aucune bannière.
- Entrer **puis sortir** du flux maintenance ne doit pas re-cacher la carte.

## Comportement actuel
La carte est `hidden`. Pour la voir, il faut appuyer sur le bouton d'entrée en maintenance, ce qui
bascule la régulation en Manuel et gèle le dosage. Aucun propriétaire ne fera ce geste pour
« aller voir s'il y a une alerte » — et s'il le fait, il paie l'information d'un arrêt de
régulation.

## Contraintes techniques
- ⚠️ **RESPONSIVE** : 390 px d'abord ; la carte doit tenir hors maintenance sans casser la page pH.
- ⛔ **Aucun champ firmware à ajouter** : `ph_offset_stale` et `ph_offset_stale_cause` sont déjà
  servis par **appel à l'accesseur** sur `/get-config` (`src/web_routes_config.cpp:310-311`) **et**
  sur la trame WS (`src/ws_manager.cpp:1691-1692`). ⛔ Ne pas rouvrir une 5ᵉ forme du piège de parité.
- ⛔ **Ne jamais recalculer le verdict côté navigateur** à partir de `ph_offset` et d'un seuil
  (ADR-0100) : lire la clé servie.
- ⚠️ Le mot « **sonde** » est **interdit** dans ces textes : la correction compense un décalage
  **d'installation** (courants de fuite de la pompe de filtration), pas une sonde. Acquis des
  incidents du 25/08 et du 11/10, protégé par C8 révisé et par `src/ph_offset.h:55-63`.
- La valeur s'affiche à **2 décimales signées** (`_fmtPhOffset2`, `data/app.js:2507`),
  ⛔ pas `_fmtPh` (1 décimale) : convention assumée, le chiffre reproduit un gabarit de journal.

## Fichiers potentiellement concernés
- `data/app.js` — retirer `ph-card-tune` de `MAINTENANCE_PAGES.ph.cardIds` (`:10524`) et traiter ce que cela implique à l'entrée/sortie du flux
- `data/index.html` — retirer `hidden` de `#ph-card-tune` (`:1347`)
- `data/app.css` — mise en page de la carte hors maintenance (`.flow-maintenance`)
- `docs/features/page-ph.md`, `docs/DESIGN_SYSTEM.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] B1 — `#ph-card-tune` visible en permanence sur `#/ph`, étalonnage EZO **inchangé** dans le flux maintenance
- [ ] B2 — Entrée **et** sortie du flux maintenance laissent la carte visible
- [ ] C1 — `docs/features/page-ph.md`, `docs/DESIGN_SYSTEM.md` (contexte d'emploi de `.flow-maintenance`), `CHANGELOG.md`
- [ ] D1 — Capture à 390 px, page pH **sans** entrer en maintenance : la bannière est lisible
- [ ] D2 — ⭐ **Mutation exigée** : remettre `ph-card-tune` dans `cardIds` doit **tuer** un test
- [ ] D3 — Capture des trois causes (`boot` / `calibration` / `expiration`) avec `-c`

## Sécurité chimique
- Dosage impliqué : **indirectement** — la correction décide des injections d'acide
- Limites concernées : aucune n'est modifiée
- Filtration requise : sans objet
- Calibration capteur modifiée : **non** — ⛔ c'est précisément le point : la correction de mesure
  **n'est pas** un étalonnage
- Algorithme PID touché : non
- Risque identifié : ⛔ **ne pas inhiber le dosage** tant que la correction n'est pas confirmée
  (verdict `pool-chemistry` du 2026-11-18, **NO GO**). Motif : la cause `boot` se lève à **chaque
  démarrage** pour **tout** écart supérieur à 0,50 ; cet appareil a une correction **légitime et
  permanente** de +1,20, donc l'inhibition s'armerait **à chaque mise à jour**. En saison, un
  bassin privé d'acide 48 h part à pH 8,2-8,4, où la fraction de chlore actif tombe sous 25 % —
  **une panne sanitaire certaine en échange d'un risque hypothétique**. Le principe du projet
  s'applique : *les plafonds protègent, le reste informe*.

---

## ⏸️ ARBITRAGE UTILISATEUR — 2026-11-21 : feature-170 D'ABORD

⭐ **Le gate `pool-chemistry` a trouvé une contradiction entre deux décisions de l'utilisateur, et a
refusé de la trancher seul.**

| Date | Décision |
|---|---|
| **2026-11-14** | *« On supprime ce message ou pas ? »* ⇒ [feature-170](../doing/feature-170-retirer-la-confirmation-de-correction-ph.md) ouverte pour **SUPPRIMER** la cause `boot`, avec **GO `pool-chemistry` de 2ᵉ passe** |
| **2026-11-18** | *« je ne vois pas l'alerte »* ⇒ **cette fiche**, pour la rendre **PERMANENTE** |

⇒ Les deux fiches tirent **en sens opposé sur le même bandeau**.

**Question posée le 2026-11-21, réponse : « supprimer d'abord, puis bug-201 ».**

⚠️ **Le motif, et il est bon** : la correction de **+1,20** de cet appareil est **légitime et
permanente**, et `src/config.cpp:829-830` relève le drapeau à **chaque démarrage** dès que
`|offset| > 0,50` — ⛔ **même après confirmation**. ⇒ Rendre l'alerte permanente afficherait en
continu, sur la page pH, une boîte ambre annonçant que la mémoire de l'appareil est peut-être
altérée **à propos d'une valeur parfaitement juste**, et elle **reviendrait après chaque mise à
jour**. ⭐ *Une alerte dont on apprend qu'elle ne veut rien dire ne se lit plus le jour où elle veut
dire quelque chose* — et ce jour-là, c'est la cause `calibration`.

⇒ **bug-201 reste entièrement justifié après feature-170**, et sa valeur devient plus nette :
*voir et régler la correction **sans geler le dosage***, et afficher un **fait** (« correction
+1,20, posée le 16/10/2026 ») au lieu d'une question.

---

⚠️ **PHRASE AMENDÉE le 2026-11-22 (arbitrage Q14 de feature-170), par le gate qui l'avait
imposée.** L'ancienne rédaction (« avant l'étalonnage qui **vient d'avoir lieu** ») affirmait un
événement que l'appareil ⛔ **ne peut pas prouver** : `markPhCalibrationStamped()` est aussi
appelée sur un **EFFACEMENT** d'étalonnage (`sensors.cpp:920`, délibéré). ⭐ Règle posée dans
ADR-0139 : *quand plusieurs chemins mènent au même état, le texte nomme l'ÉTAT, ⛔ jamais le
chemin.* ⇒ bug-201 et feature-170 portent désormais la **même** formule, mot pour mot.

## ⛔ RENUMÉROTATION IMPOSÉE — 2026-11-22 : les conditions de bug-201 partent de `C40`

**Règle posée pendant la 3ᵉ passe du gate de feature-170**, après découverte d'une collision
totale : `feature-168` occupe déjà `C1`→`C17` **sur les mêmes fichiers** que les conditions
`C0`→`C4` ci-dessous. Le cas le plus dangereux : `feature-168 (C12)` désigne le drapeau de
confirmation, et un `C12` nu dans un commentaire neuf désignerait tout autre chose.

> **Règle 1 (générale, tout le projet).** ⛔ **Aucun `Cxx` nu hors de la fiche qui le définit.**
> Toute référence porte son identifiant : **`bug-201 (C40)`**, jamais `C40`.
>
> **Règle 2 (ici).** **bug-201 renumérote ses conditions `C0`→`C4` en `C40`→`C44`** au moment
> où il reprendra. ⭐ Gratuit : **zéro ligne livrée** à ce jour, rien à renommer dans `src/`,
> `data/` ou `test/`.

| Actuel | Définitif |
|---|---|
| C0 | **C40** |
| C1 | **C41** |
| C2 | **C42** |
| C3 | **C43** |
| C4 | **C44** |

⚠️ **Et `C41` devient MOINS chère**, par effet de `feature-170 (C23)` : j'y avais écrit que le
seuil de déclenchement est « numériquement égal à `kPhOffsetUnusualAbs` **pour une autre
raison** ». Une fois `C23` appliquée (`kPhOffsetUnusualAbs = kPhSafetyMargin`, seuil de
**conséquence**), c'est **la même raison** ⇒ le déclencheur **lit la clé servie
`ph_offset_unusual_abs`**, toujours zéro champ nouveau (`C40` préservée).

⚠️ **Dette héritée de feature-170** : sa condition `C28` (la date de pose affichée en
permanence) **n'est pas visible en usage courant** tant que ce bug n'est pas livré — c'est
exactement la promesse que bug-201 débloque.

## ⚗️ AVIS `pool-chemistry` — 2026-11-21 : **GO SOUS CONDITIONS** (4 conditions, C0→C4)

⛔ **À reprendre tel quel quand ce lot redémarrera.** C4 est levée par l'arbitrage ci-dessus.

### ⭐ Le gate RENVERSE la question que je lui avais posée
Je demandais si les deux boutons devaient **rester** dans le flux maintenance. Sa réponse :
**NON — la carte sort AVEC ses boutons**, parce que

> ⭐ **le flux maintenance n'est pas une garde : il gèle le dosage pour lire une information.**
> Une barrière qui ne fait que **retarder** l'effet de ce qu'elle garde n'est pas une barrière,
> c'est un **péage** — et il se paie en dosage gelé.

Les vraies gardes sont ailleurs et **aucune ne dépend du flux** : filets BRUTS fixes 5,50 / 8,50
(`src/constants.h:2027-2028`, ⭐ ils ne bougent **jamais** avec la correction, c'est leur raison
d'être), plancher/plafond corrigés 6,70 / 7,60, inhibition ORP 7,60, et surtout `POST /tune-ph-offset`
a **son propre gate** (`src/ph_tune_logic.h:83-113` : mesure fraîche, **10 min de filtration**, sonde
étalonnée ≥ 2 points).

⭐ **Argument supplémentaire, indépendant** : aujourd'hui le geste **vertueux** (accorder la mesure
sur un test manuel) **coûte une suspension de régulation**, et dépend d'une sortie de flux qui peut
échouer. ⇒ Le design actuel rend **risqué le geste qu'on veut encourager**.

### 🔴 Le chiffrage : « Annuler la correction » par curiosité, sur CET appareil
Eau réelle 7,30, lecture brute 6,10. Annuler décale **tous les seuils exprimés en pH réel de +1,20** :

| Seuil | pH lu | pH **RÉEL** après annulation |
|---|---|---|
| Consigne 7,20-7,50 | 7,20-7,50 | **8,40-8,70** |
| Plancher acide 6,70 | 6,70 | **7,90** |
| Alerte « eau agressive » 7,00 | 7,00 | 8,20 — ⚠️ **alerte à CONTRESENS** |
| Inhibition ORP 7,60 | 7,60 | **8,80** |
| Filet BRUT 5,50 | — | 6,70 — ⭐ **ne bouge pas** |

1. **Acide : arrêté immédiatement** (6,10 < 6,70).
2. **pH+ : non** (produit absent chez cet utilisateur).
3. ⭐ **Inhibition du chlore : NON — et c'est le vrai danger.** Repoussée à **8,80 réels** ⇒ le
   chlore **continue de se doser** sur l'ORP pendant que la fraction de HOCl s'effondre.

⚠️ **Et il n'y a PAS d'escalade vers Manuel** : le compteur des 3 refus / 24 h compte des
**transitions**, et un blocage **permanent** n'en produit **qu'une seule**. ⇒ L'écran affiche
**« Automatique »** sur une régulation qui ne fait plus rien, après **une seule ligne** de journal.
HOCl passe de 63,5 % (7,30) à **12 % (8,40)** en 2 à 6 semaines, et la boucle ORP **surdose pour
rattraper** — l'hypochlorite étant basique, ⭐ **elle accélère la dérive qu'elle subit**.
⇒ *Les plafonds protègent, le reste informe* : le dégât est borné **en produit**, pas en qualité d'eau.

⭐ **L'asymétrie qui justifie C1** : annuler = **1 clic sans aucun gate** ; remettre = **test manuel
au réactif** + 10 min de filtration + sonde étalonnée. **Détruire est gratuit, réparer demande une
trousse d'analyse.**
⚠️ Le gate **révise explicitement le verdict de feature-157** (« aucune confirmation : geste
réversible qui va vers le défaut sûr ») : ⛔ ce raisonnement n'est vrai **que si la mesure nue est à
peu près juste**. Ici elle est fausse de 1,20 — la mesure nue **n'est pas le défaut sûr**.

### Conditions
- **C0** — ⛔ **ZÉRO ligne dans `src/`.** Les deux clés sont **déjà servies** par appel à l'accesseur
  (`src/web_routes_config.cpp:311-312`, `src/ws_manager.cpp:1695-1696`). Si une modification firmware
  apparaît, **le lot sort du GO**. ⛔ Et aucune réécriture client d'un verdict servi (ADR-0100).
- **C1** — ⛔ **« Annuler » exige un second geste explicite quand `|correction| > 0,50`.**
  ⭐ Seuil **dérivé, pas hérité** : à la consigne basse 7,20, annuler `o` donne `7,20 − o`, refusé dès
  que `7,20 − o < 6,70`, soit `o > kPhSafetyMargin = 0,50`. ⇒ Le déclencheur signifie exactement
  *« annuler cette correction arrête le dosage d'acide sur le champ »*. ⚠️ Numériquement égal à
  `kPhOffsetUnusualAbs` **pour une autre raison** — à écrire en commentaire.
  Sous le seuil : ⛔ **aucune confirmation** (*une garde qui bloque plus qu'elle ne protège est un
  défaut*). Action par défaut : **ne rien faire**. ⛔ Pas de `confirm()` natif.
  **Texte imposé**, valeurs **interpolées** depuis les données servies (⛔ jamais en dur) :
  > « Sans cette correction, l'appareil lira **6,10** au lieu de **7,30** : il cessera d'ajouter de
  > l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel. »
  ⚠️ **Même règle pour « Supprimer la correction »** (`#ph_tune_remove_btn`) : même effet par un
  autre chemin. ⛔ Deux boutons au même effet avec deux niveaux de friction serait **pire** que pas
  de friction du tout.
- **C2** — ⭐ **Le mot « sonde » doit sortir du texte de la cause `calibration`.**
  ⚠️ Motif propre à ce lot : la phrase ne dit pas « la sonde a été étalonnée », elle dit que la
  correction *compensait un état de la sonde* — c'est l'affirmation bannie, elle attribue
  **l'origine** à la sonde. ⭐ **Et ce lot rend la contradiction visible en permanence** : à 16 px
  d'écart, `data/index.html:1410` affiche en continu *« L'écart vient de l'installation, pas de la
  sonde »*. Tant que la carte était enfouie, les deux phrases ne se rencontraient presque jamais.
  **Phrase imposée** (⛔ seule la proposition après les deux-points change) :
  > « L'étalonnage a changé le 14/11/2026. La correction en vigueur (+1,20) a été posée le
  > 16/10/2026, donc avant : **ce changement a déplacé la mesure sur laquelle elle avait été
  > calculée.** Refaites un test manuel et accordez la
  > mesure dessus, ou remettez la correction à zéro. »
  ⛔ **`boot` et `expiration` restent MOT POUR MOT** — `boot` parle de **mémoire** (exact),
  `expiration` parle du décalage **de l'installation** (exact). ⚠️ La divergence entre les trois
  textes est **normale** : trois causes, trois gestes (C15 de feature-168).
  ⚠️ **feature-170 reprend la formule fautive** (`specs/features/todo/feature-170-…:175`) : elle doit
  adopter **la même correction**.
- **C3** — ⛔ **Aucun bouton d'annulation hors de la page pH.** Un **lien** « Voir la correction »
  depuis l'accueil est acceptable ; un bouton qui **agit** ne l'est pas.
- **C4** — ✅ **LEVÉE** par l'arbitrage utilisateur du 2026-11-21.

### ⚠️ La fiche est INEXACTE sur un point, à corriger avant de la traiter
Elle dit que les deux boutons sont le **seul** chemin qui éteint le drapeau. ⛔ **Faux** :
`setPhOffsetStamped()` est appelé par **deux routes** — `POST /save-config` branche `ph_offset`
(« Annuler », « Garder », **et** « Supprimer ») et `POST /tune-ph-offset` (**le pas-à-pas**).
⇒ **Trois gestes**, pas deux. Comme ils sortent **tous ensemble** avec la carte, la crainte
« alerte ineffaçable » **ne se pose pas**.

### ⛔ Le piège le plus probable de ce lot
Garder la classe `.flow-maintenance` sur `#ph-card-tune`, **retirer son `hidden`**
(`data/index.html:1365`) et **la retirer de `cardIds`** (`data/app.js:10661`).
⚠️ Si quelqu'un retire **la classe** au lieu de l'entrée de `cardIds`, la règle CSS générique
(`data/app.css:4708-4712`) **masquera la carte pendant le flux de calibration** — ⛔ c'est-à-dire
**exactement au moment où la cause `calibration` vient de se lever et où les boutons servent**.

### Verdict du 2026-11-18 (NO GO sur l'inhibition du dosage) : **IL TIENT**
Re-vérifié : `src/config.cpp:829-830` lève le drapeau à **chaque démarrage** dès `|offset| > 0,50`,
⛔ **même après confirmation**. ⇒ Inhiber armerait l'inhibition **à chaque mise à jour OTA**.

### Recommandations non bloquantes
- ⭐ Afficher la **date de pose** (`ph_offset_epoch`, **déjà servie des deux côtés et lue nulle part
  dans `data/`**) : « Correction +1,20 — posée le 16/10/2026 ». C'est le C11 de feature-170, gratuit ici.
- ⛔ **Ne rien afficher avant le premier `/get-config`** : une carte qui annonce « aucune correction »
  pendant 300 ms est une invitation à un geste faux.

---

## 🎨 ÉTUDE D'ERGONOMIE — 2026-11-21 (captures lues, après un premier avis rendu sans elles)

⚠️ **Le premier rapport avait été rendu SANS regarder les captures** — l'agent l'a dit lui-même.
⛔ C'est ce que CLAUDE.md appelle un avis **déduit** au lieu d'**observé**. Redemandé ; trois réponses
ont **changé** après lecture.

⭐ **Ce que seules les captures montrent** : la page pH porte **déjà** une alerte visible — une puce
rouge « Calibré · critère hors norme ». ⇒ Elle parle de **l'étalonnage de la sonde**, pas de la
correction. **Quand l'utilisateur dit « je ne vois pas l'alerte », il en voit UNE — mais pas la bonne.**

- **390 px** : le texte `boot` fait 6 lignes ≈ 145 px ; avec titre, bouton et note, ≈ **230 px**, soit
  **un tiers de la hauteur** de l'écran. ⛔ Impossible à raccourcir : les textes sont gelés.
- **Accumulation** : la carte héros de la page pH empile **8 blocs** (puce, bouton, badge, dose du
  jour, maximum journalier, atteinte de la cible, « Historique indisponible », « MAJ auto »).
  ⇒ **L'ajout doit être compensé par des retraits** : la ligne « MAJ auto toutes les 5 secondes »
  (`data/index.html:910`, l'heure est déjà dans `#ph-filter-sub`) et « Historique indisponible ».
- ⚠️ **Garde-fou non demandé** : réécrire le texte à chaque rafraîchissement ferait **relire l'alerte
  par un lecteur d'écran toutes les 30 s**. ⇒ N'écrire que si le texte **a changé**. (Même défaut que
  celui corrigé dans le journal par bug-179, à un autre endroit.)
- **À ouvrir en fiche** : le bloc « Atteinte de la cible » occupe **5 lignes** pour dire qu'il n'y a
  rien à comparer hors mode automatique · 6 lignes de mode d'emploi sous « Injection manuelle » ·
  la puce rouge « Calibré · critère hors norme » **concurrence** la nouvelle alerte · un
  `font-size:2rem` en ligne **contredit** les 32 px de feature-195.

⚠️ **Le gate chimique a renversé la forme retenue par l'ergonomie** : elle proposait la **forme B**
(bannière seule permanente, réglage derrière le flux) ; le gate impose la **forme A + friction**
(carte entière **avec** ses boutons, et un second geste sur « Annuler »). ⇒ **C'est le gate qui
prime**, son verdict est contraignant. L'ergonomie garde tout le reste : emplacement, compensations,
accessibilité, états limites.
