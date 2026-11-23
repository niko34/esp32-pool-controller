// =============================================================================
// Tests natifs — APPRENTISSAGE DE LA PAUSE MÉLANGE ORP (feature-202)
// =============================================================================
// Cible : src/orp_mixing_learn_logic.h (pur, ADR-0017) + les deux consommateurs
// du repli fail-closed / de la restauration dans dosing_window_logic.h.
// Chaque test a été validé par MUTATION (voir rapport de passage) : une suite
// qui ne tue aucun mutant ne prouve rien.
//
// NE PROUVE PAS : le câblage de PumpController (NVS réelle, mesures vivantes),
// seulement l'ORDRE d'appel dans main.cpp (sentinelle lexicale, test_16).
// =============================================================================
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "orp_mixing_learn_logic.h"
#include "dosing_window_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t W   = (uint32_t)kOrpMixingDelayMs;      // 20 min
static const uint32_t MAX = (uint32_t)kOrpMixingDelayLearnedMaxMs;  // 120 min

static OrpMixingLearnStore storeOf(uint32_t a, uint32_t b, uint32_t c, uint8_t count = 3) {
  OrpMixingLearnStore s;
  s.obsMs[0] = a; s.obsMs[1] = b; s.obsMs[2] = c; s.count = count;
  return s;
}

// Fait tourner une observation : orp[i] = mesure filtrée au jalon i+1.
static OrpMixingTickVerdict runMarks(OrpMixingObservation& o, uint32_t t0, float ref,
                                     const float* orp, size_t n) {
  orpMixingOpen(o, t0, ref);
  OrpMixingTickVerdict v;
  for (size_t i = 0; i < n; ++i) {
    OrpMixingTickInputs in;
    in.nowMs = t0 + (uint32_t)(i + 1) * W;
    in.orpFiltered = orp[i];
    in.clean = true;
    v = orpMixingTick(o, in);
    if (v.outcome != OrpMixingOutcome::kWatching) return v;
  }
  return v;
}

// --- D5 : moins de 3 observations propres => EXACTEMENT W -------------------
void test_01_zero_one_two_observations_give_exactly_floor(void) {
  OrpMixingLearnStore s;
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(s));
  orpMixingRecord(s, 6 * W);
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(s));
  orpMixingRecord(s, 6 * W);
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(s));
  TEST_ASSERT_FALSE(orpMixingIsLearned(s));
  orpMixingRecord(s, 6 * W);
  TEST_ASSERT_EQUAL_UINT32(6 * W, orpMixingEffectiveDelayMs(s));
  TEST_ASSERT_TRUE(orpMixingIsLearned(s));
}

// --- D6 : MAXIMUM, ni médiane (2W) ni moyenne (3W) ni dernière (2W) ---------
void test_02_effective_is_maximum_not_median_nor_mean(void) {
  TEST_ASSERT_EQUAL_UINT32(6 * W, orpMixingEffectiveDelayMs(storeOf(1 * W, 6 * W, 2 * W)));
  TEST_ASSERT_EQUAL_UINT32(6 * W, orpMixingEffectiveDelayMs(storeOf(6 * W, 1 * W, 2 * W)));
  TEST_ASSERT_EQUAL_UINT32(6 * W, orpMixingEffectiveDelayMs(storeOf(2 * W, 1 * W, 6 * W)));
}

// --- D2 : écrêtage bas, jamais < 20 min ; absurdes ignorés -------------------
void test_03_floor_clamp_and_absurd_rejection(void) {
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(storeOf(W / 2, W / 4, W / 3)));
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(storeOf(0, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(storeOf(W, 4000000000u, W)));
  TEST_ASSERT_EQUAL_UINT32(2 * W, orpMixingEffectiveDelayMs(storeOf(MAX + 1, 2 * W, 0)));
  TEST_ASSERT_TRUE(orpMixingEffectiveDelayMs(storeOf(MAX, MAX, MAX)) <= MAX);
}

