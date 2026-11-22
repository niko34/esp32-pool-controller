## Métadonnées
- **ID** : bug-197
- **Slug** : le-libelle-hors-gel-annonce-une-protection-qui-n-existe-pas
- **Type** : bug
- **Statut** : done
- **Priorité** : haute
- **Version cible** : v3.0.2 — ✅ livrée
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (trouvé en vérifiant un point laissé ouvert par son étude)

## Domaines touchés
- [x] Filtration / Éclairage
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **l'eau qui gèle fait éclater les canalisations** ; arbitrage produit
- [ ] embedded-developer — selon l'option retenue
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
La carte Filtration affiche la pastille **« Hors gel »**, qui laisse croire que l'appareil
protège l'installation du gel. **Il ne la protège pas.** Le libellé ne fait que *décrire*
une coïncidence : l'eau est sous 5 °C **et** la pompe se trouve déjà tourner.

## Contexte
Trouvé le **2026-11-18** en vérifiant un point que l'étude d'ergonomie de feature-195
avait laissé ouvert (« que fait l'arrêt forcé à la pause hors gel ? »). La réponse est
qu'il n'y a **rien à interrompre**.

**Vérifié dans le code, les deux côtés :**
- `data/app.js:5391` — `getFiltrationState()` : si `temperature < 5.0` **ET**
  `filtration_running`, la pastille devient « Hors gel ». C'est un **libellé calculé dans
  le navigateur**. Il ne commande rien, et il disparaît si la pompe s'arrête — c'est-à-dire
  **exactement quand le risque est maximal**.
- `src/filtration.cpp:281` — sous `kFiltrationRefTempMinC`, le firmware journalise
  « Hivernage » et **conserve le planning**. Aucun démarrage de pompe n'est commandé.
- Aucune autre logique de gel n'existe dans `src/` : les occurrences de « freeze » /
  « gel » concernent le **gel de l'intégrale du PID**, sans rapport.

⚠️ **Pourquoi c'est grave et pas cosmétique.** L'appareil est **vendu à des clients**.
Un propriétaire qui lit « Hors gel » comprend « l'appareil s'en occupe » et ne fait rien.
Si la pompe est à l'arrêt — mode Arrêt, Auto hors créneau, hivernage — l'eau immobile gèle
et **les canalisations éclatent**. Le coût est matériel et élevé, et le libellé y a
contribué en rassurant à tort.

## 🔬 Vérification du 2026-11-22 — ⚠️ la fiche était périmée sur trois points

⛔ **Relue contre le code du jour avant tout travail** (consigne permanente : une fiche
se vérifie avant d'être traitée). Ce qui suit REMPLACE les constats d'origine.

| La fiche disait | Le code dit, le 2026-11-22 |
|---|---|
| `data/app.js:5391` | `getFiltrationState()` est à **`data/app.js:5884`**, la pastille à **`:5894`** |
| « sous `kFiltrationRefTempMinC` » — sous-entend **un seul** seuil | 🔴 **DEUX seuils divergents** : l'écran bascule à **5,0 °C**, l'appareil hiverne à **4,0 °C** |
| la pastille est **ambre** | **feature-195 est livrée** : la puce est **grise** (`dashFiltrationTone(forced, state.frost, state.running)`, `data/app.js:5825`) |

### 🔴 Le seuil 5,0 n'est PAS une constante
`data/app.js:5894` porte `temp < 5.0` **écrit en dur dans le JavaScript**. ⛔ Il n'existe
dans aucun fichier de constantes, et il ne correspond à **rien** côté appareil :
`src/constants.h:3285` fixe `kFiltrationRefTempMinC = 4.0f`. ⇒ Entre 4,0 et 5,0 °C,
l'écran dit « Hors gel » pendant que l'appareil, lui, **calcule encore des créneaux
normalement**. Deux chiffres, deux significations, aucun lien.

### ✅ Ce qui se confirme — aucune protection du gel n'existe
- `grep -rni "freeze|frost|antigel"` sur `src/` ne rend **rien** hors du **gel d'intégrale
  du PID** (sans rapport, la fiche le disait déjà).
- `src/filtration.cpp:281` et `:704` : sous 4 °C, l'appareil journalise « Hivernage » en
  `info` et **conserve le planning existant**. ⚠️ Nuance utile : il ne **démarre** pas la
  pompe, mais il ne l'**arrête** pas non plus — en mode Auto, le dernier créneau calculé
  continue de tourner. ⇒ Le danger porte sur les modes **Arrêt**, **hors créneau**, et sur
  un appareil laissé en hivernage.

### 📸 Les deux états, photographiés à 390 px (`-f temperature=3`)
- **pompe en marche** → pastille « **Hors gel** » ;
- **pompe arrêtée** → pastille « **Arrêtée** ». ⛔ **Pas un mot sur le froid**, alors que
  c'est le cas dangereux. C'est le cœur du défaut, et il est désormais prouvé en image.
- 🔴 **CORRECTION du 2026-11-22, trouvée par `ux-ui-designer` et vérifiée en capture.**
  ⛔ Ma première capture était un **artefact** : forcer `temperature=3` **sans** forcer
  `water_present=false` fabrique un état qui **n'existe pas sur l'appareil**. Pompe arrêtée en
  mode `Managed`, `water_present` passe à **faux**, donc `measuresAreValid()`
  (`data/app.js:5005`) est **faux**.
  ⇒ **Le VRAI écran est pire** : la carte Température n'affiche pas « 3,0 °C / À jour », elle
  affiche « **En attente** », « **-- °C** », « Mesures non représentatives », « Filtration
  arrêtée ». ⭐ **L'appareil CACHE la température précisément quand le gel menace.**
  ⇒ Le seul mot de froid de toute la page est la légende d'échelle « Repère : 4 °C hivernage ».
  🔴 **Conséquence de conception, contraignante** : l'information « froid » ⛔ **ne peut PAS
  vivre sur la carte Température**, ni dépendre de `measuresOk`. Elle vit sur la carte
  Filtration et lit `data.temperature` **directement**.
  ⚠️ **Conséquence d'OUTILLAGE** : toute capture de cet état force **aussi** `water_present=false`.
  Sans ça, on photographie un écran imaginaire — c'est exactement ce qui vient d'arriver.

### 🔴 Défaut SUPPLÉMENTAIRE, non signalé par la fiche — le guide client
`docs/user/src/guide-utilisation.html:458` écrit, au client qui achète l'appareil :

> « l'**état** réel de la pompe (« En marche » / « Arrêtée », ou « **Hors gel** » en orange
> **pendant une pause anti-gel**) »

⛔ **« pendant une pause anti-gel » affirme une FONCTION.** L'appareil n'en a aucune.
⚠️ C'est **plus grave que la pastille** : une pastille est un mot sur un écran, une phrase
de guide est une **promesse écrite**. ⇒ Le correctif de ce guide est **obligatoire quelle
que soit l'option retenue**, et « en orange » y est faux de surcroît depuis feature-195.


## Comportement attendu
À trancher par `pool-chemistry` et l'utilisateur — **deux options, pas une**.

1. **Dire la vérité** (correctif minimal, sans firmware) : retirer ou reformuler le
   libellé pour qu'il décrive un **constat**, pas une protection. Par exemple « Eau très
   froide ». Et prévenir le propriétaire que la protection du gel est **à sa charge**.
2. **Faire ce que le libellé promet** (demande de fonction) : faire tourner la filtration
   sous un seuil de température, avec les gardes qui vont avec.

⛔ **Ce qui n'est pas acceptable, c'est l'état actuel** : un mot qui promet une protection
absente.

## ✅ A1 — ARBITRAGE TRANCHÉ le 2026-11-22 : **OPTION 1**, l'option 2 est CLOSE

**Verdict `pool-chemistry` : NO GO FRANC sur l'option 2**, pour **trois raisons indépendantes
dont chacune suffirait seule**. ⛔ Écrit ici pour de bon : **les gates ne gardent aucune mémoire
entre deux passages**, et sans ce paragraphe l'option 2 reviendra au premier hiver.

### ⛔ Pourquoi l'option 2 (commander la pompe sous un seuil) est refusée

1. **Elle mesure la mauvaise grandeur.** Le gel casse les canalisations quand l'**air** passe
   sous 0 °C pendant des heures. L'appareil ne mesure que l'eau, et 30-50 m³ perdent de l'ordre
   du **demi-degré** par nuit à −5 °C. ⇒ Air à −6 °C avec eau à 6 °C — **le cas le plus fréquent
   de l'hiver** — ne déclencherait **rien**, et c'est pourtant cette nuit-là que les tuyaux
   gèlent. ⚠️ La « protection » s'allumerait **après le sinistre**. Un vrai hors-gel du commerce
   pilote sur une **sonde d'air extérieure** — ⛔ interdite par la contrainte « rien à installer
   chez le client ».
2. 🔴 **Marche à sec — et c'est le point décisif.** ⭐ **La condition de déclenchement et la
   condition de destruction sont la MÊME condition** : une eau sous 4 °C, c'est la saison où
   l'on hiverne — canalisations vidangées, vannes fermées, bouchon de pompe retiré. L'option 2
   armerait le relais **précisément là**, et **chaque jour tant que l'eau reste froide**. La
   garniture mécanique d'une pompe centrifuge est **lubrifiée par l'eau** : à sec elle est
   détruite en **quelques minutes** (300 à 800 € de pompe). ⛔ Et l'appareil **ne peut pas le
   voir** : ni débitmètre ni pressostat, et en ajouter un est interdit par la même contrainte.
   ⚠️ Ce n'est pas une hypothèse — [incident du 23/08/2026](../../../docs/incidents/README.md),
   bypass des sondes fermé : **une vanne fermée est invisible au firmware**.
3. **Elle rouvrirait le dosage, à 2 °C.** En mode `Managed` (celui de l'utilisateur), la présence
   d'eau **EST** l'état commandé de la filtration (`src/dosing_logic.h:69`,
   `src/pump_controller.cpp:1754`). ⇒ Une marche forcée par le froid **ouvre la garde n°3 sans
   qu'une seule ligne ne soit écrite pour ça**. Or sous ~10 °C l'activité biologique est nulle :
   tout ce qui serait injecté serait **du surdosage par construction**. ⚠️ Et l'ORP **chute avec
   la température** à chlore constant — la boucle lirait « il manque du chlore » et compenserait.
   ⛔ `dosing_logic.h` ne porte **aucune garde de température** (vérifié). ⚠️ Si le circuit est
   vidangé, acide **et** hypochlorite non dilués dans un tronçon mort : c'est l'incident du
   11/10/2026, **avec les deux produits** — donc le risque de **chlore gazeux**.

⇒ **L'option 2 livrerait le même mensonge qu'aujourd'hui, avec en plus une pompe détruite.**
⚠️ Et la variante « en option, activable » ne la sauve pas : ⛔ c'est **une configuration chez
le client**, interdite par la contrainte absolue du projet.

### ✅ Option 1 retenue — et elle coûte **ZÉRO ligne de firmware**

⭐ **Correction apportée au rapport `pool-chemistry`** : il demandait un champ NEUF servi par le
firmware pour savoir si la sonde « eau » est identifiée. **Inutile — le garde-fou existe déjà.**
`data/app.js:5263` porte **`waterRoleUnknown`**, bâti sur `sondes_identified` + `sondes_detected`,
**servis des DEUX côtés** (`web_routes_data.cpp:383`, `ws_manager.cpp:1026`), avec sa limite
assumée écrite en commentaire. ⇒ Le lot reste **entièrement frontend + documentation**.

### 🔴 Condition de sécurité héritée de l'arbitrage — la sonde peut être le COFFRET

⛔ `src/sensors.cpp:1304` — `getTemperature()` **retombe sur n'importe quelle sonde détectée**
si aucun rôle « eau » n'est attribué, et c'est bien elle que `/data` (`web_routes_data.cpp:370`)
et le WebSocket (`ws_manager.cpp:954`) servent sous la clé `temperature`. ⇒ Sur un appareil dont
les sondes ne sont pas identifiées, « Hors gel » peut s'afficher parce que **le boîtier
électronique** est à 4 °C. ⚠️ **Toute mention du froid se conditionne à `!waterRoleUnknown`.**

### ⚠️ Et pompe ARRÊTÉE, la sonde ne mesure pas le bassin
Elle est sur la **ligne d'aspiration** (`src/constants.h:3289` le dit : « elle lit ce que
l'aspiration prélève »). Sans circulation, ce tronçon s'équilibre avec l'air du local technique.
⭐ **Le projet a DÉJÀ statué sur ce point, en code** : `src/filtration.cpp:248` refuse de calculer
un planning tant que `isCirculationSettled()` est faux — motif écrit, « éviter de figer un
planning sur l'eau **stagnante des tuyauteries** ». ⇒ Il serait incohérent d'afficher comme
température du bassin ce que le firmware refuse d'utiliser dans ces conditions.
ⓘ **Nuance qui joue POUR l'affichage** : pour la question du **gel**, cette mesure de tronçon
stagnant est **plus pertinente** que celle du bassin — ce sont les canalisations qui éclatent,
pas les 40 m³. ⇒ On l'affiche, **en disant ce qu'elle est**.

### ✅ A2 — le seuil : **4,0 °C**, et le `5.0` en dur disparaît
Le `5.0` de `data/app.js:5894` **ne correspond à rien** : aucune constante, aucun comportement —
un nombre magique dans le navigateur, contraire à la convention du projet. **4,0 =
`kFiltrationRefTempMinC`** décrit un **changement réel** de comportement de l'appareil.
⛔ L'objection « 4 prévient plus tard que 5 » **tombe avec la raison n°1** : un avertissement calé
sur la température de l'**eau** n'avertit pas « tard », il avertit **au mauvais moment**, à 5
comme à 4. ⇒ **Un seul seuil, pour un seul message factuel.**
ⓘ Précédent du projet : `data/app.js:3692` porte déjà `TEMP_GAUGE_MARK_LOW = 4` avec un
commentaire citant `kFiltrationRefTempMinC`. **Le patron existe, on le suit.**

### Ce que le texte a le DROIT d'affirmer
⭐ **L'appareil COMPTE une température, il ne garantit rien. Il est TÉMOIN, jamais AGENT.**
- ✅ « La température relevée est de X °C. » · « Sous 4 °C, l'appareil ne recalcule plus le
  créneau automatique et conserve le planning existant. » · « La protection contre le gel n'est
  pas assurée par cet appareil : elle est à la charge du propriétaire. »
- ⛔ **Interdits partout** (interface, journal, MQTT, Home Assistant, guides) : « Hors gel »,
  « anti-gel », « protection », « pause ». Tout mot qui fait de l'appareil un agent.
- ⛔ Et **jamais** « l'eau du bassin est à X °C » quand la pompe est arrêtée.
- ⛔ **Valeur absente ou sonde figée ⇒ on se tait.** Ne jamais déduire « pas froid » d'une absence
  de mesure.

### 📸 Deux constats de plus, vus sur les captures (aucun agent ne les avait relevés)
- **Marche forcée à 3 °C** : la pastille dit « Hors gel », mais le titre de la carte dit **déjà
  « Marche forcée »** en orange. ⇒ Le conflit de priorité de teinte est **moins grave** que ce que
  `DESIGN_SYSTEM.md` l.763-769 laisse craindre — à rejuger, pas à dramatiser.
- 🔴 **Les deux surfaces divergent DÉJÀ** : pompe arrêtée à 3 °C, le tableau de bord affiche
  « Arrêtée » en **gris**, la page Filtration l'affiche en **ROUGE**. Même état, même source,
  deux teintes. ⇒ À traiter dans ce lot (B4).


## 🎨 B — Ergonomie : décisions ARRÊTÉES le 2026-11-22 (`ux-ui-designer`, mandat ENTIER)

### B1 — La pastille redevient ce qu'elle doit être, et le froid en sort
⭐ **Le défaut de fond** : « Hors gel » ne s'**ajoutait** pas à l'état de la pompe, il le
**REMPLAÇAIT** — l'état était alors **perdu**. ⇒ La pastille **ne dit plus jamais le froid** :
« En marche » / « Arrêtée », rien d'autre.
**Disparaissent** : la branche `frost` de `getFiltrationState()`, le champ `frost`, le paramètre
`frost` de `dashFiltrationTone()`, le `5.0` en dur, et les **4 occurrences** de « Hors gel » dans
`data/app.js` (commentaires l.4025 · l.5822 · l.5894 · le texte lui-même).
ⓘ La carte reste à 5 blocs et gagne **une** bande conditionnelle, **absente 99 % de l'année**.

### B2 — Le froid vit dans une BANDE sous le contexte, pas dans une pastille
Entre `#detail-filtration-mode` et `.dash-acts`. ⭐ Le design system le dit déjà : une exception
est « **une bande sous la mesure, jamais une seconde pastille** ».
Réemploi de **`.sensor-badge.sensor-badge--warning`**, écrite par `setBadge()`.
⛔ **Écartés, avec motif** : la pastille (surchargée — c'est le défaut) · `.dash-context` (une
ligne grise de 13 px ne se lit pas comme un avertissement) · un bandeau de page (hors de la carte
concernée).
Nœud : `<div class="sensor-badge" id="filtration-frost-badge" style="display:none" role="status" aria-live="polite"></div>`.
Écrivain **unique** : `updateFiltrationBadges()`. Calcul **partagé** par une seule fonction
(`frostReading(data)` → `{cold, tempText}`), appelée aussi par la page Filtration et la carte
Température. ⛔ Jamais deux expressions pour le même fait.

### B3 — Les textes, mot pour mot
| Cas | Texte | Car. |
|---|---|---|
| ligne 1, pompe **en marche** | `Eau à 3,0 °C (sous 4 °C).` | 25 |
| ligne 1, pompe **arrêtée** | `Eau des tuyaux à 3,0 °C (sous 4 °C).` | 36 |
| ligne 2, **toujours** | `Le gel reste à votre charge.` | 28 |
⭐ La ligne « pompe arrêtée » dit **ce que la sonde lit**, sans parler du bassin — elle respecte
la contrainte posée en A1.
✅ **Vérifié à 390 px** (et non affirmé) : bande à 12 px/600, ~306 px utiles ⇒ ~48 caractères par
ligne. **Chaque ligne tient sur UNE ligne.** Tient encore à **320 px** (36 car. × 6,3 px ≈ 227 px).
Valeur par `fmtFr(Math.round(t*10)/10, 1)` ; condition sur la valeur **arrondie** `< 4`
⇒ ⛔ on n'écrit jamais « 4,0 °C (sous 4 °C) ». **Pas d'hystérésis.**

### 🔴 B3bis — Les garde-fous : la bande est ABSENTE si…
- `data.temperature` nul ou `NaN` ;
- **`waterRoleUnknown`** (⚠️ le calcul de `data/app.js:5263` est à **extraire en helper**, ⛔ pas
  à dupliquer) — c'est la garde « la sonde pourrait être le coffret électronique » posée en A1 ;
- l'application est **hors ligne** (`isAppOffline()`) : une température périmée ne déclenche rien ;
- `tempFrozenAlarm === true` **et** `measuresOk` (pompe en marche). ⚠️ Pompe **arrêtée**, une sonde
  qui rend la même valeur est **normale** — eau immobile. On l'ignore.
- ⛔ **NE PAS conditionner à `measuresOk`** : ce serait **recréer le cas muet**.

### B4 — Les deux surfaces, et la divergence de teinte TRANCHÉE
Page Filtration : **la même bande** (`#filtration-page-frost-badge`, mêmes segments) **tout en
haut** de `#filtration-content`, ⛔ **pas** dans la carte « Contrôle manuel » — celle-ci peut être
`display:none` selon le mode d'installation. Modificateur `.sensor-badge--page` (marge basse 12 px).
🔴 **Verdict sur le gris/rouge que j'avais constaté : le GRIS est juste, le ROUGE est faux.**
« Arrêtée » est un état **normal** — en mode Auto, la pompe est arrêtée **chaque nuit**. Ce n'est
jamais une alarme. ⇒ `getFiltrationState()` émet `pillClass: ''` pour « Arrêtée » (neutre) et
`'ok'` pour « En marche ». **`'bad'` et `'mid'` ne sont plus émis par cette fonction.**

### B5 — Composants partagés : AUCUN n'est modifié
Seule la **chaîne de classe émise** change ⇒ ⛔ application partielle impossible par construction.
Inventaire **réel** pour mémoire : `.pill` = **7** éléments d'`index.html` (l.642, 1214, 1726,
2175, 2323, 2912, 2934) + écrivains `app.js:5673` et `:5833` — **seul `:5833` change**, et
`.pill.mid` garde un emploi (`install-ext-pill`, l.2175). `.dash-pill` = 6 éléments, **un seul**
change de ton.
⚠️ **`.sensor-badge` est RÉEMPLOYÉE DANS UN CONTEXTE NOUVEAU** (carte Filtration + page
Filtration) ⇒ `docs/DESIGN_SYSTEM.md` §5.24 doit déclarer ce **contexte d'emploi**. C'est
précisément le déclencheur écrit dans CLAUDE.md.

### B6 — Priorité de teinte : JUGÉE, et la règle est SUPPRIMÉE
Plus de « Hors gel » dans la pastille ⇒ **le conflit disparaît par construction**.
⇒ `docs/DESIGN_SYSTEM.md` : réécrire la ligne « Hors gel » du tableau (l.744), **supprimer**
l.752-753 **et** l.763-769. En marche forcée à 3 °C on verra : titre « Marche forcée » ambre,
pastille « En marche » ambre, **et** la bande froid. ⭐ **Aucun signal n'en efface un autre.**

### B7 — Accessibilité
Le verdict est porté par le **texte** + le liseré 1 px, ⛔ jamais par la couleur seule
⇒ **daltonisme couvert**. `role="status" aria-live="polite"`, nœud **permanent** (pas créé à la
volée). Bande non interactive ⇒ pas de cible tactile. ⓘ **Constat** : le CSS n'a **ni mode sombre
ni `data-theme`** (0 occurrence) — rien à adapter. Contraste `#78350f` sur fond jaune clair
≈ **7,2:1** (valeur **déclarée dans le CSS**, ⛔ non remesurée — à vérifier si quelqu'un y touche).

### B8 — Carte Température : un segment, et la pastille ne bouge PAS
La puce « À jour » garde son sens (la *mesure* est fraîche) ⇒ **inchangée**. Mais pour que ce vert
ne se lise pas « tout va bien » à 3 °C, un **segment** s'ajoute à `tempSegments` (variant
`warning`, priorité 2) : « **Sous 4 °C : le gel reste à votre charge.** » (40 car., une ligne).
Conditions : celles de B3bis **plus** `measuresOk` — ⓘ sinon la carte dit déjà « -- » /
« En attente », et c'est la bande Filtration qui porte l'information. Apparaît donc aussi sur la
page Température (`temperature-page-badge`). **Redondance assumée** : deux cartes, deux contextes
de lecture.

### B9 — Guide client, texte proposé
Remplacer `docs/user/src/guide-utilisation.html:458` (« ou "Hors gel" en orange pendant une pause
anti-gel » — faux sur **deux** points) par : « **Sous 4 °C, une bande ambre indique la température
relevée. L'appareil ne gère pas le gel : c'est au propriétaire d'y veiller.** »

### ⚠️ B10 — Signalé par l'agent, à ne pas perdre
- Vérifier que le **harnais de pastilles** (`test/dash_pill_vectors.js`) n'assert pas `frost` ni
  « Hors gel » — il le fait (l.131-134).
- **Projet écran LVGL séparé** : si le mot « Hors gel » y est repris, le signaler. ⛔ Hors
  périmètre de ce lot.


## 🏗️ C — Plan technique arrêté (`architect`, 2026-11-22) — ⛔ zéro ligne de firmware

### 🔴 C0 — LE PIÈGE QUI TUERAIT LE CORRECTIF (trouvé DEUX FOIS, indépendamment)
⚠️ `ux-ui-designer` **et** `architect` l'ont signalé **séparément**, sans se lire. ⇒ C'est le
point le plus important de ce lot.
L'hôte « naturel » de l'énoncé serait `tempSegments` (`data/app.js:5263`)… mais **tout ce bloc
est gaté par `measuresOk = measuresAreValid(latestSensorData)`**, qui est **faux dès que
`water_present === false`** (`data/app.js:5005`), c'est-à-dire **pompe arrêtée** — ⛔ **exactement
le cas dangereux**.
⇒ **`coldWaterState()` et son rendu ne passent EN AUCUN CAS sous ce gate.**
⭐ Sans cette précaution, on **réimplémente le défaut d'origine sous un autre nom.**

### C1 — Les signatures
| Nouveau / modifié | Rôle |
|---|---|
| `const TEMP_WINTER_MIN_C = 4;` | ⛔ **pas une 3ᵉ copie du 4** : `TEMP_GAUGE_MARK_LOW` (`data/app.js:3692`) se **pose dessus**. Commentaire citant `kFiltrationRefTempMinC` (`src/constants.h:3285`) |
| `isWaterRoleUnknown(data)` | **extrait** de `data/app.js:5263`, ⛔ pas créé de rien ; commentaire « LIMITE ASSUMÉE » déplacé avec lui |
| `coldWaterState(data, isRunning)` → `{cold, running}` | fonction **pure** : ⛔ aucun DOM, aucun global ⇒ testable |
| `renderColdWaterNotice()` | **écrivain unique** du DOM |
| `dashFiltrationTone(forced, running)` | ⚠️ signature **modifiée** — `frost` retiré |

### C2 — `frost` est SUPPRIMÉ, et ses SIX appelants traités dans CE lot
⭐ Motif : `frost` ne pilotait qu'une teinte, et cette teinte disparaît. **Un champ toujours
`false` est une branche morte que le prochain lecteur rallume.**
`data/app.js:4026` signature · `:4027` corps · `:4024-4025` commentaire · `:5825` appel · `:5888`
et `:5901` les **deux** littéraux de retour · `:5894` la branche entière.
⚠️ **7ᵉ point, hors `app.js`** : `test/dash_pill_vectors.js:44` porte l'ancre exacte
`"function dashFiltrationTone(forced, frost, running) {"`. ⭐ Sans correction le harnais **meurt
bruyamment** (`Ancre introuvable`) — **c'est la bonne propriété**, pas un défaut. Ses vecteurs
`frost` se **remplacent**, ⛔ ils ne se suppriment pas sans contrepartie.

### ✅ C3 — La divergence gris/rouge se referme à ZÉRO ligne de CSS
⚠️ `architect` craignait qu'« aucun gris neutre n'existe » pour `.pill` et proposait soit un ton
sémantique, soit un `.pill.off`. **Vérifié — l'inquiétude est sans objet** : `data/app.css:2068`,
la `.pill` **nue** porte déjà `background: var(--panel2)` et `color: var(--muted)`, c'est-à-dire
**exactement le gris neutre demandé** par B4. ⇒ `pillClass: ''` suffit.
⭐ **Conséquence** : ⛔ aucun composant partagé n'est modifié, aucune entrée CSS n'est ajoutée,
et le risque d'« application partielle » **n'existe pas** sur ce lot.

### C4 — Parité firmware : CONFIRMÉ, aucune donnée nouvelle
Les quatre entrées sont **déjà servies des deux côtés** : `temperature` et `filtration_running`,
`sondes_identified` + `sondes_detected` (`web_routes_data.cpp:383-384` **et**
`ws_manager.cpp:1026-1027`). ⇒ **Les quatre sites de parité ne sont pas touchés.**
ⓘ Le **seuil**, lui, est **dupliqué** dans le navigateur plutôt que servi : c'est un **choix
assumé**, pas un oubli — le garde-fou est le commentaire croisé, ⛔ pas la CI.

### C5 — Le harnais (ADR-0099 : extraction par ancre, ⛔ jamais une recopie)
`test/filtration_state_vectors.js` + `tools/check_filtration_state_vectors.sh` + 1 job dans
`.github/workflows/hygiene-specs.yml`.
⚠️ **`filtrationRunningOverride` se stube par la PORTÉE, pas par un paramètre** : un `let` déclaré
**avant** les fonctions extraites, qu'elles capturent par fermeture ; le wrapper l'affecte avant
chaque appel. ⇒ Les **trois** valeurs (`null`, `true`, `false`) doivent être couvertes — c'est le
seul endroit où l'override prime sur `/data`.

### C6 — Mutations exigées ⇒ chacune doit TUER un vecteur NOMMÉ
| # | Mutation | Vecteur qui doit mourir |
|---|---|---|
| M1 | `4` → `5` | le vecteur posé à **4,5 °C** — ⭐ il **FIGE** la valeur tranchée, sans quoi la divergence 4/5 revient en silence |
| M2 | retirer `!isWaterRoleUnknown(...)` | « sonde non identifiée ⇒ **silence** » |
| M3 | retirer `!isNaN` | le vecteur `NaN` |
| M4 | échanger les textes marche/arrêt | les vecteurs de libellé |
| M5 | ignorer l'override | `override=false` **avec** `filtration_running=true` |
| M6 | ⛔ faire passer le rendu **sous** `measuresOk` | « eau froide + pompe arrêtée ⇒ l'écran **dit** quelque chose » — ⭐ **LE mutant central de ce bug** |
| M7 | réintroduire le mot « Hors gel » | vecteur d'**interdiction de vocabulaire** |

### C7 — Décision annexe à acter, ⛔ pas à passer sous silence
Le champ `class: 'state-badge--…'` rendu par `getFiltrationState()` **n'a aucun consommateur**
(vérifié : seuls `text`, `pillClass`, `frost`, `running` sont lus). Les deux littéraux étant
réécrits, **il est retiré** — à confirmer par `code-reviewer`.
⚠️ Et `data/app.js:8195` porte un commentaire périmé citant encore « Hors gel » : **nettoyé dans
le même passage**.

### ✅ C8 — ADR : UN SEUL, et il est nécessaire
**« L'appareil est témoin du froid, jamais agent »** — les 4 critères sont réunis.
⭐ **Motif décisif** : la fiche écrit elle-même que **les gates ne gardent aucune mémoire**.
⛔ Sans ADR, **l'option 2 revient au premier hiver**. Y loger les **trois** raisons du NO GO
(mauvaise grandeur · marche à sec · réouverture du dosage à 2 °C) **et** le seuil unique 4,0 avec
son alternative écartée (servir le seuil par l'API).
⛔ **Pas d'ADR pour la priorité de teinte** : elle ne se rejuge pas, **elle disparaît**. Sa trace
va dans `docs/DESIGN_SYSTEM.md` (document vivant) et, si besoin, en amendement d'une ligne à
ADR-0135, qui porte déjà la grammaire des pastilles.


## 🔎 D — Vérification du RENDU par l'agent principal (2026-11-22)

⭐ Règle du projet : **on regarde ce qu'on livre.** Les captures ci-dessous ont été prises
**après** implémentation et **regardées une par une**.

### ✅ Ce qui est conforme
| État forcé | Rendu observé |
|---|---|
| 3 °C, pompe **en marche** | pastille « **En marche** » (verte) **+** bande ambre « Eau à 3,0 °C (sous 4 °C). / Le gel reste à votre charge. » |
| 3 °C, pompe **arrêtée** (`water_present=false`) | pastille « **Arrêtée** » (**grise**) **+** bande « Eau des tuyaux à 3,0 °C (sous 4 °C). / … » ⭐ **le cas autrefois muet PARLE** |
| **4,0 °C pile** | ⛔ **aucune bande** — on n'écrit jamais « 4,0 °C (sous 4 °C) » |
| `sondes_identified=false` | ⛔ **aucune bande**, et l'appareil dit à la place « **Sondes de température à identifier** » — le message actionnable |
| page Filtration | la **même** bande, et la pastille est passée de **rouge** à **grise** |
⇒ ⭐ L'état de la pompe **n'est plus perdu** : avant, « Hors gel » le **remplaçait**.

### ⚠️ D-a — Un faux défaut, et comment il a été écarté
La page Filtration semblait **ignorer la bande** : capturée à 3 °C et à 20 °C, elle rendait une
image **au pixel près identique**. ⛔ Ce n'était **pas** un défaut du lot.
**Méthode qui a tranché** : rejouer la capture avec la version **d'avant** (`git stash`) — elle se
comporte **exactement pareil**. ⇒ Artefact de l'outil : `ui_shot.sh -r '#/filtration'` ouvre la
route **directement**, et la page est peinte **avant** l'arrivée de `/data`.
✅ **En naviguant comme un utilisateur** (`-k '#nav-filtration'` après chargement), tout s'affiche.
⚠️ **À retenir pour les prochaines fiches** : une surface **autre que le tableau de bord** se
photographie **par NAVIGATION** (`-k`), ⛔ pas par `-r`. Sinon on photographie une page qui n'a
jamais reçu de données — et on croit à un bug.

### 🔴 D-b — Un VRAI défaut, trouvé uniquement sur la capture
⛔ **Le même composant ne rendait pas la même couleur aux deux endroits.**
Les fonds de `.sensor-badge--*` sont **translucides** — pensés pour une carte **blanche**. En tête
de page, la bande se compose sur `--bg` (bleu-gris) et **l'ambre vire à l'olive**.
**Mesuré sur la capture**, ⛔ pas estimé :

| Surface | Fond réel | Contraste du texte `#78350f` |
|---|---|---|
| carte du tableau de bord | `(255, 247, 225)` ambre | **8,49:1** |
| page Filtration, **avant** | `(206, 208, 200)` **olive** | **5,82:1** |
| page Filtration, **après correctif** | `(255, 247, 225)` ambre | ✅ **8,49:1** |

ⓘ Les 5,82:1 **passaient** l'exigence AA (4,5:1) — ⛔ ce n'était donc **pas** un défaut
d'accessibilité, mais **une incohérence visuelle** : précisément ce que le projet interdit à un
composant partagé.
⚠️ **`ux-ui-designer` avait bien déclaré le réemploi de `.sensor-badge` dans un contexte nouveau
(B5)** — mais en supposant que l'ambre y serait le même. **Il ne l'était pas.** ⭐ C'est la
démonstration de la règle maison : *un défaut visuel trouvé après coup vient d'un mandat trop
étroit, pas d'un agent défaillant* — personne n'avait pensé à lui dire que le fond de page n'est
pas blanc.
**Correctif** (`data/app.css`) : fond **opaque** identique au rendu en carte, pour les **trois**
variantes, spécificité 0,2,0 ⇒ l'ordre des règles n'entre pas en jeu.


## Comportement actuel
Pastille « Hors gel » (ambre) quand `temperature < 5.0 && filtration_running`.
Aucun effet sur la commande de la pompe. Le libellé **s'efface** dès que la pompe s'arrête.

## Contraintes techniques
- ⛔ **Aucune solution ne doit exiger d'équipement ni de configuration chez le client.**
- L'option 2 touche la commande de la pompe ⇒ `pool-chemistry` obligatoire, et il faut
  statuer sur l'interaction avec le dosage (filtration forcée ⇒ dosage autorisé ?).
- L'option 1 est purement frontend.

## Fichiers potentiellement concernés
- `data/app.js` — `getFiltrationState()` (l. ~5381)
- `data/app.css` — l'exception `--warn` de la pastille (l. ~2499)
- `src/filtration.cpp` — seulement si l'option 2 est retenue
- `src/constants.h` — seuil éventuel
- `docs/user/src/guide-utilisation.html` et `guide-regulation.html`

## Livrables — check-list
- [ ] A1 — Arbitrage `pool-chemistry` + décision utilisateur entre les deux options
- [ ] B1 — Correctif retenu
- [ ] C1 — Guide utilisateur : dire clairement à qui incombe la protection du gel
- [ ] C2 — `CHANGELOG.md`
- [ ] D1 — Essai : eau sous 5 °C simulée (`ui_shot.sh -f temperature=3`), pompe en marche
      **et** à l'arrêt

## Sécurité chimique
- Dosage impliqué : **oui si l'option 2 est retenue** (une filtration forcée rouvre le
  dosage — à statuer)
- Risque identifié : ⚠️ **gel des canalisations** — risque matériel, pas chimique, mais
  c'est le plus coûteux que porte cette fiche.

## Critères d'acceptation
- [ ] Aucun libellé de l'interface n'annonce une protection que l'appareil n'assure pas
- [ ] Le cas « eau froide **et** pompe arrêtée » — le plus dangereux — est traité, alors
      qu'aujourd'hui il n'affiche **rien**
- [ ] Le guide utilisateur dit à qui incombe la protection du gel

## Notes pour agent
- ⚠️ **feature-195 touche DÉJÀ la couleur de cette pastille** : sa puce passe d'ambre à
  **grise**, texte et logique inchangés. Motif écrit dans cette fiche : l'ambre
  d'aujourd'hui ne s'allume que quand la pompe **tourne** — donc dans le cas favorable.
  ⇒ Ne pas lire ce changement comme un traitement du présent défaut : il reste entier.
- ⚠️ Le libellé actuel disparaît **précisément dans le cas dangereux** (pompe arrêtée).
  C'est l'inverse du comportement attendu d'une alerte : traiter ce cas d'abord.
- ⚠️ **Ne pas confondre** avec le « gel d'intégrale » du PID (`kPhIntegralFreezeDailyFraction`),
  qui n'a aucun rapport.

## ✅ CLÔTURE — v3.0.2, 2026-11-22

### Verdicts
| Gate | Verdict |
|---|---|
| `pool-chemistry` | ✅ **GO option 1** · ⛔ **NO GO franc option 2** (cf. A1, trois raisons indépendantes) |
| `architect` | ✅ plan arrêté, 1 ADR proposé et créé |
| `ux-ui-designer` | ✅ décisions arrêtées, textes mot pour mot (B) |
| `test-engineer` | ✅ 82 assertions, 7 mutations sur 8 tuent un vecteur nommé |
| `code-reviewer` | ✅ **Approved**, 1 objection de promotion + 5 mineures — **toutes traitées** |

### Chiffres **MESURÉS** (⛔ aucun recopié — ADR-0073)
- `pio run` : **SUCCESS**, **0 warning**, flash **52,7 %**, RAM **30,1 %**
- `pio test -e native` : **97 suites / 2 734 cas**, aucune régression
- Gardes : **45 lancées sur 45**, **44 vertes**
- ⚠️ La seule rouge, `check_release_artefacts.sh` : les `.sig` sont ceux de la **3.0.1** et le
  binaire est en **3.0.2**. ⭐ **C'est le comportement ATTENDU d'un bump** — ⛔ pas un défaut du
  lot. Les signatures se refont **EN DERNIER**, juste avant l'envoi.

### Suites données aux objections de revue
1. ✅ Les **trois liens** vers `doing/` (`adr/0141` ×2, `adr/0135`) corrigés **dans le commit de
   promotion**, garde relancée ensuite — ⚠️ la récidive connue du dépôt.
2. ✅ `.sensor-badge--page.sensor-badge--info` : valeur **fausse** corrigée en `rgb(229,236,253)`
   (`rgba(37,99,235,0.12)` sur blanc). ⓘ Règle aujourd'hui inatteignable, donc **latente** — c'est
   précisément pourquoi elle devait être juste.
3. ✅ `docs/features/page-filtration.md:137` : **le document se contredisait lui-même** — sa
   ligne 99 écrit que `saveFiltration()` n'existe pas, sa ligne 137 la listait. Corrigé, **plus**
   l'affirmation périmée « dashboard = `state-badge--*` » (c'est `setDashPill()`/`data-tone`
   depuis feature-195).
