// =============================================================================
// Tests unitaires natifs — Fenêtre de comptabilité pompe (bug-115)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / I²C / FreeRTOS.
// On teste le COMPORTEMENT OBSERVABLE du header pur src/acct_window_logic.h
// (ADR-0017), header-only : aucune entrée dans build_src_filter n'est requise.
//
// CE QUE CETTE SUITE PROUVE, ET RIEN DE PLUS :
//   - Le contrat pur evaluateAcctWindow() : crédit de delta, et la garantie
//     structurelle nextOn == onNow (TOUJOURS) qui rend la rémanence relevée
//     par feature-110 impossible — sur CE module, en isolation.
//   - Rejoué en SÉQUENCE (tour par tour, comme le ferait
//     settlePumpAccounting() à chaque appel d'update()), le couple
//     (volume crédité, démarrages comptés via evaluatePumpStartAccounting)
//     raconte la même histoire — aucun volume fantôme, aucun démarrage perdu
//     ni inventé.
//
// CE QU'ELLE NE PROUVE PAS (périmètre honnête, à charge de la revue/CI) :
//   - Que les QUATRE sorties anticipées de updateDosingCore() (OTA,
//     maintenance, capteurs non initialisés, eau/stabilisation/flowAssist)
//     appellent bien settlePumpAccounting() : pump_controller.cpp n'est PAS
//     compilable nativement (FreeRTOS, Arduino, I2C). Cette garantie est
//     STRUCTURELLE (le wrapper update()/updateDosingCore(), un seul point de
//     passage), vérifiée par ./tools/check_acct_settle_single_point.sh, pas
//     par un test natif.
//   - Que settlePumpAccounting() recalcule correctement phIdx/orpIdx et
//     *ManualActive depuis mqttCfg/manualMode[]/pumpDuty[] : ce recalcul vit
//     dans pump_controller.cpp, non testable nativement.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>  // snprintf : diagnostic des balayages combinatoires

#include "acct_window_logic.h"
#include "dosing_logic.h"  // evaluatePumpStartAccounting, saturatingInc — couple (N, V)
#include "constants.h"      // kAcctDeltaAnomalyMs

void setUp(void) {}
void tearDown(void) {}

static const uint32_t kSeuil = kAcctDeltaAnomalyMs;  // 5000 ms

// =============================================================================
// 1. CONTRAT PUR — axes isolés (contre-poison direct de M1..M5)
// =============================================================================

// 1. prevOn == false => AUCUN crédit, quel que soit le reste (M4).
static void test_D1_prevOn_false_jamais_de_credit(void) {
  const uint32_t lastTs[] = {0u, 1u, 1000u, UINT32_MAX};
  const uint32_t now[]    = {0u, 1u, 1000u, 5000u, UINT32_MAX};
  const bool onNows[]     = {false, true};
  char msg[128];
  for (unsigned l = 0; l < sizeof(lastTs) / sizeof(lastTs[0]); ++l) {
    for (unsigned n = 0; n < sizeof(now) / sizeof(now[0]); ++n) {
      for (unsigned o = 0; o < 2; ++o) {
        AcctWindow w = evaluateAcctWindow(false, lastTs[l], onNows[o], now[n]);
        snprintf(msg, sizeof(msg), "prevOn=false lastTs=%u now=%u onNow=%d",
                 (unsigned)lastTs[l], (unsigned)now[n], (int)onNows[o]);
        TEST_ASSERT_FALSE_MESSAGE(w.creditDelta, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, w.deltaMs, msg);
      }
    }
  }
}

// 2. lastTsMs == 0 (fenêtre jamais ouverte) => AUCUN crédit, même prevOn vrai (M5).
static void test_D1_lastTsMs_zero_jamais_de_credit(void) {
  const uint32_t now[] = {0u, 1u, 1000u, UINT32_MAX};
  char msg[96];
  for (unsigned n = 0; n < sizeof(now) / sizeof(now[0]); ++n) {
    AcctWindow w = evaluateAcctWindow(true, 0u, true, now[n]);
    snprintf(msg, sizeof(msg), "lastTs=0 now=%u : aucun credit", (unsigned)now[n]);
    TEST_ASSERT_FALSE_MESSAGE(w.creditDelta, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, w.deltaMs, msg);
  }
}

