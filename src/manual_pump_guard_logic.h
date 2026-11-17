#ifndef MANUAL_PUMP_GUARD_LOGIC_H
#define MANUAL_PUMP_GUARD_LOGIC_H

// =============================================================================
// Logique PURE du garde-fou des pompes en MODE MANUEL (bug-171).
// Header sans dépendance Arduino → testable en natif (ADR-0017).
//
// Contexte : une pompe en mode manuel (test /pumpN/on, injection manuelle,
// assistant de débit) est exemptée des trois blocs de suspension globale de
// updateDosingCore(). Sans échéance propre à l'appareil, seul un minuteur du
// navigateur l'arrêtait. Ce module décide QUAND l'appareil coupe de lui-même.
//
// Consommé par PumpControllerClass::tickManualPumpGuard() (loopTask).
// =============================================================================

#include <stdint.h>

// Cause de la coupure d'une pompe manuelle (kNone = laisser tourner).
enum class ManualCut : uint8_t {
  kNone,        // aucune raison de couper
  kWaterLost,   // présence d'eau perdue (filtration arrêtée / signal périmé)
  kCalSession,  // session d'étalonnage ouverte (feature-163)
  kDeadline,    // échéance atteinte, ou ABSENTE (fail-closed)
};

/**
 * @brief Échéance atteinte ? Wrap-safe (rollover de millis() à 49,7 j,
 * différence signée valide pour des bornes < 24,8 j).
 */
inline bool isManualRunExpired(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

/**
 * @brief Verdict de coupure d'une pompe en mode manuel.
 *
 * Ordre (pool-chemistry 2026-11-17, cond. 1) : eau perdue > étalonnage >
 * échéance. L'échéance est une fin NOMINALE : elle ne masque jamais une
 * condition actionnable. Fail-closed : mode manuel SANS échéance armée
 * ⇒ kDeadline (coupure immédiate) — un état incohérent ne laisse jamais
 * tourner une pompe.
 */
inline ManualCut evaluateManualCut(bool manualOn, bool deadlineArmed, bool expired,
                                   bool waterPresent, bool calOpen) {
  if (!manualOn) return ManualCut::kNone;
  if (!waterPresent) return ManualCut::kWaterLost;
  if (calOpen) return ManualCut::kCalSession;
  if (!deadlineArmed || expired) return ManualCut::kDeadline;
  return ManualCut::kNone;
}

#endif  // MANUAL_PUMP_GUARD_LOGIC_H
