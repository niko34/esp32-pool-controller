// =============================================================================
// Tests unitaires natifs — Parsing borné de /check-update
// (ota_check_logic, bug-checkupdate-heap-tls / ADR-0032 ; bug-165)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/ota_check_logic.h
// (ADR-0017) :
//   - AllocBudget : plafond NET (admit/release), insensible au débordement,
//     crédit au release (scénario de doublement ArduinoJson couvert) ;
//   - utf8SafeCutLen : point de coupe qui ne scinde jamais un caractère UTF-8
//     (ASCII, 2 octets é, 3 octets ✓, 4 octets emoji, frontières exactes) ;
//   - classifyOtaCheckFailure (bug-165) : l'étape à laquelle un échec
//     `/check-update` a eu lieu — AVANT ce correctif, un code HTTPClient
//     négatif (`-1`) couvrait indifféremment un TCP refusé/DNS ET un
//     handshake TLS avorté, sans que la réponse HTTP ni le journal ne le
//     disent. Le point le plus important de cette suite : prouver que la
//     fonction ne confond JAMAIS « aucune release » (404, une réponse HTTP
//     reçue) avec un échec réseau (-1, aucune réponse) — les deux cas
//     doivent produire des valeurs d'enum DIFFÉRENTES, pour que l'appelant
//     ne puisse pas les agréger par erreur.
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "ota_check_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. AllocBudget — plafond cumulatif
// ---------------------------------------------------------------------------

void test_budget_admits_up_to_cap(void) {
  AllocBudget b(100);
  TEST_ASSERT_TRUE(b.admit(60));
  TEST_ASSERT_TRUE(b.admit(40));   // exactement à ras du plafond
  TEST_ASSERT_EQUAL(100, b.used);
  TEST_ASSERT_FALSE(b.admit(1));   // plafond atteint
}

void test_budget_refuses_single_oversized(void) {
  AllocBudget b(100);
  TEST_ASSERT_FALSE(b.admit(101));
  TEST_ASSERT_EQUAL(0, b.used);    // un refus ne consomme rien
  TEST_ASSERT_TRUE(b.admit(100));  // le budget reste intègre après refus
}

void test_budget_zero_size_alloc_is_free(void) {
  AllocBudget b(10);
  TEST_ASSERT_TRUE(b.admit(0));
  TEST_ASSERT_EQUAL(0, b.used);
}

void test_budget_overflow_resistant(void) {
  // n gigantesque : la comparaison n > cap - used ne doit pas déborder.
  AllocBudget b(100);
  TEST_ASSERT_TRUE(b.admit(50));
  TEST_ASSERT_FALSE(b.admit((size_t)-1));  // SIZE_MAX
  TEST_ASSERT_EQUAL(50, b.used);
}

void test_budget_release_refunds(void) {
  AllocBudget b(10);
  TEST_ASSERT_TRUE(b.admit(10));
  TEST_ASSERT_FALSE(b.admit(1));
  b.release(4);
  TEST_ASSERT_EQUAL(6, b.used);
  TEST_ASSERT_TRUE(b.admit(4));   // l'espace libéré est réutilisable
  TEST_ASSERT_FALSE(b.admit(1));
}

void test_budget_release_clamps_to_zero(void) {
  // Défensif : un crédit incohérent (> used) ne casse pas l'invariant.
  AllocBudget b(10);
  TEST_ASSERT_TRUE(b.admit(3));
  b.release(100);
  TEST_ASSERT_EQUAL(0, b.used);
  TEST_ASSERT_TRUE(b.admit(10));
}

void test_budget_doubling_growth_pattern_fits(void) {
  // Scénario réel ArduinoJson (revue 2.16.1) : StringBuilder double son buffer
  // (31→63→127→…→4095) puis rétrécit à la taille utile. Avec le comptage NET
  // (delta facturé à la croissance, crédit au rétrécissement), un corps de
  // ~2,5 Ko tient dans 8 Ko — alors qu'un comptage monotone cumulerait ~2×
  // la somme des doublements et refuserait.
  AllocBudget b(8192);
  size_t size = 31;
  TEST_ASSERT_TRUE(b.admit(size));
  while (size < 4095) {
    size_t next = size * 2 + 1;
    TEST_ASSERT_TRUE(b.admit(next - size));  // croissance : delta seul
    size = next;
  }
  TEST_ASSERT_EQUAL(4095, b.used);           // net = buffer courant, pas le cumul
  b.release(4095 - 2505);                    // rétrécissement final à 2505 o
  TEST_ASSERT_EQUAL(2505, b.used);
  TEST_ASSERT_TRUE(b.admit(1024));           // pool de variants : ça tient
}

