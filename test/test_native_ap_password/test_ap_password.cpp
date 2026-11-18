// bug-187 : fabrication et controle des mots de passe (logique pure, ADR-0017).
//
// ⭐ POURQUOI CES TESTS EXISTENT : aucun essai sur materiel ne montrerait un biais de
// tirage ni le comportement d'une boucle de rejet affamee. On ne verrait qu'un mot de
// passe « qui a l'air normal ». Ce sont exactement les defauts que ce fichier attrape.
//
// ⛔ CE QUE CES TESTS NE PROUVENT PAS : la QUALITE de la source d'alea. esp_random() est
// remplace ici par des fournisseurs deterministes. Que le vrai generateur materiel soit
// bon se verifie ailleurs (condition A1 de la revue P1 : tirage radio demarree).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unity.h>
#include "ap_password_logic.h"

void setUp(void) {}
void tearDown(void) {}

// --- Fournisseurs d'alea deterministes ---------------------------------------
// Generateur congruentiel : reproductible, donc un echec est rejouable.
struct Lcg {
  uint32_t s;
  uint8_t operator()() {
    s = s * 1664525u + 1013904223u;
    return static_cast<uint8_t>((s >> 24) & 0xFF);
  }
};

// --- Familles ----------------------------------------------------------------
static void test_familles_classent_chaque_caractere_imprimable(void) {
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilyLower, apPwdFamilyOf('a'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilyLower, apPwdFamilyOf('z'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilyUpper, apPwdFamilyOf('A'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilyUpper, apPwdFamilyOf('Z'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilyDigit, apPwdFamilyOf('0'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilyDigit, apPwdFamilyOf('9'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilySpecial, apPwdFamilyOf('#'));
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilySpecial, apPwdFamilyOf('^'));  // accepte a la SAISIE
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamilySpecial, apPwdFamilyOf(' '));  // l'espace est licite WPA2
}

static void test_un_octet_hors_ascii_imprimable_n_est_pas_une_famille(void) {
  TEST_ASSERT_EQUAL_UINT8(0, apPwdFamilyOf(static_cast<char>(31)));   // controle
  TEST_ASSERT_EQUAL_UINT8(0, apPwdFamilyOf(static_cast<char>(127)));  // DEL
  TEST_ASSERT_EQUAL_UINT8(0, apPwdFamilyOf(static_cast<char>(0xC3)));  // UTF-8, « e accent »
  // ⛔ LE PIEGE : un accent ne doit pas etre compte comme « caractere special ».
  // Il passerait le controle des quatre familles et casserait la connexion WPA2.
  const char accentue[] = "Abcdef2#\xC3\xA9xyz";
  TEST_ASSERT_EQUAL_UINT8(kApPwdFamiliesInvalid,
                          apPwdFamiliesPresent(accentue, strlen(accentue)));
}

// --- Controle d'un mot de passe CHOISI ---------------------------------------
static void test_controle_accepte_un_mot_de_passe_conforme(void) {
  const char bon[] = "Kq7#mP2$xJ4t";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(bon, strlen(bon), kApPwdMinLength, kApPwdMaxLength, false)));
}

static void test_controle_nomme_la_famille_manquante_et_pas_autre_chose(void) {
  // 12 caracteres a chaque fois : seule la famille absente change.
  const char sansChiffre[] = "Kqa#mPbb$xJt";
  const char sansSpecial[] = "Kqa7mPbb2xJt";
  const char sansMajusc[] = "kqa7#mpbb2xj";
  const char sansMinusc[] = "KQA7#MPBB2XJ";
  const size_t mn = kApPwdMinLength, mx = kApPwdMaxLength;
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kMissingFamily),
                    static_cast<int>(apPwdCheck(sansChiffre, 12, mn, mx, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kMissingFamily),
                    static_cast<int>(apPwdCheck(sansSpecial, 12, mn, mx, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kMissingFamily),
                    static_cast<int>(apPwdCheck(sansMajusc, 12, mn, mx, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kMissingFamily),
                    static_cast<int>(apPwdCheck(sansMinusc, 12, mn, mx, false)));
}

static void test_controle_refuse_trop_court_AVANT_de_parler_des_familles(void) {
  // ⚠️ L'ORDRE COMPTE pour le message affiche : dire « il manque un caractere
  // special » sur un mot de passe de 3 caracteres envoie l'utilisateur corriger
  // la mauvaise chose. Il doit d'abord l'allonger.
  //
  // ⭐ CE CAS EST CELUI QUI DISCRIMINE, et il a fallu une MUTATION pour s'en
  // apercevoir : le premier essai ecrit ici etait "Kq7#", qui porte DEJA les
  // quatre familles. Inverser l'ordre des controles rendait donc le MEME verdict,
  // et le test passait au vert sur un code faux. Il faut un mot de passe
  // simultanement TROP COURT et INCOMPLET.
  const char courtEtIncomplet[] = "Kq7";  // 3 caracteres, et aucun caractere special
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kTooShort),
                    static_cast<int>(apPwdCheck(courtEtIncomplet, strlen(courtEtIncomplet),
                                                kApPwdMinLength, kApPwdMaxLength, false)));

  // Et le symetrique : assez long, mais incomplet => c'est bien la famille qu'on nomme.
  const char longEtIncomplet[] = "Kqabcdefghij";  // 12 caracteres, ni chiffre ni special
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kMissingFamily),
                    static_cast<int>(apPwdCheck(longEtIncomplet, strlen(longEtIncomplet),
                                                kApPwdMinLength, kApPwdMaxLength, false)));
}

