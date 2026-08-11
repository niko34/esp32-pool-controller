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
//   - historySaveAdmit (AC5, bug-nospc-partition-history : garde d'espace,
//     frontière large, anti-overflow SIZE_MAX)
// via l'API publique, pas l'implémentation interne.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdint.h>  // SIZE_MAX
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

// -----------------------------------------------------------------------------
// AC5 — historySaveAdmit (bug-nospc-partition-history)
// Sémantique : free + old >= json + marge → admis (frontière LARGE).
// -----------------------------------------------------------------------------
void test_saveAdmit_exact_boundary_admitted(void) {
  // 10000 + 20000 == 22000 + 8000 → admis (égalité exacte)
  TEST_ASSERT_TRUE(historySaveAdmit(10000, 20000, 22000, 8000));
}
void test_saveAdmit_one_byte_short_refused(void) {
  // 10000 + 20000 < 22001 + 8000 → refusé à 1 octet près
  TEST_ASSERT_FALSE(historySaveAdmit(10000, 20000, 22001, 8000));
}
void test_saveAdmit_one_byte_spare_admitted(void) {
  // 10001 + 20000 > 22000 + 8000 → admis
  TEST_ASSERT_TRUE(historySaveAdmit(10001, 20000, 22000, 8000));
}
void test_saveAdmit_no_old_file_admitted(void) {
  // Ancien fichier absent (old=0) : le libre seul doit couvrir json + marge
  TEST_ASSERT_TRUE(historySaveAdmit(30000, 0, 22000, 8000));
}
void test_saveAdmit_no_old_file_refused(void) {
  TEST_ASSERT_FALSE(historySaveAdmit(29999, 0, 22000, 8000));
}
void test_saveAdmit_zero_margin(void) {
  // Marge 0 : json seul décide (frontière large)
  TEST_ASSERT_TRUE(historySaveAdmit(22000, 0, 22000, 0));
  TEST_ASSERT_FALSE(historySaveAdmit(21999, 0, 22000, 0));
}
void test_saveAdmit_all_zero_admitted(void) {
  // Rien à écrire, rien de requis → admis (0 + 0 >= 0 + 0)
  TEST_ASSERT_TRUE(historySaveAdmit(0, 0, 0, 0));
}
void test_saveAdmit_overflow_required_side(void) {
  // json + marge déborderait size_t en addition naïve : SIZE_MAX + SIZE_MAX.
  // Disponible nul → refus (une addition débordante donnerait requis ≈ 0 → admis à tort).
  TEST_ASSERT_FALSE(historySaveAdmit(0, 0, SIZE_MAX, SIZE_MAX));
}
void test_saveAdmit_overflow_available_side(void) {
  // free + old déborderait size_t : SIZE_MAX + SIZE_MAX couvre SIZE_MAX + SIZE_MAX → admis.
  TEST_ASSERT_TRUE(historySaveAdmit(SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX));
}
void test_saveAdmit_size_max_exact(void) {
  // Disponible SIZE_MAX exactement requis SIZE_MAX → admis ; +1 de marge → refusé.
  TEST_ASSERT_TRUE(historySaveAdmit(SIZE_MAX, 0, SIZE_MAX, 0));
  TEST_ASSERT_FALSE(historySaveAdmit(SIZE_MAX, 0, SIZE_MAX, 1));
}


// ---------------------------------------------------------------------------
// bug-history-doublons-troncature — historyBucketConsolidable
// ---------------------------------------------------------------------------
// RAW_MAX_AGE = 6h = 21600 s ; bucket horaire = 3600 s.

void test_bucketConsolidable_partial_hour_refused(void) {
  // now = 10:00:00 + 6h : le bucket 04:00 court jusqu'à 04:59:59 — sa dernière
  // seconde a EXACTEMENT 6h+1s-3600... cas parlant : bucket dont seule une
  // partie des points dépasse 6h. now tel que bucketStart+3599 a 6h pile →
  // frontière stricte : NON consolidable.
  uint32_t bucketStart = 100000;
  uint32_t now = bucketStart + 3599 + 21600;  // dernière seconde : âge == maxAge
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketStart, 3600, 21600));
}

void test_bucketConsolidable_complete_hour_admitted(void) {
  uint32_t bucketStart = 100000;
  uint32_t now = bucketStart + 3599 + 21601;  // dernière seconde : âge > maxAge
  TEST_ASSERT_TRUE(historyBucketConsolidable(now, bucketStart, 3600, 21600));
}

void test_bucketConsolidable_first_second_old_but_bucket_partial(void) {
  // Le PREMIER point de l'heure a plus de 6h, mais pas la fin de l'heure →
  // l'ancien prédicat par point aurait consolidé (créant l'agrégat partiel),
  // le nouveau refuse — c'est LE cas du bug des doublons.
  uint32_t bucketStart = 100000;
  uint32_t now = bucketStart + 21601;  // début : 6h+1s ; fin : 6h+1s-3599 → trop jeune
  TEST_ASSERT_TRUE(isOlderThan(now, bucketStart, 21600));          // ancien : oui
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketStart, 3600, 21600));  // nouveau : non
}

