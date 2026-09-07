// =============================================================================
// Tests natifs — COMPOSITION DU DÉBIT D'INJECTION (feature-092, A1/A2/A3)
// =============================================================================
// Cibles PURES (ADR-0017), compilées sur PC sans ESP32 :
//   - doseComposition()            — src/dosing_logic.h / .cpp   (A3)
//   - phFeedforwardLearnedFlow()   — src/ph_feedforward_logic.h  (A1)
//   - phFeedforwardServedStatus()  — src/ph_feedforward_logic.h  (A2)
//
// CE QUE CETTE SUITE PROTÈGE, ET POURQUOI ELLE EXISTE
// ---------------------------------------------------
// feature-092 est annoncée « observabilité, aucun changement de dosage ». Cette
// promesse ne tient QUE si la fonction de composition est un miroir LITTÉRAL du
// tick. La première écriture ne l'était pas : elle bornait la somme des trois
// termes à zéro, là où le firmware borne le COUPLE (P + I) AVANT d'ajouter le
// feedforward. Le contre-exemple exigé par `pool-chemistry` (bloc A ci-dessous)
// est le seul test de cette suite qui distingue les deux formules — et il
// distingue un dosage RÉEL de 1,41 mL/min d'un dosage nul. Il est écrit en
// premier, nommé explicitement, et sa disparition doit être remarquée.
//
// ORGANISATION
//   A — LE CONTRE-EXEMPLE (borne basse sur le couple, jamais sur la somme)
//   B — ordre interne des six étapes de doseComposition(), point par point
//   C — bande morte : sur le COUPLE, frontière STRICTE, valeur lue de l'appelant
//   D — plafonds, régimes, propriété demande == PI servi + FF (200 tirages)
//   E — A1 : dose de base apprise = min(charge, fraction du soutenable)
//   F — A2 : statut réellement SERVI (traductions 1→0 et 3→0, 2 et 4 jamais)
//
// HORS D'ATTEINTE EN NATIF (dit sans détour) : les accesseurs de
// pump_controller.cpp qui traduisent l'indisponibilité en NAN (soutenable
// non caractérisé, historique < 3 jours, mesure non représentative, kp == 0).
// Ils lisent des globals ESP32 (mqttCfg, measureValidity, phPID) et ne sont pas
// compilables ici : le bloc E documente la frontière exacte entre le 0
// fail-closed du module PUR et le NAN de l'accesseur, et le bloc F couvre la
// conséquence observable de ce NAN (paramètre `learnedFinite`).
// =============================================================================

#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "dosing_logic.h"
#include "ph_feedforward_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Tolérance : les grandeurs manipulées sont des mL/min à deux décimales.
static const float kEps = 1e-4f;

// Bande morte « ouverte » par défaut dans les cas qui ne la testent pas : les
// erreurs employées (± 0,20 pH et plus) sont franchement au-dehors.
static const float kDbOpen = 0.05f;

// =============================================================================
// A — LE CONTRE-EXEMPLE EXIGÉ PAR `pool-chemistry`
// =============================================================================
// ff = 1,41 · p = −3,00 · i = 0 · régime Full ⇒ la régulation demande 1,41
// mL/min, PAS zéro. Le tick réduit le plafond du PI de la part servie au
// feedforward (`phPiMaxFlow = ampFlow − ff`, borné ≥ 0), borne le PI dans
// [0 ; ce plafond], PUIS ajoute le feedforward. Une formule qui bornerait
// clamp(p + i + ff, 0, ampFlow) rendrait 0,00 : elle ne décrirait pas ce que
// l'appareil injecte, et la carte « Débit d'injection » mentirait EXACTEMENT
// dans le cas où l'utilisateur cherche à comprendre pourquoi la pompe tourne
// alors que le pH est sous la consigne.
void test_A_contre_exemple_ff_141_p_moins_3_demande_vaut_141_et_non_zero(void) {
  // kp = 15, erreur = −0,20 pH ⇒ p = −3,00 mL/min exactement.
  const DoseComposition c = doseComposition(/*kp*/ 15.0f, /*error*/ -0.20f,
                                            /*kiIntegral*/ 0.0f, kDbOpen,
                                            /*ffServed*/ 1.41f, /*ampFlow*/ 5.0f,
                                            DoseFlowRegime::Full);

  // 1. Le terme proportionnel est servi TEL QUEL, signé, non borné : c'est la
  //    grandeur pédagogique de la ligne « Écart courant ».
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, -3.00f, c.p,
      "Le terme P doit etre servi SIGNE et NON borne (-3,00 mL/min).");

  // 2. Le couple (P + I) est borné à zéro AVANT l'addition du feedforward.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.piServed,
      "Le PI servi doit etre borne a 0 (le couple est negatif).");

  // 3. LE POINT DUR : la demande vaut le feedforward, pas zéro.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 1.41f, c.demand,
      "REGRESSION DE DOSAGE : la demande doit valoir 1,41 mL/min (le FF est "
      "ajoute APRES le bornage du couple P+I). Une borne basse appliquee a la "
      "somme des trois termes rendrait 0,00 : ce n'est pas ce que le tick "
      "injecte. Cf. dosing_logic.h, section ORDRE INTERNE.");

  // 4. Et l'on interdit explicitement la valeur qu'aurait rendue la formule
  //    fautive — pour que le message d'échec soit sans ambiguïté.
  TEST_ASSERT_TRUE_MESSAGE(c.demand > 1.0f,
      "REGRESSION DE DOSAGE : demande nulle ou quasi nulle alors que le "
      "feedforward est servi. La borne >= 0 a ete deplacee APRES l'addition du "
      "feedforward.");

  // 5. Le feedforward est intégralement servi en régime Full.
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.41f, c.ff);
  TEST_ASSERT_FALSE(c.inDeadband);
}

