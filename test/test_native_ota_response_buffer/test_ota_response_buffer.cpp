// =============================================================================
// Tests unitaires natifs — Lecture bornée du corps de /check-update dans un
// tampon PSRAM (bug-173, ADR-0032 révisé par ADR-0112)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur
// src/ota_response_buffer_logic.h (ADR-0017) :
//   - otaBufferedReadNextAction : la décision prise à CHAQUE itération de la
//     boucle de lecture — LE TEST QUI COMPTE LE PLUS est que
//     "connexion fermée" SEUL ne suffit PAS à conclure à la fin du flux tant
//     que des octets restent immédiatement disponibles (`dataAvailable`) :
//     c'est très exactement le défaut corrigé par ce bug (un lecteur qui
//     traite une absence TRANSITOIRE de donnée comme une fin de document).
//   - otaBufferedReadLengthMismatch : le content-length déclaré est une
//     entrée TIERCE, jamais une confiance aveugle.
//   - otaBufferedReadResultFor : combine les deux pour produire un verdict
//     qui ne confond JAMAIS "trop gros", "incomplet" et "timeout" — un refus
//     muet ou ambigu est le travers récurrent de ce projet.
// =============================================================================

#include <unity.h>
#include "ota_response_buffer_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. otaBufferedReadNextAction — décision à chaque itération
// ---------------------------------------------------------------------------

void test_action_continues_while_connection_open_and_room_left(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kContinue),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/100, /*maxBytes=*/1000,
                         /*connectionOpen=*/true, /*dataAvailable=*/false,
                         /*deadlineExceeded=*/false)));
}

void test_action_stops_too_large_at_exact_bound(void) {
  // Borne atteinte EXACTEMENT (>=) : refus, même si la connexion reste ouverte
  // et que l'échéance tient — la borne de taille prime.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kStopTooLarge),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/1000, /*maxBytes=*/1000,
                         /*connectionOpen=*/true, /*dataAvailable=*/true,
                         /*deadlineExceeded=*/false)));
}

void test_action_too_large_wins_over_deadline(void) {
  // Les deux conditions d'arrêt sont vraies : la taille doit être testée EN
  // PREMIER (ordre documenté dans le header) — sinon ce test meurt si
  // l'implémentation inverse l'ordre.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kStopTooLarge),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/1000, /*maxBytes=*/1000,
                         /*connectionOpen=*/true, /*dataAvailable=*/true,
                         /*deadlineExceeded=*/true)));
}

void test_action_stops_timed_out_before_connection_check(void) {
  // Échéance dépassée : arrêt immédiat, MÊME si la connexion est encore
  // ouverte et des données disponibles (une connexion qui traîne doit être
  // coupée).
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kStopTimedOut),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/10, /*maxBytes=*/1000,
                         /*connectionOpen=*/true, /*dataAvailable=*/true,
                         /*deadlineExceeded=*/true)));
}

// ⚠️ LE TEST QUI COMPTE LE PLUS (bug-173) : une connexion fermée NE SUFFIT
// PAS, à elle seule, à arrêter la lecture tant que des octets restent
// immédiatement disponibles. C'est le symptôme exact du défaut corrigé :
// traiter une absence (ici simulée par connectionOpen=false, mais le vrai bug
// était un read() à -1) comme une fin de document alors que des données
// restaient à lire.
void test_action_continues_if_data_still_available_despite_closed_connection(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kContinue),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/10, /*maxBytes=*/1000,
                         /*connectionOpen=*/false, /*dataAvailable=*/true,
                         /*deadlineExceeded=*/false)));
}

// Et le miroir : connexion ouverte, mais rien d'immédiatement disponible
// ⇒ on doit POUVOIR continuer (une fenêtre de tampon réseau transitoirement
// vide n'est pas une fin de flux) — distinct du cas d'arrêt normal ci-dessous
// où les DEUX signaux sont négatifs à la fois.
void test_action_continues_if_connection_open_but_no_data_right_now(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kContinue),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/10, /*maxBytes=*/1000,
                         /*connectionOpen=*/true, /*dataAvailable=*/false,
                         /*deadlineExceeded=*/false)));
}

void test_action_stops_ok_when_closed_and_nothing_left(void) {
  // Fin NORMALE du flux : connexion fermée ET plus rien d'immédiatement
  // disponible — les DEUX conditions, pas une seule.
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kStopOk),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/500, /*maxBytes=*/1000,
                         /*connectionOpen=*/false, /*dataAvailable=*/false,
                         /*deadlineExceeded=*/false)));
}

void test_action_below_bound_strictly_continues(void) {
  // bytesReadSoFar == maxBytes - 1 : encore de la place, pas d'arrêt forcé
  // (même avec connexion ouverte et donnée disponible → kContinue, pas
  // kStopTooLarge).
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadAction::kContinue),
                     static_cast<int>(otaBufferedReadNextAction(
                         /*bytesReadSoFar=*/999, /*maxBytes=*/1000,
                         /*connectionOpen=*/true, /*dataAvailable=*/true,
                         /*deadlineExceeded=*/false)));
}

// ---------------------------------------------------------------------------
// 2. otaBufferedReadLengthMismatch — content-length, entrée TIERCE
// ---------------------------------------------------------------------------

void test_mismatch_false_when_exact_match(void) {
  TEST_ASSERT_FALSE(otaBufferedReadLengthMismatch(16756, 16756));
}

