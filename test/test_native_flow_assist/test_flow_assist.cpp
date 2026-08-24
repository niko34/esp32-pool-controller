// =============================================================================
// Tests unitaires natifs — Assistant de mesure du débit (feature-075)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/flow_assist_logic.h
// (ADR-0017), header-only : aucune entrée dans build_src_filter n'est requise.
//   - isFlowAssistSessionExpired : expiration wrap-safe du timeout de
//     suspension (arithmétique uint32 NON SIGNÉE sur millis()) ;
//   - canStartFlowAssistPhase : verdict de démarrage d'une phase minutée
//     (ordre des gardes fail-closed : session → pompe → quota de purges) ;
//   - constantes de sécurité verrouillées (60 s / 120 s / 3 purges / 15 min).
// =============================================================================

#include <unity.h>
#include <stdint.h>

#include "constants.h"
#include "flow_assist_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. isFlowAssistSessionExpired — cas nominaux
// ---------------------------------------------------------------------------

void test_expired_not_before_timeout(void) {
  // 1 ms avant l'échéance : session encore vivante.
  TEST_ASSERT_FALSE(isFlowAssistSessionExpired(0, kFlowAssistSuspendTimeoutMs - 1,
                                               kFlowAssistSuspendTimeoutMs));
}

void test_expired_exactly_at_timeout(void) {
  // Frontière EXACTE : l'implémentation utilise >= — expirée pile au timeout.
  TEST_ASSERT_TRUE(isFlowAssistSessionExpired(0, kFlowAssistSuspendTimeoutMs,
                                              kFlowAssistSuspendTimeoutMs));
}

void test_expired_beyond_timeout(void) {
  TEST_ASSERT_TRUE(isFlowAssistSessionExpired(1000, 1000 + kFlowAssistSuspendTimeoutMs + 1,
                                              kFlowAssistSuspendTimeoutMs));
}

void test_not_expired_immediately_after_touch(void) {
  // nowMs == lastTouchMs (même milliseconde) : delta 0, pas expirée.
  TEST_ASSERT_FALSE(isFlowAssistSessionExpired(123456, 123456,
                                               kFlowAssistSuspendTimeoutMs));
}

// ---------------------------------------------------------------------------
// 2. isFlowAssistSessionExpired — wrap de millis() (rollover ~49,7 jours)
// ---------------------------------------------------------------------------

void test_wrap_not_expired_across_rollover(void) {
  // Touch 1 min avant le rollover, now 1 min après : delta réel = 2 min < 15 min.
  const uint32_t lastTouch = 0xFFFFFFFFu - 60000u + 1u;  // -60 s avant wrap
  const uint32_t now = 60000u;                            // +60 s après wrap
  TEST_ASSERT_FALSE(isFlowAssistSessionExpired(lastTouch, now,
                                               kFlowAssistSuspendTimeoutMs));
}

void test_wrap_expired_across_rollover(void) {
  // Touch 1 min avant le rollover, now 15 min après : delta réel = 16 min > 15 min.
  const uint32_t lastTouch = 0xFFFFFFFFu - 60000u + 1u;
  const uint32_t now = (uint32_t)kFlowAssistSuspendTimeoutMs;  // wrap + 15 min
  TEST_ASSERT_TRUE(isFlowAssistSessionExpired(lastTouch, now,
                                              kFlowAssistSuspendTimeoutMs));
}

void test_wrap_boundary_exact_across_rollover(void) {
  // Frontière exacte À CHEVAL sur le wrap : delta non signé == timeout → expirée.
  const uint32_t lastTouch = 0xFFFFFFFFu - 1000u;  // 1001 ms avant le wrap
  const uint32_t now = lastTouch + (uint32_t)kFlowAssistSuspendTimeoutMs;  // wrappe
  TEST_ASSERT_TRUE(now < lastTouch);  // sanity : le calcul a bien wrappé
  TEST_ASSERT_TRUE(isFlowAssistSessionExpired(lastTouch, now,
                                              kFlowAssistSuspendTimeoutMs));
}

