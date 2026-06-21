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
| `bool readSingle(float& out, float tempC)` | Séquence atomique de lecture, **différenciée par adresse I²C** (cf. [Différenciation pH / ORP](#différenciation-de-readsingle-ph--orp-v211) ci-dessous). Prend le mutex. |
| `bool calibrate(const char* arg)` | Envoie `Cal,<arg>` (ex `"mid,7.00"`, `"low,4.00"`, `"470"`). Prend le mutex. |
| `bool clearCalibration()` | `Cal,clear` — efface toute la calibration mémorisée dans le module. |
| `int queryCalPoints()` | `Cal,?` → renvoie -1 (injoignable) ou 0..3. |
| `bool readInfo(String& fw)` | `I` — version firmware module (utilisé au boot pour log diagnostique). |
| `bool querySlope(PhSlopeInfo& out)` | `Slope,?` — pente sonde pH ([feature-024](#pente-sonde-ph--feature-024)). Parsing tolérant 2 ou 3 floats. Prend le mutex. |

**Codes de retour Atlas** parsés en interne :
- `1` → succès (réponse utile suit)
- `2` → erreur de syntaxe
- `254` → commande pas encore prête (re-essayer)
- `255` → pas de données

### Différenciation de `readSingle()` pH / ORP (v2.1.1)

`AtlasEzoSensor::readSingle()` choisit la commande Atlas selon l'adresse I²C du module :

| Module | Adresse | Commande envoyée | Justification |
|--------|---------|------------------|---------------|
| EZO pH | `kEzoPhAddress = 0x63` | `RT,<tempC>` (1 décimale) | Compense la T° (équation de Nernst) ET retourne la valeur pH compensée en une seule transaction (`statut 1` + payload). |
| EZO ORP | `kEzoOrpAddress = 0x62` | `R` | L'ORP est potentiométrique direct, sans compensation T° à effectuer. La commande `RT,<t>` est ACCEPTÉE par le module (`statut 1`) mais **NE RETOURNE PAS de payload**. |

> **Bug historique fixé en v2.1.1** (commit `c0f2962`) : depuis feature-021, `RT,<temp>` était envoyé indistinctement aux deux modules. Sur l'ORP, la réponse vide était interprétée comme un échec → `_orpI2cFailStreak++` → bus I²C dégradé après `kEzoBusFailMaxConsecutive = 2` cycles → régulation ORP inhibée. Le commentaire originel prétendait à tort que « l'EZO ORP ignore RT et retourne la valeur ORP » — hypothèse non vérifiée empiriquement à l'époque, infirmée via `/debug/ezo_command` :
>
> - `0x62 cmd="RT,25.0"` → `status=1 resp=""` (vide)
> - `0x62 cmd="R"` → `status=1 resp="-369.2"` (mV)
> - `0x62 cmd="Status"` → `status=1 resp="?STATUS,P,5.24"` (module sain)

Délai après commande : `kEzoReadDelayMs = 900 ms` (datasheet Atlas) — identique pour les deux modules.

## API publique `SensorManager`

```cpp
// Cycle
void begin();
void update();                          // appelé dans loopTask, non bloquant côté HTTP

// Lectures EZO BRUTES — NaN si stale (> kSensorStaleTimeoutMs) ou jamais lu valide.
// Ne PAS utiliser pour la régulation (cf. feature-025 : le PID consomme getPhFiltered()).
float getPh()  const;
float getOrp() const;

// Chaîne de filtrage pH/ORP (feature-025) — médiane(7) + EMA. NaN si non amorcé.
float getPhRaw()  const;             // Dernière brute pH (= getPh(), NaN si stale)
float getPhMedian()  const;          // Médiane courante pH (NaN si pas de donnée)
float getPhFiltered()  const;        // pH filtré EMA — valeur PID/UI/MQTT
bool isPhFilterReady()  const;       // true après warmup + dernière mesure valide récente
bool isPhFilterUnstable()  const;    // true si trop de rejets consécutifs
uint8_t getPhRejectedCount()  const; // Compteur glissant de rejets pH (sature à 255)
float getOrpRaw()  const;
float getOrpMedian()  const;
float getOrpFiltered()  const;
bool isOrpFilterReady()  const;
bool isOrpFilterUnstable()  const;
uint8_t getOrpRejectedCount()  const;
void resetPhFilter();                // Reset filtre (warmup) — appelé après calibration pH
void resetOrpFilter();               // Reset filtre (warmup) — appelé après calibration ORP

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

// feature-024 — pente sonde pH (diagnostic d'usure)
float getPhSlopeAcid()  const;          // % pente acide (NaN si jamais lu / bus dégradé)
float getPhSlopeBase()  const;          // % pente base
float getPhSlopeZero()  const;          // mV décalage zéro (NaN si firmware EZO ancien)
uint32_t getPhSlopeAgeMs() const;       // ms depuis dernière query OK (UINT32_MAX si jamais)
bool enqueuePhSlopeQuery();             // force un refresh — dédoublonné

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

## Filtrage des mesures pH/ORP — feature-025

Lissage logiciel **robuste** des mesures EZO pour réduire l'impact des fluctuations ponctuelles (bruit secteur, EMI, couplage 12 V) sur l'affichage, MQTT/HA et **surtout la régulation**. Les valeurs **brutes restent exposées** pour diagnostic ; les valeurs **filtrées deviennent la référence** pour l'UI principale, MQTT et le PID.

### Chaîne de filtrage

```text
mesure brute Atlas EZO
  → rejet aberrant (NaN / hors plage plausible / saut > maxStep)
  → médiane glissante (fenêtre 7)
  → EMA (moyenne exponentielle lente : alpha pH 0.10 / ORP 0.08)
  → valeur filtrée → UI / MQTT / PID
```

À chaque lecture EZO valide (`_readEzoSensors`), `SensorManager` soumet la brute au filtre du capteur via `SensorFilter::addSample(raw, nowMs)`. La brute est **toujours** mémorisée (`getPhRaw()`/`getOrpRaw()` = `getPh()`/`getOrp()`), même si le filtre la rejette.

### Classe `SensorFilter`

Module dédié **déterministe et testable hors matériel** ([`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp)). Une instance par capteur dans `SensorManager` (`_phFilter`, `_orpFilter`).

- **Zéro allocation dynamique** : buffer médian FIXE `float[kSensorFilterMedianWindow]` (= 7).
- **Mono-contexte** : écrit par `addSample()` (loopTask), lu par les getters. Pas de mutex interne — l'appelant respecte le contrat mono-thread, comme `_lastPh`/`_lastOrp`.
- **Pas de membre statique** : couvert par les tests unitaires (`test/`).

| Méthode | Effet |
|---------|-------|
| `bool addSample(float raw, uint32_t nowMs)` | Soumet une brute. Retourne `true` si **acceptée** (filtre alimenté), `false` si rejetée |
| `void reset()` | Réinitialise warmup, buffers et compteurs |
| `float raw() / median() / filtered()` | Dernière brute / médiane courante / valeur EMA (NaN tant que non amorcé) |
| `bool ready(uint32_t nowMs)` | `true` ssi warmup atteint **ET** dernière mesure valide récente (`< maxAgeMs`) |
| `bool unstable()` | `true` si `consecutiveRejects >= maxConsecutiveRejects` **OU** latch anti-boucle posé |
| `uint8_t rejectedCount() / consecutiveRejects()` | Compteur glissant 8 bits (sature à 255) / rejets consécutifs courants |
| `uint8_t resyncCount()` | Nb de re-sync dans la fenêtre glissante courante (anti latch-up) |
| `bool unstableLatched()` | `true` si le latch anti-boucle EMI est posé — persistant jusqu'à `reset()` |
| `uint32_t ageMs(uint32_t nowMs)` | Âge de la dernière mesure acceptée (`UINT32_MAX` si aucune) |

### Logique de rejet

Une mesure **n'alimente pas** le filtre (mais la brute reste lisible) si :

- elle vaut `NaN` (lecture stale / bus I²C dégradé via `getPh()`/`getOrp()` qui renvoient déjà NaN) ;
- elle sort de `[minValue, maxValue]` (plage plausible) ;
- `|raw − filtered| > maxStep` — **sauf pendant le warmup** (le filtre doit pouvoir converger vers une valeur initiale même éloignée).

### États du filtre

- **warmup** : tant que `< kSensorFilterWarmupSamples` (= 5) mesures valides reçues → `ready() = false`. **Aucun dosage** tant que non prêt.
- **ready** : warmup atteint ET dernière mesure valide `< kSensorFilterMaxAgeMs` (= 20 s). Au-delà (EZO injoignable) → `ready()` repasse `false` → dosage fail-closed.
- **unstable** : `>= kSensorFilterMaxConsecutiveRejects` (= 10) rejets consécutifs → capteur déclaré instable, dosage bloqué (on bloque plutôt que de lisser un défaut EMI persistant). Cet état est **transitoire** : il s'éteint dès qu'une mesure est acceptée. À ne pas confondre avec le **latch anti-boucle** ci-dessous, qui est persistant.

### Re-synchronisation (anti latch-up)

> **Bug corrigé** : un saut réel **et durable** au-delà de `maxStep` figeait la valeur filtrée à vie. Cas terrain : sonde plongée en solution de calibration (~4.871) pendant le warmup, puis retour à la vraie valeur en bassin → toutes les mesures suivantes rejetées comme « saut excessif » → `filtered` figé indéfiniment → **dosage bloqué en permanence**.

Le filtre distingue désormais un **pic isolé** d'un **vrai changement durable** :

- Chaque rejet pour « saut excessif » incrémente `consecutiveRejects` et mémorise le brut rejeté dans un mini-buffer FIXE (`_rejBuffer`, réutilise la capacité physique `kSensorFilterMedianWindow` → aucune allocation).
- Après `kSensorFilterResyncRejects` (= **24**, ≈ 120 s à 5 s/cycle) rejets consécutifs sur saut excessif, le filtre conclut à un **changement réel** : il se **ré-amorce sur la MÉDIANE des derniers bruts rejetés** (et non sur l'échantillon courant, pour ignorer un éventuel pic final) puis **repart en warmup** → `ready()` repasse `false` → **dosage bloqué pendant le re-warmup** (invariant fail-closed préservé).
- Un **pic isolé** (`< 24` cycles avant retour dans la bande `maxStep`) reste simplement rejeté, sans jamais toucher `filtered`.
- Chaque re-sync logue un `warning` (ancienne valeur filtrée + médiane d'amorçage).

`kSensorFilterResyncRejects` (= 24) est **strictement supérieur** à `kSensorFilterMaxConsecutiveRejects` (= 10) : un capteur réellement bruité est donc déclaré `unstable()` (et le dosage bloqué) **bien avant** qu'une re-sync ne se déclenche.

### Anti-boucle latché

Un capteur qui re-synchronise en boucle n'observe pas un vrai changement mais un **défaut EMI**. Garde-fou :

- Les timestamps des re-sync sont conservés dans une **fenêtre glissante** `kSensorFilterResyncWindowMs` (= **600 000 ms** = 10 min).
- Si `>= kSensorFilterMaxResyncPerWindow` (= **3**) re-sync surviennent dans cette fenêtre, le capteur est déclaré instable de façon **LATCHÉE** : `unstable()` reste `true` (via `unstableLatched()`) **jusqu'à un `reset()` explicite**, indépendamment des mesures suivantes. Le dosage reste bloqué.
- L'activation du latch logue un `critical`.

Le latch est levé **uniquement** par `reset()`, déclenché par :
- `POST /debug/sensor_filter_reset` (reset manuel des deux filtres) ;
- une **calibration pH/ORP réussie** (`resetPhFilter()` / `resetOrpFilter()`).

Getters associés : `resyncCount()` (re-sync dans la fenêtre courante) et `unstableLatched()` (état du latch).

### Paramètres (centralisés dans [`constants.h`](../../src/constants.h))

| Paramètre | pH | ORP |
|---|---:|---:|
| Fenêtre médiane (`kSensorFilterMedianWindow`) | 7 | 7 |
| EMA alpha | `kPhEmaAlpha` = 0.10 | `kOrpEmaAlpha` = 0.08 |
| Saut max rejet | `kPhFilterMaxStep` = 0.15 pH | `kOrpFilterMaxStep` = 50 mV |
| Plage plausible | `kPhFilterMin/Max` = 0.0 – 14.0 | `kOrpFilterMin/Max` = -1000 – +1500 mV |
| Warmup (`kSensorFilterWarmupSamples`) | 5 | 5 |
| Rejets consécutifs → instable (`kSensorFilterMaxConsecutiveRejects`) | 10 | 10 |
| Rejets consécutifs → re-sync (`kSensorFilterResyncRejects`) | 24 | 24 |
| Re-sync max avant latch (`kSensorFilterMaxResyncPerWindow`) | 3 | 3 |
| Fenêtre anti-boucle (`kSensorFilterResyncWindowMs`) | 600 000 ms | 600 000 ms |
| Âge max ready (`kSensorFilterMaxAgeMs`) | 20 000 ms | 20 000 ms |

### Reset après calibration

Une calibration change la fonction de transfert de la sonde → la valeur filtrée pré-calibration n'est plus représentative. Après **succès** d'une calibration via `_processEzoQueue()` :

- **pH** (`mid` / `low` / `clear`) → `resetPhFilter()` ;
- **ORP** (`cal` / `clear`) → `resetOrpFilter()`.

Le filtre repasse en **warmup** → dosage de la sonde concernée bloqué jusqu'à `kSensorFilterWarmupSamples` nouvelles mesures valides (cumulé avec le timer de stabilisation post-cal `kStabilizationDuration*Ms`, gates indépendantes).

### Reset manuel / diagnostic

`POST /debug/sensor_filter_reset` réinitialise **les deux** filtres (warmup) — c'est aussi le **seul** moyen, avec une calibration réussie, de **lever le latch anti-boucle** (`unstableLatched()`). `GET /debug/sensor_filter_state` retourne l'état JSON brut (raw/median/filtered/ready/unstable/rejected par capteur). Voir [API.md](../API.md).

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
| `kPhSlopeQueryIntervalMs` | `86_400_000` ms (24 h) | Re-query auto `Slope,?` (feature-024) |
| `kSensorFilterMedianWindow` | `7` | Fenêtre médiane (feature-025, buffer FIXE) |
| `kPhEmaAlpha` / `kOrpEmaAlpha` | `0.10` / `0.08` | Coefficient EMA (lissage lent) |
| `kPhFilterMaxStep` / `kOrpFilterMaxStep` | `0.15` pH / `50` mV | Saut max → rejet |
| `kPhFilterMin/Max` / `kOrpFilterMin/Max` | `0/14` pH / `-1000/1500` mV | Plage plausible |
| `kSensorFilterWarmupSamples` | `5` | Mesures valides avant `ready()` |
| `kSensorFilterMaxConsecutiveRejects` | `10` | Rejets consécutifs → instable (transitoire) |
| `kSensorFilterResyncRejects` | `24` | Rejets consécutifs → re-sync (≈120 s), strictement > seuil instable |
| `kSensorFilterMaxResyncPerWindow` | `3` | Re-sync max sur la fenêtre → latch instable persistant |
| `kSensorFilterResyncWindowMs` | `600000` ms (10 min) | Fenêtre glissante anti-boucle EMI |
| `kSensorFilterMaxAgeMs` | `20000` ms | Âge max dernière mesure valide pour `ready()` |

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

## Pente sonde pH — feature-024

Diagnostic **passif** d'usure de la sonde pH via la commande Atlas `Slope,?`. La feature **n'affecte pas** `canDose()` ni le PID — elle expose uniquement des valeurs brutes que l'UI évalue (chip + modal sur la page `/ph`).

### Méthode `AtlasEzoSensor::querySlope(PhSlopeInfo& out)`

Envoie `Slope,?` sous mutex I²C tenu pour toute la séquence (cmd + delay `kEzoCalDelayMs` + read + parse). Réponse Atlas attendue :

```
?Slope,99.7,100.3,-0.89
```

- **Parsing tolérant 2 ou 3 floats** : sur firmware EZO ancien, la 3ᵉ valeur (décalage zéro) peut être absente → `out.zeroOffsetMv = NaN`. Les 2 premiers floats (acide / base) sont obligatoires.
- **Trace brute en `debug` uniquement** ([feature-017](../../specs/features/done/feature-017-toggle-logs-debug.md)) — `warning` rejeté pour éviter le spam HA via le topic `{base}/logs` à chaque re-query 24 h.
- En cas d'échec parsing : `warning` une fois + `out` inchangé.

### Cache RAM dans `Sensors`

| Champ | Type | Description |
|---|---|---|
| `_phSlopeAcid` / `_phSlopeBase` | `float` | % pente Nernst (NaN si jamais lu OU bus dégradé) |
| `_phSlopeZero` | `float` | mV décalage zéro (NaN si firmware ancien ne le rapporte pas) |
| `_phSlopeQueriedMs` | `uint32_t` | `millis()` de la dernière query OK ; `0` = jamais lu |
| `_phSlopeQueryPending` | `bool` | Anti-doublon enqueue — levé par le handler dès le début du traitement |
| `_phSlopeFailStreak` | `int` | Compteur d'échecs ; ≥ `kEzoBusFailMaxConsecutive` → cache invalidé à NaN (cohérent avec `_phCalCachedPoints`) |

### Politique de refresh

1. **Au boot** : 1 query enfilée après init EZO (1ʳᵉ valeur disponible dans les ~30 s).
2. **Après chaque calibration pH réussie** (mid / low / clear) : re-query enfilée automatiquement par `_processEzoQueue()` pour rafraîchir l'info dans les ~5 s.
3. **Automatique 24 h** : `update()` enfile une re-query si `(nowAfterQueue - _phSlopeQueriedMs) >= kPhSlopeQueryIntervalMs` ET `!_phSlopeQueryPending`.
4. **À la demande** : `POST /debug/ph_slope_refresh` (cf. [API.md](../API.md)) → `enqueuePhSlopeQuery()`.

> **Garde anti-underflow `nowAfterQueue`** (commit `933f17c`, v2.1.1) : le `now` lu en début de `SensorManager::update()` est **figé** avant `_processEzoQueue()` qui peut bloquer ~900 ms sur une transaction I²C. Si le handler `QueryPhSlope` met `_phSlopeQueriedMs = millis()` à un instant postérieur, alors `now < _phSlopeQueriedMs` → soustraction `uint32_t` underflow → ~4,3 milliards → toujours ≥ 86 400 000 → ré-enqueue immédiat à chaque cycle `update()` → spam de `Slope,?` à ~1/s, monopolisation du mutex I²C, EZO ORP perturbé. Le firmware recalcule donc `nowAfterQueue = millis()` après `_processEzoQueue()` ET ajoute la garde explicite `nowAfterQueue >= _phSlopeQueriedMs` avant la soustraction.

### Dédoublonnage `_phSlopeQueryPending`

`enqueuePhSlopeQuery()` retourne `true` même si une query est déjà en file (« noop satisfait » — pas de spam de la queue 4 slots). Le flag est levé en début de handler `QueryPhSlope` pour permettre une demande suivante.

### Invalidation en mode dégradé

Si `_phSlopeFailStreak >= kEzoBusFailMaxConsecutive` (= `2`), les 3 valeurs passent à NaN. `_phSlopeQueriedMs` n'est **pas** remis à zéro : l'âge continue de croître côté UI, qui peut afficher l'état « stale » (encadré jaune si > 36 h).

### Publication

- **WebSocket** (`sensor_data`, toutes les 5 s) : 4 champs `phSlopeAcid` (1 décimale), `phSlopeBase` (1 décimale), `phSlopeZero` (2 décimales), `phSlopeAgeMs`. `null` si NaN ou jamais lu — voir [API.md](../API.md#ws-ws--write).
- **MQTT** : 3 topics retain `{base}/ph_slope_acid|base|zero` publiés **edge-triggered** (uniquement si la valeur arrondie a changé). 3 sensors auto-discovery HA — voir [MQTT.md](../MQTT.md).

> Évaluation des seuils (vert / ambré / rouge / gris) faite **côté UI** ([page-ph.md](../features/page-ph.md#chip-détat-sonde-feature-024)) — permet d'ajuster sans reflasher.

## Surveillance des valeurs aberrantes (health check)

`checkSystemHealth()` dans [`main.cpp`](../../src/main.cpp) est appelée toutes les **60 s** (`kHealthCheckIntervalMs`). Elle vérifie si chaque valeur capteur sort de sa plage de normalité :

| Capteur | Plage normale | Alerte |
|---------|--------------|--------|
| pH | [5.0 – 9.0] | warning log + MQTT `ph_abnormal` |
| ORP | [400 – 900] mV | warning log + MQTT `orp_abnormal` |
| Température eau | [5.0 – 40.0] °C | warning log + MQTT `temp_abnormal` |

Logs et alertes MQTT émis **aux transitions uniquement** (entrée et sortie de la zone anormale), pas à chaque cycle. Logs conditionnés à `authCfg.sensorLogsEnabled`.

## Interaction avec les autres composants

- [`pump_controller`](pump-controller.md) : le PID consomme **uniquement** `getPhFiltered()` / `getOrpFiltered()` (feature-025) et exige `isPhFilterReady()` / `isOrpFilterReady()` (+ `!isPhFilterUnstable()`) dans `canDose(int)`. `getPh()`/`getOrp()` restent disponibles pour les logs et l'affichage brut. Vérifie aussi `getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` (conditions #1, #2, #5).
- [`ws_manager`](ws-manager.md) : `broadcastSensorData()` publie `ph`/`orp` (= valeur **filtrée**, fallback brut si filtre non amorcé), `temperature`, `temperature_circuit`, `phCalPoints`, `orpCalPoints`, `phSlopeAcid/Base/Zero/AgeMs` toutes les 5 s. feature-025 : champs supplémentaires `ph/orpRaw|Median|Filtered`, `ph/orpFilterReady`, `ph/orpFilterUnstable`, `ph/orpRejectedCount`, `ph/orpMixingDelayActive`, `ph/orpDoseBlockedReason`.
- [`mqtt_manager`](mqtt-manager.md) : publie `{base}/ph` / `{base}/orp` (= **filtré**), `{base}/temperature`, `{base}/temperature_circuit`, `{base}/ph_cal_points`, `{base}/orp_cal_points` toutes les 10 s (`kMqttPublishIntervalMs`). 3 topics `{base}/ph_slope_*` edge-triggered. feature-025 : topics `{base}/ph|orp_raw|median|filtered|filter_ready|filter_unstable|rejected_count` + `{base}/ph|orp_mixing_delay_active` (retain). Alertes edge-triggered `{base}/alerts/calibration_required` et `{base}/alerts/sensor_stale`.
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
- [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — filtre médiane + EMA (feature-025)
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — routes refondues `/calibrate_ph`, `/calibrate_orp`, `/calibrate_clear`
- [`src/web_routes_sensor_id.cpp`](../../src/web_routes_sensor_id.cpp) — routes feature-020 inchangées
- [`src/constants.h`](../../src/constants.h) — toutes les constantes EZO + DS18B20
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001)
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0013](../adr/0013-identification-sondes-onewire.md) — identification 2 sondes DS18B20
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025)
- [feature-021](../../specs/features/done/feature-021-migration-atlas-ezo.md) — spec d'origine
- [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) — filtrage mesures + adaptation régulation
