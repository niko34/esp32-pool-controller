// =============================================================================
// Tests unitaires natifs — Gardes de sécurité pH (ph_guard_logic, v2.25.0)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
// On teste le COMPORTEMENT observable de la logique PURE de src/ph_guard_logic.h
// (ADR-0017, comme dosing_logic / ph_offset / measure_validity_logic) :
//
//   G1 — phGuardBlocksBelow(v, seuil, was)   → refus vers le BAS + hystérésis
//   G2 — phGuardBlocksAbove(v, seuil, was)   → refus vers le HAUT + hystérésis
//   G3 — CÔTÉ de l'hystérésis (le défaut le plus probable de cette refonte)
//   G4 — politique NaN : aucun refus fantôme, aucun incrément de compteur
//   G5 — roundPhOffsetToStep(offset)         → alignement sur le pas 0,1
//   G6 — VALEURS des seuils dérivés (ici, la valeur EST le sujet du test)
//   G7 — isPhProbeHealthy(pAcid, pBase, zero) → diagnostic EZO sonde saine
//   G8 — phModeChangeNeedsAck(...)            → gate fail-closed du mode pH
//   G9 — scénarios de la refonte (table offset × brut, pH+, inhibition ORP)
//
// REFONTE v2.25.0 « seuils fixes dérivés » : le plancher CONTINU asservi à
// l'offset (phRawFloorFor) a été supprimé, ainsi que isPhTargetUnreachable(). Les
// seuils sont désormais FIXES et dérivés de la plage de consigne ; ils portent sur
// le pH CORRIGÉ, sauf deux butées de dernier recours sur le pH BRUT. Les gardes
// deviennent AGNOSTIQUES du référentiel : deux fonctions symétriques, l'appelant
// choisit la grandeur ET le seuil. Ce fichier verrouille donc, avant tout :
//   (a) le CÔTÉ de l'hystérésis — un seuil BAS relâche vers le HAUT, un seuil
//       HAUT relâche vers le BAS. L'appliquer du mauvais côté RÉTRÉCIRAIT la zone
//       de blocage au lieu de l'élargir : la garde relâcherait AVANT le seuil, et
//       une mesure oscillante produirait de nouveau la rafale de refus qui a
//       fermé la régulation le 25/08/2026 ;
//   (b) l'ABSENCE de refus fantôme sur NaN, dans les deux directions et les deux
//       états — un refus fabriqué sur une donnée qui n'existe pas alimenterait le
//       compteur des 3 refus / 24 h jusqu'à la bascule fail-closed ;
//   (c) les VALEURS des seuils dérivés et fixes (6,70 / 7,60 / 5,50 / 8,50) :
//       elles ne sont écrites nulle part en littéral dans constants.h (sauf
//       les deux filets bruts, FIXES depuis feature-168/ADR-0098), un
//       glissement de marge les déplacerait SILENCIEUSEMENT sans casser un
//       seul static_assert.
//
// RÉVISION DU PLAFOND (kPhHighMargin 0,20 → 0,10, arbitrage pool-chemistry) : le
// refus d'ALCALINISER passe de 7,70 à 7,60 et devient donc ÉGAL au seuil
// d'inhibition ORP. À 7,70 l'appareil pouvait injecter du pH+ dans une eau où il
// venait de suspendre le chlore en le jugeant inactif — il conduisait lui-même
// l'eau dans l'impasse sanitaire que l'inhibition signale. Deux conséquences
// verrouillées ici :
//   - le plafond n'est plus STRICTEMENT sous l'inhibition mais ÉGAL : l'ordre de
//     la fenêtre chimique se teste désormais au sens LARGE sur ce couple.
//   ⚠️ SUPPRIMÉ (feature-168) : ce paragraphe décrivait une égalité exacte
//   entre la butée brute haute (alors 8,10, DÉRIVÉE de l'offset) et le plafond
//   corrigé au pire cas d'offset (−0,50). Cette égalité n'a plus de sens — le
//   filet brut haut est désormais FIXE (8,50, ADR-0098) et INDÉPENDANT de
//   l'offset (cf. test_filet_brut_independant_de_l_offset_sur_tout_le_domaine).
//
// NOTE FLOTTANTS (le projet en a déjà souffert) : les frontières « pile » sont
// exprimées par l'EXPRESSION (`seuil + kPhGuardReleaseHysteresis`) et jamais par
// un littéral décimal. Contre-exemple RÉEL de ce fichier : la relâche de
// l'inhibition ORP vaut 7,5999999046 − 0,05 = 7,5499997139, alors que le littéral
// `7.55f` vaut 7,5500001907 — soit un poil AU-DESSUS du seuil de relâche. Écrire
// `TEST_ASSERT_FALSE(phGuardBlocksAbove(7.55f, ...))` échouerait alors que le code
// est correct. Écart réel : 3e-7 pH, très en dessous du bruit du filtre (~0,05).
// Partout où une ÉGALITÉ flottante est exploitée, sa pré-condition est vérifiée
// AVANT par une assertion dédiée.
// =============================================================================

#include <unity.h>
#include <math.h>   // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdio.h>  // snprintf (messages des balayages et tables de vérité)

#include "ph_guard_logic.h"
#include "ph_offset.h"  // applyPhOffset : chaîne brut → corrigé de production (G9)

void setUp(void) {}
void tearDown(void) {}

// Tolérance des comparaisons de MARGES (différences de flottants décimaux non
// représentables). Très en dessous du pas de réglage utilisateur (0,10 pH) et du
// bruit résiduel du filtre (~0,05 pH) : aucune dérive réelle ne s'y cache.
static const float kEps = 1e-4f;

// Vérifie une pré-condition d'ORDRE strict entre deux flottants avant d'en
// exploiter la conséquence. Sans cela, un test « frontière » peut passer pour de
// mauvaises raisons (les deux valeurs étant en fait égales en binary32).
static void assertOrdreStrict(float a, float b, const char* quoi) {
  char msg[160];
  snprintf(msg, sizeof(msg), "pre-condition flottante non tenue (%s) : %.9f < %.9f",
           quoi, (double)a, (double)b);
  TEST_ASSERT_TRUE_MESSAGE(a < b, msg);
}

// Variante au sens LARGE, pour les couples de seuils VOLONTAIREMENT alignés
// (plafond d'alcalinisation / inhibition ORP depuis kPhHighMargin = 0,10). Un
// ordre strict y échouerait sans qu'aucune règle de sécurité ne soit violée ;
// c'est l'inégalité large qui porte l'invariant, et elle interdit bien le seul
// sens dangereux (plafond AU-DESSUS de l'inhibition).
static void assertOrdreLarge(float a, float b, const char* quoi) {
  char msg[160];
  snprintf(msg, sizeof(msg), "invariant d'ordre non tenu (%s) : %.9f <= %.9f",
           quoi, (double)a, (double)b);
  TEST_ASSERT_TRUE_MESSAGE(a <= b, msg);
}

// =============================================================================
// G1 — phGuardBlocksBelow : franchissement vers le BAS et hystérésis
// =============================================================================

// Eau franchement au-dessus du seuil, garde au repos → aucune raison de refuser.
void test_bas_aucun_blocage_au_dessus_du_seuil(void) {
  TEST_ASSERT_FALSE(phGuardBlocksBelow(7.20f, kPhFloorCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(6.80f, kPhFloorCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(14.0f, kPhFloorCorrected, false));
}

// Sous le seuil → blocage immédiat, sans attendre : c'est la garde de dernier
// recours contre l'injection d'acide dans une eau déjà acide.
void test_bas_bloque_des_le_franchissement(void) {
  TEST_ASSERT_TRUE(phGuardBlocksBelow(6.69f, kPhFloorCorrected, false));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(6.00f, kPhFloorCorrected, false));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(0.0f, kPhFloorCorrected, false));
}

