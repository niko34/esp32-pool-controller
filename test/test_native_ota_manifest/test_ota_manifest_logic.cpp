// =============================================================================
// Tests unitaires natifs — ota_manifest_logic (feature-151 D1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// Couvre la grammaire STRICTE du manifeste de signature OTA :
//   pool-ota/1 <firmware|filesystem> <MAJ.MIN.PATCH> sha256:<64 hex minuscules>
// Les mutations EXIGÉES par la spec (LF ajouté, double espace, 'v' préfixé,
// zéro de tête, 5e jeton, jeton manquant, "Firmware" capitalisé) sont toutes
// couvertes ci-dessous — chacune DOIT être refusée (kMalformed).
// =============================================================================

#include <unity.h>
#include <string.h>

#include "ota_manifest_logic.h"

static const char kValidManifest[] =
    "pool-ota/1 firmware 2.85.0 "
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Cas valides
// -----------------------------------------------------------------------------
void test_parse_valid_firmware(void) {
  OtaManifestFields fields;
  memset(&fields, 0xAA, sizeof(fields));
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(kValidManifest, &fields));
  TEST_ASSERT_EQUAL(OtaManifestKind::kFirmware, fields.kind);
  TEST_ASSERT_EQUAL_UINT32(2, fields.major);
  TEST_ASSERT_EQUAL_UINT32(85, fields.minor);
  TEST_ASSERT_EQUAL_UINT32(0, fields.patch);
  TEST_ASSERT_EQUAL_STRING(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", fields.sha256Hex);
}

void test_parse_valid_filesystem(void) {
  const char* raw =
      "pool-ota/1 filesystem 10.2.33 "
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(raw, &fields));
  TEST_ASSERT_EQUAL(OtaManifestKind::kFilesystem, fields.kind);
  TEST_ASSERT_EQUAL_UINT32(10, fields.major);
  TEST_ASSERT_EQUAL_UINT32(2, fields.minor);
  TEST_ASSERT_EQUAL_UINT32(33, fields.patch);
}

void test_parse_valid_version_zero_component(void) {
  // "0" seul est un composant valide (pas un zéro de tête).
  const char* raw =
      "pool-ota/1 firmware 0.0.1 "
      "sha256:00000000000000000000000000000000000000000000000000000000000000ab";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(raw, &fields));
  TEST_ASSERT_EQUAL_UINT32(0, fields.major);
  TEST_ASSERT_EQUAL_UINT32(0, fields.minor);
  TEST_ASSERT_EQUAL_UINT32(1, fields.patch);
}

void test_parse_valid_max_5digit_version(void) {
  const char* raw =
      "pool-ota/1 firmware 99999.99999.99999 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(raw, &fields));
  TEST_ASSERT_EQUAL_UINT32(99999, fields.major);
  TEST_ASSERT_EQUAL_UINT32(99999, fields.minor);
  TEST_ASSERT_EQUAL_UINT32(99999, fields.patch);
}

// -----------------------------------------------------------------------------
// Mutations EXIGÉES par la spec feature-151 — toutes DOIVENT être refusées
// -----------------------------------------------------------------------------
void test_reject_trailing_lf(void) {
  // Piège "echo vs printf" identifié par l'audit — un LF final doit être
  // refusé, jamais toléré silencieusement.
  const char raw[] =
      "pool-ota/1 firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_double_space(void) {
  const char* raw =
      "pool-ota/1 firmware  2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_version_v_prefix(void) {
  const char* raw =
      "pool-ota/1 firmware v2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_version_leading_zero(void) {
  const char* raw =
      "pool-ota/1 firmware 02.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_fifth_token(void) {
  const char* raw =
      "pool-ota/1 firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef extra";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_missing_token(void) {
  // Seulement 3 jetons (genre manquant).
  const char* raw =
      "pool-ota/1 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_capitalized_firmware(void) {
  const char* raw =
      "pool-ota/1 Firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

// -----------------------------------------------------------------------------
// Autres cas invalides
// -----------------------------------------------------------------------------
void test_reject_null_raw(void) {
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(nullptr, &fields));
}

void test_reject_null_out(void) {
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(kValidManifest, nullptr));
}

void test_reject_empty_string(void) {
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest("", &fields));
}

