// =============================================================================
// Tests unitaires natifs — Logique pure de l'export chunked /get-history
// (history_export_chunk, bug-heap-toctou A3)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/history_export_chunk.h
// (ADR-0017) :
//   - historyRangeCutoff : borne du filtre ?range=, sémantique EXACTE de
//     HistoryManager::getLastHours (0 = tout renvoyer) ;
//   - formatHistoryPointJson : format JSON octet pour octet identique à
//     l'ancienne concaténation String (arrondis, null sur NaN, virgule de
//     séparation, JAMAIS de JSON partiel si cap trop petit).
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "history_export_chunk.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. historyRangeCutoff — sémantique getLastHours
// ---------------------------------------------------------------------------

void test_cutoff_now_epoch_zero_returns_all(void) {
  // Heure indisponible → 0 = tout renvoyer (comme getLastHours avec nowEpoch==0)
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 0));
}

void test_cutoff_now_smaller_than_range_returns_all(void) {
  // now < range (epoch aberrant, ex. uptime provisoire) → 0 = tout renvoyer
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 3600UL));       // 1h < 24h
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 86399UL));      // limite - 1
}

void test_cutoff_boundary_now_equals_range(void) {
  // now == range exactement : pas d'underflow → cutoff 0 (borne incluse)
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 86400UL));
}

void test_cutoff_nominal(void) {
  // Nominal : cutoff = now - hours*3600
  unsigned long now = 1700000000UL;
  TEST_ASSERT_EQUAL_UINT32(1700000000UL - 86400UL,
                           (uint32_t)historyRangeCutoff(24, now));       // 24h
  TEST_ASSERT_EQUAL_UINT32(1700000000UL - 7UL * 86400UL,
                           (uint32_t)historyRangeCutoff(24 * 7, now));   // 7d
}

void test_cutoff_hours_zero_or_negative_returns_all(void) {
  // hours <= 0 = mode "all" (aucun filtre)
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(0, 1700000000UL));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(-5, 1700000000UL));
}

// ---------------------------------------------------------------------------
// 2. formatHistoryPointJson — contrat octet pour octet
// ---------------------------------------------------------------------------

void test_format_nominal_first(void) {
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000000UL,
                                    7.234f, 650.4f, 24.56f,
                                    true, false, 0, /*first=*/true);
  const char* expected =
      "{\"timestamp\":1700000000,\"ph\":7.2,\"orp\":650,\"temperature\":24.6,"
      "\"filtration\":true,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_not_first_has_leading_comma(void) {
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000300UL,
                                    7.05f, 649.6f, 24.0f,
                                    false, true, 1, /*first=*/false);
  // 7.05*10 = 70.5 → roundf = 71 → 7.1 ; 649.6 → 650 ; 24.0 → "24.0"
  const char* expected =
      ",{\"timestamp\":1700000300,\"ph\":7.1,\"orp\":650,\"temperature\":24.0,"
      "\"filtration\":false,\"dosing\":true,\"granularity\":1}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_nan_becomes_null(void) {
  // NaN (mesures exclues, feature-061) → null explicite pour ph/orp/temperature
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000600UL,
                                    NAN, NAN, NAN,
                                    true, false, 2, /*first=*/true);
  const char* expected =
      "{\"timestamp\":1700000600,\"ph\":null,\"orp\":null,\"temperature\":null,"
      "\"filtration\":true,\"dosing\":false,\"granularity\":2}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_infinity_becomes_null(void) {
  // isfinite() couvre aussi ±inf (même garde que l'ancienne route)
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000900UL,
                                    INFINITY, -INFINITY, 24.5f,
                                    false, false, 0, /*first=*/true);
  const char* expected =
      "{\"timestamp\":1700000900,\"ph\":null,\"orp\":null,\"temperature\":24.5,"
      "\"filtration\":false,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_rounding_rules(void) {
  // pH/temp : arrondi au dixième (roundf(x*10)/10 puis 1 décimale) ;
  // ORP : entier arrondi au plus proche.
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1UL,
                                    7.249f, 650.5f, 24.94f,
                                    false, false, 0, true);
  // 7.249*10 = 72.49 → 72 → 7.2 ; 650.5 → 651 (roundf half away) ; 24.94 → 24.9
  const char* expected =
      "{\"timestamp\":1,\"ph\":7.2,\"orp\":651,\"temperature\":24.9,"
      "\"filtration\":false,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_cap_too_small_returns_zero(void) {
  // Le point ne tient pas dans cap → 0, JAMAIS de JSON partiel émis
  char out[32];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000000UL,
                                    7.2f, 650.0f, 24.5f,
                                    true, false, 0, true);
  TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_format_cap_zero_or_null_returns_zero(void) {
  char out[8];
  TEST_ASSERT_EQUAL_size_t(0, formatHistoryPointJson(out, 0, 1UL, 7.0f, 650.0f,
                                                     24.0f, false, false, 0, true));
  TEST_ASSERT_EQUAL_size_t(0, formatHistoryPointJson(nullptr, 8, 1UL, 7.0f, 650.0f,
                                                     24.0f, false, false, 0, true));
}

void test_format_fits_in_firmware_carry_capacity(void) {
  // Pire cas réaliste (timestamp 10 chiffres, valeurs négatives) : doit tenir
  // dans kHistoryPointJsonMaxBytes=128 (capacité du carry firmware).
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 4294967295UL,
                                    -1.25f, -999.9f, -12.34f,
                                    true, true, 2, /*first=*/false);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < 128);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cutoff_now_epoch_zero_returns_all);
  RUN_TEST(test_cutoff_now_smaller_than_range_returns_all);
  RUN_TEST(test_cutoff_boundary_now_equals_range);
  RUN_TEST(test_cutoff_nominal);
  RUN_TEST(test_cutoff_hours_zero_or_negative_returns_all);
  RUN_TEST(test_format_nominal_first);
  RUN_TEST(test_format_not_first_has_leading_comma);
  RUN_TEST(test_format_nan_becomes_null);
  RUN_TEST(test_format_infinity_becomes_null);
  RUN_TEST(test_format_rounding_rules);
  RUN_TEST(test_format_cap_too_small_returns_zero);
  RUN_TEST(test_format_cap_zero_or_null_returns_zero);
  RUN_TEST(test_format_fits_in_firmware_carry_capacity);
  return UNITY_END();
}
