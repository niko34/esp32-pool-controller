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
#include <stdio.h>  // snprintf : diagnostic des balayages combinatoires
#include "dosing_logic.h"
#include "pulse_dosing_logic.h"  // itération 2 : proportionnel de temps (option B+C)
#include "constants.h"           // kDoseSensorFaultAlertDelayMs (verrou de seuil)

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
  in.waterPresent = true;       // feature-056 : présence d'eau résolue en amont
  in.reading = 7.2f;            // lecture pH valide (non NaN)
  // feature-152 : filterReady/filterUnstable ont disparu — la valeur officielle
  // de sensor_filter encapsule déjà warmup/figé/périmé/porte fermée (NaN dans
  // tous ces cas), une seule garde (isnan(reading)) suffit désormais.
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

// T5 — FiltrationOff (présence d'eau non résolue → dosage refusé).
void test_T5_refusal_filtration_off(void) {
  DoseInputs in = validInputs();
  in.waterPresent = false;
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

// T7/T8 — RETIRÉS (feature-152) : la garde séparée « filtre prêt » / « capteur
// instable » (3b/3c de l'ancien canDose()) a disparu avec les champs
// filterReady/filterUnstable qui l'alimentaient. La valeur officielle de
// sensor_filter encapsule désormais directement warmup, sonde figée,
// péremption et porte fermée (NaN dans tous ces cas) : une seule garde
// (isnan(in.reading), déjà couverte par T6) suffit. La qualification de cette
// garde par filterState (warmup/figé/périmé/porte fermée) est couverte par la
// section V2251 ci-dessous (test_V2251_qualification_reading_nan_par_etat).

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
  // Paire 1 : watchdog inactif (1) + eau absente (2) → WatchdogInactive.
  {
    DoseInputs in = validInputs();
    in.watchdogActive = false;
    in.waterPresent = false;
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_EQUAL(DoseRefusal::WatchdogInactive, d.cause);
  }
  // Paire 2 : eau absente (2) + NaN (3) → FiltrationOff.
  {
    DoseInputs in = validInputs();
    in.waterPresent = false;
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
  // Cas nominal avec eau présumée (mode Powered/External frais → waterPresent).
  in.waterPresent = true;
  d = evaluateDose(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::None, d.cause);
}

// =============================================================================
// feature-163 (gate pool-chemistry G2/G6) — garde CalibrationSessionOpen (rang 3)
// =============================================================================
// Session d'étalonnage OUVERTE (mqttCfg.calSessionOpen, persisté) : refuse TOUT
// dosage, quel que soit le mode. Rang 3 — DERRIÈRE watchdog (1) et présence
// d'eau (2, causes ABSOLUES), DEVANT NaN/calibration/mode (4, 5, 7) : pendant
// une session, ces trois-là sont des CONSÉQUENCES de l'acte d'étalonner
// (Cal,clear efface la calibration, Cal,mid fait dériver la lecture) —
// les annoncer accuserait la sonde d'une panne qu'elle n'a pas (ADR-0095).

// Cas de base : session ouverte, tout le reste OK → refus propre.
void test_F163_refusal_calibration_session_open(void) {
  DoseInputs in = validInputs();
  in.calSessionOpen = true;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CalibrationSessionOpen, d.cause);
}

// ⭐ Rang 3 OBSERVABLE, pas théorique : mesure NaN ET session ouverte → la
// cause rendue est CalibrationSessionOpen, PAS ReadingNaN.
void test_F163_rang3_devant_reading_nan(void) {
  DoseInputs in = validInputs();
  in.calSessionOpen = true;
  in.reading = NAN;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CalibrationSessionOpen, d.cause);
}

// Session ouverte ET calibration insuffisante (calPoints=0) ET mode non
// automatique, simultanément → la cause reste CalibrationSessionOpen (devant
// les trois conséquences de l'étalonnage).
void test_F163_rang3_devant_calibration_et_mode(void) {
  DoseInputs in = validInputs();
  in.calSessionOpen = true;
  in.calPoints = 0;
  in.modeAutomatic = false;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CalibrationSessionOpen, d.cause);
}

// ⭐ Persistance au boot (trou G1 du verdict pool-chemistry) : calSessionOpen
// vrai ET mode régulation DÉJÀ "manual" (modeAutomatic=false — simule le cas
// où calSwitchActive est resté faux car pH+ORP étaient déjà en manual à
// l'entrée en calibration) → la garde TIENT : la cause reste
// CalibrationSessionOpen, jamais ModeNotAutomatic. C'est exactement le trou
// que calSwitchActive laissait ouvert, dans le cas où l'utilisateur est le
// plus prudent.
void test_F163_persistance_boot_session_ouverte_mode_deja_manual(void) {
  DoseInputs in = validInputs();
  in.calSessionOpen = true;
  in.modeAutomatic = false;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CalibrationSessionOpen, d.cause);
}

// Non-masquage : watchdog inactif (1) + session ouverte (3) → la cause reste
// WatchdogInactive. Les causes ABSOLUES passent toujours AVANT.
void test_F163_non_masquage_watchdog_devant_session(void) {
  DoseInputs in = validInputs();
  in.watchdogActive = false;
  in.calSessionOpen = true;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::WatchdogInactive, d.cause);
}

// Non-masquage : absence d'eau (2) + session ouverte (3) → la cause reste
// FiltrationOff.
void test_F163_non_masquage_eau_devant_session(void) {
  DoseInputs in = validInputs();
  in.waterPresent = false;
  in.calSessionOpen = true;
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::FiltrationOff, d.cause);
}

// Session FERMÉE (valeur par défaut) : AUCUN refus nouveau — comportement
// inchangé, non-régression explicite du champ ajouté.
void test_F163_session_fermee_aucun_refus_nouveau(void) {
  DoseInputs in = validInputs();
  in.calSessionOpen = false;  // explicite, même si c'est déjà le défaut
  DoseDecision d = evaluateDose(in);
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
//
// ⚠️ v2.26.0 — CE N'EST PLUS LA CONFIGURATION DU FIRMWARE pH. `Ki = 0` a été levé
// sur le pH (kPhKi*, integralMax = kPhIntegralMax*, cf. constants.h « RÉGULATION
// pH — TERME INTÉGRAL »). Ces tables AC1..AC7 restent VALIDES et sont conservées
// telles quelles : elles verrouillent le contrat de `computePidPure` pour un
// correcteur P pur — ce que reste l'ORP (Ki_ORP = 0, hors périmètre du gate) et
// ce que fait la fonction pour ki = 0. Elles ne couvrent simplement plus la
// régulation pH de production : le volet PI est ajouté juste après (AC8..AC11),
// et le comportement en boucle fermée est couvert par test_native_ph_integral.
static PidResult pidPh(float error, float integralIn, bool freeze) {
  return computePidPure(/*kp*/8.0f, /*ki*/0.0f, /*kd*/0.0f,
                        error, /*prevError*/0.0f, integralIn,
                        /*dtSec*/10.0f, /*integralMax*/50.0f, /*deadband*/0.05f,
                        /*minFlow*/5.2f, /*maxFlow*/90.0f, freeze);
}

// --- Config pH de PRODUCTION v2.26.0 (vitesse `normal`) ----------------------
// Constantes IMPORTÉES de constants.h (jamais recopiées) : kp = kPhKpNormal,
// ki = kPhKiNormal, integralMax = kPhIntegralMaxNormal. Bornes de la coquille
// (feature-073) : minFlow = 0, maxFlow = débit d'amplitude calibré 24,7 mL/min,
// deadband = seuil de démarrage 0,05.
static const float kPhAmpFlow = 24.7f;
static PidResult pidPhPI(float error, float integralIn, bool freeze) {
  return computePidPure(kPhKpNormal, kPhKiNormal, /*kd*/0.0f,
                        error, /*prevError*/0.0f, integralIn,
                        /*dtSec*/10.0f, kPhIntegralMaxNormal, /*deadband*/0.05f,
                        /*minFlow*/0.0f, kPhAmpFlow, freeze);
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
//        ⚠️ v2.26.0 : vaut encore pour l'ORP et pour tout appel à ki = 0 ; le pH
//        de production n'est PLUS dans ce cas (cf. AC8 ci-dessous).
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
// v2.26.0 — VARIANTE PI (configuration pH RÉELLE du firmware)
// =============================================================================
// Double des tables AC1/AC6/AC7 avec les constantes de PRODUCTION. Ce qui change
// par rapport au P pur : le débit dépend désormais de l'ÉTAT (l'intégrale), le
// plafond d'accumulation n'est plus 50 pH·s mais kPhIntegralMaxNormal, et le
// plancher minFlow a disparu (feature-073, servi par l'accumulateur de pouls).
// Le comportement en boucle fermée (droop, convergence) est couvert par la suite
// dédiée test_native_ph_integral.

// AC8 — L'intégrale INFLUENCE le débit : c'est l'inverse exact d'AC7. Deux états
// intégraux différents, même erreur → débits différents.
void test_AC8_pi_l_integrale_influence_le_flow(void) {
  float f0 = pidPhPI(0.20f, 0.0f, false).flow;
  float fCharge = pidPhPI(0.20f, kPhIntegralMaxNormal, false).flow;
  TEST_ASSERT_TRUE(fCharge > f0);
  // Débit = kp·error + ki·integral, exactement.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, kPhKpNormal * 0.20f, f0);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps,
                           kPhKpNormal * 0.20f + kPhKiNormal * kPhIntegralMaxNormal,
                           fCharge);
  // L'apport de l'intégrale PLEINE vaut l'autorité posée : 6 mL/min.
  TEST_ASSERT_FLOAT_WITHIN(0.06f, kPhIntegralAuthorityMlPerMin, fCharge - f0);
}

// AC9 — Anti-windup avec les VRAIES bornes : plafond kPhIntegralMaxNormal, clamp
// symétrique, gel effectif.
void test_AC9_pi_anti_windup_bornes_de_production(void) {
  // dt énorme (1 h) et grosse erreur → clamp au plafond de PRODUCTION (16 200),
  // et surtout PAS à l'ancien 50.
  PidResult r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, 5.0f, 0.0f, 0.0f,
                               /*dtSec*/3600.0f, kPhIntegralMaxNormal, 0.05f,
                               0.0f, kPhAmpFlow, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, kPhIntegralMaxNormal, r.integral);
  TEST_ASSERT_TRUE(r.integral > 100.0f);   // le piège des 50 pH·s est bien mort
  // Symétrie basse.
  r = computePidPure(kPhKpNormal, kPhKiNormal, 0.0f, -5.0f, 0.0f, 0.0f,
                     3600.0f, kPhIntegralMaxNormal, 0.05f, 0.0f, kPhAmpFlow, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, -kPhIntegralMaxNormal, r.integral);
  // Gel : aucune accumulation, mais le terme acquis reste servi.
  r = pidPhPI(1.0f, 8000.0f, /*freeze*/true);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 8000.0f, r.integral);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps,
                           kPhKpNormal * 1.0f + kPhKiNormal * 8000.0f, r.flow);
}

// AC10 — La sortie reste bornée par le débit d'amplitude QUELLE QUE SOIT
// l'intégrale : l'invariant de sécurité que la levée de Ki ne doit pas entamer.
void test_AC10_pi_flow_jamais_au_dessus_du_debit_d_amplitude(void) {
  for (float e = -1.0f; e <= 6.0f; e += 0.25f) {
    for (float integ = -kPhIntegralMaxNormal; integ <= kPhIntegralMaxNormal;
         integ += kPhIntegralMaxNormal / 4.0f) {
      PidResult r = pidPhPI(e, integ, false);
      TEST_ASSERT_TRUE(r.flow <= kPhAmpFlow + kFloatEps);
      TEST_ASSERT_TRUE(r.flow >= 0.0f);
      TEST_ASSERT_TRUE(fabsf(r.integral) <= kPhIntegralMaxNormal + kFloatEps);
    }
  }
}

// AC11 — La deadband gèle AUSSI l'intégrale sous PI (retour anticipé) : c'est la
// propriété qui empêche la bande morte d'alimenter le windup… et, en contrepartie,
// qui prive l'intégrale de toute décharge tant que l'erreur reste positive
// (cf. caractérisation dans test_native_ph_integral).
void test_AC11_pi_deadband_gele_l_integrale(void) {
  PidResult r = pidPhPI(0.049f, 5000.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, r.flow);       // aucun débit…
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5000.0f, r.integral); // …et intégrale intacte
  // Hors deadband, l'accumulation reprend : += error × dt.
  r = pidPhPI(0.06f, 5000.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 5000.0f + 0.06f * 10.0f, r.integral);
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

// =============================================================================
// feature-006 — evaluateManualInject : injection manuelle gardée (décision pure)
// =============================================================================
// On verrouille le COMPORTEMENT observable des gardes d'injection manuelle :
// ordre EXACT validé pool-chemistry (watchdog EN PREMIER), frontières
// prédictives STRICT > (daily/hourly), frontières >= (cycles/anti-rafale),
// conventions « 0/≤0 = illimité », reliquat journalier remainingMl.
// Valeurs de référence calquées sur les vraies valeurs du projet :
//   maxDailyMl=300, hourlyLimitMs=300000 (5 min), maxCyclesPerDay=20,
//   maxCyclesPerMin=6, maxCyclesPer15Min=20.

// Helper : ManualInjectInputs « tout OK » → {allowed:true, None}.
// Toute dérivation (casser UN champ) doit isoler une seule cause de refus.
static ManualInjectInputs validManualInputs() {
  ManualInjectInputs in;
  in.watchdogActive = true;
  in.waterPresent = true;               // feature-056 : présence d'eau résolue en amont
  in.stabilizationActive = false;
  in.stabilizationRemainingS = 0;
  in.alreadyInjecting = false;
  in.automaticInjectInProgress = false;  // bug-161 (gate C1) : pas de dosage auto en cours
  in.requestedMl = 50.0f;               // 100 + 50 = 150 <= 300 → OK
  in.dailyInjectedMl = 100.0f;
  in.maxDailyMl = 300.0f;               // valeur réelle projet
  in.usedMs = 60000UL;                  // 60000 + 60000 = 120000 <= 300000 → OK
  in.hourlyLimitMs = 300000UL;          // 5 min (valeur réelle projet)
  in.requestedDurationMs = 60000UL;     // 1 min demandée
  in.cyclesToday = 3;
  in.maxCyclesPerDay = 20u;             // pumpProtection.maxCyclesPerDay
  in.cyclesLastMin = 0;
  in.maxCyclesPerMin = 6;               // kMaxDosingCyclesPerMinute
  in.cyclesLast15Min = 0;
  in.maxCyclesPer15Min = 20;            // kMaxDosingCyclesPer15Min
  return in;
}

// -----------------------------------------------------------------------------
// (a) Cas nominal : tout OK → autorisé, cause None, reliquat renseigné.
// -----------------------------------------------------------------------------
void test_F006_nominal_allowed(void) {
  ManualInjectInputs in = validManualInputs();
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
  // Reliquat = maxDailyMl - dailyInjectedMl = 300 - 100 = 200.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 200.0f, d.remainingMl);
}

// -----------------------------------------------------------------------------
// (a) Une cause de refus par test : casser UN champ à la fois.
// -----------------------------------------------------------------------------

// Garde 1 — WatchdogInactive (règle absolue : aucun dosage sans watchdog).
void test_F006_refusal_watchdog_inactive(void) {
  ManualInjectInputs in = validManualInputs();
  in.watchdogActive = false;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::WatchdogInactive, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.remainingMl);
}

// Garde 2 — FiltrationOff (présence d'eau non résolue → injection refusée).
void test_F006_refusal_filtration_off(void) {
  ManualInjectInputs in = validManualInputs();
  in.waterPresent = false;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::FiltrationOff, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.remainingMl);
}

// Garde 3 — StabilizationActive (stabilisation post-calibration en cours).
void test_F006_refusal_stabilization_active(void) {
  ManualInjectInputs in = validManualInputs();
  in.stabilizationActive = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::StabilizationActive, d.cause);
}

// Garde 4 — AlreadyInjecting (double start sur la même pompe). ⚠️ bug-160
// (volet 2, REFONTE 2026-10-13) : CalibrationHold (ex-rang 4) est RETIRÉ, ce
// test devient donc le test de la garde 4 (renumérotée depuis 5).
void test_F006_refusal_already_injecting(void) {
  ManualInjectInputs in = validManualInputs();
  in.alreadyInjecting = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::AlreadyInjecting, d.cause);
}

// Garde 5 — DailyLimit PRÉDICTIVE (cumul + demandé > max).
void test_F006_refusal_daily_limit(void) {
  ManualInjectInputs in = validManualInputs();
  in.dailyInjectedMl = 280.0f;   // 280 + 50 = 330 > 300 → refus
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::DailyLimit, d.cause);
  // Reliquat renseigné pour DailyLimit : 300 - 280 = 20.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 20.0f, d.remainingMl);
}

// Garde 6 — HourlyLimit PRÉDICTIVE (usedMs + durée > limite).
void test_F006_refusal_hourly_limit(void) {
  ManualInjectInputs in = validManualInputs();
  in.usedMs = 290000UL;          // 290000 + 60000 = 350000 > 300000 → refus
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::HourlyLimit, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.remainingMl);
}

// Garde 7 — MaxCyclesPerDay (frontière >= : à la limite → refus, compteur
// partagé auto + manuel).
void test_F006_refusal_max_cycles_per_day(void) {
  ManualInjectInputs in = validManualInputs();
  in.cyclesToday = in.maxCyclesPerDay;   // 20 >= 20 → refus
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::MaxCyclesPerDay, d.cause);
}

// Garde 8 — BurstPerMinute (frontière >=, ring partagé avec l'auto).
void test_F006_refusal_burst_per_minute(void) {
  ManualInjectInputs in = validManualInputs();
  in.cyclesLastMin = in.maxCyclesPerMin;   // 6 >= 6 → refus
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::BurstPerMinute, d.cause);
}

// Garde 9 — BurstPer15Min (frontière >=, ring partagé avec l'auto).
void test_F006_refusal_burst_per_15min(void) {
  ManualInjectInputs in = validManualInputs();
  in.cyclesLast15Min = in.maxCyclesPer15Min;   // 20 >= 20 → refus
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::BurstPer15Min, d.cause);
}

// -----------------------------------------------------------------------------
// (b) Reliquat : à 290/300, demander 30 → refus DailyLimit avec remainingMl=10 ;
//     demander exactement le reliquat (10) → accepté.
// -----------------------------------------------------------------------------
void test_F006_remaining_ml_on_daily_refusal(void) {
  ManualInjectInputs in = validManualInputs();
  in.dailyInjectedMl = 290.0f;
  in.maxDailyMl = 300.0f;
  in.requestedMl = 30.0f;        // 290 + 30 = 320 > 300 → refus
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::DailyLimit, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 10.0f, d.remainingMl);
  // Demander exactement le reliquat → accepté (frontière == acceptée).
  in.requestedMl = 10.0f;        // 290 + 10 = 300 == max → accepté
  d = evaluateManualInject(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 10.0f, d.remainingMl);
}

// -----------------------------------------------------------------------------
// (c) Frontières figées pool-chemistry feature-006 (STRICT > prédictif).
// -----------------------------------------------------------------------------
void test_F006_boundary_daily_exact_accepted(void) {
  ManualInjectInputs in = validManualInputs();
  in.dailyInjectedMl = 250.0f;
  in.requestedMl = 50.0f;        // 250 + 50 == 300 exactement
  ManualInjectDecision d = evaluateManualInject(in);
  // cumul + demandé == max → ACCEPTÉ — frontière validée pool-chemistry feature-006.
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
  // +0.1 mL au-delà → refusé — frontière validée pool-chemistry feature-006.
  in.requestedMl = 50.1f;        // 250 + 50.1 = 300.1 > 300
  d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::DailyLimit, d.cause);
}

void test_F006_boundary_hourly_exact_accepted(void) {
  ManualInjectInputs in = validManualInputs();
  in.usedMs = 240000UL;
  in.requestedDurationMs = 60000UL;   // 240000 + 60000 == 300000 exactement
  ManualInjectDecision d = evaluateManualInject(in);
  // usedMs + durée == limite → ACCEPTÉ — frontière validée pool-chemistry feature-006.
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
  // +1 ms au-delà → refusé — frontière validée pool-chemistry feature-006.
  in.requestedDurationMs = 60001UL;   // 300001 > 300000
  d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::HourlyLimit, d.cause);
}

void test_F006_boundary_zero_limits_unlimited(void) {
  // maxDailyMl = 0 → garde journalière INACTIVE (convention ≤ 0 = illimité).
  ManualInjectInputs in = validManualInputs();
  in.maxDailyMl = 0.0f;
  in.dailyInjectedMl = 5000.0f;   // cumul énorme, sans effet
  in.requestedMl = 2000.0f;
  ManualInjectDecision d = evaluateManualInject(in);
  // accepté — frontière validée pool-chemistry feature-006 (maxDailyMl ≤ 0 = illimité).
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);

  // hourlyLimitMs = 0 → garde horaire INACTIVE (convention identique à l'auto).
  in = validManualInputs();
  in.hourlyLimitMs = 0UL;
  in.usedMs = 99999999UL;         // budget énorme, sans effet
  in.requestedDurationMs = 3600000UL;
  d = evaluateManualInject(in);
  // accepté — frontière validée pool-chemistry feature-006 (hourlyLimitMs = 0 = illimité).
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
}