// Entrée STRICTE : pile AU seuil, garde au repos, on ne bloque pas (`v < seuil`).
// La frontière est exprimée par la constante elle-même, pas par un littéral.
void test_bas_entree_stricte_pile_au_seuil_ne_bloque_pas(void) {
  TEST_ASSERT_FALSE(phGuardBlocksBelow(kPhFloorCorrected, kPhFloorCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(kPhFloorRawHard, kPhFloorRawHard, false));
}

// *** CŒUR DE L'HYSTÉRÉSIS *** : une fois ARMÉE, remonter juste au-dessus du
// seuil ne suffit PAS à relâcher. Sans cela, une mesure qui oscille sur le seuil
// produit une rafale bloque/relâche, chacune comptée comme un refus → bascule
// fail-closed de la régulation sur un incident unique.
void test_bas_reste_bloquant_dans_la_bande_de_relache(void) {
  const float s = kPhFloorCorrected;
  const float relache = s + kPhGuardReleaseHysteresis;
  assertOrdreStrict(s, relache, "seuil bas < seuil de relache");
  TEST_ASSERT_TRUE(phGuardBlocksBelow(s, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(s + 0.01f, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(s + 0.02f, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(s + 0.04f, s, true));
}

// Relâche à `seuil + hystérésis` (frontière INCLUSIVE), et au-delà.
void test_bas_relache_a_la_frontiere_exacte_et_au_dela(void) {
  const float s = kPhFloorCorrected;
  const float relache = s + kPhGuardReleaseHysteresis;
  TEST_ASSERT_FALSE(phGuardBlocksBelow(relache, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(relache + 0.10f, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(7.20f, s, true));
}

// ASYMÉTRIE — même mesure, verdict OPPOSÉ selon l'état précédent : c'est la
// définition même d'une hystérésis (et ce qu'un `wasBlocking` ignoré casserait).
void test_bas_meme_mesure_verdict_oppose_selon_etat_precedent(void) {
  const float s = kPhFloorCorrected;
  const float dansLaBande = s + 0.02f;
  assertOrdreStrict(s, dansLaBande, "mesure au-dessus du seuil");
  assertOrdreStrict(dansLaBande, s + kPhGuardReleaseHysteresis, "mesure sous la relache");
  TEST_ASSERT_TRUE(phGuardBlocksBelow(dansLaBande, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(dansLaBande, s, false));
}

// Cycle réaliste : descente sous le seuil, séjour dans la bande de relâche,
// remontée franche. La garde ne doit relâcher qu'à la toute fin (COLLAGE puis
// DÉCOLLAGE), et l'état est bien porté de tick en tick.
void test_bas_cycle_collage_sejour_decollage(void) {
  const float s = kPhFloorCorrected;  // 6,70
  bool blocking = false;
  const float serie[]   = {7.00f, 6.80f, 6.65f, 6.70f, 6.72f, 6.74f, 6.80f, 7.10f};
  const bool  attendu[] = {false, false, true,  true,  true,  true,  false, false};
  for (unsigned i = 0; i < sizeof(serie) / sizeof(serie[0]); ++i) {
    blocking = phGuardBlocksBelow(serie[i], s, blocking);
    char msg[96];
    snprintf(msg, sizeof(msg), "etape %u du cycle (pH=%.2f)", i, (double)serie[i]);
    TEST_ASSERT_EQUAL_MESSAGE(attendu[i], blocking, msg);
  }
}

// IDEMPOTENCE : la fonction est pure, réévaluer le MÊME tick n'a aucun effet de
// bord, et l'itération de l'état converge dès le premier pas (état stable). Un
// appel répété ne doit donc jamais produire une seconde transition — donc jamais
// un second refus comptabilisé pour un unique franchissement.
void test_bas_appels_repetes_sont_idempotents(void) {
  const float s = kPhFloorCorrected;
  for (int i = -30; i <= 60; ++i) {
    const float v = s + (float)i * 0.01f;
    for (int etatInitial = 0; etatInitial <= 1; ++etatInitial) {
      const bool e1 = phGuardBlocksBelow(v, s, etatInitial != 0);
      const bool e2 = phGuardBlocksBelow(v, s, etatInitial != 0);
      const bool e3 = phGuardBlocksBelow(v, s, e1);  // itération de l'état
      char msg[128];
      snprintf(msg, sizeof(msg), "pH=%.3f etat=%d", (double)v, etatInitial);
      TEST_ASSERT_EQUAL_MESSAGE(e1, e2, msg);
      TEST_ASSERT_EQUAL_MESSAGE(e1, e3, msg);
    }
  }
}

// La mécanique ne dépend pas du seuil : elle est IDENTIQUE sur le seuil corrigé
// (6,70) et sur le filet brut (5,50), qui sont les deux usages de production.
void test_bas_meme_mecanique_sur_le_seuil_corrige_et_la_butee_brute(void) {
  const float seuils[] = {kPhFloorCorrected, kPhFloorRawHard, kPhLowWarn};
  for (unsigned i = 0; i < sizeof(seuils) / sizeof(seuils[0]); ++i) {
    const float s = seuils[i];
    char msg[96];
    snprintf(msg, sizeof(msg), "seuil=%.2f", (double)s);
    TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(s - 0.01f, s, false), msg);
    TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksBelow(s, s, false), msg);
    TEST_ASSERT_TRUE_MESSAGE(
        phGuardBlocksBelow(s + kPhGuardReleaseHysteresis * 0.5f, s, true), msg);
    TEST_ASSERT_FALSE_MESSAGE(
        phGuardBlocksBelow(s + kPhGuardReleaseHysteresis, s, true), msg);
  }
}

// =============================================================================
// G2 — phGuardBlocksAbove : franchissement vers le HAUT et hystérésis (miroir)
// =============================================================================

void test_haut_aucun_blocage_sous_le_seuil(void) {
  TEST_ASSERT_FALSE(phGuardBlocksAbove(7.20f, kPhCeilingCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(7.50f, kPhCeilingCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(0.0f, kPhCeilingCorrected, false));
}

void test_haut_bloque_des_le_franchissement(void) {
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.61f, kPhCeilingCorrected, false));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(8.50f, kPhCeilingCorrected, false));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(14.0f, kPhCeilingCorrected, false));
}

// Entrée STRICTE : pile AU seuil, garde au repos, on ne bloque pas (`v > seuil`).
void test_haut_entree_stricte_pile_au_seuil_ne_bloque_pas(void) {
  TEST_ASSERT_FALSE(phGuardBlocksAbove(kPhCeilingCorrected, kPhCeilingCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(kPhOrpInhibit, kPhOrpInhibit, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(kPhCeilingRawHard, kPhCeilingRawHard, false));
}

// La bande de relâche est SOUS le seuil : redescendre juste dessous ne relâche pas.
void test_haut_reste_bloquant_dans_la_bande_de_relache(void) {
  const float s = kPhCeilingCorrected;
  const float relache = s - kPhGuardReleaseHysteresis;
  assertOrdreStrict(relache, s, "seuil de relache < seuil haut");
  TEST_ASSERT_TRUE(phGuardBlocksAbove(s, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(s - 0.01f, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(s - 0.04f, s, true));
}

void test_haut_relache_a_la_frontiere_exacte_et_en_deca(void) {
  const float s = kPhCeilingCorrected;
  const float relache = s - kPhGuardReleaseHysteresis;
  TEST_ASSERT_FALSE(phGuardBlocksAbove(relache, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(relache - 0.10f, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(7.20f, s, true));
}

void test_haut_meme_mesure_verdict_oppose_selon_etat_precedent(void) {
  const float s = kPhCeilingCorrected;
  const float dansLaBande = s - 0.02f;
  assertOrdreStrict(dansLaBande, s, "mesure sous le seuil");
  assertOrdreStrict(s - kPhGuardReleaseHysteresis, dansLaBande, "mesure au-dessus de la relache");
  TEST_ASSERT_TRUE(phGuardBlocksAbove(dansLaBande, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(dansLaBande, s, false));
}

void test_haut_cycle_collage_sejour_decollage(void) {
  const float s = kPhCeilingCorrected;  // 7,60
  bool blocking = false;
  // Série recentrée sur 7,60. L'étape 7 (7,50) est SOUS la relâche 7,55 : c'est
  // elle qui décolle la garde. L'étape 4 vaut le seuil PILE (7.60f est bit à bit
  // égal à kPhCeilingCorrected, cf. la pré-condition du test d'égalité) et doit
  // rester bloquante puisque la garde est armée.
  const float serie[]   = {7.30f, 7.50f, 7.65f, 7.60f, 7.58f, 7.56f, 7.50f, 7.20f};
  const bool  attendu[] = {false, false, true,  true,  true,  true,  false, false};
  for (unsigned i = 0; i < sizeof(serie) / sizeof(serie[0]); ++i) {
    blocking = phGuardBlocksAbove(serie[i], s, blocking);
    char msg[96];
    snprintf(msg, sizeof(msg), "etape %u du cycle (pH=%.2f)", i, (double)serie[i]);
    TEST_ASSERT_EQUAL_MESSAGE(attendu[i], blocking, msg);
  }
}

void test_haut_appels_repetes_sont_idempotents(void) {
  const float s = kPhCeilingCorrected;
  for (int i = -60; i <= 30; ++i) {
    const float v = s + (float)i * 0.01f;
    for (int etatInitial = 0; etatInitial <= 1; ++etatInitial) {
      const bool e1 = phGuardBlocksAbove(v, s, etatInitial != 0);
      const bool e2 = phGuardBlocksAbove(v, s, etatInitial != 0);
      const bool e3 = phGuardBlocksAbove(v, s, e1);
      char msg[128];
      snprintf(msg, sizeof(msg), "pH=%.3f etat=%d", (double)v, etatInitial);
      TEST_ASSERT_EQUAL_MESSAGE(e1, e2, msg);
      TEST_ASSERT_EQUAL_MESSAGE(e1, e3, msg);
    }
  }
}

// Trois USAGES hauts (plafond d'alcalinisation, inhibition ORP, butée brute), même
// mécanique. Depuis kPhHighMargin = 0,10 les deux premiers portent la même VALEUR
// (7,60) : ce sont bien deux gardes distinctes, avec leur propre drapeau
// d'hystérésis côté pump_controller, et l'alignement des valeurs est l'invariant
// testé en G6 — pas une redondance à supprimer ici.
void test_haut_meme_mecanique_sur_les_trois_seuils_hauts(void) {
  const float seuils[] = {kPhCeilingCorrected, kPhOrpInhibit, kPhCeilingRawHard};
  for (unsigned i = 0; i < sizeof(seuils) / sizeof(seuils[0]); ++i) {
    const float s = seuils[i];
    char msg[96];
    snprintf(msg, sizeof(msg), "seuil=%.2f", (double)s);
    TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(s + 0.01f, s, false), msg);
    TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksAbove(s, s, false), msg);
    TEST_ASSERT_TRUE_MESSAGE(
        phGuardBlocksAbove(s - kPhGuardReleaseHysteresis * 0.5f, s, true), msg);
    TEST_ASSERT_FALSE_MESSAGE(
        phGuardBlocksAbove(s - kPhGuardReleaseHysteresis, s, true), msg);
  }
}

// =============================================================================
// G3 — CÔTÉ de l'hystérésis : le défaut le plus probable de cette refonte
// =============================================================================
// Deux fonctions symétriques ont été écrites à partir d'une seule : une
// hystérésis appliquée du MAUVAIS côté (soustraite dans la garde basse, ajoutée
// dans la garde haute) compile, passe les tests de franchissement simple, et
// RÉTRÉCIT silencieusement la zone de blocage — la garde relâcherait AVANT son
// propre seuil et la rafale de refus reviendrait. Les tests ci-dessous ne
// peuvent pas passer avec une hystérésis inversée.

// Garde BASSE : la bande de relâche est AU-DESSUS du seuil. Une hystérésis
// soustraite ferait relâcher à `seuil − 0,05` (6,65) alors que l'eau est encore
// SOUS le seuil — le pire cas possible : relâcher plus acide que le seuil.
void test_cote_bas_la_bande_de_relache_est_au_dessus_du_seuil(void) {
  const float s = kPhFloorCorrected;
  const float sousLeSeuil = s - kPhGuardReleaseHysteresis;  // 6,65
  assertOrdreStrict(sousLeSeuil, s, "6,65 doit rester sous le seuil bas");
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(sousLeSeuil, s, true),
                           "hysteresis appliquee du MAUVAIS cote : la garde basse "
                           "relacherait SOUS son propre seuil");
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(sousLeSeuil - 0.10f, s, true),
                           "une eau franchement acide ne relache jamais");
}

// Garde HAUTE : miroir exact — la bande de relâche est SOUS le seuil.
void test_cote_haut_la_bande_de_relache_est_sous_le_seuil(void) {
  const float s = kPhCeilingCorrected;
  const float auDessusDuSeuil = s + kPhGuardReleaseHysteresis;  // 7,65
  assertOrdreStrict(s, auDessusDuSeuil, "7,65 doit rester au-dessus du seuil haut");
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(auDessusDuSeuil, s, true),
                           "hysteresis appliquee du MAUVAIS cote : la garde haute "
                           "relacherait AU-DESSUS de son propre seuil");
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(auDessusDuSeuil + 0.10f, s, true),
                           "une eau franchement basique ne relache jamais");
}

// PROPRIÉTÉ GÉNÉRALE, par balayage : l'hystérésis ne peut qu'ÉLARGIR la zone de
// blocage. Formellement, pour toute mesure v : bloque(v, repos) ⇒ bloque(v, armé).
// Une hystérésis inversée donnerait l'inclusion CONTRAIRE et casserait ici, quel
// que soit le seuil testé. C'est le filet qui rend impossible le bug de côté.
void test_cote_l_hysteresis_elargit_toujours_la_zone_de_blocage(void) {
  const float seuils[] = {kPhFloorCorrected, kPhFloorRawHard, kPhCeilingCorrected,
                          kPhOrpInhibit, kPhCeilingRawHard, kPhLowWarn};
  for (unsigned k = 0; k < sizeof(seuils) / sizeof(seuils[0]); ++k) {
    const float s = seuils[k];
    for (int i = -100; i <= 100; ++i) {
      const float v = s + (float)i * 0.01f;
      char msg[144];
      snprintf(msg, sizeof(msg), "seuil=%.2f pH=%.3f", (double)s, (double)v);
      if (phGuardBlocksBelow(v, s, false)) {
        TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(v, s, true), msg);
      }
      if (phGuardBlocksAbove(v, s, false)) {
        TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(v, s, true), msg);
      }
    }
  }
}

// AU REPOS, les deux gardes sont ANTAGONISTES sur un même seuil : aucune mesure
// ne peut être refusée par les deux à la fois. Si les deux bloquaient ensemble,
// c'est que le sens d'une des comparaisons aurait été perdu.
void test_cote_au_repos_les_deux_gardes_ne_bloquent_jamais_ensemble(void) {
  const float s = kPhFloorCorrected;
  for (int i = -100; i <= 100; ++i) {
    const float v = s + (float)i * 0.01f;
    char msg[128];
    snprintf(msg, sizeof(msg), "pH=%.3f", (double)v);
    TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksBelow(v, s, false) &&
                                  phGuardBlocksAbove(v, s, false), msg);
  }
}

// ARMÉES, les deux bandes de relâche se CHEVAUCHENT (±0,05 autour du seuil) :
// une même mesure y est refusée dans les deux directions. Conséquence directe et
// VOULUE de l'hystérésis — et la raison pour laquelle pump_controller porte
// QUATRE drapeaux d'état distincts (deux référentiels × deux directions) : un
// drapeau partagé entre une garde basse et une garde haute ferait relâcher l'une
// sur le franchissement de l'autre.
void test_cote_armees_les_bandes_de_relache_se_chevauchent(void) {
  const float s = kPhFloorCorrected;
  TEST_ASSERT_TRUE(phGuardBlocksBelow(s, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(s, s, true));
  // Hors de la bande commune, chaque garde retrouve son sens propre.
  TEST_ASSERT_TRUE(phGuardBlocksBelow(s - 0.10f, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(s - 0.10f, s, true));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(s + 0.10f, s, true));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(s + 0.10f, s, true));
}

// =============================================================================
// G4 — Politique NaN : aucun refus fantôme, aucun incrément de compteur
// =============================================================================
// Ce que le HEADER de ph_guard_logic.h promet, mot pour mot : « une entrée NaN ne
// produit JAMAIS de refus [...] fabriquer un refus ici ajouterait un refus
// FANTÔME au compteur des 3 refus / 24 h, jusqu'à fermer la régulation sur une
// donnée qui n'existe pas ». Les tests ci-dessous vérifient le CODE contre cette
// promesse, pas l'inverse.

void test_nan_mesure_ne_produit_aucun_refus_dans_les_deux_directions(void) {
  TEST_ASSERT_FALSE(phGuardBlocksBelow(NAN, kPhFloorCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(NAN, kPhFloorCorrected, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(NAN, kPhCeilingCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(NAN, kPhCeilingCorrected, true));
}

// Seuil NaN (ne peut pas arriver depuis constants.h, mais la fonction est
// publique) → même politique : aucun refus fabriqué sur une comparaison
// indéterminée. Sans le test explicite, `NAN < x` valant false, la garde basse
// serait « accidentellement » correcte et la garde haute aussi — mais un futur
// `!(v >= seuil)` inverserait le résultat sans que rien ne le signale.
void test_nan_seuil_ne_produit_aucun_refus(void) {
  TEST_ASSERT_FALSE(phGuardBlocksBelow(6.00f, NAN, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(6.00f, NAN, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(9.00f, NAN, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(9.00f, NAN, true));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(NAN, NAN, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(NAN, NAN, true));
}

// Infinis (offset corrompu en NVS propagé par applyPhOffset) : ils sont ORDONNÉS,
// donc traités comme des mesures — et le verdict est celui, fail-closed, qu'on
// attend d'une valeur aberrante côté dangereux.
void test_infinis_sont_traites_comme_des_mesures_ordonnees(void) {
  TEST_ASSERT_TRUE(phGuardBlocksBelow(-INFINITY, kPhFloorCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(INFINITY, kPhFloorCorrected, true));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(INFINITY, kPhCeilingCorrected, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(-INFINITY, kPhCeilingCorrected, true));
}

// Compte les TRANSITIONS « ne bloquait pas » → « bloque » d'une série, c'est-à-dire
// exactement ce que pump_controller comptabilise dans le compteur 3 refus / 24 h
// (`newRefusal = blocked && !wasBlocking`).
static int compteRefus(const float* serie, unsigned n, float seuil) {
  bool etat = false;
  int refus = 0;
  for (unsigned i = 0; i < n; ++i) {
    const bool bloque = phGuardBlocksBelow(serie[i], seuil, etat);
    if (bloque && !etat) ++refus;
    etat = bloque;
  }
  return refus;
}

// *** LE CAS QUI FERME LA RÉGULATION *** : une sonde muette par intermittence
// (NaN) au milieu d'une eau parfaitement saine ne doit produire AUCUN refus. Trois
// refus fantômes suffiraient à basculer la régulation en manuel.
void test_nan_une_serie_saine_trouee_de_nan_ne_compte_aucun_refus(void) {
  const float serie[] = {7.10f, NAN, 7.05f, NAN, NAN, 7.00f, NAN, 6.95f, NAN, 7.20f};
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, compteRefus(serie, 10, kPhFloorCorrected),
                                "un trou de mesure ne doit JAMAIS compter un refus");
}

// Un franchissement UNIQUE, même long et même bruité dans la bande de relâche, ne
// compte qu'UN refus : c'est la raison d'être de l'hystérésis.
void test_un_episode_de_blocage_ne_compte_qu_un_seul_refus(void) {
  const float serie[] = {7.00f, 6.65f, 6.68f, 6.71f, 6.73f, 6.69f, 6.72f, 6.74f, 6.66f, 6.60f};
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, compteRefus(serie, 10, kPhFloorCorrected),
                                "l'hysteresis doit absorber l'oscillation sur le seuil");
}

// COMPORTEMENT VERROUILLÉ (et connu) : un trou de mesure PENDANT un épisode de
// blocage réarme la garde — la reprise du blocage compte donc un NOUVEAU refus.
// C'est le miroir exact de pump_controller.cpp, qui remet les quatre drapeaux à
// false quand le pH brut est NaN. Ce n'est PAS un refus fantôme (l'eau est
// réellement sous le seuil aux deux extrémités du trou) mais cela accélère
// l'escalade en cas de sonde intermittente : à documenter avant de toucher aux
// drapeaux, sous peine de changer le délai de bascule fail-closed sans le voir.
void test_un_trou_de_nan_pendant_un_blocage_rearme_la_garde(void) {
  const float serie[] = {7.00f, 6.60f, NAN, 6.60f};
  TEST_ASSERT_EQUAL_INT(2, compteRefus(serie, 4, kPhFloorCorrected));
}

// =============================================================================
// G5 — roundPhOffsetToStep : alignement de l'offset sur le pas de 0,1
// =============================================================================

// Une valeur déjà sur la grille est rendue TELLE QUELLE, bit à bit (l'exactitude
// est ici le sujet : un aller-retour ne doit pas faire dériver la config NVS de
// l'utilisateur d'un ulp à chaque démarrage).
void test_arrondi_les_valeurs_sur_la_grille_sont_inchangees(void) {
  for (int n = -5; n <= 5; ++n) {
    const float v = (float)n * kPhOffsetStep;
    char msg[96];
    snprintf(msg, sizeof(msg), "n=%d valeur=%.9f", n, (double)v);
    TEST_ASSERT_TRUE_MESSAGE(roundPhOffsetToStep(v) == v, msg);
  }
}

// Arrondi au PLUS PROCHE, dans les deux sens.
void test_arrondi_au_plus_proche(void) {
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f,  roundPhOffsetToStep(0.04f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.1f,  roundPhOffsetToStep(0.06f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.1f,  roundPhOffsetToStep(0.14f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.3f,  roundPhOffsetToStep(0.26f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.1f, roundPhOffsetToStep(-0.06f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.3f, roundPhOffsetToStep(-0.34f));
  // Le cas cité par la spec : 0,47 → 0,50, soit 0,03 pH, sous le bruit du filtre.
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.5f,  roundPhOffsetToStep(0.47f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.5f, roundPhOffsetToStep(-0.47f));
}

// MI-CHEMIN : convention roundf() = « au plus loin de zéro », donc symétrique.
// Verrouillé parce qu'une bascule vers « au pair le plus proche » (rint) changerait
// silencieusement l'offset migré d'un demi-pas sur les installations existantes.
void test_arrondi_valeurs_a_mi_chemin_s_ecartent_de_zero(void) {
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.1f,  roundPhOffsetToStep(0.05f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.1f, roundPhOffsetToStep(-0.05f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.2f,  roundPhOffsetToStep(0.15f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.2f, roundPhOffsetToStep(-0.15f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.3f,  roundPhOffsetToStep(0.25f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.4f,  roundPhOffsetToStep(0.35f));
  // Symétrie stricte : arrondir −x doit donner −(arrondir x), sinon la migration
  // introduirait un biais d'acidification.
  for (int i = 0; i <= 60; ++i) {
    const float x = (float)i * 0.01f;
    char msg[96];
    snprintf(msg, sizeof(msg), "x=%.3f", (double)x);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, -roundPhOffsetToStep(x),
                                     roundPhOffsetToStep(-x), msg);
  }
}

// *** MUTATION (a) DE LA SPEC *** — ⚠️ ÉCRÊTAGE RETIRÉ (feature-168) :
// roundPhOffsetToStep() n'a plus de borne vers laquelle clamper
// (kPhOffsetMin/kPhOffsetMax ont disparu avec la borne de réglage) — elle
// ARRONDIT SEULEMENT. Ce test verrouille l'ABSENCE de clamp : une valeur très
// au-delà de l'ancienne borne ±0,50 ressort ARRONDIE, jamais ramenée à ±0,50.
// Motif retiré (ancien test_arrondi_borne_aux_limites_d_offset,
// test_ph_guard.cpp:538-541 et :555) : « l'égalité filet/plafond dérivait d'une
// borne d'offset supprimée par feature-168 » — ici le clamp lui-même, qui
// n'a plus de borne à respecter. ⛔ Si un futur commit réintroduit un clamp ici
// « par prudence », CE test doit casser — c'est la mutation (a) de la spec.
void test_arrondi_ne_clampe_plus_au_dela_de_l_ancienne_borne(void) {
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.60f, roundPhOffsetToStep(0.60f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 5.0f,  roundPhOffsetToStep(5.0f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 14.0f, roundPhOffsetToStep(14.0f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.60f, roundPhOffsetToStep(-0.60f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -5.0f,  roundPhOffsetToStep(-5.0f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, -14.0f, roundPhOffsetToStep(-14.0f));
}

// Sur tout le domaine [-2 ; 2] (bien au-delà de l'ancienne borne ±0,50), le
// résultat de l'arrondi reste à une DEMI-PAS maximum de l'entrée — c'est un
// ARRONDI, jamais un clamp qui écraserait des entrées distinctes sur une même
// borne.
void test_arrondi_reste_a_un_demi_pas_de_l_entree_sur_tout_le_domaine(void) {
  for (int i = -200; i <= 200; ++i) {
    const float x = (float)i * 0.01f;
    const float r = roundPhOffsetToStep(x);
    char msg[112];
    snprintf(msg, sizeof(msg), "x=%.3f -> %.9f", (double)x, (double)r);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(r - x) <= (kPhOffsetStep / 2.0f) + kEps, msg);
  }
}

// *** ZÉRO NÉGATIF *** — `0.0 == -0.0` est VRAI en C++ : une assertion de valeur
// ne verrait rien. On teste donc le SIGNE (signbit), parce que le symptôme est
// un affichage « -0.00 » dans l'interface et dans les logs.
void test_arrondi_ne_produit_jamais_de_zero_negatif(void) {
  const float entrees[] = {-0.0f, -0.01f, -0.02f, -0.04f, -0.049f, 0.0f, 0.04f};
  for (unsigned i = 0; i < sizeof(entrees) / sizeof(entrees[0]); ++i) {
    const float r = roundPhOffsetToStep(entrees[i]);
    char msg[112];
    snprintf(msg, sizeof(msg), "entree=%.4f resultat=%.9f", (double)entrees[i], (double)r);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, r, msg);
    TEST_ASSERT_FALSE_MESSAGE(signbit(r), msg);
  }
}

// Aucune sortie n'est un zéro négatif, sur tout le domaine d'entrée.
void test_arrondi_aucun_zero_negatif_sur_tout_le_domaine(void) {
  for (int i = -200; i <= 200; ++i) {
    const float r = roundPhOffsetToStep((float)i * 0.01f);
    if (r == 0.0f) {
      char msg[96];
      snprintf(msg, sizeof(msg), "x=%.3f produit un zero negatif", (double)i * 0.01);
      TEST_ASSERT_FALSE_MESSAGE(signbit(r), msg);
    }
  }
}

// Offset NaN (config corrompue, champ jamais écrit) → 0,00 NEUTRE, et non NaN :
// même convention que applyPhOffset(), qui neutralise déjà un offset NaN. Un NaN
// propagé jusqu'au pH corrigé désarmerait toutes les gardes chimiques (G4).
void test_arrondi_offset_nan_devient_zero_neutre(void) {
  const float r = roundPhOffsetToStep(NAN);
  TEST_ASSERT_FALSE_MESSAGE(isnan(r), "un offset NaN persiste desarmerait les gardes");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, r);
  TEST_ASSERT_FALSE_MESSAGE(signbit(r), "0,00 neutre, jamais -0,00");
}

// IDEMPOTENCE : arrondir deux fois == arrondir une fois, bit à bit. C'est ce qui
// garantit qu'un aller-retour UI → API → NVS → relecture ne fait pas dériver
// l'offset, et que le log « aligné sur le pas » n'est émis qu'une seule fois.
void test_arrondi_est_idempotent_sur_tout_le_domaine(void) {
  for (int i = -200; i <= 200; ++i) {
    const float x = (float)i * 0.01f;
    const float r1 = roundPhOffsetToStep(x);
    const float r2 = roundPhOffsetToStep(r1);
    char msg[144];
    snprintf(msg, sizeof(msg), "x=%.3f r1=%.9f r2=%.9f", (double)x, (double)r1, (double)r2);
    TEST_ASSERT_TRUE_MESSAGE(r1 == r2, msg);
  }
  TEST_ASSERT_TRUE(roundPhOffsetToStep(roundPhOffsetToStep(NAN)) ==
                   roundPhOffsetToStep(NAN));
}

// Toute sortie est EXACTEMENT un multiple entier du pas — c'est ce que le
// curseur de l'interface sait afficher. L'égalité bit à bit est ici le sujet :
// `n * kPhOffsetStep` est la forme canonique produite par la fonction
// elle-même. ⚠️ feature-168 : le domaine balayé dépasse largement l'ancienne
// borne ±0,50 (plus de clamp, cf. test_arrondi_ne_clampe_plus_au_dela_de_l_ancienne_borne) —
// l'entier `n` n'est donc PLUS borné à [-5 ; 5], seule sa forme canonique compte.
void test_arrondi_toute_sortie_est_un_multiple_exact_du_pas(void) {
  for (int i = -1500; i <= 1500; ++i) {
    const float r = roundPhOffsetToStep((float)i * 0.01f);
    const int n = (int)lrintf(r / kPhOffsetStep);
    char msg[144];
    snprintf(msg, sizeof(msg), "x=%.3f -> %.9f (n=%d)", (double)i * 0.01, (double)r, n);
    TEST_ASSERT_TRUE_MESSAGE(r == (float)n * kPhOffsetStep, msg);
  }
}

// Monotonie : l'alignement ne peut pas inverser l'ordre de deux offsets. Une
// implémentation par troncature signée le ferait au voisinage de zéro.
void test_arrondi_est_monotone_croissant(void) {
  float precedent = roundPhOffsetToStep(-1.0f);
  for (int i = -99; i <= 100; ++i) {
    const float r = roundPhOffsetToStep((float)i * 0.01f);
    char msg[128];
    snprintf(msg, sizeof(msg), "x=%.3f -> %.9f (precedent %.9f)",
             (double)i * 0.01, (double)r, (double)precedent);
    TEST_ASSERT_TRUE_MESSAGE(r >= precedent, msg);
    precedent = r;
  }
}

// =============================================================================
// G6 — VALEURS des seuils dérivés : ici, la valeur EST le sujet du test
// =============================================================================
// constants.h porte déjà des static_assert de COHÉRENCE (ordre des bornes,
// non-interférence de la butée basse, hystérésis < plus petite marge). Ce qu'ils
// ne couvrent PAS, ce sont les VALEURS : glisser kPhSafetyMargin de 0,50 à 0,30
// déplace le refus d'acidifier de 6,70 à 6,90 et laisse TOUS les static_assert
// verts. Les quatre dérivés sont donc écrits ici EN DUR, à dessein : leur
// modification doit faire échouer cette suite et repasser par pool-chemistry.

void test_derives_valeurs_numeriques_exactes(void) {
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 6.70f, kPhFloorCorrected,
                                   "refus d'ACIDIFIER [CORRIGE]");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 7.60f, kPhCeilingCorrected,
                                   "refus d'ALCALINISER [CORRIGE] — 7,60 et non "
                                   "7,70 : au-dela, on alcaliniserait une eau "
                                   "dont le chlore est deja declare inactif");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 5.50f, kPhFloorRawHard,
                                   "filet bas [BRUT] — ADR-0098, feature-168 : "
                                   "FIXE, n'est plus derive de l'offset");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 8.50f, kPhCeilingRawHard,
                                   "filet haut [BRUT] — ADR-0098, feature-168 : "
                                   "FIXE, n'est plus derive de l'offset");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 7.60f, kPhOrpInhibit,
                                   "inhibition ORP [CORRIGE]");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 7.00f, kPhLowWarn,
                                   "alerte non bloquante [CORRIGE]");
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, kPhGuardReleaseHysteresis);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, kPhOffsetStep);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, kPhTargetStep);
}

// La source unique et ses marges — si l'une bouge, les dérivés ci-dessus bougent,
// et les deux tests doivent casser ENSEMBLE (sinon c'est que la dérivation a été
// contournée par un littéral).
void test_derives_source_unique_et_marges(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.20f, kPhTargetMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.50f, kPhTargetMax);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, kPhSafetyMargin);
  // 0,10 et NON 0,20 (arbitrage pool-chemistry) : c'est cette valeur qui aligne
  // le plafond d'alcalinisation sur l'inhibition ORP. La remonter à 0,20 rouvre
  // la zone 7,60-7,70 où l'appareil alcalinise une eau dont il a suspendu le
  // chlore — et rend l'invariant de la butée brute haute insatisfiable.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.10f, kPhHighMargin,
                                   "marge haute : 0,10 aligne le plafond sur "
                                   "kPhOrpInhibit");
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, kPhOrpInhibitMargin);
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhFloorCorrected, kPhTargetMin - kPhSafetyMargin);
  TEST_ASSERT_FLOAT_WITHIN(kEps, kPhCeilingCorrected, kPhTargetMax + kPhHighMargin);
  // feature-168 (ADR-0098) : les filets BRUTS ne DÉRIVENT plus de l'offset —
  // ils sont ÉGAUX aux deux absolus à justification chimique propre, sans
  // AUCUNE trace d'offset dans leur définition (condition C1 du gate).
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      kEps, kPhFloorRawHard, kPhRawFloorAbsolute,
      "le filet bas doit etre EGAL a l'absolu chimique, pas derive de l'offset");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      kEps, kPhCeilingRawHard, kPhRawCeilingAbsolute,
      "le filet haut doit etre EGAL a l'absolu chimique, pas derive de l'offset");
  // La plage de consigne doit être un nombre ENTIER de pas : sinon le curseur de
  // l'interface ne peut pas atteindre la borne haute (4 positions : 7,2 à 7,5).
  const float positions = (kPhTargetMax - kPhTargetMin) / kPhTargetStep;
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 3.0f, positions,
                                   "la plage de consigne doit tomber juste sur le pas");
}