void test_bucketConsolidable_zero_len_falls_back_to_start(void) {
  uint32_t bucketStart = 100000;
  TEST_ASSERT_TRUE(historyBucketConsolidable(bucketStart + 21601, bucketStart, 0, 21600));
  TEST_ASSERT_FALSE(historyBucketConsolidable(bucketStart + 21600, bucketStart, 0, 21600));
}

void test_bucketConsolidable_daily_bucket(void) {
  // HOURLY_MAX_AGE = 15 j = 1296000 s ; bucket jour = 86400 s.
  uint32_t bucketStart = 1000000;
  uint32_t lastSecond = bucketStart + 86399;
  TEST_ASSERT_FALSE(historyBucketConsolidable(lastSecond + 1296000, bucketStart, 86400, 1296000));
  TEST_ASSERT_TRUE(historyBucketConsolidable(lastSecond + 1296001, bucketStart, 86400, 1296000));
}

// ---------------------------------------------------------------------------
// bug-history-doublons-troncature — historyMergeMeanFinite
// ---------------------------------------------------------------------------

void test_mergeMean_both_finite_average(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.15f, historyMergeMeanFinite(7.1f, 7.2f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 565.5f, historyMergeMeanFinite(550.0f, 581.0f));
}

void test_mergeMean_one_finite_keeps_it(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.1f, historyMergeMeanFinite(7.1f, NAN));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.2f, historyMergeMeanFinite(NAN, 7.2f));
}

void test_mergeMean_none_finite_nan(void) {
  TEST_ASSERT_TRUE(isnan(historyMergeMeanFinite(NAN, NAN)));
  TEST_ASSERT_TRUE(isnan(historyMergeMeanFinite(INFINITY, NAN)));
}


// ---------------------------------------------------------------------------
// bug-points-futurs — historyIsExpired (future-safe) + bucket futur
// ---------------------------------------------------------------------------

void test_isExpired_past_old_true(void) {
  TEST_ASSERT_TRUE(historyIsExpired(1000000, 100000, 100));
}

void test_isExpired_past_recent_false(void) {
  TEST_ASSERT_FALSE(historyIsExpired(1000000, 999990, 100));
}

void test_isExpired_future_false(void) {
  // LE cas du bug : ts > now → le wrap uint32 rendait le point « très ancien ».
  // isOlderThan (invariant conservé) dit true ; historyIsExpired doit dire false.
  uint32_t now = 1786300000, tsFutur = now + 7200;
  TEST_ASSERT_TRUE(isOlderThan(now, tsFutur, 7776000));       // wrap : « vieux »
  TEST_ASSERT_FALSE(historyIsExpired(now, tsFutur, 7776000)); // garde : non expiré
}

void test_isExpired_boundary_now_equals_ts(void) {
  TEST_ASSERT_FALSE(historyIsExpired(1000000, 1000000, 100)); // âge 0 ≤ maxAge
}

void test_bucketConsolidable_future_bucket_refused(void) {
  // Bucket horaire dont la fin est dans le futur → jamais consolidable,
  // même si le wrap le ferait paraître ancien.
  uint32_t now = 1786300000;
  uint32_t bucketFutur = now + 3600;
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketFutur, 3600, 21600));
  // Bucket en cours (commencé, pas fini) : refusé aussi.
  uint32_t bucketEnCours = (now / 3600) * 3600;
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketEnCours, 3600, 21600));
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

  // AC5 — historySaveAdmit
  RUN_TEST(test_saveAdmit_exact_boundary_admitted);
  RUN_TEST(test_saveAdmit_one_byte_short_refused);
  RUN_TEST(test_saveAdmit_one_byte_spare_admitted);
  RUN_TEST(test_saveAdmit_no_old_file_admitted);
  RUN_TEST(test_saveAdmit_no_old_file_refused);
  RUN_TEST(test_saveAdmit_zero_margin);
  RUN_TEST(test_saveAdmit_all_zero_admitted);
  RUN_TEST(test_saveAdmit_overflow_required_side);
  RUN_TEST(test_saveAdmit_overflow_available_side);
  RUN_TEST(test_saveAdmit_size_max_exact);

  // bug-history-doublons-troncature
  RUN_TEST(test_bucketConsolidable_partial_hour_refused);
  RUN_TEST(test_bucketConsolidable_complete_hour_admitted);
  RUN_TEST(test_bucketConsolidable_first_second_old_but_bucket_partial);
  RUN_TEST(test_bucketConsolidable_zero_len_falls_back_to_start);
  RUN_TEST(test_bucketConsolidable_daily_bucket);
  RUN_TEST(test_mergeMean_both_finite_average);
  RUN_TEST(test_mergeMean_one_finite_keeps_it);
  RUN_TEST(test_mergeMean_none_finite_nan);

  // bug-points-futurs
  RUN_TEST(test_isExpired_past_old_true);
  RUN_TEST(test_isExpired_past_recent_false);
  RUN_TEST(test_isExpired_future_false);
  RUN_TEST(test_isExpired_boundary_now_equals_ts);
  RUN_TEST(test_bucketConsolidable_future_bucket_refused);

  return UNITY_END();
}
