// =============================================================================
// Tests unitaires natifs — Auto-guérison mémoire (self_heal_logic, v2.9.1)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la décision PURE extraite dans
// src/self_heal_logic.{h,cpp} (ADR-0017, comme dosing_logic / schedule_logic
// / sensor_filter) :
//   - evaluateSelfHeal(SelfHealInputs) → { Continue, Restart, EnterMaintenance }
//
// Ordre des règles (priorité décroissante), verrouillé par ces tests :
//   1. rebootCountInWindow >= maxReboots → EnterMaintenance (anti-boucle,
//      PRIORITÉ HAUTE : l'emporte même si les conditions de Restart sont réunies).
//   2. uptimeMs >= minUptimeMs ET maxAllocHeap < restartThreshold ET
//      consecutiveBelow >= debounceChecks → Restart.
//   3. sinon → Continue.
//
// FRONTIÈRES figées par le code (>= vs <) :
//   - heap OK ssi maxAllocHeap >= restartThreshold (frontière == → Continue).
//   - debounce atteint ssi consecutiveBelow >= debounceChecks (== → atteint).
//   - uptime atteint ssi uptimeMs >= minUptimeMs (== → atteint).
//   - maintenance ssi rebootCountInWindow >= maxReboots (== → maintenance).
//
// Section 8 (bug-080, v2.33.0 — gate pool-chemistry 02/09/2026) : les sections
// 1-7 gardent leurs valeurs INDICATIVES (elles testent la logique, pas le
// calibrage) ; la section 8 verrouille l'ÉCHELLE RÉELLE 8 bits de constants.h
// (2048 < 4096 < 6144 < 12288 < 18432), ses frontières exactes, le débounce en
// séquence (contrat de la coquille main.cpp), le profil de famine du 02/09 et
// l'hystérésis de réarmement du parachute (oom_guard_logic.h).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "self_heal_logic.h"
// Section 8 (bug-080, v2.33.0) : les tests d'échelle chargent les VRAIES
// constantes de production et l'hystérésis de réarmement du parachute.
#include "constants.h"        // kRestartMaxAllocHeapBytes, kLowHeapWarnBytes, ...
#include "oom_guard_logic.h"  // oomShouldRearm / oomRearmTargetBytes

void setUp(void) {}
void tearDown(void) {}

// Valeurs de référence (répliquées ici, module pur sans include Arduino).
// Toute divergence firmware sera détectée par la revue.
static const uint32_t kThreshold    = 24000UL;  // kRestartMaxAllocHeapBytes (indicatif)
static const uint8_t  kDebounce     = 3;         // kAutoHealDebounceChecks (indicatif)
static const uint32_t kMinUptimeMs  = 120000UL;  // kAutoHealMinUptimeMs (indicatif)
static const uint8_t  kMaxReboots   = 3;         // kAutoHealMaxReboots (indicatif)

// Helper : entrées « saines » — heap OK, aucun reboot, uptime largement écoulé.
// Chaque test part de cette base et ne perturbe que le champ étudié.
static SelfHealInputs healthyInputs(void) {
  SelfHealInputs in;
  in.maxAllocHeap       = kThreshold * 2;   // très au-dessus du seuil
  in.restartThreshold   = kThreshold;
  in.consecutiveBelow   = 0;
  in.debounceChecks     = kDebounce;
  in.uptimeMs           = kMinUptimeMs * 10; // système démarré depuis longtemps
  in.minUptimeMs        = kMinUptimeMs;
  in.rebootCountInWindow = 0;
  in.maxReboots         = kMaxReboots;
  return in;
}

// Helper : entrées où TOUTES les conditions de Restart sont réunies (heap sous
// le seuil, debounce atteint, uptime écoulé) mais reboots sous le plafond.
static SelfHealInputs restartReadyInputs(void) {
  SelfHealInputs in = healthyInputs();
  in.maxAllocHeap     = kThreshold - 1;  // sous le seuil
  in.consecutiveBelow = kDebounce;       // debounce atteint
  // uptimeMs déjà >> minUptimeMs, rebootCountInWindow = 0 < maxReboots.
  return in;
}

