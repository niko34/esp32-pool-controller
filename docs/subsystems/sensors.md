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

// Chaîne de mesure à 5 étages (feature-152, v2.68.0) — médiane(9) + lissage
// τ=45 s. NaN si warmup / sonde figée / mesure périmée. feature-067 : côté pH,
// seuls les accesseurs `*Raw` sont bruts (cf. contrat ci-dessous).
float getPhRaw()  const;             // Dernière brute pH EZO, SANS offset (NaN si stale) — diagnostic
float getPhMedian()  const;          // Médiane courante pH CORRIGÉE — diagnostic seul
float getPhFiltered()  const;        // VALEUR OFFICIELLE pH CORRIGÉE — PID/UI/MQTT/historique
float getPhValueRaw() const;         // VALEUR OFFICIELLE pH, échelle BRUTE + garde d'âge — filets de sécurité (ex-getPhFilteredRaw, CONSERVÉ, renommé)
void  refreshPhOffsetCache();        // Recharge l'offset depuis mqttCfg (boot + après save)
float getPhOffset() const;           // Offset actuellement APPLIQUÉ aux mesures
uint8_t getPhRejectedCount()  const; // Compteur glissant de rejets pH (étage 2, diagnostic seul, sature à 255)
float getOrpRaw()  const;
float getOrpMedian()  const;         // Diagnostic seul
float getOrpFiltered()  const;       // VALEUR OFFICIELLE ORP
uint8_t getOrpRejectedCount()  const;

// v2.25.1, mis à jour feature-152 — POURQUOI la valeur officielle est NaN
// (observabilité, cf. « Prédicats d'état du filtre »). État INTERNE composé
// par resolveFilterState() : priorité pannes d'abord. Consommé par canDose()
// pour QUALIFIER la cause de refus, JAMAIS pour décider du dosage (un seul
// test isnan() suffit désormais à la décision). Ne jamais reconstruire depuis
// get*Raw().
DoseFilterState getPhFilterState() const;
DoseFilterState getOrpFilterState() const;

void resetPhFilter();                // Reset filtre (warmup) — appelé après calibration pH
void resetOrpFilter();               // Reset filtre (warmup) — appelé après calibration ORP

// feature-022 — détection capteur figé (variance nulle), CONSERVÉE telle
// quelle par feature-152 (seul mécanisme restant vers « pas de valeur »
// durable). Un capteur figé rend la VALEUR OFFICIELLE NaN → dosage bloqué.
bool isPhSensorFrozen()  const;      // 30 lectures acceptées dans une bande < kSensorFrozenEpsilonPh
bool isOrpSensorFrozen() const;      // idem, bande < kSensorFrozenEpsilonOrp
bool isTemperatureFrozen() const;    // sonde eau DS18B20 — warning-only, AUCUN impact dosage (exposé : /data, WS, MQTT, badge UI — feature-068)

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