// -----------------------------------------------------------------------------
// (d) Ordre de priorité des gardes (échecs multiples simultanés) : la cause
//     retournée est celle de la garde la plus prioritaire (ordre pool-chemistry).
// -----------------------------------------------------------------------------
void test_F006_guard_priority_order(void) {
  // Paire 1 : watchdog inactif (1) + eau absente (2) → WatchdogInactive.
  {
    ManualInjectInputs in = validManualInputs();
    in.watchdogActive = false;
    in.waterPresent = false;
    ManualInjectDecision d = evaluateManualInject(in);
    TEST_ASSERT_EQUAL(ManualInjectRefusal::WatchdogInactive, d.cause);
  }
  // Paire 2 : eau absente (2) + limite journalière (5) → FiltrationOff.
  {
    ManualInjectInputs in = validManualInputs();
    in.waterPresent = false;
    in.dailyInjectedMl = 300.0f;   // 300 + 50 > 300
    ManualInjectDecision d = evaluateManualInject(in);
    TEST_ASSERT_EQUAL(ManualInjectRefusal::FiltrationOff, d.cause);
  }
  // Paire 3 : limite journalière (5) + anti-rafale minute (8) → DailyLimit.
  {
    ManualInjectInputs in = validManualInputs();
    in.dailyInjectedMl = 300.0f;   // 300 + 50 > 300
    in.cyclesLastMin = in.maxCyclesPerMin;
    ManualInjectDecision d = evaluateManualInject(in);
    TEST_ASSERT_EQUAL(ManualInjectRefusal::DailyLimit, d.cause);
  }
  // Trio bonus : stabilisation (3) + double start (4) + horaire (6) →
  // StabilizationActive (la plus prioritaire des trois).
  {
    ManualInjectInputs in = validManualInputs();
    in.stabilizationActive = true;
    in.alreadyInjecting = true;
    in.usedMs = 300000UL;          // 300000 + 60000 > 300000
    ManualInjectDecision d = evaluateManualInject(in);
    TEST_ASSERT_EQUAL(ManualInjectRefusal::StabilizationActive, d.cause);
  }
}

// bug-160 (volet 2, REFONTE 2026-10-13) : le test d'ordre LOAD-BEARING de
// l'ex-garde CalibrationHold (rang 4) est RETIRÉ avec elle — le mécanisme de
// bail qui la justifiait est ABANDONNÉ (cal_hold_logic supprimé). Voir
// cal_manual_switch_logic.h / test_native_cal_manual_switch pour la logique
// qui la remplace (bascule explicite de régulation, hors evaluateManualInject).

// =============================================================================
// bug-161 (gate pool-chemistry C1) — garde AutomaticInjectInProgress (rang 4)
// =============================================================================
// Ce trou était INATTEIGNABLE tant que le bouton d'injection manuelle restait
// masqué hors du mode `manual` (app.js). feature-161 lève ce masquage : SANS
// cette garde, une injection manuelle pourrait démarrer PAR-DESSUS un dosage
// automatique en cours. Trois faits à verrouiller : (1) la garde refuse
// effectivement ; (2) elle est DEVANT les quotas (5-10, ADR-0095 : la cause
// annoncée doit être celle qui mord) ; (3) elle reste DERRIÈRE Watchdog (1) et
// FiltrationOff (2) — une cause bénigne ne doit jamais masquer une cause absolue.

// Garde 4 — AutomaticInjectInProgress seule : refus, même si tout le reste est OK.
void test_F161_refusal_automatic_inject_in_progress(void) {
  ManualInjectInputs in = validManualInputs();
  in.automaticInjectInProgress = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::AutomaticInjectInProgress, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.remainingMl);
}

// Garde 4 devant les quotas (5-10) : automatique en cours + limite journalière
// dépassée ⇒ la cause rendue reste AutomaticInjectInProgress (ADR-0095 : la
// cause annoncée doit être celle qui mord en premier, pas la plus "sévère").
void test_F161_automatic_inject_in_progress_devant_quotas(void) {
  ManualInjectInputs in = validManualInputs();
  in.automaticInjectInProgress = true;
  in.dailyInjectedMl = 300.0f;        // 300 + 50 > 300 (DailyLimit aussi en échec)
  in.cyclesLastMin = in.maxCyclesPerMin;  // BurstPerMinute aussi en échec
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::AutomaticInjectInProgress, d.cause);
}

// Watchdog (1) ET FiltrationOff (2) ET automatique en cours (4) simultanément :
// la cause rendue reste WatchdogInactive — ORDRE LOAD-BEARING, une cause
// bénigne ne doit JAMAIS masquer une cause absolue.
void test_F161_watchdog_devant_automatic_inject_in_progress(void) {
  ManualInjectInputs in = validManualInputs();
  in.watchdogActive = false;
  in.waterPresent = false;
  in.automaticInjectInProgress = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::WatchdogInactive, d.cause);
}

// FiltrationOff (2) devant automatique en cours (4) : même principe, un cran
// plus bas.
void test_F161_filtration_off_devant_automatic_inject_in_progress(void) {
  ManualInjectInputs in = validManualInputs();
  in.waterPresent = false;
  in.automaticInjectInProgress = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::FiltrationOff, d.cause);
}

// Automatique en cours (4) devant AlreadyInjecting (5) : vérifie le RANG
// exact d'insertion — sans cela, un double-start manuel masquerait la
// véritable cause (chevauchement avec l'automatique).
void test_F161_automatic_inject_in_progress_devant_already_injecting(void) {
  ManualInjectInputs in = validManualInputs();
  in.automaticInjectInProgress = true;
  in.alreadyInjecting = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::AutomaticInjectInProgress, d.cause);
}

// =============================================================================
// feature-163 (gate pool-chemistry G2/G6) — garde CalibrationSessionOpen (rang 3)
// =============================================================================
// Même garde côté injection MANUELLE : rang 3, AVANT StabilizationActive
// (ex-rang 3, décalée en 4) — pendant une session, la stabilisation post-cal
// finira de toute façon par s'armer à la FERMETURE de la calibration ; la
// cause annoncée pendant la session doit porter sur l'acte en cours, pas sur
// sa conséquence différée.

void test_F163_manual_refusal_calibration_session_open(void) {
  ManualInjectInputs in = validManualInputs();
  in.calSessionOpen = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::CalibrationSessionOpen, d.cause);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.remainingMl);
}

// Rang 3 devant StabilizationActive (4) : session ouverte + stabilisation en
// cours simultanément → la cause reste CalibrationSessionOpen.
void test_F163_manual_rang3_devant_stabilization(void) {
  ManualInjectInputs in = validManualInputs();
  in.calSessionOpen = true;
  in.stabilizationActive = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::CalibrationSessionOpen, d.cause);
}

// Rang 3 devant AutomaticInjectInProgress (5, bug-161) et AlreadyInjecting (6)
// simultanément → la cause reste CalibrationSessionOpen (devant tout, hors
// causes ABSOLUES).
void test_F163_manual_rang3_devant_automatic_et_already_injecting(void) {
  ManualInjectInputs in = validManualInputs();
  in.calSessionOpen = true;
  in.automaticInjectInProgress = true;
  in.alreadyInjecting = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::CalibrationSessionOpen, d.cause);
}

// Non-masquage : watchdog (1) devant session ouverte (3) — cause ABSOLUE.
void test_F163_manual_non_masquage_watchdog_devant_session(void) {
  ManualInjectInputs in = validManualInputs();
  in.watchdogActive = false;
  in.calSessionOpen = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::WatchdogInactive, d.cause);
}

// Non-masquage : absence d'eau (2) devant session ouverte (3) — cause ABSOLUE.
void test_F163_manual_non_masquage_eau_devant_session(void) {
  ManualInjectInputs in = validManualInputs();
  in.waterPresent = false;
  in.calSessionOpen = true;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::FiltrationOff, d.cause);
}

// Session FERMÉE : aucun refus nouveau — non-régression explicite.
void test_F163_manual_session_fermee_aucun_refus_nouveau(void) {
  ManualInjectInputs in = validManualInputs();
  in.calSessionOpen = false;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
}

// -----------------------------------------------------------------------------
// (e) AC3 — Partage anti-rafale AUTO/MANUEL via le ring buffer commun.
// -----------------------------------------------------------------------------
// La coquille enregistre chaque start MANUEL avec recordCycleTimestamp() dans le
// MÊME ring buffer que l'auto (feature-039), et l'auto compte via
// countCyclesInWindow(). On démontre ici qu'un start manuel enregistré dans le
// ring devient immédiatement visible des gardes anti-rafale AUTO (et
// réciproquement, puisque evaluateManualInject consomme les mêmes compteurs
// cyclesLastMin/cyclesLast15Min issus de ce ring).
void test_F006_manual_start_visible_in_shared_ring(void) {
  uint32_t hist[20] = {0};
  size_t idx = 0;
  uint32_t now = 42000000UL;

  // 5 cycles AUTO récents dans la dernière minute (sous le seuil 6/min).
  for (int i = 0; i < 5; ++i) {
    idx = recordCycleTimestamp(hist, idx, kHistorySize, now - (uint32_t)(i * 5000 + 5000));
  }
  TEST_ASSERT_EQUAL_INT(5, countCyclesInWindow(hist, kHistorySize, now, kWindow1Min));

  // Un start MANUEL est enregistré dans le MÊME ring (comme le fait la coquille).
  idx = recordCycleTimestamp(hist, idx, kHistorySize, now);

  // Le comptage fenêtre 1 min voit maintenant 6 cycles : le start manuel est
  // visible des gardes AUTO → l'anti-rafale 6/min se déclenche côté auto.
  int cycles = countCyclesInWindow(hist, kHistorySize, now, kWindow1Min);
  TEST_ASSERT_EQUAL_INT(6, cycles);
  TEST_ASSERT_TRUE(cycles >= kMaxPerMinute);

  // Boucle fermée : ces mêmes compteurs, réinjectés dans evaluateManualInject,
  // bloquent aussi une 2e injection manuelle (BurstPerMinute).
  ManualInjectInputs in = validManualInputs();
  in.cyclesLastMin = cycles;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::BurstPerMinute, d.cause);
}

// -----------------------------------------------------------------------------
// (f) StabilizationActive : la décision dépend UNIQUEMENT du booléen ;
//     stabilizationRemainingS n'est qu'un détail de formatage (409) de la coquille.
// -----------------------------------------------------------------------------
void test_F006_stabilization_independent_of_remaining_seconds(void) {
  // stabilizationActive=true + remainingS=0 → refus quand même.
  ManualInjectInputs in = validManualInputs();
  in.stabilizationActive = true;
  in.stabilizationRemainingS = 0;
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::StabilizationActive, d.cause);
  // stabilizationActive=true + remainingS énorme → même cause (pas d'effet).
  in.stabilizationRemainingS = 999999UL;
  d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::StabilizationActive, d.cause);
  // stabilizationActive=false + remainingS non nul → AUTORISÉ (le champ seul
  // ne bloque jamais : il est recopié tel quel pour le message 409).
  in.stabilizationActive = false;
  in.stabilizationRemainingS = 999999UL;
  d = evaluateManualInject(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);
}

// =============================================================================
// feature-011 — evaluateScheduledDose : répartition 24 h programmée (décision pure)
// =============================================================================
// On verrouille le COMPORTEMENT observable de la répartition « scheduled » :
// volume de fenêtre recalculé à CHAQUE nouvelle fenêtre depuis l'état courant
// (auto-correcteur), report anti short-cycling, écrêtage budget horaire
// (ADR-0020, budget PARTAGÉ), gardes fail-closed (conditions pool-chemistry
// n°2/3/4/5), conservation de stopTargetMl dans une même fenêtre.
// Valeurs de référence de la spec : cible 300 mL/jour, horizon 240 min (4 h de
// filtration restante), fenêtres de 15 min (kScheduledWindowMinutes), débit
// effectif 60 mL/min.

// Helper : ScheduledDoseInputs nominal de la spec → première fenêtre du plan.
// NB : minInjectionTimeMs=5000 par défaut pour que le v nominal (18,75 mL soit
// 18,75 s à 60 mL/min) ne soit PAS reporté ; le report anti short-cycling avec
// la valeur prod (30 s) est testé séparément (test_F011_report_sous_30s...).
static ScheduledDoseInputs validScheduledInputs() {
  ScheduledDoseInputs in;
  in.nowMin = 600;                 // 10:00 → fenêtre absolue 600/15 = 40
  in.horizonMinutes = 240;         // 4 h de plage restantes
  in.windowMinutes = 15;           // kScheduledWindowMinutes
  in.dailyTargetMl = 300.0f;       // cible utilisateur de la spec
  in.maxDailyMl = 500.0f;          // plafond sécurité AU-DESSUS de la cible
  in.dailyInjectedMl = 0.0f;
  in.effectiveFlowMlPerMin = 60.0f;// débit UNIQUE (condition n°5)
  in.usedMs = 0UL;
  in.hourlyLimitMs = 0UL;          // illimité par défaut (convention auto)
  in.minInjectionTimeMs = 5000UL;  // cf. NB ci-dessus
  in.watchdogActive = true;
  in.prevWindowIndex = -1;         // aucun tick précédent → recalcul forcé
  in.prevStopTargetMl = 0.0f;
  return in;
}

// -----------------------------------------------------------------------------
// (1) Cas nominal spec : 300 mL sur 240 min → 16 fenêtres de 18,75 mL.
// -----------------------------------------------------------------------------
void test_F011_nominal_repartition_16_fenetres(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  // nWin = 240/15 = 16 → v = 300/16 = 18,75 mL pour la fenêtre courante.
  TEST_ASSERT_TRUE(d.doseNow);
  TEST_ASSERT_EQUAL_INT(40, d.windowIndex);                       // 600/15
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 18.75f, d.stopTargetMl);    // injecté + 18,75
  // Débit moyen planifié (diagnostic WS) : 300 mL / 240 min = 1,25 mL/min.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 1.25f, d.plannedFlowMlPerMin);
}

// -----------------------------------------------------------------------------
// (2) Report anti short-cycling : v/débit < 30 s → v=0 (rien CETTE fenêtre),
//     le volume se reporte ; fenêtre ultérieure (horizon réduit) → v plus gros
//     qui repasse la barre des 30 s.
// -----------------------------------------------------------------------------
void test_F011_report_sous_30s_puis_fenetre_suivante(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.minInjectionTimeMs = 30000UL;   // valeur prod (30 s)
  in.dailyInjectedMl = 50.0f;        // remaining = 250 → v = 250/16 = 15,625 mL
  // 15,625 mL à 60 mL/min = 15 625 ms < 30 000 → report : v=0.
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  TEST_ASSERT_FALSE(d.doseNow);
  TEST_ASSERT_EQUAL_INT(40, d.windowIndex);
  // stopTarget = dailyInjectedMl (aucun volume planifié cette fenêtre).
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 50.0f, d.stopTargetMl);
  // Le plan reste actif (remaining > 0) : plannedFlow = 250/240.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 250.0f / 240.0f, d.plannedFlowMlPerMin);

  // Fenêtre ultérieure, horizon réduit (12:00, 120 min restantes) : le volume
  // reporté se concentre → nWin = 8 → v = 250/8 = 31,25 mL = 31 250 ms >= 30 s.
  in.nowMin = 720;
  in.horizonMinutes = 120;
  in.prevWindowIndex = d.windowIndex;
  in.prevStopTargetMl = d.stopTargetMl;
  ScheduledDoseDecision d2 = evaluateScheduledDose(in);
  TEST_ASSERT_TRUE(d2.doseNow);
  TEST_ASSERT_EQUAL_INT(48, d2.windowIndex);                       // 720/15
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 81.25f, d2.stopTargetMl);    // 50 + 31,25
  // v plus gros qu'à la fenêtre reportée : 31,25 > 15,625.
  TEST_ASSERT_TRUE((d2.stopTargetMl - in.dailyInjectedMl) > 15.625f);
}

// -----------------------------------------------------------------------------
// (3) Budget horaire (ADR-0020, budget PARTAGÉ) : écrêtage de v au budget
//     restant converti en mL, et frontière usedMs == hourlyLimitMs EXACTE.
// -----------------------------------------------------------------------------
void test_F011_budget_horaire_ecretage(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.hourlyLimitMs = 300000UL;   // 5 min de budget horaire
  in.usedMs = 290000UL;          // reste 10 000 ms → 10 mL à 60 mL/min
  // v nominal 18,75 mL > budget 10 mL → écrêté à 10 mL.
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  TEST_ASSERT_TRUE(d.doseNow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 10.0f, d.stopTargetMl);   // injecté(0) + 10
}

void test_F011_budget_horaire_frontiere_exacte(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.minInjectionTimeMs = 0UL;   // isole la frontière horaire du report 30 s
  in.hourlyLimitMs = 300000UL;

  // usedMs == hourlyLimitMs EXACTEMENT → budget 0 → v=0, pas d'injection.
  in.usedMs = 300000UL;
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  TEST_ASSERT_FALSE(d.doseNow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.stopTargetMl);    // = injecté (0)
  // PAS un refus fail-closed : la fenêtre reste valide (l'excédent se reporte).
  TEST_ASSERT_EQUAL_INT(40, d.windowIndex);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 1.25f, d.plannedFlowMlPerMin);

  // usedMs == hourlyLimitMs - 1 → budget strictement positif → v > 0 → dose.
  in.usedMs = 299999UL;
  d = evaluateScheduledDose(in);
  TEST_ASSERT_TRUE(d.doseNow);
  TEST_ASSERT_TRUE(d.stopTargetMl > 0.0f);
}

// -----------------------------------------------------------------------------
// (4) Auto-correcteur : changement de cible à midi (300 → 500 mL, 150 injectés)
//     → nouveau v = restant / nWin restantes, recalculé à la nouvelle fenêtre.
// -----------------------------------------------------------------------------
void test_F011_changement_cible_midi(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.nowMin = 720;               // midi → fenêtre 48 (nouvelle : prev = 47)
  in.horizonMinutes = 210;       // 3 h 30 restantes → nWin = 14
  in.dailyTargetMl = 500.0f;     // cible relevée en cours de journée
  in.dailyInjectedMl = 150.0f;   // déjà injecté sous l'ancienne cible
  in.prevWindowIndex = 47;
  in.prevStopTargetMl = 150.0f;
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  // remaining = 500 - 150 = 350 → v = 350/14 = 25 mL.
  TEST_ASSERT_TRUE(d.doseNow);
  TEST_ASSERT_EQUAL_INT(48, d.windowIndex);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 175.0f, d.stopTargetMl);   // 150 + 25
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 350.0f / 210.0f, d.plannedFlowMlPerMin);
}

// -----------------------------------------------------------------------------
// (5a) Injection manuelle entre fenêtres : dailyInjectedMl (cumul auto+manuel)
//      augmente → le v de la fenêtre suivante est RÉDUIT (auto-correcteur).
// -----------------------------------------------------------------------------
void test_F011_injection_manuelle_reduit_fenetre_suivante(void) {
  // Fenêtre 40 : v = 300/16 = 18,75 mL (nominal).
  ScheduledDoseInputs in = validScheduledInputs();
  ScheduledDoseDecision d1 = evaluateScheduledDose(in);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 18.75f, d1.stopTargetMl);

  // Fenêtre 41 : le cumul intègre 18,75 (auto) + 60 (MANUEL) = 78,75 mL.
  in.nowMin = 615;
  in.horizonMinutes = 225;       // nWin = 15
  in.dailyInjectedMl = 78.75f;
  in.prevWindowIndex = d1.windowIndex;
  in.prevStopTargetMl = d1.stopTargetMl;
  ScheduledDoseDecision d2 = evaluateScheduledDose(in);
  // remaining = 300 - 78,75 = 221,25 → v = 221,25/15 = 14,75 mL.
  float v2 = d2.stopTargetMl - in.dailyInjectedMl;
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 14.75f, v2);
  // Sans l'injection manuelle (cumul 18,75), v aurait été 281,25/15 = 18,75 :
  // le manuel réduit bien la dose suivante.
  TEST_ASSERT_TRUE(v2 < 18.75f);
}

// -----------------------------------------------------------------------------
// (5b) Condition n°3 pool-chemistry : baisse de la cible SOUS le cumul injecté
//      EN COURS de fenêtre (même windowIndex) → doseNow=false IMMÉDIAT via
//      min(stopTargetMl, cible effective), sans attendre la fenêtre suivante.
// -----------------------------------------------------------------------------
void test_F011_baisse_cible_sous_injecte_stop_immediat(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.prevWindowIndex = 40;       // MÊME fenêtre (600/15 = 40) → pas de recalcul
  in.prevStopTargetMl = 200.0f;  // cible d'arrêt fixée à l'entrée de fenêtre
  in.dailyInjectedMl = 150.0f;   // cumul courant, sous prevStopTargetMl
  in.dailyTargetMl = 100.0f;     // cible ABAISSÉE sous le cumul injecté
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  // stopCap = min(200, 100) = 100 ; 150 < 100 faux → arrêt immédiat.
  TEST_ASSERT_FALSE(d.doseNow);
  // stopTargetMl conservé (même fenêtre) — la protection vient du cap, pas
  // d'un recalcul de fenêtre.
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 200.0f, d.stopTargetMl);
  // remaining clampé >= 0 (condition n°3) → plus rien à injecter → NAN.
  TEST_ASSERT_TRUE(isnan(d.plannedFlowMlPerMin));
}

// -----------------------------------------------------------------------------
// (6) Plafond sécurité : maxDailyMl < dailyTargetMl → cible effective = plafond.
// -----------------------------------------------------------------------------
void test_F011_plafond_max_daily(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.dailyTargetMl = 500.0f;
  in.maxDailyMl = 300.0f;        // plafond SOUS la cible utilisateur
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  // effective = 300 → v = 300/16 = 18,75 (et non 500/16 = 31,25).
  TEST_ASSERT_TRUE(d.doseNow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 18.75f, d.stopTargetMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 300.0f / 240.0f, d.plannedFlowMlPerMin);

  // Cumul == plafond → plus rien à injecter, même si la cible est plus haute.
  in.dailyInjectedMl = 300.0f;
  ScheduledDoseDecision d2 = evaluateScheduledDose(in);
  TEST_ASSERT_FALSE(d2.doseNow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 300.0f, d2.stopTargetMl);  // injecté + 0
  TEST_ASSERT_TRUE(isnan(d2.plannedFlowMlPerMin));
}

