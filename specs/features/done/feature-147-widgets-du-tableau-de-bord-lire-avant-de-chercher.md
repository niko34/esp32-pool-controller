# feature-147 — Widgets du tableau de bord : lire avant de chercher

## Métadonnées
- **ID** : feature-147
- **Slug** : widgets-du-tableau-de-bord-lire-avant-de-chercher
- **Type** : feature (refonte de surface UI)
- **Statut** : doing — implémentation livrée (`data/` seul), revue `code-reviewer` en cours en parallèle de cette documentation
- **Priorité** : normale
- **Version cible** : 2.64.0
- **Dernière mise à jour** : 2026-10-09
- **Spec(s) liée(s)** : feature-146 (quatre modes — **les libellés en viennent**) · feature-053
  (Mode Boost) · feature-055 (ligne d'effet honnête du Boost)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [x] Filtration / Éclairage *(surface UI uniquement — aucune logique touchée)*
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] **Frontend (`data/`)**

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — *a priori NON : aucune logique de dosage, aucun seuil, aucune garde.*
      **À confirmer** : si la représentation graphique affiche des **zones de valeurs**
      (acide / neutre / basique, sous-consigne…), les bornes de ces zones deviennent un
      message chimique adressé à l'utilisateur ⇒ gate obligatoire sur ces bornes.
- [x] **ux-ui-designer — OBLIGATOIRE** (refonte de surface, arbitrage entre mises en page)
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Revoir les widgets du **tableau de bord** (`data/index.html` + `data/app.js`) en reprenant
quatre idées d'une carte Home Assistant que l'utilisateur juge plus lisibles que l'existant.

1. **Les modes de filtration deviennent QUATRE BOUTONS**, pas une ligne « Mode » suivie d'un
   libellé. Le mode actif est le bouton en évidence. **Pas de label « Mode » devant** : les
   boutons se suffisent.
2. **Les boutons « Démarrer » / « Arrêter » DISPARAISSENT.** Depuis feature-146 ils ne font
   que choisir un mode — les boutons de mode les rendent redondants.
