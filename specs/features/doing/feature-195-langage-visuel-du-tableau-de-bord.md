## Métadonnées
- **ID** : feature-195
- **Slug** : langage-visuel-du-tableau-de-bord
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.96.0
- **Spec(s) liée(s)** : [feature-153](../done/feature-153-signaux-capteurs-medianne-et-deux-cadences.md) (mandat UX découpé), [feature-164](../done/feature-164-regler-les-valeurs-depuis-les-cartes.md), [feature-147](../done/feature-147-widgets-du-tableau-de-bord-lire-avant-de-chercher.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] ux-ui-designer — **mandat sur l'ÉCRAN ENTIER**, pas sur un composant
- [ ] pool-chemistry — **à confirmer** : aucun seuil ni dosage touché, mais la carte pH
      porte des gardes posées par des gates chimiques antérieurs (voir « Notes »)
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Refondre le langage visuel du **tableau de bord** pour que la **mesure** redevienne
l'élément dominant de chaque carte, et que les commandes passent au second plan.
Première étape d'un chantier qui pourra s'étendre aux autres écrans — mais **cette
spec ne couvre que le tableau de bord**.

## Contexte
Demande de l'utilisateur du **2026-11-18**, sur la base de cartes vues dans un
reportage IFA 2026 (domotique) qu'il a jugées « beaucoup plus modernes » que
l'application. Analyse de l'écart, faite sur captures rendues (⚠️ pas de mémoire) :

**Ce qui fait l'écart — par ordre d'importance décroissante.**

1. **Nos cartes DEMANDENT, les leurs MONTRENT.** La carte pH contient trois boutons de
   mode, quatre boutons de consigne et un bouton Sauvegarder : c'est un **formulaire de
   réglage posé sur l'écran d'accueil**. La mesure, seul motif de consulter la page au
   bord du bassin, y occupe moins de place que les boutons.
2. **La couleur est sur la chrome, pas sur la donnée.** Relevé sur la capture nominale :
   **huit surfaces bleu `#2563eb`** sur un seul écran — Auto filtration, Activer boost,
   Éteint éclairage, Auto pH, chip de consigne 7,2, Sauvegarder pH, Auto ORP,
   Sauvegarder ORP — dont trois pleine largeur. Dans la référence, le chrome est gris
   neutre et **la couleur n'apparaît que sur les barres et les valeurs**.
   ⚠️ **Chiffre corrigé le 2026-11-18** : j'avais d'abord écrit « cinq pavés pleine
   largeur », en comptant à l'œil et en ne retenant que les pleines largeurs.
   `ux-ui-designer` a recompté sur l'image : huit surfaces, trois pleine largeur.
   Le recomptage est le bon (ADR-0073 : un chiffre porte sa méthode).
3. **Titres en capitales espacées** (`FILTRATION`, `PH`, `ORP`, `TEMPÉRATURE`) — ils
   crient et datent. La référence met un titre discret en casse normale ; c'est la
   donnée qui porte.
4. **Pas d'état lisible d'un coup d'œil.** « Mode Auto » en texte gris sur pH et ORP,
   une pastille « En marche » sur Filtration, rien sur Température : trois grammaires
   pour la même information.
5. **Profondeur quasi nulle** : rayon 16 px, ombre `0 1px 3px`, bord gris à 10 %. La
   référence : rayon ~22 px, ombre diffuse ample, aucun bord — la carte flotte.
6. **Fond gris** (`#e8ecf4 → #dfe5f0`) là où la référence pose un bleu-lavande franc qui
   fait ressortir le blanc des cartes.