// Corollaire du contre-exemple : le même couple négatif SANS feedforward donne
// bien zéro. C'est ce qui prouve que le 1,41 ci-dessus vient du FF et non d'une
// borne basse manquante.
void test_A_meme_couple_negatif_sans_feedforward_donne_zero(void) {
  const DoseComposition c = doseComposition(15.0f, -0.20f, 0.0f, kDbOpen,
                                            /*ffServed*/ 0.0f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN(kEps, -3.00f, c.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.demand);
}

// Variante du contre-exemple avec un terme intégral positif qui ne compense pas
// le proportionnel : le couple reste négatif, le FF reste servi.
void test_A_couple_negatif_malgre_integrale_positive_le_ff_reste_servi(void) {
  const DoseComposition c = doseComposition(15.0f, -0.20f, /*kiIntegral*/ 1.00f,
                                            kDbOpen, 1.41f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN(kEps, -3.00f, c.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.00f, c.i);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.41f, c.demand);
}

// =============================================================================
// B — ORDRE INTERNE, ÉTAPE PAR ÉTAPE
// =============================================================================

// Étape 1 — FAIL-CLOSED : erreur non finie ⇒ les CINQ flottants à NAN et
// inDeadband = false. On rend NAN (→ « — ») et jamais 0, qui se lirait « pH à la
// consigne » sur une mesure absente.
void test_B1_erreur_nan_rend_cinq_nan_et_pas_de_bande_morte(void) {
  const DoseComposition c = doseComposition(15.0f, NAN, 2.0f, kDbOpen, 1.41f,
                                            5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_TRUE_MESSAGE(isnan(c.p),        "p doit etre NAN");
  TEST_ASSERT_TRUE_MESSAGE(isnan(c.i),        "i doit etre NAN");
  TEST_ASSERT_TRUE_MESSAGE(isnan(c.piServed), "piServed doit etre NAN");
  TEST_ASSERT_TRUE_MESSAGE(isnan(c.ff),       "ff doit etre NAN");
  TEST_ASSERT_TRUE_MESSAGE(isnan(c.demand),   "demand doit etre NAN");
  TEST_ASSERT_FALSE_MESSAGE(c.inDeadband,
      "inDeadband doit etre FAUX sur une mesure absente : une bande morte "
      "annoncee vraie se lirait « pH a la consigne ».");
}

void test_B1_erreur_infinie_rend_les_memes_nan(void) {
  const DoseComposition c = doseComposition(15.0f, INFINITY, 0.0f, kDbOpen,
                                            1.41f, 5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_TRUE(isnan(c.p));
  TEST_ASSERT_TRUE(isnan(c.demand));
  TEST_ASSERT_FALSE(c.inDeadband);
  const DoseComposition n = doseComposition(15.0f, -INFINITY, 0.0f, kDbOpen,
                                            1.41f, 5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_TRUE(isnan(n.piServed));
  TEST_ASSERT_TRUE(isnan(n.ff));
}

// Étape 3 — p et i sont BRUTS : jamais bornés, ni en haut ni en bas, quel que
// soit le plafond d'amplitude. C'est ce qui autorise l'affichage de « −0,35 » et
// de « 2,10 » à côté d'une demande bridée.
void test_B3_p_et_i_ne_sont_jamais_bornes_par_l_amplitude(void) {
  // p = 15 × 2,0 = 30 mL/min pour une amplitude de 4,33 : p n'est PAS écrêté.
  const DoseComposition c = doseComposition(15.0f, 2.0f, /*kiIntegral*/ 12.0f,
                                            kDbOpen, 1.41f, /*ampFlow*/ 4.33f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 30.0f, c.p,
      "p doit rester BRUT (30 mL/min) meme au-dela de l'amplitude.");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 12.0f, c.i,
      "i doit rester BRUT (12 mL/min) meme au-dela de l'amplitude.");
  // ... alors que le servi, lui, est borné.
  TEST_ASSERT_FLOAT_WITHIN(kEps, 4.33f - 1.41f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 4.33f, c.demand);
}

void test_B3_i_negatif_est_servi_signe_et_non_borne(void) {
  const DoseComposition c = doseComposition(0.0f, 1.0f, /*kiIntegral*/ -7.5f,
                                            kDbOpen, 0.0f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN(kEps, -7.5f, c.i);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.demand);
}

// Étape 6 — la demande n'est JAMAIS négative, quelles que soient les entrées.
void test_B6_demande_jamais_negative(void) {
  const DoseComposition a = doseComposition(15.0f, -10.0f, -50.0f, kDbOpen,
                                            0.0f, 5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_TRUE_MESSAGE(a.demand >= 0.0f, "demande negative interdite");
  const DoseComposition b = doseComposition(15.0f, -10.0f, -50.0f, kDbOpen,
                                            1.41f, 5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.41f, b.demand);
}

// =============================================================================
// C — BANDE MORTE : SUR LE COUPLE, FRONTIÈRE STRICTE, VALEUR LUE
// =============================================================================

// La bande morte annule le COUPLE, jamais le seul terme proportionnel : dans la
// bande, computePidPure rend 0 ET n'accumule rien.
void test_C_bande_morte_annule_p_ET_i(void) {
  const DoseComposition c = doseComposition(15.0f, /*error*/ 0.02f,
                                            /*kiIntegral*/ 2.50f,
                                            /*deadband*/ 0.05f, 0.0f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_TRUE(c.inDeadband);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.p, "p doit etre 0 dans la bande morte");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.i,
      "i doit etre 0 dans la bande morte : la bande porte sur le COUPLE, pas "
      "sur le seul terme proportionnel. Un rattrapage affiche a cote d'un ecart "
      "nul decrirait un dosage qui n'a pas lieu.");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.demand);
}

// Dans la bande morte, le feedforward reste servi (il ne dépend pas de l'écart).
void test_C_bande_morte_laisse_le_feedforward(void) {
  const DoseComposition c = doseComposition(15.0f, 0.0f, 2.50f, 0.05f,
                                            /*ffServed*/ 1.41f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_TRUE(c.inDeadband);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.41f, c.ff);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.41f, c.demand);
}

// Frontière STRICTE : |erreur| == deadband est HORS bande morte (même règle que
// computePidPure — c'est le seuil de DÉMARRAGE, atteint il déclenche).
void test_C_frontiere_stricte_egalite_est_hors_bande_morte(void) {
  const DoseComposition c = doseComposition(/*kp*/ 15.0f, /*error*/ 0.05f,
                                            0.0f, /*deadband*/ 0.05f, 0.0f,
                                            5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_FALSE_MESSAGE(c.inDeadband,
      "FRONTIERE : |erreur| == deadband est HORS bande morte (comparaison "
      "STRICTE <, comme computePidPure).");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.75f, c.p,
      "A la frontiere exacte, p doit valoir kp*erreur (0,75), pas 0.");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.75f, c.demand);
}

