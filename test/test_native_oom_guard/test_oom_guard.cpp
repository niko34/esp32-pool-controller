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
//     (>= : à maxBloc == 24576 exactement, on réarme) ;
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
// 3. oomShouldRearm — hystérésis (seuil kOomRearmMinMaxAllocBytes = 24576)
// ---------------------------------------------------------------------------
// Choix documenté : le seuil est INCLUSIF (>=) — à maxBloc == 24576 exactement
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
  // Limite exacte : 24576 → réarme (>= inclusif)
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
  // bug-parachute-oom-jamais-rearme (v2.18.8) : 32768 → 24576 (= 2 ×
  // kOomReserveBytes). L'ancien seuil était AU-DESSUS du plus gros bloc réel de
  // l'appareil (27-30 Ko mesurés) → réserve jamais ré-allouée, parachute
  // one-shot en pratique.
  TEST_ASSERT_EQUAL_UINT32(24576u, kOomRearmMinMaxAllocBytes);
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

// ===========================================================================
// 7. oomRearmTargetBytes — réarmement à DEUX PALIERS (v2.18.8)
// ===========================================================================
// Défaut n°1 du 13/08/2026 : seuil unique 32768 o alors que le plus gros bloc
// contigu réel de l'appareil vaut 27-30 Ko → réserve JAMAIS ré-allouée après sa
// première consommation (parachute one-shot). Les tests ci-dessous rejouent les
// mesures TERRAIN et verrouillent le correctif.
//
// Convention des seuils : INCLUSIFS (>=) sur les deux paliers — à maxBloc ==
// 24576 exactement on arme le NOMINAL, à maxBloc == 12288 exactement on arme le
// REPLI. Retour 0 = NE RIEN FAIRE.

// Helper : appel avec les valeurs RÉELLES de constants.h (aucune valeur en dur).
static uint32_t rearmTarget(uint32_t armedBytes, uint32_t maxAllocBytes) {
  return oomRearmTargetBytes(armedBytes, maxAllocBytes,
                             (uint32_t)kOomReserveBytes, kOomRearmMinMaxAllocBytes,
                             (uint32_t)kOomFallbackReserveBytes,
                             kOomFallbackRearmMinMaxAllocBytes);
}

static const uint32_t kLegacyRearmMinMaxAllocBytes = 32768u;  // seuil v2.18.5 (le bug)

void test_rearm_real_constant_values_v2188(void) {
  // Verrouille l'échelle des seuils : tout futur changement casse ici.
  TEST_ASSERT_EQUAL_UINT32(12288u, (uint32_t)kOomReserveBytes);
  TEST_ASSERT_EQUAL_UINT32(24576u, kOomRearmMinMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(2048u, (uint32_t)kOomFallbackReserveBytes);
  TEST_ASSERT_EQUAL_UINT32(12288u, kOomFallbackRearmMinMaxAllocBytes);
  // Le palier de repli DOIT être strictement en dessous du nominal (sinon un
  // seul palier utile, et le surclassement n'a plus de sens).
  TEST_ASSERT_TRUE(kOomFallbackRearmMinMaxAllocBytes < kOomRearmMinMaxAllocBytes);
  TEST_ASSERT_TRUE((uint32_t)kOomFallbackReserveBytes < (uint32_t)kOomReserveBytes);
}

void test_rearm_field_maxblock_27584_at_12h9_arms_nominal(void) {
  // E1 — mesure TERRAIN du 13/08/2026 à 12,9 h d'uptime, au moment du crash :
  // maxB=27584 o (free=81408 o). L'ancien seuil 32768 refusait le réarmement →
  // parachute désarmé → bad_alloc de 156 o non rattrapé → abort.
  TEST_ASSERT_FALSE(oomShouldRearm(true, 27584u, kLegacyRearmMinMaxAllocBytes));  // le BUG
  // Correctif : ce même maxBloc arme désormais le palier NOMINAL (12 Ko).
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes, rearmTarget(0u, 27584u));
  // Et le prédicat historique d'hystérésis suit le nouveau seuil.
  TEST_ASSERT_TRUE(oomShouldRearm(true, 27584u, kOomRearmMinMaxAllocBytes));
}

void test_rearm_field_maxblock_29684_at_33min_arms_nominal(void) {
  // E1 — mesure TERRAIN du 13/08/2026 à 33 min d'uptime : maxB=29684 o. Même
  // démonstration : refusé par l'ancien seuil, nominal avec le nouveau.
  TEST_ASSERT_FALSE(oomShouldRearm(true, 29684u, kLegacyRearmMinMaxAllocBytes));  // le BUG
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes, rearmTarget(0u, 29684u));
  TEST_ASSERT_TRUE(oomShouldRearm(true, 29684u, kOomRearmMinMaxAllocBytes));
  // Depuis le palier de REPLI déjà armé, le régime réel permet le surclassement
  // (2 Ko → 12 Ko) : c'est la sortie de l'état dégradé, jamais atteinte en v2.18.5.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
                           rearmTarget((uint32_t)kOomFallbackReserveBytes, 29684u));
}