// ---------------------------------------------------------------------------
// 3. isFlowAssistSessionExpired — ANTI-UNDERFLOW : nowMs < lastTouchMs
// ---------------------------------------------------------------------------
// Cas d'incohérence (lastTouchMs « dans le futur » hors wrap légitime, p. ex.
// horloge relue entre deux écritures). CHOIX DE L'IMPLÉMENTATION verrouillé
// ici : la différence non signée devient énorme (≈ 2^32 − delta) ≥ timeout →
// la session est considérée EXPIRÉE. Direction FAIL-CLOSED voulue : en cas de
// doute on ferme la session et on RÉTABLIT le dosage automatique (la
// suspension ne peut pas rester accrochée sur un état incohérent).
// Indiscernable d'un vrai wrap où now a très peu avancé — comportement DÉFINI,
// pas un accident.

void test_underflow_now_before_touch_is_expired(void) {
  // now 1 ms « avant » lastTouch → delta non signé = 0xFFFFFFFF ≥ timeout.
  TEST_ASSERT_TRUE(isFlowAssistSessionExpired(1000u, 999u,
                                              kFlowAssistSuspendTimeoutMs));
}

void test_underflow_large_gap_is_expired(void) {
  // Incohérence franche : lastTouch très au-delà de now.
  TEST_ASSERT_TRUE(isFlowAssistSessionExpired(0x80000000u, 0u,
                                              kFlowAssistSuspendTimeoutMs));
}

void test_underflow_small_negative_is_still_alive_only_within_wrap_window(void) {
  // Contre-exemple qui délimite la zone : si le « retard » de now est tel que
  // le delta non signé retombe SOUS le timeout (2^32 − 1 ms de retard… n'existe
  // pas), il n'y a AUCUNE valeur nowMs < lastTouchMs vivante à moins de
  // timeout − 1 du wrap complet. On vérifie le pire delta encore vivant :
  // delta non signé = timeout − 1 via un wrap légitime.
  const uint32_t lastTouch = 0xFFFFFFF0u;
  const uint32_t now = lastTouch + (uint32_t)kFlowAssistSuspendTimeoutMs - 1u;  // wrappe
  TEST_ASSERT_TRUE(now < lastTouch);  // sanity : nowMs < lastTouchMs
  TEST_ASSERT_FALSE(isFlowAssistSessionExpired(lastTouch, now,
                                               kFlowAssistSuspendTimeoutMs));
}

// ---------------------------------------------------------------------------
// 4. canStartFlowAssistPhase — purge : quota de session
// ---------------------------------------------------------------------------

void test_purge_allowed_when_quota_available(void) {
  for (uint8_t done = 0; done < kFlowAssistMaxPurgesPerSession; done++) {
    TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge, done,
                                             kFlowAssistMaxPurgesPerSession,
                                             /*sessionActive=*/true,
                                             /*pumpBusy=*/false) ==
                     FlowAssistRefusal::kNone);
  }
}

void test_purge_refused_at_quota(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge,
                                           kFlowAssistMaxPurgesPerSession,
                                           kFlowAssistMaxPurgesPerSession,
                                           true, false) ==
                   FlowAssistRefusal::kPurgeQuota);
}

void test_purge_refused_beyond_quota(void) {
  // Défense en profondeur : compteur incohérent (> max) → toujours refusé.
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge,
                                           (uint8_t)(kFlowAssistMaxPurgesPerSession + 1),
                                           kFlowAssistMaxPurgesPerSession,
                                           true, false) ==
                   FlowAssistRefusal::kPurgeQuota);
}

// ---------------------------------------------------------------------------
// 5. canStartFlowAssistPhase — mesure : jamais soumise au quota de purges
// ---------------------------------------------------------------------------

void test_measure_allowed_in_active_session(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kMeasure, 0,
                                           kFlowAssistMaxPurgesPerSession,
                                           true, false) ==
                   FlowAssistRefusal::kNone);
}

void test_measure_allowed_even_when_purge_quota_exhausted(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kMeasure,
                                           kFlowAssistMaxPurgesPerSession,
                                           kFlowAssistMaxPurgesPerSession,
                                           true, false) ==
                   FlowAssistRefusal::kNone);
}

// ---------------------------------------------------------------------------
// 6. canStartFlowAssistPhase — pompe occupée
// ---------------------------------------------------------------------------

void test_purge_refused_when_pump_busy(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge, 0,
                                           kFlowAssistMaxPurgesPerSession,
                                           true, /*pumpBusy=*/true) ==
                   FlowAssistRefusal::kPumpBusy);
}

