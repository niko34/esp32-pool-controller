// =============================================================================
// Tests unitaires natifs — Validité des mesures pH/ORP (measure_validity_logic)
// RÉÉCRITURE INTÉGRALE feature-139 (v2.62.0) — RETRAIT de l'analyse de
// stabilité (purge_stability_logic, supprimé) et du quota horaire de dispense.
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la machine d'état PURE de
// src/measure_validity_logic.{h,cpp} (ADR-0017) :
//   - measureValidityTick(state, inputs)
//   - measureValidityElapsedS / measureValidityCirculationSettled
//   - evaluateOutageContinuity (C3 : dispense de purge à travers un reboot)
//
// RÈGLES VERROUILLÉES (v2.62.0, gate pool-chemistry du 2026-10-08) :
//   1. eau absente       → porte FERMÉE ; datation de l'arrêt (waterStopMs) et
//                          LATCH monotone stopGraceExceeded dès que l'arrêt
//                          atteint graceMs (kWaterStopGraceS) ;
//   2. front montant     → purge démarre ; si l'arrêt qui se termine était
//                          datable ET court (haveWaterStop && !stopGraceExceeded)
//                          → porte OUVERTE immédiatement (kWaived, durée 0,
//                          waivedCycle = true) ;
//   3. purge en cours    → OUVERTURE dès que l'écoulé ≥ floorMs (kStability).
//                          Plus de condition d'étendue, plus de re-fermeture,
//                          plus de plafond (ex-feature-091, retiré).
//
// UN SEUL CHEMIN DE DISPENSE : qu'un arrêt d'eau traverse un redémarrage ou
// non, c'est TOUJOURS la même comparaison « durée < graceMs » qui décide
// (arbitrage pool-chemistry — c'est le défaut corrigé du 2026-10-08 20:06 :
// dispense accordée pour 8 s puis annulée de fait au démarrage filtration
// 5 s plus tard, faute de ce chemin unique).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "constants.h"
#include "measure_validity_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Les temps viennent des CONSTANTES DE PRODUCTION : une mutation de
// constants.h (plancher, grâce) doit être vue ici.
static const uint32_t kFloorMs = kPurgeFloorMs;              // 10 min
static const uint32_t kGraceMs = kWaterStopGraceS * 1000UL;  // 3 min

static MeasureValidityInputs inputs(bool water, uint32_t nowMs,
                                    uint32_t floorMs = kFloorMs,
                                    uint32_t graceMs = kGraceMs) {
  MeasureValidityInputs in;
  in.waterPresent = water;
  in.nowMs = nowMs;
  in.floorMs = floorMs;
  in.graceMs = graceMs;
  return in;
}

static MeasureValidityState fresh() {
  MeasureValidityState st;
  memset(&st, 0, sizeof(st));
  return st;
}

// État "circulation déjà établie" (porte ouverte) : pour tester un arrêt
// d'eau sans repasser par tout un premier cycle de purge.
static MeasureValidityState established() {
  MeasureValidityState st = fresh();
  st.waterPresent = true;
  st.valid = true;
  return st;
}

static OutageRtcRecord sealedRecord(uint32_t liveEpoch, bool water) {
  OutageRtcRecord r;
  memset(&r, 0, sizeof(r));
  r.liveEpoch = liveEpoch;
  r.waterPresent = water ? 1 : 0;
  outageRecordSeal(r);
  return r;
}

static OutageContinuityInputs outageIn(const OutageRtcRecord* rec, OutageResetKind k,
                                       uint32_t epochNow, uint32_t graceS = kWaterStopGraceS,
                                       bool timeValid = true) {
  OutageContinuityInputs in;
  in.record = rec;
  in.reset = k;
  in.timeValid = timeValid;
  in.epochNow = epochNow;
  in.graceS = graceS;
  return in;
}

// =============================================================================
// Ancrages — valeurs de production (une dérive de constants.h doit casser ici
// ET à la compilation via les static_assert de constants.h)
// =============================================================================
void test_ancrage_constantes_production(void) {
  TEST_ASSERT_EQUAL_UINT32(180, kWaterStopGraceS);
  TEST_ASSERT_EQUAL_UINT32(600000UL, kPurgeFloorMs);
  TEST_ASSERT_EQUAL_UINT32(16, sizeof(OutageRtcRecord));
}

