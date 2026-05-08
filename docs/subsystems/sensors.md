# Subsystem — `sensors`

- **Fichiers** : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp), [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- **Singleton** : `extern SensorManager sensors;`
- **Responsabilité** : lecture pH (Atlas EZO 0x63), ORP (Atlas EZO 0x62), température eau + circuit (DS18B20 multi-sondes) ; calibration EZO en interne ; exposition des valeurs et de l'état de calibration aux consommateurs (PID, WS, MQTT, UART, HTTP).

> **PCB v2, firmware ≥ 2.0.0** — la chaîne analogique ADS1115 + `DFRobot_PH` a été supprimée (cf. [ADR-0014](../adr/0014-migration-atlas-ezo.md), [feature-021](../../specs/features/done/feature-021-migration-atlas-ezo.md)).

## Architecture

| Capteur | Interface | Lib | Adresse / Pin |
|---------|-----------|-----|---------------|
| pH | I²C — Atlas EZO Embedded | maison ([`AtlasEzoSensor`](../../src/atlas_ezo.h)) | `kEzoPhAddress = 0x63` |
| ORP | I²C — Atlas EZO Embedded | maison ([`AtlasEzoSensor`](../../src/atlas_ezo.h)) | `kEzoOrpAddress = 0x62` |
| Température (eau + circuit) | DS18B20 1-Wire | [OneWire](https://github.com/PaulStoffregen/OneWire), [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) | `kTempSensorPin = 5` ([`constants.h`](../../src/constants.h)) — bus partagé entre les 2 sondes |

Bus I²C partagé : `kI2cSdaPin = 21`, `kI2cSclPin = 22`, partagé entre **DS3231 RTC + EZO pH + EZO ORP**. Toute transaction est sérialisée par le mutex `i2cMutex` (timeout `kI2cMutexTimeoutMs = 2000 ms`).

Voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) (mapping pins) et [ADR-0014](../adr/0014-migration-atlas-ezo.md) (migration logicielle EZO).

## Mini-classe `AtlasEzoSensor`

Encapsule la communication I²C avec un module EZO et le timing requis par le firmware Atlas. Méthodes publiques (toutes prennent `i2cMutex` en interne, sauf indication) :

| Méthode | Effet |
|---------|-------|
| `bool sendCmd(const char* cmd)` | Envoie une commande ASCII brute (ex `"R"`, `"RT,25.5"`). **Mutex à la charge de l'appelant.** |
| `int readResponse(char* buf, size_t bufLen, uint32_t delayMs)` | Lit la réponse après attente. **Mutex à la charge de l'appelant.** |
| `bool readSingle(float& out, float tempC)` | Séquence atomique `RT,<temp>` + delay 600 ms + `R` + delay 900 ms + parse float. Prend le mutex. |
| `bool calibrate(const char* arg)` | Envoie `Cal,<arg>` (ex `"mid,7.00"`, `"low,4.00"`, `"470"`). Prend le mutex. |
| `bool clearCalibration()` | `Cal,clear` — efface toute la calibration mémorisée dans le module. |
| `int queryCalPoints()` | `Cal,?` → renvoie -1 (injoignable) ou 0..3. |
| `bool readInfo(String& fw)` | `I` — version firmware module (utilisé au boot pour log diagnostique). |

**Codes de retour Atlas** parsés en interne :
- `1` → succès (réponse utile suit)
- `2` → erreur de syntaxe
- `254` → commande pas encore prête (re-essayer)
- `255` → pas de données

## API publique `SensorManager`

```cpp
// Cycle
void begin();
void update();                          // appelé dans loopTask, non bloquant côté HTTP

// Lectures EZO — NaN si stale (> kSensorStaleTimeoutMs) ou jamais lu valide
float getPh()  const;
float getOrp() const;

// Statut calibration EZO — version live (lecture I²C bloquante ~900 ms, diagnostic)
int getPhCalibrationPoints();           // -1 = injoignable, 0..3 sinon
int getOrpCalibrationPoints();          // -1 = injoignable, 0..1 sinon

// Statut calibration EZO — version cache (atomique, hot path : WS, PID)
int getPhCalibrationPointsCached()  const;  // -1 si bus dégradé / jamais lu
int getOrpCalibrationPointsCached() const;

// Enqueue de commandes longues (handlers async safe, < 1 ms)
bool enqueueCalibratePhMid();           // Cal,mid,7.00
bool enqueueCalibratePhLow();           // Cal,low,4.00
bool enqueueCalibrateOrp(float refMv);  // Cal,<refMv>
bool enqueueClearPhCalibration();
bool enqueueClearOrpCalibration();

// Diagnostic global
bool isInitialized() const;             // true si au moins un EZO a répondu + 1 lecture valide

// DS18B20 (feature-020) — inchangé
float getTemperature() const;           // alias rétrocompat de getWaterTemperature() avec fallback
float getWaterTemperature() const;
float getCircuitTemperature() const;
bool  areSondesIdentified() const;
int   getDetectedSondeCount() const;
bool  identifySonde(const uint8_t addr[8], bool isWater);
void  resetSondeIdentification();
```

> **Fonctions supprimées avec la migration** (cf. [ADR-0014](../adr/0014-migration-atlas-ezo.md)) :
> `getRawPh`, `getRawOrp`, `getPhVoltageMv`, `isPhCalibrated`, `getRawTemperature`, `calibratePhNeutral/Acid/Alkaline`, `clearPhCalibration`, `detectAdsIfNeeded`, `recalculateCalibratedValues`, `publishValues`. Les consommateurs ont été migrés vers les nouvelles primitives.

## Cycle de lecture

`SensorManager::update()` est appelé en continu depuis `loopTask` :

1. **Dépile au plus 1 commande de la queue `_ezoQueue`** (`_processEzoQueue`). Une calibration prend ~900-1800 ms — sérialisée pour ne pas bloquer trop longtemps les autres consommateurs de `loopTask`.
2. **Lecture DS18B20** toutes les `kTempSensorIntervalMs = 2000 ms` (`_readDs18b20s`).
3. **Lecture EZO pH puis ORP** toutes les `kPhOrpSensorIntervalMs = 5000 ms` (`_readEzoSensors`) :
   - Récupère T° eau via `getWaterTemperature()`. Fallback **25.0 °C** si NaN (sonde non identifiée ou en erreur).
   - Acquiert `i2cMutex` (timeout `kI2cMutexTimeoutMs`). En cas d'échec d'acquisition : `_phI2cFailStreak++` et lecture sautée.
   - **Séquence atomique** sous mutex tenu : `RT,<temp>` + delay 600 ms + `R` + delay 900 ms + parse. Le mutex est **conservé pendant tout le délai** (condition #6 pool-chemistry — empêche qu'une lecture DS3231 s'intercale entre `RT` et `R`).
   - Mise à jour `_lastPh` / `_lastPhMs` (atomique champ par champ sur Xtensa LX6).
4. **Stale check** (`_checkStaleAndLog`) : log `critical` une seule fois quand une lecture passe `> kSensorStaleTimeoutMs = 20000 ms` (transition).

## Cache calibration EZO (`_phCalCachedPoints` / `_orpCalCachedPoints`)

Les chemins chauds (PID 100 Hz, broadcast WS 5 s, MQTT 10 s) ne peuvent pas tolérer une lecture I²C bloquante de 900 ms. Le firmware maintient un cache :

- **Initialisation** : `begin()` interroge `Cal,?` une fois par capteur. Si succès → `0..3`. Si EZO injoignable → `-1`.
- **Mise à jour** : à chaque calibration ou clear EZO réussie via `_processEzoQueue()`, le cache est rafraîchi par un nouveau `Cal,?`.
- **Invalidation en mode dégradé** (correctif Pass 3.5) : si `_phI2cFailStreak >= kEzoBusFailMaxConsecutive = 2`, alors `_lastPh = NaN` ET `_phCalCachedPoints = -1`. Idem ORP. Évite que `canDose()` autorise un dosage avec un cache `cal_points = 2` figé alors que le bus est tombé.
- **Rafraîchissement opportuniste** : à la 1ʳᵉ lecture EZO réussie suivante, `_phI2cFailStreak = 0` et un `Cal,?` est ré-émis.

`getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` retournent **toujours** ces caches (sans toucher au bus). `getPhCalibrationPoints()` / `getOrpCalibrationPoints()` (sans suffixe) déclenchent un appel I²C live et sont réservés aux routes HTTP de diagnostic + `mqttTask`.

## Queue FreeRTOS `_ezoQueue`

- **Capacité** : `kEzoQueueLen = 4` slots (`QueueHandle_t`).
- **Producteurs** : routes HTTP `/calibrate_*`, commandes UART écran, futurs handlers (boot wizard).
- **Consommateur** : `loopTask` via `_processEzoQueue()` — dépile **au plus 1 commande par cycle** pour limiter la durée d'occupation du bus.
- **Saturation** : `enqueue*()` retourne `false`. La route HTTP renvoie `503 calibration queue saturée — réessayer dans 1s`.
- **Acquittement** : la mise en queue est l'ack de la route HTTP (`{success:true, queued:true}`). L'UI observe l'avancement via les champs WS `phCalPoints` / `orpCalPoints` rafraîchis automatiquement après chaque calibration.

## Compensation T° du pH

`RT,<temp>` est envoyé **avant chaque `R`** sur l'EZO pH. Format : 1 décimale (ex `RT,25.3`). Source : `getWaterTemperature()` ([feature-020](../../specs/features/done/feature-020-deux-sondes-temperature.md), sonde DS18B20 identifiée comme « eau »).

Si la sonde eau n'est pas identifiée OU retourne `NaN` : fallback **25.0 °C**. L'erreur résiduelle sur la mesure pH reste < 0.1 pH dans la plage 15-30 °C piscine, jugée acceptable.

L'EZO ORP n'a pas besoin de compensation T° : la mesure ORP est intrinsèquement température-dépendante mais cette dépendance n'est pas linéaire et Atlas n'expose pas de commande de compensation pour ce module.

## Stale timeout (pool-chemistry condition #1)

`getPh()` / `getOrp()` retournent `NaN` si `millis() - _lastPhMs > kSensorStaleTimeoutMs = 20000 ms` (resp. `_lastOrpMs`).

Conséquences :
- `canDose(0)` / `canDose(1)` retournent `false` → dosage refusé fail-closed.
- Logger `critical` 1× à la **transition** vers stale (flag `_phStaleLogged` / `_orpStaleLogged`).
- Alerte MQTT `pool/alerts/sensor_stale` publiée edge-triggered (cf. [docs/MQTT.md](../MQTT.md)).

## Constantes clés

| Constante | Valeur | Usage |
|-----------|--------|-------|
| `kEzoPhAddress` | `0x63` | Adresse I²C EZO pH (défaut Atlas) |
| `kEzoOrpAddress` | `0x62` | Adresse I²C EZO ORP (défaut Atlas) |
| `kEzoReadDelayMs` | `900` ms | Délai après commande `R` (lecture) |
| `kEzoCalDelayMs` | `900` ms | Délai après commande `Cal,*` |
| `kEzoRtDelayMs` | `600` ms | Délai après commande `RT,<temp>` |
| `kSensorStaleTimeoutMs` | `20000` ms | Timeout pH/ORP stale (cond #1 pool-chemistry) |
| `kEzoBusFailMaxConsecutive` | `2` | Échecs I²C consécutifs → cache cal_points = -1 + lecture = NaN (cond #5) |
| `kPhOrpSensorIntervalMs` | `5000` ms | Période lecture pH/ORP |
| `kTempSensorIntervalMs` | `2000` ms | Période lecture DS18B20 |
| `kI2cMutexTimeoutMs` | `2000` ms | Timeout d'acquisition mutex I²C |

Toutes définies dans [`src/constants.h`](../../src/constants.h).

## Concurrence

- `update()` : tourne dans `loopTask` (core 1). Seul producteur des caches `_lastPh`, `_lastOrp`, `_phCalCachedPoints`, `_orpCalCachedPoints`.
- `getPh()` / `getOrp()` / `getPhCalibrationPointsCached()` : lectures atomiques (float / int 32 bits alignés sur Xtensa LX6 → instructions L32I single-cycle). Pas de mutex applicatif.
- `enqueue*()` : producteurs depuis n'importe quel core / contexte (handler HTTP core 0, UART core 1, …). FreeRTOS queue est ISR-safe.
- Mutex `i2cMutex` : acquis par `_readEzoSensors`, `_processEzoQueue`, `AtlasEzoSensor::readSingle/calibrate/clearCalibration/queryCalPoints/readInfo`, et **également par les autres consommateurs I²C** (DS3231 dans `rtc_manager`).

## Cas limites

- **EZO non détecté au boot** (cable I²C absent, alimentation EZO HS) : log `error` + `_ezoEverResponded = false` + `isInitialized() = false`. Régulation chimique automatique inhibée.
- **EZO retire son acquittement en runtime** : `_phI2cFailStreak` augmente. Au seuil `kEzoBusFailMaxConsecutive = 2`, le cache `cal_points` passe à `-1`, `_lastPh = NaN`, `canDose()` refuse. Logger `critical` 1× à la transition (flag `_phI2cDegradedLogged`).
- **Réponse EZO tronquée / parsing échoué** : compté comme un échec I²C → contribue au fail-streak.
- **Calibration en cours et lecture demandée** : sérialisées par `i2cMutex`. La lecture attend la fin de la calibration (au pire ~1.8 s).
- **DS18B20 absente** : `tempValue = NaN`. `getWaterTemperature()` retourne NaN → fallback 25.0 °C pour la compensation pH.
- **EZO froid au démarrage** (réponse `255 = no data` pendant les premières lectures) : tolérance `kEzoBusFailMaxConsecutive = 2` permet de passer 1 échec isolé avant de bloquer le dosage.

## Surveillance des valeurs aberrantes (health check)

`checkSystemHealth()` dans [`main.cpp`](../../src/main.cpp) est appelée toutes les **60 s** (`kHealthCheckIntervalMs`). Elle vérifie si chaque valeur capteur sort de sa plage de normalité :

| Capteur | Plage normale | Alerte |
|---------|--------------|--------|
| pH | [5.0 – 9.0] | warning log + MQTT `ph_abnormal` |
| ORP | [400 – 900] mV | warning log + MQTT `orp_abnormal` |
| Température eau | [5.0 – 40.0] °C | warning log + MQTT `temp_abnormal` |

Logs et alertes MQTT émis **aux transitions uniquement** (entrée et sortie de la zone anormale), pas à chaque cycle. Logs conditionnés à `authCfg.sensorLogsEnabled`.

## Interaction avec les autres composants

- [`pump_controller`](pump-controller.md) : lit `getPh()` / `getOrp()` chaque cycle pour calculer l'erreur PID. Vérifie `getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` dans `canDose(int)` (conditions #1, #2, #5).
- [`ws_manager`](ws-manager.md) : `broadcastSensorData()` publie `ph` (3 décimales), `orp`, `temperature`, `temperature_circuit`, `phCalPoints`, `orpCalPoints` toutes les 5 s.
- [`mqtt_manager`](mqtt-manager.md) : publie `{base}/ph` (3 décimales), `{base}/orp`, `{base}/temperature`, `{base}/temperature_circuit`, `{base}/ph_cal_points`, `{base}/orp_cal_points` toutes les 10 s (`kMqttPublishIntervalMs`). Alertes edge-triggered `{base}/alerts/calibration_required` et `{base}/alerts/sensor_stale`.
- [`history`](history.md) : snapshot des valeurs courantes toutes les 5 min.

## Multi-sondes DS18B20 — PCB v2 (feature-020)

Le PCB v2 ajoute une 2ᵉ sonde DS18B20 sur le même bus OneWire. Une sonde mesure l'**eau de la piscine**, l'autre est soudée sur le PCB pour mesurer la **température du circuit électronique**.

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

### Contrat mono-appelant du bus OneWire

Aujourd'hui, **un seul appelant accède au bus OneWire** : `Sensors::update()` depuis `loopTask`. Les routes HTTP `/sensors/onewire/*` lisent uniquement les caches `_sondes[].lastTempRaw` mis à jour par `update()` — elles ne déclenchent JAMAIS un `requestTemperatures()` synchrone (qui prendrait 750 ms en 12-bit, > timeout 50 ms d'AsyncWebServer). Si une feature future ajoute un autre appelant concurrent (debug, scan à la demande), il faudra introduire un mutex dédié.

### Sonde changée à chaud

Si une sonde est remplacée physiquement (adresse ROM différente), le scan au boot loggue un warning. L'identification de l'autre sonde est conservée, mais l'utilisateur doit refaire le workflow pour la sonde manquante (ou bien Réinitialiser et tout refaire).

## Fichiers liés

- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp)
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — routes refondues `/calibrate_ph`, `/calibrate_orp`, `/calibrate_clear`
- [`src/web_routes_sensor_id.cpp`](../../src/web_routes_sensor_id.cpp) — routes feature-020 inchangées
- [`src/constants.h`](../../src/constants.h) — toutes les constantes EZO + DS18B20
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001)
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0013](../adr/0013-identification-sondes-onewire.md) — identification 2 sondes DS18B20
- [feature-021](../../specs/features/done/feature-021-migration-atlas-ezo.md) — spec d'origine
