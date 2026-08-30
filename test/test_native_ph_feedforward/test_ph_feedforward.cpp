// =============================================================================
// Tests unitaires natifs — FEEDFORWARD pH (feature-085, v2.28.0)
// =============================================================================
// Cible : src/ph_feedforward_logic.h, module PUR (ADR-0017), plus le couplage
// anti-windup dans src/ph_integral_logic.h (PhIntegralInputs::ffMlPerMin).
// Tournent sur PC (env:native, Unity), hors ESP32 / Arduino.
//
// L'ENJEU, en une phrase : le FF est la seule autorité de dosage AVEUGLE à
// l'erreur mesurée — chaque garde qui tombe se paie en produit versé sans que
// personne ne puisse le constater. Les trois conditions bloquantes du gate
// (anti-windup sur « soutenable − FF », gel quota à 60 %, latch en erreur
// SIGNÉE tenue 30 min) sont couvertes condition par condition, frontières
// comprises, et validées par mutation (cf. rapport de test).
//
// ORGANISATION
//   A — calcul du débit : min(charge, 60 % du soutenable), fail-closed
//   B — historique minimal (kPhIntegralRestoreMinHistoryDays)
//   C — gel quota journalier (condition bloquante n°2)
//   D — latch « erreur défavorable » tenue 30 min (condition n°3), wrap millis
//   E — latchs mesure invalide / contexte / changement de produit
//   F — réarmement au front d'ouverture de session
//   G — anti-windup via phIntegralTick (condition n°1)
//   H — reboot sans rattrapage (A7) et stabilité 14 jours (A8)
//   I — contrat figé des statuts et des constantes
//
// CE QUI RESTE HORS D'ATTEINTE (dit sans détour) : le câblage de
// pump_controller.cpp — fourniture de l'erreur signée, de la validité de
// mesure, du hash de contexte, de l'état de session, addition du FF au débit
// PID et journalisation. Couvert par les scénarios d'intégration manuels.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <string.h>  // memset

#include "constants.h"
#include "ph_feedforward_logic.h"
#include "ph_integral_logic.h"
#include "ph_persist_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Décor commun — installation de référence : amplitude 26 mL/min, 5 min/h,
// soit un soutenable de 2,1667 mL/min ; plafond FF = 60 % = 1,30 mL/min.
// -----------------------------------------------------------------------------
static const float kSust = 26.0f * 5.0f / 60.0f;  // phSustainableAvgFlow(26, 5)
static const float kCap  = kPhFfMaxSustainableFraction * kSust;

static PhPersistLoadObservation obsOf(float loadMlPerMin, uint8_t days) {
  PhPersistLoadObservation o;
  o.loadMlPerMin = loadMlPerMin;
  o.days = days;
  return o;
}

// Jeu d'arguments par défaut : session ouverte, mesure valide, historique
// plein, quota désactivé — le tick rend kActive avec ff = charge observée.
struct FfArgs {
  float    signedError       = 0.10f;  // positif : il faut doser
  bool     measureValid      = true;
  uint32_t ctxHash           = 0xC0FFEEu;
  float    load              = 1.0f;   // < kCap : la charge gouverne
  uint8_t  days              = 7;
  float    sustainable       = kSust;
  bool     sessionOpen       = true;
  uint32_t nowMs             = 1000000u;
  float    dailyInjectedMl   = 0.0f;
  float    maxDailyMl        = 0.0f;   // ≤ 0 : limite désactivée
  bool     productChangeHold = false;
};

static PhFfVerdict tick(PhFfState& st, const FfArgs& a) {
  return phFeedforwardTick(st, a.signedError, a.measureValid, a.ctxHash,
                           obsOf(a.load, a.days), a.sustainable, a.sessionOpen,
                           a.nowMs, a.dailyInjectedMl, a.maxDailyMl,
                           a.productChangeHold);
}

// =============================================================================
// A — CALCUL DU DÉBIT
// =============================================================================

static void test_A_ff_egale_la_charge_observee_sous_le_plafond(void) {
  PhFfState st;
  FfArgs a;  // load = 1,0 < cap = 1,30
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, v.ffMlPerMin);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
}

static void test_A_charge_au_dessus_du_plafond_est_ecretee_exactement(void) {
  PhFfState st;
  FfArgs a;
  a.load = 2.86f;  // charge de référence, > 1,30
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  // Écrêtage EXACT à kPhFfMaxSustainableFraction × soutenable, pas « environ ».
  TEST_ASSERT_EQUAL_FLOAT(kCap, v.ffMlPerMin);
  TEST_ASSERT_TRUE(v.ffMlPerMin < a.load);
}