// -----------------------------------------------------------------------------
// (7) Gardes fail-closed (conditions n°2 et n°4) : refus = {false, -1, 0, NAN}.
// -----------------------------------------------------------------------------
static void assertScheduledRefusal(const ScheduledDoseDecision& d) {
  TEST_ASSERT_FALSE(d.doseNow);
  TEST_ASSERT_EQUAL_INT(-1, d.windowIndex);   // force le recalcul au retour en plage
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 0.0f, d.stopTargetMl);
  TEST_ASSERT_TRUE(isnan(d.plannedFlowMlPerMin));
}

void test_F011_gardes_fail_closed(void) {
  // Watchdog inactif (condition n°4 : vérifié DANS la fonction pure).
  {
    ScheduledDoseInputs in = validScheduledInputs();
    in.watchdogActive = false;
    assertScheduledRefusal(evaluateScheduledDose(in));
  }
  // Horizon nul (hors plage) — condition n°2 : JAMAIS nWin=1 via max(1,0).
  {
    ScheduledDoseInputs in = validScheduledInputs();
    in.horizonMinutes = 0;
    assertScheduledRefusal(evaluateScheduledDose(in));
  }
  // Horizon négatif → même refus.
  {
    ScheduledDoseInputs in = validScheduledInputs();
    in.horizonMinutes = -5;
    assertScheduledRefusal(evaluateScheduledDose(in));
  }
  // Débit effectif non configuré (0) → refus (division/durée impossibles).
  {
    ScheduledDoseInputs in = validScheduledInputs();
    in.effectiveFlowMlPerMin = 0.0f;
    assertScheduledRefusal(evaluateScheduledDose(in));
  }
  // Débit négatif → même refus.
  {
    ScheduledDoseInputs in = validScheduledInputs();
    in.effectiveFlowMlPerMin = -1.0f;
    assertScheduledRefusal(evaluateScheduledDose(in));
  }
  // windowMinutes = 0 → refus (garde défensive anti division par zéro).
  {
    ScheduledDoseInputs in = validScheduledInputs();
    in.windowMinutes = 0;
    assertScheduledRefusal(evaluateScheduledDose(in));
  }
}

// Horizon d'1 minute en toute fin de journée (23:59) : plancher nWin=1 sain,
// pas de division par zéro, tout le restant planifié sur l'unique fenêtre.
void test_F011_horizon_une_minute_fin_de_journee(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.nowMin = 1439;              // 23:59
  in.horizonMinutes = 1;         // borné à minuit
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  // nWin = 1/15 = 0 → plancher 1 (horizon > 0 garanti) → v = 300 mL.
  TEST_ASSERT_TRUE(d.doseNow);
  TEST_ASSERT_EQUAL_INT(95, d.windowIndex);                      // 1439/15
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 300.0f, d.stopTargetMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 300.0f, d.plannedFlowMlPerMin);  // 300/1
}

// -----------------------------------------------------------------------------
// (8) Même fenêtre (prevWindowIndex == windowIndex) : stopTargetMl CONSERVÉ,
//     aucun recalcul — le cumul progresse vers la cible d'arrêt figée.
// -----------------------------------------------------------------------------
void test_F011_meme_fenetre_conserve_stop_target(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.prevWindowIndex = 40;         // == 600/15 → même fenêtre
  in.prevStopTargetMl = 123.45f;   // valeur arbitraire ≠ de tout recalcul possible
  in.dailyInjectedMl = 100.0f;
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  TEST_ASSERT_EQUAL_INT(40, d.windowIndex);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 123.45f, d.stopTargetMl);  // conservé tel quel
  TEST_ASSERT_TRUE(d.doseNow);                                   // 100 < 123,45

  // Cumul atteint la cible d'arrêt → arrêt (frontière STRICT < : égalité = stop).
  in.dailyInjectedMl = 123.45f;
  d = evaluateScheduledDose(in);
  TEST_ASSERT_FALSE(d.doseNow);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 123.45f, d.stopTargetMl);
}

// -----------------------------------------------------------------------------
// (9) plannedFlowMlPerMin (diagnostic WS) : remaining/horizon si restant > 0,
//     NAN sinon (quota atteint ou dépassé).
// -----------------------------------------------------------------------------
void test_F011_planned_flow_diagnostic(void) {
  // Restant partiel : (300 - 60) / 240 = 1,0 mL/min.
  ScheduledDoseInputs in = validScheduledInputs();
  in.dailyInjectedMl = 60.0f;
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 1.0f, d.plannedFlowMlPerMin);

  // Quota exactement atteint → NAN.
  in.dailyInjectedMl = 300.0f;
  d = evaluateScheduledDose(in);
  TEST_ASSERT_TRUE(isnan(d.plannedFlowMlPerMin));

  // Quota dépassé (remaining clampé à 0) → NAN aussi.
  in.dailyInjectedMl = 350.0f;
  d = evaluateScheduledDose(in);
  TEST_ASSERT_TRUE(isnan(d.plannedFlowMlPerMin));
  TEST_ASSERT_FALSE(d.doseNow);
}

// =============================================================================
// feature-163 (gate pool-chemistry G3/G6) — garde CalibrationSessionOpen
// =============================================================================
// Entrée fail-closed : posée À CÔTÉ de watchdogActive. Le mode Programmé ne
// passe PAS par canDose()/evaluateDose() — c'est le SEUL point où cette garde
// peut fermer ce chemin (condition G3 du gate pool-chemistry).

// Session ouverte seule, tout le reste OK → refus fail-closed (même forme
// que watchdog/horizon/débit : {false, -1, 0, NAN}).
void test_F163_scheduled_refusal_calibration_session_open(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.calSessionOpen = true;
  assertScheduledRefusal(evaluateScheduledDose(in));
}

// Session ouverte ET plan déjà en cours de fenêtre (prevWindowIndex valide,
// cumul loin de la cible) → refus fail-closed quand même (aucune exception).
void test_F163_scheduled_session_ouverte_devant_plan_en_cours(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.calSessionOpen = true;
  in.prevWindowIndex = 40;         // même fenêtre que nowMin=600 → 600/15
  in.prevStopTargetMl = 200.0f;
  in.dailyInjectedMl = 50.0f;      // largement sous la cible d'arrêt
  assertScheduledRefusal(evaluateScheduledDose(in));
}

// Non-régression : watchdog inactif seul (session FERMÉE) bloque toujours de
// façon identique — le champ ajouté ne change rien au chemin existant.
void test_F163_scheduled_watchdog_toujours_bloquant_session_fermee(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.calSessionOpen = false;
  in.watchdogActive = false;
  assertScheduledRefusal(evaluateScheduledDose(in));
}

// Session FERMÉE, watchdog actif, tout le reste OK : AUCUN refus nouveau —
// non-régression explicite du champ ajouté.
void test_F163_scheduled_session_fermee_aucun_refus_nouveau(void) {
  ScheduledDoseInputs in = validScheduledInputs();
  in.calSessionOpen = false;
  ScheduledDoseDecision d = evaluateScheduledDose(in);
  TEST_ASSERT_TRUE(d.doseNow);
}

// =============================================================================
// feature-053 — Mode Boost : cible ORP et limite chlore effectives PURES
// =============================================================================
// Constantes de référence RECOPIÉES À LA MAIN depuis constants.h (valeurs
// attendues : 60 / 850 / 1.5 / 1000). La recopie est VOLONTAIRE — c'est elle qui
// fait le verrou : une dérive de constants.h doit faire échouer ces tests, pas
// les suivre silencieusement. Préfixe `kRef` (v2.25.1) pour cohabiter avec
// l'inclusion de constants.h ajoutée en tête de fichier.
// L'effet boost est STRICTEMENT réservé au mode ORP "automatic".
static const float kRefBoostOrpDeltaMv = 60.0f;
static const float kRefBoostOrpCeilingMv = 850.0f;
static const float kRefBoostDailyFactor = 1.5f;
static const float kRefBoostDailyHardCapMl = 1000.0f;

// -----------------------------------------------------------------------------
// effectiveOrpTargetPure
// -----------------------------------------------------------------------------
void test_F053_orp_boost_off_target_unchanged(void) {
  // Boost inactif → cible inchangée (même si mode automatic).
  float t = effectiveOrpTargetPure(720.0f, false, true,
                                   kRefBoostOrpDeltaMv, kRefBoostOrpCeilingMv);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 720.0f, t);
}

void test_F053_orp_boost_on_automatic_adds_delta(void) {
  // Boost actif + automatic → cible + 60 mV (sous le plafond).
  float t = effectiveOrpTargetPure(720.0f, true, true,
                                   kRefBoostOrpDeltaMv, kRefBoostOrpCeilingMv);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 780.0f, t);
}

void test_F053_orp_boost_ceiling_caps(void) {
  // 820 + 60 = 880 → plafonné à 850.
  float t = effectiveOrpTargetPure(820.0f, true, true,
                                   kRefBoostOrpDeltaMv, kRefBoostOrpCeilingMv);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 850.0f, t);
}

void test_F053_orp_target_above_ceiling_never_lowered(void) {
  // Cible déjà > plafond (880) → RESTE 880, jamais abaissée (condition #5).
  float t = effectiveOrpTargetPure(880.0f, true, true,
                                   kRefBoostOrpDeltaMv, kRefBoostOrpCeilingMv);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 880.0f, t);
}

void test_F053_orp_boost_on_non_automatic_unchanged(void) {
  // Boost actif mais mode NON automatic → cible inchangée (condition #1).
  float t = effectiveOrpTargetPure(720.0f, true, false,
                                   kRefBoostOrpDeltaMv, kRefBoostOrpCeilingMv);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 720.0f, t);
}

void test_F053_orp_boundary_exact_ceiling(void) {
  // Frontière : 790 + 60 = 850 exact (= plafond) → 850.
  float t = effectiveOrpTargetPure(790.0f, true, true,
                                   kRefBoostOrpDeltaMv, kRefBoostOrpCeilingMv);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 850.0f, t);
}

// -----------------------------------------------------------------------------
// effectiveMaxChlorinePure
// -----------------------------------------------------------------------------
void test_F053_chlorine_boost_off_unchanged(void) {
  // Boost inactif → limite inchangée (même si mode automatic).
  float m = effectiveMaxChlorinePure(500.0f, false, true,
                                     kRefBoostDailyFactor, kRefBoostDailyHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 500.0f, m);
}

void test_F053_chlorine_boost_on_automatic_x_factor(void) {
  // Boost actif + automatic → limite × 1.5 (500 → 750, sous le hard cap).
  float m = effectiveMaxChlorinePure(500.0f, true, true,
                                     kRefBoostDailyFactor, kRefBoostDailyHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 750.0f, m);
}

void test_F053_chlorine_hard_cap(void) {
  // 800 × 1.5 = 1200 → plafonné en dur à 1000 (condition #1).
  float m = effectiveMaxChlorinePure(800.0f, true, true,
                                     kRefBoostDailyFactor, kRefBoostDailyHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 1000.0f, m);
}

void test_F053_chlorine_boost_on_non_automatic_unchanged(void) {
  // Boost actif mais mode NON automatic → limite inchangée (condition #1).
  float m = effectiveMaxChlorinePure(500.0f, true, false,
                                     kRefBoostDailyFactor, kRefBoostDailyHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 500.0f, m);
}

void test_F053_chlorine_boundary_near_hard_cap(void) {
  // Frontière : 667 × 1.5 = 1000,5 → plafonné à 1000 (juste au-dessus du cap).
  float m = effectiveMaxChlorinePure(667.0f, true, true,
                                     kRefBoostDailyFactor, kRefBoostDailyHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(kFloatEps, 1000.0f, m);
}

// =============================================================================
// feature-060 — Limite cycles/jour configurable : garde 9 avec maxCyclesPerDay
// paramétré + sanitization clampCyclesPerDay (bornes kCyclesPerDayMin/Max)
// =============================================================================
// Verdict pool-chemistry 2026-07-29 (GO sous conditions) : défaut 48, bornes en
// dur 10-60 (constants.h), clamp côté serveur. On verrouille ici :
//   - la frontière >= de la garde 9 (auto) et de la garde MaxCyclesPerDay
//     (manuel) avec la NOUVELLE valeur 48 (D1a/D1b) ;
//   - le comportement de clampCyclesPerDay (hors bornes, bornes exactes,
//     négatif) et son flag wasClamped (D1c/D1d) ;
//   - la source PARTAGÉE auto+manuel : une même valeur de maxCyclesPerDay
//     produit le même seuil de refus des deux côtés (D1e) ;
//   - la non-régression d'ordre : un refus horaire ou pause mélange survient
//     AVANT la garde 9 (D1f — voir note sur le compteur ci-dessous).
static const unsigned int kNewMaxCyclesPerDay = 48u;  // nouveau défaut feature-060
// NB : on ne peut pas inclure constants.h ici (collision avec les littéraux
// F053 épinglés localement). Les bornes 10/60 de constants.h sont vérifiées
// TRANSITIVEMENT : clampCyclesPerDay() consomme kCyclesPerDayMin/Max, donc les
// asserts 5→10 / 100→60 / 10→10 / 60→60 ci-dessous échoueraient si les
// constantes déviaient du verdict pool-chemistry.

// D1a — garde 9 AUTO avec maxCyclesPerDay=48 : 48e démarrage (cyclesToday=47)
// admis, 49e (cyclesToday=48) refusé CyclesPerDay (frontière >=).
void test_F060_auto_guard9_47_allowed_48_refused(void) {
  DoseInputs in = validInputs();
  in.maxCyclesPerDay = kNewMaxCyclesPerDay;
  in.cyclesToday = 47u;                       // 47 < 48 → admis
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::None, d.cause);

  in.cyclesToday = 48u;                       // 48 >= 48 → refus
  d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CyclesPerDay, d.cause);
  // Cohérence hystérésis de démarrage (même source maxCyclesPerDay) :
  // 47 → démarrage possible, 48 → bloqué.
  TEST_ASSERT_TRUE(shouldStartDosingPure(1.0f, kStartThreshold, 47u, kNewMaxCyclesPerDay));
  TEST_ASSERT_FALSE(shouldStartDosingPure(1.0f, kStartThreshold, 48u, kNewMaxCyclesPerDay));
}

// D1b — garde MaxCyclesPerDay MANUELLE avec maxCyclesPerDay=48 : mêmes
// frontières que l'auto (compteur cyclesToday PARTAGÉ auto+manuel).
void test_F060_manual_guard_47_allowed_48_refused(void) {
  ManualInjectInputs in = validManualInputs();
  in.maxCyclesPerDay = kNewMaxCyclesPerDay;
  in.cyclesToday = 47u;                       // 47 < 48 → admis
  ManualInjectDecision d = evaluateManualInject(in);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::None, d.cause);

  in.cyclesToday = 48u;                       // 48 >= 48 → refus
  d = evaluateManualInject(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::MaxCyclesPerDay, d.cause);
}

// D1c — clampCyclesPerDay : valeurs hors bornes clampées + wasClamped=true.
void test_F060_clamp_out_of_bounds(void) {
  bool clamped = false;
  // Sous le plancher : 5 → 10.
  TEST_ASSERT_EQUAL_UINT(10u, clampCyclesPerDay(5L, &clamped));
  TEST_ASSERT_TRUE(clamped);
  // Au-dessus du plafond : 100 → 60.
  clamped = false;
  TEST_ASSERT_EQUAL_UINT(60u, clampCyclesPerDay(100L, &clamped));
  TEST_ASSERT_TRUE(clamped);
  // Négatif : -1 → 10 (jamais de conversion signée hasardeuse).
  clamped = false;
  TEST_ASSERT_EQUAL_UINT(10u, clampCyclesPerDay(-1L, &clamped));
  TEST_ASSERT_TRUE(clamped);
  // wasClamped optionnel : nullptr toléré (pas de crash, même résultat).
  TEST_ASSERT_EQUAL_UINT(60u, clampCyclesPerDay(100L));
}

// D1d — bornes exactes acceptées telles quelles, wasClamped=false.
void test_F060_clamp_exact_bounds_untouched(void) {
  bool clamped = true;
  // Plancher exact : 10 → 10 sans clamp.
  TEST_ASSERT_EQUAL_UINT(10u, clampCyclesPerDay(10L, &clamped));
  TEST_ASSERT_FALSE(clamped);
  // Plafond exact : 60 → 60 sans clamp.
  clamped = true;
  TEST_ASSERT_EQUAL_UINT(60u, clampCyclesPerDay(60L, &clamped));
  TEST_ASSERT_FALSE(clamped);
  // Défaut usine : 48 → 48 sans clamp.
  clamped = true;
  TEST_ASSERT_EQUAL_UINT(48u, clampCyclesPerDay(48L, &clamped));
  TEST_ASSERT_FALSE(clamped);
}

// D1e — source PARTAGÉE : la MÊME valeur maxCyclesPerDay (issue d'un clamp
// config, ex. 100 → 60) injectée dans les entrées AUTO et MANUELLES produit le
// MÊME seuil de refus des deux côtés (test conjoint, aux deux frontières).
void test_F060_shared_limit_same_threshold_auto_and_manual(void) {
  bool wasClamped = false;
  unsigned int limit = clampCyclesPerDay(100L, &wasClamped);  // → 60
  TEST_ASSERT_TRUE(wasClamped);
  TEST_ASSERT_EQUAL_UINT(60u, limit);

  DoseInputs autoIn = validInputs();
  ManualInjectInputs manIn = validManualInputs();
  autoIn.maxCyclesPerDay = limit;
  manIn.maxCyclesPerDay = limit;

  // limit-1 : admis des DEUX côtés.
  autoIn.cyclesToday = limit - 1u;
  manIn.cyclesToday = limit - 1u;
  DoseDecision da = evaluateDose(autoIn);
  ManualInjectDecision dm = evaluateManualInject(manIn);
  TEST_ASSERT_TRUE(da.allowed);
  TEST_ASSERT_TRUE(dm.allowed);
  TEST_ASSERT_EQUAL(da.allowed, dm.allowed);  // même verdict à la même frontière

  // limit : refusé des DEUX côtés, causes homologues cycles/jour.
  autoIn.cyclesToday = limit;
  manIn.cyclesToday = limit;
  da = evaluateDose(autoIn);
  dm = evaluateManualInject(manIn);
  TEST_ASSERT_FALSE(da.allowed);
  TEST_ASSERT_FALSE(dm.allowed);
  TEST_ASSERT_EQUAL(DoseRefusal::CyclesPerDay, da.cause);
  TEST_ASSERT_EQUAL(ManualInjectRefusal::MaxCyclesPerDay, dm.cause);
}

// D1f — NON-RÉGRESSION « un refus ne consomme pas de cycle ».
// L'incrément de cyclesToday vit dans la COQUILLE pump_controller (au start
// effectif du cycle), pas dans la logique pure : evaluateDose/evaluateManualInject
// prennent leurs entrées par const& et ne modifient AUCUN compteur — un refus ne
// peut donc pas consommer de cycle ici par construction. Le chemin coquille
// (incrément uniquement au démarrage réel) n'est PAS testable en natif (globals
// ESP32/FreeRTOS) → couverture on-target via le plan de test manuel de la spec.
// Ce qu'on verrouille en natif : les refus horaire et pause mélange surviennent
// AVANT la garde 9 (le refus est attribué à la bonne garde, jamais à CyclesPerDay
// tant que cyclesToday < max — donc aucun « cycle » n'est facturé à la garde 9).
void test_F060_hourly_and_mixing_refusals_precede_guard9(void) {
  // Pause mélange (5b) + compteur juste sous la limite → MixingActive.
  {
    DoseInputs in = validInputs();
    in.maxCyclesPerDay = kNewMaxCyclesPerDay;
    in.cyclesToday = 47u;
    in.mixingActive = true;
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(DoseRefusal::MixingActive, d.cause);
  }
  // Limite horaire (8) + compteur juste sous la limite → HourlyLimit.
  {
    DoseInputs in = validInputs();
    in.maxCyclesPerDay = kNewMaxCyclesPerDay;
    in.cyclesToday = 47u;
    in.usedMs = in.hourlyLimitMs;   // 600000 >= 600000 → garde 8
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(DoseRefusal::HourlyLimit, d.cause);
  }
  // Priorité quand la garde 9 est AUSSI en échec : mélange (5b) et horaire (8)
  // priment sur CyclesPerDay (9) — ordre figé characterization feature-036.
  {
    DoseInputs in = validInputs();
    in.maxCyclesPerDay = kNewMaxCyclesPerDay;
    in.cyclesToday = 48u;           // garde 9 en échec aussi
    in.mixingActive = true;
    TEST_ASSERT_EQUAL(DoseRefusal::MixingActive, evaluateDose(in).cause);
  }
  {
    DoseInputs in = validInputs();
    in.maxCyclesPerDay = kNewMaxCyclesPerDay;
    in.cyclesToday = 48u;
    in.usedMs = in.hourlyLimitMs;
    TEST_ASSERT_EQUAL(DoseRefusal::HourlyLimit, evaluateDose(in).cause);
  }
}

// =============================================================================
// bug-dosage-fantome (défaut n°2) — comptabilisation d'un démarrage de cycle
// =============================================================================
// Rejoue le SÉQUENCEMENT réel d'un tour de régulation automatique de
// PumpControllerClass::update() pour une pompe :
//   1. décision  : shouldStartDosingPure / shouldContinueDosingPure ;
//   2. débit     : garde temporelle de computePID (dt <= 0 ou dt > 10 s → 0,
//                  reproduite ici comme dans la coquille) + computePidPure ;
//   3. comptage  : evaluateCycleStartAccounting (cyclesToday, ring anti-rafale,
//                  latch de `active`).
// Le défaut : ~29 min séparent deux cycles, donc le PREMIER tour de chaque cycle
// rend toujours un débit nul (dt aberrant rejeté). L'ancien code comptabilisait
// le cycle sur la seule décision → `active` non latché → second comptage au tour
// suivant (2 lignes de journal, 2 entrées anti-rafale, budget cycles/jour de
// moitié). Ces tests verrouillent : UN démarrage réel = UN cycle comptabilisé.

