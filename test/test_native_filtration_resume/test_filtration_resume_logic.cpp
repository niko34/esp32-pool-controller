// =============================================================================
// Tests unitaires natifs — Reprise de la durée de filtration (feature-158)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// Couvre src/filtration_resume_logic.{h,cpp} (ADR-0017) :
//   - evaluateFiltrationResume() : UN SEUL chemin de dispense CONSOMMÉ, jamais
//     recalculé (frontière opposable, feature-158) ;
//   - filtrationRtcSeal/filtrationRtcIntact (patron OutageRtcRecord) ;
//   - les 5 mutants imposés par le gate pool-chemistry ;
//   - computeFiltrationElapsedMs() : arithmétique wrap-safe de
//     filtrationElapsedMs() (filtration.h), extraite ICI pour être exerçable
//     en natif (critère A5/B2) — nominal sans carry, carry sans repliement,
//     repliement de millis() à 2^32 sans carry, et avec carry additionné
//     après le wrap.
//
// Vecteurs et mutations imposés par le gate (voir la spec feature-158) :
//   carry = 599 s ⇒ report accordé (la garde pH, elle, refuse — testé côté
//     ph_tune_logic, hors périmètre de ce fichier) · 600 s ⇒ idem, accordé ·
//   coupure refusée (verdict.waive=false) ⇒ 0 · record corrompu ⇒ 0 ·
//   86 401 s ⇒ 0, PAS 86 400 (frontière de vraisemblance) ·
//   filtrationRunning=false ⇒ 0 · mode non managed ⇒ 0 ·
//   initialState=false ⇒ 0, même avec un record par ailleurs valide
//     (exclut le Boost par construction) · verdict absent (condition 13) ⇒ 0.
//
// Mutants à tuer :
//   M1 créditer outageS en plus · M2 écrêter au lieu de remettre à zéro ·
//   M3 supprimer le test filtrationRunning · M4 retirer le « une seule fois » ·
//   M5 frontière > → >=.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "filtration_resume_logic.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

// Construit un enregistrement SCELLÉ "propre" (filtration active, managée).
FiltrationRtcRecord makeSealedRecord(uint32_t carryS, bool running, InstallMode mode) {
  FiltrationRtcRecord r = {};
  r.carryS = carryS;
  r.filtrationRunning = running ? 1 : 0;
  r.installMode = static_cast<uint8_t>(mode);
  r.reserved[0] = 0;
  r.reserved[1] = 0;
  filtrationRtcSeal(r);
  return r;
}

OutageContinuityVerdict makeVerdict(bool waive, uint32_t outageS) {
  OutageContinuityVerdict v;
  v.waive = waive;
  v.outageS = outageS;
  v.deny = waive ? OutageDeny::kNone : OutageDeny::kTooLong;
  return v;
}

}  // namespace

// =============================================================================
// Patron OutageRtcRecord — scellement FNV-1a
// =============================================================================

void test_record_seal_et_intact(void) {
  FiltrationRtcRecord r = makeSealedRecord(120, true, InstallMode::ManagedFiltration);
  TEST_ASSERT_EQUAL_UINT32(kFiltrationRtcMagic, r.magic);
  TEST_ASSERT_TRUE(filtrationRtcIntact(r));

  r.carryS += 1;  // corruption : la donnée change, pas le CRC
  TEST_ASSERT_FALSE(filtrationRtcIntact(r));

  filtrationRtcSeal(r);
  TEST_ASSERT_TRUE(filtrationRtcIntact(r));
}

void test_record_taille_16_octets(void) {
  TEST_ASSERT_EQUAL_UINT32(16, (uint32_t)sizeof(FiltrationRtcRecord));
}

// =============================================================================
// Chemin nominal — report accordé
// =============================================================================