static void test_A_charge_nulle_est_une_observation_legitime_ff_nul(void) {
  PhFfState st;
  FfArgs a;
  a.load = 0.0f;
  PhFfVerdict v = tick(st, a);
  // Statut ACTIF (le FF vit), débit 0,00 : charge observée nulle, pas un refus.
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

static void test_A_soutenable_nul_ou_non_fini_ferme_le_ff(void) {
  {  // pompe non caractérisée
    PhFfState st;
    FfArgs a;
    a.sustainable = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, tick(st, a).ffMlPerMin);
  }
  {  // soutenable négatif
    PhFfState st;
    FfArgs a;
    a.sustainable = -1.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, tick(st, a).ffMlPerMin);
  }
  {  // soutenable NaN
    PhFfState st;
    FfArgs a;
    a.sustainable = NAN;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, tick(st, a).ffMlPerMin);
  }
}

static void test_A_charge_non_finie_ou_negative_ferme_le_ff(void) {
  {
    PhFfState st;
    FfArgs a;
    a.load = NAN;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, tick(st, a).ffMlPerMin);
  }
  {
    PhFfState st;
    FfArgs a;
    a.load = -0.5f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, tick(st, a).ffMlPerMin);
  }
}

static void test_A_hors_session_le_ff_ne_vit_pas(void) {
  PhFfState st;
  FfArgs a;
  a.sessionOpen = false;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kInactive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

// =============================================================================
// B — HISTORIQUE MINIMAL (fail-closed : un appareil neuf ne dose rien « de mémoire »)
// =============================================================================

static void test_B_moins_de_trois_jours_ff_nul_statut_3(void) {
  for (uint8_t days = 0; days < kPhIntegralRestoreMinHistoryDays; ++days) {
    PhFfState st;
    FfArgs a;
    a.days = days;
    a.load = 2.86f;  // une charge bien réelle ne change rien : la médiane ne fait pas foi
    PhFfVerdict v = tick(st, a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kNoHistory, (uint8_t)v.status);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
  }
}

static void test_B_frontiere_exacte_trois_jours_ouvrent_le_ff(void) {
  PhFfState st;
  FfArgs a;
  a.days = kPhIntegralRestoreMinHistoryDays;  // 3 : première valeur exploitable
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, v.ffMlPerMin);
}

// =============================================================================
// C — GEL QUOTA JOURNALIER (condition bloquante n°2 du gate)
// =============================================================================

static void test_C_quota_a_60_pourcent_gele_le_ff(void) {
  PhFfState st;
  FfArgs a;
  a.maxDailyMl = 300.0f;
  a.dailyInjectedMl = 200.0f;  // 66 % > 60 %
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kQuota, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

static void test_C_frontiere_exacte_du_quota(void) {
  {  // exactement 60 % : ≥ ⇒ gelé
    PhFfState st;
    FfArgs a;
    a.maxDailyMl = 300.0f;
    a.dailyInjectedMl = kPhFfDailyQuotaFraction * 300.0f;  // 180,0
    PhFfVerdict v = tick(st, a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kQuota, (uint8_t)v.status);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
  }
  {  // juste dessous : le FF vit encore
    PhFfState st;
    FfArgs a;
    a.maxDailyMl = 300.0f;
    a.dailyInjectedMl = 179.9f;
    PhFfVerdict v = tick(st, a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.ffMlPerMin);
  }
}

static void test_C_limite_desactivee_pas_de_gel_quota(void) {
  PhFfState st;
  FfArgs a;
  a.maxDailyMl = 0.0f;             // 0 = désactivée par l'utilisateur
  a.dailyInjectedMl = 100000.0f;   // même un volume énorme ne gèle pas
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, v.ffMlPerMin);
}