void test_reject_leading_space(void) {
  const char* raw =
      " pool-ota/1 firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_trailing_space(void) {
  const char* raw =
      "pool-ota/1 firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef ";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_wrong_magic(void) {
  const char* raw =
      "pool-ota/2 firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_unknown_kind(void) {
  const char* raw =
      "pool-ota/1 bootloader 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_uppercase_hex(void) {
  const char* raw =
      "pool-ota/1 firmware 2.85.0 "
      "sha256:0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_short_hex(void) {
  const char* raw = "pool-ota/1 firmware 2.85.0 sha256:0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_missing_sha256_prefix(void) {
  const char* raw =
      "pool-ota/1 firmware 2.85.0 "
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_version_component_too_long(void) {
  const char* raw =
      "pool-ota/1 firmware 123456.0.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_version_non_digit(void) {
  const char* raw =
      "pool-ota/1 firmware 2.8x.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_only_two_version_components(void) {
  const char* raw =
      "pool-ota/1 firmware 2.85 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

void test_reject_control_char_tab(void) {
  const char raw[] =
      "pool-ota/1 firmware 2.85.0 "
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab\tc";
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kMalformed, parseOtaManifest(raw, &fields));
}

// -----------------------------------------------------------------------------
// Re-sérialisation canonique
// -----------------------------------------------------------------------------
void test_serialize_roundtrip(void) {
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(kValidManifest, &fields));
  char out[kOtaManifestMaxLen];
  size_t n = serializeOtaManifest(fields, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING(kValidManifest, out);
}

void test_serialize_filesystem_kind_literal(void) {
  OtaManifestFields fields{};
  fields.kind = OtaManifestKind::kFilesystem;
  fields.major = 1;
  fields.minor = 0;
  fields.patch = 0;
  // Réutilise le même hex de 64 caractères que kValidManifest (longueur déjà
  // vérifiée ailleurs dans ce fichier) — évite tout risque de compte erroné.
  strcpy(fields.sha256Hex, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  char out[kOtaManifestMaxLen];
  size_t n = serializeOtaManifest(fields, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_TRUE(strstr(out, "filesystem") != nullptr);
  TEST_ASSERT_TRUE(strstr(out, "pool-ota/1 filesystem 1.0.0 sha256:") == out);
}

void test_serialize_rejects_too_small_buffer(void) {
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(kValidManifest, &fields));
  char out[8];
  size_t n = serializeOtaManifest(fields, out, sizeof(out));
  TEST_ASSERT_EQUAL(0, n);
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_serialize_rejects_null_out(void) {
  OtaManifestFields fields;
  TEST_ASSERT_EQUAL(OtaManifestParseVerdict::kOk, parseOtaManifest(kValidManifest, &fields));
  TEST_ASSERT_EQUAL(0, serializeOtaManifest(fields, nullptr, 10));
}

// -----------------------------------------------------------------------------
// Libellé du genre
// -----------------------------------------------------------------------------
void test_kind_literal_firmware(void) {
  TEST_ASSERT_EQUAL_STRING("firmware", otaManifestKindLiteral(OtaManifestKind::kFirmware));
}

void test_kind_literal_filesystem(void) {
  TEST_ASSERT_EQUAL_STRING("filesystem", otaManifestKindLiteral(OtaManifestKind::kFilesystem));
}

// -----------------------------------------------------------------------------
// Plancher de version
// -----------------------------------------------------------------------------
void test_floor_equal_is_not_below(void) {
  TEST_ASSERT_FALSE(otaVersionBelowFloor(2, 85, 0, 2, 85, 0));
}

void test_floor_above_is_not_below(void) {
  TEST_ASSERT_FALSE(otaVersionBelowFloor(2, 85, 1, 2, 85, 0));
  TEST_ASSERT_FALSE(otaVersionBelowFloor(2, 86, 0, 2, 85, 0));
  TEST_ASSERT_FALSE(otaVersionBelowFloor(3, 0, 0, 2, 85, 0));
}

void test_floor_below_major(void) {
  TEST_ASSERT_TRUE(otaVersionBelowFloor(1, 99, 99, 2, 0, 0));
}

void test_floor_below_minor(void) {
  TEST_ASSERT_TRUE(otaVersionBelowFloor(2, 84, 99, 2, 85, 0));
}

void test_floor_below_patch(void) {
  TEST_ASSERT_TRUE(otaVersionBelowFloor(2, 85, 0, 2, 85, 1));
}

void test_floor_zero_never_below_zero(void) {
  // D1 : plancher figé à 0.0.0 — toute version réelle du projet est >= 0.0.1.
  TEST_ASSERT_FALSE(otaVersionBelowFloor(2, 85, 0, 0, 0, 0));
  TEST_ASSERT_FALSE(otaVersionBelowFloor(0, 0, 0, 0, 0, 0));
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_parse_valid_firmware);
  RUN_TEST(test_parse_valid_filesystem);
  RUN_TEST(test_parse_valid_version_zero_component);
  RUN_TEST(test_parse_valid_max_5digit_version);

  RUN_TEST(test_reject_trailing_lf);
  RUN_TEST(test_reject_double_space);
  RUN_TEST(test_reject_version_v_prefix);
  RUN_TEST(test_reject_version_leading_zero);
  RUN_TEST(test_reject_fifth_token);
  RUN_TEST(test_reject_missing_token);
  RUN_TEST(test_reject_capitalized_firmware);

  RUN_TEST(test_reject_null_raw);
  RUN_TEST(test_reject_null_out);
  RUN_TEST(test_reject_empty_string);
  RUN_TEST(test_reject_leading_space);
  RUN_TEST(test_reject_trailing_space);
  RUN_TEST(test_reject_wrong_magic);
  RUN_TEST(test_reject_unknown_kind);
  RUN_TEST(test_reject_uppercase_hex);
  RUN_TEST(test_reject_short_hex);
  RUN_TEST(test_reject_missing_sha256_prefix);
  RUN_TEST(test_reject_version_component_too_long);
  RUN_TEST(test_reject_version_non_digit);
  RUN_TEST(test_reject_only_two_version_components);
  RUN_TEST(test_reject_control_char_tab);

  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_serialize_filesystem_kind_literal);
  RUN_TEST(test_serialize_rejects_too_small_buffer);
  RUN_TEST(test_serialize_rejects_null_out);

  RUN_TEST(test_kind_literal_firmware);
  RUN_TEST(test_kind_literal_filesystem);

  RUN_TEST(test_floor_equal_is_not_below);
  RUN_TEST(test_floor_above_is_not_below);
  RUN_TEST(test_floor_below_major);
  RUN_TEST(test_floor_below_minor);
  RUN_TEST(test_floor_below_patch);
  RUN_TEST(test_floor_zero_never_below_zero);

  return UNITY_END();
}
