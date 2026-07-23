// =============================================================================
// Tests unitaires natifs — Watchdog de connectivité (net_health_logic, v2.9.2)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la décision PURE extraite dans
// src/net_health_logic.{h,cpp} (ADR-0017, comme self_heal_logic / dosing_logic
// / schedule_logic / sensor_filter) :
//   - evaluateNetHealth(NetHealthInputs) →
//       { Continue, SoftRecover, Restart, EnterMaintenance }
//
// Ordre des règles (priorité décroissante), verrouillé par ces tests :
//   1. !wifiAssociated → Continue (horloge de stall gelée, rien à juger).
//   2. uptimeMs < minUptimeMs → Continue (démarrage/DHCP/mDNS en cours).
//   3. rebootCountInWindow >= maxReboots → EnterMaintenance (anti-boucle,
//      PRIORITÉ HAUTE : l'emporte même si un Restart serait sinon dû).
//   4. msSinceLastNetOk >= hardStallMs → Restart (gel confirmé).
//   5. msSinceLastNetOk >= softStallMs ET !softRecoverAttempted → SoftRecover.
//   6. sinon → Continue.
//
// FRONTIÈRES figées par le code (>= vs <) :
//   - stall soft atteint ssi msSinceLastNetOk >= softStallMs (== → atteint).
//   - stall hard atteint ssi msSinceLastNetOk >= hardStallMs (== → atteint).
//   - uptime atteint ssi uptimeMs >= minUptimeMs (== → atteint, watchdog armé).
//   - maintenance ssi rebootCountInWindow >= maxReboots (== → maintenance).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "net_health_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Valeurs de référence (répliquées ici, module pur sans include Arduino).
// Toute divergence firmware sera détectée par la revue.
static const uint32_t kSoftStallMs = 60000UL;   // kNetworkStallSoftMs (indicatif)
static const uint32_t kHardStallMs = 180000UL;  // kNetworkStallRebootMs (indicatif)
static const uint32_t kMinUptimeMs = 120000UL;  // kAutoHealMinUptimeMs (indicatif)
static const uint8_t  kMaxReboots  = 3;          // kAutoHealMaxReboots (indicatif)

// Helper : entrées « saines » — WiFi associé, réseau vivant (stall nul), uptime
// largement écoulé, aucun reboot, aucun soft recover tenté. Chaque test part de
// cette base et ne perturbe que le(s) champ(s) étudié(s).
static NetHealthInputs healthyInputs(void) {
  NetHealthInputs in;
  in.wifiAssociated      = true;
  in.msSinceLastNetOk    = 0;                 // signe de vie tout récent
  in.softStallMs         = kSoftStallMs;
  in.hardStallMs         = kHardStallMs;
  in.softRecoverAttempted = false;
  in.uptimeMs            = kMinUptimeMs * 10;  // démarré depuis longtemps
  in.minUptimeMs         = kMinUptimeMs;
  in.rebootCountInWindow = 0;
  in.maxReboots          = kMaxReboots;
  return in;
}

// Helper : entrées où un Restart serait dû (WiFi associé, uptime écoulé, stall
// au-delà du seuil hard, sous le plafond de reboots).
static NetHealthInputs restartReadyInputs(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk = kHardStallMs;  // gel confirmé
  return in;
}

