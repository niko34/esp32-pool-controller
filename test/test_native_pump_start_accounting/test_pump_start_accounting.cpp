// =============================================================================
// Tests unitaires natifs — Comptage des DÉMARRAGES de pompe (feature-110)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / I²C / FreeRTOS.
// On teste le COMPORTEMENT observable du contrat documenté en
// src/dosing_logic.h:281-308 via son API publique
// (evaluatePumpStartAccounting, saturatingInc), pas l'implémentation.
//
// POURQUOI CETTE SUITE EXISTE (et pourquoi elle est le garde-fou de la feature)
// Le modèle de biais de comptage s'écrit `V_compté = V_réel + N·δ`. La
// concordance jerricane du 06/09 a mesuré +240 mL sur 5 050 mL réels
// (+4,76 %), soit δ ≈ 0,21 mL par démarrage — MAIS cette estimation n'a de
// sens que si `N` est EXACT :
//   - un `N` sur-compté (p. ex. si `ON→ON` comptait, `N` compterait des TOURS
//     DE BOUCLE : plusieurs ordres de grandeur de trop) ⇒ δ effondré ⇒ le
//     modèle « perte fixe » serait rejeté à tort ;
//   - un `N` sous-compté ⇒ δ absurdement grand ⇒ modèle « perte fixe »
//     validé à tort ⇒ feature-078 calerait un correctif dans le sens qui
//     RELÂCHE les quotas.
// Une erreur de quelques pour cent sur `N` suffit à rendre `V/N` inutilisable.
// Rien d'autre que cette suite ne surveille cette exactitude : la grandeur ne
// se relit qu'après plusieurs JOURS de dosage, sur un geste (changement de
// bidon) qui DÉTRUIT les deux valeurs à relever.
//
// INVARIANTS VERROUILLÉS ICI :
//   1. countStart = !prevOn && onNow — front STRICT, jamais un tour de boucle.
//   2. countAnomaly = anomalyDeltaMs != 0 && creditedDeltaMs > anomalyDeltaMs
//      — frontière `>` STRICTE (un delta pile au seuil est NORMAL), et seuil 0
//      = indicateur DÉSACTIVÉ.
//   3. Les deux sorties sont INDÉPENDANTES (axes croisés, aucun couplage).
//   4. saturatingInc ne reboucle JAMAIS à 0 : un compteur qui reboucle produit
//      un `V/N` silencieusement faux.
//
// NOTE SUR LE PÉRIMÈTRE : la coquille (accountPumpVolume) ne peut pas produire
// `countStart && countAnomaly` simultanément — un delta n'est crédité que si
// `prevOn` était vrai, ce qui exclut le front. Le CONTRAT de la fonction pure
// garde néanmoins les deux axes indépendants, et c'est ce contrat qu'on teste :
// il ne doit pas dépendre d'une propriété de l'appelant pour être correct.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>  // snprintf : diagnostic des balayages combinatoires
#include "dosing_logic.h"
#include "constants.h"  // kAcctDeltaAnomalyMs (verrou de seuil)

void setUp(void) {}
void tearDown(void) {}

// --- Constantes de référence (valeurs réelles du projet) ---------------------
static const uint32_t kSeuil = kAcctDeltaAnomalyMs;  // 5000 ms
static const uint32_t kOff = 0u;                     // seuil 0 = désactivé

// Helper de lisibilité : évalue et rend la structure.
static PumpStartAccounting eval(bool prevOn, bool onNow, uint32_t credited,
                                uint32_t seuil) {
  return evaluatePumpStartAccounting(prevOn, onNow, credited, seuil);
}

// =============================================================================
// 1. FRONT STRICT — countStart = !prevOn && onNow
// =============================================================================

