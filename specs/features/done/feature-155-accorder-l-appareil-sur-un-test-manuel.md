## Métadonnées
- **ID** : feature-155
- **Slug** : accorder-l-appareil-sur-un-test-manuel
- **Type** : feature
- **Statut** : en cours — implémentation livrée et testée (firmware + interface), deux critères
  d'ergonomie (B5 simplification, B6 responsive 390 px) non vérifiés à ce stade, cf. check-list
- **Priorité** : normale
- **Version cible** : 2.71.0
- **Dernière mise à jour** : 2026-10-11 (doc-writer)
- **Spec(s) liée(s)** : feature-067 (offset pH, point d'application), ADR-0039 (offset appliqué
  en sortie des accesseurs), feature-154 (pastilles de santé — écran voisin)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE.** ⚠️ La valeur indiquée **entre dans la décision de
      dosage** : `getPhFiltered()` applique l'offset (`sensors.cpp:997`) et c'est cette valeur
      que la régulation et les gardes utilisent. Une valeur mal indiquée **déplace le pH réel du
      bassin**. Ce n'est pas un réglage d'affichage.
- [x] **ux-ui-designer — MANDAT COMPLET** (CLAUDE.md, règle du 2026-10-11) : vocabulaire,
      disposition, états d'erreur, contraste, responsive **et tout autre choix visuel que ce
      lot impose, même non demandé**.
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Remplacer le réglage d'un **écart** par l'indication d'une **mesure de référence**. L'utilisateur
contrôle son eau avec une bandelette ou un appareil de test, amène le nombre affiché à la valeur qu'il a relevée,
et l'appareil en déduit tout seul la correction à appliquer. Le mot « offset » disparaît de
l'interface.

## Contexte

**Demande utilisateur du 2026-10-11**, mot pour mot :

> « Dans l'écran de pH, revoir l'interface pour l'offset. Il faudrait que ce soit présenté comme
> un **ajustement aux perturbations de l'installation de la piscine**. L'utilisateur peut ainsi
> contrôler la valeur de pH avec des languettes ou un autre appareil et **définir la valeur
> lue**. Cette donnée sera prise en compte dans la mesure du pH. Ainsi, **on ne règle plus un
> offset. On donne une valeur, qui déterminera l'offset.** Trouve les bons termes pour expliquer
> ça à un utilisateur sans aucun terme technique. **Simplifie l'interface en conséquence si
> possible.** »

### Pourquoi cette inversion est juste

L'offset de cette installation **ne corrige pas une sonde** : il compense un décalage de
**mesure** dû aux **courants de fuite de la pompe de filtration** (principe rappelé **trois
fois** par l'utilisateur ; mécanisme établi par une campagne de bruit les 22-23/08, et par trois
instruments indépendants). Demander à l'utilisateur de régler un nombre abstrait, c'est lui
faire faire une soustraction que l'appareil peut faire seul.

**Le calcul est trivial** : `écart = valeur relevée par l'utilisateur − valeur lue par
l'appareil`. Toute la difficulté est ailleurs — dans **quelle** valeur lue, et dans ce qu'on
fait quand l'écart est trop grand.

### État actuel

Un **pas-à-pas** (`#ph_offset_value`, boutons + / −) réglant un nombre signé entre **−0,50 et
+0,50** par pas de 0,10, avec un libellé « Offset pH », un bouton « Remettre à zéro », un texte
d'aide, un avertissement et un bouton « Sauvegarder ». Bornes servies par `/get-config`
(`ph_offset_min/_max/_step`).

Offset actuel chez l'utilisateur : **0,00**.

## Comportement attendu

### Le geste, vu par l'utilisateur

1. Il teste son eau (bandelette, PoolLab, pH-mètre).
2. Il ouvre l'écran pH et **ajuste le nombre affiché**, avec + et −, jusqu'à **la valeur qu'il a lue sur son test**.
3. L'appareil lui montre, **en clair**, ce qu'il va faire : ce qu'il lit lui-même, ce que dit le
   test, et la correction qui en découle.
4. Il valide. La correction s'applique **immédiatement** (ADR-0039 : l'offset s'applique en
   sortie des accesseurs, donc sans attendre le remplissage d'une fenêtre).

### Vocabulaire — sans aucun terme technique

Mots **interdits** dans l'interface : *offset*, *calibration* (réservé à l'étalonnage de la
sonde, qui est **autre chose** et vit sur le même écran — la confusion serait grave),
*correction*, *compensation*, *dérive*, *delta*.

