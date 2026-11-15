// =============================================================================
// Tests unitaires natifs — décision du relais à la demande /ota-sig (bug-176, ADR-0123)
// =============================================================================
// Comportement protégé : condition S2 (« absent » UNIQUEMENT si la release a été
// analysée avec succès sans asset), contrat des codes machine, égalité exacte du
// paramètre kind, anti-déni S4 sûr au repliement de millis().
// =============================================================================

#include <string.h>
#include <unity.h>

#include "ota_sig_relay_logic.h"

void setUp(void) {}
void tearDown(void) {}

static bool kind(const char* s, uint8_t* out) { return otaSigKindFromParam(s, strlen(s), out); }

// ---- otaSigKindFromParam ----------------------------------------------------
void test_kind_firmware_filesystem(void) {
  uint8_t k = 99;
  TEST_ASSERT_TRUE(kind("firmware", &k));
  TEST_ASSERT_EQUAL_UINT8(0, k);
  TEST_ASSERT_TRUE(kind("filesystem", &k));
  TEST_ASSERT_EQUAL_UINT8(1, k);
}

void test_kind_egalite_exacte(void) {
  const char* bad[] = {"", "Firmware", "firmware ", " firmware", "firmwarex", "firmwar",
                       "FILESYSTEM", "Filesystem", "filesystem ", "filesystemx", "file",
                       "firmware\n"};
  for (const char* s : bad) {
    uint8_t k = 77;
    TEST_ASSERT_FALSE_MESSAGE(kind(s, &k), s);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(77, k, "sortie intacte sur échec");
  }
}

void test_kind_longueur_explicite_et_nul(void) {
  uint8_t k = 77;
  // « firmware » suivi d'un NUL embarqué, longueur 9 : refus
  TEST_ASSERT_FALSE(otaSigKindFromParam("firmware\0", 9, &k));
  // préfixe de longueur 4 d'un texte plus long : refus
  TEST_ASSERT_FALSE(otaSigKindFromParam("firmware", 4, &k));
  // longueur exacte, texte non terminé par NUL : accepté
  const char raw[8] = {'f', 'i', 'r', 'm', 'w', 'a', 'r', 'e'};
  TEST_ASSERT_TRUE(otaSigKindFromParam(raw, 8, &k));
  TEST_ASSERT_EQUAL_UINT8(0, k);
}

void test_kind_parametre_absent(void) {
  uint8_t k = 77;
  TEST_ASSERT_FALSE(otaSigKindFromParam(nullptr, 0, &k));
  TEST_ASSERT_FALSE(otaSigKindFromParam("firmware", 8, nullptr));
  TEST_ASSERT_EQUAL_UINT8(77, k);
}

// ---- S2 : otaSigSourceDecide ------------------------------------------------
void test_s2_absent_seulement_si_checked_no_asset(void) {
  for (int v = 0; v < 256; v++) {
    const OtaSigSourceDecision d = otaSigSourceDecide(static_cast<OtaSigSourceState>(v));
    if (v == static_cast<int>(OtaSigSourceState::kCheckedNoAsset)) {
      TEST_ASSERT_EQUAL(OtaSigRelayStatus::kAbsent, d.decision.status);
      TEST_ASSERT_EQUAL(OtaSigRelayReason::kNoAsset, d.decision.reason);
      TEST_ASSERT_FALSE(d.proceed);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(d.decision.status != OtaSigRelayStatus::kAbsent, "absent hors S2");
      TEST_ASSERT_TRUE(d.decision.reason != OtaSigRelayReason::kNoAsset);
    }
  }
}

void test_s2_autres_etats_unavailable_avec_motif(void) {
  OtaSigSourceDecision d = otaSigSourceDecide(OtaSigSourceState::kNeverChecked);
  TEST_ASSERT_EQUAL(OtaSigRelayStatus::kUnavailable, d.decision.status);
  TEST_ASSERT_EQUAL(OtaSigRelayReason::kNoCheck, d.decision.reason);
  TEST_ASSERT_FALSE(d.proceed);

  d = otaSigSourceDecide(OtaSigSourceState::kTooLong);
  TEST_ASSERT_EQUAL(OtaSigRelayStatus::kUnavailable, d.decision.status);
  TEST_ASSERT_EQUAL(OtaSigRelayReason::kUrlTooLong, d.decision.reason);
  TEST_ASSERT_FALSE(d.proceed);

  d = otaSigSourceDecide(OtaSigSourceState::kHostRefused);
  TEST_ASSERT_EQUAL(OtaSigRelayStatus::kUnavailable, d.decision.status);
  TEST_ASSERT_EQUAL(OtaSigRelayReason::kHost, d.decision.reason);
  TEST_ASSERT_FALSE(d.proceed);

  // valeur hors énumération : unavailable AVEC motif, jamais de connexion
  d = otaSigSourceDecide(static_cast<OtaSigSourceState>(200));
  TEST_ASSERT_EQUAL(OtaSigRelayStatus::kUnavailable, d.decision.status);
  TEST_ASSERT_EQUAL(OtaSigRelayReason::kNoCheck, d.decision.reason);
  TEST_ASSERT_FALSE(d.proceed);
}