**Acquis à ne pas jeter.** `data/app.css` porte déjà un système de variables nommées
(rayon, ombres, couleurs d'état validées AA, dégradés de jauge documentés). Une grande
part du travail se joue dans ces variables, **pas** dans une réécriture.

## Comportement attendu
Le tableau de bord est **l'écran de consultation**. Chaque carte répond, dans l'ordre et
sans défilement : *quelle est la valeur ?*, *est-ce normal ?*, *que fait l'appareil ?*

### Règles de conception (à instruire par `ux-ui-designer`, pas figées ici)
- La **mesure est l'élément le plus grand de la carte**, sans concurrence.
- **Un seul état par carte**, dans une grammaire unique (pastille), pour les quatre cartes.
- **La couleur ne sert qu'à la donnée.** Les commandes qui restent sur l'accueil sont
  neutres ; une seule action peut être mise en avant par carte, au plus.
- Ce qui règle part dans la page de réglage déjà existante (`#/ph`, `#/orp`, …), derrière
  un lien explicite. ⛔ **Aucune fonction ne disparaît** : tout ce qui est retiré de
  l'accueil doit être atteignable, et la spec doit dire où.
- **390 px d'abord.** Compter les caractères de tout libellé sur ligne partagée.

### Cas limites — tous à traiter, aucun n'est accessoire
- **Hors ligne** : bandeau + commandes désactivées + mesures remplacées par « – ».
- **Mesures non représentatives** (filtration arrêtée, stabilisation) : piste de jauge
  grisée + mot d'état. ⛔ C'est l'**exception bloquante** d'un gate `pool-chemistry`
  antérieur (cf. commentaire de `#ph-gauge-state` dans `index.html`) : le mot d'état ne
  peut pas être supprimé dans ce cas.
- **Sonde absente / valeur nulle** : « -- » explicite, jamais une valeur figée.
- **Daltonisme** : les jauges pH et température portent deux grammaires de couleur
  différentes (évaluative / descriptive, feature-147) — à préserver.

## Comportement actuel
Voir les captures jointes au brief (⚠️ toutes prises avec `-n`, cf. « Notes »).
Quatre cartes empilées : Filtration (mode + horaires + option Boost), Éclairage (3 modes),
pH (mesure + jauge + mode + consigne + Sauvegarder), ORP (idem, consigne en champ libre),
Température (mesure + jauge).

## Contraintes techniques
- ⚠️ **RESPONSIVE** : 390 px d'abord, puis tablette et ordinateur.
- Vanilla JS/HTML/CSS, pas de framework, pas de bundler hors `minify.js`.
- Français dans toute l'UI.
- Budget FS : la partition `spiffs` fait **512 Ko** (ADR-0068) — vérifier avec `./build_fs.sh`.
- ⛔ **Aucun nouveau champ `/data` ni WebSocket** : cette spec ne consomme que l'existant.
  Si un affichage souhaité exige une donnée absente, **l'écrire et la sortir du périmètre**.
- Parité `/data` ↔ WebSocket : ne pas introduire d'expression divergente entre les deux
  chemins de rendu (piège à 4 récidives).

## Fichiers potentiellement concernés
- `data/index.html` — structure des quatre cartes du tableau de bord
- `data/app.css` — variables de thème, carte, pastille, jauge ; mode sombre
- `data/app.js` — rendu des cartes, bascule hors ligne, jauges
- `docs/DESIGN_SYSTEM.md` — **document vivant** : contexte d'emploi de chaque composant
- `docs/features/page-dashboard.md` — doc vivante de la page
- `docs/user/src/guide-utilisation.html` — captures et descriptions
- `tools/ui_shot.sh`, `tools/ui_shot_server.py` — ✅ **déjà faits** (option `-n`)

## Livrables — check-list
- [x] T1 — Option `-n` de `ui_shot.sh` : faux WebSocket, pour que les captures du tableau
      de bord portent les mesures au lieu de « – » (fait le 2026-11-18, préalable au brief)
- [x] B1 — Étude d'ergonomie sur l'**écran entier**, rendue sur les captures jointes
      (2026-11-18 — voir « Étude d'ergonomie » ci-dessous) ⏸️ **en attente d'un arbitrage utilisateur**
- [x] B2 — Variables de thème : fond, rayon, ombre, règle « la couleur à la donnée »
- [x] B3 — Les cinq cartes du tableau de bord dans le nouveau langage
- [x] B4 — Pastille d'état unique, appliquée aux quatre cartes
- [x] B6 — États limites : hors ligne, mesures non représentatives, sonde absente
- [x] C1 — `docs/DESIGN_SYSTEM.md` (§5.24 neuf, §5.20 amendé) : composants modifiés + **contexte d'emploi**
- [x] C2 — `docs/features/page-dashboard.md`
- [x] C3 — `CHANGELOG.md` [2.96.0] + ADR-0135
- [x] C4 — Guide utilisateur : texte mis à jour + **liste des captures à refaire** (étape humaine)
- [x] D1 — Captures avant/après à 390 px et 1024 px
- [ ] D2 — Essai réel sur l'appareil après déploiement

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : **aucun**
- Champs WebSocket nouveaux : **aucun**
- Topics MQTT nouveaux : **aucun**

## Sécurité chimique
- Dosage impliqué : **non** — aucun seuil, aucune limite, aucun chemin vers `pump_controller`
- Calibration capteur modifiée : **non**
- Algorithme PID touché : **non**
- Risque identifié : ⚠️ **un seul, et il est réel** — la carte pH et la carte ORP portent
  des éléments dont la **visibilité permanente** a été imposée par des gates
  `pool-chemistry` antérieurs (voir les commentaires `C5` et « exception bloquante » dans
  `data/index.html`). Une refonte qui les déplace ou les masque **doit repasser par
  `pool-chemistry`**. ⇒ Cocher l'agent dès que l'étude touche à ces éléments.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte imposée au reste de l'interface), 3 (alternatives)
- **Sujet pressenti** : « l'accueil montre, les réglages règlent » — et la règle
  « la couleur n'est que sur la donnée », qui engage tous les écrans suivants.

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- aucun
### Champs WebSocket
- aucun
### Topics MQTT
- aucun
### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-dashboard.md`
- `docs/DESIGN_SYSTEM.md`
#### Docs transverses
- `CHANGELOG.md`
- `docs/user/src/guide-utilisation.html`
#### Docs immuables
- `docs/adr/NNNN-l-accueil-montre-les-reglages-reglent.md` (titre à arrêter par l'architect)

## Critères d'acceptation
- [ ] Sur chaque carte, la mesure est l'élément le plus grand ; vérifié **sur capture**
- [ ] Au plus **une** action mise en avant par carte ; les autres commandes sont neutres
- [ ] Les quatre cartes emploient la **même** grammaire d'état
- [ ] Aucune fonction retirée de l'accueil n'est devenue inatteignable — chaque
      déplacement est nommé dans la spec avec sa nouvelle adresse
- [ ] Rendu vérifié à **390 px** et 1024 px, sur captures
- [ ] Les états hors ligne et « mesures non représentatives » restent lisibles
- [ ] Tous les libellés utilisateur sont en français
- [ ] `./build_fs.sh` passe sans dépasser la partition
- [ ] `pio run` compile sans nouveau warning

## Plan de test manuel
1. Nominal : filtration en marche, mesures valides — les trois chiffres lisibles à bout de bras.
2. Filtration arrêtée : mesures non représentatives — le mot d'état est présent (gate chimique).
3. Appareil injoignable : bandeau hors ligne, commandes désactivées, « – » partout.
4. Les trois cas ci-dessus rejoués à 390 px et à 1024 px.

## Notes pour agent
- ⚠️ **Toutes les captures du tableau de bord se prennent avec `-n`** (option ajoutée le
  2026-11-18). Sans elle, l'application ne voit aucun socket s'ouvrir, se déclare
  « Déconnecté » et **remplace les trois mesures par « – »** : l'image ne montre alors pas
  le sujet de la page. L'écran hors ligne, lui, se photographie **sans** `-n`.
- ⚠️ **L'appareil de production est en état dégradé** (filtration arrêtée, `ph`/`orp` à
  `null`). L'état nominal s'obtient par forçage :
  `-f measures_valid=true -f filtration_running=true -f water_present=true -f ph=7.32
   -f orp=486 -f temperature=24.8 -f phDoseBlockedCode=0 -f orpDoseBlockedCode=0`
- ⚠️ **Leçon de feature-153, à ne pas répéter** : le mandat de `ux-ui-designer` ne se
  découpe pas. Tout choix visuel lui revient, **y compris ceux qu'on n'a pas pensé à lui
  confier** — palette, épaisseur de trait, typographie, densité, états vides, cibles
  tactiles, contraste, daltonisme, responsive.
- ⚠️ `docs/DESIGN_SYSTEM.md` est **vivant**, jamais une spec. Toute évolution d'un
  composant **partagé** oblige à trois choses ensemble : mesurer l'inventaire exhaustif
  des emplois, dire ce que deviennent ceux qui ne correspondent plus, et **tout traiter
  dans le même lot**. ⛔ L'application partielle rend l'interface **moins** cohérente
  qu'avant.
- L'application est **vendue à des clients** : la refonte périme des captures du guide
  utilisateur. Les lister explicitement — c'est la seule étape humaine.

## Hors périmètre
- Les autres écrans (Réglages, Produits, Historique, Assistant, Connexion) — ils suivront
  si la direction est validée sur le tableau de bord.
- **Le mode sombre.** ⚠️ Vérifié le 2026-11-18 : il **n'existe pas** dans ce projet —
  aucune règle `prefers-color-scheme` dans `data/app.css`, et `app.js:12749` le dit
  explicitement. L'introduire est une décision à part entière, pas un effet de bord d'une
  refonte de cartes. ⇒ Si l'étude d'ergonomie le recommande, **elle le dit et on ouvre une
  spec** ; elle ne le livre pas ici.
- Tout nouveau champ de donnée, tout nouvel endpoint, toute nouvelle mesure.
- Le menu, l'en-tête et le pied de page, sauf si l'étude démontre qu'ils empêchent le
  résultat — et alors elle le dit au lieu de le faire.

---

## ⭐ ÉTUDE D'ERGONOMIE — 2026-11-18 (`ux-ui-designer`, mandat écran entier)

### Ce qu'elle a corrigé dans mon analyse
- **Huit** surfaces bleues, pas cinq pleine largeur (voir « Contexte », chiffre corrigé).
- Mesure pH = **32 px** sous un en-tête de 66 px, suivie de **~290 px de commandes**.
  Page totale ≈ **1 880 px** à 390 px de large, là où cinq cartes de mesure tiendraient
  en ~1 000.
- ⛔ **Trois éléments de mon croquis sont refusés, à juste titre** :
  - « dans la zone » est le mot d'état évaluatif qu'un gate `pool-chemistry` a fait
    **retirer** (« Idéal ») — on ne le remet pas ;
  - « +1,2 °C depuis hier », « Dernière injection 11:42 », « Aujourd'hui 120 ml » :
    **aucune donnée dans `/data`** — hors périmètre ;
  - la carte « Bassin 3 sur 4 » est un contenu neuf — hors périmètre.
- Retenu du croquis : chiffre à gauche / consigne à droite sur une ligne, jauge pleine
  largeur, état en pastille.
- **Défaut trouvé au passage, sorti en fiche** : [bug-196](../todo/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md).

### Les trois options étudiées
| | Principe | Verdict |
|---|---|---|
| **A** | Aucune commande sur l'accueil, tout part vers les pages de réglage | ⛔ annule feature-161, 162 et 164, demandées explicitement par l'utilisateur |
| **B** | Mêmes commandes, couleurs neutres | ⛔ la page reste un formulaire, la mesure ne gagne pas de place |
| **C** | Mesure + pastille + contexte visibles ; commandes **existantes** dans un volet « Modifier » replié | ✅ retenue par l'agent — un tap de plus pour régler |

⏸️ **ARBITRAGE UTILISATEUR REQUIS AVANT DE CODER.** L'option C replie derrière un tap ce
que l'utilisateur a demandé le **2026-10-13** en ces termes : « *Les cartes permettraient
de choisir le mode dans la carte. C'est pratique plutôt que d'aller dans la page de
réglage de la carte.* » L'agent le signale lui-même comme une décision humaine. ⛔ Ne rien
implémenter de la structure tant que ce point n'est pas tranché.

### Règle d'écriture proposée
**L'accueil montre · un volet règle · les pages de réglage configurent.**

### Ce qui disparaît, et où ça va
| Retiré de l'accueil | Destination |
|---|---|
| Sous-titre « Vue d'ensemble de votre piscine » | supprimé (redondant) |
| Titres en capitales espacées | casse normale |
| Bordure de carte + filet sous l'en-tête | supprimés |
| Survol : lift 2 px, icône qui grossit, chiffre qui bleuit | supprimés — **fausse affordance** (les cartes ne se cliquent pas) et collant sur tactile |
| 5 roues dentées (**34 px, sous la cible de 44 px**) | dernière ligne de chaque volet, « Tous les réglages › » |
| Mode pH/ORP, consigne pH, consigne ORP, 2× Sauvegarder, Volume quotidien, Limite | volet « Modifier », **ids inchangés** |
| Filtration : 4 modes, Début/Fin, Sauvegarder, bloc Boost | volet « Modifier » |
| « Mesure actuelle » (Température) | supprimé |
| « Mode Auto » en texte gris | devient la pastille |

**Reste visible en permanence** (alertes et sécurité, hors volet) : `dose-block-banner`,
`rescue-active-banner`, `calib-chip`, bandeaux d'étalonnage et d'escalade pH,
`calibration-alert`, `sensor-badge`, `dashboard-*-stopped-hint`.
**Aucune fonction n'est supprimée.**

### ⛔ À faire valider par `pool-chemistry` avant livraison
1. **Mode pH/ORP (gate C5)** : le *texte* du mode reste permanent mais **se déplace** de la
   ligne sous le titre vers la pastille, mots identiques. Le *sélecteur* passe derrière
   « Modifier ». L'agent juge que la protection est renforcée — **c'est à `pool-chemistry`
   de le dire**, pas à lui.
2. `#ph-gauge-state` / `#orp-gauge-state` / `#temp-gauge-state` : inchangés sur le fond,
   typographie seule (13 px/700). Signalé pour mémoire.

### Grammaire d'état unique — une pastille par carte
La couleur ne vit que dans la **puce de 8 px** ; le texte est toujours `--text`, donc
**aucun verdict porté par la couleur seule** (daltonisme).

| Carte | Textes | Puce |
|---|---|---|
| Filtration | En marche / Arrêtée / Hors gel | vert / gris / ambre |
| Éclairage | Allumée / Éteinte (⚠️ désormais **toujours** visible) | vert / gris |
| pH et ORP | Mode Auto / Mode Programme / Mode Arrêt | vert / vert / gris ; ~~**ambre** si dosage bloqué ou étalonnage~~ ⛔ **RÈGLE INVALIDÉE — voir B1 de l'avis `pool-chemistry` plus bas** : elle rallumerait l'ambre toutes les nuits |
| Température | À jour / En attente / Sonde absente | vert / gris / ambre |

Une exception (stock faible, étalonnage, sonde) est une **bande** sous la mesure
(`sensor-badge`, existant), **jamais une seconde pastille**.
Largeur vérifiée à 390 px (utile 326 px) : « Température » + « Sonde absente » = 272 px.
⛔ Libellés plus longs interdits dans la pastille.

### Jetons chiffrés
Globaux (fond de page seul, tous écrans) :
`--bg-gradient-start: #d9e2fb` · `--bg-gradient-end: #e6e3f8`
⚠️ Le pied de page en `--muted` tombe à **~4,9:1** sur ce fond — juste AA, à revérifier.

⛔ **Ne pas toucher `--card-radius`, `--shadow`, `--card-border`** : ils servent aux
**43 cartes** de toute l'application. Portée tableau de bord uniquement :
`--dash-radius: 22px` · `--dash-shadow: 0 10px 30px rgba(31,41,90,.10), 0 2px 6px rgba(31,41,90,.06)`
`--dash-pad: 18px` · `--dash-chip-bg: #f1f3f7` · `--dash-dot-ok: #15803d`
`--dash-dot-off: #6b7280` · `--dash-dot-warn: #b45309`
`--hero-size: 48px` · `--hero-size-sm: 40px` (≤ 360 px) · `--hero-weight: 800`
⚠️ **PÉRIMÉ depuis la v2.96.2** — ces deux chiffres sont ceux proposés par l'étude du
2026-11-18. Le verdict D2 du 2026-11-21 les a ramenés à **`--hero-size: 32px`, valeur
unique**, et `--hero-size-sm` **n'existe plus** (voir « Verdict D2 » en fin de fiche).
⛔ Ce bloc est conservé tel quel comme **trace datée de l'étude**, pas comme état courant.
Le document vivant qui dit le vrai est `docs/DESIGN_SYSTEM.md`.

Mesure : `tabular-nums`, **alignée à gauche**, unité 16 px/600. Jauge : piste 8 px, liseré
supprimé, **curseur bordé `--text`** (corrige le bleu-sur-bleu en haut d'échelle du pH).
Les deux dégradés (pH évaluatif, température descriptif) **ne bougent pas**.

**Couleur des commandes** : segment sélectionné = **encre** (fond `--text`, texte blanc) ;
action = `--accent` ; **au plus une action bleue par carte** (⇒ « Activer » du Boost passe
en neutre). Verrou d'étalonnage : garde son fond `--warn`.

### Volet « Modifier »
`<button class="card__fold" aria-expanded aria-controls>` pleine largeur, 44 px, chevron
en **caractère de texte**. ⚠️ **Le rendu `/data` / WebSocket ne doit JAMAIS replier un
volet ouvert** : bascule au clic seulement, jamais dans les fonctions `render*`.
Tous les ids de contrôles restent inchangés ⇒ les `render*` ne changent pas.

Particularités : **Éclairage** garde ses 3 segments visibles (pas de mesure) ;
**Filtration** promeut l'horaire en ligne héros (32 px/800) — ⚠️ cela **amende
DESIGN_SYSTEM §5.20** ; **Température** n'a pas de volet, juste « Réglages › » à 44 px.
⚠️ **Format des nombres** : l'interface affiche aujourd'hui « 7.3 » et « 24.8 » — virgule
française attendue, par **un seul formatteur partagé** entre `/data` et WebSocket (piège
de parité à 4 récidives).

### Les trois états limites
- **Hors ligne** : héros « – », pastilles « Hors ligne » (puce grise) — sauf pH/ORP qui
  **gardent « Mode … »** (texte gaté C5) ; volet désactivé ; jauge `--stale`.
- **Mesures non représentatives** : héros « – », pastille « En attente », jauge `--stale`
  **+ le mot d'état conservé** (gate), bande « Filtration arrêtée ».
- **Sonde absente** : héros « – », **jamais une valeur figée** ; pastille « Sonde absente »
  (ambre) ; `sensor-badge` porte le lien de réparation.
- **Chargement initial** : pastille « Chargement… » grise, héros « – ».

### Responsive
≤ 360 px : héros 40 px, contexte en retrait. ≥ 900 px : `repeat(auto-fit, minmax(340px,1fr))`,
gap 18 px, carte plafonnée à 560 px. Pas de défilement horizontal à 320 px (`min-width: 0`,
titre en ellipse, pastille `nowrap`).

### Inventaire d'impact des composants partagés
| Composant | Emplois | Décision |
|---|---|---|
| `.card--status` | 5, tous sur l'accueil | libre |
| `.card__status-*`, `.compact-*` | ~35 dans `index.html` + 2 dans `app.js` | ⚠️ **le développeur refait ce grep** ; si un emploi hors accueil apparaît, **restreindre à `.card--status`** |
| `.state-badge` | 5 HTML + émissions JS (l. 3981, 4859, 5344, 5586, 12524) | restyle **scopé** `.card--status` |
| `.mode-select` | 7, dont **3 sur les pages de réglage** | restyle scopé ⇒ accueil (encre) ≠ réglage (bleu) le temps du pilote — **contexte d'emploi à déclarer dans DESIGN_SYSTEM**, pas une troisième forme |
| `.segmented--num` | 3 (2 accueil, 1 réglage) | scopé |
| `.card__option` | 1 (Boost) | scopé |
| `.settingsHead__sub` | partagé | retirer **le `<p>` du tableau de bord**, pas la règle CSS |

### Choix visuels que je n'avais pas confiés à l'agent — et qu'il a tranchés
Palette et curseur de jauge · rôle de la couleur · suppression des survols · cibles
tactiles (34 → 44 px) · format des nombres et `tabular-nums` (pour que le chiffre ne
« saute » pas toutes les 30 s) · `<section>` + `<h2>` par carte, **sans `aria-live` sur le
héros** (il changerait toutes les 30 s ; `aria-live` reste sur les bandes d'exception) ·
`prefers-reduced-motion` (animation d'en-tête + transition du curseur) · **mode sombre :
non recommandé dans ce lot**, spec séparée si souhaité.

### Réserve de méthode
⚠️ L'agent déclare **ne pas avoir lu `dash-nominal-1024.png`**. Les choix ≥ 900 px ci-dessus
sont donc **déduits, pas observés** — à vérifier sur capture avant de les figer.

---

## ⚖️ ARBITRAGE UTILISATEUR — 2026-11-18

Question posée : faut-il replier les commandes des cartes, comme le recommande l'étude,
alors que l'utilisateur avait demandé le contraire le 2026-10-13 ?

**Réponse : « Cacher celles qu'on règle rarement. »**

⇒ **Ni l'option C intégrale, ni le statu quo.** La règle devient :

| Carte | Commandes |
|---|---|
| **pH** | repliées derrière « Modifier » (mode, consigne, Sauvegarder) |
| **ORP** | repliées derrière « Modifier » (mode, consigne, Sauvegarder) |
| **Filtration** | ⚠️ **Marche / Arrêt restent visibles** — c'est ce qu'on touche au bord du bassin. Le reste (Auto, Programme, horaires, Boost) se replie. |
| **Éclairage** | ses segments restent visibles (déjà prévu par l'étude : pas de mesure à mettre en avant) |
| **Température** | rien à régler — ligne « Réglages › » |

**Motif retenu** : la demande d'octobre visait le pilotage **au bord du bassin**. Allumer
la lumière et lancer la filtration en relèvent ; changer un mode de régulation ou une
consigne pH, non — c'est rare et conséquent. L'esprit de la demande d'octobre est donc
préservé là où il a de la valeur, et la mesure gagne sa place là où les commandes ne
servaient qu'occasionnellement.

⚠️ **Point ouvert confié à `ux-ui-designer`** : la carte Filtration porte **un seul
sélecteur à 4 modes** (Auto · Programme · Marche · Arrêt). En montrer deux et en replier
deux **coupe un composant en deux** — ⛔ exactement le genre de décision qui fabrique une
troisième forme. C'est à l'étude de trancher la présentation, pas au développeur.

---

## ⭐ COMPLÉMENTS D'ERGONOMIE — 2026-11-18 (second passage)

### La carte Filtration — forme retenue
**Deux actions de dérogation sur la face, le sélecteur à 4 modes ENTIER dans le volet.**
⇒ Le composant n'est **jamais coupé** : ses quatre boutons restent ensemble.
Les deux actions de la face sont **un autre geste** — une dérogation par-dessus le mode —
qui passe par le **même** `setFiltrationRunMode()`.

Écartés, avec leur motif : l'**interrupteur** (il annonce un état alors qu'on change un
*mode*, et laisse « Auto » sans sortie visible) ; le **bouton unique à libellé variable**
(ambigu en Auto hors créneau) ; **2 segments sur 4 visibles** (la coupe qu'on voulait éviter).

```
Auto ou Programme                       Forcée (Marche ou Arrêt)
┌───────────────────────────────┐       ┌───────────────────────────────┐
│ [◉] Filtration  ● En marche   │       │ [◉] Filtration   ● Arrêtée    │ puce AMBRE
│ 09:10 – 16:50                 │       │ Arrêt forcé                   │ héros 28 px
│ Mode Auto                     │       │ Horaires suspendus            │ --warn, 13 px
│ [Forcer marche][Forcer arrêt] │       │ [Forcer marche][■Forcer arrêt]│
│ Modifier                   ▾  │       │ [   Revenir en Auto         ] │ 44 px
└───────────────────────────────┘       │ Modifier                   ▾  │
                                        └───────────────────────────────┘
```
Caractères comptés à 390 px (utile 326, colonnes de 159) : « Forcer marche » 13,
« Forcer arrêt » 12 ≈ 105 px — tient, **et tient encore à 320 px** (colonnes ~140 px).
« Revenir en Auto » 16, « Horaires suspendus » 19, héros forcé ≈ 190 px — tiennent.

**Règle d'écriture ajoutée à la grammaire d'état** :
**puce grise = arrêt normal du mode · puce AMBRE = état imposé par l'utilisateur.**
En Auto/Programme, **aucun** des deux boutons n'est sélectionné (`aria-pressed="false"`).
« Revenir en Auto » n'apparaît **que** en mode forcé : Auto n'étant plus sur la face,
sans ce bouton l'utilisateur n'aurait **aucun chemin de retour**.

⚠️ **Parité (piège à 4 récidives)** : Marche et Arrêt existent à deux endroits — même
donnée, **jamais deux sources**. `updateFiltrationModeButtons()` met à jour **les deux
jeux de boutons dans la même fonction**, et `updateDetailSections()` pilote le héros forcé
et « Revenir en Auto » avec **la même condition** (`mode === "force" || mode === "off"`).

### Conséquences fonctionnelles — nommées, pas seulement visuelles
1. **Forcer sort du mode Auto ou Programme, et rien n'y ramène tout seul** : pas de
   minuterie, le forçage est conservé tel quel.
2. **Arrêt forcé ⇒ dosage pH/ORP bloqué** et mesures non représentatives. Oublié
   plusieurs jours en été, l'eau se dégrade **sans alarme propre à la filtration**.
3. **Marche forcée** : pompe en continu, planning et logique de température ignorés.
4. ⚠️ **« Revenir en Auto » ramène en Auto, jamais en Programme.** Qui était en Programme
   doit le rechoisir dans le volet ; les horaires enregistrés **ne sont pas effacés**.
   C'est pourquoi le libellé dit « Revenir en Auto » et non « Annuler ».
   ⛔ Pas de mémorisation du mode précédent côté navigateur : perdue au rechargement,
   elle donnerait un comportement incohérent.
5. ⛔ **Aucun nouveau chemin vers `pump_controller`** : même appel qu'aujourd'hui.

### ⛔ Point #5 de l'étude — TRANCHÉ PAR LECTURE DU CODE, pas par avis
L'agent demandait de faire confirmer *« ce que l'Arrêt forcé fait à la pause hors gel »*.
**Vérifié le 2026-11-18 : il n'y a RIEN à interrompre, parce qu'il n'existe aucune
protection hors gel.**
- `data/app.js:5391` — « Hors gel » est un **libellé calculé dans le navigateur** :
  `temperature < 5.0 **ET** filtration déjà en marche`. Il **décrit**, il ne commande rien.
- `src/filtration.cpp:281` — sous `kFiltrationRefTempMinC`, le firmware journalise
  « Hivernage » et **conserve le planning**. Il ne démarre aucune pompe.
- ⇒ Le forçage à l'arrêt **ne dégrade aucune protection existante**, et le nouveau dessin
  **n'aggrave rien** : le bouton « Arrêt » est **déjà** à un seul tap sur la carte
  d'accueil d'aujourd'hui (sélecteur à 4 modes visible). Le dessin proposé rend au
  contraire l'état forcé **bien plus visible** (héros, puce ambre, « Horaires suspendus »,
  sortie explicite).
- 📌 **Constat à sortir en fiche** : le libellé « Hors gel » laisse croire à une protection
  qui n'existe pas. C'est un sujet à part, hors périmètre de feature-195.

### Éclairage — la même question, en plus léger
Allumé et Éteint sortent aussi de Programme, sans minuterie. **Décision : ne rien couper.**
Les 3 segments restent ensemble et visibles — c'est tout le contenu de la carte, et
« Programme » **est** la sortie, toujours à l'écran (donc pas de bouton « Revenir »).
Ajout d'une ligne de contexte 12 px `--text-secondary`, **uniquement** si le mode n'est pas
Programme **et** qu'un horaire existe : « Programme suspendu » (19 car., tient).
⛔ **Pas de puce ambre ici** : Allumé/Éteint est le geste principal de cette carte, pas une
dérogation.

### Grille ≥ 900 px — réserve de méthode LEVÉE
L'agent a lu `dash-nominal-1024.png` et **annule sa propre recommandation** :
- ⛔ `minmax(340px,1fr)` et le plafond de 560 px sont **abandonnés** — à 1024 px le menu
  latéral prend ~280 px, la zone utile tombe à ~740 px, et 2×340 + gap **basculerait en
  une colonne**.
- ✅ **Ne rien changer à la grille** : `repeat(auto-fit, minmax(320px, 1fr))` et le gap
  actuel sont conservés. Les cartes font déjà ~345 px, soit la largeur du mobile.
- Observé : une fois les volets repliés, les paires Filtration|Éclairage et pH|ORP
  s'équilibrent (aujourd'hui Filtration dépasse Éclairage de ~330 px).
- À vérifier en capture par le développeur : que la carte Température, seule en bas à
  gauche, ne déséquilibre pas la grille.

### À porter dans le design system
- Composant **« bouton de dérogation »** — contexte d'emploi : **carte d'accueil uniquement**.
- Règle **« puce ambre = état imposé par l'utilisateur »**.
- Le consensus **« forçage persistant, sortie = Auto »** comme texte du guide utilisateur.

---

## ⚗️ AVIS `pool-chemistry` — 2026-11-18 : **GO SOUS CONDITIONS** (6 conditions bloquantes)

**Le repli n'est pas ce qui bloque.** Sur l'axe « arrêt accidentel du dosage », il
**améliore** la situation. Ce qui bloque, c'est la **pastille ambre** et un **texte gaté**.

### Deux précisions sur le gate C5 — elles changent le test à appliquer
Texte d'origine (`feature-161`, l. 360-363) : *« C5 — AUCUNE autre confirmation. Les
plafonds protègent ; la friction uniforme est un anti-pattern ici. **La protection contre
l'arrêt accidentel est la VISIBILITÉ permanente du mode sur la carte, pas un modal.** »*

1. ⭐ **C5 n'est pas une exigence autonome, c'est une CONTREPARTIE.** Le gate a renoncé au
   modal *parce que* la visibilité permanente existe. ⇒ Le bon test n'est **pas** « le
   texte est-il encore là ? » mais : **après un changement de mode, l'utilisateur le
   remarque-t-il sans faire aucun geste ?**
2. ⭐ **Le sélecteur est précisément ce que le gate a refusé de considérer comme
   protecteur.** La ligne de mode existe *pour compenser* le fait que l'état actif du
   sélecteur se voit mal. ⇒ **Le sélecteur n'est pas protégé par C5**, son repli n'est pas
   un retrait déguisé.

### Verdict sur le déplacement du texte de mode vers la pastille
**Préservé — ⛔ « renforcé » N'EST PAS ratifié.** L'argument de l'agent d'ergonomie est une
déduction sur maquette, pas une observation ; `pool-chemistry` n'a vu aucun rendu. Ce qui
est acquis : mots identiques, visibilité permanente, écriture unique — suffisant pour C5
**sous B5**, et à condition que « Mode Arrêt » ne soit **jamais** tronqué, ellipsé, ni
réduit à la seule puce. ⇒ **Le vrai renforcement est ailleurs, et lui est mesurable : c'est
le tap supplémentaire.**

### ⛔ CONDITIONS BLOQUANTES

**B1 — La puce ambre ne peut PAS être pilotée par « dosage bloqué » au sens large.**
⚠️ **Vérifié dans le code le 2026-11-18, la règle proposée par l'étude est fautive** :
`data/app.js:8367` classe le code 2 « Filtration arrêtée » en tone `normal`, et
`app.js:8429-8438` porte un **verdict `pool-chemistry` antérieur explicite** : *« les
passer orange viderait l'orange de son sens — refusé explicitement par le gate »*.
⇒ « ambre si dosage bloqué » rallumerait l'ambre **toutes les nuits, sur les deux cartes**,
plus sur les codes 7, 8, 10, 11, 12, 13 — tous « marche attendue ».
**Règle retenue** : ambre ⟺ `doseBlockVariant(...) === 'danger'` (tones `fault` /
`unknown`), en **réutilisant** `DOSE_BLOCK_CODES` / `_doseBlockInfo` / `doseBlockVariant`.
⛔ Pas de seconde table de codes côté pastille. Si l'étalonnage doit aussi passer en ambre,
c'est une **exception unique et nommée**, pas un « ou ».
*Vérifiable* : `-f phDoseBlockedCode=2` → puce **inchangée** ; `=10` → inchangée ;
`=13` → inchangée ; `=1` (watchdog inactif) → **ambre**.
⚠️ Rappel : `feature-161` (l. 460-461) avait tranché *« ⛔ ne pas ajouter de badge d'état à
pH/ORP »*. La puce rouvre ce point — permis, mais l'exception doit être **étroite et écrite**.

**B2 — `#dashboard-ph-stopped-hint` / `#dashboard-orp-stopped-hint` restent HORS du volet,
et AU-DESSUS du bouton « Modifier ».** Texte **imposé mot pour mot** par C7 : *« La
régulation automatique ne dose plus. L'injection manuelle reste possible depuis les
réglages. »* (`index.html:463` et `:615`). Il a été **délibérément placé avant le
sélecteur** le 14/10 pour que cause et conséquence se lisent ensemble.
⇒ Sans lui hors volet, une carte en Arrêt ne dirait plus que « Mode Arrêt » et **plus un
mot sur ce que ça implique** — précisément le scénario que C5 couvre.
📌 *Note de méthode* : la spec le portait bien (« Reste visible en permanence »), mais
**mon brief à `pool-chemistry` l'avait omis de son tableau**. L'omission était dans le
brief, pas dans la conception — elle a quand même failli coûter la condition.

**B3 — Le volet est strictement piloté par le clic.** ⛔ Aucune fonction `render*` ne
modifie son état ; le bouton « Modifier » n'est **jamais** `disabled`, y compris sous
escalade pH ou étalonnage — sinon l'escalade rendrait « Arrêt » inatteignable, ce que
**C3** interdit. À l'ouverture dans ces états : Auto/Programme `disabled` + `aria-disabled`
(⛔ jamais `hidden`, C3), « Arrêt » cliquable, consigne `disabled` (⛔ jamais masquée, C10).

**B4 — `#rescue-active-banner` et `#dose-block-banner` restent au niveau de la VUE**, hors
carte et hors volet. Interdiction déjà écrite dans `index.html:183-186` : *« NI MASQUABLE
NI REPLIABLE — un dosage aveugle de 48 h ne doit pas pouvoir être mis hors de vue d'un
clic »*. ⇒ `.card__fold` ne les contient ni ne les enveloppe. *Vérifiable par grep.*

**B5 — Un seul écrivain pour la pastille pH/ORP** : `renderDashboardModeCard()`, ids
`detail-ph-mode` / `detail-orp-mode` **conservés** (lus en `app.js:3783`). ⛔ Aucun second
calcul du libellé ni de la puce dans `updateStatusCards` ni dans le chemin WebSocket.
⚠️ Piège de parité à 4 récidives, dont la 4ᵉ forme était **invisible au grep** (même clé,
expressions divergentes).

**B6 — Si la consigne monte dans la ligne de mesure** : lecture seule, **même source**
(`window._config[*_target]`), **un seul formateur partagé**, et **même règle de visibilité
que `#orp-gauge-target` — mode Auto SEULEMENT**. Motif : `feature-161` a retiré la cible
hors Auto parce que *« rien n'est poursuivi »* ; afficher « consigne 7,2 » en Mode Arrêt
ferait **affirmer à la carte une cible que l'appareil ne poursuit pas**.
⚠️ Cela **amende DESIGN_SYSTEM §5.20** — à déclarer, pas à faire en silence.

### Recommandations à acter (non bloquantes)
- **R1** — Acter par écrit la **perte de visibilité permanente** de
  `#dashboard-ph-target-warning` (texte C9, consigne à la borne haute 7,50 : *« à ce pH le
  chlore est nettement moins actif »*). Il est aujourd'hui **visible en permanence** sur
  l'accueil, car `renderPhTargetWarning` est appelé au chargement de la config, pas
  seulement à l'édition. Il **informe**, il ne protège pas (« les plafonds protègent, le
  reste informe ») ⇒ non bloquant, mais `renderPhTargetWarning` doit **continuer de
  s'exécuter volet replié**.
- **R2** — En mode Programme, « Volume quotidien » et « Limite de sécurité » partent dans
  le volet alors qu'ils disent **ce que l'appareil va faire**. Envisager de remonter le
  volume quotidien en ligne de contexte.
- **R3** — ⚠️ Hors ligne, pH et ORP gardent « Mode Auto » : **aucun mot « Hors ligne » sur
  ces deux cartes**. Le seul porteur est le bandeau global — **et [bug-196](../todo/bug-196-le-bandeau-hors-ligne-passe-sous-le-bouton-menu.md)
  dit qu'il passe sous le bouton menu.** Une carte affichant « Mode Auto » sur une mesure
  figée se lit « tout va bien ». ⇒ **Vérifier sur capture, ou livrer bug-196 d'abord.**
- **R4** — Puce verte pour « Mode Programme » : défendable, le mot reste porteur.
- **R5** — Volet ouvert + changement de mode ⇒ le volet reste ouvert. Conforme à B3,
  à tester : c'est le chemin naturel d'un arrêt délibéré.

### ⚠️ POINT OUVERT que l'avis ne tranche pas — l'ambre a maintenant TROIS sens
| Carte | Ambre signifierait |
|---|---|
| Filtration | « état imposé par l'utilisateur » (forçage) — proposé par l'étude |
| pH / ORP | « panne ou mesure en cause » (tones `fault`/`unknown`) — imposé par B1 |
| Filtration, aujourd'hui | « Hors gel » (`state-badge--warn`, `app.js:5391`) |
⛔ **À résoudre avant implémentation**, sinon la couleur ne veut plus rien dire — c'est
exactement le défaut que B1 cherche à éviter. ⇒ À confier à `ux-ui-designer`.

### Ce que `pool-chemistry` a vérifié et jugé intact
Chemins d'écriture inchangés (`applyRegulationMode` → `/save-config`), confirmation C4 dans
le **chemin partagé** (donc le repli ne la contourne pas), gardes serveur 409 intactes,
aucun seuil, aucune limite, aucun PID touché, `#ph-gauge-state` / `#orp-gauge-state` hors
volet. Déduplication `doseBlockBannerCarries()` : ⛔ ne pas déplacer `sensor-badge` ni
toucher à cette logique — un déplacement pourrait laisser une cause **affichée nulle part**.

### Plan de test de sécurité (à exécuter avant livraison)
Tous avec `./tools/ui_shot.sh -n`, à **390 px**, volet **replié** sauf mention.
1. `-c ph_regulation_mode=manual` → « Mode Arrêt » **et** texte C7 lisibles sans geste (B2).
2. `-f phDoseBlockedCode=2 -f filtration_running=false` → bandeau visible, **puce NON ambre** (B1).
3. `-f phDoseBlockedCode=1` → ambre ; `=10` et `=13` → **non ambre** (B1).
4. `-c ph_auto_manual_reason="…"` → « Modifier » actionnable ; à l'ouverture Auto/Programme
   verrouillés, « Arrêt » cliquable (B3, C3).
5. `-f cal_switch_active=true` → consigne `disabled`, **non masquée** (C10).
6. `-f rescue_until=<futur>` → bandeau hors carte, « Arrêter » atteignable **sans déplier** (B4).
   Puis ORP → Arrêt : confirmation C4, « Annuler » **repose le sélecteur sur le mode
   confirmé** (régression de feature-161 à ne pas rejouer).
7. **Sans `-n`** → ce qui, sur pH/ORP, dit que la mesure n'est pas fraîche (R3).
8. ⭐ **Mutation exigée de `test-engineer`** : faire passer un `tone:'normal'` en ambre doit
   **tuer** un test. **Si rien ne meurt, B1 n'est pas protégée.**

---

## ✅ GRAMMAIRE DE PUCE — ARRÊTÉE (2026-11-18, après B1)

`ux-ui-designer` **retire** sa règle « ambre si dosage bloqué ou étalonnage » et s'aligne
sur B1. **Sens unique, pour les quatre cartes :**

> **Ambre = l'appareil s'écarte de son fonctionnement automatique attendu, et ça demande
> un regard.**

| Couleur | Sens | Exemples |
|---|---|---|
| **Vert** | fonctionnement attendu en cours | En marche · Mode Auto · Mode Programme · À jour · Allumée |
| **Gris** | repos ou information normale | Arrêtée · Mode Arrêt · En attente · Éteinte · **Hors gel** |
| **Ambre** | écart à regarder | pH/ORP : `doseBlockVariant === 'danger'` (tones `fault`/`unknown` **seulement**) · Filtration : mode **forcé** · Température : **sonde absente** · Éclairage : **jamais** |

⛔ **Ce qui n'est PAS ambre** : filtration arrêtée sur pH/ORP (codes 2, 7, 8, 10, 11, 12,
13) · étalonnage en cours (il a son propre bandeau, et ce n'est pas une panne) · « En
attente » · arrêt normal d'Auto hors créneau.
⛔ Aucune autre teinte, **pas de rouge**. Le sens ne dépend **jamais** de la couleur seule :
chaque état ambre a son texte juste à côté.

### « Hors gel » passe en puce GRISE — et le motif mérite d'être écrit
Décision : **puce grise**, texte et logique **inchangés** (c'est `bug-197` qui les traitera).
⇒ Retirer la correspondance `state-badge--warn` → ambre **pour ce seul cas** dans le rendu
de la carte Filtration (`app.js:5391`). La teinte `.card__status-mode--warn` ne sert plus
sur l'accueil (le mode n'est plus une ligne sous le titre) : à retirer de ce rendu.

⚠️ **Et ce n'est PAS une perte de signal — c'est l'inverse.** Vérifié : la condition est
`temperature < 5.0` **ET** `filtration_running`. L'ambre d'aujourd'hui ne s'allume donc que
quand **la pompe tourne**, c'est-à-dire dans le cas **favorable** (l'eau circule par temps
froid). Le cas **dangereux** — eau froide, pompe à l'arrêt — n'a **jamais** porté le
moindre signal. ⇒ Passer cette puce au gris ne retire rien ; le vrai défaut est l'absence
de signal dans l'autre sens, et c'est l'objet de
[bug-197](../todo/bug-197-le-libelle-hors-gel-annonce-une-protection-qui-n-existe-pas.md).

### B6 confirmé par l'agent d'ergonomie
Sur pH et ORP, la consigne à droite du héros (« consigne 7,2 », « consigne 530 mV »)
ne s'affiche **qu'en mode Auto**. En Programme et en Arrêt, la zone est **vide** et le
héros occupe seul la ligne. ⇒ Aucune cible affirmée là où l'appareil n'en poursuit pas.

### R3 — levée par la conception elle-même
L'avis craignait qu'une carte hors ligne affichant « Mode Auto » se lise « tout va bien ».
⇒ **Le dessin retenu l'empêche déjà** : hors ligne, le héros devient « – » et la jauge
passe en `.gauge--stale`. Une carte dont la mesure est un tiret ne se lit pas « tout va
bien ». La pastille conserve « Mode … » (texte gaté C5), avec une puce grise.
⇒ `bug-196` reste **utile et indépendant**, mais **n'est plus un préalable** à ce lot.

### Correction acceptée par l'agent d'ergonomie
Il retire son « protection renforcée » : la protection est **préservée**, et le gain réel
est le **tap supplémentaire**. Son affirmation initiale était une déduction sur maquette.


---

## 📍 ÉTAT AU 2026-11-18, FIN DE JOURNÉE

| | |
|---|---|
| `pio run` | **SUCCESS** en 18,6 s après le passage de `src/version.h` à **2.96.0** |
| `./build_fs.sh` | **SUCCESS**, 512 Ko |
| Gardes d'hygiène | **27 vertes**, dont le neuf `check_dash_pill_vectors.sh` (46 vérifications) |
| Mutation B1 | ✅ passer le code 2 de `normal` à `fault` **tue** un test |
| Hauteur de page à 390 px | **1 968 → 1 433 px** (mesurée sur les deux captures, pas estimée) |

### Vérifications que j'ai faites moi-même, sans les déléguer
- **Fausse alerte levée** : le développeur signalait « Éteint » en gris clair à 1024 px, en
  *supposant* une capture prise en pleine transition. Recapturé : c'en était bien une, le
  bouton est en encre. ⇒ **Aucun défaut.** (Il avait eu raison de le signaler sans trancher.)
- **État forcé photographié** — personne ne l'avait vu : pastille ambre, « Arrêt forcé »,
  « Horaires suspendus », « Revenir en Auto ». Conforme.
- **« Commandes désactivées hors ligne »**, écrit par `doc-writer` d'après la spec sans
  relire le code : **vérifié vrai**, `app.css:3873-3876`.
- **« Le guide décrit des graphiques absents »** : **vérifié vrai**, zéro `<canvas>` dans
  `data/index.html`. ⇒ [bug-198](../todo/bug-198-le-guide-client-decrit-des-graphiques-absents-du-tableau-de-bord.md),
  ⚠️ dérive **antérieure** à ce lot (commits `cf5fe33`, `4dd8071`).

### Déploiement du 2026-11-18 — fait et vérifié

⚠️ **`./deploy.sh ota-all` N'A PAS été employé** : il **reconstruit** le firmware **et** le
système de fichiers avant d'envoyer (`deploy.sh:297-301`). Les deux binaires étaient **déjà
signés** — une reconstruction aurait changé leur empreinte et **invalidé les signatures**,
exactement l'incident du 2026-11-14 qui a détruit l'interface.
⇒ Commande réellement passée : **`./ota_update.sh --use-sig both 192.168.1.48`**, qui envoie
sans recompiler et **relit** les `.sig` au lieu de re-signer.

| Contrôle | Résultat |
|---|---|
| `check_release_artefacts.sh` | signatures vérifiées contre la clé **embarquée dans `src/constants.h`** |
| Empreintes des manifestes | identiques aux `sha256sum` réels des deux binaires |
| Envoi firmware + système de fichiers | ✅ les deux acceptés |
| Version annoncée par l'appareil | **2.96.0** (`/get-system-info`), plus de décalage |
| Interface réellement servie | `card--status` ×5, `dash-` ×60, `--bg-gradient-start:#d9e2fb`, `_fmtPh` ×17 |
| Journal après redémarrage | 29 INFO, **0 erreur**, 0 avertissement |

⚠️ **Une ligne CRITIQUE au démarrage, SANS rapport avec ce lot** : « Correction de mesure pH de
**+1,20** relue en mémoire (au-delà de 0,50) ». C'est l'alerte voulue depuis la v2.82.0, et la
valeur **a dérivé** — elle était mesurée à ~1,0 le 2026-10-16. À rapprocher de l'usure connue de
la sonde, pas du tableau de bord.

### Reste à faire
- [x] Verdict `code-reviewer` — *Request changes*, 3 points, **tous traités**
- [x] **D2 — Essai réel sur l'appareil** : jugé le **2026-11-21** sur le téléphone ET sur
      l'ordinateur. ⇒ **Un seul reproche, et il est traité** (voir « Verdict D2 » plus bas).
      ⏳ Reste à **déployer la 2.96.2** qui porte le correctif, puis à reconfirmer d'un coup d'œil.
- [ ] 📸 **Étape humaine** : `screenshots/03_Tableau_de_bord.jpg` est **entièrement périmée**
      (obligatoire). À juger : 04 à 08 montrent les pages de réglage, non touchées, mais le
      fond de page a changé sur **tous** les écrans — un écart de teinte est possible.
      `login.html`, `wifi.html` et `wizard.html` gardent l'ancien fond (captures 01 et 02
      inchangées).
- [x] Point mineur relevé sur capture : le point décimal de « Stock faible (0.7 L) ». ⇒ **Sorti
      du périmètre et doté de sa propre fiche** par la revue de code :
      [bug-199](../todo/bug-199-deux-conventions-decimales-sur-le-meme-ecran.md). ⚠️ La capture
      du 2026-11-21 le montre **deux fois sur le même écran** (pH « Stock faible (0.3 L) » et
      ORP « (0.0 L) ») — à verser à bug-199, qui avait déjà relevé que le cas n'est pas isolé.
- [ ] R1 (perte de visibilité permanente de l'avertissement C9) et R2 (volume quotidien)
      **actés comme non traités**, écrits dans le CHANGELOG.

---

## 🔍 REVUE DE CODE — 2026-11-18 : **Request changes**, 3 points bloquants, tous traités

La revue confirme **B1 à B5 sur le fichier**, pas sur la description, et relève **B6 partielle**.

### ✅ Bloquant 1 — un second formateur pour la consigne pH (B6) — **CORRIGÉ**
`app.js` écrivait le héros avec `fmtFr(Math.round(tv*100)/100, 2)` alors que
`buildPhTargetSegments` écrit **le même nombre**, dans **la même carte**, de l'autre côté du
volet, avec `_fmtPh()` — **le** formateur pH partagé.
⇒ Divergence sur une consigne **hors grille**, cas que le code **nomme lui-même**
(`setPhTarget` : *« une consigne héritée hors grille (7,35 d'une configuration
antérieure) »*) : héros « consigne 7,35 » contre segment « 7,4 ».
⚠️ **C'est la 4ᵉ forme du piège de parité** — même clé, expressions divergentes.
**Correctif** : `"consigne " + _fmtPh(tv, false)`.
**Vérifié sur capture** avec `-c ph_target=7.35` : héros et segment affichent **tous deux
7,3**. Le `.replace(/,$/, "")` mort disparaît avec.

### ⚠️ Bloquant 2 — la pastille Éclairage pourrait mentir — **GARDE POSÉE, mais le défaut N'ÉTAIT PAS ATTEIGNABLE**
La revue signalait qu'avec `lighting_feature_enabled` à faux, `src/lighting.cpp:56-63`
force la broche à LOW et sort **avant** `decideLightingOn()` — donc « Mode Allumé »
afficherait « Allumée · puce verte » **lampe éteinte**. Le firmware a été vérifié : **c'est
exact**, et `lighting_enabled` est bien absent de `/data` (0 occurrence) alors qu'il existe
dans la trame WebSocket (2).

⛔ **MAIS la revue se trompe sur l'atteignabilité, et je l'ai constaté sur capture avant de
livrer son correctif.** Quand la fonction éclairage est désactivée, `updateFeatureVisibility`
pose `display:none` sur **toute la carte** `#dashboard-lighting-card` (`app.js:2231-2234`),
et l'appel se fait **dans la même fonction** qui écrit la pastille (`app.js:4068`). ⇒ La
pastille fautive **n'est jamais visible**.

**Décision : la garde est conservée quand même**, pour deux motifs — (1) la **prémisse** de
la déduction (« le relais suit le mode ») était fausse telle qu'écrite, et une prémisse
fausse finit par être réutilisée ailleurs ; (2) elle coûte une branche et un commentaire.
⇒ Et surtout, **la borne est désormais DÉCLARÉE** dans `DESIGN_SYSTEM.md` §4, avec
l'exigence : *toute déduction d'un état à partir d'un mode doit être déclarée, avec la
preuve côté firmware que le mode entraîne bien l'état.*
📌 **À retenir** : la revue avait raison sur le **code**, tort sur la **conséquence**.
C'est exactement pourquoi un verdict ne se recopie pas sans être vérifié.

### ✅ Bloquant 3 — le garde de B1 ne tournait pas en CI — **CORRIGÉ**
`hygiene-specs.yml` **énumère ses jobs un par un**, sans boucle sur `tools/check_*.sh` :
`check_dash_pill_vectors.sh` en était absent, **seul `check_*.sh` du dépôt non câblé**.
⇒ La mutation exigée au point 8 du plan de sécurité ne mourait **qu'en local** : un futur
lot repassant le code 2 en `fault` serait passé **vert** en CI.
**Correctif** : job `dash-pill-vectors` ajouté, YAML validé (33 jobs).

### Mineurs traités
- Commentaire `app.css` renvoyant à `.card__status-label`, classe supprimée par ce lot.
- `DESIGN_SYSTEM` : « **plus** une ligne `.detail-row` » se lisait « en plus de » au lieu de
  « il n'y a **plus de** ». Reformulé.
- **§5.24 complété** : la pastille Éclairage a **quatre** libellés, pas deux — « Programme »
  est le cas où l'état est inconnu **et** indéductible, et la pastille **refuse de deviner**.
- **§5.24, priorité de teinte écrite** : « Hors gel » **l'emporte** sur « forcé ». Une
  **Marche forcée** sous 5 °C affiche « Hors gel · gris » et le signal « imposé par
  l'utilisateur » disparaît. Le cas est atteignable. Ça ne ment pas (les textes diffèrent),
  mais c'est un **choix** : le fait météo prime sur l'origine de la commande. ⇒ À **rejuger**
  si bug-197 retire « Hors gel », au lieu de le reconduire par inertie.

### Mineur laissé, et pourquoi
`.compact-value` / `.compact-unit` ne sont pas scopés `.card--status`. Sans conséquence :
ces classes n'existent que sur les trois héros du tableau de bord. Noté, non corrigé.

### Sorti du périmètre — [bug-199](../todo/bug-199-deux-conventions-decimales-sur-le-meme-ecran.md)
« Stock faible (0.7 L) » garde le **point** décimal à quelques pixels d'un héros à virgule,
et ce n'est **pas une occurrence isolée** : le bandeau de blocage écrit aussi « pH 7.3 ».
⛔ Le balayage contient un piège qui méritait d'être écrit avant d'être improvisé :
`fmtFr` ⛔ **ne doit jamais toucher un `.value` ni un `dataset`** — ce sont des valeurs
machine qu'une virgule casse (`app.js:5007-5008`, `buildPhTargetSegments`). ⇒ Fiche à part.

### Ce que la revue a vérifié et jugé sain
Aucun `id` supprimé de `index.html` ⇒ **aucun sélecteur orphelin** dans les `render*`.
Les 14 classes CSS retirées ont **0 occurrence** restante ; `#temp-mode-rescue` n'existait
déjà plus **avant** ce lot. `--card-radius`, `--shadow`, `--card-border` **intacts**.
Les deux chemins `/data` et WebSocket convergent dans `loadSensorData` ⇒ **la parité des
pastilles est structurelle, pas déclarative**. `phDoseBlockedCode` est émis **toujours**
(`null` si aucun refus) des deux côtés ⇒ le chemin `undefined → unknown → ambre` **n'est pas
atteignable** sur ce firmware : le seul angle mort possible de B1 est fermé.
L'écart d'écrivain (`renderFiltrationFace` au lieu d'`updateDetailSections`) est **accepté
et jugé meilleur** : `updateFiltrationBadges` calcule déjà l'état et les boutons au même
instant, et elle est appelée sur un **surensemble** des occasions.

### ⚠️ À ne pas oublier au déploiement
`src/version.h` passe à **2.96.0** alors que **seul `data/` change**. ⇒ Un
`./deploy.sh ota-fs` **seul** laisserait l'appareil annoncer **2.95.0** avec l'interface
2.96.0. **Déployer `ota-all`.**

### État après correctifs
`pio run` SUCCESS · `build_fs.sh` SUCCESS 512 Ko · **27 gardes vertes** dont
`check_dash_pill_vectors.sh` (46/46) · la seule garde rouge est
`check_release_artefacts.sh`, qui exige une **signature de release** — normal hors release.

### 2026-11-20 — trois retouches CSS postérieures à la v2.96.0 (v2.96.1, `data/app.css` seul)
⚠️ **Mise à jour du 2026-11-21** : la 2.96.1 **a été déployée** le 2026-11-20 (20:23-20:25) par
`./ota_update.sh --use-sig both`, puis renvoyée par la page le 2026-11-21 — c'est elle qui a servi
d'essai d'acceptation à [feature-151](../done/feature-151-signature-de-l-image-ota.md). ⇒ La phrase
« non présentes sur l'appareil » qui figurait ici est **périmée** : ces trois retouches étaient bien
à l'écran lors du jugement D2.
- **Fond de page** : `--bg-gradient-start` `#d9e2fb` → `#c8d3e4`, `--bg-gradient-end` `#e6e3f8` → `#d2d9ea` (ancien fond ~2× trop saturé, teinte 249 vers le violet ; `login/wifi/wizard` déjà corrects). Séparation des cartes blanches : 1,29 → 1,51 (haut), 1,33 → 1,41 (bas).
- **Bandeaux** `.rescue-active-banner` et `.dose-block-banner__row` : `--card-radius` → `--dash-radius`.
- **Pied de page** : `.footer .muted` en `--text-secondary` — contraste 4,54:1 → 5,35:1 sur `#d2d9ea` ; portée limitée (les 53 autres `.muted`, en carte blanche, sont à 7,4:1).

---

## ⭐ VERDICT D2 — 2026-11-21, L'ÉCRAN JUGÉ SUR LE TÉLÉPHONE

L'utilisateur a regardé le tableau de bord **sur son téléphone** et sur l'ordinateur, avec la
2.96.1 à l'écran. ⭐ **Le lot passe.** Un seul reproche, et il porte sur ce que le lot a
délibérément grossi :

> « La taille de la police pour la valeur de pH, température et ORP est même un peu grande.
> On pourrait adopter la taille de la plage horaire de la filtration. »

⭐ **À lire pour ce qu'il dit** : la règle fondatrice de la fiche — *« la mesure est l'élément le
plus grand de la carte, sans concurrence »* — est **atteinte**, et même **dépassée**. Le reproche
n'est pas « on ne voit pas la mesure », c'est « on ne voit qu'elle ». ⇒ Ce n'est pas un défaut
d'exécution, c'est un réglage de curseur rendu possible par le fait que le reste a été calmé.

⚠️ **Et la référence qu'il donne est interne au lot** : la plage horaire de la Filtration, posée
par ce même lot à 32 px (`.dash-hero__value--sm`). ⇒ Il ne demande pas une taille, il demande
**un seul étage de grande taille sur les cinq cartes**.

### Ce qui a changé — v2.96.2, `data/` seul

| Avant | Après | Motif |
|---|---|---|
| `--hero-size: 48px` | **`32px`** | le corps de la plage horaire, demandé par l'utilisateur |
| `--hero-size-sm: 40px` + `@media (max-width: 360px)` | **supprimés** | ⭐ à 32 px, un repli à 40 px rendrait la mesure **PLUS GRANDE** sur écran étroit que sur écran large |
| `.dash-hero__value--sm { 32px; letter-spacing: -0.01em }` | **supprimée** | à taille égale elle n'a plus d'objet ; la plage horaire hérite du `-0.02em` commun (~4 px sur 13 caractères) |
| classe `dash-hero__value--sm` dans `index.html:236` et `app.js:5490` | **retirée** | ⛔ ne pas laisser une classe morte derrière une règle supprimée |

⛔ **Rien d'autre n'a bougé** : `--hero-weight: 800`, `.dash-hero__value--warn` (28 px),
`.compact-unit` (16 px), `.dash-hero__target` (14 px), `--dash-pad`, marges de jauge, pastilles,
couleurs — **intacts**.

### Avis `ux-ui-designer` — captures AVANT/APRÈS jointes au brief, mandat NON découpé

Le brief ne lui a pas demandé que le chiffre : il lui a demandé **ce que la réduction entraîne
mécaniquement**. Ses tranchés :

- **32 px confirmé.** ⛔ Pas de contre-proposition : l'utilisateur a jugé sur son vrai téléphone,
  l'agent n'avait aucune mesure à lui opposer.
- **`.compact-unit` reste à 16 px.** Le rapport unité/valeur passe de 1:3 à 1:2, mais « mV » reste
  **plus petit, plus clair et moins gras** que la mesure. La passer à 14 px ferait un **quatrième
  corps de texte** pour un gain nul.
- **`.dash-hero__target` reste à 14 px.** ⭐ Le rapport avec la mesure tombe à **2,3**, et la
  hiérarchie tient quand même — **parce que la taille n'est pas seule à la porter** : la mesure est
  en 800 et en `--text` (encre), la consigne en 600 et en `--text-secondary`, alignée à droite sur
  la même ligne de base. ⚠️ C'est ce motif qu'il faut écrire dans le design system, pas le chiffre
  seul : sinon un futur lecteur croira la règle violée.
- **`--hero-weight: 800` inchangé.** La plage horaire est déjà en 800 à 32 px — descendre à 700
  romprait l'alignement que l'utilisateur demande. Et le gras relatif d'un corps plus petit est
  **plus** lisible, pas moins.
- **`--warn` gardé à 28 px.** Texte plus long que « 7,3 » (« Marche forcée ») : le réaligner sur
  32 px risquerait un débordement à 320 px sans bénéfice.
- **Espacements inchangés.** Chaque carte perd ~16 px de hauteur, l'espace valeur→jauge reste à
  ~20 px. ⛔ « Retoucher les marges sur une impression visuelle, sans mesure, rouvrirait ce qui
  vient d'être validé. »
- **Réserve honnête, écrite telle quelle** : à 32 px la mesure est *« un peu moins spectaculaire »*
  qu'à 48. ⇒ C'est le prix assumé du verdict de l'utilisateur, pas un angle mort.

### Inventaire d'impact — réel, pas « quelques pages »

| Jeton / classe | Emplois | Hors accueil ? |
|---|---|---|
| `--hero-size`, `--hero-weight` | définis `app.css:2436-2437` (scopés `.card--status`), lus `app.css:2533-2534` | **aucun** |
| `.compact-value` | `index.html:386` (pH), `:496` (ORP), `:582` (température) | aucun |
| `.compact-unit` | `index.html:496`, `:582` + injections `app.js:6130`, `:6154` | aucun |
| `.dash-hero__value--warn` | `app.js:5487` | aucun |
| mention doc | `docs/DESIGN_SYSTEM.md:655` | — |

⇒ Les **43 autres cartes** de l'application ne bougent pas.

### Contrôles

`pio run` **SUCCESS** · `./build_fs.sh` **SUCCESS, 512 Ko** · `pio test -e native`
**2696 cas, 2696 réussis** · toutes les gardes `tools/check_*.sh` **vertes**, sauf
`check_release_artefacts.sh` qui exige une signature de release — ⛔ normal hors release.

### ⚠️ À ne pas oublier au déploiement

`src/version.h` passe à **2.96.2** alors que **seul `data/` change** — même piège qu'en 2.96.0.
⇒ Un envoi du système de fichiers **seul** laisserait l'appareil annoncer **2.96.1** avec
l'interface 2.96.2. **Envoyer les deux binaires**, et ⛔ par `./ota_update.sh --use-sig both`,
jamais `./deploy.sh ota-all` qui **reconstruit** et invalide les signatures.

### ✅ DÉPLOIEMENT DE LA 2.96.2 — 2026-11-21, LES DEUX BINAIRES ACCEPTÉS

Envoi par `./ota_update.sh --use-sig <genre> 192.168.1.48`, **un binaire à la fois**.
⛔ Pas de `./deploy.sh ota-all` : il reconstruit et invaliderait les signatures.

| Envoi | authenticité | liaison manifeste↔image (paramètre) | liaison (empreinte calculée) | empreinte upload |
|---|---|---|---|---|
| **firmware** 13:45:19 | ✅ valide — refusé : **non** | ✅ liée — divergente : **non** | ✅ liée — divergente : **non** | ✅ conforme — refusée : **non** |
| **filesystem** 13:46:21 | ✅ valide — refusé : **non** | ✅ liée — divergente : **non** | ✅ liée — divergente : **non** | ✅ conforme — refusée : **non** |

Appareil relu après chaque redémarrage : `firmware_version` = **2.96.2**.
Empreinte de la clé publique rejournalisée aux deux démarrages :
`844449550704da06b085d24a8ae2b079a349730d520e61824dc2816956a57ebb`.

#### ⭐ Un acquis d'outillage qui corrige feature-151
[feature-151](../done/feature-151-signature-de-l-image-ota.md) a conclu que l'acceptation du
**firmware** ne pouvait pas être *relevée* — seulement déduite du résultat — parce que le
redémarrage chasse les lignes de verdict du journal volatil de 8 Ko. ⚠️ **C'est vrai du journal
volatil, et FAUX du journal persistant** : `/download-logs` a rendu ici les **quatre** lignes de
verdict du firmware, horodatées, **après** le redémarrage.

⇒ La parade écrite dans `docs/UPDATE_GUIDE.md` (« envoyez les deux binaires séparément pour
relever un verdict ») reste utile pour une lecture immédiate, mais elle n'est **pas** la seule :
⭐ **le journal persistant garde les verdicts en travers des redémarrages.** Corrigé dans le guide
le 2026-11-21.
