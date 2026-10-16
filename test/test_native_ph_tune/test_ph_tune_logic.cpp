// =============================================================================
// Tests unitaires natifs — Accord de la mesure pH sur un test manuel
// (src/ph_tune_logic.h, feature-155)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
// `evaluatePhTune()` est un module PUR (ADR-0017) : header-only, pas
// d'Arduino.h, pas de FreeRTOS — il est inclus directement, pas de .cpp à
// ajouter à build_src_filter.
//
// COUVERTURE EXIGÉE PAR LE GATE pool-chemistry (voir spec feature-155) :
//
//   🔴 T1 — ⚠️ CHANGEMENT DE NATURE (feature-168) : écart +0,80 ⇒ ACCEPTÉ
//        TEL QUEL (None), PLUS un écrêtage à +0,50 — la borne de réglage
//        ±0,50 a disparu (condition C3 du gate, isPhOffsetValid() ne refuse
//        plus que NaN/infini/|écart| > kPhOffsetScaleSpan = 14). Ce test
//        reste le plus important de la suite : il verrouille le fait que
//        l'écart DEMANDÉ (phReference - phValueRaw) est bien ce qui est
//        appliqué, SANS AUCUN écrêtage résiduel, même au-delà de l'ancienne
//        borne. roundPhOffsetToStep() n'arrondit plus QUE — elle ne clampe
//        plus (ph_guard_logic.h) : si un clamp était réintroduit, offsetApplied
//        redeviendrait figé à ±0,50 malgré un écart plus grand — exactement la
//        mutation (a) de la spec.
//
//   T2 — ⚠️ CHANGEMENT DE NATURE (feature-168) : ±0,50 et ±0,51 sont
//        désormais des écarts ORDINAIRES, tous deux ACCEPTÉS (plus de borne à
//        cette valeur) — seul le pas d'arrondi (0,10) les distingue encore.
//   T3 — filtration 9 min 59 (refusée) vs 10 min 01 (acceptée),
//        kPhTuneMinFiltrationMs = 600 000 ms.
//   T4 — filtration ARRÊTÉE (filtrationRunning=false) ⇒ refus, quel que soit
//        filtrationElapsedMs passé (remis à 0 en interne : régime hydraulique
//        INEXISTANT, pas juste récent).
//   T5 — phValueRaw NaN ⇒ refus MeasureUnavailable.
//   T6 — calPoints 1 (refusé) vs 2 (accepté).
//   T7 — ORDRE DES REFUS : un cas qui viole PLUSIEURS conditions à la fois
//        rend le PREMIER refus de l'ordre imposé (NaN > filtration >
//        calPoints > bornes), jamais un autre.
//   T8 — référence 7,45 / lu 7,13 ⇒ offset DEMANDÉ 0,32, ARRONDI à +0,30,
//        valeur atteinte 7,43 (PAS 7,45 — Piège 1).
// =============================================================================

#include <unity.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)

#include "ph_tune_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Jeu de paramètres NOMINAUX (tout est accordé, sauf le champ qu'on force à
// violer dans chaque test) : filtration établie depuis largement plus que
// kPhTuneMinFiltrationMs, sonde calibrée à 2 points, mesure présente.
static const float kNominalPhValueRaw = 7.00f;
static const unsigned long kNominalElapsedMs = kPhTuneMinFiltrationMs + 60000UL;  // 11 min
static const int kNominalCalPoints = (int)kPhTuneCalPointsMin;  // 2

