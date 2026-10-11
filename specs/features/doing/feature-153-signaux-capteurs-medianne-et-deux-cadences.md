## Métadonnées
- **ID** : feature-153
- **Slug** : signaux-capteurs-medianne-et-deux-cadences
- **Type** : feature
- **Statut** : doing — implémentation livrée (embedded + web-ui + tests + doc), `pio run` et
  `pio test -e native` verts, hygiène CI verte (11 jobs) ; pas encore clôturée (aucun commit
  référencé à ce stade — voir Notes techniques en fin de fichier)
- **Priorité** : normale
- **Version cible** : 2.69.0
- **Dernière mise à jour** : 2026-10-11
- **Spec(s) liée(s)** : feature-152 (refonte de la chaîne de mesure — cette carte en porte
  encore le vocabulaire retiré), feature-057 (création du tampon de trace), feature-067
  (champ `phCorrected`), feature-095 (`phFiltered`/`orpFiltered` dans l'historique)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] UART / écran LVGL
- [x] OTA
- [x] Authentification
- [x] Historique / persistance *(en LECTURE seule — rien n'est modifié, cf. Hors périmètre)*
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **non** : aucune ligne de dosage, de régulation ni de calibration
      n'est touchée. Cette carte est un instrument d'OBSERVATION. ⚠️ Si l'implémentation
      dérive vers une valeur qui DÉCIDE (seuil, garde, repli), le gate redevient obligatoire.
- [x] ux-ui-designer — **OUI, et sur l'ÉCRAN ENTIER.** La carte est refondue intégralement.
      Son mandat : *évaluer la carte complète et proposer ce qui doit DISPARAÎTRE*, pas
      seulement habiller l'ajout du jour (règle d'accumulation, CLAUDE.md).
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Refondre la carte « Signaux capteurs » de l'onglet Avancé pour qu'elle décrive la chaîne de
mesure **telle qu'elle fonctionne depuis feature-152**, et qu'elle serve son vrai usage :
**vérifier les mesures pH et ORP dans la journée**. Deux cadences au choix — le détail fin sur
une courte fenêtre, ou la journée regroupée — avec **brut, médiane et lissé** sur le même
graphique, plus les min / max / moyenne de la fenêtre affichée.

## Contexte

**Demande utilisateur du 2026-10-11**, mot pour mot sur le cadrage :

> « J'ai l'impression que ce sont des infos liées à l'ancien système. J'aurai besoin toutefois
> de continuer à avoir des données sur les mesures pH et ORP pour vérifier les mesures dans la
> journée. […] Dis moi ce qui est possible de faire sans pour autant devoir garder un nombre
> trop important de données. […] **Adapte toi au fonctionnement actuel du firmware. Ne le
> complique pas pour ce besoin. Cette demande s'adapte au fonctionnement et pas l'inverse.** »

Cette dernière phrase est la **contrainte directrice de la spec**. Elle interdit d'inventer un
mécanisme de collecte : on branche l'interface sur ce que le firmware produit déjà.

### Ce qui existe déjà, et qui sert le besoin — INVENTAIRE VÉRIFIÉ (2026-10-11)

| Source | Où | Contenu | Couverture |
|---|---|---|---|
| Tampon de trace (RAM, volatil) | `SensorManager::_traceBuffer`, `GET /debug/sensor_trace` | `ms`, `phRaw`, `phCorrected`, `phFiltered`, `orpRaw`, `orpFiltered`, `tempEau` | **300 × 28 o = 8,4 Ko**, soit **25 min** à 5 s |
| Historique persisté | `history.*`, `GET /get-history` | `ph`, `orp`, `phFiltered`, `orpFiltered`, `temperature` | **RAW = 5 min sur 8 h** (`kMaxRawDataPoints = 96`), puis horaire sur 7 j, puis quotidien sur 75 j |

**Les deux cadences demandées sont donc DÉJÀ produites.** L'historique est même **plus fin que
demandé** (5 min au lieu de 10) : le regroupement par 10 minutes se fait côté navigateur en
fusionnant deux points, sans stocker un octet de plus.

### Le seul manque réel : la médiane

`phMedian` / `orpMedian` ne sont enregistrés **nulle part** — ni dans `SensorTraceSample`, ni
dans `DataPoint`. Ils ne sont servis qu'en instantané sur `/data` et le WebSocket.

Or la médiane est précisément l'étage que feature-152 a mis au centre : c'est elle qui écarte
les aberrations *par construction*. Pouvoir la comparer au brut et au lissé sur un graphique,
c'est voir la chaîne travailler.

### L'arbitrage mémoire, chiffré et TRANCHÉ PAR L'UTILISATEUR

Ajouter médiane pH + médiane ORP fait passer `SensorTraceSample` de **28 à 36 octets**.
L'utilisateur ayant écrit « Pour la fréquence maximale, on pourrait réduire fortement la
durée », la durée a été échangée contre l'information :

| Points | Mémoire | Couverture | vs aujourd'hui |
|---|---|---|---|
| 300 | 10,8 Ko | 25 min | +2,4 Ko |
| **200 ← RETENU** | **7,2 Ko** | **16 min 40 s** | **−1,2 Ko** |
| 150 | 5,4 Ko | 12,5 min | −3,0 Ko |

⇒ **On gagne une courbe ET on consomme moins qu'avant.**

### Ce qui a été ÉCARTÉ, et pourquoi — à ne pas rouvrir sans raison neuve

**Porter l'historique fin de 8 h à 24 h** (`kMaxRawDataPoints` 96 → 288) a été proposé et
**refusé par l'utilisateur après chiffrage**. Motif : l'allocation **d'un seul tenant** de
`GET /get-history` passerait de **9 492 o à ~14 868 o**, sur un appareil dont le plus gros bloc
libre 8 bits tourne entre **21 et 30 Ko** (référence `constants.h` : 21 492) — et qui a **déjà
crashé sur ce mécanisme précis** (bug-080, `std::bad_alloc` en contexte `async_tcp`).
⇒ Au-delà de 8 h, les **moyennes horaires existantes** prennent le relais sur 7 jours.

## Comportement attendu

### La carte, après refonte

Un **sélecteur de cadence** à deux positions commande l'ensemble de la carte :

**Position « Haute cadence »** — la fenêtre fine, pour juger du bruit et d'une oscillation.
- Source : `GET /debug/sensor_trace` (tampon RAM).
- Un point par cycle capteur, **16 min 40 s** de couverture à la cadence de lecture actuelle.
- ⚠️ La durée affichée **se déduit de la cadence réelle**, elle ne se code pas en dur
  (ADR-0073 : la cadence a déjà changé, le libellé ne doit pas mentir).
- Trois courbes par voie : **brut**, **médiane**, **lissé**.
- Volatil : vidé au redémarrage. Le dire, comme aujourd'hui.

**Position « Journée »** — la vue d'ensemble, pour vérifier la journée.
- Source : `GET /get-history` (déjà persisté).
- Points fins (5 min) **regroupés par 10 minutes côté navigateur** sur les 8 dernières heures,
  puis **moyennes horaires** au-delà.
- 🚨 **UNE SEULE COURBE PAR VOIE** — la **valeur officielle**.
  ⚠️ **CORRECTION D'UNE ERREUR DE CETTE SPEC** (relevée par l'`architect`, vérifiée dans le
  code ET sur l'appareil le 2026-10-11). Une version antérieure annonçait « deux courbes :
  brut et lissé ». **C'est faux depuis feature-152** (commit `fc36951`, `history.cpp:554`) :
  `point.ph`/`point.orp` portent désormais `getPhFiltered()`/`getOrpFiltered()` — la valeur
  officielle, **corrigée côté pH** — et `point.phFiltered`/`point.orpFiltered` **ne sont plus
  écrits** (ils restent à `NAN`, donc jamais sérialisés). Vérifié sur l'appareil :
  `/get-history` rend `"phFiltered": null` sur **tous** les points.
  ⇒ L'erreur venait d'avoir lu la liste des champs de `DataPoint` **sans vérifier lesquels
  sont encore ÉCRITS**. Décision utilisateur : **une courbe suffit** — c'est la valeur sur
  laquelle la régulation décide, donc celle qu'on veut vérifier. Le détail brut/médiane reste
  en Haute cadence.
- ⚠️ **Tolérer** les points anciens qui portent encore `phFiltered`/`orpFiltered` (exports
  antérieurs) : ne pas planter, mais **ne pas en faire une seconde courbe permanente**.
- ⚠️ La bascule fin → horaire doit être **VISIBLE** sur le graphique (changement de densité
  annoncé, pas subi). ✅ **Gratuit** : `/get-history` sert **déjà** `granularity` par point
  (0 = RAW, 1 = horaire, 2 = quotidien) — rien à calculer.

### Statistiques de la fenêtre affichée

Sous les graphiques, pour **chaque courbe affichée** : **min**, **max**, **moyenne**.
**Calculées côté navigateur** sur les points effectivement rendus — aucun champ nouveau, aucun
calcul firmware.

⚠️ Elles décrivent **la fenêtre affichée**, pas « le bassin ». Le libellé doit le dire, sinon
un min/max de 16 minutes se lira comme un extremum de la journée.

### Ce qui DISPARAÎT de la carte

- Les deux lignes « État filtre pH » / « État filtre ORP » et leur ligne d'âge
  (`#trace_filter_ph`, `#trace_filter_orp`, `#trace_filter_age`, `_armFilterStateRefresh`) :
  vestiges de l'ancienne chaîne, et la route `GET /debug/sensor_filter_state` n'a plus d'autre
  consommateur dans `data/`.
- Les libellés hérités « brut / lissé » du titre et de la légende, remplacés par le vocabulaire
  des trois étages réels.

## Cas limites

- **Tampon vide** (après redémarrage, ou après « Vider le buffer ») : message explicite, pas un
  graphique vide muet.
- **Un seul point** : min = max = moyenne. Ne pas afficher d'écart ni de tendance.
- **Valeurs `null`** (sonde muette, porte de mesure fermée, fenêtre en remplissage) : **trou**
  dans la courbe, **jamais** un 0 ni une interpolation. C'est la leçon de feature-095 : une
  absence ne se replie pas sur une valeur.
- **Médiane absente mais brut présent** (fenêtre pas encore pleine au début du tampon) : la
  courbe médiane commence plus tard que la courbe brute. Normal, ne pas le masquer.
- **Historique vide ou partiel** (appareil neuf, partition effacée) : afficher ce qu'il y a.
- **Moins de 8 h d'historique** : pas de bascule horaire à annoncer, la vue s'arrête.
- **Appareil injoignable / 503 sur la garde heap** : message d'erreur distinct de « pas de
  données ». Les deux ne veulent pas dire la même chose.
- **Changement de cadence pendant l'observation** : l'axe temps vient des horodatages, jamais
  d'un pas supposé constant.

## Comportement actuel

La carte affiche aujourd'hui :
- un compteur de points (`#trace_count`) ;
- deux boutons (recharger, vider) ;
- **deux lignes « État filtre pH/ORP » + une ligne d'âge** — vestiges feature-152 ;
- deux graphiques (pH, ORP) avec **deux courbes chacun** : brut et lissé ;
- cinq lignes de statistiques : pH brut, pH lissé, ORP brut, ORP lissé, T° eau.

Une seule cadence (le tampon RAM), aucune vue journée, aucune médiane.

## Contraintes techniques

- ⚠️ **RESPONSIVE** : téléphone **ET** tablette **ET** ordinateur. Concevoir **à 390 px
  d'abord**. Le sélecteur de cadence et la rangée de statistiques sont deux surfaces à risque :
  **compter les caractères** des libellés placés sur une ligne partagée et dire s'ils tiennent.
  Un `flex-wrap` est le cas NORMAL en mobile, pas un repli.
- ⚠️ **ADR-0082, règle 1** : le chemin d'enregistrement de la trace est périodique et tourne
  dans `loopTask`. **Aucune allocation** n'y est tolérée. Les deux champs ajoutés sont des
  `float` dans une structure **déjà allouée statiquement** — l'invariant tient par construction,
  mais il doit être vérifié, pas supposé.
- ⚠️ **Concurrence** : `_traceBuffer` est écrit par `loopTask` (core 1) et lu par un handler
  `AsyncTCP` (core 0) **sans mutex**, par contrat documenté (`sensors.h`). Les deux champs
  ajoutés sont des scalaires alignés : le contrat est préservé. **Ne pas introduire de mutex**
  ici — ce serait un changement de contrat pour un outil de diagnostic.
- ⚠️ **Garde heap de la pagination** : la taille de page est ce qui protège de
  `std::bad_alloc` en contexte `async_tcp` (bug-crash-sensor-trace-badalloc). Deux champs de
  plus **grossissent chaque page** : le nombre d'échantillons par page doit être réajusté pour
  que la réponse reste du même ordre de grandeur.
  ⚠️ **TROIS CHIFFRES PÉRIMÉS SONT EN CIRCULATION** sur cette page — `constants.h:1491` dit
  « ≈ 4,5 Ko », `docs/subsystems/sensors.md:579` dit « ≈ 3,5 Ko », et une version antérieure de
  cette spec disait 4,5 Ko. **Tous faux.** Mesure réelle du **2026-10-11** sur l'appareil
  (`curl /debug/sensor_trace?offset=0 | wc -c`) : **5 905 octets** pour 50 échantillons, dont
  ~83 o d'enveloppe. Les deux champs ajoutés coûtent **+34,7 o/échantillon** (mesuré par
  réinjection dans la page réelle), d'où 50 éch. → ~7 639 o (+29 %). Projection :
  **38 éch. → ~5 825 o**, 40 éch. → ~6 127 o. C'est une projection : la valeur retenue doit
  être **CONFIRMÉE par mesure sur le firmware neuf**, et annoncée comme non confirmée tant que
  ce n'est pas fait. Les trois chiffres périmés sont à corriger dans le même lot (ADR-0073).
