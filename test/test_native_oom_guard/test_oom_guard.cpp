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
//     (>= : à maxBloc == 18432 exactement, on réarme — bug-080 v2.33.0) ;
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
// v2.20.2 (bug-famine-8bits) — trois fonctions pures ajoutées, plus le
// recalibrage du palier de repli :
//   - oomShouldRegisterHandler : filet d'urgence du new_handler, avec la
//     condition C1 du gate pool-chemistry (réserve absente → PAS de
//     ré-inscription) et l'idempotence (pas d'INFO à chaque tick) ;
//   - oomFailWarnNextDelayMs : backoff 30 s → 5 min → 30 min du WARN d'échec
//     (restauration de la fenêtre forensique du journal persistant) ;
//   - oomShouldCountFailure : exclusion des échecs AUTO-INFLIGÉS par le malloc
//     de réarmement (~100 % des 1025 échecs comptés le 15/08) ;
//   - oomRearmTargetBytes : les seuils du palier de repli sont désormais
//     exprimés dans la métrique 8 BITS (ADR-0038) — les assertions de calibrage
//     de v2.18.8 ont été mises à jour et adossées aux invariants du gate.
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
// 3. oomShouldRearm — hystérésis (seuil kOomRearmMinMaxAllocBytes = 18432,
//    bug-080 v2.33.0 ; anciennement 24576 sur la métrique brute)
// ---------------------------------------------------------------------------
// Choix documenté : le seuil est INCLUSIF (>=) — à maxBloc == 18432 exactement
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
  // Limite exacte : 18432 → réarme (>= inclusif)
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
  // Amendé par gate pool-chemistry 02/09/2026, bug-080 (v2.33.0) : 24576 →
  // 18432 = 12288 (kOomReserveBytes) + 4096 (kRestartMaxAllocHeapBytes) + 2048
  // (kOomFallbackRearmMinMaxAllocBytes) — un réarmement à 16384 laisserait un
  // reliquat < seuil d'auto-heal 4096 : le parachute sèmerait son propre
  // auto-heal (le gate a REFUSÉ 16384). L'invariant arithmétique correspondant
  // est verrouillé dans test_native_self_heal
  // (test_rearm_arithmetic_never_triggers_autoheal_bug080).
  TEST_ASSERT_EQUAL_UINT32(18432u, kOomRearmMinMaxAllocBytes);
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
// 18432 exactement on arme le NOMINAL (bug-080 v2.33.0 ; 24576 avant), à
// maxBloc == 2048 exactement on arme le REPLI (v2.20.2 ; 12288 avant).
// Retour 0 = NE RIEN FAIRE.

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
  // Amendé par gate pool-chemistry 02/09/2026, bug-080 (v2.33.0) : 24576 →
  // 18432 = 12288 + 4096 + 2048 (réserve nominale + palier auto-heal + seuil de
  // repli) — un réarmement à 16384 laisserait un reliquat < seuil d'auto-heal
  // 4096 : le parachute déclencherait lui-même le restart d'auto-guérison.
  TEST_ASSERT_EQUAL_UINT32(18432u, kOomRearmMinMaxAllocBytes);
  // RECALIBRAGE v2.20.2 (bug-famine-8bits, bloc E1) : les DEUX valeurs du palier
  // de repli ont changé — 2048 → 1024 (taille) et 12288 → 2048 (seuil). Les
  // valeurs de v2.18.8 verrouillées ici étaient calées sur ESP.getMaxAllocHeap(),
  // dont ADR-0038 a prouvé qu'elle compte l'IRAM 32 bits inutilisable par
  // malloc/new (figée à 27 636 o) ; le tick de réarmement lit désormais
  // heapLargestFreeBlock8Bit(), dont le régime réel mesuré sur 5 h vaut
  // maxBloc8 ∈ [4 084 ; 9 204]. Un seuil de repli à 12 288 dans CETTE métrique
  // n'aurait plus jamais été atteint : parachute définitivement désarmé.
  // Les assertions ci-dessous sont donc mises à jour DÉLIBÉRÉMENT (l'ancien
  // contrat de calibrage est caduc, ce n'est pas une régression de logique) ;
  // les invariants qui les justifient sont testés juste en dessous.
  TEST_ASSERT_EQUAL_UINT32(1024u, (uint32_t)kOomFallbackReserveBytes);
  TEST_ASSERT_EQUAL_UINT32(2048u, kOomFallbackRearmMinMaxAllocBytes);
  // Le palier de repli DOIT être strictement en dessous du nominal (sinon un
  // seul palier utile, et le surclassement n'a plus de sens).
  TEST_ASSERT_TRUE(kOomFallbackRearmMinMaxAllocBytes < kOomRearmMinMaxAllocBytes);
  TEST_ASSERT_TRUE((uint32_t)kOomFallbackReserveBytes < (uint32_t)kOomReserveBytes);
}

// -----------------------------------------------------------------------------
// v2.35.0 (bug-080, lot B-1) — arithmétique du seuil de bascule PSRAM
// -----------------------------------------------------------------------------
// CE QUE CE TEST APPORTE EN PLUS DES static_assert de constants.h : les deux
// static_assert verrouillent des RELATIONS (seuil + 1 == tampon, réserve >
// seuil) et tuent donc déjà, à la COMPILATION, la mutation « seuil 4095 → 4096 ».
// Ils sont en revanche indifférents à une TRANSLATION des deux constantes : poser
// kVfsStdioBufferBytes = 8192 et kPsramExtmemThresholdBytes = 8191 satisfait la
// relation, compile sans un mot, et pourtant change complètement le
// comportement — la classe « 4096 o » (le tampon stdio réel du VFS, mesuré dans
// vfs_api.cpp) repasserait EN RAM INTERNE et bug-080 reviendrait à l'identique,
// tandis que toute la plage 4097-8191 y basculerait au passage. Ce test fige donc
// les VALEURS ABSOLUES, seul moyen de refuser cette translation.
void test_psram_extmem_threshold_arithmetic_v2350(void) {
  // Le tampon stdio du VFS vaut EXACTEMENT 4096 o (miroir de
  // DEFAULT_FILE_BUFFER_SIZE, libraries/FS/src/vfs_api.cpp) : c'est une
  // constante d'un composant EXTERNE, recopiée ici. Si le core change ce
  // chiffre, l'échec de ce test est le signal attendu, pas un faux positif.
  TEST_ASSERT_EQUAL_UINT32(4096u, (uint32_t)kVfsStdioBufferBytes);
  // POURQUOI 4095 ET PAS 4096 : le test de heap_caps_malloc_default est
  // « taille <= seuil ⇒ RAM INTERNE » (inclusif, désassemblage de libheap.a).
  // À 4096, le tampon stdio retomberait du côté interne et le défaut d'origine
  // reviendrait à l'identique — il s'en faut d'UN OCTET, et c'est précisément
  // cet octet que le lot B-1 corrige.
  TEST_ASSERT_EQUAL_UINT32(4095u, (uint32_t)kPsramExtmemThresholdBytes);
  // La relation elle-même (redondante avec le static_assert, conservée pour que
  // l'invariant soit ÉNONCÉ dans la suite de tests et non seulement dans un
  // en-tête) : le seuil est le plus grand entier qui laisse encore le tampon
  // basculer, donc tampon - 1.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kVfsStdioBufferBytes,
                           (uint32_t)kPsramExtmemThresholdBytes + 1u);
  // Rien d'autre ne doit changer de camp : un seuil strictement inférieur
  // déplacerait des allocations plus petites que le tampon (correctif non
  // chirurgical, effet de bord non mesuré sur tout le reste du firmware).
  TEST_ASSERT_TRUE((uint32_t)kPsramExtmemThresholdBytes <
                   (uint32_t)kVfsStdioBufferBytes);

  // POURQUOI LA RÉSERVE DU PARACHUTE DOIT DÉPASSER LE SEUIL : c'est cette
  // relation, et elle seule, qui rend NÉCESSAIRE le MALLOC_CAP_INTERNAL explicite
  // de oomAllocReserveInternal(). Une réserve de 12 288 o allouée par un malloc()
  // ordinaire partirait désormais EN PSRAM — or libérer de la PSRAM pendant une
  // famine de RAM interne 8 bits ne rend pas un seul octet utilisable : le
  // parachute deviendrait décoratif. Si la réserve repassait SOUS le seuil, les
  // caps explicites ne seraient plus qu'une ceinture, et le #pragma GCC poison
  // malloc de oom_guard.cpp perdrait sa justification.
  TEST_ASSERT_TRUE((uint32_t)kOomReserveBytes > (uint32_t)kPsramExtmemThresholdBytes);
  // Le palier de REPLI (1 024 o), lui, est sous le seuil : il resterait interne
  // même sans caps. On le documente pour que personne ne conclue à tort que
  // « seul le nominal a besoin de l'entonnoir » — les trois sites d'allocation
  // passent par oomAllocReserveInternal() par UNIFORMITÉ, et parce qu'un futur
  // recalibrage du repli au-dessus de 4 095 ne doit pas rouvrir le défaut.
  TEST_ASSERT_TRUE((uint32_t)kOomFallbackReserveBytes <=
                   (uint32_t)kPsramExtmemThresholdBytes);
  // Délai avant CRITICAL + alerte MQTT `oom_reserve_absent` (condition C2 du
  // gate) : 5 min = 30 tentatives de réarmement à kOomRearmCheckIntervalMs.
  TEST_ASSERT_EQUAL_UINT32(300000u, (uint32_t)kOomNoReserveCriticalDelayMs);
  TEST_ASSERT_TRUE((uint32_t)kOomNoReserveCriticalDelayMs >
                   10u * (uint32_t)kOomRearmCheckIntervalMs);
}

// Mesures 8 bits ON-TARGET du 15/08/2026 (firmware 2.20.0 core 3, 5 h) — ce sont
// elles qui fondent le recalibrage, et non plus la métrique brute.
static const uint32_t kField8MinMaxBlocSinceBoot = 3060u;   // pire creux observé
static const uint32_t kField8MaxBlocLow          = 4084u;   // plus bas relevé en régime
static const uint32_t kField8MaxBlocTypical      = 4596u;   // valeur la plus fréquente
static const uint32_t kField8MaxBlocHigh         = 9204u;   // meilleur relevé

void test_rearm_fallback_tier_respects_chemistry_gate_invariants_v2202(void) {
  // Invariant C2 imposé par le gate pool-chemistry, verrouillé ici pour qu'aucun
  // recalibrage futur ne puisse le rompre en silence :
  //   (a) le seuil de réarmement du repli reste SOUS le pire creux observé depuis
  //       le boot → aucun trou de protection, le parachute reste armable même au
  //       plus mauvais moment mesuré ;
  TEST_ASSERT_TRUE(kOomFallbackRearmMinMaxAllocBytes < kField8MinMaxBlocSinceBoot);
  //   (b) la taille allouée tient dans la MOITIÉ du seuil → armer la réserve ne
  //       peut pas, à soi seul, faire retomber le tas sous son propre seuil de
  //       réarmement (pas d'auto-sabotage, pas d'oscillation) ;
  TEST_ASSERT_TRUE((uint32_t)kOomFallbackReserveBytes <=
                   kOomFallbackRearmMinMaxAllocBytes / 2u);
  //   (c) le seuil est atteignable en pratique dans le régime 8 bits mesuré.
  TEST_ASSERT_TRUE(kOomFallbackRearmMinMaxAllocBytes <= kField8MaxBlocLow);
  // La réserve de repli couvre les petites allocations vitales des trois crashs
  // (accept HTTP 356 o, AsyncFileResponse 156 o, exception C++ 100 o).
  TEST_ASSERT_TRUE((uint32_t)kOomFallbackReserveBytes >= 356u);
}

void test_rearm_arms_fallback_across_whole_measured_8bit_regime(void) {
  // Toute la plage 8 bits MESURÉE doit armer le parachute (aucun trou), du pire
  // creux au meilleur relevé — c'est ce que la calibration v2.18.8 ne faisait
  // plus une fois la mesure passée en 8 bits (12 288 > 9 204 = jamais armé).
  const uint32_t regime[] = {kField8MinMaxBlocSinceBoot, kField8MaxBlocLow,
                             kField8MaxBlocTypical, kField8MaxBlocHigh};
  for (unsigned i = 0; i < sizeof(regime) / sizeof(regime[0]); i++) {
    TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                             rearmTarget(0u, regime[i]));
    // ...et JAMAIS le palier nominal : c'est ce qui supprime le malloc(12288)
    // retenté toutes les 10 s, qui fabriquait ~100 % des 1025 échecs comptés.
    TEST_ASSERT_NOT_EQUAL_UINT32((uint32_t)kOomReserveBytes,
                                 rearmTarget(0u, regime[i]));
  }
}

