// feature-151 D3-b2, condition C5 : on teste L'APPELANT.
// Le VRAI src/ota_signature_switch.cpp est compilé contre un FAUX Preferences
// (test/native_shim/Preferences.h). Le module pur reçoit nvsReadable/entryPresent en
// booléens : c'est ici qu'on vérifie que l'appelant ne ment pas (leçon de la sonde
// /index.html de D3-b1). L'horloge est la VRAIE (time(nullptr) > kMinValidEpoch sur l'hôte).
#include <unity.h>
#include <time.h>
#include <string.h>
#include "Preferences.h"
#include "ota_signature_switch.h"
#include "constants.h"

using Req = OtaSignatureRequirement;
using R = OtaSwitchReason;
static const char* K = kOtaSignatureSwitchNvsKey;

static uint32_t now32() { return (uint32_t)time(nullptr); }

void setUp(void) { FakeNvs::reset(); otaSignatureSwitchInit(); }
void tearDown(void) {}

static void assertRequired(R reason) {
  auto d = otaSignatureSwitchRead();
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
  TEST_ASSERT_EQUAL((int)reason, (int)d.reason);
}

void test_horloge_hote_valide(void) { TEST_ASSERT_TRUE(time(nullptr) >= kMinValidEpoch); }

// --- Les trois cas C5 --------------------------------------------------------
void test_begin_en_echec_exige(void) {
  FakeNvs::seed(K, PT_U32, now32() + 600);  // une échéance VALIDE existe, mais la NVS ne s'ouvre pas
  FakeNvs::failBegin = true;
  assertRequired(R::kUnreadable);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
}

void test_cle_absente_exige(void) {
  assertRequired(R::kNoEntry);
}

void test_mauvais_type_exige_meme_avec_valeur_valide(void) {
  const PreferenceType wrong[] = {PT_I32, PT_U8, PT_U64, PT_I64, PT_STR, PT_BLOB, PT_U16};
  for (PreferenceType t : wrong) {
    FakeNvs::reset();
    FakeNvs::seed(K, t, now32() + 600);  // le faux rend la valeur quand même : piège
    assertRequired(R::kNoEntry);
  }
}

void test_temoin_positif_type_u32_valide_arme(void) {
  FakeNvs::seed(K, PT_U32, now32() + 600);
  auto d = otaSignatureSwitchRead();
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)d.requirement);
  TEST_ASSERT_TRUE(d.remainingSec > 590 && d.remainingSec <= 600);
}

void test_init_lit_positivement_aussi(void) {
  FakeNvs::seed(K, PT_STR, now32() + 600);
  otaSignatureSwitchInit();
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
  FakeNvs::reset();
  FakeNvs::seed(K, PT_U32, now32() + 600);
  otaSignatureSwitchInit();
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)otaSignatureSwitchPeek().requirement);
  FakeNvs::failBegin = true;
  otaSignatureSwitchInit();
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
}

void test_miroir_ferme_par_defaut_et_sans_io(void) {
  // Après setUp (NVS vide) : fermé. Peek ne touche jamais la NVS.
  FakeNvs::beginCount = 0;
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
  TEST_ASSERT_EQUAL(0, FakeNvs::beginCount);
}

// --- C1 vu depuis la NVS -----------------------------------------------------
void test_echeances_aberrantes_en_nvs_exigent(void) {
  const uint32_t bad[] = {0, 1, now32() - 1, now32(), now32() + kOtaSignatureSwitchWindowSec + 5,
                          now32() + 10u * 365u * 86400u, 0xFFFFFFFEu, 0xFFFFFFFFu};
  for (uint32_t v : bad) {
    FakeNvs::reset();
    FakeNvs::seed(K, PT_U32, v);
    TEST_ASSERT_EQUAL_MESSAGE((int)Req::kRequired, (int)otaSignatureSwitchRead().requirement, "echeance aberrante");
  }
}

// Une échéance NVS qui collisionne avec une sentinelle du miroir ne doit JAMAIS fabriquer un faux verrou
// (le témoin MQTT et le GET lisent « kLocked » comme un désarmement échoué).
void test_echeance_collision_sentinelle_ne_fabrique_pas_un_verrou(void) {
  const uint32_t sentinelles[] = {0xFFFFFFFEu, 0xFFFFFFFFu};
  for (uint32_t v : sentinelles) {
    FakeNvs::reset();
    FakeNvs::seed(K, PT_U32, v);
    auto d = otaSignatureSwitchRead();
    TEST_ASSERT_EQUAL((int)Req::kRequired, (int)d.requirement);
    TEST_ASSERT_TRUE((int)d.reason != (int)R::kLocked);
    TEST_ASSERT_TRUE((int)otaSignatureSwitchPeek().reason != (int)R::kLocked);
  }
}

void test_fenetre_absolue_3600_pas_de_constante_circulaire(void) {
  // Littéraux, pas la constante : une fenêtre élargie (36000) doit être vue.
  FakeNvs::seed(K, PT_U32, now32() + 3590);
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)otaSignatureSwitchRead().requirement);
  FakeNvs::seed(K, PT_U32, now32() + 3700);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchRead().requirement);
  uint32_t until = 0;
  TEST_ASSERT_EQUAL((int)OtaSwitchArmResult::kArmed, (int)otaSignatureSwitchArm(&until));
  TEST_ASSERT_TRUE(until >= now32() + 3598 && until <= now32() + 3602);
}

