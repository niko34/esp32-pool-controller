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
| Choisir un mode (2-4 options) depuis une carte d'accueil **ou** en tête d'une page de réglage pH/ORP/Éclairage | `.mode-select` (§5.2) | Carte d'accueil **et** page de réglage |
| Modifier une valeur (horaire, consigne) dans une carte, **dans le mode qui la rend pertinente** | §5.20 — `.field` (consigne ORP, horaires) **ou** `.segmented--num` **sans cadre** (consigne pH) + `.detail-actions`/`.btn--primary` « Sauvegarder » | Carte d'accueil et page de réglage — pH/ORP (Auto) et Filtration (Programme) par feature-164, Éclairage (Programme) par feature-162 |
| Barre d'onglets (≥5 items), choix ponctuel sans card-header (taille bassin, vitesse de régulation, cadence d'affichage) | `.segmented` (§5.3) | Page de réglage uniquement |
| Saisie numérique par positions (consigne pH) | `.segmented--num` (§5.3) | Page de réglage uniquement |
| Incrémenter/décrémenter une valeur (offset) | `.stepper` | Page de réglage uniquement |
| Déclencher une action immédiate (Allumer, Activer, Arrêter une injection) | `.btn` + variante (§5.4) | Les deux |
| Afficher une option secondaire dans une carte (Boost) | `.card__option` (§5.5) | Carte d'accueil |
| Dire l'état d'une carte d'accueil, d'un coup d'œil (feature-195) | `.dash-pill` (§5.24) | **Carte d'accueil uniquement** |
| Replier les réglages rares d'une carte d'accueil (feature-195) | `.card__fold` + `.card__fold-panel` (§5.24) | **Carte d'accueil uniquement** |
| Déroger au mode d'une carte d'accueil d'un seul geste (Forcer marche / Forcer arrêt, Revenir en Auto) | `.dash-act` — « bouton de dérogation » (§5.24) | **Carte d'accueil uniquement** |
| Afficher une paire libellé/valeur en lecture seule | `.detail-row` (§5.6) | Les deux |
| Indiquer un état ponctuel (En marche / Inactif / Calibration requise) | `.state-badge` (§5.7) | Les deux |
| Visualiser une mesure sur une échelle | `.gauge` (§5.8) | Carte d'accueil |
| Expliquer pourquoi une mesure n'est pas représentative | `.sensor-badge` (§5.9) | Carte d'accueil · en tête de page (`.sensor-badge--page`) |
| Emmener vers la page qui corrige, depuis un `.sensor-badge` | `.sensor-badge__cta` (§5.9bis) | **Carte d'accueil uniquement** — un seul par badge, ⛔ jamais un bouton d'action |
| Expliquer en une phrase un constat déjà affiché dans le même `.sensor-badge` | `.sensor-badge__explain` (§5.9bis) | **Carte d'accueil uniquement** — toujours en 2ᵉ segment, même variante |
| Alerter sur une condition bloquante ou un risque | `.callout` (§5.10) | Les deux |
| Réinstaller le filesystem quand l'interface est absente (page autonome, sans `app.css`) | copies **locales** : callout `--info`, sélecteur `.pick`, ligne de version `.ver`, `<progress>`, callout d'échec rouge (§5.21) | **Page de secours embarquée uniquement** — ⛔ jamais un composant partagé |
| Distinguer deux niveaux de gravité dans un même `.callout`, sans empiler deux encadrés, et séparer l'essentiel du détail | `.callout__eyebrow` + `.callout__lead` + `.callout__more` (§5.10bis) | Page de réglage (pH) — généralisable |
| Commenter un contrôle sans en faire une alerte | `.field__hint` (§5.11) | Les deux, **jamais seul porteur d'une info de sécurité** |
| Dire **pourquoi un bouton est désactivé** (un refus, pas un conseil) | `.field__hint.field__hint--refus` (§5.11bis) | Page de réglage — carte « Correction manuelle de la mesure » (pH) |
| Faire survivre un élément au mode réglage (flux maintenance) | `.flow-maintenance` (§5.25) | Pages pH, ORP, Température — ⚠️ ne veut **pas** dire « visible seulement en réglage » |
| Demander un second geste avant un acte destructif à conséquence chiffrée | bloc `.callout--warn` + `.ph-tune-confirm__actions` (§5.26) | Page de réglage pH |
| Montrer la consommation du jour face à un quota | `.daily-dose` et ses enfants (§5.27) | **Carte héros des pages pH et ORP uniquement** — ⛔ jamais le tableau de bord |
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

### ⚠️ « Donnée absente ⇒ on n'affiche rien » — et l'unique exception, bornée

Règle du projet : **une donnée absente ne s'affiche pas**, ⛔ jamais une valeur par défaut.
Elle vient d'un défaut réel, corrigé en revue de feature-162, qu'on n'avait vu **que sur
capture** parce que le code JS était juste et que c'était sa *source* qui mentait.

**feature-195 introduit une exception, et la voici déclarée avec sa borne.** La pastille
d'éclairage de la carte d'accueil **déduit** « Allumée / Éteinte » du **mode** quand l'état
réel du relais est absent — c'est le cas sur toute la cadence HTTP de 30 s, car
`lighting_enabled` n'existe **que** dans la trame WebSocket.

⛔ **La borne, et elle n'est pas négociable** : le relais ne suit le mode que si la
**fonction éclairage est activée**. Quand `lighting_feature_enabled` est faux,
`src/lighting.cpp` force la broche à LOW et sort **avant** `decideLightingOn()` — « Mode
Allumé » afficherait alors « Allumée » **lampe éteinte**. ⇒ Ce cas est traité **avant** la
déduction, et il est **certain**, pas déduit.
⇒ **Toute autre déduction d'un état à partir d'un mode doit être déclarée ici, avec la
preuve, côté firmware, que le mode entraîne bien l'état.**


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

### Nombres affichés : virgule, quatre interdits (bug-199, v3.0.1)

Même esprit que le Principe directeur (§1) : un geste, une forme. Ici, ⛔ aucun composant n'est créé.

**Règle.** Tout nombre d'un texte affiché porte une **virgule**. Pour les nombres mis en forme, on emploie **l'un des quatre formateurs du projet**, ⛔ jamais un cinquième :

| Besoin | Formateur |
|---|---|
| nombre générique | `fmtFr(value, decimals)` |
| pH mesuré, 1 décimale | `_fmtPh(v, signed)` |
| 2 décimales | `_fmt2(v)` |
| correction pH signée, 2 décimales | `_fmtPhOffset2(v)` |

**Seconde famille, déjà conforme** : `toLocaleString("fr-FR", …)` (une dizaine de sites) sert aux **dates/heures** et aux **grands nombres**. Elle rend déjà la virgule, ⛔ aucun défaut ; mais elle est **dans la garde aussi** : une virgule `fr-FR` dans un `style.width` casse exactement comme `fmtFr`.

⚠️ Motif : feature-195 a dû corriger en revue exactement ce travers, deux expressions pour la même consigne pH, divergentes à 7,35.

