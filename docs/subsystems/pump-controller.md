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
unsigned long getStabilizationRemainingS() const;

// Gate fail-closed (feature-021)
bool canDose(int pumpIndex);                    // 0 = pH, 1 = ORP

// Pause mélange hydraulique post-injection (feature-025)
void notifyPhDose(uint32_t nowMs);              // arme la pause au démarrage d'une injection pH
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
   - Journalière : `maxPhMinusMlPerDay = 300`, `maxChlorineMlPerDay = 500`.
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
| 3 | **Filtration en marche** | filtration arrêtée (sauf mode `continu`) | eau ne circule pas — risque bouchon doseuse |
| 4 | **Lecture FILTRÉE non NaN** | `getPhFiltered()` ou `getOrpFiltered()` = NaN | feature-025 : le PID consomme le filtré ; NaN = warmup non amorcé / stale |
| 4b | **Filtre prêt** (feature-025) | `!isPhFilterReady()` / `!isOrpFilterReady()` | warmup en cours, mesure trop ancienne (> 20 s), ou EZO injoignable — **fail-closed** |
| 4c | **Capteur stable** (feature-025) | `isPhFilterUnstable()` / `isOrpFilterUnstable()` | ≥ 10 rejets consécutifs (EMI persistant) → on bloque plutôt que de lisser |
| 5 | **EZO calibré** | `cal_points < 2` (pH) ou `< 1` (ORP). `-1` (bus down) bloque. | pool-chemistry **cond #2** : régulation auto inhibée tant que calibration incomplète |
| 6 | **Pas de stabilisation post-cal** | `isStabilizationTimerActive(pumpIndex)` = true | pool-chemistry **cond #3** : 5 min pH / 3 min ORP après calibration EZO |
| 6b | **Pas de pause mélange** (feature-025) | `isPhMixingDelayActive()` / `isOrpMixingDelayActive()` | homogénéisation du bassin après injection (15 min pH / 20 min ORP) — gate **indépendante** de la stabilisation post-cal |
| 7 | **Mode régulation = `automatic`** | mode `manual` ou `scheduled` | la branche `scheduled` a sa propre logique, ne passe pas par `canDose` |
| 8 | **Limite journalière non atteinte** | `dailyXxxInjectedMl >= maxXxxMlPerDay` | sécurité chimique config |
| 9 | **Limite horaire non atteinte** | `usedMs > xxxLimitMinutes × 60000` dans la fenêtre 1 h glissante | sécurité chimique config |
| 10 | **Anti-rafale court terme** (Pass 3.5) | > 6 cycles/min OU > 20 cycles/15 min | anti-emballement PID — voir [Ring buffer anti-rafale](#ring-buffer-anti-rafale) |

> **Conditions #1, #2, #3, #5, #6** issues du checklist `pool-chemistry` validé en cadrage feature-021. **Condition #10** ajoutée en correctif Pass 3.5. **Conditions #4 (filtré), #4b, #4c, #6b** ajoutées en feature-025 — toutes **fail-closed** et **prioritaires** : elles s'ajoutent aux gardes existantes sans les contourner.

### Garde filtration sur l'injection manuelle (v2.1.2)

`canDose()` couvre la régulation **automatique**. L'injection **manuelle** (routes `/ph/inject/start`, `/orp/inject/start`, `/pump1/on`, `/pump2/on`) ne passe pas par `canDose()` — elle a sa propre garde implémentée dans [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp), avec **le même critère** pour cohérence :

```cpp
mqttCfg.regulationMode == "continu" || filtration.isRunning()
```

Deux mitigations :

1. **Refus en amont** — helper `injectionAllowedOrReject(req, tag)` retourne **HTTP 409** si la condition n'est pas remplie. Évite de démarrer une injection sans circulation d'eau (risque surdosage local zone retour).
2. **Arrêt cyclique** — `updateManualInject()` (appelée chaque tour `loopTask`) interrompt l'injection si la filtration tombe pendant celle-ci. Latence < 100 ms. Émet un log `critical("[Injection] {pH|ORP} INTERROMPUE — filtration arrêtée (sécurité chimique)")` + alerte MQTT `ph_injection_aborted` ou `orp_injection_aborted` sur `{base}/alerts`.

**Routes d'arrêt** (`/ph/inject/stop`, `/orp/inject/stop`, `/pump[12]/off`) : **inconditionnelles** — pouvoir arrêter en toute circonstance.

**Pas de reprise automatique** après reprise filtration : choix produit explicite. L'injection est perdue, l'utilisateur doit relancer manuellement (toast UI explicite, voir [`docs/features/page-ph.md`](../features/page-ph.md) et [`docs/features/page-orp.md`](../features/page-orp.md)).

**Bornage durée** : `duration` query param plafonné à `kManualInjectMaxDurationS = 600 s` (10 min, abaissé de 3600 s en v2.1.2). Justification `pool-chemistry` : 3600 s expose à un risque trop long si la filtration s'arrête en milieu de cycle ; 10 min couvrent les usages typiques.

> Cohérence : la garde web reproduit **exactement** la condition #3 de `canDose()` (filtration en marche sauf mode `continu`). En mode `continu`, l'alimentation 12 V des pompes suit la filtration au niveau matériel — la garde firmware est inutile et casserait le cas d'usage.

### Ring buffer anti-rafale

Chaque pompe maintient un ring buffer `_dosingCycleHistory[2][kDosingCycleHistorySize = 20]` indexé par `_dosingCycleHistoryIdx[2]`. À chaque démarrage effectif d'un cycle de dosage (transition PWM 0 → >0), `recordDosingCycleStart(pumpIndex)` ajoute le timestamp `millis()` courant.

Au prochain `canDose(pumpIndex)`, `countRecentDosingCycles(pumpIndex, windowMs)` compte les entrées dans la fenêtre `[now - windowMs, now]` :
- `kMaxDosingCyclesPerMinute = 6` cycles max sur 60 000 ms
- `kMaxDosingCyclesPer15Min = 20` cycles max sur 900 000 ms

Sites d'appel à `recordDosingCycleStart()` (4 au total) : démarrage cycle pH automatique, démarrage cycle pH scheduled, démarrage cycle ORP automatique, démarrage cycle ORP scheduled.

> **Indépendant** des limites volumétriques (`ph_limit_minutes`, `max_ph_ml_per_day`) et de l'anti-cycling existant (`maxCyclesPerDay = 20` mesuré sur 24 h glissante). Couvre le cas d'un PID qui démarrerait 30 cycles très courts en 5 minutes (lectures bruitées sur sonde mal calibrée par exemple).

## Pause mélange hydraulique (feature-025)

Après chaque injection, le bassin a besoin de temps pour s'homogénéiser avant qu'une nouvelle mesure soit représentative. La pause mélange empêche tout surdosage par réaction prématurée.

- `_mixingEndMs[2]` ([0] = pH, [1] = ORP) : timestamps gérés **par `millis()`**, **aucun `delay()`** (contrainte loop).
- `notifyPhDose(nowMs)` / `notifyOrpDose(nowMs)` : armés **au démarrage d'une injection** (transition PWM 0 → >0), positionnent `_mixingEndMs[i] = nowMs + kXxxMixingDelayMs`.
- `isPhMixingDelayActive(nowMs)` / `isOrpMixingDelayActive(nowMs)` : `true` tant que `nowMs < _mixingEndMs[i]`. Consommés par `canDose()` (condition #6b) **et** publiés au WS / MQTT (`ph/orp_mixing_delay_active`).

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
| Démarrage filtration en mode `pilote` (pause précédente > `stabilizationDelayMin`) | `armStabilizationTimer()` (legacy, sans index) | `mqttCfg.stabilizationDelayMin` × 60 s — applique aux 2 pompes |
| Boot en mode `continu` | `armStabilizationTimer()` (legacy) | idem |
| Passage de minuit (mitigation double quota) | `armStabilizationTimer()` (legacy) | idem |

**Surcharge legacy `armStabilizationTimer()` (sans paramètre)** : conservée pour les sites « globaux » (filtration, boot continu, rollover minuit). Arme **les 2 pompes simultanément** avec `mqttCfg.stabilizationDelayMin × 60_000` ms.

- **Démarrage filtration en mode `pilote`** : appel **conditionnel** — uniquement si `pauseMs > stabilizationMs`. Empêche le réarmement après un glitch très court (sauvegarde config, redémarrage relais involontaire). Au tout premier démarrage (`lastStoppedAtMs == 0`), la pause est considérée infinie → timer armé.
- **Arrêt filtration en mode `pilote`** : `clearStabilizationTimer()` (efface les 2 pompes).
- **Mode `continu`** : aucun appel automatique au démarrage filtration. Le timer peut être armé manuellement ou ignoré selon le besoin.

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

> Une refonte est prévue pour rendre une partie de ces paramètres modifiables via un mode expert UI (cf. spec en cours).

## Mode `scheduled`

Injecte jusqu'à `phDailyTargetMl` / `orpDailyTargetMl` **pendant la filtration**, **aveugle à la mesure capteur**. Le volume à injecter est réparti sur les plages de filtration. Borné par `ph_limit_minutes`, `max_ph_ml_per_day`, et `maxCyclesPerDay`. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).

### Warnings edge-triggered

Six conditions warning/critical sont signalées **une seule fois** à l'entrée dans l'état problématique, puis un INFO de recovery quand la condition disparaît. Sans cela, chaque itération de `update()` (~100 Hz) émettait le même log → centaines de lignes par seconde, partition `history` saturée en quelques minutes.

| Branche | Message warning/critical | Message recovery (INFO) |
|---------|--------------------------|-------------------------|
| pH | `[Scheduled] Capteur pH hors plage (X) — dosage programmé maintenu` | `[Scheduled] Capteur pH revenu dans la plage normale` |
| pH | `[Scheduled] phDailyTargetMl (X mL) dépasse maxPhMinusMlPerDay (Y mL) — plafonné` | (reset silencieux du flag) |
| pH | `[Scheduled] Débit pompe pH non configuré (0 mL/min) — dosage bloqué` | (reset silencieux du flag) |
| ORP | `[Scheduled ORP] Capteur ORP hors plage (XmV) — dosage programmé maintenu` | `[Scheduled ORP] Capteur ORP revenu dans la plage normale` |
| ORP | `[Scheduled ORP] orpDailyTargetMl (X mL) dépasse maxChlorineMlPerDay (Y mL) — plafonné` | (reset silencieux du flag) |
| ORP | `[Scheduled ORP] Débit pompe ORP non configuré (0 mL/min) — dosage bloqué` | (reset silencieux du flag) |

**Pattern** : variable `static bool xxxLogged` locale à la branche, mise à `true` au premier signalement, remise à `false` quand la condition redevient normale (ce qui ré-arme le warning si l'état repart en faute).

## Interaction avec les autres composants

| Composant | Interaction |
|-----------|-------------|
| [`filtration`](filtration.md) | Démarrage filtration → `armStabilizationTimer()` ; arrêt filtration → `clearStabilizationTimer()` |
| [`sensors`](sensors.md) | Lecture **filtrée** `getPhFiltered()` / `getOrpFiltered()` pour l'erreur PID (+ `isPhFilterReady`/`isPhFilterUnstable` dans `canDose`). Brut `getPh()`/`getOrp()` pour logs uniquement (feature-025) |
| [`mqtt_manager`](mqtt-manager.md) | Publication `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph/orp_mixing_delay_active` |
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
- [`src/constants.h`](../../src/constants.h) — `kPumpPhPin = 25`, `kPumpOrpPin = 33` (PCB v2, voir ADR-0012)
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md), [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md), [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md), [ADR-0014](../adr/0014-migration-atlas-ezo.md) (refonte `canDose`), [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) (régulation P temporisée, feature-025)
- [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) — entrée filtrée, anti-windup, pause mélange, zone morte
- [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md) — UI consommatrices
