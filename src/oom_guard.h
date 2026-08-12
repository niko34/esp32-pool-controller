#ifndef OOM_GUARD_H
#define OOM_GUARD_H

// =============================================================================
// oom_guard — Parachute OOM global (glue Arduino du module pur oom_guard_logic.h)
// =============================================================================
// bug-oom-accept-http-durcissement (crash du 11/08/2026) : réserve d'urgence
// kOomReserveBytes pré-allouée au plus tôt du setup + std::set_new_handler qui
// la libère à la première famine (le new en cours est re-tenté et réussit) +
// heap_caps_register_failed_alloc_callback en mode flag-seul (diagnostic).
// Les logs (CRITICAL « réserve OOM consommée », WARN échecs heap_caps) sont
// émis en DIFFÉRÉ depuis la loop — JAMAIS depuis le handler/callback.

// Pré-alloue la réserve d'urgence, enregistre le new_handler et le callback
// diagnostic heap_caps. À appeler au PLUS TÔT du setup (avant toute
// fragmentation) — n'émet AUCUN log (systemLogger pas encore initialisé).
void initOomGuard();

// Tick de la loop : drain des flags posés par le handler/callback (émission
// différée des logs) + réarmement de la réserve à cadence lente
// (kOomRearmCheckIntervalMs) quand maxBloc repasse au-dessus de
// kOomRearmMinMaxAllocBytes (hystérésis). À appeler depuis loop() (loopTask).
void oomGuardLoopTick();

// true si la réserve d'urgence est actuellement allouée (diagnostic — permet
// au setup de journaliser l'état réel de l'armement).
bool oomGuardReserveArmed();

#endif  // OOM_GUARD_H