- ESP32, offline first. Vanilla JS. Français dans toute l'UI.
- Budget FS `spiffs` **512 Ko** (layout v5) — vérifier avec `./build_fs.sh`.
- AsyncWebServer : aucun handler bloquant > 50 ms.
- uPlot est **déjà embarqué** (`data/uPlot.*`) et déjà utilisé par cette carte : **ne pas
  ajouter de bibliothèque**.

## Fichiers potentiellement concernés

- `src/sensors.h` — `SensorTraceSample` : +`phMedian`, +`orpMedian` (28 → 36 o).
- `src/sensors.cpp` — `_recordSensorTrace()` : deux arguments de plus. La médiane est
  disponible **sur place** (`_phFilter.median()` / `_orpFilter.median()`) au point d'appel
  existant (`sensors.cpp:616`) : **aucune plomberie**, aucun accès nouveau.
- `src/constants.h` — `kSensorTraceBufferSize` 300 → **200** ;
  `kSensorTraceMaxSamplesPerPage` à **réajuster sur mesure**. Commenter la source de chaque
  valeur (ADR-0098).
- `src/web_routes_debug.cpp` — `appendSensorTraceJson` : sérialiser les deux champs (`null` si
  NaN, arrondi pH ×1000 / ORP ×10, comme les champs voisins).
