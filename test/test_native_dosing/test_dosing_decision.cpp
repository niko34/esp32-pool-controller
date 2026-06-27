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

// =============================================================================
// T21+ (feature-037) — computePidPure : cœur PID PUR (Option Y, constrain inclus)
// =============================================================================
// On verrouille le COMPORTEMENT observable (flow final borné + état intégrale)
// via la table d'équivalence de référence pool-chemistry. Tolérance flottante
// adaptée aux ordres de grandeur (flow ~0.1, intégrale ~0.5).
static const float kFloatEps = 1e-3f;

// --- Config pH de référence (table AC1) --------------------------------------
//   kp=8.0, ki=kd=0, integralMax=50, deadband=0.05, minFlow=5.2, maxFlow=90,
//   dtSec=10. Helper : applique computePidPure avec cette config + error/freeze.
static PidResult pidPh(float error, float integralIn, bool freeze) {
  return computePidPure(/*kp*/8.0f, /*ki*/0.0f, /*kd*/0.0f,
                        error, /*prevError*/0.0f, integralIn,
                        /*dtSec*/10.0f, /*integralMax*/50.0f, /*deadband*/0.05f,
                        /*minFlow*/5.2f, /*maxFlow*/90.0f, freeze);
}

// --- Config ORP de référence (table AC1) -------------------------------------
//   kp=0.3, ki=kd=0, integralMax=50, deadband=15, minFlow=5.2, maxFlow=90,
//   dtSec=10.
static PidResult pidOrp(float error, float integralIn, bool freeze) {
  return computePidPure(/*kp*/0.3f, /*ki*/0.0f, /*kd*/0.0f,
                        error, /*prevError*/0.0f, integralIn,
                        /*dtSec*/10.0f, /*integralMax*/50.0f, /*deadband*/15.0f,
                        /*minFlow*/5.2f, /*maxFlow*/90.0f, freeze);
}

// -----------------------------------------------------------------------------
// AC1 — Table d'équivalence pH (8 lignes) : assert flow ET intégrale après.
// -----------------------------------------------------------------------------
void test_AC1_ph_table(void) {
  PidResult r;

  // error 0.04 (deadband strict <) → flow 0, intégrale inchangée.
  r = pidPh(0.04f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.integral);

  // error 0.05 (PAS deadband : 0.05 < 0.05 faux) → output 0.4 → minFlow 5.2 ;
  // intégrale += 0.05*10 = 0.5.
  r = pidPh(0.05f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.5f, r.integral);

  // error 0.10 → output 0.8 → minFlow 5.2 ; intégrale += 1.0.
  r = pidPh(0.10f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 1.0f, r.integral);

  // error 0.65 → output 5.2 (= minFlow exactement) ; intégrale += 6.5.
  r = pidPh(0.65f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 6.5f, r.integral);

  // error 2.0 → output 16.0 ; intégrale += 20 → bornée à integralMax... non,
  // 20 < 50 donc 20. (La table indique « += 20 → bornée 20 » = pas de clamp.)
  r = pidPh(2.0f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 16.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 20.0f, r.integral);

  // error 11.25 freeze=true → output kp*err = 90 → maxFlow ; intégrale gelée.
  r = pidPh(11.25f, 42.0f, true);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 90.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 42.0f, r.integral);  // gelée (inchangée)

  // error 14.0 freeze=true → output 112 → clamp maxFlow 90 ; intégrale gelée.
  r = pidPh(14.0f, 7.0f, true);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 90.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 7.0f, r.integral);   // gelée

  // error -0.30 → output -2.4 → <0 → 0 → constrain(0, minFlow, maxFlow) = minFlow.
  // NB : la table de référence indique « flow 0 », mais l'algorithme Option Y
  // documenté (output<0→0 PUIS clamp[minFlow,maxFlow]) donne 5.2 : clamp(0,5.2,
  // 90)=5.2. On verrouille le COMPORTEMENT RÉEL du code (constrain inclus).
  // Voir « Notes techniques » de la spec : incohérence table vs Option Y.
  r = pidPh(-0.30f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, -3.0f, r.integral);
}

// -----------------------------------------------------------------------------
// AC1 — Table d'équivalence ORP (3 lignes).
// -----------------------------------------------------------------------------
void test_AC1_orp_table(void) {
  PidResult r;

  // error 100 freeze=non → output 0.3*100 = 30 ; intégrale += 1000 → bornée 50.
  r = pidOrp(100.0f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 30.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 50.0f, r.integral);  // clampée integralMax

  // error 300 freeze=oui → output 0.3*300 = 90 ; intégrale gelée.
  r = pidOrp(300.0f, 12.0f, true);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 90.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 12.0f, r.integral);  // gelée

  // error 10 (< deadband 15) → flow 0 (deadband) ; intégrale inchangée.
  r = pidOrp(10.0f, 5.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.0f, r.integral);
}