// v2.26.0 : la simulation prend les constantes de PRODUCTION (elle modélisait un
// P pur — `computePidPure(kSimKp, 0.0f, …)` avec un integralMax de 100 inventé —
// ce qui est devenu FAUX depuis la levée de Ki sur le pH). Elle importe donc
// désormais kPhKpNormal / kPhKiNormal / kPhIntegralMaxNormal : le séquencement
// testé ici (comptabilisation d'un cycle) reste inchangé, mais il est vérifié sur
// le correcteur RÉEL. `kSimKiZero` conserve la variante P pur, encore exacte pour
// l'ORP et utile en comparaison.
static const float kSimKp = kPhKpNormal;       // Kp pH « normal »
static const float kSimKi = kPhKiNormal;       // Ki pH « normal » (v2.26.0)
static const float kSimKiZero = 0.0f;          // variante P pur (ORP / historique)
// Itération 2 (bug-dosage-fantome) : bornes du PID alignées sur la production —
// le modèle linéaire est mort. minFlow = 0 (plus de clamp vers le haut, le
// seuil C de planPulse joue le deadband bas) ; maxFlow = débit MESURÉ à
// l'amplitude calibrée (24,7 ml/min interpolé à duty 127, pesée 2026-08-23).
static const float kSimMinFlow = 0.0f;         // plancher minFlow supprimé
static const float kSimMaxFlow = 24.7f;        // débit calibré à l'amplitude (127)
static const float kSimIntegralMax = kPhIntegralMaxNormal;  // plafond de production

struct DosingTickSim {
  bool active;                 // DosingState.active
  unsigned int cyclesToday;    // DosingState.cyclesToday
  int ringEntries;             // nombre d'appels à recordDosingCycleStart()
  int startLogs;               // nombre de lignes « Démarrage dosage » émises
  unsigned long lastStartMs;   // DosingState.lastStartTime
  unsigned long pidLastTimeMs; // PIDController.lastTime (0 = jamais appelé)
  float pidIntegral;
  float pidLastError;
};

static void simInit(DosingTickSim& s) {
  s.active = false;
  s.cyclesToday = 0u;
  s.ringEntries = 0;
  s.startLogs = 0;
  s.lastStartMs = 0UL;
  s.pidLastTimeMs = 0UL;
  s.pidIntegral = 0.0f;
  s.pidLastError = 0.0f;
}

// Un tour de update() ; renvoie le débit commandé (mL/min, 0 = pompe à l'arrêt).
// `ki` est explicite depuis v2.26.0 : simTick() = configuration pH de production
// (PI), simTickP() = variante P pur conservée pour l'ORP et la comparaison.
static float simTickKi(DosingTickSim& s, float error, unsigned long nowMs, float ki) {
  const bool wasActive = s.active;
  const bool shouldDose =
      wasActive ? shouldContinueDosingPure(error, kStopThreshold,
                                           nowMs - s.lastStartMs, kMinInjectionMs)
                : shouldStartDosingPure(error, kStartThreshold, s.cyclesToday,
                                        kMaxCyclesPerDay);
  const bool startDecided = !wasActive && shouldDose;

  // --- Débit : coquille computePID (gestion du temps) + cœur pur ---
  float flow = 0.0f;
  if (shouldDose) {
    if (s.pidLastTimeMs == 0UL) {
      s.pidLastTimeMs = nowMs;      // première invocation → débit nul
      s.pidLastError = error;
    } else {
      float dt = (float)(nowMs - s.pidLastTimeMs) / 1000.0f;
      if (dt <= 0.0f || dt > 10.0f) {
        s.pidLastTimeMs = nowMs;    // delta aberrant → débit nul (le défaut n°2)
      } else {
        PidResult r = computePidPure(kSimKp, ki, 0.0f, error, s.pidLastError,
                                     s.pidIntegral, dt, kSimIntegralMax,
                                     kStartThreshold, kSimMinFlow, kSimMaxFlow,
                                     false);
        s.pidIntegral = r.integral;
        s.pidLastError = r.lastError;
        s.pidLastTimeMs = nowMs;
        flow = r.flow;
      }
    }
  }

  // --- Comptabilisation différée + latch de `active` ---
  CycleStartAccounting acc =
      evaluateCycleStartAccounting(wasActive, startDecided, flow > 0.0f);
  if (acc.countCycleStart) {
    s.lastStartMs = nowMs;
    s.cyclesToday++;
    s.ringEntries++;
    s.startLogs++;
  }
  // Latch sur le débit réellement commandé, exactement comme la production latche
  // sur phActive/orpActive : le test ne doit pas dépendre d'un champ que la
  // production ne lit pas (cf. retrait de `latchActive`, revue 2.20.5).
  s.active = (flow > 0.0f);
  return flow;
}

// Configuration pH de PRODUCTION (PI) — celle qu'utilisent tous les scénarios de
// comptabilisation ci-dessous depuis v2.26.0.
static float simTick(DosingTickSim& s, float error, unsigned long nowMs) {
  return simTickKi(s, error, nowMs, kSimKi);
}

// Variante P pur (ki = 0) : encore exacte pour l'ORP, conservée pour comparaison.
static float simTickP(DosingTickSim& s, float error, unsigned long nowMs) {
  return simTickKi(s, error, nowMs, kSimKiZero);
}

// Table de vérité de la fonction pure : le comptage suit EXACTEMENT le latch.
void test_BUG2_accounting_pure_truth_table(void) {
  // Démarrage décidé mais débit nul → aucun comptage, `active` non latché.
  CycleStartAccounting a = evaluateCycleStartAccounting(false, true, false);
  TEST_ASSERT_FALSE(a.countCycleStart);
  // Démarrage décidé ET débit non nul → comptage unique, `active` latché.
  a = evaluateCycleStartAccounting(false, true, true);
  TEST_ASSERT_TRUE(a.countCycleStart);
  // Cycle déjà actif (poursuite) → jamais de comptage, quel que soit le débit.
  a = evaluateCycleStartAccounting(true, false, true);
  TEST_ASSERT_FALSE(a.countCycleStart);
  a = evaluateCycleStartAccounting(true, false, false);
  TEST_ASSERT_FALSE(a.countCycleStart);
  // Aucun démarrage décidé, aucun débit → état inchangé.
  a = evaluateCycleStartAccounting(false, false, false);
  TEST_ASSERT_FALSE(a.countCycleStart);
}

// Scénario du bug : premier tour à débit nul, puis tour à débit non nul.
// Attendu : UN SEUL cycle comptabilisé, UNE SEULE entrée anti-rafale.
void test_BUG2_first_tick_zero_flow_counts_one_cycle(void) {
  DosingTickSim s;
  simInit(s);
  const float error = 0.85f;        // pH 8,05 pour une cible 7,20 (trace on-target)
  unsigned long t = 1000000UL;

  // Tour 1 : le PID n'a pas d'instant de référence → débit nul.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, simTick(s, error, t));
  TEST_ASSERT_FALSE(s.active);
  TEST_ASSERT_EQUAL_UINT(0u, s.cyclesToday);   // aucun cycle consommé à vide
  TEST_ASSERT_EQUAL_INT(0, s.ringEntries);
  TEST_ASSERT_EQUAL_INT(0, s.startLogs);

  // Tour 2, 1 s plus tard : débit non nul → le cycle est comptabilisé, une fois.
  t += 1000UL;
  TEST_ASSERT_TRUE(simTick(s, error, t) > 0.0f);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT(1u, s.cyclesToday);
  TEST_ASSERT_EQUAL_INT(1, s.ringEntries);
  TEST_ASSERT_EQUAL_INT(1, s.startLogs);       // une seule ligne de journal

  // Tours de poursuite : aucun comptage supplémentaire.
  for (int i = 0; i < 10; ++i) {
    t += 1000UL;
    TEST_ASSERT_TRUE(simTick(s, error, t) > 0.0f);
  }
  TEST_ASSERT_EQUAL_UINT(1u, s.cyclesToday);
  TEST_ASSERT_EQUAL_INT(1, s.ringEntries);
  TEST_ASSERT_EQUAL_INT(1, s.startLogs);
}

// Non-régression : deux cycles légitimement espacés (~29 min) comptent DEUX
// cycles — y compris avec le tour à débit nul en tête de chaque cycle.
void test_BUG2_two_spaced_cycles_count_two(void) {
  DosingTickSim s;
  simInit(s);
  unsigned long t = 1000000UL;
  const float errHigh = 0.85f;

  // --- Cycle 1 ---
  simTick(s, errHigh, t);                 // débit nul (première invocation PID)
  t += 1000UL;
  simTick(s, errHigh, t);                 // démarrage effectif
  TEST_ASSERT_EQUAL_UINT(1u, s.cyclesToday);
  for (int i = 0; i < 35; ++i) {           // dépasser le temps min d'injection
    t += 1000UL;
    simTick(s, errHigh, t);
  }
  // Erreur retombée sous le seuil d'arrêt → fin du cycle 1.
  t += 1000UL;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, simTick(s, 0.0f, t));
  TEST_ASSERT_FALSE(s.active);
  TEST_ASSERT_EQUAL_UINT(1u, s.cyclesToday);

  // --- 29 min plus tard : cycle 2 (dt aberrant au premier tour) ---
  t += 29UL * 60UL * 1000UL;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, simTick(s, errHigh, t));  // dt > 10 s → débit nul
  TEST_ASSERT_EQUAL_UINT(1u, s.cyclesToday);              // pas de comptage à vide
  t += 1000UL;
  TEST_ASSERT_TRUE(simTick(s, errHigh, t) > 0.0f);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_UINT(2u, s.cyclesToday);              // DEUX cycles, pas trois
  TEST_ASSERT_EQUAL_INT(2, s.ringEntries);
  TEST_ASSERT_EQUAL_INT(2, s.startLogs);
}

// Un démarrage décidé alors que l'erreur retombe avant le premier débit non nul
// ne doit consommer AUCUN cycle (ni entrée anti-rafale) : la pompe n'a jamais
// tourné.
void test_BUG2_aborted_start_consumes_no_cycle(void) {
  DosingTickSim s;
  simInit(s);
  unsigned long t = 1000000UL;
  simTick(s, 0.85f, t);        // décision de démarrer, débit nul
  t += 1000UL;
  simTick(s, 0.0f, t);         // erreur retombée sous le seuil → pas de démarrage
  TEST_ASSERT_FALSE(s.active);
  TEST_ASSERT_EQUAL_UINT(0u, s.cyclesToday);
  TEST_ASSERT_EQUAL_INT(0, s.ringEntries);
  TEST_ASSERT_EQUAL_INT(0, s.startLogs);
}

// v2.26.0 — Ce que la levée de Ki change dans CE séquencement : l'intégrale
// SURVIT d'un cycle à l'autre (rien ne la remet à zéro entre deux sessions de
// dosage), donc le second cycle démarre avec une autorité déjà acquise. C'est
// précisément le mécanisme qui résorbe la droop… et celui qu'un futur « reset en
// fin de cycle » détruirait sans casser aucun autre test.
void test_BUG2_pi_integrale_survit_entre_deux_cycles(void) {
  DosingTickSim pi, p;
  simInit(pi);
  simInit(p);
  unsigned long t = 1000000UL;
  const float errHigh = 0.85f;

  // Cycle 1 identique pour les deux configurations.
  for (int i = 0; i < 40; ++i) {
    simTick(pi, errHigh, t);
    simTickP(p, errHigh, t);
    t += 1000UL;
  }
  // L'intégrale du PI s'est chargée ; celle du P pur aussi (ki=0 ⇒ sans effet).
  TEST_ASSERT_TRUE(pi.pidIntegral > 0.0f);
  TEST_ASSERT_EQUAL_UINT(1u, pi.cyclesToday);
  TEST_ASSERT_EQUAL_UINT(1u, p.cyclesToday);   // comptage INCHANGÉ sous PI

  // Fin de cycle, 29 min d'attente, puis cycle 2.
  simTick(pi, 0.0f, t);
  simTickP(p, 0.0f, t);
  const float integralApresCycle1 = pi.pidIntegral;
  TEST_ASSERT_TRUE(integralApresCycle1 > 0.0f);  // aucune purge en fin de cycle

  t += 29UL * 60UL * 1000UL;
  simTick(pi, errHigh, t);                        // dt aberrant → débit nul
  simTickP(p, errHigh, t);
  t += 1000UL;
  const float flowPi = simTick(pi, errHigh, t);
  const float flowP  = simTickP(p, errHigh, t);
  TEST_ASSERT_TRUE(flowPi > 0.0f && flowP > 0.0f);
  // Le PI démarre le cycle 2 plus haut que le P pur : l'écart vaut ki × intégrale.
  TEST_ASSERT_TRUE(flowPi > flowP);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, kPhKiNormal * pi.pidIntegral, flowPi - flowP);
  // Et le comptage de cycles reste strictement identique dans les deux mondes.
  TEST_ASSERT_EQUAL_UINT(p.cyclesToday, pi.cyclesToday);
  TEST_ASSERT_EQUAL_INT(p.ringEntries, pi.ringEntries);
}

// =============================================================================
// Itération 2 (bug-dosage-fantome, option B+C) — proportionnel de temps
// =============================================================================
// Valeurs de référence : pompe 1 (pH) pesée le 2026-08-23 — 61,0 ml/min à 255 ;
// hypothèse de recalibration à l'amplitude 127 (50 %) : 24,7 ml/min (interpolé).
// Constantes de SCÉNARIO (pas des miroirs de constants.h) : période 60 000 ms,
// pouls min 5 000 ms, kick 400 ms, MIN_ACTIVE_DUTY 98 (Dmin 82 + 16 counts P2).
//
// ⚠️ kTMinPulseMs = 5 000 est un PARAMÈTRE passé aux fonctions pures, PAS la
// valeur de production. Depuis v2.25.3, `kPulseMinMs` vaut 10 000 ms : les
// scénarios à 5 s restent délibérément en place (ils verrouillent le
// comportement de planPulse / planPulseWithCarry pour un pouls minimal
// QUELCONQUE), et le réglage de production est couvert par la section
// « v2.25.3 » en fin de fichier, qui balaye 5 s / 10 s / 20 s.

static const uint32_t kTPeriodMs = 60000;
static const uint32_t kTMinPulseMs = 5000;   // paramètre de scénario (cf. NB)
static const uint32_t kTKickMs = 400;
static const uint8_t  kTMinActiveDuty = 98;
static const uint8_t  kTCalDuty = 127;   // amplitude 50 % = duty calibré
static const float    kTAmpFlow = 24.7f; // débit mesuré à 127
static const float    kTFlow255 = 61.0f; // débit mesuré à 255 (kick)

// --- evaluatePulseGate : fail-closed de caractérisation --------------------
void test_PULSE_gate_cal_duty_zero_refuses(void) {
  // Défaut usine cal_duty = 0 → non caractérisé, quel que soit le reste.
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(127, 0, kTAmpFlow, kTMinActiveDuty));
}

void test_PULSE_gate_amplitude_differs_from_calibrated_refuses(void) {
  // Le débit n'est mesuré QU'À l'amplitude calibrée : 160 ≠ 127 → refus.
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(160, kTCalDuty, kTAmpFlow, kTMinActiveDuty));
  // Amplitude == calibrée et débit plausible → GO.
  TEST_ASSERT_EQUAL(PulseRefusal::None,
                    evaluatePulseGate(kTCalDuty, kTCalDuty, kTAmpFlow, kTMinActiveDuty));
}

void test_PULSE_gate_B2_amplitude_below_min_active(void) {
  // Condition B2 : amplitude < 98 → refus EXPLICITE (jamais de dégradation
  // silencieuse sous le seuil de rotation), même calibrée à cette valeur.
  TEST_ASSERT_EQUAL(PulseRefusal::AmplitudeBelowMinActive,
                    evaluatePulseGate(82, 82, 12.0f, kTMinActiveDuty));
  TEST_ASSERT_EQUAL(PulseRefusal::AmplitudeBelowMinActive,
                    evaluatePulseGate(97, 97, 12.0f, kTMinActiveDuty));
  // Frontière : 98 == MIN_ACTIVE_DUTY passe (si calibré à 98).
  TEST_ASSERT_EQUAL(PulseRefusal::None,
                    evaluatePulseGate(98, 98, 13.0f, kTMinActiveDuty));
}

void test_PULSE_gate_implausible_flow_refuses(void) {
  // Débit nul, négatif, NaN ou > kPulseFlowMaxMlPerMin (200) → non caractérisé.
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(kTCalDuty, kTCalDuty, 0.0f, kTMinActiveDuty));
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(kTCalDuty, kTCalDuty, -1.0f, kTMinActiveDuty));
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(kTCalDuty, kTCalDuty, NAN, kTMinActiveDuty));
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(kTCalDuty, kTCalDuty, 250.0f, kTMinActiveDuty));
}

// --- planPulse : seuil C exact ----------------------------------------------
void test_PULSE_plan_seuil_C_exact(void) {
  // Seuil C à l'amplitude 127 (24,7 ml/min) : pouls = 60 000 × mean / 24,7.
  // À 2,1 ml/min → 5 101 ms ≥ 5 000 → ACCEPTÉ.
  PulsePlan p = planPulse(2.1f, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(5101u, p.pulseMs);
  // À 2,0 ml/min → 4 858 ms < 5 000 → REFUSÉ (deadband seuil C), pulseMs = 0.
  p = planPulse(2.0f, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_TRUE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);
  // Entrées invalides → refus fail-closed.
  TEST_ASSERT_TRUE(planPulse(0.0f, kTAmpFlow, kTPeriodMs, kTMinPulseMs).refused);
  TEST_ASSERT_TRUE(planPulse(NAN, kTAmpFlow, kTPeriodMs, kTMinPulseMs).refused);
  TEST_ASSERT_TRUE(planPulse(6.4f, 0.0f, kTPeriodMs, kTMinPulseMs).refused);
}

void test_PULSE_plan_fraction_100_pouls_egale_periode(void) {
  // Demande ≥ débit d'amplitude → pompe continue : pouls = période entière.
  PulsePlan p = planPulse(kTAmpFlow, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(kTPeriodMs, p.pulseMs);
  p = planPulse(90.0f, kTAmpFlow, kTPeriodMs, kTMinPulseMs);  // saturation franche
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(kTPeriodMs, p.pulseMs);
}

void test_PULSE_plan_scenario_terrain_6_4_sur_24_7(void) {
  // Rejeu du scénario terrain : erreur 0,8 pH × Kp 8 = 6,4 ml/min demandés.
  // 60 000 × 6,4 / 24,7 = 15 546,56 → arrondi 15 547 ms (~15,5 s de pouls/min).
  PulsePlan p = planPulse(6.4f, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(15547u, p.pulseMs);
}

// --- pulseDeltaVolumeMl : volume exact, indépendant du jitter ---------------
void test_PULSE_delta_volume_a_cheval_sur_le_kick(void) {
  // Delta [start, start+1000 ms] : 400 ms de kick à 61 ml/min + 600 ms de
  // plateau à 24,7 → 61×400/60000 + 24,7×600/60000 = 0,40667 + 0,247 mL.
  const uint32_t start = 100000u;
  float v = pulseDeltaVolumeMl(start, kTKickMs, start, start + 1000u, kTFlow255, kTAmpFlow);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.65367f, v);
  // Décomposition : kick seul puis plateau seul — la somme DOIT être identique
  // (indépendance au découpage en ticks, donc au jitter de loop).
  float vKick = pulseDeltaVolumeMl(start, kTKickMs, start, start + 400u, kTFlow255, kTAmpFlow);
  float vPlat = pulseDeltaVolumeMl(start, kTKickMs, start + 400u, start + 1000u, kTFlow255, kTAmpFlow);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.40667f, vKick);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.247f, vPlat);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, v, vKick + vPlat);
  // Delta nul → volume nul.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pulseDeltaVolumeMl(start, kTKickMs, start, start, kTFlow255, kTAmpFlow));
}

