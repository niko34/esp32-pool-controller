// bug-185 : état vierge / possédé et décisions d'authentification (logique pure, ADR-0017).
// Les modèles de référence sont écrits depuis la RÈGLE (invariant : un droit anonyme
// n'existe que si aucun mot de passe n'a été choisi, et il se limite à en choisir un),
// pas depuis la structure du code.
#include <cstdio>
#include <unity.h>
#include "auth_bootstrap_logic.h"

void setUp(void) {}
void tearDown(void) {}

void test_factory_literal(void) {
  TEST_ASSERT_TRUE(isFactoryPasswordLiteral("admin"));
  TEST_ASSERT_TRUE(isFactoryPasswordLiteral(""));
  TEST_ASSERT_TRUE(isFactoryPasswordLiteral(nullptr));
  TEST_ASSERT_FALSE(isFactoryPasswordLiteral("Admin"));
  TEST_ASSERT_FALSE(isFactoryPasswordLiteral("admin1"));
  TEST_ASSERT_FALSE(isFactoryPasswordLiteral("motdepasse"));
}

// Règle : première mise en route si l'assistant n'est pas fini OU si le mot de passe est celui d'usine.
void test_compute_first_boot(void) {
  for (int w = 0; w < 2; w++)
    for (int f = 0; f < 2; f++) {
      bool expected = (w == 0) || (f == 1);
      TEST_ASSERT_EQUAL(expected, computeFirstBoot(w, f));
    }
  // Cas migration : drapeau posé par un tiers + mot de passe d'usine => assistant rouvert.
  TEST_ASSERT_TRUE(computeFirstBoot(true, true));
}

// Règle : identifiants faux -> refus ; mot de passe d'usine -> jamais de jeton.
// ⛔ ET RIEN D'AUTRE : l'état de l'assistant n'entre PAS dans cette décision.
void test_login_all_combinations(void) {
  for (int b = 0; b < 8; b++) {
    bool u = b & 1, p = b & 2, f = b & 4;
    LoginVerdict exp;
    if (!u || !p) exp = LoginVerdict::kDenyCredentials;
    else if (f) exp = LoginVerdict::kDenyMustChangePassword;
    else exp = LoginVerdict::kAllow;
    char msg[24];
    snprintf(msg, sizeof msg, "login %d", b);
    TEST_ASSERT_EQUAL_MESSAGE((int)exp, (int)decideLogin(u, p, f), msg);
  }
}

// admin/admin ne rend JAMAIS un jeton.
void test_login_factory_never_allows(void) {
  TEST_ASSERT_EQUAL((int)LoginVerdict::kDenyMustChangePassword, (int)decideLogin(true, true, true));
}

// ⭐ NON-RÉGRESSION CRITIQUE — l'appareil dont l'assistant a été INTERROMPU après
// le choix du mot de passe : son propriétaire connaît son mot de passe et DOIT
// pouvoir se connecter. ⛔ L'OTA est le seul canal de déploiement : le refuser
// ici enfermerait l'appareil dehors définitivement.
void test_login_works_when_wizard_never_finished(void) {
  TEST_ASSERT_EQUAL((int)LoginVerdict::kAllow, (int)decideLogin(true, true, /*factoryPassword=*/false));
}

static PwdChangeInput mk(int b) {
  PwdChangeInput in;
  in.virginState = b & 1;
  in.hasCurrentPassword = b & 2;
  in.currentMatches = b & 4;
  in.currentIsFactoryLiteral = b & 8;
  in.tokenValid = b & 16;
  in.newLenOk = b & 32;
  in.newEqualsCurrent = b & 64;
  return in;
}

// Modèle depuis la règle : (1) identité, (2) longueur, (3) différence.
static PwdChangeVerdict model(const PwdChangeInput& i) {
  bool identityOk;
  bool badCurrent = false;
  if (i.virginState) {
    identityOk = true;  // droit anonyme, limité à choisir un mot de passe
    if (i.hasCurrentPassword && !i.currentIsFactoryLiteral) badCurrent = true;
  } else if (i.hasCurrentPassword) {
    identityOk = i.currentMatches;
    badCurrent = !i.currentMatches;
  } else {
    identityOk = i.tokenValid;
  }
  if (!identityOk) return badCurrent ? PwdChangeVerdict::kDenyBadCurrent : PwdChangeVerdict::kDenyUnauth;
  if (badCurrent) return PwdChangeVerdict::kDenyBadCurrent;
  if (!i.newLenOk) return PwdChangeVerdict::kDenyTooShort;
  if (i.hasCurrentPassword && i.newEqualsCurrent) return PwdChangeVerdict::kDenySame;
  return PwdChangeVerdict::kAllow;
}