static void test_C_quota_est_rendu_meme_hors_session(void) {
  // Observabilité : l'UI peut dire POURQUOI le FF ne s'appliquera pas, avant
  // même qu'une session s'ouvre (le quota prime sur kInactive).
  PhFfState st;
  FfArgs a;
  a.sessionOpen = false;
  a.maxDailyMl = 300.0f;
  a.dailyInjectedMl = 250.0f;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kQuota, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

// =============================================================================
// D — LATCH « ERREUR DÉFAVORABLE » (condition bloquante n°3, erreur SIGNÉE)
// =============================================================================

// Fait mûrir la tenue : `count` ticks de `stepMs` en erreur `err`, à partir de
// `*tMs` (avancé en place). Retourne le dernier verdict.
static PhFfVerdict holdAdverse(PhFfState& st, uint32_t* tMs, float err,
                               uint32_t stepMs, uint32_t count) {
  PhFfVerdict v;
  FfArgs a;
  a.signedError = err;
  for (uint32_t i = 0; i < count; ++i) {
    *tMs += stepMs;
    a.nowMs = *tMs;
    v = tick(st, a);
  }
  return v;
}

static void test_D_latch_apres_30_minutes_tenues(void) {
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);  // premier tick : arme la base de temps, n'accumule pas
  // 179 ticks de 10 s = 29 min 50 s : toujours pas de latch
  PhFfVerdict v = holdAdverse(st, &t, -0.06f, 10000u, 179u);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  // 180e tick : 1 800 000 ms tenues ⇒ latch, FF nul
  v = holdAdverse(st, &t, -0.06f, 10000u, 1u);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kAdverseError, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

static void test_D_29_minutes_59_secondes_ne_suffisent_pas(void) {
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);
  // 179 × 10 s + 1 × 9 s = 1 799 000 ms = 29 min 59 s : PAS de latch
  holdAdverse(st, &t, -0.06f, 10000u, 179u);
  PhFfVerdict v = holdAdverse(st, &t, -0.06f, 9000u, 1u);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  // ... et la seconde manquante déclenche
  v = holdAdverse(st, &t, -0.06f, 1000u, 1u);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kAdverseError, (uint8_t)v.latched);
}

static void test_D_erreur_a_moins_004_ne_latch_jamais(void) {
  // −0,04 est DANS la bande tolérée (seuil −0,05 strict) : deux heures tenues
  // ne posent rien.
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.04f;
  arm.nowMs = t;
  tick(st, arm);
  PhFfVerdict v = holdAdverse(st, &t, -0.04f, 10000u, 720u);  // 2 h
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
}

static void test_D_frontiere_moins_005_exactement_ne_compte_pas(void) {
  // Le déclencheur est STRICT (« < −0,05 ») : −0,05 pile n'accumule pas — et
  // REMET même la tenue à zéro (remontée constatée).
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);
  holdAdverse(st, &t, -0.06f, 10000u, 120u);        // 20 min d'excursion franche
  holdAdverse(st, &t, -0.05f, 10000u, 1u);          // −0,05 pile : tenue effacée
  PhFfVerdict v = holdAdverse(st, &t, -0.06f, 10000u, 179u);  // 29 min 50 s
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  v = holdAdverse(st, &t, -0.06f, 10000u, 1u);      // 30 min PLEINES depuis le reset
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kAdverseError, (uint8_t)v.latched);
}

static void test_D_remontee_avant_30_min_remet_la_tenue_a_zero(void) {
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);
  holdAdverse(st, &t, -0.06f, 10000u, 170u);   // 28 min 20 s d'excursion
  holdAdverse(st, &t, 0.10f, 10000u, 1u);      // le pH remonte : un seul tick suffit
  // Une nouvelle excursion doit refaire ses 30 minutes ENTIÈRES
  PhFfVerdict v = holdAdverse(st, &t, -0.06f, 10000u, 179u);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  v = holdAdverse(st, &t, -0.06f, 10000u, 1u);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kAdverseError, (uint8_t)v.latched);
}

static void test_D_trou_d_evaluation_ne_fait_pas_murir_la_tenue(void) {
  // ADR-0049 : un dt aberrant (> kPhIntegralMaxTickDtMs) est REJETÉ, jamais
  // crédité. Une heure de filtration coupée en pleine excursion ne compte pas.
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);
  holdAdverse(st, &t, -0.06f, 10000u, 100u);     // 16 min 40 s observées
  PhFfVerdict v = holdAdverse(st, &t, -0.06f, 3600000u, 1u);  // trou d'UNE HEURE
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  // La tenue reprend à 16 min 40 s (gelée, pas remise à zéro) : il manque
  // 13 min 20 s exactement.
  v = holdAdverse(st, &t, -0.06f, 10000u, 79u);  // 29 min 50 s cumulées
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  v = holdAdverse(st, &t, -0.06f, 10000u, 1u);   // 30 min cumulées
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kAdverseError, (uint8_t)v.latched);
}