// -----------------------------------------------------------------------------
// AC2 — Proportionnalité monotone hors deadband, avant saturation.
// -----------------------------------------------------------------------------
//   flow(0.65) < flow(2.0) < flow(5.0). À 0.65 on est juste au plancher minFlow,
//   à 2.0 et 5.0 on est dans la zone linéaire (output 16 et 40, < maxFlow 90).
void test_AC2_proportionnalite_monotone(void) {
  float f065 = pidPh(0.65f, 0.0f, false).flow;  // 5.2 (plancher)
  float f200 = pidPh(2.0f, 0.0f, false).flow;   // 16.0
  float f500 = pidPh(5.0f, 0.0f, false).flow;   // 40.0
  TEST_ASSERT_TRUE(f065 < f200);
  TEST_ASSERT_TRUE(f200 < f500);
  // Croissance avec l'erreur : deux points internes linéaires.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 16.0f, f200);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 40.0f, f500);
}

// -----------------------------------------------------------------------------
// AC3 — Bornage haut : error >> maxFlow/kp → flow == maxFlow.
// -----------------------------------------------------------------------------
void test_AC3_bornage_haut(void) {
  // maxFlow/kp = 90/8 = 11.25 ; bien au-delà → saturation à 90.
  float f = pidPh(100.0f, 0.0f, true).flow;
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 90.0f, f);
}

// -----------------------------------------------------------------------------
// AC4 — Bornage bas : error juste > deadband → flow >= minFlow (jamais 0<flow<minFlow).
// -----------------------------------------------------------------------------
void test_AC4_bornage_bas(void) {
  // error 0.051 : output 0.408 → plancher minFlow 5.2 (pas 0.408).
  float f = pidPh(0.051f, 0.0f, false).flow;
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, f);
  // Garantie générale : pour plusieurs erreurs hors deadband et output>0,
  // flow est soit 0 (output<0), soit >= minFlow, jamais entre les deux.
  for (float e = 0.051f; e < 0.65f; e += 0.05f) {
    float flow = pidPh(e, 0.0f, false).flow;
    TEST_ASSERT_TRUE(flow == 0.0f || flow >= 5.2f - kFloatEps);
  }
}

// -----------------------------------------------------------------------------
// AC5 — Deadband : |error| < deadband (STRICT) → flow 0 ; borne 0.05 → PAS deadband.
// -----------------------------------------------------------------------------
void test_AC5_deadband(void) {
  // error 0.049 (< 0.05) → deadband → flow 0, intégrale inchangée.
  PidResult r = pidPh(0.049f, 3.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 3.0f, r.integral);
  // Borne stricte : 0.05 n'est PAS dans la deadband (0.05 < 0.05 faux) → 5.2.
  r = pidPh(0.05f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  // error négative dans la deadband (-0.04) → flow 0 (fabsf).
  r = pidPh(-0.04f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.flow);
}

// -----------------------------------------------------------------------------
// AC6 — Anti-windup : freeze → intégrale entrante == sortante (pas d'accumulation)
//        ET bornage de la grande accumulation à integralMax.
// -----------------------------------------------------------------------------
void test_AC6_anti_windup(void) {
  // freeze=true : intégrale renvoyée == intégrale entrante (aucune accumulation).
  PidResult r = pidPh(2.0f, 33.0f, true);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 33.0f, r.integral);
  // Bornage haut : ORP error 100 → += 1000 → clampée à integralMax 50.
  r = pidOrp(100.0f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 50.0f, r.integral);
  // Bornage bas symétrique : grande erreur négative → clampée à -integralMax.
  r = pidOrp(-100.0f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, -50.0f, r.integral);
}

// -----------------------------------------------------------------------------
// AC7 — P-temporisée (ki=kd=0) : output == kp*error (borné) ; l'intégrale
//        s'accumule mais N'INFLUENCE PAS le flow (ki=0).
// -----------------------------------------------------------------------------
void test_AC7_p_temporisee(void) {
  // Avec une intégrale entrante énorme, le flow reste piloté par kp*error seul.
  // error 2.0, integralIn=49 (≈ saturée) → output 16.0 inchangé (ki=0).
  PidResult r = pidPh(2.0f, 49.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 16.0f, r.flow);  // == kp*error, ki sans effet
  // L'intégrale continue d'accumuler (49 + 20 → clamp 50) mais sans effet flow.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 50.0f, r.integral);
  // Deux intégrales entrantes différentes → MÊME flow (preuve ki=0).
  float fA = pidPh(1.0f, 0.0f, false).flow;
  float fB = pidPh(1.0f, 40.0f, false).flow;
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, fA, fB);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 8.0f, fA);  // kp*error = 8.0
}

