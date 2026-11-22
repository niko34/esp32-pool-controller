## Métadonnées
- **ID** : bug-199
- **Slug** : deux-conventions-decimales-sur-le-meme-ecran
- **Type** : bug
- **Statut** : **doing** — ouvert le 2026-11-22
- **Priorité** : basse
- **Version cible** : à arrêter
- **Dernière mise à jour** : **2026-11-22**
- **Spec(s) liée(s)** : [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (relevé sur capture, sorti de son périmètre à dessein)

## Domaines touchés
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Depuis feature-195, les mesures du tableau de bord s'écrivent à la **virgule** française
(« 7,3 » · « 24,8 »). Mais plusieurs textes du **même écran**, parfois à quelques pixels,
gardent le **point** décimal anglais : « Stock faible (0.7 L) », « Chloration suspendue :
pH 7.3 — … ».

## Contexte
Relevé le **2026-11-18** sur capture pendant feature-195, puis confirmé en revue de code.
⛔ **Volontairement sorti du périmètre de ce lot**, pour deux raisons énoncées par la revue :
ce n'est pas une occurrence isolée mais un **balayage d'écran**, et ce balayage contient un
piège qui mérite d'être écrit avant d'être improvisé.

Occurrences relevées (⚠️ **à recompter**, la liste n'est pas garantie exhaustive) :
- `data/app.js:4764` — `(rem / 1000).toFixed(1)` dans `buildSensorSegments` ⇒ la bande
  « Stock faible (0.7 L) », **dans la carte pH/ORP refondue**, à quelques pixels d'un héros
  à virgule. C'est l'occurrence la plus visible.
- `data/app.js:5819` — « Chloration suspendue : pH 7.3 — … » dans le bandeau de blocage.

## Comportement attendu
Une seule convention décimale dans tout le texte **affiché** : la virgule française.

## ⛔ LE PIÈGE — à lire avant de toucher une ligne
**`fmtFr` ne doit JAMAIS toucher un `.value` d'`<input>` ni un `dataset`.** Ce sont des
**valeurs machine** : une virgule les casse silencieusement.
Deux endroits au moins sont concernés et ⛔ **ne doivent pas être convertis** :
- `data/app.js:5007-5008` — `(… / 1000).toFixed(1)` écrit dans un `input.value` ;
- `buildPhTargetSegments` — écrit dans `btn.dataset.value`.

⇒ **Règle à poser dans le lot** : la conversion ne s'applique qu'à du texte **rendu**
(`textContent`, `innerHTML`), jamais à une valeur relue par du code.

## Comportement actuel
Deux conventions coexistent sur le même écran depuis la v2.96.0.

## Fichiers potentiellement concernés
- `data/app.js` — recenser **toutes** les occurrences de `toFixed(` suivies d'un affichage
- `docs/DESIGN_SYSTEM.md` — si une règle de formatage y est posée

## Livrables — check-list
- [x] B1 — Inventaire **exhaustif et compté** des nombres affichés avec un point
- [x] B2 — Conversion des seules occurrences **affichées**
- [x] D1 — Garde ou harnais : ⛔ aucun `fmtFr` sur un `.value` ou un `dataset`
- [x] D2 — Captures avant/après du tableau de bord et des bandeaux

## Critères d'acceptation
- [ ] Aucun nombre affiché **par l'interface** ni cité des **six messages firmware identifiés** ne porte de point décimal. Exclus, assumés : `debugLog` et le bandeau de bascule manuelle pH (diagnostics de sonde, hors périmètre).
- [x] Aucune valeur **machine** (`.value`, `dataset`) n'a été convertie
- [x] Les champs de saisie acceptent toujours la saisie et l'enregistrement

## Notes pour agent
- ⚠️ `_fmtPh()` est **le** formateur pH partagé, et `fmtFr()` le formateur générique.
  ⛔ Ne pas en créer un troisième : feature-195 a dû corriger exactement ça en revue
  (deux expressions pour la même consigne pH, divergentes à 7,35).

## 📸 Observé sur l'appareil réel — 2026-11-21

Capture du tableau de bord prise pendant la clôture de
[feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) : le défaut est visible
**deux fois sur le même écran, en même temps**, et dans l'état **courant** de la machine — pas
dans un cas limite à provoquer :

- carte **pH** : « Stock faible (0.3 L) » — à six pixels d'un « 7,3 » à virgule
- carte **ORP** : « Stock faible (0.0 L) » — à six pixels d'un « 556 » et d'une « consigne 530 mV »

⇒ Deux conventions décimales **sur la même carte**, répétées **sur deux cartes voisines**. Le
lecteur voit le point et la virgule dans un même coup d'œil. ⚠️ Cela relève la priorité de fait :
ce n'est pas une occurrence rare, c'est l'écran d'accueil au repos.

⛔ **Le piège à ne pas improviser**, déjà écrit par la revue de code de feature-195 : `fmtFr`
⛔ **ne doit jamais toucher un `.value` ni un `dataset`** — ce sont des valeurs machine qu'une
virgule casse (`data/app.js`, `buildPhTargetSegments`).


---

# 📋 B1 — INVENTAIRE EXHAUSTIF ET COMPTÉ — 2026-11-22 (v3.0.0)

⚠️ **Tous les numéros de ligne de la rédaction d'origine (2026-11-18) sont PÉRIMÉS** :
`data/app.js` a pris ~300 lignes depuis. Ce qui suit est recompté sur le code du jour.

**`grep -c "toFixed(" data/app.js` ⇒ 33 occurrences**, classées en **quatre** familles.

## 🟢 Famille 1 — les FORMATEURS (ils convertissent déjà) — ⛔ ne rien y changer
| Ligne | Fonction |
|---|---|
| `:2570` | `_fmtPhOffset2` — 2 décimales **signées** |
| `:2581` | `_fmt2` — 2 décimales |
| `:3774` | `_fmtPh` — 1 décimale |
| `:4035` | `fmtFr` — générique |

⭐ **Quatre formateurs existent déjà. ⛔ NE PAS EN CRÉER UN CINQUIÈME** — feature-195 a dû corriger
exactement ce travers en revue (deux expressions pour la même consigne pH, divergentes à 7,35).

## 🔴 Famille 2 — VALEURS MACHINE — ⛔ INTERDIT de convertir
| Ligne | Cible | ⚠️ Ce qu'une virgule casse |
|---|---|---|
| `:5386`, `:5387` | `input.value` (seuils d'alerte de stock) | la saisie et l'enregistrement, **en silence** |
| `:5198` | `v => +v.toFixed(1)` | ⚠️ ce n'est **pas** un affichage : le `+` en fait un **nombre** |
| `buildPhTargetSegments` | `btn.dataset.value` | la relecture par le code — ⚠️ **plus aucun `toFixed(` à ce site aujourd'hui** (affirmation périmée de la rédaction du 18/11) ; ⭐ **la garde D1 le couvre quand même**, c'est son intérêt |
| 🔴 `:5326` | **`bar.style.width = pct.toFixed(1) + '%'`** | **la mise en page, SANS ERREUR VISIBLE** |

🔴 **`:5326` est une TROISIÈME catégorie que la fiche d'origine ne nommait pas** : une **valeur
CSS**. ⭐ `width: 72,4%` est invalide — le navigateur **ignore la règle** et la barre reste à sa
largeur précédente. ⛔ Aucune erreur, aucun log : le défaut serait **invisible en test** et visible
seulement à l'œil, sur une barre qui ne bouge plus.
⇒ **La règle du lot s'énonce donc en trois volets** : ⛔ jamais sur un `.value`, ⛔ jamais sur un
`dataset`, ⛔ **jamais sur une valeur CSS**.

## ⚪ Famille 3 — hors écran utilisateur — ⛔ laissé tel quel, et c'est un choix
| Ligne | Contexte |
|---|---|
| `:970` | `debugLog(...)` — durée en ms, **journal de développement** |

⇒ ⛔ Convertir un journal technique n'apporte rien et brouillerait une trace lue par un outil.
**Décision : inchangé.** ⚠️ À dire explicitement, ⛔ pas à omettre.

## 🟠 Famille 4 — AFFICHÉ avec un point — **le périmètre du lot**
🔴 **22 occurrences** (⚠️ **et non 18** : la rédaction initiale de cet inventaire comptait les
**lignes du tableau** ci-dessous, dont trois en regroupent plusieurs. Recompte du développeur,
vérifié : 4 formateurs + 4 machine + 1 journal + 2 déjà conformes + **22** = **33**). ⇒ Bien
au-delà des **deux** que la fiche d'origine citait.

| Ligne | Ce qui s'affiche | Où |
|---|---|---|
| `:5143` | `Stock faible (0.7 L)` | 🔴 **carte pH/ORP du tableau de bord** — l'occurrence la plus visible |
| `:5331`, `:5332` | restant / contenance du bidon, en L | cartes de dosage |
| `:6203` | `Chloration suspendue : pH 7.3 — …` | bandeau de blocage |
| `:8417` | température | `textContent` |
| `:8421` | pH | `textContent` |
| `:8628`, `:8629` | `… %` et `… mV` (signaux capteurs) | carte Signaux |
| `:8771` | `_fmtFilterVal` — valeur de filtre pH | carte Signaux |
| `:9705` | pH courant | écran pH |
| `:9738`, `:9829` | température, et `(offset +0.3 °C)` | écran Température |
| `:9970`, `:9976`, `:9978` | température corrigée (3 emplacements) | étalonnage température |
| `:10178` | lecture pH pendant l'étalonnage | écran de calibration |
| `:13117` | `… °C` | historique |
| `:13483` | `min … · max … · moy …` | statistiques de série |
| `:13925`, `:14032`, `:14847`, `:14891` | débits `… ml/min` | assistant pompes et réglages |

✅ **Déjà conformes** (`.replace(".", ",")` explicite) : `:10310`, `:10617`.

⇒ ⭐ **Ce n'est pas un correctif ponctuel, c'est un BALAYAGE D'ÉCRAN** — ce que la revue de
feature-195 avait anticipé en sortant le sujet de son périmètre.


---

# 🎨 ÉTUDE D'ERGONOMIE — 2026-11-22 (sur capture du tableau de bord, 390 px)

## U1 — La virgule ne nuit **nulle part** ⇒ conversion sans exception
⭐ Le cas qui aurait pu poser problème est vérifié et **ne le pose pas** : les statistiques de
série (`data/app.js:13483`) rendent `min 1,2 · max 3,4 · moy 2,3` — le séparateur est un **point
médian `·`**, ⛔ pas une virgule. Aucune ambiguïté.
⛔ **CONDITION pour que ça reste vrai** : ⛔ **ne JAMAIS remplacer le `·` par une virgule ou un
point-virgule** dans ce gabarit. ⇒ À écrire dans le design system (U4).
⚠️ `:13483` est la **seule** fonction qui prend `decimals` en paramètre : lui appliquer un
**formateur existant**, ⛔ pas un `.replace` bricolé sur place.

## U2 — « Stock faible (0,0 L) » : **le format n'est pas le vrai problème** ⇒ SORTI du lot
⭐ *« Faible » dit « pensez à en racheter ». « Vide » dit « la régulation ne peut plus doser ».*
⛔ Corriger la virgule laisserait un **bidon vide annoncé comme un stock faible**.

⚠️ **Et le sujet a DEUX emplois**, vérifiés dans le code : `data/app.js:5143` (bandeau du tableau
de bord) **et** `:5336` (`status.textContent = isLow ? 'Stock faible' : 'OK'`, badge des cartes de
dosage). ⛔ **Traiter l'un sans l'autre créerait exactement la seconde forme que ce chantier
combat.**

⇒ **DÉCISION : extension SORTIE de bug-199**, et ouverte en **[bug-211](../todo/bug-211-un-bidon-vide-est-annonce-comme-un-stock-faible.md)**.
**Motif** : bug-199 a un objet **net** — la convention décimale — et un critère d'acceptation
mesurable (*aucun point décimal affiché*). ⚠️ Y mêler un changement de **libellé** ferait perdre
ce critère. ⇒ Dans ce lot, `:5143` ne change **que de format** : « Stock faible (0,3 L) ».

## U3 — Décimales : **aucune incohérence de bug**, une asymétrie **voulue**
Carte pH : « **7,2** » (1 décimale, `_fmtPh`) à côté de « **7,00** eau agressive · **7,60** chlore
inefficace » (2 décimales). ⛔ **Ce n'est pas un défaut** : la **mesure** se lit d'un coup d'œil,
les **repères** sont des seuils chimiques fixes lus comme des références. ⇒ **Laissé tel quel**,
et **écrit** dans le design system pour que ce soit un **choix**, ⛔ plus un accident.
ⓘ *Si* on voulait aligner un jour, ce serait **vers 2 décimales sur les repères** — ⛔ jamais
l'inverse : « 7,0 · 7,6 » perdrait la précision des seuils. ⛔ Pas dans ce lot.

⚠️ **Deux vérifications à faire À L'IMPLÉMENTATION** (l'ergonomie ne les a pas tranchées faute
d'avoir lu le code) :
1. `:9738` (température) et `:9829` (« offset +0,3 °C ») sont **sur le même écran** ⇒ même nombre
   de décimales ? ⚠️ **C'est le seul candidat réel d'incohérence.**
2. `:8628` / `:8629` / `:8771` / `:13483` sont **sur la carte Signaux** ⇒ une valeur et **sa
   statistique** à des décimales différentes **dans la même carte** serait un défaut.
⛔ **Ne rien changer sans le dire.**

## U4 — Le design system porte la règle — section **courte**, ⛔ aucun composant neuf
À ajouter à `docs/DESIGN_SYSTEM.md`, section 4 « États obligatoires », à la suite de « Donnée
absente ⇒ on n'affiche rien » : **« Nombres affichés : virgule, trois interdits »**.
1. **Règle** : tout nombre d'un texte affiché porte une **virgule**. On emploie **un des quatre
   formateurs existants** — ⛔ **jamais un cinquième** (`fmtFr` générique · `_fmtPh` pH 1 décimale ·
   `_fmt2` 2 décimales · `_fmtPhOffset2` correction signée 2 décimales).
2. **Les TROIS interdits, chacun avec sa raison** :
   - ⛔ jamais sur un `.value` d'`<input>` — la saisie et l'enregistrement cassent **en silence** ;
   - ⛔ jamais sur un `dataset` — valeur **relue par le code** ;
   - 🔴 ⛔ **jamais sur une valeur CSS** (`bar.style.width = …`) — `width: 72,4%` est **invalide** :
     le navigateur **ignore la règle** et la barre **reste figée**, ⛔ sans aucune erreur.
3. **Deux gabarits de pH distincts, VOULUS** (cf. U3).
4. **Laissé en point à dessein** : le journal de développement (`debugLog`, `:970`).
5. **Séparateur de liste** : quand un gabarit juxtapose des nombres, c'est **`·`**, ⛔ jamais la
   virgule.

## D1 — La garde qui rend la règle **opposable**
⭐ Idée de l'ergonomie, retenue : une sentinelle qui **échoue** si `fmtFr`, `_fmtPh`, `_fmt2` ou
`_fmtPhOffset2` apparaît **à droite** d'un `.value =`, d'un `dataset.` ou d'un `style.`.
⇒ ⛔ Sans elle, les trois interdits ne sont qu'un commentaire. ⚠️ Et elle doit **échouer fermé**.


---

# ✅ LIVRAISON `data/` — 2026-11-22

**22 occurrences converties** avec les formateurs **existants**, ⛔ aucun cinquième créé.
`grep -c "toFixed(" data/app.js` : **33 → 11**. ⭐ **Et les 5 restantes sans conversion sont
EXACTEMENT les trois catégories interdites plus le journal** : `:970` (debugLog), `:5198`
(nombre, pas affichage), `:5326` (CSS), `:5386`/`:5387` (`input.value`). Les 6 autres portent déjà
un `.replace` (les 4 formateurs + les 2 sites conformes).

**Vérifié à l'image** (captures 390 px, données réelles) :
- **avant** : « Stock faible (**0.3** L) » et « (**0.0** L) », à six pixels de « 7,2 » et
  « 7,00 · 7,60 » ;
- **après** : « Stock faible (**0,3** L) » et « (**0,0** L) » ⇒ écran homogène ;
- 🔴 **et la barre de dosage est TOUJOURS REMPLIE** (capture dédiée à 1200/2000 mL) ⇒ preuve que
  `:5326` n'a pas été converti. ⭐ *C'est la seule vérification qui pouvait attraper ce piège :
  une virgule y aurait figé la barre **sans aucune erreur**.*

## D1 — la garde est écrite et câblée
`tools/check_decimal_format_guard.sh` refuse les quatre formateurs à droite d'un `.value =`,
d'un `dataset.…`, d'un `.style.…`, d'un `style.setProperty(` ou d'un `setAttribute('value' |
'style' | 'data-…')`. **Échoue fermé** : fichier illisible, aucun fichier trouvé, ou l'une des
quatre définitions de formateur absente ⇒ **rouge**. Job `decimal-format-guard` en CI.

## ✅ RÉSOLU — trouvé en chemin : ORP à 1 décimale en statistique, 0 en valeur (décision utilisateur : 0 décimale, 4 sites)
**Carte Signaux, côté ORP** : les statistiques sont à **1 décimale** (`decimals: 1`), alors que
`_fmtFilterVal` affiche l'ORP **arrondi à l'entier** (`Math.round(v)` mV).
⇒ « **556 mV** » côté valeur et « **moy 555,8** » côté statistique peuvent coexister **dans la
même carte**.
⚠️ **Sans rapport avec la virgule**, et **antérieur à ce lot** ⇒ ⛔ non corrigé ici.
ⓘ Côté **pH**, tout est cohérent : statistiques **et** `_fmtFilterVal` à 2 décimales.
✅ **TRANCHÉ par l'utilisateur le 2026-11-22 : ZÉRO décimale pour l'ORP.**

⭐ **Et c'est le bon sens** : l'ORP est une tension en millivolts, lue par pas de plusieurs
dizaines. ⚠️ Une décimale y affiche une précision que la **mesure n'a pas** — le projet a relevé
des écarts de **centaines** de mV sur cette grandeur. ⇒ « moy 555,8 » promet un dixième de
millivolt sur un signal qui ⛔ ne le porte pas.

**Les quatre sites à passer de `decimals: 1` à `decimals: 0`** (`data/app.js`) :
| Ligne | Série |
|---|---|
| `:13581` | ORP brut |
| `:13582` | ORP médiane |
| `:13583` | ORP lissé |
| `:13656` | ORP (historique) |

⛔ **Les quatre ENSEMBLE** : en changer trois sur quatre recréerait l'incohérence **à l'intérieur
de la même carte**, ce qui serait pire que l'état actuel.
✅ Le pH reste à **2 décimales** sur ses quatre séries (`:13578-13580`, `:13655`) — ⭐ cohérent
avec `_fmtFilterVal`, qui affiche le pH à 2 décimales.

⚠️ **Impact test à prévoir** : le vecteur de `test/decimal_format_vectors.js` qui fige la forme ORP
devra bouger avec. ⛔ Ne pas le « réparer » après coup : ⭐ **c'est lui qui doit MOURIR si les
quatre sites ne bougent pas ensemble.**

⏱️ **Application différée volontairement** : la revue de code lisait `data/app.js` au moment de la
décision. ⛔ Réviser un fichier qui bouge ne vaut rien.

## ⏳ Reste à faire
- ✅ `docs/DESIGN_SYSTEM.md` §4 · `CHANGELOG` 3.0.1 · version (faits).
- ✅ Décimales ORP : tranchées à 0 (app.js:13584-13586, 13659).


---

# 🔴 REVUE DE CODE — 2026-11-22 : **Request changes**. La méthode d'inventaire était fausse.

⭐ **Le travail livré est solide** : 22 conversions propres, décimales inchangées, zéro cinquième
formateur, séparateur `·` préservé, aucun libellé touché. La garde a été **ré-attaquée sur
10 scénarios** et tient ; le harnais sur **7 mutants**, tous morts.
🔴 **Mais le critère d'acceptation n'est PAS atteint**, et le défaut est **au centre de l'écran
qui a motivé la fiche**.

## 🔴 L'ERREUR DE MÉTHODE — et elle est de moi
L'inventaire B1 reposait sur **`grep toFixed`**. ⛔ **Cette méthode manque DEUX familles entières :**
1. 🔴 **les NOMBRES INTERPOLÉS** — un `Number` dans un gabarit de chaîne rend son point tout seul ;
2. 🔴 **les LITTÉRAUX DE TEXTE** — « pH 7.0 » écrit en dur, en JS **ou** en HTML.

⚠️ **Et j'ai aggravé le défaut en classant `:5198` du mauvais côté** : je l'ai rangé en
« valeur machine, ⛔ INTERDIT de convertir », avec ce motif — *« le `+` en fait un nombre, pas un
affichage »*. ⛔ **Le motif est FAUX.** La valeur est machine **à l'usage** (`v < lo`) **et
affichée** : ⭐ **les deux à la fois.**

## 🔴 B1 — « Trop bas (6.9) » sur la carte pH du tableau de bord
`outOfRangeText` (`data/app.js:5064-5069`) interpole `v` dans un gabarit ; le callback `round`
passé par `:5198` est `v => +v.toFixed(1)` ⇒ **`"6.9"`, avec un point**.
⇒ Le texte part en **`priority: 1`** — le segment **le plus prioritaire** du badge. ⭐ Sur la carte
pH : « **7,2** » en héros, « Trop bas (**6.9**) » juste dessous.
✅ **Exécuté sur le code vivant par le relecteur** : `"Trop bas (6.9)"` · `"Trop élevé (7.5)"` ·
`"Trop élevé (820 mV)"` (ORP entier, sain).
**Correctif imposé** : ⛔ **ne pas toucher au callback** (il sert aux comparaisons `v < lo` — une
virgule y casserait la détection). Formater **au point d'affichage seulement**, avec une garde
`Number.isInteger` qui **préserve l'ORP** (`820 mV`, ⛔ pas `820,0 mV`) — ⛔ sans paramètre neuf et
⛔ sans cinquième formateur.

## 🔴 B2 — l'écran d'étalonnage pH, et **ce lot CRÉE le défaut**
`data/app.js:11259` (`"pH 7.0"` / `"pH 4.0"`), `:11379`, `:11380` : littéraux qui partent dans des
toasts (« Calibration **pH 7.0** réussie »).
⚠️ **Avant le lot** : lecture « 7.02 » + libellés « 7.0 » ⇒ **cohérent**. **Après**, `:10178` ayant
été converti : « **7,02** » + « pH **7.0** » ⇒ ⛔ **les deux conventions se côtoient sur le même
écran, du fait de ce lot.**

## 🔴 B3 — le même écran, en HTML
`data/index.html:1477, 1484, 1490, 1505, 1512, 1518` : « solution pH **7.00** », « Calibrer le
point **7.0** », idem **4.00** / **4.0** — ⚠️ **à deux lignes** des lectures que ce lot vient de
passer à la virgule.

🔴 **QUATRIÈME INTERDIT, révélé ici et nommé nulle part** : sur **les mêmes balises**, `step="0.1"`,
`min="0.1"`, `max="200"` sont des **attributs machine** ⇒ ⛔ **jamais convertis**.
⚠️ **Et la garde ne protège PAS ce terrain** : elle ne lit que `data/*.js`.

## 🟠 B4 — deux conventions dans **la même phrase**
`data/app.js:14847` : `` `${vol} ml en … → ≈ ${fmtFr(flow, 1)} ml/min` ``. `vol` vient d'un
`parseFloat` de saisie utilisateur. ⇒ ⭐ **L'utilisateur tape « 56,5 » et l'appareil lui répond
« 56.5 ml … → ≈ 28,3 ml/min » : il lui rend sa virgule transformée en point, à côté d'une
virgule.**

## Mineures retenues
- **Placeholders** `data/index.html:867, 2853, 2878` (« ex: 25.0 °C ») — texte **affiché**, jamais
  relu ⇒ conversion sûre.
- **Famille de formatage parallèle** : ~10 sites `toLocaleString("fr-FR", …)` rendent **déjà** la
  virgule ⇒ ⛔ aucun défaut. ⚠️ Mais (a) la règle « **un des quatre** formateurs » décrit un code
  qui en compte **deux familles** — à écrire, sinon le code contredit la règle à la première
  lecture ; (b) `toLocaleString` devrait entrer dans `FMT_CALL` : ⭐ **une virgule `fr-FR` dans un
  `style.width` casse exactement comme `fmtFr`**.
- `test/decimal_format_vectors.js` saute **M5** sans un mot ⇒ ⛔ indistinguable d'un oubli.
- **La limite du balayage n'est pas écrite** : il ne garantit que « aucun `toFixed` nu » — ⛔ il ne
  voit **ni les littéraux, ni les nombres interpolés**. ⭐ **C'est le trou par lequel les quatre
  bloquants sont passés**, et le projet s'est appuyé dessus pour conclure à l'exhaustivité.
- 🔴 **Le harnais est aveugle par SUBSTITUTION** : `:174` et `:251` passent `(v) => v` au lieu du
  vrai `v => +v.toFixed(1)`. ⭐ *ADR-0099 extrait le code vivant — mais un **paramètre faux**
  annule le bénéfice.* ⇒ C'est cette substitution qui a laissé passer B1.

## ✅ Ce que la revue a vérifié et trouvé CORRECT
Les **trois interdits** (11 `toFixed` restants, 5 sans conversion, **exactement** les attendus) ·
aucun cinquième formateur (⭐ `fmtPct`/`fmtMv` **délèguent** désormais : c'est une **dé-duplication**) ·
**22/22** sites relus un par un, **aucune décimale changée** · les deux cas U3 **tranchés** (aucune
incohérence sur l'écran Température ; l'asymétrie ORP est bien **antérieure**) · séparateur `·`
préservé et mutant tué · **garde ré-attaquée sur 10 scénarios**, la garantie fausse est bien
**réparée**, et sa limite « variable intermédiaire » **confirmée par exécution** · **7 mutants
injectés, 7 morts** · **aucun libellé modifié**, bug-211 intact.


---

# 🔴 ARBITRAGE UTILISATEUR — 2026-11-22 : les textes du firmware

## Le problème, trouvé par la revue, et invisible à toutes les méthodes précédentes
**Six messages de blocage sont rédigés DANS le firmware** avec `String(x, 1)` — donc un **point** —
et `_doseBlockInfo` les **cite verbatim** sur les cartes pH/ORP (`src/pump_controller.cpp:1084,
1114, 1196, 1233, 1661, 1698`). ⭐ **Aucune méthode de ce lot ne pouvait les voir** : l'inventaire,
le balayage et la garde ne lisent que `data/`.

## ⛔ Ma première implémentation était FAUSSE, et l'utilisateur l'a reprise
J'avais retenu une règle **de FORME** : « tout nombre à un point et 1-2 décimales ».
> *« Ce n'est pas une bonne façon de faire. J'ai demandé à convertir uniquement des données
> précises. »*

⭐ **Il a raison** : cette règle **devine**, elle ne **sait pas**. Elle convertit aussi bien un seuil
de pH qu'un « 12.5 » qui serait un pourcentage ou un débit — ⚠️ le développeur l'avait lui-même
listé comme la **faille n°1** de son implémentation. ⇒ Convertir des **données identifiées** et
convertir des **nombres de la bonne forme** ne sont pas la même chose.

## ⭐ ET L'ARBITRAGE DE FOND, qui renverse ma recommandation
Je recommandais de corriger **dans le firmware** (précis par construction, et répare du même coup
le journal et Home Assistant). ⇒ **Écarté par l'utilisateur, pour une raison meilleure** :
> *« Si un jour on traduit l'application dans plusieurs langues, il serait intéressant de garder le
> format anglophone côté serveur et ne convertir que côté front. »*

⭐ **C'est le bon découpage** : le serveur émet un format **canonique**, la couche de présentation
**localise**. Corriger dans le firmware figerait une convention française **à la source** et
casserait toute traduction future.

⚠️ **Et cela met au jour plus gros que la virgule** : ces six messages sont des **phrases entières
rédigées dans le firmware**. ⛔ Un front traduit ⛔ **ne peut pas traduire une phrase qu'il reçoit
toute faite.** ⇒ La virgule n'est que le **symptôme visible** d'un découpage qui bloquera la
traduction.
ⓘ Le projet a **déjà la bonne forme ailleurs** : pour les causes **énumérables**, le firmware
envoie un **code** et le front rédige la phrase. Le cas « garde » est l'exception qui **cite**,
parce que les valeurs n'étaient pas énumérables.
⛔ **Aucune fiche de suivi ouverte** (décision utilisateur du 2026-11-22 : *« non, pas encore des
fiches »*). ⇒ Le constat est consigné **ici** pour ne pas se perdre.

## Ce qui est retenu pour CE lot
**Conversion côté front, ANCRÉE sur les six formulations connues** — ⛔ jamais sur la forme d'un
nombre. Chaque motif capture **la valeur de capteur**, et rien d'autre.
🔴 **Avec le garde-fou qui manque à cette approche** : une **vérification automatique** qui compare
les motifs du front aux **chaînes réelles** de `src/pump_controller.cpp`. ⇒ Si le firmware
reformule un message, la CI passe au **ROUGE** — ⛔ au lieu que la conversion s'arrête **en
silence**. ⭐ Même mécanique que la parité de texte déjà en place pour un autre message.

## ⚠️ Laissé hors périmètre, et écrit
Le bandeau de **bascule manuelle pH** (`renderPhAutoManualBanner`) cite lui aussi un texte
firmware verbatim (`pump_controller.cpp:1326-1328`) : « pente acide **98.5** %, … zéro **4.12**
mV ». ⛔ **Non traité** : la décision de l'utilisateur borne la règle à **pH, ORP, température**, et
ce sont des **diagnostics de sonde** (pente en %, zéro en mV).