- `data/index.html` — refonte de la carte : sélecteur, graphiques, statistiques ; **suppression**
  des lignes d'état filtre.
- `data/app.js` — chargement des deux sources, regroupement 10 min, calcul min/max/moyenne,
  rendu uPlot ; **suppression** de `_armFilterStateRefresh` et de ses appelants.
- `data/app.css` — styles du sélecteur et de la rangée de statistiques.
- `docs/features/page-settings.md`, `docs/API.md`, `CHANGELOG.md`.

## Livrables — check-list

- [x] A1 — `SensorTraceSample` porte `phMedian` et `orpMedian` ; `_recordSensorTrace()` les
      renseigne depuis `_phFilter.median()` / `_orpFilter.median()` au point d'appel existant.
- [x] A2 — `kSensorTraceBufferSize` = 200, avec commentaire de source (décision produit +
      arbitrage mémoire chiffré, ADR-0098). **Occupation réelle recalculée et écrite.**
- [~] A3 — `kSensorTraceMaxSamplesPerPage` porté à **38**, pour que la réponse reste du même
      ordre que les **5 905 o mesurés le 2026-10-11** (mesure de référence AVANT ce lot).
      ✅ Les **trois chiffres périmés** sont corrigés : `constants.h`, `docs/subsystems/sensors.md`
      (« 3,5 Ko ») et `docs/API.md` (« 4,5 Ko »).
      🔴 **PARTIEL, ET C'EST VOULU : la valeur 38 est une PROJECTION (~5 825 o), PAS une
      mesure.** Le firmware neuf **n'a pas été déployé** — le déploiement exige un feu vert
      utilisateur, non donné à ce stade. La confirmation reste **DUE** :
      `curl -s 'http://192.168.1.48/debug/sensor_trace?offset=0' | wc -c` sur firmware neuf
      **et tampon plein (200 points)**. La réserve « projection non confirmée » est écrite
      dans `constants.h` ET dans `docs/API.md` : **ne pas cocher cet item** tant que la mesure
      n'est pas faite, et **ne jamais présenter l'estimation comme une mesure**.
