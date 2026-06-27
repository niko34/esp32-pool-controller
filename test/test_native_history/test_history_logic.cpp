// =============================================================================
// Tests unitaires natifs — history_logic (feature-041)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// On teste le COMPORTEMENT observable du module pur d'agrégation d'historique :
//   - bucketTimestamp (AC1, frontières strictes + garde bucketSeconds==0)
//   - isOlderThan     (AC2, frontière stricte + wrap uint32)
//   - finalizeMean    (AC3, count==0 → NaN)
//   - isMajority      (AC4, division entière stricte)
//   - anyTrue         (AC4)
// via l'API publique, pas l'implémentation interne.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include "history_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// AC1 — bucketTimestamp
// -----------------------------------------------------------------------------
void test_bucketTimestamp_hourly_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(7200, bucketTimestamp(7200, 3600));  // frontière exacte
}
void test_bucketTimestamp_hourly_above_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(7200, bucketTimestamp(7201, 3600));
}
void test_bucketTimestamp_hourly_below_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(3600, bucketTimestamp(7199, 3600));
}
void test_bucketTimestamp_hourly_zero_ts(void) {
  TEST_ASSERT_EQUAL_UINT32(0, bucketTimestamp(0, 3600));
}
void test_bucketTimestamp_daily_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(86400, bucketTimestamp(86400, 86400));
}
void test_bucketTimestamp_daily_above_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(86400, bucketTimestamp(90000, 86400));
}
void test_bucketTimestamp_daily_below_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(0, bucketTimestamp(86399, 86400));
}
void test_bucketTimestamp_zero_bucket_guard(void) {
  TEST_ASSERT_EQUAL_UINT32(12345, bucketTimestamp(12345, 0));  // garde /0 → ts
}

// -----------------------------------------------------------------------------
// AC2 — isOlderThan (frontière STRICTE + wrap uint32)
// -----------------------------------------------------------------------------
void test_isOlderThan_age_zero(void) {
  TEST_ASSERT_FALSE(isOlderThan(10000, 10000, 3600));  // age 0
}
void test_isOlderThan_age_equals_max(void) {
  TEST_ASSERT_FALSE(isOlderThan(13600, 10000, 3600));  // 3600 > 3600 faux
}
void test_isOlderThan_age_max_plus_one(void) {
  TEST_ASSERT_TRUE(isOlderThan(13601, 10000, 3600));   // 3601 > 3600 vrai
}
void test_isOlderThan_uint32_wrap_within(void) {
  // (now - ts) = 50 - 0xFFFFFFF0 wrap = 66 ≤ 100 → false
  TEST_ASSERT_FALSE(isOlderThan(50, 0xFFFFFFF0u, 100));
}
void test_isOlderThan_uint32_wrap_exceeds(void) {
  // 66 > 10 → true
  TEST_ASSERT_TRUE(isOlderThan(50, 0xFFFFFFF0u, 10));
}

// -----------------------------------------------------------------------------
// AC3 — finalizeMean (count==0 → NaN)
// -----------------------------------------------------------------------------
void test_finalizeMean_zero_count_is_nan(void) {
  TEST_ASSERT_TRUE(isnan(finalizeMean(42.0f, 0)));
}
void test_finalizeMean_integer_result(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.0f, finalizeMean(21.0f, 3));
}
void test_finalizeMean_fractional_result(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.35f, finalizeMean(14.7f, 2));
}

// -----------------------------------------------------------------------------
// AC4 — isMajority (division ENTIÈRE stricte) + anyTrue
// -----------------------------------------------------------------------------
void test_isMajority_2_of_4_false(void) {
  TEST_ASSERT_FALSE(isMajority(2, 4));  // 2 > 2 faux
}
void test_isMajority_3_of_4_true(void) {
  TEST_ASSERT_TRUE(isMajority(3, 4));
}
void test_isMajority_3_of_5_true(void) {
  TEST_ASSERT_TRUE(isMajority(3, 5));   // 3 > 2
}
void test_isMajority_0_of_0_false(void) {
  TEST_ASSERT_FALSE(isMajority(0, 0));
}
void test_isMajority_1_of_1_true(void) {
  TEST_ASSERT_TRUE(isMajority(1, 1));   // 1 > 0
}
void test_isMajority_1_of_2_false(void) {
  TEST_ASSERT_FALSE(isMajority(1, 2));  // 1 > 1 faux
}
void test_anyTrue_zero_false(void) {
  TEST_ASSERT_FALSE(anyTrue(0));
}
void test_anyTrue_one_true(void) {
  TEST_ASSERT_TRUE(anyTrue(1));
}
void test_anyTrue_five_true(void) {
  TEST_ASSERT_TRUE(anyTrue(5));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // AC1
  RUN_TEST(test_bucketTimestamp_hourly_boundary);
  RUN_TEST(test_bucketTimestamp_hourly_above_boundary);
  RUN_TEST(test_bucketTimestamp_hourly_below_boundary);
  RUN_TEST(test_bucketTimestamp_hourly_zero_ts);
  RUN_TEST(test_bucketTimestamp_daily_boundary);
  RUN_TEST(test_bucketTimestamp_daily_above_boundary);
  RUN_TEST(test_bucketTimestamp_daily_below_boundary);
  RUN_TEST(test_bucketTimestamp_zero_bucket_guard);

  // AC2
  RUN_TEST(test_isOlderThan_age_zero);
  RUN_TEST(test_isOlderThan_age_equals_max);
  RUN_TEST(test_isOlderThan_age_max_plus_one);
  RUN_TEST(test_isOlderThan_uint32_wrap_within);
  RUN_TEST(test_isOlderThan_uint32_wrap_exceeds);

  // AC3
  RUN_TEST(test_finalizeMean_zero_count_is_nan);
  RUN_TEST(test_finalizeMean_integer_result);
  RUN_TEST(test_finalizeMean_fractional_result);

  // AC4
  RUN_TEST(test_isMajority_2_of_4_false);
  RUN_TEST(test_isMajority_3_of_4_true);
  RUN_TEST(test_isMajority_3_of_5_true);
  RUN_TEST(test_isMajority_0_of_0_false);
  RUN_TEST(test_isMajority_1_of_1_true);
  RUN_TEST(test_isMajority_1_of_2_false);
  RUN_TEST(test_anyTrue_zero_false);
  RUN_TEST(test_anyTrue_one_true);
  RUN_TEST(test_anyTrue_five_true);

  return UNITY_END();
}
