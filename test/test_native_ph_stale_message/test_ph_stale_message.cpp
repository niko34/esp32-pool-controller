// feature-170 (C32) — vecteur V6a : le TEXTE de l'alerte « correction à vérifier »
// produit par le module pur src/ph_stale_message.h (ADR-0017).
//
// ⭐ Ce que cette suite ne prouve PAS : que quelqu'un l'appelle, et au bon endroit.
// Un vecteur natif ne prouve jamais un site d'appel — c'est le travail de
// tools/check_ph_stale_alert_emitted.sh (feature-170, V6b).
//
// Ce qu'elle protège : la forme du message (valeur signée à 2 décimales, virgule),
// l'absence du mot « sonde », la sûreté du tampon, et — surtout — la MARGE de
// canal : publishAlert() tronque à ~46 octets utiles ; le verdict « à vérifier »
// doit tenir en entier dans ces 46 octets, y compris pour une valeur à 2 chiffres.
#include <unity.h>
#include <string.h>
#include <stdio.h>

#include "ph_stale_message.h"

void setUp(void) {}
void tearDown(void) {}

// Budget utile de publishAlert() : kMaxPayloadLen=128, enveloppe JSON ~62 o,
// queue de troncature 19 o => ~46 octets de message.
static const size_t kUsefulBytes = 46;

static void render(char* out, size_t cap, float offset) {
  phStaleCalibrationMessage(out, cap, offset);
}

// `needle` figure ENTIÈREMENT dans les `limit` premiers OCTETS de `msg`.
static bool fitsInFirstBytes(const char* msg, const char* needle, size_t limit) {
  const char* hit = strstr(msg, needle);
  if (!hit) return false;
  return (size_t)(hit - msg) + strlen(needle) <= limit;
}

void test_valeur_positive_deux_decimales_signee_virgule(void) {
  char buf[320];
  render(buf, sizeof(buf), 1.20f);
  TEST_ASSERT_NOT_NULL(strstr(buf, "+1,20"));
  TEST_ASSERT_NULL_MESSAGE(strstr(buf, "1.20"), "separateur decimal : virgule, jamais point");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, strncmp(buf, "Correction de mesure +1,20", 26),
                                  "la valeur suit immediatement « Correction de mesure »");
}

void test_valeur_negative_signe_moins_virgule(void) {
  char buf[320];
  render(buf, sizeof(buf), -0.30f);
  // Le firmware écrit un '-' ASCII (snprintf %+.2f) ; l'écran, lui, un « − » U+2212.
  TEST_ASSERT_NOT_NULL(strstr(buf, "-0,30"));
  TEST_ASSERT_NULL(strstr(buf, "0.30"));
}

void test_signe_plus_explicite_et_deux_decimales_sur_valeurs_rondes(void) {
  char buf[320];
  render(buf, sizeof(buf), 1.0f);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "+1,00"), "2 decimales meme sur une valeur ronde, signe + explicite");
  render(buf, sizeof(buf), 10.5f);
  TEST_ASSERT_NOT_NULL(strstr(buf, "+10,50"));
}

void test_le_mot_sonde_n_apparait_jamais(void) {
  const float samples[] = {1.20f, -0.30f, 10.50f, -10.50f, 0.01f};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    char buf[320];
    render(buf, sizeof(buf), samples[i]);
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "sonde"), "la correction compense la MESURE, pas la sonde");
    TEST_ASSERT_NULL(strstr(buf, "Sonde"));
  }
}

// feature-170 (C29/C32), arbitrage Q14 du 2026-11-22.
// 🔴 markPhCalibrationStamped() est aussi appelee sur un EFFACEMENT d'etalonnage
// (ClearPhCal, sensors.cpp:920, deliberement) : « refait » et « vient d'avoir
// lieu » y seraient FAUX, au moment meme d'un depannage — donc la ou
// l'utilisateur lit, et ou ce projet a deja fait condamner une sonde SAINE
// (25/08/2026). ⭐ Le texte nomme l'ETAT, ⛔ jamais le chemin (ADR-0139).
void test_les_mots_qui_affirment_un_evenement_non_prouve_sont_bannis(void) {
  const char* bannis[] = {"refait", "vient d'avoir lieu", "Vous avez"};
  const float samples[] = {1.20f, -0.30f, 10.50f, -10.50f, 0.01f};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    char buf[320];
    render(buf, sizeof(buf), samples[i]);
    for (size_t j = 0; j < sizeof(bannis) / sizeof(bannis[0]); ++j) {
      TEST_ASSERT_NULL_MESSAGE(strstr(buf, bannis[j]),
          "mot banni : trois chemins menent ici, le texte ne peut pas affirmer lequel");
    }
  }
}

void test_la_phrase_d_acte_termine_le_message(void) {
  char buf[320];
  render(buf, sizeof(buf), 1.20f);
  const size_t n = strlen(buf), a = strlen(PH_STALE_ACTE);
  TEST_ASSERT_TRUE(n > a);
  TEST_ASSERT_EQUAL_STRING(PH_STALE_ACTE, buf + n - a);
}

