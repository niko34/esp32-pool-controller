#include "self_heal_logic.h"

// =============================================================================
// self_heal_logic — implémentation PURE (v2.9.1)
// =============================================================================
// Aucune dépendance runtime : logique déterministe testable en natif.

SelfHealDecision evaluateSelfHeal(const SelfHealInputs& in) {
  // 1. Anti-boucle prioritaire : plafond de reboots atteint dans la fenêtre →
  //    on bascule en maintenance et on NE redémarre plus (sécurité pool-chemistry
  //    C2 : mieux vaut rester joignable pour diagnostic qu'entrer en boucle de
  //    reboot avec dosage réarmé à chaque cycle).
  if (in.rebootCountInWindow >= in.maxReboots) {
    return SelfHealDecision::EnterMaintenance;
  }

  // 2. Heap critique confirmé : uptime minimal écoulé (on laisse le système finir
  //    son démarrage/allocations légitimes) ET plus gros bloc sous le seuil ET
  //    condition confirmée sur `debounceChecks` ticks consécutifs (anti oscillation).
  if (in.uptimeMs >= in.minUptimeMs &&
      in.maxAllocHeap < in.restartThreshold &&
      in.consecutiveBelow >= in.debounceChecks) {
    return SelfHealDecision::Restart;
  }

  // 3. Fonctionnement normal.
  return SelfHealDecision::Continue;
}