void test_measure_refused_when_pump_busy(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kMeasure, 0,
                                           kFlowAssistMaxPurgesPerSession,
                                           true, true) ==
                   FlowAssistRefusal::kPumpBusy);
}

// ---------------------------------------------------------------------------
// 7. canStartFlowAssistPhase — session inactive (garde prioritaire, fail-closed)
// ---------------------------------------------------------------------------
// Sémantique côté route : /flow-assist/start passe TOUJOURS sessionActive=true
// (le start OUVRE la session) — la branche kSessionInactive protège tout futur
// appelant qui interrogerait le verdict hors session. On la verrouille.

void test_purge_refused_when_session_inactive(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge, 0,
                                           kFlowAssistMaxPurgesPerSession,
                                           /*sessionActive=*/false, false) ==
                   FlowAssistRefusal::kSessionInactive);
}

void test_measure_refused_when_session_inactive(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kMeasure, 0,
                                           kFlowAssistMaxPurgesPerSession,
                                           false, false) ==
                   FlowAssistRefusal::kSessionInactive);
}

void test_guard_order_session_inactive_wins_over_pump_busy(void) {
  // Ordre fail-closed documenté : session → pompe → quota. Session inactive
  // masque pompe occupée ET quota épuisé.
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge,
                                           kFlowAssistMaxPurgesPerSession,
                                           kFlowAssistMaxPurgesPerSession,
                                           false, true) ==
                   FlowAssistRefusal::kSessionInactive);
}

void test_guard_order_pump_busy_wins_over_purge_quota(void) {
  TEST_ASSERT_TRUE(canStartFlowAssistPhase(FlowAssistPhase::kPurge,
                                           kFlowAssistMaxPurgesPerSession,
                                           kFlowAssistMaxPurgesPerSession,
                                           true, true) ==
                   FlowAssistRefusal::kPumpBusy);
}

// ---------------------------------------------------------------------------
// 8. Constantes de sécurité verrouillées (valeurs du verdict gate feature-075)
// ---------------------------------------------------------------------------

void test_constant_purge_duration_is_60s(void) {
  TEST_ASSERT_EQUAL_UINT32(60u, kFlowAssistPurgeDurationS);
}

void test_constant_measure_duration_is_120s(void) {
  TEST_ASSERT_EQUAL_UINT32(120u, kFlowAssistMeasureDurationS);
}

void test_constant_max_purges_is_3(void) {
  TEST_ASSERT_EQUAL_UINT8(3u, kFlowAssistMaxPurgesPerSession);
}

void test_constant_suspend_timeout_is_15min(void) {
  TEST_ASSERT_EQUAL_UINT32(900000u, (uint32_t)kFlowAssistSuspendTimeoutMs);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_expired_not_before_timeout);
  RUN_TEST(test_expired_exactly_at_timeout);
  RUN_TEST(test_expired_beyond_timeout);
  RUN_TEST(test_not_expired_immediately_after_touch);

  RUN_TEST(test_wrap_not_expired_across_rollover);
  RUN_TEST(test_wrap_expired_across_rollover);
  RUN_TEST(test_wrap_boundary_exact_across_rollover);

  RUN_TEST(test_underflow_now_before_touch_is_expired);
  RUN_TEST(test_underflow_large_gap_is_expired);
  RUN_TEST(test_underflow_small_negative_is_still_alive_only_within_wrap_window);

  RUN_TEST(test_purge_allowed_when_quota_available);
  RUN_TEST(test_purge_refused_at_quota);
  RUN_TEST(test_purge_refused_beyond_quota);

  RUN_TEST(test_measure_allowed_in_active_session);
  RUN_TEST(test_measure_allowed_even_when_purge_quota_exhausted);

  RUN_TEST(test_purge_refused_when_pump_busy);
  RUN_TEST(test_measure_refused_when_pump_busy);

  RUN_TEST(test_purge_refused_when_session_inactive);
  RUN_TEST(test_measure_refused_when_session_inactive);
  RUN_TEST(test_guard_order_session_inactive_wins_over_pump_busy);
  RUN_TEST(test_guard_order_pump_busy_wins_over_purge_quota);

  RUN_TEST(test_constant_purge_duration_is_60s);
  RUN_TEST(test_constant_measure_duration_is_120s);
  RUN_TEST(test_constant_max_purges_is_3);
  RUN_TEST(test_constant_suspend_timeout_is_15min);

  return UNITY_END();
}
