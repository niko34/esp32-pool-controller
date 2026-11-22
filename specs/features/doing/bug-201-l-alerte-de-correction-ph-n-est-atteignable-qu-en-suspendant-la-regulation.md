## Métadonnées
- **ID** : bug-201
- **Slug** : l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation
- **Type** : bug
- **Statut** : **doing** — ✅ **DÉBLOQUÉ** : [feature-170](../done/feature-170-retirer-la-confirmation-de-correction-ph.md) est livrée, déployée (v2.99.0) et vérifiée sur l'appareil le 2026-11-22
- **Priorité** : **haute**
- **Version cible** : **à définir** (⚠️ 2.99.0 est en ligne ; trancher 2.99.1 ou 3.0.0 à la livraison)
- **Dernière mise à jour** : **2026-11-22**
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

## 🔴 CE QUE feature-170 A CHANGÉ — à lire AVANT tout le reste de cette fiche

⚠️ **Cette fiche a été écrite le 2026-11-18. feature-170 a été livrée et déployée le 2026-11-22 au
matin, et elle en a périmé une partie.** Tout ce qui suit a été **revérifié dans le code du jour**.

### ✅ Ce que feature-170 a DÉJÀ payé — ⛔ ne pas le refaire
| Point de cette fiche | État |
|---|---|
| `C42` (ex-`C2`) — sortir le mot « sonde » du texte `calibration` | ✅ **LIVRÉ** par `feature-170 (C29)`, avec la phrase amendée le 2026-11-22 |
| Recommandation « afficher la date de pose » | ✅ **LIVRÉ** par `feature-170 (C28)` — ⚠️ mais **invisible en usage courant**, et c'est précisément la dette que CE lot paie |
| « ⛔ Ne rien afficher avant le premier `/get-config` » | ✅ **LIVRÉ** (`renderPhTuneAppliedLine`) |
| « La fiche est INEXACTE : trois gestes, pas deux » | ✅ **Intégré** — et un **quatrième** chemin existe désormais, voir ci-dessous |

### ⛔ Ce qui est devenu FAUX dans cette fiche
1. 🔴 **Les causes `boot` et `expiration` N'EXISTENT PLUS.** Il n'en reste **qu'une** :
   `calibration`. ⇒ Toute phrase de cette fiche parlant de « trois causes », de « trois textes »,
   du drapeau `phOffsetBootUnconfirmed` ou du texte `boot` est **caduque**.
   ⚠️ Un firmware **antérieur** peut encore servir `"boot"` / `"expiration"` : le client affiche
   alors un **texte neutre** (`feature-170 (C33c)`) et **masque le chapeau**.
2. 🔴 **Le drapeau `phOffsetBootUnconfirmed` est supprimé**, ainsi que le bloc d'alerte MQTT
   différée de `setup()` sous `if (wifiConnected)`. ⇒ Le paragraphe « la seconde voie de sortie
   est également défaillante » est **sans objet**.
3. 🔴 **Les boutons ont changé de nom** : « Annuler la correction » → **« Remettre à zéro »**,
   « Garder la correction » → **« Je l'ai vérifiée — garder »**, et le destructif est passé
   **en second**, en `btn--danger action-destructive` (`feature-170 (C30)`).
4. 🔴 **La carte a MAIGRI** : `#ph_tune_calib_note` et `#ph_tune_remove_label` sont **supprimés**,
   une **ligne de date** est ajoutée. ⇒ La carte compte **6 blocs** bandeau éteint, **7** avec —
   ⛔ plus les 7/8 que cette fiche suppose. Le calcul d'accumulation de l'étude d'ergonomie est
   **à refaire** sur les captures du jour.
