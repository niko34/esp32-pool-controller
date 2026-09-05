## Métadonnées
- **ID** : feature-067
- **Slug** : offset-mesure-ph
- **Type** : feature
- **Statut** : doing — implémenté (tests verts, revue OK), **validation on-target restante**
- **Priorité** : normale
- **Version cible** : 2.20.0 (livrée)
- **Dernière mise à jour** : 2026-08-15
- **Spec(s) liée(s)** : feature-025 (filtre pH médiane + EMA), project_ezo_migration (modules Atlas EZO I2C)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — la valeur corrigée devient l'entrée de la régulation pH
- [ ] ux-ui-designer — un champ numérique dans une carte existante, pas de nouvelle surface
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter un paramètre logiciel `ph_offset` (−1,00 à +1,00 pH, pas de 0,01, défaut 0,00) appliqué à la mesure pH — **sans jamais toucher la calibration interne du module Atlas EZO**. Objectif : corriger un décalage lié à l'installation de la sonde dans le circuit hydraulique.

## Contexte
La mesure pH présente un décalage attribué à l'implantation de la sonde (position dans le circuit, potentiel électrique de l'eau). Recalibrer l'EZO pour compenser fausserait la calibration électrochimique elle-même ; on veut donc une correction **logicielle, réversible et visible**, séparée de la calibration pH 4 / pH 7.

**Précédent architectural existant à suivre** : `mqttCfg.tempCalibrationOffset` (clé NVS `temp_cal_off`, défaut 0.0f) fait déjà exactement cela pour la température — persistance, exposition dans la config, application à la lecture. `ph_offset` doit s'aligner sur ce patron plutôt que d'inventer un mécanisme.

## Comportement attendu
- `pH corrigé = pH brut EZO + ph_offset`.
- La **régulation** (PID auto, dosage programmé, sécurités) consomme la valeur **corrigée et lissée**.
- Le **diagnostic** continue d'exposer la valeur **brute** : on doit pouvoir dire à tout instant ce qui vient de l'EZO et ce qui vient de la correction.
- L'offset est modifiable à chaud ; sa prise en compte doit être **immédiate**, sans attendre le renouvellement de la fenêtre de lissage.
- Persistance NVS, conservé au redémarrage ; valeur par défaut 0,00 si absent.

### Cas limites
- Valeur hors plage [−1,00 ; +1,00] → **refus** de l'enregistrement (validation serveur, pas seulement UI).
- Valeur non numérique / champ absent dans le POST → paramètre inchangé (patron des autres champs).
- Mesure brute indisponible (NaN, capteur stale, filtre non amorcé) → la valeur corrigée reste NaN : **l'offset ne doit jamais fabriquer une mesure** à partir de rien, ni lever un `isnan` qui protège le dosage.
- Changement d'offset pendant une injection en cours : préciser le comportement attendu (arrêt naturel au prochain cycle d'évaluation — à confirmer par `pool-chemistry`).
- Offset non nul + filtre pH en latch EMI : le latch dépend du signal brut, il ne doit pas être perturbé.

## Comportement actuel
Chaîne pH actuelle (analyse préalable) :
1. `SensorManager::_readEzoSensors()` (`src/sensors.cpp`) lit l'EZO → `_lastPh` (brut) ;
2. le brut alimente `_phFilter.addSample()` (médiane + EMA + rejet de pics + latch EMI, feature-025) ;
3. accesseurs : `getPh()` / `getPhRaw()` (brut), `getPhFiltered()` (lissé), `getPhMedian()` ;
4. consommateurs : `pump_controller.cpp:430` et `:942` (PID/dosage, sur **filtré**), `:1035` (`getPh()`), `history.cpp:402`, `main.cpp:1220`, `mqtt_manager.cpp:696/698/814/816/870/1013`, `ws_manager.cpp:319/321`, `web_routes_data.cpp:46/50`, `web_routes_debug.cpp:105/107` ;
5. trace de diagnostic : `SensorTraceSample{ phRaw, phFiltered, … }` (`sensors.h:183`).