// Les 4 combinaisons de (prevOn, onNow), exhaustivement. C'est LA table de
// vérité du dénominateur `N`. Chaque ligne vérifie AUSSI countAnomaly, ce qui
// verrouille l'ordre des champs du struct retourné.
static void test_F110_front_table_de_verite_4_combinaisons(void) {
  // OFF → OFF : pompe à l'arrêt, rien ne se passe.
  PumpStartAccounting a = eval(false, false, 0u, kSeuil);
  TEST_ASSERT_FALSE_MESSAGE(a.countStart, "OFF->OFF ne demarre pas");
  TEST_ASSERT_FALSE_MESSAGE(a.countAnomaly, "OFF->OFF : delta nul, pas d'anomalie");

  // OFF → ON : LE démarrage. Le seul cas qui incrémente `N`.
  PumpStartAccounting b = eval(false, true, 0u, kSeuil);
  TEST_ASSERT_TRUE_MESSAGE(b.countStart, "OFF->ON est un demarrage");
  TEST_ASSERT_FALSE_MESSAGE(b.countAnomaly, "OFF->ON : aucun delta credite ce tour");

  // ON → ON : la pompe TOURNE déjà. Ne compte PAS — sinon `N` compterait des
  // tours de boucle (~1 par seconde) au lieu de démarrages (~quelques par jour).
  PumpStartAccounting c = eval(true, true, 1000u, kSeuil);
  TEST_ASSERT_FALSE_MESSAGE(c.countStart, "ON->ON ne compte pas : ce serait un tour de boucle");
  TEST_ASSERT_FALSE_MESSAGE(c.countAnomaly, "ON->ON avec delta sain");

  // ON → OFF : arrêt. Le démarrage a déjà été compté à l'allumage.
  PumpStartAccounting d = eval(true, false, 1000u, kSeuil);
  TEST_ASSERT_FALSE_MESSAGE(d.countStart, "ON->OFF n'est pas un demarrage");
  TEST_ASSERT_FALSE_MESSAGE(d.countAnomaly, "ON->OFF avec delta sain");
}

// Une session `scheduled` (marche CONTINUE, kick en tête) tourne des milliers
// de tours de boucle. Elle doit produire EXACTEMENT 1 démarrage. Ce test est
// le contre-poison direct de la mutation `countStart = onNow`.
static void test_F110_session_continue_1000_tours_un_seul_demarrage(void) {
  uint32_t n = 0u;
  bool prevOn = false;
  for (int tour = 0; tour < 1000; ++tour) {
    bool onNow = true;  // la pompe reste alimentée tout du long
    PumpStartAccounting acct = eval(prevOn, onNow, (tour == 0) ? 0u : 1000u, kSeuil);
    if (acct.countStart) n = saturatingInc(n);
    prevOn = onNow;
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, n,
      "Une session continue = 1 demarrage, jamais 1 par tour de boucle");
}

// Deux sessions espacées comptent deux démarrages. Vérifie que le front se
// RÉARME après un passage par OFF.
static void test_F110_deux_sessions_espacees_comptent_deux(void) {
  const bool marche[] = {false, true, true, true, false, false, true, true, false};
  uint32_t n = 0u;
  bool prevOn = false;
  for (unsigned i = 0; i < sizeof(marche) / sizeof(marche[0]); ++i) {
    PumpStartAccounting acct = eval(prevOn, marche[i], prevOn ? 1000u : 0u, kSeuil);
    if (acct.countStart) n = saturatingInc(n);
    prevOn = marche[i];
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, n, "Deux fronts OFF->ON = 2 demarrages");
}