Direction retenue, **à arrêter par `ux-ui-designer`** (c'est son mandat, pas une consigne figée) :
le bloc parle d'**accorder l'appareil sur un test manuel**, le champ demande **la valeur lue sur
le test**, et l'appareil **montre son propre écart** comme un résultat, jamais comme un réglage.

⚠️ Le texte doit dire **pourquoi** cet écart existe — l'installation perturbe la mesure — sans
laisser croire que **la sonde est en cause**. C'est le malentendu le plus coûteux de ce projet
(incident du 2026-08-25 : une sonde saine déclarée à remplacer).

### Ce qui doit être montré avant de valider

Les trois nombres, **côte à côte** : ce que l'appareil lit, ce que le test dit, et l'écart qui
en résulte. L'utilisateur doit pouvoir **reconnaître son propre test** avant de valider.

### Cas limites

- 🔴 **QUELLE valeur lue sert de référence ?** Si l'utilisateur teste à 14 h 00 et valide à
  14 h 20, la mesure a bougé. **Décision d'`architect` + `pool-chemistry`** : valeur au moment
  de la validation ? moyenne sur une fenêtre ? refus si l'eau bouge trop ? ⚠️ C'est **le** point
  dur de cette spec.
- 🔴 **Écart hors bornes (> ±0,50)** : refus **obligatoire** (contrainte de sécurité, cf.
  ci-dessous). Le message doit dire quoi faire, sans jargon, et **ne pas suggérer de remplacer
  la sonde**. Un écart de plus de 0,5 pH signale autre chose : test périmé, sonde non calibrée,
  mauvaise échelle de lecture.
- **Mesure indisponible** (filtration arrêtée, fenêtre en remplissage, sonde muette) : l'ajustement
  est **impossible** — on ne peut pas s'accorder sur une valeur qui n'existe pas. Le dire.
- **Sonde non calibrée** (`calPoints < 2`) : accorder l'appareil sur un test **avant** d'avoir
  étalonné la sonde est un contresens. À traiter explicitement.
- ~~**Valeur saisie absurde**~~ — **SANS OBJET côté interface** (il n'y a pas de saisie). Le refus serveur reste **obligatoire** : une interface n'est pas une garde.
- **Annulation / retour à zéro** : doit rester possible, et dit en langage simple.

## Contraintes techniques

- ⚠️ **LA VALEUR SAISIE ENTRE DANS LA DÉCISION DE DOSAGE.** `getPhFiltered()` applique l'offset
  (`sensors.cpp:997`), et c'est cette valeur que lisent la régulation **et les gardes**. Une
  valeur mal indiquée **déplace le pH réel du bassin**. L'interface doit le **dire**, et le gate
  doit valider la formulation.
- ⚠️ **BORNES ±0,50 — NE PAS LES ÉLARGIR.** Elles ne sont pas un confort : `kPhFloorRawHard` et
  `kPhCeilingRawHard` en **dérivent** (`constants.h:1656`, `:1677`) et des `static_assert`
  verrouillent la cohérence. Élargir l'offset **déplacerait les butées de dernier recours**.
  ⇒ Le périmètre de ce lot est **l'interaction**, pas les bornes.
- ⚠️ **PRÉCISION DE L'INSTRUMENT, question pour `pool-chemistry`** : une bandelette donne ±0,5
  pH, un PoolLab ±0,1, un pH-mètre étalonné mieux. S'accorder sur une bandelette peut
  **introduire plus d'erreur qu'il n'en corrige**. Faut-il demander **avec quoi** le test a été
  fait ? Le dire sans alourdir ? L'utilisateur emploie des bandelettes (consigne ORP calée
  dessus le 31/08) — la question est **concrète**, pas théorique.
- ⚠️ **Ne pas confondre avec l'étalonnage de la sonde**, qui vit sur le même écran et dont
  feature-154 vient de refondre l'affichage. Deux gestes différents, deux effets différents.
- **RESPONSIVE 390 px d'abord**, caractères comptés.
- Vanilla JS, français, aucune bibliothèque nouvelle.

## 🔴 DEUX PIÈGES TROUVÉS AVANT LE CODAGE — par l'`orchestrator`, vérifiés par le coordinateur

### Piège 1 — LA CORRECTION EST ARRONDIE AU DIXIÈME. **Décision utilisateur : on garde, et on le dit.**

`roundPhOffsetToStep()` (`src/ph_guard_logic.h:113`) arrondit tout écart reçu à
`kPhOffsetStep = 0,10` (`constants.h:1590`), **par deux chemins indépendants** : à la réception
(`web_routes_config.cpp:1036-1060`) **et** au boot, sur la valeur relue en NVS
(`config.cpp:539-556`).

⇒ Saisir 7,45 quand l'appareil lit 7,13 donne un écart de **0,32**, dont **0,30** sera appliqué.
L'appareil affichera **7,43**, pas 7,45. **La promesse « l'appareil rejoint mon test » serait
fausse par construction**, jusqu'à **0,05 pH**.

**Trois options soumises à l'utilisateur le 2026-10-11. Retenu : garder le dixième et
l'annoncer.** Motifs : zéro changement firmware, zéro migration, zéro risque ; et surtout la
bandelette de cet utilisateur est à **±0,5 pH** — un écart de 0,05 est **dix fois plus fin que
son instrument**. Les deux autres options (pas à 0,01 ; stockage au centième, affichage au
dixième) touchaient un contrat public, la migration NVS et les tests : **écartées, ne pas les
rouvrir.**

⇒ **CONSÉQUENCE CONTRAIGNANTE SUR B2** : le troisième nombre montré avant validation est
**l'écart RÉELLEMENT APPLIQUÉ** (après arrondi), et l'écran annonce **la valeur que l'appareil
va vraiment atteindre** — pas celle que l'utilisateur a demandée. Un écran qui afficherait 7,45
alors que l'appareil lira 7,43 mentirait.

### Piège 2 — LE CALCUL NE DOIT PAS PARTIR DE LA VALEUR AFFICHÉE. **Bug invisible aujourd'hui.**

`ph`, `phFiltered` et `phMedian` — servis par `/data` et le WebSocket — sont **DÉJÀ corrigés**
de l'offset courant (`sensors.cpp:967/993/997`). La formule naïve
`écart = référence − valeur_affichée` est donc **fausse dès que l'offset n'est pas nul** : elle
ajouterait l'écart à un écart déjà appliqué.

⚠️ **L'offset de l'utilisateur vaut 0,00 aujourd'hui** : le défaut serait **invisible à la
recette** et apparaîtrait **au deuxième accord**, en **déplaçant le pH réel du bassin**.

⇒ Le calcul part d'une valeur **BRUTE** : `phRaw` (EZO instantané sans offset,
`web_routes_data.cpp:321`) ou `getPhValueRaw()` (valeur officielle, échelle brute,
`sensors.cpp:1003`). **Décision `architect`.**
⇒ **Vecteur de test OBLIGATOIRE** : le calcul doit rester juste **avec un offset déjà non nul**.