static void test_controle_refuse_au_dela_du_plafond_wpa2(void) {
  char trop[80];
  memset(trop, 'a', sizeof(trop));
  trop[0] = 'A'; trop[1] = '7'; trop[2] = '#';
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kTooLong),
                    static_cast<int>(apPwdCheck(trop, 64, kApPwdMinLength, kApPwdMaxLength, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(trop, 63, kApPwdMinLength, kApPwdMaxLength, false)));
}

static void test_controle_accepte_un_symbole_hors_alphabet_de_fabrication(void) {
  // ⭐ LE POINT A NE PAS CONFONDRE : `^` et `l` sont EXCLUS de la fabrication
  // (touche morte, confusion de lecture) mais un gestionnaire de mots de passe
  // peut tres bien en produire. Les refuser serait absurde.
  const char gestionnaire[] = "lI10O^oQ7#xZ";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(gestionnaire, strlen(gestionnaire),
                                                kApPwdMinLength, kApPwdMaxLength, false)));
}

static void test_controle_le_plancher_est_un_parametre_pas_une_constante(void) {
  // Le mot de passe de l'APPAREIL a un plancher plus bas que la cle Wi-Fi.
  const char huit[] = "Kq7#mP2$";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(huit, 8, 8, kApPwdMaxLength, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kTooShort),
                    static_cast<int>(apPwdCheck(huit, 8, kApPwdMinLength, kApPwdMaxLength, false)));
}

// --- Tirage ------------------------------------------------------------------
static void test_l_alphabet_de_fabrication_fait_bien_72_symboles(void) {
  TEST_ASSERT_EQUAL_size_t(72, kApPwdGenAlphabetN);
  TEST_ASSERT_EQUAL_size_t(kApPwdGenLowerN, strlen(apPwdGenLower()));
  TEST_ASSERT_EQUAL_size_t(kApPwdGenUpperN, strlen(apPwdGenUpper()));
  TEST_ASSERT_EQUAL_size_t(kApPwdGenDigitN, strlen(apPwdGenDigit()));
  TEST_ASSERT_EQUAL_size_t(kApPwdGenSpecialN, strlen(apPwdGenSpecial()));
}

static void test_aucun_caractere_confondable_ni_touche_morte_dans_la_fabrication(void) {
  // ⛔ CE TEST GARDE UNE DECISION PRODUIT, pas une propriete mathematique :
  // I l 1 O 0 o se confondent a la lecture d'une etiquette, et `^` est une
  // TOUCHE MORTE sur un clavier francais (`^` puis `a` donne `a` accentue).
  for (size_t i = 0; i < kApPwdGenAlphabetN; ++i) {
    const char c = apPwdSymbolAt(i);
    TEST_ASSERT_TRUE_MESSAGE(c != 'I' && c != 'l' && c != '1' && c != 'O' && c != '0' &&
                                 c != 'o' && c != '^',
                             "caractere confondable ou touche morte dans l'alphabet");
  }
}