void test_rearm_never_downgrades_nominal_in_8bit_regime(void) {
  // Non-régression de l'invariant historique avec les NOUVELLES valeurs : une
  // réserve nominale déjà armée n'est jamais déclassée vers le repli, y compris
  // aux creux 8 bits réels (qui sont désormais au-dessus du seuil de repli).
  const uint32_t regime[] = {0u, kOomFallbackRearmMinMaxAllocBytes,
                             kField8MinMaxBlocSinceBoot, kField8MaxBlocLow,
                             kField8MaxBlocTypical, kField8MaxBlocHigh};
  for (unsigned i = 0; i < sizeof(regime) / sizeof(regime[0]); i++) {
    TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget((uint32_t)kOomReserveBytes, regime[i]));
  }
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
  // E2 — maxBloc ∈ [kOomFallbackRearmMinMaxAllocBytes, kOomRearmMinMaxAllocBytes[
  // (= [2048, 18432[ depuis bug-080) → palier de REPLI
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                           rearmTarget(0u, kOomFallbackRearmMinMaxAllocBytes));  // borne INCLUSIVE
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                           rearmTarget(0u, kOomFallbackRearmMinMaxAllocBytes + 1u));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes,
                           rearmTarget(0u, kOomRearmMinMaxAllocBytes - 1u));  // juste sous le nominal
}

void test_rearm_nominal_tier_at_and_above_threshold(void) {
  // E2 — maxBloc >= 18432 (bug-080) → palier NOMINAL (12 Ko), borne INCLUSIVE
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

  // v2.20.2 : les valeurs de tas de ce scénario sont RÉÉCHELONNÉES. Elles étaient
  // exprimées en métrique brute (9 000 = « tas à sec », 14 000 = « tas partiel »),
  // or le tick lit désormais heapLargestFreeBlock8Bit() où 9 000 est au contraire
  // le MEILLEUR relevé de terrain (9 204). Avec le seuil de repli à 2 048, 9 000
  // arme donc le repli — ce n'est pas une régression mais le changement d'échelle
  // décidé au bloc E1. Les bornes sont désormais SYMBOLIQUES (dérivées des
  // constantes) ou tirées des mesures 8 bits réelles, pour ne plus se périmer.
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, kOomFallbackRearmMinMaxAllocBytes - 1u));  // tas à sec : rien
  uint32_t t = rearmTarget(armed, kField8MaxBlocTypical);          // régime 8 bits réel : repli
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes, t);
  armed = t;
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, kField8MaxBlocTypical));  // stable, pas d'oscillation
  t = rearmTarget(armed, 27584u);                                 // défragmentation : surclassement
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomReserveBytes, t);
  armed = t;
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, 27584u));       // stable
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, kField8MaxBlocTypical));  // pas de déclassement
  TEST_ASSERT_EQUAL_UINT32(0u, rearmTarget(armed, 0u));           // ni même tas à zéro
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

void test_postmortem_wire_format_is_little_endian_48_bytes(void) {
  // E3 — le format est un CONTRAT (la zone est écrite par mots u32 bruts depuis
  // le chemin d'échec, cf. oom_guard.cpp) : offsets et boutisme verrouillés.
  //
  // ATTENDU MIS À JOUR EN bug-080 B-2 (32 → 48 o). Ce que ce test prouvait ne
  // change pas — que la zone RTC a une taille et un boutisme FIGÉS, parce que
  // les chemins d'échec y font des stores BRUTS par index de mot et qu'une
  // divergence entre l'encodeur pur et ces stores ne produirait aucune erreur,
  // seulement des chiffres faux. C'est la zone qui a grandi de 4 mots (w8..w11,
  // la sonde du refus), pas le contrat.
  TEST_ASSERT_EQUAL_UINT32(48u, kOomPostMortemBytes);
  TEST_ASSERT_EQUAL_UINT32(0u, kOomPostMortemBytes % 4u);  // stores u32 alignés
  // Magic RE-bumpé (0xF00DFA12 → 0xF00DFA13) : cf. section 14bis, une zone de
  // 32 o écrite par l'ancien firmware ne doit JAMAIS être relue avec ce format.
  TEST_ASSERT_EQUAL_HEX32(0xF00DFA13u, kOomPostMortemMagic);
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();
  f.lastCaps = 0x11223344u;
  oomPostMortemEncode(raw, f);
  TEST_ASSERT_EQUAL_HEX8(0x13, raw[0]);  // magic LE : 13 FA 0D F0
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

// ⚠️ ATTENDUS RÉVISÉS PAR bug-080 B-2 — À LIRE AVANT DE TOUCHER À CETTE SECTION
// -----------------------------------------------------------------------------
// Six tests de cette section attendaient kOomVerdictCorruptionSuspected. Ils ne
// l'attendent plus, et ce n'est PAS un assouplissement : c'est la SUPPRESSION
// D'UN FAUX VERDICT, qui est l'objet même du lot.
//
// La règle historique (v2.18.8) concluait « corruption » par ÉLIMINATION : si ce
// n'est ni une rafale, ni une comptabilité écroulée, alors un bloc existait donc
// le tas ment. Cette conclusion reposait sur ringMaxAllocBytes, qui est
//   (a) PÉRIMÉ de 0 à 2 s (miroir de l'anneau, cadence 2 s), et
//   (b) mesuré sur des caps FIXES (INTERNAL|8BIT), donc muet sur le pool
//       réellement demandé.
// Le 05/09/2026 elle a produit « SUSPICION CORRUPTION » sur 6 refus de 1 622 o en
// caps=0x080C (8BIT|DMA|INTERNAL) alors que l'anneau annonçait 26 612 o : le tas
// 8 bits était sain, c'est le pool DMA qui était à sec. Un verdict de corruption
// sur du matériel non corrompu, chiffré, crédible, et faux.
//
// makeFields() ne pose AUCUN champ de sonde (probeSize/probeCaps à 0, drapeau
// kOomPmFlagProbeTaken absent) : tous les tests ci-dessous empruntent donc la
// RÈGLE 3 (sonde absente), désormais amputée de sa conclusion. Ce qu'ils
// prouvaient — la fenêtre de rafale, la frontière du ratio, la frontière du
// nombre d'échecs, l'absence de débordement — est INTACT : seule la branche
// « sinon » passe de CorruptionSuspected à Undetermined. Le chemin vers la
// corruption existe toujours, mais il exige maintenant une MESURE CONTEMPORAINE
// (section 16, cas c) — c'est tout le point.

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
  // CE QUE CE TEST PROUVE (inchangé) : hors fenêtre de rafale, le NOMBRE
  // d'échecs ne suffit jamais à conclure « famine ». Seule la conclusion de la
  // branche « sinon » change (B-2) : sans mesure contemporaine, on s'abstient au
  // lieu d'accuser le tas de mentir.
  TEST_ASSERT_NOT_EQUAL(kOomVerdictStarvation, verdict(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
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
  // La FRONTIÈRE est ce que ce test prouve, et elle est intacte : un octet de
  // temps en plus et la famine n'est plus retenue. C'est l'AUTRE CÔTÉ de la
  // frontière qui a changé de nom en B-2 (CorruptionSuspected → Undetermined),
  // parce qu'aucune donnée contemporaine ne permet d'aller plus loin.
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
}

void test_verdict_real_case_20260813_needs_a_probe_to_accuse(void) {
  // E5 — cas RÉEL du 13/08/2026 : 2 échecs seulement (156 o puis 100 o pour
  // __cxa_allocate_exception), anneau annonçant free=81408 o et maxB=27584 o
  // (soit 275× la taille refusée). Le raisonnement d'origine — « une famine
  // capable de refuser 100 o refuserait TOUT, donc le tas ment » — RESTE VALIDE.
  //
  // ATTENDU RÉVISÉ EN B-2, ET C'EST LE CŒUR DU LOT : ce raisonnement était
  // appliqué à une donnée qui n'y donnait pas droit. 27 584 o est un chiffre de
  // l'ANNEAU : vieux de 0 à 2 s, et mesuré sur INTERNAL|8BIT quelles que soient
  // les caps du refus. Le 05/09, exactement la même arithmétique a accusé de
  // corruption un appareil dont seul le pool DMA était à sec. On ne peut pas
  // garder la conclusion en 2026-08 et la refuser en 2026-09 : la donnée est de
  // même nature dans les deux cas. Sans sonde, donc, INDÉTERMINÉ.
  OomPostMortemFields f = makeFields();
  TEST_ASSERT_EQUAL_UINT32(2u, f.failCount);
  TEST_ASSERT_EQUAL_UINT32(156u, f.firstSize);
  TEST_ASSERT_EQUAL_UINT32(100u, f.lastSize);
  TEST_ASSERT_EQUAL_UINT32(81408u, f.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(27584u, f.ringMaxAllocBytes);
  TEST_ASSERT_FALSE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));

  // OBSERVATION QUE LA SONDE REND POSSIBLE, et qui échappait complètement à
  // l'ancienne règle : le masque RELEVÉ ce jour-là, 0x0404, est
  // MALLOC_CAP_8BIT (1<<2) | MALLOC_CAP_SPIRAM (1<<10). L'allocation demandait
  // donc de la mémoire EN PSRAM — sur un WROOM qui n'en a pas. Une sonde sur
  // cette classe bascule en parcours O(régions) (condition 2 du gate) et
  // rapporte un libre NUL : ce n'est pas une corruption, c'est un pool
  // INEXISTANT. Le verdict rendu est FAMINE, et il est exact.
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(f.lastCaps));
  f.flags |= kOomPmFlagProbeTaken;
  f.probeSize = f.lastSize;                       // 100 o
  f.probeCaps = (uint16_t)(f.lastCaps & 0xFFFFu); // 0x0404, mêmes caps
  f.probeCapsMaxB = kOomProbeBlockUnknown;        // O(régions) : bloc non renseigné
  f.probeCapsFree = 0u;                           // aucune PSRAM sur ce module
  f.probeRefMaxB = 27584u;                        // le tas INTERNE, lui, est intact
  f.probeRefFree = 81408u;
  f.probeDurationUs = 120u;
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));

  // ET le verdict de corruption n'est pas perdu pour autant : il est rendu au
  // même scénario dès lors que le refus porte sur une classe INTERNE et qu'un
  // bloc suffisant y existait AU MOMENT EXACT du refus. C'est là toute la
  // différence entre inférer et mesurer.
  f.lastCaps = 0x00000804u;                       // 8BIT|INTERNAL
  f.probeCaps = 0x0804u;
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(f.lastCaps));
  f.probeCapsMaxB = 27584u;                       // un bloc 275× la demande
  f.probeCapsFree = 81408u;
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));

  // Et le verdict survit à l'aller-retour RTC (c'est le chemin réel du dump) :
  // ce n'est pas la structure en mémoire qui est dumpée, mais 48 octets relus.
  uint8_t raw[kOomPostMortemBytes];
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemValid(o));
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(o));
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
  // E5 — frontière du critère de comptabilité saine : maxBloc >= 4 × taille.
  // C'EST CETTE FRONTIÈRE que le test prouve, et elle est inchangée : à 4 000
  // pour 1 000 demandés la comptabilité est réputée SAINE (donc pas de famine),
  // un octet en dessous elle est réputée ÉCROULÉE (donc famine). Seul le nom du
  // côté « saine » change en B-2 : sans mesure contemporaine, INDÉTERMINÉ.
  OomPostMortemFields f = makeFields();
  f.failCount = 1;
  f.lastSize = 1000;
  f.ringMaxAllocBytes = 4000;  // exactement 4 × → SAINE (inclusif)
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  f.ringMaxAllocBytes = 3999;  // un octet sous → famine
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_failcount_threshold_boundary(void) {
  // E5 — frontière du nombre d'échecs. ATTENTION : le code applique
  // `failCount > maxFails` (STRICT), donc 8 échecs = encore analysable,
  // 9 échecs = famine. Ce test documente l'inclusivité réelle — inchangée.
  // Côté « encore analysable », B-2 rend INDÉTERMINÉ au lieu d'accuser (la
  // makeFields() de référence ne porte pas de sonde).
  OomPostMortemFields f = makeFields();
  f.failCount = kOomCorruptionMaxFails;      // 8
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
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
  // saine → jamais de division par zéro. CE QUE CE TEST PROUVE (absence de
  // division par zéro et de débordement) est inchangé ; la branche « saine »
  // rend désormais INDÉTERMINÉ, comme partout ailleurs sans sonde.
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined,
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

// ===========================================================================
// 11. oomShouldRegisterHandler — FILET D'URGENCE du new_handler (v2.20.2, A)
// ===========================================================================
// Défaut PROUVÉ le 15/08/2026 : l'unique std::set_new_handler(oomNewHandler)
// vivait en FIN de oomGuardLoopTick(), donc APRÈS le `return` de la branche
// d'échec de réarmement. Le handler se désinscrivant LUI-MÊME à la 2ᵉ famine,
// plus aucun chemin ne le ré-inscrivait : parachute mort ~2 h après le boot.

void test_register_handler_refused_when_reserve_absent(void) {
  // CONDITION C1 du gate pool-chemistry — cas OBLIGATOIRE : sans réserve armée,
  // on NE ré-inscrit PAS. Un handler sans réserve n'a rien à libérer ; il ne
  // ferait que retarder d'un tour le bad_alloc en masquant l'état réel.
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(false, false));
  // Même sans réserve, si le handler est (encore) inscrit, rien à faire non plus.
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(false, true));
}

