// =============================================================================
// Tests unitaires natifs — Disjoncteur boot-loop init history
// (history_guard_logic, v2.10.0, feature-059)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la décision PURE extraite dans
// src/history_guard_logic.{h,cpp} (ADR-0017, comme net_health_logic /
// self_heal_logic / dosing_logic / schedule_logic / sensor_filter) :
//   - isCrashResetKind(HistoryResetKind) → bool
//   - evaluateHistoryGuard(HistoryGuardInputs) →
//       { Proceed, CountCrash, ErasePartition }
//
// Ordre des règles (priorité décroissante), verrouillé par ces tests :
//   1. crashCount == 0              → Proceed (pas de drapeau NVS : l'init
//      précédente s'est terminée proprement, ou premier boot).
//   2. !resetWasCrash               → Proceed (reset propre : réarmement du
//      drapeau sans compter — POWERON/SW/EXT/BROWNOUT/DEEPSLEEP/Other).
//   3. crashCount >= eraseThreshold → ErasePartition (frontière >=).
//   4. sinon                        → CountCrash (crash compté, sous le seuil).
//
// FRONTIÈRE figée par le code (>= vs <) :
//   - effacement ssi crashCount >= eraseThreshold (== → effacement).
//     Avec seuil=2 : crashCount=1 → CountCrash ; crashCount=2 → ErasePartition.
//   - Cas dégénéré seuil=0 : tout crash compté (crashCount>=1) satisfait
//     trivialement crashCount >= 0 → ErasePartition dès le premier crash.
//     COMPORTEMENT ASSUMÉ : un seuil nul est un « effacement au moindre crash »,
//     jamais un désarmement du disjoncteur (fail-safe, disponibilité > données).
//
// Mapping isCrashResetKind (exhaustif sur les 10 valeurs de l'enum) :
//   Panic / IntWdt / TaskWdt / OtherWdt → true (causes « crash ») ;
//   PowerOn / Software / External / Brownout / DeepSleep / Other → false.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "history_guard_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Valeur de référence (répliquée ici, module pur sans include Arduino) :
// kHistoryInitCrashThreshold = 2 dans constants.h. Toute divergence firmware
// sera détectée par la revue.
static const uint8_t kEraseThreshold = 2;

// Helper : entrées « nominales » — pas de drapeau NVS (crashCount=0), dernier
// reset propre, seuil firmware. Chaque test part de cette base et ne perturbe
// que le(s) champ(s) étudié(s).
static HistoryGuardInputs baseInputs(void) {
  HistoryGuardInputs in;
  in.crashCount     = 0;
  in.resetWasCrash  = false;
  in.eraseThreshold = kEraseThreshold;
  return in;
}

// =============================================================================
// 0. Mapping isCrashResetKind — exhaustif sur les 10 valeurs de l'enum
// =============================================================================

// Causes « crash » : exception CPU + les 3 watchdogs → true.
void test_crash_kind_panic_is_crash(void) {
  TEST_ASSERT_TRUE(isCrashResetKind(HistoryResetKind::Panic));
}
void test_crash_kind_int_wdt_is_crash(void) {
  TEST_ASSERT_TRUE(isCrashResetKind(HistoryResetKind::IntWdt));
}
void test_crash_kind_task_wdt_is_crash(void) {
  TEST_ASSERT_TRUE(isCrashResetKind(HistoryResetKind::TaskWdt));
}
void test_crash_kind_other_wdt_is_crash(void) {
  TEST_ASSERT_TRUE(isCrashResetKind(HistoryResetKind::OtherWdt));
}