MQTT expose **déjà** trois entités distinctes : `poolcontroller_ph`, `poolcontroller_ph_raw`, `poolcontroller_ph_filtered` — ce qui permet d'introduire la correction **sans casser aucun contrat** (voir « Nouvelles signatures »).

Aucun offset pH n'existe aujourd'hui.

## Contraintes techniques
- ESP32, offline first ; vanilla JS ; français dans l'UI.
- **Ne pas modifier** la calibration EZO ni les commandes `Cal,*` envoyées au module.
- Le filtre pH (médiane + EMA + latch EMI) est un module **pur testé en natif** (ADR-0017) : ne pas le modifier pour y injecter l'offset.
- Décision d'implémentation à trancher par l'`architect`, recommandation issue de l'analyse : **appliquer l'offset EN SORTIE des accesseurs**, pas à l'entrée du filtre. Médiane et EMA sont invariantes par translation (`filtre(x + c) = filtre(x) + c`), donc le résultat est mathématiquement identique — mais en sortie : (a) un changement d'offset est pris en compte **immédiatement** au lieu d'être dilué sur toute la fenêtre de lissage, (b) l'état interne du filtre et ses seuils de rejet de pics restent sur le signal brut, donc le latch EMI est inchangé, (c) la trace de diagnostic conserve naturellement le brut.
- Aucun handler HTTP bloquant > 50 ms.

