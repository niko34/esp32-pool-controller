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

// Cause de refus de dosage (énum pur). L'ordre suit exactement l'ordre des
// gardes de canDose(). None = dosage autorisé.
enum class DoseRefusal {
  None,
  WatchdogInactive,        // watchdog inactif
  FiltrationOff,           // filtration arrêtée (hors mode continu)
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
  bool continuousMode;          // mqttCfg.regulationMode == "continu"
  bool filtrationRunning;       // filtration.isRunning()
  float reading;                // sensors.getPhFiltered() / getOrpFiltered()
  bool filterReady;             // isPhFilterReady() / isOrpFilterReady()
  bool filterUnstable;          // isPhFilterUnstable() / isOrpFilterUnstable()
  int calPoints;                // points de calibration cachés (-1 si EZO injoignable)
  int requiredPoints;           // 2 (pH) ou 1 (ORP)
  bool stabilizationActive;     // isStabilizationTimerActive(pumpIndex)
  bool mixingActive;            // isPhMixingDelayActive / isOrpMixingDelayActive
  bool modeAutomatic;           // phRegulationMode / orpRegulationMode == "automatic"
  float dailyInjectedMl;        // safetyLimits.dailyPhInjectedMl / dailyOrpInjectedMl
  float maxDailyMl;             // maxPhMinusMlPerDay / maxChlorineMlPerDay
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
  bool filtrationOk;               // filtration.isRunning() OU mode continu
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

#endif // DOSING_LOGIC_H