// =============================================================================
// T1 — 🔴 LE VECTEUR LE PLUS IMPORTANT : écart +0,80 ⇒ ACCEPTÉ TEL QUEL
// (feature-168), PAS un écrêtage silencieux à +0,50 — la borne de réglage a
// disparu, et roundPhOffsetToStep() n'arrondit plus QUE (son clamp a été
// retiré, voir ph_guard_logic.h). On vérifie que l'écart DEMANDÉ (0,80, un
// multiple exact du pas 0,10) ressort INTACT, et pas figé à l'ancienne borne.
// =============================================================================
void test_ecart_080_accepte_sans_ecretage(void) {
  const float phReference = kNominalPhValueRaw + 0.80f;  // 7.80
  PhTuneResult r = evaluatePhTune(phReference, kNominalPhValueRaw, true,
                                  kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  // Non-régression de la mutation (a) de la spec : si un futur commit
  // réintroduit l'écrêtage dans roundPhOffsetToStep(), offsetApplied
  // redeviendrait figé à +0,50 au lieu de +0,80.
  TEST_ASSERT_EQUAL_FLOAT(0.80f, r.offsetApplied);
}

// =============================================================================
// T2 — ±0,50 et ±0,51 sont désormais des écarts ORDINAIRES, tous ACCEPTÉS.
// Seul le pas d'arrondi (0,10) les distingue : 0,50 est un multiple exact
// (ressort inchangé), 0,51 est arrondi au plus proche multiple (0,50).
// =============================================================================
void test_borne_exacte_plus_050_acceptee(void) {
  const float phReference = kNominalPhValueRaw + 0.50f;
  PhTuneResult r = evaluatePhTune(phReference, kNominalPhValueRaw, true,
                                  kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  TEST_ASSERT_EQUAL_FLOAT(0.50f, r.offsetApplied);
}

void test_borne_exacte_moins_050_acceptee(void) {
  const float phReference = kNominalPhValueRaw - 0.50f;
  PhTuneResult r = evaluatePhTune(phReference, kNominalPhValueRaw, true,
                                  kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  TEST_ASSERT_EQUAL_FLOAT(-0.50f, r.offsetApplied);
}

void test_ecart_051_accepte_arrondi_050(void) {
  const float phReference = kNominalPhValueRaw + 0.51f;
  PhTuneResult r = evaluatePhTune(phReference, kNominalPhValueRaw, true,
                                  kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  TEST_ASSERT_EQUAL_FLOAT(0.50f, r.offsetApplied);
}

void test_ecart_moins_051_accepte_arrondi_moins_050(void) {
  const float phReference = kNominalPhValueRaw - 0.51f;
  PhTuneResult r = evaluatePhTune(phReference, kNominalPhValueRaw, true,
                                  kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  TEST_ASSERT_EQUAL_FLOAT(-0.50f, r.offsetApplied);
}

// =============================================================================
// T1bis — SALUBRITÉ DE DOMAINE (feature-168, C3/C4) : le SEUL refus OutOfRange
// encore atteignable porte sur NaN/infini/|écart| > kPhOffsetScaleSpan (14,0,
// l'échelle pH entière) — plus jamais sur une amplitude "inhabituelle".
// =============================================================================
void test_ecart_hors_echelle_refuse_out_of_range(void) {
  // Écart de 14,1 (au-delà de kPhOffsetScaleSpan) : refusé.
  PhTuneResult r1 = evaluatePhTune(kNominalPhValueRaw + 14.1f, kNominalPhValueRaw,
                                   true, kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::OutOfRange, (int)r1.refusal);

  // Écart de 13,9 (juste EN DEÇÀ de l'échelle) : accepté.
  PhTuneResult r2 = evaluatePhTune(kNominalPhValueRaw + 13.9f, kNominalPhValueRaw,
                                   true, kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r2.refusal);

  // phReference lui-même NaN (test de référence jamais relevé proprement) :
  // l'écart demandé devient NaN, isPhOffsetValid(NaN) => false => OutOfRange.
  PhTuneResult r3 = evaluatePhTune(NAN, kNominalPhValueRaw, true,
                                   kNominalElapsedMs, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::OutOfRange, (int)r3.refusal);
}

// =============================================================================
// T3 — filtration 9 min 59 s (599 000 ms) refusée, 10 min 01 s (601 000 ms)
// acceptée. kPhTuneMinFiltrationMs = 600 000 ms (constants.h).
// =============================================================================
void test_filtration_9min59_refusee(void) {
  const unsigned long elapsed = 599000UL;  // 9 min 59 s < 600 000 ms
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 0.10f, kNominalPhValueRaw,
                                  true, elapsed, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::FiltrationNotEstablished, (int)r.refusal);
  TEST_ASSERT_EQUAL_UINT32(1000UL, r.remainingFiltrationMs);
}

void test_filtration_10min01_acceptee(void) {
  const unsigned long elapsed = 601000UL;  // 10 min 01 s > 600 000 ms
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 0.10f, kNominalPhValueRaw,
                                  true, elapsed, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
}

// =============================================================================
// T4 — filtration ARRÊTÉE : refusée même avec un filtrationElapsedMs
// (rémanent) très au-delà du plancher — le régime hydraulique est INEXISTANT,
// pas juste récent. Le reste annoncé est alors le délai PLEIN.
// =============================================================================
void test_filtration_arretee_refusee_malgre_elapsed_eleve(void) {
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 0.10f, kNominalPhValueRaw,
                                  false, 999999UL, kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::FiltrationNotEstablished, (int)r.refusal);
  TEST_ASSERT_EQUAL_UINT32(kPhTuneMinFiltrationMs, r.remainingFiltrationMs);
}

// =============================================================================
// T5 — mesure de référence indisponible (phValueRaw NaN).
// =============================================================================
void test_ph_value_raw_nan_refusee(void) {
  PhTuneResult r = evaluatePhTune(7.40f, NAN, true, kNominalElapsedMs,
                                  kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::MeasureUnavailable, (int)r.refusal);
}

// =============================================================================
// T6 — sonde non étalonnée : calPoints 1 refusé, 2 accepté.
// =============================================================================
void test_cal_points_1_refusee(void) {
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 0.10f, kNominalPhValueRaw,
                                  true, kNominalElapsedMs, 1);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::ProbeNotCalibrated, (int)r.refusal);
}

void test_cal_points_2_acceptee(void) {
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 0.10f, kNominalPhValueRaw,
                                  true, kNominalElapsedMs, 2);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
}