4. ✅ Piège `setBadge()` (qui **écraserait** `--page` via `el.className =`) documenté en commentaire
   dans `data/app.css`, à côté de la règle.
5. ✅ **Objection la plus utile** : `isNaN("")` et `isNaN(false)` valent **FAUX** ⇒ ces valeurs
   traversaient la garde et produisaient « **Eau à 0,0 °C** » — une **affirmation de froid sur une
   donnée vide**. Garde `typeof t !== 'number'` posée.
   ⭐ **Et la leçon de méthode** : la sentinelle est **restée verte** après l'ajout de la garde.
   ⇒ J'ai joué la mutation : **aucun vecteur ne la protégeait**. Deux vecteurs ajoutés
   (« chaîne vide », « booléen false »), mutation rejouée, elle **meurt** maintenant.
   ⛔ **Une garde sans vecteur qui la tue ne prouve rien.**
6. ✅ Guide : la bande apparaît **aussi** en tête de la page Filtration — dit.

### Ce que j'ai attaqué moi-même (⛔ pas seulement lu les rapports)
- Mutation « retirer `t == null` » ⇒ **tuée** par le vecteur « null : silence ». ⚠️ Sans elle, une
  sonde absente afficherait « Eau à 0,0 °C ».
- Mutation « renommer `coldWaterState` » ⇒ la sentinelle meurt **bruyamment**
  (`Ancre introuvable`) — ⛔ jamais un faux vert.
