## Métadonnées
- **ID** : feature-096 (= item **N5** du plan `specs/plan-2026-09-regulation-et-robustesse.md`)
- **Slug** : indicateur-temps-d-atteinte-de-cible
- **Type** : feature
- **Statut** : done — **livré non déployé** (v2.37.0 ; déploiement groupé avec 094 + 095 + bug-089)
- **Priorité** : haute
- **Version cible** : 2.37.0 (l'appareil tourne en 2.35.0 ; la 2.36.0 — feature-094 — est
  commitée mais NON déployée ; cette spec porte la 2.37.0)
- **Dernière mise à jour** : 2026-09-05
- **Spec(s) liée(s)** : plan 2026-09 (juge de toutes les phases : O1-O4), feature-095 (N4, même cycle), feature-094 (à juger avec cet indicateur), feature-091 (porte de mesure : instant de départ), bug-089 (parité, même cycle)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — lecture de la filtrée et de l'état de la porte
- [x] Régulation (pump_controller, PID) → **passage par `pool-chemistry`** en lecture (aucun dosage modifié, mais l'indicateur lit l'erreur et les seuils de la régulation ; validation que rien n'est réinjecté dans la boucle)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (`/data` + WS, parité)
- [ ] MQTT / Home Assistant (option : un capteur HA par régulation — à trancher par l'architect, coût discovery)
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (mémoire 7 jours en NVS, ≤ 2 écritures/jour)
- [x] Frontend (`data/`)


> ⚠️ **Prérequis technique — RECTIFIÉ le 05/09/2026 (le texte du 04/09 était faux)**
>
> ~~« sérialisation tronquée silencieusement »~~ : **il n'existe aucune troncature
> silencieuse sur `/data`.** En ArduinoJson 7, `StaticJson<N>` dérive de
> `JsonDocument` et `N` est **purement documentaire** (le pool est alloué
> dynamiquement) ; `sendJsonResponse` sérialise dans une `String` dimensionnée à
> la demande. Ce chemin n'a **aucun plafond dur** — c'est écrit noir sur blanc dans
> `src/web_routes_data.cpp` lignes 57-62 et 75-77. Le gabarit 2304 reste néanmoins
> à tenir à jour **par honnêteté documentaire** (recompter le pire cas).
>
> Le **seul plafond DUR** du projet est `kWsFrameBufBytes = 3584` (`src/constants.h:178`),
> côté WebSocket. Sa garde `measureJson(doc) + 1 > cap` (`ws_manager.cpp:285`, `:666`,
> `:761`) **SAUTE le push** et incrémente `_oversizedFrames` (warn throttlé,
> `ws_manager.cpp:110-112`) — elle ne tronque **jamais**. Trame `sensor_data` au pire
> cas mesuré : **2 842 o**, soit **742 o de marge**. Les ~180 o de cette spec
> ramènent la marge à ~560 o : **aucun bump de `kWsFrameBufBytes` n'est requis**,
> seulement un bump du gabarit documentaire `StaticJson<2944>` → 3072 et un
> commentaire daté dans les deux fichiers.
>
> Budgets voisins, vérifiés : **feature-095 n'ajoute aucun champ à `/data` ni au WS**
> (ses livrables portent sur `/get-history` et `DataPoint` ; sa propre ligne
> « Aucun champ WS d'état nouveau ») → rien à lui réserver. **bug-089 touche la trame
> `config`** (`_buildConfigJson`, `ws_manager.cpp:670-674`, `StaticJson<2560>`),
> **distincte** de `sensor_data` : la garde est **par trame**, pas cumulative.

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — lecture seule des grandeurs de régulation ; doit confirmer qu'aucune sortie de l'indicateur n'alimente un dosage
- [x] ux-ui-designer — placement sur les pages pH/ORP et le tableau de bord, densité
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Le firmware mesure lui-même, chaque jour de filtration, le **temps mis par
chaque régulation (pH, ORP) pour atteindre et tenir sa cible** depuis la
première mesure valide de la journée, le journalise, le mémorise 7 jours et
l'affiche. C'est la mesure de succès du plan 2026-09 : sans elle, chaque
version se juge sur un CSV externe et une lecture humaine.

## Contexte
Deux matinées analysées à la main (02 et 04/09) donnent ~4 h 45 (pH) et
~5 h 50 (ORP) pour résorber 0,1-0,2 d'écart. Le plan vise ≤ 2 h avec 094, puis
≤ 1 h si N2 s'avère nécessaire. Sans indicateur embarqué : pas de comparaison
avant/après fiable, pas d'alerte quand une journée dérape, et un Mac qui doit
rester allumé.

## Comportement attendu

### Définition (identique pour pH et ORP, paramètres propres)
- **Départ (t0)** : premier instant du jour où la porte de mesure est ouverte
  (`measuresValid`) **et** la filtrée existe, filtration en marche. Un seul
  départ par jour civil (le premier) ; les reprises suivantes (re-purge,
  redémarrage) n'en créent pas un nouveau.
- **Écart initial e0** : erreur signée à t0 (filtrée − consigne), mémorisée.
- **Cible atteinte** : `|erreur| < seuil de démarrage` (`phStartThreshold`
  0,05 / `orpStartThreshold` 15 mV, valeurs vivantes de la config) **tenu
  pendant `kTimeToTargetHoldMin = 30` minutes de filtration consécutives**.
  Une sortie de la bande remet la tenue à zéro, pas le chronomètre.
- **Temps d'atteinte** : minutes de **filtration** écoulées entre t0 et le
  **début** de la tenue de 30 min (pas sa fin). Le temps hors filtration ne
  compte pas (le compteur monotone de filtration de feature-093 sert de base).
- **Non atteinte** : si la filtration s'arrête pour la journée (fin de plage
  horaire ou passage OFF > 1 h) sans tenue accomplie → état « non atteinte »,
  avec l'écart résiduel au moment de l'arrêt.
- **Déjà dans la cible à t0** (|e0| < seuil) : la tenue démarre à t0 ; si elle
  aboutit, temps = 0 min (« déjà à la cible »). Sinon règle générale.
- Le jour où la consigne change (utilisateur), l'indicateur du jour est marqué
  « consigne modifiée » et non comparable (drapeau), sans être effacé.

### Journalisation et mémoire
- `systemLogger.info` à t0 (« [KPI] pH : départ, écart +0,18 »), à l'atteinte
  (« [KPI] pH : cible atteinte en 285 min de filtration (écart initial +0,18) »)
  et à la non-atteinte (« [KPI] ORP : cible non atteinte, écart résiduel
  −22 mV à l'arrêt de la filtration »).
- Mémoire glissante **7 jours** par régulation : `{jour, e0, minutes, état,
  drapeau consigne}`, en NVS, écrite **au plus 2 fois par jour et par
  régulation** (à l'atteinte ou à la non-atteinte ; jamais en boucle).
- Survit au reboot : l'état « en cours » du jour (t0, e0, minutes cumulées,
  tenue en cours) est persisté en mémoire RTC (pattern 091) et recalé sur le
  compteur de filtration ; un POWERON en pleine journée reprend le chronomètre
  là où il était (pas de nouveau t0) si le jour civil est le même.

### Exposition (parité `/data` ↔ WS obligatoire)
- `ph_ttt_state` : 0 pas encore de départ aujourd'hui / 1 en cours / 2 atteinte
  / 3 non atteinte ; `ph_ttt_min` : minutes (état 1 : minutes écoulées ; état 2 :
  temps d'atteinte ; état 3 : durée totale de filtration) ; `ph_ttt_e0` : écart
  initial signé ; `ph_ttt_flag` : 0 / 1 consigne modifiée. Idem `orp_ttt_*`.
- `/get-kpi` (GET, authentifié) : les 7 derniers jours des deux régulations
  (tableau), pour le graphique et pour toute analyse externe.

### Affichage
- Pages pH et ORP : une ligne `.kv` « Cible atteinte » → « en 4 h 45 (écart
  initial +0,18) » / « en cours — 1 h 10, écart +0,07 » / « non atteinte hier
  (−22 mV à l'arrêt) » / « déjà à la cible ». Sous la ligne, sept pastilles
  (7 derniers jours) : durée abrégée, couleur neutre ≤ 2 h, avertissement
  au-delà, gris = non atteinte, hachuré = consigne modifiée. Choix final par
  `ux-ui-designer`.
- Tableau de bord : rien de plus (les cartes sont déjà denses) sauf avis
  contraire du designer.

### Cas limites
- Filtration en mode manuel ON toute la nuit : le jour civil change à minuit →
  nouveau t0 au premier instant valide après minuit (la porte est ouverte : t0 =
  00:00, e0 = erreur à minuit).
- Redémarrage avec heure invalide (pas de DS3231, pas de NTP) : indicateur
  suspendu (état 0, log warn une fois) jusqu'à heure valide ; pas de mémoire
  écrite avec une date fausse.
- Sonde HS / mesure NaN prolongée après t0 : le chronomètre de filtration
  continue, la tenue est remise à zéro ; si la journée finit ainsi → « non
  atteinte » avec écart résiduel « indisponible ».
- Purge de 091 déclenchée au plafond (mesures jamais stabilisées) : t0 est
  quand même posé à l'ouverture (la porte est ouverte) ; le drapeau
  « stabilisation non atteinte » du jour est mémorisé dans l'entrée KPI pour
  lecture honnête.
- Changement de consigne : drapeau posé, calcul poursuivi sur la nouvelle
  consigne.

## Comportement actuel
Rien n'existe. Le temps d'atteinte se lit à la main dans un CSV externe
(`builds/2.33.0/esp32dev_core3/ph_filtered_*.csv`) ou dans l'historique brut
5 min (qui n'est pas la valeur régulée : cf. feature-095).

## Contraintes techniques
- ESP32, offline first ; logique du chronomètre/tenue/décision dans un **module
  pur** (`time_to_target_logic.{h,cpp}`) testé en natif (ADR-0017), le firmware
  ne fait que l'alimenter (erreur, seuil, filtration ON, minute de filtration,
  jour civil, porte ouverte)
- NVS : ≤ 2 écritures/jour/régulation ; mémoire RTC pour l'état vivant
- `/data` et WS : +4 champs × 2 régulations ≈ 8 × ~22 o ≈ 180 o → décompte du
  budget WS (`ws_manager.cpp` ~306-313) avec commentaire daté ; grep bilatéral
- Aucune sortie de l'indicateur ne doit être lue par `pump_controller` (lecture
  seule, sens unique) — condition pool-chemistry
- Français dans toute l'UI

## Fichiers potentiellement concernés
- `src/time_to_target_logic.{h,cpp}` — [nouveau] module pur
- `src/time_to_target.{h,cpp}` — [nouveau] intégration : alimentation, RTC, NVS, logs
- `src/main.cpp` — tick dans loopTask (1 s)
- `src/pump_controller.h` — accesseurs en lecture (erreur courante, seuils) s'ils n'existent pas déjà
- `src/measure_validity.h` — état de la porte (déjà exposé par 091)
- `src/config.{h,cpp}` — namespace NVS `pool-kpi` (7 jours × 2)
- `src/web_routes_data.cpp`, `src/ws_manager.cpp` — 8 champs en parité
- `src/web_routes_*.cpp` — `GET /get-kpi`
- `data/index.html`, `data/app.js` — ligne + pastilles pages pH/ORP
- `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/subsystems/time-to-target.md` [nouveau], `docs/features/page-ph.md`, `page-orp.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Module pur : machine d'états (0-3), t0 unique par jour, tenue 30 min en minutes de circulation, non-atteinte à l'arrêt, drapeau consigne, cas « déjà à la cible »
- [x] A2 — Intégration : alimentation depuis la grandeur de décision/porte/circulation/compteur 093, tick 1 s, logs `[KPI]`
- [x] A3 — Persistance : état vivant en RTC (magic `KPIT` + FNV-1a), mémoire 7 jours en NVS `pool-kpi` ≤ 2 écritures/jour/régulation
- [x] A4 — `/data` + WS : 8 champs en parité, budget WS décompté (2 842 → 2 992 o, gabarit 2944 → 3072, marge 592 o)
- [x] A5 — `GET /get-kpi` authentifié (`RouteProtection::WRITE`, 503 explicite si mémoire indisponible)
- [x] B1 — Pages pH/ORP : ligne « Atteinte de la cible » + 7 pastilles, nourries par `latestSensorData` (/data ET WS) et `/get-kpi`
- [x] C1 — Docs : API, ws-manager, subsystem [nouveau], pages features, ADR-0063, CHANGELOG, guide utilisateur (PDF v2.37) — ⚠️ **capture de la page pH restant à refaire (étape humaine)**
- [x] D1 — Tests natifs `test_native_time_to_target` : rejeu 04/09, sortie de bande à 29 min (tenue remise, chrono continu), non-atteinte à l'arrêt, minuit en filtration manuelle, déjà à la cible, changement de consigne, mesure NaN ; **mutations tuées** : tenue 30 → 0, minutes murales au lieu de minutes de circulation, t0 re-posé à chaque re-purge, + les 3 gardes de `tttHistoryCloseOrphans`

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : `GET /get-kpi` (auth)
- Champs `/data` + WS nouveaux : `ph_ttt_state`, `ph_ttt_min`, `ph_ttt_e0`, `ph_ttt_flag`, `orp_ttt_state`, `orp_ttt_min`, `orp_ttt_e0`, `orp_ttt_flag`
- Topics MQTT : aucun dans cette version (option HA à évaluer plus tard, coût discovery — cf. bug-080 lot B : rafales discovery)
- Constantes : `kTimeToTargetHoldMin = 30`, `kTimeToTargetHistoryDays = 7`, `kTimeToTargetFiltrationOffDayEndMin = 60`

## Sécurité chimique
- Dosage impliqué : non (lecture seule)
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : non — l'indicateur lit `erreur` et `startThreshold`, n'écrit rien
- Risque identifié : tentation future de « fermer la boucle » (adapter kp au KPI) → interdit sans spec + pool-chemistry ; l'indicateur est un instrument de mesure, pas un régulateur

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 2
- **Sujet pressenti** : « définition normative du temps d'atteinte de cible : minutes de filtration depuis la première mesure valide du jour, tenue 30 min, un départ par jour » — c'est la métrique de référence de toutes les comparaisons de versions ; la figer évite de la redéfinir à chaque analyse

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `GET /get-kpi` (nouveau) ; `GET /data` (8 champs)
### Champs WebSocket
- les 8 champs ci-dessus
### Topics MQTT
- aucun
### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-ph.md`, `page-orp.md` — ligne + pastilles
- `docs/subsystems/time-to-target.md` [nouveau], `docs/subsystems/ws-manager.md`
#### Docs transverses
- `docs/API.md`, `CHANGELOG.md` (2.35.0), `docs/user/src/guide-regulation.html` (comment lire l'indicateur) → PDF
#### Docs immuables
- `docs/adr/00NN-definition-temps-d-atteinte-de-cible.md`

## Critères d'acceptation
- [x] Rejeu natif du 04/09 → **223 min** sur la **grandeur de décision** (233 min si t0 part du brut — l'écart de 10 min EST `kPhDecisionWindowMs`, cf. ADR-0063), 303 ± 5 min (ORP), e0 = +0,18 / −45 mV
- [x] Un reboot logiciel en cours de journée ne crée pas de second t0 et conserve les minutes cumulées
- [x] Fin de filtration sans tenue → état 3 dans les 60 s, une écriture NVS, un log
- [x] `/data` et WS portent les 8 champs identiques au même instant (grep bilatéral)
- [x] `/get-kpi` renvoie 7 jours max, JSON valide, non bloquant (miroir RAM sous portMUX, aucune lecture NVS)
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutations D1 tuées
- [x] `./build_fs.sh` passe sans dépasser la partition

## Plan de test manuel
1. Après OTA en cours de journée : état 1 avec minutes écoulées cohérentes (t0 = ouverture de la porte de ce matin, si l'heure est connue via le compteur 093 ; sinon t0 = instant de l'OTA, documenté dans UPDATE_GUIDE).
2. Lendemain matin : log `[KPI] départ` à l'ouverture de la porte ; ligne « en cours » sur la page pH ; atteinte dans la matinée → log + pastille.
3. Changer la consigne pH de 0,05 en cours de journée : drapeau visible, chronomètre poursuivi.

## Notes pour agent
- Le **compteur monotone de minutes de filtration** introduit par feature-093 (v2.34) est la base de temps : ne pas recréer un compteur. Localisation exacte : `SafetyLimits::filtTotalMin` (`src/config.h:230`), persisté dans le namespace NVS `pool-daily` (clé `filt_total`), jamais remis à zéro au basculement de date.
- **Fixtures de test = journée du 04/09 uniquement** (t0 07:47, pH atteinte ~11:40 → 233 min, e0 = +0,18 ; ORP 12:50 → 303 min, e0 = −45 mV). Le matin du **05/09 est CONTAMINÉ** (air dans le circuit, niveau d'eau bas — condition nouvelle, absente les jours précédents) : illustration seulement, **jamais** référence de dimensionnement ni fixture.
- La **porte de mesure** de feature-091 (v2.34) donne t0 : ne pas réinventer un critère de « première mesure ».
- Parité `/data` ↔ WS : 3 récidives documentées + bug-089 dans le même lot ; prouver par grep des deux côtés.
- ~~Ordre du lot v2.35 : 095 → 096 → bug-089 → 094~~ **PÉRIMÉ (05/09)** : feature-094
  est LIVRÉE (commit `ee227af`, v2.36.0, non déployée). Ordre imposé désormais :
  **096 → 095 → bug-089**, puis un **déploiement groupé unique** (094+096+095+089).
  096 passe en premier précisément pour instrumenter le déploiement de 094.

## Hors périmètre
- Toute action automatique fondée sur l'indicateur (adaptation des gains, alertes de dérive) — observation seulement dans cette version.
- Capteurs Home Assistant.

---

## Notes d'implémentation (05/09/2026 — v2.37.0, livré non déployé)

**Décisions tranchées par le gate `pool-chemistry`**, toutes formalisées dans
[ADR-0063](../../../docs/adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md) :

1. **On juge sur la grandeur de DÉCISION**, pas sur la filtrée — règle uniforme,
   réalisation asymétrique (signal lent 10 min pour le pH, filtrée pour l'ORP,
   qui **EST** sa grandeur de décision). ⚠️ **Le zéro du chronomètre pH est donc
   indexé sur `kPhDecisionWindowMs`** : toute modification future de cette
   constante crée une **discontinuité de série**, rendue lisible par
   l'estampille `serieStamp` de l'anneau NVS + un warning au démarrage.
2. **« Atteint » = « la régulation ne redémarrerait pas »** : bande = seuils de
   démarrage **vivants** (0,05 pH / 15 mV), tenue **30 min** de circulation.
3. **Base de temps `filtTotalMin` réutilisée telle quelle**, en lecture seule —
   ce sont des minutes de **présence d'eau**. **Libellé « minutes de
   circulation » partout, jamais « de filtration ».**
4. **Un boost chlore ne lève PAS le bit 0** : on **juge** contre la consigne
   **effective**, on **surveille** la **configurée**. Le boost est en bit 1.
5. **Règle de partage du bit 4** : *ce que le régulateur CHOISIT compte ; ce
   qu'on lui IMPOSE de l'extérieur porte le bit.* Quota, limite horaire,
   actionneur sous-dimensionné ⇒ **aucun bit** (vraie non-atteinte, c'est ce que
   la métrique doit voir). Manuel, maintenance, assistant de débit ⇒ **bit 4**,
   journées à **exclure des moyennes de comparaison de versions**.
6. **Garde de vraisemblance** 1,0 pH / 300 mV : elle **refuse seulement de
   démarrer**, n'invente jamais de valeur (cas réel du 05/09 : air dans le
   circuit, brut à 5,45, e0 = −1,75).
7. **NVS `pool-kpi` disjoint de `pool-daily`** — argument : le **droit de
   destruction en masse** (le namespace est l'unité de `clear()`), pas l'espace
   ni la corruption de page. Moindre privilège.
8. **Persistance à deux étages** ⇒ exactement **2 écritures/jour/régulation**.
9. **Sens unique** garanti par un **test de grep bidirectionnel** (le `#error`
   de compilation a été écarté : il aurait cassé le build du module lui-même).

**Chiffres du lot**

| Mesure | Valeur |
|---|---|
| Tests natifs | **31 suites / 1 210 cas**, zéro régression (suite `time_to_target` : 56 cas) |
| Mutations | tenue 30 → 0, minutes murales, t0 re-posé à chaque re-purge, 3 gardes de `tttHistoryCloseOrphans` — **toutes tuées** |
| Flash | **1 819 517 / 1 835 008 o** |
| Marge flash | **15 491 o** |
| Trame WS `sensor_data` (pire cas) | 2 842 → **2 992 o** (gabarit 2944 → 3072, marge sous `kWsFrameBufBytes` : **592 o**) |

**Fichiers livrés**

- `src/time_to_target_logic.{h,cpp}` [nouveaux] — logique pure (ADR-0017)
- `src/time_to_target.{h,cpp}` [nouveaux] — coquille : mesures, heure, NVS, RTC, journal
- `src/constants.h` — `kTimeToTargetHoldMin`, `kTimeToTargetHistoryDays`, `kTimeToTargetOffDayEndMin`, `kTttMaxPlausibleE0Ph/Orp` + `static_assert`
- `src/config.{h,cpp}` — `loadTttHistory()` / `saveTttHistory()`, namespace `pool-kpi`
- `src/main.cpp` — `timeToTarget.begin()` (setup) / `.tick()` (loop, avant `PumpController.update()`)
- `src/web_routes_data.cpp` — 8 champs `/data` + `GET /get-kpi`
- `src/ws_manager.cpp` — 8 champs `sensor_data`, gabarit 2944 → 3072
- `data/index.html`, `data/app.js`, `data/app.css` — blocs `#ph-ttt-block` / `#orp-ttt-block`
- `test/test_native_time_to_target/test_time_to_target.cpp` [nouveau]

**Documentation produite** : `docs/adr/0063-*` [nouveau],
`docs/subsystems/time-to-target.md` [nouveau], `docs/API.md`,
`docs/subsystems/ws-manager.md`, `docs/features/page-ph.md`,
`docs/features/page-orp.md`, `CHANGELOG.md` (2.37.0),
`docs/user/src/guide-regulation.html` + PDF v2.37 (PDF v2.36 supprimés),
index `docs/README.md`, `docs/adr/README.md`, `docs/subsystems/README.md`.

**Reste à faire (étape humaine)** : refaire la **capture d'écran de la page pH**
(`screenshots/`) — elle ne montre pas encore le bloc « Atteinte de la cible ».

**Déploiement** : groupé avec **094 + 095 + bug-089**. 096 passe en premier
précisément pour **instrumenter** le déploiement de 094.