// =============================================================================
// 1. !wifiAssociated → Continue (même si stall énorme)
// =============================================================================
void test_continue_when_wifi_not_associated(void) {
  NetHealthInputs in = healthyInputs();
  in.wifiAssociated   = false;
  in.msSinceLastNetOk = kHardStallMs * 100;  // stall gigantesque : ignoré
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// WiFi absent l'emporte même quand le plafond de reboots est atteint : la
// règle 1 court-circuite tout (l'horloge de stall est gelée par la coquille).
void test_continue_wifi_absent_even_with_reboots_maxed(void) {
  NetHealthInputs in = healthyInputs();
  in.wifiAssociated      = false;
  in.msSinceLastNetOk    = kHardStallMs * 10;
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 2. uptimeMs < minUptimeMs → Continue (même si stall > hardStall)
// =============================================================================
void test_continue_when_uptime_below_min(void) {
  NetHealthInputs in = restartReadyInputs();  // stall au-delà du hard
  in.uptimeMs = kMinUptimeMs - 1;             // watchdog pas encore armé
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Uptime insuffisant l'emporte sur un stall gigantesque.
void test_continue_uptime_below_min_even_huge_stall(void) {
  NetHealthInputs in = healthyInputs();
  in.uptimeMs         = kMinUptimeMs - 1;
  in.msSinceLastNetOk = kHardStallMs * 100;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 3. EnterMaintenance PRIORITAIRE : plafond de reboots atteint l'emporte même
//    quand un Restart serait sinon dû (anti-boucle).
// =============================================================================
void test_maintenance_priority_over_restart(void) {
  NetHealthInputs in = restartReadyInputs();  // Restart aurait été renvoyé…
  in.rebootCountInWindow = kMaxReboots;        // …mais plafond atteint
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// Maintenance même sur un réseau parfaitement sain dès que le plafond est
// atteint : la règle 3 court-circuite Restart/SoftRecover.
void test_maintenance_even_when_network_healthy(void) {
  NetHealthInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// Reboots au-delà du plafond → maintenance (>= et non ==).
void test_maintenance_when_reboots_exceed_max(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = kMaxReboots + 5;
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// =============================================================================
// 4. msSinceLastNetOk >= hardStallMs → Restart (WiFi associé, uptime OK, sous
//    le plafond de reboots)
// =============================================================================
void test_restart_when_hard_stall(void) {
  NetHealthInputs in = restartReadyInputs();
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Stall bien au-delà du hard → toujours Restart (sous le plafond).
void test_restart_when_stall_far_beyond_hard(void) {
  NetHealthInputs in = restartReadyInputs();
  in.msSinceLastNetOk = kHardStallMs * 3;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Restart l'emporte sur SoftRecover même si le soft n'a jamais été tenté :
// une fois le seuil hard franchi, on redémarre (règle 4 avant règle 5).
void test_restart_over_soft_when_soft_not_attempted(void) {
  NetHealthInputs in = restartReadyInputs();
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Juste sous le plafond de reboots + hard stall → Restart (la règle 3 ne
// s'active pas encore).
void test_restart_reboots_one_below_max(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// =============================================================================
// 5. msSinceLastNetOk >= softStallMs ET !softRecoverAttempted → SoftRecover ;
//    si softRecoverAttempted → Continue (pas de 2e soft, on attend le hard).
// =============================================================================
void test_soft_recover_when_soft_stall_and_not_attempted(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs;  // stall léger atteint
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Stall entre soft et hard, soft déjà tenté → Continue (attente du seuil hard).
void test_continue_when_soft_already_attempted(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs;  // au-dessus du soft, sous le hard
  in.softRecoverAttempted = true;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Stall à mi-chemin (bien au-dessus du soft, sous le hard) sans soft tenté →
// SoftRecover.
void test_soft_recover_mid_range(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = (kSoftStallMs + kHardStallMs) / 2;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Même stall à mi-chemin mais soft déjà tenté → Continue.
void test_continue_mid_range_soft_attempted(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = (kSoftStallMs + kHardStallMs) / 2;
  in.softRecoverAttempted = true;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 6. Réseau sain (msSinceLastNetOk < softStallMs) → Continue
// =============================================================================
void test_continue_when_network_healthy(void) {
  NetHealthInputs in = healthyInputs();  // stall = 0
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Stall non nul mais sous le seuil soft → Continue (même si soft jamais tenté).
void test_continue_when_stall_below_soft(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs - 1;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 7. Frontières exactes aux seuils (== )
// =============================================================================

// Soft : msSinceLastNetOk == softStallMs → atteint (>=) → SoftRecover.
void test_boundary_soft_stall_equals_soft_recover(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Soft : msSinceLastNetOk == softStallMs - 1 → non atteint → Continue.
void test_boundary_soft_stall_one_below_continue(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs - 1;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Hard : msSinceLastNetOk == hardStallMs → atteint (>=) → Restart.
void test_boundary_hard_stall_equals_restart(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk = kHardStallMs;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Hard : msSinceLastNetOk == hardStallMs - 1 → sous le hard → SoftRecover
// (soft jamais tenté, stall bien au-dessus du soft).
void test_boundary_hard_stall_one_below_soft_recover(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kHardStallMs - 1;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Uptime : uptimeMs == minUptimeMs → watchdog armé (>=). Avec hard stall → Restart.
void test_boundary_uptime_equals_min_armed(void) {
  NetHealthInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Uptime : uptimeMs == minUptimeMs - 1 → non armé → Continue.
void test_boundary_uptime_one_below_min_continue(void) {
  NetHealthInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs - 1;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Maintenance : rebootCountInWindow == maxReboots → atteint (>=) → maintenance.
void test_boundary_reboots_equals_max_maintenance(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// Maintenance : rebootCountInWindow == maxReboots - 1 → sous le plafond → pas
// de maintenance (ici hard stall → Restart).
void test_boundary_reboots_one_below_max_restart(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

int main(int, char**) {
  UNITY_BEGIN();
  // 1. WiFi non associé
  RUN_TEST(test_continue_when_wifi_not_associated);
  RUN_TEST(test_continue_wifi_absent_even_with_reboots_maxed);
  // 2. Uptime insuffisant
  RUN_TEST(test_continue_when_uptime_below_min);
  RUN_TEST(test_continue_uptime_below_min_even_huge_stall);
  // 3. Maintenance prioritaire
  RUN_TEST(test_maintenance_priority_over_restart);
  RUN_TEST(test_maintenance_even_when_network_healthy);
  RUN_TEST(test_maintenance_when_reboots_exceed_max);
  // 4. Restart (hard stall)
  RUN_TEST(test_restart_when_hard_stall);
  RUN_TEST(test_restart_when_stall_far_beyond_hard);
  RUN_TEST(test_restart_over_soft_when_soft_not_attempted);
  RUN_TEST(test_restart_reboots_one_below_max);
  // 5. SoftRecover / pas de 2e soft
  RUN_TEST(test_soft_recover_when_soft_stall_and_not_attempted);
  RUN_TEST(test_continue_when_soft_already_attempted);
  RUN_TEST(test_soft_recover_mid_range);
  RUN_TEST(test_continue_mid_range_soft_attempted);
  // 6. Réseau sain
  RUN_TEST(test_continue_when_network_healthy);
  RUN_TEST(test_continue_when_stall_below_soft);
  // 7. Frontières
  RUN_TEST(test_boundary_soft_stall_equals_soft_recover);
  RUN_TEST(test_boundary_soft_stall_one_below_continue);
  RUN_TEST(test_boundary_hard_stall_equals_restart);
  RUN_TEST(test_boundary_hard_stall_one_below_soft_recover);
  RUN_TEST(test_boundary_uptime_equals_min_armed);
  RUN_TEST(test_boundary_uptime_one_below_min_continue);
  RUN_TEST(test_boundary_reboots_equals_max_maintenance);
  RUN_TEST(test_boundary_reboots_one_below_max_restart);
  return UNITY_END();
}