// =============================================================================
// CAS 1 — Grâce d'arrêt d'eau court : bornes EXACTES 179 / 180 / 181 s
// =============================================================================
void test_grace_boundary_179_180_181(void) {
  // 179 s, sans tick intermédiaire au seuil → dispense immédiate.
  MeasureValidityState a = established();
  measureValidityTick(a, inputs(false, 1000UL));             // drop, waterStopMs = 1000
  measureValidityTick(a, inputs(true, 1000UL + 179000UL));   // reprise 179 s plus tard
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_TRUE(a.lastPurgeEnd == PurgeEnd::kWaived);
  TEST_ASSERT_EQUAL_UINT32(0, a.lastPurgeDurationS);
  TEST_ASSERT_TRUE(a.waivedCycle);

  // Latch direct : 179 999 ms → toujours FAUX ; 180 000 ms pile → VRAI (>=).
  MeasureValidityState b = established();
  measureValidityTick(b, inputs(false, 1000UL));
  measureValidityTick(b, inputs(false, 1000UL + kGraceMs - 1UL));
  TEST_ASSERT_FALSE(b.stopGraceExceeded);
  measureValidityTick(b, inputs(false, 1000UL + kGraceMs));
  TEST_ASSERT_TRUE(b.stopGraceExceeded);

  // 180 s PILE : le latch est déjà armé au moment de la reprise (ticks
  // réalistes — l'eau était surveillée en continu pendant l'arrêt) → PAS de
  // dispense, plancher complet exigé. C'est la frontière exacte de la
  // condition « >= » (pool-chemistry).
  MeasureValidityState c = established();
  measureValidityTick(c, inputs(false, 1000UL));
  measureValidityTick(c, inputs(false, 1000UL + kGraceMs));  // latch armé à 180 s pile
  measureValidityTick(c, inputs(true, 1000UL + kGraceMs));   // reprise au même instant
  TEST_ASSERT_FALSE(c.valid);
  TEST_ASSERT_TRUE(c.purging);
  TEST_ASSERT_FALSE(c.waivedCycle);

  // 181 s → idem : plancher complet, qui s'applique ensuite normalement.
  MeasureValidityState d = established();
  measureValidityTick(d, inputs(false, 1000UL));
  measureValidityTick(d, inputs(false, 1000UL + kGraceMs));       // latch armé
  measureValidityTick(d, inputs(true, 1000UL + 181000UL));        // reprise à 181 s
  TEST_ASSERT_FALSE(d.valid);
  TEST_ASSERT_TRUE(d.purging);
  measureValidityTick(d, inputs(true, 1000UL + 181000UL + kFloorMs - 1UL));
  TEST_ASSERT_FALSE(d.valid);
  measureValidityTick(d, inputs(true, 1000UL + 181000UL + kFloorMs));
  TEST_ASSERT_TRUE(d.valid);
  TEST_ASSERT_TRUE(d.lastPurgeEnd == PurgeEnd::kStability);
}

// =============================================================================
// CAS 2 — LE CŒUR DE L'ARBITRAGE : même verdict AVEC et SANS reboot
// =============================================================================
// Contexte (2026-10-08 20:06) : l'ancien code accordait la dispense pour une
// coupure de 8 s puis l'ANNULAIT DE FAIT au démarrage filtration 5 s plus
// tard (deux chemins de décision distincts). Ce test interdit la récidive :
// la durée choisie (90 s) est volontairement ENTRE l'ancien seuil disparu
// (ex-kShortOutageS = 10 s) et le nouveau (kWaterStopGraceS = 180 s) — tout
// mutant qui réintroduirait l'ancien seuil sur l'un des deux chemins fait
// diverger ce test.
void test_meme_verdict_avec_et_sans_reboot(void) {
  const uint32_t outageS = 90UL;

  // --- SANS reboot : l'arrêt est vu en DIRECT par la machine d'état (tick).
  MeasureValidityState live = established();
  measureValidityTick(live, inputs(false, 1000UL));
  measureValidityTick(live, inputs(true, 1000UL + outageS * 1000UL));
  TEST_ASSERT_TRUE(live.valid);
  TEST_ASSERT_TRUE(live.lastPurgeEnd == PurgeEnd::kWaived);
  TEST_ASSERT_EQUAL_UINT32(0, live.lastPurgeDurationS);
  TEST_ASSERT_TRUE(live.waivedCycle);

  // --- AVEC reboot : la coupure est portée par la mémoire RTC ; reproduction
  // fidèle de MeasureValidityManager::begin() — evaluateOutageContinuity()
  // PUIS pré-semis de waterStopMs/haveWaterStop dans le référentiel millis()
  // post-boot (soustraction non signée, wrap-safe).
  OutageRtcRecord rec = sealedRecord(5000000UL, true);
  OutageContinuityVerdict v =
      evaluateOutageContinuity(outageIn(&rec, OutageResetKind::kSoftware, 5000000UL + outageS));
  TEST_ASSERT_TRUE(v.waive);
  TEST_ASSERT_EQUAL_UINT32(outageS, v.outageS);

  MeasureValidityState rebooted = fresh();  // boot : _state = {} (zéro)
  const uint32_t bootMs = 50UL;             // millis() juste après begin()
  rebooted.haveWaterStop = true;
  rebooted.stopGraceExceeded = false;
  rebooted.waterStopMs = bootMs - (v.outageS * 1000UL);  // wrap-safe
  measureValidityTick(rebooted, inputs(true, bootMs));   // 1er update() : eau déjà là

  // MÊME VERDICT que le chemin sans reboot — à l'identique.
  TEST_ASSERT_TRUE(rebooted.valid);
  TEST_ASSERT_TRUE(rebooted.lastPurgeEnd == PurgeEnd::kWaived);
  TEST_ASSERT_EQUAL_UINT32(live.lastPurgeDurationS, rebooted.lastPurgeDurationS);
  TEST_ASSERT_TRUE(rebooted.waivedCycle);
}