void test_rearm_nothing_when_heap_below_fallback_threshold(void) {
  // E2 — aucune réserve + tas trop dégradé même pour le repli → 0 (ne rien faire)
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(0u, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(0u, kOomFallbackRearmMinMaxAllocBytes - 1u));
}

void test_rearm_fallback_tier_between_thresholds(void) {
  // E2 — maxBloc ∈ [12288, 24576[ → palier de REPLI (2 Ko)
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                           rearmTarget(0u, kOomFallbackRearmMinMaxAllocBytes));  // borne INCLUSIVE
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                           rearmTarget(0u, kOomFallbackRearmMinMaxAllocBytes + 1u));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                           rearmTarget(0u, kOomRearmMinMaxAllocBytes - 1u));  // juste sous le nominal
}

void test_rearm_nominal_tier_at_and_above_threshold(void) {
  // E2 — maxBloc >= 24576 → palier NOMINAL (12 Ko), borne INCLUSIVE
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
                           rearmTarget(0u, kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
                           rearmTarget(0u, kOomRearmMinMaxAllocBytes + 1u));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes, rearmTarget(0u, 0xFFFFFFFFu));
}

void test_rearm_upgrade_fallback_to_nominal_allowed(void) {
  // E2 — repli armé + tas remonté au-dessus du seuil nominal → surclassement
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
                           rearmTarget((uint32_t)kOomFallbackReserveBytes,
                                       kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
                           rearmTarget((uint32_t)kOomFallbackReserveBytes, 60000u));
  // ...mais tant que le tas reste sous le seuil nominal, on NE TOUCHE À RIEN
  // (pas de réallocation inutile du repli, pas d'oscillation).
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomFallbackReserveBytes,
                                           kOomRearmMinMaxAllocBytes - 1u));
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomFallbackReserveBytes,
                                           kOomFallbackRearmMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomFallbackReserveBytes, 0u));
}

void test_rearm_never_downgrades_nominal_to_fallback(void) {
  // E2 — nominal armé : JAMAIS de déclassement, quel que soit l'état du tas
  // (aucune valeur de maxBloc ne doit rendre kOomFallbackReserveBytes).
  const uint32_t samples[] = {0u, 1u, kOomFallbackRearmMinMaxAllocBytes - 1u,
                              kOomFallbackRearmMinMaxAllocBytes,
                              kOomRearmMinMaxAllocBytes - 1u,
                              kOomRearmMinMaxAllocBytes, 27584u, 29684u,
                              100000u, 0xFFFFFFFFu};
  for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
    TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomReserveBytes, samples[i]));
  }
  // Réserve plus GROSSE que le nominal (théorique) : idem, rien à faire.
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomReserveBytes + 1u, 0xFFFFFFFFu));
}

void test_rearm_no_double_allocation_when_already_nominal(void) {
  // E2 — pas de double allocation : réserve nominale présente → 0, y compris au
  // seuil exact et avec un tas immense (le pendant de
  // test_rearm_never_when_reserve_present pour la fonction à deux paliers).
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomReserveBytes,
                                           kOomRearmMinMaxAllocBytes));
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomReserveBytes, 0xFFFFFFFFu));
}

void test_rearm_two_tier_full_scenario_after_famine(void) {
  // E2 — scénario complet vécu le 13/08, rejoué avec le correctif :
  // famine → réserve consommée (armedBytes=0) → tas très dégradé → repli →
  // tas revenu au régime réel (27584 o) → surclassement nominal → stable.
  uint32_t armed = (uint32_t)kOomReserveBytes;
  armed = 0u;  // le handler a consommé la réserve (pointeur nul = seule autorité)

  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, 9000u));       // tas à sec : rien
  uint32_t t = rearmTarget(armed, 14000u);                        // tas partiel : repli
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes, t);
  armed = t;
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, 14000u));       // stable, pas d'oscillation
  t = rearmTarget(armed, 27584u);                                 // régime réel : surclassement
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes, t);
  armed = t;
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, 27584u));       // stable
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, 9000u));        // pas de déclassement
}

// ===========================================================================
// 8. Post-mortem RTC — encodage / décodage (32 octets little-endian)
// ===========================================================================

static OomPostMortemFields makeFields(void) {
  OomPostMortemFields f;
  f.magic = kOomPostMortemMagic;
  f.failCount = 2;
  f.firstSize = 156;
  f.lastSize = 100;
  f.lastCaps = 0x00000404u;
  f.firstUptimeS = 46400;
  f.lastUptimeS = 46401;
  f.ringFreeBytes = 81408;
  f.ringMaxAllocBytes = 27584;
  f.ringUptimeS = 46399;
  f.flags = kOomPmFlagNominalUsed | kOomPmFlagHandlerUnregistered;
  f.releaseCount = 1;
  return f;
}