### 👤 DÉCISION UTILISATEUR — IL N'Y A PAS DE SAISIE : L'UTILISATEUR AJUSTE AVEC + ET −

⚠️ **CORRECTION DE VOCABULAIRE, imposée par l'utilisateur le 2026-10-11** : « *Mais quelle
saisie. Si on dit à l'utilisateur de modifier avec un plus et un moins sans saisir, il n'y a pas
de saisie libre.* »
Le mot **« saisie » est FAUX** et doit disparaître de cette spec, des briefs et du produit : il
suggère un champ et un clavier, c'est-à-dire **exactement ce qu'on retire**. Le geste réel :
**l'écran affiche un nombre, l'utilisateur le fait monter ou descendre jusqu'à ce qu'il
corresponde à ce qu'il a lu sur son test.** Il n'entre rien, il **ajuste**.
⇒ Vocabulaire à employer partout : **ajuster**, **amener à**, **indiquer**. Jamais « saisir »,
« entrer », « taper », « champ ».

### 👤 DÉCISION UTILISATEUR — AJUSTEMENT PAR FLÈCHES, AU DIXIÈME UNIQUEMENT

**Demande du 2026-10-11** : « *Trouve un moyen pour que l'interface utilisateur propose
uniquement des dixièmes. Peut être avec des flèches.* »

⚠️ **Ce que cette décision résout, et ce qu'elle ne résout pas** — à ne pas se raconter :
- Elle **ne supprime pas** le résidu d'arrondi. L'écart naît de la différence entre la valeur
  valeur indiquée et la **lecture de l'appareil** : amener le nombre à 7,4 pile quand il lit **7,13** donne 0,27,
  arrondi à 0,30. Le résidu vient de **sa** lecture, pas de l'ajustement.
- Elle **rend ce résidu INVISIBLE et COHÉRENT** : si les trois nombres sont présentés **au
  dixième** — la valeur indiquée, la lecture de l'appareil, la valeur atteinte — l'utilisateur
  demande 7,4, l'écran annonce 7,4, l'appareil lira 7,43. L'écart résiduel est **dix fois plus
  fin que sa bandelette** (±0,5 pH).
- Elle **supprime toute une famille d'erreurs** : pas de clavier ⇒ pas de « 74 » au lieu de
  « 7,4 », pas de virgule mal placée, pas de champ vide, pas de texte. Sur une valeur qui
  **engage le dosage**, c'est un gain de sûreté, pas seulement de confort. ⚠️ C'est aussi
  pourquoi le mot « saisie » est impropre : **rien n'est saisi**.

