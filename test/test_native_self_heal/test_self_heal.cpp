// =============================================================================
// Tests unitaires natifs — Auto-guérison mémoire (self_heal_logic, v2.9.1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la décision PURE extraite dans
// src/self_heal_logic.{h,cpp} (ADR-0017, comme dosing_logic / schedule_logic
// / sensor_filter) :
//   - evaluateSelfHeal(SelfHealInputs) → { Continue, Restart, EnterMaintenance }
//
// Ordre des règles (priorité décroissante), verrouillé par ces tests :
//   1. rebootCountInWindow >= maxReboots → EnterMaintenance (anti-boucle,
//      PRIORITÉ HAUTE : l'emporte même si les conditions de Restart sont réunies).
//   2. uptimeMs >= minUptimeMs ET maxAllocHeap < restartThreshold ET
//      consecutiveBelow >= debounceChecks → Restart.
//   3. sinon → Continue.
//
// FRONTIÈRES figées par le code (>= vs <) :
//   - heap OK ssi maxAllocHeap >= restartThreshold (frontière == → Continue).
//   - debounce atteint ssi consecutiveBelow >= debounceChecks (== → atteint).
//   - uptime atteint ssi uptimeMs >= minUptimeMs (== → atteint).
//   - maintenance ssi rebootCountInWindow >= maxReboots (== → maintenance).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "self_heal_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Valeurs de référence (répliquées ici, module pur sans include Arduino).
// Toute divergence firmware sera détectée par la revue.
static const uint32_t kThreshold    = 24000UL;  // kRestartMaxAllocHeapBytes (indicatif)
static const uint8_t  kDebounce     = 3;         // kAutoHealDebounceChecks (indicatif)
static const uint32_t kMinUptimeMs  = 120000UL;  // kAutoHealMinUptimeMs (indicatif)
static const uint8_t  kMaxReboots   = 3;         // kAutoHealMaxReboots (indicatif)

// Helper : entrées « saines » — heap OK, aucun reboot, uptime largement écoulé.
// Chaque test part de cette base et ne perturbe que le champ étudié.
static SelfHealInputs healthyInputs(void) {
  SelfHealInputs in;
  in.maxAllocHeap       = kThreshold * 2;   // très au-dessus du seuil
  in.restartThreshold   = kThreshold;
  in.consecutiveBelow   = 0;
  in.debounceChecks     = kDebounce;
  in.uptimeMs           = kMinUptimeMs * 10; // système démarré depuis longtemps
  in.minUptimeMs        = kMinUptimeMs;
  in.rebootCountInWindow = 0;
  in.maxReboots         = kMaxReboots;
  return in;
}

// Helper : entrées où TOUTES les conditions de Restart sont réunies (heap sous
// le seuil, debounce atteint, uptime écoulé) mais reboots sous le plafond.
static SelfHealInputs restartReadyInputs(void) {
  SelfHealInputs in = healthyInputs();
  in.maxAllocHeap     = kThreshold - 1;  // sous le seuil
  in.consecutiveBelow = kDebounce;       // debounce atteint
  // uptimeMs déjà >> minUptimeMs, rebootCountInWindow = 0 < maxReboots.
  return in;
}

