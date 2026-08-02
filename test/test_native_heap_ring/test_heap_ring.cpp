// =============================================================================
// Tests unitaires natifs — Encodage/index du ring heap RTC noinit
// (heap_ring_logic, bug-heap-toctou A4)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/heap_ring_logic.h
// (ADR-0017) :
//   - heapRingEncode/heapRingDecode : entrée 8 octets little-endian quantifiée
//     (uptime mod 65536, maxAlloc>>6, freeHeap>>8, wsClients/skip saturés 255) ;
//   - heapRingNextHead / heapRingIndexOfLast / heapRingDumpCount / heapRingAgeS :
//     arithmétique d'index du ring (wrap) et âge modulaire u16.
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "heap_ring_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. Encode/décode — aller-retour exact sur les multiples des quanta
// ---------------------------------------------------------------------------

void test_encode_decode_roundtrip_exact(void) {
  uint8_t e[kHeapRingEntryBytes];
  // maxAlloc multiple de 64, freeHeap multiple de 256 → aller-retour sans perte
  heapRingEncode(e, 1234, 18624, 45312, 2, 7);
  HeapRingSampleFields f = heapRingDecode(e);
  TEST_ASSERT_EQUAL_UINT16(1234, f.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(18624, f.maxAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(45312, f.freeHeapBytes);
  TEST_ASSERT_EQUAL_UINT8(2, f.wsClients);
  TEST_ASSERT_EQUAL_UINT8(7, f.skippedDelta);
}

void test_encode_decode_quantization_floors(void) {
  uint8_t e[kHeapRingEntryBytes];
  // Non-multiples : arrondi au quantum INFÉRIEUR (décalage de bits)
  heapRingEncode(e, 0, 18624 + 63, 45312 + 255, 0, 0);
  HeapRingSampleFields f = heapRingDecode(e);
  TEST_ASSERT_EQUAL_UINT32(18624, f.maxAllocBytes);   // 18687 >> 6 << 6
  TEST_ASSERT_EQUAL_UINT32(45312, f.freeHeapBytes);   // 45567 >> 8 << 8
}

void test_encode_zero_values(void) {
  uint8_t e[kHeapRingEntryBytes];
  heapRingEncode(e, 0, 0, 0, 0, 0);
  HeapRingSampleFields f = heapRingDecode(e);
  TEST_ASSERT_EQUAL_UINT16(0, f.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(0, f.maxAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(0, f.freeHeapBytes);
  TEST_ASSERT_EQUAL_UINT8(0, f.wsClients);
  TEST_ASSERT_EQUAL_UINT8(0, f.skippedDelta);
}

// ---------------------------------------------------------------------------
// 2. Saturations et modulo
// ---------------------------------------------------------------------------

void test_uptime_wraps_modulo_65536(void) {
  uint8_t e[kHeapRingEntryBytes];
  heapRingEncode(e, 65536 + 42, 0, 0, 0, 0);
  TEST_ASSERT_EQUAL_UINT16(42, heapRingDecode(e).uptimeS);
}

void test_max_alloc_saturates_at_u16_quantum(void) {
  uint8_t e[kHeapRingEntryBytes];
  // > 65535*64 = 4 194 240 o → plafonné à la dynamique u16 post-quantum
  heapRingEncode(e, 0, 8 * 1024 * 1024, 0, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(65535UL << 6, heapRingDecode(e).maxAllocBytes);
}

void test_free_heap_saturates_at_u16_quantum(void) {
  uint8_t e[kHeapRingEntryBytes];
  heapRingEncode(e, 0, 0, 32UL * 1024 * 1024, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(65535UL << 8, heapRingDecode(e).freeHeapBytes);
}

void test_ws_clients_and_skip_delta_saturate_255(void) {
  uint8_t e[kHeapRingEntryBytes];
  heapRingEncode(e, 0, 0, 0, 300, 1000);
  HeapRingSampleFields f = heapRingDecode(e);
  TEST_ASSERT_EQUAL_UINT8(255, f.wsClients);
  TEST_ASSERT_EQUAL_UINT8(255, f.skippedDelta);
}

// ---------------------------------------------------------------------------
// 3. Arithmétique d'index du ring
// ---------------------------------------------------------------------------

void test_next_head_increments_then_wraps(void) {
  TEST_ASSERT_EQUAL_size_t(1, heapRingNextHead(0, 8));
  TEST_ASSERT_EQUAL_size_t(7, heapRingNextHead(6, 8));
  TEST_ASSERT_EQUAL_size_t(0, heapRingNextHead(7, 8));  // wrap
}

void test_dump_count_is_min(void) {
  TEST_ASSERT_EQUAL_size_t(5, heapRingDumpCount(5, 32));    // ring pas encore plein
  TEST_ASSERT_EQUAL_size_t(32, heapRingDumpCount(128, 32)); // plafond demandé
  TEST_ASSERT_EQUAL_size_t(0, heapRingDumpCount(0, 32));    // vide
}

void test_index_of_last_no_wrap(void) {
  // head=5 (prochaine écriture), n=3 dernières → indices 2,3,4
  TEST_ASSERT_EQUAL_size_t(2, heapRingIndexOfLast(5, 3, 8, 0));  // plus ancienne
  TEST_ASSERT_EQUAL_size_t(3, heapRingIndexOfLast(5, 3, 8, 1));
  TEST_ASSERT_EQUAL_size_t(4, heapRingIndexOfLast(5, 3, 8, 2));  // plus récente
}

void test_index_of_last_with_wrap(void) {
  // head=1, n=3 dernières → indices 6,7,0 (wrap autour de la fin du ring)
  TEST_ASSERT_EQUAL_size_t(6, heapRingIndexOfLast(1, 3, 8, 0));
  TEST_ASSERT_EQUAL_size_t(7, heapRingIndexOfLast(1, 3, 8, 1));
  TEST_ASSERT_EQUAL_size_t(0, heapRingIndexOfLast(1, 3, 8, 2));
}

void test_index_of_last_full_ring(void) {
  // Ring plein (n == capacity), head=3 : la plus ancienne est à head lui-même
  TEST_ASSERT_EQUAL_size_t(3, heapRingIndexOfLast(3, 8, 8, 0));
  TEST_ASSERT_EQUAL_size_t(2, heapRingIndexOfLast(3, 8, 8, 7));  // plus récente = head-1
}

// ---------------------------------------------------------------------------
// 4. Âge modulaire u16
// ---------------------------------------------------------------------------

void test_age_nominal(void) {
  TEST_ASSERT_EQUAL_UINT16(0, heapRingAgeS(100, 100));
  TEST_ASSERT_EQUAL_UINT16(62, heapRingAgeS(100, 38));
}

void test_age_across_u16_wrap(void) {
  // uptime a bouclé (65530 → 10) : l'âge modulaire reste correct (16 s)
  TEST_ASSERT_EQUAL_UINT16(16, heapRingAgeS(10, 65530));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_roundtrip_exact);
  RUN_TEST(test_encode_decode_quantization_floors);
  RUN_TEST(test_encode_zero_values);
  RUN_TEST(test_uptime_wraps_modulo_65536);
  RUN_TEST(test_max_alloc_saturates_at_u16_quantum);
  RUN_TEST(test_free_heap_saturates_at_u16_quantum);
  RUN_TEST(test_ws_clients_and_skip_delta_saturate_255);
  RUN_TEST(test_next_head_increments_then_wraps);
  RUN_TEST(test_dump_count_is_min);
  RUN_TEST(test_index_of_last_no_wrap);
  RUN_TEST(test_index_of_last_with_wrap);
  RUN_TEST(test_index_of_last_full_ring);
  RUN_TEST(test_age_nominal);
  RUN_TEST(test_age_across_u16_wrap);
  return UNITY_END();
}