// =============================================================================
// 1. Continue quand le heap est OK (maxAllocHeap >= seuil)
// =============================================================================
void test_continue_when_heap_ok(void) {
  SelfHealInputs in = healthyInputs();
  // Même avec debounce et uptime « favorables » au restart, le heap OK protège.
  in.consecutiveBelow = kDebounce + 5;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// =============================================================================
// 2. Continue quand sous le seuil mais debounce PAS atteint
// =============================================================================
void test_continue_when_below_but_debounce_not_reached(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = kDebounce - 1;  // il manque un tick
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Cas extrême : sous le seuil, uptime OK, mais aucun tick consécutif encore.
void test_continue_when_below_debounce_zero(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = 0;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// =============================================================================
// 3. Continue quand sous le seuil + debounce atteint mais uptime < minUptime
//    (pas de reboot pendant le démarrage / allocations légitimes)
// =============================================================================
void test_continue_when_uptime_below_min(void) {
  SelfHealInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs - 1;  // système encore en phase de démarrage
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// =============================================================================
// 4. Restart quand uptime >= min ET heap < seuil ET debounce atteint
// =============================================================================
void test_restart_when_all_conditions_met(void) {
  SelfHealInputs in = restartReadyInputs();
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Debounce largement dépassé → toujours Restart.
void test_restart_when_debounce_exceeded(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = kDebounce + 10;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// =============================================================================
// 5. EnterMaintenance PRIORITAIRE : le plafond de reboots l'emporte même si
//    toutes les conditions de Restart sont par ailleurs réunies (anti-boucle).
// =============================================================================
void test_maintenance_priority_over_restart(void) {
  SelfHealInputs in = restartReadyInputs();  // Restart aurait été renvoyé…
  in.rebootCountInWindow = kMaxReboots;      // …mais plafond atteint → maintenance
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// Maintenance même sur un système parfaitement sain (heap OK, uptime OK) dès
// que le plafond est atteint : la règle 1 court-circuite tout.
void test_maintenance_even_when_healthy(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// Reboots au-delà du plafond → maintenance (>= et non ==).
void test_maintenance_when_reboots_exceed_max(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots + 5;
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// =============================================================================
// 6. Frontières exactes aux seuils
// =============================================================================

// Heap : maxAllocHeap == restartThreshold → PAS sous le seuil (< strict) → Continue.
void test_boundary_heap_equals_threshold_continue(void) {
  SelfHealInputs in = restartReadyInputs();
  in.maxAllocHeap = kThreshold;  // exactement au seuil
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Heap : maxAllocHeap == restartThreshold - 1 → sous le seuil → Restart.
void test_boundary_heap_one_below_threshold_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.maxAllocHeap = kThreshold - 1;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Debounce : consecutiveBelow == debounceChecks → atteint (>=) → Restart.
void test_boundary_debounce_equals_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.consecutiveBelow = kDebounce;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Uptime : uptimeMs == minUptimeMs → atteint (>=) → Restart.
void test_boundary_uptime_equals_min_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs;
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// Uptime : uptimeMs == minUptimeMs - 1 → non atteint → Continue.
void test_boundary_uptime_one_below_min_continue(void) {
  SelfHealInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs - 1;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Maintenance : rebootCountInWindow == maxReboots → atteint (>=) → maintenance.
void test_boundary_reboots_equals_max_maintenance(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(SelfHealDecision::EnterMaintenance, evaluateSelfHeal(in));
}

// Maintenance : rebootCountInWindow == maxReboots - 1 → sous le plafond → pas
// de maintenance (ici heap OK → Continue).
void test_boundary_reboots_one_below_max_no_maintenance(void) {
  SelfHealInputs in = healthyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, evaluateSelfHeal(in));
}

// Sous le plafond de reboots ET conditions de Restart réunies → Restart
// (confirme que la règle 1 ne s'active pas juste en dessous du plafond).
void test_reboots_one_below_max_still_restart(void) {
  SelfHealInputs in = restartReadyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart, evaluateSelfHeal(in));
}

// =============================================================================
// 7. heapSampleIntervalMs — cadence d'échantillonnage adaptative
//    (bug-badalloc-ws-broadcast). Frontière figée par le code :
//    maxAllocHeap < warnThreshold → fast ; >= (== inclus) → normal.
// =============================================================================

// Valeurs de référence (indicatives, comme kThreshold ci-dessus).
static const uint32_t kWarnThreshold  = 20000UL;  // kLowHeapWarnBytes (indicatif)
static const uint32_t kNormalInterval = 60000UL;  // kHealthCheckIntervalMs (indicatif)
static const uint32_t kFastInterval   = 5000UL;   // kHeapCheckFastIntervalMs (indicatif)

// Au-dessus du seuil d'alerte → cadence normale.
void test_heap_interval_above_threshold_normal(void) {
  TEST_ASSERT_EQUAL_UINT32(kNormalInterval,
      heapSampleIntervalMs(kWarnThreshold + 1, kWarnThreshold, kNormalInterval, kFastInterval));
}

// En dessous du seuil d'alerte → cadence rapprochée.
void test_heap_interval_below_threshold_fast(void) {
  TEST_ASSERT_EQUAL_UINT32(kFastInterval,
      heapSampleIntervalMs(kWarnThreshold - 1, kWarnThreshold, kNormalInterval, kFastInterval));
}

// Frontière : maxAllocHeap == warnThreshold → PAS sous le seuil (< strict) → normal.
void test_heap_interval_boundary_equals_threshold_normal(void) {
  TEST_ASSERT_EQUAL_UINT32(kNormalInterval,
      heapSampleIntervalMs(kWarnThreshold, kWarnThreshold, kNormalInterval, kFastInterval));
}

// Extrême bas : maxAllocHeap == 0 (heap effondré) → fast.
void test_heap_interval_zero_fast(void) {
  TEST_ASSERT_EQUAL_UINT32(kFastInterval,
      heapSampleIntervalMs(0, kWarnThreshold, kNormalInterval, kFastInterval));
}

// Extrême haut / sentinelle « pas encore d'échantillon » (UINT32_MAX) → normal
// (verrouille le premier intervalle du timer dans loop()).
void test_heap_interval_uint32_max_normal(void) {
  TEST_ASSERT_EQUAL_UINT32(kNormalInterval,
      heapSampleIntervalMs(UINT32_MAX, kWarnThreshold, kNormalInterval, kFastInterval));
}

// =============================================================================
// 8. Échelle heap 8 bits RÉELLE — bug-080 (v2.33.0), gate pool-chemistry
//    du 02/09/2026. À partir d'ici, plus de valeurs « indicatives » : on charge
//    les VRAIES constantes de production (constants.h) pour verrouiller
//    l'échelle validée :
//      2 048 (réarmement repli parachute, kOomFallbackRearmMinMaxAllocBytes)
//    < 4 096 (auto-heal, kRestartMaxAllocHeapBytes)
//    < 6 144 (skip push WS, kWsPushMinAllocBytes)
//    < 12 288 (WARN + cadence rapide, kLowHeapWarnBytes)
//    < 18 432 (réarmement nominal parachute, kOomRearmMinMaxAllocBytes)
//    < 21 492 (régime nominal terrain mesuré le 02/09).
// =============================================================================

// Régime nominal terrain (maxBloc8 mesuré le 02/09/2026 sur l'appareil sain).
static const uint32_t kFieldNominalMaxBloc8 = 21492u;

// Entrées avec les constantes RÉELLES (le pendant de healthyInputs pour la
// section 8) : seul le couple (maxAllocHeap, consecutiveBelow) varie.
static SelfHealInputs realInputs(uint32_t maxAlloc, uint8_t consecutiveBelow) {
  SelfHealInputs in;
  in.maxAllocHeap        = maxAlloc;
  in.restartThreshold    = kRestartMaxAllocHeapBytes;
  in.consecutiveBelow    = consecutiveBelow;
  in.debounceChecks      = kAutoHealDebounceChecks;
  in.uptimeMs            = kAutoHealMinUptimeMs * 10;  // hors phase de démarrage
  in.minUptimeMs         = kAutoHealMinUptimeMs;
  in.rebootCountInWindow = 0;
  in.maxReboots          = kAutoHealMaxReboots;
  return in;
}

// Un tick de checkHeapHealth() : réplique le CONTRAT documenté de la coquille
// impure (main.cpp) — le compteur consecutiveBelow s'incrémente si
// maxAlloc < kRestartMaxAllocHeapBytes (saturation à 255), sinon il est REMIS À
// ZÉRO — puis délègue la décision au module pur. Permet de tester des SÉQUENCES
// de mesures (débounce, creux bref vs famine soutenue).
static SelfHealDecision tickHeapReal(uint8_t& consecutiveBelow, uint32_t maxAlloc) {
  if (maxAlloc < kRestartMaxAllocHeapBytes) {
    if (consecutiveBelow < 255) consecutiveBelow++;
  } else {
    consecutiveBelow = 0;
  }
  return evaluateSelfHeal(realInputs(maxAlloc, consecutiveBelow));
}

// Helper réarmement parachute avec les valeurs RÉELLES (aucun littéral).
static uint32_t rearmTargetReal(uint32_t armedBytes, uint32_t maxAllocBytes) {
  return oomRearmTargetBytes(armedBytes, maxAllocBytes,
                             (uint32_t)kOomReserveBytes, kOomRearmMinMaxAllocBytes,
                             (uint32_t)kOomFallbackReserveBytes,
                             kOomFallbackRearmMinMaxAllocBytes);
}

// --- 8a. Sentinelles de calibrage + ordre strict de l'échelle ---
void test_scale_real_constants_bug080(void) {
  // Sentinelles (gate pool-chemistry 02/09/2026, bug-080) : tout recalibrage
  // futur doit casser ICI et être re-validé en connaissance de cause.
  TEST_ASSERT_EQUAL_UINT32(2048u, kOomFallbackRearmMinMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(4096u, kRestartMaxAllocHeapBytes);
  TEST_ASSERT_EQUAL_UINT32(6144u, kWsPushMinAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(12288u, kLowHeapWarnBytes);
  TEST_ASSERT_EQUAL_UINT32(18432u, kOomRearmMinMaxAllocBytes);
  // Ordre STRICT : chaque garde s'active avant la suivante (le WARN précède le
  // skip WS, qui précède l'auto-heal, qui laisse le repli parachute en dessous).
  TEST_ASSERT_TRUE(kOomFallbackRearmMinMaxAllocBytes < kRestartMaxAllocHeapBytes);
  TEST_ASSERT_TRUE(kRestartMaxAllocHeapBytes < kWsPushMinAllocBytes);
  TEST_ASSERT_TRUE(kWsPushMinAllocBytes < kLowHeapWarnBytes);
  TEST_ASSERT_TRUE(kLowHeapWarnBytes < kOomRearmMinMaxAllocBytes);
  // Et le régime nominal terrain est AU-DESSUS de toute l'échelle : aucune
  // garde active en fonctionnement sain.
  TEST_ASSERT_TRUE(kFieldNominalMaxBloc8 > kOomRearmMinMaxAllocBytes);
}

// --- 8b. Arithmétique du réarmement (refus du gate MATÉRIALISÉ) ---
void test_rearm_arithmetic_never_triggers_autoheal_bug080(void) {
  // Le gate du 02/09/2026 a REFUSÉ 16 384 : après allocation de la réserve
  // nominale (12 288), le reliquat aurait été 4 096 — pile le seuil d'auto-heal,
  // sans marge : le parachute aurait semé son propre restart d'auto-guérison.
  // Propriété exigée : reliquat >= palier auto-heal + seuil de repli, exprimée
  // sur les CONSTANTES (aucun littéral recopié). Une mutation 18432 → 16384 de
  // kOomRearmMinMaxAllocBytes meurt ici (4096 < 4096 + 2048).
  TEST_ASSERT_TRUE(kOomRearmMinMaxAllocBytes > (uint32_t)kOomReserveBytes);  // pas d'underflow ci-dessous
  TEST_ASSERT_TRUE(kOomRearmMinMaxAllocBytes - (uint32_t)kOomReserveBytes >=
                   kRestartMaxAllocHeapBytes + kOomFallbackRearmMinMaxAllocBytes);
}

// --- 8c. Frontière auto-heal réelle : 4 095 compte, 4 096 ne compte pas ---
void test_boundary_autoheal_real_4095_restarts_4096_does_not(void) {
  // 4 095 (< strict) avec débounce atteint → Restart.
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart,
      evaluateSelfHeal(realInputs(kRestartMaxAllocHeapBytes - 1,
                                  kAutoHealDebounceChecks)));
  // 4 096 pile → ne compte pas comme famine : Continue, même « débounce atteint ».
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
      evaluateSelfHeal(realInputs(kRestartMaxAllocHeapBytes,
                                  kAutoHealDebounceChecks)));
}

// --- 8d. Frontière WARN réelle : cadence rapide à 12 287, normale à 12 288 ---
void test_boundary_warn_real_12287_fast_12288_normal(void) {
  TEST_ASSERT_EQUAL_UINT32(kHeapCheckFastIntervalMs,
      heapSampleIntervalMs(kLowHeapWarnBytes - 1, kLowHeapWarnBytes,
                           (uint32_t)kHealthCheckIntervalMs,
                           kHeapCheckFastIntervalMs));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kHealthCheckIntervalMs,
      heapSampleIntervalMs(kLowHeapWarnBytes, kLowHeapWarnBytes,
                           (uint32_t)kHealthCheckIntervalMs,
                           kHeapCheckFastIntervalMs));
}

// --- 8e. Débounce réel : 2 creux + remontée = rien ; 3 consécutifs = Restart ---
void test_debounce_real_two_dips_then_recovery_no_restart(void) {
  uint8_t below = 0;
  // 2 mesures sous 4 096 : comptées, mais débounce (3) pas atteint.
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
  // Remontée (une seule mesure saine suffit) : compteur remis à zéro.
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes));
  TEST_ASSERT_EQUAL_UINT8(0, below);
  // Preuve du reset : deux NOUVEAUX creux ne cumulent pas avec l'épisode
  // précédent (2 + 2 ≠ débounce) → toujours pas de restart.
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
}

void test_debounce_real_three_consecutive_restart(void) {
  uint8_t below = 0;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
  // 3ᵉ mesure consécutive sous le seuil = kAutoHealDebounceChecks → Restart.
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart,
                    tickHeapReal(below, kRestartMaxAllocHeapBytes - 1));
}

// --- 8f. Profil du 02/09/2026 (cas imposé par le gate) ---
void test_profile_20260902_sustained_famine_restarts(void) {
  // Famine SOUTENUE : maxBloc8 reste sous 4 096 pendant toute la durée du
  // débounce. Sous le WARN (12 288), la cadence passe à 5 s : les 3 ticks du
  // débounce couvrent ~15 s de famine réelle — c'est la fenêtre voulue.
  const uint32_t famine = 3060u;  // pire creux 8 bits jamais mesuré (15/08/2026)
  TEST_ASSERT_EQUAL_UINT32(kHeapCheckFastIntervalMs,
      heapSampleIntervalMs(famine, kLowHeapWarnBytes,
                           (uint32_t)kHealthCheckIntervalMs,
                           kHeapCheckFastIntervalMs));
  uint8_t below = 0;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, tickHeapReal(below, famine));
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, tickHeapReal(below, famine));
  TEST_ASSERT_EQUAL(SelfHealDecision::Restart,  tickHeapReal(below, famine));
}

void test_profile_20260902_brief_8s_dip_no_restart(void) {
  // Creux BREF de ~8 s : à la cadence rapide (5 s), il n'est vu que par 1 à 2
  // échantillons avant la remontée → AUCUN restart, ni pendant ni après.
  uint8_t below = 0;
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, tickHeapReal(below, 3500u));  // t = 0 s
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue, tickHeapReal(below, 3900u));  // t = 5 s
  // t = 10 s : le creux de 8 s est passé, retour au régime nominal terrain.
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kFieldNominalMaxBloc8));
  TEST_ASSERT_EQUAL_UINT8(0, below);
  TEST_ASSERT_EQUAL(SelfHealDecision::Continue,
                    tickHeapReal(below, kFieldNominalMaxBloc8));  // stable
}