- [x] A5 — **TRANCHÉ : la trace reste 100 % en échelle BRUTE, l'affichage est CORRIGÉ.**
      Le firmware enregistre `_phFilter.median()` (brut, membres internes) — **jamais**
      `getPhMedian()`, qui applique `applyPhOffset` (ADR-0039 : l'offset s'applique **en sortie
      des accesseurs**). Les trois champs `phRaw` / `phMedian` / `phFiltered` sont donc sur la
      **même référence brute**, `phCorrected` restant le seul porteur de l'offset.
      ⇒ **Côté navigateur** (item B11), l'offset est **ajouté aux trois courbes à l'affichage**,
      de sorte que la Haute cadence, la vue Journée et le tableau de bord soient sur **LA MÊME
      ÉCHELLE** — décision utilisateur du 2026-10-11, motif : pouvoir comparer directement à un
      test manuel (PoolLab, bandelette). Un changement d'offset se répercute **immédiatement**,
      ce qui est exactement le comportement documenté par ADR-0039.
- [x] A6 — **Vérifié, pas supposé** : (i) `_computeMedian()` n'alloue rien (copie sur pile +
      tri par insertion, borne fixe) ⇒ ADR-0082 règle 1 tenue sur le chemin périodique ;
      (ii) les deux champs sont des scalaires alignés ⇒ contrat lock-free de `_traceBuffer`
      intact. **Aucun mutex ajouté.**