3. **Le Boost devient une OPTION DE LA CARTE FILTRATION**, et non une carte séparée.
   ⚠️ **La ligne qui explique l'effet réel du Boost est CONSERVÉE** — l'utilisateur la juge
   utile pour comprendre ce que le Boost fait. C'est l'acquis de feature-055 (honnêteté
   d'effet) : ne pas la perdre dans le déménagement.
4. **pH et ORP gagnent une représentation GRAPHIQUE de la valeur courante** : une position sur
   une échelle, lue d'un coup d'œil, avant même de lire le nombre.

## Contexte

L'utilisateur a fait produire une carte Lovelace pour Home Assistant et constate qu'elle se lit
mieux que son propre tableau de bord. Ses mots : *« Plutôt que d'avoir une ligne "Mode" avec un
libellé du mode, HomeAssistant propose des boutons pour activer les différents modes. Je trouve
cela plus pertinent. […] Il n'y a pas besoin de bouton Marche et Arrêt. Les boutons des modes
suffisent. Le Boost ne devrait être qu'une option de la carte filtration. Mais le libellé qui
explique le boost est intéressant. […] Le widget du pH et de l'ORP sont aussi intéressants avec
une représentation graphique de la valeur actuelle. »*

Ce que montre la carte de référence :
- **pH** : barre en dégradé continu (rouge → vert → bleu) avec un **curseur** à la position de
  la mesure, et trois repères textuels — Acide · Neutre · Basique. Sous la barre :
  « Consigne 7,4 · Régulation Automatique ».
- **ORP** : barre à **deux marqueurs** — la **mesure** (à gauche, rouge) et la **consigne**
  (trait vertical) — avec légende. Sous la barre : « Consigne 530 mV · **Sous consigne** ·
  Mode Automatique ».
- **Filtration** : titre « Filtration » + mention discrète du mode à droite, puis **quatre
  boutons en grille 2×2**, puis une ligne « Programme enregistré · 08:04 à 17:55 · 9 h 51 ».

⚠️ **PIÈGE DE LIBELLÉ** : la carte de référence affiche **« Manuel »** pour le troisième mode.
C'est l'ANCIEN libellé, celui qui désignait le mode qui **ARRÊTAIT** la pompe et qui a causé
l'incident du 2026-10-09. feature-146 l'a renommé **« Marche »** dans le firmware ET dans Home
Assistant. **Les quatre libellés sont Auto · Programme · Marche · Arrêt**, sans exception.

## Comportement attendu

1. Les quatre boutons de mode envoient `filtration_mode` (`auto|manual|force|off`) via
   `setFiltrationRunMode()` — **le chemin existe déjà**, ne pas en créer un second.
2. Le bouton du mode actif est visuellement distinct. **Aucun bouton n'est jamais désactivé** :
   en Arrêt, « Auto » est la sortie.
3. Le Boost reste **indisponible en mode Arrêt**, grisé avec son motif (acquis de feature-146).
4. La ligne d'effet réel du Boost (`#detail-boost-effect`) survit au déménagement, à
   l'identique.
5. Les représentations graphiques affichent la **valeur courante** et la **consigne**. Elles
   **complètent** les mini-graphiques temporels existants (`#ph-mini-chart`,
   `#orp-mini-chart`), elles ne les remplacent pas — sauf arbitrage motivé de
   `ux-ui-designer`.
6. Dégradation propre : mesure absente, `NaN`, hors échelle ⇒ pas de curseur, pas de barre
   fantôme, jamais de position fausse.

### Cas limites
- pH hors de l'échelle affichée (brut < 6,2 ou > 8,1) : le curseur se borne visuellement, mais
  **la valeur numérique reste exacte**.
- ORP très loin de la consigne (relevé réel du 2026-10-09 : **108 mV pour 530**) : la barre
  doit rester lisible dans ce cas, qui n'est pas exceptionnel.
- Filtration arrêtée : les mesures ne sont pas représentatives — la représentation graphique ne
  doit pas suggérer une mesure fiable.

## Comportement actuel

`data/index.html:242-253` — carte filtration du tableau de bord : une ligne « Mode » + une ligne
« Horaires » + deux boutons « Démarrer »/« Arrêter ».
`data/index.html:~265+` — `#dashboard-temp-modes-card`, **carte séparée** ne contenant que le
Boost et sa ligne d'effet.
`data/index.html:371-420` — cartes pH et ORP : valeur numérique, badge capteur, mini-graphique
temporel (uPlot), légende. **Aucune représentation de position.**

## Contraintes techniques

- **Vanilla JS**, pas de framework, pas de bundler hors `minify.js`. Pas de nouvelle
  dépendance : si une barre dégradée suffit en CSS, **pas de bibliothèque**.
- **Budget LittleFS** : `data/` est gzippé dans une partition de 512 Ko. Toute addition se
  mesure (`./build_fs.sh` affiche les tailles).
- ⚠️ **AUCUN nouveau champ** `/data` ou WebSocket sans nécessité démontrée. La marge de trame
  WS est de **87 o** (feature-146). Consigne, mesure et mode sont **déjà publiés**.
- Réutiliser les variables CSS existantes (`--border`, `card__body`, `detail-row`, `btn`…) :
  pas de nouveau système de couleurs.
- Accessibilité : l'information portée par la couleur doit l'être **aussi** par le texte (la
  valeur numérique et la consigne restent affichées).

## Fichiers potentiellement concernés

`data/index.html` (carte filtration ~240-255, carte Boost ~265+, cartes pH/ORP ~365-420) ·
`data/app.js` (`updateDetailSections`, `setFiltrationRunMode`, `updateBoostCard`, rendu pH/ORP)
· `data/app.css`

## Livrables — check-list

- [x] A1 — Maquette `ux-ui-designer` : grille des 4 boutons, place du Boost dans la carte
      filtration, forme des deux représentations graphiques. **Arbitrage écrit** : la
      représentation remplace-t-elle le mini-graphique temporel, ou s'y ajoute-t-elle ?
      → **S'ajoute** (confirmé dans le code : les mini-graphiques `#ph-mini-chart` /
      `#orp-mini-chart` sont intacts, les jauges `.gauge` sont des blocs distincts).
