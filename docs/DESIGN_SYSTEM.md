# Design System — Interface web (`data/`)

> Document de référence, **pas une spec**. Il ne se termine jamais, ne passe jamais par
> `todo/doing/done`. Il décrit ce qui EST dans `data/app.css` / `data/index.html` et ce qui
> DOIT être employé pour chaque geste d'interface. Voir « Comment ce document se maintient »
> en fin de fichier avant d'y toucher, et §10 pour ce qu'il ne couvre PAS encore.

## Sommaire
1. [Principe directeur](#1-principe-directeur)
2. [Index rapide par geste](#2-index-rapide-par-geste)
3. [Jetons](#3-jetons)
4. [États obligatoires](#4-états-obligatoires)
5. [Catalogue des composants](#5-catalogue-des-composants)
6. [Responsive — 390 px d'abord](#6-responsive--390-px-dabord)
7. [Daltonisme](#7-daltonisme)
8. [Mode sombre](#8-mode-sombre)
9. [Vérification visuelle](#9-vérification-visuelle)
10. [Portée actuelle](#10-portée-actuelle--ce-document-est-incomplet-et-le-dit)
11. [Comment ce document se maintient](#11-comment-ce-document-se-maintient)

## 1. Principe directeur

**Un geste utilisateur = un composant**, quel que soit l'écran où ce geste apparaît.
« Choisir un mode parmi 2 à 4 options depuis une carte de statut » n'a **qu'une** réponse
(§5.2), que la carte soit Filtration, pH, ORP ou Éclairage.

Un composant est défini par **son rôle** et **son ou ses contextes déclarés** — pas par
l'écran où il a été écrit la première fois. Avant de réutiliser une classe existante sur
un nouvel écran, vérifier dans ce document qu'elle **déclare** ce contexte. Si elle ne le
déclare pas : ne pas réutiliser telle quelle — soit étendre sa déclaration ici (et vérifier
qu'aucun contexte existant n'en souffre), soit choisir un autre composant du catalogue.

C'est l'absence de cette règle qui a produit le défaut de 2026-10-14 : `.segmented` est
déclaré pour « page de réglage », et a été repris tel quel sur une carte d'accueil sans
que personne ne se demande si son habillage (cadre, fond, ombre) avait un sens hors de son
contexte d'origine.

## 2. Index rapide par geste

| Je veux… | Composant | Contexte(s) déclaré(s) |
|---|---|---|
| Choisir un mode (2-4 options) depuis une carte d'accueil **ou** en tête d'une page de réglage pH/ORP | `.mode-select` (§5.2) | Carte d'accueil **et** page de réglage |
| Barre d'onglets (≥5 items), choix ponctuel sans card-header (taille bassin, vitesse de régulation, cadence d'affichage) | `.segmented` (§5.3) | Page de réglage uniquement |
| Saisie numérique par positions (consigne pH) | `.segmented--num` (§5.3) | Page de réglage uniquement |
| Incrémenter/décrémenter une valeur (offset) | `.stepper` | Page de réglage uniquement |
| Déclencher une action immédiate (Allumer, Activer, Arrêter une injection) | `.btn` + variante (§5.4) | Les deux |
| Afficher une option secondaire dans une carte (Boost) | `.card__option` (§5.5) | Carte d'accueil |
| Afficher une paire libellé/valeur en lecture seule | `.detail-row` (§5.6) | Les deux |
| Indiquer un état ponctuel (En marche / Inactif / Calibration requise) | `.state-badge` (§5.7) | Les deux |
| Visualiser une mesure sur une échelle | `.gauge` (§5.8) | Carte d'accueil |
| Expliquer pourquoi une mesure n'est pas représentative | `.sensor-badge` (§5.9) | Carte d'accueil |
| Alerter sur une condition bloquante ou un risque | `.callout` (§5.10) | Les deux |
| Commenter un contrôle sans en faire une alerte | `.field__hint` (§5.11) | Les deux, **jamais seul porteur d'une info de sécurité** |
| Signaler une anomalie de régulation nécessitant un acquittement | `.regul-alert` (§5.12) | Carte d'accueil et page de réglage |
| Saisir une valeur dans un formulaire de réglage | `.field` (§5.13) | Page de réglage — ⚠️ nom repris **sans styles partagés** par `wizard.html` et `wifi.html` |
| Activer/désactiver une option binaire | `.switch` (§5.14) | Page de réglage uniquement |
| Ajuster une valeur par pas fixes, sans clavier | `.stepper` (§5.15) | Page de réglage (pH) — ⚠️ nom employé par `wizard.html` pour un geste **DIFFÉRENT** |
| Suivre une calibration en plusieurs étapes | `.calibration-steps` (§5.16), `.cal-stepper` si chaque étape a son action (§5.17) | Pages de réglage et assistant de débit |
| Indiquer la robustesse d'un mot de passe | `.password-strength` (§5.18) | Page de réglage Sécurité — ⚠️ **dupliqué** dans `wizard.html` |
| Lister les critères de validité d'un mot de passe | `.password-checklist` (§5.19) | Page de réglage Sécurité — ⚠️ **dupliqué** dans `wizard.html` |

## 3. Jetons

Tous définis dans `:root` de `data/app.css`. Ne jamais recréer une couleur/espacement en
dur dans une nouvelle règle si un jeton existe déjà.

**Couleurs** — `--text` (#111827), `--text-secondary`/`--muted` (gris, AA validé),
`--accent`/`--accent-hover`/`--accent-light` (bleu, jamais `--accent-light` comme couleur de
texte seule), `--success`/`--warn`/`--danger` (texte, contrastes AA documentés en commentaire
à côté de chaque valeur — ne jamais changer une valeur sans revérifier le ratio et mettre à
jour le commentaire), `--gauge-*` (aplats de jauge, **jamais** utilisés comme couleur de
texte : ce sont des fonds de piste, pas des teintes AA).

**Espacement** — pas de jeton générique `--space-*` aujourd'hui : les marges sont posées
composant par composant (14px entre cartes, 16px padding de carte, 8-14px entre blocs
internes). **Lacune connue** (§10) : tant qu'aucun jeton d'espacement n'existe, tout nouveau
bloc à l'intérieur d'une carte doit se caler sur 8 / 12 / 14 / 16px, jamais une valeur
arbitraire.

**Rayon** — `--radius` (12px, contrôles), `--radius-lg`/`--card-radius` (16px, cartes),
`--radius-sm` (8px, petits éléments).

**Ombres** — `--shadow-sm` (repos), `--shadow` (carte), `--shadow-lg`, `--shadow-hover`.
Une ombre signale une élévation réelle (carte, pastille active) — ne pas en ajouter à un
élément qui n'a pas besoin d'être perçu « au-dessus » du reste.

**Transitions** — `--transition-fast` (150ms, hover de détail), `--transition-base` (200ms,
changement d'état standard), `--transition-slow` (300ms, apparition de bloc).

## 4. États obligatoires

Tout élément interactif (bouton, segment, badge cliquable) déclare au minimum :

| État | Attendu |
|---|---|
| Normal | Contraste AA, cible tactile ≥ 44×44px |
| Survol (`:hover`) | Changement perceptible mais discret (translateY, fond) — `prefers-reduced-motion` respecté (déjà géré globalement) |
| **Sélectionné/actif** | **Jamais la couleur seule** : fond plein + `aria-pressed="true"` ou `.is-active` + contraste de texte inversé (blanc sur accent). Un bouton non sélectionné **doit avoir l'air d'un bouton** (bordure ou fond visible à l'œil nu) — c'est l'état que le défaut du jour a laissé invisible |
| Focus clavier (`:focus-visible`) | `outline: 2px solid` une couleur du jeton, jamais supprimé |
| Désactivé | `opacity` réduite + `cursor:not-allowed` + **texte qui explique pourquoi** à proximité (jamais un bouton mort sans explication) |
| Chargement | `.btn--loading` (spinner, texte masqué) |
| Donnée absente | `—` (tiret cadratin), jamais `0`, `null` ou une chaîne vide |

## 5. Catalogue des composants

### 5.1 Carte de statut — `.card.card--status`
**Rôle** : une tuile du tableau de bord (Filtration, Éclairage, pH, ORP, Température).
**Contexte** : carte d'accueil uniquement (la page de réglage utilise `.card--detail`,
colonne unique, pas d'en-tête icône+badge).
**Structure** : `.card__status-header` (icône, libellé, **un seul** badge d'état, bouton
réglages) → `.card__body` (contenu propre à la carte, voir composants ci-dessous).
⚠️ `.card--status .card__body` n'a **pas de `gap`** — c'est une dette (§10). En attendant,
tout nouveau bloc ajoute sa propre marge, toujours une valeur du jeton d'espacement (§3),
**jamais 0** sauf si le bloc précédent porte déjà la marge.

### 5.2 Sélecteur de mode — `.mode-select`
**Rôle** : choisir UN mode parmi 2 à 4, avec effet immédiat (pas de validation séparée).
**Contexte déclaré** : carte d'accueil (Filtration, pH, ORP, et Éclairage dès feature-162)
**et** page de réglage quand le même choix y est dupliqué (`#ph_regulation_mode`,
`#orp_regulation_mode`, `#lighting_...` à venir).
**Base** : ce que Filtration fait déjà — ne pas réinventer.
**Balisage** :

```html
<div class="grid mode-select" role="group" aria-label="Mode de régulation pH">
  <button type="button" class="btn btn--ghost" data-value="automatic" aria-pressed="false">Auto</button>
  <button type="button" class="btn btn--ghost" data-value="scheduled" aria-pressed="false">Programme</button>
  <button type="button" class="btn btn--ghost" data-value="manual" aria-pressed="false">Arrêt</button>
</div>
```

**CSS** : `.mode-select` étend `.grid` avec `grid-template-columns: repeat(N, minmax(0,1fr))`
(N = nombre d'options, **jamais de `flex-wrap`** : une grille CSS ne reflue pas, ce qui
élimine structurellement le bug « 2+1 »), `gap: 8px`.
**JS** : un seul mécanisme, déjà écrit — `classList.toggle('btn--primary', active)` /
`classList.toggle('btn--ghost', !active)` / `aria-pressed`. `updateFiltrationModeButtons`
est la référence ; `bindDashboardModeSelectors` (pH/ORP) doit converger vers le même motif
plutôt que vers `initSegmented`.
**Pourquoi pas `.segmented`** : `.segmented` porte un cadre (bordure + fond + ombre) pensé
pour un sélecteur isolé dans une page de réglage ; dans une carte qui a déjà un en-tête
(icône, titre, badge), ce cadre crée un second niveau de boîte dans la boîte — c'est
exactement le défaut remonté.

### 5.3 `.segmented` et ses variantes — page de réglage uniquement
**Rôle** : barre d'onglets ou sélection à options nombreuses, **sans** en-tête de carte à
proximité immédiate (le cadre y a un rôle : délimiter un groupe dans une page dense de
champs).
**Contexte déclaré** : page de réglage. Exemples légitimes : `#settings-segmented` (9
onglets), `#pool_size` / `#regulation_speed` (3 options, mais en plein milieu d'un
formulaire, pas sous un en-tête de carte), `#trace_rate_select`.
**Variante `.segmented--num`** : positions numériques de largeur égale (consigne pH) —
ne s'emploie que pour une suite de valeurs, jamais pour un mode.
⚠️ **Piège mesuré** : la règle `@media (max-width: 640px) { .segmented__btn { flex: 1 1 calc(50% - 4px) } }`
force **deux boutons par ligne**. Elle a été taillée pour les 9 onglets de la page Réglages ;
appliquée à un sélecteur à **3** options, elle produit un « 2 + 1 » où le troisième occupe
toute la largeur. ⛔ C'est la cause mécanique du défaut du 14/10 — pas un `flex-wrap`
générique.
⛔ **Ne plus ajouter** de nouvel usage de `.segmented` nu à 2-4 options en tête d'une page
dont la section répète un sélecteur déjà présent sur la carte d'accueil correspondante —
utiliser `.mode-select` (§5.2) aux deux endroits pour que l'utilisateur retrouve le même
repère visuel.

### 5.4 `.btn` et variantes
`.btn` (neutre bleuté), `.btn--ghost` (gris, état « non sélectionné » d'un `.mode-select`
ou action secondaire), `.btn--primary` (accent plein, action principale **ou** option
sélectionnée d'un `.mode-select`), `.btn--ok` (vert, confirmation), `.btn--danger` (rouge,
action destructive — ne jamais le coller à l'action principale). Toutes ≥44px de haut.

### 5.5 `.card__option`
**Rôle** : option mineure secondaire à l'intérieur d'une carte de statut (ex. Boost).
**Contexte** : carte d'accueil uniquement. Fond `--panel2`, liseré `--stroke`, titre 14px
sans majuscules forcées (contraste voulu avec `.card__status-label`, qui lui capitalise).
Un seul gabarit « option » pour tout le projet — ne pas en créer un second par carte.

### 5.6 `.detail-row`
Paire libellé/valeur en lecture seule, `justify-content:space-between`. Piloté par
`[hidden]` (pas `style.display`) : `.detail-row[hidden]{display:none!important}` existe déjà
et l'emporte sur `.detail-row{display:flex}` — toujours utiliser l'attribut `hidden`, jamais
un style inline, pour toute nouvelle ligne conditionnelle.

### 5.7 `.state-badge`
Pastille d'état : `--ok` (vert, fonctionnement normal), `--off` (gris neutre, à l'arrêt),
`--active` (bleu accent, option activée), `--warn` (ambre, attention sans danger immédiat).
**Un seul badge d'état par en-tête de carte** — ne pas empiler deux pastilles qui décrivent
la même chose sous deux angles (motif déjà choisi pour Filtration : `état réel` dans le
badge, `mode demandé` dans `.card__status-mode`, jamais les deux dans un badge).

### 5.8 `.gauge`
Jauge de mesure (pH/ORP/Température). Piste neutre `--gauge-track-bg`. Dégradé évaluatif
(rouge→vert→bleu) réservé au pH ; dégradé descriptif (froid→chaud) réservé à la température ;
palette ASYMÉTRIQUE pour l'ORP (rouge sous consigne, vert dans la bande, neutre au-dessus) —
**ne jamais réutiliser la palette pH pour une autre grandeur** : les jetons `--gauge-*` sont
nommés par grandeur précisément pour empêcher ce mélange.

### 5.9 `.sensor-badge`
Bandeau expliquant pourquoi une mesure n'est pas représentative (ex. « Filtration arrêtée »).
Variantes `--info` (bleu, état normal), `--warning` (ambre, anomalie), `--danger` (rouge).
**Contexte** : carte d'accueil, juste sous la jauge. `margin-top:8px`, **aucun
`margin-bottom`** aujourd'hui — tout bloc placé juste après doit apporter sa propre marge
haute.

### 5.10 `.callout` et variantes (`--info`, `--success`, `--warn`)
Alerte ou information qui mérite un encadré visible (fond teinté, liseré). Réservé aux
messages qui **engagent une action ou une sécurité** (gates `pool-chemistry`, bidon bas,
calibration en cours). Ne pas en faire un habillage décoratif pour un texte purement
informatif sans enjeu — c'est à ça que sert `.field__hint` ou `.detail-row`.

### 5.11 `.field__hint`
Texte d'aide discret (12px, `--muted`, pas de fond) attaché à **un contrôle précis** juste
au-dessus ou en dessous de lui. ⚠️ **Ne doit jamais être le seul support d'une information de
sécurité ou de conséquence directe d'un mode** (ex. « la régulation automatique ne dose
plus ») : dans ce cas, la positionner **immédiatement adjacente** au badge/bandeau qui
annonce la cause (`.sensor-badge`, `.regul-alert`), pas après le contrôle qui a produit l'état.
Ce point est le cœur du défaut « message presque invisible » du 14/10.

### 5.12 `.regul-alert` (+ `--inline`)
Bandeau d'exception de régulation (escalade de sécurité pH, étalonnage en cours). Rouge
clignotant (`__dot`) réservé aux alertes qui exigent un acquittement **sur la page
détaillée** — jamais d'acquittement depuis une carte d'accueil.

### 5.13 `.field` (famille complète)
**Rôle** : envelopper un contrôle de formulaire (label, aide, erreur, succès) dans un bloc
cohérent — PAS un geste en soi, mais le conteneur de tout geste de saisie de réglage.
**Contexte déclaré, vérifié** : `data/index.html` **uniquement** pour cette implémentation CSS
(classes BEM `.field__label/__hint/__error/__success`, `.field--inline`).
⚠️ **`wizard.html` ET `wifi.html` ont chacun leur PROPRE `.field`**, défini dans leur `<style>`
interne — **aucun des deux ne charge `app.css`**. Même nom de classe, **trois implémentations
indépendantes, zéro partage de règle** : `app.css` en BEM avec jetons ; `wizard.html` et
`wifi.html` en sélecteurs plats avec couleurs hex en dur (valeurs identiques entre eux, copie
mutuelle probable). `login.html` n'emploie pas `.field` du tout — il a sa propre convention
`.form-group`, **un quatrième nom**.
**États, confrontés à §4** : normal / hover / focus ✅ · désactivé ✅ (+ variante `.is-computed`
pour une lecture seule calculée) · erreur et validé ✅ via `.field.is-valid` / `.is-error` posés
sur le **conteneur**.
⚠️ **MANQUE constaté** : **aucun `aria-invalid` dans tout le projet** (0 occurrence). Un lecteur
d'écran n'est jamais informé qu'un champ est en erreur — seule la couleur du label change, et
`.field__error` n'est pas relié à l'input par `aria-describedby`. ⚠️ C'est exactement l'état
« couleur seule » que §4 et §7 interdisent pour un bouton ; il n'était simplement pas encore
nommé pour un champ.
⛔ **Composant mort constaté** : `.field__label-icon` (et ses variantes) — **0 usage** dans
`index.html`. Candidat à suppression, à confirmer (vérifier qu'aucun JS ne l'injecte).

### 5.14 `.switch`
**Rôle** : activer/désactiver une option binaire persistante. ⛔ Pas un mode à 3 valeurs — pour
ce geste, voir §5.2.
**Contexte déclaré, vérifié** : **page de réglage uniquement** — 8 emplois dans `index.html`,
**aucun** sur une carte d'accueil, **aucun** dans `wizard.html` / `wifi.html` / `login.html`.
**Sémantique** : `<input type="checkbox">` natif + `aria-labelledby` vers un titre externe — pas
de `role` custom, l'état coché est natif.
**États** : normal / hover / coché / focus et `:focus-visible` ✅.
⚠️ **MANQUE constaté** : **aucune règle `:disabled`**. Si un développeur désactive un
interrupteur (verrouillage pendant une sauvegarde), **rien ne le distingue visuellement** d'un
interrupteur actif — alors que `.btn` et `.mode-select`, eux, déclarent cet état (§4).

### 5.15 `.stepper` — incrémenteur ±
**Rôle** : ajuster une valeur par pas fixes, **sans clavier libre** (correction de lecture pH,
± 0,1 par appui).
**Contexte déclaré, vérifié** : page de réglage pH (`#ph-tune-stepper`), **seul emploi réel**.
⛔ **COLLISION DE NOM, pas un composant partagé** : `wizard.html` définit **sa propre** `.stepper`
pour un geste **totalement différent** — la barre de progression du parcours d'installation.
**Aucune règle commune** : seule la coïncidence du nom crée un risque de confusion. ⚠️ À rappeler
dans tout futur renommage : « stepper » désigne **deux choses** selon le fichier.
**États** : normal / focus ✅ · donnée absente `--` conforme §4 · `role="spinbutton"` complété
par `aria-valuenow` posé en JS (**vérifié, pas supposé**).
⚠️ **Piège** : une décision produit a **retiré le clavier** de ce contrôle. Un futur `.stepper`
qui réintroduirait une saisie libre doit **redéfinir son rôle**, pas réemployer celui-ci.

### 5.16 `.calibration-steps` / `.step`
**Rôle** : présenter une séquence numérotée d'étapes, avec un état par étape.
**Contexte déclaré, vérifié** : pages de réglage Température, pH, ORP, et assistant de débit —
toujours dans `index.html`.
**États** : `is-active` (pastille accent) · `is-completed` (pastille verte) · `is-upcoming`
(atténué) ✅ · `aria-current="step"` sur l'étape active ✅.
⚠️ **Deux formes du même composant, volontaires mais jamais déclarées avant ce document** :
`.calibration-steps` **seule** quand l'action est **globale** sous la liste (Température,
assistant de débit) ; `.cal-stepper` (§5.17) quand **chaque étape** porte sa propre action
(pH/ORP). ⛔ Ne pas ajouter le wrapper à un parcours qui n'en a pas besoin : il change la mise en
page (boutons à pleine largeur).

### 5.17 `.cal-stepper`
**Rôle** : variante de contexte de §5.16 pour un parcours où **chaque étape** porte son bouton
et/ou son champ.
**Contexte déclaré, vérifié** : exclusivement pH et ORP.
**CSS propre au contexte** : `.btn` interne forcé à `width:100%` + `min-height:44px` ; la pastille
`is-completed` remplace le chiffre par un « ✓ » **injecté en CSS**, jamais dans le DOM — le sens
est porté par l'état, pas par le glyphe.
**Verrouillage croisé** des boutons pendant une calibration : passe par `.btn:disabled`
générique (§5.4), **pas** par une règle propre. Cohérent.

### 5.18 `.password-strength`
**Rôle** : retour visuel **continu** (pas un simple pass/fail) sur la robustesse d'un mot de
passe en cours de saisie.
**Contexte déclaré, vérifié** : page de réglage Sécurité.
⛔ **DUPLIQUÉ INTÉGRALEMENT ET INDÉPENDAMMENT dans `wizard.html`** — mêmes noms, mêmes seuils,
implémentation **copiée** plutôt que partagée.
**États** : masqué avant saisie (donnée absente, conforme §4) · `.weak` / `.medium` / `.strong`.
⚠️ **MANQUE constaté** : la force est portée **par la seule couleur** des barres. Le mot
« faible / moyen / fort » vit dans un libellé **placé après**, et aucun `aria-live` ne l'annonce.
⚠️ **Piège majeur** : toute évolution des seuils doit être répercutée dans **les deux** fichiers.
Sans quoi « fort » n'a pas le même sens selon qu'on crée le compte au premier démarrage ou qu'on
change le mot de passe depuis les réglages. **Rien ne garantit cette parité aujourd'hui** — ni
test, ni génération partagée.

### 5.19 `.password-checklist` / `.password-checklist-item`
**Rôle** : lister les critères de validité d'un mot de passe et cocher ceux satisfaits, en direct.
**Contexte déclaré, vérifié** : page de réglage Sécurité. ⛔ **Même duplication que §5.18** dans
`wizard.html`.
**États** : masqué avant saisie · `.valid` colore le texte et remplit l'icône (`✓` injecté en CSS).
⚠️ **MANQUES constatés** : **aucun `aria-live`** — un critère qui devient satisfait pendant la
frappe n'est annoncé à aucun lecteur d'écran ; et l'icône emploie une bordure **codée en dur**
alors qu'un jeton existe (§3).

## 6. Responsive — 390 px d'abord

Référence de calcul pour toute nouvelle rangée de boutons dans une carte d'accueil :
- Largeur d'écran : 390px.
- `.main{padding:18px}` → largeur de carte ≈ **354px**.
- `.card__body{padding:16px}` → largeur utile ≈ **322px**.
- Pour **3 colonnes** (`.mode-select` à 3 options), `gap:8px` → `(322 - 16) / 3 ≈ 102px` par
  colonne. Avec un padding horizontal de bouton de 10px de chaque côté → **≈ 82px** de zone de
  texte. Le libellé le plus large du projet pour ce geste est « Programme » (9 caractères) ;
  à 13px/600 (police système), sa largeur mesurée est d'environ 66-70px → **tient**, avec
  une marge de sécurité de 12-16px. « Auto » (4) et « Arrêt » (5, avec accent) sont plus
  courts : aucun risque.
- Pour **4 colonnes** sur une seule ligne, le même calcul ne tient PAS (zone de texte
  ≈ 45-50px < 66px) — c'est pourquoi Filtration (4 options) reste en grille **2×2**
  (`grid--2`), jamais en ligne unique. Ne pas copier le motif 3-colonnes vers un sélecteur à
  4 options sans refaire ce calcul.
- Toute nouvelle rangée de boutons/libellés : refaire ce calcul (largeur de colonne −
  padding horizontal du bouton ≥ largeur mesurée du libellé le plus long + 10px de marge),
  ne jamais se contenter d'« a l'air de tenir ».

## 7. Daltonisme

Aucun état ne doit reposer sur la seule teinte :
- `.mode-select` / `.segmented__btn.is-active` : fond plein **+** texte blanc **+**
  `aria-pressed="true"` (le lecteur d'écran et le focus clavier portent la même info).
- `.gauge` : toujours doublée d'un marqueur de position ET d'un texte dans
  `.gauge__legend-state` quand l'état sort du nominal (jamais la couleur seule sur la piste).
- `.sensor-badge` / `.callout` : la couleur de fond n'est jamais le seul signal — le texte
  nomme explicitement la nature du message (« Filtration arrêtée », pas une pastille vide).
- Courbes à plusieurs séries (ex. carte Signaux capteurs) : teintes choisies pour un écart
  perceptible en simulation protanopie/deutéranopie, **pas seulement un écart de teinte** —
  varier aussi la clarté ou le style de trait (plein/pointillé) quand deux séries sont
  proches sémantiquement (brut/lissé). C'est le défaut déjà remonté sur feature-153 : ne pas
  le reproduire.

## 8. Mode sombre

**État actuel : non implémenté.** Aucune règle `prefers-color-scheme` dans `app.css`, aucun
jeu de jetons sombres. Ne pas improviser un mode sombre partiel sur un seul composant à
l'occasion d'un autre lot — si le mode sombre devient une demande, il doit introduire son
propre jeu de jetons (`--bg-dark`, `--panel-dark`, etc.) et repasser par ce document avant
tout code. Tant que ce n'est pas fait, **tout contraste AA validé dans ce document est
validé sur fond clair uniquement**.

## 9. Vérification visuelle

**Outil : `./tools/ui_shot.sh`.** Il affiche `data/` dans un navigateur sans interface, avec les
données **réelles recopiées depuis l'appareil**, et enregistre une capture PNG. ⇒ On **regarde**
le rendu au lieu de le déduire d'une feuille de style.

```bash
./tools/ui_shot.sh                              # tableau de bord, 390 px
./tools/ui_shot.sh -w 1024                      # largeur desktop
./tools/ui_shot.sh -r '#/ph' -o /tmp/ph.png     # une autre page
./tools/ui_shot.sh -f cal_switch_active=true    # forcer un état rare
```

⚠️ **`-f` est le plus utile et le moins évident** : il force un champ de `/data` pour atteindre un
état qu'on ne peut pas provoquer à la demande. C'est ainsi qu'a été vérifié le rendu **verrouillé**
(étalonnage en cours) du 14/10, que personne n'avait jamais vu.
⛔ Les données recopiées contiennent le **SSID Wi-Fi** et l'**identifiant MQTT** : elles vont dans
un dossier temporaire, **jamais dans le dépôt**. Ne pas les y ajouter « pour la reproductibilité ».

Avant toute livraison touchant `data/app.css` ou la structure d'une carte :
1. Capturer la vue concernée à **390px** (référence mobile) et comparer visuellement les
   cartes entre elles sur le **même tableau de bord** — pas une carte isolée. Le défaut du
   14/10 n'était détectable qu'en comparant Filtration à pH/ORP côte à côte.
2. Capturer la même vue à une largeur desktop (≥1024px) pour vérifier qu'aucune grille ne
   dégénère (colonnes qui s'étirent à l'excès, texte perdu dans un espace vide).
3. Si un composant du catalogue (§5) est modifié, capturer **toutes** les cartes qui le
   déclarent (§2, colonne contexte) — pas seulement celle qui a motivé le changement.
4. Joindre les captures avant/après à la revue — une description textuelle de
   l'espacement ou du contraste ne remplace pas l'image.

⚠️ ⚠️ **Une description textuelle d'un espacement ou d'un contraste ne remplace pas l'image.** Le
défaut du 14/10 a traversé une étude d'ergonomie, une implémentation et une revue — toutes faites
en lisant du code.

## 10. Portée actuelle — ⚠️ ce document est INCOMPLET, et le dit

**Mesuré le 2026-10-14, après la passe « Formulaires et saisie »** : `data/app.css` contient
**108 familles de composants** (classes racines — 107 avant l'ajout de `.mode-select`) ; ce
document en nomme **40**.

⚠️ **ET LE DÉNOMINATEUR EST FAUX — découvert pendant cette même passe.** `app.css` **n'est pas la
seule feuille de style du projet** : `wizard.html` (**40** familles), `wifi.html` (**18**) et
`login.html` (**7**) portent chacun leur CSS **dans un `<style>` interne**, et ⛔ **aucun des
trois ne charge `app.css`**. Soit **65 familles supplémentaires** qui n'ont jamais figuré dans
aucun inventaire.

⚠️ **Conséquence, bien pire qu'un trou de documentation** : plusieurs noms sont **réemployés avec
des implémentations totalement indépendantes** — `.field` existe en **trois** versions sans règle
commune (et `login.html` appelle la même chose `.form-group`, un quatrième nom) ; `.stepper`
désigne **deux gestes sans rapport** ; `.password-strength` et `.password-checklist` sont
**dupliqués à l'identique** entre `app.css` et `wizard.html`. ⇒ Un changement de seuil de
robustesse appliqué d'un seul côté **désynchronise silencieusement** les deux parcours de mot de
passe, et rien — ni test, ni génération partagée — ne garantit leur parité.

⛔ **Ne pas lire ce document comme un inventaire exhaustif.** Il a été écrit en réponse à un
défaut précis — deux composants différents pour le même geste sur les cartes du tableau de bord
— et couvre donc d'abord ce vocabulaire-là, plus les règles transverses (jetons, états,
responsive, daltonisme, gouvernance) qui, elles, s'appliquent à **tout**.

⚠️ **Conséquence pratique** : si le geste que vous cherchez n'est pas dans l'index §2, cela ne
veut **pas** dire qu'aucun composant n'existe — cela veut dire qu'il n'est **pas encore
documenté**. Allez le lire dans `data/app.css`, puis **ajoutez-le ici** (§11).

### Familles non encore couvertes, par thème

| Thème | Familles |
|---|---|
| Navigation et cadre | `nav`, `topbar`, `burger`, `view`, `settings` |
| Modales et surcouches | `probe-modal`, `ttt` |
| Bandeaux et alertes | `dose-block-banner`, `rescue-active-banner`, `calibration-alert`, `alert`, `wifi-notification`, `tips` |
| Graphiques | `chart`, `chart-tab`, `chart-export`, `u-tooltip`, `ph-stats`, `orp-stats`, `trace-block`, `cal-stability` |
| Cartes spécialisées | `install-card`, `product-card`, `product-volume`, `daily-dose` |
| Petits éléments | `pill`, `chip`, `kv`, `readout`, `metric`, `status`, `row`, `panel`, `terminal` |

⚠️ **Ce tableau est lui-même daté.** Le recompter plutôt que le recopier :
`grep -oE "^\.[a-z][a-z0-9-]*" data/app.css | sed 's/^\.//;s/--.*//' | sort -u | wc -l`

## 11. Comment ce document se maintient

- **Qui le déclenche** : quiconque touche `data/app.css` ou la structure DOM d'une carte/page
  (`ux-ui-designer` en premier, `web-ui-developer` en implémentant). La règle « l'UX n'est pas
  optionnel » de `CLAUDE.md` s'applique : si `ux-ui-designer` est invoqué, la mise à jour de
  ce document fait partie de son livrable, pas une tâche séparée qu'on oublie.
- **Quand** : dans le **même tour** que le changement visuel — pas en fin de cycle. Un
  composant ajouté, renommé ou dont le contexte d'emploi change (§2) sans mise à jour de ce
  document est un défaut au même titre qu'un test qui ne compile plus.
- **Ce qui périme vite et doit donc être vérifié, pas recopié** : les valeurs chiffrées de
  §6 (le calcul dépend de `.main{padding}` / `.card__body{padding}` / police système — si l'un
  de ces trois change, refaire le calcul, ne pas garder l'ancien) ; la liste des contextes
  déclarés en §2 (vérifier dans `data/index.html` que la classe est encore employée là où ce
  document l'affirme).
- **Évolution d'un composant PARTAGÉ** : elle n'est jamais locale. Elle oblige, **ensemble**,
  à (1) **mesurer** l'inventaire exhaustif des emplois — comptes réels, jamais « quelques
  pages » ; (2) dire ce que deviennent les emplois qui ne correspondent plus ; (3) faire
  réaliser **tous** les impacts dans le **même lot**. ⛔ L'application partielle est interdite :
  ⚠️ un design system appliqué à moitié rend l'interface **moins** cohérente qu'avant, car il
  ajoute une troisième forme au lieu d'en retirer une. Si un impact ne peut pas être traité,
  **on l'écrit**.
- **Lacunes connues, à ne pas prétendre résolues** : pas de jeton d'espacement générique
  (§3), pas de `gap` sur `.card--status .card__body` (§5.1), pas de mode sombre (§8). Les
  mentionner comme telles tant qu'elles n'ont pas été traitées — ne jamais les retirer de ce
  document sans avoir réellement livré le correctif.