- [x] A7 — `test_native_trace_page` code en dur `kTotal = 300` / `kMax = 50` : à mettre à jour
      (constantes de test, pas un contrat) — sinon faux rouge imputé au mauvais endroit.
- [x] A4 — `appendSensorTraceJson()` sérialise les deux champs, `null` si NaN, arrondis alignés
      sur les champs voisins.
- [x] B1 — Sélecteur de cadence à deux positions, commandant toute la carte.
- [x] B2 — Vue « Haute cadence » : 3 courbes par voie (brut / médiane / lissé), durée annoncée
      **déduite de la cadence réelle**, jamais codée en dur. ✅ Gratuit : la route sert **déjà**
      `interval_ms` (mesuré : 5000), simplement **non consommé** par `data/app.js` aujourd'hui.
- [x] B8 — Le libellé **« Δ 25 min » codé en dur** (`app.js:9375`) **disparaît** — il ment
      déjà aujourd'hui, et l'information crête-à-crête reste lisible dans `max − min`.
- [x] B11 — **Échelle d'affichage CORRIGÉE** : l'offset pH (lu dans `/get-config`, déjà chargé
      dans `window._config`) est ajouté aux trois courbes de la Haute cadence. Les deux vues et
      le tableau de bord sont alors sur la même échelle — **aucune marche en basculant de vue**.
      Le libellé doit dire que l'échelle est corrigée.
- [x] B12 — **(ergonomie) Suppressions validées** — la carte gagne 3 courbes sans gagner en
      densité : (a) la ligne de statistique **« T° eau »** sort (orpheline : aucun graphique de
      température sur cette carte, et la température a sa page dédiée avec historique) —
      **choix utilisateur du 2026-10-11** ; (b) les **blocs de légende séparés** disparaissent,
      la pastille de couleur migrant devant le libellé de chaque ligne de statistiques (une
      seule rangée nomme ET chiffre) ; (c) le bouton **« Vider le buffer » devient conditionnel
      à la position Haute cadence** (il agit sur la RAM : l'afficher en vue Journée est une
      action sans effet observable, donc trompeuse) ; (d) le **titre** passe de « Signaux
      capteurs (brut / lissé) » (37 car.) à **« Signaux capteurs »** (16 car.) — la parenthèse
      devient fausse avec trois courbes et coûte cher à 390 px ; (e) le **texte d'intro** devient
      **un texte par position** (l'historique est persisté, le tampon ne l'est pas).
- [x] B13 — **(ergonomie) Un seul sous-titre** au-dessus du bloc de statistiques — « Sur la
      fenêtre affichée (…) » — **jamais répété** sur chaque ligne. Sa durée se **recalcule**
      (B2), elle ne se code pas en dur, même en français.
- [x] B14 — **(ergonomie) Courbe absente ≠ ligne absente** : si la médiane n'est pas encore
      disponible au début d'une fenêtre, **garder la ligne** avec `—` sur ses trois nombres.
      Une ligne qui s'évapore se lit comme un défaut.
- [x] B15 — **(ergonomie) Bascule de granularité marquée HORS du tracé** : trait vertical
      pointillé + étiquette, dessinés dans le hook `draw` d'uPlot (précédent : `tooltipPlugin`,
      `app.js:861`). ⚠️ **Ne PAS** changer le style de la courbe elle-même : on le confondrait
      avec un **trou de mesure** (`null`), qui est précisément ce que B5 rend lisible.
- [x] B16 — **(ergonomie) Cible tactile** : `.segmented__btn` générique n'a **pas** de
      `min-height` garanti ≥ 44 px (seule la variante `--num` l'impose). L'ajouter sur cette
      instance — **vérifier, ne pas supposer que l'existant suffit**.
- [x] B9 — La borne d'agrégation des pages côté client (`for (let i = 0; i < 12; i++)`,
      `app.js:9469`) se **dérive** de `count`/`returned` : elle ne reste pas calée sur
      l'ancien couple 300/50.