// Train pulsé réaliste : 24 pouls (le protocole de banc de constants.h:2088).
// `N` doit valoir exactement 24 — c'est le dénominateur qui départage les deux
// modèles de biais.
static void test_F110_scenario_24_pouls_N_vaut_24(void) {
  uint32_t n = 0u;
  bool prevOn = false;
  for (int pouls = 0; pouls < 24; ++pouls) {
    // 5 ticks pompe ON (pouls de 5 s) puis 5 ticks pompe OFF (période).
    for (int t = 0; t < 5; ++t) {
      PumpStartAccounting acct = eval(prevOn, true, prevOn ? 1000u : 0u, kSeuil);
      if (acct.countStart) n = saturatingInc(n);
      prevOn = true;
    }
    for (int t = 0; t < 5; ++t) {
      PumpStartAccounting acct = eval(prevOn, false, prevOn ? 1000u : 0u, kSeuil);
      if (acct.countStart) n = saturatingInc(n);
      prevOn = false;
    }
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(24u, n, "24 pouls => N = 24 exactement");
}

// Le front ne dépend NI du delta crédité NI du seuil : balayage croisé.
static void test_F110_front_independant_des_deltas_et_du_seuil(void) {
  const uint32_t deltas[] = {0u, 1u, 4999u, 5000u, 5001u, 3600000u, UINT32_MAX};
  const uint32_t seuils[] = {0u, 1u, kSeuil, UINT32_MAX};
  const bool bools[] = {false, true};
  char msg[128];
  for (unsigned d = 0; d < sizeof(deltas) / sizeof(deltas[0]); ++d) {
    for (unsigned s = 0; s < sizeof(seuils) / sizeof(seuils[0]); ++s) {
      for (unsigned p = 0; p < 2; ++p) {
        for (unsigned o = 0; o < 2; ++o) {
          bool attendu = (!bools[p] && bools[o]);
          PumpStartAccounting acct = eval(bools[p], bools[o], deltas[d], seuils[s]);
          snprintf(msg, sizeof(msg),
                   "prevOn=%d onNow=%d delta=%u seuil=%u : countStart attendu=%d",
                   (int)bools[p], (int)bools[o], (unsigned)deltas[d],
                   (unsigned)seuils[s], (int)attendu);
          TEST_ASSERT_EQUAL_MESSAGE(attendu, acct.countStart, msg);
        }
      }
    }
  }
}

// =============================================================================
// 2. SEUIL D'ANOMALIE — frontière `>` STRICTE
// =============================================================================

// seuil-1 / seuil / seuil+1. La frontière est STRICTE : un delta PILE au seuil
// est NORMAL. Contre-poison direct de la mutation `>` → `>=`.
static void test_F110_seuil_anomalie_frontiere_stricte(void) {
  TEST_ASSERT_FALSE_MESSAGE(eval(true, true, kSeuil - 1u, kSeuil).countAnomaly,
      "4999 ms : sous le seuil, normal");
  TEST_ASSERT_FALSE_MESSAGE(eval(true, true, kSeuil, kSeuil).countAnomaly,
      "5000 ms : PILE au seuil = NORMAL (frontiere > stricte)");
  TEST_ASSERT_TRUE_MESSAGE(eval(true, true, kSeuil + 1u, kSeuil).countAnomaly,
      "5001 ms : au-dessus du seuil, anomalie");
}

// Seuil 1 : la frontière basse de la même règle. delta=1 est PILE au seuil,
// donc normal ; delta=2 est une anomalie.
static void test_F110_seuil_un_frontiere_basse(void) {
  TEST_ASSERT_FALSE_MESSAGE(eval(true, true, 0u, 1u).countAnomaly, "delta 0 < seuil 1");
  TEST_ASSERT_FALSE_MESSAGE(eval(true, true, 1u, 1u).countAnomaly,
      "delta 1 == seuil 1 : normal");
  TEST_ASSERT_TRUE_MESSAGE(eval(true, true, 2u, 1u).countAnomaly, "delta 2 > seuil 1");
}

// Seuil 0 = indicateur DÉSACTIVÉ, y compris avec un delta énorme. Sans ce
// court-circuit, un seuil 0 rendrait TOUT tick crédité anormal (delta > 0).
static void test_F110_seuil_zero_desactive_meme_delta_enorme(void) {
  const uint32_t deltas[] = {0u, 1u, 5000u, 3600000u, UINT32_MAX - 1u, UINT32_MAX};
  char msg[96];
  for (unsigned d = 0; d < sizeof(deltas) / sizeof(deltas[0]); ++d) {
    PumpStartAccounting acct = eval(true, true, deltas[d], kOff);
    snprintf(msg, sizeof(msg), "seuil=0 delta=%u : detection DESACTIVEE",
             (unsigned)deltas[d]);
    TEST_ASSERT_FALSE_MESSAGE(acct.countAnomaly, msg);
  }
}

// Un delta crédité NUL n'est jamais une anomalie (premier tour, pompe éteinte
// au tour précédent, delta nul) — quel que soit le seuil.
static void test_F110_delta_credite_zero_jamais_anomalie(void) {
  const uint32_t seuils[] = {0u, 1u, 2u, kSeuil, UINT32_MAX};
  char msg[96];
  for (unsigned s = 0; s < sizeof(seuils) / sizeof(seuils[0]); ++s) {
    for (unsigned p = 0; p < 2; ++p) {
      for (unsigned o = 0; o < 2; ++o) {
        PumpStartAccounting acct = eval(p != 0, o != 0, 0u, seuils[s]);
        snprintf(msg, sizeof(msg), "delta=0 seuil=%u prevOn=%u onNow=%u",
                 (unsigned)seuils[s], p, o);
        TEST_ASSERT_FALSE_MESSAGE(acct.countAnomaly, msg);
      }
    }
  }
}

// Balayage : countAnomaly == (seuil != 0 && delta > seuil), sur seuils et
// deltas arbitraires. Verrouille la règle complète, pas seulement 5000.
static void test_F110_anomalie_balayage_seuils_arbitraires(void) {
  const uint32_t deltas[] = {0u, 1u, 2u, 99u, 100u, 101u, 4999u, 5000u,
                             5001u, 60000u, UINT32_MAX - 1u, UINT32_MAX};
  const uint32_t seuils[] = {0u, 1u, 2u, 100u, kSeuil, 60000u, UINT32_MAX};
  char msg[128];
  for (unsigned d = 0; d < sizeof(deltas) / sizeof(deltas[0]); ++d) {
    for (unsigned s = 0; s < sizeof(seuils) / sizeof(seuils[0]); ++s) {
      bool attendu = (seuils[s] != 0u) && (deltas[d] > seuils[s]);
      PumpStartAccounting acct = eval(true, true, deltas[d], seuils[s]);
      snprintf(msg, sizeof(msg), "delta=%u seuil=%u : countAnomaly attendu=%d",
               (unsigned)deltas[d], (unsigned)seuils[s], (int)attendu);
      TEST_ASSERT_EQUAL_MESSAGE(attendu, acct.countAnomaly, msg);
    }
  }
}

// Bornes 32 bits : aucun débordement, aucune comparaison signée cachée.
// UINT32_MAX contre UINT32_MAX est une ÉGALITÉ, donc normal.
static void test_F110_bornes_uint32_pas_de_debordement(void) {
  TEST_ASSERT_FALSE_MESSAGE(eval(true, true, UINT32_MAX, UINT32_MAX).countAnomaly,
      "MAX == MAX : egalite, donc normal");
  TEST_ASSERT_TRUE_MESSAGE(eval(true, true, UINT32_MAX, UINT32_MAX - 1u).countAnomaly,
      "MAX > MAX-1 : anomalie");
  TEST_ASSERT_FALSE_MESSAGE(eval(true, true, UINT32_MAX - 1u, UINT32_MAX).countAnomaly,
      "MAX-1 < MAX : normal");
  // Un delta au-delà du repliement de millis() (24,8 j) reste comparé sans
  // signe : 2^31 ms doit être vu comme > 5000, pas comme un négatif.
  TEST_ASSERT_TRUE_MESSAGE(eval(true, true, 2147483648u, kSeuil).countAnomaly,
      "2^31 ms > 5000 : comparaison NON signee");
}

// Verrou de constante : le seuil du projet vaut 5000 ms. S'il change, ce test
// tombe et force à relire l'hygiène de mesure du relevé de bidon.
static void test_F110_constante_seuil_verrouillee(void) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000u, kAcctDeltaAnomalyMs,
      "kAcctDeltaAnomalyMs verrouille a 5000 ms");
}