// --- anneau : la plus ancienne sort, la fenêtre glisse ----------------------
void test_04_ring_keeps_last_three(void) {
  OrpMixingLearnStore s;
  orpMixingRecord(s, 6 * W);
  orpMixingRecord(s, 1 * W);
  orpMixingRecord(s, 1 * W);
  TEST_ASSERT_EQUAL_UINT32(6 * W, orpMixingEffectiveDelayMs(s));
  orpMixingRecord(s, 2 * W);  // 6W sort
  TEST_ASSERT_EQUAL_UINT32(2 * W, orpMixingEffectiveDelayMs(s));
  TEST_ASSERT_EQUAL_UINT8(4, s.count);
}

// --- mécanisme : k x W, jalons espacés de la CONSTANTE W --------------------
void test_05_observation_closes_at_k_times_window(void) {
  OrpMixingObservation o;
  const float up[] = {510.f, 520.f, 520.f};  // monte, monte, stagne => k=3
  OrpMixingTickVerdict v = runMarks(o, 1000u, 500.f, up, 3);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kClosed, v.outcome);
  TEST_ASSERT_EQUAL_UINT32(3 * W, v.observedMs);
  TEST_ASSERT_FALSE(o.active);
}

void test_06_flat_at_first_mark_gives_one_window(void) {
  OrpMixingObservation o;
  const float flat[] = {500.f};
  OrpMixingTickVerdict v = runMarks(o, 0u, 500.f, flat, 1);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kClosed, v.outcome);
  TEST_ASSERT_EQUAL_UINT32(W, v.observedMs);
}

void test_07_no_decision_before_the_mark_and_window_is_constant(void) {
  // Même avec une durée apprise de 6W en vigueur, le jalon reste à W.
  OrpMixingObservation o;
  orpMixingOpen(o, 5000u, 500.f);
  OrpMixingTickInputs in; in.clean = true; in.orpFiltered = 400.f;
  in.nowMs = 5000u + W - 1;
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kWatching, orpMixingTick(o, in).outcome);
  in.nowMs = 5000u + W;
  OrpMixingTickVerdict v = orpMixingTick(o, in);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kClosed, v.outcome);
  TEST_ASSERT_EQUAL_UINT32(W, v.observedMs);
}

void test_08_idle_when_not_open(void) {
  OrpMixingObservation o;
  OrpMixingTickInputs in; in.clean = true; in.orpFiltered = 500.f; in.nowMs = 99u;
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kIdle, orpMixingTick(o, in).outcome);
}

void test_09_wrap_of_millis_is_safe(void) {
  OrpMixingObservation o;
  const uint32_t t0 = 0xFFFFFFFFu - W / 2;  // le jalon franchit le repliement
  const float flat[] = {500.f};
  OrpMixingTickVerdict v = runMarks(o, t0, 500.f, flat, 1);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kClosed, v.outcome);
  TEST_ASSERT_EQUAL_UINT32(W, v.observedMs);
}

// --- D4 : observation contaminée jetée, jamais enregistrée ------------------
void test_10_dirty_window_is_discarded(void) {
  OrpMixingObservation o;
  orpMixingOpen(o, 0u, 500.f);
  OrpMixingTickInputs in; in.clean = false; in.orpFiltered = 400.f; in.nowMs = W;
  OrpMixingTickVerdict v = orpMixingTick(o, in);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kDiscarded, v.outcome);
  TEST_ASSERT_EQUAL(OrpMixingDiscard::kDirty, v.discard);
  TEST_ASSERT_EQUAL_UINT32(0u, v.observedMs);
  TEST_ASSERT_FALSE(o.active);
}

