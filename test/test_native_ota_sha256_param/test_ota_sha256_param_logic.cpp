// =============================================================================
// Tests unitaires natifs — feature-118 lot A
// =============================================================================
// Couvre les DEUX fonctions pures ajoutées par le lot A, non testées par
// test_native_ota_integrity (qui couvre feature-026 : parseSha256Digest,
// sha256Equal, sha256ToHex) :
//   - parseOtaSha256Param(present, digestStr, out) -> OtaSha256ParamVerdict
//   - otaUploadIntegrityVerdict(paramVerdict, expected, computed)
//       -> OtaUploadIntegrityVerdict
//   - otaUploadIntegrityVerdictLabel(verdict)
//   - otaUploadIntegrityIsRejected(verdict)
//
// Cas exigés nommément par l'audit sécurité (condition 2) pour le paramètre
// `sha256` de POST /update : chaîne vide, préfixe seul, 63 hex, 65 hex,
// majuscules, espace final, \n final, forme valide AVEC et SANS préfixe,
// absent. Voir specs/features/doing/feature-118-authenticite-de-l-image-ota.md.
// =============================================================================

#include <unity.h>
#include <string.h>
#include "ota_integrity_logic.h"

static const char kHexRef[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const uint8_t kBinRef[32] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

static void fillSentinel(uint8_t out[32]) { memset(out, 0x5A, 32); }

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// parseOtaSha256Param — ABSENT
// -----------------------------------------------------------------------------
void test_param_absent_when_not_present(void) {
  uint8_t out[32];
  fillSentinel(out);
  // present == false : digestStr ignoré, même s'il contient une empreinte
  // valide — c'est bien l'absence du PARAMÈTRE qui doit primer.
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kAbsent,
                     parseOtaSha256Param(false, kHexRef, out));
}

void test_param_absent_when_present_but_null_digest(void) {
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kAbsent,
                     parseOtaSha256Param(true, NULL, out));
}

// -----------------------------------------------------------------------------
// parseOtaSha256Param — MALFORMED
// -----------------------------------------------------------------------------
void test_param_malformed_empty_string(void) {
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, "", out));
}

void test_param_malformed_prefix_only(void) {
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, "sha256:", out));
}

void test_param_malformed_63_hex_chars_bare(void) {
  char hex63[64];
  memcpy(hex63, kHexRef, 63);
  hex63[63] = '\0';
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, hex63, out));
}

void test_param_malformed_63_hex_chars_prefixed(void) {
  char hex63[64];
  memcpy(hex63, kHexRef, 63);
  hex63[63] = '\0';
  char digest[80];
  strcpy(digest, "sha256:");
  strcat(digest, hex63);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, digest, out));
}

void test_param_malformed_65_hex_chars_bare(void) {
  char hex65[66];
  memcpy(hex65, kHexRef, 64);
  hex65[64] = 'a';
  hex65[65] = '\0';
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, hex65, out));
}

void test_param_malformed_trailing_space(void) {
  char digest[80];
  strcpy(digest, kHexRef);
  strcat(digest, " ");
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, digest, out));
}

void test_param_malformed_trailing_newline(void) {
  char digest[80];
  strcpy(digest, kHexRef);
  strcat(digest, "\n");
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kMalformed,
                     parseOtaSha256Param(true, digest, out));
}

// -----------------------------------------------------------------------------
// parseOtaSha256Param — PARSED (forme valide AVEC et SANS préfixe, majuscules)
// -----------------------------------------------------------------------------
void test_param_parsed_bare_no_prefix(void) {
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kParsed,
                     parseOtaSha256Param(true, kHexRef, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kBinRef, out, 32);
}

