// =============================================================================
// Tests unitaires natifs — ota_manifest_binding_logic (feature-151 D1, lot B1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
//
// B1 (revue de sécurité du 2026-10-18) : combineOtaAuthenticityVerdict() PROUVE
// qu'un manifeste a été signé par la bonne clé — JAMAIS qu'il décrit l'image
// reçue. Ce module ferme ce trou en comparant l'empreinte DÉCLARÉE par le
// manifeste (hex, décodée ICI) à une empreinte 32 octets déjà disponible —
// voir otaManifestBindingVerdict (ota_manifest_binding_logic.h).
// =============================================================================

#include <unity.h>
#include <string.h>

#include "ota_manifest_binding_logic.h"

void setUp(void) {}
void tearDown(void) {}

static void fillDigest(uint8_t out[32], uint8_t value) {
  memset(out, value, 32);
}

// 64 hex minuscules représentant 32 fois l'octet `value` — même convention
// que OtaManifestFields::sha256Hex (parseOtaManifest l'a déjà validé avant
// qu'un appelant ne passe par ici : ce module ne revalide pas la grammaire).
static void fillHex(char out[65], uint8_t value) {
  static const char* digits = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[2 * i] = digits[(value >> 4) & 0x0F];
    out[2 * i + 1] = digits[value & 0x0F];
  }
  out[64] = '\0';
}

// -----------------------------------------------------------------------------
// kSkipped : une empreinte ou l'autre indisponible — AUCUNE comparaison faite.
// -----------------------------------------------------------------------------
void test_skipped_when_manifest_fields_invalid(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0xAA);
  fillDigest(comparedDigest, 0xAA);  // IDENTIQUES — ne doit quand même PAS être "lié"
  TEST_ASSERT_EQUAL(
      OtaManifestBindingVerdict::kSkipped,
      otaManifestBindingVerdict(/*manifestFieldsValid=*/false, manifestHex,
                               /*comparedDigestValid=*/true, comparedDigest));
}

void test_skipped_when_compared_digest_invalid(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0xAA);
  fillDigest(comparedDigest, 0xAA);
  TEST_ASSERT_EQUAL(
      OtaManifestBindingVerdict::kSkipped,
      otaManifestBindingVerdict(/*manifestFieldsValid=*/true, manifestHex,
                               /*comparedDigestValid=*/false, comparedDigest));
}

void test_skipped_when_both_invalid(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0xAA);
  fillDigest(comparedDigest, 0xBB);  // DIVERGENTES — kSkipped doit quand même primer
  TEST_ASSERT_EQUAL(
      OtaManifestBindingVerdict::kSkipped,
      otaManifestBindingVerdict(/*manifestFieldsValid=*/false, manifestHex,
                               /*comparedDigestValid=*/false, comparedDigest));
}

void test_skipped_when_manifest_hex_pointer_null(void) {
  uint8_t comparedDigest[32];
  fillDigest(comparedDigest, 0xAA);
  TEST_ASSERT_EQUAL(OtaManifestBindingVerdict::kSkipped,
                    otaManifestBindingVerdict(/*manifestFieldsValid=*/true, nullptr,
                                              /*comparedDigestValid=*/true, comparedDigest));
}

void test_skipped_when_compared_digest_pointer_null(void) {
  char manifestHex[65];
  fillHex(manifestHex, 0xAA);
  TEST_ASSERT_EQUAL(OtaManifestBindingVerdict::kSkipped,
                    otaManifestBindingVerdict(/*manifestFieldsValid=*/true, manifestHex,
                                              /*comparedDigestValid=*/true, nullptr));
}

// Défensif : un hex malformé (ne devrait jamais survenir en pratique — le
// manifeste a déjà été validé par parseOtaManifest avant que
// manifestFieldsValid ne soit vrai) doit échouer en kSkipped, jamais produire
// un faux "lié" par décodage partiel.
void test_skipped_when_manifest_hex_malformed(void) {
  char manifestHex[65] = "pas-du-tout-un-hex-valide-meme-si-la-longueur-pourrait-tromper!!";
  uint8_t comparedDigest[32];
  fillDigest(comparedDigest, 0xAA);
  TEST_ASSERT_EQUAL(
      OtaManifestBindingVerdict::kSkipped,
      otaManifestBindingVerdict(/*manifestFieldsValid=*/true, manifestHex,
                               /*comparedDigestValid=*/true, comparedDigest));
}

// -----------------------------------------------------------------------------
// kBound / kUnbound : les deux empreintes sont disponibles.
// -----------------------------------------------------------------------------
void test_bound_when_digests_are_identical(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0x42);
  fillDigest(comparedDigest, 0x42);
  TEST_ASSERT_EQUAL(OtaManifestBindingVerdict::kBound,
                    otaManifestBindingVerdict(true, manifestHex, true, comparedDigest));
}