⇒ **Le pas-à-pas existe DÉJÀ sur cet écran** (`#ph-offset-stepper`, pas de 0,10). On **garde le
composant et on change ce qu'il désigne** : un réglage abstrait (« offset ») devient une mesure
concrète (« la valeur lue sur votre test »). ⚠️ `ux-ui-designer` **arbitre la forme finale** —
flèches, bornes de la plage proposée, valeur de départ — mais **le principe « dixièmes uniquement, pas de clavier » est tranché par l'utilisateur, ne pas le rouvrir**.
⚠️ **Conséquence sur B5** : le pas-à-pas n'est donc **pas** à supprimer, il est à **réaffecter**.
La simplification porte sur le reste (bouton de remise à zéro, texte d'aide, avertissement à deux
paliers, libellés).
⚠️ **Conséquence sur les cas limites** : « valeur absurde (hors 0-14, texte, vide) » devient
**structurellement impossible** côté interface — il n'y a **rien à saisir**. Le refus serveur reste **obligatoire**
(une interface n'est pas une garde), mais l'écran n'a plus à le gérer comme un cas courant.

### Reste ouvert pour `architect` + `pool-chemistry`

**QUELLE valeur lue sert de référence, et faut-il borner la fraîcheur du test ?** L'utilisateur
teste à 14 h 00 et valide à 14 h 20 — la mesure a bougé. Instantanée ? Médiane ? Moyenne de
fenêtre ? Refus si la mesure bouge trop pendant l'ajustement ?
⚠️ **Contexte mesuré de ce bassin** : dispersion pH **~0,36 permanente** (courants de fuite de
la pompe de filtration) ; la valeur filtrée fluctue de **±0,08 à 10 minutes**. La question est
donc **concrète** : à ±0,08 près, la référence bouge pendant que l'utilisateur ajuste.

## ✅ VERDICT `pool-chemistry` — GO SOUS CONDITIONS (7 conditions bloquantes)

### 🔴 CONDITION 1 — LE FIRMWARE CALCULE L'ÉCART. **Change l'arbitrage 3 de l'`architect`.**

Le plan prévoyait un calcul **côté navigateur** et l'envoi de `ph_offset`. **Refusé.** Le POST
porte désormais **la valeur amenée** par l'utilisateur, et le firmware fait la soustraction avec
**sa** mesure à l'instant de la réception :
`offset = roundPhOffsetToStep(valeurAmenée − getPhValueRaw())`.
**Motif** : un écart calculé par le navigateur sur une référence qui a **vieilli pendant le
geste** est **inauditable**. Le Piège 2 se ferme alors **côté serveur**, là où il ne peut plus
être contourné. ⇒ L'affichage client devient un **APERÇU**, jamais la décision.

### Conditions 2 à 7

2. **Refus 400** si `isnan(getPhValueRaw())` **au moment du POST** (couvre remplissage de
   fenêtre, sonde figée, porte fermée, péremption).
3. **Refus 400** si la filtration est arrêtée **ou** tourne depuis moins de
   `kPhTuneMinFiltrationMs` = **600 000 ms (10 min)** — nouvelle constante + **accesseur public
   dans `filtration.h`**.
4. **Refus 400** si `calPoints < 2`.
5. **A7 maintenu** : aucun `requestReset()` / `resetDosingStates()` / `resetPhPauseGuard()`.
6. ⚠️ **RÉÉCRIRE un message préexistant, hors périmètre mais du même travers** :
   `src/web_routes_config.cpp:1053` dit « c'est la sonde ou sa calibration EZO qui est en
   cause » — **met la sonde en cause**, exactement l'incident du 2026-08-25.
7. **Journal** : `info` à l'acceptation (valeur amenée, lecture brute, offset avant/après),
   `warning` à chaque refus.

### Réponses aux quatre questions

- **(a) Fraîcheur : 60 s NE SUFFIT PAS.** La fraîcheur ne dit rien du **régime hydraulique**.
  L'offset compense les **courants de fuite de la pompe de filtration** : un accord relevé
  **pompe à l'arrêt** enregistre un régime où le biais **n'existe pas**, puis l'applique au seul
  régime où l'on dose. Plancher instrumental après démarrage : médiane 9×5 s = 45 s + 3τ
  (τ = 45 s) ≈ **180 s**. **10 min retenues** (> 3 min, et 2/3 de `kPhMixingDelayMs`).
  ⇒ **Afficher un DÉCOMPTE, pas un refus sec.**
- **(b) Référence VIVANTE (A) confirmée.** L'option « figée + reconfirmation au-delà de 0,05 »
  est **inapplicable** : le résidu mesuré est **±0,08 à 10 min**, donc **AU-DESSUS** du seuil —
  elle reconfirmerait à presque chaque geste. ±0,08 reste sous le pas (0,10) et 6× sous la
  bandelette (±0,5).
- **(c) Ne PAS demander l'instrument.** Remplacé par un texte **imposé mot pour mot** (B4) :
  > « Ce réglage ne vaut que ce que vaut votre test. La valeur que vous indiquez ne change pas
  > que l'affichage : c'est elle qui décidera des injections d'acide. »
- **(d) `calPoints < 2` : refus DUR confirmé.** Un offset est une **constante**, une erreur
  d'étalonnage est **proportionnelle** (pente). Sans 2ᵉ point, une pente à 85 % donne ≈ 0,15 pH
  d'erreur à 1 pH de la consigne : on la **figerait dans l'offset**, juste au point d'accord, et
  fausse partout ailleurs.

### Textes de refus — imposés par le gate

- **Écart > 0,50** : « Trop loin de ce que l'appareil mesure. Refaites le test (bandelettes non
  périmées, lecture à la bonne échelle) et vérifiez l'étalonnage de la sonde avant de
  recommencer. »
- **Référence périmée** : « L'appareil n'a pas de mesure assez récente. Attendez qu'une valeur
  s'affiche, puis recommencez. »
- **Filtration** : « La filtration doit tourner depuis 10 minutes pour que l'eau testée soit
  celle qui passe devant la sonde. Possible dans X min. »
