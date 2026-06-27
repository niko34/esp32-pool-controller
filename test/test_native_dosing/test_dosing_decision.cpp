// =============================================================================
// Tests unitaires natifs — Décision de dosage PURE (feature-036)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / I²C / FreeRTOS.
// On teste le COMPORTEMENT observable de la logique de décision extraite dans
// src/dosing_logic.{h,cpp} via son API publique (evaluateDose,
// shouldStartDosingPure, shouldContinueDosingPure), pas l'implémentation.
//
// INVARIANT (characterization refactor) : ces tests verrouillent l'ordre des
// gardes 1→15 de canDose() et les seuils d'hystérésis. Tout écart = régression.
//
// Config de référence calquée sur les valeurs réelles du projet :
//   pH  : requiredPoints=2, startThreshold deadband=0.05, stopThreshold=0.01,
//         minInjectionTimeMs=30000, maxCyclesPerDay=20.
// =============================================================================

#include <unity.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include "dosing_logic.h"

void setUp(void) {}
void tearDown(void) {}

// --- Constantes de référence (valeurs réelles du projet) ---------------------
static const float kStartThreshold = 0.05f;     // deadband démarrage (pH)
static const float kStopThreshold = 0.01f;      // deadband arrêt (pH)
static const unsigned long kMinInjectionMs = 30000UL;  // temps min injection
static const unsigned int kMaxCyclesPerDay = 20u;

// Helper : DoseInputs « tout OK » → evaluateDose doit retourner {true, None}.
// Toute dérivation (casser un champ) doit isoler une seule cause de refus.
static DoseInputs validInputs() {
  DoseInputs in;
  in.watchdogActive = true;
  in.continuousMode = false;
  in.filtrationRunning = true;
  in.reading = 7.2f;            // lecture pH valide (non NaN)
  in.filterReady = true;
  in.filterUnstable = false;
  in.calPoints = 2;            // pH : 2 points calibrés
  in.requiredPoints = 2;      // pH requiert 2 points
  in.stabilizationActive = false;
  in.mixingActive = false;
  in.modeAutomatic = true;
  in.dailyInjectedMl = 100.0f;  // sous la limite
  in.maxDailyMl = 500.0f;
  in.usedMs = 60000UL;         // sous la limite horaire
  in.hourlyLimitMs = 600000UL; // 10 min de limite horaire
  in.cyclesToday = 3;
  in.maxCyclesPerDay = kMaxCyclesPerDay;
  in.cyclesLastMin = 0;
  in.maxCyclesPerMin = 3;
  in.cyclesLast15Min = 0;
  in.maxCyclesPer15Min = 8;
  return in;
}

// =============================================================================
// T2 (AC2) — shouldStartDosingPure : hystérésis de démarrage
// =============================================================================
// Démarre ssi error > startThreshold ET cyclesToday < maxCyclesPerDay.
void test_T2_should_start_deadband_borders(void) {
  // error == seuil → pas de démarrage (comparaison stricte >).
  TEST_ASSERT_FALSE(shouldStartDosingPure(kStartThreshold, kStartThreshold, 3, kMaxCyclesPerDay));
  // Juste au-dessus du seuil → démarrage.
  TEST_ASSERT_TRUE(shouldStartDosingPure(kStartThreshold + 0.001f, kStartThreshold, 3, kMaxCyclesPerDay));
  // Juste en-dessous du seuil → pas de démarrage.
  TEST_ASSERT_FALSE(shouldStartDosingPure(kStartThreshold - 0.001f, kStartThreshold, 3, kMaxCyclesPerDay));
}

void test_T2_should_start_blocked_when_cycles_at_max(void) {
  // cyclesToday == max → pas de démarrage MÊME si error largement au-dessus.
  TEST_ASSERT_FALSE(shouldStartDosingPure(1.0f, kStartThreshold, kMaxCyclesPerDay, kMaxCyclesPerDay));
  // cyclesToday == max-1 → démarrage autorisé si error suffisante.
  TEST_ASSERT_TRUE(shouldStartDosingPure(1.0f, kStartThreshold, kMaxCyclesPerDay - 1, kMaxCyclesPerDay));
}