void test_C_frontiere_stricte_negative_egalite_est_hors_bande_morte(void) {
  const DoseComposition c = doseComposition(15.0f, /*error*/ -0.05f, 3.0f,
                                            /*deadband*/ 0.05f, 0.0f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FALSE(c.inDeadband);
  TEST_ASSERT_FLOAT_WITHIN(kEps, -0.75f, c.p);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 3.0f, c.i,
      "Hors bande morte, i est servi tel quel meme si p est negatif.");
}

// Juste EN DEÇÀ de la frontière : dans la bande.
void test_C_juste_en_deca_de_la_frontiere_est_dans_la_bande_morte(void) {
  const DoseComposition c = doseComposition(15.0f, 0.0499f, 3.0f, 0.05f, 0.0f,
                                            5.0f, DoseFlowRegime::Full);
  TEST_ASSERT_TRUE(c.inDeadband);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.i);
}

// La bande morte VIENT DE L'APPELANT (verdict de porte de session) : elle n'est
// jamais reconstruite depuis les seuils. Même erreur, deux deadbands, deux
// verdicts — c'est le mécanisme exact du défaut v2.26.0, qui mentait dans la
// bande ]0,01 ; 0,05].
void test_C_la_bande_morte_vient_de_l_appelant_jamais_recalculee(void) {
  const float err = 0.03f;  // dans ]stop=0,01 ; start=0,05]
  const DoseComposition ferme = doseComposition(15.0f, err, 0.0f,
                                                /*deadband*/ 0.05f, 0.0f, 5.0f,
                                                DoseFlowRegime::Full);
  const DoseComposition ouvert = doseComposition(15.0f, err, 0.0f,
                                                 /*deadband*/ 0.01f, 0.0f, 5.0f,
                                                 DoseFlowRegime::Full);
  TEST_ASSERT_TRUE_MESSAGE(ferme.inDeadband,
      "Session FERMEE (deadband = phStartThreshold) : 0,03 est dans la bande.");
  TEST_ASSERT_FALSE_MESSAGE(ouvert.inDeadband,
      "Session OUVERTE (deadband = phStopThreshold) : 0,03 est HORS bande. Si "
      "les deux verdicts sont identiques, la bande morte est recalculee en "
      "interne au lieu d'etre LUE du verdict de porte de session.");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f,  ferme.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.45f, ouvert.p);
}