// ---- Sûreté du tampon (patron snprintf) --------------------------------------

void test_valeur_de_retour_est_la_longueur_voulue_meme_tronque(void) {
  char full[320];
  const size_t want = phStaleCalibrationMessage(full, sizeof(full), 1.20f);
  TEST_ASSERT_EQUAL_UINT32(strlen(full), want);

  char small[16];
  const size_t wantSmall = phStaleCalibrationMessage(small, sizeof(small), 1.20f);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, wantSmall, "retour = longueur VOULUE, pas la longueur ecrite");
  TEST_ASSERT_EQUAL_UINT32(sizeof(small) - 1, strlen(small));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, small[sizeof(small) - 1], "toujours termine par '\\0'");
  TEST_ASSERT_EQUAL_UINT8(0, strncmp(small, full, sizeof(small) - 1));
}

void test_tampon_court_ne_depasse_jamais(void) {
  // Garde-fous de part et d'autre du tampon : rien ne doit y être écrit.
  for (size_t cap = 1; cap <= 64; ++cap) {
    char arena[64 + 2 * 4];
    memset(arena, 0x5A, sizeof(arena));
    char* out = arena + 4;
    phStaleCalibrationMessage(out, cap, 10.50f);
    TEST_ASSERT_NOT_NULL_MESSAGE(memchr(out, 0, cap), "'\\0' absent du tampon");
    for (int g = 0; g < 4; ++g) {
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x5A, (unsigned char)arena[g], "garde AVANT ecrasee");
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x5A, (unsigned char)out[cap + g], "garde APRES ecrasee");
    }
  }
}

void test_tampon_de_taille_zero_n_ecrit_rien(void) {
  char c = 0x5A;
  const size_t want = phStaleCalibrationMessage(&c, 0, 1.20f);
  TEST_ASSERT_EQUAL_UINT8(0x5A, (unsigned char)c);
  TEST_ASSERT_TRUE(want > 0);
}

// ---- LA MARGE (exigence du gate) ---------------------------------------------
// « à vérifier » EN ENTIER dans les 46 premiers OCTETS (à = 2 octets, é = 2 octets).
// Sans cette assertion, la marge se perdra à la première retouche de la tête.

void test_marge_verdict_dans_les_46_premiers_octets_pour_1_20(void) {
  char buf[320];
  render(buf, sizeof(buf), 1.20f);
  TEST_ASSERT_TRUE_MESSAGE(fitsInFirstBytes(buf, "à vérifier", kUsefulBytes),
                           "« à vérifier » doit tenir en entier dans les 46 premiers octets (+1,20)");
}

void test_marge_verdict_dans_les_46_premiers_octets_pour_10_50(void) {
  char buf[320];
  render(buf, sizeof(buf), 10.50f);
  TEST_ASSERT_TRUE_MESSAGE(fitsInFirstBytes(buf, "à vérifier", kUsefulBytes),
                           "« à vérifier » doit tenir en entier dans les 46 premiers octets (+10,50)");
}

void test_marge_valeur_negative_a_deux_chiffres(void) {
  char buf[320];
  render(buf, sizeof(buf), -10.50f);
  TEST_ASSERT_TRUE(fitsInFirstBytes(buf, "à vérifier", kUsefulBytes));
}

void test_la_tete_garde_au_moins_quelques_octets_de_marge(void) {
  // Le gate a raccourci la tête pour garder ~5 octets de marge (une tête à 46
  // pile aurait coupé « vérifier » dès +10,50). On exige ≥ 4 octets de marge.
  char buf[320];
  render(buf, sizeof(buf), 10.50f);
  const char* hit = strstr(buf, "à vérifier");
  TEST_ASSERT_NOT_NULL(hit);
  const size_t end = (size_t)(hit - buf) + strlen("à vérifier");
  TEST_ASSERT_TRUE_MESSAGE(end + 4 <= kUsefulBytes, "moins de 4 octets de marge sur le canal MQTT");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valeur_positive_deux_decimales_signee_virgule);
  RUN_TEST(test_valeur_negative_signe_moins_virgule);
  RUN_TEST(test_signe_plus_explicite_et_deux_decimales_sur_valeurs_rondes);
  RUN_TEST(test_le_mot_sonde_n_apparait_jamais);
  RUN_TEST(test_les_mots_qui_affirment_un_evenement_non_prouve_sont_bannis);
  RUN_TEST(test_la_phrase_d_acte_termine_le_message);
  RUN_TEST(test_valeur_de_retour_est_la_longueur_voulue_meme_tronque);
  RUN_TEST(test_tampon_court_ne_depasse_jamais);
  RUN_TEST(test_tampon_de_taille_zero_n_ecrit_rien);
  RUN_TEST(test_marge_verdict_dans_les_46_premiers_octets_pour_1_20);
  RUN_TEST(test_marge_verdict_dans_les_46_premiers_octets_pour_10_50);
  RUN_TEST(test_marge_valeur_negative_a_deux_chiffres);
  RUN_TEST(test_la_tete_garde_au_moins_quelques_octets_de_marge);
  return UNITY_END();
}
