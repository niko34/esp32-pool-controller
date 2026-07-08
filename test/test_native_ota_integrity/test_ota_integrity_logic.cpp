// =============================================================================
// Tests unitaires natifs — ota_integrity_logic (feature-026)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// On teste le COMPORTEMENT observable du module pur d'intégrité OTA :
//   - parseSha256Digest : format "sha256:<64hex>" strict, fail-closed
//     (préfixe obligatoire casse indifférente, longueur exacte, hex only,
//      rien après les 64 hex, NULL refusé)
//   - sha256Equal       : égalité 32 octets (temps constant côté impl)
//   - sha256ToHex       : formatage minuscule, défensif si buffer < 65
// via l'API publique, pas l'implémentation interne.
// =============================================================================

#include <unity.h>
#include <string.h>  // Headers C uniquement (libc++ indisponible sur l'hôte)
#include "ota_integrity_logic.h"

// Empreinte de référence : 64 hex couvrant tous les nibbles 0..f,
// avec octets connus pour vérifier la conversion binaire.
static const char kHexRef[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const uint8_t kBinRef[32] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

// Motif sentinelle pour détecter toute écriture parasite dans `out`
// quand parseSha256Digest doit échouer (fail-closed ne garantit pas le
// contenu de out, mais on initialise pour éviter les lectures indéfinies).
static void fillSentinel(uint8_t out[32]) { memset(out, 0x5A, 32); }

// Construit "sha256:" + hex dans buf (buf >= 8 + strlen(hex) + 1).
static void makeDigest(char* buf, const char* hex) {
  strcpy(buf, "sha256:");
  strcat(buf, hex);
}

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// parseSha256Digest — cas valides
// -----------------------------------------------------------------------------
void test_parse_valid_lowercase(void) {
  char digest[80];
  makeDigest(digest, kHexRef);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_TRUE(parseSha256Digest(digest, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kBinRef, out, 32);
}

void test_parse_valid_uppercase_hex(void) {
  // Hex tout en MAJUSCULES → accepté (casse indifférente), même binaire.
  char digest[80];
  makeDigest(digest,
             "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_TRUE(parseSha256Digest(digest, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kBinRef, out, 32);
}

void test_parse_valid_mixed_case_prefix_and_hex(void) {
  // Préfixe ET hex en casse mixte → accepté.
  char digest[80];
  strcpy(digest, "ShA256:");
  strcat(digest,
         "0123456789aBcDeF0123456789AbCdEf0123456789abcdef0123456789ABCDEF");
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_TRUE(parseSha256Digest(digest, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kBinRef, out, 32);
}

// -----------------------------------------------------------------------------
// parseSha256Digest — cas invalides (fail-closed → false)
// -----------------------------------------------------------------------------
void test_parse_rejects_bare_hex_without_prefix(void) {
  // "<64hex>" nu, sans "sha256:" → refusé.
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(kHexRef, out));
}

void test_parse_rejects_wrong_prefix_md5(void) {
  char digest[80];
  strcpy(digest, "md5:");
  strcat(digest, kHexRef);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_prefix_without_colon(void) {
  // "sha256" collé aux hex (pas de ':') → refusé.
  char digest[80];
  strcpy(digest, "sha256");
  strcat(digest, kHexRef);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_63_hex_chars(void) {
  char hex63[64];
  memcpy(hex63, kHexRef, 63);
  hex63[63] = '\0';
  char digest[80];
  makeDigest(digest, hex63);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_65_hex_chars(void) {
  char hex65[66];
  memcpy(hex65, kHexRef, 64);
  hex65[64] = 'a';
  hex65[65] = '\0';
  char digest[80];
  makeDigest(digest, hex65);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_nonhex_first_position(void) {
  char hex[65];
  memcpy(hex, kHexRef, 65);
  hex[0] = 'g';  // Non-hex en tête
  char digest[80];
  makeDigest(digest, hex);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_nonhex_middle_position(void) {
  char hex[65];
  memcpy(hex, kHexRef, 65);
  hex[31] = 'z';  // Non-hex au milieu (nibble bas de l'octet 15)
  char digest[80];
  makeDigest(digest, hex);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_nonhex_last_position(void) {
  char hex[65];
  memcpy(hex, kHexRef, 65);
  hex[63] = '!';  // Non-hex en dernière position
  char digest[80];
  makeDigest(digest, hex);
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

void test_parse_rejects_empty_string(void) {
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest("", out));
}

void test_parse_rejects_null_digest(void) {
  // L'API accepte un pointeur nul en entrée → false (fail-closed documenté).
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(NULL, out));
}

void test_parse_rejects_null_out(void) {
  char digest[80];
  makeDigest(digest, kHexRef);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, NULL));
}

void test_parse_rejects_prefix_only(void) {
  // "sha256:" sans aucun hex → refusé (chaîne trop courte).
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest("sha256:", out));
}

void test_parse_rejects_trailing_garbage(void) {
  // Caractères en trop APRÈS les 64 hex (y compris espace) → refusé.
  char digest[90];
  makeDigest(digest, kHexRef);
  strcat(digest, " xyz");
  uint8_t out[32];
  fillSentinel(out);
  TEST_ASSERT_FALSE(parseSha256Digest(digest, out));
}

// -----------------------------------------------------------------------------
// Round-trip : parse puis sha256ToHex redonne les 64 hex MINUSCULES d'origine
// -----------------------------------------------------------------------------
void test_roundtrip_parse_then_tohex(void) {
  // Entrée en MAJUSCULES pour vérifier que la sortie est normalisée minuscule.
  char digest[80];
  makeDigest(digest,
             "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
  uint8_t bin[32];
  TEST_ASSERT_TRUE(parseSha256Digest(digest, bin));
  char hexOut[65];
  sha256ToHex(bin, hexOut, sizeof(hexOut));
  TEST_ASSERT_EQUAL_STRING(kHexRef, hexOut);  // 64 hex minuscules identiques
}

// -----------------------------------------------------------------------------
// sha256Equal
// -----------------------------------------------------------------------------
void test_equal_identical_hashes(void) {
  uint8_t a[32], b[32];
  memcpy(a, kBinRef, 32);
  memcpy(b, kBinRef, 32);
  TEST_ASSERT_TRUE(sha256Equal(a, b));
}

void test_equal_differs_first_byte(void) {
  uint8_t a[32], b[32];
  memcpy(a, kBinRef, 32);
  memcpy(b, kBinRef, 32);
  b[0] ^= 0x01;  // Différence sur le PREMIER octet
  TEST_ASSERT_FALSE(sha256Equal(a, b));
}

void test_equal_differs_last_byte(void) {
  uint8_t a[32], b[32];
  memcpy(a, kBinRef, 32);
  memcpy(b, kBinRef, 32);
  b[31] ^= 0x80;  // Différence sur le DERNIER octet
  TEST_ASSERT_FALSE(sha256Equal(a, b));
}

// -----------------------------------------------------------------------------
// sha256ToHex — tailles de buffer
// -----------------------------------------------------------------------------
void test_tohex_buffer_exactly_65(void) {
  char out[66];
  memset(out, 'X', sizeof(out));
  sha256ToHex(kBinRef, out, 65);  // Taille minimale exacte
  TEST_ASSERT_EQUAL_STRING(kHexRef, out);
  TEST_ASSERT_EQUAL_CHAR('X', out[65]);  // Aucun débordement au-delà de 65
}

void test_tohex_buffer_64_gives_empty_string(void) {
  // Buffer trop petit (64 < 65) → chaîne vide, comportement défensif.
  char out[64];
  memset(out, 'X', sizeof(out));
  sha256ToHex(kBinRef, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // parseSha256Digest — valides
  RUN_TEST(test_parse_valid_lowercase);
  RUN_TEST(test_parse_valid_uppercase_hex);
  RUN_TEST(test_parse_valid_mixed_case_prefix_and_hex);

  // parseSha256Digest — invalides (fail-closed)
  RUN_TEST(test_parse_rejects_bare_hex_without_prefix);
  RUN_TEST(test_parse_rejects_wrong_prefix_md5);
  RUN_TEST(test_parse_rejects_prefix_without_colon);
  RUN_TEST(test_parse_rejects_63_hex_chars);
  RUN_TEST(test_parse_rejects_65_hex_chars);
  RUN_TEST(test_parse_rejects_nonhex_first_position);
  RUN_TEST(test_parse_rejects_nonhex_middle_position);
  RUN_TEST(test_parse_rejects_nonhex_last_position);
  RUN_TEST(test_parse_rejects_empty_string);
  RUN_TEST(test_parse_rejects_null_digest);
  RUN_TEST(test_parse_rejects_null_out);
  RUN_TEST(test_parse_rejects_prefix_only);
  RUN_TEST(test_parse_rejects_trailing_garbage);

  // Round-trip
  RUN_TEST(test_roundtrip_parse_then_tohex);

  // sha256Equal
  RUN_TEST(test_equal_identical_hashes);
  RUN_TEST(test_equal_differs_first_byte);
  RUN_TEST(test_equal_differs_last_byte);

  // sha256ToHex
  RUN_TEST(test_tohex_buffer_exactly_65);
  RUN_TEST(test_tohex_buffer_64_gives_empty_string);

  return UNITY_END();
}
