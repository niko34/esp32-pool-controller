# Subsystem — `pump_controller`

- **Fichiers** : [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
- **Singleton** : `extern PumpControllerClass PumpController;`
- **Responsabilité** : régulation PID pH/ORP + anti-cycling + cumuls journaliers + mode manuel + stabilisation filtration → pompes doseuses.

## Rôle

Pilote les deux pompes doseuses (PWM sur MOSFET IRLZ44N) selon le mode sélectionné (`automatic` / `scheduled` / `manual`), les valeurs cibles, l'état de la filtration, les limites de sécurité et les cumuls journaliers. C'est le composant le plus critique côté chimie — **toute modification passe obligatoirement par l'agent `pool-chemistry`**.

## API publique

```cpp
void begin();
void update();          // appelé dans loop()
void stopAll();
void setOtaInProgress(bool);
bool isPhDosing() const;
bool isOrpDosing() const;
unsigned long getPhUsedMs() const;
unsigned long getOrpUsedMs() const;
void applyRegulationSpeed();   // applique kp/ki/kd selon mqttCfg.regulationSpeed
void setPhPID(float kp, float ki, float kd);
void setOrpPID(float kp, float ki, float kd);
void resetDosingStates();
void resetPhPauseGuard();
void armStabilizationTimer();
void clearStabilizationTimer();
unsigned long getStabilizationRemainingS() const;
void setManualPump(int pumpIndex, uint8_t duty);  // test manuel
```

Voir [`pump_controller.h`](../../src/pump_controller.h).

## Boucle d'exécution

`PumpController.update()` est invoquée depuis [`main.cpp:181`](../../src/main.cpp:181) à chaque tour de `loop()`. Le `loop()` se termine par `delay(kLoopDelayMs)` (= 10 ms, [`constants.h:10`](../../src/constants.h:10)) → fréquence pratique ~100 Hz, mais les capteurs ont leur propre throttling interne (pH/ORP toutes les 5 s, DS18B20 toutes les 2 s) lu via `sensors.getPh()` / `getOrp()`.

Ordre par cycle :
1. Consommation des resets atomiques (`_resetRequested`, `_phPauseResetRequested`) — évite les races inter-core (web handler vs loop).
2. Chargement différé des cumuls NVS : `_dailyLoaded` reste à `false` tant que NTP/RTC n'a pas synchronisé une date valide (sinon on chargerait sur une date 1970 fantôme).
3. **Court-circuit OTA** : `otaInProgress` actif → `applyPumpDuty(0,0)` + `applyPumpDuty(1,0)` puis `return`. Pompes coupées tant que l'OTA dure.
4. Refresh des fenêtres glissantes 1 h (`refreshDosingState`).
5. Court-circuit capteurs : `!sensors.isInitialized()` → arrêt pompes.
6. Gate `canDose()` (cf. ci-dessous) → arrêt pompes si non autorisé (mais respecte `manualMode[i]` pour ne pas couper un test développeur en cours).
7. Pour pH puis ORP : calcul de l'erreur, anti-cycling start/stop, PID, conversion duty via `flowToDuty()`, `applyPumpDuty()`.

## Algorithme (résumé)

1. **Gate `canDose()`** bloque le dosage si :
   - Mode régulation = `manual`
   - Mode régulation = `pilote` + filtration à l'arrêt (sauf mode `scheduled`)
   - OTA en cours
   - Timer de stabilisation non expiré
2. **Calcul de l'erreur** ([`pump_controller.cpp:432`](../../src/pump_controller.cpp:432)) — deux modes exclusifs sélectionnés via `mqttCfg.phCorrectionType` :

   | Mode | Formule erreur | Direction du dosage |
   |------|----------------|---------------------|
   | `ph_minus` (acide, défaut) | `error = pH_mesuré − phTarget` | dose si `error > 0` (pH trop haut) |
   | `ph_plus` (base) | `error = phTarget − pH_mesuré` | dose si `error > 0` (pH trop bas) |
   | ORP | `error = orpTarget − ORP_mesuré` | dose chlore si `error > 0` (ORP trop bas) |

3. **PID** : `computePID(pid, error, now)` avec anti-windup (`integralMax = 50.0`). `applyRegulationSpeed()` réécrit kp/ki/kd selon `mqttCfg.regulationSpeed` ∈ {`slow`, `normal`, `fast`} ([`pump_controller.cpp:17`](../../src/pump_controller.cpp:17)) :

   | Vitesse | Kp | Ki | Kd |
   |---------|----|----|----|
   | `slow` | 3 | 0.05 | 12 |
   | `normal` (défaut) | 6 | 0.1 | 8 |
   | `fast` | 12 | 0.2 | 4 |

   Normalisation d'erreur (bornée) : `kPhMaxError = 1.0`, `kOrpMaxError = 200 mV` ([`constants.h:89`](../../src/constants.h:89)). `dt` PID plafonné : deltas > 10 s ignorés.
4. **Anti-cycling** ([`config.h:152`](../../src/config.h:152) `PumpProtection`) — **en dur, non configurables via UI** :
   - `minInjectionTimeMs = 30000` — 30 s minimum par injection démarrée.
   - Hystérésis de seuils (démarrage / arrêt) :

     | Grandeur | Deadband | Seuil démarrage | Seuil arrêt |
     |----------|----------|-----------------|-------------|
     | pH | `PH_DEADBAND = 0.01` | `phStartThreshold = 0.05` | `phStopThreshold = 0.01` |
     | ORP | `ORP_DEADBAND = 2.0 mV` | `orpStartThreshold = 10 mV` | `orpStopThreshold = 2 mV` |

   - `maxCyclesPerDay = 20` — démarrages comptés dans une fenêtre 24 h glissante.
5. **Limites de sécurité** ([`config.h:141`](../../src/config.h:141) `SafetyLimits`) :
   - Horaire : `ph_limit_minutes` / `orp_limit_minutes` dans une fenêtre glissante de 1 h (`windowStart` / `usedMs` dans `DosingState`). **Non reflétée dans l'UI** (pas de badge dédié à date).
   - Journalière : `maxPhMinusMlPerDay = 300`, `maxChlorineMlPerDay = 500`.
6. **Cumul journalier persisté** : `dailyPhInjectedMl` / `dailyOrpInjectedMl`, persistés en NVS, reset à minuit local (détection via `currentDayDate[9]`). Voir [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md).

## Stabilisation au démarrage filtration

Géré par [`filtration.cpp:213`](../../src/filtration.cpp:213) (démarrage) et [`filtration.cpp:252`](../../src/filtration.cpp:252) (arrêt) :

- **Démarrage filtration en mode `pilote`** : `armStabilizationTimer()` n'est appelé **que si** la pause précédente est plus longue que `stabilizationDelayMin` (`pauseMs > stabilizationMs`). Empêche le réarmement après un glitch très court (sauvegarde config, redémarrage relais involontaire). Au tout premier démarrage (`lastStoppedAtMs == 0`), la pause est considérée infinie → timer armé.
- **Arrêt filtration en mode `pilote`** : `clearStabilizationTimer()`.
- Mode `continu` : aucun appel automatique. Le timer peut être armé manuellement ou ignoré selon le besoin.
- **Mitigation double quota** : `armStabilizationTimer()` est aussi appelé au passage de minuit ([`pump_controller.cpp:268`](../../src/pump_controller.cpp:268)) pour éviter qu'un cumul reset déclenche immédiatement un dosage.

`stabilizationDelayMin` est **configurable via `/save-config`** (plage 0-60 min, défaut 5). Valeur 0 = stabilisation désactivée.

## Conversion duty ↔ débit

Implémentée dans [`pump_controller.cpp:227`](../../src/pump_controller.cpp:227) (`dutyToFlow`) et [`pump_controller.cpp:233`](../../src/pump_controller.cpp:233) (`flowToDuty`) :

- `MIN_ACTIVE_DUTY = 80` ([`config.h:28`](../../src/config.h:28)) — duty PWM minimum sous lequel la pompe ne tourne pas (vaincre couple statique).
- `MAX_PWM_DUTY = 255` ([`config.h:27`](../../src/config.h:27)) — résolution PWM 8 bits.
- `kPumpMinFlowMlPerMin = 5.2`, `kPumpMaxFlowMlPerMin = 90.0` ([`constants.h:85`](../../src/constants.h:85)) — débits par défaut, surchargés par `pumpMaxFlowMlPerMin` configuré.

Formule (interpolation linéaire) :

```
flow = minFlow + ((duty − MIN_ACTIVE_DUTY) / (MAX_PWM_DUTY − MIN_ACTIVE_DUTY)) × (maxFlow − minFlow)   si duty ≥ 80
flow = 0                                                                                                   si duty <  80

duty = MIN_ACTIVE_DUTY + round( ((flow − minFlow) / (maxFlow − minFlow)) × (MAX_PWM_DUTY − MIN_ACTIVE_DUTY) )
```

`flowToDuty` clampe `flow` à `[minFlow, maxFlow]` avant calcul. `flow ≤ 0` → duty 0.

## Paramètres en dur (récapitulatif)

| Catégorie | Constante | Valeur | Source |
|-----------|-----------|--------|--------|
| PID — anti-windup | `integralMax` | 50.0 | `PIDState` ([`pump_controller.h`](../../src/pump_controller.h)) |
| PID — borne erreur pH | `kPhMaxError` | 1.0 | [`constants.h:89`](../../src/constants.h:89) |
| PID — borne erreur ORP | `kOrpMaxError` | 200.0 mV | [`constants.h:90`](../../src/constants.h:90) |
| PID — `dt` max | (literal in code) | 10 s | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| Anti-cycling — durée min | `minInjectionTimeMs` | 30000 | [`config.h:154`](../../src/config.h:154) |
| Anti-cycling — start pH | `phStartThreshold` | 0.05 | [`config.h:155`](../../src/config.h:155) |
| Anti-cycling — stop pH | `phStopThreshold` | 0.01 | [`config.h:156`](../../src/config.h:156) |
| Anti-cycling — start ORP | `orpStartThreshold` | 10.0 mV | [`config.h:157`](../../src/config.h:157) |
| Anti-cycling — stop ORP | `orpStopThreshold` | 2.0 mV | [`config.h:158`](../../src/config.h:158) |
| Anti-cycling — cycles/jour | `maxCyclesPerDay` | 20 | [`config.h:159`](../../src/config.h:159) |
| Deadband pH | `PH_DEADBAND` | 0.01 | [`config.h:25`](../../src/config.h:25) |
| Deadband ORP | `ORP_DEADBAND` | 2.0 mV | [`config.h:26`](../../src/config.h:26) |
| Pompe — duty min actif | `MIN_ACTIVE_DUTY` | 80 (sur 255) | [`config.h:28`](../../src/config.h:28) |
| Pompe — duty max | `MAX_PWM_DUTY` | 255 (8 bits) | [`config.h:27`](../../src/config.h:27) |
| Pompe — débit min | `kPumpMinFlowMlPerMin` | 5.2 mL/min | [`constants.h:85`](../../src/constants.h:85) |
| Pompe — débit max | `kPumpMaxFlowMlPerMin` | 90.0 mL/min | [`constants.h:86`](../../src/constants.h:86) |

> Une refonte est prévue pour rendre une partie de ces paramètres modifiables via un mode expert UI (cf. spec en cours).

## Mode `scheduled`

Injecte jusqu'à `phDailyTargetMl` / `orpDailyTargetMl` **pendant la filtration**, **aveugle à la mesure capteur**. Le volume à injecter est réparti sur les plages de filtration. Borné par `ph_limit_minutes`, `max_ph_ml_per_day`, et `maxCyclesPerDay`. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).

## Interaction avec les autres composants

| Composant | Interaction |
|-----------|-------------|
| [`filtration`](filtration.md) | Démarrage filtration → `armStabilizationTimer()` ; arrêt filtration → `clearStabilizationTimer()` |
| [`sensors`](sensors.md) | Lecture `getPh()` / `getOrp()` chaque cycle pour calculer l'erreur |
| [`mqtt_manager`](mqtt-manager.md) | Publication `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit` |
| [`ota_manager`](ota-manager.md) | `setOtaInProgress(true)` arrête toutes les pompes |
| [`web_routes_control`](web-server.md) | `/ph/inject/start`, `/orp/inject/start`, `/pump[12]/on` |

## État persistant (NVS)

Voir [`pump_controller.cpp`](../../src/pump_controller.cpp) `savePersistentDailyState()` / `loadPersistentDailyState()` :
- `phDailyInjected`, `orpDailyInjected` (float, mL)
- `phLimitReached`, `orpLimitReached` (bool)
- `currentDayDate` (YYYYMMDD, char[9])
- `phDosingCycles`, `orpDosingCycles` (uint16)

Flush différé : `_dailyCountersDirty` armé à chaque injection, écrit en NVS au max toutes les 60 s pour réduire la charge d'écriture.

## Cas limites

- **Démarrage sans NTP** : `_dailyLoaded = false` tant que `kMinValidEpoch` n'est pas atteint → cumul NVS chargé **après** synchro temps, sinon on risque de reset le cumul sur une date de boot fantôme (1970).
- **OTA en cours** : `setOtaInProgress(true)` appelé par `ota_manager` → `stopAll()` immédiat, pompes coupées jusqu'à fin OTA ou reboot.
- **Changement `ph_correction_type`** (pH- ↔ pH+) : `resetPhPauseGuard()` appelé depuis [`web_routes_config.cpp`](../../src/web_routes_config.cpp) pour repartir propre.

## Fichiers liés

- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
- [`src/config.h:152`](../../src/config.h:152) — `PumpProtection`
- [`src/config.h:141`](../../src/config.h:141) — `SafetyLimits`
- [`src/constants.h:84`](../../src/constants.h:84) — `kPumpMinFlowMlPerMin`, `kPumpMaxFlowMlPerMin`
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md), [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md)
- [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md) — UI consommatrices