// Deadband nulle : tout écart non nul est hors bande (et 0 exactement l'est
// aussi, puisque |0| < 0 est faux).
void test_C_deadband_nulle_ne_met_rien_dans_la_bande(void) {
  const DoseComposition c = doseComposition(15.0f, 0.0f, 0.0f,
                                            /*deadband*/ 0.0f, 0.0f, 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FALSE(c.inDeadband);
}

// =============================================================================
// D — PLAFONDS, RÉGIMES, PROPRIÉTÉ D'ADDITION
// =============================================================================

// Le PI servi est plafonné à l'amplitude RESTANTE après le feedforward :
// exactement `ampFlow − ff`, frontière atteinte.
void test_D_plafond_du_pi_vaut_amplitude_moins_ff(void) {
  const DoseComposition c = doseComposition(/*kp*/ 15.0f, /*error*/ 1.0f,
                                            /*kiIntegral*/ 0.0f, kDbOpen,
                                            /*ffServed*/ 1.41f,
                                            /*ampFlow*/ 4.33f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 15.0f, c.p, "p reste brut");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 4.33f - 1.41f, c.piServed,
      "Le PI servi doit etre plafonne a l'amplitude RESTANTE (ampFlow - ff).");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 4.33f, c.demand,
      "La demande sature l'amplitude, jamais au-dela.");
}

// Sous le plafond, le PI servi vaut exactement le couple.
void test_D_sous_le_plafond_le_pi_servi_vaut_le_couple(void) {
  const DoseComposition c = doseComposition(1.0f, /*error*/ 0.35f,
                                            /*kiIntegral*/ 2.10f, kDbOpen,
                                            1.41f, 10.0f, DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.35f, c.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 2.10f, c.i);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 2.45f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 3.86f, c.demand);
}

// `fmaxf(0, ampFlow − ff)` : un feedforward SUPÉRIEUR à l'amplitude donne un
// plafond de PI NÉGATIF. Sans la protection, clampf(v, 0, plafond_negatif) rend
// le plafond — soit un PI servi négatif, affiché tel quel.
void test_D_ff_superieur_a_l_amplitude_donne_un_pi_servi_nul_jamais_negatif(void) {
  const DoseComposition c = doseComposition(/*kp*/ 1.0f, /*error*/ 2.0f,
                                            0.0f, kDbOpen, /*ffServed*/ 6.0f,
                                            /*ampFlow*/ 5.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.piServed,
      "PI SERVI NEGATIF : le plafond ampFlow - ff est negatif (ff > ampFlow) et "
      "n'a pas ete ramene a 0 par fmaxf. Un PI servi negatif se soustrairait au "
      "feedforward dans la lecture de la carte.");
  TEST_ASSERT_TRUE_MESSAGE(c.piServed >= 0.0f, "piServed doit rester >= 0");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 5.0f, c.demand);
}

// Amplitude nulle (pompe non caractérisée en amplitude) : rien n'est servi.
void test_D_amplitude_nulle_ne_sert_rien(void) {
  const DoseComposition c = doseComposition(15.0f, 1.0f, 3.0f, kDbOpen, 1.41f,
                                            /*ampFlow*/ 0.0f,
                                            DoseFlowRegime::Full);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.demand);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 15.0f, c.p,
      "Meme sans amplitude, le terme P mesure reste affichable.");
}

// Régime Zero (bassin sous la consigne pendant une prolongation) : FF coupé, PI
// annulé, demande nulle.
void test_D_regime_zero_coupe_le_ff_et_le_pi(void) {
  const DoseComposition c = doseComposition(/*kp*/ 15.0f, /*error*/ 1.0f,
                                            /*kiIntegral*/ 2.0f, kDbOpen,
                                            /*ffServed*/ 1.41f, 5.0f,
                                            DoseFlowRegime::Zero);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.ff,
      "REGIME Zero NON APPLIQUE : le feedforward doit etre coupe (bassin sous "
      "la consigne pendant une prolongation de session).");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.piServed);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.demand,
      "REGIME Zero NON APPLIQUE : la demande doit etre NULLE.");
  // Les termes mesurés restent bruts : le régime borne ce qui est SERVI.
  TEST_ASSERT_FLOAT_WITHIN(kEps, 15.0f, c.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 2.0f,  c.i);
}