void test_param_parsed_with_prefix(void) {
  char digest[80];
  strcpy(digest, "sha256:");
  strcat(digest, kHexRef);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(OtaSha256ParamVerdict::kParsed,
                     parseOtaSha256Param(true, digest, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kBinRef, out, 32);
}

void test_param_parsed_uppercase_bare(void) {
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_EQUAL(
      OtaSha256ParamVerdict::kParsed,
      parseOtaSha256Param(
          true, "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
          out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kBinRef, out, 32);
}

// -----------------------------------------------------------------------------
// otaUploadIntegrityVerdict — les quatre verdicts
// -----------------------------------------------------------------------------
void test_upload_verdict_absent(void) {
  TEST_ASSERT_EQUAL(
      OtaUploadIntegrityVerdict::kAbsent,
      otaUploadIntegrityVerdict(OtaSha256ParamVerdict::kAbsent, NULL, NULL));
}

void test_upload_verdict_malformed(void) {
  TEST_ASSERT_EQUAL(
      OtaUploadIntegrityVerdict::kMalformed,
      otaUploadIntegrityVerdict(OtaSha256ParamVerdict::kMalformed, NULL, NULL));
}

void test_upload_verdict_conform(void) {
  uint8_t a[32], b[32];
  memcpy(a, kBinRef, 32);
  memcpy(b, kBinRef, 32);
  TEST_ASSERT_EQUAL(
      OtaUploadIntegrityVerdict::kConform,
      otaUploadIntegrityVerdict(OtaSha256ParamVerdict::kParsed, a, b));
}

void test_upload_verdict_non_conform(void) {
  uint8_t expected[32], computed[32];
  memcpy(expected, kBinRef, 32);
  memcpy(computed, kBinRef, 32);
  computed[0] ^= 0x01;  // L'empreinte calculée diffère de celle attendue
  TEST_ASSERT_EQUAL(
      OtaUploadIntegrityVerdict::kNonConform,
      otaUploadIntegrityVerdict(OtaSha256ParamVerdict::kParsed, expected, computed));
}

// -----------------------------------------------------------------------------
// otaUploadIntegrityVerdictLabel — libellé FR stable
// -----------------------------------------------------------------------------
void test_label_absent(void) {
  TEST_ASSERT_EQUAL_STRING(
      "absente", otaUploadIntegrityVerdictLabel(OtaUploadIntegrityVerdict::kAbsent));
}

void test_label_malformed(void) {
  TEST_ASSERT_EQUAL_STRING(
      "malformée",
      otaUploadIntegrityVerdictLabel(OtaUploadIntegrityVerdict::kMalformed));
}

void test_label_conform(void) {
  TEST_ASSERT_EQUAL_STRING(
      "conforme", otaUploadIntegrityVerdictLabel(OtaUploadIntegrityVerdict::kConform));
}

void test_label_non_conform(void) {
  TEST_ASSERT_EQUAL_STRING(
      "non conforme",
      otaUploadIntegrityVerdictLabel(OtaUploadIntegrityVerdict::kNonConform));
}

// -----------------------------------------------------------------------------
// otaUploadIntegrityIsRejected — DÉCISION RÉELLE du lot B (tout sauf kConform
// est refusé) ; sous feature-118 lot A, cette même fonction (alors nommée
// otaUploadWouldLotBReject) ne faisait que journaliser une PROJECTION.
// test_is_rejected_absent_true est la couverture explicitement demandée par
// security-reviewer/lot B : le verdict kAbsent (empreinte manquante) doit
// désormais être REFUSÉ, pas seulement observé.
// -----------------------------------------------------------------------------
void test_is_rejected_absent_true(void) {
  TEST_ASSERT_TRUE(otaUploadIntegrityIsRejected(OtaUploadIntegrityVerdict::kAbsent));
}

void test_is_rejected_malformed_true(void) {
  TEST_ASSERT_TRUE(otaUploadIntegrityIsRejected(OtaUploadIntegrityVerdict::kMalformed));
}

void test_is_rejected_non_conform_true(void) {
  TEST_ASSERT_TRUE(otaUploadIntegrityIsRejected(OtaUploadIntegrityVerdict::kNonConform));
}

void test_is_rejected_conform_false(void) {
  TEST_ASSERT_FALSE(otaUploadIntegrityIsRejected(OtaUploadIntegrityVerdict::kConform));
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_param_absent_when_not_present);
  RUN_TEST(test_param_absent_when_present_but_null_digest);

  RUN_TEST(test_param_malformed_empty_string);
  RUN_TEST(test_param_malformed_prefix_only);
  RUN_TEST(test_param_malformed_63_hex_chars_bare);
  RUN_TEST(test_param_malformed_63_hex_chars_prefixed);
  RUN_TEST(test_param_malformed_65_hex_chars_bare);
  RUN_TEST(test_param_malformed_trailing_space);
  RUN_TEST(test_param_malformed_trailing_newline);

  RUN_TEST(test_param_parsed_bare_no_prefix);
  RUN_TEST(test_param_parsed_with_prefix);
  RUN_TEST(test_param_parsed_uppercase_bare);

  RUN_TEST(test_upload_verdict_absent);
  RUN_TEST(test_upload_verdict_malformed);
  RUN_TEST(test_upload_verdict_conform);
  RUN_TEST(test_upload_verdict_non_conform);

  RUN_TEST(test_label_absent);
  RUN_TEST(test_label_malformed);
  RUN_TEST(test_label_conform);
  RUN_TEST(test_label_non_conform);

  RUN_TEST(test_is_rejected_absent_true);
  RUN_TEST(test_is_rejected_malformed_true);
  RUN_TEST(test_is_rejected_non_conform_true);
  RUN_TEST(test_is_rejected_conform_false);

  return UNITY_END();
}
