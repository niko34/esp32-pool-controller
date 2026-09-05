## Métadonnées
- **ID** : feature-004
- **Slug** : regles-regulation-chimique
- **Type** : feature (documentaire + cadrage des évolutions)
- **Statut** : doing — itérations 3 et 4 en préparation
- **Priorité** : haute — référence transverse consommée par feature-003, future refonte ORP, écran LVGL, MQTT/HA
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-04-24
- **Spec(s) liée(s)** : feature-003 (page `/ph`)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage (interaction filtration ↔ régulation)
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] UART / écran LVGL (événements dosage)
- [ ] OTA
- [x] Authentification (endpoints d'action protégés)
- [x] Historique / persistance (points toutes les 5 min)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire**, cette spec est le socle chimie/sécurité
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Spec de référence décrivant **l'ensemble des règles de régulation chimique pH et ORP** appliquées par le firmware : délais, seuils, limites de sécurité, protections anti-cycling, interactions avec la filtration, persistance du cumul, et exposition via WebSocket, MQTT et Home Assistant. Sert de contrat unique consommé par les pages `/ph`, `/orp`, dashboard, Paramètres, page Produits, écran LVGL et intégration HA.

## Contexte
Aujourd'hui, les règles de régulation sont **réparties entre** `src/pump_controller.cpp`, `src/filtration.cpp`, `src/web_routes_control.cpp`, `src/ws_manager.cpp`, `src/mqtt_manager.cpp`, ainsi que dans des constantes (`src/constants.h`, `src/config.h`) et dans les structures `SafetyLimits`, `PumpProtection`, `PumpControlParams`. Certaines sont configurables via la page Paramètres, d'autres sont **en dur dans le code** et invisibles à l'utilisateur. Cette spec consolide l'état actuel pour :
- éviter la duplication dans les specs de pages (`feature-003`, future spec ORP)
- servir de check-list pour `pool-chemistry` sur toute évolution touchant à la chimie
- préparer les évolutions futures (exposition de règles aujourd'hui en dur, suivi per-injection, etc.)

## Comportement attendu
> Cette section **décrit l'existant**. Les évolutions souhaitées sont listées en « Écarts / évolutions identifiées » plus bas.

### 1. Boucle de régulation
- Exécutée à chaque `loop()` d'Arduino (fréquence de l'ordre du kHz, délai plancher `kLoopDelayMs = 10 ms` — cf. [src/constants.h:10](../../../src/constants.h:10)).
- Point d'entrée : `PumpControllerClass::update()` ([src/pump_controller.cpp:308](../../../src/pump_controller.cpp:308)), appelée depuis `main.cpp` loop.
- L'injection effective est modulée par PWM 20 kHz, 8 bits (`PUMP_PWM_FREQ`, `PUMP_PWM_RES_BITS` — cf. [src/config.h:14-15](../../../src/config.h:14)).

### 2. Algorithme de correction
- **PID** par produit (pH et ORP), structure `PIDController` ([src/pump_controller.h:27](../../../src/pump_controller.h:27)).
- Gains **recalculés** à chaque changement du paramètre **Vitesse de correction** (`regulationSpeed`) — cf. `applyRegulationSpeed()` [src/pump_controller.cpp:17](../../../src/pump_controller.cpp:17) :
  - `slow` : Kp=3, Ki=0.05, Kd=12
  - `normal` (défaut) : Kp=6, Ki=0.1, Kd=8
  - `fast` : Kp=12, Ki=0.2, Kd=4
- Anti-windup intégral : `integralMax = 50` ([src/pump_controller.h:35](../../../src/pump_controller.h:35)).
- Protection `dt` PID : les deltas > 10 s sont ignorés ([src/pump_controller.cpp:192](../../../src/pump_controller.cpp:192)).

### 3. Sens de correction
- **pH** : deux modes exclusifs, sélectionnés par `phCorrectionType` ([src/config.h:62](../../../src/config.h:62)) :
  - `ph_minus` (défaut) : dose acide pour **baisser** le pH → `error = pH - cible`
  - `ph_plus` : dose base pour **monter** le pH → `error = cible - pH`
- **ORP** : toujours montée (injection de chlore) → `error = cible - ORP` ([src/pump_controller.cpp:476](../../../src/pump_controller.cpp:476)). Pas de correction dans l'autre sens.

### 4. Seuils et bande morte
- **pH** : `PH_DEADBAND = 0.01` ([src/config.h:29](../../../src/config.h:29)), seuil démarrage `phStartThreshold = 0.05`, seuil arrêt `phStopThreshold = 0.01` ([src/config.h:157-158](../../../src/config.h:157)). Hystérésis : démarre si erreur > 0.05, continue tant que erreur > 0.01.
- **ORP** : `ORP_DEADBAND = 2.0 mV` ([src/config.h:30](../../../src/config.h:30)), seuil démarrage 10 mV, seuil arrêt 2 mV ([src/config.h:159-160](../../../src/config.h:159)).
- Erreur max pour normalisation PID : `kPhMaxError = 1.0`, `kOrpMaxError = 200 mV` ([src/constants.h:89-90](../../../src/constants.h:89)).

### 5. Mode de régulation (`regulationMode`)
Deux valeurs, sélectionnées dans Paramètres → Régulation pH/ORP ([src/config.h:59](../../../src/config.h:59), UI : [data/index.html:1264](../../../data/index.html:1264)) :
- **`pilote`** : régulation autorisée **uniquement quand la filtration tourne**. Au démarrage de la filtration, un **délai de stabilisation** est armé (cf. §6). À l'arrêt de la filtration, le timer est effacé et les pompes stoppées.
- **`continu`** : régulation permanente (le contrôleur est supposé alimenté via la ligne filtration). Le timer de stabilisation est armé **au boot**.
- Point de décision : `canDose()` ([src/pump_controller.cpp:169](../../../src/pump_controller.cpp:169)).

### 6. Délai de stabilisation au démarrage de la filtration
- Paramètre utilisateur : `stabilizationDelayMin` (minutes, défaut 5, plage 0-60) — cf. [data/index.html:1322](../../../data/index.html:1322), [src/config.h:60](../../../src/config.h:60).
- Armé par `armStabilizationTimer()` ([src/pump_controller.cpp:148](../../../src/pump_controller.cpp:148)) :
  - en mode `pilote` : au démarrage de la filtration, **uniquement si** la pause depuis le dernier arrêt filtration était **> délai de stabilisation** (évite les glitches — sauvegarde config, coupure brève) ([src/filtration.cpp:213-221](../../../src/filtration.cpp:213))
  - en mode `continu` : au boot ([src/main.cpp:110](../../../src/main.cpp:110))
- Pendant la stabilisation : toute tentative de dosage refusée par `canDose()`.
- Durée restante exposée via WebSocket (`stabilization_remaining_s`) et UART.
- Valeur 0 = désactivé.

### 7. Limites de sécurité
Toutes stockées dans `SafetyLimits` ([src/config.h:143](../../../src/config.h:143)) et persistées via `saveMqttConfig()` (LittleFS `/config.json`).

**Limite horaire (durée d'injection cumulée)**
- `phInjectionLimitSeconds` (défaut 300 s = 5 min/h)
- `orpInjectionLimitSeconds` (défaut 600 s = 10 min/h)
- Fenêtre **glissante d'1 h** (3 600 000 ms) réinitialisée à chaque cycle, pas à l'heure civile — cf. `refreshDosingState()` [src/pump_controller.cpp:70](../../../src/pump_controller.cpp:70).
- Quand atteinte : dosage suspendu jusqu'à la fin de la fenêtre ; un warning est loggé une fois par cycle ([src/pump_controller.cpp:376](../../../src/pump_controller.cpp:376)).
- **Non reflétée dans l'UI** (pas de badge dédié).

**Limite journalière (volume)**
- `maxPhMinusMlPerDay` (défaut 300 mL) — nom en dur `maxPhMinus` mais s'applique aussi à `ph_plus`
- `maxChlorineMlPerDay` (défaut 500 mL)
- Fenêtre **glissante de 24 h** (`86 400 000 ms`), réinitialisée à chaque cycle ; **pas de reset à minuit local** — cf. [src/pump_controller.cpp:256](../../../src/pump_controller.cpp:256).
- Quand atteinte : dosage arrêté, flag `phLimitReached` / `orpLimitReached`, log `critical`, exposé via WS (`ph_limit_reached`, `orp_limit_reached`) et MQTT/HA (`binary_sensor.*_ph_limit`, `*_orp_limit`).
- Cumul journalier courant en RAM : `dailyPhInjectedMl`, `dailyOrpInjectedMl` — exposés via WS (`ph_daily_ml`, `orp_daily_ml`) et MQTT.
- **Persistance** : cumul journalier **perdu au reboot** (RAM seulement), le flag de limite atteinte également. → *Traité par itération 3 de cette spec.*

### 8. Protection anti-cycling des pompes
Structure `PumpProtection` ([src/config.h:153](../../../src/config.h:153)), **en dur dans le code**, non exposée dans l'UI :
- `minInjectionTimeMs = 30 000` : toute injection démarrée dure au moins 30 s, même si l'erreur repasse sous le seuil d'arrêt.
- `minPauseBetweenMs = 1 800 000` (30 min par défaut, mais en pratique **écrasé par `minPauseBetweenMin` × 60000** à la charge de config — cf. [src/config.cpp], à vérifier) : pause minimale entre deux fins d'injection.
- `maxCyclesPerDay = 20` : au-delà de 20 démarrages dans une fenêtre glissante de 24 h, plus aucun dosage jusqu'au reset du compteur.
- Compteur de cycles par jour réinitialisé par fenêtre glissante `86 400 000 ms`.

### 9. Paramètres pompes
- Deux pompes PWM indépendantes (PCB v2, MOSFET IRLZ44N). Attribution produit → pompe via `phPump`, `orpPump` (1 ou 2).
- Puissance max par pompe en régulation : `pump1MaxDutyPct`, `pump2MaxDutyPct` (défaut 50 %, plage 0-100 %).
- Débit nominal configurable : `pumpMaxFlowMlPerMin` (défaut 90 mL/min, plage 1-500).
- Débit minimal actif (duty ≥ `MIN_ACTIVE_DUTY = 80`) : `kPumpMinFlowMlPerMin = 5.2 mL/min` ([src/constants.h:85](../../../src/constants.h:85)) — **en dur**.
- Conversion débit ↔ duty linéaire entre `minFlow` et `maxFlow` (cf. `flowToDuty()` / `dutyToFlow()`).

### 10. Injection manuelle
- Endpoints `POST /ph/inject/start`, `/ph/inject/stop`, `/orp/inject/start`, `/orp/inject/stop` — protégés auth (`REQUIRE_AUTH … WRITE`) ([src/web_routes_control.cpp:76-153](../../../src/web_routes_control.cpp:76)).
- Paramètre préféré : `?volume=N` (mL, borné 1 – 2000). Fallback legacy : `?duration=N` (secondes).
- Durée calculée à partir du débit effectif à la puissance max configurée : `durationS = (volume / flow) × 60`, plafonnée à 3 600 s.
- L'arrêt automatique à fin de durée se fait dans `updateManualInject()` ([src/web_routes_control.cpp:28](../../../src/web_routes_control.cpp:28)) (appelée depuis le loop principal).
- Exposé via WS : `ph_inject_remaining_s`, `orp_inject_remaining_s`.
- **Règles appliquées** :
  - le volume injecté manuellement est **compté dans le cumul journalier** (`updateSafetyTracking()` couvre auto + manuel, [src/pump_controller.cpp:558](../../../src/pump_controller.cpp:558))
  - **mais** l'injection manuelle **ne vérifie pas** explicitement les limites journalières/horaires avant de démarrer : elle tourne la pompe en `setManualPump(…)` sans passer par `canDose()`, `checkSafetyLimits()`, ni le délai de stabilisation. Cela constitue un **écart** (voir §20).

### 11. Suivi des produits (page `/dosages`)
- Structure `ProductConfig` ([src/config.h:131](../../../src/config.h:131)), persistée via `saveProductConfig()` dans `/product.json` (LittleFS), flag `productConfigDirty` pour écriture différée.
- Par produit : activation tracking, volume bidon (mL, défaut 20 000), total injecté depuis dernier reset (mL), seuil d'alerte (mL, défaut 2000).
- Incrémenté **à chaque tick de loop en même temps que le cumul journalier** (`updateSafetyTracking()` [src/pump_controller.cpp:288](../../../src/pump_controller.cpp:288)), si tracking activé.
- Page UI : `/dosages` ([data/index.html:918](../../../data/index.html:918)) — barre de volume restant, bouton « Nouveau bidon » (remise à zéro `phTotalInjectedMl`/`orpTotalInjectedMl`).
- Exposé via WS : `ph_tracking_enabled`, `ph_remaining_ml`, `ph_container_ml`, `ph_alert_threshold_ml` (idem ORP).
- Auto-discovery HA : `sensor.*_ph_remaining`, `sensor.*_orp_remaining`, `binary_sensor.*_ph_stock_low`, `binary_sensor.*_orp_stock_low`.

### 12. Interaction filtration ↔ régulation
- Démarrage filtration :
  - Mode `pilote` : arme le timer de stabilisation si pause précédente suffisante
  - Mode `continu` : aucun effet (timer déjà armé au boot)
- Arrêt filtration :
  - Mode `pilote` : `clearStabilizationTimer()`, `canDose()` retourne `false`, les pompes sont stoppées
  - Mode `continu` : aucun effet
- Pendant la filtration, le calcul automatique de l'horaire peut se rafraîchir après 5 min si `mode=auto` (recalcul avec la vraie température de l'eau en circulation) ([src/filtration.cpp:229](../../../src/filtration.cpp:229)).
- OTA en cours : `PumpController.setOtaInProgress(true)` arrête toutes les pompes et fige l'état ([src/pump_controller.cpp:601](../../../src/pump_controller.cpp:601)).

### 13. Capteurs : disponibilité et fraîcheur
- Lecture pH/ORP toutes les **5 s** (`kPhOrpSensorIntervalMs = 5000` — [src/constants.h:23](../../../src/constants.h:23)) via modules Atlas EZO I2C.
- Température DS18B20 toutes les **2 s** (`kTempSensorIntervalMs = 2000`).
- Échantillonnage : médiane sur `kNumSensorSamples = 3` (non appliqué aux EZO, utilisé pour l'ADC legacy).
- Aucune mesure de **fraîcheur explicite** : si le module EZO ne répond pas, `sensors.getPh()` peut renvoyer NAN. `PumpController.update()` tolère NAN (pas de démarrage de dosage car `error` sera NAN → `shouldStartDosing()` retourne `false` ou résultats imprévisibles). **Point d'attention : pas de badge « capteur stale » côté UI, pas de timeout de péremption.** Voir §20 pour évolution.
- `sensors.isInitialized()` est la seule porte en amont : si faux, toutes les pompes sont stoppées ([src/pump_controller.cpp:335](../../../src/pump_controller.cpp:335)).

### 14. Historique / persistance des mesures
- `HistoryManager` ([src/history.h:27](../../../src/history.h:27)) enregistre un point toutes les **5 min** (`RECORD_INTERVAL = 300 000 ms`).
- Données conservées : pH, ORP, température, `filtrationActive`, `phDosing`, `orpDosing`.
- Trois granularités (raw / hourly / daily), budgets : 72 raw (6 h), 360 hourly (15 j), 75 daily (75 j) — cf. [src/constants.h:49-51](../../../src/constants.h:49).
- Sauvegarde sur LittleFS toutes les 5 min (`SAVE_INTERVAL = 300 000 ms`).
- **Pas de journal par-injection** (volume, timestamp, durée par cycle) — seul l'agrégat sur 5 min indique qu'un dosage a eu lieu.

### 15. Exposition WebSocket (`/ws`)
Cadence : push toutes les **5 s** (`kSensorPushIntervalMs`), plus push initial à la connexion.
Champs régulation/dosage ([src/ws_manager.cpp:127](../../../src/ws_manager.cpp:127)) :
- Mesure : `ph`, `ph_raw`, `ph_slope`, `orp`, `orp_raw`, `temperature`, `temperature_raw`, `temperature_ambient`, `temperature_ambient_raw`
- Dosage en cours : `ph_dosing`, `orp_dosing`, `ph_used_ms`, `orp_used_ms`
- Stabilisation : `stabilization_remaining_s`
- Cumul journalier : `ph_daily_ml`, `orp_daily_ml`, `ph_limit_reached`, `orp_limit_reached`
- Injection manuelle : `ph_inject_remaining_s`, `orp_inject_remaining_s`
- Produits : `ph_tracking_enabled`, `ph_remaining_ml`, `ph_container_ml`, `ph_alert_threshold_ml` (+ ORP)
- Filtration : `filtration_running`, `filtration_force_on`, `filtration_force_off`

Champs config (push à la connexion + sur changement) :
- `ph_target`, `orp_target`, `ph_enabled`, `orp_enabled`, `ph_pump`, `orp_pump`, `ph_correction_type`
- `pump1_max_duty_pct`, `pump2_max_duty_pct`, `ph_limit_seconds`, `orp_limit_seconds`
- `regulation_mode`, `regulation_speed` (via `/get-config`), `stabilization_delay_min` (via `/get-config`), `min_pause_between_min` (via `/get-config`)
- `max_ph_ml_per_day`, `max_chlorine_ml_per_day`
- Calibration : `ph_cal_valid`, `ph_calibration_date`, `ph_calibration_temp`, `orp_cal_valid`, `orp_calibration_date`, `orp_calibration_reference`, `orp_calibration_temp`

### 16. Exposition MQTT / Home Assistant
Publication état toutes les **10 s** (`kMqttPublishIntervalMs = 10 000`). Auto-discovery HA ([src/mqtt_manager.cpp:326+](../../../src/mqtt_manager.cpp:326)) :
- `sensor.*_ph`, `sensor.*_orp`, `sensor.*_temperature`, `sensor.*_ph_remaining`, `sensor.*_orp_remaining`
- `number.*_ph_target`, `number.*_orp_target` (command topics `/ph_target/set`, `/orp_target/set`)
- `binary_sensor.*_ph_dosing`, `*_orp_dosing`, `*_ph_limit`, `*_orp_limit`, `*_ph_stock_low`, `*_orp_stock_low`, `*_filtration`, `*_status`
- `switch.*_filtration_switch`, `*_lighting`
- `select.*_filtration_mode`
- **Pas d'entité HA pour** : activer/désactiver la régulation pH/ORP, changer le mode `pilote` / `continu`, changer la vitesse de régulation.

### 17. Dashboard
[data/index.html:253](../../../data/index.html:253) carte pH, [data/index.html:279](../../../data/index.html:279) carte ORP :
- Valeur + cible configurée
- Badge « dosing-status » pendant une injection
- Badge « sensor-badge » en cas d'anomalie capteur (logique frontend)
- Bandeau « Calibration requise » si `*_cal_valid = false`
- Mini-graphique (Chart.js)
- **Pas d'affichage** du cumul journalier, du temps de stabilisation restant, ni de la limite horaire.

### 18. Paramètres modifiables (page `/settings`)
Section Régulation pH/ORP ([data/index.html:1256](../../../data/index.html:1256)) — sauvegarde via `POST /api/config` :
- `regulation_mode` (pilote / continu)
- `regulation_speed` (slow / normal / fast)
- `ph_limit` (s/h, libellé « pH — durée max / heure »)
- `ph_daily_limit` (mL/j, libellé « pH — volume max / jour »)
- `orp_limit` (s/h)
- `orp_daily_limit` (mL/j)
- `min_pause_between_min` (pause entre deux injections, 1-120 min)
- `stabilization_delay_min` (délai au démarrage filtration, 0-60 min)

Section Pompes :
- `pump1_max_duty_pct`, `pump2_max_duty_pct` (0-100 %)
- `pump_max_flow_ml_per_min` (1-500)

### 19. Paramètres en dur (non exposés dans l'UI)
Liste exhaustive à date :
- `PH_DEADBAND = 0.01` — [src/config.h:29](../../../src/config.h:29)
- `ORP_DEADBAND = 2.0` mV — [src/config.h:30](../../../src/config.h:30)
- Seuils anti-cycling `phStartThreshold = 0.05`, `phStopThreshold = 0.01`, `orpStartThreshold = 10 mV`, `orpStopThreshold = 2 mV` — [src/config.h:157-160](../../../src/config.h:157)
- `minInjectionTimeMs = 30 000` (durée minimale d'une injection) — [src/config.h:155](../../../src/config.h:155)
- `maxCyclesPerDay = 20` (nombre max de démarrages / 24 h glissantes) — [src/config.h:161](../../../src/config.h:161)
- `kPhMaxError = 1.0`, `kOrpMaxError = 200` (normalisation PID) — [src/constants.h:89-90](../../../src/constants.h:89)
- `kPumpMinFlowMlPerMin = 5.2`, `kPumpMaxFlowMlPerMin = 90` (bornes absolues) — [src/constants.h:85-86](../../../src/constants.h:85)
- `MIN_ACTIVE_DUTY = 80` (PWM minimum pour que la pompe tourne) — [src/config.h:32](../../../src/config.h:32)
- Gains PID par vitesse (slow/normal/fast) — [src/pump_controller.cpp:17](../../../src/pump_controller.cpp:17)
- `integralMax = 50` (anti-windup) — [src/pump_controller.h:35](../../../src/pump_controller.h:35)
- Cadences WS/MQTT/capteurs — [src/constants.h](../../../src/constants.h)
- Fenêtres de limite : 1 h glissante (horaire), 24 h glissantes (journalier + cycles) — hard-codées

### 20. Écarts / évolutions identifiées (pour specs filles)
1. **Cumul journalier non persistant** : `dailyPhInjectedMl`, `dailyOrpInjectedMl` sont en RAM. Un reboot remet à zéro, contournant potentiellement la limite journalière (risque sécurité chimique). → *Traité par itération 3 de cette spec.*
2. **Fenêtre « journalière » glissante au lieu de minuit local** : n'est pas alignée sur le calendrier utilisateur. → *Traité par itération 3 de cette spec (reset aligné sur minuit local via RTC).*
3. **Injection manuelle non gardée** : elle contourne `canDose()`, les limites horaires/journalières et le délai de stabilisation (elle compte dans le cumul mais peut le faire dépasser, et peut injecter filtration arrêtée). → à décider : appliquer les mêmes gardes que l'auto.
4. **Fraîcheur capteur non tracée** : pas de timestamp `lastReadMs` ni seuil de péremption ; une panne EZO silencieuse laisserait `ph`/`orp` à NAN mais sans alerte explicite. → exposer un champ WS `ph_sensor_fresh` / `orp_sensor_fresh` et un badge UI.
5. **Pas de journal par-injection** : impossible d'afficher l'historique « à 14h02, 25 mL de pH- injectés ». Seuls les agrégats 5 min existent. → ajouter un fichier `/injection.log` circulaire, endpoint `GET /api/history/injection`.
6. **Pas d'entité HA pour activer la régulation** (`ph_enabled`, `orp_enabled` ne sont pas exposés en switch). → à ajouter.
7. **Nom `maxPhMinusMlPerDay`** alors que la limite s'applique aussi à `ph_plus` : renommage `maxPhMlPerDay` pour clarté.
8. **Mode `Programmée`** (feature-003 itération 2) introduit un volume quotidien à injecter. La logique de **répartition sur 24 h** reste à définir : elle doit dialoguer avec les règles ci-dessus (pauses, max cycles, stabilisation, etc.). → spec dédiée.
9. **Dashboard** : pas de badge cumul journalier ni de « stabilisation en cours ». → décider si visible sur dashboard ou réservé aux pages /ph et /orp.
10. **Pas d'UI** pour : seuils anti-cycling, `maxCyclesPerDay`, `minInjectionTimeMs`, deadband pH/ORP, `integralMax`. À statuer : les laisser en dur (sécurité) ou exposer en mode expert.

## Contraintes techniques
- ESP32 Arduino framework, PlatformIO. Pas de FreeRTOS task dédiée à la régulation : tout passe par `loop()`.
- Persistance : LittleFS (`/config.json`, `/product.json`, `/history.json`), NVS pour certains flags d'auth.
- Mutex : `configMutex` protège les configs partagées, `i2cMutex` protège le bus I2C EZO + DS3231 + DS18B20.
- AsyncWebServer : handlers non bloquants (< 50 ms).
- Sécurité chimique : toute évolution touchant aux §4, §7, §8 → validation `pool-chemistry` obligatoire.

## Fichiers potentiellement concernés
- `src/pump_controller.cpp/.h` — logique régulation, PID, anti-cycling, cumul
- `src/config.h` — structures `MqttConfig`, `SafetyLimits`, `PumpProtection`, `ProductConfig`
- `src/config.cpp` — persistance, chargement, application
- `src/constants.h` — constantes temporelles et PID
- `src/filtration.cpp` — déclenche arm/clear du timer de stabilisation
- `src/sensors.cpp/.h` — lecture pH/ORP/temp, calibration
- `src/ws_manager.cpp` — diffusion WS
- `src/mqtt_manager.cpp` — publication MQTT + auto-discovery HA
- `src/web_routes_control.cpp` — endpoints d'injection manuelle
- `src/web_routes_config.cpp` — endpoints de config (GET/POST)
- `src/web_routes_calibration.cpp` — endpoints de calibration
- `src/history.cpp/.h` — historique 5 min
- `src/main.cpp` — arm stabilisation au boot en mode continu
- `data/index.html` (cartes dashboard, /ph, /orp, /dosages, /settings) — surfaces UI consommatrices
- `data/app.js` — logique UI de régulation, injection, dosage
- `docs/API.md`, `docs/MQTT.md` — documentation externe

## Éléments visibles (pour doc-writer)

### Endpoints HTTP consommés par les règles
- `POST /api/config` — persistance des paramètres de régulation (tous les champs §18)
- `GET /api/config`, `GET /get-config` — lecture
- `POST /ph/inject/start`, `POST /ph/inject/stop`, `POST /orp/inject/start`, `POST /orp/inject/stop` — injection manuelle (auth WRITE)
- `POST /api/calibrate_ph_neutral`, `POST /api/calibrate_ph_acid`, `POST /api/calibrate_orp` — calibration (auth WRITE)
- `GET /api/history/ph`, `GET /api/history/orp`, `GET /api/history/temp` — historique 5 min

### Champs WebSocket
Cf. §15 ci-dessus.

### Topics MQTT / Entités HA
Cf. §16 ci-dessus.

### Documentation à produire / mettre à jour
- `docs/API.md` — endpoints régulation, calibration, injection manuelle
- `docs/MQTT.md` — topics et auto-discovery HA
- `README.md` — bref rappel des règles de sécurité
- `CHANGELOG.md` — entrée à ajouter au fil des évolutions dérivées

## Plan de test manuel
(Tests ciblant les règles, indépendamment des surfaces UI.)
1. Mode **pilote** + filtration ON après 5 min de pause → timer stabilisation armé `stabilization_remaining_s` décompte puis 0 → dosage autorisé si écart > seuil.
2. Mode **pilote** + filtration OFF → `canDose()` refuse toute injection auto.
3. Filtration OFF → OFF (glitch 10 s) → ON → **pas** d'armement du timer (pause < délai).
4. Erreur pH > `phStartThreshold` mais cumul journalier proche de `maxPhMinusMlPerDay` → dosage s'arrête à la limite, flag `ph_limit_reached=true`, badge HA `binary_sensor.*_ph_limit` ON.
5. 20 démarrages dans une fenêtre 24 h glissante → 21ᵉ refusé (`maxCyclesPerDay`), warning loggé toutes les heures.
6. Fin d'injection → nouvelle demande < `minPauseBetweenMin` plus tard → refus.
7. Injection manuelle 50 mL en mode pilote, filtration OFF → la pompe tourne quand même (écart §20-3). À documenter ou corriger.
8. Reboot pendant qu'on a 250 mL au compteur du jour → cumul remis à 0 (écart §20-1). À documenter ou corriger.
9. Déconnexion physique du module EZO pH → `ph` passe NAN, **aucun badge UI stale** (écart §20-4).
10. Home Assistant : `number.*_ph_target` publié via `/ph_target/set` → `ph_target` mis à jour et rétroaffiché dans le bandeau carte pH sur dashboard.

## Critères d'acceptation
Cette spec étant **documentaire**, ses critères sont :
- [x] Toutes les règles présentes dans le code sont documentées (§1 à §18)
- [x] Les paramètres en dur sont listés (§19)
- [x] Les écarts/évolutions candidats sont identifiés (§20) pour être traités dans des specs filles
- [ ] Les nouvelles specs filles créées à partir du §20 devront référencer `feature-004` comme source
- [ ] Les specs consommatrices (`feature-003`, future spec ORP, future spec page Produits) référencent `feature-004` au lieu de redéfinir ces règles

## Notes pour agent
- **Cette spec est un contrat de référence**, elle ne produit pas de code par elle-même. Toute évolution concrète se fait via une spec fille dérivée d'un ou plusieurs écarts du §20.
- **Sécurité chimique** : toute modification des §4, §7, §8, §10, §13 → passage obligatoire par `pool-chemistry` avant `embedded-developer`.
- **Cohérence pH/ORP** : toute règle introduite pour l'un doit être évaluée pour l'autre. La symétrie actuelle est partielle (ex. ORP n'a pas de mode bidirectionnel comme pH).
- **Invariants à préserver** :
  - une injection ne démarre jamais si filtration OFF en mode pilote
  - les limites horaires et journalières sont bloquantes, même pour l'injection manuelle (à corriger via §20-3)
  - la durée minimale d'une injection (30 s) protège les pompes
- **Lien avec feature-003** : la page /ph lit/affiche les champs de §15 ; son mode **Programmée** introduira une nouvelle source de volume à injecter qui devra composer avec les règles §7 et §8.
- **Lien avec feature-002** : le bloc « Dosage du jour » affiché dans /ph et /orp consomme directement `ph_daily_ml` / `orp_daily_ml` tels que définis ici.

## Hors périmètre
- Refonte UX de la page Paramètres (tabs, ergonomie) — spec dédiée si besoin
- Ajout d'entités HA manquantes — à traiter via une spec fille §20-6
- Ajout d'un journal par-injection — spec fille §20-5
- Persistance cumul journalier — spec fille §20-1

---

## Suivi / itérations

### Itération 4 — 2026-04-24 — Exposition de la raison du dernier reboot via WebSocket

- Commit(s) : —
- Statut : livré — tests verts, revue OK
- Fichiers modifiés : `src/ws_manager.cpp`
- Contexte :
  - Quand l'ESP32 redémarre (watchdog, brownout, panic…) puis se reconnecte, l'utilisateur n'a aucune visibilité sur la cause de l'interruption.
  - Cette information permettrait de distinguer un redémarrage volontaire (OTA, secteur coupé) d'un crash inattendu (watchdog, brownout), et d'expliquer pourquoi les compteurs journaliers ont été remis à zéro avant l'itération 3.
  - Aucun impact chimique. Exposé via WebSocket pour consommation par l'UI (feature-003 itération 6).
- Écarts par rapport à l'existant :
  - **`src/ws_manager.cpp`** — `_buildSensorJson()` :
    - Ajouter un champ `reset_reason` (const char*) dans le payload `sensor_data`.
    - Valeur calculée une seule fois (constante pendant tout le runtime) via `esp_reset_reason()` :
      - `ESP_RST_POWERON` → `"POWER_ON"`
      - `ESP_RST_SW` → `"SW_RESET"` (inclut les reboots OTA)
      - `ESP_RST_TASK_WDT` / `ESP_RST_INT_WDT` / `ESP_RST_WDT` → `"WATCHDOG"`
      - `ESP_RST_BROWNOUT` → `"BROWNOUT"`
      - `ESP_RST_PANIC` → `"PANIC"`
      - `ESP_RST_DEEPSLEEP` → `"DEEP_SLEEP"`
      - `ESP_RST_EXT` → `"EXTERNAL"`
      - Tous autres → `"UNKNOWN"`
    - Stocker la chaîne dans une variable statique ou constante au début de `_buildSensorJson()`.
    - Impact sur la taille du message WS : +30 octets max, négligeable.
- Agents requis :
  - `embedded-developer`
  - `test-engineer`
  - `code-reviewer`
- Critères d'acceptation :
  - [x] Le champ `reset_reason` est présent dans chaque message WebSocket `sensor_data`
  - [x] La valeur est correcte pour les cas POWER_ON, SW_RESET, WATCHDOG, BROWNOUT, PANIC
  - [x] La valeur ne change pas pendant le runtime (calculée une fois)
  - [x] `pio run` compile sans nouveau warning
- Hors périmètre :
  - Consommation UI du champ (couverte par feature-003 itération 6)
  - Historique ou journalisation des causes de reboot

### Itération 3 — 2026-04-24 — Persistance des compteurs journaliers en NVS + reset à minuit local

- Commit(s) : —
- Statut : livré — tests verts, revue OK
- Fichiers modifiés : `src/config.h`, `src/config.cpp`, `src/constants.h`, `src/pump_controller.cpp`
- Contexte :
  - `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont **stockés uniquement en RAM** (`SafetyLimits`). Tout reboot de l'ESP32 (watchdog, brownout, coupure secteur, OTA) les remet silencieusement à zéro — potentiellement en milieu de journée — contournant la limite journalière de sécurité.
  - De plus, le reset journalier est piloté par `millis()` avec une fenêtre glissante de 24 h : il ne correspond pas à minuit local mais à « 24 h après le dernier boot », ce qui peut décaler le reset de plusieurs heures selon l'heure du reboot.
  - Bug observé : lors d'une coupure réseau ayant entraîné un reboot ESP32, le compteur de dosage affiché dans l'UI est revenu à 0 à la reconnexion, masquant le volume déjà injecté dans la journée.
  - Couvre §20-1 (cumul non persistant) et §20-2 (fenêtre glissante vs minuit) de cette spec.
- Écarts par rapport à l'existant :

  **A. `src/config.h`** — `struct SafetyLimits` :
  - Ajouter `char currentDayDate[9]` (format `YYYYMMDD\0`, initialisé à `{}`) pour stocker la date locale du jour en cours.

  **B. `src/config.cpp`** + **`src/config.h`** (déclarations) :
  - Ajouter `void saveDailyCounters()` : acquérir `configMutex` (pattern identique à `saveProductConfig()`), namespace NVS `pool-daily`, clés `ph_daily_ml` (float), `orp_daily_ml` (float), `daily_date` (String YYYYMMDD).
  - Ajouter `void loadDailyCounters()` :
    - Si la clé `daily_date` est absente (première installation, flash complet) : initialiser à 0.0f sans restore.
    - **Valider les valeurs lues** : valeur négative → forcer 0.0f ; valeur > `maxLimit × 1.1` → forcer à `maxLimit` et logguer `critical` ("NVS daily counter out of range").
    - Si `time(nullptr) >= kMinValidEpoch` (heure valide via NTP/RTC) : comparer la date lue avec la date locale actuelle (`localtime_r` + `strftime "%Y%m%d"`). Si **même jour** → restaurer les compteurs validés. Si **nouveau jour** → remettre à zéro et sauvegarder.
    - Si l'heure n'est **pas encore synchronisée** (`time < kMinValidEpoch`) : restaurer les valeurs validées de manière **conservatrice** (hypothèse même jour — vaut mieux sur-estimer que sous-estimer le dosage déjà fait).
  - Définir `kMinValidEpoch` dans `src/constants.h` : `constexpr time_t kMinValidEpoch = 1700000000;` (14 nov. 2023 — postérieure à toute date de fabrication du matériel). Cette constante est peut-être déjà définie dans `ws_manager.cpp` sous une forme locale — la déplacer dans `constants.h` si c'est le cas.

  **C. `src/pump_controller.cpp`** :
  - Appel de `loadDailyCounters()` lors du premier `update()` (flag `_dailyLoaded`). Cela laisse le temps à NTP/RTC d'initialiser l'heure avant de décider du reset.
  - Dans `checkSafetyLimits()` : **remplacer** le bloc millis()-based (lignes ~242–255) par :
    - Si heure valide (`time >= kMinValidEpoch`) : comparer `currentDayDate` à la date locale actuelle. Si différent → reset des compteurs + flags + `currentDayDate` mis à jour + appel `saveDailyCounters()` + **armer `armStabilizationTimer()`** (mitigation risque double quota — voir §Sécurité ci-dessous).
    - Sinon (fallback) : conserver le comportement millis() existant.
  - Dans `updateSafetyTracking()` : poser flag `_dailyCountersDirty = true`. **Si la session d'injection vient de démarrer** (transition `phDosingState.active` false→true ou `orpDosingState.active` false→true dans `update()`) : appeler `saveDailyCounters()` immédiatement, sans attendre le flush périodique. Cela garantit que le démarrage est persisté même en cas de coupure dans la première fenêtre de 30 s.
  - Dans `update()` : flush toutes les 30 s si `_dailyCountersDirty` (`saveDailyCounters()` + reset flag + mémorisation timestamp).

  **Sécurité — risque du reset calendaire à minuit (validé par pool-chemistry) :**
  - Scénario limite : injection de 295/300 mL avant 23h59, reset à 00h00, quota repart à 300 mL → 595 mL sur 24 h réelles.
  - Mitigation retenue : armer `armStabilizationTimer()` lors du reset de minuit. Si `stabilizationDelayMin > 0`, aucun dosage n'est possible pendant ce délai. Si `stabilizationDelayMin = 0`, l'utilisateur a délibérément désactivé ce garde — acceptable et documenté.
  - À documenter dans `docs/UPDATE_GUIDE.md` : recommander `stabilizationDelayMin ≥ 5 min` pour bénéficier de la protection au passage de minuit.

- Agents requis :
  - `pool-chemistry` — **validé GO sous conditions** (conditions intégrées ci-dessus)
  - `embedded-developer`
  - `test-engineer`
  - `code-reviewer`

- Critères d'acceptation :
  - [x] Après un reboot ESP32 le même jour calendaire, `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont restaurés depuis NVS à la valeur avant reboot (tolérance : −max 30 s d'injection non persistée)
  - [x] La limite journalière reste bloquante après reboot : si le compteur restauré atteint déjà `maxPhMinusMlPerDay`, aucun dosage n'est déclenché
  - [x] Au passage de minuit local (heure RTC/NTP), les compteurs sont remis à 0 et `armStabilizationTimer()` est armé
  - [x] Si NTP/RTC non encore synchronisé au boot (`time < kMinValidEpoch`), les valeurs NVS sont restaurées conservativement, aucun reset erroné n'est déclenché
  - [x] Les valeurs NVS corrompues (négatives ou > maxLimit×1.1) sont corrigées et loguées `critical`
  - [x] `saveDailyCounters()` est appelée immédiatement au démarrage d'une injection (en plus du flush 30 s)
  - [x] `saveDailyCounters()` acquiert `configMutex` (cohérence avec `saveProductConfig()`)
  - [x] `kMinValidEpoch` est défini dans `constants.h`
  - [x] Fréquence d'écriture NVS : au maximum une fois toutes les 30 s en dehors des démarrages d'injection
  - [x] `dayStartTimestamp` (fallback millis) reste fonctionnel quand l'heure n'est pas synchronisée
  - [x] `pio run` compile sans nouveau warning

- Hors périmètre :
  - Modifications UI (les champs `ph_daily_ml` / `orp_daily_ml` WS sont déjà affichés)
  - Modification de la logique de dosage elle-même
  - Persistance de la limite horaire (fenêtre glissante 1 h, non critique au reboot)

### Itération 2 — 2026-04-24 — limites horaires en minutes + suppression de la pause inter-injections

- Commit(s) : —
- Statut : terminé — en attente de commit
- Contexte :
  - Les limites horaires d'injection sont actuellement exprimées en **secondes** dans le code, la config NVS, l'API et l'UI (`phInjectionLimitSeconds` = 300 s, `orpInjectionLimitSeconds` = 600 s). L'unité secondes est incohérente avec la façon dont l'utilisateur raisonne (« je veux injecter au max 5 minutes par heure ») et rend la saisie dans l'UI peu naturelle.
  - Le paramètre `minPauseBetweenMin` (pause minimale entre deux injections) n'a de sens qu'en mode Automatique pour éviter le short-cycling. Avec l'introduction du mode Programmée (injection libre jusqu'au quota journalier), ce paramètre est devenu source de confusion et sera supprimé.
- Écarts par rapport à l'existant :

  **A. Config / firmware**
  - Renommer `phInjectionLimitSeconds` → `phInjectionLimitMinutes` (type `int`, défaut 5, plage 1–60) dans `MqttConfig` (`config.h`)
  - Renommer `orpInjectionLimitSeconds` → `orpInjectionLimitMinutes` (type `int`, défaut 10, plage 1–60) dans `MqttConfig`
  - Adapter le calcul de `phLimitMs` / `orpLimitMs` : `phLimitMs = phInjectionLimitMinutes * 60 000UL`
  - **Migration NVS au premier boot** : si la clé `ph_limit_sec` existe, lire sa valeur, diviser par 60, écrire dans `ph_limit_min`, puis supprimer `ph_limit_sec`. Idem pour `orp_limit_sec` → `orp_limit_min`. Si `ph_limit_sec` n'existe pas (install fraîche), initialiser `ph_limit_min` à 5 et `orp_limit_min` à 10.
  - Supprimer `minPauseBetweenMin` de `MqttConfig` et sa clé NVS `pump_pause_min`
  - Supprimer `pumpProtection.minPauseBetweenMs` de la structure `PumpProtection` (`config.h:153`)
  - Supprimer dans `shouldStartDosing()` le bloc de garde basé sur `minPauseBetweenMs` (lignes ~111-115 `pump_controller.cpp`)
  - Supprimer dans `applyConfigAndReload()` / `handleSaveConfig()` l'écriture de `minPauseBetweenMs`

  **B. API HTTP**
  - `GET /get-config` : remplacer `ph_limit_seconds` → `ph_limit_minutes`, `orp_limit_seconds` → `orp_limit_minutes`, supprimer `min_pause_between_min`
  - `POST /save-config` : même renommage, supprimer la lecture de `min_pause_between_min`
  - Pas de backward-compat nécessaire (champ renommé, ancienne valeur inutilisable dans la nouvelle unité)

  **C. WebSocket**
  - Vérifier que `ph_limit_seconds` / `orp_limit_seconds` ne sont pas poussés dans les messages WS ; si oui, les renommer

  **D. Frontend**
  - `data/index.html` : renommer les inputs correspondants, mettre les labels à jour (« min/h » au lieu de « s/h »), ajuster les attributs `min`/`max`/`step`
  - `data/app.js` : mettre à jour `collectConfig()` et `applyConfig()` pour les nouveaux noms de champs ; supprimer toute référence à `min_pause_between_min`

  **E. MQTT / Home Assistant**
  - Vérifier si `ph_limit_seconds` ou `orp_limit_seconds` apparaissent dans les topics ou l'auto-discovery ; mettre à jour si nécessaire

- Agents requis :
  - `pool-chemistry` — validation obligatoire (modification des bornes temporelles de sécurité)
  - `embedded-developer` — firmware : renommage, migration NVS, suppression `minPauseBetweenMs`
  - `web-ui-developer` — API, WS, frontend, MQTT
  - `test-engineer` — `pio run` + `./build_fs.sh`
  - `code-reviewer`
  - `doc-writer` — `docs/API.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`

- Critères d'acceptation :
  - [x] `phInjectionLimitMinutes` et `orpInjectionLimitMinutes` sont les seuls champs dans `MqttConfig` ; `phInjectionLimitSeconds` et `orpInjectionLimitSeconds` n'existent plus
  - [x] Migration NVS : sur un ESP32 ayant l'ancienne config (`ph_limit_sec = 300`), après mise à jour, `ph_limit_min = 5` et `ph_limit_sec` n'existe plus
  - [x] `minPauseBetweenMin` et `pumpProtection.minPauseBetweenMs` sont supprimés du code, de la config NVS, de l'API et de l'UI
  - [x] `shouldStartDosing()` ne référence plus `minPauseBetweenMs`
  - [x] Les inputs UI affichent l'unité « min/h », les valeurs par défaut sont 5 et 10
  - [x] `pio run` compile sans nouveau warning
  - [x] `./build_fs.sh` passe

- Hors périmètre :
  - Toute modification de l'algorithme scheduled (couverte par feature-003 itération 5)
  - `minInjectionTimeMs` (durée minimale d'injection en mode automatique) : conservé tel quel

### Itération 1 — 2026-04-23 — baseline documentaire
- Commit(s) : —
- Statut : point de départ documentaire (pas de livraison associée à cette itération)
- Résumé :
  - Première version, produite par lecture systématique de `src/` et `data/index.html` (voir les références `file:line` dans le corps).
  - Documente l'état actuel des règles de régulation pH et ORP, des limites de sécurité, de la protection des pompes, du suivi des produits, de l'exposition WS/MQTT et des surfaces UI consommatrices.
  - Inventorie les **paramètres en dur** (§19) et les **écarts / évolutions identifiés** (§20) qui deviendront le périmètre des specs filles.
  - Les itérations ultérieures consigneront chaque spec fille créée (ID, lien, écart couvert) et la mise à jour progressive de cette référence si les règles évoluent.
