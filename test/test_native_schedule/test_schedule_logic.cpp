// =============================================================================
// Tests unitaires natifs — schedule_logic (feature-038, bug-126)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32 / RTC.
// On teste le COMPORTEMENT observable du module de planning horaire PUR :
//   - timeStringToMinutes (AC3)
//   - isMinutesInRange (AC2)
//   - computeAutoWindow (AC4, pivot = kFiltrationPivotHour = 13.0)
//   - decideFiltrationRun (AC5/AC1)
//   - CycleTempAccumulator / isSampleEligible / accumulateCycleTemp (bug-126, C2/C9)
//   - isPlausibleReferenceTemp / selectFiltrationRefTemp (bug-126, C4/C16)
//   - resolveReferenceTemp / decideSchedulePromotion (bug-126, C5/C6)
// via l'API publique, pas l'implémentation interne.
//
// Section « Sentinelles de source » en fin de fichier : quelques invariants du
// bug-126 ne sont observables que dans la COQUILLE (filtration.cpp,
// pump_controller.cpp), pas dans la logique pure. Patron repris de
// test_native_temp_compensation/test_temp_compensation.cpp:948 (readProjectFile
// / countOccurrences / extractFunctionBody / stripCppComments).
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <string.h>
#include <stdio.h>   // fopen (sentinelles de source, bug-126)
#include <stdlib.h>  // malloc/free (sentinelles de source, bug-126)
#include "schedule_logic.h"
#include "constants.h"  // kFiltrationRefTempMinC/MaxC/MinRepresentativeSamples (bug-126) — self-contained, pas d'Arduino.h

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
// feature-011 — remainingRangeMinutes : horizon de répartition scheduled,
// BORNÉ À MINUIT (les compteurs journaliers se réinitialisent à minuit).
// -----------------------------------------------------------------------------
void test_F011_remaining_simple_range(void) {
  // Plage simple 10:00-14:00 = [600, 840).
  TEST_ASSERT_EQUAL_INT(180, remainingRangeMinutes(660, 600, 840));  // 11:00 → 180
  TEST_ASSERT_EQUAL_INT(120, remainingRangeMinutes(720, 600, 840));  // 12:00 → 120
}

void test_F011_remaining_out_of_range_zero(void) {
  // Hors plage (avant le début / après la fin) → 0.
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(599, 600, 840));   // juste avant
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(900, 600, 840));   // bien après
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(0, 600, 840));     // minuit
}

void test_F011_remaining_midnight_range(void) {
  // Plage à cheval sur minuit 22:00-06:00 = [1320, 360).
  // Partie du soir : horizon BORNÉ À MINUIT (jamais end - now modulo 1440).
  TEST_ASSERT_EQUAL_INT(60, remainingRangeMinutes(1380, 1320, 360));  // 23:00 → 60
  TEST_ASSERT_EQUAL_INT(120, remainingRangeMinutes(1320, 1320, 360)); // 22:00 → 120
  // Partie du matin : horizon = end - now.
  TEST_ASSERT_EQUAL_INT(180, remainingRangeMinutes(180, 1320, 360));  // 03:00 → 180
  TEST_ASSERT_EQUAL_INT(360, remainingRangeMinutes(0, 1320, 360));    // 00:00 → 360
  // Milieu de journée, hors plage → 0.
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(720, 1320, 360));
}

void test_F011_remaining_invalid_zero(void) {
  // Bornes invalides (-1) → 0 (délégué à isMinutesInRange).
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(660, -1, 840));
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(660, 600, -1));
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(660, -1, -1));
  // start == end → plage invalide filtration (equalMeansAlways=false) → 0.
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(660, 600, 600));
}

void test_F011_remaining_boundaries(void) {
  // Plage simple [600, 840) : frontières exactes.
  TEST_ASSERT_EQUAL_INT(240, remainingRangeMinutes(600, 600, 840));  // now==start
  TEST_ASSERT_EQUAL_INT(1, remainingRangeMinutes(839, 600, 840));    // now==end-1 → >= 1
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(840, 600, 840));    // now==end (exclusive)
  // Plage à cheval [1320, 360) : frontières du matin et veille de minuit.
  TEST_ASSERT_EQUAL_INT(1, remainingRangeMinutes(1439, 1320, 360));  // 23:59 → 1 (borné minuit)
  TEST_ASSERT_EQUAL_INT(1, remainingRangeMinutes(359, 1320, 360));   // end-1 → 1
  TEST_ASSERT_EQUAL_INT(0, remainingRangeMinutes(360, 1320, 360));   // end exclu
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
  TEST_ASSERT_TRUE(decideFiltrationRun(false, "auto", true, true, true, 720, 480, 1080, false));
  TEST_ASSERT_TRUE(decideFiltrationRun(false, "off", true, false, false, 0, -1, -1, false));
}

void test_decide_force_off_without_force_on(void) {
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "auto", false, true, true, 720, 480, 1080, true));
}

void test_decide_auto_follows_range_when_have_time(void) {
  // dans la plage → marche ; hors plage → arrêt
  TEST_ASSERT_TRUE(decideFiltrationRun(false, "auto", false, false, true, 720, 480, 1080, false));
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "auto", false, false, true, 1200, 480, 1080, true));
}

void test_decide_manual_follows_range_when_have_time(void) {
  TEST_ASSERT_TRUE(decideFiltrationRun(false, "manual", false, false, true, 720, 480, 1080, false));
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "manual", false, false, true, 1200, 480, 1080, true));
}

void test_decide_no_time_keeps_current_state(void) {
  // haveTime=false sans forçage → renvoie currentlyRunning tel quel
  TEST_ASSERT_TRUE(decideFiltrationRun(false, "auto", false, false, false, 720, 480, 1080, true));
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "auto", false, false, false, 720, 480, 1080, false));
}

void test_decide_off_or_unknown_mode_is_false(void) {
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "off", false, false, true, 720, 480, 1080, true));
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "bogus", false, false, true, 720, 480, 1080, true));
}

// -----------------------------------------------------------------------------
// feature-053 — decideFiltrationRun : boostForce en priorité MAXIMALE.
// boostForce=true → toujours true, quels que soient forceOff/mode/horaire/RTC
// (turnover maximal pendant le Boost, prioritaire sur forceOff).
// -----------------------------------------------------------------------------
void test_F053_boost_force_beats_force_off(void) {
  // forceOff=true + hors plage + mode off → boostForce l'emporte quand même.
  TEST_ASSERT_TRUE(decideFiltrationRun(true, "off", false, true, true, 1200, 480, 1080, false));
}

void test_F053_boost_force_beats_everything(void) {
  // Toutes combinaisons défavorables : mode inconnu, pas d'heure, hors plage,
  // forceOff, currentlyRunning=false → boostForce force la marche.
  TEST_ASSERT_TRUE(decideFiltrationRun(true, "bogus", false, true, false, 0, -1, -1, false));
  TEST_ASSERT_TRUE(decideFiltrationRun(true, "auto", false, true, true, 1200, 480, 1080, false));
  TEST_ASSERT_TRUE(decideFiltrationRun(true, "manual", true, true, false, 720, -1, -1, false));
}

void test_F053_boost_force_false_no_regression(void) {
  // boostForce=false → comportement strictement identique à avant feature-053.
  // Dans la plage auto → marche ; hors plage → arrêt ; forceOff → arrêt.
  TEST_ASSERT_TRUE(decideFiltrationRun(false, "auto", false, false, true, 720, 480, 1080, false));
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "auto", false, false, true, 1200, 480, 1080, true));
  TEST_ASSERT_FALSE(decideFiltrationRun(false, "auto", false, true, true, 720, 480, 1080, true));
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

// =============================================================================
// bug-126 — CycleTempAccumulator / isSampleEligible / accumulateCycleTemp
// (condition C2 pool-chemistry : purge ET plage ET sonde vivante, cumulatives)
// =============================================================================

// Encodage SondeRole attendu par selectFiltrationRefTemp (voir static_assert
// côté coquille, src/filtration.cpp).
static const uint8_t kRoleUnknown = 0;
static const uint8_t kRoleWater = 1;
static const uint8_t kRoleCircuit = 2;

void test_reset_cycle_temp_accumulator_is_nan_and_zero(void) {
  CycleTempAccumulator acc;
  acc.mean = 12.0f; acc.minC = 1.0f; acc.maxC = 2.0f; acc.count = 99;
  resetCycleTempAccumulator(acc);
  TEST_ASSERT_TRUE(isnan(acc.mean));
  TEST_ASSERT_TRUE(isnan(acc.minC));
  TEST_ASSERT_TRUE(isnan(acc.maxC));
  TEST_ASSERT_EQUAL_UINT16(0, acc.count);
}

// M1/M2/M3 — table de vérité exhaustive des 8 combinaisons. Une garde retirée
// (n'importe laquelle des trois) rend au moins UNE ligne fausse.
void test_sample_eligible_truth_table_all_eight_combinations(void) {
  for (int settled = 0; settled <= 1; settled++) {
    for (int inSched = 0; inSched <= 1; inSched++) {
      for (int frozen = 0; frozen <= 1; frozen++) {
        bool expected = (settled != 0) && (inSched != 0) && (frozen == 0);
        bool actual = isSampleEligible(settled != 0, inSched != 0, frozen != 0);
        char msg[96];
        snprintf(msg, sizeof(msg), "settled=%d inSchedule=%d frozen=%d", settled, inSched, frozen);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected, actual, msg);
      }
    }
  }
}

// M1 — preuve de la condition C2 nommément : un forceOn nocturne (22:00-02:00)
// produit des échantillons hors-plage. Même avec la purge réglée et la sonde
// vivante, AUCUN des 240 échantillons ne doit être retenu — la garde
// `inSchedule` doit, à elle seule, fermer tout le lot.
void test_C2_force_on_nocturnal_samples_all_rejected_by_in_schedule_guard(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  for (int minute = 0; minute < 240; minute++) {
    // circulationSettled=true, inSchedule=false (forceOn hors plage 07:36-18:24
    // par ex.), temperatureFrozen=false : SEULE la garde de plage doit rejeter.
    bool eligible = isSampleEligible(/*circulationSettled=*/true, /*inSchedule=*/false,
                                     /*temperatureFrozen=*/false);
    accumulateCycleTemp(acc, 18.0f + (float)(minute % 5) * 0.1f, eligible);
  }
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, acc.count,
      "Un forceOn nocturne ne doit jamais nourrir la moyenne (C2) : si ce test "
      "passe alors que la garde inSchedule a saute, la preuve de C2 est perdue.");
}

