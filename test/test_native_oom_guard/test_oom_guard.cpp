// =============================================================================
// Tests unitaires natifs — Logique pure du parachute OOM global
// (oom_guard_logic, bug-oom-accept-http-durcissement)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/oom_guard_logic.h
// (ADR-0017, modèle heap_ring_logic.h) :
//   - oomOnAllocFailed : consommation de la réserve (Armed→Released) puis
//     épuisement (Released→Depleted, bad_alloc laissé suivre) ;
//   - oomShouldLogRelease : one-shot du log CRITICAL par épisode ;
//   - oomShouldRearm : hystérésis kOomRearmMinMaxAllocBytes — seuil INCLUSIF
//     (>= : à maxBloc == 32768 exactement, on réarme) ;
//   - cycle complet échec→réarmement→échec→réarmement (pas d'oscillation
//     d'états, log one-shot rejoué à chaque nouvel épisode) ;
//   - oomTlsGateOk : garde bicritère TLS partagée avec /check-update
//     (web_routes_ota.cpp) — 4 quadrants + limites exactes, avec les VALEURS
//     RÉELLES kCheckUpdateMinFreeHeapBytes / kCheckUpdateMinMaxAllocBytes.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "oom_guard_logic.h"
#include "constants.h"  // kOomRearmMinMaxAllocBytes, kCheckUpdateMin* (valeurs réelles)

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. oomOnAllocFailed — consommation de la réserve
// ---------------------------------------------------------------------------

void test_first_alloc_failure_releases_reserve(void) {
  // État Armed, réserve extraite avec succès (exchange non nul côté impur) :
  // l'action doit LIBÉRER la réserve (le new est re-tenté et réussit),
  // ne PAS désenregistrer le handler, et passer en Released.
  OomHandlerAction a = oomOnAllocFailed(true);
  TEST_ASSERT_TRUE(a.freeReserve);
  TEST_ASSERT_FALSE(a.unregisterHandler);
  TEST_ASSERT_EQUAL_UINT8(kOomStateReleased, a.nextState);
}

void test_second_alloc_failure_depletes_and_lets_bad_alloc_follow(void) {
  // Réserve déjà consommée (exchange nul) : l'action doit se désenregistrer
  // (le new suivant lève bad_alloc = comportement d'avant le parachute),
  // ne RIEN libérer, et passer en Depleted.
  OomHandlerAction a = oomOnAllocFailed(false);
  TEST_ASSERT_FALSE(a.freeReserve);
  TEST_ASSERT_TRUE(a.unregisterHandler);
  TEST_ASSERT_EQUAL_UINT8(kOomStateDepleted, a.nextState);
}

// ---------------------------------------------------------------------------
// 2. oomShouldLogRelease — one-shot du log CRITICAL par épisode
// ---------------------------------------------------------------------------

void test_log_release_fires_once_per_episode(void) {
  uint32_t releases = 0, logged = 0;
  // Aucun épisode : pas de log
  TEST_ASSERT_FALSE(oomShouldLogRelease(releases, logged));
  // 1er épisode (handler a incrémenté) : log dû UNE fois
  releases = 1;
  TEST_ASSERT_TRUE(oomShouldLogRelease(releases, logged));
  logged = releases;  // la loop draine
  // Ticks suivants sans nouvel épisode : plus jamais de log
  TEST_ASSERT_FALSE(oomShouldLogRelease(releases, logged));
  TEST_ASSERT_FALSE(oomShouldLogRelease(releases, logged));
}

void test_log_release_stays_one_shot_after_n_failures_same_cycle(void) {
  // N échecs d'alloc dans le MÊME cycle Armed→Released : le handler ne libère
  // la réserve qu'une fois (les échecs suivants tombent en Depleted, compteur
  // d'épisodes inchangé) → releaseCount reste à 1, un seul log.
  uint32_t releases = 1;  // 1er échec : épisode compté
  // 2e, 3e... échecs : oomOnAllocFailed(false) ne compte PAS d'épisode
  OomHandlerAction a = oomOnAllocFailed(false);
  TEST_ASSERT_FALSE(a.freeReserve);  // → pas d'incrément côté impur
  uint32_t logged = 0;
  TEST_ASSERT_TRUE(oomShouldLogRelease(releases, logged));   // une fois
  logged = releases;
  TEST_ASSERT_FALSE(oomShouldLogRelease(releases, logged));  // plus jamais
}

void test_log_release_fires_again_on_new_episode(void) {
  // Après réarmement + nouvelle famine : releaseCount passe 1→2 → nouveau log
  uint32_t logged = 1;
  TEST_ASSERT_FALSE(oomShouldLogRelease(1, logged));
  TEST_ASSERT_TRUE(oomShouldLogRelease(2, logged));
  logged = 2;
  TEST_ASSERT_FALSE(oomShouldLogRelease(2, logged));
}

