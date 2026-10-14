## Métadonnées
- **ID** : feature-161
- **Slug** : choisir-le-mode-depuis-les-cartes
- **Type** : feature
- **Statut** : doing
- **Priorité** : normale
- **Version cible** : 2.77.0
- **Dernière mise à jour** : 2026-10-13
- **Spec(s) liée(s)** : feature-147 (les 4 boutons de mode déjà posés sur la carte Filtration),
  feature-140 (Boost), chantier de simplification de l'interface (direction du 2026-10-09)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket *(a priori faux : le **lot C** a modifié `web_routes_config.cpp` et `mqtt_manager.cpp`)*
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader · [x] architect *(à confirmer)*
- [x] **pool-chemistry** — ⚠️ OBLIGATOIRE : changer le mode de régulation **depuis l'accueil**,
      c'est **démarrer ou arrêter le dosage en un seul geste**.
- [x] **ux-ui-designer — MANDAT SUR L'ÉCRAN ENTIER** (règle d'accumulation + « l'UX n'est pas
      optionnel »). ⚠️ Son brief doit dire : *évalue le tableau de bord COMPLET, propose ce qui
      doit DISPARAÎTRE*.
- [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] embedded-developer — *l'« a priori non » s'est révélé faux* : le lot C touche `src/config.cpp`, `src/mqtt_manager.cpp`, `src/web_routes_config.cpp`.

## Description

Permettre de **choisir le mode directement dans la carte** du tableau de bord, pour les **quatre**
cartes concernées : **Filtration, pH, ORP, Éclairage**. Et, dans la foulée, **remettre le Boost à
sa place** : une option, pas un bloc aussi lourd que le reste.

## Contexte

**Demande utilisateur du 2026-10-13**, avec trois maquettes (Filtration, ORP en Auto, ORP en
Programme) : « *J'aimerais une refonte des cartes. Les cartes permettraient de choisir le mode
dans la carte. C'est pratique plutôt que d'aller dans la page de réglage de la carte. […] Il n'y a
pas d'exemple pour les cartes pH et Éclairage, mais elles sont concernées par la refonte. Pour la
filtration, le Boost est mieux présenté dans cet exemple. On voit que ce n'est qu'une option.* »

### État actuel, vérifié dans le code

| carte | sélecteur de mode dans la carte ? |
|---|---|
| **Filtration** | ✅ **oui** — 4 boutons depuis feature-147 (Auto / Programme / Marche / Arrêt) |
| **pH** | ❌ non — valeur, cible, jauge, et un lien vers la page de réglage |
| **ORP** | ❌ non — idem |
| **Éclairage** | ❌ non — à instruire |

⇒ La demande **étend aux trois autres** le motif déjà posé sur la Filtration, **et** fait évoluer
la Filtration elle-même (horaires dans la carte, Boost rétrogradé).

### ⚠️ CE QUE LES MAQUETTES CHANGENT, AU-DELÀ DU SÉLECTEUR

1. **Le contenu de la carte DÉPEND DU MODE.** ORP en *Auto* montre la jauge, la valeur et la
   cible ; ORP en *Programme* montre un **volume quotidien** et sa **limite de sécurité**.
   Filtration en *Programme* montre les **horaires**. ⚠️ La hauteur de la carte **varie** —
   conséquence de mise en page à instruire, pas un détail.
2. **Le Boost devient une OPTION** : sous-bloc intitulé `OPTION`, séparé, avec sa phrase
   d'explication et son bouton. ⚠️ Il porte aujourd'hui le même poids visuel que la filtration
   elle-même, ce que l'utilisateur relève explicitement.
3. **La limite de sécurité est AFFICHÉE à côté du champ** (« Limite de sécurité : 2 000 mL »),
   pas seulement opposée au moment d'enregistrer.

### ⚠️ UN CHANGEMENT DE VOCABULAIRE, ET IL N'EST PAS ANODIN

Les pages de réglage disent **« Automatique / Programmée / Manuelle »**. Les maquettes disent
**« Auto / Programme / Arrêt »**.
⇒ **« Manuelle » devient « Arrêt »**. Ce n'est pas un raccourci : « manuelle » laisse croire qu'on
dosera à la main, « Arrêt » dit que **la régulation automatique ne fonctionne plus**. Le second
est plus vrai.
⚠️ **MAIS attention au piège, déjà présent** : sur la Filtration, la valeur interne `manual`
s'affiche **« Programme »**, tandis que `off` s'affiche **« Arrêt »**. La même valeur interne
porterait donc deux libellés différents selon la carte. **À trancher explicitement**, et à rendre
cohérent — ou à assumer par écrit.

## Comportement attendu

- Sur chacune des **quatre** cartes, le mode se change **sans quitter l'accueil**.
- Le contenu de la carte **s'adapte au mode choisi** (horaires, volume quotidien, cible…).
- Le **Boost** est présenté comme une **option** de la filtration, visuellement secondaire.
- Le lien vers la page de réglage **demeure** pour tout ce qui ne tient pas dans la carte.

### Cas limites
- **Échec de l'enregistrement** (appareil injoignable, refus serveur) : le sélecteur doit revenir
  à l'état **réel**, jamais rester sur le mode demandé. ⚠️ Un sélecteur qui ment sur le mode de
  dosage est pire que pas de sélecteur.
- **Mode changé depuis ailleurs** (MQTT, autre onglet, escalade automatique) : la carte doit
  suivre. ⚠️ ADR-0100 — l'écran **répète** ce que l'appareil dit, il ne le **devine** pas.
- ⚠️ **Escalade de sécurité active** (`phAutoManual*`) : le mode pH est verrouillé en Manuel par
  l'appareil et sortir exige un acquittement explicite (`web_routes_config.cpp`, incident du
  2026-08-25). **Que montre la carte dans ce cas ?** ⛔ Elle ne doit **ni** proposer un retour en
  Auto silencieux, **ni** faire croire à un refus inexpliqué.
