## Métadonnées
- **ID** : feature-086
- **Slug** : limite-actionneur-dosage-ph
- **Type** : feature
- **Statut** : done — livrée en v2.29.0 (30/08), tous livrables réalisés, close le 06/09
- **Priorité** : haute
- **Version cible** : 2.29.0
- **Dernière mise à jour** : 2026-08-30
- **Spec(s) liée(s)** : `feature-084` (livrée en 2.27.0), `feature-085` (feedforward),
  `feature-078` (rendement des impulsions), `feature-079` (débits à pleine puissance)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
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
- [x] pool-chemistry — **obligatoire** (le sujet EST le volume injecté)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La régulation pH ne peut pas atteindre sa consigne parce que **la limite horaire
d'injection borne le débit moyen en dessous du besoin du bassin**. Ce n'est pas un
défaut du correcteur. Rendre cette limite visible, alertable et — après
vérification — relevable en sécurité.

## Contexte

L'utilisateur observe depuis plusieurs versions que le pH ne redescend pas à la
consigne, et a formulé le 30/08/2026 une critique de fond : *« le système
d'accumulation ressemble plus à un contournement qu'à une solution »*.

Deux versions ont été livrées en réponse — `feature-084` (cadence et mémoire de
l'intégrale) et la préparation de `feature-085` (feedforward appris). **Le visa
`pool-chemistry` du 30/08/2026 établit qu'aucune des deux ne peut résoudre le
problème**, et le chiffrage est sans ambiguïté :

| Grandeur | Valeur |
|---|---|
| Débit d'amplitude mesuré (banc 28/08) | ~26 mL/min |
| `phInjectionLimitMinutes` configuré | 5 min/h |
| **Débit moyen soutenable** | **2,17 mL/min** |
| Filtration | ~7 h/j (420 min) |
| **Maximum physiquement délivrable** | **~911 mL/jour** |
| Besoin de neutralisation estimé | **~1 800 mL/jour** |

**L'installation est limitée par l'actionneur, pas par le correcteur.** Un
feedforward parfait calculerait la bonne dose et se ferait couper par la même
limite horaire. C'est la raison pour laquelle `feature-085` seule ne suffira pas.

### Deux conséquences immédiates

1. **Le gel d'anti-windup « demande ≥ soutenable » est actif en permanence** sur
   ce bassin. L'intégrale ne peut pas charger, quelle que soit sa cadence.
2. **L'alerte « régulation saturée » est chimiquement vraie et légitime.** Elle
   désigne la limite horaire — elle a probablement été lue comme un faux positif
   ou comme un signe de bidon vide.

## Comportement attendu

### 1. Rendre la limite lisible (prérequis, aucun risque)

L'utilisateur doit pouvoir constater lui-même que le plafond est atteint, sans
lire le journal. Exposer, sur `/data` **et** WebSocket (parité — 5 récidives du
motif dans ce projet), puis afficher sur la page pH :

- le **débit moyen soutenable** courant, dérivé de la limite horaire configurée
  et du débit d'amplitude calibré (`phSustainableFlowFromConfig()` existe déjà,
  livrée en 2.27.0) ;
- le **maximum journalier délivrable** qui en découle, rapporté à la durée de
  filtration programmée ;
- l'indication explicite que la régulation est **bornée par la limite horaire**
  quand c'est le cas, distinguée d'un bidon vide ou d'une panne de sonde.

Formulation à soigner : le message doit dire *pourquoi* le pH ne descend pas, en
français, sans jargon d'automatique.

### 2. Alerter quand le plafond est structurellement insuffisant

Si, sur N jours consécutifs exploitables, le volume délivré atteint le maximum
possible **et** l'erreur pH reste positive, l'installation est sous-dimensionnée
en autorité de dosage. Le signaler comme tel — c'est une information de
diagnostic, pas une anomalie de fonctionnement.

L'anneau de charge 7 jours livré en 2.27.0 (`ph_load7`) fournit déjà la donnée.

### 3. Relever la limite — **sous conditions strictes**

C'est le seul remède réel, avec l'allongement de la filtration. Mais le visa pose
un avertissement chiffré qu'il faut respecter :

> Le volume injecté « à l'aveugle » pendant les 15 min de retard de mélange croît
> proportionnellement à la limite horaire.

