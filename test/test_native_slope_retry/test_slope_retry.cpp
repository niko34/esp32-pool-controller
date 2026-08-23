// =============================================================================
// Tests unitaires natifs — Retentative Slope,? de boot (slope_retry_logic,
// bug-slope-boot-perdue-et-data-incomplet, v2.21.2)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / I²C.
// On teste le COMPORTEMENT observable de la fonction PURE
// src/slope_retry_logic.h::shouldRetryBootSlope() (ADR-0017, comme ph_offset).
//
// Condition C3 du gate pool-chemistry (GO SOUS CONDITIONS) : la logique de
// retentative doit être couverte par des tests natifs, en particulier :
//   - C1 : borne DURE de kPhSlopeBootRetryMaxAttempts transactions (chaque
//     Slope,? bloque loopTask ~900 ms en I²C synchrone) ;
//   - C2 : jamais de retry sans module EZO pH détecté (bus muet) ;
//   - anti-underflow : nowMs < lastAttemptMs ne doit JAMAIS autoriser un retry
//     (une soustraction non signée donnerait ~4,3 milliards >= intervalle).
//
// Chaîne de production reproduite (src/sensors.cpp, étape 5bis d'update()) :
//   shouldRetryBootSlope(_phSlopeQueriedMs, _phSlopeQueryPending, _phEzoDetected,
//                        _phSlopeBootAttempts, kPhSlopeBootRetryMaxAttempts,
//                        _phSlopeBootLastAttemptMs, nowAfterQueue,
//                        kPhSlopeBootRetryIntervalMs)
// L'appelant incrémente _phSlopeBootAttempts AVANT l'enqueue, même si la file
// EZO est pleine (tentative décomptée) — vérifié par lecture de sensors.cpp,
// non testable en pur ici (SensorManager n'est pas pur).
// =============================================================================

#include <unity.h>
#include <stdint.h>

#include "slope_retry_logic.h"
#include "constants.h"  // kPhSlopeBootRetryIntervalMs / kPhSlopeBootRetryMaxAttempts (self-contained)

void setUp(void) {}
void tearDown(void) {}

// Raccourcis lisibles pour les paramètres booléens.
static const bool kPending = true;
static const bool kNotPending = false;
static const bool kEzoOk = true;
static const bool kEzoAbsent = false;

// Valeurs de production (verrouillées par test_retry_constants_locked).
static const uint32_t kInterval = 30000;  // kPhSlopeBootRetryIntervalMs
static const uint8_t kMax = 5;            // kPhSlopeBootRetryMaxAttempts

// =============================================================================
// (a) Pente déjà lue (queriedMs != 0) : le cycle 24 h prend le relais, la
//     retentative de boot ne doit JAMAIS se déclencher — même si toutes les
//     autres conditions sont favorables (y compris la 1ʳᵉ tentative immédiate).
// =============================================================================

void test_no_retry_when_slope_already_read(void) {
  // Cas favorable par ailleurs : 1ʳᵉ tentative, module détecté, rien en file.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(/*queriedMs*/ 1, kNotPending, kEzoOk,
                                         /*attempts*/ 0, kMax,
                                         /*lastAttemptMs*/ 0, /*nowMs*/ 100000, kInterval));
  // Timestamp de réussite tardif + intervalle largement écoulé : toujours non.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(/*queriedMs*/ 500000, kNotPending, kEzoOk,
                                         /*attempts*/ 2, kMax,
                                         /*lastAttemptMs*/ 10000, /*nowMs*/ 900000, kInterval));
  // queriedMs = UINT32_MAX (valeur extrême, toujours "déjà lu") : non.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(UINT32_MAX, kNotPending, kEzoOk,
                                         0, kMax, 0, 100000, kInterval));
}

// =============================================================================
// (b) Query déjà en file (pending) : anti-doublon strict — on ne ré-enfile pas
//     tant que la précédente n'a pas été traitée (succès OU échec).
// =============================================================================

void test_no_retry_when_query_pending(void) {
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kPending, kEzoOk,
                                         0, kMax, 0, 100000, kInterval));
  // Même avec l'intervalle très largement écoulé depuis la dernière tentative.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kPending, kEzoOk,
                                         3, kMax, 10000, 10000000, kInterval));
}

// =============================================================================
// (c) Module EZO pH non détecté au boot (gate C2 pool-chemistry) : JAMAIS de
//     retry — sinon jusqu'à 5 × ~900 ms de blocage loopTask sur un bus muet.
// =============================================================================