// --- 8g. Réarmement parachute : hystérésis réelle à 18 431 / 18 432 ---
void test_rearm_real_hysteresis_18431_18432_never_downgrades(void) {
  // Prédicat d'hystérésis : 18 431 → pas de réarmement nominal ; 18 432 → oui.
  TEST_ASSERT_FALSE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes - 1,
                                   kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_TRUE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes,
                                  kOomRearmMinMaxAllocBytes));
  // Surclassement repli → nominal à la borne exacte (18 432)...
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
      rearmTargetReal((uint32_t)kOomFallbackReserveBytes,
                      kOomRearmMinMaxAllocBytes));
  // ...pas à 18 431 : on GARDE le repli (0 = rien à faire, pas d'oscillation).
  TEST_ASSERT_EQUAL_UINT32(0u,
      rearmTargetReal((uint32_t)kOomFallbackReserveBytes,
                      kOomRearmMinMaxAllocBytes - 1));
  // JAMAIS l'inverse : nominal armé → aucun déclassement vers le repli, même
  // avec le tas juste sous le seuil, au régime terrain, ou effondré.
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTargetReal((uint32_t)kOomReserveBytes,
                                               kOomRearmMinMaxAllocBytes - 1));
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTargetReal((uint32_t)kOomReserveBytes,
                                               kFieldNominalMaxBloc8));
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTargetReal((uint32_t)kOomReserveBytes, 0u));
}