void test_register_handler_when_reserve_armed_and_handler_absent(void) {
  // Le seul cas qui déclenche : réserve armée + handler absent (état Depleted
  // après la 2ᵉ famine, puis réarmement réussi).
  TEST_ASSERT_TRUE(oomShouldRegisterHandler(true, false));
}

void test_register_handler_is_idempotent(void) {
  // Déjà inscrit avec réserve : NON — sinon l'INFO « new_handler ré-inscrit »
  // serait émise à chaque tick (10 s) et re-saturerait le journal persistant,
  // le défaut même que ce cycle corrige.
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(true, true));
  // Idempotence sur une longue série de ticks sans changement d'état.
  for (int tick = 0; tick < 100; tick++) {
    TEST_ASSERT_FALSE(oomShouldRegisterHandler(true, true));
  }
}

void test_register_handler_full_scenario_of_20260815_bug(void) {
  // Scénario complet du bug, rejoué : boot armé → 1ʳᵉ famine (réserve libérée,
  // handler CONSERVÉ) → 2ᵉ famine (handler DÉSINSCRIT, plus de réserve) →
  // le tick ne ré-inscrit pas tant que rien n'est armé → réarmement → filet.
  bool reserve = true, handler = true;
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(reserve, handler));  // régime nominal

  OomHandlerAction a1 = oomOnAllocFailed(true);                   // 1ʳᵉ famine
  reserve = false;                                                // réserve libérée
  handler = handler && !a1.unregisterHandler;                     // toujours inscrit
  TEST_ASSERT_TRUE(handler);
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(reserve, handler));

  OomHandlerAction a2 = oomOnAllocFailed(false);                  // 2ᵉ famine
  handler = handler && !a2.unregisterHandler;                     // se désinscrit
  TEST_ASSERT_FALSE(handler);
  // ICI vivait le bug : réserve absente ET handler absent. La ré-inscription
  // reste REFUSÉE (C1) — c'est le réarmement qui doit rétablir la protection.
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(reserve, handler));

  // Réarmement effectif dans le régime 8 bits réel → réserve de repli armée.
  uint32_t target = rearmTarget(0u, kField8MaxBlocTypical);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFallbackReserveBytes, target);
  reserve = true;
  TEST_ASSERT_TRUE(oomShouldRegisterHandler(reserve, handler));   // le filet joue
  handler = true;                                                 // la coquille inscrit
  TEST_ASSERT_FALSE(oomShouldRegisterHandler(reserve, handler));  // une seule fois

  // NOTE DE PORTÉE (contre-revue 2.20.2) : ce scénario modélise le prédicat pur
  // et l'usage qu'en fait initOomGuard(). Il ne décrit PLUS le tick, qui
  // n'interroge délibérément plus le miroir : il appelle set_new_handler() à
  // chaque passage tant qu'une réserve est armée (set_new_handler est
  // idempotent), justement pour que la vivacité du parachute ne dépende JAMAIS
  // d'un miroir susceptible de se désynchroniser. Le miroir n'y sert qu'à
  // n'émettre l'INFO qu'une fois et à alimenter oom_handler_registered.
}

// ===========================================================================
// 12. oomFailWarnNextDelayMs — BACKOFF du WARN d'échec (v2.20.2, C)
// ===========================================================================
// 30 s fixes → ~120 WARN/h → rétention du journal persistant de 25-35 min : le
// post-mortem du crash du 15/08 a été détruit par notre propre journalisation.

void test_fail_warn_backoff_first_emission_uses_initial_delay(void) {
  // Convention : 0 = jamais émis → palier INITIAL (le signal reste immédiat).
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayInitialMs,
                           oomFailWarnNextDelayMs(0u));
  // Toute valeur résiduelle sous le palier initial se recale sur l'initial.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayInitialMs,
                           oomFailWarnNextDelayMs(1u));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayInitialMs,
                           oomFailWarnNextDelayMs((uint32_t)kOomFailWarnDelayInitialMs - 1u));
}

void test_fail_warn_backoff_escalates_30s_5min_30min(void) {
  // Progression imposée par la spec : 30 s → 5 min → 30 min.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMidMs,
                           oomFailWarnNextDelayMs((uint32_t)kOomFailWarnDelayInitialMs));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMaxMs,
                           oomFailWarnNextDelayMs((uint32_t)kOomFailWarnDelayMidMs));
  // Bornes : le palier suivant est décidé sur « strictement inférieur à ».
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMidMs,
                           oomFailWarnNextDelayMs((uint32_t)kOomFailWarnDelayInitialMs + 1u));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMidMs,
                           oomFailWarnNextDelayMs((uint32_t)kOomFailWarnDelayMidMs - 1u));
}

void test_fail_warn_backoff_saturates_at_last_tier(void) {
  // Saturation : le WARN n'est JAMAIS complètement éteint (pas de délai infini,
  // pas de désactivation) — il plafonne au dernier palier.
  uint32_t d = (uint32_t)kOomFailWarnDelayMaxMs;
  for (int i = 0; i < 50; i++) {
    d = oomFailWarnNextDelayMs(d);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMaxMs, d);
  }
  // Valeur aberrante au-delà du plafond (état corrompu) : ramenée au plafond.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMaxMs,
                           oomFailWarnNextDelayMs(0xFFFFFFFFu));
}

void test_fail_warn_backoff_sequence_from_scratch_is_monotonic(void) {
  // Séquence complète telle que la vit la loop : 3 émissions suffisent à
  // atteindre la saturation, et la suite est monotone non décroissante.
  uint32_t d = 0u;  // jamais émis
  d = oomFailWarnNextDelayMs(d);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayInitialMs, d);
  uint32_t previous = d;
  d = oomFailWarnNextDelayMs(d);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMidMs, d);
  TEST_ASSERT_TRUE(d > previous);
  previous = d;
  d = oomFailWarnNextDelayMs(d);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMaxMs, d);
  TEST_ASSERT_TRUE(d > previous);
  for (int i = 0; i < 20; i++) {
    previous = d;
    d = oomFailWarnNextDelayMs(d);
    TEST_ASSERT_TRUE(d >= previous);
  }
}

void test_fail_warn_backoff_reset_restarts_at_initial_tier(void) {
  // Remise au palier initial : la RÈGLE RETENUE est que l'horloge appartient à
  // l'appelant (kOomFailWarnDelayResetMs = 1 h sans nouvel échec → la coquille
  // repose sFailWarnDelayMs au palier initial et remet sLastFailWarnMs à 0).
  // La fonction pure doit donc, depuis cet état remis à zéro, redonner la
  // séquence complète — aucun délai résiduel de 30 min ne survit à un épisode.
  uint32_t d = (uint32_t)kOomFailWarnDelayMaxMs;   // épisode précédent saturé
  d = (uint32_t)kOomFailWarnDelayInitialMs;        // remise par l'appelant
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayMidMs, oomFailWarnNextDelayMs(d));
  // Et depuis l'état « jamais émis » (sLastFailWarnMs == 0), palier initial.
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOomFailWarnDelayInitialMs, oomFailWarnNextDelayMs(0u));
}

void test_fail_warn_backoff_real_constant_values(void) {
  TEST_ASSERT_EQUAL_UINT32(30000u, (uint32_t)kOomFailWarnDelayInitialMs);
  TEST_ASSERT_EQUAL_UINT32(300000u, (uint32_t)kOomFailWarnDelayMidMs);
  TEST_ASSERT_EQUAL_UINT32(1800000u, (uint32_t)kOomFailWarnDelayMaxMs);
  TEST_ASSERT_EQUAL_UINT32(3600000u, (uint32_t)kOomFailWarnDelayResetMs);
  // Paliers STRICTEMENT croissants, sinon l'escalade est un no-op silencieux.
  TEST_ASSERT_TRUE(kOomFailWarnDelayInitialMs < kOomFailWarnDelayMidMs);
  TEST_ASSERT_TRUE(kOomFailWarnDelayMidMs < kOomFailWarnDelayMaxMs);
  // La remise doit être PLUS LONGUE que le palier de saturation : sinon un
  // épisode continu serait remis à zéro entre deux WARN et l'escalade
  // n'atteindrait jamais son effet (retour au régime 120 WARN/h).
  TEST_ASSERT_TRUE(kOomFailWarnDelayResetMs > kOomFailWarnDelayMaxMs);
  // BUDGET JOURNAL (critère de la spec : rétention > 12 h). Au palier de
  // saturation, le WARN coûte au plus 3 600 000 / 1 800 000 = 2 lignes/h ; avec
  // la trace « Heap 8bits » ramenée elle aussi à 30 min (2 lignes/h) et une
  // ligne persistée de ~116 o, le journal de 8 Ko (rotation à 6 Ko) tient
  // largement au-delà de 12 h. On verrouille ici la borne de cadence.
  const uint32_t kLignesParHeureMax = 3600000u / (uint32_t)kOomFailWarnDelayMaxMs;
  TEST_ASSERT_TRUE(kLignesParHeureMax <= 2u);
}

// ===========================================================================
// 13. oomShouldCountFailure — exclusion des échecs AUTO-INFLIGÉS (v2.20.2, E1)
// ===========================================================================
// Sur les 1025 échecs comptés le 15/08, ~100 % étaient le malloc(12288) du
// réarmement du parachute lui-même, visant un palier inatteignable en régime
// fragmenté : la garde s'alarmait de son propre bruit et saturait le journal.

void test_count_failure_true_for_genuine_failure(void) {
  // Aucune allocation auto-infligée en cours → l'échec est RÉEL, il compte
  // (post-mortem RTC + compteur + WARN d'attribution).
  TEST_ASSERT_TRUE(oomShouldCountFailure(false));
}

void test_count_failure_false_while_self_alloc_in_flight(void) {
  // Drapeau posé autour du SEUL malloc de réarmement → échec ignoré.
  TEST_ASSERT_FALSE(oomShouldCountFailure(true));
}

void test_count_failure_window_is_closed_after_self_alloc(void) {
  // Le drapeau est retiré IMMÉDIATEMENT après le malloc, y compris en cas
  // d'échec : la fenêtre d'aveuglement ne doit pas fuir sur les ticks suivants.
  bool selfAlloc = false;
  TEST_ASSERT_TRUE(oomShouldCountFailure(selfAlloc));   // avant
  selfAlloc = true;
  TEST_ASSERT_FALSE(oomShouldCountFailure(selfAlloc));  // pendant le malloc(target)
  selfAlloc = false;
  TEST_ASSERT_TRUE(oomShouldCountFailure(selfAlloc));   // après (échec réel recompté)
}

void test_count_failure_suppresses_the_1025_self_inflicted_of_20260815(void) {
  // Rejeu quantitatif : 3 tentatives de réarmement par fenêtre de 30 s, toutes
  // en échec (c'est le « ×3 » observé toutes les 30 s pendant des heures). Avec
  // le drapeau, AUCUNE n'est comptée ; un unique échec réel intercalé, lui, l'est.
  uint32_t comptes = 0;
  for (int fenetre = 0; fenetre < 400; fenetre++) {     // ~3 h 20 de ticks
    for (int tentative = 0; tentative < 3; tentative++) {
      if (oomShouldCountFailure(true)) comptes++;       // malloc de réarmement
    }
  }
  TEST_ASSERT_EQUAL_UINT32(0u, comptes);
  if (oomShouldCountFailure(false)) comptes++;          // vrai échec (async_tcp)
  TEST_ASSERT_EQUAL_UINT32(1u, comptes);
}

// ===========================================================================
// 14. bug-080 B-2 — SONDE DU REFUS : aller-retour RTC des 4 mots neufs (T1)
// ===========================================================================
// La zone passe de 32 à 48 o. Les mots w8..w11 sont écrits par le chemin
// d'échec en STORES BRUTS (oom_guard.cpp) et relus par l'encodeur/décodeur pur :
// une divergence de quantum ou d'offset ne lèverait aucune erreur, elle
// produirait un DIAGNOSTIC FAUX. D'où des attendus en valeurs absolues.

