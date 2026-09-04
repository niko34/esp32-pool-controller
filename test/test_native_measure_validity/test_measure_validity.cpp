// =============================================================================
// Tests unitaires natifs — Validité des mesures pH/ORP (measure_validity_logic,
// feature-061 → feature-091 v2.34.0 : purge ADAPTATIVE par stabilité)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la machine d'état PURE extraite dans
// src/measure_validity_logic.{h,cpp} (ADR-0017) :
//   - measureValidityTick(state, inputs)
//   - measureValidityElapsedS / measureValidityCirculationSettled
//   - evaluateOutageContinuity (C3 : dispense de purge sur coupure courte)
//
// Règles verrouillées :
//   1. eau absente            → porte fermée, compteur de re-purges à zéro ;
//   2. front montant          → purge démarre ; dispense présentée → ouverte
//                               immédiatement (motif kWaived, durée 0) ;
//   3. purge en cours         → ouverture si (écoulé ≥ plancher ET stable) ;
//                               sinon au plafond (motif kCap, re-fermeture
//                               DÉSARMÉE, C2) ;
//   4. porte ouverte          → `unstable` ⇒ re-fermeture (C1), repurgeCount++,
//                               fin par stabilité = motif kRepurge.
// Adapté par embedded-developer pour compiler avec la nouvelle API ; le
// test-engineer complète (mutation) dans le cycle v2.34.0.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "constants.h"
#include "measure_validity_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Les temps viennent des CONSTANTES DE PRODUCTION : une mutation de constants.h
// (plancher, plafond, coupure courte, cadence de dispense) doit être vue ici.
static const uint32_t kFloorMs = kPurgeFloorMs;   // 10 min (= fenêtre du détecteur)
static const uint32_t kCapMs   = kPurgeCapMs;     // 90 min

static MeasureValidityInputs inputs(bool water, uint32_t nowMs, bool stable = false,
                                    bool unstable = false, bool waived = false) {
  MeasureValidityInputs in;
  in.waterPresent = water;
  in.nowMs = nowMs;
  in.stable = stable;
  in.unstable = unstable;
  in.purgeWaived = waived;
  in.floorMs = kFloorMs;
  in.capMs = kCapMs;
  return in;
}

static MeasureValidityState fresh() {
  MeasureValidityState st;
  memset(&st, 0, sizeof(st));
  return st;
}

// =============================================================================
// 1. Eau absente → fermée
// =============================================================================
void test_water_absent_closed(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(false, 1000UL, true));
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_FALSE(st.purging);
  TEST_ASSERT_EQUAL_UINT32(0, measureValidityElapsedS(st, 1000UL));
}

// =============================================================================
// 2. Front montant → purge ; plancher ET stabilité requis
// =============================================================================
void test_rise_starts_purge_and_needs_floor_and_stability(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 10000UL, true));  // stable dès le front : pas suffisant
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kPurgeStarted);

  // Plancher − 1 ms, stable → toujours fermée.
  measureValidityTick(st, inputs(true, 10000UL + kFloorMs - 1, true));
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_EQUAL_UINT32((kFloorMs - 1) / 1000UL, measureValidityElapsedS(st, 10000UL + kFloorMs - 1));

  // Plancher atteint mais PAS stable → fermée.
  measureValidityTick(st, inputs(true, 10000UL + kFloorMs, false));
  TEST_ASSERT_FALSE(st.valid);

  // Plancher atteint ET stable → ouverte, motif stabilité, durée = plancher + 10 s.
  measureValidityTick(st, inputs(true, 20000UL + kFloorMs, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_FALSE(st.purging);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kStability);
  TEST_ASSERT_EQUAL_UINT32(kFloorMs / 1000UL + 10, st.lastPurgeDurationS);
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kOpened);
  TEST_ASSERT_EQUAL_UINT8(0, st.repurgeCount);
}