// ---------------------------------------------------------------------------
// 2. utf8SafeCutLen — coupe sur frontière de caractère
// ---------------------------------------------------------------------------

void test_utf8_cut_short_string_untouched(void) {
  const char* s = "notes";
  TEST_ASSERT_EQUAL(5, utf8SafeCutLen(s, 5, 10));
  TEST_ASSERT_EQUAL(5, utf8SafeCutLen(s, 5, 5));  // exactement à la limite
}

void test_utf8_cut_ascii_at_limit(void) {
  const char* s = "abcdef";
  TEST_ASSERT_EQUAL(4, utf8SafeCutLen(s, 6, 4));  // ASCII : coupe franche
}

void test_utf8_cut_two_byte_char_not_split(void) {
  // "réglage" : é = 0xC3 0xA9 aux offsets 1-2. Couper à 2 tomberait au milieu.
  const char* s = "r\xC3\xA9glage";
  TEST_ASSERT_EQUAL(1, utf8SafeCutLen(s, strlen(s), 2));  // recule avant le é
  TEST_ASSERT_EQUAL(3, utf8SafeCutLen(s, strlen(s), 3));  // é entier conservé
}

void test_utf8_cut_three_byte_char_not_split(void) {
  // ✓ = 0xE2 0x9C 0x93 (3 octets) après "ok" → offsets 2-4.
  // Littéraux scindés : sans cela le compilateur lirait « \x93f » (le f de
  // « fin » est un chiffre hexadécimal) → séquence hors plage.
  const char* s = "ok\xE2\x9C\x93" "fin";
  TEST_ASSERT_EQUAL(2, utf8SafeCutLen(s, strlen(s), 3));  // milieu du ✓ → recule
  TEST_ASSERT_EQUAL(2, utf8SafeCutLen(s, strlen(s), 4));  // dernier octet du ✓ → recule
  TEST_ASSERT_EQUAL(5, utf8SafeCutLen(s, strlen(s), 5));  // ✓ entier conservé
}

void test_utf8_cut_four_byte_emoji_not_split(void) {
  // 🤖 = 0xF0 0x9F 0xA4 0x96 (4 octets) en tête de chaîne.
  const char* s = "\xF0\x9F\xA4\x96 ok";
  TEST_ASSERT_EQUAL(0, utf8SafeCutLen(s, strlen(s), 1));
  TEST_ASSERT_EQUAL(0, utf8SafeCutLen(s, strlen(s), 2));
  TEST_ASSERT_EQUAL(0, utf8SafeCutLen(s, strlen(s), 3));
  TEST_ASSERT_EQUAL(4, utf8SafeCutLen(s, strlen(s), 4));  // emoji entier
}

void test_utf8_cut_all_continuation_returns_zero(void) {
  // Entrée pathologique : que des octets de continuation → coupe à 0 (sûr).
  const char* s = "\x80\x80\x80\x80";
  TEST_ASSERT_EQUAL(0, utf8SafeCutLen(s, 4, 2));
}

// ---------------------------------------------------------------------------
// 3. classifyOtaCheckFailure — étape d'un échec /check-update (bug-165)
// ---------------------------------------------------------------------------

void test_classify_tls_error_code_is_handshake(void) {
  // Code mbedtls RÉELLEMENT observé en production (bug-165) :
  // -9984 = -0x2700 = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED (chaîne non
  // validée — exactement le symptôme de la racine cross-signée).
  // httpCode == -1 ici, comme en conditions réelles : WiFiClientSecure
  // renvoie 0 à connect() aussi bien pour un TCP refusé QUE pour un
  // handshake TLS avorté — c'est tlsErrorCode qui lève l'ambiguïté, PAS
  // httpCode. C'est le cas qui tue la régression « priorité inversée » :
  // si l'implémentation testait httpCode == -1 AVANT tlsErrorCode != 0,
  // ce cas précis (le vrai bug) retomberait à tort en kConnection.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kTlsHandshake),
                     static_cast<int>(classifyOtaCheckFailure(-1, -9984)));
}