## Fichiers potentiellement concernés
- `src/config.h` — champ `phOffset` (défaut 0.0f), à côté de `tempCalibrationOffset`
- `src/config.cpp` — persistance NVS (`putFloat`/`getFloat`, clé courte type `ph_offset`)
- `src/constants.h` — bornes de validation (`kPhOffsetMin` / `kPhOffsetMax`)
- `src/sensors.h` / `src/sensors.cpp` — accesseurs corrigés + conservation explicite du brut
- `src/web_routes_config.cpp` — lecture/écriture du champ + **validation de plage**
- `src/ws_manager.cpp` — champ de config poussé à l'UI, et valeurs temps réel
- `src/mqtt_manager.cpp` — cohérence des entités `ph` / `ph_raw` / `ph_filtered`
- `src/web_routes_debug.cpp` — trace de diagnostic (brut vs corrigé)
- `data/index.html`, `data/app.js` — champ « Offset pH » + texte d'aide
- `docs/API.md`, `docs/MQTT.md`, `docs/features/page-ph.md`, `docs/subsystems/sensors.md`, `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — `phOffset` dans `config.h` (défaut 0.0f) + persistance NVS (clé `ph_offset`) + relecture au boot **avec fail-safe** (valeur hors bornes neutralisée à 0,00 + `WARN`)
- [x] A2 — Bornes `kPhOffsetMin`/`kPhOffsetMax` dans `constants.h` — ⚠️ **±0,50 et non ±1,00** : resserrement imposé par `pool-chemistry` (arbitrage 1 des Notes d'implémentation)
- [x] A3 — Offset appliqué **en sortie des accesseurs** ([ADR-0039](../../../docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) ; brut conservé explicitement ; `NaN in → NaN out` (`applyPhOffset`, logique pure `src/ph_offset.h`)
- [x] A4 — Accesseurs distincts : `getPhRaw()` (brut EZO), `getPh()` / `getPhMedian()` / `getPhFiltered()` (corrigés), `getPhFilteredRaw()` (brut filtré + garde d'âge), `getPhOffset()`
- [x] A5 — Validation serveur : **HTTP 400** hors plage (refus, pas d'écrêtage) ; champ absent ou non numérique → paramètre inchangé
- [x] A6 — Régulation auto, dosage programmé et sécurités alimentés par la valeur **corrigée**, assortis de **3 filets de sécurité sur le brut** exigés par le gate chimique
- [x] A7 — Trace de diagnostic : `phRaw` + `phCorrected` + `phFiltered` (brut) dans `/debug/sensor_trace`
- [x] B1 — Carte « Correction de la mesure pH » (page pH) : champ au centième signé + texte d'aide permanent + **2 paliers d'avertissement** (0,15 / 0,30)
- [x] B2 — Affichage diagnostic (modal « État filtre pH ») : brut EZO, offset, corrigé, médiane corrigée, filtrée corrigée
- [x] C1 — Docs : `API.md`, `MQTT.md`, `page-ph.md`, `sensors.md`, `pump-controller.md`, `ws-manager.md`, **ADR-0039**, `CHANGELOG.md` [2.20.0]
- [x] D1 — Tests natifs de la logique pure (application de l'offset, bornes, propagation NaN) — suite `test_native_ph_offset`
- [x] D2 — Tests de non-régression du filtre pH (latch EMI et rejet de pics inchangés) + **preuve d'invariance par translation** `filtre(x+c) == filtre(x)+c` sur le filtre réel

## Nouvelles signatures / API
- Champ config HTTP/WS : `ph_offset` (float, −1.0 à +1.0)
- Fonctions publiques : accesseur(s) pH corrigé/brut dans `src/sensors.h` — nommage à arrêter par l'`architect`
- Constantes : `constexpr float kPhOffsetMin = -1.0f; constexpr float kPhOffsetMax = 1.0f;`
- MQTT : **aucun renommage d'entité existante**. `poolcontroller_ph` et `poolcontroller_ph_filtered` doivent porter la valeur **corrigée** (c'est la valeur « vraie » du bassin), `poolcontroller_ph_raw` reste le **brut EZO**. Exposition éventuelle de `ph_offset` : à trancher par l'`architect` (utile pour l'historique HA, mais ajoute une entité).
  ⚠️ Ce choix change la **valeur** publiée sur des entités existantes sans changer leur nom : les historiques Home Assistant présenteront une marche le jour de l'activation. À signaler à l'utilisateur dans le CHANGELOG.

## Sécurité chimique
- Dosage impliqué : **oui** — produit : pH− (acide)
- Limites journalières / horaires concernées : inchangées, mais l'**entrée** de la régulation change
- Filtration requise avant action : inchangé
- Calibration capteur modifiée : **non** — exigence absolue, l'EZO n'est pas touché
- Algorithme PID touché : non dans sa logique, **oui dans son entrée**
- Risque identifié : un offset erroné décale la consigne effective. Exemple : offset +0,40 avec cible 7,20 → le système régule en réalité sur un pH brut de 6,80. Un offset mal saisi peut donc conduire à **surdoser l'acide** en croyant corriger. La borne ±1,00 limite l'ampleur ; `pool-chemistry` doit se prononcer sur cette borne, sur l'affichage d'avertissement, et sur le comportement d'un changement d'offset pendant une injection.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 2 (décision invisible dans le code : offset en sortie plutôt qu'en entrée du filtre), 4 (verrouillage : sémantique des entités MQTT `ph` / `ph_raw`)
- **Sujet pressenti** : « Correction logicielle de mesure pH : point d'application et sémantique des valeurs publiées »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-config` et `POST /save-config` — champ `ph_offset`
- `GET /data`, `/debug/sensor_trace` — valeurs brutes vs corrigées

### Champs WebSocket
- `ph_offset` (config), et champs de mesure existants dont la sémantique est précisée

### Topics MQTT
- Aucun nouveau topic obligatoire. Sémantique clarifiée de `poolcontroller_ph` (corrigé), `poolcontroller_ph_raw` (brut EZO), `poolcontroller_ph_filtered` (lissé corrigé). Entité `ph_offset` optionnelle — décision `architect`.

### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/MQTT.md`, `docs/features/page-ph.md`, `docs/subsystems/sensors.md`, `CHANGELOG.md`, ADR si confirmé

---

## Notes d'implémentation (v2.20.0, 2026-08-15)

### Les 7 arbitrages de l'`architect`