void test_report_accorde_599s(void) {
  FiltrationRtcRecord r = makeSealedRecord(599, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(599, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kResumed, out.motif);
  // Le report est un COMPTEUR, pas une dispense de la garde pH (testé côté
  // ph_tune_logic) : 599 s < kPhTuneMinFiltrationMs (600 s) continuerait de
  // refuser l'accord — ce fichier ne teste que la DONNÉE reportée.
  TEST_ASSERT_EQUAL_UINT32(9, out.coupureDureeS);
}

void test_report_accorde_600s(void) {
  FiltrationRtcRecord r = makeSealedRecord(600, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(600, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kResumed, out.motif);
}

// M1 — créditer outageS EN PLUS du carryS persisté est interdit (condition 1,
// imposée mot pour mot). Un mutant qui ajouterait v.outageS à out.carryS ferait
// échouer ce test (100 + 50 = 150 != 100).
void test_M1_la_coupure_nest_jamais_creditee(void) {
  FiltrationRtcRecord r = makeSealedRecord(100, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 50);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(100, out.carryS);
}

// =============================================================================
// Refus — chaque motif doit ramener carryS à ZÉRO
// =============================================================================

// Coupure refusée par measure_validity (verdict.waive == false) : UN SEUL
// chemin de dispense, consommé tel quel — aucune comparaison de durée ici.
void test_coupure_refusee_par_measure_validity(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(false, 200);  // kTooLong par ex.

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kDenied, out.motif);
}

void test_record_corrompu(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, true, InstallMode::ManagedFiltration);
  r.carryS += 1;  // casse le CRC sans resceller
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kNoRecord, out.motif);
}

void test_record_absent(void) {
  OutageContinuityVerdict v = makeVerdict(true, 9);
  FiltrationResumeInputs in;
  in.record = nullptr;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kNoRecord, out.motif);
}

// M2 — borne de vraisemblance : retour à ZÉRO, JAMAIS écrêtage à 86 400.
void test_M2_86401s_implausible_zero_pas_86400(void) {
  FiltrationRtcRecord r = makeSealedRecord(86401, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_FALSE(86400 == out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kImplausible, out.motif);
}

// M5 — la borne elle-même (86 400 s PILE) reste PLAUSIBLE : frontière stricte
// `>`, pas `>=`.
void test_M5_86400s_pile_reste_plausible(void) {
  FiltrationRtcRecord r = makeSealedRecord(86400, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(86400, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kResumed, out.motif);
}

// M3 — filtrationRunning doit être testé : un enregistrement "propre" mais
// filtrationRunning=false (pompe arrêtée avant la coupure) ne crédite rien.
void test_M3_filtrationRunning_false(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, /*running=*/false, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kNotRunning, out.motif);
}

// Condition 2 — preuve PROPRE à la filtration, jamais déduite d'un verdict de
// continuité : mode mémorisé non managé.
void test_mode_memorise_non_managed(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, true, InstallMode::PoweredByFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;  // courant managé, mémorisé non
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kNotManaged, out.motif);
}

// Condition 12 — modes Powered/External : AUCUN crédit, même avec un record
// par ailleurs valide et managé, si le mode COURANT a changé entre-temps.
void test_mode_courant_non_managed(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ExternalFiltration;  // mode courant changé
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kNotManaged, out.motif);
}

// M4 — consommation UNE SEULE FOIS : initialState=false (Boost, ou tout
// démarrage qui n'est pas le tout premier de l'uptime) ⇒ 0, MÊME avec un
// record et un verdict par ailleurs valides. C'est cette condition qui exclut
// le Boost par construction.
void test_M4_initialState_false_exclut_le_boost(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, true, InstallMode::ManagedFiltration);
  OutageContinuityVerdict v = makeVerdict(true, 9);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = &v;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = false;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kNotInitialRun, out.motif);
}

// Condition 13 — drapeau "verdict produit" : verdict absent (measure_validity
// pas encore passé par begin()) ⇒ refus bruyant côté coquille, et ICI 0.
void test_verdict_absent_condition_13(void) {
  FiltrationRtcRecord r = makeSealedRecord(300, true, InstallMode::ManagedFiltration);

  FiltrationResumeInputs in;
  in.record = &r;
  in.verdict = nullptr;
  in.installMode = InstallMode::ManagedFiltration;
  in.initialState = true;

  FiltrationResumeResult out = evaluateFiltrationResume(in);
  TEST_ASSERT_EQUAL_UINT32(0, out.carryS);
  TEST_ASSERT_EQUAL(FiltrationResumeMotif::kVerdictNotReady, out.motif);
}

// =============================================================================
// computeFiltrationElapsedMs — arithmétique WRAP-SAFE (A5/B2, feature-158)
// =============================================================================
// Extraite de filtration.h::filtrationElapsedMs() : c'est le calcul qui ne
// pouvait pas être exercé en natif tant qu'il vivait derrière <Arduino.h>.