| Limite horaire | Volume aveugle | Excursion pH correspondante (40 m³) |
|---|---|---|
| 5 min/h (actuel) | 130 mL | 0,046 pH |
| 15 min/h | 390 mL | 0,14 pH |
| 30 min/h | 780 mL | **0,28 pH** |

À comparer à `kPhSafetyMargin` = 0,50 et à `kPhFloorCorrected` = 6,70.

**Prérequis absolu** : les correctifs fail-open de la 2.27.0 (fenêtre d'âge,
dénominateur, purge sèche) doivent être **déployés et vérifiés sur cible** avant
tout relèvement. Le visa est explicite : l'immunité actuelle aux défauts de
l'intégrale *« est louée, pas possédée »* — elle repose entièrement sur les
5 min/h.

### Cas limites

- Pompe non caractérisée : le débit soutenable n'est pas calculable → afficher
  l'indisponibilité, ne rien inventer, ne pas alerter.
- Filtration très courte (hiver, 2 h/j) : le maximum journalier s'effondre ;
  l'alerte ne doit pas se déclencher sur une saison basse où la charge est
  également plus faible.
- Changement de produit ou de concentration : le besoin en mL change sans que le
  bassin change. L'alerte doit se réarmer, pas persister.
- Relèvement de la limite pendant une session en cours : ne doit pas rallonger
  l'impulsion déjà décidée.

## Comportement actuel

Rien n'existe. La limite horaire est configurable et appliquée, mais sa
**conséquence** — un plafond de débit moyen, donc un maximum journalier — n'est
calculée nulle part, n'est affichée nulle part, et n'est expliquée dans aucun
message. L'utilisateur constate un pH qui ne descend pas sans disposer de
l'élément qui l'explique.

## Contraintes techniques

- ESP32, offline first ; vanilla JS dans `data/` ; français dans toute l'UI
- **Flash à 97,6 %, marge 44,6 Ko.** C'est le facteur limitant du projet. Toute
  addition doit être justifiée ; privilégier la réutilisation de
  `phSustainableAvgFlow()` et `phSustainableFlowFromConfig()`, déjà livrées.
- AsyncWebServer : aucun handler bloquant > 50 ms
- Parité `/data` ↔ WebSocket à prouver par grep des deux côtés (motif à
  5 récidives : un champ WS-seul ne disparaît pas, il **clignote**)
- ADR-0049 : toute durée est un intervalle fermé depuis un instant d'armement

## Fichiers potentiellement concernés

- `src/pump_controller.cpp` — exposition du débit soutenable et de l'état « borné
  par la limite horaire » ; détection multi-jours via l'anneau `ph_load7`
