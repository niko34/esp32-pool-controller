# Subsystem — `sensors`

- **Fichiers** : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp)
- **Singleton** : `extern SensorManager sensors;`
- **Responsabilité** : lecture capteurs pH, ORP, température ; calibration pH ; exposition des valeurs brutes et calibrées.

## Matériel

| Capteur | Interface | Lib | Pin / Canal |
|---------|-----------|-----|-------------|
| pH | ADS1115 canal A0 + DFRobot_PH | [DFRobot_PH](https://github.com/DFRobot/DFRobot_PH), [Adafruit_ADS1X15](https://github.com/adafruit/Adafruit_ADS1X15) | I²C (`kI2cSdaPin=21`, `kI2cSclPin=22`) |
| ORP | ADS1115 canal A1 | Adafruit_ADS1X15 | I²C (partagé) |
| Température (eau + circuit) | DS18B20 1-Wire | [OneWire](https://github.com/PaulStoffregen/OneWire), [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) | `kTempSensorPin = 5` ([`constants.h`](../../src/constants.h)) — bus partagé entre les 2 sondes |

Voir [ADR-0001](../adr/0001-capteurs-analogiques-ads1115.md) pour la justification historique du choix ADS1115 + DFRobot_PH (PCB v1). Le PCB v2 supprime l'ADS1115 au profit de modules Atlas EZO I²C — voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) (mapping pin) et feature-020 à venir (intégration logicielle EZO).

> **PCB v2** : la 2ᵉ sonde DS18B20 (température circuit/électronique) est câblée sur le même bus `kTempSensorPin=5` que la sonde eau, mais elle ne sera détectée et utilisée par le firmware qu'à partir de feature-021. Aujourd'hui, seule la sonde eau est lue.

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

## Surveillance des valeurs aberrantes (health check)

`checkSystemHealth()` dans [`main.cpp`](../../src/main.cpp) est appelée toutes les **60 s** (`kHealthCheckIntervalMs` [`constants.h:18`](../../src/constants.h:18)).

Elle vérifie si chaque valeur capteur sort de sa plage de normalité :

| Capteur | Plage normale | Alerte |
|---------|--------------|--------|
| pH | [5.0 – 9.0] | warning log + MQTT `ph_abnormal` |
| ORP | [400 – 900] mV | warning log + MQTT `orp_abnormal` |
| Température | [5.0 – 40.0] °C | warning log + MQTT `temp_abnormal` |

Les logs et alertes MQTT ne sont émis qu'**aux transitions** (entrée et sortie de la zone anormale), pas à chaque cycle de 60 s. Un message `info` est logué lors du retour à la normale. Les logs sont conditionnés à `authCfg.sensorLogsEnabled` (paramètre `sensor_logs_enabled` dans la config).

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
- [`src/constants.h`](../../src/constants.h) — `kTempSensorPin = 5` (OneWire), `kI2cSdaPin = 21`, `kI2cSclPin = 22`
- [`src/constants.h:22`](../../src/constants.h:22) — intervalles de lecture
- [ADR-0001](../adr/0001-capteurs-analogiques-ads1115.md) — choix ADS1115 (historique, PCB v1)
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calibration ORP client-side
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2 (bus OneWire conservé sur GPIO 5, ADS1115 supprimé)