void test_11_nan_measure_is_discarded(void) {
  OrpMixingObservation o;
  orpMixingOpen(o, 0u, 500.f);
  OrpMixingTickInputs in; in.clean = true; in.orpFiltered = NAN; in.nowMs = W;
  OrpMixingTickVerdict v = orpMixingTick(o, in);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kDiscarded, v.outcome);
  TEST_ASSERT_EQUAL(OrpMixingDiscard::kInvalidMeasure, v.discard);
  // référence NaN à l'ouverture : même verdict
  OrpMixingObservation p;
  orpMixingOpen(p, 0u, NAN);
  in.orpFiltered = 400.f;
  v = orpMixingTick(p, in);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kDiscarded, v.outcome);
  TEST_ASSERT_EQUAL(OrpMixingDiscard::kInvalidMeasure, v.discard);
}

// --- plafond : au-delà de 6 jalons => jetée, jamais « très longue » ---------
void test_12_ceiling_is_discarded_not_recorded(void) {
  OrpMixingObservation o;
  const float up[] = {501.f, 502.f, 503.f, 504.f, 505.f, 506.f, 507.f};
  OrpMixingTickVerdict v = runMarks(o, 0u, 500.f, up, 7);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kDiscarded, v.outcome);
  TEST_ASSERT_EQUAL(OrpMixingDiscard::kCeiling, v.discard);
  TEST_ASSERT_EQUAL_UINT32(0u, v.observedMs);
  // Sixième jalon plat : k=6 est ENCORE enregistrable (6W = plafond).
  OrpMixingObservation p;
  const float up6[] = {501.f, 502.f, 503.f, 504.f, 505.f, 505.f};
  v = runMarks(p, 0u, 500.f, up6, 6);
  TEST_ASSERT_EQUAL(OrpMixingOutcome::kClosed, v.outcome);
  TEST_ASSERT_EQUAL_UINT32(MAX, v.observedMs);
}

// --- Instantané NVS ---------------------------------------------------------
static OrpMixingSnapshot goodSnap() {
  return orpMixingSnapshotFromStore(storeOf(1 * W, 6 * W, 2 * W));
}

void test_13_snapshot_roundtrip(void) {
  const OrpMixingSnapshot s = goodSnap();
  TEST_ASSERT_TRUE(orpMixingSnapshotIntact(s, sizeof(s)));
  const OrpMixingLearnStore st = orpMixingStoreFromSnapshot(s);
  TEST_ASSERT_EQUAL_UINT8(3, st.count);
  TEST_ASSERT_EQUAL_UINT32(6 * W, orpMixingEffectiveDelayMs(st));
}

void test_14_snapshot_rejections_crc_schema_size(void) {
  OrpMixingSnapshot s = goodSnap();
  // CRC : un bit retourné dans les données
  OrpMixingSnapshot bad = s; bad.obsMs[1] ^= 1u;
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(bad, sizeof(bad)));
  // CRC : champ crc lui-même altéré
  bad = s; bad.crc ^= 1u;
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(bad, sizeof(bad)));
  // SCHÉMA : version différente mais CRC RECALCULÉ (seul le schéma peut refuser)
  bad = s; bad.schema = (uint8_t)(kOrpMixingLearnSchema + 1);
  bad.crc = orpMixingSnapshotCrc(bad);
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(bad, sizeof(bad)));
  // TAILLE : exacte exigée, ni plus courte ni plus longue
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(s, sizeof(s) - 1));
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(s, sizeof(s) + 1));
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(s, 0));
}

void test_15_absent_store_means_twenty_minutes(void) {
  // Appareil neuf : lecture NVS = 0 octet, struct à zéro. Rejeté, store par
  // défaut => EXACTEMENT 20 min, jamais une valeur indéterminée.
  OrpMixingSnapshot blank;
  memset(&blank, 0, sizeof(blank));
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(blank, 0));
  TEST_ASSERT_FALSE(orpMixingSnapshotIntact(blank, sizeof(blank)));  // schéma 0
  const OrpMixingLearnStore fresh;
  TEST_ASSERT_EQUAL_UINT32(W, orpMixingEffectiveDelayMs(fresh));
  // Octets réservés non normalisés : le scellement les remet à 0 (CRC stable)
  OrpMixingSnapshot a = goodSnap(); OrpMixingSnapshot b = a;
  b.reserved = 0xBEEF; orpMixingSealSnapshot(b);
  TEST_ASSERT_EQUAL_UINT32(a.crc, b.crc);
}

