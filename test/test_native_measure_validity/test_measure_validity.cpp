// =============================================================================
// Tests unitaires natifs — Validité des mesures pH/ORP (measure_validity_logic,
// feature-061)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la décision PURE extraite dans
// src/measure_validity_logic.{h,cpp} (ADR-0017, comme dosing_logic /
// history_guard_logic) :
//   - evaluateMeasureValidity(MeasureValidityInputs) → { valid, remainingS }
//
// Ordre des règles (priorité décroissante), verrouillé par ces tests :
//   1. eau absente                → invalide, remaining = 0 ;
//   2. délai de purge == 0        → valide, remaining = 0 ;
//   3. front jamais observé (waterRiseMs == 0, sentinelle) → invalide,
//      remaining = délai complet arrondi seconde SUP ;
//   4. (nowMs - waterRiseMs) < délai → invalide, remaining = restant arrondi
//      seconde SUP (soustraction non signée wrap-safe) ;
//   5. sinon                      → valide, remaining = 0.
//
// FRONTIÈRES figées : elapsed == délai - 1 → invalide (1 s) ;
// elapsed == délai → valide ; elapsed == délai + 1 → valide.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "measure_validity_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Délai de purge de référence : 5 min (mqttCfg.stabilizationDelayMin = 5, défaut prod).
static const uint32_t kDelayMs = 5UL * 60UL * 1000UL;  // 300 000 ms

// Helper : entrées « purge armée » — eau présente, front montant à riseMs.
static MeasureValidityInputs inputsAt(uint32_t nowMs, uint32_t riseMs,
                                      uint32_t delayMs = kDelayMs) {
  MeasureValidityInputs in;
  in.waterPresent = true;
  in.nowMs = nowMs;
  in.waterRiseMs = riseMs;
  in.purgeDelayMs = delayMs;
  return in;
}

