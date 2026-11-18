// bug-183 : comparaison d'octets de largeur fixe (logique pure, ADR-0017).
// Ces tests fixent le RÉSULTAT. Que la comparaison ne s'arrête pas au premier écart
// (propriété de temps) n'est pas observable en natif : voir la spec bug-183.
#include <cstdint>
#include <cstring>
#include <unity.h>
#include "secret_compare.h"

void setUp(void) {}
void tearDown(void) {}

static void fill(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + 3); }

void test_octets_identiques_sont_egaux(void) {
  uint8_t a[32], b[32]; fill(a, 32); fill(b, 32);
  TEST_ASSERT_TRUE(fixedWidthEquals(a, b, 32));
}
void test_difference_au_premier_octet(void) {
  uint8_t a[32], b[32]; fill(a, 32); fill(b, 32); b[0] ^= 0x10;
  TEST_ASSERT_FALSE(fixedWidthEquals(a, b, 32));
}
void test_difference_au_dernier_octet(void) {
  uint8_t a[32], b[32]; fill(a, 32); fill(b, 32); b[31] ^= 0x10;
  TEST_ASSERT_FALSE(fixedWidthEquals(a, b, 32));
}
void test_difference_d_un_seul_bit_a_chaque_position(void) {
  for (size_t i = 0; i < 32; i++)
    for (int bit = 0; bit < 8; bit++) {
      uint8_t a[32], b[32]; fill(a, 32); fill(b, 32); b[i] ^= (uint8_t)(1u << bit);
      TEST_ASSERT_FALSE(fixedWidthEquals(a, b, 32));
    }
}
// n = 0 : aucun octet à comparer, donc aucun écart => vrai (égalité vide, comme memcmp(...,0)==0).
// Les appelants passent toujours 32 ; le cas ne doit jamais rendre « faux » ni lire un octet.
void test_n_zero_est_egal_sans_lecture(void) {
  TEST_ASSERT_TRUE(fixedWidthEquals(nullptr, nullptr, 0));
}
void test_meme_tampon_est_egal(void) {
  uint8_t a[32]; fill(a, 32);
  TEST_ASSERT_TRUE(fixedWidthEquals(a, a, 32));
}
void test_ne_compare_que_n_octets(void) {
  uint8_t a[32], b[32]; fill(a, 32); fill(b, 32); b[20] ^= 1;
  TEST_ASSERT_TRUE(fixedWidthEquals(a, b, 20));
  TEST_ASSERT_FALSE(fixedWidthEquals(a, b, 21));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_octets_identiques_sont_egaux);
  RUN_TEST(test_difference_au_premier_octet);
  RUN_TEST(test_difference_au_dernier_octet);
  RUN_TEST(test_difference_d_un_seul_bit_a_chaque_position);
  RUN_TEST(test_n_zero_est_egal_sans_lecture);
  RUN_TEST(test_meme_tampon_est_egal);
  RUN_TEST(test_ne_compare_que_n_octets);
  return UNITY_END();
}
