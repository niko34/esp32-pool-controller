// =============================================================================
// Tests unitaires natifs — ota_authenticity_logic (feature-151 D1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// Couvre le combinateur PUR combineOtaAuthenticityVerdict() : il ne fait
// QUE composer des verdicts déjà calculés (parse, décodage base64, crypto
// injectée en paramètre) — aucune dépendance mbedtls ici.
// =============================================================================

#include <unity.h>
#include <string.h>

#include "ota_authenticity_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Combinaison par défaut : tout est au vert sauf ce qui est explicitement
// changé par le test — évite de répéter 13 paramètres à chaque appel.
static OtaAuthenticityVerdict combineOk(bool manifestPresent = true, bool signaturePresent = true,
                                        OtaManifestParseVerdict parseVerdict =
                                            OtaManifestParseVerdict::kOk,
                                        bool signatureDecodeOk = true,
                                        OtaCryptoVerdict cryptoVerdict = OtaCryptoVerdict::kValid,
                                        OtaManifestKind parsedKind = OtaManifestKind::kFirmware,
                                        OtaManifestKind expectedKind = OtaManifestKind::kFirmware,
                                        uint32_t major = 2, uint32_t minor = 85,
                                        uint32_t patch = 0, uint32_t floorMajor = 0,
                                        uint32_t floorMinor = 0, uint32_t floorPatch = 0) {
  return combineOtaAuthenticityVerdict(manifestPresent, signaturePresent, parseVerdict,
                                       signatureDecodeOk, cryptoVerdict, parsedKind, expectedKind,
                                       major, minor, patch, floorMajor, floorMinor, floorPatch);
}

// -----------------------------------------------------------------------------
// Chemin nominal
// -----------------------------------------------------------------------------
void test_valid_when_everything_matches(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kValid, combineOk());
}

// -----------------------------------------------------------------------------
// Priorité 1 : absente
// -----------------------------------------------------------------------------
void test_absent_when_manifest_missing(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kAbsent, combineOk(/*manifestPresent=*/false));
}

void test_absent_when_signature_missing(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kAbsent, combineOk(true, /*signaturePresent=*/false));
}

void test_absent_when_both_missing(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kAbsent, combineOk(false, false));
}

// absente prime sur tout le reste, même si la crypto serait invalide.
void test_absent_takes_priority_over_crypto(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kAbsent,
                    combineOk(false, true, OtaManifestParseVerdict::kMalformed, false,
                              OtaCryptoVerdict::kInvalid));
}

// -----------------------------------------------------------------------------
// Priorité 2 : malformée (grammaire OU base64)
// -----------------------------------------------------------------------------
void test_malformed_when_manifest_parse_fails(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kMalformed,
                    combineOk(true, true, OtaManifestParseVerdict::kMalformed));
}

void test_malformed_when_signature_decode_fails(void) {
  TEST_ASSERT_EQUAL(
      OtaAuthenticityVerdict::kMalformed,
      combineOk(true, true, OtaManifestParseVerdict::kOk, /*signatureDecodeOk=*/false));
}

void test_malformed_takes_priority_over_crypto(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kMalformed,
                    combineOk(true, true, OtaManifestParseVerdict::kMalformed, false,
                              OtaCryptoVerdict::kValid));
}

// -----------------------------------------------------------------------------
// Priorité 3 : résultat crypto (impossible avant invalide)
// -----------------------------------------------------------------------------
void test_verification_impossible(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kVerificationImpossible,
                    combineOk(true, true, OtaManifestParseVerdict::kOk, true,
                              OtaCryptoVerdict::kImpossible));
}

void test_invalid_signature(void) {
  TEST_ASSERT_EQUAL(
      OtaAuthenticityVerdict::kInvalid,
      combineOk(true, true, OtaManifestParseVerdict::kOk, true, OtaCryptoVerdict::kInvalid));
}

// -----------------------------------------------------------------------------
// Priorité 4 : genre discordant (seulement si crypto valide)
// -----------------------------------------------------------------------------
void test_kind_mismatch_firmware_vs_filesystem(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kKindMismatch,
                    combineOk(true, true, OtaManifestParseVerdict::kOk, true,
                              OtaCryptoVerdict::kValid, OtaManifestKind::kFirmware,
                              OtaManifestKind::kFilesystem));
}