void test_no_retry_when_ezo_not_detected(void) {
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoAbsent,
                                         0, kMax, 0, 100000, kInterval));
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoAbsent,
                                         2, kMax, 10000, 10000000, kInterval));
}

// =============================================================================
// (d) Borne DURE (gate C1 pool-chemistry) : attempts >= maxAttempts → JAMAIS,
//     même si tout le reste est favorable et quel que soit le temps écoulé.
//     Le compteur n'étant JAMAIS réarmé côté appelant, ce refus est définitif.
// =============================================================================

void test_no_retry_when_attempts_exhausted(void) {
  // Exactement à la borne.
  TEST_ASSERT_FALSE_MESSAGE(
      shouldRetryBootSlope(0, kNotPending, kEzoOk, kMax, kMax,
                           /*lastAttemptMs*/ 10000, /*nowMs*/ 10000000, kInterval),
      "attempts == maxAttempts doit refuser (borne dure C1)");
  // Au-delà de la borne (défense en profondeur si le compteur dérivait).
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                         (uint8_t)(kMax + 1), kMax,
                                         10000, 10000000, kInterval));
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                         255, kMax, 10000, 10000000, kInterval));
}

// La dernière tentative AUTORISÉE est attempts == maxAttempts − 1 : la borne
// donne bien exactement maxAttempts transactions au total (l'appelant
// incrémente APRÈS la décision).
void test_last_allowed_attempt_is_max_minus_one(void) {
  TEST_ASSERT_TRUE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                        (uint8_t)(kMax - 1), kMax,
                                        /*lastAttemptMs*/ 10000,
                                        /*nowMs*/ 10000 + kInterval, kInterval));
}

// maxAttempts == 0 (configuration "retry désactivé") : aucune tentative, même
// la première — 0 est bien une borne, pas un "illimité".
void test_max_attempts_zero_disables_retry_entirely(void) {
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                         0, /*maxAttempts*/ 0, 0, 100000, kInterval));
}

// =============================================================================
// (e) Première tentative (attempts == 0, lastAttemptMs == 0) : retry IMMÉDIAT
//     autorisé — le module vient probablement de finir son init, inutile
//     d'attendre 30 s pour compenser un échec du begin().
// =============================================================================

void test_first_attempt_is_immediate(void) {
  // Dès le premier tick après le boot (nowMs quelconque, même petit).
  TEST_ASSERT_TRUE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                        0, kMax, 0, /*nowMs*/ 1, kInterval));
  TEST_ASSERT_TRUE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                        0, kMax, 0, /*nowMs*/ 0, kInterval));
  TEST_ASSERT_TRUE(shouldRetryBootSlope(0, kNotPending, kEzoOk,
                                        0, kMax, 0, /*nowMs*/ 123456, kInterval));
}

// L'immédiateté de la 1ʳᵉ tentative ne court-circuite AUCUN des gates amont :
// (a), (b), (c) priment (déjà couverts ci-dessus avec attempts == 0).

// =============================================================================
// (f) Cadencement : intervalle STRICT de 30 s entre retentatives —
//     refus à 29 999 ms d'écart, accord à 30 000 ms (>= inclusif).
// =============================================================================

void test_interval_refused_one_ms_before(void) {
  TEST_ASSERT_FALSE_MESSAGE(
      shouldRetryBootSlope(0, kNotPending, kEzoOk, /*attempts*/ 1, kMax,
                           /*lastAttemptMs*/ 100000,
                           /*nowMs*/ 100000 + kInterval - 1, kInterval),
      "29 999 ms d'ecart => refus (cadence 30 s stricte)");
}

void test_interval_granted_at_exact_boundary(void) {
  TEST_ASSERT_TRUE_MESSAGE(
      shouldRetryBootSlope(0, kNotPending, kEzoOk, /*attempts*/ 1, kMax,
                           /*lastAttemptMs*/ 100000,
                           /*nowMs*/ 100000 + kInterval, kInterval),
      "30 000 ms d'ecart exactement => accord (>= inclusif)");
  // Et bien sûr au-delà.
  TEST_ASSERT_TRUE(shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                                        100000, 100000 + kInterval + 1, kInterval));
}

// Écart nul (même tick) : refusé — deux tentatives ne peuvent pas partir dans
// la même passe d'update().
void test_interval_refused_at_zero_elapsed(void) {
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                                         /*lastAttemptMs*/ 100000,
                                         /*nowMs*/ 100000, kInterval));
}

