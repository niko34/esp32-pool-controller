# ADR-0014 — Migration Atlas EZO pH/ORP (PCB v2)

- **Statut** : Accepté
- **Date** : 2026-05-06
- **Décideurs** : Nicolas Philippe
- **Spec(s) liée(s)** : [`feature-021-migration-atlas-ezo`](../../specs/features/done/feature-021-migration-atlas-ezo.md), [`feature-020-deux-sondes-temperature`](../../specs/features/done/feature-020-deux-sondes-temperature.md) (dépendance — fournit `getWaterTemperature()` pour la compensation T° du pH), [ADR-0001](0001-capteurs-analogiques-ads1115.md) (**superseded by this ADR**), [ADR-0012](0012-mapping-gpio-pcb-v2.md) (mapping GPIO PCB v2)

## Contexte

Le **PCB v1** mesure le pH et l'ORP via une chaîne analogique : sondes BNC → ADS1115 (ADC 16 bits I²C, canaux A0/A1) → conversion logicielle (`DFRobot_PH` pour le pH, formule `offset + slope` côté firmware pour l'ORP). Voir [ADR-0001](0001-capteurs-analogiques-ads1115.md) pour la justification historique.

Cette chaîne souffre de **trois défauts structurels** rendus visibles par les essais terrain en saison 2025 :

1. **Sensibilité au bruit de filtration** : delta pH ≈ ±0.3 et delta ORP ≈ ±50 mV entre filtration ON et OFF, sur la même eau, dans une fenêtre d'une minute. Cause confirmée : couplage capacitif entre la pompe 230 V et les masses analogiques de l'ADS1115. Or la régulation chimique a précisément besoin de mesurer **pendant** la filtration (eau qui circule devant la sonde) → le capteur le moins fiable au moment où il est le plus utile.
2. **Dérive thermique** : la compensation T° du pH par `DFRobot_PH` est partielle (correction Nernst seule), pas de compensation ORP. Erreur résiduelle ≈ -0.05 à -0.10 pH entre 15 °C et 30 °C.
3. **Calibration laborieuse** : workflow 2 points pH (point neutre 7.0 + point acide 4.0) implémenté côté firmware (calculs offset/slope, persistance NVS, retour à des valeurs « usine » DFRobot non triviales à effacer). ORP : calibration 1 ou 2 points entièrement côté UI client, persistance via `POST /save-config`. Difficile à diagnostiquer côté terrain (pas de codes retour explicites du capteur).

Le **PCB v2** intègre matériellement les modules **Atlas Scientific EZO Embedded I²C** (pH 0x63, ORP 0x62) à la place de l'ADS1115 (cf. [ADR-0012](0012-mapping-gpio-pcb-v2.md)). Cette ADR documente la **migration logicielle correspondante** : abandon complet de la chaîne analogique au profit des EZO numériques, avec recâblage de la régulation chimique sur les nouvelles primitives capteurs.

## Décision

Le firmware **2.0.0** pilote les capteurs pH et ORP via les **modules Atlas Scientific EZO Embedded I²C** uniquement. La chaîne analogique ADS1115 + `DFRobot_PH` est entièrement supprimée du code et des `lib_deps`.

Choix structurants :