| # | Question | Arbitrage retenu | Pourquoi |
|---|---|---|---|
| 1 | **Bornes de l'offset** | **±0,50** au lieu des ±1,00 de la spec (`kPhOffsetMin` / `kPhOffsetMax`) | Un offset décale la consigne **effective** : cible 7,20 avec offset +0,40 ⇒ l'eau est régulée à 6,80. Au-delà de ±0,50, l'écart pH régulé / pH réel devient dangereux (corrosion côté bas, chlore inactif côté haut) et le défaut vient de la sonde ou de la calibration, pas de l'implantation. Resserrement **imposé par `pool-chemistry`**. |
| 2 | **Point d'application** | **En sortie des accesseurs**, pas à l'entrée du filtre → [ADR-0039](../../../docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) | Médiane et EMA sont invariantes par translation (résultat identique), mais en sortie : prise en compte **immédiate** au lieu d'être diluée sur ~65 s, seuils de rejet et latch EMI **inchangés**, trace de diagnostic naturellement brute. Le patron `tempCalibrationOffset` (ingestion) **n'est pas transposable** : la T° corrigée doit partir sur le bus dans `RT,<temp>`, le pH corrigé n'est réinjecté nulle part. |
| 3 | **Sémantique des accesseurs** | Règle **« seuls les `*Raw` sont bruts »** | `getPhRaw()` cesse d'être un alias pur de `getPh()` (dette corrigée dans ce lot). Une règle unique, énonçable en une phrase, vaut mieux qu'une table de correspondance à mémoriser. |
| 4 | **Accesseur brut filtré** | Ajout de `getPhFilteredRaw()` **avec garde d'âge** (`NaN` si filtre non `ready()`) | `filtered()` n'expire jamais de lui-même (contrat feature-058) : sans garde, une EMA périmée pourrait déclencher un faux plancher (et consommer le compteur des 3 refus) ou masquer un vrai franchissement. |
| 5 | **Validation serveur** | **Refus HTTP 400**, pas d'écrêtage silencieux | Premier champ de `/save-config` à refuser (cf. `max_cycles_per_day`, clampé). Un offset écrêté serait un **mensonge de mesure**. Incohérence de style assumée et documentée. |
| 6 | **Contrat MQTT** | **Aucun renommage**, aucune entité `ph_offset` | Renommer `poolcontroller_ph` casserait automatisations, dashboards et historiques HA. Une entité `ph_offset` ajouterait un point de commande sur l'**entrée de la régulation chimique** hors du chemin validé. |
| 7 | **Effets de bord de l'écriture** | L'écriture de l'offset ne touche **aucun** compteur de dosage | `requestReset()` / `resetDosingStates()` / `resetPhPauseGuard()` sont **interdits** dans ce chemin : ils permettraient de contourner limites journalières et pause anti-cycling par enregistrements répétés. |

### Gate `pool-chemistry` — conditions et 3 passes

**Passe 1 — NO GO.** Verdict : « l'offset déplace l'entrée de la régulation sans aucun filet ». Conditions posées :
1. Bornes resserrées à **±0,50** (arbitrage 1) ;
2. **Plancher dur d'injection d'acide sur le pH BRUT** (`kPhRawDosingFloor = 6,80`), avec **palier élargi** `kPhRawDosingFloorWide = 6,90` au-delà de `kPhOffsetWideThreshold = 0,30` d'offset — l'incertitude sur le pH réel croît avec |offset| ;
3. **Inhibition du dosage ORP** si pH brut > `kPhRawOrpInhibit = 7,60` (fraction HClO effondrée : la régulation ORP compenserait en surdosant) ;
4. Avertissements UI **avant enregistrement**, à deux paliers, annonçant le pH réel visé ;
5. Aucune modification de la calibration EZO ;
6. `NaN in → NaN out` strict — l'offset ne fabrique jamais une mesure ;
7. **Compteur de refus de plancher** : 3 en 24 h ⇒ `CRITICAL` « sonde à recalibrer ou remplacer » + bascule de la régulation pH en **manuel** (persistée) ;
8. **Journalisation** de tout changement d'offset (ancien → nouveau).

