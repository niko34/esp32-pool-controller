#ifndef DOSING_LOGIC_H
#define DOSING_LOGIC_H

// =============================================================================
// dosing_logic — Décision de dosage PURE (feature-036, characterization refactor)
// =============================================================================
// Logique de décision « peut-on doser ? » extraite de pump_controller pour la
// rendre testable en natif (sans ESP32, sans I²C, sans FreeRTOS).
//
// INVARIANT DIRECTEUR : ce module ne change AUCUN comportement de dosage. Il
// reproduit EXACTEMENT l'ordre des gardes 2→15 de canDose(), fail-closed strict.
// Toute la collecte des entrées (globals, I²C, millis(), historique anti-rafale)
// et le formatage français des causes de refus restent dans la coquille
// pump_controller.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <vector>/<FS.h>/<String>.
// On utilise les en-têtes C <stdint.h>/<math.h> (et non <cstdint>/<cmath>) : la
// libc++ n'est pas disponible sur l'hôte de CI/dev (cf. test/native_shim/Arduino.h),
// or le module doit compiler en natif. isnan() suffit pour le test NaN.
// =============================================================================

#include <stdint.h>
#include <math.h>
#include <string.h>

// =============================================================================
// Mode d'installation (feature-056) — décrit le câblage réel et pilote de façon
// cohérente : la présence d'eau (garde dosage), le pilotage du relais filtration
// et l'horizon de répartition scheduled. Enum PUR partagé par config.h (aucun
// Arduino.h ici). Sérialisation JSON/WS/UART STABLE (ne JAMAIS renuméroter) :
//   ManagedFiltration   -> "managed"   (ancien "pilote" + filtrationCfg.enabled)
//   PoweredByFiltration -> "powered"   (ancien "continu")
//   ExternalFiltration  -> "external"  (NOUVEAU : filtration tierce signalée)
// =============================================================================
enum class InstallMode : uint8_t {
  ManagedFiltration = 0,    // PoolController pilote le relais filtration
  PoweredByFiltration = 1,  // PC alimenté par la phase filtration → eau présumée présente
  ExternalFiltration = 2    // filtration tierce, état signalé via HTTP/MQTT (fail-safe OFF)
};

// Source retenue pour la présence d'eau (diagnostic / WS).
enum class WaterSource : uint8_t {
  FiltrationCommanded,  // Managed : état commandé par PoolController
  PoweredAssumed,       // Powered : présumé par câblage
  ExternalSignal        // External : dernier signal externe
};

// Entrées POD de la résolution « eau présente ? ». Tous les champs collectés par
// la coquille (filtration.*) AVANT l'appel. age = (now - lastMs) non signé,
// calculé wrap-safe dans la coquille (jamais ici).
struct WaterPresenceInputs {
  InstallMode mode;
  bool filtrationCommandedOn;    // filtration.isRunning() (Managed)
  bool externalSignalOn;         // dernier état signalé (External)
  bool externalSignalKnown;      // au moins un signal reçu depuis le boot (External)
  uint32_t externalSignalAgeMs;  // âge du dernier signal externe (now - lastMs)
  uint32_t externalStaleMs;      // fraîcheur max tolérée (kExternalFiltrationStaleMs)
};

// Verdict de présence d'eau. `stale` : mode External, signal connu mais périmé.
struct WaterPresence {
  bool waterPresent;
  WaterSource source;
  bool stale;
};

// Résout la présence d'eau selon le mode d'installation. Fail-closed strict
// (condition pool-chemistry #2 : doute = refus).
//   Managed  → waterPresent = filtrationCommandedOn ;
//   Powered  → waterPresent = true (eau présumée présente) ;
//   External → waterPresent = externalSignalOn && externalSignalKnown &&
//              externalSignalAgeMs <= externalStaleMs (on teste `known` AVANT
//              l'âge), sinon false ;
//   mode inconnu → false.
WaterPresence resolveWaterPresent(const WaterPresenceInputs& in);

// Migration de l'ancien schéma (regulationMode + filtrationCfg.enabled) vers
// InstallMode : "continu" → PoweredByFiltration ; sinon → ManagedFiltration.
// Ne produit JAMAIS ExternalFiltration (mode nouveau, jamais migré).
InstallMode migrateInstallMode(const char* regMode, bool filtrationEnabled);

