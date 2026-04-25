# Subsystem — `sensors`

- **Fichiers** : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp)
- **Singleton** : `extern SensorManager sensors;`
- **Responsabilité** : lecture capteurs pH, ORP, température ; calibration pH ; exposition des valeurs brutes et calibrées.

## Matériel

| Capteur | Interface | Lib | Pin / Canal |
|---------|-----------|-----|-------------|
| pH | ADS1115 canal A0 + DFRobot_PH | [DFRobot_PH](https://github.com/DFRobot/DFRobot_PH), [Adafruit_ADS1X15](https://github.com/adafruit/Adafruit_ADS1X15) | I²C (SDA=21, SCL=22) |
| ORP | ADS1115 canal A1 | Adafruit_ADS1X15 | I²C (partagé) |
| Température eau | DS18B20 1-Wire | [OneWire](https://github.com/PaulStoffregen/OneWire), [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) | GPIO (voir `config.h`) |

Voir [ADR-0001](../adr/0001-capteurs-analogiques-ads1115.md) pour la justification du choix ADS1115 + DFRobot_PH vs modules I²C intelligents.

## API publique

```cpp
void begin();
void update();                     // non bloquant
float getPh() const;
float getOrp() const;
float getPhVoltageMv() const;      // tension brute canal A0
float getTemperature() const;      // calibrée (avec offset)
float getRawPh() const;
float getRawOrp() const;
float getRawTemperature() const;
bool  isInitialized() const;
bool  isPhCalibrated() const;      // true si EEPROM ≠ valeurs par défaut DFRobot
void  calibratePhNeutral();        // pH 7.0
void  calibratePhAcid();           // pH 4.0
void  calibratePhAlkaline();       // pH 9.18 (présent mais non exposé dans l'UI)
void  clearPhCalibration();
void  detectAdsIfNeeded();
void  recalculateCalibratedValues();
void  publishValues();
```

## Intervalles de lecture

- pH / ORP : **5 s** (`kPhOrpSensorIntervalMs` [`constants.h:23`](../../src/constants.h:23))
- Température : **2 s** (`kTempSensorIntervalMs` [`constants.h:22`](../../src/constants.h:22))

## Filtrage

`readMedianAdsChannel()` : lecture de `kNumSensorSamples = 3` échantillons ADS1115 à 8 SPS (125 ms / échantillon), retourne la médiane. Stats optionnelles (min / max / sum) pour diagnostic.

Temps total d'une mesure pH ou ORP : `kAds1115ThreeSamplesMs = 375 ms`.

## Calibration

### pH (firmware-side)

`DFRobot_PH` persiste offset/slope dans l'**EEPROM simulée** sur flash via la lib. Endpoints HTTP :
- `POST /calibrate_ph_neutral` ([`web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp))
- `POST /calibrate_ph_acid`
- `POST /clear_ph_calibration`

Après calibration, `recalculateCalibratedValues()` recalcule immédiatement la valeur pH affichée.

Détection de calibration : `_phCalibrated` à `true` si offset ≠ valeurs par défaut DFRobot (voir `begin()` dans [`sensors.cpp`](../../src/sensors.cpp)).

### ORP (client-side)

**Pas d'endpoint firmware**. Calcul `ORP_final = ORP_raw × slope + offset` côté firmware, mais offset/slope calculés et sauvegardés côté UI via `POST /save-config` (`orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_date`, `orp_calibration_reference`). Voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md).

### Température (client-side)

`temp_final = temp_raw + tempCalibrationOffset`. Offset calculé dans l'UI (`ref − raw`), sauvegardé via `POST /save-config` (`temp_calibration_offset`, `temp_calibration_date`).

## Cas limites

- **ADS1115 absent** (pas de pull-ups, mauvais branchement I²C) : `adsAvailable = false`, pH/ORP restent à `NAN`, WS publie `--`.
- **DS18B20 absent** : `tempValue = NAN`, UI affiche `--` + badge erreur.
- **Tension pH instable (dérive > 50 mV)** : reste mesurée, mais l'UI affiche un avertissement en mode calibration (voir `data/app.js`).
- **Mutex I²C** : partagé avec le RTC et l'OLED (si présent). Timeout `kI2cMutexTimeoutMs = 2000 ms`.

## Interaction avec les autres composants

- **`pump_controller`** : lit `getPh()` / `getOrp()` chaque cycle pour calculer l'erreur PID.
- **`ws_manager`** : `broadcastSensorData()` publie `ph`, `orp`, `temperature` + valeurs brutes toutes les 5 s.
- **`mqtt_manager`** : `publishSensorState()` publie `{base}/ph`, `{base}/orp`, `{base}/temperature` toutes les 10 s (`kMqttPublishIntervalMs`).
- **`history`** : lit les valeurs actuelles pour snapshot toutes les 5 min.

## Fichiers liés

- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp)
- [`src/constants.h:22`](../../src/constants.h:22) — intervalles de lecture
- [ADR-0001](../adr/0001-capteurs-analogiques-ads1115.md) — choix ADS1115
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calibration ORP client-side