5. 🔴 **Le texte du bandeau a changé** et ne fait plus « 6 lignes » : il est désormais en **deux
   paragraphes** (le fait, puis l'acte), avec un **chapeau** « Étalonnage modifié ».
6. ⚠️ **Tous les numéros de ligne de cette fiche sont périmés.**

### ⭐ Ce que feature-170 RENFORCE dans ce lot
- **Le bandeau sera éteint la plupart du temps.** Mesuré sur l'appareil le 2026-11-22 :
  `ph_offset_stale: false`. ⇒ La crainte du gate — *« afficher en continu une boîte ambre à propos
  d'une valeur parfaitement juste »* — **tombe**. Rendre la carte permanente n'expose plus une
  alerte perpétuelle, mais un **fait** (« Correction +1,20 pH — posée le 17/10/2026 »).
- **Le chiffrage de l'annulation est confirmé par une voie indépendante** : ADR-0138 établit qu'une
  corruption NVS fait **DISPARAÎTRE** la correction. ⇒ Le scénario « correction absente » n'est pas
  hypothétique, c'est **le seul mode de défaillance atteignable** — et `bug-201 (C41)` le couvre.
- 🔴 **Un QUATRIÈME chemin destructif est désormais VISIBLE** : « Supprimer la correction »
  (`#ph_tune_remove_btn`) était masqué par le bandeau ; le bandeau étant éteint, **il s'affiche**.
  ⇒ `bug-201 (C41)` doit le ferrer **au même titre** que « Remettre à zéro » — la fiche le disait
  déjà, mais le cas est devenu **réel**, plus théorique.

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
- [x] B1 — `#ph-card-tune` visible en permanence sur `#/ph`, étalonnage EZO **inchangé** dans le flux maintenance
- [x] B2 — Entrée **et** sortie du flux maintenance laissent la carte visible
- [x] C1 — `docs/features/page-ph.md`, `docs/DESIGN_SYSTEM.md` (contexte d'emploi de `.flow-maintenance`), `CHANGELOG.md`
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
| **2026-11-14** | *« On supprime ce message ou pas ? »* ⇒ [feature-170](../done/feature-170-retirer-la-confirmation-de-correction-ph.md) ouverte pour **SUPPRIMER** la cause `boot`, avec **GO `pool-chemistry` de 2ᵉ passe** |
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
- **C40** (ex-C0) — ⛔ **ZÉRO ligne dans `src/`.** Les deux clés sont **déjà servies** par appel à l'accesseur
  (`src/web_routes_config.cpp:311-312`, `src/ws_manager.cpp:1695-1696`). Si une modification firmware
  apparaît, **le lot sort du GO**. ⛔ Et aucune réécriture client d'un verdict servi (ADR-0100).
- **C41** (ex-C1) — ⛔ **Un second geste explicite sur TOUT bouton destructif quand `|correction| > 0,50`.**
  ⚠️ **« Annuler la correction » s'appelle désormais « Remettre à zéro »** (`feature-170 (C30)`), et
  **« Supprimer la correction » est maintenant VISIBLE** : les deux sont concernés.
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
- **C42** (ex-C2) — ✅ **LIVRÉE par `feature-170 (C29)`** le 2026-11-22. ⛔ Rien à refaire ici ; conservée pour mémoire.
  ⭐ Le mot « sonde » devait sortir du texte de la cause `calibration`.
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
  ⚠️ **PÉRIMÉ** : `boot` et `expiration` **n'existent plus** (`feature-170 (C25)`/`(C26)`). Il ne
  reste qu'**une** cause, et sa phrase est celle ci-dessus, déjà livrée.
- **C43** (ex-C3) — ⛔ **Aucun bouton d'annulation hors de la page pH.** Un **lien** « Voir la correction »
  depuis l'accueil est acceptable ; un bouton qui **agit** ne l'est pas.
- **C44** (ex-C4) — ✅ **LEVÉE** par l'arbitrage utilisateur du 2026-11-21.

### ⚠️ La fiche est INEXACTE sur un point, à corriger avant de la traiter
⚠️ **Et elle l'est doublement depuis feature-170** : le « drapeau » dont elle parle
(`phOffsetBootUnconfirmed`) **n'existe plus**. Ce qui suit vaut désormais pour l'extinction du
verdict `calibration`, par re-estampillage.
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

### Verdict du 2026-11-18 (NO GO sur l'inhibition du dosage) : **IL TIENT — pour une raison neuve**
⚠️ Son motif d'origine (« le drapeau se lève à chaque démarrage ») est **périmé** : le drapeau
n'existe plus. ⭐ **Mais le NO GO tient, et plus fermement** : inhiber le dosage sur une correction
non confirmée punirait **le seul mode de défaillance que NVS ne produit pas** (ADR-0138 : une
corruption fait DISPARAÎTRE la valeur, elle ne la rend pas fausse-et-grande). ⇒ On inhiberait sur
un risque **inexistant**, au prix d'une panne sanitaire **certaine** en saison.

### Recommandations non bloquantes
- ⭐ Afficher la **date de pose** (`ph_offset_epoch`, **déjà servie des deux côtés et lue nulle part
  dans `data/`**) : « Correction +1,20 — posée le 16/10/2026 ». C'est le C11 de feature-170, gratuit ici.
- ⛔ **Ne rien afficher avant le premier `/get-config`** : une carte qui annonce « aucune correction »
  pendant 300 ms est une invitation à un geste faux.

---

## ⚗️ GATE `pool-chemistry` — PASSE DE CONFIRMATION du 2026-11-22 : **GO SOUS CONDITIONS**

`C40`, `C41`, `C43` **tiennent**. `C42` reste **livrée** par `feature-170 (C29)`. `C44` reste levée.
**Quatre conditions neuves : `C45` → `C48`.** ⚠️ Trois découlent de feature-170 ; la quatrième
(`C46`) corrige **une erreur de rédaction du gate lui-même**, que seules les captures du jour ont
rendue visible.

### ⭐ Ce que `ph_offset_stale: false` change — il RENFORCE la forme imposée
⛔ Il ne l'affaiblit pas. Bandeau éteint, ce que la carte montre en permanence est **exactement**
ce que le gate demandait, et rien de plus : la **ligne de fait** (⛔ aucune boîte, aucune couleur
d'alerte) et le pas-à-pas.
⇒ **Ne sortir QUE la ligne de fait**, en laissant les boutons derrière le péage, donnerait **la
pire combinaison** : l'information permanente, et le geste de **réparation** au prix d'un gel de
dosage. ⭐ C'est l'asymétrie de `C41` **retournée contre l'utilisateur**. **Forme A confirmée.**

### ⭐ Un désamorçage que le gate n'avait pas vu en novembre
Les deux chemins destructifs ⛔ **ne coexistent JAMAIS à l'écran** : `renderPhTuneRemoveRow`
(`data/app.js:2748-2751`) masque `#ph_tune_remove_btn` dès que le bandeau est affiché.
⇒ Sa crainte « deux boutons au même effet avec deux niveaux de friction » **ne peut pas se
produire tant que cette exclusion tient** — d'où `C48`, qui la **verrouille par un test** au lieu
de la laisser au rang d'effet de bord.

---

### 🔴 `C45` — Le texte de friction a DEUX variantes, choisies sur le SIGNE de la correction
⚠️ **Le texte imposé le 2026-11-21 était FAUX pour une correction NÉGATIVE**, que `C41` déclenche
pourtant (son seuil porte sur `|correction|`). Pour `o = −0,60`, retirer la correction fait lire
**plus haut** : l'appareil ajoute **plus** d'acide et fait **baisser** le pH — ⛔ l'exact contraire
de « il cessera d'ajouter de l'acide et laissera le pH monter ».
⭐ *Un texte de friction qui annonce la mauvaise conséquence est **pire que pas de texte** : il
oriente vers le mauvais geste de rattrapage.*

- `ph_offset > 0` — **inchangé, mot pour mot** :
  > « Sans cette correction, l'appareil lira **6,10** au lieu de **7,30** : il cessera d'ajouter de l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel. »
- `ph_offset < 0` — **variante imposée** :
  > « Sans cette correction, l'appareil lira **7,90** au lieu de **7,30** : il ajoutera de l'acide en trop et pourra arrêter le chlore en croyant le pH trop haut. Pour la remettre, il faudra refaire un test manuel. »

L'ergonomie peut raccourcir la proposition centrale **à condition que les deux effets restent
nommés** (acide en trop **ET** arrêt possible du chlore). La dernière phrase est **gelée** dans les
deux variantes.
⛔ **Interdit** d'appliquer la variante positive aux deux signes « parce que le cas négatif
n'existe pas aujourd'hui » : c'est une donnée **persistée**, que l'utilisateur change en six appuis.

### 🔴 `C46` — Les deux nombres sont LUS, au MÊME étage de filtrage, avec un repli OBLIGATOIRE
⛔ **`phRaw` est INTERDIT ici** — et c'est le piège le plus probable, parce que c'est **lui** qui
s'affiche à l'écran (« brut 5.93 » sur la capture A). `src/web_routes_data.cpp:333-337` l'écrit :
*instantané EZO, **dispersion ~0,36 sur ce bassin, jusqu'à ±0,18 d'écart avec le lissé** — deux pas
de réglage*. ⇒ Un couple formé avec lui **se contredirait lui-même** : le texte annoncerait un
écart différent de la correction qu'il décrit.
⚠️ **Vérifié en direct le 2026-11-22** : `phRaw` valait **5,93** à la capture, **5,799** quelques
minutes plus tard — **0,13 d'écart**, plus d'un pas de réglage.

| Rôle dans la phrase | Clé **servie** |
|---|---|
| « il lira **6,10** » | **`phValueRaw`** (`web_routes_data.cpp:338-339`, `ws_manager.cpp:972-973`) |
| « au lieu de **7,30** » | **`ph`** (`web_routes_data.cpp:310-326`, la valeur officielle corrigée) |

⭐ `ph = phValueRaw + correction` **par construction** ⇒ ⛔ **aucune arithmétique côté client**,
ADR-0100 respecté sans discussion. Format : 2 décimales, virgule (`_fmt2`).

🔴 **REPLI OBLIGATOIRE — et ce n'est pas un cas d'école, c'est l'état RÉEL de l'appareil.**
Mesuré le 2026-11-22 : **`ph: null`, `phValueRaw: null`, `phRaw: 5.799`** (filtration arrêtée).
⚠️ Or `#ph_tune_remove_btn` **est cliquable dans cet état** : rien dans `renderPhTuneRemoveRow` ne
dépend d'une mesure. ⇒ **Le texte imposé est inécrivable exactement là où la capture A nous place.**
> « Sans cette correction, l'appareil lira **1,20** pH de moins qu'aujourd'hui : … »
> (« **de plus** » si `ph_offset < 0`), la valeur venant du seul `ph_offset` servi, toujours présent.

⛔ **La friction ne disparaît JAMAIS faute de mesure** (ce serait l'inverse exact du but), et
⛔ **aucun nombre n'est inventé, deviné ni figé en dur.**

### 🔴 `C47` — La forme de la friction : sept exigences chimiques
1. **Deux gestes délibérés**, ⛔ jamais un seul clic depuis l'état au repos.
2. La **phrase de conséquence est à l'écran au moment du second geste** — ⛔ pas seulement avant,
   ⛔ pas dans un toast après.
3. ⛔ **La cible du second geste n'occupe pas la position du premier.** Un bouton qui se
   **transforme sur place** est **REFUSÉ** : ⚠️ à 390 px, un double-appui rapide — *geste courant au
   bord d'un bassin, doigt mouillé* — le franchirait **sans qu'aucune phrase ait été lue**.
4. **Sortie au moins aussi facile que la confirmation**, et **désarmement automatique** à tout
   re-rendu, changement de page ou retour de `/get-config`. ⛔ Un état armé **ne survit jamais** à
   un rafraîchissement.
5. ⛔ Pas de `confirm()` natif · ⛔ pas de mot à saisir · ⛔ pas d'appui long (invisible, non
   découvrable, inaccessible).
6. ⛔ **La friction ne porte QUE sur les deux boutons destructifs.** ⛔ Jamais sur « Corriger »,
   ⛔ jamais sur « Je l'ai vérifiée — garder » : ⭐ ce sont les gestes **vertueux**, et les alourdir
   retournerait contre l'utilisateur **l'asymétrie même qui justifie `C41`**.
7. La phrase est annoncée **une fois** à l'ouverture, ⛔ pas réécrite à chaque trame (même défaut
   que celui corrigé par bug-179).

### 🔴 `C48` — L'exclusion mutuelle est un INVARIANT, pas un effet de bord
⛔ **Exactement UN contrôle destructif visible à la fois.** ⇒ À couvrir **par un test**, ⛔ pas par
une relecture. *Deux boutons au même effet, visibles ensemble, chacun avec sa friction, seraient
**pires** que pas de friction.*

### ✅ Q4 — Sous le seuil : pas de friction, mais la justification n'est **pas symétrique**
- **Correction positive, `|o| ≤ 0,50`** : la lecture reste ≥ 6,70, l'acide **continue** ⇒ aucune
  conséquence immédiate. ⚠️ Le pas valant 0,10, la friction se déclenche en pratique à **0,60**.
- 🔴 **Correction NÉGATIVE : non, ce n'est pas sans conséquence.** Deux effets, dès **`|o| > 0,10`**
  (`kPhHighMargin`, ⛔ **pas** 0,50) : acide en trop (eau réelle sous `kPhLowWarn = 7,00` à
  `|o| = 0,50`), et **chlore suspendu** à un pH réel de `7,60 − |o|`.
- **Pourquoi un seul seuil quand même** : ⭐ les deux effets négatifs vont vers le **repli sûr**
  (plus d'acide ⇒ chlore *plus* actif ; arrêt du chlore ⛔ jamais un surdosage) et sont **bornés par
  `|o|`** et **signalés**. Le cas positif, lui, fabrique un **surdosage de chlore sur une eau qui
  monte**, invisible, sans escalade. ⚠️ Et un seuil à 0,10 sur une grille à 0,10 déclencherait la
  friction sur **quasiment toute correction** — ⭐ *une garde qui bloque plus qu'elle ne protège est
  un défaut*, et c'est le mécanisme même que l'utilisateur a dénoncé.
- ⛔ **En contrepartie** : aucun texte ne doit **affirmer** qu'en dessous du seuil le retrait est
  sans effet.

### ✅ Q5 — Le pas-à-pas permanent : son gate suffit, **rien de plus exigé**
⭐ *Ce que le flux maintenance protégeait ici : **rien**.* Il gelait le dosage pendant le geste et
le dégelait à la sortie **avec la nouvelle correction déjà posée** — même effet, simplement
**différé**. C'est le « péage, pas une garde », et il vaut aussi pour le pas-à-pas.
Ce qui borne réellement une correction fautive : les filets **BRUTS fixes** 5,50 / 8,50 (⛔ ils ne
bougent **jamais** avec la correction), les limites par injection / horaire / journalière, la
grille à 0,10 **sans saisie libre**, l'avertissement `feature-168 (C8)`, le `critical` + alerte
MQTT du firmware, et ⭐ **une régulation en P pur : aucun terme intégral à décharger.**

### ⚠️ Corrections de fiche apportées par le gate
- Le tableau « Faits vérifiés » cite `web_routes_config.cpp:310-311` et `ws_manager.cpp:1691-1692`
  — **périmés**. Les bonnes lignes : `:311-312` et `:1695-1696`.
- `C41` : ⛔ retirer la mention « numériquement égal à `kPhOffsetUnusualAbs` **pour une autre
  raison** ». ⭐ C'est **la même raison** depuis `feature-170 (C23)` ⇒ le déclencheur **lit la clé
  servie** `ph_offset_unusual_abs`, et il n'y a **rien à écrire en commentaire** sur ce point.

### ✅ ARBITRAGE DES QUATRE CONFLITS — 2026-11-22. `C48` amendée, `C49`/`C50`/`C51` neuves.

#### 🔴 `C49` (K1) — « Remettre à zéro » supprimé, **en PAQUET INDISSOCIABLE de trois**
⭐ **Le motif de l'ergonomie est meilleur qu'elle ne le pensait.** « Remettre à zéro » évoque le
**zéro de l'EZO** — ⛔ pas une gêne de vocabulaire, **la confusion la plus coûteuse possible sur
cette page** : le zéro de l'électrode est une **grandeur différente**, et c'est précisément celle
qui est **dégradée sur cet appareil** (128 mV relevés le 11/10, sain < 15). ⇒ Un utilisateur qui
croit « remettre le zéro de la sonde » et **supprime en réalité sa correction de +1,20**
déclenche le scénario de `C41` **en croyant faire un geste de maintenance** — et le bouton est à
deux cartes de celle qui s'intitule « Calibration de la sonde pH ».

🔴 **Mais la forme proposée creusait un trou pire** : `renderPhTuneRemoveRow` masque
« Supprimer la correction » **dès que le bandeau est affiché** (`!staleBannerShown`,
`data/app.js:2748-2751`). ⇒ Supprimer « Remettre à zéro » **sans toucher à cette ligne**
laisserait, bandeau affiché, **un seul bouton : « Je l'ai vérifiée — garder »** — pendant que le
texte dit *« …ou remettez la correction à zéro »*. ⭐ **Un bandeau qui demande de vérifier et
n'offre que de conserver pousse à la confirmation sans vérification.** ⛔ NO GO sur cette forme.

**Le paquet accepté, ⛔ indissociable :**
1. `#ph_tune_stale_cancel_btn` (« Remettre à zéro ») **supprimé** du bandeau ;
2. ⛔ **DANS LE MÊME LOT**, retirer le terme `!staleBannerShown` de `renderPhTuneRemoveRow` ⇒ les
   deux conditions deviennent `offset !== 0`, et « Supprimer la correction » est visible
   **exactement quand le bandeau peut l'être**. ⛔ **Livrer 1 sans 2 est le NO GO ci-dessus** ;
3. le bandeau garde **« Je l'ai vérifiée — garder »**, et le **seul** chemin destructif de la
   carte est le bouton du bas, porteur de la friction `C41`/`C45`/`C46`/`C47`.

⚠️ **Cela AMENDE `feature-170 (C30)`, et le gate le dit explicitement** : son **esprit** est
intégralement préservé (le destructif reste second, reste `btn--danger`, reste précédé du choix
vertueux) — ⛔ seul son **EMPLACEMENT** change : plus un geste destructif *par bloc*, mais **un par
carte**. ⇒ **À inscrire dans les deux fiches.**

⚠️ **Écart assumé, ⛔ à NE PAS « corriger »** : le bandeau dira *« ou remettez la correction à
zéro »* et le bouton *« Supprimer la correction »*. ⛔ Ne pas aligner les mots : cette phrase est
comparée **octet pour octet** entre `src/ph_stale_message.h` et `data/app.js` par
`tools/check_ph_stale_text_parity.sh`. La changer **touche `src/`** ⇒ sortie de `C40`.

#### ✅ `C48` — AMENDÉE, plus forte et plus simple que l'exclusion mutuelle
> ⛔ **Un seul contrôle destructif existe sur `#ph-card-tune`, toujours le même, toujours au même
> endroit, quel que soit l'état de `ph_offset_stale`.** Deux boutons de même effet, visibles
> ensemble **ou alternant de place**, sont interdits. À verrouiller **par un test**.

#### ✅ `C47` (K2) — la forme proposée par l'ergonomie est **déclarée CONFORME**, et préférée
Le gate **préfère U5 à son propre croquis**, et explique pourquoi :

| Forme | Ce que reçoit le **second appui** d'un double-tap |
|---|---|
| **U5** — le bouton est **remplacé** par le bloc | du **texte INERTE**, puis « Ne rien changer ». ⭐ La cible destructive est séparée par le texte **et** par le bouton sûr |
| croquis du gate — le bouton reste | le **bouton d'origine**, qui refermerait le bloc — ⛔ ambigu |

⇒ ⭐ *L'exigence n°3 porte sur **la cible destructive**, jamais sur le bloc : qu'il occupe la place
du bouton est sans danger **tant que ce qui s'y trouve est inerte**.*
Confirmés : focus sur **« Ne rien changer »** (⭐ une barre d'espace parasite ne détruit rien),
Échap et clic extérieur qui referment (**ils vont au défaut sûr**), ⛔ pas de modale, ⛔ pas de
minuterie.
⚠️ **Deux précisions maintenues** : (a) fermer par Échap/clic extérieur ⛔ **ne suffit pas** — l'état
armé doit **aussi** tomber à tout re-rendu et à tout retour de `/get-config` ; (b) la phrase est
annoncée **une fois** à l'ouverture.

#### 🔴 `C50` (K3.1) — `#ph_tune_stakes` : raccourci **ACCEPTÉ**, suppression **REFUSÉE**
⛔ **La phrase « L'écart vient de l'installation, pas de la sonde. » ne se supprime pas.**
⭐ C'est **la seule phrase de l'écran qui empêche d'attribuer l'écart à la sonde** — l'erreur qui a
réellement eu lieu le **25/08/2026** (sonde saine déclarée à remplacer). ⚠️ L'argument « le bandeau
le dit quand ça compte » **s'effondre sur un fait mesuré** : `ph_offset_stale: false`, le bandeau
est éteint **la plupart du temps**, et cette carte devient **permanente**.
⚠️ **Et ce lot AGGRAVE l'exposition** : la ligne « Correction +1,20 pH — posée le 17/10/2026 »
devient visible en continu. ⭐ *Un nombre de cette taille, sans sa cause, **invite** à soupçonner
la sonde.*

**Texte autorisé** (⇒ **6 lignes au lieu de 8**, l'acquis d'incident conservé) :
> « Indiquez la valeur lue sur un test manuel : l'appareil corrigera sa mesure. **L'écart vient de l'installation, pas de la sonde.** »
>
> « **Ce réglage ne vaut que ce que vaut votre test. La valeur que vous indiquez ne change pas que l'affichage : c'est elle qui décidera des injections d'acide.** »

- ✅ **Préservé mot pour mot** : « L'écart vient de l'installation, pas de la sonde. »
- ✅ **Préservé mot pour mot** : le second paragraphe entier. ⛔ Il ne passe **pas** dans un
  `<details>`, ⛔ ne se réduit pas, ⛔ ne perd pas son gras.
- ✅ **Libre** : la proposition d'ouverture (une instruction, ⛔ pas une affirmation chimique).

#### 🔴 `C51` (K3.2) — LE PIÈGE : l'avertissement C8 ⛔ **ne suit PAS** les refus
Le changement de forme (refus en `.field__hint`) **convient**, textes mot pour mot. ⛔ **MAIS** :
les trois refus **partagent le MÊME élément** `#ph_tune_blocked` avec **l'avertissement
`feature-168 (C8)`** (`data/index.html:1454-1475`, « un seul message affiché à la fois,
mutuellement exclusifs »). ✅ **Vérifié ligne à ligne.**
⇒ 🔴 **Dégrader `#ph_tune_blocked` dégraderait AUSSI, en silence, l'avertissement qui dit que la
correction est devenue une pièce du dosage.** ⭐ *Exactement le défaut que ce lot répare, reproduit
à 30 px de distance.*
**Condition** : ⛔ **`feature-168 (C8)` reste un `callout--warn` avec son chapeau et son
repliable.** Le `.field__hint` ne vaut **que** pour les trois refus ⇒ **deux rendus distincts**.
**Test exigé** + **mutation** : forcer les deux branches sur le même rendu ⇒ le test meurt.

**Trois contraintes sur les refus** : ⛔ pas de `.muted` (⭐ *ce sont des REFUS, pas des conseils*) ·
adjacence immédiate au bouton « Corriger » désactivé + `aria-describedby` · annoncé à l'apparition,
⛔ réécrit **seulement s'il a changé** · ⛔ **au moins aussi saillant que le bouton qu'il désactive**
(⭐ *un bouton grisé sans explication lisible est le défaut que ces textes existent pour éviter*).

#### ✅ `K4` — `ph_tune_remove_hint` **reste tel quel**, et l'interdit est BORNÉ
⭐ **L'interdit porte sur l'ATTRIBUTION, pas sur le mot.** Le test, applicable à toute phrase future :
> ⭐ **La phrase accuse-t-elle la sonde, la soupçonne-t-elle, ou invite-t-elle implicitement à la
> remplacer ?** Si non, le mot est permis.

« Sans correction, l'appareil régule sur la mesure de la **sonde** telle quelle » **n'attribue
rien** ⇒ **autorisée**. ⚠️ Un interdit lexical absolu obligerait à renommer « Calibration de la
**sonde** pH » : absurde.
⚠️ **Et la reformulation proposée n'est pas neutre** : « sa mesure **brute** » entre en collision
avec le « brut 5.93 » affiché en tête de page, qui désigne `phRaw` — **un autre étage de
filtrage**. ⛔ Le lecteur pourrait croire que la régulation se fait sur ce 5,93.

#### ✅ Bonus — la pastille « Calibré · critère hors norme »
- ⛔ **Elle reste ROUGE.** Sur cet appareil, c'est le **zéro à 128 mV** (sain < 15) qui la
  déclenche. ⭐ Ce critère **engage le dosage** : une jonction de référence qui se dégrade
  **déplace la mesure de pH**, et cette mesure **décide des injections d'acide** — deux épisodes
  en deux jours, ~2 unités d'écart. ⚠️ **Et depuis la v2.82.0 la borne ±0,50 n'existe plus :
  plus rien ne bute mécaniquement. Cette alerte est ce qui reste.**
- ✅ **Renommage « Étalonnage : critère hors norme » : approuvé** — il nomme le bon objet et lève
  la collision avec la ligne de correction neuve. ⭐ C'est **exactement** le défaut rapporté le
  18/11 : « je ne vois pas l'alerte » devant un écran qui en montrait **une autre**.
- ✅ **Déplacement approuvé, sous limite** : ⛔ la pastille reste visible **sans entrer en
  maintenance** et **sans défilement** ⇒ elle reste dans la carte héros, sur la ligne du bouton
  d'entrée. ⛔ Elle ne descend **pas** sur `#ph-card-calibration`, qui reste derrière le péage.

---

## ✅ POINTS D'EXÉCUTION TRANCHÉS — 2026-11-22 (E1, E2, et la graisse de `C50`)

### `E1` — « désarmement à tout re-rendu » visait le chemin de la CONFIG, ⛔ pas la trame capteurs
⚠️ `renderPhTuneCard()` est appelée **à chaque trame WebSocket**, toutes les quelques secondes.
⇒ Désarmer à chaque appel refermerait le bloc **sous le doigt** : ⭐ *une garde qui bloque plus
qu'elle ne protège*, et qui ferait échouer le geste **vertueux** autant que le destructif.

**Retenu** : l'état armé tombe sur **tout retour de `/get-config`**, **Échap**, **clic extérieur**
et **changement de page**. ⛔ Pas sur une trame capteurs.
⚠️ **Conséquence assumée** : le texte est **figé à l'armement**, avec les `ph` / `phValueRaw` de
cet instant.
⭐ **Pourquoi c'est sans conséquence chimique** : *ce qui porte le sens de la phrase est l'**ÉCART**
entre les deux nombres — et cet écart **EST la correction**, qui ⛔ ne bouge pas pendant
l'armement.* Seules les valeurs absolues peuvent dériver, pas le propos. Et réécrire à chaque
trame violerait le point 7 de `C47`.

🔴 **Le garde-fou exigé, et il est VÉRIFIÉ** : l'état doit tomber si **`ph_offset` lui-même
change** pendant que le bloc est ouvert (autre onglet, MQTT, autre client) — sans quoi la phrase
décrirait une correction qui n'existe plus.
✅ **Vérifié dans le code le 2026-11-22** : la branche `msg.type === 'config'` du WebSocket
(`data/app.js:119-120`) appelle **`loadConfig({ data: msg.data })`**, c'est-à-dire **le chemin même
qui atteint `renderPhTuneRemoveRow` (`data/app.js:4696`) et son désarmement**. ⇒ Rien à ajouter.
ⓘ Risque résiduel nul de toute façon : le POST envoie `ph_offset: 0`, donc l'acte est « plus de
correction » **quelle que soit** la valeur du moment — seul le **chiffre cité** pourrait être
périmé, ⛔ pas l'effet.

### `E2` — Le texte de repli de `C46` est VALIDÉ tel quel
> « Sans cette correction, l'appareil lira **1,20** pH de moins qu'aujourd'hui : il cessera d'ajouter de l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel. »

⭐ La conséquence ne dépend **que du signe**, ⛔ pas de la disponibilité de la mesure ⇒ même clause
par signe que la variante à nombres, « de moins » / « de plus », dernière phrase **gelée**.
🔴 ⛔ **Ce n'est PAS un cas de bord** : filtration arrêtée, `ph` et `phValueRaw` tombent
**ensemble** (même étage, même garde de fraîcheur) — c'est l'état **le plus fréquent hors cycle de
filtration**, et celui de l'appareil au moment de la rédaction. ⇒ **À couvrir par un test au même
titre que la variante à nombres**, ⛔ pas comme un repli de secours.

### `C50` — le GRAS n'était pas une exigence : son retrait est approuvé
⭐ *Le gras de la rédaction du gate marquait la **citation** (« voici les mots qui ne bougent
pas »), ⛔ pas une consigne de rendu. **Ce qui est imposé, ce sont les MOTS.***
⇒ Et le motif du retrait est jugé **chimiquement meilleur** que la mise en forme d'origine : la
seule phrase de cette carte qui doit ressortir est celle qui dit que **cette valeur décide des
injections d'acide**. La noyer dans un encadré **entièrement gras** aurait affaibli exactement le
message que `feature-168 (B4)` protège. ⭐ *Quand tout est en gras, plus rien n'est souligné.*

**Exigence précisée** : « L'écart vient de l'installation, pas de la sonde. » reste **lisible et
non atténuée** — ⛔ pas `.muted`, ⛔ pas de corps réduit — **sans être en gras**. Le gras est
**réservé au second paragraphe**.

### ✅ Défaut attrapé SUR CAPTURE, hors périmètre du gate
La carte devenue permanente, le lien **« ← Retour à la page pH » se retrouvait COINCÉ ENTRE les
deux cartes** en mode réglage, et se lisait comme un **pied de la carte de correction**.
⇒ **Remonté AVANT `#ph-card-tune`** (`data/index.html:994`). ⭐ *C'est exactement le genre de défaut
qu'on ne voit que sur image.*

---

## ✅ REVUE DE CODE — 2026-11-22 : **Approved**, après un premier « Request changes » levé

**12 mutations jouées par le relecteur, 12 tuées.** Les huit points d'examen sont conformes, et il
a vérifié **par exécution** (⛔ pas par lecture) les deux rendus distincts de `C51` et l'absence
d'écriture redondante.

### 🔴 `C40` EST AMENDÉE — le numéro de version est l'EXCEPTION, et elle est écrite
Le premier verdict était **bloquant** sur un seul point : `src/version.h` avait bougé, et `C40`
est rédigée en absolu (*« ⛔ ZÉRO ligne dans `src/` »*). ⭐ **Le relecteur a eu raison de le
bloquer plutôt que de l'interpréter.**

> **Amendement, sur le modèle de celui que le gate a appliqué à `feature-170 (C30)`** :
> `bug-201 (C40)` interdit toute modification de **COMPORTEMENT** firmware. ⛔ Le **numéro de
> version** de `src/version.h` en est **explicitement exclu** : il accompagne la livraison, il ne
> change aucune ligne de logique, aucune clé servie, aucun contrat.

✅ **Vérifié par le relecteur lui-même, ⛔ pas sur parole** : `git diff --stat 4d52d24 -- src/`
rend **un seul fichier, une seule ligne** (`FIRMWARE_VERSION`), ⛔ aucun fichier non suivi dans
`src/`. Les sept clés du lot sont consommées **telles que servies**.
⭐ **Et une vérification qu'il a faite de son chef** : *un bump abandonné en route est un classique
de version à moitié appliquée.* `grep -rn "2\.100\.0"` sur tout le dépôt ⇒ **zéro occurrence**,
et `3.0.0` est cohérent dans les six fichiers qui le citent.

### ⚠️ `3.0.0` est une décision PRODUIT, ⛔ pas une rupture technique — à écrire
`src/version.h:4-7` définit « MAJEUR = changements incompatibles ». ⛔ **Ce lot ne casse aucune
compatibilité** : frontend seul, aucun contrat HTTP / WebSocket / MQTT modifié. Le passage en
3.0.0 est le choix de l'utilisateur (2026-11-22), et il lui revient entièrement.
⇒ **Consigné ici pour que personne ne cherche plus tard la rupture qui l'aurait justifiée.**

### Quatre défauts réels trouvés par la revue — ⛔ aucun n'était couvert par un vecteur
Tous **corrigés dans ce lot**, et renvoyés à `test-engineer` pour verrouillage par mutation :
1. 🔴 **État résiduel** : en sortant de l'avertissement `feature-168 (C8)`, le chapeau gardait
   « Avertissement » et le repliable restait ouvert dans un conteneur masqué — ⛔ exactement le
   « texte qui reste dans l'élément inactif » que `C51` interdit.
2. 🔴 **Honnêteté d'écran** : « Ne rien changer » restait cliquable **pendant l'envoi** ⇒
   l'utilisateur croyait avoir annulé **alors que la suppression partait quand même**. ⭐ *Le même
   défaut que la course corrigée par `await loadConfig()`, à trois lignes de là.*
3. 🔴 **Garde `fail-open` annulée par son site d'appel** : `phTuneRemoveNeedsFriction` échoue vers
   la friction (bon réflexe), ⛔ mais elle n'était consultée que sur un nombre fini — un
   `ph_offset` non numérique tombait **directement sur la suppression, sans friction**.
   ⇒ Inversé : hors nombre fini **on arme**, et l'armement rend `false` ⇒ **clic no-op**.
4. 🔴 **`C51`, l'ordre d'annonce** : le texte était écrit **pendant que l'élément était `hidden`**
   (`display:none` ⇒ hors de l'arbre d'accessibilité) ⇒ ⛔ **aucune annonce**, alors que la
   condition exige « annoncé à l'apparition ». ⇒ `hidden` levé **avant** l'écriture.
Également corrigés : les rejets de promesse non gérés (réseau coupé), le sous-titre du mode
réglage qui annonçait encore la correction, et **tous les `Cxx` nus**.

### ⚖️ Les trois changements de texte HORS conditions — nommés, ⛔ pas tus
La revue a relevé qu'ils ne relèvent d'aucune condition et n'ont **aucun vecteur**, et que deux
d'entre eux sont **incohérents entre eux** sur le périmètre pH/ORP. **Décision : ils restent dans
le lot et sont nommés**, dans la fiche **et** dans le `CHANGELOG` :
| Changement | Périmètre | Pourquoi il est là |
|---|---|---|
| « Injection manuelle » réduit à une phrase | **pH seul** | compensation de la carte permanente |
| « Historique indisponible » masqué | **pH seul** ⚠️ (l'ORP garde le sien) | idem |
| « Atteinte de la cible » raccourcie | 🔴 **pH ET ORP** — fonction **partagée** | idem |
⚠️ **L'asymétrie est assumée et écrite** : elle alimente sinon le piège de parité pH/ORP de ce
projet. ⇒ L'alignement de la page ORP est **hors lot**.

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