// Régime CappedToFeedforward : le FF est servi, le PI est annulé — la demande ne
// peut pas dépasser le feedforward SEUL.
void test_D_regime_capped_annule_le_pi_et_sert_le_ff_seul(void) {
  const DoseComposition c = doseComposition(/*kp*/ 15.0f, /*error*/ 1.0f,
                                            /*kiIntegral*/ 2.0f, kDbOpen,
                                            /*ffServed*/ 1.41f, 5.0f,
                                            DoseFlowRegime::CappedToFeedforward);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 1.41f, c.ff);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 0.0f, c.piServed,
      "REGIME CappedToFeedforward NON APPLIQUE : le PI doit etre annule.");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 1.41f, c.demand,
      "REGIME CappedToFeedforward NON APPLIQUE : la demande doit valoir le "
      "feedforward SEUL (prolongation de session).");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 15.0f, c.p);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 2.0f,  c.i);
}

// Régime Capped avec un FF nul : la prolongation ne dose rien.
void test_D_regime_capped_sans_ff_ne_dose_rien(void) {
  const DoseComposition c = doseComposition(15.0f, 1.0f, 2.0f, kDbOpen,
                                            /*ffServed*/ 0.0f, 5.0f,
                                            DoseFlowRegime::CappedToFeedforward);
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, c.demand);
}

// Générateur pseudo-aléatoire déterministe (LCG) : reproductible, sans <random>.
static uint32_t sLcg = 0x13572468u;
static float frand(float lo, float hi) {
  sLcg = sLcg * 1664525u + 1013904223u;
  const float u = (float)((sLcg >> 8) & 0xFFFFFFu) / (float)0xFFFFFFu;
  return lo + u * (hi - lo);
}

// PROPRIÉTÉ, 200 tirages : la demande est EXACTEMENT la somme du PI servi et du
// feedforward servi. C'est l'invariant que la carte affiche (trois lignes + un
// total) : si la somme des lignes ne fait pas le total, l'utilisateur cherchera
// une explication qui n'existe pas.
void test_D_propriete_demande_egale_pi_servi_plus_ff_sur_200_tirages(void) {
  sLcg = 0x13572468u;
  for (int n = 0; n < 200; ++n) {
    const float kp   = frand(0.0f, 30.0f);
    const float err  = frand(-2.0f, 2.0f);
    const float ki   = frand(-4.0f, 6.0f);
    const float db   = frand(0.0f, 0.10f);
    const float amp  = frand(0.05f, 8.0f);
    const float ff   = frand(0.0f, amp);  // ff <= amp : cas nominal du tick
    const DoseFlowRegime reg = (n % 7 == 0) ? DoseFlowRegime::Zero
                             : (n % 5 == 0) ? DoseFlowRegime::CappedToFeedforward
                                            : DoseFlowRegime::Full;
    const DoseComposition c = doseComposition(kp, err, ki, db, ff, amp, reg);

    char msg[192];
    snprintf(msg, sizeof(msg),
             "Tirage %d : demande %.5f != piServed %.5f + ff %.5f — la somme "
             "des lignes de la carte ne fait plus le total.",
             n, (double)c.demand, (double)c.piServed, (double)c.ff);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, c.piServed + c.ff, c.demand, msg);

    snprintf(msg, sizeof(msg), "Tirage %d : demande %.5f hors [0 ; %.5f]",
             n, (double)c.demand, (double)amp);
    TEST_ASSERT_TRUE_MESSAGE(c.demand >= 0.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(c.demand <= amp + 1e-3f, msg);
    TEST_ASSERT_TRUE_MESSAGE(c.piServed >= 0.0f, msg);
  }
}

// =============================================================================
// E — A1 : DOSE DE BASE APPRISE = min(charge, fraction du soutenable)
// =============================================================================

static PhPersistLoadObservation obsOf(float load, uint8_t days) {
  PhPersistLoadObservation o;
  o.loadMlPerMin = load;
  o.days = days;
  return o;
}

// La charge gouverne quand elle est SOUS le plafond.
void test_E_charge_sous_le_plafond_est_servie_telle_quelle(void) {
  // plafond = 0,60 × 4,33 = 2,598
  const float v = phFeedforwardLearnedFlow(obsOf(1.41f, 7), 4.33f);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, 1.41f, v,
      "Sous le plafond, la dose apprise EST la charge observee.");
}

// Le PLAFOND gouverne quand la charge le dépasse : c'est le `min`, et il garantit
// qu'il reste (1 − fraction) du soutenable au PI.
void test_E_charge_au_dela_du_plafond_est_ecretee_a_la_fraction(void) {
  const float sust = 2.00f;
  const float v = phFeedforwardLearnedFlow(obsOf(/*charge*/ 5.00f, 7), sust);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, kPhFfMaxSustainableFraction * sust, v,
      "PLAFOND NON APPLIQUE : la dose apprise doit etre ECRETEE a la fraction "
      "du soutenable (min, pas max). Sans ce min, le feedforward ne laisserait "
      "AUCUNE marge au PI pour l'ecart residuel.");
  TEST_ASSERT_TRUE_MESSAGE(v < 5.00f,
      "PLAFOND NON APPLIQUE : la valeur servie est la charge brute.");
  TEST_ASSERT_TRUE_MESSAGE(v < sust,
      "La dose apprise doit rester strictement sous le soutenable.");
}

