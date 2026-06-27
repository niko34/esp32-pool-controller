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

#endif // DOSING_LOGIC_H
