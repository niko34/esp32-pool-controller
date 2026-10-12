## Métadonnées
- **ID** : feature-157
- **Slug** : correction-ph-dixieme-et-suppression
- **Type** : feature
- **Statut** : livré — tests verts (2 121 cas), doc à jour
- **Priorité** : normale
- **Version cible** : 2.73.0
- **Dernière mise à jour** : 2026-10-12
- **Spec(s) liée(s)** : feature-155 (la carte de correction), feature-156 (l'écran dédié qui
  l'héberge)

## Domaines touchés
- [ ] Capteurs / Régulation / Web / MQTT / Historique
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [ ] architect — *a priori* non : aucun contrat, aucun calcul nouveau. **À confirmer.**
- [x] **pool-chemistry** — ⚠️ OUI, pour **un seul point** : supprimer une correction est une
      **écriture de `ph_offset`**, donc une valeur qui **engage le dosage**. Le chemin
      (`/save-config` avec `ph_offset: 0`, sans les gardes de `/tune-ph-offset`) a **déjà été
      validé** au gate 2 de feature-156 — il reste à valider **le libellé** et l'absence de
      confirmation.
- [x] **ux-ui-designer — MANDAT COMPLET** (règle CLAUDE.md du 2026-10-11).
- [ ] embedded-developer — non, rien côté firmware.
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Deux corrections sur la carte « Correction manuelle de la mesure » : **n'afficher qu'une seule
décimale**, et **permettre de supprimer la correction**.

## Contexte

**Demande utilisateur du 2026-10-12** : « *Dans l'écran de correction, il faudrait que la
correction ne gère qu'une seule décimale et il faudrait pouvoir supprimer la correction.* »

### (1) Une seule décimale — c'est la règle du projet, pas une préférence

`_fmtPh()` (`data/app.js:2791`) fait `toFixed(2)`. Or la **règle produit du 2026-10-10** impose
**une décimale** pour tous les affichages de pH, hors fenêtre d'état du filtre.
⚠️ **Le `code-reviewer` l'avait signalé en mineur sur feature-155 et ce n'a pas été traité.**

**Et c'est plus juste sur le fond** : le pas vaut **0,10**, l'utilisateur amène un nombre **au
dixième**. Afficher « après validation : 7,43 » expose un **résidu de calcul qu'il ne contrôle
pas**. Tout au dixième, il demande 7,4 et l'écran annonce 7,4 — c'est exactement le
raisonnement tenu le 2026-10-11 en décidant des flèches : *« si les trois nombres sont au
dixième, le résidu devient invisible et cohérent »*.

### ⚠️ PRÉCISION DE L'UTILISATEUR, 2026-10-12 — LA FRONTIÈRE EST L'AFFICHAGE

> « C'est à l'**affichage** et pour les **données choisies par l'utilisateur** que je veux une
> seule décimale. On peut en avoir **deux pour les calculs internes**. »

⇒ **La règle porte sur ce qui se VOIT et sur ce qu'on CHOISIT, pas sur ce qu'on CALCULE.**

| | décimales | exemples |
|---|---|---|
| **Affiché** | **1** | mesure actuelle, valeur atteinte, valeur amenée, pas |
| **Choisi par l'utilisateur** | **1** | la valeur amenée au pas-à-pas (déjà le cas : pas = 0,10) |
| **Calculé / transporté / stocké** | **inchangé** | `phValueRaw` (3 déc.), l'écart, la valeur envoyée au serveur |

⚠️ **CONSÉQUENCE SUR L'IMPLÉMENTATION — elle simplifie le lot** : on **n'arrondit RIEN** dans
la logique. Ni la valeur envoyée au serveur, ni l'écart, ni la référence. **Seul le formatage
final change.** Un arrondi qui remonterait dans le calcul introduirait une erreur réelle là où
on ne voulait qu'un confort de lecture.

⚠️ **Et cela recadre la question de `_fmtPh()`** : c'est une fonction d'**affichage**, donc elle
**est** concernée — mais uniquement pour les appelants qui affichent du pH **soumis à la règle**
(la fenêtre d'état du filtre en est **exclue**). La liste des 10 appelants reste à établir.

### (2) La suppression — une simplification excessive à réparer

⚠️ **Défaut introduit par moi dans feature-156.** Le bouton « Remettre à zéro » a été supprimé
au motif que revenir à zéro était « une position du pas-à-pas ». **C'était faux** : supprimer
une correction est un **geste distinct**, pas une valeur parmi d'autres.

**Conséquence constatée en production le 2026-10-12** : l'utilisateur a une correction de
**+0,50** qui ne sert à rien (l'écart réel est de ~2 unités, voir plus bas) et **aucun moyen
direct de la retirer**. Le seul chemin existant est le bouton « Annuler la correction » de la
bannière « correction caduque » — qui n'apparaît **qu'après un étalonnage**.

## Comportement attendu

- **Toute valeur de pH de cette carte s'affiche avec UNE décimale** : la valeur amenée, la
  mesure actuelle, la valeur atteinte, le pas.
- **Un bouton « Supprimer la correction »** apparaît **uniquement si la correction en vigueur
  n'est pas nulle**, et disparaît sinon (**décision utilisateur** : pas de bouton inerte sur une
  carte qu'on vient d'alléger). Il écrit `ph_offset: 0` par `POST /save-config`.

### Cas limites
- **Correction déjà nulle** : aucun bouton.
- **Suppression pendant que la bannière « correction caduque » est affichée** : les deux gestes
  mènent au même résultat. ⚠️ Ne pas laisser deux boutons dire la même chose côte à côte —
  `ux-ui-designer` tranche.
- **Échec du POST** : la correction reste, le message du serveur est affiché tel quel.
- ⚠️ **Confirmation ou non ?** Supprimer est **réversible** (on peut re-corriger) et **va vers
  le défaut sûr** (plus de correction = mesure nue). Une confirmation serait probablement de
  trop — **à trancher par `ux-ui-designer` et `pool-chemistry`**.
- **Arrondi** : afficher 7,4 pour une valeur réelle de 7,43 est-il acceptable ? **Oui** — le
  résidu (≤ 0,05) est très inférieur à la précision de tout instrument de l'utilisateur
  (bandelette ±0,5). ⚠️ Mais **le dire** quelque part si l'écart devient trompeur.

## Contraintes techniques
- ⚠️ **Ne PAS toucher les textes imposés par `pool-chemistry`** (avertissement d'enjeu, les
  quatre refus, la bannière de correction caduque) — ils sont gelés.
- ⚠️ **`_fmtPh()` est partagé** : vérifier **tous** ses appelants avant de changer son format.
  Certains peuvent dépendre des deux décimales (la fenêtre d'état du filtre est **exclue** de la
  règle du 10/10). Si le partage pose problème, **ne pas muter la fonction commune** — c'est le
  genre de changement qui casse un écran voisin sans qu'on le voie.
- ⚠️ **La suppression passe par `/save-config` avec `ph_offset: 0`** — chemin **déjà validé** au
  gate 2 de feature-156 : les gardes de `/tune-ph-offset` (10 min de filtration, sonde
  étalonnée) **ne s'appliquent pas**, « exiger 10 min de filtration pour **retirer** une
  correction serait une garde qui bloque plus qu'elle ne protège ».
- ⛔ **INTERDIT** : `requestReset()`, `resetDosingStates()`, `resetPhPauseGuard()`.
- **RESPONSIVE 390 px**, caractères comptés. Vanilla JS, français.

## ✅ VERDICT `pool-chemistry` — GO AVEC REFORMULATION (2026-10-12)

Le **chemin d'écriture n'a pas été rouvert** : `POST /save-config` avec `ph_offset: 0`, sans les
gardes de `/tune-ph-offset`, reste validé depuis le gate 2 de feature-156.

### (a) Libellé — « Supprimer la correction » convient, **ne pas l'allonger**
24 caractères, tient à 390 px, et c'est **un geste, pas une valeur**. Mais la conséquence doit
être dite, en **une ligne sous le bouton** (texte imposé) :
> « Sans correction, l'appareil régule sur la mesure de la sonde telle quelle. »

### (b) Confirmation — **NON**
Trois motifs, dont un chimique : l'offset est borné à ±0,50, donc **l'excursion retirée est
inférieure à la largeur de la fenêtre d'exploitation 7,20-7,50** ; le geste est **réversible** ;
et il ramène la régulation sur **la grandeur mesurée**, celle sur laquelle les butées sont
calibrées. *« Les gardes de `/tune-ph-offset` protègent l'AJOUT d'une interprétation, pas son
RETRAIT. »* Conforme au principe « les plafonds protègent, le reste informe ».

### (c) Message après suppression — **factuel, SANS incitation** (texte imposé)
> « **Correction supprimée — la mesure affichée est celle de la sonde.** »

⚠️ **NE PAS écrire « vous pouvez en poser une nouvelle »** : ce serait le piège **symétrique**
de celui déjà évité sur « Garder » — encourager un geste que le gate juge risqué quand
l'instrument de référence est une bandelette (±0,5 pH).

### ⚠️ EFFET DE BORD À DOCUMENTER (recommandation non bloquante)

**Retirer un offset POSITIF abaisse le pH corrigé** et peut le faire **repasser sous
`kPhOrpInhibit` (7,60)**, donc **LEVER l'inhibition ORP** : le chlore peut reprendre.
Comportement **correct** — la mesure nue fait foi — mais à écrire dans
`docs/features/page-ph.md`.
**Cas réel du 2026-10-12** : correction +0,50, pH affiché **7,66** (chlore inhibé). Supprimer la
ramènerait à **~7,15**, donc **sous le seuil** ⇒ inhibition levée. Sans effet immédiat chez
l'utilisateur (ses deux voies sont en **manuel**), mais déterminant s'il repassait en
automatique.

### Tests exigés par le gate
Suppression avec +0,50 en vigueur : le bouton **disparaît**, `/get-config` rend `ph_offset: 0`,
**aucun appel à `/tune-ph-offset`**. Échec du POST : correction **conservée**, **aucun** toast
de succès.

## Livrables — check-list
- [x] B1 — Une seule décimale sur **toutes** les valeurs de pH de la carte.
- [x] B2 — Bouton « Supprimer la correction », **visible seulement si la correction est non
      nulle**.
- [x] B3 — Pas de doublon visuel avec « Annuler la correction » de la bannière.
- [x] B4 — `_fmtPh()` : **vérifier tous les appelants** avant de changer le format ; ne pas
      casser un écran voisin.
- [x] B5 — Responsive **vérifié à 390 px**, caractères comptés.
- [x] B6 — Ligne sous le bouton : « Sans correction, l'appareil régule sur la mesure de la
      sonde telle quelle. » (**texte imposé**)
- [x] B7 — Message après suppression : « Correction supprimée — la mesure affichée est celle de
      la sonde. » (**texte imposé**) ⛔ **Sans** invitation à re-corriger.
- [x] B8 — **Aucune confirmation** avant suppression (verdict du gate).
- [x] C1 — `docs/features/page-ph.md`, `CHANGELOG.md`. ⚠️ **Y écrire l'effet de bord** : retirer
      un offset positif peut **lever l'inhibition ORP** et permettre au chlore de reprendre.
- [x] D1 — Tests sur la logique pure si elle change (harnais ADR-0099, `ph_tune_vectors.js`) ;
      sinon le dire. **Non applicable** : la logique pure (`evaluatePhTunePreview`,
      `test_native_ph_tune`, `ph_tune_vectors.js`) est **inchangée** — seule la fonction de
      formatage `_fmtPh()` (`toFixed(2)` → `toFixed(1)`) et le binding du bouton de suppression
      sont nouveaux, ni l'un ni l'autre n'est de la logique de calcul. `pio test -e native`
      reste à 68 suites / 2 121 cas, zéro régression. ⚠️ **Mais voir « Trou de couverture
      constaté » en fin de spec** : ni `renderPhTuneRemoveRow()` ni `_fmtPh()` ne sont couverts
      par un harnais du dépôt — la validation par mutation a été faite hors suite.

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — supprimer une correction **modifie la valeur sur
  laquelle la régulation décide**.
- Risque : **faible et orienté vers le défaut sûr** — retirer une correction ramène à la mesure
  nue de la sonde, et la suppression est **réversible**.
- Questions au gate : (a) le libellé « Supprimer la correction » convient-il ? (b) une
  **confirmation** est-elle nécessaire, ou serait-elle une friction inutile sur un geste
  réversible ? (c) faut-il un message particulier après suppression ?

## ADR attendu ?
- **Probable** : non.

## Éléments visibles (pour doc-writer)
- `docs/features/page-ph.md` — le format d'affichage et le nouveau bouton.
- `CHANGELOG.md` — entrée 2.73.0.
- **Aucun** endpoint nouveau, **aucun** champ nouveau.

## Critères d'acceptation
- [x] Aucune valeur de pH de cette carte n'affiche deux décimales.
- [x] Avec une correction de +0,50 en vigueur, le bouton de suppression est **présent** ; après
      l'avoir utilisé, il **disparaît** et la mesure affichée redevient la mesure nue.
- [x] Les textes gelés par le gate sont **inchangés**.
- [x] Aucun écran voisin n'est cassé par le changement de format.
- [x] Vérifié **à 390 px**.
- [x] `./build_fs.sh` dans les 512 Ko (512K exact) · `pio test -e native` zéro régression
      (68 suites / 2 121 cas, mesurés le 2026-10-12).

## Plan de test manuel
1. Avec une correction en vigueur : le bouton est là, la suppression fonctionne, il disparaît.
2. Sans correction : pas de bouton.
3. Parcourir les écrans voisins (état du filtre, consigne, calibration) : aucun format cassé.
4. Téléphone, 390 px.

## Hors périmètre
- Les **textes imposés** par `pool-chemistry`.
- Le **pas de 0,10** et les **bornes ±0,50** (verrouillent les butées de sécurité).
- L'écart **matériel** entre la sonde et la mesure de référence de l'utilisateur — ⚠️ **ce n'est
  pas un défaut logiciel** (décision explicite du 2026-10-12 : « *c'est un problème de sonde et
  donc de matériel, pas logiciel* »). Aucun bug ouvert pour cela.

## Notes techniques de clôture (2026-10-12)

### Les deux questions que la revue laissait ouvertes — tranchées

**1. « Un offset hérité de 0,03 afficherait +0,0 avec un bouton actif. » → CAS IMPOSSIBLE.**
Soulevé comme risque théorique par `code-reviewer`. Vérifié : tout `ph_offset` est ramené à un
multiple du pas par `roundPhOffsetToStep()` à **trois endroits indépendants** — à la réception
par `/save-config` (`web_routes_config.cpp`, après le refus de plage), à la réception par
`/tune-ph-offset`, et **au chargement de la configuration** (`src/config.cpp:616`). Ce dernier
est le point décisif : même une NVS écrite par une version antérieure est normalisée **au
démarrage**. Une valeur de 0,03 ne survit pas à un boot, elle devient 0,00 — donc le bouton est
masqué, et non « actif sur un +0,0 ». Aucun correctif nécessaire.
⚠️ Consigné ici pour ne pas relitiger : le raisonnement « la valeur affichée est arrondie, donc
elle peut mentir » est juste **en général** (c'est la racine des quatre défauts de feature-155),
mais il ne s'applique pas ici parce que la valeur STOCKÉE est elle-même contrainte.

**2. « L'écran de calibration EZO doit-il aussi passer à une décimale ? » → NON.**
La règle existait **avant cette spec**, dans `docs/subsystems/pump-controller.md` : « *Une
décimale pour un pH de mesure ou de seuil ; deux pour un réglage ou une bande de diagnostic.* »
Les valeurs de l'écran de calibration sont des **valeurs de tampon** (4,00 / 7,00 / 10,00) et des
grandeurs de diagnostic — pas des mesures. Elles gardent deux décimales. feature-157 n'étend donc
le format à une décimale qu'aux valeurs de **mesure** et à la **correction choisie par
l'utilisateur**, conformément à sa propre précision : « *C'est à l'affichage et pour les données
choisies par l'utilisateur que je veux une seule décimale. On peut en avoir deux pour les calculs
internes.* »

### Correctifs mineurs de revue appliqués

- **Le motif d'échec est désormais NOMMÉ.** Les trois boutons qui écrivent `ph_offset`
  (« Supprimer la correction », plus « Annuler » et « Garder » de la bannière « correction
  caduque ») passaient par `sendConfig`, qui ne rend qu'un booléen, et affichaient « réessayez ».
  ⇒ `sendConfigDetailed`, et affichage de `res.message || res.error`.
  ⚠️ **Le gain n'est pas celui qu'on croit** : le refus de PLAGE est hors d'atteinte sur ces trois
  boutons (0 et la valeur courante sont toujours valides). Ce qui arrive réellement, c'est le
  **contrôleur injoignable** — et « réessayez » invitait alors à répéter un geste qui échouera à
  l'identique. Les deux boutons de la bannière viennent de feature-156 : ils sont corrigés ici
  **parce qu'ils vivent dans la même carte** ; en laisser deux dire « réessayez » et un seul dire
  la vérité aurait créé l'incohérence que la revue cherchait à éviter.
- **`aria-label` du pas-à-pas** : les valeurs statiques du HTML disaient « 0,10 » alors que le JS
  écrit « 0,1 » dès le premier rendu. Un lecteur d'écran atteignant la carte avant ce rendu
  annonçait un pas périmé. Alignés sur « 0,1 ».
- **`aria-describedby` du bouton destructif** : ne pointait que vers la **valeur** supprimée
  (`ph_tune_remove_label`), jamais vers la **conséquence** (`ph_tune_remove_hint`). Les deux,
  dans l'ordre de lecture.
- **Séparation du bouton destructif** : classe locale `.action-destructive` —
  `margin-left:auto` en vue large (rejeté à l'opposé de l'action principale), `margin-top:12px`
  sous 768 px, où les boutons s'empilent en pleine largeur et où les 8 px de `gap` ne suffisent
  pas pour un pouce. ⚠️ Classe **locale** et non règle sur `.detail-actions` : ce conteneur sert
  sur tout le produit, lui imposer cet écart déplacerait des écrans hors périmètre.

### ⚠️ TROU DE COUVERTURE CONSTATÉ — à ne pas confondre avec « testé »

**Le critère D1 était coché « non applicable » au motif que la logique pure est inchangée.
C'est exact mais INCOMPLET**, et le dire ainsi laisserait croire que ce lot est protégé.

`test-engineer` a cherché un harnais couvrant les deux fonctions que feature-157 introduit ou
modifie — `renderPhTuneRemoveRow()` (condition d'affichage du bouton + masquage mutuel avec la
bannière) et `_fmtPh()` (le `toFixed(1)` qui EST la feature. **Résultat : aucun.** Les quatre
harnais JS existants (`ph_tune_vectors.js`, `ph_health_vectors.js`, `trace_logic_vectors.js`,
`sha256_vectors.js`) extraient d'autres fonctions. Aucun `tools/check_*.sh` ne touche celle-ci.

**La validation par mutation a donc été faite HORS SUITE**, par un script jetable du scratchpad
(non committé) utilisant le même `test/js_extract.js`. Les trois mutations attendues divergent
bien de la référence :
- `!staleBannerShown` → `staleBannerShown` : le masquage mutuel s'inverse ;
- `offset !== 0` → `true` : bouton visible sur une correction nulle ;
- `_fmtPh` → `toFixed(2)` : « +0,30 » au lieu de « +0,3 ».

⇒ **La logique est discriminante — mais rien dans le dépôt ne tuerait ces mutants demain.**
C'est exactement la situation que la règle de mutation de `CLAUDE.md` décrit : *« une suite qui
ne tue aucun mutant ne prouve rien »*, ici poussée d'un cran — il n'y a pas de suite du tout.

**Le risque concret n'est pas théorique** : `_fmtPh()` est une fonction PARTAGÉE. Un futur lot
qui la ramènerait à `toFixed(2)` pour les besoins d'un autre écran annulerait feature-157 **en
silence**, sans qu'aucun test ni aucune revue ne le signale — et c'est précisément le motif de
défaut qui a produit les quatre erreurs de feature-155.

**⛔ Non traité ici, délibérément** : créer `test/ph_tune_remove_vectors.js` sur le patron de
`ph_health_vectors.js` + un 15ᵉ script d'hygiène est une addition STRUCTURELLE (nouveau job CI),
hors du périmètre d'une spec d'affichage. **À arbitrer par l'utilisateur**, pas à glisser dans
ce lot.
