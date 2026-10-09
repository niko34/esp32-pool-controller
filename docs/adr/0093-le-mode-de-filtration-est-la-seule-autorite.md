# ADR-0093 — Le mode de filtration est la seule autorité

- **Statut** : Accepté
- **Date** : 2026-10-09
- **Décideurs** : Nicolas Philippe (architect), pool-chemistry (GO sous conditions, deux passages — conception initiale puis amendements du Boost)
- **Spec(s) liée(s)** : feature-146 (quatre-modes-de-filtration-qui-disent-ce-quils-font) · feature-053 (Mode Boost, remplace [ADR-0025](0025-mode-boost.md)) · bug-126

## Contexte

Deux systèmes concurrents décidaient si la pompe de filtration tourne : le **mode affiché**
(`auto`/`manual`/`force`/`off`) et un **« ordre direct »** (`forceOn`/`forceOff`) évalué
**avant** lui dans `decideFiltrationRun()`, expirant au bout de 4 h (`kForceTimeoutMs`).

Le 2026-10-09, en production, l'utilisateur a remis trois fois le mode sur `auto` **dans le
créneau horaire programmé**, sans que la pompe démarre. Journal relevé :

```
10:21:22  Filtration forcée OFF (MQTT)   → forceOff = true (tient 4 h, jusqu'à 14:21)
10:23:38  Mode filtration changé: auto   → RIEN ne démarre
10:23:55  Mode filtration changé: off
10:23:58  Mode filtration changé: auto   → RIEN ne démarre
10:24:18  (bouton « Démarrer » de l'UI web) → force_on=true, force_off=false → démarre
```

Cause racine : `decideFiltrationRun()` évaluait `forceOff` **avant** le mode — tant qu'il
restait vrai, aucun mode ne pouvait rien démarrer. Et la commande MQTT `FiltrationMode` ne
touchait jamais `forceOn`/`forceOff`, alors que le chemin HTTP `/save-config`, lui, les
effaçait : **le même geste utilisateur produisait deux résultats différents** selon la voie
employée (web vs Home Assistant/MQTT).

En creusant, le système de **modes** était déjà cassé indépendamment de l'ordre direct :
`mqtt_manager.h` déclarait quatre jetons (`auto|manual|force|off`), mais `"force"` n'était
traité **nulle part** dans `decideFiltrationRun()` — il tombait dans le `return false` par
défaut. Un mode nommé « Manuel » dans l'UI (jeton `force`) **arrêtait** la pompe au lieu de la
faire tourner. L'ordre direct avait été greffé pour compenser cette absence d'un vrai pilotage
manuel par le mode, sans jamais corriger le défaut sous-jacent.

Le Mode Boost (feature-053, [ADR-0025](0025-mode-boost.md)) ajoutait une troisième source de
vérité : un chemin de forçage dédié (`boostForce`), prioritaire sur tout le reste y compris
l'ordre direct.

## Décision

**Le mode affiché est la seule autorité qui décide si la pompe de filtration tourne. Aucun état
caché ne peut le contredire.** Concrètement :

### 1. Quatre modes, un seul mécanisme de décision

| Mode | Jeton protocole (inchangé) | Comportement |
|---|---|---|
| **Auto** | `auto` | créneau calculé depuis la température de l'eau, ajusté quand elle change |
| **Programme** | `manual` | créneau fixé par l'utilisateur, jamais recalculé |
| **Marche** | `force` | la pompe tourne — sans expiration |
| **Arrêt** | `off` | la pompe est arrêtée — sans expiration |

`decideFiltrationRun(FiltrationMode mode, bool haveTime, int nowMin, int startMin, int endMin,
bool currentlyRunning)` traite les quatre modes dans un `switch` **exhaustif, sans `default`**
(voir point 4). L'« ordre direct » (`forceOn`/`forceOff`, son minuteur de 4 h, les champs
`FiltrationConfig::forceOn/forceOff`, `FiltrationRuntime::forceOnStartMs/forceOffStartMs`) est
**supprimé**, pas neutralisé.

### 2. Jetons de protocole conservés — `parseFiltrationMode()` unique site de correspondance