// =============================================================================
// 3. Plafond → ouverture fail-safe, re-fermeture désarmée (C2)
// =============================================================================
void test_cap_opens_and_disarms_reclose(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, kCapMs - 1));
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, kCapMs));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kCap);
  TEST_ASSERT_FALSE(st.recloseArmed);
  // Instable après plafond → PAS de re-fermeture (C2).
  measureValidityTick(st, inputs(true, kCapMs + 60000UL, false, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_EQUAL_UINT8(0, st.repurgeCount);
}

// =============================================================================
// 4. Re-fermeture (C1) puis ré-ouverture par stabilité = motif re-purge
// =============================================================================
void test_reclose_then_reopen_counts_repurge(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, kFloorMs, true));
  TEST_ASSERT_TRUE(st.valid);

  // Instable → re-fermeture, compteur 1, nouvelle purge depuis ce tick.
  const uint32_t tReclose = kFloorMs + 120000UL;
  measureValidityTick(st, inputs(true, tReclose, false, true));
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_EQUAL_UINT8(1, st.repurgeCount);
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kReclosed);

  // Stable avant le NOUVEAU plancher → toujours fermée.
  measureValidityTick(st, inputs(true, tReclose + kFloorMs - 1, true));
  TEST_ASSERT_FALSE(st.valid);
  // Nouveau plancher + stable → ouverte, motif re-purge.
  measureValidityTick(st, inputs(true, tReclose + kFloorMs, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kRepurge);
  TEST_ASSERT_EQUAL_UINT32(kFloorMs / 1000UL, st.lastPurgeDurationS);

  // Retombée d'eau : compteur remis à zéro.
  measureValidityTick(st, inputs(false, tReclose + kFloorMs + 1000UL));
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_EQUAL_UINT8(0, st.repurgeCount);
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kWaterDrop);
}

// =============================================================================
// 5. Dispense (C3) : ouverte au front montant, durée 0, circulation établie
// =============================================================================
void test_waiver_opens_immediately_only_on_rise(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 5000UL, false, false, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kWaived);
  TEST_ASSERT_EQUAL_UINT32(0, st.lastPurgeDurationS);
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(st, 5000UL, kFloorMs));

  // Une dispense présentée SANS front montant (eau déjà présente) est ignorée.
  MeasureValidityState st2 = fresh();
  measureValidityTick(st2, inputs(true, 0));
  measureValidityTick(st2, inputs(true, 1000UL, false, false, true));
  TEST_ASSERT_FALSE(st2.valid);
}

// =============================================================================
// 6. Circulation établie (C4) : eau présente depuis ≥ plancher
// =============================================================================
void test_circulation_settled_after_floor(void) {
  MeasureValidityState st = fresh();
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(st, 0, kFloorMs));
  measureValidityTick(st, inputs(true, 1000UL));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(st, 1000UL + kFloorMs - 1, kFloorMs));
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(st, 1000UL + kFloorMs, kFloorMs));
}

// =============================================================================
// 7. Wrap millis — soustractions non signées
// =============================================================================
void test_millis_wrap_elapsed(void) {
  MeasureValidityState st = fresh();
  const uint32_t rise = UINT32_MAX - 999UL;
  measureValidityTick(st, inputs(true, rise));
  measureValidityTick(st, inputs(true, 2000UL, true));  // écoulé 3 s < plancher
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_EQUAL_UINT32(3, measureValidityElapsedS(st, 2000UL));
  measureValidityTick(st, inputs(true, kFloorMs - 1000UL, true));  // écoulé = plancher
  TEST_ASSERT_TRUE(st.valid);
}

// =============================================================================
// 8. Continuité de coupure (C3) — fail-closed
// =============================================================================
static OutageRtcRecord sealedRecord(uint32_t liveEpoch, bool water, uint32_t lastWaiver = 0) {
  OutageRtcRecord r;
  memset(&r, 0, sizeof(r));
  r.liveEpoch = liveEpoch;
  r.waterPresent = water ? 1 : 0;
  r.lastWaiverEpoch = lastWaiver;
  outageRecordSeal(r);
  return r;
}

static OutageContinuityInputs outageIn(const OutageRtcRecord* rec, OutageResetKind k,
                                       uint32_t epochNow, bool timeValid = true) {
  OutageContinuityInputs in;
  in.record = rec;
  in.reset = k;
  in.timeValid = timeValid;
  in.epochNow = epochNow;
  in.shortOutageS = kShortOutageS;
  in.waiverMinIntervalS = kOutageWaiverMinIntervalS;
  return in;
}