// Frontière : charge == plafond ⇒ plafond (les deux branches coïncident).
void test_E_charge_egale_au_plafond(void) {
  const float sust = 4.00f;
  const float cap = kPhFfMaxSustainableFraction * sust;
  TEST_ASSERT_FLOAT_WITHIN(kEps, cap, phFeedforwardLearnedFlow(obsOf(cap, 7), sust));
}

// Charge NULLE avec un historique exploitable : 0,00 est une OBSERVATION
// légitime (trois journées sans injection), pas une indisponibilité. Le module
// pur rend 0 ; c'est l'accesseur qui distingue « 0,00 appris » de « rien
// d'appris » (NAN) — cf. l'en-tête de cette suite.
void test_E_charge_nulle_rend_zero_et_pas_nan(void) {
  const float v = phFeedforwardLearnedFlow(obsOf(0.0f, 7), 4.33f);
  TEST_ASSERT_FALSE_MESSAGE(isnan(v), "0,00 appris n'est pas une indisponibilite");
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, v);
}

// FAIL-CLOSED du module pur : soutenable non fini, nul ou négatif ⇒ 0.
void test_E_soutenable_non_fini_ou_non_positif_rend_zero(void) {
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(1.41f, 7), NAN));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(1.41f, 7), INFINITY));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(1.41f, 7), 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(1.41f, 7), -1.0f));
}

// FAIL-CLOSED : charge non finie ou négative ⇒ 0.
void test_E_charge_non_finie_ou_negative_rend_zero(void) {
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(NAN, 7), 4.33f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(INFINITY, 7), 4.33f));
  TEST_ASSERT_FLOAT_WITHIN(kEps, 0.0f, phFeedforwardLearnedFlow(obsOf(-2.0f, 7), 4.33f));
}

// CE QUE LA FONCTION NE DIT PAS : elle ne teste PAS la profondeur d'historique.
// Un appelant qui veut distinguer « 0,00 appris » de « rien d'appris » doit
// tester obs.days lui-même (c'est ce que fait le tick, garde kNoHistory, et
// l'accesseur de lecture, qui rend NAN sous 3 journées).
void test_E_la_profondeur_d_historique_n_est_PAS_testee_ici(void) {
  const float avec = phFeedforwardLearnedFlow(obsOf(1.41f, 7), 4.33f);
  const float sans = phFeedforwardLearnedFlow(obsOf(1.41f, 0), 4.33f);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kEps, avec, sans,
      "CONTRAT : phFeedforwardLearnedFlow ignore obs.days. Si ce test echoue, "
      "une garde d'historique a ete deplacee DANS le module pur — la garde du "
      "tick (kNoHistory) et le NAN de l'accesseur deviendraient redondants ou "
      "contradictoires.");
}

// =============================================================================
// F — A2 : STATUT RÉELLEMENT SERVI
// =============================================================================

static const uint8_t kInactive  = (uint8_t)PhFfStatus::kInactive;
static const uint8_t kActive    = (uint8_t)PhFfStatus::kActive;
static const uint8_t kHeld      = (uint8_t)PhFfStatus::kHeld;
static const uint8_t kNoHistory = (uint8_t)PhFfStatus::kNoHistory;
static const uint8_t kQuota     = (uint8_t)PhFfStatus::kQuota;

// 3 → 0 dès que l'anneau porte assez de journées : c'est le défaut du 03/09,
// où l'interface restait figée sur « en apprentissage » pendant sept heures,
// entre l'archivage de minuit et le premier tick du matin.
void test_F_statut_3_traduit_en_0_quand_l_historique_est_suffisant(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kInactive,
      phFeedforwardServedStatus(kNoHistory, /*sessionOpen*/ false,
                                /*historyDays*/ 5, /*learnedFinite*/ true),
      "TRADUCTION 3 -> 0 ABSENTE : l'instantane « historique insuffisant » est "
      "PERIME des que l'anneau porte assez de journees. Sans cette traduction, "
      "la carte affiche « en apprentissage » alors que la dose EST apprise "
      "(defaut du 03/09, sept heures d'affichage faux).");
}

// Frontière EXACTE du seuil de journées : >= et non >.
void test_F_frontiere_exacte_du_seuil_de_jours(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kInactive,
      phFeedforwardServedStatus(kNoHistory, false,
                                kPhIntegralRestoreMinHistoryDays, true),
      "FRONTIERE : days == kPhIntegralRestoreMinHistoryDays doit deja traduire "
      "3 -> 0 (comparaison >=). Avec un > strict, la journee qui vient d'etre "
      "archivee ne compte pas et l'affichage reste faux 24 h de plus.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kNoHistory,
      phFeedforwardServedStatus(kNoHistory, false,
                                (uint8_t)(kPhIntegralRestoreMinHistoryDays - 1),
                                true),
      "Sous le seuil, le 3 doit etre servi TEL QUEL : « en apprentissage » est "
      "alors la verite.");
}