// Ordonnancement chimique complet de la fenêtre d'exploitation, du plus acide au
// plus basique. Une inversion rendrait un seuil inatteignable (code mort) ou
// masquerait un seuil derrière un autre.
void test_derives_ordonnancement_de_la_fenetre_chimique(void) {
  assertOrdreStrict(kPhFloorRawHard, kPhFloorCorrected, "butee brute < seuil bas");
  assertOrdreStrict(kPhFloorCorrected, kPhLowWarn, "seuil bas < alerte 7,00");
  assertOrdreStrict(kPhLowWarn, kPhTargetMin, "alerte < consigne mini");
  assertOrdreStrict(kPhTargetMin, kPhTargetMax, "consigne mini < consigne maxi");
  assertOrdreStrict(kPhTargetMax, kPhOrpInhibit, "consigne maxi < inhibition ORP");
  // Sens LARGE et sens INVERSÉ de l'ancienne version : depuis kPhHighMargin =
  // 0,10 le plafond est ÉGAL à l'inhibition, et le seul ordre dangereux serait
  // l'autre (plafond AU-DESSUS). Cf. le test dédié ci-dessous.
  assertOrdreLarge(kPhCeilingCorrected, kPhOrpInhibit, "plafond <= inhibition ORP");
  assertOrdreStrict(kPhCeilingCorrected, kPhCeilingRawHard, "plafond < butee brute haute");
  // L'hystérésis doit rester PETITE devant la plus petite marge, sinon relâcher
  // une garde suffirait à en franchir une autre.
  TEST_ASSERT_TRUE(kPhGuardReleaseHysteresis < (kPhLowWarn - kPhFloorCorrected));
  TEST_ASSERT_TRUE(kPhGuardReleaseHysteresis < kPhHighMargin);
}

