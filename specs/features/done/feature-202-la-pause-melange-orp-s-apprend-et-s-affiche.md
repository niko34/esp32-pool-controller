## Métadonnées
- **ID** : feature-202
- **Slug** : la-pause-melange-orp-s-apprend-et-s-affiche
- **Type** : feature
- **Statut** : done — ⚠️ E1 et H1 en attente d'observation terrain (voir Notes d'implémentation)
- **Priorité** : moyenne
- **Version cible** : **3.2.0** (mineure — comportement de régulation nouveau et visible ; 3.1.3 livrée au 2026-11-23)
- **Dernière mise à jour** : 2026-11-23 — fiche **revérifiée contre le code** (trois corrections marquées
  « CORRECTION DU 2026-11-23 ») **puis la CONCEPTION a été REFAITE et validée par l'utilisateur le
  même jour** — voir « Conception retenue » ci-dessous. ⛔ Les § 1 à 3 du 19/11 (seuil en mV,
  médiane 7 jours, minimum 3 jours) sont **remplacés**, pas amendés.
- **Spec(s) liée(s)** : [feature-085](../done/feature-085-dose-de-base-apprise-feedforward.md) (le patron d'apprentissage à copier) · [feature-025](../done/feature-025-lissage-mesures-ph-orp-pid.md) (la pause créée) · [bug-200](../done/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md) (le principe de visibilité qui s'applique ici)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP / WebSocket
- [x] Frontend (`data/`)
- [ ] MQTT / Home Assistant

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry *(✅ **P1 sur le DESIGN fait le 2026-11-23**, trois tours, conception arrêtée
  et validée par l'utilisateur — ⚠️ **P2 sur le DIFF reste dû**)*
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Après une injection de chlore, l'appareil s'interdit de réinjecter pendant une **pause mélange**
de **20 minutes**, identique sur tous les bassins. Sur ce bassin, l'effet d'une injection met
**~88 minutes** à se produire entièrement. La pause autorise donc une seconde injection alors que
la première n'a exprimé qu'un quart de son effet — c'est un empilement de doses.

Cette fiche fait **apprendre** cette durée à l'appareil, à partir de ce qu'il observe sur son
propre bassin, et la rend **visible**.

## Contexte
Demande de l'utilisateur du 2026-11-19, après le diagnostic de l'injection du 2026-11-18.

### Les chiffres relevés sur l'appareil de production (2026-11-18)
| Fait | Valeur |
|---|---|
| Déficit au départ | 479 mV mesurés pour 530 de consigne → **51 mV** |
| Fin de l'injection | 09:59:55, arrêtée par le **plafond horaire**, pas par l'atteinte de la cible |
| Volume injecté | **456 mL** |
| Mesure au sommet | **585 mV à 11:28** |
| ⭐ Durée de montée **après la fin** de l'injection | **~88 min** |
| Pause mélange en vigueur | **20 min** (`kOrpMixingDelayMs`, `src/constants.h:2339`) |

⇒ À 10:20 l'appareil se croyait autorisé à réinjecter. Si la mesure était restée sous la consigne
à cet instant — ce qui était le cas —, il l'aurait fait **par-dessus une dose encore en train de
monter**.

⚠️ **Ce que cette fiche NE corrige PAS** : le dépassement du 18/11 vient de la **taille de la
dose** (456 mL pour un besoin d'environ 220), pas de la pause. La pause n'a fait qu'éviter que ce
soit pire. L'apprentissage du rendement (mV par mL) est un **levier distinct**, à traiter
séparément. ⛔ Ne pas laisser croire que ce lot supprime les dépassements.

### Décisions déjà arrêtées par l'utilisateur (2026-11-19) — ⛔ ne pas les rouvrir
1. **Les noms actuels sont conservés.** « Pause mélange » reste « Pause mélange », et le bloc
   « Dernière pause » des réglages reste tel quel. ⛔ Aucun travail de vocabulaire dans ce lot.
   ⚠️ **PRÉCISION DU 2026-11-23** : le bloc « Dernière pause » (`data/index.html:2636`,
   `id="measures_purge_last"`) ne parle **PAS** de cette pause-ci — c'est l'attente après un
   **arrêt d'eau** (feature-091/139), un autre mécanisme sur un autre écran. ⇒ La décision reste
   valable (on n'y touche pas), mais ⛔ ce n'est **pas** la surface d'affichage de ce lot : la
   pause mélange n'existe aujourd'hui que comme **compte à rebours transitoire**
   (`data/app.js:6300`, « Pause mélange · M min »), ⛔ jamais comme une durée consultable.
2. **L'apprentissage ne peut qu'ALLONGER la pause**, jamais la raccourcir sous les 20 minutes
   d'aujourd'hui.
3. **La durée en vigueur doit être affichée.**

## Comportement attendu

## Conception retenue — arrêtée le 2026-11-23, validée par l'utilisateur

⭐ **Le mécanisme tient en trois phrases**, et n'introduit que **DEUX nombres neufs**.

1. **Après chaque injection de chlore, l'appareil surveille sa mesure ; dès qu'elle n'est plus
   au-dessus de ce qu'elle valait une pause plus tôt, il note combien de temps la montée aura
   duré.**
2. **La pause en vigueur est la plus longue des trois dernières montées observées — jamais moins
   que les 20 minutes d'aujourd'hui, jamais plus que le plafond.**
3. **Si la montée n'en finit pas, ou si quoi que ce soit perturbe la fenêtre (filtration arrêtée,
   autre injection, mesure invalide), l'observation est jetée et rien ne change.**

### ⛔ Les deux contraintes de conception qui ont fait rejeter les versions précédentes
Posées par l'utilisateur le 2026-11-23, elles s'appliquent **à tout ce lot** :

1. **Rien ne doit dépendre du bassin de l'utilisateur.** « *C'est un appareil destiné à être
   installé sur n'importe quelle piscine.* » ⛔ Apprendre d'un bassin pour figer un critère **en
   dur dans le code** est une erreur de méthode, pas un réglage à affiner. C'est le principe déjà
   écrit au projet : *physique et chimie sont universelles, un seuil calé sur un bassin ne l'est
   pas.*
2. **Le système doit être simple, voire simpliste.** « *Il n'est pas possible de produire des
   systèmes à 5 seuils. C'est beaucoup trop complexe, devient incompréhensible. C'est la porte
   ouverte à des effets de bord et dans le temps cela rend la maintenance risquée.* »

⚠️ **Ces deux contraintes sont des critères de REVUE**, pas un préambule : une proposition qui
ajoute un seuil chiffré doit dire d'où il sort **sans citer ce bassin**, ou être refusée.

### 1. Observer — comparer la mesure à ELLE-MÊME, pas à un seuil
À la fin de chaque session d'injection ORP, l'appareil ouvre une **observation**. Il compare
périodiquement la mesure filtrée à celle qu'il avait relevée **une pause plus tôt**. Dès qu'elle
n'est **plus au-dessus**, la montée est considérée terminée : le temps écoulé depuis la fin de
l'injection est l'observation.

⭐ **Ce que ce critère élimine — c'est là qu'est la décision :**
- **Zéro seuil en millivolts.** Rien à calibrer, rien à recalibrer.
- **La fenêtre de comparaison est `kOrpMixingDelayMs`** (20 min) — ce n'est pas un nombre neuf,
  c'est celui qui existe déjà.
  ⛔ **CORRECTION DU 2026-11-23 (architecte)** : c'est la **CONSTANTE**, ⛔ **jamais la durée en
  vigueur**. Si la fenêtre suivait la durée apprise, toute observation future serait
  mécaniquement ≥ la durée courante ⇒ **cliquet irréversible**, la pause ne pourrait plus
  jamais redescendre. Avec une fenêtre constante, le « maximum des trois dernières » reste un
  vrai maximum **glissant**. ⭐ À trancher dans l'ADR-0145.
- **Le bruit de mesure n'a pas à être mesuré.** La bascule se produit d'elle-même quand la montée
  réelle tombe au niveau du bruit **de cette installation-là** : une installation silencieuse
  observe plus longtemps, une installation agitée s'arrête plus tôt. ⚠️ Le critère s'auto-échelonne,
  il n'est pas « tolérant au bruit » par un seuil posé à la main.
- **Aucune observation ne peut structurellement être plus courte que la pause en vigueur**, puisque
  c'est la durée même de la comparaison. ⇒ Le plancher de 20 min est tenu **par construction**.

⛔ **Piste écartée le 2026-11-23 — à ne pas rouvrir** : attendre que la mesure **franchisse la
consigne**. Trois objections, chiffrées sur le relévé du 18/11 :
1. Elle mesure le **surdosage**, pas le mélange — plus la dose est excessive, plus le
   franchissement est précoce, donc **plus la pause raccourcit**. Le mécanisme joue à l'envers.
2. Le 18/11 (déficit 51 mV pour une montée totale de 106 mV, franchissement à 48 % de la montée)
   elle aurait rendu **~22 minutes**, écrêtées au plancher ⇒ **inerte sur le cas même qui motive
   cette fiche**.
3. Elle n'apprend que des injections où la pause est **déjà inutile** (la bande morte interdit de
   réinjecter au-dessus de la consigne) et applique ce résultat aux injections **insuffisantes** —
   exactement là où l'empilement se produit.

⛔ **IMPOSSIBLE de reconstruire cette courbe depuis l'historique, et c'est vérifié** :
`src/history.h:79` fixe `RAW_MAX_AGE = 21600` — les points à 5 minutes ne vivent que **6 heures**,
après quoi ils deviennent des **moyennes horaires** (`HOURLY_MAX_AGE`, 15 jours). Une montée de
88 minutes s'y réduit à deux points. ⇒ L'observation se fait **en direct**, et seul le **résultat**
est persisté — exactement le choix de feature-084 (`src/ph_persist_logic.h`), qui persiste un
instantané compact en NVS plutôt que de réinterroger l'historique.

⚠️ **Une observation ne compte que si elle est propre.** Elle est **jetée** si, pendant la fenêtre :
la filtration s'est arrêtée · une seconde injection (pH **ou** ORP) a eu lieu · une attente eau
neuve s'est déclenchée · la mesure est devenue non représentative (`getOrpFiltered()` rend **NaN**
si la sonde n'est pas amorcée, est figée ou périmée — `src/sensors.h:142` : l'invalidité est déjà
portée par l'accesseur officiel, ⛔ rien à réinventer). ⇒ Le projet a déjà été trompé par une
observation contaminée (matin du 05/09, air dans le circuit) : **mieux vaut une journée manquante
qu'une journée fausse**, règle déjà écrite dans `src/time_to_target_logic.h`.

### 2. Apprendre — le MAXIMUM des trois dernières, ⛔ pas une médiane
La durée retenue est la **plus longue des trois dernières observations propres**.

⭐ **Pourquoi le maximum** : la détection se déclenche au **premier creux du bruit**, donc chaque
observation est **biaisée court**. Prendre la plus longue corrige ce biais, et c'est le seul
estimateur cohérent avec la décision utilisateur « l'apprentissage ne peut qu'**allonger** ».
⛔ Une médiane ou une moyenne mélangerait un biais systématique à une dispersion aléatoire.

⭐ **Pourquoi TROIS observations, et ⛔ plus aucune fenêtre en jours** : la grandeur apprise est
**hydraulique** (volume du bassin, débit de filtration, point d'injection) — elle ne varie ni d'un
jour ni d'un mois. Et il n'y a **qu'une observation par injection** : le 18/11 l'appareil n'a
injecté **qu'une seule fois**, donc « 7 jours » n'aurait rien mesuré de plus que « quelques
observations ». ⇒ On compte en **observations**, jamais en jours.
⛔ **La médiane 7 jours / minimum 3 jours / empreinte de contexte de la rédaction du 19/11 est
RETIRÉE.**

⇒ **Sous 3 observations propres, le comportement est RIGOUREUSEMENT celui d'aujourd'hui** : 20 min.

### 3. Appliquer — un seul point de code, et un plafond exprimé en MULTIPLE du plancher

    duréeEnVigueur = min( max(3 dernières observations), kOrpMixingDelayLearnedMaxMs )

- Le **plancher** est `kOrpMixingDelayMs` (20 min). Il est tenu par construction (§1), l'écrêtage
  bas reste en ceinture-bretelles et doit être **testé** (D2).
- Le **plafond** `kOrpMixingDelayLearnedMaxMs` est le **seul nombre vraiment neuf**, et il s'écrit
  **`6 * kOrpMixingDelayMs`** — ⚠️ en **multiple du plancher**, ⛔ jamais en minutes absolues : il
  vaut numériquement 120 min, mais il est **déduit d'une constante du code**, pas d'un bassin.
  Son rôle est un garde-fou contre une observation **aberrante** (mesure qui ne plafonne jamais,
  fuite, sonde dérivante), ⛔ pas une protection saisonnière — voir le chiffrage de débit maximal
  plus bas, qui a retiré cette inquiétude.

⭐ **Le plancher fait TROIS métiers avec un seul nombre** : durée minimale de la pause, **fenêtre de
comparaison** de l'observation, et borne basse structurelle de toute observation. C'est ce qui
permet au mécanisme de tenir en trois phrases.

La logique pure (`src/dosing_window_logic.h:298`) reçoit **déjà** la durée en **paramètre**
(`fullMixingDelayMs`), alimentée par un tableau à `src/dosing_window.cpp:96` :
`const uint32_t fullMixing[2] = {(uint32_t)kPhMixingDelayMs, (uint32_t)kOrpMixingDelayMs};`
⇒ **Rien à changer dans la logique de fenêtre** : seule la valeur qu'on lui passe change.

### ⚠️ CORRECTION DU 2026-11-23 — il y a TROIS points d'écriture, pas deux
La rédaction du 19/11 affirmait que la pause n'était armée qu'à deux endroits. ⛔ **C'est faux**, et
le troisième change la conception. Relevé le 2026-11-23 :

| # | Emplacement | Rôle |
|---|---|---|
| 1 | `src/pump_controller.cpp:548` | armement pH à la fin d'une injection |
| 2 | `src/pump_controller.cpp:552` | armement ORP à la fin d'une injection |
| 3 | ⭐ `src/pump_controller.cpp:365` | **restauration** : `_mixingEndMs[ch] = r.mixingEndMs;` |

Le point 3 est alimenté par l'instantané écrit en `:3704` (`c.mixingEndMs = _mixingEndMs[ch];`).
⇒ **La pause survit DÉJÀ à un redémarrage**, par un mécanisme que la fiche ignorait.

⚠️ **Et la logique de restauration RÉARME EN PLEIN** : `dosing_window_logic.h:364` fait
`rem = fullMixingDelayMs;` quand la pompe tournait au moment de l'écriture (cas C5). ⇒ Si la durée
apprise change entre la sauvegarde et la restauration, c'est la **NOUVELLE** qui s'applique à une
pause déjà commencée.

⛔ **À trancher** (ajouté aux points P1) : la pause en cours au redémarrage est-elle restaurée avec
la durée **qui l'a armée**, ou avec celle **en vigueur à la restauration** ? ⚠️ Les deux sont
défendables — la seconde est le comportement actuel, et elle ne peut qu'**allonger**, donc elle
reste du côté prudent. Mais c'est un choix, ⛔ pas un détail d'implémentation.

⛔ **Ce lot ne touche QUE la voie ORP.** La pause pH (15 min, `kPhMixingDelayMs`) reste
**strictement inchangée** : son terme intégral est verrouillé dessus par des `static_assert`
(`src/constants.h:2993`, `:3121`, `:3831`, `:3835`, `:3886`). ⚠️ La rendre variable casserait ces
preuves — c'est un autre chantier, pas celui-ci.

#### ⚠️ CORRECTION DU 2026-11-23 — l'ORP AUSSI porte un `static_assert`
La rédaction du 19/11 laissait croire que seul le pH était verrouillé, et s'en servait pour
justifier l'exclusion. ⛔ **Incomplet.** `src/constants.h:3221` :

    static_assert(uint32_t(kSensorFrozenSamples) * kPhOrpSensorIntervalMs <= kOrpMixingDelayMs,
                  "Detecteur de sonde figee : N x cadence depasse la pause melange ORP");

Il exige que la fenêtre du **détecteur de sonde figée** reste **plus courte** que la pause mélange
ORP — sans quoi la sortie fail-closed du détecteur pourrait courir plus longtemps qu'une pause
légitime et **se confondre avec elle** (garde exigée par le gate `pool-chemistry`, feature-152).

⭐ **L'apprentissage va dans le BON sens** : c'est une borne **basse**, et la durée apprise ne peut
qu'**allonger**. L'invariant est donc préservé, et même renforcé.
⛔ **MAIS il porte sur la CONSTANTE, pas sur la durée EN VIGUEUR.** Dès que celle-ci devient
variable, ce `static_assert` ne prouve plus rien sur ce qui tourne réellement. ⇒ **Livrable
ajouté (A6)** : l'invariant doit être re-prouvé sur la valeur **appliquée**, et par un vecteur qui
l'EXÉCUTE. ⛔ Ne pas se contenter de l'ancien `static_assert`.

⚠️ **Pourquoi cette exclusion est écrite noir sur blanc dans une fiche qui parle de chlore** : les
deux pauses sont **jumelles dans le code**. Elles sont armées à **quatre lignes d'écart**
(`pump_controller.cpp:545` et `:549`) et transmises **dans le même tableau** —
`const uint32_t fullMixing[2] = {kPhMixingDelayMs, kOrpMixingDelayMs};`
(`src/dosing_window.cpp:96`). ⇒ Rendre « la pause mélange » apprise **sans préciser laquelle**
mènerait naturellement à les changer **toutes les deux**. L'exclusion n'est pas un rappel de
chimie : c'est une **barrière contre un dégât collatéral**.

ℹ️ ⛔ **Et ce n'est pas en contradiction avec le fait que le pH apprend déjà** (feature-085) : le pH
apprend **sa DOSE** (combien d'acide par jour), il n'apprend **pas sa pause**. Deux grandeurs
distinctes, et seule la seconde est verrouillée.

### 4. Afficher — la contrepartie obligatoire
⭐ **Le principe du projet s'applique directement** : *quand l'appareil s'écarte de son automatisme
nominal, il le dit sans qu'on ait à le chercher.* Une pause qui passe silencieusement de 20 à
60 minutes refabriquerait le défaut de bug-200.

L'écran doit donner, **sans geste** :
- la **durée en vigueur** et son origine : « réglage d'usine » ou « observée sur votre bassin » ;
- sur combien d'observations elle repose.

⛔ Le compte à rebours, lui, **existe déjà** et ne change pas : `orp_mix_remaining_s` est servi sur
`/data` (`src/web_routes_data.cpp:632`) **et** sur la trame WS (`src/ws_manager.cpp:1352`), par le
**même accesseur** `getMixingRemainingS(1)`. ⇒ Le champ **neuf** est la *durée en vigueur*.

### ✅ La mesure se fait PAR BLOCS de 20 minutes — arbitré par l'utilisateur le 2026-11-23

L'appareil ne compare sa mesure **qu'une fois tous les `kOrpMixingDelayMs`**, ⛔ pas en continu.
⇒ **Toute observation vaut k × 20 min, avec k ∈ [1, 6]** : la durée apprise ne peut valoir que
**20 · 40 · 60 · 80 · 100 · 120 minutes**. Six valeurs, jamais autre chose.

⭐ **Conséquences, toutes voulues :**
- **Zéro mémoire, zéro cadence d'échantillonnage, zéro index.** Le mécanisme n'a besoin que de
  l'instant du prochain jalon et de la mesure du jalon précédent.
- **L'arrondi se fait toujours VERS LE HAUT**, donc du côté prudent — cohérent avec « ne peut
  qu'allonger ».
- ⚠️ **Le repère du 18/11 (~88 min réelles) rendra 100 min affichées**, ⛔ pas 88. Le critère E1
  doit être lu ainsi : *la valeur attendue est **100 min**, et c'est un succès, pas un écart.*
- ⚠️ L'affichage ne montrera donc jamais « 1 h 25 » : les seuls libellés possibles sont
  « 20 min », « 40 min », « 1 h », « 1 h 20 », « 1 h 40 », « 2 h ». ⭐ **Cela SIMPLIFIE l'écran**
  et le formatage.

⛔ **Option écartée** : comparaison continue à la minute (~88 min sur le 18/11). Elle exigeait un
tampon des 20 dernières minutes de mesures (~80 o), un index et une cadence — **trois pièces de
plus** dans un mécanisme voulu simpliste, pour une précision dont une **garde** n'a pas besoin.

### ⚠️ Risque reconnu : l'apprentissage peut rester INERTE sur un bassin très déficitaire
Tant que la pause vaut 20 min, l'appareil est **autorisé à réinjecter pendant la montée** — ce qui
**salit** l'observation et la fait jeter. Sur un bassin qui réclame du chlore en permanence, les
trois observations propres pourraient ne **jamais** s'accumuler.
⇒ ⛔ **Ce n'est pas un défaut du mécanisme** : dans ce cas il reste **strictement** le comportement
d'aujourd'hui (20 min). Mais il doit être **écrit** et **journalisé** : compter les observations
jetées **par motif**, pour qu'un apprentissage inerte soit **constatable** au lieu d'être silencieux.

### ✅ Conception de l'écran — arrêtée le 2026-11-23 (`ux-ui-designer`, puis vérifiée dans le code)

**Emplacement retenu** : une ligne clé/valeur **dans la carte « Débit d'injection ORP »**
(`#orp-card-dosing`), sous le bloc `.kv--dose`, avec un modificateur CSS neuf `.kv--plain`
purement additif. ⛔ Pas de cinquième carte, ⛔ pas de modification de la pastille de compte à
rebours existante.

    Pause mélange                               1 h 25
    observée sur votre bassin
    3 observations

Avant l'apprentissage : « réglage d'usine » puis « 1 observation sur 3 ».
✅ **Tenue à 390 px vérifiée par comptage** : libellé 13 car., valeur 6 car. max, note ligne 1
25 car. max, note ligne 2 19 car. max — chaque ligne sous les 27 caractères déjà observés sur
une note pleine largeur. ⚠️ À **reconfirmer par capture** après implémentation.

**Cohabitation avec le compte à rebours** : la pastille d'en-tête reste « Pause mélange · 11 min »
(le **reste**), la ligne permanente dit « 1 h 25 » (la **durée en vigueur**). ⛔ Aucun lien à coder
entre les deux.

#### ⭐ La carte est masquée hors mode Auto — et c'est CORRECT, pas un défaut
`data/app.js:8010` masque `#orp-card-dosing` dès que `mode !== "automatic"`. J'ai vérifié
pourquoi c'est le bon emplacement malgré ça : **la pause mélange ne s'applique PAS hors mode
Auto**. Elle est une garde de `canDose()` (`src/pump_controller.cpp:1774`,
`in.mixingActive = … isOrpMixingDelayActive(nowMs)`), et le commentaire de
`src/pump_controller.cpp:855` l'écrit noir sur blanc : « *mode Programmé/manuel, où canDose()
n'est appelé à AUCUN tick* ».
⇒ ⛔ Afficher la durée en mode Programme ou Arrêt annoncerait une garde **qui ne garde rien**.
✅ Le masquage est donc la **bonne** décision, ⛔ pas un compromis.

#### Règle d'accumulation — ce que ce lot RETIRE
⚠️ CLAUDE.md : *un ajout sans retrait est un refus.*
- ✅ **RETENU dans ce lot** : le paragraphe « Injection manuelle » de la carte Régulation passe de
  ~5 lignes à « Démarre la pompe ORP à la puissance configurée. Un nouvel appui arrête
  l'injection. » (−3 lignes à 390 px). ⛔ La phrase en gras « disponible dans tous les modes —
  les limites de sécurité s'appliquent » **reste** : c'est une information de sécurité.
- ⛔ **ÉCARTÉ de ce lot** : masquer « Historique indisponible » sous « Atteinte de la cible ».
  ⚠️ L'agent a raison sur le fond — c'est un état vide affiché en permanence — **mais
  `data/app.js:8236` montre que bug-201 a déjà tranché ce point** : la légende a été retirée du
  héros pH, et « *l'ORP, hors lot, garde son aveu* ». ⇒ Y toucher ici rouvrirait un lot clos sans
  l'avoir étudié. **À traiter dans une fiche séparée**, ⛔ pas au fil de celle-ci.
- ⛔ **ÉCARTÉ** : « Plage autorisée : 300 à 950 mV » — consigne de sécurité, conservée.
- ⛔ **ÉCARTÉ** : replier les deux paragraphes validés par `pool-chemistry` dans un `<details>` —
  décision à prendre **avec** `pool-chemistry`, pas ici.

#### ⚠️ Pour photographier la pause en cours, il faut forcer DEUX champs
`data/app.js:7820` exige `orpMixingDelayActive === true` **en plus** de `orp_mix_remaining_s`.
⇒ `./tools/ui_shot.sh -n -r '#/orp' -w 390 -f orp_mix_remaining_s=640 -f orpMixingDelayActive=true`
⛔ Avec le seul `orp_mix_remaining_s`, la pastille **ne se rend pas** et la capture est muette.

### ⚠️ E1 et H1 ne peuvent PAS être cochés dans ce cycle — et c'est prévu
E1 (« la durée affichée converge vers la réalité observée ») exige **trois observations propres**,
à raison d'**une par injection de chlore** ⇒ plusieurs jours de fonctionnement réel **après**
déploiement. H1 (capture humaine montrant une durée apprise) en dépend.
⇒ ✅ **Décision : la fiche passe en `done/` dès le code fini**, avec E1 et H1 marqués
« **en attente d'observation terrain** » — ⛔ jamais cochés par commodité. Laisser la fiche en
`doing/` plusieurs jours bloquerait la règle « une seule fiche en cours ».
⚠️ Le déploiement lui-même exige le feu vert de l'utilisateur **et sa phrase de passe**.

### Cas limites
- **Moins de 3 observations propres** ⇒ 20 min, et l'écran dit « réglage d'usine ».
- **Redémarrage** ⇒ la valeur apprise survit (NVS). Une observation **en cours** est perdue, jamais
  comptée à moitié.
- **Changement de produit ou de consigne** ⇒ ⛔ **rien à faire**, et c'est une conséquence de la
  conception du 23/11 : la durée apprise est **hydraulique**, elle ne dépend ni de la consigne ni
  de la concentration du produit. ⛔ L'« empreinte de contexte » de `PhIntegralSnapshot` est
  **retirée du périmètre** (elle était justifiée quand on apprenait un seuil en mV, plus
  maintenant). ⚠️ Un **CRC** reste exigé sur l'instantané persisté, lui.
- **La mesure ne se stabilise jamais** (dérive, fuite, bassin en déséquilibre) ⇒ l'observation est
  **abandonnée** au plafond, ⛔ jamais enregistrée comme « très longue ».
- **Observation plus courte que 20 min** ⇒ ⛔ **ne peut pas se produire** (§1, par construction).
  L'écrêtage bas reste codé et testé comme filet, ⛔ et ce n'est **pas** une anomalie à signaler.
- **Mode maintenance / dosage suspendu** ⇒ aucune observation.
- ⭐ **La mesure DESCEND pendant la fenêtre** (consommation forte, soleil) ⇒ le critère se déclenche
  au premier pas : la pause reste **au plancher**. ⚠️ C'est le comportement voulu — voir
  « Pourquoi le mécanisme se corrige tout seul » plus bas.

## Comportement actuel
La pause vaut 20 minutes sur tout bassin, n'apparaît nulle part dans les réglages, et rien ne
mesure si elle correspond à la réalité de l'installation.

## Contraintes techniques
- **ADR-0017** : toute la décision dans un module **pur** (`src/orp_mixing_learn_logic.h` attendu) —
  ⛔ pas de `millis()`, pas de `time()`, pas de NVS, pas d'`Arduino.h` ; le temps est un
  **paramètre**. `pump_controller.cpp` ne fait que le câblage.
- ⚠️ **Parité `/data` ↔ WebSocket sur l'EXPRESSION, pas seulement sur la clé** — 4ᵉ récidive connue,
  la 4ᵉ forme était **invisible au grep**. Les deux chemins appellent **le même accesseur**.
- **Budget de trame WS** : documenté à `src/ws_manager.cpp:482` (bump à 2048 lors de l'ajout de
  `ph/orp_mix_remaining_s`) et détaillé champ par champ vers `:607-614`. ⚠️ Le numéro donné le
  19/11 (`:1484`) était faux — c'est `rescue_lift`. Le **vérifier**, ⛔ ne pas l'affirmer.
- **ADR-0082** : aucune allocation sur le chemin de journalisation.
- **ADR-0100** : ⛔ le navigateur ne recalcule **jamais** la durée ni son origine depuis un seuil
  local — il **lit** ce qui est servi.
- ⚠️ **RESPONSIVE** : 390 px d'abord.
- ⚠️ **Usure NVS** : une observation par injection, ⛔ pas une écriture par échantillon. Patron de
  cadence : R5 de `ph_persist_logic.h`.

## Fichiers potentiellement concernés
- `src/orp_mixing_learn_logic.h` *(neuf — logique pure)*
- `src/pump_controller.{h,cpp}` — observation, accesseur unique, armement à `:549`
- `src/dosing_window.cpp:96` — passer la durée apprise au lieu de la constante
- `src/constants.h` — `kOrpMixingDelayLearnedMaxMs` + `static_assert` de bornes
- `src/config.cpp` — persistance NVS de l'instantané
- `src/web_routes_data.cpp`, `src/ws_manager.cpp` — le champ neuf, **même expression**
- `data/app.js`, `data/index.html`, `data/app.css`
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/subsystems/ws-manager.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Module pur `orp_mixing_learn_logic.h`, compilable en natif
- [x] A2 — Observation ouverte/fermée/**jetée** selon les conditions de propreté
- [x] A3 — **Maximum des 3 dernières** observations propres, persisté en NVS **avec CRC**
  (⛔ sans empreinte de contexte — retirée du périmètre, voir Cas limites)
- [x] A4 — Écrêtage **vers le haut uniquement**, plafond écrit **`6 * kOrpMixingDelayMs`**
  (⛔ jamais une valeur en minutes en dur), appliqué au point d'armement ORP
  (`src/pump_controller.cpp:552`)
- [x] A5 — ⛔ `kPhMixingDelayMs` **inchangé**, `static_assert` pH tous verts
- [x] A6 — ⭐ **(ajouté le 2026-11-23)** L'invariant du détecteur de sonde figée
  (`constants.h:3221`) re-prouvé sur la durée **EN VIGUEUR**, pas seulement sur la constante
- [x] A7 — ⭐ Pause **en cours au redémarrage** : durée **en vigueur à la restauration**
  (condition C7), couverte par un vecteur
- [x] A8 — ⛔ **BLOQUANT (C5)** : le repli de sécurité de `dosing_window_logic.h:302-309` arme la
  pause à la durée en vigueur au lieu de `out.mixingEndMs = 0`
- [x] A9 — ⛔ **BLOQUANT (C6)** : la durée apprise est chargée **avant** `dosingWindow.begin()`
  (`src/main.cpp:452`)
- [x] B1 — Durée en vigueur + origine + nombre d'observations servies dans `/data`
- [x] B2 — **Même champ, même expression** dans la trame WS
- [x] B3 — Affichage sans geste sur la page ORP, lisible à 390 px
- [x] C1 — Doc : API, ws-manager, pump-controller, page-orp, guide de régulation, CHANGELOG
- [x] D1 — Test de parité `/data` ↔ WS sur l'**expression**
- [x] D2 — ⭐ **Mutation exigée** : retirer l'écrêtage bas (autoriser < 20 min) doit **tuer** un test
- [x] D3 — ⭐ **Mutation exigée** : retirer le champ d'**un seul** des deux chemins doit **tuer** un test
- [x] D4 — ⭐ **Mutation exigée** : accepter une observation contaminée doit **tuer** un test
- [x] D5 — Vecteurs natifs : < 3 observations ⇒ sortie **exactement** 20 min
- [x] D6 — ⭐ **Mutation exigée** : remplacer le **maximum** des 3 par une médiane ou une
  moyenne doit **tuer** un test
- [x] D7 — ⭐ **Mutation exigée** : ramener le repli de sécurité à `mixingEndMs = 0` (C5) doit
  **tuer** un test, avec une durée apprise **supérieure à 60 min** dans le vecteur
- [x] D8 — ⭐ **Mutation exigée** : inverser l'ordre de `main.cpp:452` (C6) doit **tuer** un test
- [ ] E1 (en attente d'observation terrain) — Essai réel sur l'appareil de production : la durée affichée converge vers la réalité
  observée (repère du 18/11 : ~88 min)

## ✅ Notes d'implémentation — 2026-11-23, v3.2.0

**Livré** : module pur `src/orp_mixing_learn_logic.h` (observation, anneau de 3, instantané NVS
20 o scellé FNV-1a) · câblage dans `pump_controller` · passe-plat `setOrpFullMixingMs` · 3 champs
servis · ligne « Pause mélange » sur la page ORP · ADR-0145.

**Chiffres MESURÉS** (⛔ ne pas les recopier ailleurs, ils périmeront) :
- `pio test -e native` : **98 suites, 2 765 cas, 0 échec** ; suite neuve `test_native_orp_mixing_learn`
  = **23 cas**.
- `pio run -e esp32dev_core3` : SUCCESS, **flash 52,9 %**, **RAM 30,1 %**, 0 warning neuf.
- Trame WS : **+77 o** (23 + 30 + 24), marge **446 o** sous `kWsFrameBufBytes = 4096`, **aucun bump**.

### ⭐ Deux défauts trouvés EN REVUE, et corrigés — aucun des deux n'était dans la conception
1. **`pool-chemistry` P2 (BLOQUANT)** : le prédicat de propreté ignorait l'**étalonnage**.
   Pendant une session de calibration ou sa stabilisation, l'**échelle mV change sous
   l'observation** (et la sonde peut être hors de l'eau) ⇒ un décalage vers le haut se lirait
   comme une montée qui dure, et la valeur fausse serait persistée **sans péremption**.
   ⚠️ Les deux mêmes prédicats bloquaient **déjà** le dosage à `pump_controller.cpp:4039`.
   ⇒ Corrigé, couvert par `test_20` (sentinelle lexicale, les deux mutations pratiquées
   **séparément**).
2. **`code-reviewer`** : `.kv { display: grid }` **bat** l'attribut `hidden` de la feuille UA ⇒ la
   ligne serait restée visible **avec un tiret** sur tout appareil ne servant pas encore ces
   champs. ⇒ `.kv[hidden] { display: none !important; }` ajouté — motif déjà présent pour
   `.chip`, `.btn`, `.state-badge`. **Vérifié par capture**, la ligne disparaît bien.

### ⚠️ Un faux bloquant, et la leçon à en tirer
`code-reviewer` a conclu « le binaire ne reflète pas le diff » en comparant les **dates** des
fichiers. ⛔ **Faux** : PlatformIO compare les **empreintes de contenu**, pas les mtimes — les
restaurations de mutations avaient touché les dates sans changer le contenu. Tranché en
recompilant : no-op de 5 s, puis **14 s** après une modification réelle. ⚠️ **J'avais fait la même
erreur** une heure plus tôt.

### ⚠️ Un mutant SURVIVANT, assumé
Retirer l'écrêtage final au plafond ne tue aucun test : le rejet des valeurs aberrantes en amont
rend ce cas **inatteignable**. ✅ **Mutant équivalent** — ⛔ ne pas fabriquer un test artificiel
pour le tuer, ce serait une fausse garantie.

### Correction documentaire hors périmètre, assumée
`docs/subsystems/ws-manager.md` annonçait encore `kWsFrameBufBytes = 3584` dans **cinq passages
normatifs**, alors que le code dit **4096** depuis feature-163. ⚠️ Ce chiffre périmé a trompé
**deux agents et moi-même** pendant ce lot. Les cinq sont corrigés ; ⛔ les sections **historiques
datées** gardent leurs valeurs d'époque, qui sont justes.

### ⛔ Ce qui RESTE à faire
- **E1** — essai réel. ⚠️ Exige **trois observations propres**, soit plusieurs jours **après**
  déploiement. ⭐ **Valeur attendue : 1 h 40** (100 min), ⛔ **pas 88 min** — la granularité est de
  20 min et l'arrondi se fait vers le haut. **1 h 40 sera un SUCCÈS, pas un écart.**
- **H1** — capture humaine de la page ORP montrant la durée apprise (dépend de E1).
- **Déploiement** — ⛔ non fait. Exige le feu vert de l'utilisateur **et sa phrase de passe**.
- **Fiches de suivi suggérées** (⛔ non créées) : alerter quand le plafond est atteint (une montée
  de plus de 2 h signe un circuit très lent ou une sonde dérivante) · masquer « Historique
  indisponible » côté ORP (bug-201 l'a laissé « hors lot »).

## ADR attendu ?
**Oui** — premier paramètre de **sécurité de dosage** dont la valeur est **apprise** plutôt que
fixée. Le trade-off à consigner : *pourquoi l'apprentissage ne peut jouer que dans le sens
prudent*, et *pourquoi la pause pH reste figée alors que l'ORP ne l'est plus*.

## Éléments visibles (pour doc-writer)
### Champs WebSocket
- Durée de pause mélange ORP en vigueur + origine + profondeur d'observation *(noms à arrêter)*
### Endpoints HTTP concernés
- `/data` — mêmes champs, **même expression**
### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/subsystems/pump-controller.md`,
  `docs/features/page-orp.md`, `docs/user/src/guide-regulation.html` + `./tools/build_user_pdfs.sh`,
  `CHANGELOG.md`, ADR
- 📸 **Étape humaine** : capture de la page ORP montrant la durée apprise

## Sécurité chimique
- **Dosage impliqué** : **oui** — la pause conditionne l'autorisation de réinjecter
- **Limites concernées** : ⛔ **aucune n'est modifiée**. Plafond horaire, plafond journalier,
  nombre de cycles, présence d'eau, watchdog : tous **intacts**
- **Filtration requise** : sans objet
- **Calibration capteur modifiée** : non
- **Algorithme PID touché** : non — ⛔ la voie pH, seule porteuse d'un terme intégral, est **hors
  périmètre**
- **Sens de l'écart** : ⭐ l'apprentissage ne peut que **rendre l'appareil plus patient**. Au pire
  il injecte **moins souvent** qu'aujourd'hui, ⛔ jamais plus

### ⭐ Pourquoi le mécanisme se corrige TOUT SEUL dans le bon sens (2026-11-19)
Ce que l'observation mesure, c'est **le moment où la mesure cesse de monter**. Or en forte
consommation — eau chaude, fréquentation, soleil — le chlore est **consommé pendant qu'il se
disperse** : la courbe s'aplatit **plus tôt** et **plus bas**. ⇒ **Plus la saison est exigeante,
plus la durée apprise est COURTE**, donc plus vite écrêtée au plancher de 20 min.

⇒ ⭐ **Le cas « l'appareil devient trop patient » ne peut PAS se produire quand le bassin a
réellement besoin de chlore.** Il se produit en **basse saison** — eau froide, demande faible,
montée longue et propre — c'est-à-dire précisément quand le bassin n'a **pas** besoin de la
cadence maximale. ⚠️ En pleine saison, le mécanisme **dégénère vers le comportement
d'aujourd'hui**, ce qui est exactement le repli souhaitable.

### Le chiffrage du débit maximal, puisqu'il était l'argument du plafond
| | Pause 20 min (aujourd'hui) | Pause 88 min (valeur observée le 18/11) |
|---|---|---|
| Cycle complet (10 min d'injection + pause) | 30 min | 98 min |
| Temps de pompe maximal | **non contraignant** — le plafond horaire (10 min/h) borne avant | ~**6,1 min/h** — la pause devient contraignante |
| Pour atteindre le plafond journalier (2 000 mL ≈ **44 min** de pompe au débit relevé de 45,6 mL/min) | ~4,4 h de filtration | ~**7,2 h** de filtration |

⇒ Même à 88 minutes, le plafond journalier reste **atteignable dans une fenêtre de filtration
normale**. ⚠️ **Le plafond de la durée apprise n'est donc PAS un frein saisonnier** — c'était une
inquiétude mal orientée, retirée le 2026-11-19 après chiffrage.

### ✅ Passage P1 de `pool-chemistry` — FAIT le 2026-11-23 (trois tours)
Le design a été soumis **trois fois**. Les deux premières versions ont été **refusées par
l'utilisateur** : la première posait **cinq seuils tous déduits de ce bassin-ci** (12 mV, 30 min,
40 mV, 120 min, 180 min), la seconde les a universalisés mais en **augmentant** la complexité
(constante de temps apprise, mesure de bruit, équation implicite, normalisation, mode exploration).
⇒ La conception retenue plus haut est la **troisième**, et elle supprime **seize pièces** par
rapport à la deuxième.

**Points du 19/11 — tous tranchés, ⛔ ne pas les rouvrir :**
1. **Plafond** ⇒ `6 * kOrpMixingDelayMs`, exprimé en multiple du plancher (§3). Quand il est
   atteint, l'observation est **jetée** — ⚠️ et le **dire** reste souhaitable, mais c'est une
   **alerte d'état du bassin**, pas une information de réglage ⇒ **hors périmètre de ce lot**,
   fiche séparée.
2. **Critère de plateau** ⇒ ⛔ il n'y en a plus. Remplacé par la comparaison à soi-même (§1).
3. **Fenêtre de 7 jours** ⇒ ⛔ retirée. On compte en **observations** (trois), jamais en jours.

### ⛔ Conditions BLOQUANTES retenues — deux défauts du code existant, vérifiés ligne à ligne
⚠️ Ces deux-là sont **indépendants du mécanisme d'apprentissage** : ils existent aujourd'hui et
restent invisibles **uniquement parce que la pause vaut 20 minutes**.

**C5 — le repli de sécurité n'arme AUCUNE pause.**
`src/dosing_window_logic.h:302-309` : en cas d'instantané invalide, la sortie est
`out.usedMs = kDosingWindowHourMs; … out.mixingEndMs = 0;`. ⇒ L'heure entière est verrouillée,
ce qui **couvre** aujourd'hui les 20 minutes de pause — par coïncidence, pas par conception.
⛔ **Dès que la durée apprise dépasse 60 minutes, le verrou horaire ne la couvre plus** : au
changement d'heure, l'appareil pourrait réinjecter **avant la fin de la pause**.
⇒ Le repli doit armer la pause à la **durée en vigueur**.

**C6 — l'ordre de démarrage charge la fenêtre AVANT la valeur apprise.**
`src/main.cpp:452` appelle `dosingWindow.begin()` **avant** `PumpController.begin()`, lequel
exécute `applyRestoredDosingWindow()` (`src/pump_controller.cpp:298`). ⇒ Si la valeur apprise est
chargée dans le second, la restauration du premier travaille avec la **constante d'usine**.
⇒ La durée en vigueur doit être disponible **avant** `dosingWindow.begin()`.

**C7 — la pause EN COURS au redémarrage** (point 4 du 19/11, toujours ouvert et à trancher
explicitement dans l'ADR) : `src/pump_controller.cpp:365` la restaure depuis un instantané, et
`src/dosing_window_logic.h:364` la **réarme en PLEIN** (`rem = fullMixingDelayMs;`, cas C5 du
module). ⇒ **Décision retenue : c'est la durée EN VIGUEUR à la restauration qui s'applique** —
c'est le comportement actuel, il ne peut qu'**allonger**, et il évite de persister une seconde
grandeur. ⚠️ À **écrire dans l'ADR** et à **couvrir par un vecteur**, ⛔ pas à subir.

**C8 — l'invariant du détecteur de sonde figée** (`src/constants.h:3221`) devient une propriété
d'**exécution** et non plus de compilation. ⇒ Re-prouvé sur la durée **appliquée**, par un vecteur
qui l'EXÉCUTE (livrable A6). ⭐ Le sens est favorable : l'apprentissage ne fait qu'**allonger** la
borne basse.

## Hors périmètre
- ⛔ L'apprentissage du **rendement** (mV par mL) — levier distinct, fiche séparée
- ⛔ Toute modification de la pause mélange **pH**
- ⛔ Tout renommage dans l'interface (décision utilisateur du 2026-11-19)
- ⛔ Le compteur de stock de chlore (`orp_total_injected_ml` > `orp_container_ml` constaté le
  18/11 : le bidon n'a pas été remis à zéro au dernier remplissage) — défaut réel, **autre sujet**