// =============================================================================
// T3 (AC3) — shouldContinueDosingPure : hystérésis de poursuite
// =============================================================================
void test_T3_should_continue_min_injection_then_stop_threshold(void) {
  // runTimeMs < min → poursuite forcée même si error <= stopThreshold.
  TEST_ASSERT_TRUE(shouldContinueDosingPure(0.0f, kStopThreshold, 10000UL, kMinInjectionMs));
  // runTimeMs >= min, error <= stopThreshold → arrêt.
  TEST_ASSERT_FALSE(shouldContinueDosingPure(kStopThreshold, kStopThreshold, kMinInjectionMs, kMinInjectionMs));
  // runTimeMs >= min, error > stopThreshold → poursuite.
  TEST_ASSERT_TRUE(shouldContinueDosingPure(kStopThreshold + 0.001f, kStopThreshold, kMinInjectionMs, kMinInjectionMs));
}

// =============================================================================
// T4 (AC4) — NON-RÉGRESSION pause-mélange (verrou contre le bug v2.2.5)
// =============================================================================
// Le bug v2.2.5 : la pompe était coupée après ~1 cycle car minInjectionTimeMs
// n'était jamais respecté. La logique pure GARANTIT qu'une injection ne peut PAS
// s'arrêter tant que runTimeMs < minInjectionTimeMs, indépendamment de l'erreur.
// Tant que runTimeMs < min → poursuite TOUJOURS true (error faible/sous stop).
void test_T4_min_injection_lock_against_v225_bug(void) {
  const float kLowError = 0.0f;  // erreur sous le seuil d'arrêt
  // Verrou : poursuite forcée à 0, 1000, 29999 ms (< min) même error faible.
  TEST_ASSERT_TRUE(shouldContinueDosingPure(kLowError, kStopThreshold, 0UL, kMinInjectionMs));
  TEST_ASSERT_TRUE(shouldContinueDosingPure(kLowError, kStopThreshold, 1000UL, kMinInjectionMs));
  TEST_ASSERT_TRUE(shouldContinueDosingPure(kLowError, kStopThreshold, 29999UL, kMinInjectionMs));
  // Une fois >= min, le verrou se lève : la décision suit l'hystérésis (error>stop).
  // error faible → arrêt autorisé.
  TEST_ASSERT_FALSE(shouldContinueDosingPure(kLowError, kStopThreshold, kMinInjectionMs, kMinInjectionMs));
  TEST_ASSERT_FALSE(shouldContinueDosingPure(kLowError, kStopThreshold, 60000UL, kMinInjectionMs));
  // error forte au-delà du min → poursuite (== error > stop).
  TEST_ASSERT_TRUE(shouldContinueDosingPure(0.5f, kStopThreshold, kMinInjectionMs, kMinInjectionMs));
}

// =============================================================================
// T5-T17 (AC5) — evaluateDose : une cause de refus par test
// =============================================================================

// T5 — FiltrationOff (filtration arrêtée, hors mode continu).
void test_T5_refusal_filtration_off(void) {
  DoseInputs in = validInputs();
  in.filtrationRunning = false;
  in.continuousMode = false;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::FiltrationOff, d.cause);
}

// T6 — ReadingNaN (lecture filtrée indisponible).
void test_T6_refusal_reading_nan(void) {
  DoseInputs in = validInputs();
  in.reading = NAN;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaN, d.cause);
}

// T7 — FilterNotReady (warmup / EZO injoignable).
void test_T7_refusal_filter_not_ready(void) {
  DoseInputs in = validInputs();
  in.filterReady = false;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::FilterNotReady, d.cause);
}

// T8 — FilterUnstable (rejets consécutifs).
void test_T8_refusal_filter_unstable(void) {
  DoseInputs in = validInputs();
  in.filterUnstable = true;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::FilterUnstable, d.cause);
}