void test_mismatch_true_when_fewer_bytes_than_declared(void) {
  // Le cas EXACT du bug : le pair a fermé avant la fin annoncée.
  TEST_ASSERT_TRUE(otaBufferedReadLengthMismatch(12000, 16756));
}

void test_mismatch_true_when_more_bytes_than_declared(void) {
  // Pas de confiance aveugle dans l'autre sens non plus : plus d'octets que
  // ce que le serveur avait annoncé est tout aussi anormal.
  TEST_ASSERT_TRUE(otaBufferedReadLengthMismatch(20000, 16756));
}

void test_mismatch_false_when_declared_length_unknown_zero(void) {
  // declaredContentLength <= 0 : absent/non exploitable — aucune vérification
  // possible, ce n'est PAS une anomalie en soi.
  TEST_ASSERT_FALSE(otaBufferedReadLengthMismatch(5000, 0));
}

void test_mismatch_false_when_declared_length_negative(void) {
  TEST_ASSERT_FALSE(otaBufferedReadLengthMismatch(5000, -1));
}

// ---------------------------------------------------------------------------
// 3. otaBufferedReadResultFor — verdict final, jamais ambigu
// ---------------------------------------------------------------------------

void test_result_ok_when_stop_ok_and_no_mismatch(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadResult::kOk),
                     static_cast<int>(otaBufferedReadResultFor(
                         OtaBufferedReadAction::kStopOk, /*lengthMismatch=*/false)));
}

// ⚠️ LE TEST QUI TUE LA RÉGRESSION DE CE BUG : un arrêt "normal" (kStopOk)
// dont le compte d'octets ne correspond PAS au content-length annoncé ne doit
// JAMAIS produire kOk — l'ancien défaut acceptait silencieusement un document
// tronqué. Sans cette règle, le document partiel serait parsé comme valide.
void test_result_incomplete_when_stop_ok_but_length_mismatch(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadResult::kIncomplete),
                     static_cast<int>(otaBufferedReadResultFor(
                         OtaBufferedReadAction::kStopOk, /*lengthMismatch=*/true)));
}

void test_result_too_large_regardless_of_mismatch(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadResult::kTooLarge),
                     static_cast<int>(otaBufferedReadResultFor(
                         OtaBufferedReadAction::kStopTooLarge, /*lengthMismatch=*/false)));
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadResult::kTooLarge),
                     static_cast<int>(otaBufferedReadResultFor(
                         OtaBufferedReadAction::kStopTooLarge, /*lengthMismatch=*/true)));
}

void test_result_timed_out_regardless_of_mismatch(void) {
  TEST_ASSERT_EQUAL(static_cast<int>(OtaBufferedReadResult::kTimedOut),
                     static_cast<int>(otaBufferedReadResultFor(
                         OtaBufferedReadAction::kStopTimedOut, /*lengthMismatch=*/false)));
}

// Les trois verdicts d'échec doivent être MUTUELLEMENT DISTINCTS — un refus
// ambigu (deux causes retombant sur la même valeur) serait le travers nommé
// par la spec de ce bug.
void test_result_three_failure_causes_are_pairwise_distinct(void) {
  const int tooLarge = static_cast<int>(
      otaBufferedReadResultFor(OtaBufferedReadAction::kStopTooLarge, false));
  const int timedOut = static_cast<int>(
      otaBufferedReadResultFor(OtaBufferedReadAction::kStopTimedOut, false));
  const int incomplete = static_cast<int>(
      otaBufferedReadResultFor(OtaBufferedReadAction::kStopOk, true));
  const int ok = static_cast<int>(
      otaBufferedReadResultFor(OtaBufferedReadAction::kStopOk, false));

  TEST_ASSERT_NOT_EQUAL(tooLarge, timedOut);
  TEST_ASSERT_NOT_EQUAL(tooLarge, incomplete);
  TEST_ASSERT_NOT_EQUAL(timedOut, incomplete);
  TEST_ASSERT_NOT_EQUAL(ok, tooLarge);
  TEST_ASSERT_NOT_EQUAL(ok, timedOut);
  TEST_ASSERT_NOT_EQUAL(ok, incomplete);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_action_continues_while_connection_open_and_room_left);
  RUN_TEST(test_action_stops_too_large_at_exact_bound);
  RUN_TEST(test_action_too_large_wins_over_deadline);
  RUN_TEST(test_action_stops_timed_out_before_connection_check);
  RUN_TEST(test_action_continues_if_data_still_available_despite_closed_connection);
  RUN_TEST(test_action_continues_if_connection_open_but_no_data_right_now);
  RUN_TEST(test_action_stops_ok_when_closed_and_nothing_left);
  RUN_TEST(test_action_below_bound_strictly_continues);
  RUN_TEST(test_mismatch_false_when_exact_match);
  RUN_TEST(test_mismatch_true_when_fewer_bytes_than_declared);
  RUN_TEST(test_mismatch_true_when_more_bytes_than_declared);
  RUN_TEST(test_mismatch_false_when_declared_length_unknown_zero);
  RUN_TEST(test_mismatch_false_when_declared_length_negative);
  RUN_TEST(test_result_ok_when_stop_ok_and_no_mismatch);
  RUN_TEST(test_result_incomplete_when_stop_ok_but_length_mismatch);
  RUN_TEST(test_result_too_large_regardless_of_mismatch);
  RUN_TEST(test_result_timed_out_regardless_of_mismatch);
  RUN_TEST(test_result_three_failure_causes_are_pairwise_distinct);
  return UNITY_END();
}
