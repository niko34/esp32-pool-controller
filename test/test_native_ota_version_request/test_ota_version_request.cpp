// feature-151 D3-b2 : liaison genre + version, logique pure. Le résultat n'est que journalisé.
#include <unity.h>
#include <string.h>
#include "ota_version_request_logic.h"

void setUp(void) {}
void tearDown(void) {}

static bool parse(const char* t, uint32_t* a = nullptr, uint32_t* b = nullptr, uint32_t* c = nullptr) {
  uint32_t x = 7, y = 7, z = 7;
  bool ok = otaParseVersionTag(t, &x, &y, &z);
  if (!ok) { TEST_ASSERT_EQUAL_UINT32(7, x); TEST_ASSERT_EQUAL_UINT32(7, y); TEST_ASSERT_EQUAL_UINT32(7, z); }
  if (a) *a = x; if (b) *b = y; if (c) *c = z;
  return ok;
}

void test_v_prefixe(void) { uint32_t a, b, c; TEST_ASSERT_TRUE(parse("v2.89.0", &a, &b, &c)); TEST_ASSERT_EQUAL_UINT32(2, a); TEST_ASSERT_EQUAL_UINT32(89, b); TEST_ASSERT_EQUAL_UINT32(0, c); }
void test_sans_prefixe(void) { uint32_t a, b, c; TEST_ASSERT_TRUE(parse("2.89.0", &a, &b, &c)); TEST_ASSERT_EQUAL_UINT32(89, b); }
void test_V_majuscule_refuse(void) { TEST_ASSERT_FALSE(parse("V2.89.0")); }
void test_deux_composantes_refuse(void) { TEST_ASSERT_FALSE(parse("2.89")); }
void test_suffixe_rc_refuse(void) { TEST_ASSERT_FALSE(parse("2.89.0-rc1")); }
void test_vide_refuse(void) { TEST_ASSERT_FALSE(parse("")); TEST_ASSERT_FALSE(parse("v")); TEST_ASSERT_FALSE(parse(nullptr)); }
void test_tag_64_octets_refuse(void) {
  char t[65]; memset(t, '1', 64); t[64] = 0;
  TEST_ASSERT_FALSE(parse(t));
  char u[65]; memset(u, 'a', 64); u[64] = 0; memcpy(u, "v2.89.0", 7);
  TEST_ASSERT_FALSE(parse(u));
}
void test_zeros_de_tete_acceptes_et_equivalents(void) {
  uint32_t a, b, c; TEST_ASSERT_TRUE(parse("v02.089.000", &a, &b, &c));
  TEST_ASSERT_EQUAL_UINT32(2, a); TEST_ASSERT_EQUAL_UINT32(89, b); TEST_ASSERT_EQUAL_UINT32(0, c);
}
void test_borne_6_7_chiffres(void) { TEST_ASSERT_TRUE(parse("999999.0.0")); TEST_ASSERT_FALSE(parse("1000000.0.0")); }
void test_signes_espaces_points_parasites_refuses(void) {
  TEST_ASSERT_FALSE(parse("+2.89.0")); TEST_ASSERT_FALSE(parse("-2.89.0")); TEST_ASSERT_FALSE(parse(" 2.89.0"));
  TEST_ASSERT_FALSE(parse("2.89.0 ")); TEST_ASSERT_FALSE(parse("2..0")); TEST_ASSERT_FALSE(parse("2.89.0.1")); TEST_ASSERT_FALSE(parse("2.89.0."));
  TEST_ASSERT_FALSE(parse("vv2.89.0"));
}