// Jeu de champs de sonde REPRÉSENTATIF (le drapeau + l'égalité de classe
// d'échec), monté sur la base de makeFields().
// ⚠️ COHÉRENCE PHYSIQUE DES CAPS. MALLOC_CAP_SPIRAM vaut 1<<10 = 0x0400 et
// MALLOC_CAP_INTERNAL 1<<11 = 0x0800. Le masque 0x0404 de makeFields() (relevé
// du 13/08) porte donc le bit SPIRAM : une sonde sur cette classe emprunterait
// le parcours O(RÉGIONS) et son « plus gros bloc » vaudrait la sentinelle. Pour
// que les fixtures de sonde restent PHYSIQUEMENT POSSIBLES, on les cale sur la
// classe INTERNE 0x080C (8BIT|DMA|INTERNAL, celle des 6 refus du 05/09) ; le cas
// SPIRAM a sa propre fixture (T3-h).
static OomPostMortemFields makeFieldsWithProbe(void) {
  OomPostMortemFields f = makeFields();
  f.lastCaps = 0x0000080Cu;
  f.flags |= kOomPmFlagProbeTaken;
  f.probeSize = f.lastSize;                        // même classe d'échec
  f.probeCaps = (uint16_t)(f.lastCaps & 0xFFFFu);  // 0x080C
  f.probeCapsMaxB = 1024;
  f.probeCapsFree = 8192;
  f.probeRefMaxB = 26560;
  f.probeRefFree = 81408;
  f.probeDurationUs = 137;
  return f;
}

void test_probe_roundtrip_preserves_all_four_new_words(void) {
  // T1 — aller-retour fidèle sur des valeurs déjà alignées sur les quanta.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields in = makeFieldsWithProbe();
  oomPostMortemEncode(raw, in);
  OomPostMortemFields out = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(in.probeSize, out.probeSize);
  TEST_ASSERT_EQUAL_UINT32(1024u, out.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(8192u, out.probeCapsFree);
  TEST_ASSERT_EQUAL_UINT32(26560u, out.probeRefMaxB);
  TEST_ASSERT_EQUAL_UINT32(81408u, out.probeRefFree);
  TEST_ASSERT_EQUAL_HEX16(0x080Cu, out.probeCaps);
  TEST_ASSERT_EQUAL_UINT16(137u, out.probeDurationUs);
  TEST_ASSERT_EQUAL_UINT8(in.flags, out.flags);
  TEST_ASSERT_TRUE(oomPostMortemValid(out));
  // Les champs HISTORIQUES ne bougent pas d'un octet : l'extension est ADDITIVE
  // (w0..w7 inchangés), sans quoi l'ancien dump serait décodé de travers.
  TEST_ASSERT_EQUAL_UINT32(in.failCount, out.failCount);
  TEST_ASSERT_EQUAL_UINT32(in.lastSize, out.lastSize);
  TEST_ASSERT_EQUAL_UINT32(in.ringFreeBytes, out.ringFreeBytes);
  TEST_ASSERT_EQUAL_UINT32(in.ringMaxAllocBytes, out.ringMaxAllocBytes);
  TEST_ASSERT_EQUAL_UINT8(in.releaseCount, out.releaseCount);
}

void test_probe_wire_offsets_are_w8_to_w11(void) {
  // T1 — offsets EXACTS : w8=[32..35], w9=[36..39], w10=[40..43], w11=[44..47].
  // C'est le contrat que respectent les stores bruts sOomPmWords[8..11].
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();
  f.probeSize = 0x11223344u;
  f.probeCaps = 0xABCDu;
  f.probeDurationUs = 0x1234u;
  oomPostMortemEncode(raw, f);
  TEST_ASSERT_EQUAL_HEX8(0x44, raw[32]);  // probeSize LE, EXACT (non quantifié)
  TEST_ASSERT_EQUAL_HEX8(0x33, raw[33]);
  TEST_ASSERT_EQUAL_HEX8(0x22, raw[34]);
  TEST_ASSERT_EQUAL_HEX8(0x11, raw[35]);
  // w9 : bits 0-15 = maxBloc >> 6 (1024 >> 6 = 16), bits 16-31 = libre >> 8
  // (8192 >> 8 = 32). Quanta IDENTIQUES à ceux des champs « ring ».
  TEST_ASSERT_EQUAL_UINT16(16u, (uint16_t)(raw[36] | (raw[37] << 8)));
  TEST_ASSERT_EQUAL_UINT16(32u, (uint16_t)(raw[38] | (raw[39] << 8)));
  // w10 : réf8 26560 >> 6 = 415 ; 81408 >> 8 = 318
  TEST_ASSERT_EQUAL_UINT16(415u, (uint16_t)(raw[40] | (raw[41] << 8)));
  TEST_ASSERT_EQUAL_UINT16(318u, (uint16_t)(raw[42] | (raw[43] << 8)));
  // w11 : caps sur les 16 bits BAS, durée sur les 16 bits HAUTS — les deux
  // doivent cohabiter sans se mordre (0xABCD et 0x1234 sont choisis distincts).
  TEST_ASSERT_EQUAL_UINT16(0xABCDu, (uint16_t)(raw[44] | (raw[45] << 8)));
  TEST_ASSERT_EQUAL_UINT16(0x1234u, (uint16_t)(raw[46] | (raw[47] << 8)));
}

void test_probe_size_is_exact_never_quantized(void) {
  // T1 — probeSize N'EST PAS quantifié, contrairement aux blocs/libres. 1 622 o
  // (la taille refusée le 05/09) n'est multiple d'aucun quantum : la quantifier
  // casserait l'égalité probeSize == lastSize, donc la garde de
  // REPRÉSENTATIVITÉ, donc tout le verdict neuf. Cas de bord voulu.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();
  const uint32_t tailles[] = {1u, 63u, 100u, 156u, 356u, 1622u, 4095u, 65537u,
                              kOomPmMaxSizeBytes};
  for (unsigned i = 0; i < sizeof(tailles) / sizeof(tailles[0]); i++) {
    f.probeSize = tailles[i];
    f.lastSize = tailles[i];
    oomPostMortemEncode(raw, f);
    OomPostMortemFields o = oomPostMortemDecode(raw);
    TEST_ASSERT_EQUAL_UINT32(tailles[i], o.probeSize);
    TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(o));
  }
}

void test_probe_quantization_is_bounded_and_pessimistic(void) {
  // T1 — quanta 64 o (bloc) et 256 o (libre), perte TOUJOURS par défaut : la
  // valeur relue ne SURESTIME jamais le tas, sans quoi le verdict pencherait à
  // tort vers « un bloc existait » (corruption).
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();
  f.probeCapsMaxB = 1087;   // 16,98 quanta → 16 → 1024 (perte 63 o)
  f.probeCapsFree = 8447;   // 32,99 quanta → 32 → 8192 (perte 255 o)
  f.probeRefMaxB = 26612;   // relevé RÉEL du 05/09 : 415,8 → 415 → 26560
  f.probeRefFree = 81599;
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(1024u, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(8192u, o.probeCapsFree);
  TEST_ASSERT_EQUAL_UINT32(26560u, o.probeRefMaxB);
  TEST_ASSERT_EQUAL_UINT32(81408u, o.probeRefFree);
  TEST_ASSERT_TRUE(o.probeCapsMaxB <= f.probeCapsMaxB);
  TEST_ASSERT_TRUE(f.probeCapsMaxB - o.probeCapsMaxB < 64u);
  TEST_ASSERT_TRUE(o.probeRefMaxB <= f.probeRefMaxB);
  TEST_ASSERT_TRUE(f.probeRefMaxB - o.probeRefMaxB < 64u);
  TEST_ASSERT_TRUE(o.probeCapsFree <= f.probeCapsFree);
  TEST_ASSERT_TRUE(f.probeCapsFree - o.probeCapsFree < 256u);
  TEST_ASSERT_TRUE(o.probeRefFree <= f.probeRefFree);
  TEST_ASSERT_TRUE(f.probeRefFree - o.probeRefFree < 256u);
}

void test_probe_blocks_saturate_above_4_mib(void) {
  // T1 — au-delà de la dynamique 16 bits, SATURATION (jamais de repliement, qui
  // afficherait un tas minuscule pour un tas énorme). Le plafond du champ
  // « plus gros bloc » EST la sentinelle kOomProbeBlockUnknown : c'est voulu —
  // aucun encodage spécial, aucune règle de décodage neuve pour le parcours
  // O(régions) de la PSRAM.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();
  f.probeCapsMaxB = 0xFFFFFFFFu;
  f.probeCapsFree = 0xFFFFFFFFu;
  f.probeRefMaxB = 8u * 1024u * 1024u;   // 8 Mio > 4 Mio de dynamique
  f.probeRefFree = 32u * 1024u * 1024u;
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(kOomProbeBlockUnknown, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 6, o.probeRefMaxB);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 8, o.probeCapsFree);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 8, o.probeRefFree);
  TEST_ASSERT_EQUAL_UINT32(4194240u, kOomProbeBlockUnknown);
  // Juste sous la saturation : valeur exacte conservée (pas de sentinelle par
  // accident sur un tas de 4 Mio - 64 o).
  f.probeCapsMaxB = kOomProbeBlockUnknown - 64u;
  oomPostMortemEncode(raw, f);
  o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(kOomProbeBlockUnknown - 64u, o.probeCapsMaxB);
}

void test_probe_duration_saturates_at_fffe_reserving_ffff(void) {
  // T1 — 0xFFFF est RÉSERVÉ au marqueur « non mesurable » (migration de cœur
  // pendant la mesure : les compteurs CCOUNT sont PAR CŒUR). Une durée réelle
  // plafonne donc à 0xFFFE. Sans cette réservation, une sonde pathologique de
  // 70 ms serait relue « mesure impossible » — et n'aurait déclenché AUCUNE
  // désactivation, l'autolimitation ignorant les échantillons invalides.
  TEST_ASSERT_EQUAL_UINT16(0u, oomProbeSaturateDurationUs(0u));
  TEST_ASSERT_EQUAL_UINT16(137u, oomProbeSaturateDurationUs(137u));
  TEST_ASSERT_EQUAL_UINT16(kOomProbeMaxDurationUs,
                           oomProbeSaturateDurationUs(kOomProbeMaxDurationUs));
  TEST_ASSERT_EQUAL_UINT16(0xFFFEu, oomProbeSaturateDurationUs(0xFFFEu));
  TEST_ASSERT_EQUAL_UINT16(0xFFFEu, oomProbeSaturateDurationUs(0xFFFFu));      // pas le marqueur
  TEST_ASSERT_EQUAL_UINT16(0xFFFEu, oomProbeSaturateDurationUs(70000u));       // 70 ms
  TEST_ASSERT_EQUAL_UINT16(0xFFFEu, oomProbeSaturateDurationUs(0xFFFFFFFFu));
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, kOomProbeDurationInvalid);
  // Et le marqueur traverse la zone RTC sans être confondu avec une durée.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();
  f.probeDurationUs = kOomProbeDurationInvalid;
  f.probeCaps = 0x080Cu;
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT16(kOomProbeDurationInvalid, o.probeDurationUs);
  TEST_ASSERT_EQUAL_HEX16(0x080Cu, o.probeCaps);  // les caps ne sont pas mangées
}