- [x] B10 — **Trois messages DISTINCTS** : tampon vide · appareil injoignable · 503 « heap
      fragmenté ». Les trois ne veulent pas dire la même chose.
- [x] B3 — Vue « Journée » : points fins regroupés par 10 min sur 8 h, puis horaire ; **bascule
      de granularité visible** sur le graphique.
- [x] B4 — Min / max / moyenne par courbe affichée, calculés côté client, **libellés comme
      portant sur la fenêtre affichée**.
- [x] B5 — Trous préservés sur les `null` : aucune interpolation, aucun repli sur 0.
- [x] B6 — **Retrait** de `#trace_filter_ph`, `#trace_filter_orp`, `#trace_filter_age`,
      `_armFilterStateRefresh` **ET de leurs orphelins**, inventoriés par grep :
      `loadFilterState`, `_setFilterStateRow`, `_renderDispAge`, `_filterStateLastFetchAt`,
      `_filterStateLoading`, `_dispAgeTickTimer`, `_dispFetchTimer`, `DISPERSION_REFRESH_MS`
      (`app.js:2901`), les deux appels (`app.js:9556` et `:11649`), le commentaire
      `app.js:2898`. **Prouver le zéro-orphelin par grep**, ne pas l'affirmer.
- [x] B7 — Responsive **vérifié à 390 px**, caractères comptés pour le sélecteur et la rangée
      de statistiques.
- [x] C1 — `docs/features/page-settings.md`, `docs/API.md` (champs de `/debug/sensor_trace`,
      nouvelle taille de page), `CHANGELOG.md`.
- [x] D1 — Tests natifs sur la **logique pure extractible** : regroupement par 10 min et calcul
      min/max/moyenne, **y compris avec des `null` au milieu** (ADR-0017).
- [x] D2 — **Mutations** : (i) modifier la largeur de fenêtre du regroupement ; (ii) remplacer
      `null` par `0` dans l'agrégation ; (iii) remplacer `interval_ms` par `5000` en dur.
      **Chacune doit tuer au moins un test**, rapportée une par une.
- [x] D3 — **TRANCHÉ : harnais par EXTRACTION DE TEXTE, câblé en CI.**
      `test/trace_logic_vectors.js` extrait le **texte réel** des trois fonctions de
      `data/app.js` (ancres + comptage de profondeur), l'évalue sous Node ;
      `tools/check_trace_logic_vectors.sh` le lance ; **job 11 de `hygiene-specs.yml`**.
      Calqué sur `check_sha256_vectors.sh` (feature-118). La duplication C++ est **écartée** :
      elle crée deux sources de vérité pour une logique qui vit dans le navigateur — le défaut
      exact que feature-118 a corrigé.
      ⇒ **Contrainte induite sur B** : les trois fonctions doivent être **pures, nommées, sans
      dépendance au DOM ni à `authFetch`**, sinon elles ne sont pas extractibles. Noms figés :
      `groupHistoryPoints(points, windowMs)`, `seriesStats(values)` → `{min, max, avg, n}`,
      `traceWindowDurationMs(sampleCount, intervalMs)`.