// --- tickPulse : wrap millis (periodStart proche de UINT32_MAX) -------------
void test_PULSE_tick_wrap_millis(void) {
  const uint32_t startNearWrap = 0xFFFFF000u;  // 4 294 963 200 (wrap dans 4 096 ms)
  const uint32_t pulseMs = 15547u;
  PulseState st = {startNearWrap, startNearWrap, true};

  // t = start + 200 ms : dans le kick.
  PulseTick tk = tickPulse(st, startNearWrap + 200u, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_TRUE(tk.pumpOn);
  TEST_ASSERT_TRUE(tk.kickPhase);
  // t = start + 400 ms : frontière du kick (exclusif) → plateau.
  tk = tickPulse(tk.next, startNearWrap + 400u, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_TRUE(tk.pumpOn);
  TEST_ASSERT_FALSE(tk.kickPhase);
  // t = start + 8 192 ms — APRÈS le wrap de millis (0x1000 + 0x1000) : toujours
  // dans le pouls, l'arithmétique non signée doit rester cohérente.
  tk = tickPulse(tk.next, startNearWrap + 8192u /* wrappe en 0x1000 */, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_TRUE(tk.pumpOn);
  TEST_ASSERT_FALSE(tk.kickPhase);
  // t = start + pulseMs (post-wrap) : fin de pouls → pompe OFF.
  tk = tickPulse(tk.next, startNearWrap + pulseMs, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_FALSE(tk.pumpOn);
  TEST_ASSERT_FALSE(tk.next.pulsing);
  // t = start + 60 000 (post-wrap) : nouvelle période → nouveau pouls + kick.
  tk = tickPulse(tk.next, startNearWrap + kTPeriodMs, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_TRUE(tk.pumpOn);
  TEST_ASSERT_TRUE(tk.kickPhase);
  TEST_ASSERT_EQUAL_UINT32(startNearWrap + kTPeriodMs, tk.next.periodStartMs);
}

// --- Rejeu d'une période complète : ON-time et volume conformes au plan -----
void test_PULSE_periode_complete_on_time_et_volume(void) {
  // Simule la coquille : tick toutes les 100 ms sur une période, comptabilité
  // par delta (attribution ARRIÈRE : crédit si la pompe était ON au tick
  // précédent). Attendu : ON-time == pulseMs (à un tick près) et volume ==
  // kick à 61 + plateau à 24,7 (le kick SUR-livre légèrement vs 6,4 demandé —
  // surestimation fail-safe P4, jamais de sous-compte).
  const uint32_t pulseMs = 15547u;
  PulseState st = {0u, 0u, true};  // session amorcée à t=0
  uint32_t onTimeMs = 0;
  float volMl = 0.0f;
  bool prevOn = false;
  uint32_t prevT = 0;
  for (uint32_t t = 0; t <= kTPeriodMs; t += 100u) {
    if (prevOn) {
      onTimeMs += (t - prevT);
      volMl += pulseDeltaVolumeMl(st.pulseStartMs, kTKickMs, prevT, t, kTFlow255, kTAmpFlow);
    }
    PulseTick tk = tickPulse(st, t, kTPeriodMs, pulseMs, kTKickMs);
    st = tk.next;
    prevOn = tk.pumpOn;
    prevT = t;
  }
  // ON-time : pulseMs arrondi au tick supérieur (la coquille coupe au tick suivant).
  TEST_ASSERT_UINT32_WITHIN(100u, pulseMs, onTimeMs);
  // Volume géométrique attendu : 61×0,4/60 + 24,7×15,147/60 = 0,4067 + 6,2355 mL.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 6.642f, volMl);
  // Jamais de sous-compte vs la demande de 6,4 mL/min sur 1 min (P4).
  TEST_ASSERT_TRUE(volMl >= 6.4f);
}

// =============================================================================
// Audit test-engineer (itération 2) — cas de bord complémentaires
// =============================================================================

// --- Seuil C : frontière FINE (encadrement à 0,01 mL/min près) ---------------
// Seuil exact = ampFlow × minPulse / period, ici avec le POULS MINIMAL DU
// SCÉNARIO (kTMinPulseMs = 5 000, cf. NB en tête de section — pas la constante
// prod) : 24,7 × 5000 / 60000 = 2,0583 mL/min.
// 2,05 → pouls 4 980 ms < 5 000 → REFUSÉ ; 2,06 → 5 004 ms ≥ 5 000 → ACCEPTÉ.
// La dérivation du seuil À PARTIR de minPulseMs (donc son doublement à 10 s)
// est verrouillée par test_V253_seuil_de_bascule_derive_du_pouls_minimal.
void test_PULSE_plan_seuil_C_frontiere_fine(void) {
  PulsePlan p = planPulse(2.05f, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_TRUE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);
  p = planPulse(2.06f, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(5004u, p.pulseMs);  // 60000 × 2,06 / 24,7 = 5004,05
  // ampFlow négatif → refus fail-closed (garde d'entrée, symétrique de 0).
  TEST_ASSERT_TRUE(planPulse(6.4f, -1.0f, kTPeriodMs, kTMinPulseMs).refused);
}

// --- Invariant : AUCUN plan accepté ne produit un pouls < minPulse ni > période.
// Conséquence structurelle : le kick (400 ms) tient TOUJOURS dans un pouls
// accepté (minPulse 5 000 ms >> 400 ms) — jamais de pouls tronqué au kick.
void test_PULSE_plan_jamais_de_pouls_hors_bornes(void) {
  const float means[] = {0.01f, 0.5f, 1.0f, 2.0f, 2.06f, 3.0f, 6.4f,
                         12.0f, 20.0f, 24.69f, 24.7f, 30.0f, 90.0f};
  for (float m : means) {
    PulsePlan p = planPulse(m, kTAmpFlow, kTPeriodMs, kTMinPulseMs);
    if (p.refused) {
      TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);  // refus = fail-closed strict
    } else {
      TEST_ASSERT_TRUE(p.pulseMs >= kTMinPulseMs);
      TEST_ASSERT_TRUE(p.pulseMs <= kTPeriodMs);
      TEST_ASSERT_TRUE(p.pulseMs >= kTKickMs);  // le kick tient toujours
    }
  }
}

// --- Gate : +inf refusé (complément du NaN déjà couvert) ----------------------
void test_PULSE_gate_flow_infini_refuse(void) {
  TEST_ASSERT_EQUAL(PulseRefusal::FlowUncharacterized,
                    evaluatePulseGate(kTCalDuty, kTCalDuty, INFINITY, kTMinActiveDuty));
}

// --- pulseDeltaVolumeMl : delta ENTIÈREMENT dans le kick, ENTIÈREMENT plateau
//     loin du kick, et t0 AVANT le pouls (hors contrat → plateau conservateur).
void test_PULSE_delta_volume_segments_purs(void) {
  const uint32_t start = 100000u;
  // [start+100, start+300] : 200 ms entièrement DANS le kick →
  // 61 × 200 / 60000 = 0,203333 mL.
  float v = pulseDeltaVolumeMl(start, kTKickMs, start + 100u, start + 300u, kTFlow255, kTAmpFlow);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.20333f, v);
  // [start+20000, start+21000] : 1000 ms entièrement PLATEAU (loin du kick) →
  // 24,7 × 1000 / 60000 = 0,411667 mL.
  v = pulseDeltaVolumeMl(start, kTKickMs, start + 20000u, start + 21000u, kTFlow255, kTAmpFlow);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.41167f, v);
  // t0 AVANT pulseStart (hors contrat documenté) : r0 wrappe très grand → tout
  // le delta est crédité PLATEAU — conservateur, JAMAIS zéro (P4) :
  // 24,7 × 200 / 60000 = 0,082333 mL.
  v = pulseDeltaVolumeMl(start, kTKickMs, start - 100u, start + 100u, kTFlow255, kTAmpFlow);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.08233f, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

// --- tickPulse : pulseMs == periodMs EXACTEMENT (fraction 100 %) --------------
// La pompe doit rester ON en CONTINU à travers la frontière de période (le
// pouls EST la période) ; le kick se ré-arme à chaque nouvelle période.
void test_PULSE_tick_pouls_egale_periode_pompe_continue(void) {
  PulseState st = {0u, 0u, true};
  const uint32_t pulseMs = kTPeriodMs;  // 60 000 == 60 000
  // Balayage sur 2 périodes : ON à CHAQUE tick, aucun trou.
  for (uint32_t t = 0; t <= 2u * kTPeriodMs; t += 500u) {
    PulseTick tk = tickPulse(st, t, kTPeriodMs, pulseMs, kTKickMs);
    TEST_ASSERT_TRUE(tk.pumpOn);
    st = tk.next;
  }
  // Frontière exacte : à t = periodMs, nouvelle période ré-ancrée + kick ré-armé.
  st = {0u, 0u, true};
  PulseTick tk = tickPulse(st, kTPeriodMs, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_TRUE(tk.pumpOn);
  TEST_ASSERT_TRUE(tk.kickPhase);
  TEST_ASSERT_EQUAL_UINT32(kTPeriodMs, tk.next.periodStartMs);
}

// --- tickPulse : pouls PLUS COURT que le kick (défensif) ----------------------
// planPulse ne peut pas le produire (minPulse 5 s >> kick 0,4 s — verrouillé par
// test_PULSE_plan_jamais_de_pouls_hors_bornes), mais tickPulse doit rester sain
// si on le lui impose : ON en kick pendant tout le pouls, OFF dès pulseMs.
void test_PULSE_tick_pouls_plus_court_que_kick(void) {
  PulseState st = {0u, 0u, true};
  const uint32_t pulseMs = 200u;  // < kickMs (400)
  PulseTick tk = tickPulse(st, 100u, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_TRUE(tk.pumpOn);
  TEST_ASSERT_TRUE(tk.kickPhase);   // tout le pouls est en phase kick
  tk = tickPulse(tk.next, 200u, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_FALSE(tk.pumpOn);     // fin de pouls à 200 ms, pas à 400
  TEST_ASSERT_FALSE(tk.next.pulsing);
  tk = tickPulse(tk.next, 300u, kTPeriodMs, pulseMs, kTKickMs);
  TEST_ASSERT_FALSE(tk.pumpOn);     // reste OFF jusqu'à la prochaine période
}

// =============================================================================
// feature-073 — planPulseWithCarry : accumulateur de dosage sous le pouls
// minimal (amende le seuil C de l'ADR-0043).
// =============================================================================
// Décisions gate verrouillées ici :
//   n°1 — V_pouls de référence KICK-INCLUS : TOUJOURS dérivé de
//         pulseDeltaVolumeMl(0, kickMs, 0, minPulseMs, flow255, ampFlow),
//         jamais recalculé à la main dans les assertions ;
//   F1  — entrées invalides → refus fail-closed ET carryMl = 0 ;
//   F2  — retour ≥ plancher → carry ABANDONNÉ, délégation bit-identique à
//         planPulse ;
//   F3  — tir dès cumul ≥ V_pouls (égalité INCLUSE), résidu conservé,
//         invariant de sortie carryMl < V_pouls, structurellement ≤ 1 tir
//         par période (1 appel = 1 période).

// Constantes du scénario de référence du gate (0,7 ml/min @ amplitude 25).
static const float kCFAmpFlow = 25.0f;   // débit mesuré à l'amplitude calibrée
static const float kCFFlow255 = 61.0f;   // débit mesuré à 255 (kick)

// V_pouls de référence kick-inclus — LA fonction de référence (décision n°1).
static float carryVPulseMl(void) {
  return pulseDeltaVolumeMl(0u, kTKickMs, 0u, kTMinPulseMs, kCFFlow255, kCFAmpFlow);
}

// Incrément de cumul d'une période, MÊME formule flottante que l'implémentation
// (mL = mL/min × ms ÷ 60000) pour des assertions d'exactitude bit à bit.
static float carryIncMl(float meanFlow) {
  return meanFlow * (float)kTPeriodMs / 60000.0f;
}

// --- (a) Cumul exact : chaque appel sous-plancher ajoute demande×période/60000 -
void test_F073_cumul_exact_par_periode(void) {
  const float vP = carryVPulseMl();
  TEST_ASSERT_TRUE(vP > 0.0f);
  float carry = 0.0f;
  // 0,7 ml/min : trois périodes d'accumulation pure (3 × 0,7 = 2,1 < V_pouls).
  for (int i = 1; i <= 3; ++i) {
    PulseCarryPlan p = planPulseWithCarry(0.7f, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, carry);
    TEST_ASSERT_FALSE(p.refused);
    TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);   // période d'accumulation, pompe OFF
    TEST_ASSERT_FALSE(p.carryFired);
    // Exactitude : même arithmétique float que l'implémentation, comparaison stricte.
    float expected = carry + carryIncMl(0.7f);
    TEST_ASSERT_EQUAL_FLOAT(expected, p.carryMl);
    carry = p.carryMl;
  }
  TEST_ASSERT_TRUE(carry < vP);  // invariant de sortie
}

// --- (b) Déclenchement à l'ÉGALITÉ exacte cumul == V_pouls ---------------------
// Construction : inc = 0,5 ml (exactement représentable via 0,5 ml/min ×
// 60000/60000) et carryIn = V_pouls − 0,5 ; (vP − 0,5) + 0,5 == vP en float
// (0,5 est une puissance de 2, la soustraction est exacte autour de 2 ml).
void test_F073_tir_a_egalite_exacte(void) {
  const float vP = carryVPulseMl();
  const float inc = carryIncMl(0.5f);
  TEST_ASSERT_EQUAL_FLOAT(0.5f, inc);          // pré-condition de la construction
  const float carryIn = vP - 0.5f;
  TEST_ASSERT_EQUAL_FLOAT(vP, carryIn + inc);  // l'égalité EST atteinte en float
  PulseCarryPlan p = planPulseWithCarry(0.5f, kCFAmpFlow, kCFFlow255,
                                        kTPeriodMs, kTMinPulseMs, kTKickMs, carryIn);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_TRUE(p.carryFired);                        // égalité INCLUSE → tir
  TEST_ASSERT_EQUAL_UINT32(kTMinPulseMs, p.pulseMs);     // pouls minimal EXACTEMENT
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);              // vP − vP = 0, résidu nul
}

// --- (c) Résidu conservé après tir + invariant carryMl < V_pouls à CHAQUE appel
void test_F073_residu_conserve_et_invariant_sortie(void) {
  const float vP = carryVPulseMl();
  // Tir avec dépassement : carryIn + inc > V_pouls → résidu = (carryIn+inc)−vP,
  // même ordre d'opérations flottantes que l'implémentation.
  const float carryIn = vP - 0.25f;
  const float meanFlow = 0.7f;
  PulseCarryPlan p = planPulseWithCarry(meanFlow, kCFAmpFlow, kCFFlow255,
                                        kTPeriodMs, kTMinPulseMs, kTKickMs, carryIn);
  TEST_ASSERT_TRUE(p.carryFired);
  TEST_ASSERT_EQUAL_UINT32(kTMinPulseMs, p.pulseMs);
  float expectedResidue = (carryIn + carryIncMl(meanFlow)) - vP;
  TEST_ASSERT_EQUAL_FLOAT(expectedResidue, p.carryMl);
  TEST_ASSERT_TRUE(p.carryMl > 0.0f);   // le résidu n'est PAS perdu
  TEST_ASSERT_TRUE(p.carryMl < vP);     // invariant de sortie

  // Invariant balayé : 200 périodes chaînées à demandes sous-plancher variées,
  // carryMl < V_pouls en sortie de CHAQUE appel, tir = minPulseMs exactement.
  float carry = 0.0f;
  for (int i = 0; i < 200; ++i) {
    float demand = 0.1f + 1.9f * (float)(i % 11) / 10.0f;  // 0,1 → 2,0 ml/min
    PulseCarryPlan q = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, carry);
    TEST_ASSERT_FALSE(q.refused);
    TEST_ASSERT_TRUE(q.carryMl >= 0.0f);
    TEST_ASSERT_TRUE(q.carryMl < vP);                      // invariant de sortie
    if (q.carryFired) {
      TEST_ASSERT_EQUAL_UINT32(kTMinPulseMs, q.pulseMs);
    } else {
      TEST_ASSERT_EQUAL_UINT32(0u, q.pulseMs);
    }
    carry = q.carryMl;
  }
}

// --- (d) Scénario long du gate : 0,7 ml/min @ amp 25 / f255 61, 60 périodes ----
// V_pouls kick-inclus = (61×400 + 25×4600)/60000 ≈ 2,3233 ml > 2,0833 nominal →
// cadence dérivée de la FORMULE : intervalle moyen V_pouls/0,7 ≈ 3,32 périodes
// (tirs espacés de 3 OU 4 périodes — PAS le « toutes les 3 » de la spec initiale,
// corrigé au gate). Critère ±5 % sur le VOLUME RESTITUÉ moyen.
void test_F073_scenario_long_0_7_ml_min_volume_et_cadence(void) {
  const float vP = carryVPulseMl();
  const int nPeriods = 60;
  const float demand = 0.7f;
  float carry = 0.0f;
  int fires = 0;
  int lastFirePeriod = -1;
  float delivered = 0.0f;
  for (int i = 0; i < nPeriods; ++i) {
    PulseCarryPlan p = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, carry);
    TEST_ASSERT_FALSE(p.refused);
    TEST_ASSERT_TRUE(p.carryMl < vP);  // invariant à chaque période
    if (p.carryFired) {
      ++fires;
      TEST_ASSERT_EQUAL_UINT32(kTMinPulseMs, p.pulseMs);
      // Volume RÉELLEMENT restitué par ce tir : Σ pulseDeltaVolumeMl (réf. gate).
      delivered += pulseDeltaVolumeMl(0u, kTKickMs, 0u, kTMinPulseMs,
                                      kCFFlow255, kCFAmpFlow);
      // Cadence conforme à la formule : intervalle ∈ {3, 4} périodes
      // (⌈vP/0,7⌉ et ⌊vP/0,7⌋+1) — jamais 2 tirs rapprochés.
      if (lastFirePeriod >= 0) {
        int gap = i - lastFirePeriod;
        TEST_ASSERT_TRUE(gap >= 3);
        TEST_ASSERT_TRUE(gap <= 4);
      } else {
        TEST_ASSERT_EQUAL_INT(3, i);  // premier tir à la 4e période (indice 3)
      }
      lastFirePeriod = i;
    } else {
      TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);  // jamais de pouls hors tir
    }
    carry = p.carryMl;
  }
  // Nombre de tirs dérivé de la formule : ⌊demande×N / V_pouls⌋ = ⌊42/2,3233⌋ = 18.
  const float totalDemandMl = demand * (float)nPeriods;  // 42 ml sur 60 min
  TEST_ASSERT_EQUAL_INT((int)(totalDemandMl / vP), fires);
  // Critère d'acceptation : volume restitué moyen = demande à ±5 %.
  TEST_ASSERT_TRUE(delivered >= totalDemandMl * 0.95f);
  TEST_ASSERT_TRUE(delivered <= totalDemandMl * 1.05f);
}

// --- (e) F2 : retour ≥ plancher avec carry non nul → délégation bit-identique --
void test_F073_F2_retour_plancher_delegation_bit_identique(void) {
  const float vP = carryVPulseMl();
  const float carryIn = 0.75f * vP;  // dette accumulée non nulle
  // Demande franchement au-dessus du plancher (2,0833 ml/min à amp 25).
  const float demand = 6.4f;
  PulsePlan base = planPulse(demand, kCFAmpFlow, kTPeriodMs, kTMinPulseMs);
  TEST_ASSERT_FALSE(base.refused);   // pré-condition : régime proportionnel
  PulseCarryPlan p = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                        kTPeriodMs, kTMinPulseMs, kTKickMs, carryIn);
  // Champ à champ identique à planPulse, carry ABANDONNÉ (pas fusionné).
  TEST_ASSERT_EQUAL(base.refused, p.refused);
  TEST_ASSERT_EQUAL_UINT32(base.pulseMs, p.pulseMs);
  TEST_ASSERT_FALSE(p.carryFired);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);
  // Fraction ≥ 100 % (demande ≥ ampFlow) : pouls = période entière, carry abandonné.
  p = planPulseWithCarry(30.0f, kCFAmpFlow, kCFFlow255,
                         kTPeriodMs, kTMinPulseMs, kTKickMs, carryIn);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_EQUAL_UINT32(kTPeriodMs, p.pulseMs);
  TEST_ASSERT_FALSE(p.carryFired);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);
}

// --- (f) F1 : entrées invalides → refus fail-closed, carryMl = 0 ---------------
void test_F073_entrees_invalides_fail_closed(void) {
  const float dette = 1.5f;  // dette préexistante : elle doit DISPARAÎTRE
  // Demande morte ou non finie.
  const float badMean[] = {0.0f, -1.0f, NAN, INFINITY};
  for (float m : badMean) {
    PulseCarryPlan p = planPulseWithCarry(m, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, dette);
    TEST_ASSERT_TRUE(p.refused);
    TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);
    TEST_ASSERT_FALSE(p.carryFired);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);  // F1 : la dette ne survit pas
  }
  // ampFlow ≤ 0 ou non fini (NaN/±inf refusés par la même garde que planPulse).
  const float badAmp[] = {0.0f, -5.0f, NAN, INFINITY};
  for (float a : badAmp) {
    PulseCarryPlan p = planPulseWithCarry(0.7f, a, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, dette);
    TEST_ASSERT_TRUE(p.refused);
    TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);
    TEST_ASSERT_FALSE(p.carryFired);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);  // F1 : la dette ne survit pas
  }
  // Période nulle.
  PulseCarryPlan p = planPulseWithCarry(0.7f, kCFAmpFlow, kCFFlow255,
                                        0u, kTMinPulseMs, kTKickMs, dette);
  TEST_ASSERT_TRUE(p.refused);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);
}

// --- (f bis) Carry d'entrée suspect (NaN, négatif, ≥ V_pouls) → reset défensif -
// Le plan repart de ZÉRO (résultat identique à carryIn = 0) : jamais de rafale
// de rattrapage sur une dette douteuse.
void test_F073_carry_entree_suspect_reset_defensif(void) {
  const float vP = carryVPulseMl();
  const float demand = 0.7f;
  PulseCarryPlan ref = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, 0.0f);
  const float badCarry[] = {NAN, -0.5f, vP, 10.0f * vP, INFINITY};
  for (float c : badCarry) {
    PulseCarryPlan p = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs, c);
    TEST_ASSERT_EQUAL(ref.refused, p.refused);
    TEST_ASSERT_EQUAL_UINT32(ref.pulseMs, p.pulseMs);   // pas de tir volé
    TEST_ASSERT_EQUAL(ref.carryFired, p.carryFired);    // (0,7 < vP : pas de tir)
    TEST_ASSERT_EQUAL_FLOAT(ref.carryMl, p.carryMl);    // reparti de zéro
  }
}

// --- (g) Domaine de pulseMs : jamais entre 1 et minPulseMs−1 sous le plancher --
void test_F073_pulse_jamais_hors_domaine(void) {
  const float vP = carryVPulseMl();
  // Balayage sous-plancher (demandes 0,05 → 2,05 ml/min) × carries variés :
  // pulseMs ∈ {0, minPulseMs} STRICTEMENT — jamais un pouls court intermédiaire.
  for (int d = 1; d <= 41; ++d) {
    float demand = 0.05f * (float)d;  // < plancher 2,083 ml/min
    for (int c = 0; c <= 9; ++c) {
      float carry = vP * (float)c / 10.0f;  // 0 → 0,9 × V_pouls (entrées valides)
      PulseCarryPlan p = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                            kTPeriodMs, kTMinPulseMs, kTKickMs, carry);
      TEST_ASSERT_FALSE(p.refused);
      TEST_ASSERT_TRUE(p.pulseMs == 0u || p.pulseMs == kTMinPulseMs);
      TEST_ASSERT_EQUAL(p.pulseMs == kTMinPulseMs, p.carryFired);
    }
  }
}

