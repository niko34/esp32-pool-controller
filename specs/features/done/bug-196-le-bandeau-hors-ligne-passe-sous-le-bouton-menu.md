## Métadonnées
- **ID** : bug-196
- **Slug** : le-bandeau-hors-ligne-passe-sous-le-bouton-menu
- **Type** : bug
- **Statut** : ✅ **done** — v3.1.2. Revue de code en **deux passes**. ⚠️ La 2ᵉ a conclu « Approve **dès que** `CHANGELOG.md` dit le périmètre réel » : condition **levée** (périmètre corrigé + les 3 constats d'écriture inscrits). ⛔ Aucun verdict Approve formel n'a été réémis après cette dernière écriture.
- **Priorité** : normale
- **Version cible** : à arrêter
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (trouvé pendant son étude d'ergonomie) · [feature-169](../todo/feature-169-onglet-systeme-lisibilite-et-contraste.md) (B5a et B5b trouvent leur réponse ici)

## Domaines touchés
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [ ] architect
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
À 390 px — **l'écran de référence du projet** — la ligne d'état de l'en-tête
« ● Déconnecté — Modifications non sauvegardées » déborde et **passe sous le bouton
de menu**. Les derniers caractères sont illisibles.

## Contexte
Trouvé le **2026-11-18** par `ux-ui-designer` pendant l'étude de feature-195, sur une
capture de l'état réel de l'appareil, puis **vérifié sur image recadrée**.

⚠️ Le défaut n'apparaît que dans l'état **le plus défavorable** : les deux mentions sont
présentes en même temps (perte de connexion **et** modifications non sauvegardées). C'est
pourquoi il a survécu — l'écran nominal n'affiche qu'une puce verte et « En ligne ».

## Comportement attendu
À 390 px, la ligne d'état ne recouvre jamais le bouton de menu, dans **toutes** les
combinaisons d'état. Au choix de l'étude : raccourcir le libellé, le tronquer avec des
points de suspension, passer à la ligne, ou déplacer la mention « modifications non
sauvegardées » hors de l'en-tête. ⛔ Ne pas la **supprimer** sans la remplacer.

⚠️ **Correction (2026-11-22)** : la fiche affirmait que la mention « prévient d'une perte de
saisie ». C'est faux : elle ne consultait **aucun état de saisie**, seule la perte de connexion
la déclenchait (`.app--offline .topbar__offline`). Elle a été remplacée par « Enregistrement
impossible », vraie dans tous les cas.

## Comportement actuel
Les deux mentions s'écrivent à la suite sur une seule ligne, sans largeur maximale ni
troncature, et le bouton de menu est positionné par-dessus.

## Contraintes techniques
- ⚠️ **390 px d'abord** ; vérifier aussi à 320 px.
- Compter les caractères : « Déconnecté — Modifications non sauvegardées » = 43 caractères,
  ⚠️ mais le pire cas est **48** : `data/app.js` peut afficher « Connexion lente » (15 car.) à la
  place de « Hors ligne » (10).
- L'en-tête est **partagé par toutes les vues** : un correctif y touche tous les écrans.
  ⇒ Inventaire des emplois obligatoire (cf. règle du design system).

## Fichiers potentiellement concernés
- `data/index.html` — structure de l'en-tête
- `data/app.css` — mise en page de la ligne d'état
- `data/app.js` — composition du texte (`#net-text`)

## Livrables — check-list
- [x] B1 — Correctif de mise en page (`data/app.css`, `data/index.html`)
- [x] B2 — Vérifié **à la capture, images regardées une à une** : 320, 390, **501** et 1024 px,
      hors ligne **et** en ligne. ⭐ 501 px est ajouté au plan : c'est la largeur la **plus serrée**
      où tout doit encore tenir sur **une** ligne (la mise en deux lignes s'arrête à 500 px).
      ⛔ « Connexion lente » n'est pas atteignable par `ui_shot.sh` : vérifié **par le calcul**
      (≈ 107 px pour 15 car. à ≈ 7,2 px/car., dans ≈ 200 px disponibles à 320 px).
- [x] C1 — `docs/DESIGN_SYSTEM.md` §5.28 (contrat de l'en-tête)
- [x] D1 — Captures avant / après, non versionnées (dossier de travail) :
      `avant_{320,390}_entete.png` → `fix_offline_{320,390,501}_entete.png`,
      `apres_offline_1024_entete.png`, `apres_online_{390,1024}_entete.png`,
      et une **seconde vue** `fix_offline_390_reglages_haut.png` (Réglages › Installation).

## Notes techniques (v3.1.2)
- **Cause** : `.burgerBtn` est `position: absolute` (hors flux) ; rien ne réservait sa place ni ne bornait le texte. Correctif : `.topbar { padding-right: 62px }` dans le `@media (max-width: 980px)` existant ; hors ligne ≤ 500 px le nom cède sa ligne ; barre toujours 64 px.
- **Texte** : « Modifications non sauvegardées » → « Enregistrement impossible » (voir correction ci-dessus). Bouton ☰ 36 → 44 px (⛔ **pas** le focus : il existait déjà, cf. R1) ; couleur `#b45309` → `var(--warn)`.
- **Inventaire** : `.topbar`, `.topbar__status`, `.topbar__offline`, `.status__dot`, `.status__text` : 1 emploi chacun, `index.html` ; rien dans `login.html`, `wifi.html`, `wizard.html`.
- **Périmètre réel** (⛔ corrigé après R2, il disait « aucun JS, aucun firmware ») : `data/index.html` (1 ligne), `data/app.css`, **`data/app.js` (1 ligne)** et **`src/version.h`** ⇒ **envoi firmware + interface**.
- **Reste ouvert, hors de ce lot** : `filtrationDirty` existe mais n'est affiché nulle part — aucun avertissement de saisie en attente. Fiche à créer.

## Critères d'acceptation
- [x] À 390 px, aucun caractère de la ligne d'état n'est recouvert par le bouton de menu
- [x] Idem à 320 px
- [x] La mention hors ligne reste lisible et présente (devenue « Enregistrement impossible »)
- [x] Aucune autre vue n'est dégradée : l'en-tête est **un seul élément du DOM**, et
      `padding-right` ne porte que sur `.topbar` (⛔ rien dans `.main`). Vérifié **en image** sur
      une **seconde vue** (Réglages › Installation, 390 px hors ligne) : en-tête correct,
      contenu en dessous inchangé.

## ⭐ Un défaut **introduit par le correctif**, vu seulement à la capture

⚠️ La première version du correctif réglait bien le recouvrement, mais la **ligne 2 démarrait
au bord du bloc de marque** : elle n'était pas alignée sur le libellé d'état (décalé de la puce
+ son `gap`) et venait **toucher le logo**. ⛔ Invisible en lisant le CSS, évident sur l'image.
⇒ `padding-left: 16px` (8 px de puce + 8 px de `gap`), **commenté dans le code avec sa raison**.
⭐ C'est la règle du projet appliquée à elle-même : **regarder le rendu**, ⛔ ne pas l'imaginer.

## Revue de code — constats traités

### 🔴 R1 — le CHANGELOG annonçait un apport qui n'en était pas
Il disait que le bouton ☰ « reçoit un contour de focus clavier **qu'il n'avait pas** ».
⛔ **Faux** : `data/app.css:183-188` applique `button:focus-visible { outline: 2px solid
var(--accent); outline-offset: 2px }` **depuis toujours**, et `.topbar a:focus-visible
{ outline:none }` ne vise que les `<a>` — or le ☰ est un `<button>` **frère** du lien, pas
un descendant. ⇒ La règle locale ajoutée était **inerte** : elle est **retirée**, et la phrase
livrée au client est corrigée. ⚠️ Le passage **36 → 44 px**, lui, est réel et conservé.

### 🔴 R2 — ⭐ « Enregistrement impossible » était FAUX pendant la connexion
`setNetStatus` pose `.app--offline` sur **tout ce qui n'est pas `ok`** — donc aussi sur
`mid` (« Connexion… »). Or `mid` est posé **au démarrage nominal, juste APRÈS un `loadConfig`
RÉUSSI** (`data/app.js:16492`) et après **un seul** échec de `/data` (`:10168`).
⇒ L'application venait de prouver que l'appareil répond, et annonçait la ligne suivante que
l'enregistrement était impossible. ⛔ C'est exactement le défaut que ce lot prétend corriger.
⚠️ **Et ce lot y ajoutait une régression neuve** : le masquage du nom aurait fait **disparaître
puis reparaître « PoolController » à chaque chargement de page sur téléphone**.

**Correctif** : une **seconde classe**, `.app--disconnected`, posée **seulement** sur `bad`
(perte constatée). Elle seule autorise l'affirmation à l'écran et le masquage du nom.
⛔ `.app--offline` **garde exactement son rôle** : le verrou des contrôles ne change pas.
⭐ **Effet de bord utile** : l'ancienne mention clignotait déjà à chaque chargement — ce défaut
préexistant disparaît aussi.
⚠️ Coût assumé : le lot n'est plus « sans JS ». **Une ligne** (`classList.toggle`), commentée.

### Constats mineurs traités
- Commentaire ajouté dans `app.css` : `overflow`/`text-overflow` sur `.topbar__offline` ne
  tiennent **que** parce que `.topbar__status` est `display:flex` — repasser le conteneur en
  non-flex ferait tomber le filet **en silence**.
- `docs/DESIGN_SYSTEM.md` §5.28 : la hauteur est un `height`, ⛔ **pas** un `min-height` — à
  fort grossissement du texte le contenu **déborde** au lieu de pousser la barre.

## Reste ouvert — écrit, non traité

- `filtrationDirty` (`data/app.js`) : l'appareil **sait** qu'une programmation de filtration
  est modifiée sans être enregistrée, et ⛔ **ne le dit nulle part**. Fiche à créer.
- Le verrou hors ligne est en `pointer-events: none` (`app.css`), qui ⛔ **ne bloque pas le
  clavier** : un utilisateur au clavier peut encore activer un bouton. Préexistant, hors
  périmètre. ⚠️ Cela n'invalide pas la mention (l'appareil étant injoignable, la requête
  échoue réellement), mais le CHANGELOG s'appuie sur ce verrou pour expliquer le grisage.
- ⭐ **Le même faux survit sur les pastilles du tableau de bord, et il y est désormais SEUL.**
  `setDashPill` passe par `isAppOffline()`, qui lit **`.app--offline`** — ⛔ pas la classe neuve.
  ⇒ Pendant « Connexion… », les pastilles affichent **« Hors ligne »** alors que l'en-tête dit
  « Connexion… ». ⚠️ Les deux surfaces sont rafraîchies **dans le même geste**
  (`refreshDashPills()` est appelé depuis `setNetStatus`) et **se contredisent**.
  ⛔ Préexistant, ⛔ **non introduit** par ce lot — mais le lot en **change le statut** : l'en-tête
  ne porte plus le faux, les cartes si. Le ton `off` et `keepTextOffline` relèvent de
  feature-195 ⇒ hors périmètre. **Écrit ici pour ne pas être oublié.**
- ⭐ **L'état « Connexion… » n'est pas photographiable.** `ui_shot.sh` ne l'atteint pas : il est
  transitoire, et l'option `-n` installe un WebSocket **qui s'ouvre**. ⇒ Un drapeau qui laisserait
  le faux WebSocket **en cours d'ouverture**, avec un `/get-config` qui répond, figerait
  l'application dans cet état et le rendrait **opposable**. Fiche à créer. ⚠️ Tant qu'il n'existe
  pas, tout lot qui touche à cet état se vérifie « par lecture » — et le point ci-dessus montre
  que ce n'est **pas théorique** : un faux y vivait sans que personne ne l'ait vu.
- Le séparateur « — » est un `::before` : Chrome et Firefox l'exposent à l'arbre
  d'accessibilité, donc un lecteur d'écran annonce le cadratin. La sortie propre serait un
  `border-left` plutôt qu'un caractère. Non traité.

## Plan de test manuel
1. Appareil injoignable **et** une modification non enregistrée en cours : les deux
   mentions coexistent — c'est le cas qui reproduit le défaut.
2. Les trois autres combinaisons (en ligne/hors ligne × avec/sans modification).
3. Rejouer à 320 px et à 1024 px.

## Notes pour agent
- ⚠️ La capture se prend **sans** l'option `-n` de `ui_shot.sh` : c'est l'absence de
  WebSocket qui met l'application hors ligne. Avec `-n`, le défaut ne se voit pas.