void test_source_present_differe_et_autorise_session(void) {
  const OtaSigSourceDecision d = otaSigSourceDecide(OtaSigSourceState::kPresent);
  TEST_ASSERT_EQUAL(OtaSigRelayStatus::kDeferred, d.decision.status);
  TEST_ASSERT_EQUAL(OtaSigRelayReason::kNone, d.decision.reason);
  TEST_ASSERT_TRUE(d.proceed);
}

void test_seul_present_autorise_une_session(void) {
  for (int v = 0; v < 256; v++) {
    const bool p = otaSigSourceDecide(static_cast<OtaSigSourceState>(v)).proceed;
    TEST_ASSERT_EQUAL(v == static_cast<int>(OtaSigSourceState::kPresent), p);
  }
}

// ---- otaSigRelayDecide : matrice issue -> (statut, motif) ------------------
static void expect(OtaSigFetchOutcome o, OtaSigRelayStatus s, OtaSigRelayReason r) {
  const OtaSigRelayDecision d = otaSigRelayDecide(o);
  TEST_ASSERT_EQUAL(s, d.status);
  TEST_ASSERT_EQUAL(r, d.reason);
}

void test_relais_matrice(void) {
  expect(OtaSigFetchOutcome::kValidated, OtaSigRelayStatus::kRelayed, OtaSigRelayReason::kNone);
  expect(OtaSigFetchOutcome::kHostRefused, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kHost);
  expect(OtaSigFetchOutcome::kGateClosed, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kMemory);
  expect(OtaSigFetchOutcome::kBeginFailed, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kHttp);
  expect(OtaSigFetchOutcome::kHttpError, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kHttp);
  expect(OtaSigFetchOutcome::kBodyTooLong, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kGrammar);
  expect(OtaSigFetchOutcome::kTimedOut, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kTimeout);
  expect(OtaSigFetchOutcome::kBadGrammar, OtaSigRelayStatus::kUnavailable, OtaSigRelayReason::kGrammar);
}

void test_relais_jamais_absent_ni_relayed_hors_succes(void) {
  for (int v = 0; v < 256; v++) {
    const OtaSigRelayDecision d = otaSigRelayDecide(static_cast<OtaSigFetchOutcome>(v));
    TEST_ASSERT_TRUE(d.status != OtaSigRelayStatus::kAbsent);
    if (v != static_cast<int>(OtaSigFetchOutcome::kValidated)) {
      TEST_ASSERT_EQUAL(OtaSigRelayStatus::kUnavailable, d.status);
      TEST_ASSERT_TRUE(d.reason != OtaSigRelayReason::kNone);
    }
  }
}

void test_relais_issue_hors_enumeration_unavailable(void) {
  const OtaSigRelayDecision d = otaSigRelayDecide(static_cast<OtaSigFetchOutcome>(250));
  TEST_ASSERT_EQUAL(OtaSigRelayStatus::kUnavailable, d.status);
  TEST_ASSERT_TRUE(d.reason != OtaSigRelayReason::kNone);
}

// ---- Codes machine (contrat) ------------------------------------------------
void test_codes_statut(void) {
  TEST_ASSERT_EQUAL_STRING("absent", otaSigRelayStatusCode(OtaSigRelayStatus::kAbsent));
  TEST_ASSERT_EQUAL_STRING("deferred", otaSigRelayStatusCode(OtaSigRelayStatus::kDeferred));
  TEST_ASSERT_EQUAL_STRING("relayed", otaSigRelayStatusCode(OtaSigRelayStatus::kRelayed));
  TEST_ASSERT_EQUAL_STRING("unavailable", otaSigRelayStatusCode(OtaSigRelayStatus::kUnavailable));
  // hors énumération : jamais « absent »
  TEST_ASSERT_EQUAL_STRING("unavailable", otaSigRelayStatusCode(static_cast<OtaSigRelayStatus>(99)));
}