// --- (h) Régime ≥ plancher : balayage [plancher..ampFlow+] ≡ planPulse bit à bit
void test_F073_regime_normal_balayage_identique_planPulse(void) {
  const float vP = carryVPulseMl();
  // 2,09 (juste au-dessus du plancher 2,0831 arrondi inclus) → 27,5 (> ampFlow).
  for (int d = 0; d <= 254; ++d) {
    float demand = 2.09f + 0.1f * (float)d;
    PulsePlan base = planPulse(demand, kCFAmpFlow, kTPeriodMs, kTMinPulseMs);
    if (base.refused) continue;  // on ne teste ICI que le régime proportionnel
    PulseCarryPlan p = planPulseWithCarry(demand, kCFAmpFlow, kCFFlow255,
                                          kTPeriodMs, kTMinPulseMs, kTKickMs,
                                          0.5f * vP);  // carry non nul : abandonné
    TEST_ASSERT_EQUAL(base.refused, p.refused);
    TEST_ASSERT_EQUAL_UINT32(base.pulseMs, p.pulseMs);  // bit-identique
    TEST_ASSERT_FALSE(p.carryFired);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);
  }
}

// =============================================================================
// feature-105 (A3) — GARDE FAIL-CLOSED : référence de volume incalculable
// (pulse_dosing_logic.cpp, `if (!isfinite(vPulseMl) || vPulseMl <= 0.0f)`)
// =============================================================================
// SÉMANTIQUE VERROUILLÉE ICI (validée par pool-chemistry, gate feature-105) :
// quand le volume d'une impulsion minimale ne peut pas être calculé, on REFUSE
// de tirer, ET l'accumulateur est ÉCRASÉ. Le tuple {refused=true, pulseMs=0,
// carryFired=false, carryMl=0} est un refus COMPLET : côté coquille, `refused`
// pose flow = 0, referme la séance et recopie le carry NUL — aucune dette de
// rattrapage ne survit au refus. C'est le point le plus facile à rater : un
// refus qui conserverait la dette produirait une rafale au retour à la normale.
//
// COMMENT ON Y ENTRE — la SEULE entrée réaliste est `kickMs >= minPulseMs` ET
// `flow255MlPerMin <= 0` : l'impulsion minimale est alors intégralement dans la
// fenêtre de kick, et le kick est facturé à un débit à 255 nul (valeur d'usine
// jamais mesurée) ou négatif (configuration corrompue). Le volume de référence
// vaut exactement 0 → garde déclenchée.
//
// MOITIÉ DE BRANCHE VOLONTAIREMENT NON COUVERTE : le sous-terme
// `!isfinite(vPulseMl)`. Il est INATTEIGNABLE par entrée réaliste, parce que
// pulseDeltaVolumeMl assainit déjà NaN/±inf en 0 avant de multiplier — il
// faudrait un flow255 de l'ordre de 1e30 pour faire déborder le produit en
// +inf. Une telle valeur sentinelle ne prouverait que l'EXÉCUTION de la ligne,
// pas un comportement que l'appareil puisse produire. On laisse donc cette
// moitié découverte, avec ce motif écrit.
//
// ÉTAT EN PRODUCTION : avec kPulseKickMs = 400 ms < kPulseMinMs = 10 000 ms,
// cette branche est MORTE. Ce test est un filet contre un futur changement de
// constante — exactement la classe de défaut que ce projet a déjà connue
// (propagation d'une constante de pouls, trouvée par mutation).

void test_F105_volume_reference_incalculable_refus_complet(void) {
  // Entrées du gate pool-chemistry, reprises telles quelles.
  const float carryIn = 3.0f;  // dette bien réelle en entrée
  const uint32_t periodMs = 60000u;
  const uint32_t minPulseMs = 10000u;
  const uint32_t kickMs = 10000u;  // kick ≥ pouls minimal : tout le pouls est du kick

  // Pré-condition explicite : la référence vaut bien 0 avec ces entrées.
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, pulseDeltaVolumeMl(0u, kickMs, 0u, minPulseMs, 0.0f, 12.0f));

  PulseCarryPlan p = planPulseWithCarry(0.5f, 12.0f, /*flow255=*/0.0f, periodMs,
                                        minPulseMs, kickMs, carryIn);
  TEST_ASSERT_TRUE_MESSAGE(p.refused,
                           "reference de volume incalculable : il faut REFUSER de tirer");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, p.pulseMs, "aucun pouls ne doit partir");
  TEST_ASSERT_FALSE_MESSAGE(p.carryFired, "aucun tir ne doit etre impute");
  // LE point du test : la dette d'entrée de 3,0 ml est ÉCRASÉE, pas reportée.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, p.carryMl,
                                  "l'accumulateur doit etre ecrase, pas reporte");

  // Débit à 255 NÉGATIF (configuration corrompue) : même refus complet.
  PulseCarryPlan n = planPulseWithCarry(0.5f, 12.0f, -61.0f, periodMs,
                                        minPulseMs, kickMs, carryIn);
  TEST_ASSERT_TRUE(n.refused);
  TEST_ASSERT_EQUAL_UINT32(0u, n.pulseMs);
  TEST_ASSERT_FALSE(n.carryFired);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, n.carryMl);
}

// Contre-test de NON-RÉGRESSION : mêmes demande, amplitude, période et pouls
// minimal ; seuls flow255 et kickMs redeviennent réalistes (74 ml/min, 400 ms).
// La garde ne doit PAS se déclencher et le mécanisme de cumul doit vivre.
void test_F105_contre_test_garde_non_declenchee(void) {
  const uint32_t periodMs = 60000u;
  const uint32_t minPulseMs = 10000u;
  const uint32_t kickMs = 400u;
  const float ampFlow = 12.0f;
  const float flow255 = 74.0f;

  const float vP = pulseDeltaVolumeMl(0u, kickMs, 0u, minPulseMs, flow255, ampFlow);
  TEST_ASSERT_TRUE_MESSAGE(vP > 0.0f, "reference de volume calculable attendue");

  // Dette 3,0 ml ≥ V_pouls → reset défensif (pas la garde) puis accumulation.
  PulseCarryPlan p = planPulseWithCarry(0.5f, ampFlow, flow255, periodMs,
                                        minPulseMs, kickMs, 3.0f);
  TEST_ASSERT_FALSE_MESSAGE(p.refused, "garde declenchee a tort");
  TEST_ASSERT_EQUAL_UINT32(0u, p.pulseMs);
  TEST_ASSERT_FALSE(p.carryFired);
  TEST_ASSERT_EQUAL_FLOAT(0.5f * (float)periodMs / 60000.0f, p.carryMl);

  // Et une dette licite juste sous le seuil fait bien PARTIR un pouls minimal :
  // la voie de tir reste ouverte quand la référence est calculable.
  PulseCarryPlan t = planPulseWithCarry(0.5f, ampFlow, flow255, periodMs,
                                        minPulseMs, kickMs, vP - 0.1f);
  TEST_ASSERT_FALSE(t.refused);
  TEST_ASSERT_EQUAL_UINT32(minPulseMs, t.pulseMs);
  TEST_ASSERT_TRUE(t.carryFired);
}

// =============================================================================
// v2.25.3 — kPulseMinMs 5 s → 10 s : la MOYENNE restituée est INDÉPENDANTE du
// pouls minimal (seul le rendement physique de l'injection change)
// =============================================================================
// C'est L'affirmation sur laquelle repose l'absence de risque de surdosage :
// planPulseWithCarry accumule `meanFlow × period / 60000` et retire EXACTEMENT
// V_pouls au tir, donc le volume restitué sur une longue série est égal au
// volume demandé, à la dette résiduelle près (< 1 × V_pouls), QUEL QUE SOIT
// minPulseMs. Allonger le pouls ne change que la CADENCE des démarrages.
//
// Ce que cette section protège :
//   1. conservation demandé = restitué + dette, et écart au volume demandé
//      borné par UN pouls (pas par la longueur de la série) — pour 5 s, 10 s
//      ET 20 s : si le décrément au tir n'était pas exactement V_pouls, l'erreur
//      croîtrait avec la série et ces tests tomberaient ;
//   2. l'invariant de sortie `0 ≤ carryMl < V_pouls` à 10 s sur série longue,
//      et le reset défensif (pulse_dosing_logic.cpp:86) RECALÉ sur le nouveau
//      V_pouls — une dette de 2,47 mL (ex-V_pouls à 5 s) est désormais légitime ;
//   3. le domaine du pouls à 10 s : {0} ∪ {minPulseMs} sous la bascule,
//      [minPulseMs, periodMs] au-dessus, jamais deux pouls dans une période ;
//   4. le seuil de bascule DÉRIVÉ de minPulseMs (2,15 → 4,29 mL/min), jamais
//      un littéral 5000 en miroir de la constante firmware.
//
// Valeurs de PRODUCTION du gate pool-chemistry du 28/08/2026 : débit mesuré
// 25,75 mL/min à l'amplitude calibrée, kick crédité au majorant de flotte
// `kPumpFlow255MlPerMin` (74 mL/min) → V_pouls 2,47 mL à 5 s, 4,61 mL à 10 s.
// AUCUNE de ces valeurs n'est écrite en dur dans une assertion : V_pouls est
// TOUJOURS dérivé de pulseDeltaVolumeMl (décision n°1 du gate feature-073).
static const float kV253AmpFlow = 25.75f;                // mesuré à l'amplitude calibrée
static const float kV253Flow255 = kPumpFlow255MlPerMin;  // majorant de flotte (kick)

// V_pouls kick-inclus pour UN pouls minimal donné — LA fonction de référence.
static float v253VPulseMl(uint32_t minPulseMs) {
  return pulseDeltaVolumeMl(0u, kTKickMs, 0u, minPulseMs, kV253Flow255, kV253AmpFlow);
}

// Résultat d'un rejeu de N périodes à demande constante, carry CHAÎNÉ.
struct V253Run {
  float deliveredMl;  // Σ des volumes RÉELS des pouls émis (pulseDeltaVolumeMl)
  float carryEndMl;   // dette encore en attente en fin de série
  int fires;          // nombre de démarrages de pompe
};

// Rejeu de N périodes : assert au passage l'invariant de sortie et le domaine
// du pouls à CHAQUE appel (conditions 2 et 3), et cumule le volume restitué.
static V253Run v253Replay(float demand, uint32_t minPulseMs, int nPeriods) {
  V253Run r = {0.0f, 0.0f, 0};
  const float vP = v253VPulseMl(minPulseMs);
  float carry = 0.0f;
  for (int i = 0; i < nPeriods; ++i) {
    PulseCarryPlan p = planPulseWithCarry(demand, kV253AmpFlow, kV253Flow255,
                                          kTPeriodMs, minPulseMs, kTKickMs, carry);
    TEST_ASSERT_FALSE(p.refused);
    TEST_ASSERT_TRUE(p.carryMl >= 0.0f);
    TEST_ASSERT_TRUE(p.carryMl < vP);  // invariant de sortie, chaque période
    if (p.pulseMs > 0u) {
      ++r.fires;                       // structurellement UN pouls par appel
      TEST_ASSERT_TRUE(p.pulseMs >= minPulseMs);
      TEST_ASSERT_TRUE(p.pulseMs <= kTPeriodMs);
      r.deliveredMl += pulseDeltaVolumeMl(0u, kTKickMs, 0u, p.pulseMs,
                                          kV253Flow255, kV253AmpFlow);
    } else {
      TEST_ASSERT_FALSE(p.carryFired);
    }
    carry = p.carryMl;
  }
  r.carryEndMl = carry;
  return r;
}

// --- (1) LE test du gate : moyenne restituée indépendante de minPulseMs -------
// Demandes choisies SOUS le plancher aux TROIS réglages (plancher à 5 s =
// 25,75 × 5000/60000 = 2,146 mL/min) pour comparer le régime d'accumulation à
// lui-même, et non à un autre régime. Inclut 0,5 mL/min (très faible) et la
// demande terrain 1,76 mL/min. 1 200 périodes = 20 h de régulation continue.
void test_V253_moyenne_restituee_independante_du_pouls_minimal(void) {
  const float demands[] = {0.5f, 1.0f, 1.76f, 2.0f};
  const uint32_t mins[] = {5000u, 10000u, 20000u};
  const int nPeriods = 1200;
  for (float d : demands) {
    const float totalDemandMl = d * (float)nPeriods;  // periodMs = 1 min
    float delivered[3] = {0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 3; ++k) {
      V253Run r = v253Replay(d, mins[k], nPeriods);
      delivered[k] = r.deliveredMl;
      const float vP = v253VPulseMl(mins[k]);
      // (i) CONSERVATION : tout le volume demandé est soit restitué, soit
      // encore en dette. Rien n'est perdu (sous-dosage) ni créé (surdosage).
      // C'est CE point qui rend la moyenne indépendante de minPulseMs.
      TEST_ASSERT_FLOAT_WITHIN(0.05f, totalDemandMl, r.deliveredMl + r.carryEndMl);
      // (ii) Écart au volume demandé borné par UN pouls — PAS par la série.
      // Si le décrément au tir n'était pas exactement V_pouls, l'écart
      // dériverait proportionnellement à nPeriods et cette borne tomberait.
      TEST_ASSERT_TRUE(r.deliveredMl <= totalDemandMl + 0.05f);
      TEST_ASSERT_TRUE(totalDemandMl - r.deliveredMl < vP);
      TEST_ASSERT_TRUE(r.fires > 0);  // la régulation n'est pas morte
    }
    // (iii) Comparaison DIRECTE 5 s ↔ 10 s ↔ 20 s : même volume restitué, à la
    // dette résiduelle du réglage le plus long près (≤ 1 × V_pouls).
    TEST_ASSERT_FLOAT_WITHIN(v253VPulseMl(10000u), delivered[0], delivered[1]);
    TEST_ASSERT_FLOAT_WITHIN(v253VPulseMl(20000u), delivered[0], delivered[2]);
    TEST_ASSERT_FLOAT_WITHIN(v253VPulseMl(20000u), delivered[1], delivered[2]);
  }
}

// --- (1 bis) Demande juste SOUS le nouveau seuil de bascule (4,29 mL/min) -----
// À 4,25 mL/min, le réglage 10 s bascule dans l'accumulateur alors que le
// réglage 5 s reste en proportionnel DIRECT. La moyenne restituée doit rester
// égale à la demande dans les DEUX régimes — c'est le cas le plus sensible car
// c'est celui où le changement de constante change de régime.
void test_V253_moyenne_juste_sous_le_nouveau_seuil_de_bascule(void) {
  const float demand = 4.25f;  // < 4,2917 = 25,75 × 10000/60000
  const int nPeriods = 600;
  const float totalDemandMl = demand * (float)nPeriods;
  // Pré-conditions de régime (dérivées, pas devinées).
  TEST_ASSERT_FALSE(planPulse(demand, kV253AmpFlow, kTPeriodMs, 5000u).refused);
  TEST_ASSERT_TRUE(planPulse(demand, kV253AmpFlow, kTPeriodMs, 10000u).refused);

  // Régime accumulateur (10 s) : conservation exacte, écart ≤ 1 × V_pouls.
  V253Run r10 = v253Replay(demand, 10000u, nPeriods);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, totalDemandMl, r10.deliveredMl + r10.carryEndMl);
  TEST_ASSERT_TRUE(totalDemandMl - r10.deliveredMl < v253VPulseMl(10000u));

  // Régime direct (5 s) : le pouls proportionnel est calculé sur le débit de
  // PLATEAU, alors que le volume réel crédite le kick au majorant de flotte →
  // léger EXCÉDENT systématique par période (majorant volontaire, P4), borné
  // ici pour documenter l'écart. Aucun sous-dosage dans l'un ou l'autre régime.
  V253Run r5 = v253Replay(demand, 5000u, nPeriods);
  const float kickSurplusPerPeriod =
      (kV253Flow255 - kV253AmpFlow) * (float)kTKickMs / 60000.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.5f, totalDemandMl + kickSurplusPerPeriod * (float)nPeriods,
                           r5.deliveredMl);
  TEST_ASSERT_TRUE(r5.deliveredMl >= totalDemandMl);
  // Les deux régimes restituent la demande à ce surplus de kick près : passer
  // à 10 s ne SUR-dose pas, il retire même l'excédent de kick.
  TEST_ASSERT_TRUE(r10.deliveredMl <= r5.deliveredMl);
}

// --- (2) Invariant de sortie à 10 s + reset défensif recalé sur V_pouls -------
void test_V253_invariant_sortie_et_reset_defensif_recales_sur_10s(void) {
  const float vP5 = v253VPulseMl(5000u);
  const float vP10 = v253VPulseMl(10000u);
  // Le seuil de tir a grossi (pas exactement doublé : le kick n'est compté
  // qu'une fois par pouls) — dérivé, jamais écrit en dur.
  TEST_ASSERT_TRUE(vP10 > vP5);
  TEST_ASSERT_TRUE(vP10 < 2.0f * vP5);

  // Série longue à demandes HÉTÉROGÈNES sous le nouveau seuil de bascule :
  // l'invariant 0 ≤ carryMl < V_pouls tient à CHAQUE appel (3 000 périodes,
  // soit ~50 h), et le pouls reste dans son domaine.
  float carry = 0.0f;
  for (int i = 0; i < 3000; ++i) {
    const float demand = 0.05f + 4.20f * (float)(i % 97) / 96.0f;  // 0,05 → 4,25
    PulseCarryPlan p = planPulseWithCarry(demand, kV253AmpFlow, kV253Flow255,
                                          kTPeriodMs, 10000u, kTKickMs, carry);
    TEST_ASSERT_FALSE(p.refused);
    TEST_ASSERT_TRUE(p.carryMl >= 0.0f);
    TEST_ASSERT_TRUE(p.carryMl < vP10);
    TEST_ASSERT_TRUE(p.pulseMs == 0u || p.pulseMs == 10000u);
    carry = p.carryMl;
  }

  // Reset défensif (l.86) RECALÉ : une dette de 2,47 mL — exactement l'ancien
  // V_pouls à 5 s — est LÉGITIME sous le seuil de 10 s. Elle doit SURVIVRE :
  // la remettre à zéro effacerait un demi-pouls de dette au changement de
  // réglage, donc sous-doserait.
  PulseCarryPlan p = planPulseWithCarry(0.5f, kV253AmpFlow, kV253Flow255,
                                        kTPeriodMs, 10000u, kTKickMs, vP5);
  TEST_ASSERT_FALSE(p.refused);
  TEST_ASSERT_FALSE(p.carryFired);                   // vP5 + 0,5 < vP10
  TEST_ASSERT_EQUAL_FLOAT(vP5 + 0.5f, p.carryMl);    // dette CONSERVÉE
  // Frontière : une dette ≥ le NOUVEAU V_pouls viole l'invariant d'entrée →
  // reset à zéro (jamais de rafale de rattrapage).
  p = planPulseWithCarry(0.5f, kV253AmpFlow, kV253Flow255,
                         kTPeriodMs, 10000u, kTKickMs, vP10);
  TEST_ASSERT_FALSE(p.carryFired);
  TEST_ASSERT_EQUAL_FLOAT(0.5f, p.carryMl);
  // Juste SOUS le nouveau V_pouls : dette conservée et tir immédiat.
  p = planPulseWithCarry(0.5f, kV253AmpFlow, kV253Flow255,
                         kTPeriodMs, 10000u, kTKickMs, vP10 - 0.25f);
  TEST_ASSERT_TRUE(p.carryFired);
  TEST_ASSERT_EQUAL_UINT32(10000u, p.pulseMs);
  TEST_ASSERT_EQUAL_FLOAT((vP10 - 0.25f) + 0.5f - vP10, p.carryMl);
  TEST_ASSERT_TRUE(p.carryMl < vP10);
}

// --- (3) Domaine du pouls à 10 s : {0} ∪ {10 000}, et un seul pouls/période ---
void test_V253_pouls_jamais_hors_domaine_a_10s(void) {
  const uint32_t minPulseMs = 10000u;
  const float vP10 = v253VPulseMl(minPulseMs);
  // Sous la bascule (0,05 → 4,25 mL/min) × dettes d'entrée valides : JAMAIS un
  // pouls court intermédiaire (1..9 999 ms), qui serait un pouls non
  // reproductible — exactement ce que la constante est censée interdire.
  for (int d = 1; d <= 85; ++d) {
    const float demand = 0.05f * (float)d;
    for (int c = 0; c <= 9; ++c) {
      const float carry = vP10 * (float)c / 10.0f;
      PulseCarryPlan p = planPulseWithCarry(demand, kV253AmpFlow, kV253Flow255,
                                            kTPeriodMs, minPulseMs, kTKickMs, carry);
      TEST_ASSERT_FALSE(p.refused);
      TEST_ASSERT_TRUE(p.pulseMs == 0u || p.pulseMs == minPulseMs);
      TEST_ASSERT_EQUAL(p.pulseMs == minPulseMs, p.carryFired);
      TEST_ASSERT_TRUE(p.carryMl < vP10);  // pas de dette de rafale reportée
    }
  }
  // Au-dessus de la bascule (4,30 → 34,3 mL/min) : pouls ∈ [10 000, 60 000],
  // carry ABANDONNÉ (F2), et un SEUL pouls par appel donc par période.
  for (int d = 0; d <= 300; ++d) {
    const float demand = 4.30f + 0.1f * (float)d;
    PulseCarryPlan p = planPulseWithCarry(demand, kV253AmpFlow, kV253Flow255,
                                          kTPeriodMs, minPulseMs, kTKickMs,
                                          0.9f * vP10);
    TEST_ASSERT_FALSE(p.refused);
    TEST_ASSERT_TRUE(p.pulseMs >= minPulseMs);
    TEST_ASSERT_TRUE(p.pulseMs <= kTPeriodMs);
    TEST_ASSERT_TRUE(p.pulseMs >= kTKickMs);  // le kick tient toujours (10 s ≫ 0,4 s)
    TEST_ASSERT_FALSE(p.carryFired);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.carryMl);
  }
}