void test_postmortem_roundtrip_nominal(void) {
  // E3 — aller-retour fidèle sur le cas réel du 13/08 (les deux champs d'anneau
  // sont des multiples exacts de leur quantum : aucune perte attendue ici).
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields in = makeFields();
  oomPostMortemEncode(raw, in);
  OomPostMortemFields out = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(in.magic, out.magic);
  TEST_ASSERT_EQUAL_UINT32(in.failCount, out.failCount);
  TEST_ASSERT_EQUAL_UINT32(in.firstSize, out.firstSize);
  TEST_ASSERT_EQUAL_UINT32(in.lastSize, out.lastSize);
  TEST_ASSERT_EQUAL_UINT32(in.lastCaps, out.lastCaps);
  TEST_ASSERT_EQUAL_UINT16(in.firstUptimeS, out.firstUptimeS);
  TEST_ASSERT_EQUAL_UINT16(in.lastUptimeS, out.lastUptimeS);
  TEST_ASSERT_EQUAL_UINT32(81408u, out.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(27584u, out.ringMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT16(in.ringUptimeS, out.ringUptimeS);
  TEST_ASSERT_EQUAL_UINT8(in.flags, out.flags);
  TEST_ASSERT_EQUAL_UINT8(in.releaseCount, out.releaseCount);
  TEST_ASSERT_TRUE(oomPostMortemValid(out));
}

void test_postmortem_wire_format_is_little_endian_32_bytes(void) {
  // E3 — le format est un CONTRAT (la zone est écrite par mots u32 bruts depuis
  // le chemin d'échec, cf. oom_guard.cpp) : offsets et boutisme verrouillés.
  TEST_ASSERT_EQUAL_UINT32(32u, kOomPostMortemBytes);
  // v2.19.0 : magic bumpé (0xF00DFA11 → 0xF00DFA12) — bascule de la métrique
  // « ring » vers la mesure 8 bits, une zone 2.18.x ne doit plus être décodée.
  TEST_ASSERT_EQUAL_HEX32(0xF00DFA12u, kOomPostMortemMagic);
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();
  f.lastCaps = 0x11223344u;
  oomPostMortemEncode(raw, f);
  TEST_ASSERT_EQUAL_HEX8(0x12, raw[0]);  // magic LE : 12 FA 0D F0
  TEST_ASSERT_EQUAL_HEX8(0xFA, raw[1]);
  TEST_ASSERT_EQUAL_HEX8(0x0D, raw[2]);
  TEST_ASSERT_EQUAL_HEX8(0xF0, raw[3]);
  TEST_ASSERT_EQUAL_HEX8(0x44, raw[16]);  // lastCaps LE
  TEST_ASSERT_EQUAL_HEX8(0x11, raw[19]);
  TEST_ASSERT_EQUAL_UINT8(f.flags, raw[30]);
  TEST_ASSERT_EQUAL_UINT8(f.releaseCount, raw[31]);
  // 81408 >> 8 = 318 ; 27584 >> 6 = 431 (valeurs écrites, pas les octets bruts)
  TEST_ASSERT_EQUAL_UINT16(318u, (uint16_t)(raw[24] | (raw[25] << 8)));
  TEST_ASSERT_EQUAL_UINT16(431u, (uint16_t)(raw[26] | (raw[27] << 8)));
}

void test_postmortem_roundtrip_zero_and_extreme_sizes(void) {
  // E3 — valeurs extrêmes : tout à zéro, puis tailles maximales u32.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields z = makeFields();
  z.failCount = 0; z.firstSize = 0; z.lastSize = 0; z.lastCaps = 0;
  z.firstUptimeS = 0; z.lastUptimeS = 0; z.ringFreeBytes = 0;
  z.ringMaxAllocBytes = 0; z.ringUptimeS = 0; z.flags = 0; z.releaseCount = 0;
  oomPostMortemEncode(raw, z);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(kOomPostMortemMagic, o.magic);
  TEST_ASSERT_EQUAL_UINT32(0u, o.failCount);
  TEST_ASSERT_EQUAL_UINT32(0u, o.lastSize);
  TEST_ASSERT_EQUAL_UINT32(0u, o.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(0u, o.ringMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT8(0u, o.releaseCount);

  OomPostMortemFields big = makeFields();
  big.firstSize = 0xFFFFFFFFu;
  big.lastSize = kOomPmMaxSizeBytes;  // 4 Mio pile
  big.lastCaps = 0xFFFFFFFFu;
  big.failCount = 0xFFFFFFFFu;
  big.releaseCount = 255;
  big.flags = 0xFF;
  oomPostMortemEncode(raw, big);
  o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, o.firstSize);   // u32 pleins : aucun écrêtage
  TEST_ASSERT_EQUAL_UINT32(kOomPmMaxSizeBytes, o.lastSize);
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, o.lastCaps);
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, o.failCount);
  TEST_ASSERT_EQUAL_UINT8(255u, o.releaseCount);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, o.flags);
}