static void test_un_indice_hors_alphabet_rend_le_caractere_nul(void) {
  TEST_ASSERT_EQUAL_CHAR('\0', apPwdSymbolAt(kApPwdGenAlphabetN));
  TEST_ASSERT_EQUAL_CHAR('\0', apPwdSymbolAt(1000));
}

static void test_le_rejet_d_octet_supprime_le_biais_de_modulo(void) {
  // ⭐ LE TEST QUI JUSTIFIE LA BOUCLE DE REJET. Sans elle, les 40 premiers
  // symboles sortiraient 4 fois sur 256 et les 32 autres 3 fois.
  size_t comptes[kApPwdGenAlphabetN] = {0};
  for (int b = 0; b < 256; ++b) {
    const uint8_t o = static_cast<uint8_t>(b);
    if (!apPwdByteAccepted(o)) continue;
    comptes[apPwdIndexFromByte(o)]++;
  }
  for (size_t i = 0; i < kApPwdGenAlphabetN; ++i) {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(3, comptes[i], "tirage biaise : symbole non equiprobable");
  }
  TEST_ASSERT_EQUAL_UINT8(216, kApPwdByteLimit);  // 3 x 72, exact
}

static void test_sans_rejet_le_biais_serait_bien_reel(void) {
  // Contre-epreuve : on montre que le defaut existe si l'on retire la garde.
  size_t comptes[kApPwdGenAlphabetN] = {0};
  for (int b = 0; b < 256; ++b) comptes[static_cast<uint8_t>(b) % kApPwdGenAlphabetN]++;
  size_t mini = 999, maxi = 0;
  for (size_t i = 0; i < kApPwdGenAlphabetN; ++i) {
    if (comptes[i] < mini) mini = comptes[i];
    if (comptes[i] > maxi) maxi = comptes[i];
  }
  TEST_ASSERT_EQUAL_size_t(3, mini);
  TEST_ASSERT_EQUAL_size_t(4, maxi);  // 40 symboles sortiraient 33 % plus souvent
}