int main(int, char**) {
  UNITY_BEGIN();
  // 1. Heap OK
  RUN_TEST(test_continue_when_heap_ok);
  // 2. Debounce non atteint
  RUN_TEST(test_continue_when_below_but_debounce_not_reached);
  RUN_TEST(test_continue_when_below_debounce_zero);
  // 3. Uptime insuffisant
  RUN_TEST(test_continue_when_uptime_below_min);
  // 4. Restart
  RUN_TEST(test_restart_when_all_conditions_met);
  RUN_TEST(test_restart_when_debounce_exceeded);
  // 5. Maintenance prioritaire
  RUN_TEST(test_maintenance_priority_over_restart);
  RUN_TEST(test_maintenance_even_when_healthy);
  RUN_TEST(test_maintenance_when_reboots_exceed_max);
  // 6. Frontières
  RUN_TEST(test_boundary_heap_equals_threshold_continue);
  RUN_TEST(test_boundary_heap_one_below_threshold_restart);
  RUN_TEST(test_boundary_debounce_equals_restart);
  RUN_TEST(test_boundary_uptime_equals_min_restart);
  RUN_TEST(test_boundary_uptime_one_below_min_continue);
  RUN_TEST(test_boundary_reboots_equals_max_maintenance);
  RUN_TEST(test_boundary_reboots_one_below_max_no_maintenance);
  RUN_TEST(test_reboots_one_below_max_still_restart);
  // 7. heapSampleIntervalMs (cadence adaptative)
  RUN_TEST(test_heap_interval_above_threshold_normal);
  RUN_TEST(test_heap_interval_below_threshold_fast);
  RUN_TEST(test_heap_interval_boundary_equals_threshold_normal);
  RUN_TEST(test_heap_interval_zero_fast);
  RUN_TEST(test_heap_interval_uint32_max_normal);
  // 8. Échelle réelle bug-080 (constants.h + réarmement parachute)
  RUN_TEST(test_scale_real_constants_bug080);
  RUN_TEST(test_rearm_arithmetic_never_triggers_autoheal_bug080);
  RUN_TEST(test_boundary_autoheal_real_4095_restarts_4096_does_not);
  RUN_TEST(test_boundary_warn_real_12287_fast_12288_normal);
  RUN_TEST(test_debounce_real_two_dips_then_recovery_no_restart);
  RUN_TEST(test_debounce_real_three_consecutive_restart);
  RUN_TEST(test_profile_20260902_sustained_famine_restarts);
  RUN_TEST(test_profile_20260902_brief_8s_dip_no_restart);
  RUN_TEST(test_rearm_real_hysteresis_18431_18432_never_downgrades);
  return UNITY_END();
}