// M2 — un échantillon pris avant kPurgeFloorMs (eau stagnante) doit être rejeté
// même si l'heure est en plage et la sonde vivante.
void test_C2_sample_before_purge_settled_is_rejected(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  bool eligible = isSampleEligible(/*circulationSettled=*/false, /*inSchedule=*/true,
                                   /*temperatureFrozen=*/false);
  bool retained = accumulateCycleTemp(acc, 22.0f, eligible);
  TEST_ASSERT_FALSE(retained);
  TEST_ASSERT_EQUAL_UINT16(0, acc.count);
}

// M3 — une sonde figée ne doit plus nourrir la moyenne (remplaçant de la bande
// morte ADR-0074).
void test_C2_frozen_probe_sample_is_rejected(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  bool eligible = isSampleEligible(/*circulationSettled=*/true, /*inSchedule=*/true,
                                   /*temperatureFrozen=*/true);
  bool retained = accumulateCycleTemp(acc, 22.0f, eligible);
  TEST_ASSERT_FALSE(retained);
  TEST_ASSERT_EQUAL_UINT16(0, acc.count);
}

void test_accumulate_rejects_non_finite_even_when_eligible(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  TEST_ASSERT_FALSE(accumulateCycleTemp(acc, NAN, /*eligible=*/true));
  TEST_ASSERT_FALSE(accumulateCycleTemp(acc, INFINITY, /*eligible=*/true));
  TEST_ASSERT_EQUAL_UINT16(0, acc.count);
}

void test_accumulate_tracks_min_max_and_count(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  float samples[5] = {20.0f, 22.0f, 19.5f, 23.0f, 21.0f};
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_TRUE(accumulateCycleTemp(acc, samples[i], /*eligible=*/true));
  }
  TEST_ASSERT_EQUAL_UINT16(5, acc.count);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, acc.minC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.0f, acc.maxC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.1f, acc.mean); // (20+22+19.5+23+21)/5 = 21.1
}

// M5 — PIÈGE explicite : une séquence COURTE ne distingue PAS moyenne
// incrémentale et somme/compteur (l'écart n'apparaît qu'après des dizaines de
// milliers d'échantillons, quand la somme flottante grossit au point que son
// ULP dépasse les increments de température). Ce test le DOCUMENTE : il doit
// rester vert que l'implémentation soit incrémentale OU somme/compteur — la
// preuve de M5 est ailleurs (test suivant).
void test_C9_short_sequence_does_not_distinguish_incremental_from_naive_sum(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  double sum = 0.0;
  int n = 60;
  for (int i = 0; i < n; i++) {
    float v = (i % 2 == 0) ? 20.00f : 20.02f;
    accumulateCycleTemp(acc, v, true);
    sum += v;
  }
  float trueMean = (float)(sum / n);
  // Écart minuscule sur une courte séquence : les DEUX formulations passeraient
  // ce seuil large. Ne PAS resserrer cette assertion pour "prouver" M5 ici —
  // c'est exactement le piège documenté par le plan.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, trueMean, acc.mean);
}

// M5 — LA preuve : sur une longue séquence (mode PoweredByFiltration, eau
// 24/7), la moyenne incrémentale reste précise à 0,001°C près quand une somme
// flottante remise en compteur dérive de plusieurs millièmes (l'ULP d'une
// somme ~1,2M dépasse les incréments de 0,02°C). Si `accumulateCycleTemp`
// redevient une somme/compteur, cette assertion doit rougir.
void test_C9_incremental_mean_stays_precise_over_long_sequence(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  double sum = 0.0;
  const int n = 60000; // ~41 jours a 1 ech/min — sous le plafond 65535 (M-C9-sat)
  for (int i = 0; i < n; i++) {
    float v = (i % 2 == 0) ? 20.00f : 20.02f;
    TEST_ASSERT_TRUE(accumulateCycleTemp(acc, v, true));
    sum += v;
  }
  double trueMean = sum / n;
  char msg[160];
  snprintf(msg, sizeof(msg),
           "mean=%.6f attendu~%.6f : la moyenne incrementale doit rester a "
           "moins de 0.001 du vrai calcul en double sur %d echantillons.",
           (double)acc.mean, trueMean, n);
  TEST_ASSERT_TRUE_MESSAGE(fabs((double)acc.mean - trueMean) < 0.001, msg);
}

void test_C9_counter_saturates_at_65535_without_overflow(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  for (int i = 0; i < 65535; i++) {
    TEST_ASSERT_TRUE(accumulateCycleTemp(acc, 20.0f, true));
  }
  TEST_ASSERT_EQUAL_UINT16(65535, acc.count);
  float meanAtSaturation = acc.mean;
  // Un échantillon de plus NE DOIT PAS faire déborder le compteur (65535 -> 0).
  TEST_ASSERT_TRUE(accumulateCycleTemp(acc, 30.0f, true));
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(65535, acc.count,
      "Le compteur uint16 a deborde (65535 + 1 -> 0) : bug repliement, meme "
      "famille que le repliement de millis() a 24,8 j deja rencontre sur ce depot.");
  TEST_ASSERT_EQUAL_FLOAT(meanAtSaturation, acc.mean); // figé, pas sur-pondéré
}

// =============================================================================
// bug-126 — isPlausibleReferenceTemp (M16 : bornes INCLUSES)
// =============================================================================
void test_M16_plausible_ref_temp_bounds_are_inclusive(void) {
  TEST_ASSERT_TRUE_MESSAGE(isPlausibleReferenceTemp(4.0f, 4.0f, 40.0f),
      "La borne basse est INCLUSE (kFiltrationRefTempMinC).");
  TEST_ASSERT_TRUE_MESSAGE(isPlausibleReferenceTemp(40.0f, 4.0f, 40.0f),
      "La borne haute est INCLUSE (kFiltrationRefTempMaxC).");
  TEST_ASSERT_FALSE(isPlausibleReferenceTemp(3.99f, 4.0f, 40.0f));
  TEST_ASSERT_FALSE(isPlausibleReferenceTemp(40.01f, 4.0f, 40.0f));
}

void test_plausible_ref_temp_rejects_nan_inf(void) {
  TEST_ASSERT_FALSE(isPlausibleReferenceTemp(NAN, 4.0f, 40.0f));
  TEST_ASSERT_FALSE(isPlausibleReferenceTemp(INFINITY, 4.0f, 40.0f));
  TEST_ASSERT_FALSE(isPlausibleReferenceTemp(-INFINITY, 4.0f, 40.0f));
}

// =============================================================================
// bug-126 — selectFiltrationRefTemp (M4 : repli de TOLÉRANCE uniquement, arbitrage B)
// =============================================================================
void test_C4_select_ref_temp_prefers_water_probe(void) {
  uint8_t roles[2] = {kRoleCircuit, kRoleWater};
  float temps[2] = {8.0f, 21.5f};
  float out = -999.0f;
  TEST_ASSERT_TRUE(selectFiltrationRefTemp(roles, temps, 2, out));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, out);
}

void test_C4_select_ref_temp_falls_back_to_unknown_when_no_water_no_circuit(void) {
  // Appareil neuf : aucun rôle attribué (Unknown partout).
  uint8_t roles[2] = {kRoleUnknown, kRoleUnknown};
  float temps[2] = {NAN, 19.3f};
  float out = -999.0f;
  TEST_ASSERT_TRUE(selectFiltrationRefTemp(roles, temps, 2, out));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.3f, out);
}

// M4 — LE scénario nommé : sonde eau NaN + sonde Circuit déclarée à 8°C. La
// fonction doit rendre false, JAMAIS retomber sur la température du boîtier.
// C'est la preuve de C4 : si le mutant réintroduit le comportement de
// getTemperature() (repli sur n'importe quelle sonde finie), ce test rougit.
void test_C4_never_falls_back_to_circuit_probe_even_with_nan_water(void) {
  uint8_t roles[2] = {kRoleWater, kRoleCircuit};
  float temps[2] = {NAN, 8.0f};
  float out = 12345.0f; // sentinelle : ne doit PAS être écrasée par 8.0f
  bool ok = selectFiltrationRefTemp(roles, temps, 2, out);
  TEST_ASSERT_FALSE_MESSAGE(ok,
      "Sonde eau NaN + sonde Circuit declaree : selectFiltrationRefTemp doit "
      "rendre false, jamais la temperature du boitier electronique (C4).");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(12345.0f, out,
      "outTempC ne doit PAS etre ecrit quand la fonction rend false (ADR-0072).");
}

void test_C4_select_ref_temp_all_nan_or_no_probe_returns_false(void) {
  uint8_t roles[2] = {kRoleUnknown, kRoleUnknown};
  float temps[2] = {NAN, NAN};
  float out = 42.0f;
  TEST_ASSERT_FALSE(selectFiltrationRefTemp(roles, temps, 2, out));
  TEST_ASSERT_EQUAL_FLOAT(42.0f, out);
  // count=0 : aucune sonde du tout.
  float out2 = 43.0f;
  TEST_ASSERT_FALSE(selectFiltrationRefTemp(roles, temps, 0, out2));
  TEST_ASSERT_EQUAL_FLOAT(43.0f, out2);
}

// =============================================================================
// bug-126 — resolveReferenceTemp (M6 : ADR-0072, M7 : bootstrap vs plausibilité,
// M8 : ordre des verdicts, + frontière 29/30 + amorçage)
// =============================================================================
void test_resolve_ref_temp_no_sample_when_count_zero(void) {
  CycleTempAccumulator acc;
  resetCycleTempAccumulator(acc);
  float out = -1.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RefTempVerdict::NoSample), static_cast<int>(v));
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, out); // M6 : jamais ecrit hors Ok
}