⚠️ **Depuis la v2.75.0 (feature-159), `phCalPoints`/`orpCalPoints` (champs WS) sont aussi la SEULE source de vérité côté frontend pour figer les boutons « Calibrer » d'un point déjà acquis** — à la (ré)ouverture de l'écran de calibration, et pas seulement pendant la session en cours. `-1` (EZO injoignable) y désarme tout bouton, y compris « Recommencer ». Voir [page-ph.md — Figeage des points déjà calibrés](../features/page-ph.md#figeage-des-points-déjà-calibrés-_calfrozen-feature-159-v2750).

## Queue FreeRTOS `_ezoQueue`

- **Capacité** : `kEzoQueueLen = 4` slots (`QueueHandle_t`).
- **Producteurs** : routes HTTP `/calibrate_*`, futurs handlers (boot wizard). *(Les commandes UART de l'écran externe, autre producteur historique, ont disparu en v2.17.0 — feature-065.)*
- **Consommateur** : `loopTask` via `_processEzoQueue()` — dépile **au plus 1 commande par cycle** pour limiter la durée d'occupation du bus.
- **Saturation** : `enqueue*()` retourne `false`. La route HTTP renvoie `503 calibration queue saturée — réessayer dans 1s`.
- **Acquittement** : la mise en queue est l'ack de la route HTTP (`{success:true, queued:true}`). L'UI observe l'avancement via les champs WS `phCalPoints` / `orpCalPoints` rafraîchis automatiquement après chaque calibration.

## Authentification du CORPS des routes de calibration (v2.46.1)

Les trois routes [`/calibrate_ph`, `/calibrate_orp`, `/calibrate_clear`](../API.md#calibration-atlas-ezo--feature-021) suivent le patron ESPAsyncWebServer **handler de requête + handler de corps**. Le `REQUIRE_AUTH(req, WRITE)` est posé dans le handler de **requête**, exécuté **APRÈS** le handler de corps. Jusqu'en 2.46.0, le corps mettait donc la commande EZO dans `_ezoQueue` **avant** toute vérification : le client recevait bien son `401`, et `loopTask` exécutait la calibration **~900 ms plus tard**. Il n'existe **pas de dé-calibration** — l'effet est **irréversible**.

**Pourquoi le défaut a survécu à plusieurs revues** : `REQUIRE_AUTH` **est présent**, trois lignes au-dessus du handler de corps. Un relecteur qui cherche la macro voit une route protégée. C'est exactement la mécanique du bug d'origine sur [`POST /save-config`](../API.md#post-save-config--critical), corrigé en v2.25.0 par `saveConfigBodyAuthorized()`. La macro n'est **pas** redondante avec la garde de corps et la garde ne **remplace pas** la macro : `REQUIRE_AUTH` produit le `401` propre, la garde empêche l'effet de bord.

### La garde `calibrationBodyAuthorized()`

[`src/web_routes_calibration.cpp:46`](../../src/web_routes_calibration.cpp:46) — appelée en **première instruction** des trois handlers de corps, avant tout `deserializeJson()` et avant tout `enqueue*()`.

- **Prédicat identique à `checkAuth()` moins le rate limiting** : `authManager.isEnabled()` faux → autorisé ; sinon `checkTokenAuth() || checkBasicAuth()`. Toute requête que `REQUIRE_AUTH` accepterait est acceptée ici — **aucune calibration légitime n'est refusée** (vérifié dans les deux sens).
- **Pas de `checkRateLimit()` ni de `sendAuthRequired()` dans la garde** : une seule réponse au client, un seul décompte de quota par requête. C'est le handler de requête qui répond.
- **Pas d'exemption `isFirstBootDetected()`**, contrairement au site de référence `/save-config` : aucune des trois routes n'appartient au flux de premier démarrage (l'interface les appelle via `authFetch` depuis la page Réglages ; `/calibrate_clear` n'a aucun appelant d'interface). L'exemption y serait un trou, pas une compatibilité.

### Portée chimique — deux régimes opposés, tous deux graves

Verdict `pool-chemistry` (**GO**, gravité **haute**) : l'effet de bord n'est pas un simple accès, c'est une **mesure falsifiée** en entrée du dosage.

| Commande passée | Régime | Conséquence |
|---|---|---|
| `Cal,clear` — et `Cal,mid` **seul**, l'EZO effaçant les autres points | **fail-closed** | `calPoints < requiredPoints` ([`pump_controller.cpp:1738`](../../src/pump_controller.cpp:1738), [`dosing_logic.h:180`](../../src/dosing_logic.h:180)) **refuse** le dosage automatique : arrêt **silencieux** de la régulation. À CYA ~35 mg/L, la boucle ORP saturant déjà son quota horaire chaque jour, la sous-chloration se compte **en jours**. |
| `/calibrate_orp` | **surdosage** | `requiredPoints = 1` : **un seul POST suffit**, la garde de calibration reste satisfaite. `reference` bornée `0..1000` mV, soit jusqu'à **±500 mV** de décalage sur une consigne de 530 mV → erreur écrêtée, régulation **P pur en saturation permanente**. |
| `Cal,mid` puis `Cal,low` (pH) | **surdosage** | Même chaîne côté pH : mesure faussée → erreur contre 7,30 → sortie saturée → 10 min/h × 26,0 mL/min = **260 mL/h d'acide**. |

> ⚠️ **Les filets de sécurité ne bornent pas, ils ralentissent.** Les gardes `kPhFloorCorrected` = **6,70** (pH corrigé), `kPhFloorRawHard` = **6,20** (pH brut) et `kPhOrpInhibit` = **7,60** (inhibition ORP) sont calculées **sur la mesure falsifiée**. Dérivées de `kPhOffsetMax`, elles couvrent l'**offset** de mesure, pas la **calibration** ([offset pH](#offset-logiciel-de-mesure-ph--feature-067)). Seules bornent réellement `maxPhMlPerDay` (défaut **300 mL/j**) et la limite horaire : elles plafonnent le **débit**, mais le quota **se réarme à minuit** — la **dérive cumulative n'a donc pas de plafond**. Le timer de stabilisation post-calibration ([`sensors.cpp:882-915`](../../src/sensors.cpp:882)) ne fait que **différer**.

### WARN throttlé — et pourquoi il l'est

Une tentative non authentifiée est journalisée `WARNING` **au plus 1 fois par 60 s et par route** (`kCalibrationAuthWarnThrottleMs`, statique locale **par site**), avec annonce du nombre de tentatives occultées quand le site redevient bavard :

```
[Calibration] /calibrate_ph NON AUTHENTIFIÉ depuis 192.168.1.50 — corps ignoré (aucune commande EZO mise en file) ; et 12 tentatives non journalisées
```

- **Le quota `kMaxRequestsPerMinute` ne borne PAS cet émetteur** : `checkRateLimit()` est appelé par `checkAuth()` dans le handler de **requête**, donc après le corps. Le handler de corps tourne pour **chaque** requête reçue, quota dépassé ou non. Motif chiffré : [logger.md](logger.md#émetteur-non-authentifié--le-motif-du-throttle-de-calibration-v2461).
- **Invariant : le chemin muet n'alloue rien** — sortie avant toute concaténation, aucune `String` construite. Ce chemin est atteignable sans authentification et s'exécute en RAM interne pendant une famine mémoire active (`bug-080`).
- L'IP est lue sans jamais déréférencer un client nul : le client peut avoir disparu entre l'arrivée du corps et la journalisation.

## Compensation T° du pH

`RT,<temp>` est envoyé **avant chaque `R`** sur l'EZO pH. Format : 1 décimale (ex `RT,25.3`). Source : `getWaterTemperature()` ([feature-020](../../specs/features/done/feature-020-deux-sondes-temperature.md), sonde DS18B20 identifiée comme « eau »).

L'EZO ORP n'a pas besoin de compensation T° : la mesure ORP est intrinsèquement température-dépendante mais cette dépendance n'est pas linéaire et Atlas n'expose pas de commande de compensation pour ce module.

### Ce qu'une température fausse coûte en pH (chiffrage de référence)

Méthode (ADR-0073 — **à refaire, pas à recopier**) : pente de Nernst `S(T) = 2,303·R·T/F` = 59,16 mV/pH à 25 °C, l'EZO calculant `pH = 7 + (E_iso − E)/S(T_comp)`. À potentiel d'électrode donné :

> `pH_lu − pH_vrai = (pH_vrai − 7) × (T_vrai − T_comp) / T_comp`  (T en **kelvins**)

Soit **≈ 0,7 millième de pH par °C** d'erreur de compensation. **Sens** : `T_comp` trop haute ⇒ pente surestimée ⇒ lecture **tirée vers 7** ⇒ pour un brut **sous** 7, pH lu **trop basique** ⇒ **léger sur-dosage** d'acide (symétrie inversée au-dessus de 7).

Conséquences, toutes chiffrées par la même formule :

| Cas | Erreur de pH |
|---|---|
| Sonde **figée** — 26,6 °C figé contre 25,0 °C réel, brut ≈ 6,80 (cas de production du 16/08) | **+0,0011** |
| Repli sur `kEzoFallbackTempC` = 25 °C | ≤ **±0,01** |
| Bornes de la fenêtre brute du DS18B20 `(−55 ; 125)` °C | 0,05 à 0,07 |
| Offset de calibration **non borné** de +1000 ⇒ `RT,1025.0` | ≈ **+0,15** |
| Offset très **négatif** ⇒ `T_comp` → 0 K | **non bornée** |

Les deux premières lignes sont **2 à 3 ordres de grandeur** sous le premier seuil de régulation (`kPhStopThreshold = 0,01`) : c'est le fondement de [ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md) — une sonde figée est **instrumentée**, jamais traitée. Les deux dernières sont le défaut réel corrigé par `bug-114`.

### ⛔ « Suspendre la compensation » n'existe pas

Ne pas envoyer `RT` ([`atlas_ezo.cpp:149-155`](../../src/atlas_ezo.cpp)) **ne suspend rien** : la lecture retombe sur `R` et le module **conserve sa dernière température interne**, silencieusement périmée, **invisible et indatable** côté firmware. C'est **pire que n'importe quel repli**. Il y a donc **toujours** une valeur envoyée, et l'étage qui l'a produite est **toujours** exposé — la cascade ne rend **jamais** `NaN` (vérifié par test).

### Cascade de résolution de `RT` (bug-114)

`_resolveEzoCompensationTemp()` ([`sensors.cpp`](../../src/sensors.cpp)) délègue la règle à la fonction pure `resolveCompensationTemp()` ([`temp_compensation_logic.h`](../../src/temp_compensation_logic.h), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) :

| Étage | Condition | Valeur envoyée | `TempCompStage` / libellé |
|---|---|---|---|
| 1 | mesure courante **plausible** (`getWaterTemperature()` ∈ `[0 ; 40]` °C, offset **borné** compris) | la mesure | `Current` / `mesure` |
| 2 | sinon, **dernière** valeur plausible d'âge ≤ `kSensorStaleTimeoutMs` = **20 s** (≈ 10 lectures DS18B20) | la dernière plausible | `LastPlausible` / `derniere_valeur_plausible` |
| 3 | sinon | `kEzoFallbackTempC` = **25,0 °C** | `Fallback` / `repli_defaut` |

**Journalisation** : `warning` à **chaque** substitution — immédiatement à la transition d'étage, puis **rappelé** toutes les `kEzoCompSubstitutionLogIntervalMs` = 60 s tant qu'elle dure (sans throttle, la réévaluation toutes les 5 s produirait 12 messages/min) ; `info` au retour sur la mesure ; `critical` **one-shot** si le repli à 25 °C dépasse `kEzoCompFallbackCriticalMs` = **10 min** (= 300 cycles de lecture manqués — au-delà, ce n'est plus un incident de lecture, c'est une panne).

Exemples :

- `warning` — `Compensation T° pH substituée (repli_defaut) : mesure NaN °C hors [0 ; 40] °C → RT,25.0`
- `critical` — `Compensation T° pH figée sur le repli 25.0 °C depuis plus de 10 min — sonde eau indisponible ou hors plage (erreur pH ≤ 0,01)`

⚠️ La borne haute **40 °C** est une borne de **PRUDENCE**, **NON CONFIRMÉE par la datasheet Atlas** : la plage réellement admise par `RT` n'a pas été vérifiée sur le document constructeur (recherche dans le dépôt le 2026-09-07, aucune trace — cette page ne documente que les **délais** de transaction). Et ⚠️ **ne pas resserrer sur 15-30 °C** : cela rejetterait des températures **vraies** (hors saison, canicule) et **dégraderait** la compensation au lieu de la protéger.

### Les QUATRE points de clamp de l'offset de calibration

`mqttCfg.tempCalibrationOffset` est borné à `[kTempCalOffsetMin ; kTempCalOffsetMax]` = `[−5 ; +5]` °C par `clampTempCalOffset()` (un `NaN` est neutralisé à 0). Motif de la valeur : un DS18B20 dérive de **dixièmes** de degré, pas de degrés. **Quatre** sites d'appel — compte relevé par `grep -rn clampTempCalOffset src/` le 2026-09-07 —, **aucun doublon** : chacun couvre une **provenance** que les autres ne voient pas.

| # | Site | Provenance couverte |
|---|---|---|
| **1/4** | `handleSaveConfig()` — [`web_routes_config.cpp`](../../src/web_routes_config.cpp) | saisie de l'opérateur (`POST /save-config`). Écrêtage **et non refus** (pas de canal d'erreur dédié dans ce flux) + `warning` nommant la valeur demandée, la valeur retenue et l'IP cliente |
| **2/4** | `loadMqttConfig()` — [`config.cpp`](../../src/config.cpp) | valeur **héritée** d'une version sans borne, jamais réenregistrée. Sans lui, `/get-config` exposerait une valeur que le firmware n'applique **pas** — et la règle du dépôt « toujours lire `/get-config` avant tout diagnostic chiffré » deviendrait un **piège** (un diagnostic chiffré a déjà déraillé sur ce motif le 2026-09-06). `warning` une seule fois, au boot |
| **3/4** | `_readDs18b20s()` — [`sensors.cpp`](../../src/sensors.cpp) | émission de la valeur de **mesure** : cache `tempValue` servi à l'affichage, à MQTT, à l'historique et au planning. **N'alimente PAS `RT`** |
| **4/4** | `getWaterTemperature()` — [`sensors.cpp`](../../src/sensors.cpp) | **émission `RT`** : le **seul** site qui alimente la commande de compensation partant sur le bus I²C |

⚠️ **Ne pas fusionner 3/4 et 4/4**, et n'en supprimer **aucun** comme doublon. La sentinelle de [`test_native_temp_compensation`](../../test/test_native_temp_compensation/test_temp_compensation.cpp) vise chaque point **dans sa fonction**, par **signature**, sur du code **dépouillé de ses commentaires** — donc indépendamment de toute renumérotation, et insensible à un commentaire qui citerait le nom de la fonction sans l'appeler. Les quatre suppressions ont été démontrées **détectées** (l'ancienne version de la sentinelle, satisfaite par un compte textuel, restait **verte** dans les quatre cas).

### Diagnostic exposé (feature-068)

| Accesseur ([`sensors.h`](../../src/sensors.h)) | Retour | Usage |
|---|---|---|
| `float getEzoCompensationTempC() const` | °C **effectivement** envoyés dans `RT` (offset borné + cascade). `NaN` **uniquement** avant la première lecture EZO du démarrage | `ezo_comp_t` du topic MQTT `<base>/diagnostic` |
| `TempCompStage getEzoCompensationStage() const` | `Current` / `LastPlausible` / `Fallback` | logique interne |
| `const char* getEzoCompensationStageLabel() const` | `"mesure"` / `"derniere_valeur_plausible"` / `"repli_defaut"` | `ezo_comp_stage` du topic diagnostic |
| `bool isEzoCompensationSubstituted() const` | `true` si l'étage ≠ `Current` | synthèse |

Lecture **lock-free** (un `float` et un `uint8_t` alignés ⇒ accès atomique sur Xtensa LX6) : appelables depuis `loopTask` comme depuis un handler async. Comparés à `temperature`, ces champs sont le **seul** diagnostic qui distingue une valeur **figée** (constante, étage `mesure`), un **repli** (étage ≠ `mesure`) et un **offset** mal réglé (écart constant avec `getWaterTemperatureRaw()`).

## Stale timeout (pool-chemistry condition #1)

`getPh()` / `getOrp()` retournent `NaN` si `millis() - _lastPhMs > kSensorStaleTimeoutMs = 20000 ms` (resp. `_lastOrpMs`).

Conséquences :
- `canDose(0)` / `canDose(1)` retournent `false` → dosage refusé fail-closed.
- Logger `critical` 1× à la **transition** vers stale (flag `_phStaleLogged` / `_orpStaleLogged`).
- Alerte MQTT `pool/alerts/sensor_stale` publiée edge-triggered (cf. [docs/MQTT.md](../MQTT.md)).

## Chaîne de mesure à 5 étages — feature-152

⚠️ **Refonte complète (v2.68.0, feature-152, [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md))** — cette
section **remplace entièrement** les descriptions `feature-025`/`feature-058`/`v2.20.3` qui
suivaient jusqu'ici (chaîne à 14 étages, rejet de spike, sortie de secours à trois gardes,
latch EMI, alarme « filtre non prêt durablement »). Toute cette ancienne architecture a
**disparu** — pas assouplie, **retirée**, après que le gate `pool-chemistry` n'ait trouvé
**aucun cas** où elle a démontrablement évité un dommage, et après deux incidents de
production où elle a au contraire **arrêté la régulation** sans raison (dosage pH et chlore à
l'arrêt ≥ 3 h, v2.20.3 ; sonde replongée après étalonnage verrouillée 141 rejets, 2026-10-11).
Historique archivé en fin de section pour qui doit retrouver le comportement d'un firmware
antérieur à la v2.68.0.

**Ligne directrice** : *le filtre lisse, il ne juge pas — c'est la couche de dosage
(`pump_controller`/`dosing_logic`) qui juge, avec des bornes qui comparent sans délibérer*
(butées de plausibilité, plafonds de dose). Voir [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md).

### Les 5 étages (mêmes qu'en température, qui n'a jamais posé de problème)

| # | Étage | Constante | Source (voir [ADR-0098](../adr/0098-toute-constante-de-mesure-declare-sa-source.md)) |
|---|---|---|---|
| 1 | Lecture du module EZO | — | matériel |
| 2 | Rejet de l'absurde : NaN / hors plage physique, comparaison **stricte** | `kPhFilterMin/Max` (0–14), `kOrpFilterMin/Max` (±1019,9 mV) | physique / matériel (plage de sortie EZO-ORP) |
| 3 | **Médiane** glissante (`kSensorFilterMedianWindow` = **9**) **puis** lissage exponentiel (`kSensorFilterEmaTauMs` = **45 000 ms**, α recalculé à chaque échantillon depuis le temps réellement écoulé) | `kSensorFilterMedianWindow`, `kSensorFilterEmaTauMs` | décision produit (dynamique du bassin — voir ci-dessous) |
| 4 | Offset utilisateur, **en sortie d'accesseur** | `ph_offset` | décision produit, [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md), **inchangé** |
| 5 | Fraîcheur : pas de mesure **acceptée** depuis `kSensorFilterMaxAgeMs` (20 s) ⇒ vidage de la fenêtre | `kSensorFilterMaxAgeMs` | matériel (couvre un échec I²C isolé) |

### 🔑 La médiane EST le rejet d'aberrations

Une médiane de N écarte **par construction** jusqu'à (N−1)/2 valeurs extrêmes, **sans aucun
seuil réglable sur une installation**. À N = 9 : 4 aberrations consécutives écartées, un
échelon légitime (sonde replongée après étalonnage, remise en eau) traverse la fenêtre en **5
échantillons**, quoi qu'il arrive. Il n'existe donc **aucune séquence d'entrées qui verrouille
ce module** — contrairement à l'ancienne chaîne (rejet de spike + sortie de secours), qui
pouvait se verrouiller indéfiniment sur une médiane périmée.

Largeur et constante de temps sont des **décisions produit** (verdict `pool-chemistry`,
2026-10-11), pas des mesures d'installation : retard total ≈ **67 s à 5 s de cadence**, soit
**1/13 de `kPhMixingDelayMs`** (900 s) — invisible pour la régulation. ⚠️ **La cadence de
lecture (5 s, `kPhOrpSensorIntervalMs`) n'est volontairement PAS une entrée de ce calcul** : la
largeur se compte en **échantillons**, le lissage se chronomètre en **millisecondes** — doubler
la cadence ne change ni l'un ni l'autre (test D3, `test/test_native_sensor_filter/`).

### Ce qui reste, et pourquoi

| Mécanisme | Rôle | Pourquoi il reste |
|---|---|---|
| Rejet NaN / hors plage physique (étage 2) | borne dure, comparaison stricte | gratuit, jamais faux |
| Médiane + lissage (étage 3) | lisse, **ne juge pas** | fait le travail de rejet sans seuil et sans verrou |
| **Détecteur de sonde figée** (`FrozenDetector`, feature-022) | sortie **immédiate** dès une lecture hors bande, **sans verrou** | seul mécanisme qui attrape une sonde morte rendant une constante plausible — **sourcé** : epsilon = ½ LSB du capteur (matériel), borne haute de N = décision produit |
| Fraîcheur (étage 5) | fait, pas jugement | pas de lecture récente ⇒ pas de valeur |
| Porte d'alimentation (filtration arrêtée / purge) | **simplifiée** : fermeture ⇒ vidage immédiat, ouverture ⇒ aucune action (la fenêtre se remplit seule) | plus de plancher de gel ni de ré-amorçage explicite à gérer |
| Butées de plausibilité pH, plafonds de dose | **bornes de dosage**, pas de mesure | couche de décision, hors de ce module |

### Ce qui a disparu (verdict `pool-chemistry` P1, feature-152, aucun contre-exemple trouvé)
Rejet de spike isolé (`spikeReject`) · sortie de secours de ré-amorçage et ses **trois gardes
cumulatives** (cooldown 30 min, bande admissible, cohérence de série) · dispersion de fenêtre et
son **latch EMI collant** (il se posait sur du **bruit normal**, dispersion 0,402 relevée pour
un cap à 0,5 — il n'inhibait pas un danger) · plancher de gel de la porte (60 s) · tolérance aux
trous de la sortie de secours · grâce de péremption post-ré-amorçage · alarme « filtre non prêt
durablement » (5 min / répétition 30 min) · état `unstable` et ses rejets consécutifs comptés.
**Les deux verrous collants du projet disparaissent avec ce lot.**

### Classe `SensorFilter` — API actuelle

Module pur, 1 instance par capteur (`_phFilter`, `_orpFilter` dans `SensorManager`), **zéro
allocation**, buffer médian fixe `float[kSensorFilterMedianWindowMax]`
([`src/sensor_filter.h`](../../src/sensor_filter.h)/[`.cpp`](../../src/sensor_filter.cpp),
[ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) :

| Méthode | Effet |
|---------|-------|
| `bool addSample(float raw, uint32_t nowMs)` | Soumet une brute. `true` si **acceptée** (NaN/hors-plage/porte fermée rejettent) |
| `void setContext(bool waterPresent, bool measuresValid, uint32_t nowMs)` | Porte d'alimentation — à appeler **avant** `addSample()` à chaque cycle. Fermée si `!waterPresent \|\| !measuresValid`. Fermeture ⇒ vidage immédiat ; ouverture ⇒ aucune action |
| `void reset()` | Vide la fenêtre, le détecteur de sonde figée. Appelé après calibration EZO réussie ou bouton debug/UI |
| `float raw() / median()` | Dernière brute / médiane courante — **diagnostic seul** |
| `float value(uint32_t nowMs)` | **VALEUR OFFICIELLE** — NaN si fenêtre non pleine (warmup), capteur figé, ou dernière mesure périmée. Jamais figée par un jugement d'instabilité : il n'y en a plus |
| `bool gateClosed() / frozen() / stale(nowMs) / warmingUp()` | Prédicats d'observabilité **lecture seule**, composés par `resolveFilterState()` (priorité pannes d'abord) pour qualifier la cause de refus `DoseRefusal` — voir [pump-controller.md](pump-controller.md#codes-de-refus-ph--devenus-sans-objet-retirés-feature-152) |
| `uint8_t rejectedCount()` | Compteur glissant de rejets (étage 2) — **diagnostic seul**, ne décide plus rien |

`Config` compte **7 champs** (contre 13 avant ce lot) : `minValue`, `maxValue`, `medianWindow`,
`emaTauMs`, `maxAgeMs`, `frozenSamples`, `frozenEpsilon` — aucun champ de réglage de rejet
(spike/dispersion/ré-amorçage), ces mécanismes n'existent plus.

### Une seule valeur officielle, fin du repli silencieux

Avant ce lot : `phRaw`, `ph`, `phMedian`, `phFiltered` coexistaient, plus `getPhFilteredRaw()`
pour les gardes — et `ws_manager.cpp`/`web_routes_data.cpp`/`mqtt_manager.cpp` retombaient
chacun sur le brut (ou la dernière mesure) si la filtrée était `NaN`, **sans le dire**. Pendant
la montée en régime, l'écran montrait 8,3 pendant que le dosage refusait sur une valeur absente
(défaut constaté le 2026-10-11). Depuis la v2.68.0 : **une seule** valeur officielle
(`getPhFiltered()`/`getOrpFiltered()`), consommée **à l'identique** par l'affichage,
l'historique, MQTT **et** le dosage ; `null`/`NaN` pendant la montée en régime, **sans
substitution**. `getPhRaw()`/`getOrpRaw()` restent du diagnostic.

`getPhFilteredRaw()` est **conservé** (verdict `pool-chemistry` P2, condition bloquante),
**renommé `getPhValueRaw()`** et reclassé « valeur officielle, échelle brute » : `pump_controller.cpp`
l'utilise pour les **butées de plausibilité pH**, qui travaillent volontairement sur l'échelle
brute pour rester valides si l'offset lui-même est faux (`pump_controller.h:312-314`) — le
supprimer ferait dépendre la seule butée du projet de l'offset qu'elle est censée contourner.

### Signal pH brut — anti-corrélation et mode commun (feature-097)

Fait mesuré sur le bassin de référence, distinct de la robustesse du filtre ci-dessus — et **le
motif pour lequel une médiane, pas une moyenne, est le bon estimateur** : le pH **brut** de ce
bassin n'est **pas** du bruit blanc autour d'une moyenne — c'est un signal **anti-corrélé,
presque à deux niveaux**. Trace du 2026-09-20 (`test/fixtures/f097_purge_noise_20260920.csv`,
300 échantillons à 5 s, 25 min, eau établie, porte ouverte) :

| Indice | Mesuré | Attendu si bruit IID |
|---|---|---|
| Autocorrélation au lag 1 | **−0,440** | 0 |
| Saut moyen entre 2 lectures consécutives | 0,173 pH = **1,47 σ** | 1,128 σ (≈ 0,133) |
| Changements de signe autour de la médiane | **197 / 299** | ~150 |

La corrélation instantanée pH↔ORP vaut **+0,462**, et l'ORP est lui aussi anti-corrélé
(**−0,396**) : les **deux électrodes voient la même perturbation** à l'instant même de la
lecture — signature d'un **mode commun** (courants de fuite de la pompe de filtration,
hypothèse déjà admise sur ce bassin), pas d'un bruit indépendant par voie.

⚠️ **Rappel de l'exigence de généricité (feature-152)** : cette trace **vérifie** que la
médiane tient sur du signal réel (fixture D5), elle n'a **jamais** servi à **choisir** la
largeur ou la constante de temps — celles-ci se déduisent de la dynamique du procédé
(homogénéisation en minutes/heures) et de la cadence matérielle, jamais d'un bruit mesuré sur
une installation. Toute **future statistique** calculée sur le brut doit être vérifiée sur ce
signal bimodal avant d'être calée sur une hypothèse de bruit gaussien ou uniforme — c'est le
piège qui a produit le défaut principal corrigé par ce lot (bande de cohérence à 0,20 contre un
bruit terrain à 0,36, inopérante par construction).

<details>
<summary>Archive — ancienne chaîne de filtrage (feature-025 à v2.67.x, RETIRÉE en v2.68.0)</summary>

> **Refonte feature-058 ([ADR-0027](../adr/0027-filtre-tendance-centrale.md), superseded par
> [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md))** — La chaîne d'origine (rejet de
> saut `maxStep` → médiane(7) → EMA + re-sync anti latch-up) **collait** au haut ou au bas d'une
> oscillation soutenue, et pouvait **figer** la valeur sur un nombre trompeur via le latch. Elle
> avait été remplacée par un estimateur à 14 étages (médiane longue + EMA + spike reject + sortie
> de secours à trois gardes + dispersion + latch EMI + alarme), lui-même **intégralement retiré**
> par feature-152 : aucun cas n'a été trouvé où ces mécanismes ont démontrablement évité un
> dommage, et ils ont causé deux arrêts de régulation en production (v2.20.3, 2026-10-11).
>
> Paramètres historiques (tous retirés, conservés ici pour mémoire) : fenêtre médiane 13,
> EMA α 0,10/0,08, spike dur 1,0 pH/300 mV, cap dispersion 0,5 pH/100 mV, 12 rejets cohérents
> avant ré-amorçage, cohérence 0,20 pH/40 mV, bande admissible 6,50–8,20/300–950 mV, cooldown
> 30 min, plancher de gel 60 s, alarme à 5 min/répétition 30 min. Décisions d'origine :
> [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)
> (superseded partiellement — le principe de porte survit, simplifié),
> [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) (en
> vigueur, amendé), [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md) (déjà
> superseded par l'ADR-0092, non concerné par ce lot).

</details>

## Offset logiciel de mesure pH — feature-067

Correction logicielle `pH corrigé = pH brut EZO + phOffset`, destinée à compenser un **écart d'implantation** de la sonde (position dans le circuit, potentiel électrique de l'eau). **La calibration interne du module Atlas EZO n'est JAMAIS touchée** : aucune commande `Cal,*` n'est émise, l'offset vit strictement **en aval** de la lecture.

- Configuration : `mqttCfg.phOffset` ([`config.h`](../../src/config.h)), persistée en NVS sous la clé `ph_offset`, défaut `0.0f`. Une valeur relue hors bornes est **neutralisée à 0,00** avec un `WARN` (fail-safe).
- Bornes : `kPhOffsetMin = -0.50f` / `kPhOffsetMax = 0.50f` ([`constants.h`](../../src/constants.h)).
- Logique **pure** isolée dans [`src/ph_offset.h`](../../src/ph_offset.h) — `applyPhOffset(value, offset)` (NaN in → NaN out) et `isPhOffsetValid(offset)`, header self-contained sans Arduino, testé en natif (suite `test_native_ph_offset`, pattern [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).

### Point d'application : EN SORTIE des accesseurs

Le filtre pH (`SensorFilter` : médiane + lissage exponentiel, feature-152) continue de
travailler sur le signal **brut** ; l'offset est ajouté **au moment de lire** la valeur.
Médiane et lissage étant **invariants par translation** (`filtre(x + c) == filtre(x) + c`,
propriété démontrée par un test natif sur le filtre réel), le résultat numérique est
identique — mais :

1. un changement d'offset est pris en compte **immédiatement**, au lieu d'être dilué sur toute la fenêtre de lissage (≈ 67 s à 5 s de cadence) ;
2. le **rejet de l'absurde** (étage 2, plage physique) reste calé sur le signal brut, donc inchangé ;
3. la **trace de diagnostic** conserve naturellement le brut.

Décision structurante et alternatives écartées : [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md).

> ⚠️ **Divergence assumée avec `tempCalibrationOffset`.** L'offset de température s'applique lui **à l'ingestion**, par **nécessité** : la valeur corrigée est envoyée à l'EZO pH (`RT,<temp>`) pour la compensation thermique — c'est la valeur corrigée qui doit partir sur le bus. Le pH, lui, n'est **réinjecté nulle part** : rien n'oblige à corriger tôt, et corriger tard apporte l'immédiateté. Le patron « offset température » n'est donc **pas transposable** au pH ; ce n'est pas une incohérence. **Depuis `bug-114`, cet offset est écrêté à ±5 °C aux quatre points de la chaîne** — voir [Les QUATRE points de clamp](#les-quatre-points-de-clamp-de-loffset-de-calibration).

### Contrat des accesseurs pH — règle mnémotechnique

> **Seuls les accesseurs suffixés `*Raw` renvoient du BRUT capteur.**

| Accesseur | Valeur | Offset | NaN quand |
|---|---|---|---|
| `getPh()` | dernière mesure **corrigée** | ✅ | brut NaN ou lecture stale (> `kSensorStaleTimeoutMs`) |
| `getPhMedian()` | médiane **corrigée** | ✅ | fenêtre médiane vide |
| `getPhFiltered()` | VALEUR OFFICIELLE **corrigée** — entrée du PID, de l'affichage, de l'historique et de MQTT | ✅ | warmup / sonde figée / périmée |
| `getPhRaw()` | dernière mesure **brute EZO** | ❌ | brut NaN ou lecture stale |
| `getPhValueRaw()` | VALEUR OFFICIELLE **brute** — filets de sécurité chimique (ex-`getPhFilteredRaw`, renommé feature-152, **conservé**) | ❌ | **filtre non prêt** (voir garde d'âge) |
| `getPhOffset()` | offset **effectivement appliqué** (copie locale) | — | jamais (toujours dans les bornes) |

⚠️ `getPhRaw()` était historiquement un **alias pur** de `getPh()`. Depuis la v2.20.0 les deux divergent dès que l'offset est non nul : `getPhRaw()` est la source unique de la fenêtre stale, `getPh()` s'appuie dessus et y ajoute l'offset. Tout code qui utilisait `getPhRaw()` en pensant « la même chose que `getPh()` » doit être relu.

Côté ORP, rien ne change : `getOrpRaw()` reste un alias de `getOrp()` et aucun offset n'existe.

### Garde d'âge de `getPhValueRaw()` — pourquoi elle existe

`getPhValueRaw()` retourne `_phFilter.value(millis())` à l'échelle brute (sans offset) — la
valeur officielle est déjà `NaN` tant que la fenêtre n'est pas pleine, que la sonde est figée,
ou que la dernière mesure acceptée est périmée (feature-152, étages 3/5) :

```cpp
float SensorManager::getPhValueRaw() const {
  return _phFilter.value(millis());  // échelle brute, pas d'offset
}
```

Raison (exigence `pool-chemistry`) : la valeur officielle **n'expire jamais silencieusement**
— c'est l'étage 5 (fraîcheur) qui porte la péremption, intégré à `value()` elle-même depuis
feature-152 (auparavant porté séparément par `ready()`). Sans cette propriété, une valeur
**périmée** (EZO muet depuis plusieurs minutes) resterait lisible et pourrait :

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
| Filets de sécurité chimique (planchers pH brut, inhibition ORP) | **brute** (`getPhValueRaw()`) |
| `/data`, WebSocket `sensor_data`, MQTT `{base}/ph` | **corrigée** (valeur officielle, `null`/NaN pendant la montée en régime depuis v2.68.0 — **plus de repli**, voir feature-152) |
| MQTT `{base}/ph_raw`, test de vivacité `phStale` de l'alerte `sensor_stale` | **brute** |
| [`history`](history.md) (`/history.json`) | **corrigée** (`getPh()`) — ⚠️ discontinuité à l'activation, cf. CHANGELOG 2.20.0 |
| Health check `ph_abnormal` ([`main.cpp`](../../src/main.cpp)) | détection sur la **corrigée**, message citant **toujours** brut + offset |
| Trace `/debug/sensor_trace` | `phRaw` et `phFiltered` **bruts**, `phCorrected` = brut + offset |
| Écran de calibration (UI) | **brut** exclusivement ([page-ph.md](../features/page-ph.md#lécran-de-calibration-reste-sur-le-brut)) |

## Horodatage de justesse — feature-156, v2.72.0

Un étalonnage EZO modifie le zéro **et** la pente de la sonde : une correction manuelle
(`phOffset`) posée **avant** le dernier étalonnage réussi compensait un état de la sonde qui
**n'existe plus** — elle est **caduque**. Le firmware le détecte en comparant deux événements
indépendants, chacun porteur d'un `{epoch, seq}`.

### Les quatre champs persistés (`MqttConfig`, [`config.h`](../../src/config.h))

| Champ | Type | NVS | Rôle |
|---|---|---|---|
| `phOffsetEpoch` | `uint32_t` | `ph_offset_ep` | `time(nullptr)` à la pose de la correction. `0` = date inconnue (horloge non synchronisée). **Affichage seul.** |
| `phOffsetSeq` | `uint32_t` | `ph_offset_sq` | Séquence de la correction, posée par `nextJustnessSeq()`. **Fait foi pour l'ordre.** |
| `phCalEpoch` | `uint32_t` | `ph_cal_ep` | `time(nullptr)` au dernier étalonnage réussi (ou effacement). `0` = date inconnue. **Affichage seul.** |
| `phCalSeq` | `uint32_t` | `ph_cal_sq` | Séquence du dernier étalonnage, posée par `nextJustnessSeq()`. **Fait foi pour l'ordre.** |

Défauts `0/0/0/0` = « aucun événement connu » ⇒ **rétrocompatible** : une installation existante
mise à jour sans ces clés NVS ne déclenche jamais un faux verdict de caducité au premier
démarrage (voir [ADR-0102](../adr/0102-ordre-des-gestes-de-justesse-par-compteur.md)).

### Deux écrivains uniques, et eux seuls

- **`setPhOffsetStamped(float offset, const char* source)`** ([`config.cpp`](../../src/config.cpp))
  — mute `phOffset`/`phOffsetEpoch`/`phOffsetSeq` **en RAM seulement**, l'appelant persiste
  (`saveMqttConfig()`). Appelée avec `configMutex` **déjà détenu par l'appelant** (ne le
  reprend pas) depuis les deux routes qui écrivent `ph_offset` :
  [`handleTunePhOffset`](../API.md#post-tune-ph-offset--critical) et la branche `ph_offset` de
  [`/save-config`](../API.md#post-save-config--critical).
- **`markPhCalibrationStamped()`** ([`config.cpp`](../../src/config.cpp)) — mute
  `phCalEpoch`/`phCalSeq` **et persiste dans la même transaction** (prend `configMutex`
  elle-même, mutex récursif). Appelée depuis `loopTask` (hors section critique), **jamais**
  depuis une route HTTP directement. Sur timeout : `error` journalisé, **aucune mutation** —
  ne jamais laisser une séquence posée sans sa persistance.

⚠️ **Ne jamais écrire ces quatre champs ailleurs**, y compris dans `loadMqttConfig()` : la
migration d'arrondi du pas y réécrit déjà `phOffset` — estampiller à cet endroit rajeunirait
une correction périmée et effacerait silencieusement le verdict.

### Les trois points d'estampillage de l'étalonnage (`sensors.cpp`, `_executeEzoCmd`)

`markPhCalibrationStamped()` est appelée après chacune des **trois** commandes EZO qui changent
le zéro ou la pente de la sonde :

| Commande | Point d'appel |
|---|---|
| `CalibratePhMid` (`Cal,mid,7.00`) | après succès, juste après `enqueuePhSlopeQuery()` |
| `CalibratePhLow` (`Cal,low,4.00`) | après succès, juste après `enqueuePhSlopeQuery()` |
| `ClearPhCal` (`Cal,clear`) | après succès, juste après `resetPhFilter()` |

⚠️ **`ClearPhCal` estampille aussi**, contre-visite du gate `pool-chemistry` (feature-156) : le
lot n'estampillait au départ que `CalibratePhMid`/`Low`, au motif qu'« un effacement est
toujours suivi d'un étalonnage ». Refusé — c'est une **hypothèse**, pas une garantie, et après
un `Cal,clear` le zéro et la pente **ont changé** (l'EZO repart sur ses valeurs d'usine) : la
correction est caduque au sens chimique, exactement comme après un étalonnage. Un effacement
seul laisserait sinon la sonde sans points **et** la correction périmée **non signalée**.
Estampiller ici ne peut rendre le verdict que **plus conservateur**, jamais moins.

### Logique pure et verdict servi

`src/ph_offset.h` (testé en natif, suite `test_native_ph_offset`, section D4) :

- **`isPhOffsetStale(float offset, uint32_t offsetSeq, uint32_t calSeq)`** — `true` ssi
  `offset != 0` **et** `offsetSeq < calSeq`. Deux séquences égales (y compris `0 == 0` au
  premier démarrage) ne sont **pas** une préséance de l'étalonnage — `false`.
- **`nextJustnessSeq(uint32_t a, uint32_t b)`** — `max(a, b) + 1`, monotone quel que soit
  l'ordre des arguments.

Le verdict (`ph_offset_stale`) est **calculé par le firmware** et servi par `GET /get-config` et
la trame WS `config` — **jamais recalculé côté client** (ADR-0100, [ADR-0102](../adr/0102-ordre-des-gestes-de-justesse-par-compteur.md)).
Les deux séquences internes (`phOffsetSeq`/`phCalSeq`) **ne sont exposées nulle part** : seuls
le verdict et les deux epochs (affichage) le sont. Voir [API.md](../API.md#get-get-config--write)
et [page-ph.md](../features/page-ph.md#correction-devenue-caduque-après-un-étalonnage-ph_tune_stale_banner-feature-156-v2720).

## Trace des signaux capteurs — feature-057

Ring buffer de trace **haute cadence** des signaux capteurs (pH & ORP, brut, **médiane** et lissé), réintroduction fidèle de l'outil retiré en feature-045 (v2.5.0), **étendue à l'ORP** (feature-057) puis à la **médiane** (feature-153, v2.69.0). Objectif : diagnostiquer bruit / oscillation de la chaîne de mesure en comparant les trois étages sur une fenêtre continue, à cadence bien supérieure à l'échantillonnage WS. Alimente la card « Signaux capteurs » de l'onglet Avancé ([page-settings.md](../features/page-settings.md#card-signaux-capteurs-feature-153-v2690)) et les routes `GET /debug/sensor_trace` / `POST /debug/sensor_trace_clear` ([API.md](../API.md#trace-des-signaux-capteurs-feature-057-v270)).

### Structure et coût mémoire

```cpp
struct SensorTraceSample {
  uint32_t ms;          // millis() à l'enregistrement
  float    phRaw;       // pH brut Atlas EZO, SANS offset (NaN si lecture échouée)
  float    phCorrected; // feature-067 : pH brut + offset logiciel (NaN si brut NaN)
  float    phMedian;    // feature-153 : médiane BRUTE du filtre pH (_phFilter.median(), NaN si fenêtre vide)
  float    phFiltered;  // pH lissé (médiane + EMA), SANS offset (NaN si filtre vide)
  float    orpRaw;      // ORP brut (NaN si lecture échouée)
  float    orpMedian;   // feature-153 : médiane BRUTE du filtre ORP (_orpFilter.median(), NaN si fenêtre vide)
  float    orpFiltered; // ORP lissé (NaN si filtre vide)
  float    tempEau;     // T° eau envoyée à l'EZO pour compensation (NaN si sonde absente)
};
// sizeof(SensorTraceSample) == 36 octets — figé par un static_assert dans sensors.h
// (le padding du compilateur peut dépasser sizeof(float) × N, ne pas le recalculer à la main).
SensorTraceSample _traceBuffer[kSensorTraceBufferSize];  // 200 × 36 o = 7 200 o RAM (mesuré le 2026-10-11)
```

> **feature-067 — la trace est le seul endroit où `phFiltered` est BRUT.** Objectif de l'outil : pouvoir dire à tout instant **ce qui vient de l'EZO** et **ce qui vient de la correction**. Les champs pH sont donc `phRaw` (brut), `phMedian` (médiane brute, feature-153), `phFiltered` (brut filtré) et `phCorrected` (brut + offset). Partout ailleurs (`/data`, WS, MQTT), `phFiltered` est **corrigé**.

> **feature-153 (v2.69.0) — l'offset est ajouté par le NAVIGATEUR, pas par le firmware.** `phMedian`/`orpMedian` sont enregistrés depuis `_phFilter.median()`/`_orpFilter.median()` — jamais `getPhMedian()`, qui applique l'offset ([ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Les trois champs pH de la trace (`phRaw`, `phMedian`, `phFiltered`) partagent donc la **même référence brute** ; seul `phCorrected` porte l'offset. La carte « Signaux capteurs » ([page-settings.md](../features/page-settings.md#card-signaux-capteurs-feature-153-v2690)) ajoute l'offset à l'affichage pour que la Haute cadence, la vue Journée et le tableau de bord restent sur une seule échelle.

- `kSensorTraceBufferSize = 200` depuis la v2.69.0 (feature-153 ; était 300) ([`constants.h`](../../src/constants.h)) → **7 200 o** RAM statique (200 × 36 o), **sous** les 8 400 o d'avant malgré les deux champs de plus — décision produit chiffrée de l'utilisateur du 2026-10-11 : le gain d'information (médiane) est payé en réduisant la durée du tampon, pas en grossissant son occupation. La fenêtre est dimensionnée en **nombre d'échantillons** (diagnostic pur) ; sa durée réelle — **16 min 40 s** à la cadence de lecture actuelle (5 s) — **se déduit de la cadence**, elle n'est volontairement pas figée ici (ADR-0073 : une durée affichée en dur aurait menti dès que la cadence change).
- **Volatil** : perdu au reboot (comme l'original — pas de persistance, hors périmètre).
- Les NaN sont **enregistrés tels quels** (filtre non amorcé, EZO injoignable) et exposés en `null` côté JSON → trou dans la courbe, pas de crash.

### Enregistrement

`_recordSensorTrace(phRaw, phFiltered, orpRaw, orpFiltered, tempEau)` est appelée à la **fin de `_readEzoSensors()`** (donc une fois par cycle capteur, `kPhOrpSensorIntervalMs` ≈ 5 s). L'enregistrement **n'ajoute aucune transaction I²C** : il consomme les valeurs déjà en cache. Watchdog non impacté.

### API publique associée

| Méthode | Effet |
|---------|-------|
| `size_t getSensorTraceCount() const` | Nombre d'échantillons présents (0..200). |
| `void appendSensorTraceJson(JsonArray out, size_t offset, size_t count) const` | Sérialise une **page** du buffer (ordre chronologique, `offset` 0 = le plus ancien) : les échantillons `[offset, offset+count)` — `t` (ms), `phRaw`/`phCorrected`/`phMedian`/`phFiltered` (arrondi ×1000), `orpRaw`/`orpMedian`/`orpFiltered` (×10), `tempEau` (×10) ; `null` si NaN. Signature paginée depuis v2.10.1 (bug-crash-sensor-trace-badalloc) ; champ `phCorrected` ajouté en v2.20.0, `phMedian`/`orpMedian` ajoutés en v2.69.0 (feature-153, +34,7 o/échantillon mesurés par réinjection — page de 38 = **5 775 o mesurés** au 2026-10-11 sur firmware déployé). |
| `void clearSensorTrace()` | Vide le buffer (`_traceCount = 0`). |

### Pagination — bug-crash-sensor-trace-badalloc (v2.10.1)

La route `GET /debug/sensor_trace` sérialisait les 300 échantillons (taille d'alors) dans un seul `AsyncResponseStream` (~20 Ko contigus dans son `cbuf`) → `std::bad_alloc` non rattrapé → `abort()` → reboot sur heap fragmenté (coredump v2.10.0, 2026-07-23). Elle est désormais **paginée** (`?offset=N&count=M`, page bornée à `kSensorTraceMaxSamplesPerPage = 38` depuis la v2.69.0 — était 50 ; mesure réelle le 2026-10-11 **avant** feature-153 : 5 905 o pour 50 échantillons, pas les ≈ 4,5 Ko/3,5 Ko longtemps recopiés sans mesure, ADR-0073) avec **garde heap** (`measureJson` + `kSensorTraceHeapMarginBytes = 4096` vs `heapLargestFreeBlock8Bit()` depuis la **v2.40.0** — `ESP.getMaxAllocHeap()` avant, mesure qui comptait l'IRAM 32 bits inutilisable par ce `new` : voir [oom-guard.md](oom-guard.md) et [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) ; **seuil inchangé** → `503` propre) et stream **pré-dimensionné** (une seule allocation exacte, zéro `resize()`).

Le clamp des bornes de page est un calcul **pur header-only** dans [`src/sensor_trace_page.h`](../../src/sensor_trace_page.h) (`computeTracePage()`, invariants `offset ≤ total`, `n ≤ max`, `offset + n ≤ total` pour tout input — testé en natif, suite `test_native_trace_page`, 16 tests, pattern [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)). `appendSensorTraceJson()` applique en plus un **re-clamp défensif interne** sur son snapshot local de `_traceCount` (le buffer peut avoir glissé entre le clamp de la route et la lecture). Contrat complet de la route : [API.md](../API.md#get-debugsensor_trace--pas-dauth-cohérent-avec-autres-debug).

### Contrat de concurrence — lock-free

Même modèle que `_lastPh`/`_lastOrp` : **un seul producteur** (`loopTask` core 1 via `_recordSensorTrace()`), **un seul type de lecteur** (handler AsyncTCP core 0 via `appendSensorTraceJson()` / `getSensorTraceCount()`). Aucun mutex : l'écriture d'un échantillon puis l'incrément de `_traceCount` sont des écritures atomiques 32 bits sur Xtensa LX6 ; le lecteur ne voit au pire qu'un échantillon de moins (jamais de valeur déchirée). La pagination (v2.10.1) **n'introduit aucun verrou** : le glissement du ring entre deux pages (~1 échantillon max) est toléré (route de diagnostic). Handler HTTP non bloquant (< 50 ms), réponse pré-dimensionnée (`beginResponseStream(type, taille)`) sans allocation dans la loop.

## Détection capteur figé — feature-022

Un module EZO peut répondre sans erreur I²C mais retourner indéfiniment la **même valeur** (électronique interne figée, sonde HS côté BNC). La panne franche (NaN / stale / bus dégradé) est couverte ailleurs ; ici on détecte la **variance nulle** : N lectures **acceptées** consécutives contenues dans une bande < epsilon → capteur figé.

### Classe pure `FrozenDetector`

Définie dans [`src/sensor_filter.h`](../../src/sensor_filter.h) (logique pure, testée en natif — pattern [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)). Algorithme « **run ancré** » en **O(1)** mémoire et CPU — pas de fenêtre glissante stockée :

- Le run courant est décrit par 3 champs : `runMin` / `runMax` (bornes observées) et `runCount` (longueur, saturé à `UINT16_MAX`).
- Chaque échantillon qui garde `(newMax − newMin) < epsilon` **étend les bornes** et allonge le run.
- Dès qu'une lecture sort de la bande, le run est **ré-ancré sur cette lecture** (`runMin = runMax = x`, `runCount = 1`) → **sortie immédiate de l'état figé, sans re-warmup**.
- `frozen()` = `runCount >= samples` ; `samples = 0` désactive la détection (compat init positionnelle courte de `SensorFilter::Config` dans les tests).

| Instance | Alimentée par | N (`samples`) | epsilon | Effet si figé |
|---|---|---:|---:|---|
| `_phFilter` (composé dans `SensorFilter`) | lectures brutes **acceptées** par le filtre pH | `kSensorFrozenSamples` = 30 (≈ 2,5 min à 5 s) | `kSensorFrozenEpsilonPh` = 0.0005 (½ LSB EZO pH, résolution 0.001) | `ready()` = false → **dosage pH bloqué fail-closed** |
| `_orpFilter` (idem) | lectures brutes acceptées par le filtre ORP | 30 | `kSensorFrozenEpsilonOrp` = 0.05 mV (½ LSB EZO ORP, résolution 0.1) | dosage ORP bloqué fail-closed |
| `_waterTempFrozen` (dédié, dans `SensorManager`) | lectures DS18B20 **valides** de la sonde « eau » (valeur **brute**, non arrondie au 0.1 °C) | `kTempFrozenSamples` = **43 200** (≈ **24 h** à 2 s, bug-142 — était 900 / 30 min) | `kTempFrozenEpsilonC` = 0.03 °C (< ½ LSB DS18B20 12 bits = 0.0625/2, **inerte** sous ce seuil, voir justification ci-dessous) | **warning-only pour le DOSAGE uniquement** — aucun chemin vers `pump_controller`, erreur de pH induite ≈ 0,001 ([ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md)). ⚠️ Le drapeau **gate néanmoins le planning de filtration** : `filtration.cpp:162` bloque tout recalcul pendant l'épisode, `:405` (`isSampleEligible`) rejette les échantillons de T° (bug-142). Depuis feature-068, l'état est aussi **exposé** (`/data`, WS, MQTT, badge UI) |

### Fenêtre de 24 h — justification physique (bug-142)

Le DS18B20 fonctionne en **12 bits** : son pas de quantification est **0,0625 °C**
(`g_ds18b20ResolutionBits = 12`). La bande du détecteur température,
`kTempFrozenEpsilonC = 0,03 °C`, est **plus étroite qu'un pas** — elle ne peut
donc contenir qu'**une seule valeur possible**. Le détecteur ne dit pas « la
mesure varie peu », il dit « N lectures sont tombées exactement sur le même
seizième de degré ».

Mesuré sur l'appareil le 2026-10-07 (journal système + `/get-history?range=24h`) :
la dérive de température la plus rapide observée est de **0,05 °C/h**, soit
**1 h 15** pour franchir un seul pas de quantification. La nuit, à l'équilibre
thermique, la dérive tend vers zéro et le pas peut tenir **indéfiniment**. Une
fenêtre de 30 min — deux fois et demie plus courte que le temps minimal pour
franchir un pas au meilleur moment de la journée — déclenchait donc le
détecteur **tous les jours** sur une sonde parfaitement saine.

**12 h a été envisagé puis écarté sur données** : le 2026-10-07, le bassin est
resté affiché à 20,5 °C de 08:00 à 13:00 locales, soit **six heures** — une
fenêtre de 12 h aurait pu être avalée par une nuit calme suivie d'une matinée
calme. **24 h** est le seul seuil adossé à une garantie physique plutôt qu'à
une marge choisie à vue : sur un cycle jour/nuit complet, un bassin extérieur
franchit plusieurs pas de quantification (six le 2026-10-07). Une sonde qui
rend la même valeur sur 24 h complètes n'a plus de scénario sain — c'est
réellement une panne.

`kTempFrozenEpsilonC` reste **inerte** sous 0,0625 °C : `0.03` et `0.001`
produiraient rigoureusement le même comportement, puisqu'un seul pas tient de
toute façon dans la bande. Le seul levier réel de ce détecteur est la
**fenêtre** (`kTempFrozenSamples`), jamais l'epsilon.

⚠️ **Plafond `uint16_t`** : `_runCount` (`sensor_filter.h`) sature à
`UINT16_MAX`, ce qui plafonne la fenêtre exploitable à **~36 h** (64 800
lectures à 2 s). Au-delà, le seuil n'est **jamais** atteint et le détecteur ne
devient pas plus strict : il **s'éteint en silence**. `kTempFrozenSamples =
43 200` ne garde qu'une marge de **×1,5** — écrire `86400` (48 h) pour « être
prudent » replierait silencieusement la constante modulo 2¹⁶ à **20 864**
(11 h 35), cassant la détection au lieu de la renforcer. La garde réelle
n'est donc pas une borne haute mais une borne **basse** sur la durée dérivée :
`static_assert(kTempFrozenWindowSec >= 20 * 3600)` dans `constants.h` —
si la durée tombe sous ~20 h, c'est que la constante a été repliée ou mal
réglée.

**Cas du redémarrage** : `_runCount` repart de zéro à chaque boot. Un appareil
qui redémarre plus souvent que toutes les 24 h ne détectera **jamais** une
sonde figée par ce mécanisme. Accepté (aucun effet dosage, et le projet vise
justement à réduire les redémarrages) — mais c'est une limite réelle, à
connaître avant de se fier à ce détecteur comme seul filet de sécurité
matériel.

### Choix d'epsilon = ½ LSB (validation pool-chemistry)

Un capteur **vivant** bruite d'au moins ±1 LSB (quantification + bruit sonde + bruit thermique). Avec epsilon = ½ LSB, un simple toggle de 1 LSB — même en arithmétique float32 — **casse le run** : aucune eau réelle, même parfaitement stable, ne peut être déclarée figée tant que la dernière décimale bouge.

> **Consigne terrain** : en cas de faux positifs, **durcir N** (allonger la fenêtre), ne **JAMAIS élargir epsilon** — un epsilon plus large capturerait le bruit LSB d'un capteur vivant et transformerait la stabilité réelle de l'eau en faux positif.

### Intégration fail-closed dans `SensorFilter`

- Le détecteur est alimenté **uniquement par les bruts ACCEPTÉS** dans `addSample()` : les rejets (NaN / hors plage / saut) ne l'alimentent jamais → l'état figé **persiste pendant une rafale de rejets** (pas d'échappatoire par le bruit EMI).
- `frozen()` ⇒ la valeur officielle (`value()`) devient NaN ⇒ garde `ReadingNaN` existante de `evaluateDose()` (feature-152) — **aucune modification de l'ordre des gardes verrouillé**, pas de nouvelle valeur d'enum `DoseRefusal`.
- `SensorFilter::reset()` (calibration réussie, reset manuel) réinitialise aussi le détecteur.
- La sortie de l'état figé est **immédiate** (une seule lecture vivante suffit) et **sans re-warmup** : la valeur figée était par définition stable, le filtre est resté cohérent.

### Observabilité

- **Logs edge-triggered `[SENSOR_FROZEN]`** (`_checkFrozenAndLog()`, une seule ligne par transition) : `critical` pH/ORP (mentionne « régulation auto inhibée (valeur officielle NaN) » — c'est CE log qui porte le diagnostic, la garde générique `ReadingNaN` ne dit pas la cause), `warning` température, `info` à la levée.
- **MQTT** : alerte `{base}/alerts/sensor_frozen` (JSON retain, clear payload vide, pH/ORP uniquement) + états binaires `{base}/ph|orp_sensor_problem` (`ON` = stale OU figé) avec discovery HA `binary_sensor` `device_class: problem` — voir [MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-feature-022-problème-capteur-phorp-v2100).
- **Getters lock-free** : `isPhSensorFrozen()` / `isOrpSensorFrozen()` / `isTemperatureFrozen()` (simple comparaison d'entiers 16 bits, même contrat de concurrence que les autres getters).

#### Exposition de la sonde de température figée — feature-068

Ajoutée **sans changer aucun comportement** ([ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md)) :

> **bug-142 (v2.56.1)** a porté la fenêtre de détection de 30 min à 24 h (voir
> [la justification physique ci-dessus](#fenêtre-de-24-h--justification-physique-bug-142))
> sans toucher à ce tableau ni au contrat des champs — seule la **fréquence**
> de passage à `ON` / `true` change, de quotidienne à quasi nulle sur une
> sonde saine.

| Canal | Détail |
|---|---|
| `GET /data` + trame WS `sensor_data` | `tempFrozenAlarm` (bool), **parité stricte**, valeur **instantanée** — miroir direct de `isTemperatureFrozen()` |
| MQTT | `<base>/temp_sensor_frozen` (`ON`/`OFF`, retain) + `binary_sensor` HA `device_class: problem`. **Garde « aucune sonde eau »** : jamais `ON` si `getWaterTemperatureRaw()` est `NaN` (rôle « eau » non identifié **ou** dernière lecture invalide) — cela couvre du même coup la sonde **débranchée en cours d'épisode**, qui retombe à `OFF` au lieu de rester « figée » indéfiniment. **Maintien 60 s** (`kMqttTempFrozenHoldMs`) sur **ce seul chemin de publication** |
| UI | badge « Sonde température figée » (variant `warning`) sur la carte du tableau de bord et la page Température |

**La sortie de l'état figé est immédiate, sans hystérésis** côté firmware — le drapeau est informatif, un battement ne coûte rien chimiquement, et une hystérésis ajouterait un second seuil à régler. L'anti-battement de 60 s existe **uniquement** à la publication MQTT (une entité HA qui clignote est inutilisable en automatisation) : `/data`, la trame WS et l'UI voient l'état instantané.

⚠️ La température **n'entre pas** dans `alerts/sensor_frozen` ni dans `*_sensor_problem` : la sémantique établie de ces canaux est « le dosage est compromis ». Motif complet dans [MQTT.md](../MQTT.md#pourquoi-la-température-nentre-pas-dans-alertssensor_frozen) et [ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md).

## Constantes clés

| Constante | Valeur | Usage |
|-----------|--------|-------|
| `kEzoPhAddress` | `0x63` | Adresse I²C EZO pH (défaut Atlas) |
| `kEzoOrpAddress` | `0x62` | Adresse I²C EZO ORP (défaut Atlas) |
| `kEzoReadDelayMs` | `900` ms | Délai après commande `R` (lecture) |
| `kEzoCalDelayMs` | `900` ms | Délai après commande `Cal,*` |
| `kCalibrationAuthWarnThrottleMs` | `60000` ms | 1 WARN/min/route maximum sur les tentatives de calibration non authentifiées (v2.46.1) |
| `kEzoRtDelayMs` | `600` ms | Délai après commande `RT,<temp>` |
| `kTempCalOffsetMin` / `kTempCalOffsetMax` | `-5.0` / `+5.0` °C | Bornes de `tempCalibrationOffset` (bug-114) — un DS18B20 dérive de **dixièmes** de degré |
| `kEzoCompTempMinC` / `kEzoCompTempMaxC` | `0.0` / `40.0` °C | Fenêtre de plausibilité de la température envoyée dans `RT`. ⚠️ Borne haute **non confirmée par la datasheet Atlas** |
| `kEzoFallbackTempC` | `25.0` °C | Étage 3 de la cascade (dernier recours) — coût maximal **±0,01 pH** |
| `kEzoCompFallbackCriticalMs` | `600000` ms (10 min) | Durée au-delà de laquelle le repli au défaut passe en `critical` (= 300 lectures manquées) |
| `kEzoCompSubstitutionLogIntervalMs` | `60000` ms | Rappel `warning` d'une substitution **durable** (sans lui : 12 messages/min) |
| `kSensorStaleTimeoutMs` | `20000` ms | Timeout pH/ORP stale (cond #1 pool-chemistry) |
| `kEzoBusFailMaxConsecutive` | `2` | Échecs I²C consécutifs → cache cal_points = -1 + lecture = NaN (cond #5) |
| `kPhOrpSensorIntervalMs` | `5000` ms | Période lecture pH/ORP |
| `kTempSensorIntervalMs` | `2000` ms | Période lecture DS18B20 |
| `kI2cMutexTimeoutMs` | `2000` ms | Timeout d'acquisition mutex I²C |
| `kPhSlopeQueryIntervalMs` | `86_400_000` ms (24 h) | Re-query auto `Slope,?` (feature-024) |
| `kPhSlopeBootRetryIntervalMs` | `30_000` ms | Intervalle entre retentatives `Slope,?` de boot (v2.21.2) |
| `kPhSlopeBootRetryMaxAttempts` | `5` | Borne DURE de retentatives de boot, jamais réarmée (gate C1 pool-chemistry) |
| `kSensorFilterMedianWindow` / `kSensorFilterMedianWindowMax` | `9` / `9` | Largeur de la médiane (étage 3) et capacité physique du buffer FIXE — décision produit, feature-152, retard ≈ 67 s à 5 s de cadence |
| `kSensorFilterEmaTauMs` | `45000` ms | Constante de temps du lissage exponentiel (étage 3) — décision produit, dynamique du bassin, feature-152 |
| `kPhFilterMin/Max` | `0` / `14` pH | Plage physique plausible (étage 2) |
| `kOrpFilterMin/Max` | `-1019,9` / `+1019,9` mV | Plage matérielle plausible (étage 2) — plage de sortie du module EZO-ORP, comparaison **stricte** : la saturation exacte du module est rejetée, pas lissée (feature-152) |
| `kSensorFilterMaxAgeMs` | `20000` ms | Étage 5 — âge max d'une mesure acceptée avant vidage de la fenêtre |
| `kSensorFrozenSamples` | `30` | Lectures acceptées consécutives dans la bande → figé (≈ 2,5 min à 5 s — feature-022, borne haute de N = décision produit, feature-152) |
| `kSensorFrozenEpsilonPh` | `0.0005` pH | Bande figé pH = ½ LSB EZO (résolution 0.001) |
| `kSensorFrozenEpsilonOrp` | `0.05` mV | Bande figé ORP = ½ LSB EZO (résolution 0.1) |
| `kTempFrozenSamples` / `kTempFrozenEpsilonC` | `43200` / `0.03` °C | Figé T° eau (≈ **24 h** à 2 s, < ½ LSB DS18B20, bug-142 — était `900` / 30 min) — warning-only pour le DOSAGE, gate le planning de filtration |
| `kPhOffsetMin` / `kPhOffsetMax` | `-0.50` / `+0.50` pH | Bornes de l'offset logiciel pH (feature-067) — validation serveur, refus 400 hors plage |
| `kSensorTraceBufferSize` | `200` | Taille ring buffer trace signaux — 7 200 o RAM (200 × 36 o), depuis feature-153 v2.69.0 (était 300 × 28 o ≈ 8,4 Ko) |
| `kSensorTraceMaxSamplesPerPage` | `38` | Borne d'une page de `GET /debug/sensor_trace` (**5 775 o JSON mesurés** le 2026-10-11 sur firmware déployé — v2.69.0, était 50 = 5 905 o mesurés avant ce lot) |
| `kSensorTraceHeapMarginBytes` | `4096` | Marge entre `measureJson` et `heapLargestFreeBlock8Bit()` → 503 au lieu de `bad_alloc` (v2.10.1 ; mesure basculée depuis `ESP.getMaxAllocHeap()` en **v2.40.0**, valeur du seuil **inchangée**) |

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

- [`pump_controller`](pump-controller.md) : le PID consomme **uniquement** `getPhFiltered()` / `getOrpFiltered()` (VALEUR OFFICIELLE, **corrigée** côté pH) et teste `isnan()` dans `canDose(int)` (feature-152 : une seule garde, plus de `isPhFilterReady()`/`isPhFilterUnstable()`). Les **filets de sécurité chimique** feature-067 consomment `getPhValueRaw()` (brut, avec garde d'âge, ex-`getPhFilteredRaw`) et `getPhOffset()`. `getPh()`/`getOrp()` restent disponibles pour les logs et l'affichage brut. Vérifie aussi `getPhCalibrationPointsCached()` / `getOrpCalibrationPointsCached()` (conditions #1, #2, #5).
- [`ws_manager`](ws-manager.md) : `broadcastSensorData()` publie `ph`/`orp` (= VALEUR OFFICIELLE, **plus de repli silencieux** depuis v2.68.0 — `null` pendant la montée en régime), `temperature`, `temperature_circuit`, `phCalPoints`, `orpCalPoints`, `phSlopeAcid/Base/Zero/AgeMs` toutes les 5 s. Champs de diagnostic : `ph/orpRaw|Median|Filtered`, `ph/orpRejectedCount`, `ph/orpMixingDelayActive`, `ph/orpDoseBlockedReason`/`Code` (parité `/data` ↔ WS). ⚠️ `ph/orpFilterReady`, `ph/orpFilterUnstable` et `ph/orpFilterAlarm` sont **retirés en v2.68.0** (feature-152).
- [`mqtt_manager`](mqtt-manager.md) : publie `{base}/ph` / `{base}/orp` (= **filtré**), `{base}/temperature`, `{base}/temperature_circuit`, `{base}/ph_cal_points`, `{base}/orp_cal_points` toutes les 10 s (`kMqttPublishIntervalMs`). 3 topics `{base}/ph_slope_*` edge-triggered. feature-025 : topics `{base}/ph|orp_raw|median|filtered|filter_ready|filter_unstable|rejected_count` + `{base}/ph|orp_mixing_delay_active` (retain). Alertes edge-triggered `{base}/alerts/calibration_required`, `{base}/alerts/sensor_stale` et `{base}/alerts/sensor_frozen` + états binaires dédupliqués `{base}/ph|orp_sensor_problem` (feature-022).
- [`history`](history.md) : snapshot des valeurs courantes toutes les 5 min.

## Indicateur de stabilité de calibration (bug-160)

⚠️ **Ce mécanisme vit côté frontend** (`data/app.js`, objet `CAL_STAB` et fonctions
`pushStabilitySample()` / `_calStabilityVerdict()`), pas dans `sensors.cpp` — documenté ici parce
qu'il décide **quand la mesure brute d'une sonde pH/ORP est jugée assez stable pour être
calibrée**, au même titre que les autres verdicts de cette page. Voir
[`docs/features/page-ph.md`](../features/page-ph.md#les-six-états-de-lindicateur-de-stabilité-renderstability-bug-160)
pour le rendu UI.

### Le défaut corrigé

Avant ce lot, le verdict reposait sur une **étendue** (`max − min` d'une fenêtre glissante de
20 s, seuil 0,9 pH). Une étendue croît avec le nombre d'échantillons et **mélange bruit et
dérive** : sur une rampe réaliste d'électrode (τ = 30-60 s après le changement de solution
étalon), le seuil se déclenchait à **0,8 à 1,1 pH de l'équilibre réel** — treize fois le bruit de
lecture invoqué pour le justifier.

### Le nouveau critère — un estimateur de vitesse de dérive, pas une étendue

Le tampon par produit (`_calStab.ph` / `.orp`, `{t, v}` triés par `t` croissant, horloge
**monotone** `performance.now()` — jamais `Date.now()`, qu'un recalage NTP ferait exploser ou
vider dans les deux sens) est coupé en **deux moitiés par comptage** (`floor(n/2)`, l'échantillon
médian écarté si `n` est impair) :

```
D = |médiane(moitié récente, valeurs) − médiane(moitié ancienne, valeurs)|   // dérive
S = médiane(moitié récente, temps) − médiane(moitié ancienne, temps)        // séparation RÉALISÉE
a = nowMs − médiane(moitié récente, temps)                                  // âge RÉALISÉ
G = e^(a/τmax) · (e^(S/τmax) − 1)                                           // garde-fou
R = D / G                                                                    // résiduel garanti
```

**Verdict rendu seulement si `n ≥ MIN_SAMPLES` (= 6) ET `G ≥ G_MIN`** ; sinon **muet** (icône ⏳,
jamais un faux « stable » par manque de données). `stable` ⟺ `D ≤ SEUIL`.

`G_MIN = SEUIL / RÉSIDUEL_MAX` — **aucun degré de liberté** : si un vecteur de test échoue, ce
n'est **jamais** le seuil qu'il faut assouplir (⛔ interdit par [ADR-0098](../adr/0098-toute-constante-de-mesure-declare-sa-source.md)),
c'est `G ≥ G_MIN` qui est violée — la seule issue légitime est d'allonger la fenêtre ou la
cadence, toutes deux **plus strictes**, jamais l'inverse.

**Pourquoi les deux gardes (comptage ET `G`) sont imposées ensemble** : une coupure au comptage
**sans** `G` laisserait passer une **rafale** (beaucoup d'échantillons en peu de temps, donc une
« ancienne » et une « récente » moitié séparées de quelques secondes seulement — aucune garantie
que l'électrode a eu le temps de dériver ou non). Une coupure au temps **sans** le plancher de
comptage laisserait passer une **cadence mixte**. Les deux tuent des mutants différents.

### Table des sources (ADR-0098) — recontrôler, pas croire

| Constante | Valeur | Source déclarée | Rôle |
|---|---|---|---|
| `RESIDUAL_MAX` (pH) | **0,10 pH** | produit — `kPhHighMargin` | résiduel garanti maximal toléré |
| `THRESHOLD` (pH) | **0,05 pH** | produit — `kPhGuardReleaseHysteresis` | seuil de `stable` sur `D` |
| `THRESHOLD` (ORP) | **0,05 × 59,16 mV/pH** ≈ 2,96 mV | physique — pente de Nernst à 25 °C (⚠️ toujours écrite comme expression, jamais le littéral `3.0` : un littéral serait précisément le mouvement qu'ADR-0098 interdit) | seuil de `stable` sur `D` |
| `RESIDUAL_MAX` (ORP) | **0,10 × 59,16 mV/pH** ≈ 5,92 mV | physique | résiduel garanti maximal toléré |
| `TAU_MAX_MS` (pH) | **120 000 ms** | matériel — Atlas EZO pH, majoration de conception **non relevée sur datasheet** (⛔ ne jamais écrire « matériel » sans cette réserve — une source étiquetée matériel sans l'être est une garantie plus forte que ce qu'on sait réellement) | borne de l'exponentielle `G` |
| `TAU_MAX_MS` (ORP) | **180 000 ms** | matériel, même réserve — plus courte que la pose normale d'une sonde redox | idem |
| `G_MIN` (les deux, **par produit**, jamais partagé) | `THRESHOLD / RESIDUAL_MAX` = **0,5** | dérivé, aucun degré de liberté | garde-fou d'admissibilité du verdict |
| `WINDOW_MS` | `TAU_MAX_MS − SLOW_CADENCE_MS` | dérivé | réserve de rétention |
| `MIN_SAMPLES` | **6** | produit, source unique (ne vit plus en deux littéraux indépendants) | plancher de comptage avant tout verdict |

⚠️ **Direction fail-safe de `TAU_MAX_MS`** : `WINDOW_MS` en dérive, et **surestimer** `TAU_MAX_MS`
**agrandit** la fenêtre, donc **durcit** le critère — l'erreur ne peut jamais produire un faux
« stable ». C'est la **baisse** qui serait dangereuse : toute baisse de cette valeur exige un
relevé fabricant, une hausse est toujours sûre.

Résiduels garantis obtenus avec ces valeurs (vérifiés par `test/cal_stability_vectors.js`) : pH
**0,0911** à τ = 120 s · ORP **4,65 mV** à τ = 180 s — tous deux sous leur `RESIDUAL_MAX` respectif.

### Rétention — robuste à la cadence (5 s avec WebSocket, 30 s sans)

Deux purges distinctes du tampon, à chaque échantillon poussé :
- **Purge souple** (`WINDOW_MS`) : retire les échantillons plus vieux que la fenêtre, **sauf**
  s'il resterait moins de `MIN_SAMPLES` — une cadence dégradée (30 s, WebSocket coupé) doit
  pouvoir accumuler au-delà de la fenêtre nominale le temps d'atteindre le plancher de comptage,
  sinon : état **muet permanent**, le pire résultat possible.
- **Plafond d'âge dur** (`HARD_AGE_MS = WINDOW_MS × HARD_AGE_FACTOR`, facteur 4, décision produit
  déclarée arbitraire mais large) : jamais un échantillon plus vieux que ce plafond, quel que soit
  le nombre restant — protège contre une fuite de pertinence si une session reste ouverte des
  heures.

Le tampon est en outre borné en **nombre** (`CAL_STAB_MAX_SAMPLES = 256`), indépendamment de l'âge.

### Diagnostic après 5 minutes — un constat, jamais un verdict

Si le verdict reste différent de `stable` **5 minutes** après le premier échantillon du tampon
courant (`CAL_STAB_DIAG_MS`, dérivé de `kStabilizationDurationPhMs` — **le même délai pour les
deux produits**, décision du gate : la durée ORP post-cal, 180 s, serait plus courte que la pose
normale d'une sonde redox et donc inadaptée ici), l'écran bascule sur un texte de diagnostic (voir
[page-ph.md](../features/page-ph.md#diagnostic-après-5-minutes)) plutôt que de répéter « ça
bouge encore ». ⚠️ **Ce n'est ni un succès ni un échec** : le mécanisme a statistiquement
**raison** sur une sonde dégradée qui ne redescendra jamais sous le seuil — ce qui change, c'est
la **nature du message** (un diagnostic, pas une consigne d'attente), jamais le critère lui-même,
et **le bouton « Calibrer » reste cliquable** dans tous les cas (voir ADR-0105, point 1).

### Distinction avec ADR-0092 — ce n'est PAS un détecteur de stabilité de porte de mesure

Voir [ADR-0105](../adr/0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md)
pour la doctrine complète. En bref : contrairement au mécanisme retiré par
[ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md), celui-ci (1) ne bloque jamais
rien, (2) mesure un transitoire dont la valeur vraie est déjà connue (le tampon d'étalonnage), pas
une représentativité permanente, (3) est un estimateur de vitesse de dérive et non une étendue,
(4) a des constantes sourcées et un résiduel garanti par construction.

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
- [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — chaîne à 5 étages, médiane + lissage exponentiel (feature-152, v2.68.0 ; 393 lignes au total, contre 959 avant)
- [`test/test_native_sensor_filter/`](../../test/test_native_sensor_filter/) — tests natifs du module pur, dont le test central de non-verrouillage (D2, feature-152)
- [`src/ph_offset.h`](../../src/ph_offset.h) — logique pure de l'offset pH `applyPhOffset()` / `isPhOffsetValid()` (feature-067, testé en natif `test/test_native_ph_offset/`)
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — routes refondues `/calibrate_ph`, `/calibrate_orp`, `/calibrate_clear`
- [bug-save-config-applique-avant-authentification](../../specs/features/done/bug-save-config-applique-avant-authentification.md) — audit des handlers de corps, garde d'authentification des trois routes de calibration (v2.46.1)
- [`src/web_routes_debug.cpp`](../../src/web_routes_debug.cpp) — routes `/debug/sensor_trace` (GET, paginée v2.10.1) + `/debug/sensor_trace_clear` (POST) (feature-057)
- [`src/sensor_trace_page.h`](../../src/sensor_trace_page.h) — clamp pur header-only `computeTracePage()` (pagination v2.10.1, testé en natif `test/test_native_trace_page/`)
- [`src/web_routes_sensor_id.cpp`](../../src/web_routes_sensor_id.cpp) — routes feature-020 inchangées
- [`src/constants.h`](../../src/constants.h) — toutes les constantes EZO + DS18B20
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001)
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0013](../adr/0013-identification-sondes-onewire.md) — identification 2 sondes DS18B20
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025)
- [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) — offset pH appliqué en sortie des accesseurs (feature-067)
- [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md) — refonte de la chaîne de mesure à 5 étages, retrait des verrous collants (feature-152, supersède l'[ADR-0027](../adr/0027-filtre-tendance-centrale.md), amende l'[ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) et l'[ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md))
- [ADR-0098](../adr/0098-toute-constante-de-mesure-declare-sa-source.md) — règle de conception opposable : toute constante de mesure déclare sa source (physique, décision produit, ou matériel)
- [`data/app.js`](../../data/app.js) — objet `CAL_STAB`, `pushStabilitySample()`, `_calStabilityVerdict()` (bug-160, indicateur de stabilité de calibration, voir section dédiée ci-dessus)
- [`test/cal_stability_vectors.js`](../../test/cal_stability_vectors.js) — vecteurs qui épinglent `G_MIN` et le résiduel garanti (ADR-0099 : extraction, pas duplication)
- [ADR-0105](../adr/0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md) — distinction avec ADR-0092 (bug-160, volet 1)
- [feature-021](../../specs/features/done/feature-021-migration-atlas-ezo.md) — spec d'origine
- [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) — filtrage mesures + adaptation régulation
- feature-022 (`specs/features/`) — détection capteur figé `FrozenDetector` + observabilité HA (v2.10.0)
