// =============================================================================
// Tests unitaires natifs — schedule_logic (feature-038)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32 / RTC.
// On teste le COMPORTEMENT observable du module de planning horaire PUR :
//   - timeStringToMinutes (AC3)
//   - isMinutesInRange (AC2)
//   - computeAutoWindow (AC4, pivot = kFiltrationPivotHour = 13.0)
//   - decideFiltrationRun (AC5/AC1)
// via l'API publique, pas l'implémentation interne.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <string.h>
#include "schedule_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Pivot de référence (constants.h : kFiltrationPivotHour = 13.0f).
static const float kPivot = 13.0f;

// -----------------------------------------------------------------------------
// AC3 — timeStringToMinutes
// -----------------------------------------------------------------------------
void test_time_string_valid(void) {
  TEST_ASSERT_EQUAL_INT(0, timeStringToMinutes("00:00"));
  TEST_ASSERT_EQUAL_INT(1439, timeStringToMinutes("23:59"));
  TEST_ASSERT_EQUAL_INT(750, timeStringToMinutes("12:30"));
}

void test_time_string_invalid_length_or_separator(void) {
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes(""));      // longueur < 5
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes("1:30"));  // longueur < 5
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes("12-30")); // séparateur != ':'
}

void test_time_string_invalid_out_of_range(void) {
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes("24:00")); // hh > 23
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes("12:60")); // mm > 59
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes("99:99")); // hh & mm hors plage
}

void test_time_string_null(void) {
  TEST_ASSERT_EQUAL_INT(-1, timeStringToMinutes(nullptr));
}

// -----------------------------------------------------------------------------
// AC2 — isMinutesInRange
// -----------------------------------------------------------------------------
void test_range_simple_start_lt_end(void) {
  // fenêtre 08:00-18:00 = [480, 1080)
  TEST_ASSERT_TRUE(isMinutesInRange(480, 480, 1080));   // borne basse incluse
  TEST_ASSERT_TRUE(isMinutesInRange(1079, 480, 1080));  // juste avant la fin
  TEST_ASSERT_FALSE(isMinutesInRange(1080, 480, 1080)); // borne haute exclue
  TEST_ASSERT_FALSE(isMinutesInRange(479, 480, 1080));  // juste avant le début
}

void test_range_midnight_start_gt_end(void) {
  // fenêtre 22:00-06:00 = [1320, 360) à cheval sur minuit
  TEST_ASSERT_TRUE(isMinutesInRange(1320, 1320, 360));  // début inclus
  TEST_ASSERT_TRUE(isMinutesInRange(0, 1320, 360));     // minuit inclus
  TEST_ASSERT_TRUE(isMinutesInRange(359, 1320, 360));   // juste avant la fin
  TEST_ASSERT_FALSE(isMinutesInRange(360, 1320, 360));  // fin exclue
  TEST_ASSERT_FALSE(isMinutesInRange(720, 1320, 360));  // milieu de journée hors plage
}

void test_range_invalid(void) {
  TEST_ASSERT_FALSE(isMinutesInRange(600, -1, 1080)); // start invalide
  TEST_ASSERT_FALSE(isMinutesInRange(600, 480, -1));  // end invalide
  TEST_ASSERT_FALSE(isMinutesInRange(600, 600, 600)); // start == end
}

// -----------------------------------------------------------------------------
// feature-040 AC2 — isMinutesInRange : 4e param equalMeansAlways + non-régression
// -----------------------------------------------------------------------------
void test_range_equal_means_always_default_false(void) {
  // start==end : par défaut (filtration) → false ; explicite false → false ;
  // explicite true (éclairage) → toute la journée → true.
  TEST_ASSERT_FALSE(isMinutesInRange(600, 300, 300));         // défaut = false
  TEST_ASSERT_FALSE(isMinutesInRange(600, 300, 300, false));  // filtration
  TEST_ASSERT_TRUE(isMinutesInRange(600, 300, 300, true));    // éclairage
}

void test_range_minus_one_guard_beats_equal_means_always(void) {
  // La garde -1 prime sur start==end, même avec equalMeansAlways=true.
  TEST_ASSERT_FALSE(isMinutesInRange(600, -1, -1, true));   // -1==-1 ne doit pas
                                                           // renvoyer true
  TEST_ASSERT_FALSE(isMinutesInRange(600, -1, 300, true)); // start invalide
}

// feature-040 AC3 — fenêtres éclairage (equalMeansAlways=true ne change rien hors
// du cas start==end : les plages normales conservent la sémantique [start, end)).
void test_range_lighting_simple_window(void) {
  // 20:00-23:00 = [1200, 1380)
  TEST_ASSERT_TRUE(isMinutesInRange(1200, 1200, 1380, true));  // début inclus
  TEST_ASSERT_TRUE(isMinutesInRange(1379, 1200, 1380, true));  // avant fin
  TEST_ASSERT_FALSE(isMinutesInRange(1380, 1200, 1380, true)); // fin exclue
  TEST_ASSERT_FALSE(isMinutesInRange(1199, 1200, 1380, true)); // avant début
}