// ---------------------------------------------------------------------------
// 3. oomShouldRearm — hystérésis (seuil kOomRearmMinMaxAllocBytes = 32768)
// ---------------------------------------------------------------------------
// Choix documenté : le seuil est INCLUSIF (>=) — à maxBloc == 32768 exactement
// le réarmement est autorisé (cf. `>=` dans oomShouldRearm).
// Depuis la revue v2.18.5, le critère est l'ABSENCE DE RÉSERVE (pointeur nul),
// pas l'état : reserveAbsent=true ⇔ sOomReserve == nullptr côté impur.

void test_rearm_denied_below_threshold(void) {
  TEST_ASSERT_FALSE(oomShouldRearm(true,  // réserve absente (Released)
                                   kOomRearmMinMaxAllocBytes - 1,
                                   kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_FALSE(oomShouldRearm(true,  // réserve absente (Depleted)
                                   0, kOomRearmMinMaxAllocBytes));
}

void test_rearm_allowed_at_exact_threshold(void) {
  // Limite exacte : 32768 → réarme (>= inclusif)
  TEST_ASSERT_TRUE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes,
                                  kOomRearmMinMaxAllocBytes));
}

void test_rearm_allowed_above_threshold(void) {
  TEST_ASSERT_TRUE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes + 1,
                                  kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_TRUE(oomShouldRearm(true, 100000, kOomRearmMinMaxAllocBytes));
}

void test_rearm_never_when_reserve_present(void) {
  // Réserve présente : jamais de réarmement, même avec un heap immense (pas
  // de double allocation de réserve depuis la loop)
  TEST_ASSERT_FALSE(oomShouldRearm(false, 0xFFFFFFFFu,
                                   kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_FALSE(oomShouldRearm(false, kOomRearmMinMaxAllocBytes,
                                   kOomRearmMinMaxAllocBytes));
}

void test_rearm_converges_after_handler_race(void) {
  // Course revue v2.18.5 : le handler consomme la réserve ENTRE l'exchange du
  // réarmement et le store(Armed) → état affiché Armed, réserve nulle. Le
  // critère pointeur (reserveAbsent=true) doit quand même autoriser le
  // réarmement au tick suivant — l'ancien critère d'état l'aurait bloqué.
  TEST_ASSERT_TRUE(oomShouldRearm(true /* réserve nulle malgré état Armed */,
                                  kOomRearmMinMaxAllocBytes,
                                  kOomRearmMinMaxAllocBytes));
}

// ---------------------------------------------------------------------------
// 4. Cycle complet — échec→réarmement→échec→réarmement (pas d'oscillation)
// ---------------------------------------------------------------------------

void test_full_cycle_failure_rearm_failure_rearm(void) {
  uint32_t releases = 0, logged = 0;
  uint8_t state = kOomStateArmed;

  // --- Épisode 1 : famine, réserve consommée ---
  OomHandlerAction a1 = oomOnAllocFailed(true);
  state = a1.nextState;
  releases++;  // côté impur : fetch_add si freeReserve
  TEST_ASSERT_EQUAL_UINT8(kOomStateReleased, state);
  TEST_ASSERT_TRUE(oomShouldLogRelease(releases, logged));
  logged = releases;

  // Heap encore famélique : pas de réarmement (hystérésis). Réserve absente
  // après consommation (reserveAbsent = true côté impur).
  TEST_ASSERT_FALSE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes - 1,
                                   kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_FALSE(oomShouldLogRelease(releases, logged));  // log déjà émis

  // Heap récupéré : réarmement (la loop ré-alloue → Armed, réserve présente)
  TEST_ASSERT_TRUE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes + 4096,
                                  kOomRearmMinMaxAllocBytes));
  state = kOomStateArmed;

  // --- Épisode 2 : nouvelle famine, le one-shot rejoue ---
  OomHandlerAction a2 = oomOnAllocFailed(true);
  state = a2.nextState;
  releases++;
  TEST_ASSERT_EQUAL_UINT8(kOomStateReleased, state);
  TEST_ASSERT_TRUE(oomShouldLogRelease(releases, logged));  // 2e log dû
  logged = releases;
  TEST_ASSERT_FALSE(oomShouldLogRelease(releases, logged));

  // Réarmement final : retour au régime nominal
  TEST_ASSERT_TRUE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes,
                                  kOomRearmMinMaxAllocBytes));
  state = kOomStateArmed;
  (void)state;
  TEST_ASSERT_FALSE(oomShouldRearm(false /* réserve ré-allouée */,
                                   kOomRearmMinMaxAllocBytes,
                                   kOomRearmMinMaxAllocBytes));  // stable, pas d'oscillation
}

void test_depleted_path_then_rearm_recovers_handler(void) {
  // Double famine SANS réarmement intermédiaire : Armed→Released→Depleted,
  // puis le réarmement reste possible depuis Depleted (state != Armed).
  uint8_t state = oomOnAllocFailed(true).nextState;
  TEST_ASSERT_EQUAL_UINT8(kOomStateReleased, state);
  state = oomOnAllocFailed(false).nextState;
  TEST_ASSERT_EQUAL_UINT8(kOomStateDepleted, state);
  // Réserve absente en Depleted → réarmement possible dès que maxBloc remonte
  TEST_ASSERT_TRUE(oomShouldRearm(true, kOomRearmMinMaxAllocBytes,
                                  kOomRearmMinMaxAllocBytes));
}

