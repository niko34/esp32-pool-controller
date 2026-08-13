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
//   - oomTlsGateReason : garde TLS sortante partagée par /check-update et
//     /download-update (web_routes_ota.cpp) — motif PSRAM LIBRE (fail-closed,
//     évalué en premier) puis 4 quadrants heap + limites exactes, avec les
//     VALEURS RÉELLES kOtaTlsMinPsramBytes / kCheckUpdateMin* ;
//   - non-régression bug-garde-tls-trop-permissive-sans-psram (v2.18.6) : les
//     mesures TERRAIN du 12/08 sur WROOM sans PSRAM (free=87708 o,
//     maxBloc=34804 o) que la v2.18.5 laissait passer doivent être REFUSÉES,
//     avec priorité du motif PSRAM sur le motif mémoire ; idem pour une PSRAM
//     installée mais SATURÉE (revue v2.18.6 : le critère est la PSRAM LIBRE).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "oom_guard_logic.h"
#include "constants.h"  // kOomRearmMinMaxAllocBytes, kOtaTlsMinPsramBytes, kCheckUpdateMin*

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
// 5. oomTlsGateReason — garde TLS sortante (PSRAM libre + bicritère heap)
// ---------------------------------------------------------------------------
// Valeurs RÉELLES de production : kOtaTlsMinPsramBytes = 1048576,
// kCheckUpdateMinFreeHeapBytes = 65536, kCheckUpdateMinMaxAllocBytes = 20480
// (constants.h). Seuils INCLUSIFS (>=). PSRAM LIBRE évaluée EN PREMIER : les cas
// de refus heap doivent donc fournir une PSRAM libre conforme pour être atteints.
// Le 1er paramètre est la PSRAM **LIBRE** (ESP.getFreePsram() côté route), pas
// la taille installée : une PSRAM installée mais saturée ne protège de rien.
static const uint32_t kPsramWroverFreeBytes = 4194304u;  // ~4 Mo LIBRES sur un WROVER-E au repos

