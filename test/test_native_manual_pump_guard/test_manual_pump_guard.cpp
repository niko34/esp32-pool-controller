// bug-171 : garde-fou des pompes en mode manuel (logique pure, ADR-0017).
#include <unity.h>
#include "manual_pump_guard_logic.h"
#include "constants.h"

void setUp(void) {}
void tearDown(void) {}

// Raccourci : pompe en manuel, échéance armée, eau présente, pas d'étalonnage.
static ManualCut cut(bool expired, bool water = true, bool cal = false) {
  return evaluateManualCut(true, true, expired, water, cal);
}

// C1
void test_c1_deadline_reached_cuts(void) {
  TEST_ASSERT_EQUAL(ManualCut::kDeadline, cut(true));
}
// C2
void test_c2_water_lost_cuts(void) {
  TEST_ASSERT_EQUAL(ManualCut::kWaterLost, cut(false, false));
}
// C3
void test_c3_cal_session_cuts(void) {
  TEST_ASSERT_EQUAL(ManualCut::kCalSession, cut(false, true, true));
}
// C4 : marches d'assistant de débit non tronquées
void test_c4_flow_wizard_runs_not_truncated(void) {
  const uint32_t starts[] = {0u, 1000u, 0xFFFFFF00u};  // dont un départ proche du repliement
  const uint32_t durations[] = {kFlowAssistPurgeDurationS * 1000u, kFlowAssistMeasureDurationS * 1000u};
  for (uint32_t s : starts) {
    for (uint32_t d : durations) {
      uint32_t deadline = s + d + kManualRunGraceMs;
      // à chaque instant avant l'échéance (pas de 500 ms) : on laisse tourner
      for (uint32_t t = 0; t < d + kManualRunGraceMs; t += 500) {
        bool exp = isManualRunExpired(s + t, deadline);
        TEST_ASSERT_FALSE(exp);
        TEST_ASSERT_EQUAL(ManualCut::kNone, cut(exp));
      }
      TEST_ASSERT_TRUE(isManualRunExpired(deadline, deadline));
    }
  }
  // la purge/mesure doivent tenir sous le plafond d'une marche d'assistant, pas sous celui du test
  TEST_ASSERT_TRUE(kManualPumpTestMaxOnMs < 60000u);
}
// C5 : mode manuel levé => jamais de coupure, toutes combinaisons
void test_c5_manual_off_never_cuts(void) {
  for (int m = 0; m < 16; m++) {
    bool armed = m & 1, exp = m & 2, water = m & 4, cal = m & 8;
    TEST_ASSERT_EQUAL(ManualCut::kNone, evaluateManualCut(false, armed, exp, water, cal));
  }
}
// C5 bis : manuel levé ET échéance annulée, sinon rien ne doit rendre kNone à tort
void test_c5b_manual_on_nominal_only_none(void) {
  for (int m = 0; m < 16; m++) {
    bool armed = m & 1, exp = m & 2, water = m & 4, cal = m & 8;
    bool nominal = armed && !exp && water && !cal;
    ManualCut r = evaluateManualCut(true, armed, exp, water, cal);
    TEST_ASSERT_EQUAL(nominal, r == ManualCut::kNone);
  }
}
// C6 : priorité
void test_c6_priority(void) {
  TEST_ASSERT_EQUAL(ManualCut::kWaterLost, cut(true, false, true));
  TEST_ASSERT_EQUAL(ManualCut::kWaterLost, cut(true, false, false));
  TEST_ASSERT_EQUAL(ManualCut::kWaterLost, cut(false, false, true));
  TEST_ASSERT_EQUAL(ManualCut::kCalSession, cut(true, true, true));
}
// C7 : fail-closed
void test_c7_fail_closed_no_deadline(void) {
  TEST_ASSERT_EQUAL(ManualCut::kDeadline, evaluateManualCut(true, false, false, true, false));
  TEST_ASSERT_EQUAL(ManualCut::kDeadline, evaluateManualCut(true, false, true, true, false));
}
// C8 : repliement de millis()
void test_c8_millis_wraparound(void) {
  const uint32_t deadline = 0x00000064u;
  TEST_ASSERT_FALSE(isManualRunExpired(0xFFFFFF00u, deadline));
  TEST_ASSERT_FALSE(isManualRunExpired(0xFFFFFFFFu, deadline));
  TEST_ASSERT_FALSE(isManualRunExpired(0x00000063u, deadline));
  TEST_ASSERT_TRUE(isManualRunExpired(0x00000064u, deadline));
  TEST_ASSERT_TRUE(isManualRunExpired(0x00000065u, deadline));
}
// Borne exacte
void test_boundary_equal_is_expired(void) {
  TEST_ASSERT_TRUE(isManualRunExpired(1000u, 1000u));
  TEST_ASSERT_FALSE(isManualRunExpired(999u, 1000u));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_c1_deadline_reached_cuts);
  RUN_TEST(test_c2_water_lost_cuts);
  RUN_TEST(test_c3_cal_session_cuts);
  RUN_TEST(test_c4_flow_wizard_runs_not_truncated);
  RUN_TEST(test_c5_manual_off_never_cuts);
  RUN_TEST(test_c5b_manual_on_nominal_only_none);
  RUN_TEST(test_c6_priority);
  RUN_TEST(test_c7_fail_closed_no_deadline);
  RUN_TEST(test_c8_millis_wraparound);
  RUN_TEST(test_boundary_equal_is_expired);
  return UNITY_END();
}
