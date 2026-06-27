#include "dosing_logic.h"

// =============================================================================
// evaluateDose — décision PURE, ordre des gardes 2→15 identique à canDose().
// =============================================================================
// IMPORTANT (pool-chemistry, feature-036) : NE PAS réordonner, NE PAS fusionner,
// NE PAS court-circuiter les gardes. L'ordre et les seuils comparatifs doivent
// rester strictement identiques au canDose() d'origine (characterization refactor).
DoseDecision evaluateDose(const DoseInputs& in) {
  // 1. Watchdog actif (le plus critique).
  if (!in.watchdogActive) {
    return { false, DoseRefusal::WatchdogInactive };
  }

  // 2. Filtration en marche (sauf mode continu : eau alimentée 24/7).
  if (!in.continuousMode && !in.filtrationRunning) {
    return { false, DoseRefusal::FiltrationOff };
  }

  // 3. Lecture filtrée non NaN (stale / bus I²C dégradé → fail-closed).
  if (isnan(in.reading)) {
    return { false, DoseRefusal::ReadingNaN };
  }

  // 3b. Filtre capteur prêt (warmup / EZO injoignable → fail-closed).
  if (!in.filterReady) {
    return { false, DoseRefusal::FilterNotReady };
  }

  // 3c. Capteur non instable (rejets consécutifs → fail-closed).
  if (in.filterUnstable) {
    return { false, DoseRefusal::FilterUnstable };
  }

  // 4. Calibration suffisante (pH ≥ 2 points, ORP ≥ 1 ; -1 si injoignable → bloqué).
  if (in.calPoints < in.requiredPoints) {
    return { false, DoseRefusal::CalibrationInsufficient };
  }

  // 5. Pas de stabilisation post-calibration en cours.
  if (in.stabilizationActive) {
    return { false, DoseRefusal::StabilizationActive };
  }

  // 5b. Pas de pause mélange hydraulique active (gate indépendante).
  if (in.mixingActive) {
    return { false, DoseRefusal::MixingActive };
  }

  // 6. Mode régulation = automatic.
  if (!in.modeAutomatic) {
    return { false, DoseRefusal::ModeNotAutomatic };
  }

  // 7. Limite journalière non atteinte.
  if (in.dailyInjectedMl >= in.maxDailyMl) {
    return { false, DoseRefusal::DailyLimit };
  }

  // 8. Limite horaire non atteinte (0 = pas de limite).
  if (in.hourlyLimitMs > 0 && in.usedMs >= in.hourlyLimitMs) {
    return { false, DoseRefusal::HourlyLimit };
  }

  // 9. Anti-cycling : cyclesToday < maxCyclesPerDay.
  if (in.cyclesToday >= in.maxCyclesPerDay) {
    return { false, DoseRefusal::CyclesPerDay };
  }

  // 10. Anti-rafale court terme (fenêtre 1 min).
  if (in.cyclesLastMin >= in.maxCyclesPerMin) {
    return { false, DoseRefusal::BurstPerMinute };
  }

  // 10b. Anti-rafale court terme (fenêtre 15 min).
  if (in.cyclesLast15Min >= in.maxCyclesPer15Min) {
    return { false, DoseRefusal::BurstPer15Min };
  }

  // Toutes les gardes sont passées.
  return { true, DoseRefusal::None };
}

bool shouldStartDosingPure(float error, float startThreshold,
                           unsigned int cyclesToday, unsigned int maxCyclesPerDay) {
  // 1. Nombre de cycles par jour (le warning éventuel reste dans la coquille).
  if (cyclesToday >= maxCyclesPerDay) {
    return false;
  }
  // 2. Seuil de démarrage (hystérésis).
  return error > startThreshold;
}

bool shouldContinueDosingPure(float error, float stopThreshold,
                              unsigned long runTimeMs, unsigned long minInjectionTimeMs) {
  // 1. Forcer le temps minimum d'injection.
  if (runTimeMs < minInjectionTimeMs) {
    return true;
  }
  // 2. Continuer si au-dessus du seuil d'arrêt (hystérésis).
  return error > stopThreshold;
}