void test_tls_gate_ok_both_criteria_above(void) {
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes + 1,
                                           kCheckUpdateMinMaxAllocBytes + 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_refuses_low_free_heap_only(void) {
  // Quadrant : free bas / maxBloc haut → refus mémoire (heap total insuffisant)
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes - 1,
                                           kCheckUpdateMinMaxAllocBytes + 10000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_refuses_low_max_alloc_only(void) {
  // Quadrant : free haut / maxBloc bas → refus mémoire (fragmentation)
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes + 100000,
                                           kCheckUpdateMinMaxAllocBytes - 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_refuses_both_low(void) {
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes - 1,
                                           kCheckUpdateMinMaxAllocBytes - 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes, 0, 0,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_exact_thresholds_pass(void) {
  // Limites exactes : 65536 / 20480 → PASSE (>= inclusif sur les deux critères)
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // Un critère exactement à la limite, l'autre au-dessus → passe aussi
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes + 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes + 1,
                                           kCheckUpdateMinMaxAllocBytes,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // PSRAM exactement au seuil → passe aussi (>= inclusif sur les trois critères)
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_real_constant_values(void) {
  // Verrouille les valeurs réelles (T2) : si un futur commit change les seuils
  // de constants.h sans mettre à jour la doc/les tests, ce test casse.
  TEST_ASSERT_EQUAL_UINT32(1048576u, kOtaTlsMinPsramBytes);
  TEST_ASSERT_EQUAL_UINT32(65536u, kCheckUpdateMinFreeHeapBytes);
  TEST_ASSERT_EQUAL_UINT32(20480u, kCheckUpdateMinMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(32768u, kOomRearmMinMaxAllocBytes);
  // Scénario réaliste du crash du 11/08 : heap ~48 Ko, maxBloc ~12 Ko → refus
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes, 49152, 12288,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // Mesure terrain du 12/08 (WROOM sans PSRAM, v2.18.5) : free=87708 o et
  // maxBloc=34804 o passaient l'ancienne garde → désormais refus PSRAM.
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(0, 87708, 34804,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

// ---------------------------------------------------------------------------
// 6. oomTlsGateReason — critère PSRAM fail-closed (v2.18.6, non-régression)
// ---------------------------------------------------------------------------
// Le bug : la v2.18.5 n'évaluait QUE le bicritère heap, donc un WROOM sans
// PSRAM en régime normal passait la garde puis affamait le tas pendant le
// handshake (fenêtre de crash bad_alloc ~1 s). Le refus PSRAM doit être
// INDÉPENDANT de l'état du heap et PRIORITAIRE sur le motif mémoire.

void test_tls_gate_field_measurement_20260812_now_refused(void) {
  // Chiffres EXACTS mesurés en production le 12/08 (WROOM-32 sans PSRAM,
  // core 3, v2.18.5) : la garde bicritère les acceptait (87708 >= 65536 et
  // 34804 >= 20480) → tentative TLS vouée à l'échec. Doit désormais être
  // refusée avec le motif matériel.
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(0, 87708, 34804,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_no_psram_refused_even_with_luxurious_heap(void) {
  // Heap largement au-dessus des deux seuils : le refus ne dépend PAS du heap.
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(0, 200000, 100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // Cas extrême : heap "infini" mais toujours pas de PSRAM
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(0, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_psram_reason_wins_over_low_memory(void) {
  // PSRAM absente ET heap dégradé : le motif rendu doit être NoPsram (refus
  // DÉFINITIF orientant vers l'upload manuel), pas LowMemory (qui invite à
  // réessayer une opération de toute façon impossible sur ce matériel).
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(0,
                                           kCheckUpdateMinFreeHeapBytes - 1,
                                           kCheckUpdateMinMaxAllocBytes - 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(0, 0, 0,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_insufficient_psram_is_refused(void) {
  // PSRAM libre présente mais trop faible (512 Ko < 1 Mo requis) : mbedTLS ne
  // trouverait pas ses blocs de ~16 Ko → même refus matériel que PSRAM=0.
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(524288u, 200000, 100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_saturated_psram_is_refused(void) {
  // Revue v2.18.6 : WROVER-E avec 4 Mo INSTALLÉS mais quasi SATURÉS (256 Ko
  // libres) et heap interne parfaitement sain. Passer getPsramSize() aurait
  // ouvert la garde (4 Mo >= 1 Mo, valeur constante et jamais décroissante) et
  // mbedTLS serait retombé sur la RAM interne — le régime de crash de v2.18.5.
  // Avec getFreePsram(), 262144 < 1048576 → refus matériel.
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(262144u, 200000, 100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_psram_threshold_is_inclusive(void) {
  // Limite exacte : 1048576 o → PAS de refus PSRAM (>= inclusif)...
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kOtaTlsMinPsramBytes, 200000, 100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // ...et à la limite exacte avec heap dégradé, le motif basculé est bien le
  // motif MÉMOIRE (donc le critère PSRAM a été franchi, pas contourné).
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes - 1,
                                           kCheckUpdateMinMaxAllocBytes,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // Un octet sous le seuil → refus PSRAM
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateNoPsram,
                          oomTlsGateReason(kOtaTlsMinPsramBytes - 1, 200000, 100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
}

void test_tls_gate_with_psram_preserves_v2185_behaviour(void) {
  // Contraste direct avec le cas terrain : MÊMES mesures heap, mais sur un
  // module PSRAM → autorisé (le comportement bicritère v2.18.5 est intact).
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes, 87708, 34804,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // Les 4 quadrants heap, PSRAM conforme : haut/haut = OK, les 3 autres = mémoire
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes, 200000, 100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes, 200000,
                                           kCheckUpdateMinMaxAllocBytes - 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes - 1,
                                           100000,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateLowMemory,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes - 1,
                                           kCheckUpdateMinMaxAllocBytes - 1,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes));
  // Limites exactes des deux critères heap avec PSRAM conforme → OK (inclusif)
  TEST_ASSERT_EQUAL_UINT8(kOomTlsGateOk,
                          oomTlsGateReason(kPsramWroverFreeBytes,
                                           kCheckUpdateMinFreeHeapBytes,
                                           kCheckUpdateMinMaxAllocBytes,
                                           kOtaTlsMinPsramBytes,
                                           kCheckUpdateMinFreeHeapBytes,
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
  RUN_TEST(test_tls_gate_field_measurement_20260812_now_refused);
  RUN_TEST(test_tls_gate_no_psram_refused_even_with_luxurious_heap);
  RUN_TEST(test_tls_gate_psram_reason_wins_over_low_memory);
  RUN_TEST(test_tls_gate_insufficient_psram_is_refused);
  RUN_TEST(test_tls_gate_saturated_psram_is_refused);
  RUN_TEST(test_tls_gate_psram_threshold_is_inclusive);
  RUN_TEST(test_tls_gate_with_psram_preserves_v2185_behaviour);
  return UNITY_END();
}