// Cause de refus de dosage (énum pur). L'ordre suit exactement l'ordre des
// gardes de canDose(). None = dosage autorisé.
enum class DoseRefusal {
  None,
  WatchdogInactive,        // watchdog inactif
  FiltrationOff,           // pas de présence d'eau (resolveWaterPresent, selon le mode d'installation)
  ReadingNaN,              // lecture pH/ORP filtrée indisponible (NaN)
  FilterNotReady,          // filtre capteur non prêt (warmup / EZO injoignable)
  FilterUnstable,          // capteur instable (rejets consécutifs)
  CalibrationInsufficient, // calibration insuffisante (cal=X, requis=Y)
  StabilizationActive,     // stabilisation post-calibration en cours
  MixingActive,            // pause mélange en cours
  ModeNotAutomatic,        // mode régulation != automatic (X)
  DailyLimit,              // limite journalière atteinte
  HourlyLimit,             // limite horaire atteinte
  CyclesPerDay,            // limite cycles/jour atteinte
  BurstPerMinute,          // anti-rafale : N cycles dans la dernière minute
  BurstPer15Min            // anti-rafale : N cycles dans les 15 dernières minutes
};

// Entrées POD de la décision de dosage. Tous les champs sont collectés par la
// coquille canDose() depuis les globals AVANT l'appel à evaluateDose().
struct DoseInputs {
  bool watchdogActive;          // esp_task_wdt_status(NULL) == ESP_OK
  bool waterPresent;            // feature-056 : source UNIQUE resolveWaterPresent()
                                // (Managed=commandé, Powered=présumé, External=signal frais)
  float reading;                // sensors.getPhFiltered() / getOrpFiltered()
  bool filterReady;             // isPhFilterReady() / isOrpFilterReady()
  bool filterUnstable;          // isPhFilterUnstable() / isOrpFilterUnstable()
  int calPoints;                // points de calibration cachés (-1 si EZO injoignable)
  int requiredPoints;           // 2 (pH) ou 1 (ORP)
  bool stabilizationActive;     // isStabilizationTimerActive(pumpIndex)
  bool mixingActive;            // isPhMixingDelayActive / isOrpMixingDelayActive
  bool modeAutomatic;           // phRegulationMode / orpRegulationMode == "automatic"
  float dailyInjectedMl;        // safetyLimits.dailyPhInjectedMl / dailyOrpInjectedMl
  float maxDailyMl;             // maxPhMlPerDay / maxChlorineMlPerDay
  unsigned long usedMs;         // phDosingState.usedMs / orpDosingState.usedMs
  unsigned long hourlyLimitMs;  // limitMin*60000 ; 0 = pas de limite horaire
  unsigned int cyclesToday;     // phDosingState.cyclesToday / orpDosingState.cyclesToday
  unsigned int maxCyclesPerDay; // pumpProtection.maxCyclesPerDay
  int cyclesLastMin;            // countRecentDosingCycles(pumpIndex, 60000)
  int maxCyclesPerMin;          // kMaxDosingCyclesPerMinute
  int cyclesLast15Min;          // countRecentDosingCycles(pumpIndex, 900000)
  int maxCyclesPer15Min;        // kMaxDosingCyclesPer15Min
};

// Verdict de dosage : allowed=true ssi cause==None.
struct DoseDecision {
  bool allowed;
  DoseRefusal cause;
};

// Reproduit EXACTEMENT l'ordre des gardes 2→15 de canDose() (la garde 1,
// pumpIndex invalide, reste dans la coquille). Première garde en échec →
// cause correspondante ; sinon { true, None }. Fail-closed strict.
DoseDecision evaluateDose(const DoseInputs& in);

// Hystérésis de démarrage (extrait pur de shouldStartDosing).
// true ssi cyclesToday < maxCyclesPerDay ET error > startThreshold.
bool shouldStartDosingPure(float error, float startThreshold,
                           unsigned int cyclesToday, unsigned int maxCyclesPerDay);

// Hystérésis de poursuite (extrait pur de shouldContinueDosing).
// true si runTimeMs < minInjectionTimeMs (force poursuite — temps minimum
// d'injection) OU error > stopThreshold.
bool shouldContinueDosingPure(float error, float stopThreshold,
                              unsigned long runTimeMs, unsigned long minInjectionTimeMs);