// =============================================================================
// CAS 3 — Coupure secteur : purge COMPLÈTE, jamais de dispense
// =============================================================================
void test_coupure_secteur_toujours_purge_complete(void) {
  // Mémoire absente (démarrage à froid, RAM RTC aléatoire).
  TEST_ASSERT_FALSE(evaluateOutageContinuity(outageIn(nullptr, OutageResetKind::kSoftware, 5000000UL)).waive);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(nullptr, OutageResetKind::kSoftware, 5000000UL)).deny
                   == OutageDeny::kMemory);

  // CRC cassé (coupure secteur réelle : RAM RTC non rémanente → incohérente).
  OutageRtcRecord rec = sealedRecord(5000000UL, true);
  OutageRtcRecord bad = rec;
  bad.liveEpoch ^= 1UL;
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&bad, OutageResetKind::kSoftware, 5000000UL + 5UL)).deny
                   == OutageDeny::kMemory);

  // Heure invalide au boot (pas encore de NTP/RTC) → purge complète malgré un
  // enregistrement intact et un reset logiciel.
  OutageContinuityInputs noTime = outageIn(&rec, OutageResetKind::kSoftware, 5000000UL + 5UL);
  noTime.timeValid = false;
  TEST_ASSERT_TRUE(evaluateOutageContinuity(noTime).deny == OutageDeny::kClock);

  // Reset matériel (coupure secteur possible) → refusé même mémoire/horloge OK.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&rec, OutageResetKind::kOther, 5000000UL + 5UL)).deny
                   == OutageDeny::kResetKind);

  // Dans TOUS ces cas la coquille ne pré-sème rien : le premier update() suit
  // le chemin fail-closed (cas 6), jamais kWaived.
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 100UL));
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_TRUE(st.purging);
  measureValidityTick(st, inputs(true, 100UL + kFloorMs));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kStability);  // jamais kWaived ici
}

// =============================================================================
// CAS 4 — Deux arrêts courts en 20 min → DEUX dispenses (retrait du quota)
// =============================================================================
void test_deux_arrets_courts_en_20_min_deux_dispenses(void) {
  MeasureValidityState st = established();

  // Arrêt #1 : 8 s, à t = 0.
  measureValidityTick(st, inputs(false, 0UL));
  measureValidityTick(st, inputs(true, 8000UL));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kWaived);
  TEST_ASSERT_EQUAL_UINT32(0, st.lastPurgeDurationS);

  // Arrêt #2 : 8 s, 10 min plus tard (< 20 min depuis le premier). Aucun
  // quota horaire/journalier ne doit s'y opposer : DEUXIÈME dispense.
  const uint32_t t2 = 8000UL + 600000UL;  // +10 min
  measureValidityTick(st, inputs(false, t2));
  measureValidityTick(st, inputs(true, t2 + 8000UL));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kWaived);
  TEST_ASSERT_EQUAL_UINT32(0, st.lastPurgeDurationS);
  TEST_ASSERT_TRUE((t2 + 8000UL) - 8000UL < 1200000UL);  // bien dans la fenêtre de 20 min
}

// =============================================================================
// CAS 5 — Ouverture EXACTEMENT au plancher, aucune autre condition
// =============================================================================
void test_ouverture_exactement_au_plancher(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0UL));  // purge complète (pas d'arrêt daté)
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_FALSE(st.valid);

  measureValidityTick(st, inputs(true, kFloorMs - 1UL));
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, kFloorMs));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kStability);
  TEST_ASSERT_EQUAL_UINT32(kFloorMs / 1000UL, st.lastPurgeDurationS);

  // Même résultat en sautant directement au plancher : il n'y a plus de
  // fenêtre de stabilité à « remplir » entre les deux.
  MeasureValidityState jump = fresh();
  measureValidityTick(jump, inputs(true, 0UL));
  measureValidityTick(jump, inputs(true, kFloorMs));
  TEST_ASSERT_TRUE(jump.valid);
  TEST_ASSERT_EQUAL_UINT32(kFloorMs / 1000UL, jump.lastPurgeDurationS);

  // Jamais AVANT, même en multipliant les ticks rapprochés.
  MeasureValidityState dense = fresh();
  measureValidityTick(dense, inputs(true, 0UL));
  for (uint32_t t = 1000UL; t < kFloorMs; t += 1000UL) {
    measureValidityTick(dense, inputs(true, t));
    TEST_ASSERT_FALSE(dense.valid);
  }
  measureValidityTick(dense, inputs(true, kFloorMs));
  TEST_ASSERT_TRUE(dense.valid);
}

// =============================================================================
// CAS 6 — FAIL-CLOSED au boot : état zéro, eau DÉJÀ présente
// =============================================================================
void test_fail_closed_au_boot_eau_deja_presente(void) {
  MeasureValidityState st = fresh();  // zéro-initialisé : boot, eau jamais vue
  TEST_ASSERT_FALSE(st.haveWaterStop);
  measureValidityTick(st, inputs(true, 0UL));  // eau DÉJÀ présente au 1er tick
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_FALSE(st.waivedCycle);
  // Attente COMPLÈTE : un arrêt non daté n'est JAMAIS traité comme court.
  measureValidityTick(st, inputs(true, kFloorMs - 1UL));
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, kFloorMs));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kStability);
}

