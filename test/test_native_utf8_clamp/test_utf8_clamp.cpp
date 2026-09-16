// =============================================================================
// Tests unitaires natifs — utf8_clamp (feature-129, phase B)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32.
//
// CE QUE CETTE SUITE PROUVE.
// utf8ClampInPlace() est l'ex-clampReasonUtf8() de pump_controller.cpp,
// extraite telle quelle (même algorithme) dans un module pur, puis réécrite
// pour s'appuyer sur utf8ClampLen() — la primitive dont log_arena_logic a
// aussi besoin pour clamper des fragments d'arène NON NUL-terminés.
//
// AXE 1 — NON-RÉGRESSION : une réimplémentation littérale de l'ANCIEN
// clampReasonUtf8() (recopiée depuis l'historique git, AVANT extraction) sert
// de référence. Elle est comparée à utf8ClampInPlace() sur un jeu de cas qui
// couvre : ASCII pur, 2 octets (é), 3 octets (—, →), 4 octets (émoji), octet
// de tête invalide, séquence tronquée en fin de tampon, chaîne vide.
//
// AXE 2 — utf8ClampLen() directement, sur des fragments PAS NUL-terminés
// (le cas d'usage réel dans l'arène : `len` est une longueur, pas une fin de
// chaîne C).
// =============================================================================

#include <unity.h>
#include <string.h>

#include "utf8_clamp.h"

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// AXE 1 — Référence : ANCIEN clampReasonUtf8() (pump_controller.cpp, avant
// feature-129). Recopié tel quel depuis l'historique git — ne PAS "améliorer"
// cette fonction, elle doit rester un miroir fidèle de l'ancien comportement.
// =============================================================================
static void oldClampReasonUtf8(char* dst) {
  size_t len = strlen(dst);
  if (len == 0) return;
  size_t start = len - 1;
  while (start > 0 && (static_cast<unsigned char>(dst[start]) & 0xC0) == 0x80) start--;
  const unsigned char lead = static_cast<unsigned char>(dst[start]);
  size_t expected;
  if ((lead & 0x80) == 0x00)      expected = 1;   // ASCII
  else if ((lead & 0xE0) == 0xC0) expected = 2;
  else if ((lead & 0xF0) == 0xE0) expected = 3;
  else if ((lead & 0xF8) == 0xF0) expected = 4;
  else { dst[start] = '\0'; return; }  // octet de tête invalide
  if (start + expected > len) dst[start] = '\0';  // séquence tronquée
}

// Compare ANCIEN et NOUVEAU sur une copie indépendante de `input`, et affirme
// qu'ils produisent EXACTEMENT le même résultat (non-régression stricte).
static void assertSameAsOld(const char* input, const char* caseLabel) {
  char bufOld[256];
  char bufNew[256];
  strncpy(bufOld, input, sizeof(bufOld) - 1);
  bufOld[sizeof(bufOld) - 1] = '\0';
  strncpy(bufNew, input, sizeof(bufNew) - 1);
  bufNew[sizeof(bufNew) - 1] = '\0';

  oldClampReasonUtf8(bufOld);
  utf8ClampInPlace(bufNew);

  TEST_ASSERT_EQUAL_STRING_MESSAGE(bufOld, bufNew, caseLabel);
}

void test_non_regression_ascii_pur_inchange(void) {
  assertSameAsOld("Bidon vide, verifier amorcage", "ASCII pur : rien a couper");
}

void test_non_regression_chaine_vide(void) {
  assertSameAsOld("", "chaine vide : no-op");
}

void test_non_regression_coupe_2_octets_e_accent(void) {
  // "bande morte" coupé en plein milieu du dernier "é" de "coupée" (2 octets,
  // 0xC3 0xA9) — c'est exactement le défaut v2.26.0 documenté dans
  // constants.h : msg[248] coupait "bande mor(te)".
  char truncated[] = "bande mort\xC3";  // dernier octet = tête de séquence 2o, incomplète
  assertSameAsOld(truncated, "coupe en plein 'é' (2o)");
}

void test_non_regression_coupe_3_octets_tiret_cadratin(void) {
  // "—" (tiret cadratin, U+2014) = 0xE2 0x80 0x94. On tronque après le 2e
  // octet de continuation : séquence incomplète.
  char truncated[] = "texte \xE2\x80";  // 2 des 3 octets du tiret cadratin
  assertSameAsOld(truncated, "coupe en plein '—' (3o), 2/3 octets presents");
}

void test_non_regression_coupe_3_octets_fleche(void) {
  // "→" (flèche, U+2192) = 0xE2 0x86 0x92. Un seul octet de tête présent.
  char truncated[] = "texte \xE2";
  assertSameAsOld(truncated, "coupe en plein '->' (3o), 1/3 octet present");
}

void test_non_regression_coupe_4_octets_emoji(void) {
  // U+1F4A7 (goutte d'eau) = 0xF0 0x9F 0x92 0xA7. On tronque à 3 des 4 octets.
  char truncated[] = "eau \xF0\x9F\x92";
  assertSameAsOld(truncated, "coupe en plein caractere 4o, 3/4 octets presents");
}

void test_non_regression_octet_tete_invalide(void) {
  // 0xFF n'est un octet de tête valide dans AUCUN schéma UTF-8 : lecture
  // déchirée en amont (donnée corrompue). L'ancien ET le nouveau doivent le
  // retirer.
  char corrupted[] = "avant\xFF";
  assertSameAsOld(corrupted, "octet de tete invalide (0xFF) retire");
}