// Résultat pur d'un pas PID : débit final borné + nouvel état PID (intégrale,
// dernière erreur) renvoyés explicitement, jamais lus/écrits en global.
struct PidResult {
  float flow;       // débit FINAL borné [minFlow, maxFlow], 0 dans la zone morte / sortie négative
  float integral;   // intégrale après mise à jour anti-windup (inchangée si gelée ou deadband)
  float lastError;  // erreur courante (= error), à recopier dans pid.lastError
};

// Cœur PID PUR extrait de PumpControllerClass::computePID (feature-037,
// characterization refactor — AUCUN changement de comportement).
//
// Reproduit EXACTEMENT, dans cet ordre :
//   1. deadband STRICT : inDeadband = fabsf(error) < deadband ;
//   2. si inDeadband → { flow:0, integral inchangée, lastError:error } ;
//   3. sinon allowIntegration = !freezeIntegral → integral += error*dtSec puis
//      bornage ±integralMax (clamp haut ET bas) ; gelée si freezeIntegral ;
//   4. output = kp*error + ki*integral + kd*(error - prevError)/dtSec ;
//   5. if (output < 0) output = 0 ;
//   6. flow = clampf(output, minFlow, maxFlow) (bornage final, déplacé ici
//      depuis la coquille — feature-037 Option Y ; équivalent au constrain()
//      Arduino externe d'origine, réécrit en clampf() car constrain indispo en natif).
//
// L'état PID (integral, prevError) est passé en paramètre et renvoyé ; la
// coquille fournit dtSec (calculé depuis millis) et le flag freezeIntegral.
PidResult computePidPure(float kp, float ki, float kd,
                         float error, float prevError, float integral,
                         float dtSec, float integralMax, float deadband,
                         float minFlow, float maxFlow, bool freezeIntegral);

// =============================================================================
// Anti-rafale + rollover journalier PURS (feature-039, characterization refactor)
// =============================================================================
// Logique pure extraite de pump_controller (countRecentDosingCycles,
// recordDosingCycleStart, déclencheurs de tickDailyRollover). AUCUN changement
// de comportement : seuils/fenêtres/frontières strictement préservés. La
// coquille fournit millis()/time()/les buffers membres.

// Compte les démarrages de cycle dans la fenêtre [now-windowMs, now].
// Copie exacte de countRecentDosingCycles : slot 0 ignoré (jamais utilisé),
// comptage si (now - ts) <= windowMs.
// frontière <= inclusive volontaire ; wrap millis uint32 (arithmétique non
// signée → fenêtre cohérente au passage 0xFFFFFFFF) ; size doit valoir
// kDosingCycleHistorySize.
int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs);

// Écrit `now` au slot idx du ring buffer et renvoie le prochain index circulaire.
size_t recordCycleTimestamp(uint32_t* history, size_t idx, size_t size, uint32_t now);

// Déclencheur rollover par changement de date NTP : vrai ssi une date est déjà
// connue (non vide) ET diffère de la date du jour.
bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr);

// Déclencheur rollover fallback 24 h (heure non synchronisée).
// frontière >= inclusive volontaire ; wrap millis uint32 (arithmétique non signée).
bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now);

// =============================================================================
// Injection manuelle gardée — décision PURE (feature-006)
// =============================================================================
// Gardes de sécurité appliquées à `POST /ph|orp/inject/start` UNIQUEMENT.
// Les routes test `/pumpN/on` ne passent PAS par evaluateManualInject : elles
// ne sont protégées que par la garde filtration ; seul le budget horaire
// (usedMs, comptabilisé dans PumpController) couvre aussi ces pompes test.
// Le manuel est volontairement AVEUGLE à la mesure (pas de garde NaN/filtre/
// calibration/mode/mélange) : l'opérateur assume la décision chimique, mais ne
// peut pas dépasser le budget (journalier, horaire, cycles, anti-rafale) ni
// injecter sans eau / pendant une stabilisation post-calibration.
// Ordre des gardes et frontières validés pool-chemistry feature-006 (GO sous
// conditions) — NE PAS réordonner.

