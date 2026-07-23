// =============================================================================
// Tests unitaires natifs — Normalisation du préfixe de topics MQTT
// (mqtt_topics_logic, v2.10.x, bug-race-refreshtopics-uaf)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la normalisation PURE extraite dans
// src/mqtt_topics_logic.{h,cpp} (ADR-0017, comme history_guard_logic /
// net_health_logic / dosing_logic) :
//   - normalizeMqttBase(raw, out, outLen)
//
// Règles (ordre figé, identique à l'ancien code inline de refreshTopics()) :
//   1. trim des blancs (espace, \t, \n, \v, \f, \r) en tête ET en queue —
//      mêmes caractères que String::trim() Arduino ;
//   2. suppression des '/' finaux, y compris multiples ("pool///" → "pool") ;
//      PAS de re-trim ensuite : "pool /" → "pool " (espace interne conservé,
//      comportement historique verrouillé tel quel) ;
//   3. vide ou réduit à rien après 1-2 → fallback kMqttDefaultBase
//      ("pool/sensors").
//
// Robustesse verrouillée :
//   - raw == NULL traité comme vide (→ fallback) ;
//   - out toujours NUL-terminé, troncature silencieuse à outLen-1 (jamais
//     d'écriture au-delà de outLen — y compris pour le fallback) ;
//   - out == NULL ou outLen == 0 → no-op strict.
//
// Le guard anti-rebuild de refreshTopics() (early-return si base inchangée)
// repose sur le DÉTERMINISME de cette normalisation (même entrée brute →
// même sortie), PAS sur son idempotence : le guard normalise toujours depuis
// la config brute (mqttCfg.topic) et compare à la sortie précédente — il ne
// re-normalise jamais sa propre sortie. Exception d'idempotence assumée
// (comportement historique) : "pool /" → "pool " puis "pool " → "pool"
// (le trim d'un 2ᵉ passage mange l'espace exposé par le strip des '/').
// =============================================================================

#include <unity.h>
#include <string.h>
#include "mqtt_topics_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Sentinelle pour détecter toute écriture hors bornes.
static constexpr char kCanary = '\x7f';

static void fillCanary(char* buf, size_t len) { memset(buf, kCanary, len); }

// ---------------------------------------------------------------------------
// 1. Cas nominaux
// ---------------------------------------------------------------------------