Les jetons `auto|manual|force|off` restent **inchangés sur le fil** (NVS, MQTT, HTTP). Toute la
correspondance jeton → sens vit dans une seule fonction, `parseFiltrationMode()`
([`src/schedule_logic.h`](../../src/schedule_logic.h)/`.cpp`) : c'est le **seul** endroit du
code métier où une chaîne littérale de mode est comparée.

### 3. Le Boost modifie le créneau, il ne décide plus

`boostForce` **disparaît** de `decideFiltrationRun()` — ni paramètre, ni branche. Une fonction
pure distincte, `RunWindow effectiveRunWindow(ScheduleWindow base, bool boostActive, int
nowMin)`, dérive un créneau **effectif** (`{nowMin, 1440}` si le Boost est actif, le créneau de
base sinon), **en RAM uniquement**, jamais stocké. Le Boost est disponible dans tous les modes
**sauf Arrêt** (voir « Trois décisions amendées en cours de route » ci-dessous) : il surchlore
une eau qui doit circuler, et `Arrêt` ne consulte de toute façon pas le créneau.

### 4. Exhaustivité garantie par le compilateur

`build_src_flags = -Wswitch -Werror=switch` (env `esp32dev_core3` et `native`,
`platformio.ini`) : un mode non traité dans un `switch` sur `FiltrationMode` **ne compile pas**.
Avant ce lot, cette garantie était **annoncée mais inexistante** — c'est précisément ce qui a
permis à `"force"` de tomber en silence dans un `return` par défaut pendant des mois. Vérifié
empiriquement le 2026-10-09 (retirer une branche compilait sans un mot), puis fermé.

## Alternatives considérées

- **Renommer les jetons protocole** (`"programme"`, `"marche"`, `"arret"`) pour qu'ils
  correspondent enfin aux libellés affichés — **rejetée**. Coût chiffrable et non négligeable :
  migration NVS, rupture de toute automatisation Home Assistant qui publie le jeton brut sur
  `{base}/filtration_mode/set`, et surtout `PendingSchedule::basisMode` ([`src/filtration.h`](../../src/filtration.h))
  est un `char[8]` — `"programme"` (9 caractères) y serait **tronqué en silence**, un bug
  distinct introduit par une correction cosmétique. La contrepartie exigée en échange de la
  conservation des jetons : `parseFiltrationMode()` devient l'unique site de correspondance,
  pour qu'un futur renommage (si jamais décidé) n'ait qu'un seul endroit à changer.
- **Garder l'ordre direct mais corriger l'asymétrie MQTT/HTTP** (faire en sorte que la commande
  MQTT `FiltrationMode` efface aussi `forceOn`/`forceOff`) — **rejetée**. Corrige le symptôme du
  2026-10-09 sans retirer la cause : deux sources de vérité resteraient en présence, l'une des
  deux pouvant à nouveau diverger au prochain chemin d'écriture non aligné. Demande explicite de
  l'utilisateur de **retirer**, pas de rattraper : « *tu as mis en œuvre des solutions
  complexes… propose une solution pour revenir à un fonctionnement simple* ».