**Passe 2 — GO sous conditions (P2 bloquant).** Deux trous relevés dans la première implémentation :
- `getPhFilteredRaw()` sans garde d'âge (⇒ arbitrage 4) ;
- **fail-closed manquant sur pH brut indisponible** en régulation ORP automatique : `canDose(1)` ne consulte **jamais** le pH, ce filet est donc l'**unique** garde du couplage pH/chlore. Un pH inconnu doit suspendre la chloration automatique. Journalisé en `INFO` (transitoire), pas `CRITICAL`. Le dosage **programmé** reste aveugle à la mesure, par conception ([ADR-0002](../../../docs/adr/0002-mode-programmee-volume-quotidien.md)).

**Passe 3 — GO.** Conditions 1 à 8 vérifiées, filets P2 livrés, plancher confirmé **inerte à offset nul** et sans objet en correction basique (`ph_plus`).

### Chiffres

- **537 tests natifs** (+30, nouvelle suite `test_native_ph_offset`), zéro régression.
- Flash : **core 2 = 83,5 %**, **core 3 = 93,3 %**. `build_fs.sh` OK.
- Trace capteur : échantillon 24 → **28 octets** (champ `phCorrected`), buffer ≈ **8,4 Ko** RAM, page de 50 ≈ **4,5 Ko** JSON.
- Buffers JSON : `_buildConfigJson()` et `/get-config` → marge portée à **2368 octets** (+1 champ ≈ 24 o).

### Écarts par rapport à la spec initiale

| Point de la spec | Livré | Raison |
|---|---|---|
| Plage ±1,00 | **±0,50** | `pool-chemistry`, condition 1 |
| « Entité `ph_offset` : à trancher » | **Non créée** | Arbitrage 6 |
| Rien sur les filets de sécurité | **3 filets sur le pH brut** + escalade 3 refus / 24 h | Conditions 2, 3, 7 + P2 de la passe 2 |
| Docs listées : API, MQTT, page pH, sensors, CHANGELOG | **+ `pump-controller.md`, `ws-manager.md`, ADR-0039** | Filets de dosage et champ WS non anticipés au cadrage |

## Validation on-target restante

Non couvert par les tests natifs — à vérifier sur l'appareil avant de clore la spec :

- [ ] **Persistance NVS** : saisir un offset (ex. `+0,15`), redémarrer, vérifier qu'il est relu et appliqué (log `INFO` au boot, valeur dans `/get-config`).
- [ ] **Immédiateté** : enregistrer un offset et vérifier que `ph` / `phFiltered` (WS, MQTT, UI) bougent **au push suivant**, sans attendre la purge de la fenêtre de lissage.
- [ ] **Refus 400** : `POST /save-config` avec `ph_offset = 0.8` → 400, corps exact, valeur **inchangée** côté appareil ; puis `ph_offset` absent → inchangé.
- [ ] **Plancher acide** : simuler un pH brut sous 6,80 (solution étalon ou offset large) → refus de dosage acide, `CRITICAL`, alerte MQTT `ph_raw_floor`, cause visible dans le modal filtre pH. Vérifier le passage au palier **6,90** avec |offset| > 0,30.
- [ ] **Escalade 3 refus / 24 h** → bascule effective de `ph_regulation_mode` en `manual` **persistée après reboot**.
- [ ] **Inhibition ORP** : pH brut > 7,60 → dosage chlore suspendu + alerte `orp_ph_raw_inhibit` ; et EZO pH débranché en mode automatique → chloration suspendue avec log `INFO` (pas d'alerte).
- [ ] **Non-réinitialisation des compteurs** : enregistrer l'offset plusieurs fois de suite pendant une journée de dosage → `ph_daily_ml`, cycles du jour et pause mélange **inchangés**.
- [ ] **Écran de calibration** : vérifier que le readout suit le brut lors d'un changement de solution étalon avec un offset non nul actif, et affiche `--` (jamais le corrigé) si `phRaw` est indisponible.
- [ ] **Home Assistant** : constater la marche sur `sensor.*_ph` sans changement d'`entity_id`, et l'absence de marche sur `sensor.*_ph_raw`.
- [ ] **Occupation mémoire** : relever RAM/heap après ajout du champ `phCorrected` (buffer trace +1,2 Ko) et ouvrir la card « Signaux capteurs » de l'onglet Avancé (pagination ≈ 4,5 Ko/page).
