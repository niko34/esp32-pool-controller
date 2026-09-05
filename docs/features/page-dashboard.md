# Page Tableau de bord — `/dashboard`

- **Fichier UI** : [`data/index.html:162`](../../data/index.html:162) (section `#view-dashboard`)
- **URL** : `http://poolcontroller.local/#/dashboard` (page d'accueil par défaut)

## Rôle

Vue d'ensemble de la piscine en temps réel. C'est la page que l'utilisateur voit en arrivant sur l'interface. Elle doit donner **en un coup d'œil** l'état de l'installation et permettre les actions fréquentes (allumer/éteindre filtration et éclairage) sans navigation.

## Structure

La page est une **grille de six cartes** (classe `grid--status-cards`) :

1. **Carte Filtration** (`#dashboard-filtration-card`) — état, mode, horaires, boutons Démarrer / Arrêter. Un lien engrenage amène vers `/filtration`.
2. **Carte Éclairage** (`#dashboard-lighting-card`) — état, mode (programmation on/off), horaires, boutons Allumer / Éteindre. Lien vers `/lighting`.
3. **Carte pH** (`#dashboard-ph-card`) — valeur pH courante, cible, badge dosage actif, mini-graphique + légende « Lissé (régulation) » / interrupteur « Brut » (v2.38.0). Bandeau « Calibration requise » si la sonde n'a jamais été calibrée. Lien vers `/ph`.
4. **Carte ORP** (`#dashboard-orp-card`) — valeur ORP courante (mV), cible, badge dosage actif, mini-graphique + même légende (v2.38.0). Bandeau calibration. Lien vers `/orp`.
5. **Carte Température** (`#dashboard-temperature-card`) — valeur courante en °C, badge `#temperature-sensor-badge` (« Capteur indisponible » v2.10.5 + segments d'état mesure feature-061 itération 2), mini-graphique. Lien vers `/temperature`.
6. **Carte Boost** (`#dashboard-boost-card`) — activation / désactivation du Mode Boost, heure d'expiration quand il est actif (feature-053, v2.18.0, voir [Carte Boost](#carte-boost-feature-053)).

Chaque mini-graphique est rendu par **uPlot** (usine `createMiniLineChart`, feature-043 — remplace Chart.js) dans un conteneur dédié, alimenté par l'historique de la **journée en cours** (depuis minuit local — v2.18.9, était une fenêtre glissante de 24 h en v2.18.4 et 3 jours avant ; points bruts + moyennes horaires selon l'ancienneté — voir [Plage « journée en cours »](#plage-des-mini-graphiques--journée-en-cours-v2189)). La coloration de la courbe est **conditionnelle par segment** (vert dans la tolérance, interpolation continue vers le rouge au-delà, gris si donnée absente — `getMiniChartRGB`, app.js).

**Responsive / mobile (v2.18.3)** : la hauteur des graphiques uPlot (mini-charts ~88 px, écrans détail 220 px) est posée en **styles inline** par uPlot et resynchronisée par le `ResizeObserver` partagé (`observeChart`, app.js) — debounce 100 ms, `setSize` uniquement si les dimensions sont > 0 et ont réellement changé, re-synchronisation initiale au premier passage (couvre un conteneur aux dimensions provisoires au moment de la création), rotation portrait/paysage incluse. ⚠️ **Ne jamais dimensionner un `canvas` uPlot en CSS** : une règle morte héritée de Chart.js (`canvas { height: 240px !important }` sous 980 px, `data/app.css`) étirait puis écrêtait les graphiques sur iPhone — supprimée en v2.18.3 (un commentaire de garde marque l'emplacement dans `app.css`).

Un **chip `Calibration nécessaire`** (`#calib-chip`) s'affiche en haut de page si pH **ou** ORP est flagué non calibré.

## Bannière « Régulation à l'arrêt » (v2.20.3)

`<div class="regul-alert" id="regul-alert" role="alert" hidden>` en tête de page, au-dessus de la grille de cartes. Rendue par `updateRegulationAlert(json)` à partir des drapeaux firmware `phFilterAlarm` / `orpFilterAlarm` (`GET /data` **et** WebSocket).

Motivation : lors de l'incident du 2026-08-16, le bassin est resté **sans régulation pH ni chlore pendant ≥ 3 h** et rien ne le signalait — l'utilisateur ne l'a détecté que parce qu'une valeur affichée lui semblait incohérente.

| Drapeaux | Titre | Cible du bouton « Diagnostiquer » |
|---|---|---|
| `phFilterAlarm` seul | « Régulation pH à l'arrêt » | `#/ph` |
| `orpFilterAlarm` seul | « Régulation chlore à l'arrêt » | `#/orp` |
| les deux | « Régulation pH et chlore à l'arrêt » | `#/ph` |
| aucun | masquée (`hidden`) | — |

Texte : `Le filtre de mesure est bloqué depuis plus de 5 minutes alors que la filtration tourne : aucun dosage <pH|chlore|pH et chlore> n'est effectué. Ouvrez l'état du filtre pour diagnostiquer et le réarmer si besoin.`

Le firmware n'arme ces drapeaux que si la filtration tourne, que les mesures sont représentatives **et** que le filtre reste non prêt > 5 min : **aucun affichage** pendant une purge, un warmup ou une filtration à l'arrêt. La reprise se fait depuis le modal d'état filtre de la page concernée ([page-ph.md](page-ph.md#bouton--réarmer-le-filtre-ph--v2203), [page-orp.md](page-orp.md#bouton--réarmer-le-filtre-orp--v2203)).

Une variante `regul-alert--inline` du même style est réutilisée dans la carte Régulation pH pour la bannière de bascule automatique en manuel.

## Plage des mini-graphiques : journée en cours (v2.18.9)

Les mini-graphiques des cartes pH, ORP et Température n'affichent **que la journée en cours**, à partir de **minuit local**. Une fenêtre glissante de 24 h (comportement v2.18.4) faisait apparaître une portion de la veille en tête de courbe, ce qui brouillait la lecture « où en est-on aujourd'hui ».

- **Filtrage côté navigateur** : la requête au firmware reste `GET /get-history?range=24h`. Aucun changement de contrat HTTP, aucun paramètre nouveau, aucun changement firmware — les points dont le `timestamp` est antérieur à minuit local sont simplement écartés à la réception (`startOfTodaySeconds()`, `loadMiniChartData()`, app.js). Le firmware et [`docs/API.md`](../API.md#get-get-history--write) sont inchangés.
- **Filtrage avant calcul des coupures** : l'écart est mesuré entre points **déjà filtrés**, sinon le premier point du jour créerait un faux trou de plusieurs heures avec le dernier point de la veille. Le « point précédent » du chemin incrémental n'est retenu que s'il appartient lui aussi au jour courant.
- **Passage de minuit automatique** : le jour civil affiché est mémorisé (`miniChartDayStart`). Au premier rafraîchissement suivant le changement de date (cadence **5 min**), le polling bascule du mode incrémental (`?since=`) vers un **rechargement complet** qui **remplace** la série. Les points de la veille disparaissent seuls, sans action de l'utilisateur ni rechargement de la page.
- **Début de journée assumé** : entre minuit et le premier relevé, les courbes sont **vides** ; elles restent courtes pendant les premières heures. C'est le comportement demandé, pas un défaut. Le repli d'échelle existant (`applyMiniChartFallbackScale`, cadre cible ± tolérance) conserve un graphique lisible même sans point.
- **Étiquettes de l'axe X** : tous les points appartenant au jour courant, `miniChartPointLabel()` affiche l'heure seule. L'**infobulle reste datée et typée** (`jj/mm hh:mm` + mesure / moyenne horaire / moyenne journalière — acquis v2.18.4, voir section suivante).
- **Hors périmètre** : les graphiques des **écrans détail** (`/ph`, `/orp`, `/temperature`) conservent leurs plages `Tout` / `30j` / `7j` / `24h`. Les coupures de courbe aux arrêts de filtration (> 90 min) sont inchangées.
- **Fuseau horaire de référence = celui du NAVIGATEUR**, pas celui du contrôleur (`startOfTodaySeconds()` s'appuie sur `new Date().setHours(0,0,0,0)`). Les horodatages reçus sont des epoch UTC, la conversion est donc exacte ; mais consulter le tableau de bord depuis un appareil réglé sur un autre fuseau décale la coupure de « minuit » d'autant. Sans conséquence en usage normal (même fuseau que le bassin) ; c'est le même choix que pour les étiquettes horaires, qui suivent déjà la locale du navigateur.

> ⚠️ Le changement est **entièrement dans `data/app.js`** : il faut **reconstruire et reflasher le filesystem** (`./build_fs.sh` puis `./deploy.sh fs` ou `ota-fs`). Mettre à jour le firmware seul ne suffit pas.

## Mini-graphiques pH / ORP : la courbe suit le nombre affiché (feature-095, v2.38.0)

**La vraie raison du lot.** Le grand nombre des cartes pH et ORP affichait **déjà** la mesure **lissée** servie par `/data` — celle sur laquelle la régulation décide — au-dessus d'une courbe **brute**. Le nombre et la courbe ne parlaient donc pas du même signal, ce qui a produit **deux mauvais diagnostics** par le passé. La feature-095 ne « promeut » pas la lissée : elle **aligne la courbe sur le nombre qu'elle est censée expliquer**.

Depuis la v2.38.0, les mini-graphiques pH et ORP tracent **deux séries** :

| Série | Rôle | Rendu | Source `/get-history` |
|---|---|---|---|
| **Lissé (régulation)** | courbe **principale** — c'est la valeur du grand nombre, celle qui décide du dosage | trait épais (2 px), **spline**, **aire** remplie, coloration conditionnelle par segment, point de survol | `phFiltered` / `orpFiltered` |
| **Brut** | courbe **secondaire**, diagnostic sonde | trait fin (1 px), tracé **linéaire** (une spline inventerait des dépassements sur un signal bruité et dense) | `ph` / `orp` |

- **pH affiché à 2 décimales** sur ces mini-graphiques (était 1) : à 0,1 près, la quantification dépasse l'amplitude utile du signal lissé (~0,05).
- **Infobulle** : quand les deux séries existent, deux lignes préfixées `lissé` / `brut` (+ la ligne de granularité). Quand seul le brut existe, une seule ligne `brut` — l'infobulle ne disparaît **jamais** sur la portion sans lissée (ce serait un effet « panne »).
- **La carte Température n'a qu'une série** : elle passe par la même fabrique `createMiniLineChart` avec une série brute entièrement nulle. Un seul chemin de code, la série principale reste toujours l'index 2.
- **Les graphiques des écrans détail (`/ph`, `/orp`, `/temperature`) ne changent pas** : ils agrègent **1 point par jour**, granularité à laquelle la lissée vaut toujours `null` — et où elle serait de toute façon indiscernable du brut. Seuls les mini-graphiques du tableau de bord sont touchés. C'était aussi la **réduction de risque** du lot : la fonction de tracé principale (`createLineChart`) reste mono-série.

### La frontière ~8 h, et pourquoi elle ne ressemble pas à une panne

La fenêtre affichée couvre la journée en cours (jusqu'à 24 h) ; la mesure lissée, elle, n'est historisée **que sur les points 5 min**, soit **~8 h de profondeur** ([history.md](../subsystems/history.md#la-lissée-nest-historisée-que-sur-les-points-raw)). Le problème de conception n'était pas « la courbe s'arrête » — c'était « **une courbe forte apparaît de nulle part** » au milieu du graphique.

Trois dispositifs, qui font lire **un seul événement** au lieu de deux :

1. **Le brut couvre toujours 100 % de la fenêtre** — il n'y a jamais de zone vide.
2. **La lissée est une aire, pas une ligne** : une **zone qui commence** évoque une période ; une **ligne qui s'arrête** évoque une panne.
3. **L'opacité du brut bascule à la même frontière** : `0,35` (halo, sous la lissée) là où la lissée existe, `0,9` (courbe pleine, aspect d'avant la 2.38) là où elle n'existe pas. Le basculement tombe **exactement** au x où l'aire commence.

**Aucune lissée sur toute la fenêtre** (première heure après la mise à jour, journée entièrement antérieure à la 2.38) : la légende le **dit** — l'entrée « Lissé (régulation) » s'atténue et reçoit le suffixe *« (aucune sur la période) »* + un `title`. La couleur et l'opacité ne portent **jamais seules** l'information.

### Interrupteur « Brut » (`#ph-mini-raw-toggle`, `#orp-mini-raw-toggle`)

Sous chaque mini-graphique pH et ORP, une légende (`.mini-legend`) porte l'entrée « Lissé (régulation) » et un **interrupteur « Brut »** (`role="switch"`, `aria-checked`, cible tactile 44 px compensée par marge négative pour ne pas grandir la carte).

- **Un interrupteur par graphique**, indépendants. **Défaut : brut visible.**
- **Mémorisé** dans `localStorage` sous `poolctl.ui.showRaw.ph` / `poolctl.ui.showRaw.orp` (`'1'` visible / `'0'` masqué). L'état est restauré **avant** le premier chargement de données.
- **Bénéfice réel du masquage** : l'échelle Y **se resserre sur la lissée**. C'est pour cela que masquer le brut passe ses valeurs à `null` et **jamais** par `show: false` — une série cachée continuerait de dicter l'échelle.
- **Aucun appel réseau au clic** : les valeurs brutes sont déjà en mémoire (`chart.st.raw`).

> **Première utilisation de `localStorage` dans le projet.** L'accès est **entièrement protégé** (`uiPrefGet` / `uiPrefSet`, app.js) : `localStorage` peut lever **à la lecture comme à l'écriture** (Safari en navigation privée, stockage désactivé par politique). Un objet en mémoire sert de repli et un échec ne remonte **jamais** à l'utilisateur — la préférence vaut alors pour la session seulement. Tout nouveau réglage d'interface persisté doit passer par ces deux fonctions et par le préfixe `poolctl.ui.`.

## Tooltip daté/typé et coupures de courbe (v2.18.4, bug-points-futurs-et-ux-courbes)

Conventions partagées entre les **mini-graphiques** du dashboard (`createMiniLineChart`) et les **graphiques des écrans détail** pH/ORP/température (`createLineChart`, app.js) :

- **Tooltip toujours daté ET typé** : le titre du tooltip affiche systématiquement **date + heure** au format `jj/mm hh:mm` (`tooltipDateTime()`) — plus d'heure seule pour « aujourd'hui » —, et une ligne indique le **type de point** selon la granularité `/get-history` (`granularityLabel()` : `0` → « mesure », `1` → « moyenne horaire », `2` → « moyenne journalière »). Les étiquettes de l'**axe X** restent courtes (heure seule si aujourd'hui) — le tooltip lève l'ambiguïté sans surcharger l'axe. Implémentation : tableaux **parallèles** `st.ts` / `st.gran` à côté de `st.labels` / `st.values` (les 3 mini-charts partagent les points sources → `updateMiniChart` copie par `slice()` pour que `appendMiniChart` ne mute pas les voisins). Sur les écrans détail (agrégation 1 point/jour), le timestamp/granularité retenu est celui du point qui a fourni **la valeur du capteur concerné** ; le point live « Aujourd'hui » (`updateChart`) est daté de maintenant, granularité « mesure ». `setPoints(labels, values, ts, gran)` accepte `ts`/`gran` optionnels (appels existants sans tooltip daté restent valides).
- **Coupure de courbe aux arrêts** (mini-graphiques) : deux points consécutifs espacés de **plus de 90 min** (`CHART_GAP_SECONDS = 5400`) → une entrée `null` est insérée dans la série (uPlot lève le crayon, même convention que les mesures invalides feature-061) — les **arrêts de filtration** et coupures de courant deviennent visibles. Appliqué **aussi au chemin incrémental** (`?since=`) : le « point précédent » du premier nouveau point est le dernier point déjà affiché (`lastMiniChartTimestamp`), un arrêt à cheval sur le polling crée aussi le trou.
- **Coupure sur les écrans détail** : à 1 point/jour, le seuil de 90 min est inapplicable (deux jours consécutifs sont espacés de ~24 h) — l'équivalent est un **jour calendaire entièrement absent** entre deux jours présents → entrée `null` insérée (comparaison en UTC des clés `YYYY-MM-DD`).

## Badge multi-états des cartes pH / ORP (`#ph-sensor-badge`, `#orp-sensor-badge`)

Chaque carte pH et ORP porte un badge composé (`composeSensorBadge()`, app.js — feature-012) qui agrège jusqu'à cinq états candidats, par ordre de priorité décroissante :

| Priorité | Segment | Variant | Condition |
|---|---|---|---|
| 0 | `Capteur indisponible` | `danger` | valeur `ph` / `orp` `null` ou NaN dans le WS (capteur stale ou figé — feature-022, v2.10.0) — **gaté par `measures_valid`** (feature-061, voir ci-dessous) |
| 1 | `Filtration arrêtée` | `info` | `water_present === false` sans staleness (feature-061, v2.13.0 — voir [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)) |
| 1 | `Signal filtration périmé` | `warning` | `water_present === false` et `filtration_state_stale === true` (feature-061) |
| 1 | `Stabilisation depuis N min` / `Re-stabilisation depuis N min` (v2.34.0 ; était `Stabilisation : ~N min`) | `info` | `water_present === true` et `measures_valid === false` (purge post-reprise, feature-061) |
| 1 | `Trop bas (X)` / `Trop élevé (X)` | `danger` | valeur hors plage (cible ± 0,2 pH / ± 150 mV) — **gaté par `measures_valid`** (feature-061) |
| 2 | `Limite journalière atteinte` | `warning` | `ph_limit_reached` / `orp_limit_reached` |
| 3 | `Cumul XX %` | `warning` | cumul journalier ≥ 75 % du quota |
| 4 | `Stock faible (X,X L)` | `warning` | `*_remaining_ml` ≤ `*_alert_threshold_ml` (seuil > 0) |

**Règle de composition** :
- **Maximum 2 segments** affichés simultanément, séparés par « · ». Au-delà de deux candidats, seuls les **deux plus prioritaires** sont retenus (ex. « hors plage + limite atteinte + stock faible » → `Trop élevé (7.8) · Limite journalière atteinte`, le stock est tronqué).
- Variant du badge : hiérarchie **`danger` > `warning` > `info`** (feature-061 — avant : `danger` sinon `warning`). La variante `info` (`.sensor-badge--info`, fond bleu) signale un **état normal informatif** — « attendre », par opposition au rouge « agir ».
- « Hors plage » et « Limite journalière atteinte » simultanés affichent désormais **les deux segments** (avant v2.9.0 : seul « hors plage » était visible).

**Segment « Capteur indisponible »** (feature-022, v2.10.0) :
- Priorité la plus haute (0) : un capteur mort passe toujours devant les autres états.
- Piloté par les **signaux WS existants** (`ph` / `orp` à `null` quand le firmware ne dispose plus d'une valeur valide — stale ou figé) : aucun champ WS nouveau.
- **Garde anti-faux-positif au boot** : le segment n'est produit que si `latestSensorData !== null` (au moins un message WS reçu) — `updateSensorBadges()` étant aussi appelé depuis `loadConfig()` avant le premier push WS.
- « Capteur indisponible » et « hors plage » sont mutuellement exclusifs (une valeur `null` n'est pas comparable à la cible).
- **Garde anti-faux-positif « mesures invalides » (feature-061)** : le segment n'est produit que si `measuresAreValid(latestSensorData)` — on ne peut pas juger une sonde (ni une valeur hors plage) quand l'eau ne circule pas. Filtration arrêtée → seul le segment d'état mesure s'affiche, jamais un faux « Capteur indisponible » rouge.

**Segment « Cumul XX % »** :
- Affiché à partir de **75 %** du quota journalier (`ph_daily_ml / max_ph_ml_per_day`, `orp_daily_ml / max_chlorine_ml_per_day`), même arithmétique que la barre de cumul des pages `/ph` et `/orp` (arrondi des entrées, pourcentage plafonné à 100).
- Masqué quand la limite est atteinte (le segment « Limite journalière atteinte » prend le relais, pas de redondance), quand la limite n'est pas configurée (quota ≤ 0), ou quand les données sont indisponibles.

## Badge de la carte Température (`#temperature-sensor-badge`, v2.10.5, étendu feature-061 itération 2)

La carte Température porte un badge calqué sur les badges pH/ORP :

- **« Capteur indisponible »** (variant `danger`, v2.10.5) : affiché quand `temperature` est `null` ou NaN dans le WS (sonde DS18B20 débranchée, non identifiée ou en erreur). Même **garde anti-faux-positif au boot** (badge produit uniquement si `latestSensorData !== null`) et, depuis l'itération 2 de feature-061, même **garde anti-faux-positif « mesures invalides »** que pH/ORP (segment gaté par `measuresAreValid()` — on ne juge pas la sonde quand l'eau ne circule pas).
- **Segments d'état mesure** (feature-061 itération 2) : mêmes états que pH/ORP via `measureStateSegments()` — « Filtration arrêtée » (info bleu), « Signal filtration périmé » (warning), « Stabilisation depuis N min » (info, v2.34.0). Voir la [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130).
- Masqué dès que la valeur redevient valide (disparition au prochain push WS, sans reload).

Pas de segments dosage/limites/calibration ici : la température n'a ni dosage, ni limites, ni calibration.

## Convention « `--` » quand un capteur est indisponible (v2.10.5)

Toute valeur capteur `null`/NaN s'affiche « **`--`** » sur **tous** les sites d'affichage numérique — jamais de conservation silencieuse de la dernière valeur connue (bug corrigé en v2.10.5 : le pattern « skip si null » figeait indéfiniment l'ancienne valeur à l'écran quand une sonde était débranchée) :

- **Métriques du tableau de bord** (`updateDashboardMetrics`, app.js) : `#m-temp`, `#m-ph`, `#m-orp` ;
- **Widgets compacts** : `#compact-ph`, `#compact-orp`, `#compact-temperature` — l'unité est conservée dans la structure (ex. `--<span class="compact-unit">°C</span>`) pour ne pas casser la mise en page ;
- **Sections détaillées** des cartes.

Cette convention est celle déjà utilisée par les pages Réglages/Calibration. **Exceptions volontaires** : les graphiques et mini-graphiques affichent des **trous** sur les points `null` (comportement correct — depuis feature-061 les `null` sont poussés dans la série pour que uPlot lève le crayon, au lieu d'être filtrés). Retour du capteur → les valeurs réapparaissent automatiquement au prochain push WS (≤ 1 cycle de rafraîchissement).

**Extension feature-061 (v2.13.0)** : la convention « `--` » s'applique aussi quand la mesure est **présente mais non représentative** (`measures_valid === false` — filtration arrêtée ou purge post-reprise). La distinction est portée par le badge : bleu « Filtration arrêtée » / « Stabilisation » (état normal) vs rouge « Capteur indisponible » (anomalie).

## Mesures pH/ORP non représentatives — filtration arrêtée / stabilisation (feature-061, v2.13.0)

Les sondes pH/ORP étant montées dans le circuit de filtration, **filtration arrêtée = eau stagnante** : la valeur lue dérive localement et ne reflète plus le bassin. Depuis l'**itération 2** de feature-061, la **température** est traitée à l'identique (sonde DS18B20 dans la canalisation → dérive vers la température ambiante du local à l'arrêt) — même drapeau `measures_valid`, mêmes badges, mêmes trous dans les courbes. Le firmware publie le verdict (`measures_valid`, voir [measure-validity.md](../subsystems/measure-validity.md)) ; l'UI l'applique :

**Segments d'état mesure** (`measureStateSegments()`, app.js — partagés dashboard / pages pH-ORP / carte et compact température, priorité 1, variant selon le cas) :

| Condition | Segment (libellé exact) | Variant | Sens |
|---|---|---|---|
| `water_present === false`, pas de staleness | `Filtration arrêtée` | `info` (bleu) | État **normal** — attendre la reprise, rien à faire |
| `water_present === false` et `filtration_state_stale === true` | `Signal filtration périmé` | `warning` (ambré) | Anomalie du signal externe — vérifier le câblage/l'émetteur |
| `water_present === true` et `measures_valid === false` | `Stabilisation depuis N min` — ou `Re-stabilisation depuis N min` si `measures_purge_count ≥ 1` (`depuis < 1 min` sous 60 s ; `Stabilisation en cours` si le champ manque) | `info` (bleu) | Purge de la chambre de mesure en cours — attendre |

- **Nouvelle variante CSS `.sensor-badge--info`** (fond bleu `--accent-light`, texte `#1e40af`, contraste AA) : distingue visuellement l'**état normal** (bleu = attendre) de l'**anomalie** (rouge « Capteur indisponible » = agir).
- **v2.34.0 (feature-091) — plus de compte à rebours.** La purge se termine sur la **stabilité des mesures** ([ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)) : sa fin n'est pas datée à l'avance. Le libellé passe donc de « Stabilisation : ~N min » (restant) à « **Stabilisation depuis N min** » (écoulé, `measures_stab_elapsed_s`), et une **re-purge** (mesure redevenue instable après ouverture) se dit « **Re-stabilisation depuis N min** ». Toujours **recalculé à chaque push WS**, aucun tick JS.
- ⚠️ Le motif de fin `2` (**plafond de 90 min atteint sans stabilité**) n'apparaît **jamais** dans ces segments : le plafond **ouvre** la porte, `measures_valid` repasse donc à `true` et la condition ci-dessus n'est plus remplie. L'information est portée **uniquement** par les bandeaux des pages [pH](page-ph.md) et [ORP](page-orp.md) et par la ligne « Dernière purge » des [Réglages](page-settings.md). Une branche « stabilisation non atteinte » testée ici ne se serait déclenchée qu'à la purge **suivante**, tant que le motif restait mémorisé — texte faux, retirée en revue : **ne pas la réintroduire**.
- **Valeurs « `--` »** : `#compact-ph` / `#compact-orp` / `#compact-temperature` (et les valeurs des pages pH/ORP, la carte température) affichent `--` quand `measuresAreValid()` est faux — la ligne « Cible » reste affichée.
- **Trous dans les courbes** : les points d'historique avec pH/ORP/température `null` sont **poussés `null`** dans les séries uPlot (lever de crayon = trou visible), y compris sur le chemin incrémental (`appendMiniChart`) et sur l'agrégation par jour (un jour sans mesure valide reste dans la série avec `null`, au lieu d'être sauté). La série température a rejoint ce comportement en itération 2 (mini-charts + page historique) — elle en était exclue en itération 1. Le point « Aujourd'hui » injecté depuis le temps réel n'est pas alimenté par une mesure invalide (la dernière mesure valide du jour reste).
- **Dé-duplication avec le statut dosage** (`dedupDosingReason()`, app.js) : quand le badge affiche déjà « Filtration arrêtée » / « Signal filtration périmé », le motif équivalent du `dosing-status` est masqué — le badge fait foi. Les autres motifs (stabilisation pompe, pause mélange, limites…) sont inchangés.
- **Rétrocompat** : champs absents (firmware < 2.13.0) → `measuresAreValid()` retourne `true`, rien ne change ; pas de badge avant le premier message WS (pas de faux positif au boot).
- Périmètre : la page **Calibration** et l'onglet **Avancé** ne sont **pas** gatés (lectures live nécessaires — sonde hors de l'eau / diagnostic). MQTT/HA inchangé.

Le statut `#ph-dosing-status` / `#orp-dosing-status` de chaque carte (`getDosingStopReason()`, app.js) affiche notamment « Stabilisation : X min YY s » pendant la stabilisation post-calibration. Depuis v2.9.0, il consomme les champs **par pompe** `ph_stab_remaining_s` / `orp_stab_remaining_s` (feature-006) avec repli sur le champ global `stabilization_remaining_s` (compatibilité firmware plus ancien) : chaque carte affiche la stabilisation de **sa** pompe — une calibration ORP n'affiche plus « Stabilisation » sur la carte pH, et inversement.

Depuis **v2.19.1**, le statut affiche aussi la **pause mélange hydraulique** post-injection (« Pause mélange : X min YY s ») via `ph_mix_remaining_s` / `orp_mix_remaining_s` (repli sur le booléen `ph/orp MixingDelayActive` → « Pause mélange en cours » sans compte à rebours). Avant cette version, la pause mélange n'était visible que dans le modal « État filtre » des pages pH/ORP, d'où une impression de dosage bloqué sans explication sur le tableau de bord. Ordre d'affichage dans `getDosingStopReason()` : régulation désactivée → stabilisation → **pause mélange** → présence d'eau → limites → **cause de refus firmware**.

> **v2.25.1 — la ligne de statut se tait quand le badge capteur fait déjà foi.** Quand `measures_valid === false`, le badge capteur affiche déjà « Stabilisation depuis N min » ; descendre jusqu'à la cause de refus firmware juxtaposait **deux messages contradictoires** sur la même carte. Le statut est donc masqué — mais **uniquement** pour la famille « stabilisation des mesures » (codes de refus marqués `stab` : préchauffage du filtre, mesures non représentatives). Une **panne** (sonde figée, sonde muette) ou tout autre motif reste dit, badge ou pas. La cause de refus est lue depuis `ph/orpDoseBlockedCode` (code machine) et non plus par analyse du texte français — même mécanisme que le chip d'état filtre, voir [page-ph.md](page-ph.md#chip-détat-filtre-feature-025) et [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md).

## Statut dosage des cartes pH/ORP — commandé vs effectif (v2.21.1)

> Itération 3 de bug-dosage-fantome ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)). Avant : `data/app.js` faisait un **OU** de `ph_dosing` (session — l'intention) et `ph_inject_remaining_s` (manuel minuté) → la carte affichait « Injection en cours » pendant les **repos** du train d'impulsions ([dosage en proportionnel de temps](../subsystems/pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210)), alors que la pompe ne tournait pas. C'est cette conflation qui a masqué le dosage fantôme d'origine.

`updateDosingStatus(sensor, isActive, stopReason)` (app.js) rend le statut `#ph-dosing-status` / `#orp-dosing-status` selon **6 priorités décroissantes** :

| # | Condition | Affichage | Dot | Classe |
|---|-----------|-----------|-----|--------|
| 1 | Injection **manuelle** minutée (`*_inject_remaining_s` ancré > 0) | « Injection manuelle : Ns » (compte à rebours) | animé | `is-active` |
| 2 | Session ouverte **et** `phase < pulse_ms` — la pompe **TOURNE** | « Injection en cours » | animé | `is-active` |
| 3 | Session ouverte, `pulse_ms > 0`, phase ≥ pulse_ms (repos entre deux pouls) | « Prochaine impulsion : Ns » (décompte jusqu'à la fin de période) ; à l'échéance sans nouvelle donnée → « Prochaine impulsion… » | **statique** | `is-waiting` |
| 4 | Session ouverte, `pulse_ms == 0` (deadband — période sans pouls planifié) | « Dosage en attente : Ns » / « Dosage en attente… » | statique | `is-waiting` |
| 5 | Motifs existants (`getDosingStopReason()` : Stabilisation, Pause mélange, limites…) | texte du motif | aucun | — |
| 6 | Rien à afficher | vide (masqué) | — | — |

**Sémantique du dot** : dot **présent** = une séance de dosage est ouverte ; dot **animé** (pulsation) = la pompe est réellement alimentée ; dot **statique** gris (`.is-waiting`, app.css) = silence **volontaire** du train d'impulsions — la distinction visuelle entre « commandé » et « effectif ».

**Honnêteté du compte à rebours** : les phases firmware sont **relatives** au début de période — à chaque trame (WS 5 s ou `/data`), le client les **ancre** sur sa propre horloge (`dosingAnchor`, app.js) et anime à la seconde (un seul `setInterval` 1 s partagé pH+ORP, actif uniquement quand un décompte est affiché). Précision ±1-2 s (jitter de push + arrondi), resynchronisée à chaque trame. À l'échéance de la période **sans donnée fraîche** → ellipsis (« Prochaine impulsion… »), **jamais** de bascule optimiste vers « Injection en cours », jamais 0 s ni de valeur négative. La seule bascule locale autorisée est **RESTRICTIVE** : impulsion → attente quand la phase ancrée dépasse `pulse_ms` (la pompe s'est arrêtée localement, sans attendre la trame suivante) — le sens inverse exige une donnée serveur.

**Rétrocompat** : champs `pulse_*` absents (firmware < 2.21.1) → comportement historique (`ph/orp_dosing` seul ⇒ « Injection en cours »).

Champs consommés : `pulse_period_ms`, `ph/orp_pulse_session`, `ph/orp_pulse_ms`, `ph/orp_pulse_phase_ms` — contrat identique WS / `GET /data`, voir [ws-manager.md](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2211--calendrier-dimpulsion-adr-0045).

## Données consommées (WebSocket `/ws`)

La carte pH lit :
- `ph`, `ph_target`, `ph_dosing`, `ph_cal_valid`
- Badge multi-états : `ph_limit_reached`, `ph_daily_ml`, `ph_remaining_ml`, `ph_alert_threshold_ml` (+ `max_ph_ml_per_day` de la config)
- Stabilisation : `ph_stab_remaining_s` (repli `stabilization_remaining_s`)
- Pause mélange : `ph_mix_remaining_s` (repli booléen `phMixingDelayActive`)
- Statut dosage v2.21.1 : `pulse_period_ms`, `ph_pulse_session`, `ph_pulse_ms`, `ph_pulse_phase_ms`, `ph_inject_remaining_s` (voir [section dédiée](#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211))

La carte ORP lit :
- `orp`, `orp_target`, `orp_dosing`, `orp_cal_valid`
- Badge multi-états : `orp_limit_reached`, `orp_daily_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml` (+ `max_chlorine_ml_per_day` de la config)
- Stabilisation : `orp_stab_remaining_s` (repli `stabilization_remaining_s`)
- Pause mélange : `orp_mix_remaining_s` (repli booléen `orpMixingDelayActive`)
- Statut dosage v2.21.1 : `pulse_period_ms`, `orp_pulse_session`, `orp_pulse_ms`, `orp_pulse_phase_ms`, `orp_inject_remaining_s`

Les cartes pH et ORP lisent aussi (feature-061 ; **v2.34.0** feature-091) :
- `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_purge_count`, `filtration_state_stale` — segments d'état mesure + gate « Capteur indisponible » / « hors plage » + valeurs « `--` ». ⚠️ `measures_stab_remaining_s` **n'existe plus** depuis la v2.34.0.

La bannière de tête lit (v2.20.3) :
- `phFilterAlarm`, `orpFilterAlarm` — voir [Bannière « Régulation à l'arrêt »](#bannière--régulation-à-larrêt--v2203)

La carte Température lit :
- `temperature`
- `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_purge_count`, `filtration_state_stale` — segments d'état mesure + gate « Capteur indisponible » + valeur « `--` » (feature-061 itération 2 ; champs de purge renommés en v2.34.0)

La carte Filtration lit :
- `filtration_running`, `filtration_mode`, `filtration_start`, `filtration_end`
- Flag d'override optimiste côté JS : `filtrationRunningOverride` (voir mémoire projet)

La carte Éclairage lit :
- `lighting_enabled`, `lighting_schedule_enabled`, `lighting_start`, `lighting_end`

La carte Boost lit :
- `boost_active` (bool), `boost_until` (epoch d'expiration) — voir [Carte Boost](#carte-boost-feature-053)
- `boost_filtration_extended`, `boost_chlorine_boosted` (bool, feature-055) — leviers réellement actifs, pilotent la ligne « Effet »

Les mini-graphiques consomment `GET /get-history?range=24h` au chargement, puis se rafraîchissent par **polling HTTP incrémental toutes les 5 minutes** (`GET /get-history?range=24h&since=<dernier timestamp>`, ajout des nouveaux points en fin de série, avec insertion de la coupure > 90 min le cas échéant) — pas via WebSocket (`loadMiniChartData`, app.js). Depuis la **v2.18.9**, la réponse est **filtrée à minuit local** côté navigateur (seule la journée en cours est tracée) et un **changement de jour civil force un rechargement complet** au lieu de l'incrément — voir [Plage « journée en cours »](#plage-des-mini-graphiques--journée-en-cours-v2189). Le paramètre `range=24h` est conservé tel quel : le contrat HTTP est inchangé. Depuis la **v2.38.0**, la réponse porte en plus `phFiltered` / `orpFiltered` (mesure lissée, `null` au-delà de ~8 h) — voir [Mini-graphiques pH / ORP](#mini-graphiques-ph--orp--la-courbe-suit-le-nombre-affiché-feature-095-v2380).

## Carte Boost (feature-053)

La carte **Boost** (`#dashboard-boost-card`, v2.18.0) permet d'activer d'un geste le **Mode Boost** : surchloration temporaire du jour (filtration prolongée + cible ORP relevée) après une forte fréquentation. Décision et bornes de sécurité : [ADR-0025](../adr/0025-mode-boost.md).

- **Boost inactif** : bouton **Activer le Boost**. Au clic → `POST /boost/start`. À l'activation, un **toast adaptatif** (v2.18.1, feature-054) informe selon les leviers réellement actifs, à partir des booléens `filtration_extended` / `chlorine_boosted` renvoyés par la réponse 200 :

  | `filtration_extended` | `chlorine_boosted` | Toast |
  |---|---|---|
  | ✅ | ✅ | *info* — « Boost activé — vérifiez le taux de chlore avant la baignade. » (message nominal, le contrôleur ne mesure pas le chlore libre ppm) |
  | ❌ | ✅ | *info* — « Boost activé (surchloration seule) : la filtration n'est pas gérée par PoolController. Vérifiez le taux de chlore avant la baignade. » |
  | ✅ | ❌ | *warning* — « Boost activé (filtration prolongée seule) : la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé. » |
  | ❌ | ❌ | *warning* — « Boost sans effet : filtration non gérée et régulation ORP non automatique. » |
- **Boost actif** (`boost_active === true`) : la carte affiche l'**heure d'expiration** dérivée de `boost_until` (prochain minuit local), une **ligne « Effet » persistante** (`#detail-boost-effect-row` / `#detail-boost-effect`, feature-055, v2.18.2 — voir ci-dessous) et un bouton **Arrêter le Boost** → `POST /boost/stop`.

**Ligne « Effet » persistante (feature-055)** — complément **persistant** du toast fugace de la feature-054 (le toast n'apparaît qu'au clic UI, jamais lors d'une activation depuis Home Assistant, et disparaît au rechargement). La ligne indique en permanence les leviers réellement actifs, à partir des booléens WS `boost_filtration_extended` / `boost_chlorine_boosted` (calculés au vol, donc valides après activation HA **et** après reload — voir [ws-manager.md §feature-055](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-feature-055-effet-boost-persistant-v2182)) :

  | `boost_filtration_extended` | `boost_chlorine_boosted` | Libellé « Effet » | Badge |
  |---|---|---|---|
  | ✅ | ✅ | « Filtration prolongée + surchloration » | `--ok` |
  | ❌ | ✅ | « Surchloration seule » | `--ok` |
  | ✅ | ❌ | « Filtration prolongée seule » | `--ok` |
  | ❌ | ❌ | « Sans effet (filtration non gérée, ORP non automatique) » | `--warn` (orange, attire l'œil) |

  Quand le Boost est **inactif**, la ligne « Effet » est masquée (badge de carte `--off`). Au **clic sur Activer**, `updateBoostCard()` mappe `filtration_extended` / `chlorine_boosted` de la réponse `POST /boost/start` vers `latestSensorData.boost_filtration_extended` / `boost_chlorine_boosted` pour un affichage immédiat, sans attendre le prochain push WS (~5 s).

  **Texte d'aide cohérent (`#detail-boost-hint`, v2.18.3)** — le hint sous le widget ne promet plus systématiquement l'effet nominal. Quand le Boost est **actif**, il est masqué (la ligne « Effet » fait foi, plus de contradiction). Quand il est **inactif**, il devient **prospectif** (« À l'activation : … ») et reflète les mêmes booléens WS que la ligne « Effet » : filtration prolongée + cible ORP relevée / cible ORP relevée seule / filtration prolongée seule / aucun effet.
- **Expiration automatique** : le firmware désactive le Boost à minuit local et pousse `boost_active: false` en WS → la carte repasse à l'état inactif sous ≤ 5 s, sans reload.
- **Heure non synchronisée** : `POST /boost/start` renvoie `409 time_not_synced` (l'expiration à minuit serait incalculable) → l'UI affiche un message d'indisponibilité et ne bascule pas la carte.

> **Effet chlore gaté au mode ORP `automatic`** : en mode de régulation ORP Manuel / Programmé, le Boost n'étend que la filtration (la cible et la limite chlore ne sont pas relevées). Comportement porté par le firmware ([pump-controller.md §Mode Boost](../subsystems/pump-controller.md#mode-boost-feature-053)). De même, si la filtration n'est pas gérée par PoolController, le Boost ne prolonge pas la filtration. Le toast adaptatif ci-dessus signale ces cas ; côté firmware, `startBoost()` logue un `warning` par levier inerte (couvre aussi l'activation via Home Assistant).

## Actions

| Bouton | Endpoint HTTP |
|--------|--------------|
| `#detail-filtration-start` | `POST /filtration/on` (ou équivalent) |
| `#detail-filtration-stop` | `POST /filtration/off` |
| `#detail-lighting-on` | `POST /lighting/on` |
| `#detail-lighting-off` | `POST /lighting/off` |
| Boost — Activer | `POST /boost/start` (toast rappel baignade) |
| Boost — Arrêter | `POST /boost/stop` |
| Liens engrenage | Navigation SPA (fragment `#/ph`, etc.) |

La fonction `updateFiltrationBadges()` côté JS met à jour simultanément `#detail-filtration-status` (dashboard) et `#filtration-current-status` (page filtration) pour rester cohérent.

## Cas limites

- **WebSocket déconnecté** : les valeurs affichent `--`. Les mini-graphiques conservent la dernière série.
- **Capteur pH ou ORP en erreur** : segment « Capteur indisponible » (danger, priorité 0) dans le badge multi-états (voir [Badge multi-états](#badge-multi-états-des-cartes-ph--orp-ph-sensor-badge-orp-sensor-badge)), valeur `--` (voir [Convention « `--` »](#convention-----quand-un-capteur-est-indisponible-v2105)).
- **Sonde de température en erreur** (v2.10.5) : badge « Capteur indisponible » (`#temperature-sensor-badge`), valeur `--` partout où la température s'affiche.
- **Filtration arrêtée / purge post-reprise** (feature-061) : valeurs pH/ORP et — depuis l'itération 2 — température à `--`, badge bleu « Filtration arrêtée » ou « Stabilisation depuis N min » sur les trois cartes (voir [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)).
- **Sonde non calibrée** : bandeau `calibration-alert` + chip global en haut de page.
- **Dosage en cours** : badge `dosing-status` affiché dans la carte concernée (motif filtration masqué si le badge capteur le porte déjà — dé-duplication feature-061).

Le détail complet du cumul journalier (barre de progression, mL consommés / quota) reste sur les pages `/ph` et `/orp` ; le dashboard n'en montre que le segment « Cumul XX % » à partir de 75 %.

## Fichiers

- [`data/index.html:162`](../../data/index.html:162) — structure HTML du dashboard
- [`data/app.js`](../../data/app.js) — `latestSensorData`, `updateFiltrationBadges()`, `composeSensorBadge()`, `dailyCumulSegment()`, `getDosingStopReason()`, `updateDosingStatus()` + `dosingAnchor` (v2.21.1), rendu uPlot (`createMiniLineChart`), préférences d'interface `uiPrefGet()` / `uiPrefSet()` + `bindMiniRawToggles()` / `refreshMiniLegendAbsence()` (v2.38.0)
- [`data/uPlot.iife.min.js`](../../data/uPlot.iife.min.js), [`data/uPlot.min.css`](../../data/uPlot.min.css) — bibliothèque uPlot 1.6.32 (voir [BUILD.md](../BUILD.md#bibliothèque-de-graphiques-uplot-frontend))
- [`data/app.css`](../../data/app.css) — classes `.card--status`, `.compact-value`, `.dosing-status`, `.sensor-badge`, `.mini-legend*` (v2.38.0 — pastilles **neutres** à dessein : les traits sont colorés **par la valeur**, une pastille colorée mentirait sur la couleur réelle)
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — `broadcastSensorData()` pousse les champs consommés ici
- [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — `GET /data`, `GET /get-history`

## Specs historiques

- `feature-012-badges-dashboard-cumul-stabilisation` — segment « Cumul XX % », règle de composition du badge multi-états (max 2 segments), stabilisation par pompe (v2.9.0, périmètre réduit sur avis UX)
- `feature-022-observabilite-dosage` — segment « Capteur indisponible » (priorité 0, danger) piloté par `ph`/`orp` null dans le WS, garde anti-faux-positif au boot (v2.10.0)
- `bug-ui-valeurs-figees-capteur-absent` — convention « `--` » généralisée à tous les sites d'affichage numérique (fin du pattern « skip si null ») + badge « Capteur indisponible » température (v2.10.5)
- `bug-minicharts-ecretes-mobile` — graphiques étirés/écrêtés sur iPhone : règle CSS morte Chart.js supprimée, `ResizeObserver` durci (v2.18.3, voir note « Responsive / mobile » dans [Structure](#structure))
- `feature-061-mesures-ph-orp-filtration-arretee` — mesures pH/ORP non représentatives sans circulation d'eau : badges info bleus, valeurs `--`, trous dans les courbes, dé-duplication dosing-status (v2.13.0) ; itération 2 : extension du même traitement à la température (carte/compact `--`, badges, courbe avec trous)
- `ux-minicharts-journee-en-cours` — mini-graphiques de l'accueil limités à la **journée en cours** (minuit local) au lieu des 24 h glissantes, filtrage navigateur avant calcul des coupures, rechargement complet au passage de minuit (v2.18.9, voir [section dédiée](#plage-des-mini-graphiques--journée-en-cours-v2189))
- `bug-dosage-fantome-duty-non-calibre` (itération 3) — statut dosage des cartes pH/ORP distinguant « commandé » (session) de « effectif » (pompe alimentée pendant un pouls) : 6 priorités d'affichage, compte à rebours de la prochaine impulsion ancré localement, dot animé/statique (v2.21.1, [ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md), voir [section dédiée](#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211))
- `feature-095-historiser-la-mesure-filtree` — la **mesure lissée** (celle de la régulation) devient la courbe principale des mini-graphiques pH/ORP, le brut passe en courbe secondaire avec interrupteur mémorisé, frontière visuelle à ~8 h ; les graphiques d'historique des écrans détail restent **bruts** (v2.38.0, [ADR-0064](../adr/0064-la-courbe-suit-le-nombre-affiche.md), voir [section dédiée](#mini-graphiques-ph--orp--la-courbe-suit-le-nombre-affiché-feature-095-v2380))
- `bug-points-futurs-et-ux-courbes` — tooltip toujours daté (`jj/mm hh:mm`) et typé (mesure / moyenne horaire / moyenne journalière), mini-graphiques sur 24 h (était 3 j — plage resserrée à la journée en cours en v2.18.9), coupure de courbe > 90 min entre deux points (arrêts de filtration visibles, incrémental inclus) et jour calendaire manquant sur les écrans détail (v2.18.4, voir [section dédiée](#tooltip-datétypé-et-coupures-de-courbe-v2184-bug-points-futurs-et-ux-courbes)) ; côté firmware : garde anti-futur de l'historique ([history.md](../subsystems/history.md#garde-anti-futur--purge-des-points-futurs-v2184-bug-points-futurs-et-ux-courbes))