// Cause de refus d'injection manuelle. None = injection autorisée.
enum class ManualInjectRefusal {
  None,
  WatchdogInactive,     // watchdog inactif (règle absolue : aucun dosage sans watchdog)
  FiltrationOff,        // filtration arrêtée (hors mode continu — exemption côté collecte)
  StabilizationActive,  // stabilisation post-calibration en cours (par pompe)
  AlreadyInjecting,     // une injection manuelle est déjà en cours (double start)
  DailyLimit,           // cumul + volume demandé dépasserait la limite journalière
  HourlyLimit,          // usedMs + durée demandée dépasserait la limite horaire
  MaxCyclesPerDay,      // limite cycles/jour atteinte (compteur partagé avec l'auto)
  BurstPerMinute,       // anti-rafale : fenêtre 1 min (ring partagé avec l'auto)
  BurstPer15Min         // anti-rafale : fenêtre 15 min (ring partagé avec l'auto)
};

// Entrées POD collectées par la coquille (web_routes_control) AVANT l'appel.
struct ManualInjectInputs {
  bool watchdogActive;             // esp_task_wdt_status(NULL) == ESP_OK
  bool waterPresent;               // feature-056 : resolveWaterPresent() (source UNIQUE)
  bool stabilizationActive;        // isStabilizationTimerActive(pumpIndex)
  uint32_t stabilizationRemainingS;// secondes restantes (formatage 409 uniquement,
                                   // non utilisé par la décision — recopié tel quel)
  bool alreadyInjecting;           // injection manuelle déjà active sur cette pompe
  float requestedMl;               // volume demandé (déjà borné ≤ 2000 mL côté route)
  float dailyInjectedMl;           // cumul journalier (safetyLimits.daily*InjectedMl)
  float maxDailyMl;                // limite journalière ; ≤ 0 = illimité
  uint32_t usedMs;                 // budget horaire consommé (PARTAGÉ avec l'auto)
  uint32_t hourlyLimitMs;          // limite horaire ; 0 = illimité (convention auto)
  uint32_t requestedDurationMs;    // durée de l'injection demandée
  unsigned int cyclesToday;        // cycles du jour (auto + manuel, pending inclus)
  unsigned int maxCyclesPerDay;    // pumpProtection.maxCyclesPerDay
  int cyclesLastMin;               // getRecentCycles(pumpIndex, 60000)
  int maxCyclesPerMin;             // kMaxDosingCyclesPerMinute
  int cyclesLast15Min;             // getRecentCycles(pumpIndex, 900000)
  int maxCyclesPer15Min;           // kMaxDosingCyclesPer15Min
};

// Verdict : allowed=true ssi cause==None. remainingMl = reliquat journalier
// (max(0, maxDailyMl - dailyInjectedMl)) renseigné pour DailyLimit ET pour le
// cas autorisé ; 0 pour les autres causes de refus.
struct ManualInjectDecision {
  bool allowed;
  ManualInjectRefusal cause;
  float remainingMl;
};

// Évalue les gardes d'injection manuelle dans l'ORDRE EXACT validé
// pool-chemistry (condition #1 : watchdog EN PREMIER). Première garde en
// échec → cause correspondante. Fail-closed strict.
ManualInjectDecision evaluateManualInject(const ManualInjectInputs& in);

// =============================================================================
// Répartition scheduled — décision PURE (feature-011)
// =============================================================================
// Répartit le volume quotidien du mode "scheduled" (pH/ORP) par fenêtres de
// `windowMinutes` (kScheduledWindowMinutes = 15 en prod) sur l'horizon de
// filtration restant, borné à minuit. Le volume de fenêtre est RECALCULÉ à
// chaque nouvelle fenêtre depuis l'état courant (auto-correcteur : changement
// de cible en cours de journée, injection manuelle comptée dans
// dailyInjectedMl, retard subi par la limite horaire, redémarrage ESP32).
// Fail-closed strict.
//
// Conditions pool-chemistry feature-011 (bloquantes — NE PAS altérer) :
//   n°2 : horizonMinutes <= 0 → aucune injection (le plancher nWin >= 1 ne
//         s'applique qu'à un horizon strictement positif, jamais via max(1,0)) ;
//   n°3 : doseNow réévalué à CHAQUE tick contre min(stopTargetMl, cible
//         effective) — une baisse de cible/plafond en cours de fenêtre arrête
//         l'injection immédiatement ; remaining et v clampés >= 0 ;
//   n°4 : watchdogActive transmis en entrée et vérifié DANS la fonction pure ;
//   n°5 : le MÊME effectiveFlowMlPerMin sert au bornage horaire, à la durée
//         d'injection et au duty (la coquille fournit une seule variable).
// (Condition n°1 — consultation du ring anti-rafale avant tout démarrage —
// appliquée par la coquille pump_controller, qui possède le ring.)