// --- (4) Seuil de bascule DÉRIVÉ de minPulseMs (aucun 5000 en miroir) --------
static void v253AssertSwitchThreshold(uint32_t minPulseMs) {
  // Seuil NOMINAL de bascule = ampFlow × minPulseMs / periodMs.
  const float thr = kV253AmpFlow * (float)minPulseMs / (float)kTPeriodMs;
  PulsePlan below = planPulse(thr * 0.99f, kV253AmpFlow, kTPeriodMs, minPulseMs);
  TEST_ASSERT_TRUE(below.refused);            // → régime accumulateur
  TEST_ASSERT_EQUAL_UINT32(0u, below.pulseMs);
  PulsePlan above = planPulse(thr * 1.01f, kV253AmpFlow, kTPeriodMs, minPulseMs);
  TEST_ASSERT_FALSE(above.refused);           // → proportionnel direct
  TEST_ASSERT_TRUE(above.pulseMs >= minPulseMs);
}

void test_V253_seuil_de_bascule_derive_du_pouls_minimal(void) {
  v253AssertSwitchThreshold(5000u);
  v253AssertSwitchThreshold(10000u);
  v253AssertSwitchThreshold(20000u);
  // Chiffres du gate : 2,15 mL/min à 5 s → 4,29 mL/min à 10 s. Encadrement fin
  // à ±0,01 mL/min autour de la valeur DÉRIVÉE, pour les deux réglages.
  TEST_ASSERT_FALSE(planPulse(2.15f, kV253AmpFlow, kTPeriodMs, 5000u).refused);
  TEST_ASSERT_TRUE(planPulse(2.14f, kV253AmpFlow, kTPeriodMs, 5000u).refused);
  TEST_ASSERT_FALSE(planPulse(4.30f, kV253AmpFlow, kTPeriodMs, 10000u).refused);
  TEST_ASSERT_TRUE(planPulse(4.29f, kV253AmpFlow, kTPeriodMs, 10000u).refused);

  // Demande terrain 1,76 mL/min : SOUS le seuil aux deux réglages → même
  // régime, seule la CADENCE change. Passer à 10 s divise les démarrages par
  // ~1,87 (pas exactement 2 : le kick n'est compté qu'une fois par pouls) POUR
  // LE MÊME volume restitué.
  const int nPeriods = 600;
  V253Run r5 = v253Replay(1.76f, 5000u, nPeriods);
  V253Run r10 = v253Replay(1.76f, 10000u, nPeriods);
  TEST_ASSERT_TRUE(r10.fires < r5.fires);
  // Ratio attendu = V_pouls(5 s) / V_pouls(10 s), dérivé — pas un littéral.
  const float expectedRatio = v253VPulseMl(5000u) / v253VPulseMl(10000u);
  TEST_ASSERT_INT_WITHIN(1, (int)((float)r5.fires * expectedRatio + 0.5f), r10.fires);
  // ... et le VOLUME restitué est le même à un pouls près (l'affirmation clé).
  TEST_ASSERT_FLOAT_WITHIN(v253VPulseMl(10000u), r5.deliveredMl, r10.deliveredMl);
}

// --- (5) Écrêtage fail-closed (l.98) : inatteignable, y compris à 10 s -------
// La borne structurelle est inc/V_pouls < minPulseMs/(minPulseMs − kickMs), soit
// < 1,042 à 10 s : une seule période sous-plancher ne peut JAMAIS financer deux
// pouls, même avec une calibration atypique (flow255 < ampFlow). On le vérifie
// en observant que le résidu après tir vaut EXACTEMENT inc + carryIn − V_pouls
// (aucun écrasement à zéro par l'écrêtage) sur un balayage large.
void test_V253_ecretage_fail_closed_inatteignable_a_10s(void) {
  const uint32_t minPulseMs = 10000u;
  const float amps[] = {5.0f, 13.0f, 25.75f, 60.0f};
  const float f255s[] = {0.0f, 5.0f, 25.75f, 74.0f, 190.0f};
  for (float amp : amps) {
    for (float f255 : f255s) {
      const float vP = pulseDeltaVolumeMl(0u, kTKickMs, 0u, minPulseMs, f255, amp);
      if (!(vP > 0.0f)) continue;  // référence incalculable : cas F1, hors sujet
      const float floorDemand = amp * (float)minPulseMs / (float)kTPeriodMs;
      for (int c = 0; c <= 9; ++c) {
        const float carryIn = vP * (float)c / 10.0f;
        const float demand = floorDemand * 0.999f;  // juste sous la bascule
        PulseCarryPlan p = planPulseWithCarry(demand, amp, f255, kTPeriodMs,
                                              minPulseMs, kTKickMs, carryIn);
        TEST_ASSERT_FALSE(p.refused);
        TEST_ASSERT_TRUE(p.carryMl >= 0.0f);
        TEST_ASSERT_TRUE(p.carryMl < vP);
        const float inc = demand * (float)kTPeriodMs / 60000.0f;
        if (p.carryFired) {
          // Résidu EXACT → l'écrêtage n'a pas mordu (il aurait mis 0).
          TEST_ASSERT_FLOAT_WITHIN(0.001f, (carryIn + inc) - vP, p.carryMl);
        } else {
          TEST_ASSERT_FLOAT_WITHIN(0.001f, carryIn + inc, p.carryMl);
        }
      }
    }
  }
}

// =============================================================================
// V2251 — Qualification des causes de refus par l'état interne du filtre
// =============================================================================
// Contexte du défaut corrigé (v2.25.1) : l'UI devinait la cause par
// sous-chaînes dans une phrase française, et l'ancienne cause FilterNotReady
// conflatait « warmup » et « EZO injoignable » — une VRAIE panne de sonde
// s'affichait donc comme une stabilisation normale. Le firmware sert depuis un
// CODE machine.
//
// feature-152 (refonte de la chaîne de mesure) : la garde séparée « filtre
// prêt »/« capteur instable » (3b/3c de l'ancien canDose(), codes
// FilterNotReady/FilterUnstable/FilterWarmup/FilterFrozen/FilterStale/
// FilterGateClosed) a disparu — la valeur officielle de sensor_filter
// encapsule désormais directement warmup, sonde figée, péremption et porte
// fermée (NaN dans tous ces cas) : UNE SEULE garde (isnan(reading)) suffit,
// qualifiée par les codes ReadingNaN{Warmup,Frozen,Stale,GateClosed} (15-18).
// Ces six codes retirés (4, 5, 19-22) ne sont JAMAIS recyclés (un appareil non
// mis à jour afficherait l'ancien libellé sur un code réattribué) : ce lot ne
// les référence donc plus du tout, y compris dans les tests — un appel au nom
// retiré redeviendrait une erreur de COMPILATION, la garde la plus stricte
// possible contre un recyclage accidentel.
//
// Ce que ces tests protègent, dans l'ordre d'importance :
//   1. le VERDICT (`allowed`) est strictement inchangé, quel que soit
//      `filterState` : la qualification ne renomme que la cause ;
//   2. la priorité « pannes d'abord » de resolveFilterState() ;
//   3. le contrat de sérialisation (valeurs numériques figées).

// --- Oracle de verdict INDÉPENDANT DE L'ORDRE des gardes ---------------------
// Conjonction pure des 12 conditions (14 avant feature-152 : la fusion des
// gardes 3/3b/3c en une seule a retiré deux conditions distinctes) : ne
// reproduit NI l'ordre, NI la structure de evaluateDose. Si l'implémentation
// ajoutait/retirait une condition, l'oracle divergerait.
static bool allGuardsPassOracle(const DoseInputs& in) {
  if (!in.watchdogActive) return false;
  if (!in.waterPresent) return false;
  if (isnan(in.reading)) return false;
  if (in.calPoints < in.requiredPoints) return false;
  if (in.stabilizationActive) return false;
  if (in.mixingActive) return false;
  if (!in.modeAutomatic) return false;
  if (in.dailyInjectedMl >= in.maxDailyMl) return false;
  if (in.hourlyLimitMs > 0 && in.usedMs >= in.hourlyLimitMs) return false;
  if (in.cyclesToday >= in.maxCyclesPerDay) return false;
  if (in.cyclesLastMin >= in.maxCyclesPerMin) return false;
  if (in.cyclesLast15Min >= in.maxCyclesPer15Min) return false;
  return true;
}

// Les 6 valeurs de DoseFilterState (l'énum d'entrée, non sérialisé).
static const DoseFilterState kAllFilterStates[6] = {
    DoseFilterState::Unknown, DoseFilterState::Ready,  DoseFilterState::Warmup,
    DoseFilterState::Frozen,  DoseFilterState::Stale,  DoseFilterState::GateClosed};

// Cause ATTENDUE pour l'état `st`, dérivée de la cause de référence obtenue avec
// filterState = Unknown : seule la famille ReadingNaN change de nom (feature-152
// a retiré la seconde famille, FilterNotReady, avec la garde qu'elle qualifiait).
static DoseRefusal expectedQualified(DoseRefusal ref, DoseFilterState st) {
  if (ref == DoseRefusal::ReadingNaN) {
    switch (st) {
      case DoseFilterState::Warmup:     return DoseRefusal::ReadingNaNWarmup;
      case DoseFilterState::Frozen:     return DoseRefusal::ReadingNaNFrozen;
      case DoseFilterState::Stale:      return DoseRefusal::ReadingNaNStale;
      case DoseFilterState::GateClosed: return DoseRefusal::ReadingNaNGateClosed;
      default:                          return DoseRefusal::ReadingNaN;
    }
  }
  return ref;  // toutes les autres causes IGNORENT filterState
}

// --- Pré-condition d'égalité flottante --------------------------------------
// Le balayage ci-dessous casse les gardes journalière/horaire EN S'APPUYANT sur
// les frontières `>=` à égalité exacte. Si les valeurs choisies n'étaient pas
// représentables en float32, la garde ne se déclencherait pas et le balayage
// passerait au vert en n'exerçant JAMAIS ces branches. On le vérifie AVANT.
void test_V2251_precondition_frontieres_flottantes_exactes(void) {
  const float kMaxDaily = 500.0f;
  const float kAtLimit = 500.0f;
  TEST_ASSERT_TRUE_MESSAGE(kAtLimit == kMaxDaily,
      "500.0f doit etre exactement representable, sinon la garde DailyLimit "
      "n'est jamais exercee par le balayage");
  DoseInputs in = validInputs();
  in.maxDailyMl = kMaxDaily;
  in.dailyInjectedMl = kAtLimit;
  TEST_ASSERT_EQUAL_MESSAGE(DoseRefusal::DailyLimit, evaluateDose(in).cause,
      "frontiere journaliere >= reellement atteinte a egalite");
  in = validInputs();
  in.hourlyLimitMs = 600000UL;
  in.usedMs = 600000UL;
  TEST_ASSERT_EQUAL_MESSAGE(DoseRefusal::HourlyLimit, evaluateDose(in).cause,
      "frontiere horaire >= reellement atteinte a egalite");
}

// --- (1) NON-RÉGRESSION DU VERDICT — balayage 2^12 × 6 états -----------------
// Pour chacune des 4 096 combinaisons de gardes passantes/échouantes (12 bits,
// contre 14 avant feature-152 — la fusion des gardes 3/3b/3c en retire deux),
// et pour chacun des 6 états de filtre :
//   * `allowed` == oracle indépendant (le verdict ne dépend JAMAIS de
//     filterState : aucune condition ajoutée ni retirée par la refonte) ;
//   * `allowed` == (cause == None) (cohérence interne du verdict) ;
//   * la cause est celle du cas Unknown, SAUF pour la famille ReadingNaN*
//     qualifiable — donc aucune garde n'a changé de rang.
void test_V2251_verdict_invariant_balayage_exhaustif(void) {
  static char msg[192];
  uint32_t causesSeen = 0;  // bitmask des DoseRefusal observés
  for (uint32_t mask = 0; mask < (1u << 12); ++mask) {
    DoseInputs base = validInputs();
    base.watchdogActive     =  (mask & (1u << 0)) == 0 ? true : false;
    base.waterPresent       =  (mask & (1u << 1)) == 0 ? true : false;
    base.reading            =  (mask & (1u << 2)) == 0 ? 7.2f : NAN;
    base.calPoints          =  (mask & (1u << 3)) == 0 ? 2 : 1;   // requis 2
    base.stabilizationActive = (mask & (1u << 4)) == 0 ? false : true;
    base.mixingActive       =  (mask & (1u << 5)) == 0 ? false : true;
    base.modeAutomatic      =  (mask & (1u << 6)) == 0 ? true : false;
    base.dailyInjectedMl    =  (mask & (1u << 7)) == 0 ? 100.0f : 500.0f;  // max 500
    base.usedMs             =  (mask & (1u << 8)) == 0 ? 60000UL : 600000UL;
    base.cyclesToday        =  (mask & (1u << 9)) == 0 ? 3u : kMaxCyclesPerDay;
    base.cyclesLastMin      =  (mask & (1u << 10)) == 0 ? 0 : 3;   // max 3
    base.cyclesLast15Min    =  (mask & (1u << 11)) == 0 ? 0 : 8;   // max 8

    const bool expectedAllowed = allGuardsPassOracle(base);

    DoseInputs ref = base;
    ref.filterState = DoseFilterState::Unknown;
    const DoseRefusal refCause = evaluateDose(ref).cause;

    for (int s = 0; s < 6; ++s) {
      DoseInputs in = base;
      in.filterState = kAllFilterStates[s];
      DoseDecision d = evaluateDose(in);
      causesSeen |= (1u << (uint32_t)d.cause);

      if (d.allowed != expectedAllowed) {
        snprintf(msg, sizeof(msg),
                 "mask=%u etat=%d : verdict %d attendu %d (le correctif ne doit "
                 "changer AUCUNE decision de dosage)",
                 mask, s, (int)d.allowed, (int)expectedAllowed);
        TEST_FAIL_MESSAGE(msg);
      }
      if (d.allowed != (d.cause == DoseRefusal::None)) {
        snprintf(msg, sizeof(msg), "mask=%u etat=%d : allowed incoherent avec cause=%d",
                 mask, s, (int)d.cause);
        TEST_FAIL_MESSAGE(msg);
      }
      const DoseRefusal want = expectedQualified(refCause, kAllFilterStates[s]);
      if (d.cause != want) {
        snprintf(msg, sizeof(msg),
                 "mask=%u etat=%d : cause=%d attendue=%d (rang de garde ou "
                 "qualification incorrect)",
                 mask, s, (int)d.cause, (int)want);
        TEST_FAIL_MESSAGE(msg);
      }
    }
  }
  // Le balayage doit atteindre tous les codes ENCORE VALIDES (0-18 moins les
  // deux retirés 4 et 5) : aucun n'est mort, aucun n'est inatteignable (une
  // cause inatteignable = une table UI jamais validée). Les codes 19-22 sont
  // volontairement ABSENTS de cette liste : ils n'existent plus dans l'énum
  // (voir expectedQualified) et ne doivent donc jamais apparaître ici.
  const int kValidCodes[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                             15, 16, 17, 18};
  for (int c : kValidCodes) {
    snprintf(msg, sizeof(msg), "code %d jamais atteint par le balayage", c);
    TEST_ASSERT_TRUE_MESSAGE((causesSeen & (1u << (uint32_t)c)) != 0, msg);
  }
  // Les deux codes retirés ne sont par construction jamais observés (ils ne
  // peuvent plus être produits par evaluateDose) : le vérifier explicitement
  // documente l'absence, plutôt que de la laisser implicite.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, causesSeen & (1u << 4),
      "code 4 (FilterNotReady, retire) jamais recycle");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, causesSeen & (1u << 5),
      "code 5 (FilterUnstable, retire) jamais recycle");
}

// --- (2) resolveFilterState : états simples, chacun atteignable UNE fois -----
void test_V2251_resolve_filter_state_etats_simples(void) {
  // Aucun drapeau → Ready.
  TEST_ASSERT_EQUAL(DoseFilterState::Ready,
                    resolveFilterState(false, false, false, false));
  // Un seul drapeau à la fois → l'état correspondant, exactement une fois.
  TEST_ASSERT_EQUAL(DoseFilterState::Frozen,
                    resolveFilterState(true, false, false, false));
  TEST_ASSERT_EQUAL(DoseFilterState::GateClosed,
                    resolveFilterState(false, true, false, false));
  TEST_ASSERT_EQUAL(DoseFilterState::Stale,
                    resolveFilterState(false, false, true, false));
  TEST_ASSERT_EQUAL(DoseFilterState::Warmup,
                    resolveFilterState(false, false, false, true));
  // Unknown n'est JAMAIS produit par la composition (c'est un défaut d'entrée).
  for (int m = 0; m < 16; ++m) {
    DoseFilterState st = resolveFilterState((m & 1) != 0, (m & 2) != 0,
                                            (m & 4) != 0, (m & 8) != 0);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(DoseFilterState::Unknown, st,
        "resolveFilterState ne doit jamais rendre Unknown");
  }
}

// --- (2 bis) Priorité PANNES D'ABORD, y compris en simultané -----------------
// Cas critique (celui qui a réfuté le premier discriminant proposé) : une sonde
// FIGÉE répond, donc la lecture brute paraît fraîche et la fenêtre se remplit —
// l'état composé doit rester Frozen, JAMAIS Warmup.
void test_V2251_resolve_filter_state_priorite_pannes_dabord(void) {
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::Frozen,
      resolveFilterState(true, false, false, true),
      "sonde figee + fenetre en remplissage => Frozen (une panne ne doit jamais "
      "etre maquillee en stabilisation normale)");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::Frozen,
      resolveFilterState(true, false, true, false), "figee + perimee => Frozen");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::Frozen,
      resolveFilterState(true, true, false, false), "figee + porte fermee => Frozen");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::Frozen,
      resolveFilterState(true, true, true, true), "les 4 => Frozen (panne la plus grave)");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::GateClosed,
      resolveFilterState(false, true, true, false),
      "porte fermee + perime => GateClosed (l'age croit PAR CONCEPTION porte "
      "fermee : accuser la sonde serait faux)");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::GateClosed,
      resolveFilterState(false, true, false, true), "porte fermee + warmup => GateClosed");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::GateClosed,
      resolveFilterState(false, true, true, true), "porte fermee + perime + warmup => GateClosed");
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::Stale,
      resolveFilterState(false, false, true, true),
      "perime + warmup => Stale (sonde muette depuis le boot : panne d'abord)");
}

// --- (2 ter) Balayage exhaustif des 16 combinaisons -------------------------
// Table attendue indépendante : Frozen > GateClosed > Stale > Warmup > Ready.
// Vérifie aussi que les 5 états sont TOUS atteignables.
void test_V2251_resolve_filter_state_balayage_16_combinaisons(void) {
  bool seen[6] = {false, false, false, false, false, false};
  for (int m = 0; m < 16; ++m) {
    const bool frozen = (m & 1) != 0;
    const bool gate = (m & 2) != 0;
    const bool stale = (m & 4) != 0;
    const bool warm = (m & 8) != 0;
    DoseFilterState want = DoseFilterState::Ready;
    if (frozen) want = DoseFilterState::Frozen;
    else if (gate) want = DoseFilterState::GateClosed;
    else if (stale) want = DoseFilterState::Stale;
    else if (warm) want = DoseFilterState::Warmup;
    DoseFilterState got = resolveFilterState(frozen, gate, stale, warm);
    TEST_ASSERT_EQUAL(want, got);
    seen[(int)got] = true;
  }
  TEST_ASSERT_TRUE(seen[(int)DoseFilterState::Ready]);
  TEST_ASSERT_TRUE(seen[(int)DoseFilterState::Warmup]);
  TEST_ASSERT_TRUE(seen[(int)DoseFilterState::Frozen]);
  TEST_ASSERT_TRUE(seen[(int)DoseFilterState::Stale]);
  TEST_ASSERT_TRUE(seen[(int)DoseFilterState::GateClosed]);
  TEST_ASSERT_FALSE(seen[(int)DoseFilterState::Unknown]);
}

// --- Qualification de la garde 3 (lecture NaN) -------------------------------
void test_V2251_qualification_reading_nan_par_etat(void) {
  const DoseFilterState states[4] = {
      DoseFilterState::Warmup, DoseFilterState::Frozen,
      DoseFilterState::Stale, DoseFilterState::GateClosed};
  const DoseRefusal wants[4] = {
      DoseRefusal::ReadingNaNWarmup, DoseRefusal::ReadingNaNFrozen,
      DoseRefusal::ReadingNaNStale, DoseRefusal::ReadingNaNGateClosed};
  for (int i = 0; i < 4; ++i) {
    DoseInputs in = validInputs();
    in.reading = NAN;
    in.filterState = states[i];
    DoseDecision d = evaluateDose(in);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(wants[i], d.cause);
  }
  // Unknown / Ready (état incohérent avec le refus) → cause GÉNÉRIQUE : on
  // n'invente jamais une cause bénigne à partir d'un état non collecté.
  DoseInputs in = validInputs();
  in.reading = NAN;
  in.filterState = DoseFilterState::Unknown;
  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaN, evaluateDose(in).cause);
  in.filterState = DoseFilterState::Ready;
  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaN, evaluateDose(in).cause);
}

// --- RETIRÉS (feature-152) --------------------------------------------------
// test_V2251_qualification_filter_not_ready_par_etat et
// test_V2251_panne_ezo_nest_plus_annoncee_comme_stabilisation testaient la
// garde séparée « filtre non prêt » (3b), qui n'existe plus : la valeur
// officielle de sensor_filter est NaN dans tous les cas qu'elle couvrait, la
// seule garde restante est isnan(reading) (garde 3), déjà qualifiée par
// test_V2251_qualification_reading_nan_par_etat ci-dessus.
//
// --- Scénario de production : panne EZO annoncée comme panne, pas comme
// warmup — réécrit sur la garde 3 UNIQUE qui subsiste (reading NaN qualifié
// par filterState, composé via resolveFilterState()).
void test_V2251_panne_ezo_annoncee_comme_panne_pas_comme_stabilisation(void) {
  // Sonde figée (répond, valeur constante) : la fenêtre est pleine et fraîche,
  // mais frozen() est vrai → valeur officielle NaN. Avant v2.25.1 : « warmup /
  // EZO injoignable » aurait masqué la panne.
  DoseInputs in = validInputs();
  in.reading = NAN;
  in.filterState = resolveFilterState(/*frozen*/ true, /*gate*/ false,
                                      /*stale*/ false, /*warmingUp*/ false);
  DoseDecision d = evaluateDose(in);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL_MESSAGE(DoseRefusal::ReadingNaNFrozen, d.cause,
      "une vraie panne de sonde ne doit PAS partager son code avec le warmup");
  TEST_ASSERT_NOT_EQUAL(DoseRefusal::ReadingNaNWarmup, d.cause);

  // Symétrique : warmup légitime au boot → code de warmup, jamais un code de panne.
  in.filterState = resolveFilterState(false, false, false, true);
  d = evaluateDose(in);
  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaNWarmup, d.cause);
  TEST_ASSERT_NOT_EQUAL(DoseRefusal::ReadingNaNFrozen, d.cause);
  TEST_ASSERT_NOT_EQUAL(DoseRefusal::ReadingNaNStale, d.cause);
}