void test_accord(void) {
  OtaParsedVersion r{}; 
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kAgree, (int)otaCompareRequestedVersion(true, "v2.89.0", true, 2, 89, 0, &r));
  TEST_ASSERT_EQUAL_UINT32(89, r.minor);
}
void test_accord_malgre_prefixe_et_zeros(void) {
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kAgree, (int)otaCompareRequestedVersion(true, "02.089.00", true, 2, 89, 0, nullptr));
}
void test_desaccord_sur_chaque_composante(void) {
  OtaParsedVersion r{};
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kDisagree, (int)otaCompareRequestedVersion(true, "v2.89.1", true, 2, 89, 0, &r));
  TEST_ASSERT_EQUAL_UINT32(1, r.patch);
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kDisagree, (int)otaCompareRequestedVersion(true, "v2.90.0", true, 2, 89, 0, nullptr));
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kDisagree, (int)otaCompareRequestedVersion(true, "v3.89.0", true, 2, 89, 0, nullptr));
}
void test_reference_absente(void) {
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kNoReference, (int)otaCompareRequestedVersion(false, nullptr, true, 2, 89, 0, nullptr));
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kNoReference, (int)otaCompareRequestedVersion(false, "v9.9.9", true, 2, 89, 0, nullptr));
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kNoReference, (int)otaCompareRequestedVersion(true, nullptr, true, 2, 89, 0, nullptr));
}
void test_manifeste_invalide_sans_comparaison(void) {
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kNoReference, (int)otaCompareRequestedVersion(true, "v9.9.9", false, 2, 89, 0, nullptr));
}
void test_reference_inparsable_et_refOut_intact(void) {
  OtaParsedVersion r{5, 5, 5};
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kReferenceUnparsable, (int)otaCompareRequestedVersion(true, "2.89.0-rc1", true, 2, 89, 0, &r));
  TEST_ASSERT_EQUAL_UINT32(5, r.major);
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kReferenceUnparsable, (int)otaCompareRequestedVersion(true, "", true, 2, 89, 0, nullptr));
}

void test_selection_le_tag_memorise_prime_toujours(void) {
  bool fp = true;
  const char* r = otaSelectVersionReference("v2.89.0", "v9.9.9", &fp);
  TEST_ASSERT_EQUAL_STRING("v2.89.0", r); TEST_ASSERT_FALSE(fp);
}
void test_selection_parametre_sans_tag(void) {
  bool fp = false;
  TEST_ASSERT_EQUAL_STRING("v1.2.3", otaSelectVersionReference(nullptr, "v1.2.3", &fp)); TEST_ASSERT_TRUE(fp);
  fp = false;
  TEST_ASSERT_EQUAL_STRING("v1.2.3", otaSelectVersionReference("", "v1.2.3", &fp)); TEST_ASSERT_TRUE(fp);
}
void test_selection_rien(void) {
  bool fp = true;
  TEST_ASSERT_NULL(otaSelectVersionReference(nullptr, nullptr, &fp)); TEST_ASSERT_FALSE(fp);
  TEST_ASSERT_NULL(otaSelectVersionReference("", "", nullptr));
}

void test_format_reserialise_depuis_entiers(void) {
  char o[24]; otaFormatVersion(o, sizeof o, 2, 89, 0); TEST_ASSERT_EQUAL_STRING("2.89.0", o);
  char small[4]; otaFormatVersion(small, sizeof small, 123456, 123456, 123456);
  TEST_ASSERT_EQUAL(3, (int)strlen(small));  // tronqué, NUL-terminé
  otaFormatVersion(nullptr, 5, 1, 2, 3); otaFormatVersion(o, 0, 1, 2, 3);
}
void test_format_ne_peut_pas_porter_de_texte_brut(void) {
  // Un tag hostile n'arrive jamais au format : seuls des entiers parsés y entrent.
  OtaParsedVersion r{};
  TEST_ASSERT_EQUAL((int)OtaVersionAgreement::kReferenceUnparsable,
                    (int)otaCompareRequestedVersion(true, "2.89.0\"}\n[forgé]", true, 2, 89, 0, &r));
  TEST_ASSERT_EQUAL_UINT32(0, r.major);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_v_prefixe); RUN_TEST(test_sans_prefixe); RUN_TEST(test_V_majuscule_refuse);
  RUN_TEST(test_deux_composantes_refuse); RUN_TEST(test_suffixe_rc_refuse); RUN_TEST(test_vide_refuse);
  RUN_TEST(test_tag_64_octets_refuse); RUN_TEST(test_zeros_de_tete_acceptes_et_equivalents);
  RUN_TEST(test_borne_6_7_chiffres); RUN_TEST(test_signes_espaces_points_parasites_refuses);
  RUN_TEST(test_accord); RUN_TEST(test_accord_malgre_prefixe_et_zeros); RUN_TEST(test_desaccord_sur_chaque_composante);
  RUN_TEST(test_reference_absente); RUN_TEST(test_manifeste_invalide_sans_comparaison);
  RUN_TEST(test_reference_inparsable_et_refOut_intact);
  RUN_TEST(test_selection_le_tag_memorise_prime_toujours); RUN_TEST(test_selection_parametre_sans_tag);
  RUN_TEST(test_selection_rien); RUN_TEST(test_format_reserialise_depuis_entiers);
  RUN_TEST(test_format_ne_peut_pas_porter_de_texte_brut);
  return UNITY_END();
}