// --- Arm / Read / Clear / Consume -------------------------------------------
void test_arm_puis_read_arme_avec_fenetre_une_heure(void) {
  uint32_t until = 0;
  TEST_ASSERT_EQUAL((int)OtaSwitchArmResult::kArmed, (int)otaSignatureSwitchArm(&until));
  TEST_ASSERT_TRUE(until >= now32() + kOtaSignatureSwitchWindowSec - 2 && until <= now32() + kOtaSignatureSwitchWindowSec + 2);
  auto d = otaSignatureSwitchRead();
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)d.requirement);
  TEST_ASSERT_TRUE(d.remainingSec <= kOtaSignatureSwitchWindowSec);
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)otaSignatureSwitchPeek().requirement);
}

void test_arm_ecriture_echouee_n_arme_pas(void) {
  uint32_t until = 12345;
  FakeNvs::failPut = true;
  TEST_ASSERT_EQUAL((int)OtaSwitchArmResult::kWriteFailed, (int)otaSignatureSwitchArm(&until));
  TEST_ASSERT_EQUAL_UINT32(12345, until);
  FakeNvs::failPut = false;
  assertRequired(R::kNoEntry);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
}

void test_arm_begin_en_echec_n_arme_pas(void) {
  uint32_t until = 0;
  FakeNvs::failBegin = true;
  TEST_ASSERT_EQUAL((int)OtaSwitchArmResult::kWriteFailed, (int)otaSignatureSwitchArm(&until));
}

void test_clear_efface_et_exige(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  TEST_ASSERT_TRUE(otaSignatureSwitchClear());
  TEST_ASSERT_NULL(FakeNvs::find(K));
  assertRequired(R::kNoEntry);
}

void test_clear_sans_entree_reussit(void) { TEST_ASSERT_TRUE(otaSignatureSwitchClear()); }

void test_clear_remove_en_echec_repli_a_zero(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  FakeNvs::failRemove = true;
  TEST_ASSERT_TRUE(otaSignatureSwitchClear());
  TEST_ASSERT_EQUAL_UINT32(0, FakeNvs::find(K)->value);  // repli : 0 = pas d'échéance
  FakeNvs::failRemove = false;
  assertRequired(R::kNoEntry);  // 0 stocké = « pas d échéance » ; un redémarrage ne ré-arme pas
  otaSignatureSwitchInit();
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
}

void test_c7_desarmement_totalement_en_echec_verrou_ram_honore_par_read(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  FakeNvs::failRemove = true; FakeNvs::failPut = true;
  TEST_ASSERT_FALSE(otaSignatureSwitchClear());
  FakeNvs::failRemove = false; FakeNvs::failPut = false;
  // L'échéance valide est toujours en NVS, mais le verrou RAM est honoré par Read() (pas de relecture).
  TEST_ASSERT_NOT_NULL(FakeNvs::find(K));
  assertRequired(R::kLocked);
  TEST_ASSERT_EQUAL((int)Req::kRequired, (int)otaSignatureSwitchPeek().requirement);
  TEST_ASSERT_EQUAL_UINT32(FakeNvs::find(K)->value, otaSignatureSwitchResidualEpoch());
  // Seul un armement explicite lève le verrou.
  TEST_ASSERT_EQUAL((int)OtaSwitchArmResult::kArmed, (int)otaSignatureSwitchArm(&u));
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)otaSignatureSwitchRead().requirement);
  TEST_ASSERT_EQUAL_UINT32(0, otaSignatureSwitchResidualEpoch());
}

// --- R2 / C7 : « inconnu » ne se lit jamais « rien ne reste » ------------------------
void test_c7_begin_en_echec_residu_inconnu_jamais_zero(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  FakeNvs::failBegin = true;                       // NVS totalement inaccessible
  TEST_ASSERT_FALSE(otaSignatureSwitchClear());
  FakeNvs::failBegin = false;
  TEST_ASSERT_EQUAL_UINT32(kOtaSwitchResidualUnknown, otaSignatureSwitchResidualEpoch());
  TEST_ASSERT_TRUE(otaSignatureSwitchResidualEpoch() != 0u);
  TEST_ASSERT_EQUAL((int)OtaSwitchRearm::kUnknown, (int)otaSignatureSwitchRearmsOnReboot());
  assertRequired(R::kLocked);                      // le verrou RAM tient quand même
}

void test_rearms_on_reboot_oui_quand_echeance_valide_survit(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  FakeNvs::failRemove = true; FakeNvs::failPut = true;
  TEST_ASSERT_FALSE(otaSignatureSwitchClear());
  FakeNvs::failRemove = false; FakeNvs::failPut = false;
  TEST_ASSERT_EQUAL((int)OtaSwitchRearm::kYes, (int)otaSignatureSwitchRearmsOnReboot());
  TEST_ASSERT_EQUAL_UINT32(u, otaSignatureSwitchResidualEpoch());
}