void test_range_lighting_midnight_window(void) {
  // 22:00-02:00 = [1320, 120) à cheval sur minuit
  TEST_ASSERT_TRUE(isMinutesInRange(1320, 1320, 120, true));  // début inclus
  TEST_ASSERT_TRUE(isMinutesInRange(0, 1320, 120, true));     // minuit inclus
  TEST_ASSERT_TRUE(isMinutesInRange(119, 1320, 120, true));   // avant fin
  TEST_ASSERT_FALSE(isMinutesInRange(120, 1320, 120, true));  // fin exclue
  TEST_ASSERT_FALSE(isMinutesInRange(720, 1320, 120, true));  // milieu de journée
}

// -----------------------------------------------------------------------------
// AC4 — computeAutoWindow (pivot = 13.0)
// -----------------------------------------------------------------------------
void test_auto_window_floor_one_hour_temp_zero(void) {
  // tempC=0 → duration plancher 1h, centrée sur 13.0 → 12:30-13:30
  ScheduleWindow w = computeAutoWindow(0.0f, kPivot);
  TEST_ASSERT_EQUAL_INT(750, w.startMin); // 12:30
  TEST_ASSERT_EQUAL_INT(810, w.endMin);   // 13:30
}

void test_auto_window_floor_one_hour_temp_negative(void) {
  // tempC négative → ramené à 0 → duration 1h → 12:30-13:30
  ScheduleWindow w = computeAutoWindow(-5.0f, kPivot);
  TEST_ASSERT_EQUAL_INT(750, w.startMin);
  TEST_ASSERT_EQUAL_INT(810, w.endMin);
}

void test_auto_window_temp_two_is_one_hour(void) {
  // tempC=2 → duration = 2/2 = 1h (plancher atteint) → 12:30-13:30
  ScheduleWindow w = computeAutoWindow(2.0f, kPivot);
  TEST_ASSERT_EQUAL_INT(750, w.startMin);
  TEST_ASSERT_EQUAL_INT(810, w.endMin);
}

void test_auto_window_temp_eight_four_hours_centered(void) {
  // tempC=8 → duration = 4h centrée sur 13.0 → start 11:00, end 15:00
  ScheduleWindow w = computeAutoWindow(8.0f, kPivot);
  TEST_ASSERT_EQUAL_INT(660, w.startMin); // 11:00
  TEST_ASSERT_EQUAL_INT(900, w.endMin);   // 15:00
}

void test_auto_window_temp_48_full_day_with_wrap(void) {
  // tempC=48 → duration = 24h (plafond). start = 13-12 = 1.0 → 01:00 = 60.
  // end = 1+24 = 25.0 → wrap → 1.0 → 01:00 = 60. (fenêtre = 24h complète)
  ScheduleWindow w = computeAutoWindow(48.0f, kPivot);
  TEST_ASSERT_EQUAL_INT(60, w.startMin);
  TEST_ASSERT_EQUAL_INT(60, w.endMin);
}

void test_auto_window_temp_50_capped_24h(void) {
  // tempC=50 → duration plafonnée à 24h → identique à tempC=48
  ScheduleWindow w = computeAutoWindow(50.0f, kPivot);
  TEST_ASSERT_EQUAL_INT(60, w.startMin);
  TEST_ASSERT_EQUAL_INT(60, w.endMin);
}

// -----------------------------------------------------------------------------
// AC5/AC1 — decideFiltrationRun
// -----------------------------------------------------------------------------
void test_decide_force_on_has_priority(void) {
  // forceOn=true l'emporte sur tout, même forceOff=true
  TEST_ASSERT_TRUE(decideFiltrationRun("auto", true, true, true, 720, 480, 1080, false));
  TEST_ASSERT_TRUE(decideFiltrationRun("off", true, false, false, 0, -1, -1, false));
}

void test_decide_force_off_without_force_on(void) {
  TEST_ASSERT_FALSE(decideFiltrationRun("auto", false, true, true, 720, 480, 1080, true));
}

void test_decide_auto_follows_range_when_have_time(void) {
  // dans la plage → marche ; hors plage → arrêt
  TEST_ASSERT_TRUE(decideFiltrationRun("auto", false, false, true, 720, 480, 1080, false));
  TEST_ASSERT_FALSE(decideFiltrationRun("auto", false, false, true, 1200, 480, 1080, true));
}

void test_decide_manual_follows_range_when_have_time(void) {
  TEST_ASSERT_TRUE(decideFiltrationRun("manual", false, false, true, 720, 480, 1080, false));
  TEST_ASSERT_FALSE(decideFiltrationRun("manual", false, false, true, 1200, 480, 1080, true));
}

