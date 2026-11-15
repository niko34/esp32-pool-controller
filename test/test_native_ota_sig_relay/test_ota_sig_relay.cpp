// =============================================================================
// Tests unitaires natifs — ota_sig_relay_logic (feature-151 D3-a)
// =============================================================================
// 🔴 TROISIÈME copie de la grammaire du fichier `.sig` (après le shell
// tools/check_release_artefacts.sh et le JS parseSigFile de data/app.js).
// Cette suite REPREND la matrice de test/sig_parse_vectors.js (ADR-0099) :
// les verdicts C++ ont VOLONTAIREMENT les mêmes libellés que les codes du JS
// ("vide", "lignes", "manifeste", "signature") — on compare donc le libellé au
// code attendu par le vecteur JS, ligne pour ligne. Une divergence entre les
// copies fait échouer l'une des deux suites.
//
// Cas propres au C++ (entrée RÉSEAU) : borne 1 Ko, entrée non terminée par NUL,
// NUL embarqué, re-sérialisation canonique.
// =============================================================================

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unity.h>

#include "ota_sig_relay_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Mêmes fixtures que test/sig_parse_vectors.js (hex et base64 de 512 car.).
static const std::string HEX_A = "a19d0444d96180703c7a52c0f2dfdf6a00d3020ff61ab95eac4d7664cc63d4a1";
static const std::string HEX_B = "19ba22b001d0e53496d2ca02700955b54bb2839e9765746be37e1fa5713cf71e";
static const std::string B64_A =
    "HA+ekXlBh0Hr6gHmFjv9jOKxOvJ3GOYYlmIcAFPKAoLDZsPJPrLJPFELVNrj03Kly6mHCeQ+/CXXW8sktCqZfp602JyT/"
    "u+IOBoZ6i06+2dU5xeMLxbH5gn9aXV1polUwj98gNYg0NTNCbaDRHXMYXDAVjRdMdFDDXKXe/XhZE1pmUpPWCn6vFoP5Dk2X6"
    "xtKSdz9SXubkVkBsLbDG8YVzVAvLLugTTBlTleM04dFeLzn1lsRYTeQyhxYg8eGTQVhXjUFBJLJ/"
    "ddcS5rljJ1rtYOZ8ZM9tgeC8LXuXlgcRIKlIV51THn/mOzJLnwBik2sSNWzgBdx7PHZEst7JyexY/"
    "iLUWeByHg4qYI2QpHudh5C0eBZl5poFRoXONU9Lk1UFu6I/sLsD+pDbCQ584aMQyfmATlFeyFjiETN0eAtKltSvH4CJKYq6"
    "GkwsHLKEgkHZZ31bLaapaAqHuQ2FMYESEVzXZ+JAU50KQYl+loCFbV8uj38i5hvGCcKBAvC4O5";

static std::string manifestLine(const std::string& genre = "firmware",
                                const std::string& version = "2.86.0",
                                const std::string& hex = HEX_A) {
  return "pool-ota/1 " + genre + " " + version + " sha256:" + hex;
}
static std::string sigText(const std::string& genre, const std::string& version,
                           const std::string& hex, const std::string& b64) {
  return manifestLine(genre, version, hex) + "\n" + b64 + "\n";
}

// Appelle le parseur avec un tampon de la TAILLE EXACTE (aucun NUL final : un
// dépassement serait vu par ASan, et strlen() planterait/divergerait).
static OtaSigParseVerdict parse(const std::string& s, OtaSigRelayPayload* out = nullptr) {
  OtaSigRelayPayload local;
  memset(&local, 0x5A, sizeof(local));
  char* buf = (char*)malloc(s.size() == 0 ? 1 : s.size());
  memcpy(buf, s.data(), s.size());
  const OtaSigParseVerdict v = parseOtaSigFile(buf, s.size(), out ? out : &local);
  free(buf);
  return v;
}

static void expectLabel(const char* label, const std::string& text, const char* jsCode) {
  const OtaSigParseVerdict v = parse(text);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(jsCode, otaSigParseVerdictLabel(v), label);
}

// --- Nominal ----------------------------------------------------------------
void test_nominal_firmware(void) {
  OtaSigRelayPayload p;
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk,
                    (int)parse(sigText("firmware", "2.86.0", HEX_A, B64_A), &p));
  TEST_ASSERT_EQUAL((int)OtaManifestKind::kFirmware, (int)p.fields.kind);
  TEST_ASSERT_EQUAL_UINT32(2, p.fields.major);
  TEST_ASSERT_EQUAL_UINT32(86, p.fields.minor);
  TEST_ASSERT_EQUAL_UINT32(0, p.fields.patch);
  TEST_ASSERT_EQUAL_STRING(HEX_A.c_str(), p.fields.sha256Hex);
  TEST_ASSERT_EQUAL_STRING(manifestLine().c_str(), p.canonical);
  TEST_ASSERT_EQUAL_UINT32(manifestLine().size(), p.canonicalLen);
  TEST_ASSERT_EQUAL_STRING(B64_A.c_str(), p.signatureBase64);
}