// =============================================================================
// CAS 7 — Repliement de millis() : le latch rend le wrap inoffensif
// =============================================================================
void test_repliement_millis_latch_monotone(void) {
  // --- Wrap simple (0xFFFFFFFF → 0) : la soustraction non signée reste
  // correcte même sans solliciter le latch.
  MeasureValidityState w = established();
  const uint32_t stopNearMax = UINT32_MAX - 500UL;
  measureValidityTick(w, inputs(false, stopNearMax));
  measureValidityTick(w, inputs(false, stopNearMax + (kGraceMs - 1U)));  // wrappe, juste avant
  TEST_ASSERT_FALSE(w.stopGraceExceeded);
  measureValidityTick(w, inputs(false, stopNearMax + kGraceMs));          // wrappe, pile dessus
  TEST_ASSERT_TRUE(w.stopGraceExceeded);

  // --- Danger « 24,8 jours » (2^31 ms) : point où un stockage/une comparaison
  // SIGNÉ(e) de la durée bascule en négatif (bug réel de ce projet, cf.
  // CLAUDE.md). Un arrêt de ~24,86 jours DOIT latcher "dépassé" normalement.
  MeasureValidityState d31 = established();
  measureValidityTick(d31, inputs(false, 1000UL));
  measureValidityTick(d31, inputs(false, 1000UL + 2147483648UL));  // 2^31 ms
  TEST_ASSERT_TRUE_MESSAGE(d31.stopGraceExceeded,
      "soustraction signee suspectee : le latch devrait etre arme bien avant 2^31 ms");
  measureValidityTick(d31, inputs(true, 1000UL + 2147483648UL + 1UL));
  TEST_ASSERT_FALSE(d31.valid);  // pas de dispense : l'arret est bien vu comme long
  TEST_ASSERT_TRUE(d31.purging);

  // --- Danger « 49,7 jours » (2^32 ms) : un arrêt réellement DURABLE dont la
  // reprise retombe, PAR REPLIEMENT, sur une distance apparente COURTE — bit
  // pour bit indiscernable d'un arrêt de quelques secondes. Sans latch
  // MONOTONE, une ré-évaluation naïve à cet instant précis dirait "court"
  // (c'est l'exact invariant que le latch protège, measure_validity_logic.h).
  MeasureValidityState l = established();
  measureValidityTick(l, inputs(false, 1000UL));
  measureValidityTick(l, inputs(false, 1000UL + kGraceMs));  // latché TÔT, à 180 s réelles
  TEST_ASSERT_TRUE(l.stopGraceExceeded);
  // nowMs = 6000 ≡ (1000 + 2^32 + 5000) [mod 2^32] : représente ~49,7 jours +
  // 5 s après le drop, mais la variable uint32_t est BIT POUR BIT identique à
  // "5 s après le drop".
  measureValidityTick(l, inputs(false, 6000UL));
  TEST_ASSERT_TRUE_MESSAGE(l.stopGraceExceeded,
      "le latch doit rester arme : sans lui, l'arret redeviendrait COURT par repliement");
  measureValidityTick(l, inputs(true, 6001UL));
  TEST_ASSERT_FALSE(l.valid);
  TEST_ASSERT_TRUE(l.purging);
  TEST_ASSERT_FALSE(l.waivedCycle);
}

// =============================================================================
// CAS 8 — measureValidityCirculationSettled() : ses trois branches
// =============================================================================
void test_circulation_settled_trois_branches(void) {
  // Branche 1 : eau absente → faux, quel que soit l'historique.
  MeasureValidityState dry = established();
  measureValidityTick(dry, inputs(false, 0UL));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(dry, 0UL, kFloorMs));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(dry, 10UL * kFloorMs, kFloorMs));

  // Branche 2 : cycle DISPENSÉ → vrai IMMÉDIATEMENT (avant même le plancher).
  MeasureValidityState w = established();
  measureValidityTick(w, inputs(false, 0UL));
  measureValidityTick(w, inputs(true, 5000UL));  // 5 s d'arrêt → dispensé
  TEST_ASSERT_TRUE(w.waivedCycle);
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(w, 5000UL, kFloorMs));
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(w, 5000UL + 1UL, kFloorMs));

  // Branche 3 : cas normal → vrai seulement à partir du plancher depuis le
  // front montant (waterRiseMs).
  MeasureValidityState n = fresh();
  measureValidityTick(n, inputs(true, 1000UL));  // purge complète (pas d'arrêt daté)
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(n, 1000UL + kFloorMs - 1UL, kFloorMs));
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(n, 1000UL + kFloorMs, kFloorMs));

  // Une chute d'eau fait retomber "settled" à faux immédiatement.
  measureValidityTick(n, inputs(false, 1000UL + kFloorMs + 10UL));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(n, 1000UL + kFloorMs + 10UL, kFloorMs));
}