static void test_le_mot_de_passe_fabrique_respecte_toujours_les_quatre_familles(void) {
  char out[kApPwdGenLength + 1];
  for (uint32_t graine = 1; graine <= 400; ++graine) {
    Lcg alea{graine};
    TEST_ASSERT_TRUE(apPwdGenerate(alea, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(kApPwdGenLength, strlen(out));
    TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                      static_cast<int>(apPwdCheck(out, strlen(out), kApPwdMinLength, kApPwdMaxLength, false)));
  }
}

static void test_le_mot_de_passe_fabrique_ne_sort_jamais_de_son_alphabet(void) {
  char out[kApPwdGenLength + 1];
  for (uint32_t graine = 1; graine <= 200; ++graine) {
    Lcg alea{graine};
    TEST_ASSERT_TRUE(apPwdGenerate(alea, out, sizeof(out)));
    for (size_t i = 0; i < strlen(out); ++i) {
      bool trouve = false;
      for (size_t j = 0; j < kApPwdGenAlphabetN && !trouve; ++j) trouve = (apPwdSymbolAt(j) == out[i]);
      TEST_ASSERT_TRUE_MESSAGE(trouve, "symbole hors alphabet de fabrication");
    }
  }
}

static void test_deux_graines_differentes_donnent_deux_mots_de_passe_differents(void) {
  char a[kApPwdGenLength + 1], b[kApPwdGenLength + 1];
  Lcg g1{12345}, g2{67890};
  TEST_ASSERT_TRUE(apPwdGenerate(g1, a, sizeof(a)));
  TEST_ASSERT_TRUE(apPwdGenerate(g2, b, sizeof(b)));
  TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

// --- ⛔ LES ECHECS : c'est ici que se joue le point d'acces OUVERT ------------
static void test_un_fournisseur_qui_ne_rend_que_des_octets_rejetes_echoue_SANS_chaine_vide(void) {
  // ⛔ LE DEFAUT REDOUTE : rendre "" ferait demarrer le point d'acces SANS
  // CHIFFREMENT (AP.cpp:236-240), et /wifi/connect est ouvert sans jeton en mode AP.
  auto toujoursRejete = []() -> uint8_t { return 0xFF; };  // 255 >= 216
  char out[kApPwdGenLength + 1];
  memset(out, 'X', sizeof(out));
  TEST_ASSERT_FALSE(apPwdGenerate(toujoursRejete, out, sizeof(out)));
  TEST_ASSERT_EQUAL_CHAR('\0', out[0]);  // vide ET signale : l'appelant GARDE l'ancien
}

static void test_un_fournisseur_constant_ne_peut_pas_porter_quatre_familles(void) {
  // Octet 0 accepte => toujours le meme symbole => jamais les quatre familles.
  auto toujoursZero = []() -> uint8_t { return 0; };
  char out[kApPwdGenLength + 1];
  TEST_ASSERT_FALSE(apPwdGenerate(toujoursZero, out, sizeof(out)));
  TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

static void test_la_generation_refuse_un_tampon_trop_petit(void) {
  Lcg alea{7};
  char trop[kApPwdGenLength];  // il manque la place du zero terminal
  TEST_ASSERT_FALSE(apPwdGenerate(alea, trop, sizeof(trop)));
  char* nul = nullptr;
  Lcg alea2{7};
  TEST_ASSERT_FALSE(apPwdGenerate(alea2, nul, 32));
}

static void test_les_boucles_sont_bornees_donc_la_generation_termine(void) {
  // Un fournisseur pathologique ne doit pas bloquer la tache : on compte les appels.
  static size_t appels = 0;
  appels = 0;
  auto compte = []() -> uint8_t { appels++; return 0xFF; };
  char out[kApPwdGenLength + 1];
  TEST_ASSERT_FALSE(apPwdGenerate(compte, out, sizeof(out)));
  TEST_ASSERT_TRUE_MESSAGE(appels <= kApPwdMaxByteDraws + 1, "boucle de tirage non bornee");
}

// --- ⛔ La garde du site softAP ----------------------------------------------
static void test_une_phrase_trop_courte_ou_vide_ne_doit_jamais_demarrer_le_point_d_acces(void) {
  // ⭐ CE PREDICAT EXISTE PARCE QU'UN MESSAGE NE GARDE RIEN. Le code annoncait
  // « le point d'acces NE DEMARRERA PAS » alors que rien ne l'en empechait :
  // softAP() ne pose WPA2 que si la phrase est NON VIDE, et cree sinon un reseau
  // OUVERT sans la moindre erreur.
  TEST_ASSERT_FALSE(apPwdUsableAsWpaKey(0));   // ⛔ le cas qui ouvrait le reseau
  TEST_ASSERT_FALSE(apPwdUsableAsWpaKey(7));   // refuse par softAP lui-meme
  TEST_ASSERT_TRUE(apPwdUsableAsWpaKey(8));    // plancher WPA2
  TEST_ASSERT_TRUE(apPwdUsableAsWpaKey(kApPwdGenLength));
  TEST_ASSERT_TRUE(apPwdUsableAsWpaKey(63));
  TEST_ASSERT_FALSE(apPwdUsableAsWpaKey(64));  // au-dela du plafond WPA2
}

static void test_tout_mot_de_passe_fabrique_est_utilisable_comme_cle_wpa(void) {
  char out[kApPwdGenLength + 1];
  for (uint32_t graine = 1; graine <= 100; ++graine) {
    Lcg alea{graine};
    TEST_ASSERT_TRUE(apPwdGenerate(alea, out, sizeof(out)));
    TEST_ASSERT_TRUE(apPwdUsableAsWpaKey(strlen(out)));
  }
}

// --- Espaces de bord ---------------------------------------------------------
static void test_un_espace_en_tete_ou_en_fin_est_refuse(void) {
  // ⛔ WPA2 l'accepte, et c'est TOUT le probleme : il est INVISIBLE. Colle depuis un
  // gestionnaire, il produit un refus de connexion que personne ne peut diagnostiquer.
  const char devant[] = " Kq7#mP2$xJ4t";
  const char derriere[] = "Kq7#mP2$xJ4t ";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kBadCharacter),
                    static_cast<int>(apPwdCheck(devant, strlen(devant), kApPwdMinLength, kApPwdMaxLength, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kBadCharacter),
                    static_cast<int>(apPwdCheck(derriere, strlen(derriere), kApPwdMinLength, kApPwdMaxLength, false)));
  // ⚠️ Mais un espace INTERIEUR reste licite : « Kq7 mP2$xJ4t » est une phrase valable.
  const char milieu[] = "Kq7 mP2$xJ4t";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(milieu, strlen(milieu), kApPwdMinLength, kApPwdMaxLength, false)));
}