void test_nominal_filesystem(void) {
  OtaSigRelayPayload p;
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk,
                    (int)parse(sigText("filesystem", "2.86.0", HEX_B, B64_A), &p));
  TEST_ASSERT_EQUAL((int)OtaManifestKind::kFilesystem, (int)p.fields.kind);
  TEST_ASSERT_EQUAL_STRING(HEX_B.c_str(), p.fields.sha256Hex);
  TEST_ASSERT_EQUAL_STRING(manifestLine("filesystem", "2.86.0", HEX_B).c_str(), p.canonical);
}

// --- Tolérances -------------------------------------------------------------
// ⭐ La charge relayée est la ligne RE-SÉRIALISÉE : jamais de \r ni de \n.
void test_crlf_tolerated_and_canonical_has_no_control_chars(void) {
  OtaSigRelayPayload p;
  const std::string crlf = manifestLine() + "\r\n" + B64_A + "\r\n";
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk, (int)parse(crlf, &p));
  TEST_ASSERT_EQUAL_STRING(manifestLine().c_str(), p.canonical);
  TEST_ASSERT_EQUAL_UINT32(manifestLine().size(), p.canonicalLen);
  TEST_ASSERT_NULL(strchr(p.canonical, '\r'));
  TEST_ASSERT_NULL(strchr(p.canonical, '\n'));
  TEST_ASSERT_EQUAL_STRING(B64_A.c_str(), p.signatureBase64);  // sans \r
  TEST_ASSERT_EQUAL_UINT32(512, strlen(p.signatureBase64));
}

void test_crlf_between_lines_only_and_lf_only_and_no_final_lf(void) {
  OtaSigRelayPayload p;
  // CRLF entre les lignes, pas de saut final
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk,
                    (int)parse(manifestLine() + "\r\n" + B64_A, &p));
  TEST_ASSERT_EQUAL_STRING(manifestLine().c_str(), p.canonical);
  // LF final unique
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk,
                    (int)parse(manifestLine() + "\n" + B64_A + "\n", &p));
  TEST_ASSERT_EQUAL_STRING(manifestLine().c_str(), p.canonical);
  // aucun LF final (équivalent JS « sans LF final : ok »)
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk, (int)parse(manifestLine() + "\n" + B64_A, &p));
  TEST_ASSERT_EQUAL_STRING(B64_A.c_str(), p.signatureBase64);
}

// --- Matrice JS (mêmes cas, mêmes codes) -------------------------------------
void test_js_matrix_lignes(void) {
  expectLabel("3 lignes", manifestLine() + "\n" + B64_A + "\nligne de trop\n", "lignes");
  expectLabel("LF final EN PLUS", manifestLine() + "\n" + B64_A + "\n\n", "lignes");
  expectLabel("CRLF final EN PLUS", manifestLine() + "\r\n" + B64_A + "\r\n\r\n", "lignes");
  expectLabel("une seule ligne", manifestLine(), "lignes");
  expectLabel("une seule ligne + LF", manifestLine() + "\n", "lignes");
  expectLabel("ligne 2 vide", manifestLine() + "\n", "lignes");
  expectLabel("ligne 1 vide", "\n" + B64_A, "lignes");
  expectLabel("seulement un LF", "\n", "lignes");
  expectLabel("contenu au-dela de la ligne 2 (sans LF terminal)",
              manifestLine() + "\n" + B64_A + "\nx", "lignes");
}

void test_js_matrix_vide(void) {
  expectLabel("fichier vide", "", "vide");
  OtaSigRelayPayload p;
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kEmpty, (int)parseOtaSigFile(nullptr, 5, &p));
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kEmpty, (int)parseOtaSigFile("x", 1, nullptr));
}