// --- A6 : le détecteur de sonde figée reste plus court que la pause EN VIGUEUR
void test_16_frozen_invariant_executed_on_effective_delay(void) {
  const uint32_t frozenMs = (uint32_t)kSensorFrozenSamples * (uint32_t)kPhOrpSensorIntervalMs;
  TEST_ASSERT_TRUE(orpMixingFrozenInvariantHolds(frozenMs));
  TEST_ASSERT_FALSE(orpMixingFrozenInvariantHolds(frozenMs - 1));
  TEST_ASSERT_TRUE(orpMixingFrozenInvariantHolds(orpMixingEffectiveDelayMs(OrpMixingLearnStore())));
  for (uint32_t k = 1; k <= 6; ++k) {
    TEST_ASSERT_TRUE(orpMixingFrozenInvariantHolds(orpMixingEffectiveDelayMs(storeOf(k * W, k * W, k * W))));
  }
}

// --- D7 : repli fail-closed armé à la durée APPRISE (> 60 min) --------------
void test_17_fail_closed_arms_learned_pause(void) {
  DosingWindowSourceInputs none; none.available = false;
  const uint32_t learned = 5 * W;  // 100 min > verrou horaire de 60 min
  const uint32_t nowMs = 55555u;
  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(none, none, learned, nowMs);
  TEST_ASSERT_EQUAL(DosingWindowSource::kFailClosed, v.source);
  TEST_ASSERT_EQUAL_UINT32(nowMs + learned, v.mixingEndMs);
}

// --- A7 : pause en cours au redémarrage => durée en vigueur À LA RESTAURATION
void test_18_pump_on_at_write_rearms_effective_delay(void) {
  DosingWindowSourceInputs t2;
  t2.available = true;
  t2.usedMs = 60000u; t2.windowElapsedAtWriteMs = 60000u;
  t2.cyclesDayElapsedAtWriteMs = 60000u; t2.cyclesToday = 1;
  t2.outageElapsedMs = 1000u; t2.unknownElapsed = false;
  t2.mixingRemainingMs = 1000u;
  t2.pumpOnAtWrite = true;
  DosingWindowSourceInputs none; none.available = false;
  const uint32_t learned = 4 * W;
  const uint32_t nowMs = 777777u;
  DosingWindowChannelVerdict v = dosingWindowResolveChannel(t2, none, learned, nowMs);
  TEST_ASSERT_EQUAL_UINT32(nowMs + learned, v.mixingEndMs);
  // Une autre durée en vigueur donne une autre échéance (pas une constante)
  v = dosingWindowResolveChannel(t2, none, 2 * W, nowMs);
  TEST_ASSERT_EQUAL_UINT32(nowMs + 2 * W, v.mixingEndMs);
}

// --- D8 : ordre d'appel dans main.cpp (sentinelle lexicale) -----------------
static int lineOf(const char* text, const char* stmt) {
  int line = 0;
  const char* p = text;
  while (*p) {
    const char* e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    const char* q = p;
    while (q < p + n && (*q == ' ' || *q == '\t')) ++q;
    if (!(q + 1 < p + n && q[0] == '/' && q[1] == '/') &&
        (size_t)(p + n - q) >= strlen(stmt) && strncmp(q, stmt, strlen(stmt)) == 0) {
      return line;
    }
    ++line;
    if (!e) break;
    p = e + 1;
  }
  return -1;
}