// --- ⭐ Le mot de passe de l'APPAREIL n'a pas les contraintes du Wi-Fi ---------
static void test_un_accent_est_refuse_pour_le_wifi_mais_accepte_pour_l_appareil(void) {
  // ⭐ CE TEST GARDE UN DEFAUT QUE J'AVAIS INTRODUIT : en reutilisant le controle
  // de la cle WPA2 pour le mot de passe de l'appareil, « Ete2024! » avec un accent
  // etait REFUSE. Or il voyage en JSON, pas dans une trame Wi-Fi : rien ne l'interdit,
  // et le refuser serait absurde pour un utilisateur francais.
  // ⚠️ Il FAUT une majuscule ASCII : « É » est tolere mais ne compte pour AUCUNE
  // famille — c'est voulu, et mon premier essai s'y est fait prendre.
  const char accentue[] = "\xC3\xA9" "te2024!Xyz";  // « été2024!Xyz » en UTF-8
  const size_t n = strlen(accentue);
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kBadCharacter),
                    static_cast<int>(apPwdCheck(accentue, n, kApPwdMinLength, kApPwdMaxLength, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(accentue, n, 8, 128, true)));
}

static void test_un_accent_ne_permet_pas_de_contourner_la_regle_des_familles(void) {
  // ⛔ Tolere ne veut pas dire COMPTE.
  // ⭐ CE CAS EST CELUI QUI DISCRIMINE, et il a fallu une MUTATION pour le trouver :
  // mon premier essai manquait DEUX familles (majuscule ET special), donc faire
  // compter l'accent comme une majuscule rendait quand meme kMissingFamily et le test
  // restait vert sur un code faux. Ici il ne manque QUE la majuscule, et seul l'accent
  // pourrait la fournir : « ée1!abcdefgh » n'a aucune majuscule ASCII.
  // ⛔ CONCATÉNATION OBLIGATOIRE : en C, `\x` avale TOUS les chiffres hexadécimaux qui
  // suivent. « \xA9e » ne vaut donc PAS « é » suivi de « e » mais UN caractère 0xA9E.
  // Les deux chaînes de ce fichier s'y étaient fait prendre, et l'une passait au vert
  // par accident. Couper la chaîne force la fin de l'échappement.
  const char accent_seul_candidat[] = "\xC3\xA9" "e1!abcdefgh";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kMissingFamily),
                    static_cast<int>(apPwdCheck(accent_seul_candidat, strlen(accent_seul_candidat),
                                                8, 128, true)));
  // Et la contre-epreuve : une vraie majuscule ASCII, et tout passe.
  const char avec_vraie_majuscule[] = "\xC3\xA9" "E1!abcdefgh";
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kOk),
                    static_cast<int>(apPwdCheck(avec_vraie_majuscule, strlen(avec_vraie_majuscule),
                                                8, 128, true)));
}

static void test_un_caractere_de_controle_reste_refuse_dans_les_deux_cas(void) {
  char avecControle[] = "Kq7#mP2$xJ4t";
  avecControle[5] = static_cast<char>(9);  // tabulation
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kBadCharacter),
                    static_cast<int>(apPwdCheck(avecControle, 12, kApPwdMinLength, kApPwdMaxLength, false)));
  TEST_ASSERT_EQUAL(static_cast<int>(ApPwdVerdict::kBadCharacter),
                    static_cast<int>(apPwdCheck(avecControle, 12, 8, 128, true)));
}