// 3. nowMs == lastTsMs => delta nul => AUCUN crédit (tour re-rentré deux fois
// à la même milliseconde, ou tick inaugural à now==lastTs).
static void test_D1_delta_nul_jamais_de_credit(void) {
  AcctWindow w1 = evaluateAcctWindow(true, 5000u, true, 5000u);
  TEST_ASSERT_FALSE(w1.creditDelta);
  TEST_ASSERT_EQUAL_UINT32(0u, w1.deltaMs);

  AcctWindow w2 = evaluateAcctWindow(true, 0u, true, 0u);  // lastTs==0 ET delta nul
  TEST_ASSERT_FALSE(w2.creditDelta);
}

// 4. nextOn == onNow TOUJOURS — LA garantie structurelle. Quatre combinaisons
// de (prevOn, onNow) exhaustivement (contre-poison direct de M2).
static void test_D1_nextOn_toujours_egal_a_onNow_quatre_combinaisons(void) {
  const bool prevOns[] = {false, true};
  const bool onNows[]  = {false, true};
  char msg[64];
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned o = 0; o < 2; ++o) {
      AcctWindow w = evaluateAcctWindow(prevOns[p], 1000u, onNows[o], 2000u);
      snprintf(msg, sizeof(msg), "prevOn=%d onNow=%d", (int)prevOns[p], (int)onNows[o]);
      TEST_ASSERT_EQUAL_MESSAGE(onNows[o], w.nextOn, msg);
    }
  }
}

// 5. nextTimestampMs == (onNow ? nowMs : 0) (contre-poison direct de M1).
static void test_D1_nextTimestampMs_suit_onNow(void) {
  AcctWindow on = evaluateAcctWindow(true, 1000u, true, 9999u);
  TEST_ASSERT_EQUAL_UINT32(9999u, on.nextTimestampMs);

  AcctWindow off = evaluateAcctWindow(true, 1000u, false, 9999u);
  TEST_ASSERT_EQUAL_UINT32(0u, off.nextTimestampMs);

  // Même depuis prevOn=false (démarrage) : le nouvel état est posé, pas
  // conditionné à l'ancien.
  AcctWindow restart = evaluateAcctWindow(false, 0u, true, 42u);
  TEST_ASSERT_EQUAL_UINT32(42u, restart.nextTimestampMs);
}

// =============================================================================
// 2. SÉQUENCES — où se joue le défaut réellement corrigé
// =============================================================================

// Harnais : rejoue une suite de ticks (onNow, nowMs) comme le ferait
// settlePumpAccounting() à CHAQUE appel d'update(), qu'il vienne de la queue
// nominale ou d'une sortie anticipée. Accumule le delta total crédité (proxy
// du volume : le débit est une constante multiplicative, hors périmètre de ce
// module pur) et le nombre de démarrages comptés par evaluatePumpStartAccounting
// sur le MÊME flux d'états — c'est le couple (N, V) de D2.
struct SeqResult {
  uint32_t totalCreditedMs;
  uint32_t starts;
  uint32_t anomalies;
};

static SeqResult replaySequence(const bool* onNowSeq, const uint32_t* nowMsSeq,
                                unsigned count, uint32_t anomalyThresholdMs) {
  SeqResult r = {0u, 0u, 0u};
  bool prevOn = false;
  uint32_t lastTs = 0u;
  for (unsigned i = 0; i < count; ++i) {
    AcctWindow w = evaluateAcctWindow(prevOn, lastTs, onNowSeq[i], nowMsSeq[i]);
    if (w.creditDelta) r.totalCreditedMs += w.deltaMs;
    PumpStartAccounting acct =
        evaluatePumpStartAccounting(prevOn, onNowSeq[i], w.deltaMs, anomalyThresholdMs);
    if (acct.countStart) r.starts = saturatingInc(r.starts);
    if (acct.countAnomaly) r.anomalies = saturatingInc(r.anomalies);
    prevOn = w.nextOn;
    lastTs = w.nextTimestampMs;
  }
  return r;
}