// =============================================================================
// 3. INDÉPENDANCE DES DEUX SORTIES — les axes se croisent
// =============================================================================

// 4 combinaisons de front × 3 régimes de delta = 12 cas. Chaque cas vérifie
// LES DEUX champs. Contre-poison de l'inversion countStart/countAnomaly dans
// le struct retourné, et de tout couplage entre les deux décisions.
static void test_F110_deux_sorties_independantes_axes_croises(void) {
  const uint32_t deltas[] = {0u, 1000u, 6000u};  // nul, sain, anormal
  char msg[128];
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned o = 0; o < 2; ++o) {
      for (unsigned d = 0; d < 3; ++d) {
        bool startAttendu = (p == 0 && o == 1);
        bool anomAttendue = (deltas[d] > kSeuil);
        PumpStartAccounting acct = eval(p != 0, o != 0, deltas[d], kSeuil);
        snprintf(msg, sizeof(msg),
                 "prevOn=%u onNow=%u delta=%u : start attendu=%d", p, o,
                 (unsigned)deltas[d], (int)startAttendu);
        TEST_ASSERT_EQUAL_MESSAGE(startAttendu, acct.countStart, msg);
        snprintf(msg, sizeof(msg),
                 "prevOn=%u onNow=%u delta=%u : anomalie attendue=%d", p, o,
                 (unsigned)deltas[d], (int)anomAttendue);
        TEST_ASSERT_EQUAL_MESSAGE(anomAttendue, acct.countAnomaly, msg);
      }
    }
  }
}