void test_classify_tls_error_wins_even_with_other_httpcode(void) {
  // tlsErrorCode != 0 doit l'emporter quelle que soit la valeur de httpCode
  // (pas seulement -1) : le signal TLS est plus fort que le code HTTPClient.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kTlsHandshake),
                     static_cast<int>(classifyOtaCheckFailure(-11, -9984)));
}

void test_classify_minus1_without_tls_error_is_connection(void) {
  // httpCode == -1 SANS erreur TLS (tlsErrorCode == 0) : résolution de nom
  // ou TCP refusé — jamais un handshake TLS.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kConnection),
                     static_cast<int>(classifyOtaCheckFailure(-1, 0)));
}

void test_classify_other_negative_code_is_http_other(void) {
  // -11 = HTTPC_ERROR_READ_TIMEOUT par exemple : négatif, mais PAS -1, et
  // aucune erreur TLS associée → ni TLS ni « connexion », la case par défaut.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kHttpOther),
                     static_cast<int>(classifyOtaCheckFailure(-11, 0)));
}

void test_classify_zero_is_http_other(void) {
  // Valeur limite : 0 n'est ni -1 ni un tlsErrorCode non nul.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kHttpOther),
                     static_cast<int>(classifyOtaCheckFailure(0, 0)));
}

void test_classify_404_is_never_connection_failure(void) {
  // ⚠️ LE TEST QUI COMPTE LE PLUS (spec bug-165) : 404 (une RÉPONSE HTTP
  // reçue — « aucune release publiée ») ne doit JAMAIS produire la même
  // classification qu'un -1 (AUCUNE réponse — échec réseau). Dans la route
  // réelle (web_routes_ota.cpp), le cas 404 est intercepté AVANT même
  // d'appeler classifyOtaCheckFailure (réponse 200 dédiée, "no_release":
  // true) ; ce test verrouille que si ce court-circuit disparaissait un
  // jour, la fonction PURE elle-même ne confondrait pas les deux cas sous
  // la même étiquette d'étape.
  OtaCheckFailureStep step404 = classifyOtaCheckFailure(404, 0);
  OtaCheckFailureStep stepNetworkFailure = classifyOtaCheckFailure(-1, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kHttpOther),
                     static_cast<int>(step404));
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(stepNetworkFailure),
                         static_cast<int>(step404));
}

void test_classify_positive_code_is_http_other(void) {
  // Autre valeur limite : un code positif (ex. 500) retombe dans la case
  // par défaut, jamais pris pour un échec de connexion.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaCheckFailureStep::kHttpOther),
                     static_cast<int>(classifyOtaCheckFailure(500, 0)));
}

// ---------------------------------------------------------------------------
// 3. Comparaison de versions (bug-178) — « proposer seulement si STRICTEMENT
//    plus récent ». Comparaison NUMÉRIQUE : "2.10.0" > "2.9.0".
// ---------------------------------------------------------------------------

static OtaUpdateState st(const char* cur, const char* lat) {
  return otaUpdateStateFor(cur, lat);
}
#define ASSERT_STATE(exp, cur, lat) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(exp), static_cast<int>(st(cur, lat)))

void test_version_numeric_not_lexicographic_both_ways(void) {
  ASSERT_STATE(OtaUpdateState::kUpdateAvailable, "2.9.0", "2.10.0");
  ASSERT_STATE(OtaUpdateState::kDeviceAhead, "2.10.0", "2.9.0");
}

void test_version_field_case_device_ahead(void) {
  // Situation réelle du 2026-11-15 : installé 2.88.0, release 2.87.1.
  ASSERT_STATE(OtaUpdateState::kDeviceAhead, "2.88.0", "2.87.1");
}

void test_version_patch_level_decides(void) {
  ASSERT_STATE(OtaUpdateState::kUpdateAvailable, "2.87.0", "2.87.1");
  ASSERT_STATE(OtaUpdateState::kDeviceAhead, "2.87.1", "2.87.0");
}

void test_version_major_dominates(void) {
  ASSERT_STATE(OtaUpdateState::kUpdateAvailable, "2.99.99", "3.0.0");
  ASSERT_STATE(OtaUpdateState::kDeviceAhead, "3.0.0", "2.99.99");
}

void test_version_equal(void) {
  ASSERT_STATE(OtaUpdateState::kUpToDate, "2.88.0", "2.88.0");
}

void test_version_missing_components_are_zero(void) {
  ASSERT_STATE(OtaUpdateState::kUpToDate, "2.88", "2.88.0");
  ASSERT_STATE(OtaUpdateState::kUpToDate, "2.88.0", "2.88");
  ASSERT_STATE(OtaUpdateState::kUpToDate, "2", "2.0.0");
  ASSERT_STATE(OtaUpdateState::kUpdateAvailable, "2.88", "2.88.1");
}