// Ancre : les valeurs documentées (10 s, 1 h) sont bien celles de production.
void test_outage_constantes_de_production(void) {
  TEST_ASSERT_EQUAL_UINT32(10, kShortOutageS);
  TEST_ASSERT_EQUAL_UINT32(3600, kOutageWaiverMinIntervalS);
  TEST_ASSERT_EQUAL_UINT32(600000UL, kPurgeFloorMs);  // 10 min (décision pool-chemistry 04/09)
  TEST_ASSERT_EQUAL_UINT32(5400000UL, kPurgeCapMs);
}

void test_outage_waived_on_short_software_reset(void) {
  OutageRtcRecord r = sealedRecord(1756800000UL, true);
  OutageContinuityVerdict v =
      evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, 1756800007UL));
  TEST_ASSERT_TRUE(v.waive);
  TEST_ASSERT_EQUAL_UINT32(7, v.outageS);
  TEST_ASSERT_TRUE(v.deny == OutageDeny::kNone);
}

void test_outage_denied_cases(void) {
  OutageRtcRecord r = sealedRecord(1756800000UL, true);
  // Mémoire absente.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(nullptr, OutageResetKind::kSoftware, 1756800005UL)).deny == OutageDeny::kMemory);
  // CRC cassé.
  OutageRtcRecord bad = r; bad.liveEpoch ^= 1;
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&bad, OutageResetKind::kSoftware, 1756800005UL)).deny == OutageDeny::kMemory);
  // Reset matériel.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kOther, 1756800005UL)).deny == OutageDeny::kResetKind);
  // Heure invalide / antérieure.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kPanic, 1756800005UL, false)).deny == OutageDeny::kClock);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kPanic, 1756799999UL)).deny == OutageDeny::kClock);
  // Trop long (11 s > 10).
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kTaskWdt, 1756800011UL)).deny == OutageDeny::kTooLong);
  // Frontière : 10 s exactement → accordée.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kIntWdt, 1756800010UL)).waive);
  // Eau absente avant la coupure.
  OutageRtcRecord dry = sealedRecord(1756800000UL, false);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&dry, OutageResetKind::kSoftware, 1756800005UL)).deny == OutageDeny::kNoWater);
  // Dispense récente (il y a 30 min).
  OutageRtcRecord recent = sealedRecord(1756800000UL, true, 1756800000UL - 1800UL);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&recent, OutageResetKind::kSoftware, 1756800005UL)).deny == OutageDeny::kRateLimited);
  // Dispense il y a ≥ 1 h → accordée.
  OutageRtcRecord old = sealedRecord(1756800000UL, true, 1756800005UL - 3600UL);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&old, OutageResetKind::kSoftware, 1756800005UL)).waive);
}

// =============================================================================
// 9. Compléments machine d'état (cycle v2.34.0, test-engineer)
// =============================================================================

// « Stable avant le plancher n'ouvre pas avant le plancher » : le détecteur
// pourrait dire stable plus tôt (fenêtre = plancher en production, mais on
// verrouille le plancher indépendamment du détecteur). Ici stable en continu
// depuis t+3 min : la porte n'ouvre qu'au tick où elapsed ≥ plancher (10 min).
void test_stable_a_t3_n_ouvre_pas_avant_le_plancher(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  for (uint32_t t = 180000UL; t < kFloorMs; t += 5000UL) {
    measureValidityTick(st, inputs(true, t, true));
    TEST_ASSERT_FALSE(st.valid);
    TEST_ASSERT_TRUE(st.purging);
  }
  measureValidityTick(st, inputs(true, kFloorMs, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_EQUAL_UINT32(kFloorMs / 1000UL, st.lastPurgeDurationS);
}

// Le plafond ouvre MÊME si le détecteur dit « instable » à ce tick : la
// filtration ne doit jamais rester aveugle plus de 90 min (fail-safe > pureté).
void test_plafond_ouvre_meme_si_instable(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, kCapMs, false, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kCap);
  TEST_ASSERT_EQUAL_UINT32(kCapMs / 1000UL, st.lastPurgeDurationS);
  TEST_ASSERT_FALSE(st.recloseArmed);
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kOpened);
}

