## Métadonnées
- **ID** : feature-023
- **Slug** : tests-atlas-ezo
- **Type** : feature
- **Statut** : **clôturée — supersédée** (2026-07-06, infrastructure livrée par les features 036-041)
- **Priorité** : normale (à la rédaction — caduque à la clôture)
- **Version cible** : n/a
- **Dernière mise à jour** : 2026-07-06

## Note de clôture (2026-07-06)

Spec rédigée en mai 2026, **massivement supersédée** par la campagne de tests des features 036-041 (postérieures), qui a livré son objet principal au-delà du périmètre demandé :

| Livrable spec | Réalité v2.10.1 |
|---|---|
| Infrastructure `test/` + Unity + env native | ✅ 4 suites natives (`test_native_sensor_filter/dosing/history/schedule`) + shim Arduino, **163 tests** |
| `pio test` < 30 s | ✅ ~3,4 s |
| `tools/run_tests.sh` (« pas de CI cloud » — hors périmètre spec) | ✅ Dépassé : CI GitHub Actions (push+PR, seuil couverture 90 %, `.github/workflows/tests-natifs.yml`) |
| Couverture ≥ 80 % chemins critiques | ✅ 100 % lignes des 4 modules purs, outillage gcovr (`tools/coverage.sh`) |
| Tests des 6 conditions pool-chemistry de la 021 | ✅ Couverts en mieux : gardes dans `evaluateDose` **pur** (15 causes dont `FilterNotReady`, `NotCalibrated`, anti-rafale), 74 tests dosing |
| Correction CLAUDE.md « pas de dossier test/ » | ✅ Faite à la clôture (2026-07-06) |

**Résidu non couvert et volontairement écarté** : les 10 tests unitaires `AtlasEzoSensor` avec mock du bus I²C (`mock_wire`). L'approche mock-de-périphérique est **architecturalement périmée** : le projet a tranché (ADR-0017, features 036-041) pour l'extraction en logique pure plutôt que le mocking. Le parsing EZO est stable en production, besoin non démontré.

**Porte de sortie** : si un bug de parsing EZO apparaît un jour, ouvrir une micro-spec « extraire le parsing des réponses EZO (status bytes, `?CAL,n`, `?I`, float) en fonctions pures + tests natifs » — même pattern que les 4 modules existants, PAS de mock Wire.