// ---------------------------------------------------------------------------
// 5. oomTlsGateOk — garde bicritère /check-update (4 quadrants + limites)
// ---------------------------------------------------------------------------
// Valeurs RÉELLES de production : kCheckUpdateMinFreeHeapBytes = 65536,
// kCheckUpdateMinMaxAllocBytes = 20480 (constants.h). Seuils INCLUSIFS (>=).

void test_tls_gate_ok_both_criteria_above(void) {
  TEST_ASSERT_TRUE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes + 1,
                                kCheckUpdateMinMaxAllocBytes + 1,
                                kCheckUpdateMinFreeHeapBytes,
                                kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_refuses_low_free_heap_only(void) {
  // Quadrant : free bas / maxBloc haut → refus (heap total insuffisant)
  TEST_ASSERT_FALSE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes - 1,
                                 kCheckUpdateMinMaxAllocBytes + 10000,
                                 kCheckUpdateMinFreeHeapBytes,
                                 kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_refuses_low_max_alloc_only(void) {
  // Quadrant : free haut / maxBloc bas → refus (fragmentation)
  TEST_ASSERT_FALSE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes + 100000,
                                 kCheckUpdateMinMaxAllocBytes - 1,
                                 kCheckUpdateMinFreeHeapBytes,
                                 kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_refuses_both_low(void) {
  TEST_ASSERT_FALSE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes - 1,
                                 kCheckUpdateMinMaxAllocBytes - 1,
                                 kCheckUpdateMinFreeHeapBytes,
                                 kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_FALSE(oomTlsGateOk(0, 0, kCheckUpdateMinFreeHeapBytes,
                                 kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_exact_thresholds_pass(void) {
  // Limites exactes : 65536 / 20480 → PASSE (>= inclusif sur les deux critères)
  TEST_ASSERT_TRUE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes,
                                kCheckUpdateMinMaxAllocBytes,
                                kCheckUpdateMinFreeHeapBytes,
                                kCheckUpdateMinMaxAllocBytes));
  // Un critère exactement à la limite, l'autre au-dessus → passe aussi
  TEST_ASSERT_TRUE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes,
                                kCheckUpdateMinMaxAllocBytes + 1,
                                kCheckUpdateMinFreeHeapBytes,
                                kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_TRUE(oomTlsGateOk(kCheckUpdateMinFreeHeapBytes + 1,
                                kCheckUpdateMinMaxAllocBytes,
                                kCheckUpdateMinFreeHeapBytes,
                                kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_real_constant_values(void) {
  // Verrouille les valeurs réelles (T2) : si un futur commit change les seuils
  // de constants.h sans mettre à jour la doc/les tests, ce test casse.
  TEST_ASSERT_EQUAL_UINT32(65536u, kCheckUpdateMinFreeHeapBytes);
  TEST_ASSERT_EQUAL_UINT32(20480u, kCheckUpdateMinMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(32768u, kOomRearmMinMaxAllocBytes);
  // Scénario réaliste du crash du 11/08 : heap ~48 Ko, maxBloc ~12 Ko → refus
  TEST_ASSERT_FALSE(oomTlsGateOk(49152, 12288, kCheckUpdateMinFreeHeapBytes,
                                 kCheckUpdateMinMaxAllocBytes));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_alloc_failure_releases_reserve);
  RUN_TEST(test_second_alloc_failure_depletes_and_lets_bad_alloc_follow);
  RUN_TEST(test_log_release_fires_once_per_episode);
  RUN_TEST(test_log_release_stays_one_shot_after_n_failures_same_cycle);
  RUN_TEST(test_log_release_fires_again_on_new_episode);
  RUN_TEST(test_rearm_denied_below_threshold);
  RUN_TEST(test_rearm_allowed_at_exact_threshold);
  RUN_TEST(test_rearm_allowed_above_threshold);
  RUN_TEST(test_rearm_never_when_reserve_present);
  RUN_TEST(test_rearm_converges_after_handler_race);
  RUN_TEST(test_full_cycle_failure_rearm_failure_rearm);
  RUN_TEST(test_depleted_path_then_rearm_recovers_handler);
  RUN_TEST(test_tls_gate_ok_both_criteria_above);
  RUN_TEST(test_tls_gate_refuses_low_free_heap_only);
  RUN_TEST(test_tls_gate_refuses_both_low);
  RUN_TEST(test_tls_gate_refuses_low_max_alloc_only);
  RUN_TEST(test_tls_gate_exact_thresholds_pass);
  RUN_TEST(test_tls_gate_real_constant_values);
  return UNITY_END();
}