- **Étalonnage en cours** : la régulation est basculée en Manuel par l'appareil (bug-160). La
  carte doit-elle laisser changer le mode pendant ce temps ?

## Contraintes techniques
- ⚠️ **RESPONSIVE 390 px D'ABORD**, caractères **comptés**. ⚠️ Un sélecteur à **quatre** segments
  (Filtration) sur 390 px : « Auto / Programme / Marche / Arrêt » — **compter**, et dire si ça
  tient sur une ligne ou si ça doit s'empiler. ⛔ Un `flex-wrap` n'est pas un repli, c'est le cas
  normal sur téléphone.
- ⚠️ **Ne pas dupliquer les chemins d'écriture** : les routes existent, la page de réglage les
  utilise déjà. ⛔ Un second chemin vers le même réglage est exactement ce que ce projet a déjà
  payé plusieurs fois.
- Vanilla JS/CSS, français, aucune dépendance nouvelle.
- Logique de décision **extraite et pure** si elle devient non triviale (ADR-0017/0099).

## Sécurité chimique
- **Dosage impliqué : OUI, directement.** Passer de « Arrêt » à « Auto » depuis l'accueil **relance
  le dosage automatique en un geste**. L'inverse l'arrête.
- **Sens des deux erreurs** : un démarrage accidentel **fait doser** une eau qu'on n'a peut-être
  pas regardée ; un arrêt accidentel **suspend** la régulation sans qu'on s'en aperçoive.
- **Questions au gate** : (a) faut-il une **confirmation** pour certaines transitions, et
  lesquelles — ⚠️ en gardant à l'esprit le principe « les plafonds protègent, le reste informe »,
  et qu'une friction uniforme s'apprend à cliquer sans lire (leçon de feature-159) ; (b) le
  passage en « Arrêt » doit-il être **journalisé** comme l'est une escalade automatique ; (c) que
  montre la carte quand un **verrou de sécurité** est actif ; (d) doit-on pouvoir changer le mode
  **pendant un étalonnage** ?

## ADR attendu ?
- **Peu probable** — sauf si le gate impose une règle générale sur les commandes de dosage
  accessibles depuis l'accueil.

## Éléments visibles (pour doc-writer)
- `docs/features/page-accueil.md` (ou équivalent), `docs/features/page-ph.md`,
  `docs/features/page-orp.md`, `CHANGELOG.md`, `docs/user/src/guide-utilisation.html`.
- ⚠️ **Captures à refaire** — et `screenshots/07_pH_Calibration.jpg` est **déjà périmée** pour une
  raison antérieure.

## Critères d'acceptation
- [x] ⚠️ **TROIS** cartes (pH, ORP, Filtration) — **pas quatre**. L'Éclairage a été sorti du lot sur **décision utilisateur** (spec séparée **feature-162**, qui devra aussi corriger un défaut existant : la carte affiche « Programmation activée » sur le seul `lighting_schedule_enabled`, alors que `state.manualOverride` la neutralise et n'est **jamais publié**).
- [x] Le contenu de chaque carte **suit** le mode.
- [x] Le Boost est visuellement une **option**.
- [x] Un échec d'enregistrement **ramène le sélecteur à l'état réel** — fonction pure `resolveSelectorMode`, **3 mutants tués**. ⚠️ Corrige au passage un défaut **existant** côté ORP.
- [x] Un changement venu d'ailleurs est **reflété** sans rechargement (trame temps réel → `renderDashboardModeCard`).
- [x] Le cas « verrou de sécurité actif » est traité **et** compréhensible : segments **désactivés, jamais masqués**, refus 409, et **même phrase** sur les chemins HTTP et MQTT.
- [ ] ⚠️ **NON FAIT — tâche humaine.** Vérification **à 390 px sur appareil réel**, plus les captures `03_Tableau_de_bord.jpg`, `07_pH.jpg`, `08_ORP.jpg`. ⛔ Ce critère reste **décoché** : aucun agent ne peut le remplir.

## Plan de test manuel
1. Changer chaque mode depuis l'accueil, vérifier sur la page de réglage que c'est bien pris.
2. Couper le réseau, changer un mode : le sélecteur revient-il à l'état réel ?
3. Changer un mode depuis la page de réglage : la carte suit-elle ?
4. Téléphone, 390 px, les quatre cartes.

## Hors périmètre
- La page de réglage elle-même (elle **demeure** pour ce qui ne tient pas dans la carte).
- Les seuils et limites de dosage.

## ⚠️ PRÉCISION DE L'UTILISATEUR (2026-10-13) — « Manuelle » cesse d'être un mode

> « *Je pense que Manuelle ne devrait pas être un mode. Arrêt est mieux. Et on pourrait laisser la
> possibilité d'injecter manuellement dans la page des réglages.* »

**C'est une simplification de FOND, pas un renommage.** Le mode répond à une seule question :
*est-ce que la régulation automatique tourne ?* Injecter à la main est une **ACTION**, pas un
**MODE**. Les confondre obligeait à « se mettre en Manuelle » pour faire un geste ponctuel — et à
penser à en ressortir.

### ✅ VÉRIFIÉ DANS LE CODE : le firmware est DÉJÀ d'accord

⚠️ **Aucune des neuf gardes d'`evaluateManualInject` ne regarde le mode** (`src/dosing_logic.h` :
watchdog, présence d'eau, stabilisation, double départ, limites journalière/horaire, cycles/jour,
anti-rafale ×2). Et **aucune route d'injection ne le vérifie** non plus.
⇒ **L'injection manuelle fonctionne DÉJÀ dans tous les modes.** Toutes les gardes s'appliquent, et
les compteurs sont **partagés avec l'automatique** : les limites tiennent.