void test_non_regression_caractere_complet_non_touche(void) {
  // "café" se termine sur un 'é' COMPLET (2 octets présents en entier) : ni
  // l'ancien ni le nouveau ne doivent rien couper.
  assertSameAsOld("caf\xC3\xA9", "caractere final complet : inchange");
}

void test_non_regression_message_garde_realiste_avec_accents(void) {
  // Un message de garde réel, riche en accents/tirets/flèches français, coupé
  // à une frontière arbitraire (simulateur de tampon plein) : preuve sur un
  // cas représentatif, pas seulement synthétique.
  char msg[] = "Ecart : projection annonce 1.23 mL/min alors que le calcul "
               "applique 4.56 mL/min \xE2\x80\x94 comparer doseComposition() "
               "et la branche automatique \xE2\x86";  // coupé en plein "->"
  assertSameAsOld(msg, "message de garde realiste, coupe en plein fleche");
}

// =============================================================================
// AXE 2 — utf8ClampLen() sur fragments NON NUL-terminés (cas d'usage arène).
// =============================================================================

void test_len_ascii_len_inchangee(void) {
  const char s[] = "abcdef";
  TEST_ASSERT_EQUAL_size_t(6, utf8ClampLen(s, 6));
}

void test_len_zero_retourne_zero(void) {
  TEST_ASSERT_EQUAL_size_t(0, utf8ClampLen("abc", 0));
}

void test_len_coupe_e_2_octets_incomplet(void) {
  // "caf" + 0xC3 seul (tête de 'é' sans continuation) : len doit retomber à 3.
  const char s[] = {'c', 'a', 'f', (char)0xC3};
  TEST_ASSERT_EQUAL_size_t(3, utf8ClampLen(s, 4));
}

void test_len_e_complet_conserve(void) {
  // "caf" + 0xC3 0xA9 ('é' complet) : len doit rester 5.
  const char s[] = {'c', 'a', 'f', (char)0xC3, (char)0xA9};
  TEST_ASSERT_EQUAL_size_t(5, utf8ClampLen(s, 5));
}

void test_len_coupe_tiret_cadratin_3_octets_2_presents(void) {
  // "x" + 0xE2 0x80 (2 des 3 octets de "—") : retombe a 1 (le 'x').
  const char s[] = {'x', (char)0xE2, (char)0x80};
  TEST_ASSERT_EQUAL_size_t(1, utf8ClampLen(s, 3));
}

void test_len_tiret_cadratin_complet_conserve(void) {
  const char s[] = {'x', (char)0xE2, (char)0x80, (char)0x94};
  TEST_ASSERT_EQUAL_size_t(4, utf8ClampLen(s, 4));
}

void test_len_coupe_emoji_4_octets_3_presents(void) {
  const char s[] = {'y', (char)0xF0, (char)0x9F, (char)0x92};
  TEST_ASSERT_EQUAL_size_t(1, utf8ClampLen(s, 4));
}

void test_len_emoji_complet_conserve(void) {
  const char s[] = {'y', (char)0xF0, (char)0x9F, (char)0x92, (char)0xA7};
  TEST_ASSERT_EQUAL_size_t(5, utf8ClampLen(s, 5));
}

void test_len_octet_tete_invalide_seul(void) {
  const char s[] = {(char)0xFF};
  TEST_ASSERT_EQUAL_size_t(0, utf8ClampLen(s, 1));
}

void test_len_octet_continuation_seul_recule_jusqu_au_debut(void) {
  // Un seul octet de continuation isolé (0x80..0xBF sans tête) : la boucle de
  // recul s'arrête à start==0 (garde `start > 0`), puis cet octet est
  // considéré comme tête -> invalide -> retiré (longueur 0).
  const char s[] = {(char)0x80};
  TEST_ASSERT_EQUAL_size_t(0, utf8ClampLen(s, 1));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_non_regression_ascii_pur_inchange);
  RUN_TEST(test_non_regression_chaine_vide);
  RUN_TEST(test_non_regression_coupe_2_octets_e_accent);
  RUN_TEST(test_non_regression_coupe_3_octets_tiret_cadratin);
  RUN_TEST(test_non_regression_coupe_3_octets_fleche);
  RUN_TEST(test_non_regression_coupe_4_octets_emoji);
  RUN_TEST(test_non_regression_octet_tete_invalide);
  RUN_TEST(test_non_regression_caractere_complet_non_touche);
  RUN_TEST(test_non_regression_message_garde_realiste_avec_accents);
  RUN_TEST(test_len_ascii_len_inchangee);
  RUN_TEST(test_len_zero_retourne_zero);
  RUN_TEST(test_len_coupe_e_2_octets_incomplet);
  RUN_TEST(test_len_e_complet_conserve);
  RUN_TEST(test_len_coupe_tiret_cadratin_3_octets_2_presents);
  RUN_TEST(test_len_tiret_cadratin_complet_conserve);
  RUN_TEST(test_len_coupe_emoji_4_octets_3_presents);
  RUN_TEST(test_len_emoji_complet_conserve);
  RUN_TEST(test_len_octet_tete_invalide_seul);
  RUN_TEST(test_len_octet_continuation_seul_recule_jusqu_au_debut);
  return UNITY_END();
}