// =============================================================================
// (g) ANTI-UNDERFLOW (critère de sécurité) : nowMs < lastAttemptMs — timestamp
//     posé APRÈS l'échantillonnage de nowMs (cf. bug historique du cycle 24 h,
//     _processEzoQueue() bloque ~900 ms) ou wrap de millis(). La soustraction
//     non signée donnerait ~4,3 milliards >= intervalle et validerait à tort :
//     la garde doit refuser, TOUJOURS.
// =============================================================================

void test_underflow_guard_refuses_when_now_before_last_attempt(void) {
  // Cas historique : lastAttemptMs = nowMs + 900 (timestamp posé après le
  // blocage I²C). Sans garde : (nowMs − lastAttemptMs) mod 2³² ≈ 4,29 Md → true.
  TEST_ASSERT_FALSE_MESSAGE(
      shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                           /*lastAttemptMs*/ 100900, /*nowMs*/ 100000, kInterval),
      "nowMs < lastAttemptMs ne doit JAMAIS autoriser (anti-underflow)");
  // Écart minimal (1 ms d'incohérence) : refus aussi.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                                         /*lastAttemptMs*/ 100001,
                                         /*nowMs*/ 100000, kInterval));
  // Écart maximal (lastAttemptMs = UINT32_MAX, nowMs = 0) : refus.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                                         UINT32_MAX, 0, kInterval));
}

// Vérification de l'invariant sur un balayage systématique : pour TOUT couple
// (lastAttemptMs, nowMs) avec nowMs < lastAttemptMs et attempts >= 1, le
// résultat est false — jamais un underflow qui autoriserait.
void test_underflow_guard_holds_across_sweep(void) {
  const uint32_t lasts[] = {1, 1000, 30000, 100000, 2147483648u, UINT32_MAX - 1, UINT32_MAX};
  for (unsigned i = 0; i < sizeof(lasts) / sizeof(lasts[0]); ++i) {
    const uint32_t last = lasts[i];
    const uint32_t nows[] = {0, last / 2, last - 1};
    for (unsigned j = 0; j < sizeof(nows) / sizeof(nows[0]); ++j) {
      if (nows[j] >= last) continue;  // on ne teste que nowMs < lastAttemptMs
      TEST_ASSERT_FALSE_MESSAGE(
          shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                               last, nows[j], kInterval),
          "balayage anti-underflow : tout nowMs < lastAttemptMs doit refuser");
    }
  }
}

// =============================================================================
// (h) Wrap millis() LÉGITIME — comportement défini et DOCUMENTÉ.
// =============================================================================
// Scénario : lastAttemptMs proche de UINT32_MAX, millis() wrappe, nowMs petit,
// écart RÉEL (arithmétique modulaire) >= intervalle. L'arithmétique modulaire
// pure (nowMs − lastAttemptMs mod 2³²) donnerait le bon écart et AUTORISERAIT.
//
// CHOIX DE L'IMPLÉMENTATION (vérifié ici) : la garde anti-underflow REFUSE.
// C'est un choix CONSERVATEUR délibéré : la fonction ne peut pas distinguer un
// wrap légitime d'un timestamp incohérent (posé après nowMs), et le refus est
// le côté sûr (pas de transaction I²C de 900 ms indue). Coût réel : NUL en
// pratique — cette logique ne vit que dans la fenêtre de boot (5 tentatives
// espacées de 30 s, soit ~2,5 min après démarrage), alors que le wrap de
// millis() survient à ~49,7 jours d'uptime : lastAttemptMs ne peut pas être
// proche de UINT32_MAX pendant la fenêtre de boot. Si un futur refactor rendait
// ce chemin actif longtemps après le boot, ce test force à re-décider.
void test_legit_millis_wrap_is_refused_conservative_choice(void) {
  const uint32_t last = UINT32_MAX - 1000;  // tentative juste avant le wrap
  const uint32_t now = 29001;               // écart modulaire réel = 30 002 ms >= 30 000
  // Preuve que le cas est bien "légitime" au sens modulaire :
  TEST_ASSERT_TRUE((uint32_t)(now - last) >= kInterval);
  // ... et pourtant la garde refuse (nowMs < lastAttemptMs) :
  TEST_ASSERT_FALSE_MESSAGE(
      shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax, last, now, kInterval),
      "wrap legitime => refus assume (garde conservatrice, cf. commentaire)");
  // Cas limite : écart modulaire exactement égal à l'intervalle → refus aussi.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(0, kNotPending, kEzoOk, 1, kMax,
                                         UINT32_MAX - 14999, 15000, kInterval));
}

// =============================================================================
// Scénario intégré : vie nominale de la retentative de boot, du 1ᵉʳ tick à
// l'épuisement, puis vérification qu'une réussite tardive éteint tout.
// =============================================================================

