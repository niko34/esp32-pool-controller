# Subsystem — `sensors`

- **Fichiers** : [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp), [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- **Singleton** : `extern SensorManager sensors;`
- **Responsabilité** : lecture pH (Atlas EZO 0x63), ORP (Atlas EZO 0x62), température eau + circuit (DS18B20 multi-sondes) ; calibration EZO en interne ; exposition des valeurs et de l'état de calibration aux consommateurs (PID, WS, MQTT, HTTP).

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

> **Bug historique fixé en v2.1.1** (commit `c0f2962`) : depuis feature-021, `RT,<temp>` était envoyé indistinctement aux deux modules. Sur l'ORP, la réponse vide était interprétée comme un échec → `_orpI2cFailStreak++` → bus I²C dégradé après `kEzoBusFailMaxConsecutive = 2` cycles → régulation ORP inhibée. Le commentaire originel prétendait à tort que « l'EZO ORP ignore RT et retourne la valeur ORP » — hypothèse non vérifiée empiriquement à l'époque, infirmée via `/debug/ezo_command` (endpoint de diagnostic retiré en v2.5.0, feature-045) :
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

// Lectures EZO — NaN si stale (> kSensorStaleTimeoutMs) ou jamais lu valide.
// Ne PAS utiliser pour la régulation (cf. feature-025 : le PID consomme getPhFiltered()).
float getPh()  const;                // feature-067 : pH CORRIGÉ (brut + phOffset)
float getOrp() const;                // ORP brut (aucun offset côté ORP)

// Chaîne de filtrage pH/ORP (feature-025) — médiane(13) + EMA. NaN si non amorcé.
// feature-067 : côté pH, seuls les accesseurs `*Raw` sont bruts (cf. contrat ci-dessous).
float getPhRaw()  const;             // Dernière brute pH EZO, SANS offset (NaN si stale)
float getPhMedian()  const;          // Médiane courante pH CORRIGÉE (NaN si pas de donnée)
float getPhFiltered()  const;        // pH filtré EMA CORRIGÉ — valeur PID/UI/MQTT
float getPhFilteredRaw() const;      // pH filtré SANS offset + garde d'âge — filets de sécurité
void  refreshPhOffsetCache();        // Recharge l'offset depuis mqttCfg (boot + après save)
float getPhOffset() const;           // Offset actuellement APPLIQUÉ aux mesures
bool isPhFilterReady()  const;       // true après warmup + dernière mesure valide récente
bool isPhFilterUnstable()  const;    // true si trop de rejets consécutifs
uint8_t getPhRejectedCount()  const; // Compteur glissant de rejets pH (sature à 255)
float getOrpRaw()  const;
float getOrpMedian()  const;
float getOrpFiltered()  const;
bool isOrpFilterReady()  const;
bool isOrpFilterUnstable()  const;
uint8_t getOrpRejectedCount()  const;

// v2.25.1 — POURQUOI le filtre n'est pas prêt (observabilité, cf. « Prédicats
// d'état du filtre »). État INTERNE composé par resolveFilterState() : priorité
// pannes d'abord. Consommé par canDose() pour QUALIFIER la cause de refus,
// JAMAIS pour décider du dosage. Ne jamais reconstruire depuis get*Raw().
DoseFilterState getPhFilterState() const;
DoseFilterState getOrpFilterState() const;

// v2.20.6 — passerelles de diagnostic sur la dispersion (cf. « Observabilité de
// la dispersion »). Amplitude (max−min) de la fenêtre médiane PLEINE, NaN sinon :
// c'est la grandeur qui DÉCIDE du drapeau d'instabilité. Échelle brute (une
// amplitude est insensible à l'offset pH).
float getPhDispersion()  const;
float getOrpDispersion() const;
uint8_t getPhDispersionCleanStreak()  const;  // Progression vers l'auto-levée du latch
uint8_t getOrpDispersionCleanStreak() const;
void resetPhFilter();                // Reset filtre (warmup) — appelé après calibration pH
void resetOrpFilter();               // Reset filtre (warmup) — appelé après calibration ORP

// feature-022 — détection capteur figé (variance nulle). Un capteur figé rend
// is*FilterReady() = false → dosage bloqué par la garde FilterNotReady existante.
bool isPhSensorFrozen()  const;      // 30 lectures acceptées dans une bande < kSensorFrozenEpsilonPh
bool isOrpSensorFrozen() const;      // idem, bande < kSensorFrozenEpsilonOrp
bool isTemperatureFrozen() const;    // sonde eau DS18B20 — warning-only, AUCUN impact dosage

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

// feature-057 — ring buffer de trace haute cadence (pH/ORP brut + lissé, T°)
size_t getSensorTraceCount() const;        // 0..kSensorTraceBufferSize
void appendSensorTraceJson(JsonArray out,
                           size_t offset, size_t count) const;  // sérialise une PAGE ancien → récent (null si NaN)
void clearSensorTrace();                   // vide le buffer

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
5. **Frozen check** (`_checkFrozenAndLog`, feature-022) : logs `[SENSOR_FROZEN]` edge-triggered — `critical` pH/ORP (dosage inhibé), `warning` température (aucun impact dosage), `info` à la levée. Voir [Détection capteur figé](#détection-capteur-figé--feature-022).

## Cache calibration EZO (`_phCalCachedPoints` / `_orpCalCachedPoints`)

Les chemins chauds (PID 100 Hz, broadcast WS 5 s, MQTT 10 s) ne peuvent pas tolérer une lecture I²C bloquante de 900 ms. Le firmware maintient un cache :

- **Initialisation** : `begin()` interroge `Cal,?` une fois par capteur. Si succès → `0..3`. Si EZO injoignable → `-1`.
- **Mise à jour** : à chaque calibration ou clear EZO réussie via `_processEzoQueue()`, le cache est rafraîchi par un nouveau `Cal,?`.
- **Invalidation en mode dégradé** (correctif Pass 3.5) : si `_phI2cFailStreak >= kEzoBusFailMaxConsecutive = 2`, alors `_lastPh = NaN` ET `_phCalCachedPoints = -1`. Idem ORP. Évite que `canDose()` autorise un dosage avec un cache `cal_points = 2` figé alors que le bus est tombé.
- **Rafraîchissement opportuniste** : à la 1ʳᵉ lecture EZO réussie suivante, `_phI2cFailStreak = 0` et un `Cal,?` est ré-émis.

`getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` retournent **toujours** ces caches (sans toucher au bus). `getPhCalibrationPoints()` / `getOrpCalibrationPoints()` (sans suffixe) déclenchent un appel I²C live et sont réservés aux routes HTTP de diagnostic + `mqttTask`.

## Queue FreeRTOS `_ezoQueue`

- **Capacité** : `kEzoQueueLen = 4` slots (`QueueHandle_t`).
- **Producteurs** : routes HTTP `/calibrate_*`, futurs handlers (boot wizard). *(Les commandes UART de l'écran externe, autre producteur historique, ont disparu en v2.17.0 — feature-065.)*
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

## Filtrage des mesures pH/ORP — feature-025 → feature-058

Lissage logiciel **robuste** des mesures EZO pour livrer une valeur représentative de la **tendance centrale** du signal, entrée de l'affichage, MQTT/HA et **surtout de la régulation** (PID). Les valeurs **brutes restent exposées** pour diagnostic ; les valeurs **filtrées sont la référence** pour l'UI principale, MQTT et le PID.

> **Refonte feature-058 ([ADR-0027](../adr/0027-filtre-tendance-centrale.md))** — La chaîne d'origine (rejet de saut `maxStep` → médiane(7) → EMA + re-sync anti latch-up) **collait** au haut ou au bas d'une oscillation soutenue (le rejet de saut traitait les transitions comme aberrantes → n'intégrait qu'un côté de l'enveloppe), et pouvait **figer** la valeur sur un nombre trompeur via le latch. Elle est remplacée par un **estimateur de tendance centrale** dont la valeur est **découplée** du drapeau d'instabilité. La cause racine du bruit reste **matérielle** ; ce filtre livre en attendant une valeur **honnête et centrée**, plus sûre qu'une valeur collée ou gelée.

### Chaîne de filtrage

```text
mesure brute Atlas EZO
  → rejet des VRAIES aberrations uniquement :
      NaN  /  hors plage plausible  /  spike DUR isolé (|raw − médiane| > spikeReject)
  → médiane sur fenêtre LONGUE (13 échantillons ≈ 65 s, > 1 période d'oscillation)
  → EMA (moyenne exponentielle lente : alpha pH 0.10 / ORP 0.08)
  → valeur filtrée (tendance centrale, JAMAIS figée) → UI / MQTT / PID
```

L'**oscillation légitime soutenue n'est plus rejetée** : elle alimente le filtre, la médiane longue la moyenne sur ≥ 1 période → valeur **centrée**. Seule la vraie aberration (spike dur isolé, NaN, hors-plage) est écartée.

À chaque lecture EZO valide (`_readEzoSensors`), `SensorManager` soumet la brute au filtre du capteur via `SensorFilter::addSample(raw, nowMs)`. La brute est **toujours** mémorisée (`getPhRaw()` / `getOrpRaw()`), même si le filtre la rejette.

> **feature-067** : le filtre pH est alimenté par le signal **brut** ; l'offset logiciel `ph_offset` est ajouté **en sortie** des accesseurs. `getOrpRaw()` reste un alias de `getOrp()`, mais **`getPhRaw()` n'est plus un alias de `getPh()`** — voir [Offset logiciel de mesure pH](#offset-logiciel-de-mesure-ph--feature-067).

### Contrat `filtered()` — jamais figée

`filtered()` renvoie **toujours** l'EMA de la médiane-longue **recalculée en continu** : c'est l'estimateur central **courant**. Elle n'est **jamais figée**, **jamais ré-amorcée** en cours de route par une instabilité. Elle vaut `NaN` **uniquement** tant que la fenêtre centrale n'est pas **pleine** (warmup, C2). Invariant iso-sécurité : `filtered() ∈ [min, max]` de la fenêtre observée, **jamais** tirée vers la consigne.

Le **drapeau** `unstable()` (inhibition de la régulation) est **découplé** de la valeur — il ne modifie jamais `filtered()`.

### Classe `SensorFilter`

Module dédié **déterministe et testable hors matériel** ([`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp)). Une instance par capteur dans `SensorManager` (`_phFilter`, `_orpFilter`).

- **Zéro allocation dynamique** : buffer médian FIXE `float[kSensorFilterCentralWindowMax]` (capacité physique 16, fenêtre logique `kSensorFilterCentralWindow` = 13).
- **Mono-contexte** : écrit par `addSample()` (loopTask), lu par les getters. Pas de mutex interne — l'appelant respecte le contrat mono-thread, comme `_lastPh`/`_lastOrp`.
- **Pas de membre statique** : module **100 % pur**, couvert par les tests natifs (`test/test_native_sensor_filter/`).

| Méthode | Effet |
|---------|-------|
| `bool addSample(float raw, uint32_t nowMs)` | Soumet une brute. Retourne `true` si **acceptée** (filtre alimenté), `false` si rejetée (NaN / hors-plage / spike dur isolé) |
| `void reset()` | Réinitialise warmup, buffers, compteurs **et le latch EMI** |
| `float raw() / median() / filtered()` | Dernière brute / médiane courante / EMA (estimateur central courant, NaN tant que la fenêtre n'est pas pleine) |
| `float dispersion()` | Amplitude (`max − min`) de la fenêtre brute **pleine** (NaN sinon). Diagnostic + moteur du drapeau d'instabilité |
| `bool ready(uint32_t nowMs)` | `true` ssi fenêtre centrale **pleine** **ET** dernière mesure valide récente (`< maxAgeMs`) **ET** non figé (feature-022) |
| `bool unstable()` | `true` si **dispersion > cap** (fenêtre pleine) **OU** **latch EMI** posé **OU** `consecutiveRejects >= maxConsecutiveRejects` |
| `bool frozen()` | `true` si `frozenSamples` échantillons acceptés consécutifs tiennent dans une bande < `frozenEpsilon` (feature-022, via `FrozenDetector` composé) |
| `uint8_t rejectedCount() / consecutiveRejects()` | Compteur glissant 8 bits (sature à 255) / rejets NaN/hors-plage consécutifs courants |
| `bool unstableLatched()` | `true` si le latch EMI (dispersion soutenue) est posé — auto-levé après `kSensorFilterLatchClearCycles` fenêtres pleines propres consécutives, ou immédiatement par `reset()` (v2.8.4) |
| `uint8_t dispersionCleanStreak()` | Fenêtres pleines **propres** consécutives (disp ≤ cap ET zéro rejet). Compteur qui pilote l'auto-levée du latch EMI ; remis à 0 par tout rejet ou tout dépassement de dispersion (diagnostic, v2.8.4) |
| `uint32_t ageMs(uint32_t nowMs)` | Âge de la dernière mesure acceptée (`UINT32_MAX` si aucune) |
| `void setContext(bool waterPresent, bool measuresValid, uint32_t nowMs)` | **v2.20.3** — Porte d'alimentation : ferme le filtre aux mesures non représentatives (voir [Porte d'alimentation](#porte-dalimentation-du-filtre-v2203)). **v2.25.1** : **désarme en tête** une grâce de ré-amorçage expirée — appelé toutes les 5 s **inconditionnellement**, y compris bus I²C mort |
| `void rearm()` | **v2.20.3** — Ré-amorce l'**estimateur seul** (buffer, EMA, clean streak) — **ne touche NI** le `FrozenDetector` **NI** le latch EMI (voir [rearm() distincte de reset()](#rearm-distincte-de-reset-v2203)). **v2.25.1** : arme aussi une **grâce d'observabilité** sur `stale()` |
| `bool gateClosed()` | **v2.20.3** — `true` si la porte est fermée : `addSample()` sort en tête, aucune alimentation |
| `bool warmingUp()` | **v2.25.1** — Fenêtre centrale **incomplète** (`_bufCount < centralWindow`). Observabilité uniquement |
| `bool stale(uint32_t nowMs)` | **v2.25.1** — Aucune mesure acceptée, **ou** âge > `maxAgeMs` — **sauf** pendant la grâce de ré-amorçage (intervalle fermé depuis l'armement, wrap-safe). Observabilité uniquement, **même frontière** que la garde d'âge de `ready()` |
| `bool rearmedSinceLastPoll()` | **v2.20.3** — Drapeau consommé (edge-triggered) : un ré-amorçage a eu lieu depuis le dernier appel. Sert à la journalisation par la coquille (module pur : pas de logger, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) |
| `uint8_t rearmDenied()` | **v2.20.3** — Motif du **dernier refus** de ré-amorçage, **collant** : `0` aucun, `1` cooldown non écoulé, `2` valeur hors bande admissible. Un refus ⇒ filtre non prêt ⇒ dosage fail-closed ⇒ la coquille **doit** alarmer |
| `uint8_t rejectedBeforeRearm()` | **v2.20.3** — Valeur de `rejectedCount()` juste **avant** le dernier `rearm()`. Le ré-amorçage remet le compteur à 0 (sinon `/debug/sensor_filter_state` continuait d'afficher `rejected=255` sur un filtre réparé) : c'est cette valeur qu'il faut journaliser pour tracer l'ampleur du verrouillage levé |

### Logique de rejet — vraies aberrations uniquement

Une mesure **n'alimente pas** le filtre (mais la brute reste lisible) si :

- elle vaut `NaN` (lecture stale / bus I²C dégradé via `getPh()`/`getOrp()` qui renvoient déjà NaN) → incrémente `consecutiveRejects` ;
- elle sort de `[minValue, maxValue]` (plage plausible) → incrémente `consecutiveRejects` ;
- **spike DUR isolé** : `|raw − médiane| > spikeReject` (`kPhFilterSpikeReject` = 1.0 pH / `kOrpFilterSpikeReject` = 300 mV) → l'échantillon est **écarté**, **exclu** du calcul de dispersion, **ne compte PAS** comme instabilité (n'incrémente pas `consecutiveRejects`) et **ne met pas à jour** `lastValid` → l'âge grandit. Un spike **répété** maintient donc le filtre non alimenté → âge > maxAge → non prêt → dosage fail-closed (invariant feature-022 préservé).

Le seuil `spikeReject` est **très au-dessus** de l'amplitude d'oscillation légitime : il ne rejette que l'aberration franche, jamais une transition d'oscillation soutenue (qui, elle, doit alimenter la médiane).

### Le piège du spike répété — incident du 2026-08-16 (v2.20.3)

Jusqu'en v2.20.2, le comportement décrit ci-dessus (« un spike **répété** rend le filtre non prêt ») était documenté comme un **fail-closed volontaire**, pensé pour une sonde défaillante. C'était en réalité un **piège sans issue**, et il s'est refermé en production :

```text
/debug/sensor_filter_state (firmware 2.20.0)
ph : raw=6.957  median=5.293(corrigé)  filtered=5.282  ready=false  rejected=255
orp: raw=556    median=561             filtered=568    ready=true   rejected=0
```

Médiane **interne** = 5,293 − 0,50 (offset) = **4,793**, contre un brut à **6,957** : 2,16 pH d'écart. Le raisonnement du piège est déterministe :

1. la médiane ne peut évoluer **que** si des échantillons entrent dans le buffer ;
2. un échantillon à plus de `spikeReject` de la médiane est écarté **avant** d'y entrer ;
3. donc dès que la grandeur **vraie** s'éloigne durablement de la médiane courante, **chaque** échantillon suivant est rejeté à son tour → médiane figée **pour toujours**.

Les deux échappatoires existantes étaient inopérantes par conception : `consecutiveRejects` n'est **volontairement pas** incrémenté sur ce chemin (donc `maxConsecutiveRejects` ne se déclenche jamais), et `dispersionCleanStreak` est remis à 0 par tout spike écarté (donc l'auto-levée du latch EMI ne se déclenche jamais non plus). `rejectedCount` sature à 255 sans rien piloter. **Seul `reset()` en sortait.**

Conséquence : `ready() == false` en permanence ⇒ dosage pH bloqué (garde `FilterNotReady`) **et** dosage chlore inhibé (fail-closed feature-067 sur pH brut indisponible). Le bassin est resté **sans aucune régulation ≥ 3 h**, sans alarme, la seule porte de sortie étant une route de debug absente de l'interface. Le mécanisme n'avait jamais été pensé pour un **échelon légitime** de la grandeur mesurée.

Le **déclencheur** est distinct du mécanisme, et c'est lui qui rendait le défaut récurrent : le filtre ingérait les mesures de la **stagnation** (eau immobile dans la chambre de mesure, au voisinage du point d'injection d'acide), que `measure_validity` déclarait pourtant **non représentatives** — puis leur donnait autorité sur les mesures représentatives suivantes, qu'il rejetait une à une.

> **En une phrase** : le filtre ingérait des échantillons que le système lui-même avait déclarés non représentatifs, puis leur donnait autorité sur les échantillons représentatifs suivants.

Trois correctifs complémentaires en v2.20.3 (tous dans le module **pur**, donc couverts par les tests natifs) : la **porte d'alimentation**, la **sortie de secours**, et l'**alarme**. Décision et alternatives écartées : [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md).

### Porte d'alimentation du filtre (v2.20.3)

`setContext(waterPresent, measuresValid, nowMs)` est appelée à **chaque** cycle de lecture, **avant** `addSample()`, avec le verdict de représentativité de [`measure_validity`](measure-validity.md) (lu dans son **cache** sous spinlock, jamais via `filtration` directement).

| Condition | Porte |
|---|---|
| `!waterPresent` (circulation arrêtée / signal externe périmé) | **fermée** |
| `!measuresValid` (purge de la chambre de mesure en cours) | **fermée** |
| Front montant d'eau, pendant `kSensorFilterGateFloorMs` = **60 s** | **fermée** (plancher de gel) |
| sinon | ouverte |

- **Porte fermée** ⇒ `addSample()` **sort en tête** : l'échantillon est mémorisé pour le diagnostic (`raw()`) mais ne touche **ni** le buffer, **ni** le `FrozenDetector`, **ni** aucun compteur — il n'existe pas pour le filtre. L'âge de la dernière mesure valide continue de grandir ⇒ `ready() = false` ⇒ dosage fail-closed pendant toute la purge, ce qui est le comportement voulu.
- **Plancher de gel de 60 s** : appliqué **même si** la coquille déclare déjà les mesures représentatives — il couvre le cas d'une porte de validité qui s'ouvrirait sans délai (dispense après coupure courte). Comparaison wrap-safe sur `(int32_t)(nowMs - _gateFloorUntilMs)`. Ce plancher est **indépendant** du plancher de purge `kPurgeFloorMs` (10 min) de `measure_validity`, qui joue en amont.
- **Transition fermée → ouverte** ⇒ `rearm()` : la médiane construite avant/pendant la stagnation n'a **aucune** autorité sur les mesures représentatives qui suivent. Le filtre repart en warmup complet (13 échantillons ≈ 65 s) — disponibilité sacrifiée au profit de la justesse, arbitrage `pool-chemistry`.
- **Rétrocompatibilité** : `_gateClosed` vaut `false` par défaut. Un appelant qui n'utilise pas `setContext()` (tests natifs existants) conserve strictement le comportement antérieur.

Journalisation **edge-triggered** par `SensorManager::_updateFilterGates()` (les deux filtres partageant le même contexte, une seule paire de lignes) :

- fermeture — `info` : `Filtres pH/ORP : alimentation GELÉE — mesures non représentatives (circulation arrêtée | stabilisation en cours) — dosage auto bloqué tant que la porte est fermée`
- réouverture — `info` : `Filtres pH/ORP : alimentation REPRISE — mesures représentatives, filtres ré-amorcés (warmup en cours, la médiane de stagnation est abandonnée)`

Le drapeau `rearmedSinceLastPoll()` est **drainé** juste après, pour que le ré-amorçage de porte (légitime et déjà journalisé) ne soit pas attribué à tort à la sortie de secours.

#### `ready()` est faux tant que la porte est fermée (C6, v2.34.0)

Depuis la v2.34.0 (feature-091, condition **C6** de `pool-chemistry`), `SensorFilter::ready(nowMs)` retourne **faux immédiatement** quand la porte est fermée — un cinquième prédicat s'ajoute aux quatre existants (fenêtre pleine, mesure présente, âge ≤ `maxAgeMs`, non figé), à la toute fin de [`sensor_filter.cpp`](../../src/sensor_filter.cpp) (`if (_gateClosed) return false;`).

**Le trou qu'il ferme** : jusque-là, `ready()` ne devenait faux qu'*indirectement*, par vieillissement du dernier échantillon accepté (`maxAgeMs` = 20 s). À la **première** ouverture ce n'était pas visible (le ré-amorçage vide déjà la fenêtre), mais lors d'une **re-fermeture** (re-purge C1, mesure redevenue instable après ouverture) le filtre restait « prêt » pendant jusqu'à 20 s **avec la dernière valeur filtrée du rinçage**. Pendant cette fenêtre, le terme proportionnel, le **terme intégral**, le **feedforward** et les gardes d'alerte pH/ORP pouvaient consommer une mesure que le système venait de déclarer non représentative.

- **Aucun terme de régulation ni aucune alerte pH/ORP ne consomme les mesures de rinçage** : toutes ces consommations passent par `ready()`.
- **Sans effet** sur un appelant qui n'utilise pas `setContext()` (tests natifs antérieurs : `_gateClosed` reste `false`), ni à la première ouverture d'un cycle d'eau.
- Le prédicat d'observabilité `gateClosed()` (v2.25.1) est **inchangé** : il continue de qualifier le refus en « attente normale », jamais en panne de sonde.

Décision d'origine : [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) ; purge adaptative qui rend la re-fermeture possible : [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md) et [measure-validity.md](measure-validity.md).

### Sortie de secours sur rejets cohérents (v2.20.3)

Le critère qui distingue un capteur qui crache des aberrations **isolées** (à filtrer) d'une grandeur qui a **franchement changé** (à suivre) est la **cohérence mutuelle des rejets** : `kSensorFilterSpikeRearmCount` = **12** rejets de spike consécutifs contenus dans une bande `max − min ≤ rearmCoherence` (**0,20 pH** / **40 mV**) décrivent une **nouvelle valeur**, pas du bruit. Une série dispersée **ré-ancre** immédiatement le compteur sur le dernier brut : du bruit n'atteindra jamais 12.

Au 12ᵉ rejet cohérent, **trois gardes cumulatives** (verdict `pool-chemistry`) décident :

| Garde | Valeur | Effet si refus |
|---|---|---|
| **Cooldown** par capteur depuis le dernier ré-amorçage | `kSensorFilterRearmCooldownMs` = **30 min** | `rearmDenied() = 1` |
| **Bande admissible**, jugée sur le **centre** de la série (`(min+max)/2`) | pH **[6,50 ; 8,20]**, ORP **[300 ; 950]** | `rearmDenied() = 2` |
| **Cohérence** de la série | `max − min ≤ rearmCoherence` | série ré-ancrée |

- La bande est jugée sur le **centre** de la série et non sur le dernier brut reçu (revue `pool-chemistry`) : le centre représente la valeur vers laquelle on s'apprête à converger, et la fuite hors bande est ainsi divisée par deux à disponibilité identique.
- ⚠️ **Les bornes pH sont exprimées sur l'échelle BRUTE** (le filtre travaille sur le brut, [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) : la bande **effective en pH corrigé** est donc décalée de `ph_offset`. Avec un offset de +0,40, elle vaut `[6,90 ; 8,60]` en valeur affichée.
- Les 12 échantillons rejetés sont **JETÉS**, jamais réinjectés dans le buffer : aucune valeur aberrante n'entre par cette porte. Le filtre re-converge **uniquement** sur les mesures à venir, après un **warmup complet** (13 échantillons ≈ 65 s), dosage bloqué pendant toute la re-convergence — fail-closed conservé.
- Un refus rend `rearmDenied()` **collant** : le filtre **reste** verrouillé, donc sans régulation. La coquille journalise alors un `critical` et publie une alerte MQTT (`ph_filter_rearm_denied` / `orp_filter_rearm_denied`), **une seule fois par épisode**. Une sonde réellement HS ne ré-amorce donc **jamais** en silence.
- `rearmCoherence == 0` (init agrégat courte, cas des tests) ⇒ sortie de secours **désactivée**, comportement strictement antérieur.

Journalisation (`SensorManager::_checkRearmAndLog()`) :

- succès — `warning` : `Filtre pH : RÉ-AMORÇAGE après 12 rejets cohérents autour de 6.957 — la grandeur mesurée a franchement changé, la médiane figée est abandonnée (warmup en cours, dosage auto bloqué)`
- refus — `critical` + alerte MQTT : `Filtre pH : ré-amorçage REFUSÉ (délai anti-répétition de 30 min non écoulé | valeur hors bande admissible) sur brut 4.793 — le filtre reste NON PRÊT, régulation pH à l'arrêt : vérifier la sonde`

### Tolérance aux trous de lecture (v2.20.3)

Condition **bloquante** du gate `pool-chemistry`, ajoutée en revue : un **trou** (`NaN` ou hors-plage) survenant pendant une série de rejets cohérents **n'annule plus** la série. Sans elle, une sonde ratant **un cycle sur douze** pendant un échelon légitime n'aurait **jamais** ré-amorcé — c'est-à-dire que l'incident d'origine se serait reproduit à l'identique.

Règle chiffrée (`_noteSpikeSeriesGap()`) :

- au plus **2 trous** par série ;
- **jamais 2 consécutifs** ;
- un trou **ne fait pas avancer** `_spikeCount` : l'évidence doit rester acquise par des rejets **réels** ;
- 2 trous consécutifs ou 3ᵉ trou ⇒ série remise à zéro (fail-closed).

Un rejet réel remet `_lastWasGap` à `false` : la continuité de l'évidence reprend.

### rearm() distincte de reset() (v2.20.3)

| | `rearm()` | `reset()` |
|---|---|---|
| Buffer médian, EMA, `dispersionCleanStreak` | vidés | vidés |
| `rejectedCount` | remis à 0 (valeur d'avant lisible via `rejectedBeforeRearm()`) | remis à 0 |
| Bookkeeping sortie de secours (`_spike*`, `rearmDenied`) | remis à zéro | remis à zéro |
| **`FrozenDetector`** (feature-022) | **intact** | réinitialisé |
| **Latch EMI** (feature-058) | **intact** | levé |
| Porte d'alimentation | inchangée | remise à l'état ouvert (recalculée au prochain `setContext()`) |
| **Grâce « périmé »** (v2.25.1) | **armée** (`maxAgeMs` au plus, datée de `_lastSeenMs`) | **jamais armée**, et une grâce en cours est **désarmée** |

**Pourquoi cette distinction existe** : re-converger ne doit **jamais blanchir un défaut de mesure**. Une sonde figée le reste jusqu'à fournir un échantillon hors bande ; une ambiance EMI soutenue continue d'inhiber le dosage. C'est la garantie explicite qu'on ne masque pas une sonde défaillante en réparant le filtre — d'autant que **l'origine de l'excursion à ~4,8 n'est toujours pas tranchée** (stagnation acide au point d'injection **ou** défaillance de sonde ; cf. l'instrumentation de [`measure_validity`](measure-validity.md#instrumentation-des-bruts-au-front-montant-deau-v2203)).

Conséquence voulue : après `rearm()`, `_bufCount = 0` ⇒ `ready() = false` pendant **tout** le warmup ⇒ dosage fail-closed pendant la reprise.

### États du filtre

- **warmup** : tant que la fenêtre centrale n'est pas **pleine** (`< kSensorFilterCentralWindow` = 13 échantillons acceptés) → `filtered() = NaN`, `ready() = false`. **Aucun dosage** tant que non prêt.
- **ready** : fenêtre pleine ET dernière mesure valide `< kSensorFilterMaxAgeMs` (= 20 s) ET capteur non figé ET **porte d'alimentation ouverte** (C6, v2.34.0). Au-delà (EZO injoignable) → `ready()` repasse `false` → dosage fail-closed.
- **unstable** (drapeau d'inhibition, **découplé** de la valeur — ne fige jamais `filtered()`) : `true` si l'un de ces trois signaux est posé :
  - **(a) dispersion > cap** : l'amplitude `max − min` de la fenêtre brute **pleine** dépasse `kPhFilterMaxDispersion` (= 0.5 pH) / `kOrpFilterMaxDispersion` (= 100 mV) → oscillation de niveau EMI. Les caps sont maintenus **sous** l'amplitude EMI observée (feature-057) pour préserver l'inhibition existante — **ne pas assouplir** ;
  - **(b) latch EMI** : dispersion > cap **soutenue** (`kSensorFilterDispersionLatchCycles` = 3 fenêtres pleines consécutives en dépassement) → latch collant `unstableLatched()`. Auto-levé après `kSensorFilterLatchClearCycles` = 12 fenêtres pleines **propres** consécutives (≈ 2 min de signal réellement propre, v2.8.4) ou immédiatement par `reset()`/recalibration ;
  - **(c) rejets consécutifs** : `>= kSensorFilterMaxConsecutiveRejects` (= 10) rejets NaN/hors-plage consécutifs. État transitoire, s'éteint dès qu'une mesure est acceptée.
- **frozen** (feature-022) : `kSensorFrozenSamples` (= 30) lectures acceptées consécutives dans une bande < epsilon → `ready()` repasse `false` → dosage fail-closed via la garde #4b existante (cause exposée `FilterFrozen` depuis la v2.25.1, `FilterNotReady` avant). Sortie **immédiate sans re-warmup** dès une lecture vivante. Voir [Détection capteur figé](#détection-capteur-figé--feature-022).

### Prédicats d'état du filtre (observabilité, v2.25.1)

`ready()` est faux pour **quatre** raisons physiquement distinctes, dont deux sont des états d'attente normaux et deux des **pannes**. (Depuis la v2.34.0, la porte fermée est en plus une condition **directe** de `ready()` — voir [`ready()` est faux tant que la porte est fermée](#ready-est-faux-tant-que-la-porte-est-fermée-c6-v2340) ; les quatre prédicats d'observabilité ci-dessous sont inchangés.) Jusqu'à la v2.25.1, la chaîne de refus de dosage les confondait dans un seul libellé (« warmup / EZO injoignable ») : une vraie panne d'EZO s'affichait « Stabilisation du filtre ». `SensorFilter` expose donc quatre prédicats `const`, **lecture seule, sans effet sur la décision** :

| Prédicat | Vrai quand | Registre |
|---|---|---|
| `warmingUp()` | `_bufCount < centralWindow` (fenêtre de lissage incomplète) | attente normale |
| `gateClosed()` | porte d'alimentation fermée : mesures déclarées **non représentatives** ([ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)) | attente normale |
| `stale(nowMs)` | aucune mesure acceptée, **ou** âge > `kSensorFilterMaxAgeMs` (20 s) — hors grâce de ré-amorçage | **panne** |
| `frozen()` | capteur figé (`FrozenDetector`, feature-022) | **panne** |

Composition par la fonction pure `resolveFilterState()` ([`dosing_logic.cpp`](../../src/dosing_logic.cpp)), **priorité pannes d'abord** : `frozen → gateClosed → stale → warmup → ready`. Exposition côté `SensorManager` :

```cpp
DoseFilterState getPhFilterState() const;   // lock-free, même contrat que isPhFilterReady()
DoseFilterState getOrpFilterState() const;
```

> ⚠️ **Ne JAMAIS reconstruire ce diagnostic depuis `getPhRaw()` / `getOrpRaw()`.** Le cache brut est alimenté par **toute** lecture, y compris hors plage et y compris une valeur constante : il reste donc **frais** sur une sonde figée ou sur un EZO débitant du garbage. Un discriminant fondé sur sa fraîcheur — proposé lors du cadrage, **refusé par le gate `pool-chemistry`** — aurait classé une panne en « warmup ». C'est exactement le défaut que la v2.25.1 corrige. Voir [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md).

`stale()` partage **exactement** la frontière de la garde d'âge de `ready()` (`age > maxAgeMs`, `age == maxAgeMs` n'est pas périmé) : sinon la cause affichée et le verdict de dosage divergeraient sur un intervalle d'une lecture.

#### Grâce de ré-amorçage sur `stale()`

`rearm()` vide l'estimateur mais **ne fabrique pas de mesure** : `_hasValid` et `_lastValidMs` restent ceux d'avant, invariant qui rend `ready()` et `ageMs()` honnêtes. Conséquence non voulue de la priorité pannes-d'abord : juste après une réouverture de porte (démarrage de filtration, fin de purge), le filtre était simultanément `warmingUp` **et** `stale` → l'interface affichait une puce rouge **« Sonde muette »** sur une sonde parfaitement saine.

Correctif : `rearm()` **mémorise que l'âge courant est imputable au ré-amorçage** — la datation n'est pas falsifiée. Règles :

- **armement** : `rearm()` uniquement — les **deux** chemins réels de ré-amorçage (réouverture de porte dans `setContext()`, sortie de secours sur rejets cohérents dans `addSample()`). L'instant d'armement est `_lastSeenMs`, la dernière horloge vue par ces deux chemins, affectée en tête : il vaut donc exactement le `nowMs` du cycle en cours ;
- **durée** : `kSensorFilterMaxAgeMs` (20 s) au plus, levée par la **première mesure acceptée** ;
- **`stale()` seul lit ce drapeau.** `ready()`, `unstable()`, `ageMs()` et tout le chemin du verdict de dosage sont **inchangés au bit près** — la grâce est un mécanisme d'affichage, pas une tolérance de sécurité ;
- `reset()` **n'arme rien** et **désarme** une grâce en cours : une sonde muette au **démarrage à froid** est annoncée **immédiatement**, sans le moindre délai de courtoisie. La grâce ne couvre que le cas où l'âge affiché est *imputable au ré-amorçage*, ce qui n'a aucun sens sur un filtre qui n'a jamais rien mesuré ;
- retard résiduel assumé : une sonde réellement muette au moment précis d'un ré-amorçage est annoncée jusqu'à 20 s plus tard. Le retard est intrinsèque — on ne peut pas conclure avant d'avoir laissé sa chance à la première lecture qui suit.

##### Deux protections cumulatives contre le repliement de `millis()`

La **première écriture** de cette grâce portait le **troisième défaut de repliement du projet** (après la retentative de `Slope,?` en v2.21.2 et la comptabilité d'impulsions `tickPulse`). L'échéance n'était évaluée que dans `stale()`, qui est `const` : sur une sonde restée muette, le drapeau ne redescendait **jamais**, et la comparaison de deadline `(int32_t)(now − until) < 0` — juste au premier tour — **se retournait 2³¹ ms plus tard**. La panne était **masquée à nouveau pendant 24,8 jours**, l'appareil réaffichant « en stabilisation » sur une sonde morte. Atteignable : `millis()` se replie à 49,7 jours et l'appareil tourne en continu.

Le correctif est **redondant par conception** :

| # | Protection | Où | Ce qu'elle garantit seule |
|---|---|---|---|
| 1 | **Forme du prédicat** : intervalle fermé `[from, from + maxAgeMs[`, testé en `(nowMs − _staleGraceFromMs) < maxAgeMs` **non signé** — jamais de date de fin, jamais de comparaison signée à une échéance | `stale()` ([`sensor_filter.cpp`](../../src/sensor_filter.cpp)) | le prédicat est monotone en **durée écoulée**, pas en `now` : même un drapeau resté armé ne peut re-rentrer dans la grâce qu'au repliement **complet** de `millis()` (49,7 j, limite intrinsèque du projet) au lieu de 24,8 j |
| 2 | **Désarmement effectif** : `_expireStaleGrace(nowMs)` remet `_staleGraceArmed` à `false`, appelée en **tête** de `setContext()` **et** de `addSample()` — les deux seuls chemins non-`const` recevant l'horloge | `sensor_filter.cpp` | `setContext()` est atteint **toutes les 5 s inconditionnellement** depuis `SensorManager::_updateFilterGates()`, **y compris bus I²C mort** (il ne lit aucun capteur) : la grâce expire donc pour de bon, même quand plus aucune mesure n'arrive |

> ⚠️ **Les deux protections sont testées séparément** (`test_V2251_grace_expiree_ne_se_rearme_pas_apres_2exp31_ms` pour la forme du prédicat, `test_V2251_grace_desarmee_par_setContext_meme_sans_lecture` et `test_V2251_grace_desarmee_par_addSample_sonde_muette` pour le désarmement, `test_V2251_grace_traverse_le_repliement_de_millis` pour le franchissement). Une mutation supprimant l'une **laisse survivre** les tests de l'autre : c'est délibéré, personne ne doit pouvoir en retirer une au motif que l'autre suffit.

La règle générale qui s'en dégage — **mémoriser l'instant d'armement et tester la durée écoulée, jamais une date de fin** — est consignée dans [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md#pourquoi-la-forme-du-prédicat-compte-correctif-de-repliement-revue-v2251) et vaut pour tout code manipulant `millis()`.

Une panne persistante est en outre remontée sur MQTT (`ph/orp_dose_sensor_fault`, 60 s de panne continue) — voir [pump-controller.md](pump-controller.md#alerte-de-panne-de-mesure-v2251).

### Latch EMI (persistance de l'inhibition)

Un bruit de niveau EMI **soutenu** (dispersion > cap sur `kSensorFilterDispersionLatchCycles` = 3 fenêtres pleines consécutives) pose un latch **collant** :

- `unstable()` reste `true` (via `unstableLatched()`) **indépendamment des mesures suivantes** → dosage bloqué.
- Le latch distingue un **pic de dispersion ponctuel** (inhibition transitoire, s'éteint dès que la fenêtre repasse sous le cap) d'un **défaut EMI persistant** (latch collant).
- **La valeur `filtered()` reste publiée et recalculée** pendant tout l'état latché : le latch inhibe le **dosage**, il ne fige **pas** la valeur (contrat feature-058).

#### Auto-levée après signal propre soutenu (v2.8.4)

Le latch n'est **plus purement collant** : il s'auto-lève après un retour au calme durable, sans intervention. Asymétrie **voulue** — rapide à poser (3 fenêtres ≈ 15 s, défaut EMI), lent à lever (~2 min de signal réellement propre, anti-flapping) :

- Compteur `dispersionCleanStreak()` : nombre de fenêtres pleines **propres** consécutives. Une fenêtre est « propre » ssi sa dispersion `max − min` reste **sous le cap** ET qu'aucun échantillon n'a été rejeté sur cette fenêtre.
- **Incrément uniquement sur fenêtre PLEINE et propre.** Un rejet (NaN / hors-plage / spike dur isolé) fait `return` avant l'évaluation de dispersion en **remettant la streak à 0** ; tout dépassement de dispersion la remet aussi à 0. → la streak ne compte **que** des fenêtres sans aucune perturbation. Un capteur qui crache des spikes ne peut donc **jamais** lever le latch à tort.
- Auto-levée dès `dispersionCleanStreak() >= kSensorFilterLatchClearCycles` (= 12). 12 fenêtres ≈ 60 s auxquelles s'ajoute la purge de la fenêtre médiane (~13 échantillons ≈ 65 s) → **≈ 2 min de signal réellement propre** avant reprise du dosage.

`reset()` reste la **levée manuelle immédiate** (sans attendre la streak), déclenché par :
- `POST /reset-sensor-filter?sensor=ph|orp` (réarmement manuel depuis l'interface, v2.20.3) ou `POST /debug/sensor_filter_reset[?sensor=…]` (**un** filtre si le paramètre est fourni, les deux sinon) ;
- une **calibration pH/ORP réussie** (`resetPhFilter()` / `resetOrpFilter()`).

> Le reset filtre + warmup post-calibration est **mode-indépendant** : il est déclenché par le **succès de la commande EZO** dans `_processEzoQueue()`, sans aucune condition sur le mode de régulation (`automatic` / `scheduled` / `manual`). L'élargissement de l'accès à la calibration dans tous les modes (feature-034, purement frontend) ne modifie donc pas ce comportement firmware.

#### Logs de transition (`SensorManager`)

Le module `SensorFilter` est **100 % pur** (pas de logger, ADR-0017) : c'est la coquille `SensorManager` qui détecte la transition de `unstableLatched()` et journalise (une ligne par transition, par capteur) :

- **Pose** — `warning` : `Filtre pH : latch EMI posé (oscillation soutenue > seuil) — dosage inhibé` (idem `Filtre ORP : …`).
- **Auto-levée** — `info` : `Filtre pH : latch EMI levé (signal stable) — dosage à nouveau possible si mesure OK` (idem ORP).

Getters associés : `unstableLatched()` (état du latch EMI), `dispersionCleanStreak()` (progression vers l'auto-levée).

### Observabilité de la dispersion (v2.20.6)

La dispersion est le **moteur** des deux mécanismes ci-dessus (cap → `unstable`, dépassement soutenu → latch EMI), donc de l'**inhibition du dosage automatique**. Jusqu'en v2.20.5 elle n'était exposée par **aucune route** : l'inhibition était pilotée par une valeur que personne ne pouvait observer, ni depuis l'interface, ni depuis l'API. Conséquence concrète : pour évaluer la marge au seuil pendant une campagne de mesure, il fallait la **reconstruire de l'extérieur** par un minorant (crête-à-crête sur des relevés espacés de 30 s, alors que le critère porte sur 13 mesures à 5 s) — une incertitude qu'aucune analyse ne pouvait lever.

Quatre champs par capteur sont désormais publiés par [`GET /debug/sensor_filter_state`](../API.md#get-debugsensor_filter_state--pas-dauth-cohérent-avec-autres-debug) (route toujours **sans auth**, lecture seule) :

| Champ JSON | Source firmware | Rôle |
|---|---|---|
| `dispersion` | `getPhDispersion()` / `getOrpDispersion()` → `SensorFilter::dispersion()` | Amplitude `max − min` de la fenêtre pleine ; `null` si `NaN` |
| `dispersion_max` | `kPhFilterMaxDispersion` / `kOrpFilterMaxDispersion` | Seuil (0,5 pH / 100 mV) — évite au client de connaître les constantes |
| `latch_cycles` | `kSensorFilterDispersionLatchCycles` | Fenêtres consécutives en dépassement avant latch (3) |
| `clean_streak` | `getPhDispersionCleanStreak()` / `getOrpDispersionCleanStreak()` | Progression vers l'auto-levée (12 requises) |

Arrondi de sérialisation : **3 décimales** en pH, **1 décimale** en mV. Lecture **lock-free** (scalaires), cohérente avec le reste de la route.

**Rattachement à la porte d'alimentation** — `dispersion` vaut `null` dès que la fenêtre n'est pas pleine, ce qui inclut le cas **porte fermée** ([Porte d'alimentation](#porte-dalimentation-du-filtre-v2203)) : `addSample()` sort en tête, le buffer reste vide, il n'y a **rien à mesurer**. Ce n'est pas un défaut, c'est le comportement voulu.

> ⚠️ **Piège de lecture, à ne jamais confondre** : circulation arrêtée, `unstable = false` ne signifie **pas** « mesure stable » mais « **aucun jugement rendu**, faute de données ». La confusion conduit à déclarer saine une configuration qui n'a simplement jamais été évaluée — elle s'est produite en analyse. La seule lecture valide de l'instabilité est **filtration en marche + `dispersion` non nulle**.

**Échelle** : la dispersion est calculée sur le buffer interne, en échelle **brute**. Une amplitude étant invariante par translation, elle est **insensible à `ph_offset`** — contrairement aux bornes de la sortie de secours (exprimées en brut, donc à décaler, cf. [Sortie de secours](#sortie-de-secours-sur-rejets-cohérents-v2203)) et contrairement à `median` / `filtered` de la même route, qui sont corrigés ([ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)).

### Paramètres (centralisés dans [`constants.h`](../../src/constants.h), bloc feature-058)

| Paramètre | pH | ORP |
|---|---:|---:|
| Fenêtre médiane logique (`kSensorFilterCentralWindow`) | 13 | 13 |
| Capacité physique buffer (`kSensorFilterCentralWindowMax`) | 16 | 16 |
| EMA alpha | `kPhEmaAlpha` = 0.10 | `kOrpEmaAlpha` = 0.08 |
| Spike dur isolé écarté | `kPhFilterSpikeReject` = 1.0 pH | `kOrpFilterSpikeReject` = 300 mV |
| Cap dispersion → instable | `kPhFilterMaxDispersion` = 0.5 pH | `kOrpFilterMaxDispersion` = 100 mV |
| Plage plausible | `kPhFilterMin/Max` = 0.0 – 14.0 | `kOrpFilterMin/Max` = -1000 – +1500 mV |
| Rejets consécutifs → instable (`kSensorFilterMaxConsecutiveRejects`) | 10 | 10 |
| Fenêtres en dépassement → latch EMI (`kSensorFilterDispersionLatchCycles`) | 3 | 3 |
| Âge max ready (`kSensorFilterMaxAgeMs`) | 20 000 ms | 20 000 ms |
| **v2.20.3** — Rejets cohérents avant ré-amorçage (`kSensorFilterSpikeRearmCount`) | 12 (≈ 60 s) | 12 (≈ 60 s) |
| **v2.20.3** — Cohérence de la série rejetée (`k*FilterRearmCoherence`) | 0,20 pH | 40 mV |
| **v2.20.3** — Bande admissible de ré-amorçage (`k*FilterRearmMin/Max`) | 6,50 – 8,20 (**brut**) | 300 – 950 mV |
| **v2.20.3** — Cooldown entre deux ré-amorçages (`kSensorFilterRearmCooldownMs`) | 30 min | 30 min |
| **v2.20.3** — Plancher de gel après front montant d'eau (`kSensorFilterGateFloorMs`) | 60 s | 60 s |
| **v2.20.3** — Délai avant alarme « filtre non prêt » (`kSensorFilterNotReadyAlarmMs`) | 5 min | 5 min |
| **v2.20.3** — Répétition de l'alarme (`kSensorFilterNotReadyRepeatMs`) | 30 min | 30 min |
| **v2.20.3** — Cooldown du réarmement manuel UI (`kSensorFilterUiResetMinIntervalMs`) | 10 min | 10 min |

> **Latence** : la fenêtre médiane passe de 7 à 13 échantillons → retard médian ≈ 30 s (à 5 s/échantillon), soit **+15 s** vs l'ancien filtre. Négligeable devant la **pause mélange hydraulique** post-injection (15–20 min) ; pH/ORP varient lentement.

### Reset après calibration

Une calibration change la fonction de transfert de la sonde → la valeur filtrée pré-calibration n'est plus représentative. Après **succès** d'une calibration via `_processEzoQueue()` :

- **pH** (`mid` / `low` / `clear`) → `resetPhFilter()` ;
- **ORP** (`cal` / `clear`) → `resetOrpFilter()`.

Le filtre repasse en **warmup** → dosage de la sonde concernée bloqué jusqu'à ce que la **fenêtre centrale soit de nouveau pleine** (`kSensorFilterCentralWindow` = 13 nouvelles mesures valides ; cumulé avec le timer de stabilisation post-cal `kStabilizationDuration*Ms`, gates indépendantes).

### Reset manuel / diagnostic

Le reset est **sélectif par capteur depuis la v2.20.3** — réparer le pH ne sacrifie plus un filtre ORP sain (ce qui est arrivé lors du déblocage en production du 2026-08-16) :

| Route | Auth | Portée |
|---|---|---|
| `POST /reset-sensor-filter?sensor=ph\|orp` | **CRITICAL** | **un seul** capteur (paramètre obligatoire), cooldown 10 min/capteur → `429` |
| `POST /debug/sensor_filter_reset?sensor=ph\|orp` | **CRITICAL** (⚠️ était sans auth ≤ v2.20.2) | un seul capteur ; **sans paramètre** = les deux (comportement historique) |
| `GET /debug/sensor_filter_state` | pas d'auth | lecture seule (raw/median/filtered/ready/unstable/rejected par capteur ; **+ dispersion/dispersion_max/latch_cycles/clean_streak depuis la v2.20.6**) |

Un reset reste la **levée manuelle immédiate** du latch EMI (`unstableLatched()`), en complément de l'auto-levée après ~2 min de signal propre (v2.8.4) et de la levée sur calibration réussie. Voir [API.md](../API.md#post-reset-sensor-filter--critical).

### Réarmement manuel par capteur (v2.20.3)

**Contrat de concurrence** — `resetPhFilter()` / `resetOrpFilter()` écrivent dans le filtre : elles sont réservées au contexte **`loopTask`**. Plus **aucun** handler asynchrone ne les appelle directement (l'ancienne route de debug le faisait, en violation du contrat `sensor_filter.h` : écriture concurrente du buffer pendant un `addSample()`).

Depuis n'importe quel contexte (`async_tcp`, `mqttTask`) on passe par :

```cpp
bool requestFilterReset(bool ph, bool orp, const char* origin = "inconnue");
```

- Pose un drapeau `volatile` **consommé en tête de `SensorManager::update()`** — même pattern que `enqueuePhSlopeQuery()`. La réponse HTTP est donc « **acceptée** », pas « exécutée » (effectif < 100 ms).
- `origin` **doit** être un littéral de chaîne / une constante statique : seul le **pointeur** est mémorisé (aucune copie, aucune allocation dans un contexte async).
- Retourne `false` si aucun filtre n'est demandé.

**Journalisation de l'état détruit** (`_executeRequestedReset()`, au moment de l'exécution) — médiane avant → après, rejets, latch EMI, filtre prêt, sonde figée :

- `warning` en cas normal ;
- `critical` si une **détection de sonde figée** vient d'être effacée : `… — la DÉTECTION DE SONDE FIGÉE vient d'être effacée : si la sonde est réellement HS, le défaut sera de nouveau masqué au prochain cycle`.

### Alarme « filtre non prêt durablement » (v2.20.3)

Un filtre non prêt **alors que** la filtration tourne **et que** les mesures sont déclarées représentatives = plus **aucune** régulation sur ce paramètre. C'est exactement l'état dans lequel l'installation est restée ≥ 3 h le 2026-08-16, **sans la moindre alarme**. Verdict `pool-chemistry` :

- déclenchement après `kSensorFilterNotReadyAlarmMs` = **5 min** de blocage **continu** (toute interruption remet le compteur à zéro) ;
- la **purge** de la chambre de mesure (`measures_valid == false`) et la **circulation arrêtée** ne comptent **jamais** : ce sont des états bloquants légitimes ;
- **pose** : `critical` + alerte MQTT (`ph_filter_not_ready` / `orp_filter_not_ready`), avec la **cause probable** diagnostiquée (`sonde figée` / `ré-amorçage refusé (sonde suspecte)` / `capteur instable (bruit/EMI)` / `fenêtre de mesure non remplie`) et le compteur de rejets ;
- **répétition** toutes les `kSensorFilterNotReadyRepeatMs` = **30 min** tant que la panne dure ;
- **levée** : `info` — `Filtre pH : de nouveau PRÊT — régulation pH automatique réarmée (alerte levée)`.

Accesseurs : `isPhFilterNotReadyAlarm()` / `isOrpFilterNotReadyAlarm()`. Consommés par `GET /data` (`phFilterAlarm` / `orpFilterAlarm`), le WebSocket, MQTT (`{base}/ph_filter_stuck`, `binary_sensor` HA `device_class: problem`) et l'UI (chip d'état + bannière « Régulation à l'arrêt »).

## Offset logiciel de mesure pH — feature-067

Correction logicielle `pH corrigé = pH brut EZO + phOffset`, destinée à compenser un **écart d'implantation** de la sonde (position dans le circuit, potentiel électrique de l'eau). **La calibration interne du module Atlas EZO n'est JAMAIS touchée** : aucune commande `Cal,*` n'est émise, l'offset vit strictement **en aval** de la lecture.

- Configuration : `mqttCfg.phOffset` ([`config.h`](../../src/config.h)), persistée en NVS sous la clé `ph_offset`, défaut `0.0f`. Une valeur relue hors bornes est **neutralisée à 0,00** avec un `WARN` (fail-safe).
- Bornes : `kPhOffsetMin = -0.50f` / `kPhOffsetMax = 0.50f` ([`constants.h`](../../src/constants.h)).
- Logique **pure** isolée dans [`src/ph_offset.h`](../../src/ph_offset.h) — `applyPhOffset(value, offset)` (NaN in → NaN out) et `isPhOffsetValid(offset)`, header self-contained sans Arduino, testé en natif (suite `test_native_ph_offset`, pattern [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).

### Point d'application : EN SORTIE des accesseurs

Le filtre pH (`SensorFilter` : médiane longue + EMA + rejet de pics + latch EMI) continue de travailler sur le signal **brut** ; l'offset est ajouté **au moment de lire** la valeur. Médiane et EMA étant **invariantes par translation** (`filtre(x + c) == filtre(x) + c`, propriété démontrée par un test natif sur le filtre réel), le résultat numérique est identique — mais :

1. un changement d'offset est pris en compte **immédiatement**, au lieu d'être dilué sur toute la fenêtre de lissage (≈ 13 échantillons ≈ 65 s) ;
2. les **seuils de rejet de pics** et le **latch EMI** restent calés sur le signal brut, donc inchangés ;
3. la **trace de diagnostic** conserve naturellement le brut.

Décision structurante et alternatives écartées : [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md).

> ⚠️ **Divergence assumée avec `tempCalibrationOffset`.** L'offset de température s'applique lui **à l'ingestion**, par **nécessité** : la valeur corrigée est envoyée à l'EZO pH (`RT,<temp>`) pour la compensation thermique — c'est la valeur corrigée qui doit partir sur le bus. Le pH, lui, n'est **réinjecté nulle part** : rien n'oblige à corriger tôt, et corriger tard apporte l'immédiateté. Le patron « offset température » n'est donc **pas transposable** au pH ; ce n'est pas une incohérence.

### Contrat des accesseurs pH — règle mnémotechnique

> **Seuls les accesseurs suffixés `*Raw` renvoient du BRUT capteur.**

| Accesseur | Valeur | Offset | NaN quand |
|---|---|---|---|
| `getPh()` | dernière mesure **corrigée** | ✅ | brut NaN ou lecture stale (> `kSensorStaleTimeoutMs`) |
| `getPhMedian()` | médiane **corrigée** | ✅ | fenêtre médiane vide |
| `getPhFiltered()` | EMA **corrigée** — entrée du PID | ✅ | filtre non amorcé (warmup) |
| `getPhRaw()` | dernière mesure **brute EZO** | ❌ | brut NaN ou lecture stale |
| `getPhFilteredRaw()` | EMA **brute** — filets de sécurité chimique | ❌ | **filtre non `ready()`** (voir garde d'âge) |
| `getPhOffset()` | offset **effectivement appliqué** (copie locale) | — | jamais (toujours dans les bornes) |

⚠️ `getPhRaw()` était historiquement un **alias pur** de `getPh()`. Depuis la v2.20.0 les deux divergent dès que l'offset est non nul : `getPhRaw()` est la source unique de la fenêtre stale, `getPh()` s'appuie dessus et y ajoute l'offset. Tout code qui utilisait `getPhRaw()` en pensant « la même chose que `getPh()` » doit être relu.

Côté ORP, rien ne change : `getOrpRaw()` reste un alias de `getOrp()` et aucun offset n'existe.

### Garde d'âge de `getPhFilteredRaw()` — pourquoi elle existe

`getPhFilteredRaw()` retourne `_phFilter.filtered()` **seulement si** `_phFilter.ready(millis())`, sinon `NaN` :

```cpp
float SensorManager::getPhFilteredRaw() const {
  return _phFilter.ready(millis()) ? _phFilter.filtered() : NAN;
}
```

Raison (exigence `pool-chemistry`) : par contrat feature-058, **`filtered()` n'expire jamais de lui-même** — c'est `ready()` qui porte la fraîcheur. Sans cette garde, une EMA **périmée** (EZO muet depuis plusieurs minutes) resterait lisible et pourrait :

- déclencher un **faux plancher** de refus d'acide, et consommer le compteur des 3 refus jusqu'à basculer la régulation pH en manuel sur une donnée fantôme ;
- ou, symétriquement, **masquer un vrai franchissement** de seuil en gelant une valeur rassurante.

`NaN` signifie donc « pH brut **inconnu** », que chaque filet de sécurité interprète selon sa propre logique fail-closed ([pump-controller.md](pump-controller.md#filets-de-sécurité-sur-le-ph-brut-feature-067-v2200)).

### Copie locale `_phOffset` et prise en compte immédiate

`SensorManager` conserve une **copie locale** de l'offset (`_phOffset`) plutôt que de lire `mqttCfg.phOffset` à chaque accès (les accesseurs sont sur des chemins chauds et lock-free). Elle est rechargée par `refreshPhOffsetCache()` :

- au **boot**, depuis `begin()` ;
- après **tout** enregistrement de configuration (`POST /save-config`).

La méthode revalide la valeur (`isPhOffsetValid`) et neutralise à 0,00 si besoin, puis journalise le changement en `INFO` : `Offset pH modifié: ancien=0.00 nouveau=0.15` (traçabilité exigée par `pool-chemistry` — tout changement d'entrée de la régulation pH doit être daté). Aucun mutex : écriture d'un `float` aligné, atomique sur Xtensa LX6 (même contrat que `_lastPh`).

### Consommateurs

| Consommateur | Valeur consommée |
|---|---|
| PID / régulation pH ([`pump_controller`](pump-controller.md)) | **corrigée** (`getPhFiltered()`) |
| Filets de sécurité chimique (planchers pH brut, inhibition ORP) | **brute** (`getPhFilteredRaw()`) |
| `/data`, WebSocket `sensor_data`, MQTT `{base}/ph` et `{base}/ph_filtered` | **corrigée** (repli warmup sur `getPh()`, jamais sur le brut) |
| MQTT `{base}/ph_raw`, test de vivacité `phStale` de l'alerte `sensor_stale` | **brute** |
| [`history`](history.md) (`/history.json`) | **corrigée** (`getPh()`) — ⚠️ discontinuité à l'activation, cf. CHANGELOG 2.20.0 |
| Health check `ph_abnormal` ([`main.cpp`](../../src/main.cpp)) | détection sur la **corrigée**, message citant **toujours** brut + offset |
| Trace `/debug/sensor_trace` | `phRaw` et `phFiltered` **bruts**, `phCorrected` = brut + offset |
| Écran de calibration (UI) | **brut** exclusivement ([page-ph.md](../features/page-ph.md#lécran-de-calibration-reste-sur-le-brut)) |

## Trace des signaux capteurs — feature-057

Ring buffer de trace **haute cadence** des signaux capteurs (pH & ORP, brut **et** lissé), réintroduction fidèle de l'outil retiré en feature-045 (v2.5.0) et **étendue à l'ORP**. Objectif : diagnostiquer bruit / oscillation de la chaîne de mesure en comparant brut vs lissé sur une fenêtre continue, à cadence bien supérieure à l'échantillonnage WS. Alimente la card « Signaux capteurs » de l'onglet Avancé ([page-settings.md](../features/page-settings.md#card-signaux-capteurs-brut--lissé-feature-057)) et les routes `GET /debug/sensor_trace` / `POST /debug/sensor_trace_clear` ([API.md](../API.md#trace-des-signaux-capteurs-feature-057-v270)).

### Structure et coût mémoire

```cpp
struct SensorTraceSample {
  uint32_t ms;          // millis() à l'enregistrement
  float    phRaw;       // pH brut Atlas EZO, SANS offset (NaN si lecture échouée)
  float    phCorrected; // feature-067 : pH brut + offset logiciel (NaN si brut NaN)
  float    phFiltered;  // pH lissé (médiane + EMA), SANS offset (NaN si filtre vide)
  float    orpRaw;      // ORP brut (NaN si lecture échouée)
  float    orpFiltered; // ORP lissé (NaN si filtre vide)
  float    tempEau;     // T° eau envoyée à l'EZO pour compensation (NaN si sonde absente)
};
SensorTraceSample _traceBuffer[kSensorTraceBufferSize];  // 300 × 28 octets ≈ 8,4 Ko RAM
```

> **feature-067 — la trace est le seul endroit où `phFiltered` est BRUT.** Objectif de l'outil : pouvoir dire à tout instant **ce qui vient de l'EZO** et **ce qui vient de la correction**. Les trois champs pH sont donc `phRaw` (brut), `phFiltered` (brut filtré) et `phCorrected` (brut + offset). Partout ailleurs (`/data`, WS, MQTT), `phFiltered` est **corrigé**.

- `kSensorTraceBufferSize = 300` ([`constants.h`](../../src/constants.h)) → ~8,4 Ko RAM statique depuis la v2.20.0 (+1,2 Ko dus au champ `phCorrected` ; ~7,2 Ko auparavant).
- **Volatil** : perdu au reboot (comme l'original — pas de persistance, hors périmètre).
- Les NaN sont **enregistrés tels quels** (filtre non amorcé, EZO injoignable) et exposés en `null` côté JSON → trou dans la courbe, pas de crash.

### Enregistrement

`_recordSensorTrace(phRaw, phFiltered, orpRaw, orpFiltered, tempEau)` est appelée à la **fin de `_readEzoSensors()`** (donc une fois par cycle capteur, `kPhOrpSensorIntervalMs` ≈ 5 s). L'enregistrement **n'ajoute aucune transaction I²C** : il consomme les valeurs déjà en cache. Watchdog non impacté.

### API publique associée

| Méthode | Effet |
|---------|-------|
| `size_t getSensorTraceCount() const` | Nombre d'échantillons présents (0..300). |
| `void appendSensorTraceJson(JsonArray out, size_t offset, size_t count) const` | Sérialise une **page** du buffer (ordre chronologique, `offset` 0 = le plus ancien) : les échantillons `[offset, offset+count)` — `t` (ms), `phRaw`/`phCorrected`/`phFiltered` (arrondi ×1000), `orpRaw`/`orpFiltered` (×10), `tempEau` (×10) ; `null` si NaN. Signature paginée depuis v2.10.1 (bug-crash-sensor-trace-badalloc) ; champ `phCorrected` ajouté en v2.20.0 (page de 50 ≈ 4,5 Ko). |
| `void clearSensorTrace()` | Vide le buffer (`_traceCount = 0`). |

### Pagination — bug-crash-sensor-trace-badalloc (v2.10.1)

La route `GET /debug/sensor_trace` sérialisait les 300 échantillons dans un seul `AsyncResponseStream` (~20 Ko contigus dans son `cbuf`) → `std::bad_alloc` non rattrapé → `abort()` → reboot sur heap fragmenté (coredump v2.10.0, 2026-07-23). Elle est désormais **paginée** (`?offset=N&count=M`, page bornée à `kSensorTraceMaxSamplesPerPage = 50` ≈ 3,5 Ko) avec **garde heap** (`measureJson` + `kSensorTraceHeapMarginBytes = 4096` vs `ESP.getMaxAllocHeap()` → `503` propre) et stream **pré-dimensionné** (une seule allocation exacte, zéro `resize()`).

Le clamp des bornes de page est un calcul **pur header-only** dans [`src/sensor_trace_page.h`](../../src/sensor_trace_page.h) (`computeTracePage()`, invariants `offset ≤ total`, `n ≤ max`, `offset + n ≤ total` pour tout input — testé en natif, suite `test_native_trace_page`, 16 tests, pattern [ADR-0017](../adr/0017-logique-pure-testable.md)). `appendSensorTraceJson()` applique en plus un **re-clamp défensif interne** sur son snapshot local de `_traceCount` (le buffer peut avoir glissé entre le clamp de la route et la lecture). Contrat complet de la route : [API.md](../API.md#get-debugsensor_trace--pas-dauth-cohérent-avec-autres-debug).

### Contrat de concurrence — lock-free

Même modèle que `_lastPh`/`_lastOrp` : **un seul producteur** (`loopTask` core 1 via `_recordSensorTrace()`), **un seul type de lecteur** (handler AsyncTCP core 0 via `appendSensorTraceJson()` / `getSensorTraceCount()`). Aucun mutex : l'écriture d'un échantillon puis l'incrément de `_traceCount` sont des écritures atomiques 32 bits sur Xtensa LX6 ; le lecteur ne voit au pire qu'un échantillon de moins (jamais de valeur déchirée). La pagination (v2.10.1) **n'introduit aucun verrou** : le glissement du ring entre deux pages (~1 échantillon max) est toléré (route de diagnostic). Handler HTTP non bloquant (< 50 ms), réponse pré-dimensionnée (`beginResponseStream(type, taille)`) sans allocation dans la loop.

## Détection capteur figé — feature-022

Un module EZO peut répondre sans erreur I²C mais retourner indéfiniment la **même valeur** (électronique interne figée, sonde HS côté BNC). La panne franche (NaN / stale / bus dégradé) est couverte ailleurs ; ici on détecte la **variance nulle** : N lectures **acceptées** consécutives contenues dans une bande < epsilon → capteur figé.

### Classe pure `FrozenDetector`

Définie dans [`src/sensor_filter.h`](../../src/sensor_filter.h) (logique pure, testée en natif — pattern [ADR-0017](../adr/0017-logique-pure-testable.md)). Algorithme « **run ancré** » en **O(1)** mémoire et CPU — pas de fenêtre glissante stockée :

- Le run courant est décrit par 3 champs : `runMin` / `runMax` (bornes observées) et `runCount` (longueur, saturé à `UINT16_MAX`).
- Chaque échantillon qui garde `(newMax − newMin) < epsilon` **étend les bornes** et allonge le run.
- Dès qu'une lecture sort de la bande, le run est **ré-ancré sur cette lecture** (`runMin = runMax = x`, `runCount = 1`) → **sortie immédiate de l'état figé, sans re-warmup**.
- `frozen()` = `runCount >= samples` ; `samples = 0` désactive la détection (compat init positionnelle courte de `SensorFilter::Config` dans les tests).

| Instance | Alimentée par | N (`samples`) | epsilon | Effet si figé |
|---|---|---:|---:|---|
| `_phFilter` (composé dans `SensorFilter`) | lectures brutes **acceptées** par le filtre pH | `kSensorFrozenSamples` = 30 (≈ 2,5 min à 5 s) | `kSensorFrozenEpsilonPh` = 0.0005 (½ LSB EZO pH, résolution 0.001) | `ready()` = false → **dosage pH bloqué fail-closed** |
| `_orpFilter` (idem) | lectures brutes acceptées par le filtre ORP | 30 | `kSensorFrozenEpsilonOrp` = 0.05 mV (½ LSB EZO ORP, résolution 0.1) | dosage ORP bloqué fail-closed |
| `_waterTempFrozen` (dédié, dans `SensorManager`) | lectures DS18B20 **valides** de la sonde « eau » (valeur **brute**, non arrondie au 0.1 °C) | `kTempFrozenSamples` = 900 (≈ 30 min à 2 s) | `kTempFrozenEpsilonC` = 0.03 °C (< ½ LSB DS18B20 12 bits = 0.0625/2) | **warning-only** — AUCUN impact dosage/filtration (effets réels : compensation EZO pH + planning auto filtration sur valeur figée) |

### Choix d'epsilon = ½ LSB (validation pool-chemistry)

Un capteur **vivant** bruite d'au moins ±1 LSB (quantification + bruit sonde + bruit thermique). Avec epsilon = ½ LSB, un simple toggle de 1 LSB — même en arithmétique float32 — **casse le run** : aucune eau réelle, même parfaitement stable, ne peut être déclarée figée tant que la dernière décimale bouge.

> **Consigne terrain** : en cas de faux positifs, **durcir N** (allonger la fenêtre), ne **JAMAIS élargir epsilon** — un epsilon plus large capturerait le bruit LSB d'un capteur vivant et transformerait la stabilité réelle de l'eau en faux positif.

### Intégration fail-closed dans `SensorFilter`

- Le détecteur est alimenté **uniquement par les bruts ACCEPTÉS** dans `addSample()` : les rejets (NaN / hors plage / saut) ne l'alimentent jamais → l'état figé **persiste pendant une rafale de rejets** (pas d'échappatoire par le bruit EMI).
- `frozen()` ⇒ `ready() == false` ⇒ garde `FilterNotReady` existante de `evaluateDose()` — **aucune modification de l'ordre des gardes verrouillé**, pas de nouvelle valeur d'enum `DoseRefusal`.
- `SensorFilter::reset()` (calibration réussie, reset manuel) réinitialise aussi le détecteur.
- La sortie de l'état figé est **immédiate** (une seule lecture vivante suffit) et **sans re-warmup** : la valeur figée était par définition stable, le filtre est resté cohérent.

### Observabilité

- **Logs edge-triggered `[SENSOR_FROZEN]`** (`_checkFrozenAndLog()`, une seule ligne par transition) : `critical` pH/ORP (mentionne « régulation auto inhibée (filtre non prêt) » — c'est CE log qui porte le diagnostic, la garde mutualisée `FilterNotReady` ne dit pas la cause), `warning` température, `info` à la levée.
- **MQTT** : alerte `{base}/alerts/sensor_frozen` (JSON retain, clear payload vide, pH/ORP uniquement) + états binaires `{base}/ph|orp_sensor_problem` (`ON` = stale OU figé) avec discovery HA `binary_sensor` `device_class: problem` — voir [MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-feature-022-problème-capteur-phorp-v2100).
- **Getters lock-free** : `isPhSensorFrozen()` / `isOrpSensorFrozen()` / `isTemperatureFrozen()` (simple comparaison d'entiers 16 bits, même contrat de concurrence que `is*FilterUnstable()`).

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
| `kPhSlopeBootRetryIntervalMs` | `30_000` ms | Intervalle entre retentatives `Slope,?` de boot (v2.21.2) |
| `kPhSlopeBootRetryMaxAttempts` | `5` | Borne DURE de retentatives de boot, jamais réarmée (gate C1 pool-chemistry) |
| `kSensorFilterCentralWindow` | `13` | Fenêtre médiane logique longue (feature-058, retard ≈30 s) |
| `kSensorFilterCentralWindowMax` | `16` | Capacité physique buffer médian FIXE |
| `kPhEmaAlpha` / `kOrpEmaAlpha` | `0.10` / `0.08` | Coefficient EMA (lissage lent) |
| `kPhFilterSpikeReject` / `kOrpFilterSpikeReject` | `1.0` pH / `300` mV | Spike DUR isolé (`|raw − médiane|`) → écarté (feature-058) |
| `kPhFilterMaxDispersion` / `kOrpFilterMaxDispersion` | `0.5` pH / `100` mV | Cap dispersion fenêtre pleine → instable (feature-058) |
| `kPhFilterMin/Max` / `kOrpFilterMin/Max` | `0/14` pH / `-1000/1500` mV | Plage plausible |
| `kSensorFilterMaxConsecutiveRejects` | `10` | Rejets NaN/hors-plage consécutifs → instable (transitoire) |
| `kSensorFilterDispersionLatchCycles` | `3` | Fenêtres pleines en dépassement de dispersion → latch EMI (≈ 15 s pour poser) |
| `kSensorFilterLatchClearCycles` | `12` | Fenêtres pleines propres consécutives → auto-levée du latch EMI (≈ 2 min effectif avec purge médiane, v2.8.4) |
| `kSensorFilterMaxAgeMs` | `20000` ms | Âge max dernière mesure valide pour `ready()` |
| `kSensorFrozenSamples` | `30` | Lectures acceptées consécutives dans la bande → figé (≈ 2,5 min à 5 s — feature-022) |
| `kSensorFrozenEpsilonPh` | `0.0005` pH | Bande figé pH = ½ LSB EZO (résolution 0.001) |
| `kSensorFrozenEpsilonOrp` | `0.05` mV | Bande figé ORP = ½ LSB EZO (résolution 0.1) |
| `kTempFrozenSamples` / `kTempFrozenEpsilonC` | `900` / `0.03` °C | Figé T° eau (≈ 30 min à 2 s, < ½ LSB DS18B20) — warning-only |
| `kPhOffsetMin` / `kPhOffsetMax` | `-0.50` / `+0.50` pH | Bornes de l'offset logiciel pH (feature-067) — validation serveur, refus 400 hors plage |
| `kSensorTraceBufferSize` | `300` | Taille ring buffer trace signaux (~8,4 Ko RAM depuis feature-067) |
| `kSensorTraceMaxSamplesPerPage` | `50` | Borne d'une page de `GET /debug/sensor_trace` (~3,5 Ko JSON — v2.10.1) |
| `kSensorTraceHeapMarginBytes` | `4096` | Marge entre `measureJson` et `ESP.getMaxAllocHeap()` → 503 au lieu de `bad_alloc` (v2.10.1) |

Toutes définies dans [`src/constants.h`](../../src/constants.h).

## Concurrence

- `update()` : tourne dans `loopTask` (core 1). Seul producteur des caches `_lastPh`, `_lastOrp`, `_phCalCachedPoints`, `_orpCalCachedPoints`.
- `getPh()` / `getOrp()` / `getPhCalibrationPointsCached()` : lectures atomiques (float / int 32 bits alignés sur Xtensa LX6 → instructions L32I single-cycle). Pas de mutex applicatif.
- `enqueue*()` : producteurs depuis n'importe quel core / contexte (handler HTTP core 0, futurs producteurs core 1, …). FreeRTOS queue est ISR-safe.
- Ring buffer de trace (feature-057) : seul producteur `loopTask` (`_recordSensorTrace`), lecteur handler AsyncTCP core 0 (`appendSensorTraceJson` / `getSensorTraceCount`). Lock-free (écritures atomiques 32 bits), voir [Trace des signaux capteurs](#trace-des-signaux-capteurs--feature-057).
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
| `_phEzoDetected` | `bool` | Gate C2 (v2.21.2) : `readInfo()` pH OK au boot — jamais de retentative sans module détecté |
| `_phSlopeBootAttempts` | `uint8_t` | Retentatives de boot décomptées — **jamais remis à zéro** (gate C1) |
| `_phSlopeBootLastAttemptMs` | `uint32_t` | `millis()` de la dernière retentative de boot |
| `_phSlopeBootFirstFailLogged` / `_phSlopeBootExhaustedLogged` | `bool` | Flags one-shot des 2 warnings de boot (jamais réarmés) |

### Politique de refresh

1. **Au boot** : 1 query enfilée après init EZO (1ʳᵉ valeur disponible dans les ~30 s) — **retentée automatiquement en cas d'échec** depuis la v2.21.2 (cf. [Retentative bornée de boot](#retentative-bornée-de-la-slope-de-boot--v2212)).
2. **Après chaque calibration pH réussie** (mid / low / clear) : re-query enfilée automatiquement par `_processEzoQueue()` pour rafraîchir l'info dans les ~5 s.
3. **Automatique 24 h** : `update()` enfile une re-query si `(nowAfterQueue - _phSlopeQueriedMs) >= kPhSlopeQueryIntervalMs` ET `!_phSlopeQueryPending`.
4. **À la demande** : `POST /debug/ph_slope_refresh` (cf. [API.md](../API.md)) → `enqueuePhSlopeQuery()`.

> **Garde anti-underflow `nowAfterQueue`** (commit `933f17c`, v2.1.1) : le `now` lu en début de `SensorManager::update()` est **figé** avant `_processEzoQueue()` qui peut bloquer ~900 ms sur une transaction I²C. Si le handler `QueryPhSlope` met `_phSlopeQueriedMs = millis()` à un instant postérieur, alors `now < _phSlopeQueriedMs` → soustraction `uint32_t` underflow → ~4,3 milliards → toujours ≥ 86 400 000 → ré-enqueue immédiat à chaque cycle `update()` → spam de `Slope,?` à ~1/s, monopolisation du mutex I²C, EZO ORP perturbé. Le firmware recalcule donc `nowAfterQueue = millis()` après `_processEzoQueue()` ET ajoute la garde explicite `nowAfterQueue >= _phSlopeQueriedMs` avant la soustraction.

### Retentative bornée de la `Slope,?` de boot — v2.21.2

**Cause racine** (bug-slope-boot-perdue-et-data-incomplet, diagnostic on-target) : la `Slope,?` enfilée au boot pouvait échouer **en silence** — statut EZO 255/NoData ou parasite pendant l'init du module, seuls chemins de `atlas_ezo.cpp` sans aucun log, et échec isolé (streak = 1 < `kEzoBusFailMaxConsecutive`) donc jamais journalisé. Elle n'était ensuite **jamais retentée** avant le cycle 24 h, car le re-enqueue exigeait `_phSlopeQueriedMs != 0` → « Pente non disponible » pendant ~20 min et plus après chaque reboot.

**Mécanisme** : décision **pure** dans [`src/slope_retry_logic.h`](../../src/slope_retry_logic.h) (`shouldRetryBootSlope()`, pattern ADR-0017, 15 tests natifs), consommée en fin d'`update()`. Retentative enfilée **seulement si TOUTES** ces conditions tiennent :

- `_phSlopeQueriedMs == 0` — aucune pente **jamais** lue (dès la 1ʳᵉ réussite, le cycle 24 h prend le relais) ;
- `!_phSlopeQueryPending` — anti-doublon ;
- `_phEzoDetected` — **gate C2** (pool-chemistry) : jamais de retry sans module EZO pH détecté au boot (chaque `Slope,?` bloque `loopTask` ~900 ms) ;
- `_phSlopeBootAttempts < kPhSlopeBootRetryMaxAttempts` (= 5) — **gate C1** : borne DURE de transactions ; le compteur est incrémenté **avant** l'enqueue (même si la file est pleine) et **jamais remis à zéro** ;
- cadencement : 1ʳᵉ retentative immédiate, puis `kPhSlopeBootRetryIntervalMs` (= 30 s) entre chaque.

> **Garde anti-underflow** (encodée dans la logique pure — même bug historique que le cycle 24 h, cf. blockquote ci-dessus) : si `nowMs < lastAttemptMs`, la soustraction non signée validerait l'intervalle à tort → refus et attente du cycle suivant. Choix **conservateur** documenté : un wrap de `millis()` légitime (~49 j, hors fenêtre de boot) est également refusé.

**Logs associés** (tous préfixés `EZO pH`) :

| Événement | Niveau | Format | Cadence |
|---|---|---|---|
| 1ᵉʳ échec d'une `Slope,?` alors qu'aucune pente n'a jamais été lue | `warning` | `EZO pH slope : échec de la Slope,? de boot (aucune pente jamais lue) — retentative automatique` | **one-shot** (flag jamais réarmé) |
| Retentative enfilée | `info` | `EZO pH : Slope,? de boot retentée (n/5)` — suffixe ` — file EZO pleine, tentative décomptée` si l'enqueue a échoué | 1 par retentative (5 max) |
| Épuisement des 5 retentatives sans pente lue | `warning` | `EZO pH : pente toujours jamais lue après 5 retentatives de boot — abandon (cycle 24 h inopérant, utiliser POST /debug/ph_slope_refresh)` | **one-shot** |
| Statut EZO hors protocole (≠ 1/2/254/255) sur **toute** commande | `warning` | `<nom> : statut EZO inattendu (<valeur>) — réponse ignorée` (`atlas_ezo.cpp`) | à chaque occurrence ; le cas 255/NoData reste muet (chemin nominal) |

### Dédoublonnage `_phSlopeQueryPending`

`enqueuePhSlopeQuery()` retourne `true` même si une query est déjà en file (« noop satisfait » — pas de spam de la queue 4 slots). Le flag est levé en début de handler `QueryPhSlope` pour permettre une demande suivante.

### Invalidation en mode dégradé

Si `_phSlopeFailStreak >= kEzoBusFailMaxConsecutive` (= `2`), les 3 valeurs passent à NaN. `_phSlopeQueriedMs` n'est **pas** remis à zéro : l'âge continue de croître côté UI, qui peut afficher l'état « stale » (encadré jaune si > 36 h).

### Publication

- **WebSocket** (`sensor_data`, toutes les 5 s) : 4 champs `phSlopeAcid` (1 décimale), `phSlopeBase` (1 décimale), `phSlopeZero` (2 décimales), `phSlopeAgeMs`. `null` si NaN ou jamais lu — voir [API.md](../API.md#ws-ws--write).
- **`GET /data`** (v2.21.2 — parité de transport) : les 4 **mêmes** champs, mêmes règles null/arrondi, lecture cache uniquement (zéro I²C dans le chemin async). Un client sans WS (poll seul) voit désormais la santé de sonde — voir [API.md](../API.md#get-data--write).
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

- [`pump_controller`](pump-controller.md) : le PID consomme **uniquement** `getPhFiltered()` / `getOrpFiltered()` (feature-025 ; **valeur corrigée** côté pH depuis feature-067) et exige `isPhFilterReady()` / `isOrpFilterReady()` (+ `!isPhFilterUnstable()`) dans `canDose(int)`. Les **filets de sécurité chimique** feature-067 consomment `getPhFilteredRaw()` (brut, avec garde d'âge) et `getPhOffset()`. `getPh()`/`getOrp()` restent disponibles pour les logs et l'affichage brut. Vérifie aussi `getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` (conditions #1, #2, #5).
- [`ws_manager`](ws-manager.md) : `broadcastSensorData()` publie `ph`/`orp` (= valeur **filtrée**, fallback brut si filtre non amorcé), `temperature`, `temperature_circuit`, `phCalPoints`, `orpCalPoints`, `phSlopeAcid/Base/Zero/AgeMs` toutes les 5 s. feature-025 : champs supplémentaires `ph/orpRaw|Median|Filtered`, `ph/orpFilterReady`, `ph/orpFilterUnstable`, `ph/orpRejectedCount`, `ph/orpMixingDelayActive`, `ph/orpDoseBlockedReason` ; v2.25.1 : `ph/orpDoseBlockedCode` (code machine de la cause de refus, parité `/data` ↔ WS).
- [`mqtt_manager`](mqtt-manager.md) : publie `{base}/ph` / `{base}/orp` (= **filtré**), `{base}/temperature`, `{base}/temperature_circuit`, `{base}/ph_cal_points`, `{base}/orp_cal_points` toutes les 10 s (`kMqttPublishIntervalMs`). 3 topics `{base}/ph_slope_*` edge-triggered. feature-025 : topics `{base}/ph|orp_raw|median|filtered|filter_ready|filter_unstable|rejected_count` + `{base}/ph|orp_mixing_delay_active` (retain). Alertes edge-triggered `{base}/alerts/calibration_required`, `{base}/alerts/sensor_stale` et `{base}/alerts/sensor_frozen` + états binaires dédupliqués `{base}/ph|orp_sensor_problem` (feature-022).
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
- [`src/ph_offset.h`](../../src/ph_offset.h) — logique pure de l'offset pH `applyPhOffset()` / `isPhOffsetValid()` (feature-067, testé en natif `test/test_native_ph_offset/`)
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — routes refondues `/calibrate_ph`, `/calibrate_orp`, `/calibrate_clear`
- [`src/web_routes_debug.cpp`](../../src/web_routes_debug.cpp) — routes `/debug/sensor_trace` (GET, paginée v2.10.1) + `/debug/sensor_trace_clear` (POST) (feature-057)
- [`src/sensor_trace_page.h`](../../src/sensor_trace_page.h) — clamp pur header-only `computeTracePage()` (pagination v2.10.1, testé en natif `test/test_native_trace_page/`)
- [`src/web_routes_sensor_id.cpp`](../../src/web_routes_sensor_id.cpp) — routes feature-020 inchangées
- [`src/constants.h`](../../src/constants.h) — toutes les constantes EZO + DS18B20
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001)
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0013](../adr/0013-identification-sondes-onewire.md) — identification 2 sondes DS18B20
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025)
- [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) — offset pH appliqué en sortie des accesseurs (feature-067)
- [feature-021](../../specs/features/done/feature-021-migration-atlas-ezo.md) — spec d'origine
- [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) — filtrage mesures + adaptation régulation
- feature-022 (`specs/features/`) — détection capteur figé `FrozenDetector` + observabilité HA (v2.10.0)