void test_probe_absent_zone_decodes_to_zeroed_probe(void) {
  // T1 — une zone SANS sonde (drapeau absent) doit relire des champs à ZÉRO, et
  // surtout ne PAS être jugée représentative : c'est ce qui évite d'afficher
  // « au refus de 0 o → maxBloc=0 » comme s'il s'agissait d'une mesure.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();  // aucun champ de sonde renseigné
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(0u, o.probeSize);
  TEST_ASSERT_EQUAL_UINT32(0u, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(0u, o.probeRefMaxB);
  TEST_ASSERT_EQUAL_UINT16(0u, o.probeCaps);
  TEST_ASSERT_EQUAL_UINT16(0u, o.probeDurationUs);
  TEST_ASSERT_EQUAL_UINT8(0u, o.flags & kOomPmFlagProbeTaken);
  TEST_ASSERT_FALSE(oomPostMortemProbeRepresentative(o));
}

// ===========================================================================
// 14bis. bug-080 B-2 — L'ANCIEN MAGIC DOIT ÊTRE JUGÉ INVALIDE (T2)
// ===========================================================================
// LE test le plus important de la section format. Une zone de 32 o écrite par un
// firmware ≤ v2.35.0 est suivie, en RAM RTC noinit, de 16 octets JAMAIS
// INITIALISÉS. Les relire comme w8..w11 avec les nouveaux libellés produirait
// des valeurs de sonde inventées — mais parfaitement crédibles : une taille de
// quelques centaines d'octets, un masque de caps plausible, un maxBloc de
// quelques kilo-octets. Et ces valeurs feraient AUTORITÉ sur l'anneau (règle 2),
// donc décideraient du verdict. Perdre le post-mortem du boot de bascule est
// accepté ; afficher un diagnostic inventé ne l'est pas.

void test_old_magic_f00dfa12_is_rejected(void) {
  // T2 — le magic de la zone de 32 o (v2.19.0 → v2.35.0) doit être REFUSÉ.
  OomPostMortemFields f = makeFieldsWithProbe();
  f.magic = 0xF00DFA12u;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  // Et celui d'avant lui aussi (v2.18.x, métrique heap brute).
  f.magic = 0xF00DFA11u;
  TEST_ASSERT_FALSE(oomPostMortemValid(f));
  // Le magic courant, et lui seul, ouvre la lecture.
  f.magic = kOomPostMortemMagic;
  TEST_ASSERT_TRUE(oomPostMortemValid(f));
  TEST_ASSERT_EQUAL_HEX32(0xF00DFA13u, kOomPostMortemMagic);
  // Le bump doit être STRICTEMENT croissant et d'exactement un cran : c'est la
  // convention du projet (v2.19.0 avait fait FA11 → FA12).
  TEST_ASSERT_EQUAL_HEX32(0xF00DFA12u + 1u, kOomPostMortemMagic);
}

void test_old_32_byte_zone_followed_by_rtc_garbage_is_rejected(void) {
  // T2 — SIMULATION du boot de bascule, le vrai scénario redouté : les 32
  // premiers octets sont une zone LÉGITIME écrite par l'ancien firmware (magic
  // 0xF00DFA12, échecs réels), les 16 suivants sont de la RAM RTC non
  // initialisée dont le contenu est arbitraire mais PLAUSIBLE une fois décodé.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields ancien = makeFields();
  oomPostMortemEncode(raw, ancien);
  raw[0] = 0x12;  // magic de l'ANCIEN format (0xF00DFA12)
  // Résidu RTC : des octets quelconques, choisis pour se décoder en une sonde
  // crédible plutôt qu'en absurdité — c'est le piège exact.
  const uint8_t residu[16] = {0x64, 0x02, 0x00, 0x00,   // « probeSize = 612 »
                              0x40, 0x00, 0x20, 0x00,   // « maxBloc = 4096, libre = 8192 »
                              0x00, 0x02, 0x40, 0x01,   // « réf8 = 32768 / 81920 »
                              0x0C, 0x08, 0x91, 0x00};  // « caps = 0x080C, 145 µs »
  for (unsigned i = 0; i < 16u; i++) raw[32 + i] = residu[i];
  OomPostMortemFields o = oomPostMortemDecode(raw);
  // Le décodage produit bien des valeurs qui PASSERAIENT pour une mesure...
  TEST_ASSERT_EQUAL_UINT32(612u, o.probeSize);
  TEST_ASSERT_EQUAL_HEX16(0x080Cu, o.probeCaps);
  TEST_ASSERT_EQUAL_UINT32(4096u, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT16(145u, o.probeDurationUs);
  // ...mais la zone est REFUSÉE en bloc par le magic : rien n'est jamais dumpé.
  TEST_ASSERT_FALSE(oomPostMortemValid(o));
  // Contre-épreuve (anti-vacuité) : le MÊME contenu avec le magic courant serait
  // accepté. C'est donc bien le bump du magic, et rien d'autre, qui protège.
  raw[0] = 0x13;
  OomPostMortemFields o2 = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemValid(o2));
}

// ===========================================================================
// 15. bug-080 B-2 — VERDICT RÉVISÉ, les 7 cas de la règle (T3)
// ===========================================================================

void test_verdict_probe_dma_starvation_is_the_new_case(void) {
  // T3-a — sonde représentative, le pool DEMANDÉ est à sec (maxBloc < taille)
  // mais la RÉFÉRENCE INTERNAL|8BIT reste saine (>= ratio × taille) :
  // FAMINE DMA. C'est le verdict que le lot ajoute, et le seul qui décrive
  // correctement un pool restreint épuisé sur un tas général sain.
  OomPostMortemFields f = makeFieldsWithProbe();
  f.lastSize = 1622;
  f.lastCaps = 0x0000080Cu;
  f.probeSize = 1622;
  f.probeCaps = 0x080Cu;
  f.probeCapsMaxB = 1024;    // pool DMA à sec
  f.probeCapsFree = 3072;
  f.probeRefMaxB = 26560;    // tas 8 bits sain (16× la demande)
  f.probeRefFree = 81408;
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictDmaStarvation, verdict(f));
  // Ce verdict est DISTINCT des trois autres (pas un alias) et vaut 3.
  TEST_ASSERT_EQUAL_UINT8(3u, kOomVerdictDmaStarvation);
  TEST_ASSERT_NOT_EQUAL(kOomVerdictStarvation, kOomVerdictDmaStarvation);
  TEST_ASSERT_NOT_EQUAL(kOomVerdictCorruptionSuspected, kOomVerdictDmaStarvation);
  TEST_ASSERT_NOT_EQUAL(kOomVerdictUndetermined, kOomVerdictDmaStarvation);
}

void test_verdict_probe_general_starvation_when_reference_also_dry(void) {
  // T3-b — sonde représentative, pool demandé à sec ET référence à sec : ce
  // n'est plus une affaire de pool, tout le tas est vide → FAMINE MÉMOIRE.
  OomPostMortemFields f = makeFieldsWithProbe();
  f.lastSize = 1622;
  f.probeSize = 1622;
  f.probeCapsMaxB = 512;
  f.probeRefMaxB = 3200;   // 3200 / 4 = 800 < 1622 → référence PAS saine
  f.probeRefFree = 4096;
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
  // FRONTIÈRE du ratio (anti-mutant « saneBlockRatio → 1 ») : la référence bascule
  // « saine » à exactement 4 × la taille demandée, pas avant. Avec un ratio de 1,
  // les deux valeurs ci-dessous rendraient FAMINE DMA — le test le refuse.
  f.probeRefMaxB = 4u * 1622u;        // 6488 : pile le ratio → DMA
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictDmaStarvation, verdict(f));
  f.probeRefMaxB = 4u * 1622u - 1u;   // 6487 : un octet sous → famine générale
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_probe_corruption_is_now_the_only_path(void) {
  // T3-c — sonde représentative et un bloc SUFFISANT existait à l'instant exact
  // du refus : l'allocateur a menti → SUSPICION CORRUPTION. C'est désormais le
  // SEUL chemin vers ce verdict, et il repose sur une mesure contemporaine.
  OomPostMortemFields f = makeFieldsWithProbe();
  f.lastSize = 1622;
  f.probeSize = 1622;
  f.probeCapsMaxB = 26560;   // un bloc 16× plus gros que la demande, AU REFUS
  f.probeCapsFree = 81408;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
  // FRONTIÈRE (anti-mutant « inversion de probeCapsMaxB < probeSize ») : le
  // basculement se fait à l'octet près, et l'égalité compte comme « ça tenait ».
  f.probeCapsMaxB = 1622u;       // exactement la taille demandée → ça tenait
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
  f.probeCapsMaxB = 1621u;       // un octet de moins → l'allocateur disait vrai
  TEST_ASSERT_NOT_EQUAL(kOomVerdictCorruptionSuspected, verdict(f));
  // Et la corruption l'emporte sur TOUT le reste : rafale d'échecs, anneau
  // effondré — la sonde fait autorité, l'anneau est IGNORÉ (règle 2).
  f.probeCapsMaxB = 26560u;
  f.failCount = kOomCorruptionMaxFails + 50u;
  f.firstUptimeS = 100;
  f.lastUptimeS = 102;                 // rafale de 2 s
  f.ringMaxAllocBytes = 0;             // anneau à sec
  f.ringFreeBytes = 0;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
}

void test_verdict_probe_not_representative_falls_back_to_rule_3(void) {
  // T3-d — la sonde décrit un AUTRE refus que celui qu'on qualifie. Le jeton
  // « un échantillon par épisode » rend ce cas parfaitement possible : la sonde
  // est prise au 1er refus, et c'est le DERNIER qui est enregistré dans w2..w4.
  // Une sonde non représentative ne doit jamais faire autorité.
  OomPostMortemFields base = makeFieldsWithProbe();
  base.lastSize = 1622;
  base.lastCaps = 0x0000080Cu;
  base.probeCapsMaxB = 26560;   // « un bloc existait » — piège à corruption
  base.probeCapsFree = 81408;
  base.probeRefMaxB = 26560;
  base.ringMaxAllocBytes = 26560;  // anneau sain lui aussi → règle 3 s'abstient

  // (i) MÊMES caps, taille DIFFÉRENTE
  OomPostMortemFields f = base;
  f.probeSize = 356;                            // ≠ lastSize
  f.probeCaps = (uint16_t)(f.lastCaps & 0xFFFFu);
  TEST_ASSERT_FALSE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));

  // (ii) MÊME taille, caps DIFFÉRENTES — c'est la garde que le mutant
  // « suppression de probeCaps == lastCaps » retire. Sans elle, une sonde prise
  // sur un refus INTERNAL|8BIT servirait à juger un refus DMA : exactement la
  // confusion de classes que le lot supprime.
  f = base;
  f.probeSize = f.lastSize;
  f.probeCaps = 0x0404u;                        // ≠ lastCaps (0x080C)
  TEST_ASSERT_FALSE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));

  // (iii) tout concorde MAIS le drapeau ProbeTaken est absent (w8..w11 sont des
  // zéros, ou pire, des résidus) → jamais exploités.
  f = base;
  f.probeSize = f.lastSize;
  f.probeCaps = (uint16_t)(f.lastCaps & 0xFFFFu);
  f.flags &= (uint8_t)~kOomPmFlagProbeTaken;
  TEST_ASSERT_FALSE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));

  // CONTRE-ÉPREUVE (anti-vacuité) : les trois champs concordants ET le drapeau
  // → la sonde reprend autorité et le verdict change. Les trois cas ci-dessus
  // prouvent donc bien quelque chose.
  f = base;
  f.probeSize = f.lastSize;
  f.probeCaps = (uint16_t)(f.lastCaps & 0xFFFFu);
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
}

void test_verdict_no_probe_with_healthy_ring_is_undetermined(void) {
  // T3-e — LE CAS QUI JUSTIFIE LE LOT. Aucune sonde (jamais prise, sautée en
  // ISR, ou sonde désactivée) et un anneau qui annonce un tas sain : la règle
  // historique concluait « SUSPICION CORRUPTION » sur une donnée vieille de 2 s
  // et mesurée sur d'autres caps. Elle s'abstient désormais.
  OomPostMortemFields f = makeFields();     // pas de sonde du tout
  f.failCount = 3;
  f.lastSize = 1622;
  f.ringMaxAllocBytes = 26560;              // 16× la demande : « sain »
  f.ringFreeBytes = 81408;
  f.firstUptimeS = 100;
  f.lastUptimeS = 1377;                     // 1277 s : pas une rafale
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  // Les trois sous-cas d'absence sont traités à l'identique par le verdict (le
  // dump, lui, les NOMME pour que l'indétermination soit attribuable).
  f.flags |= kOomPmFlagProbeIsrSkipped;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  f.flags |= kOomPmFlagProbeDisabled;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
}

