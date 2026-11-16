// feature-151 D3-b2 (C1) : décision PURE de l'interrupteur de signature. Oracle écrit
// indépendamment de l'implémentation : « kWaived ssi lu positivement, horloge valide,
// 0 < échéance - now <= fenêtre ».
#include <unity.h>
#include <string.h>
#include "ota_signature_switch_logic.h"
#include "constants.h"

void setUp(void) {}
void tearDown(void) {}

using Req = OtaSignatureRequirement;
using R = OtaSwitchReason;
static const uint32_t W = kOtaSignatureSwitchWindowSec;
static const uint32_t NOW = 1800000000u;  // > kMinValidEpoch

static OtaSignatureSwitchInputs in(bool readable, bool present, uint32_t expiry, bool clock, uint32_t now) {
  return OtaSignatureSwitchInputs{readable, present, expiry, clock, now};
}

void test_nvs_vide_exige_la_signature(void) {
  auto d = decideOtaSignatureSwitch(in(true, false, 0, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kNoEntry, (int)d.reason);
  TEST_ASSERT_EQUAL_UINT32(0, d.remainingSec);
}

void test_nvs_vide_malgre_echeance_residuelle_exige(void) {
  auto d = decideOtaSignatureSwitch(in(true, false, NOW + 100, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
}

void test_nvs_illisible_exige_meme_avec_echeance_valide(void) {
  auto d = decideOtaSignatureSwitch(in(false, true, NOW + 100, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kUnreadable, (int)d.reason);
}

void test_heure_invalide_exige_meme_cle_presente_non_expiree(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + 100, false, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kNoClock, (int)d.reason);
}

void test_heure_invalide_exige_quel_que_soit_nowEpoch(void) {
  // L'appelant peut fournir un now plausible tout en disant clockValid=false : clockValid prime.
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + 100, false, NOW + 1), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
}

void test_expire_exige(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW - 1, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kExpired, (int)d.reason);
}

void test_borne_now_egal_echeance_est_expire(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kExpired, (int)d.reason);
}

void test_une_seconde_avant_echeance_est_arme(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + 1, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)d.requirement);
  TEST_ASSERT_EQUAL_UINT32(1, d.remainingSec);
  TEST_ASSERT_EQUAL_UINT32(NOW + 1, d.untilEpoch);
}

void test_arme_rend_remaining(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + 1234, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kArmed, (int)d.reason);
  TEST_ASSERT_EQUAL_UINT32(1234, d.remainingSec);
}

void test_echeance_zero_exige_c1(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, 0, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kInvalid, (int)d.reason);
}

void test_echeance_zero_exige_meme_si_now_nul_horloge_valide_declaree(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, 0, true, 0), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
}

void test_borne_haute_fenetre_exacte_est_arme(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + W, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)d.requirement);
  TEST_ASSERT_EQUAL_UINT32(W, d.remainingSec);
}

void test_borne_haute_fenetre_plus_une_seconde_exige_c1(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + W + 1, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kInvalid, (int)d.reason);
}

void test_horloge_fausse_vers_le_futur_a_l_armement_ne_survit_pas_g1(void) {
  // Armé quand l'horloge avançait de 10 ans : échéance = futur + 1 h. L'horloge est ensuite
  // corrigée : l'échéance est à des années => HORS fenêtre => exigée.
  const uint32_t future = NOW + 10u * 365u * 86400u;
  auto d = decideOtaSignatureSwitch(in(true, true, future + W, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kInvalid, (int)d.reason);
}

void test_echeance_maximale_exige(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, 0xFFFFFFFFu, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
}

void test_echeance_passe_lointain_exige(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, 1, true, NOW), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)R::kExpired, (int)d.reason);
}

void test_now_juste_sous_kMinValidEpoch_cote_appelant(void) {
  // L'appelant calcule clockValid = now >= kMinValidEpoch ; ici on joue son contrat.
  const uint32_t justBelow = (uint32_t)kMinValidEpoch - 1;
  auto d = decideOtaSignatureSwitch(in(true, true, justBelow + 100, justBelow >= (uint32_t)kMinValidEpoch, justBelow), W);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  auto ok = decideOtaSignatureSwitch(in(true, true, (uint32_t)kMinValidEpoch + 100, true, (uint32_t)kMinValidEpoch), W);
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)ok.requirement);
}