// Causes « propres » : un drapeau resté posé dans ces cas ne compte pas → false.
void test_crash_kind_power_on_is_clean(void) {
  TEST_ASSERT_FALSE(isCrashResetKind(HistoryResetKind::PowerOn));
}
void test_crash_kind_software_is_clean(void) {
  TEST_ASSERT_FALSE(isCrashResetKind(HistoryResetKind::Software));
}
void test_crash_kind_external_is_clean(void) {
  TEST_ASSERT_FALSE(isCrashResetKind(HistoryResetKind::External));
}
void test_crash_kind_brownout_is_clean(void) {
  TEST_ASSERT_FALSE(isCrashResetKind(HistoryResetKind::Brownout));
}
void test_crash_kind_deep_sleep_is_clean(void) {
  TEST_ASSERT_FALSE(isCrashResetKind(HistoryResetKind::DeepSleep));
}
void test_crash_kind_other_is_clean(void) {
  TEST_ASSERT_FALSE(isCrashResetKind(HistoryResetKind::Other));
}

// =============================================================================
// 1. crashCount == 0 → Proceed (pas de drapeau, quel que soit resetWasCrash)
// =============================================================================

// Nominal : premier boot / init précédente complète, reset propre → Proceed.
void test_proceed_when_no_flag_clean_reset(void) {
  HistoryGuardInputs in = baseInputs();  // crashCount=0, resetWasCrash=false
  TEST_ASSERT_EQUAL(HistoryGuardDecision::Proceed, evaluateHistoryGuard(in));
}

// Panic HORS fenêtre d'init (drapeau absent car begin() précédent avait fini) :
// la règle 1 court-circuite tout, y compris resetWasCrash=true.
void test_proceed_when_no_flag_even_after_crash(void) {
  HistoryGuardInputs in = baseInputs();
  in.resetWasCrash = true;  // panic ailleurs dans le firmware : hors périmètre
  TEST_ASSERT_EQUAL(HistoryGuardDecision::Proceed, evaluateHistoryGuard(in));
}

// crashCount=0 l'emporte même avec un seuil dégénéré à 0 (0 >= 0 serait vrai,
// mais la règle 1 est évaluée AVANT la règle 3).
void test_proceed_no_flag_priority_over_zero_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.resetWasCrash  = true;
  in.eraseThreshold = 0;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::Proceed, evaluateHistoryGuard(in));
}

// =============================================================================
// 2. Drapeau présent + reset propre → Proceed (réarmement sans compter)
// =============================================================================

// Coupure de courant pendant la fenêtre d'init : drapeau resté posé mais le
// reset n'est pas un crash → on ne compte pas, montage normal.
void test_proceed_flag_set_but_clean_reset(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = 1;
  in.resetWasCrash = false;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::Proceed, evaluateHistoryGuard(in));
}

// Reset propre l'emporte MÊME si le compteur a déjà atteint/dépassé le seuil :
// la règle 2 est évaluée avant la règle 3 (pas d'effacement sur reset propre).
void test_proceed_clean_reset_even_at_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = kEraseThreshold;  // seuil atteint…
  in.resetWasCrash = false;            // …mais reset propre → Proceed
  TEST_ASSERT_EQUAL(HistoryGuardDecision::Proceed, evaluateHistoryGuard(in));
}

// Idem bien au-delà du seuil (compteur incohérent hérité) : reset propre → Proceed.
void test_proceed_clean_reset_far_beyond_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = 200;
  in.resetWasCrash = false;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::Proceed, evaluateHistoryGuard(in));
}

// =============================================================================
// 3./4. Frontière du seuil (>= vs <) — seuil firmware = 2
// =============================================================================

// crashCount=1 < seuil=2 → sous le seuil → CountCrash (compteur+1, montage).
void test_count_crash_one_below_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = 1;
  in.resetWasCrash = true;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::CountCrash, evaluateHistoryGuard(in));
}