Aucun code modifié au titre de cette spec (hors correction documentaire CLAUDE.md). Contenu conservé ci-dessous pour traçabilité.
- **Spec(s) liée(s)** : `specs/features/done/feature-021-migration-atlas-ezo.md` § Tests à prévoir lignes 373-380

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`** uniquement pour valider que les tests couvrent bien les 6 conditions
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
- [ ] pool-chemistry — uniquement en revue finale pour valider la couverture des 6 conditions
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer — **principal**
- [x] code-reviewer
- [x] doc-writer

## Description

Mettre en place l'infrastructure de tests automatisés du projet (dossier `test/` PlatformIO + Unity), puis écrire les tests unitaires et d'intégration de feature-021 (Atlas EZO) qui n'ont pas pu être livrés par manque de framework.

## Contexte

Le projet n'a actuellement **aucun test automatisé** (`CLAUDE.md` mentionne « pas de dossier `test/` actuellement ; à mettre en place progressivement »). La spec feature-021 § Tests à prévoir listait 6 tests qui n'ont jamais été créés :
- Unitaire `AtlasEzoSensor` — mock I²C, parsing des réponses (`?CAL,2`, `7.012`, `*ER`)
- Intégration : `Cal,?` injoignable → `getPhCalibrationPoints() == -1` → `canDose() == false`
- Intégration : stale > 20 s → NaN + log critical + alerte MQTT
- Intégration : calibration mid puis low → `phCalPoints` passe 0→1→2, alerte clearée, `armStabilizationTimer()` appelé
- UI : queue → spinner → WS transition → callout success
- Régression UART écran : `actionCalibratePhNeutral` enqueue sans crash

Cette feature crée le socle de tests pour le projet et livre cette première salve de tests pour Atlas EZO. Les tests futurs (feature-022 follow-up dosage, etc.) s'appuieront sur la même infrastructure.

## Comportement attendu

### Infrastructure
- Dossier `test/` à la racine du projet, conforme à la convention PlatformIO Unity
- Structure :
  ```
  test/
    test_atlas_ezo/
      test_main.cpp           # Tests unitaires AtlasEzoSensor
      mock_wire.h/cpp         # Mock du bus I²C (Wire.write/requestFrom)
    test_sensors_integration/
      test_main.cpp           # Tests intégration Sensors (stale, queue, cal points)
      mock_wire.h/cpp         # Mock partagé
    README.md                 # Comment lancer les tests
  ```
- Commande `pio test` doit fonctionner localement (sans ESP32 branché — environnement `native` ou émulateur Unity)
- CI : ajouter une vérification `pio test` dans un script `tools/run_tests.sh` (pas de CI cloud à mettre en place dans cette feature, mais un script standalone exécutable manuellement)

### Tests unitaires `AtlasEzoSensor` (`test_atlas_ezo/`)

| Test | Description |
|------|-------------|
| `test_send_cmd_writes_to_wire` | `sendCmd("R")` appelle `Wire.beginTransmission(addr)`, `Wire.write("R")`, `Wire.endTransmission()` |
| `test_read_response_parses_status_1` | Mock retourne `[1, '7', '.', '0', '1', '2', 0]` → `readResponse()` retourne `7.012` (after parsing) |
| `test_read_response_status_2_returns_negative` | Mock retourne `[2, ...]` → `readResponse()` retourne `-1` |
| `test_read_response_status_254_returns_pending` | Mock retourne `[254]` → comportement attendu (re-essayer ou retourner -1) |
| `test_read_single_full_sequence` | `readSingle(out, 25.5)` → vérifier ordre `RT,25.5` puis `R`, parsing du résultat, et que mutex I²C est tenu pendant TOUTE la séquence |
| `test_query_cal_points_parses_correctly` | Mock retourne `[1, '?', 'C', 'A', 'L', ',', '2', 0]` → retourne `2` |
| `test_query_cal_points_unreachable_returns_minus_1` | Mock simule `Wire.endTransmission` retournant erreur → retourne `-1` |
| `test_calibrate_returns_true_on_status_1` | Mock retourne `[1]` → `calibrate("mid,7.00")` retourne `true` |
| `test_clear_calibration_sends_cal_clear` | `clearCalibration()` envoie bien la commande `Cal,clear` |
| `test_read_info_parses_firmware_version` | Mock retourne `[1, '?', 'I', ',', 'p', 'H', ',', '2', '.', '1', '0', 0]` → fw = `"EZO,pH,2.10"` ou similaire |

### Tests d'intégration `Sensors` (`test_sensors_integration/`)

| Test | Description |
|------|-------------|
| `test_get_ph_returns_nan_when_stale` | Forcer `_lastPh` = 7.0 et `_lastPhMs` = `millis() - 21000` → `getPh()` retourne NaN |
| `test_get_ph_returns_value_when_fresh` | `_lastPhMs` = `millis() - 5000` → `getPh()` retourne la valeur |
| `test_fail_streak_invalidates_cache` | Simuler 2 échecs I²C consécutifs → `_lastPh` passe à NaN, `_phCalCachedPoints` passe à -1 |
| `test_can_dose_blocks_on_stale` | Mocker `getPh()` à NaN → `canDose(0)` retourne `false` avec la bonne cause |
| `test_can_dose_blocks_on_low_cal_points` | `_phCalCachedPoints = 1` → `canDose(0)` retourne `false` (requis ≥ 2) |
| `test_can_dose_blocks_on_unreachable` | `_phCalCachedPoints = -1` → `canDose(0)` retourne `false` |
| `test_calibration_arms_stabilization` | Enqueue `CalibratePhMid` → exécution → vérifier `_stabilizationEndMs[0] > millis()` après |
| `test_anti_burst_blocks_after_6_per_min` | Appeler `recordDosingCycleStart(0)` 6 fois en < 60 s → `canDose(0)` retourne `false` au 7ᵉ |
| `test_compensation_temp_with_nan_uses_25C` | Mocker `getWaterTemperature()` = NaN → vérifier que `RT,25.0` est envoyé à l'EZO pH |

### Tests UI (à part — optionnel, faible priorité)

À discuter avec `ux-ui-designer` : framework de tests JS (Jest, Vitest, ou Playwright pour E2E) ou tests manuels seulement. **Hors périmètre de cette feature** sauf décision explicite.

### Cas limites
- Tests doivent passer sans ESP32 connecté (environnement `native` recommandé pour `pio test`)
- Mock I²C doit pouvoir simuler à la fois succès, échec d'écriture, échec de lecture, et délais de réponse variables
- Tests d'intégration ne doivent PAS dépendre de `millis()` réel — utiliser un mock de temps si nécessaire

## Comportement actuel

- Aucun test automatisé
- Validation feature-021 reposait uniquement sur `pio run` (compilation) + tests manuels hardware (à faire par utilisateur)

## Contraintes techniques

- Compatible PlatformIO 6.x + Unity (framework de test embarqué standard)
- Tests doivent être **rapides** (< 30 s pour la suite complète) pour être lançables fréquemment
- Pas de modification du code source de feature-021 sauf ajout de hooks de test (par exemple : exposer `_lastPh` / `_lastPhMs` en `friend` ou via méthode `#ifdef UNIT_TEST`)
- Mock du bus I²C : créer un wrapper minimal qui peut être substitué à `Wire` en mode test
- Couverture cible : ≥ 80 % des lignes de `atlas_ezo.cpp` et des chemins critiques de `sensors.cpp` (lecture, stale, fail streak, queue)