void test_version_v_prefix_stripped(void) {
  ASSERT_STATE(OtaUpdateState::kUpToDate, "v2.88.0", "2.88.0");
  ASSERT_STATE(OtaUpdateState::kUpToDate, "V2.88.0", "2.88.0");
  ASSERT_STATE(OtaUpdateState::kUpdateAvailable, "2.88.0", "v2.88.1");
  ASSERT_STATE(OtaUpdateState::kDeviceAhead, "V2.88.1", "v2.88.0");
}

void test_version_double_v_prefix_unreadable(void) {
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.0.0", "vv2.0.0");
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "vv2.0.0", "2.0.0");
}

void test_version_suffixes_unreadable(void) {
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.87.0", "2.88.0-rc1");
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.87.0", "2.88.0+b3");
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.87.0", "2.88.0b");
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.88.0-rc1", "2.87.0");
}

void test_version_unreadable_strings(void) {
  const char* bad[] = {"", ".", "2..0", "2.88.", "abc", "2.88.0.1", "v", ".2.0", " 2.0.0"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.88.0", bad[i]);
    ASSERT_STATE(OtaUpdateState::kUnknownVersion, bad[i], "2.88.0");
  }
}

void test_version_nullptr_unknown(void) {
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, nullptr, "2.88.0");
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.88.0", nullptr);
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, nullptr, nullptr);
  uint32_t v[kOtaVersionMaxParts];
  TEST_ASSERT_FALSE(parseOtaVersion(nullptr, v));
}

void test_version_component_digit_limit(void) {
  uint32_t v[kOtaVersionMaxParts];
  // 9 chiffres : valide, valeur exacte (pas de débordement)
  TEST_ASSERT_TRUE(parseOtaVersion("999999999.0.0", v));
  TEST_ASSERT_EQUAL_UINT32(999999999u, v[0]);
  TEST_ASSERT_TRUE(parseOtaVersion("1.999999999.999999999", v));
  TEST_ASSERT_EQUAL_UINT32(999999999u, v[2]);
  // 10 chiffres : refusé
  TEST_ASSERT_FALSE(parseOtaVersion("1000000000.0.0", v));
  TEST_ASSERT_FALSE(parseOtaVersion("1.0000000000.0", v));
  // 10 chiffres dont la valeur déborderait 2^32 : refusé, jamais « petit »
  TEST_ASSERT_FALSE(parseOtaVersion("4294967297.0.0", v));
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "2.0.0", "4294967297.0.0");
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "4294967297.0.0", "2.0.0");
  // zéros de tête : comptent comme chiffres
  TEST_ASSERT_FALSE(parseOtaVersion("0000000001.0.0", v));
  ASSERT_STATE(OtaUpdateState::kUpdateAvailable, "999999998.0.0", "999999999.0.0");
}

void test_version_identical_strings_up_to_date_even_unreadable(void) {
  ASSERT_STATE(OtaUpdateState::kUpToDate, "abc", "abc");
  ASSERT_STATE(OtaUpdateState::kUpToDate, "2.88.0-rc1", "2.88.0-rc1");
  ASSERT_STATE(OtaUpdateState::kUpToDate, "", "");
  ASSERT_STATE(OtaUpdateState::kUpToDate, "vabc", "abc");  // après retrait du v
  // mais pas pour des chaînes différentes illisibles
  ASSERT_STATE(OtaUpdateState::kUnknownVersion, "abc", "abd");
}