// *** CRITÈRE D'ACCEPTATION DE LA SPEC *** — la butée BRUTE basse ne doit JAMAIS
// mordre avant le seuil CORRIGÉ, pour tout offset du domaine autorisé. Balayage
// complet (offset × pH brut) : si la butée bloque, le seuil corrigé bloquait déjà.
// C'est ce qui rend le seuil corrigé effectivement opérant, et c'est l'argument
// qui a fait refuser une butée à 6,40.
void test_derives_la_butee_brute_basse_ne_mord_jamais_la_premiere(void) {
  int couples = 0;
  for (int o = -50; o <= 50; ++o) {
    const float offset = (float)o * 0.01f;
    for (int r = 550; r <= 750; ++r) {
      const float brut = (float)r * 0.01f;
      const float corrige = applyPhOffset(brut, offset);
      if (phGuardBlocksBelow(brut, kPhFloorRawHard, false)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "offset=%.2f brut=%.2f corrige=%.4f",
                 (double)offset, (double)brut, (double)corrige);
        TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(corrige, kPhFloorCorrected, false), msg);
      }
      ++couples;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(101 * 201, couples, "balayage incomplet");
}

// Miroir HAUT : le filet BRUT haut (8,50) ne peut pas mordre avant
// l'inhibition ORP sur le corrigé (7,60), pour tout offset du domaine — les deux
// forment la garde ORP, dont la butée est le filet contre un offset NÉGATIF faux.
void test_derives_la_butee_brute_haute_ne_mord_jamais_avant_l_inhibition_orp(void) {
  for (int o = -50; o <= 50; ++o) {
    const float offset = (float)o * 0.01f;
    for (int r = 700; r <= 900; ++r) {
      const float brut = (float)r * 0.01f;
      const float corrige = applyPhOffset(brut, offset);
      if (phGuardBlocksAbove(brut, kPhCeilingRawHard, false)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "offset=%.2f brut=%.2f corrige=%.4f",
                 (double)offset, (double)brut, (double)corrige);
        TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(corrige, kPhOrpInhibit, false), msg);
      }
    }
  }
}