// Cas nominal, sans report : aucun repliement, aucun carry.
void test_elapsed_nominal_sans_carry(void) {
  const uint32_t startedAtMs = 1000;
  const uint32_t nowMs = 4500;
  TEST_ASSERT_EQUAL_UINT32(3500, computeFiltrationElapsedMs(nowMs, startedAtMs, 0));
}

// carryMs additionné après un calcul SANS repliement : simple report, pas
// de débordement sur 32 bits (bien en-deçà de 0xFFFFFFFF).
void test_elapsed_carry_sans_wrap(void) {
  const uint32_t startedAtMs = 1000;
  const uint32_t nowMs = 4500;       // écoulé brut = 3500
  const uint32_t carryMs = 599000;   // 599 s reportées (feature-158)
  TEST_ASSERT_EQUAL_UINT32(3500 + 599000, computeFiltrationElapsedMs(nowMs, startedAtMs, carryMs));
}

// Repliement de millis() à 2^32 : startedAtMs proche de UINT32_MAX, nowMs
// après le wrap (reparti de 0). La soustraction non signée 32 bits doit
// retrouver la durée réelle sans passer par un type plus large.
void test_elapsed_repliement_millis_sans_carry(void) {
  const uint32_t startedAtMs = 0xFFFFFFF0UL;  // 16 ms avant le wrap
  const uint32_t nowMs = 100;                 // 100 ms après le wrap (donc 0 puis +100)
  // Écoulé réel = 16 ms (jusqu'au wrap) + 100 ms (après) = 116 ms.
  TEST_ASSERT_EQUAL_UINT32(116, computeFiltrationElapsedMs(nowMs, startedAtMs, 0));
}

// Même repliement, avec un carryMs non nul additionné APRÈS le wrap, sans
// débordement (bien en-deçà de 0xFFFFFFFF une fois additionné).
void test_elapsed_repliement_millis_avec_carry(void) {
  const uint32_t startedAtMs = 0xFFFFFFF0UL;  // 16 ms avant le wrap
  const uint32_t nowMs = 100;                 // 100 ms après le wrap
  const uint32_t carryMs = 59900;             // report d'un cycle précédent
  // 116 ms d'écoulé réel (cf. test précédent) + le report.
  TEST_ASSERT_EQUAL_UINT32(116 + 59900, computeFiltrationElapsedMs(nowMs, startedAtMs, carryMs));
}

// =============================================================================
// Libellés — non vides, utilisables tels quels dans le journal.
// =============================================================================
void test_motif_labels_non_vides(void) {
  const FiltrationResumeMotif all[] = {
      FiltrationResumeMotif::kNoRecord,       FiltrationResumeMotif::kNotRunning,
      FiltrationResumeMotif::kNotManaged,     FiltrationResumeMotif::kNotInitialRun,
      FiltrationResumeMotif::kVerdictNotReady, FiltrationResumeMotif::kDenied,
      FiltrationResumeMotif::kImplausible,    FiltrationResumeMotif::kResumed,
  };
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    const char* label = filtrationResumeMotifLabel(all[i]);
    TEST_ASSERT_NOT_NULL(label);
    TEST_ASSERT_TRUE(strlen(label) > 0);
  }
}

// =============================================================================
// Runner
// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_seal_et_intact);
  RUN_TEST(test_record_taille_16_octets);
  RUN_TEST(test_report_accorde_599s);
  RUN_TEST(test_report_accorde_600s);
  RUN_TEST(test_M1_la_coupure_nest_jamais_creditee);
  RUN_TEST(test_coupure_refusee_par_measure_validity);
  RUN_TEST(test_record_corrompu);
  RUN_TEST(test_record_absent);
  RUN_TEST(test_M2_86401s_implausible_zero_pas_86400);
  RUN_TEST(test_M5_86400s_pile_reste_plausible);
  RUN_TEST(test_M3_filtrationRunning_false);
  RUN_TEST(test_mode_memorise_non_managed);
  RUN_TEST(test_mode_courant_non_managed);
  RUN_TEST(test_M4_initialState_false_exclut_le_boost);
  RUN_TEST(test_verdict_absent_condition_13);
  RUN_TEST(test_elapsed_nominal_sans_carry);
  RUN_TEST(test_elapsed_carry_sans_wrap);
  RUN_TEST(test_elapsed_repliement_millis_sans_carry);
  RUN_TEST(test_elapsed_repliement_millis_avec_carry);
  RUN_TEST(test_motif_labels_non_vides);
  return UNITY_END();
}
