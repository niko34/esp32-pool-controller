// bug-184 : décision d'admission d'une requête (logique pure, ADR-0017).
#include <cstdio>
#include <unity.h>
#include "auth_admission_logic.h"

void setUp(void) {}
void tearDown(void) {}

static AuthAdmissionInput mk(int bits) {
  AuthAdmissionInput in;
  in.authEnabled = bits & 1;
  in.publicRoute = bits & 2;
  in.firstBoot = bits & 4;
  in.isSaveConfigUrl = bits & 8;
  in.rateLimitOk = bits & 16;
  in.tokenValid = bits & 32;
  return in;
}

// Modèle de référence écrit à partir de la règle métier (pas de la structure du code).
static AuthVerdict expected(const AuthAdmissionInput& i) {
  if (!i.authEnabled || i.publicRoute) return AuthVerdict::kAllow;
  if (!i.rateLimitOk) return AuthVerdict::kDenyRateLimited;
  if (i.firstBoot && i.isSaveConfigUrl) return AuthVerdict::kAllow;
  return i.tokenValid ? AuthVerdict::kAllow : AuthVerdict::kDenyUnauth;
}

// C1
void test_c1_all_64_combinations(void) {
  int allow = 0;
  for (int b = 0; b < 64; b++) {
    AuthAdmissionInput in = mk(b);
    AuthVerdict v = decideAdmission(in);
    char msg[32];
    snprintf(msg, sizeof msg, "combinaison %d", b);
    TEST_ASSERT_EQUAL_MESSAGE((int)expected(in), (int)v, msg);
    if (v == AuthVerdict::kAllow) allow++;
  }
  // 32 (auth off) + 16 (auth on, route publique) + 5 (auth on, quota ok, exemption ou jeton).
  TEST_ASSERT_EQUAL(53, allow);
}

// C2
void test_c2_auth_disabled_allows_everything(void) {
  for (int b = 0; b < 64; b++) {
    AuthAdmissionInput in = mk(b);
    in.authEnabled = false;
    TEST_ASSERT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(in));
  }
}

// C3 : la route publique échappe au quota (constat : ordre imposé par le module).
void test_c3_public_route_allowed_even_when_rate_limited(void) {
  AuthAdmissionInput in = mk(1 | 2);  // auth on, publique, quota dépassé, pas de jeton
  TEST_ASSERT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(in));
}

// C4
void test_c4_rate_limited_never_allows(void) {
  for (int b = 0; b < 64; b++) {
    AuthAdmissionInput in = mk(b);
    if (!in.authEnabled || in.publicRoute) continue;
    in.rateLimitOk = false;
    TEST_ASSERT_EQUAL((int)AuthVerdict::kDenyRateLimited, (int)decideAdmission(in));
  }
}

// C5
void test_c5_wizard_exemption_needs_both_conditions(void) {
  AuthAdmissionInput in = mk(1 | 16 | 4 | 8);  // sans jeton
  TEST_ASSERT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(in));
  in = mk(1 | 16 | 8);  // save-config sans firstBoot
  TEST_ASSERT_EQUAL((int)AuthVerdict::kDenyUnauth, (int)decideAdmission(in));
  in = mk(1 | 16 | 4);  // firstBoot sans save-config
  TEST_ASSERT_EQUAL((int)AuthVerdict::kDenyUnauth, (int)decideAdmission(in));
}

// C6
void test_c6_token(void) {
  TEST_ASSERT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(mk(1 | 16 | 32)));
  TEST_ASSERT_EQUAL((int)AuthVerdict::kDenyUnauth, (int)decideAdmission(mk(1 | 16)));
}

// C7
void test_c7_nothing_true_denies(void) {
  AuthAdmissionInput in = mk(0);
  in.authEnabled = true;  // auth off serait un mode assumé ; ici « rien d'autre n'est vrai »
  TEST_ASSERT_NOT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(in));
  // Tous booléens à false, authEnabled compris : mode sans authentification, assumé.
  TEST_ASSERT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(mk(0)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_c1_all_64_combinations);
  RUN_TEST(test_c2_auth_disabled_allows_everything);
  RUN_TEST(test_c3_public_route_allowed_even_when_rate_limited);
  RUN_TEST(test_c4_rate_limited_never_allows);
  RUN_TEST(test_c5_wizard_exemption_needs_both_conditions);
  RUN_TEST(test_c6_token);
  RUN_TEST(test_c7_nothing_true_denies);
  return UNITY_END();
}