// *** LE VERROU DE L'ARBITRAGE (kPhHighMargin 0,20 → 0,10) *** — le plafond
// d'alcalinisation ne peut JAMAIS dépasser le seuil d'inhibition ORP. Au-delà,
// l'appareil autoriserait le pH+ à pousser une eau dont il vient de décider que
// le chlore y est inactif : il conduirait lui-même l'eau dans l'impasse sanitaire
// que l'inhibition signale. Le static_assert de kPhOrpInhibitMargin ne protège
// que la CONSIGNE — sans le test ci-dessous, remonter kPhHighMargin à 0,20
// recompilait sans un mot (c'est exactement ce qui est arrivé).
void test_derives_le_plafond_ne_depasse_jamais_l_inhibition_orp(void) {
  assertOrdreLarge(kPhCeilingCorrected, kPhOrpInhibit,
                   "plafond d'alcalinisation <= inhibition ORP");
  // Conséquence OBSERVABLE, à état de garde ÉGAL (c'est le seul rapprochement
  // légitime : les deux gardes portent leur propre drapeau d'hystérésis dans
  // pump_controller). Dès que le chlore est inhibé, le pH+ est déjà refusé.
  for (int i = -100; i <= 100; ++i) {
    const float v = kPhOrpInhibit + (float)i * 0.01f;
    for (int w = 0; w <= 1; ++w) {
      const bool arme = (w != 0);
      char msg[176];
      snprintf(msg, sizeof(msg),
               "pH corrige=%.4f arme=%d : chlore inhibe mais pH+ autorise — "
               "l'appareil alcaliniserait une eau au chlore inactif",
               (double)v, w);
      if (phGuardBlocksAbove(v, kPhOrpInhibit, arme)) {
        TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(v, kPhCeilingCorrected, arme), msg);
      }
    }
  }
}

// ⚠️ SUPPRIMÉ (feature-168, révision C11 du gate pool-chemistry) :
// l'ancien test_derives_la_butee_brute_haute_ne_mord_jamais_avant_le_plafond_corrige
// balayait le domaine kPhOffsetMin → kPhOffsetMax en affirmant que la butée
// BRUTE haute ne mord JAMAIS avant le plafond CORRIGÉ, pour tout offset du
// domaine autorisé. Cet invariant est devenu FAUX et ne peut plus être vrai :
// l'offset n'a plus de domaine borné, donc aucun filet FIXE ne peut plus
// dominer TOUS les offsets possibles (c'est exactement l'objet de la zone
// d'impasse, C5/C5bis). Remplacé ci-dessous par l'INDÉPENDANCE : le filet BRUT
// se déclenche EXACTEMENT à 5,50 / 8,50, QUEL QUE SOIT L'OFFSET — l'offset
// n'entre simplement plus dans sa décision.
//
// ⚠️ SUPPRIMÉ AUSSI (motif identique, approuvé par le gate) :
// test_derives_egalite_de_la_butee_haute_et_du_plafond_a_offset_min —
// « l'égalité filet/plafond dérivait d'une borne d'offset supprimée par
// feature-168 » (kPhOffsetMin n'existe plus, et l'égalité qu'il vérifiait —
// kPhCeilingRawHard == kPhCeilingCorrected − kPhOffsetMin — n'a plus de sens :
// le filet est maintenant FIXE à 8,50, indépendamment de tout offset).

// *** INDÉPENDANCE DU FILET BRUT (feature-168, révision C11 du gate) ***
// Balayage offset ∈ [−14 ; +14] × pH BRUT ∈ [4 ; 10] (critère d'acceptation de
// la spec) : le filet BAS se déclenche EXACTEMENT à 5,50, le filet HAUT
// EXACTEMENT à 8,50, et ceci QUEL QUE SOIT L'OFFSET balayé — la décision ne
// dépend QUE du BRUT, jamais du CORRIGÉ. C'est ce qui remplace l'ancien
// invariant « ne mord jamais avant » (devenu faux au-delà des deux seuils
// d'impasse) : ici on ne demande plus une RELATION entre deux seuils, mais
// l'INDÉPENDANCE totale du filet vis-à-vis de l'offset.
// *** C'EST CE TEST QUI TUE LA MUTATION (b) *** : si kPhFloorRawHard ou
// kPhCeilingRawHard redevenaient dérivés d'un offset (ex. littéral recalé sur
// un ancien kPhOffsetMax réintroduit), le point de déclenchement observé
// dévierait de 5,50/8,50 et cette comparaison EXACTE casserait.
void test_filet_brut_independant_de_l_offset_sur_tout_le_domaine(void) {
  // Pré-condition : les deux valeurs attendues sont bien celles des constantes
  // à justification chimique propre (ADR-0098), pas des littéraux arbitraires
  // choisis pour faire passer ce test.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 5.50f, kPhFloorRawHard);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.50f, kPhCeilingRawHard);

  int couples = 0;
  for (int o = -1400; o <= 1400; o += 100) {  // pas 0,10 (kPhOffsetStep) sur [-14;14]
    const float offset = (float)o * 0.01f;
    for (int r = 400; r <= 1000; ++r) {       // pH BRUT [4 ; 10], pas 0,01
      const float brut = (float)r * 0.01f;
      char msg[160];
      snprintf(msg, sizeof(msg), "offset=%+.2f brut=%.2f", (double)offset, (double)brut);
      // Le filet BAS mord SSI brut < 5,50 — jamais influencé par l'offset.
      TEST_ASSERT_EQUAL_MESSAGE(brut < kPhFloorRawHard,
                                phGuardBlocksBelow(brut, kPhFloorRawHard, false), msg);
      // Le filet HAUT mord SSI brut > 8,50 — jamais influencé par l'offset.
      TEST_ASSERT_EQUAL_MESSAGE(brut > kPhCeilingRawHard,
                                phGuardBlocksAbove(brut, kPhCeilingRawHard, false), msg);
      ++couples;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(29 * 601, couples, "balayage incomplet");
}

// =============================================================================
// G6bis — ZONE D'IMPASSE DE RÉGULATION (feature-168, C5 + C5bis)
// =============================================================================
// isPhRegulationImpasseLow/High NE BLOQUENT RIEN par elles-mêmes : elles
// DISENT qu'au-delà d'un seuil d'offset, le filet BRUT (fixe) mord
// STRUCTURELLEMENT avant que la régulation puisse atteindre son seuil
// CORRIGÉ. Logique pure, testée ici aux bornes EXACTES.

// Côté BAS : kPhOffsetImpasseThreshold = kPhFloorCorrected − kPhRawFloorAbsolute
// (= 1,20). Entrée STRICTE (`offset > seuil`).
void test_impasse_basse_valeur_du_seuil_calcule(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.20f, kPhOffsetImpasseThreshold);
}

void test_impasse_basse_pile_au_seuil_n_est_pas_en_impasse(void) {
  TEST_ASSERT_FALSE(isPhRegulationImpasseLow(kPhOffsetImpasseThreshold));
}

void test_impasse_basse_un_centieme_au_dela_est_en_impasse(void) {
  TEST_ASSERT_TRUE(isPhRegulationImpasseLow(kPhOffsetImpasseThreshold + 0.01f));
}

void test_impasse_basse_un_centieme_en_deca_n_est_pas_en_impasse(void) {
  TEST_ASSERT_FALSE(isPhRegulationImpasseLow(kPhOffsetImpasseThreshold - 0.01f));
}

// Offsets usuels (zone non-impasse) et extrêmes (zone d'impasse confirmée) —
// aucun refus fantôme sur NaN.
void test_impasse_basse_offsets_usuels_et_extremes(void) {
  TEST_ASSERT_FALSE(isPhRegulationImpasseLow(0.0f));
  TEST_ASSERT_FALSE(isPhRegulationImpasseLow(0.50f));
  TEST_ASSERT_FALSE(isPhRegulationImpasseLow(-14.0f));
  TEST_ASSERT_TRUE(isPhRegulationImpasseLow(1.50f));
  TEST_ASSERT_TRUE(isPhRegulationImpasseLow(14.0f));
  TEST_ASSERT_FALSE_MESSAGE(isPhRegulationImpasseLow(NAN),
                           "NaN ne doit jamais fabriquer une impasse fantome");
}

// Côté HAUT (C5bis) : kPhOffsetImpasseThresholdHigh = kPhCeilingCorrected −
// kPhRawCeilingAbsolute (= −0,90, NÉGATIF). Entrée LARGE (`offset <= seuil`,
// miroir intentionnel de la stricte du bas — un offset NUL ne doit jamais se
// retrouver des deux côtés à la fois, et l'inclusion ici protège le cas
// offset == seuil exact, qui EST en impasse côté haut).
void test_impasse_haute_valeur_du_seuil_calcule(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, -0.90f, kPhOffsetImpasseThresholdHigh);
}