void test_kind_match_firmware(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kValid,
                    combineOk(true, true, OtaManifestParseVerdict::kOk, true,
                              OtaCryptoVerdict::kValid, OtaManifestKind::kFirmware,
                              OtaManifestKind::kFirmware));
}

void test_kind_match_filesystem(void) {
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kValid,
                    combineOk(true, true, OtaManifestParseVerdict::kOk, true,
                              OtaCryptoVerdict::kValid, OtaManifestKind::kFilesystem,
                              OtaManifestKind::kFilesystem));
}

// -----------------------------------------------------------------------------
// Priorité 5 : plancher de version (seulement si crypto valide et genre OK)
// -----------------------------------------------------------------------------
void test_below_floor_major(void) {
  TEST_ASSERT_EQUAL(
      OtaAuthenticityVerdict::kBelowFloor,
      combineOk(true, true, OtaManifestParseVerdict::kOk, true, OtaCryptoVerdict::kValid,
               OtaManifestKind::kFirmware, OtaManifestKind::kFirmware, /*major=*/1, 0, 0,
               /*floorMajor=*/2, 0, 0));
}

void test_at_floor_is_not_below(void) {
  TEST_ASSERT_EQUAL(
      OtaAuthenticityVerdict::kValid,
      combineOk(true, true, OtaManifestParseVerdict::kOk, true, OtaCryptoVerdict::kValid,
               OtaManifestKind::kFirmware, OtaManifestKind::kFirmware, 2, 85, 0, 2, 85, 0));
}

void test_floor_never_triggers_with_default_zero_floor(void) {
  // D1 : plancher figé à 0.0.0 — jamais franchi par une version réelle.
  TEST_ASSERT_EQUAL(OtaAuthenticityVerdict::kValid, combineOk());
}

// -----------------------------------------------------------------------------
// otaAuthenticityIsRejected / libellés
// -----------------------------------------------------------------------------
void test_is_rejected_true_for_all_but_valid(void) {
  TEST_ASSERT_TRUE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kAbsent));
  TEST_ASSERT_TRUE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kMalformed));
  TEST_ASSERT_TRUE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kVerificationImpossible));
  TEST_ASSERT_TRUE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kInvalid));
  TEST_ASSERT_TRUE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kKindMismatch));
  TEST_ASSERT_TRUE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kBelowFloor));
}

void test_is_rejected_false_for_valid(void) {
  TEST_ASSERT_FALSE(otaAuthenticityIsRejected(OtaAuthenticityVerdict::kValid));
}

void test_labels_are_distinct_and_nonnull(void) {
  const char* labels[7] = {
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kAbsent),
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kMalformed),
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kVerificationImpossible),
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kInvalid),
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kKindMismatch),
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kBelowFloor),
      otaAuthenticityVerdictLabel(OtaAuthenticityVerdict::kValid),
  };
  for (int i = 0; i < 7; ++i) {
    TEST_ASSERT_NOT_NULL(labels[i]);
  }
  // Distinction deux à deux par CONTENU (pas seulement par pointeur).
  for (int i = 0; i < 7; ++i) {
    for (int j = i + 1; j < 7; ++j) {
      TEST_ASSERT_TRUE(strcmp(labels[i], labels[j]) != 0);
    }
  }
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_valid_when_everything_matches);

  RUN_TEST(test_absent_when_manifest_missing);
  RUN_TEST(test_absent_when_signature_missing);
  RUN_TEST(test_absent_when_both_missing);
  RUN_TEST(test_absent_takes_priority_over_crypto);

  RUN_TEST(test_malformed_when_manifest_parse_fails);
  RUN_TEST(test_malformed_when_signature_decode_fails);
  RUN_TEST(test_malformed_takes_priority_over_crypto);

  RUN_TEST(test_verification_impossible);
  RUN_TEST(test_invalid_signature);

  RUN_TEST(test_kind_mismatch_firmware_vs_filesystem);
  RUN_TEST(test_kind_match_firmware);
  RUN_TEST(test_kind_match_filesystem);

  RUN_TEST(test_below_floor_major);
  RUN_TEST(test_at_floor_is_not_below);
  RUN_TEST(test_floor_never_triggers_with_default_zero_floor);

  RUN_TEST(test_is_rejected_true_for_all_but_valid);
  RUN_TEST(test_is_rejected_false_for_valid);
  RUN_TEST(test_labels_are_distinct_and_nonnull);

  return UNITY_END();
}