// 6. ON x3 -> coupure 6h -> reprise.
// t=1000 ON (demarrage) ; t=2000 ON ; t=3000 ON ; t=4000 sortie anticipee
// (eau absente) détectée : pompe réellement coupée A CET INSTANT -> onNow
// bascule a false, le dernier delta ON (3000->4000, 1s) est credite (benefice
// collateral, test 10) ; puis 6h de ticks a onNow=false, un par seconde
// (21600 ticks) ; puis reprise ON.
static void test_D1_seq_on3_coupure6h_reprise_zero_fantome_deux_demarrages(void) {
  const unsigned kGapTicks = 21600;  // 6h a 1 tick/s
  const unsigned kTotal = 4 + kGapTicks + 1;
  static bool onNowSeq[4 + 21600 + 1];
  static uint32_t nowMsSeq[4 + 21600 + 1];

  onNowSeq[0] = true;  nowMsSeq[0] = 1000u;   // demarrage
  onNowSeq[1] = true;  nowMsSeq[1] = 2000u;
  onNowSeq[2] = true;  nowMsSeq[2] = 3000u;
  onNowSeq[3] = false; nowMsSeq[3] = 4000u;   // sortie anticipee : coupure reelle

  uint32_t t = 4000u;
  for (unsigned i = 0; i < kGapTicks; ++i) {
    t += 1000u;
    onNowSeq[4 + i] = false;
    nowMsSeq[4 + i] = t;
  }
  t += 1000u;
  onNowSeq[kTotal - 1] = true;  // reprise
  nowMsSeq[kTotal - 1] = t;

  SeqResult r = replaySequence(onNowSeq, nowMsSeq, kTotal, kSeuil);

  // Volume : uniquement les 3 deltas ON->ON/ON->OFF transitoire (1000+1000+1000
  // = 3000 ms). Aucun mL fantome sur les 6h de coupure.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(3000u, r.totalCreditedMs,
      "seuls les ticks ON (+ le dernier ON avant coupure) sont credites, zero fantome");
  // Demarrages : 1 au depart, 1 a la reprise = 2, PAS 1.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, r.starts,
      "le redemarrage physique apres coupure DOIT etre compte (etait perdu avant bug-115)");
}

// 7. Manuel resté ON pendant toute la fenêtre (cas flowAssistSuspend) :
// l'early-exit ne force PAS applyPumpDuty(*,0) en mode manuel -> onNow reste
// vrai sur TOUTE la séquence. Crédit continu, 1 SEUL démarrage : aucun front,
// donc aucun comptage parasite (condition C5 du gate).
static void test_D1_seq_manuel_continu_flowAssistSuspend_credit_continu_un_demarrage(void) {
  const unsigned kCount = 50;
  bool onNowSeq[kCount];
  uint32_t nowMsSeq[kCount];
  for (unsigned i = 0; i < kCount; ++i) {
    onNowSeq[i] = true;  // manuel : jamais coupe, meme dans la branche flowAssist
    nowMsSeq[i] = 1000u + i * 1000u;
  }
  SeqResult r = replaySequence(onNowSeq, nowMsSeq, kCount, kSeuil);

  // Credit continu : 49 intervalles de 1000ms (le premier tick n'a pas de
  // delta puisque lastTs etait a 0).
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(49000u, r.totalCreditedMs,
      "credit continu sur toute la marche manuelle, aucune perte");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, r.starts,
      "aucun front OFF->ON pendant une marche manuelle continue : 1 seul demarrage");
}