// Frontière EXACTE de représentativité, dans les deux sens (29 -> rien, 30 -> planning).
void test_resolve_ref_temp_threshold_29_too_short_30_ok(void) {
  CycleTempAccumulator acc29; resetCycleTempAccumulator(acc29);
  acc29.count = 29; acc29.mean = 22.0f; acc29.minC = 21.0f; acc29.maxC = 23.0f;
  float out29 = -2.0f;
  RefTempVerdict v29 = resolveReferenceTemp(acc29, kFiltrationMinRepresentativeSamples,
                                            /*bootstrapAllowed=*/false, 4.0f, 40.0f, out29);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::TooShort), static_cast<int>(v29),
      "29 echantillons < 30 (kFiltrationMinRepresentativeSamples) : TooShort attendu.");
  TEST_ASSERT_EQUAL_FLOAT(-2.0f, out29);

  CycleTempAccumulator acc30; resetCycleTempAccumulator(acc30);
  acc30.count = 30; acc30.mean = 22.0f; acc30.minC = 21.0f; acc30.maxC = 23.0f;
  float out30 = -3.0f;
  RefTempVerdict v30 = resolveReferenceTemp(acc30, kFiltrationMinRepresentativeSamples,
                                            /*bootstrapAllowed=*/false, 4.0f, 40.0f, out30);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::Ok), static_cast<int>(v30),
      "30 echantillons == seuil : Ok attendu (planning produit).");
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, out30);
}

void test_resolve_ref_temp_implausible_when_mean_out_of_bounds(void) {
  CycleTempAccumulator acc; resetCycleTempAccumulator(acc);
  acc.count = 50; acc.mean = 85.0f; acc.minC = 80.0f; acc.maxC = 90.0f;
  float out = -4.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RefTempVerdict::Implausible), static_cast<int>(v));
  // M6 : outTempC INTOUCHE sur un verdict != Ok — ADR-0072, l'absence n'est
  // jamais une valeur du domaine.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-4.0f, out,
      "outTempC a ete ecrit alors que le verdict n'est PAS Ok (ADR-0072).");
}

// M6 — la même garde, mais côté TooShort (pas seulement Implausible/NoSample) :
// aucune écriture d'outTempC tant que le verdict n'est pas Ok.
void test_M6_too_short_does_not_write_output_either(void) {
  CycleTempAccumulator acc; resetCycleTempAccumulator(acc);
  acc.count = 5; acc.mean = 22.0f; acc.minC = 21.0f; acc.maxC = 23.0f;
  float out = -5.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RefTempVerdict::TooShort), static_cast<int>(v));
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, out);
}

// M7 — amorçage : bootstrapAllowed=true, UN SEUL échantillon suffit à passer
// la garde de représentativité — MAIS les bornes de plausibilité s'appliquent
// quand même (elles ne dépendent PAS de bootstrapAllowed). Si le mutant fait
// sauter la plausibilité en même temps que la représentativité, ce test rougit.
void test_M7_bootstrap_bypasses_too_short_but_not_implausible(void) {
  // 1er sous-cas : un seul échantillon, PLAUSIBLE -> Ok malgré bootstrapAllowed.
  CycleTempAccumulator ok; resetCycleTempAccumulator(ok);
  ok.count = 1; ok.mean = 20.0f; ok.minC = 20.0f; ok.maxC = 20.0f;
  float outOk = -6.0f;
  RefTempVerdict vOk = resolveReferenceTemp(ok, 30, /*bootstrapAllowed=*/true, 4.0f, 40.0f, outOk);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::Ok), static_cast<int>(vOk),
      "Amorcage : 1 seul echantillon plausible doit suffire (bootstrapAllowed=true).");
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, outOk);

  // 2e sous-cas : un seul échantillon, IMPLAUSIBLE (85°C) -> Implausible QUAND
  // MEME, malgre bootstrapAllowed=true. C'est LA preuve de M7 : un mutant qui
  // ferait sauter aussi la plausibilite renverrait Ok ici, a tort.
  CycleTempAccumulator bad; resetCycleTempAccumulator(bad);
  bad.count = 1; bad.mean = 85.0f; bad.minC = 85.0f; bad.maxC = 85.0f;
  float outBad = -7.0f;
  RefTempVerdict vBad = resolveReferenceTemp(bad, 30, /*bootstrapAllowed=*/true, 4.0f, 40.0f, outBad);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::Implausible), static_cast<int>(vBad),
      "bootstrapAllowed ne doit JAMAIS faire sauter la plausibilite (M7).");
  TEST_ASSERT_EQUAL_FLOAT(-7.0f, outBad);
}

// M8 — ORDRE IMPOSÉ : NoSample AVANT TooShort et AVANT Implausible. count==0
// avec une moyenne (artificiellement) implausible et bootstrapAllowed=false
// (qui rendrait TooShort candidat aussi) : le verdict doit rester NoSample.
void test_M8_order_no_sample_wins_over_too_short_and_implausible(void) {
  CycleTempAccumulator acc;
  acc.count = 0;
  acc.mean = 999.0f;   // implausible, si évalué
  acc.minC = 999.0f; acc.maxC = 999.0f;
  float out = -8.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::NoSample), static_cast<int>(v),
      "count==0 doit rendre NoSample AVANT tout examen de TooShort/Implausible (ordre M8).");
  TEST_ASSERT_EQUAL_FLOAT(-8.0f, out);
}

// M8 — ORDRE IMPOSÉ : TooShort AVANT Implausible. count < seuil ET moyenne
// implausible simultanément : TooShort doit gagner (bootstrapAllowed=false).
void test_M8_order_too_short_wins_over_implausible(void) {
  CycleTempAccumulator acc;
  acc.count = 5; // < 30
  acc.mean = 999.0f; // implausible, si évalué avant TooShort
  acc.minC = 999.0f; acc.maxC = 999.0f;
  float out = -9.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::TooShort), static_cast<int>(v),
      "count(5) < seuil(30) doit rendre TooShort AVANT tout examen d'Implausible (ordre M8).");
  TEST_ASSERT_EQUAL_FLOAT(-9.0f, out);
}

// =============================================================================
// bug-126 — decideSchedulePromotion (M9 à M15 : une garde par mutation, cumulatives)
// =============================================================================

// Cascade : à chaque étape on corrige EXACTEMENT une garde et on vérifie que le
// verdict avance d'un cran dans l'ordre impose. Si UNE garde est retiree ou
// deplacee du switch de decideSchedulePromotion, une des sept assertions ci-
// dessous echoue — c'est la table de mapping bug-126 (§2 du plan) verifiee pas
// a pas (M9 pendingValid, M10 newDay, M11 alreadyPromotedToday, M12
// basisUnchanged, M13 modeIsAuto, M14 pumpRunning).
void test_promotion_cascading_guards_in_order(void) {
  bool pendingValid = false, newDay = false, alreadyPromoted = true,
       basisUnchanged = false, modeIsAuto = false, pumpRunning = true;

  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::NoPending),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "M9 pendingValid=false doit rendre NoPending, meme si tout le reste est defavorable.");

  pendingValid = true;
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::NotNewDay),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "M10 newDay=false doit rendre NotNewDay.");

  newDay = true;
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::AlreadyPromoted),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "M11 alreadyPromotedToday=true doit rendre AlreadyPromoted.");

  alreadyPromoted = false;
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::BasisChanged),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "M12 basisUnchanged=false doit rendre BasisChanged.");

  basisUnchanged = true;
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::NotAuto),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "M13 modeIsAuto=false doit rendre NotAuto.");

  modeIsAuto = true;
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::PumpRunning),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "M14 pumpRunning=true doit rendre PumpRunning.");

  pumpRunning = false;
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::Promote),
      static_cast<int>(decideSchedulePromotion(pendingValid, newDay, alreadyPromoted,
                                               basisUnchanged, modeIsAuto, pumpRunning)),
      "Toutes les gardes reunies : Promote.");
}

// Scénario nommé C5 (obligatoire) : pending produit en auto, mode passé en
// manual 22:00-06:00, pompe en marche à minuit. Deux variantes, JAMAIS Promote.
void test_C5_mode_changed_or_pump_running_overnight_never_promotes(void) {
  // Variante A : la base a changé (mode auto -> manual) => BasisChanged,
  // AVANT même de regarder l'état de la pompe.
  PromotionVerdict vA = decideSchedulePromotion(/*pendingValid=*/true, /*newDay=*/true,
                                                /*alreadyPromotedToday=*/false,
                                                /*basisUnchanged=*/false, // mode a change
                                                /*modeIsAuto=*/false,     // desormais manual
                                                /*pumpRunning=*/true);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::BasisChanged), static_cast<int>(vA),
      "Mode passe en manual : la base a change, le pending doit etre juge PERIME (BasisChanged).");

  // Variante B : la base est INTACTE (toujours auto, horaires inchangés) mais
  // la pompe tourne encore à minuit (forceOn manuel a cheval) => PumpRunning,
  // jamais Promote — la promotion est DIFFEREE, pas sautee.
  PromotionVerdict vB = decideSchedulePromotion(/*pendingValid=*/true, /*newDay=*/true,
                                                /*alreadyPromotedToday=*/false,
                                                /*basisUnchanged=*/true,
                                                /*modeIsAuto=*/true,
                                                /*pumpRunning=*/true);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(PromotionVerdict::PumpRunning), static_cast<int>(vB),
      "Base intacte + pompe en marche a minuit : PumpRunning (differe), jamais Promote.");
}

// =============================================================================
// bug-126 — computeAutoWindow : jamais de franchissement de minuit sur
// l'intervalle ADMIS [kFiltrationRefTempMinC, kFiltrationRefTempMaxC].
// Ferme les deux cas degeneres mesures (44°C franchit minuit, 48°C sature a
// start==end) : ils ne sont atteignables que HORS de l'intervalle admis, que
// isPlausibleReferenceTemp (M16) rejette en amont.
// =============================================================================
void test_auto_window_never_crosses_midnight_on_admitted_interval(void) {
  for (float t = kFiltrationRefTempMinC; t <= kFiltrationRefTempMaxC; t += 0.25f) {
    ScheduleWindow w = computeAutoWindow(t, kFiltrationPivotHour);
    char msg[96];
    snprintf(msg, sizeof(msg), "tempC=%.2f : startMin(%d) doit etre < endMin(%d)",
             (double)t, w.startMin, w.endMin);
    TEST_ASSERT_TRUE_MESSAGE(w.startMin < w.endMin, msg);
  }
  // Bornes exactes.
  ScheduleWindow wMin = computeAutoWindow(kFiltrationRefTempMinC, kFiltrationPivotHour);
  TEST_ASSERT_TRUE(wMin.startMin < wMin.endMin);
  ScheduleWindow wMax = computeAutoWindow(kFiltrationRefTempMaxC, kFiltrationPivotHour);
  TEST_ASSERT_TRUE(wMax.startMin < wMax.endMin);
}