// =============================================================================
// 1. Eau absente → invalide, remaining = 0
// =============================================================================
void test_water_absent_invalid_remaining_zero(void) {
  MeasureValidityInputs in = inputsAt(1000000UL, 500000UL);
  in.waterPresent = false;
  MeasureValidityResult r = evaluateMeasureValidity(in);
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// Eau absente PRIME sur tout : même délai 0 → invalide (règle 1 avant règle 2).
void test_water_absent_wins_over_zero_delay(void) {
  MeasureValidityInputs in = inputsAt(1000UL, 1000UL, 0);
  in.waterPresent = false;
  MeasureValidityResult r = evaluateMeasureValidity(in);
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// =============================================================================
// 2. Délai 0 (purge désactivée) → valide immédiat dès que l'eau circule
// =============================================================================
void test_zero_delay_valid_immediately(void) {
  // Même au tick exact du front montant (elapsed = 0).
  MeasureValidityResult r = evaluateMeasureValidity(inputsAt(5000UL, 5000UL, 0));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// Délai 0 prime même sur la sentinelle « front jamais observé » (règle 2 avant 3).
void test_zero_delay_valid_even_without_rise(void) {
  MeasureValidityResult r = evaluateMeasureValidity(inputsAt(5000UL, 0, 0));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// =============================================================================
// 3. Front jamais observé (sentinelle 0) → conservateur, délai complet
// =============================================================================
void test_no_rise_observed_conservative_full_delay(void) {
  MeasureValidityResult r = evaluateMeasureValidity(inputsAt(123456UL, 0));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(kDelayMs / 1000UL, r.stabilizationRemainingS);  // 300 s
}

// =============================================================================
// 4. Front montant → invalide pendant le délai, puis valide (frontières)
// =============================================================================
void test_invalid_right_after_rise(void) {
  // elapsed = 0 : tout le délai reste.
  MeasureValidityResult r = evaluateMeasureValidity(inputsAt(10000UL, 10000UL));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(300, r.stabilizationRemainingS);
}

void test_boundary_delay_minus_one_ms_invalid(void) {
  // elapsed = délai - 1 → encore invalide, 1 ms restante arrondie à 1 s.
  MeasureValidityResult r =
      evaluateMeasureValidity(inputsAt(10000UL + kDelayMs - 1, 10000UL));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(1, r.stabilizationRemainingS);
}

void test_boundary_delay_exact_valid(void) {
  // elapsed == délai → valide (frontière < stricte).
  MeasureValidityResult r =
      evaluateMeasureValidity(inputsAt(10000UL + kDelayMs, 10000UL));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

void test_boundary_delay_plus_one_valid(void) {
  MeasureValidityResult r =
      evaluateMeasureValidity(inputsAt(10000UL + kDelayMs + 1, 10000UL));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// =============================================================================
// 5. Retombée d'eau → invalide immédiat, même purge écoulée depuis longtemps
// =============================================================================
void test_water_drop_invalid_immediately(void) {
  MeasureValidityInputs in = inputsAt(10000UL + kDelayMs * 10, 10000UL);
  in.waterPresent = false;  // l'eau retombe : la validité tombe au même tick
  MeasureValidityResult r = evaluateMeasureValidity(in);
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// =============================================================================
// 6. Wrap millis (front proche de UINT32_MAX) — soustraction non signée
// =============================================================================
void test_millis_wrap_still_counting(void) {
  // Front 1000 ms avant le wrap ; now = 2000 ms après le wrap → elapsed = 3000 ms.
  uint32_t rise = UINT32_MAX - 999UL;  // wrap dans 1000 ms
  uint32_t now = 2000UL;
  MeasureValidityResult r = evaluateMeasureValidity(inputsAt(now, rise));
  TEST_ASSERT_FALSE(r.valid);
  // restant = 300 000 - 3 000 = 297 000 ms → 297 s.
  TEST_ASSERT_EQUAL_UINT32(297, r.stabilizationRemainingS);
}

void test_millis_wrap_delay_elapsed_valid(void) {
  // Front 1000 ms avant le wrap ; délai entièrement écoulé après le wrap.
  uint32_t rise = UINT32_MAX - 999UL;
  uint32_t now = kDelayMs - 1000UL;  // elapsed = kDelayMs exactement
  MeasureValidityResult r = evaluateMeasureValidity(inputsAt(now, rise));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(0, r.stabilizationRemainingS);
}

// =============================================================================
// 7. Arrondi seconde SUPÉRIEURE du restant
// =============================================================================
void test_remaining_rounds_up_partial_second(void) {
  // elapsed = délai - 1500 ms → restant 1500 ms → 2 s (jamais 1).
  MeasureValidityResult r =
      evaluateMeasureValidity(inputsAt(10000UL + kDelayMs - 1500UL, 10000UL));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(2, r.stabilizationRemainingS);
}

void test_remaining_exact_second_not_inflated(void) {
  // restant = 2000 ms pile → 2 s (l'arrondi SUP n'ajoute rien sur un multiple exact).
  MeasureValidityResult r =
      evaluateMeasureValidity(inputsAt(10000UL + kDelayMs - 2000UL, 10000UL));
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(2, r.stabilizationRemainingS);
}

// =============================================================================
// Runner
// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_water_absent_invalid_remaining_zero);
  RUN_TEST(test_water_absent_wins_over_zero_delay);
  RUN_TEST(test_zero_delay_valid_immediately);
  RUN_TEST(test_zero_delay_valid_even_without_rise);
  RUN_TEST(test_no_rise_observed_conservative_full_delay);
  RUN_TEST(test_invalid_right_after_rise);
  RUN_TEST(test_boundary_delay_minus_one_ms_invalid);
  RUN_TEST(test_boundary_delay_exact_valid);
  RUN_TEST(test_boundary_delay_plus_one_valid);
  RUN_TEST(test_water_drop_invalid_immediately);
  RUN_TEST(test_millis_wrap_still_counting);
  RUN_TEST(test_millis_wrap_delay_elapsed_valid);
  RUN_TEST(test_remaining_rounds_up_partial_second);
  RUN_TEST(test_remaining_exact_second_not_inflated);
  return UNITY_END();
}
