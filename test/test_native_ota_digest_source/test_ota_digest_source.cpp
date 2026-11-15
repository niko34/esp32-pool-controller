// =============================================================================
// Tests unitaires natifs — ota_digest_source_logic (feature-151 D3-a)
// =============================================================================
// Table de vérité de chooseOtaExpectedDigestSource :
//   - source = manifeste SSI authentique ET hex du manifeste exploitable ;
//   - sinon repli sur le paramètre de l'appelant ;
//   - divergent = les deux empreintes sont disponibles ET différentes,
//     comparaison INSENSIBLE À LA CASSE, indépendamment de la source retenue.
// =============================================================================

#include <stdio.h>
#include <unity.h>

#include "ota_digest_source_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const char* HEX_A = "a19d0444d96180703c7a52c0f2dfdf6a00d3020ff61ab95eac4d7664cc63d4a1";
static const char* HEX_A_UP = "A19D0444D96180703C7A52C0F2DFDF6A00D3020FF61AB95EAC4D7664CC63D4A1";
static const char* HEX_A_MIX = "a19d0444d96180703c7a52c0f2dfdf6a00d3020ff61ab95eac4d7664cc63D4A1";
static const char* HEX_B = "19ba22b001d0e53496d2ca02700955b54bb2839e9765746be37e1fa5713cf71e";
// Diffère de HEX_A par UN seul caractère (dernier : 1 -> 2).
static const char* HEX_A_1DIFF = "a19d0444d96180703c7a52c0f2dfdf6a00d3020ff61ab95eac4d7664cc63d4a2";

static OtaDigestSourceDecision run(bool auth, bool mValid, const char* mHex, bool cValid,
                                   const char* cHex) {
  return chooseOtaExpectedDigestSource(auth, mValid, mHex, cValid, cHex);
}

// --- Source -----------------------------------------------------------------
void test_manifest_when_valid_and_hex_usable(void) {
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kManifest,
                    (int)run(true, true, HEX_A, true, HEX_A).source);
  // même quand le paramètre est absent/invalide
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kManifest,
                    (int)run(true, true, HEX_A, false, nullptr).source);
}

void test_caller_param_when_not_authentic(void) {
  // Manifeste exploitable mais PAS authentique : le paramètre reste la source.
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kCallerParam,
                    (int)run(false, true, HEX_A, true, HEX_B).source);
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kCallerParam,
                    (int)run(false, true, HEX_A, true, HEX_A).source);
}

void test_caller_param_when_manifest_hex_unusable(void) {
  // authentique mais hex du manifeste inexploitable (drapeau faux) ou nul
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kCallerParam,
                    (int)run(true, false, HEX_A, true, HEX_B).source);
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kCallerParam,
                    (int)run(true, true, nullptr, true, HEX_B).source);
  TEST_ASSERT_EQUAL((int)OtaExpectedDigestSource::kCallerParam,
                    (int)run(true, false, nullptr, false, nullptr).source);
}

void test_source_truth_table_exhaustive(void) {
  for (int auth = 0; auth < 2; ++auth) {
    for (int mValid = 0; mValid < 2; ++mValid) {
      for (int mNull = 0; mNull < 2; ++mNull) {
        const bool expectManifest = auth && mValid && !mNull;
        const OtaDigestSourceDecision d =
            run(auth, mValid, mNull ? nullptr : HEX_A, true, HEX_B);
        TEST_ASSERT_EQUAL(expectManifest ? (int)OtaExpectedDigestSource::kManifest
                                         : (int)OtaExpectedDigestSource::kCallerParam,
                          (int)d.source);
      }
    }
  }
}

// --- Divergence -------------------------------------------------------------
void test_divergent_on_different_hex(void) {
  TEST_ASSERT_TRUE(run(true, true, HEX_A, true, HEX_B).divergent);
  TEST_ASSERT_TRUE(run(true, true, HEX_A, true, HEX_A_1DIFF).divergent);
  // indépendant de la source retenue (hors kValid aussi)
  TEST_ASSERT_TRUE(run(false, true, HEX_A, true, HEX_B).divergent);
}

void test_not_divergent_on_identical_hex(void) {
  TEST_ASSERT_FALSE(run(true, true, HEX_A, true, HEX_A).divergent);
  TEST_ASSERT_FALSE(run(false, true, HEX_A, true, HEX_A).divergent);
}

// ⚠️ Casse : la comparaison est insensible à la casse => PAS de divergence.
void test_case_insensitive_comparison(void) {
  TEST_ASSERT_FALSE(run(true, true, HEX_A, true, HEX_A_UP).divergent);
  TEST_ASSERT_FALSE(run(true, true, HEX_A_UP, true, HEX_A).divergent);
  TEST_ASSERT_FALSE(run(true, true, HEX_A, true, HEX_A_MIX).divergent);
  // … mais une vraie différence d'un caractère reste vue en majuscules.
  TEST_ASSERT_TRUE(run(true, true, HEX_A_UP, true, HEX_A_1DIFF).divergent);
}

void test_not_divergent_when_a_side_is_missing(void) {
  TEST_ASSERT_FALSE(run(true, false, HEX_A, true, HEX_B).divergent);   // manifeste inexploitable
  TEST_ASSERT_FALSE(run(true, true, nullptr, true, HEX_B).divergent);  // hex manifeste nul
  TEST_ASSERT_FALSE(run(true, true, HEX_A, false, HEX_B).divergent);   // paramètre invalide
  TEST_ASSERT_FALSE(run(true, true, HEX_A, true, nullptr).divergent);  // hex paramètre nul
}

// Une chaîne de longueur ≠ 64 n'est jamais « égale » : divergence signalée.
void test_wrong_length_counts_as_divergent(void) {
  TEST_ASSERT_TRUE(run(true, true, HEX_A, true, "a19d").divergent);
  char longer[80];
  snprintf(longer, sizeof(longer), "%sff", HEX_A);
  TEST_ASSERT_TRUE(run(true, true, HEX_A, true, longer).divergent);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_manifest_when_valid_and_hex_usable);
  RUN_TEST(test_caller_param_when_not_authentic);
  RUN_TEST(test_caller_param_when_manifest_hex_unusable);
  RUN_TEST(test_source_truth_table_exhaustive);
  RUN_TEST(test_divergent_on_different_hex);
  RUN_TEST(test_not_divergent_on_identical_hex);
  RUN_TEST(test_case_insensitive_comparison);
  RUN_TEST(test_not_divergent_when_a_side_is_missing);
  RUN_TEST(test_wrong_length_counts_as_divergent);
  return UNITY_END();
}