// =============================================================================
// bug-126 — Sentinelles de SOURCE (ce que la logique pure ne peut pas prouver
// seule : que la coquille filtration.cpp / pump_controller.cpp est bien
// câblée). Patron repris de test_native_temp_compensation/test_temp_compensation.cpp:948.
// =============================================================================

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_schedule");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i == 0) {
      if (!rooted[0]) continue;
      snprintf(path, sizeof(path), "%s", rooted);
    } else if (i == 1) {
      snprintf(path, sizeof(path), "%s", rel);
    } else {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4000000L) { fclose(fp); continue; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(fp); continue; }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    *outLen = (long)got;
    return buf;
  }
  return NULL;
}

static int countOccurrences(const char* hay, const char* needle) {
  int n = 0;
  const char* p = hay;
  const size_t len = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) { ++n; p += len; }
  return n;
}

static char* extractFunctionBody(const char* src, const char* sig) {
  const char* p = strstr(src, sig);
  if (p == NULL) return NULL;
  const char* open = strchr(p, '{');
  if (open == NULL) return NULL;
  int depth = 0;
  const char* q = open;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++depth;
    else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
  }
  const size_t n = (size_t)(q - open);
  char* body = (char*)malloc(n + 1);
  if (body == NULL) return NULL;
  memcpy(body, open, n);
  body[n] = '\0';
  return body;
}

// Retire EN PLACE les commentaires C et C++ (cf. test_temp_compensation.cpp
// pour la justification : une sentinelle satisfaite par un COMMENTAIRE ne
// prouve rien — c'est arrivé sur ce dépôt).
static void stripCppComments(char* s) {
  bool inLine = false, inBlock = false, inStr = false, inChr = false;
  for (size_t i = 0; s[i] != '\0'; ++i) {
    if (inLine) {
      if (s[i] == '\n') inLine = false; else s[i] = ' ';
    } else if (inBlock) {
      if (s[i] == '*' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = false; }
      else if (s[i] != '\n') s[i] = ' ';
    } else if (inStr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '"') inStr = false;
    } else if (inChr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '\'') inChr = false;
    } else if (s[i] == '/' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inLine = true; }
    else if (s[i] == '/' && s[i + 1] == '*') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = true; }
    else if (s[i] == '"') inStr = true;
    else if (s[i] == '\'') inChr = true;
  }
}

static char* readProjectCode(const char* rel) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src != NULL) stripCppComments(src);
  return src;
}

// A4 — le défaut originel a disparu du source : ni le délai magique de 5 min,
// ni le champ retiré ne doivent réapparaître (même en commentaire mort).
void test_filtration_cpp_no_longer_contains_the_defect_literals(void) {
  long len = 0;
  char* src = readProjectFile("src/filtration.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/filtration.cpp INTROUVABLE.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(src, "300000UL"),
      "Le delai magique de 5 min (recalcul en marche, LE defaut bug-126) est de retour.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(src, "scheduleComputedThisCycle"),
      "Le champ scheduleComputedThisCycle (retire par bug-126) est de retour.");
  free(src);
}

// Arbitrage B — INTERDIT ABSOLU : sensors.getTemperature() ne doit apparaître
// que dans des COMMENTAIRES (ceux qui expliquent l'interdit), jamais comme
// appel réel. Sur le texte brut, la mention existe (deux commentaires) — la
// sentinelle doit donc dépouiller le code, pas juste chercher "0 occurrence".
void test_filtration_cpp_never_calls_get_temperature_only_mentions_it_in_comments(void) {
  long len = 0;
  char* raw = readProjectFile("src/filtration.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(raw, "src/filtration.cpp INTROUVABLE.");
  const int rawCount = countOccurrences(raw, "getTemperature()");
  TEST_ASSERT_TRUE_MESSAGE(rawCount >= 1,
      "Le texte BRUT devrait mentionner getTemperature() dans au moins un "
      "commentaire explicatif (sinon cette sentinelle est vide de sens).");
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL(code);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(code, "getTemperature()"),
      "filtration.cpp appelle reellement sensors.getTemperature() (hors "
      "commentaire) : retomberait sur une sonde Circuit = temperature du "
      "boitier electronique (arbitrage B, INTERDIT ABSOLU).");
  free(raw);
  free(code);
}

// Formulation exécutable du bug lui-même : aucune écriture de
// filtrationCfg.start/.end entre le front montant (démarrage) et le front
// descendant (arrêt) d'update(). C'est très exactement ce que faisait le
// bloc supprimé (filtration.cpp:237-251 avant fix).
void test_filtration_cpp_no_config_write_between_rising_and_falling_edge(void) {
  long len = 0;
  char* raw = readProjectFile("src/filtration.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(raw, "src/filtration.cpp INTROUVABLE.");
  char* code = readProjectCode("src/filtration.cpp"); // meme texte, commentaires depouilles
  char* body = extractFunctionBody(code, "void FiltrationManager::update()");
  TEST_ASSERT_NOT_NULL_MESSAGE(body,
      "FiltrationManager::update INTROUVABLE : sentinelle aveugle, la "
      "renommer ici si la fonction a change de signature.");

  const char* risingMarker = "if (!wasRunning && runTarget)";
  const char* fallingMarker = "if (runNow && !runTarget)";
  const char* riseAt = strstr(body, risingMarker);
  const char* fallAt = strstr(body, fallingMarker);
  TEST_ASSERT_NOT_NULL_MESSAGE(riseAt,
      "Marqueur de front montant introuvable : sentinelle aveugle.");
  TEST_ASSERT_NOT_NULL_MESSAGE(fallAt,
      "Marqueur de front descendant introuvable : sentinelle aveugle.");
  TEST_ASSERT_TRUE_MESSAGE(fallAt > riseAt,
      "Le front descendant doit textuellement suivre le front montant dans update().");

  const size_t midLen = (size_t)(fallAt - riseAt);
  char* mid = (char*)malloc(midLen + 1);
  memcpy(mid, riseAt, midLen);
  mid[midLen] = '\0';

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(mid, "filtrationCfg.start ="),
      "Ecriture de filtrationCfg.start ENTRE le demarrage et l'arret d'un "
      "cycle : c'est EXACTEMENT le defaut bug-126 (recalcul en marche).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(mid, "filtrationCfg.end ="),
      "Ecriture de filtrationCfg.end ENTRE le demarrage et l'arret d'un "
      "cycle : c'est EXACTEMENT le defaut bug-126 (recalcul en marche).");

  free(mid);
  free(body);
  free(code);
  free(raw);
}

// M15 — la promotion différée (pompe en marche) doit CONSERVER le pending,
// jamais le jeter. Seuls BasisChanged/NotAuto le jettent (table de mapping).
void test_M15_pump_running_defers_promotion_never_discards_pending(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  const char* caseStart = strstr(code, "case PromotionVerdict::PumpRunning:");
  TEST_ASSERT_NOT_NULL_MESSAGE(caseStart,
      "case PromotionVerdict::PumpRunning INTROUVABLE : sentinelle aveugle.");
  const char* caseEnd = strstr(caseStart, "case PromotionVerdict::BasisChanged:");
  TEST_ASSERT_NOT_NULL_MESSAGE(caseEnd,
      "case PromotionVerdict::BasisChanged INTROUVABLE (borne de fin de bloc) : sentinelle aveugle.");
  const size_t n = (size_t)(caseEnd - caseStart);
  char* block = (char*)malloc(n + 1);
  memcpy(block, caseStart, n);
  block[n] = '\0';
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(block, "_pending.valid = false"),
      "Le case PumpRunning jette le pending (_pending.valid = false) au lieu "
      "de le DIFFERER : un jour de planning serait perdu silencieusement (M15).");
  free(block);
  free(code);
}

// C1 — sentinelle de non-régression : pump_controller.cpp reste À L'ÉCART de
// bug-126. Rouvrir A6 (horizon de dosage aligné sur la fenêtre "effective")
// doit faire rougir la suite AVANT de changer un comportement chimique.
void test_C1_pump_controller_untouched_by_bug126_horizon_shortcut(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp INTROUVABLE.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(src, "effectiveDosingHorizon"),
      "pump_controller.cpp reference 'effectiveDosingHorizon' : l'horizon A6 a "
      "ete introduit sans repasser par pool-chemistry + code-reviewer (C1).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(src, "filtration.isRunning()"),
      "pump_controller.cpp appelle filtration.isRunning() : couplage neuf a la "
      "filtration hors du chemin existant remainingRangeMinutes() (C1).");
  free(src);
}

// =============================================================================
// bug-126 — SECONDE PASSE (revue pool-chemistry + code-reviewer, 8 correctifs)
// =============================================================================
// RefTempVerdict::TooCold (P2-A), garde anti-écrasement M4, garde haveTime M3,
// garde mode auto en tête de writePendingSchedule, reset accumulateur en
// begin() M2, mutex récursif M1, armement B1, budget d'octets B2, alerte de
// boot différée P2-C. Ordre imposé désormais :
//   NoSample -> TooShort -> TooCold -> Implausible.