// Ouverture par stabilité → re-fermeture ARMÉE ; dispense → re-fermeture ARMÉE
// (une coupure courte n'a pas changé l'eau, mais la purge suivante reste
// surveillée). Seul le plafond désarme.
void test_refermeture_armee_apres_stabilite_et_dispense(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, kFloorMs, true));
  TEST_ASSERT_TRUE(st.recloseArmed);

  MeasureValidityState w = fresh();
  measureValidityTick(w, inputs(true, 0, false, false, true));
  TEST_ASSERT_TRUE(w.valid);
  TEST_ASSERT_TRUE(w.recloseArmed);
  // Et une instabilité après dispense referme bien (C1 actif).
  measureValidityTick(w, inputs(true, 60000UL, false, true));
  TEST_ASSERT_FALSE(w.valid);
  TEST_ASSERT_TRUE(w.purging);
  TEST_ASSERT_EQUAL_UINT8(1, w.repurgeCount);
}

// Deux re-fermetures successives : compteur 2, chaque re-purge exige SON
// plancher complet ; la re-purge peut aussi finir au plafond (désarme).
void test_deux_refermetures_puis_plafond(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, kFloorMs, true));
  // Instants RELATIFS au plancher (10 min) : le temps ne doit jamais reculer.
  const uint32_t reclose1 = kFloorMs + 60000UL;              // 1 min après l'ouverture
  const uint32_t reclose2 = reclose1 + kFloorMs + 60000UL;   // 1 min après la ré-ouverture
  measureValidityTick(st, inputs(true, reclose1, false, true));   // re-fermeture 1
  TEST_ASSERT_EQUAL_UINT8(1, st.repurgeCount);
  measureValidityTick(st, inputs(true, reclose1 + kFloorMs - 1, true));  // plancher pas atteint
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, reclose1 + kFloorMs, true));  // ré-ouverte
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kRepurge);
  measureValidityTick(st, inputs(true, reclose2, false, true));   // re-fermeture 2
  TEST_ASSERT_EQUAL_UINT8(2, st.repurgeCount);
  TEST_ASSERT_FALSE(st.valid);
  // Jamais stable : la re-purge finit au plafond compté depuis la re-fermeture.
  measureValidityTick(st, inputs(true, reclose2 + kCapMs - 1));
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, reclose2 + kCapMs));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kCap);
  TEST_ASSERT_FALSE(st.recloseArmed);
  TEST_ASSERT_EQUAL_UINT8(2, st.repurgeCount);
}

// Porte ouverte : un tick « stable » ne change rien, aucun événement.
// Porte fermée en purge : un tick sans transition → kNone.
void test_ticks_sans_transition_sont_silencieux(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, 1000UL));
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kNone);
  measureValidityTick(st, inputs(true, kFloorMs, true));
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kOpened);
  measureValidityTick(st, inputs(true, kFloorMs + 1000UL, true));
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kNone);
  TEST_ASSERT_TRUE(st.valid);
  // Eau absente répétée : un seul kWaterDrop, puis silence.
  measureValidityTick(st, inputs(false, kFloorMs + 2000UL));
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kWaterDrop);
  measureValidityTick(st, inputs(false, kFloorMs + 3000UL));
  TEST_ASSERT_TRUE(st.event == MeasureValidityEvent::kNone);
}

// Chute d'eau EN COURS de purge : purge abandonnée, compteur à zéro, un nouveau
// front repart d'une purge complète (le compteur de re-purges ne survit pas).
void test_chute_d_eau_pendant_la_purge_reinitialise(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 0));
  measureValidityTick(st, inputs(true, kFloorMs, true));
  measureValidityTick(st, inputs(true, kFloorMs + 60000UL, false, true));  // re-fermeture
  TEST_ASSERT_EQUAL_UINT8(1, st.repurgeCount);
  measureValidityTick(st, inputs(false, kFloorMs + 120000UL));
  TEST_ASSERT_FALSE(st.purging);
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_EQUAL_UINT8(0, st.repurgeCount);
  const uint32_t rise2 = kFloorMs + 180000UL;
  measureValidityTick(st, inputs(true, rise2, true));
  TEST_ASSERT_TRUE(st.purging);
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, rise2 + kFloorMs - 1, true));
  TEST_ASSERT_FALSE(st.valid);
  measureValidityTick(st, inputs(true, rise2 + kFloorMs, true));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kStability);  // pas kRepurge
}