void test_codes_motif(void) {
  TEST_ASSERT_EQUAL_STRING("", otaSigRelayReasonCode(OtaSigRelayReason::kNone));
  TEST_ASSERT_EQUAL_STRING("no_asset", otaSigRelayReasonCode(OtaSigRelayReason::kNoAsset));
  TEST_ASSERT_EQUAL_STRING("no_check", otaSigRelayReasonCode(OtaSigRelayReason::kNoCheck));
  TEST_ASSERT_EQUAL_STRING("url_too_long", otaSigRelayReasonCode(OtaSigRelayReason::kUrlTooLong));
  TEST_ASSERT_EQUAL_STRING("host", otaSigRelayReasonCode(OtaSigRelayReason::kHost));
  TEST_ASSERT_EQUAL_STRING("memory", otaSigRelayReasonCode(OtaSigRelayReason::kMemory));
  TEST_ASSERT_EQUAL_STRING("time", otaSigRelayReasonCode(OtaSigRelayReason::kTime));
  TEST_ASSERT_EQUAL_STRING("http", otaSigRelayReasonCode(OtaSigRelayReason::kHttp));
  TEST_ASSERT_EQUAL_STRING("timeout", otaSigRelayReasonCode(OtaSigRelayReason::kTimeout));
  TEST_ASSERT_EQUAL_STRING("grammar", otaSigRelayReasonCode(OtaSigRelayReason::kGrammar));
  TEST_ASSERT_EQUAL_STRING("throttled", otaSigRelayReasonCode(OtaSigRelayReason::kThrottled));
}

// ---- otaSigThrottled --------------------------------------------------------
void test_throttle_basique(void) {
  TEST_ASSERT_FALSE(otaSigThrottled(false, 0, 0, 10000));      // jamais tenté
  TEST_ASSERT_FALSE(otaSigThrottled(false, 5000, 5001, 10000));
  TEST_ASSERT_TRUE(otaSigThrottled(true, 1000, 1000, 10000));  // même instant
  TEST_ASSERT_TRUE(otaSigThrottled(true, 1000, 10999, 10000)); // 9999 ms
  TEST_ASSERT_FALSE(otaSigThrottled(true, 1000, 11000, 10000)); // 10000 ms pile : libre
  TEST_ASSERT_FALSE(otaSigThrottled(true, 1000, 50000, 10000));
}

void test_throttle_repliement_millis(void) {
  // dernier essai 3 s avant le repliement de millis() (2^32 ms = 49,7 j)
  const uint32_t last = 0xFFFFFFFFu - 3000u;
  TEST_ASSERT_TRUE(otaSigThrottled(true, last, 0xFFFFFFFFu, 10000));  // 3 s écoulées
  TEST_ASSERT_TRUE(otaSigThrottled(true, last, 2000u, 10000));        // 5 s, après repli
  TEST_ASSERT_FALSE(otaSigThrottled(true, last, 7000u, 10000));       // ~10 s : libre
  TEST_ASSERT_FALSE(otaSigThrottled(true, last, 20000u, 10000));
}

void test_throttle_ecart_superieur_24_8_jours(void) {
  // Écart de 30 jours (> 2^31 ms = 24,8 j) : l'essai est très ancien => libre.
  // Une soustraction/comparaison SIGNÉE y verrait un écart négatif => refus à tort.
  const uint32_t trenteJours = 30u * 24u * 3600u * 1000u;
  TEST_ASSERT_FALSE(otaSigThrottled(true, 0u, trenteJours, 10000));
  TEST_ASSERT_FALSE(otaSigThrottled(true, 1000u, 1000u + 0x90000000u, 10000));
  // et juste en deçà de la demi-plage reste libre aussi
  TEST_ASSERT_FALSE(otaSigThrottled(true, 0u, 0x7FFFFFFFu, 10000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_kind_firmware_filesystem);
  RUN_TEST(test_kind_egalite_exacte);
  RUN_TEST(test_kind_longueur_explicite_et_nul);
  RUN_TEST(test_kind_parametre_absent);
  RUN_TEST(test_s2_absent_seulement_si_checked_no_asset);
  RUN_TEST(test_s2_autres_etats_unavailable_avec_motif);
  RUN_TEST(test_source_present_differe_et_autorise_session);
  RUN_TEST(test_seul_present_autorise_une_session);
  RUN_TEST(test_relais_matrice);
  RUN_TEST(test_relais_jamais_absent_ni_relayed_hors_succes);
  RUN_TEST(test_relais_issue_hors_enumeration_unavailable);
  RUN_TEST(test_codes_statut);
  RUN_TEST(test_codes_motif);
  RUN_TEST(test_throttle_basique);
  RUN_TEST(test_throttle_repliement_millis);
  RUN_TEST(test_throttle_ecart_superieur_24_8_jours);
  return UNITY_END();
}