void test_postmortem_uptime_wraps_modulo_16_bits(void) {
  // E3 — les uptimes sont stockés sur 16 bits (troncature modulo 65536 assumée,
  // documentée dans le format). Au-delà de 18 h 12 min, l'uptime « repart ».
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();
  f.firstUptimeS = (uint16_t)(70000u & 0xFFFFu);   // 70000 s → 4464
  f.lastUptimeS = (uint16_t)(70050u & 0xFFFFu);    // 70050 s → 4514
  f.ringUptimeS = (uint16_t)(70048u & 0xFFFFu);
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT16(4464u, o.firstUptimeS);
  TEST_ASSERT_EQUAL_UINT16(4514u, o.lastUptimeS);
  TEST_ASSERT_EQUAL_UINT16(4512u, o.ringUptimeS);
  // L'âge de l'échantillon d'anneau (last - ring) reste exploitable après wrap.
  TEST_ASSERT_EQUAL_UINT16(2u, (uint16_t)(o.lastUptimeS - o.ringUptimeS));
  // Valeur maximale représentable
  f.firstUptimeS = 65535u; f.lastUptimeS = 65535u;
  oomPostMortemEncode(raw, f);
  o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT16(65535u, o.lastUptimeS);
}

void test_postmortem_ring_quantization_is_bounded(void) {
  // E3 — perte de précision ATTENDUE et BORNÉE : < 256 o sur ringFree (>>8),
  // < 64 o sur ringMaxAlloc (>>6), toujours par défaut (jamais d'optimisme :
  // la valeur relue ne SURESTIME jamais le tas réel).
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();
  f.ringFreeBytes = 81599;      // 318,7 quanta → 318 → 81408 (perte 191 o)
  f.ringMaxAllocBytes = 27647;  // 431,9 quanta → 431 → 27584 (perte 63 o)
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(81408u, o.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(27584u, o.ringMaxAllocBytes);
  TEST_ASSERT_TRUE(o.ringFreeBytes <= f.ringFreeBytes);
  TEST_ASSERT_TRUE(f.ringFreeBytes - o.ringFreeBytes < 256u);
  TEST_ASSERT_TRUE(o.ringMaxAllocBytes <= f.ringMaxAllocBytes);
  TEST_ASSERT_TRUE(f.ringMaxAllocBytes - o.ringMaxAllocBytes < 64u);
}

void test_postmortem_ring_saturates_instead_of_wrapping(void) {
  // E3 — au-delà de la dynamique 16 bits, SATURATION (jamais de repliement, qui
  // afficherait un tas minuscule pour un tas énorme et fausserait le verdict).
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();
  f.ringFreeBytes = 0xFFFFFFFFu;
  f.ringMaxAllocBytes = 0xFFFFFFFFu;
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 8, o.ringFreeBytes);      // 16 776 960 o
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 6, o.ringMaxAllocBytes);  // 4 194 240 o
  // Juste sous la saturation : valeur exacte conservée
  f.ringFreeBytes = 0xFFFFu << 8;
  f.ringMaxAllocBytes = 0xFFFFu << 6;
  oomPostMortemEncode(raw, f);
  o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 8, o.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 6, o.ringMaxAllocBytes);
}

// ===========================================================================
// 9. Post-mortem RTC — validité / bornes de plausibilité
// ===========================================================================
// La RAM RTC n'est PAS initialisée au power-on : au 1er boot elle contient des
// octets arbitraires. Un décodage plausible ne doit JAMAIS produire un dump
// mensonger au démarrage.

void test_postmortem_invalid_without_magic(void) {
  OomPostMortemFields f = makeFields();
  f.magic = 0;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f.magic = kOomPostMortemMagic - 1u;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f.magic = 0xB10C4EA9u;  // magic de l'anneau heap : zones distinctes
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f.magic = 0xA5C0FFEEu;  // magic de l'auto-guérison
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f.magic = kOomPostMortemMagic;
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
}

void test_postmortem_invalid_on_random_rtc_garbage(void) {
  // E4 — zone remplie de 0xFF (RAM RTC vierge/corrompue) : refus par le magic.
  uint8_t raw[kOomPostMortemBytes];
  for (unsigned i = 0; i < kOomPostMortemBytes; i++) raw[i] = 0xFF;
  TEST_ASSERT_FALSE(oomPostMortemValid(oomPostMortemDecode(raw)));
  for (unsigned i = 0; i < kOomPostMortemBytes; i++) raw[i] = 0x00;
  TEST_ASSERT_FALSE(oomPostMortemValid(oomPostMortemDecode(raw)));
}