void test_nominal_default_base_unchanged(void) {
  char out[64];
  normalizeMqttBase("pool/sensors", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_nominal_custom_prefix_preserved(void) {
  char out[64];
  normalizeMqttBase("maison/piscine", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("maison/piscine", out);
}

void test_nominal_single_segment(void) {
  char out[64];
  normalizeMqttBase("pool", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool", out);
}

// ---------------------------------------------------------------------------
// 2. Trim des blancs (tête et queue, tous les caractères de String::trim())
// ---------------------------------------------------------------------------

void test_trim_leading_spaces(void) {
  char out[64];
  normalizeMqttBase("   pool/sensors", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_trim_trailing_spaces(void) {
  char out[64];
  normalizeMqttBase("pool/sensors   ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_trim_all_whitespace_kinds(void) {
  char out[64];
  normalizeMqttBase(" \t\n\v\f\rpool\t\r\n ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool", out);
}

void test_inner_space_preserved(void) {
  char out[64];
  normalizeMqttBase("ma piscine/mqtt", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("ma piscine/mqtt", out);
}

// ---------------------------------------------------------------------------
// 3. Suppression des '/' finaux
// ---------------------------------------------------------------------------

void test_strip_single_trailing_slash(void) {
  char out[64];
  normalizeMqttBase("pool/sensors/", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_strip_multiple_trailing_slashes(void) {
  char out[64];
  normalizeMqttBase("pool///", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool", out);
}

void test_leading_slash_preserved(void) {
  char out[64];
  normalizeMqttBase("/pool", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("/pool", out);
}

void test_trim_then_strip_slashes(void) {
  char out[64];
  normalizeMqttBase("  pool//  ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool", out);
}

// Comportement historique verrouillé : pas de re-trim après le strip des '/'.
void test_no_retrim_after_slash_strip(void) {
  char out[64];
  normalizeMqttBase("pool /", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool ", out);
}

// ---------------------------------------------------------------------------
// 4. Fallback kMqttDefaultBase
// ---------------------------------------------------------------------------

void test_fallback_empty_string(void) {
  char out[64];
  normalizeMqttBase("", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_fallback_null_raw(void) {
  char out[64];
  normalizeMqttBase(nullptr, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_fallback_whitespace_only(void) {
  char out[64];
  normalizeMqttBase("  \t\r\n  ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_fallback_single_slash(void) {
  char out[64];
  normalizeMqttBase("/", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_fallback_only_slashes(void) {
  char out[64];
  normalizeMqttBase("///", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

void test_fallback_spaces_and_slashes(void) {
  char out[64];
  normalizeMqttBase("  ///  ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("pool/sensors", out);
}

// ---------------------------------------------------------------------------
// 5. Déterminisme (prérequis du guard anti-rebuild de refreshTopics()) et
//    idempotence sur entrées bien formées
// ---------------------------------------------------------------------------

void test_deterministic_same_raw_same_output(void) {
  char run1[64];
  char run2[64];
  const char* inputs[] = {"pool/sensors", "  maison/piscine// ", "", "///", "pool /"};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    normalizeMqttBase(inputs[i], run1, sizeof(run1));
    normalizeMqttBase(inputs[i], run2, sizeof(run2));
    TEST_ASSERT_EQUAL_STRING(run1, run2);
  }
}

void test_idempotent_on_wellformed_output(void) {
  char pass1[64];
  char pass2[64];
  const char* inputs[] = {"pool/sensors", "  maison/piscine// ", "", "///"};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    normalizeMqttBase(inputs[i], pass1, sizeof(pass1));
    normalizeMqttBase(pass1, pass2, sizeof(pass2));
    TEST_ASSERT_EQUAL_STRING(pass1, pass2);
  }
}

// Exception d'idempotence assumée (comportement historique, cf. en-tête) :
// "pool /" → "pool " (1ᵉʳ passage) mais "pool " → "pool" (2ᵉ passage).
// Sans conséquence : le guard ne re-normalise jamais sa propre sortie.
void test_known_non_idempotent_edge_trailing_space(void) {
  char pass1[64];
  char pass2[64];
  normalizeMqttBase("pool /", pass1, sizeof(pass1));
  TEST_ASSERT_EQUAL_STRING("pool ", pass1);
  normalizeMqttBase(pass1, pass2, sizeof(pass2));
  TEST_ASSERT_EQUAL_STRING("pool", pass2);
}

// ---------------------------------------------------------------------------
// 6. Troncature et bornes du buffer (jamais d'écriture hors [0, outLen-1])
// ---------------------------------------------------------------------------

void test_truncation_long_input(void) {
  char buf[8 + 1];  // 8 utiles + 1 sentinelle après out
  fillCanary(buf, sizeof(buf));
  normalizeMqttBase("abcdefghijklmnop", buf, 8);
  TEST_ASSERT_EQUAL_STRING("abcdefg", buf);          // 7 chars + NUL
  TEST_ASSERT_EQUAL_CHAR(kCanary, buf[8]);           // rien écrit après outLen
}

void test_truncation_fallback_also_bounded(void) {
  char buf[6 + 1];  // fallback "pool/sensors" (12) tronqué dans 6
  fillCanary(buf, sizeof(buf));
  normalizeMqttBase("", buf, 6);
  TEST_ASSERT_EQUAL_STRING("pool/", buf);            // 5 chars + NUL
  TEST_ASSERT_EQUAL_CHAR(kCanary, buf[6]);
}

void test_outlen_one_yields_empty_string(void) {
  char buf[1 + 1];
  fillCanary(buf, sizeof(buf));
  normalizeMqttBase("pool", buf, 1);
  TEST_ASSERT_EQUAL_STRING("", buf);                 // NUL seul
  TEST_ASSERT_EQUAL_CHAR(kCanary, buf[1]);
}

void test_noop_when_out_null(void) {
  normalizeMqttBase("pool", nullptr, 64);            // ne doit pas crasher
  TEST_PASS();
}

void test_noop_when_outlen_zero(void) {
  char buf[2];
  fillCanary(buf, sizeof(buf));
  normalizeMqttBase("pool", buf, 0);
  TEST_ASSERT_EQUAL_CHAR(kCanary, buf[0]);           // strictement intouché
  TEST_ASSERT_EQUAL_CHAR(kCanary, buf[1]);
}

void test_exact_fit_no_truncation(void) {
  char buf[5];                                       // "pool" + NUL = 5, pile
  normalizeMqttBase("pool", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("pool", buf);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  // 1. Nominaux
  RUN_TEST(test_nominal_default_base_unchanged);
  RUN_TEST(test_nominal_custom_prefix_preserved);
  RUN_TEST(test_nominal_single_segment);
  // 2. Trim
  RUN_TEST(test_trim_leading_spaces);
  RUN_TEST(test_trim_trailing_spaces);
  RUN_TEST(test_trim_all_whitespace_kinds);
  RUN_TEST(test_inner_space_preserved);
  // 3. Slashes finaux
  RUN_TEST(test_strip_single_trailing_slash);
  RUN_TEST(test_strip_multiple_trailing_slashes);
  RUN_TEST(test_leading_slash_preserved);
  RUN_TEST(test_trim_then_strip_slashes);
  RUN_TEST(test_no_retrim_after_slash_strip);
  // 4. Fallback
  RUN_TEST(test_fallback_empty_string);
  RUN_TEST(test_fallback_null_raw);
  RUN_TEST(test_fallback_whitespace_only);
  RUN_TEST(test_fallback_single_slash);
  RUN_TEST(test_fallback_only_slashes);
  RUN_TEST(test_fallback_spaces_and_slashes);
  // 5. Déterminisme et idempotence
  RUN_TEST(test_deterministic_same_raw_same_output);
  RUN_TEST(test_idempotent_on_wellformed_output);
  RUN_TEST(test_known_non_idempotent_edge_trailing_space);
  // 6. Bornes buffer
  RUN_TEST(test_truncation_long_input);
  RUN_TEST(test_truncation_fallback_also_bounded);
  RUN_TEST(test_outlen_one_yields_empty_string);
  RUN_TEST(test_noop_when_out_null);
  RUN_TEST(test_noop_when_outlen_zero);
  RUN_TEST(test_exact_fit_no_truncation);
  return UNITY_END();
}