// 8. Repliement millis() (24,8 j) au milieu d'une fenêtre : delta EXACT,
// jamais négatif (contre-poison M3 : soustraction signée).
static void test_D1_repliement_millis_delta_exact_jamais_negatif(void) {
  uint32_t lastTs = UINT32_MAX - 2000u;  // juste avant le repliement
  uint32_t now = 3000u;                   // apres le repliement
  AcctWindow w = evaluateAcctWindow(true, lastTs, true, now);
  TEST_ASSERT_TRUE_MESSAGE(w.creditDelta, "le repliement ne doit pas annuler le credit");
  // Elapsed reel = (2^32 - lastTs) + now = 2001 + 3000 = 5001.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(5001u, w.deltaMs,
      "delta exact a travers le repliement de millis(), arithmetique NON signee");

  // Repliement pile sur la fenetre d'anomalie : ne doit PAS apparaitre comme
  // un delta negatif (qui, mal interprete en uint32, vaudrait un nombre
  // ENORME proche de 2^32 si la soustraction etait signee puis recastee).
  TEST_ASSERT_TRUE(w.deltaMs < 1000000u);
}

// 8bis. Delta REELLEMENT au-dela de la frontiere de debordement int32
// (2^31 ms ~ 24,8 j) : c'est LA valeur qui distingue une soustraction NON
// signee (correcte) d'une soustraction SIGNEE (M3) — en dessous de cette
// frontiere, les deux arithmetiques coincident par construction et ne
// permettent pas de les departager (piege rencontre en ecrivant cette suite).
// Avec une soustraction signee, (int32_t)2147483648u deborde et redevient
// NEGATIF : creditDelta serait perdu alors que le delta est reel et positif.
static void test_D1_delta_au_dela_du_debordement_int32_reste_credite(void) {
  // lastTsMs=1 (non nul, condition d'ouverture de fenetre respectee) ;
  // nowMs = 1 + 2^31 : delta reel = 2^31 ms, EXACTEMENT a la frontiere.
  AcctWindow w = evaluateAcctWindow(true, 1u, true, 2147483649u);
  TEST_ASSERT_TRUE_MESSAGE(w.creditDelta,
      "delta de 2^31 ms : reel et positif, doit rester credite (non signe)");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(2147483648u, w.deltaMs,
      "delta exact, pas de signe perdu au-dela de INT32_MAX");
}

// 9. Coupure longue 6h : AUCUN traitement spécial — une suite de ticks à
// creditDelta=false (condition C4). Si un seuil de durée existait, ce test
// échouerait en trouvant un unique "gros" delta au lieu d'une suite de zéros.
static void test_D1_coupure_longue_aucune_credit_isole_suite_de_zeros(void) {
  bool prevOn = false;   // deja retombe a OFF (ancien front gere ailleurs)
  uint32_t lastTs = 0u;  // purge par le tick transitoire precedent
  uint32_t t = 4000u;
  for (int i = 0; i < 21600; ++i) {  // 6h a 1 tick/s
    t += 1000u;
    AcctWindow w = evaluateAcctWindow(prevOn, lastTs, false, t);
    TEST_ASSERT_FALSE_MESSAGE(w.creditDelta,
        "aucun credit isole sur un tick OFF->OFF pendant une coupure longue");
    prevOn = w.nextOn;
    lastTs = w.nextTimestampMs;
  }
  TEST_ASSERT_FALSE(prevOn);
  TEST_ASSERT_EQUAL_UINT32(0u, lastTs);
}