// =============================================================================
// CAS 9 — La porte ne se referme JAMAIS après ouverture (sous-tend 18/22)
// =============================================================================
// Les codes DoseRefusal 18 (ReadingNaNGateClosed) et 22 (FilterGateClosed),
// dans dosing_logic.h / pump_controller.cpp, se déclenchent sur `!measuresValid`.
// Ce module ne les émet pas lui-même (hors périmètre), mais GARANTIT la
// propriété structurelle dont ils dépendent : une fois la porte ouverte, plus
// AUCUNE condition ne la referme tant que l'eau reste présente (la
// re-fermeture C1 a été retirée). Donc ces codes ne peuvent survenir
// qu'AVANT kPurgeFloorMs après une reprise d'eau, jamais après.
void test_porte_ne_se_referme_jamais_apres_ouverture(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0UL));
  TEST_ASSERT_FALSE(st.valid);  // avant le plancher : un consommateur verrait "non valide"
  measureValidityTick(st, inputs(true, kFloorMs));
  TEST_ASSERT_TRUE(st.valid);

  // Très nombreux ticks ultérieurs (y compris de grands écarts) : jamais de
  // retour à "non valide" tant que l'eau reste présente.
  uint32_t t = kFloorMs;
  for (int i = 0; i < 20; ++i) {
    t += 3600000UL;  // +1 h à chaque itération
    measureValidityTick(st, inputs(true, t));
    TEST_ASSERT_TRUE_MESSAGE(st.valid, "la porte s'est refermee sans chute d'eau");
  }

  // Seule une chute d'eau referme la porte — ce n'est pas une "re-fermeture"
  // au sens de l'ancien C1 : c'est juste la règle 1, un nouveau cycle.
  measureValidityTick(st, inputs(false, t + 1000UL));
  TEST_ASSERT_FALSE(st.valid);
}

// =============================================================================
// Compléments — continuité de coupure (non-régression de l'API retenue)
// =============================================================================
void test_whitelist_resets(void) {
  OutageRtcRecord r = sealedRecord(5000000UL, true);
  const OutageResetKind accepted[] = {OutageResetKind::kSoftware, OutageResetKind::kPanic,
                                      OutageResetKind::kIntWdt, OutageResetKind::kTaskWdt};
  for (size_t i = 0; i < 4; ++i) {
    OutageContinuityVerdict v = evaluateOutageContinuity(outageIn(&r, accepted[i], 5000000UL + 5UL));
    TEST_ASSERT_TRUE(v.waive);
    TEST_ASSERT_EQUAL_UINT32(5, v.outageS);
  }
  OutageContinuityVerdict o = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kOther, 5000000UL + 5UL));
  TEST_ASSERT_FALSE(o.waive);
  TEST_ASSERT_TRUE(o.deny == OutageDeny::kResetKind);
}

void test_duree_de_coupure_frontieres_evaluateOutageContinuity(void) {
  // Frontière ALIGNÉE MOT POUR MOT sur le latch de measureValidityTick() :
  // `>=`, pas `>`. À durée == graceS PILE, ce N'EST PAS un arrêt court.
  OutageRtcRecord r = sealedRecord(5000000UL, true);
  OutageContinuityVerdict justUnder = evaluateOutageContinuity(
      outageIn(&r, OutageResetKind::kSoftware, 5000000UL + kWaterStopGraceS - 1UL));
  TEST_ASSERT_TRUE(justUnder.waive);  // graceS - 1 : accordé
  OutageContinuityVerdict exact =
      evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, 5000000UL + kWaterStopGraceS));
  TEST_ASSERT_FALSE(exact.waive);  // == graceS PILE : refusé (>=)
  TEST_ASSERT_TRUE(exact.deny == OutageDeny::kTooLong);
  OutageContinuityVerdict over = evaluateOutageContinuity(
      outageIn(&r, OutageResetKind::kSoftware, 5000000UL + kWaterStopGraceS + 1UL));
  TEST_ASSERT_FALSE(over.waive);
  TEST_ASSERT_TRUE(over.deny == OutageDeny::kTooLong);
}

// =============================================================================
// CAS 1 bis — L'INVARIANT LUI-MÊME : même frontière, DEUX chemins, verdicts
// ÉGAUX (pas seulement "corrects" chacun de son côté)
// =============================================================================
// Défaut réel trouvé puis corrigé (2026-10-08) : `evaluateOutageContinuity`
// utilisait `>` quand le latch de measureValidityTick() utilisait `>=` — à
// durée == kWaterStopGraceS PILE, le chemin SANS reboot refusait la dispense
// pendant que le chemin AVEC reboot l'accordait. Les deux sont maintenant sur
// `>=`. Ce test assure l'ÉGALITÉ des deux verdicts (pas juste leur valeur).

