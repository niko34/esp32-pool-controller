// =============================================================================
// Tests unitaires natifs — Cohérence « type de mise à jour ↔ image reçue »
// (ota_type_logic, bug-echec-ota-indechiffrable-et-noyade-du-journal / 2.20.1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/ota_type_logic.h
// (ADR-0017), header-only : aucune entrée dans build_src_filter n'est requise.
//   - inferOtaKindFromFilename : déduction du type par le NOM du fichier, dont
//     le nom NU `littlefs.bin` produit par build_fs.sh — le trou historique de
//     la détection, qui n'acceptait que les suffixes précédés d'un point ;
//   - checkOtaFirstByte : pré-contrôle de l'octet magique ESP32 (0xE9) sur le
//     tout premier octet reçu, AVANT le moindre effet de bord.
// =============================================================================

#include <unity.h>
#include "ota_type_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. inferOtaKindFromFilename — images filesystem
// ---------------------------------------------------------------------------

// LE cas du bug : nom NU, sans point séparateur, tel que produit par build_fs.sh.
void test_infer_bare_littlefs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("littlefs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_dotted_littlefs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename(".littlefs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_versioned_littlefs_bin_is_filesystem(void) {
  // Nom des artefacts de release GitHub.
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("v2.20.1.littlefs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_bare_spiffs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("spiffs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_dotted_spiffs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename(".spiffs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_dotted_fs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("x.fs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_filesystem_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("filesystem.bin") == OtaImageKind::kFilesystem);
}

// ---------------------------------------------------------------------------
// 2. inferOtaKindFromFilename — images firmware
// ---------------------------------------------------------------------------

void test_infer_firmware_bin_is_firmware(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("firmware.bin") == OtaImageKind::kFirmware);
}

void test_infer_prefixed_firmware_bin_is_firmware(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("x.firmware.bin") == OtaImageKind::kFirmware);
}

void test_infer_unrelated_bin_defaults_to_firmware(void) {
  // Repli historique assumé : tout .bin non reconnu comme FS est présumé firmware.
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("blob.bin") == OtaImageKind::kFirmware);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename(".bin") == OtaImageKind::kFirmware);
}

// `fs.bin` est reconnu comme filesystem UNIQUEMENT s'il est précédé d'un
// séparateur (`.`, `-`, `_`) ou s'il constitue le nom de base entier. Le miroir
// JS (data/app.js, detectUpdateType) ancre la même règle : toute évolution
// touche les DEUX côtés.
void test_infer_bare_fs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("fs.bin") == OtaImageKind::kFilesystem);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("/tmp/build/fs.bin") == OtaImageKind::kFilesystem);
}

void test_infer_separated_fs_bin_is_filesystem(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("pool.fs.bin") == OtaImageKind::kFilesystem);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("pool-fs.bin") == OtaImageKind::kFilesystem);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("pool_fs.bin") == OtaImageKind::kFilesystem);
}

// NON-RÉGRESSION (revue 2.20.1) : un `endsWith("fs.bin")` non ancré classait
// ces noms en filesystem et pré-réglait le type silencieusement — un firmware
// ainsi déclaré part dans la partition LittleFS, sans refus dur possible dans
// ce sens (LittleFS n'a pas de magique fiable à l'offset 0).
void test_infer_fs_bin_suffix_is_anchored(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("refs.bin") == OtaImageKind::kFirmware);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("defs.bin") == OtaImageKind::kFirmware);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("mytoolfs.bin") == OtaImageKind::kFirmware);
}

// ---------------------------------------------------------------------------
// 3. inferOtaKindFromFilename — noms non concluants
// ---------------------------------------------------------------------------

void test_infer_null_is_unknown(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename(nullptr) == OtaImageKind::kUnknown);
}

void test_infer_empty_is_unknown(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("") == OtaImageKind::kUnknown);
}

void test_infer_no_extension_is_unknown(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("littlefs") == OtaImageKind::kUnknown);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("firmware") == OtaImageKind::kUnknown);
}

void test_infer_other_extension_is_unknown(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("littlefs.bin.gz") == OtaImageKind::kUnknown);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("notes.txt") == OtaImageKind::kUnknown);
}

// Nom réduit au séparateur : le nom de base est vide → indécidable.
void test_infer_trailing_separator_is_unknown(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("/") == OtaImageKind::kUnknown);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("build/") == OtaImageKind::kUnknown);
}

// ---------------------------------------------------------------------------
// 4. inferOtaKindFromFilename — chemins et casse
// ---------------------------------------------------------------------------

void test_infer_ignores_posix_directories(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("/home/u/data-build/littlefs.bin") ==
                   OtaImageKind::kFilesystem);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("./.pio/build/esp32dev/firmware.bin") ==
                   OtaImageKind::kFirmware);
}