- **Mini-classe maison `AtlasEzoSensor`** ([`src/atlas_ezo.h`](../../src/atlas_ezo.h) / [`.cpp`](../../src/atlas_ezo.cpp), ~80 lignes) encapsule les commandes Atlas EZO utiles (`R`, `RT,<t>`, `Cal,*`, `Cal,?`, `Cal,clear`, `I`) et les délais imposés par le firmware EZO (600 ms après `RT,*`, 900 ms après `R` ou `Cal,*`).
- **Mutex I²C `i2cMutex`** tenu pendant **toute la séquence** `RT,<temp>` + délai 600 ms + `R` + délai 900 ms (atomicité chimique, condition #6 pool-chemistry). Timeout d'acquisition `kI2cMutexTimeoutMs = 2000 ms`.
- **Queue FreeRTOS `_ezoQueue`** (4 slots) pour exécuter les commandes longues (calibrations ~1-2 s) hors du contexte HTTP/UART. Les routes `POST /calibrate_*` retournent `{success:true, queued:true}` immédiatement (< 1 ms) ; `loopTask` dépile via `_processEzoQueue()`. Pattern miroir de `mqttTask` ([ADR-0011](0011-mqtt-task-dediee.md)).
- **Calibration mémorisée dans le module EZO** (NVRAM interne). Le firmware **ne calcule plus rien** : il transmet `Cal,mid,7.00`, `Cal,low,4.00`, `Cal,<reference>` puis lit les valeurs déjà calibrées via `R`. Suppression de toutes les clés NVS pH/ORP héritées (`ph_cal_date`, `ph_cal_temp`, `orp_cal_offset`, `orp_cal_slope`, `orp_cal_reference`, `orp_cal_date`, `orp_cal_temp`).
- **6 garde-fous pool-chemistry** intégrés à `canDose(int)` (cf. [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md)) :
  1. `kSensorStaleTimeoutMs = 20000` — `getPh()`/`getOrp()` retournent `NaN` si dernière lecture valide > 20 s.
  2. `cal_points` doit être `>= 2` (pH) ou `>= 1` (ORP). Valeur `-1` (EZO injoignable / bus dégradé) bloque le dosage.
  3. Stabilisation post-calibration : `kStabilizationDurationPhMs = 5 min`, `kStabilizationDurationOrpMs = 3 min` (par pompe).
  4. Logger `critical` + alerte MQTT `pool/alerts/calibration_required` (retain) au boot non calibré et à chaque transition.
  5. Bus I²C dégradé : `kEzoBusFailMaxConsecutive = 2` échecs consécutifs → `_phCalCachedPoints = -1` ET `_lastPh = NaN` → dosage bloqué.
  6. Mutex I²C atomique (cf. ci-dessus).
- **Compensation T° pH** : `RT,<temp>` envoyé avant chaque `R` avec la valeur de [`Sensors::getWaterTemperature()`](../../src/sensors.h) (feature-020). Fallback **25.0 °C** si la sonde eau n'est pas identifiée ou retourne `NaN` (erreur < 0.1 pH dans la plage 15-30 °C piscine, acceptable).
- **Anti-rafale court terme** (correctif Pass 3.5) : ring buffer de 20 timestamps par pompe. Refus de démarrage si > 6 cycles/min OU > 20 cycles/15 min, indépendant des limites journalières/horaires existantes.

## Alternatives considérées

- **Lib externe `EZO_pH` / `EZO_ORP` Arduino** (rejetée) — aucune lib mature pour ESP32-Arduino. Les variantes existantes ciblent AVR (Arduino Uno) et utilisent `Wire.beginTransmission()` sans gestion du mutex I²C partagé. Adapter aurait demandé autant d'effort qu'écrire la mini-classe maison, sans bénéfice.
- **Conserver ADS1115 + `DFRobot_PH` + filtre numérique amélioré** (rejetée) — un filtre médian sur 16 échantillons ou un Kalman de 1ᵉʳ ordre réduisent le bruit de filtration mais **ne résolvent ni la dérive thermique de l'ORP, ni la calibration laborieuse, ni le couplage capacitif intrinsèque** au PCB v1. Le PCB v2 étant déjà fabriqué avec les EZO, cette piste serait du sparadrap sur un PCB obsolète.
- **Chaîne mixte (ADS1115 pH + EZO ORP)** (rejetée) — double effort de maintenance (deux chemins de code, deux workflows de calibration, deux jeux de tests) sans bénéfice opérationnel. Les EZO étant déjà soudés sur le PCB v2, autant les exploiter complètement.
- **Tâche FreeRTOS dédiée Atlas (à la manière de `mqttTask`)** (rejetée) — surcoût stack injustifié (~8 KB) pour un volume d'opérations longues très faible (2 à 3 calibrations par an, en pratique). La queue + `loopTask` couvre le besoin avec un coût mémoire négligeable (4 slots × 8 octets = 32 octets).
- **Exécution synchrone des calibrations dans le handler HTTP** (rejetée) — viole le contrat AsyncWebServer (50 ms max par handler). Une calibration EZO dure ~900 ms (transaction I²C bloquante) → blocage du serveur asynchrone, timeouts navigateur, dégradation perceptible des routes parallèles. La queue est obligatoire.
- **Conserver les clés NVS legacy pH/ORP pour compatibilité descendante** (rejetée) — la chaîne de mesure change radicalement, aucune migration de données possible (les `offset`/`slope` ADS1115 n'ont aucun sens pour un EZO qui calibre en interne). Suppression silencieuse au 1ᵉʳ boot après upgrade ; recalibration obligatoire documentée dans [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md).

## Conséquences

### Positives

- **Lecture pH/ORP non perturbée par la filtration** (objectif fonctionnel principal AC3 de feature-021). L'isolation galvanique du PCB v2 + la conversion numérique côté EZO suppriment le couplage capacitif observé en v1.
- **Calibration plus fiable et plus rapide** : workflow EZO en 2 commandes (`Cal,mid,7.00` + `Cal,low,4.00`) avec mémorisation interne au module. Plus de calculs offset/slope côté firmware, plus de persistance NVS pH/ORP côté ESP32.
- **Compensation T° intégrée** : `RT,<temp>` envoyé à l'EZO pH avant chaque lecture, le module applique la correction Nernst en interne. Résolution effective 0.001 pH (vs ~0.05 pH en v1 après filtrage médian).
- **Code firmware simplifié** : suppression de 10 fonctions publiques `Sensors` legacy (`getRawPh`, `getRawOrp`, `getPhVoltageMv`, `isPhCalibrated`, `getRawTemperature`, `calibratePhNeutral/Acid/Alkaline`, `clearPhCalibration`, `detectAdsIfNeeded`, `recalculateCalibratedValues`, `publishValues`) + 7 champs `MqttConfig` legacy (`orp_cal_*`, `ph_cal_*`).
- **Garde-fous chimiques renforcés** : 6 conditions pool-chemistry + 2 correctifs Pass 3.5 → fail-closed strict. Auparavant le PID acceptait silencieusement des lectures stale ou non calibrées.
- **Auto-discovery HA enrichi** : 2 nouveaux sensors `Piscine pH Points Calibrés` / `Piscine ORP Points Calibrés` permettent à l'utilisateur de surveiller l'état de calibration depuis Home Assistant.

### Négatives / dette assumée

- **Verrouillage long terme à l'écosystème Atlas EZO** : un changement de fournisseur (Sensorex, Hach, etc.) demanderait une réécriture complète de la chaîne capteurs. Compensé par la maturité du protocole Atlas (stable depuis 2015) et par la modularité de la mini-classe maison.
- **Recalibration obligatoire après upgrade** v1.x → v2.0.0 (les calibrations NVS legacy n'ont aucune correspondance avec la calibration interne EZO). Documenté dans [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md). Tant que la calibration n'est pas faite, la régulation pH/ORP automatique est inhibée + alerte MQTT `pool/alerts/calibration_required` + chip ambrée UI.
- **Marge flash réduite** : build courant à 98.8 % flash (~17 KB de marge). La mini-classe `AtlasEzoSensor` est compacte mais la queue + les helpers de calibration ajoutent leur empreinte. Point d'attention pour les futures features.
- **Calibrations bloquantes ~900 ms** : exécution dans `loopTask` via la queue. Pendant ce délai, les autres consommateurs de `loopTask` (capteurs DS18B20, watchdog reset) sont bloqués. Sous le watchdog 30 s avec marge confortable, mais il faut surveiller le profil temporel si plusieurs calibrations sont enchaînées.
- **Comportement EZO froid au démarrage non testé sur cycle long** : risque résiduel mineur d'un EZO qui répond `255 (no data)` pendant les premières lectures. Mitigation : le ring buffer de fail-streak `kEzoBusFailMaxConsecutive = 2` accepte un échec isolé.

### Ce que ça verrouille

- **Adresses I²C Atlas figées** : `kEzoPhAddress = 0x63` / `kEzoOrpAddress = 0x62` (défauts Atlas, modifiables via commande `I2C,<addr>` mais non exposé dans l'UI).
- **Bus I²C partagé** entre DS3231 + EZO pH + EZO ORP. Toute future addition I²C doit utiliser `i2cMutex` avec timeout `kI2cMutexTimeoutMs`.
- **Plage de référence ORP 0..1000 mV** côté `POST /calibrate_orp` (tolérante : couvre les standards 225, 470, 650 mV). Élargie au-delà de la suggestion initiale 200..1000 post code-review.
- **pH publié à 3 décimales** sur tous les contrats publics (HTTP `/data`, WebSocket, MQTT). Avant la migration : 1 décimale. Tout consommateur HA qui parsait avec `int()` doit basculer sur `float()`.
- **Routes legacy supprimées** : `POST /calibrate_ph_neutral`, `POST /calibrate_ph_acid`, `POST /clear_ph_calibration` retournent désormais 404. Remplacées par `POST /calibrate_ph {step:"mid"|"low"}` + `POST /calibrate_clear {sensor:"ph"|"orp"}`.
- **Champs WS supprimés** : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw` (la notion de « valeur brute » n'a pas de sens côté EZO — la valeur retournée est déjà calibrée).
- **Inhibition régulation auto si capteur non calibré** : contrat de sécurité documenté, le retour à un comportement « régule quand même » exigerait un nouvel ADR.

## Références

- Code : [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp) — mini-classe pilote
- Code : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — refonte Pass 2 (queue, stale, cache cal_points)
- Code : [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `canDose(int)` 10 garde-fous fail-closed (Pass 3 + 3.5)
- Code : [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — routes refondues `/calibrate_ph`, `/calibrate_orp`, `/calibrate_clear`
- Code : [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — alertes `calibration_required` / `sensor_stale` + auto-discovery HA `ph_cal_points` / `orp_cal_points`
- Code : [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — champs `phCalPoints` / `orpCalPoints`, pH 3 décimales
- Code : [`src/constants.h`](../../src/constants.h) — `kEzoPhAddress`, `kEzoOrpAddress`, `kEzoReadDelayMs`, `kEzoCalDelayMs`, `kEzoRtDelayMs`, `kSensorStaleTimeoutMs`, `kEzoBusFailMaxConsecutive`, `kStabilizationDurationPhMs`, `kStabilizationDurationOrpMs`, `kMaxDosingCyclesPerMinute`, `kMaxDosingCyclesPer15Min`, `kDosingCycleHistorySize`
- Spec : [`specs/features/done/feature-021-migration-atlas-ezo.md`](../../specs/features/done/feature-021-migration-atlas-ezo.md)
- Doc subsystem : [`docs/subsystems/sensors.md`](../subsystems/sensors.md), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md), [`docs/subsystems/mqtt-manager.md`](../subsystems/mqtt-manager.md)
- Doc transverse : [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md), [`docs/BUILD.md`](../BUILD.md), [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md)
- Doc page : [`docs/features/page-ph.md`](../features/page-ph.md), [`docs/features/page-orp.md`](../features/page-orp.md)
- ADR superseded : [ADR-0001](0001-capteurs-analogiques-ads1115.md) (chaîne analogique ADS1115 + DFRobot_PH)
- ADR liés : [ADR-0011](0011-mqtt-task-dediee.md) (pattern queue + tâche dédiée réutilisé), [ADR-0012](0012-mapping-gpio-pcb-v2.md) (mapping pins PCB v2), [ADR-0013](0013-identification-sondes-onewire.md) (identification sondes DS18B20 — feature-020 dont feature-021 dépend)