// --- Ancienne forme ----------------------------------------------------------
static void test_reconnait_l_ancienne_forme_a_huit_hexadecimaux(void) {
  TEST_ASSERT_TRUE(apPwdIsLegacyForm("A1B2C3D4", 8));
  TEST_ASSERT_TRUE(apPwdIsLegacyForm("deadbeef", 8));
}

static void test_ne_confond_pas_un_mot_de_passe_neuf_avec_l_ancienne_forme(void) {
  // La longueur suffit a discriminer : 12 contre 8. Aucun faux positif possible.
  char out[kApPwdGenLength + 1];
  for (uint32_t graine = 1; graine <= 100; ++graine) {
    Lcg alea{graine};
    TEST_ASSERT_TRUE(apPwdGenerate(alea, out, sizeof(out)));
    TEST_ASSERT_FALSE(apPwdIsLegacyForm(out, strlen(out)));
  }
  TEST_ASSERT_FALSE(apPwdIsLegacyForm("A1B2C3D", 7));
  TEST_ASSERT_FALSE(apPwdIsLegacyForm("A1B2C3D4E", 9));
  TEST_ASSERT_FALSE(apPwdIsLegacyForm("A1B2C3G4", 8));  // G n'est pas hexadecimal
  TEST_ASSERT_FALSE(apPwdIsLegacyForm(nullptr, 8));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_familles_classent_chaque_caractere_imprimable);
  RUN_TEST(test_un_octet_hors_ascii_imprimable_n_est_pas_une_famille);
  RUN_TEST(test_controle_accepte_un_mot_de_passe_conforme);
  RUN_TEST(test_controle_nomme_la_famille_manquante_et_pas_autre_chose);
  RUN_TEST(test_controle_refuse_trop_court_AVANT_de_parler_des_familles);
  RUN_TEST(test_controle_refuse_au_dela_du_plafond_wpa2);
  RUN_TEST(test_controle_accepte_un_symbole_hors_alphabet_de_fabrication);
  RUN_TEST(test_controle_le_plancher_est_un_parametre_pas_une_constante);
  RUN_TEST(test_l_alphabet_de_fabrication_fait_bien_72_symboles);
  RUN_TEST(test_aucun_caractere_confondable_ni_touche_morte_dans_la_fabrication);
  RUN_TEST(test_un_indice_hors_alphabet_rend_le_caractere_nul);
  RUN_TEST(test_le_rejet_d_octet_supprime_le_biais_de_modulo);
  RUN_TEST(test_sans_rejet_le_biais_serait_bien_reel);
  RUN_TEST(test_le_mot_de_passe_fabrique_respecte_toujours_les_quatre_familles);
  RUN_TEST(test_le_mot_de_passe_fabrique_ne_sort_jamais_de_son_alphabet);
  RUN_TEST(test_deux_graines_differentes_donnent_deux_mots_de_passe_differents);
  RUN_TEST(test_un_fournisseur_qui_ne_rend_que_des_octets_rejetes_echoue_SANS_chaine_vide);
  RUN_TEST(test_un_fournisseur_constant_ne_peut_pas_porter_quatre_familles);
  RUN_TEST(test_la_generation_refuse_un_tampon_trop_petit);
  RUN_TEST(test_les_boucles_sont_bornees_donc_la_generation_termine);
  RUN_TEST(test_une_phrase_trop_courte_ou_vide_ne_doit_jamais_demarrer_le_point_d_acces);
  RUN_TEST(test_tout_mot_de_passe_fabrique_est_utilisable_comme_cle_wpa);
  RUN_TEST(test_un_espace_en_tete_ou_en_fin_est_refuse);
  RUN_TEST(test_un_accent_est_refuse_pour_le_wifi_mais_accepte_pour_l_appareil);
  RUN_TEST(test_un_accent_ne_permet_pas_de_contourner_la_regle_des_familles);
  RUN_TEST(test_un_caractere_de_controle_reste_refuse_dans_les_deux_cas);
  RUN_TEST(test_reconnait_l_ancienne_forme_a_huit_hexadecimaux);
  RUN_TEST(test_ne_confond_pas_un_mot_de_passe_neuf_avec_l_ancienne_forme);
  return UNITY_END();
}