void test_infer_ignores_windows_directories(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("C:\\builds\\littlefs.bin") ==
                   OtaImageKind::kFilesystem);
}

// Seul le nom de BASE compte : un répertoire trompeur ne doit pas déteindre.
void test_infer_directory_name_does_not_leak(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("/littlefs.bin/firmware.bin") ==
                   OtaImageKind::kFirmware);
}

void test_infer_is_case_insensitive(void) {
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("LittleFS.BIN") == OtaImageKind::kFilesystem);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("SPIFFS.Bin") == OtaImageKind::kFilesystem);
  TEST_ASSERT_TRUE(inferOtaKindFromFilename("FIRMWARE.BIN") == OtaImageKind::kFirmware);
}

// ---------------------------------------------------------------------------
// 5. checkOtaFirstByte — les 4 combinaisons
// ---------------------------------------------------------------------------

void test_check_firmware_with_magic_accepts(void) {
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0xE9) == OtaTypeVerdict::kAccept);
}

void test_check_firmware_without_magic_rejects(void) {
  // Le cas du bug : image LittleFS envoyée en type « firmware ».
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0x00) == OtaTypeVerdict::kRejectNotFirmware);
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0xE8) == OtaTypeVerdict::kRejectNotFirmware);
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0xFF) == OtaTypeVerdict::kRejectNotFirmware);
}

void test_check_filesystem_with_magic_warns_only(void) {
  // Jamais un refus dans ce sens : 0xE9 fortuit possible en tête d'image LittleFS.
  TEST_ASSERT_TRUE(checkOtaFirstByte(false, 0xE9) == OtaTypeVerdict::kWarnLooksLikeFirmware);
}

void test_check_filesystem_without_magic_accepts(void) {
  TEST_ASSERT_TRUE(checkOtaFirstByte(false, 0x00) == OtaTypeVerdict::kAccept);
  TEST_ASSERT_TRUE(checkOtaFirstByte(false, 0xFF) == OtaTypeVerdict::kAccept);
}

// Le pré-contrôle doit rester aligné sur ESP_IMAGE_HEADER_MAGIC du framework.
void test_magic_constant_is_esp_image_header_magic(void) {
  TEST_ASSERT_EQUAL_HEX8(0xE9, kEspImageMagic);
}

// checkOtaFirstByte est une fonction PURE : appels répétés, verdict identique
// (aucun état résiduel entre deux tentatives — AC5 feature-026).
void test_check_is_stateless_across_calls(void) {
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0x00) == OtaTypeVerdict::kRejectNotFirmware);
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0x00) == OtaTypeVerdict::kRejectNotFirmware);
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0xE9) == OtaTypeVerdict::kAccept);
  TEST_ASSERT_TRUE(checkOtaFirstByte(true, 0x00) == OtaTypeVerdict::kRejectNotFirmware);
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_infer_bare_littlefs_bin_is_filesystem);
  RUN_TEST(test_infer_dotted_littlefs_bin_is_filesystem);
  RUN_TEST(test_infer_versioned_littlefs_bin_is_filesystem);
  RUN_TEST(test_infer_bare_spiffs_bin_is_filesystem);
  RUN_TEST(test_infer_dotted_spiffs_bin_is_filesystem);
  RUN_TEST(test_infer_dotted_fs_bin_is_filesystem);
  RUN_TEST(test_infer_filesystem_bin_is_filesystem);

  RUN_TEST(test_infer_firmware_bin_is_firmware);
  RUN_TEST(test_infer_prefixed_firmware_bin_is_firmware);
  RUN_TEST(test_infer_unrelated_bin_defaults_to_firmware);
  RUN_TEST(test_infer_bare_fs_bin_is_filesystem);
  RUN_TEST(test_infer_separated_fs_bin_is_filesystem);
  RUN_TEST(test_infer_fs_bin_suffix_is_anchored);

  RUN_TEST(test_infer_null_is_unknown);
  RUN_TEST(test_infer_empty_is_unknown);
  RUN_TEST(test_infer_no_extension_is_unknown);
  RUN_TEST(test_infer_other_extension_is_unknown);
  RUN_TEST(test_infer_trailing_separator_is_unknown);

  RUN_TEST(test_infer_ignores_posix_directories);
  RUN_TEST(test_infer_ignores_windows_directories);
  RUN_TEST(test_infer_directory_name_does_not_leak);
  RUN_TEST(test_infer_is_case_insensitive);

  RUN_TEST(test_check_firmware_with_magic_accepts);
  RUN_TEST(test_check_firmware_without_magic_rejects);
  RUN_TEST(test_check_filesystem_with_magic_warns_only);
  RUN_TEST(test_check_filesystem_without_magic_accepts);
  RUN_TEST(test_magic_constant_is_esp_image_header_magic);
  RUN_TEST(test_check_is_stateless_across_calls);

  return UNITY_END();
}
