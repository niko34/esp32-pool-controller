# feature-145 — Butée de plausibilité sur l'ORP brut

## Métadonnées
- **ID** : feature-145
- **Slug** : butee-de-plausibilite-sur-l-orp-brut
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale — manque réel mais borné (voir « Contexte ») ; pas d'incident déclencheur, écarté comme prérequis au retrait de feature-139
- **Version cible** : N/A (à cadrer)
- **Dernière mise à jour** : 2026-10-08
- **Spec(s) liée(s)** : feature-139 (retrait de l'analyse de stabilité de la porte de mesure, v2.62.0) — [ADR-0092](../../docs/adr/0092-retrait-analyse-stabilite-porte-mesures.md) ; [ADR-0048](../../docs/adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (butées pH existantes, modèle à suivre)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : touche une garde de dosage chlore
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La voie **pH** dispose de quatre butées de plausibilité sur la mesure **brute** (6,20 bas / 8,10 haut, plus le plancher/plafond corrigés) : une lecture brute hors de ces bornes refuse le dosage **immédiatement**, sans analyse de tendance. La voie **ORP** n'a **aucune** butée équivalente — rien n'existe entre la sonde et la pompe à chlore hormis les plafonds horaire/journalier. Cette spec cadre l'ajout d'une butée grossière, non statistique, sur l'ORP brut.

## Contexte

Ce manque a été identifié le 2026-10-08 par le gate `pool-chemistry`, pendant l'instruction de feature-139 (retrait du détecteur de stabilité de la porte de mesure). Deux points établis à cette occasion :

1. **Le détecteur de stabilité retiré par feature-139 ne couvrait de toute façon pas ce cas** : c'est un détecteur d'étendue, aveugle à une mesure durablement fausse (stable). La voie ORP n'a donc **jamais** été protégée contre une sonde lisant faussement bas ou haut de façon stable — ce manque est **préexistant**, pas une régression de feature-139.
2. **Le coût concret identifié** (transitoire matinal, feature-069) : une chute de 56-74 mV de l'ORP au redémarrage de la filtration reste dans `kOrpMaxError`, donc la sortie P tourne à pleine puissance pendant ~45 min chaque matin ⇒ **0,45-0,6 ppm de chlore versés sans raison**, tous les matins, sans aucun garde-fou. Le gate a jugé ce coût « acceptable en soi, inacceptable sans aucun garde-fou ».
3. **Écarté comme prérequis au retrait de feature-139** (décision utilisateur) : 0,5 ppm est borné, temporaire et auto-corrigé dès que l'ORP remonte — ajouter une butée pour cela aurait remplacé un mécanisme par un autre au lieu d'en retirer un. Le besoin reste réel et distinct.

Une sonde ORP qui dérive et lit **faussement bas en permanence** (pas seulement au transitoire matinal) ferait doser jusqu'au plafond tous les jours, sans qu'aucune garde ne le signale avant que l'utilisateur ne le remarque (ORP qui ne monte pas, consommation de bidon anormale).

## Comportement attendu

À cadrer par `architect` + `pool-chemistry`, sur le modèle des butées pH existantes ([ADR-0048](../../docs/adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)) :

- une butée **basse** et une butée **haute** sur l'ORP **brut** (pas filtré), grossières et non statistiques — pas calées sur un bassin particulier ;
- refus de dosage chlore **immédiat** tant que le brut est hors bornes, indépendamment de la valeur filtrée ;
- alerte (journal + MQTT, patron `ph_guard`) à la pose et à la levée ;
- ne doit **pas** réintroduire une analyse de variation/tendance (ce serait recréer le mécanisme retiré par feature-139) — la butée porte sur la **valeur instantanée**, comme les butées pH.

### Cas limites
- Le transitoire matinal normal (chute 56-74 mV au redémarrage filtration, feature-069) ne doit **pas** déclencher la butée à tort — bornes à calibrer en conséquence.
- Sonde ORP jamais lue / hors plage physique (câble débranché) : distinct d'une butée de plausibilité, déjà couvert par les gardes de mesure existantes (`kSensorStaleTimeoutMs`) — ne pas dupliquer.
- Interaction avec `isOrpInhibitedByPh()` (inhibition chlore sur pH haut) : les deux gardes sont indépendantes, l'ordre de priorité d'affichage/log est à définir.

## Comportement actuel

Aucune butée sur l'ORP brut. Seules les gardes de fraîcheur/figement de la mesure (`kSensorStaleTimeoutMs`, filtre de tendance centrale) et les plafonds horaire/journalier de dosage protègent la voie ORP.

## Contraintes techniques
- Logique **pure** (ADR-0017) : la décision doit vivre dans un module testable en natif, comme les butées pH (`pump_controller.cpp` / module de garde dédié).
- Toute constante nouvelle dans `constants.h`, **avec la mesure qui la justifie et sa date** (ADR-0073) — ne pas recopier une valeur du pH sans vérifier qu'elle a un sens sur l'échelle ORP (mV, pas pH).
- Ne pas réintroduire de fenêtre glissante ni d'étendue calculée sur plusieurs échantillons : la butée doit être évaluable sur **une seule lecture**, par construction, pour rester un garde-fou universel au sens de l'ADR-0092.

## Fichiers potentiellement concernés
- `src/constants.h` — nouvelles bornes ORP brut
- `src/pump_controller.{h,cpp}` — garde de dosage chlore, patron `ph_guard`
- `test/test_native_*` — nouvelle suite ou extension d'une suite existante
- `docs/subsystems/pump-controller.md`, `docs/MQTT.md`, `docs/adr/`

## Livrables — check-list
- [ ] A1 — Bornes ORP brut chiffrées (mesure réelle, pas une estimation), avec marge documentée face au transitoire matinal connu (56-74 mV)
- [ ] A2 — Garde de dosage chlore implémentée, module pur testable en natif
- [ ] A3 — Alerte MQTT (patron `ph_guard`), pose et levée journalisées
- [ ] D1 — Tests natifs : transitoire matinal normal ne déclenche PAS la butée ; sonde faussement basse en permanence LA déclenche
- [ ] D2 — Mutation : resserrer/élargir la borne doit tuer un test
- [ ] C1 — `docs/subsystems/pump-controller.md`, `docs/MQTT.md`
- [ ] C2 — `CHANGELOG.md`

## Nouvelles signatures / API (optionnelle)
- Constantes : à nommer sur le modèle `kOrpFloorRawHard` / `kOrpCeilRawHard` (ou équivalent) — nom définitif à l'architect
- Topic MQTT : alerte sur le patron `ph_guard` (nom à définir, ex. `orp_guard`)

## Sécurité chimique
- **Dosage impliqué : OUI** — produit : chlore (ORP).
- Limites journalières / horaires concernées : aucune modifiée, la butée s'ajoute en amont.
- Filtration requise avant action : inchangé (garde de présence d'eau existante).
- Calibration capteur modifiée : non.
- Algorithme PID touché : non (garde amont, ne touche pas au calcul P).
- Risque identifié : une borne mal calibrée pourrait soit laisser passer une sonde faussement basse en continu (borne trop large), soit déclencher à tort sur le transitoire matinal normal (borne trop serrée) — **gate `pool-chemistry` obligatoire** sur les valeurs retenues.

## ADR attendu ?
- **Probable** : à confirmer par `architect` / `pool-chemistry` — probablement non (suit le patron déjà posé par l'ADR-0048 pour le pH, pas une décision nouvelle), sauf si le choix des bornes ou leur articulation avec l'inhibition pH→ORP s'avère structurant.
- **Critère(s) candidat(s)** : aucun a priori
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun a priori (à confirmer — si une valeur de butée devient visible dans `/data` ou `/get-config`, mettre à jour `docs/API.md`)

### Champs WebSocket
- Aucun a priori

### Topics MQTT
- Une nouvelle alerte, patron `ph_guard` — à documenter dans `docs/MQTT.md`

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — nouvelle garde de dosage chlore

#### Docs transverses
- `docs/MQTT.md` — nouvelle alerte
- `CHANGELOG.md` — nouvelle protection visible (comportement de sécurité)

#### Docs immuables
- `docs/adr/NNNN-*.md` — si tranché comme structurant par `architect`/`pool-chemistry`

## Critères d'acceptation
- [ ] Une sonde ORP lisant faussement bas ou haut de façon stable et durable refuse le dosage chlore
- [ ] Le transitoire matinal normal (feature-069) ne déclenche pas la butée
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués
- [ ] Tous les libellés utilisateur/journal sont en français

## Plan de test manuel
1. Rejouer le transitoire matinal connu (chute 56-74 mV) : la régulation doit continuer normalement.
2. Simuler (banc ou injection de valeur) une lecture ORP brute hors bornes stable : le dosage chlore doit être refusé et l'alerte émise.
3. Vérifier que la levée de la butée (retour en plage) réautorise le dosage et journalise la levée.

## Notes pour agent
- Ce besoin a été identifié par le gate `pool-chemistry` de feature-139 mais **explicitement écarté comme prérequis** à son retrait, sur décision de l'utilisateur : ne pas traiter cette spec comme urgente ou bloquante pour d'autres travaux.
- Suivre le **même principe** que les butées pH de l'ADR-0048 : bornes fixes, dérivées de la physique/chimie, pas du bassin particulier de l'utilisateur ni d'une statistique calculée à l'exécution (cohérent avec ADR-0092 : « les plafonds protègent, le reste informe »).

## Hors périmètre
- Toute réintroduction d'une analyse de variation/tendance sur l'ORP (retirée par feature-139/ADR-0092) — cette spec porte sur une butée **instantanée**, pas un détecteur de stabilité.
- Le retrait ou l'ajustement des plafonds horaire/journalier existants.