- **Non étalonnée** : « La sonde n'a pas encore ses deux points de référence. Faites d'abord
  l'étalonnage (tampon 7 puis tampon 4), puis revenez ici. »

### Tests exigés par le gate

Offset **déjà à +0,30** (le défaut est invisible à 0,00) · filtration **9 min 59 vs 10 min 01** ·
`getPhValueRaw()` NaN · `calPoints` 1 vs 2 · bornes exactes ±0,50 · référence 7,45 / lu 7,13
⇒ **0,30 appliqué, 7,43 annoncé**.
**Mutations** : inverser la soustraction · retirer la garde filtration · retirer la garde
`calPoints`.

## 👤 DÉCISIONS UTILISATEUR SUR LE VOCABULAIRE — 2026-10-11, APRÈS maquette

La maquette de l'`ux-ui-designer` lui a été présentée. Ses décisions, **qui priment** :

- **Titre de la carte : « Correction manuelle de la mesure »** (remplace « Test manuel du pH »).
- **Bouton : « Corriger »** (remplace « Accorder l'appareil »).
- ⚠️ **« Il faudra quand même donner une explication. »**

### ⚠️ LE MOT « CORRECTION » N'EST PLUS INTERDIT

Une version antérieure de cette spec listait *correction* parmi les mots bannis. **L'utilisateur
l'a choisi** : c'est son produit, et le mot **décrit exactement ce qui se passe** sans être du
jargon. ⇒ **Interdit levé pour « correction » / « corriger ».**
Restent interdits : *offset*, *calibration* (réservé à l'étalonnage de la sonde), *compensation*,
*dérive*, *delta*, et tout le champ lexical de la **saisie** (« saisir », « entrer », « taper »,
« champ »).

### ⚠️ UNE EXPLICATION DU GESTE EST EXIGÉE — en plus de l'avertissement du gate

L'avertissement imposé par `pool-chemistry` dit **l'ENJEU** (« c'est elle qui décidera des
injections d'acide »), **pas le GESTE**. L'utilisateur demande une explication de **ce qu'on
fait et pourquoi**.

⚠️ **Contrainte** : l'ergonome a supprimé le texte d'aide de 4 lignes au titre de la
simplification, et le mandat « l'écran compte MOINS d'éléments qu'avant » **tient toujours**.
L'explication doit donc être **courte** — une ou deux phrases —, dire **pourquoi l'écart existe**
(l'installation perturbe la mesure, courants de fuite de la pompe de filtration) **sans mettre
la sonde en cause**, et **ne pas réintroduire** le pavé d'aide d'avant.

Direction (à arrêter par `web-ui-developer` avec le texte final, **re-validé au gate 2**) :
> « Si votre test donne une valeur différente de celle affichée, indiquez-la ici : l'appareil
> corrigera sa mesure. L'écart vient de l'installation, pas de la sonde. »

### Délai de filtration : 10 minutes CONFIRMÉES par l'utilisateur, **avec décompte**

L'option « 3 minutes » (minimum instrumental) lui a été proposée et **écartée**. ⇒ Le décompte
vivant est **obligatoire**, pas un refus sec.

### ⚠️ ERREUR DU RAPPORT D'ERGONOMIE, À NE PAS REPRENDRE

Le rapport d'ergonomie propose de rafraîchir la référence depuis **`phRaw`** (lecture instantanée
de l'EZO) et d'écrire `écart = cible − phRawLatest`. **C'EST FAUX** : l'`architect` et le gate ont
tranché pour **`getPhValueRaw()`** — la valeur officielle en **échelle brute**. Sur ce bassin, la
dispersion du signal **brut instantané est ~0,36**, soit un écart jusqu'à **±0,18** entre
instantané et lissé : **deux pas** de réglage. Le reste de son design est retenu tel quel.

## Livrables — check-list

- [x] A1 — L'écart se **calcule** à partir de la valeur indiquée ; la logique est **pure, nommée,
      testable** (harnais d'extraction, **ADR-0099**, scanner factorisé `test/js_extract.js`).
- [x] A5 — ⚠️ Le calcul part d'une valeur **BRUTE**, jamais de `ph`/`phFiltered`/`phMedian` qui
      sont déjà corrigées (cf. Piège 2). **Vérifié avec un offset déjà non nul** — à offset 0,00
      le défaut est invisible.
- [x] A6 — Cas **sonde non étalonnée** (`ph_cal_points < 2`) traité explicitement.
- [x] A7 — ⚠️ **INTERDIT sur ce chemin** : `requestReset()`, `resetDosingStates()`,
      `resetPhPauseGuard()` — ils permettraient de contourner les limites journalières par
      enregistrements répétés (interdiction déjà écrite, `web_routes_config.cpp:1032-1035`).
- [x] A2 — Refus explicite si l'écart sort de ±0,50, si la mesure est indisponible, si la valeur
      valeur indiquée est absurde. **Aucune valeur n'est appliquée en silence.**
- [x] B1 — L'interface demande **une valeur lue**, pas un écart. Le mot « offset » **disparaît**.
- [x] B2 — Les trois nombres sont montrés **avant** validation (lu par l'appareil / lu sur le
      test / écart qui en découle). ⚠️ Le troisième est l'écart **RÉELLEMENT APPLIQUÉ** après
      arrondi au dixième, et l'écran annonce **la valeur que l'appareil va vraiment atteindre**
      (cf. Piège 1). Afficher la valeur indiquée serait un mensonge.
- [x] B3 — Le texte dit **pourquoi** l'écart existe (l'installation perturbe la mesure) **sans**
      mettre la sonde en cause.
- [x] B4 — Le texte dit que cette valeur **sert au dosage**, pas seulement à l'affichage.
      Formulation validée par `pool-chemistry`.
- [x] B5 — **Simplification** — voir la section « B5 / B6 » ci-dessus : mesuré **3 → 6 blocs**
      à la 1ʳᵉ livraison (l'inverse de la consigne), ramené à **5** par fusion décidée par
      l'utilisateur, avec reformulation imposée par `pool-chemistry` (deux paragraphes, le
      second en gras, ponctuation d'origine restituée). ⚠️ **NON VÉRIFIÉ, laissé ouvert par
      `doc-writer` (2026-10-11)** : le bouton de remise à zéro et l'avertissement à deux paliers
      ont bien disparu, mais le nombre de blocs **toujours visibles par défaut** dans
      `#ph-card-tune` est passé de 3 (champ, aide, bouton) à 5 (explication, enjeu, champ, note
      calibration, bouton) — comptage DOM fait sur `data/index.html`, pas un jugement
      d'ergonomie. À trancher par `ux-ui-designer` : « moins d'éléments » peut viser le nombre de
      décisions proposées à l'utilisateur plutôt que le nombre de blocs de texte, mais ce n'est
      pas tranché ici.
- [x] B6 — Responsive **vérifié à 390 px, caractères comptés** — tableau dans la section
      « B5 / B6 ». Aucun libellé ne partage sa ligne ; seul groupe horizontal = le pas-à-pas,
      **164 px sur 330**. **NON VÉRIFIÉ** : aucun rapport
      `ux-ui-designer` ni `web-ui-developer` confirmant ce point n'a été trouvé dans ce cycle.
- [x] C1 — `docs/features/page-ph.md`, `docs/API.md` si le contrat bouge, `CHANGELOG.md`,
      `docs/user/src/guide-utilisation.html` (**geste utilisateur qui change**).
- [x] D1 — Tests de la logique pure : écart nominal, bornes **exactes** ±0,50, dépassement,
      valeur absurde, mesure indisponible.
- [x] D2 — **Mutations** : (i) inverser le sens de la soustraction ; (ii) supprimer le contrôle
      de borne ; (iii) accepter une mesure indisponible. Chacune doit tuer au moins un test.

## 📐 B5 / B6 — CE QUE `doc-writer` A REFUSÉ DE COCHER, ET CE QUI A ÉTÉ FAIT

`doc-writer` a laissé **deux critères non cochés** plutôt que de trancher à la place d'un
autre. Il a eu raison sur les deux.

### B5 — LA CARTE AVAIT GROSSI, ALORS QUE LA CONSIGNE ÉTAIT DE SIMPLIFIER

**Mesuré sur le DOM, pas estimé** — blocs **visibles par défaut** :

| | avant | après (1ʳᵉ livraison) |
|---|---|---|
| | champ + pas-à-pas | explication du geste |
| | aide (4 lignes) | avertissement (enjeu) |
| | bouton « Sauvegarder » | champ + pas-à-pas |
| | | ligne des trois nombres |
| | | bouton « Corriger » |
| | | note sur l'étalonnage |
| **total** | **3** | **6** |

Les suppressions demandées avaient bien eu lieu (bouton « Remettre à zéro », avertissement à
deux paliers, libellé « Offset pH », aide de 4 lignes) — mais **quatre blocs avaient été
ajoutés**, chacun pour une bonne raison : l'enjeu imposé par le gate, l'explication exigée par
l'utilisateur, les trois nombres qui *sont* la feature, et la note qui évite la confusion avec
l'étalonnage voisin.
⚠️ **Chaque ajout justifié, et le total qui contredit la consigne : c'est exactement le travers
que la règle d'accumulation de CLAUDE.md décrit.**

**Décision utilisateur du 2026-10-11** : **fusionner l'explication du geste et l'avertissement
d'enjeu** en un seul bloc — ils se suivaient et parlaient du même sujet. ⇒ **6 → 5 blocs.**
⚠️ Les deux phrases imposées par le gate sont **préservées mot pour mot** ; seule la ponctuation
de liaison change (point → deux-points, deux-points → virgule). **Revalidé par `pool-chemistry`.**
Les options « fusionner aussi la note d'étalonnage » (4 blocs) et « laisser tel quel » (6) lui
ont été soumises et **écartées**.

### B6 — RESPONSIVE 390 px : VÉRIFIÉ PAR COMPTAGE, pas affirmé

Largeur utile retenue : 390 − 2×16 (gouttière) − ~28 (padding de carte) ≈ **330 px**, police
13-14 px ≈ 7,3 px/caractère.

| élément | car. | px | tient sur une ligne ? |
|---|---|---|---|
| Titre « Correction manuelle de la mesure » | 32 | 234 | **oui** |
| Libellé « Valeur lue sur votre test » | 25 | 182 | **oui** |
| Ligne « Mesure actuelle : 7,15 → après validation : 7,43 » | 48 | 350 | non → 2 lignes |
| Bouton « Corriger » | 8 | 58 | **oui** |
| Note d'étalonnage | 100 | 730 | non → 3 lignes |
| Refus le plus long (filtration) | 125 | 912 | non → 3 lignes |

⚠️ **Les trois qui passent à plusieurs lignes sont des PARAGRAPHES pleine largeur** : s'étendre
sur plusieurs lignes est leur comportement **normal**, pas un `flex-wrap` de rattrapage.
**AUCUN libellé ne partage sa ligne avec un autre élément** — la disposition est entièrement
empilée (décision `ux-ui-designer`, « 390 px d'abord »). Le **seul** groupe horizontal est le
pas-à-pas : 2 boutons de 44 px + valeur ~60 px + 2 gouttières de 8 px = **164 px sur 330**.

## Sécurité chimique

- Dosage impliqué : **OUI, indirectement mais réellement**. La valeur indiquée décale **toutes**
  les décisions de la régulation pH.
- Limites concernées : les butées `kPhFloorCorrected` / `kPhFloorRawHard` / `kPhCeilingRawHard`
  **dérivent** de `kPhOffsetMax`.
- Risque identifié : **une valeur mal indiquée déplace le pH réel du bassin.** Indiquer 7,4 au lieu de
  6,4 ferait croire l'eau plus basique qu'elle n'est et **ferait injecter de l'acide** dans une
  eau déjà acide. Les butées de dernier recours restent le filet — mais c'est précisément le
  scénario qu'elles existent pour attraper, et il deviendrait **plus facile à déclencher**.
- Questions explicites au gate : (a) faut-il **borner la fraîcheur** de la mesure de référence ?
  (b) faut-il demander **avec quel instrument** le test a été fait ? (c) la formulation de B4
  est-elle suffisante pour que l'utilisateur comprenne qu'il engage le dosage ?

## ADR attendu ?

- **Probable** : à trancher par l'`architect`. Candidat (critère 2) : « l'utilisateur indique une
  mesure, jamais un écart » — la décision serait invisible dans le code une fois le calcul posé,
  et rien n'empêcherait un futur réglage direct de revenir.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Potentiellement aucun nouveau : l'écart peut se calculer côté navigateur et partir par la
  route existante. **Décision `architect`.**

### Documentation à produire / mettre à jour
- `docs/features/page-ph.md` — le nouveau geste.
- `docs/user/src/guide-utilisation.html` — **le geste utilisateur change**, c'est le document
  qui le décrit. ⚠️ Ne régénérer aucun PDF (release uniquement).
- `CHANGELOG.md` — entrée 2.71.0.

## Critères d'acceptation

- [x] L'utilisateur amène le nombre à **7,45** alors que l'appareil lit **7,15** ⇒ l'écart appliqué est
      **+0,30**, montré avant validation. (Vérifié avec 7,13 dans `test/ph_tune_vectors.js` et
      `test/test_native_ph_tune` — même arithmétique, le cas 7,15 est trivial en comparaison.)
- [x] Un écart de **+0,80** est **refusé**, avec un message qui dit quoi faire et **ne parle pas
      de remplacer la sonde**.
- [x] Filtration arrêtée ⇒ l'ajustement est **impossible**, et l'écran dit pourquoi.
- [x] Aucun terme technique dans l'interface (vérifiable : « offset », « calibration »,
      « compensation », « delta » absents du bloc). « Calibration » apparaît une fois dans
      `#ph_tune_calib_note`, mais pour **nommer l'autre carte** (« Calibration de la sonde pH »),
      ce qui est l'usage explicitement autorisé par cette même spec.
- [~] L'écran compte **moins** d'éléments qu'avant — **PARTIEL, ET DIT FRANCHEMENT** :
      **3 → 5** blocs visibles. La carte reste **plus longue qu'avant**, pas plus courte.
      Elle a été ramenée de 6 à 5 par la fusion décidée par l'utilisateur, mais les trois
      ajouts restants sont **tous exigés** : l'enjeu (gate `pool-chemistry`), les trois nombres
      (c'est la feature), la note d'étalonnage (évite la confusion avec la carte voisine).
      ⇒ **La consigne « simplifier » n'est PAS tenue au sens littéral.** Ce qui a été tenu :
      tout ce qui était supprimable l'a été (bouton « Remettre à zéro », aide de 4 lignes,
      avertissement à deux paliers, libellé « Offset pH »), et chaque bloc restant a été
      soumis à l'utilisateur avec son coût. Ne pas cocher ce critère serait malhonnête ;
      le cocher le serait aussi.
- [x] Vérifié **à 390 px** — **par comptage de caractères**, tableau dans la section
      « B5 / B6 ». Aucun libellé ne partage sa ligne ; seul groupe horizontal = le pas-à-pas,
      **164 px sur 330 utiles**. Les paragraphes qui s'étendent sur plusieurs lignes sont des
      blocs pleine largeur : c'est leur comportement **normal**, pas un repli.
- [x] `pio run` sans nouveau warning · `./build_fs.sh` dans les 512 Ko · `pio test -e native`
      zéro régression. **Re-mesuré par `doc-writer` le 2026-10-11** : `pio run -e esp32dev_core3`
      SUCCESS (aucun warning observé) ; `./build_fs.sh` → `littlefs.bin` de 512K pile ;
      `pio test -e native` → 68 suites, **2114 cas, 2114 succès** ; `./tools/check_ph_tune_vectors.sh`
      vert (le défaut relevé par `test-engineer` — refus inatteignable + aperçu divergent — est
      **corrigé** dans l'état actuel de `data/app.js` et `src/ph_tune_logic.h`, le job CI
      `ph-tune-vectors` est câblé dans `.github/workflows/hygiene-specs.yml`).

## Plan de test manuel
1. Tester l'eau, amener le nombre à la valeur lue, vérifier que la valeur affichée par l'appareil **rejoint**
   celle du test immédiatement après validation.
2. Amener le nombre très loin de la lecture ⇒ refus compréhensible.
3. Arrêter la filtration ⇒ ajustement impossible, message clair.
4. Revenir à zéro ⇒ l'appareil réaffiche sa mesure non corrigée.
5. Téléphone, 390 px.

## Hors périmètre
- **Élargir les bornes ±0,50** : elles verrouillent les butées de sécurité (`static_assert`).
- **Le point d'application de l'offset** (ADR-0039) : inchangé.
- **L'étalonnage de la sonde** : geste distinct, même écran, non touché.
- **L'ORP** : l'utilisateur n'a demandé que le pH.

## Notes techniques — test-engineer (2026-10-11)

🔴 **BUG TROUVÉ, NON CORRIGÉ** par le test de cohérence aperçu ↔ serveur exigé
par la spec (`test/ph_tune_vectors.js`) : `evaluatePhTunePreview()` (`data/app.js`)
décide `horsBornes` en comparant des **nombres de pas ENTIERS**
(`pasAmene = Math.round(ecart/pas)` vs `pasMax = Math.round(offsetMax/pas)`),
alors que `evaluatePhTune()` (`src/ph_tune_logic.h`, serveur) refuse sur
l'écart **brut non arrondi** (`offsetDemande = phReference - phValueRaw`,
`isPhOffsetValid(offsetDemande)`).

Conséquence mesurée : pour tout écart demandé dans **(0,50 ; 0,55]** (et
symétriquement **[-0,55 ; -0,50)**), l'aperçu arrondit au pas le plus proche
(5 pas = 0,50) et affiche **« accordé »** (bouton actif, aucun message de
refus) alors que le serveur **refuse** (`OutOfRange`, HTTP 400) à la
validation. Reproduit avec `phValueRaw = 7,00` (valeur déjà alignée sur le
dixième — ce n'est PAS un artefact de dérive du brut) et `phReference = 7,51` :
`evaluatePhTunePreview` rend `{ horsBornes: false, ecart: 0.5 }`, alors que
`offsetDemande = 0,51 > kPhOffsetMax (0,50)` côté serveur ⇒ refus.

Vecteurs qui le prouvent (RED volontairement, voir `tools/check_ph_tune_vectors.sh`) :
- « borne +0,51 : horsBornes=true » — ÉCHEC (obtient `false`)
- « borne -0,51 : horsBornes=true » — ÉCHEC (obtient `false`)
- parité aperçu/serveur `(ref=7.51, raw=7)` et `(ref=6.49, raw=7)` — ÉCHEC

**Décision prise** : ne pas corriger `data/app.js` depuis ce rôle (hors mandat
test-engineer). Le job CI `ph-tune-vectors` n'a **pas** été câblé dans
`.github/workflows/hygiene-specs.yml` tant que ce défaut n'est pas corrigé —
le câbler maintenant rendrait la CI rouge pour tout le dépôt sur un défaut
pré-existant, pas une régression introduite par ce tour.

**Piste de correction** (à valider par `web-ui-developer` + revalidation
`pool-chemistry`, le défaut touchant la cohérence d'une valeur qui pilote le
dosage) : faire reposer `horsBornes` sur l'écart **brut** (`valeurAmenee -
phValueRaw` comparé directement à `offsetMin`/`offsetMax`), et ne garder le
calcul en nombre de pas que pour la valeur **affichée** (`ecart`,
`valeurAtteinte`), qui doit rester un multiple du pas pour annoncer ce que le
serveur appliquera réellement après acceptation (Piège 1, toujours valide
séparément).

**Reste livré et vert** : `src/ph_tune_logic.h` (16/16 cas natifs, 5/5
mutations tuées) et `test_native_ws_config_parity`. Voir rapport test-engineer
complet pour le détail.