void test_verdict_no_probe_burst_is_still_starvation(void) {
  // T3-f — la règle 3 garde ce qu'elle savait établir sans mesure
  // contemporaine : une RAFALE d'échecs reste une famine. Le lot ampute la
  // conclusion « corruption », pas les deux autres.
  OomPostMortemFields f = makeFields();
  f.failCount = kOomCorruptionMaxFails + 1u;
  f.lastSize = 1622;
  f.ringMaxAllocBytes = 26560;   // anneau « sain » — la rafale l'emporte
  f.firstUptimeS = 1000;
  f.lastUptimeS = 1005;          // 5 s
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
  // ...et la comptabilité écroulée aussi, hors rafale.
  f.failCount = 1;
  f.lastUptimeS = 1000 + 3600;
  f.ringMaxAllocBytes = 2048;    // 2048 / 4 = 512 < 1622
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

void test_verdict_zero_failcount_stays_undetermined_even_with_a_probe(void) {
  // T3-g — règle 1, évaluée AVANT tout le reste : sans échec exploitable, rien
  // à qualifier. Une sonde résiduelle ne doit pas ressusciter un verdict.
  OomPostMortemFields f = makeFieldsWithProbe();
  f.probeCapsMaxB = 26560;   // « un bloc existait » — ne doit RIEN déclencher
  f.failCount = 0;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  f = makeFieldsWithProbe();
  f.probeCapsMaxB = 26560;
  f.lastSize = 0;
  f.probeSize = 0;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
}

void test_verdict_probe_region_scan_marks_block_unknown(void) {
  // T3-h (borne a priori PSRAM, condition 2 du gate) — quand la sonde a pris le
  // chemin O(régions), le plus gros bloc est « non renseigné ». On ne doit alors
  // JAMAIS conclure « un bloc existait » : on tranche sur le LIBRE seul, et à
  // défaut on s'abstient. Le pire cas est ainsi borné AVANT coup.
  OomPostMortemFields f = makeFieldsWithProbe();
  f.lastSize = 1622;
  f.lastCaps = 0x00000404u;               // 8BIT|SPIRAM — le masque relevé le 13/08
  f.probeSize = 1622;
  f.probeCaps = 0x0404u;
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(f.lastCaps));  // la sonde a bien basculé
  f.probeCapsMaxB = kOomProbeBlockUnknown;
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  // Libre insuffisant → famine, c'est un indice honnête.
  f.probeCapsFree = 1024;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
  // Libre suffisant mais fragmentation inconnue → ABSTENTION (surtout pas
  // « corruption » : c'est précisément l'inférence que le lot supprime).
  f.probeCapsFree = 81408;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  // Frontière exacte du libre (inclusive côté « ça aurait dû tenir »).
  f.probeCapsFree = 1622;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  f.probeCapsFree = 1621;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictStarvation, verdict(f));
}

// ===========================================================================
// 15bis. DÉFAUT B2 DE LA REVUE — la mesure écrite puis silencieusement JETÉE
// ===========================================================================
// oomPostMortemStore() construit le mot 7 (qui porte l'octet `flags`) AVANT que
// la sonde tourne. Sans le flush ajouté en correctif, tout drapeau posé APRÈS
// lui — ProbeTaken en tête — n'atteignait la RAM RTC qu'au refus SUIVANT, donc
// JAMAIS dans le scénario « refus → abort → reboot » que le post-mortem existe
// précisément pour qualifier. La zone se retrouvait avec w8..w11 PARFAITEMENT
// RENSEIGNÉS et un `flags` sans ProbeTaken.

void test_b2_probe_words_written_but_flag_lost_discards_the_measure(void) {
  // REJEU EXACT DU BUG, au niveau des 48 octets. On fabrique la zone telle que
  // le firmware fautif la laissait : mots de sonde écrits, bit ProbeTaken absent
  // de w7. C'est le pire cas de tous — la mesure EXISTE, elle est simplement
  // ignorée, et rien dans le dump ne signale la perte.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();     // sonde complète et cohérente
  f.lastSize = 1622;
  f.probeSize = 1622;
  f.probeCapsMaxB = 1024;                            // pool à sec
  f.probeRefMaxB = 26560;                            // référence saine → FAMINE DMA
  f.ringMaxAllocBytes = 26560;                       // anneau « sain » → règle 3 s'abstient
  oomPostMortemEncode(raw, f);
  // Le bug : l'octet 30 (= bits 16-23 de w7) n'a jamais reçu le bit.
  raw[30] &= (uint8_t)~kOomPmFlagProbeTaken;

  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemValid(o));
  // Les mesures sont bien là, intactes...
  TEST_ASSERT_EQUAL_UINT32(1622u, o.probeSize);
  TEST_ASSERT_EQUAL_UINT32(1024u, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(26560u, o.probeRefMaxB);
  // ...mais elles ne font pas autorité, et le verdict RÉGRESSE vers l'ancien
  // régime : INDÉTERMINÉ au lieu de FAMINE DMA. Le lot est neutralisé.
  TEST_ASSERT_FALSE(oomPostMortemProbeRepresentative(o));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(o));
  // Et la ligne 2 du dump n'est pas émise : sa condition d'émission EST le bit.
  TEST_ASSERT_EQUAL_UINT8(0u, o.flags & kOomPmFlagProbeTaken);

  // CONTRE-ÉPREUVE : le bit remis (ce que fait oomPostMortemFlushFlags), le
  // verdict correct revient. C'est donc bien ce seul bit qui décide.
  raw[30] |= kOomPmFlagProbeTaken;
  OomPostMortemFields ok = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(ok));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictDmaStarvation, verdict(ok));
  TEST_ASSERT_NOT_EQUAL(0u, ok.flags & kOomPmFlagProbeTaken);
}

void test_b2_flush_mask_preserves_ring_uptime_and_release_count(void) {
  // Le flush est un store BRUT dans un mot PARTAGÉ : w7 porte ringUptimeS
  // (bits 0-15), flags (16-23) et releaseCount (24-31). Un masque erroné
  // détruirait l'âge de l'échantillon d'anneau ou le compteur d'épisodes —
  // silencieusement, comme d'habitude avec cette zone.
  const uint32_t w7 = 0x0712ABCDu;  // release=0x07, flags=0x12, ringUptime=0xABCD
  uint32_t sortie = oomPmPackFlagsIntoW7(w7, 0xE5u);
  TEST_ASSERT_EQUAL_HEX32(0xABCDu, sortie & 0xFFFFu);          // uptime INTACT
  TEST_ASSERT_EQUAL_HEX8(0xE5u, (sortie >> 16) & 0xFFu);       // flags remplacés
  TEST_ASSERT_EQUAL_HEX8(0x07u, (sortie >> 24) & 0xFFu);       // épisodes INTACTS
  // Idempotence : reflusher la même valeur ne change rien (le hook peut être
  // réentré, et le flush est appelé depuis trois sites).
  TEST_ASSERT_EQUAL_HEX32(sortie, oomPmPackFlagsIntoW7(sortie, 0xE5u));
  // Cas limites : octet plein / vide, et mot 7 saturé.
  TEST_ASSERT_EQUAL_HEX32(0x07FFABCDu, oomPmPackFlagsIntoW7(w7, 0xFFu));
  TEST_ASSERT_EQUAL_HEX32(0x0700ABCDu, oomPmPackFlagsIntoW7(w7, 0x00u));
  TEST_ASSERT_EQUAL_HEX32(0xFF00FFFFu, oomPmPackFlagsIntoW7(0xFFFFFFFFu, 0x00u));
  TEST_ASSERT_EQUAL_HEX32(0x00200000u, oomPmPackFlagsIntoW7(0u, kOomPmFlagProbeTaken));

  // CONTRAT DE POSITION : le masque n'est correct que si `flags` occupe bien les
  // bits 16-23 de w7, c'est-à-dire l'octet 30 de la zone. On le vérifie contre
  // l'encodeur, seule autre autorité sur ce placement.
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFields();
  f.flags = 0xE5u;
  f.ringUptimeS = 0xABCDu;
  f.releaseCount = 0x07u;
  oomPostMortemEncode(raw, f);
  const uint32_t w7Encode = (uint32_t)raw[28] | ((uint32_t)raw[29] << 8) |
                            ((uint32_t)raw[30] << 16) | ((uint32_t)raw[31] << 24);
  TEST_ASSERT_EQUAL_HEX32(0x07E5ABCDu, w7Encode);
  // Le flush appliqué à ce mot doit être un NO-OP quand les drapeaux sont déjà
  // à jour : preuve que les deux écritures parlent bien du même octet.
  TEST_ASSERT_EQUAL_HEX32(w7Encode, oomPmPackFlagsIntoW7(w7Encode, f.flags));
}

void test_b2_late_flags_isr_and_disabled_also_reach_the_zone(void) {
  // ProbeTaken n'est pas le seul drapeau posé APRÈS oomPostMortemStore() :
  // ProbeIsrSkipped et ProbeDisabled le sont aussi. Sans flush, le dump dirait
  // « sonde absente » là où la vraie raison est « ISR » ou « désactivée » — et
  // l'indétermination cesserait d'être ATTRIBUABLE, ce qui est justement
  // l'exigence du gate.
  const uint8_t tardifs[] = {kOomPmFlagProbeTaken, kOomPmFlagProbeIsrSkipped,
                             kOomPmFlagProbeDisabled};
  for (unsigned i = 0; i < sizeof(tardifs) / sizeof(tardifs[0]); i++) {
    uint32_t w7 = 0x0300FA01u;  // flags à zéro au moment du store
    TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)((w7 >> 16) & 0xFFu));
    w7 = oomPmPackFlagsIntoW7(w7, tardifs[i]);
    TEST_ASSERT_EQUAL_UINT8(tardifs[i], (uint8_t)((w7 >> 16) & 0xFFu));
    TEST_ASSERT_EQUAL_HEX32(0xFA01u, w7 & 0xFFFFu);        // uptime préservé
    TEST_ASSERT_EQUAL_HEX8(0x03u, (w7 >> 24) & 0xFFu);     // épisodes préservés
  }
  // Cumul des trois (une sonde prise, un refus en ISR, puis désactivation) :
  // l'octet est saturé mais reste cohérent.
  uint32_t w7 = oomPmPackFlagsIntoW7(0x0300FA01u,
                                     (uint8_t)(kOomPmFlagProbeTaken |
                                               kOomPmFlagProbeIsrSkipped |
                                               kOomPmFlagProbeDisabled));
  TEST_ASSERT_EQUAL_HEX8(0xE0u, (uint8_t)((w7 >> 16) & 0xFFu));
}

// ===========================================================================
// 16. TEST-PREUVE — rejeu du relevé TERRAIN du 05/09/2026 (T4)
// ===========================================================================
// C'est ce relevé qui justifie tout le lot. Journal réel de l'appareil :
// 6 refus d'allocation de 1 622 o en caps=0x080C (8BIT|DMA|INTERNAL) étalés sur
// ~1 277 s (21 min), sans PANIC, alors que la métrique 8 bits annonçait
// 26 612 o de plus gros bloc. Le firmware de l'époque a rendu
// « SUSPICION CORRUPTION » — un verdict CRITICAL, chiffré, crédible et FAUX sur
// un appareil dont seul le pool DMA était épuisé.
static const uint32_t kField0509FailCount   = 6u;
static const uint32_t kField0509LastSize    = 1622u;
static const uint32_t kField0509LastCaps    = 0x0000080Cu;   // 8BIT|DMA|INTERNAL
static const uint32_t kField0509RingMaxB    = 26612u;
static const uint16_t kField0509SpanS       = 1277u;

static OomPostMortemFields makeField0509(void) {
  OomPostMortemFields f;
  f.magic = kOomPostMortemMagic;
  f.failCount = kField0509FailCount;
  f.firstSize = kField0509LastSize;
  f.lastSize = kField0509LastSize;
  f.lastCaps = kField0509LastCaps;
  f.firstUptimeS = 20000;
  f.lastUptimeS = (uint16_t)(20000u + kField0509SpanS);
  f.ringFreeBytes = 81408;
  f.ringMaxAllocBytes = kField0509RingMaxB;
  f.ringUptimeS = (uint16_t)(20000u + kField0509SpanS - 1u);
  f.flags = kOomPmFlagIntegrityNeverRun;
  f.releaseCount = 0;
  return f;
}

void test_field_20260905_without_probe_is_no_longer_corruption(void) {
  // T4 — LE cas. Sans sonde, la règle 3 ne peut plus accuser : INDÉTERMINÉ.
  OomPostMortemFields f = makeField0509();
  TEST_ASSERT_EQUAL_UINT16(kField0509SpanS, oomPostMortemSpanS(f));
  // Les deux garde-fous de la règle 3 sont bien FRANCHIS (anti-vacuité : ce
  // n'est ni une rafale, ni une comptabilité écroulée — donc on est bien dans
  // la branche « sinon », celle qui concluait « corruption »).
  TEST_ASSERT_TRUE(f.failCount <= kOomCorruptionMaxFails);
  TEST_ASSERT_TRUE(f.ringMaxAllocBytes / kOomCorruptionSaneBlockRatio >= f.lastSize);
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(f));
  // Le verdict FAUX de l'époque n'est plus rendu, sous aucun angle.
  TEST_ASSERT_NOT_EQUAL(kOomVerdictCorruptionSuspected, verdict(f));
  // Et il traverse la zone RTC (chemin réel du dump au boot suivant).
  uint8_t raw[kOomPostMortemBytes];
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemValid(o));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictUndetermined, verdict(o));
}