// =============================================================================
// 1. Continue quand le heap est OK (maxAllocHeap >= seuil)
// =============================================================================
void test_continue_when_heap_ok(void) {
  SelfHealInputs in = healthyInputs();
  // Même avec debounce et uptime « favorables » au restart, le heap OK protège.
  in.consecutiveBelow = kDebounce + 5;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// =============================================================================
// 2. Continue quand sous le seuil mais debounce PAS atteint
// =============================================================================
void test_continue_when_below_but_debounce_not_reached(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = kDebounce - 1;  // il manque un tick
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Cas extrême : sous le seuil, uptime OK, mais aucun tick consécutif encore.
void test_continue_when_below_debounce_zero(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = 0;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// =============================================================================
// 3. Continue quand sous le seuil + debounce atteint mais uptime < minUptime
//    (pas de reboot pendant le démarrage / allocations légitimes)
// =============================================================================
void test_continue_when_uptime_below_min(void) {
  SelfHealInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs - 1;  // système encore en phase de démarrage
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// =============================================================================
// 4. Restart quand uptime >= min ET heap < seuil ET debounce atteint
// =============================================================================
void test_restart_when_all_conditions_met(void) {
  SelfHealInputs in = restartReadyInputs();
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Debounce largement dépassé → toujours Restart.
void test_restart_when_debounce_exceeded(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = kDebounce + 10;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// =============================================================================
// 5. EnterMaintenance PRIORITAIRE : le plafond de reboots l'emporte même si
//    toutes les conditions de Restart sont par ailleurs réunies (anti-boucle).
// =============================================================================
void test_maintenance_priority_over_restart(void) {
  SelfHealInputs in = restartReadyInputs();  // Restart aurait été renvoyé…
  in.rebootCountInWindow = kMaxReboots;      // …mais plafond atteint → maintenance
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// Maintenance même sur un système parfaitement sain (heap OK, uptime OK) dès
// que le plafond est atteint : la règle 1 court-circuite tout.
void test_maintenance_even_when_healthy(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// Reboots au-delà du plafond → maintenance (>= et non ==).
void test_maintenance_when_reboots_exceed_max(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots + 5;
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// =============================================================================
// 6. Frontières exactes aux seuils
// =============================================================================

// Heap : maxAllocHeap == restartThreshold → PAS sous le seuil (< strict) → Continue.
void test_boundary_heap_equals_threshold_continue(void) {
  SelfHealInputs in = restartReadyInputs();
  in.maxAllocHeap = kThreshold;  // exactement au seuil
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Heap : maxAllocHeap == restartThreshold - 1 → sous le seuil → Restart.
void test_boundary_heap_one_below_threshold_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.maxAllocHeap = kThreshold - 1;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Debounce : consecutiveBelow == debounceChecks → atteint (>=) → Restart.
void test_boundary_debounce_equals_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = kDebounce;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Uptime : uptimeMs == minUptimeMs → atteint (>=) → Restart.
void test_boundary_uptime_equals_min_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Uptime : uptimeMs == minUptimeMs - 1 → non atteint → Continue.
void test_boundary_uptime_one_below_min_continue(void) {
  SelfHealInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs - 1;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Maintenance : rebootCountInWindow == maxReboots → atteint (>=) → maintenance.
void test_boundary_reboots_equals_max_maintenance(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// Maintenance : rebootCountInWindow == maxReboots - 1 → sous le plafond → pas
// de maintenance (ici heap OK → Continue).
void test_boundary_reboots_one_below_max_no_maintenance(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Sous le plafond de reboots ET conditions de Restart réunies → Restart
// (confirme que la règle 1 ne s'active pas juste en dessous du plafond).
void test_reboots_one_below_max_still_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

int main(int, char**) {
  UNITY_BEGIN();
  // 1. Heap OK
  RUN_TEST(test_continue_when_heap_ok);
  // 2. Debounce non atteint
  RUN_TEST(test_continue_when_below_but_debounce_not_reached);
  RUN_TEST(test_continue_when_below_debounce_zero);
  // 3. Uptime insuffisant
  RUN_TEST(test_continue_when_uptime_below_min);
  // 4. Restart
  RUN_TEST(test_restart_when_all_conditions_met);
  RUN_TEST(test_restart_when_debounce_exceeded);
  // 5. Maintenance prioritaire
  RUN_TEST(test_maintenance_priority_over_restart);
  RUN_TEST(test_maintenance_even_when_healthy);
  RUN_TEST(test_maintenance_when_reboots_exceed_max);
  // 6. Frontières
  RUN_TEST(test_boundary_heap_equals_threshold_continue);
  RUN_TEST(test_boundary_heap_one_below_threshold_restart);
  RUN_TEST(test_boundary_debounce_equals_restart);
  RUN_TEST(test_boundary_uptime_equals_min_restart);
  RUN_TEST(test_boundary_uptime_one_below_min_continue);
  RUN_TEST(test_boundary_reboots_equals_max_maintenance);
  RUN_TEST(test_boundary_reboots_one_below_max_no_maintenance);
  RUN_TEST(test_reboots_one_below_max_still_restart);
  return UNITY_END();
}