// M18/M19 — TooCold existe ET prime sur Implausible. `mean < minC` échoue
// AUSSI isPlausibleReferenceTemp (même borne basse) : un mutant qui retire
// TooCold (M18) ou qui vérifie Implausible avant TooCold (M19) rend
// Implausible ici au lieu de TooCold — les deux mutations sont tuées par
// CE MÊME scénario, ce qui est attendu (ce sont deux formes de la même
// confusion hivernage/panne).
void test_M18_M19_mean_below_min_is_toocold_not_implausible(void) {
  CycleTempAccumulator acc; resetCycleTempAccumulator(acc);
  acc.count = 50; acc.mean = 2.0f; acc.minC = 1.5f; acc.maxC = 2.5f; // hivernage plausible
  float out = -10.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::TooCold), static_cast<int>(v),
      "mean=2.0 < kFiltrationRefTempMinC(4.0) : HIVERNAGE (TooCold), PAS une "
      "panne de sonde (Implausible) — P2-A. Un critical + alerte MQTT tout "
      "l'hiver apprendrait a l'utilisateur a ignorer le canal.");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-10.0f, out, "TooCold n'est pas Ok : outTempC ne doit pas etre ecrit (ADR-0072).");
}

// M20 — ORDRE IMPOSÉ : TooShort AVANT TooCold. count < seuil ET mean < minC
// simultanément (bootstrapAllowed=false) : TooShort doit gagner.
void test_M20_order_too_short_wins_over_too_cold(void) {
  CycleTempAccumulator acc; resetCycleTempAccumulator(acc);
  acc.count = 5; acc.mean = 2.0f; acc.minC = 1.5f; acc.maxC = 2.5f; // < 30 ET < minC
  float out = -11.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/false, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::TooShort), static_cast<int>(v),
      "count(5) < seuil(30) doit rendre TooShort AVANT tout examen de TooCold (ordre M20).");
  TEST_ASSERT_EQUAL_FLOAT(-11.0f, out);
}

// M21 — bootstrapAllowed ne doit PAS faire sauter TooCold (contrairement à
// TooShort, qu'il bypasse EXPRÈS pour l'amorçage). Un seul échantillon
// d'hivernage (bootstrapAllowed=true) reste TooCold, jamais Ok.
void test_M21_bootstrap_does_not_bypass_too_cold(void) {
  CycleTempAccumulator acc; resetCycleTempAccumulator(acc);
  acc.count = 1; acc.mean = 2.0f; acc.minC = 2.0f; acc.maxC = 2.0f;
  float out = -12.0f;
  RefTempVerdict v = resolveReferenceTemp(acc, 30, /*bootstrapAllowed=*/true, 4.0f, 40.0f, out);
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RefTempVerdict::TooCold), static_cast<int>(v),
      "bootstrapAllowed ne doit JAMAIS faire sauter TooCold (M21) : l'amorcage "
      "bypasse UNIQUEMENT le seuil de representativite (TooShort), jamais la "
      "plausibilite physique (TooCold/Implausible).");
  TEST_ASSERT_EQUAL_FLOAT(-12.0f, out);
}

// =============================================================================
// M17 — B1 : `_haveRepresentativeMeasure` doit être armé AVANT le test de
// fenêtre inchangée, PAS après. Sentinelle de SOURCE (le drapeau lui-même vit
// dans filtration.cpp, hors de portée de la logique pure) — mais ancrée sur
// une expression de CODE (`w.startMin == curStart && w.endMin == curEnd`),
// pas sur un libellé de log, pour rester robuste à une reformulation de
// doc-writer (suggestion de robustesse, revue).
//
// ⚠️ PIÈGE (documenté par la revue) : un test à un seul cycle ne distingue
// RIEN — que le drapeau soit armé avant ou après le retour anticipé, un cycle
// UNIQUE en régime établi ne révèle pas la différence, puisque aucun second
// cycle ne vient ensuite lire l'état du drapeau. La preuve n'existe QUE dans
// la position relative du texte source : c'est pourquoi ce test compare des
// OFFSETS de chaîne, pas un scénario à un cycle. Le scénario complet
// « plusieurs cycles à fenêtre identique puis un cycle tronqué à fenêtre
// différente » est IRREPRODUCTIBLE en logique pure (FiltrationManager n'est
// pas instanciable nativement) ; documenté ici en commentaire, prouvé en
// source ci-dessous.
// =============================================================================
void test_M17_have_representative_measure_armed_before_window_unchanged_check(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");

  const char* caseOkStart = strstr(code, "case RefTempVerdict::Ok:");
  TEST_ASSERT_NOT_NULL_MESSAGE(caseOkStart,
      "case RefTempVerdict::Ok INTROUVABLE : sentinelle aveugle (M17).");
  const char* nextCase = strstr(caseOkStart + strlen("case RefTempVerdict::Ok:"),
                                "case RefTempVerdict::");
  TEST_ASSERT_NOT_NULL_MESSAGE(nextCase,
      "case RefTempVerdict:: suivant introuvable (borne de fin de bloc, M17).");

  const size_t blockLen = (size_t)(nextCase - caseOkStart);
  char* block = (char*)malloc(blockLen + 1);
  memcpy(block, caseOkStart, blockLen);
  block[blockLen] = '\0';

  const char* armPos = strstr(block, "_haveRepresentativeMeasure = true");
  const char* windowCheckPos = strstr(block, "w.startMin == curStart && w.endMin == curEnd");
  TEST_ASSERT_NOT_NULL_MESSAGE(armPos,
      "Armement _haveRepresentativeMeasure introuvable dans le cas Ok (M17/B1).");
  TEST_ASSERT_NOT_NULL_MESSAGE(windowCheckPos,
      "Comparaison de fenetre introuvable dans le cas Ok (M17/B1).");
  TEST_ASSERT_TRUE_MESSAGE(armPos < windowCheckPos,
      "_haveRepresentativeMeasure DOIT etre arme AVANT le test de fenetre "
      "inchangee (B1). En regime etabli (meme fenetre chaque jour), le "
      "retour anticipe est le chemin NORMAL : arme APRES laisserait le "
      "drapeau perpetuellement a false, et un cycle tronque (< 30 ech.) "
      "pourrait ensuite ecrire un pending sur un seul echantillon — le "
      "critere d'acceptation 'un cycle ecourte ne pilote pas le lendemain' "
      "tomberait.");
  free(block);
  free(code);
}

// =============================================================================
// M22 — garde anti-écrasement (revue P2, correctif M4) : un pending DÉJÀ DÛ
// (computedOnDate antérieur à aujourd'hui, différé pour cause de pompe en
// marche) ne doit PAS être écrasé par writePendingSchedule() au même tour.
// Scénario nommé : pending daté d'hier, encore `valid`, promotion différée
// (pompe en marche à minuit) ; le cycle qui s'arrête ce même jour ne doit
// PAS réécrire par-dessus.
// =============================================================================
void test_M22_pending_due_yesterday_is_not_overwritten_same_day(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::writePendingSchedule(bool haveTime)");
  TEST_ASSERT_NOT_NULL_MESSAGE(body,
      "writePendingSchedule(bool) INTROUVABLE : sentinelle aveugle (M22). Si "
      "la signature a change, mettre a jour cette recherche.");

  const char* guardPos = strstr(body,
      "if (_pending.valid && shouldRolloverByDate(_pending.computedOnDate, todayStr))");
  TEST_ASSERT_NOT_NULL_MESSAGE(guardPos,
      "Garde anti-ecrasement (M4) introuvable : un pending deja du serait "
      "ecrase par la mesure du cycle qui vient de finir, et sa promotion "
      "differee n'aurait alors JAMAIS lieu (M22).");

  const char* resolvePos = strstr(body, "resolveReferenceTemp(");
  TEST_ASSERT_NOT_NULL_MESSAGE(resolvePos, "resolveReferenceTemp( introuvable dans writePendingSchedule.");
  TEST_ASSERT_TRUE_MESSAGE(guardPos < resolvePos,
      "La garde anti-ecrasement doit s'executer AVANT resolveReferenceTemp() "
      "(donc avant toute ecriture de _pending) — sinon la mesure du cycle "
      "courant a deja pu ecraser _pending avant que la garde ne s'applique.");

  // La garde doit RENDRE LA MAIN (return) sans toucher _pending — elle ne
  // fait que journaliser et sortir, jamais _pending.valid = false.
  const char* afterGuard = guardPos;
  const char* returnAfterGuard = strstr(afterGuard, "return;");
  const char* nextBraceCloseGuess = strstr(afterGuard, "}");
  TEST_ASSERT_NOT_NULL_MESSAGE(returnAfterGuard, "La garde anti-ecrasement doit se terminer par un return (M22).");
  // Le return doit arriver AVANT resolveReferenceTemp (donc dans le bloc de
  // la garde elle-même, pas plus loin dans la fonction).
  TEST_ASSERT_TRUE_MESSAGE(returnAfterGuard < resolvePos,
      "Le 'return' de la garde anti-ecrasement doit precede resolveReferenceTemp (M22).");
  (void)nextBraceCloseGuess;
  free(body);
  free(code);
}

// =============================================================================
// M23 — garde `haveTime` (revue P2, correctif M3) : sans heure valide,
// writePendingSchedule() doit sortir AVANT tout calcul de date — sinon
// `computedOnDate` vaudrait "19700101" (epoch), que shouldRolloverByDate lit
// comme "jour different" au premier passage a une date reelle -> promotion
// IMMEDIATE en pleine journee au lieu de minuit.
// =============================================================================
void test_M23_no_valid_time_writes_nothing_before_date_computation(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::writePendingSchedule(bool haveTime)");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "writePendingSchedule(bool) INTROUVABLE (M23).");

  const char* haveTimeGuard = strstr(body, "if (!haveTime)");
  TEST_ASSERT_NOT_NULL_MESSAGE(haveTimeGuard,
      "Garde 'if (!haveTime)' introuvable : sans heure valide, computedOnDate "
      "vaudrait 19700101 -> promotion immediate en pleine journee (M23/M3).");

  const char* localtimeCall = strstr(body, "localtime_r(&nowEpoch, &ti)");
  TEST_ASSERT_NOT_NULL_MESSAGE(localtimeCall,
      "localtime_r( introuvable : sentinelle aveugle (M23).");
  TEST_ASSERT_TRUE_MESSAGE(haveTimeGuard < localtimeCall,
      "La garde haveTime doit precede TOUT calcul de date (localtime_r) — "
      "sinon computedOnDate peut etre ecrit meme sans heure valide (M23).");
  free(body);
  free(code);
}