// Les 4 quadrants (start, anomalie) sont tous ATTEIGNABLES sur le contrat pur
// — y compris le quadrant que la coquille ne produit pas (front + tick
// anormal). Le contrat ne doit pas dépendre d'une propriété de l'appelant.
static void test_F110_quatre_quadrants_atteignables(void) {
  PumpStartAccounting q00 = eval(true, true, 1000u, kSeuil);   // ni l'un ni l'autre
  TEST_ASSERT_FALSE(q00.countStart);
  TEST_ASSERT_FALSE(q00.countAnomaly);
  PumpStartAccounting q10 = eval(false, true, 1000u, kSeuil);  // start seul
  TEST_ASSERT_TRUE(q10.countStart);
  TEST_ASSERT_FALSE(q10.countAnomaly);
  PumpStartAccounting q01 = eval(true, true, 9000u, kSeuil);   // anomalie seule
  TEST_ASSERT_FALSE(q01.countStart);
  TEST_ASSERT_TRUE(q01.countAnomaly);
  PumpStartAccounting q11 = eval(false, true, 9000u, kSeuil);  // les deux
  TEST_ASSERT_TRUE_MESSAGE(q11.countStart, "front + tick anormal : le front reste vu");
  TEST_ASSERT_TRUE_MESSAGE(q11.countAnomaly, "front + tick anormal : l'anomalie reste vue");
}

// Campagne polluée : quelques ticks anormaux au milieu de ticks sains. Les
// anomalies se comptent À PART, sans toucher au comptage des démarrages —
// c'est ce qui permet de DISQUALIFIER un relevé de bidon pollué.
static void test_F110_campagne_polluee_anomalies_comptees_a_part(void) {
  // 1 démarrage, puis 10 ticks dont 3 anormaux (loop bloquée, OTA, I²C).
  const uint32_t ticks[] = {1000u, 1000u, 12000u, 1000u, 1000u,
                            8000u, 1000u, 1000u, 30000u, 1000u};
  uint32_t n = 0u;
  uint32_t anomalies = 0u;
  bool prevOn = false;
  PumpStartAccounting demarrage = eval(prevOn, true, 0u, kSeuil);
  if (demarrage.countStart) n = saturatingInc(n);
  if (demarrage.countAnomaly) anomalies = saturatingInc(anomalies);
  prevOn = true;
  for (unsigned i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
    PumpStartAccounting acct = eval(prevOn, true, ticks[i], kSeuil);
    if (acct.countStart) n = saturatingInc(n);
    if (acct.countAnomaly) anomalies = saturatingInc(anomalies);
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, n, "un seul demarrage dans la session");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, anomalies, "3 ticks anormaux comptes a part");
}

// =============================================================================
// 4. saturatingInc — le compteur ne reboucle JAMAIS
// =============================================================================

static void test_F110_inc_zero_vers_un(void) {
  TEST_ASSERT_EQUAL_UINT32(1u, saturatingInc(0u));
}

static void test_F110_inc_valeur_courante_plus_un(void) {
  const uint32_t vals[] = {1u, 2u, 41u, 255u, 256u, 1000u, 65535u, 65536u,
                           1000000u, 2147483647u, 2147483648u, 4000000000u};
  char msg[96];
  for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
    snprintf(msg, sizeof(msg), "saturatingInc(%u) doit valoir %u",
             (unsigned)vals[i], (unsigned)(vals[i] + 1u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(vals[i] + 1u, saturatingInc(vals[i]), msg);
  }
}

static void test_F110_inc_avant_dernier_atteint_le_plafond(void) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(UINT32_MAX, saturatingInc(UINT32_MAX - 1u),
      "MAX-1 -> MAX");
}