void test_impasse_haute_pile_au_seuil_est_en_impasse(void) {
  TEST_ASSERT_TRUE(isPhRegulationImpasseHigh(kPhOffsetImpasseThresholdHigh));
}

void test_impasse_haute_un_centieme_au_dela_n_est_pas_en_impasse(void) {
  // "au-delà" côté haut = PLUS GRAND (moins négatif) : hors impasse.
  TEST_ASSERT_FALSE(isPhRegulationImpasseHigh(kPhOffsetImpasseThresholdHigh + 0.01f));
}

void test_impasse_haute_un_centieme_en_deca_est_en_impasse(void) {
  // "en deçà" côté haut = PLUS NÉGATIF : toujours en impasse.
  TEST_ASSERT_TRUE(isPhRegulationImpasseHigh(kPhOffsetImpasseThresholdHigh - 0.01f));
}

void test_impasse_haute_offsets_usuels_et_extremes(void) {
  TEST_ASSERT_FALSE(isPhRegulationImpasseHigh(0.0f));
  TEST_ASSERT_FALSE(isPhRegulationImpasseHigh(-0.50f));
  TEST_ASSERT_FALSE(isPhRegulationImpasseHigh(14.0f));
  TEST_ASSERT_TRUE(isPhRegulationImpasseHigh(-1.50f));
  TEST_ASSERT_TRUE(isPhRegulationImpasseHigh(-14.0f));
  TEST_ASSERT_FALSE_MESSAGE(isPhRegulationImpasseHigh(NAN),
                           "NaN ne doit jamais fabriquer une impasse fantome");
}

// Les deux zones d'impasse ne se CHEVAUCHENT jamais (un offset ne peut pas être
// simultanément "trop grand" et "trop négatif") — l'ASYMÉTRIE des deux seuils
// (1,20 / −0,90) le garantit structurellement, vérifiée ici par construction.
void test_impasse_les_deux_zones_ne_se_chevauchent_jamais(void) {
  for (int o = -1500; o <= 1500; o += 10) {
    const float offset = (float)o * 0.01f;
    const bool low = isPhRegulationImpasseLow(offset);
    const bool high = isPhRegulationImpasseHigh(offset);
    char msg[80];
    snprintf(msg, sizeof(msg), "offset=%+.2f low=%d high=%d", (double)offset, low, high);
    TEST_ASSERT_FALSE_MESSAGE(low && high, msg);
  }
}

// =============================================================================
// G7 — isPhProbeHealthy : diagnostic électrochimique EZO (INCHANGÉ v2.25.0)
// =============================================================================

// Valeurs RÉELLES relevées sur l'installation le 2026-08-25 : pente acide
// 101,0 %, pente base 100,0 %, zéro −1,03 mV. La sonde est SAINE — le message
// d'escalade « sonde à recalibrer ou remplacer » était donc trompeur.
void test_sonde_cas_nominal_installation_reelle_est_saine(void) {
  TEST_ASSERT_TRUE(isPhProbeHealthy(101.0f, 100.0f, -1.03f));
}

void test_sonde_pentes_parfaites_et_zero_nul_sont_sains(void) {
  TEST_ASSERT_TRUE(isPhProbeHealthy(100.0f, 100.0f, 0.0f));
}

void test_sonde_pente_89_pct_est_non_saine(void) {
  TEST_ASSERT_FALSE(isPhProbeHealthy(89.0f, 100.0f, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 89.0f, 0.0f));
}

void test_sonde_pente_111_pct_est_non_saine(void) {
  TEST_ASSERT_FALSE(isPhProbeHealthy(111.0f, 100.0f, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 111.0f, 0.0f));
}

void test_sonde_frontieres_de_pente_sont_inclusives(void) {
  TEST_ASSERT_TRUE(isPhProbeHealthy(kPhSlopeHealthyMinPct, kPhSlopeHealthyMaxPct, 0.0f));
  TEST_ASSERT_TRUE(isPhProbeHealthy(90.0f, 110.0f, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(89.9f, 100.0f, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 110.1f, 0.0f));
}

void test_sonde_zero_a_15mv_pile_est_encore_sain(void) {
  TEST_ASSERT_TRUE(isPhProbeHealthy(100.0f, 100.0f, kPhZeroHealthyMaxMv));
  TEST_ASSERT_TRUE(isPhProbeHealthy(100.0f, 100.0f, -kPhZeroHealthyMaxMv));
  TEST_ASSERT_TRUE(isPhProbeHealthy(100.0f, 100.0f, 15.0f));
  TEST_ASSERT_TRUE(isPhProbeHealthy(100.0f, 100.0f, -15.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 100.0f, 15.1f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 100.0f, -15.1f));
}

// Zéro à −30 mV ≈ un décalage INTERNE de 0,5 pH (59 mV/pH) : c'est la signature
// qu'aurait eue une sonde réellement dérivée. Elle doit être déclarée NON saine —
// c'est exactement ce qui la distingue du cas de l'installation (décalage
// EXTERNE, zéro à −1 mV).
void test_sonde_zero_moins_30mv_signe_un_decalage_interne_de_05_ph(void) {
  TEST_ASSERT_FALSE(isPhProbeHealthy(101.0f, 100.0f, -30.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 100.0f, 30.0f));
}

void test_sonde_nan_n_est_jamais_declaree_saine(void) {
  TEST_ASSERT_FALSE(isPhProbeHealthy(NAN, 100.0f, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, NAN, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(100.0f, 100.0f, NAN));
  TEST_ASSERT_FALSE(isPhProbeHealthy(NAN, NAN, NAN));
}

void test_sonde_pentes_effondrees_sont_non_saines(void) {
  TEST_ASSERT_FALSE(isPhProbeHealthy(0.0f, 0.0f, 0.0f));
  TEST_ASSERT_FALSE(isPhProbeHealthy(45.0f, 50.0f, -80.0f));
}

// =============================================================================
// G8 — phModeChangeNeedsAck : table de vérité EXHAUSTIVE (INCHANGÉ v2.25.0)
// =============================================================================

void test_gate_table_de_verite_exhaustive_16_combinaisons(void) {
  for (int i = 0; i < 16; ++i) {
    const bool trace      = (i & 0x8) != 0;
    const bool curManual  = (i & 0x4) != 0;
    const bool newManual  = (i & 0x2) != 0;
    const bool ack        = (i & 0x1) != 0;
    const bool attendu    = trace && curManual && !newManual && !ack;
    char msg[96];
    snprintf(msg, sizeof(msg),
             "combinaison trace=%d cur_manual=%d new_manual=%d ack=%d",
             (int)trace, (int)curManual, (int)newManual, (int)ack);
    TEST_ASSERT_EQUAL_MESSAGE(
        attendu, phModeChangeNeedsAck(trace, curManual, newManual, ack), msg);
  }
}

void test_gate_sans_trace_aucun_acquittement_n_est_jamais_requis(void) {
  for (int i = 0; i < 8; ++i) {
    const bool curManual = (i & 0x4) != 0;
    const bool newManual = (i & 0x2) != 0;
    const bool ack       = (i & 0x1) != 0;
    TEST_ASSERT_FALSE(phModeChangeNeedsAck(false, curManual, newManual, ack));
  }
}

// *** LE CAS DE L'INCIDENT *** : trace active, régulation en manuel, POST qui
// demande "automatic" sans acquittement → acquittement REQUIS (409, trace gardée).
void test_gate_trace_manual_vers_automatic_sans_ack_exige_un_acquittement(void) {
  TEST_ASSERT_TRUE(phModeChangeNeedsAck(true, true, false, false));
}

void test_gate_trace_avec_ack_explicite_autorise_la_sortie_de_manuel(void) {
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(true, true, false, true));
}

void test_gate_trace_manual_vers_manual_ne_demande_rien(void) {
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(true, true, true, false));
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(true, true, true, true));
}

void test_gate_trace_mais_mode_courant_non_manuel_laisse_passer(void) {
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(true, false, false, false));
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(true, false, true, false));
}

void test_gate_entrer_en_manuel_est_toujours_libre(void) {
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(true, false, true, false));
  TEST_ASSERT_FALSE(phModeChangeNeedsAck(false, false, true, false));
}