- **Le Boost reste une priorité absolue, indépendante du mode** (statu quo ADR-0025) —
  **rejetée**. C'était la cause d'un défaut de sécurité **préexistant et bloquant**, relevé par
  `pool-chemistry` : `relayShouldBeOn = state.running && (filtrationCfg.mode != "off")`
  (`filtration.cpp:374`, avant ce lot) laissait `state.running = true` avec Boost actif + mode
  `off`, mais le relais réel restait **ouvert** — alors que `resolveWaterPresence()` lit
  `state.running`, pas l'état du relais. L'appareil annonçait donc une présence d'eau fausse et
  autorisait l'injection dans une conduite immobile. Deux conceptions intermédiaires ont été
  examinées et rejetées à leur tour avant la décision retenue :
  - *Le Boost consulté seulement en branche Auto* (première proposition utilisateur) —
    remplacée par une version qui va plus loin et ferme une branche de plus.
  - *Le Boost réécrit `filtrationCfg.start/end`* — rejetée : `PendingSchedule::basisStart/basisEnd`
    ([`src/filtration.h`](../../src/filtration.h)) est un instantané de ces mêmes champs, utilisé
    pour détecter qu'un changement **utilisateur** a eu lieu. Si le Boost les réécrivait, chaque
    planning auto en attente serait jeté silencieusement (`BasisChanged`,
    [ADR-0079](0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) — l'apprentissage
    automatique de la durée de filtration mourrait sans un mot. D'où la fonction pure
    `effectiveRunWindow()`, qui **dérive** un créneau en RAM sans jamais écrire la config.
- **Un garde-fou qui empêcherait le Boost en Arrêt pour protéger le dosage** (conception
  intermédiaire) — devenue une **règle de cohérence**, pas un garde-fou, une fois la source
  unique du relais posée (`relayShouldBeOn = state.running`, sans second terme) : le trou de
  sécurité est fermé par construction, la restriction Arrêt ne fait plus que refléter qu'il n'y
  a rien à surchlorer sans circulation.

## Conséquences

### Positives

- Changer de mode **s'applique immédiatement** par les trois voies (web, MQTT, Home Assistant),
  prouvé par test — fin de l'asymétrie du 2026-10-09.
- **Code net plus simple** (demande explicite de l'utilisateur, mesurée en C1 de la spec) :
  −2 paramètres de `decideFiltrationRun()`, −2 branches de priorité, −4 champs d'état
  (`forceOn`, `forceOff`, `forceOnStartMs`, `forceOffStartMs`), −2 clés WebSocket
  (`filtration_force_on`, `filtration_force_off`), −1 minuteur (`kForceTimeoutMs`), −1 mécanisme
  entier.
- Le défaut de sécurité préexistant (relais/décision divergents en Boost+Arrêt) est fermé **deux
  fois** : le Boost ne peut plus contourner la décision, et le relais a désormais une source
  unique (`state.running`).
- Marche et Arrêt durent jusqu'au prochain changement de mode — comportement explicite,
  lisible dans l'UI, au lieu d'un minuteur invisible de 4 h.

### Négatives / dette assumée

- **Marche et Arrêt survivent à un redémarrage** (ils n'expirent plus) : un hivernage ou une
  maintenance oubliés en mode Arrêt restent arrêtés indéfiniment ; un mode Marche oublié
  consomme de l'électricité sans limite. Assumé : c'est la contrepartie directe de « plus de
  minuteur caché », demandée explicitement par l'utilisateur.
- **Migration de configuration** : un appareil qui redémarre avec `mode = "force"` passait
  auparavant par le `return false` par défaut (pompe arrêtée, un bug) ; il bascule désormais en
  Marche (pompe tournante) — changement de comportement réel au boot, signalé par `warning` +
  alerte MQTT `filtration_mode_force_migrated`.
- **Boost + Marche** est une combinaison neuve, jamais atteignable avant ce lot (le mode Marche
  étant lui-même inatteignable). Validée par `pool-chemistry` : aucun plafond n'est desserré, le
  volume de chlore reste borné à `kBoostDailyHardCapMl = 1000 mL` quelle que soit la durée
  (plafond journalier en dur, pas le plafond horaire qui devient non contraignant).
- Rupture d'automatisation Home Assistant : les **libellés** du `select` « Mode Filtration »
  changent (`Programmation`→`Programme`, `Manuel`→`Marche`, `Désactivé`→`Arrêt`) — une
  automatisation qui appelle `select.select_option` avec un ancien libellé français cesse de
  fonctionner. Celles qui publient le jeton brut sur MQTT survivent (protocole inchangé).

### Ce que ça verrouille

- **Aucun futur mécanisme d'override parallèle au mode n'est acceptable** sans passer par un
  nouvel ADR qui justifie explicitement pourquoi le mode seul ne suffit pas. Le défaut du
  2026-10-09 est la preuve qu'un tel mécanisme, même introduit pour de bonnes raisons
  (compenser un mode cassé), finit par diverger silencieusement d'une voie d'écriture à l'autre.
- `parseFiltrationMode()` reste l'**unique** site de correspondance jeton → sens. Tout code qui
  comparerait un littéral de mode ailleurs (`== "auto"`, `.equalsIgnoreCase("auto")`, etc.) est
  une régression de cette décision.
- Le Boost ne stocke et ne doit **jamais stocker** de créneau élargi : la dérivation en RAM
  (`effectiveRunWindow`) est la seule forme acceptée pour un modificateur temporaire du créneau.
  Toute évolution future du Boost (ou d'un mécanisme similaire) qui voudrait écrire
  `filtrationCfg.start/end` doit d'abord démontrer comment elle préserve `PendingSchedule` sans
  le casser.
- **ADR-0079 reste intact** : la condition C2 (`isSampleEligible`, échantillonnage de
  température pendant un cycle) continue de juger le créneau de **base**, jamais le créneau
  effectif élargi par le Boost. Un Boost nocturne continue de faire entrer des échantillons
  froids dans la moyenne s'il tombe dans le créneau de base, mais ne peut jamais en faire entrer
  via l'élargissement — sans quoi le créneau calculé le lendemain se punirait lui-même avec un
  jour de retard.
- `relayShouldBeOn = state.running`, **source unique**, invariant `state.running == relayState`
  en mode `ManagedFiltration`. Toute réintroduction d'un second terme dans ce calcul
  (`&& filtrationCfg.mode != "off"` ou équivalent) réouvrirait la classe de défaut fermée ici.

## ADR amendé

**[ADR-0025](0025-mode-boost.md) (Mode Boost) est REMPLACÉ, pas amendé**, et son statut passe à
`Superseded by ADR-0093` sans toucher à son corps. Son indépendance vis-à-vis du mode de
filtration était sa propriété définissante (« priorité MAXIMALE », « chemin dédié
`boostForce`, indépendant du `forceOn` ») — cette propriété disparaît : le Boost perd son
chemin de décision dédié et devient un modificateur de créneau consommé par
`decideFiltrationRun()` au même titre que n'importe quel créneau de base. Les bornes de
sécurité chimique posées par l'ADR-0025 (`kBoostOrpDeltaMv`, `kBoostOrpCeilingMv`,
`kBoostDailyFactor`, `kBoostDailyHardCapMl`) et le gate au mode ORP `automatic` restent
**inchangés** — seul l'effet filtration (point 1 de l'ADR-0025) change de mécanisme.

## Références

- Code : [`src/schedule_logic.h`](../../src/schedule_logic.h) (`FiltrationMode`,
  `parseFiltrationMode`, `RunWindow`, `effectiveRunWindow`),
  [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) (`decideFiltrationRun`),
  [`src/filtration.cpp`](../../src/filtration.cpp) (`begin()`, `update()` — migration `force`,
  annulation du Boost en Arrêt, source unique du relais),
  [`src/config.cpp`](../../src/config.cpp) (`startBoost()` — garde Arrêt),
  [`src/config.h`](../../src/config.h) (`FiltrationConfig`, `forceOn`/`forceOff` retirés),
  [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (`FiltrationMode`/`FiltrationOnOff`,
  libellés HA), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (validation
  serveur du mode), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
  (`filtrationDosingHorizonMinutes`), `platformio.ini` (`-Werror=switch`)
- Spec : `specs/features/done/feature-146-quatre-modes-de-filtration-qui-disent-ce-quils-font.md`
- Tests : [`test/test_native_schedule/test_schedule_logic.cpp`](../../test/test_native_schedule/test_schedule_logic.cpp)
  (matrice 4 modes × dans/hors créneau × heure connue/inconnue × état courant,
  `effectiveRunWindow`, `parseFiltrationMode`)
- Doc : [`docs/subsystems/filtration.md`](../subsystems/filtration.md),
  [`docs/features/page-filtration.md`](../features/page-filtration.md), [`docs/API.md`](../API.md),
  [`docs/MQTT.md`](../MQTT.md), [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md),
  [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md), [`CHANGELOG.md`](../../CHANGELOG.md)
- ADR liés : [ADR-0025](0025-mode-boost.md) (Mode Boost — **remplacé** par celui-ci),
  [ADR-0079](0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md) (planning auto figé à
  l'arrêt, promu à minuit — **préservé**, la condition C2 reste sur le créneau de base),
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique métier pure,
  Humble Object — patron réutilisé pour `parseFiltrationMode`/`effectiveRunWindow`),
  [ADR-0026](0026-mode-installation.md) (mode d'installation, non modifié par ce lot)