### ⚠️ C'EST L'INTERFACE QUI LA CACHE, et c'est purement cosmétique

`data/app.js`, `updatePhModeControls()` :
```js
inject.style.display = mode === "manual" ? "" : "none";   // idem ORP
```
⇒ **La restriction n'est pas une sécurité, c'est un masquage.** La lever ne demande **aucun
changement de firmware**.

### Ce que ça change pour ce lot

- Le sélecteur devient **Auto / Programme / Arrêt** partout, et `manual` **n'est plus présenté
  comme un mode d'usage** — il reste la valeur interne qui signifie « régulation automatique
  arrêtée ». ⛔ **Ne pas renommer la valeur interne** : elle est persistée en NVS, publiée en MQTT,
  et `phEnabled` en est le miroir (`config.h:146`).
- La section **« Injection manuelle » cesse d'être conditionnée au mode** et reste accessible
  **depuis la page de réglage**, quel que soit le mode.
- ⇒ **L'incohérence relevée plus haut se résout d'elle-même** côté pH/ORP : plus de libellé
  « Manuelle » du tout. ⚠️ **Reste le cas Filtration**, où `manual` s'affiche « Programme » — à
  trancher séparément.

### ⚠️ QUESTIONS AU GATE que cette précision OUVRE

1. **Injecter à la main pendant que l'automatique régule** devient possible **et visible**. Les
   compteurs sont partagés, donc les limites tiennent — mais **est-ce souhaitable** ? Faut-il un
   avertissement, ou est-ce au contraire l'usage normal (ajustement ponctuel sur une eau qu'on
   vient de tester) ?
2. L'écran affiche-t-il **ce que l'automatique est en train de faire** au moment où l'on injecte
   à la main ? ⚠️ Deux dosages simultanés sans que l'écran le dise seraient un piège.
3. ⚠️ **Le mot « Arrêt » dit-il assez ?** Il signifie « la régulation automatique ne dose plus » —
   mais l'appareil **peut encore doser** si l'on appuie sur Injecter. Un utilisateur pourrait lire
   « Arrêt » comme « plus aucun dosage possible ». **À instruire** : faut-il préciser quelque part
   que l'injection manuelle reste disponible ?

## ⚠️ ANALYSE D'IMPACT — ce qui a été VÉRIFIÉ, et la décision utilisateur qui en découle

### L'Éclairage n'a PAS de modes — et l'utilisateur a tranché de lui en donner

**Constat vérifié** (`index.html:302-339`) : la carte Éclairage n'a **ni Auto, ni Programme, ni
Arrêt**. Elle porte **un booléen** (`lighting_schedule_enabled` — programmation activée ou non) et
**deux boutons** Allumer/Éteindre qui pilotent le relais **indépendamment du planning**.

⚖️ **DÉCISION UTILISATEUR du 2026-10-13** : lui donner **trois modes équivalents** aux autres
cartes — **`Programme` / `Allumé` / `Éteint`** — avec les horaires affichés sous le mode
`Programme`, comme pour la Filtration.

⚠️ **CONSÉQUENCE À ASSUMER, pas à découvrir** : **on crée une notion de « mode » qui n'existe pas
dans l'appareil.** Il faudra décider comment les trois libellés se projettent sur l'état réel
(un booléen de planning + un relais), et **où** vit cette projection. ⛔ **Ne pas inventer un
quatrième état persistant** si les deux existants suffisent à le représenter.
⚠️ Question ouverte pour `architect` : « Allumé » / « Éteint » sont-ils des **modes persistants**
(le planning reste désactivé) ou de simples **commandes** ? Le choix change la sémantique au
redémarrage.

### Vérifié aussi

- **pH et ORP sont strictement symétriques** — même structure, mêmes trois modes, mêmes fonctions
  miroirs. Portage symétrique. En mode programmé, les deux affichent déjà un volume quotidien.
- **Un seul chemin d'écriture** : `POST /save-config`. ⛔ Rien à créer.
- **Injection manuelle** : masquage identique côté pH (`app.js:2005`) et ORP (`:3029`).
  ⚠️ **Mais les sous-blocs de paramètres Auto et Programmé RESTENT légitimement conditionnés au
  mode** — ce sont les maquettes elles-mêmes qui l'exigent. ⛔ Ne pas tout démasquer en bloc.
- **Mapping Filtration confirmé** (deux occurrences, `app.js:4165` et `:6500`) :
  `auto→Auto`, **`manual→Programme`**, `force→Marche`, `off→Arrêt`.
  ⇒ L'incohérence de libellé **subsiste côté Filtration** et reste un cas à part, assumé.