// =============================================================================
// M24 — garde de mode auto EN TÊTE de writePendingSchedule() : hors mode
// auto, aucun pending ne doit être produit (bruit de journal + armement
// erroné de _haveRepresentativeMeasure sur une mesure hors mode auto).
// =============================================================================
void test_M24_non_auto_mode_writes_nothing(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::writePendingSchedule(bool haveTime)");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "writePendingSchedule(bool) INTROUVABLE (M24).");

  const char* modeGuard = strstr(body,
      "if (!filtrationCfg.mode.equalsIgnoreCase(\"auto\")) return;");
  TEST_ASSERT_NOT_NULL_MESSAGE(modeGuard,
      "Garde de mode auto introuvable EN TETE de writePendingSchedule (M24).");

  const char* haveTimeGuard = strstr(body, "if (!haveTime)");
  const char* armPos = strstr(body, "_haveRepresentativeMeasure = true");
  TEST_ASSERT_NOT_NULL(haveTimeGuard);
  TEST_ASSERT_NOT_NULL(armPos);
  TEST_ASSERT_TRUE_MESSAGE(modeGuard < haveTimeGuard,
      "La garde de mode auto doit etre EN TETE, avant toute autre garde (M24).");
  TEST_ASSERT_TRUE_MESSAGE(modeGuard < armPos,
      "La garde de mode auto doit precede l'armement du drapeau d'amorcage (M24).");
  free(body);
  free(code);
}

// =============================================================================
// M25 — begin() doit remettre l'accumulateur à l'état vide AVANT tout usage :
// sans resetCycleTempAccumulator(), `_cycleTemp` (non `= {}`, revue P2)
// contiendrait des octets non initialisés interprétés comme mean=?/count=?,
// violant l'invariant « NaN de mean <=> count == 0 » — et un `mean` proche de
// 0.0°C serait une valeur d'apparence PHYSIQUE, hors bornes basses, qui
// declencherait une alerte a tort.
// =============================================================================
void test_M25_begin_resets_cycle_temp_accumulator(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::begin()");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "FiltrationManager::begin INTROUVABLE (M25).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "resetCycleTempAccumulator(_cycleTemp)") >= 1,
      "begin() n'appelle plus resetCycleTempAccumulator(_cycleTemp) : "
      "_cycleTemp (sans '= {}') resterait a l'etat indefini, en violation de "
      "l'invariant NaN<=>count==0 (M25).");
  free(body);
  free(code);
}

// =============================================================================
// M26 — timeout configMutex : le pending doit être CONSERVÉ et RETENTÉ au
// tour suivant, jamais abandonné.
// =============================================================================
void test_M26_mutex_timeout_keeps_pending_for_retry(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::promotePendingScheduleIfDue");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "promotePendingScheduleIfDue INTROUVABLE (M26).");

  const char* takePos = strstr(body,
      "if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE)");
  TEST_ASSERT_NOT_NULL_MESSAGE(takePos,
      "Prise du mutex recursif introuvable dans le cas Promote (M1/M26).");
  const char* blockOpen = strchr(takePos, '{');
  TEST_ASSERT_NOT_NULL(blockOpen);
  int depth = 0; const char* q = blockOpen;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++depth;
    else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
  }
  const size_t n = (size_t)(q - blockOpen);
  char* timeoutBlock = (char*)malloc(n + 1);
  memcpy(timeoutBlock, blockOpen, n);
  timeoutBlock[n] = '\0';

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(timeoutBlock, "_pending.valid = false"),
      "Le bloc de timeout mutex jette le pending au lieu de le CONSERVER pour "
      "retentative (M26) : un jour de planning serait perdu silencieusement "
      "sur une simple contention momentanee du mutex.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(timeoutBlock, "break;") >= 1,
      "Le bloc de timeout doit sortir par 'break' (pending conserve, retente "
      "au tour suivant), pas par un chemin qui consommerait le pending (M26).");
  free(timeoutBlock);
  free(body);
  free(code);
}

// =============================================================================
// P2-C — alerte de boot différée : begin() (avant mqttManager.begin(),
// outQueue==nullptr) arme un drapeau CONSOMMÉ plus tard par update() une fois
// MQTT connecté. Sans cela l'alerte 'filtration_schedule_invalid' émise au
// boot partirait dans le vide (publishAlert() = no-op silencieux tant que
// outQueue n'existe pas).
// =============================================================================
void test_P2C_boot_repair_alert_is_deferred_not_emitted_directly(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");

  char* ensureBody = extractFunctionBody(code, "void FiltrationManager::ensureTimesValid()");
  TEST_ASSERT_NOT_NULL_MESSAGE(ensureBody, "ensureTimesValid INTROUVABLE (P2-C).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(ensureBody, "publishAlert("),
      "ensureTimesValid() appelle publishAlert() DIRECTEMENT : begin() precede "
      "mqttManager.begin() (outQueue==nullptr), l'alerte partirait dans le "
      "vide (P2-C, meme piege que main.cpp pour l'alerte history).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(ensureBody, "_scheduleRepairAlertPending = true") >= 1,
      "ensureTimesValid() n'arme plus le drapeau differe _scheduleRepairAlertPending (P2-C).");
  free(ensureBody);

  char* updateBody = extractFunctionBody(code, "void FiltrationManager::update()");
  TEST_ASSERT_NOT_NULL_MESSAGE(updateBody, "FiltrationManager::update INTROUVABLE (P2-C).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(updateBody, "_scheduleRepairAlertPending") >= 1,
      "update() ne consulte plus _scheduleRepairAlertPending : l'alerte de "
      "reparation au BOOT ne serait jamais publiee (P2-C).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(updateBody, "mqttManager.isConnected()") >= 1,
      "update() doit attendre mqttManager.isConnected() avant de publier "
      "l'alerte differee (P2-C) — sinon publishAlert() serait a nouveau "
      "appele avant que outQueue n'existe.");
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(updateBody, "publishAlert(\"filtration_schedule_invalid\"") >= 1,
      "update() ne publie plus l'alerte filtration_schedule_invalid differee (P2-C).");
  free(updateBody);
  free(code);
}

// =============================================================================
// B2 — budget d'octets des DEUX alertes MQTT (pool-chemistry, nommément).
// Calcule la taille RÉELLE en octets UTF-8 (pas en caractères : é et ° comptent
// CHACUN 2 octets) de l'enveloppe JSON complète {"type":...,"message":...,
// "timestamp":...} au PIRE CAS, et vérifie qu'elle tient sous
// kMaxPayloadLen-1 (mqtt_manager.h:358, recopié ici car mqtt_manager.h exige
// Arduino.h — verrouillé par une sentinelle qui grep la vraie constante).
// =============================================================================
void test_B2_alert_messages_fit_mqtt_payload_budget(void) {
  const size_t kMaxPayloadLenLocal = 128; // mqtt_manager.h:358 — voir garde ci-dessous

  {
    long len = 0;
    char* mqttHeader = readProjectFile("src/mqtt_manager.h", &len);
    TEST_ASSERT_NOT_NULL_MESSAGE(mqttHeader, "src/mqtt_manager.h INTROUVABLE.");
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(mqttHeader, "kMaxPayloadLen = 128") >= 1,
        "kMaxPayloadLen a change de valeur reelle dans mqtt_manager.h : mettre "
        "a jour kMaxPayloadLenLocal dans CE test (B2) — sinon le budget verifie "
        "n'est plus le budget reel.");
    free(mqttHeader);
  }

  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");

  // --- Message 1 : filtration_schedule_invalid (litteral fixe, sans valeur numerique) ---
  const char* schedInvalidMsg = "Plage auto nulle : repli 08:00-20:00";
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(code, schedInvalidMsg) >= 1,
      "Message 'filtration_schedule_invalid' introuvable TEL QUEL dans le "
      "source : la formulation a change, revalider le budget d'octets (B2) "
      "avant de mettre a jour ce test.");

  // --- Message 2 : filtration_temp_implausible (prefixe + valeur + suffixe) ---
  // DEUX sites d'emission INDEPENDANTS : computeAutoSchedule() (mise en
  // service) et writePendingSchedule() (cycle qui vient de finir). Un decor
  // demasque pendant la validation par mutation : vérifier la presence du
  // fragment sur le FICHIER ENTIER ne suffit pas — allonger le message d'UN
  // SEUL des deux sites laissait le test vert, l'autre site (inchange)
  // satisfaisant la recherche globale à sa place. Chaque site est donc
  // extrait et vérifié SEUL.
  const char* implausiblePrefix = "Réf. ";
  const char* implausibleSuffix = "°C hors bornes : planning conservé";
  const char* implausibleSites[2] = {
      "void FiltrationManager::computeAutoSchedule()",
      "void FiltrationManager::writePendingSchedule(bool haveTime)",
  };
  for (int i = 0; i < 2; i++) {
    char* siteBody = extractFunctionBody(code, implausibleSites[i]);
    char msg[192];
    snprintf(msg, sizeof(msg), "%s INTROUVABLE : sentinelle aveugle (B2).", implausibleSites[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(siteBody, msg);
    snprintf(msg, sizeof(msg), "Prefixe du message filtration_temp_implausible absent de %s (B2).",
             implausibleSites[i]);
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(siteBody, implausiblePrefix) >= 1, msg);
    snprintf(msg, sizeof(msg), "Suffixe du message filtration_temp_implausible absent de %s (B2).",
             implausibleSites[i]);
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(siteBody, implausibleSuffix) >= 1, msg);
    free(siteBody);
  }
  free(code);

  // Pire cas numerique : String(t,1) pour une sonde DS18B20 debranchee lit
  // -127.0°C (code fournisseur) — 6 caracteres (signe + 3 chiffres + point +
  // decimale), le format le plus long plausible de String(float,1) ici.
  char worstImplausibleMsg[128];
  snprintf(worstImplausibleMsg, sizeof(worstImplausibleMsg), "%s-127.0%s",
           implausiblePrefix, implausibleSuffix);

  const char* types[2] = {"filtration_schedule_invalid", "filtration_temp_implausible"};
  const char* msgs[2] = {schedInvalidMsg, worstImplausibleMsg};
  for (int i = 0; i < 2; i++) {
    char payload[256];
    // Reproduit EXACTEMENT l'enveloppe de MqttManager::publishAlert()
    // (mqtt_manager.cpp:736) : {"type":"...","message":"...","timestamp":N}.
    // Timestamp = UINT32_MAX (10 chiffres) : pire cas de millis().
    snprintf(payload, sizeof(payload),
             "{\"type\":\"%s\",\"message\":\"%s\",\"timestamp\":4294967295}", types[i], msgs[i]);
    const size_t payloadLen = strlen(payload); // OCTETS (UTF-8), pas caracteres
    char msg[256];
    snprintf(msg, sizeof(msg),
             "%s : enveloppe JSON = %zu o (budget = %zu o = kMaxPayloadLen-1). "
             "Une reformulation a fait deborder le budget MQTT (B2) : le "
             "message serait tronque cote transport.",
             types[i], payloadLen, kMaxPayloadLenLocal - 1);
    TEST_ASSERT_TRUE_MESSAGE(payloadLen <= kMaxPayloadLenLocal - 1, msg);
  }
}

