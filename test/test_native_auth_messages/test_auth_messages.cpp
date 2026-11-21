// bug-200 (M3) — les PHRASES des refus d'authentification, pas les verdicts
// (ceux-ci ont leurs propres suites). Module pur auth_messages.h (ADR-0017).
// Ce que protege ce fichier : l'utilisateur lit une phrase en francais qui dit
// QUEL mot de passe est en cause (appareil ou point d'acces).
#include <unity.h>
#include <string.h>
#include <ctype.h>
#include <initializer_list>

#include "auth_messages.h"

void setUp(void) {}
void tearDown(void) {}

static const ApPwdVerdict kApRefusals[] = {ApPwdVerdict::kTooShort, ApPwdVerdict::kTooLong,
                                           ApPwdVerdict::kBadCharacter, ApPwdVerdict::kMissingFamily};

void test_ap_phrases_exactes(void) {
  TEST_ASSERT_EQUAL_STRING("", apPwdMessage(ApPwdVerdict::kOk));
  TEST_ASSERT_EQUAL_STRING("Le mot de passe du point d'acces doit faire au moins 12 caracteres",
                           apPwdMessage(ApPwdVerdict::kTooShort));
  TEST_ASSERT_EQUAL_STRING("Le mot de passe du point d'acces ne peut pas depasser 63 caracteres",
                           apPwdMessage(ApPwdVerdict::kTooLong));
  TEST_ASSERT_EQUAL_STRING(
      "Le mot de passe du point d'acces n'accepte ni accent, ni caractere invisible, ni espace en debut ou en fin",
      apPwdMessage(ApPwdVerdict::kBadCharacter));
  TEST_ASSERT_EQUAL_STRING(
      "Le mot de passe du point d'acces doit contenir au moins une minuscule, une majuscule, un chiffre et un caractere special",
      apPwdMessage(ApPwdVerdict::kMissingFamily));
}

void test_change_phrases_exactes(void) {
  const ApPwdVerdict any = ApPwdVerdict::kOk;
  TEST_ASSERT_EQUAL_STRING("", pwdChangeMessage(PwdChangeVerdict::kAllow, any));
  TEST_ASSERT_EQUAL_STRING("Authentification requise", pwdChangeMessage(PwdChangeVerdict::kDenyUnauth, any));
  TEST_ASSERT_EQUAL_STRING("Mot de passe actuel incorrect", pwdChangeMessage(PwdChangeVerdict::kDenyBadCurrent, any));
  TEST_ASSERT_EQUAL_STRING("Le nouveau mot de passe doit faire au moins 8 caracteres",
                           pwdChangeMessage(PwdChangeVerdict::kDenyTooShort, any));
  TEST_ASSERT_EQUAL_STRING("Le nouveau mot de passe doit etre different de l'actuel",
                           pwdChangeMessage(PwdChangeVerdict::kDenySame, any));
  TEST_ASSERT_EQUAL_STRING("Le mot de passe ne peut pas depasser 128 caracteres",
                           pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kTooLong));
  TEST_ASSERT_EQUAL_STRING("Le mot de passe n'accepte ni caractere invisible, ni espace en debut ou en fin",
                           pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kBadCharacter));
  TEST_ASSERT_EQUAL_STRING(
      "Le mot de passe doit contenir au moins une minuscule, une majuscule, un chiffre et un caractere special",
      pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kMissingFamily));
}

void test_les_quatre_refus_du_point_d_acces_le_nomment(void) {
  // Les quatre refus reels (kOk n'en est pas un) ; le cinquieme message du lot
  // est l'echec du 'default' (verdict inconnu), couvert ci-dessous.
  for (ApPwdVerdict v : kApRefusals) {
    TEST_ASSERT_NOT_NULL(strstr(apPwdMessage(v), "point d'acces"));
  }
  TEST_ASSERT_NOT_NULL(strstr(apPwdMessage(static_cast<ApPwdVerdict>(99)), "point d'acces"));
}

void test_aucun_refus_de_l_appareil_ne_nomme_le_point_d_acces(void) {
  const PwdChangeVerdict vs[] = {PwdChangeVerdict::kDenyUnauth, PwdChangeVerdict::kDenyBadCurrent,
                                 PwdChangeVerdict::kDenyTooShort, PwdChangeVerdict::kDenySame};
  for (PwdChangeVerdict v : vs) TEST_ASSERT_NULL(strstr(pwdChangeMessage(v, ApPwdVerdict::kOk), "point d'acces"));
  for (ApPwdVerdict a : kApRefusals)
    TEST_ASSERT_NULL(strstr(pwdChangeMessage(PwdChangeVerdict::kDenyWeak, a), "point d'acces"));
}

// Avant bug-200 les deux phrases « famille manquante » etaient mot pour mot
// identiques : sur un ecran ou les deux objets coexistent, on ne savait pas
// lequel etait reproche.
void test_famille_manquante_differe_entre_les_deux_objets(void) {
  TEST_ASSERT_NOT_EQUAL(0, strcmp(apPwdMessage(ApPwdVerdict::kMissingFamily),
                                  pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kMissingFamily)));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(apPwdMessage(ApPwdVerdict::kTooLong),
                                  pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kTooLong)));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(apPwdMessage(ApPwdVerdict::kBadCharacter),
                                  pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kBadCharacter)));
}

static bool contientMotInsensible(const char* hay, const char* mot) {
  const size_t n = strlen(mot);
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] && tolower((unsigned char)p[i]) == mot[i]) ++i;
    if (i == n) return true;
  }
  return false;
}

void test_aucun_mot_anglais_dans_la_table(void) {
  static const char* kAnglais[] = {"must", "password", "characters", "at least", "current", "different from", "same as",
                                   "required", "incorrect password", "invalid", "too short", "too long",
                                   "should", "cannot", "special character", "lowercase", "uppercase"};
  std::initializer_list<const char*> all = {
      apPwdMessage(ApPwdVerdict::kTooShort), apPwdMessage(ApPwdVerdict::kTooLong),
      apPwdMessage(ApPwdVerdict::kBadCharacter), apPwdMessage(ApPwdVerdict::kMissingFamily),
      pwdChangeMessage(PwdChangeVerdict::kDenyUnauth, ApPwdVerdict::kOk),
      pwdChangeMessage(PwdChangeVerdict::kDenyBadCurrent, ApPwdVerdict::kOk),
      pwdChangeMessage(PwdChangeVerdict::kDenyTooShort, ApPwdVerdict::kOk),
      pwdChangeMessage(PwdChangeVerdict::kDenySame, ApPwdVerdict::kOk),
      pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kTooLong),
      pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kBadCharacter),
      pwdChangeMessage(PwdChangeVerdict::kDenyWeak, ApPwdVerdict::kMissingFamily)};
  for (const char* m : all)
    for (const char* mot : kAnglais) {
      // « different » seul est ambigu (« different de » = français sans accent) : on cherche les tournures anglaises.
      TEST_ASSERT_FALSE_MESSAGE(contientMotInsensible(m, mot), m);
    }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ap_phrases_exactes);
  RUN_TEST(test_change_phrases_exactes);
  RUN_TEST(test_les_quatre_refus_du_point_d_acces_le_nomment);
  RUN_TEST(test_aucun_refus_de_l_appareil_ne_nomme_le_point_d_acces);
  RUN_TEST(test_famille_manquante_differe_entre_les_deux_objets);
  RUN_TEST(test_aucun_mot_anglais_dans_la_table);
  return UNITY_END();
}
