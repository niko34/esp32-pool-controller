// =============================================================================
// Tests unitaires natifs — Clamp de pagination de GET /debug/sensor_trace
// (sensor_trace_page, v2.10.1, bug-crash-sensor-trace-badalloc)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du clamp PUR header-only
// src/sensor_trace_page.h (ADR-0017) :
//   - computeTracePage(total, offset, count, maxPerPage) → {offset, n}
//
// Règles verrouillées (source : en-tête du module) :
//   1. offset clampé à [0, total] — offset == total → page vide (n = 0) ;
//   2. count == 0 (absent ou explicite) → maxPerPage demandé ;
//   3. n = min(count demandé, maxPerPage, total - offset effectif).
//
// Invariants de sûreté (raison d'être du module — le crash bad_alloc venait
// d'une réponse non bornée) : pour TOUT input, y compris SIZE_MAX :
//   - b.offset <= total ;
//   - b.n <= maxPerPage ;
//   - b.offset + b.n <= total (jamais de lecture hors ring buffer).
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include "sensor_trace_page.h"

void setUp(void) {}
void tearDown(void) {}

// Valeurs du firmware : kSensorTraceBufferSize=200, kSensorTraceMaxSamplesPerPage=38
// (feature-153 : constantes de TEST, pas un contrat — à garder en phase avec
// constants.h pour éviter un faux rouge imputé au mauvais endroit, ADR-0073).
static constexpr size_t kTotal = 200;
static constexpr size_t kMax = 38;

// ---------------------------------------------------------------------------
// 1. Défauts (paramètres absents → offset=0, count=0)
// ---------------------------------------------------------------------------