void test_postmortem_invalid_on_insane_sizes(void) {
  // E4 — magic présent mais tailles insensées (> 4 Mio, soit plus que toute la
  // RAM adressable en tas de l'ESP32) → zone rejetée.
  OomPostMortemFields f = makeFields();
  f.firstSize = kOomPmMaxSizeBytes + 1u;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f = makeFields();
  f.lastSize = kOomPmMaxSizeBytes + 1u;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f = makeFields();
  f.lastSize = 0xFFFFFFFFu;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  // Bornes EXACTES : 4 Mio pile est encore accepté (<=, inclusif)
  f = makeFields();
  f.firstSize = kOomPmMaxSizeBytes;
  f.lastSize = kOomPmMaxSizeBytes;
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
}

void test_postmortem_invalid_on_insane_failcount(void) {
  // E4 — compteur d'échecs délirant → zone rejetée ; 1e6 pile accepté.
  OomPostMortemFields f = makeFields();
  f.failCount = kOomPmMaxFailCount + 1u;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f.failCount = 0xFFFFFFFFu;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  f.failCount = kOomPmMaxFailCount;
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
}

void test_postmortem_valid_across_uptime_wrap(void) {
  // E4 / revue v2.18.8 — les uptimes sont tronqués sur 16 bits (repliement toutes
  // les 18 h 12) et l'appareil tourne des JOURS : un dernier échec « antérieur »
  // au premier est le cas NORMAL après repliement. La zone doit rester VALIDE
  // (l'ancien critère la rejetait → post-mortem jamais émis, défaut bloquant).
  OomPostMortemFields f = makeFields();
  f.firstUptimeS = 65000;   // avant le repliement
  f.lastUptimeS = 100;      // après : 65000 -> 65535 -> 0 -> 100
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
  // Le span modulo reste juste : 65535 - 65000 + 100 + 1 = 636 s.
  TEST_ASSERT_EQUAL_UINT16(636, oomPostMortemSpanS(f));
  // Égalité (deux échecs dans la même seconde) : span nul.
  f.firstUptimeS = 5000;
  f.lastUptimeS = 5000;
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
  TEST_ASSERT_EQUAL_UINT16(0, oomPostMortemSpanS(f));
}

void test_postmortem_valid_with_zero_failcount(void) {
  // E4 — zone initialisée mais AUCUN échec (drapeaux/anneau seuls, cas du
  // handler ayant consommé la réserve sans échec heap_caps compté) : la zone est
  // VALIDE (elle sera dumpée), mais le verdict reste indéterminé (cf. section 10).
  OomPostMortemFields f = makeFields();
  f.failCount = 0;
  f.firstSize = 0;
  f.lastSize = 0;
  f.firstUptimeS = 0;
  f.lastUptimeS = 0;
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined,
                          oomPostMortemVerdict(f, kOomCorruptionMaxFails,
                                               kOomCorruptionSaneBlockRatio,
                                               kOomCorruptionBurstSpanS));
}

// ===========================================================================
// 10. Post-mortem RTC — verdict famine / corruption / indéterminé
// ===========================================================================

static uint8_t verdict(const OomPostMortemFields& f) {
  return oomPostMortemVerdict(f, kOomCorruptionMaxFails, kOomCorruptionSaneBlockRatio,
                              kOomCorruptionBurstSpanS);
}

void test_verdict_many_failures_spread_over_hours_is_not_starvation(void) {
  // E5 / revue v2.18.8 (défaut B3) — le compteur d'échecs est CUMULÉ sur tout le
  // boot. Sans fenêtre temporelle, quelques échecs bénins étalés sur des heures
  // concluaient « FAMINE MÉMOIRE » sur le scénario même que ce verdict doit
  // trancher. Ici : beaucoup d'échecs MAIS étalés sur ~3 h, avec une comptabilité
  // saine → ce n'est PAS une famine.
  OomPostMortemFields f = makeFields();
  f.failCount = kOomCorruptionMaxFails + 20u;   // largement au-dessus du seuil
  f.firstUptimeS = 1000;
  f.lastUptimeS = 1000 + 10800;                 // 3 h plus tard
  TEST_ASSERT_TRUE(oomPostMortemSpanS(f) > kOomCorruptionBurstSpanS);
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
}