- [x] D4 — **ADR à créer** (critère 3, tranché par l'`architect`) : « Tester la logique pure du
      navigateur par extraction de texte, jamais par duplication ». Le sujet ne se lit nulle
      part dans le code, `sha256_vectors.js` est un précédent **isolé** qu'on généralise, et
      l'alternative (duplication C++) est crédible.
      ⚠️ **PAS d'ADR** pour le refus du 24 h : l'`architect` a tranché **non** — le refus est
      conditionné à une contrainte **mesurable et réversible**, et la règle structurante
      (« aucune allocation contiguë non bornée en contexte `async_tcp` ») est **déjà** portée
      par ADR-0029 et bug-080. Un ADR de plus la dupliquerait avec un chiffre qui périmerait.

## Nouvelles signatures / API

- Endpoints HTTP nouveaux : **aucun**. `GET /debug/sensor_trace` et `GET /get-history`
  existent et sont consommés tels quels.
- Champs ajoutés à la réponse de `GET /debug/sensor_trace` : `phMedian`, `orpMedian`
  (`null` si indisponible).
- Champs WebSocket nouveaux : **aucun**.
- Topics MQTT nouveaux : **aucun**.
- Constantes : `kSensorTraceBufferSize` 300 → 200 ; `kSensorTraceMaxSamplesPerPage` réajusté.

## Sécurité chimique

- Dosage impliqué : **non**.
- Limites journalières / horaires concernées : aucune.
- Filtration requise avant action : sans objet.
- Calibration capteur modifiée : **non**.
- Algorithme PID touché : **non**.
- Risque identifié : **aucun**. Cette carte **observe**, elle ne décide de rien. ⚠️ Invariant à
  tenir : aucune valeur produite ici ne doit devenir une entrée de décision.

## ADR attendu ?

- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun. L'arbitrage mémoire (200 points, et le refus du 24 h) est une
  décision **chiffrée et tracée dans cette spec**, pas une contrainte structurante imposée au
  reste du système. ⚠️ Si l'`architect` juge que le refus du 24 h verrouille un choix futur
  (critère 4), qu'il le dise — c'est lui qui tranche, pas cette ligne.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /debug/sensor_trace` — deux champs de plus (`phMedian`, `orpMedian`), taille de page
  modifiée.
- `GET /get-history` — **inchangée**, nouvellement consommée par cette carte.
- `GET /debug/sensor_filter_state` — **perd son dernier consommateur dans `data/`**. La route
  reste ; le dire, ne pas laisser croire qu'elle a disparu.

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-settings.md` — refonte de la carte « Signaux capteurs ».
- `docs/subsystems/sensors.md` — `SensorTraceSample` à 36 o, tampon à 200 points, couverture
  recalculée.

#### Docs transverses
- `docs/API.md` — champs et taille de page de `/debug/sensor_trace`.
- `CHANGELOG.md` — entrée sous la version cible.

## Critères d'acceptation

- [x] Le sélecteur bascule entre les deux cadences sans rechargement de page.
- [x] En haute cadence, les **trois** courbes sont présentes par voie, et la durée annoncée
      correspond à la cadence réelle (vérifié en changeant la cadence dans un test).
- [x] En vue journée, les points des 8 dernières heures sont regroupés par 10 min, et la
      bascule vers l'horaire est **visible**.
- [x] Un `null` au milieu d'une série produit un **trou**, jamais un 0 ni une interpolation.
- [x] Les min/max/moyenne correspondent aux points **réellement affichés**, et leur libellé dit
      qu'ils portent sur la fenêtre affichée.
- [x] Aucune trace des lignes d'état filtre ni de `_armFilterStateRefresh` dans `data/`.
- [x] `GET /debug/sensor_trace` : page réelle **mesurée**, du même ordre de grandeur qu'avant.
- [x] Occupation du tampon **mesurée** et conforme (200 × 36 = 7 200 o), inférieure aux 8 400 o
      actuels.
- [x] Tous les libellés utilisateur sont en français.
- [x] `pio run` compile sans nouveau warning.
- [x] `./build_fs.sh` passe sans dépasser la partition.
- [x] `pio test -e native` : zéro régression.
- [x] Affichage vérifié **à 390 px de large**.

## Plan de test manuel

1. **Nominal haute cadence** : ouvrir l'onglet Avancé, laisser tourner 17 min, vérifier que la
   fenêtre glisse et que les trois courbes sont cohérentes (médiane entre brut et lissé en
   régime bruité).
2. **Nominal journée** : basculer, vérifier la couverture 8 h en points fins puis l'horaire.
3. **Trou de mesure** : arrêter la filtration (porte de mesure fermée → valeurs `null`),
   vérifier que la courbe se **coupe** au lieu de retomber à 0.
4. **Tampon vide** : « Vider le buffer », vérifier le message explicite.
5. **Après redémarrage** : vérifier que la haute cadence repart de zéro et que la vue journée,
   elle, a survécu (persistée).
6. **Téléphone, 390 px** : vérifier le sélecteur, la légende et la rangée de statistiques.
7. **Contre-épreuve du cas réel** : sortir la sonde pH, la replonger — sur le graphique haute
   cadence, voir la médiane écarter l'excursion et la valeur lissée revenir **sans
   intervention**. C'est le geste qui a motivé feature-152 ; cette carte doit le rendre
   observable.

## Notes pour agent

- ⚠️ **La contrainte directrice est celle de l'utilisateur** : « Cette demande s'adapte au
  fonctionnement et pas l'inverse. » Le réflexe sera d'ajouter un mécanisme de collecte dédié.
  **C'est exactement ce qu'il ne faut pas faire.** Tout est déjà produit sauf la médiane.
- ⚠️ **ADR-0073 — ne recopier aucun compteur.** Les chiffres de cette spec (8,4 Ko, 25 min,
  9 492 o, 21 492) ont été **mesurés le 2026-10-11**. Les re-mesurer avant de les réécrire.
- ⚠️ La **cadence de lecture n'est pas un acquis** : feature-152 a délibérément désolidarisé la
  chaîne de mesure de toute durée codée en dur. Une durée affichée se **calcule**, et cette
  propriété doit être **tuée par une mutation** (changer la cadence doit changer le libellé).
- ⚠️ `#trace_filter_*` et `_armFilterStateRefresh` viennent d'être **renommés** (feature-152,
  ex-`#trace_disp_*` / `_armDispersionRefresh`). Ils sont ici **supprimés**, pas renommés une
  fois de plus.
