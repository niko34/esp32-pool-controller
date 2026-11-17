// feature-151 D3-c — décision de refus et phrases (ADR-0017, module pur).
// Les tests décrivent le COMPORTEMENT : table verdict -> {refus, HTTP, code}, phrases par site,
// et surtout la séparation décision / phrase (le marqueur `ui` ne change JAMAIS la décision).
#include <unity.h>
#include <initializer_list>
#include <string.h>

#include "ota_refusal_messages.h"

void setUp(void) {}
void tearDown(void) {}

typedef OtaAuthenticityVerdict V;
static const V kAll[] = {V::kAbsent, V::kMalformed, V::kVerificationImpossible, V::kInvalid,
                         V::kKindMismatch, V::kBelowFloor, V::kValid};
static const OtaRefusalSite kSites[] = {OtaRefusalSite::kEarly, OtaRefusalSite::kLateFirmware,
                                        OtaRefusalSite::kLateFilesystem};

static void expectDecision(V v, bool rej, int http, const char* code) {
  const OtaRefusalDecision d = otaRefusalDecide(v);
  TEST_ASSERT_EQUAL(rej, d.rejected);
  TEST_ASSERT_EQUAL_INT(http, d.http);
  TEST_ASSERT_EQUAL_STRING(code, d.code);
}

void test_table_decision_complete(void) {
  expectDecision(V::kAbsent, true, 403, "sig_absente");
  expectDecision(V::kMalformed, true, 400, "sig_malformee");
  expectDecision(V::kInvalid, true, 403, "sig_invalide");
  expectDecision(V::kVerificationImpossible, true, 503, "sig_verif_impossible");
  expectDecision(V::kKindMismatch, true, 415, "sig_genre_discordant");
  expectDecision(V::kBelowFloor, true, 403, "sig_sous_plancher");
  expectDecision(V::kValid, false, 0, "");
}

void test_seul_kvalid_est_accepte(void) {
  for (V v : kAll) TEST_ASSERT_EQUAL((v != V::kValid), otaRefusalDecide(v).rejected);
}

void test_409_reserve_a_l_envoi_concurrent(void) {
  for (V v : kAll) TEST_ASSERT_NOT_EQUAL(409, otaRefusalDecide(v).http);
}

// La garde qui compte : `pageAnnouncedVersion` ne change QUE la phrase (et seulement pour kAbsent).
void test_marqueur_ui_ne_change_que_la_phrase(void) {
  for (V v : kAll) {
    for (OtaRefusalSite s : kSites) {
      const OtaRefusalDecision a = otaRefusalDecide(v);  // la décision n'a pas de paramètre de page
      const OtaRefusalDecision b = otaRefusalDecide(v);
      TEST_ASSERT_EQUAL(a.rejected, b.rejected);
      TEST_ASSERT_EQUAL_INT(a.http, b.http);
      TEST_ASSERT_EQUAL_STRING(a.code, b.code);
      const char* p1 = otaRefusalPhrase(v, true, s);
      const char* p0 = otaRefusalPhrase(v, false, s);
      if (v == V::kAbsent && s == OtaRefusalSite::kEarly) {
        TEST_ASSERT_TRUE(strcmp(p1, p0) != 0);
      } else {
        TEST_ASSERT_EQUAL_STRING(p1, p0);
      }
    }
  }
}

void test_phrases_kabsent_selon_ui(void) {
  const char* withUi = otaRefusalPhrase(V::kAbsent, true, OtaRefusalSite::kEarly);
  const char* noUi = otaRefusalPhrase(V::kAbsent, false, OtaRefusalSite::kEarly);
  TEST_ASSERT_NOT_NULL(strstr(withUi, "Signature manquante"));
  TEST_ASSERT_NOT_NULL(strstr(noUi, "page est périmée"));
}

void test_phrases_precoces_distinctes_et_rien_modifie(void) {
  const V refus[] = {V::kMalformed, V::kInvalid, V::kVerificationImpossible, V::kKindMismatch};
  const char* seen[8];
  int n = 0;
  seen[n++] = otaRefusalPhrase(V::kAbsent, true, OtaRefusalSite::kEarly);
  seen[n++] = otaRefusalPhrase(V::kAbsent, false, OtaRefusalSite::kEarly);
  for (V v : refus) seen[n++] = otaRefusalPhrase(v, true, OtaRefusalSite::kEarly);
  for (int i = 0; i < n; ++i) {
    TEST_ASSERT_NOT_NULL(strstr(seen[i], "Rien n'a été modifié."));
    for (int j = i + 1; j < n; ++j) TEST_ASSERT_TRUE(strcmp(seen[i], seen[j]) != 0);
  }
}

