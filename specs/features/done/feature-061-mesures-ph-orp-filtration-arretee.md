# Feature — Filtration à l'arrêt : mesures pH/ORP non affichées et exclues de l'historique

## Métadonnées
- **ID** : feature-061
- **Slug** : mesures-ph-orp-filtration-arretee
- **Type** : feature
- **Statut** : done (itérations 1 et 2 livrées)
- **Priorité** : normale
- **Version cible** : 2.13.0
- **Dernière mise à jour** : 2026-07-29
- **Spec(s) liée(s)** : feature-056 (présence d'eau — `resolveWaterPresent()`, source unique), bug-ui-valeurs-figees-capteur-absent (convention `--` + badges, v2.10.5), feature-007 (fraîcheur capteurs)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — sémantique de VALIDITÉ d'affichage uniquement (aucun changement de lecture/filtrage)
- [ ] Régulation (pump_controller, PID) — non touchée (la garde « présence d'eau » refuse déjà le dosage filtration arrêtée)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (selon arbitrage architect : où se décide la non-représentativité)
- [ ] MQTT / Home Assistant — hors périmètre (voir section dédiée)
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non requis a priori (aucun chemin de dosage/régulation/calibration modifié — le dosage refuse DÉJÀ sans eau, garde 2) ; à réévaluer par l'architect si l'implémentation venait à toucher sensors/régulation
- [x] ux-ui-designer — **requis** : arbitrage de la représentation « mesure présente mais non représentative » (masquer ? `--` ? valeur grisée + badge ?), distincte de « capteur indisponible » (v2.10.5)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Quand la filtration ne tourne pas (pas de circulation d'eau dans la chambre de mesure), les valeurs pH et ORP lues par les sondes ne représentent pas l'eau du bassin : ne plus les afficher comme des mesures valides (tableau de bord + pages dédiées pH/ORP) et ne plus les enregistrer dans l'historique (courbes dashboard + page historique).

## Contexte
Les sondes pH/ORP sont montées dans le circuit de filtration. Filtration arrêtée = eau stagnante autour des sondes : la valeur dérive localement (consommation du désinfectant dans le volume mort, température, absence de brassage) et ne reflète plus le bassin. Aujourd'hui l'UI affiche ces valeurs comme normales et l'historique les enregistre — polluant les courbes avec des plateaux/dérives nocturnes non représentatifs. Le dosage, lui, est déjà protégé (garde « présence d'eau » = filtration active, feature-056).

## Comportement attendu
1. **Source de vérité** : la « circulation d'eau » est déterminée par la source unique existante `resolveWaterPresent()` (feature-056 — couvre la filtration pilotée ET le mode signal externe). Aucune nouvelle heuristique.
2. **Affichage (dashboard, widgets compacts, pages pH et ORP)** : filtration arrêtée → les valeurs pH/ORP ne sont plus présentées comme des mesures courantes valides. Représentation exacte : arbitrage `ux-ui-designer` (options : `--` + badge « Filtration arrêtée » ; valeur grisée horodatée « dernière mesure valide » ; masquage). Elle doit être **distincte** du badge « Capteur indisponible » (qui signifie sonde absente/HS).
3. **Historique** : filtration arrêtée → les points pH/ORP ne sont plus enregistrés (null dans le point, comme un capteur absent) ; la **température et l'état filtration restent enregistrés** (la température dans la canalisation reste une donnée, et l'état filtration est nécessaire aux courbes). Les courbes affichent des trous sur les périodes d'arrêt (comportement existant pour les null).
4. **Reprise** : au redémarrage de la filtration, les mesures redeviennent valides. Cas limite à arbitrer (architect, avec bon sens chimique) : immédiatement, ou après un court délai de purge/renouvellement de la chambre de mesure (l'eau stagnante doit être chassée — un délai type `stabilizationDelayMin` existe déjà pour le dosage) ; si un délai est retenu, l'UI l'indique (« stabilisation... »).
5. **La chaîne de mesure interne ne change PAS** : les sondes continuent d'être lues, filtrées, publiées en interne (le gel de variance, la calibration, les diagnostics de l'onglet Avancé continuent de fonctionner). Seules la **présentation** et la **persistance historique** changent.

### Cas limites
- Mode installation « filtration externe » : `resolveWaterPresent()` fait foi (signal frais requis) — même comportement.
- Boot avec filtration arrêtée : pas de valeurs pH/ORP affichées ni enregistrées d'emblée.
- Filtration en marche forcée/boost : eau circule → mesures valides (rien de spécial).
- Page calibration : NON concernée (on calibre sonde sortie de l'eau/dans les solutions — les lectures live doivent rester visibles). Idem onglet Avancé (diagnostic brut/filtré).
- WS/`/data` : selon l'arbitrage architect (null côté firmware vs décision côté UI avec l'état filtration déjà présent dans le payload), la compatibilité HA/MQTT ne doit PAS changer (hors périmètre).

## Comportement actuel
Valeurs pH/ORP affichées et historisées en continu, filtration en marche ou non. Le dosage est déjà bloqué sans eau (garde 2).

## Contraintes techniques
- ESP32, offline first, vanilla JS, français.
- Réutiliser `resolveWaterPresent()` (source unique feature-056) — ne pas dupliquer l'heuristique de présence d'eau.
- L'enregistrement historique se fait dans `history.update()` (points 5 min) : l'exclusion doit s'appliquer au point enregistré, pas en post-traitement.
- Ne PAS toucher : chaîne de lecture/filtrage sensors, MQTT/HA, calibration, page Avancé, dosage.
- Convention d'affichage : cohérente avec la v2.10.5 (`--`, badges) mais état distinct.

## Fichiers potentiellement concernés
- `src/history.cpp` — exclusion pH/ORP du point quand pas d'eau
- `src/ws_manager.cpp` / `src/web_routes_data.cpp` — selon arbitrage (exposer l'état « mesure non représentative » ou laisser l'UI décider)
- `data/app.js` / `data/index.html` — représentation UI (dashboard, widgets, pages pH/ORP, badges)
- `test/test_native_history/` — exclusion dans la logique d'agrégation si applicable
- `docs/subsystems/history.md`, `docs/features/page-dashboard.md` + pages pH/ORP, `docs/API.md` si contrat modifié, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Exclusion des points pH/ORP de l'historique quand `resolveWaterPresent()` est faux (température + état filtration conservés)
- [x] A2 — Exposition de l'état « eau ne circule pas » au frontend si l'arbitrage retient la décision côté firmware (sinon : justification consignée) — décision côté firmware : champs `water_present` / `measures_valid` / `measures_stab_remaining_s` en WS **et** `/data`
- [x] B1 — Représentation UI arbitrée par ux-ui-designer, appliquée sur : dashboard (cartes + widgets compacts), page pH, page ORP — distincte de « Capteur indisponible » (badge bleu `info` vs rouge `danger`)
- [x] B2 — Cas « reprise » (délai de purge éventuel) implémenté selon arbitrage — purge de 5 min (`stabilizationDelayMin`), badge « Stabilisation : ~N min »
- [x] D1 — Tests natifs de la logique d'exclusion (si extraite en pur) ; non-régression suites existantes — 14 tests `test_native_measure_validity`, 388/388 verts
- [x] D2 — `pio run` 0 warning nouveau, tests verts, `./build_fs.sh` OK
- [x] C1 — Docs (history.md, measure-validity.md nouveau, pages dashboard/pH/ORP, API.md, ws-manager.md) + `CHANGELOG.md` [2.13.0]

## Nouvelles signatures / API (optionnelle)
- Éventuel champ WS/`/data` type `water_circulating` (bool) — arbitrage architect ; AUCUN topic MQTT nouveau

## Sécurité chimique (optionnelle)
- Dosage impliqué : non (garde présence d'eau existante inchangée). Si l'architect détecte le moindre impact sur un chemin de dosage → pool-chemistry obligatoire avant implémentation.

## ADR attendu ?
- **Probable** : non — politique de présentation/persistance, pas de contrainte structurante. L'architect confirme.

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `/get-history` : points avec pH/ORP null pendant les arrêts (contrat existant, null déjà géré)
- `/data` / WS : éventuel champ nouveau (arbitrage)
### Documentation à produire / mettre à jour
- `docs/subsystems/history.md`, `docs/features/page-dashboard.md` + pages pH/ORP, `docs/API.md` si champ nouveau, `CHANGELOG.md` [2.13.0]

## Critères d'acceptation
- [x] Filtration arrêtée → aucune valeur pH/ORP présentée comme mesure courante sur dashboard/pages dédiées (représentation arbitrée), température toujours affichée.
- [x] Les points d'historique enregistrés pendant l'arrêt ont pH/ORP null ; les courbes montrent des trous ; température/filtration continuent d'être tracées.
- [x] Reprise de la filtration → mesures de nouveau affichées et historisées (avec le délai de purge si arbitré).
- [x] Calibration et onglet Avancé : lectures live toujours visibles (non-régression) — champs live jamais gatés côté firmware, gates UI limités à l'affichage « mesure courante ».
- [x] MQTT/HA inchangé.
- [x] `pio run` + tests + `./build_fs.sh` verts ; libellés français.

## Plan de test manuel
1. Filtration en marche : valeurs normales partout, points historisés.
2. Arrêter la filtration (mode manuel off) : dashboard et pages pH/ORP basculent sur la représentation « filtration arrêtée » en ≤ 1 rafraîchissement ; température toujours là ; page calibration affiche toujours le live.
3. Laisser 15 min : vérifier le trou dans les courbes (dashboard + page historique).
4. Relancer la filtration : retour des mesures (après le délai de purge éventuel), reprise de l'historisation.

## Hors périmètre
- MQTT/Home Assistant (les capteurs HA continuent de recevoir les valeurs — à traiter dans une éventuelle spec ultérieure si souhaité, avec réflexion sur availability/state).
- L'écran UART LVGL (même réflexion à mener côté firmware écran — spec séparée).
- Tout changement de la chaîne de mesure, du gel de variance, de la calibration, du dosage.

## Notes d'implémentation (v2.13.0, 2026-07-29)

**Statut** : livré — tests verts (388/388, dont 14 nouveaux `test_native_measure_validity`), revue **Approved**, `src/version.h` = 2.13.0.

### Fichiers

- **Nouveaux** : `src/measure_validity_logic.{h,cpp}` (logique pure, ADR-0017), `src/measure_validity.{h,cpp}` (coquille + cache portMUX), `test/test_native_measure_validity/test_measure_validity.cpp` (14 tests).
- **Modifiés firmware** : `src/history.cpp` (NaN pH/ORP dans `recordDataPoint()` si invalide), `src/main.cpp` (`measureValidity.update()` avant `history.update()`), `src/ws_manager.cpp` (+2 champs, buffer 2048 → 2112), `src/web_routes_data.cpp` (+3 champs, buffer 1152 → 1280), `src/version.h`, `platformio.ini` (build_src_filter natif).
- **Modifiés frontend** : `data/app.js` (`measuresAreValid()`, `measureStateSegments()`, `dedupDosingReason()`, nulls poussés dans les séries uPlot, gates badges/valeurs), `data/index.html` (`#ph-page-badge`, `#orp-page-badge`), `data/app.css` (`.sensor-badge--info`).
- **Docs** : `docs/subsystems/measure-validity.md` (nouveau), `history.md`, `ws-manager.md`, `docs/API.md`, `docs/features/page-dashboard.md` / `page-ph.md` / `page-orp.md`, `CHANGELOG.md` [2.13.0].

### Arbitrages

1. **Décision côté firmware, source unique** : nouvelle coquille `measure_validity` (module pur `measure_validity_logic` + coquille impure) branchée sur `resolveWaterPresence()` (feature-056). Cache sous spinlock portMUX, `update()` en loop **avant** `history.update()` ; les consommateurs (history, WS, `/data`) lisent le cache, jamais la résolution directement (contexte async_tcp interdit).
2. **Champs exposés WS + `/data`** : `water_present` (déjà en WS depuis feature-056, **ajouté à `/data`**), `measures_valid` (bool), `measures_stab_remaining_s` (uint). **Écart de nommage vs spec** : le champ générique `water_circulating` envisagé n'a pas été créé (le `water_present` existant fait foi) ; `measures_stab_remaining_s` choisi pour éviter la **collision** avec `stabilization_remaining_s` (stabilisation post-calibration des pompes, feature-006).
3. **Valeurs live conservées dans les payloads** : `ph`/`orp`/`phRaw`/… restent publiés même invalides (calibration + onglet Avancé intacts) — seuls l'**affichage** (via `measures_valid`) et l'**historique** (NaN → `null`) changent.
4. **Délai de purge à la reprise** : réutilise `stabilizationDelayMin` (défaut 5 min), 0 = désactivé. **Boot avec eau présente = délai complet** (conservateur). Arrondi seconde sup côté firmware, minute sup côté UI.
5. **UI** : badges composites — « Filtration arrêtée » (nouvelle variante **info bleue** `.sensor-badge--info`, état normal ≠ anomalie rouge), « Signal filtration périmé » (warning), « Stabilisation : ~N min » (info, recalcul à chaque push WS, aucun tick JS) ; badges de page `#ph-page-badge`/`#orp-page-badge` ; valeurs « `--` » ; dé-duplication avec le `dosing-status` ; garde anti-faux-positif (« Capteur indisponible » et « hors plage » gatés par `measures_valid`) ; charts : **nulls poussés** dans les séries (lever de crayon uPlot = trous), y compris chemin incrémental.
6. **Pas d'ADR** (confirmé architect) : politique de présentation/persistance sans contrainte structurante — la source unique est actée par l'ADR-0026, le pattern pur/coquille par l'ADR-0017.

### Mineures actées en revue (non bloquantes)

- `/data` n'expose pas `filtration_state_stale` → le repli HTTP dégrade « Signal filtration périmé » en « Filtration arrêtée » (asymétrie **préexistante** WS vs `/data`, à harmoniser un jour).
- Skew d'un tour de loop possible entre un `water_present` frais et le `measures_valid` du cache (conservateur, sans conséquence).
- **Test de caractérisation « consolidation des NaN » non ajouté** : l'accumulation des sommes/compteurs vit dans la coquille impure `consolidateData()` (hors périmètre natif, contrainte libc++ — cf. history.md) ; `finalizeMean(sum, 0) → NAN` est déjà couvert par les tests existants de `history_logic` ; le chemin a été vérifié par lecture (architect).

## Itération 2 — extension à la température (2026-07-30, demande utilisateur)

**Demande** : appliquer le même traitement à la température (affichage + historique) quand l'eau ne circule pas.

**Justification physique** : sonde DS18B20 dans la canalisation → à l'arrêt, l'eau du tuyau dérive vers la température ambiante du local (plus lentement que le pH/ORP, mais les plateaux nocturnes polluent réellement les courbes — c'est la température du local qui est enregistrée, pas celle du bassin).

**Vérification fonctionnelle préalable (faite)** : le mode Auto de la filtration lit `sensors.getTemperature()` EN DIRECT (filtration.cpp:86) — la chaîne interne reste intacte par construction du pattern 061, aucun impact sur le calcul de durée. MQTT/UART écran lisent aussi le live (hors périmètre, inchangés).

**Décisions d'itération** (dans la continuité des arbitrages 061, pas de nouvel arbitrage) :
- Même drapeau `measures_valid` (pas de validité spécifique température : le délai de purge de 5 min est conservateur pour la température — l'eau du tuyau se renouvelle en < 1 min de circulation — mais une validité séparée ajouterait de la complexité pour un gain nul).
- Historique : `point.temperature = NAN` quand invalide (comme pH/ORP) ; l'état filtration et les flags dosage restent.
- UI : carte température du dashboard + compact → `--` + mêmes segments de badge (« Filtration arrêtée » info / « Signal filtration périmé » warning / « Stabilisation » info) via `#temperature-sensor-badge` (existant, v2.10.5) — le badge rouge « Capteur indisponible » température garde sa garde anti-faux-positif comme pH/ORP.
- Charts : étendre le push de null + lever de crayon à la série température (mini-charts + page historique), qui en était exclue en itération 1.

### Livrables itération 2
- [x] I2-A1 — history.cpp : temperature → NAN quand `!measuresValid()`
- [x] I2-B1 — UI : `--` + segments badge sur carte/compact température ; garde anti-faux-positif ; charts température avec trous
- [x] I2-D1 — validation complète (pio run, tests natifs, build_fs)
- [x] I2-C1 — docs (history.md, measure-validity.md, page-dashboard.md, CHANGELOG 2.13.0 amendé)

### Notes d'itération 2

**Statut** : livré — revue **Approved**, validation complète (pio run OK, tests natifs 388/388 verts, `./build_fs.sh` OK). v2.13.0 non déployée au moment de la clôture.

- Fichiers touchés : `src/history.cpp` + `data/app.js` **seuls** (pas de changement de `measure_validity` — le drapeau existant couvre la température ; aucun champ WS/`/data` nouveau).
- Revue : 1 mineure **transitoire** acceptée (affichage calibration) — non bloquante.
- Docs amendées (pas de nouvelle entrée CHANGELOG) : `history.md`, `measure-validity.md`, `page-dashboard.md`, `CHANGELOG.md` [2.13.0].
