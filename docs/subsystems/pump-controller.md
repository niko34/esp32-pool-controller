# Subsystem — `pump_controller`

- **Fichiers** : [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
- **Singleton** : `extern PumpControllerClass PumpController;`
- **Responsabilité** : régulation PID pH/ORP + anti-cycling + cumuls journaliers + mode manuel + stabilisation filtration → pompes doseuses.

## Rôle

Pilote les deux pompes doseuses (PWM sur MOSFET IRLZ44N) selon le mode sélectionné (`automatic` / `scheduled` / `manual`), les valeurs cibles, l'état de la filtration, les limites de sécurité et les cumuls journaliers. C'est le composant le plus critique côté chimie — **toute modification passe obligatoirement par l'agent `pool-chemistry`**.

## Mapping pompes ↔ pins (PCB v2)

Convention figée par [feature-019](../../specs/features/done/feature-019-gpio-pcb-v2.md) et [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) :

| Index | Sortie pin | Constante | Rôle chimique |
|-------|-----------|-----------|---------------|
| `pumps[0]` | GPIO 25 | `kPumpPhPin` ([`constants.h`](../../src/constants.h)) | Pompe doseuse **pH** (acide ou base selon `phCorrectionType`) |
| `pumps[1]` | GPIO 33 | `kPumpOrpPin` ([`constants.h`](../../src/constants.h)) | Pompe doseuse **ORP/chlore** |

L'affectation `pumps[0] = pH` / `pumps[1] = ORP` est posée dans `PumpController.begin()` ([`pump_controller.cpp:22`](../../src/pump_controller.cpp:22)) et ne doit pas être inversée — un module externe (test manuel via `setManualPump(int pumpIndex, ...)`) qui passerait l'index inverse activerait la mauvaise pompe (risque chimique). Le PWM est généré via `ledc` (channels `PUMP1_CHANNEL` / `PUMP2_CHANNEL` définis dans [`config.h`](../../src/config.h)).

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

// Stabilisation par pompe (feature-021) — durée différenciée pH / ORP
void armStabilizationTimer(int pumpIndex);     // 0 = pH (5 min), 1 = ORP (3 min)
void armStabilizationTimer();                   // surcharge legacy : arme les 2 pompes
                                                // avec mqttCfg.stabilizationDelayMin
bool isStabilizationTimerActive(int pumpIndex) const;
void clearStabilizationTimer();
unsigned long getStabilizationRemainingS() const;              // max des 2 pompes (UI WS)
unsigned long getStabilizationRemainingS(int pumpIndex) const; // par pompe (feature-006)

// Gate fail-closed (feature-021)
bool canDose(int pumpIndex);                    // 0 = pH, 1 = ORP

// Compteurs exposés aux gardes d'injection manuelle (feature-006) — lecture
// SNAPSHOT sans mutex depuis les handlers web async (écriture loopTask only,
// lectures 32 bits atomiques), pending manuel inclus
unsigned int getCyclesToday(int pumpIndex) const;             // cycles jour auto + manuel
int getRecentCycles(int pumpIndex, uint32_t windowMs) const;  // ring anti-rafale partagé
void requestManualCycleRecord(int pumpIndex);                 // enregistrement atomique différé

// Crédit plancher fin d'injection manuelle volumée (v2.9.1) — loopTask only,
// appelé par updateManualInject() à la fin NATURELLE uniquement
void creditManualInjectionFloor(int idx, float startCumulMl, float creditMl);

// Pause mélange hydraulique post-injection (feature-025)
void notifyPhDose(uint32_t nowMs);              // arme la pause à l'arrêt d'une injection pH
void notifyOrpDose(uint32_t nowMs);
bool isPhMixingDelayActive(uint32_t nowMs) const;
bool isOrpMixingDelayActive(uint32_t nowMs) const;

// Raison de blocage du dosage (feature-025) — "" si autorisé
String getPhDoseBlockedReason() const;          // dernière cause de refus canDose(0)
String getOrpDoseBlockedReason() const;         // dernière cause de refus canDose(1)

void setManualPump(int pumpIndex, uint8_t duty);  // test manuel
```

Voir [`pump_controller.h`](../../src/pump_controller.h).

## Boucle d'exécution

`PumpController.update()` est invoquée depuis [`main.cpp:181`](../../src/main.cpp:181) à chaque tour de `loop()`. Le `loop()` se termine par `delay(kLoopDelayMs)` (= 10 ms, [`constants.h:10`](../../src/constants.h:10)) → fréquence pratique ~100 Hz, mais les capteurs ont leur propre throttling interne (pH/ORP toutes les 5 s, DS18B20 toutes les 2 s).

> **feature-025** : l'entrée de la régulation est la mesure **filtrée** `sensors.getPhFiltered()` / `getOrpFiltered()` (médiane + EMA), **jamais** la brute. `getPh()` / `getOrp()` (brut) restent utilisés pour les logs de diagnostic uniquement.

Ordre par cycle :
1. Consommation des resets atomiques (`_resetRequested`, `_phPauseResetRequested`) — évite les races inter-core (web handler vs loop).
2. Chargement différé des cumuls NVS : `_dailyLoaded` reste à `false` tant que NTP/RTC n'a pas synchronisé une date valide (sinon on chargerait sur une date 1970 fantôme).
3. **`tickDailyRollover()`** — bascule date / reset des compteurs journaliers. Appelé **avant** `canDose()` pour que le passage à minuit soit honoré même si la filtration est arrêtée. Voir [Reset journalier](#reset-journalier) ci-dessous.
4. **Court-circuit OTA** : `otaInProgress` actif → `applyPumpDuty(0,0)` + `applyPumpDuty(1,0)` puis `return`. Pompes coupées tant que l'OTA dure.
5. Refresh des fenêtres glissantes 1 h (`refreshDosingState`).
6. Court-circuit capteurs : `!sensors.isInitialized()` → arrêt pompes.
7. Gate `canDose()` (cf. ci-dessous) → arrêt pompes si non autorisé (mais respecte `manualMode[i]` pour ne pas couper un test développeur en cours).
8. Pour pH puis ORP : calcul de l'erreur, anti-cycling start/stop, PID, conversion duty via `flowToDuty()`, `applyPumpDuty()`.

## Algorithme (résumé)

1. **Gate `canDose(int pumpIndex)`** — fail-closed strict (feature-021, validé pool-chemistry). Voir [Garde-fous `canDose`](#garde-fous-candose) ci-dessous pour la liste complète des 10 conditions évaluées dans l'ordre.
2. **Calcul de l'erreur** ([`pump_controller.cpp:432`](../../src/pump_controller.cpp:432)) — deux modes exclusifs sélectionnés via `mqttCfg.phCorrectionType` :

   | Mode | Formule erreur | Direction du dosage |
   |------|----------------|---------------------|
   | `ph_minus` (acide, défaut) | `error = pH_mesuré − phTarget` | dose si `error > 0` (pH trop haut) |
   | `ph_plus` (base) | `error = phTarget − pH_mesuré` | dose si `error > 0` (pH trop bas) |
   | ORP | `error = orpTarget − ORP_mesuré` | dose chlore si `error > 0` (ORP trop bas) |

3. **Régulation P temporisée** (feature-025, [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md)) : `computePID(pid, error, now, deadband, freezeIntegral)`. La stratégie par défaut est **proportionnelle pure temporisée**, pas un PID complet :

   | Grandeur | Kp | Ki | Kd |
   |----------|----|----|----|
   | pH (défaut struct) | 8 | 0 | 0 |
   | ORP (override constructeur) | 0.3 | 0 | 0 |

   - **`Kd = 0` IMPÉRATIF** — un terme dérivé amplifierait le bruit résiduel après filtrage (incompatible avec un système hydraulique lent).
   - **`Ki = 0`** — l'intégrale est inerte (l'`integralMax = 50` reste codé pour une réactivation terrain future, mais sans effet tant que Ki = 0). Anti-windup strict de toute façon (cf. ci-dessous).
   - La temporisation vient de la **pause mélange** (point 6) et de l'anti-cycling : on dose un pulse court, puis on attend l'homogénéisation avant de réévaluer.
   - Normalisation d'erreur (bornée) : `kPhMaxError = 1.0`, `kOrpMaxError = 200 mV`. `dt` PID plafonné : deltas > 10 s ignorés.

   **Anti-windup** : `computePID()` reçoit `freezeIntegral` — l'intégrale est **gelée** (aucune accumulation) si l'un des cas suivants est vrai : filtre non prêt, `canDose() == false`, pause mélange active, sortie PID saturée, erreur dans la zone morte, ou mesure rejetée / capteur instable.

   > Le **cœur du calcul** (terme proportionnel, anti-windup, plancher 0, bornage final min/max) est extrait dans la fonction pure `computePidPure` testée en natif. `computePID` n'est plus qu'une coquille qui gère `dt`/`millis()` et l'état PID. Voir [Calcul proportionnel pur (`computePidPure`)](#calcul-proportionnel-pur-computepidpure).

   **Zone morte** : le paramètre `deadband` passé à `computePID()` est le **seuil de démarrage existant** (`phStartThreshold` / `orpStartThreshold`) — **un seul** deadband fait foi, aucun second seuil dupliqué. Si `|error| < deadband` → sortie forcée à 0 **et** intégrale figée.
4. **Anti-cycling** ([`config.h:152`](../../src/config.h:152) `PumpProtection`) — **en dur, non configurables via UI** :
   - `minInjectionTimeMs = 30000` — 30 s minimum par injection démarrée.
   - Hystérésis de seuils (démarrage / arrêt) :

     | Grandeur | Deadband | Seuil démarrage (= zone morte feature-025) | Seuil arrêt |
     |----------|----------|-----------------|-------------|
     | pH | `PH_DEADBAND = 0.01` | `phStartThreshold = 0.05` | `phStopThreshold = 0.01` |
     | ORP | `ORP_DEADBAND = 2.0 mV` | `orpStartThreshold = 15 mV` (relevé de 10, feature-025) | `orpStopThreshold = 2 mV` |

   > feature-025 : `phStartThreshold` / `orpStartThreshold` servent désormais aussi de **zone morte PID** (passés comme `deadband` à `computePID`). ORP relevé de 10 → 15 mV pour élargir la zone morte (l'ORP étant plus bruité et dépendant du pH/T°/stabilisant).

   - `maxCyclesPerDay = 20` — démarrages comptés dans une fenêtre 24 h glissante.
5. **Limites de sécurité** ([`config.h:141`](../../src/config.h:141) `SafetyLimits`) :
   - Horaire : `ph_limit_minutes` / `orp_limit_minutes` dans une fenêtre glissante de 1 h (`windowStart` / `usedMs` dans `DosingState`). **Non reflétée dans l'UI** (pas de badge dédié à date).
   - Journalière : `maxPhMlPerDay = 300`, `maxChlorineMlPerDay = 500`.
6. **Cumul journalier persisté** : `dailyPhInjectedMl` / `dailyOrpInjectedMl`, persistés en NVS, reset à minuit local (détection via `currentDayDate[9]`). Voir [Reset journalier](#reset-journalier) et [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md).

## Reset journalier

`PumpControllerClass::tickDailyRollover()` ([`pump_controller.cpp:251`](../../src/pump_controller.cpp:251)) gère seul la bascule de date et la remise à zéro des compteurs `dailyPhInjectedMl` / `dailyOrpInjectedMl` + flags `phLimitReached` / `orpLimitReached`. Cette méthode est **appelée depuis `update()` avant le check `canDose()`** : la réinitialisation à minuit se produit donc **indépendamment de l'état de la filtration**.

> Bug historique corrigé : la logique de reset était auparavant dans `checkSafetyLimits()`, qui n'est invoqué qu'après `canDose()`. Tant que la filtration n'avait pas tourné dans la journée, les compteurs restaient figés sur la valeur de la veille. `checkSafetyLimits()` ne fait désormais plus que la vérification des seuils journaliers.

Trois branches dans `tickDailyRollover()` :

1. **NTP/RTC synchronisé + date connue + jour différent** → reset complet, `saveDailyCounters()`, `armStabilizationTimer()` (mitigation double quota).
2. **NTP/RTC synchronisé + `currentDayDate` vide** (première initialisation après boot) → date stockée, `dayStartTimestamp` remis à `0` pour invalider tout timer fallback `millis()` accumulé depuis le boot. Évite un double reset si NTP retombe en panne plus tard.
3. **Pas de temps valide** → fallback `millis()` : reset après 24 h écoulées depuis `dayStartTimestamp`. Persiste les compteurs (`saveDailyCounters()`) et arme le timer de stabilisation.

## Garde-fous `canDose`

`canDose(int pumpIndex)` (feature-021, validé pool-chemistry — voir [ADR-0014](../adr/0014-migration-atlas-ezo.md)) est appelée à chaque cycle `update()` pour la pompe pH (index 0) puis ORP (index 1), **avant** tout calcul PID ou démarrage d'injection. La fonction est **fail-closed strict** : tout résultat ambigu retourne `false` (refus de dosage).

Les conditions sont évaluées dans l'ordre suivant. Le premier `false` rencontré court-circuite les suivants et logue la cause **edge-triggered** (1 entrée info par transition de cause, pas de spam). La dernière cause de refus est **exposée au WS** via `getPhDoseBlockedReason()` / `getOrpDoseBlockedReason()` (chaîne vide si autorisé).

| # | Condition | Refus si | Cause documentée |
|---|-----------|----------|------------------|
| 1 | **Index pompe valide** | `pumpIndex` ∉ {0, 1} | bug interne |
| 2 | **Watchdog actif** | wdt non initialisé | sécurité hardware |
| 3 | **Présence d'eau** | `filtration.resolveWaterPresence().waterPresent == false` | eau ne circule pas — risque bouchon doseuse / injection en eau stagnante. **Source unique** selon le mode d'installation (feature-056, voir [Présence d'eau selon le mode d'installation](#présence-deau-selon-le-mode-dinstallation-feature-056)) |
| 4 | **Lecture FILTRÉE non NaN** | `getPhFiltered()` ou `getOrpFiltered()` = NaN | feature-025 : le PID consomme le filtré ; NaN = warmup non amorcé / stale |
| 4b | **Filtre prêt** (feature-025) | `!isPhFilterReady()` / `!isOrpFilterReady()` | warmup en cours, mesure trop ancienne (> 20 s), ou EZO injoignable — **fail-closed** |
| 4c | **Capteur stable** (feature-025) | `isPhFilterUnstable()` / `isOrpFilterUnstable()` | ≥ 10 rejets consécutifs (EMI persistant) → on bloque plutôt que de lisser |
| 5 | **EZO calibré** | `cal_points < 2` (pH) ou `< 1` (ORP). `-1` (bus down) bloque. | pool-chemistry **cond #2** : régulation auto inhibée tant que calibration incomplète |
| 6 | **Pas de stabilisation post-cal** | `isStabilizationTimerActive(pumpIndex)` = true | pool-chemistry **cond #3** : 5 min pH / 3 min ORP après calibration EZO |
| 6b | **Pas de pause mélange** (feature-025) | `isPhMixingDelayActive()` / `isOrpMixingDelayActive()` | homogénéisation du bassin après injection (15 min pH / 20 min ORP) — gate **indépendante** de la stabilisation post-cal |
| 7 | **Mode régulation = `automatic`** | mode `manual` ou `scheduled` | la branche `scheduled` a sa propre logique, ne passe pas par `canDose` |
| 8 | **Limite journalière non atteinte** | `dailyXxxInjectedMl >= maxXxxMlPerDay` | sécurité chimique config |
| 9 | **Limite horaire non atteinte** | `usedMs >= xxxLimitMinutes × 60000` dans la fenêtre 1 h glissante (limite à 0 = désactivée) | sécurité chimique config |
| 10 | **Anti-rafale court terme** (Pass 3.5) | > 6 cycles/min OU > 20 cycles/15 min | anti-emballement PID — voir [Ring buffer anti-rafale](#ring-buffer-anti-rafale) |

> **Conditions #1, #2, #3, #5, #6** issues du checklist `pool-chemistry` validé en cadrage feature-021. **Condition #10** ajoutée en correctif Pass 3.5. **Conditions #4 (filtré), #4b, #4c, #6b** ajoutées en feature-025 — toutes **fail-closed** et **prioritaires** : elles s'ajoutent aux gardes existantes sans les contourner.

## Présence d'eau selon le mode d'installation (feature-056)

La garde #3 (« Présence d'eau ») ne teste plus `filtration.isRunning()` directement : elle consomme la **source unique** `filtration.resolveWaterPresence().waterPresent` ([ADR-0026](../adr/0026-mode-installation.md)). La même source alimente **tous** les chemins de dosage (régulation auto, injection manuelle `evaluateManualInject`, monitor d'injection en cours) — plus de logique de présence d'eau dupliquée par site.

Cette valeur est résolue par la fonction **pure** `resolveWaterPresent(WaterPresenceInputs)` ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp), pattern Humble Object [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) selon le mode d'installation `InstallMode` (remplace `regulationMode` + `filtrationCfg.enabled`) — **fail-closed strict** :

| Mode | Sérialisé | `waterPresent` | Ex-équivalent |
|------|-----------|----------------|---------------|
| `ManagedFiltration` | `managed` | `filtration.isRunning()` (état commandé) | `pilote` + `enabled=true` |
| `PoweredByFiltration` | `powered` | `true` (eau présumée présente 24/7) | `continu` |
| `ExternalFiltration` | `external` | signal externe `ON` **connu ET récent** (< `kExternalFiltrationStaleMs` = 180 s), sinon `false` (fail-safe OFF au boot et à l'expiration) | *nouveau* |

**L'ordre des gardes est inchangé** ; seule la source du booléen change. La garde d'injection manuelle reproduit la même condition (`in.waterPresent = filtration.resolveWaterPresence().waterPresent`, [`web_routes_control.cpp:66`](../../src/web_routes_control.cpp:66)) — l'ancienne « exemption mode `continu` » est désormais absorbée par la résolution `powered` (toujours `true`). L'arrêt cyclique d'une injection en cours (`updateManualInject()`) s'appuie aussi sur cette source : en `external`, un signal `OFF` ou périmé pendant une injection volumée déclenche l'interruption CRITICAL.

## Décision de dosage : logique pure (`src/dosing_logic`) vs coquille hardware

[feature-036](../../specs/features/done/feature-036-dosage-testable-decision-pure.md) ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) applique le pattern **Humble Object** à la décision « peut-on doser ? ». La décision est extraite dans un module **pur** ([`src/dosing_logic.h`](../../src/dosing_logic.h) / [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)) **sans `<Arduino.h>`, sans FreeRTOS, sans I²C, sans `String`** — donc **compilable et testable en natif** (sur PC). C'est un *characterization refactor* : **aucun verdict, aucune cause de refus, aucun seuil ni ordre d'évaluation n'a changé**.

### Frontière

| | Logique pure (`dosing_logic`) | Coquille hardware (`pump_controller.cpp`) |
|---|---|---|
| **Entrées** | `struct DoseInputs` (POD : `bool` / `int` / `float` / `unsigned long`) | collecte les globals : `sensors`, `filtration`, `mqttCfg`, `safetyLimits`, `millis()`, `esp_task_wdt_status()`, ring buffer anti-rafale |
| **Sortie** | `struct DoseDecision { bool allowed; DoseRefusal cause; }` (énum pur) | mappe `DoseRefusal` → **String FR** (cause au mot près), réinjecte les valeurs runtime (`cal=X requis=Y`, `mode=X`, compteurs…) |
| **Effets de bord** | aucun | `logRefusalOnce()` edge-triggered, exposition WS via `getPhDoseBlockedReason()` / `getOrpDoseBlockedReason()`, log `warning(...)` |
| **Temps** | injecté en paramètre (`runTimeMs`, `usedMs`) | fourni depuis `millis()` |

### Fonctions pures

- **`DoseDecision evaluateDose(const DoseInputs& in)`** — reproduit **exactement** l'ordre des gardes 2→15 de `canDose()` (la garde 1 « index pompe invalide » reste dans la coquille). Première garde en échec → cause correspondante ; sinon `{ true, None }`. **Fail-closed strict** conservé.
- **`bool shouldStartDosingPure(error, startThreshold, cyclesToday, maxCyclesPerDay)`** — démarre ssi `cyclesToday < maxCyclesPerDay` ET `error > startThreshold`.
- **`bool shouldContinueDosingPure(error, stopThreshold, runTimeMs, minInjectionTimeMs)`** — force la poursuite tant que `runTimeMs < minInjectionTimeMs` (30 s), puis poursuit ssi `error > stopThreshold`. Le **temps est injecté** → le minimum d'injection est testable sans attendre 30 s réelles.

### Causes de refus (énum ↔ String FR)

`enum class DoseRefusal` ([`dosing_logic.h`](../../src/dosing_logic.h:27)) — l'ordre des valeurs **suit l'ordre des gardes** de `canDose()`. La coquille traduit chaque valeur en la chaîne française **identique** à celle exposée avant le refactor (les valeurs runtime — points de calibration, mode, nombre de cycles — sont réinjectées par la coquille au moment du formatage) :

| `DoseRefusal` | Garde `canDose` | Cause FR (réinjectée) |
|---|---|---|
| `WatchdogInactive` | #2 | watchdog inactif |
| `FiltrationOff` | #3 | pas de présence d'eau (résolue par `resolveWaterPresent` selon le mode d'installation) |
| `ReadingNaN` | #4 | lecture filtrée indisponible |
| `FilterNotReady` | #4b | filtre non prêt |
| `FilterUnstable` | #4c | capteur instable |
| `CalibrationInsufficient` | #5 | calibration insuffisante (`cal=X requis=Y`) |
| `StabilizationActive` | #6 | stabilisation post-calibration en cours |
| `MixingActive` | #6b | pause mélange en cours |
| `ModeNotAutomatic` | #7 | mode régulation ≠ automatic (`X`) |
| `DailyLimit` | #8 | limite journalière atteinte |
| `HourlyLimit` | #9 | limite horaire atteinte |
| `CyclesPerDay` / `BurstPerMinute` / `BurstPer15Min` | #4 (anti-cycling) / #10 | limite cycles/jour / anti-rafale |

> ⚠️ **Invariant** : toute évolution future de la décision de dosage **passe par `dosing_logic`** et doit conserver l'équivalence stricte (table d'équivalence validée par `pool-chemistry`). La logique pure est verrouillée par les tests natifs (`test/test_native_dosing/`, dont un **verrou de non-régression** du bug pause-mélange v2.2.5 : une injection auto dure **≥ `minInjectionTimeMs`** avant que la pause ne s'arme). Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

### Calcul proportionnel pur (`computePidPure`)

[feature-037](../../specs/features/done/feature-037-dosage-proportionnel-testable.md) prolonge le pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), **pas de nouvel ADR**) au **cœur du calcul de débit**. Le calcul PID lui-même est extrait de `computePID()` vers une fonction **pure** [`computePidPure`](../../src/dosing_logic.h) ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)) — sans `<Arduino.h>`, sans `millis()`, sans état membre — donc **testable en natif**. *Characterization refactor* : **les débits calculés et l'évolution de l'intégrale sont strictement préservés** (équivalence validée par `pool-chemistry`).

**Ce que `computePidPure` calcule** (tout le métier) :

1. Terme proportionnel `kp × error`, plus termes `ki × integral` et `kd × (error − lastError) / dt` (inertes tant que `Ki = Kd = 0`).
2. **Anti-windup** : si `freezeIntegral` est vrai, l'intégrale est **gelée** (aucune accumulation) ; sinon elle est accumulée puis **bornée** à `±integralMax` (= 50).
3. **Plancher** : une sortie négative est forcée à `0` (pas de débit négatif).
4. **Bornage final min/max intégré (« Option Y »)** : le `constrain(flow, minFlow, maxFlow)` qui était appliqué *chez les appelants* pH/ORP **après** `computePID` est désormais **dans la fonction pure**. `computePidPure` renvoie donc directement le **débit FINAL borné** ; le `constrain` externe a été supprimé des chemins pH et ORP.

**État PID injecté / renvoyé** : l'état n'est plus une variable membre mutée en place. Il est **passé en paramètre** (intégrale + dernière erreur) et **renvoyé** via `struct PidResult { float flow; float integral; float lastError; }`. La coquille relit `flow` (débit final) et réinjecte `integral` / `lastError` dans l'état PID persistant de la pompe.

**`computePID` = coquille mince** : ne fait plus que gérer le **temps** (`dt` via `millis()`, plafonnement des deltas > 10 s), porter l'**état PID** d'un cycle à l'autre, dériver le flag `freezeIntegral`, déléguer à `computePidPure`, puis renvoyer le débit final. Les **deux chemins appelants pH et ORP** sont eux aussi des coquilles : ils ne réimplémentent aucune arithmétique de régulation.

> `computeFlowFromError` est marquée **DEAD CODE** (ancien calcul de débit non utilisé par le chemin actif) — **non extraite** dans le module pur.

**Testabilité native** : `computePidPure` est couverte à **100 % des lignes** par la suite Unity native (`test/test_native_dosing/`). Le temps et l'état étant injectés, chaque branche (gel intégrale, bornage `±integralMax`, plancher 0, bornage final min/max) est exercée sans matériel ni attente réelle.

### Gardes des injections manuelles (feature-006)

`canDose()` couvre la régulation **automatique**. Depuis v2.6.0, les injections manuelles volumées (`/ph/inject/start`, `/orp/inject/start`) passent par leur propre décision **pure** `evaluateManualInject(const ManualInjectInputs&)` ([`src/dosing_logic.h`](../../src/dosing_logic.h)), appelée par la coquille `manualInjectGuardOrReject()` ([`src/web_routes_control.cpp`](../../src/web_routes_control.cpp)) qui collecte les entrées et formate le refus en **409 JSON** `{"error","message","seconds_remaining"?,"remaining_ml"?}` (codes documentés dans [API.md](../API.md#codes-de-refus-409-v260)). Ordre validé `pool-chemistry` (GO pré + post) — **ne pas réordonner** :

| # | Garde | Refus si | Frontière figée |
|---|-------|----------|-----------------|
| 1 | Watchdog actif | wdt inactif sur `loopTask` | — |
| 2 | Présence d'eau | `resolveWaterPresence().waterPresent == false` (feature-056 : `managed`→filtration commandée, `powered`→toujours vrai, `external`→signal récent) | — |
| 3 | Stabilisation post-cal | fenêtre active **pour la pompe visée** | — |
| 4 | Double démarrage | injection manuelle déjà active sur cette pompe | — |
| 5 | Limite journalière **prédictive** | `cumul + volume demandé > max` (limite ≤ 0 = illimité) | `==` **acceptée** (strict `>`) ; `remaining_ml` renseigné dans le refus |
| 6 | Limite horaire **prédictive** | `usedMs + durée demandée > limite` (limite 0 = illimité) | `==` acceptée (strict `>`) ; budget **partagé** avec l'auto ([ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) |
| 7 | Cycles/jour | `cyclesToday >= maxCyclesPerDay` (compteur partagé auto + manuel) | `>=` (identique à l'auto) |
| 8 | Anti-rafale 1 min | `>= 6` démarrages dans la fenêtre (ring partagé) | `>=` |
| 9 | Anti-rafale 15 min | `>= 20` démarrages dans la fenêtre (ring partagé) | `>=` |

**Volontairement absents** (le manuel est aveugle à la mesure — l'opérateur assume la décision chimique) : gardes NaN / filtre capteur / calibration / mode de régulation / pause mélange. Le volume/durée évalués sont **post-plafonnement** `kManualInjectMaxDurationS` (condition pool-chemistry #3 : on évalue ce qui sera vraiment injecté).

**Budget horaire partagé ([ADR-0020](../adr/0020-budget-horaire-dosage-unique.md))** : avant feature-006, le manuel ne contournait pas seulement la limite horaire — il n'était **pas compté** dans `usedMs` (jusqu'à 2× le budget/heure possible). Désormais `refreshDosingState(state, now, manualActive)` (privée, [`pump_controller.cpp:111`](../../src/pump_controller.cpp:111)) accumule `usedMs` si `state.active || manualActive`, avec pour prédicat manuel **celui du safety tracking** (`manualMode[i] && pumpDuty[i] > 0`) : injections manuelles web, **pompes test** (`/pumpN/on`, `/pumpN/duty/*`) et **commandes UART** consomment donc aussi le budget. OR unique sur un seul point d'accumulation → pas de double comptage.

**Enregistrement de cycle manuel (pending atomique)** : à l'acceptation, la route appelle `requestManualCycleRecord(pumpIndex)` (incrément `std::atomic<uint8_t> _manualCycleStartPending[2]`, thread-safe depuis `async_tcp`). Le pending est consommé **en tête de `update()`** ([`pump_controller.cpp:657`](../../src/pump_controller.cpp:657)), en `loopTask`, avant tout early-return : `recordDosingCycleStart()` (ring anti-rafale **partagé** avec l'auto) + `cyclesToday++` + flush NVS différé. Le ring et `cyclesToday` restent ainsi écrits par **une seule tâche** (pas de mutex). Les getters `getCyclesToday()` / `getRecentCycles()` **ajoutent le pending** non consommé : deux starts manuels rapprochés se voient mutuellement (fail-closed). Lectures snapshot sans mutex : compteurs écrits en `loopTask` uniquement, lectures 32 bits atomiques sur ESP32 (course bénigne : au pire une valeur en retard d'un tour de loop).

**Asymétrie stabilisation assumée (condition pool-chemistry #5)** : la boucle **auto** suspend les **2 pompes** dès qu'une stabilisation est active (garde globale d'`update()`), alors que l'injection **manuelle** n'est bloquée que par la fenêtre de **sa** pompe (`getStabilizationRemainingS(pumpIndex)`) — les fenêtres sont indépendantes par sonde (cinétiques différentes, 5 min pH / 3 min ORP).

**Garde watchdog et tâches** : les handlers AsyncWebServer tournent dans `async_tcp`, non abonnée à la TWDT — `esp_task_wdt_status(NULL)` y renverrait toujours NOT_FOUND. La garde interroge donc `esp_task_wdt_status(loopTaskHandle)` (la tâche qui exécute le dosage).

**Routes de test `/pump1/on`, `/pump2/on`** : gardées **uniquement** sur la filtration (helper `injectionAllowedOrReject()`, pas de volume demandé → pas de garde volumétrique), désormais au **même format 409 JSON** (`filtration_off`). Leur temps de marche consomme le budget horaire partagé (cf. ci-dessus).

> ⚠️ **Commande UART `pump_test` non gardée** : l'action `run_action/pump_test` du protocole écran ([`src/uart_commands.cpp:496`](../../src/uart_commands.cpp:496)) appelle `setManualPump()` **sans passer par `evaluateManualInject()`** — bypass assumé car l'écran LVGL n'est **pas déployé**. **À guarder si l'écran est mis en service** (condition pool-chemistry #4, feature-006). Son temps de marche consomme néanmoins le budget horaire partagé.

**Arrêt cyclique (v2.1.2, conservé)** : `updateManualInject()` (appelée chaque tour `loopTask`) interrompt l'injection si la filtration tombe pendant celle-ci. Latence < 100 ms. Émet un log `critical("[Injection] {pH|ORP} INTERROMPUE — filtration arrêtée (sécurité chimique)")` + alerte MQTT `ph_injection_aborted` ou `orp_injection_aborted` sur `{base}/alerts`. **Pas de reprise automatique** : l'injection est perdue, l'utilisateur relance manuellement (toast UI explicite, voir [`docs/features/page-ph.md`](../features/page-ph.md) et [`docs/features/page-orp.md`](../features/page-orp.md)).

**Routes d'arrêt** (`/ph/inject/stop`, `/orp/inject/stop`, `/pump[12]/off`) : **inconditionnelles** — pouvoir arrêter en toute circonstance, y compris pendant une stabilisation.

**Crédit plancher à la fin naturelle (v2.9.1)** : le cumul journalier de sécurité (`dailyPhInjectedMl` / `dailyOrpInjectedMl`) est alimenté par **intégration débit × temps** (`updateSafetyTracking`), qui perd systématiquement quelques dixièmes de mL aux bornes d'une injection manuelle volumée : arrondi de `durationS` à la seconde entière, tranche entre le démarrage réel (handler async) et le premier tick de comptage, et surtout **dernière tranche jamais comptée** (`updateManualInject()` coupe la pompe à l'expiration → au tick suivant le delta final est jeté). La garde d'acceptation raisonnant, elle, en **volume exact** (garde #5, frontière `==` acceptée), demander exactement le reliquat du quota (limite 50 mL, injection 50 mL) laissait le cumul à 49,x → `ph/orp_limit_reached` jamais latché, badge dashboard « Cumul 98 % » figé.

Correctif : à l'acceptation, la route mémorise dans `ManualInjectState` le cumul au départ (`startCumulMl`) et le volume promis post-plafonnement (`creditMl = effectiveMl`) ([`web_routes_control.cpp:335`](../../src/web_routes_control.cpp:335)). À la **fin naturelle** (expiration de `durationMs`), `updateManualInject()` appelle `creditManualInjectionFloor(idx, startCumulMl, creditMl)` ([`pump_controller.cpp:675`](../../src/pump_controller.cpp:675)) qui **planche** le registre de sécurité :

- cas nominal : `daily = max(daily, startCumulMl + creditMl)` ;
- **rollover minuit pendant l'injection** (détecté par `daily < startCumulMl`) : `daily = max(daily, creditMl)` — le volume **entier** est crédité au jour nouveau (choix conservateur, sur-compte possible).

| Chemin d'arrêt | Crédit plancher |
|---|---|
| Fin naturelle (expiration de durée, cas 1 d'`updateManualInject`) | ✅ oui |
| Stop manuel (`/ph\|orp/inject/stop`) | ❌ non — l'intégration réelle fait foi (injecté moins → compté moins) |
| Interruption filtration (arrêt sécurité chimique) | ❌ non — idem |

- **Propriété fail-safe (validée pool-chemistry)** : le registre de sécurité **ne sous-compte jamais** un volume promis et injecté en entier ; il peut **sur-compter de quelques dixièmes de mL** (conservateur — la limite journalière se déclenche au plus tôt, jamais au plus tard).
- **Suivi produit NON planché** : `productCfg.phTotalInjectedMl` / `orpTotalInjectedMl` (stock) restent sur l'intégration réelle → peuvent **sous-estimer légèrement la consommation** (dérive cumulative possible sur les fractions de mL au fil des injections manuelles). Assumé : le stock est un indicateur, pas un registre de sécurité.
- Exécution en `loopTask` uniquement (mêmes règles d'accès aux compteurs qu'`updateSafetyTracking` : écrivain unique, pas de mutex) ; ajustement effectif → `_dailyCountersDirty` armé (flush NVS différé).
- **Logs `info`** : `[Sécurité] Compteur journalier pH|ORP ajusté de X à Y mL (crédit fin d'injection manuelle)` (émis seulement si un ajustement a réellement eu lieu) ; le log de fin de durée mentionne désormais le cumul crédité : `[Injection] pH|ORP arrêtée automatiquement (fin de durée) — cumul crédité à X mL`.

**Écrêtage au reliquat journalier (v2.9.2)** : les routes `/ph|orp/inject/start` ([`web_routes_control.cpp:297`](../../src/web_routes_control.cpp:297) et [`:392`](../../src/web_routes_control.cpp:392)) n'opposent plus le refus `daily_limit` quand la demande dépasse le reliquat (`max*MlPerDay − daily*InjectedMl`) : la demande est **écrêtée au reliquat** — durée recalculée arrondie **vers le bas** (`floor` volontaire : le volume effectif ne re-déborde jamais la limite), log `info` `[Injection] pH|ORP : volume écrêté de X à Y mL (reliquat journalier)`. Interaction avec le crédit plancher ci-dessus :

- **écrêtage sans re-plafonnement durée** : `creditMl = remaining` (reliquat **entier**, pas `effectiveMl`) → à la fin naturelle, le plancher porte le cumul **exactement à la limite** → latch `ph/orp_limit_reached` + badge « Limite journalière atteinte ». Sur-compte borné à < 1 s de débit (conservateur, cohérent avec la propriété fail-safe v2.9.1) ;
- **double-clamp** (reliquat > 10 min de pompe) : le plafond `kManualInjectMaxDurationS` gagne → `creditMl = effectiveMl` — le crédit n'excède **jamais** ce qui a pu être réellement injecté.

Le refus `daily_limit` ne subsiste que si le reliquat est **nul ou < 1 s de pompe** ; son message et son champ `remaining_ml` arrondissent désormais **vers le bas** (`floorf`, plus jamais de « reste 11 mL » qui refuse 11). La garde pure `evaluateManualInject()` est **inchangée** (frontière `==` toujours verrouillée par test natif) : l'écrêtage est fait en amont dans le handler, symétrique pH/ORP et identique pour le chemin legacy `?duration=`.

**Bornage durée** : `duration` query param plafonné à `kManualInjectMaxDurationS = 600 s` (10 min, abaissé de 3600 s en v2.1.2). Justification `pool-chemistry` : 3600 s expose à un risque trop long si la filtration s'arrête en milieu de cycle ; 10 min couvrent les usages typiques.

> Cohérence : la garde filtration reproduit **exactement** la condition #3 de `canDose()` — elle consomme la même source unique `filtration.resolveWaterPresence().waterPresent` (feature-056). En mode `powered` (ex-`continu`), l'alimentation 12 V des pompes suit la filtration au niveau matériel → présence d'eau présumée `true` en permanence ; en `external`, la présence dépend du signal récent (fail-safe OFF).

**Testabilité native** : `evaluateManualInject` est couverte par **17 tests** dédiés (`test/test_native_dosing/test_dosing_decision.cpp`, préfixe `test_F006_*` — 135 tests au total) : une cause de refus par test + nominal, reliquat, frontières `==` acceptées, limites 0/≤0 = illimité, ordre de priorité des gardes, partage du ring anti-rafale avec l'auto. `dosing_logic.cpp` reste à **100 % des lignes**.

### Ring buffer anti-rafale

Chaque pompe maintient un ring buffer `_dosingCycleHistory[2][kDosingCycleHistorySize = 20]` indexé par `_dosingCycleHistoryIdx[2]`. À chaque démarrage effectif d'un cycle de dosage (transition PWM 0 → >0), `recordDosingCycleStart(pumpIndex)` ajoute le timestamp `millis()` courant.

Au prochain `canDose(pumpIndex)`, `countRecentDosingCycles(pumpIndex, windowMs)` compte les entrées dans la fenêtre `[now - windowMs, now]` :
- `kMaxDosingCyclesPerMinute = 6` cycles max sur 60 000 ms
- `kMaxDosingCyclesPer15Min = 20` cycles max sur 900 000 ms

Sites d'appel à `recordDosingCycleStart()` (4 au total) : démarrage cycle pH automatique, démarrage cycle pH scheduled, démarrage cycle ORP automatique, démarrage cycle ORP scheduled.

> **Indépendant** des limites volumétriques (`ph_limit_minutes`, `max_ph_ml_per_day`) et de l'anti-cycling existant (`maxCyclesPerDay = 20` mesuré sur 24 h glissante). Couvre le cas d'un PID qui démarrerait 30 cycles très courts en 5 minutes (lectures bruitées sur sonde mal calibrée par exemple).

## Anti-rafale & rollover journalier (logique pure)

[feature-039](../../specs/features/done/feature-039-anti-rafale-rollover-testable.md) prolonge le pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), **pas de nouvel ADR**) à deux gardes critiques restées non testées : l'**anti-rafale** (comptage de cycles sur fenêtres glissantes) et les **déclencheurs de rollover journalier**. Leur cœur est extrait de `pump_controller.cpp` vers le module pur [`src/dosing_logic.{h,cpp}`](../../src/dosing_logic.h) (sans `<Arduino.h>`, sans `millis()`, sans `time()`/NVS, sans état membre) → **testable en natif**. *Characterization refactor* : **aucun seuil, fenêtre ni frontière n'a changé** (équivalence stricte, 2 passages `pool-chemistry`).

### Fonctions pures

- **`int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs)`** — copie exacte de `countRecentDosingCycles` : ignore les slots à `0`, compte un timestamp si `(now - ts) <= windowMs`. L'arithmétique en **`uint32_t` non signé** est **wrap-safe** : au passage `0xFFFFFFFF → 0`, un `ts` pré-wrap et un `now` post-wrap donnent toujours un delta cohérent (la fenêtre reste correcte). Frontière `<=` **inclusive** (un `ts == now`, delta 0, est compté). `now` injecté en paramètre.
- **`size_t recordCycleTimestamp(uint32_t* history, size_t idx, size_t size, uint32_t now)`** — écrit `now` au slot `idx` du ring buffer et **renvoie** le prochain index circulaire `(idx + 1) % size` (pas de référence mutée). Écrase le plus ancien une fois le buffer plein.
- **`bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr)`** — vrai ssi une date est **déjà connue** (`strlen > 0`) **ET** diffère de la date du jour. `currentDayDate` vide (première init après boot) → `false` (cas géré par la coquille, branche 2 de [Reset journalier](#reset-journalier)).
- **`bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now)`** — fallback 24 h quand l'heure n'est pas synchronisée : vrai ssi `(now - dayStartMs) >= 86400000` (24 h). Frontière `>=` **inclusive**, arithmétique `uint32_t` wrap-safe.

### Coquilles `pump_controller`

`countRecentDosingCycles(pumpIndex, windowMs)`, `recordDosingCycleStart(pumpIndex)` et `tickDailyRollover()` deviennent des **coquilles minces** : elles fournissent `millis()` / `time()` / les buffers membres (`_dosingCycleHistory`, `_dosingCycleHistoryIdx`) et délèguent aux fonctions pures. Restent dans la coquille `tickDailyRollover()` : `localtime_r` / `strftime` (calcul de `todayStr`), l'écriture de `safetyLimits`, `saveDailyCounters()`, `armStabilizationTimer()`, les logs et la **branche première-init** (date vide).

> ⚠️ **Comportement strictement préservé** : la garde anti-emballement de `canDose()` (conditions #14 `BurstPerMinute` / #15 `BurstPer15Min` — refus si > 6 cycles/min OU > 20 cycles/15 min) et le reset des quotas journaliers à minuit (date NTP) / fallback 24 h sont **inchangés**. Seuls les seuils `kMaxDosingCyclesPerMinute = 6`, `kMaxDosingCyclesPer15Min = 20`, `kDosingCycleHistorySize = 20` et le délai de 24 h restent en vigueur, à l'identique.

### Testabilité native

`countCyclesInWindow`, `recordCycleTimestamp`, `shouldRolloverByDate` et `shouldRolloverByMillis` sont couvertes par la suite Unity native (`test/test_native_dosing/`, **15 tests** dédiés, **85 tests au total**) ; `dosing_logic.cpp` atteint **100 % des lignes**. Le temps étant injecté, le **wrap `millis()`** (piège principal) et les frontières (`86400000`, fenêtres 60 000 / 900 000 ms) sont exercés sans matériel ni attente réelle. Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Pause mélange hydraulique (feature-025)

Après chaque injection, le bassin a besoin de temps pour s'homogénéiser avant qu'une nouvelle mesure soit représentative. La pause mélange empêche tout surdosage par réaction prématurée.

- `_mixingEndMs[2]` ([0] = pH, [1] = ORP) : timestamps gérés **par `millis()`**, **aucun `delay()`** (contrainte loop).
- `notifyPhDose(nowMs)` / `notifyOrpDose(nowMs)` : armés à l'**arrêt d'une injection** (lorsque l'injection en cours se termine, dans le bloc où `lastStopTime` est posé), positionnent `_mixingEndMs[i] = nowMs + kXxxMixingDelayMs`. La pause s'applique donc **après que la dose est versée** et bloque le **cycle suivant**, sans interrompre l'injection en cours.
- `isPhMixingDelayActive(nowMs)` / `isOrpMixingDelayActive(nowMs)` : `true` tant que `nowMs < _mixingEndMs[i]`. Consommés par `canDose()` (condition #6b) **et** publiés au WS / MQTT (`ph/orp_mixing_delay_active`).

> ⚠️ **Correctif v2.2.5 — armement à l'arrêt et non au démarrage.** Armer la pause au **démarrage** de l'injection (comportement initial de feature-025) la rendait active dès le cycle `update()` suivant : `canDose()` (condition #6b) court-circuitait alors la branche de régulation et coupait la pompe après ~un cycle de boucle. `minInjectionTimeMs` (30 s) et `shouldContinueDosing` devenaient du code mort sur le chemin auto → injections trop courtes, pH/ORP ne convergeant jamais. En armant la pause à l'**arrêt**, l'injection en cours dure au moins `minInjectionTimeMs` et `shouldContinueDosing` reste effectif ; la pause ne bloque que le cycle d'injection **suivant**. Bug **fail-safe** (sous-dosage, jamais surdosage).

| Constante | Valeur | Grandeur |
|-----------|--------|----------|
| `kPhMixingDelayMs` | `900000` ms (15 min) | pause après injection pH |
| `kOrpMixingDelayMs` | `1200000` ms (20 min) | pause après injection ORP (plus conservateur) |

> Gate **indépendante** du timer de stabilisation post-calibration (`_stabilizationEndMs`). Les deux sont des `OR` dans `canDose()` : la pompe reste bloquée tant que l'une OU l'autre est active. Écrits/lus uniquement en `loopTask` → pas de mutex (cohérent avec `_stabilizationEndMs`). Pendant la pause, l'intégrale PID est gelée (`freezeIntegral`).

## Stabilisation au démarrage filtration et post-calibration

**Stabilisation par pompe** (feature-021) : `_stabilizationEndMs[2]` — un timer indépendant pour pH (index 0) et ORP (index 1). La cinétique chimique différente justifie une fenêtre par sonde.

| Source d'arming | Fonction | Durée |
|-----------------|----------|-------|
| Calibration EZO pH réussie (`Cal,mid`, `Cal,low`, `Cal,clear`) | `armStabilizationTimer(0)` | `kStabilizationDurationPhMs = 5 min` |
| Calibration EZO ORP réussie | `armStabilizationTimer(1)` | `kStabilizationDurationOrpMs = 3 min` |
| Démarrage filtration en mode `managed` (pause précédente > `stabilizationDelayMin`) | `armStabilizationTimer()` (legacy, sans index) | `mqttCfg.stabilizationDelayMin` × 60 s — applique aux 2 pompes |
| Boot en mode `powered` | `armStabilizationTimer()` (legacy) | idem |
| Passage de minuit (mitigation double quota) | `armStabilizationTimer()` (legacy) | idem |

**Surcharge legacy `armStabilizationTimer()` (sans paramètre)** : conservée pour les sites « globaux » (filtration, boot continu, rollover minuit). Arme **les 2 pompes simultanément** avec `mqttCfg.stabilizationDelayMin × 60_000` ms.

- **Démarrage filtration en mode `managed`** : appel **conditionnel** — uniquement si `pauseMs > stabilizationMs`. Empêche le réarmement après un glitch très court (sauvegarde config, redémarrage relais involontaire). Au tout premier démarrage (`lastStoppedAtMs == 0`), la pause est considérée infinie → timer armé.
- **Arrêt filtration en mode `managed`** : `clearStabilizationTimer()` (efface les 2 pompes).
- **Modes `powered` / `external`** : le relais n'est pas piloté (feature-056), donc aucun appel automatique au démarrage/arrêt filtration. Le timer peut être armé manuellement (boot `powered`) ou ignoré selon le besoin.

`stabilizationDelayMin` est **configurable via `/save-config`** (plage 0-60 min, défaut 5). Valeur 0 = stabilisation legacy désactivée — la stabilisation post-calibration EZO reste, elle, toujours active (5 min pH / 3 min ORP).

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
| Régulation P — Kp pH (feature-025) | `kp` (défaut struct) | 8.0 | [`pump_controller.h`](../../src/pump_controller.h) |
| Régulation P — Kp ORP (feature-025) | `kp` (override constructeur) | 0.3 | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| Régulation P — Ki / Kd (feature-025) | `ki` / `kd` | 0 / 0 (Kd=0 impératif) | [`pump_controller.h`](../../src/pump_controller.h) |
| PID — anti-windup | `integralMax` | 50.0 (inerte avec Ki=0) | `PIDState` ([`pump_controller.h`](../../src/pump_controller.h)) |
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
| Stabilisation post-cal pH (feature-021) | `kStabilizationDurationPhMs` | 300 000 ms (5 min) | [`constants.h`](../../src/constants.h) |
| Stabilisation post-cal ORP (feature-021) | `kStabilizationDurationOrpMs` | 180 000 ms (3 min) | [`constants.h`](../../src/constants.h) |
| Anti-rafale — fenêtre 1 min (Pass 3.5) | `kMaxDosingCyclesPerMinute` | 6 | [`constants.h`](../../src/constants.h) |
| Anti-rafale — fenêtre 15 min (Pass 3.5) | `kMaxDosingCyclesPer15Min` | 20 | [`constants.h`](../../src/constants.h) |
| Anti-rafale — capacité ring buffer | `kDosingCycleHistorySize` | 20 entrées / pompe | [`constants.h`](../../src/constants.h) |
| Injection manuelle — durée max (v2.1.2) | `kManualInjectMaxDurationS` | 600 s (10 min) | [`constants.h`](../../src/constants.h) |
| Pause mélange pH (feature-025) | `kPhMixingDelayMs` | 900 000 ms (15 min) | [`constants.h`](../../src/constants.h) |
| Pause mélange ORP (feature-025) | `kOrpMixingDelayMs` | 1 200 000 ms (20 min) | [`constants.h`](../../src/constants.h) |
| Répartition scheduled — fenêtre (feature-011) | `kScheduledWindowMinutes` | 15 min | [`constants.h`](../../src/constants.h) |
| Boost — delta cible ORP (feature-053) | `kBoostOrpDeltaMv` | +60 mV | [`constants.h`](../../src/constants.h) |
| Boost — plafond cible ORP (feature-053) | `kBoostOrpCeilingMv` | 850 mV (< alerte `orp_abnormal` 900) | [`constants.h`](../../src/constants.h) |
| Boost — facteur limite journalière chlore (feature-053) | `kBoostDailyFactor` | 1,5× | [`constants.h`](../../src/constants.h) |
| Boost — plafond dur limite journalière chlore (feature-053) | `kBoostDailyHardCapMl` | 1000 mL | [`constants.h`](../../src/constants.h) |

> Une refonte est prévue pour rendre une partie de ces paramètres modifiables via un mode expert UI (cf. spec en cours).

## Mode `scheduled`

Injecte jusqu'à `phDailyTargetMl` / `orpDailyTargetMl` **pendant la filtration**, **aveugle à la mesure capteur** ([ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md)). Depuis la **v2.8.0 (feature-011)**, le volume quotidien n'est plus injecté d'un bloc : il est **réparti par fenêtres de 15 min** sur l'horizon de filtration restant, borné à minuit ([ADR-0021](../adr/0021-repartition-scheduled.md)).

### Algorithme de répartition (feature-011, v2.8.0)

**Architecture Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) : la décision vit dans la fonction **pure** `evaluateScheduledDose(ScheduledDoseInputs) -> ScheduledDoseDecision` ([`src/dosing_logic.h`](../../src/dosing_logic.h)) ; les branches scheduled pH et ORP de `update()` sont des **coquilles symétriques** (collecte des entrées + application du verdict).

Logique pure (à chaque tick) :

1. **Fenêtre absolue alignée horloge murale** : `windowIndex = nowMin / kScheduledWindowMinutes` (0..95) — idempotent après reboot, pas de dérive de phase.
2. **À l'entrée d'une nouvelle fenêtre**, recalcul depuis l'état courant : `remaining = min(dailyTargetMl, maxDailyMl) − dailyInjectedMl` (injections manuelles incluses), `nWin = horizonMinutes / 15` (plancher 1), volume de fenêtre `v = remaining / nWin`. Le recalcul par fenêtre absorbe **automatiquement** changement de cible, injection manuelle, retard subi (limite horaire) et redémarrage ESP32.
3. **Bornage budget horaire partagé** ([ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) : `v` est plafonné par `(hourlyLimitMs − usedMs)` converti en mL via le débit effectif ; l'excédent se reporte aux fenêtres suivantes.
4. **Report anti short-cycling** : si la durée d'injection de `v` est < `minInjectionTimeMs` (30 s, `pumpProtection`), `v = 0` pour cette fenêtre — le volume se reporte naturellement (moins de fenêtres restantes → doses plus longues). Effet visible avec une **pompe rapide et une petite cible** : les premières fenêtres sont reportées, les injections démarrent quand l'horizon se resserre.
5. **Snapshot `stopTargetMl = dailyInjectedMl + v`** figé à l'entrée de fenêtre ; `doseNow` réévalué **à chaque tick** contre `min(stopTargetMl, cible effective)` — une baisse de cible ou de plafond mi-fenêtre arrête l'injection immédiatement.
6. **Fail-closed** : watchdog inactif, horizon ≤ 0, débit effectif ≤ 0 ou fenêtre ≤ 0 → refus (`{false, -1, 0, NAN}`), débit planifié `NAN`.

Côté coquille (`pump_controller.cpp`) :

- **Heure locale** via `time()` + `kMinValidEpoch` : heure invalide → **aucune injection** (fail-closed historique), warning unique `[Scheduled] Heure locale indisponible — dosage pH programmé suspendu` (idem ORP).
- **Horizon** : `remainingRangeMinutes(nowMin, start, end)` ([`src/schedule_logic.h`](../../src/schedule_logic.h)) — minutes restantes de la plage de filtration, **bornées à minuit** (reset des compteurs journaliers, [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md)). **Hors mode d'installation `managed`** (feature-056 : `installMode != ManagedFiltration`), horizon = `1440 − nowMin` : le dosage hors plage reste permis (eau présumée 24/7 en `powered`, horizon plein en `external` borné par la garde présence d'eau). Comportement de l'ex-`continu` préservé.
- **Ring anti-rafale consulté AVANT tout démarrage** (condition pool-chemistry n°1) : transition inactive → active refusée si `kMaxDosingCyclesPerMinute` (6/min) ou `kMaxDosingCyclesPer15Min` (20/15 min) atteint — mêmes seuils et fenêtres que `canDose`, ring **partagé** auto + manuel + scheduled. Chaque démarrage est enregistré via `recordDosingCycleStart()`.
- **Exemption `cyclesToday` conservée** (arbitrage R4 validé pool-chemistry) : les démarrages scheduled n'incrémentent pas `maxCyclesPerDay`, car ils sont **bornés structurellement** (≤ 4/h par le cadencement 15 min) et gardés par le ring anti-rafale. Voir [ADR-0021](../adr/0021-repartition-scheduled.md).
- **Débit effectif unique** (condition n°5) : la même variable `effectiveFlowMlPerMin` (`maxFlowMlPerMin × pumpNMaxDutyPct`) sert au bornage horaire, à la durée d'injection et au duty PWM.
- **Plafonnement `maxPhMlPerDay` / `maxChlorineMlPerDay`** et logs existants conservés ; reset PID anti-windup conservé.
- **Reliquat à minuit** : `tickDailyRollover()` logge en `info` le volume non injecté (`[Scheduled] Reliquat pH perdu au passage de minuit : X mL non injectés`) puis réarme les fenêtres (`_*SchedWindowIdx = -1`). **Pas de rattrapage J+1** (décision produit, ADR-0021).
- **Diagnostic WS** : accesseurs publics `getPhScheduledPlannedFlow()` / `getOrpScheduledPlannedFlow()` (débit moyen planifié restant `remaining / horizon`, mL/min ; `NAN` hors scheduled / hors plage / heure invalide) → champs WS `ph/orp_scheduled_flow_ml_per_min` (`null` si `NAN`).

**Tests natifs** : 12 tests `evaluateScheduledDose` ([`test/test_native_dosing/test_dosing_decision.cpp`](../../test/test_native_dosing/test_dosing_decision.cpp)) + 5 tests `remainingRangeMinutes` ([`test/test_native_schedule/test_schedule_logic.cpp`](../../test/test_native_schedule/test_schedule_logic.cpp)) — 152 tests au total.

### Warnings edge-triggered

Les conditions warning/critical sont signalées **une seule fois** à l'entrée dans l'état problématique, puis un INFO de recovery quand la condition disparaît. Sans cela, chaque itération de `update()` (~100 Hz) émettait le même log → centaines de lignes par seconde, partition `history` saturée en quelques minutes.

| Branche | Message warning/critical | Message recovery (INFO) |
|---------|--------------------------|-------------------------|
| pH | `[Scheduled] Capteur pH hors plage (X) — dosage programmé maintenu` | `[Scheduled] Capteur pH revenu dans la plage normale` |
| pH | `[Scheduled] phDailyTargetMl (X mL) dépasse maxPhMlPerDay (Y mL) — plafonné` | (reset silencieux du flag) |
| pH | `[Scheduled] Débit pompe pH non configuré (0 mL/min) — dosage bloqué` | (reset silencieux du flag) |
| pH | `[Scheduled] Heure locale indisponible — dosage pH programmé suspendu` (feature-011) | (reset silencieux du flag) |
| pH | `[Scheduled] Volume de fenêtre pH tronqué par le budget horaire (X mL → Y mL) — report aux fenêtres suivantes` (feature-011) | (reset silencieux du flag) |
| ORP | `[Scheduled ORP] Capteur ORP hors plage (XmV) — dosage programmé maintenu` | `[Scheduled ORP] Capteur ORP revenu dans la plage normale` |
| ORP | `[Scheduled ORP] orpDailyTargetMl (X mL) dépasse maxChlorineMlPerDay (Y mL) — plafonné` | (reset silencieux du flag) |
| ORP | `[Scheduled ORP] Débit pompe ORP non configuré (0 mL/min) — dosage bloqué` | (reset silencieux du flag) |
| ORP | `[Scheduled ORP] Heure locale indisponible — dosage ORP programmé suspendu` (feature-011) | (reset silencieux du flag) |
| ORP | `[Scheduled ORP] Volume de fenêtre ORP tronqué par le budget horaire (X mL → Y mL) — report aux fenêtres suivantes` (feature-011) | (reset silencieux du flag) |

**Pattern** : variable `static bool xxxLogged` locale à la branche, mise à `true` au premier signalement, remise à `false` quand la condition redevient normale (ce qui ré-arme le warning si l'état repart en faute). Le démarrage différé par l'anti-rafale scheduled est loggé en `info` (edge-triggered aussi).

## Mode Boost (feature-053)

Le **Mode Boost** est une **surcouche temporaire non destructive** permettant d'assainir davantage la piscine pour la journée après une forte fréquentation. Il **ne modifie jamais la config persistée** (cible, limites, mode) : il expose des **valeurs *effectives*** consommées par la régulation et la filtration. Décision structurante et bornes de sécurité : [ADR-0025](../adr/0025-mode-boost.md) (validé `pool-chemistry` GO pré + post).

### État & persistance

- `boostState` = `{ bool active; time_t untilEpoch; }` persisté en **NVS dédié** (namespace `poolctrl`) → survit à un reboot dans la journée.
- **Activation refusée sans heure synchronisée** : `untilEpoch` = **prochain minuit local**, incalculable sans horloge valide → l'activation retourne `409 time_not_synced` (route HTTP) ou ignore la commande MQTT (pas de boost « sans fin »).
- Getters publics : `isBoostActive()`, `getBoostUntilEpoch()`. Pilotage : `startBoost()` / `stopBoost()` (routes `POST /boost/start` \| `/boost/stop`, commande HA `{base}/boost/set`).

### Expiration au rollover minuit

`tickBoostExpiry()` est appelée **avant** le reset des compteurs journaliers, **dans les 2 branches** de `tickDailyRollover()` (NTP/RTC synchronisé ET fallback `millis()`) : à minuit local, si le boost est actif il est désactivé (log `info`) et l'état republié (WS + MQTT). Au **boot**, si `now >= untilEpoch` le boost est inactif ; sinon il reprend. Le budget est donc **trivialement borné à un seul jour calendaire** (jamais à cheval sur deux fenêtres de compteur journalier).

### Trois effets tant que `isBoostActive()`

1. **Filtration forcée en marche** via un chemin de forçage **dédié** `boostForce`, prioritaire dans `decideFiltrationRun` ([`src/filtration.cpp`](../../src/filtration.cpp)), **indépendant** du `forceOn` utilisateur et de son `kForceTimeoutMs`. Dérivé de `isBoostActive()` → **ré-appliqué automatiquement au boot** sans dépendre d'un timer.
2. **Cible ORP effective** : `getEffectiveOrpTarget() = min(orpTarget + kBoostOrpDeltaMv, kBoostOrpCeilingMv)` (60 mV / plafond 850 mV, sous l'alerte `orp_abnormal` > 900). **Jamais abaissée.** La régulation ORP **automatique** existante injecte naturellement vers cette cible — **aucun nouveau chemin d'injection**.
3. **Limite journalière chlore effective** : `getEffectiveMaxChlorine() = min(maxChlorineMlPerDay × kBoostDailyFactor, kBoostDailyHardCapMl)` (×1,5 / plafond dur 1000 mL).

### Gate mode `automatic` (logique pure)

L'effet **chlore** (points 2 et 3) est **strictement gaté au mode de régulation ORP `automatic`** : les fonctions pures `effectiveOrpTargetPure(...)` / `effectiveMaxChlorinePure(...)` ([`src/dosing_logic.h`](../../src/dosing_logic.h), patron [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) ne relèvent cible et limite que si `mode == automatic`. En mode **Manuel** ou **Programmé**, le Boost n'étend **que la filtration** (point 1) ; l'**injection manuelle reste bornée à la limite normale**.

### Signalement des leviers inertes (feature-054, v2.18.1)

Chacun des deux leviers du Boost peut être **inerte** selon la config :

| Levier | Condition d'effet | Inerte si |
|--------|-------------------|-----------|
| Filtration prolongée | `filtrationCfg.enabled` (filtration gérée par PoolController) | Filtration non gérée → le forçage `boostForce` n'est jamais atteint (retour anticipé dans `filtration.update()`) |
| Chlore relevé | `orpRegulationMode == "automatic"` | Mode ORP Manuel / Programmé → cible et limite non relevées (gate ci-dessus) |

`startBoost()` ([`src/config.cpp`](../../src/config.cpp)) émet, après activation, un log `warning` par levier inerte — ce qui **couvre les activations HTTP ET MQTT/HA** (canal unique) :
- `!filtrationCfg.enabled` → « Filtration non gérée par PoolController — le Boost ne prolonge PAS la filtration ».
- `orpRegulationMode != "automatic"` → « Régulation ORP non automatique — le Boost ne relève PAS le chlore ».

La route `POST /boost/start` expose en parallèle ces deux états dans sa réponse 200 (`filtration_extended`, `chlorine_boosted`) pour que l'UI affiche un toast adaptatif ([API.md](../API.md#post-booststart--write), [page-dashboard.md](../features/page-dashboard.md#carte-boost-feature-053)). **Purement informatif** : aucun chemin de dosage ni aucune garde n'est modifié, l'activation n'est jamais bloquée.

### Garde-fous inchangés

**Tous les autres garde-fous restent intacts** : limite horaire, temps min d'injection (`minInjectionTimeMs`), anti-rafale (ring partagé), garde « injection uniquement si filtration active », watchdog. Le Boost ne fait que déplacer cible + plafond journalier consommés par `canDose()` / la régulation auto existante ; il **n'ouvre aucun chemin de dosage** et ne contourne aucune garde de `evaluateDose`.

### Testabilité native

`effectiveOrpTargetPure` / `effectiveMaxChlorinePure` (bornage + gate mode) sont couvertes par la suite Unity native (**+14 tests boost**, 199 tests au total) : plafonds, non-abaissement, gate `automatic` vs Manuel/Programmé, arithmétique du delta et du facteur. Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Interaction avec les autres composants

| Composant | Interaction |
|-----------|-------------|
| [`filtration`](filtration.md) | Démarrage filtration → `armStabilizationTimer()` ; arrêt filtration → `clearStabilizationTimer()`. **Boost** : `boostForce` prioritaire dans `decideFiltrationRun` force la marche tant que `isBoostActive()` (feature-053) |
| [`sensors`](sensors.md) | Lecture **filtrée** `getPhFiltered()` / `getOrpFiltered()` pour l'erreur PID (+ `isPhFilterReady`/`isPhFilterUnstable` dans `canDose`). Brut `getPh()`/`getOrp()` pour logs uniquement (feature-025) |
| [`mqtt_manager`](mqtt-manager.md) | Publication `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph/orp_mixing_delay_active`, `boost` (switch HA « Boost », feature-053) ; commande `{base}/boost/set` |
| [`ota_manager`](ota-manager.md) | `setOtaInProgress(true)` arrête toutes les pompes |
| [`web_routes_control`](web-server.md) | `/ph/inject/start`, `/orp/inject/start`, `/pump[12]/on` — gardes feature-006 : lecture `getCyclesToday`/`getRecentCycles`/`getStabilizationRemainingS(i)`/`get*UsedMs`, enregistrement via `requestManualCycleRecord` |
| [`uart_commands`](../../src/uart_commands.cpp) | `run_action/pump_test` → `setManualPump()` **sans garde** (écran non déployé — cf. [Gardes des injections manuelles](#gardes-des-injections-manuelles-feature-006)) |

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
- [`src/constants.h`](../../src/constants.h) — `kPumpPhPin = 25`, `kPumpOrpPin = 33` (PCB v2, voir ADR-0012)
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — décision de dosage pure (feature-036) + gardes d'injection manuelle `evaluateManualInject` (feature-006) + répartition scheduled `evaluateScheduledDose` (feature-011)
- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — `remainingRangeMinutes` (horizon de répartition scheduled, feature-011)
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md), [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md), [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md), [ADR-0014](../adr/0014-migration-atlas-ezo.md) (refonte `canDose`), [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) (régulation P temporisée, feature-025), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure Humble Object, feature-036), [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md) (budget horaire unique auto + manuel, feature-006), [ADR-0021](../adr/0021-repartition-scheduled.md) (répartition scheduled par fenêtres de 15 min, feature-011), [ADR-0025](../adr/0025-mode-boost.md) (Mode Boost — surcouche « valeurs effectives » + relèvement borné de la limite chlore, feature-053), [ADR-0026](../adr/0026-mode-installation.md) (Mode d'installation — 3 archétypes de câblage + résolution unique de la présence d'eau, feature-056)
- [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) — entrée filtrée, anti-windup, pause mélange, zone morte
- [feature-036](../../specs/features/done/feature-036-dosage-testable-decision-pure.md) — extraction de la décision de dosage en module pur testable
- [feature-039](../../specs/features/done/feature-039-anti-rafale-rollover-testable.md) — extraction de l'anti-rafale + déclencheurs de rollover en logique pure testable
- [feature-011](../../specs/features/doing/feature-011-repartition-24h-programmee.md) — répartition du volume quotidien scheduled par fenêtres de 15 min
- [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md) — UI consommatrices