void test_change_password_128_combinations(void) {
  int allow = 0;
  for (int b = 0; b < 128; b++) {
    PwdChangeInput in = mk(b);
    char msg[24];
    snprintf(msg, sizeof msg, "chg %d", b);
    PwdChangeVerdict v = decidePasswordChange(in);
    TEST_ASSERT_EQUAL_MESSAGE((int)model(in), (int)v, msg);
    if (v == PwdChangeVerdict::kAllow) allow++;
  }
  // ⛔ COMPTE FIGÉ, pas « > 0 » : un simple « au moins un » ne verrait pas un
  // ÉLARGISSEMENT du droit. 32 = (vierge : 16 sans currentPassword + 4 avec
  // "admin" et un nouveau différent) + (possédé : 8 par jeton + 4 par
  // currentPassword correct). Recalculé à la main par code-reviewer.
  TEST_ASSERT_EQUAL(32, allow);
}

// M7 : un anonyme (ni état vierge, ni mot de passe, ni jeton) reçoit 401 MÊME avec
// un nouveau mot de passe trop court — sinon 400 fuirait l'existence du compte.
void test_authentication_before_length(void) {
  PwdChangeInput in;  // tout false : pas vierge, aucun courant, pas de jeton, longueur KO
  TEST_ASSERT_EQUAL((int)PwdChangeVerdict::kDenyUnauth, (int)decidePasswordChange(in));
  in.hasCurrentPassword = true;  // mauvais courant + trop court
  TEST_ASSERT_EQUAL((int)PwdChangeVerdict::kDenyBadCurrent, (int)decidePasswordChange(in));
}

void test_anonymous_when_pristine_only(void) {
  PwdChangeInput in;
  in.virginState = true;
  in.newLenOk = true;
  TEST_ASSERT_EQUAL((int)PwdChangeVerdict::kAllow, (int)decidePasswordChange(in));
  in.virginState = false;  // possédé : plus de droit anonyme
  TEST_ASSERT_EQUAL((int)PwdChangeVerdict::kDenyUnauth, (int)decidePasswordChange(in));
}

// M1 / M2 : le jeton tourne TOUJOURS et le nouveau est rendu ; WS révoqués.
void test_effect_plan(void) {
  for (int fb = 0; fb < 2; fb++) {
    AuthEffectPlan p = planAfterPasswordChange(fb);
    TEST_ASSERT_TRUE(p.rotateToken);
    TEST_ASSERT_TRUE(p.returnRotatedToken);
    TEST_ASSERT_TRUE(p.revokeWebsockets);
    TEST_ASSERT_EQUAL((bool)fb, p.closeFirstBootWindow);
  }
}

// Modèle : sans jeton -> 401 (quoi qu'il arrive d'autre) ; mot de passe d'usine -> 409 ;
// sinon ferme seulement si la fenêtre est ouverte.
void test_wizard_close_all_combinations(void) {
  for (int b = 0; b < 8; b++) {
    bool fb = b & 1, tok = b & 2, f = b & 4;
    WizardCloseVerdict exp;
    if (!tok) exp = WizardCloseVerdict::kDenyUnauth;
    else if (f) exp = WizardCloseVerdict::kDenyFactoryPassword;
    else exp = fb ? WizardCloseVerdict::kClose : WizardCloseVerdict::kAlreadyClosed;
    TEST_ASSERT_EQUAL((int)exp, (int)decideWizardClose(fb, tok, f));
  }
}

// M3 / M4 : un tiers sans jeton, ou sur un appareil encore en mot de passe d'usine, ne ferme rien.
void test_wizard_close_third_party_cannot_close(void) {
  TEST_ASSERT_EQUAL((int)WizardCloseVerdict::kDenyUnauth, (int)decideWizardClose(true, false, false));
  TEST_ASSERT_EQUAL((int)WizardCloseVerdict::kDenyUnauth, (int)decideWizardClose(true, false, true));
  TEST_ASSERT_EQUAL((int)WizardCloseVerdict::kDenyFactoryPassword, (int)decideWizardClose(true, true, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_factory_literal);
  RUN_TEST(test_compute_first_boot);
  RUN_TEST(test_login_all_combinations);
  RUN_TEST(test_login_factory_never_allows);
  RUN_TEST(test_login_works_when_wizard_never_finished);
  RUN_TEST(test_change_password_128_combinations);
  RUN_TEST(test_authentication_before_length);
  RUN_TEST(test_anonymous_when_pristine_only);
  RUN_TEST(test_effect_plan);
  RUN_TEST(test_wizard_close_all_combinations);
  RUN_TEST(test_wizard_close_third_party_cannot_close);
  return UNITY_END();
}
