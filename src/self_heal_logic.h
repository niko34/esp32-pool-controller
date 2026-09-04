#ifndef SELF_HEAL_LOGIC_H
#define SELF_HEAL_LOGIC_H

// =============================================================================
// self_heal_logic — Décision d'auto-guérison PURE (v2.9.1)
// =============================================================================
// Logique de décision de l'auto-guérison mémoire (redémarrage propre sur heap
// critique / fragmentation) extraite de main.cpp pour être testable en natif
// (sans ESP32, sans FreeRTOS, sans Arduino). Style ADR-0017, comme
// dosing_logic / schedule_logic / sensor_filter.
//
// INVARIANT : ce module ne fait QUE décider à partir de valeurs déjà collectées.
// La collecte (heapLargestFreeBlock8Bit, millis, RTC RAM, fenêtre glissante) et les
// effets (stopAll, saveDailyCounters, publishAlert, flushToDisk, ESP.restart)
// restent dans la coquille impure main.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. On
// utilise l'en-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// Verdict de l'évaluation d'auto-guérison.
enum class SelfHealDecision : uint8_t {
  Continue = 0,      // rien à faire, fonctionnement normal
  Restart,           // heap critique confirmé → redémarrage propre
  EnterMaintenance   // trop de reboots dans la fenêtre → maintenance (anti-boucle)
};

// Entrées de la décision — toutes fournies par l'appelant (aucune API Arduino).
struct SelfHealInputs {
  uint32_t maxAllocHeap;        // plus gros bloc allouable 8 bits (heapLargestFreeBlock8Bit, bug-080)
  uint32_t restartThreshold;    // seuil de restart (kRestartMaxAllocHeapBytes)
  uint8_t  consecutiveBelow;    // nb de ticks consécutifs sous le seuil
  uint8_t  debounceChecks;      // debounce requis (kAutoHealDebounceChecks)
  uint32_t uptimeMs;            // uptime courant (millis)
  uint32_t minUptimeMs;         // uptime minimal avant tout restart (kAutoHealMinUptimeMs)
  uint8_t  rebootCountInWindow; // reboots auto déjà effectués dans la fenêtre glissante
  uint8_t  maxReboots;          // plafond de reboots auto (kAutoHealMaxReboots)
};

// Décision pure d'auto-guérison. Ordre des règles (priorité décroissante) :
//   1. rebootCountInWindow >= maxReboots → EnterMaintenance (anti-boucle, priorité
//      HAUTE : on n'enchaîne jamais un reboot de plus une fois le plafond atteint).
//   2. uptimeMs >= minUptimeMs ET maxAllocHeap < restartThreshold ET
//      consecutiveBelow >= debounceChecks → Restart.
//   3. sinon → Continue.
SelfHealDecision evaluateSelfHeal(const SelfHealInputs& in);

// Cadence d'échantillonnage heap adaptative (bug-badalloc-ws-broadcast) : rapproche
// les ticks sous le seuil d'alerte pour que le débounce (kAutoHealDebounceChecks = 3
// ticks) confirme en ~15 s au lieu de 180 s — sinon un effondrement rapide du plus
// gros bloc gagne la course contre l'auto-guérison (coredump 2026-07-28).
// Frontière : maxAllocHeap < warnThreshold → fast ; >= → normal.
uint32_t heapSampleIntervalMs(uint32_t maxAllocHeap, uint32_t warnThreshold,
                              uint32_t normalIntervalMs, uint32_t fastIntervalMs);

#endif // SELF_HEAL_LOGIC_H