// 10. Le dernier tick ON avant la coupure est CRÉDITÉ — bénéfice collatéral,
// et UNE SEULE FOIS (pas rejoué au tick suivant).
static void test_D1_dernier_tick_on_avant_coupure_credite_une_seule_fois(void) {
  // t=1000 ON (demarrage, pas de credit : lastTs etait 0)
  AcctWindow w0 = evaluateAcctWindow(false, 0u, true, 1000u);
  TEST_ASSERT_FALSE(w0.creditDelta);

  // t=1500 : sortie anticipee, pompe coupee A CET INSTANT : le delai
  // 1000->1500 (pompe reellement alimentee) DOIT etre credite.
  AcctWindow w1 = evaluateAcctWindow(w0.nextOn, w0.nextTimestampMs, false, 1500u);
  TEST_ASSERT_TRUE_MESSAGE(w1.creditDelta, "le dernier intervalle ON avant coupure est credite");
  TEST_ASSERT_EQUAL_UINT32(500u, w1.deltaMs);

  // t=1600 : tick suivant, toujours OFF : AUCUN second credit du meme
  // intervalle (lastTs a ete remis a 0 par le tick precedent).
  AcctWindow w2 = evaluateAcctWindow(w1.nextOn, w1.nextTimestampMs, false, 1600u);
  TEST_ASSERT_FALSE_MESSAGE(w2.creditDelta, "le meme intervalle ne doit pas etre recredite");
}

// Enchaînement sortie/reprise × 10 — vérifie que N ticks de rupture de front
// produisent EXACTEMENT N démarrages (pas plus, pas moins), avec volume
// conforme au seul temps réellement ON.
static void test_D1_enchainement_dix_sorties_reprises_dix_demarrages(void) {
  const unsigned kCycles = 10;
  const unsigned kPerCycle = 4;  // 2 ON, 1 transitoire OFF, 1 OFF pur
  const unsigned kCount = kCycles * kPerCycle;
  bool onNowSeq[40];
  uint32_t nowMsSeq[40];
  uint32_t t = 0u;
  unsigned idx = 0;
  for (unsigned c = 0; c < kCycles; ++c) {
    t += 1000u; onNowSeq[idx] = true;  nowMsSeq[idx] = t; idx++;  // demarrage
    t += 1000u; onNowSeq[idx] = true;  nowMsSeq[idx] = t; idx++;  // ON
    t += 1000u; onNowSeq[idx] = false; nowMsSeq[idx] = t; idx++;  // coupure (transitoire credite)
    t += 1000u; onNowSeq[idx] = false; nowMsSeq[idx] = t; idx++;  // OFF pur (aucun credit)
  }
  TEST_ASSERT_EQUAL_UINT32(kCount, idx);
  SeqResult r = replaySequence(onNowSeq, nowMsSeq, kCount, kSeuil);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(kCycles, r.starts, "10 cycles sortie/reprise = 10 demarrages");
  // Volume : 2 deltas de 1000ms credites par cycle (ON->ON, ON->OFF transitoire).
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(kCycles * 2000u, r.totalCreditedMs,
      "volume = exactement le temps reellement ON, par cycle");
}

// =============================================================================
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  // 1. Contrat pur, axes isoles.
  RUN_TEST(test_D1_prevOn_false_jamais_de_credit);
  RUN_TEST(test_D1_lastTsMs_zero_jamais_de_credit);
  RUN_TEST(test_D1_delta_nul_jamais_de_credit);
  RUN_TEST(test_D1_nextOn_toujours_egal_a_onNow_quatre_combinaisons);
  RUN_TEST(test_D1_nextTimestampMs_suit_onNow);
  // 2. Sequences.
  RUN_TEST(test_D1_seq_on3_coupure6h_reprise_zero_fantome_deux_demarrages);
  RUN_TEST(test_D1_seq_manuel_continu_flowAssistSuspend_credit_continu_un_demarrage);
  RUN_TEST(test_D1_repliement_millis_delta_exact_jamais_negatif);
  RUN_TEST(test_D1_delta_au_dela_du_debordement_int32_reste_credite);
  RUN_TEST(test_D1_coupure_longue_aucune_credit_isole_suite_de_zeros);
  RUN_TEST(test_D1_dernier_tick_on_avant_coupure_credite_une_seule_fois);
  RUN_TEST(test_D1_enchainement_dix_sorties_reprises_dix_demarrages);
  return UNITY_END();
}