- `data/app.js` vérifié **restauré à l'octet près** (`diff`) après chaque mutation.
- **Rendu photographié et REGARDÉ** dans 7 états (cf. D) — c'est ce qui a trouvé D-b.

### 📸 Captures humaines : AUCUNE à refaire — ⛔ vérifié, pas supposé
`doc-writer` en annonçait trois par précaution. **Regardées** : `04_Filtration.jpg` montre la pompe
« En marche » (vert **inchangé**). Le seul changement à température normale est la pastille
« Arrêtée » de la **page** Filtration (rouge → gris), **absente de toute capture** ; au tableau de
bord elle était **déjà grise** (`dashFiltrationTone` rendait `"off"`). Et la bande n'existe que
**sous 4 °C**, état non reproductible à la demande sur l'appareil.

### Critères d'acceptation
- [x] Aucun libellé n'annonce une protection que l'appareil n'assure pas — ⛔ **zéro occurrence**
      de « Hors gel » / « anti-gel » dans `data/`, `src/`, `test/`, `tools/`, `docs/user/src/`
- [x] Le cas « eau froide **et** pompe arrêtée » est traité — ⭐ il affichait **RIEN**, et il
      cachait même la température (« -- °C », « En attente »)
- [x] Le guide utilisateur dit **à qui incombe** la protection du gel

### Check-list d'origine
- [x] A1 arbitrage · [x] B1 correctif · [x] C1 guide · [x] C2 CHANGELOG · [x] D1 essais

### ⚠️ Reste à faire HORS de cette fiche
- **Déployer** : ⛔ signer **en dernier** (`check_release_artefacts.sh` le refuse sinon).
- **Projet écran LVGL séparé** : vérifier s'il reprend le mot interdit. ⛔ Autre dépôt.
- 💡 **Piste gratuite, à ouvrir en fiche séparée si souhaité** (`pool-chemistry`) : un **rappel
  saisonnier** à la première descente durable sous le seuil — une aide réelle, ⛔ sans promesse ni
  commande d'actionneur.