static void test_D_wrap_de_millis_pendant_la_tenue(void) {
  // Piège avéré du projet (repliement à 49,7 j, ADR-0049) : la tenue de 30 min
  // doit mûrir NORMALEMENT à cheval sur le wrap de millis() — ni latch précoce
  // (dt géant), ni tenue perdue.
  PhFfState st;
  uint32_t t = 0xFFFFFFFFu - 900000u;  // 15 min avant le repliement
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);
  // 179 ticks de 10 s : traverse 0xFFFFFFFF → 0x00000000 vers 15 min APRÈS
  PhFfVerdict v = holdAdverse(st, &t, -0.06f, 10000u, 179u);
  TEST_ASSERT_TRUE(t < 900000u + 100000u);  // preuve : on a bien wrapé
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  v = holdAdverse(st, &t, -0.06f, 10000u, 1u);  // 30e minute, de l'autre côté
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kAdverseError, (uint8_t)v.latched);
}

// =============================================================================
// E — LATCHS MESURE INVALIDE / CONTEXTE / CHANGEMENT DE PRODUIT
// =============================================================================

static void test_E_mesure_invalide_latch_immediat_sans_tenue(void) {
  // Fail-closed IMMÉDIAT (comportement lu dans le code, étape 3c) : sans mesure
  // digne de confiance, personne ne peut constater que le FF dose à tort.
  PhFfState st;
  FfArgs a;
  a.measureValid = false;
  PhFfVerdict v = tick(st, a);  // PREMIER tick : pas besoin de tenue
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kMeasureInvalid, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

static void test_E_mesure_invalide_efface_la_tenue_d_erreur(void) {
  // L'erreur n'a plus de sens ⇒ la tenue non plus : après réarmement, une
  // nouvelle excursion doit refaire ses 30 minutes entières.
  PhFfState st;
  uint32_t t = 5000u;
  FfArgs arm;
  arm.signedError = -0.06f;
  arm.nowMs = t;
  tick(st, arm);
  holdAdverse(st, &t, -0.06f, 10000u, 170u);  // 28 min 20 s
  FfArgs bad;
  bad.measureValid = false;
  t += 10000u;
  bad.nowMs = t;
  tick(st, bad);                              // latch mesure + tenue effacée
  TEST_ASSERT_EQUAL_UINT32(0u, st.adverseHeldMs);
}

static void test_E_changement_de_contexte_latch(void) {
  PhFfState st;
  FfArgs a;
  a.ctxHash = 0x1111u;
  a.nowMs = 5000u;
  tick(st, a);            // arme la référence
  a.nowMs = 10000u;
  PhFfVerdict v = tick(st, a);  // même contexte : rien
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  a.ctxHash = 0x2222u;    // produit / consigne / vitesse / plafond changé
  a.nowMs = 15000u;
  v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kContext, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

static void test_E_premier_tick_arme_la_reference_sans_latcher(void) {
  // Détection par FRONT : au premier tick, lastCtxHash (0) ≠ hash courant, et
  // pourtant AUCUN latch — la référence n'était pas armée.
  PhFfState st;
  FfArgs a;
  a.ctxHash = 0xDEADBEEFu;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kNone, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
}

static void test_E_product_change_hold_annule_le_ff_quel_que_soit_le_reste(void) {
  // Historique plein, session ouverte, mesure valide, quota loin : la tenue
  // produit prime quand même — l'anneau contient des mL de l'AUTRE produit.
  PhFfState st;
  FfArgs a;
  a.productChangeHold = true;
  a.load = 2.86f;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kProductChange, (uint8_t)v.latched);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
}

static void test_E_le_latch_prime_sur_l_historique_et_le_quota(void) {
  // Priorité des statuts : held (2) > noHistory (3) > quota (4).
  {
    PhFfState st;
    FfArgs a;
    a.measureValid = false;
    a.days = 1;  // historique court ET mesure invalide ⇒ c'est le latch qui parle
    PhFfVerdict v = tick(st, a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  }
  {
    PhFfState st;
    FfArgs a;
    a.days = 2;
    a.maxDailyMl = 300.0f;
    a.dailyInjectedMl = 299.0f;  // quota dépassé ET historique court ⇒ noHistory
    PhFfVerdict v = tick(st, a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kNoHistory, (uint8_t)v.status);
  }
}

// =============================================================================
// F — RÉARMEMENT AU FRONT D'OUVERTURE DE SESSION
// =============================================================================

// Pose un latch de contexte sur un état frais, session OUVERTE. Laisse st held.
static uint32_t latchByContext(PhFfState& st, uint32_t t0) {
  FfArgs a;
  a.ctxHash = 0x1111u;
  a.nowMs = t0;
  tick(st, a);
  a.ctxHash = 0x2222u;
  a.nowMs = t0 + 5000u;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfLatchCause::kContext, (uint8_t)v.latched);
  return t0 + 5000u;
}

static void test_F_latch_tenu_tant_que_la_session_reste_ouverte(void) {
  PhFfState st;
  uint32_t t = latchByContext(st, 5000u);
  FfArgs a;
  a.ctxHash = 0x2222u;
  for (int i = 0; i < 10; ++i) {
    t += 5000u;
    a.nowMs = t;
    PhFfVerdict v = tick(st, a);  // session toujours ouverte : pas de front
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
    TEST_ASSERT_FALSE(v.released);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.ffMlPerMin);
  }
}

