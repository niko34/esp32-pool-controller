// =============================================================================
// Tests unitaires natifs — Report de ligne (« carry ») de l'export chunked
// (logs_export_chunk, v2.10.4, bug-crash-getlogs-badalloc-audit-routes)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du report PUR header-only
// src/logs_export_chunk.h (ADR-0017) :
//   - LogLineCarry<N> {line, len, pos} — ligne en cours d'émission ;
//   - emitCarry(c, buf, maxLen) → octets écrits, c.pos avance.
//
// Garanties verrouillées (source : en-tête du module) :
//   - retourne 0 SEULEMENT si carry épuisé (pos >= len) OU maxLen == 0 ;
//   - PROGRESSION : jamais 0 si (pos < len) et maxLen > 0 — AsyncWebServer
//     interpréterait un 0 comme la fin de la réponse chunked ;
//   - jamais d'écriture au-delà de maxLen dans buf ;
//   - le contenu émis, recollé sur N appels, est identique à line[0..len).
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "logs_export_chunk.h"

void setUp(void) {}
void tearDown(void) {}

// Capacité de test arbitraire (le firmware instancie kLogExportLineMaxBytes=256).
static constexpr size_t kN = 64;

static LogLineCarry<kN> makeCarry(const char* s) {
  LogLineCarry<kN> c;
  c.len = strlen(s);
  c.pos = 0;
  memcpy(c.line, s, c.len);
  return c;
}

static constexpr uint8_t kCanary = 0x7f;

// ---------------------------------------------------------------------------
// 1. Nominal — ligne plus courte que le buffer : émise en un appel
// ---------------------------------------------------------------------------

void test_line_fits_single_call(void) {
  LogLineCarry<kN> c = makeCarry("12345 WARN hello\n");
  uint8_t buf[32];
  size_t n = emitCarry(c, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_size_t(17, n);
  TEST_ASSERT_EQUAL_MEMORY("12345 WARN hello\n", buf, 17);
  TEST_ASSERT_EQUAL_size_t(c.len, c.pos);           // carry épuisé
  TEST_ASSERT_EQUAL_size_t(0, emitCarry(c, buf, sizeof(buf)));  // plus rien
}

// ---------------------------------------------------------------------------
// 2. Ligne plus longue que maxLen : répartie sur N appels, contenu intact
// ---------------------------------------------------------------------------

void test_line_split_across_chunks(void) {
  const char* s = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";  // 26 octets
  LogLineCarry<kN> c = makeCarry(s);
  uint8_t out[32];
  size_t total = 0;
  uint8_t buf[7];
  while (true) {
    size_t n = emitCarry(c, buf, sizeof(buf));
    if (n == 0) break;
    TEST_ASSERT_TRUE(n <= sizeof(buf));
    memcpy(out + total, buf, n);
    total += n;
    TEST_ASSERT_TRUE(total <= 26);
  }
  TEST_ASSERT_EQUAL_size_t(26, total);
  TEST_ASSERT_EQUAL_MEMORY(s, out, 26);
}

void test_progression_guarantee_maxlen_one(void) {
  // maxLen=1 : chaque appel doit émettre exactement 1 octet, jamais 0
  // tant que le carry n'est pas épuisé (garantie anti-fin-de-flux prématurée).
  LogLineCarry<kN> c = makeCarry("xyz");
  uint8_t b;
  TEST_ASSERT_EQUAL_size_t(1, emitCarry(c, &b, 1));
  TEST_ASSERT_EQUAL_CHAR('x', (char)b);
  TEST_ASSERT_EQUAL_size_t(1, emitCarry(c, &b, 1));
  TEST_ASSERT_EQUAL_CHAR('y', (char)b);
  TEST_ASSERT_EQUAL_size_t(1, emitCarry(c, &b, 1));
  TEST_ASSERT_EQUAL_CHAR('z', (char)b);
  TEST_ASSERT_EQUAL_size_t(0, emitCarry(c, &b, 1));  // épuisé : fin légitime
}

// ---------------------------------------------------------------------------
// 3. Bornes — pas d'écriture au-delà de maxLen ; maxLen == 0
// ---------------------------------------------------------------------------

void test_no_write_beyond_maxlen(void) {
  LogLineCarry<kN> c = makeCarry("0123456789");
  uint8_t buf[8];
  memset(buf, kCanary, sizeof(buf));
  size_t n = emitCarry(c, buf, 4);
  TEST_ASSERT_EQUAL_size_t(4, n);
  TEST_ASSERT_EQUAL_MEMORY("0123", buf, 4);
  for (size_t i = 4; i < sizeof(buf); ++i) {
    TEST_ASSERT_EQUAL_UINT8(kCanary, buf[i]);       // rien écrit après maxLen
  }
}

void test_maxlen_zero_returns_zero_without_progress(void) {
  LogLineCarry<kN> c = makeCarry("abc");
  uint8_t buf[4];
  TEST_ASSERT_EQUAL_size_t(0, emitCarry(c, buf, 0));
  TEST_ASSERT_EQUAL_size_t(0, c.pos);               // aucune avance
}

void test_empty_carry_returns_zero(void) {
  LogLineCarry<kN> c;  // len = 0, pos = 0 (défauts de la struct)
  uint8_t buf[4];
  TEST_ASSERT_EQUAL_size_t(0, emitCarry(c, buf, sizeof(buf)));
}

// ---------------------------------------------------------------------------
// 4. Réutilisation — carry rechargé après épuisement (cycle par entrée RAM)
// ---------------------------------------------------------------------------

void test_carry_reload_cycle(void) {
  LogLineCarry<kN> c = makeCarry("ligne1\n");
  uint8_t buf[16];
  TEST_ASSERT_EQUAL_size_t(7, emitCarry(c, buf, sizeof(buf)));
  // Rechargement (nouvelle entrée) : le firmware réécrit line/len et remet pos=0.
  c = makeCarry("deuxième ligne\n");
  size_t n = emitCarry(c, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_size_t(c.len, n);
  TEST_ASSERT_EQUAL_size_t(c.len, c.pos);
}

// ---------------------------------------------------------------------------
// 5. Capacité pleine — ligne occupant exactement N octets
// ---------------------------------------------------------------------------

void test_line_at_full_capacity(void) {
  LogLineCarry<8> c;
  memcpy(c.line, "ABCDEFGH", 8);  // exactement N, pas de NUL requis
  c.len = 8;
  c.pos = 0;
  uint8_t buf[3];
  size_t total = 0;
  size_t n;
  uint8_t out[8];
  while ((n = emitCarry(c, buf, sizeof(buf))) > 0) {
    memcpy(out + total, buf, n);
    total += n;
  }
  TEST_ASSERT_EQUAL_size_t(8, total);
  TEST_ASSERT_EQUAL_MEMORY("ABCDEFGH", out, 8);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_line_fits_single_call);
  RUN_TEST(test_line_split_across_chunks);
  RUN_TEST(test_progression_guarantee_maxlen_one);
  RUN_TEST(test_no_write_beyond_maxlen);
  RUN_TEST(test_maxlen_zero_returns_zero_without_progress);
  RUN_TEST(test_empty_carry_returns_zero);
  RUN_TEST(test_carry_reload_cycle);
  RUN_TEST(test_line_at_full_capacity);
  return UNITY_END();
}