void test_fenetre_zero_n_arme_jamais(void) {
  auto d = decideOtaSignatureSwitch(in(true, true, NOW + 1, true, NOW), 0);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
}

void test_exhaustif_kWaived_ssi_oracle(void) {
  const uint32_t expiries[] = {0, 1, NOW - 1, NOW, NOW + 1, NOW + W, NOW + W + 1, 0xFFFFFFFEu, 0xFFFFFFFFu};
  int waived = 0, total = 0;
  for (int m = 0; m < 8; m++) {
    for (uint32_t e : expiries) {
      const bool rd = m & 1, pr = m & 2, ck = m & 4;
      auto d = decideOtaSignatureSwitch(in(rd, pr, e, ck, NOW), W);
      const bool want = rd && pr && ck && e != 0 && e > NOW && (e - NOW) <= W;
      TEST_ASSERT_EQUAL_MESSAGE(want ? 1 : 0, d.requirement == Req::kWaived ? 1 : 0, "oracle");
      // hors kArmed : aucun reste, aucune échéance exposée
      if (!want) { TEST_ASSERT_EQUAL_UINT32(0, d.remainingSec); TEST_ASSERT_EQUAL_UINT32(0, d.untilEpoch); }
      waived += want; total++;
    }
  }
  TEST_ASSERT_EQUAL(72, total);
  TEST_ASSERT_EQUAL(2, waived);  // (NOW+1) et (NOW+W) avec rd/pr/ck vrais
}

void test_codes_stables_et_distincts(void) {
  const R all[] = {R::kNoEntry, R::kUnreadable, R::kNoClock, R::kExpired, R::kArmed, R::kInvalid, R::kLocked};
  for (size_t i = 0; i < 7; i++)
    for (size_t j = i + 1; j < 7; j++)
      TEST_ASSERT_TRUE(strcmp(otaSwitchReasonCode(all[i]), otaSwitchReasonCode(all[j])) != 0);
  TEST_ASSERT_EQUAL_STRING("armed", otaSwitchReasonCode(R::kArmed));
  TEST_ASSERT_EQUAL_STRING("locked", otaSwitchReasonCode(R::kLocked));
}

void test_constante_fenetre_est_une_heure(void) {
  TEST_ASSERT_EQUAL_UINT32(3600, kOtaSignatureSwitchWindowSec);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nvs_vide_exige_la_signature);
  RUN_TEST(test_nvs_vide_malgre_echeance_residuelle_exige);
  RUN_TEST(test_nvs_illisible_exige_meme_avec_echeance_valide);
  RUN_TEST(test_heure_invalide_exige_meme_cle_presente_non_expiree);
  RUN_TEST(test_heure_invalide_exige_quel_que_soit_nowEpoch);
  RUN_TEST(test_expire_exige);
  RUN_TEST(test_borne_now_egal_echeance_est_expire);
  RUN_TEST(test_une_seconde_avant_echeance_est_arme);
  RUN_TEST(test_arme_rend_remaining);
  RUN_TEST(test_echeance_zero_exige_c1);
  RUN_TEST(test_echeance_zero_exige_meme_si_now_nul_horloge_valide_declaree);
  RUN_TEST(test_borne_haute_fenetre_exacte_est_arme);
  RUN_TEST(test_borne_haute_fenetre_plus_une_seconde_exige_c1);
  RUN_TEST(test_horloge_fausse_vers_le_futur_a_l_armement_ne_survit_pas_g1);
  RUN_TEST(test_echeance_maximale_exige);
  RUN_TEST(test_echeance_passe_lointain_exige);
  RUN_TEST(test_now_juste_sous_kMinValidEpoch_cote_appelant);
  RUN_TEST(test_fenetre_zero_n_arme_jamais);
  RUN_TEST(test_exhaustif_kWaived_ssi_oracle);
  RUN_TEST(test_codes_stables_et_distincts);
  RUN_TEST(test_constante_fenetre_est_une_heure);
  return UNITY_END();
}