static void test_F_le_latch_se_leve_au_front_d_ouverture_suivant(void) {
  PhFfState st;
  uint32_t t = latchByContext(st, 5000u);
  FfArgs a;
  a.ctxHash = 0x2222u;
  a.sessionOpen = false;  // la session se ferme...
  t += 5000u;
  a.nowMs = t;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  TEST_ASSERT_FALSE(v.released);
  a.sessionOpen = true;   // ... et se rouvre : FRONT ⇒ réarmement
  t += 5000u;
  a.nowMs = t;
  v = tick(st, a);
  TEST_ASSERT_TRUE(v.released);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, v.ffMlPerMin);  // le FF revit aussitôt
}

static void test_F_la_tenue_produit_prime_sur_le_rearmement(void) {
  PhFfState st;
  FfArgs a;
  a.productChangeHold = true;
  a.nowMs = 5000u;
  tick(st, a);            // latch produit posé
  a.sessionOpen = false;  // fermeture...
  a.nowMs = 10000u;
  tick(st, a);
  a.sessionOpen = true;   // ... réouverture : le front NE réarme PAS
  a.nowMs = 15000u;
  PhFfVerdict v = tick(st, a);
  TEST_ASSERT_FALSE(v.released);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kHeld, (uint8_t)v.status);
  // La coquille lève la tenue (journée archivée sous le nouveau régime) : le
  // cycle fermeture → réouverture réarme alors normalement.
  a.productChangeHold = false;
  a.sessionOpen = false;
  a.nowMs = 20000u;
  tick(st, a);
  a.sessionOpen = true;
  a.nowMs = 25000u;
  v = tick(st, a);
  TEST_ASSERT_TRUE(v.released);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
}

// =============================================================================
// G — ANTI-WINDUP via phIntegralTick (condition bloquante n°1)
// =============================================================================

static PhIntegralTuning tunNormal() {
  PhIntegralTuning t;
  t.kp = kPhKpNormal;                  // 8 mL/min par pH
  t.ki = kPhKiNormal;
  t.integralMax = kPhIntegralMaxNormal;
  t.startThreshold = 0.05f;
  t.stopThreshold = 0.01f;
  return t;
}

static PhIntegralInputs integralIn(uint32_t nowMs, float error, float ff) {
  PhIntegralInputs in;
  in.nowMs = nowMs;
  in.error = error;
  in.ampFlow = 26.0f;        // soutenable plein = 2,1667 mL/min
  in.budgetMinPerHour = 5;
  in.quotaNearLimit = false;
  in.ffMlPerMin = ff;
  return in;
}

static void test_G_le_ff_est_retranche_du_soutenable_vu_par_le_pi(void) {
  PhIntegralState st;
  PhIntegralVerdict v = phIntegralTick(st, integralIn(1000u, 0.05f, 1.2f), tunNormal());
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, kSust - 1.2f, v.sustainableMlPerMin);
}

static void test_G_sans_ff_le_soutenable_reste_plein(void) {
  PhIntegralState st;
  PhIntegralVerdict v = phIntegralTick(st, integralIn(1000u, 0.05f, 0.0f), tunNormal());
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, kSust, v.sustainableMlPerMin);
}