void test_js_matrix_manifeste(void) {
  std::string s = sigText("firmware", "2.86.0", HEX_A, B64_A);
  s.replace(s.find("pool-ota/1"), 10, "pool-ota/2");
  expectLabel("prefixe altere", s, "manifeste");
  expectLabel("genre mal case", sigText("Firmware", "2.86.0", HEX_A, B64_A), "manifeste");
  expectLabel("genre inconnu", sigText("bootloader", "2.86.0", HEX_A, B64_A), "manifeste");
  expectLabel("version avec prefixe v", sigText("firmware", "v2.86.0", HEX_A, B64_A), "manifeste");
  expectLabel("zero de tete (major)", sigText("firmware", "02.86.0", HEX_A, B64_A), "manifeste");
  expectLabel("zero de tete (minor)", sigText("firmware", "2.086.0", HEX_A, B64_A), "manifeste");
  expectLabel("version non SemVer (lettre)", sigText("firmware", "2.x.0", HEX_A, B64_A), "manifeste");
  expectLabel("version a 4 composants", sigText("firmware", "2.86.0.1", HEX_A, B64_A), "manifeste");
  std::string up = HEX_A;
  for (char& c : up) c = (char)toupper((unsigned char)c);
  expectLabel("hex MAJUSCULES", sigText("firmware", "2.86.0", up, B64_A), "manifeste");
  expectLabel("hex 63", sigText("firmware", "2.86.0", HEX_A.substr(0, 63), B64_A), "manifeste");
  expectLabel("hex 65", sigText("firmware", "2.86.0", HEX_A + "a", B64_A), "manifeste");
  expectLabel("double espace",
              "pool-ota/1 firmware  2.86.0 sha256:" + HEX_A + "\n" + B64_A + "\n", "manifeste");
  expectLabel("espace de queue", manifestLine() + " \n" + B64_A + "\n", "manifeste");
  expectLabel("CR parasite en fin de ligne 1 (\\r\\r\\n)", manifestLine() + "\r\r\n" + B64_A + "\n",
              "manifeste");
}

void test_js_matrix_signature(void) {
  expectLabel("511 caracteres", sigText("firmware", "2.86.0", HEX_A, B64_A.substr(0, 511)),
              "signature");
  expectLabel("513 caracteres", sigText("firmware", "2.86.0", HEX_A, B64_A + "A"), "signature");
  expectLabel("512 OK au bord (controle positif)", sigText("firmware", "2.86.0", HEX_A, B64_A), "ok");
  expectLabel("'=' au milieu", sigText("firmware", "2.86.0", HEX_A, "=" + B64_A.substr(1)),
              "signature");
  expectLabel("'=' final (padding) a 512", sigText("firmware", "2.86.0", HEX_A,
                                                   B64_A.substr(0, 511) + "="), "signature");
  expectLabel("'-' (base64url)", sigText("firmware", "2.86.0", HEX_A, "-" + B64_A.substr(1)),
              "signature");
  expectLabel("'_' (base64url)", sigText("firmware", "2.86.0", HEX_A, B64_A.substr(0, 511) + "_"),
              "signature");
  expectLabel("espace dans la signature",
              sigText("firmware", "2.86.0", HEX_A, " " + B64_A.substr(1)), "signature");
  expectLabel("caractere non ASCII (0xC3)",
              sigText("firmware", "2.86.0", HEX_A, std::string("\xC3") + B64_A.substr(1)),
              "signature");
  expectLabel("CR final seul sans LF", manifestLine() + "\n" + B64_A + "\r", "signature");
}

// --- Cas propres au flux réseau ----------------------------------------------
void test_size_bound_1kib(void) {
  // 1024 octets : sous la borne => analysé (donc PAS kTooLong).
  TEST_ASSERT_TRUE((int)parse(std::string(1024, 'A')) != (int)OtaSigParseVerdict::kTooLong);
  // 1 Ko + 1 octet : refusé AVANT toute tokenisation.
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kTooLong, (int)parse(std::string(1025, 'A')));
  // Même un fichier VALIDE rallongé d'un octet au-delà de 1 Ko est refusé.
  std::string big = manifestLine() + "\n" + B64_A + "\n" + std::string(1025 - (manifestLine().size() + 1 + 512 + 1), ' ');
  TEST_ASSERT_EQUAL_UINT32(1025, big.size());
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kTooLong, (int)parse(big));
  // Gros corps (64 Ko) : refusé sans lire plus loin.
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kTooLong, (int)parse(std::string(65536, '\n')));
  TEST_ASSERT_EQUAL_STRING("trop long", otaSigParseVerdictLabel(OtaSigParseVerdict::kTooLong));
}

void test_embedded_nul(void) {
  // NUL juste après un manifeste VALIDE (avant le LF) : un strcpy le tronquerait
  // en un manifeste valide => doit être refusé.
  std::string nulAfterManifest = manifestLine() + std::string(1, '\0') + "\n" + B64_A + "\n";
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kManifest, (int)parse(nulAfterManifest));
  // NUL au milieu de la ligne 1
  std::string nulInside = manifestLine();
  nulInside[5] = '\0';
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kManifest, (int)parse(nulInside + "\n" + B64_A + "\n"));
  // NUL dans la signature (longueur 512 conservée) / NUL final
  std::string sig = B64_A;
  sig[100] = '\0';
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kSignature, (int)parse(manifestLine() + "\n" + sig + "\n"));
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kSignature,
                    (int)parse(manifestLine() + "\n" + B64_A + std::string(1, '\0')));
}