// ⛔ Invariant central : kUpdateAvailable <=> les deux lisibles ET latest > current.
// Balayage d'un corpus mêlant lisibles et illisibles, avec un oracle INDÉPENDANT
// (entiers 64 bits, composants fournis à part).
void test_invariant_update_available_only_if_both_readable_and_strictly_newer(void) {
  struct Case { const char* s; bool readable; unsigned long long key; };
  const unsigned long long B = 1000000000ULL;
  #define K(a,b,c) ((unsigned long long)(a) * B * B + (unsigned long long)(b) * B + (c))
  const Case corpus[] = {
    {"2.9.0", true, K(2,9,0)},   {"2.10.0", true, K(2,10,0)},
    {"2.88.0", true, K(2,88,0)}, {"2.87.1", true, K(2,87,1)},
    {"v2.88.1", true, K(2,88,1)},{"2.88", true, K(2,88,0)},
    {"3", true, K(3,0,0)},       {"0.0.0", true, K(0,0,0)},
    {"999999999.0.0", true, K(999999999,0,0)},
    {"", false, 0},              {"abc", false, 0},
    {"2.88.0-rc1", false, 0},    {"2.88.0.1", false, 0},
    {"vv2.0.0", false, 0},       {"1000000000.0.0", false, 0},
    {"2..0", false, 0},          {nullptr, false, 0},
  };
  #undef K
  const size_t n = sizeof(corpus) / sizeof(corpus[0]);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      const Case& c = corpus[i];
      const Case& l = corpus[j];
      OtaUpdateState got = otaUpdateStateFor(c.s, l.s);
      bool expectAvail = c.readable && l.readable && l.key > c.key;
      char msg[96];
      snprintf(msg, sizeof(msg), "cur=%s latest=%s", c.s ? c.s : "(null)", l.s ? l.s : "(null)");
      TEST_ASSERT_EQUAL_MESSAGE(expectAvail, got == OtaUpdateState::kUpdateAvailable, msg);
      if (c.readable && l.readable) {
        OtaUpdateState exp = l.key > c.key ? OtaUpdateState::kUpdateAvailable
                           : l.key < c.key ? OtaUpdateState::kDeviceAhead
                                           : OtaUpdateState::kUpToDate;
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(exp), static_cast<int>(got), msg);
      }
    }
  }
}

void test_state_codes_contract(void) {
  TEST_ASSERT_EQUAL_STRING("unknown_version", otaUpdateStateCode(OtaUpdateState::kUnknownVersion));
  TEST_ASSERT_EQUAL_STRING("up_to_date", otaUpdateStateCode(OtaUpdateState::kUpToDate));
  TEST_ASSERT_EQUAL_STRING("update_available", otaUpdateStateCode(OtaUpdateState::kUpdateAvailable));
  TEST_ASSERT_EQUAL_STRING("device_ahead", otaUpdateStateCode(OtaUpdateState::kDeviceAhead));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_budget_admits_up_to_cap);
  RUN_TEST(test_budget_refuses_single_oversized);
  RUN_TEST(test_budget_zero_size_alloc_is_free);
  RUN_TEST(test_budget_overflow_resistant);
  RUN_TEST(test_budget_release_refunds);
  RUN_TEST(test_budget_release_clamps_to_zero);
  RUN_TEST(test_budget_doubling_growth_pattern_fits);
  RUN_TEST(test_utf8_cut_short_string_untouched);
  RUN_TEST(test_utf8_cut_ascii_at_limit);
  RUN_TEST(test_utf8_cut_two_byte_char_not_split);
  RUN_TEST(test_utf8_cut_three_byte_char_not_split);
  RUN_TEST(test_utf8_cut_four_byte_emoji_not_split);
  RUN_TEST(test_utf8_cut_all_continuation_returns_zero);
  RUN_TEST(test_classify_tls_error_code_is_handshake);
  RUN_TEST(test_classify_tls_error_wins_even_with_other_httpcode);
  RUN_TEST(test_classify_minus1_without_tls_error_is_connection);
  RUN_TEST(test_classify_other_negative_code_is_http_other);
  RUN_TEST(test_classify_zero_is_http_other);
  RUN_TEST(test_classify_404_is_never_connection_failure);
  RUN_TEST(test_classify_positive_code_is_http_other);
  RUN_TEST(test_version_numeric_not_lexicographic_both_ways);
  RUN_TEST(test_version_field_case_device_ahead);
  RUN_TEST(test_version_patch_level_decides);
  RUN_TEST(test_version_major_dominates);
  RUN_TEST(test_version_equal);
  RUN_TEST(test_version_missing_components_are_zero);
  RUN_TEST(test_version_v_prefix_stripped);
  RUN_TEST(test_version_double_v_prefix_unreadable);
  RUN_TEST(test_version_suffixes_unreadable);
  RUN_TEST(test_version_unreadable_strings);
  RUN_TEST(test_version_nullptr_unknown);
  RUN_TEST(test_version_component_digit_limit);
  RUN_TEST(test_version_identical_strings_up_to_date_even_unreadable);
  RUN_TEST(test_invariant_update_available_only_if_both_readable_and_strictly_newer);
  RUN_TEST(test_state_codes_contract);
  return UNITY_END();
}