// Circulation établie : faux tant que < plancher, vrai immédiat si dispensée,
// et RETOMBE à faux après une chute d'eau.
void test_circulation_settled_matrice(void) {
  MeasureValidityState st = fresh();
  measureValidityTick(st, inputs(true, 1000UL));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(st, 1000UL + 1, kFloorMs));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(st, 1000UL + kFloorMs / 2, kFloorMs));
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(st, 1000UL + kFloorMs, kFloorMs));
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(st, 1000UL + 10 * kFloorMs, kFloorMs));
  measureValidityTick(st, inputs(false, 1000UL + 10 * kFloorMs));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(st, 1000UL + 10 * kFloorMs, kFloorMs));
  TEST_ASSERT_FALSE(measureValidityCirculationSettled(st, 1000UL + 20 * kFloorMs, kFloorMs));
}

// Wrap millis autour de 2^32 : plafond franchi après le wrap, circulation
// établie après le wrap, re-fermeture puis re-purge dont le plancher chevauche
// le wrap.
void test_millis_wrap_plafond_et_repurge(void) {
  MeasureValidityState st = fresh();
  const uint32_t rise = UINT32_MAX - kCapMs / 2;
  measureValidityTick(st, inputs(true, rise));
  // Écoulé = plafond − 1 → fermée ; = plafond → ouverte (les deux APRÈS le wrap).
  measureValidityTick(st, inputs(true, rise + kCapMs - 1));
  TEST_ASSERT_FALSE(st.valid);
  TEST_ASSERT_TRUE(rise + kCapMs - 1 < rise);  // on a bien wrappé
  measureValidityTick(st, inputs(true, rise + kCapMs));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_TRUE(st.lastPurgeEnd == PurgeEnd::kCap);
  TEST_ASSERT_EQUAL_UINT32(kCapMs / 1000UL, st.lastPurgeDurationS);
  TEST_ASSERT_TRUE(measureValidityCirculationSettled(st, rise + kCapMs, kFloorMs));

  MeasureValidityState r = fresh();
  const uint32_t rise2 = UINT32_MAX - 2 * kFloorMs;
  measureValidityTick(r, inputs(true, rise2));
  measureValidityTick(r, inputs(true, rise2 + kFloorMs, true));
  TEST_ASSERT_TRUE(r.valid);
  const uint32_t reclose = UINT32_MAX - kFloorMs / 2;
  measureValidityTick(r, inputs(true, reclose, false, true));
  TEST_ASSERT_FALSE(r.valid);
  measureValidityTick(r, inputs(true, reclose + kFloorMs - 1, true));  // après wrap
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT32((kFloorMs - 1) / 1000UL, measureValidityElapsedS(r, reclose + kFloorMs - 1));
  measureValidityTick(r, inputs(true, reclose + kFloorMs, true));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.lastPurgeEnd == PurgeEnd::kRepurge);
}

// Libellés de fin de purge : non vides, tous distincts (journal + WS lisibles).
void test_purge_end_labels(void) {
  const PurgeEnd all[] = {PurgeEnd::kNone, PurgeEnd::kStability, PurgeEnd::kCap,
                          PurgeEnd::kWaived, PurgeEnd::kRepurge};
  for (size_t i = 0; i < 5; ++i) {
    TEST_ASSERT_NOT_NULL(purgeEndLabel(all[i]));
    TEST_ASSERT_TRUE(strlen(purgeEndLabel(all[i])) > 0);
    for (size_t j = i + 1; j < 5; ++j) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(purgeEndLabel(all[i]), purgeEndLabel(all[j])) != 0,
                               "deux motifs de fin de purge ont le meme libelle");
    }
  }
}

// =============================================================================
// 10. Continuité de coupure — matrice complète
// =============================================================================
static const uint32_t kEpoch0 = 1756800000UL;  // 02/09/2025 08:00 UTC, > kMinValidEpoch