// Site tardif : l'écriture a commencé, ne JAMAIS dire « Rien n'a été modifié », verdict ignoré.
void test_sites_tardifs(void) {
  for (V v : kAll) {
    for (bool ui : {false, true}) {
      const char* fw = otaRefusalPhrase(v, ui, OtaRefusalSite::kLateFirmware);
      const char* fs = otaRefusalPhrase(v, ui, OtaRefusalSite::kLateFilesystem);
      TEST_ASSERT_NULL(strstr(fw, "Rien n'a été modifié"));
      TEST_ASSERT_NULL(strstr(fs, "Rien n'a été modifié"));
      TEST_ASSERT_NOT_NULL(strstr(fw, "image de secours"));
      TEST_ASSERT_NOT_NULL(strstr(fs, "interface a été effacée"));
      TEST_ASSERT_EQUAL_STRING(otaRefusalPhrase(V::kAbsent, true, OtaRefusalSite::kLateFirmware), fw);
    }
  }
}

// Nombre de CARACTÈRES UTF-8 (octets de tête, hors continuation 10xxxxxx).
static size_t utf8Chars(const char* s) {
  size_t n = 0;
  for (; *s; ++s) if (((unsigned char)*s & 0xC0) != 0x80) ++n;
  return n;
}

// Deux limites DISTINCTES : data/app.js tronque à 200 CARACTÈRES ; un tampon C++ compte des OCTETS
// (un char[200] tronquerait la phrase 8, 199 octets + NUL).
void test_phrases_bornees_caracteres_et_octets(void) {
  size_t maxChars = 0, maxBytes = 0;
  for (V v : kAll) {
    for (OtaRefusalSite s : kSites) {
      for (bool ui : {false, true}) {
        const char* p = otaRefusalPhrase(v, ui, s);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_TRUE(strlen(p) > 0);
        TEST_ASSERT_TRUE_MESSAGE(utf8Chars(p) <= 200, "phrase > 200 caracteres : tronquee par app.js");
        TEST_ASSERT_TRUE_MESSAGE(strlen(p) <= 255, "phrase > 255 octets : depasse le tampon de 256");
        if (utf8Chars(p) > maxChars) maxChars = utf8Chars(p);
        if (strlen(p) > maxBytes) maxBytes = strlen(p);
      }
    }
  }
  // Garde aveugle : la pire phrase mesurée est bien longue (190 car. / 199 octets).
  TEST_ASSERT_TRUE(maxChars >= 150);
  // Piège à un octet près : en octets, la pire phrase dépasse 190 (accents sur 2 octets).
  TEST_ASSERT_TRUE(maxBytes > maxChars);
}

// kBelowFloor reste inatteignable en production (plancher 0.0.0) : la phrase ne fait pas de
// promesse « rien modifié » trompeuse, repli générique.
void test_below_floor_repli_generique(void) {
  const char* p = otaRefusalPhrase(V::kBelowFloor, true, OtaRefusalSite::kEarly);
  TEST_ASSERT_NOT_NULL(strstr(p, "refusée"));
}

// Non-regression (decision utilisateur) : avec le plancher fige a 0.0.0, une version ANTERIEURE
// signee reste installable : le verdict est kValid, donc aucun refus.
void test_version_anterieure_signee_reste_installable(void) {
  const V v = combineOtaAuthenticityVerdict(true, true, OtaManifestParseVerdict::kOk, true,
                                            OtaCryptoVerdict::kValid, OtaManifestKind::kFirmware,
                                            OtaManifestKind::kFirmware, 0, 0, 1, 0, 0, 0);
  TEST_ASSERT_EQUAL(V::kValid, v);
  TEST_ASSERT_FALSE(otaRefusalDecide(v).rejected);
  const V old = combineOtaAuthenticityVerdict(true, true, OtaManifestParseVerdict::kOk, true,
                                              OtaCryptoVerdict::kValid, OtaManifestKind::kFirmware,
                                              OtaManifestKind::kFirmware, 2, 0, 0, 0, 0, 0);
  TEST_ASSERT_EQUAL(V::kValid, old);
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_table_decision_complete);
  RUN_TEST(test_seul_kvalid_est_accepte);
  RUN_TEST(test_409_reserve_a_l_envoi_concurrent);
  RUN_TEST(test_marqueur_ui_ne_change_que_la_phrase);
  RUN_TEST(test_phrases_kabsent_selon_ui);
  RUN_TEST(test_phrases_precoces_distinctes_et_rien_modifie);
  RUN_TEST(test_sites_tardifs);
  RUN_TEST(test_phrases_bornees_caracteres_et_octets);
  RUN_TEST(test_below_floor_repli_generique);
  RUN_TEST(test_version_anterieure_signee_reste_installable);
  return UNITY_END();
}