// Chemin DIRECT (sans reboot) : arrêt vu en live par measureValidityTick().
// L'absence est explicitement échantillonnée au terme exact de l'arrêt (tick
// réaliste) pour que le latch soit évalué à la bonne frontière.
static bool liveDispense(uint32_t outageS) {
  MeasureValidityState st = established();
  measureValidityTick(st, inputs(false, 1000UL));
  measureValidityTick(st, inputs(false, 1000UL + outageS * 1000UL));
  measureValidityTick(st, inputs(true, 1000UL + outageS * 1000UL));
  return st.waivedCycle && st.lastPurgeEnd == PurgeEnd::kWaived;
}

// Chemin RTC (avec reboot) : reproduction fidèle de
// MeasureValidityManager::begin() — evaluateOutageContinuity() PUIS, si
// accordé seulement, pré-semis de l'état avant le 1er update().
static bool rebootDispense(uint32_t outageS) {
  OutageRtcRecord rec = sealedRecord(5000000UL, true);
  OutageContinuityVerdict v =
      evaluateOutageContinuity(outageIn(&rec, OutageResetKind::kSoftware, 5000000UL + outageS));
  MeasureValidityState st = fresh();
  const uint32_t bootMs = 50UL;
  if (v.waive) {
    st.haveWaterStop = true;
    st.stopGraceExceeded = false;
    st.waterStopMs = bootMs - (v.outageS * 1000UL);
  }
  measureValidityTick(st, inputs(true, bootMs));
  return st.waivedCycle && st.lastPurgeEnd == PurgeEnd::kWaived;
}

void test_frontiere_179_180_181_memes_verdicts_sur_les_deux_chemins(void) {
  const uint32_t durations[] = {179UL, 180UL, 181UL};
  const bool expected[] = {true, false, false};  // 179 s dispense ; 180 et 181 s : attente complète
  for (size_t i = 0; i < 3; ++i) {
    const bool live = liveDispense(durations[i]);
    const bool reboot = rebootDispense(durations[i]);
    char msg[96];
    snprintf(msg, sizeof(msg),
             "duree %u s : chemin direct=%d chemin reboot=%d (doivent etre EGAUX)",
             (unsigned)durations[i], (int)live, (int)reboot);
    // L'INVARIANT protégé : les deux chemins rendent le MÊME verdict.
    TEST_ASSERT_TRUE_MESSAGE(live == reboot, msg);
    // Et ce verdict commun est bien celui attendu.
    TEST_ASSERT_TRUE_MESSAGE(live == expected[i], msg);
  }
}

// =============================================================================
// CORRECTIF 2 (code-reviewer, 2e passage) — FAIL-OPEN sur begin() → premier
// update() RETARDÉ (remesure ajoutée en règle 2, en plus du latch)
// =============================================================================
// Défaut : `begin()` fige waterStopMs/stopGraceExceeded=false sur un `outageS`
// daté AU BOOT. Le setup (Wi-Fi, NTP, FS) ajoute ensuite des DIZAINES DE
// SECONDES avant le premier update(). Comme le latch n'est REMESURÉ qu'en
// règle 1 (eau absente) — jamais atteinte ici, l'eau est déjà présente au
// tout premier tick — la règle 2 faisait confiance à `!stopGraceExceeded`
// SANS REMESURER : un arrêt de 175 s + 30 s de setup (205 s réels > 180)
// était dispensé À TORT. Corrigé en ajoutant une remesure `(nowMs -
// waterStopMs) < graceMs` à la condition de dispense, SANS retirer le latch
// (qui reste nécessaire contre le repliement à 49,7 j).

// Reproduction FIDÈLE de begin() : waterStopMs = millis()_au_boot − outageS*1000.
static MeasureValidityState seedFromBegin(uint32_t beginMs, uint32_t outageS) {
  MeasureValidityState st = fresh();
  st.haveWaterStop = true;
  st.stopGraceExceeded = false;  // begin() n'arme le latch que si v.waive == true
  st.waterStopMs = beginMs - (outageS * 1000UL);
  return st;
}

// LE TEST QUI PROUVE LA CORRECTION : sans la remesure, ce test échoue (le
// latch seul, figé par begin() sur 175 s < 180, autoriserait la dispense).
void test_begin_puis_premier_update_retarde_205s_pas_de_dispense(void) {
  const uint32_t beginMs = 50UL;           // millis() au moment de begin()
  const uint32_t outageS = 175UL;          // arrêt mesuré au boot (< 180 s, seul)
  const uint32_t setupDelayMs = 30000UL;   // Wi-Fi/NTP/FS avant le 1er update()

  MeasureValidityState st = seedFromBegin(beginMs, outageS);
  measureValidityTick(st, inputs(true, beginMs + setupDelayMs));  // 1er update(), 30 s plus tard

  TEST_ASSERT_FALSE_MESSAGE(st.valid,
      "205 s reels > grace (180 s) : PAS de dispense - remesure manquante = regression fail-open");
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_FALSE(st.waivedCycle);
}

