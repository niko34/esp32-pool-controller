## Métadonnées
- **ID** : feature-219
- **Slug** : securite-des-pompes-coupure-commune-et-relecture
- **Type** : feature
- **Statut** : todo
- **Priorité** : haute — sans elle, le firmware ne dose pas sur le PCB v3
- **Version cible** : à fixer quand la première carte v3 sera là (firmware du PCB v3)
- **Dernière mise à jour** : 2026-11-28
- **Spec(s) liée(s)** : [feature-215](feature-215-conception-du-pcb-v3.md) (la carte),
  [feature-217](feature-217-carte-de-mesure-a-composants-courants.md) (l'autre chantier logiciel du v3)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [x] OTA
- [x] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : nouveau chemin vers `pump_controller`
- [x] ux-ui-designer — un défaut de sortie doit s'afficher et se lever depuis l'interface
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Sur le PCB v3 (carte principale rév. 0.3), les pompes doseuses ne reçoivent le 12 V que si le
firmware ferme un interrupteur commun, et la sortie de chaque pompe est relue. Le firmware doit
s'en servir : armer seulement pour injecter, vérifier avant, pendant et après, et bloquer le
dosage dès qu'une sortie ne répond pas comme attendu.

## Contexte
Étude des parasites et de la fiabilité du 2026-11-28 (demande de l'utilisateur). Sur le v2 et sur
le v3 d'avant la rév. 0.3, chaque pompe n'a qu'un interrupteur : un transistor qui lâche reste
passant, et la pompe dose sans arrêt — sans que le logiciel le voie ni puisse l'arrêter.

Le matériel est décrit dans
[`kicad/v3-principale/README.md`, « Sécurité des pompes »](../../../kicad/v3-principale/README.md#sécurité-des-pompes--rév-03-2026-11-28),
ses règles dans [`kicad/REGLES-CEM-ET-FIABILITE.md`](../../../kicad/REGLES-CEM-ET-FIABILITE.md) (P1 à P3).
Avis de `pool-chemistry` sur le montage, 2026-11-28 : accord sous conditions, toutes tenues côté carte.

## Comportement attendu
Les dix exigences E1 à E10 et les trois interdits du README de la carte. En résumé :

| Moment | Ce que fait le firmware |
|---|---|
| Démarrage | IO4 en sortie à 0 avant tout le reste ; rail coupé : les deux relectures lisent 0 |
| Avant d'injecter | commandes à 0, armer, attendre 1 ms, lire : chaque pompe présente lit 1 |
| Pendant | l'autre pompe est relue toutes les 100 ms : elle reste à 1 |
| Après | la relecture de la pompe arrêtée revient à 1 ; désarmer ; après 10 ms : 0 et 0 |
| Hors injection | rail coupé |
| Un contrôle échoue | désarmer aussitôt, bloquer **tout** dosage, le dire (journal `critical`, MQTT, WebSocket, interface) |

Broches : `ARME_POMPES` = IO4, `LU_POMPE_PH` = IO18, `LU_POMPE_ORP` = IO23.

### Cas limites
- **Une seule pompe installée** : une pompe déclarée absente n'est ni commandée ni surveillée.
  ⚠️ Une pompe débranchée ne se reconnaît pas de façon sûre : à froid elle se lit comme un
  transistor passant (0), à chaud la fuite de sa diode peut faire lire 1. ⛔ Ne pas en faire un
  diagnostic « pompe absente » — la présence d'une pompe se déclare, elle ne se devine pas.
- **L'interrupteur est commun** : le défaut d'une pompe bloque aussi l'autre. C'est voulu, et
  c'est à dire clairement à l'utilisateur.
- **Blocage** : il survit au redémarrage et ne se lève que par une action authentifiée.
- **Mise à jour** : désarmer avant `Update.begin()`.
- **Carte v2** : elle n'a ni interrupteur commun ni relecture. Le même firmware doit continuer
  d'y doser — comment il sait sur quelle carte il tourne est à décider (architect).

## Comportement actuel
Rien n'existe. `src/constants.h` déclare `kRtcSqwPin = 23` « pour plus tard », sans aucun emploi ;
IO4 et IO18 n'y figurent pas. Sur une carte v3 rév. 0.3, le firmware actuel pilote tout le reste
mais **ne fait tourner aucune pompe** : le rail n'est jamais armé.

## Contraintes techniques
- Toutes les règles de sécurité absolues de `CLAUDE.md` restent : limites de dosage,
  `resolveWaterPresent()` avant toute injection, chien de garde actif.
- ⚠️ Les relectures disent l'état des **sorties**. Elles ne disent ni qu'un produit a été injecté,
  ni que l'eau circule (ADR-0026, ADR-0144). Ne jamais l'écrire autrement, dans le code comme
  dans l'interface.
- ⛔ Jamais `gpio_hold_en` ni `rtc_gpio_*` sur IO4 : ils garderaient l'armement à travers un redémarrage.
- ⛔ Ne jamais relire une pompe pendant qu'elle est pilotée (signal haché à 20 kHz).
- ⛔ IO18 et IO23 se lisent en entrée **sans rappel interne** : avec un rappel vers le haut, la
  relecture resterait vers 1,2 V, ni 0 ni 1 (relecture `electronics-reviewer`, 2026-11-28).
- La lecture d'un niveau haut n'est garantie que si le 12 V reste au-dessus de 10,2 V.
- Un seul endroit du code arme et désarme, greffé sur `applyPumpDuty()` (`src/pump_controller.cpp`).
- La décision (quoi lire, quand, quoi conclure) est dans un **module pur**, testable hors carte (ADR-0017).

## Fichiers potentiellement concernés
- `src/constants.h` — broches, délais (1 ms, 10 ms, 100 ms)
- `src/pump_controller.*` — armement, contrôles, blocage
- `src/pump_output_guard_logic.*` — [nouveau] la décision, en module pur
- `src/ota_manager.*`, `src/web_routes_ota.cpp` — désarmer avant une mise à jour
- `src/ws_manager.*`, `src/mqtt_manager.*`, `data/` — dire le défaut, permettre de le lever
- `test/test_native_*` — [nouveau] suite du module pur

## Livrables — check-list
- [ ] A1 — broches et délais dans `constants.h` ; **retirer `kRtcSqwPin = 23`** (« INPUT_PULLUP
      attendu », sans aucun emploi) : IO23 est désormais la relecture de la pompe chlore
- [ ] A2 — module pur de décision
- [ ] A3 — armement et désarmement en un seul endroit, greffés sur `applyPumpDuty()`
- [ ] A4 — contrôles au démarrage, avant, pendant et après chaque injection
- [ ] A5 — blocage persistant, levé par une action authentifiée
- [ ] A6 — désarmement avant une mise à jour
- [ ] A7 — fonctionnement inchangé sur la carte v2
- [ ] B1 — le défaut est visible dans l'interface, avec ce qu'il faut faire
- [ ] B2 — MQTT et WebSocket portent le défaut
- [ ] C1 — documentation (régulation, sous-système pompes, guide utilisateur, CHANGELOG, ADR)
- [ ] D1 — suite native du module pur, éprouvée par mutation
- [ ] D2 — essais sur la première carte (plan de test ci-dessous)

## Sécurité chimique
- Dosage impliqué : oui — produits : pH− et chlore
- Limites journalières / horaires concernées : aucune n'est modifiée
- Filtration requise avant action : inchangé
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : un contrôle mal écrit qui laisse le rail armé en permanence ramène à la
  situation d'avant ; un contrôle trop sensible bloque le dosage à tort

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 3
- **Sujet pressenti** : l'interrupteur commun (une panne d'une pompe bloque les deux), et la façon
  dont un même firmware sert la carte v2 et la carte v3

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- à définir : lever un blocage (authentifié)

### Champs WebSocket
- à définir : état des sorties des pompes, blocage en cours

### Topics MQTT
- à définir — auto-discovery Home Assistant si un nouveau capteur binaire est créé

### Documentation à produire / mettre à jour
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/ws-manager.md`
- `docs/user/src/guide-regulation.html` et `guide-utilisation.html`
- `CHANGELOG.md`, `docs/adr/NNNN-…md`

## Critères d'acceptation
- [ ] Rail coupé au démarrage, à la remise à zéro et hors injection (oscilloscope sur IO4)
- [ ] Transistor d'une pompe mis en court-circuit exprès : armement refusé, pompe à l'arrêt, défaut affiché
- [ ] Interrupteur commun mis en court-circuit exprès : défaut signalé au démarrage
- [ ] Pompe débranchée : signalée comme « pompe absente ou sortie en défaut »
- [ ] Un blocage survit à un redémarrage et ne se lève qu'authentifié
- [ ] Sur une carte v2, le dosage est inchangé
- [ ] La suite du module pur tue ses mutants
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning ; `pio test -e native` sans régression

## Plan de test manuel
Les essais de la section « À mesurer sur le prototype » de
[`kicad/REGLES-CEM-ET-FIABILITE.md`](../../../kicad/REGLES-CEM-ET-FIABILITE.md#à-mesurer-sur-le-prototype).

## Notes pour agent
- ⛔ Ne rien écrire avant le passage de `pool-chemistry` sur le design.
- ⚠️ Les broches 13, 14 et 16 du connecteur de débogage portent ces trois signaux : ce sont les
  points de mesure des essais, et rien ne doit y être branché pendant un dosage.
- Essais sur un appareil **dédié**, jamais sur celui de production.

## Hors périmètre
- La mesure du pH et de l'ORP par les convertisseurs de la carte de mesure (feature-217).
- Toute modification des limites de dosage.