void test_verdict_many_failures_in_burst_is_starvation(void) {
  // E5 — mêmes échecs, mais RESSERRÉS : une vraie famine refuse tout en quelques
  // secondes. Span sous la fenêtre → famine, quelle que soit la comptabilité.
  OomPostMortemFields f = makeFields();
  f.failCount = kOomCorruptionMaxFails + 20u;
  f.firstUptimeS = 1000;
  f.lastUptimeS = 1000 + 5;                     // rafale de 5 s
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_burst_span_boundary_is_inclusive(void) {
  // E5 — frontière exacte de la fenêtre de rafale (inclusive : <=).
  OomPostMortemFields f = makeFields();
  f.failCount = kOomCorruptionMaxFails + 1u;
  f.firstUptimeS = 100;
  f.lastUptimeS = (uint16_t)(100 + kOomCorruptionBurstSpanS);       // pile la fenêtre
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
  f.lastUptimeS = (uint16_t)(100 + kOomCorruptionBurstSpanS + 1);   // juste au-delà
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
}

void test_verdict_real_case_20260813_is_corruption_suspected(void) {
  // E5 — cas RÉEL du 13/08/2026 : 2 échecs seulement (156 o puis 100 o pour
  // __cxa_allocate_exception), anneau annonçant free=81408 o et maxB=27584 o
  // (soit 275× la taille refusée). Une famine capable de refuser 100 o refuserait
  // TOUT : la comptabilité saine désigne une corruption du tas.
  OomPostMortemFields f = makeFields();
  TEST_ASSERT_EQUAL_UINT32(2u, f.failCount);
  TEST_ASSERT_EQUAL_UINT32(156u, f.firstSize);
  TEST_ASSERT_EQUAL_UINT32(100u, f.lastSize);
  TEST_ASSERT_EQUAL_UINT32(81408u, f.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(27584u, f.ringMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
  // Et le verdict survit à l'aller-retour RTC (c'est le chemin réel du dump).
  uint8_t raw[kOomPostMortemBytes];
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemValid(o));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(o));
}

void test_verdict_real_starvation_many_failures(void) {
  // E5 — famine franche : échecs en série et anneau effondré.
  OomPostMortemFields f = makeFields();
  f.failCount = 137;
  f.firstSize = 356;
  f.lastSize = 356;
  f.ringFreeBytes = 4096;
  f.ringMaxAllocBytes = 1024;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_starvation_when_accounting_collapsed(void) {
  // E5 — peu d'échecs MAIS comptabilité écroulée (plus gros bloc du même ordre
  // que la taille demandée) → famine, pas corruption.
  OomPostMortemFields f = makeFields();
  f.failCount = 1;
  f.lastSize = 8192;
  f.ringFreeBytes = 20000;
  f.ringMaxAllocBytes = 16384;  // 2 × seulement (< ratio 4)
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_sane_block_ratio_boundary(void) {
  // E5 — frontière du critère de comptabilité saine : maxBloc >= 4 × taille
  OomPostMortemFields f = makeFields();
  f.failCount = 1;
  f.lastSize = 1000;
  f.ringMaxAllocBytes = 4000;  // exactement 4 × → SAINE (inclusif)
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
  f.ringMaxAllocBytes = 3999;  // un octet sous → famine
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_failcount_threshold_boundary(void) {
  // E5 — frontière du nombre d'échecs. ATTENTION : le code applique
  // `failCount > maxFails` (STRICT), donc 8 échecs = encore analysable,
  // 9 échecs = famine. Ce test documente l'inclusivité réelle.
  OomPostMortemFields f = makeFields();
  f.failCount = kOomCorruptionMaxFails;      // 8
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
  f.failCount = kOomCorruptionMaxFails + 1u;  // 9
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
  // Au-delà du seuil, la famine l'emporte MÊME avec une comptabilité saine.
  f.ringMaxAllocBytes = 200000;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_undetermined_cases(void) {
  // E5 — cas ambigus : rien à analyser.
  OomPostMortemFields f = makeFields();
  f.failCount = 0;                       // aucun échec compté
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  f = makeFields();
  f.lastSize = 0;                        // taille demandée inconnue
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  f = makeFields();
  f.ringFreeBytes = 0;                   // aucun échantillon d'anneau (boot précoce)
  f.ringMaxAllocBytes = 0;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  // Un seul des deux champs d'anneau non nul suffit pour trancher.
  f.ringFreeBytes = 81408;
  TEST_ASSERT_NOT_EQUAL(kOomVerdictUndetermined, verdict(f));
}

void test_verdict_no_overflow_on_giant_last_size(void) {
  // E5 — robustesse : taille demandée géante (zone partiellement corrompue mais
  // dans les bornes) — la comparaison se fait par DIVISION, aucun débordement,
  // et le verdict reste famine (comptabilité incohérente avec la demande).
  OomPostMortemFields f = makeFields();
  f.failCount = 1;
  f.lastSize = kOomPmMaxSizeBytes;
  f.ringMaxAllocBytes = 27584;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
  // Ratio 0 (désactivation du critère par l'appelant) : comptabilité réputée
  // saine → jamais de division par zéro.
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected,
                          oomPostMortemVerdict(f, kOomCorruptionMaxFails, 0, kOomCorruptionBurstSpanS));
}

void test_verdict_real_constant_values(void) {
  TEST_ASSERT_EQUAL_UINT32(8u, kOomCorruptionMaxFails);
  TEST_ASSERT_EQUAL_UINT32(4u, kOomCorruptionSaneBlockRatio);
  TEST_ASSERT_EQUAL_UINT32(4194304u, kOomPmMaxSizeBytes);
  TEST_ASSERT_EQUAL_UINT32(1000000u, kOomPmMaxFailCount);
}

// ===========================================================================
// 11. Contrôle d'intégrité du tas — cadence + autolimitation (C3)
// ===========================================================================

void test_integrity_runs_immediately_on_first_call(void) {
  // E6 — premier passage dès que possible (everRun faux), indépendamment de la
  // date du dernier passage.
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(true, false, 0u, 0u,
                                          (uint32_t)kHeapIntegrityCheckIntervalMs));
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(true, false, 1u, 0u,
                                          (uint32_t)kHeapIntegrityCheckIntervalMs));
}

void test_integrity_respects_interval(void) {
  // E6 — cadence 5 min : rien avant, passage à l'échéance EXACTE (>= inclusif).
  const uint32_t itv = (uint32_t)kHeapIntegrityCheckIntervalMs;
  TEST_ASSERT_FALSE(heapIntegrityShouldRun(true, true, 1000u, 1000u, itv));
  TEST_ASSERT_FALSE(heapIntegrityShouldRun(true, true, 1000u + itv - 1u, 1000u, itv));
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(true, true, 1000u + itv, 1000u, itv));
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(true, true, 1000u + itv * 3u, 1000u, itv));
}