- [x] A2 — Décision écrite sur les **bornes** des échelles pH et ORP, et sur les zones
      nommées. Si des zones chimiques sont affichées ⇒ **gate `pool-chemistry`**. → Gate
      passé : zones « Trop bas/Idéal/Trop haut » pour le pH (bornes = `_phBounds`, config
      réelle), **aucune** zone nommée pour l'ORP (justification chimique documentée dans
      `docs/features/page-dashboard.md`). Maquette initiale « Acide/Neutre/Basique »
      réfutée par le gate (7,00 n'est pas une plage, 7,60 n'est pas une frontière
      d'acido-basicité).
- [x] B1 — Quatre boutons de mode, libellés **Auto · Programme · Marche · Arrêt**, passant par
      `setFiltrationRunMode()` existant.
- [x] B2 — « Démarrer »/« Arrêter » **supprimés** du tableau de bord (et du détail si la
      refonte s'y étend — à trancher). → Tranché : **dashboard seulement**. La page
      `/filtration` (carte « Contrôle manuel ») garde ses boutons Démarrer/Arrêter intacts —
      hors périmètre, confirmé par l'absence de diff sur cette carte.
- [x] B3 — Boost intégré à la carte filtration, **ligne d'effet conservée**, indisponibilité en
      Arrêt préservée.
- [x] B4 — Représentations graphiques pH et ORP, avec consigne.
- [x] C1 — **Mesure** : `git diff --shortstat -- data/` et delta de taille du FS. → voir
      Notes d'implémentation ci-dessous.
- [x] D1 — Dégradation propre vérifiée : mesure absente / `NaN` / hors échelle / filtration
      arrêtée. → `phGaugePosition()`/`orpGaugePosition()` retournent `null` sur
      valeur non finie (curseur masqué) ; clamp `Math.max(0, Math.min(100, pct))` sur
      hors-échelle ; classe `.gauge--stale` + texte « Mesures non représentatives » sur
      `measuresOk` faux.
- [x] D2 — `node --check data/app.js`, `./build_fs.sh` SUCCESS, aucun champ public ajouté
      (prouvé par grep). → `node --check` OK ; minification + mklittlefs rejoués en
      sandbox, SUCCESS ; grep confirme zéro nouveau champ `/data`/WebSocket (les jauges
      consomment `ph`, `orp`, `orp_target`, `measures_valid`, déjà publiés).

## ADR attendu ?

**Non** — refonte de surface sans décision structurante : aucune des 4 critères de
déclenchement du gabarit ADR n'est rempli (pas de contrainte posée à d'autres parties du
système, le choix des zones/bornes est documenté dans `docs/features/page-dashboard.md` et
reste reversible sans migration, pas d'alternative structurante écartée au-delà du choix
d'ergonomie déjà tranché par `ux-ui-designer`/`pool-chemistry`, aucun contrat MQTT/OTA
verrouillé). Confirmé en documentation.

## Éléments visibles (pour doc-writer)

`docs/features/page-dashboard.md` · `docs/features/page-filtration.md` ·
`docs/user/src/guide-utilisation.html` · `CHANGELOG.md`.
**Captures d'écran à refaire** : tableau de bord, et page Filtration si elle change.

## Note de méthode

L'utilisateur a demandé, trois fois dans la même journée, de **retirer** plutôt que d'ajouter,
et a reproché un vocabulaire trop complexe. Cette refonte **supprime** deux boutons et une
carte ; elle n'ajoute que de la lisibilité. Toute proposition qui alourdit l'écran doit être
justifiée ou abandonnée.

## Notes d'implémentation (doc-writer, 2026-10-09)

**Mesure du diff (C1)**, au moment de la rédaction documentaire (le `code-reviewer` travaille
en parallèle sur le même diff — ce chiffre peut avoir légèrement évolué depuis) :

```
$ git diff --shortstat -- data/
3 files changed, 393 insertions(+), 65 deletions(-)
```

**Delta de taille du FS gzippé** — mesuré en reconstruisant les deux bundles (`node minify.js`
+ `mklittlefs`) côte à côte : la version `HEAD` (pré-feature-147, reconstruite via `git show`
dans un répertoire isolé, sans toucher l'arbre de travail) et la version courante.

| | Total gzippé (`data-build/`) |
|---|---|
| Avant (HEAD, pré-feature-147) | 199 632 o |
| Après (feature-147) | 201 249 o |
| **Delta** | **+1 617 o (+0,8 %)** |

Budget partition `spiffs` (ADR-0068) : 524 288 o (512 Ko). Usage après ce lot : 201 249 / 524 288
≈ **38,4 %** — marge large, le lot retire plus de markup (carte + 2 boutons) qu'il n'en ajoute
(jauges CSS pures, pas de bibliothèque).

**Documentation mise à jour** : `docs/features/page-dashboard.md` (deux nouvelles sections —
Carte Filtration quatre boutons/mode vs état/Boost intégré, Jauges de position pH/ORP — plus
mise à jour de la liste des cartes, de la section historique « Carte Modes temporaires », de la
table Actions et des inventaires Fichiers/Specs historiques) · `docs/features/page-filtration.md`
(note de non-régression explicite : page de détail intacte) · `docs/user/src/guide-utilisation.html`
(chapitre « Le tableau de bord » réécrit en langage simple : quatre boutons, mode vs état,
lecture des deux jauges, absence volontaire de zones nommées côté ORP) · `CHANGELOG.md` (ajout
dans l'entrée `[2.63.0]` existante — **aucun bump de version** puisque `src/version.h` n'a pas
changé et que ce lot est `data/` seul ; aucun autre précédent de lot frontend pur n'a été trouvé
dans l'historique récent avec une entrée CHANGELOG dédiée à traiter en référence, voir ci-dessous).

⚠️ **Précédent frontend-only trouvé, mais non exploitable tel quel** : le seul autre commit
`data/`-seul des 200 derniers (`513731c`, juste avant ce lot) n'a ajouté **ni** entrée CHANGELOG
**ni** bump de version — ce qui ressemble à une omission du tour qui l'a produit plutôt qu'à un
usage délibéré (CLAUDE.md impose `doc-writer` à chaque tour modifiant un comportement
observable). Faute d'un usage établi à suivre, ce lot a été documenté en l'ajoutant à l'entrée
`[2.63.0]` **courante et non taguée** (`git tag` ne connaît pas `v2.63.0`), plutôt qu'en
inventant un numéro de version absent de `src/version.h` — signalé à l'utilisateur pour
arbitrage si un usage différent est préféré pour les prochains lots `data/`-seuls.

**Captures d'écran à refaire** (humain, avant la prochaine génération de PDF utilisateur) :
- Tableau de bord (`03_Tableau_de_bord.jpg`) — changé en profondeur (carte Filtration à quatre
  boutons, jauges pH/ORP, carte Modes temporaires disparue).
- Page Filtration (`04_Filtration.jpg`) — **probablement pas nécessaire** : aucun diff constaté
  sur cette page (hors périmètre, confirmé par B2 ci-dessus) ; à confirmer visuellement si doute.