// -----------------------------------------------------------------------------
// Erreur négative : output < 0 → flow 0 (clamp négatif avant constrain).
// -----------------------------------------------------------------------------
void test_pid_negative_error_zero_flow(void) {
  // error -0.30 → output -2.4 → 0 → constrain(0,minFlow,maxFlow)=minFlow 5.2.
  // (Comportement réel Option Y : le plancher minFlow ré-élève la sortie nulle.
  // Incohérence avec la table « flow 0 » signalée en Notes techniques.)
  // Hors deadband donc l'intégrale accumule (-3.0).
  PidResult r = pidPh(-0.30f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, -3.0f, r.integral);
  // Grande erreur négative hors deadband → output 0 → plancher minFlow 5.2.
  r = pidPh(-10.0f, 0.0f, true);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5.2f, r.flow);
  // En revanche, erreur négative DANS la deadband → flow strictement 0
  // (retour avant constrain, le plancher ne s'applique pas).
  r = pidPh(-0.04f, 0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.flow);
}

// =============================================================================
// feature-039 — Anti-rafale + rollover journalier PURS
// =============================================================================
// On verrouille le COMPORTEMENT observable des 4 fonctions pures extraites de
// pump_controller (countCyclesInWindow, recordCycleTimestamp, shouldRolloverByDate,
// shouldRolloverByMillis). Constantes réelles : kDosingCycleHistorySize=20,
// fenêtres 60000 (1 min) / 900000 (15 min), seuils anti-rafale 6/min, 20/15min,
// frontière rollover 86400000 ms (24 h). Wrap millis uint32 testé explicitement.

static const size_t   kHistorySize  = 20;       // kDosingCycleHistorySize
static const uint32_t kWindow1Min   = 60000UL;
static const uint32_t kWindow15Min  = 900000UL;
static const int      kMaxPerMinute = 6;        // kMaxDosingCyclesPerMinute
static const int      kMaxPer15Min  = 20;       // kMaxDosingCyclesPer15Min
static const uint32_t kDayMs        = 86400000UL;

// -----------------------------------------------------------------------------
// AC1/AC2 — countCyclesInWindow
// -----------------------------------------------------------------------------

// Buffer entièrement vide (tous 0) → 0.
void test_F039_count_empty_buffer_zero(void) {
  uint32_t hist[20] = {0};
  TEST_ASSERT_EQUAL_INT(0, countCyclesInWindow(hist, kHistorySize, 1000000UL, kWindow1Min));
}