// Frontières : 179 s + 2 s = 181 s (> 180, pas de dispense) ; 175 s + 2 s =
// 177 s (< 180, dispense). La remesure recalcule la durée RÉELLE au moment du
// premier tick, pas seulement celle connue de begin().
void test_begin_puis_update_retarde_frontieres(void) {
  const uint32_t beginMs = 1000UL;

  MeasureValidityState a = seedFromBegin(beginMs, 179UL);
  measureValidityTick(a, inputs(true, beginMs + 2000UL));  // 179 + 2 = 181 s réels
  TEST_ASSERT_FALSE_MESSAGE(a.valid, "181 s reels : pas de dispense");
  TEST_ASSERT_TRUE(a.purging);

  MeasureValidityState b = seedFromBegin(beginMs, 175UL);
  measureValidityTick(b, inputs(true, beginMs + 2000UL));  // 175 + 2 = 177 s réels
  TEST_ASSERT_TRUE_MESSAGE(b.valid, "177 s reels : dispense");
  TEST_ASSERT_TRUE(b.waivedCycle);
}

// Frontière EXACTE de la remesure elle-même (isole l'opérateur `<` de la
// remesure, latch TOUJOURS false ici — aucun tick « eau absente » n'a eu
// l'occasion de l'armer entre begin() et ce premier update()) : élapsed ==
// graceMs PILE ⇒ PAS de dispense (`<` strict). Une mutation `<=` dispenserait
// à tort, sans que le latch (inchangé, toujours false) puisse l'empêcher.
void test_begin_puis_update_retarde_frontiere_exacte_de_la_remesure(void) {
  const uint32_t beginMs = 2000UL;
  const uint32_t outageS = 150UL;          // arrêt mesuré au boot (< 180 s)
  const uint32_t setupDelayMs = 30000UL;   // 30 s de setup → total EXACTEMENT 180 s

  MeasureValidityState st = seedFromBegin(beginMs, outageS);
  TEST_ASSERT_FALSE(st.stopGraceExceeded);  // latch jamais remis en cause ici
  measureValidityTick(st, inputs(true, beginMs + setupDelayMs));
  TEST_ASSERT_FALSE_MESSAGE(st.valid,
      "elapsed == graceMs PILE (remesure) : pas de dispense, frontiere '<' stricte");
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_FALSE(st.waivedCycle);
}

// Le latch continue de protéger le repliement de millis() à >49,7 j MÊME avec
// la remesure ajoutée : la remesure seule (nowMs - waterStopMs) redeviendrait
// trompeuse après un repliement complet, mais le AND avec `!stopGraceExceeded`
// (latché tôt, monotone) empêche toute dispense — rejeu du cas 7.
void test_latch_protege_toujours_le_repliement_49_7_jours_avec_remesure(void) {
  MeasureValidityState l = established();
  measureValidityTick(l, inputs(false, 1000UL));
  measureValidityTick(l, inputs(false, 1000UL + kGraceMs));  // latché tôt, à 180 s réelles
  TEST_ASSERT_TRUE(l.stopGraceExceeded);
  // nowMs = 6000 ≡ (1000 + 2^32 + 5000) [mod 2^32] : apparaît comme "5 s après
  // le drop" (remesure seule dirait "court"), mais le latch, lui, reste vrai.
  measureValidityTick(l, inputs(false, 6000UL));
  TEST_ASSERT_TRUE(l.stopGraceExceeded);
  measureValidityTick(l, inputs(true, 6001UL));
  TEST_ASSERT_FALSE_MESSAGE(l.valid,
      "le latch doit dominer meme si la remesure seule suggererait un arret court");
  TEST_ASSERT_TRUE(l.purging);
  TEST_ASSERT_FALSE(l.waivedCycle);
}

void test_horloge_invalide_fail_closed(void) {
  OutageRtcRecord r = sealedRecord(5000000UL, true);
  OutageContinuityInputs noTime = outageIn(&r, OutageResetKind::kSoftware, 5000000UL + 5UL,
                                           kWaterStopGraceS, false);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(noTime).deny == OutageDeny::kClock);
  // epochNow < liveEpoch enregistré : horloge incohérente (RTC reculée).
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, 5000000UL - 1UL)).deny
                   == OutageDeny::kClock);
}

void test_eau_absente_avant_coupure_et_ordre_des_refus(void) {
  OutageRtcRecord dry = sealedRecord(5000000UL, false);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&dry, OutageResetKind::kPanic, 5000000UL + 3UL)).deny
                   == OutageDeny::kNoWater);

  // Ordre des refus : mémoire → reset → horloge → eau → durée.
  OutageRtcRecord all = sealedRecord(5000000UL, false);
  OutageRtcRecord broken = all;
  broken.magic ^= 0x80;
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&broken, OutageResetKind::kOther,
                                                     5000000UL + 10800UL, 180, false)).deny
                   == OutageDeny::kMemory);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&all, OutageResetKind::kOther,
                                                     5000000UL + 10800UL, 180, false)).deny
                   == OutageDeny::kResetKind);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&all, OutageResetKind::kSoftware,
                                                     5000000UL + 10800UL, 180, false)).deny
                   == OutageDeny::kClock);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&all, OutageResetKind::kSoftware,
                                                     5000000UL + 10800UL)).deny
                   == OutageDeny::kNoWater);
  OutageRtcRecord wet = sealedRecord(5000000UL, true);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&wet, OutageResetKind::kSoftware,
                                                     5000000UL + 10800UL)).deny
                   == OutageDeny::kTooLong);
}