// Un enregistrement scellé est intact ; chacun des 160 bits inversé (magic,
// epoch, dispense, eau, réservé, CRC lui-même) le rend NON intact → kMemory.
void test_record_bit_flip_exhaustif(void) {
  OutageRtcRecord r = sealedRecord(kEpoch0, true, kEpoch0 - 7200UL);
  TEST_ASSERT_TRUE(outageRecordIntact(r));
  TEST_ASSERT_EQUAL_UINT32(20, sizeof(OutageRtcRecord));
  uint8_t* bytes = reinterpret_cast<uint8_t*>(&r);
  for (size_t bit = 0; bit < sizeof(OutageRtcRecord) * 8; ++bit) {
    bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    char msg[48];
    snprintf(msg, sizeof(msg), "bit %u inverse non detecte", (unsigned)bit);
    TEST_ASSERT_FALSE_MESSAGE(outageRecordIntact(r), msg);
    TEST_ASSERT_TRUE_MESSAGE(
        evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 + 5)).deny ==
            OutageDeny::kMemory,
        msg);
    bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
  TEST_ASSERT_TRUE(outageRecordIntact(r));
  // Enregistrement tout à zéro (RTC vierge) : non intact.
  OutageRtcRecord zero;
  memset(&zero, 0, sizeof(zero));
  TEST_ASSERT_FALSE(outageRecordIntact(zero));
}

// Whitelist des resets : les 4 resets LOGICIELS (SW, PANIC, INT_WDT, TASK_WDT)
// dispensent ; kOther (POWERON, BROWNOUT, UNKNOWN, DEEPSLEEP… côté coquille)
// refuse avec kResetKind. La coquille mappe ESP_RST_* → ces 5 valeurs
// (measure_validity.cpp, resetKindFromEsp) : le natif teste la décision pure.
void test_whitelist_resets(void) {
  OutageRtcRecord r = sealedRecord(kEpoch0, true);
  const OutageResetKind accepted[] = {OutageResetKind::kSoftware, OutageResetKind::kPanic,
                                      OutageResetKind::kIntWdt, OutageResetKind::kTaskWdt};
  for (size_t i = 0; i < 4; ++i) {
    OutageContinuityVerdict v = evaluateOutageContinuity(outageIn(&r, accepted[i], kEpoch0 + 5));
    TEST_ASSERT_TRUE(v.waive);
    TEST_ASSERT_TRUE(v.deny == OutageDeny::kNone);
    TEST_ASSERT_EQUAL_UINT32(5, v.outageS);
  }
  OutageContinuityVerdict o = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kOther, kEpoch0 + 5));
  TEST_ASSERT_FALSE(o.waive);
  TEST_ASSERT_TRUE(o.deny == OutageDeny::kResetKind);
}

// Durée de coupure : 9 s et 10 s accordées, 11 s refusée (kTooLong) ; 3 h refusée ;
// la durée calculée est rapportée même en cas de refus.
void test_duree_de_coupure_frontieres(void) {
  OutageRtcRecord r = sealedRecord(kEpoch0, true);
  OutageContinuityVerdict v9 = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 + kShortOutageS - 1));
  TEST_ASSERT_TRUE(v9.waive);
  TEST_ASSERT_EQUAL_UINT32(kShortOutageS - 1, v9.outageS);
  OutageContinuityVerdict v10 = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 + kShortOutageS));
  TEST_ASSERT_TRUE(v10.waive);
  OutageContinuityVerdict v11 = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 + kShortOutageS + 1));
  TEST_ASSERT_FALSE(v11.waive);
  TEST_ASSERT_TRUE(v11.deny == OutageDeny::kTooLong);
  TEST_ASSERT_EQUAL_UINT32(kShortOutageS + 1, v11.outageS);
  OutageContinuityVerdict v3h = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 + 10800UL));
  TEST_ASSERT_FALSE(v3h.waive);
  TEST_ASSERT_TRUE(v3h.deny == OutageDeny::kTooLong);
  TEST_ASSERT_EQUAL_UINT32(10800UL, v3h.outageS);
  // Coupure de 0 s (même seconde) : accordée.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0)).waive);
}