// --- Initialiseur fail-closed d'observabilité -------------------------------
void test_V2251_filter_state_defaut_est_unknown(void) {
  DoseInputs in;  // agrégat NON value-initialisé : seul filterState a un défaut
  TEST_ASSERT_EQUAL_MESSAGE(DoseFilterState::Unknown, in.filterState,
      "sans initialiseur de membre, un site de collecte lirait une valeur "
      "indeterminee et pourrait afficher une cause fausse");
  // validInputs() ne renseigne PAS filterState → les tests historiques (T6/T18)
  // exercent bien les causes génériques : c'est ce qui les rend inchangés.
  DoseInputs v = validInputs();
  TEST_ASSERT_EQUAL(DoseFilterState::Unknown, v.filterState);
}

// --- (4) Contrat de sérialisation : valeurs numériques FIGÉES ----------------
// Les codes partent sur /data et le WebSocket, et sont traduits par une table
// côté client. Une renumérotation décalerait la table et réafficherait une
// mauvaise cause — exactement la classe de défaut corrigée en v2.25.1.
// Baseline vérifiée sur `git show 15a10de:src/dosing_logic.h` (0→14 implicites).
// feature-152 : 4 (FilterNotReady) et 5 (FilterUnstable) sont RETIRÉS — ils
// n'existent plus comme énumérateurs (voir dosing_logic.h) : toute tentative
// de les référencer ici redeviendrait une erreur de COMPILATION, la preuve la
// plus stricte qu'ils ne sont pas recyclés. Le test vérifie donc l'ÉCART
// (ReadingNaN=3 puis CalibrationInsufficient=6, sans rien entre les deux).
void test_V2251_contrat_serialisation_codes_0_a_14_inchanges(void) {
  TEST_ASSERT_EQUAL_INT(0, (int)DoseRefusal::None);
  TEST_ASSERT_EQUAL_INT(1, (int)DoseRefusal::WatchdogInactive);
  TEST_ASSERT_EQUAL_INT(2, (int)DoseRefusal::FiltrationOff);
  TEST_ASSERT_EQUAL_INT(3, (int)DoseRefusal::ReadingNaN);
  TEST_ASSERT_EQUAL_INT_MESSAGE(6, (int)DoseRefusal::CalibrationInsufficient,
      "4 et 5 (FilterNotReady/FilterUnstable) retires : le prochain code reel est 6");
  TEST_ASSERT_EQUAL_INT(7, (int)DoseRefusal::StabilizationActive);
  TEST_ASSERT_EQUAL_INT(8, (int)DoseRefusal::MixingActive);
  TEST_ASSERT_EQUAL_INT(9, (int)DoseRefusal::ModeNotAutomatic);
  TEST_ASSERT_EQUAL_INT(10, (int)DoseRefusal::DailyLimit);
  TEST_ASSERT_EQUAL_INT(11, (int)DoseRefusal::HourlyLimit);
  TEST_ASSERT_EQUAL_INT(12, (int)DoseRefusal::CyclesPerDay);
  TEST_ASSERT_EQUAL_INT(13, (int)DoseRefusal::BurstPerMinute);
  TEST_ASSERT_EQUAL_INT(14, (int)DoseRefusal::BurstPer15Min);
}

// feature-152 : 19-22 (FilterWarmup/FilterFrozen/FilterStale/FilterGateClosed)
// sont RETIRÉS eux aussi — seuls 15-18 (famille ReadingNaN*) subsistent, et
// sont désormais le DERNIER bloc de l'énum (prochaine valeur disponible : 23).
void test_V2251_contrat_serialisation_codes_15_a_18_inchanges(void) {
  TEST_ASSERT_EQUAL_INT(15, (int)DoseRefusal::ReadingNaNWarmup);
  TEST_ASSERT_EQUAL_INT(16, (int)DoseRefusal::ReadingNaNFrozen);
  TEST_ASSERT_EQUAL_INT(17, (int)DoseRefusal::ReadingNaNStale);
  TEST_ASSERT_EQUAL_INT(18, (int)DoseRefusal::ReadingNaNGateClosed);
  // Ces codes sont AJOUTÉS EN FIN : tous > BurstPer15Min (14), et le bloc est
  // contigu (aucun trou, aucune insertion au milieu).
  TEST_ASSERT_TRUE((int)DoseRefusal::ReadingNaNWarmup >
                   (int)DoseRefusal::BurstPer15Min);
  // DoseFilterState est un enum d'ENTRÉE : ses valeurs ne sont pas transportées,
  // mais elles indexent les tables locales ci-dessus — on les verrouille aussi.
  TEST_ASSERT_EQUAL_INT(0, (int)DoseFilterState::Unknown);
  TEST_ASSERT_EQUAL_INT(1, (int)DoseFilterState::Ready);
  TEST_ASSERT_EQUAL_INT(2, (int)DoseFilterState::Warmup);
  TEST_ASSERT_EQUAL_INT(3, (int)DoseFilterState::Frozen);
  TEST_ASSERT_EQUAL_INT(4, (int)DoseFilterState::Stale);
  TEST_ASSERT_EQUAL_INT(5, (int)DoseFilterState::GateClosed);
}

// =============================================================================
// ÉPISODE de panne de mesure (revue v2.25.1) — couverture PARTIELLE, délimitée
// =============================================================================
// PÉRIMÈTRE DE CE TEST — à lire avant de le croire complet.
// La machine d'état de l'épisode (`_sensorFaultActive` / `_sensorFaultSinceMs` /
// `_sensorFaultAlerted`, fenêtre kDoseSensorFaultAlertDelayMs) vit dans
// PumpControllerClass::logRefusalOnce() : elle dépend de millis(), systemLogger et
// mqttManager, donc elle N'EST PAS testable en env:native, et la dupliquer ici
// produirait un test en trompe-l'œil (on testerait la copie, pas le code livré).
// Ce test couvre ce qui EST pur : les ENTRÉES de cette machine, c'est-à-dire
//   (a) la PRÉMISSE du défaut corrigé — une panne de mesure CONTINUE peut faire
//       varier le code de refus (20 ↔ 21), ce qui redatait la fenêtre ;
//   (b) la FRONTIÈRE de classification — quels codes sont des pannes et quels
//       codes n'en sont pas (une erreur ici enverrait l'alerte sur un warmup, ou
//       la retiendrait sur une panne) ;
//   (c) la valeur de la temporisation.
// La chronologie (redatation, alerte unique, clôture d'épisode) reste couverte
// UNIQUEMENT par les scénarios d'intégration manuels du rapport de test.
void test_V2251_episode_premisse_codes_de_panne_alternent_a_panne_continue(void) {
  // Un seul et même épisode : la mesure est en panne de bout en bout, seule la
  // NATURE de la panne change (EZO agonisant : figé, puis plus aucune mesure
  // acceptée, puis figé de nouveau). Chaque phase produit un code DIFFÉRENT.
  // feature-152 : la garde séparée « filtre non prêt » (filterReady) a
  // disparu — il ne reste qu'une seule garde (reading NaN), qualifiée par
  // filterState. L'épisode s'exerce donc entièrement sur cette famille.
  DoseInputs in = validInputs();
  in.reading = NAN;

  in.filterState = resolveFilterState(/*frozen*/ true, /*gate*/ false,
                                      /*stale*/ false, /*warmingUp*/ false);
  const DoseRefusal phase1 = evaluateDose(in).cause;

  in.filterState = resolveFilterState(/*frozen*/ false, /*gate*/ false,
                                      /*stale*/ true, /*warmingUp*/ false);
  const DoseRefusal phase2 = evaluateDose(in).cause;

  in.filterState = resolveFilterState(true, false, false, false);
  const DoseRefusal phase3 = evaluateDose(in).cause;

  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaNFrozen, phase1);
  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaNStale, phase2);
  TEST_ASSERT_EQUAL(DoseRefusal::ReadingNaNFrozen, phase3);
  TEST_ASSERT_TRUE_MESSAGE(phase1 != phase2,
      "PREMISSE du defaut corrige : une panne CONTINUE change de code => une "
      "fenetre redatee sur tout changement de code n'expirait jamais");
  TEST_ASSERT_EQUAL_MESSAGE(phase1, phase3,
      "le retour a la nature initiale ne doit pas etre pris pour un nouvel episode");
}

// (b) Frontière de classification + (c) temporisation.
// Les deux codes de PANNE sont exactement {16, 17} (feature-152 a retiré la
// seconde famille, {20, 21} : FilterFrozen/FilterStale n'existent plus) ; les
// qualifications bénignes (warmup, porte fermée) et les causes génériques
// n'en font pas partie. Ce verrou est la contrepartie testable de la liste
// `sensorFault` de pump_controller.cpp : si un code y était ajouté ou retiré,
// la liste des codes « PANNE » documentée dans dosing_logic.h et ici divergerait.
void test_V2251_episode_frontiere_de_classification_et_temporisation(void) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(60000UL, kDoseSensorFaultAlertDelayMs,
      "temporisation d'alerte de panne de mesure : 60 s (ecarte le transitoire de "
      "reamorcage du filtre a la reprise de circulation)");

  // PANNES (la regulation est a l'arret et la sonde est en cause).
  const DoseRefusal faults[2] = {
      DoseRefusal::ReadingNaNFrozen, DoseRefusal::ReadingNaNStale};
  const int wants[2] = {16, 17};
  for (int i = 0; i < 2; ++i) TEST_ASSERT_EQUAL_INT(wants[i], (int)faults[i]);

  // NON-PANNES : etats normaux ou blocage voulu par le systeme. Alerter ici
  // banaliserait l'alerte (demarrage de filtration, purge, boot).
  const DoseRefusal benign[3] = {
      DoseRefusal::ReadingNaNWarmup, DoseRefusal::ReadingNaNGateClosed,
      DoseRefusal::ReadingNaN};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 2; ++j) {
      TEST_ASSERT_TRUE_MESSAGE(benign[i] != faults[j],
          "un etat benin ne doit JAMAIS partager le code d'une panne");
    }
  }
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
  // T7/T8 retirés (feature-152) — voir commentaire au site de définition.
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
  // feature-163 (gate G2/G6) — garde CalibrationSessionOpen (evaluateDose).
  RUN_TEST(test_F163_refusal_calibration_session_open);
  RUN_TEST(test_F163_rang3_devant_reading_nan);
  RUN_TEST(test_F163_rang3_devant_calibration_et_mode);
  RUN_TEST(test_F163_persistance_boot_session_ouverte_mode_deja_manual);
  RUN_TEST(test_F163_non_masquage_watchdog_devant_session);
  RUN_TEST(test_F163_non_masquage_eau_devant_session);
  RUN_TEST(test_F163_session_fermee_aucun_refus_nouveau);
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
  // v2.26.0 — variante PI (configuration pH réelle du firmware)
  RUN_TEST(test_AC8_pi_l_integrale_influence_le_flow);
  RUN_TEST(test_AC9_pi_anti_windup_bornes_de_production);
  RUN_TEST(test_AC10_pi_flow_jamais_au_dessus_du_debit_d_amplitude);
  RUN_TEST(test_AC11_pi_deadband_gele_l_integrale);
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
  // feature-006 — evaluateManualInject (injection manuelle gardée).
  RUN_TEST(test_F006_nominal_allowed);
  RUN_TEST(test_F006_refusal_watchdog_inactive);
  RUN_TEST(test_F006_refusal_filtration_off);
  RUN_TEST(test_F006_refusal_stabilization_active);
  RUN_TEST(test_F006_refusal_already_injecting);
  RUN_TEST(test_F006_refusal_daily_limit);
  RUN_TEST(test_F006_refusal_hourly_limit);
  RUN_TEST(test_F006_refusal_max_cycles_per_day);
  RUN_TEST(test_F006_refusal_burst_per_minute);
  RUN_TEST(test_F006_refusal_burst_per_15min);
  RUN_TEST(test_F006_remaining_ml_on_daily_refusal);
  RUN_TEST(test_F006_boundary_daily_exact_accepted);
  RUN_TEST(test_F006_boundary_hourly_exact_accepted);
  RUN_TEST(test_F006_boundary_zero_limits_unlimited);
  RUN_TEST(test_F006_guard_priority_order);
  // bug-161 (gate C1) — garde AutomaticInjectInProgress (rang 4).
  RUN_TEST(test_F161_refusal_automatic_inject_in_progress);
  RUN_TEST(test_F161_automatic_inject_in_progress_devant_quotas);
  RUN_TEST(test_F161_watchdog_devant_automatic_inject_in_progress);
  RUN_TEST(test_F161_filtration_off_devant_automatic_inject_in_progress);
  RUN_TEST(test_F161_automatic_inject_in_progress_devant_already_injecting);
  // feature-163 (gate G2/G6) — garde CalibrationSessionOpen (evaluateManualInject).
  RUN_TEST(test_F163_manual_refusal_calibration_session_open);
  RUN_TEST(test_F163_manual_rang3_devant_stabilization);
  RUN_TEST(test_F163_manual_rang3_devant_automatic_et_already_injecting);
  RUN_TEST(test_F163_manual_non_masquage_watchdog_devant_session);
  RUN_TEST(test_F163_manual_non_masquage_eau_devant_session);
  RUN_TEST(test_F163_manual_session_fermee_aucun_refus_nouveau);
  RUN_TEST(test_F006_manual_start_visible_in_shared_ring);
  RUN_TEST(test_F006_stabilization_independent_of_remaining_seconds);
  // feature-011 — evaluateScheduledDose (répartition 24 h programmée).
  RUN_TEST(test_F011_nominal_repartition_16_fenetres);
  RUN_TEST(test_F011_report_sous_30s_puis_fenetre_suivante);
  RUN_TEST(test_F011_budget_horaire_ecretage);
  RUN_TEST(test_F011_budget_horaire_frontiere_exacte);
  RUN_TEST(test_F011_changement_cible_midi);
  RUN_TEST(test_F011_injection_manuelle_reduit_fenetre_suivante);
  RUN_TEST(test_F011_baisse_cible_sous_injecte_stop_immediat);
  RUN_TEST(test_F011_plafond_max_daily);
  RUN_TEST(test_F011_gardes_fail_closed);
  RUN_TEST(test_F011_horizon_une_minute_fin_de_journee);
  RUN_TEST(test_F011_meme_fenetre_conserve_stop_target);
  RUN_TEST(test_F011_planned_flow_diagnostic);
  // feature-163 (gate G3/G6) — garde CalibrationSessionOpen (evaluateScheduledDose).
  RUN_TEST(test_F163_scheduled_refusal_calibration_session_open);
  RUN_TEST(test_F163_scheduled_session_ouverte_devant_plan_en_cours);
  RUN_TEST(test_F163_scheduled_watchdog_toujours_bloquant_session_fermee);
  RUN_TEST(test_F163_scheduled_session_fermee_aucun_refus_nouveau);
  // feature-053 — Mode Boost : cible ORP effective.
  RUN_TEST(test_F053_orp_boost_off_target_unchanged);
  RUN_TEST(test_F053_orp_boost_on_automatic_adds_delta);
  RUN_TEST(test_F053_orp_boost_ceiling_caps);
  RUN_TEST(test_F053_orp_target_above_ceiling_never_lowered);
  RUN_TEST(test_F053_orp_boost_on_non_automatic_unchanged);
  RUN_TEST(test_F053_orp_boundary_exact_ceiling);
  // feature-053 — Mode Boost : limite chlore effective.
  RUN_TEST(test_F053_chlorine_boost_off_unchanged);
  RUN_TEST(test_F053_chlorine_boost_on_automatic_x_factor);
  RUN_TEST(test_F053_chlorine_hard_cap);
  RUN_TEST(test_F053_chlorine_boost_on_non_automatic_unchanged);
  RUN_TEST(test_F053_chlorine_boundary_near_hard_cap);
  // feature-060 — limite cycles/jour configurable (garde 9 + clampCyclesPerDay).
  RUN_TEST(test_F060_auto_guard9_47_allowed_48_refused);
  RUN_TEST(test_F060_manual_guard_47_allowed_48_refused);
  RUN_TEST(test_F060_clamp_out_of_bounds);
  RUN_TEST(test_F060_clamp_exact_bounds_untouched);
  RUN_TEST(test_F060_shared_limit_same_threshold_auto_and_manual);
  RUN_TEST(test_F060_hourly_and_mixing_refusals_precede_guard9);
  // bug-dosage-fantome (défaut n°2) — comptabilisation d'un démarrage de cycle.
  RUN_TEST(test_BUG2_accounting_pure_truth_table);
  RUN_TEST(test_BUG2_first_tick_zero_flow_counts_one_cycle);
  RUN_TEST(test_BUG2_two_spaced_cycles_count_two);
  RUN_TEST(test_BUG2_aborted_start_consumes_no_cycle);
  RUN_TEST(test_BUG2_pi_integrale_survit_entre_deux_cycles);
  // bug-dosage-fantome itération 2 (défaut n°1) — proportionnel de temps (B+C).
  RUN_TEST(test_PULSE_gate_cal_duty_zero_refuses);
  RUN_TEST(test_PULSE_gate_amplitude_differs_from_calibrated_refuses);
  RUN_TEST(test_PULSE_gate_B2_amplitude_below_min_active);
  RUN_TEST(test_PULSE_gate_implausible_flow_refuses);
  RUN_TEST(test_PULSE_plan_seuil_C_exact);
  RUN_TEST(test_PULSE_plan_fraction_100_pouls_egale_periode);
  RUN_TEST(test_PULSE_plan_scenario_terrain_6_4_sur_24_7);
  RUN_TEST(test_PULSE_delta_volume_a_cheval_sur_le_kick);
  RUN_TEST(test_PULSE_tick_wrap_millis);
  RUN_TEST(test_PULSE_periode_complete_on_time_et_volume);
  // Audit test-engineer (itération 2) — cas de bord complémentaires.
  RUN_TEST(test_PULSE_plan_seuil_C_frontiere_fine);
  RUN_TEST(test_PULSE_plan_jamais_de_pouls_hors_bornes);
  RUN_TEST(test_PULSE_gate_flow_infini_refuse);
  RUN_TEST(test_PULSE_delta_volume_segments_purs);
  RUN_TEST(test_PULSE_tick_pouls_egale_periode_pompe_continue);
  RUN_TEST(test_PULSE_tick_pouls_plus_court_que_kick);

  // feature-073 — accumulateur de dosage sous le pouls minimal.
  RUN_TEST(test_F073_cumul_exact_par_periode);
  RUN_TEST(test_F073_tir_a_egalite_exacte);
  RUN_TEST(test_F073_residu_conserve_et_invariant_sortie);
  RUN_TEST(test_F073_scenario_long_0_7_ml_min_volume_et_cadence);
  RUN_TEST(test_F073_F2_retour_plancher_delegation_bit_identique);
  RUN_TEST(test_F073_entrees_invalides_fail_closed);
  RUN_TEST(test_F073_carry_entree_suspect_reset_defensif);
  RUN_TEST(test_F073_pulse_jamais_hors_domaine);
  RUN_TEST(test_F073_regime_normal_balayage_identique_planPulse);
  RUN_TEST(test_F105_volume_reference_incalculable_refus_complet);
  RUN_TEST(test_F105_contre_test_garde_non_declenchee);
  // v2.25.3 — pouls minimal 5 s → 10 s : moyenne restituée indépendante.
  RUN_TEST(test_V253_moyenne_restituee_independante_du_pouls_minimal);
  RUN_TEST(test_V253_moyenne_juste_sous_le_nouveau_seuil_de_bascule);
  RUN_TEST(test_V253_invariant_sortie_et_reset_defensif_recales_sur_10s);
  RUN_TEST(test_V253_pouls_jamais_hors_domaine_a_10s);
  RUN_TEST(test_V253_seuil_de_bascule_derive_du_pouls_minimal);
  RUN_TEST(test_V253_ecretage_fail_closed_inatteignable_a_10s);
  // V2251 — qualification des causes de refus par l'état interne du filtre.
  RUN_TEST(test_V2251_precondition_frontieres_flottantes_exactes);
  RUN_TEST(test_V2251_verdict_invariant_balayage_exhaustif);
  RUN_TEST(test_V2251_resolve_filter_state_etats_simples);
  RUN_TEST(test_V2251_resolve_filter_state_priorite_pannes_dabord);
  RUN_TEST(test_V2251_resolve_filter_state_balayage_16_combinaisons);
  RUN_TEST(test_V2251_qualification_reading_nan_par_etat);
  // qualification_filter_not_ready_par_etat retiré (feature-152, mécanisme disparu).
  RUN_TEST(test_V2251_panne_ezo_annoncee_comme_panne_pas_comme_stabilisation);
  RUN_TEST(test_V2251_filter_state_defaut_est_unknown);
  RUN_TEST(test_V2251_contrat_serialisation_codes_0_a_14_inchanges);
  RUN_TEST(test_V2251_contrat_serialisation_codes_15_a_18_inchanges);
  // Épisode de panne de mesure : entrées de la machine d'état (couverture
  // partielle assumée — la chronologie vit dans PumpControllerClass).
  RUN_TEST(test_V2251_episode_premisse_codes_de_panne_alternent_a_panne_continue);
  RUN_TEST(test_V2251_episode_frontiere_de_classification_et_temporisation);
  return UNITY_END();
}
