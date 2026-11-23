## Métadonnées
- **ID** : bug-212
- **Slug** : les-pastilles-disent-hors-ligne-pendant-la-connexion
- **Type** : bug
- **Statut** : ✅ **done** — v3.1.3. Revue de code : **3 constats bloquants, tous traités** (+ la moitié manquante de l'invariant, fermée ensuite). **16 mutations posées, 16 tuées.** ⛔ Aucun verdict Approve formel n'a été réémis après ces corrections.
- **Priorité** : normale
- **Version cible** : 3.1.3
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [bug-196](../done/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md) (origine du constat), [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (auteur des pastilles), [feature-213](../todo/feature-213-photographier-l-etat-connexion-en-cours.md) (rend l'état vérifiable à l'image)

## Domaines touchés
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [ ] architect
- [x] ux-ui-designer — ⚠️ **l'écran d'accueil entier**, pas la seule pastille : c'est le texte
      d'état de **cinq** cartes qui change d'interprétation. Son mandat inclut le cas
      « première ouverture, rien n'est encore connu » et **tout autre choix visuel que ce lot
      impose — même non demandé**.
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
À l'ouverture d'une page, et après un simple creux réseau, les pastilles du tableau de bord
annoncent « **Hors ligne** » alors que l'appareil **vient de répondre**. Elles disent donc un
faux, et elles contredisent l'en-tête, qui affiche au même instant « Connexion… ».

## Contexte
⭐ **Trouvé pendant la revue de code de [bug-196](../done/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md)**, le 2026-11-22.

Ce défaut **préexistait** à bug-196 : ⛔ il n'a pas été introduit par lui. ⚠️ **Mais bug-196 en
change le statut.** L'en-tête portait exactement le même faux ; il ne le porte plus. Les
pastilles sont donc devenues **le dernier endroit** qui l'affiche, et elles sont désormais
**en contradiction visible** avec la barre de titre, sur le même écran, au même instant.

## Comportement actuel
`setNetStatus()` (`data/app.js`) pose `.app--offline` sur **tout ce qui n'est pas `ok`**, donc
aussi sur l'état `mid` (« Connexion… »). Or `mid` est posé :

1. **au démarrage nominal, JUSTE APRÈS un `loadConfig()` RÉUSSI** — l'application vient de
   prouver que l'appareil répond ;
2. après **un seul** échec de `/data` (`consecutiveFailures === 1`) — un creux d'une requête.

`setDashPill()` lit `isAppOffline()`, qui teste **`.app--offline`** :
```js
if (isAppOffline()) { tone = "off"; if (!keepTextOffline) text = "Hors ligne"; }
```
⇒ Les pastilles **Filtration**, **Éclairage** et **Température** affichent « Hors ligne ».
Les cartes pH et ORP gardent leur texte (`keepTextOffline`) mais passent au ton `off`.

⚠️ `refreshDashPills()` est appelé **depuis `setNetStatus()` lui-même**, juste après la pose des
classes : les deux surfaces sont rafraîchies **dans le même geste** et **se contredisent**.

## Comportement attendu
⛔ **Aucune surface n'annonce une perte de connexion qui n'est pas constatée.**
`bug-196` a introduit la distinction et l'a inscrite dans `docs/DESIGN_SYSTEM.md` §5.28 :

| classe | sens | usage |
|---|---|---|
| `.app--offline` | « pas `ok` », **inclut « Connexion… »** | verrouille les contrôles |
| `.app--disconnected` | **perte CONSTATÉE** (`bad`) seule | autorise une **affirmation** à l'écran |

⇒ Un texte d'état se raccroche à la **seconde**. Reste à trancher **ce que les pastilles
disent pendant `mid`**, et c'est le vrai sujet de la fiche :

- **Piste A — garder le dernier état connu** et ne rien annoncer de neuf. ⚠️ Mais à la
  **première** ouverture, il n'y a **aucun** état connu : que montre-t-on ?
- **Piste B — un libellé d'attente** (« Chargement… » existe déjà sur la température).
  ⚠️ Vérifier qu'il ne clignote pas à chaque creux réseau d'une seule requête.
- **Piste C — ne rien changer visuellement pendant `mid`**, le ton `off` restant admis comme
  « pas encore frais » tant qu'aucun **texte** n'affirme une perte.

⛔ Ne pas choisir sans l'`ux-ui-designer` : c'est un arbitrage de lisibilité sur **cinq** cartes.

### Cas limites
- **Première ouverture de l'application** : config chargée, `/data` pas encore reçu, aucune
  mesure connue. C'est le cas le plus fréquent du défaut.
- **Creux réseau d'une seule requête** en cours d'usage : l'état revient à `ok` en quelques
  secondes. ⛔ Le remède ne doit pas faire clignoter l'écran plus que le mal.
- **Perte réelle** (`bad`) : ⛔ le comportement actuel **ne doit pas changer** — « Hors ligne »
  est alors **vrai** et doit rester lisible.
- Mode d'installation ≠ `managed` : la carte Filtration est masquée (`applyInstallMode()`).

## Contraintes techniques
- ⚠️ **390 px d'abord**, vérifier 320 px.
- ⛔ **Ne pas toucher au verrou des contrôles** (`.app--offline` sur `.main`) : il est correct,
  et bug-196 l'a laissé intact **délibérément**.
- ⚠️ `keepTextOffline` et le ton `off` viennent de **feature-195** : toute évolution de leur
  sens est un changement du **langage visuel du tableau de bord**, pas un correctif local.
- `setDashPill()` est l'**écrivain unique** des pastilles — ⛔ ne pas créer un second chemin.
- Vanilla JS, français, offline first.

## Fichiers potentiellement concernés
- `data/app.js` — `isAppOffline()`, `setDashPill()`, `refreshDashPills()`, `setNetStatus()`
- `data/app.css` — si un ton d'attente distinct est retenu
- `docs/features/page-dashboard.md` — langage des pastilles
- `docs/DESIGN_SYSTEM.md` — §5.28 porte déjà la règle des deux classes ; la compléter si le
  ton `off` reçoit un sens explicite

## Livrables — check-list
- [x] B1 — Les pastilles n'annoncent plus une perte non constatée
- [x] B2 — Le cas « première ouverture, rien n'est connu » est traité explicitement
- [x] B3 — ⛔ L'état `bad` est **inchangé**, vérifié **à l'image** (`apres_perte_390.png`
      comparée à `pastilles_horsligne_390.png`) : Filtration, Éclairage et Température disent
      « Hors ligne », pH et ORP gardent « Mode … », commandes grisées, bandeau présent.
- [x] C1 — `docs/features/page-dashboard.md` + `docs/DESIGN_SYSTEM.md` si le ton change de sens
- [~] D1 — Captures 390 px : **perte réelle ✅** et **état normal ✅**
      (`apres_perte_390.png`, `apres_normal_390.png`, regardées). ⛔ **Première ouverture et
      creux réseau : IMPOSSIBLES à photographier** — ils vivent dans l'état « Connexion… », que
      `ui_shot.sh` ne sait pas figer ([feature-213](../todo/feature-213-photographier-l-etat-connexion-en-cours.md)).
      ⇒ Vérifiés par **vecteurs** et par **lecture**, ⛔ pas à l'image. ⚠️ Écrit tel quel :
      c'est précisément cet angle mort qui a laissé vivre le défaut.
- [x] D2 — Vecteurs sur la fonction de décision du libellé, si elle devient extractible (ADR-0099)

## Critères d'acceptation
- [x] Au chargement d'une page, aucune pastille n'affiche « Hors ligne » alors que l'appareil répond
- [x] L'en-tête et les pastilles **ne se contredisent jamais** sur l'état de la connexion
- [x] En perte réelle, « Hors ligne » s'affiche comme aujourd'hui — **vérifié à l'image** (B3)
- [x] Un creux réseau d'une seule requête ne produit pas de clignotement
- [x] Tous les libellés en français
- [x] `pio run` **SUCCESS** (Flash 52,8 %, RAM 30,1 %), `./build_fs.sh` OK, `pio test -e native` **2 742 / 2 742**. Toutes les `tools/check_*.sh` vertes **sauf** `check_release_artefacts.sh` — **attendu**, les signatures datent de 3.1.2.

## Arbitrage retenu (2026-11-22)
- **Piste A retenue** : les pastilles lisent `.app--disconnected` (perte constatée) via `isAppDisconnected()` ; pendant `mid` elles gardent le dernier état connu.
- **Pistes B (libellé d'attente partout) et C (ton éteint conservé) écartées** : elles **clignoteraient** à chaque creux réseau d'une requête.
- **Cas « première ouverture »** : traité par « Chargement… » sur la carte Filtration (aucun override, aucune donnée) — sans quoi la carte aurait affirmé « Arrêtée » sur une pompe non observée.
- `isAppOffline()` intacte (note du guide de pompe, qui rassure pendant qu'une pompe tourne).
- Fichiers : `data/app.js`, `test/filtration_state_vectors.js` (cas neufs + sentinelle d'appelant, validée par mutation), `src/version.h`.
- **Vérifié sans image** (D1 non fait : voir feature-213). B3 (`bad` inchangé) : par lecture du code, non photographié.
- **Reste ouvert, relève de feature-195** : en perte réelle, trois annonces du même fait (en-tête, bandeau, pastilles) ; cartes pH/ORP distinguées de l'état normal par la seule teinte d'une pastille (daltonisme).

## Revue de code — trois constats bloquants, tous traités

### 🔴 R1 — ⭐ **le correctif lui-même n'était couvert par RIEN**
`grep -rl "isAppDisconnected|isAppOffline|app--disconnected" test/ tools/` → **vide**.
⇒ ⛔ Revenir à `isAppOffline()` dans `setDashPill()` restaurait **bug-212 en entier** avec
**toute la CI verte**. ⚠️ Le lot testait sa **conséquence** (`getFiltrationState`) et laissait sa
**cause** nue. ⚠️ `test/dash_pill_vectors.js` **neutralisait** `setDashPill`, il ne voyait donc
jamais la porte hors-ligne.
**Correctif** : table des états `ok` / `mid` / `bad` exécutée contre le `setDashPill` **livré**
(extrait par ancre) et un `document` feint, avec et sans `keepTextOffline`.

### 🔴 R2 — ⭐ **la sentinelle lexicale de l'appelant ne tenait pas**
Elle ne tuait **qu'un** mutant : celui qui avait servi à l'écrire. **5 contournements sur 6**
passaient au vert, dont `const data = latestSensorData || {}` — ⚠️ **l'idiome DOMINANT du
fichier, 11 occurrences** : la prochaine harmonisation de style réintroduisait le défaut.
⛔ **Et le défaut était structurel** : `src.match()` **non global** rend la **première**
occurrence ; l'appel ne matchant plus, le motif tombait sur la **déclaration** de la fonction —
une liste de paramètres qui ne contiendra **jamais** `||`. ⇒ Le garde-fou
« introuvable ⇒ échec » **ne pouvait pas se déclencher**.
⭐ **Récidive exacte de l'incident feature-210** : vert pendant que le défaut vit.
**Correctif** : contrôle **comportemental** — `updateFiltrationBadges` exécuté avec un **espion**
sur `getFiltrationState`, qui vérifie qu'une **absence** est transmise, ⛔ pas un objet vide.
Plus aucun `src.match`, donc ⛔ plus de faux positif sur un commentaire.

### 🔴 R3 — le balisage affirmait encore « Arrêtée »
`data/index.html`, `#filtration-current-status` portait **« Arrêtée » en dur**, ⛔ **avant qu'une
seule ligne de JS tourne**. C'était le **dernier endroit** à porter le faux, alors que les trois
pastilles du tableau de bord déclarent déjà `Chargement…`. ⭐ **Le balisage donnait donc raison
au correctif avant lui** : celui-ci ne crée pas un état, il fait **rejoindre au runtime ce que la
conception avait déjà écrit**. ⇒ Aligné, même mot.

### ⭐ La moitié manquante de l'invariant, fermée ensuite
Les vecteurs ci-dessus garantissaient qu'on **LIT** la bonne classe, ⛔ **pas qu'on la POSE**.
⚠️ Remplacer `state === "bad"` par `state !== "ok"` dans `setNetStatus()` ramenait **bug-212 en
entier**, tests verts. ⇒ Contrôle comportemental sur la **pose**, au même endroit que la
lecture. Mutant n°4 exigé et tué : `.app--offline` posée sur `bad` seulement — il
**déverrouillerait les commandes** pendant « Connexion… ».

### Mutations — **16 posées, 16 tuées**
5 sur la porte hors-ligne, 7 sur l'appelant (dont **les 6 contournements**), 4 sur la pose.
Chacune seule, source restaurée et **prouvée identique** (`cmp`).

## ⭐ « Connexion… » n'est pas photographiable, mais il est EXÉCUTABLE

⚠️ Constat le plus utile du lot. `ui_shot.sh` ne sait produire que `ok` et `bad`
([feature-213](../todo/feature-213-photographier-l-etat-connexion-en-cours.md) existe pour ça).
⇒ Les vecteurs **figent `mid` en CI pour toujours** — ce qu'une capture ne ferait **pas**.
**feature-213 reste utile pour l'œil, elle n'est plus nécessaire comme preuve.**

## Limites CONNUES, écrites et non traitées

⛔ Non couvert, **par choix** (le couvrir redeviendrait lexical, donc contournable) :
- un **second appelant** de `getFiltrationState` ajouté ailleurs ;
- un **autre écrivain** de pastille qui court-circuiterait `setDashPill` ;
- un **second écrivain** de `app--disconnected` ailleurs que `setNetStatus()` ;
- un **appelant qui passerait le mauvais état** à `setNetStatus()` (`"bad"` au lieu de `"mid"`).
⚠️ Écrites ici **et en commentaire dans les tests** : connues plutôt que découvertes.

## Reste ouvert — hors périmètre (feature-195)

- En perte réelle, le même fait est annoncé **trois fois** : en-tête, bandeau, pastilles.
- Les cartes pH/ORP ne s'y distinguent de l'état normal que par la **teinte d'une pastille**
  — angle mort **daltonisme**.
- ⚠️ **Asymétrie assumée** : Filtration dit « Chargement… », Éclairage **déduit** son état du
  mode (ADR-0107/0108). `predictFiltrationRunning()` **existe** et aurait permis la même
  déduction côté filtration : ⛔ ne pas l'employer est un **choix**, écrit pour ne pas devenir
  « la somme que personne ne regarde ».
- ℹ️ Dans `bad` **avant toute donnée**, `#filtration-current-status` passe désormais de
  « Arrêtée » à « Chargement… » : ⚠️ c'est un changement **dans `bad`**, dans le bon sens,
  écrit ici pour ne pas être découvert.

## Plan de test manuel
1. Ouvrir l'application et **regarder la première seconde** : en-tête et pastilles doivent dire
   la même chose. ⚠️ C'est l'état `mid`, ⛔ **non photographiable** aujourd'hui — voir
   [feature-213](../todo/feature-213-photographier-l-etat-connexion-en-cours.md).
2. Couper le réseau et attendre la perte réelle : « Hors ligne » doit revenir, inchangé.
3. Rétablir : retour à l'état normal sans état intermédiaire mensonger.
4. Rejouer à 320 px et 1024 px.

## Notes pour agent
- ⭐ **Le piège de cette fiche** : corriger `isAppOffline()` en bloc. ⚠️ Elle est lue par
  `setDashPill()` **et** par la note du guide de pompe (perte réseau pendant une injection
  manuelle). ⛔ Ces deux surfaces n'ont **pas** le même enjeu : l'une informe, l'autre
  **rassure pendant qu'une pompe tourne**. Vérifier les deux avant de changer la fonction.
- ⚠️ **Vérifier la fiche avant de la traiter** : elle est écrite le 2026-11-22 sur l'état
  v3.1.2. Les numéros de ligne ne sont **pas** donnés volontairement — ils pourrissent.

## Hors périmètre
- ⛔ Le verrou des contrôles hors ligne (`pointer-events: none`), y compris le fait qu'il ne
  bloque pas le clavier — constat préexistant écrit dans bug-196.
- ⛔ Le langage visuel des pastilles lui-même (feature-195) au-delà du strict nécessaire.
