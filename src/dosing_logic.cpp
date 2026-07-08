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

// =============================================================================
// Anti-rafale + rollover journalier PURS (feature-039)
// =============================================================================

int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs) {
  int count = 0;
  for (size_t i = 0; i < size; ++i) {
    uint32_t ts = history[i];
    // ts == 0 = slot vide (jamais utilisé). On ignore.
    // Cas wrap millis() (49.7 jours) : (now - ts) déborde correctement en
    // arithmétique non-signée → la fenêtre reste cohérente au passage 0xFFFFFFFF.
    // frontière <= inclusive volontaire.
    if (ts != 0 && (now - ts) <= windowMs) count++;
  }
  return count;
}

size_t recordCycleTimestamp(uint32_t* history, size_t idx, size_t size, uint32_t now) {
  history[idx] = now;
  return (idx + 1) % size;
}

bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr) {
  return strlen(currentDayDate) > 0 && strcmp(currentDayDate, todayStr) != 0;
}

bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now) {
  // frontière >= inclusive volontaire ; wrap millis uint32 (arithmétique non signée).
  return (now - dayStartMs) >= 86400000UL;
}

// =============================================================================
// evaluateManualInject — gardes d'injection manuelle PURES (feature-006)
// =============================================================================
// ORDRE EXACT validé pool-chemistry (GO sous conditions, condition #1 :
// watchdog EN PREMIER). NE PAS réordonner, NE PAS fusionner les gardes.
ManualInjectDecision evaluateManualInject(const ManualInjectInputs& in) {
  // Reliquat journalier informatif : plancher 0 (jamais négatif).
  float remaining = in.maxDailyMl - in.dailyInjectedMl;
  if (remaining < 0.0f) remaining = 0.0f;

  // 1. Watchdog actif (règle absolue : aucun dosage sans watchdog).
  if (!in.watchdogActive) {
    return { false, ManualInjectRefusal::WatchdogInactive, 0.0f };
  }

  // 2. Filtration en marche (présence d'eau ; l'exemption mode continu est
  // résolue côté collecte, avant l'appel).
  if (!in.filtrationOk) {
    return { false, ManualInjectRefusal::FiltrationOff, 0.0f };
  }

  // 3. Pas de stabilisation post-calibration en cours (par pompe).
  if (in.stabilizationActive) {
    return { false, ManualInjectRefusal::StabilizationActive, 0.0f };
  }

  // 4. Pas de double démarrage (une injection manuelle est déjà en cours).
  if (in.alreadyInjecting) {
    return { false, ManualInjectRefusal::AlreadyInjecting, 0.0f };
  }

  // 5. Limite journalière PRÉDICTIVE : refuser si le cumul + volume demandé
  // DÉPASSERAIT la limite. Frontière `cumul + volume == max` ACCEPTÉE
  // (STRICT >) — frontière figée pool-chemistry feature-006. maxDailyMl ≤ 0
  // = illimité (pas de garde journalière).
  if (in.maxDailyMl > 0.0f && (in.dailyInjectedMl + in.requestedMl > in.maxDailyMl)) {
    return { false, ManualInjectRefusal::DailyLimit, remaining };
  }

  // 6. Limite horaire PRÉDICTIVE : mêmes conventions que l'auto (0 = illimité) ;
  // frontière `usedMs + durée == limite` ACCEPTÉE (STRICT >) — figée
  // pool-chemistry feature-006. Budget usedMs PARTAGÉ avec l'auto.
  if (in.hourlyLimitMs > 0 && (in.usedMs + in.requestedDurationMs > in.hourlyLimitMs)) {
    return { false, ManualInjectRefusal::HourlyLimit, 0.0f };
  }

  // 7. Anti-cycling journalier : frontière >= (à la limite → refus), identique
  // à la garde 9 d'evaluateDose (compteur partagé auto + manuel).
  if (in.cyclesToday >= in.maxCyclesPerDay) {
    return { false, ManualInjectRefusal::MaxCyclesPerDay, 0.0f };
  }

  // 8. Anti-rafale fenêtre 1 min : frontière >= (identique à l'auto, ring partagé).
  if (in.cyclesLastMin >= in.maxCyclesPerMin) {
    return { false, ManualInjectRefusal::BurstPerMinute, 0.0f };
  }

  // 9. Anti-rafale fenêtre 15 min : frontière >= (identique à l'auto, ring partagé).
  if (in.cyclesLast15Min >= in.maxCyclesPer15Min) {
    return { false, ManualInjectRefusal::BurstPer15Min, 0.0f };
  }

  // Toutes les gardes sont passées : injection autorisée, reliquat renseigné.
  return { true, ManualInjectRefusal::None, remaining };
}