- `src/web_routes_data.cpp` — champs `/data`
- `src/ws_manager.cpp` — mêmes champs, parité obligatoire
- `data/app.js`, `data/index.html`, `data/app.css` — affichage page pH
- `src/constants.h` — seuils de l'alerte multi-jours
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/features/page-ph.md`

## Livrables — check-list

- [x] A1 — Débit moyen soutenable et maximum journalier calculés et exposés
- [x] A2 — État « régulation bornée par la limite horaire », distinct de bidon
      vide et de panne de sonde
- [x] A3 — Détection multi-jours du sous-dimensionnement via l'anneau `ph_load7`
- [x] B1 — Affichage sur la page pH, avec explication en français
- [x] C1 — `docs/API.md`, `docs/features/page-ph.md`,
      `docs/subsystems/pump-controller.md`
- [x] C2 — CHANGELOG : l'utilisateur doit comprendre pourquoi son pH ne descendait pas
- [x] D1 — Tests natifs du calcul de plafond et de la détection multi-jours,
      **validés par mutation**
- [x] D2 — Parité `/data` ↔ WS prouvée par grep

## Nouvelles signatures / API

- Champs `/data` **et** WebSocket : `ph_sustainable_ml_per_min`,
  `ph_daily_max_ml`, `ph_limited_by_hourly_budget` (booléen)
- Constantes : seuil de jours consécutifs pour l'alerte de sous-dimensionnement

## Sécurité chimique

- Dosage impliqué : **oui** — produit : pH− (12 % H₂SO₄ + 13 % HCl, ~6,9 éq/L)
- Limites concernées : `phInjectionLimitMinutes`, quota journalier, anti-cycling
- Filtration requise avant action : oui (inchangé)
- Calibration capteur modifiée : non
- Algorithme PID touché : non pour les livrables A1-A3 et B1 ; **oui** si le
  relèvement de la limite est instruit dans la même itération
- **Risque identifié** : le point 3 (relèvement) augmente proportionnellement le
  volume injecté pendant le retard de mélange. À 30 min/h l'excursion atteint
  0,28 pH, soit plus de la moitié de `kPhSafetyMargin`. **Ne pas instruire le
  point 3 avant déploiement et vérification sur cible des correctifs 2.27.0.**

## ADR attendu ?

Oui, si le relèvement de la limite horaire est retenu : c'est un compromis entre
autorité de dosage et amplitude d'excursion, avec une alternative crédible
(allonger la filtration, qui dégaze aussi et ne rend que 15-20 % net). La
décision et son chiffrage doivent être tracés.

## Ordre recommandé

> **Note de cadrage — 30/08/2026.** Le cycle courant (version cible 2.29.0,
> v2.28.1 déployée le 30/08) couvre les **points 1 et 2 uniquement** (livrables
> A1, A2, A3, B1, C1, C2, D1, D2). Le **point 3 (relèvement de
> `phInjectionLimitMinutes`) est HORS PÉRIMÈTRE et verrouillé** jusqu'à la fin
> de l'observation terrain, jalons : 3 journées exploitables dans l'anneau
> `ph_load7` (~2-3/09), 72 h de feedforward (~5-6/09), et une restauration
> vérifiée. Par ailleurs, **MQTT est hors périmètre de ce cycle** (budget
> flash) — follow-up possible dans une spec ultérieure.

1. **Points 1 et 2** (lisibilité et alerte) — aucun risque chimique, gain
   immédiat de compréhension. À faire même si le reste est repoussé.
2. Déploiement et vérification sur cible de la 2.27.0.
3. **Point 3** (relèvement) — seulement ensuite, avec visa `pool-chemistry` amont
   et contre-visite, et observation du pH sur 2 h après le premier relèvement
   (critère : jamais sous consigne − 0,10).

## Fausse piste à ne pas reprendre

Chercher la cause du déficit dans le correcteur. Trois itérations l'ont fait —
cadence de l'intégrale, persistance, feedforward — et le chiffrage montre
qu'aucune ne pouvait aboutir. Le facteur limitant est le produit
`débit d'amplitude × fraction horaire autorisée`, rien d'autre.

## Notes d'implémentation — points 1-2 (v2.29.0, 30/08/2026)

**Livrables A1, A2, A3, B1, C1, C2, D1, D2 livrés.** La spec **reste en
`doing/`** : le point 3 (relèvement de `phInjectionLimitMinutes`) est verrouillé
jusqu'aux jalons — 3 journées exploitables dans l'anneau (~2-3/09), 72 h de
feedforward (~5-6/09), restauration vérifiée.

### ⚠️ Rectificatif majeur (30/08, soir — première lecture des indicateurs)

La première lecture des champs livrés révèle `ph_limit_minutes = 25` sur
l'appareil, pas 5 : **l'utilisateur avait déjà relevé la limite** (entre le
24 et le 30/08 — confirmé par lui, date exacte inconnue ; bornée par les
volumes : 765-802 mL le 23/08 ⇒ limite encore à 5 ; 1 052 mL le 30/08 ⇒ 25).
Toute la chaîne d'analyse (ADR-0053, cette spec, le « ~911 mL/j ») raisonnait
sur la **valeur d'usine** de `config.h`, jamais relue sur l'appareil.

Conséquences :
- **Le point 3 est SANS OBJET** — le relèvement a déjà eu lieu. Ce qui reste
  des jalons d'observation vaut toujours (les protections 2.27.0/2.28.0
  doivent être observées, d'autant que le volume de mélange aveugle à
  25 min/h ≈ 650 mL/h ≈ 0,23 pH sur 40 m³).
- Le maximum délivrable réel est **~8 450 mL/j** (25/60 × 26 mL/min × 13 h
  programmées) : l'installation n'est plus limitée par l'actionneur. Premier
  résultat cohérent : pH 7,298 (consigne 7,30) sous filtration active le
  30/08 17h31, 1 052 mL délivrés dans la journée.