void test_F_statut_3_conserve_sous_le_seuil(void) {
  TEST_ASSERT_EQUAL_UINT8(kNoHistory, phFeedforwardServedStatus(kNoHistory, false, 0, true));
  TEST_ASSERT_EQUAL_UINT8(kNoHistory, phFeedforwardServedStatus(kNoHistory, true,  1, true));
  TEST_ASSERT_EQUAL_UINT8(kNoHistory, phFeedforwardServedStatus(kNoHistory, false, 2, true));
}

// PRÉCÉDENCE VOULUE (validée par le visa) : le 3 est examiné AVANT le
// fail-closed `learnedFinite`. Le cas « historique insuffisant » est PRÉCISÉMENT
// celui où la dose apprise n'est pas disponible : si le fail-closed passait en
// premier, il effacerait le « en apprentissage » — la seule explication utile.
void test_F_precedence_le_3_est_examine_avant_le_fail_closed(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kNoHistory,
      phFeedforwardServedStatus(kNoHistory, false, /*days*/ 1,
                                /*learnedFinite*/ false),
      "PRECEDENCE INVERSEE : avec un historique court, la dose apprise est "
      "indisponible PAR DEFINITION. Le fail-closed learnedFinite ne doit pas "
      "effacer le statut « en apprentissage », qui porte la raison.");
  // Et si l'historique est redevenu suffisant, le 3 est traduit en 0 — quelle
  // que soit la disponibilité de la valeur.
  TEST_ASSERT_EQUAL_UINT8(kInactive,
      phFeedforwardServedStatus(kNoHistory, false, 5, false));
}

// 1 → 0 hors session : le feedforward ne VIT que sous une session automatique.
void test_F_statut_1_traduit_en_0_hors_session(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kInactive,
      phFeedforwardServedStatus(kActive, /*sessionOpen*/ false, 7, true),
      "TRADUCTION 1 -> 0 ABSENTE : « actif » hors session est un instantane "
      "PERIME (le FF ne vit que sous une session automatique).");
}

void test_F_statut_1_conserve_en_session(void) {
  TEST_ASSERT_EQUAL_UINT8(kActive, phFeedforwardServedStatus(kActive, true, 7, true));
}

// Fail-closed : « actif » avec une dose indisponible ferait afficher
// « appliquee » a cote d'un tiret.
void test_F_statut_1_en_session_mais_dose_indisponible_devient_0(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kInactive,
      phFeedforwardServedStatus(kActive, true, 7, /*learnedFinite*/ false),
      "FAIL-CLOSED : servir « actif » sans dose de base disponible afficherait "
      "« appliquee a la prochaine session » a cote d'un tiret.");
}

// 2 et 4 : JAMAIS traduits. Ils restent vrais entre deux ticks (un latch ne se
// leve qu'a l'ouverture de session suivante, un quota qu'au rollover de minuit)
// et ils portent la seule explication utile : POURQUOI le FF ne s'appliquera pas.
void test_F_statut_2_jamais_traduit(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kHeld,
      phFeedforwardServedStatus(kHeld, /*sessionOpen*/ false, /*days*/ 0,
                                /*learnedFinite*/ false),
      "STATUT 2 TRADUIT : le latch de securite reste VRAI entre deux ticks et "
      "porte la raison (« suspendue »). Le traduire effacerait l'explication.");
  TEST_ASSERT_EQUAL_UINT8(kHeld, phFeedforwardServedStatus(kHeld, true, 7, true));
  TEST_ASSERT_EQUAL_UINT8(kHeld, phFeedforwardServedStatus(kHeld, false, 7, true));
}

void test_F_statut_4_jamais_traduit(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kQuota,
      phFeedforwardServedStatus(kQuota, false, 0, false),
      "STATUT 4 TRADUIT : le gel quota reste VRAI jusqu'au rollover de minuit "
      "et porte la raison (« 60 % du quota atteint »).");
  TEST_ASSERT_EQUAL_UINT8(kQuota, phFeedforwardServedStatus(kQuota, true, 7, true));
  TEST_ASSERT_EQUAL_UINT8(kQuota, phFeedforwardServedStatus(kQuota, false, 7, true));
}

// 0 reste 0, et le fail-closed ne le change pas.
void test_F_statut_0_reste_0(void) {
  TEST_ASSERT_EQUAL_UINT8(kInactive, phFeedforwardServedStatus(kInactive, false, 7, true));
  TEST_ASSERT_EQUAL_UINT8(kInactive, phFeedforwardServedStatus(kInactive, true, 7, false));
}