void test_gate_ack_explicite_neutralise_toutes_les_combinaisons(void) {
  for (int i = 0; i < 8; ++i) {
    const bool trace     = (i & 0x4) != 0;
    const bool curManual = (i & 0x2) != 0;
    const bool newManual = (i & 0x1) != 0;
    TEST_ASSERT_FALSE(phModeChangeNeedsAck(trace, curManual, newManual, true));
  }
}

// =============================================================================
// G9 — Scénarios de la refonte (livrable D1 de la spec)
// =============================================================================
// On reproduit ici le CÂBLAGE de pump_controller.cpp — quelle grandeur est
// comparée à quel seuil — sans sa machine à états (impure, cf. note de
// couverture en fin de fichier).

// Décision de dosage ACIDE (`ph_minus`), gardes au repos :
// refus si la butée BRUTE mord OU si le seuil CORRIGÉ mord.
static bool acideRefuse(float brut, float offset) {
  const float corrige = applyPhOffset(brut, offset);
  return phGuardBlocksBelow(brut, kPhFloorRawHard, false) ||
         phGuardBlocksBelow(corrige, kPhFloorCorrected, false);
}

// TABLE de la spec : (offset −0,50 / 0 / +0,50) × (6,19 / 6,20 / 6,21 / 6,69 /
// 6,71) en pH BRUT. Le verdict attendu est calculé à la main, seuil par seuil.
void test_scenario_table_offset_x_brut_pour_le_dosage_acide(void) {
  // Pré-condition flottante EXPLOITÉE par la ligne (+0,50 ; 6,20, valeur de
  // table sans rapport avec le filet brut fixe 5,50) : le corrigé
  // doit tomber EXACTEMENT sur le seuil, sinon la ligne testerait autre chose que
  // la frontière stricte. ⚠️ 0,50 n'est PLUS kPhOffsetMax (disparu, feature-168) :
  // c'est ici un offset ORDINAIRE, choisi comme cas de table ; kPhOffsetUnusualAbs
  // partage numériquement sa valeur (0,50) mais n'est pas, par construction, le
  // point d'égalité du filet brut — celui-ci (5,50) est FIXE et n'intervient plus
  // dans cette table (tous les brut testés ici, 6,19 à 6,71, restent au-dessus).
  TEST_ASSERT_TRUE_MESSAGE(applyPhOffset(6.20f, kPhOffsetUnusualAbs) == kPhFloorCorrected,
                           "pre-condition : 6,20 brut + 0,50 doit valoir le seuil "
                           "corrige AU BIT PRES");
  struct { float offset; float brut; bool refuse; const char* pourquoi; } cas[] = {
      // offset +0,50 : valeur de table (plus "la borne maximale", feature-168).
      {  0.50f, 6.19f, true,  "corrige 6,69 sous le seuil 6,70 (filet brut 5,50 hors jeu)" },
      {  0.50f, 6.20f, false, "corrige pile au seuil 6,70 (filet brut 5,50 hors jeu)" },
      {  0.50f, 6.21f, false, "corrige 6,71 au-dessus du seuil" },
      {  0.50f, 6.69f, false, "corrige 7,19 : plein fonctionnement nominal" },
      {  0.50f, 6.71f, false, "corrige 7,21 : la consigne 7,20 est atteignable" },
      // offset nul : brut == corrigé, seul le seuil 6,70 décide.
      {  0.00f, 6.19f, true,  "6,19 < 6,70 : le seuil corrige mord (filet brut 5,50 hors jeu)" },
      {  0.00f, 6.20f, true,  "6,20 < 6,70 : le seuil corrige mord" },
      {  0.00f, 6.21f, true,  "6,21 < 6,70" },
      {  0.00f, 6.69f, true,  "6,69 < 6,70 : frontiere stricte" },
      {  0.00f, 6.71f, false, "6,71 > 6,70" },
      // offset −0,50 : le corrigé est PLUS ACIDE que le brut, la butée brute est
      // hors jeu et seul le seuil corrigé décide (fail-closed).
      { -0.50f, 6.19f, true,  "corrige 5,69" },
      { -0.50f, 6.20f, true,  "corrige 5,70" },
      { -0.50f, 6.21f, true,  "corrige 5,71" },
      { -0.50f, 6.69f, true,  "corrige 6,19" },
      { -0.50f, 6.71f, true,  "corrige 6,21" },
  };
  for (unsigned i = 0; i < sizeof(cas) / sizeof(cas[0]); ++i) {
    char msg[192];
    snprintf(msg, sizeof(msg), "offset=%+.2f brut=%.2f corrige=%.4f — %s",
             (double)cas[i].offset, (double)cas[i].brut,
             (double)applyPhOffset(cas[i].brut, cas[i].offset), cas[i].pourquoi);
    TEST_ASSERT_EQUAL_MESSAGE(cas[i].refuse, acideRefuse(cas[i].brut, cas[i].offset), msg);
  }
}

// *** CRITÈRE D'ACCEPTATION *** — offset +0,50 et consigne 7,20 (l'installation
// du 25/08/2026) : AUCUN refus en fonctionnement nominal, c'est-à-dire sur toute
// la descente du pH corrigé jusqu'à la bande de relâche du seuil (6,75). C'est
// exactement ce que l'ancien plancher brut fixe interdisait. ⚠️ 0,50 n'est plus
// "la borne maximale" (disparue, feature-168) : c'est ici l'offset HISTORIQUE de
// l'incident, conservé comme cas de non-régression.
void test_scenario_offset_050_et_consigne_mini_ne_refusent_jamais_en_nominal(void) {
  const float offset = kPhOffsetUnusualAbs;
  bool bloqueCorrige = false, bloqueBrut = false;
  for (int c = 720; c >= 675; --c) {           // corrigé de 7,20 à 6,75
    const float corrige = (float)c * 0.01f;
    const float brut = corrige - offset;       // ce que le capteur lit réellement
    bloqueBrut    = phGuardBlocksBelow(brut, kPhFloorRawHard, bloqueBrut);
    bloqueCorrige = phGuardBlocksBelow(corrige, kPhFloorCorrected, bloqueCorrige);
    char msg[144];
    snprintf(msg, sizeof(msg), "corrige=%.2f brut=%.4f", (double)corrige, (double)brut);
    TEST_ASSERT_FALSE_MESSAGE(bloqueBrut, msg);
    TEST_ASSERT_FALSE_MESSAGE(bloqueCorrige, msg);
  }
}

// Produit BASIQUE (`ph_plus`) : seuil HAUT uniquement, à 7,60 corrigé. 7,59 passe,
// 7,61 refuse. AUCUN seuil bas ne doit s'appliquer — un pH+ sur une eau acide est
// précisément le geste attendu.
// 7,69 NE PASSE PLUS depuis l'abaissement du plafond (kPhHighMargin 0,20 → 0,10) :
// c'est le cœur de l'arbitrage, et la ligne dédiée ci-dessous le verrouille.
void test_scenario_ph_plus_refuse_au_dessus_du_plafond_et_jamais_en_dessous(void) {
  TEST_ASSERT_FALSE(phGuardBlocksAbove(7.59f, kPhCeilingCorrected, false));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.61f, kPhCeilingCorrected, false));
  // Zone 7,60-7,70 : autorisée par l'ancien plafond, désormais REFUSÉE. Y doser du
  // pH+ revenait à alcaliniser une eau dont l'inhibition ORP venait de déclarer le
  // chlore inactif.
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(7.69f, kPhCeilingCorrected, false),
                           "7,69 doit etre REFUSE : l'ancien plafond 7,70 le "
                           "laissait passer au-dessus de kPhOrpInhibit");
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.65f, kPhCeilingCorrected, false));
  // Bande de relâche : une fois refusé, il faut redescendre à 7,55 pour réautoriser.
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.59f, kPhCeilingCorrected, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(kPhCeilingCorrected - kPhGuardReleaseHysteresis,
                                       kPhCeilingCorrected, true));
  // Eau franchement acide : le pH+ n'est JAMAIS refusé par le plafond.
  const float acides[] = {6.00f, 6.19f, 6.69f, 7.00f};
  for (unsigned i = 0; i < sizeof(acides) / sizeof(acides[0]); ++i) {
    TEST_ASSERT_FALSE(phGuardBlocksAbove(acides[i], kPhCeilingCorrected, false));
    TEST_ASSERT_FALSE(phGuardBlocksAbove(acides[i], kPhCeilingCorrected, true));
  }
}

// Inhibition ORP : 7,59 passe, 7,61 inhibe, relâche à 7,55 (exprimée par
// l'expression — cf. NOTE FLOTTANTS en tête de fichier : le littéral 7.55f vaut
// 3e-7 de PLUS que kPhOrpInhibit − kPhGuardReleaseHysteresis et bloquerait encore).
void test_scenario_inhibition_orp_sur_le_corrige_avec_relache_a_755(void) {
  const float relache = kPhOrpInhibit - kPhGuardReleaseHysteresis;
  assertOrdreStrict(relache, kPhOrpInhibit, "relache ORP sous le seuil");
  TEST_ASSERT_FALSE(phGuardBlocksAbove(7.59f, kPhOrpInhibit, false));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.61f, kPhOrpInhibit, false));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.59f, kPhOrpInhibit, true));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(7.56f, kPhOrpInhibit, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(relache, kPhOrpInhibit, true));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(7.54f, kPhOrpInhibit, true));
}

// L'inhibition ORP porte sur le CORRIGÉ (v2.25.0) : à consigne tenue, un offset
// légitime ne doit pas la déclencher — et un pH RÉEL de 7,61 la déclenche quel
// que soit l'offset, ce qui est tout l'objet du changement de référentiel.
void test_scenario_inhibition_orp_suit_le_ph_reel_et_non_la_lecture_brute(void) {
  const float offsets[] = {-kPhOffsetUnusualAbs, -0.20f, 0.0f, 0.20f, kPhOffsetUnusualAbs};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    const float offset = offsets[i];
    char msg[128];
    snprintf(msg, sizeof(msg), "offset=%+.2f", (double)offset);
    // Eau RÉELLE à 7,61 : inhibition, quelle que soit la lecture brute.
    const float brutHaut = 7.61f - offset;
    TEST_ASSERT_TRUE_MESSAGE(
        phGuardBlocksAbove(applyPhOffset(brutHaut, offset), kPhOrpInhibit, false), msg);
    // Eau RÉELLE à la consigne haute 7,50 : jamais d'inhibition (le static_assert
    // kPhTargetMax + marge <= kPhOrpInhibit garantit cette marge structurelle).
    const float brutConsigne = kPhTargetMax - offset;
    TEST_ASSERT_FALSE_MESSAGE(
        phGuardBlocksAbove(applyPhOffset(brutConsigne, offset), kPhOrpInhibit, false), msg);
  }
}