void test_record_bit_flip_exhaustif(void) {
  OutageRtcRecord r = sealedRecord(5000000UL, true);
  TEST_ASSERT_TRUE(outageRecordIntact(r));
  uint8_t* bytes = reinterpret_cast<uint8_t*>(&r);
  for (size_t bit = 0; bit < sizeof(OutageRtcRecord) * 8; ++bit) {
    bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    char msg[48];
    snprintf(msg, sizeof(msg), "bit %u inverse non detecte", (unsigned)bit);
    TEST_ASSERT_FALSE_MESSAGE(outageRecordIntact(r), msg);
    bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
  TEST_ASSERT_TRUE(outageRecordIntact(r));
  OutageRtcRecord zero;
  memset(&zero, 0, sizeof(zero));
  TEST_ASSERT_FALSE(outageRecordIntact(zero));
}

void test_purge_end_labels_non_vides_et_distincts(void) {
  const PurgeEnd all[] = {PurgeEnd::kNone, PurgeEnd::kStability, PurgeEnd::kWaived};
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_NOT_NULL(purgeEndLabel(all[i]));
    TEST_ASSERT_TRUE(strlen(purgeEndLabel(all[i])) > 0);
    for (size_t j = i + 1; j < 3; ++j) {
      TEST_ASSERT_TRUE(strcmp(purgeEndLabel(all[i]), purgeEndLabel(all[j])) != 0);
    }
  }
}

void test_outage_deny_labels_non_vides_et_distincts(void) {
  const OutageDeny all[] = {OutageDeny::kNone,     OutageDeny::kMemory, OutageDeny::kNoWater,
                            OutageDeny::kResetKind, OutageDeny::kClock,  OutageDeny::kTooLong};
  const size_t n = sizeof(all) / sizeof(all[0]);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_NOT_NULL(outageDenyLabel(all[i]));
    TEST_ASSERT_TRUE(strlen(outageDenyLabel(all[i])) > 0);
    for (size_t j = i + 1; j < n; ++j) {
      TEST_ASSERT_TRUE(strcmp(outageDenyLabel(all[i]), outageDenyLabel(all[j])) != 0);
    }
  }
  // Valeur hors énumération nommée : libellé imprimable quand même (fonction
  // TOTALE — enum class à type sous-jacent fixé, feature-105 A5).
  TEST_ASSERT_EQUAL_STRING("inconnue", outageDenyLabel(static_cast<OutageDeny>(200)));
}

void test_record_seal_et_magic(void) {
  OutageRtcRecord r;
  memset(&r, 0xA5, sizeof(r));
  outageRecordSeal(r);
  TEST_ASSERT_EQUAL_UINT32(kOutageRtcMagic, r.magic);
  TEST_ASSERT_TRUE(outageRecordIntact(r));
  r.liveEpoch += 1;
  TEST_ASSERT_FALSE(outageRecordIntact(r));
  outageRecordSeal(r);
  TEST_ASSERT_TRUE(outageRecordIntact(r));
}

// =============================================================================
// Runner
// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ancrage_constantes_production);
  // Cas exigés 1 à 9 (pool-chemistry, feature-139)
  RUN_TEST(test_grace_boundary_179_180_181);
  RUN_TEST(test_meme_verdict_avec_et_sans_reboot);
  RUN_TEST(test_frontiere_179_180_181_memes_verdicts_sur_les_deux_chemins);
  RUN_TEST(test_begin_puis_premier_update_retarde_205s_pas_de_dispense);
  RUN_TEST(test_begin_puis_update_retarde_frontieres);
  RUN_TEST(test_begin_puis_update_retarde_frontiere_exacte_de_la_remesure);
  RUN_TEST(test_latch_protege_toujours_le_repliement_49_7_jours_avec_remesure);
  RUN_TEST(test_coupure_secteur_toujours_purge_complete);
  RUN_TEST(test_deux_arrets_courts_en_20_min_deux_dispenses);
  RUN_TEST(test_ouverture_exactement_au_plancher);
  RUN_TEST(test_fail_closed_au_boot_eau_deja_presente);
  RUN_TEST(test_repliement_millis_latch_monotone);
  RUN_TEST(test_circulation_settled_trois_branches);
  RUN_TEST(test_porte_ne_se_referme_jamais_apres_ouverture);
  // Compléments — continuité de coupure
  RUN_TEST(test_whitelist_resets);
  RUN_TEST(test_duree_de_coupure_frontieres_evaluateOutageContinuity);
  RUN_TEST(test_horloge_invalide_fail_closed);
  RUN_TEST(test_eau_absente_avant_coupure_et_ordre_des_refus);
  RUN_TEST(test_record_bit_flip_exhaustif);
  RUN_TEST(test_purge_end_labels_non_vides_et_distincts);
  RUN_TEST(test_outage_deny_labels_non_vides_et_distincts);
  RUN_TEST(test_record_seal_et_magic);
  return UNITY_END();
}
