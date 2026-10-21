// =============================================================================
// Tests unitaires natifs — ota_base64_logic (feature-151 D1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// Décodeur base64 STRICT (RFC 4648, alphabet standard, padding '=').
// =============================================================================

#include <unity.h>
#include <string.h>

#include "ota_base64_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Cas valides
// -----------------------------------------------------------------------------
void test_decode_no_padding(void) {
  // "Man" -> "TWFu" (exemple canonique RFC 4648, aucun padding).
  uint8_t out[16];
  size_t n = otaBase64Decode("TWFu", 4, out, sizeof(out));
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL_UINT8('M', out[0]);
  TEST_ASSERT_EQUAL_UINT8('a', out[1]);
  TEST_ASSERT_EQUAL_UINT8('n', out[2]);
}

void test_decode_one_padding_char(void) {
  // "Ma" -> "TWE=" (1 octet de padding).
  uint8_t out[16];
  size_t n = otaBase64Decode("TWE=", 4, out, sizeof(out));
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT8('M', out[0]);
  TEST_ASSERT_EQUAL_UINT8('a', out[1]);
}

void test_decode_two_padding_chars(void) {
  // "M" -> "TQ==" (2 octets de padding).
  uint8_t out[16];
  size_t n = otaBase64Decode("TQ==", 4, out, sizeof(out));
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_UINT8('M', out[0]);
}

void test_decode_multi_group(void) {
  // "Many hands make light work." (RFC 4648 test vector, tronqué pour lisibilité)
  // "light work." -> "bGlnaHQgd29yay4="
  uint8_t out[32];
  const char* b64 = "bGlnaHQgd29yay4=";
  size_t n = otaBase64Decode(b64, strlen(b64), out, sizeof(out));
  TEST_ASSERT_EQUAL(11, n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY("light work.", out, 11);
}

void test_decode_all_zero_bytes(void) {
  // 384 octets à zéro (taille exacte d'une signature RSA-3072) encodés en
  // base64 standard : 128 groupes de "AAAA" = 512 caractères, aucun padding.
  char b64[513];
  for (int i = 0; i < 128; ++i) {
    memcpy(b64 + i * 4, "AAAA", 4);
  }
  b64[512] = '\0';
  uint8_t out[384];
  size_t n = otaBase64Decode(b64, 512, out, sizeof(out));
  TEST_ASSERT_EQUAL(384, n);
  for (size_t i = 0; i < 384; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0, out[i]);
  }
}

// -----------------------------------------------------------------------------
// Cas invalides (fail-closed → 0)
// -----------------------------------------------------------------------------
void test_reject_null_in(void) {
  uint8_t out[16];
  TEST_ASSERT_EQUAL(0, otaBase64Decode(nullptr, 4, out, sizeof(out)));
}

void test_reject_null_out(void) {
  TEST_ASSERT_EQUAL(0, otaBase64Decode("TWFu", 4, nullptr, 16));
}

void test_reject_zero_length(void) {
  uint8_t out[16];
  TEST_ASSERT_EQUAL(0, otaBase64Decode("", 0, out, sizeof(out)));
}

void test_reject_length_not_multiple_of_4(void) {
  uint8_t out[16];
  TEST_ASSERT_EQUAL(0, otaBase64Decode("TWF", 3, out, sizeof(out)));
  TEST_ASSERT_EQUAL(0, otaBase64Decode("TWFub", 5, out, sizeof(out)));
}

void test_reject_invalid_character(void) {
  uint8_t out[16];
  // '!' hors alphabet.
  TEST_ASSERT_EQUAL(0, otaBase64Decode("TW!u", 4, out, sizeof(out)));
}

void test_reject_padding_in_middle_group(void) {
  uint8_t out[16];
  // 2 groupes, padding dans le PREMIER (non-dernier) : invalide.
  TEST_ASSERT_EQUAL(0, otaBase64Decode("TQ==TWFu", 8, out, sizeof(out)));
}

void test_reject_padding_misplaced_within_last_group(void) {
  uint8_t out[16];
  // '=' en position 1 d'un groupe final de padding count 1 (seule la
  // position 3 est autorisée) : invalide.
  TEST_ASSERT_EQUAL(0, otaBase64Decode("T=Fu", 4, out, sizeof(out)));
}

void test_reject_three_padding_chars(void) {
  uint8_t out[16];
  // 3 '=' d'affilée : jamais valide en base64 standard.
  TEST_ASSERT_EQUAL(0, otaBase64Decode("A===", 4, out, sizeof(out)));
}

void test_reject_output_buffer_too_small(void) {
  uint8_t out[2];  // "TWFu" décode en 3 octets -> ne tient pas dans 2
  TEST_ASSERT_EQUAL(0, otaBase64Decode("TWFu", 4, out, sizeof(out)));
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_decode_no_padding);
  RUN_TEST(test_decode_one_padding_char);
  RUN_TEST(test_decode_two_padding_chars);
  RUN_TEST(test_decode_multi_group);
  RUN_TEST(test_decode_all_zero_bytes);

  RUN_TEST(test_reject_null_in);
  RUN_TEST(test_reject_null_out);
  RUN_TEST(test_reject_zero_length);
  RUN_TEST(test_reject_length_not_multiple_of_4);
  RUN_TEST(test_reject_invalid_character);
  RUN_TEST(test_reject_padding_in_middle_group);
  RUN_TEST(test_reject_padding_misplaced_within_last_group);
  RUN_TEST(test_reject_three_padding_chars);
  RUN_TEST(test_reject_output_buffer_too_small);

  return UNITY_END();
}
