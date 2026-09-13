# Bug — Le recalcul du planning auto annule le cycle de filtration qui l'a produit

## Métadonnées
- **ID** : bug-126
- **Slug** : le-recalcul-du-planning-annule-le-cycle-qui-l-a-produit
- **Type** : bug de conception — la correction introduit un mécanisme, elle ne se limite pas à un correctif local
- **Statut** : done
- **Priorité** : **normale** — aucun risque sanitaire établi (2,9 % de la fenêtre de dosage), mais le cas se répète chaque fois que la température varie de plus de 1 °C, donc quasi quotidiennement en intersaison
- **Version cible** : à déterminer
- **Dernière mise à jour** : 2026-09-13
- **Spec(s) liée(s)** : `feature-091` (purge adaptative, v2.34.0), `feature-053` (Mode Boost), `feature-011` (horizon de répartition), `feature-056` (modes d'installation)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

⚠️ La filtration **conditionne le dosage** (garde « présence d'eau »). Tout changement
de sa logique de marche/arrêt touche la régulation, même sans modifier une ligne de
`pump_controller`.

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : la filtration conditionne le dosage
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le planning de filtration en mode auto est recalculé **pendant la marche**, cinq minutes
après le démarrage. Quand la température baisse, la fenêtre se rétracte autour du pivot
de 13:00 — le début recule. Si le nouveau début est postérieur à l'heure courante, le
cycle qui vient de démarrer **s'arrête lui-même**.

La mesure de température n'est fiable que parce que la pompe tourne, et elle sert à
arrêter la pompe. La boucle est circulaire.

## Contexte

Observé le 2026-09-13, en clair dans le journal de l'appareil :

```
2026-09-13T07:17:00  INFO  Démarrage filtration
2026-09-13T07:22:00  INFO  Planning auto: 21.6°C → 07:36-18:24
2026-09-13T07:22:00  INFO  Arrêt filtration
```

La veille : `Planning auto: 22.9°C → 07:17-18:44`. La fenêtre s'est rétractée de
39 minutes, réparties symétriquement — début +19 min, fin −20 min — et l'heure courante
(07:22) s'est retrouvée avant le nouveau début (07:36).

**Coût mesuré** : 14 minutes d'arrêt, un cycle pompe de 5 minutes, et la purge adaptative
qui repart de zéro (`kPurgeFloorMs` = 10 min) — soit environ **19 minutes avant la
première fenêtre de dosage possible**.

### Ce que `pool-chemistry` a établi (audit du 2026-09-13)

- **Aucun risque sanitaire.** 19 min sur ~11 h = 2,9 % de la fenêtre. Le chlore n'est pas
  perdu, il est **décalé** ; la boucle ORP rattrape dans la journée tant que la limite
  horaire n'est pas saturante. Stabilisant ~35 mg/L, UV faible à 07:20.
- **Le choix de la température de référence est indifférent au plan sanitaire.** L'écart
  22,9 → 21,6 °C vaut 5,7 % du turnover, alors que l'heuristique `durée = T/2` ignore la
  charge baigneurs, les UV, le TAC et le stabilisant. Ce qui compte n'est pas *laquelle*,
  mais qu'elle soit **stable et mesurée hors du cycle qu'elle pilote**.
- **Le coût réel est diagnostique** : ces arrêts polluent le journal et faussent l'analyse
  du droop. Plus une usure pompe réelle mais secondaire.

## Comportement actuel

`FiltrationManager::update()` (`src/filtration.cpp:237`) :

```cpp
if (runNow && mode == "auto" && elapsed >= 300000UL && !state.scheduleComputedThisCycle) {
  computeAutoSchedule();
  saveMqttConfig();
  state.scheduleComputedThisCycle = true;
  if (haveTime) {
    startMin = timeStringToMinutes(filtrationCfg.start);
    endMin = timeStringToMinutes(filtrationCfg.end);
    // Ne pas annuler un forceOn : si l'utilisateur a forcé le démarrage
    // (ex: avant la plage horaire calculée), le planning ne doit pas l'arrêter.
    if (!filtrationCfg.forceOn && !isMinutesInRange(nowMinutes, startMin, endMin)) {
      runTarget = false;
    }
  }
}
```

⚠️ Un démarrage **manuel** est protégé de l'annulation ; un démarrage **programmé** ne
l'est pas. Le raisonnement du commentaire — *le planning ne doit pas annuler un démarrage
déjà survenu* — vaut pourtant dans les deux cas.

`computeAutoSchedule()` (`src/filtration.cpp:82`) ne vérifie **aucune validité de
circulation** : seulement `isnan()`. Seul le délai de cinq minutes la protège d'une mesure
d'eau stagnante, et ce délai n'est écrit nulle part comme une garantie.

## Comportement attendu

**Une seule règle : le planning ne s'écrit qu'à l'arrêt, et ne s'applique qu'à minuit.**
Les deux moments où la pompe ne tourne pas.

1. **Pendant le cycle**, la température est moyennée — mais **seulement après la purge**
   (`kPurgeFloorMs` = 10 min), qui chasse précisément le bouchon d'eau stagnante réchauffée
   dans les tuyauteries.
2. **À l'arrêt du cycle**, cette moyenne produit une fenêtre symétrique autour du pivot,
   écrite dans un **planning en attente** — jamais dans le planning courant.
3. **À minuit**, le planning en attente devient le planning courant.
4. **Plus aucun recalcul pendant la marche.** Le bloc des lignes 237-251 disparaît.

### Cas limites

- **Mise en service en mode auto**, ou tout premier cycle sans moyenne de la veille :
  mesure **instantanée après purge**, produisant elle aussi un planning **en attente**.
  Le premier cycle tourne sur le planning stocké ; c'est un jour de transition, assumé.
- **Cycle écourté** (coupure, `forceOff`, redémarrage) : une moyenne non représentative ne
  doit pas piloter le lendemain. Une durée minimale de cycle reste à fixer — **c'est un
  arbitrage pour `architect` et `pool-chemistry`**.
- **Fenêtre à cheval sur minuit** : `isMinutesInRange` la gère (`start > end`), mais
  appliquer le planning à 00:00 suppose la pompe arrêtée. La règle doit le **dire** au
  lieu de le supposer.

  ⚠️ **Mesuré** : la fenêtre ne franchit minuit qu'à partir de **44 °C**, et à **48 °C**
  la durée sature à 24 h → `start == end` → `isMinutesInRange` rend **false** → **la
  filtration ne tourne plus jamais**, en silence. `computeAutoSchedule()` ne borne la
  température que par `isnan()`. Le cas n'est donc pas « rare » : il est **réservé à une
  sonde en défaut**, et la bascule à minuit rendrait ce mode dégradé **persistant** au
  lieu de transitoire. Une borne de plausibilité sur la température de référence est à
  trancher — dans cette spec ou dans une spec de suivi.
- **Élargissement de la fenêtre** (température en hausse) : aujourd'hui la fin est
  rallongée **silencieusement** sur le cycle en cours. Avec le report à minuit, le cycle
  garde sa fenêtre. Asymétrie à assumer explicitement.
- **`forceOn` / Mode Boost** : inchangés, prioritaires. Vérifier que l'expiration d'un
  `forceOn` (4 h) applique bien le planning courant et non un planning en attente.

## Contraintes techniques

⚠️ **Le démarrage est décidé sur le NIVEAU, pas sur un front.**
`decideFiltrationRun()` (`src/schedule_logic.cpp`) rend `isMinutesInRange(...)` ;
`currentlyRunning` ne sert que si l'heure est indisponible.

**Conséquence directe et non négociable** : écrire un planning dont la fin est postérieure
à l'heure d'arrêt fait **redémarrer la pompe au tour de boucle suivant**. C'est ce qui
interdit la variante « écrire à l'arrêt, appliquer au démarrage suivant » et impose le
planning en attente appliqué à minuit.

⚠️ **`remainingRangeMinutes()` lit le planning STOCKÉ** et rend **0** hors plage
(`src/schedule_logic.cpp:46`). Il pilote l'horizon du dosage programmé
(`src/pump_controller.cpp:3980` et `:4455`).

> **AMENDEMENT (clôture)** : le risque *(e)* est **fermé par construction**, pas par un
> alignement sur la fenêtre effective. `remainingRangeMinutes()` continue de lire le
> planning **stocké**, inchangé par ce lot — `pump_controller.cpp` n'est pas modifié.
> L'alignement sur la fenêtre effective envisagé ci-dessus a été **examiné et REFUSÉ** :
> `v = remaining / nWin` (`src/dosing_logic.cpp:498-500`) fait **croître** la dose par
> fenêtre quand l'horizon **décroît** — raccourcir l'horizon pour suivre une fin de cycle
> qui recule aurait concentré le reliquat de dosage au lieu de le lisser. Chiffrage
> `pool-chemistry` : sur un Boost à 21:00 avec 600 mL de reliquat, **+1,7 mg/L de chlore
> libre** injectés de nuit pour une cible de 2,5-3. Voir [ADR-0079](../../../docs/adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)
> pour les trois garde-fous qui seraient exigibles avant de rouvrir cette option.

⚠️ **`_lastScheduledTemp` n'est pas persisté** (`src/filtration.h:20`). La bande morte de
1 °C est donc **inopérante au premier cycle après chaque redémarrage**. Avec une seule
écriture par jour, cette bande morte n'a plus d'objet et peut disparaître — ce qui ferme
le risque *(f)* plutôt que de le contourner.

- Aucune allocation dynamique récurrente : la moyenne se tient en somme + compteur.
- `esp_task_wdt_reset()` inutile ici (aucune boucle longue ajoutée).
- Persistance : **une seule écriture NVS par jour**, à l'arrêt du cycle.

## Fichiers potentiellement concernés

- `src/filtration.cpp` — suppression du recalcul en marche, accumulation de la moyenne,
  écriture à l'arrêt, application à minuit
- `src/filtration.h` — état du planning en attente et de l'accumulateur
- `src/schedule_logic.{h,cpp}` — logique pure : moyenne, bascule à minuit, fenêtre effective
- ~~`src/pump_controller.cpp` — alignement de l'horizon de dosage sur la fenêtre effective~~
  **RETIRÉ (clôture)** : le repli d'horizon est refusé (voir amendement ci-dessus) ;
  `pump_controller.cpp` n'est **pas** dans le lot livré, C1 de `pool-chemistry`.
- `src/config.{h,cpp}` — persistance du planning en attente — **SANS OBJET (clôture)** :
  le planning en attente est **100 % RAM**, jamais persisté (arbitrage RAM vs NVS,
  [ADR-0079](../../../docs/adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)).
  `src/config.{h,cpp}` n'est pas modifié.
- `src/constants.h` — durée minimale de cycle représentatif
- `test/test_native_schedule/test_schedule_logic.cpp` — logique pure. ⚠️ Ce chemin a
  d'abord été écrit `test_native_schedule_logic/`, qui **n'existe pas** ; la suite réelle
  couvre déjà `remainingRangeMinutes`. `schedule_logic.cpp` est déjà dans
  `build_src_filter` de l'env natif (`platformio.ini:103`) : toute fonction pure neuve y
  est testable sans configuration.

## Livrables — check-list

- [x] A1 — accumulation de la température moyenne du cycle, **hors purge ET en plage**,
      sonde eau non figée (`isSampleEligible` : les trois conditions sont cumulatives)
- [x] A2 — écriture du planning **en attente** à l'arrêt du cycle
- [x] A3 — bascule du planning en attente vers le planning courant **à minuit**
- [x] A4 — suppression du recalcul pendant la marche (`filtration.cpp:237-251`)
- [x] A5 — mesure instantanée après purge au **premier cycle sans moyenne**
- [x] A6 — **SANS OBJET** (clôture) : remplacé par une sentinelle de non-régression —
      `pump_controller.cpp` n'est pas modifié, `remainingRangeMinutes()` continue de lire
      le planning stocké. Voir l'amendement du refus d'horizon, plus haut.
- [x] A7 — retrait de la bande morte `_lastScheduledTemp` ; remplaçant : gel de sonde
      (`isSampleEligible` refuse tout échantillon si `sensors.isTemperatureFrozen()`) +
      fenêtre calculée identique à la fenêtre courante ⇒ aucun pending écrit
- [x] A8 — réparation d'une plage `start == end` héritée en mode auto (repli
      `08:00`-`20:00` + alerte `filtration_schedule_invalid`), auparavant un mode dégradé
      **persistant et silencieux**
- [x] A9 — source de température strictement « sonde eau » (`sensors.getWaterTemperature()`
      en chemin nominal, repli de tolérance `selectFiltrationRefTemp()` uniquement en
      l'absence de tout rôle attribué) — jamais `sensors.getTemperature()`
- [x] D1 — tests natifs : rétraction, élargissement, minuit, cycle écourté, premier cycle,
      fenêtre à cheval sur minuit, `forceOn` expirant en marche (`pio test -e native`
      vert — voir rapport `test-engineer`)
- [x] D2 — **validation par mutation** de chaque garde : **26 mutants nommés tués,
      aucun équivalent**, sur deux passes `test-engineer` (M1-M16 sur le lot initial,
      M17-M26 sur les 8 correctifs de revue). Chaque mutation a été appliquée, la suite
      lancée, le test mort relevé, puis la mutation restaurée. **Deux décors ont été
      démasqués dans les tests eux-mêmes** et corrigés : (a) une séquence de 60
      échantillons ne distingue pas la moyenne incrémentale d'une somme naïve (écart
      ~1e-6, sous l'epsilon) — le test conservé est déclaré non-tueur ; (b) le test de
      budget d'octets cherchait le message d'alerte dans le fichier entier, or il a
      **deux sites d'émission** : allonger un seul site laissait le test vert, l'autre
      satisfaisant la recherche à sa place. Corrigé en extrayant chaque corps de
      fonction séparément.
- [x] C1 — `docs/subsystems/filtration.md` et `CHANGELOG.md`

## Sécurité chimique

- **Dosage impliqué** : indirectement — la filtration conditionne la garde « présence d'eau »
- **Limites journalières / horaires concernées** : aucune modifiée. ⚠️ Vérifier la limite
  horaire réelle via `/get-config`, **jamais** les défauts d'usine (piège documenté)
- **Filtration requise avant action** : c'est l'objet même de la spec
- **Calibration capteur modifiée** : non
- **Algorithme PID touché** : non
- **Risque identifié** : remplacer l'arrêt de filtration par un **gel du dosage** si
  l'horizon `remainingRangeMinutes()` n'est pas aligné sur la fenêtre effective (livrable A6)

## ADR attendu ?

- **Probable** : **oui**
- **Critère(s) candidat(s)** : 1 (contrainte structurante) et 3 (alternatives crédibles)

Quatre options ont été examinées et écartées, et un ADR doit dire **pourquoi** :
ne pas annuler un cycle démarré (sur-filtre) · séparer planning stocké et fin effective
(gel du dosage) · recalculer à l'arrêt en appliquant aussitôt (**relance immédiate**, le
démarrage étant décidé sur le niveau) · moyenne 24 h (**polluée** : la sonde est sur le
circuit de filtration, l'eau stagnante y est plus chaude, et `history.cpp:463` enregistre
sans garde de circulation).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
Aucun nouveau. **Arbitrage tranché à la clôture** : le planning **en attente** n'est **pas
exposé** dans `/get-config` dans cette itération (arbitrage C du plan retenu — `ux-ui-designer`
et `web-ui-developer` n'étaient pas dans ce cycle). Reporté à une spec de suivi, priorité
haute (créée dans `specs/features/todo/`, voir notes techniques).

### Champs WebSocket
Aucun nouveau a priori. ⚠️ Vérifier la **parité `/data` ↔ WebSocket** si un champ est
ajouté — trois récidives documentées sur ce point.

### Topics MQTT
Aucun nouveau.

### Documentation à produire / mettre à jour
- `docs/subsystems/filtration.md` — la règle « écrit à l'arrêt, appliqué à minuit »
- `CHANGELOG.md` — comportement utilisateur modifié
- `docs/adr/NNNN-*.md` — les quatre options écartées
- `docs/user/src/guide-regulation.html` si le comportement visible change → puis
  `./tools/build_user_pdfs.sh`

## Critères d'acceptation

- [x] Une baisse de température **n'arrête jamais** un cycle en cours
- [x] Une hausse de température **ne prolonge jamais** un cycle en cours
- [x] Écrire un planning dont la fin est postérieure à l'heure d'arrêt **ne relance pas** la pompe
- [x] La moyenne **exclut** les `kPurgeFloorMs` premières minutes — précision (clôture) :
      elle exige en plus **30 échantillons post-purge ET en plage** horaire
      (`kFiltrationMinRepresentativeSamples`), à l'exception du premier cycle sans moyenne
      (mise en service), qui accepte une mesure instantanée (amorçage)
- [x] L'horizon de dosage n'est **jamais** nul pendant un **cycle programmé** — reformulé
      (clôture) : la formulation initiale (« pendant que la pompe tourne ») ne distinguait
      pas un cycle programmé d'un forçage manuel/Boost, tous deux non concernés par le
      risque *(e)*, désormais fermé par construction (voir amendement plus haut)
- [x] Une seule écriture NVS par jour **sur le chemin automatique** — reformulé (clôture) :
      une écriture manuelle (HTTP/MQTT) reste possible à tout moment et n'est pas comptée
      ici ; elle **annule** le planning en attente plutôt que l'inverse (`BasisChanged`)
- [x] Un cycle écourté ne pilote pas le lendemain — la promotion d'un planning en attente
      alors que la pompe tourne encore est **différée**, jamais sautée ni forcée
      (`PromotionVerdict::PumpRunning`) : le planning attend le prochain tour à l'arrêt
- [x] `forceOn` et Mode Boost restent prioritaires et inchangés

## Plan de test manuel

1. Forcer une baisse de température de plus de 1 °C entre deux cycles ; vérifier que le
   cycle du lendemain démarre à l'heure du planning appliqué à minuit, sans interruption.
2. Forcer une hausse ; vérifier que le cycle en cours n'est pas prolongé.
3. Redémarrer l'appareil pendant un cycle ; vérifier qu'aucun recalcul ne survient.
4. Observer le journal : plus aucune paire `Démarrage filtration` / `Arrêt filtration`
   séparée de moins de dix minutes.

## Notes pour agent

Le journal de l'appareil est la **preuve** de ce bug — il le montre en trois lignes. Toute
correction doit se vérifier de la même façon : par le journal, pas par le raisonnement.

⚠️ **Ne pas toucher aux seuils de dosage, ni aux limites journalières ou horaires.** Cette
spec ne modifie que le moment où le planning est calculé et appliqué.

⚠️ L'affirmation « le motif du recalcul à +5 min est obsolète depuis `feature-091` » a été
avancée puis **nuancée** : `feature-091` porte sur le timer de stabilisation du **dosage**,
pas sur la validité de la mesure de température. L'argument qui tient est autre — on n'a
pas besoin de mesurer *pendant* ce cycle pour planifier *ce* cycle.

## Hors périmètre

- Le choix du pivot (`kFiltrationPivotHour` = 13:00) et la formule `durée = T/2`
- Le bornage de la concurrence sur les fichiers statiques (S2, `bug-080`)
- **Toute modification de `pump_controller`** — amendé (clôture) : A6 est retiré (voir
  amendement du refus d'horizon) ; **aucune** ligne de `pump_controller.cpp` n'est
  modifiée par ce lot, sans exception.

## Notes techniques — fin de cycle (2026-09-13)

- **Statut** : implémentation livrée, revue deux fois (`pool-chemistry` P2, `code-reviewer`),
  corrigée, `pio run` SUCCESS, `pio test -e native` vert — mesuré par `doc-writer` au
  moment de cette clôture documentaire (ADR-0073 : ne pas recopier un compteur) :
  `ls -d test/test_native_* | wc -l` = **44 suites**, `pio test -e native` =
  **1734 cas, tous PASSED**. Ce chiffre diffère du 1722/1722 annoncé en tête de cycle — la
  seconde passe `test-engineer` en parallèle (ajout de tests sans toucher à `src/`) a
  vraisemblablement progressé entre-temps ; c'est cohérent avec le principe de mesure et
  non un signal d'alarme. Spec **laissée en `doing/`** : ni les commits ni la validation
  D2 (mutation) ne sont du ressort de `doc-writer`.
- **Fichiers modifiés** : `src/constants.h`, `src/filtration.{h,cpp}`, `src/schedule_logic.{h,cpp}`,
  `test/test_native_schedule/test_schedule_logic.cpp`,
  `test/test_native_temp_compensation/test_temp_compensation.cpp`.
- **`computeAutoSchedule()` a DEUX appelants externes, pas trois** — correction d'une
  affirmation véhiculée en cours de cycle. Vérifié par grep : `web_routes_config.cpp:1108`
  et `mqtt_manager.cpp:1473` uniquement (confirmé aussi par le commentaire du test natif,
  `test_schedule_logic.cpp:1318` : « DEUX sites d'émission INDÉPENDANTS »). Les deux
  correspondent à un changement de mode de filtration vers `auto` (HTTP, MQTT) — c'est la
  voie « mise en service », distincte du chemin d'accumulation en marche.
- **`sensors.getTemperature()` a un repli non offseté vers N'IMPORTE QUELLE sonde** —
  c'est le motif exact de l'interdiction de cette fonction dans `filtration.cpp`
  (arbitrage B) : elle appelle d'abord `getWaterTemperature()` (offset appliqué, rôle
  `Water` uniquement) puis, en repli, renvoie la **première sonde détectée** sans
  distinction de rôle ni offset (`src/sensors.cpp:1424-1432`) — potentiellement une sonde
  `Circuit` (température du boîtier électronique). Le planning utilise désormais
  exclusivement `getWaterTemperature()` + un repli dédié (`selectFiltrationRefTemp()`) qui,
  lui, refuse tout repli dès qu'une sonde `Circuit` est déclarée.
- **Défaut B1 découvert en revue** : le drapeau d'amorçage (`_haveRepresentativeMeasure`)
  devait être armé **avant** le test de fenêtre identique dans `writePendingSchedule()`,
  pas après. En régime établi (même fenêtre chaque jour — le chemin normal, qui retourne
  tôt sur « fenêtre inchangée »), un armement tardif aurait laissé le drapeau `false`
  indéfiniment ; le premier cycle produisant ensuite une fenêtre différente aurait pu le
  faire sur un seul échantillon (cycle tronqué par un `forceOff`), faisant tomber le
  critère d'acceptation « un cycle écourté ne pilote pas le lendemain ». Corrigé avant
  la clôture.
- **Le piège `start == end` était préexistant, pas introduit par ce lot** — la spec
  d'origine (section « Cas limites ») l'avait déjà identifié : à 48 °C, `computeAutoWindow`
  sature la durée à 24 h, produisant `start == end`, et `isMinutesInRange` rend `false` en
  permanence ⇒ filtration arrêtée pour toujours en silence. Ce lot le ferme par deux voies
  indépendantes : les bornes de plausibilité `[4, 40] °C` (48 °C devient inatteignable par
  la voie automatique) et la réparation explicite d'`ensureTimesValid()` (A8) pour toute
  plage nulle héritée par une autre voie (NVS corrompue, saisie manuelle incohérente).