void test_integrity_interval_survives_millis_wraparound(void) {
  // E6 — millis() repasse par 0 après ~49,7 jours : l'arithmétique non signée
  // doit donner un écart CORRECT (356 ms ici), pas ~4,29 milliards.
  const uint32_t itv = (uint32_t)kHeapIntegrityCheckIntervalMs;
  TEST_ASSERT_FALSE(heapIntegrityShouldRun(true, true, 100u, 0xFFFFFF00u, itv));
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(true, true, itv, 0xFFFFFF00u, itv));
}

void test_integrity_disabled_never_runs(void) {
  // E6 — désactivation définitive : plus AUCUN passage, même au premier appel
  // ou après une éternité.
  const uint32_t itv = (uint32_t)kHeapIntegrityCheckIntervalMs;
  TEST_ASSERT_FALSE(heapIntegrityShouldRun(false, false, 0u, 0u, itv));
  TEST_ASSERT_FALSE(heapIntegrityShouldRun(false, true, 0xFFFFFFFFu, 0u, itv));
}

void test_integrity_admits_next_run_below_ceiling(void) {
  // E6 — durée sous le plafond → contrôle conservé. Le seuil de WARN (5 ms) ne
  // désactive PAS : il ne fait qu'alerter.
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(0u, kHeapIntegrityMaxDurationUs));
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(kHeapIntegrityWarnDurationUs,
                                             kHeapIntegrityMaxDurationUs));
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(kHeapIntegrityWarnDurationUs + 1u,
                                             kHeapIntegrityMaxDurationUs));
  // Plafond EXACT : encore admis (<=, inclusif)
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(kHeapIntegrityMaxDurationUs,
                                             kHeapIntegrityMaxDurationUs));
}

void test_integrity_denies_next_run_above_ceiling(void) {
  // E6 — au-delà de 20 ms, le parcours devient une source de gigue pour le
  // watchdog et la régulation → refus.
  TEST_ASSERT_FALSE(heapIntegrityAdmitNextRun(kHeapIntegrityMaxDurationUs + 1u,
                                              kHeapIntegrityMaxDurationUs));
  TEST_ASSERT_FALSE(heapIntegrityAdmitNextRun(500000u, kHeapIntegrityMaxDurationUs));
}

void test_integrity_disable_is_permanent_across_ticks(void) {
  // E6 — scénario complet : 2 passages sains, un 3e trop long → désactivation
  // DÉFINITIVE (aucun passage ultérieur, même très longtemps après).
  const uint32_t itv = (uint32_t)kHeapIntegrityCheckIntervalMs;
  bool enabled = true, everRun = false;
  uint32_t lastRun = 0, now = 0;

  TEST_ASSERT_TRUE(heapIntegrityShouldRun(enabled, everRun, now, lastRun, itv));
  everRun = true; lastRun = now;
  enabled = heapIntegrityAdmitNextRun(1500u, kHeapIntegrityMaxDurationUs);  // 1,5 ms
  TEST_ASSERT_TRUE(enabled);

  now += itv;
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(enabled, everRun, now, lastRun, itv));
  lastRun = now;
  enabled = heapIntegrityAdmitNextRun(6000u, kHeapIntegrityMaxDurationUs);  // WARN, conservé
  TEST_ASSERT_TRUE(enabled);

  now += itv;
  TEST_ASSERT_TRUE(heapIntegrityShouldRun(enabled, everRun, now, lastRun, itv));
  lastRun = now;
  enabled = heapIntegrityAdmitNextRun(25000u, kHeapIntegrityMaxDurationUs);  // > plafond
  TEST_ASSERT_FALSE(enabled);

  // ...et plus jamais de passage, quelle que soit l'attente
  for (int i = 0; i < 5; i++) {
    now += itv * 10u;
    TEST_ASSERT_FALSE(heapIntegrityShouldRun(enabled, everRun, now, lastRun, itv));
  }
}

