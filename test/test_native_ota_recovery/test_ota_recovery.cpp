// feature-151 D3-b1 : décision pure du secours OTA. On teste le COMPORTEMENT
// (table de vérité attendue, écrite indépendamment de l'implémentation).
#include <unity.h>
#include <string.h>
#include <cstdio>
#include "ota_recovery_logic.h"

void setUp(void) {}
void tearDown(void) {}

using R = OtaRecoveryReason;

// Oracle indépendant : priorité marqueur > montage > index, replis sûrs.
static R oracle(bool mounted, bool probed, bool present, bool markReadable, bool inFlight) {
  if (markReadable && inFlight) return R::kFsUpdateInterrupted;
  if (!mounted) return R::kMountFailed;
  if (probed && !present) return R::kIndexMissing;
  return R::kNone;
}

void test_reason_32_combinaisons_selon_oracle(void) {
  int n = 0;
  for (int m = 0; m < 32; m++) {
    OtaRecoveryInputs in{(m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0, (m & 16) != 0};
    R got = decideOtaRecoveryReason(in);
    R want = oracle(in.fsMounted, in.indexProbed, in.indexPresent, in.markerReadable, in.fsUpdateInFlight);
    char msg[64];
    snprintf(msg, sizeof msg, "combinaison %d", m);
    TEST_ASSERT_EQUAL_MESSAGE((int)want, (int)got, msg);
    n++;
  }
  TEST_ASSERT_EQUAL(32, n);
}

void test_sain_ne_arme_rien(void) {
  TEST_ASSERT_EQUAL((int)R::kNone, (int)decideOtaRecoveryReason({true, true, true, true, false}));
}

void test_marqueur_prime_sur_index_absent(void) {
  TEST_ASSERT_EQUAL((int)R::kFsUpdateInterrupted, (int)decideOtaRecoveryReason({true, true, false, true, true}));
}

void test_marqueur_prime_sur_montage_echoue(void) {
  TEST_ASSERT_EQUAL((int)R::kFsUpdateInterrupted, (int)decideOtaRecoveryReason({false, false, false, true, true}));
}

void test_marqueur_pose_sur_fs_sain_rend_secours_pendant_mise_a_jour(void) {
  // Pendant une mise à jour FS : fs monté, index présent, marqueur posé ⇒ `/` rend le secours.
  TEST_ASSERT_EQUAL((int)R::kFsUpdateInterrupted, (int)decideOtaRecoveryReason({true, true, true, true, true}));
}

void test_montage_echoue_prime_sur_index_absent(void) {
  TEST_ASSERT_EQUAL((int)R::kMountFailed, (int)decideOtaRecoveryReason({false, true, false, true, false}));
}

void test_index_absent_arme_si_sonde_effective(void) {
  TEST_ASSERT_EQUAL((int)R::kIndexMissing, (int)decideOtaRecoveryReason({true, true, false, true, false}));
}

void test_repli_sonde_non_effectuee_n_arme_jamais(void) {
  // indexProbed=false : quelle que soit indexPresent, jamais kIndexMissing.
  for (int present = 0; present < 2; present++)
    for (int mr = 0; mr < 2; mr++)
      TEST_ASSERT_EQUAL((int)R::kNone, (int)decideOtaRecoveryReason({true, false, present != 0, mr != 0, false}));
}

void test_repli_marqueur_illisible_n_arme_jamais(void) {
  // markerReadable=false : la valeur fsUpdateInFlight est ignorée, quelle qu'elle soit.
  TEST_ASSERT_EQUAL((int)R::kNone, (int)decideOtaRecoveryReason({true, true, true, false, true}));
  TEST_ASSERT_EQUAL((int)R::kNone, (int)decideOtaRecoveryReason({true, true, true, false, false}));
  // et ne masque pas un vrai défaut de montage
  TEST_ASSERT_EQUAL((int)R::kMountFailed, (int)decideOtaRecoveryReason({false, false, false, false, true}));
}

void test_labels_stables_et_distincts(void) {
  TEST_ASSERT_EQUAL_STRING("aucun", otaRecoveryReasonLabel(R::kNone));
  TEST_ASSERT_EQUAL_STRING("montage du filesystem échoué", otaRecoveryReasonLabel(R::kMountFailed));
  TEST_ASSERT_EQUAL_STRING("mise à jour du filesystem interrompue", otaRecoveryReasonLabel(R::kFsUpdateInterrupted));
  TEST_ASSERT_EQUAL_STRING("index.html absent", otaRecoveryReasonLabel(R::kIndexMissing));
  TEST_ASSERT_EQUAL_STRING("inconnu", otaRecoveryReasonLabel((R)99));
}

void test_S3_firmware_ne_touche_jamais_le_marqueur(void) {
  TEST_ASSERT_EQUAL((int)OtaRecoveryMarkerAction::kNone,
                    (int)decideOtaRecoveryMarkerAction(false, OtaRecoveryUpdatePhase::kSucceeded));
  TEST_ASSERT_EQUAL((int)OtaRecoveryMarkerAction::kNone,
                    (int)decideOtaRecoveryMarkerAction(false, OtaRecoveryUpdatePhase::kBeforeUnmount));
}

void test_filesystem_arme_avant_demontage_leve_au_succes(void) {
  TEST_ASSERT_EQUAL((int)OtaRecoveryMarkerAction::kArm,
                    (int)decideOtaRecoveryMarkerAction(true, OtaRecoveryUpdatePhase::kBeforeUnmount));
  TEST_ASSERT_EQUAL((int)OtaRecoveryMarkerAction::kDisarm,
                    (int)decideOtaRecoveryMarkerAction(true, OtaRecoveryUpdatePhase::kSucceeded));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reason_32_combinaisons_selon_oracle);
  RUN_TEST(test_sain_ne_arme_rien);
  RUN_TEST(test_marqueur_prime_sur_index_absent);
  RUN_TEST(test_marqueur_prime_sur_montage_echoue);
  RUN_TEST(test_marqueur_pose_sur_fs_sain_rend_secours_pendant_mise_a_jour);
  RUN_TEST(test_montage_echoue_prime_sur_index_absent);
  RUN_TEST(test_index_absent_arme_si_sonde_effective);
  RUN_TEST(test_repli_sonde_non_effectuee_n_arme_jamais);
  RUN_TEST(test_repli_marqueur_illisible_n_arme_jamais);
  RUN_TEST(test_labels_stables_et_distincts);
  RUN_TEST(test_S3_firmware_ne_touche_jamais_le_marqueur);
  RUN_TEST(test_filesystem_arme_avant_demontage_leve_au_succes);
  return UNITY_END();
}