// Horloge : heure non synchronisée → kClock quelle que soit la valeur d'epoch ;
// epochNow < epoch enregistré (RTC reculée) → kClock ; l'un et l'autre AVANT
// l'examen de l'eau et de la durée (fail-closed : on ne raisonne pas sur une
// durée qu'on ne sait pas calculer).
void test_horloge_fail_closed(void) {
  OutageRtcRecord r = sealedRecord(kEpoch0, true);
  OutageContinuityVerdict a = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 + 5, false));
  TEST_ASSERT_FALSE(a.waive);
  TEST_ASSERT_TRUE(a.deny == OutageDeny::kClock);
  OutageContinuityVerdict b = evaluateOutageContinuity(outageIn(&r, OutageResetKind::kSoftware, kEpoch0 - 1));
  TEST_ASSERT_FALSE(b.waive);
  TEST_ASSERT_TRUE(b.deny == OutageDeny::kClock);
  TEST_ASSERT_EQUAL_UINT32(0, b.outageS);
  // Heure non sync + eau absente : c'est l'horloge qui est rapportée.
  OutageRtcRecord dry = sealedRecord(kEpoch0, false);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&dry, OutageResetKind::kSoftware, kEpoch0 + 5, false)).deny == OutageDeny::kClock);
}

// Eau absente avant la coupure : refus kNoWater même si tout le reste est bon.
void test_eau_absente_avant_coupure(void) {
  OutageRtcRecord dry = sealedRecord(kEpoch0, false);
  OutageContinuityVerdict v = evaluateOutageContinuity(outageIn(&dry, OutageResetKind::kPanic, kEpoch0 + 3));
  TEST_ASSERT_FALSE(v.waive);
  TEST_ASSERT_TRUE(v.deny == OutageDeny::kNoWater);
  // Eau absente + coupure longue : l'eau est examinée avant la durée.
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&dry, OutageResetKind::kPanic, kEpoch0 + 100)).deny == OutageDeny::kNoWater);
}

// Cadence des dispenses : dispense antérieure il y a 3599 s → refusée
// (kRateLimited) ; 3600 s → accordée ; aucune dispense (0) → accordée.
// La distance est mesurée depuis epochNow (heure du redémarrage).
void test_rate_limit_dispense_frontieres(void) {
  const uint32_t now = kEpoch0 + 5;
  OutageRtcRecord recent = sealedRecord(kEpoch0, true, now - (kOutageWaiverMinIntervalS - 1));
  OutageContinuityVerdict a = evaluateOutageContinuity(outageIn(&recent, OutageResetKind::kSoftware, now));
  TEST_ASSERT_FALSE(a.waive);
  TEST_ASSERT_TRUE(a.deny == OutageDeny::kRateLimited);
  OutageRtcRecord edge = sealedRecord(kEpoch0, true, now - kOutageWaiverMinIntervalS);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&edge, OutageResetKind::kSoftware, now)).waive);
  OutageRtcRecord never = sealedRecord(kEpoch0, true, 0);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&never, OutageResetKind::kSoftware, now)).waive);
  // Dispense « dans le futur » (RTC désordonnée) : refusée aussi (fail-closed).
  OutageRtcRecord future = sealedRecord(kEpoch0, true, now + 60);
  TEST_ASSERT_FALSE(evaluateOutageContinuity(outageIn(&future, OutageResetKind::kSoftware, now)).waive);
}

