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

// Clamp maison : Arduino::constrain est une macro indisponible en natif.
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

PidResult computePidPure(float kp, float ki, float kd,
                         float error, float prevError, float integral,
                         float dtSec, float integralMax, float deadband,
                         float minFlow, float maxFlow, bool freezeIntegral) {
  // feature-025 (B7) : zone morte = seuil de démarrage (deadband). STRICT < :
  // |erreur| < deadband → sortie 0 ET aucune accumulation intégrale.
  bool inDeadband = (fabsf(error) < deadband);

  if (inDeadband) {
    // Sortie nulle dans la zone morte ; intégrale NON modifiée.
    return { 0.0f, integral, error };
  }

  // feature-025 (B6) : anti-windup strict — geler l'accumulation si la coquille
  // le signale (saturation, filtre non prêt, etc.) ou si on est dans la deadband
  // (déjà traité ci-dessus).
  bool allowIntegration = !freezeIntegral;

  // Calcul PID
  float proportional = kp * error;

  if (allowIntegration) {
    integral += error * dtSec;
    // Anti-windup (borne intégrale : clamp haut ET bas)
    if (integral > integralMax) integral = integralMax;
    if (integral < -integralMax) integral = -integralMax;
  }
  float integralTerm = ki * integral;

  float derivative = kd * (error - prevError) / dtSec;

  float output = proportional + integralTerm + derivative;
  if (output < 0.0f) output = 0.0f;

  // feature-037 (Option Y) : bornage final déplacé ici depuis la coquille.
  float flow = clampf(output, minFlow, maxFlow);

  return { flow, integral, error };
}