- **Reste à faire sur la Filtration** : rendre « Horaires » **conditionnel au mode** (affiché
  inconditionnellement aujourd'hui, `index.html:250-253`) et re-présenter le **Boost** en option
  mineure (aujourd'hui un sous-bloc au **même gabarit** que les cartes principales).

### ⚠️ Le verrou de sécurité : refus 409 CONFIRMÉ

`web_routes_config.cpp:518-566`, **avant toute mutation**. Réponse : *« Bascule de sécurité pH non
acquittée — acquittez l'alerte sur la page pH »*.
⇒ Une requête venue de la **carte** reçoit **exactement la même réponse** que depuis la page de
réglage. **Cohérent** — mais ⚠️ **la carte doit savoir afficher ce refus**, et le message renvoie
vers « la page pH ». ⛔ Jamais de redirection silencieuse, jamais un refus inexpliqué.

### ⚠️ ANGLE MORT TROUVÉ — changer le mode PENDANT un étalonnage

bug-160 mémorise le mode **avant** l'entrée en calibration (`phCalSwitchPrevMode`) et le restaure
à la sortie. ⚠️ **Le gate 409 ne regarde QUE `phAutoManual*`, PAS `calSwitchActive`** ⇒ un
changement de mode depuis la carte **serait accepté** pendant une session d'étalonnage… puis
**silencieusement écrasé** à la sortie par la valeur mémorisée avant l'entrée.

⇒ **À trancher par `pool-chemistry` + `architect`** : bloquer le changement de mode pendant un
étalonnage, ou **mettre à jour le mode mémorisé** en cours de session ? ⛔ Ne pas laisser en
l'état : un réglage accepté puis annulé sans un mot est exactement le motif « l'écran ment » que
ce projet traque.

## ⚖️ DÉCISIONS UTILISATEUR DU 2026-10-13 — périmètre arrêté

### 1. L'Éclairage SORT de ce lot → spec séparée `feature-162`

⚠️ **Motif, trouvé par l'orchestrateur et décisif** : la carte Éclairage **MENT DÉJÀ**.
`data/app.js:6471` affiche « Programmation activée » à partir du **seul**
`lighting_schedule_enabled`, alors que `state.manualOverride` (`src/lighting.cpp`) **neutralise le
programme sans jamais être publié** — ni dans `/get-config`, ni dans la trame WS.
Et `decideLightingOn()` évalue l'override **AVANT** le programme : c'est **exactement le motif que
l'ADR-0093 a corrigé sur la filtration** (incident du 2026-10-09, « trois Auto sans effet »).

⇒ **Y poser un sélecteur transformerait un affichage faux en COMMANDE fausse.**
⇒ L'appareil n'a **aucun modèle de mode** pour l'éclairage : l'aligner exige un état persistant
neuf, sa migration au démarrage, sa publication MQTT et WS — **un lot firmware**, pas une carte.
⇒ **Décision : spec dédiée.** Ce lot se limite à **pH, ORP et Filtration**.

### 2. Les libellés Home Assistant SONT alignés

⚖️ **Décision utilisateur : aligner**, en acceptant que des automatisations Home Assistant
envoyant « Manuelle » doivent être mises à jour de son côté.
⇒ **Ce lot comporte donc une part firmware** : `src/mqtt_manager.cpp:2399-2406` (pH) et
`:2418-2425` (ORP) publient un `select` de découverte dont les **options sont des libellés
français**, avec des `command_template` qui les mappent vers `automatic`/`scheduled`/`manual`.
⚠️ **Republication de la découverte obligatoire** (ADR-0085) — sans elle, Home Assistant garde
l'ancienne liste d'options.
⛔ **La valeur interne `manual` ne change pas** : seuls les **libellés** bougent, des deux côtés.

### Périmètre arrêté

| | dans ce lot | ailleurs |
|---|---|---|
| **pH + ORP** (cartes, vocabulaire, injection déconditionnée) | ✅ lot A | |
| **Filtration** (Boost en option, horaires conditionnés) | ✅ lot B | |
| **Libellés Home Assistant** | ✅ part firmware | |
| **Éclairage** | ⛔ | **`feature-162`**, avec le défaut d'affichage existant à corriger |

### ⚠️ Deux défauts EXISTANTS à corriger au passage, relevés par l'orchestrateur

1. **Le sélecteur ORP de la page de réglage ne revient PAS à l'état réel en cas d'échec**
   (`data/app.js:12596-12602`), contrairement au pH (`:12541-12570`).
   ⇒ **L'invariant « un sélecteur qui ment est pire que pas de sélecteur » est DÉJÀ violé
   aujourd'hui.** À corriger avant d'en ajouter deux autres.
2. ⚠️ **Le mode ORP `manual` DÉSARME la chloration de secours** (`web_routes_config.cpp:795-800`),
   et le secours est **refusé** en mode manual. ⇒ Passer en « Arrêt » **depuis l'accueil** peut
   **arrêter un secours en cours en un clic**. ⛔ Absent de la liste de questions de cette spec :
   **à porter au gate**.

### Autres pièges à traiter (orchestrateur)

- ⚠️ **« Programme » désignera DEUX valeurs internes** : `manual` sur la Filtration, `scheduled`
  sur pH/ORP. Cette spec concluait que le problème « se résout de lui-même » — **il ne se résout
  pas, il se déplace**. À trancher par écrit.
- ⚠️ **Hauteur variable + grille `auto-fit minmax(320px,1fr)`** (`app.css:2185`) : les cartes
  d'une même ligne **s'étirent sur la plus haute**. Passer l'ORP en Programme peut donc **gonfler
  la carte voisine**. Cette spec parlait de la hauteur d'une carte, pas de l'effet sur la ligne.
- ⚠️ **Une bannière « Régulation à l'arrêt » a existé puis été RETIRÉE** (feature-152, v2.68.0).
  Le mode « Arrêt » réintroduit cette information sous une autre forme : ⛔ vérifier qu'on ne
  reconstruit pas ce qui a été supprimé pour de bonnes raisons.
- ⚠️ **Contradiction interne de cette spec** : « Hors périmètre » exclut la page de réglage, alors
  que la précision utilisateur impose d'y déconditionner l'injection manuelle **et** d'y changer
  le libellé. ⇒ **Exclusion levée** pour ces deux points précis.

## ✅ VERDICT `pool-chemistry` — GO SOUS 10 CONDITIONS (2026-10-13)

⚠️ **C1, C2 et C4 sont BLOQUANTES et comportent du FIRMWARE. Sans elles : NO GO.**

### ⚠️ C1 — UNE GARDE MANQUANTE, que ce lot allait rendre ATTEIGNABLE

`web_routes_control.cpp:102` → `in.alreadyInjecting = manualInjectPh.active` : **seule l'injection
MANUELLE est regardée**. Une injection **AUTOMATIQUE en cours ne bloque rien**.

⇒ **Aujourd'hui inatteignable** parce que le bouton est masqué hors du mode `manual`
(`app.js:2005`/`:3029`). ⚠️ **Déconditionner l'injection la rend atteignable** — on aurait pu
injecter à la main **par-dessus un dosage automatique en cours**.
⇒ **Ajouter une garde `automaticInjectInProgress` (rang 4, même refus) AVANT de démasquer.**
Texte imposé : `Une injection automatique est en cours — réessayez dans un instant.`

> **C'est le défaut le plus intéressant du lot** : il n'existait pas, il était *caché* par une
> restriction d'affichage que l'on s'apprêtait à lever. Une simplification d'interface a failli
> ouvrir un chemin de dosage.

### ⚠️ C2 — Étalonnage : bloquer côté SERVEUR

Étendre le gate **pré-mutation** à `mqttCfg.calSwitchActive`, pour `ph_regulation_mode` **et**
`orp_regulation_mode`. ⛔ **Ne PAS mettre à jour `ph/orpCalSwitchPrevMode`** — *« ce serait un
second chemin d'écriture vers une mémoire de sécurité »*. Refus **409** :
`Étalonnage en cours — le mode est rétabli à la fin de l'étalonnage.`

### ⚠️ C4 — Secours chlore : LA SEULE friction du lot

`web_routes_config.cpp:796` désarme le secours **sans rien demander**. Depuis l'accueil, c'est un
clic. ⇒ Si `rescue_active`, la bascule ORP → Arrêt ouvre une confirmation :
`La chloration de secours est en cours. Passer en Arrêt l'interrompt. Confirmer ?`
(boutons `Interrompre le secours` / `Annuler`).
⚠️ *« Elle est RARE, donc elle ne s'apprend pas à cliquer sans lire »* — leçon de feature-159.

### Les autres conditions

**C3 — Escalade pH : carte en LECTURE SEULE.** Segments Auto/Programme **désactivés** (⛔ pas
masqués), ligne : `Sécurité pH : régulation arrêtée par l'appareil. Acquittez sur la page pH.`
⛔ Jamais d'acquittement depuis l'accueil ; le 409 s'affiche **sur la carte**, jamais en silence.

**C5 — AUCUNE autre confirmation.** Ni Arrêt→Auto, ni Auto→Arrêt. *« Les plafonds protègent
(9 gardes + présence d'eau + capteurs) ; la friction uniforme est un anti-pattern ici. **La
protection contre l'arrêt accidentel est la VISIBILITÉ permanente du mode sur la carte, pas un
modal.** »*

**C6 — Journalisation : parité ORP.** `:792` journalise **sans l'origine**, contrairement au pH.
Aligner. Niveau `info` ; **`warning` si la transition désarme un secours**.

**C7 — « Arrêt » ne suffit pas.** Sous le sélecteur en Arrêt :
`La régulation automatique ne dose plus. L'injection manuelle reste possible depuis les réglages.`
Sur la **page de réglage**, au-dessus d'Injecter :
`Injection manuelle — disponible dans tous les modes. Les limites de sécurité s'appliquent.`

**C8 — Injection manuelle : OK, mais ⛔ PAS sur la carte d'accueil.** *« Un geste qui dose ne doit
pas être à un clic de l'écran d'accueil. »*

**C9 — Limite permanente validée** : `Volume quotidien 450 mL — limite de sécurité 2 000 mL`.

**C10 — Corriger le défaut EXISTANT d'abord** : `app.js:12596-12602`, le sélecteur ORP ne revient
pas à l'état réel en cas d'échec.

### (j) La bannière retirée n'est PAS reconstruite — vérifié

Celle de feature-152 affichait une **cause de blocage INFÉRÉE** (`phFilterAlarm`), remplacée par
`#dose-block-banner`. Le mode « Arrêt » affiche un **état CHOISI par l'utilisateur**, sur la carte
concernée. **Différence réelle.** ⛔ Ne pas ajouter de bannière de tête « Régulation à l'arrêt ».

### Risques non listés, relevés par le gate

- ⚠️ **Libellés HA** : sans republication de découverte (ADR-0085), Home Assistant enverrait une
  option **inexistante** ⇒ le POST tomberait dans un `else` **silencieux** (`:789`) : mode
  inchangé, **aucun log**. ⇒ **Journaliser en `warning` tout mode rejeté.**
- « Programme » désigne deux valeurs internes : sans danger chimique, mais **un futur lecteur de
  journal confondra** — à écrire dans la doc.
- **Droop** : un utilisateur qui passe en Arrêt parce que « le pH ne rejoint pas la consigne »
  observe probablement l'équilibre de droop du P pur (Ki = 0, feature-025), **pas un défaut**.
  Rien à coder — à garder en tête pour le support.

## 🎨 DESIGN ARRÊTÉ (`ux-ui-designer`, 2026-10-13) — tout est RÉUTILISÉ, rien n'est inventé

**Aucun composant neuf.** Le sélecteur 3 positions (`.segmented`), le bandeau d'escalade
(`.regul-alert--inline`), la modale de confirmation (`confirmDialog`), la ligne label/valeur
(`.detail-row`) et le ton discret (`.field__hint`) **existent déjà** et sont tous repris tels
quels.

### ⚖️ DÉCISION UTILISATEUR : la Filtration GARDE sa grille 2×2

⚠️ **Écart assumé avec la maquette fournie**, qui montrait les quatre modes sur **une ligne**.
Motif : ces quatre boutons sont **déjà en production** (feature-147), déjà validés tactiles et à
390 px, et un commentaire du code (`app.css:913-920`) précise que le **2×2 est délibéré, pas un
repli**. Quatre segments sur une ligne à 390 px donneraient ~80 px chacun — « Programme » devrait
être raccourci, pour un gain purement cosmétique.
⇒ **Changement minimal sur cette carte** : conditionner les horaires au mode, et retravailler le
Boost. ⛔ Ne pas porter la Filtration sur `.segmented`.

### Les points de conception

1. **Boost : un sous-bloc MINEUR, pas une mini-carte.** Le défaut actuel est qu'il **répète
   l'en-tête complet** — icône 34 px, libellé en majuscules, badge : **le même gabarit que la
   carte elle-même**. ⇒ `.card__option` : surlignage « OPTION » discret, titre 14 px sans
   majuscules forcées, badge en ligne, **pas d'icône dupliquée**. ⛔ Mécanique JS **inchangée** —
   seul le poids visuel change.
2. **pH/ORP : `.segmented` réutilisé tel quel.** « Auto / Programme / Arrêt » (4+9+5 caractères)
   tient **plus facilement** que l'actuel « Automatique / Programmée / Manuelle », déjà en
   production dans **ce même composant**. ⇒ gain net, **aucun `flex-wrap` à prévoir**.
   ⚠️ **Défaut latent à corriger au passage** : `.segmented__btn` n'a **pas** de
   `min-height:44px` (seul `.segmented--num` l'a). L'ajouter sécurise aussi **les pages de réglage
   existantes**.
3. **La mesure reste visible dans TOUS les modes** — c'est une lecture de capteur, pas une action.
   Seuls la **cible** et son marqueur disparaissent hors Auto : rien n'est poursuivi.
   En Programme : deux `.detail-row` **en lecture seule** (volume, limite). ⛔ Aucun champ
   éditable sur la carte — pas de second chemin d'écriture.
4. ⚠️ **LE CŒUR DU LOT — rendre le mode visible AVANT le sélecteur.** Une ligne « Mode Auto / Mode
   Programme / Mode Arrêt » dans l'**en-tête** de la carte, au même endroit que celle déjà posée
   sur la Filtration.
   > **Redondance VOLONTAIRE** avec l'état actif du sélecteur, qui ne se voit qu'en regardant le
   > bas de la carte. **Le gate a dit que la protection contre l'arrêt accidentel EST cette
   > visibilité permanente** — pas un modal. ⛔ **Ne pas la supprimer en croyant simplifier : ici
   > la redondance a un rôle de SÉCURITÉ documenté, pas de bruit.**
5. **Hauteur variable : le problème n'existe QU'au-dessus de 980 px.** Vérifié — sous 980 px la
   grille passe à une colonne, donc **aucun étirement sur mobile**. Correctif : `align-items:
   start` sur `.grid--status-cards`, pour que chaque carte garde sa hauteur **naturelle**.
6. **États d'exception** : escalade ⇒ `.regul-alert--inline` au-dessus du sélecteur, segments
   `disabled` + `aria-disabled` (⛔ jamais `hidden`), pill actif en teinte `--warn` **et** texte
   porteur de la même information (daltonisme couvert) · étalonnage ⇒ même bandeau · secours ⇒
   `confirmDialog` en `danger` · échec ⇒ retour au **dernier mode confirmé par le serveur**,
   réutilisant le style « information non rafraîchie » **déjà existant** plutôt qu'un troisième
   traitement du même fait.

### ⚠️ Point à vérifier par `architect` / `web-ui-developer`

Le tableau de bord reçoit-il **déjà** `calSwitchActive` ? Si oui, désactiver les segments
**proactivement** ; sinon, se contenter du refus serveur — mais ⛔ **alors ne jamais laisser le
sélecteur revenir en arrière silencieusement** (invariant « un sélecteur qui ment »).

### Note pour la doc, pas pour l'écran

« Programme » désignera deux valeurs internes (`manual` côté Filtration, `scheduled` côté pH/ORP).
**Acceptable visuellement** — les deux ne sont jamais côte à côte sur un même contrôle — mais **à
écrire** : un futur lecteur de journal confondra.
Et ⛔ **ne pas ajouter de badge d'état à pH/ORP** : contrairement à la Filtration (qui peut être
« en Auto hors créneau »), il n'y existe **pas d'état réel distinct du mode**.

## ✅ ARBITRAGES `pool-chemistry` APRÈS REVUE (2026-10-13)

### L8 est DÉGELÉ — un texte de sécurité ne peut pas nommer un bouton inexistant

`data/app.js:7648` disait « *la régulation du chlore est en mode **Manuel**. Repassez en
**Automatique** ou **Programmée*** » — **trois libellés qui n'existent plus** depuis que
l'utilisateur a choisi « Auto / Programme / Arrêt ».

⛔ **L'option « assumer la divergence » est ÉCARTÉE** : *« un texte de sécurité qui nomme un bouton
inexistant n'est pas une divergence cosmétique, c'est une **CONSIGNE INAPPLICABLE** — exactement
le motif "l'écran ment" que ce projet traque. »*

**Texte imposé (L8 v2, feature-161)** :
> `Chloration de secours indisponible : la régulation du chlore est en mode Arrêt. Repassez en Auto ou Programme pour l'utiliser.`

⚠️ La formulation évidente a été écartée parce qu'elle *« dit "Repassez en Auto ou Programme" sans
dire **OÙ**, alors que le sélecteur est désormais sur DEUX surfaces »* — d'où le « pour l'utiliser ».

Contraintes attachées : ⛔ les mots **Auto / Programme / Arrêt** doivent être **strictement** ceux
des segments ; **si un segment change, ce texte change dans le même lot** · ⛔ **ne jamais
mentionner la valeur interne `manual`** · le commentaire reste, **daté feature-161**, en marquant
la v1 comme périmée.

⚠️ **GREP OBLIGATOIRE avant clôture** — *« L8 ne sera pas le seul »* : chercher tout texte citant
« Manuel », « Automatique » ou « Programmée » dans `data/` **et** `docs/user/src/`. Chaque
occurrence est soit alignée, soit **justifiée par écrit**.

### C4 est ÉTENDUE à la page de réglage

Relevé en revue : la confirmation n'existait que sur l'accueil, alors que les deux surfaces
partagent désormais **le même chemin d'écriture**. Le gate révise sa propre condition, pour trois
raisons :

1. ⚠️ **« La friction protège l'ACTION, pas la SURFACE. Ce qui est dangereux, c'est d'interrompre
   un secours en cours ; l'endroit d'où part le clic n'y change rien. »**
2. ⚠️ **« Un même chemin d'écriture avec deux niveaux de protection, c'est une GARDE
   CONTOURNABLE — il suffit d'aller sur l'autre page. »** Le projet a déjà payé ce motif.
3. Le secours est **rare** ⇒ la confirmation ne devient un réflexe appris **sur aucune** des deux
   surfaces (la leçon de feature-159 tient).

⇒ Confirmation sur **TOUTE** bascule ORP → Arrêt venue de l'interface.
⚠️ **Implémentation MUTUALISÉE : une fonction, deux appelants.** ⛔ Pas deux copies du texte.

**Non couvert, et assumé par écrit** : MQTT et l'API HTTP directe restent **sans confirmation** —
pas de surface d'interaction. Le désarmement y est **déjà journalisé en `warning`**, et *« c'est la
protection disponible sur ces chemins, et elle suffit »*.

### ⚠️ Le trou de couverture n'était pas théorique — il a MORDU

La revue a montré que le bloquant n°1 **est** ce trou : `initSegmented` déplace la pastille **avant**
l'ouverture de la confirmation ⇒ **annuler** laissait l'écran sur « Arrêt » pendant que l'ORP
régulait et que le secours tournait. *« C'est exactement "un sélecteur qui ment", dans le SEUL cas
où ça coûte cher. »*
⇒ Extraction imposée d'une fonction **pure** `resolveSelectorMode(requested, lastConfirmed,
outcome)` : le DOM reste hors test, **la décision devient tuable**. Harnais par **extraction du
texte réel** (ADR-0099), 18ᵉ contrôle d'hygiène et job CI.

### Contre-visite : Approved — et trois points INSTRUITS, pas ratifiés

Les deux bloquants sont levés. Ce qui mérite d'être conservé, ce sont les raisons, pas le verdict.

**1. Le déplacement de la confirmation dans le chemin partagé ne crée pas d'angle mort.** Le
no-op s'évalue **avant** l'ouverture, donc recliquer « Arrêt » déjà actif n'ouvre rien ;
l'annulation repose la pastille que `initSegmented` avait déjà déplacée. Un seul cas résiduel, et
il **penche du bon côté** : sans config chargée, le mode précédent est un repli deviné ⇒ la
confirmation s'ouvre **quand même** si un secours tourne. ⚠️ **À garder tel quel** — c'est un
fail-safe, pas un oubli.

**2. Panne réseau et refus 409 partagent un même verdict, et c'est le bon découpage.** Pour
*cette* décision, les deux conduisent à l'identique : revenir au dernier mode confirmé. Les
distinguer ici créerait **deux branches au comportement égal**, donc du code mort **intuable par
mutation** — le contraire de ce que le lot cherche.

⚠️ **RÉSERVE CONSIGNÉE, hors périmètre de ce lot.** Il existe un cas où les deux diffèrent
vraiment : la **réponse perdue alors que l'appareil A appliqué** le changement. Dans ce cas, le
libellé « modification non enregistrée » de `sendConfigDetailed` est **faux**. Ce défaut est
**antérieur** à feature-161 et concerne **tous** ses appelants, pas le sélecteur.
⇒ Il ne se corrige **pas** dans `resolveSelectorMode` : l'écran doit répéter le dernier dire de
l'appareil (ADR-0100), et la réconciliation existe déjà (rechargement de config + trame
temps réel). Si une distinction devient nécessaire, elle appartient au **message** et au
**renvoi**, jamais à la fonction de décision.

**3. L'affirmation sur la page de réglage a été CHIFFRÉE, pas répétée.** À 390 px, une
demi-colonne vaut ~170 px ; « Régulation pH / ORP » = 19 caractères ≈ 139 px + 32 px de marge
≈ 171 px ⇒ l'onglet était **déjà sur deux lignes AVANT ce lot**, et la hauteur minimale n'y
change rien. Seuls les onglets d'une seule ligne gagnent ~7 px, soit **+30 à 40 px** au total.
⇒ Aucun changement. ⚠️ Et le motif vaut au-delà de ce lot : *une affirmation sur un rendu se
compte, elle ne se plaide pas.*

## Lot C — le verrou d'étalonnage ne tenait qu'un chemin sur trois

Signalé par `doc-writer` en fin de lot, **vérifié dans le code avant d'être relayé**, puis instruit
par `pool-chemistry` : **GO SOUS CONDITIONS**, à fermer **dans ce lot**.
⚠️ Motif du gate : *« Un gate à moitié posé est pire qu'absent : il crée la confiance sans la
protection. »* La condition **C2 n'est pas satisfaite** tant qu'un second chemin écrit la même
donnée sans la garde — c'est le motif « garde contournable » déjà condamné pour C4.

### Ce que l'instruction a corrigé dans MON analyse

J'avais écrit que le sens pH était **sûr**. ⛔ **Faux en général.**
- Vrai **uniquement** avec `ph_minus` (erreur négative + plancher `kPhFloorCorrected` 6,70).
- En **`ph_plus`**, un tampon à 4,0 est très sous consigne ⇒ **injection de base**, et le refus
  d'alcaliniser est un seuil **HAUT** (`src/ph_guard_logic.h:73`) : **aucun plancher ne l'arrête**.
- ⚠️ Et le tampon 4,0 **n'arme pas** l'inhibition ORP : ce seuil est **haut** lui aussi
  (`kPhOrpInhibit` 7,60, `src/pump_controller.cpp:1384`). À 4,0 le frein est **LEVÉ, pas posé** —
  le cas que je croyais protecteur est **aggravant**.

⇒ Leçon : *« sens sûr » n'est pas une propriété du pH, c'est une propriété du PRODUIT installé.*

### Deux trous que personne n'avait demandé de chercher

1. 🚨 **`src/web_routes_config.cpp:854`** — `orp_enabled` accepté en **écriture directe**, hors du
   gate. L'équivalent pH avait été fermé en **v2.25.0 (B7, ligne 792)** ; **l'ORP ne l'a jamais
   été.** Portée **vérifiée** : `pump_controller` ne lit jamais `orpEnabled` ⇒ **pas** un chemin de
   dosage, mais une **divergence d'affichage** — « l'écran ment » pendant une bascule.
2. 🚨 **`src/config.cpp:551-563`** — si le trou MQTT sort les **deux** modes de `manual`,
   `saveMqttConfig()` persiste et le **redémarrage suivant purge la session en silence** : bandeau
   disparu, alerte d'oubli jamais émise, **sonde toujours dans le tampon**.
   ⚠️ **Le trou n'ouvre pas seulement le dosage : il EFFACE LA TRACE.**

### Conditions bloquantes

| # | Objet | Vérifié par |
|---|---|---|
| **C-1** | Gate MQTT pH **et** ORP, **après** le gate `phAutoManual*` (ordre inchangé) ; refus sur la **sortie** de `manual` seulement ; **même fonction pure** que HTTP | test natif + **mutation** (`true`/`false` constant ⇒ ≥ 2 cas morts) |
| **C-2** | `orp_enabled` n'est plus accepté en écriture — état **dérivé du mode**, aligné sur le pH | parité `/data` ↔ WS + mutation |
| **C-3** | Commentaire nommant **C-1 comme précondition** de la purge de boot | revue |
| **C-4** | Aucun refus muet : `warning` **et** `publishTargetState()` | revue / harnais de libellés |

⛔ Texte du journal **imposé mot pour mot**, identique au 409 HTTP (`web_routes_config.cpp:620-623`)
— l'utilisateur doit lire **la même phrase** sur les deux chemins :
`Mode régulation pH REFUSÉ (MQTT → <payload>) : étalonnage en cours — le mode est rétabli à la fin de l'étalonnage.`
(et son miroir ORP).

### ⚠️ Recommandations NON bloquantes — à ne pas perdre

1. **Défense en profondeur, hors de ce lot** : refuser **tout** dosage tant que `calSwitchActive`
   est vrai, **dans `canDose()`** — donc **indépendamment du mode**. ⇒ *Cela rendrait ces trois
   trous inoffensifs.* Les gardes posées ici protègent **trois chemins nommés** ; celle-là
   protégerait **le chemin final**. **Spec séparée.**
2. **`ph_plus` n'a aucun plancher bas.** Hors périmètre, mais c'est un défaut réel, indépendant de
   l'étalonnage. **Spec séparée.**

## Clôture — ce qui est livré, et ce qui ne l'est pas

**Version 2.77.0.** Mesuré, pas recopié (ADR-0073) : `pio run -e esp32dev_core3` **SUCCESS** ·
`pio test -e native` **2 173 cas / 70 suites**, zéro régression · **18/18** contrôles d'hygiène ·
**5 535** liens relatifs résolus.

### Verdicts
`pool-chemistry` GO sous 10 conditions, puis arbitrages après revue, puis **GO sous 4 conditions**
sur le lot C · `code-reviewer` **Approved** sur le front (après 2 bloquants levés) **et** sur le
firmware (2 mineures, appliquées) · `ux-ui-designer` mandat sur l'écran entier · `doc-writer`
deux passages.

### Les deux mineures de revue, appliquées
1. `src/config.cpp` — le commentaire C-3 **nomme désormais** la migration NVS comme le seul
   écrivain du mode qui ne passe par aucun gate, et dit **pourquoi elle est inatteignable** (la
   clé `cal_sw_active` est postérieure). ⚠️ L'exception est **vue** au lieu d'être redécouverte.
2. `src/mqtt_manager.cpp` — les branches « mode invalide » resynchronisent HA elles aussi :
   **C-4 n'a plus d'exception.** ⚠️ *Une règle à exception se perd.*

### ⛔ Ce qui N'EST PAS prouvé (assumé, pas masqué)
`mqtt_manager.cpp` et `web_routes_config.cpp` **ne compilent pas nativement**. Le câblage mutex,
l'**ordre d'exécution effectif** des deux gates pH et l'**exactitude des textes de journal** ne
sont vérifiés que par **relecture** (`code-reviewer`, comparaison octet à octet) et par
`pio run` — **pas** par un test qui exécute ce code. Écrit ici pour qu'un futur lot ne prenne pas
cette zone pour couverte.

### Suites à ouvrir
| # | Sujet | Pourquoi |
|---|---|---|
| **feature-162** | Carte Éclairage | Sortie du lot sur décision utilisateur ; corriger aussi `manualOverride` jamais publié |
| **à nommer** | Refus de dosage **au niveau `canDose()`** tant que l'étalonnage est ouvert | ⚠️ **Le plus utile des trois** : les gardes de ce lot protègent **trois chemins nommés** ; celle-ci protégerait **le chemin final**, et rendrait inoffensifs les trous **pas encore trouvés** |
| **à nommer** | `ph_plus` n'a **aucun plancher bas** | Un tampon pH 4,0 déclencherait une injection de base sans butée |
| **à nommer** | `sendConfigDetailed` annonce « modification non enregistrée » même quand l'appareil **a** appliqué | Antérieur au lot, touche **tous** les appelants |
| **à nommer** | **11 ADR absents de l'index** (0083, 0087-0091, 0094, 0095, 0099, 0100, 0101) | Dette antérieure mesurée pendant ce lot ; le 0108 a été posé |