// Ordre des refus : mémoire → reset → horloge → eau → durée → cadence. Chaque
// étape est testée avec TOUTES les suivantes également fautives : c'est bien la
// première qui est rapportée (le journal dit la cause racine, pas un symptôme).
void test_ordre_des_refus(void) {
  // Tout faux : record cassé, reset matériel, heure non sync, eau absente,
  // coupure longue, dispense récente.
  OutageRtcRecord all = sealedRecord(kEpoch0, false, kEpoch0 + 10000UL);
  OutageRtcRecord broken = all; broken.magic ^= 0x80;
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&broken, OutageResetKind::kOther, kEpoch0 + 10800UL, false)).deny == OutageDeny::kMemory);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&all, OutageResetKind::kOther, kEpoch0 + 10800UL, false)).deny == OutageDeny::kResetKind);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&all, OutageResetKind::kSoftware, kEpoch0 + 10800UL, false)).deny == OutageDeny::kClock);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&all, OutageResetKind::kSoftware, kEpoch0 + 10800UL)).deny == OutageDeny::kNoWater);
  OutageRtcRecord wet = sealedRecord(kEpoch0, true, kEpoch0 + 10000UL);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&wet, OutageResetKind::kSoftware, kEpoch0 + 10800UL)).deny == OutageDeny::kTooLong);
  TEST_ASSERT_TRUE(evaluateOutageContinuity(outageIn(&wet, OutageResetKind::kSoftware, kEpoch0 + 5)).deny == OutageDeny::kRateLimited);
}

// Libellés de refus : non vides, tous distincts, y compris kNone.
void test_outage_deny_labels(void) {
  const OutageDeny all[] = {OutageDeny::kNone, OutageDeny::kMemory, OutageDeny::kNoWater,
                            OutageDeny::kResetKind, OutageDeny::kClock, OutageDeny::kTooLong,
                            OutageDeny::kRateLimited};
  const size_t n = sizeof(all) / sizeof(all[0]);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_NOT_NULL(outageDenyLabel(all[i]));
    TEST_ASSERT_TRUE(strlen(outageDenyLabel(all[i])) > 0);
    for (size_t j = i + 1; j < n; ++j) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(outageDenyLabel(all[i]), outageDenyLabel(all[j])) != 0,
                               "deux motifs de refus ont le meme libelle");
    }
  }
}

// Scellement : le CRC couvre les 16 premiers octets (magic inclus) ; re-sceller
// après modification donne un enregistrement intact ; le magic est imposé.
void test_record_seal_et_magic(void) {
  OutageRtcRecord r;
  memset(&r, 0xA5, sizeof(r));
  outageRecordSeal(r);
  TEST_ASSERT_EQUAL_UINT32(kOutageRtcMagic, r.magic);
  TEST_ASSERT_TRUE(outageRecordIntact(r));
  TEST_ASSERT_EQUAL_UINT32(outageFnv1a(&r, 16), r.crc);
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
  RUN_TEST(test_outage_constantes_de_production);
  RUN_TEST(test_water_absent_closed);
  RUN_TEST(test_rise_starts_purge_and_needs_floor_and_stability);
  RUN_TEST(test_cap_opens_and_disarms_reclose);
  RUN_TEST(test_reclose_then_reopen_counts_repurge);
  RUN_TEST(test_waiver_opens_immediately_only_on_rise);
  RUN_TEST(test_circulation_settled_after_floor);
  RUN_TEST(test_millis_wrap_elapsed);
  RUN_TEST(test_outage_waived_on_short_software_reset);
  RUN_TEST(test_outage_denied_cases);
  // 9. compléments machine d'état
  RUN_TEST(test_stable_a_t3_n_ouvre_pas_avant_le_plancher);
  RUN_TEST(test_plafond_ouvre_meme_si_instable);
  RUN_TEST(test_refermeture_armee_apres_stabilite_et_dispense);
  RUN_TEST(test_deux_refermetures_puis_plafond);
  RUN_TEST(test_ticks_sans_transition_sont_silencieux);
  RUN_TEST(test_chute_d_eau_pendant_la_purge_reinitialise);
  RUN_TEST(test_circulation_settled_matrice);
  RUN_TEST(test_millis_wrap_plafond_et_repurge);
  RUN_TEST(test_purge_end_labels);
  // 10. continuité de coupure — matrice
  RUN_TEST(test_record_bit_flip_exhaustif);
  RUN_TEST(test_whitelist_resets);
  RUN_TEST(test_duree_de_coupure_frontieres);
  RUN_TEST(test_horloge_fail_closed);
  RUN_TEST(test_eau_absente_avant_coupure);
  RUN_TEST(test_rate_limit_dispense_frontieres);
  RUN_TEST(test_ordre_des_refus);
  RUN_TEST(test_outage_deny_labels);
  RUN_TEST(test_record_seal_et_magic);
  return UNITY_END();
}