static void test_G_pi_avec_marge_n_est_pas_gele_malgre_le_ff(void) {
  // Demande PI = 8 × 0,05 = 0,40 mL/min < 0,9667 (part restante après FF 1,2) :
  // le FF occupe 55 % de l'actionneur et pourtant l'intégrale VIT — pas de gel
  // parasite causé par la seule présence du FF.
  PhIntegralState st;
  PhIntegralTuning tun = tunNormal();
  phIntegralTick(st, integralIn(1000u, 0.05f, 1.2f), tun);   // arme la base de temps
  PhIntegralVerdict v = phIntegralTick(st, integralIn(6000u, 0.05f, 1.2f), tun);
  TEST_ASSERT_FALSE(v.frozen);
  // 5 s × (0,05 − 0,01) = 0,20 pH·s : la charge a bien eu lieu
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.2f, st.integral);
}

static void test_G_SENTINELLE_saturation_de_la_part_pi_gele_l_integrale(void) {
  // LE test qui donne sa dimension à l'anti-windup (condition n°1) : demande
  // PI = 8 × 0,15 = 1,20 mL/min. Face au soutenable PLEIN (2,17) elle passerait ;
  // face à la PART restante après FF (2,17 − 1,20 = 0,97) elle SATURE ⇒ gel.
  // Toute mutation qui supprime la soustraction du FF rend ce test rouge.
  PhIntegralState st;
  PhIntegralTuning tun = tunNormal();
  phIntegralTick(st, integralIn(1000u, 0.15f, 1.2f), tun);
  PhIntegralVerdict v = phIntegralTick(st, integralIn(6000u, 0.15f, 1.2f), tun);
  TEST_ASSERT_TRUE(v.frozen);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.integral);  // delta positif bloqué
  // Contre-épreuve : SANS FF, la même demande ne sature pas et l'intégrale vit.
  PhIntegralState st2;
  phIntegralTick(st2, integralIn(1000u, 0.15f, 0.0f), tun);
  PhIntegralVerdict v2 = phIntegralTick(st2, integralIn(6000u, 0.15f, 0.0f), tun);
  TEST_ASSERT_FALSE(v2.frozen);
  TEST_ASSERT_TRUE(st2.integral > 0.0f);
}

static void test_G_saturation_reelle_gele_avec_ou_sans_ff(void) {
  // Demande PI = 8 × 0,30 = 2,40 > 2,17 : saturation RÉELLE, gel même sans FF.
  PhIntegralState st;
  PhIntegralTuning tun = tunNormal();
  phIntegralTick(st, integralIn(1000u, 0.30f, 0.0f), tun);
  PhIntegralVerdict v = phIntegralTick(st, integralIn(6000u, 0.30f, 0.0f), tun);
  TEST_ASSERT_TRUE(v.frozen);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.integral);
}

static void test_G_ff_devorant_tout_le_soutenable_degenere_en_gel(void) {
  // Impossible par construction (plafond 60 %), mais fail-closed vérifié : un FF
  // ≥ soutenable laisse une part nulle ⇒ gel, jamais un soutenable négatif.
  PhIntegralState st;
  PhIntegralVerdict v = phIntegralTick(st, integralIn(1000u, 0.05f, 5.0f), tunNormal());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, v.sustainableMlPerMin);
  TEST_ASSERT_TRUE(v.frozen);
}

// =============================================================================
// H — REBOOT SANS RATTRAPAGE (A7) et STABILITÉ 14 JOURS (A8)
// =============================================================================

static void test_H_reboot_meme_debit_instantane_aucun_rattrapage(void) {
  // Le FF est un DÉBIT, pas un accumulateur : un état NEUF avec les mêmes
  // entrées rend le même débit dès le premier tick — pas de « dette » servie
  // en rafale après un redémarrage en pleine séance.
  FfArgs a;
  a.load = 2.86f;  // écrêté à kCap : régime établi
  // Référence : régime établi après 10 minutes de session
  PhFfState established;
  uint32_t t = 5000u;
  a.nowMs = t;
  PhFfVerdict vEst = tick(established, a);
  for (int i = 0; i < 60; ++i) {
    t += 10000u;
    a.nowMs = t;
    vEst = tick(established, a);
  }
  // Reboot simulé : état neuf, mêmes entrées, PREMIER tick
  PhFfState fresh;
  a.nowMs = 42u;  // l'horloge repart de presque zéro
  PhFfVerdict vFresh = tick(fresh, a);
  TEST_ASSERT_EQUAL_FLOAT(vEst.ffMlPerMin, vFresh.ffMlPerMin);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)vFresh.status);
  // Et le débit reste CONSTANT ensuite : aucun terme volumique dans le verdict.
  a.nowMs = 10042u;
  PhFfVerdict vNext = tick(fresh, a);
  TEST_ASSERT_EQUAL_FLOAT(vFresh.ffMlPerMin, vNext.ffMlPerMin);
}

