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
- [ADR-0013](../adr/0013-identification-sondes-onewire.md) — identification 2 sondes DS18B20 par adresse ROM persistée NVS (feature-020)

## Multi-sondes DS18B20 — PCB v2 (feature-020)

Le PCB v2 ajoute une **2ᵉ sonde DS18B20** sur le même bus OneWire (`kTempSensorPin = 5`). Une sonde mesure l'**eau de la piscine**, l'autre est soudée sur le PCB pour mesurer la **température du circuit électronique**.

### Identification

Les adresses ROM 1-Wire (8 octets) étant uniques à la fabrication et l'ordre de scan non garanti entre PCB, l'utilisateur doit identifier chaque sonde via le workflow UI (Paramètres → Avancé → card « Identification des sondes »). L'adresse de chaque sonde est persistée en NVS sous les clés `kNvsKeyOwWaterAddr = "ow_water_addr"` et `kNvsKeyOwCircuitAddr = "ow_circuit_addr"` (8 octets binaires via `Preferences::putBytes`).

API publique exposée par `Sensors` :

| Méthode | Comportement |
|---------|--------------|
| `getWaterTemperature()` | T° de la sonde marquée « eau » ; NaN si non identifiée |
| `getCircuitTemperature()` | T° de la sonde marquée « circuit » ; NaN si non identifiée |
| `getTemperature()` | **Alias rétrocompat** : retourne `getWaterTemperature()` ; **fallback** sur la T° de la 1ʳᵉ sonde présente si NaN. Garantit qu'aucun consommateur existant (MQTT, WS, HA) ne casse tant que l'utilisateur n'a pas fait l'identification |
| `areSondesIdentified()` | true ssi les 2 adresses NVS matchent 2 sondes détectées |
| `getDetectedSondeCount()` | 0, 1 ou 2 |
| `identifySonde(addr, isWater)` | Persiste l'adresse en NVS + **auto-permutation** si une autre sonde avait déjà ce rôle |
| `resetSondeIdentification()` | Efface les 2 clés NVS |

### Auto-permutation

Si l'utilisateur identifie la sonde A comme « eau » alors qu'une autre sonde B était déjà marquée « eau », B bascule automatiquement à « circuit » (son adresse est ré-écrite en NVS). Log info : `"Sonde XXXX permutée eau→circuit (suite à identification de YYYY comme eau)"`. Cohérent avec le workflow UI à un seul clic décisif.

### Calibration

Le `tempCalibrationOffset` (calibration utilisateur via Paramètres → Calibrations) **ne s'applique qu'à la T° eau** (`getWaterTemperature()` et le fallback `getTemperature()`). La T° circuit reste **brute** : la précision usine DS18B20 (±0.5 °C) est suffisante pour la surveillance interne du boîtier.

### ⚠️ Contrat mono-appelant du bus OneWire

Aujourd'hui, **un seul appelant accède au bus OneWire** : `Sensors::update()` depuis `loopTask`. Les routes HTTP `/sensors/onewire/*` lisent uniquement les caches `_sondes[].lastTempRaw` mis à jour par `update()` — elles ne déclenchent JAMAIS un `requestTemperatures()` synchrone (qui prendrait 750 ms en 12-bit, > timeout 50 ms d'AsyncWebServer). Si une feature future ajoute un autre appelant concurrent (debug, scan à la demande), il faudra introduire un mutex dédié.

### Sonde changée à chaud

Si une sonde est remplacée physiquement (adresse ROM différente), le scan au boot loggue un warning. L'identification de l'autre sonde est conservée, mais l'utilisateur doit refaire le workflow pour la sonde manquante (ou bien Réinitialiser et tout refaire).