// =============================================================================
// Robustesse (suggestion de revue) : l'assertion symétrique sur
// sampleCycleTemperature() — aucune écriture de filtrationCfg.start/.end n'y
// est glissée. L'ancienne sentinelle (test_native_temp_compensation) ne
// couvrait que le texte ENTRE les marqueurs de update(), pas les fonctions
// appelées depuis cette zone.
// =============================================================================
void test_sample_cycle_temperature_never_writes_filtration_config(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::sampleCycleTemperature");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "sampleCycleTemperature INTROUVABLE.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "filtrationCfg.start ="),
      "sampleCycleTemperature ecrit filtrationCfg.start : le defaut bug-126 "
      "se rouvrirait par une fonction APPELEE depuis la zone marche, hors de "
      "portee de la sentinelle sur le texte d'update() seul.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "filtrationCfg.end ="),
      "sampleCycleTemperature ecrit filtrationCfg.end (meme risque).");
  free(body);
  free(code);
}

// =============================================================================
// M27 — variante SAISONNIÈRE de B1 (contre-revue code-reviewer) : sur
// `RefTempVerdict::TooCold`, `_haveRepresentativeMeasure` doit être armé, mais
// SEULEMENT si `_cycleTemp.count >= kFiltrationMinRepresentativeSamples`.
//
// Raisonnement : tant que le drapeau est `false`, `bootstrapAllowed` vaut
// `true`, donc `TooShort` est SAUTÉ — un seul échantillon suffit à atteindre
// `TooCold`. Un appareil mis en service l'hiver n'armerait donc JAMAIS le
// drapeau (verdict toujours `TooCold`, jamais `Ok`), et le PREMIER cycle de
// printemps pourrait fixer le planning sur un échantillon unique — exactement
// le défaut B1, dans une autre saison.
//
// ⚠️ PIÈGE (même famille que M17, signalé par la revue) : un scénario à cycle
// unique ne distingue RIEN — que l'armement soit inconditionnel ou gardé par
// le compte, un SEUL cycle hivernal ne révèle pas la différence si son compte
// dépasse déjà le seuil. La séquence complète « plusieurs cycles hivernaux
// représentatifs PUIS un cycle court de printemps à fenêtre différente » est
// IRREPRODUCTIBLE en logique pure (FiltrationManager n'est pas instanciable
// nativement, comme pour M17) : la preuve vit dans la POSITION et la GARDE du
// texte source, pas dans un scénario simulé qui ne testerait que mon propre
// miroir de la logique, pas le code réel.
// =============================================================================
void test_M27_have_representative_measure_armed_on_toocold_only_if_count_reaches_threshold(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");

  const char* caseStart = strstr(code, "case RefTempVerdict::TooCold:");
  TEST_ASSERT_NOT_NULL_MESSAGE(caseStart,
      "case RefTempVerdict::TooCold introuvable : sentinelle aveugle (M27).");
  const char* nextCase = strstr(caseStart + strlen("case RefTempVerdict::TooCold:"),
                                "case RefTempVerdict::");
  TEST_ASSERT_NOT_NULL_MESSAGE(nextCase,
      "case RefTempVerdict:: suivant introuvable (borne de fin de bloc, M27).");
  const size_t blockLen = (size_t)(nextCase - caseStart);
  char* block = (char*)malloc(blockLen + 1);
  memcpy(block, caseStart, blockLen);
  block[blockLen] = '\0';

  // 1) L'armement doit exister dans ce cas (kill : "retour à l'état d'avant
  //    le correctif", c'est-à-dire aucun armement du tout sur TooCold).
  const char* armPos = strstr(block, "_haveRepresentativeMeasure = true");
  TEST_ASSERT_NOT_NULL_MESSAGE(armPos,
      "TooCold n'arme plus _haveRepresentativeMeasure : un appareil mis en "
      "service l'hiver ne l'armerait JAMAIS (bootstrapAllowed resterait vrai, "
      "TooShort resterait saute), et le premier cycle de printemps pourrait "
      "fixer le planning sur un seul echantillon (M27, variante saisonniere "
      "de B1).");

  // 2) ... mais SEULEMENT gardé par le compte — sinon un cycle hivernal COURT
  //    (< seuil) armerait A TORT (la "variante a couvrir aussi" du plan) :
  //    on aurait juste deplace le probleme au lieu de le fermer.
  const char* guardPos = strstr(block,
      "if (_cycleTemp.count >= kFiltrationMinRepresentativeSamples)");
  TEST_ASSERT_NOT_NULL_MESSAGE(guardPos,
      "Garde de comptage introuvable devant l'armement sur TooCold (M27) : "
      "sans elle, un cycle hivernal COURT (< 30 ech.) armerait le drapeau a "
      "tort — meme defaut que M27, deplace au lieu d'etre ferme.");
  TEST_ASSERT_TRUE_MESSAGE(guardPos < armPos,
      "La garde de comptage doit precede textuellement l'armement (M27).");

  // L'armement doit être A L'INTERIEUR du bloc du if (pas seulement APRES,
  // sans lien réel) : borne la fin du bloc par appariement d'accolades.
  const char* ifOpenBrace = strchr(guardPos, '{');
  TEST_ASSERT_NOT_NULL_MESSAGE(ifOpenBrace, "Bloc du if de garde introuvable (M27).");
  int depth = 0;
  const char* q = ifOpenBrace;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++depth;
    else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
  }
  TEST_ASSERT_TRUE_MESSAGE(armPos < q,
      "_haveRepresentativeMeasure = true doit etre A L'INTERIEUR du bloc "
      "'if (_cycleTemp.count >= kFiltrationMinRepresentativeSamples)' — sinon "
      "l'armement serait en realite INCONDITIONNEL malgre la presence du if "
      "juste au-dessus (M27, variante 'cycle court arme a tort').");
  free(block);

  // 3) Implausible ne doit JAMAIS armer, quel que soit le compte : cette
  //    mesure-là n'est pas digne de foi (contrairement à TooCold, qui dit
  //    juste que l'eau est froide).
  const char* implausibleStart = strstr(code, "case RefTempVerdict::Implausible:");
  TEST_ASSERT_NOT_NULL_MESSAGE(implausibleStart,
      "case RefTempVerdict::Implausible introuvable : sentinelle aveugle (M27).");
  const char* afterImplausible = strstr(
      implausibleStart + strlen("case RefTempVerdict::Implausible:"), "case RefTempVerdict::");
  TEST_ASSERT_NOT_NULL_MESSAGE(afterImplausible,
      "case RefTempVerdict:: suivant introuvable apres Implausible (M27).");
  const size_t implausibleLen = (size_t)(afterImplausible - implausibleStart);
  char* implausibleBlock = (char*)malloc(implausibleLen + 1);
  memcpy(implausibleBlock, implausibleStart, implausibleLen);
  implausibleBlock[implausibleLen] = '\0';
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(implausibleBlock, "_haveRepresentativeMeasure"),
      "Implausible arme (ou consulte) _haveRepresentativeMeasure : cette "
      "mesure n'est PAS digne de foi, elle ne doit jamais armer, quel que "
      "soit le compte (M27).");
  free(implausibleBlock);
  free(code);
}

// =============================================================================
// Corrections mécaniques signalées par le coordinateur (troisième passe) —
// deux méritent une sentinelle légère, une non :
//   - R1 (throttle du log de timeout mutex) : gardée, cause reelle
//     d'operation degradee (eviction du tampon circulaire du journal PENDANT
//     l'incident meme ou il servirait), cout de test negligeable.
//   - Capture du journal DANS la section critique : gardee, c'est une
//     correction de RACE reelle (meme famille que le bug refreshTopics deja
//     corrige dans ce depot), pas une reformulation de log.
//   - R2 (initialiseur explicite de _cycleTemp) : PAS de sentinelle dediee.
//     Le correctif rend un no-op explicite (objet a duree de stockage
//     statique deja zero-initialise), mais l'invariant OBSERVABLE
//     (mean NaN <=> count==0 au premier usage) est deja garanti par
//     resetCycleTempAccumulator(_cycleTemp) en tete de begin() — deja couvert
//     par test_M25_begin_resets_cycle_temp_accumulator. Ajouter un test sur
//     l'initialiseur lui-meme testerait un detail d'implementation qui n'a
//     plus d'effet observable independant (le principe du projet : ne pas
//     tester l'implementation plutot que le comportement).
// =============================================================================

// R1 — le timeout configMutex de la promotion est throttlé (sinon : log à
// CHAQUE tour de loop() tant que handleSaveConfig() détient configMutex,
// donc éviction du tampon circulaire du journal pendant l'incident).
void test_R1_promote_mutex_timeout_log_is_throttled(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void FiltrationManager::promotePendingScheduleIfDue");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "promotePendingScheduleIfDue INTROUVABLE (R1).");

  const char* takePos = strstr(body,
      "if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE)");
  TEST_ASSERT_NOT_NULL_MESSAGE(takePos, "Prise du mutex recursif introuvable (R1).");
  const char* blockOpen = strchr(takePos, '{');
  TEST_ASSERT_NOT_NULL(blockOpen);
  int depth = 0; const char* q = blockOpen;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++depth;
    else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
  }
  const size_t n = (size_t)(q - blockOpen);
  char* timeoutBlock = (char*)malloc(n + 1);
  memcpy(timeoutBlock, blockOpen, n);
  timeoutBlock[n] = '\0';

  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(timeoutBlock, "kMutexTimeoutWarnThrottleMs") >= 1,
      "Le log de timeout configMutex n'est plus throttle (R1) : l'etat qui "
      "provoque l'echec persiste par construction (newDay && !alreadyPromoted "
      "&& basisUnchanged && auto && !running reste vrai au tour suivant), donc "
      "log a CHAQUE tour de loop() -> eviction du tampon circulaire du "
      "journal pendant l'incident meme ou il servirait.");
  free(timeoutBlock);
  free(body);
  free(code);
}