// Tous les slots non nuls et récents (dans la fenêtre) → count = nombre de slots non nuls.
void test_F039_count_all_recent_in_window(void) {
  uint32_t hist[20];
  for (size_t i = 0; i < kHistorySize; ++i) hist[i] = 1000000UL - (uint32_t)(i * 1000);  // 0..19s avant now
  uint32_t now = 1000000UL;
  // delta max = 19000 < 60000 → tous comptés.
  TEST_ASSERT_EQUAL_INT(20, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
}

// ts hors fenêtre (now-ts > windowMs) ignoré.
void test_F039_count_out_of_window_ignored(void) {
  uint32_t now = 1000000UL;
  uint32_t hist[20] = {0};
  hist[1] = now - 30000UL;   // dans la fenêtre
  hist[2] = now - 90000UL;   // hors fenêtre (90s > 60s)
  TEST_ASSERT_EQUAL_INT(1, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
}

// ts == now (delta 0) compté.
void test_F039_count_ts_equals_now_counted(void) {
  uint32_t now = 1000000UL;
  uint32_t hist[20] = {0};
  hist[5] = now;             // delta 0 → compté
  TEST_ASSERT_EQUAL_INT(1, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
}

// Frontière : now-ts == windowMs → compté ; now-ts == windowMs+1 → ignoré.
void test_F039_count_window_boundary_inclusive(void) {
  uint32_t now = 1000000UL;
  {
    uint32_t hist[20] = {0};
    hist[0] = now - kWindow1Min;       // delta == 60000 → <= window → compté
    TEST_ASSERT_EQUAL_INT(1, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
  }
  {
    uint32_t hist[20] = {0};
    hist[0] = now - (kWindow1Min + 1); // delta == 60001 → > window → ignoré
    TEST_ASSERT_EQUAL_INT(0, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
  }
}

// WRAP MILLIS : now=50, ts=0xFFFFFFF0 → (now-ts) = 66 (uint32) → compté.
void test_F039_count_millis_wrap_counted(void) {
  uint32_t now = 50UL;
  uint32_t hist[20] = {0};
  hist[3] = 0xFFFFFFF0UL;             // 50 - 0xFFFFFFF0 = 66 en arithmétique uint32
  // Vérifie l'arithmétique de débordement attendue.
  TEST_ASSERT_EQUAL_UINT32(66UL, now - 0xFFFFFFF0UL);
  TEST_ASSERT_EQUAL_INT(1, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
}

// Slot à valeur 0 ignoré même si "récent par hasard" (0 == vide par convention).
void test_F039_count_zero_slot_is_empty(void) {
  // now choisi pour que (now - 0) <= window serait vrai si 0 n'était pas traité comme vide.
  uint32_t now = 30000UL;            // now - 0 = 30000 <= 60000
  uint32_t hist[20] = {0};
  hist[7] = now - 10000UL;           // un vrai slot récent
  // Seul le slot non nul compte ; tous les 0 restent ignorés.
  TEST_ASSERT_EQUAL_INT(1, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
}

// -----------------------------------------------------------------------------
// AC3 — recordCycleTimestamp
// -----------------------------------------------------------------------------

// Écrit à idx et renvoie (idx+1)%size.
void test_F039_record_writes_and_advances(void) {
  uint32_t hist[20] = {0};
  size_t next = recordCycleTimestamp(hist, 0, kHistorySize, 12345UL);
  TEST_ASSERT_EQUAL_UINT32(12345UL, hist[0]);
  TEST_ASSERT_EQUAL_UINT(1, next);
  next = recordCycleTimestamp(hist, next, kHistorySize, 67890UL);
  TEST_ASSERT_EQUAL_UINT32(67890UL, hist[1]);
  TEST_ASSERT_EQUAL_UINT(2, next);
}

// Après `size` écritures successives (en réinjectant le retour), idx revient à 0.
void test_F039_record_wraps_index_after_size(void) {
  uint32_t hist[20] = {0};
  size_t idx = 0;
  for (size_t i = 0; i < kHistorySize; ++i) {
    idx = recordCycleTimestamp(hist, idx, kHistorySize, (uint32_t)(1000 + i));
  }
  TEST_ASSERT_EQUAL_UINT(0, idx);  // revenu au début du ring buffer
}

// Écrase le plus ancien : 21e écriture remplace le slot 0.
void test_F039_record_overwrites_oldest(void) {
  uint32_t hist[20] = {0};
  size_t idx = 0;
  for (size_t i = 0; i < kHistorySize; ++i) {
    idx = recordCycleTimestamp(hist, idx, kHistorySize, (uint32_t)(1000 + i));
  }
  TEST_ASSERT_EQUAL_UINT32(1000UL, hist[0]);          // ancien
  idx = recordCycleTimestamp(hist, idx, kHistorySize, 9999UL);
  TEST_ASSERT_EQUAL_UINT32(9999UL, hist[0]);          // écrasé par le plus récent
  TEST_ASSERT_EQUAL_UINT(1, idx);
}

// Après 20 écritures, le buffer reflète les 20 derniers timestamps :
// countCyclesInWindow voit bien size cycles dans une fenêtre les couvrant tous.
void test_F039_record_then_count_sees_all(void) {
  uint32_t hist[20] = {0};
  size_t idx = 0;
  uint32_t base = 5000000UL;
  // 20 cycles espacés de 1 s, tous dans la dernière minute.
  for (size_t i = 0; i < kHistorySize; ++i) {
    idx = recordCycleTimestamp(hist, idx, kHistorySize, base + (uint32_t)(i * 1000));
  }
  uint32_t now = base + 19000UL;  // delta max 19s < 60s
  TEST_ASSERT_EQUAL_INT(20, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));
}

// -----------------------------------------------------------------------------
// AC4 — shouldRolloverByDate
// -----------------------------------------------------------------------------
void test_F039_rollover_by_date(void) {
  // Date courante vide → pas encore de jour connu → pas de rollover.
  TEST_ASSERT_FALSE(shouldRolloverByDate("", "20260627"));
  // Même date → pas de rollover.
  TEST_ASSERT_FALSE(shouldRolloverByDate("20260627", "20260627"));
  // Date différente (jour précédent) → rollover.
  TEST_ASSERT_TRUE(shouldRolloverByDate("20260626", "20260627"));
}

// -----------------------------------------------------------------------------
// AC5 — shouldRolloverByMillis (frontière >= inclusive, wrap uint32)
// -----------------------------------------------------------------------------
void test_F039_rollover_by_millis(void) {
  // Exactement 24 h écoulées → rollover (frontière inclusive).
  TEST_ASSERT_TRUE(shouldRolloverByMillis(0UL, kDayMs));
  // 1 ms avant 24 h → pas encore.
  TEST_ASSERT_FALSE(shouldRolloverByMillis(0UL, kDayMs - 1));
}

// Wrap : dayStart=0xFFFFFFFF, now=86399999 → (now-dayStart) = 86400000 → true.
void test_F039_rollover_by_millis_wrap(void) {
  uint32_t dayStart = 0xFFFFFFFFUL;
  uint32_t now = 86399999UL;
  // Arithmétique uint32 : 86399999 - 0xFFFFFFFF = 86400000.
  TEST_ASSERT_EQUAL_UINT32(86400000UL, now - dayStart);
  TEST_ASSERT_TRUE(shouldRolloverByMillis(dayStart, now));
}

// -----------------------------------------------------------------------------
// AC6 — scénario anti-rafale combiné (cohérence avec seuils 6/min et 20/15min)
// -----------------------------------------------------------------------------
void test_F039_burst_scenario_combined(void) {
  uint32_t hist[20] = {0};
  size_t idx = 0;
  uint32_t base = 10000000UL;

  // 7 cycles dans la dernière minute (espacés de 5 s : 0,5,...,30 s) →
  // count sur 60s == 7 >= seuil 6/min → anti-rafale minute déclenché.
  for (int i = 0; i < 7; ++i) {
    idx = recordCycleTimestamp(hist, idx, kHistorySize, base + (uint32_t)(i * 5000));
  }
  uint32_t now = base + 30000UL;  // dernier cycle = now, plus ancien à -30s
  int c1 = countCyclesInWindow(hist, kHistorySize, now, kWindow1Min);
  TEST_ASSERT_EQUAL_INT(7, c1);
  TEST_ASSERT_TRUE(c1 >= kMaxPerMinute);

  // 21 cycles sur 15 min mais buffer plafonné à 20 → count plafonné à 20.
  uint32_t hist2[20] = {0};
  size_t idx2 = 0;
  uint32_t base2 = 20000000UL;
  // 21 cycles espacés de 40 s (840 s total < 900 s) ; le ring n'en garde que 20.
  for (int i = 0; i < 21; ++i) {
    idx2 = recordCycleTimestamp(hist2, idx2, kHistorySize, base2 + (uint32_t)(i * 40000));
  }
  uint32_t now2 = base2 + 20 * 40000UL;  // dernier cycle
  int c15 = countCyclesInWindow(hist2, kHistorySize, now2, kWindow15Min);
  TEST_ASSERT_EQUAL_INT(20, c15);             // plafonné par la taille du buffer
  TEST_ASSERT_TRUE(c15 >= kMaxPer15Min);      // >= seuil 20/15min → déclenché
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
  // feature-037 — computePidPure (cœur PID pur, Option Y).
  RUN_TEST(test_AC1_ph_table);
  RUN_TEST(test_AC1_orp_table);
  RUN_TEST(test_AC2_proportionnalite_monotone);
  RUN_TEST(test_AC3_bornage_haut);
  RUN_TEST(test_AC4_bornage_bas);
  RUN_TEST(test_AC5_deadband);
  RUN_TEST(test_AC6_anti_windup);
  RUN_TEST(test_AC7_p_temporisee);
  RUN_TEST(test_pid_negative_error_zero_flow);
  // feature-039 — anti-rafale + rollover journalier purs.
  RUN_TEST(test_F039_count_empty_buffer_zero);
  RUN_TEST(test_F039_count_all_recent_in_window);
  RUN_TEST(test_F039_count_out_of_window_ignored);
  RUN_TEST(test_F039_count_ts_equals_now_counted);
  RUN_TEST(test_F039_count_window_boundary_inclusive);
  RUN_TEST(test_F039_count_millis_wrap_counted);
  RUN_TEST(test_F039_count_zero_slot_is_empty);
  RUN_TEST(test_F039_record_writes_and_advances);
  RUN_TEST(test_F039_record_wraps_index_after_size);
  RUN_TEST(test_F039_record_overwrites_oldest);
  RUN_TEST(test_F039_record_then_count_sees_all);
  RUN_TEST(test_F039_rollover_by_date);
  RUN_TEST(test_F039_rollover_by_millis);
  RUN_TEST(test_F039_rollover_by_millis_wrap);
  RUN_TEST(test_F039_burst_scenario_combined);
  return UNITY_END();
}