// =============================================================================
// evaluateScheduledDose — répartition scheduled PURE (feature-011)
// =============================================================================
// Conditions pool-chemistry n°2/3/4/5 documentées dans dosing_logic.h.
// NE PAS réordonner les gardes ni relâcher les clamps.
ScheduledDoseDecision evaluateScheduledDose(const ScheduledDoseInputs& in) {
  // Fail-closed (condition n°4 watchdog ; condition n°2 horizon) : watchdog
  // inactif, hors plage / horizon nul, débit non configuré, ou fenêtre invalide
  // (garde défensive anti division par zéro) → aucune injection.
  // windowIndex=-1 force le recalcul du volume au retour en plage.
  if (!in.watchdogActive || in.horizonMinutes <= 0 ||
      in.effectiveFlowMlPerMin <= 0.0f || in.windowMinutes <= 0) {
    return { false, -1, 0.0f, NAN };
  }

  // Cible journalière effective : plafonnée par la limite de sécurité (si > 0).
  float effective = in.dailyTargetMl;
  if (in.maxDailyMl > 0.0f && effective > in.maxDailyMl) {
    effective = in.maxDailyMl;
  }

  // Volume restant à répartir sur la journée (clamp >= 0 — condition n°3).
  float remaining = effective - in.dailyInjectedMl;
  if (remaining < 0.0f) remaining = 0.0f;

  // Index de fenêtre absolu dans la journée (0..95 pour des fenêtres de 15 min).
  int windowIndex = in.nowMin / in.windowMinutes;

  float stopTargetMl;
  if (windowIndex != in.prevWindowIndex) {
    // Nouvelle fenêtre : recalcul du volume de fenêtre depuis l'état courant.
    // floor volontaire ; plancher nWin >= 1 SÛR : horizonMinutes > 0 est garanti
    // par la garde d'entrée (condition n°2 — jamais nWin=1 via max(1,0)).
    int nWin = in.horizonMinutes / in.windowMinutes;
    if (nWin < 1) nWin = 1;
    float v = remaining / static_cast<float>(nWin);

    // Borne budget horaire (ADR-0020, budget PARTAGÉ auto+manuel+test+UART) :
    // le volume de fenêtre est converti en temps via le débit effectif ;
    // l'excédent se reporte mécaniquement aux fenêtres suivantes.
    if (in.hourlyLimitMs > 0) {
      float budgetMl = (in.usedMs >= in.hourlyLimitMs)
          ? 0.0f
          : (static_cast<float>(in.hourlyLimitMs - in.usedMs) / 60000.0f) *
                in.effectiveFlowMlPerMin;
      if (v > budgetMl) v = budgetMl;
    }
    if (v < 0.0f) v = 0.0f;  // clamp >= 0 (condition n°3)

    // Report anti short-cycling : si la durée d'injection correspondante est
    // < minInjectionTimeMs, rien dans CETTE fenêtre — le volume se reporte
    // (remaining inchangé, moins de fenêtres restantes → doses plus longues).
    if (v > 0.0f &&
        (v / in.effectiveFlowMlPerMin) * 60000.0f <
            static_cast<float>(in.minInjectionTimeMs)) {
      v = 0.0f;
    }
    stopTargetMl = in.dailyInjectedMl + v;
  } else {
    // Même fenêtre : la cible d'arrêt cumulée reste celle calculée à l'entrée
    // de la fenêtre (le cumul progresse vers elle pendant l'injection).
    stopTargetMl = in.prevStopTargetMl;
  }

  // Débit moyen planifié restant (diagnostic WS) : NAN quand plus rien à
  // injecter (quota atteint). horizonMinutes > 0 garanti par la garde d'entrée.
  float plannedFlow = (remaining > 0.0f)
      ? remaining / static_cast<float>(in.horizonMinutes)
      : NAN;

  // Condition n°3 : doseNow réévalué à CHAQUE tick contre
  // min(stopTargetMl, effective) — une baisse de cible ou de plafond en cours
  // de fenêtre arrête l'injection immédiatement, sans attendre la fenêtre
  // suivante.
  float stopCap = (stopTargetMl < effective) ? stopTargetMl : effective;
  bool doseNow = in.dailyInjectedMl < stopCap;

  return { doseNow, windowIndex, stopTargetMl, plannedFlow };
}

// =============================================================================
// Mode Boost — valeurs effectives PURES (feature-053)
// =============================================================================
// Effet STRICTEMENT réservé au mode ORP "automatic" (condition pool-chemistry #1).
float effectiveOrpTargetPure(float orpTarget, bool boostActive, bool orpModeAutomatic,
                             float deltaMv, float ceilingMv) {
  if (!boostActive || !orpModeAutomatic) {
    return orpTarget;
  }
  // fminf plafonne la cible relevée ; le max() garantit que la cible n'est
  // JAMAIS abaissée si elle dépasse déjà le plafond (condition #5).
  float raised = fminf(orpTarget + deltaMv, ceilingMv);
  return orpTarget > raised ? orpTarget : raised;
}

float effectiveMaxChlorinePure(float maxChlorine, bool boostActive, bool orpModeAutomatic,
                               float factor, float hardCapMl) {
  if (!boostActive || !orpModeAutomatic) {
    return maxChlorine;
  }
  // Limite journalière relevée × facteur, bornée en dur (condition #1).
  return fminf(maxChlorine * factor, hardCapMl);
}