// Capture du journal de promotion DANS la section critique : relire
// filtrationCfg.start/.end APRÈS xSemaphoreGiveRecursive ferait journaliser la
// valeur d'une écriture concurrente — une AUTRE fenêtre que celle réellement
// promue. Même famille de défaut que le bug refreshTopics déjà corrigé.
void test_promote_log_captures_window_inside_critical_section(void) {
  char* code = readProjectCode("src/filtration.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/filtration.cpp INTROUVABLE.");
  const char* caseStart = strstr(code, "case PromotionVerdict::Promote:");
  TEST_ASSERT_NOT_NULL_MESSAGE(caseStart,
      "case PromotionVerdict::Promote introuvable : sentinelle aveugle.");
  const char* caseEnd = strstr(caseStart, "case PromotionVerdict::PumpRunning:");
  TEST_ASSERT_NOT_NULL_MESSAGE(caseEnd,
      "case PromotionVerdict::PumpRunning introuvable (borne de fin de bloc).");
  const size_t blockLen = (size_t)(caseEnd - caseStart);
  char* block = (char*)malloc(blockLen + 1);
  memcpy(block, caseStart, blockLen);
  block[blockLen] = '\0';

  const char* givePos = strstr(block, "xSemaphoreGiveRecursive(configMutex)");
  TEST_ASSERT_NOT_NULL_MESSAGE(givePos, "xSemaphoreGiveRecursive introuvable dans le cas Promote.");
  const char* captureStart = strstr(block, "String promuStart = filtrationCfg.start;");
  const char* captureEnd = strstr(block, "String promuEnd = filtrationCfg.end;");
  TEST_ASSERT_NOT_NULL_MESSAGE(captureStart,
      "Capture 'promuStart' introuvable : le journal risque de relire "
      "filtrationCfg.start APRES la liberation du mutex (race, meme famille "
      "que le bug refreshTopics deja corrige).");
  TEST_ASSERT_NOT_NULL_MESSAGE(captureEnd, "Capture 'promuEnd' introuvable (meme risque).");
  TEST_ASSERT_TRUE_MESSAGE(captureStart < givePos && captureEnd < givePos,
      "La capture de promuStart/promuEnd doit avoir lieu AVANT "
      "xSemaphoreGiveRecursive (DANS la section critique) — sinon le journal "
      "peut rapporter une fenetre issue d'une ecriture concurrente, pas celle "
      "reellement promue.");

  // Le log doit utiliser les variables CAPTUREES, pas relire filtrationCfg
  // directement après le Give.
  const char* afterGive = givePos;
  const char* logCall = strstr(afterGive, "systemLogger.info(\"[Filtration] Planning promu");
  TEST_ASSERT_NOT_NULL_MESSAGE(logCall, "Log de promotion introuvable apres le Give.");
  const char* logLineEnd = strstr(logCall, ";");
  TEST_ASSERT_NOT_NULL(logLineEnd);
  const size_t logLen = (size_t)(logLineEnd - logCall);
  char* logLine = (char*)malloc(logLen + 1);
  memcpy(logLine, logCall, logLen);
  logLine[logLen] = '\0';
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(logLine, "filtrationCfg.start"),
      "Le log de promotion relit filtrationCfg.start APRES le Give (au lieu "
      "de la variable capturee promuStart) : la meme race reapparait.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(logLine, "filtrationCfg.end"),
      "Le log de promotion relit filtrationCfg.end APRES le Give (meme risque).");
  free(logLine);
  free(block);
  free(code);
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

  // feature-011 — remainingRangeMinutes (horizon scheduled borné à minuit)
  RUN_TEST(test_F011_remaining_simple_range);
  RUN_TEST(test_F011_remaining_out_of_range_zero);
  RUN_TEST(test_F011_remaining_midnight_range);
  RUN_TEST(test_F011_remaining_invalid_zero);
  RUN_TEST(test_F011_remaining_boundaries);

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

  // feature-053 — decideFiltrationRun boostForce (priorité maximale)
  RUN_TEST(test_F053_boost_force_beats_force_off);
  RUN_TEST(test_F053_boost_force_beats_everything);
  RUN_TEST(test_F053_boost_force_false_no_regression);

  // feature-040 AC1/AC4 — decideLightingOn
  RUN_TEST(test_lighting_manual_override_returns_enabled_flag);
  RUN_TEST(test_lighting_schedule_disabled_returns_enabled_flag);
  RUN_TEST(test_lighting_schedule_follows_range_when_have_time);
  RUN_TEST(test_lighting_schedule_no_time_keeps_current_state);

  // bug-126 — CycleTempAccumulator / isSampleEligible / accumulateCycleTemp (C2/C9)
  RUN_TEST(test_reset_cycle_temp_accumulator_is_nan_and_zero);
  RUN_TEST(test_sample_eligible_truth_table_all_eight_combinations);
  RUN_TEST(test_C2_force_on_nocturnal_samples_all_rejected_by_in_schedule_guard);
  RUN_TEST(test_C2_sample_before_purge_settled_is_rejected);
  RUN_TEST(test_C2_frozen_probe_sample_is_rejected);
  RUN_TEST(test_accumulate_rejects_non_finite_even_when_eligible);
  RUN_TEST(test_accumulate_tracks_min_max_and_count);
  RUN_TEST(test_C9_short_sequence_does_not_distinguish_incremental_from_naive_sum);
  RUN_TEST(test_C9_incremental_mean_stays_precise_over_long_sequence);
  RUN_TEST(test_C9_counter_saturates_at_65535_without_overflow);

  // bug-126 — isPlausibleReferenceTemp (M16)
  RUN_TEST(test_M16_plausible_ref_temp_bounds_are_inclusive);
  RUN_TEST(test_plausible_ref_temp_rejects_nan_inf);

  // bug-126 — selectFiltrationRefTemp (M4, arbitrage B)
  RUN_TEST(test_C4_select_ref_temp_prefers_water_probe);
  RUN_TEST(test_C4_select_ref_temp_falls_back_to_unknown_when_no_water_no_circuit);
  RUN_TEST(test_C4_never_falls_back_to_circuit_probe_even_with_nan_water);
  RUN_TEST(test_C4_select_ref_temp_all_nan_or_no_probe_returns_false);

  // bug-126 — resolveReferenceTemp (M6, M7, M8, seuil 29/30, amorçage)
  RUN_TEST(test_resolve_ref_temp_no_sample_when_count_zero);
  RUN_TEST(test_resolve_ref_temp_threshold_29_too_short_30_ok);
  RUN_TEST(test_resolve_ref_temp_implausible_when_mean_out_of_bounds);
  RUN_TEST(test_M6_too_short_does_not_write_output_either);
  RUN_TEST(test_M7_bootstrap_bypasses_too_short_but_not_implausible);
  RUN_TEST(test_M8_order_no_sample_wins_over_too_short_and_implausible);
  RUN_TEST(test_M8_order_too_short_wins_over_implausible);

  // bug-126 — decideSchedulePromotion (M9-M14 cascade + scénario nommé C5)
  RUN_TEST(test_promotion_cascading_guards_in_order);
  RUN_TEST(test_C5_mode_changed_or_pump_running_overnight_never_promotes);

  // bug-126 — computeAutoWindow, intervalle admis (ferme les cas 44°C/48°C)
  RUN_TEST(test_auto_window_never_crosses_midnight_on_admitted_interval);

  // bug-126 — Sentinelles de source (filtration.cpp, pump_controller.cpp)
  RUN_TEST(test_filtration_cpp_no_longer_contains_the_defect_literals);
  RUN_TEST(test_filtration_cpp_never_calls_get_temperature_only_mentions_it_in_comments);
  RUN_TEST(test_filtration_cpp_no_config_write_between_rising_and_falling_edge);
  RUN_TEST(test_M15_pump_running_defers_promotion_never_discards_pending);
  RUN_TEST(test_C1_pump_controller_untouched_by_bug126_horizon_shortcut);

  // bug-126 — seconde passe (revue) : TooCold (M18-M21)
  RUN_TEST(test_M18_M19_mean_below_min_is_toocold_not_implausible);
  RUN_TEST(test_M20_order_too_short_wins_over_too_cold);
  RUN_TEST(test_M21_bootstrap_does_not_bypass_too_cold);

  // bug-126 — seconde passe : sentinelles de source (M17, M22-M26, P2-C, B2)
  RUN_TEST(test_M17_have_representative_measure_armed_before_window_unchanged_check);
  RUN_TEST(test_M22_pending_due_yesterday_is_not_overwritten_same_day);
  RUN_TEST(test_M23_no_valid_time_writes_nothing_before_date_computation);
  RUN_TEST(test_M24_non_auto_mode_writes_nothing);
  RUN_TEST(test_M25_begin_resets_cycle_temp_accumulator);
  RUN_TEST(test_M26_mutex_timeout_keeps_pending_for_retry);
  RUN_TEST(test_P2C_boot_repair_alert_is_deferred_not_emitted_directly);
  RUN_TEST(test_B2_alert_messages_fit_mqtt_payload_budget);
  RUN_TEST(test_sample_cycle_temperature_never_writes_filtration_config);

  // bug-126 — troisieme passe (contre-revue) : M27
  RUN_TEST(test_M27_have_representative_measure_armed_on_toocold_only_if_count_reaches_threshold);
  RUN_TEST(test_R1_promote_mutex_timeout_log_is_throttled);
  RUN_TEST(test_promote_log_captures_window_inside_critical_section);

  return UNITY_END();
}
