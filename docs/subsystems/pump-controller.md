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

// Mode maintenance auto-guérison (v2.9.1) — inhibe/réarme TOUT dosage
// v2.27.0 : n'est plus `inline` — l'ENTRÉE en maintenance pose les compteurs
// journaliers en NVS (le mode est de durée non bornée et se termine le plus
// souvent par un redémarrage ; perdre les minutes de circulation surévaluerait
// la charge observée, cf. ADR-0053)
void setMaintenanceInhibit(bool);   // true → dosage suspendu toutes voies
bool isMaintenanceInhibited() const;

// Suspension « assistant de mesure du débit » (feature-075, v2.24.0) —
// suspend le dosage AUTO + SCHEDULED (deux chimies), préserve le mode MANUEL
// (véhicule de l'assistant). Écrit en contexte web async, consommé par update()
void setFlowAssistSuspend(bool suspend);
bool isFlowAssistSuspended() const;
// ⚠️ v2.36.0 — SENS CHANGÉ : isPhDosing() dit « une SESSION est ouverte », plus
// « la pompe tourne ». Depuis feature-094 le verrou de session suit la PORTE et
// non le débit : il reste vrai POMPE À L'ARRÊT pendant une prolongation (jusqu'à
// kPhSessionMaxWallMs = 3 h). Voir « Porte de session pH robuste au bruit ».
// Pour « la pompe est alimentée maintenant » : calendrier d'impulsion (ADR-0045).
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
void armStabilizationTimer();                   // arme les 2 pompes aux MÊMES durées
                                                // post-cal (v2.34.0 : ne lit plus
                                                // stabilizationDelayMin, retiré)
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

// Raison de blocage du dosage (feature-025) — chaîne vide si autorisé.
// v2.25.0 : les ex-getters `String get*DoseBlockedReason()` sont remplacés par une
// COPIE dans un tampon fourni par l'appelant. La cause est écrite par loopTask
// (surveillance, escalade, canDose) et lue depuis les handlers async_tcp : rendre
// une `String` par valeur allouait sur le tas dans la tâche réseau, à chaque appel.
void copyPhDoseBlockedReason(char* out, size_t outSize) const;
void copyOrpDoseBlockedReason(char* out, size_t outSize) const;

// v2.25.1 — CODE MACHINE de la cause de refus, exposé À CÔTÉ du texte sur /data
// ET sur le WebSocket. Le client traduit un code par une table ; il ne cherche
// plus de fragment dans une phrase française (ADR-0049).
//   0   (kDoseBlockedCodeNone)  = aucun refus → publié `null` ;
//   1..22                       = valeur de `DoseRefusal` (contrat FIGÉ) ;
//   250 (kDoseBlockedCodeGuard) = cause posée par une garde de sécurité pH/ORP
//                                 (message à valeurs runtime → lire le TEXTE).
uint8_t getPhDoseBlockedCode() const;
uint8_t getOrpDoseBlockedCode() const;
// feature-138 (v2.54.0) — durée (s) depuis laquelle le code ci-dessus est
// inchangé, par voie. Bornée à kDoseBlockedForMaxS (24h), repliement millis()
// traité (module pur dose_block_duration_logic.h). Voir section dédiée plus bas.
uint32_t getPhDoseBlockedForS() const;
uint32_t getOrpDoseBlockedForS() const;

// Butées de sécurité sur le pH BRUT : true = la butée mord pour ce produit.
// Exposées à /data ET au WebSocket (parité stricte) pour que l'UI puisse avertir
// avant une injection MANUELLE, qui contourne délibérément ces butées.
bool isPhRawGuardBlocked() const;
bool isOrpPhRawGuardBlocked() const;

// Caractérisation des pompes (itération 2, v2.21.0 — lecture seule, sûre depuis
// les handlers async). Index PHYSIQUE (0 = pompe 1, 1 = pompe 2).
uint8_t getPumpAmplitudeDuty(int pumpIndex) const;      // pumpNMaxDutyPct × 255/100
bool isPumpFlowUncharacterized(int pumpIndex) const;    // evaluatePulseGate != None (fail-closed si index invalide)
bool isPhPumpFlowUncharacterized() const;               // raccourci par CHIMIE (résout ph_pump)
bool isOrpPumpFlowUncharacterized() const;              // raccourci par CHIMIE (résout orp_pump)

// Calendrier d'impulsion exposé à l'UI/WS (itération 3, v2.21.1 — lecture seule,
// AUCUN setter, aucun effet dosage). Index par CHIMIE (chem 0 = pH, 1 = ORP).
bool isPulseSessionActive(int chem) const;         // séance ouverte (auto OU scheduled) ; false si index invalide
uint32_t getPulseMs(int chem) const;               // durée du pouls planifié (ms), 0 = période sans pouls (accumulation) ; 0 si index invalide
uint32_t getPulsePhaseMs(int chem, uint32_t nowMs) const;  // position dans la période, clampée [0..kPulsePeriodMs] ; 0 si session inactive

// v2.26.0 — TERME INTÉGRAL pH (ADR-0052), LECTURE SEULE. Aucun setter, aucune
// influence sur une décision de dosage. Publiés à l'IDENTIQUE sur /data, sur le
// WebSocket et sur MQTT (mêmes noms, mêmes arrondis, mêmes règles null).
// Course bénigne assumée (patron isAnyPumpEnergized) : écrits par loopTask, lus
// en contexte web async ; pire cas = instantané en retard d'un tour de loop.
float getPhIntegral() const;       // intégrale accumulée (pH·s, SIGNÉE) — NAN si non finie
float getPhIntegralTerm() const;   // ki × intégrale = contribution au débit demandé (mL/min)
float getPhIntegralPct() const;    // remplissage de l'anti-windup (% de integralMax, ABSOLU)

// feature-085 (v2.28.0) — DOSE DE BASE APPRISE (feedforward pH), LECTURE SEULE.
// Contrat de statut FIGÉ (0-4, cf. PhFfStatus). Même course bénigne assumée.
float getPhFeedforwardFlow() const;    // débit FF appliqué à la session auto (mL/min, 0 hors session)
uint8_t getPhFeedforwardStatus() const; // 0 inactif · 1 actif · 2 latch · 3 historique < 3 j · 4 quota
                                        // v2.46.0 : le 3 est traduit en 0 si l'anneau porte >= 3 journées

// feature-092 (v2.46.0) — COMPOSITION DU DÉBIT D'INJECTION, LECTURE SEULE.
// Copie sous spinlock portMUX : les cinq flottants d'une même composition sont
// cohérents entre eux. Aucun I2C, aucune NVS, aucun LittleFS — sûrs en async.
float getPhFeedforwardLearnedFlow() const;  // dose de base APPRISE (mL/min) — vraie hors session, NAN si < 3 j
float getPhPTermFlow() const;               // kp·erreur, SIGNÉ, 0 en bande morte, jamais borné
float getPhITermFlow() const;               // ki·intégrale SERVI (0 en bande morte) — != getPhIntegralTerm()
float getPhDemandFlow() const;              // clamp(PI servi + FF, 0, ampFlow)
float getOrpPTermFlow() const;              // idem ORP (P-pure)
float getOrpDemandFlow() const;             // demande ORP = terme P borné
float getOrpSustainableMlPerMin() const;    // miroir de getPhSustainableMlPerMin()
bool isOrpLimitedByHourlyBudget() const;    // VERDICT « chlore borné par sa limite horaire »

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
3bis. **v2.27.0** — `tickDailyFiltrationMinutes()` (dénominateur de la charge acide observée) puis le **flush périodique 30 s** des compteurs journaliers, tous deux **remontés ici, en amont de toutes les sorties anticipées** (OTA, maintenance, capteurs non initialisés, stabilisation). Voir [Points de flush ajoutés](#points-de-flush-ajoutés-défaut-fail-open-corrigé-en-revue).
4. **Court-circuit OTA** : `otaInProgress` actif → `applyPumpDuty(0,0)` + `applyPumpDuty(1,0)` puis `return`. Pompes coupées tant que l'OTA dure.
4bis. **Court-circuit maintenance** (v2.9.1) : `maintenanceInhibit` actif → `applyPumpDuty(0,0)` + `applyPumpDuty(1,0)` puis `return`. Gate identique à l'OTA, placée **avant** toute évaluation de dosage ([`pump_controller.cpp:798`](../../src/pump_controller.cpp:798)). Voir [Mode maintenance auto-guérison](#mode-maintenance-auto-guérison-v291).
5. Refresh des fenêtres glissantes 1 h (`refreshDosingState`).
6. Court-circuit capteurs : `!sensors.isInitialized()` → arrêt pompes.
7. Gate `canDose()` (cf. ci-dessous) → arrêt pompes si non autorisé (mais respecte `manualMode[i]` pour ne pas couper un test développeur en cours).
7bis. **v2.27.0** — `tickPhIntegralAuto()` : **entretien du terme intégral pH, hors de toute conjonction de dosage**, après `tickPhGuardSurveillance()` et après la garde de présence d'eau. Porte aussi la confirmation R4 d'une restauration NVS en attente. Voir [Cadence, mémoire et charge observée de l'intégrale pH](#cadence-mémoire-et-charge-observée-de-lintégrale-ph-v2270). **v2.28.0** — `tickPhFeedforward()` est évalué **immédiatement avant** ce tick (ordre impératif : l'anti-windup du PI retranche le débit FF de son soutenable dans le **même** tour). Voir [Dose de base apprise](#dose-de-base-apprise--feedforward-ph-v2280).
8. Pour pH puis ORP : calcul de l'erreur, anti-cycling start/stop, PID (demande de débit **moyen**), puis ordonnanceur d'impulsions (`evaluatePulseGate` → `planPulseWithCarry` (v2.22.0, [accumulateur sous-plancher](#accumulateur-sous-le-pouls-minimal-feature-073-v2220)) → `tickPulse`, [Dosage en proportionnel de temps](#dosage-en-proportionnel-de-temps-itération-2-v2210)), `applyPumpDuty()` (255 pendant le kick, amplitude calibrée sinon). *(`flowToDuty()`/`dutyToFlow()` supprimés en v2.21.0, [ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md).)*

## Mode maintenance auto-guérison (v2.9.1)

Piloté par la santé système ([system-health.md](system-health.md), [ADR-0029](../adr/0029-auto-guerison-heap.md)) : après trop de redémarrages d'auto-guérison dans la fenêtre glissante, `main.cpp` appelle `PumpController.setMaintenanceInhibit(true)` pour **suspendre entièrement le dosage** en restant joignable pour diagnostic.

Le flag `maintenanceInhibit` ([`pump_controller.h:53`](../../src/pump_controller.h:53)) ferme **toutes** les voies de dosage, à deux endroits complémentaires :

| Voie | Point de fermeture |
|---|---|
| Régulation **auto** + **scheduled** | Court-circuit en tête d'`update()` ([`pump_controller.cpp:798`](../../src/pump_controller.cpp:798)) — pompes forcées à 0 chaque cycle |
| Injection **manuelle web** (`/ph\|orp/inject/start`) | Refus **409** `maintenance_mode` dans `manualInjectGuardOrReject()` ([`web_routes_control.cpp:67`](../../src/web_routes_control.cpp:67)), **avant** d'armer `manualInject*.active` (sinon `creditManualInjectionFloor()` créditerait un volume jamais versé) |
| Test web (`/pumpN/on`, `/pumpN/duty/*`) | `setManualPump()` — point de passage **unique** de toute activation manuelle directe — court-circuité ([`pump_controller.cpp:1574`](../../src/pump_controller.cpp:1574)) : sans cette garde, `setManualPump()` énergiserait la pompe via `ledcWrite()` depuis la tâche web jusqu'au prochain `update()` |

> ⚠️ **Condition pool-chemistry C2** : l'inhibition doit couvrir **auto + scheduled + manuel web + test** (et toute future source d'activation). Fermer `update()` seul ne suffit pas — `setManualPump()` active la pompe **immédiatement** hors cycle `update()`, d'où la garde dédiée à ce point de passage unique. *(Jusqu'en v2.16.x, la voie « manuel UART » de l'écran externe était couverte au même point ; elle a disparu avec le retrait de l'écran en v2.17.0, feature-065.)*

Le flag ne se lève **qu'au redémarrage** du contrôleur (le mode maintenance est un état terminal jusqu'au reboot manuel). Logs : `warning` throttlé une fois par boot sur refus de commande manuelle ; `critical` à l'entrée en maintenance (côté `main.cpp`).

## Suspension « assistant de mesure du débit » (feature-075, v2.24.0)

Pendant une session de l'[assistant de mesure du débit](../features/page-settings.md#assistant--mesure-du-débit--feature-075-v2240) (routes [`/flow-assist/*`](../API.md#assistant-de-mesure-du-débit-des-pompes-feature-075-v2240)), le dosage **automatique et programmé** des **deux chimies** est suspendu — la pompe en assistant tourne à l'eau claire, tuyau potentiellement débranché : aucune régulation ne doit démarrer en parallèle.

- **Flag** : `_flowAssistSuspend` (`volatile bool`, [`pump_controller.h`](../../src/pump_controller.h) — patron identique à `maintenanceInhibit`) posé/levé par `setFlowAssistSuspend()` depuis les handlers web async (et le tick de timeout en loopTask). Écriture d'un octet atomique sur Xtensa, course bénigne (prise en compte au pire différée d'un tour de loop), **fail-closed côté suspension**.
- **Fermeture par la garde structurelle existante** ([`pump_controller.cpp`](../../src/pump_controller.cpp), branche `!filtrationOk || stabilizationActive || flowAssistSuspend`) — **aucune duplication de chemin** : mêmes garanties que la perte de filtration (états de dosage auto + scheduled refermés `dosingState.active = false`, pompes **non manuelles** forcées à 0, drapeau de session pouls périmé absorbé à la reprise). Le mode **MANUEL** — véhicule de l'assistant (purge/mesure via le chemin `manualInject*`) — reste pleinement fonctionnel.
- **Log one-shot par transition** (jamais de spam par tour, état précédent `_flowAssistSuspendPrev` loopTask-only) : `INFO Dosage automatique suspendu (assistant de mesure du débit)` / `INFO Dosage automatique rétabli (fin de l'assistant de mesure du débit)`.

### Timeout serveur et réconciliation fail-closed

Un assistant abandonné (onglet fermé, WiFi tombé) ne laisse **jamais** la régulation suspendue : la session expire après `kFlowAssistSuspendTimeoutMs` = **15 min** sans requête `/flow-assist/*` (`lastTouchMs` réarmé à chaque requête, expiration wrap-safe — prédicat pur `isFlowAssistSessionExpired()`, [`src/flow_assist_logic.h`](../../src/flow_assist_logic.h)). Le tick (`updateManualInject()`, loopTask) arrête défensivement la marche éventuelle, lève la suspension et journalise `WARN [Assistant débit] Session expirée (15 min sans interaction) — dosage automatique rétabli`.

Deux protections contre les courses timeout (loopTask) / handlers (async), relevées en revue :

1. **Réconciliation fail-closed au tick** : une session active **exige** la suspension — si `sFlowAssist.active && !isFlowAssistSuspended()` (le `setFlowAssistSuspend(false)` du timeout a écrasé le `true` d'un `start` concurrent), la suspension est **re-posée**, idempotent. Fenêtre résiduelle ≈ 1 tour de loop, bénigne (aucun pouls significatif armable en un tour).
2. **Invariant de `/flow-assist/exit` : session fermée AVANT la levée de la suspension.** Dans l'ordre inverse, la réconciliation (1) pourrait s'intercaler entre la levée et le reset, re-poser la suspension, puis le reset effacerait la session → **suspension collée définitivement** (le timeout exige `active`, plus rien ne la lèverait).

### Comptabilité sous suspension (P4) et refus `assist_active`

Sous suspension flow-assist, `update()` sort par le chemin perte-filtration **avant** `accountPumpVolume()` → la comptabilité **par tick** est sautée pendant toute la marche. Conséquences (condition gate 6 + anomalie P4 trouvée en revue) :

- **Fin naturelle** : le crédit-plancher à échéance (`creditManualInjectionFloor`) est **LE** mécanisme de crédit — armé au `start` avec le volume **majorant** `flow255 × durée` (surestimer est fail-safe : le registre journalier ne sous-compte jamais, et `daily_limit` est évaluée sur ce majorant).
- **Arrêt anticipé** (`stop`, `exit`, timeout — `flowAssistStopRun()`) : pompe coupée immédiatement, `creditMl` ramené au **majorant prorata** du temps écoulé puis `durationMs` raccourci à `elapsed` — la « fin naturelle » du prochain tour de loop exécute le crédit dans le seul contexte autorisé (ordre d'écriture voulu : `creditMl` **avant** `durationMs`, course bénigne).
- **Interruption par perte de filtration** (`flowAssistCreditAbortedRun()`) : hors assistant, l'abandon historique ne crédite pas (l'intégration par tick a déjà compté) — **strictement inchangé**. Marche d'assistant : crédit **majorant prorata** appliqué directement (on est en loopTask), sinon le volume réellement pompé ne serait **jamais** inscrit (violation P4 : jamais zéro sur une pompe alimentée) + drapeau `aborted` posé pour que l'UI ne présente jamais une mesure partielle comme réussie.
- **Injections manuelles classiques refusées** (`409 assist_active`, contre-visite gate) : `/ph|orp/inject/start` est fermé pendant la session — une injection classique sur l'**autre** chimie, interrompue par perte de filtration, serait créditée 0 (le crédit prorata ci-dessus est réservé aux marches d'assistant). Les routes de test `/pumpN/on?duty=` ne sont pas concernées (pas de crédit plancher).

### Constantes (`kFlowAssist*`, [`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---|---|
| `kFlowAssistPurgeDurationS` | 60 s | Durée de purge, à duty **255 impérativement** (seul débit connu d'usine → volume résiduel poussé au bassin bornable) |
| `kFlowAssistMeasureDurationS` | 120 s | Marche de mesure à l'amplitude **exacte** d'exploitation — 120 s et non 60 s pour doubler le volume lu (~50 mL) et halver l'erreur de lecture relative |
| `kFlowAssistMaxPurgesPerSession` | 3 | Borne le volume envoyé au bassin et le temps hors régulation (3 purges sans amorçage = problème matériel) |
| `kFlowAssistSuspendTimeoutMs` | 900 000 ms (15 min) | Timeout de session/suspension, réarmé à chaque requête `/flow-assist/*` |

Le verdict pur `canStartFlowAssistPhase()` (quota de purges, marche déjà en cours) et le prédicat d'expiration vivent dans le header **sans dépendance Arduino** [`src/flow_assist_logic.h`](../../src/flow_assist_logic.h) (pattern ADR-0017) — 25 tests natifs `test/test_native_flow_assist/`.

## Mise en sécurité `pumpUsable` (feature-063, v2.15.0 ; **actif en production depuis la v2.20.4**)

Le PWM des pompes emprunte **un chemin unique** : `begin()` appelle `ledcAttachChannel(pin, PUMP_PWM_FREQ, PUMP_PWM_RES_BITS, channel)` puis `ledcWriteChannel()` — canaux 0/1 explicites.

> 🔒 **Invariant de sécurité chimique** : le PWM est cadencé à **20 kHz sur 8 bits** (`PUMP_PWM_FREQ` / `PUMP_PWM_RES_BITS`), **l'échelle du duty est 0-255 et ne change pas**. Toute modification de la fréquence ou de la résolution modifierait le volume injecté à consigne constante et exige un passage par `pool-chemistry`.

*(Historique : de la v2.15.0 à la v2.42.0, ce chemin était sous garde `#if ESP_ARDUINO_VERSION_MAJOR >= 3`, la branche `ledcSetup`/`ledcAttachPin`/`ledcWrite` servant au repli core 2. Ce repli a été retiré en v2.42.1 — [ADR-0069](../adr/0069-retrait-repli-core-2.md) — sans aucun changement du binaire produit : la branche core 2 n'était plus compilée depuis la bascule production de la v2.20.4.)*

Point important : contrairement à `ledcSetup()`, `ledcAttachChannel()` **retourne un bool**. Prescription **P1 pool-chemistry** (GO sous conditions, toutes implémentées) — échec d'attache = **pompe inutilisable, fail-closed** :

- `pumpUsable[i]` (`volatile bool`, [`pump_controller.h`](../../src/pump_controller.h) — patron identique à `maintenanceInhibit`) passé à `false` **uniquement** dans `begin()` sur échec d'attache, **jamais réarmé à chaud** (reboot requis). Log **CRITICAL** « pompe déclarée INUTILISABLE jusqu'au reboot ».
- Le GPIO est **tenu LOW** (le `pinMode(OUTPUT)` + `digitalWrite(LOW)` précèdent la tentative d'attache) — pompe physiquement arrêtée.
- **Inhibition par pompe** à deux points complémentaires : garde en **première ligne** d'`applyPumpDuty()` (avant le cache `pumpDuty` et toute mutation → `pumpDuty[i]` reste 0 à vie, aucun `ledcWriteChannel` sur canal non attaché) + refus dans `setManualPump()` (défense en profondeur, `warning` throttlé une fois par boot).
- L'autre pompe reste pilotable (inhibition **par pompe**, pas globale).

### Accesseur `isPumpUsable()` (feature-064, v2.16.0)

```cpp
bool isPumpUsable(int index) const;  // 0 = pH, 1 = ORP/chlore
```

Lecture seule **STRICTE** de l'utilisabilité de la pompe ([`pump_controller.h`](../../src/pump_controller.h), impl. [`pump_controller.cpp:123`](../../src/pump_controller.cpp:123)) — **aucun setter**, l'accesseur ne doit JAMAIS servir à réactiver une pompe (reboot requis pour réarmer). Sémantique **unique**, sans condition de plateforme :

- reflète `pumpUsable[]`, posé à `false` au boot sur échec `ledcAttachChannel` ;
- index hors bornes (≠ 0/1) → **`false`** (fail-closed).

> Jusqu'à la v2.42.0, l'accesseur retournait **toujours `true`** quand le firmware était compilé pour le repli core 2 — le drapeau `pumpUsable[]` n'existait alors pas. Une pompe y était donc **réputée utilisable sans vérification**. Ce comportement n'existe plus depuis le retrait du repli ([ADR-0069](../adr/0069-retrait-repli-core-2.md)) : le durcissement est unilatéral, aucun chemin n'est assoupli.

### Remontée MQTT `pump_unusable` (feature-064, v2.16.0 — exigence P1 livrée)

✅ La **recommandation P1 pool-chemistry** (« le CRITICAL local ne suffit pas en production — alerte MQTT dédiée obligatoire avant toute bascule production core 3 », condition de la porte de bascule [ADR-0034](../adr/0034-migration-core3-idf5.md)) est **livrée** : `MqttManager::connectInTask()` lit `PumpController.isPumpUsable(i)` et publie une alerte `pump_unusable` **par pompe inutilisable**, **à chaque (re)connexion MQTT** (arbitrage Q1 : l'alerte est non-retained et l'état permanent jusqu'au reboot — un broker absent au boot ne la perd plus). Aucun chemin d'écriture nouveau vers `pumpUsable[]` ni vers les pompes. Voir [`docs/MQTT.md`](../MQTT.md#alertes) et [`mqtt-manager.md`](mqtt-manager.md). L'alerte est **armée sans condition** : elle l'est en production depuis la bascule core 3 de la v2.20.4 ([ADR-0042](../adr/0042-core3-environnement-par-defaut.md)), et depuis le retrait du repli core 2 en v2.42.1 ([ADR-0069](../adr/0069-retrait-repli-core-2.md)) il n'existe plus de configuration de build où elle serait dormante.

## Algorithme (résumé)

> 🧭 **Vue d'ensemble narrative** de la chaîne complète (pH ET ORP, de la mesure à l'observabilité, avec les ADR) : [regulation-vue-d-ensemble.md](regulation-vue-d-ensemble.md) — commencer là avant de plonger dans les sections détaillées de ce fichier.

0. **Gardes de sécurité pH** (feature-067, refondues en v2.25.0) — évaluées **avant** `canDose()` : refus de dosage pH et inhibition ORP, sur le pH **corrigé** pour les seuils chimiques et sur le pH **brut** pour les deux butées de dernier recours. Voir [Gardes de sécurité pH](#gardes-de-sécurité-ph-refonte-v2250).
1. **Gate `canDose(int pumpIndex)`** — fail-closed strict (feature-021, validé pool-chemistry). Voir [Garde-fous `canDose`](#garde-fous-candose) ci-dessous pour la liste complète des 10 conditions évaluées dans l'ordre.
2. **Calcul de l'erreur** ([`pump_controller.cpp:432`](../../src/pump_controller.cpp:432)) — deux modes exclusifs sélectionnés via `mqttCfg.phCorrectionType` :

   | Mode | Formule erreur | Direction du dosage |
   |------|----------------|---------------------|
   | `ph_minus` (acide, défaut) | `error = pH_mesuré − phTarget` | dose si `error > 0` (pH trop haut) |
   | `ph_plus` (base) | `error = phTarget − pH_mesuré` | dose si `error > 0` (pH trop bas) |
   | ORP | `error = orpTarget − ORP_mesuré` | dose chlore si `error > 0` (ORP trop bas) |

   > **feature-067 (v2.20.0)** : `pH_mesuré` est la valeur **filtrée et corrigée** (`getPhFiltered()` = brut + `ph_offset`). La consigne `phTarget` est donc comparée au pH **corrigé** — le pH réellement visé dans le bassin est `phTarget − ph_offset`.

3. **Régulation temporisée** (feature-025, [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) ; **PI côté pH depuis la v2.26.0**, [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) : `computePID(pid, error, now, deadband, freezeIntegral)`.

   | Grandeur | Kp | Ki | Kd | `integralMax` |
   |----------|----|----|----|---|
   | pH — `slow` | 4 | 1,389e-4 (Ti = 8 h) | 0 | 43 200 pH·s |
   | pH — `normal` (défaut) | 8 | 3,704e-4 (Ti = 6 h) | 0 | 16 200 pH·s |
   | pH — `fast` | 12 | 8,333e-4 (Ti = 4 h) | 0 | 7 200 pH·s |
   | ORP (`slow`/`normal`/`fast`) | 0.15 / 0.3 / 0.5 | **0** | 0 | (inerte) |

   - **`Kd = 0` IMPÉRATIF** sur les deux chimies — un terme dérivé amplifierait le bruit résiduel après filtrage (incompatible avec un système hydraulique lent).
   - **`Ki_ORP = 0` inchangé** — l'ORP dépend du pH, de la T° et du stabilisant ; son droop n'est pas le défaut diagnostiqué. Le lever exigerait un nouvel ADR.
   - **`Ki_pH` levé en v2.26.0** — voir [Terme intégral pH : levée de `Ki = 0`](#terme-intégral-ph--levée-de-ki--0-v2260). `kp`, `ki` **et** `integralMax` sont posés ensemble par `applyRegulationSpeed()` depuis les constantes `kPhKp*` / `kPhKi*` / `kPhIntegralMax*` : ils ne peuvent plus divorcer silencieusement.
   - La temporisation vient de la **pause mélange** (point 6) et de l'anti-cycling : on dose un pulse court, puis on attend l'homogénéisation avant de réévaluer.
   - Normalisation d'erreur (bornée) : `kPhMaxError = 1.0`, `kOrpMaxError = 200 mV`. `dt` PID plafonné : deltas > 10 s ignorés.

   **Anti-windup** : `computePID()` reçoit `freezeIntegral` — l'intégrale est **gelée** (aucune accumulation) si l'un des cas suivants est vrai : filtre non prêt, `canDose() == false`, pause mélange active, sortie PID saturée, erreur dans la zone morte, ou mesure rejetée / capteur instable. **v2.26.0** : côté pH, `freezeIntegral` est passé **inconditionnellement à `true`** — l'intégrale appartient désormais au module pur `ph_integral_logic`, qui l'entretient une fois par tick ; `computePidPure` ne fait plus que **servir** le terme acquis (`kp·e + ki·I`, borné). Laisser `computePidPure` intégrer une seconde fois compterait le même `dt` deux fois, et le ferait sur `error` au lieu de `error − stopThreshold`.

   > Le **cœur du calcul** (terme proportionnel, anti-windup, plancher 0, bornage final min/max) est extrait dans la fonction pure `computePidPure` testée en natif. `computePID` n'est plus qu'une coquille qui gère `dt`/`millis()` et l'état PID. Voir [Calcul proportionnel pur (`computePidPure`)](#calcul-proportionnel-pur-computepidpure).

   **Zone morte** : le paramètre `deadband` passé à `computePID()` est le **seuil de démarrage existant** (`phStartThreshold` / `orpStartThreshold`) — **un seul** deadband fait foi, aucun second seuil dupliqué. Si `|error| < deadband` → sortie forcée à 0 **et** intégrale figée.
   >
   > ⚠️ **v2.26.0 — côté pH, la bande morte suit désormais l'HYSTÉRÉSIS.** Elle valait *toujours* `phStartThreshold` (0,05), y compris pendant une session **déjà ouverte**, alors que `shouldContinueDosing` poursuit jusqu'à `phStopThreshold` (0,01) : la plage `]0,01 ; 0,05]` était une **session vivante à débit nul**, et le seuil d'arrêt réellement appliqué était 0,05. La valeur passée est maintenant `phWasActiveTick ? phStopThreshold : phStartThreshold`. Le côté ORP est **inchangé**.
   >
   > ⚠️ **v2.36.0 — côté pH, ce choix de bande morte n'est plus écrit ici.** Il est rendu **par le même appel** que le verdict de porte (`phSessionGate()`, [Porte de session pH robuste au bruit](#porte-de-session-ph-robuste-au-bruit-feature-094-v2360)) : un seul point de vérité, pour que bande morte et hystérésis ne puissent plus diverger. **Les valeurs et le rôle sont inchangés** (déplacement du point d'équilibre = 0,000 pH) ; ce qui change, c'est **le signal qui décide de la porte** — une moyenne glissante de 10 min, pas la filtrée instantanée. P, I et feedforward continuent de lire la **filtrée actuelle**.
4. **Anti-cycling** ([`config.h:152`](../../src/config.h:152) `PumpProtection`) — **en dur, non configurables via UI** (exception : `maxCyclesPerDay`, configurable borné depuis feature-060) :
   - `minInjectionTimeMs = 30000` — 30 s minimum par injection démarrée.
   - Hystérésis de seuils (démarrage / arrêt) :

     | Grandeur | Deadband | Seuil démarrage (= zone morte feature-025) | Seuil arrêt |
     |----------|----------|-----------------|-------------|
     | pH | `PH_DEADBAND = 0.01` | `phStartThreshold = 0.05` | `phStopThreshold = 0.01` |
     | ORP | `ORP_DEADBAND = 2.0 mV` | `orpStartThreshold = 15 mV` (relevé de 10, feature-025) | `orpStopThreshold = 2 mV` |

   > feature-025 : `phStartThreshold` / `orpStartThreshold` servent désormais aussi de **zone morte PID** (passés comme `deadband` à `computePID`). ORP relevé de 10 → 15 mV pour élargir la zone morte (l'ORP étant plus bruité et dépendant du pH/T°/stabilisant).

   - `maxCyclesPerDay = 24` (défaut, **abaissé de 48 → 24 en v2.20.5**) — démarrages comptés dans une fenêtre 24 h glissante. **Configurable** via `/save-config` (`max_cycles_per_day`), borné en dur [10-60] — voir [Limite de cycles/jour configurable](#limite-de-cyclesjour-configurable-garde-9-feature-060-v2120).
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

### Alerte « plafond journalier atteint J jours consécutifs » (v2.68.0, feature-152, condition #6 du gate)

Avant la remise à zéro de `phLimitReached`/`orpLimitReached` (branche NTP ci-dessus, chemin
**daté uniquement** — le fallback 24 h n'alimente pas cette streak), la coquille compte les
jours calendaires consécutifs où le plafond a été atteint, via le module pur
[`src/daily_limit_streak_logic.h`](../../src/daily_limit_streak_logic.h)
(`evaluateDailyLimitStreak()` — « une alerte non testée est une promesse », motif du gate) :

```cpp
static uint8_t sPhDailyLimitStreakDays = 0;
static uint8_t sOrpDailyLimitStreakDays = 0;
const DailyLimitStreakResult phStreakResult = evaluateDailyLimitStreak(
    safetyLimits.phLimitReached, sPhDailyLimitStreakDays, kDailyLimitStreakAlertDays);
```

- **Alerte seulement, aucun nouveau verrou** : `systemLogger.critical()` + `mqttManager.publishAlert("ph_daily_limit_streak", …)` / `("orp_daily_limit_streak", …)` dès que la streak atteint `kDailyLimitStreakAlertDays` (= **3**). Ne bloque rien, ne modifie aucune décision de `canDose()`.
- **Motif** : sous les deux hypothèses possibles (sonde qui ment / eau réellement mauvaise),
  poursuivre le goutte-à-goutte au-delà de 3 jours est faux dans les deux cas — et sous le
  seuil de redox, il **fabrique des chloramines** (épisode de boue d'octobre 2026). C'est la
  borne qui manquait, et elle **ne juge pas la mesure**.
- ⚠️ **Le compteur n'est PAS persisté** — `static` locales à `pump_controller.cpp`, pas de NVS :
  il repart à **0** à chaque redémarrage de l'appareil. Un appareil qui redémarre plus souvent
  que tous les `kDailyLimitStreakAlertDays` (3) jours **n'atteindra jamais** ce seuil, et cette
  alerte précise ne se lèvera jamais sur un tel appareil. **Accepté sans persistance** (lot de
  retrait, aucun mécanisme ajouté au-delà de l'extraction demandée) : (1) le plafond journalier
  **lui-même** reste signalé chaque jour, redémarrage ou non — c'est seulement l'**escalade**
  « plusieurs jours de suite » qui peut manquer ; (2) un appareil qui redémarre tous les 1-2
  jours a un problème distinct (reboot-loop) déjà couvert par d'autres diagnostics
  (ADR-0029 et suivants) ; (3) persister ajouterait une écriture NVS quotidienne pour un strict
  gain de visibilité, pas de sécurité — si un usage réel montre que des redémarrages fréquents
  masquent cette alerte, c'est le sujet d'une spec dédiée, pas un ajout silencieux ici.
- Tests natifs : [`test/test_native_daily_limit_streak/`](../../test/test_native_daily_limit_streak/).

## Garde-fous `canDose`

`canDose(int pumpIndex)` (feature-021, validé pool-chemistry — voir [ADR-0014](../adr/0014-migration-atlas-ezo.md)) est appelée à chaque cycle `update()` pour la pompe pH (index 0) puis ORP (index 1), **avant** tout calcul PID ou démarrage d'injection. La fonction est **fail-closed strict** : tout résultat ambigu retourne `false` (refus de dosage).

Les conditions sont évaluées dans l'ordre suivant. Le premier `false` rencontré court-circuite les suivants et logue la cause **edge-triggered** (1 entrée info par transition de cause, pas de spam). La dernière cause de refus est **exposée à `/data` et au WS** sous **deux formes** : le **texte** via `copyPhDoseBlockedReason()` / `copyOrpDoseBlockedReason()` (chaîne vide si autorisé) et, depuis la v2.25.1, le **code machine** via `getPhDoseBlockedCode()` / `getOrpDoseBlockedCode()` (voir [Causes de refus](#causes-de-refus-code-machine--string-fr)).

| # | Condition | Refus si | Cause documentée |
|---|-----------|----------|------------------|
| 1 | **Index pompe valide** | `pumpIndex` ∉ {0, 1} | bug interne |
| 2 | **Watchdog actif** | wdt non initialisé | sécurité hardware |
| 3 | **Présence d'eau** | `filtration.resolveWaterPresence().waterPresent == false` | eau ne circule pas — risque bouchon doseuse / injection en eau stagnante. **Source unique** selon le mode d'installation (feature-056, voir [Présence d'eau selon le mode d'installation](#présence-deau-selon-le-mode-dinstallation-feature-056)) |
| 4 | **Valeur officielle non NaN** | `getPhFiltered()` ou `getOrpFiltered()` = NaN | feature-152 (v2.68.0) : le PID consomme la valeur officielle (**corrigée** côté pH depuis feature-067) ; NaN = warmup non amorcé / sonde figée / mesure périmée / porte fermée. La cause exposée est **qualifiée** en 4 codes distincts (15-18) selon l'état interne du filtre — **seule garde restante** depuis feature-152, qui a retiré les gardes 4b/4c ci-dessous |
| ~~4b~~ | ~~Filtre prêt~~ (feature-025) | — | **RETIRÉE en v2.68.0** (feature-152) — `isPhFilterReady()`/`isOrpFilterReady()` n'existent plus : `isnan()` de la garde 4 ci-dessus encapsule désormais directement warmup, sonde figée, péremption et porte fermée |
| ~~4c~~ | ~~Capteur stable~~ (feature-025) | — | **RETIRÉE en v2.68.0** — `isPhFilterUnstable()`/`isOrpFilterUnstable()` n'existent plus : la médiane écarte les aberrations par construction, sans jugement d'instabilité séparé (verdict `pool-chemistry` : ce mécanisme se posait sur du bruit normal, il n'inhibait pas un danger) |
| 5 | **EZO calibré** | `cal_points < 2` (pH) ou `< 1` (ORP). `-1` (bus down) bloque. | pool-chemistry **cond #2** : régulation auto inhibée tant que calibration incomplète |
| 6 | **Pas de stabilisation post-cal** | `isStabilizationTimerActive(pumpIndex)` = true | pool-chemistry **cond #3** : 5 min pH / 3 min ORP après calibration EZO |
| 6b | **Pas de pause mélange** (feature-025) | `isPhMixingDelayActive()` / `isOrpMixingDelayActive()` | homogénéisation du bassin après injection (15 min pH / 20 min ORP) — gate **indépendante** de la stabilisation post-cal |
| 7 | **Mode régulation = `automatic`** | mode `manual` ou `scheduled` | la branche `scheduled` a sa propre logique, ne passe pas par `canDose` |
| 8 | **Limite journalière non atteinte** | `dailyXxxInjectedMl >= maxXxxMlPerDay` | sécurité chimique config |
| 9 | **Limite horaire non atteinte** | `usedMs >= xxxLimitMinutes × 60000` dans la fenêtre 1 h glissante (limite à 0 = désactivée) | sécurité chimique config |
| 10 | **Anti-rafale court terme** (Pass 3.5) | > 6 cycles/min OU > 20 cycles/15 min | anti-emballement PID — voir [Ring buffer anti-rafale](#ring-buffer-anti-rafale) |

> **Conditions #1, #2, #3, #5, #6** issues du checklist `pool-chemistry` validé en cadrage feature-021. **Condition #10** ajoutée en correctif Pass 3.5. **Condition #4 (valeur officielle), #6b** ajoutées en feature-025 — toutes **fail-closed** et **prioritaires** : elles s'ajoutent aux gardes existantes sans les contourner. **#4b et #4c retirées en v2.68.0** (feature-152) : leur rôle est entièrement couvert par #4 depuis que la valeur officielle encapsule directement warmup, sonde figée, péremption et porte fermée.
>
> **L'ordre et le contenu de `canDose()` sont INCHANGÉS par feature-067 comme par la refonte v2.25.0.** Les gardes pH sont évaluées **en amont**, jamais à l'intérieur : aucune valeur d'énum `DoseRefusal` n'est ajoutée, aucune garde existante n'est déplacée. feature-152 (v2.68.0) retire les gardes #4b/#4c **sans déplacer** les autres — leur numérotation reste un historique de revue, pas un contrat figé comme celui de `DoseRefusal`.

## Gardes de sécurité pH (refonte v2.25.0)

Décision, alternatives écartées et asymétries assumées : [ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

L'offset logiciel pH ([ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) **déplace l'entrée de la régulation** : `canDose(0)`, le calcul d'erreur et le PID consomment `getPhFiltered()`, c'est-à-dire la valeur **corrigée** (`brut + ph_offset`). Les gardes de cette section encadrent ce risque, en amont de `canDose()` dans `update()` : quand une garde bloque, `canDose()` n'est pas évalué et `_lastRefusalCause` porte la cause de la garde.

### Le principe : deux référentiels, jamais mélangés

Jusqu'à la v2.25.0, le plancher de refus était **asservi à l'offset** (`phRawFloorFor()`) et comparé au pH **BRUT**. Ce mécanisme est **supprimé** — il était juste mais incompréhensible (le seuil affiché bougeait avec l'offset) et il a lui-même désactivé la régulation en production le 25/08/2026.

Les seuils sont désormais **FIXES**, **dérivés** de la plage de consigne, et comparés au pH **CORRIGÉ** — celui que l'utilisateur voit à l'écran. Justification : l'offset est une correction de **mesure** ; une fois appliqué, la valeur *est* le pH de l'eau, et un seuil chimique (agressivité, fraction de HClO) porte sur l'eau réelle.

Le pH brut ne garde qu'un rôle : **deux butées de dernier recours**, qui doivent rester valides si l'offset est faux. Elles lisent `sensors.getPhValueRaw()` (ex-`getPhFilteredRaw`, renommé feature-152), seul accesseur portant la garde d'âge du filtre ([sensors.md](sensors.md#garde-dâge-de-getphvalueraw--pourquoi-elle-existe)). Le pH corrigé est **recalculé** ici — `applyPhOffset(getPhValueRaw(), offset)` — et non lu via `getPhFiltered()`, dont le contrat est celui de `/data`, du WS et de MQTT.

| Grandeur | Valeur | Référentiel | Dérivation (`constants.h`) |
|---|---:|---|---|
| Consigne pH | `7,20` → `7,50`, pas `0,10` | corrigé | `kPhTargetMin` / `kPhTargetMax` / `kPhTargetStep` (source unique) |
| Refus d'**ACIDIFIER** (`ph_minus`) | `6,70` | corrigé | `kPhFloorCorrected = kPhTargetMin − kPhSafetyMargin` (0,50) |
| Refus d'**ALCALINISER** (`ph_plus`) | `7,60` | corrigé | `kPhCeilingCorrected = kPhTargetMax + kPhHighMargin` (0,10) |
| Inhibition du dosage ORP | `7,60` (relâche ≈ 7,55) | corrigé | `kPhOrpInhibit` — littéral à justification chimique propre |
| Avertissement d'eau acide, **non bloquant** | `7,00` | corrigé | `kPhLowWarn` — littéral à justification chimique propre |
| **Butée basse** | `6,20` | **brut** | `kPhFloorRawHard = kPhFloorCorrected − kPhOffsetMax` |
| **Butée haute** | `8,10` | **brut** | `kPhCeilingRawHard = kPhOrpInhibit + kPhOffsetMax` |
| Offset pH | `−0,50` → `+0,50`, pas `0,10` | — | `kPhOffsetMin` / `kPhOffsetMax` / `kPhOffsetStep` |
| Hystérésis de relâche | `0,05` | — | `kPhGuardReleaseHysteresis`, commune à **toutes** les gardes |

**Aucun seuil n'est écrit en littéral**, hors les deux exceptions ci-dessus. Le calcul est délégué au module **pur** [`src/ph_guard_logic.h`](../../src/ph_guard_logic.h) — deux comparateurs symétriques avec hystérésis (`phGuardBlocksBelow` / `phGuardBlocksAbove`), pattern Humble Object [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), tests natifs dans `test/test_native_ph_guard/`.

#### Pourquoi les butées brutes valent exactement 6,20 et 8,10

Chaque butée est placée à **exactement l'offset maximal** de son seuil corrigé. Conséquence : à l'offset extrême, butée et seuil mordent **au même point** et se relâchent ensemble — aucune ne peut rendre l'autre inopérante.

| Offset | Butée basse 6,20 [BRUT] équivaut à | Seuil bas 6,70 [CORRIGÉ] | Verdict |
|---:|---:|---:|---|
| `+0,50` | `6,70` corrigé | `6,70` | mordent ensemble |
| `0,00` | `6,20` corrigé | `6,70` | le seuil corrigé mord d'abord |
| `−0,50` | `5,70` corrigé | `6,70` | le seuil corrigé mord d'abord |

> Une butée **plus haute** (6,40 avait été proposée) mordrait AVANT le seuil corrigé pour un offset **légitime** de `+0,50` (6,40 brut = 6,90 corrigé > 6,70), rendant l'offset inopérant et réintroduisant par la bande la dépendance qu'on venait de supprimer. Le `static_assert` est écrit en **inégalité** : la remonter ne compile plus, l'abaisser reste possible.

#### Invariants verrouillés à la compilation

Trois `static_assert` de [`constants.h`](../../src/constants.h) — toute retouche d'une valeur oblige à relire la justification chimique, pas seulement à recompiler :

1. `kPhTargetMax + kPhOrpInhibitMargin <= kPhOrpInhibit` — la consigne haute ne peut **pas atteindre** le seuil d'inhibition ORP. Sinon, une eau maintenue à la consigne inhiberait le chlore **en permanence** : panne sanitaire silencieuse.
2. `kPhCeilingCorrected <= kPhOrpInhibit` — le plafond d'alcalinisation ne peut **pas dépasser** l'inhibition ORP. Sinon l'appareil alcaliniserait une eau dont il vient de décider que le chlore y est inactif. Verrou **dédié** : celui de `kPhOrpInhibitMargin` ne protège que la consigne, pas le plafond.
3. `kPhFloorRawHard <= kPhFloorCorrected − kPhOffsetMax` et son symétrique haut — chaque butée brute ne peut **pas mordre avant** son seuil corrigé, pour tout offset du domaine.

S'y ajoutent les gardes de cohérence : bornes non inversées, marges strictement positives, `kPhLowWarn` compris entre le seuil bas et la consigne minimale, hystérésis non nulle et plus petite que la plus petite marge.

### Surveillance INCONDITIONNELLE, séparée de la décision

Défaut corrigé en v2.25.0 : la détection vivait **à l'intérieur** de la conjonction d'éligibilité `phMode == "automatic" && phLimitOk && … && !isPhGuardBlocking(...)`. Le court-circuit `&&` la rendait muette dès qu'un terme amont était faux — quota journalier épuisé, régulation désactivée, **mode `manual`**, c'est-à-dire l'état exact où l'escalade laisse l'appareil. La seule alerte désignant la vraie cause ne partait jamais quand elle était le plus nécessaire.

| | `tickPhGuardSurveillance()` / `tickOrpPhInhibitSurveillance()` | `isPhGuardBlocking(bool countRefusal)` / `isOrpInhibitedByPh()` |
|---|---|---|
| **Appel** | **inconditionnel**, 1 fois par tick d'`update()`, hors de toute conjonction | sous conjonction d'éligibilité |
| **Rôle** | franchissements, hystérésis, logs `warn`/`critical`, alertes MQTT, cause exposée | rend le verdict du tick + ce qui relève d'un refus **réel** |
| **Compte / escalade** | **jamais** (un appareil laissé en manuel s'auto-escaladerait sinon) | compteur 24 h + bascule fail-closed, en mode `automatic` seulement |
| **Coût** | lectures de **caches** capteur uniquement, aucune transaction I²C | — |

L'ordre surveillance → décision est garanti **dans le même tick** : aucune décision n'est prise sur un verdict périmé. Un pH qui s'effondre produit donc son `warn` à 7,00, son `critical` à 6,70 et ses alertes MQTT **même quota journalier épuisé et même en mode `manual`**.

> ⚠️ **Les indicateurs exposés GÈLENT pendant les sorties anticipées d'`update()`** (OTA, mode maintenance, capteurs non initialisés, **filtration arrêtée / eau absente**, stabilisation post-calibration, assistant de mesure du débit). La surveillance n'y tourne pas : `phRawGuardBlocked`, `orpPhRawGuardBlocked` et les causes de refus conservent la valeur du **dernier tick complet**. Voulu — en eau stagnante la mesure n'est pas représentative du bassin, surveiller là produirait de fausses alertes, et l'appareil n'y dose rien. Conséquence pratique : après un arrêt de filtration, l'encart d'avertissement d'injection manuelle peut refléter un état antérieur.

> ⚠️ **Dette résiduelle** : l'alerte de refus en mode **`programmé`** (`ph_guard_scheduled`) vit encore sous une conjonction d'éligibilité — même classe de défaut que celui corrigé ici. Impact faible : la surveillance couvre la **détection** (franchissement, `critical`, alerte `ph_guard`), seul le signalement périodique peut manquer.

### 1. Refus de dosage pH (`isPhGuardBlocking`)

Le seuil appliqué dépend du **produit déclaré** (`phCorrectionType`) : un acide ne peut violer qu'un seuil bas, une base qu'un seuil haut.

⚠️ **Asymétrie assumée entre produits depuis la v2.55.1.** Côté **base** (`ph_plus`), la butée brute reste évaluée en premier et prime inconditionnellement dans le journal et dans la cause exposée (comportement historique, inchangé). Côté **acide** (`ph_minus`), la priorité est désormais **discriminante** — voir [ci-dessous](#priorité-des-causes-pour-lacide-discriminante-depuis-v2551). Les deux chemins ne sont pas symétriques : seul le chemin acide a été corrigé, parce que c'est lui qui a produit l'incident du 2026-10-05 (bug-135). Harmoniser le chemin base sur la même règle n'est pas fait et resterait à instruire via `pool-chemistry` si un incident analogue s'y produisait.

| Produit | Butée [BRUT] | Seuil chimique [CORRIGÉ] |
|---|---|---|
| `ph_minus` (acide) | `phRaw < 6,20` → refus | `ph < 6,70` → refus d'acidifier |
| `ph_plus` (base) | `phRaw > 8,10` → refus | `ph > 7,60` → refus d'alcaliniser |

> **Durcissement v2.25.0** : le `ph_plus` n'était gardé par **aucun** seuil jusqu'ici. Rien n'empêchait le système d'alcaliniser l'eau dans la zone où il inhibe lui-même le chlore. La butée brute haute a été ajoutée en contre-visite : sans elle, un offset `−0,50` erroné laissait le pH+ pousser l'eau au-delà de 8,10 brut pendant que l'inhibition ORP suspendait le chlore — l'appareil conduisait lui-même l'eau dans une impasse sanitaire.

**Changement de produit** : les cinq drapeaux d'hystérésis sont réarmés (le seuil ET la direction changent, les drapeaux deviennent caducs), mais le **compteur de refus n'est PAS réinitialisé** — une anomalie durable ne s'effface pas d'un changement de réglage.

**Mesure indisponible** (`getPhValueRaw()` = NaN, garde d'âge du filtre) : **aucun refus, aucune alerte, aucun refus compté**. Les gardes NaN existantes (`canDose`, garde #4) couvrent déjà le cas ; fabriquer un refus ici ajouterait un refus **fantôme** au compteur des 3 refus / 24 h, jusqu'à fermer la régulation sur une donnée qui n'existe pas. Tous les drapeaux d'hystérésis pH sont **réarmés**.

> ⚠️ **Asymétrie assumée de la politique NaN** — les gardes pH **réarment** leurs mémoires d'hystérésis sur mesure indisponible, les gardes ORP les **conservent**. Côté pH c'est un *fail-open borné à 0,05 pH* : après un trou de mesure, la garde peut se relâcher 0,05 pH plus tôt — au niveau du bruit résiduel du filtre. Côté ORP la conservation est **nécessaire** (voir [filet 2](#2-inhibition-du-dosage-orp-sur-ph-élevé-isorpinhibitedbyph)). Détail dans l'[ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

#### Avertissement non bloquant à 7,00 (`kPhLowWarn`)

Seuil `warn` sur le pH **corrigé**, edge-triggered, **dans les deux cas de produit**. Il **ne bloque rien** et **n'incrémente aucun compteur** : c'est le premier avertisseur, conservé (condition du gate) malgré l'abaissement du seuil de blocage à 6,70. Sous 7,0, l'eau commence à être agressive pour l'installation bien avant qu'il faille refuser de doser.

- Pose : `warning` + alerte MQTT `ph_low_warning` — `pH 6.95 sous 7.00 — eau agressive (liner, joints, parties métalliques). Dosage encore autorisé jusqu'à 6.70 ; contrôler le pH au réactif.`
- Levée : `info`, pas d'alerte.

#### Hystérésis de relâche (0,05 pH, toutes les gardes)

Entrée en blocage à `value < threshold` (ou `>` pour un seuil haut) ; **sortie** seulement à `value >= threshold + 0,05` (ou `<= threshold − 0,05`). Sans elle, un pH qui oscille sur un seuil (bruit du filtre) produit une **rafale** de transitions bloque/relâche, chacune comptée comme un refus — donc une bascule fail-closed sur un incident unique.

Les logs de relâche citent l'hystérésis : `[Dosage pH] pH remonté à 6.75 (plancher 6.70 + hystérésis 0.05) — injection d'acide de nouveau possible`.

#### Alertes de franchissement

`critical` + alerte MQTT **`ph_guard`** à la transition ; `info` au retour, sans alerte. Un franchissement de butée brute et un franchissement de seuil corrigé sur le même tick valent **un seul** refus.

Ce tableau décrit le texte **journalisé / publié en alerte MQTT** (`systemLogger.critical` + `mqttManager.publishAlert`). Côté acide, depuis la v2.55.1, il n'est **plus inconditionnel** — voir la note sous le tableau.

| Garde | Message publié (log + MQTT) |
|---|---|
| Seuil bas [CORRIGÉ] | `pH 6.65 sous le plancher de sécurité 6.70 (offset=0.10) — injection d'acide refusée` |
| Butée basse [BRUT] | `pH BRUT 6.18 sous la butée de sécurité 6.20 (offset=0.50) — injection d'acide refusée quelle que soit la valeur corrigée : offset positif probablement erroné, contrôler le pH au réactif` — **émis seulement si la garde corrigée ne mord pas au même tick** (v2.55.1, voir ci-dessous) |
| Plafond [CORRIGÉ] | `pH 7.65 au-dessus du plafond de sécurité 7.60 (offset=-0.10) — injection de pH+ refusée : au-delà, le chlore est peu actif et la régulation ORP est inhibée` |
| Butée haute [BRUT] | `pH BRUT 8.14 au-dessus de la butée de sécurité 8.10 (offset=-0.20) — injection de pH+ refusée quelle que soit la valeur corrigée : offset négatif probablement erroné, contrôler le pH au réactif` — inconditionnel (chemin base, non modifié) |

#### Priorité des causes pour l'acide — discriminante depuis v2.55.1

**Incident du 2026-10-05** : sur un offset `ph_offset = 0,50` (le maximum autorisé), le bandeau « Dosage bloqué » affichait *« pH brut sous la butée de sécurité (5.94 < 6.20) »* pendant que la carte pH affichait **6,4 corrigé** — deux chiffres irréconciliables pour une même grandeur, alors que la décision de blocage elle-même était correcte (6,44 corrigé < 6,70). Cause : `kPhFloorRawHard = kPhFloorCorrected − kPhOffsetMax`, donc quand `offset == kPhOffsetMax`, `corrigé < 6,70 ⟺ brut < 6,20` — **les deux gardes mordent alors ensemble par construction**, et la butée brute prenait jusqu'ici la main **inconditionnellement** sur la cause de refus exposée (`setGuardRefusalCause`, lue par le bandeau/WS), alors qu'elle n'apportait plus d'information et évinçait la seule formulation vérifiable contre l'écran. [bug-135](../../specs/features/todo/bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md) documente l'épisode complet, note du 2026-10-05.

**Règle appliquée** (`src/pump_controller.cpp`, bloc `ACIDE`, validée GO par le gate `pool-chemistry` du 2026-10-05 — étiquetage seul, aucun seuil ni condition de blocage modifié) : la garde CORRIGÉE est évaluée en premier ([`pump_controller.cpp:889`](../../src/pump_controller.cpp:889)) ; la butée brute, évaluée ensuite, ne pose son log critique + alerte MQTT ni sa cause de refus que si elle est **discriminante** — elle mord **et** la garde corrigée ne mord **pas** au même tick ([`pump_controller.cpp:904`](../../src/pump_controller.cpp:904)). Quand les deux mordent, c'est **toujours** la cause corrigée qui est exposée, posée **inconditionnellement** ([`pump_controller.cpp:957`](../../src/pump_controller.cpp:957)) — c'est la valeur corrigée qui fait foi à l'écran (ADR-0039).

Textes de **cause de refus** (`setGuardRefusalCause`, exposée au WS / au bandeau « Dosage bloqué » — distincts du texte log/MQTT ci-dessus), imposés mot à mot par le gate. **Depuis le 2026-10-10 (feature-149)**, ils ne citent plus une mesure mais un **seuil** — voir [la règle ci-dessous](#règle--une-cause-latchée-ne-porte-jamais-une-mesure-vive-v2571) :

| Cas | Texte de la cause de refus exposée |
|---|---|
| Garde corrigée mord (seule, ou les deux mordent) | `pH sous le plancher de sécurité (6.7) — dosage acide suspendu.` |
| Butée brute mord **seule** (discriminante) | `Vérifiez l'offset pH : brut sous 6.2, corrigé au-dessus de 6.7 — dosage acide suspendu.` |
| Butée brute haute [BRUT, produit `ph_plus`] | `pH brut au-dessus de la butée de sécurité (8.1) — dosage pH+ suspendu.` |
| Plafond [CORRIGÉ, produit `ph_plus`] | `pH au-dessus du plafond de sécurité (7.6) — dosage pH+ suspendu.` |

⚠️ **Le texte « Vérifiez l'offset pH » a été TRONQUÉ en production du v2.55.1 à la v2.64.0** : son ancienne formulation (mesures comprises) pesait ~160 octets pour un tampon `_lastRefusalCause[2][96]` de 95 octets utiles (`kDoseBlockedReasonMaxLen`) — `strlcpy` coupait à « …reste au-dessus du pla », et l'instruction finale « vérifiez l'offset pH » n'a **jamais atteint l'écran**. Le nouveau texte (seuils, pas mesures) pèse **91 octets** et tient avec une marge de 4 octets. Bénéfice collatéral : l'alerte horaire du mode Programmé (`ph_guard_scheduled`, [ci-dessous](#escalade--3-refus-en-24-h-mode-automatic-uniquement)), qui réémet cette même cause toutes les heures, était tronquée elle aussi et **redevient complète**.

> ⚠️ **Défaut non traité, repéré pendant cette documentation (hors périmètre feature-149, à router vers `code-reviewer`/`embedded-developer`)** : les quatre nouveaux textes se terminent désormais par un point (`…suspendu.`), alors que l'ancien format ne s'achevait jamais par une ponctuation. Le message `ph_guard_scheduled` (ligne 1191) concatène `"Dosage pH programmé refusé — " + String(_lastRefusalCause[0]) + ". Le volume quotidien…"` : avec un texte de cause qui se termine déjà par un point, le message publié affiche désormais **deux points consécutifs** (`…dosage acide suspendu.. Le volume…`). Inoffensif fonctionnellement, mais visible dans le journal et sur MQTT.

⚠️ **Conséquence** : le texte « offset positif probablement erroné » (log + alerte MQTT de la butée brute) ne peut désormais survenir qu'avec `offset < kPhOffsetMax` — un offset `== kPhOffsetMax` (le cas le plus fréquent en pratique, celui de l'incident) ne le déclenche plus jamais puisque les deux gardes mordent alors systématiquement ensemble. Ce message n'est donc plus un bruit de fond : sa simple apparition signale une **divergence entre deux lectures** du pH brut (butée sur un échantillon instantané, garde corrigée sur la valeur filtrée) — c'est le mécanisme même que `bug-135` continue d'instruire (fenêtre glissante sur la butée brute, hors périmètre de ce correctif).

Non touché par ce correctif : les seuils (6,20 / 6,70), l'hystérésis (0,05), `blocked = rawHard || low`, le chemin **base** (`ph_plus`, toujours prioritaire sur la butée brute, voir asymétrie ci-dessus).

#### Escalade — 3 refus en 24 h, mode `automatic` uniquement

`kPhGuardRefusalMax = 3`, `kPhGuardRefusalWindowMs = 86 400 000` (fenêtre glissante). Le firmware bascule `mqttCfg.phRegulationMode` en `"manual"`, met `phEnabled` à `false` (invariant du projet : `ph_enabled` est le miroir de « mode ≠ manual »), **persiste** (`saveMqttConfig()`), loggue `critical` et publie une seconde alerte `ph_guard`.

**Seuls les refus survenus en mode `automatic` sont comptés** (condition du gate) : une consigne volumétrique choisie par l'utilisateur en mode **programmé** ne doit pas fermer la régulation. Le mode programmé refuse tout autant — c'est un filet chimique, pas une boucle de régulation — mais **signale** à la place, en `warning` + alerte MQTT **`ph_guard_scheduled`** à débit limité (`kPhGuardScheduledAlertIntervalMs` = 1 h). Sans cette réémission, un refus permanent y serait un no-op **silencieux** après la première transition.

Le latch de refus est **consommé** par la décision et non relu comme une transition : le franchissement a pu survenir alors que la branche automatique était inéligible (quota épuisé) — le refus n'en est pas moins réel dès qu'elle le redevient.

**Le message d'escalade ne présume plus de la cause.** L'ancien texte (« sonde pH à recalibrer ou remplacer ») a induit l'utilisateur en erreur le 25/08/2026 alors que le diagnostic EZO était nominal. Le firmware interroge `isPhProbeHealthy(slopeAcid, slopeBase, zeroMv)` — pentes ∈ [90 ; 110] % **et** \|zéro\| ≤ 15 mV, toute valeur `NaN` ⇒ `false` (on ne déclare jamais une sonde saine sans preuve) — et choisit un des deux textes :

| Diagnostic | Message publié |
|---|---|
| Sonde **saine** | `3 refus de garde pH en 24 h — régulation pH basculée en manuel. Diagnostic sonde EZO NORMAL (pente acide 101.0 %, base 100.0 %, zéro −1.03 mV) : la sonde n'est pas en cause. Vérifier la cohérence consigne / offset / produit déclaré, puis un décalage de référence EXTERNE (courants de fuite de la pompe de filtration, équipotentialité du bassin). Contrôler le pH au réactif avant toute intervention sur la sonde.` |
| Hors norme ou **indisponible** | `… Diagnostic sonde EZO hors norme ou indisponible (…) : vérifier la calibration et l'état de la sonde, puis un éventuel décalage de référence externe (courants de fuite, équipotentialité). Contrôler le pH au réactif.` |

Ces seuils de diagnostic servent **uniquement** à formuler le message : **aucune décision de dosage n'en dépend**.

**v2.70.0 (feature-154)** : ces mêmes trois constantes sont désormais **servies au navigateur** (`GET /get-config` : `ph_slope_healthy_min_pct`, `ph_slope_healthy_max_pct`, `ph_zero_healthy_max_mv`) et consomées par l'écran « État sonde pH » pour afficher **une pastille par critère** (pente acide, pente base, décalage zéro) — voir [page-ph.md](../features/page-ph.md#classification-ui) et [API.md](../API.md). `isPhProbeHealthy()` reste la **seule** fonction qui décide, inchangée par cette feature ; l'affichage ne fait que la rendre lisible critère par critère. Principe verrouillé par [ADR-0100](../adr/0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md) : un seuil de santé affiché est servi par le firmware, jamais réécrit côté client.

> ℹ️ Ces messages dépassent la charge utile MQTT (`kMaxPayloadLen` = 128 o) : `publishAlert()` les tronque proprement (frontière UTF-8 + suffixe `... (voir journal)`) pour que le JSON reste valide côté Home Assistant. Texte intégral dans le journal système et dans `ph_auto_manual_reason`.

**Effacement de la cause exposée.** La **relâche** d'une garde (et le cas mesure indisponible) efface la cause de refus quand une garde en est propriétaire — y compris le message « régulation pH basculée en manuel », qui ne décrit plus un refus en cours. C'est le seul point du firmware qui tourne à chaque tick **quel que soit le mode** : `canDose(0)` ne le ferait qu'en mode `automatic`, donc en `manual` la chaîne restait figée sur un blocage révolu — précisément le mode où l'UI expose l'injection manuelle. La trace de l'escalade est conservée à part (`setPhAutoManualTrace` + bannière d'acquittement).

#### Bornes de la consigne pH et de l'offset (v2.25.0)

`kPhTargetMin` = **7,20**, `kPhTargetMax` = **7,50**, pas `kPhTargetStep` = **0,10** (4 positions), **identiques** pour `ph_minus` et `ph_plus` : le type de produit choisit la **pompe** à actionner, pas la fenêtre de pH visée — une eau à 7,80 est aussi mauvaise qu'elle ait été atteinte par excès de base ou par défaut d'acide.

| Borne | Valeur | Justification |
|---|---:|---|
| Basse | `7,20` | Sous 7,2 l'eau devient **agressive** (liner, joints, parties métalliques), inconfortable, et **consomme le TAC**. Le gain d'acide hypochloreux entre 7,2 et 7,0 ne justifie pas une corrosion permanente : le bon levier pour l'efficacité du chlore est le **TAC / stabilisant**, pas un pH bas. |
| Haute | `7,50` | Au-delà, la fraction de HClO s'effondre. **7,50 et non 7,60** : à 7,60 pile la consigne atteindrait `kPhOrpInhibit` et le chlore serait suspendu **en permanence** dès que la régulation tient sa consigne — panne sanitaire silencieuse. Verrouillé par `static_assert`. |

⚠️ **Changement visible sur les installations existantes** : la borne haute passe de 7,60 (v2.25.0 initiale, non publiée) à **7,50**. Une consigne persistée à 7,60 est **clampée au chargement NVS**, journalisée (`warning` quand l'écart vient du seul resserrement de borne, `critical` sinon) et **réécrite immédiatement** en NVS. Valeur non finie ⇒ `kPhTargetMin`.

Les bornes sont appliquées sur les **trois** chemins d'écriture :

| Chemin | Comportement hors bornes |
|---|---|
| `POST /save-config` | **Refus 400** avec message français, **avant toute mutation** de `mqttCfg`. Jamais d'écrêtage silencieux — écrêter ferait croire que la consigne saisie est appliquée. Dans la plage, la valeur est **alignée sur le pas** (`info` si l'alignement change quelque chose). Voir [API.md](../API.md#post-save-config--critical). |
| Commande MQTT `ph_target/set` | **Refus** + `warning` + `publishTargetState()` pour resynchroniser Home Assistant sur la consigne réelle. Voir [MQTT.md](../MQTT.md). |
| Chargement NVS (`loadMqttConfig`) | **Clamp** + log + **réécriture NVS immédiate** (installations existantes, ancienne commande MQTT, NVS corrompue). |

L'**offset** suit la même logique : bornes `±0,50`, pas `0,10`, arrondi au plus proche multiple par la fonction pure `roundPhOffsetToStep()` — partagée par la route de sauvegarde et par la **migration NVS** des installations ayant enregistré une valeur libre (0,47 → 0,50). L'écart maximal introduit est de 0,05 pH, soit le bruit résiduel du filtre : aucun effet mesurable sur la régulation. Offset `NaN` ⇒ 0,00 (neutre), jamais `−0,0`.

**Bornes servies au client, jamais codées en dur.** `GET /get-config` publie six champs dérivés des constantes firmware — `ph_target_min`, `ph_target_max`, `ph_target_step`, `ph_offset_min`, `ph_offset_max`, `ph_offset_step`. Le frontend n'en code aucune : élargir la plage ou passer de 4 à 5 positions se fait **côté firmware seul**. Aucun miroir de seuil de sécurité ne subsiste côté client — les seuils sont fixes et ne dépendent plus de l'offset, il n'y a plus rien à recalculer ([page-ph.md](../features/page-ph.md)).

**Suppressions** : `phRawFloorFor()`, `kPhRealFloorTarget`, `kPhRawFloorAbsolute`, `kPhOffsetWideThreshold`, `checkPhTargetReachable()`, `isPhTargetUnreachable()`, `kPhTargetFloorMarginMin` et l'alerte MQTT `ph_target_unreachable` **n'existent plus** : la marge est devenue structurelle (garantie par les `static_assert`), il n'y a donc plus rien à détecter à l'exécution.

#### Persistance de la bascule automatique en manuel (v2.20.3)

Le journal système est un **ring buffer** : quelques heures après l'escalade il ne restait **aucune** trace de la bascule, et il devenait impossible de dire si `ph_regulation_mode == "manual"` venait du firmware ou d'une action humaine (constat de l'incident du 2026-08-16). La cause est donc désormais **persistée en NVS**.

Deux champs dans `mqttCfg` ([`config.h`](../../src/config.h)) :

| Champ | Clé NVS | Contenu |
|---|---|---|
| `phAutoManualEpoch` | `ph_am_epoch` | epoch UNIX de la bascule ; `0` si aucune bascule en attente **ou** si l'horloge n'était pas synchronisée (`< kMinValidEpoch`) |
| `phAutoManualReason` | `ph_am_reason` | cause en clair (français) ; chaîne vide si `epoch == 0` |

- **Écriture** : `setPhAutoManualTrace(reason)` est appelée **juste avant** `saveMqttConfig()` sur le chemin d'escalade — la trace part donc dans la **même transaction NVS** que le mode lui-même. Un `critical` + l'alerte MQTT `ph_guard` (ex-`ph_raw_floor`) sont émis comme avant.
- **Acquittement** : voir [Acquittement fail-closed](#acquittement-fail-closed-de-la-bascule-v2250) ci-dessous — depuis la v2.25.0, il exige un **acte délibéré** et ne peut plus résulter d'un simple changement de mode. ⚠️ Historique : tester la seule **présence** de la clé faisait acquitter la trace à la moindre sauvegarde des réglages (correctif bloquant de revue v2.20.3), puis un `ph_regulation_mode` **réémis** par le client suffisait encore (incident du 2026-08-25).
- **Relecture au boot** : clés absentes ⇒ `0` / `""`, **rétrocompatible** sur un appareil mis à jour. Une trace non acquittée relue au boot est re-journalisée en `critical`. Un epoch invalide est neutralisé avec la raison.
- **Exposition** : `ph_auto_manual_epoch` / `ph_auto_manual_reason` en lecture seule dans `GET /get-config` ⇒ bannière « Bascule automatique en mode manuel » de la page pH. Voir [API.md](../API.md#get-get-config--write) et [page-ph.md](../features/page-ph.md#bannière--bascule-automatique-en-mode-manuel--v2203).

L'invariant du projet est préservé : `ph_enabled` reste le **miroir** de « mode ≠ manual » (mis à `false` par la bascule), sans quoi l'UI et Home Assistant afficheraient la régulation pH active juste après un basculement de sécurité. Depuis la v2.25.0 ce miroir est un **dérivé pur** : `ph_enabled` n'est **plus accepté en écriture** par `/save-config` (il reste exposé en lecture) — le laisser écrivable ouvrait un second chemin capable de rouvrir la régulation en contournant le gate d'acquittement.

#### Acquittement fail-closed de la bascule (v2.25.0)

Tant qu'une trace existe (`phAutoManualReason` non vide **ou** `phAutoManualEpoch != 0`), toute sortie du mode `manual` est **refusée**. La décision est portée par la fonction pure `phModeChangeNeedsAck(traceActive, curIsManual, newIsManual, ack)` ([`ph_guard_logic.h`](../../src/ph_guard_logic.h)), appelée depuis **les deux** chemins de commande :

| Chemin | Comportement sous trace active |
|---|---|
| `POST /save-config` | **409 Conflict** + corps JSON français. Le bloc de gate est le **premier** du handler : il précède toute mutation de `mqttCfg`, sinon un refus laisserait la config RAM divergente de la NVS. |
| Commande MQTT `ph_regulation_mode` (Home Assistant) | **Refus silencieux côté bus** + log `warning` + `publishTargetState()` pour **resynchroniser HA** sur le mode réel (`manual`). La trace n'est **jamais** effacée par MQTT. |

**Sortie unique : l'interface web.** Un select Home Assistant est un clic distant qui n'affiche ni la cause ni le refus ; une automation ou une restauration d'état HA au redémarrage rejouerait `automatic` et désarmerait la sécurité sans témoin. L'acquittement passe donc **obligatoirement** par le bouton de la bannière de la page pH (champ `ph_auto_manual_ack`), seul endroit où la cause est affichée — recommandation du gate `pool-chemistry`.

Sémantique du champ :

- `ph_auto_manual_ack: true` **seul** ⇒ efface la trace et **laisse le mode en manuel** (la bannière acquitte, elle ne réarme pas). L'utilisateur choisit ensuite son mode.
- `ph_auto_manual_ack: true` + `ph_regulation_mode` ⇒ autorise en plus la transition dans le même POST.
- **Booléen STRICT** : ArduinoJson convertit *toute* chaîne en `true` via `as<bool>()` — y compris `"false"`. Le firmware exige donc le type booléen JSON (`isExplicitJsonTrue`), sans quoi un client maladroit acquitterait une sécurité chimique par accident.

Journalisation : l'**IP d'origine** est tracée sur les deux issues (`REFUSÉ (409) depuis …` / `Mode régulation pH changé: … (depuis …)`) — sans elle, l'incident du 25/08 était indiagnosticable.

Voir [API.md](../API.md#post-save-config--critical), [MQTT.md](../MQTT.md) et [page-ph.md](../features/page-ph.md#bannière--bascule-automatique-en-mode-manuel--v2203).

#### Verrou étalonnage sur le changement de mode (bug-161, gate C2)

⚠️ **Angle mort comblé en feature-161 (v2.77.0), distinct de l'acquittement ci-dessus.** bug-160
bascule la régulation (pH **ou** ORP) en `manual` à l'**entrée** en étalonnage
(`POST /calibration/enter`, `mqttCfg.calSwitchActive`) et la restaure au mode **antérieur**
mémorisé (`ph/orpCalSwitchPrevMode`) à la **sortie** (`POST /calibration/exit`). **Sans garde**, une
demande de changement de mode reçue **pendant** la session serait acceptée… puis **silencieusement
écrasée** à la sortie par la valeur mémorisée à l'entrée — un réglage accepté puis annulé sans un
mot, exactement le motif « l'écran ment » que ce projet traque.

**Décision pure** `calSwitchBlocksModeChange(calSwitchActive, modeWouldChange)`
([`cal_manual_switch_logic.h`](../../src/cal_manual_switch_logic.h)) : refuse **ssi** la session
est ouverte **et** qu'un changement **réel** est demandé (mode demandé ≠ mode courant, calculé par
l'appelant) — jamais sur la seule **présence** de la clé dans la requête, car l'interface renvoie
systématiquement le mode courant dans chaque payload de sauvegarde : refuser sur la présence
bloquerait **tout autre réglage** pendant une session d'étalonnage.

⛔ **Cette fonction REFUSE, elle ne réconcilie jamais** : elle ne met **jamais** à jour
`ph/orpCalSwitchPrevMode` — un second chemin d'écriture vers cette mémoire de sécurité serait
exactement le piège qu'elle ferme (seul `/calibration/enter` y écrit).

| Chemin | Comportement sous session ouverte |
|---|---|
| `POST /save-config` (pH **et** ORP) | **409 Conflict** avant toute mutation (même emplacement que le gate `phAutoManual*` ci-dessus — **évalué après lui**, intact et prioritaire), corps JSON : `Étalonnage en cours — le mode est rétabli à la fin de l'étalonnage.` |
| Commande MQTT `ph\|orp_regulation_mode/set` | ✅ **Couvert depuis le lot C (correctif post-livraison, même jour)** — voir sous-section dédiée ci-dessous. |

Journalisation : `warning`, IP d'origine, voie(s) concernée(s) — `[Config] /save-config REFUSÉ
(409) depuis <ip> : changement de mode pH refusé — étalonnage en cours (calSwitchActive).`

**Risque relevé par le gate, corrigé en même temps** : une option de mode **inexistante** envoyée
par une automatisation Home Assistant non mise à jour (après le renommage des libellés, voir
[MQTT.md](../MQTT.md#libellés-français-des-selects-mode-régulation-phorp-bug-ha-regulation-mode-labels-v2173))
tombait **silencieusement** dans un `else` sans trace — mode inchangé, aucun log. Un `warning`
journalise désormais tout mode rejeté (`ph_regulation_mode` / `orp_regulation_mode` « rejeté
(valeur inconnue) »).

#### Lot C — le verrou couvre désormais MQTT, et deux trous de plus fermés (bug-161, correctif post-livraison)

⚠️ **Un gate à moitié posé est pire qu'absent — il crée la confiance sans la protection.** Signalé
par `doc-writer` en documentant la section ci-dessus (« non vérifié par ce lot… à porter à un futur
gate »), vérifié puis instruit par `pool-chemistry` : **GO sous conditions**, fermé le jour même.
L'instruction a trouvé **deux trous de plus**, dans le même chemin, plus graves que le premier :

1. **Le verrou MQTT lui-même** — `drainCommandQueue()` (`src/mqtt_manager.cpp`) appelle désormais
   la **même** décision pure `calSwitchBlocksModeChange(calSwitchActive, changed)` que le chemin
   HTTP, au **même rang** (après le gate `phAutoManual*` existant côté pH, inchangé — l'ORP n'a
   aucun gate de sécurité équivalent, celui-ci est donc le seul évalué sur cette voie). Refus =
   sortie de `manual` uniquement ; une commande entrante `manual` reste un no-op accepté. Même
   texte de refus, même `warning`, même `publishTargetState()` de resynchronisation que le chemin
   HTTP — **aucune seconde variante** de la décision.
2. **`orp_enabled` acceptait l'écriture directe** (`web_routes_config.cpp:854`,
   `mqttCfg.orpEnabled = doc["orp_enabled"]`) — chemin **jamais fermé** depuis que son équivalent
   `ph_enabled` l'avait été en v2.25.0 (B7). Un payload `{"orp_enabled": true}` seul, sans
   `orp_regulation_mode`, pouvait réarmer `orpEnabled` **pendant que `orpRegulationMode` restait
   `manual`** — y compris pendant une session d'étalonnage, contournant le verrou C2 par un champ
   voisin. `orp_enabled` est désormais un **dérivé pur** du mode (comme `ph_enabled`), ignoré en
   écriture (clé envoyée = no-op silencieux, même patron que `ph_enabled`/`screen_enabled`).
3. **La trace de session était purgée au redémarrage suivant si le trou MQTT avait été exploité**
   (`config.cpp:548` — la purge au boot suppose que les deux modes n'ont pu quitter `manual` qu'**
   après** la fermeture réelle de la session ; le trou MQTT rendait cette hypothèse fausse).
   **Conséquence la plus grave des trois** : pas seulement un dosage possible sur une mesure non
   représentative, mais la **disparition de la trace elle-même** — bandeau d'étalonnage éteint,
   alerte d'oubli (2 h puis 6 h) jamais émise, **sonde toujours physiquement dans le tampon** sans
   aucun signal à l'écran. Fermé par construction : la purge au boot reste correcte dès lors que
   les trois chemins d'écriture du mode (HTTP, MQTT, et la purge elle-même) partagent le même
   verrou — documenté comme invariant à vérifier **avant** tout futur chemin d'écriture du mode,
   pas après (commentaire dans `config.cpp`).

**Tests natifs** (`test/test_native_cal_manual_switch/test_cal_manual_switch.cpp`) : vecteurs
`(calActive, modeCourant, payload)` pH **et** ORP, dérivant `modeWouldChange` exactement comme les
deux sites d'appel réels — sortie de `manual` refusée, entrée `manual` acceptée (no-op), hors
session acceptée, et un cas de défense en profondeur (mode courant non-`manual` pendant une session
active, état non atteignable en production mais refusé quand même).
`test/test_native_ws_config_parity/test_ws_config_parity.cpp` (test structurel, lit les sources) :
vérifie qu'`orp_enabled` n'est affecté **qu'une seule fois** dans chaque fichier (HTTP et MQTT), et
que c'est toujours la forme **dérivée** du mode — un mutant qui réintroduirait l'écriture directe,
ou un second chemin d'écriture même dérivé, fait échouer le test.

### ⚠️ Limites connues, non corrigées par ce lot (laissées à des specs séparées)

Deux défauts **constatés** pendant l'instruction du lot C, volontairement **non corrigés** ici —
ce sont des limites connues, pas des oublis :

- **Aucun garde-fou indépendant du mode** n'existe au niveau de `canDose()` pour refuser **tout**
  dosage tant qu'un étalonnage est ouvert. Les gardes posées (HTTP, MQTT, purge au boot) protègent
  **trois chemins nommés**, pas « le dosage pendant un étalonnage » comme propriété générale — un
  futur quatrième chemin d'écriture du mode qui ne passerait pas par `calSwitchBlocksModeChange()`
  rouvrirait le même risque.
- **Côté `ph_plus` (base), aucun plancher bas n'existe** : un tampon pH 4,0 déclencherait une
  injection de base sans aucune garde pour l'arrêter (asymétrie avec le plafond haut déjà couvert
  par l'inhibition ORP ci-dessous).

### 2. Inhibition du dosage ORP sur pH élevé (`isOrpInhibitedByPh`)

Au-dessus de `kPhOrpInhibit` = **7,60**, la fraction d'**acide hypochloreux** (HClO, forme active du chlore) s'effondre (< 25 %) : l'ORP mesuré chute à concentration de chlore constante, et la régulation ORP compenserait en **surdosant**. Le dosage ORP est donc suspendu — le vrai correctif est de **faire baisser le pH**.

**v2.25.0 — le seuil porte désormais sur le pH CORRIGÉ**, plus sur le brut : la fraction de HClO dépend du pH **réel** de l'eau, pas de la lecture non corrigée du capteur. Cela supprime la limitation connue de la v2.25.0 initiale (avec un offset négatif légitime, le brut d'exploitation valait 7,50-8,10 et l'inhibition devenait **permanente**).

Deux seuils, **OU logique**, appliqués aux branches automatique **et** programmée, quel que soit le type de produit déclaré :

| Seuil | Référentiel | Rôle |
|---|---|---|
| `kPhOrpInhibit` = `7,60` (relâche ≈ 7,55) | **corrigé** | chlore peu actif → la régulation ORP surdoserait |
| `kPhCeilingRawHard` = `8,10` (relâche `8,05`) | **brut** | butée inconditionnelle contre un offset **négatif** saisi à tort, qui masquerait une eau réellement haute |

> La relâche est écrite `kPhOrpInhibit − kPhGuardReleaseHysteresis` et **jamais** en littéral `7.55f` : en `float` l'expression vaut 7,5499997, donc à 7,55 pile la garde bloque encore. Écart de 3e−7 pH, cinq ordres sous le bruit du filtre, et il penche du côté **bloquant** — fail-closed.

`critical` + alerte MQTT **`orp_ph_inhibit`** à la transition ; `info` au retour sous le seuil.

#### Une mémoire d'hystérésis PAR SEUIL, et anti-rafale par épisode

Deux correctifs de revue v2.25.0, tous deux issus de cas observés :

**R1 — découplage mémoire d'hystérésis / latch d'inhibition.** Un drapeau unique cumulait deux rôles : la mémoire de franchissement (`wasBlocking`) et le latch « inhibition en cours » qui porte le motif et l'edge-trigger d'alerte. Or les branches NaN arment le latch pour des motifs **étrangers à tout seuil** (pH indisponible, mesures non représentatives). Résultat observé : reprise de filtration → mesures non représentatives → latch posé ; le pH revient à 7,57 **sans jamais être monté au-dessus de 7,60**, mais l'hystérésis, croyant sortir d'un blocage, exigeait ≤ 7,55 et **maintenait l'inhibition du chlore indéfiniment**, à chaque démarrage de filtration, en annonçant « stabilisation des mesures ». Les mémoires de franchissement sont désormais écrites **exclusivement** par les comparaisons de seuil ; aucune branche NaN ne les touche. Le fail-closed sur pH inconnu reste entier — il agit sur le latch, pas sur la mémoire.

**R2 — une mémoire par seuil.** Les deux seuils **partageaient** un drapeau, écrit par `(raw || corr)`. Le seuil le moins souvent armé perdait alors toute hystérésis : tant que le seuil corrigé maintenait le drapeau, la butée brute se comportait en simple comparateur à 8,05. Cas physique observé — bassin à pH 8,03-8,12 avec offset 0,00 : chaque mesure franchissant 8,05 re-qualifiait le motif et produisait un `critical` + une alerte MQTT, en alternant **deux messages contradictoires** (« offset négatif probablement erroné » alors que l'offset vaut 0,00, puis « pH > 7,60 »). Sur une partition `history` de 64 Ko, ce battement chasse tout le reste du journal.

**Anti-rafale d'alertes** : une alerte est émise **uniquement** si la gravité du motif courant **dépasse** celle déjà signalée dans l'épisode (butée brute `3` > seuil corrigé `2` > mesure absente `1`). La re-qualification ne va donc jamais en dégradation, et une gravité déjà signalée ne réémet rien. La mémoire n'est remise à zéro qu'à la **levée** de l'inhibition — jamais avant, sinon l'épisode se scinderait et la rafale reviendrait.

**Libellés honnêtes** : quand la mesure est retombée **dans la bande de relâche** (l'hystérésis peut rester armée après un effacement de latch), le message ne prétend pas « au-dessus de 8,10 » mais « maintenu dans la bande d'hystérésis de la butée 8,10 (relâche à 8,05) ». Et l'offset n'est mis en cause que s'il est réellement **négatif** : à offset ≥ 0 le pH corrigé est au moins égal au brut, l'eau est réellement haute, et accuser l'offset est un contresens (c'est l'accusation que recevait le bassin à offset 0,00).

#### Règle : une cause latchée ne porte jamais une mesure vive (v2.57.1)

**Une cause de refus latchée ne porte jamais une mesure vive — elle porte un seuil, ou rien.** Propagée à la voie pH en v2.65.0 (feature-149), puis harmonisée en décimales avec la voie pH en v2.65.1 (feature-150, voir [plus bas](#propagation-à-la-voie-ph-puis-à-la-voie-orp-feature-149-v2650-feature-150-v2651)).

`setGuardRefusalCause()` est volontairement un **latch** : il n'est réécrit qu'en cas d'**aggravation** de gravité (voir l'anti-rafale ci-dessus), pas à chaque tour. C'est délibéré — réécrire à chaque mesure faisait battre deux libellés contradictoires, avec un `critical` et une alerte MQTT par oscillation, et chassait le journal de la partition `history`. Mais ce choix a une conséquence directe : **toute valeur de mesure inscrite dans ce texte se fige** au moment du déclenchement et ne se rafraîchit plus tant que l'épisode dure, alors que la carte pH, elle, continue d'évoluer en direct.

**Incident du 2026-10-08** : le bandeau affichait *« pH trop élevé (8.28) — dosage chlore suspendu »* pendant que la carte pH affichait **8,1** au même instant — deux valeurs différentes pour la même grandeur, au même écran, toutes deux correctes chacune à leur heure. Correctif : les deux causes de refus de ce bloc (`tickOrpPhInhibitSurveillance()`) remplacent la mesure par le **seuil franchi**, qui est une constante et ne périme jamais — et qui est de toute façon le chiffre utile pour l'utilisateur, puisqu'il dit **où redescendre** :

| Avant (mesure, périmée) | Après le correctif v2.57.1 (seuil, deux décimales) | Après feature-150, v2.65.1 (seuil, une décimale) |
|---|---|---|
| `pH trop élevé (8.28) — dosage chlore suspendu` | `pH trop élevé (seuil 7.60) — dosage chlore suspendu` | `pH trop élevé (seuil 7.6) — dosage chlore suspendu` ([`pump_controller.cpp:1524`](../../src/pump_controller.cpp:1524), `kPhOrpInhibit`) |
| `pH brut en butée haute (8.14) — dosage chlore suspendu` | `pH brut en butée haute (butée 8.10) — dosage chlore suspendu` | `pH brut en butée haute (butée 8.1) — dosage chlore suspendu` ([`pump_controller.cpp:1487`](../../src/pump_controller.cpp:1487), `kPhCeilingRawHard`) |

⚠️ **feature-150 (2026-10-10)** : ces deux textes sont passés de deux à **une** décimale, pour la
même raison que les causes pH ci-dessous — `kPhCeilingRawHard` est la **même constante** que
celle citée par la cause pH « pH brut au-dessus de la butée de sécurité (8.1) », et les deux
gardes se déclenchent sur la **même condition** (pH brut trop haut) : le bandeau du tableau de
bord, qui affiche une ligne par voie, pouvait montrer le même seuil écrit `8.1` et `8.10` l'un
sous l'autre. Les textes de relâche et les bandes d'hystérésis ORP (`8,05` / `7,55`, non ronds)
**restent à deux décimales** — voir [la section dédiée](#propagation-à-la-voie-ph-puis-à-la-voie-orp-feature-149-v2650-feature-150-v2651),
dont la portée s'étend désormais explicitement aux deux voies.

**Aucun seuil ni aucune décision de dosage n'est modifié** : ce sont deux chaînes de caractères. La mesure en direct reste disponible — c'est la **carte pH** du tableau de bord (champ `ph` de `GET /data` et du message WebSocket `sensor_data`), rafraîchie à chaque tour, jamais latchée.

Les alertes MQTT (`publishAlert("orp_ph_inhibit", …)`, lignes 1351 et 1378) **ne sont pas concernées** : elles portent un message distinct, construit juste au-dessus du latch, contenant la mesure **et** le seuil **et** l'offset, émis à l'instant du franchissement — donc jamais périmé par construction.

**Pour tout futur ajout d'une cause de refus** (`setGuardRefusalCause`) : n'y inscrire une mesure que si le texte est **réécrit à chaque tour** (jamais latché). Si le texte est latché, n'y mettre qu'un seuil — une constante — ou rien.

##### Propagation à la voie pH, puis à la voie ORP (feature-149 v2.65.0, feature-150 v2.65.1)

Le même défaut existait sur les quatre causes pH (`isPhGuardBlocking`, lignes 916/938/994/1025) : un relevé de production du 2026-10-10 a montré le bandeau affichant *« pH 6.70 sous le plancher de sécurité (6.70) »* — **le même nombre apparaît deux fois** — pendant que la carte affichait 6,67. La mesure vaut par construction le seuil à l'instant de l'edge-trigger qui écrit la cause, donc elle n'apporte **aucune information au moment où elle est écrite**, puis devient fausse ensuite. Le correctif propage exactement la règle ci-dessus à la voie pH, avec trois précisions tranchées par le gate `pool-chemistry` (GO sous conditions, deux passages, 2026-10-10) :

1. **Une cause latchée cite un seuil, jamais une mesure** — déjà posé ci-dessus, désormais vrai des **deux** voies (pH et ORP).
2. **Action d'abord, constat ensuite.** Le tampon `_lastRefusalCause[2][96]` ne fait que 95 octets utiles (`kDoseBlockedReasonMaxLen`) et `strlcpy` coupe silencieusement la fin : une phrase qui annonce l'action requise (« dosage acide suspendu ») avant d'éventuels détails survit à une troncature ; une phrase qui termine sur l'instruction la perd (voir l'exemple de troncature documenté [ci-dessus](#priorité-des-causes-pour-lacide-discriminante-depuis-v2551)).
3. **Une décimale pour un pH de mesure ou de seuil ; deux pour un réglage ou une bande de diagnostic.** Les cinq seuils cités (`kPhFloorCorrected`, `kPhFloorRawHard`, `kPhCeilingCorrected`, `kPhCeilingRawHard`, `kPhOrpInhibit`) sont tous **ronds au dixième** — c'est ce qui rend une décimale suffisante sans perte d'information. ⛔ **Exception explicite, à ne PAS appliquer par réflexe** : `kPhGuardReleaseHysteresis = 0,05` n'est **pas** ronde au dixième. Les messages de **relâche** (logs de retour sous/au-dessus du seuil + hystérésis) et les bandes d'hystérésis ORP restent à **deux** décimales — les points de relâche réels valent 6,75 / 7,55 / 6,25 / 8,05, et les arrondir au dixième (6,8 / 7,5 / 6,3 / 8,1) annoncerait une reprise de dosage à un pH que l'appareil n'utilise pas.

⚠️ **Résidu laissé par feature-149, corrigé par feature-150 (2026-10-10).** La spec feature-149 avait qualifié la voie ORP de « saine » dans son hors-périmètre — mais une seule chose avait été vérifiée à ce moment-là : que ses causes ne citent pas de mesure vive. Le nombre de décimales n'avait **pas** été examiné. Conséquence concrète : les deux causes ORP (`pump_controller.cpp:1487, 1524`) citaient encore `kPhCeilingRawHard` et `kPhOrpInhibit` à **deux** décimales (« butée 8.10 », « seuil 7.60 ») alors que la cause pH affichait désormais **8.1** pour `kPhCeilingRawHard` — **la même constante, écrite deux fois différemment**, visible simultanément puisque pH et ORP se bloquent sur la même condition (pH brut trop haut) et que le bandeau affiche une ligne par voie. feature-150 aligne les deux causes ORP sur une décimale (voir le tableau « Alertes de franchissement » ci-dessus) et ajoute `kPhOrpInhibit` au `static_assert` de rondeur. **La règle des trois points ci-dessus vaut désormais identiquement sur les deux voies, sans exception résiduelle.**

**Recensement exhaustif des citations de seuil pH** (fait après feature-150, pour ne pas reproduire l'erreur d'appréciation globale de feature-149) : six causes latchées à une décimale (quatre pH + deux ORP) ; tout le reste du fichier reste à deux décimales, chaque site ayant une raison identifiée et vérifiée — logs `critical` + alertes MQTT au franchissement (horodatés, jamais périmés par construction), messages de **relâche**, bandes d'**hystérésis ORP** (8,05 / 7,55, non rondes). **Deux sites n'avaient jamais été examinés avant feature-150** et le sont désormais explicitement : `publishAlert("ph_low_warning", …)` (~ligne 884) et le `systemLogger.info` de relâche ORP (~ligne 1385) — tous deux conformes à la règle (journal/MQTT horodaté ⇒ deux décimales), **à ne pas « corriger » par symétrie** avec les six causes latchées : ce ne sont pas des causes latchées, ce sont des événements ponctuels datés.

**Garde-fou à la compilation.** Un `static_assert(numfmt::isRoundedToTenth(...))` ([`constants.h:1752-1756`](../../src/constants.h:1752)) vérifie que les **cinq** seuils cités dans les causes pH et ORP sont bien ronds au dixième. Sans lui, un futur `kPhSafetyMargin` à 0,45 (au lieu de 0,50) ferait afficher « 6,8 » pour un seuil réel à 6,75 — un **seuil de sécurité faux** affiché à l'utilisateur. Vérifié par mutation : abaisser `kPhSafetyMargin` à 0,45 casse effectivement la compilation.

Voir [test_native_ph_guard_causes](../../test/test_native_ph_guard_causes/test_ph_guard_causes.cpp) : le test lit le **texte source** des dix appels à `setGuardRefusalCause()` (huit pH + deux ORP depuis feature-150) et échoue si un identifiant hors liste blanche de seuils y apparaît (donc si une mesure est réintroduite), et mesure la longueur en **octets** (pas en caractères) de chaque texte après substitution des seuils réels. Les bandes d'hystérésis restent couvertes par un test de mutation distinct (les y faire passer à une décimale doit toujours tuer).

### 3. Fail-closed sur pH indisponible (régulation ORP automatique)

En régulation **automatique**, un pH `NaN` (sonde muette, filtre non amorcé) **inhibe aussi** le dosage chlore. Justification : **`canDose(1)` ne consulte JAMAIS le pH** — ce filet est donc l'**unique garde du couplage pH/chlore**. Si la sonde pH est morte alors que l'eau est réellement au-dessus de 7,60, le chlore y est peu actif et l'asservissement ORP courrait à l'aveugle vers un surdosage. Un pH inconnu doit **suspendre** la chloration automatique, pas la laisser courir.

Journalisé en **`info`, sans alerte MQTT** : c'est une situation transitoire normale (démarrage, warmup de filtre). Le `critical` reste réservé à un vrai franchissement de seuil. Le motif « mesures non représentatives » (purge de la chambre de mesure, [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)) est distingué du motif « capteur muet » : les confondre faisait annoncer un « capteur muet » alarmant à chaque reprise de filtration, pour une stabilisation que l'appareil venait lui-même de déclencher.

Le paramètre `requireKnownPh` a disparu de la signature : la politique est **dérivée du mode ORP courant** dans la surveillance — `automatic` ⇒ fail-closed, `scheduled` ⇒ **aveugle à la mesure** par conception (comportement historique préservé, cohérent avec [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md)). En revanche les **gardes pH (filet 1) s'appliquent aux deux branches**, automatique et programmée.

### Interdiction : écrire l'offset ne réinitialise aucun compteur

⚠️ **Invariant de sécurité.** Le handler `POST /save-config` qui applique `ph_offset` n'appelle **ni** `requestReset()`, **ni** `resetDosingStates()`, **ni** `resetPhPauseGuard()`. Ces appels remettraient à zéro `cyclesToday` ou `lastStopTime` : des **enregistrements répétés de l'offset** permettraient alors de contourner la limite journalière, la limite de cycles/jour et la pause anti-cycling. Écrire l'offset ne touche **aucun** compteur de dosage ; seul `sensors.refreshPhOffsetCache()` est appelé.

Une injection **en cours** n'est pas interrompue par un changement d'offset : le nouvel offset prend effet au prochain cycle d'évaluation.

### Constantes

| Constante | Valeur | Rôle |
|---|---:|---|
| `kPhOffsetMin` / `kPhOffsetMax` / `kPhOffsetStep` | `−0.50` / `+0.50` / `0.10` pH | Bornes et pas de l'**offset** (validation serveur, refus 400, arrondi au pas) |
| `kPhTargetMin` / `kPhTargetMax` / `kPhTargetStep` | `7.20` / `7.50` / `0.10` pH | **v2.25.0** — Plage et pas de la **consigne** pH, source unique dont tout dérive (HTTP 400, refus MQTT, clamp NVS, auto-discovery HA, `/get-config`) |
| `kPhSafetyMargin` | `0.50` pH | **v2.25.0** — Distance consigne basse ↔ refus d'ACIDIFIER |
| `kPhHighMargin` | `0.10` pH | **v2.25.0** — Distance consigne haute ↔ refus d'ALCALINISER (asymétrie assumée) |
| `kPhOrpInhibitMargin` | `0.10` pH | **v2.25.0** — Garde-fou **structurel** consigne haute ↔ inhibition ORP. N'entre dans **aucun** calcul de dosage : n'existe que pour le `static_assert` |
| `kPhFloorCorrected` | `6.70` pH (dérivé) | **v2.25.0** — Refus d'**ACIDIFIER** (`ph_minus`) sur le pH **CORRIGÉ** |
| `kPhCeilingCorrected` | `7.60` pH (dérivé) | **v2.25.0** — Refus d'**ALCALINISER** (`ph_plus`) sur le pH **CORRIGÉ** |
| `kPhFloorRawHard` | `6.20` pH (dérivé) | **v2.25.0** — **Butée basse** sur le pH **BRUT** (offset positif saisi à tort) |
| `kPhCeilingRawHard` | `8.10` pH (dérivé) | **v2.25.0** — **Butée haute** sur le pH **BRUT** : inhibition ORP **et** refus d'alcaliniser (offset négatif saisi à tort) |
| `kPhOrpInhibit` | `7.60` pH | Seuil d'inhibition du dosage ORP — **sur le pH CORRIGÉ** depuis la v2.25.0 |
| `kPhLowWarn` | `7.00` pH | **v2.25.0** — Avertissement d'eau acide **NON BLOQUANT** sur le pH CORRIGÉ (ne compte ni ne bloque rien) |
| `kPhGuardReleaseHysteresis` | `0.05` pH | **v2.25.0** — Hystérésis de relâche **commune à toutes** les gardes pH (basses, hautes, butées brutes, inhibition ORP) |
| `kPhSlopeHealthyMinPct` / `kPhSlopeHealthyMaxPct` | `90` / `110` % | **v2.25.0** — Fenêtre de pente « sonde saine » (message d'escalade) ; **v2.70.0** : servie au navigateur ([ADR-0100](../adr/0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)), affichée critère par critère sur [page-ph.md](../features/page-ph.md) |
| `kPhZeroHealthyMaxMv` | `15` mV | **v2.25.0** — \|zéro\| maximal d'une sonde saine (≈ 0,25 pH) ; **v2.70.0** : idem, servie et affichée |
| `kPhGuardRefusalMax` | `3` | Refus tolérés dans la fenêtre avant bascule en manuel (mode `automatic` **seul**) |
| `kPhGuardRefusalWindowMs` | `86 400 000` ms (24 h) | Fenêtre glissante du compteur de refus |
| `kPhGuardScheduledAlertIntervalMs` | `3 600 000` ms (1 h) | **v2.25.0** — Débit maximal de réémission de l'alerte `ph_guard_scheduled` (le mode programmé n'escalade pas) |
| `kDoseBlockedReasonMaxLen` | `96` o | Tampon statique de la cause de refus en **texte** (troncature respectant les frontières UTF-8) |
| `kDoseBlockedCodeNone` | `0` | **v2.25.1** — Sentinelle « aucun refus » : le champ `*DoseBlockedCode` est alors publié `null` |
| `kDoseBlockedCodeGuard` | `250` | **v2.25.1** — Sentinelle « cause posée par une garde de sécurité » (message à valeurs runtime, non énumérable → le client lit le texte). Volontairement éloignée de la fin de `DoseRefusal`, avec `static_assert` de non-collision |
| `kDoseSensorFaultAlertDelayMs` | `60 000` ms (60 s) | **v2.25.1** — Panne de mesure **continue** exigée avant l'alerte `ph/orp_dose_sensor_fault`. 12 cycles de lecture EZO : écarte le transitoire de ré-amorçage de filtre sans masquer une panne réelle (l'affichage, lui, est immédiat) |

Toutes dans [`constants.h`](../../src/constants.h).

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
| **Effets de bord** | aucun | `logRefusalOnce()` edge-triggered, exposition `/data` + WS via `copyPhDoseBlockedReason()` / `copyOrpDoseBlockedReason()`, log `warning(...)` |
| **Temps** | injecté en paramètre (`runTimeMs`, `usedMs`) | fourni depuis `millis()` |

### Fonctions pures

- **`DoseDecision evaluateDose(const DoseInputs& in)`** — reproduit **exactement** l'ordre des gardes 2→15 de `canDose()` (la garde 1 « index pompe invalide » reste dans la coquille). Première garde en échec → cause correspondante ; sinon `{ true, None }`. **Fail-closed strict** conservé.
- **`bool shouldStartDosingPure(error, startThreshold, cyclesToday, maxCyclesPerDay)`** — démarre ssi `cyclesToday < maxCyclesPerDay` ET `error > startThreshold`.
- **`bool shouldContinueDosingPure(error, stopThreshold, runTimeMs, minInjectionTimeMs)`** — force la poursuite tant que `runTimeMs < minInjectionTimeMs` (30 s), puis poursuit ssi `error > stopThreshold`. Le **temps est injecté** → le minimum d'injection est testable sans attendre 30 s réelles.
- **`CycleStartAccounting evaluateCycleStartAccounting(wasActive, startDecided, flowActive)`** (v2.20.5) — vrai ssi `!wasActive && startDecided && flowActive`. Verrouille l'invariant « cycle compté ⇔ cycle réellement démarré » : voir [Comptabilisation d'un démarrage de cycle](#comptabilisation-dun-démarrage-de-cycle--invariant-v2205).

### Causes de refus (code machine ↔ String FR)

`enum class DoseRefusal` ([`dosing_logic.h`](../../src/dosing_logic.h)) — l'ordre des valeurs **suit l'ordre des gardes** de `canDose()` ; les valeurs 15-18 **qualifient** la garde #4. La coquille traduit chaque valeur en une chaîne française (les valeurs runtime — points de calibration, mode, nombre de cycles — sont réinjectées au moment du formatage), **et** publie la valeur numérique telle quelle sur `/data` + WS.

> ⚠️ **CONTRAT DE SÉRIALISATION FIGÉ ([ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md), amendé par [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md))** : ces valeurs sont **écrites explicitement** dans l'énum, **jamais renumérotées**, **jamais insérées au milieu** — toute cause nouvelle s'**ajoute à la fin** (prochaine valeur disponible : **23**), une cause retirée reste **réservée, jamais recyclée**. **18 `static_assert`** (23 avant v2.68.0 — six retirés avec les codes ci-dessous, zéro ajouté) verrouillent la correspondance valeur par valeur en tête de [`pump_controller.cpp`](../../src/pump_controller.cpp), et un `static_assert` supplémentaire garantit que l'énum ne rejoindra jamais la sentinelle `kDoseBlockedCodeGuard` = 250.

| Code | `DoseRefusal` | Garde | Cause FR (réinjectée) |
|---:|---|---|---|
| 0 | `None` | — | *(aucun refus — publié `null`)* |
| 1 | `WatchdogInactive` | #2 | watchdog inactif |
| 2 | `FiltrationOff` | #3 | filtration arrêtée (présence d'eau résolue par `resolveWaterPresent`) |
| 3 | `ReadingNaN` | #4 | valeur officielle indisponible (**état du filtre indéterminé**) |
| ~~4~~ | ~~`FilterNotReady`~~ | ~~#4b~~ | **RETIRÉ en v2.68.0** (feature-152) — la garde séparée a disparu, jamais recyclé |
| ~~5~~ | ~~`FilterUnstable`~~ | ~~#4c~~ | **RETIRÉ en v2.68.0** — la médiane écarte les aberrations par construction, jamais recyclé |
| 6 | `CalibrationInsufficient` | #5 | calibration insuffisante (`cal=X, requis=Y`) |
| 7 | `StabilizationActive` | #6 | stabilisation post-calibration en cours |
| 8 | `MixingActive` | #6b | pause mélange en cours |
| 9 | `ModeNotAutomatic` | #7 | mode régulation != automatic (`X`) |
| 10 | `DailyLimit` | #8 | limite journalière pH / ORP atteinte |
| 11 | `HourlyLimit` | #9 | limite horaire atteinte |
| 12 | `CyclesPerDay` | #9 | limite cycles/jour atteinte |
| 13 | `BurstPerMinute` | #10 | anti-rafale : N cycles dans la dernière minute |
| 14 | `BurstPer15Min` | #10 | anti-rafale : N cycles dans les 15 dernières minutes |
| 15 | `ReadingNaNWarmup` | #4 | valeur officielle indisponible : fenêtre de lissage en cours |
| 16 | `ReadingNaNFrozen` | #4 | valeur officielle indisponible : **sonde figée** |
| 17 | `ReadingNaNStale` | #4 | valeur officielle indisponible : **plus aucune mesure acceptée** |
| 18 | `ReadingNaNGateClosed` | #4 | valeur officielle indisponible : mesures non représentatives |
| ~~19~~-~~22~~ | ~~`FilterWarmup`/`FilterFrozen`/`FilterStale`/`FilterGateClosed`~~ | ~~#4b~~ | **RETIRÉS en v2.68.0** — qualifiaient la garde #4b, qui a disparu avec eux. Jamais recyclés |
| ~~23~~ | ~~`CalibrationHold`~~ | — | **N'A JAMAIS ÉTÉ LIVRÉ** — envisagé pour `bug-160` (volet 2) au moment du mécanisme de bail (`cal_hold_logic`), **ABANDONNÉ avant tout déploiement** (décision utilisateur du 2026-10-13, remplacé par une bascule explicite de régulation en `manual`, voir [plus bas](#bascule-de-régulation-en-manuel-pendant-une-calibration-bug-160-volet-2)). Le code 23 n'est **jamais recyclé**, bien qu'aucune release n'ait jamais contenu `DoseRefusal::CalibrationHold` |
| 250 | *(sentinelle `kDoseBlockedCodeGuard`)* | — | cause posée par une **garde de sécurité pH/ORP** ou par l'escalade (hors `canDose`) |

**Aucun libellé ne mélange plus deux causes.** L'ancienne phrase « lecture pH/ORP filtrée indisponible (NaN) » recouvrait plusieurs causes physiques, et l'interface en extrayait un fragment de texte : une **vraie panne d'EZO** s'affichait « Stabilisation du filtre ». Le code 3 subsiste pour le cas où l'état du filtre n'a **pas** été collecté (`DoseFilterState::Unknown`) et dit alors explicitement qu'il ne sait pas — pire cas : une cause **imprécise**, jamais une panne maquillée en état normal. **Depuis v2.68.0** (feature-152), il n'existe plus qu'**une seule** famille de qualification (15-18, garde #4) : la garde #4b et ses quatre qualifications symétriques (19-22) ont disparu avec le mécanisme « filtre non prêt » séparé.

#### Qualification par l'état interne du filtre (`resolveFilterState`)

`SensorFilter::ready()` est faux pour **quatre** raisons physiquement distinctes. La fonction pure `resolveFilterState(frozen, gateClosed, stale, warmingUp)` ([`dosing_logic.cpp`](../../src/dosing_logic.cpp)) les compose en un `DoseFilterState`, **unique lieu de décision de priorité** :

```
frozen → Frozen ; sinon gateClosed → GateClosed ; sinon stale → Stale ;
sinon warmingUp → Warmup ; sinon Ready
```

- **Les pannes d'abord** : un défaut ne doit jamais être masqué par un état bénin concomitant. Après une reprise de circulation sur une sonde figée, le filtre est simultanément `warmingUp` **et** `frozen` — annoncer le warmup masquerait la panne.
- **`gateClosed` AVANT `stale`** : porte fermée, l'âge de la dernière mesure acceptée croît **par conception** (rien n'est ingéré). L'annoncer « périmé » accuserait la sonde d'un blocage voulu par le système, à chaque arrêt de filtration.
- **Jamais depuis `getPhRaw()`** : le cache brut est alimenté par **toute** lecture, donc reste frais sur une sonde figée ou un EZO débitant du garbage. Un discriminant fondé sur sa fraîcheur — proposé puis **refusé par le gate `pool-chemistry`** — aurait conclu « warmup » sur une panne, soit exactement le défaut corrigé.
- **Aucun effet sur le verdict** : `DoseInputs::filterState` ne sert qu'à qualifier la cause. Valeur par défaut `Unknown` (initialiseur de membre obligatoire : `DoseInputs in;` ne value-initialise pas l'agrégat) ⇒ retour aux causes génériques.

Collecte : `SensorManager::getPhFilterState()` / `getOrpFilterState()` — voir [sensors.md](sensors.md#prédicats-détat-du-filtre-observabilité-v2251).

#### Alerte de panne de mesure (v2.25.1)

Quand la cause de refus courante est une **panne** de la chaîne de mesure — `ReadingNaNFrozen`, `ReadingNaNStale` (seuls codes restants depuis le retrait de `FilterFrozen`/`FilterStale` en v2.68.0, feature-152) — `logRefusalOnce()` escalade en `warning` + alerte MQTT `ph_dose_sensor_fault` / `orp_dose_sensor_fault`, **une seule fois par épisode**, après `kDoseSensorFaultAlertDelayMs` = **60 s** de panne **continue**.

| Mécanisme | Règle | Motif |
|---|---|---|
| **Temporisation 60 s** | 12 cycles de lecture EZO avant d'alerter | à la réouverture de la porte du filtre, l'état interne est légitimement « périmé » pendant au plus un cycle. Sans délai, chaque démarrage de filtration criait « sonde muette ». L'**affichage** reste immédiat |
| **Épisode = panne CONTINUE** | la fenêtre n'est redatée **que** sur la transition non-panne → panne | un EZO agonisant alterne `Frozen` ↔ `Stale` par phases plus courtes que 60 s : redater à chaque bascule réarmait le droit d'alerter et **l'alerte ne partait jamais** — le correctif anti-panne-silencieuse introduisait une panne silencieuse (défaut relevé en revue) |
| **Une trace de journal** | `publishAlert()` journalise déjà en `warning`, y compris MQTT déconnecté | un `systemLogger.warning()` supplémentaire produisait deux lignes quasi identiques par épisode dans une partition `history` de 64 Ko |
| **Clôture** | `!sensorFault` ⇒ épisode clos, droit d'alerter **non** réarmé avant la prochaine transition ; `resetRefusalLogState()` remet tout à zéro | une panne ultérieure ré-attend la temporisation complète |

**Sémantique exacte de l'épisode.** Un épisode est une **plage de panne continue**, indépendante de la **nature** que la panne prend à l'intérieur. Trois états par pompe le décrivent : `_sensorFaultActive[]` (la panne était-elle en cours au tour précédent ?), `_sensorFaultSinceMs[]` (instant d'**ouverture** de l'épisode), `_sensorFaultAlerted[]` (droit d'alerter déjà consommé).

| Transition | Effet |
|---|---|
| non-panne → **panne** | ouverture d'un épisode : `_sensorFaultSinceMs = nowMs`, `_sensorFaultAlerted = false` — **seul** cas de datation |
| panne → **panne d'une autre nature** (`Frozen` ↔ `Stale`, `Filter*` ↔ `ReadingNaN*`) | **rien** : ni redatation, ni réarmement du droit d'alerter. Le compteur de 60 s continue de courir sur l'épisode entier |
| panne → **non-panne** | clôture : `_sensorFaultSinceMs = 0`, `_sensorFaultAlerted = false` |

Conséquences : (1) un EZO **agonisant**, alternant « figé » et « muet » par phases de moins d'une minute, déclenche bien l'alerte à 60 s cumulées — la première écriture, qui redatait à chaque changement de code, ne l'aurait **jamais** déclenchée (le correctif anti-panne-silencieuse contenait sa propre panne silencieuse) ; (2) l'alerte part **une seule fois** par épisode, quelle que soit la succession de natures ; (3) la comparaison `nowMs − _sensorFaultSinceMs >= kDoseSensorFaultAlertDelayMs` est en arithmétique **non signée** sur une **durée écoulée depuis l'instant d'ouverture** — forme wrap-safe imposée par [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md#pourquoi-la-forme-du-prédicat-compte-correctif-de-repliement-revue-v2251).

> 📋 Cette machine d'épisode vit aujourd'hui dans la coquille (`logRefusalOnce()`), donc **hors couverture native**. Son extraction en module pur est spécifiée par [feature-077](../../specs/features/todo/feature-077-extraction-machine-episode-panne-sonde.md) ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).

Les états **normaux** (`FilterWarmup`, `FilterGateClosed`, et leurs équivalents `ReadingNaN*`) n'émettent jamais sur ce type. Complémentaire de `ph_filter_not_ready` (5 min, conditionné à la filtration en marche et aux mesures représentatives) : cette alerte-ci qualifie la **nature** de la panne. Voir [MQTT.md](../MQTT.md#alertes).

#### Ordre d'écriture code / texte

Le **code est écrit avant le texte** dans `logRefusalOnce()` et dans `setGuardRefusalCause()`. Un lecteur asynchrone (`async_tcp`) qui verrait le nouveau texte avec l'ancien code afficherait une cause **périmée** ; dans cet ordre il voit au pire le nouveau code avec l'ancien texte, et le **code fait foi**. `resetRefusalLogState()` et `clearGuardRefusalCause()` remettent les deux à zéro ensemble (invariant code ⇔ texte).

#### Durée du refus courant par voie (feature-138, v2.54.0)

Incident fondateur (2026-10-02) : après des pluies diluviennes, les deux voies ont été déclarées instables et tout dosage automatique s'est arrêté, chlore compris. Le code de refus était déjà exposé ; sa **durée** ne l'était pas, et un relevé sans date a produit un diagnostic faux côté UI (« le calcul de dispersion est bloqué » alors qu'il avait déjà été recalculé trois fois) — voir [page-dashboard.md](../features/page-dashboard.md#bandeau--dosage-bloqué--feature-138-v2540).

- **`_refusalCodeSinceMs[2]`** — `millis()` du dernier **changement** de `_lastRefusalCode[]`, par voie. `0` n'y est **pas** une sentinelle « pas d'épisode » (contrairement à `_sensorFaultSinceMs` ci-dessus, qui collisionne avec la milliseconde 0 après boot et après repliement) : la validité est portée uniquement par `_lastRefusalCode[i] != kDoseBlockedCodeNone`.
- **`setRefusalCode(int pumpIndex, uint8_t code)`** — **seul point d'écriture** de `_lastRefusalCode[]`. Les quatre anciennes affectations directes (`logRefusalOnce()`, `resetRefusalLogState()`, `setGuardRefusalCause()`, `clearGuardRefusalCause()`) sont devenues des appels à ce setter ; un grep ne doit plus montrer qu'**une seule** affectation directe du tableau.
- ⚠️ **Invariant d'ordre — date AVANT code**, et c'est l'**ordre inverse** de la règle code-avant-texte ci-dessus : un lecteur async qui tombe entre les deux écritures du setter voit alors l'**ancien** code avec la **nouvelle** date — durée **sous-estimée**, bénigne — jamais le **nouveau** code avec l'**ancienne** date, qui annoncerait « bloqué depuis 9 h » sur un blocage d'une seconde. Chaque ordre protège contre le pire cas de lecture déchirée propre à son propre champ (texte périmé d'un côté, durée mensongère de l'autre).
- **`getPhDoseBlockedForS()` / `getOrpDoseBlockedForS()`** — durée en secondes depuis le dernier changement, bornée à `kDoseBlockedForMaxS` (24 h, `constants.h`) par le module pur [`doseBlockElapsedS()`](../../src/dose_block_duration_logic.h) (ADR-0017, header-only, testable nativement) : arithmétique **non signée**, exacte à travers le repliement de `millis()` (24,8 j) — aucune comparaison `now < since`, aucune branche de rattrapage. `0` est une durée **valide** (refus qui vient de commencer) ; c'est `*DoseBlockedCode != kDoseBlockedCodeNone`, jamais cette durée, qui décide de publier `null` côté JSON. Publiés à l'identique sur `/data` et le WebSocket (`phDoseBlockedForS` / `orpDoseBlockedForS`, parité obligatoire) — voir [ws-manager.md](ws-manager.md#champs-sensor_data-ajoutés-en-v2540--durée-du-refus-de-dosage-feature-138-et-élagage-de-ph_integralph_integral_term).
- Aucun mutex nouveau : écriture par `loopTask` seul, lecture alignée par `async_tcp` ; pire cas une durée en retard d'un tour de loop, jamais une décision de dosage (même course bénigne assumée que `_sensorFaultSinceMs`).

> ⚠️ **Invariant** : toute évolution future de la décision de dosage **passe par `dosing_logic`** et doit conserver l'équivalence stricte (table d'équivalence validée par `pool-chemistry`). La logique pure est verrouillée par les tests natifs (`test/test_native_dosing/`, dont un **verrou de non-régression** du bug pause-mélange v2.2.5 : une injection auto dure **≥ `minInjectionTimeMs`** avant que la pause ne s'arme). Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

### Calcul proportionnel pur (`computePidPure`)

[feature-037](../../specs/features/done/feature-037-dosage-proportionnel-testable.md) prolonge le pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), **pas de nouvel ADR**) au **cœur du calcul de débit**. Le calcul PID lui-même est extrait de `computePID()` vers une fonction **pure** [`computePidPure`](../../src/dosing_logic.h) ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)) — sans `<Arduino.h>`, sans `millis()`, sans état membre — donc **testable en natif**. *Characterization refactor* : **les débits calculés et l'évolution de l'intégrale sont strictement préservés** (équivalence validée par `pool-chemistry`).

**Ce que `computePidPure` calcule** (tout le métier) :

1. Terme proportionnel `kp × error`, plus termes `ki × integral` et `kd × (error − lastError) / dt`. Le terme dérivé est inerte partout (`Kd = 0`) ; le terme intégral est inerte côté ORP (`Ki = 0`) et **actif côté pH depuis la v2.26.0**.
2. **Anti-windup** : si `freezeIntegral` est vrai, l'intégrale est **gelée** (aucune accumulation) ; sinon elle est accumulée puis **bornée symétriquement** à `±integralMax`. ⚠️ **v2.26.0** : côté pH, `freezeIntegral` vaut **toujours `true`** — l'accumulation a lieu dans [`ph_integral_logic`](#terme-intégral-ph--levée-de-ki--0-v2260), `computePidPure` ne fait que servir le terme acquis. `integralMax` n'est plus la valeur inerte de 50 mais **43 200 / 16 200 / 7 200 pH·s** selon la vitesse.
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
| 4 | **Injection automatique en cours** (bug-161, gate C1) | `PumpController.isPhDosing()` / `isOrpDosing()` vrai **sur cette pompe** | — |
| 5 | Double démarrage | injection manuelle déjà active sur cette pompe | — |
| 6 | Limite journalière **prédictive** | `cumul + volume demandé > max` (limite ≤ 0 = illimité) | `==` **acceptée** (strict `>`) ; `remaining_ml` renseigné dans le refus |
| 7 | Limite horaire **prédictive** | `usedMs + durée demandée > limite` (limite 0 = illimité) | `==` acceptée (strict `>`) ; budget **partagé** avec l'auto ([ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) |
| 8 | Cycles/jour | `cyclesToday >= maxCyclesPerDay` (compteur partagé auto + manuel) | `>=` (identique à l'auto) |
| 9 | Anti-rafale 1 min | `>= 6` démarrages dans la fenêtre (ring partagé) | `>=` |
| 10 | Anti-rafale 15 min | `>= 20` démarrages dans la fenêtre (ring partagé) | `>=` |

**Volontairement absents** (le manuel est aveugle à la mesure — l'opérateur assume la décision chimique) : gardes NaN / filtre capteur / calibration / mode de régulation / pause mélange. Le volume/durée évalués sont **post-plafonnement** `kManualInjectMaxDurationS` (condition pool-chemistry #3 : on évalue ce qui sera vraiment injecté).

### Garde rang 4 — injection automatique en cours (bug-161, gate C1)

⚠️ **Ce trou existait depuis feature-006, mais était INATTEIGNABLE** : `web_routes_control.cpp`
ne regardait jusqu'ici que `manualInjectPh.active` / `manualInjectOrp.active` (double démarrage
**manuel**, rang 5) — jamais un dosage **automatique** en cours. En pratique, cela ne mordait
jamais : le bouton d'injection manuelle restait **masqué** tant que le mode n'était pas `manual`
(`app.js`, `updatePhModeControls()`/`updateOrpModeControls()`), et le mode automatique ne pouvait
donc jamais être actif en même temps que le bouton visible.

**feature-161 (v2.77.0) lève ce masquage** — « Manuelle » cesse d'être un mode, l'injection
manuelle devient accessible dans les **trois** modes de régulation (voir
[page-ph.md](../features/page-ph.md#injection-manuelle-déconditionnée-du-mode-feature-161-v2770)).
Sans garde nouvelle, on aurait pu injecter à la main **par-dessus** un dosage automatique en cours
sur la **même pompe**. Le gate `pool-chemistry` a donc imposé une garde `AutomaticInjectInProgress`
**avant** de démasquer le bouton, pas après :

> **C'est le défaut le plus intéressant du lot** : il n'existait pas, il était *caché* par une
> restriction d'affichage que l'on s'apprêtait à lever. Une simplification d'interface a failli
> ouvrir un chemin de dosage.

**Pourquoi le rang 4, précisément** (`web_routes_control.cpp:103`, `dosing_logic.h` /
`dosing_logic.cpp`) :

- **Derrière** Watchdog (1) et Présence d'eau (2) : une cause **absolue** (rien ne doit jamais
  injecter sans watchdog ou sans eau) ne doit jamais être masquée par une cause plus bénigne.
- **Devant** le double démarrage manuel (5) et les quotas (6-10, [ADR-0095](../adr/0095-une-cause-de-garde-cite-un-seuil-pas-une-mesure.md)
  : « la cause annoncée doit être celle qui mord en premier ») : un chevauchement avec l'automatique
  est une cause plus informative qu'un quota, s'il se produit en même temps.

⚠️ **Contrat de numérotation verrouillé par `static_assert`** (`dosing_logic.cpp`) : l'**ordinal**
de `ManualInjectRefusal::AutomaticInjectInProgress` (`= 10`, en **fin d'énumération**, pour ne
renuméroter aucune valeur existante — elles sont persistées indirectement via le code machine
JSON du refus) est **indépendant** du **rang d'évaluation** (4, documenté par les commentaires
numérotés d'`evaluateManualInject()`). Toute cause future s'ajoute à la fin de l'énum, quel que
soit son rang d'évaluation réel — ne **jamais** renuméroter ni insérer au milieu.

**Texte de refus** (409 JSON, `web_routes_control.cpp`) : `automatic_inject_in_progress` —
« Une injection automatique est en cours — réessayez dans un instant. » Voir
[API.md](../API.md#codes-de-refus-409-v260).

**Testabilité native** : 5 tests dédiés (`test/test_native_dosing/test_dosing_decision.cpp`,
préfixe `test_F161_*`) — refus seul, devant les quotas (5-10), derrière Watchdog (1) et
FiltrationOff (2), devant AlreadyInjecting (5). `evaluateManualInject` compte désormais **22**
tests dédiés au total (17 `test_F006_*` + 5 `test_F161_*`, était 17 avant ce lot).

⚠️ **bug-160 (volet 2, REFONTE 2026-10-13)** : pas de garde dédiée ici pour la calibration — une
injection manuelle déjà active est **interrompue immédiatement** par `POST /calibration/enter`
(hors de cette liste, C6 du gate `pool-chemistry` : « le seul trou que le bail couvrait et que la
bascule ne couvre pas »), et une **nouvelle** demande manuelle pendant une calibration n'a plus
besoin d'un rang dédié : la régulation ORP/pH est en `manual`, et `evaluateManualInject` reste
**volontairement aveugle au mode** — rien ne l'empêche donc de démarrer. La protection du bassin
repose sur l'interruption à l'entrée, pas sur un refus à la demande suivante.

**Budget horaire partagé ([ADR-0020](../adr/0020-budget-horaire-dosage-unique.md))** : avant feature-006, le manuel ne contournait pas seulement la limite horaire — il n'était **pas compté** dans `usedMs` (jusqu'à 2× le budget/heure possible). Depuis la **v2.21.0** ([ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)), `refreshDosingState(state, now, pumpOn)` (privée, [`pump_controller.cpp`](../../src/pump_controller.cpp)) accumule `usedMs` sur le prédicat **POMPE-ON** (pompe physiquement alimentée — pouls auto/scheduled, injection manuelle web, **pompes test** `/pumpN/on`) : entre deux pouls d'une session, le budget n'est **pas** consommé (« minutes où le produit coule », validé gate). Toute future source d'activation passant par `applyPumpDuty()` est comptée par construction. Point d'accumulation unique → pas de double comptage.

**Enregistrement de cycle manuel (pending atomique)** : à l'acceptation, la route appelle `requestManualCycleRecord(pumpIndex)` (incrément `std::atomic<uint8_t> _manualCycleStartPending[2]`, thread-safe depuis `async_tcp`). Le pending est consommé **en tête de `update()`** ([`pump_controller.cpp:657`](../../src/pump_controller.cpp:657)), en `loopTask`, avant tout early-return : `recordDosingCycleStart()` (ring anti-rafale **partagé** avec l'auto) + `cyclesToday++` + flush NVS différé. Le ring et `cyclesToday` restent ainsi écrits par **une seule tâche** (pas de mutex). Les getters `getCyclesToday()` / `getRecentCycles()` **ajoutent le pending** non consommé : deux starts manuels rapprochés se voient mutuellement (fail-closed). Lectures snapshot sans mutex : compteurs écrits en `loopTask` uniquement, lectures 32 bits atomiques sur ESP32 (course bénigne : au pire une valeur en retard d'un tour de loop).

**Asymétrie stabilisation assumée (condition pool-chemistry #5)** : la boucle **auto** suspend les **2 pompes** dès qu'une stabilisation est active (garde globale d'`update()`), alors que l'injection **manuelle** n'est bloquée que par la fenêtre de **sa** pompe (`getStabilizationRemainingS(pumpIndex)`) — les fenêtres sont indépendantes par sonde (cinétiques différentes, 5 min pH / 3 min ORP).

**Garde watchdog et tâches** : les handlers AsyncWebServer tournent dans `async_tcp`, non abonnée à la TWDT — `esp_task_wdt_status(NULL)` y renverrait toujours NOT_FOUND. La garde interroge donc `esp_task_wdt_status(loopTaskHandle)` (la tâche qui exécute le dosage).

**Routes de test `/pump1/on`, `/pump2/on`** : gardées **uniquement** sur la filtration (helper `injectionAllowedOrReject()`, pas de volume demandé → pas de garde volumétrique), désormais au **même format 409 JSON** (`filtration_off`). Leur temps de marche consomme le budget horaire partagé (cf. ci-dessus).

> ✅ **Bypass `pump_test` UART : FERMÉ en v2.17.0 (feature-065)**. Jusqu'en v2.16.x, l'action `run_action/pump_test` du protocole écran LVGL (`uart_commands`, supprimé) appelait `setManualPump()` **sans passer par `evaluateManualInject()`** — bypass assumé car l'écran n'a jamais été déployé (condition pool-chemistry #4, feature-006 : « à garder si l'écran est mis en service »). Le retrait complet du support écran supprime cette voie non gardée : **toutes** les activations manuelles restantes passent par les gardes web.

**Arrêt cyclique (v2.1.2, conservé)** : `updateManualInject()` (appelée chaque tour `loopTask`) interrompt l'injection si la filtration tombe pendant celle-ci. Latence < 100 ms. Émet un log `critical("[Injection] {pH|ORP} INTERROMPUE — filtration arrêtée (sécurité chimique)")` + alerte MQTT `ph_injection_aborted` ou `orp_injection_aborted` sur `{base}/alerts`. **Pas de reprise automatique** : l'injection est perdue, l'utilisateur relance manuellement (toast UI explicite, voir [`docs/features/page-ph.md`](../features/page-ph.md) et [`docs/features/page-orp.md`](../features/page-orp.md)).

**Routes d'arrêt** (`/ph/inject/stop`, `/orp/inject/stop`, `/pump[12]/off`) : **inconditionnelles** — pouvoir arrêter en toute circonstance, y compris pendant une stabilisation.

**Crédit plancher à la fin naturelle (v2.9.1)** : le cumul journalier de sécurité (`dailyPhInjectedMl` / `dailyOrpInjectedMl`) est alimenté par **intégration débit × temps** (`updateSafetyTracking` à l'époque — depuis v2.21.0 : `accountPumpVolume` + `creditInjectedVolume`, voir [Comptabilité du volume](#comptabilité-du-volume-p4)), qui perd systématiquement quelques dixièmes de mL aux bornes d'une injection manuelle volumée : arrondi de `durationS` à la seconde entière, tranche entre le démarrage réel (handler async) et le premier tick de comptage, et surtout **dernière tranche jamais comptée** (`updateManualInject()` coupe la pompe à l'expiration → au tick suivant le delta final est jeté). La garde d'acceptation raisonnant, elle, en **volume exact** (garde #6, frontière `==` acceptée), demander exactement le reliquat du quota (limite 50 mL, injection 50 mL) laissait le cumul à 49,x → `ph/orp_limit_reached` jamais latché, badge dashboard « Cumul 98 % » figé.

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
- Exécution en `loopTask` uniquement (mêmes règles d'accès aux compteurs que la comptabilité de volume : écrivain unique, pas de mutex) ; ajustement effectif → `_dailyCountersDirty` armé (flush NVS différé).
- **Logs `info`** : `[Sécurité] Compteur journalier pH|ORP ajusté de X à Y mL (crédit fin d'injection manuelle)` (émis seulement si un ajustement a réellement eu lieu) ; le log de fin de durée mentionne désormais le cumul crédité : `[Injection] pH|ORP arrêtée automatiquement (fin de durée) — cumul crédité à X mL`.

**Écrêtage au reliquat journalier (v2.9.2)** : les routes `/ph|orp/inject/start` ([`web_routes_control.cpp:297`](../../src/web_routes_control.cpp:297) et [`:392`](../../src/web_routes_control.cpp:392)) n'opposent plus le refus `daily_limit` quand la demande dépasse le reliquat (`max*MlPerDay − daily*InjectedMl`) : la demande est **écrêtée au reliquat** — durée recalculée arrondie **vers le bas** (`floor` volontaire : le volume effectif ne re-déborde jamais la limite), log `info` `[Injection] pH|ORP : volume écrêté de X à Y mL (reliquat journalier)`. Interaction avec le crédit plancher ci-dessus :

- **écrêtage sans re-plafonnement durée** : `creditMl = remaining` (reliquat **entier**, pas `effectiveMl`) → à la fin naturelle, le plancher porte le cumul **exactement à la limite** → latch `ph/orp_limit_reached` + badge « Limite journalière atteinte ». Sur-compte borné à < 1 s de débit (conservateur, cohérent avec la propriété fail-safe v2.9.1) ;
- **double-clamp** (reliquat > 10 min de pompe) : le plafond `kManualInjectMaxDurationS` gagne → `creditMl = effectiveMl` — le crédit n'excède **jamais** ce qui a pu être réellement injecté.

Le refus `daily_limit` ne subsiste que si le reliquat est **nul ou < 1 s de pompe** ; son message et son champ `remaining_ml` arrondissent désormais **vers le bas** (`floorf`, plus jamais de « reste 11 mL » qui refuse 11). La garde pure `evaluateManualInject()` est **inchangée** (frontière `==` toujours verrouillée par test natif) : l'écrêtage est fait en amont dans le handler, symétrique pH/ORP et identique pour le chemin legacy `?duration=`.

**Bornage durée** : `duration` query param plafonné à `kManualInjectMaxDurationS = 600 s` (10 min, abaissé de 3600 s en v2.1.2). Justification `pool-chemistry` : 3600 s expose à un risque trop long si la filtration s'arrête en milieu de cycle ; 10 min couvrent les usages typiques.

> Cohérence : la garde filtration reproduit **exactement** la condition #3 de `canDose()` — elle consomme la même source unique `filtration.resolveWaterPresence().waterPresent` (feature-056). En mode `powered` (ex-`continu`), l'alimentation 12 V des pompes suit la filtration au niveau matériel → présence d'eau présumée `true` en permanence ; en `external`, la présence dépend du signal récent (fail-safe OFF).

**Testabilité native** : `evaluateManualInject` est couverte par **22 tests** dédiés (`test/test_native_dosing/test_dosing_decision.cpp`, préfixes `test_F006_*` (17) et `test_F161_*` (5, garde rang 4 — voir [section dédiée](#garde-rang-4--injection-automatique-en-cours-bug-161-gate-c1)) — 148 `RUN_TEST` au total dans ce fichier au 2026-10-13, à **mesurer** plutôt qu'à recopier, cf. CLAUDE.md ADR-0073) : une cause de refus par test + nominal, reliquat, frontières `==` acceptées, limites 0/≤0 = illimité, ordre de priorité des gardes, partage du ring anti-rafale avec l'auto. `dosing_logic.cpp` reste à **100 % des lignes**.

### Ring buffer anti-rafale

Chaque pompe maintient un ring buffer `_dosingCycleHistory[2][kDosingCycleHistorySize = 20]` indexé par `_dosingCycleHistoryIdx[2]`. À chaque démarrage effectif d'un cycle de dosage (transition PWM 0 → >0), `recordDosingCycleStart(pumpIndex)` ajoute le timestamp `millis()` courant.

Au prochain `canDose(pumpIndex)`, `countRecentDosingCycles(pumpIndex, windowMs)` compte les entrées dans la fenêtre `[now - windowMs, now]` :
- `kMaxDosingCyclesPerMinute = 6` cycles max sur 60 000 ms
- `kMaxDosingCyclesPer15Min = 20` cycles max sur 900 000 ms

Sites d'appel à `recordDosingCycleStart()` (4 au total) : démarrage cycle pH automatique, démarrage cycle pH scheduled, démarrage cycle ORP automatique, démarrage cycle ORP scheduled.

> **Indépendant** des limites volumétriques (`ph_limit_minutes`, `max_ph_ml_per_day`) et de l'anti-cycling existant (`maxCyclesPerDay`, 24 par défaut — configurable 10-60, feature-060 — mesuré sur 24 h glissante). Couvre le cas d'un PID qui démarrerait 30 cycles très courts en 5 minutes (lectures bruitées sur sonde mal calibrée par exemple).

## Terme intégral pH — levée de `Ki = 0` (v2.26.0)

> Décision structurante : [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md), qui **amende le `Ki = 0` de l'[ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md)** pour le **seul pH**. `Kd = 0` et `Ki_ORP = 0` sont **inchangés**.

### Pourquoi : un P pur ne peut pas rejoindre sa consigne sous charge permanente

Le pH ne descendait jamais à sa consigne : ~1 000 mL d'acide injectés dans la journée, pH **tenu à 7,50-7,66** pour une consigne à 7,30. Ce n'était **ni un défaut du firmware, ni un problème de dosage**, mais une propriété du correcteur. Un proportionnel pur soumis à une charge permanente se stabilise sur son **droop** :

```
e* = besoin / (kp × T)
```

Le terme P **est** la seule source de débit : pour délivrer un débit non nul, il lui faut une erreur non nulle. Vérification numérique avec `kp = 8` : `e* = 0,358` ⇒ 2,86 mL/min ⇒ 1 800 mL/j pour 628 min ≈ **10,5 h de filtration/jour** — exactement le régime de l'installation. **Le système était à son équilibre théorique.**

Charge basique quotidienne mesurée : **CO₂ (dégazage) ≈ 80 %**, **chlore (hypochlorite, basique) ≈ 20 %**. Besoin ≈ **1 800 mL/j pour TENIR**, ≈ **2 700 mL/j** pour descendre de 0,2.

### Dimensionnement par l'AUTORITÉ, jamais « en pH·s »

On pose le **débit maximal que l'intégrale seule est autorisée à demander**, puis on en déduit le plafond d'anti-windup :

```
kPhIntegralAuthorityMlPerMin = 6,0 mL/min     (2 × la charge d'équilibre 2,86)
ki          = kp / Ti
integralMax = U_I,max / ki  =  U × Ti / kp
```

| Vitesse | `kp` | `Ti` | `ki` (mL/min par pH·s) | `integralMax` (pH·s) |
|---|---:|---:|---:|---:|
| `slow` | 4 | 8 h | 1,389e-4 | 43 200 |
| `normal` (défaut) | 8 | 6 h | 3,704e-4 | **16 200** |
| `fast` | 12 | 4 h | 8,333e-4 | 7 200 |

`integralMax` **varie avec la vitesse de régulation**, exactement comme `kp` : l'autorité est constante, seule la **vitesse de charge** change. **9 `static_assert`** verrouillent l'ensemble, dont le verrou central `ki × integralMax ≈ U` pour les trois vitesses, la monotonie de `kp`/`Ti`, et `Ti_fast ≥ 4 × kPhMixingDelayMs`.

> ⚠️ **Le piège évité.** L'ancien `integralMax = 50` (inerte tant que `Ki = 0`) aurait plafonné la contribution intégrale à `3,704e-4 × 50 = 0,0185 mL/min` pour un besoin de 2,86 — **0,6 %**, saturé en **140 s**. Réactiver `Ki` sans redimensionner `integralMax` aurait été un **échec silencieux** : intégrale sur le papier, droop intact dans le bassin.
>
> **À l'équilibre l'erreur tend vers 0, donc le terme P disparaît et l'intégrale porte TOUTE la charge.** Son autorité doit couvrir la charge, pas une correction d'appoint.

### Intégration SIGNÉE sur `erreur − phStopThreshold`

```
dI/dt = (erreur − phStopThreshold)
```

évaluée à **chaque tour de la branche automatique**, que l'on dose ou non. La **sortie**, elle, reste gatée par `shouldDose` et par la bande morte : entretenir l'intégrale n'autorise aucune injection.

- **La décharge devient structurelle** : dès que l'erreur passe sous le seuil d'arrêt, l'intégrande devient négatif. L'équilibre s'établit à **`consigne + phStopThreshold`**, c'est-à-dire au point exact où l'hystérésis de dosage coupe — correcteur et ordonnanceur visent enfin le même point. **Résidu attendu : +0,01 pH.**
- **Par quanta d'une seconde** (`kPhIntegralChargeQuantumMs`) : à 16 200 pH·s, l'*ulp* du flottant simple précision vaut ~2e-3 ; un incrément de 5 ms × 0,01 pH (5e-5) serait **perdu à l'addition** — la décharge n'aurait pas existé. Le reliquat sous le quantum est **reporté**, aucun temps n'est perdu.
- **`dt > 10 s` rejeté** (`kPhIntegralMaxTickDtMs`) : un trou d'évaluation (pause mélange de 15 min, garde bloquante, changement de mode) n'est **jamais** crédité d'un bloc.
- **Gel ASYMÉTRIQUE** : l'anti-windup bloque la **charge**, jamais la **décharge**. Strictement plus sûr que l'ancien gel total.

> **Défaut préexistant corrigé au passage.** La bande morte valait `phStartThreshold` (0,05) alors que l'hystérésis poursuit jusqu'à `phStopThreshold` (0,01) : la plage `]0,01 ; 0,05]` était une **session vivante à débit nul**, et le point fixe de l'intégration signée était **inatteignable**. Voir la note de l'[algorithme, point 3](#algorithme-résumé). À l'équilibre, le volume total injecté est **inchangé** (bilan de matière).

### Module pur `ph_integral_logic` ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md))

Toute la logique vit dans [`src/ph_integral_logic.h`](../../src/ph_integral_logic.h) : charge signée, oubli exponentiel, reset sec, péremption, hystérésis de saturation, paliers et rappel. **Motif de l'extraction** : les conditions C2-C5 du gate vivaient dans la coquille ESP32 et **aucune mutation ne les tuait** (« restaurer le reset à −0,01 », « supprimer la décroissance » laissaient la suite native verte) — et le défaut trouvé était **exactement** dans la garde de C2.

| Élément | Rôle |
|---|---|
| `PhIntegralState` | POD sans allocation, porté par `PumpControllerClass`. Écrit/lu depuis la **seule** `loopTask` → aucun mutex. |
| `PhIntegralTuning` | `kp`, `ki`, `integralMax`, `startThreshold`, `stopThreshold` — passés en paramètre, ce ne sont pas des constantes firmware. |
| `phIntegralTick()` | Un tick : base de temps → reset sec **temporisé** → demande + gel → charge signée → oubli → hystérésis → anomalie. Rend un `PhIntegralVerdict`. *(v2.27.0 : la base de temps passe **devant** le reset sec, qui a besoin du `dt` pour accumuler sa tenue.)* |
| `phIntegralExpire()` | Péremption 24 h, appelée **hors de toute conjonction d'éligibilité**. |
| `phSustainableAvgFlow()` | `ampFlow × budget_min/60` — le débit moyen réellement **tenable**. |
| `phIntegralFillPct()` | Remplissage de l'anti-windup en % (valeur absolue), exposé à `/data`, au WS et à MQTT. |

La coquille (`tickPhIntegralExpiry`, `applyPhIntegralVerdict` dans [`pump_controller.cpp`](../../src/pump_controller.cpp)) ne fournit que le **temps** et **applique** le verdict (journal + alerte MQTT). `phPID.integral` n'est plus qu'un **miroir** de `_phIntegral.integral`, consommé par `computePidPure` et par les accesseurs.

### Garde-fous

| Garde | Règle |
|---|---|
| **Gel sur saturation d'actionneur** | Accumulation gelée si `demande ≥ ampFlow × budget_horaire / 60`. ⚠️ Le prédicat porte sur le **débit moyen soutenable** (~2 mL/min à 5 min/h), **pas** sur le débit d'amplitude (~25 mL/min) : comparé à l'amplitude, ce gel ne s'était **jamais** déclenché et ne protégeait de rien. |
| **Gel sur quota journalier** | Au-delà de **80 %** du quota (`kPhIntegralFreezeDailyFraction`). Couvre la sonde bloquée sur une valeur haute (plage et fraîcheur valides — le filtre la croit bonne) : le volume déjà injecté est le seul témoin fiable. Le terme acquis reste servi, on n'en ajoute plus. |
| **Gel sur quota de CYCLES épuisé** *(v2.27.0)* | `cyclesToday ≥ maxCyclesPerDay`. L'anti-cycling était traité comme une suspension rythmique alors qu'il ne se relève **qu'au basculement de 24 h**, exactement comme le quota volumique. Épuisé à 10 h du matin, il laissait l'intégrale charger **neuf heures** sur une erreur que l'actionneur n'a plus le droit de résorber (~1 300 pH·s de windup pur, servis d'un bloc au premier cycle du lendemain). C'est un **gel**, surtout pas une fermeture du prédicat d'accumulation : la voie de décharge reste ouverte. |
| **Reset sec** | `erreur < −(phStartThreshold + kPhIntegralResetMargin)`, soit **−0,15** par défaut : une excursion **franche** de l'autre côté de la consigne, pas le bruit du filtre (~0,05). *Avant la v2.26.0 le reset mordait dès −0,01 — six heures d'accumulation effacées par 0,011 pH de dérive.* **v2.27.0 : le seuil est inchangé, la purge est TEMPORISÉE** — voir [ci-dessous](#2-le-reset-sec-est-temporisé-15-min-de-temps-de-branche). |
| **Oubli exponentiel** | Demi-vie **1 h** sous la consigne, par **pas fixes** de 1 min (`powf()` coûte 2 320 o de flash sur une ressource à 97 %) ; facteur `0,9885140` vérifié par `static_assert`, rattrapage borné à 600 pas. Épisode fermé sous 1 pH·s. |
| **Péremption** | Purge après **24 h** sans évaluation (`kPhIntegralStaleResetMs`) : mode `off`/`manual`, garde bloquante durable, filtration coupée tout l'hiver. Appelée **hors de toute conjonction d'éligibilité**, après les sorties OTA/maintenance mais **avant** les sorties « capteurs non initialisés » / « eau absente ». |
| ~~**Aucune persistance NVS**~~ → **persistance CONDITIONNELLE** *(v2.27.0)* | L'interdiction de l'ADR-0052 est **amendée** par l'[ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) : l'intégrale est persistée sous **sept garanties cumulatives R1-R7**, et la valeur relue **n'est jamais appliquée au démarrage**. Voir [ci-dessous](#3-persistance-nvs-conditionnelle-r1-r7). |

**Points de purge de l'intégrale** (tous appellent `phIntegralReset()` **et** remettent le miroir `phPID` à zéro) :

| Événement | Motif |
|---|---|
| Changement **réel** de vitesse de régulation | `Ki` change d'un facteur jusqu'à 2,25 : servir l'intégrale acquise avec le nouveau `Ki` produirait jusqu'à 13,5 mL/min, **plus du double de l'autorité**. La purge a lieu **AVANT** l'écriture de `ki`/`integralMax` : quel que soit l'entrelacement vu par `loopTask`, le terme vaut `ki_quelconque × 0 = 0`. ⚠️ La purge n'a lieu **que si les coefficients changent réellement** — le client renvoie `regulation_speed` à chaque enregistrement des réglages, purger à chaque appel reproduirait le défaut C2. |
| Bascule `ph_minus` ↔ `ph_plus` | Le **signe de l'erreur s'inverse** : une intégrale chargée pour acidifier deviendrait une demande d'alcaliniser de même amplitude. |
| `resetDosingStates()` | `phPID = {}` restaure les **défauts de structure** (`Ki = 0`, `integralMax = 50`) : sans ré-application, la réinitialisation **désarmerait silencieusement** le terme intégral et laisserait l'ORP à `kp = 8` (26× trop agressif). `applyRegulationSpeed()` est donc rappelée. |
| Fin de branche `scheduled` | Cette branche est **hors PID** — elle dose une quantité planifiée, aveugle à la mesure. Une intégrale entretenue pendant des jours de mode Programmée serait servie d'un bloc au retour en automatique. |
| Péremption 24 h | Voir ci-dessus. |

### Surveillance de saturation (C5)

« La régulation demande son maximum **SANS atteindre la consigne** » se dit en **deux termes** : demande **saturée** ET erreur **encore au-dessus du seuil d'arrêt**. Une demande saturée avec l'erreur déjà rentrée est un transitoire de fin de correction, pas un défaut.

- Le prédicat porte sur le **rapport demande / débit soutenable** — et non sur le remplissage de `integralMax`. Le gel d'anti-windup immobilise l'intégrale à la valeur qui sature l'actionneur, très inférieure au plafond (~34 % avec 5 min/h de budget) : un seuil sur `integralMax` **ne serait jamais atteint**.
- **Hystérésis 95 / 85 %** : sans bande, une demande oscillant autour du seuil ouvrirait et refermerait l'épisode en rafale et réarmerait le rappel à chaque oscillation.
- La tenue se compte en **temps de branche automatique**, jamais en temps mural : un passage en `manual`, une garde bloquante ou un hiver sans filtration **suspendent** l'épisode. **Cette base de temps est verrouillée par test natif et n'est pas négociable** — c'est la seule qui suspende l'épisode au lieu de le faire mûrir sur une erreur périmée.
- Paliers **recalibrés en v2.27.0** (le tarif de la base de temps a changé, l'intention est de préserver la **fenêtre de preuve en temps réel** validée en v2.26.0) : `warning` à **2 h 20**, `critical` à **7 h**, **rappel du critical toutes les 14 h**. Voir [Recalibration des paliers](#recalibration-des-paliers-dalerte-de-saturation). *Une seule alerte par épisode laissait un silence **permanent** sur un état qui, lui, ne cessait pas.*
- Alerte MQTT `ph_integral_saturated` (voir [MQTT.md](../MQTT.md#alertes)).

### Observabilité

Trois grandeurs **lecture seule**, publiées à l'identique sur `GET /data`, sur la trame WebSocket **et** sur MQTT (mêmes noms, mêmes arrondis, `NaN` → `null` / non publié) :

| Champ | Unité | Arrondi | Sens |
|---|---|---|---|
| `ph_integral` | pH·s (signé) | 1 déc. | Intégrale accumulée |
| `ph_integral_term` | mL/min | 3 déc. | `ki × intégrale` = contribution au débit demandé |
| `ph_integral_pct` | % | 1 déc. | Remplissage de l'anti-windup (valeur **absolue**) |

Détail : [API.md](../API.md#champs-du-terme-intégral-ph-v2260), [ws-manager.md](ws-manager.md), [MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-v2260-terme-intégral-ph), affichage : [page-ph.md](../features/page-ph.md#rattrapage-accumulé-par-la-régulation-ph-v2260).

### Journal

| Message | Niveau | Quand |
|---|---|---|
| `PID régulation: vitesse=… Kp_pH=… Ki_pH=… (Ti=…h, intMax=… pH·s, autorité=… mL/min) Kp_ORP=… Ki_ORP=0 Kd=0` | `info` | À chaque `applyRegulationSpeed()`. ⚠️ **Format changé** : l'ancien `PID régulation (P temporisée): … Ki=0 Kd=0` n'existe plus — un filtre de journal sur « P temporisée » ou « Ki=0 » ne matche plus. |
| `Intégrale pH remise à zéro (changement de vitesse de régulation) : N pH·s abandonnés` | `info` | Changement **réel** de coefficients |
| `Intégrale pH purgée (erreur … — pH franchement au-delà de la consigne depuis plus de 15 min) : N pH·s abandonnés` | `info` | Reset sec. ⚠️ **Format changé en v2.27.0** : le fragment « depuis plus de N min » est ajouté — une purge sèche est désormais la conclusion d'une excursion **soutenue**, pas la réaction à un à-coup. Un filtre de journal sur l'ancienne phrase exacte ne matche plus. |
| `Intégrale pH périmée (aucune évaluation depuis 24 h) : N pH·s purgés` | `info` | Péremption |
| `Régulation pH saturée depuis plus de 2 h (… mL/min demandés pour … soutenables) — la correction n'a pas d'effet mesurable sur le pH` | `warning` | Palier warning (+ alerte MQTT). *v2.27.0 : le palier passe à **2 h 20** de temps de branche — le texte affiche la valeur courante.* |
| `Régulation pH saturée depuis plus de N h (…, terme intégral … mL/min) — … vérifier le niveau et l'amorçage du bidon, l'étanchéité du circuit de dosage, la sonde pH et la limite horaire d'injection` | `critical` | Palier critical, **répété toutes les 12 h** (+ alerte MQTT) |
| `Intégrale pH sortie de saturation (…) — alerte levée` | `info` | Sortie d'hystérésis |
| `PID pH configuré: Kp=… Ki=… Kd=… (integralMax INCHANGÉ=… pH·s)` | `info` | `setPhPID()` — **aucun appelant à ce jour** ; le suffixe rappelle que ce chemin ne redimensionne pas le plafond |

### Testabilité

Suite native dédiée [`test/test_native_ph_integral/`](../../test/test_native_ph_integral/). Total du projet **823 → 849 tests**, et **cinq mutations tuent désormais** — dont **trois qui n'en tuaient aucune** avant l'extraction du module pur. Le test de **droop** vérifie la propriété qui fonde la décision : en boucle fermée, un P pur **ne converge pas**, un PI **converge**.

## Cadence, mémoire et charge observée de l'intégrale pH (v2.27.0)

> Décisions structurantes : [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) (persistance **conditionnelle**, qui **amende la clause « aucune persistance NVS » de l'[ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)**) et [ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) (un seuil de vraisemblance dérivé d'un réglage est une **fonction** de ce réglage). Gate `pool-chemistry` : **GO**, 30/08/2026.
>
> ⚠️ **Non vérifié sur le terrain** au moment de la rédaction : le déploiement OTA n'a pas eu lieu.

### ⚠️ Le point de fond : l'installation est limitée par l'ACTIONNEUR, pas par le correcteur

Ce constat est **plus important que la feature elle-même** et conditionne la lecture de tout ce qui suit.

À **5 min/h** de limite horaire et **~26 mL/min** de débit d'amplitude calibré, le maximum **physiquement délivrable** vaut :

```
5/60 × 26  ≈  2,17 mL/min      ⇒  ~911 mL/jour sur 7 h de filtration
```

pour un besoin de **~1 800 mL/jour**. **Ni la feature-084 ni la feature-085 ne peuvent combler cet écart** : elles agissent sur le **correcteur**, pas sur la vanne. Une régulation parfaite ne délivre pas plus que ce que l'actionneur laisse passer.

Deux corollaires :

1. **L'alerte « régulation saturée » est chimiquement VRAIE et LÉGITIME.** Elle désigne la **limite horaire**, pas un bidon vide ni une sonde en panne. La traiter comme un faux positif à faire taire serait une erreur de diagnostic.
2. ⚠️ **Relever `phInjectionLimitMinutes` est le seul levier réel — et il n'est pas neutre.** Il augmente **proportionnellement** le volume injecté pendant les **15 minutes de retard de mélange** (`kPhMixingDelayMs`), fenêtre pendant laquelle la boucle dose **à l'aveugle** sur une mesure qui ne reflète pas encore les doses passées :

| Limite horaire | Volume injecté pendant les 15 min de retard | Excursion pH correspondante |
|---|---:|---:|
| **5 min/h** (défaut d'usine ≤ v2.29.x) | 130 mL | **0,046** |
| **10 min/h** (défaut d'usine depuis v2.30.0, feature-087 — visa conditionné à un bassin ≥ ~20 m³) | 260 mL | **0,092** |
| 30 min/h | 780 mL | **0,28** |

à comparer à `kPhSafetyMargin` = **0,50**. À 30 min/h, un seul temps mort consomme **56 %** de la garde de sécurité. **Ce réglage ne doit pas être relevé sans en tenir compte.**

Suivi : [feature-086](../../specs/features/done/feature-086-limite-actionneur-dosage-ph.md) (limite d'actionneur) et [feature-085](../../specs/features/done/feature-085-dose-de-base-apprise-feedforward.md) (dose de base apprise, **livrée en v2.28.0** — voir [ci-dessous](#dose-de-base-apprise--feedforward-ph-v2280)).

### 1. Le tick d'entretien sort de la conjonction de dosage

Jusqu'à la v2.26.0, `phIntegralTick()` n'était appelé que **dans** la branche automatique, elle-même gardée par `canDose(0)` : rapport cyclique ≈ **25 %** du temps. L'intégrale n'apprenait ni pendant les pauses de mélange, ni pendant les suspensions de limite horaire — précisément les états où l'eau circule, la mesure est valide et **la charge du bassin continue d'agir**. `Ti` = 6 h « de branche » valait donc ≈ **24 h réelles**, et la convergence demandait **2,4 jours de filtration**.

Le tick rejoint donc `tickPhGuardSurveillance()` **hors de toute conjonction** (précédent de la v2.25.0 : *surveillance ≠ décision*). **La sortie reste gatée exactement comme avant**, par `shouldDose` et par la bande morte de `computePidPure` : entretenir l'intégrale n'autorise **aucune** injection.

Prédicat `phIntegralAccumulationAllowed()` — en clair : `canDose(0)` **moins** {pause mélange, limite horaire, quota journalier, anti-cycling, plafond de session}, **plus** `phMode == "automatic"`.

| Accumule | N'accumule pas |
|---|---|
| Pause mélange (l'eau circule, la mesure est valide ; sur-crédit borné à 15 min × 0,15 = **135 pH·s**, soit 0,8 % du plafond) | **Filtration coupée / eau absente**, stabilisation post-cal, OTA, maintenance, assistant de débit — `update()` sort **avant** le point d'appel : la garantie est **structurelle** |
| Limite horaire épuisée, quota journalier, anti-cycling, plafond de session — ce sont des **suspensions rythmiques** du dosage, pas des interdits | Porte de validité fermée, warmup, sonde figée/périmée, `NaN`, calibration < 2 points — **rejet EXPLICITE**, jamais hérité du `isfinite(error)` interne au module pur (une sonde figée haut rend une erreur parfaitement finie) |
| | Mode `manual`/`off` : le dosage est **durablement** impossible, charger armerait une autorité libérée d'un bloc au retour |
| | Garde pH bloquante ou escalade des 3 refus (fail-closed) ; watchdog inactif |

> **Le prédicat lit `_phGuardBlockingNow`** — le verdict posé par `tickPhGuardSurveillance()` — et **non** `isPhGuardBlocking(true)`, qui **compte** les refus et peut escalader. Un prédicat d'entretien doit être **sans effet de bord**.
>
> **Ordre impératif** : après `tickPhGuardSurveillance()` (dont il consomme le verdict) et **après** la garde de présence d'eau. Déplacer l'appel en amont de cette garde romprait l'exigence chimique « pas d'accumulation en eau stagnante ».
>
> **Un seul point d'évaluation par tour.** Deux appels compteraient le même `dt` deux fois. Le calcul de l'erreur signée est lui aussi remonté hors branche et **réutilisé tel quel** par la branche automatique — deux formules pour une seule grandeur finiraient par diverger.

**Gain réel : rapport cyclique 25 % → 29 %**, convergence **2,4 j → 2,05 j** (−15 %). Le facteur limitant n'est **pas** les pauses de dosage, ce sont les **7 heures de filtration sur 24**.

### 2. Le reset sec est TEMPORISÉ (15 min de temps de branche)

Le **seuil** est conservé à l'identique (exigence du gate) : `erreur < −(phStartThreshold + 0,10)`, soit −0,15. Ce qui change, c'est que **la purge n'est plus instantanée** : l'excursion doit être **continue** pendant `kPhIntegralResetHoldMs` = **15 min de temps de branche**, et toute remontée **constatée par un tick** remet la tenue à zéro.

Motif : un transitoire de −0,15 — **une pluie légèrement acide suffit** — effaçait deux jours et demi d'apprentissage, sans commune mesure avec le trouble observé.

- **Ce différé ne suspend AUCUNE protection.** Pendant l'attente, l'erreur est franchement négative : la **décharge signée** et l'**oubli exponentiel** travaillent tous les deux (**−16 % d'intégrale sur les 15 minutes**). La protection est **graduée**, pas suspendue ; seule la purge **sèche** est différée, le temps de vérifier que l'excursion est réelle.
- **15 min = la pause mélange** (`kPhMixingDelayMs`) : en deçà, l'excursion mesurée peut n'être que la dose précédente pas encore homogénéisée. Verrou `static_assert` (x) : `kPhMixingDelayMs ≤ kPhIntegralResetHoldMs < kPhIntegralDecayHalfLifeMs` — au-delà de la demi-vie d'oubli, le reset sec ne servirait plus à rien, or c'est le **seul mécanisme rapide sur une inversion de sens**.
- ⚠️ **« Temps de branche » veut dire ce qu'il dit.** Quand `phIntegralAccumulationAllowed()` est faux, `phIntegralTick()` n'est pas appelé : `resetHoldMs` est **gelé**, pas remis à zéro. Une excursion de 10 min interrompue par une heure de filtration à l'arrêt reprend **à 10 min**. La direction reste sûre (geler ne fait que **retarder** une purge, et une purge n'ajoute jamais d'autorité), mais la remise à zéro exige **un tick**, pas une simple absence d'excursion.
- L'addition de la tenue est **saturée** (`kPhIntegralHoldNever`) : jamais de repliement.

### 3. Persistance NVS conditionnelle (R1-R7)

L'[ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) l'**interdisait** (« une valeur stockée serait une autorisation de dosage **pré-chargée** au boot »). L'[ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) l'autorise sous **sept garanties cumulatives**, l'objection étant levée **par R4** :

> **La valeur relue n'est jamais appliquée au démarrage.** Elle attend que le bassin démontre lui-même, pendant **5 min de temps de branche**, une erreur **au-dessus du seuil de démarrage** — état dans lequel la boucle aurait de toute façon ouvert une session sur le seul terme proportionnel. **Ce qui est restauré n'est pas une autorisation de doser, c'est la VITESSE d'une correction déjà décidée par la mesure courante.**

| # | Garantie | Où |
|---|---|---|
| **R1** | Plafond `min(0,30 × integralMax ; 0,50 × (charge observée − kp × stop) / ki)`, **réappliqué à l'instant de l'application** | `phPersistRestoreCap`, `phPersistClampRestore` |
| **R2** | Scellement (schéma + empreinte FNV-1a + **taille exacte**, aucune réinterprétation partielle) et **empreinte de contexte** (vitesse, produit, consigne, plafond) | `phPersistSnapshotIntact`, `phPersistContextHash` |
| **R3** | Péremption **en temps de filtration** (v2.34.0) : ≤ **360 min** de circulation **ET** ≤ **72 h** murales, **réévaluée à chaque tick** | `phPersistAge`, `phPersistFresh` |
| **R4** | Confirmation par la mesure : **5 min continues** d'écart au-dessus du seuil de démarrage | `phPersistConfirmTick` |
| **R5** | Écriture **conjonctive** (`kDelta`) : ≤ 1 par **30 min** **ET** > **5 % du plafond** de variation — complétée en v2.34.0 par une **échéance** (`kDue`) et une **réécriture** (`kRewrite`) | `phPersistWriteReason` |
| **R6** | Journalisation de la retenue, de la restauration, de **chaque rejet motivé** | coquille `pump_controller.cpp` |
| **R7** | Anti-boucle : **3 restaurations** sans écriture normale intercalée ⇒ désarmement. Compteur **dans l'instantané NVS**, incrémenté **avant** confirmation, incrément **saturé** | `phPersistStreakExhausted`, `phPersistNextStreak` |

**Fail-closed sur R1** : moins de 3 jours d'historique exploitable, `ki` inerte, charge non finie ou équilibre ≤ 0 ⇒ **plafond nul ⇒ aucune restauration**. Un appareil neuf ne restaure rien.

**Points d'abandon DÉFINITIF d'une candidate en attente** (`phPersistConfirmAbandon`) — ce sont les **ruptures de sens**, à distinguer des simples **interruptions** (`phPersistConfirmInterrupt`, qui ne remettent que la tenue à zéro) :

| Rupture | Motif |
|---|---|
| Changement **réel** de vitesse de régulation | La candidate a été validée contre l'**ancien** `ki` et l'ancien plafond ; le contrôle de contexte n'a lieu qu'au chargement |
| Bascule `ph_minus` ↔ `ph_plus` | Le **signe de l'erreur s'inverse** |
| Passage en mode `manual` | L'opérateur dose à la main, hors de toute conduite automatique. Traité **en amont** de la garde de présence d'eau : une session manuelle filtration coupée ne doit pas laisser la candidate en vie |
| Fin de branche `scheduled` | Le mode Programmée a dosé **à l'aveugle** entre-temps |
| **Purge sèche** (`kDryReset`) | ⚠️ **Défaut corrigé en revue.** Sans cet abandon : candidate en attente → pluie acide → purge sèche → le pH remonte → la candidate se confirme et **recharge l'autorité que la purge venait d'invalider**. Séquence atteignable dans la fenêtre R3 de 6 h |

**Structures NVS** — `PhIntegralSnapshot` (**24 o, schéma 2** depuis la v2.34.0 ; 20 o / schéma 1 avant) dans un namespace **dédié** `pool-phint`, clé `snap`. Aucun `configMutex` : la charge utile appartient exclusivement à `loopTask`.

#### Fraîcheur mesurée en TEMPS DE FILTRATION (feature-093, v2.34.0)

**Le défaut corrigé** : la péremption murale de 6 h (v2.27.0) jetait l'intégrale **chaque matin**. Le bassin ne s'acidifie et ne se mélange que **pendant que l'eau circule** — une nuit sans filtration ne périme rien, mais l'horloge murale, elle, continue d'avancer. Cas réel du 03/09/2026 : valeur stable la veille, aucune écriture déclenchée, rejet « périmé » au matin.

L'instantané passe donc au **schéma 2** (24 o), avec un champ supplémentaire `filtTotalMin` : le **compteur monotone de minutes de filtration** au moment de l'écriture. Un enregistrement de schéma 1 (20 o) est **rejeté** au premier démarrage après mise à jour, jamais réinterprété — l'intégrale repart alors de zéro, comportement attendu et **journalisé**.

| Borne | Constante | Valeur | Rôle |
|---|---|---|---|
| Filtration accumulée depuis l'écriture | `kPhIntegralPersistMaxFiltMin` | **360 min** (6 h) | Le **critère nominal** : une constante d'intégration entière à vitesse `normal` |
| Âge mural | `kPhIntegralPersistMaxWallAgeS` | **72 h** | **Filet** de sécurité (hivernage, compteur à l'arrêt, horloge incohérente) — jamais le critère nominal |
| Échéance d'écriture forcée | `kPhIntegralPersistDueFiltMin` | **180 min** (3 h) | Garantit qu'un instantané **frais** existe à tout instant, même à valeur stable |

Le compteur vient de `SafetyLimits::filtTotalMin` (clé NVS `filt_total` du namespace `pool-daily`, écrite dans la **même transaction** que les compteurs journaliers). Il est **monotone** : jamais remis à zéro à minuit, contrairement à `dailyFiltrationMinutes`. **Fail-closed** — un compteur courant **inférieur** à celui écrit (NVS `pool-daily` effacée, restauration d'usine) donne le rejet `kFiltCounter` : un âge négatif n'est pas « très frais », il est **incalculable**. Sans heure valide, on ne restaure ni n'écrit (`kEpoch`).

#### Trois motifs d'écriture (`phPersistWriteReason`)

| Motif | Condition | Journalisé |
|---|---|---|
| `kRewrite` | Aucun instantané exploitable au boot (absent, schéma 1, empreinte fausse) **ou** instantané rejeté, **et** intégrale courante > 0 → on remplace sans attendre la cadence (C5 `pool-chemistry`). Une seule fois : la coquille baisse le drapeau après écriture | oui |
| `kDelta` | Critère historique : ≥ 30 min depuis la dernière écriture **et** variation > 5 % du plafond | non (rare mais nominal) |
| `kDue` | **180 min de filtration** écoulées depuis la dernière écriture, même à valeur inchangée — et seulement s'il y a quelque chose à préserver (valeur > 0) | oui |

Coût : ~3 écritures de plus par jour de filtration, négligeable devant les 100 000 cycles garantis d'un secteur NVS.

#### Garde `candidatePending` — « la NVS a raison, pas la RAM » (revue v2.34.0)

Tant qu'une restauration R4 attend sa confirmation (`_phIntegralConfirm.candidate > 0`), **aucune écriture n'est autorisée**, quel que soit le motif.

**Sans cette garde** : l'intégrale en RAM vaut 0 pendant l'attente, alors que la NVS porte précisément la valeur à préserver. 30 min plus tard (pH proche de la consigne, donc pas de confirmation), `kDelta` constatait `|0 − savedValue| > seuil` et **écrasait l'instantané valide par un zéro** — le redémarrage suivant repartait de zéro, exactement ce que la feature-093 doit empêcher. Si la candidate meurt (péremption R3, changement de contexte), la coquille baisse le drapeau et la cadence normale reprend : un zéro écrit à ce moment-là est **légitime**, l'ancien instantané n'étant plus restaurable.

#### Journal au boot quand rien n'est exploitable

Un `info` (et non un `warning` : c'est l'état nominal d'un appareil neuf ou d'une première mise à jour) — `Intégrale pH : aucun instantané NVS exploitable (absent, ancien schéma ou empreinte invalide) — départ de zéro, réécriture dès la première intégrale positive`. Les **rejets motivés** (R6) restent en `warning`, et le rejet « périmé » cite **les deux âges** (`N min de filtration, N h murales`) : on doit pouvoir dire lequel des deux critères a tranché.

> R1, R2, R4 et R7 sont **inchangés** par la feature-093. Décisions : [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) (persistance conditionnelle) et [ADR-0060](../adr/0060-fraicheur-en-temps-de-filtration.md) (fraîcheur mesurée en temps de filtration). Logique pure et tests : [`src/ph_persist_logic.h`](../../src/ph_persist_logic.h), coquille [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`loadPhIntegralSnapshotDeferred`, `tickPhIntegralPersistence`), constantes [`src/constants.h`](../../src/constants.h).

### 4. L'anneau de charge acide sur 7 jours

`phPersistRestoreCap()` consomme une **charge observée** qui **n'existait nulle part** : `pool-daily` n'a que le compteur du jour courant (écrasé au changement de date) et `history.*` ne stocke que pH/ORP/température. La fonction rendait donc `days = 0` et le fail-closed interdisait **toute** restauration.

**`PhLoadHistory`** — 7 échantillons `{date AAAAMMJJ, mL, minutes}` dans **un blob de 64 octets**, namespace **existant** `pool-daily`, clé `ph_load7`, **une écriture par jour**.

- **Alimenté aux DEUX points de basculement de date existants** — `tickDailyRollover()` (minuit, appareil allumé) et `loadDailyCounters()` (l'appareil était éteint à minuit). Aucun troisième point n'est créé.
- **Déduplication sur TOUTE l'étendue de l'anneau** : si une case porte déjà cette date, elle est **remplacée**. Ne comparer qu'à la case la plus récente ne suffit pas — un recul d'horloge (correction NTP à cheval sur minuit) rouvre puis reclôt une date archivée **une case plus loin**, et la journée serait **pondérée double** dans la médiane.
- **Médiane, pas moyenne** : une seule journée aberrante (purge, incident, assistant de mesure de débit, mise en service) déplacerait une moyenne sur 7 valeurs de plus de 10 % — et le plafond de restauration avec elle. La médiane encaisse **jusqu'à trois journées folles sur sept**.
- **Aucun scellement partiel** : taille exacte + schéma `kPhLoadHistorySchema` + empreinte FNV-1a, sinon **historique vidé** (avec un `warning`, cf. journal).

#### Critère de jour exploitable — les trois filtres n'écartent QUE par le haut

| Filtre | Règle | Pourquoi |
|---|---|---|
| **Circulation minimale** | `minutes ≥ kPhLoadDayMinFiltrationMin` (**120 min**) | Sous 2 h, le rapport mL/min ne décrit plus la charge du bassin mais des **transitoires** : la purge des mesures (≥ 10 min, v2.34.0) et la pause de mélange amputent une part **fixe** de la journée, et le budget horaire rend une journée courte structurellement bornée par le **quota**, pas par la chimie |
| **Fenêtre d'âge** | ≤ 7 jours **calendaires**, vue de la date du jour (`phLoadDayWithinWindow`) | ⚠️ **Défaut corrigé en revue.** L'anneau garde les 7 dernières journées **ARCHIVÉES**, pas les 7 derniers **jours** : un appareil arrêté deux mois retrouvait des journées d'été intactes. 4 journées à 2,86 + 3 journées d'automne à 1,00 ⇒ médiane **2,86**, plafond **3 753 pH·s** contre **2 484** d'équilibre réel = **1,5 × l'équilibre courant**. Conversion Howard Hinnant (`days_from_civil`) : sans elle, « 20260301 − 20260228 » vaudrait **73** |
| **Vraisemblance physique** | `ml / minutes ≤ 1,10 × débit soutenable` | Un rapport au-dessus du délivrable n'est pas l'observation d'un bassin chargé : c'est la **preuve** qu'il manque des minutes au dénominateur. La journée est **écartée**, jamais rapiécée. ⚠️ **Le seuil est une FONCTION de la configuration** — [ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) |

> **Une journée à volume NUL est CONSERVÉE.** Le volume n'entre pas dans le critère de représentativité : une circulation suffisante sans dosage est une observation **légitime** de charge faible. L'écarter tirerait la médiane **vers le haut**, c'est-à-dire **du mauvais côté** (fail-open).

Le seuil de vraisemblance suit `phSustainableFlowFromConfig()` = `phSustainableAvgFlow(débit d'amplitude calibré, limite horaire configurée)` — **source unique** partagée avec le gel d'anti-windup de l'intégrale. Le figer aurait tué la restauration **en silence** dès que l'utilisateur applique la recommandation de relever la limite horaire : à 15 min/h le délivré légitime atteint ~6,5 mL/min, et un seuil bloqué à 2,17 écarterait **toutes** les journées.

#### Le dénominateur : pourquoi les minutes de circulation existent

**La charge se mesure en mL/min, pas en mL/jour.** Un jour à 4 h et un jour à 10 h de circulation qui injectent le même volume ne décrivent **pas** la même charge — le second est 2,5 fois moins chargé. Sans dénominateur, la médiane serait celle d'une grandeur qui dépend surtout de l'**horaire de filtration**.

`SafetyLimits::dailyFiltrationMinutes` ([`config.h`](../../src/config.h)) est incrémenté par `tickDailyFiltrationMinutes()`, **hors de toute conjonction d'éligibilité** — compter la circulation n'est pas décider de doser. La source est le **cache de présence d'eau** (`measureValidity.waterPresent()`, lui-même alimenté par `resolveWaterPresence()`), donc valable dans les **trois modes d'installation**, pas seulement quand ce contrôleur pilote le relais. Saturation à **1440 min/jour**.

> **Seuil de `dt` distinct et volontairement plus large** : `kPhLoadMinuteMaxTickDtMs` = **60 s**, contre 10 s pour l'intégrale. Les deux compteurs ne courent pas le même risque : l'intégrale se protège d'un `dt` aberrant qui la **chargerait trop** (fail-open) ; le dénominateur, lui, ne risque que d'être **tronqué** — et un dénominateur tronqué **surévalue** la charge mL/min, donc le plafond de restauration. `accountPumpVolume` crédite le numérateur sur le delta **entier**. Le sens du fail-closed est inversé, le seuil l'est donc aussi.

**Relecture d'une valeur > 1440** (impossible ⇒ enregistrement corrompu) : **deux usages, deux traitements opposés**, chacun choisi du côté qui **sous-estime** la charge — journée **close** à archiver ⇒ sentinelle **0** (journée rendue non archivable, elle ne pollue pas la médiane ; l'écrêter à 1440 fabriquerait un échantillon de toutes pièces) ; journée **en cours** à reprendre ⇒ dénominateur **saturé à 1440** (la journée est déjà « pleine », sa charge sera minimale).

#### Usure NVS : un facteur 5 à 6, assumé

`_dailyCountersDirty` est désormais posé **une fois par minute de circulation**. Le flush périodique 30 s se déclenche donc tant que l'eau tourne :

| | Avant v2.27.0 | v2.27.0 |
|---|---:|---:|
| Transactions `pool-daily` / jour | ~70-100 | **~420-600** |

**Le choix est délibéré, pas un effet de bord.** Numérateur (mL) et dénominateur (minutes) sont persistés **ensemble** dans la même transaction : une coupure ne perd au plus qu'**une minute de chacun**, donc **le rapport mL/min est préservé**. L'ordre de grandeur reste très en deçà des 100 000 cycles garantis d'un secteur (NVS à nivellement d'usure, réécritures à valeur identique ignorées), mais le coût doit être **présenté**, pas minimisé.

#### Points de flush ajoutés (défaut fail-open corrigé en revue)

Le flush 30 s vivait en **fin** d'`update()`, derrière **quatre `return`** : OTA, mode maintenance, capteurs non initialisés, stabilisation / assistant de débit. Pendant ces épisodes **l'eau circule** et les minutes s'accumulent, mais `_dailyCountersDirty` n'était **jamais consommé** — et l'OTA comme la maintenance se terminent par un **redémarrage**. Chiffrage : une journée de 480 min de circulation dont 360 en maintenance et 120 en régulation à 5 mL était relue **5/120 = 0,042 mL/min** au lieu de **0,0104**, soit une charge observée **quatre fois trop grande**.

| Point | Fichier |
|---|---|
| Flush périodique **remonté en amont de toutes les sorties anticipées** d'`update()` | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| `setMaintenanceInhibit(true)` — **entrée** en maintenance (durée non bornée, se termine le plus souvent par un redémarrage) | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| Redémarrage **après OTA** et redémarrage en **mode Point d'accès** | [`web_server.cpp`](../../src/web_server.cpp) |
| Redémarrage demandé par le **web** (`/restart`) | [`web_routes_config.cpp`](../../src/web_routes_config.cpp) |
| Redémarrage demandé par **MQTT / Home Assistant** | [`mqtt_manager.cpp`](../../src/mqtt_manager.cpp) |

> ⚠️ **Aucune sortie anticipée d'`update()` ne doit repasser devant le flush des compteurs journaliers.** L'intégrité de la charge observée est une **propriété de sécurité** ([ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md)).

Les deux flushs de **transition** (démarrage d'injection pH / ORP), qui dépendent de `phActive`/`orpActive`, restent en fin d'`update()`.

### Recalibration des paliers d'alerte de saturation

La **base de temps est conservée** (temps de branche automatique) : c'est la seule qui **suspende** l'épisode en `manual`, sous garde bloquante ou pendant un hiver sans filtration. **Mais son tarif a changé** : le tick d'entretien tourne plus souvent. À seuils inchangés, les paliers mûriraient d'autant plus vite et signaleraient un **transitoire de rattrapage légitime** — le faux positif qui décrédibilise l'alerte.

**L'invariant préservé est la FENÊTRE DE PREUVE EN TEMPS RÉEL** de la v2.26.0, validée sur le terrain. Facteur = `0,29 / 0,25` = **1,16**, arrondi à la minute ronde :

| Palier | v2.26.0 (branche) | v2.27.0 (branche) | Fenêtre réelle (inchangée) |
|---|---:|---:|---:|
| `warning` | 2 h | **2 h 20** | ~8,0 h |
| `critical` | 6 h | **7 h** | ~24,1 h (≈ 3 jours de filtration à 7 h/j) |
| Rappel du `critical` | 12 h | **14 h** | ~48,3 h |

Trois `static_assert` (vii bis-a) verrouillent le rapport `palier / rapport cyclique` à **5 % près**, sous forme croisée (pas de division).

> ⚠️ **D'où vient le facteur — à lire avant toute recalibration future.** Une première rédaction avait retenu **×4**, repris d'une analyse d'impact **préliminaire** (« le tick passe de `canDose(0)` à toute la fenêtre de filtration », soit un rapport cyclique implicitement porté à 100 %) sans le confronter au chiffrage du gate. Résultat mesuré : la fenêtre avant `warning` passait de ~8 h à **~27,6 h** et celle avant `critical` à **~3,4 jours** — un bidon vide détecté **3,45 × plus lentement**, l'inverse exact de l'intention. **Ne recalibrer ces paliers que sur un rapport cyclique chiffré et argumenté, jamais sur une intuition d'impact.**

> **Ce que sont ces deux rapports cycliques, dit exactement.** `kPhIntegralDutyV2260 = 0,25` et `kPhIntegralDutyF084 = 0,29` sont des **ESTIMATIONS ANALYTIQUES** du gate, **pas des relevés de terrain** : 0,29 vaut **7/24 exactement**, c'est-à-dire la fenêtre de filtration quotidienne de la spec **privée de son facteur de disponibilité 0,85** ; 0,25 est le chiffrage correspondant sous `canDose(0)`. *(Le commentaire de `constants.h` les qualifiait de « MESURES, pas des estimations » — c'était faux, et faux dans le paragraphe même qui interdit de recalibrer sur une estimation. Corrigé en v2.27.0.)*
>
> **Pourquoi cela reste acceptable** : le **sens de l'erreur est conservateur**. Ces deux nombres ne servent qu'au **rapport** 0,29/0,25. Sur-estimer l'ancien rapport cyclique **raccourcit** les paliers recalibrés, donc **avance** les alertes : le défaut possible est le **faux positif**, jamais le **silence** — qui est la classe de défaut que cette surveillance existe pour éliminer.

### Nouvelles signatures publiques

```cpp
// pump_controller.h — setMaintenanceInhibit n'est PLUS inline (flush NVS à l'entrée)
void setMaintenanceInhibit(bool inhibit);

// config.h — anneau de charge et charge observée (transport NVS seul)
bool loadPhIntegralSnapshot(PhIntegralSnapshot& out);
void savePhIntegralSnapshot(PhIntegralSnapshot& s);
bool loadPhLoadHistory(PhLoadHistory& out);
void savePhLoadHistory(PhLoadHistory& h);
void recordPhLoadDay(const char* date8, float phMl, uint32_t filtrationMinutes,
                     float sustainableMlPerMin);
PhPersistLoadObservation readPhLoadObservation(float sustainableMlPerMin);

// config.h — SOURCE UNIQUE du débit moyen soutenable de la voie pH (mL/min).
// Rend 0 quand la pompe n'est pas caractérisée (l'appelant retombe alors sur
// kPhLoadSustainableFallbackMlPerMin). ADR-0054.
float phSustainableFlowFromConfig();
```

`readPhLoadObservation()` est prévue pour être appelée **rarement** (démarrage + une clôture de journée) : l'appelant met le résultat en **cache** (`_phLoadObs`), jamais de lecture NVS depuis `update()`. Elle est **réutilisable telle quelle** par la dose de base apprise (feature-085), qui exige la même médiane.

### Nouvelles constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---:|---|
| `kPhIntegralResetHoldMs` | 900 000 ms (**15 min**) | Tenue de l'excursion avant purge sèche (temps de branche) |
| `kPhIntegralRestoreMaxFraction` | 0,30 | R1 — **butée absolue** (fraction de `integralMax`) |
| `kPhIntegralRestoreLoadFraction` | 0,50 | R1 — fraction de l'**équilibre observé** |
| `kPhEquilibriumLoadMlPerMin` | 2,86 mL/min | **DEMANDE** à l'équilibre (réf. 2026-08) — ⚠️ pas un volume délivré |
| `kPhIntegralRestoreHistoryWindowDays` / `…MinHistoryDays` | 7 / **3** | Fenêtre de l'anneau / exigence **fail-closed** |
| `kPhIntegralPersistMaxFiltMin` | **360 min** de filtration (= 1 `Ti` à vitesse `normal`) | R3 — péremption, **critère nominal** (v2.34.0 ; remplace `kPhIntegralPersistMaxAgeS`, 6 h murales) |
| `kPhIntegralPersistMaxWallAgeS` | 259 200 s (**72 h**) | R3 — filet mural (v2.34.0) |
| `kPhIntegralPersistDueFiltMin` | **180 min** de filtration | R5 — échéance d'écriture forcée `kDue` (v2.34.0) |
| `kPhIntegralRestoreConfirmMs` | 300 000 ms (**5 min**) | R4 — tenue de confirmation |
| `kPhIntegralRestoreArmWindowMs` | 600 000 ms (10 min) | R4 — fenêtre d'armement après boot |
| `kPhIntegralPersistMinIntervalMs` / `…MinDeltaFrac` | 30 min / 5 % | R5 — cadence **conjonctive** du motif `kDelta` |
| `kPhIntegralRestoreMaxStreak` | 3 | R7 — anti-boucle de démarrage |
| `kPhIntegralPersistSchema` / `kPhLoadHistorySchema` | **2** / 1 | Versions de schéma NVS (l'instantané passe à **2** en v2.34.0 : +`filtTotalMin`, 20 → 24 o) |
| `kPhLoadDayMinFiltrationMin` | **120 min** | Critère de jour exploitable |
| `kPhLoadMinuteMaxTickDtMs` | 60 000 ms (**60 s**) | `dt` maximal crédité au **dénominateur** (≠ 10 s de l'intégrale) |
| `kPhLoadSustainableFallbackMlPerMin` | 2,17 mL/min | **REPLI seulement** (ex-`kPhLoadSustainableMlPerMin`, **renommée**) — jamais un seuil nominal, [ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) |
| `kPhLoadDayMaxRateFactor` | 1,10 | Tolérance d'arrondi du filet de vraisemblance |
| `kPhIntegralDutyV2260` / `kPhIntegralDutyF084` | 0,25 / 0,29 | Rapports cycliques (**estimations analytiques**) servant à la recalibration des paliers |

### Journal (v2.27.0)

| Message | Niveau | Quand |
|---|---|---|
| `Intégrale pH : instantané NVS retenu — N pH·s écrits il y a M min de filtration (H h murales), plafonnés à P pH·s par {la charge observée\|la butée absolue} ; application DIFFÉRÉE jusqu'à confirmation par la mesure (5 min d'écart soutenu)` | `info` | Candidate acceptée au démarrage (R6). **v2.34.0** : l'âge est donné en **temps de filtration** (le critère), l'âge mural en complément |
| `Intégrale pH restaurée : N pH·s (X % du plafond), confirmée par 5 min d'écart soutenu ; plafond P pH·s, charge observée C mL/min sur D jour(s), équilibre E pH·s` | `info` | Confirmation R4 aboutie — **la charge observée et l'origine du plafond sont journalisées** : sans elles, impossible de dire après coup si la restauration a été bornée par la butée ou par la chimie |
| `Intégrale pH : instantané NVS REJETÉ (motif) — la régulation repart de zéro` | `warning` | Rejet motivé : schéma, anti-boucle, contexte, horodatage, **compteur de filtration incohérent** (v2.34.0), péremption, valeur. Sur « périmé », le message cite **les deux âges** (`M min de filtration, H h murales`) — on doit pouvoir dire lequel a tranché |
| `Intégrale pH : aucun instantané NVS exploitable (absent, ancien schéma ou empreinte invalide) — départ de zéro, réécriture dès la première intégrale positive` | `info` | **v2.34.0** : rien de lisible en NVS. État nominal d'un appareil neuf **et** du premier démarrage après la migration schéma 1 → 2 |
| `Intégrale pH : instantané NVS écrit par échéance (180 min de filtration depuis le précédent) — N pH·s à M min de filtration cumulée` | `info` | **v2.34.0**, motif `kDue` |
| `Intégrale pH : instantané NVS écrit en remplacement d'un enregistrement rejeté — N pH·s à M min de filtration cumulée` | `info` | **v2.34.0**, motif `kRewrite` (C5). Les écritures par amplitude (`kDelta`) restent **muettes** |
| `Intégrale pH : instantané NVS non restauré — plafond de restauration nul (D jour(s) d'historique de charge sur 3 requis) ; la régulation repart de zéro` | `info` | **État nominal** d'un appareil neuf — `info`, pas `warning`, sinon les vrais rejets seraient noyés |
| `Intégrale pH : restauration abandonnée — instantané périmé avant confirmation` | `warning` | R3 pendant l'attente |
| `Intégrale pH : restauration abandonnée — plafond de restauration retombé à zéro avant confirmation` | `warning` | R1 pendant l'attente. **Cause distincte de la péremption** : les confondre ferait partir le diagnostic sur l'horloge au lieu de la charge observée |
| `Intégrale pH : restauration abandonnée — heure non synchronisée (âge de l'instantané incalculable)` | `warning` | Fenêtre d'armement écoulée sans NTP/RTC |
| `Intégrale pH : restauration en attente abandonnée — régulation pH en mode manuel` | `info` | Rupture de sens |
| `[Charge pH] Journée AAAAMMJJ archivée : N mL en M min de circulation ; D/3 jour(s) exploitable(s), charge médiane C mL/min` | `info` | Clôture d'une journée (une fois par jour) |
| `[Charge pH] Anneau NVS illisible (taille, schéma ou empreinte) — historique vidé, plus de restauration d'intégrale avant 3 nouvelles journées` | `warning` | Anneau corrompu. **Une absence (0 octet) reste silencieuse** — c'est le cas nominal avant la première clôture |
| `[Charge pH] Historique de charge : ouverture NVS refusée` / `[Config] Instantané intégrale pH : ouverture NVS refusée` | `warning` | Échec d'ouverture en écriture |

### Testabilité

Nouvelle suite native [`test/test_native_ph_persist/`](../../test/test_native_ph_persist/) — total du projet **849 → 932 tests**. **Motif de l'extraction** : R1 à R7 vivaient dans `config.cpp` et `pump_controller.cpp`, **ni l'un ni l'autre n'étant dans le `build_src_filter` de `env:native`**. Trois mutations survivaient : *supprimer l'écrêtage R1*, *supprimer la vérification de CRC*, *rendre le prédicat permissif*. C'est la situation exacte qui avait précédé l'extraction de `ph_integral_logic.h` — où le défaut réel se trouvait **dans la garde de C2**.

La sentinelle du filet de vraisemblance **verrouille les deux sens** : la même journée à 6 mL/min est **écartée** à 5 min/h et **retenue** à 15 min/h. Un seuil figé — à 2,17 comme à 6,5 — casse **l'une des deux lignes** ; un test à sens unique ne tuerait pas la mutation « seuil figé » ([ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md)).

## Dose de base apprise — feedforward pH (v2.28.0)

> Décision structurante : [ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md) (feedforward appris **additif plafonné**). Gate `pool-chemistry` : **GO amont sous 4 conditions** (toutes implémentées) **+ GO contre-visite**, 30/08/2026. Spec : [feature-085](../../specs/features/done/feature-085-dose-de-base-apprise-feedforward.md).

### Principe

Le PI redécouvrait par tâtonnement une charge **parfaitement prévisible** (le dégazage de CO₂, stable d'un jour à l'autre). Le feedforward injecte la charge **connue** ; le PI ne traite plus que l'**écart résiduel** :

```
débit = FF(charge apprise) + kp·e + ki·∫(e − seuil)
        └─ connu d'avance ─┘  └── correction de l'écart résiduel ──┘
```

La charge est couverte **dès la première heure de filtration** au lieu de J+2,4. Le FF est un **débit** (mL/min), jamais un accumulateur : aucun état volumique, donc aucune « dette » à rattraper après un reboot ou une suspension — pas de rafale possible (critère A7 de la spec).

Toute la logique vit dans le module pur [`src/ph_feedforward_logic.h`](../../src/ph_feedforward_logic.h) ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) ; la coquille `tickPhFeedforward()` fournit les entrées (erreur signée, validité de mesure, empreinte de contexte, charge observée, soutenable, session) et journalise les transitions.

### Source : la médiane 7 jours, RÉUTILISÉE

La dose de base est la **médiane des mL/min journaliers sur 7 jours** de l'anneau `ph_load7` ([feature-084, ci-dessus](#4-lanneau-de-charge-acide-sur-7-jours)), consommée via `phIntegralObservedLoad()` — **aucune nouvelle médiane, aucun second anneau** : la restauration R1 et le FF lisent la même grandeur, elles ne peuvent pas diverger. **Fail-closed sous 3 jours** d'historique exploitable (`kPhIntegralRestoreMinHistoryDays`, statut `kNoHistory`) : un appareil neuf ne dose rien « de mémoire ». Elle est **apprise, jamais saisie** — aucun paramètre de volume de bassin, de TAC ni de concentration (exigence utilisateur, critère A9).

### Plafond double (condition bloquante n°2 du gate)

```
ff = min(charge observée, kPhFfMaxSustainableFraction × débit soutenable)   // plafond d'ACTIONNEUR
ff = 0 dès que dailyPhInjectedMl ≥ kPhFfDailyQuotaFraction × maxPhMlPerDay  // gel de QUOTA
```

- **Plafond d'actionneur (0,60)** : le PI garde au moins 40 % du soutenable pour l'écart résiduel. Sur l'installation de référence (soutenable 2,17 mL/min, charge 2,86) : FF borné à **1,30 mL/min**, ~45 % de la charge couverte immédiatement, 0,87 mL/min d'autorité restante au PI.
- **Gel de quota (0,60 de `maxPhMlPerDay`)** : « 60 % du soutenable » **n'est pas** « 60 % du quota » — c'est une condition **affaiblie**. Contre-exemple du gate : quota 300 mL/j avec 12 h de filtration ⇒ 0,42 mL/min consomment 100 % du quota alors que l'actionneur autorise 1,30. Sans ce gel, le FF viderait seul la limite journalière. Limite ≤ 0 (désactivée) = pas de gel, comme partout. `static_assert` : le gel du FF (0,60) mord **avant** le gel d'anti-windup de l'intégrale (0,80) — l'autorité aveugle à l'erreur cède la première.

### Latch de sécurité (condition n°3) et réarmement

Le FF est la seule source de dosage **aveugle à l'erreur** : sans mesure digne de confiance, personne ne peut constater qu'il dose à tort. Une fois le latch posé, FF = 0 jusqu'au **front d'ouverture de la session automatique suivante** (un latch posé en cours de session tient jusqu'à sa fin).

| Cause | Règle |
|---|---|
| **Erreur défavorable tenue** | Erreur **SIGNÉE** < −`kPhFfAdverseErrorPh` (−0,05) tenue `kPhFfLowHoldMs` (**30 min de temps de branche**, durées observées, jamais d'échéance future — ADR-0049). Erreur signée et non pH brut : le même déclencheur vaut pour `ph_minus` **et** `ph_plus`. C'est le scénario « fausse pluie » de la spec : annulation en < 30 min |
| **Mesure invalide** | Filtre non prêt / instable / valeur non finie ⇒ fail-closed **immédiat, sans tenue** ; la tenue d'erreur est remise à zéro (l'erreur n'a plus de sens) |
| **Changement de contexte** | Détection par front sur `phIntegralContextHash()` (vitesse, produit, consigne, plafond) — même règle R2 que la persistance : la charge a été apprise pour un autre point de fonctionnement |

**Tenue produit** (recommandation du gate) : au changement de `phCorrectionType`, le latch tient jusqu'à ce qu'une journée **entière** ait été archivée sous le nouveau régime — la première clôture après le changement archive une journée **mixte** et est sautée (`_ffProductHoldFreshDay`), la suivante lève la tenue. **RAM-only, fail-open borné assumé** : un reboot lève la tenue, mais le FF reste borné par le plafond d'actionneur et le gel de quota — borne chiffrée par le gate à **~39 mL au pire**, soit ~0,01-0,02 pH sur 30-50 m³ ; la persistance NVS est jugée non requise ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md), alternative (e)).

### Un seul chemin de commande (condition n°1) et anti-windup (condition n°4)

Le FF est un **terme additif** dans la branche automatique, jamais une seconde branche de dosage : il passe par le chemin existant (`computePID` borné à `ampFlow − ff`, somme clampée à `ampFlow`, `planPulseWithCarry`, `tickPulse`, comptabilité) — **une seule comptabilité de volume, un seul jeu de gardes**. L'alternative « branche `scheduled` parallèle » a été écartée par le gate (double comptabilité, double jeu de gardes, double dosage à la première divergence).

**Anti-windup recalculé** : le soutenable moyen vu par le tick intégral devient `max(0, soutenable − ff)` (`PhIntegralInputs::ffMlPerMin`, [`ph_integral_logic.h`](../../src/ph_integral_logic.h)) — sinon l'intégrale se gèlerait sur une saturation **causée par le FF** et le PI deviendrait sourd. D'où l'**ordre impératif** dans `update()` : `tickPhFeedforward()` **avant** `tickPhIntegralAuto()`, dans le même tour.

### Stabilité : par l'hystérésis de session, structurellement

Le FF ne vit que sous `shouldDose` (session automatique ouverte, statut `kInactive` sinon). Un FF qui sur-corrigerait fait retomber l'erreur sous le seuil d'**arrêt** : la session se ferme et le FF cesse **avec elle** — sa condition d'existence est précisément la session qu'il vient de fermer ; la réouverture exige une remontée à `phStartThreshold`, la bande [stop ; start] absorbe le rebond. L'argument de fond est le **bilan de matière** : le volume journalier livré converge vers la charge réelle du bassin **quel que soit le FF**, donc la médiane apprise est **non expansive** (pas d'auto-amplification d'un jour sur l'autre). **C'est le mécanisme de stabilité ; le plafond d'actionneur n'est qu'une ceinture.** Ne pas ajouter d'hystérésis interne au FF — double emploi et second point de réglage.

Corollaire « sonde figée » (critère A6) : une sonde figée ferme la porte de validité ⇒ `canDose()` refuse ⇒ aucune session ⇒ FF = 0 — garantie structurelle, **doublée** par le latch « mesure invalide » explicite.

### Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---:|---|
| `kPhFfMaxSustainableFraction` | 0,60 | Plafond d'actionneur — le PI garde ≥ 40 % du soutenable |
| `kPhFfDailyQuotaFraction` | 0,60 | Gel de quota — `static_assert` : < `kPhIntegralFreezeDailyFraction` (0,80) |
| `kPhFfAdverseErrorPh` | 0,05 | Seuil du latch « erreur défavorable » (erreur signée < −0,05) |
| `kPhFfLowHoldMs` | 1 800 000 ms (**30 min**) | Tenue du latch, en temps de branche — `static_assert` : ≥ `kPhIntegralResetHoldMs` (15 min, le FF est une autorité permanente, la preuve exigée est plus longue) et ≤ 30 min (critère « fausse pluie » de la spec) |

### Accesseurs publics (contrat figé)

`getPhFeedforwardFlow()` (mL/min, 0 hors session) et `getPhFeedforwardStatus()` — code `PhFfStatus` **FIGÉ**, jamais renuméroté (même règle que `DoseRefusal`, ADR-0049) :

> ⚠️ **Deux ajustements en v2.46.0** (feature-092), tous deux des **corrections** : `getPhFeedforwardFlow()` rapporte le feedforward du **régime appliqué** — donc `0,00` en régime de maintien à zéro — et le statut `3` est **traduit en `0`** dès que l'anneau porte ≥ 3 journées exploitables. Voir [Composition du débit d'injection](#composition-du-débit-dinjection--accesseurs-de-lecture-seule-feature-092-v2460).

| Code | État | Servi hors session ? |
|---|---|---|
| 0 | `kInactive` — aucune session automatique | oui (état de repos) |
| 1 | `kActive` — FF appliqué (0,00 possible : charge observée nulle = observation légitime) | **non** — traduit en 0 hors session (les instantanés ne sont rafraîchis que quand `update()` atteint le tick : sans cette garde, un « actif » périmé survivrait à une sortie anticipée) |
| 2 | `kHeld` — latch de sécurité | oui — l'UI peut dire **pourquoi** le FF ne s'appliquera pas |
| 3 | `kNoHistory` — < 3 jours exploitables | oui |
| 4 | `kQuota` — ≥ 60 % de la limite journalière consommés | oui |

Publiés en **parité stricte** sur `/data`, le WebSocket et MQTT (`ph_ff_ml_per_min` 2 décimales, `ph_ff_status`) : [API.md](../API.md#champs-du-feedforward-ph-feature-085-v2280), [ws-manager.md](ws-manager.md#champs-sensor_data-ajoutés-en-v2280--feedforward-ph-adr-0055), [MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-v2280-dose-de-base-apprise--feedforward-ph), UI : [page-ph.md](../features/page-ph.md#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460).

### Journal

| Message | Niveau | Quand |
|---|---|---|
| `Dosage pH : dose de base apprise X.XX mL/min (charge observée X.XX mL/min sur N jour(s), plafond X.XX)` | `info` | Premier tick **actif** de chaque session (anti-spam `_ffAnnounced`) |
| `Feedforward pH retenu (pH franchement au-delà de la consigne depuis plus de 30 min \| mesure pH invalide \| réglages de régulation modifiés) — dose de base suspendue jusqu'à la prochaine session` | `info` | Pose du latch (par transition, jamais par tour) |
| `Feedforward pH retenu : changement de TYPE de produit (pH- <-> pH+) — dose de base suspendue jusqu'à une journée entière archivée sous le nouveau régime` | `info` | Tenue produit (le TYPE déclaré dans les réglages — un simple changement de bidon n'a aucun effet sur l'apprentissage) |
| `Feedforward pH réarmé (ouverture de session)` | `info` | Réarmement au front de session |

### Testabilité

Nouvelle suite native [`test/test_native_ph_feedforward/`](../../test/test_native_ph_feedforward/) — total du projet **932 → 970 tests**, **6 mutations tuées sur 6** (validation par mutation exigée par le workflow projet).

## Limite d'actionneur pH — exposition et alerte de sous-dimensionnement (feature-086, v2.29.0)

> Décision structurante : [ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) (3 jours × 80 %, anneau évalué au soutenable courant). Visa `pool-chemistry` : **GO sous conditions** (les deux conditions prouvées par test et par grep), revue **Approved**, 30/08/2026. Spec : [feature-086](../../specs/features/done/feature-086-limite-actionneur-dosage-ph.md).

Le visa du 30/08/2026 établit que le bassin de référence est limité par l'**actionneur**, pas par le correcteur : maximum délivrable ~911 mL/j (2,17 mL/min soutenable × 420 min de filtration) contre un besoin de ~1 800 mL/j. Cette section rend la limite **visible** et **alertable** — **lecture seule stricte** : aucun de ces chemins n'est consommé par une décision de dosage, aucun chemin nouveau vers les pompes. Logique pure dans [`src/ph_actuator_limit_logic.h`](../../src/ph_actuator_limit_logic.h) ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).

### Les 4 accesseurs (lecture seule, parité `/data` ↔ WS)

| Accesseur | Retour | Contrat |
|---|---|---|
| `getPhSustainableMlPerMin()` | float | Débit moyen soutenable = amplitude calibrée × limite horaire (`phSustainableFlowFromConfig()`, source unique, recalcul à la demande — une multiplication, pas de cache à invalider). `NAN` si pompe non caractérisée ou valeur non finie → `null` en JSON. |
| `getPhDailyMaxMl()` | float | Soutenable × `filtration.scheduledMinutesToday()` (minutes **programmées**, voir ci-dessous). `NAN` si soutenable indisponible ou planning inexploitable (−1). |
| `isPhLimitedByHourlyBudget()` | bool | Épisode de saturation C5 **ouvert** (`_phIntegral.satActive`, hystérésis sur la **demande**) **ET** mesure pH valide. |
| `isPhUndersizedSuspected()` | bool | `phActuatorUndersized(_phLimitStreakDays, anomalyHeldMs > 0)` **ET** mesure pH valide. |

Course bénigne assumée (patron `isAnyPumpEnergized`) : état écrit par `loopTask`, lu en contexte web async, lectures 8/32 bits atomiques sur Xtensa. Publication : [API.md](../API.md#champs-de-la-limite-dactionneur-ph-feature-086-v2290), [ws-manager.md](ws-manager.md#champs-sensor_data-ajoutés-en-v2290--limite-dactionneur-ph-feature-086), UI : [page-ph.md](../features/page-ph.md#capacités-de-dosage-et-bandeaux-de-limite-dactionneur-feature-086-v2290).

### Critère de détection (`phActuatorLimitStreak`, module pur)

La série compte **N = 3 journées calendaires consécutives exploitables** (`kPhActuatorLimitConsecutiveDays`) dont le volume délivré atteint **≥ 80 %** du maximum délivrable de la journée (`kPhActuatorLimitDayFillRatio` × soutenable × minutes **réellement archivées** de circulation). Règles du parcours de l'anneau `ph_load7` (du plus récent au plus ancien) :

- **Ancrage hier/aujourd'hui** : le jour archivé le plus récent doit dater d'hier (cas nominal post-minuit) ou d'aujourd'hui (journée close par recul d'horloge). Un appareil arrêté deux mois retrouve des journées d'été intactes : une série ancienne ne décrit plus le régime courant → série 0.
- **Les jours non exploitables et les trous calendaires CASSENT la série** — ils ne la traversent pas : journée non archivée (horloge invalide, appareil éteint), journée sous 2 h de circulation, rapport mL/min implausible (mêmes critères `phLoadDayUsable` que la médiane R1). Arithmétique de dates exacte (`phLoadDayNumber`, bissextiles comprises).
- **Marge 80 % et non 90-95 %, justifiée par l'incident du 23/08** : en saturation permanente réelle, le délivré observé plafonne à **84-88 % du maximum théorique** (765-802 mL comptés pour ~911 mL max) — pauses de mélange, délai de purge et plafonds de session amputent mécaniquement le reste. Un seuil à 90-95 % ne se déclencherait **jamais** ; un régime convergé délivre < 50 %, donc aucun faux positif à 80 % (`static_assert` verrouille ]0,5 ; 0,9[).
- Soutenable ≤ 0/non fini ou date du jour inexploitable → série 0 (fail-closed, on n'invente pas de maximum).

### Réarmement implicite : l'anneau est évalué au soutenable COURANT

Aucun compteur d'alerte n'est persisté : la série est **recalculée** à chaque relecture de l'anneau contre le débit soutenable **courant**. Conséquences voulues :

- **Limite relevée** ⇒ le soutenable monte ⇒ les ratios des journées passées retombent sous 80 % ⇒ la série retombe, l'alerte se lève d'elle-même.
- **Limite baissée** ⇒ les journées passées deviennent implausibles au filet de vraisemblance ⇒ écartées.

**Trade-off assumé** ([ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) : relever la limite « blanchit » jusqu'à 7 jours d'historique — mais c'est précisément l'**action corrective attendue** ; l'alerte n'a plus de raison de persister une fois le remède appliqué (cas limite « changement de produit ou de concentration » de la spec : l'alerte se réarme, elle ne persiste pas).

### Garde mesure-valide sur les DEUX accesseurs booléens

L'épisode C5 est tenu en **temps de branche automatique** : sur mesure pH invalide (panne de sonde), la branche d'entretien s'arrête et `anomalyHeldMs` **GÈLE** à sa dernière valeur > 0 — sans garde, une montée du streak 2→3 à la clôture de journée pendant une panne de sonde émettrait l'alerte sur un épisode **figé** (trou trouvé par le `test-engineer` en cours de cycle, corrigé — condition bloquante n°1 du visa). Les deux booléens portent donc **strictement le même prédicat** `!isnan(sensors.getPhValueRaw())` (garde d'âge du filtre comprise) : « borné par la limite » et « sous-dimensionné » restent distincts de « bidon vide » et de « panne de sonde ». Le journal passe par l'accesseur lui-même : journal et champ publié **ne peuvent pas diverger** — une panne de sonde fait aussi retomber l'alerte (fail-closed), qui se réarme d'elle-même au retour de la mesure.

### Cache `_phLimitStreakDays` — zéro NVS dans `update()`

La série est relue par `readPhActuatorLimitStreak()` (config.cpp) au **même rythme** que la médiane R1, dans `refreshPhLoadObservation()` : au **démarrage différé** et à la **clôture de journée** — les deux seuls instants où l'anneau bouge. C'est la garantie « jamais de lecture NVS dans `update()` ». Avant amorçage : 0 par défaut (fail-closed).

### `scheduledMinutesToday()` (FiltrationManager)

Durée **programmée** de filtration du jour (minutes), −1 = indisponible. Prédicat de mode **aligné sur `decideFiltrationRun`**, via `parseFiltrationMode()` (feature-146 — l'ancienne comparaison `strcmp` directe est retirée, un seul site de correspondance jeton → sens désormais) : les modes **Auto** et **Programme** suivent tous deux la plage start/end (Programme = « horaires fixés par l'utilisateur », pas « sans planning ») ; **Marche** → **1440** (la pompe tourne en permanence, affichage seul — ne borne aucun dosage, voir `filtrationDosingHorizonMinutes()` ci-dessus) ; **Arrêt** et plage invalide → −1, rien n'est inventé. ⚠️ **L'ordre direct (`forceOn`/`forceOff`) a disparu** (feature-146) — la nuance qu'il nécessitait (« le forçage peut faire tourner la filtration hors plage ») n'a plus d'objet : le Mode Boost, lui, modifie le créneau **effectif** consommé par `decideFiltrationRun`, mais cette fonction continue de lire le créneau de **base** ([ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) — le maximum journalier annoncé reste une capacité **ANNONCÉE** fondée sur le planning de base (capacité stable), pas un décompte des minutes réelles. Plage `[start, end)` fermée à gauche, ouverte à droite (durée = end − start, modulo minuit), même sémantique qu'`isMinutesInRange`.

### Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---:|---|
| `kPhActuatorLimitConsecutiveDays` | 3 | Journées consécutives saturées requises — alignée sur le jalon d'observation terrain et `kPhIntegralRestoreMinHistoryDays` ; une journée isolée (canicule, baignade) n'alerte pas, trois ne sont plus un épisode mais un régime. `static_assert` : [2 ; 7] (tient dans l'anneau). |
| `kPhActuatorLimitDayFillRatio` | 0,80 | Fraction du maximum théorique comptant une journée « saturée ». `static_assert` : ]0,5 ; 0,9[. |

### Journal (par transition, jamais par tour)

| Message | Niveau | Quand |
|---|---|---|
| `[Dosage pH] Sous-dimensionnement suspecté : N jour(s) consécutif(s) au maximum délivrable (débit soutenable X.XX mL/min) et pH toujours au-dessus de la consigne — la limite horaire d'injection borne le débit sous le besoin du bassin` | `warning` | Montée de l'alerte (`tickPhUndersizedAlert()`) — diagnostic d'installation, pas un `critical` d'incident |
| `[Dosage pH] Suspicion de sous-dimensionnement levée : la série de journées saturées est interrompue ou l'erreur pH est rentrée (épisode de saturation clos)` | `info` | Retombée |

### Testabilité

Nouvelle suite native `test/test_native_ph_actuator_limit/` — total du projet **970 → 994 tests**, **5 mutations tuées sur 5** (validation par mutation exigée par le workflow projet).

## Porte de session pH robuste au bruit (feature-094, v2.36.0)

> [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md). Module pur : [`src/ph_session_logic.h`](../../src/ph_session_logic.h). Câblage : [`pump_controller.cpp`](../../src/pump_controller.cpp).

### Le problème : la bande de décision était plus étroite que le bruit

Les valeurs ci-dessous ne sortent pas d'un objectif de précision, elles sont **mesurées** sur la filtrée (celle que lit la régulation) les 03 et 04/09/2026 :

| Grandeur | Valeur mesurée |
|---|---|
| σ de la filtrée sur 60 s | **0,043 pH** |
| Crête-à-crête sur 15 min | **≈ 0,150 pH** |
| Bande d'hystérésis | 0,05 (démarrage) / 0,01 (arrêt) |
| **Constante de corrélation τc** | **≈ 50 s** (période dominante ≈ 5 min) |

**C'est τc qui débloque le dossier** : un bruit de τc ≈ 50 s est un processus **quasi blanc**, pas une oscillation lente — donc **le moyenner marche, et coûte peu de retard**. Une fenêtre de 10 min ramène σ à **≈ 0,017** pour **≈ 7 min** de retard, et laisse une ouverture sur échelon de +0,30 pH se produire en **< 4 min**.

Sans cela, près de la consigne, c'est le bruit qui ouvrait et fermait les sessions : chaque fermeture armait 15 min de [pause mélange](#pause-mélange-hydraulique-feature-025), le [feedforward](#dose-de-base-apprise--feedforward-ph-v2280) était retenu puis réarmé au même rythme, et le **rapport cyclique effectif tombait à ~1/3**. La série du 03/09 ne montre **aucune relation dose-réponse** (10 mL « font » −0,18 ; 83 mL « font » −0,08) : ce sont des fluctuations de mesure.

> **Le droop observé était un symptôme, pas l'équilibre.** Le bassin était parqué à **7,38-7,40**, pas à l'équilibre théorique 7,31 : la régulation était **bridée par la perte des 2/3 du rapport cyclique**. Restaurer ce rapport la fait **descendre vers la consigne** — **−0,07 pH au maximum**, dans le bon sens. C'est une correction, pas un déplacement.

### Un seul point de vérité : `phSessionGate()`

`phSessionGate(const PhGateIn&) → PhGateOut` produit **la porte ET la bande morte du PI** dans le même appel. C'est le cœur du lot : la régression v2.26.0 (« la session ne se referme jamais ») était née de la divergence entre ces deux décisions, écrites à deux endroits différents de `pump_controller.cpp` — le PI se parquait exactement au point de coupure de l'hystérésis. Les rendre indissociables rend ce défaut **structurellement impossible à réintroduire**.

Ordre des décisions (**aucune ne peut être réordonnée sans nouveau passage `pool-chemistry`**) :

| # | Décision | Effet |
|---|---|---|
| 1 | **Signal lent non prêt** (fenêtre en cours de remplissage après une reprise de filtration) | Aucune ouverture, **fermeture** d'une session en cours — fail-closed. La durée minimale ne s'y oppose pas. |
| 2 | **Plancher de sous-dépassement** : signal lent < **−0,05** | Fermeture **immédiate**, qui **prime sur la durée minimale** |
| 3 | **Hystérésis sur le signal lent** | Fermée → ouverture ssi `lent > phStartThreshold` (**inchangé**) ; ouverte → maintien tant que `lent > phStopThreshold − 0,03` ; sinon **prolongation** tant que la durée minimale murale n'est pas écoulée |
| 4 | **Régime de débit** pendant la prolongation | `lent ≤ phStopThreshold` → débit total **plafonné au feedforward seul** ; `lent ≤ 0` → **débit nul**, porte maintenue |

Le point 4 est la contrepartie indispensable de la prolongation : sans lui, `flow += phFfFlow` servirait le feedforward **aveuglément** pendant toute la durée minimale — le seul vrai danger de L3.

**Bande morte** : `wasOpen ? phStopThreshold : phStartThreshold` — position et rôle **strictement inchangés** (start hors session, stop en session, cible de l'intégrande `erreur − phStopThreshold`). **Déplacement du point d'équilibre : 0,000 pH**, condition d'acceptation du gate. Elle s'applique à l'erreur **instantanée** dans `computePidPure` : le signal lent ne décide **que** de la porte.

### Le signal lent (`PhDecisionFilter`, `phDecisionTick()`)

Moyenne glissante du **premier ordre** de constante de temps `kPhDecisionWindowMs`, alimentée par la **filtrée**. État POD, **loopTask uniquement** (écrivain unique, aucun mutex — même contrat que `PhFfState` / `PhIntegralState`). Tické **hors branche**, à côté du tick intégral et après la garde de présence d'eau : la porte doit continuer de mûrir pendant les pauses de mélange et les suspensions de limite horaire, où l'eau circule et la mesure est valide.

Trois règles de ré-amorçage (`phDecisionRearm()`), toutes fail-closed — plus **aucune décision** tant que la fenêtre n'est pas de nouveau pleine :

- **mesure non finie** (reprise de filtration : la filtrée redevient `NaN` le temps que la médiane se reconstruise ; le transitoire du 04/09 a duré **40 min**) ;
- **`dt` aberrant** (> `kPhIntegralMaxTickDtMs` = 10 s) : un trou d'évaluation donnerait α ≈ 1, c'est-à-dire une « moyenne » réduite au dernier échantillon **avec `primed` encore vrai** — exactement la décision sur bruit que ce module supprime ;
- `tauMs == 0` (défense).

Le pas de temps est **fourni par l'appelant**, jamais supposé : à α fixe, la fenêtre effective dépendrait de la charge CPU. La maturation (`warmupMs`) est une **addition saturée**, jamais un repliement.

### Le verrou de session change de sens — et ce que ça implique

`phDosingState.active` latchait sur **« un débit a été commandé »** (`flow > 0`). Il suit désormais le **verdict de porte**. Sans ce changement, ni le signal lent ni la durée minimale n'auraient d'effet : la session se refermait au premier tour où le PI retombait dans sa bande morte.

⚠️ **Conséquence observable, à connaître avant toute lecture de l'UI ou d'un tableau Home Assistant** : `isPhDosing()` est désormais **vrai pompe à l'arrêt** pendant une prolongation. Sont concernés, tous alimentés par ce seul accesseur :

| Surface | Champ / topic | Nouveau sens |
|---|---|---|
| `GET /data` | `ph_dosing` | **session ouverte** (la pompe peut être à l'arrêt) |
| WebSocket `sensor_data` | `ph_dosing` | idem |
| MQTT | `{base}/ph_dosing` (`ON`/`OFF`), `ph_dosing_active` du JSON de statut | idem |
| Historique | `history.phDosing` | idem |

**Aucun champ ajouté, aucun renommé.** Un client qui veut « la pompe tourne **maintenant** » dispose déjà du **calendrier d'impulsion** ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)) : pompe alimentée ⇔ `ph_pulse_phase_ms < ph_pulse_ms`.

**Le fail-closed est conservé** : `shouldDose` porte encore tous les refus avals (plafond de session, pompe non caractérisée via `evaluatePulseGate`), et un refus de plan referme aussi la session. **Aucune** de ces fermetures ne consulte la durée minimale.

### Ce que la porte NE fait PAS : les gardes gardent la priorité absolue

Plancher acide, butée brute, capteur instable, filtration absente, limite horaire, quota journalier, `maxCyclesPerDay`, anti-rafale, OTA, maintenance, watchdog : ces gardes ferment la session **dans le même tour**, arment la pause de mélange et journalisent, **quelle que soit la durée minimale**. Elles vivent en amont (`canDose` / `evaluateDose`) ou hors branche (`closePhDosingSession`) et **ne passent jamais** par `phSessionGate()`. Le paramètre `minSessionMs` n'est consommé que dans la branche hystérésis.

### Comptabilisation du cycle et journal de démarrage

La porte pouvant ouvrir la session **plusieurs tours avant** que le PI ne produise un débit (`computePID` rejette `dt > 10 s` au premier tick), `phStartDecided` (vrai un seul tour) ne peut plus servir de témoin. Un nouveau témoin `_phCycleCounted` garantit qu'un cycle est compté **une fois par session, au PREMIER DÉBIT RÉEL** — l'invariant « un cycle compté ⇔ un cycle réellement démarré » est préservé, son écriture change. Remis à `false` par `closePhDosingSession()`.

Deux conséquences de journal :

- la ligne `Démarrage dosage pH (auto)` est émise **au premier débit réel**, plus à la décision ;
- elle porte désormais **`erreur_lente=`** en plus de `erreur=`. Sans elle, le journal pouvait afficher une erreur **instantanée sous le seuil de démarrage** au moment même où la porte s'ouvrait — et envoyer le prochain diagnostic sur une fausse piste ;
- le warning **« Limite cycles atteinte »** devient possible **session ouverte** : les effets de bord journaliers de la porte de démarrage (rollover 24 h de `cyclesToday` + warning throttlé) ont été extraits dans `tickDosingCycleDay()` pour rester atteignables par le pH, dont la porte n'est plus `shouldStartDosing`.

> **Réserve connue, non corrigée volontairement** : `_phSessionStartMs` est posé au **premier débit réel**, pas à l'ouverture de porte — les 30 min murales partent donc du **premier pouls**. Sans conséquence chimique (la prolongation est en régime débit nul ou plafonné au feedforward) et de toute façon borné par `kPhSessionMaxWallMs` (3 h).

### Trois motifs d'arrêt de session (journal)

Un « erreur résorbée » unique ne distinguait plus un arrêt normal d'un sous-dépassement franc ni d'un ré-amorçage du signal. Le motif passé à `closePhDosingSession()` dit désormais **lequel des trois chemins de porte** a refermé la session :

| Verdict de porte | Motif journalisé |
|---|---|
| Sous-dépassement | `sous-dépassement du signal lent (pH passé sous la consigne)` |
| Signal ré-amorcé | `signal lent de décision ré-amorcé (mesure non représentative)` |
| Erreur résorbée | `erreur résorbée (signal lent, durée minimale écoulée)` |
| (fermeture par refus aval : pompe non caractérisée, plan invalide) | `erreur résorbée` — libellé **historique conservé** |

### Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---:|---|
| `kPhDecisionWindowMs` | 600 000 ms (**10 min**) | Fenêtre du signal lent — σ ramené à ≈ 0,017 pour ≈ 7 min de retard. Sert **aussi** de durée de remplissage avant toute décision |
| `kPhDoorCloseMarginPh` | **0,03** pH | Marge de fermeture : la porte ferme sous `phStopThreshold − 0,03` = **−0,02**. **N'affecte pas** la bande d'actionnement |
| `kPhSessionMinWallMs` | 1 800 000 ms (**30 min murales**) | Durée minimale de session. **Murale et non pompe-ON** : au rapport cyclique mesuré (3,4-5,3 %), 30 min de pompe-ON dureraient ~10 h murales — la borne serait décorative |
| `kPhSessionUndershootFloorPh` | **0,05** pH | Plancher de sous-dépassement, **prioritaire sur la durée minimale** |
| `kPhStartThresholdDefault` / `kPhStopThreshold` | 0,05 / 0,01 | **Promues `constexpr`** (elles ne vivaient que comme initialiseurs de `PumpProtection`) — vérification faite : jamais relues depuis la NVS, jamais exposées par `/get-config` ou `/save-config` |

**Sous-injection bornée** : pendant la prolongation, le débit est plafonné au feedforward (**2,60 mL/min** sur la configuration vivante) puis annulé dès que le signal lent passe sous la consigne. **Sous-dépassement pire cas calculé : −0,058 pH → 7,242**, au-dessus du plancher `kPhTargetMin` = 7,20.

### `static_assert` neufs (condition C12 du gate)

| Assertion | Ce qu'elle protège |
|---|---|
| `kPhStartThresholdDefault − kPhStopThreshold ≥ 0,04` | Une bande de décision **plus étroite que 2σ du signal lent** (0,020) serait de nouveau franchie par le seul bruit — le défaut que la feature corrige |
| `0 < kPhStopThreshold ≤ 0,05` | Un seuil d'arrêt nul rendrait le point fixe de l'intégration inatteignable ; au-delà de 0,05 l'hystérésis dégénère |
| `kPhSessionMinWallMs + kPhMixingDelayMs ≤ kPhSessionMaxWallMs` | Le plafond mural couperait des sessions que la durée minimale prolonge encore |
| `kPhSessionMinWallMs ≥ 2 × kPhDecisionWindowMs` | Une session doit durer au moins **deux fenêtres** : en deçà elle se refermerait avant que le signal lent ait pu constater l'effet du dosage |
| `kPhSessionUndershootFloorPh == kPhFfAdverseErrorPh` | Deux organes qui constatent **le même événement physique** ne doivent pas le constater à deux valeurs différentes (latch défavorable du feedforward, [ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)) |
| `0 < kPhDoorCloseMarginPh < kPhIntegralResetMargin` | Une marge nulle refermerait au point de coupure exact (régression v2.26.0) ; au-delà, elle empiéterait sur la purge sèche de l'intégrale, qui décrit un **tout autre** événement |
| `kHeapIntegrityMaxDurationUs × 100 ≤ kPulseMinMs × 1000` | Le parcours d'intégrité du tas doit rester **sous 1 % du pouls minimal** — voir [system-health.md](system-health.md#contrôle-dintégrité-périodique-du-tas-v2188) et l'amendement d'[ADR-0037](../adr/0037-postmortem-oom-et-integrite-tas.md) |

### Ce qui a été refusé (et pourquoi c'est écrit ici)

**Élargir la bande d'hystérésis au-dessus du bruit (L2) — REFUSÉ par le gate.** Couvrir le bruit brut de façon robuste exigerait **0,17 pH**, c'est-à-dire parquer le bassin à **+0,09** au-dessus de la consigne et le laisser descendre à **−0,08** en dessous. Or l'installation ne dispose que d'**acide** : sous la consigne, **aucun moyen de remonter** — remontée passive **≤ 0,02 pH/h**, ~4 h de récupération. Un levier qui déplace l'équilibre du côté où l'actionneur est absent est refusé. Détail et alternatives : [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md).

### Testabilité

Suite native [`test/test_native_ph_session/`](../../test/test_native_ph_session/) — logique pure ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)), y compris le **rejeu de la série réelle du 03/09** convertie en fixture. Total du projet : **30 suites / 1 154 cas**, **8 mutations tuées sur 8**.

⚠️ **Ce que le rejeu ne prouve PAS** : il est en **boucle ouverte**. Il démontre l'anti-chatter (moins de sessions, plus longues), **pas** que le bassin reste dosé — l'acide des 7 sessions réelles est déjà dans la série. Le critère « feedforward servi ≥ 80 % du temps » y ressort à **0,0 %** parce que le signal lent y siège **à la consigne** (moyenne 7,2999). Il est à mesurer sur l'appareil (enregistrement `/data` sur 4 h une après-midi à la consigne).

## Composition du débit d'injection — accesseurs de lecture seule (feature-092, v2.46.0)

> **Lecture seule stricte.** Aucun chemin nouveau vers les pompes : ces accesseurs **projettent** des grandeurs que la branche automatique calcule déjà. Visa `pool-chemistry` du 07/09/2026 : **GO de non-régression**, sous deux conditions implémentées (contre-exemple de composition testé, aucun appel à `getPhDecisionValue()` hors `loopTask`). Spec : [feature-092](../../specs/features/done/feature-092-carte-debit-injection-ph-orp.md).

### Les 8 accesseurs `const`

| Accesseur | Retour | Contrat |
|---|---|---|
| `getPhFeedforwardLearnedFlow()` | `float` (NAN → `null`) | Dose de base **apprise** = `min(charge médiane 7 j, kPhFfMaxSustainableFraction × soutenable)`, calculée **à la lecture** depuis le cache `_phLoadObs` — jamais depuis un instantané du tick, qui ne tourne que sous filtration hors purge. NAN sous `kPhIntegralRestoreMinHistoryDays` journées exploitables **ou** soutenable indisponible. Charge médiane nulle avec 3 journées ⇒ **0,00** (observation, pas indisponibilité). |
| `getPhPTermFlow()` | `float` | `kp·erreur`, **signé**, `0` dans la bande morte, **jamais borné**. NAN si la porte de validité des mesures est fermée, **et** NAN si `kp = 0` (régulateur inerte). |
| `getPhITermFlow()` | `float` | `ki·intégrale` **servi** (`0` dans la bande morte) — **distinct** de `getPhIntegralTerm()`, terme brut et contrat externe historique. |
| `getPhDemandFlow()` | `float` | `clamp(PI servi + FF, 0, ampFlow)`. |
| `getOrpPTermFlow()` / `getOrpDemandFlow()` | `float` | Mêmes règles, régulation **P-pure** : la demande **vaut** le terme proportionnel borné. |
| `getOrpSustainableMlPerMin()` | `float` | Miroir strict de `getPhSustainableMlPerMin()` : recalcul à la demande depuis `orpSustainableFlowFromConfig()`, NAN si ≤ 0 ou non fini. |
| `isOrpLimitedByHourlyBudget()` | `bool` | Verdict, jamais une comparaison laissée au client. Vrai si `orpDosingState.usedMs ≥ limite` **ou** si `demande ≥ soutenable` (deux faces du même phénomène). Faux si la limite est désactivée (≤ 0 ou ≥ 60 min/h) et faux quand la mesure n'est pas représentative — même fail-closed que le pH : servir un verdict figé ferait annoncer « bridé » sur une filtration à l'arrêt. **Il n'y a pas de terme intégral ORP**, donc aucune hystérésis d'épisode : au voisinage exact du plafond, la note « plafonnée » peut apparaître et disparaître d'un rafraîchissement à l'autre. |

**Course bénigne assumée**, patron `isAnyPumpEnergized` : l'instantané est écrit par `loopTask` (branche automatique) et lu en contexte web async / tâche MQTT. La copie passe par un **spinlock `portMUX`** (`doseCompositionSnapshot()`, patron feature-096) : les cinq flottants d'une **même** composition sont donc cohérents **entre eux**. Le spinlock ne prétend pas rendre l'instantané synchrone avec la mesure publiée dans la même trame — pire cas, un instantané en retard d'un tour de loop. Aucun de ces accesseurs ne touche l'I²C, LittleFS, la NVS, ni `getPhDecisionValue()` (réservée à `loopTask`, condition C2 du gate feature-096) : ils sont sûrs dans un handler `AsyncWebServer`.

### Traduction de statut `3 → 0`, et sa précédence

`getPhFeedforwardStatus()` s'appuie désormais sur le module pur `phFeedforwardServedStatus(raw, sessionOpen, historyDays, learnedFinite)` ([`src/ph_feedforward_logic.h`](../../src/ph_feedforward_logic.h)), qui regroupe **les deux** traductions :

| Ordre | Règle |
|---:|---|
| **1** | `raw == 3` (historique insuffisant) ⇒ `0` si `historyDays ≥ kPhIntegralRestoreMinHistoryDays`, sinon `3` |
| 2 | `raw == 2` ou `4` ⇒ servis **tels quels** (vrais entre deux ticks) |
| 3 | `raw == 1` et session fermée ⇒ `0` (traduction préexistante d'un instantané périmé) |
| 4 | `!learnedFinite` ⇒ `0` (fail-closed : servir « actif » à côté d'un tiret serait un contresens) |

⚠️ **Le statut 3 est testé AVANT le fail-closed `!learnedFinite`**, et cet ordre est porteur : au moment où l'anneau atteint 3 journées, la dose apprise devient finie **par la même occasion** — inverser les deux branches ne changerait rien au résultat nominal, mais rendrait le cas « 3 journées, soutenable indisponible » indiscernable de « historique insuffisant », donc afficherait « en apprentissage » sur une pompe non caractérisée. C'est le défaut du 03/09 dans une autre robe.

Le contrat `PhFfStatus` est **inchangé** : aucun code ajouté (en ajouter un exigerait un ADR).

### Bornage en deux étages — non réordonnable

La composition vit dans le module pur `doseComposition()` ([`src/dosing_logic.h`](../../src/dosing_logic.h) / `.cpp`), **miroir littéral du tick** :

```
piServed = clamp(p + i, 0, max(0, ampFlow − ff))   // étage 1 : le COUPLE
demand   = clamp(piServed + ff, 0, ampFlow)        // étage 2 : la SOMME
```

**La borne basse à zéro porte sur le couple `p + i`, jamais sur la somme des trois termes.** Le tick réduit d'abord le plafond du PI de la part servie au feedforward (`phPiMaxFlow = ampFlow − ff`, borné ≥ 0), borne le PI dans `[0 ; ce plafond]`, **puis** ajoute le feedforward et clampe à `ampFlow`.

Déplacer la borne basse **après** l'addition changerait le **dosage réel** : avec `ff = 1,41`, `p = −3,00`, `i = 0` et régime `Full`, le tick injecte **1,41 mL/min**, tandis que `clamp(p + i + ff, 0, ampFlow)` donnerait **0**. L'écart n'est pas cosmétique — c'est un dosage réel contre un dosage nul. Ce piège a été attrapé en revue ; le **premier test** de [`test/test_native_dose_composition/`](../../test/test_native_dose_composition/) est ce contre-exemple, exigé par `pool-chemistry`.

**La bande morte se LIT, elle ne se recalcule pas** : `deadband` vient du verdict de porte de session (`phSessionGate`, feature-094) — `phStopThreshold` (0,01) session ouverte, `phStartThreshold` (0,05) session fermée. La reconstruire depuis les seuils mentirait dans la bande `]0,01 ; 0,05]` : c'est le mécanisme exact du défaut v2.26.0.

**Régimes de service du feedforward** (`DoseFlowRegime`, contrat **interne**, jamais sérialisé) : `Full` (PI + FF servis), `CappedToFeedforward` (PI annulé, FF seul — prolongation), `Zero` (débit nul, bassin repassé sous la consigne). **Fail-closed** : `error` non finie ⇒ les cinq flottants à NAN.

### Le filet de divergence (un WARN par session)

Les deux écritures de l'équation — la projection d'observabilité et le calcul PI qui commande réellement — sont comparées **dans la branche de dosage**, seul endroit où le débit appliqué existe :

| Élément | Valeur |
|---|---|
| Constante | `kDoseCompositionDivergenceMlPerMin` = **0,05 mL/min** |
| Cadence | **un seul WARN par session** (`_phCompDivergenceLogged`) |
| Témoin | **RAM seule** — aucune persistance, aucun champ servi |
| Neutralisé quand | le PI est **muet** : premier tick (`lastTime == 0`) ou intervalle > 10 s, les deux sorties anticipées de `computePID()` où le tick sert `0 + ff` sans que la projection les modélise |

Le message nomme les deux nombres, l'écart, l'âge de session, l'erreur pH, le FF, le plafond **et la bande morte** — c'est cette dernière qui pointe l'entrée du défaut v2.26.0, et le tampon a été dimensionné à 480 o **pour ne pas la tronquer** (un `msg[248]` coupait à « …bande mor »).

### Asymétrie GELÉE — servir le FF sur une erreur vive franchement négative

Le régime `HoldZero` statue sur le **signal lent** (moyenne glissante de 10 min de la filtrée, `phSessionGate`), pas sur l'erreur instantanée. Il existe donc une fenêtre où l'**erreur vive** est franchement négative — le bassin est repassé sous la consigne à l'instant — alors que le régime sert encore le feedforward, parce que le signal lent n'a pas encore basculé. **Cette asymétrie est gelée dans ce cycle** : la corriger serait un changement de **dosage**, hors du périmètre purement observationnel de feature-092. Elle est renvoyée à une spec de suivi. Ce qui a changé, en revanche, c'est que l'écart est désormais **lisible** : `ph_ff_ml_per_min` rapporte le FF **du régime appliqué**, donc `0,00` en régime de maintien à zéro.

### `ph_ff_ml_per_min` passe au régime appliqué (correction, pas rupture)

`getPhFeedforwardFlow()` lit désormais le `ff` de l'**instantané de composition** (régime appliqué) et non plus le verdict brut du tick. Conséquence **unique** : pendant une prolongation de session à débit nul (régime `Zero`), le champ rapporte **0,00** au lieu de la dose de base. C'est exactement ce que le champ documente depuis l'origine (« appliqué »), et c'est ce qui rend **exacte** la reconstitution client `PI servi = demande − FF`. La dose de base **apprise** reste, elle, lisible en permanence par `getPhFeedforwardLearnedFlow()`.

### Fenêtre résiduelle des trois sorties anticipées

⚠️ **Connue, documentée, non corrigée.** Le point de publication de la composition vit dans la branche automatique d'`update()`. Or `update()` a **trois** sorties anticipées qui reviennent **avant** ce point **sans invalider `measuresValid`** — la filtration peut très bien tourner :

1. **OTA en cours** (`closePhDosingSession(now, "OTA en cours")`) ;
2. **mode maintenance** (inhibition totale du dosage) ;
3. **assistant de mesure du débit** (`flowAssistSuspend`).

Pendant ces trois états, l'instantané n'est **pas rafraîchi** : la carte, `/data`, le WebSocket et MQTT servent la **dernière** composition connue avec des mesures déclarées valides. Fenêtre **bornée et déclenchée par l'opérateur** (aucune n'est spontanée), sans commune mesure avec le défaut initial — d'où la décision de ne pas la corriger dans ce cycle.

> **Pourquoi cette trace est là et pas ailleurs** : c'est la **même** chaîne de sorties anticipées que celle visée par **bug-115** (l'état de comptabilité restait rémanent pendant la fenêtre aveugle, et toute la durée de la coupure était créditée au débit plateau à la reprise) — **corrigé en v2.61.0**, voir [Correction de la rémanence de comptabilité](#correction-de-la-rémanence-bug-115-v2610--point-de-passage-unique) plus bas. Le défaut **voisin** décrit ci-dessus (instantané de composition périmé, `bug-116`) reste, lui, **non corrigé**. Même famille, même fonction : quiconque touche à `update()`/`updateDosingCore()` doit trouver les **deux** traces au même endroit, sinon il rejouera l'un ou l'autre.

### Publication

Les 8 champs sont publiés en **parité stricte** `/data` ↔ WS (marqueurs `PARITE-DATA`, [ADR-0071](../adr/0071-parite-data-trame-capteurs.md)) et **5** d'entre eux sur MQTT :
[API.md](../API.md#champs-de-composition-du-débit-dinjection-feature-092-v2460) · [ws-manager.md](ws-manager.md#champs-sensor_data-ajoutés-en-feature-092--composition-du-débit-dinjection-v2460) · [MQTT.md](../MQTT.md#topics-et-entités-ajoutés-en-v2460-composition-du-débit-dinjection) · UI : [page-ph.md](../features/page-ph.md#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460), [page-orp.md](../features/page-orp.md#carte--débit-dinjection-orp--orp-card-dosing-feature-092-v2460).

### Testabilité

[`test/test_native_dose_composition/`](../../test/test_native_dose_composition/) — **41 cas** couvrant `doseComposition()`, `phFeedforwardLearnedFlow()` et `phFeedforwardServedStatus()`. Campagne de **mutation** : 11 mutants (9 sur la logique, 2 sur la sentinelle de parité), **aucun survivant**. La suite [`test_native_ph_feedforward/`](../../test/test_native_ph_feedforward/) est restée **inchangée et verte** — c'est elle qui prouve que le tick n'a pas bougé.

## Comptabilisation d'un démarrage de cycle : invariant (v2.20.5)

> **INVARIANT** : dans les branches `automatic` (pH **et** ORP), **un cycle est comptabilisé si et seulement si un cycle a réellement démarré** — c'est-à-dire si un duty non nul a été commandé ce tour de boucle.

Ce que « comptabiliser » englobe (les quatre effets sont indissociables) : `cyclesToday++`, l'entrée dans le **ring anti-rafale** (`recordDosingCycleStart`), la ligne `INFO Démarrage dosage …` et la pose de `lastStartTime`.

### Le défaut corrigé (double comptage)

`computePID()` rend **0** quand `dt > 10 s` (garde anti-delta-aberrant, [`pump_controller.cpp:732`](../../src/pump_controller.cpp:732)) et n'est appelé **que** lorsque le dosage est décidé. Or il s'écoule ~29 min entre deux cycles (pause mélange + anti-cycling) : le **premier tour de chaque cycle rend donc toujours un débit nul**.

L'ancien code comptabilisait sur la seule **décision** de doser, alors que `active` ne se latchait que sur un débit effectif. Enchaînement :

1. tour *n* — `shouldStartDosing` dit oui → cycle **compté**, débit 0, `active` **non** latché ;
2. tour *n+1* — l'état est toujours « arrêté » → on repasse par `shouldStartDosing` → **second** cycle compté, cette fois avec du débit.

Conséquences observées en production : **deux** lignes « Démarrage dosage » à la même seconde, **deux** entrées dans le ring anti-rafale, et un budget cycles/jour valant réellement **la moitié** de sa valeur configurée.

```
[17:39:21] Démarrage dosage pH (auto): pH=8.05 cible=7.20 erreur=0.854 (cycle 3/48)
[17:39:21] Démarrage dosage pH (auto): pH=8.05 cible=7.20 erreur=0.854 (cycle 4/48)
```

### Pourquoi l'invariant est structurel

La comptabilisation a été **déplacée après** le calcul du débit et conditionnée au **même prédicat** que le latch de `active` (`phActive` / `orpActive`). Ce n'est pas une garde ajoutée à côté du latch, qui pourrait en diverger : c'est le **même booléen** qui décide des deux. Un double comptage redeviendrait donc possible **seulement** si l'on remettait un comptage en amont du calcul de débit.

`computePID` et `pid.lastTime` sont **intacts** : **aucun `dt` n'est synthétisé** (prescription **P5** du gate `pool-chemistry`). Amorcer artificiellement `lastTime` à l'entrée du cycle serait un piège latent le jour où `ki` deviendrait non nul (un `dt` inventé fabriquerait de l'intégrale). C'est la comptabilité qui attend le débit, jamais le PID qu'on force.

> ⚠️ **Ce jour est arrivé en v2.26.0** : `ki` **n'est plus nul côté pH**. La prescription P5 cesse d'être une précaution théorique et devient une garde active — le module pur `ph_integral_logic` la reprend à son compte (base de temps propre, `dt > 10 s` **rejeté** et non crédité d'un bloc, reliquat sous le quantum reporté). Aucun `dt` n'est synthétisé nulle part.

Le premier tour « à vide » de chaque cycle **subsiste** donc : il coûte un tour de boucle, aucun produit et aucun cycle.

### Logique pure

```cpp
struct CycleStartAccounting { bool countCycleStart; };

CycleStartAccounting evaluateCycleStartAccounting(bool wasActive, bool startDecided,
                                                 bool flowActive);
```

[`src/dosing_logic.h`](../../src/dosing_logic.h) — `countCycleStart = !wasActive && startDecided && flowActive`. Couverte par **4 tests natifs** dédiés dans `test/test_native_dosing/`, dont le **rejeu du scénario réel** traversant la garde des 10 s (609 tests natifs au total).

### `WARN` « démarrage décidé mais aucun débit commandé »

Corollaire du correctif : avant lui, une régulation qui décidait de doser **sans jamais produire de débit** réémettait une ligne de démarrage à chaque tour — un bruit involontaire, mais **c'est lui qui a révélé le bug**. La situation serait désormais totalement silencieuse. Elle est donc rendue explicite :

| | |
|---|---|
| Niveau | `WARNING` |
| Message | `[Dosage pH] décision de doser SANS débit commandé depuis <n> tours — régulation sans effet (boucle trop lente, ou consigne sous le plancher de duty). Erreur=<e>` (symétrique ORP, erreur en mV) |
| Throttle | streak ≥ `kDosingNoFlowWarnStreak = 5` tours consécutifs **ET** `kDosingNoFlowWarnIntervalMs = 300000` ms (**5 min**), compteurs **indépendants** par grandeur ([`constants.h`](../../src/constants.h)) |
| Condition | `startDecided && !active` **et** ni refus de **plan** (v2.22.0 : entrées invalides fail-closed uniquement) ni refus de **gate de caractérisation** |

⚠️ **Comment le lire.** Le premier tour de chaque cycle remplissant la condition de base par construction (voir ci-dessus), le streak de 5 tours filtre le régime sain (série de 1 en nominal). Ce qui est anormal, c'est une occurrence **toutes les 5 min en continu** : la régulation décide alors de doser sans que rien ne sorte. Deux états ne nourrissent **pas** le streak (sinon inondation type 2.20.2) : le refus de **plan** (v2.22.0 : entrées invalides fail-closed, déjà silencieux par conception — l'ex-refus de seuil C n'existe plus, le régime sous-plancher maintient la séance ouverte, `phActive`/`orpActive` restent `true`, et ne passe donc **jamais** ici) et le refus de **gate** (pompe non caractérisée / amplitude sous plancher — critical throttlé dédié). Le WARN reste réservé aux vrais cas anormaux (PID muet plusieurs tours). Voir [logger.md](logger.md#rétention-réelle-du-journal-persistant-v2202).

## Limite de cycles/jour configurable (garde 9, feature-060, v2.12.0)

La garde 9 anti-cycling (`cyclesToday >= maxCyclesPerDay` dans `evaluateDose` / `shouldStartDosingPure`, garde 7 côté `evaluateManualInject` — **même source** `pumpProtection.maxCyclesPerDay`, compteur partagé auto + manuel) n'est plus une valeur codée en dur :

- **Défaut : 24** (v2.20.5 — était 48 depuis feature-060, 20 avant). Fondement historique du 48 (verdict `pool-chemistry` 2026-07-29) : un régime d'entretien **sain** par petites retouches (~5 mL/cycle, pause mélange 20 min) produit naturellement **20-40 cycles/jour** — la limite de 20, non fondée chimiquement, le bloquait en milieu de journée (incident du 2026-07-29 : régulation ORP arrêtée à 104 mL injectés, dérive ORP tout l'après-midi = risque sanitaire). Le chattering rapide reste couvert par l'anti-rafale (6/min, 20/15 min, en dur) et la pause mélange borne physiquement la cadence à **~70 démarrages/j** : la garde ne couvre que l'emballement lent (sonde mentant sous la cible).
- **Pourquoi 48 → 24 (condition P5 du gate `pool-chemistry`, v2.20.5)** : à cause du [double comptage de cycle](#comptabilisation-dun-démarrage-de-cycle--invariant-v2205), l'enveloppe **réellement vécue** valait déjà **24**. Corriger le comptage sans abaisser la limite aurait **doublé** le nombre d'injections quotidiennes réelles — sous couvert de correctif de bug, et sans qu'aucune décision chimique ne l'ait validé. 24 reste dans le régime d'entretien sain (20-40/j) et dans les bornes dures `[10, 60]`.
- ⚠️ **PIÈGE — ce défaut n'est qu'un repli, il ne migre rien.** `max_cycles_day` étant **persisté en NVS**, un appareil déjà provisionné **conserve sa valeur au flash** : passer à la v2.20.5 ne fait donc **pas** descendre un 48 enregistré vers 24. Aucune migration n'a été écrite (l'écriture d'un défaut par-dessus un choix utilisateur serait pire). Le nouveau défaut ne s'applique qu'aux appareils **sans** la clé NVS (première mise en service, `factory`). Sur l'appareil de production, la valeur a été portée à 24 **à la main via `/save-config`** (vérifié : 48 → 24, autres limites inchangées) ; **tout autre appareil déjà provisionné doit être réglé manuellement** dans Paramètres → Régulation.
- **Configurable** via `POST /save-config` (champ `max_cycles_per_day`), UI : Paramètres → Régulation pH / ORP (voir [page-settings.md](../features/page-settings.md#régulation-panel-regulation)).
- **Bornes dures** : `kCyclesPerDayMin = 10` / `kCyclesPerDayMax = 60` ([`constants.h:423`](../../src/constants.h:423)) — **ne JAMAIS élargir sans nouveau verdict `pool-chemistry`** (60 reste sous la cadence physique ~70/j ; 10 est le plancher sous lequel la garde bloquerait un régime d'entretien sain).
- **Clamp serveur systématique** via la fonction pure `clampCyclesPerDay(long requested, bool* wasClamped)` ([`dosing_logic.h:156`](../../src/dosing_logic.h:156)) — c'est une **sanitization de config, PAS une garde de dosage** (hors de l'ordre figé des gardes). Appliqué à **deux points d'entrée** : la route `/save-config` ([`web_routes_config.cpp:337`](../../src/web_routes_config.cpp:337) — le `min`/`max` HTML ne suffit pas) et le **chargement NVS** ([`config.cpp:389`](../../src/config.cpp:389) — valeur écrite par un firmware antérieur ou bidouillée).
- **Persistance NVS** : clé `max_cycles_day` ([`config.cpp:193`](../../src/config.cpp:193)). Clé absente (migration depuis ≤ 2.11.x) → défaut de la struct, soit **24** depuis la v2.20.5 (48 avant).
- **Traçabilité (2 `WARN`)** : `max_cycles_per_day hors bornes : X → Y` (valeur clampée, route ou NVS) et `Limite cycles/jour modifiée : ancienne → nouvelle` (toute modification effective).
- **Effet immédiat** : le compteur `cyclesToday` en cours n'est **pas** remis à zéro à la modification — seule la borne de comparaison change (dans les deux sens).
- **Inchangés** : exemption `cyclesToday` du mode `scheduled` ([ADR-0021](../adr/0021-repartition-scheduled.md)), anti-rafale, limites volumétriques/horaire, ordre des gardes, message UI « Limite cycles/jour atteinte — reprise à minuit ».

> **Double validation `pool-chemistry` (2026-07-29)** : GO pré-design **sous conditions** (48, bornes 10-60, clamp serveur, logs — conditions toutes implémentées) + **GO** sur re-validation du diff d'implémentation. Voir [feature-060](../../specs/features/done/feature-060-limite-cycles-configurable.md).

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

## Survie du budget horaire de dosage au redémarrage (feature-136, v2.58.0)

Jusqu'à la v2.57.x, `DosingState` (`usedMs`, `windowStart`, `cyclesToday`), `_mixingEndMs[2]`
et le ring anti-rafale (`_dosingCycleHistory[2][20]`) étaient **entièrement en RAM**. Relevé
sur l'appareil le 2026-10-08 : `ph_limit_minutes = 10` ⇒ **deux redémarrages dans l'heure
autorisaient jusqu'à 20 minutes d'acide**, seul le plafond journalier (2 000 mL) faisant
barrage — **la seule garde du projet qui autorisait à tort** plutôt que de refuser à tort.
Gate `pool-chemistry` du 2026-10-08 : GO sous 12 conditions, repris ci-dessous.

### Deux supports redondants, fusionnés « au plus défavorable »

| | T2 — RAM RTC | T1 — NVS |
|---|---|---|
| Stockage | `RTC_NOINIT_ATTR DosingWindowRtcRecord` ([`dosing_window.cpp:24`](../../src/dosing_window.cpp:24)) | namespace **dédié** `pool-dose`, clé `snap` |
| Survit à | reset logiciel / panic / watchdog | **tout**, y compris coupure secteur |
| Écoulé mesuré par | compteur **monotone** `esp_rtc_get_time_us()` (RTC_CNTL_TIME, condition C7) — **indépendant de l'horloge murale** | `epochAtWrite` : ancre sur l'heure murale, exige `isTimeValid()` pour être interprétée |
| S'invalide d'elle-même | non (bruit électrique au power-on peut, par chance, satisfaire le CRC — d'où le filtre `resetIsSoftware()`, [`dosing_window.cpp:39`](../../src/dosing_window.cpp:39), qui exige en plus un motif de reset **logiciel**) | oui, implicitement : sans heure valide, `t1Available = false` |

Le compteur T2 couvre le cas **le plus fréquent** (OTA hebdomadaire, auto-guérison, panic) sans
dépendre d'une horloge qui n'a peut-être pas resynchronisé ; exiger l'horloge murale seule
aurait renvoyé ce cas en T3 — une heure de blocage à **chaque** déploiement, chimiquement pire
que la dette ci-dessous.

**Quand les deux sont disponibles et se contredisent, le verdict retient le plus défavorable
GRANDEUR PAR GRANDEUR** (`usedMs` max, `cyclesToday` max, reliquat de pause mélange max) —
**jamais la moyenne, jamais le plus récent**. Voir `dosingWindowResolveChannel()`
([`dosing_window_logic.h:296`](../../src/dosing_window_logic.h:296)). L'ancre (`windowStart` /
`cyclesDayStart`) suit systématiquement la source qui a fourni la grandeur retenue : mélanger
l'ancre d'une source avec la valeur de l'autre produirait une paire incohérente.

### T3 — fail-closed, option (d) : une seule fenêtre, jamais un blocage indéfini

Quand ni T1 ni T2 ne sont exploitables (première mise en service, NVS effacée, RAM RTC
corrompue par un power-on) : `usedMs` est posé à la limite pour **une seule fenêtre horaire**
(`kDosingWindowHourMs`), puis retour au nominal dès l'heure suivante.

L'alternative « bloquer jusqu'à horloge valide » a été **examinée et refusée** par le gate, et
le motif est **chimique, pas ergonomique** : RTC morte + sans réseau = blocage indéfini de
l'acide. Sous charge basique permanente et avec une régulation P pur (droop structurel), le pH
sort de la plage 7,20-7,50 et finit par franchir `kPhOrpInhibit` = 7,60 ; la fraction HOCl
active tombe de ~52 % à pH 7,50 à ~41 % à pH 7,70, et **la boucle ORP compense alors par
surdosage de chlore** pour tenter de maintenir le potentiel. *« Une protection qui crée un
surdosage sur l'autre chimie n'est pas une protection. »* Un budget réduit (plutôt que posé à
la limite d'une seule fenêtre) a aussi été écarté : il aurait une sémantique neuve et
**autoriserait** un dosage sur un budget antérieur inconnu — l'exposition même que ce lot ferme.

L'option (d) l'emporte sur un simple blocage muet parce que l'état « fenêtre horaire épuisée »
est **déjà exposé partout** — carte Régulation, `ph_used_ms` en MQTT/WS, libellé de refus — :
aucun chemin de refus silencieux n'est introduit. Coût assumé : au pire une heure de capacité
perdue après un démarrage à froid sans horloge, soit ≤ 10 min d'acide différés, rattrapables à
la fenêtre suivante.

### Le piège des deux péremptions : 1 h (fenêtre) ≠ 24 h (cycles/jour)

Deux horloges distinctes périment deux choses différentes : la **fenêtre horaire** (`usedMs`,
`kDosingWindowHourMs = 3 600 000 ms`) et le **jour des cycles** (`cyclesToday`,
`kDosingWindowDayMs = 86 400 000 ms`, condition C3). Les confondre viderait l'anti-cycling **à
chaque heure** — un redémarrage à n'importe quel moment de la journée remettrait `cyclesToday`
à zéro toutes les 60 minutes. `dosingWindowRebaseAnchor()` applique donc la **même formule** de
rebasage mais avec une `periodMs` différente selon la grandeur traitée.

### `cyclesDayStart` est indispensable — le mutant clé de la suite de tests

Sans `cyclesDayStart` persisté, [`pump_controller.cpp:3240`](../../src/pump_controller.cpp:3240)
(`if (st.cyclesDayStart == 0 || now - st.cyclesDayStart >= 86400000UL) { st.cyclesToday = 0;
st.cyclesDayStart = now; }`) rencontrerait systématiquement `cyclesDayStart == 0` **au premier
tick** après redémarrage — un code qui, sans en avoir l'air, **redéfinit secrètement le
correctif en no-op** : `cyclesToday` repartirait à 0 à chaque reboot comme avant, en laissant
croire que la garde anti-cycling est rétablie alors qu'elle ne l'est pas. C'est pourquoi
`cyclesDayStart` fait partie des champs persistés au même titre que `usedMs`, et pourquoi la
suite native fait d'« omettre `cyclesDayStart` » un mutant **obligatoire** (condition C12).

### La pause de mélange est ARMÉE au restore si l'instantané porte « pompe-ON »

Condition C5 du gate, **trou manquant du plan initial** — ni la spec ni le plan ne l'avaient
vu. Si l'instantané (T1 ou T2) a été écrit pendant que la pompe était physiquement alimentée,
le produit **a déjà été injecté**, mais `notifyPhDose()` / `notifyOrpDose()` n'ont jamais pu
tourner : le reboot a interrompu le cycle avant l'arrêt normal de l'injection, qui est leur seul
point d'armement (voir [Pause mélange hydraulique](#pause-mélange-hydraulique-feature-025)
ci-dessus). Sans cet armement explicite au restore, l'appareil redoserait sur une mesure **non
représentative** du produit déjà présent dans l'eau — un chemin de surdosage à part entière.
`dosingWindowResolveChannel()` réarme donc la pause **pleinement** (`kPhMixingDelayMs` /
`kOrpMixingDelayMs`, pas un reliquat calculé) dès que `pumpOnAtWrite` est vrai sur l'une ou
l'autre source, prioritaire sur tout calcul de reliquat.

### Cadence d'écriture NVS : chiffrée, namespace dédié

T1 écrit dans son **propre** namespace `pool-dose` (pas `pool-daily`), à une cadence **dédiée**
de 30 s pendant que l'une des deux pompes est alimentée (`kDosingWindowPersistIntervalMs`,
[`dosing_window.cpp:260`](../../src/dosing_window.cpp:260)), plus une écriture immédiate à
chaque transition pompe-ON ↔ pompe-OFF (événement, retenté au tour suivant si le mutex ou
l'horloge ne sont pas disponibles — `_dosingWindowNvsEventPending`,
[`pump_controller.h:788`](../../src/pump_controller.h:788)).

**≈ 62 000 écritures/an en régime réel ⇒ 271 ans avant les 100 000 cycles d'endurance garantis
par le wear-levelling NVS (48 ans au plafond physique estimé)** — très en deçà du risque d'usure.

Le flush existant de `saveDailyCounters()` (namespace `pool-daily`, différé 60 s) a été
**écarté à dessein comme passager** : il aurait couplé un cycle de vie **horaire** (le budget
de dosage) à une transaction **journalière** partagée par trois autres consommateurs sous
`configMutex` — toute future modification de l'un des trois aurait risqué de retarder ou de
bloquer l'écriture de l'autre, sans lien fonctionnel entre eux.

### Occupation de la RAM RTC (`RTC_NOINIT_ATTR`)

L'instantané T2 de ce lot (`DosingWindowRtcRecord`) pèse **220 octets** (le ring seul en
représente 160 — `uint32_t[2][20]`, le plan initial l'avait chiffré à tort à 80). Cette zone
mémoire est **partagée** par plusieurs modules du projet, chacun avec sa propre structure
(`heap_ring`, `oom_guard`, `time_to_target`, `measure_validity`, l'auto-guérison de
`main.cpp`) ; aucun inventaire consolidé n'existait avant ce lot :

| Consommateur | Structure |
|---|---|
| feature-136 (ce lot) | `DosingWindowRtcRecord`, 220 o |
| `measure_validity` ([measure-validity.md](measure-validity.md)) | `OutageRtcRecord` |
| `time_to_target` ([time-to-target.md](time-to-target.md)) | `TttRtcRecord` |
| `heap_ring` / `oom_guard` ([oom-guard.md](oom-guard.md)) | ring de post-mortem + mots de secours |
| auto-guérison (`main.cpp`) | magic + compteur de reboots + epoch de fenêtre |
| **Total mesuré au linker** | **1 396 / 7 680 o (18,2 %)** |
| **Marge restante** | **6 284 o** |

### Nouvelle ligne de journal au démarrage

```
[Dosage] Budget horaire pH restauré (source : le plus défavorable (T1+T2), coupure=142s) : usedMs=312000/3600000 cyclesToday=4 mélange=180s
```

Émise une fois par voie (pH, ORP) depuis `DosingWindowManager::begin()`
([`dosing_window.cpp:207`](../../src/dosing_window.cpp:207)), en `WARNING` si la source retenue
est `kFailClosed`, en `INFO` sinon. **L'écoulé et son statut (coupure connue vs « écoulé=inconnu
») y figurent exprès** (condition C11, prescription S2 du gate P2) : sans eux, un repli
conservateur (C4 : instantané intact mais écoulé incalculable) serait **indiscernable** d'une
reprise nominale dans le journal — et la dette ci-dessous (C7) ne pourrait **jamais** être levée
par la seule observation du terrain. Deux mots qui transforment une surveillance en preuve.

### Dette assumée : C7 non prouvée on-target

Le compteur monotone de T2 (`esp_rtc_get_time_us()`) **n'a pas été prouvé, sur matériel réel,
traverser `esp_restart()`** — seule sa consommation (un `bool unknownElapsed` déjà tranché) est
couverte par les tests natifs. Le gate a jugé cette dette **acceptable** parce que son mode de
défaillance est **structurellement conservateur** : un compteur qui reviendrait en arrière
(`rtcNowUs < writtenUs`) bascule en `unknownElapsed` → repli C4 (fenêtre restaurée
intégralement, ancre `now`) ; un compteur qui redémarrerait à zéro **sous-estimerait** l'écoulé,
ce qui ne fait que **retenir** le budget plus longtemps que nécessaire — jamais l'inverse.
Cette dette est **à retirer par l'observation** : la ligne de journal ci-dessus, en particulier
le champ « coupure=… », permet de vérifier sur plusieurs redémarrages OTA que l'écoulé rapporté
est plausible.

### Fichiers

[`src/dosing_window_logic.h`](../../src/dosing_window_logic.h) (module **pur**, ADR-0017 :
structures `DosingWindowRtcRecord`/`DosingWindowNvsRecord`, rebasage d'ancre, résolution
T1/T2/T3) · [`src/dosing_window.{h,cpp}`](../../src/dosing_window.h) (coquille : RTC_NOINIT,
`Preferences`, `esp_reset_reason()`, `esp_rtc_get_time_us()`, journalisation) ·
`PumpControllerClass::applyRestoredDosingWindow()`
([`pump_controller.cpp:291`](../../src/pump_controller.cpp:291), appelée une seule fois depuis
`begin()`) · hooks `refreshRtc()` / `persistNvsIfDue()` dans `update()`
([`pump_controller.cpp:3176`](../../src/pump_controller.cpp:3176)) · `dosingWindow.begin()`
appelé depuis `main.cpp` **après** `rtcManager.applyToSystem()` et **avant**
`PumpController.begin()`. Tests : [`test/test_native_dosing_window/`](../../test/test_native_dosing_window/).
Voir [ADR-0089](../adr/0089-ce-qu-un-redemarrage-a-le-droit-doublier.md).

## Stabilisation post-calibration et passage de minuit (v2.34.0)

**Stabilisation par pompe** (feature-021) : `_stabilizationEndMs[2]` — un timer indépendant pour pH (index 0) et ORP (index 1). La cinétique chimique différente justifie une fenêtre par sonde.

| Source d'arming | Fonction | Durée |
|-----------------|----------|-------|
| Calibration EZO pH réussie (`Cal,mid`, `Cal,low`, `Cal,clear`) | `armStabilizationTimer(0)` | `kStabilizationDurationPhMs` = **5 min** |
| Calibration EZO ORP réussie | `armStabilizationTimer(1)` | `kStabilizationDurationOrpMs` = **3 min** |
| Passage de minuit — mitigation du risque de double quota (`tickDailyRollover`, chemin horloge valide **et** chemin de repli `millis()` 24 h) | `armStabilizationTimer()` (les 2 pompes) | 5 min pH / 3 min ORP |

> ⚠️ **Changement v2.34.0 (feature-091)** — deux sites d'arming ont été **RETIRÉS** : le **démarrage de la filtration** en mode `managed` ([`filtration.cpp`](../../src/filtration.cpp)) et le **boot** en mode `powered` ([`main.cpp`](../../src/main.cpp)). Le sas de reprise d'eau est désormais porté **entièrement** par [`measure_validity`](measure-validity.md) : deux temporisations concurrentes sur le même événement n'avaient plus de sens, et celle-ci était aveugle à l'état réel de l'eau. **v2.62.0 (feature-139)** : ce sas n'est plus une purge adaptative sur stabilité des mesures — c'est un plancher fixe de 10 min, sauté si l'arrêt d'eau a duré moins de 3 min ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'[ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)).
>
> Corollaire : la surcharge `armStabilizationTimer()` (sans index) n'utilise **plus** `mqttCfg.stabilizationDelayMin` — retiré comme réglage utilisateur ([API.md](../API.md#get-get-config--write)) — mais les **mêmes constantes post-calibration** que la version indexée. Son seul appelant restant est le passage de minuit.

- **Arrêt filtration en mode `managed`** : `clearStabilizationTimer()` (efface les 2 pompes) — **inchangé**.
- Le champ NVS `stab_delay` et le champ `mqttCfg.stabilizationDelayMin` subsistent mais sont **inertes** : plus aucun chemin de dosage ne les lit (précédent `screen_enabled` de la feature-065).
- Les champs WS `ph_stab_remaining_s` / `orp_stab_remaining_s` sont **conservés à l'identique** : ils décrivent ce sas post-calibration, à ne pas confondre avec `measures_stab_elapsed_s` (purge de la chambre de mesure).

## Bascule de régulation en Manuel pendant une calibration (bug-160, volet 2)

⚠️ **REFONTE du 2026-10-13 (décision utilisateur)** : un premier mécanisme (bail de suspension
auto-expirant, `cal_hold_logic`, `POST /calibration/hold`) a été conçu, revu par deux gates
`pool-chemistry` supplémentaires, puis **abandonné avant tout déploiement** — remplacé par celui
décrit ci-dessous. Voir [ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)
pour l'arbitrage entre les deux approches, et [ADR-0106](../adr/0106-toute-suspension-demandee-par-un-client-est-bornee.md)
(statut Superseded) pour le mécanisme abandonné.

**Défaut corrigé** : pendant qu'une sonde pH ou ORP trempe dans un bécher d'étalonnage, rien
n'empêchait l'appareil de continuer à réguler sur ce qu'elle lisait — un étalon ORP à 470 mV
contre une consigne de 530 mV pouvait injecter du chlore dans le **bassin** sur la foi de la
lecture d'un **bécher**.

**Mécanisme retenu** : à l'entrée dans l'écran de calibration, l'appareil bascule lui-même la
régulation **pH ET ORP en `manual`**, interrompt tout dosage en cours, et attend un **geste
humain** (bouton « Fermer ») pour restaurer le mode antérieur. « On échange un oubli qu'on VOIT
(le mode Manuel affiché au tableau de bord) contre une panne qu'on ne voit PAS (une minuterie
coincée, invisible) » (gate 5). Logique pure dans
[`src/cal_manual_switch_logic.h`](../../src/cal_manual_switch_logic.h) /
[`.cpp`](../../src/cal_manual_switch_logic.cpp) ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)),
coquille dans [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp)
(`POST /calibration/enter` / `POST /calibration/exit`) et dans
`PumpControllerClass::tickCalSwitchAlert()` (alerte d'oubli, ci-dessous).

### `POST /calibration/enter` — à l'entrée dans l'écran

Appelée par `onEnter` de l'écran de calibration (`data/app.js`, `_calSwitchEnter`). Sous
`configMutex` :

1. **Décision par voie, indépendante pH/ORP**, déléguée à la fonction pure
   `calSwitchShouldApplyEnter(safetyTraceActive, alreadyManual)`
   ([`src/cal_manual_switch_logic.h`](../../src/cal_manual_switch_logic.h) /
   [`.cpp`](../../src/cal_manual_switch_logic.cpp)) : `true` seulement si **ni** un verrou de
   sécurité **ni** le mode courant ne vaut déjà `manual`.
   - **Exception de sécurité (gate 5, C3)**, pH uniquement : si `phAutoManualEpoch`/
     `phAutoManualReason` (feature-067, escalade de plancher acide) est **déjà** active, le pH
     n'est **pas** touché — il est déjà verrouillé en Manuel pour une raison plus grave, et cette
     bascule ne mémorise jamais ce `manual`-là comme un mode à restaurer (l'effacer serait un
     nouveau trou de sécurité). Aucun verrou équivalent n'existe côté ORP (vérifié : zéro résultat
     sur `grep -r "orp_am_\|orpAutoManual" src/`).
   - ⚠️ **Mode courant déjà `manual`, hors tout verrou** (correctif 2026-10-13, v2.76.2,
     `bug-160` suite) : aucune bascule non plus, **par voie**. Avant ce correctif, seule
     l'exception de sécurité ci-dessus était vérifiée — une boucle en Manuel **par choix de
     l'utilisateur** (ni verrou pH, ni équivalent ORP, qui n'en a jamais eu) était tout de même
     déclarée « basculée » : bandeau « a été mise en Manuel », bouton « rétablir la régulation »,
     et alerte d'oubli armée, alors que **rien n'avait changé**. Ce cas était pourtant **spécifié
     par le gate 5** (« entrée alors que l'utilisateur était déjà en `manual` → aucune bascule,
     bouton "Fermer" simple ») — non vérifié par personne (ni développeur, ni revue, ni doc) avant
     qu'un utilisateur dont les deux boucles étaient en Manuel ne le signale. **Leçon** : un cas de
     test écrit noir sur blanc dans un verdict de gate n'est pas un cas de test exécuté — s'assurer
     qu'il a un test natif dédié (voir `test/test_native_cal_manual_switch/`) et pas seulement une
     ligne de spec.
2. Si **applicable**, mémorise le mode **antérieur** de la boucle (`phCalSwitchPrevMode` /
   `orpCalSwitchPrevMode`) et bascule `phRegulationMode` / `orpRegulationMode` → `"manual"`
   (`phEnabled`/`orpEnabled` → `false`, miroir habituel du mode).
3. **Chloration de secours** (feature-140) : si l'ORP bascule effectivement et qu'elle est active,
   elle est désarmée (même patron que `/save-config`) — sinon un double dosage serait possible
   (étalonnage **et** secours).
4. **Idempotente** : un second appel pendant une session déjà ouverte ne réécrit **pas** le mode
   antérieur mémorisé (sinon un second appel capturerait `"manual"` lui-même comme mode à
   restaurer).
5. ⚠️ **Armement de la session** (`calSwitchActive`), correctif 2026-10-13 : seulement si **au
   moins une** des deux voies a réellement basculé (`applyPh || applyOrp`). Si **les deux**
   étaient déjà en `manual`, aucune session ne s'ouvre — pas de bandeau, pas d'alerte d'oubli pour
   un état que personne n'a changé. Journal : « pH et ORP déjà en Manuel, aucune bascule ».

Hors `configMutex` (même patron que `/ph|orp/inject/stop`) : **interrompt immédiatement** toute
injection **manuelle** en cours sur les deux pompes (⚠️ gate 5, C6 — « le seul trou que le bail
couvrait et que la bascule ne couvre pas » : changer de mode n'arrête pas une injection **déjà
lancée** — voir [Gardes des injections manuelles](#gardes-des-injections-manuelles-feature-006) pour ce
que cette interruption remplace). Demande aussi l'interruption **one-shot** d'une session
automatique/programmée déjà ouverte, consommée au tour suivant de `update()`
(`_calEntryInterruptRequested` / `requestCalibrationEntryInterrupt()`).

Réponse : `{ "success": true, "ph_switched": bool, "orp_switched": bool }` — `orp_switched` n'est
**plus** inconditionnellement `true` depuis le correctif 2026-10-13.

### `POST /calibration/exit` — à la fermeture de l'écran

Appelée par `onExit` (succès **ou** non — toujours) et par le bouton « Fermer et rétablir la
régulation » (libellé dynamique : simplement « Fermer » si `enter` n'avait rien basculé). Sous
`configMutex` :

1. Restaure le mode **antérieur** de chaque boucle — ⛔ **jamais** `"automatic"` en dur : un mode
   `"scheduled"` redevient `"scheduled"` (gate 5, C5).
2. ⚠️ **N'acquitte jamais** `phAutoManualEpoch`/`phAutoManualReason` (gate 5, C2 — **piège
   central** de la refonte, cf. ci-dessous) : si cette trace de sécurité est active — qu'elle
   l'ait déjà été à l'entrée, ou qu'elle soit apparue **pendant** la session de calibration — le
   pH reste en Manuel et seul l'ORP est restauré.
3. **Idempotente** : appelée sans session ouverte (double clic, session jamais ouverte), répond
   200 sans rien muter (`restored: false`).

Réponse (session ouverte) : `{ "success": true, "restored": true, "ph_mode": "...", "orp_mode": "..." }`.

### ⚠️ C2 — le piège que la refonte a failli créer

`web_routes_config.cpp:518-566` est un **gate fail-closed** posé après l'incident du 2026-08-25 :
tant qu'une trace `phAutoManual*` existe, sortir de `manual` **exige** `ph_auto_manual_ack: true`.
Réutiliser `phAutoManual*` pour porter la bascule de calibration aurait donc signifié : (a) le
bouton « rétablir » prendrait un **409** ; ou (b) s'il envoyait l'acquittement, il **effacerait
une escalade de plancher acide RÉELLE** (feature-067) — un nouveau trou de sécurité, créé par la
simplification elle-même. ⇒ Trace d'étalonnage **distincte** (`calSwitchActive`,
`phCalSwitchApplied`/`PrevMode`, `orpCalSwitchApplied`/`PrevMode` — `src/config.h`) ; le chemin de
fermeture n'acquitte **jamais** `phAutoManual*`.

### Persistance et traces (`src/config.h`)

| Champ | Rôle |
|---|---|
| `calSwitchActive` | une session de bascule est **ouverte** (pas encore restaurée). **Persisté** : survit à un redémarrage survenu pendant une calibration — le bandeau et l'alerte d'oubli restent vrais après reboot. |
| `calSwitchEpoch` | epoch UNIX de l'**entrée** (`0` = heure inconnue, patron `phAutoManualEpoch` — jamais un 1970 trompeur). Sert aussi de garde fail-safe pour l'alerte d'oubli. |
| `phCalSwitchApplied` / `phCalSwitchPrevMode` | pH a-t-il été réellement basculé par **cette** session ? Si oui, mode à restaurer à la fermeture. |
| `orpCalSwitchApplied` / `orpCalSwitchPrevMode` | miroirs ORP — appliqué sauf si l'ORP était **déjà** en `manual` à l'entrée (correctif 2026-10-13, v2.76.2 ; aucun verrou de sécurité équivalent côté ORP, mais le mode courant compte désormais). |
| `calSwitchLastAlertEpoch` / `calSwitchAlertCount` | échéance de l'alerte d'oubli (ci-dessous), remises à zéro à chaque **nouvelle** session. |

### Alerte d'oubli (gate 5, C4 — BLOQUANTE)

« On fabrique **délibérément** des bascules, il faut donc fermer le risque d'oubli. »
`PumpControllerClass::tickCalSwitchAlert()` (appelée à chaque tour d'`update()`, patron
`tickPhUndersizedAlert`) émet un premier avertissement `kCalSwitchAlertFirstDelayS` (2 h) après
l'**entrée**, puis répété toutes les `kCalSwitchAlertRepeatS` (6 h) tant que `calSwitchActive`
reste vrai — décision **pure** déléguée à `calSwitchAlertDue()`
(`src/cal_manual_switch_logic.h`), ce tick n'en est que la coquille temporelle.

Fail-safe epoch (patron `phAutoManualEpoch`) : **aucune** alerte tant que l'heure n'était pas
synchronisée à l'entrée (`calSwitchEpoch == 0`) ou ne l'est pas maintenant, et aucune si
l'horloge a reculé depuis (NTP) — une échéance calculée sur une horloge non fiable doit produire
un faux **silence**, jamais un faux positif.

Texte **imposé** (gate 5), `warning` + MQTT `publishAlert("cal_switch_overdue", …)` :

```
Régulation pH et chlore en Manuel depuis <N>h<MM> (étalonnage du <AAAA-MM-JJ HH:MM>) — aucun dosage automatique. Rétablissez-la.
```

### Contrat `/data` et WebSocket

Deux champs, publiés **à l'identique** sur `GET /data` et `sensor_data` WS (parité garantie
structurellement par un accesseur unique, pas par grep) :

| Champ | Type | Sémantique |
|---|---|---|
| `cal_switch_active` | booléen | une session de bascule est ouverte |
| `cal_switch_epoch` | entier (epoch), `0` si inactive ou heure inconnue | instant de l'entrée qui a ouvert la session courante |

Voir [API.md](../API.md#post-calibrationenter--write) et
[ws-manager.md](ws-manager.md#champs-sensor_data--bascule-de-calibration-bug-160). La régulation
`manual` elle-même est **déjà** visible via `phRegulationMode`/`orpRegulationMode` — ces deux
champs ne font que dater et qualifier la bascule, pas dupliquer le mode.

### Pourquoi aucune garde dédiée dans `evaluateDose()` / `evaluateManualInject()`

Contrairement au mécanisme de bail abandonné (qui ajoutait un rang de garde dans les deux
fonctions pures), cette bascule **ne touche aucune des deux** :

- **Dosage automatique/programmé** : fermé **nativement** par `ModeNotAutomatic` — la régulation
  est en `manual`, la conjonction `phMode == "automatic"` échoue comme pour n'importe quelle
  bascule manuelle de l'utilisateur. Aucun code de refus dédié n'est nécessaire.
- **Dosage manuel** : `evaluateManualInject()` reste **volontairement aveugle au mode** (comme à
  la mesure) — rien n'empêche une **nouvelle** demande manuelle pendant une calibration. La
  protection repose sur l'interruption **immédiate** d'une injection déjà lancée, à l'entrée
  (C6, ci-dessus) — pas sur un refus de la demande suivante : l'opérateur qui déclenche une
  injection manuelle pendant qu'il calibre assume cette décision, au même titre que toute
  injection manuelle.

Le code `DoseRefusal::CalibrationHold` (23) envisagé pour le mécanisme abandonné n'a **jamais été
livré** — voir la table des causes de refus plus haut.

## Cycle de régulation : chronologie stabilisation + pause mélange

Les deux temporisations ci-dessus jouent à des moments **différents** et sont deux gates **indépendantes** (`OR`) de `canDose()`. Vue d'ensemble pour la régulation `automatic` (exemple ORP, mode d'installation `managed`) :

```
T0            Filtration démarre        → attente des mesures (measure_validity : plancher 10 min
                                           fixe, sauté si l'arrêt d'eau a duré < 3 min)              [dosage bloqué]
T0 + ~10 min  Porte de validité ouverte → si |erreur| > seuil de démarrage (orpStartThreshold, 15 mV),
              (ou plus tard)               une injection démarre
  injection   dure ≥ minInjectionTimeMs (30 s) et continue tant que |erreur| > seuil d'arrêt (2 mV) ;
              s'arrête quand la cible est ~atteinte (ou budget horaire épuisé)
  à l'arrêt   → pause mélange armée (kOrpMixingDelayMs = 20 min ; pH = 15 min)                        [dosage bloqué]
T + 20 min    Pause mélange terminée    → nouvelle mesure → réinjection si toujours hors cible
              → boucle : [injection] → [pause mélange 20 min] → [mesure] → [injection] → …
```

Points clés :
- **Attente des mesures** (v2.34.0, remaniée v2.62.0) = sas de reprise d'eau, armé à **chaque** front montant de circulation. Depuis la v2.62.0, c'est un plancher **fixe** de 10 min (plus de verdict de stabilité) : il peut être réduit à 0 si l'arrêt d'eau qui précède a duré moins de 3 min (`kWaterStopGraceS`). Voir [measure-validity.md](measure-validity.md).
- **Stabilisation** = sas **ponctuel** des pompes, rejoué uniquement sur un événement d'armement — depuis la v2.34.0 : **calibration EZO** ou **passage de minuit** seulement. Ne se répète pas tant que la filtration tourne.
- **Pause mélange** = sas **récurrent**, armé à l'arrêt de **chaque** injection ; il rythme la régulation.
- **Cadence résultante** : la régulation ORP est plafonnée à **~1 injection / 20 min** (pH : ~1 / 15 min). C'est délibéré — la réponse de la sonde ORP et l'homogénéisation du chlore sont lentes ; réinjecter avant serait un surdosage.
- Les deux peuvent coïncider (ex. filtration qui redémarre juste après une dose) : `canDose()` attend que **les deux** soient levées.
- **Observabilité (v2.19.1)** : le temps restant est poussé au WS — stabilisation `ph/orp_stab_remaining_s`, pause mélange `ph/orp_mix_remaining_s` (getter `getMixingRemainingS(pumpIndex)`) — et affiché sur le widget du tableau de bord (« Stabilisation : … » / « Pause mélange : … »).

## Dosage en proportionnel de temps (itération 2, v2.21.0)

> **[ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)** — le modèle linéaire duty↔débit (`dutyToFlow`/`flowToDuty`, `kPumpMinFlowMlPerMin`/`kPumpMaxFlowMlPerMin`) est **SUPPRIMÉ**. Mesure au banc du 2026-08-23 : à duty 82 il créditait 6,4 mL/min pour **12,0 mesurés** (×1,9), à 255 il supposait 90 mL/min pour **61,0 mesurés** (×1,5). C'était la cause du **dosage fantôme** (volume compté sur de l'acide qui ne sortait pas — 479 mL fictifs en 75 min).

Le débit **moyen** demandé par la régulation est désormais réalisé par la **fraction du temps de marche** de la pompe, à une **amplitude unique dont le débit a été pesé**. Logique pure dans [`src/pulse_dosing_logic.{h,cpp}`](../../src/pulse_dosing_logic.h) ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md), testée en natif) ; `pump_controller.cpp` est la coquille (état `_pulseState`/`_pulseMs`/`_pulseSession` par chimie).

### Machine à états (par chimie, [0] = pH, [1] = ORP)

```
demande PID (mL/min moyen)
  │
  ├─ evaluatePulseGate(ampDuty, calibratedDuty, ampFlow, MIN_ACTIVE_DUTY)
  │    ≠ None → REFUS (critical throttlé 30 min, badge UI) — fin de session
  │
  ├─ planPulseWithCarry(demande, ampFlow, flow255, 60 s, 10 s, 400 ms, carry)
  │    — à l'entrée de séance et à chaque frontière de période (v2.22.0)
  │    demande ≥ plancher → pouls proportionnel (planPulse INCHANGÉ, bit-identique),
  │      accumulateur abandonné (F2)
  │    demande vivante SOUS le plancher (ex-seuil C) → ACCUMULATION : pouls = 0,
  │      séance MAINTENUE, cumul += demande × période ; cumul ≥ V_pouls (volume
  │      réel kick-inclus) → UNE impulsion de 10 s, résidu conservé (F3)
  │    refused (entrées invalides) → fail-closed, accumulateur remis à zéro (F1)
  │
  └─ tickPulse(...) chaque tour :
       pouls : [ kick 400 ms à duty 255 ][ plateau à l'amplitude ]……… repos ………
       période fixe 60 s, réancrée à chaque frontière (dérive ≤ un tour de loop)
```

- **Amplitude** = `pumpN_max_duty_pct × 255 / 100` (getter `getPumpAmplitudeDuty(pumpIndex)`). Le réglage garde sa sémantique PWM, borné **[40..100] %** côté serveur (`kPumpAmpMinPct = 40` ⇒ 102 counts ≥ `MIN_ACTIVE_DUTY = 98`). **v2.23.0 ([ADR-0047](../adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)) : amplitude GELÉE à 50 % (`kPumpAmpDefaultPct`)** — curseur UI retiré (ligne lecture seule à la place), le frontend n'envoie plus `pumpN_max_duty_pct` ; le champ API + validation [40..100] restent vivants pour cas exceptionnel.
- **Kick** : chaque pouls démarre par `kPulseKickMs = 400 ms` à duty 255 (couple de décollage à froid — symptôme terrain : rotation entretenue OK à 82, non-démarrage au même duty), puis retombe à l'amplitude.
- **Condition B3** : période **fixe** `kPulsePeriodMs = 60 s`, pouls minimal `kPulseMinMs` = **10 s depuis la v2.25.3** (était 5 s — [ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md), voir [Pouls minimal porté à 10 s](#pouls-minimal-porté-à-10-s-v2253)).
- **Condition B2** : une amplitude sous `MIN_ACTIVE_DUTY = 98` (Dmin mesuré 82 + 16 counts de marge P2) → **refus + critical**, jamais un clamp silencieux.

### Gate de caractérisation (fail-closed, P4)

`evaluatePulseGate()` refuse tout dosage automatique **et** programmé si :

| Cas | `PulseRefusal` | Log |
|---|---|---|
| `pumpN_calibrated_duty == 0` (**v2.23.0** : n'est plus le défaut usine — uniquement **effacement explicite** via API ou **clé NVS héritée présente à 0**, cf. [calibration de type](#calibration-de-type-par-défaut-feature-074-v2230)) | `FlowUncharacterized` | `CRITICAL` throttlé 30 min |
| Débit pesé implausible (NaN, ≤ 0, > `kPulseFlowMaxMlPerMin = 200`) | `FlowUncharacterized` | idem |
| **Amplitude ≠ duty calibré** (le débit n'est mesuré qu'à l'amplitude pesée) | `FlowUncharacterized` | idem |
| Amplitude < `MIN_ACTIVE_DUTY` | `AmplitudeBelowMinActive` | `CRITICAL` throttlé 30 min (B2) |
| Demande < débit_amplitude × 10/60 (pouls < `kPulseMinMs` = 10 s ; était 5/60 avant la v2.25.3) | **plus un refus depuis la v2.22.0** — demande **accumulée** ([accumulateur sous-plancher](#accumulateur-sous-le-pouls-minimal-feature-073-v2220), [ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md)) | `INFO` au premier tir d'accumulateur de la séance |

**AUCUNE interpolation nulle part** : les deux seuls débits connus sont la mesure à l'amplitude (`pumpN_pulse_flow_ml_per_min`, propre à CETTE pompe) et le **majorant de flotte à 255** (`kPumpFlow255MlPerMin = 74,0` depuis la v2.25.2, [`constants.h`](../../src/constants.h) — voir [Débit à 255 : constante unique de flotte](#débit-à-255--constante-unique-de-flotte-v2252)). L'état est exposé à l'UI/WS : `isPumpFlowUncharacterized(pumpIndex)` (+ raccourcis par chimie `isPhPumpFlowUncharacterized()` / `isOrpPumpFlowUncharacterized()`) → badge « Débit non caractérisé — pesée requise » et champs `/data` `ph/orp_pump_flow_uncharacterized`, `ph/orp_pump_amplitude_duty`.

**Persistance NVS** : `p1_cal_duty` / `p1_pulse_flow` / `p2_cal_duty` / `p2_pulse_flow` + drapeaux `p1_flow_weighed` / `p2_flow_weighed` (v2.23.0). L'ancienne clé partagée `pump_max_flow` est **supprimée SANS conversion** (purgée au premier `saveMqttConfig()`) : c'est le modèle faux qu'on retire. Une valeur NVS implausible est neutralisée à 0 (non caractérisé) au chargement, avec `warning` — **et rabat aussi `weighed = false`** (une pesée corrompue n'en est plus une, condition C4).

### Calibration de type par défaut (feature-074, v2.23.0)

> **[ADR-0047](../adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)** — amende le « pesée obligatoire, pas de valeur provisoire » de l'ADR-0043 (contexte flotte : appareils fournis à des tiers, même modèle de pompe partout — aucun tiers ne pèsera jamais). Gate `pool-chemistry` : GO amont sous conditions C1-C5 + GO en contre-visite.

Les **défauts** de la struct `MqttConfig` passent de « non caractérisé » (0, fail-closed) à la **calibration de type** (valeur usine du modèle de pompe) :

| Constante | Valeur | Rôle |
|---|---|---|
| `kPumpAmpDefaultPct` ([`config.h`](../../src/config.h)) | 50 % | Amplitude d'impulsion par défaut (gelée — UI retirée, champ API vivant) |
| `kPumpTypeCalDuty` ([`config.h`](../../src/config.h)) | **127** | Duty de type = `(kPumpAmpDefaultPct × MAX_PWM_DUTY) / 100` — **même expression entière** que `getPumpAmplitudeDuty` (condition C2 : l'égalité exacte amplitude == duty calibré du gate est garantie **par construction**). Verrous `static_assert` : `== 127` et `≥ MIN_ACTIVE_DUTY`. |
| `kPumpTypePulseFlowMlPerMin` ([`constants.h`](../../src/constants.h)) | **28,0 mL/min** | Débit de type au duty 127. **v2.25.2 : justification refondée sur une MESURE** ([ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)) — mesures réelles du 2026-08-28 (assistant feature-075, 120 s à duty 127) : **26,0 mL/min pompe 1, 25,5 pompe 2** ; la valeur d'usine vaut **max mesuré 26,0 + 8 % de dispersion**, elle n'est plus adossée aux interpolations 24,7 / ~27,2 (bonnes à 4-7 % près). **Biais SÛR** : surestimer le débit ⇒ pompe ON moins longtemps (sous-dosage) + compteurs journaliers majorants. |
| `kPumpTypeFlowMeasuredMaxMlPerMin` ([`constants.h`](../../src/constants.h)) | **26,0 mL/min** | v2.25.2 — maximum **réellement mesuré** à duty 127. N'intervient dans **aucun calcul** : porte le `static_assert` qui interdit de descendre la valeur d'usine sous le max mesuré. Ne change que sur une nouvelle mesure d'exemplaire. |

**Sémantique NVS (condition C1)** — clé **absente** (appareil vierge / jamais sauvegardé) → défaut de type, le dosage fonctionne au premier flash ; clé **présente à 0** (effacement explicite, ou appareil ≤ 2.22 jamais calibré ayant sauvegardé) → **0 lu tel quel, fail-closed conservé** — jamais de conversion silencieuse. Un appareil provisionné garde ses valeurs NVS intactes.

**Drapeaux `pumpN_flow_weighed`** (`false` = valeur usine de type, `true` = pesée individuelle de CETTE pompe) : **purement informatifs** — alimentent l'indication « valeur usine / pesé » de l'UI, **aucun gate ne s'y fie** (condition C5). Posés par `/save-config` à toute pesée (débit seul → duty déduit de l'amplitude courante ; duty explicite d'un vieux client → aussi, résidu assumé) ; rabattus à `false` par l'effacement (`duty=0`) et par la garde de plausibilité NVS.

### Débit à 255 : constante unique de flotte (v2.25.2)

> **[ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)** — amende les **valeurs d'usine** de l'ADR-0047 (le reste de l'ADR-0047 est inchangé).

`kPump1Flow255MlPerMin = 61,0` et `kPump2Flow255MlPerMin = 67,0` sont **supprimées** et remplacées par **`kPumpFlow255MlPerMin = 74,0`**. Les helpers `pumpFlow255()` / `flowAssistFlow255()`, qui **dispatchaient par index physique de pompe**, disparaissent avec elles (11 sites d'appel ramenés à la constante).

**Le défaut corrigé.** Ces deux constantes n'étaient **pas configurables**, provenaient d'**un seul exemplaire chacune**, et servaient de **majorant de sécurité** dans trois usages :

| Usage | Fichier |
|---|---|
| Crédit du **kick** de chaque impulsion (400 ms à duty 255) | `pulseDeltaVolumeMl()` — [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| **Majorant du mode manuel** `?duty=N` et des marches d'assistant (P4 : jamais créditer zéro sur une pompe alimentée) | [`web_routes_control.cpp`](../../src/web_routes_control.cpp) |
| Durée d'une **injection volumée sur le chemin dégradé** (amplitude ≠ duty calibré → duty forcé à 255) | `planManualInject()` |

La constante de la pompe 1 était calée sur l'exemplaire **le plus lent** du lot : un appareil de flotte dont la pompe pH aurait ressemblé à l'exemplaire « 67 » se serait **sous-crédité de 11 %** — il aurait injecté plus que ses limites ne le croyaient. Attribuer une mesure d'exemplaire à un **index physique** n'a de surcroît aucun sens sur une flotte.

**Pourquoi 74,0** : **max réellement mesuré 68,0 + 8 % de dispersion inter-unités** (`kPumpFlow255MeasuredMaxMlPerMin = 68,0`, verrou `static_assert`). La valeur 70,0 a été **refusée par le gate `pool-chemistry`** — +3 % de marge alors que l'écart **déjà constaté** entre deux exemplaires du même lot est de **11 %** : une marge inférieure à la dispersion observée est statistiquement nulle. Même raisonnement, le même jour, pour `kPumpTypePulseFlowMlPerMin` (26,0 proposé, refusé, 28,0 conservé).

**Pourquoi UNE seule valeur et non deux** (majorant pour créditer + estimateur central pour la durée) : les trois usages poussent **dans le même sens** — surestimer ne sur-injecte **jamais**. Un « estimateur central » destiné à la durée serait par construction une valeur choisie pour **allonger la marche de la pompe**, soit exactement le risque interdit. La précision de l'injection volumée dispose déjà d'un **chemin exact** (`ampDuty == calDuty` ⇒ débit mesuré) ; le chemin à 255 est un **repli dégradé** dont la précision ne s'achète pas contre une garde.

⚠️ **Effet visible : les compteurs journaliers se remplissent un peu plus vite.** Le crédit du kick passe de **0,407 à 0,494 mL par impulsion**. Sur une journée au pouls minimal (~45 impulsions/h × 12 h) : **+46,8 mL/j de sur-crédit sur la pompe pH (+3,6 %)** et **+25,2 mL sur la pompe chlore (+1,9 %)**, sur ~1 300 mL comptés. Effet **borné**, direction **fail-safe** : les limites journalières mordent plus tôt.
>
> **v2.25.3** : ces pourcentages sont établis avec un pouls minimal de **5 s**. Le pouls passant à **10 s** ([ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)), le nombre de kicks **par volume compté** est divisé par deux — le sur-crédit du kick l'est donc aussi (≈ +1,8 % pH / +0,9 % chlore).

**Côté client** : le miroir en dur `FLOW_255_ML_PER_MIN = {1: 61.0, 2: 67.0}` de [`data/app.js`](../../data/app.js) (devenu faux de ~21 %) est remplacé par la lecture de `flow_assist.flow255` servi par [`GET /data`](../API.md#get-data--write) — **et par la trame WS `sensor_data` depuis la v2.47.1** (`bug-082` : servi d'un seul côté, le champ était effacé par chaque push WS toutes les 5 s). Le **repli à 61 / 67** est conservé **délibérément** : un firmware antérieur à la v2.25.2 ne publie pas ce champ et dose bien à ces débits-là — et le garder **distinct de 74** est ce qui rend une disparition du champ détectable.

> **Dette repérée, non traitée** (consignée dans l'ADR-0050) : `phPumpControl` / `orpPumpControl` ([`config.cpp`](../../src/config.cpp)) sont **entièrement mortes** — déclarées et initialisées (leur second membre portait le débit à 255), lues **nulle part** dans `src/` ni `test/`. Elles ont été mises à jour mécaniquement pour ne pas laisser traîner une valeur périmée, mais elles doivent disparaître.

### Accumulateur sous le pouls minimal (feature-073, v2.22.0)

> **[ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md)** — amende le **seuil C** de l'ADR-0043. Le refus créait une **zone morte** (< débit_amplitude × poulsMin/60 ÷ kp) : stagnation terrain du pH à **~cible + 0,26** constatée le 23/08 (kp = 8). La demande sous le plancher est désormais **cumulée**, jamais refusée.
>
> ⚠️ Les valeurs chiffrées ci-dessous supposaient un pouls minimal de **5 s** ; elles sont recalculées pour le pouls de **10 s** de la v2.25.3 ([ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)). **La mécanique et la moyenne restituée sont inchangées** — seuls le plancher, la taille du paquet et la cadence des tirs doublent / se divisent par deux.

Logique pure dans `planPulseWithCarry` ([`src/pulse_dosing_logic.h`](../../src/pulse_dosing_logic.h)) :

```cpp
struct PulseCarryPlan {
  bool refused;       // true = entrées invalides, ne pas injecter (fail-closed)
  uint32_t pulseMs;   // durée du pouls (0 = accumulation en cours, pompe OFF)
  bool carryFired;    // true = pouls déclenché PAR l'accumulateur (= minPulseMs)
  float carryMl;      // accumulateur en sortie (recopié par la coquille)
};
PulseCarryPlan planPulseWithCarry(float meanFlowMlPerMin, float ampFlowMlPerMin,
                                  float flow255MlPerMin, uint32_t periodMs,
                                  uint32_t minPulseMs, uint32_t kickMs,
                                  float carryMl);
```

Sémantique par régime (une fois par frontière de période) :

- **Demande ≥ plancher** : délégation **stricte** à `planPulse` — comportement **bit-identique** (vérifié en natif sur 255 demandes) — et accumulateur **abandonné** (**F2** : fusionner reviendrait à compter deux fois la même erreur).
- **Demande vivante sous le plancher** (ex-seuil C) : `carryMl += demande × période / 60000` (mL), **séance MAINTENUE** (`phActive`/`orpActive` restent `true`, pompe OFF via `tickPulse` — l'UI affiche « Dosage en attente : Ns », calendrier ADR-0045 inchangé). Dès que le cumul **atteint** (égalité incluse) **V_pouls**, **UNE** impulsion de `kPulseMinMs` (**10 s** depuis la v2.25.3) exactement part (**F3**), le **résidu est conservé** — jamais deux pouls par période, invariant de sortie `carryMl < V_pouls`.
- **V_pouls = volume RÉEL kick-inclus** (décision gate n°1) : `pulseDeltaVolumeMl(0, kPulseKickMs, 0, kPulseMinMs, flow255, ampFlow)` — seuil ET décrément ; le nominal `ampFlow × kPulseMinMs` sous-estimerait de **+7-8 % au pouls de 10 s** (+10-12 % au pouls de 5 s — le kick de 400 ms pèse deux fois moins dans un pouls deux fois plus long), d'où une sur-livraison systématique du même ordre. Le `flow255` est le **majorant de flotte** `kPumpFlow255MlPerMin` (v2.25.2 — le dispatch par pompe `pumpFlow255(pumpIdx)` est supprimé).
- **Entrées invalides** (demande nulle/non finie, ampFlow invalide, période 0) ou `carryMl` d'entrée suspect (non fini, négatif, ≥ V_pouls) : refus / écrêtage **fail-closed** à zéro (**F1** : une demande disparue ne laisse pas de dette ; jamais de rafale de rattrapage).

Ordre de grandeur (**v2.25.3**, pouls 10 s, débit d'amplitude **mesuré** 25,75 mL/min, flow255 74 → **V_pouls ≈ 4,61 mL**) : une demande de 0,7 mL/min donne une impulsion de 10 s toutes les **6-7 périodes** ; une demande de 1,76 mL/min, une impulsion toutes les **~2,7 min** (contre ~1,4 min au pouls de 5 s). Volume restitué exact à **±5 %** (test natif `test_F073_scenario_long_*`). *Historique : V_pouls ≈ 2,47 mL au pouls de 5 s ; ≈ 2,32 mL avant la v2.25.2 (flow255 61).*

**Coquille** (`pump_controller.{h,cpp}`) : `_pulseCarryMl[2]` / `_carryPulseLogged[2]` par **chimie** ([0] = pH, [1] = ORP), écrits/lus en loopTask uniquement (patron `_stabilizationEndMs`, aucun mutex). L'ouverture de séance est faite **AVANT** le plan (la séance s'ouvre aussi en régime d'accumulation — décision gate n°2 : une **seule** séance, jamais une par pouls, sinon ~480 séances/j auto-bloquantes par `maxCyclesPerDay`).

**Resets exhaustifs** (jamais de dette suspecte) :

| Événement | Effet |
|---|---|
| Ouverture / reprise de séance | cumul = 0 (décision gate n°3 : aucune dette héritée) + flag de log réarmé |
| Fermeture de séance — **inconditionnelle à chaque tick sans `flow > 0`** : demande nulle, refus de gate / `canDose`, limite atteinte, suspension (OTA, filtration…) | cumul = 0 + flag de log réarmé |
| Reboot | **non persistant** — perte bornée < V_pouls (≈ 4,6 mL depuis la v2.25.3, ≈ 2,5 mL avant), voulu |
| Entrées invalides / cumul d'entrée ≥ V_pouls | reset défensif fail-closed dans la logique pure |

**Aucun passe-droit** : le tir d'accumulateur est un pouls **normal** — chemin standard `tickPulse`, `usedMs` pompe-ON (ADR-0044), volume crédité par `pulseDeltaVolumeMl`, toutes les gardes réévaluées à chaque tour en amont.

**Log (A3)** : `INFO [Dosage pH|ORP] Dosage par accumulation : impulsion minimale de 10 s (X mL cumulés, résidu Y mL)` — la durée affichée est dérivée de `kPulseMinMs` (`10 s` depuis la v2.25.3, `5 s` avant) — **un par séance** (premier tir, `_carryPulseLogged`), les suivants silencieux (le log d'arrêt de séance résume le volume). Remplace l'`INFO` « injection différée (seuil C) », **supprimé**. *Limite connue (revue)* : le « X mL cumulés » peut être légèrement faux si l'écrêtage défensif a joué — cosmétique, le volume **crédité** reste exact.

### Pouls minimal porté à 10 s (v2.25.3)

> **[ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)** — amende le **pouls minimal** (condition B3) de l'ADR-0043. Gate `pool-chemistry` : **GO SOUS CONDITIONS**. `kPulseMinMs` : **5 000 → 10 000 ms**. Rien d'autre ne change : ni `kPulsePeriodMs`, ni l'amplitude, ni le modèle de comptage.

**Constat de terrain (28/08/2026).** La régulation pH ne converge pas : **968 mL comptés en une journée**, pH toujours à **7,50** pour une consigne à **7,30**. Le besoin réel est de **800-900 mL pour ΔpH 0,2** sur 40 m³ — chiffre validé par deux voies indépendantes et concordantes (composition du produit : 12 % H₂SO₄ + 13 % HCl ≈ 6,9 éq/L ; dosage constructeur : 0,2 L/10 m³ pour 0,1-0,2 unité). Près d'un litre **compté** sans effet mesurable.

**Hypothèse retenue** : la **perte fixe par démarrage** de la pompe péristaltique (relaxation puis ré-occlusion du tube, reprise d'amorçage) vaut **0,1 à 0,5 mL** sur une pompe dont un tour délivre 1-2 mL, alors que `pulseDeltaVolumeMl` compte comme si le débit s'établissait **instantanément**. Facteur aggravant : `ph_pulse_ms = 5000` **en permanence** ⇒ l'erreur porte sur **100 % du volume**, pas sur une fraction.

**Ce que le changement fait — et ce qu'il ne fait pas** (point contre-intuitif) :

- il **n'augmente PAS la dose demandée**. `planPulseWithCarry` accumule `meanFlow × period` et retire **exactement** `V_pouls` au tir : la moyenne restituée est **indépendante** de `kPulseMinMs` (invariance vérifiée par le gate dans [`pulse_dosing_logic.cpp`](../../src/pulse_dosing_logic.cpp)). Les impulsions deviennent **deux fois plus grosses, deux fois moins fréquentes** ;
- il améliore le **rendement** : deux fois moins de démarrages pour un même volume, donc la perte fixe est amortie sur deux fois plus de volume utile — et le débit d'un pouls de 10 s est plus reproductible que celui d'un pouls de 5 s.

**Chiffres** (débit à l'amplitude **mesuré** 25,75 mL/min, `flow255` = 74,0) :

| Grandeur | Pouls 5 s | Pouls 10 s |
|---|---:|---:|
| `V_pouls` (volume réel kick-inclus d'une impulsion minimale) | 2,47 mL | **4,61 mL** |
| Seuil de bascule vers le régime proportionnel direct | 2,15 mL/min | **4,29 mL/min** |
| Cadence à demande 1,76 mL/min | ~1 / 1,4 min | **~1 / 2,7 min** |

Bolus unitaire 4,6 mL = **32 méq** ⇒ **ΔpH < 0,002** même sur 15 m³ ; négligeable jusqu'à ~5 m³ (réserve du gate : un spa < 2 m³ sortirait du raisonnement — hors périmètre flotte).

**Effets de bord instruits, tous inchangés** — `maxCyclesPerDay` et l'anti-rafale comptent par **séance**, pas par impulsion ; la **limite horaire** est réévaluée à chaque tick (dépassement borné par un tour de boucle, pas par 10 s) ; le **temps minimum d'injection** raisonne en temps pompe-ON cumulé ([ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)), donc la durée murale d'une séance ne bouge pas ; la **pause mélange** s'arme en fin de séance seulement.

**`kPulsePeriodMs` reste à 60 s** (décision explicite) : la fraction de temps pompe maximale est inchangée et le refus antérieur de l'utilisateur d'allonger la période (cycle feature-073) reste fondé.

⚠️ **DETTE ASSUMÉE — condition n°1 du gate : le compteur reste FAUX.** En divisant les démarrages par deux à volume **compté** constant, on augmente le volume **réel** délivré sous le même plafond journalier : la limite journalière devient légèrement **moins conservatrice** (quelques dizaines de mL/jour si la perte vaut ~0,5 mL). Cet allongement **atténue** le défaut de comptage, **il ne le corrige pas**. Exigences du gate : **surveiller le plafond journalier sur 3 jours**, et la **mesure de la perte fixe reste due** (M2 : 24 pouls de 5 s contre 12 pouls de 10 s à temps de pompe égal ⇒ `V₁₀ − V₅ = 12 × L`), après quoi le modèle de comptage devra être corrigé — et traité **explicitement comme une hausse de dose**. Voir [feature-078](../../specs/features/done/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md).

### Biais du compteur de volume — MESURÉ le 06/09/2026 : +4,76 %, conservateur

> **La dette annoncée ci-dessus est désormais chiffrée.** Ce n'est plus une
> hypothèse : c'est une mesure sur baseline contrôlée. Sa **nature** reste
> ouverte.

Première concordance bidon/compteur exploitable (baseline posée le 31/08 : bidon
chlore mesuré 8 750 mL, compteur remis à 0 au même instant), relevée au titre du
critère P12 de [feature-075](../../specs/features/done/feature-075-assistant-mesure-debit-pompe.md).

| Grandeur | Valeur |
|---|---:|
| Compteur appareil (`orp_total_injected_ml`) | **5 290,44 mL** |
| Restant **estimé** (8 750 − 5 290,44) | 3 459,56 mL |
| Restant **mesuré** | **3 700 mL** |
| Injecté **réel** | **5 050 mL** |
| **Biais** | **+240,44 mL = +4,76 %** — le compteur **sur-compte** |

**Méthode** : récipient doseur de 3 L rempli à ras + complément de 0,7 L,
incertitude ~±30-50 mL ⇒ biais réel entre **+3,8 % et +5,8 %**, donc
**distinguable de zéro**. **Fenêtre** : 6 jours, 5 050 mL injectés, amplitude
`duty = 127`, pouls minimal 10 s.

**Sens conservateur — aucune conséquence de sécurité chimique.** Sur-compter
**resserre** les limites journalières et **avance** l'alerte de bidon vide. C'est
exactement le sens dans lequel les gardes doivent se tromper
([propriété fail-safe](#comptabilité-du-volume-p4)). Rien n'appelle de correctif
en urgence.

**⚠️ +4,76 % est un PLANCHER, pas une valeur.** Une purge de l'assistant de mesure
expulse du liquide **sans le compter** : elle **masque** une partie du
sur-comptage. `measures_purge_count` valait 0 au relevé, mais l'appareil avait
redémarré 20 min plus tôt — ce compteur ne couvre pas la fenêtre.

**Ordre de grandeur par impulsion** ([`pulse_dosing_logic.cpp`](../../src/pulse_dosing_logic.cpp),
`pulseDeltaVolumeMl`) : un pouls minimal est compté **4,573 mL** (kick 0,493 +
plateau 4,08) ⇒ ~**1 160 impulsions** sur la fenêtre ⇒ **0,21 mL de surcoût par
impulsion**, soit ~**0,5 s de débit perdu au démarrage** — l'ordre de grandeur
d'un transitoire péristaltique, cohérent avec l'hypothèse de perte fixe.

**~19 % de ce surcoût est VOLONTAIRE et ne doit jamais être « corrigé ».** Le kick
est crédité au majorant de flotte `kPumpFlow255MlPerMin = 74,0` contre **68,0
réellement mesurés** ([ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md),
[`constants.h`](../../src/constants.h)) : c'est une marge dimensionnée sur une
dispersion inter-unités de 11 % constatée dans un même lot. La retirer serait
supprimer une sécurité en croyant gagner en précision.

**⛔ NO GO `pool-chemistry` du 06/09 sur tout facteur de comptabilité de flotte.**
Avec le **volume seul**, « biais multiplicatif » et « perte fixe par impulsion »
sont **indiscernables** — une inconnue, une équation. Un facteur multiplicatif
calé sur ce point unique corrigerait **mal** une perte par impulsion : la fraction
de biais varie avec la durée moyenne de pouls, donc avec la demande, et sur pouls
longs il **sur-corrigerait et relâcherait les quotas**.

**Réserve sur l'ampleur** : le passage de `kPulseMinMs` à 10 s
([ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md)) a
**divisé par deux** le nombre d'impulsions — le +4,76 % est donc mesuré **après**
atténuation, et ce même allongement a réduit d'autant le coussin de sécurité
involontaire que le sur-comptage constituait.

**Suites** : [feature-110](../../specs/features/done/feature-110-compteur-d-impulsions-de-dosage.md)
(compteur de **démarrages** par pompe — deux inconnues, deux équations ; **préalable**,
sans quoi chaque concordance future sera aussi peu concluante) — **livré en v2.45.0**,
voir [Comptage des démarrages de pompe](#comptage-des-démarrages-de-pompe-feature-110-v2450) — puis
[feature-078](../../specs/features/done/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md)
(nature établie, puis correction du modèle — **à traiter explicitement comme une
hausse de dose**). Le protocole de banc qui trancherait sans attendre est écrit
dans [`constants.h`](../../src/constants.h) : **24 pouls de 5 s contre 12 de 10 s**,
à temps de pompe égal — volumes égaux ⇒ multiplicatif, premier plus faible ⇒
perte fixe.

### Comptabilité du volume (P4)

`accountPumpVolume(chem, physIdx, now, manualActive)` crédite par **delta de tick** (attribution arrière — le volume du delta est celui de la commande posée au tour précédent) :

- **Pouls auto/scheduled** : `pulseDeltaVolumeMl()` — la part du delta recouvrant le kick est créditée au **majorant de flotte à 255** (`kPumpFlow255MlPerMin`), le reste au **débit d'amplitude mesuré**. Exact au jitter de loop près (seule la géométrie du pouls compte). *Limite connue* : la transition kick→plateau est vue au premier tick après 400 ms → sous-comptage ~0,1 mL/pouls, borné.
- **Manuel `?duty=N`** : crédit au **MAJORANT** débit-255 (surestimer est fail-safe), **sauf** `duty == calibrated_duty` → débit pesé exact.
- **Jamais zéro sur une pompe alimentée** (prescription P4).

Remplace `updateSafetyTracking(flow, delta)` par `creditInjectedVolume(isPhPump, volumeMl)` : le volume est calculé en amont, plus jamais dérivé d'un débit de modèle.

**v2.61.0 (bug-115)** : `accountPumpVolume()` n'a plus qu'un seul appelant, `settlePumpAccounting()` — point de passage unique invoqué à **chaque** sortie d'`update()`, anticipée ou non. Détail complet : [Correction de la rémanence de comptabilité](#correction-de-la-rémanence-bug-115-v2610--point-de-passage-unique) plus bas.

### Comptage des démarrages de pompe (feature-110, v2.45.0)

Six compteurs `ProductConfig` ([`config.h`](../../src/config.h)) mesurent le **dénominateur** du biais de comptage, par produit. **Aucune correction n'est appliquée : cette feature mesure.**

| Champ | Type | Rôle |
|---|---|---|
| `phStartCount` / `orpStartCount` | `uint32` | **`N`** — démarrages de pompe **crédités** depuis le dernier « Nouveau bidon » |
| `phAcctAnomalyCount` / `orpAcctAnomalyCount` | `uint32` | Ticks de comptabilité **anormalement longs** parmi ces crédits (hygiène) |
| `phAcctAnomalyMl` / `orpAcctAnomalyMl` | float (mL) | Volume crédité **par** ces ticks anormaux (hygiène) |

`uint32` et non `uint16` : à ~1 400 démarrages/jour observés, un `uint16` saturerait en un peu plus d'un mois — **avant** le premier changement de bidon qu'il doit qualifier. L'incrément passe par `saturatingInc()` ([`dosing_logic.cpp`](../../src/dosing_logic.cpp)) : à `UINT32_MAX` la valeur **reste figée** et ne reboucle jamais, un compteur figé étant un aveu lisible là où un compteur rebouclé donnerait un rapport `V/N` silencieusement faux.

#### Point d'incrémentation : le front OFF→ON, dans la branche qui crédite le volume

Dans `accountPumpVolume()` ([`pump_controller.cpp:2664`](../../src/pump_controller.cpp:2664)), la décision est prise par la fonction **pure** `evaluatePumpStartAccounting(prevOn, onNow, creditedDeltaMs, anomalyDeltaMs)` ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) :

- `countStart = !prevOn && onNow` — **front strict**, évalué **AVANT** l'affectation `_acctPumpOn[chem] = onNow`, donc sur l'état du tour **précédent**. Une session `scheduled` en marche continue ne produit **qu'un** démarrage ; un start/stop logé entre deux ticks n'en produit **aucun** — mais il ne crédite aucun volume non plus, la paire (`N`, `V`) reste cohérente.
- `countAnomaly = anomalyDeltaMs != 0 && creditedDeltaMs > anomalyDeltaMs` — frontière `>` **stricte** : un delta **pile au seuil** est **normal**.

Le crédit s'exécute sous la garde `productTrackingEnabled(chem == 0)`, **la même** que celle qui laisse avancer `productCfg.*TotalInjectedMl`. C'est ce **couplage structurel** qui garantit que `N` et `V` couvrent exactement le même ensemble d'événements — sans aucune garde ajoutée, et y compris pour l'**angle mort hérité** des sorties anticipées d'`update()` (voir plus bas). Ni allocation, ni `String`, ni log, ni verrou : écrivain unique `loopTask`, même discipline que le volume.

#### `kAcctDeltaAnomalyMs = 5000` — pourquoi ce seuil

Le volume d'un tick est crédité par **attribution arrière** sur `[lastSafetyTimestamp, now]`. Au-delà de 5 s, on ne sait plus si la pompe a tourné tout du long (loop bloquée, OTA, I²C en souffrance). Ces ticks ne sont **ni rejetés ni corrigés** — la comptabilité du volume est intouchée — ils sont comptés **à part, avec leur volume**, pour pouvoir **disqualifier** un relevé de bidon pollué.

Calage : `accountPumpVolume()` tourne à la cadence de `loop()` (`kLoopDelayMs` = 10 ms + le travail du tour, quelques dizaines de ms en pratique) — 5 s est **deux ordres de grandeur** au-dessus du jitter normal, et vaut la **moitié** du pouls minimal (`kPulseMinMs` = 10 s) : un tick qui couvre la moitié d'une impulsion entière est une anomalie, pas du bruit.

#### Le défaut CORRIGÉ que ces compteurs captaient (bug-115, v2.61.0)

Les sorties anticipées d'`update()` — eau absente, stabilisation post-calibration, assistant de mesure du débit (`flowAssistSuspend`), capteurs non initialisés, OTA, maintenance — coupaient les pompes puis **`return` AVANT** l'appel à `accountPumpVolume()`. L'état `_acctPumpOn[chem]` restait donc **rémanent** : à la reprise, le firmware créditait **toute la durée de la coupure** au débit plateau, tandis que le redémarrage physique **n'était pas compté**.

Ordre de grandeur mesuré : 5 min → ~5,7 mL, 1 h → ~68 mL, 6 h → ~408 mL, jusqu'à **950 mL** sur une coupure nocturne longue — soit **20 à 48 %** du plafond journalier vivant (`max_ph_ml_per_day` / `max_orp_ml_per_day` = 2 000 mL). Ce défaut n'était **pas introduit** par feature-110 — il était **révélé** par elle, et c'est exactement le cas que `*_anom` / `*_anom_ml` captaient (le tick de reprise dépassait les 5 000 ms de plusieurs ordres de grandeur).

**Corrigé en v2.61.0** ([bug-115](../../specs/features/done/bug-115-etat-de-comptabilite-remanent-aux-sorties-anticipees.md)) par le point de passage unique — voir [Correction de la rémanence de comptabilité](#correction-de-la-rémanence-bug-115-v2610--point-de-passage-unique) ci-dessous. **`kAcctDeltaAnomalyMs = 5000` et les compteurs `*_anom`/`*_anom_ml` sont conservés tels quels**, mais changent de **rôle** : d'un *détecteur d'un défaut connu* ils deviennent un **canari d'un chemin inconnu**. Après correctif, un `anom` non nul ne signale plus la rémanence de bug-115 mais un chemin de code qu'on n'avait pas prévu (blocage de `loop()`, gel I²C, future sortie anticipée non couverte).

#### Correction de la rémanence (bug-115, v2.61.0) : point de passage unique

`update()` est devenu un **wrapper de quatre lignes** autour de `updateDosingCore(now)` (l'ancienne `update()`, corps **non réindenté**, ses quatre `return` anticipés intacts) :

```cpp
void PumpControllerClass::update() {
  unsigned long now = millis();
  _acctSettledThisTick = false;
  updateDosingCore(now);
  if (!_acctSettledThisTick) settlePumpAccounting(now);
}
```

Chaque sortie anticipée appelle désormais elle-même `settlePumpAccounting()` — **seul appelant** d'`accountPumpVolume()` — juste après avoir forcé `applyPumpDuty(*, 0)`. Le wrapper est le **filet structurel** : il couvrirait aussi une sortie future qu'un chemin de code oublierait d'appeler — même motif qu'[ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) pour `fopen` : il n'y a plus rien à ne pas oublier.

⚠️ **Deux renversements par rapport à l'intuition initiale de la spec** :
1. **La spec disait « purger » l'état de comptabilité, le gate `pool-chemistry` l'a INTERDIT.** Un reset aveugle de `_acctPumpOn[]`/`lastSafetyTimestamp` **perdrait le volume réellement injecté** — deux sites le rendent inacceptable : `flowAssistSuspend` laisse tourner le **manuel** (c'est précisément la marche de mesure de l'assistant de débit) et OTA/maintenance coupent **en plein pouls**. Corriger un défaut qui surcompte en créant un défaut qui **sous-compte** aurait été pire : le sous-compte va dans la direction dangereuse (plus de produit réellement injecté sous le même plafond journalier). Le point unique **appelle le comptable**, il ne purge jamais.
2. **Il y a QUATRE sorties anticipées, pas cinq** : le gate P1 en citait cinq, imprécision interne à la spec — le triplet eau absente / stabilisation / `flowAssistSuspend` partage en réalité **un seul** `return`. Sans conséquence pratique : le wrapper les couvre toutes **par construction**, quel qu'en soit le nombre — c'est justement l'argument qui a fait retenir le point de passage unique plutôt que la purge branche par branche.

La propriété qui rend la rémanence **structurellement impossible** vit dans le module pur [`src/acct_window_logic.h`](../../src/acct_window_logic.h) (`evaluateAcctWindow()`, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) : `nextOn = onNow` **toujours**, que l'appel vienne de la queue nominale ou d'une sortie anticipée — une garde n'aurait pas suffi, c'est la forme même de l'affectation qui ferme le défaut. **Bénéfice collatéral** : le dernier tick ON avant une coupure, auparavant perdu, est désormais crédité exactement une fois.

Deux gardes CI (`hygiene-specs.yml`, job « Comptabilité pompe — point de passage unique ») exécutent [`tools/check_acct_settle_single_point.sh`](../../tools/check_acct_settle_single_point.sh) : (1) `accountPumpVolume()` n'a **qu'un** appelant, `settlePumpAccounting()` ; (2) le wrapper d'`update()` conserve ses trois lignes (`_acctSettledThisTick = false`, `updateDosingCore(...)`, `if (!_acctSettledThisTick) settlePumpAccounting(...)`). ⚠️ Le second contrôle a été ajouté **en revue** : sans lui, réintégrer `updateDosingCore()` dans `update()` en retirant `_acctSettledThisTick` laissait la garde **verte** tout en rejouant le défaut en silence — éprouvé trois fois (drapeau retiré, filet retiré, commentaire mentionnant l'appel sans l'appel lui-même).

⚠️ **Discontinuité métrologique** : après ce correctif, `N` (démarrages) **augmente** (les reprises comptent désormais) et `V` (volume) **diminue** (plus de crédit fantôme) — `δ` change de valeur **sans que le matériel ait bougé**. Aucun relevé de concordance postérieur à v2.61.0 n'est comparable aux deux campagnes jerricane de 2026 (+5,11 % sur 86 démarrages le 08/09, +6,25 % sur 356 le 08/10). Voir [ADR-0091](../adr/0091-comptabilite-hors-du-corps-decisionnel-dupdate.md).

Honnêteté sur le périmètre testé : `pump_controller.cpp` n'est pas compilable en `env:native` (FreeRTOS, Arduino, I²C) — aucun test natif ne peut démontrer que les quatre sorties anticipées atteignent bien `settlePumpAccounting()`. Cette garantie est **structurelle** (le wrapper), pas testée unitairement ; c'est la garde CI ci-dessus qui en tient lieu. La suite native [`test/test_native_acct_window/`](../../test/test_native_acct_window/) couvre en revanche exhaustivement le contrat pur d'`evaluateAcctWindow()` et les séquences (coupure 6 h, manuel continu sous `flowAssistSuspend`, enchaînements de dix cycles).

#### Persistance et remise à zéro

- **NVS** : namespace `pool_prod`, clés `ph_starts` / `ph_anom` / `ph_anom_ml` / `orp_starts` / `orp_anom` / `orp_anom_ml` ([`config.cpp:723`](../../src/config.cpp:723)). **Aucun flush ajouté** : le drapeau `productConfigDirty` est armé au **même endroit** que pour le volume et l'écriture reste celle, **différée de 60 s**, de la boucle de configuration ([`main.cpp:501`](../../src/main.cpp:501)).
- **Remise à zéro** : **exclusivement** dans les blocs `ph_reset_container` / `orp_reset_container` de [`web_routes_config.cpp:1110`](../../src/web_routes_config.cpp:1110) et `:1131`, **avec** `*TotalInjectedMl`, par le **même geste** (« Nouveau bidon »). Un compteur d'hygiène qui survivrait à la campagne qu'il vient de qualifier **disqualifierait un relevé propre**.

#### Exposition

`ph_starts` / `orp_starts` : [`GET /get-config`](../API.md#suivi-produit--relevé-du-biais-de-comptage-feature-110-v2450) **et** trame WebSocket `config`. Les 4 compteurs d'hygiène sont **HTTP-seuls** et déclarés dans la sentinelle de parité ([ws-manager.md](ws-manager.md#feature-110-v2450--compteurs-de-démarrages-de-pompe-ph_starts--orp_starts)). Les **huit** valeurs, plus `*_total_ml`, sont publiées dans le topic retenu `{base}/diagnostic` ([MQTT.md](../MQTT.md#relevé-du-biais-de-comptage-feature-110-v2450)).

Mode d'emploi du relevé (quoi noter, quand, et comment reconnaître un relevé qui ne conclut rien) : [page Produits](../features/page-dosages.md#relevé-du-biais-de-comptage--mode-demploi-feature-110-v2450).

### Sessions, cycles, budget horaire

- **Cycle = session** : `cyclesToday`, le ring anti-rafale et la ligne `Démarrage dosage` comptent la **session** (invariant v2.20.5 conservé) — les impulsions internes n'incrémentent **rien**.
- La **pause mélange** s'arme en **fin de session** (y compris fin par gate ou entrées de plan invalides), jamais entre deux pouls ni entre deux périodes d'accumulation (v2.22.0 — la séance reste ouverte sous le plancher).
- **Limite horaire (`usedMs`) et temps minimum d'injection : temps POMPE-ON** ([ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md), validé gate) — `refreshDosingState(state, now, pumpOn)` accumule quand la pompe est physiquement alimentée (pouls, manuel, test), et `shouldContinueDosing(error, stopThreshold, sessionOnMs)` raisonne sur le temps pompe-ON cumulé de la session (`_sessionOnMs`). Sémantique chimique : « minutes où le produit coule ».
- Log d'arrêt de session : `Arrêt dosage pH (auto): session=Xs pompe-ON=Ys vol≈Z mL total jour=…` (symétrique ORP).

### Exposition du calendrier d'impulsion (itération 3, v2.21.1)

Trois getters **lecture seule** ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)) exposent l'état du train d'impulsions à l'UI/WS, par **chimie** (chem 0 = pH, 1 = ORP) :

- `isPulseSessionActive(chem)` — séance de dosage ouverte (auto **ou** scheduled) = `_pulseSession[chem]`. C'est l'état « commandé » (ce que disent déjà `isPhDosing()`/`isOrpDosing()` côté auto).
- `getPulseMs(chem)` — durée du pouls **planifié** pour la période courante (`_pulseMs[chem]`). `0` = période sans pouls (v2.22.0 : régime d'**accumulation** sous le plancher — l'UI affiche « Dosage en attente : Ns », aucun champ nouveau).
- `getPulsePhaseMs(chem, nowMs)` — position dans la période : `nowMs − periodStartMs` en arithmétique uint32 **non signée** (wrap-safe au rollover de `millis()`), **clampée [0..`kPulsePeriodMs`]**, **relative** (jamais un `millis()` absolu — le client l'ancre sur sa propre horloge). `0` si session inactive. **Pompe alimentée ⇔ `phase < pulse_ms`.**

Garanties : **aucun setter associé**, aucun chemin de décision de dosage traversé ; gardes d'index **fail-closed** (`chem` hors [0..1] → `false`/`0`). **Course bénigne assumée** (patron `isAnyPumpEnergized`/`getPumpAmplitudeDuty`) : `_pulseSession`/`_pulseMs`/`_pulseState` sont écrits par loopTask et lus depuis le contexte web async — lectures 8/32 bits atomiques sur Xtensa, pire cas un instantané en retard d'un tour de loop. Consommateurs : `GET /data` et push WS `sensor_data` (7 champs `pulse_*`, [ws-manager.md](ws-manager.md#champs-sensor_data-ajoutés-en-v2211--calendrier-dimpulsion-adr-0045)) → statut dosage des cartes pH/ORP ([page-dashboard.md](../features/page-dashboard.md#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211)).

### Injection manuelle volumée (`/ph|orp/inject/start`)

Plus jamais de durée déduite du modèle linéaire (`calcInjectFlow` supprimé). `planManualInject()` ([`web_routes_control.cpp`](../../src/web_routes_control.cpp)) ne connaît que **deux points** :

- amplitude configurée **== duty calibré** → duty calibré, débit pesé ;
- sinon → duty **forcé à 255**, débit `kPumpFlow255MlPerMin` (**majorant de flotte** depuis la v2.25.2 : la durée annoncée est donc légèrement **courte**, jamais longue) ;
- état incohérent (duty calibré posé mais débit pesé invalide) → **409 `flow_uncharacterized`** + `critical`. Voir [API.md](../API.md#codes-de-refus-409-v260).

## Paramètres en dur (récapitulatif)

| Catégorie | Constante | Valeur | Source |
|-----------|-----------|--------|--------|
| Régulation — Kp pH par vitesse (**externalisés en v2.26.0** : `integralMax` en dérive) | `kPhKpSlow` / `kPhKpNormal` / `kPhKpFast` | 4 / **8** / 12 | [`constants.h`](../../src/constants.h) |
| Régulation P — Kp ORP (feature-025) | `kp` (override constructeur) | 0.15 / **0.3** / 0.5 | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| Régulation — Kd (feature-025, **impératif sur les 2 chimies**) | `kd` | 0 | [`pump_controller.h`](../../src/pump_controller.h) |
| Régulation — Ki ORP (feature-025, **inchangé en v2.26.0**) | `ki` (ORP) | 0 | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| Intégrale pH — **autorité** du terme intégral seul (v2.26.0, [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) — **constante de SÉCURITÉ CHIMIQUE**) | `kPhIntegralAuthorityMlPerMin` | 6,0 mL/min (= 2 × charge mesurée 2,86) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — constantes de temps par vitesse | `kPhIntegralTiSlowS` / `TiNormalS` / `TiFastS` | 8 h / **6 h** / 4 h | [`constants.h`](../../src/constants.h) |
| Intégrale pH — Ki dérivé (`kp / Ti`) | `kPhKiSlow` / `kPhKiNormal` / `kPhKiFast` | 1,389e-4 / **3,704e-4** / 8,333e-4 | [`constants.h`](../../src/constants.h) |
| PID — anti-windup pH, **dérivé de l'autorité** `U × Ti / kp` (v2.26.0 ; était 50,0 inerte) | `kPhIntegralMaxSlow` / `Normal` / `Fast` | 43 200 / **16 200** / 7 200 pH·s | [`constants.h`](../../src/constants.h) |
| PID — anti-windup, défaut de structure (**volontairement inerte**, `Ki = 0` par défaut) | `integralMax` | 50.0 | `PIDController` ([`pump_controller.h`](../../src/pump_controller.h)) |
| Intégrale pH — marge du reset sec (seuil réel −0,15 avec les défauts) | `kPhIntegralResetMargin` | 0,10 pH | [`constants.h`](../../src/constants.h) |
| Intégrale pH — **tenue** de l'excursion avant purge sèche (v2.27.0 ; = pause mélange) | `kPhIntegralResetHoldMs` | 900 000 ms (15 min de branche) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — demi-vie d'oubli sous consigne (pas fixes de 1 min, facteur 0,9885140) | `kPhIntegralDecayHalfLifeMs` | 3 600 000 ms (1 h) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — `dt` maximal crédité | `kPhIntegralMaxTickDtMs` | 10 000 ms (10 s) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — quantum d'intégration (garantit un incrément représentable en `float`) | `kPhIntegralChargeQuantumMs` | 1 000 ms (1 s) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — gel sur quota journalier | `kPhIntegralFreezeDailyFraction` | 0,80 | [`constants.h`](../../src/constants.h) |
| Intégrale pH — péremption | `kPhIntegralStaleResetMs` | 86 400 000 ms (24 h) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — hystérésis de saturation (entrée / sortie) | `kPhIntegralSaturationPct` / `…ClearPct` | 95 % / 85 % | [`constants.h`](../../src/constants.h) |
| Intégrale pH — paliers d'alerte et rappel (**recalibrés en v2.27.0** : fenêtre réelle inchangée, ~8 h / ~24 h / ~48 h) | `…SaturationWarnMs` / `…CriticalMs` / `…ReminderMs` | **2 h 20 / 7 h / 14 h** de branche (était 2 h / 6 h / 12 h) | [`constants.h`](../../src/constants.h) |
| Intégrale pH — rapports cycliques du tick d'entretien (**estimations analytiques**, servent au seul rapport 0,29/0,25) | `kPhIntegralDutyV2260` / `kPhIntegralDutyF084` | 0,25 / 0,29 | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — plafond de restauration (v2.27.0, [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) — **constantes de SÉCURITÉ CHIMIQUE**) | `kPhIntegralRestoreMaxFraction` / `…RestoreLoadFraction` | 0,30 (butée absolue) / 0,50 (de l'équilibre **observé**) | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — historique de charge (fenêtre / exigence fail-closed) | `kPhIntegralRestoreHistoryWindowDays` / `…MinHistoryDays` | 7 j / **3 j** | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — péremption de l'instantané (R3, v2.34.0 — [ADR-0060](../adr/0060-fraicheur-en-temps-de-filtration.md)) | `kPhIntegralPersistMaxFiltMin` / `…PersistMaxWallAgeS` | **360 min de filtration** (critère) **ET** 72 h murales (filet) | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — confirmation par la mesure (R4) / fenêtre d'armement | `kPhIntegralRestoreConfirmMs` / `…RestoreArmWindowMs` | 5 min de branche / 10 min après boot | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — cadence d'écriture **conjonctive** (R5, motif `kDelta`) | `kPhIntegralPersistMinIntervalMs` / `…MinDeltaFrac` | 30 min **ET** 5 % du plafond | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — échéance d'écriture forcée (R5, motif `kDue`, v2.34.0) | `kPhIntegralPersistDueFiltMin` | 180 min de filtration | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — anti-boucle de démarrage (R7) | `kPhIntegralRestoreMaxStreak` | 3 restaurations | [`constants.h`](../../src/constants.h) |
| Persistance intégrale — schémas NVS (toute retouche de structure les incrémente) | `kPhIntegralPersistSchema` / `kPhLoadHistorySchema` | **2** / 1 | [`constants.h`](../../src/constants.h) |
| Charge observée — circulation minimale d'un jour exploitable | `kPhLoadDayMinFiltrationMin` | 120 min (2 h) | [`constants.h`](../../src/constants.h) |
| Charge observée — `dt` maximal crédité au **dénominateur** (≠ 10 s de l'intégrale : sens du fail-closed inversé) | `kPhLoadMinuteMaxTickDtMs` | 60 000 ms (60 s) | [`constants.h`](../../src/constants.h) |
| Charge observée — filet de vraisemblance : **REPLI** et tolérance ([ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) — le seuil nominal est une **fonction** de la limite horaire et du débit calibré) | `kPhLoadSustainableFallbackMlPerMin` (ex-`kPhLoadSustainableMlPerMin`) / `kPhLoadDayMaxRateFactor` | 2,17 mL/min (repli) / 1,10 | [`constants.h`](../../src/constants.h) |
| Régulation — **DEMANDE** à l'équilibre de l'installation de référence (⚠️ pas un volume délivré) | `kPhEquilibriumLoadMlPerMin` | 2,86 mL/min | [`constants.h`](../../src/constants.h) |
| PID — borne erreur pH | `kPhMaxError` | 1.0 | [`constants.h:89`](../../src/constants.h:89) |
| PID — borne erreur ORP | `kOrpMaxError` | 200.0 mV | [`constants.h:90`](../../src/constants.h:90) |
| PID — `dt` max | (literal in code) | 10 s | [`pump_controller.cpp`](../../src/pump_controller.cpp) |
| Anti-cycling — durée min | `minInjectionTimeMs` | 30000 | [`config.h:154`](../../src/config.h:154) |
| Anti-cycling — start pH (**source `constexpr` depuis la v2.36.0**) | `phStartThreshold` ← `kPhStartThresholdDefault` | 0.05 | [`constants.h`](../../src/constants.h), initialise [`config.h:155`](../../src/config.h:155) |
| Anti-cycling — stop pH (**source `constexpr` depuis la v2.36.0**) | `phStopThreshold` ← `kPhStopThreshold` | 0.01 | [`constants.h`](../../src/constants.h), initialise [`config.h:156`](../../src/config.h:156) |
| Porte de session pH — fenêtre du signal lent (feature-094, [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) | `kPhDecisionWindowMs` | 600 000 ms (10 min) | [`constants.h`](../../src/constants.h) |
| Porte de session pH — marge de fermeture | `kPhDoorCloseMarginPh` | 0,03 pH | [`constants.h`](../../src/constants.h) |
| Porte de session pH — durée minimale (**murale**) | `kPhSessionMinWallMs` | 1 800 000 ms (30 min) | [`constants.h`](../../src/constants.h) |
| Porte de session pH — plancher de sous-dépassement (**= `kPhFfAdverseErrorPh`**, `static_assert`) | `kPhSessionUndershootFloorPh` | 0,05 pH | [`constants.h`](../../src/constants.h) |
| Anti-cycling — start ORP | `orpStartThreshold` | 10.0 mV | [`config.h:157`](../../src/config.h:157) |
| Anti-cycling — stop ORP | `orpStopThreshold` | 2.0 mV | [`config.h:158`](../../src/config.h:158) |
| Anti-cycling — cycles/jour (**configurable 10-60**, feature-060) | `maxCyclesPerDay` | **24** (défaut, était 48 — v2.20.5) | [`config.h:165`](../../src/config.h:165), bornes [`constants.h:423`](../../src/constants.h:423) |
| Dosage — throttle du `WARN` « aucun débit commandé » (v2.20.5) | `kDosingNoFlowWarnIntervalMs` | 300 000 ms (5 min) | [`constants.h:706`](../../src/constants.h:706) |
| Deadband pH | `PH_DEADBAND` | 0.01 | [`config.h:25`](../../src/config.h:25) |
| Deadband ORP | `ORP_DEADBAND` | 2.0 mV | [`config.h:26`](../../src/config.h:26) |
| Pompe — plancher de rotation (**98 en v2.21.0** : Dmin mesuré 82 + 16 counts marge P2) | `MIN_ACTIVE_DUTY` | 98 (sur 255) | [`config.h`](../../src/config.h) |
| Pompe — duty max | `MAX_PWM_DUTY` | 255 (8 bits) | [`config.h:27`](../../src/config.h:27) |
| Impulsions — période fixe (B3, v2.21.0) | `kPulsePeriodMs` | 60 000 ms (1 min) | [`constants.h`](../../src/constants.h) |
| Impulsions — pouls minimal (B3 ; durée exacte du tir d'accumulateur, v2.22.0 ; **10 s depuis la v2.25.3**, [ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md) — rendement des démarrages, dose demandée inchangée) | `kPulseMinMs` | **10 000 ms (10 s)** — était 5 000 | [`constants.h`](../../src/constants.h) |
| Impulsions — kick de décollage à 255 | `kPulseKickMs` | 400 ms | [`constants.h`](../../src/constants.h) |
| Pompes — débit **majorant de flotte** à 255 (v2.25.2, ADR-0050 — remplace `kPump1/2Flow255MlPerMin` = 61,0 / 67,0) | `kPumpFlow255MlPerMin` | 74,0 mL/min (= max mesuré 68,0 + 8 %) | [`constants.h`](../../src/constants.h) |
| Pompes — maximum **réellement mesuré** à 255 (verrou `static_assert`, aucun calcul) | `kPumpFlow255MeasuredMaxMlPerMin` | 68,0 mL/min | [`constants.h`](../../src/constants.h) |
| Amplitude — plancher du réglage `pumpN_max_duty_pct` | `kPumpAmpMinPct` | 40 % (≈ 102 counts) | [`constants.h`](../../src/constants.h) |
| Amplitude — défaut (gelée, v2.23.0) | `kPumpAmpDefaultPct` | 50 % | [`config.h`](../../src/config.h) |
| Calibration de type — duty (v2.23.0, ADR-0047, static_assert == 127) | `kPumpTypeCalDuty` | 127 (= 50 % × 255, division entière) | [`config.h`](../../src/config.h) |
| Calibration de type — débit au duty 127 (v2.23.0 ; **justification refondée sur la mesure en v2.25.2**, ADR-0050 : max mesuré 26,0 + 8 %) | `kPumpTypePulseFlowMlPerMin` | 28,0 mL/min | [`constants.h`](../../src/constants.h) |
| Calibration de type — maximum **réellement mesuré** à 127 (v2.25.2, verrou `static_assert`, aucun calcul) | `kPumpTypeFlowMeasuredMaxMlPerMin` | 26,0 mL/min | [`constants.h`](../../src/constants.h) |
| Calibration — plausibilité du débit pesé | `kPulseFlowMaxMlPerMin` | 200 mL/min | [`constants.h`](../../src/constants.h) |
| Impulsions — throttle des logs de refus de gate (critical ; l'info seuil C est supprimé en v2.22.0) | `kPulseRefusalLogIntervalMs` | 1 800 000 ms (30 min) | [`constants.h`](../../src/constants.h) |
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
| Chloration de secours — durée glissante (feature-140) | `kRescueChlorineDurationS` | 172 800 s (48 h) | [`constants.h`](../../src/constants.h) |
| Chloration de secours — volume quotidien, **défaut grand bassin / repli final** (feature-140, rôle redéfini feature-141) | `kRescueChlorineDailyMl` | 1500 mL/j | [`constants.h`](../../src/constants.h) |
| Chloration de secours — plafond dur (feature-140) | `kRescueChlorineDailyHardCapMl` | 2000 mL/j | [`constants.h`](../../src/constants.h) |
| Chloration de secours — plancher d'efficacité du volume SAISI (feature-141) | `kRescueChlorineDailyMinMl` | 100 mL/j | [`constants.h`](../../src/constants.h) |
| Chloration de secours — défauts de pré-remplissage par taille (feature-141) | `kRescueChlorineDailyBySizeMl[3]` | 500 / 1000 / 1500 mL/j | [`constants.h`](../../src/constants.h) |
| Chloration de secours — tenue avant levée auto (feature-140) | `kRescueChlorineAutoLiftHoldMin` | 30 min | [`constants.h`](../../src/constants.h) |
| Chloration de secours — fenêtre de visibilité de `rescue_lift` (feature-140) | `kRescueLiftReasonVisibleMs` | 90 000 ms | [`constants.h`](../../src/constants.h) |

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
- **Horizon** : factorisé depuis feature-146 (v2.63.0) dans `filtrationDosingHorizonMinutes(nowMin)` (fonction statique de `pump_controller.cpp`, appelée par les deux branches pH et ORP) : `remainingRangeMinutes(nowMin, start, end)` ([`src/schedule_logic.h`](../../src/schedule_logic.h)) — minutes restantes de la plage de filtration **de base**, **bornées à minuit** (reset des compteurs journaliers, [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md)). **Hors mode d'installation `managed`** (feature-056 : `installMode != ManagedFiltration`) **et en mode Marche** (feature-146 : `filtrationCfg.mode == "force"`, le créneau start/end ne décide plus rien dans ce mode), horizon = `1440 − nowMin` — même traitement « journée entière », repris du patron existant plutôt que dupliqué : le dosage hors plage reste permis (eau présumée 24/7 en `powered`, horizon plein en `external` borné par la garde présence d'eau, horizon plein en Marche car la pompe tourne en permanence). Comportement de l'ex-`continu` préservé. ⚠️ **Statu quo volontaire sur le Boost** ([ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) : l'horizon lit toujours le créneau de **base**, jamais le créneau élargi par le Boost (`effectiveRunWindow`) — un Boost nocturne continue de **refuser** le dosage réparti hors du créneau programmé, aucune régression introduite.
- **Ring anti-rafale consulté AVANT tout démarrage** (condition pool-chemistry n°1) : transition inactive → active refusée si `kMaxDosingCyclesPerMinute` (6/min) ou `kMaxDosingCyclesPer15Min` (20/15 min) atteint — mêmes seuils et fenêtres que `canDose`, ring **partagé** auto + manuel + scheduled. Chaque démarrage est enregistré via `recordDosingCycleStart()`.
- **Exemption `cyclesToday` conservée** (arbitrage R4 validé pool-chemistry) : les démarrages scheduled n'incrémentent pas `maxCyclesPerDay`, car ils sont **bornés structurellement** (≤ 4/h par le cadencement 15 min) et gardés par le ring anti-rafale. Voir [ADR-0021](../adr/0021-repartition-scheduled.md).
- **Débit effectif unique** (condition n°5) : la même variable `effectiveFlowMlPerMin` sert au bornage horaire, à la durée d'injection et au duty. **v2.21.0** : c'est le **débit pesé à l'amplitude calibrée** (`pumpN_pulse_flow_ml_per_min`) — la branche scheduled dose **en continu à l'amplitude** (kick en démarrage de session) et passe par la **même gate de caractérisation** que l'auto : pompe non caractérisée → `CRITICAL [Scheduled] Pompe pH|ORP non caractérisée … — dosage programmé BLOQUÉ (pesée au banc requise)`, throttlé 30 min.
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

## Mode Boost (feature-053, effet filtration révisé par feature-146)

Le **Mode Boost** est une **surcouche temporaire non destructive** permettant d'assainir davantage la piscine pour la journée après une forte fréquentation. Il **ne modifie jamais la config persistée** (cible, limites, mode) : il expose des **valeurs *effectives*** consommées par la régulation, et un **créneau effectif** consommé par la décision de filtration. Décision structurante et bornes de sécurité : [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md) (remplace [ADR-0025](../adr/0025-mode-boost.md), validé `pool-chemistry` GO sous conditions).

### État & persistance

- `boostState` = `{ bool active; time_t untilEpoch; }` persisté en **NVS dédié** (namespace `poolctrl`) → survit à un reboot dans la journée.
- **Activation refusée sans heure synchronisée** : `untilEpoch` = **prochain minuit local**, incalculable sans horloge valide → l'activation retourne `409 time_not_synced` (route HTTP) ou ignore la commande MQTT (pas de boost « sans fin »).
- **Activation refusée en mode de filtration Arrêt** (feature-146, v2.63.0) : `startBoost()` ([`src/config.cpp`](../../src/config.cpp)) teste `parseFiltrationMode(filtrationCfg.mode) == FiltrationMode::Arret` **avant** de calculer l'échéance → `409 filtration_stopped` (route HTTP) ou commande MQTT ignorée. Motif : le Boost surchlore une eau qui doit circuler, pas de circulation en Arrêt.
- Getters publics : `isBoostActive()`, `getBoostUntilEpoch()`. Pilotage : `startBoost()` / `stopBoost()` (routes `POST /boost/start` \| `/boost/stop`, commande HA `{base}/boost/set`).

### Expiration au rollover minuit, et annulation par le mode Arrêt

`tickBoostExpiry()` est appelée **avant** le reset des compteurs journaliers, **dans les 2 branches** de `tickDailyRollover()` (NTP/RTC synchronisé ET fallback `millis()`) : à minuit local, si le boost est actif il est désactivé (log `info`) et l'état republié (WS + MQTT). Au **boot**, si `now >= untilEpoch` le boost est inactif ; sinon il reprend. Le budget est donc **trivialement borné à un seul jour calendaire** (jamais à cheval sur deux fenêtres de compteur journalier).

⚠️ **Second mécanisme d'annulation (feature-146, condition 2′ du gate `pool-chemistry`)** : `filtration.update()` appelle `stopBoost()` — **effacement de `boost_until`, pas un simple contournement** — dès que le mode courant est Arrêt et qu'un Boost est actif. Un `if` qui se contenterait d'**ignorer** `isBoostActive()` en Arrêt aurait laissé la consigne ORP relevée et le plafond chlore ×1,5 actifs en coulisses (ces deux effets lisent `isBoostActive()` **directement**, voir plus bas) : la surchloration repartirait **seule** au retour en Marche, sans geste utilisateur. L'effacement ferme ce cas.

### Trois effets tant que `isBoostActive()`

1. **Créneau de filtration effectif élargi jusqu'à minuit** — feature-146 : `boostForce` a **disparu** de `decideFiltrationRun()`. Le Boost ne décide plus rien ; il **modifie le créneau en amont**, via la fonction pure `effectiveRunWindow(base, boostActive, nowMin)` ([`src/schedule_logic.h`](../../src/schedule_logic.h)), dérivé **en RAM uniquement** (jamais stocké dans `filtrationCfg`). Disponible dans tous les modes **sauf Arrêt** (voir ci-dessus). En mode **Marche**, cet effet est **sans objet** : `decideFiltrationRun()` n'y consulte de toute façon pas le créneau (la pompe tourne déjà). Voir [filtration.md](filtration.md#décision-et-temps-indisponible) et [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md).
2. **Cible ORP effective** : `getEffectiveOrpTarget() = min(orpTarget + kBoostOrpDeltaMv, kBoostOrpCeilingMv)` (60 mV / plafond 850 mV, sous l'alerte `orp_abnormal` > 900). **Jamais abaissée.** La régulation ORP **automatique** existante injecte naturellement vers cette cible — **aucun nouveau chemin d'injection**. Lit `isBoostActive()` directement ([`src/dosing_logic.cpp:549`](../../src/dosing_logic.cpp)), indépendamment du mode de filtration.
3. **Limite journalière chlore effective** : `getEffectiveMaxChlorine() = min(maxChlorineMlPerDay × kBoostDailyFactor, kBoostDailyHardCapMl)` (×1,5 / plafond dur 1000 mL). Lit `isBoostActive()` directement ([`src/dosing_logic.cpp:560`](../../src/dosing_logic.cpp)), indépendamment du mode de filtration — **c'est précisément pourquoi l'annulation en Arrêt doit effacer `boost_until`** (ci-dessus) plutôt que de bloquer seulement l'effet 1.

### Gate mode `automatic` (logique pure)

L'effet **chlore** (points 2 et 3) est **strictement gaté au mode de régulation ORP `automatic`** : les fonctions pures `effectiveOrpTargetPure(...)` / `effectiveMaxChlorinePure(...)` ([`src/dosing_logic.h`](../../src/dosing_logic.h), patron [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) ne relèvent cible et limite que si `mode == automatic`. En mode **Manuel** ou **Programmé**, le Boost n'étend **que le créneau de filtration** (point 1) ; l'**injection manuelle reste bornée à la limite normale**.

### Disponibilité par mode de filtration (feature-146, v2.63.0)

| Mode | Boost | Effet |
|---|---|---|
| **Auto** | autorisé | créneau étendu jusqu'à minuit + surchloration |
| **Programme** | autorisé | identique à Auto — aucune différence de traitement, et **plus sûr** (le créneau de base n'est jamais recalculé en Programme, donc aucun effet rétroactif via l'échantillonnage de température) |
| **Marche** | autorisé | effet filtration sans objet (pompe déjà en marche) ; **surchloration active** — cas neuf, validé `pool-chemistry` : aucun plafond desserré, les deux effets de dosage sont bornés **en dur** (`kBoostDailyHardCapMl`, `kBoostOrpCeilingMv`), pas additifs avec l'éligibilité 24 h/24 de ce mode |
| **Arrêt** | **refusé** | pas de circulation ⇒ pas de dosage ⇒ surchloration impossible |

### Signalement des leviers inertes (feature-054, v2.18.1 — révisé feature-146)

Un seul levier peut encore être **inerte** selon la config (à distinguer d'un **refus** d'activation, qui bloque) :

| Levier | Condition d'effet | Inerte si |
|--------|-------------------|-----------|
| Créneau de filtration étendu | `install_mode == "managed"` (filtration gérée par PoolController) | Filtration non gérée (`powered`/`external`) → le relais n'est de toute façon pas piloté par le PoolController (retour anticipé dans `filtration.update()`) |
| Chlore relevé | `orpRegulationMode == "automatic"` | Mode ORP Manuel / Programmé → cible et limite non relevées (gate ci-dessus) |

`startBoost()` ([`src/config.cpp`](../../src/config.cpp)) émet, après activation, un log `warning` par levier inerte — ce qui **couvre les activations HTTP ET MQTT/HA** (canal unique) :
- `install_mode != "managed"` → « Filtration non gérée par PoolController — le Boost ne prolonge PAS la filtration ».
- `orpRegulationMode != "automatic"` → « Régulation ORP non automatique — le Boost ne relève PAS le chlore ».

La route `POST /boost/start` expose en parallèle ces deux états dans sa réponse 200 (`filtration_extended`, `chlorine_boosted`) pour que l'UI affiche un toast adaptatif ([API.md](../API.md#post-booststart--write), [page-dashboard.md](../features/page-dashboard.md#carte-modes-temporaires-feature-140)). **Purement informatif** : aucun chemin de dosage ni aucune garde n'est modifié, l'activation n'est jamais bloquée par un levier inerte — à la différence du refus **mode Arrêt**, qui lui bloque (409 côté HTTP, commande ignorée côté MQTT).

### Garde-fous inchangés

**Tous les autres garde-fous restent intacts** : limite horaire, temps min d'injection (`minInjectionTimeMs`), anti-rafale (ring partagé), garde « injection uniquement si filtration active », watchdog. Le Boost ne fait que déplacer cible + plafond journalier consommés par `canDose()` / la régulation auto existante, et élargir le créneau lu par `decideFiltrationRun()` ; il **n'ouvre aucun chemin de dosage** et ne contourne aucune garde de `evaluateDose`.

⚠️ **Source unique du relais (feature-146, condition bloquante n°1 du gate)** : `relayShouldBeOn = state.running` dans `filtration.update()` ([`src/filtration.cpp`](../../src/filtration.cpp)) — sans second terme. L'ancien `&& (filtrationCfg.mode != "off")` lisait le mode **brut** quand la décision lisait le mode **minusculisé**, et **relisait la config vivante après la décision** : avec Boost actif + mode Arrêt (avant que la restriction ci-dessus n'existe), `state.running` pouvait valoir `true` pendant que le relais restait réellement ouvert — `resolveWaterPresence()` lit `state.running`, pas l'état du relais, donc l'appareil annonçait une présence d'eau fausse. Invariant désormais prouvé par test : `state.running == relayState` en mode `ManagedFiltration`.

### Testabilité native

`effectiveOrpTargetPure` / `effectiveMaxChlorinePure` (bornage + gate mode) sont couvertes par la suite Unity native depuis la feature-053 : plafonds, non-abaissement, gate `automatic` vs Manuel/Programmé, arithmétique du delta et du facteur. feature-146 ajoute la couverture de `effectiveRunWindow` et de la matrice `decideFiltrationRun` (quatre modes, voir [filtration.md](filtration.md#testabilité-native)) — voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Chloration de secours (feature-140)

**Deuxième surcouche temporaire non destructive**, décalquée du Mode Boost ci-dessus mais de nature différente : le Boost **amplifie** la régulation, la Chloration de secours la **remplace** temporairement par un volume fixe, aveugle à l'ORP. Objet : continuer à injecter du chlore quand les sondes pH/ORP sont déclarées non fiables (eau trouble, boue au fond du bassin après un épisode de pluie) — sans quoi tout dosage automatique s'arrête, chlore compris, au moment où la demande en désinfectant est la plus forte (incident fondateur du 2026-10-02). Décision structurante et bornes de sécurité : [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md) (`pool-chemistry` GO sous conditions, deux passages — sur l'arbitrage initial puis sur le diff réel).

### Aucun second chemin de dosage : la branche `scheduled` existante, empruntée

La Chloration de secours **ne crée pas** de nouvelle logique de dosage : elle active la branche `scheduled` déjà en place (section [Mode `scheduled`](#mode-scheduled) ci-dessus), déjà validée `pool-chemistry` en feature-011, avec son propre volume quotidien dédié et son propre plafond dur. Le travail de la feature-140 est donc entièrement une **surcouche de découvrabilité, de réversibilité et de bornage dans le temps** — pas une nouvelle chimie.

### Le piège qui a failli rendre la feature morte à l'arrivée : mode EFFECTIF vs NVS

La branche `scheduled` ne s'ouvre que si `mqttCfg.orpRegulationMode == "scheduled"` **en NVS**. Or la Chloration de secours est non destructive : elle **n'écrit jamais** cette valeur. Avec la NVS restée sur `"automatic"` et un pH NaN (scénario nominal : boue sur les deux sondes), `tickOrpPhInhibitSurveillance()` aurait posé l'inhibition ORP (`requireKnownPh = (mode == "automatic")`), et `!isOrpInhibitedByPh()` aurait fermé la branche `scheduled` même activée — **zéro injection**, exactement le défaut que la feature doit corriger.

La règle opposable qui referme ce piège (posée par [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md)) :

> **décision de dosage ⇒ mode ORP EFFECTIF · publication de configuration (HTTP/MQTT/WS) ⇒ valeur NVS, inchangée**

Portée par `src/temp_mode_logic.h` (module **pur**, patron [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) :

```cpp
enum class OrpMode : uint8_t { Automatic = 0, Scheduled = 1, Manual = 2 };
OrpMode effectiveOrpModePure(OrpMode nvsMode, bool rescueActive);
// nvsMode == Manual            => Manual (fail-closed, condition #5 du gate)
// rescueActive && nvsMode != Manual => Scheduled (quelle que soit la valeur NVS)
// sinon                        => nvsMode inchangé (non-régression D3)
```

Côté coquille, `PumpControllerClass::effectiveOrpMode()` ([`pump_controller.cpp`](../../src/pump_controller.cpp)) collecte `mqttCfg.orpRegulationMode` (NVS) + `isRescueChlorineActive(time(nullptr))` et délègue la décision à `effectiveOrpModePure()`. **Tout site qui décide d'un dosage ORP lit cet accesseur**, jamais `mqttCfg.orpRegulationMode` directement — audité et routé en deux passes (un premier audit avait laissé passer quatre sites supplémentaires, `mqttCfg.orpDailyTargetMl` lu à quatre endroits) :

- `tickOrpPhInhibitSurveillance()`, `canDose(1)`, la conjonction `automatic`/`scheduled` de `update()`, `tickDailyRollover()` (reliquat perdu à minuit), `time_to_target.cpp` (`in.automatic`), `web_routes_control.cpp:767` (`chlorBoost` publié par `/boost/start`) : **décision** ⇒ `effectiveOrpMode()`.
- `ws_manager.cpp` (`boost_chlorine_boosted`), `mqtt_manager.cpp`, `web_routes_config.cpp` (`/get-config`) : **publication** ⇒ `mqttCfg.orpRegulationMode` (NVS), inchangée.

`getEffectiveOrpDailyTargetMl()` compose, de façon symétrique, le volume quotidien EFFECTIF — délègue à `effectiveOrpDailyTargetMlPure()` :

```cpp
float effectiveOrpDailyTargetMlPure(float nvsDailyTargetMl, bool rescueActive,
                                     bool boostActive, float rescueDailyMl,
                                     float boostFactor, float maxChlorineMlPerDay,
                                     float rescueHardCapMl);
```

- secours **inactif** : `nvsDailyTargetMl` inchangé (non-régression D3 : le Boost seul reste inerte sur le volume `scheduled`/`manual`, exactement comme avant cette feature).
- secours **actif** : part de `rescueDailyMl` — **depuis la v2.56.0 (feature-141), c'est `rescueState.dailyMl` (le volume ARMÉ à l'activation, résolu par `resolveRescueDailyMlPure`/`clampRescueDailyMlPure`), plus la constante `kRescueChlorineDailyMl`** (qui ne sert plus que de repli final de la chaîne de pré-remplissage — voir [§ Volume saisi à l'activation](#volume-de-la-chloration-de-secours-saisi-à-lactivation-feature-141-v2560) ci-dessous) — amplifié par le Boost (`× kBoostDailyFactor`) **si** `isBoostActive()`, puis les **deux** plafonds sont appliqués **en dernier**, dans cet ordre : limite utilisateur (`safetyLimits.maxChlorineMlPerDay`, si > 0 — **souveraine**) **puis** plafond dur `kRescueChlorineDailyHardCapMl`. ⚠️ **L'ordre ferme un chemin de contournement identifié par le gate, et reste INCHANGÉ par feature-141** — seule la SOURCE de `rescueDailyMl` a changé (une constante devient une variable d'état), pas la séquence des opérations : un clamp au plafond dur *avant* l'amplification Boost laisserait passer `1900 × 1,5 = 2850` mL (ou `1500 × 1,5 = 2250` à l'époque de la constante), au-dessus du plafond — fermé en appliquant le clamp dur **après** l'amplification (`temp_mode_logic.cpp:32-41`, vérifié au fichier et à la ligne par le gate 2, puis re-vérifié sans régression par le gate feature-141).

### État, persistance et activation

`RescueState { bool active; time_t untilEpoch; float dailyMl; }` ([`config.h`](../../src/config.h)), persisté en **NVS dédié** (namespace `poolctrl`, clés `rescue_active`/`rescue_until`/**`rescue_ml`** — toutes **sous** la limite Preferences de 15 caractères). ⚠️ **v2.56.0 (feature-141)** : l'obstacle initial à un volume paramétrable était le **nom** de la clé (`rescue_target_ml`, 16 caractères, ne passerait pas), pas le concept — `rescue_ml` (9 caractères) l'a levé. Le volume **n'est donc plus une constante**. `saveRescueState()`/`loadRescueState()` ([`config.cpp`](../../src/config.cpp)), appelée au boot (`main.cpp`, après `loadBoostState()`) — les trois clés sont écrites/lues dans la **même transaction** Preferences.

- `startRescueChlorine(float requestedDailyMl)` (signature élargie en v2.56.0) **refuse** (log `warning`, aucun état modifié) si :
  - `mqttCfg.orpRegulationMode == "manual"` (condition #5 du gate — double dosage : l'utilisateur dose déjà lui-même, l'activer en plus doserait deux fois) ;
  - l'heure n'est pas synchronisée (`now < kMinValidEpoch`, condition #6 — l'échéance 48 h serait indéterminable, même motif fail-closed que le Boost pour minuit).
- Activation réussie : `rescueState.untilEpoch = rescueDeadlinePure(now, kRescueChlorineDurationS)` — **epoch**, **48 h glissantes depuis l'instant d'activation**, pas un ancrage sur minuit local (voir ci-dessous pourquoi).
- `stopRescueChlorine(RescueLiftReason reason)` désactive et persiste immédiatement, **idempotent**. `reason` (voir plus bas) est publié à parité `/data` ↔ WS ↔ MQTT.
- `isRescueChlorineActive(time_t now)` délègue à `rescueActivePure()` : **fail-closed** si `now < kMinValidEpoch` (⚠️ à l'**inverse** du Boost, qui préserve l'état persisté sans heure valide — le secours dose à l'aveugle pour 48 h d'affilée, le risque d'un mode « actif pour toujours » sans contrôle d'échéance est jugé plus grave qu'une interruption transitoire au redémarrage).

### Pourquoi 48 h glissantes, et pas minuit local comme le Boost

**Minuit local est disqualifié techniquement** (et non par préférence) : le compteur journalier de volume chlore (`safetyLimits.dailyOrpInjectedMl`) **remet aussi à zéro à minuit**. Ancrer l'expiration du secours sur minuit la ferait coïncider avec ce rollover — une activation à 10 h ne recevrait qu'une **fraction** de son volume avant de mourir au reset suivant. D'où une échéance **epoch**, glissante, sans lien avec le passage de minuit (`tickRescueChlorineExpiry()`, appelée depuis `tickDailyRollover()` par simple mutualisation d'un point déjà exécuté à chaque tour — **pas** parce que l'expiration dépend de minuit). 72 h a été écarté (mode oublié, sans réévaluation) — voir [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md) pour l'arbitrage complet.

### Levée automatique — à SENS UNIQUE

`PumpControllerClass::tickRescueChlorineAutoLift(unsigned long nowMs)`, appelée à **chaque tour** d'`update()` (après `tickOrpPhInhibitSurveillance()`, avant toute décision de dosage) : après `kRescueChlorineAutoLiftHoldMs` (30 min) de tenue **continue** de `isfinite(sensors.getOrpFiltered()) && measureValidity.measuresValid()` (condition réécrite en v2.68.0, feature-152 : `isOrpFilterUnstable()` n'existe plus, une valeur officielle non-NaN suffit), le mode est **levé** (`stopRescueChlorine(RescueLiftReason::AutoLift)`) et le log **L6** (libellé imposé mot à mot par le gate) est émis. ⚠️ **À sens unique par construction** : cette levée **termine** le mode, elle ne le **ré-arme jamais** — un état absorbant ne peut pas battre (contrairement au défaut corrigé par feature-139). La moindre instabilité réarme le minuteur de tenue à zéro ; le mode inactif maintient le minuteur à zéro (no-op).

### Garde-fous conservés, sans exception

- **Présence d'eau (filtration)**, watchdog, budget horaire, anti-cycling, anti-rafale, limites journalière/horaire : **tous hérités de la branche `scheduled` existante**, aucune dérogation.
- **`isOrpInhibitedByPh()` conservée telle quelle** : pH connu et > 7,60 ⇒ chloration suspendue, **y compris en secours** (41 % de HOCl actif à pH 7,70 — le chlore y est peu efficace).
- **Compteur journalier PARTAGÉ** (`safetyLimits.dailyOrpInjectedMl`), **aucun compteur dédié** : armer le secours après que le plafond normal soit déjà atteint ⇒ 0 mL (pas de contournement par activation du secours).
- **Le reboot ne remet jamais à zéro** le compteur journalier (sinon un cycle de redémarrages contournerait le plafond).
- **Le pH n'est jamais dosé** par ce mode — aucun chemin ne peut atteindre la pompe pH depuis la Chloration de secours (condition #11 du gate).
- **Bascule en mode Manuel pendant le mode actif ⇒ désarmement automatique** (`web_routes_config.cpp`, `mqtt_manager.cpp` — même motif que le refus à l'activation, condition #5).

### Volume de la Chloration de secours saisi à l'activation (feature-141, v2.56.0)

Jusqu'à la v2.55.x, le volume quotidien était la constante `kRescueChlorineDailyMl` (1500 mL/j), identique pour tous les bassins — **juste par coïncidence** pour l'utilisateur qui l'avait fixée : le gate `pool-chemistry` a corrigé lui-même, à cette occasion, le rationale d'origine (« ≈ 3× le besoin d'entretien calculé ») en confrontant les relevés de terrain (resets de minuit, 939-1500+ mL/j) — la demande **réelle** est 2 à 3× le calcul **théorique**, donc 1500 n'était jamais une large marge. Depuis la v2.56.0, le volume est **saisi par l'utilisateur à l'activation**, pré-rempli, borné, persisté en NVS (`rescue_ml`, dans `RescueState.dailyMl`).

**Deux fonctions pures distinctes** ([`src/temp_mode_logic.h`](../../src/temp_mode_logic.h) / [`.cpp`](../../src/temp_mode_logic.cpp)) — à ne jamais confondre :

```cpp
float clampRescueDailyMlPure(float requestedMl, float userDailyLimitMl, float hardCapMl);
float resolveRescueDailyMlPure(float storedMl, float orpDailyTargetMl, float sizeDefaultMl,
                                float fallbackMl, float userDailyLimitMl, float hardCapMl);
```

- `clampRescueDailyMlPure` : une valeur **explicitement fournie** (saisie utilisateur, ou toute valeur déjà résolue) n'est **que bornée** — plancher **`kRescueChlorineDailyMinMl` (100 mL/j, FIGÉ)** appliqué **en premier**, puis plafond `min(userDailyLimitMl si > 0, hardCapMl)`. **10 mL saisis redeviennent 100, jamais remplacés par un défaut** (plan de test manuel #3 de la spec). ⚠️ **La limite journalière de l'utilisateur reste SOUVERAINE même sous le plancher de 100** : si elle est configurée à 60 mL/j, le volume armé sera 60, pas 100 — motif chimique posé par le gate, à citer tel quel : *« une limite journalière est une borne d'EXPOSITION, le plancher de 100 une borne d'EFFICACITÉ ; refuser l'armement serait pire, car on laisserait la boucle ORP doser sur une mesure reconnue non fiable, sans plafond de maintien »*. C'est le **seul** cas où le mode s'arme sous le plancher d'efficacité **sans toast côté UI** (saisie et valeur retenue étant déjà égales) : un `systemLogger.warning()` dédié comble cet angle mort (`startRescueChlorine()`, [`config.cpp`](../../src/config.cpp)).
- `resolveRescueDailyMlPure` : une valeur **absente ou hors bornes** (`storedMl` non dans `[kRescueChlorineDailyMinMl, hardCapMl]` — stockage NVS corrompu, ou aucune valeur fournie à l'activation) est **remplacée** par la **chaîne de pré-remplissage**, puis passée par le **même** `clampRescueDailyMlPure` (zéro duplication du bornage) : `orpDailyTargetMl` (> 0) → `sizeDefaultMl` (> 0, sinon étage **sauté**) → `fallbackMl` (**jamais 0** — condition bloquante #2 du gate : *« un secours armé à 0 mL est un mode qui ment »*).

**Ce clamp de saisie est DISTINCT des clamps C3/C4** de `effectiveOrpDailyTargetMlPure` (section ci-dessus) : il s'applique **une seule fois, à l'armement**, et ne voit **jamais** le Boost. Les deux ordres de clamp ne peuvent pas être fusionnés sans rouvrir C4 : `1900` mL saisis (déjà clampés à l'armement) + Boost (`× 1,5 = 2850`) doivent rester plafonnés à **2000** par C3/C4, jamais retomber à 100 par le clamp de saisie.

**Chaîne de pré-remplissage côté HTTP** (dialogue d'activation web, `rescueDailyMlForDisplay()` dans [`config.cpp`](../../src/config.cpp), **seul lecteur** de `kRescueChlorineDailyBySizeMl` dans tout le firmware) : `orp_daily_target_ml` (NVS, mode Programmée) si renseigné → **défaut par taille de bassin** (table séparée ci-dessous) → `kRescueChlorineDailyMl` en repli final. Utilisée par `loadRescueState()` au boot (stockage absent/hors bornes) et exposée **à parité stricte** `/get-config` ↔ trame WS `config` (champ `rescue_ml`, voir [API.md](../API.md#get-get-config--write) / [ws-manager.md](ws-manager.md)).

⚠️ **Défauts par taille — table SÉPARÉE de `kRegulationDefaultsBySize` à DESSEIN** : le visa `pool-chemistry` du 31/08/2026 sur cette dernière interdit explicitement tout **quota** de dosage ([`regulation_defaults.h`](../../src/regulation_defaults.h)). Un volume journalier de secours **est** un quota — l'étendre aurait violé cet arbitrage. `kRescueChlorineDailyBySizeMl[3]` (constants.h, indexé par la sérialisation de `PoolSize`) :

| Taille | Volume de référence | Défaut |
|---|---|---|
| Petit | ≈ 20 m³ | 500 mL/j |
| Moyen | ≈ 40 m³ | 1000 mL/j |
| Grand | ≈ 60 m³ et plus | 1500 mL/j |

Base : 3 mg/L/jour de Cl₂ (haut de la demande réelle mesurée), chlore 36° ≈ 114 g Cl₂/L ⇒ ≈ 26 mL/m³/jour. Ces défauts passent par le **même clamp C3/C4** que tout le reste une fois armés.

**Chemin MQTT `ON` sans volume — invariant feature-090 préservé** : [`src/regulation_defaults.h:18-19`](../../src/regulation_defaults.h) pose que `PoolSize` « n'entre JAMAIS dans un calcul de dosage, de débit, de quota ou de correction ». Le pré-remplissage HTTP respecte la lettre de cet invariant (l'utilisateur **voit** et peut modifier la valeur avant de valider) — mais un `ON` MQTT arme le mode **sans qu'un humain ne voie ni ne modifie rien**. `MqttManager::drainCommandQueue()` appelle donc `startRescueChlorine(0.0f)`, qui résout **sans étage taille** : `orp_daily_target_ml` (si > 0) sinon `kRescueChlorineDailyMl` directement — `PoolSize` n'est **jamais consultée** sur ce chemin. **Pas d'héritage** de la dernière valeur saisie (arbitrage tranché le 2026-10-05 : le cas protégé était trop étroit pour justifier une seconde clé NVS de fraîcheur).

**Log d'armement, provenance** : `startRescueChlorine()` nomme désormais le volume **et sa provenance** — « saisi » (route HTTP `?ml=`), « cible programmée » (`orp_daily_target_ml` > 0) ou « défaut » (repli `kRescueChlorineDailyMl`). Avant la v2.56.0, le log donnait le volume sans dire d'où il venait — impossible de savoir après coup avec quoi le mode avait tourné.

**Annonce côté Home Assistant** : le switch `{base}/rescue` reste binaire (`ON`/`OFF`), aucun champ numérique ne peut y transiter sans changer son type d'entité HA (hors périmètre). L'exigence d'annonce du gate (l'invisibilité du volume est **spécifiquement** un problème Home Assistant) se résout par un **topic dédié** `{base}/rescue_ml` (sensor, lecture seule, auto-discovery HA) — voir [MQTT.md](../MQTT.md#topic-et-entité-ajoutées-en-feature-141-volume-de-secours-saisi-v2560). **Rien n'est ajouté à la trame `sensor_data`** pour cela : sa marge était à 39 o après `rescue_lift`, et le bandeau `#rescue-active-banner` porte déjà l'information pour qui regarde l'interface web.

### `RescueLiftReason` — trois fins distinctes, une seule clé publiée

```cpp
enum class RescueLiftReason : uint8_t {
  None = 0, AutoLift = 1, Expired48h = 2, Manual = 3,
};
```

Publiée en `uint8_t` (`rescue_lift`, parité `/data` ↔ WS ↔ MQTT `{base}/rescue_lift_reason`), **RAM uniquement, jamais persistée** — un reboot n'a, par construction, aucune « fin récente » à rapporter. Fenêtre de **visibilité glissante** `kRescueLiftReasonVisibleMs` (90 s — 3× la cadence `/data`) portée par `getRescueLiftReasonCode(nowMs)`, qui délègue à la fonction pure `rescueLiftReasonVisiblePure()` (arithmétique non signée, wrap-safe au repliement de `millis()`). Motif de la distinction : le libellé **L6** (« les mesures ORP sont de nouveau fiables ») n'est **vrai** que pour la levée automatique — l'afficher sur une expiration 48 h mentirait sur l'état réel des sondes. Le gate a exigé un libellé **séparé** pour l'expiration (**L9**, voir [API.md](../API.md#champs-ajoutés-en-feature-140-chloration-de-secours)), ton `warning`, qui rappelle que la régulation automatique a **déjà repris** et invite à contrôler le chlore libre au réactif DPD **et** le pH avant de lui faire confiance — indépendamment de la sonde qu'on soupçonne déjà.

### Constantes (feature-140 / feature-141, `constants.h`)

| Constante | Valeur | Rôle |
|---|---:|---|
| `kRescueChlorineDurationS` | 172 800 s (48 h) | Durée glissante depuis l'activation (epoch, pas minuit) |
| `kRescueChlorineDailyMl` | 1500 mL/j | **feature-141** : n'est plus LA valeur — défaut de pré-remplissage « grand bassin » et repli final de la chaîne. Rationale terrain inchangée : maintien, jamais rattrapage (2000 mL/j × 3 j n'ont pas fait bouger l'ORP, bloqué à 261 mV pour une consigne de 530) |
| `kRescueChlorineDailyHardCapMl` | 2000 mL/j | Plafond dur absolu, appliqué **après** l'amplification Boost — inchangé par feature-141 |
| `kRescueChlorineDailyMinMl` (feature-141) | 100 mL/j | Plancher ABSOLU du volume **saisi** — FIGÉ, pas un paramètre (verdict `pool-chemistry` : « un secours à 20 mL/j donnerait l'illusion d'une désinfection sans la fournir ») |
| `kRescueChlorineDailyBySizeMl[3]` (feature-141) | 500 / 1000 / 1500 mL/j | Défauts de **pré-remplissage** par taille de bassin — table séparée de `kRegulationDefaultsBySize` (visa pool-chemistry 31/08 : jamais de quota dans cette dernière) |
| `kRescueChlorineAutoLiftHoldMin` / `…HoldMs` | 30 min | Tenue continue de mesures fiables avant levée automatique |
| `kRescueLiftReasonVisibleMs` | 90 000 ms | Fenêtre de visibilité de `rescue_lift` (3× la cadence `/data`) |

### Testabilité native

`effectiveOrpModePure`, `rescueActivePure`, `rescueDeadlinePure`, `effectiveOrpDailyTargetMlPure`, `rescueLiftReasonVisiblePure`, et depuis la v2.56.0 `clampRescueDailyMlPure` / `resolveRescueDailyMlPure` ([`src/temp_mode_logic.h`](../../src/temp_mode_logic.h) / [`.cpp`](../../src/temp_mode_logic.cpp)) sont couvertes par la suite native dédiée `test/test_native_temp_mode/` : mode effectif (Manuel prioritaire, secours bascule vers Scheduled, non-régression sans secours), composition Boost × secours et **ordre des clamps** (mutation dédiée : inverser l'ordre amplification/plafond dur doit tuer un test), fail-closed sans heure valide, fenêtre de visibilité de `rescue_lift` avec repliement de `millis()`, **plancher/plafond du volume saisi, chaîne de pré-remplissage à trois étages, `storedMl` absent/nul/hors bornes, et le cas longtemps manquant depuis feature-140 : 1900 mL saisis + Boost (`× 1,5`) ⇒ volume effectif plafonné à 2000, jamais 2850**. Validation par **mutation** (ADR obligatoire du workflow) : suppression du plancher, repli sur 0 au lieu de la chaîne, et déplacement du clamp du plafond dur font chacun mourir un test dédié. Voir [BUILD.md](../BUILD.md) pour `pio test -e native` — ne pas recopier de compteur de tests ici (ADR-0073), le mesurer avec `ls -d test/test_native_* | wc -l` et la dernière ligne de `pio test -e native`.

## Restauration des valeurs d'usine de régulation (feature-088, v2.31.0)

> ℹ️ Le code vit dans le sous-système **config** (`restoreRegulationDefaults()`, [`config.cpp`](../../src/config.cpp) ; déclaration [`config.h`](../../src/config.h)) — documenté ici parce que son objet est **exclusivement** le tuning de la régulation pH/ORP et que le périmètre a été arbitré par `pool-chemistry`.

Déclenchée par [`POST /reset-regulation-defaults`](../API.md#post-reset-regulation-defaults--critical) (bouton de la [page Paramètres](../features/page-settings.md#bouton--restaurer-les-valeurs-par-défaut--feature-088-v2310)). Remet **7 champs de tuning** de `MqttConfig` aux valeurs d'usine (8 avant la v2.34.0 — `stabilizationDelayMin` retiré du périmètre par la feature-091), puis persiste (`saveMqttConfig()`). **v2.32.0 (feature-090)** : les valeurs d'usine sont désormais **par taille de bassin** — la restauration applique la colonne de la taille **courante** (`mqttCfg.poolSize`, la taille elle-même n'est pas modifiée).

### `regulation_defaults.h` — source UNIQUE des valeurs d'usine, PAR TAILLE (feature-090, v2.32.0)

[`src/regulation_defaults.h`](../../src/regulation_defaults.h) est le **seul** endroit où les valeurs d'usine de tuning existent — depuis la v2.32.0 sous forme de **table à trois colonnes** `kRegulationDefaultsBySize[3]`, indexée par `enum class PoolSize : uint8_t { Small = 0, Medium = 1, Large = 2 }` (sérialisation stable `"small"`/`"medium"`/`"large"`, `poolSizeTo/FromString()` — valeur inconnue ou NVS corrompue ⇒ **Medium**, fail-safe de migration) :

> **v2.34.0 (feature-091)** : la table ne porte plus que **7 réglages** — la ligne `stabilizationDelayMin` a disparu de `struct RegulationDefaults` (la purge des mesures n'est plus réglable). L'alias `kDefaultStabilizationDelayMin` (= 5) subsiste **uniquement** comme défaut de lecture NVS du champ inerte.

| Réglage | Small (< 20 m³) | **Medium (20-60 m³)** | Large (> 60 m³) |
|---|---|---|---|
| `phInjectionLimitMin` / `orpInjectionLimitMin` | 5 / 5 min/h | **10 / 10 min/h** | 15 / 15 min/h |
| `regulationSpeed` | slow | **normal** | fast |
| `phTarget` / `orpTarget` | 7,3 / 600 mV | 7,3 / 600 mV | 7,3 / 600 mV |
| `ph/orpDailyTargetMl` | 0 | 0 | 0 |

**Statut** : Medium = **mesures** du bassin de référence (v2.30.0, visa pool-chemistry du 31/08/2026) ; Small et Large = **EXTRAPOLATIONS** étiquetées (excursion de mélange aveugle ~constante à travers les tailles, voir [ADR-0057](../adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)). Les `kDefault*` historiques (088) subsistent comme **alias de la colonne Medium** — les initialiseurs de `MqttConfig` les référencent, garantie qu'une mise à jour seule ne modifie rien. Consommateurs de la table : initialiseurs de `MqttConfig` ([`config.h`](../../src/config.h)), `restoreRegulationDefaults()`, le bloc `pool_size` de `/save-config` et la sérialisation `fillRegulationDefaults(o, size)` ([`web_routes_config.cpp`](../../src/web_routes_config.cpp) — objets `regulation_defaults` [colonne courante] et `regulation_defaults_by_size` [les trois] de `/get-config`, réponse de la route). Au prochain recalibrage, **une seule colonne change et rien ne peut diverger**.

- **Header PUR** (n'inclut que `constants.h` et `<string.h>`) : compilable en `env:native` ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).
- **Verrous compile-time** (conditions du visa, généralisés par la 090) : `static_assert` de **bornes chimiques par colonne** (`regulationDefaultsColumnValid()` : consignes ∈ fenêtres projet, limites horaires ∈ [1 ; 60], volumes ≥ 0, vitesse = libellé connu), de **monotonie** (limites d'injection et rang de vitesse **croissants** avec la taille) et **VERROU DE MIGRATION** : la colonne Medium est comparée aux **littéraux v2.30.0 en dur** (10/10, normal, 7,3, 600, 0/0) — la retoucher exige de retoucher aussi l'assert, en connaissance de cause (nouveau visa pool-chemistry requis). Un recalibrage hors fenêtre **casse la compilation**.
- **`applyRegulationDefaults<Cfg>(c, size)`** — template pur qui **EST la liste des champs copiés** : `phTarget`, `orpTarget`, `ph/orpInjectionLimitMinutes`, `ph/orpDailyTargetMl`, `regulationSpeed`, depuis la colonne de `size` (**7 champs** depuis la v2.34.0). **`poolSize` n'est PAS copié** (la taille est l'entrée de la sélection, pas un réglage restauré). Template plutôt que fonction sur `MqttConfig` (qui dépend d'`Arduino.h`) : les tests natifs ([`test/test_native_regulation_defaults/`](../../test/test_native_regulation_defaults/test_regulation_defaults.cpp)) l'instancient sur un **double portant tous les champs** de `MqttConfig` et prouvent **par mutation** que rien d'autre ne bouge.
- **`regulationDefaultsRaiseDosing(target, curOrpTarget, curPhLimit, curOrpLimit)`** — prédicat pur du niveau de journal, **paramétré par la colonne cible** depuis la 090 : l'application de `target` **augmente-t-elle la capacité de dosage** par rapport à l'état courant ? Oui si la consigne ORP **monte** ou si une limite horaire d'injection **monte** (restauration sur un réglage abaissé à la main, OU passage à une taille **supérieure** : 5 → 10 → 15 min/h) → journal en **`warning`** avec mention `(dosage AUGMENTÉ)`, sinon `info`. Partagé entre la restauration 088 et le changement de taille 090.

### `restoreRegulationDefaults()` — transport (mutex, NVS, journal)

```cpp
bool restoreRegulationDefaults();  // false = timeout mutex, RIEN modifié
```

1. **Prise bornée de `configMutex`** (`kConfigMutexTimeoutMs`, miroir de `saveMqttConfig`). Timeout → **restauration annulée en bloc** (aucun champ modifié, `error` journalisé) et retour `false` — la route répond alors **503 `success:false`** (correctif de revue v2.31.0 : la première version répondait un faux succès).
2. Instantané **avant** des 8 champs, puis `applyRegulationDefaults(mqttCfg, mqttCfg.poolSize)` (colonne de la taille **courante** depuis la v2.32.0) et `saveMqttConfig()` dans la **même section critique** (mutex récursif).
3. **Journal compact en UN message** : les 8 champs **avant → après** (un champ inchangé s'y lit `x → x` — l'état complet d'avant restauration reste reconstituable depuis le journal, leçon `phAutoManual`). Niveau `warning` + mention `(dosage AUGMENTÉ)` si `regulationDefaultsRaiseDosing()`, `info` sinon.

**Ce qui n'est JAMAIS touché** (invariants du visa, prouvés par mutation) : modes de régulation, `ph/orpEnabled`, `phCorrectionType` (le bidon physiquement branché), `phOffset`, trace `phAutoManual*`, calibration (`pumpN_*`) et sélection des pompes, `SafetyLimits` (limites journalières), `maxCyclesPerDay`. Ces champs décrivent un **état physique** ou une **sécurité**, pas un réglage.

**Après restauration** : la route déclenche le **retune PID différé** existant (`applyRegulationSpeed()` pose le drapeau atomique `_phTuningChangeRequested`, consommé par `update()` dans loopTask — aucune écriture PID depuis le handler HTTP, invariant v2.26.0 ; un changement **effectif** de vitesse purge l'intégrale, [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) et le broadcast WS `config` différé ([ws-manager.md](ws-manager.md#feature-088-v2310--restauration-des-défauts-de-régulation--aucun-champ-nouveau)).

## Interaction avec les autres composants

| Composant | Interaction |
|-----------|-------------|
| [`filtration`](filtration.md) | Arrêt filtration → `clearStabilizationTimer()` (l'arming au **démarrage** est retiré en v2.34.0 : la purge adaptative de [`measure_validity`](measure-validity.md) le remplace). **Boost** : `boostForce` prioritaire dans `decideFiltrationRun` force la marche tant que `isBoostActive()` (feature-053) |
| [`sensors`](sensors.md) | Lecture de la **valeur officielle** `getPhFiltered()` / `getOrpFiltered()` pour l'erreur PID (+ `isnan()` dans `canDose`, garde #4 seule depuis feature-152). Brut `getPh()`/`getOrp()` pour logs uniquement |
| [`mqtt_manager`](mqtt-manager.md) | Publication `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph/orp_mixing_delay_active`, `boost` (switch HA « Boost », feature-053) ; commande `{base}/boost/set` ; `rescue` + `rescue_lift_reason` (switch HA « Chloration de secours », feature-140) ; commande `{base}/rescue/set` |
| [`ota_manager`](ota-manager.md) | `setOtaInProgress(true)` arrête toutes les pompes |
| [`web_routes_control`](web-server.md) | `/ph/inject/start`, `/orp/inject/start`, `/pump[12]/on` — gardes feature-006 : lecture `getCyclesToday`/`getRecentCycles`/`getStabilizationRemainingS(i)`/`get*UsedMs`, enregistrement via `requestManualCycleRecord` |

## État persistant (NVS)

Voir [`pump_controller.cpp`](../../src/pump_controller.cpp) `savePersistentDailyState()` / `loadPersistentDailyState()` :
- `phDailyInjected`, `orpDailyInjected` (float, mL)
- `phLimitReached`, `orpLimitReached` (bool)
- `currentDayDate` (YYYYMMDD, char[9])
- `phDosingCycles`, `orpDosingCycles` (uint16)
- **v2.27.0** — `filt_min` (uint32) : **minutes de circulation effectives** de la journée courante, **dénominateur** de la charge acide observée. Persisté dans la **même** transaction que les volumes, pour que le rapport mL/min survive à une coupure. **Remis à zéro à chaque minuit.**
- **v2.34.0** — `filt_total` (uint32, `SafetyLimits::filtTotalMin`) : compteur **MONOTONE** de minutes de circulation cumulées, **jamais** remis à zéro au changement de date. C'est la base de temps de la fraîcheur de l'instantané d'intégrale pH ([ADR-0060](../adr/0060-fraicheur-en-temps-de-filtration.md)) — écrit dans la même transaction que les compteurs journaliers. Un compteur qui **recule** (NVS effacée, restauration d'usine) fait rejeter l'instantané, jamais réinterpréter son âge.

Flush différé : `_dailyCountersDirty` armé à chaque injection, écrit en NVS au max toutes les 60 s pour réduire la charge d'écriture. **v2.27.0** : le drapeau est désormais **aussi** posé à chaque minute de circulation, le flush périodique est **remonté en amont de toutes les sorties anticipées** d'`update()`, et les compteurs sont explicitement posés à l'entrée en maintenance ainsi qu'avant les redémarrages OTA / mode AP / web / MQTT. Coût assumé : **~420-600 transactions/jour contre ~70-100** — voir [Usure NVS](#usure-nvs--un-facteur-5-à-6-assumé).

**v2.27.0 — deux structures NVS supplémentaires** ([ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md)) :

| Structure | Emplacement | Taille | Cadence d'écriture |
|---|---|---:|---|
| `PhIntegralSnapshot` (intégrale pH + schéma + contexte + horodatage + **compteur de filtration cumulé** + compteur anti-boucle + empreinte) | namespace **dédié** `pool-phint`, clé `snap` | **24 o** (schéma 2, v2.34.0 ; 20 o en schéma 1) | `kDelta` : ≤ 1 / 30 min **ET** > 5 % du plafond (~2/jour) — **plus** `kDue` toutes les 180 min de filtration et `kRewrite` (une fois) ⇒ ~3 écritures/jour de plus |
| `PhLoadHistory` (anneau de 7 journées `{date, mL, minutes}` + schéma + empreinte) | namespace existant `pool-daily`, clé `ph_load7` | 64 o | **1 / jour**, au basculement de date |

Aucune de ces deux fonctions ne prend `configMutex` : la charge utile appartient exclusivement à `loopTask` — le mutex ne protégerait rien tout en introduisant une prise de verrou dans `update()`.

**v2.45.0 (feature-110)** — les six compteurs de démarrages et d'hygiène (`ph_starts`, `ph_anom`, `ph_anom_ml`, `orp_starts`, `orp_anom`, `orp_anom_ml`) vivent dans le namespace **`pool_prod`**, avec le volume injecté, et suivent son flush **différé de 60 s** (`productConfigDirty`) — **aucune transaction NVS supplémentaire**. Voir [Comptage des démarrages de pompe](#comptage-des-démarrages-de-pompe-feature-110-v2450).

Hors compteurs journaliers, la **trace de bascule automatique pH** est persistée par `saveMqttConfig()` (namespace de configuration, pas ce flush) : `ph_am_epoch` + `ph_am_reason`, v2.20.3 — voir [Persistance de la bascule automatique en manuel](#persistance-de-la-bascule-automatique-en-manuel-v2203).

## Cas limites

- **Démarrage sans NTP** : `_dailyLoaded = false` tant que `kMinValidEpoch` n'est pas atteint → cumul NVS chargé **après** synchro temps, sinon on risque de reset le cumul sur une date de boot fantôme (1970).
- **OTA en cours** : `setOtaInProgress(true)` appelé par `ota_manager` → `stopAll()` immédiat, pompes coupées jusqu'à fin OTA ou reboot.
- **Changement `ph_correction_type`** (pH- ↔ pH+) : `resetPhPauseGuard()` appelé depuis [`web_routes_config.cpp`](../../src/web_routes_config.cpp) pour repartir propre.

## Fichiers liés

- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
- [`src/config.h:152`](../../src/config.h:152) — `PumpProtection`
- [`src/config.h:141`](../../src/config.h:141) — `SafetyLimits`
- [`src/constants.h`](../../src/constants.h) — `kPulsePeriodMs`, `kPulseMinMs`, `kPulseKickMs`, `kPumpAmpMinPct`, `kPulseFlowMaxMlPerMin`, `kPulseRefusalLogIntervalMs` (v2.21.0), `kPumpFlow255MlPerMin` + `kPumpFlow255MeasuredMaxMlPerMin` et `kPumpTypeFlowMeasuredMaxMlPerMin` (v2.25.2, ADR-0050 — remplacent `kPump1/2Flow255MlPerMin`)
- [`src/ph_integral_logic.h`](../../src/ph_integral_logic.h) — **terme intégral pH pur** (v2.26.0, [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) : charge signée, oubli, reset sec **temporisé** (v2.27.0), péremption, hystérésis de saturation, `phSustainableAvgFlow()`
- [`src/ph_persist_logic.h`](../../src/ph_persist_logic.h) — **persistance de l'intégrale pH, pure** (v2.27.0, [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) / [ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md)) : scellement et empreinte FNV-1a, empreinte de contexte, plafond R1, péremption R3, machine de confirmation R4, cadence R5, anti-boucle R7, anneau de charge sur 7 jours, fenêtre d'âge calendaire, filet de vraisemblance, médiane
- [`src/constants.h`](../../src/constants.h) — section « RÉGULATION pH — TERME INTÉGRAL » (v2.26.0) : `kPhIntegralAuthorityMlPerMin`, `kPhIntegralTi*`, `kPhKp*`, `kPhKi*`, `kPhIntegralMax*`, `kPhIntegralResetMargin`, `kPhIntegralDecay*`, `kPhIntegralMaxTickDtMs`, `kPhIntegralChargeQuantumMs`, `kPhIntegralFreezeDailyFraction`, `kPhIntegralStaleResetMs`, `kPhIntegralSaturation*` + les 9 `static_assert` ; section « PERSISTANCE NVS de l'intégrale pH » (v2.27.0) : `kPhIntegralResetHoldMs`, `kPhIntegralRestore*`, `kPhIntegralPersist*`, `kPhLoad*`, `kPhEquilibriumLoadMlPerMin`, `kPhIntegralDuty*` + les `static_assert` (vii bis-a) et (x) à (xiv)
- [`src/config.h`](../../src/config.h) / [`src/config.cpp`](../../src/config.cpp) — transport NVS de l'instantané et de l'anneau (`loadPhIntegralSnapshot`, `savePhIntegralSnapshot`, `loadPhLoadHistory`, `savePhLoadHistory`, `recordPhLoadDay`, `readPhLoadObservation`), `phSustainableFlowFromConfig()` (**source unique** du débit soutenable) et `SafetyLimits::dailyFiltrationMinutes`
- [`src/pulse_dosing_logic.h`](../../src/pulse_dosing_logic.h), [`src/pulse_dosing_logic.cpp`](../../src/pulse_dosing_logic.cpp) — ordonnanceur d'impulsions pur (gate, plan, tick, volume — v2.21.0 ; accumulateur sous-plancher `planPulseWithCarry` — v2.22.0)
- [`src/acct_window_logic.h`](../../src/acct_window_logic.h) — **fenêtre de comptabilité pompe, pure** (bug-115, v2.61.0, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) : `evaluateAcctWindow()`, `nextOn = onNow` toujours
- [`test/test_native_acct_window/`](../../test/test_native_acct_window/) — suite native du module ci-dessus (bug-115)
- [`tools/check_acct_settle_single_point.sh`](../../tools/check_acct_settle_single_point.sh) — garde CI : `accountPumpVolume()` n'a qu'un appelant, et le wrapper d'`update()` conserve ses trois lignes (bug-115, v2.61.0)
- [`src/constants.h`](../../src/constants.h) — `kPumpPhPin = 25`, `kPumpOrpPin = 33` (PCB v2, voir ADR-0012)
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — décision de dosage pure (feature-036) + gardes d'injection manuelle `evaluateManualInject` (feature-006) + répartition scheduled `evaluateScheduledDose` (feature-011)
- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — `remainingRangeMinutes` (horizon de répartition scheduled, feature-011)
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md), [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md), [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md), [ADR-0014](../adr/0014-migration-atlas-ezo.md) (refonte `canDose`), [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) (régulation P temporisée, feature-025), [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) (logique pure Humble Object, feature-036), [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md) (budget horaire unique auto + manuel, feature-006), [ADR-0021](../adr/0021-repartition-scheduled.md) (répartition scheduled par fenêtres de 15 min, feature-011), [ADR-0025](../adr/0025-mode-boost.md) (Mode Boost — surcouche « valeurs effectives » + relèvement borné de la limite chlore, feature-053 ; **Superseded by ADR-0093**), [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md) (le mode de filtration est la seule autorité — le Boost devient un modificateur de créneau, disponible partout sauf en mode Arrêt, feature-146, v2.63.0), [ADR-0026](../adr/0026-mode-installation.md) (Mode d'installation — 3 archétypes de câblage + résolution unique de la présence d'eau, feature-056), [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) (offset pH en sortie des accesseurs + filets sur le brut, feature-067), [ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md) (proportionnel de temps à amplitude calibrée, v2.21.0), [ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) (budget horaire et temps min en temps pompe-ON, v2.21.0), [ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md) (accumulateur sous le pouls minimal, v2.22.0), [ADR-0047](../adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md) (calibration de type par défaut + gel de l'amplitude, v2.23.0), [ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md) (débit à 255 = constante unique de flotte, marge ≥ dispersion mesurée, v2.25.2), [ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md) (pouls minimal 5 s → 10 s : rendement des démarrages d'abord, correction du comptage ensuite, v2.25.3), [ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md) (**levée de `Ki = 0` sur le seul pH**, `integralMax` dimensionné par l'autorité, intégration signée sur le seuil d'arrêt, v2.26.0 — amende l'ADR-0016), [ADR-0053](../adr/0053-persistance-conditionnelle-integrale-ph.md) (**persistance conditionnelle de l'intégrale pH** sous sept garanties R1-R7 — la valeur relue n'est jamais appliquée au démarrage ; amende la clause « aucune persistance NVS » de l'ADR-0052, v2.27.0), [ADR-0054](../adr/0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) (un seuil de vraisemblance dérivé d'un réglage configurable est une **fonction** de ce réglage, jamais une constante, v2.27.0), [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md) (**Chloration de secours** — couple mode ORP EFFECTIF / NVS publiée, non-exclusivité des surcouches temporaires, expiration epoch 48h disqualifiant minuit local, feature-140, v2.55.0)
- [feature-084](../../specs/features/done/feature-084-integrale-ph-cadence-et-memoire.md) — cadence du tick d'entretien, reset sec temporisé, persistance NVS conditionnelle, anneau de charge acide (v2.27.0)
- [feature-025](../../specs/features/done/feature-025-lissage-mesures-ph-orp-pid.md) — entrée filtrée, anti-windup, pause mélange, zone morte
- [feature-036](../../specs/features/done/feature-036-dosage-testable-decision-pure.md) — extraction de la décision de dosage en module pur testable
- [feature-039](../../specs/features/done/feature-039-anti-rafale-rollover-testable.md) — extraction de l'anti-rafale + déclencheurs de rollover en logique pure testable
- [feature-011](../../specs/features/done/feature-011-repartition-24h-programmee.md) — répartition du volume quotidien scheduled par fenêtres de 15 min
- [bug-dosage-fantome-duty-non-calibre](../../specs/features/done/bug-dosage-fantome-duty-non-calibre.md) — **spec ouverte** : défaut n°2 (double comptage) livré en v2.20.5 ; défaut n°1 (modèle duty↔débit) livré en **v2.21.0** (proportionnel de temps, [ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)/[ADR-0044](../adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)) ; défaut n°3 (UI commandé/effectif) = itération 3 restante
- [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md) — UI consommatrices
- [bug-115](../../specs/features/done/bug-115-etat-de-comptabilite-remanent-aux-sorties-anticipees.md) — rémanence de comptabilité aux sorties anticipées, corrigée en v2.61.0 par le point de passage unique `settlePumpAccounting()`
- [ADR-0091](../adr/0091-comptabilite-hors-du-corps-decisionnel-dupdate.md) — la comptabilité des volumes est hors du corps décisionnel d'`update()` (bug-115, v2.61.0)