// crashCount=2 == seuil=2 → frontière >= atteinte → ErasePartition.
void test_erase_at_threshold_exact(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = kEraseThreshold;  // == 2
  in.resetWasCrash = true;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

// crashCount=3 > seuil=2 → au-delà de la frontière → ErasePartition.
void test_erase_above_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = 3;
  in.resetWasCrash = true;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

// =============================================================================
// 5. Seuil = 1 : effacement dès le 1er crash compté
// =============================================================================

// crashCount=1 >= seuil=1 → ErasePartition au premier crash pendant l'init.
void test_erase_first_crash_when_threshold_one(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount     = 1;
  in.resetWasCrash  = true;
  in.eraseThreshold = 1;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

// =============================================================================
// 6. Robustesse : valeurs extrêmes / dégénérées
// =============================================================================

// Compteur saturé uint8_t (255) + crash → ErasePartition (255 >= 2).
void test_erase_when_crash_count_saturated(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount    = 255;
  in.resetWasCrash = true;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

// Seuil dégénéré = 0 : tout crash compté (crashCount>=1) déclenche l'effacement
// (1 >= 0). COMPORTEMENT ASSUMÉ (cf. en-tête) : un seuil nul signifie
// « effacement au moindre crash », jamais un désarmement du disjoncteur.
void test_erase_any_counted_crash_when_threshold_zero(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount     = 1;
  in.resetWasCrash  = true;
  in.eraseThreshold = 0;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

// Combo extrême : compteur saturé + seuil 0 + crash → ErasePartition.
void test_erase_saturated_count_zero_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.crashCount     = 255;
  in.resetWasCrash  = true;
  in.eraseThreshold = 0;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

// Seuil maximal uint8_t (255) : crashCount=254 → CountCrash ; 255 → Erase.
void test_boundary_max_threshold(void) {
  HistoryGuardInputs in = baseInputs();
  in.resetWasCrash  = true;
  in.eraseThreshold = 255;
  in.crashCount     = 254;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::CountCrash, evaluateHistoryGuard(in));
  in.crashCount = 255;
  TEST_ASSERT_EQUAL(HistoryGuardDecision::ErasePartition, evaluateHistoryGuard(in));
}

int main(int, char**) {
  UNITY_BEGIN();
  // 0. Mapping isCrashResetKind (exhaustif : 10 valeurs)
  RUN_TEST(test_crash_kind_panic_is_crash);
  RUN_TEST(test_crash_kind_int_wdt_is_crash);
  RUN_TEST(test_crash_kind_task_wdt_is_crash);
  RUN_TEST(test_crash_kind_other_wdt_is_crash);
  RUN_TEST(test_crash_kind_power_on_is_clean);
  RUN_TEST(test_crash_kind_software_is_clean);
  RUN_TEST(test_crash_kind_external_is_clean);
  RUN_TEST(test_crash_kind_brownout_is_clean);
  RUN_TEST(test_crash_kind_deep_sleep_is_clean);
  RUN_TEST(test_crash_kind_other_is_clean);
  // 1. Pas de drapeau → Proceed
  RUN_TEST(test_proceed_when_no_flag_clean_reset);
  RUN_TEST(test_proceed_when_no_flag_even_after_crash);
  RUN_TEST(test_proceed_no_flag_priority_over_zero_threshold);
  // 2. Reset propre → Proceed (réarmement sans compter)
  RUN_TEST(test_proceed_flag_set_but_clean_reset);
  RUN_TEST(test_proceed_clean_reset_even_at_threshold);
  RUN_TEST(test_proceed_clean_reset_far_beyond_threshold);
  // 3./4. Frontière du seuil (seuil=2)
  RUN_TEST(test_count_crash_one_below_threshold);
  RUN_TEST(test_erase_at_threshold_exact);
  RUN_TEST(test_erase_above_threshold);
  // 5. Seuil = 1
  RUN_TEST(test_erase_first_crash_when_threshold_one);
  // 6. Robustesse
  RUN_TEST(test_erase_when_crash_count_saturated);
  RUN_TEST(test_erase_any_counted_crash_when_threshold_zero);
  RUN_TEST(test_erase_saturated_count_zero_threshold);
  RUN_TEST(test_boundary_max_threshold);
  return UNITY_END();
}