void test_field_20260905_with_probe_is_dma_starvation(void) {
  // T4 — MÊME relevé, mais instrumenté par B-2 : la sonde mesure le pool
  // RÉELLEMENT demandé (0x080C, ~1 Ko de plus gros bloc) et la référence
  // INTERNAL|8BIT (26 612 o, le chiffre que l'anneau donnait déjà). Le verdict
  // devient FAMINE DMA — le diagnostic qu'il fallait rendre le 05/09.
  OomPostMortemFields f = makeField0509();
  f.flags |= kOomPmFlagProbeTaken;
  f.probeSize = kField0509LastSize;
  f.probeCaps = (uint16_t)(kField0509LastCaps & 0xFFFFu);
  f.probeCapsMaxB = 1024u;                  // pool DMA : ~1 Ko
  f.probeCapsFree = 3328u;
  f.probeRefMaxB = kField0509RingMaxB;      // réf8 : 26 612 o
  f.probeRefFree = 81408u;
  f.probeDurationUs = 180u;                 // sous kOomProbeWarnDurationUs
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(f));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictDmaStarvation, verdict(f));

  // ...et il SURVIT à l'aller-retour RTC, quantification comprise : 26 612 se
  // relit 26 560 (>> 6 puis << 6), ce qui ne change pas le verdict — la marge
  // du ratio est de 16×, pas de 1 %.
  uint8_t raw[kOomPostMortemBytes];
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_TRUE(oomPostMortemValid(o));
  TEST_ASSERT_EQUAL_UINT32(kField0509LastSize, o.probeSize);   // EXACT, non quantifié
  TEST_ASSERT_EQUAL_HEX16(0x080Cu, o.probeCaps);
  TEST_ASSERT_EQUAL_UINT32(1024u, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(26560u, o.probeRefMaxB);
  TEST_ASSERT_EQUAL_UINT16(180u, o.probeDurationUs);
  TEST_ASSERT_TRUE(oomPostMortemProbeRepresentative(o));
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictDmaStarvation, verdict(o));

  // CONTRE-ÉPREUVE : si le pool DMA avait été SAIN au refus (un bloc de 26 Ko
  // disponible pour 1 622 o demandés), alors et seulement alors le verdict de
  // corruption serait justifié — et il est bien rendu. La distinction repose
  // donc entièrement sur la mesure contemporaine, pas sur un réglage de seuil.
  f.probeCapsMaxB = 26560u;
  TEST_ASSERT_EQUAL_UINT8(kOomVerdictCorruptionSuspected, verdict(f));
}

// ===========================================================================
// 17. bug-080 B-2 — PRÉDICATS DE BORNAGE DE LA SONDE (T5, ADR-0017)
// ===========================================================================
// La sonde est le premier appel heap_caps_* JAMAIS autorisé depuis un chemin
// d'échec. Ce qui la rend acceptable n'est pas sa mesure, c'est son BORNAGE :
// jamais en ISR, un échantillon par épisode, un plancher temporel absolu, une
// borne a priori sur le pire cas PSRAM, une désactivation définitive au-delà du
// plafond. Ces décisions vivent dans oom_guard_logic.h — la coquille ne fait
// plus que lire des atomiques et une horloge.

void test_probe_token_is_one_sample_per_episode(void) {
  // T5 — le jeton se consomme UNE fois et ne se réarme JAMAIS tout seul. C'est
  // ce qui empêche une rafale de 6 refus (le relevé du 05/09) de déclencher
  // 6 parcours de tas interruptions coupées.
  bool jeton = true;
  TEST_ASSERT_TRUE(oomProbeTakeTokenModel(jeton));
  TEST_ASSERT_FALSE(jeton);
  for (int refus = 0; refus < 50; refus++) {
    TEST_ASSERT_FALSE(oomProbeTakeTokenModel(jeton));  // rafale : plus rien
  }
  // Seule la loop repose le jeton (drain du WARN d'échec / clôture d'épisode).
  jeton = true;
  TEST_ASSERT_TRUE(oomProbeTakeTokenModel(jeton));
  TEST_ASSERT_FALSE(oomProbeTakeTokenModel(jeton));
  // Jeton déjà désarmé : l'appel ne le RÉARME pas par effet de bord.
  jeton = false;
  TEST_ASSERT_FALSE(oomProbeTakeTokenModel(jeton));
  TEST_ASSERT_FALSE(jeton);
}

void test_probe_temporal_floor_is_1000_ms(void) {
  // T5 / CONDITION 3 DU GATE — plancher ABSOLU de 1 s, EN PLUS du jeton. Sans
  // lui, le backoff du WARN démarrant à 0 ms, son drain immédiat réarmerait le
  // jeton quelques millisecondes après la sonde précédente.
  const uint32_t itv = kOomProbeMinIntervalMs;
  TEST_ASSERT_EQUAL_UINT32(1000u, itv);
  // 1re sonde du boot : jamais bloquée (lastProbeMs vaut 0, l'instant du boot
  // ne doit pas décider de l'admission du tout premier échantillon).
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(0u, 0u, 0u, itv));
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(0u, 5u, 0u, itv));
  // Ensuite : refus strictement sous 1 000 ms, admission à 1 000 ms pile (>=).
  TEST_ASSERT_FALSE(oomProbeFloorElapsed(1u, 10000u, 10000u, itv));   // 0 ms
  TEST_ASSERT_FALSE(oomProbeFloorElapsed(1u, 10005u, 10000u, itv));   // 5 ms — le cas redouté
  TEST_ASSERT_FALSE(oomProbeFloorElapsed(1u, 10999u, 10000u, itv));   // 999 ms
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(1u, 11000u, 10000u, itv));    // pile
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(9u, 60000u, 10000u, itv));
  // Repliement de millis() (49,7 j) : arithmétique non signée, écart CORRECT et
  // non ~4,29 milliards. C'est le défaut qui a déjà mordu ce projet.
  TEST_ASSERT_FALSE(oomProbeFloorElapsed(1u, 100u, 0xFFFFFF00u, itv));  // 356 ms réels
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(1u, 1000u, 0xFFFFFF00u, itv));  // 1256 ms réels
}

void test_probe_admission_bounds_are_300_and_1500_us(void) {
  // T5 / CONDITION 1 DU GATE — WARN à 300 µs (sonde CONSERVÉE), désactivation
  // DÉFINITIVE au-delà de 1 500 µs. L'assiette de ces seuils est le COUPLE des
  // DEUX parcours (caps de l'échec + référence), pas chacun séparément : c'est
  // le total que subit le système interruptions coupées.
  TEST_ASSERT_EQUAL_UINT32(300u, kOomProbeWarnDurationUs);
  TEST_ASSERT_EQUAL_UINT32(1500u, kOomProbeMaxDurationUs);
  // Le seuil de WARN ne désactive PAS : il alerte.
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(0u, kOomProbeMaxDurationUs));
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(kOomProbeWarnDurationUs,
                                             kOomProbeMaxDurationUs));
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(kOomProbeWarnDurationUs + 1u,
                                             kOomProbeMaxDurationUs));
  // Plafond EXACT : encore admis (<=, inclusif) ; un µs de plus : refusé.
  TEST_ASSERT_TRUE(heapIntegrityAdmitNextRun(kOomProbeMaxDurationUs,
                                             kOomProbeMaxDurationUs));
  TEST_ASSERT_FALSE(heapIntegrityAdmitNextRun(kOomProbeMaxDurationUs + 1u,
                                              kOomProbeMaxDurationUs));
  TEST_ASSERT_FALSE(heapIntegrityAdmitNextRun(70000u, kOomProbeMaxDurationUs));
  // ORDRE DES SEUILS : un WARN doit toujours pouvoir précéder la désactivation,
  // et la sonde (chemin d'échec, rafales, interruptions coupées) doit rester un
  // ordre de grandeur sous le WARN du contrôle d'intégrité (loop, 5 ms).
  TEST_ASSERT_TRUE(kOomProbeWarnDurationUs < kOomProbeMaxDurationUs);
  TEST_ASSERT_TRUE(kOomProbeMaxDurationUs < kHeapIntegrityWarnDurationUs);
  TEST_ASSERT_TRUE(kOomProbeMaxDurationUs * 3u <= kHeapIntegrityWarnDurationUs);
}

void test_probe_region_scan_unless_mask_guarantees_internal_ram(void) {
  // T5 / CONDITION 2 DU GATE — sélection de branche. Sur 4 Mo de PSRAM, le
  // parcours O(blocs) n'a pas de pire cas connu ; on ne SAUTE pas la sonde
  // (interdit : les 12 sites d'appel du hook doivent tous rendre un verdict),
  // on bascule sur les agrégats O(régions).
  //
  // ═══ ATTENDUS INVERSÉS PAR LA REVUE DE B-2 — DÉFAUT B1, BLOQUANT ═══════════
  // LA VERSION PRÉCÉDENTE DE CE TEST VERROUILLAIT LE DÉFAUT. Elle affirmait
  // `TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x0000FBFFu))` — masque qui
  // CONTIENT 0x1000 — et épinglait `kOomProbeSpiramCapBit == 0x0400`. Elle
  // passait, en prouvant exactement l'inverse de ce qu'on croyait : que la
  // classe d'échec PRINCIPALE partait bien en parcours O(blocs).
  //
  // Ce que le désassemblage de `libheap.a` a établi (`heap_caps.c.obj`,
  // `.iram1.2`, `.literal+0x8 = 0x00001000`) : tout échec de `malloc()` ou
  // d'`operator new` ordinaire remonte au hook `caps = MALLOC_CAP_DEFAULT`
  // (0x1000), SANS bit SPIRAM. C'est le masque des crashs du 11/08 (bad_alloc à
  // l'accept HTTP) et du 13/08 (156 o) — ceux qui fondent tout ce chantier. Et
  // le tas PSRAM est enregistré `DEFAULT|SPIRAM|8BIT|32BIT` (c'est ce qui rend
  // opérant le heap_caps_malloc_extmem_enable() de B-1), donc `heap_caps_match()`
  // le sélectionne : `tlsf_walk_pool` aurait parcouru 4 Mo interruptions
  // coupées, et la sonde se serait DÉSACTIVÉE dès le premier `new` refusé.
  //
  // Le critère n'est donc plus « le masque demande-t-il de la PSRAM ? » (test de
  // PRÉSENCE, qui échoue en silence sur tout masque non énuméré) mais
  // « le masque GARANTIT-IL la RAM interne ? » — FAIL-SAFE : au moindre doute,
  // agrégats. D'où le renommage du test : il ne parle plus de « spiram caps ».
  TEST_ASSERT_EQUAL_HEX32(0x0800u, kOomProbeInternalCapBit);  // MALLOC_CAP_INTERNAL (1<<11)

  // (A) O(BLOCS) — et UNIQUEMENT ici : le masque porte MALLOC_CAP_INTERNAL, donc
  //     heap_caps_match() ne peut PAS sélectionner le tas PSRAM. Le cas motivant
  //     du lot est intact.
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x0000080Cu));  // 8BIT|DMA|INTERNAL — les 6 refus du 05/09
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x00000804u));  // 8BIT|INTERNAL — la RÉFÉRENCE de la sonde
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x00000800u));  // INTERNAL seul
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x00001804u));  // 8BIT|INTERNAL|DEFAULT
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0xFFFFFFFFu));  // tout, INTERNAL compris

  // (B) O(RÉGIONS) — LE CŒUR DU CORRECTIF. Ces masques atteignent la PSRAM.
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00001000u));   // ⇐ MALLOC_CAP_DEFAULT SEUL :
                                                           //   le masque de TOUT malloc/new refusé,
                                                           //   donc des crashs du 11/08 et du 13/08.
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00001400u));   // SPIRAM|DEFAULT
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00001404u));   // 8BIT|SPIRAM|DEFAULT (bascule B-1)
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00001004u));   // 8BIT|DEFAULT
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00000400u));   // SPIRAM seul
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00000404u));   // 8BIT|SPIRAM (relevé du 13/08)
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00000004u));   // 8BIT seul : ne garantit RIEN
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00000008u));   // DMA seul
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0u));            // masque vide → fail-safe
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x0000F7FFu));   // tout SAUF INTERNAL
  // PRÉCISION SUR L'ANCIEN TEST, pour ne pas raconter d'histoire : son assertion
  // `FALSE(0x0000FBFFu)` reste VRAIE ici, parce que 0xFBFF est « tout sauf
  // SPIRAM » et contient donc encore INTERNAL. Ce n'est pas cette assertion qui
  // était fausse — c'est l'ABSENCE de couverture de `0x1000` SEUL, le masque
  // réellement remonté par le hook, qui laissait passer le défaut.
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x0000FBFFu));  // contient INTERNAL

  // (C) ANTI-VACUITÉ / anti-retour en arrière : un prédicat qui testerait de
  //     nouveau la PRÉSENCE d'un bit (SPIRAM, ou n'importe quel autre) se
  //     trompe forcément sur au moins un de ces trois masques, car ils
  //     n'ont AUCUN bit en commun deux à deux hors INTERNAL.
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00001000u));   // DEFAULT sans SPIRAM → régions
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00000400u));   // SPIRAM sans DEFAULT → régions
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x00000804u));  // INTERNAL, ni l'un ni l'autre → blocs
  // Le bit voisin d'INTERNAL ne doit pas être confondu avec lui (mutation
  // 0x0800 → 0x0400 ou 0x1000 : elle meurt ici).
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00000400u));
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x00001000u));
}

