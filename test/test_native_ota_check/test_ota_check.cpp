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
  return UNITY_END();
}