// Le filet BRUT haut (FIXE, 8,50 depuis feature-168) est le dernier recours
// contre un offset NÉGATIF faux : une eau réellement à 8,55 affichée 7,65
// (offset −0,90) est vue par les DEUX gardes ORP.
void test_scenario_butee_brute_haute_rattrape_un_offset_negatif_faux(void) {
  const float brut = 8.55f;
  const float offset = -0.90f;
  const float affiche = applyPhOffset(brut, offset);  // 7,65
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 7.65f, affiche);
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(brut, kPhCeilingRawHard, false),
                           "filet brut haut : 8,55 > 8,50");
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(affiche, kPhOrpInhibit, false),
                           "seuil corrige : 7,65 > 7,60");
  // TROISIÈME garde depuis l'abaissement du plafond : le pH+ est refusé lui aussi.
  // Avec l'ancien plafond 7,70, cette même eau (7,65 affiché) autorisait encore
  // l'alcalinisation pendant que les deux gardes ORP suspendaient le chlore.
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(affiche, kPhCeilingCorrected, false),
                           "plafond d'alcalinisation : 7,65 > 7,60");
}

// =============================================================================
// NOTE DE COUVERTURE — ce qui n'est PAS testable dans cette suite
// =============================================================================
// PumpControllerClass::isPhGuardBlocking() et ::isOrpInhibitedByPh() ne sont pas
// des fonctions pures (membres d'état, singletons sensors/mqttCfg/mqttManager,
// millis(), saveMqttConfig()). Restent à couvrir par pool-chemistry + tests
// on-target :
//   - compteur des 3 refus / 24 h INCRÉMENTÉ EN AUTOMATIQUE SEULEMENT (le mode
//     Programmée passe countRefusal=false : signalement warn à débit limité, pas
//     d'escalade) ;
//   - fenêtre glissante 24 h et bascule fail-closed en manuel au 3ᵉ refus, avec
//     `ph_enabled` remis à false et trace persistée AVANT saveMqttConfig() ;
//   - réarmement des drapeaux au changement de produit de correction, compteur
//     de refus CONSERVÉ ;
//   - drapeau d'hystérésis PARTAGÉ par les deux seuils de l'inhibition ORP
//     (levée seulement quand les deux sont retombés) ;
//   - un seul log critical + une seule alerte MQTT par transition.
// =============================================================================

int main(int, char**) {
  UNITY_BEGIN();
  // G1 — garde basse
  RUN_TEST(test_bas_aucun_blocage_au_dessus_du_seuil);
  RUN_TEST(test_bas_bloque_des_le_franchissement);
  RUN_TEST(test_bas_entree_stricte_pile_au_seuil_ne_bloque_pas);
  RUN_TEST(test_bas_reste_bloquant_dans_la_bande_de_relache);
  RUN_TEST(test_bas_relache_a_la_frontiere_exacte_et_au_dela);
  RUN_TEST(test_bas_meme_mesure_verdict_oppose_selon_etat_precedent);
  RUN_TEST(test_bas_cycle_collage_sejour_decollage);
  RUN_TEST(test_bas_appels_repetes_sont_idempotents);
  RUN_TEST(test_bas_meme_mecanique_sur_le_seuil_corrige_et_la_butee_brute);
  // G2 — garde haute
  RUN_TEST(test_haut_aucun_blocage_sous_le_seuil);
  RUN_TEST(test_haut_bloque_des_le_franchissement);
  RUN_TEST(test_haut_entree_stricte_pile_au_seuil_ne_bloque_pas);
  RUN_TEST(test_haut_reste_bloquant_dans_la_bande_de_relache);
  RUN_TEST(test_haut_relache_a_la_frontiere_exacte_et_en_deca);
  RUN_TEST(test_haut_meme_mesure_verdict_oppose_selon_etat_precedent);
  RUN_TEST(test_haut_cycle_collage_sejour_decollage);
  RUN_TEST(test_haut_appels_repetes_sont_idempotents);
  RUN_TEST(test_haut_meme_mecanique_sur_les_trois_seuils_hauts);
  // G3 — côté de l'hystérésis
  RUN_TEST(test_cote_bas_la_bande_de_relache_est_au_dessus_du_seuil);
  RUN_TEST(test_cote_haut_la_bande_de_relache_est_sous_le_seuil);
  RUN_TEST(test_cote_l_hysteresis_elargit_toujours_la_zone_de_blocage);
  RUN_TEST(test_cote_au_repos_les_deux_gardes_ne_bloquent_jamais_ensemble);
  RUN_TEST(test_cote_armees_les_bandes_de_relache_se_chevauchent);
  // G4 — NaN
  RUN_TEST(test_nan_mesure_ne_produit_aucun_refus_dans_les_deux_directions);
  RUN_TEST(test_nan_seuil_ne_produit_aucun_refus);
  RUN_TEST(test_infinis_sont_traites_comme_des_mesures_ordonnees);
  RUN_TEST(test_nan_une_serie_saine_trouee_de_nan_ne_compte_aucun_refus);
  RUN_TEST(test_un_episode_de_blocage_ne_compte_qu_un_seul_refus);
  RUN_TEST(test_un_trou_de_nan_pendant_un_blocage_rearme_la_garde);
  // G5 — arrondi de l'offset
  RUN_TEST(test_arrondi_les_valeurs_sur_la_grille_sont_inchangees);
  RUN_TEST(test_arrondi_au_plus_proche);
  RUN_TEST(test_arrondi_valeurs_a_mi_chemin_s_ecartent_de_zero);
  RUN_TEST(test_arrondi_ne_clampe_plus_au_dela_de_l_ancienne_borne);
  RUN_TEST(test_arrondi_reste_a_un_demi_pas_de_l_entree_sur_tout_le_domaine);
  RUN_TEST(test_arrondi_ne_produit_jamais_de_zero_negatif);
  RUN_TEST(test_arrondi_aucun_zero_negatif_sur_tout_le_domaine);
  RUN_TEST(test_arrondi_offset_nan_devient_zero_neutre);
  RUN_TEST(test_arrondi_est_idempotent_sur_tout_le_domaine);
  RUN_TEST(test_arrondi_toute_sortie_est_un_multiple_exact_du_pas);
  RUN_TEST(test_arrondi_est_monotone_croissant);
  // G6 — valeurs des dérivés
  RUN_TEST(test_derives_valeurs_numeriques_exactes);
  RUN_TEST(test_derives_source_unique_et_marges);
  RUN_TEST(test_derives_ordonnancement_de_la_fenetre_chimique);
  RUN_TEST(test_derives_la_butee_brute_basse_ne_mord_jamais_la_premiere);
  RUN_TEST(test_derives_la_butee_brute_haute_ne_mord_jamais_avant_l_inhibition_orp);
  RUN_TEST(test_derives_le_plafond_ne_depasse_jamais_l_inhibition_orp);
  RUN_TEST(test_filet_brut_independant_de_l_offset_sur_tout_le_domaine);
  // G6bis — zone d'impasse de regulation (feature-168, C5 + C5bis)
  RUN_TEST(test_impasse_basse_valeur_du_seuil_calcule);
  RUN_TEST(test_impasse_basse_pile_au_seuil_n_est_pas_en_impasse);
  RUN_TEST(test_impasse_basse_un_centieme_au_dela_est_en_impasse);
  RUN_TEST(test_impasse_basse_un_centieme_en_deca_n_est_pas_en_impasse);
  RUN_TEST(test_impasse_basse_offsets_usuels_et_extremes);
  RUN_TEST(test_impasse_haute_valeur_du_seuil_calcule);
  RUN_TEST(test_impasse_haute_pile_au_seuil_est_en_impasse);
  RUN_TEST(test_impasse_haute_un_centieme_au_dela_n_est_pas_en_impasse);
  RUN_TEST(test_impasse_haute_un_centieme_en_deca_est_en_impasse);
  RUN_TEST(test_impasse_haute_offsets_usuels_et_extremes);
  RUN_TEST(test_impasse_les_deux_zones_ne_se_chevauchent_jamais);
  // G7 — santé de la sonde
  RUN_TEST(test_sonde_cas_nominal_installation_reelle_est_saine);
  RUN_TEST(test_sonde_pentes_parfaites_et_zero_nul_sont_sains);
  RUN_TEST(test_sonde_pente_89_pct_est_non_saine);
  RUN_TEST(test_sonde_pente_111_pct_est_non_saine);
  RUN_TEST(test_sonde_frontieres_de_pente_sont_inclusives);
  RUN_TEST(test_sonde_zero_a_15mv_pile_est_encore_sain);
  RUN_TEST(test_sonde_zero_moins_30mv_signe_un_decalage_interne_de_05_ph);
  RUN_TEST(test_sonde_nan_n_est_jamais_declaree_saine);
  RUN_TEST(test_sonde_pentes_effondrees_sont_non_saines);
  // G8 — gate d'acquittement
  RUN_TEST(test_gate_table_de_verite_exhaustive_16_combinaisons);
  RUN_TEST(test_gate_sans_trace_aucun_acquittement_n_est_jamais_requis);
  RUN_TEST(test_gate_trace_manual_vers_automatic_sans_ack_exige_un_acquittement);
  RUN_TEST(test_gate_trace_avec_ack_explicite_autorise_la_sortie_de_manuel);
  RUN_TEST(test_gate_trace_manual_vers_manual_ne_demande_rien);
  RUN_TEST(test_gate_trace_mais_mode_courant_non_manuel_laisse_passer);
  RUN_TEST(test_gate_entrer_en_manuel_est_toujours_libre);
  RUN_TEST(test_gate_ack_explicite_neutralise_toutes_les_combinaisons);
  // G9 — scénarios de la refonte
  RUN_TEST(test_scenario_table_offset_x_brut_pour_le_dosage_acide);
  RUN_TEST(test_scenario_offset_050_et_consigne_mini_ne_refusent_jamais_en_nominal);
  RUN_TEST(test_scenario_ph_plus_refuse_au_dessus_du_plafond_et_jamais_en_dessous);
  RUN_TEST(test_scenario_inhibition_orp_sur_le_corrige_avec_relache_a_755);
  RUN_TEST(test_scenario_inhibition_orp_suit_le_ph_reel_et_non_la_lecture_brute);
  RUN_TEST(test_scenario_butee_brute_haute_rattrape_un_offset_negatif_faux);
  return UNITY_END();
}