static void test_H_stabilite_A8_quatorze_jours_sans_derive(void) {
  // Boucle plante + régulation sur 14 jours, avec le VRAI anneau de charge
  // (phLoadHistoryAppend / phLoadObservation) : chaque jour, le FF calculé est
  // réinjecté dans la consommation, la consommation archivée est BORNÉE par
  // l'hystérésis de session à la charge réelle (une sur-injection ferme la
  // session : elle ne peut pas s'archiver), et la médiane est recalculée.
  // Attendu : le FF converge vers min(charge réelle, 60 % du soutenable) et ne
  // dérive JAMAIS au-dessus de la charge réelle — pas d'emballement
  // auto-entretenu « je dose plus ⇒ j'apprends plus ⇒ je dose plus ».
  const float kRealLoad = 1.8f;         // mL/min — charge basique vraie du bassin
  const uint16_t kMinutes = 720;        // 12 h de filtration par jour
  const float kExpected = (kRealLoad < kCap) ? kRealLoad : kCap;  // ici kCap = 1,30

  PhLoadHistory ring;
  memset(&ring, 0, sizeof(ring));
  ring.schema = kPhLoadHistorySchema;

  PhFfState st;
  uint32_t t = 5000u;
  float lastFf = -1.0f;
  for (uint32_t day = 0; day < 14; ++day) {
    const uint32_t today = 20270601u + day;  // juin 2027 : 14 dates valides
    const PhPersistLoadObservation obs = phLoadObservation(ring, today, kSust);
    TEST_ASSERT_TRUE(obs.loadMlPerMin <= kRealLoad + 1e-4f);  // la médiane ne gonfle pas

    t += 60000u;  // un tick par jour suffit : le débit ne dépend pas du dt
    const PhFfVerdict v = phFeedforwardTick(st, 0.06f, true, 0xC0FFEEu, obs, kSust,
                                            true, t, 0.0f, 0.0f, false);
    // INVARIANT A8 : jamais au-dessus de la charge réelle, jamais au-dessus du plafond
    TEST_ASSERT_TRUE(v.ffMlPerMin <= kRealLoad + 1e-4f);
    TEST_ASSERT_TRUE(v.ffMlPerMin <= kCap + 1e-4f);
    if (day < kPhIntegralRestoreMinHistoryDays) {
      TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kNoHistory, (uint8_t)v.status);
    } else {
      TEST_ASSERT_EQUAL_UINT8((uint8_t)PhFfStatus::kActive, (uint8_t)v.status);
      if (lastFf >= 0.0f) {  // convergé : plus AUCUNE variation d'un jour à l'autre
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, lastFf, v.ffMlPerMin);
      }
      TEST_ASSERT_FLOAT_WITHIN(1e-4f, kExpected, v.ffMlPerMin);
      lastFf = v.ffMlPerMin;
    }

    // Consommation du jour : FF + résidu PI, BORNÉE par l'hystérésis à la
    // charge réelle (le bassin n'absorbe pas plus ; au-delà la session ferme).
    const float dosedMlPerMin = kRealLoad;  // le PI complète toujours jusqu'à la charge
    PhLoadDaySample s;
    s.date = today;
    s.ml = (uint16_t)(dosedMlPerMin * (float)kMinutes + 0.5f);
    s.minutes = kMinutes;
    TEST_ASSERT_TRUE(phLoadHistoryAppend(ring, s));
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, kExpected, lastFf);
}

// =============================================================================
// I — CONTRAT FIGÉ (statuts exposés à /data + WS, constantes du gate)
// =============================================================================

static void test_I_contrat_numerique_des_statuts(void) {
  // Comme DoseRefusal : ne jamais renuméroter — le client traduit ces valeurs.
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)PhFfStatus::kInactive);
  TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)PhFfStatus::kActive);
  TEST_ASSERT_EQUAL_UINT8(2, (uint8_t)PhFfStatus::kHeld);
  TEST_ASSERT_EQUAL_UINT8(3, (uint8_t)PhFfStatus::kNoHistory);
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)PhFfStatus::kQuota);
}