- La carte vit dans l'onglet Avancé des Réglages — un écran déjà dense. `ux-ui-designer` juge
  **la carte entière**, et a mandat pour proposer des **suppressions**.
- Une **capture d'écran** sera à refaire : `screenshots/10_Paramètres_8_Avancé_2.jpg`. Elle
  l'était déjà au titre de feature-152 (non encore refaite) — **une seule reprise couvre les
  deux**. Le signaler à l'utilisateur plutôt que de le lui faire faire deux fois.

## Hors périmètre

- **Porter l'historique fin au-delà de 8 h** — chiffré, proposé, **refusé par l'utilisateur**
  (cf. Contexte). Ne pas le rouvrir sans élément nouveau.
- **Ajouter la médiane à `DataPoint`** (historique persisté) : coûterait 8 o × 339 points sur
  une partition déjà dimensionnée, pour une grandeur de diagnostic court terme. La vue journée
  se contente de brut + lissé.
- **Persister le tampon de trace** : il est volatil par conception, et doit le rester.
- **Toute modification de la chaîne de mesure elle-même** (feature-152 est close).
- **Le retrait de `GET /debug/sensor_filter_state`** : la route perd son dernier consommateur
  `data/` mais reste exposée. Son sort est une décision produit distincte — comme celui de
  `POST /reset-sensor-filter`, laissé ouvert à l'utilisateur en clôture de feature-152.

## Notes techniques (doc-writer, 2026-10-11)

- **Chiffres mesurés de ce lot** : `sizeof(SensorTraceSample)` figé à **36 o** par
  `static_assert` ; tampon `200 × 36 = 7 200 o` (sous les 8 400 o d'avant) ; couverture
  **16 min 40 s** à la cadence actuelle (5 s), déduite et non codée en dur ; page de route
  réajustée à **38** échantillons. La taille de page JSON (`~5 825 o`) reste une **projection
  non confirmée** — `constants.h`, `docs/subsystems/sensors.md` et `docs/API.md` le disent
  explicitement, à re-mesurer sur le firmware déployé avant de la recopier ailleurs (ADR-0073).
- **ADR-0099 créé** : « Tester la logique pure du navigateur par extraction de texte, jamais
  par duplication » — généralise le patron `sha256_vectors.js` (feature-118), câblé en CI
  (job 11 `trace-logic-vectors` de `hygiene-specs.yml`).
- **Pas d'ADR pour le refus du 24 h** : confirmé conforme à l'arbitrage de l'`architect` — la
  règle structurante est déjà portée par ADR-0029 et bug-080.
- **Documentation mise à jour** : `docs/features/page-settings.md` (section carte refondue),
  `docs/subsystems/sensors.md` (chiffres C2, anchor interne corrigée), `docs/API.md` (champs
  `phMedian`/`orpMedian`, taille de page, note sur `/debug/sensor_filter_state`),
  `docs/BUILD.md` (onzième job CI), `CHANGELOG.md` (entrée `[2.69.0]`), `docs/README.md`
  (index ADR complété pour 0096-0099), `docs/user/src/guide-utilisation.html` (texte source,
  **aucun PDF régénéré** — réservé à la release). `src/version.h` porté à `2.69.0`.
- **Capture d'écran à refaire** (signalée, pas effectuée par cet agent) :
  `screenshots/10_Paramètres_8_Avancé_2.jpg` — déjà due depuis feature-152, une seule reprise
  couvre les deux lots.
- **Clôture non effectuée** : `pio run` / `pio test -e native` (67 suites / 2 098 cas) /
  hygiène CI (11 jobs) sont verts et tous les critères d'acceptation et livrables sont cochés,
  mais **aucun commit n'a été créé à ce stade** (règle du dépôt : jamais de commit sans demande
  explicite). La spec reste donc dans `doing/` — à déplacer vers `done/` une fois le commit
  (et le `git mv`) effectués, en relançant `./tools/check_doc_links.sh` **après** le
  déplacement, pas avant.
