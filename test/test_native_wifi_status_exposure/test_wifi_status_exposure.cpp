// bug-181 A2 : qui voit le SSID dans GET /wifi/status.
// RÈGLE MÉTIER (modèle de référence, écrit hors du code testé) : le SSID n'est
// montré qu'à qui a le droit : authentification coupée par l'utilisateur, jeton
// valide, ou appareil en mode point d'accès (AP=2 ou AP+STA=3).
#include <unity.h>
#include "wifi_status_exposure_logic.h"

void setUp(void) {}
void tearDown(void) {}

static WifiSsidExposureInput mk(bool auth, bool tok, int mode, bool sta) {
  WifiSsidExposureInput in;
  in.authEnabled = auth;
  in.tokenValid = tok;
  in.wifiMode = mode;
  in.staConnected = sta;
  return in;
}

// Modèle de référence, depuis la règle métier (valeurs wifi_mode_t littérales).
static bool modele(bool auth, bool tok, int mode) {
  const bool pointAcces = (mode == 2) || (mode == 3);
  return !auth || tok || pointAcces;
}

void test_auth_coupee_expose_toujours(void) {
  const int modes[] = {0, 1, 2, 3, 4, 99, -1};
  for (int m : modes)
    for (int t = 0; t < 2; ++t)
      for (int s = 0; s < 2; ++s)
        TEST_ASSERT_TRUE(shouldExposeWifiSsid(mk(false, t, m, s)));
}

void test_jeton_valide_sta_connecte_expose(void) {
  TEST_ASSERT_TRUE(shouldExposeWifiSsid(mk(true, true, 1, true)));
}

void test_sans_jeton_mode_sta_masque(void) {
  TEST_ASSERT_FALSE(shouldExposeWifiSsid(mk(true, false, 1, true)));
  TEST_ASSERT_FALSE(shouldExposeWifiSsid(mk(true, false, 1, false)));
}

void test_sans_jeton_mode_ap_expose(void) {
  TEST_ASSERT_TRUE(shouldExposeWifiSsid(mk(true, false, 2, false)));
  TEST_ASSERT_TRUE(shouldExposeWifiSsid(mk(true, false, 2, true)));
}

void test_sans_jeton_mode_apsta_expose(void) {
  TEST_ASSERT_TRUE(shouldExposeWifiSsid(mk(true, false, 3, true)));
  TEST_ASSERT_TRUE(shouldExposeWifiSsid(mk(true, false, 3, false)));
}

void test_wifiModeHasAp_valeur_par_valeur(void) {
  TEST_ASSERT_FALSE(wifiModeHasAp(0));
  TEST_ASSERT_FALSE(wifiModeHasAp(1));
  TEST_ASSERT_TRUE(wifiModeHasAp(2));
  TEST_ASSERT_TRUE(wifiModeHasAp(3));
  TEST_ASSERT_FALSE(wifiModeHasAp(4));
  TEST_ASSERT_FALSE(wifiModeHasAp(99));
  TEST_ASSERT_FALSE(wifiModeHasAp(-1));
  TEST_ASSERT_FALSE(wifiModeHasAp(-2147483647 - 1));
}

void test_entree_par_defaut_masque(void) {
  TEST_ASSERT_FALSE(shouldExposeWifiSsid(WifiSsidExposureInput{}));
}

void test_staConnected_est_sans_effet(void) {
  const int modes[] = {0, 1, 2, 3, 4, 99, -1};
  for (int a = 0; a < 2; ++a)
    for (int t = 0; t < 2; ++t)
      for (int m : modes) {
        WifiSsidExposureInput v = mk(a, t, m, true);
        WifiSsidExposureInput f = mk(a, t, m, false);
        // Nomme le champ : le supprimer casse la compilation.
        TEST_ASSERT_TRUE(v.staConnected);
        TEST_ASSERT_FALSE(f.staConnected);
        TEST_ASSERT_EQUAL(shouldExposeWifiSsid(v), shouldExposeWifiSsid(f));
      }
}

// Balayage exhaustif : 2 auth x 2 jeton x 7 modes x 2 sta = 56 combinaisons.
// Exposées : auth coupée = 1*2*7*2 = 28 ; auth active + jeton = 1*1*7*2 = 14 ;
// auth active sans jeton, modes 2 et 3 = 2*2 = 4. Total 28+14+4 = 46 (10 masquées).
void test_balayage_exhaustif_compte_fige(void) {
  const int modes[] = {0, 1, 2, 3, 4, 99, -1};
  int total = 0, exposes = 0;
  for (int a = 0; a < 2; ++a)
    for (int t = 0; t < 2; ++t)
      for (int m : modes)
        for (int s = 0; s < 2; ++s) {
          const bool r = shouldExposeWifiSsid(mk(a, t, m, s));
          TEST_ASSERT_EQUAL(modele(a, t, m), r);
          ++total;
          if (r) ++exposes;
        }
  TEST_ASSERT_EQUAL(56, total);
  TEST_ASSERT_EQUAL(46, exposes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_auth_coupee_expose_toujours);
  RUN_TEST(test_jeton_valide_sta_connecte_expose);
  RUN_TEST(test_sans_jeton_mode_sta_masque);
  RUN_TEST(test_sans_jeton_mode_ap_expose);
  RUN_TEST(test_sans_jeton_mode_apsta_expose);
  RUN_TEST(test_wifiModeHasAp_valeur_par_valeur);
  RUN_TEST(test_entree_par_defaut_masque);
  RUN_TEST(test_staConnected_est_sans_effet);
  RUN_TEST(test_balayage_exhaustif_compte_fige);
  return UNITY_END();
}