// LE cas qui compte : au plafond, la valeur RESTE au plafond. Un compteur qui
// reboucle à 0 produirait un rapport V/N silencieusement faux — pire qu'un
// compteur figé, qui est un aveu LISIBLE.
static void test_F110_inc_au_plafond_reste_au_plafond_jamais_zero(void) {
  uint32_t v = saturatingInc(UINT32_MAX);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(UINT32_MAX, v, "MAX -> MAX, jamais 0");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, v, "le compteur ne reboucle JAMAIS a 0");
  // Et il y reste, même sollicité en boucle (plusieurs saisons de dosage).
  for (int i = 0; i < 100; ++i) {
    v = saturatingInc(v);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(UINT32_MAX, v, "reste fige au plafond");
  }
}

// Monotonie : saturatingInc n'est JAMAIS décroissant. Propriété qui tient sur
// tout le domaine, y compris au plafond (où elle devient l'égalité).
static void test_F110_inc_monotone_jamais_decroissant(void) {
  const uint32_t vals[] = {0u, 1u, 100u, 65535u, 2147483647u, 2147483648u,
                           4294967294u, UINT32_MAX};
  char msg[96];
  for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
    uint32_t r = saturatingInc(vals[i]);
    snprintf(msg, sizeof(msg), "saturatingInc(%u)=%u doit etre >= %u",
             (unsigned)vals[i], (unsigned)r, (unsigned)vals[i]);
    TEST_ASSERT_TRUE_MESSAGE(r >= vals[i], msg);
  }
}

// Cumul depuis 0 : N incréments donnent exactement N. C'est l'usage réel du
// compteur entre deux changements de bidon.
static void test_F110_inc_cumul_exact_depuis_zero(void) {
  uint32_t n = 0u;
  for (int i = 0; i < 5000; ++i) n = saturatingInc(n);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000u, n, "5000 increments = 5000, sans derive");
}

// =============================================================================
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  // 1. Front strict OFF→ON.
  RUN_TEST(test_F110_front_table_de_verite_4_combinaisons);
  RUN_TEST(test_F110_session_continue_1000_tours_un_seul_demarrage);
  RUN_TEST(test_F110_deux_sessions_espacees_comptent_deux);
  RUN_TEST(test_F110_scenario_24_pouls_N_vaut_24);
  RUN_TEST(test_F110_front_independant_des_deltas_et_du_seuil);
  // 2. Seuil d'anomalie, frontière > stricte.
  RUN_TEST(test_F110_seuil_anomalie_frontiere_stricte);
  RUN_TEST(test_F110_seuil_un_frontiere_basse);
  RUN_TEST(test_F110_seuil_zero_desactive_meme_delta_enorme);
  RUN_TEST(test_F110_delta_credite_zero_jamais_anomalie);
  RUN_TEST(test_F110_anomalie_balayage_seuils_arbitraires);
  RUN_TEST(test_F110_bornes_uint32_pas_de_debordement);
  RUN_TEST(test_F110_constante_seuil_verrouillee);
  // 3. Indépendance des deux sorties.
  RUN_TEST(test_F110_deux_sorties_independantes_axes_croises);
  RUN_TEST(test_F110_quatre_quadrants_atteignables);
  RUN_TEST(test_F110_campagne_polluee_anomalies_comptees_a_part);
  // 4. saturatingInc : jamais de rebouclage.
  RUN_TEST(test_F110_inc_zero_vers_un);
  RUN_TEST(test_F110_inc_valeur_courante_plus_un);
  RUN_TEST(test_F110_inc_avant_dernier_atteint_le_plafond);
  RUN_TEST(test_F110_inc_au_plafond_reste_au_plafond_jamais_zero);
  RUN_TEST(test_F110_inc_monotone_jamais_decroissant);
  RUN_TEST(test_F110_inc_cumul_exact_depuis_zero);
  return UNITY_END();
}