static void test_I_contrat_numerique_des_constantes(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.60f, kPhFfMaxSustainableFraction);
  TEST_ASSERT_EQUAL_FLOAT(0.60f, kPhFfDailyQuotaFraction);
  TEST_ASSERT_EQUAL_UINT32(1800000u, kPhFfLowHoldMs);   // 30 min pile (critère spec)
  TEST_ASSERT_EQUAL_FLOAT(0.05f, kPhFfAdverseErrorPh);
  TEST_ASSERT_EQUAL_UINT8(3, kPhIntegralRestoreMinHistoryDays);
}

// =============================================================================

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  // A — calcul du débit
  RUN_TEST(test_A_ff_egale_la_charge_observee_sous_le_plafond);
  RUN_TEST(test_A_charge_au_dessus_du_plafond_est_ecretee_exactement);
  RUN_TEST(test_A_charge_nulle_est_une_observation_legitime_ff_nul);
  RUN_TEST(test_A_soutenable_nul_ou_non_fini_ferme_le_ff);
  RUN_TEST(test_A_charge_non_finie_ou_negative_ferme_le_ff);
  RUN_TEST(test_A_hors_session_le_ff_ne_vit_pas);
  // B — historique minimal
  RUN_TEST(test_B_moins_de_trois_jours_ff_nul_statut_3);
  RUN_TEST(test_B_frontiere_exacte_trois_jours_ouvrent_le_ff);
  // C — gel quota
  RUN_TEST(test_C_quota_a_60_pourcent_gele_le_ff);
  RUN_TEST(test_C_frontiere_exacte_du_quota);
  RUN_TEST(test_C_limite_desactivee_pas_de_gel_quota);
  RUN_TEST(test_C_quota_est_rendu_meme_hors_session);
  // D — latch erreur défavorable
  RUN_TEST(test_D_latch_apres_30_minutes_tenues);
  RUN_TEST(test_D_29_minutes_59_secondes_ne_suffisent_pas);
  RUN_TEST(test_D_erreur_a_moins_004_ne_latch_jamais);
  RUN_TEST(test_D_frontiere_moins_005_exactement_ne_compte_pas);
  RUN_TEST(test_D_remontee_avant_30_min_remet_la_tenue_a_zero);
  RUN_TEST(test_D_trou_d_evaluation_ne_fait_pas_murir_la_tenue);
  RUN_TEST(test_D_wrap_de_millis_pendant_la_tenue);
  // E — latchs mesure / contexte / produit
  RUN_TEST(test_E_mesure_invalide_latch_immediat_sans_tenue);
  RUN_TEST(test_E_mesure_invalide_efface_la_tenue_d_erreur);
  RUN_TEST(test_E_changement_de_contexte_latch);
  RUN_TEST(test_E_premier_tick_arme_la_reference_sans_latcher);
  RUN_TEST(test_E_product_change_hold_annule_le_ff_quel_que_soit_le_reste);
  RUN_TEST(test_E_le_latch_prime_sur_l_historique_et_le_quota);
  // F — réarmement
  RUN_TEST(test_F_latch_tenu_tant_que_la_session_reste_ouverte);
  RUN_TEST(test_F_le_latch_se_leve_au_front_d_ouverture_suivant);
  RUN_TEST(test_F_la_tenue_produit_prime_sur_le_rearmement);
  // G — anti-windup via phIntegralTick
  RUN_TEST(test_G_le_ff_est_retranche_du_soutenable_vu_par_le_pi);
  RUN_TEST(test_G_sans_ff_le_soutenable_reste_plein);
  RUN_TEST(test_G_pi_avec_marge_n_est_pas_gele_malgre_le_ff);
  RUN_TEST(test_G_SENTINELLE_saturation_de_la_part_pi_gele_l_integrale);
  RUN_TEST(test_G_saturation_reelle_gele_avec_ou_sans_ff);
  RUN_TEST(test_G_ff_devorant_tout_le_soutenable_degenere_en_gel);
  // H — reboot et stabilité
  RUN_TEST(test_H_reboot_meme_debit_instantane_aucun_rattrapage);
  RUN_TEST(test_H_stabilite_A8_quatorze_jours_sans_derive);
  // I — contrat figé
  RUN_TEST(test_I_contrat_numerique_des_statuts);
  RUN_TEST(test_I_contrat_numerique_des_constantes);
  return UNITY_END();
}
