# Changelog - ESP32 Pool Controller

## [2.1.2] - 2026-05-10

### Sécurité chimique

- **Garde filtration sur l'injection manuelle** — bug observé en production : injection manuelle volumée pH lancée pendant filtration active continuait après l'arrêt programmé de la filtration → surdosage local d'acide dans la zone du retour d'eau (pas de circulation). Refonte de [`src/web_routes_control.cpp`](src/web_routes_control.cpp) avec deux mitigations :
  - **Refus en amont (HTTP 409)** — helper privé `injectionAllowedOrReject(req, tag)` appelé en début des handlers `/ph/inject/start`, `/orp/inject/start`, `/pump1/on`, `/pump2/on`. Vérifie `mqttCfg.regulationMode == "continu" || filtration.isRunning()` (même critère que `PumpController::canDose()`, cohérence des deux gardes). Refus = HTTP `409 Conflict` avec corps texte explicite. Les handlers d'arrêt (`/ph/inject/stop`, `/orp/inject/stop`, `/pump[12]/off`) restent **inconditionnels** — pouvoir arrêter en toute circonstance.
  - **Arrêt cyclique en cours d'injection** — `updateManualInject()` (appelée à chaque tour `loopTask`) interrompt l'injection si la filtration tombe pendant celle-ci. Latence < 100 ms après détection. Logue `critical("[Injection] {pH|ORP} INTERROMPUE — filtration arrêtée (sécurité chimique)")` et publie l'alerte MQTT correspondante.
- **Bornage durée injection 3600 → 600 s** — nouvelle constante `kManualInjectMaxDurationS = 600` ([`src/constants.h`](src/constants.h)) appliquée aux deux routes inject/start. Justification pool-chemistry : 3600 s trop long en cas d'arrêt filtration en milieu de cycle ; 10 min couvrent les usages typiques et l'utilisateur peut toujours relancer.
- **Pas de reprise automatique** après reprise filtration — choix produit. L'injection en cours est perdue, l'utilisateur doit relancer manuellement (toast UI explicite).

> Validation `pool-chemistry` : GO sous conditions, **toutes appliquées**. Cohérence avec la condition #3 de `canDose()` (filtration en marche sauf mode `continu`) — voir [docs/subsystems/pump-controller.md](docs/subsystems/pump-controller.md).

### API

- **`POST /ph/inject/start`, `POST /orp/inject/start`, `POST /pump1/on`, `POST /pump2/on`** : nouveau code retour **`409 Conflict`** « filtration arrêtée — injection refusée pour sécurité chimique » (sauf si `regulationMode == "continu"`). Voir [`docs/API.md`](docs/API.md).
- **`POST /ph/inject/start`, `POST /orp/inject/start`** : paramètre `duration` désormais borné à **600 s** (`kManualInjectMaxDurationS`) au lieu de 3600 s.

### MQTT

Deux nouveaux types d'alerte sur le topic existant `{base}/alerts` (QoS 0, no retain — mécanisme `mqttManager.publishAlert()` inchangé). Aucun nouveau topic, aucune nouvelle entité auto-discovery HA — l'utilisateur peut créer une automation HA sur le topic alerts.

| Type | Condition |
|------|-----------|
| `ph_injection_aborted` | Injection manuelle pH interrompue par la sécurité chimique (filtration arrêtée pendant injection) |
| `orp_injection_aborted` | Injection manuelle ORP/chlore interrompue par la sécurité chimique |

### Frontend

- **Refus 409 au démarrage** : `startInject(product)` dans [`data/app.js`](data/app.js) restaure le bouton et affiche un toast rouge « Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local). »
- **Interruption en cours** : `_onWsLog(entry)` détecte `entry.level === 'CRITICAL'` + message contenant `[Injection]` et `INTERROMPUE` (capté via le canal WS log existant `broadcastLog()`, pas de nouveau message WS) → toast rouge « Injection {pH|ORP/chlore} interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. »
- **Erreurs HTTP autres** : lecture du body texte et affichage si court (< 200 chars), sinon message générique.

### Documentation

- `docs/API.md` : nouveau code 409 documenté sur les 4 routes concernées, mention du nouveau bornage `kManualInjectMaxDurationS = 600 s`, encart sur l'arrêt cyclique automatique et l'alerte MQTT émise.
- `docs/MQTT.md` : ajout des deux types d'alertes `ph_injection_aborted` et `orp_injection_aborted` dans la table des alertes.
- `docs/subsystems/pump-controller.md` : nouvel encart « Garde filtration injection manuelle » dans la section sécurité chimique, ajout de `kManualInjectMaxDurationS` au tableau des constantes.
- `docs/features/page-ph.md` et `docs/features/page-orp.md` : section comportement UI mise à jour (toasts, refus, interruption), avertissement « injection manuelle non gardée » remplacé par la documentation de la nouvelle garde filtration.

### Notes