void test_full_boot_retry_lifecycle(void) {
  uint32_t queriedMs = 0;      // aucune pente jamais lue
  uint8_t attempts = 0;        // état initial (sensors.h)
  uint32_t lastAttemptMs = 0;
  uint32_t now = 5000;         // ~5 s après le boot

  // Exactement kMax tentatives sont accordées, espacées de kInterval.
  for (uint8_t expected = 1; expected <= kMax; ++expected) {
    TEST_ASSERT_TRUE_MESSAGE(
        shouldRetryBootSlope(queriedMs, kNotPending, kEzoOk, attempts, kMax,
                             lastAttemptMs, now, kInterval),
        "chaque tentative de 1 a maxAttempts doit etre accordee a l'echeance");
    // Reproduit l'appelant (sensors.cpp) : décompte AVANT l'enqueue.
    attempts++;
    lastAttemptMs = now;
    TEST_ASSERT_EQUAL_UINT8(expected, attempts);
    // Entre deux échéances, AUCUNE tentative supplémentaire (balayage fin).
    for (uint32_t dt = 0; dt < kInterval; dt += 7499) {
      TEST_ASSERT_FALSE(shouldRetryBootSlope(queriedMs, kNotPending, kEzoOk,
                                             attempts, kMax, lastAttemptMs,
                                             now + dt, kInterval));
    }
    now += kInterval;
  }

  // 6ᵉ échéance et suivantes : borne dure, plus JAMAIS de tentative.
  for (int i = 0; i < 10; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(
        shouldRetryBootSlope(queriedMs, kNotPending, kEzoOk, attempts, kMax,
                             lastAttemptMs, now, kInterval),
        "au-dela de maxAttempts : plus jamais de retry (C1)");
    now += kInterval;
  }

  // Variante : si une tentative REUSSIT en cours de route (queriedMs posé),
  // tout s'arrête immédiatement, même avec des tentatives restantes.
  TEST_ASSERT_FALSE(shouldRetryBootSlope(/*queriedMs*/ now, kNotPending, kEzoOk,
                                         /*attempts*/ 2, kMax,
                                         /*lastAttemptMs*/ 60000,
                                         /*nowMs*/ now + kInterval, kInterval));
}

// =============================================================================
// Verrou de constantes (arbitrées avec pool-chemistry) : 5 × 30 s. Tout
// changement doit casser ce test et repasser par pool-chemistry (chaque
// tentative = ~900 ms de blocage loopTask en I²C synchrone).
// =============================================================================

void test_retry_constants_locked(void) {
  TEST_ASSERT_EQUAL_UINT32(30000UL, kPhSlopeBootRetryIntervalMs);
  TEST_ASSERT_EQUAL_UINT8(5, kPhSlopeBootRetryMaxAttempts);
  // Fenêtre totale de retentatives (~2,5 min) très inférieure au wrap millis
  // (~49,7 j) : la garde conservatrice du cas (h) est sans coût en production.
  TEST_ASSERT_TRUE((uint64_t)kPhSlopeBootRetryMaxAttempts * kPhSlopeBootRetryIntervalMs
                   < 4294967295ULL);
}

int main(int, char**) {
  UNITY_BEGIN();
  // (a) pente déjà lue
  RUN_TEST(test_no_retry_when_slope_already_read);
  // (b) anti-doublon
  RUN_TEST(test_no_retry_when_query_pending);
  // (c) gate C2 — module absent
  RUN_TEST(test_no_retry_when_ezo_not_detected);
  // (d) gate C1 — borne dure
  RUN_TEST(test_no_retry_when_attempts_exhausted);
  RUN_TEST(test_last_allowed_attempt_is_max_minus_one);
  RUN_TEST(test_max_attempts_zero_disables_retry_entirely);
  // (e) première tentative immédiate
  RUN_TEST(test_first_attempt_is_immediate);
  // (f) cadencement 30 s strict
  RUN_TEST(test_interval_refused_one_ms_before);
  RUN_TEST(test_interval_granted_at_exact_boundary);
  RUN_TEST(test_interval_refused_at_zero_elapsed);
  // (g) anti-underflow
  RUN_TEST(test_underflow_guard_refuses_when_now_before_last_attempt);
  RUN_TEST(test_underflow_guard_holds_across_sweep);
  // (h) wrap légitime — choix conservateur documenté
  RUN_TEST(test_legit_millis_wrap_is_refused_conservative_choice);
  // Scénario intégré + verrou de constantes
  RUN_TEST(test_full_boot_retry_lifecycle);
  RUN_TEST(test_retry_constants_locked);
  return UNITY_END();
}