// T9 — CalibrationInsufficient (calPoints < requiredPoints).
void test_T9_refusal_calibration_insufficient(void) {
  DoseInputs in = validInputs();
  in.calPoints = 1;       // < requiredPoints (2)
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CalibrationInsufficient, d.cause);
  // -1 = EZO injoignable → également bloqué fail-closed.
  in.calPoints = -1;
  d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CalibrationInsufficient, d.cause);
}

// T10 — StabilizationActive (stabilisation post-calibration).
void test_T10_refusal_stabilization_active(void) {
  DoseInputs in = validInputs();
  in.stabilizationActive = true;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::StabilizationActive, d.cause);
}

// T11 — MixingActive (pause mélange hydraulique).
void test_T11_refusal_mixing_active(void) {
  DoseInputs in = validInputs();
  in.mixingActive = true;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::MixingActive, d.cause);
}

// T12 — ModeNotAutomatic (mode régulation != automatic).
void test_T12_refusal_mode_not_automatic(void) {
  DoseInputs in = validInputs();
  in.modeAutomatic = false;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::ModeNotAutomatic, d.cause);
}

// T13 — DailyLimit (dailyInjectedMl >= maxDailyMl).
void test_T13_refusal_daily_limit(void) {
  DoseInputs in = validInputs();
  in.dailyInjectedMl = in.maxDailyMl;  // atteint la limite (>=)
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::DailyLimit, d.cause);
}

// T14 — HourlyLimit (usedMs >= hourlyLimitMs, hourlyLimitMs > 0).
void test_T14_refusal_hourly_limit(void) {
  DoseInputs in = validInputs();
  in.hourlyLimitMs = 600000UL;
  in.usedMs = 600000UL;   // atteint la limite (>=)
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::HourlyLimit, d.cause);
  // hourlyLimitMs == 0 → pas de limite horaire, ne doit PAS bloquer ici.
  in.hourlyLimitMs = 0UL;
  in.usedMs = 9999999UL;
  d = evaluateDose(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::None, d.cause);
}

// T15 — CyclesPerDay (cyclesToday >= maxCyclesPerDay).
void test_T15_refusal_cycles_per_day(void) {
  DoseInputs in = validInputs();
  in.cyclesToday = in.maxCyclesPerDay;  // atteint la limite (>=)
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CyclesPerDay, d.cause);
}

// T16 — BurstPerMinute (cyclesLastMin >= maxCyclesPerMin).
void test_T16_refusal_burst_per_minute(void) {
  DoseInputs in = validInputs();
  in.cyclesLastMin = in.maxCyclesPerMin;  // atteint la limite (>=)
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::BurstPerMinute, d.cause);
}

// T17 — BurstPer15Min (cyclesLast15Min >= maxCyclesPer15Min).
void test_T17_refusal_burst_per_15min(void) {
  DoseInputs in = validInputs();
  in.cyclesLast15Min = in.maxCyclesPer15Min;  // atteint la limite (>=)
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::BurstPer15Min, d.cause);
}

// T17b — WatchdogInactive (watchdogActive == false). Garde la plus prioritaire.
void test_T17b_refusal_watchdog_inactive(void) {
  DoseInputs in = validInputs();
  in.watchdogActive = false;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::WatchdogInactive, d.cause);
}