// Contrat FIGÉ des codes : ils sont sérialisés vers /data, le WS et MQTT ; le
// client les traduit en libellés. Aucune renumérotation, aucune insertion.
void test_F_contrat_des_codes_fige(void) {
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)PhFfStatus::kInactive);
  TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)PhFfStatus::kActive);
  TEST_ASSERT_EQUAL_UINT8(2, (uint8_t)PhFfStatus::kHeld);
  TEST_ASSERT_EQUAL_UINT8(3, (uint8_t)PhFfStatus::kNoHistory);
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)PhFfStatus::kQuota);
  // Les régimes de composition sont un contrat INTERNE, jamais sérialisé — mais
  // leur ordre est lu par la coquille : on le verrouille aussi.
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)DoseFlowRegime::Full);
  TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)DoseFlowRegime::CappedToFeedforward);
  TEST_ASSERT_EQUAL_UINT8(2, (uint8_t)DoseFlowRegime::Zero);
}

// =============================================================================

int main(int, char**) {
  UNITY_BEGIN();

  // A — le contre-exemple, en premier
  RUN_TEST(test_A_contre_exemple_ff_141_p_moins_3_demande_vaut_141_et_non_zero);
  RUN_TEST(test_A_meme_couple_negatif_sans_feedforward_donne_zero);
  RUN_TEST(test_A_couple_negatif_malgre_integrale_positive_le_ff_reste_servi);

  // B — ordre interne
  RUN_TEST(test_B1_erreur_nan_rend_cinq_nan_et_pas_de_bande_morte);
  RUN_TEST(test_B1_erreur_infinie_rend_les_memes_nan);
  RUN_TEST(test_B3_p_et_i_ne_sont_jamais_bornes_par_l_amplitude);
  RUN_TEST(test_B3_i_negatif_est_servi_signe_et_non_borne);
  RUN_TEST(test_B6_demande_jamais_negative);

  // C — bande morte
  RUN_TEST(test_C_bande_morte_annule_p_ET_i);
  RUN_TEST(test_C_bande_morte_laisse_le_feedforward);
  RUN_TEST(test_C_frontiere_stricte_egalite_est_hors_bande_morte);
  RUN_TEST(test_C_frontiere_stricte_negative_egalite_est_hors_bande_morte);
  RUN_TEST(test_C_juste_en_deca_de_la_frontiere_est_dans_la_bande_morte);
  RUN_TEST(test_C_la_bande_morte_vient_de_l_appelant_jamais_recalculee);
  RUN_TEST(test_C_deadband_nulle_ne_met_rien_dans_la_bande);

  // D — plafonds, régimes, propriété
  RUN_TEST(test_D_plafond_du_pi_vaut_amplitude_moins_ff);
  RUN_TEST(test_D_sous_le_plafond_le_pi_servi_vaut_le_couple);
  RUN_TEST(test_D_ff_superieur_a_l_amplitude_donne_un_pi_servi_nul_jamais_negatif);
  RUN_TEST(test_D_amplitude_nulle_ne_sert_rien);
  RUN_TEST(test_D_regime_zero_coupe_le_ff_et_le_pi);
  RUN_TEST(test_D_regime_capped_annule_le_pi_et_sert_le_ff_seul);
  RUN_TEST(test_D_regime_capped_sans_ff_ne_dose_rien);
  RUN_TEST(test_D_propriete_demande_egale_pi_servi_plus_ff_sur_200_tirages);

  // E — A1
  RUN_TEST(test_E_charge_sous_le_plafond_est_servie_telle_quelle);
  RUN_TEST(test_E_charge_au_dela_du_plafond_est_ecretee_a_la_fraction);
  RUN_TEST(test_E_charge_egale_au_plafond);
  RUN_TEST(test_E_charge_nulle_rend_zero_et_pas_nan);
  RUN_TEST(test_E_soutenable_non_fini_ou_non_positif_rend_zero);
  RUN_TEST(test_E_charge_non_finie_ou_negative_rend_zero);
  RUN_TEST(test_E_la_profondeur_d_historique_n_est_PAS_testee_ici);

  // F — A2
  RUN_TEST(test_F_statut_3_traduit_en_0_quand_l_historique_est_suffisant);
  RUN_TEST(test_F_frontiere_exacte_du_seuil_de_jours);
  RUN_TEST(test_F_statut_3_conserve_sous_le_seuil);
  RUN_TEST(test_F_precedence_le_3_est_examine_avant_le_fail_closed);
  RUN_TEST(test_F_statut_1_traduit_en_0_hors_session);
  RUN_TEST(test_F_statut_1_conserve_en_session);
  RUN_TEST(test_F_statut_1_en_session_mais_dose_indisponible_devient_0);
  RUN_TEST(test_F_statut_2_jamais_traduit);
  RUN_TEST(test_F_statut_4_jamais_traduit);
  RUN_TEST(test_F_statut_0_reste_0);
  RUN_TEST(test_F_contrat_des_codes_fige);

  return UNITY_END();
}
