## Métadonnées
- **ID** : feature-095 (= item **N4** du plan `specs/plan-2026-09-regulation-et-robustesse.md`)
- **Slug** : historiser-la-mesure-filtree
- **Type** : feature
- **Statut** : **done — livré non déployé** (v2.38.0, 05/09/2026) — tests verts, revue OK ; l'appareil tourne toujours en 2.35.0, les versions 2.36.0 / 2.37.0 / 2.38.0 sont commitées et **non déployées**
- **Priorité** : haute
- **Version cible** : 2.38.0 (l'appareil tourne en 2.35.0 ; 2.36.0 — feature-094 — et
  2.37.0 — feature-096 — sont commitées mais NON déployées ; cette spec porte la 2.38.0)
- **Dernière mise à jour** : 2026-09-05
- **Spec(s) liée(s)** : plan 2026-09 (N4, N5, N7, N9), feature-094 (signal de décision), feature-096 (KPI temps d'atteinte), bug-089 (parité WS `config`, même cycle)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — lecture seule de la filtrée
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (`/get-history`)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`) — courbes pH/ORP


> ⚠️ **Prérequis technique du 04/09/2026 — ANNULÉ le 05/09/2026 : il était faux
> ET sans objet ici.**
>
> ~~« relever le gabarit `/data` avant d'ajouter le moindre champ, sinon
> sérialisation tronquée silencieusement »~~ — deux erreurs superposées :
>
> 1. **Il n'existe aucune troncature silencieuse sur `/data`.** En ArduinoJson 7,
>    `StaticJson<N>` dérive de `JsonDocument` et `N` est **purement documentaire**
>    (le pool est alloué dynamiquement) ; `sendJsonResponse` sérialise dans une
>    `String` dimensionnée à la demande. Ce chemin n'a **aucun plafond dur** —
>    c'est écrit dans `src/web_routes_data.cpp` lignes 57-62 et 75-77. Le seul
>    plafond **DUR** du projet est `kWsFrameBufBytes = 3584` (`src/constants.h:178`),
>    côté WebSocket : sa garde `measureJson(doc) + 1 > cap` (`ws_manager.cpp:285`,
>    `:666`, `:761`) **SAUTE le push** et incrémente `_oversizedFrames` (warn
>    throttlé) — elle ne tronque **jamais**. Rectification déjà portée dans
>    `specs/features/done/feature-096-indicateur-temps-d-atteinte-de-cible.md`.
> 2. **Ce bloc est entièrement sans objet pour la feature-095** : cette spec
>    **n'ajoute aucun champ à `/data` ni au WebSocket**. Ses livrables portent sur
>    `/get-history`, sur la structure `DataPoint` et sur sa persistance
>    (`/history.json`). Sa propre section « Nouvelles signatures » le dit déjà :
>    « Aucun endpoint, aucun topic MQTT, aucun champ WS d'état nouveau ».
>
> **Les vrais points durs de cette spec sont ailleurs** — partition `history`
> 64 Ko et `kHistoryPointJsonMaxBytes` : voir « Cas limites » et le livrable A4.

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **NON REQUIS pour le cœur de 095, confirmé le 05/09** : aucun
  chemin de dosage, aucune calibration, aucun fichier `pump_controller.*` touché ;
  lecture seule de `sensors.getPhFiltered()` / `getOrpFiltered()`, accesseurs **déjà**
  consommés par `/data` (`web_routes_data.cpp:136-137`). **Devient OBLIGATOIRE si et
  seulement si** l'architect retient l'historisation de `pd` : cela créerait un
  **nouveau consommateur** de `PumpController::getPhDecisionValue()`, accesseur
  porteur d'un contrat explicite (loopTask uniquement, sens unique, garde par grep
  bidirectionnel — `pump_controller.h:1168-1187`).
- [x] ux-ui-designer — arbitrage « deux courbes (brut + filtrée) ou une seule » sur les graphiques pH/ORP
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
L'historique 5 min enregistre aujourd'hui le pH **brut + offset** et l'ORP brut.
La régulation, elle, travaille sur la valeur **filtrée** (médiane + EMA). On
veut historiser la filtrée à côté du brut, pour que chaque analyse de la
régulation (temps d'atteinte, bruit, rebonds, couplage Javel → pH) puisse se
faire depuis `/get-history` sans enregistreur externe.

## Contexte
Toutes les analyses du plan 2026-09 (02, 03 et 04/09) ont exigé un script
externe qui interroge `/data` toutes les 10-60 s depuis un Mac qui doit rester
éveillé (une mise en veille a déjà troué l'enregistrement du 04/09). Sans la
filtrée dans l'historique :
- on ne peut pas juger si 094 (porte de session robuste au bruit) marche ;
- on ne peut pas mesurer N7 (dérive nocturne) ni N9 (couplage Javel → pH) ;
- le KPI N5 (feature-096) n'a aucune donnée persistée pour être recalculé
  a posteriori.

Le brut reste indispensable (diagnostic sonde, N1 : transitoire de reprise) :
**on ajoute, on ne remplace pas**.

## Comportement attendu
- Chaque point RAW (5 min) porte, en plus de `p` (brut + offset) et `o` (ORP
  brut), la filtrée pH `pf` et la filtrée ORP `of` telles que vues par la
  régulation à l'instant de l'enregistrement (mêmes valeurs que `ph` / `orp`
  de `/data`, arrondies à 0,01 / 1 mV).
- Les agrégats HOURLY et DAILY moyennent `pf` / `of` comme `p` / `o` (même
  fonction de fusion, `historyMergeMeanFinite`).
- Quand la filtrée n'existe pas (porte de mesure fermée, filtres non amorcés,
  sonde HS) : champ **absent** du point (pas 0, pas null), comme `p` aujourd'hui
  quand le brut est NaN.
- `/get-history` sert `pf` / `of` sur toutes les plages ; les points anciens
  (enregistrés avant 2.35) ne les ont pas — le client tolère l'absence.
- Graphiques pH et ORP : la filtrée est la courbe **principale** (c'est ce que
  la régulation voit et ce que la consigne compare) ; le brut passe en courbe
  secondaire fine/pointillée, désactivable par un interrupteur mémorisé
  (`localStorage`), défaut : visible. Choix final par `ux-ui-designer`.
- Les repères des graphiques (consigne, bande, seuils fixes ADR-0048) se lisent
  par rapport à la filtrée.
- **Question réelle, à trancher par l'architect — historiser le signal lent
  `pd` ?** ~~« 094 est prévue après N4/N5 dans le même lot v2.35 »~~ **PÉRIMÉ
  (05/09)** : **feature-094 est LIVRÉE** (v2.36.0, commit `ee227af`, non
  déployée) et expose déjà `PumpController::getPhDecisionValue()`
  (`src/pump_controller.h:1185`) — moyenne glissante de `kPhDecisionWindowMs`
  de la filtrée, `NAN` tant que la fenêtre n'est pas pleine. **`pd` existe et
  diffère de `pf`** : l'hypothèse d'échappatoire « si `pd == pf`, ne rien
  ajouter » ne s'applique pas. L'arbitrage est donc un vrai choix, à faire
  contre le budget de A4 (un 3ᵉ champ = +~10 o/point) et sous les deux
  réserves du contrat de l'accesseur (voir « Notes pour agent »).

### Cas limites
- Point enregistré pendant la purge de stabilisation (091) : `pf`/`of` absents,
  `p`/`o` présents (le brut est enregistré pendant la purge ? — **non** : le
  comportement actuel « `measuresValid=false` → pas d'enregistrement » est
  conservé ; rien ne change sur ce point).
- Redémarrage : la filtrée met ~65 s à s'amorcer ; le premier point 5 min a
  presque toujours une filtrée. Pas de cas spécial.
- Fichier `/history.json` écrit par 2.34 relu par 2.35 : champs absents →
  `NaN` en mémoire, réécrits absents. Relecture 2.35 → 2.34 (retour arrière
  OTA) : clés inconnues ignorées par ArduinoJson. Aucune migration.
- **Partition `history` 64 Ko — chiffres MESURÉS le 05/09, ne pas les
  redécouvrir** : point persisté actuel **69 o** (`history.cpp:462-468`, clés
  courtes `t,p,o,T,f,d,g`) ; avec les 2 nouveaux champs **90 o**, soit
  **+21 o/point**. Sur 339 points max (96 RAW + 168 HOURLY + 75 DAILY,
  `constants.h:113-115`) → **+7 119 o**. `/history.json` pire cas
  **23,4 Ko → 30,5 Ko**. La marge de sauvegarde tombe de **~10 Ko à ~3,5 Ko**
  (`kHistorySaveSpaceMarginBytes = 8192` est déjà déduit).
  → **Le budget passe, mais la marge devient mince** : l'architect doit décider
  explicitement s'il l'accepte ou s'il réduit `kMaxHourlyDataPoints`
  (7 j → 5 j). Une réduction est une décision ADR (critère 2).
  ⚠️ **Incohérence de commentaire signalée, À NE PAS CORRIGER dans ce cycle**
  (hors périmètre, ne rien toucher) : `src/history.h:31` annonce « 15 jours de
  moyennes horaires » alors que `kMaxHourlyDataPoints = 168` vaut **7 jours**
  (`constants.h:114`). Ne pas raisonner sur ce commentaire.
- **`kHistoryPointJsonMaxBytes = 128` (`constants.h:248`) est DÉPASSÉ dans les
  deux hypothèses de nommage** — c'est le vrai point dur. Point exporté actuel
  ~**112 o** (clés longues `timestamp,ph,orp,temperature,…` de
  `formatHistoryPointJson`) ; avec 2 champs en **clés courtes** (`pf`/`of`) :
  **132 o** ; en **clés longues** (`phFiltered`/`orpFiltered`) : **149 o**.
  → Relever la constante ET le budget du chunk d'export
  (`history_export_chunk.h`) est **obligatoire**, quel que soit le nommage
  retenu ; le nommage n'est qu'un arbitrage de coût.
- **Mode de défaillance du budget partition — à connaître** : un dépassement ne
  corrompt **rien**. `historySaveAdmit()` **refuse la sauvegarde** avant de
  toucher au fichier et journalise un WARNING throttlé 1/h
  (`history.cpp:489-502`). Conséquence réelle : l'historique cesse
  **silencieusement** d'être persisté (l'UI continue d'afficher la RAM ; la
  perte n'apparaît qu'au reboot). C'est un mode de panne **discret**, pas
  bruyant : le critère d'acceptation sur l'occupation est donc bloquant.
- **Budget flash `data/` non concerné** : `data/` va en partition `spiffs`
  (**148 Ko libres**) — les modifications d'UI ne consomment pas le budget
  critique de la partition `history`.

## Comportement actuel
`HistoryManager::recordDataPoint()` (`src/history.cpp:402-406`) enregistre
`sensors.getPh()` (brut + offset), `sensors.getOrp()`, la température, les
états filtration/dosage. La filtrée n'est nulle part dans l'historique. Les
courbes affichent donc une valeur que la régulation **ne voit pas**, ce qui a
déjà induit deux mauvais diagnostics (mémoire projet « historique = brut+offset »).

## Contraintes techniques
- ESP32, offline first ; ArduinoJson 7
- Partition `history` 64 Ko (layout v4, ADR-0024) — le point dur de cette spec
- `struct DataPoint` grandit de 8 o (2 floats) × 339 points max ≈ 2,7 Ko de RAM
  interne supplémentaire — acceptable mais à mentionner dans le bilan heap
  (bug-080 : famine 8 bits en cours de traitement)
- Aucun handler bloquant > 50 ms ; l'export reste en chunks
- Français dans toute l'UI

## Fichiers potentiellement concernés
- `src/history.h` — `DataPoint.phFiltered`, `DataPoint.orpFiltered`
- `src/history.cpp` — enregistrement (`sensors.getPhFilteredRaw()` ou
  l'accesseur exact de la filtrée exposée dans `/data`), sérialisation
  `pf`/`of`, désérialisation tolérante, fusion des agrégats
- `src/history_logic.{h,cpp}` — moyenne équipondérée étendue aux 2 champs
  (module pur, tests natifs)
- `src/history_export_chunk.h` — budget par point
- `src/web_routes_data.cpp` — `formatHistoryPointJson` (export `/get-history`) **et**
  `/import-history` (relecture des 2 nouvelles clés, cf. A2bis)
- `src/constants.h` — `kHistoryPointJsonMaxBytes` si nécessaire ;
  `kMaxHourlyDataPoints` si arbitrage budget
- `data/app.js` — datasets filtrée/brut, interrupteur, repères sur la filtrée
- `data/index.html` — interrupteur « brut »
- `docs/API.md`, `docs/subsystems/history.md`, `docs/features/page-ph.md`,
  `page-orp.md`, `page-dashboard.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — `DataPoint` étendu (2 floats) ; enregistrement de la filtrée pH/ORP dans `recordDataPoint()`
- [x] A1bis — **Initialiseurs de membre `= NAN` sur les nouveaux champs de `DataPoint`** (`src/history.h:16-25`). **Non optionnel** : `DataPoint` est construit **sans initialisation** en deux endroits qui n'initialisent que les champs qu'ils connaissent — `/import-history` (`web_routes_data.cpp:956-967`) et `avgPoint` dans `consolidateData()` (`history.cpp:854-862` HOURLY et `929-937` DAILY). Sans initialiseurs, on **persiste de la mémoire non initialisée** en `pf`/`of` (valeurs fantômes affichées comme des mesures réelles). Idem si un 3ᵉ champ `pd` est retenu.
- [x] A2 — Sérialisation `pf`/`of` (absents si NaN) et désérialisation tolérante
- [x] A2bis — **`/import-history` (`web_routes_data.cpp:956-967`) relit les 2 nouvelles clés.** Absent de la spec initiale, **régression silencieuse** sinon : restaurer une sauvegarde JSON contenant la filtrée la **perd sans le moindre message** (les points sont réimportés, la filtrée devient NaN). Choisir les clés d'import **cohérentes avec l'export** de `formatHistoryPointJson` (l'import lit aujourd'hui les clés LONGUES `ph`/`orp`/`temperature`, pas les clés courtes du fichier persisté — ne pas confondre les deux formats).
- [x] A3 — Fusion des agrégats HOURLY/DAILY étendue aux 2 champs (module pur)
- [x] A4 — Budget : relever `kHistoryPointJsonMaxBytes` (≥ 132 o clés courtes / ≥ 149 o clés longues) + budget du chunk d'export, occupation partition chiffrée avant/après (23,4 → 30,5 Ko attendus), décision explicite sur `kMaxHourlyDataPoints`
- [x] B1 — Courbes pH/ORP : filtrée principale, brut secondaire avec interrupteur mémorisé
- [x] B2 — Repères rapportés à la lissée — **périmètre réduit en connaissance de cause** ([ADR-0064](../../../docs/adr/0064-la-courbe-suit-le-nombre-affiche.md)) : sur les **mini-graphiques**, la coloration conditionnelle et le repli d'échelle suivent désormais la **série principale (lissée)** ; sur les **graphiques de détail**, les repères (bande 7,2-7,6, ligne de consigne) restent rapportés au **brut** — ils agrègent 1 point/jour, granularité où la lissée vaut toujours `null`
- [x] C1 — `docs/API.md` (`/get-history` : `pf`, `of`), `docs/subsystems/history.md`, pages features, CHANGELOG
- [x] D1 — Tests natifs `test_native_history` : sérialisation/désérialisation avec et sans `pf`/`of`, fusion des moyennes avec un champ absent, mutation (retirer `pf` de la fusion → le test meurt)

## Nouvelles signatures / API (optionnelle)
- `/get-history` : clés `pf` (float, 2 déc.) et `of` (float, 1 déc.) sur chaque point, absentes si indisponibles
- Aucun endpoint, aucun topic MQTT, aucun champ WS d'état nouveau

## Sécurité chimique (optionnelle)
- Dosage impliqué : non
- Algorithme PID touché : non
- Risque identifié : aucun (lecture seule)

## ADR attendu ?
- **Probable** : oui si arbitrage budget (réduction de la profondeur horaire) ; non sinon
- **Critère(s) candidat(s)** : 2 (compromis profondeur d'historique ↔ richesse du point)
- **Sujet pressenti** : « la filtrée est la mesure de référence des courbes ; le brut devient diagnostic »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-history` — 2 clés par point

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-ph.md`, `page-orp.md`, `page-dashboard.md` — courbe principale = filtrée, interrupteur brut
- `docs/subsystems/history.md` — format du point, budget partition
#### Docs transverses
- `docs/API.md` — `/get-history`
- `CHANGELOG.md` — 2.38.0
- `docs/user/src/guide-utilisation.html` → PDF (les courbes changent de sens) ; capture d'écran page pH à refaire

## Critères d'acceptation
- [ ] ⏳ **après déploiement** — Un point RAW enregistré pendant une régulation active contient `pf` et `of` égaux (à l'arrondi) aux `ph`/`orp` de `/data` au même instant *(le code lit les mêmes accesseurs que `/data`, `history.cpp:407-408` ; reste à constater sur l'appareil)*
- [x] Un point enregistré filtres non amorcés ne contient ni `pf` ni `of`, mais contient `p`/`o` — couvert par `test_native_history` (NaN ⇒ clé omise dans le persisté, `null` sur le fil)
- [x] `/history.json` écrit par une version antérieure est relu sans erreur ni perte de points — désérialisation tolérante (`point["pf"] | NAN`), aucune migration
- [ ] ⏳ **après déploiement (24 h)** — Occupation de la partition history < capacité − `kHistorySaveSpaceMarginBytes` ; **aucun WARNING « Partition history pleine : save sauté »**. *Calculé et documenté : 12/16 blocs, résiduel 8 102 o — voir A4 ; reste la confirmation terrain, critère **bloquant** car le mode de panne est silencieux*
- [ ] ⏳ **après déploiement** — **Aller-retour complet** `/get-history` → `POST /history/import` → `/get-history` restitue `pf`/`of` à l'identique *(les deux extrémités du chemin sont livrées et le format est verrouillé par test ; la boucle complète passe par le firmware, non testable en natif)*
- [x] **Un point importé ou agrégé sans filtrée porte `pf`/`of` ABSENTS**, jamais une valeur fantôme — initialiseurs de membre `= NAN` (A1bis) + test construisant `DataPoint` sans initialisation explicite
- [x] Les courbes pH/ORP affichent la lissée en principal ; l'interrupteur « Brut » survit au rechargement (`localStorage`, `poolctl.ui.showRaw.*`, accès protégés en lecture **et** en écriture)
- [x] Tous les libellés utilisateur sont en français — et disent **« Lissé » / « Brut »**, jamais « filtrée » (terme d'API)
- [x] `pio run` compile sans nouveau warning ; `pio test -e native` vert (**31 suites / 1 226 cas**) ; **7 mutations tuées**
- [ ] ⏳ **à l'étape de déploiement** — `./build_fs.sh` passe sans dépasser la partition `spiffs` *(148 Ko libres avant ce lot ; `data/` n'entame pas le budget critique de la partition `history`)*

## Plan de test manuel
1. Après OTA, attendre 10 min : `/get-history?range=1h` → les 2 derniers points ont `pf`/`of`, les précédents (pré-2.35) non ; courbe pH continue sans trou visuel.
2. Forcer une purge (POST `/debug/reset` pendant filtration ou attendre la reprise du matin) : point suivant sans `pf` si la purge dure > 5 min.
3. Basculer l'interrupteur « brut », recharger : état conservé.

## Notes pour agent
- Le brut historisé est **brut + offset** (feature-067) — ne pas changer.
- Parité : ce cycle n'ajoute **aucun** champ WS ni `/data` ; bug-089 (parité de la trame `config`) reste une spec distincte du même lot de déploiement.
- **Si `pd` est retenu** : `getPhDecisionValue()` est réservée à **loopTask** — `recordDataPoint()` est appelée depuis la loop, donc compatible ; **vérifier** qu'aucun handler AsyncWebServer ne l'atteint par un chemin détourné, et **relancer le grep bidirectionnel** de sens unique (`pump_controller.h:1182-1184`). Gate `pool-chemistry` obligatoire dans ce cas.
- **Argument contre `pd`** : il est indexé sur `kPhDecisionWindowMs` (ADR-0063) ; l'historiser importe cette contrainte dans une série **longue durée** — toute modification future de la constante rendrait l'historique non comparable, sans l'estampille de série dont dispose l'anneau KPI de 096.
- Ne pas confondre avec le tampon `sensor_trace` (anneau brut 5 s, 25 min, RAM) qui reste l'outil de N1.

## Hors périmètre
- Historiser l'intégrale, le FF ou les volumes injectés par point (idée pour N9 ; spec séparée si le besoin se confirme).
- Export CSV.

---

## Notes d'implémentation (05/09/2026, v2.38.0 — livré non déployé)

### Arbitrages retenus (par rapport à la spec initiale)

1. **La lissée n'est historisée QUE sur les points RAW.** La spec prévoyait « les agrégats HOURLY et DAILY moyennent `pf`/`of` comme `p`/`o` » : **écarté**, sur chiffres. Toutes granularités = **+7 119 o** ⇒ **16/16 blocs** ⇒ `historySaveAdmit()` refuse la sauvegarde ⇒ l'historique cesse **silencieusement** d'être persisté. RAW seul = **+2 016 o**, **12/16 blocs**, résiduel **8 102 o**, profondeur horaire **intacte** (aucune réduction de `kMaxHourlyDataPoints`, contrairement à l'option envisagée en A4). Second argument, indépendant du budget : la lissée est un signal passe-bas — sa moyenne horaire **coïncide** avec celle du brut, la série aurait été redondante.
2. **`pd` (signal lent de décision) NON historisé.** Dérivable de la lissée, il aurait importé `kPhDecisionWindowMs` dans une série longue durée sans estampille, coûté un 3ᵉ champ sur un budget déjà arbitré, et créé un nouveau consommateur de `getPhDecisionValue()` ⇒ **gate `pool-chemistry` obligatoire pour zéro information nouvelle**. `pool-chemistry` n'a donc **pas** été requis sur ce cycle.
3. **Asymétrie fichier / fil, voulue.** `/history.json` : clés **courtes** (`pf`, `of`), clé **omise** si non finie. `/get-history` : clés **longues**, **toujours présentes**, `null` si absente — contrat existant de `ph`/`orp`. La spec disait « champ absent, pas null » : vrai pour le **fichier**, faux pour le **fil**, où l'omission aurait forcé chaque client à distinguer deux formes d'absence.
4. **Écrans détail hors périmètre** : `createLineChart` reste **mono-série** (réduction de risque du lot). Seuls les mini-graphiques du tableau de bord portent les deux courbes.

### Correction embarquée dans le lot (hors spec initiale)

**Perte silencieuse de points de `/get-history`** : un point dont le JSON dépassait `kHistoryPointJsonMaxBytes` était **sauté sans aucun log**, en produisant un JSON valide — trou de courbe **indiscernable d'une panne capteur**. Désormais : compteur `HistoryDownloadState::skipped` + **un seul** WARNING à la clôture, throttlé 1/h (`kHistorySkipWarnThrottleMs`), message actionnable. `kHistoryPointJsonMaxBytes` **128 → 192** (pire cas réaliste recompté : **161 o**, dont 114 de clés/ponctuation ; pire cas théorique 213 o documenté et assumé).

### Chiffres

| | Valeur |
|---|---|
| Flash | **1 821 097 / 1 835 008 o** — marge **13 911 o** |
| Partition `history` | **12 / 16 blocs**, résiduel **8 102 o** |
| Point RAW persisté | 68 → **90 o** (+21 o mesurés) |
| `/history.json` pire cas (339 points) | 23 497 → **25 513 o** (+2 016 o, 96 points RAW) |
| `sizeof(DataPoint)` | 20 → **28 o** (verrouillé par `static_assert`) — allocation contiguë `/get-history` = **9 492 o** |
| Tests | **31 suites / 1 226 cas** — **7 mutations tuées** |

### Fichiers modifiés

- Firmware : `src/history.h`, `src/history.cpp`, `src/history_export_chunk.h`, `src/web_routes_data.cpp`, `src/constants.h`, `src/version.h`
- Frontend : `data/app.js`, `data/app.css`, `data/index.html`
- Tests : `test/test_native_history/test_history_logic.cpp`, `test/test_native_history_export/test_history_export.cpp`
- Docs : `docs/adr/0064-la-courbe-suit-le-nombre-affiche.md` (**nouveau**), `docs/adr/README.md`, `docs/README.md`, `docs/API.md`, `docs/subsystems/history.md`, `docs/features/page-dashboard.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/user/src/guide-utilisation.html`, `docs/user/README.md`, `CHANGELOG.md`
- PDF : `docs/user/guide-d-utilisation-v2.38.pdf`, `docs/user/guide-regulation-v2.38.pdf` (les v2.37 sont supprimés). **Capture `screenshots/03_Tableau_de_bord.jpg` à refaire** — elle ne montre pas encore les légendes et l'interrupteur « Brut ».

### Specs de suivi à ouvrir (constatées pendant ce cycle, hors périmètre)

1. **Le drapeau `orpDosing` est perdu à l'écriture.** `saveToFile()` fusionne les deux drapeaux de dosage en une seule clé `d` (`point.phDosing || point.orpDosing`) et `loadFromFile()` remet `dp.orpDosing = false`. Ce **n'est pas** un bug d'import : c'est le **format persisté** — l'information est irrécupérable quelle que soit la relecture. Toute analyse « couplage Javel → pH » depuis l'historique persisté est aujourd'hui impossible.
2. **Le fichier `.tmp` orphelin n'est jamais nettoyé au démarrage.** Combiné à un tampon de log plein, il réduit la marge de croissance de la partition `history` à **90 o** — c'est-à-dire à un seul point d'historique.
3. **`getPhFiltered()` n'a pas de garde d'âge.** Une mesure périmée serait historisée comme fraîche. **La revue conclut de ne rien changer** (c'est exactement ce que `/data` sert au même instant ; une divergence historique/interface serait pire) — la nuance est **documentée** dans [`docs/subsystems/history.md`](../../../docs/subsystems/history.md#fraîcheur-de-la-mesure-lissée--nuance-connue-aucun-changement) plutôt que corrigée.