// =============================================================================
// T18 — Ordre de priorité des gardes (plusieurs échecs simultanés)
// =============================================================================
// La cause retournée est celle de la garde la plus prioritaire (1→15).
void test_T18_guard_priority_order(void) {
  // Paire 1 : watchdog inactif (1) + filtration off (2) → WatchdogInactive.
  {
    DoseInputs in = validInputs();
    in.watchdogActive = false;
    in.filtrationRunning = false;
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_EQUAL(DoseRefusal::WatchdogInactive, d.cause);
  }
  // Paire 2 : filtration off (2) + NaN (3) → FiltrationOff.
  {
    DoseInputs in = validInputs();
    in.filtrationRunning = false;
    in.reading = NAN;
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_EQUAL(DoseRefusal::FiltrationOff, d.cause);
  }
  // Paire 3 : NaN (3) + calibration insuffisante (4) → ReadingNaN.
  {
    DoseInputs in = validInputs();
    in.reading = NAN;
    in.calPoints = 0;
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaN, d.cause);
  }
  // Paire 4 (bonus) : calibration (4) + mode non auto (6) + limite jour (7)
  // → CalibrationInsufficient (la plus prioritaire des trois).
  {
    DoseInputs in = validInputs();
    in.calPoints = 0;
    in.modeAutomatic = false;
    in.dailyInjectedMl = in.maxDailyMl;
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_EQUAL(DoseRefusal::CalibrationInsufficient, d.cause);
  }
}

// =============================================================================
// T19 — Cas nominal : tout OK → autorisé
// =============================================================================
void test_T19_nominal_all_ok_allowed(void) {
  DoseInputs in = validInputs();
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::None, d.cause);
  // Cas nominal en mode continu (filtration off autorisée car eau 24/7).
  in.continuousMode = true;
  in.filtrationRunning = false;
  d = evaluateDose(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::None, d.cause);
}

// =============================================================================
// T20 — Deadband : pas de démarrage si error <= startThreshold
// =============================================================================
void test_T20_deadband_no_start_below_or_at_threshold(void) {
  // error == seuil → pas de démarrage.
  TEST_ASSERT_FALSE(shouldStartDosingPure(kStartThreshold, kStartThreshold, 0, kMaxCyclesPerDay));
  // error nettement sous le seuil → pas de démarrage.
  TEST_ASSERT_FALSE(shouldStartDosingPure(0.0f, kStartThreshold, 0, kMaxCyclesPerDay));
  TEST_ASSERT_FALSE(shouldStartDosingPure(0.02f, kStartThreshold, 0, kMaxCyclesPerDay));
}

int main(int, char **) {
  UNITY_BEGIN();
  // T2 — hystérésis de démarrage.
  RUN_TEST(test_T2_should_start_deadband_borders);
  RUN_TEST(test_T2_should_start_blocked_when_cycles_at_max);
  // T3 — hystérésis de poursuite.
  RUN_TEST(test_T3_should_continue_min_injection_then_stop_threshold);
  // T4 — non-régression pause-mélange (verrou bug v2.2.5).
  RUN_TEST(test_T4_min_injection_lock_against_v225_bug);
  // T5-T17 — causes de refus, une par test.
  RUN_TEST(test_T5_refusal_filtration_off);
  RUN_TEST(test_T6_refusal_reading_nan);
  RUN_TEST(test_T7_refusal_filter_not_ready);
  RUN_TEST(test_T8_refusal_filter_unstable);
  RUN_TEST(test_T9_refusal_calibration_insufficient);
  RUN_TEST(test_T10_refusal_stabilization_active);
  RUN_TEST(test_T11_refusal_mixing_active);
  RUN_TEST(test_T12_refusal_mode_not_automatic);
  RUN_TEST(test_T13_refusal_daily_limit);
  RUN_TEST(test_T14_refusal_hourly_limit);
  RUN_TEST(test_T15_refusal_cycles_per_day);
  RUN_TEST(test_T16_refusal_burst_per_minute);
  RUN_TEST(test_T17_refusal_burst_per_15min);
  RUN_TEST(test_T17b_refusal_watchdog_inactive);
  // T18 — ordre de priorité des gardes.
  RUN_TEST(test_T18_guard_priority_order);
  // T19 — cas nominal autorisé.
  RUN_TEST(test_T19_nominal_all_ok_allowed);
  // T20 — deadband de démarrage.
  RUN_TEST(test_T20_deadband_no_start_below_or_at_threshold);
  return UNITY_END();
}