void test_integrity_real_constant_values(void) {
  TEST_ASSERT_EQUAL_UINT32(300000u, (uint32_t)kHeapIntegrityCheckIntervalMs);
  TEST_ASSERT_EQUAL_UINT32(20000u, kHeapIntegrityMaxDurationUs);
  TEST_ASSERT_EQUAL_UINT32(5000u, kHeapIntegrityWarnDurationUs);
  // Le seuil de WARN doit rester STRICTEMENT sous le plafond de désactivation,
  // sinon aucun WARN ne précéderait jamais la désactivation.
  TEST_ASSERT_TRUE(kHeapIntegrityWarnDurationUs < kHeapIntegrityMaxDurationUs);
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
  // v2.18.8 — réarmement à deux paliers
  RUN_TEST(test_rearm_real_constant_values_v2188);
  RUN_TEST(test_rearm_field_maxblock_27584_at_12h9_arms_nominal);
  RUN_TEST(test_rearm_field_maxblock_29684_at_33min_arms_nominal);
  RUN_TEST(test_rearm_nothing_when_heap_below_fallback_threshold);
  RUN_TEST(test_rearm_fallback_tier_between_thresholds);
  RUN_TEST(test_rearm_nominal_tier_at_and_above_threshold);
  RUN_TEST(test_rearm_upgrade_fallback_to_nominal_allowed);
  RUN_TEST(test_rearm_never_downgrades_nominal_to_fallback);
  RUN_TEST(test_rearm_no_double_allocation_when_already_nominal);
  RUN_TEST(test_rearm_two_tier_full_scenario_after_famine);
  // v2.18.8 — post-mortem RTC : encodage/décodage
  RUN_TEST(test_postmortem_roundtrip_nominal);
  RUN_TEST(test_postmortem_wire_format_is_little_endian_32_bytes);
  RUN_TEST(test_postmortem_roundtrip_zero_and_extreme_sizes);
  RUN_TEST(test_postmortem_uptime_wraps_modulo_16_bits);
  RUN_TEST(test_postmortem_ring_quantization_is_bounded);
  RUN_TEST(test_postmortem_ring_saturates_instead_of_wrapping);
  // v2.18.8 — post-mortem RTC : validité / bornes
  RUN_TEST(test_postmortem_invalid_without_magic);
  RUN_TEST(test_postmortem_invalid_on_random_rtc_garbage);
  RUN_TEST(test_postmortem_invalid_on_insane_sizes);
  RUN_TEST(test_postmortem_invalid_on_insane_failcount);
  RUN_TEST(test_postmortem_valid_across_uptime_wrap);
  RUN_TEST(test_postmortem_valid_with_zero_failcount);
  // v2.18.8 — verdict d'attribution
  RUN_TEST(test_verdict_many_failures_spread_over_hours_is_not_starvation);
  RUN_TEST(test_verdict_many_failures_in_burst_is_starvation);
  RUN_TEST(test_verdict_burst_span_boundary_is_inclusive);
  RUN_TEST(test_verdict_real_case_20260813_is_corruption_suspected);
  RUN_TEST(test_verdict_real_starvation_many_failures);
  RUN_TEST(test_verdict_starvation_when_accounting_collapsed);
  RUN_TEST(test_verdict_sane_block_ratio_boundary);
  RUN_TEST(test_verdict_failcount_threshold_boundary);
  RUN_TEST(test_verdict_undetermined_cases);
  RUN_TEST(test_verdict_no_overflow_on_giant_last_size);
  RUN_TEST(test_verdict_real_constant_values);
  // v2.18.8 — contrôle d'intégrité du tas
  RUN_TEST(test_integrity_runs_immediately_on_first_call);
  RUN_TEST(test_integrity_respects_interval);
  RUN_TEST(test_integrity_interval_survives_millis_wraparound);
  RUN_TEST(test_integrity_disabled_never_runs);
  RUN_TEST(test_integrity_admits_next_run_below_ceiling);
  RUN_TEST(test_integrity_denies_next_run_above_ceiling);
  RUN_TEST(test_integrity_disable_is_permanent_across_ticks);
  RUN_TEST(test_integrity_real_constant_values);
  return UNITY_END();
}