void test_rearms_on_reboot_non_apres_desarmement_reussi_et_au_depart(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  TEST_ASSERT_TRUE(otaSignatureSwitchClear());
  TEST_ASSERT_EQUAL((int)OtaSwitchRearm::kNo, (int)otaSignatureSwitchRearmsOnReboot());
  TEST_ASSERT_EQUAL_UINT32(0, otaSignatureSwitchResidualEpoch());
}

void test_rearms_on_reboot_non_quand_residu_est_zero_en_nvs(void) {
  FakeNvs::seed(K, PT_U32, 1000);                  // échéance
  otaSignatureSwitchInit();
  FakeNvs::failRemove = true; FakeNvs::failPut = true;
  FakeNvs::seed(K, PT_U32, 0);                     // 0 = pas d'échéance (repli C1)
  TEST_ASSERT_FALSE(otaSignatureSwitchClear());
  FakeNvs::failRemove = false; FakeNvs::failPut = false;
  TEST_ASSERT_EQUAL((int)OtaSwitchRearm::kNo, (int)otaSignatureSwitchRearmsOnReboot());
}

void test_consume_desarme_apres_flash_reussi(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  otaSignatureSwitchConsume(true);
  assertRequired(R::kNoEntry);
  TEST_ASSERT_NULL(FakeNvs::find(K));
}

void test_consume_sans_arme_ne_touche_pas_la_nvs(void) {
  FakeNvs::beginCount = 0;
  otaSignatureSwitchConsume(false);
  TEST_ASSERT_EQUAL(0, FakeNvs::beginCount);
}

void test_consume_session_exigee_ne_devient_jamais_levee(void) {
  // Session commencée « exigée » (false) puis interrupteur armé en cours de route : la
  // consommation désarme, mais ne bascule rien vers « levé ».
  uint32_t u; otaSignatureSwitchArm(&u);
  otaSignatureSwitchConsume(false);
  assertRequired(R::kNoEntry);
}

void test_consume_en_echec_verrou_et_pas_d_armement(void) {
  uint32_t u; otaSignatureSwitchArm(&u);
  FakeNvs::failRemove = true; FakeNvs::failPut = true;
  otaSignatureSwitchConsume(true);
  FakeNvs::failRemove = false; FakeNvs::failPut = false;
  assertRequired(R::kLocked);
}

void test_expiration_pendant_la_session_ne_revoque_rien_et_exige_ensuite(void) {
  FakeNvs::seed(K, PT_U32, now32() + 1);   // armé, expire dans 1 s
  TEST_ASSERT_EQUAL((int)Req::kWaived, (int)otaSignatureSwitchRead().requirement);
  FakeNvs::seed(K, PT_U32, now32() - 1);   // expiré entre-temps
  otaSignatureSwitchConsume(true);         // ne plante pas, journalise
  assertRequired(R::kNoEntry);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_horloge_hote_valide);
  RUN_TEST(test_begin_en_echec_exige);
  RUN_TEST(test_cle_absente_exige);
  RUN_TEST(test_mauvais_type_exige_meme_avec_valeur_valide);
  RUN_TEST(test_temoin_positif_type_u32_valide_arme);
  RUN_TEST(test_init_lit_positivement_aussi);
  RUN_TEST(test_miroir_ferme_par_defaut_et_sans_io);
  RUN_TEST(test_echeances_aberrantes_en_nvs_exigent);
  RUN_TEST(test_echeance_collision_sentinelle_ne_fabrique_pas_un_verrou);
  RUN_TEST(test_fenetre_absolue_3600_pas_de_constante_circulaire);
  RUN_TEST(test_arm_puis_read_arme_avec_fenetre_une_heure);
  RUN_TEST(test_arm_ecriture_echouee_n_arme_pas);
  RUN_TEST(test_arm_begin_en_echec_n_arme_pas);
  RUN_TEST(test_clear_efface_et_exige);
  RUN_TEST(test_clear_sans_entree_reussit);
  RUN_TEST(test_clear_remove_en_echec_repli_a_zero);
  RUN_TEST(test_c7_desarmement_totalement_en_echec_verrou_ram_honore_par_read);
  RUN_TEST(test_c7_begin_en_echec_residu_inconnu_jamais_zero);
  RUN_TEST(test_rearms_on_reboot_oui_quand_echeance_valide_survit);
  RUN_TEST(test_rearms_on_reboot_non_apres_desarmement_reussi_et_au_depart);
  RUN_TEST(test_rearms_on_reboot_non_quand_residu_est_zero_en_nvs);
  RUN_TEST(test_consume_desarme_apres_flash_reussi);
  RUN_TEST(test_consume_sans_arme_ne_touche_pas_la_nvs);
  RUN_TEST(test_consume_session_exigee_ne_devient_jamais_levee);
  RUN_TEST(test_consume_en_echec_verrou_et_pas_d_armement);
  RUN_TEST(test_expiration_pendant_la_session_ne_revoque_rien_et_exige_ensuite);
  return UNITY_END();
}