void test_decide_no_time_keeps_current_state(void) {
  // haveTime=false sans forçage → renvoie currentlyRunning tel quel
  TEST_ASSERT_TRUE(decideFiltrationRun("auto", false, false, false, 720, 480, 1080, true));
  TEST_ASSERT_FALSE(decideFiltrationRun("auto", false, false, false, 720, 480, 1080, false));
}

void test_decide_off_or_unknown_mode_is_false(void) {
  TEST_ASSERT_FALSE(decideFiltrationRun("off", false, false, true, 720, 480, 1080, true));
  TEST_ASSERT_FALSE(decideFiltrationRun("bogus", false, false, true, 720, 480, 1080, true));
}

// -----------------------------------------------------------------------------
// feature-040 AC1/AC4 — decideLightingOn (table de vérité, copie de update())
// -----------------------------------------------------------------------------
void test_lighting_manual_override_returns_enabled_flag(void) {
  // manualOverride=true → renvoie enabledFlag, quel que soit le reste.
  TEST_ASSERT_TRUE(decideLightingOn(true, true, true, true, 1200, 1200, 1380, false));
  TEST_ASSERT_FALSE(decideLightingOn(true, false, true, true, 1200, 1200, 1380, true));
  // indépendant de scheduleEnabled / haveTime / plage
  TEST_ASSERT_TRUE(decideLightingOn(true, true, false, false, 0, -1, -1, false));
  TEST_ASSERT_FALSE(decideLightingOn(true, false, false, false, 0, -1, -1, true));
}

void test_lighting_schedule_disabled_returns_enabled_flag(void) {
  // manualOverride=false, scheduleEnabled=false → renvoie enabledFlag.
  TEST_ASSERT_TRUE(decideLightingOn(false, true, false, true, 1200, 1200, 1380, false));
  TEST_ASSERT_FALSE(decideLightingOn(false, false, false, true, 1200, 1200, 1380, true));
}

void test_lighting_schedule_follows_range_when_have_time(void) {
  // scheduleEnabled=true, haveTime=true → suit isMinutesInRange(...,true).
  // dans la plage 20:00-23:00
  TEST_ASSERT_TRUE(decideLightingOn(false, false, true, true, 1200, 1200, 1380, false));
  // hors plage
  TEST_ASSERT_FALSE(decideLightingOn(false, true, true, true, 1199, 1200, 1380, true));
  // start==end → equalMeansAlways=true → allumé toute la journée
  TEST_ASSERT_TRUE(decideLightingOn(false, false, true, true, 600, 300, 300, false));
}

void test_lighting_schedule_no_time_keeps_current_state(void) {
  // scheduleEnabled=true, haveTime=false → renvoie currentlyOn tel quel.
  TEST_ASSERT_TRUE(decideLightingOn(false, false, true, false, 1200, 1200, 1380, true));
  TEST_ASSERT_FALSE(decideLightingOn(false, true, true, false, 1200, 1200, 1380, false));
}

// -----------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();

  // AC3
  RUN_TEST(test_time_string_valid);
  RUN_TEST(test_time_string_invalid_length_or_separator);
  RUN_TEST(test_time_string_invalid_out_of_range);
  RUN_TEST(test_time_string_null);

  // AC2
  RUN_TEST(test_range_simple_start_lt_end);
  RUN_TEST(test_range_midnight_start_gt_end);
  RUN_TEST(test_range_invalid);

  // feature-040 AC2/AC3 — isMinutesInRange equalMeansAlways
  RUN_TEST(test_range_equal_means_always_default_false);
  RUN_TEST(test_range_minus_one_guard_beats_equal_means_always);
  RUN_TEST(test_range_lighting_simple_window);
  RUN_TEST(test_range_lighting_midnight_window);

  // AC4
  RUN_TEST(test_auto_window_floor_one_hour_temp_zero);
  RUN_TEST(test_auto_window_floor_one_hour_temp_negative);
  RUN_TEST(test_auto_window_temp_two_is_one_hour);
  RUN_TEST(test_auto_window_temp_eight_four_hours_centered);
  RUN_TEST(test_auto_window_temp_48_full_day_with_wrap);
  RUN_TEST(test_auto_window_temp_50_capped_24h);

  // AC5/AC1
  RUN_TEST(test_decide_force_on_has_priority);
  RUN_TEST(test_decide_force_off_without_force_on);
  RUN_TEST(test_decide_auto_follows_range_when_have_time);
  RUN_TEST(test_decide_manual_follows_range_when_have_time);
  RUN_TEST(test_decide_no_time_keeps_current_state);
  RUN_TEST(test_decide_off_or_unknown_mode_is_false);

  // feature-040 AC1/AC4 — decideLightingOn
  RUN_TEST(test_lighting_manual_override_returns_enabled_flag);
  RUN_TEST(test_lighting_schedule_disabled_returns_enabled_flag);
  RUN_TEST(test_lighting_schedule_follows_range_when_have_time);
  RUN_TEST(test_lighting_schedule_no_time_keeps_current_state);

  return UNITY_END();
}
