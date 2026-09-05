## Métadonnées
- **ID** : feature-011
- **Slug** : repartition-24h-programmee
- **Type** : feature
- **Statut** : done
- **Priorité** : haute
- **Version cible** : 2.8.0
- **Dernière mise à jour** : 2026-07-05 (**implémenté en v2.8.0** — voir « Notes d'implémentation » en fin de fichier ; RÉVISION 2 ci-dessous ; la spec d'origine d'avril suit, à lire À TRAVERS la révision)

## RÉVISION 2 (2026-07-05) — mise à niveau contre le code v2.7.0

La spec d'avril reste valable sur l'intention (répartir le volume quotidien du mode `scheduled` au lieu d'injecter d'un bloc — comportement actuel confirmé : `pump_controller.cpp` l.860-932 injecte en continu au débit max jusqu'au quota). Les points suivants **remplacent** ou précisent la spec d'origine :

### R1 — Une seule plage de filtration (simplification majeure)
`FiltrationConfig` = une plage quotidienne unique `start`/`end` (« HH:MM », à cheval sur minuit possible, mode auto = plage calculée par température). **Tous les cas « plages discontinues » de la spec d'origine sont SANS OBJET.**
- `remaining_filtration_minutes` = minutes restantes de la plage courante **bornées à minuit** (les compteurs journaliers se réinitialisent à minuit : l'horizon de répartition ne le franchit jamais).
- Helper à créer en **logique pure** dans `schedule_logic` (qui fournit déjà `timeStringToMinutes`/`isMinutesInRange`, feature-038) : `int remainingRangeMinutes(int nowMin, int startMin, int endMin)` — 0 si hors plage ou plage invalide.

### R2 — Algorithme en logique pure (ADR-0017, remplace `PumpController::computeScheduledFlow()`)
La décision de répartition vit dans `dosing_logic` (fonctions pures, testables en natif), la coquille `pump_controller.cpp` ne fait que collecter les entrées et appliquer la sortie. **Recalcul à CHAQUE fenêtre** depuis l'état courant (auto-correcteur) :
- `remaining = effectiveDailyMl − dailyInjectedMl` (le plafonnement par `maxPhMlPerDay` existant est conservé).
- volume de fenêtre = `remaining / fenêtres_restantes` (fenêtres de `kScheduledWindowMs = 15 min` dans `remaining_filtration_minutes`).
- Si la durée d'injection correspondante < `minInjectionTimeMs` (30 s) → **report** : rien dans cette fenêtre, le volume se reporte mécaniquement (remaining inchangé, moins de fenêtres restantes).
- Le recalcul par fenêtre couvre AUTOMATIQUEMENT : changement de cible en cours de journée, injections manuelles (feature-006 : comptées dans `dailyInjectedMl`), retard subi (limite horaire), redémarrage ESP32.

### R3 — Intégration ADR-0020 (budget horaire partagé) — remplace « ph_limit_minutes »
Vrais noms : `phInjectionLimitMinutes` (5 min/h) / `orpInjectionLimitMinutes` (10 min/h), budget temps `usedMs` **partagé** auto+manuel+test+UART. La garde d'entrée `phLimitOk` existante reste ; en plus, le volume de fenêtre est borné par le budget horaire restant (converti en mL via le débit effectif) — l'excédent se reporte aux fenêtres suivantes.

### R4 — Arbitrage `maxCyclesPerDay` (tension non vue par la spec d'origine)
1 injection/fenêtre 15 min sur une plage de 8-12 h = 30-45 démarrages/jour > `maxCyclesPerDay = 20`. La branche scheduled n'incrémente **volontairement pas** `cyclesToday` aujourd'hui (l.908-910). **Proposition à valider par pool-chemistry** : conserver cette exemption (le cadencement structurel ≤ 4 démarrages/h + ring anti-rafale partagé bornent déjà les démarrages) ; l'ADR (C4) documente l'arbitrage et les alternatives (fenêtre 30 min, limite dédiée). A4 de la spec d'origine est amendé en conséquence : « respect de maxCyclesPerDay » → « démarrages bornés structurellement par le cadencement, exemption cyclesToday conservée et documentée ».

### R5 — Livrables amendés
- A2 → fonctions pures dans `dosing_logic` (+ helper `schedule_logic`), PAS de méthode PumpController.
- A7 → `remainingRangeMinutes` dans `schedule_logic` (pure), la coquille filtration n'est PAS modifiée.
- D2 → tests natifs obligatoires (dossier `test/test_native_dosing` ou dédié) : nominal, report < 30 s, borne horaire, changement de cible à midi, injection manuelle en cours de journée, plage à cheval sur minuit, hors plage, RTC invalide (heure indisponible → aucune injection scheduled, comportement actuel conservé).
- B2 (toast reliquat) → réalisable côté client avec les données déjà exposées (cible dans la config, injecté dans le WS) ; à défaut simple hint statique — arbitrage web-ui-developer, pas de nouveau champ WS dédié au reliquat.
- Champs WS `ph/orp_scheduled_flow_ml_per_min` (A8) conservés : débit moyen planifié restant, `null` hors mode scheduled ou hors plage.
- Version cible : **2.8.0**.
- **Doc(s) liée(s)** : [docs/features/page-ph.md](../../docs/features/page-ph.md#règles-firmware-appliquées), [docs/features/page-orp.md](../../docs/features/page-orp.md#règles-firmware-appliquées), [docs/adr/0002-mode-programmee-volume-quotidien.md](../../docs/adr/0002-mode-programmee-volume-quotidien.md), [docs/subsystems/pump-controller.md](../../docs/subsystems/pump-controller.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage (interaction plages)
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
- [x] pool-chemistry — **obligatoire** (algorithme de répartition en contact direct avec la chimie)
- [ ] ux-ui-designer — non (info supplémentaire dans une carte existante, pas de refonte)
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Définir et implémenter un algorithme de **répartition** du volume quotidien programmé (`ph_daily_target_ml`, `orp_daily_target_ml`) sur les plages de filtration de la journée, en respectant les limites horaires, `maxCyclesPerDay`, `minInjectionTimeMs` et le délai de stabilisation.

## Contexte
Le mode `scheduled` (voir [ADR-0002](../../docs/adr/0002-mode-programmee-volume-quotidien.md)) permet à l'utilisateur de saisir un volume quotidien à injecter, indépendant de la mesure capteur. Le comportement actuel est grossier : injecter tant qu'on n'a pas atteint la cible **ou** la limite horaire, sans lisser sur la journée.

Problèmes observés :
- L'intégralité du volume peut être injectée dans la première plage de filtration du matin → pic de concentration localisé.
- Si la filtration démarre tard (ex. 14h) et que les plages sont courtes, la limite horaire bloque avant que le quota soit atteint → l'utilisateur ne sait pas pourquoi.
- Aucun lissage avec les fluctuations de pH naturelles (retour de baigneurs, UV, etc.).

## Comportement attendu

### Côté firmware
- Au début de chaque plage de filtration (ou au passage de minuit), calculer :
  - Volume restant à injecter : `remaining = target - daily_injected`
  - Durée de filtration restante dans la journée : `remaining_filtration_minutes` (somme des plages futures planifiées + fin de la plage courante)
  - Débit cible : `flow = remaining / remaining_filtration_minutes` (mL/min), borné par `kPumpMaxFlowMlPerMin`
- Répartir **uniformément** par fenêtres de 15 min : chaque 15 min pendant une plage de filtration, vérifier si `volume_injecté_dans_la_fenêtre < flow × 15`, si oui déclencher une injection courte (≥ `minInjectionTimeMs`) du volume manquant.
- Respecter strictement `ph_limit_minutes` / `orp_limit_minutes` (horaire) et `maxCyclesPerDay`.
- Respecter `minInjectionTimeMs = 30 s` : si le volume cumulé à injecter dans la fenêtre est < ce qui produit une injection ≥ 30 s, **reporter** à la fenêtre suivante (évite le short-cycling).
- Si la journée se termine avec `remaining > 0` (retard cumulé, limite horaire), **ne pas** basculer automatiquement au lendemain : le lendemain recommence à zéro, `remaining` est perdu (et loggué `info`).
- Exposer un nouveau champ WS `ph_scheduled_flow_ml_per_min` / `orp_scheduled_flow_ml_per_min` pour diagnostic UI.

### Côté UI (optionnel mais recommandé)
- Dans la carte « Régulation » mode Programmée, afficher sous le volume cible :
  - « Débit calculé : X mL/min réparti sur Y h restantes »
  - Si `remaining > 0` et `remaining_filtration_minutes == 0` : toast warning « Quota non atteint, reliquat perdu au passage de minuit ».

### Cas limites
- Plage unique 10h-14h : 4 h de filtration ; répartir sur 16 fenêtres de 15 min.
- Filtration démarre tard (ex. 18h pour le lendemain 0h) : 6 h, compresser le débit sans dépasser les limites horaires.
- Plages discontinues (10h-12h + 18h-20h) : calculer sur la somme = 4 h.
- Filtration OFF prolongée (panne pompe) : pas d'injection, `remaining` non diminué, warning si fin de journée.
- Mode `scheduled` → passage à `automatic` en milieu de journée : le volume déjà injecté est mémorisé dans `ph_daily_ml` (pas de réinitialisation), le PID reprend la main.
- Changement du volume cible en cours de journée : recalculer le débit sur le reste de la journée avec le nouveau target.

## Comportement actuel
Le mode `scheduled` injecte dès qu'il peut, sans répartition. Voir [docs/subsystems/pump-controller.md#mode-scheduled](../../docs/subsystems/pump-controller.md#mode-scheduled).

## Contraintes techniques
- ESP32, offline first
- Le calcul de répartition doit être simple (pas de PID imbriqué)
- Interaction avec `filtration` : utiliser l'API existante pour lister les plages à venir
- L'horloge locale (NTP/RTC) est requise ; si invalide, le mode scheduled attend comme aujourd'hui
- `pool-chemistry` doit valider l'algorithme (notamment les limites, l'absence de short-cycling, le comportement en fin de journée)

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` / `.h` — branche mode `scheduled`, nouvelle méthode `computeScheduledFlow()`
- `src/filtration.h` / `.cpp` — helper `uint32_t filtration::remainingMinutesInDay(time_t now)` si pas déjà existant
- `src/constants.h` — `constexpr uint32_t kScheduledWindowMs = 15 * 60 * 1000;`
- `src/ws_manager.cpp` — nouveaux champs WS `ph_scheduled_flow_ml_per_min`, `orp_scheduled_flow_ml_per_min`
- `data/index.html` — affichage du débit calculé dans la carte Régulation
- `data/app.js` — consommation WS
- `docs/features/page-ph.md`, `page-orp.md` — enrichir le mode Programmée
- `docs/subsystems/pump-controller.md` — §Mode scheduled — nouvel algorithme
- `docs/subsystems/filtration.md` — helper ajouté si créé
- `docs/adr/NNNN-repartition-scheduled.md` [nouveau] — ADR décisionnelle

## Livrables — check-list
- [x] A1 — `kScheduledWindowMs = 15 * 60 * 1000` dans `constants.h` *(livré sous la forme `kScheduledWindowMinutes = 15` — voir Notes d'implémentation)*
- [x] A2 — `float PumpController::computeScheduledFlow(Chemistry)` (fenêtre 15 min, report si volume < min_time) *(livré en logique pure `evaluateScheduledDose()` dans `dosing_logic`, conformément à R2/R5)*
- [x] A3 — Branche mode `scheduled` réécrite : injection cadencée par fenêtre 15 min
- [x] A4 — Respect strict `ph_limit_minutes` / `orp_limit_minutes`, `maxCyclesPerDay`, `minInjectionTimeMs`, stabilisation *(amendé R4 : exemption `cyclesToday` conservée et documentée, démarrages bornés structurellement + ring anti-rafale consulté)*
- [x] A5 — Recalcul du débit au changement de cible / au changement de plage de filtration
- [x] A6 — Pas de rattrapage J+1 : reliquat perdu à minuit, log `info`
- [x] A7 — Helper `filtration::remainingMinutesInDay(time_t)` si non existant *(livré en pur : `remainingRangeMinutes()` dans `schedule_logic`, conformément à R5)*
- [x] A8 — `ws_manager` publie `ph_scheduled_flow_ml_per_min`, `orp_scheduled_flow_ml_per_min`
- [x] B1 — Carte « Régulation » mode Programmée : afficher « Débit calculé : X mL/min sur Y h restantes » *(sans « Y h restantes » — horloge navigateur ≠ ESP32, choix commenté dans `app.js`)*
- [x] B2 — Toast warning « Quota non atteint, reliquat perdu » si fin de journée + remaining > 0 *(arbitrage web-ui-developer prévu par R5 : hint statique retenu — voir Notes d'implémentation)*
- [x] C1 — `docs/features/page-ph.md`, `page-orp.md` : mode Programmée détaillé
- [x] C2 — `docs/subsystems/pump-controller.md` : §Mode scheduled mis à jour
- [x] C3 — `docs/subsystems/filtration.md` : helper documenté si créé
- [x] C4 — `docs/adr/0021-repartition-scheduled.md` : ADR fenêtre 15 min + report + pas de rattrapage
- [x] C5 — `docs/API.md` : champs WS
- [x] C6 — `CHANGELOG.md` : entrée [2.8.0]
- [x] D1 — `pio run` sans nouveau warning, `./build_fs.sh` passe
- [x] D2 — Tests unitaires : répartition sur 1, 2, 4 plages avec différents reliquats *(amendé R1 : plage unique — 12 tests `evaluateScheduledDose` + 5 tests `remainingRangeMinutes`, 152/152)*

## Nouvelles signatures / API
- Champ WS : `ph_scheduled_flow_ml_per_min` (float), `orp_scheduled_flow_ml_per_min` (float)
- Méthode interne : `float PumpController::computeScheduledFlow(Chemistry)`
- Constante : `constexpr uint32_t kScheduledWindowMs = 15 * 60 * 1000;`
- ADR : doit documenter le choix d'algorithme (fenêtre 15 min + report si < min_time) et les alternatives écartées (répartition par plage, PID slow, etc.)

## Sécurité chimique
- Dosage impliqué : **oui** (produits pH- / chlore)
- Limites journalières / horaires concernées : toutes (`ph_limit_minutes`, `max_ph_ml_per_day`, `maxCyclesPerDay`, `minInjectionTimeMs`, stabilisation)
- Filtration requise avant action : **oui** (le mode scheduled n'injecte que pendant filtration)
- Calibration capteur modifiée : non
- Algorithme PID touché : non (scheduled ne passe pas par le PID)
- Risque identifié : **mauvaise répartition → pic local de concentration**. L'algorithme doit garantir qu'aucune fenêtre 15 min ne dépasse `flow × 15` mL.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte structurante : fenêtre temporelle imposée à toute future évolution scheduled), 3 (alternatives crédibles : PID slow imbriqué, répartition par plage, rattrapage J+1)
- **Sujet pressenti** : `docs/adr/NNNN-repartition-scheduled.md` — fenêtre 15 min uniforme, report si volume < min_time, pas de rattrapage J+1.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun nouveau (config déjà via `/save-config`)

### Champs WebSocket
- `ph_scheduled_flow_ml_per_min` (float)
- `orp_scheduled_flow_ml_per_min` (float)

### Topics MQTT
- Aucun (diagnostic UI uniquement)

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-ph.md`, `docs/features/page-orp.md` — mode Programmée détaillé
- `docs/subsystems/pump-controller.md` — §Mode scheduled mis à jour
- `docs/subsystems/filtration.md` — helper documenté si créé

#### Docs transverses (si contrat externe modifié)
- `docs/API.md` — champs WS `*_scheduled_flow_ml_per_min`
- `CHANGELOG.md` — entrée

#### Docs immuables
- `docs/adr/NNNN-repartition-scheduled.md` — ADR algorithme

## Critères d'acceptation
- [ ] Volume cible 300 mL sur 2 plages de filtration 10h-12h + 18h-20h (4 h total) → débit 75 mL/h, soit 18.75 mL par fenêtre de 15 min. Plage 10h-12h injecte ~150 mL, plage 18h-20h les 150 mL restants. *(sans objet en l'état — R1 : plage unique ; équivalent plage unique à valider on-target, cf. plan de test manuel)*
- [x] Limite horaire atteinte : l'algorithme reporte à la fenêtre suivante sans dépasser *(test natif : bornage budget horaire ADR-0020)*
- [x] `minInjectionTimeMs` respecté : si le volume de fenêtre est < 30 s équivalent, report à la fenêtre suivante *(test natif)*
- [x] `maxCyclesPerDay` respecté *(amendé R4 : démarrages bornés structurellement par le cadencement + ring anti-rafale consulté avant tout démarrage ; exemption `cyclesToday` conservée et documentée dans ADR-0021)*
- [x] En fin de journée avec quota non atteint, pas de débordement au lendemain, warning loggé *(livré : log `info` dans `tickDailyRollover`, cf. A6 — niveau `info` et non `warning`, cohérent avec la spec R2)*
- [x] Champ WS `ph_scheduled_flow_ml_per_min` expose le débit calculé
- [x] UI affiche le débit calculé dans la carte Régulation Programmée
- [ ] Mode Programmée → Automatique en cours de journée : le volume déjà injecté est conservé dans `ph_daily_ml` *(structurel — les compteurs ne sont pas réinitialisés au changement de mode ; validation on-target restante, point 5 du plan manuel)*
- [x] ADR créée *(ADR-0021)*
- [x] `pio run` compile sans nouveau warning
- [x] `./build_fs.sh` passe
- [x] Tous les libellés utilisateur sont en français

## Plan de test manuel
1. Plages 10-12 + 18-20, cible 300 mL pH- → vérifier que la répartition donne ~18 mL toutes les 15 min dans chaque plage. Pas d'injection > 30 mL en 15 min.
2. Cible 50 mL sur 8 h de filtration → débit très faible (6 mL/h) : le `minInjectionTimeMs` doit forcer des injections moins fréquentes mais plus longues.
3. Changer le volume cible de 300 à 500 mL à midi alors que 150 ont été injectés → débit recalculé sur le reste de la journée = 350 / durée_restante.
4. Simuler filtration OFF toute la journée → aucune injection, warning loggé en fin de journée.
5. Passer manuellement de Programmée à Automatique à 15h avec 200 mL déjà injectés → `ph_daily_ml = 200`, le PID reprend avec ce compteur.

## Notes pour agent
- **Concertation `pool-chemistry` obligatoire** : l'algorithme doit être validé sur un scénario d'hiver (pompe peu fréquente) et un scénario d'été (piscine chauffée très active).
- **Simplicité** : fenêtre 15 min vs PID imbriqué = choix explicite pour la prévisibilité et la testabilité.
- **Interaction avec feature-006** (injection manuelle gardée) : cette feature suppose déjà que les gardes chimiques sont factorisées. Synchroniser l'ordre.
- **Pas de rattrapage au lendemain** : décision produit simple (pas de « dette » accumulée).
- **ADR** : formaliser la décision algorithmique pour qu'elle ne soit pas diluée dans le code.

## Hors périmètre
- Répartition horaire variable (ex. plus le matin, moins le soir) — non, uniforme
- Rattrapage J+1 en cas de sous-injection J — non
- Prévision meteo / UV pour ajuster le débit — non, hors scope
- Écran LVGL — hors scope

## Notes d'implémentation (2026-07-05)

**Version livrée** : 2.8.0 (`src/version.h`).

### Fichiers modifiés

- `src/dosing_logic.h` / `.cpp` — logique pure : `ScheduledDoseDecision evaluateScheduledDose(const ScheduledDoseInputs&)` (fenêtre absolue `nowMin/15`, `remaining = min(cible, maxDailyMl) − injecté`, `v = remaining/nWin` borné budget horaire ADR-0020, report si durée < `minInjectionTimeMs`, snapshot `stopTargetMl`, `doseNow` réévalué chaque tick contre `min(stopTargetMl, cible effective)`, fail-closed watchdog/horizon/débit/fenêtre → `{false, -1, 0, NAN}`)
- `src/schedule_logic.h` / `.cpp` — `int remainingRangeMinutes(int nowMin, int startMin, int endMin)` (borné à minuit, 0 hors plage/invalide, wrap minuit géré)
- `src/pump_controller.cpp` / `.h` — coquilles scheduled pH/ORP symétriques (heure via `time()`+`kMinValidEpoch`, horizon `remainingRangeMinutes` ou `1440−nowMin` en mode `continu`, ring anti-rafale consulté avant tout démarrage, warn edge-triggered troncature budget, reliquat loggé `info` + réarmement fenêtres dans `tickDailyRollover`) ; nouveaux accesseurs `getPhScheduledPlannedFlow()` / `getOrpScheduledPlannedFlow()`
- `src/constants.h` — `kScheduledWindowMinutes = 15`
- `src/ws_manager.cpp` — champs `ph/orp_scheduled_flow_ml_per_min` (1 décimale, `null` si NAN), buffer 1472 → 1536
- `data/index.html`, `data/app.js` — ligne « Débit calculé » + hint reliquat dans les blocs Programmée
- `test/test_native_dosing/test_dosing_decision.cpp` (12 tests `evaluateScheduledDose`), `test/test_native_schedule/test_schedule_logic.cpp` (5 tests `remainingRangeMinutes`)

### Verdicts

- **pool-chemistry** : GO SOUS CONDITIONS (5 conditions : ring consulté avant démarrage ; horizon ≤ 0 → refus sans plancher `max(1,0)` ; clamps ≥ 0 + `doseNow` réévalué chaque tick ; watchdog vérifié dans la fonction pure ; débit effectif unique) → **GO, 5 conditions levées** post-implémentation. Arbitrage R4 (exemption `cyclesToday`) validé.
- **code-reviewer** : **Approved**.
- **test-engineer** : **152/152 tests natifs verts**.

### Choix d'implémentation

- **B2 → hint statique (choix B3)** : pas de toast dynamique « reliquat perdu » — hint statique permanent « Le volume non injecté avant minuit est perdu (pas de report au lendemain). » (arbitrage web-ui-developer prévu par R5 ; le toast exigerait une horloge client fiable).
- **Pas de « Y h restantes »** dans la ligne Débit calculé (horloge navigateur ≠ ESP32, choix commenté dans `app.js`).

### Écarts mineurs vs spec

- `minInjectionTimeMs` provient de `pumpProtection` (config), pas d'une constante.
- `kScheduledWindowMs` (A1) livré sous la forme `kScheduledWindowMinutes = 15` ; une constante `kScheduledWindowMs` intermédiaire a été **supprimée post-revue** (constante morte).
- Littéral `900000UL` (fenêtre 15 min du ring) aligné sur `canDose`.
- Le log de reliquat à minuit est en `info` (spec d'origine disait `warning` ; R2/A6 disait `info` — R2 fait foi).

### Recommandations non bloquantes (consignées, hors périmètre)

- `pump1MaxDutyPct` figé pour le calcul du débit effectif pH — **PRÉEXISTANT** à feature-011, à traiter dans une future spec.
- Troncature de log dupliquée (calcul du warn budget répété en coquille) — candidate à factorisation future.

### Validation on-target restante

Les 5 points du **Plan de test manuel** ci-dessus restent à dérouler sur matériel (répartition réelle sur plage, cible faible/report, changement de cible à midi, filtration OFF, bascule Programmée → Automatique).
