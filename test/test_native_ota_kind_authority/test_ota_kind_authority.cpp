// =============================================================================
// Tests unitaires natifs — ota_kind_authority_logic (feature-151 D3-a)
// =============================================================================
// Table de vérité COMPLÈTE de resolveOtaEffectiveKind : 2 (appelant) x 2 (signé)
// x 2 (exploitable) x 2 (authentique) = 16 lignes, vérifiées une à une contre
// l'oracle écrit ici en clair (jamais recalculé avec la même expression que le
// code). Contrats testés :
//   - l'autorité (genre effectif = genre signé) n'est accordée QUE si le genre
//     signé est exploitable ET l'authenticité valide ;
//   - sinon le genre effectif == genre de l'appelant ;
//   - le désaccord est vrai dès que le genre signé est exploitable ET diffère,
//     MÊME hors kValid ; faux sinon.
// =============================================================================

#include <stdio.h>
#include <unity.h>

#include "ota_kind_authority_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const OtaManifestKind kF = OtaManifestKind::kFirmware;
static const OtaManifestKind kS = OtaManifestKind::kFilesystem;

struct Row {
  OtaManifestKind caller;
  OtaManifestKind signedK;
  bool usable;
  bool valid;
  OtaManifestKind expectKind;
  bool expectDisagree;
};

// Oracle EXPLICITE, ligne par ligne (pas de formule).
static const Row kRows[16] = {
    // caller signé  usable valid  => effectif disaccord
    {kF, kF, false, false, kF, false},
    {kF, kF, false, true, kF, false},
    {kF, kF, true, false, kF, false},
    {kF, kF, true, true, kF, false},
    {kF, kS, false, false, kF, false},
    {kF, kS, false, true, kF, false},  // inexploitable : l'autorité ne s'applique pas
    {kF, kS, true, false, kF, true},   // désaccord VRAI hors kValid, appelant l'emporte
    {kF, kS, true, true, kS, true},    // seul cas où le signé l'emporte sur l'appelant
    {kS, kF, false, false, kS, false},
    {kS, kF, false, true, kS, false},
    {kS, kF, true, false, kS, true},
    {kS, kF, true, true, kF, true},
    {kS, kS, false, false, kS, false},
    {kS, kS, false, true, kS, false},
    {kS, kS, true, false, kS, false},
    {kS, kS, true, true, kS, false},
};

void test_truth_table_full(void) {
  for (int i = 0; i < 16; ++i) {
    const Row& r = kRows[i];
    const OtaKindResolution out = resolveOtaEffectiveKind(r.caller, r.signedK, r.usable, r.valid);
    char msg[64];
    snprintf(msg, sizeof(msg), "ligne %d : genre effectif", i);
    TEST_ASSERT_EQUAL_MESSAGE((int)r.expectKind, (int)out.effectiveKind, msg);
    snprintf(msg, sizeof(msg), "ligne %d : desaccord", i);
    TEST_ASSERT_EQUAL_MESSAGE(r.expectDisagree, out.disagreement, msg);
  }
}

// Point dur 1 : l'autorité est REFUSÉE hors kValid, quel que soit le genre signé.
void test_authority_refused_when_not_valid(void) {
  for (int c = 0; c < 2; ++c) {
    for (int s = 0; s < 2; ++s) {
      const OtaManifestKind caller = (OtaManifestKind)c;
      const OtaManifestKind signedK = (OtaManifestKind)s;
      const OtaKindResolution out = resolveOtaEffectiveKind(caller, signedK, true, false);
      TEST_ASSERT_EQUAL((int)caller, (int)out.effectiveKind);
    }
  }
}

// Point dur 2 : le désaccord est vrai même hors kValid.
void test_disagreement_true_even_when_not_valid(void) {
  TEST_ASSERT_TRUE(resolveOtaEffectiveKind(kF, kS, true, false).disagreement);
  TEST_ASSERT_TRUE(resolveOtaEffectiveKind(kS, kF, true, false).disagreement);
  // … et il n'y en a pas quand le genre signé n'est pas exploitable.
  TEST_ASSERT_FALSE(resolveOtaEffectiveKind(kF, kS, false, false).disagreement);
  TEST_ASSERT_FALSE(resolveOtaEffectiveKind(kS, kF, false, true).disagreement);
}

// Point dur 3 : autorité non accordée => genre effectif == genre appelant
// (jamais le genre signé, même inexploitable avec une valeur « piégée »).
void test_effective_equals_caller_when_authority_not_granted(void) {
  for (int c = 0; c < 2; ++c) {
    for (int s = 0; s < 2; ++s) {
      for (int usable = 0; usable < 2; ++usable) {
        for (int valid = 0; valid < 2; ++valid) {
          const bool granted = usable && valid;
          if (granted) continue;
          const OtaKindResolution out =
              resolveOtaEffectiveKind((OtaManifestKind)c, (OtaManifestKind)s, usable, valid);
          TEST_ASSERT_EQUAL(c, (int)out.effectiveKind);
        }
      }
    }
  }
}

// Quand l'autorité est accordée, le genre signé l'emporte (y compris contre l'appelant).
void test_signed_kind_wins_when_valid_and_usable(void) {
  TEST_ASSERT_EQUAL((int)kS, (int)resolveOtaEffectiveKind(kF, kS, true, true).effectiveKind);
  TEST_ASSERT_EQUAL((int)kF, (int)resolveOtaEffectiveKind(kS, kF, true, true).effectiveKind);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_truth_table_full);
  RUN_TEST(test_authority_refused_when_not_valid);
  RUN_TEST(test_disagreement_true_even_when_not_valid);
  RUN_TEST(test_effective_equals_caller_when_authority_not_granted);
  RUN_TEST(test_signed_kind_wins_when_valid_and_usable);
  return UNITY_END();
}