- Le bandeau « Dosage au maximum autorisé » et l'alerte de sous-dimensionnement
  ne devraient pas apparaître sur cette installation — c'est le comportement
  honnête attendu, les livrables restent corrects (calculés sur la config
  réelle, ADR-0054).
- **Leçon de méthode** (reprise dans l'ADR-0056) : toute analyse doit lire la
  **configuration vivante** de l'appareil (`/get-config`), jamais les valeurs
  d'usine des sources.

### Choix retenus

- **N = 3 jours, marge 80 %** (`kPhActuatorLimitConsecutiveDays`,
  `kPhActuatorLimitDayFillRatio`) : marge < 90 % justifiée par l'incident du
  23/08 — délivré réel 84-88 % du max théorique en saturation permanente
  (765-802 mL / ~911 mL). Un seuil à 90-95 % ne se déclencherait jamais.
- **Réarmement implicite** : la série est réévaluée au débit soutenable
  **courant** — relever la limite fait retomber l'alerte, la baisser rend les
  journées passées implausibles. Trade-off assumé (relever blanchit 7 jours
  d'historique = l'action corrective attendue). Consigné dans
  [ADR-0056](../../../docs/adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md).
- **Garde mesure-valide ajoutée en cours de cycle** sur les DEUX accesseurs
  booléens : `anomalyHeldMs` gèle quand la branche d'entretien s'arrête sur
  mesure invalide — trou trouvé par le `test-engineer` (montée du streak 2→3 à
  la clôture pendant une panne de sonde ⇒ alerte sur épisode figé), corrigé.
- Zéro NVS dans `update()` : cache `_phLimitStreakDays` relu au démarrage
  différé et à la clôture de journée seulement. `scheduledMinutesToday()` :
  comparaison exacte alignée sur `decideFiltrationRun`, capacité ANNONCÉE
  (forceOn/boost hors plage n'y entrent pas).

### Verdicts

- Visa `pool-chemistry` (léger) : **GO SOUS CONDITIONS** — les 2 conditions
  prouvées (garde mesure-valide par test, parité `/data` ↔ WS par grep).
- Revue de code : **Approved**.
- Tests : **994/994 natifs verts** (nouvelle suite
  `test_native_ph_actuator_limit`), **5/5 mutants tués**.

### Deltas d'empreinte

- Firmware : **+2 428 o** (légèrement ajusté par les mineures de revue).
- Filesystem gzippé : **+800 o**.

### Constat E1 (cas limite « relèvement en cours de session »)

Relever la limite horaire en cours de session **ne rallonge pas** l'impulsion
déjà décidée : la limite est une **porte d'entrée relue à chaque tour** (budget
horaire vérifié avant chaque planification de pouls), pas une entrée du plan
d'impulsion. Aucun correctif nécessaire.

### Anomalie séparée à ouvrir si souhaité (hors cycle, relevée en revue)

Lecture async des `String` de `filtrationCfg` (`mode`/`start`/`end` lus par
`scheduledMinutesToday()` depuis les handlers web) — motif **préexistant**
partagé avec `/get-config`, non introduit par ce cycle. À traiter dans une spec
dédiée si souhaité.


---

## Clôture — 06/09/2026

**Tous les livrables sont réalisés** (A1-A3, B1, C1-C2, D1-D2) et la fenêtre
d'observation prévue au plan est close. Livrée en **v2.29.0 le 30/08**.

*(Les cases non cochées de « Domaines touchés » — filtration, MQTT, UART, OTA,
authentification, historique — signalent des domaines **non concernés**, pas des
livrables manquants.)*

### État en production au 06/09

`ph_limited_by_hourly_budget = false` — la régulation n'est pas bornée par
l'actionneur ce matin. Le témoin est donc en place, observable, et **au repos**,
ce qui est le comportement attendu quand le dimensionnement suffit.

### La leçon que cette spec laisse au projet

Le diagnostic initial — « la régulation est limitée par l'actionneur » — reposait
sur la limite d'**usine** de 5 min/h, alors que l'appareil était réglé à **25**.
D'où l'**ADR-0053** et une règle qui vaut au-delà de cette spec : **lire
`/get-config` avant tout diagnostic chiffré**, car la configuration vivante n'est
pas la configuration par défaut.

Le témoin livré ici sert précisément à ne plus avoir à le déduire : il dit, en
production et en français, si la limite horaire borne réellement le dosage.