void test_19_main_loads_learned_value_before_dosing_window_begin(void) {
  FILE* f = fopen("src/main.cpp", "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "src/main.cpp illisible : sentinelle aveugle, corriger le chemin");
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char* txt = (char*)malloc((size_t)n + 1);
  size_t got = fread(txt, 1, (size_t)n, f); txt[got] = 0; fclose(f);
  const int learn = lineOf(txt, "PumpController.beginOrpMixingLearn();");
  const int setMs = lineOf(txt, "dosingWindow.setOrpFullMixingMs(");
  const int begin = lineOf(txt, "dosingWindow.begin();");
  const int pumpBegin = lineOf(txt, "PumpController.begin();");
  free(txt);
  TEST_ASSERT_TRUE_MESSAGE(learn >= 0 && setMs >= 0 && begin >= 0 && pumpBegin >= 0,
                           "appels introuvables dans main.cpp");
  TEST_ASSERT_TRUE_MESSAGE(begin < pumpBegin,
      "dosingWindow.begin() doit preceder PumpController.begin() : sans cela une pause apprise > 60 min n'est pas appliquee a la restauration");
  TEST_ASSERT_TRUE_MESSAGE(learn < setMs, "beginOrpMixingLearn doit preceder setOrpFullMixingMs");
  TEST_ASSERT_TRUE_MESSAGE(setMs < begin, "setOrpFullMixingMs doit preceder dosingWindow.begin()");
}

// --- Gate chimique : propreté de l'observation (câblage, sentinelle lexicale)
static char* readWholeFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char* t = (char*)malloc((size_t)n + 1);
  size_t got = fread(t, 1, (size_t)n, f); t[got] = 0; fclose(f);
  return t;
}

// Texte de l'appel `tickOrpMixingLearn((uint32_t)now,` (hors commentaires), jusqu'au ';'.
static char* tickCallText(const char* text) {
  const char* stmt = "tickOrpMixingLearn((uint32_t)now,";
  const char* p = text;
  while (*p) {
    const char* e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    const char* q = p;
    while (q < p + n && (*q == ' ' || *q == '\t')) ++q;
    if (strncmp(q, stmt, strlen(stmt)) == 0) {
      const char* semi = strchr(q, ';');
      if (!semi) return NULL;
      size_t len = (size_t)(semi - q);
      char* out = (char*)malloc(len + 1);
      memcpy(out, q, len); out[len] = 0;
      return out;
    }
    if (!e) break;
    p = e + 1;
  }
  return NULL;
}