- Aucun ADR créé : c'est une mitigation d'un bug de sécurité (ajout d'une garde manquante). Pas de contrainte structurante, pas d'alternatives crédibles à arbitrer (`pool-chemistry` impose la garde), pas de verrouillage long terme.
- Pas de spec formelle — fix réactif sur bug observé en production. Le rapport `pool-chemistry` (GO sous conditions) tient lieu de cadrage.

---

## [2.1.1] - 2026-05-10

### Firmware

- **Détection corruption LittleFS au boot** (`3c8b657`) — `HistoryManager::begin()` force maintenant un test write/read sur fichier témoin `/.fscheck` après le mount LittleFS de la partition `history`. Si l'opération échoue (open / write / read / compare KO), la partition est effacée via `esp_partition_erase_range` puis remontée propre. Garde-fou contre un bug LittleFS connu : un FS corrompu peut être monté sans erreur puis crasher en `IntegerDivideByZero` à la 1ʳᵉ écriture (observé sur ESP32 PCB v1 après une longue inactivité). Logs : warning « Partition history corrompue détectée — reformatage automatique » + info « Partition history reformatée et remontée ». ⚠️ L'historique persistant est perdu si le reformatage est déclenché. Coût négligeable en cas nominal (~10 ms, ~500 B flash).
- **Underflow uint32 dans la garde 24 h Slope query** (`933f17c`, feature-024) — `SensorManager::update()` étape 5 calculait `(now - _phSlopeQueriedMs)` avec `now` figé en début de fonction. Après `_processEzoQueue()` qui peut bloquer ~900 ms, le handler met `_phSlopeQueriedMs = millis()` à un instant postérieur à `now` → underflow uint32 → ~4,3 milliards → toujours ≥ 86 400 000 → ré-enqueue immédiat → spam de query `Slope,?` ~1/s, monopolisation du mutex I²C, EZO ORP perturbé (logs « bus I²C dégradé » récurrents). Fix : recalcul de `now` après `_processEzoQueue()` + garde anti-underflow `nowAfterQueue >= _phSlopeQueriedMs`.
- **EZO ORP utilise `R` au lieu de `RT,<temp>`** (`c0f2962`) — `AtlasEzoSensor::readSingle()` envoyait indistinctement `RT,<temp>` aux deux modules Atlas. Sur l'EZO pH (0x63), cette commande compense la T° (Nernst) ET retourne la valeur compensée (statut 1 + payload). Sur l'EZO ORP (0x62), elle est ACCEPTÉE (statut 1 success) mais NE RETOURNE PAS de payload — l'ORP est potentiométrique direct sans compensation T°. Le firmware attendait un payload qui ne venait jamais → fail streak → bus I²C dégradé après 2 cycles → régulation ORP inhibée. Fix : différenciation par adresse I²C — `kEzoPhAddress` → `RT,<temp>`, `kEzoOrpAddress` → `R`. Validation empirique via `/debug/ezo_command` : `0x62 RT,25.0 → status=1 resp=""` vs `0x62 R → status=1 resp="-369.2"`.
- **Rate limit 30 → 120 req/min** (`6c79cfc`) — `kMaxRequestsPerMinute` passé de 30 à 120 (`src/constants.h`). Ancienne valeur trop basse pour navigation UI active normale (page /params ouverte + polls `/get-config` + `/data` + `/coredump/info` + clics → 30-45 req/min observés → warnings « Rate limit dépassé » fréquents). 120 req/min = 2 req/s moyenne reste une protection efficace contre brute-force auth (des années pour craquer un mot de passe 8 caractères) et DOS accidentel. WebSocket `/ws` non comptabilisé (connexion permanente).

### Fonctionnalités

- **Outil diagnostic EZO + endpoint factory reset** (`20e4a9b`) — 2 nouveaux endpoints HTTP (pas d'auth, cohérent avec les autres `/debug/*`) :
  - `POST /debug/ezo_command` — envoie une commande Atlas EZO arbitraire (1-30 chars) à n'importe quelle adresse I²C (`8..119` décimal = `0x08..0x77`) avec délai d'attente paramétrable (50-5000 ms). Retourne `{success, addr, cmd, status_code, status_label, response, raw_hex, delay_ms}`. Permet de diagnostiquer un module qui ne répond pas comme attendu, valider une réponse vide silencieuse, préparer un RMA Atlas avec preuves reproductibles.
  - `POST /debug/ezo_factory?addr=N` — restaure les paramètres usine d'un module Atlas EZO (calibration effacée, adresse I²C par défaut, baud rate UART par défaut, compensation T° reset). Le mode de communication (I²C vs UART) est PRÉSERVÉ — la commande ne touche pas le firmware EZO. Power-cycle ESP32 + recalibration recommandés après usage.
- **Carte UI « Diagnostic EZO »** dans Paramètres → Avancé (sous « Debug oscillation pH ») : sélecteur module (ORP 0x62 / pH 0x63), 10 boutons préprogrammés (`I`, `Status`, `R`, `Cal,?`, `Slope,?`, `L,?`, `L,1`, `L,0`, `Plock,?`, `Find`), champ commande personnalisée (max 30 chars) + délai paramétrable (50-5000 ms), affichage parsé (status code coloré vert/rouge selon code, réponse texte, bytes hex), historique scrollable des 30 dernières commandes (timestamp + cmd + status + réponse). Touche Entrée dans le champ perso → envoie.

### Documentation

- `docs/API.md` : nouvelle section « Outil diagnostic EZO » avec les 2 endpoints `/debug/ezo_command` et `/debug/ezo_factory`, exemples curl complets, table des status codes Atlas.
- `docs/features/page-settings.md` : nouvelle sous-section « Card Diagnostic EZO » sous le panel Avancé.
- `docs/subsystems/sensors.md` : section `readSingle()` mise à jour pour la différenciation pH/ORP, encart sur la garde 24 h `Slope,?` et l'anti-underflow `nowAfterQueue`.
- `docs/subsystems/history.md` : nouvelle sous-section « Détection de corruption LittleFS au boot » détaillant le test `/.fscheck` et la procédure de reformatage automatique.

### Notes

- Aucun ADR créé : les 5 commits sont des mitigations de bugs externes (LittleFS, firmware Atlas EZO), un outil de debug et un ajustement de seuil empirique. Pas de décision structurante avec alternative crédible à arbitrer.
- Build OK, validé en condition réelle avant publication.

---

## [2.1.0] - 2026-05-08

### Firmware
- **Pente sonde pH (feature-024)** — exposition des indicateurs d'usure de la sonde pH Atlas EZO via la commande `Slope,?`. Cache RAM `_phSlopeAcid/Base/Zero/QueriedMs` dans `Sensors`, getters publics `getPhSlopeAcid/Base/Zero()` + `getPhSlopeAgeMs()` + `enqueuePhSlopeQuery()`. Refresh automatique au boot, après chaque calibration EZO réussie (mid/low/clear) et toutes les 24 h (`kPhSlopeQueryIntervalMs = 86_400_000` ms). Dédoublonnage via flag `_phSlopeQueryPending` (anti-spam queue 4 slots). Invalidation cache à NaN après ≥ 2 échecs consécutifs (cohérent avec `_phCalCachedPoints`).
- **`AtlasEzoSensor::querySlope(PhSlopeInfo&)`** — parsing tolérant 2 ou 3 floats sur la réponse `?Slope,<acide>,<base>[,<zéro>]`. Mutex I²C tenu pendant toute la séquence (cmd + delay + read).
- **Feature strictement diagnostique** : aucun impact sur `canDose()` ni le PID. Validation `pool-chemistry` skip (passive).

### API HTTP / WebSocket
- **Nouvel endpoint** : `POST /debug/ph_slope_refresh` (sans auth, cohérent avec autres `/debug/*`) — force une re-query `Slope,?` immédiate. Réponse 200 `{success:true, queued:true}` ou 503 `{error:"queue full or already pending"}`.
- **Champs WS ajoutés à `sensor_data`** (4 champs, `null` si jamais lus / bus dégradé) : `phSlopeAcid` (% 1 décimale), `phSlopeBase` (% 1 décimale), `phSlopeZero` (mV 2 décimales), `phSlopeAgeMs` (entier ms ou `null`). Buffer WS bumpé à 1024 octets (+80).

### MQTT / Home Assistant
- **3 nouveaux topics** retain edge-triggered : `{base}/ph_slope_acid` (1 décimale), `{base}/ph_slope_base` (1 décimale), `{base}/ph_slope_zero` (2 décimales). Publication uniquement à la transition de la valeur arrondie — pas de spam.
- **3 sensors auto-discovery HA** : « Piscine pH Pente Acide » (`mdi:angle-acute`, unit `%`), « Piscine pH Pente Base » (`mdi:angle-obtuse`, unit `%`), « Piscine pH Décalage Zéro » (`mdi:sine-wave`, unit `mV`), tous avec `state_class: measurement`.
- Pas de `binary_sensor` « à remplacer » côté firmware — l'utilisateur peut le créer en automation HA selon ses propres seuils.

### Frontend
- **Chip d'état sonde pH** sur la carte « Lecture pH » (page `/ph`) — bouton focusable sous l'affichage pH 3 décimales. 5 variantes CSS (`chip--probe-good/warn/warn2/bad/unknown`) + classe `chip--probe-stale` (encadré jaune si âge > 36 h). Évaluation dans `data/app.js` (fonction `classifyPhProbe()`, constantes `PH_PROBE_*`) — seuils ajustables sans reflasher.
- **Modal détails** (`<dialog id="ph-probe-modal">`) au clic du chip : pente acide, pente base, décalage zéro, âge dernière vérif + bouton « Rafraîchir » → `POST /debug/ph_slope_refresh` (timeout 8 s, attente que `phSlopeAgeMs` redescende sous 60 s). Fallback non-cliquable si `<dialog>` non supporté (warning console).

### Fixes (correctifs code-reviewer feature-024)
- **Anti-spam HA** : trace brute `Slope,?` passée en `debug` (au lieu de `warning`) — la query auto 24 h faisait remonter du bruit dans le topic `{base}/logs` consommé par HA.
- **Cohérence WS** : `phSlopeAgeMs` désormais `null` si jamais lu (au lieu de `UINT32_MAX`), aligné avec les `phSlope*` nullables.
- **Icônes HA discriminantes** : `mdi:angle-acute` / `mdi:angle-obtuse` / `mdi:sine-wave` (au lieu de `mdi:chart-line` générique partagé).

### Documentation
- `docs/subsystems/sensors.md` : nouvelle section « Pente sonde pH — feature-024 » (méthode `querySlope()`, cache, refresh policy, invalidation), ajout `kPhSlopeQueryIntervalMs` au tableau des constantes, mention dans la liste des consommateurs WS / MQTT.
- `docs/MQTT.md` : nouvelle section « Topics et entités ajoutés en feature-024 » (3 topics + 3 sensors HA), ajout des 3 lignes dans le tableau topics capteurs.
- `docs/API.md` : nouvelle section « Endpoints ajoutés en feature-024 » (`POST /debug/ph_slope_refresh`), nouveaux champs WS dans le tableau payload `sensor_data`.
- `docs/features/page-ph.md` : nouvelle section « Chip d'état sonde » (placement, classification, modal, états stale, fallback `<dialog>`), ajout dans la table Actions et Interaction MQTT.
- Spec `specs/features/done/feature-024-pente-sonde-ph.md` (déplacée depuis `doing/`) avec section « Notes d'implémentation ».

### Build
- `pio run` SUCCESS, **RAM 18.0 %**, **Flash 99.8 %** (marge ~2.6 KB — point d'attention : prochaine feature potentiellement bloquée si pas de gain). `./build_fs.sh` SUCCESS, FS 1.1 MB.

### Risques résiduels
- Format de réponse `Slope,?` à confirmer empiriquement avec EZO réel — tolérance 2 ou 3 floats déjà en place pour les firmwares anciens.

---

## [2.0.0] - 2026-05-06

### Hardware
- **Migration PCB v2 — chaîne pH/ORP refondue sur Atlas Scientific EZO Embedded I²C (feature-021)**. Suppression matérielle de l'ADS1115 et de la sonde pH analogique. Modules EZO pH (`0x63`) et EZO ORP (`0x62`) sur le bus I²C partagé avec le DS3231. Voir [ADR-0014](docs/adr/0014-migration-atlas-ezo.md) — supersedes [ADR-0001](docs/adr/0001-capteurs-analogiques-ads1115.md).

### Firmware
- **Refonte complète chaîne pH/ORP** (`src/sensors.h/cpp`, `src/atlas_ezo.h/cpp`). Mini-classe maison `AtlasEzoSensor` (~80 lignes) encapsulant les commandes Atlas (`R`, `RT,<t>`, `Cal,*`, `Cal,?`, `Cal,clear`, `I`) et les délais EZO (600/900 ms). Mutex I²C tenu pendant la séquence atomique `RT,<temp>` + delay + `R` + delay (cond #6 pool-chemistry).
- **Queue FreeRTOS `_ezoQueue`** (4 slots) pour exécuter les calibrations EZO (~900 ms bloquantes) hors handlers HTTP. Pattern miroir de `mqttTask` ([ADR-0011](docs/adr/0011-mqtt-task-dediee.md)). Routes `/calibrate_*` retournent `{success:true, queued:true}` immédiatement.
- **`canDose(int pumpIndex)` refondu — 10 garde-fous fail-closed** dans l'ordre : index pompe valide, watchdog actif, filtration en marche, lecture pH/ORP non NaN (cond #1 stale 20 s + cond #5 bus dégradé), EZO calibré (cond #2, `cal_points >= 2` pH / `>= 1` ORP, `-1` bloque), pas de stabilisation post-cal en cours (cond #3), mode régulation = `automatic`, limite journalière, limite horaire, anti-rafale court terme (`kMaxDosingCyclesPerMinute=6` + `kMaxDosingCyclesPer15Min=20` via ring buffer 20 entrées par pompe — correctif Pass 3.5). Log « edge-triggered » : 1 entrée par transition de cause de refus.
- **Stabilisation post-calibration par pompe** : `armStabilizationTimer(int pumpIndex)` avec durée différenciée — `kStabilizationDurationPhMs = 5 min`, `kStabilizationDurationOrpMs = 3 min`. Surcharge legacy `armStabilizationTimer()` conservée pour les sites « globaux » (filtration, boot continu, rollover minuit) — applique aux 2 pompes simultanément avec `mqttCfg.stabilizationDelayMin`.
- **Stale timeout** `kSensorStaleTimeoutMs = 20000` ms : `getPh()`/`getOrp()` retournent NaN si dernière lecture > 20 s. Logger `critical` 1× à la transition. Alerte MQTT `pool/alerts/sensor_stale` edge-triggered.
- **Cache `_phCalCachedPoints` / `_orpCalCachedPoints`** mis à jour en `begin()` puis après chaque calibration. Invalidé à `-1` si `_phI2cFailStreak >= kEzoBusFailMaxConsecutive = 2` (durcissement Pass 3.5) → `_lastPh = NaN` ET cache à `-1` simultanément. Rafraîchissement opportuniste à la 1ʳᵉ lecture réussie suivante.
- **Compensation T° pH** : `RT,<temp>` envoyé avant chaque `R`. Source : `getWaterTemperature()` (feature-020). Fallback **25.0 °C** si NaN (sonde eau non identifiée ou en erreur).
- **Suppression** : libs PlatformIO `Adafruit ADS1X15` et `DFRobot_PH` retirées de `lib_deps`. 10 fonctions publiques `Sensors` legacy supprimées (`getRawPh`, `getRawOrp`, `getPhVoltageMv`, `isPhCalibrated`, `getRawTemperature`, `calibratePhNeutral/Acid/Alkaline`, `clearPhCalibration`, `detectAdsIfNeeded`, `recalculateCalibratedValues`, `publishValues`). 7 champs `MqttConfig` ORP/pH legacy supprimés (`orp_cal_*`, `ph_cal_*`).

### API HTTP / WebSocket
- **Nouvelles routes** `POST /calibrate_ph {step:"mid"|"low"}`, `POST /calibrate_orp {reference:<0..1000 mV>}`, `POST /calibrate_clear {sensor:"ph"|"orp"}`. Toutes répondent immédiatement `{success:true, queued:true}` (< 1 ms). 400 si payload invalide, 503 si queue saturée.
- **Routes supprimées** (404 désormais) : `POST /calibrate_ph_neutral`, `POST /calibrate_ph_acid`, `POST /clear_ph_calibration`.
- **Précision pH** : 3 décimales sur tous les contrats publics (`GET /data`, WS `sensor_data`, MQTT `{base}/ph`). Avant : 1 décimale.
- **Champs WS ajoutés** : `phCalPoints` (int `-1..3`), `orpCalPoints` (int `-1..1`).
- **Champs WS supprimés** : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw` (la notion de « valeur brute » n'existe plus côté EZO).

### MQTT / Home Assistant
- **Nouveaux topics** (retain) : `{base}/ph_cal_points`, `{base}/orp_cal_points`, `{base}/alerts/calibration_required` (edge-triggered, payload JSON ou vide=clear), `{base}/alerts/sensor_stale` (idem).
- **Auto-discovery HA enrichi** : 2 nouveaux sensors « Piscine pH Points Calibrés » (`unique_id: poolcontroller_ph_cal_points`, `icon: mdi:numeric`) et « Piscine ORP Points Calibrés ».
- **`{base}/ph`** publié avec **3 décimales** (vs 1 décimale en v1.x). Tout consommateur HA qui parsait `int()` doit basculer sur `float()`.

### Frontend
- **Refonte carte calibration pH** : 2 sous-blocs **parallèles** `.cal-point-block` (point milieu pH 7.00 + point bas pH 4.00). Plus de stepper séquentiel. Chaque bloc : badge état, micro-étapes, readout live, bouton « Calibrer le point X.X ». Workflow : POST `/calibrate_ph` → toast « Calibration en cours… » → polling 15 s sur `phCalPoints` → toast succès. Lectures pH 3 décimales sur les readouts.
- **Refonte carte calibration ORP** : 1 sous-bloc `.cal-point-block` unique avec input `orp-cal-reference` (`0..1000` mV, défaut 470). Suppression du sélecteur 1pt/2pts hérité du firmware v1. Polling 15 s avec fallback succès si `orpCalPoints >= 1` (recalibration ne change pas le compteur).
- **Cartes Régulation pH / ORP** : callout vert « Calibré N points ✓ » si calibration nominale, chip ambrée d'inhibition si `phCalPoints < 2` ou `orpCalPoints < 1`, chip rouge « EZO injoignable » si compteur à `-1`.
- **Suppression** : chip tension pH legacy de la page pH, sélecteur 1pt/2pts ORP.

### Sécurité chimique
- 6 conditions impératives `pool-chemistry` intégrées (#1 stale, #2 cal_points, #3 stabilisation, #4 alerte MQTT, #5 bus I²C dégradé, #6 mutex atomique).
- 2 correctifs additionnels Pass 3.5 : invalidation simultanée cache cal_points + lecture en mode bus dégradé ; ring buffer anti-rafale 6/min + 20/15min.

### Documentation
- **Nouveau** : [`docs/adr/0014-migration-atlas-ezo.md`](docs/adr/0014-migration-atlas-ezo.md) — décision, alternatives écartées (lib EZO_pH, ADS1115 + filtre amélioré, chaîne mixte, tâche FreeRTOS dédiée, exécution synchrone), conséquences, ce que ça verrouille.
- **Annoté Superseded** : [`docs/adr/0001-capteurs-analogiques-ads1115.md`](docs/adr/0001-capteurs-analogiques-ads1115.md) — bandeau « Superseded by ADR-0014 ».
- Mis à jour : [`docs/subsystems/sensors.md`](docs/subsystems/sensors.md) (refonte majeure : architecture EZO, mini-classe `AtlasEzoSensor`, queue, cache cal_points, stale, mutex), [`docs/subsystems/pump-controller.md`](docs/subsystems/pump-controller.md) (10 garde-fous `canDose`, ring buffer anti-rafale, stabilisation par pompe, constantes ajoutées au tableau), [`docs/MQTT.md`](docs/MQTT.md) (4 nouveaux topics + 2 sensors HA), [`docs/API.md`](docs/API.md) (3 routes refondues, payloads `/data` et WS, champs `phCalPoints`/`orpCalPoints`), [`docs/features/page-ph.md`](docs/features/page-ph.md) (refonte UI 2 sous-blocs), [`docs/features/page-orp.md`](docs/features/page-orp.md) (refonte UI 1 bloc), [`docs/BUILD.md`](docs/BUILD.md) (suppression libs ADS1115/DFRobot_PH des `lib_deps`), [`docs/UPDATE_GUIDE.md`](docs/UPDATE_GUIDE.md) (procédure recalibration obligatoire v1.x → v2.0.0), [`docs/adr/README.md`](docs/adr/README.md) (index : ADR-0001 superseded, ADR-0014 ajouté).

### Migration utilisateur — v1.x → v2.0.0 (BREAKING)
- **Recalibration obligatoire** : pH 2 points (mid 7.00 + low 4.00) + ORP 1 point (kit 225 mV ou 470 mV). Tant que ce n'est pas fait, régulation auto inhibée + alerte MQTT `pool/alerts/calibration_required` retain.
- Données NVS legacy (`ph_cal_*`, `orp_cal_*`) supprimées silencieusement au 1ᵉʳ boot — aucune migration possible (chaîne de mesure totalement différente).
- Firmware v2.0.0 conçu pour PCB v2 uniquement. **NE PAS** flasher sur PCB v1 (sondes incompatibles).

### Build
- `pio run` SUCCESS, RAM 16.5 %, **Flash 98.8 %** (marge ~17 KB — point d'attention pour les futures features), 0 nouveau warning. `./build_fs.sh` SUCCESS, FS 1.1 MB.

---

## [Unreleased] - 2026-05-06

### Hardware/Firmware (PCB v2)
- **Support 2 sondes DS18B20 (eau piscine + circuit électronique) avec identification (feature-020)**. Le PCB v2 ajoute une 2ᵉ sonde DS18B20 sur le bus OneWire (GPIO 5 partagé). Identification par adresse ROM persistée NVS via workflow utilisateur (chauffer une sonde dans la main 30 s, cliquer le bon bouton dans Paramètres → Avancé). Auto-permutation activée : si l'utilisateur identifie la sonde A comme « eau » alors qu'une autre était déjà eau, cette dernière bascule automatiquement à « circuit ». Fallback gracieux : `Sensors::getTemperature()` reste un alias de `getWaterTemperature()` avec repli sur la 1ʳᵉ sonde détectée si non identifié, garantissant la rétrocompat MQTT/WS/HA. Voir [ADR-0013](docs/adr/0013-identification-sondes-onewire.md).

### Frontend
- Nouvelle card « Identification des sondes de température » dans Paramètres → Avancé (affichage temps réel adresses ROM + T° brutes, boutons d'assignation, badge état une fois identifiée, bouton réinitialiser). Polling 2 s scoped au panel-dev.
- Chip de notification Dashboard ambré (pattern `.chip` existant) tant que `sondes_identified === false && sondes_detected >= 1`. Clic → redirige vers la card.

### MQTT/HA
- Nouveau topic `pool/sensors/temperature_circuit` (retain) + entité auto-discovery HA « Piscine Température Circuit ». Topic `pool/sensors/temperature` (eau) inchangé (rétrocompat).

### API HTTP / WebSocket
- 3 nouveaux endpoints (auth) : `GET /sensors/onewire/scan`, `POST /sensors/onewire/identify`, `POST /sensors/onewire/reset`.
- 3 nouveaux champs WS `sensor_data` : `temperature_circuit`, `sondes_identified`, `sondes_detected`. Buffer 832 → 896 octets.

### Documentation
- `docs/adr/0013-identification-sondes-onewire.md` créé. ADR-README mis à jour.
- `docs/subsystems/sensors.md`, `docs/subsystems/ws-manager.md`, `docs/MQTT.md`, `docs/API.md`, `docs/features/page-settings.md`, `docs/UPDATE_GUIDE.md` mis à jour.

### Build
- `pio run` SUCCESS, RAM 16.5 %, **Flash 98.5 %** (marge ~21 KB — point d'attention pour feature-021 EZO), 0 nouveau warning. `./build_fs.sh` SUCCESS.

---

## [Unreleased] - 2026-05-05

### BREAKING / Hardware
- **Bascule cible PCB v1 → PCB v2 (feature-019)**. Le firmware n'est plus compatible avec le PCB v1. Mapping GPIO entièrement réassigné : 10 pins actifs (LED=2, OneWire DS18B20=5, I²C SDA=21, I²C SCL=22, pompe pH=25, relais filtration=26, relais éclairage=27, pompe ORP=33, bouton factory reset=35) + 3 pins réservés feature-future (`kRtcSqwPin=23`, `kCtnAuxPin=32` MOSFET 12V tableau, `kRtcIntPin=36`). L'ADS1115 et ses pins associés sont supprimés (les modules Atlas EZO pH/ORP les remplaceront en feature-020). La 2ᵉ sonde DS18B20 est câblée sur le bus OneWire mais ne sera détectée qu'en feature-021. Ce changement est unidirectionnel : la version cible 2.0.0 ne fonctionnera plus sur PCB v1. Voir [ADR-0012](docs/adr/0012-mapping-gpio-pcb-v2.md) pour la justification complète des choix de pin et les alternatives écartées

### Firmware
- **Migration des `#define` de pin vers `constexpr kXxxPin`**. Suppression de 7 `#define` historiques de `src/config.h` (`PUMP1_PWM_PIN`, `PUMP2_PWM_PIN`, `TEMP_SENSOR_PIN`, `FILTRATION_RELAY_PIN`, `LIGHTING_RELAY_PIN`, `BUILTIN_LED_PIN`, `FACTORY_RESET_BUTTON_PIN`) au profit de constantes typées `constexpr uint8_t kXxxPin` regroupées en tête de `src/constants.h` sous une section dédiée « GPIO PIN ASSIGNMENTS - PCB v2 ». Cohérent avec la convention CLAUDE.md (`kConstantes` dans `constants.h`, pas de `#define` dispersés)
  - **Logique du bouton factory reset inversée** : `pinMode(kFactoryResetButtonPin, INPUT)` (au lieu de `INPUT_PULLDOWN` interne v1) car GPIO 35 est input-only et ne supporte pas de pull-up/pull-down interne — un pull-up externe 10 kΩ vers 3V3 sur le PCB v2 est obligatoire. Lecture firmware passée à `pressed = digitalRead(kFactoryResetButtonPin) == LOW` (au lieu de `== HIGH` v1). **Comportement utilisateur identique** : appui maintenu 10 s = factory reset
  - **Convention `pumps[0]` = pH (kPumpPhPin=25), `pumps[1]` = ORP (kPumpOrpPin=33)** figée dans `pump_controller.cpp` `begin()`. Inversion = bug de sécurité chimique (mauvaise pompe activée)
  - **Pins réservés `kRtcSqwPin`, `kCtnAuxPin`, `kRtcIntPin`** déclarés mais sans `pinMode` initial — restent en haute impédance jusqu'à activation par une feature future (économie de courant pull-up, détection facile d'un court-circuit hardware)
  - 7 fichiers touchés : `src/constants.h`, `src/config.h`, `src/pump_controller.cpp`, `src/filtration.cpp`, `src/lighting.cpp`, `src/sensors.cpp`, `src/main.cpp`
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning
  - Tests dynamiques (boot, voltmètre relais, bouton 10 s) délégués à l'humain sur PCB v2 réel — le PCB v1 ne peut plus être testé fonctionnellement

### Documentation
- `docs/adr/0012-mapping-gpio-pcb-v2.md` créé — décision complète, tableau du mapping (13 pins), alternatives écartées (mapping v1 conservé / bouton sur pin avec pull-up interne / bus I²C séparé pour EZO / `pinMode(INPUT_PULLUP)` activé immédiatement sur les pins réservés / `#define` ré-injecté dans `config.h`), conséquences positives et dette assumée, ce que la décision verrouille
- `docs/adr/README.md` : index mis à jour (entrées ADR-0011 et ADR-0012 ajoutées)
- `docs/subsystems/pump-controller.md` : nouvelle section « Mapping pompes ↔ pins (PCB v2) » documentant la convention `pumps[0]`=pH/`pumps[1]`=ORP avec référence ADR-0012 et feature-019
- `docs/subsystems/sensors.md` : tableau Matériel mis à jour (pin OneWire `kTempSensorPin=5` cité, mention du bus partagé avec la 2ᵉ sonde DS18B20 à activer en feature-021), note sur la suppression de l'ADS1115 au profit des Atlas EZO (feature-020)
- `docs/subsystems/filtration.md` : `kFiltrationRelayPin = 26` (au lieu de `FILTRATION_RELAY_PIN = 25` v1)
- `docs/subsystems/lighting.md` : `kLightingRelayPin = 27` (au lieu de `LIGHTING_RELAY_PIN = 26` v1)

---

## [Unreleased] - 2026-04-30

### Firmware
- **MQTT — `requestReconnect()` n'est plus déclenché après un save de paramètres non-MQTT (feature-018)**. Évite la republication des 17 messages discovery HA et la transition transitoire « Déconnecté » du badge UI lors d'un save de modes de régulation, NTP, logs DEBUG, écran LVGL, calibrations, etc. Le handler `POST /save-config` snapshotte désormais les 6 champs MQTT (`server`, `port`, `topic`, `username`, `password`, `enabled`) après prise du `configMutex` et avant parsing JSON, puis compare après application du payload : `requestReconnect()` n'est appelé que si au moins un champ a réellement changé. Nouveau log INFO FR `"MQTT reconnect demandé (config MQTT modifiée)"` pour tracer les reconnects légitimes. Comportement déclenché par feature-015 IT1bis qui a rendu visibles ces transitions transitoires (badge MQTT en temps réel via WS) — la session MQTT était coupée puis rétablie en ~1 s à chaque save UI quel qu'il soit. Contrat externe `POST /save-config` strictement inchangé (mêmes payloads, mêmes réponses)
  - **`src/web_routes_config.cpp`** : snapshot des 6 champs + booléen `mqttChanged` + appel conditionnel `mqttManager.requestReconnect()` (+24/-1 lignes)
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : nouvelle sous-section « Déclenchement conditionnel post-`POST /save-config` (feature-018) » sous « Reconnexion » — tableau des 6 champs MQTT comparés, mécanique du snapshot/comparaison/appel conditionnel, bénéfice (plus de re-discovery HA inutile, badge UI stable), cas `enabled` true → false

### Firmware
- **Toggle pour activer/désactiver les logs DEBUG (firmware + UI)** — `Logger::debug()` est désormais conditionné par `authCfg.debugLogsEnabled` (default `false`, persisté en NVS sous la clé `debug_logs`). Quand le toggle est désactivé, la fonction effectue un early return en première ligne — aucune allocation `String`, aucun lock mutex, aucun push WebSocket, aucune écriture buffer. Default désactivé pour alléger le buffer logs (200 entrées max) en production. Pattern miroir de `sensorLogsEnabled` (« Log des sondes ») introduit précédemment. Les niveaux `INFO`, `WARN`, `ERROR`, `CRITICAL` ne sont **pas** affectés. Effet immédiat (pas de redémarrage requis), persistance NVS au reboot
  - **`src/config.h`** : `bool debugLogsEnabled = false;` ajouté dans `AuthConfig`
  - **`src/config.cpp`** : write/read NVS clé `debug_logs` (default `false`) dans `saveAuthConfig()` / `loadAuthConfig()`
  - **`src/logger.cpp`** : `#include "config.h"` + early return en TOUTE PREMIÈRE LIGNE de `Logger::debug()`. Autres niveaux non touchés
  - **`src/web_routes_config.cpp`** : champ `debug_logs_enabled` exposé dans `/get-config` et accepté dans `/save-config` avec log INFO « Logs DEBUG: activés/désactivés »
  - **`src/ws_manager.cpp`** : champ `debug_logs_enabled` ajouté dans la payload WS `config` (snapshot config push WS broadcast)
  - Le filtre UI `#log_level_debug` reste indépendant (filtre d'affichage seulement, ne pilote pas la production firmware)
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Frontend
- **Nouveau switch « Logs DEBUG activés »** dans Paramètres → Avancé → card Logs, placé immédiatement sous « Log des sondes ». Description en français : « Active les messages de diagnostic détaillé. À activer uniquement pour le diagnostic. ». Lecture/écriture via `applyConfig()` (avec `=== true` pour default `false`) et `collectAuthConfig()`, autosave via event listener (pattern miroir de `sensor_logs_enabled`)
  - **`data/index.html`** : nouveau switch `#debug_logs_enabled` dans la card Logs du panneau Avancé
  - **`data/app.js`** : lecture, écriture et listener autosave

### Documentation
- `docs/features/page-settings.md` : section card Logs étoffée — nouveau toggle « Logs DEBUG activés » documenté sous « Log des sondes » (default `false`, effet immédiat, persistance NVS, indépendance du filtre UI `#log_level_debug`)
- `docs/subsystems/logger.md` : nouvelle section « Toggle DEBUG runtime » documentant l'early return dans `Logger::debug()`, la clé NVS `debug_logs`, le default `false`, l'absence d'impact sur `info/warning/error/critical`, et la complémentarité avec le filtre UI
- `docs/API.md` : champs `sensor_logs_enabled` et `debug_logs_enabled` ajoutés dans le payload de `/get-config` et la table des champs notables (validés dans `/save-config`)
- `docs/UPDATE_GUIDE.md` : nouvelle note de migration « Logs DEBUG désactivés par défaut — depuis 2026-04-30 » expliquant le comportement par défaut et la procédure d'activation via Paramètres → Avancé

### Firmware
- **IT5 — MQTT — fix déconnexions `exceeded timeout` Mosquitto** : remplacement du mode non-bloquant `O_NONBLOCK` (IT4) par un timeout d'écriture borné `SO_SNDTIMEO=500 ms` posé via `setsockopt()` après chaque `mqtt.connect()` réussi. Le PINGREQ keepalive PubSubClient (2 octets toutes les 60 s) part désormais de manière fiable même quand un publish concurrent occupe le send buffer TCP — avant IT5, un `lwip_send()` qui retournait `EAGAIN` instantanément faisait perdre silencieusement le PINGREQ (PubSubClient n'audite pas le retour de `_client->write` pour le PINGREQ), et Mosquitto coupait la session après 90 s sans paquet reçu. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 5 »
  - **Nouvelle constante** `kMqttSocketSendTimeoutMs = 500` (ms) dans `src/constants.h` avec commentaire d'unité explicite et référence ADR-0011 IT5
  - **`src/mqtt_manager.cpp` `connectInTask()`** : `fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)` remplacé par `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))` avec `tv = {0, 500_000 µs}` ; include `<fcntl.h>` retiré, `<lwip/sockets.h>` ajouté
  - **Wrapper `safePublish()` inchangé runtime** : commentaire d'en-tête mis à jour pour refléter le nouveau mécanisme (`mqtt.publish()` borné à 500 ms par appel via `SO_SNDTIMEO`, plus de retour immédiat `EAGAIN`)
  - **Trade-off** : un publish lent peut prendre jusqu'à 500 ms (vs retour immédiat IT4 sur send buffer plein). Pire cas `publishDiscovery` (17 publishes enchaînés) = 8.5 s, sous le watchdog 30 s avec marge. Imperceptible utilisateur sur LAN sain
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : section « Garde-fou » renommée « `safePublish()` + socket avec `SO_SNDTIMEO` (IT5, remplace O_NONBLOCK d'IT4) » avec snippet `setsockopt`, explication du side-effect IT4 sur le PINGREQ keepalive et son fix IT5 ; tableau des paramètres tâche enrichi de `kMqttSocketSendTimeoutMs` ; sections « Keepalive », « Bornage TCP côté lwip », « Bascule de dominance entre `-3` et `-4` » et tableaux mis à jour pour refléter le timeout socket borné IT5 au lieu du non-bloquant IT4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 5 — 2026-04-30 » dans « Évolutions » détaillant la cause racine (PINGREQ silencieusement perdu en `O_NONBLOCK`), le fix `SO_SNDTIMEO=500 ms`, les fixes F17–F20, le trade-off et les tests dynamiques restants. La décision principale de l'ADR (tâche dédiée) reste retenue
- `specs/features/done/feature-014-mqtt-task-dediee.md` : statut passé à `done`, version cible 1.0.5, itération 5 marquée livrée (build vert + revue OK ; AC-IT5-3 et AC-IT5-4 délégués à l'humain post-flash)

---

## [Unreleased] - 2026-04-29

### Frontend
- **UI cards — placement uniformisé des badges d'état (feature-001)** : uniformisation du placement des badges d'état (Marche/Arrêt, Allumé/Éteint, Connecté/Déconnecté) à droite du titre dans le `card__head` des cards Filtration « Contrôle manuel », Éclairage « Contrôle manuel » et MQTT (Paramètres). Suppression des lignes « État actuel » redondantes dans le body des cards Filtration et Éclairage. Cohérence visuelle inter-pages, gain de place vertical. Côté JS, `updateFiltrationBadges()` et `updateLightingStatus()` ont été splittés (page détail = `pill ok/bad/mid` ; dashboard `card--status` = `state-badge--*` inchangé), `getFiltrationState()` expose désormais `pillClass` (mapping `warn → mid`). Règle CSS de garde `.card__head .pill { flex-shrink: 0; white-space: nowrap; }`. Aucun impact sur le dashboard ni sur les autres cards (Wi-Fi, Heure, Sécurité, Régulation, Calibrations, Produits, Historique, Système).
- **Bug fix** : Badge MQTT (Paramètres → MQTT) — propagation fiabilisée vers l'UI : la mise à jour temps réel via WS s'applique désormais en tête de `_onWsSensorData` (blindée par try/catch) et un re-render explicite est déclenché au passage sur le panel MQTT. Corrige un cas où le badge restait à « Déconnecté » après reconnexion firmware sans switch d'onglet.
- **WebSocket** : badge statut MQTT (Paramètres → MQTT) mis à jour en temps réel via push WS toutes les 5 s, sans nécessité de reload page. Quand le broker devient injoignable (câble HA débranché, broker arrêté), le badge bascule sur « Déconnecté » en moins de 5 s suivant la détection firmware ; idem pour la reconnexion. Le champ `mqtt_connected` est désormais inclus dans la payload `sensor_data` (en plus du snapshot `config` déjà présent). Source : `mqttManager.isConnected()` (single source of truth `connectedAtomic` introduit par feature-014 IT2)

### Documentation
- `docs/features/page-filtration.md` : badge Marche/Arrêt désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateFiltrationBadges()` / `getFiltrationState()`
- `docs/features/page-lighting.md` : badge Allumé/Éteint désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateLightingStatus()`
- `docs/features/page-settings.md` : précision du placement DOM uniformisé du badge MQTT (frère direct du `<h2>` dans `card__head`) et de la règle CSS de garde
- `docs/subsystems/ws-manager.md` : nouveau champ `mqtt_connected` documenté dans `sensor_data` avec la précision du doublon volontaire vs `config` (canal temps réel 5 s vs snapshot stable à la transition)
- `docs/features/page-settings.md` : précision sur le comportement temps réel du badge MQTT (Paramètres → MQTT) — bascule en < 5 s sans reload

---

## [Unreleased] - 2026-04-29

### Firmware
- **IT4 — Wrapper `safePublish()` + socket non-bloquante** — corrige un nouveau PANIC watchdog observé au 3ᵉ re-test D2 humain APRÈS le flash IT3. Le point de blocage avait migré vers `drainOutQueue()` (publish ~110 octets `orp_limit`), fonction qui n'avait pas été instrumentée par F8/F9 d'IT3 (oubli). Découverte parallèle : `CONFIG_LWIP_TCP_MAXRTX=5` borne en réalité à ~93 s (et non ~10 s comme estimé en IT3) à cause de `TCP_RTO_INITIAL=3 s` × backoff exponentiel — le bornage seul ne protège pas `mqttTask`. Pivot architectural : socket TCP non-bloquante via `fcntl(F_SETFL, O_NONBLOCK)` après chaque `mqtt.connect()` réussi → tout `WiFiClient::write()` retourne immédiatement avec `EAGAIN` si send buffer plein, plus de blocage dans `lwip_select`. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 4 »
  - **Mode non-bloquant via `fcntl()`** : passage de la socket en `O_NONBLOCK` après chaque connect réussi dans `connectInTask()`, avant `subscribe()`. Includes `<fcntl.h>` et `<errno.h>` ajoutés
  - **Wrapper unique `safePublish(topic, payload, retain)`** : remplace les 24 `mqtt.publish()` directs dans `mqttTask` (status `online` au connect, `drainOutQueue`, 20 publishes de `publishAllStatesInternal`, `publishDiagnosticInternal`, 17 publishes de la lambda `publishConfig` dans `publishDiscovery`). Le wrapper fait `esp_task_wdt_reset()` puis check `mqtt.connected()` avant délégation à `mqtt.publish()`
  - **Suppression des garde-fous IT3 redondants** : ~50 lignes de `esp_task_wdt_reset()` et `if (!mqtt.connected()) return;` éparpillées dans `publishAllStatesInternal()` (20 resets + 5 bail-out) et la lambda `publishConfig` (1 bail-out) ont été supprimées — factorisées dans le wrapper, plus lisible
  - **F12 annulé** : la constante `kMqttPublishHeadersOverhead` envisagée pour un check `availableForWrite()` est **inutile** car `WiFiClient::availableForWrite()` retourne toujours 0 dans Arduino-ESP32 6.9.0 (méthode héritée de `Print::availableForWrite()` non override). Le mode non-bloquant remplace ce mécanisme
  - **Trade-off accepté** : drop silencieux des publish quand le send buffer TCP est plein. Les états retain sont republiés au cycle suivant (10 s), les alertes ponctuelles peuvent être perdues — c'était déjà le cas en mode bloquant pré-IT4 (timeout 30 s puis drop), IT4 rend le drop instantané

- **IT3 — Borne TCP write côté lwip + bail-out anticipé** — corrige un nouveau PANIC watchdog observé au re-test D2 humain APRÈS le flash IT2. Le point de blocage avait migré de `mqtt.connect()` (résolu en IT2) vers `mqtt.publish()` : coredump-5 confirme `mqttTask` bloquée dans `WiFiClient::write` → `lwip_select` lors d'un publish 33 octets ("OFF") sur send buffer TCP saturé. Le reset wdt par groupes de 5 publish d'IT2 ne suffisait pas. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 3 »
  - **`CONFIG_LWIP_TCP_MAXRTX=5` dans `platformio.ini`** (`build_flags`) : limite à 5 retransmissions TCP avant abandon de socket par lwip (~10 s cumulés au lieu de ~75 s avec la valeur par défaut 12). Trade-off : paramètre global à toute la pile lwip → impact aussi AsyncWebServer / OTA HTTP / NTP (abandon socket ~10 s vs ~75 s). Acceptable pour un firmware temps réel ; OTA en réseau très dégradé peut être avorté plus tôt et nécessiter un retry humain
  - **Cadence `esp_task_wdt_reset()` 1:1 dans `publishAllStatesInternal()`** : reset posé **avant chaque** `mqtt.publish()` (20 resets, vs 2 resets par groupes en IT2). Garantit qu'au pire un seul publish reste à l'intérieur de la fenêtre wdt 30 s
  - **Bail-out fail-fast** : 5 `if (!mqtt.connected()) return;` répartis dans `publishAllStatesInternal()` (tous les ~3-4 publish) + 1 bail-out en tête de la lambda `publishConfig` de `publishDiscovery()`. Dès que lwip ferme la socket (cf. `CONFIG_LWIP_TCP_MAXRTX=5`), les publish restants sont court-circuités au lieu d'enchaîner 14 erreurs de ~2 s chacune
  - **F7/F10 annulés** : `setsockopt(TCP_USER_TIMEOUT)` envisagé initialement pour borner par-socket (RFC 5482) — **non supporté par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). Le bornage TCP repose donc uniquement sur F6 (paramètre global lwip). Constante `kMqttTcpUserTimeoutMs` non créée

- **Durcissement watchdog `mqttTask` sur broker injoignable** — corrige un PANIC watchdog 30 s observé pendant le test D2 (câble Ethernet HA débranché 2–3 min) où `mqttTask` restait bloquée dans `WiFiClient::connect()` → `lwip_select` sans reset. La régulation pH/ORP, la filtration et les autres tâches n'étaient pas concernées (déjà isolées par ADR-0011), mais le PANIC du core 0 entraînait un reboot complet. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions »
  - **Timeout client TCP corrigé de 5000 s à 2 s** : `WiFiClient::setTimeout()` (Arduino-ESP32 6.9.0) attend des **secondes**, pas des ms — le code historique `setTimeout(5000)` programmait 5000 secondes (~83 min), bug latent qui rendait le timeout côté client TCP totalement inopérant. Nouvelle constante `kMqttClientConnectTimeoutSec = 2` dans `constants.h` avec commentaire d'unité explicite
  - **`esp_task_wdt_reset()` granulaire** : reset ajouté juste après le retour de `mqtt.connect()` (couvre le cas SYN TCP retransmis sur broker injoignable jusqu'à ~75 s avant abandon lwip), reset au milieu de `publishAllStatesInternal()` (≤ 5 publish entre 2 resets), reset après chaque publish individuel dans le helper `publishConfig` de `publishDiscovery()` (17 publishes auto-discovery)
  - **`connectedAtomic` single source of truth** : store canonique posé en début de `taskLoop()` à chaque tour, complété par les transitions explicites en `connectInTask()` (succès) et `disconnect()`. Suppression de 3 stores intermédiaires redondants qui créaient une fenêtre de divergence UI/WARN observée pendant D2 (UI affichait « Déconnecté » sans WARN dans les logs)
  - **Suppression de la race au boot** : `mqttManager.requestReconnect()` retiré de `setup()` dans `main.cpp` — `mqttTask` se connecte déjà toute seule au premier tour de `taskLoop()`. Élimine le double publish d'auto-discovery au démarrage (32 messages au lieu de 17)

### Documentation
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT après validation D2 humaine du 2026-04-29 14:12:43 — avec le mode non-bloquant IT4, l'état dominant lors d'une coupure réseau est `-4` (`MQTT_CONNECTION_TIMEOUT`, keepalive PubSubClient, ~60 s) au lieu de `-3` (`MQTT_CONNECTION_LOST`, abandon TCP lwip, 90–180 s). La dominance documentée pré-IT4 (`-3` majoritaire) est inversée. Bénéfice indirect : détection plus précoce et plus propre. Tableau « États observés selon le scénario », tableau des codes d'état et workflow troubleshooting mis à jour en conséquence ; D2 marqué VALIDÉ dans la spec avec logs de référence
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Garde-fou : `safePublish()` + socket non-bloquant (IT4) » (snippet du wrapper, `fcntl(F_SETFL, O_NONBLOCK)` après chaque connect, tableau des 24 call sites, trade-off drop silencieux) ; AVERTISSEMENT en tête de section sur `WiFiClient::availableForWrite()` qui retourne toujours 0 dans Arduino-ESP32 6.9.0 ; section « Watchdog dans `mqttTask` » mise à jour (les `esp_task_wdt_reset()` IT3 ont été factorisés dans `safePublish()`) ; section « Bornage TCP côté lwip » corrigée — `CONFIG_LWIP_TCP_MAXRTX=5` borne à ~93 s (RTO_INITIAL × backoff), pas 10 s. Le mode non-bloquant IT4 est le vrai mécanisme de protection ; section « Bail-out fail-fast pendant les salves de publish » mise à jour pour pointer vers le wrapper
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 4 — 2026-04-29 » dans « Évolutions » détaillant le pivot socket non-bloquante, le wrapper `safePublish()` sur 24 call sites, l'annulation de F12 (`availableForWrite()` cassé dans Arduino-ESP32 6.9.0), la découverte du bornage réel à ~93 s pour `MAXRTX=5`, le trade-off drop silencieux. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : section « Watchdog dans `mqttTask` » mise à jour (cadence 1:1 dans `publishAllStatesInternal()`, 5 bail-out répartis, rationnel IT3) ; section « Détection des déconnexions » étendue avec sous-section « Bail-out fail-fast pendant les salves de publish (IT3) » ; nouvelle section « Bornage TCP côté lwip » documentant `CONFIG_LWIP_TCP_MAXRTX=5` et son impact global (AsyncWebServer / OTA / NTP) avec mention de l'absence de support `TCP_USER_TIMEOUT` dans lwip ESP-IDF 4.4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 3 — 2026-04-29 » dans « Évolutions » détaillant les 3 fixes appliqués (F6, F8, F9), l'annulation de F7/F10 (TCP_USER_TIMEOUT non supporté par lwip ESP-IDF 4.4), le coredump-5 confirmant le déplacement du blocage de connect vers publish, et le trade-off du paramètre lwip global sur AsyncWebServer / OTA / NTP. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Watchdog dans `mqttTask` » (tableau des 10 emplacements de `esp_task_wdt_reset()` avec cadence garantie ≤ 5 publish entre 2 resets) ; nouvelle section « Timeout client TCP — UNITÉ EN SECONDES » avec avertissement visible pour le bug latent `setTimeout(5000)` ; section « Détection des déconnexions » étendue avec le store canonique `connectedAtomic` en début de `taskLoop()` et la suppression des stores intermédiaires ; section « Reconnexion » étendue avec la sous-section « Connexion initiale au boot » documentant la suppression du `requestReconnect()` dans `setup()`
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la section « Évolutions » → « Itération 2 — 2026-04-29 » détaillant les 5 fixes appliqués, le bug latent `WiFiClient::setTimeout()` et le coredump confirmant la cause racine. La décision principale de l'ADR (tâche dédiée) reste retenue — c'est un durcissement post-test, pas un revirement

---

## [Unreleased] - 2026-04-27

### Firmware
- **MQTT déplacé dans une tâche FreeRTOS dédiée** (`mqttTask`, core 0, prio 2, stack 8 KB) — corrige les crashes `PANIC IntegerDivideByZero` watchdog 30 s observés en production lorsqu'une publication MQTT (de 33 octets dans le 3ᵉ crash) bloquait `loopTask` dans `lwip_select` à cause de la saturation du TCP send window sur CPL bruyant. La régulation pH/ORP, la filtration et le watchdog continuent désormais indépendamment de l'état du réseau MQTT. Comportement utilisateur strictement identique : mêmes topics, mêmes payloads, mêmes intervalles, même auto-discovery HA. API publique de `MqttManager::publishXxx()` inchangée — les méthodes deviennent simplement non-bloquantes (producteurs sur queue FreeRTOS). Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md)
- **Arrêt propre OTA** : publication `status=offline` synchrone (timeout 1 s) avant `ESP.restart()` sur tous les sites concernés (OTA firmware, redémarrage mode AP, factory reset bouton, factory reset HTTP). Les clients HA voient désormais le passage à `offline` immédiatement, sans attendre les 90 s de timeout broker
- **Stabilité MQTT** : fin des déconnexions périodiques (toutes les 1–2 h) — désactivation du WiFi power save (`WiFi.setSleep(false)`), pré-résolution DNS explicite avant `WiFiClient::connect`, `setSocketTimeout` réduit à 2 s, suppression du `requestReconnect()` périodique qui réinitialisait le backoff exponentiel
- **Stabilité réseau** : latence LAN ramenée sous 10 ms (était 90–260 ms à cause du DTIM WiFi) ; loop principale jamais bloquée plus de ~7 s par opération réseau ; backoff MQTT effectif jusqu'à 120 s sur broker injoignable
- **Coredump** : ajout de la partition `coredump` dédiée (64 KB, offset `0x3F0000`) — les crashes `PANIC` sont désormais persistés en flash et accessibles via l'API HTTP ou l'UI
- **Partitions** : partition `history` réduite de 128 KB à 64 KB pour libérer l'espace nécessaire au coredump ; flash USB obligatoire pour cette migration (OTA insuffisant)
- **History** : `kMaxHourlyDataPoints` réduit de 360 à 168 (7 jours de rétention horaire, contre 15 jours précédemment) pour respecter le budget de la partition réduite
- **History** : `HistoryManager::begin()` détecte un redimensionnement de partition via NVS (`hist_meta/part_sz`) et efface le filesystem avant montage si la taille a changé — évite un crash `IntegerDivideByZero` dans `lfs_alloc`
- **Régulation** : reset journalier des compteurs `dailyPhInjectedMl` / `dailyOrpInjectedMl` désormais indépendant de l'état de la filtration — extraction de la logique de bascule de date dans `tickDailyRollover()`, appelée depuis `update()` **avant** le check `canDose()`. Le bug rendait le compteur figé sur la valeur de la veille tant que la filtration n'avait pas tourné dans la journée. La transition `currentDayDate` vide → date NTP valide remet aussi `dayStartTimestamp = 0` pour invalider tout timer fallback `millis()` accumulé depuis le boot
- **Régulation** : ajout de `saveDailyCounters()` + `armStabilizationTimer()` dans la branche fallback `millis()` du reset journalier (manquaient avant le fix)
- **Régulation** : warnings/criticals du mode `scheduled` (capteur pH/ORP hors plage, daily target plafonné, débit pompe à 0) passés en **edge-triggered** — un seul log à l'entrée dans l'état + un INFO de recovery au retour à la normale. Stoppe le spam de centaines de lignes par seconde quand l'état persistait
- **Historique** : suppression de l'appel `saveToFile()` redondant dans `HistoryManager::update()` (`consolidateData()` l'appelle déjà en interne) — moitié moins d'écritures flash sur la partition `history` toutes les 5 min
- **Historique** : trace `Consolidation terminée: N points` rétrogradée de INFO à DEBUG, suppression du marqueur `DEBUG: Début consolidation historique` ; commentaire corrigé (consolidation effective toutes les 5 min, pas « toutes les heures »)
- **Logger** : intervalle de flush ramené de 60 s à **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et la persistance du coredump couvrent les crashes, l'écriture flash périodique en INFO/DEBUG n'apporte rien
- **Logger** : nouvelle méthode `clearAll()` qui vide RAM + `_persistBuffer` + supprime `/system.log` et `/system.log.tmp` (la méthode existante `clear()` ne touche que le buffer RAM)
- **Diagnostic réseau** : ajout d'un handler `WiFi.onEvent()` dans `setupWiFi()` qui logge les événements `STA_DISCONNECTED` (WARN, avec `reason=N`), `STA_CONNECTED` (INFO), `STA_GOT_IP` (INFO, avec IP et RSSI), `STA_LOST_IP` (WARN). Permet de distinguer un drop Wi-Fi (séquence `DISCONNECTED → CONNECTED → GOT_IP`), un DHCP renew (`LOST_IP → GOT_IP`) ou un problème non-Wi-Fi (broker, firewall) lors d'un blackout réseau
- **Diagnostic MQTT** : log de la cause de déconnexion au front de transition `connecté → déconnecté` (`WARN: MQTT déconnecté détecté — état=N` où `N` est le code `PubSubClient::state()` : `-4` timeout keepalive, `-3` TCP fermé, `-2` TCP refusé, `-1` déconnexion propre). Court-circuit DNS quand `mqttCfg.server` est déjà une IP littérale (`IPAddress::fromString()` avant `WiFi.hostByName()`) — élimine tout cycle DNS pour les installations LAN par IP fixe
- **Stabilité MQTT** : keepalive PubSubClient relevé de 30 s à **60 s** (`mqtt.setKeepAlive(60)` dans `MqttManager::begin()`). La tolérance broker passe corrélativement de 45 s à 90 s (1.5 × keepalive côté Mosquitto), ce qui absorbe les microcoupures réseau prolongées sans déclencher de `état=-4`. Cible : chemins instables type CPL/Powerline où des publishes et PINGREQ se perdent sporadiquement à cause du bruit électrique secteur. Trade-off accepté : une vraie déconnexion (broker arrêté, WiFi coupé) est détectée en 90 s au lieu de 45 s — sans impact sur la régulation pH/ORP qui ne dépend pas d'une latence sub-minute

### Fonctionnalités
- **Paramètres → Avancé** : nouvelle card "Diagnostic crash" — statut coredump (tâche, exception, PC), boutons Actualiser / Télécharger / Effacer, hint de décodage `./tools/decode_coredump.sh`
- **Paramètres → Avancé → Logs** : bouton existant « Effacer » renommé **« Effacer (écran) »** (vide uniquement la vue navigateur) ; nouveau bouton **« Effacer (firmware) »** en rouge `btn--danger` qui appelle `DELETE /logs` après confirmation et purge intégralement les logs côté ESP32 (RAM + fichier persistant)
- **Script** : `tools/decode_coredump.sh` — décode un `coredump.bin` avec `xtensa-esp32-elf-gdb` et `esp_coredump` du penv PlatformIO

### API
- `GET /coredump/info` : résumé JSON du dernier crash (tâche, PC, cause exception)
- `GET /coredump/download` : téléchargement du binaire brut `coredump.bin` (streamé, pas d'allocation 64 KB)
- `DELETE /coredump` : effacement de la partition pour le prochain crash
- `DELETE /logs` (WRITE) : efface intégralement les logs côté ESP32 (RAM + tampon de flush + `/system.log` + `/system.log.tmp`). Réponse `{"success": true}`. Une entrée INFO `Logs effacés (RAM + fichier persistant)` est écrite immédiatement après pour tracer l'action

### Documentation
- `docs/adr/0011-mqtt-task-dediee.md` : ADR créé — déplacement de toute la logique MQTT (publish, connect, loop, callback) dans une tâche FreeRTOS dédiée `mqttTask` ; producer/consumer via deux queues (`outQueue` 32 entrées, `inQueue` 16 entrées) ; arrêt propre `status=offline` avant `ESP.restart()`. Décision motivée par 3 crashes production confirmant que `setSocketTimeout(2)` (ADR-0010) ne couvre pas `lwip_write` sur réseau lossy
- `docs/adr/0010-stabilite-mqtt-reseau.md` : note de mise à jour — l'alternative « Tâche FreeRTOS dédiée pour MQTT » écartée à l'origine est désormais retenue par ADR-0011 (Superseded for MQTT decoupling). Les fixes synchrones D1–D5 restent valides et sont préservés intégralement dans la nouvelle tâche dédiée
- `docs/subsystems/mqtt-manager.md` : refonte majeure — section « Architecture producer/consumer », règles d'or (aucun `mqtt.publish()` depuis `loopTask`, aucun acteur direct depuis `mqttTask`, aucun appel `Async*` depuis `mqttTask`), tableau des paramètres `mqttTask`, refonte gestion des commandes HA (queue entrante drainée par `loopTask`), section « Arrêt propre OTA », troubleshooting drops `outQueue`
- `docs/UPDATE_GUIDE.md` : note user-facing — stabilité réseau améliorée (la régulation continue de tourner même en cas de microcoupures broker MQTT)
- `docs/adr/0010-stabilite-mqtt-reseau.md` : ADR créé — décisions de stabilité réseau (WiFi sans power save, pré-résolution DNS, backoff non réinitialisé)
- `docs/adr/0009-partition-coredump.md` : ADR créé — table de partitions avec coredump + conséquences de migration
- `docs/adr/0007-table-partitions-custom.md` : statut mis à jour → `Superseded by ADR-0009`
- `docs/subsystems/history.md` : partition 64 KB, rétention 7 jours, section protection redimensionnement, budget partition ; clarification que `consolidateData()` appelle `saveToFile()` en interne et que la trace de fin est en DEBUG
- `docs/subsystems/logger.md` : flush 10 min, flush immédiat ERROR/CRITICAL, `_persistBuffer` borné, fichier log 16 KB ; ajout de `clearAll()` et de l'endpoint `DELETE /logs`
- `docs/subsystems/pump-controller.md` : section "Reset journalier" décrivant `tickDailyRollover()` et son emplacement dans `update()` avant `canDose()` ; section "Warnings edge-triggered" listant les six logs concernés et le pattern `static bool`
- `docs/subsystems/mqtt-manager.md` : section "Pattern de connexion (DNS séparé du TCP)" ; clarification que la reconnexion est 100 % pilotée par `update()` ; lien vers ADR-0010
- `docs/API.md` : section "Diagnostic crash (coredump)" avec les 3 endpoints + nouvelle entrée `DELETE /logs`
- `docs/features/page-settings.md` : card "Diagnostic crash" dans le panneau Avancé + section dédiée à la card Logs (4 boutons : Actualiser, Effacer (écran), Télécharger, Effacer (firmware))
- `docs/subsystems/logger.md` : nouvelle section « Logs WiFi » documentant les 4 messages émis par le handler `WiFi.onEvent()` et les codes de raison Wi-Fi courants
- `docs/subsystems/mqtt-manager.md` : note de troubleshooting — vérifier d'abord les logs WiFi (`WARN: WiFi déconnecté (reason=...)`) avant de chercher la cause d'une reconnexion MQTT répétée
- `docs/subsystems/mqtt-manager.md` : section « Diagnostic — Codes d'état PubSubClient » (codes `-4` à `-1` du log `MQTT déconnecté détecté`), tableau du chemin DNS selon IP littérale vs hostname, workflow de troubleshooting basé sur le code d'état
- `docs/subsystems/logger.md` : nouvelle section « Logs MQTT » listant les 6 messages émis par `MqttManager` (incluant le nouveau `MQTT déconnecté détecté — état=N` edge-triggered)
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Keepalive » (60 s côté client, 90 s de tolérance broker, trade-off de détection des vraies déconnexions) ; précision dans le tableau des codes d'état (`-4` = sans PINGREQ/PONG dans la fenêtre 90 s) ; ajout d'une étape 3 au workflow troubleshooting couvrant les chemins physiques instables (CPL/Powerline, RSSI marginal) quand WiFi et broker sont innocentés
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT — état dominant `-3` (`MQTT_CONNECTION_LOST`, socket TCP invalide), `-4` (timeout keepalive) quasi-inatteignable car `mqtt.connected()` est testé avant `mqtt.loop()` dans `taskLoop()` ; délai typique 100–180 s sur arrêt brutal du broker (RTO TCP lwip, pas keepalive applicatif), quasi-immédiat sur fermeture propre TCP. Nouvelle section « Détection des déconnexions » dans la doc subsystem

---

## [Unreleased] - 2026-04-24

### Firmware
- **Persistance compteurs journaliers** : `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont désormais persistés en NVS (namespace `pool-daily`) — les compteurs survivent aux reboots ESP32 et sont restaurés si le jour calendaire est identique
- **Reset journalier** : aligné sur minuit local (RTC/NTP) au lieu d'une fenêtre glissante 24 h ; `armStabilizationTimer()` est armé au passage de minuit (mitigation double quota)
- **`kMinValidEpoch`** : constante consolidée dans `src/constants.h` (valeur : 1700000000, 14 nov. 2023)
- **Raison du dernier reboot** : champ `reset_reason` ajouté dans le payload WebSocket `sensor_data` — valeurs possibles : `POWER_ON`, `SW_RESET`, `WATCHDOG`, `BROWNOUT`, `PANIC`, `DEEP_SLEEP`, `EXTERNAL`, `UNKNOWN` ; constant pendant le runtime

### Fonctionnalités
- **Pages /ph et /orp** : les blocs Statistiques sont grisés (`opacity: 0.5`) lors d'une déconnexion WebSocket — indique visuellement que les données affichées ne sont plus à jour
- **Toast reboot inattendu** : un toast dismissable s'affiche une fois par session si le champ `reset_reason` indique un reboot inattendu (`WATCHDOG`, `BROWNOUT` ou `PANIC`) — libellé : « Redémarrage inattendu détecté (raison : X) »
- **Régulation pH** : remplacement du toggle binaire `ph_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation pH** : mode Programmée — volume quotidien configurable (mL), injecté pendant les plages de filtration jusqu'au quota journalier
- **Régulation pH** : migration automatique au premier boot : `ph_enabled=true` → `automatic`, `ph_enabled=false` → `manual`
- **Limites horaires** : renommage `phInjectionLimitSeconds` → `phInjectionLimitMinutes` (idem ORP) — les limites sont désormais saisies en minutes (1–60) au lieu de secondes ; migration NVS transparente au boot (`ph_limit_sec` → `ph_limit_min`)
- **Protection pompes** : suppression de `minPauseBetweenMs` — la pause inter-injections configurable est retirée ; la protection contre le short-cycling reste assurée par `minInjectionTimeMs` (30 s) et `maxCyclesPerDay` (20/24 h)
- **MQTT** : publication des champs `ph_regulation_mode` et `ph_daily_target_ml` dans `publishTargetState()`
- **Sécurité** : suppression du log du mot de passe WiFi en clair dans les traces de reconnexion
- **Régulation pH (Programmée)** : refonte de l'algorithme d'injection — la pompe injecte librement pendant la filtration jusqu'à atteindre le quota journalier (`phDailyTargetMl`), sans répartition sur 24 h ; la limite horaire (`phInjectionLimitMinutes`) reste la seule barrière contre l'injection rapide
- **Régulation ORP** : remplacement du toggle binaire `orp_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation ORP** : mode Programmée — volume quotidien de chlore configurable (mL), aveugle au capteur ORP, borné par `maxChlorineMlPerDay` ; PID réinitialisé au retour en mode automatique
- **Régulation ORP** : migration automatique au premier boot : `orp_enabled=true` → `automatic`, `orp_enabled=false` → `manual` ; champ `orp_enabled` conservé comme miroir pour compatibilité HA
- **MQTT** : publication des champs `orp_regulation_mode` et `orp_daily_target_ml` dans `publishTargetState()`

### API
- `GET /get-config` / `POST /save-config` : `ph_limit_seconds` → `ph_limit_minutes`, `orp_limit_seconds` → `orp_limit_minutes` ; suppression de `min_pause_between_min`
- WebSocket config : mêmes renommages (`ph_limit_minutes`, `orp_limit_minutes`)
- `GET /get-config` : ajout des champs `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`, `orp_cal_valid`
- `POST /save-config` : validation de `orp_regulation_mode` (enum), `orp_daily_target_ml` (borné par `max_orp_ml_per_day`, HTTP 400 si dépassé)

### Fonctionnalités
- **Page pH** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels
- **Page pH** : mode Programmée avec saisie du volume quotidien (mL) borné par la limite journalière configurée
- **Paramètres** : champs durée max pH/ORP en minutes (1–60 min/h) au lieu de secondes ; suppression du champ « Pause entre deux injections »
- **Page ORP** : refonte complète — architecture 4 cartes (Statistiques compact / Régulation / Historique / Calibration conditionnelle)
- **Page ORP** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels (symétrie avec page pH)
- **Page ORP** : mode Programmée avec saisie du volume quotidien de chlore (mL), borné par la limite journalière de sécurité
- **Page ORP** : calibration accessible uniquement en mode Automatique (bouton Calibrer dans le sous-bloc Automatique) ; carte Calibration en superposition pendant le protocole
- **Page ORP** : bloc Statistiques compact (ORP actuelle + Dosage du jour) en en-tête de page, hors carte

---

## [1.1.0] - 2026-03-29

### Firmware
- **MQTT** : ajout des topics publiés `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph_target`, `orp_target`
- **MQTT** : ajout des topics de commande `ph_target/set` et `orp_target/set` (modification des consignes pH et ORP depuis HA ou MQTT)
- **MQTT** : correction du switch "Filtration Marche/Arrêt" — la commande `OFF` forçait l'arrêt de la filtration mais elle redémarrait immédiatement selon le planning
- **Home Assistant Auto-Discovery** : ajout de 6 nouvelles entités (Dosage pH Actif, Dosage Chlore Actif, Limite Journalière pH, Limite Journalière Chlore, Consigne pH, Consigne ORP)

### Documentation
- `docs/MQTT.md` : documentation complète des topics publiés, commandes et entités Home Assistant avec les noms tels qu'ils apparaissent dans l'interface HA
- `docs/API.md` : réécriture complète — tous les endpoints documentés (30+)
- `docs/UPDATE_GUIDE.md` : mise à jour avec les modes OTA de `deploy.sh`
- `deploy.sh` : ajout des modes `ota-firmware`, `ota-fs`, `ota-all` (compile + envoi OTA en une commande)
- Renommage `quick_update.sh` → `ota_update.sh`

---

## [1.0.3] - 2026-03-27

### Firmware
- **Factory reset** : détection par appui long 10s pendant le fonctionnement normal — plus besoin de couper l'alimentation
- Suppression des constantes `PH_SENSOR_PIN` / `ORP_SENSOR_PIN` (vestiges ADC interne non utilisés depuis le passage à l'ADS1115 I2C)

### Documentation
- Procédure factory reset mise à jour (fonctionnement runtime)
- Section Matériel Requis : schéma électronique et PCB illustrés, liens vers fichiers Gerber et STL
- `build_all.sh` documenté dans BUILD.md et UPDATE_GUIDE.md

---

## [1.0.1] - 2026-03-26

### Firmware
- **Bouton factory reset (GPIO32)** : appui de 10 secondes au démarrage pour réinitialisation usine complète
  - LED intégrée clignote pendant l'appui pour indiquer la progression
  - Efface la partition NVS (mot de passe, WiFi, MQTT, calibrations)
  - Préserve les consignes, limites et l'historique des mesures
  - L'ESP32 redémarre en mode AP avec l'assistant de configuration

### Hardware
- Ajout des fichiers Gerber (fabrication PCB) dans le dossier `hardware/`
- Ajout des fichiers STL du boîtier v3 (corps + couvercle) dans le dossier `hardware/`

---

## [1.0.0] - 2026-03-24 — Première release publique

### Fonctionnalités
- Régulation automatique pH et ORP (chlore) via algorithme PID
- Gestion filtration (auto / manuel / off) avec programmation horaire
- Contrôle éclairage avec programmation horaire
- Interface web avec tableau de bord temps réel (graphiques pH, ORP, température)
- Intégration Home Assistant via MQTT Auto-Discovery
- Mises à jour OTA via interface web (firmware et filesystem)
- Assistant de configuration au premier démarrage (mot de passe, WiFi, heure)
- Protocole UART pour écran LVGL externe
- Historique des mesures sur partition dédiée (préservé lors des mises à jour)
- Alertes MQTT en cas d'anomalie (valeurs aberrantes, limites atteintes, mémoire faible)
- Factory reset via bouton physique GPIO32