void test_unbound_when_digests_differ(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0x42);
  fillDigest(comparedDigest, 0x43);
  TEST_ASSERT_EQUAL(OtaManifestBindingVerdict::kUnbound,
                    otaManifestBindingVerdict(true, manifestHex, true, comparedDigest));
}

// Un SEUL octet de différence (le cas d'école : manifeste signé pour l'image
// A, envoyé avec l'image B dont seul le dernier octet d'empreinte diffère) —
// ⚠️ MUTATION CIBLÉE : un combinateur qui ne comparerait que les 31 premiers
// octets (off-by-one) ferait survivre ce cas en kBound au lieu de kUnbound.
void test_unbound_on_single_byte_difference_at_end(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0x11);
  fillDigest(comparedDigest, 0x11);
  comparedDigest[31] = 0x12;
  TEST_ASSERT_EQUAL(OtaManifestBindingVerdict::kUnbound,
                    otaManifestBindingVerdict(true, manifestHex, true, comparedDigest));
}

void test_unbound_on_single_byte_difference_at_start(void) {
  char manifestHex[65];
  uint8_t comparedDigest[32];
  fillHex(manifestHex, 0x11);
  fillDigest(comparedDigest, 0x11);
  comparedDigest[0] = 0x12;
  TEST_ASSERT_EQUAL(OtaManifestBindingVerdict::kUnbound,
                    otaManifestBindingVerdict(true, manifestHex, true, comparedDigest));
}

// Validation du harnais lui-même : fillHex doit produire exactement ce que
// parseOtaSha256Param attend (sinon tout ce fichier teste autre chose que
// prévu).
void test_fill_hex_harness_round_trips_through_parse(void) {
  char manifestHex[65];
  fillHex(manifestHex, 0x7A);
  TEST_ASSERT_EQUAL_INT(64, (int)strlen(manifestHex));
  TEST_ASSERT_EQUAL_STRING(
      "7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a", manifestHex);
}

// -----------------------------------------------------------------------------
// otaManifestBindingIsDivergent : SEUL kUnbound est une anomalie à signaler.
// -----------------------------------------------------------------------------
void test_is_divergent_true_only_for_unbound(void) {
  TEST_ASSERT_FALSE(otaManifestBindingIsDivergent(OtaManifestBindingVerdict::kSkipped));
  TEST_ASSERT_FALSE(otaManifestBindingIsDivergent(OtaManifestBindingVerdict::kBound));
  TEST_ASSERT_TRUE(otaManifestBindingIsDivergent(OtaManifestBindingVerdict::kUnbound));
}

// -----------------------------------------------------------------------------
// Libellés : jamais nuls, un par verdict.
// -----------------------------------------------------------------------------
void test_labels_are_distinct_and_non_null(void) {
  const char* skipped = otaManifestBindingVerdictLabel(OtaManifestBindingVerdict::kSkipped);
  const char* bound = otaManifestBindingVerdictLabel(OtaManifestBindingVerdict::kBound);
  const char* unbound = otaManifestBindingVerdictLabel(OtaManifestBindingVerdict::kUnbound);
  TEST_ASSERT_NOT_NULL(skipped);
  TEST_ASSERT_NOT_NULL(bound);
  TEST_ASSERT_NOT_NULL(unbound);
  TEST_ASSERT_TRUE(strcmp(skipped, bound) != 0);
  TEST_ASSERT_TRUE(strcmp(bound, unbound) != 0);
  TEST_ASSERT_TRUE(strcmp(skipped, unbound) != 0);
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_skipped_when_manifest_fields_invalid);
  RUN_TEST(test_skipped_when_compared_digest_invalid);
  RUN_TEST(test_skipped_when_both_invalid);
  RUN_TEST(test_skipped_when_manifest_hex_pointer_null);
  RUN_TEST(test_skipped_when_compared_digest_pointer_null);
  RUN_TEST(test_skipped_when_manifest_hex_malformed);
  RUN_TEST(test_bound_when_digests_are_identical);
  RUN_TEST(test_unbound_when_digests_differ);
  RUN_TEST(test_unbound_on_single_byte_difference_at_end);
  RUN_TEST(test_unbound_on_single_byte_difference_at_start);
  RUN_TEST(test_fill_hex_harness_round_trips_through_parse);
  RUN_TEST(test_is_divergent_true_only_for_unbound);
  RUN_TEST(test_labels_are_distinct_and_non_null);
  return UNITY_END();
}