void test_probe_region_scan_marks_largest_block_as_unknown(void) {
  // T5 — la contrepartie de la bascule : le champ « plus gros bloc » est marqué
  // NON RENSEIGNÉ, et cette sentinelle est exactement la valeur de saturation du
  // champ RTC (0xFFFF << 6). Aucun encodage spécial, aucune règle de décodage
  // neuve — mais un verdict qui sait ne pas conclure (cf. T3-h).
  TEST_ASSERT_EQUAL_UINT32(0xFFFFu << 6, kOomProbeBlockUnknown);
  TEST_ASSERT_EQUAL_UINT32(4194240u, kOomProbeBlockUnknown);
  uint8_t raw[kOomPostMortemBytes];
  OomPostMortemFields f = makeFieldsWithProbe();
  f.lastCaps = 0x00000404u;               // 8BIT|SPIRAM → parcours O(régions)
  f.probeCaps = 0x0404u;
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(f.lastCaps));
  f.probeCapsMaxB = kOomProbeBlockUnknown;
  f.probeCapsFree = 2u * 1024u * 1024u;   // 2 Mo de PSRAM libres, agrégat
  oomPostMortemEncode(raw, f);
  OomPostMortemFields o = oomPostMortemDecode(raw);
  TEST_ASSERT_EQUAL_UINT32(kOomProbeBlockUnknown, o.probeCapsMaxB);
  TEST_ASSERT_EQUAL_UINT32(2u * 1024u * 1024u, o.probeCapsFree);
}

void test_probe_full_bounded_sequence_of_an_episode(void) {
  // T5 — SCÉNARIO COMPLET du bornage, tel que le vit un épisode réel (les
  // 6 refus du 05/09 en 21 min). Les prédicats sont enchaînés dans l'ORDRE
  // IMPÉRATIF de la coquille : désactivation → ISR → plancher → jeton.
  bool active = true;      // sonde pas encore désactivée
  bool jeton = true;       // armé par le 1er oomGuardLoopTick()
  uint32_t sondes = 0, derniereMs = 0;
  uint32_t nowMs = 500000u;

  // Refus n°1 : tout est ouvert → sonde prise.
  TEST_ASSERT_TRUE(active);
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(sondes, nowMs, derniereMs, kOomProbeMinIntervalMs));
  TEST_ASSERT_TRUE(oomProbeTakeTokenModel(jeton));
  sondes++; derniereMs = nowMs;
  active = heapIntegrityAdmitNextRun(180u, kOomProbeMaxDurationUs);  // 180 µs : sain
  TEST_ASSERT_TRUE(active);

  // Refus n°2 à 6, dans la même seconde : le PLANCHER les refuse, et surtout il
  // les refuse AVANT le jeton — qui reste donc désarmé, sans être « regagné ».
  for (int i = 0; i < 5; i++) {
    nowMs += 50u;
    TEST_ASSERT_FALSE(oomProbeFloorElapsed(sondes, nowMs, derniereMs, kOomProbeMinIntervalMs));
  }
  // Une seconde plus tard, le plancher s'ouvre — mais le JETON, lui, est vide :
  // les deux gardes sont bien INDÉPENDANTES et cumulatives.
  nowMs = derniereMs + kOomProbeMinIntervalMs;
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(sondes, nowMs, derniereMs, kOomProbeMinIntervalMs));
  TEST_ASSERT_FALSE(oomProbeTakeTokenModel(jeton));

  // La loop draine le WARN d'échec → réarmement du jeton (backoff : 30 s après).
  nowMs += 30000u;
  jeton = true;
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(sondes, nowMs, derniereMs, kOomProbeMinIntervalMs));
  TEST_ASSERT_TRUE(oomProbeTakeTokenModel(jeton));
  sondes++; derniereMs = nowMs;

  // Cet échantillon-là dépasse le plafond → DÉSACTIVATION DÉFINITIVE : plus
  // aucune sonde ensuite, même jeton réarmé et plancher largement écoulé.
  active = heapIntegrityAdmitNextRun(kOomProbeMaxDurationUs + 1u, kOomProbeMaxDurationUs);
  TEST_ASSERT_FALSE(active);
  jeton = true;
  nowMs += 3600000u;
  TEST_ASSERT_TRUE(oomProbeFloorElapsed(sondes, nowMs, derniereMs, kOomProbeMinIntervalMs));
  TEST_ASSERT_TRUE(jeton);
  TEST_ASSERT_FALSE(active);  // la garde de tête ferme tout : c'est définitif
  TEST_ASSERT_EQUAL_UINT32(2u, sondes);
}

void test_probe_real_constant_values(void) {
  // T5 — verrouillage des valeurs validées par le gate. Un futur commit qui les
  // change sans repasser par pool-chemistry casse ici.
  TEST_ASSERT_EQUAL_UINT32(300u, kOomProbeWarnDurationUs);
  TEST_ASSERT_EQUAL_UINT32(1500u, kOomProbeMaxDurationUs);
  TEST_ASSERT_EQUAL_UINT32(1000u, kOomProbeMinIntervalMs);
  TEST_ASSERT_EQUAL_UINT32(240u, kOomProbeDefaultCpuMhz);
  // Défaut B1 de la revue : la constante épinglée ici était kOomProbeSpiramCapBit
  // (0x0400), c'est-à-dire un invariant qui n'était PAS celui qui protège. Le
  // critère est désormais MALLOC_CAP_INTERNAL (1<<11) et le prédicat est
  // FAIL-SAFE (absence du bit ⇒ agrégats), ce qui couvre MALLOC_CAP_DEFAULT
  // (0x1000) — le masque de tout malloc/new refusé.
  TEST_ASSERT_EQUAL_HEX32(0x0800u, kOomProbeInternalCapBit);
  TEST_ASSERT_TRUE(oomProbeUsesRegionScan(0x1000u));   // le cas qui a défait la condition 2
  TEST_ASSERT_FALSE(oomProbeUsesRegionScan(0x080Cu));  // ...sans perdre le cas motivant
  TEST_ASSERT_EQUAL_UINT32(4194240u, kOomProbeBlockUnknown);
  // Drapeaux de la sonde : bits 5, 6, 7 — l'octet est SATURÉ (8/8) depuis B-2,
  // tout drapeau futur exigera un mot de plus ET un nouveau bump de magic.
  TEST_ASSERT_EQUAL_HEX8(0x20, kOomPmFlagProbeTaken);
  TEST_ASSERT_EQUAL_HEX8(0x40, kOomPmFlagProbeIsrSkipped);
  TEST_ASSERT_EQUAL_HEX8(0x80, kOomPmFlagProbeDisabled);
  const uint8_t tous = kOomPmFlagNominalUsed | kOomPmFlagFallbackUsed |
                       kOomPmFlagHandlerUnregistered | kOomPmFlagIntegrityFailed |
                       kOomPmFlagIntegrityNeverRun | kOomPmFlagProbeTaken |
                       kOomPmFlagProbeIsrSkipped | kOomPmFlagProbeDisabled;
  TEST_ASSERT_EQUAL_HEX8(0xFF, tous);  // aucun bit libre, aucun bit en double
  // Budget RAM RTC noinit du projet : 9 (auto-heal) + 1032 (anneau) + 48.
  TEST_ASSERT_EQUAL_UINT32(1089u, 9u + 1032u + kOomPostMortemBytes);
  TEST_ASSERT_TRUE(9u + 1032u + kOomPostMortemBytes < 8192u);
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
  // v2.35.0 (bug-080 B-1) — arithmétique du seuil de bascule PSRAM
  RUN_TEST(test_psram_extmem_threshold_arithmetic_v2350);
  RUN_TEST(test_rearm_fallback_tier_respects_chemistry_gate_invariants_v2202);
  RUN_TEST(test_rearm_arms_fallback_across_whole_measured_8bit_regime);
  RUN_TEST(test_rearm_never_downgrades_nominal_in_8bit_regime);
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
  RUN_TEST(test_postmortem_wire_format_is_little_endian_48_bytes);
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
  RUN_TEST(test_verdict_real_case_20260813_needs_a_probe_to_accuse);
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

  RUN_TEST(test_register_handler_refused_when_reserve_absent);
  RUN_TEST(test_register_handler_when_reserve_armed_and_handler_absent);
  RUN_TEST(test_register_handler_is_idempotent);
  RUN_TEST(test_register_handler_full_scenario_of_20260815_bug);

  RUN_TEST(test_fail_warn_backoff_first_emission_uses_initial_delay);
  RUN_TEST(test_fail_warn_backoff_escalates_30s_5min_30min);
  RUN_TEST(test_fail_warn_backoff_saturates_at_last_tier);
  RUN_TEST(test_fail_warn_backoff_sequence_from_scratch_is_monotonic);
  RUN_TEST(test_fail_warn_backoff_reset_restarts_at_initial_tier);
  RUN_TEST(test_fail_warn_backoff_real_constant_values);

  RUN_TEST(test_count_failure_true_for_genuine_failure);
  RUN_TEST(test_count_failure_false_while_self_alloc_in_flight);
  RUN_TEST(test_count_failure_window_is_closed_after_self_alloc);
  RUN_TEST(test_count_failure_suppresses_the_1025_self_inflicted_of_20260815);

  // bug-080 B-2 — sonde du refus : aller-retour RTC des 4 mots neufs (T1)
  RUN_TEST(test_probe_roundtrip_preserves_all_four_new_words);
  RUN_TEST(test_probe_wire_offsets_are_w8_to_w11);
  RUN_TEST(test_probe_size_is_exact_never_quantized);
  RUN_TEST(test_probe_quantization_is_bounded_and_pessimistic);
  RUN_TEST(test_probe_blocks_saturate_above_4_mib);
  RUN_TEST(test_probe_duration_saturates_at_fffe_reserving_ffff);
  RUN_TEST(test_probe_absent_zone_decodes_to_zeroed_probe);
  // bug-080 B-2 — l'ANCIEN magic doit être jugé invalide (T2)
  RUN_TEST(test_old_magic_f00dfa12_is_rejected);
  RUN_TEST(test_old_32_byte_zone_followed_by_rtc_garbage_is_rejected);
  // bug-080 B-2 — verdict révisé, les 7 cas de la règle (T3)
  RUN_TEST(test_verdict_probe_dma_starvation_is_the_new_case);
  RUN_TEST(test_verdict_probe_general_starvation_when_reference_also_dry);
  RUN_TEST(test_verdict_probe_corruption_is_now_the_only_path);
  RUN_TEST(test_verdict_probe_not_representative_falls_back_to_rule_3);
  RUN_TEST(test_verdict_no_probe_with_healthy_ring_is_undetermined);
  RUN_TEST(test_verdict_no_probe_burst_is_still_starvation);
  RUN_TEST(test_verdict_zero_failcount_stays_undetermined_even_with_a_probe);
  RUN_TEST(test_verdict_probe_region_scan_marks_block_unknown);
  // bug-080 B-2 / revue — défaut B2 : le drapeau tardif non flushé en RAM RTC
  RUN_TEST(test_b2_probe_words_written_but_flag_lost_discards_the_measure);
  RUN_TEST(test_b2_flush_mask_preserves_ring_uptime_and_release_count);
  RUN_TEST(test_b2_late_flags_isr_and_disabled_also_reach_the_zone);
  // bug-080 B-2 — TEST-PREUVE : relevé terrain du 05/09/2026 (T4)
  RUN_TEST(test_field_20260905_without_probe_is_no_longer_corruption);
  RUN_TEST(test_field_20260905_with_probe_is_dma_starvation);
  // bug-080 B-2 — prédicats de bornage de la sonde (T5, ADR-0017)
  RUN_TEST(test_probe_token_is_one_sample_per_episode);
  RUN_TEST(test_probe_temporal_floor_is_1000_ms);
  RUN_TEST(test_probe_admission_bounds_are_300_and_1500_us);
  RUN_TEST(test_probe_region_scan_unless_mask_guarantees_internal_ram);
  RUN_TEST(test_probe_region_scan_marks_largest_block_as_unknown);
  RUN_TEST(test_probe_full_bounded_sequence_of_an_episode);
  RUN_TEST(test_probe_real_constant_values);
  return UNITY_END();
}