// =============================================================================
// T7 — ORDRE DES REFUS : un cas violant PLUSIEURS conditions à la fois doit
// rendre le PREMIER refus de l'ordre imposé : 1=NaN, 2=filtration, 3=calPoints,
// 4=bornes. NE PAS RÉORDONNER evaluatePhTune() sans mettre ces tests à jour.
// =============================================================================

// Viole les 4 conditions à la fois (NaN + filtration arrêtée + calPoints=0 +
// écart hors bornes) ⇒ doit rendre MeasureUnavailable (1er de l'ordre).
void test_ordre_nan_gagne_sur_tout(void) {
  PhTuneResult r = evaluatePhTune(99.0f, NAN, false, 0UL, 0);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::MeasureUnavailable, (int)r.refusal);
}

// Mesure PRÉSENTE mais filtration arrêtée + calPoints=0 + écart hors bornes
// ⇒ doit rendre FiltrationNotEstablished (2e de l'ordre), pas ProbeNotCalibrated
// ni OutOfRange.
void test_ordre_filtration_gagne_sur_calpoints_et_bornes(void) {
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 99.0f, kNominalPhValueRaw,
                                  false, 999999UL, 0);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::FiltrationNotEstablished, (int)r.refusal);
}

// Mesure présente, filtration établie, mais calPoints=0 ET écart hors bornes
// ⇒ doit rendre ProbeNotCalibrated (3e de l'ordre), pas OutOfRange.
void test_ordre_calpoints_gagne_sur_bornes(void) {
  PhTuneResult r = evaluatePhTune(kNominalPhValueRaw + 99.0f, kNominalPhValueRaw,
                                  true, kNominalElapsedMs, 0);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::ProbeNotCalibrated, (int)r.refusal);
}

// =============================================================================
// T8 — cas imposé par le gate : référence 7,45 / lu 7,13 ⇒ écart DEMANDÉ 0,32,
// ARRONDI à +0,30 (kPhOffsetStep = 0,10), valeur atteinte 7,43 — PAS 7,45
// (Piège 1 : l'écran doit annoncer ce que l'appareil va VRAIMENT atteindre).
// =============================================================================
void test_reference_745_lu_713_offset_030_atteinte_743(void) {
  PhTuneResult r = evaluatePhTune(7.45f, 7.13f, true, kNominalElapsedMs,
                                  kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  TEST_ASSERT_EQUAL_FLOAT(0.30f, r.offsetApplied);
  const float valeurAtteinte = 7.13f + r.offsetApplied;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.43f, valeurAtteinte);
}

// =============================================================================
// Vecteur Piège 2 — le calcul reste juste avec un OFFSET DÉJÀ NON NUL. Cette
// fonction ne reçoit QUE des valeurs brutes (phReference "amenée" et
// phValueRaw brut) : rien ici ne dépend d'un offset courant, mais ce test
// documente explicitement que phValueRaw doit rester la valeur BRUTE même
// quand un offset est déjà appliqué ailleurs dans la chaîne (l'appelant, pas
// cette fonction, est responsable de ne JAMAIS passer ph/phFiltered/phMedian).
// =============================================================================
void test_offset_deja_non_nul_ne_change_pas_le_calcul(void) {
  // Offset courant fictif +0,30 déjà appliqué ailleurs : phValueRaw reste la
  // valeur BRUTE (7,13), pas la valeur déjà corrigée (7,43) que l'appelant
  // NE DOIT PAS transmettre ici (cf. ph_tune_logic.h, Piège 2).
  PhTuneResult r = evaluatePhTune(7.45f, 7.13f, true, kNominalElapsedMs,
                                  kNominalCalPoints);
  TEST_ASSERT_EQUAL((int)PhTuneRefusal::None, (int)r.refusal);
  TEST_ASSERT_EQUAL_FLOAT(0.30f, r.offsetApplied);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_ecart_080_accepte_sans_ecretage);
  RUN_TEST(test_borne_exacte_plus_050_acceptee);
  RUN_TEST(test_borne_exacte_moins_050_acceptee);
  RUN_TEST(test_ecart_051_accepte_arrondi_050);
  RUN_TEST(test_ecart_moins_051_accepte_arrondi_moins_050);
  RUN_TEST(test_ecart_hors_echelle_refuse_out_of_range);
  RUN_TEST(test_filtration_9min59_refusee);
  RUN_TEST(test_filtration_10min01_acceptee);
  RUN_TEST(test_filtration_arretee_refusee_malgre_elapsed_eleve);
  RUN_TEST(test_ph_value_raw_nan_refusee);
  RUN_TEST(test_cal_points_1_refusee);
  RUN_TEST(test_cal_points_2_acceptee);
  RUN_TEST(test_ordre_nan_gagne_sur_tout);
  RUN_TEST(test_ordre_filtration_gagne_sur_calpoints_et_bornes);
  RUN_TEST(test_ordre_calpoints_gagne_sur_bornes);
  RUN_TEST(test_reference_745_lu_713_offset_030_atteinte_743);
  RUN_TEST(test_offset_deja_non_nul_ne_change_pas_le_calcul);
  return UNITY_END();
}
