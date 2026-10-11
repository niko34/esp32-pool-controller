// =============================================================================
// Tests unitaires natifs — evaluateDailyLimitStreak (daily_limit_streak_logic.h)
// feature-152 (C6, extraction C1 gate pool-chemistry P2)
// =============================================================================
// Motif du gate, mot pour mot : « une alerte non testée est une promesse. »
// Ces tests protègent le SEUL calcul extrait de pump_controller.cpp : un jour
// non atteint ROMPT la série (pas de pardon partiel), la série SATURE à 255
// (ne reboucle jamais), et l'alerte se lève dès que le SEUIL est atteint ou
// dépassé — jamais avant, jamais seulement « au franchissement ».
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "daily_limit_streak_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t kThreshold = 3;  // kDailyLimitStreakAlertDays en production

// =============================================================================
// CAS 1 — Incrément jour après jour, pas d'alerte avant le seuil
// =============================================================================
void test_incremente_jour_apres_jour_sans_alerte_avant_le_seuil(void) {
  DailyLimitStreakResult r1 = evaluateDailyLimitStreak(true, 0, kThreshold);
  TEST_ASSERT_EQUAL_UINT8(1, r1.streak);
  TEST_ASSERT_FALSE(r1.alert);

  DailyLimitStreakResult r2 = evaluateDailyLimitStreak(true, r1.streak, kThreshold);
  TEST_ASSERT_EQUAL_UINT8(2, r2.streak);
  TEST_ASSERT_FALSE(r2.alert);
}

// =============================================================================
// CAS 2 — L'alerte se lève EXACTEMENT au seuil, et reste vraie au-delà
// =============================================================================
void test_alerte_se_leve_au_seuil_et_reste_vraie_au_dela(void) {
  DailyLimitStreakResult r3 = evaluateDailyLimitStreak(true, 2, kThreshold);
  TEST_ASSERT_EQUAL_UINT8(3, r3.streak);
  TEST_ASSERT_TRUE_MESSAGE(r3.alert, "l'alerte doit se lever des que streak == seuil");

  DailyLimitStreakResult r4 = evaluateDailyLimitStreak(true, r3.streak, kThreshold);
  TEST_ASSERT_EQUAL_UINT8(4, r4.streak);
  TEST_ASSERT_TRUE_MESSAGE(r4.alert, "l'alerte reste vraie tant que la serie continue");
}

// =============================================================================
// CAS 3 — Un jour NON atteint rompt la série, SANS pardon partiel
// =============================================================================
void test_jour_non_atteint_rompt_la_serie_sans_pardon(void) {
  // Streak déjà à 5 (bien au-dessus du seuil) : un seul jour où le plafond
  // n'est PAS atteint doit la remettre STRICTEMENT à 0, pas la décrémenter.
  DailyLimitStreakResult r = evaluateDailyLimitStreak(false, 5, kThreshold);
  TEST_ASSERT_EQUAL_UINT8(0, r.streak);
  TEST_ASSERT_FALSE(r.alert);
}

// =============================================================================
// CAS 4 — Saturation à 255 : ne reboucle JAMAIS à 0 par débordement
// =============================================================================
void test_sature_a_255_sans_jamais_reboucler(void) {
  DailyLimitStreakResult r = evaluateDailyLimitStreak(true, 255, kThreshold);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, r.streak,
      "la streak doit saturer a 255, jamais reboucler a 0 (faux << plus d'alerte >>)");
  TEST_ASSERT_TRUE(r.alert);
}

// =============================================================================
// CAS 5 — Juste SOUS le seuil : pas d'alerte (frontière stricte)
// =============================================================================
void test_juste_sous_le_seuil_pas_d_alerte(void) {
  DailyLimitStreakResult r = evaluateDailyLimitStreak(true, 1, kThreshold);  // -> streak=2 < 3
  TEST_ASSERT_EQUAL_UINT8(2, r.streak);
  TEST_ASSERT_FALSE_MESSAGE(r.alert, "2 jours < seuil de 3 : pas encore d'alerte");
}

// =============================================================================
// CAS 6 — Seuil 0 : fonction TOTALE, alerte désactivée (jamais le cas en
// production, mais ne doit jamais se comporter comme un seuil « toujours vrai »)
// =============================================================================
void test_seuil_zero_desactive_l_alerte(void) {
  DailyLimitStreakResult r = evaluateDailyLimitStreak(true, 10, 0);
  TEST_ASSERT_FALSE_MESSAGE(r.alert, "alertThresholdDays == 0 doit desactiver l'alerte");
}

// =============================================================================
// CAS 7 — Deux voies indépendantes : pH et ORP ne se contaminent jamais
// =============================================================================
void test_ph_et_orp_sont_independants(void) {
  DailyLimitStreakResult ph = evaluateDailyLimitStreak(true, 2, kThreshold);
  DailyLimitStreakResult orp = evaluateDailyLimitStreak(false, 2, kThreshold);
  TEST_ASSERT_TRUE(ph.alert);
  TEST_ASSERT_EQUAL_UINT8(3, ph.streak);
  TEST_ASSERT_FALSE(orp.alert);
  TEST_ASSERT_EQUAL_UINT8(0, orp.streak);
}

// =============================================================================
// Runner
// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_incremente_jour_apres_jour_sans_alerte_avant_le_seuil);
  RUN_TEST(test_alerte_se_leve_au_seuil_et_reste_vraie_au_dela);
  RUN_TEST(test_jour_non_atteint_rompt_la_serie_sans_pardon);
  RUN_TEST(test_sature_a_255_sans_jamais_reboucler);
  RUN_TEST(test_juste_sous_le_seuil_pas_d_alerte);
  RUN_TEST(test_seuil_zero_desactive_l_alerte);
  RUN_TEST(test_ph_et_orp_sont_independants);
  return UNITY_END();
}