## Fichiers potentiellement concernés

- `test/test_atlas_ezo/test_main.cpp` (NOUVEAU)
- `test/test_atlas_ezo/mock_wire.h/cpp` (NOUVEAU)
- `test/test_sensors_integration/test_main.cpp` (NOUVEAU)
- `test/README.md` (NOUVEAU)
- `tools/run_tests.sh` (NOUVEAU)
- `platformio.ini` — ajouter section `[env:native]` pour les tests
- `src/atlas_ezo.h/cpp` — éventuels hooks `#ifdef UNIT_TEST` (à minimiser)
- `src/sensors.h/cpp` — idem
- `CLAUDE.md` — mettre à jour la section « pas de dossier `test/` actuellement »

## Livrables — check-list
- [ ] Dossier `test/` créé avec structure conforme PlatformIO
- [ ] Mock I²C fonctionnel
- [ ] 10 tests unitaires `AtlasEzoSensor` passants
- [ ] 9 tests intégration `Sensors` passants
- [ ] `pio test` exécutable localement, < 30 s
- [ ] Script `tools/run_tests.sh` documenté
- [ ] `test/README.md` explique comment ajouter de nouveaux tests
- [ ] Mise à jour `CLAUDE.md` (suppression de la mention « pas de dossier test/ »)

## Sécurité chimique (à valider par pool-chemistry)

- **Dosage impliqué : indirect** — les tests vérifient que `canDose()` se comporte correctement face aux conditions critiques (stale, cal manquante, bus dégradé, anti-rafale)
- **Recommandation pool-chemistry** : valider en revue finale que les 6 conditions implémentées en feature-021 sont toutes couvertes par au moins 1 test d'intégration

## ADR attendu ?
- **Probable** : non. Mise en place d'infrastructure standard PlatformIO/Unity, pas de décision structurante.
- ADR potentiel uniquement si on choisit un framework non standard (par exemple Catch2, GoogleTest sur ESP32, ou un mock framework lourd).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- aucune

#### Docs transverses
- `docs/BUILD.md` — section nouvelle « Tests automatisés » (commande `pio test`, structure du dossier, comment ajouter un test)
- `CLAUDE.md` — supprimer la mention « pas de dossier `test/` actuellement »

#### Docs immuables
- aucun ADR a priori

## Critères d'acceptation
- [ ] AC1 — `pio test -e native` exécute la suite sans erreur en < 30 s
- [ ] AC2 — Au moins 1 test couvre chacune des 6 conditions pool-chemistry de feature-021
- [ ] AC3 — Mock I²C réutilisable pour des features futures (signature documentée)
- [ ] AC4 — Documentation `test/README.md` permet à un nouveau contributeur d'ajouter un test sans assistance
- [ ] AC5 — `tools/run_tests.sh` retourne code 0 sur succès, ≠ 0 sur échec (utilisable en CI future)

## Plan de test manuel
1. Cloner le projet propre, lancer `pio test -e native` → succès
2. Casser volontairement une garde de `canDose()` (ex. retirer le check stale) → `pio test` doit échouer avec un message clair
3. Lancer `tools/run_tests.sh` → vérifier code retour 0
4. Ajouter un nouveau test bidon en suivant `test/README.md` → doit s'intégrer sans toucher au build firmware

## Notes pour agent

- Cette feature est **transverse** : elle bénéficie au projet entier, pas seulement à feature-021. Une fois livrée, toute spec future ayant un agent `test-engineer` pourra produire de vrais tests automatisés au lieu de juste vérifier `pio run`.
- Découpage suggéré en 2 passes courtes :
  - Pass A : infrastructure (`test/`, mock I²C, `pio test` exécutable, 1 test minimal qui passe)
  - Pass B : 19 tests Atlas EZO + intégration Sensors + script `run_tests.sh`
- Ne pas sur-engineerer le mock I²C — un simple buffer d'octets en/out + erreurs configurables suffit pour 90 % des cas.
- Le projet utilise déjà ArduinoJson 7 / FreeRTOS / Wire / OneWire — tout cela doit pouvoir être mocké ou stubbé en environnement `native`.
- Référence : la spec feature-021 § Tests à prévoir détaillait précisément les 6 scénarios. Les reprendre en l'état pour cohérence.

## Hors périmètre
- Tests UI / E2E (Jest, Playwright, etc.) — à voir en feature séparée si besoin
- CI cloud (GitHub Actions, etc.) — script `run_tests.sh` standalone seulement, pas de pipeline cloud
- Tests des autres subsystems (filtration, lighting, OTA, etc.) — à ajouter au cas par cas dans les features dédiées une fois l'infra en place
- Couverture 100 % — viser 80 % suffit. La feature ne doit pas devenir un puits sans fond.