void test_input_not_nul_terminated(void) {
  // parse() alloue la taille EXACTE ; on vérifie en plus qu'un octet SUIVANT
  // valide n'est jamais lu : on passe len=taille utile d'un tampon plus grand
  // dont la suite est un second fichier valide.
  const std::string good = manifestLine() + "\n" + B64_A;
  std::string buffer = good + "\nGARBAGE-QUI-NE-DOIT-PAS-ETRE-LU";
  OtaSigRelayPayload p;
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk,
                    (int)parseOtaSigFile(buffer.data(), good.size(), &p));
  TEST_ASSERT_EQUAL_STRING(B64_A.c_str(), p.signatureBase64);
  // len tronquée au milieu de la signature => 'signature', pas de lecture au-delà.
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kSignature,
                    (int)parseOtaSigFile(buffer.data(), good.size() - 1, &p));
}

// ⭐ La charge relayée est la RE-SÉRIALISATION CANONIQUE, pas le brut reçu.
void test_canonical_reserialization_differs_from_raw(void) {
  OtaSigRelayPayload p;
  const std::string raw = manifestLine() + "\r\n" + B64_A + "\r\n";
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kOk, (int)parse(raw, &p));
  // Le brut (tolérable) contient des CR/LF que le canonique n'a pas.
  const std::string rawLine1 = raw.substr(0, raw.find('\n') + 1);  // « …\r\n »
  TEST_ASSERT_TRUE(rawLine1.size() > p.canonicalLen);
  TEST_ASSERT_TRUE(strcmp(rawLine1.c_str(), p.canonical) != 0);
  TEST_ASSERT_EQUAL_UINT32(strlen(p.canonical), p.canonicalLen);
  // Canonique == sérialisation des champs parsés (re-sérialisation, pas copie).
  char again[kOtaManifestMaxLen];
  const size_t n = serializeOtaManifest(p.fields, again, sizeof(again));
  TEST_ASSERT_EQUAL_UINT32(n, p.canonicalLen);
  TEST_ASSERT_EQUAL_STRING(again, p.canonical);
  TEST_ASSERT_NULL(strchr(p.canonical, '\r'));
}

void test_out_untouched_on_failure(void) {
  OtaSigRelayPayload p;
  memset(&p, 0x5A, sizeof(p));
  OtaSigRelayPayload ref;
  memset(&ref, 0x5A, sizeof(ref));
  const std::string bad = sigText("firmware", "2.86.0", HEX_A, B64_A.substr(0, 511));
  TEST_ASSERT_EQUAL((int)OtaSigParseVerdict::kSignature, (int)parse(bad, &p));
  TEST_ASSERT_EQUAL_MEMORY(&ref, &p, sizeof(p));
}

void test_labels_distinct_and_nonnull(void) {
  const OtaSigParseVerdict all[6] = {
      OtaSigParseVerdict::kOk,       OtaSigParseVerdict::kEmpty,    OtaSigParseVerdict::kTooLong,
      OtaSigParseVerdict::kLines,    OtaSigParseVerdict::kManifest, OtaSigParseVerdict::kSignature};
  for (int i = 0; i < 6; ++i) {
    TEST_ASSERT_NOT_NULL(otaSigParseVerdictLabel(all[i]));
    for (int j = i + 1; j < 6; ++j) {
      TEST_ASSERT_TRUE(strcmp(otaSigParseVerdictLabel(all[i]), otaSigParseVerdictLabel(all[j])) != 0);
    }
  }
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_nominal_firmware);
  RUN_TEST(test_nominal_filesystem);
  RUN_TEST(test_crlf_tolerated_and_canonical_has_no_control_chars);
  RUN_TEST(test_crlf_between_lines_only_and_lf_only_and_no_final_lf);
  RUN_TEST(test_js_matrix_lignes);
  RUN_TEST(test_js_matrix_vide);
  RUN_TEST(test_js_matrix_manifeste);
  RUN_TEST(test_js_matrix_signature);
  RUN_TEST(test_size_bound_1kib);
  RUN_TEST(test_embedded_nul);
  RUN_TEST(test_input_not_nul_terminated);
  RUN_TEST(test_canonical_reserialization_differs_from_raw);
  RUN_TEST(test_out_untouched_on_failure);
  RUN_TEST(test_labels_distinct_and_nonnull);
  return UNITY_END();
}