void test_defaults_first_page(void) {
  TracePageBounds b = computeTracePage(kTotal, 0, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(0, b.offset);
  TEST_ASSERT_EQUAL_size_t(kMax, b.n);
}

void test_defaults_small_buffer_partial_page(void) {
  TracePageBounds b = computeTracePage(17, 0, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(0, b.offset);
  TEST_ASSERT_EQUAL_size_t(17, b.n);
}

void test_defaults_empty_buffer(void) {
  TracePageBounds b = computeTracePage(0, 0, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(0, b.offset);
  TEST_ASSERT_EQUAL_size_t(0, b.n);
}

// ---------------------------------------------------------------------------
// 2. Pagination nominale (enchaînement UI : offset += returned)
// ---------------------------------------------------------------------------

void test_middle_page(void) {
  TracePageBounds b = computeTracePage(kTotal, 100, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(100, b.offset);
  TEST_ASSERT_EQUAL_size_t(kMax, b.n);
}

void test_last_page_exact(void) {
  TracePageBounds b = computeTracePage(kTotal, 162, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(162, b.offset);
  TEST_ASSERT_EQUAL_size_t(38, b.n);  // 162 + 38 == 200, dernière page pleine
}

void test_last_page_partial(void) {
  TracePageBounds b = computeTracePage(280, 250, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(250, b.offset);
  TEST_ASSERT_EQUAL_size_t(30, b.n);  // il ne reste que 30 échantillons
}

void test_full_walk_covers_exactly_total(void) {
  // Simule la boucle UI : cumule n jusqu'à la page vide, vérifie total exact.
  size_t off = 0, sum = 0, guard = 0;
  while (guard++ < 12) {  // même borne dure que l'UI
    TracePageBounds b = computeTracePage(kTotal, off, 0, kMax);
    if (b.n == 0) break;
    sum += b.n;
    off += b.n;
  }
  TEST_ASSERT_EQUAL_size_t(kTotal, sum);
  TEST_ASSERT_EQUAL_size_t(kTotal, off);
}

// ---------------------------------------------------------------------------
// 3. Clamp offset (règle 1)
// ---------------------------------------------------------------------------

void test_offset_equals_total_empty_page(void) {
  TracePageBounds b = computeTracePage(kTotal, kTotal, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(kTotal, b.offset);
  TEST_ASSERT_EQUAL_size_t(0, b.n);
}

void test_offset_beyond_total_clamped(void) {
  TracePageBounds b = computeTracePage(kTotal, 9999, 0, kMax);
  TEST_ASSERT_EQUAL_size_t(kTotal, b.offset);
  TEST_ASSERT_EQUAL_size_t(0, b.n);
}

void test_offset_size_max_clamped(void) {
  TracePageBounds b = computeTracePage(kTotal, SIZE_MAX, SIZE_MAX, kMax);
  TEST_ASSERT_EQUAL_size_t(kTotal, b.offset);
  TEST_ASSERT_EQUAL_size_t(0, b.n);
}

// ---------------------------------------------------------------------------
// 4. Clamp count (règles 2-3)
// ---------------------------------------------------------------------------

void test_count_explicit_below_max(void) {
  TracePageBounds b = computeTracePage(kTotal, 0, 10, kMax);
  TEST_ASSERT_EQUAL_size_t(10, b.n);
}

void test_count_above_max_clamped(void) {
  TracePageBounds b = computeTracePage(kTotal, 0, 200, kMax);
  TEST_ASSERT_EQUAL_size_t(kMax, b.n);
}

void test_count_size_max_clamped(void) {
  TracePageBounds b = computeTracePage(kTotal, 0, SIZE_MAX, kMax);
  TEST_ASSERT_EQUAL_size_t(kMax, b.n);
}

void test_count_larger_than_remaining(void) {
  TracePageBounds b = computeTracePage(kTotal, 190, 50, kMax);
  TEST_ASSERT_EQUAL_size_t(190, b.offset);
  TEST_ASSERT_EQUAL_size_t(10, b.n);  // 200 - 190 == 10 restants, sous count ET kMax
}

// ---------------------------------------------------------------------------
// 5. Cas dégénérés — invariants de sûreté quel que soit l'input
// ---------------------------------------------------------------------------

void test_max_per_page_zero_yields_empty(void) {
  // maxPerPage=0 : configuration absurde → page vide, jamais de débordement.
  TracePageBounds b = computeTracePage(kTotal, 0, 0, 0);
  TEST_ASSERT_EQUAL_size_t(0, b.n);
}

void test_invariants_hold_on_adversarial_grid(void) {
  // Balayage adversarial : les 3 invariants tiennent pour toute combinaison.
  const size_t vals[] = {0, 1, 37, 38, 39, 199, 200, 201, 9999, SIZE_MAX};
  const size_t nVals = sizeof(vals) / sizeof(vals[0]);
  for (size_t t = 0; t < nVals; ++t) {
    for (size_t o = 0; o < nVals; ++o) {
      for (size_t c = 0; c < nVals; ++c) {
        TracePageBounds b = computeTracePage(vals[t], vals[o], vals[c], kMax);
        TEST_ASSERT_TRUE(b.offset <= vals[t]);
        TEST_ASSERT_TRUE(b.n <= kMax);
        TEST_ASSERT_TRUE(b.offset + b.n <= vals[t]);  // pas de lecture hors buffer
      }
    }
  }
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  // 1. Défauts
  RUN_TEST(test_defaults_first_page);
  RUN_TEST(test_defaults_small_buffer_partial_page);
  RUN_TEST(test_defaults_empty_buffer);
  // 2. Pagination nominale
  RUN_TEST(test_middle_page);
  RUN_TEST(test_last_page_exact);
  RUN_TEST(test_last_page_partial);
  RUN_TEST(test_full_walk_covers_exactly_total);
  // 3. Clamp offset
  RUN_TEST(test_offset_equals_total_empty_page);
  RUN_TEST(test_offset_beyond_total_clamped);
  RUN_TEST(test_offset_size_max_clamped);
  // 4. Clamp count
  RUN_TEST(test_count_explicit_below_max);
  RUN_TEST(test_count_above_max_clamped);
  RUN_TEST(test_count_size_max_clamped);
  RUN_TEST(test_count_larger_than_remaining);
  // 5. Cas dégénérés
  RUN_TEST(test_max_per_page_zero_yields_empty);
  RUN_TEST(test_invariants_hold_on_adversarial_grid);
  return UNITY_END();
}
