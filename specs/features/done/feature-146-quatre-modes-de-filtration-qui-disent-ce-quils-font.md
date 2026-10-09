# feature-146 — Quatre modes de filtration qui disent ce qu'ils font

## Métadonnées
- **ID** : feature-146
- **Slug** : quatre-modes-de-filtration-qui-disent-ce-quils-font
- **Type** : refactor (simplification — RETRAIT d'une couche)
- **Statut** : livré — tests verts, revue OK (v2.63.0, 2026-10-09)
- **Priorité** : **haute** — défaut rencontré en production le 2026-10-09, et l'utilisateur
  a explicitement demandé un retour à un fonctionnement simple.
- **Version cible** : 2.63.0
- **Dernière mise à jour** : 2026-10-09
- **Spec(s) liée(s)** : feature-053 (Mode Boost, à préserver) · bug-126 (`computeAutoSchedule`)
  · ADR-0085 (empreinte d'auto-discovery HA)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → **passage obligatoire par `pool-chemistry`**
      — indirect mais réel : la présence d'eau garde le dosage, et elle dépend de la
      filtration. Toute modification du chemin qui décide « la pompe tourne » doit être
      validée.
- [x] **Filtration / Éclairage**
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — garde de présence d'eau du dosage
- [ ] ux-ui-designer — *conditionnel* : seulement si le choix des 4 modes impose une
      nouvelle mise en page. Un simple renommage de libellés ne le justifie pas.
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le mode de filtration devient **la seule chose qui décide** si la pompe tourne. Quatre modes,
chacun faisant exactement ce que son nom annonce :

| Mode | Comportement |
|---|---|
| **Auto** | L'appareil calcule le créneau à partir de la température de l'eau et l'ajuste quand elle change. |
| **Programme** | L'utilisateur fixe le créneau ; l'appareil s'y tient et ne le recalcule **jamais**. |
| **Marche** | La pompe tourne. |
| **Arrêt** | La pompe est arrêtée. |

**Marche** et **Arrêt** durent tant que l'utilisateur ne change pas de mode. Aucun minuteur,
aucune expiration, aucun retour automatique.

**L'« ordre direct » (`forceOn` / `forceOff`) est SUPPRIMÉ**, avec son minuteur de 4 h.

## Contexte — ce qui a été constaté en production le 2026-10-09

L'utilisateur nettoyait sa piscine. Séquence relevée au journal de l'appareil :

```
10:21:22  Filtration forcée OFF (MQTT)   → forceOff = true (tient 4 h, soit jusqu'à 14:21)
10:23:38  Mode filtration changé: auto   → RIEN ne démarre
10:23:55  Mode filtration changé: off
10:23:58  Mode filtration changé: auto   → RIEN ne démarre
10:24:18  (bouton « Démarrer » de l'UI web) → force_on=true, force_off=false → démarre
```

Il était 10 h 23, **dans le créneau** 08:04–17:55, en mode `auto`. La pompe ne démarrait pas.

**Cause racine établie** (`src/schedule_logic.cpp:90-111`) : `decideFiltrationRun()` évalue
`forceOff` **AVANT** le mode. Tant que `forceOff` est vrai, `auto` ne peut rien démarrer.
Et `src/mqtt_manager.cpp:1652-1672` (commande `FiltrationMode`) **ne touche jamais**
`forceOn` / `forceOff` — alors que le chemin HTTP `src/web_routes_config.cpp:969-978` les
efface, lui. Le même geste donne donc deux résultats différents selon qu'on passe par Home
Assistant ou par la page web.

Sans l'appui sur « Démarrer », la filtration serait restée arrêtée jusqu'à 14:21 pendant que
l'interface affichait « mode : auto ».

### Pourquoi le code en était arrivé là

Le système de modes était **déjà cassé**, et l'ordre direct a été greffé pour compenser :

- `src/mqtt_manager.h:415` déclare quatre modes : `"auto"|"manual"|"force"|"off"` ;
- dans `decideFiltrationRun()`, **`"force"` n'est pas traité** : il tombe dans le `return false`
  final. Un mode nommé « Marche » qui **arrête** la pompe ;
- `"manual"` et `"auto"` y sont traités **à l'identique** (tous deux appliquent le créneau).
  Leur seule différence vit ailleurs : `FiltrationManager::computeAutoSchedule()`
  (`src/filtration.cpp:131`) sort immédiatement si le mode n'est pas `"auto"` — donc en
  `"manual"` le créneau saisi n'est jamais recalculé.

⚠️ **Ce point a d'abord été diagnostiqué à tort** comme « le mode Manuel ne sert à rien » :
l'erreur venait d'avoir lu `decideFiltrationRun()` seul. `"manual"` **est déjà** le mode
« Programme » demandé — il est simplement mal nommé, invisible, et voisin d'un mode cassé.

Il n'existait donc **aucun moyen de piloter la pompe à la main par le mode**. D'où l'ordre
direct, avec son minuteur de 4 h et son invisibilité totale.

## Comportement attendu

1. **Changer de mode s'applique IMMÉDIATEMENT.** Passer en Auto pendant le créneau démarre la
   pompe sans délai perceptible ; passer en Arrêt l'arrête.
2. **Le mode affiché est toujours ce qui décide.** Aucun état caché ne peut le contredire.
3. **Un même changement de mode donne le même résultat** qu'il vienne de la page web, de Home
   Assistant ou de MQTT.
4. **Marche et Arrêt n'expirent pas.** Ils durent jusqu'au prochain changement de mode.
5. **Auto** recalcule le créneau depuis la température de l'eau (comportement actuel, inchangé).
6. **Programme** applique le créneau saisi et ne le recalcule jamais (= l'actuel `"manual"`).
7. Les boutons **Démarrer** / **Arrêter** de l'interface **changent le mode** (→ Marche / → Arrêt).
   Ils ne posent plus d'état séparé.

### Cas limites
- **Heure indisponible** (perte RTC, OTA) en Auto ou Programme : conserver l'état courant,
  comme aujourd'hui (`currentlyRunning`) — condition `pool-chemistry` déjà en place, à ne pas
  casser.
- **Mode inconnu** reçu par MQTT ou en configuration : fail-safe explicite, et journalisé.
  Décider lequel (vraisemblablement : conserver le mode courant et refuser le changement).
- **Migration de configuration** : un appareil qui démarre avec `mode = "manual"` doit se
  retrouver en **Programme** ; avec `mode = "force"`, en **Marche** (et non arrêté, comme
  aujourd'hui). Un `forceOn` / `forceOff` persisté en NVS doit être **ignoré et effacé**.
- **Mode Boost** (feature-053) : garde sa priorité maximale, intact. Ce n'est pas un état
  caché — il est déclenché sciemment et dispose de son propre affichage.

## Comportement actuel

Décrit en détail dans « Contexte » ci-dessus. En résumé : deux systèmes concurrents
(mode + ordre direct), dont le second gagne, expire au bout de 4 h, n'est annulé que par
certaines voies, et **n'est affiché nulle part**.

## Contraintes techniques

- **ADR-0017** : la décision reste dans le module PUR `schedule_logic.cpp`. La coquille
  `filtration.cpp` n'héberge aucune logique de décision.
- **Changements minimaux hors périmètre** : ne pas toucher au calcul de `computeAutoSchedule()`
  lui-même (bug-126, sélection de sonde, offset borné, ADR-0074) — seulement à la **condition**
  qui décide s'il s'applique.
- **Soustractions de temps non signées** partout où il en reste.
- ⚠️ **Parité `/data` ↔ WebSocket** — 4e récidive constatée : `filtration_force_on` /
  `filtration_force_off` sont publiés par `src/ws_manager.cpp:933-934` et **absents** de
  `src/web_routes_data.cpp` (vérifié : 2 occurrences contre 0). Ces champs **disparaissent**
  avec l'ordre direct ; vérifier qu'aucun champ ne subsiste d'un seul côté.
- **Home Assistant** : deux entités existent aujourd'hui — un `switch` « Filtration
  Marche/Arrêt » (`mqtt_manager.cpp:2431-2441`, topic `filtration/set`) et un `select` « Mode
  Filtration » (topic `filtration_mode/set`). **À arbitrer et à écrire** : le `switch` doit
  piloter le mode (Marche/Arrêt) ou disparaître au profit du seul `select`. Toute modification
  des entités publiées impose de revoir l'empreinte d'auto-discovery (ADR-0085).

## Fichiers potentiellement concernés

`src/schedule_logic.{h,cpp}` (`decideFiltrationRun`) · `src/filtration.{h,cpp}`
(`computeAutoSchedule`, expiration 4 h lignes ~283-305, `forceOnStartMs`/`forceOffStartMs`) ·
`src/mqtt_manager.{h,cpp}` (lignes ~415, 1652-1699, 2431-2441) ·
`src/web_routes_config.cpp` (~969-990, 1142) · `src/web_routes_data.cpp` ·
`src/ws_manager.cpp` (~933-934) · `src/config.{h,cpp}` (champs NVS `forceOn`/`forceOff`,
migration) · `data/app.js` (~6114, 6136, 12012, 12037) · `data/index.html` (~250-251, 478-479) ·
`test/test_native_schedule_logic/`

## Livrables — check-list

- [x] A1 — Inventaire **nominatif** de tout ce qui lit ou écrit `forceOn` / `forceOff`
      (firmware, NVS, MQTT, HTTP, WS, UI), avant toute suppression.
- [x] A2 — Verdict `pool-chemistry` : la garde de présence d'eau du dosage reste intacte, et
      aucun chemin ne peut faire tourner les pompes doseuses sans circulation.
- [x] A3 — Décision écrite sur l'entité `switch` de Home Assistant (pilote le mode, ou retirée)
      et conséquence sur l'empreinte d'auto-discovery.
- [x] A4 — Décision écrite sur la migration de configuration (`"manual"` → Programme,
      `"force"` → Marche, `forceOn`/`forceOff` persistés → effacés).
- [x] B1 — Les 4 modes sont traités **explicitement** dans `decideFiltrationRun()` ; plus aucun
      mode ne tombe dans un `return` par défaut.
- [x] B2 — L'ordre direct, son minuteur de 4 h et ses champs d'état sont **supprimés**, pas
      neutralisés. Aucune constante orpheline, aucun champ WS ou HTTP résiduel.
- [x] B3 — Le changement de mode s'applique immédiatement par **toutes** les voies (web, MQTT,
      HA), prouvé par un test.
- [x] C1 — **Mesure de simplification** : lignes supprimées vs ajoutées, et nombre de champs
      d'état retirés. Ce lot doit être **net négatif** — c'est la demande explicite de
      l'utilisateur.
- [x] D1 — Tests natifs sur `decideFiltrationRun` : les 4 modes × (dans le créneau / hors
      créneau) × (heure connue / inconnue), plus le mode inconnu.
- [x] D2 — **Mutation** : rendre `"programme"` identique à `"auto"` (recalcul du créneau) ⇒ un
      test doit mourir ; faire tomber un mode dans le `return` par défaut ⇒ un test doit mourir.
- [x] D3 — Non-régression du Mode Boost (feature-053) et de la garde « heure indisponible ».

## Sécurité chimique

Le dosage n'est autorisé que si l'eau circule. Ce lot modifie le chemin qui décide « la pompe
de filtration tourne », donc **indirectement** la garde de présence d'eau. `pool-chemistry`
doit confirmer qu'aucun des 4 modes, ni la migration, ni la suppression de l'ordre direct ne
peut ouvrir un chemin vers les pompes doseuses sans circulation réelle.

## ADR attendu ?

**Oui, probablement.** Critères pressentis : on **retire** un mécanisme de contrôle et on
verrouille un principe — *« le mode affiché est la seule autorité ; aucun état caché ne peut le
contredire »*. À confirmer par l'`architect`.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
`/get-config`, `/save-config`, `/data` — champs de filtration ; disparition de tout champ
d'ordre direct.

### Champs WebSocket
`filtration_force_on`, `filtration_force_off` → **supprimés**. Vérifier la parité avec `/data`.

### Topics MQTT
`filtration_mode/set` (les 4 valeurs), `filtration/set` (selon la décision A3), et l'état
publié correspondant.

### Documentation à produire / mettre à jour
`docs/subsystems/filtration.md` · `docs/features/page-filtration.md` (ou équivalent) ·
`docs/API.md` · `docs/MQTT.md` (entités HA + empreinte) · `docs/subsystems/ws-manager.md` ·
`CHANGELOG.md` · `docs/user/src/guide-utilisation.html` (les 4 modes, en langage simple) ·
ADR si retenu.

## Note de méthode — demande explicite de l'utilisateur

> « Encore une fois, tu as mis en œuvre des solutions complexes en imaginant résoudre des
> problèmes et tu en crées de nouveaux. Propose une solution pour revenir à un fonctionnement
> simple. […] Quand un utilisateur choisit d'arrêter, ça arrête. Quand il choisit le mode Auto,
> ça met le mode Auto. Il n'y a pas plus de question à se poser que ça. »

Et : *« je voudrais que tu en profites pour simplifier le code de l'application. Ce changement
doit permettre d'avoir un code plus simple, plus propre. »*

**Conséquence contraignante pour tous les agents** : ce lot **RETIRE**. Toute proposition qui
ajoute un mécanisme, un réglage, une confirmation, une temporisation ou un état doit être
justifiée explicitement, ou abandonnée. La simplification du code est un **livrable** (C1),
pas un effet secondaire espéré.

**Ne PAS basculer la configuration de l'utilisateur** : il reste en Auto avec son créneau
calculé. Décision explicite du 2026-10-09.

---

## ➲ DÉCISION UTILISATEUR DU 2026-10-09 — le Boost devient une option du mode Auto

**Demande, mot pour mot** : « je propose que le mode Boost ne soit disponible qu'en mode Auto.
Ce serait un moyen de simplifier tout le fonctionnement. Le Boost serait une option sous le
mode Auto. Elle garderait le même fonctionnement et se désactiverait automatiquement une fois
son délai de fonctionnement dépassé. On reviendrait alors en mode Auto. »

### Ce que ça change
- Le Boost n'est **plus une priorité qui écrase tous les modes**. Il devient une **option
  interne au mode Auto**. `boostForce` n'est consulté **que** si le mode est `Auto`.
- En **Programme**, **Marche** et **Arrêt**, le Boost n'a aucun effet et ne peut pas être
  activé.
- Son fonctionnement propre est **inchangé** : même durée, même expiration automatique. À
  l'expiration on revient au mode Auto nominal (créneau calculé).
- Quitter le mode Auto alors qu'un Boost est actif **annule le Boost** — à implémenter
  explicitement, et à journaliser.

### Pourquoi c'est la bonne correction du défaut de sécurité
Le gate `pool-chemistry` a relevé un défaut **PRÉEXISTANT et BLOQUANT** (`filtration.cpp:374`) :
`relayShouldBeOn = state.running && (filtrationCfg.mode != "off")`. Avec **Boost actif + mode
`off`**, `decideFiltrationRun()` renvoie `true` ⇒ `state.running = true`, mais le relais reste
**OUVERT** — et `resolveWaterPresence()` lit `state.running`, **pas l'état du relais**. L'appareil
annonce donc `waterPresent = true` **sans aucune circulation**, et autorise le dosage : acide et
chlore injectés dans une conduite immobile.

Subordonner le Boost au mode Auto rend cette combinaison **impossible par construction**. Le cas
disparaît au lieu d'être rattrapé.

### ⚠️ La correction de la ligne du relais reste EXIGÉE malgré tout
Le trou devient inatteignable, mais le code conserverait **deux sources de vérité** pour « la
pompe tourne » (`state.running` d'un côté, `relayState` de l'autre). C'est précisément ce qui a
permis au défaut d'exister. ⇒ `relayShouldBeOn = state.running`, **source unique**, et
l'invariant `state.running == relayState` en mode `ManagedFiltration` prouvé par test.

### Conséquence assumée
Un utilisateur en mode **Programme** ne peut plus lancer de Boost (après un orage, une forte
fréquentation). Repli : basculer en Auto le temps du Boost. Arbitrage explicite de
l'utilisateur, qui est en Auto. **Ne pas le « compenser » par un mécanisme : ce lot RETIRE.**

### Impacts à traiter
- `decideFiltrationRun()` : `boostForce` n'est consulté que dans la branche `Auto` — il cesse
  d'être la première condition du `switch`.
- Activation du Boost (HTTP, MQTT `boost/set`, UI) : **refusée** hors mode Auto, journalisée,
  avec republication de l'état pour resynchroniser Home Assistant.
- Changement de mode Auto → autre chose : **annule** le Boost actif.
- UI : le contrôle Boost n'est proposé qu'en mode Auto.
- **ADR-0025 (Mode Boost)** : son indépendance vis-à-vis du mode était sa propriété
  définissante. Elle disparaît. ⇒ à **amender ou remplacer**, verdict à l'`architect`.
- Tests : le cas « Boost l'emporte sur Arrêt » est **inversé** — il doit désormais prouver que
  le Boost n'a **aucun** effet hors Auto.

### ➲ AMENDEMENT du 2026-10-09 — le Boost MODIFIE LE CRÉNEAU, il ne décide plus

**Proposition de l'utilisateur, mot pour mot** : « on pourrait faire en sorte que le mode Boost
modifie le créneau horaire du mode Auto. Il n'y aurait qu'une source de vérité. »

Cette version **remplace** celle de la section précédente (« Boost consulté dans la branche
Auto ») : elle va plus loin et supprime une branche de plus.

- **`boostForce` DISPARAÎT de `decideFiltrationRun()`** — paramètre retiré, branche retirée.
  Signature cible : `decideFiltrationRun(mode, haveTime, nowMin, startMin, endMin,
  currentlyRunning)` — **deux paramètres de moins qu'à l'origine**. Le `switch` reste exhaustif
  **sans `default`**.
- Fonction PURE nouvelle (`schedule_logic`) :
  `RunWindow effectiveRunWindow(ScheduleWindow base, bool boostActive, int nowMin);`
  → `if (!boostActive) return base; return { nowMin, 1440 };`
- **`nowMin → 1440` est EXACT, pas une approximation** : le Boost expire **au passage de
  minuit** (`tickBoostExpiry`, `config.cpp:1251-1260` — vérifié). `endMin = 1440` plutôt que `0`
  évite le cas dégénéré `start == end` à 00:00 pile, où `isMinutesInRange` renverrait **false**.
- **Dérivation en RAM uniquement.** Rien n'écrit `filtrationCfg.start/end` ⇒ rien à restaurer
  ⇒ aucune seconde source de vérité.

#### ⚠️ RÈGLE CENTRALE (gate `pool-chemistry`)
> **Le créneau EFFECTIF ne sert QU'À `decideFiltrationRun()`. Tous les autres consommateurs
> restent sur le créneau de BASE.**

Garantie exigée : un **type distinct** (`RunWindow` vs `ScheduleWindow`) — passer le créneau
élargi à l'échantillonnage de température **NE COMPILE PAS**. Le compilateur, pas un
commentaire.

#### Pourquoi la non-réécriture est VITALE (argument décisif du gate)
`PendingSchedule::basisStart/basisEnd` (`filtration.cpp:515-517`) est un instantané de
`filtrationCfg.start/end`, utilisé pour détecter que l'utilisateur a changé son créneau. Si le
Boost écrivait les heures, `basisUnchanged` deviendrait faux et **chaque planning en attente
serait jeté SILENCIEUSEMENT** — l'apprentissage automatique de la durée de filtration mourrait
sans un mot.

#### Deux piges nommés
1. **Échantillonnage de température (ADR-0079 / C2)** : `isSampleEligible` DOIT rester sur le
   créneau de BASE. Sur l'effectif, un Boost nocturne ferait entrer des mesures FROIDES dans la
   moyenne ⇒ créneau calculé **plus court** le lendemain ⇒ moins de brassage, juste après
   l'épisode qui en appelait plus. **Le Boost se punirait lui-même avec un jour de retard.**
2. **`remainingRangeMinutes()`** (`pump_controller.cpp:4337`, `4822`) : lit la BASE ⇒ dosage
   réparti refusé pendant la rallonge de Boost. **Statu quo, fail-closed, aucune régression** —
   mais à **ÉCRIRE** dans le code pour que ce ne soit plus un accident heureux.

#### Conditions du gate `pool-chemistry` (GO SOUS CONDITIONS)
1. **[BLOQUANT]** `relayShouldBeOn = state.running`, **SOURCE UNIQUE**, invariant
   `state.running == relayState` en `ManagedFiltration` prouvé par test. Motifs relevés :
   le terme actuel lit le mode **brut** quand la décision lit le mode **minusculisé**, et il
   **relit la config vivante APRÈS la décision** ⇒ course réelle avec les callbacks web/MQTT
   dans le même tour.
2. **2′ [BLOQUANT]** Quitter Auto **remet `boost_until` à zéro** — l'effacer, pas l'ignorer.
   Sinon un retour en Auto avant expiration **réélargit le créneau tout seul**. Test :
   Auto+Boost → Arrêt → Auto ⇒ créneau de base.
3. Mode `Inconnu` → `currentlyRunning`, `critical` throttlé, coercition maintenue par
   `ensureTimesValid()`.
4. `scheduledMinutesToday()` → **1440** pour Marche (affichage SEUL, vérifié : consommateurs
   `web_routes_data.cpp:574` et `ws_manager.cpp:1011`, ne borne rien). Commentaires citant
   `off`/`force` à mettre à jour, sinon ils mentent.
5. Purge armée au boot prouvée par test — **étendue au boot en mode Marche**, atteignable pour
   la première fois.
6. Migration `"force"` → Marche : l'appareil passe de pompe **arrêtée** à pompe **tournante**
   ⇒ `warning` au boot + alerte MQTT.

#### ADR
**ADR-0025 est REMPLACÉ** (renforcé) : le Boost ne perd pas seulement sa priorité, il perd son
**chemin dédié** — `boostForce` cesse d'exister comme entrée de décision. Rien d'amendable.
ADR-0093 absorbe les deux décisions.

### ➲ AMENDEMENT 2 du 2026-10-09 — le Boost est disponible PARTOUT SAUF en mode Arrêt

**Demande de l'utilisateur** : « pourquoi ne pas donner aussi l'option Boost en mode Programme.
Je pense que ce n'est pas coûteux. » → généralisé après analyse : **seul le mode Arrêt
l'exclut**. Cet amendement REMPLACE la restriction « Boost réservé au mode Auto » des deux
sections précédentes.

#### Ce que fait réellement le Boost — à connaître avant de raisonner
Ce **n'est pas** un mode de brassage. C'est un mode de **SURCHLORATION** à **trois effets** :
1. **consigne ORP relevée** — `effectiveOrpTargetPure()` (`dosing_logic.cpp:549`) ;
2. **plafond de chlore relevé** — `effectiveMaxChlorinePure()` (`dosing_logic.cpp:560`) ;
3. **filtration forcée** — qui n'est pas le but mais le **SUPPORT** des deux premiers : il faut
   que l'eau circule pour distribuer le chlore supplémentaire.

Autres consommateurs de `isBoostActive()`, **non touchés par ce lot** et qui continuent de le
lire directement : `temp_mode_logic.cpp:34`, `time_to_target.cpp:301`,
`pump_controller.cpp:2779/2794/2805`, `ws_manager.cpp:1083`.
⚠️ Ce lot ne transforme en modificateur de créneau **QUE l'effet n° 3**. Les effets 1 et 2
restent inchangés.

#### Pourquoi la restriction n'est plus une garde de sécurité
Avec la conception retenue, le trou est fermé **DEUX FOIS** :
- le Boost ne contourne plus la décision (il ne fait que décaler le créneau, et `Arret`
  n'interroge pas le créneau) ;
- `relayShouldBeOn = state.running` (condition 1) ⇒ relais et décision ne peuvent plus diverger.
La règle devient donc une **règle de cohérence**, pas un garde-fou.

#### Règle retenue
> **Le Boost surchlore ; il lui faut de l'eau qui circule ; il est donc impossible en mode
> Arrêt, et disponible partout ailleurs.**

| Mode | Boost | Effet |
|---|---|---|
| **Auto** | autorisé | créneau étendu jusqu'à minuit + surchloration |
| **Programme** | autorisé | identique — aucune différence de traitement |
| **Marche** | autorisé | effet filtration sans objet (pompe déjà en marche) ; **surchloration active** |
| **Arrêt** | **refusé** | pas de circulation ⇒ pas de dosage ⇒ surchloration impossible |

#### Conséquences sur l'implémentation
- `startBoost()` : la garde devient `mode == Arret` → refus (au lieu de `mode != Auto`).
  **Même coût**, même entonnoir unique.
- Annulation dans `filtration.update()` : `if (isBoostActive(now) && mode == Arret) stopBoost();`
  La condition 2′ du gate (**effacer** `boost_until`, ne pas l'ignorer) reste **entièrement
  valable** : un Boost latent qui se réarmerait en quittant Arrêt serait un état caché.
- `effectiveRunWindow()` inchangée. En `Marche` elle n'est **pas consultée** (la branche renvoie
  `true` sans lire le créneau) — cohérent.
- UI : le contrôle Boost est proposé dans les trois modes, grisé (avec motif) en Arrêt.

#### ⚠️ À FAIRE VALIDER PAR `pool-chemistry` — cas NEUF, jamais examiné
**Boost + mode Marche** : filtration permanente + consigne ORP relevée + plafond de chlore
relevé, le tout jusqu'à minuit. Les plafonds horaire et journalier s'appliquent toujours, mais
la combinaison n'a jamais existé (le mode Marche était inatteignable). Vérifier qu'aucun
plafond ne se trouve desserré par l'addition des deux.

#### ✓ Validation `pool-chemistry` de l'amendement 2 (delta 3) — GO SOUS CONDITIONS

**Boost + Marche : AUCUN desserrage.** Les deux effets de dosage sont **multiplicatifs et
bornés EN DUR**, pas additifs : journalier chlore `×1,5` plafonné à
`kBoostDailyHardCapMl = 1000 mL`, cible ORP `+60 mV` plafonnée à `kBoostOrpCeilingMv = 850 mV`
(`dosing_logic.cpp:556, 566`). **Le plafond HORAIRE n'est pas relevé par le Boost.** La
filtration permanente rend éligibles 24 h au lieu de ~10 ⇒ le terme contraignant bascule de
`24 × horaire` vers le **journalier**, précisément celui qui est borné en dur. Volume maximal =
**1000 mL quelle que soit la durée**.

**Boost + Programme = Boost + Auto, en plus sûr** : en Programme le créneau de base n'est jamais
recalculé, donc aucun effet rétroactif possible via l'échantillonnage de température.

#### ⚠️ CONDITION 4 AMENDÉE — conséquence de l'atteignabilité du mode Marche
`remainingRangeMinutes()` (`pump_controller.cpp:4337`, `4822`) lit `filtrationCfg.start/end`. En
**Marche**, ce créneau ne décide plus rien mais bornerait encore le dosage **réparti** : on
doserait dans un créneau fantôme pendant que l'affichage annonce 1440 min. Fail-closed
(sous-dosage), mais **mensonger**.
⇒ Les deux lectures doivent s'accorder : `scheduledMinutesToday() = 1440` **ET**
`horizonMinutes = 1440 - nowMin`. **Patron déjà écrit** pour les modes non-`Managed`
(`pump_controller.cpp:4335`) — à RÉUTILISER, pas à dupliquer. Ce n'est pas une garde neuve.

#### ⚠️ CONDITION 2′ — argument RENFORCÉ, l'effacement n'est pas optionnel
**Seul l'effet FILTRATION du Boost passe par le mode.** La consigne ORP relevée et le plafond
de chlore ×1,5 lisent `isBoostActive()` **DIRECTEMENT** (`dosing_logic.cpp:551`, `562`). Un
`boost_until` simplement **ignoré** en mode Arrêt laisserait donc la consigne relevée et le
plafond desserré, et **la surchloration repartirait SEULE** au retour en Marche, sans geste
utilisateur. ⇒ `stopBoost()` (effacement), jamais un `if` qui ignore.
Test exigé : Arrêt → Marche ⇒ consigne ORP **nominale**.

#### Ce que le Boost stocke — et pourquoi la dérivation est possible
**Le Boost ne stocke AUCUN créneau.** `BoostState` (`config.h:273-276`) ne contient que
`bool active` et `time_t untilEpoch` (prochain minuit local), persistés en NVS sous
`boost_active` / `boost_until` (namespace `poolctrl`, `saveBoostState()`).
Il n'existe donc **pas deux créneaux côte à côte** : il y a **un créneau** (celui de
l'utilisateur) et **une échéance**. Le créneau élargi n'existe nulle part en mémoire — il est
dérivé à chaque cycle, consommé, puis jeté. C'est la différence entre « le Boost modifie les
heures » (qui exigerait de mémoriser les anciennes pour les restaurer ⇒ seconde source de
vérité) et « le Boost décale la lecture des heures » (qui n'exige rien).

---

## Notes d'implémentation (clôture, v2.63.0, 2026-10-09)

### Décisions retenues (voir amendements ci-dessus, le dernier prime)

- Jetons protocole **conservés** (`auto`/`manual`/`force`/`off`) — `parseFiltrationMode()`
  (`schedule_logic.h`/`.cpp`) unique site de correspondance jeton → sens.
- `decideFiltrationRun()` perd `boostForce`/`forceOn`/`forceOff` (2 paramètres de moins),
  `switch` exhaustif sur `FiltrationMode`, sans `default` — garanti par le compilateur
  (`-Werror=switch`, `build_src_flags` des env `esp32dev_core3` et `native`).
- Le Mode Boost ne décide plus : `effectiveRunWindow(base, boostActive, nowMin)` dérive un
  créneau effectif **en RAM**, jamais stocké dans `filtrationCfg` (préserve `PendingSchedule` et
  l'apprentissage automatique du planning, ADR-0079).
- Boost disponible dans tous les modes **sauf Arrêt** (règle finale de l'amendement 2) :
  surchloration qui exige une circulation d'eau. Passer en mode Arrêt avec un Boost actif
  **efface** `boost_until` (`stopBoost()`), ne se contente pas de l'ignorer — condition
  bloquante du gate `pool-chemistry` (2′).
- Source unique du relais : `relayShouldBeOn = state.running` dans `filtration.update()`, sans
  second terme — ferme un défaut de sécurité préexistant (Boost + mode Arrêt pouvait faire
  diverger `state.running` et l'état réel du relais).
- Migration : `"manual"` → Programme (pas de changement de comportement), `"force"` → Marche
  (**changement réel** : la pompe passe d'arrêtée à tournante au boot — `warning` + alerte MQTT
  `filtration_mode_force_migrated`).
- `scheduledMinutesToday()` / `filtrationDosingHorizonMinutes()` (`pump_controller.cpp`,
  nouvelle fonction factorisée) : mode Marche → 1440 minutes, affichage seul, ne borne aucun
  dosage.
- ADR-0025 (Mode Boost) **remplacé** par [ADR-0093](../../../docs/adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)
  — statut passé à « Superseded by ADR-0093 », corps non modifié.

### Mesure de simplification (C1)

- `src/` : **+155** lignes brutes, dont **190 sur 347** lignes ajoutées sont des commentaires
  ⇒ **code métier net : +29 lignes**, pour : −2 paramètres de `decideFiltrationRun()`,
  −2 branches de priorité (`forceOn`/`forceOff`), −4 champs d'état (`FiltrationConfig::forceOn`,
  `forceOff`, `FiltrationRuntime::forceOnStartMs`, `forceOffStartMs`), −2 clés WebSocket
  (`filtration_force_on`, `filtration_force_off`), −1 minuteur (`kForceTimeoutMs`),
  **−1 mécanisme entier** (l'ordre direct).
- `data/` : **−35** lignes nettes (un seul chemin `setFiltrationRunMode()` partagé par les
  quatre jeux de boutons Démarrer/Arrêter au lieu de quatre handlers dupliqués).
- `test/` : **+246** lignes — couverture neuve : matrice complète 4 modes × dans/hors créneau ×
  heure connue/inconnue × état courant sur `decideFiltrationRun`, plus `parseFiltrationMode` et
  `effectiveRunWindow` dédiés (y compris le piège de minuit `nowMin=0` → `endMin=1440`, pas `0`).

### Fichiers modifiés

`src/schedule_logic.{h,cpp}`, `src/filtration.{h,cpp}`, `src/mqtt_manager.cpp`,
`src/web_routes_config.cpp`, `src/web_routes_control.cpp`, `src/pump_controller.{h,cpp}`,
`src/config.{h,cpp}`, `src/ws_manager.cpp`, `platformio.ini`, `data/app.js`, `data/index.html`,
`test/test_native_schedule/test_schedule_logic.cpp`,
`test/test_native_ws_data_parity/test_ws_data_parity.cpp`, `src/version.h` (2.63.0).

### Documentation mise à jour dans ce même lot

`docs/adr/0093-le-mode-de-filtration-est-la-seule-autorite.md` (nouveau),
`docs/adr/0025-mode-boost.md` (statut → Superseded), `docs/adr/README.md`,
`docs/subsystems/filtration.md`, `docs/subsystems/pump-controller.md`,
`docs/subsystems/ws-manager.md`, `docs/features/page-filtration.md`,
`docs/features/page-dashboard.md`, `docs/subsystems/web-server.md`, `docs/API.md`,
`docs/MQTT.md`, `docs/test-on-target.md`, `docs/user/src/guide-utilisation.html`,
`CHANGELOG.md`.

⚠️ **Capture d'écran à refaire** (seule étape humaine) : `screenshots/04_Filtration.jpg` — le
sélecteur de mode et le texte d'aide ont changé (Auto / Programme / Marche / Arrêt, description
des quatre modes en permanence affichée). Les PDF utilisateur ne sont **pas** régénérés par ce
lot (réservé à `/release`).