**Les quatre interdits, chacun avec sa raison :**
- ⛔ Jamais sur un `.value` d'`<input>` : la saisie et l'enregistrement cassent **en silence**.
- ⛔ Jamais sur un `dataset` : la valeur est **relue par le code**.
- ⛔ Jamais sur une valeur CSS (`bar.style.width = …`) : `width: 72,4%` est **invalide**, le navigateur **ignore la règle** et la barre **reste figée**, sans aucune erreur.
- ⛔ Jamais sur un **attribut numérique HTML** (`step="0.1"`, `min`, `max`, `value` d'un `<input type=number|range>`) : ce sont des valeurs machine, une virgule les casse.

⚠️ Les deux cas cohabitent sur le même écran (`data/index.html`, étalonnage pH) : « solution pH 7,00 » est un **texte affiché**, converti ; `step="0.1"` à côté est un **attribut machine**, ⛔ intouchable.

**Deux gabarits de pH, voulus.** 1 décimale pour la **mesure** (lue d'un coup d'œil), 2 pour les **repères** et la **correction** (seuils chimiques, lus comme des références). ⛔ Ne pas les fusionner.

**Laissé en point à dessein** : le journal de développement (`debugLog`).

**Textes rédigés par le firmware : le serveur émet le canonique (point), la présentation localise.** Choix de l'utilisateur : garder le format anglophone côté serveur rend une traduction possible, alors que corriger dans `src/` figerait une convention française à la source. Le firmware rédige **six messages de blocage en clair** que l'interface cite verbatim (leurs valeurs ne sont pas énumérables). La conversion se fait par **six motifs ancrés** (`_FIRMWARE_TEXT_MOTIFS`, `data/app.js`) : chacun fixe le libellé réel et ne capture que la valeur. ⛔ **Tout texte non reconnu sort inchangé** : on ne convertit que là où l'on sait ce qu'on convertit.

🔴 Garde-fou obligatoire d'une approche ancrée : `tools/check_dose_guard_text_parity.sh`, câblée en CI. Elle lit les appels réels de `src/pump_controller.cpp`, reconstitue chaque message et le passe dans les motifs réels sous Node. Une reformulation du firmware **fait rougir la CI** au lieu d'arrêter la conversion en silence. Ses limites :
1. Elle **clampe à 95 octets** comme l'appareil (`kDoseBlockedReasonMaxLen`), sinon elle validerait un texte que l'écran ne reçoit pas. Le message à deux valeurs n'a que 4 octets de marge, sur un texte déjà tronqué en production une fois.
2. Elle garantit la parité **dans le dépôt, pas sur l'appareil** : firmware et système de fichiers se déploient séparément, envoyer l'un sans l'autre peut refaire apparaître un point avec une CI verte.
3. **Hors périmètre** : le bandeau de bascule manuelle pH (« pente acide 98.5 %, zéro 4.12 mV ») n'est pas traité. La décision borne la règle à pH, ORP, température, et ce sont des diagnostics de sonde.

**Décimales de l'ORP : zéro.** Les statistiques ORP de la carte Signaux sont à **0 décimale** (4 sites ensemble), comme la valeur affichée. Motif : l'ORP est une tension en mV qui bouge par dizaines, une décimale (« moy 555,8 ») affichait une précision que la mesure n'a pas. Le pH reste à 2 décimales sur ses quatre séries, cohérent avec `_fmtFilterVal`.

**Séparateur de liste.** Quand un gabarit juxtapose des nombres, c'est `·`, ⛔ jamais la virgule : c'est lui qui rend « min 1,2 · max 3,4 · moy 2,3 » non ambigu.

**Règle opposable** : `tools/check_decimal_format_guard.sh`, câblée en CI (job `decimal-format-guard`). Deux volets : `data/*.js` (formateurs et `toLocaleString` sur `.value`, `dataset`, CSS) et `data/*.html` (virgule dans les attributs numériques). ⚠️ Limite assumée : la garde est **syntaxique**, elle ne voit **pas** un formateur passé par une variable intermédiaire (`const t = fmtFr(v,1); el.value = t;`).

## 5. Catalogue des composants

### 5.1 Carte de statut — `.card.card--status`
**Rôle** : une tuile du tableau de bord (Filtration, Éclairage, pH, ORP, Température).
**Contexte** : carte d'accueil uniquement (la page de réglage utilise `.card--detail`,
colonne unique, pas d'en-tête icône+badge).
**Structure** : ⚠️ **refondue par feature-195 (v2.96.0)** — voir §5.24. `.dash-head` (icône, titre en
casse normale, **une seule** pastille `.dash-pill`) → `.card__body` (mesure `.dash-hero`, jauge,
bandeaux d'exception, volet `.card__fold`, lien « Réglages › »). Les anciens `.card__status-header`,
`.card__status-label` et la roue dentée de 34 px ne servent plus sur l'accueil.
⚠️ `.card--status .card__body` n'a **pas de `gap`** — c'est une dette (§10). En attendant,
tout nouveau bloc ajoute sa propre marge, toujours une valeur du jeton d'espacement (§3),
**jamais 0** sauf si le bloc précédent porte déjà la marge.

### 5.2 Sélecteur de mode — `.mode-select`
**Rôle** : choisir UN mode parmi 2 à 4, avec effet immédiat (pas de validation séparée).
**Contexte déclaré** : carte d'accueil (Filtration, pH, ORP, Éclairage) **et** page de réglage
quand le même choix y est dupliqué (`#ph_regulation_mode`, `#orp_regulation_mode`,
`#dashboard-lighting-mode` et la page `/lighting` — feature-162).
**Emplois mesurés au 2026-10-14** : **4** avant feature-162 (`dashboard-ph-mode`,
`dashboard-orp-mode`, `ph_regulation_mode`, `orp_regulation_mode`), **6** après.
⚠️ La carte **Filtration n'emploie pas** `.mode-select` : 4 options en grille 2×2 dédiée
(`.filtration-modes`), parce que 4 colonnes ne tiennent pas à 390 px (calcul §6).
⚠️ **Deux apparences selon le contexte (feature-195, v2.96.0)** : sur la carte d'accueil
(`.card--status .mode-select`), le segment sélectionné est à l'**encre** (fond `--text`, texte blanc),
boutons de 44 px ; sur les pages de réglage, il reste **bleu** (`btn--primary`). C'est un
**contexte d'emploi déclaré**, pas une troisième forme. ⚠️ Les pages de réglage s'aligneront dans un
lot ultérieur **si la direction est validée** — d'ici là, l'écart est connu et voulu.
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
⚠️ **Extension de contexte, feature-164** : `.segmented--num` est repris pour la **consigne pH sur
la carte d'accueil**, avec **exactement la raison** qui a fait retirer le cadre de `.segmented` pour
`.mode-select` (§5.2) : une carte a **déjà un en-tête**, le cadre y créerait une boîte dans la boîte.
⇒ Sur la carte, le balisage porte **`class="segmented--num"` SEUL** — ⛔ pas `"segmented
segmented--num"`. Les `.segmented__btn` sont inchangés.
⚠️ **Conséquence CSS** : `.segmented--num` doit porter son **propre** `gap: 4px`, jusqu'ici **hérité
du parent `.segmented`** et donc absent sans lui. Ajout **neutre** pour la page de réglage (même
valeur).
⛔ **Ne pas étendre ce retrait de cadre à un AUTRE usage** de `.segmented--num` sans revérifier
qu'aucune page de réglage n'en dépendait comme repère visuel (ici : vérifié, seul `#ph_target_group`
existe).

⛔ **Ne plus ajouter** de nouvel usage de `.segmented` nu à 2-4 options en tête d'une page
dont la section répète un sélecteur déjà présent sur la carte d'accueil correspondante —
utiliser `.mode-select` (§5.2) aux deux endroits pour que l'utilisateur retrouve le même
repère visuel.

### 5.4 `.btn` et variantes
`.btn` (neutre bleuté), `.btn--ghost` (gris, état « non sélectionné » d'un `.mode-select`
ou action secondaire), `.btn--primary` (accent plein, action principale **ou** option
sélectionnée d'un `.mode-select`), `.btn--ok` (vert, confirmation), `.btn--danger` (rouge,
action destructive — ne jamais le coller à l'action principale). Toutes ≥44px de haut.

⚠️ **Règle sur `.btn--ok` (feature-151 D1-bis, 2026-10-21)** : il est réservé à un **flash
transitoire**, posé **après** que la page a reçu la confirmation du succès de **sa propre**
requête (motif des boutons « Sauvegarder » : `classList.add('btn--ok')` puis retour à
`.btn--primary`). ⛔ **Jamais une classe statique dans le HTML** sur un bouton qui déclenche
une action dont la vérification est **déportée** (l'appareil, un service tiers) — le vert
affirmerait « tout est vérifié » alors que la page, elle, n'a vérifié rien de tel.
**Inventaire mesuré** : **9 emplois dynamiques conformes** (boutons « Sauvegarder »), **0 emploi
statique légitime**. Le seul qui existait (`#install_update_btn`, carte « Mise à jour
manuelle ») était une **violation** de cette règle — il est corrigé dans le même lot (le bouton
devient neutre `.btn`, voir [`docs/features/page-settings.md`](features/page-settings.md) et
[`docs/UPDATE_GUIDE.md`](UPDATE_GUIDE.md)) : la carte ne fait que **transmettre** un fichier et
**comparer** une empreinte, jamais vérifier une signature — l'authenticité reste du ressort de
l'appareil.

### 5.5 `.card__option`
**Rôle** : option mineure secondaire à l'intérieur d'une carte de statut (ex. Boost).
**Contexte** : carte d'accueil uniquement. Fond `--panel2`, liseré `--stroke`, titre 14px
sans majuscules forcées (contraste voulu avec `.card__status-label`, qui lui capitalise).
Un seul gabarit « option » pour tout le projet — ne pas en créer un second par carte.
⚠️ Depuis feature-195, sur l'accueil (`.card--status .card__option`) : sans surlignage en capitales ;
le bouton « Activer » du Boost est **neutre** (au plus une action bleue par carte, §5.24). Dans le
volet « Modifier » de la Filtration.

### 5.6 `.detail-row`
Paire libellé/valeur en lecture seule, `justify-content:space-between`. Piloté par
`[hidden]` (pas `style.display`) : `.detail-row[hidden]{display:none!important}` existe déjà
et l'emporte sur `.detail-row{display:flex}` — toujours utiliser l'attribut `hidden`, jamais
un style inline, pour toute nouvelle ligne conditionnelle.

### 5.7 `.state-badge`
Pastille d'état : `--ok` (vert, fonctionnement normal), `--off` (gris neutre, à l'arrêt),
`--active` (bleu accent, option activée), `--warn` (ambre, attention sans danger immédiat).
**Un badge d'état AU MAXIMUM par en-tête de carte** — ⚠️ « au maximum », pas « un » : un
en-tête peut n'en afficher **aucun** quand le mode actif décrit déjà entièrement l'état (carte
Éclairage en Allumé ou Éteint). Le badge ne réapparaît que lorsque **l'état réel peut différer du
mode demandé** (Éclairage en Programme, Filtration dans tous ses modes).
⚠️ **Sur la carte d'accueil, `.state-badge` n'est plus employé pour l'état de la carte** : c'est la
pastille `.dash-pill` (§5.24) qui le dit, dans une grammaire unique. `.state-badge` garde son rôle
ailleurs, et son apparence est **scopée** (`.card--status`) pour les emplois restants — même
contexte d'emploi à deux apparences que `.mode-select`.
⛔ Ne pas empiler deux pastilles qui décrivent la même chose sous deux angles (motif déjà choisi pour Filtration : `état réel` dans le
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
**Contextes d'emploi déclarés (deux)** :
1. **Carte d'accueil**, juste sous la jauge (ou, sur la carte Filtration, entre `#detail-filtration-mode` et les boutons : `#filtration-frost-badge`). `margin-top:8px`, **aucun `margin-bottom`** — tout bloc placé juste après doit apporter sa propre marge haute.
2. **En tête de page** (bug-197) : `#filtration-page-frost-badge`, tout en haut de `#filtration-content`, avec le modificateur **`.sensor-badge--page`** (`margin-top:0`, `margin-bottom:12px`).

🔴 **Enseignement de ce réemploi** : les fonds de `.sensor-badge--*` sont **translucides**, prévus pour une carte **blanche**. En tête de page ils se composent sur `--bg` (bleu-gris) et **l'ambre vire à l'olive** — mesuré : fond `(206,208,200)`, contraste 5,82:1, au lieu de `(255,247,225)` et 8,49:1 en carte. Pas un défaut d'accessibilité (AA passé) mais une **incohérence** : le même composant ne rendait pas la même couleur. ⇒ Règles **opaques** `.sensor-badge--page.sensor-badge--{warning,danger,info}` (spécificité 0,2,0), identiques au rendu en carte. ⚠️ Tout nouvel emploi sur un fond non blanc doit vérifier ce point.

**Emploi « froid » (bug-197)** : variante `--warning`, texte sur deux lignes, nœud permanent `role="status" aria-live="polite"`, masqué par `display:none`. Le verdict est porté par le texte, jamais par la couleur seule.

#### 5.9bis `.sensor-badge__cta` et `.sensor-badge__explain` (bug-211, v3.0.3)

Deux sous-éléments de `.sensor-badge`, **indépendants l'un de l'autre**, nés du compteur de bidon à zéro. ⚠️ La variante `.sensor-badge--info` **préexiste** (feature-061) et ne change pas : seuls ces deux sous-éléments sont nouveaux.

- **`.sensor-badge__cta` — le segment qui emmène ailleurs.** **Contexte d'emploi déclaré : carte d'accueil uniquement, un seul segment par badge, et seulement quand l'état n'est pas réparable sur place.** C'est le **seul** segment interactif qu'un `.sensor-badge` ait le droit de porter : `setBadge()` rend un `<a>` au lieu d'un `<span>` dès qu'un segment porte un `href`, et remplace le nœud si la balise ne correspond pas. Cible tactile **≥ 44 px** (`min-height:44px`, `width:100%` — ⚠️ un lien inline dans une bande de 2 lignes ne l'atteint pas), `color:inherit` (la couleur vient de la variante, ⛔ jamais du bleu de lien), `text-decoration:none`, `font-weight:600`, et un `:focus-visible` en `outline: 2px solid currentColor`. Le chevron final « › » dit la navigation **sans** couleur ni soulignement.
  ⛔ **Ne pas en faire un bouton d'action.** Un `.sensor-badge` **explique**, il n'agit pas : le lien emmène vers la page qui corrige, il ne corrige pas lui-même. Un geste qui modifie l'état reste un `.btn`, dans la carte ou la page de réglage.
- **`.sensor-badge__explain` — la phrase qui suit le constat.** **Contexte d'emploi déclaré : carte d'accueil, toujours en second segment, toujours dans la même variante que le constat qu'elle explique.** `font-weight:400` (le constat garde la graisse, l'explication la perd) et `line-height:1.4`. Elle existe parce que le badge est devenu **empilé** (`.sensor-badge--stacked` au-delà d'un segment) et que deux phrases de même graisse dans la même couleur se lisent comme une seule.
  ⚠️ **Mesurer la hauteur avant d'ajouter une phrase.** La rédaction écartée (« constat + ajout ») occupait **six lignes de rouge à 390 px** : à cette densité, un `.sensor-badge` cesse d'informer et devient un mur. ⇒ Deux textes entiers choisis par l'état, jamais une phrase concaténée à une autre.

Aucune animation sur l'un ni sur l'autre. Aucun emploi existant de `.sensor-badge` ne change.

### 5.10 `.callout` et variantes (`--info`, `--success`, `--warn`)
Alerte ou information qui mérite un encadré visible (fond teinté, liseré). Réservé aux
messages qui **engagent une action ou une sécurité** (gates `pool-chemistry`, bidon bas,
calibration en cours). Ne pas en faire un habillage décoratif pour un texte purement
informatif sans enjeu — c'est à ça que sert `.field__hint` ou `.detail-row`.

**Emploi ajouté (feature-151 D3-c) : refus de l'appareil dans une carte de réglage** — `#update_refusal` (carte « Mise à jour manuelle ») et `#update_sig_note` en blocage (carte en ligne) : `callout callout--warn` + `role="alert"`, texte inséré par `textContent` (la phrase de l'appareil, jamais du HTML). Contexte : page de réglage, sous le champ concerné. Le même `#update_sig_note` reste un simple `.field__hint` (`role="status"`) tant que rien ne bloque. Aucune nouvelle classe.

**Emploi ajouté (bug-171) : refus ou échec d'un test de pompe** — `#pump_test_msg` (carte « Test manuel des pompes », onglet Avancé) : `callout callout--warn` + `role="alert"`, persistant (effacé au prochain appui ou en fin de décompte, pas de toast), texte par `textContent`. Contexte : page de réglage. La grille `.pump-test` (2 colonnes + « Arrêter » pleine largeur, `.btn--danger`) est locale à la carte ; aucun composant partagé modifié.

### 5.10bis `.callout__eyebrow` / `.callout__lead` / `.callout__more` (v2.82.0, feature-168)

**Contexte d'emploi déclaré : page de réglage pH (`#ph_tune_blocked`), généralisable à tout `.callout` qui doit distinguer deux niveaux de gravité sur le MÊME propos sans empiler deux encadrés.** Né de l'avertissement non bloquant de correction de mesure pH (dépassement de l'ancienne borne ±0,50) : au-delà d'un second seuil (« impasse »), le même encadré doit se faire plus pressant **sans changer de couleur de boîte** (⛔ pas de 3ᵉ couleur — daltonisme, §7) et sans forcer la lecture d'un pavé de texte entier à chaque ouverture.

- **`.callout__eyebrow`** — chapeau de 2-3 mots (reprend la typo de `.product-card__eyebrow` : petite majuscule, 12px, 700), **toujours accompagné du texte** (« Avertissement » / « ⚠ Impasse possible ») : c'est le **mot** qui porte le sens, la variante `--danger` (couleur) ne fait que l'**appuyer**. La boîte `.callout` elle-même reste dans sa variante d'origine (`--warn`) dans les deux cas.
- **`.callout__lead`** — première phrase, **toujours visible**, ~160 caractères : le constat minimal suffisant pour agir.
- **`.callout__more`** (`<details>`) — reste du texte, **replié par défaut**, idiome `.tips > summary` (curseur, graisse, `list-style`) + `.linklike` (couleur accent, soulignement, hover) + cible tactile de 10px de padding vertical (`.tips > summary` seul ne la garantit pas — ne pas reconduire ce manque) + `:focus-visible`.

Les trois sous-éléments sont **optionnels indépendamment** : un refus simplement bloquant n'utilise que `.callout__lead` (chapeau et détail masqués) ; l'avertissement non bloquant utilise les trois. Voir [`docs/features/page-ph.md`](features/page-ph.md#avertissement-et-impasse-de-régulation-v2820) pour l'emploi complet et les textes exacts.

**Second contexte d'emploi (feature-170 (C31), v2.99.0) : bandeau d'acquittement** — `#ph_tune_stale_banner` (carte « Correction manuelle de la mesure », page pH) : `.callout__eyebrow` + `--danger` sur un bandeau où l'utilisateur doit **trancher** (« Je l'ai vérifiée — garder » ; ⚠️ le second acte « Remettre à zéro » a été **supprimé du bandeau en v3.0.0**, le seul chemin destructif étant « Supprimer la correction » en bas de carte), et non plus seulement le couple avertissement/impasse de `#ph_tune_blocked`. Aucun composant partagé modifié, aucun emploi existant ne change.

Ce qui distingue les deux emplois :

| | `#ph_tune_blocked` | `#ph_tune_stale_banner` |
|---|---|---|
| Le chapeau nomme | un **état** (« Avertissement », « ⚠ Impasse possible »), bascule mot + couleur | un **état établi** (« Étalonnage modifié ») |
| `.callout__more` repliable | oui | **non** (rien à replier : replier la raison ou l'acte, c'est les supprimer pour qui ne lit pas) |
| Chapeau masqué | jamais | **oui, quand l'événement n'est pas établi** (cause héritée `boot`/`expiration` d'un firmware antérieur) |

⭐ **Règle générale** : *un chapeau qui nomme un ÉVÉNEMENT ne s'affiche que lorsque cet événement est établi — sinon il affirme un fait que l'appareil ne connaît pas.* Un chapeau qui nomme un ÉTAT n'a pas cette contrainte.

⛔ **Aucune couleur nouvelle** : la boîte reste `callout--warn`, le rouge vient de `--danger` (`data/app.css`, déjà employé par « ⚠ Impasse possible »). Contraste mesuré **6,14:1** sur le fond ambre composé (seuil 4,5:1) : `--danger` conservé. ⚠️ **Limite à réévaluer** : le mode sombre n'existe pas dans `app.css` (aucun `prefers-color-scheme`, `data-theme` ni `.dark`, vérifié) — rien à mesurer dans ce mode aujourd'hui ; à mesurer le jour où il existera. Le mot porte le sens, la couleur ne fait que l'appuyer (daltonisme). Voir [`docs/features/page-ph.md`](features/page-ph.md#correction-devenue-caduque-ph_tune_stale_banner).


### 5.10bis `.tips__code--wrap` — bloc de commande qui se replie (bug-184, v2.92.0)

**Contexte d'emploi déclaré : les blocs `<pre class="tips__code">` d'un `.tips` qui contiennent une COMMANDE (shell, `curl`, `mosquitto_pub`). ⛔ PAS un bloc dont l'indentation porte du sens — YAML, JSON, arborescence.**

`.tips__code` est en `white-space: pre` + `overflow-x: auto` : à 390 px, **environ 34 à 36 caractères** sont visibles, le reste se lit par défilement latéral. ⚠️ Mesuré sur capture le 2026-11-17 : c'est l'**URL** qui disparaissait — c'est-à-dire l'élément dont l'utilisateur a le plus besoin (`http://poolcontroller.local/filtration/external-state`, 53 caractères). Le défilement latéral n'est pas un repli : c'est une information cachée.

Le modificateur passe le bloc en `white-space: pre-wrap` + `overflow-wrap: anywhere`. ⭐ **Le repli est VISUEL : aucun saut de ligne n'est inséré dans le texte, donc le copier-coller reste exact.**

Deux règles d'écriture qui accompagnent le modificateur :
1. **Une option par ligne** (`\` en fin de ligne), l'**URL en dernier** — c'est la seule qui peut dépasser, et elle se replie alors proprement.
2. **Les commentaires sortent des lignes de commande** : un commentaire en fin de ligne allonge la ligne et c'est lui qu'on perd.

⛔ **Ne pas l'appliquer** au bloc YAML Home Assistant du même `.tips` : son indentation est porteuse, un repli la rendrait fausse à l'œil. C'est le seul endroit où le défilement latéral reste assumé.

### 5.11 `.field__hint`
Texte d'aide discret (12px, `--muted`, pas de fond) attaché à **un contrôle précis** juste
au-dessus ou en dessous de lui. ⚠️ **Ne doit jamais être le seul support d'une information de
sécurité ou de conséquence directe d'un mode** (ex. « la régulation automatique ne dose
plus ») : dans ce cas, la positionner **immédiatement adjacente** au badge/bandeau qui
annonce la cause (`.sensor-badge`, `.regul-alert`), pas après le contrôle qui a produit l'état.
Ce point est le cœur du défaut « message presque invisible » du 14/10.

**Contexte d'emploi ajouté (bug-176)** : *pied de carte de réglage, près d'un contrôle* —
`#update_sig_note` (carte « Mise à jour en ligne », avant les boutons Vérifier / Mettre à jour).
Piloté par l'attribut `hidden`, `role="status"`, texte seulement pour l'état anormal ; le code
brut du firmware va dans `title`. Aucune évolution du composant : CSS inchangé, donc aucun
impact sur les 20 emplois de `index.html` ni sur l'emploi de `app.js`.

**Contexte d'emploi ajouté (bug-178)** : *complément d'un constat, non d'un contrôle* —
`<span class="field__hint">` dans `#no_update_msg` (même carte), après la ligne en gras
(`ℹ` / `⚠` en glyphes texte, pas emoji). Construit en DOM (`textContent`). Aucune évolution du
composant : CSS inchangé, aucun inventaire d'emplois dû.

### 5.11bis `.field__hint--refus` un refus adjacent à ce qu'il désactive (bug-201, v3.0.0)

**Contexte d'emploi déclaré : page de réglage pH, carte « Correction manuelle de la mesure » — `#ph_tune_refus`, juste sous le bouton « Corriger » (`aria-describedby` depuis ce bouton).** Modificateur de `.field__hint` (§5.11), ajouté à `data/app.css` : `font-size: 14px`, `color: var(--text)`, `margin-top: 8px`. Aucun cadre, aucune teinte d'alerte : l'ambre reste réservé à l'avertissement `feature-168 (C8)` de la même carte (`#ph_tune_blocked`, `.callout--warn`).

⛔ **Ce n'est pas un `.callout`** (un refus n'est pas une alerte de régulation) **mais ce n'est pas non plus `.muted` / `.field__hint` seul** : *ce sont des REFUS, pas des conseils*. Un refus doit rester **au moins aussi saillant que le bouton qu'il désactive** — un bouton grisé sans explication lisible est précisément le défaut que ces textes existent pour éviter. D'où le texte plein et 14 px au lieu de 12 px `--muted`.

⚠️ **Règle de conception associée** : les refus et l'avertissement C8 **ne partagent plus d'élément** (deux rendus distincts). Dégrader l'un dégraderait l'autre en silence. Écriture a11y : texte et `hidden` ne sont écrits **que s'ils changent**.

Aucun impact sur les emplois existants de `.field__hint` : la règle ne s'applique qu'avec le modificateur. Voir [`docs/features/page-ph.md`](features/page-ph.md#les-refus-bloquants-de-laccord).

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

⚠️ **MANQUE constaté (feature-151 D1-bis, 2026-10-21)** : `input[type="file"]` dans `.field`
n'a **aucune règle de largeur propre** — il hérite de la largeur naturelle du contrôle natif du
navigateur, qui ne respecte pas la grille de la carte. Risque de **débordement de grille** avec
un nom de fichier long et insécable (ex. `v2.86.0.firmware.bin.sig`). **Contexte d'emploi mesuré :
3 occurrences, toutes dans `data/index.html`** — `#update_file` et `#update_sig` (onglet
Système, carte « Mise à jour manuelle »), `#history_import_file` (onglet Avancé) ; ⛔ **aucune**
dans `wizard.html` / `wifi.html` / `login.html`. À corriger la prochaine fois qu'un de ces trois
champs est retouché visuellement — pas un oubli de ce lot, un manque déjà présent qu'il expose.
⛔ **Règle du motif `.pick` + `.vh`** : il **exige** que toute règle globale sur `input` EXCLUE `.vh` (`app.css` : `input:where(:not(.vh))`, `:where` pour ne pas relever la spécificité ; page de secours : `input:not(.vh)`). Sans cela le champ masqué hérite de `padding`/`border`/`height` et occupe ~32×26 px sur le début du libellé (défaut vu en capture le 2026-11-16, aucune garde ne le détectait).
✅ **Résolu (feature-151 D3-c, C6)** : les trois champs emploient désormais `.pick` (`<input class="vh">` + `<label class="pick">`, `app.css` ~l.1195, libellé « Appuyer pour choisir un fichier », nom en `textContent`, synchronisé par `bindFilePicks()`/`syncFilePick()`). **Contexte d'emploi : page de réglage ET page de secours** ; la règle de largeur `.field > input[type="file"]:not(.vh)` ne vise plus que d'éventuels champs natifs (0 aujourd'hui). Le refus d'une installation manuelle s'affiche dans `#update_refusal` (`callout--warn`, `role="alert"`, `textContent`).

#### ⚠️ Une UNITÉ ne se met JAMAIS en majuscules
`.field__label` applique `text-transform: uppercase`. ⇒ « (mV) » s'affichait « **(MV)** », c'est-à-dire
**mégavolt**. ⛔ Même piège pour `mL`, `°C`, `mg/L`.
⇒ Toute unité dans un libellé va dans `<span class="unit">`, qui porte `text-transform: none`.

⚠️ **Défaut repéré sur CAPTURE, invisible à la relecture** : le HTML contenait bien `mV`. Seul le
rendu montrait la faute. C'est l'illustration la plus courte de pourquoi §9 existe.

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
**Contexte déclaré, vérifié** : **trois contextes depuis bug-187 (v2.95.0)** — (1) page de réglage
Sécurité (`#passwordChecklist`, seuil 8) ; (2) assistant de première configuration (`wizard.html`,
seuil 8) ; (3) carte « Point d'accès de secours » de l'onglet Wi-Fi (`#apPwdChecklist`, seuil **12**).
Six critères identiques partout : longueur, minuscule, majuscule, chiffre, spécial, ASCII imprimable.
La liste est **visible dès l'ouverture** (la règle précède la frappe) et la même fonction JS
(`pwdRules` / `pwdRenderChecklist`) la pilote. ⛔ **Même duplication que §5.18** dans
`wizard.html` (le CSS y est recopié, pas partagé).
**États** : masqué avant saisie · `.valid` colore le texte et remplit l'icône (`✓` injecté en CSS).
⚠️ **MANQUES constatés** : **aucun `aria-live`** — un critère qui devient satisfait pendant la
frappe n'est annoncé à aucun lecteur d'écran ; et l'icône emploie une bordure **codée en dur**
alors qu'un jeton existe (§3).

### 5.20 Modifier une valeur dans une carte — une COMPOSITION, pas une classe nouvelle
**Rôle** : ajuster une valeur qui commande le mode actif (horaire, consigne) **en restant sur la
carte**, sans aller dans une page de réglage.
**Contexte déclaré** : carte Éclairage en mode Programme (feature-162, horaires). Prévu pour
pH/ORP (consignes) et Filtration (horaires) par feature-164.
**Composition** : **trois formes, choisies par la NATURE de la valeur** — ⛔ pas par préférence.
- **Intervalle horaire** : deux `.field` (§5.13) côte à côte en `.grid.grid--2.schedule-fields`
  (Filtration, Éclairage).
- **Consigne à plage continue** (ORP) : **un seul** `.field` + `<input type="number">`.
- **Consigne à positions discrètes déjà validées par le serveur** (pH) : `.segmented--num` **sans**
  la classe `.segmented` (§5.3) — **le même geste que la page de réglage**, cadre retiré pour la
  raison de `.mode-select`.
  ⛔ **Ne pas remettre un `<input>` libre pour le pH** : la page de réglage n'en a plus depuis une
  décision produit d'août, et un geste différent entre la carte et la page **romprait §5.1**.

Dans les trois cas : un `.detail-actions` avec **un seul** `.btn--primary` « Sauvegarder ».

### ⚠️ Plage visible : cela dépend de la NATURE du contrôle, pas d'une parité mécanique
- **Consigne à positions discrètes** (pH, `.segmented--num`) : la plage est **montrée par
  construction**. ⛔ **Aucun texte de plage n'est dû** sur la carte — ce serait répéter du visible.
  ⚠️ Le texte de la **page** pH reste légitime, mais **pour une autre raison** : il porte la règle
  *« les seuils de sécurité portent sur le pH affiché, offset compris »*, **absente** des segments.
  ⛔ Ne pas le confondre avec un rappel de plage, ni le porter sur la carte pour ce motif.
- **Consigne à champ libre** (ORP) : elle n'a **aucun** moyen d'afficher sa plage. ⇒ **`.field__hint`
  OBLIGATOIRE**, sur **la carte ET la page** : « **Plage autorisée : {min} à {max} mV.** »
  ⛔ Pas de mention de pas (il vaut 1 mV, donc non contraignant — contrairement au pH).
  ⚠️ Valeurs **lues** sur `orp_target_min` / `orp_target_max`, ⛔ **jamais en dur** (ADR-0100). Hint
  **masqué** tant que la configuration n'est pas chargée — ⛔ ne jamais afficher une plage devinée.

⚠️ **La règle générale qui s'en dégage** : *contrôle discret ⇒ plage visible ⇒ pas de texte ;
contrôle libre ⇒ plage invisible ⇒ texte requis.* La parité pH/ORP porte sur la **PRÉSENCE de
l'information** quelque part dans l'interface, ⛔ **pas** sur la forme du composant qui la porte.

### ⚠️ AMENDEMENT feature-195 (v2.96.0) — la consigne reste affichée en lecture seule, en mode Auto seulement
La règle ci-dessous (« toute mention statique disparaît quand la valeur devient éditable ») est
**amendée** : l'édition passe désormais derrière un volet « Modifier » **replié** (§5.24), donc la
valeur n'est plus visible *dans* la zone d'édition. La **consigne pH / ORP** s'affiche en **lecture
seule à droite de la mesure** (`#dash-ph-target-ro`, `#dash-orp-target-ro`), **en mode Auto
seulement** — même règle de visibilité que la jauge : en Programme ou en Arrêt rien n'est poursuivi,
donc la carte **n'affirme aucune cible**. Même source (`window._config`), un seul formateur partagé.
⇒ La règle « l'information vit à UN SEUL endroit » reste vraie : la valeur éditable est dans le volet,
la valeur lue est à côté de la mesure — jamais deux lignes de lecture.

### ⚠️ AMENDEMENT feature-195 — Filtration : l'horaire devient un titre, pas une ligne de formulaire
Sur la face de la carte Filtration, l'horaire (Auto, Programme) est la **ligne héros** (32 px / 800,
`#detail-filtration-schedule`) — il n'y a **plus de** ligne `.detail-row` étiquetée « Horaires ». En mode forcé, la
ligne héros dit « Marche forcée » / « Arrêt forcé » avec « Horaires suspendus ». L'**édition** de
l'intervalle horaire (deux `.field`) est dans le volet « Modifier », en mode Programme.

### ⚠️ RETRAIT ASSOCIÉ — la règle qui empêche l'empilement
Quand une valeur devient **éditable** dans le corps d'une carte, **toute mention STATIQUE de cette
même valeur ailleurs dans la carte DISPARAÎT** — retirée du DOM, ⛔ pas seulement masquée.
⇒ feature-164 retire `#ph-compact-target-row` et `#orp-compact-target-row` (« Cible : X »).
⚠️ **L'information vit à UN SEUL endroit.** C'est ce retrait qui compense le 7ᵉ ajout sur des cartes
qui en portaient déjà six — sans lui, l'écran se dégrade comme la page Produits.

### ⚠️ Verrou pendant étalonnage : condition DIFFÉRENTE de celle du mode
Le bloc consigne se désactive sur **`calActive` SEUL** — ⛔ **PAS** `calActive || escalationActive`
comme le sélecteur de mode juste au-dessus. Motif du gate : une escalade de sécurité pH force déjà le
mode en Arrêt (dosage stoppé), donc verrouiller **aussi** la consigne n'y protège rien.
⚠️ **Deux variables de verrouillage distinctes dans la même carte** — ⛔ ne pas les fusionner par
souci de symétrie.

### ⚠️ Correctif visuel du verrou — « techniquement désactivé » ne suffit pas
Réserve remontée **sur capture réelle** : `opacity:.55` sur une position déjà pleine (`--accent`) est
**quasi invisible** à l'œil. ⚠️ §4 exige un signal **perceptible**, ⛔ pas seulement un `disabled`
correct dans le DOM — un utilisateur cliquerait sans comprendre pourquoi rien ne se passe.
⇒ **Réutiliser le code couleur déjà établi** pour `.mode-select` : une position active **désactivée**
passe en `--warn`.

```css
.segmented--num .segmented__btn:disabled.is-active {
  background: var(--warn); border-color: var(--warn); color: #fff; opacity: 1;
}
```
⚠️ **Portée MESURÉE avant d'écrire la règle** : `.segmented--num` n'a que **deux** emplois dans tout
le projet — `#ph_target_group` (page de réglage, **jamais désactivé**, vérifié) et
`#dashboard-ph-target-group` (carte). ⇒ **Rien ne change sur la page de réglage.**

⛔ **NE PAS recolorer `.btn:disabled` / `.btn--primary:disabled`** pour le bouton Sauvegarder : règle
partagée par **des dizaines** de boutons sans rapport (calibration, tous les « Sauvegarder » de
réglage). La recolorer les repeindrait **tous, en silence**. ⇒ **Surcharge par ID**, limitée aux deux
boutons concernés :
```css
#dashboard-ph-target-save:disabled,
#dashboard-orp-target-save:disabled { background: var(--warn); border-color: var(--warn); color: #fff; opacity: 1; }
```

### ⚠️ Adjacence du texte explicatif — le bandeau du haut ne compte PAS
Le bandeau d'étalonnage (`.regul-alert`) est **juxtaposé au sélecteur de mode**, ⛔ **pas** au bloc
consigne : le sélecteur entier s'intercale entre les deux. §5.11 exige l'adjacence **au contrôle**,
⛔ pas « ailleurs dans la carte ».
⇒ Ajouter un `.field__hint` **court et autonome**, **dans** le bloc consigne — ⛔ pas un second
`.regul-alert`, qui serait un **ajout de composant** au lieu d'une mise en conformité :
```html
<p class="field__hint" id="dashboard-ph-target-lock-hint" hidden>Modification désactivée pendant l'étalonnage.</p>
```
Miroir ORP, même texte. Visibilité pilotée par **la même condition que le verrou** (`calActive`), et
l'identifiant **ajouté à `aria-describedby`** du contrôle — ⚠️ pour que le lecteur d'écran porte la
**même** explication que l'œil (§7 : jamais la couleur seule).

### Confirmation à la baisse pendant une chloration de secours (ORP seulement)
Avant d'envoyer une consigne ORP **inférieure** à la courante alors qu'un secours est actif : passer
par le **même** mécanisme que `confirmOrpRescueInterruption`. ⚠️ Critère = **baisse de consigne**,
⛔ jamais comparaison à la mesure. Pas de confirmation à la hausse, ni hors secours.

### ⚠️ DÉRIVE LIVRÉE À CORRIGER (feature-162)
`bindLightingScheduleSave` pose son texte d'erreur dans un élément `.field__hint` (**gris**) au lieu
de `.field__error` (**rouge**) prescrit ici. ⇒ La généralisation de cette fonction **corrige la
classe pour les DEUX cartes existantes**, ⛔ **ne pas committer une 3ᵉ instance qui reproduit la
dérive** (gouvernance §11 : tous les impacts dans le même lot).

### Survol imbriqué
Un `.callout` placé **dans** une `.card--status` hérite de `.callout:hover{transform:translateY(-1px)}`
et **sautille** en plus du `.card--status:hover{transform:translateY(-2px)}` englobant.
⇒ Neutraliser par `.card--status .callout:hover { transform: none; }` — ⚠️ **correctif déjà appliqué
à `.product-card .callout`**, pas un composant neuf.

⚠️ **PIÈGE MESURÉ le 2026-10-14, sur capture réelle.** Sans `.schedule-fields`, la règle générique
`@media (max-width: 640px) { .grid--2 { grid-template-columns: 1fr } }` (`app.css`) **empile** les
deux champs et ajoute **~80 px** de hauteur à la carte. ⛔ **Un raisonnement sur la seule règle de
base ne l'avait pas vu** — ni l'ergonome, ni le développeur, ni la relecture ; **l'image l'a montré
en deux secondes**. La technique est celle, déjà éprouvée, de `.filtration-modes.grid--2` :
spécificité supérieure dans la même media query. Classe **générique**, ⛔ pas `lighting-*`.
⚠️ **Le mot du projet est « Sauvegarder »**, pas « Enregistrer » (mesuré : 10 occurrences contre 2,
et les deux exceptions sont des boutons secondaires de calibration).

⛔ **RÈGLE ABSOLUE, née du défaut de la page Produits (2026-10-09)** : le bouton n'est **JAMAIS**
sur la même ligne qu'un champ, **à aucune largeur**. Il est sur sa **propre ligne**, en dessous.
⚠️ Ce n'est **pas un repli mobile**, c'est la disposition **par défaut** — c'est exactement la
confusion qui avait produit un bouton « perdu entre les 2 champs » sur téléphone.

**Visibilité** : le bloc entier porte l'attribut `hidden` (⛔ pas `style.display`) hors du mode qui
le rend pertinent. ⛔ Jamais affiché en lecture seule à côté.
**États** : échec d'enregistrement ⇒ **valeur réelle restaurée** + `.field.is-error` et
`.field__error` sur le champ fautif (§5.13) ; sauvegarde en cours ⇒ `.btn--loading`.
⚠️ **Ne pas hériter du manque de §5.13** : relier le champ à son message d'erreur par
`aria-describedby` dès la création. Le projet n'a **aucun** `aria-invalid` ; un composant neuf n'a
aucune raison de reprendre ce défaut.
**Pourquoi pas une classe dédiée** : la composition existante couvre le besoin sans dupliquer
`.field` ni `.detail-actions`. Une classe enveloppe ajouterait une variante sans mieux réutiliser.

### 5.21 Page de secours OTA — quatre contextes d'emploi, **hors `data/`** (feature-151 D3-b1)

**Contexte d'emploi déclaré : la page de secours embarquée dans le firmware** ([`src/ota_recovery_page.html.in`](../src/ota_recovery_page.html.in), servie par `GET /recovery`). Elle vit dans la flash, **sans** `app.css` : le filesystem est précisément ce qui peut manquer. Elle ne charge aucune feuille ni aucun fichier (pas même le logo). Elle reprend le **vocabulaire** du tableau de bord et la palette **conforme** de `app.css` (`--accent #2563eb`, 5,2:1), pas le style sous AA de `login.html`/`wizard.html`/`wifi.html` (voir `bug-182`).

⚠️ **Aucun composant partagé n'évolue** : ce sont des **copies figées et locales** à la page. Aucun inventaire d'emplois dans `data/` n'est dû, et un changement de `app.css` ne les met pas à jour.

| Élément | Nature | Détail |
|---|---|---|
| Callout `--info` **en page autonome** | copie figée de `.callout--info` | message « L'interface web est absente ou endommagée. » + « La régulation continue : pH, ORP, pompes et filtration fonctionnent. » — la phrase rassurante est le libellé le plus important de la page, **jamais retirée** |
| Sélecteur de fichier à libellé propre (`.pick`) | copie **locale** (D3-b2) | Remplace le « champ fichier à 44 px » (le sélecteur natif affichait « Choose file / No file chosen » en anglais). `<input type=file class="vh">` reste dans le DOM (**pas** `display:none` : focus clavier et accessibilité) **immédiatement avant** son `<label class="pick" for>` (sélecteur `+` du focus). **Une seule** étiquette porte le nom du champ et l'état : **vide** = cadre **pointillé** + « Appuyer pour choisir un fichier » ; **rempli** = cadre **plein** + nom du fichier inséré par `textContent`. Le sens ne repose **jamais** sur la couleur seule (forme du cadre + texte). Boîte ≥ 56 px, noms longs à la ligne (`overflow-wrap:anywhere`). La règle `input{…}` devient `input:not(.vh)`. 🔴 **(périmé, D3-c : `.pick` est désormais AUSSI dans `app.css`, appliqué aux 3 `<input type="file">` de `data/index.html` ; voir §champ fichier)** Ancien impact : NUL — le composant n'est **pas** partagé, aucun sélecteur de `app.css` n'est touché (inventaire réel : 3 emplois dans `data/index.html`, 0 modifié) |
| Ligne de version (`.ver`) | copie **locale** (D3-b2) | « Version du firmware : <b>2.89.0</b> » en tête de la carte d'envoi (28 caractères, une ligne à 390 px) ; numéro en `<b>` (`--text`), préfixe en `--muted`. **Constante de compilation**, pas un titre : poids inférieur au bandeau rassurant. Absente de l'écran de connexion |
| `<progress>` natif | premier emploi | aucun `<progress>` n'existait dans `data/` (0 occurrence mesurée dans `index.html`, `app.js`, `login/wizard/wifi.html`) ; barre pleine largeur de 12 px, libellé « Vérification du fichier : 42 % » en texte à côté, **jamais seule porteuse** du statut |
| Callout d'**échec rouge** | variante **locale** (`#msg.err`) | ⛔ **pas** un `.callout--danger` partagé. Toujours préfixé « Échec : » (le mot porte le sens, la couleur l'appuie — §7), texte fixe inséré par `textContent` |

**Supprimés en D3-b2** (octets et sobriété) : le **dégradé** de fond (`body` en `background:var(--bg)`), l'**ombre** des cartes (la bordure reste), l'**en-tête de page** « Mode secours » (le `<title>` le garde ; le `h1` porte l'état) et les **`<h2>` de carte** (« Connexion », « Restauration », « Premier démarrage »), redondants avec le bouton ou l'aide. L'aide « Les deux fichiers sont nécessaires » est **gardée** : elle explique pourquoi le bouton est grisé. **Pas de mode sombre ajouté.**

Parcours : page **unique**, étapes révélées par `hidden`, **aucune redirection** (le lien « Ouvrir l'interface quand même » vers `/ui` doit rester visible). Tout est empilé en pleine largeur : **aucun libellé sur une ligne partagée** (390 px d'abord, §6).

### 5.22 Écran de connexion (`login.html`) — ralentissement annoncé (bug-189, v2.94.0)

**Contexte d'emploi déclaré : `data/login.html` uniquement** (feuille `<style>` interne, `app.css` n'est pas chargé). **Aucun composant partagé n'évolue** ; aucun inventaire d'emplois dans `app.css` n'est dû.

- **`.alert.warn`** — variante **locale** à `login.html` (fond ambre translucide, bordure `rgba(180,83,9,.35)`, texte `#92400e`), à côté de `.alert` d'erreur (rouge `#b91c1c`, assombri de `#dc2626`). Emploi : le seul message « Trop d'essais. Patientez un instant. » (texte fixe par `textContent`, jamais celui du serveur). Factuel et sans reproche : le ralentissement peut frapper le propriétaire à cause d'un tiers. Le compte à rebours n'est **pas** dans l'alerte : il vit dans le libellé du **bouton désactivé** (« Réessayer dans 0:42 »), seule source du chiffre, hors `aria-live`. L'alerte 429 vit exactement le temps du décompte ; les alertes 401/« injoignable » ne s'effacent plus au bout de 5 s (une alerte de 5 s sur un blocage de 60 s laissait un bouton inerte sans explication).
- **Champ « Nom d'utilisateur » supprimé** (il était en lecture seule, valeur `admin`). ⚠️ Un champ `name="username"` masqué (`.vh`, `tabindex="-1"`, `aria-hidden`) est **conservé** pour les gestionnaires de mots de passe ; la règle d'entrée devient `input:not(.vh)`.
- **Contraste du bouton** : fond de `#5c7cdb` (**3,92:1** avec le texte blanc, sous le seuil AA de 4,5) à `#4a68c8` (**5,12:1**, mesuré) ; second arrêt du dégradé `#687ad8` → `#5470d0` (4,56:1). Appliqué à `login.html`, `wifi.html` et `wizard.html` : **3 occurrences** changées sur 15 — `#5c7cdb` sert aussi aux bordures de focus et à la barre de progression (critère 3:1, non textuels, **non concernés**). ⛔ Un remplacement global aurait été faux.
- Bouton désactivé : opacité 0,4 → 0,6 (le libellé de décompte doit rester lisible).
- La **page de secours OTA** (§5.21) reprend le même mécanisme (`#msg.warn`, décompte dans `#b-login`), en copie figée locale.

🔴 **RÉSIDU NON TRAITÉ — sous le seuil AA** : le **sous-titre de `wizard.html`** (14 px, poids 400, blanc sur le dégradé d'en-tête) reste à **3,92:1**, et c'est le **premier écran qu'un client voit à la livraison**. La **pastille d'étape** (`wizard.html:148`, chiffre blanc) est dans le même cas. L'en-tête de `wifi.html` (24 px) **passe**, lui, au titre du « grand texte ». Spec de suite à ouvrir ; non traité dans bug-189 (composant et écrans hors périmètre).

### 5.23 `.field__help` et `.secret-value` (bug-187, v2.95.0)
**`.field__help`** — **Rôle** : phrase de règle placée **entre le libellé et le champ**, avant la
frappe (« Règle : 12 caractères minimum, avec… »). 12 px, couleur `--muted`. **Contexte** : page de
réglage (Wi-Fi, Sécurité). ⚠️ À ne pas confondre avec `.field__hint` (§5.11), qui se place **sous**
le champ ; relier au champ par `aria-describedby`.
**`.secret-value`** — **Rôle** : afficher une valeur secrète à **recopier à la main** (clé Wi-Fi du
réseau de secours). Monospace 24 px, ligatures coupées, `user-select: all`, passe à la ligne
(`overflow-wrap: anywhere`) : 12 caractères tiennent à 390 px. **Contexte** : page de réglage
uniquement. Masqué par défaut (puces), révélé sur geste explicite (60 s puis re-masqué, et masqué
dès que la page passe en arrière-plan). Le bouton associé (`.ap-pwd-btn`) fait 44 px de haut et
passe en pleine largeur sous 500 px.

### 5.24 Langage visuel du tableau de bord (feature-195, v2.96.0, [ADR-0135](adr/0135-l-accueil-montre-les-reglages-reglent-la-couleur-n-est-que-sur-la-donnee.md))
**Principe** : **l'accueil montre, un volet règle, les pages de réglage configurent. La couleur n'est
que sur la donnée.** Portée : **tableau de bord uniquement**. Les jetons sont **scopés** `.card--status` :
`--card-radius`, `--shadow` et `--card-border` (43 cartes de l'application) **ne bougent pas**.
Fond de page plus bleuté (`--bg-gradient-start/end`, global) ; `login.html`, `wifi.html` et `wizard.html`
gardent leur propre fond.
**Retouches v2.96.1 (2026-11-20)** :
- **Fond** : `--bg-gradient-start` `#c8d3e4`, `--bg-gradient-end` `#d2d9ea` (étaient `#d9e2fb` / `#e6e3f8`,
  environ deux fois trop saturés, teinte 249 dérivant vers le violet). Aligné sur les trois pages autonomes
  (`#e8ecf4` / `#dfe5f0`, teinte 219-220), qui étaient déjà correctes. Séparation carte blanche / fond :
  1,29 → 1,51 (haut), 1,33 → 1,41 (bas).
- **Rayon** : `--card-radius` garde sa valeur, mais `.rescue-active-banner` et `.dose-block-banner__row`
  (bandeaux posés sur le tableau de bord, composant réemployé dans un contexte nouveau) prennent
  `--dash-radius` (22 px) au lieu de 16 px. La phrase « ne bougent pas » vaut pour la valeur du jeton, **pas**
  pour ces deux emplois.
- **Exception de couleur documentée** : `.footer .muted { color: var(--text-secondary); }`. Le pied de page est
  le seul `.muted` posé directement sur le fond dégradé : `var(--muted)` y tombe à 4,54:1 (AA, sur la limite),
  `--text-secondary` donne 5,35:1. Les 53 autres emplois de `.muted` sont dans des cartes blanches (7,4:1) et
  ne doivent pas être modifiés : **ne pas généraliser cette exception « par cohérence ».**

**Jetons (scopés `.card--status`)** : `--dash-radius: 22px` · `--dash-shadow` (diffuse, sans bordure) ·
`--dash-pad: 18px` · `--dash-chip-bg: #f1f3f7` · `--dash-dot-ok #15803d` / `--dash-dot-off #6b7280` /
`--dash-dot-warn #b45309` · `--hero-size: 32px` — **valeur unique, pas de repli en petit écran** (à 32 px, un
repli à 40 px rendrait la mesure plus grande sur écran étroit que sur écran large), même corps que la plage
horaire de Filtration · `--hero-weight: 800`.
**Variantes du héros** : `--warn` = 28 px (hero de Filtration en alerte : texte plus long que « 7,3 », gardé plus
petit pour ne pas déborder à 320 px). La variante `--sm` est **supprimée** : la plage horaire utilise la taille standard.
**Corps des satellites** (pour que la règle « la mesure est l'élément le plus grand de la carte, sans concurrence »
reste vérifiable) : `.compact-unit` 16 px, `.dash-hero__target` 14 px. À 32 px le rapport avec la consigne tombe à 2,3 ;
la hiérarchie tient parce que la taille n'est pas seule à la porter : la mesure est en **800** et en `--text` (encre),
la consigne en **600** et en `--text-secondary`, alignée à droite sur la même ligne de base.
Aucun survol sur ces cartes (elles ne se cliquent pas) ; `prefers-reduced-motion` respecté.

#### `.dash-pill` — pastille d'état
**Contexte d'emploi** : **carte d'accueil uniquement**. **Une par carte**, écrite par `setDashPill()` seule.
**Grammaire unique** (la couleur est dans la **puce de 8 px**, le texte est toujours `--text` : aucun
verdict porté par la couleur seule, §7) :

| Puce | Sens | Exemples |
|---|---|---|
| **Verte** (`data-tone="ok"`) | fonctionnement attendu en cours | En marche · Mode Auto · Mode Programme · À jour · Allumée |
| **Grise** (`off`, défaut) | repos ou information normale | Arrêtée · Mode Arrêt · En attente · Éteinte · Hors ligne |
| **Ambre** (`warn`) | l'appareil s'écarte de son fonctionnement automatique attendu, **et ça demande un regard** | pH/ORP : `doseBlockVariant === 'danger'` · Filtration : mode **forcé** · Température : sonde absente |

⛔ **Pas d'ambre** pour : filtration arrêtée sur pH/ORP (codes 2, 7, 8, 10-14, marches attendues),
étalonnage en cours (il a son bandeau), « En attente », arrêt normal d'Auto hors créneau, Éclairage
(jamais). Aucune autre teinte, **pas de rouge**. Une exception (stock faible, étalonnage, sonde) est une
**bande** sous la mesure (`.sensor-badge`), jamais une seconde pastille. Libellés ≤ 14 caractères
(« Température » + « Sonde absente » = 272 px sur 326 utiles à 390 px).
**Sur pH/ORP**, la pastille porte le **texte du mode** (« Mode Auto / Programme / Arrêt ») — texte gaté
C5, jamais tronqué ni réduit à la puce ; hors ligne il est conservé (puce grise), les autres cartes
disent « Hors ligne ».

**Sur Éclairage**, la pastille a **quatre** libellés, pas deux : « Allumée », « Éteinte »,
**« Programme »** et « Hors ligne ». « Programme » est le cas où l'état réel du relais est inconnu
**et** où le mode ne permet pas de le déduire : ⇒ la pastille **refuse de deviner** et nomme le mode.
⛔ Ne jamais la faire basculer sur « Allumée » ou « Éteinte » par défaut dans ce cas (règle §4).

ⓘ **Le froid n'est plus une teinte de pastille (bug-197).** L'ancien arbitrage de priorité entre l'ancien libellé de froid et « forcé » est **sans objet** : la pastille ne dit plus jamais le froid, il n'y a plus qu'un axe. En Marche forcée à 3 °C on voit : titre « Marche forcée » ambre, pastille « En marche », **et** la bande de froid (§5.9) — aucun signal n'en efface un autre. « Arrêtée » reste **grise** sur l'accueil comme sur la page Filtration (la pastille rouge de la page a été retirée : état normal en Auto). Voir [ADR-0141](adr/0141-l-appareil-est-temoin-du-froid-jamais-agent.md).

#### `.dash-act` — « bouton de dérogation » (Forcer marche / Forcer arrêt / Revenir en Auto)
**Contexte d'emploi** : **carte d'accueil uniquement** (carte Filtration). Autre geste que le choix du
mode : une **dérogation** par-dessus lui, qui passe par le **même** `setFiltrationRunMode()` que le
sélecteur à 4 modes du volet. 44 px, neutre ; sélectionné (`aria-pressed="true"`) = encre. En Auto et
Programme, aucun n'est sélectionné. « Revenir en Auto » (`.dash-act--wide`) n'apparaît **que** forcé et
ramène **toujours en Auto** (jamais en Programme). ⚠️ Marche/Arrêt existent à deux endroits : la parité est
tenue par `updateFiltrationModeButtons()` qui met à jour **les deux jeux** dans la même fonction.
**Règle de teinte** : puce grise = arrêt normal du mode ; puce **ambre = état imposé par l'utilisateur**.

#### `.dose-block-banner__row--suspend` — bandeau d'arrêt de sécurité (bug-200, v2.98.0)
- **Contexte d'emploi : tableau de bord uniquement** (`#dose-suspend-banner`). Colonne unique, `border-left: 4px solid var(--danger)`, texte `--text`, titre `--danger`, **sans pulsation**, sans bouton. Modificateur de `.dose-block-banner__row--fault`.
- **Opacité pleine même périmé** (`.is-stale`) : `--danger` à 60 % passe sous 4,5:1 ; la note de péremption est dupliquée dans le bandeau.
- ⚠️ **Règle d'ordre** : ce bandeau passe **au-dessus** de celui du secours (`#rescue-active-banner`). C'est l'**inverse** de feature-140 (le secours au-dessus du blocage) : ici la **cause** prime sur sa **conséquence**.

#### `.card__fold` / `.card__fold-panel` — volet « Modifier »
**Contexte d'emploi** : **carte d'accueil uniquement** (pH, ORP, Filtration). Bouton pleine largeur de
44 px, chevron en **caractère de texte** (▾ / ▴), `aria-expanded` + `aria-controls`. **Piloté par le clic
seul** (`bindDashFolds`) : ⛔ aucune fonction `render*` ne l'ouvre ni ne le ferme, et le bouton n'est
**jamais** `disabled` (sinon l'escalade rendrait « Arrêt » inatteignable). Les bandeaux
(`#dose-block-banner`, `#rescue-active-banner`) et le texte C7 « La régulation automatique ne dose plus… »
restent **hors** du volet. Le lien « Tous les réglages › » (`.card__link`, 44 px) en est la dernière ligne ;
la carte Température, sans volet, n'a que ce lien. Les roues dentées de 34 px disparaissent.

#### Qui a deux apparences — contexte d'emploi déclaré, pas une troisième forme
`.mode-select`, `.state-badge`, `.segmented--num` et `.card__option` ont **deux apparences selon le
contexte** : **encre** sur la carte d'accueil (scopées `.card--status`), **bleu** sur les pages de
réglage. Les pages de réglage **s'aligneront dans un lot ultérieur si la direction est validée** ; ce
lot ne touche que le tableau de bord. ⚠️ Ne pas « corriger » un emploi hors accueil dans l'intervalle :
ce serait une application partielle.
**Règle de couleur des commandes (accueil)** : sélection = encre ; action = `--accent`, **au plus une
action bleue par carte** ; verrou d'étalonnage : garde son fond `--warn`.

#### Cas limites
Hors ligne : héros « – », pastilles « Hors ligne » (sauf pH/ORP), commandes désactivées, jauge `--stale`.
Mesures non représentatives : héros « – », pastille « En attente », jauge `--stale` **+ le mot d'état
conservé** (`#ph-gauge-state`, exception bloquante d'un gate `pool-chemistry`). Sonde absente : « – »,
**jamais une valeur figée**, pastille « Sonde absente » (ambre). Chargement : « Chargement… » grise.
Nombres à la française (virgule), **un seul formateur** `fmtFr` pour `/data` et WebSocket, chiffres de
largeur fixe.

#### Choses connues et non faites
- **R1 — perte assumée** : l'avertissement « consigne pH à la borne haute » n'est plus visible en
  permanence sur l'accueil (dans le volet). Il informe, il ne protège pas.
- **R2 — non fait** : « Volume quotidien » et « Limite de sécurité » sont dans le volet alors qu'en mode
  Programme ils disent ce que l'appareil va faire.
- **Mode sombre** : toujours inexistant (§8).

### 5.25 `.flow-maintenance` survit au mode réglage (bug-201, v3.0.0)

> ⭐ **`.flow-maintenance` ne signifie PAS « visible seulement en mode réglage » : elle signifie « SURVIT au mode réglage ».** Les éléments qui la portent sont, pour la plupart, `hidden` par défaut et dévoilés par `enterMaintenance()` ; **`#ph-card-tune` est l'exception** : permanente, mais marquée, parce que la règle générique masquerait sinon tout enfant non marqué pendant le flux.

**Mécanisme** (`data/app.css`) : `#view-ph.is-flow-maintenance > :not(.flow-maintenance)` (idem `#view-orp`, `#view-temperature`) masque tout enfant direct de la vue qui n'est pas marqué. `enterMaintenance()` / `exitMaintenance()` (`data/app.js`, table `MAINTENANCE_PAGES`) ne font que lever / reposer le `hidden` des éléments listés dans `cardIds`.

**Inventaire mesuré le 2026-11-22** — **18 éléments** portent la classe dans `data/index.html` (compte par attributs `class="…flow-maintenance…"`, hors commentaires) :

| Vue | Éléments | Nombre |
|---|---|---:|
| Température | en-tête `.settingsHead`, région d'annonce `.sr-only`, lien de retour haut, carte d'étalonnage, lien de retour bas | 5 |
| pH | en-tête `.settingsHead`, lien de retour haut, **`#ph-card-tune`**, région d'annonce `.sr-only`, bandeau de bascule (`callout--warn`), carte d'étalonnage, lien de retour bas | 7 |
| ORP | en-tête `.settingsHead`, région d'annonce `.sr-only`, lien de retour haut, bandeau de bascule, carte d'étalonnage, lien de retour bas | 6 |

⭐ **Un seul emploi change d'appartenance** : `#ph-card-tune` (sortie de `cardIds`, **plus de `hidden`**, classe conservée). Les 17 autres sont **inchangés** (en-têtes et régions d'annonce : toujours visibles, la classe leur évite d'être masqués ; liens, cartes et bandeaux : `hidden` par défaut). C'est la **première** carte à sortir du flux.

⛔ **Pièges**
- Retirer **la classe** (et non l'entrée de `cardIds`) masque la carte **pendant l'étalonnage**, au moment où la cause `calibration` vient de se lever et où ses boutons servent.
- Ne jamais réintroduire `hidden` ni un `el.style.display` sur `#ph-card-tune` (un style inline écrase la règle sans effet visible).
- ⚠️ **Placement dans le DOM** : l'ordre compte en mode réglage. Le lien `#ph-maintenance-back-top` est **avant** `#ph-card-tune` ; entre deux cartes visibles, il se lisait comme un pied de la carte de correction (défaut vu sur capture).

Décision et alternatives : [ADR-0140](adr/0140-quand-un-gate-renonce-a-bloquer-la-contrepartie-est-la-visibilite-permanente.md). Voir aussi [ADR-0103](adr/0103-gestes-d-entretien-quittent-la-vue-courante.md) (le mécanisme d'origine) et [`docs/features/page-ph.md`](features/page-ph.md#mécanisme--entermaintenance--exitmaintenance).

### 5.26 Bloc de confirmation d'un acte destructif (`.ph-tune-confirm__actions`, bug-201, v3.0.0)

**Contexte d'emploi déclaré : page de réglage pH, « Supprimer la correction » (`#ph_tune_remove_confirm`).** Un premier appui **remplace** le bouton par : un `callout callout--warn` (`role="group"`, la phrase de conséquence chiffrée), puis `.ph-tune-confirm__actions` — conteneur `flex` en **colonne**, `gap: 12px`, enfants `width: 100%` et `min-height: 44px` (390 px d'abord, doigt mouillé). Ordre : **« Ne rien changer »** (`btn--secondary`, reçoit le focus) **puis** le destructif (`btn--danger action-destructive`).

Règles : la cible destructive n'occupe **pas** la position du bouton d'origine ; sortie au moins aussi facile que la confirmation (Échap, clic extérieur, changement de page, retour de config) ; ⛔ ni `confirm()` natif, ni mot à saisir, ni appui long, ni modale, ni minuterie. Ne se déclenche que si la conséquence le justifie (seuil servi) : *une garde qui bloque plus qu'elle ne protège est un défaut*. Aucun autre emploi à ce jour ; un nouvel emploi devra déclarer son contexte ici. Détail : [`docs/features/page-ph.md`](features/page-ph.md#friction-sur-supprimer-la-correction-bug-201-c41-c45-c47-v300).

### 5.27 `.daily-dose` — barre de quota journalier (bug-208, v3.1.0)

**Contexte d'emploi déclaré : carte héros des pages pH et ORP, et nulle part ailleurs.**
⛔ **Jamais sur le tableau de bord** : l'accueil dit le cumul par un **segment de `.sensor-badge`**
(« Cumul XX % », à partir de 75 %), pas par une barre. Réemployer `.daily-dose` sur une carte
d'accueil y introduirait une troisième forme pour la même information.

| Élément | Classe | Rôle |
|---|---|---|
| Conteneur | `.daily-dose` | le bloc entier, dans la colonne droite de la carte héros |
| En-tête | `.daily-dose__header` | rangée du libellé et de la valeur |
| Libellé | `.daily-dose__label` | « DOSAGE DU JOUR », à gauche |
| Valeur | `.daily-dose__value` | `injecté / limite mL` — ⚠️ **les deux nombres au même format** (`toLocaleString('fr-FR')`, cf. §4) |
| Barre | `.daily-dose__bar` dans `.daily-dose__bar-wrap` | `role="progressbar"`, variantes `dose-bar--ok` / `--warning` / `--danger` |
| État | `.daily-dose__status` | texte court piloté **uniquement** par le verdict firmware `*_limit_reached` |
| Note d'écart | `.daily-dose__note` | **gris** (`var(--text-secondary)`, 12 px), `:empty { display:none }`, cible d'`aria-describedby` de la barre |

### ⭐ La règle opposable — elle vaut pour TOUTE barre de quota, pas seulement celle-ci

> **Une barre de quota se remplit vers la limite réellement opposable — le minimum entre ce qui
> est réglé et ce que l'appareil peut délivrer — et le chiffre réglé, s'il diffère, est porté par
> une note attachée à la barre, jamais par une ligne flottante.**

**Origine du défaut.** La barre pH visait le plafond **réglé** (2 000 mL) tandis qu'une ligne
« Maximum journalier : 1 768 mL/j » flottait dessous, rattachée visuellement à **ni la barre ni
la rubrique suivante**. Résultat : le dosage s'arrêtait avec une barre à **88 %**, et le seul
élément qui disait pourquoi était posé à côté, sans lien. La correction **supprime la ligne
flottante** et déplace son information dans une note **attachée** à la barre — affichée
**seulement s'il y a un écart**.

⛔ **Pièges**
- **Gris, pas ambre.** C'est une information d'**installation**, vraie tous les jours chez cet
  utilisateur. Une alerte permanente cesse d'être lue et contaminerait le code couleur de la
  barre (`ok` / `warning` / `danger`), qui, lui, porte un verdict.
- **Aucune ligne permanente.** `:empty` referme le bloc : le composant ne doit **rien** ajouter à
  la hauteur de la carte quand il n'a rien à dire. La carte héros pH portait déjà **13 éléments**.
- **Aucun impératif dans la note**, et ⛔ **aucun nom de réglage** : suggérer de relever un plafond
  reviendrait à conseiller de défaire une garde (limite horaire) ou un geste inopérant
  (consigne). La note se termine par « il n'y a rien à corriger si l'eau est bonne ».
- **La note cède la place au stock.** Si le compteur du bidon est au bout ou bas, c'est ce
  message-là qui s'affiche : une explication juste sur un symptôme dont la cause est ailleurs
  retarde le seul geste utile.
- **Valeur inconnue ≠ 0 %.** `aria-valuenow` est **retiré** et remplacé par
  `aria-valuetext="indisponible"` — ⛔ ne jamais faire annoncer « 0 % » à un lecteur d'écran là où
  la valeur n'est pas connue (§4).

Détail fonctionnel : [`docs/features/page-ph.md`](features/page-ph.md#barre--dosage-du-jour--bug-208-v310)
et [`page-orp.md`](features/page-orp.md#barre--dosage-du-jour--bug-208-v310). Contrat des trois
notions servies : [`docs/API.md`](API.md#champs-du-plafond-journalier-chlore-bug-208-v310).

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
- Pour une **rangée de positions numériques sans cadre** (consigne pH, 4 positions,
  `.segmented--num` seul) : `gap:4px` × 3 intervalles = 12 px ⇒ `(322 − 12)/4 = `**77,5 px** par
  position. Le padding interne du bouton (`10px 4px`) retire 8 px ⇒ zone de texte **≈69,5 px**. Le
  libellé le plus large, « 7,2 » (3 caractères en chiffres tabulaires) ≈ **21 px** ⇒ marge
  **≈48 px**. Tient largement, et resterait tenable jusqu'à 5-6 positions.
  ⚠️ **Refaire ce calcul si les bornes serveur changent le nombre de positions généré.**
- Pour **deux champs côte à côte** (intervalle horaire dans une carte), `gap:8px` ⇒
  `(322 − 8) / 2 ≈ `**157 px** par colonne. Un `<input type="time">` (« 20:00 » + icône d'horloge +
  marges internes) demande **90-110 px** ⇒ **tient**, marge ≈ 47 px. Les libellés « Début » et
  « Fin » sont bien en-dessous du libellé de référence « Programme » (9 caractères) : aucun risque.
  ⚠️ **Mais sans la classe `.schedule-fields`, la règle générique empile les champs sous 640 px** —
  piège mesuré le 2026-10-14 sur capture réelle. ⛔ Ne jamais compter sur la règle de base seule.
- Toute nouvelle rangée de boutons/libellés : refaire ce calcul (largeur de colonne −
  padding horizontal du bouton ≥ largeur mesurée du libellé le plus long + 10px de marge),
  ne jamais se contenter d'« a l'air de tenir ».

## 7. Daltonisme

Aucun état ne doit reposer sur la seule teinte :
- `.mode-select` / `.segmented__btn.is-active` : fond plein **+** texte blanc **+**
  `aria-pressed="true"` (le lecteur d'écran et le focus clavier portent la même info).
- Bloc consigne verrouillé (feature-164) : `.segmented--num .segmented__btn:disabled.is-active` et
  `#dashboard-{ph,orp}-target-save:disabled` recolorent en `--warn` **ET** un `.field__hint` adjacent
  **nomme la cause** — ⛔ jamais la couleur seule, même pour un verrou hérité d'un composant déjà
  conforme ailleurs dans la même carte.
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
./tools/ui_shot.sh -k '#ph_cal_trigger_btn'      # cliquer avant la capture (v2.82.0)
./tools/ui_shot.sh -n                           # faux WebSocket : PAS de bandeau hors ligne (v2.96.0)
```

⚠️ **`-k SELECTEUR` (v2.82.0)** : clique sur l'élément désigné **avant** la capture — indispensable pour
photographier un écran situé **derrière un bouton** (sous-vue ouverte par JavaScript, panneau
repliable, modale). Sans elle, ces écrans étaient **invisibles à l'outil**, et un brief
d'ergonomie les concernant partait **sans image jointe** — ce que `CLAUDE.md` qualifie de brief
incomplet (voir « L'UX n'est pas optionnel »). Plusieurs `-k` s'enchaînent dans l'ordre donné
(ex. ouvrir le mode réglage **puis** cliquer sur un onglet à l'intérieur).

⚠️ **`-n` (2026-11-18) — sans elle, une capture du TABLEAU DE BORD ne montre AUCUNE mesure.**
L'outil sert `data/` et les réponses figées, mais **aucun WebSocket** : l'application ne voit
jamais de socket s'ouvrir, se déclare « Déconnecté », et `setNetState('bad')` **remplace les
valeurs pH, ORP et température par « – »**. ⇒ L'image ne porte alors pas le sujet même de la
page. `-n` installe un faux WebSocket qui s'ouvre et se tait — exactement l'état d'un appareil
connecté et silencieux.
⛔ **Elle reste optionnelle, et ce n'est pas un oubli** : l'écran **hors ligne** est lui aussi à
photographier, et il l'est **par défaut**. C'est d'ailleurs sur une capture prise **sans** `-n`
qu'a été trouvé [bug-196](../specs/features/todo/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md)
(le bandeau d'état passe sous le bouton de menu à 390 px).
⇒ **Règle** : capture du tableau de bord pour un brief d'ergonomie ⇒ **avec `-n`** ; capture de
l'état hors ligne ⇒ **sans**.

⚠️ **`-f` est le plus utile et le moins évident** : il force un champ de `/data` pour atteindre un
état qu'on ne peut pas provoquer à la demande. C'est ainsi qu'a été vérifié le rendu **verrouillé**
(étalonnage en cours) du 14/10, que personne n'avait jamais vu.
⛔ Les données recopiées contiennent le **SSID Wi-Fi** et l'**identifiant MQTT** : elles vont dans
un dossier temporaire, **jamais dans le dépôt**. Ne pas les y ajouter « pour la reproductibilité ».

⚠️ **Limite connue, à ne pas confondre avec un défaut** : les champs `<input type="time">`
apparaissent au format **américain** (« 08:00 PM ») dans les captures. Ce type de champ est rendu
par le navigateur selon **sa** locale, et le navigateur sans interface l'ignore ici (testé avec
`--lang`, `--accept-lang`, `LANG`/`LC_ALL` et la locale `fr_FR.UTF-8` générée : aucun effet). Sur
un téléphone en français, l'heure s'affiche **20:00**. ⛔ Ne pas « corriger » l'interface sur la
foi d'une capture à cause de ça.

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
| Cartes spécialisées | `install-card`, `product-card`, `product-volume` |
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