// Entrées POD collectées par la coquille pump_controller AVANT l'appel.
struct ScheduledDoseInputs {
  int nowMin;                  // minutes locales depuis minuit (heure valide requise)
  int horizonMinutes;          // minutes restantes de la plage (remainingRangeMinutes)
                               // ou 1440-nowMin hors mode Managed ; <=0 → refus
  int windowMinutes;           // taille de fenêtre (kScheduledWindowMinutes = 15)
  float dailyTargetMl;         // mqttCfg.ph/orpDailyTargetMl (cible utilisateur)
  float maxDailyMl;            // plafond sécurité (maxPhMlPerDay / maxChlorineMlPerDay) ; <=0 = sans plafond
  float dailyInjectedMl;       // cumul journalier (auto + manuel, safetyLimits)
  float effectiveFlowMlPerMin; // débit effectif pompe (UNIQUE — condition n°5)
  uint32_t usedMs;             // budget horaire consommé (PARTAGÉ, ADR-0020)
  uint32_t hourlyLimitMs;      // limite horaire ; 0 = illimité (convention auto)
  uint32_t minInjectionTimeMs; // durée min d'injection (anti short-cycling, 30 s)
  bool watchdogActive;         // esp_task_wdt_status(NULL) == ESP_OK (condition n°4)
  int prevWindowIndex;         // index de fenêtre du tick précédent (-1 = aucun)
  float prevStopTargetMl;      // cible d'arrêt cumulée de la fenêtre courante
};

// Verdict de répartition. En refus fail-closed (watchdog/horizon/débit) :
// { false, -1, 0, NAN } — windowIndex=-1 force le recalcul au retour en plage.
struct ScheduledDoseDecision {
  bool doseNow;              // vrai ssi dailyInjectedMl < min(stopTargetMl, cible effective)
  int windowIndex;           // nowMin / windowMinutes (fenêtre absolue du jour)
  float stopTargetMl;        // cumul journalier cible d'arrêt de la fenêtre courante
  float plannedFlowMlPerMin; // débit moyen planifié restant (diagnostic WS) ; NAN si rien à injecter
};

// Décision de répartition scheduled (voir bloc de doc ci-dessus).
ScheduledDoseDecision evaluateScheduledDose(const ScheduledDoseInputs& in);

// =============================================================================
// Mode Boost — cible ORP et limite journalière effectives PURES (feature-053)
// =============================================================================
// Surcouche NON destructive : ces fonctions renvoient les valeurs *effectives*
// consommées par la régulation ORP quand le Boost est actif, sans jamais
// modifier la config persistée. L'effet chimique est STRICTEMENT réservé au mode
// ORP "automatic" (paramètre orpModeAutomatic) — le mode scheduled/manual ne voit
// JAMAIS la valeur boostée (condition pool-chemistry #1).

// Cible ORP effective (mV). Si boost actif ET mode automatic :
//   max(orpTarget, fminf(orpTarget + deltaMv, ceilingMv))
// Le max() protège une cible déjà supérieure au plafond : la cible n'est JAMAIS
// abaissée par le boost (condition pool-chemistry #5). Sinon → orpTarget inchangé.
float effectiveOrpTargetPure(float orpTarget, bool boostActive, bool orpModeAutomatic,
                             float deltaMv, float ceilingMv);

// Limite journalière chlore effective (mL). Si boost actif ET mode automatic :
//   fminf(maxChlorine * factor, hardCapMl)  (borné en dur, condition #1)
// Sinon → maxChlorine inchangée.
float effectiveMaxChlorinePure(float maxChlorine, bool boostActive, bool orpModeAutomatic,
                               float factor, float hardCapMl);

#endif // DOSING_LOGIC_H