void test_20_learning_cleanliness_excludes_calibration_and_stabilization(void) {
  char* txt = readWholeFile("src/pump_controller.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(txt, "src/pump_controller.cpp illisible : sentinelle aveugle, corriger le chemin");
  char* call = tickCallText(txt);
  free(txt);
  TEST_ASSERT_NOT_NULL_MESSAGE(call, "appel tickOrpMixingLearn introuvable (hors commentaires)");
  const bool cal = strstr(call, "!mqttCfg.calSessionOpen") != NULL;
  const bool stab = strstr(call, "!isStabilizationTimerActive(1)") != NULL;
  free(call);
  TEST_ASSERT_TRUE_MESSAGE(cal, "proprete : !mqttCfg.calSessionOpen manquant (echelle mV changee sous l'observation)");
  TEST_ASSERT_TRUE_MESSAGE(stab, "proprete : !isStabilizationTimerActive(1) manquant");
}

// --- C5 voie pH : le repli n'ajoute AUCUNE autorisation (pause < verrou horaire)
void test_21_ph_fail_closed_pause_expires_before_hour_lock(void) {
  DosingWindowSourceInputs none; none.available = false;
  const uint32_t nowMs = 55555u;
  const DosingWindowChannelVerdict v =
      dosingWindowResolveChannel(none, none, (uint32_t)kPhMixingDelayMs, nowMs);
  TEST_ASSERT_EQUAL_UINT32(nowMs + 900000u, v.mixingEndMs);
  // Le verrou horaire (usedMs = limite) tient jusqu'a windowStart + 1 h.
  TEST_ASSERT_EQUAL_UINT32(kDosingWindowHourMs, v.usedMs);
  const uint32_t lockEnd = v.windowStart + kDosingWindowHourMs;
  TEST_ASSERT_TRUE((int32_t)(lockEnd - v.mixingEndMs) > 0);  // pause expire AVANT le verrou
}

// --- Boot froid, store appris 120 min, aucune source : pause NON tronquee ---
void test_22_cold_boot_learned_120_min_not_truncated(void) {
  const OrpMixingLearnStore st = orpMixingStoreFromSnapshot(
      orpMixingSnapshotFromStore(storeOf(6 * W, 1 * W, 1 * W)));
  const uint32_t eff = orpMixingEffectiveDelayMs(st);
  TEST_ASSERT_EQUAL_UINT32(120u * 60u * 1000u, eff);
  DosingWindowSourceInputs none; none.available = false;
  const DosingWindowChannelVerdict v = dosingWindowResolveChannel(none, none, eff, 1000u);
  TEST_ASSERT_EQUAL_UINT32(1000u + 120u * 60u * 1000u, v.mixingEndMs);
  TEST_ASSERT_TRUE(v.mixingEndMs - 1000u > kDosingWindowHourMs);  // > 60 min
}

// --- Bornes de orpMixingEffectiveDelayMs ------------------------------------
void test_23_effective_delay_bounds(void) {
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOrpMixingDelayMs, orpMixingEffectiveDelayMs(storeOf(0, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kOrpMixingDelayMs, orpMixingEffectiveDelayMs(storeOf(0, 0, 0, 255)));
  const uint32_t big = (uint32_t)kOrpMixingDelayLearnedMaxMs + 1u;
  TEST_ASSERT_TRUE(orpMixingEffectiveDelayMs(storeOf(big, big, big)) <= (uint32_t)kOrpMixingDelayLearnedMaxMs);
  TEST_ASSERT_TRUE(orpMixingEffectiveDelayMs(storeOf(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 255))
                   <= (uint32_t)kOrpMixingDelayLearnedMaxMs);
  TEST_ASSERT_TRUE(orpMixingEffectiveDelayMs(storeOf(big, big, big)) >= (uint32_t)kOrpMixingDelayMs);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_01_zero_one_two_observations_give_exactly_floor);
  RUN_TEST(test_02_effective_is_maximum_not_median_nor_mean);
  RUN_TEST(test_03_floor_clamp_and_absurd_rejection);
  RUN_TEST(test_04_ring_keeps_last_three);
  RUN_TEST(test_05_observation_closes_at_k_times_window);
  RUN_TEST(test_06_flat_at_first_mark_gives_one_window);
  RUN_TEST(test_07_no_decision_before_the_mark_and_window_is_constant);
  RUN_TEST(test_08_idle_when_not_open);
  RUN_TEST(test_09_wrap_of_millis_is_safe);
  RUN_TEST(test_10_dirty_window_is_discarded);
  RUN_TEST(test_11_nan_measure_is_discarded);
  RUN_TEST(test_12_ceiling_is_discarded_not_recorded);
  RUN_TEST(test_13_snapshot_roundtrip);
  RUN_TEST(test_14_snapshot_rejections_crc_schema_size);
  RUN_TEST(test_15_absent_store_means_twenty_minutes);
  RUN_TEST(test_16_frozen_invariant_executed_on_effective_delay);
  RUN_TEST(test_17_fail_closed_arms_learned_pause);
  RUN_TEST(test_18_pump_on_at_write_rearms_effective_delay);
  RUN_TEST(test_19_main_loads_learned_value_before_dosing_window_begin);
  RUN_TEST(test_20_learning_cleanliness_excludes_calibration_and_stabilization);
  RUN_TEST(test_21_ph_fail_closed_pause_expires_before_hour_lock);
  RUN_TEST(test_22_cold_boot_learned_120_min_not_truncated);
  RUN_TEST(test_23_effective_delay_bounds);
  return UNITY_END();
}
