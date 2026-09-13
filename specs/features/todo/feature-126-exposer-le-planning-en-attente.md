## Métadonnées
- **ID** : feature-126
- **Slug** : exposer-le-planning-en-attente
- **Type** : feature
- **Statut** : todo
- **Priorité** : haute — sans cette visibilité, un utilisateur qui observe un changement
  d'horaire de filtration à minuit n'a **aucun moyen** de savoir qu'il allait arriver, ni
  sur quelle température il repose
- **Version cible** : à déterminer
- **Dernière mise à jour** : 2026-09-13
- **Spec(s) liée(s)** : [bug-126](../doing/bug-126-le-recalcul-du-planning-annule-le-cycle-qui-l-a-produit.md), [ADR-0079](../../../docs/adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — pas de dosage/régulation/calibration touché par cette spec
- [x] ux-ui-designer — **obligatoire** : nouvelle information à intégrer à une carte
  existante (Filtration), arbitrage de présentation
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Exposer, côté `/get-config` et WebSocket, le **planning de filtration en attente**
introduit par `bug-126` ([ADR-0079](../../../docs/adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) : sa présence, les horaires qu'il contient, la
température de référence sur laquelle il repose, et la date à laquelle il a été calculé.
Sans cette information, un changement d'horaire de filtration à minuit n'est visible qu'a
posteriori, jamais annoncé.

## Contexte

`bug-126` a introduit un mécanisme où le planning auto de filtration est calculé à l'arrêt
d'un cycle et appliqué au passage de minuit suivant, jamais pendant que la pompe tourne.
C'était un arbitrage assumé du plan retenu (« arbitrage C ») : `ux-ui-designer` et
`web-ui-developer` n'étaient pas dans ce cycle, la priorité étant de corriger le défaut
(arrêt intempestif d'un cycle) avant d'exposer le nouveau mécanisme.

`code-reviewer`, en revue du lot, a signalé — hors de son périmètre habituel, mais à juste
titre — que cette absence d'exposition est un trou d'observabilité réel : **rien ne permet
à l'utilisateur de savoir que sa fenêtre de filtration va changer à minuit, ni sur quelle
température**. Un utilisateur qui consulte la page Filtration à 22h voit l'horaire du jour
en cours, sans aucune indication qu'un nouvel horaire attend d'être appliqué dans deux
heures.

## Comportement attendu

- `GET /get-config` expose un objet (nom à trancher par `architect`/`ux-ui-designer`, par
  exemple `filtration_pending`) contenant : présence d'un planning en attente (bool),
  heures de début/fin calculées, température de référence, nombre d'échantillons ayant
  produit la moyenne, date de calcul (`computedOnDate`).
- Le WebSocket expose les mêmes champs, en respectant la **parité stricte `/data` ↔
  WebSocket** — voir Contraintes techniques.
- La page Filtration affiche cette information de façon non intrusive : par exemple une
  ligne « Prochain planning (dès minuit) : 07:36-18:24, calculé sur 21.6 °C » quand un
  planning est en attente, rien si aucun n'est en attente (`writePendingSchedule()` ne
  produit pas systématiquement un `pending` — fenêtre inchangée, cycle trop court,
  température implausible, etc.).
- **Ne rien exposer côté MQTT/Home Assistant dans cette itération** (sauf décision
  contraire d'`architect` en cours de cycle) — le planning en attente est une information
  d'interface, pas un contrat externe à maintenir indéfiniment ; il change chaque jour et
  n'a pas vocation à être une entité HA persistante.

### Cas limites
- Aucun planning en attente (fenêtre identique, cycle jamais lancé depuis le boot, mode non
  auto) : la carte ne doit rien afficher de trompeur — pas de « --:-- » qui laisse croire à
  une valeur absente par erreur.
- Planning en attente **jeté** entre deux rafraîchissements de la page (saisie utilisateur
  concurrente qui invalide le `basis`) : la disparition de l'information ne doit pas être
  interprétée par l'utilisateur comme un bug.
- WebSocket déconnecté : la dernière valeur connue reste affichée avec l'indicateur habituel
  de désynchronisation (patron existant des autres cartes).

## Comportement actuel

Le planning en attente (`FiltrationManager::_pending`, `src/filtration.h`) existe en RAM
depuis bug-126 mais n'est lu par aucune route HTTP ni par `ws_manager.cpp`. Aucune UI ne le
consomme.

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- ⚠️ **Parité `/data` (ou `/get-config`) ↔ WebSocket obligatoire** — voir
  [ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md) et la note mémoire
  associée : **trois récidives déjà documentées** sur ce projet. Un champ exposé
  uniquement côté WebSocket ne « disparaît » jamais silencieusement pour l'utilisateur, il
  **clignote** — remplacé à chaque rafraîchissement HTTP par une valeur qui n'a pas ce
  champ. Le champ neuf doit être ajouté **des deux côtés dans le même commit**, prouvé par
  grep des deux chemins d'émission.
- Budget de la trame WebSocket : vérifier la marge restante avant d'ajouter des champs
  (voir [ADR-0074](../../../docs/adr/0074-temperature-figee-statu-quo-instrumente.md),
  note sur la marge à 43 o) — un planning en attente porte au moins deux horaires `HH:MM`,
  une température et un compteur, budget non négligeable.
- AsyncWebServer : aucun handler bloquant > 50 ms

## Fichiers potentiellement concernés
- `src/filtration.h` / `src/filtration.cpp` — accesseur(s) publics pour lire `_pending`
  sans exposer la structure interne telle quelle (respecter l'encapsulation existante)
- `src/web_routes_config.cpp` — `GET /get-config`
- `src/ws_manager.cpp` — champs neufs dans la trame `sensor_data` (ou équivalent)
- `data/app.js`, `data/index.html` — carte Filtration
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/features/page-filtration.md` —
  documentation des champs neufs

## Livrables — check-list
- [ ] A1 — accesseur `FiltrationManager` exposant l'état du planning en attente (lecture
  seule, pas de nouvelle méthode de mutation)
- [ ] B1 — champ(s) neufs dans `GET /get-config`
- [ ] B2 — champ(s) identiques dans la trame WebSocket (parité prouvée par grep)
- [ ] B3 — affichage dans la page Filtration (`data/`), maquette validée par
  `ux-ui-designer`
- [ ] C1 — `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/features/page-filtration.md`,
  `CHANGELOG.md`

## Nouvelles signatures / API (optionnelle)
- Champs `/get-config` nouveaux : à trancher par `architect` (ex. `filtration_pending_valid`,
  `filtration_pending_start`, `filtration_pending_end`, `filtration_pending_temp_c`,
  `filtration_pending_samples`)
- Champs WebSocket nouveaux : identiques, même nommage que `/get-config` (parité)
- Topics MQTT nouveaux : aucun dans cette itération

## Sécurité chimique (optionnelle)
- Dosage impliqué : non — cette spec n'ajoute que de l'observabilité, aucune écriture
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : aucun — lecture seule d'un état déjà calculé par bug-126

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun — c'est l'exposition d'un état déjà décidé par
  ADR-0079, pas une nouvelle décision structurante. Convention de nommage de champ à
  documenter dans `docs/API.md`, pas dans un ADR.
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-config` — champs neufs (planning en attente)

### Champs WebSocket
- Champs neufs, identiques à `/get-config` — parité stricte obligatoire

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-filtration.md` — nouvelle information affichée
- `docs/subsystems/filtration.md` — accesseur(s) neufs de `FiltrationManager`
- `docs/subsystems/ws-manager.md` — champs neufs de la trame

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — champs neufs de `/get-config`
- `CHANGELOG.md` — entrée à ajouter

#### Docs immuables
- Aucune (voir ADR attendu ci-dessus)

## Critères d'acceptation
- [ ] Un planning en attente valide est visible dans `/get-config` ET dans la trame
  WebSocket, avec les mêmes champs et les mêmes valeurs
- [ ] Aucun planning en attente ⇒ les champs le disent explicitement (pas d'ambiguïté avec
  une valeur absente par erreur réseau)
- [ ] La page Filtration affiche l'horaire à venir et la température qui le fonde quand un
  planning est en attente
- [ ] Aucune régression sur les champs existants de `/get-config` et de la trame WebSocket
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous

## Plan de test manuel
1. Provoquer un changement de température suffisant pour produire un planning en attente à
   l'arrêt d'un cycle ; vérifier que `/get-config` et la page Filtration l'affichent avant
   minuit.
2. Attendre (ou avancer l'horloge de test) le passage de minuit ; vérifier que
   l'information « en attente » disparaît au profit du planning promu.
3. Vérifier qu'aucun champ neuf n'apparaît uniquement côté WebSocket ou uniquement côté
   `/get-config` (grep des deux chemins d'émission).

## Notes pour agent
- Cette spec fait suite à une remarque de `code-reviewer` en fin de cycle `bug-126`, hors
  de son périmètre habituel de revue — signalée comme fondée par l'orchestrateur du cycle.
- Rappel mémoire projet : trois récidives déjà documentées sur la parité `/data` ↔
  WebSocket. Un champ WS-seul ne disparaît jamais silencieusement, il **clignote** —
  remplacé à chaque rafraîchissement `/get-config`.

## Hors périmètre (optionnelle)
- Toute entité MQTT / Home Assistant pour le planning en attente
- Toute modification du mécanisme de calcul ou de promotion du planning lui-même
  (ADR-0079, figé)
