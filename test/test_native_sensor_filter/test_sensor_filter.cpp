// =============================================================================
// Tests unitaires natifs — SensorFilter (feature-025 → feature-058)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel Atlas EZO.
//
// NOUVEAU CONTRAT feature-058 (VALEUR ≠ DRAPEAU) — on teste le COMPORTEMENT
// observable via l'API publique, jamais l'implémentation :
//   * Chaîne : rejet aberrations (NaN / hors-plage / spike DUR |raw−médiane|>seuil)
//     → médiane fenêtre LONGUE (13) → EMA (α pH 0,10 / ORP 0,08).
//   * filtered() = EMA de la médiane-longue, JAMAIS figée ; NaN tant que la
//     fenêtre n'est pas PLEINE (13 échantillons).
//   * unstable() = latch EMI OU consecutiveRejects>=max OU dispersion>cap.
//     DÉCOUPLÉ de la valeur (ne fige jamais filtered()).
//   * dispersion() = max−min du buffer brut accepté (NaN si fenêtre non pleine).
//
// Config de référence calquée sur constants.h :
//   pH  : min 0  max 14   spike 1.0  α 0.10  window 13  maxRej 10  cap 0.5
//   ORP : min -1000 max 1500 spike 300 α 0.08 window 13 maxRej 10 cap 100
// =============================================================================

#include <unity.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include "sensor_filter.h"

// Ordre positionnel de SensorFilter::Config (cf. sensor_filter.h) :
//   minValue, maxValue, spikeReject, emaAlpha, centralWindow,
//   maxConsecutiveRejects, maxAgeMs, maxDispersion, [frozenSamples, frozenEpsilon]
static SensorFilter::Config phCfg() {
  return SensorFilter::Config{
      /*minValue*/ 0.0f,
      /*maxValue*/ 14.0f,
      /*spikeReject*/ 1.0f,
      /*emaAlpha*/ 0.10f,
      /*centralWindow*/ 13,
      /*maxConsecutiveRejects*/ 10,
      /*maxAgeMs*/ 20000,
      /*maxDispersion*/ 0.5f};
  // frozenSamples / frozenEpsilon value-initialisés à 0 → détection figé OFF.
}

static SensorFilter::Config orpCfg() {
  return SensorFilter::Config{
      -1000.0f, 1500.0f, 300.0f, 0.08f, 13, 10, 20000, 100.0f};
}

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// Verrou de régression — valeurs des constantes du nouveau contrat
// =============================================================================
void test_constants_values(void) {
  TEST_ASSERT_EQUAL_UINT8(13, kSensorFilterCentralWindow);
  TEST_ASSERT_EQUAL_UINT8(16, kSensorFilterCentralWindowMax);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, kPhEmaAlpha);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.08f, kOrpEmaAlpha);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, kPhFilterMaxDispersion);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 100.0f, kOrpFilterMaxDispersion);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, kPhFilterSpikeReject);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 300.0f, kOrpFilterSpikeReject);
  TEST_ASSERT_EQUAL_UINT8(3, kSensorFilterDispersionLatchCycles);
  TEST_ASSERT_EQUAL_UINT8(10, kSensorFilterMaxConsecutiveRejects);
  // Invariant : la fenêtre logique ne dépasse jamais la capacité physique.
  TEST_ASSERT_LESS_OR_EQUAL_UINT8(kSensorFilterCentralWindowMax, kSensorFilterCentralWindow);
}

// =============================================================================
// Warmup / ready / bases
// =============================================================================

// --- Warmup : NaN et non-ready tant que la fenêtre (13) n'est pas PLEINE ------
void test_warmup_not_ready_until_window_full(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  TEST_ASSERT_FALSE(f.ready(t));
  for (int i = 0; i < 12; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    TEST_ASSERT_TRUE_MESSAGE(isnan(f.filtered()), "filtered() doit rester NaN avant fenetre pleine");
    TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "ready() faux avant 13 echantillons");
    t += 1000;
  }
  // 13e échantillon → fenêtre pleine → amorçage filtered + ready true.
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));
  TEST_ASSERT_FALSE(isnan(f.filtered()));
  TEST_ASSERT_TRUE(f.ready(t));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.00f, f.filtered());
}

// --- ready() repasse false si mesure trop ancienne (EZO injoignable) ---------
void test_ready_false_when_stale(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(7.00f, t); t += 1000; }
  uint32_t last = t - 1000;
  TEST_ASSERT_TRUE(f.ready(last));
  // 21 s sans nouvelle mesure valide (> maxAgeMs 20 s) → plus prêt.
  TEST_ASSERT_FALSE(f.ready(last + 21000));
}

// --- Médiane sur fenêtre 13 ---------------------------------------------------
void test_median_window_13(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // 13 valeurs 6.94..7.06 (pas 0.01), désordonnées ; toutes dans une bande
  // largement < spikeReject → toutes acceptées. Triées → médiane = 7.00.
  float vals[13] = {7.00f, 7.06f, 6.94f, 7.03f, 6.97f, 7.01f, 6.99f,
                    7.02f, 6.98f, 7.05f, 6.95f, 7.04f, 6.96f};
  for (int i = 0; i < 13; ++i) {
    TEST_ASSERT_TRUE(f.addSample(vals[i], t));
    t += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.00f, f.median());
}

// --- Signal propre stable : filtered≈valeur, unstable false, ready true (T7) --
void test_clean_stable_signal(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 25; ++i) { f.addSample(7.20f, t); t += 1000; }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.20f, f.filtered());
  TEST_ASSERT_FALSE_MESSAGE(f.unstable(), "signal propre => stable");
  TEST_ASSERT_TRUE(f.ready(t - 1000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, f.dispersion());  // fenêtre plate
}

// --- EMA : convergence vers une valeur stable --------------------------------
void test_ema_converges_to_stable_value(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 60; ++i) { f.addSample(7.20f, t); t += 1000; }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.20f, f.filtered());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.20f, f.median());
}

// --- Warmup (T5) : fenêtre demi-pleine → filtered/dispersion NaN, pas de -------
//     jugement d'instabilité prématuré même si l'amplitude dépasserait le cap.
void test_warmup_half_full_no_premature_judgement(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // 6 échantillons alternant 7.2/7.8 (amplitude 0.6 > cap 0.5) mais fenêtre
  // NON pleine → dispersion NaN, aucun verdict d'instabilité.
  for (int i = 0; i < 6; ++i) {
    f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t);
    t += 1000;
  }
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.filtered()), "filtered NaN en warmup");
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.dispersion()), "dispersion NaN sur fenetre non pleine");
  TEST_ASSERT_FALSE_MESSAGE(f.unstable(), "pas de verdict d'instabilite premature");
  TEST_ASSERT_FALSE(f.ready(t));
}

// =============================================================================
// Tendance centrale (cœur feature-058)
// =============================================================================

// --- T1 : oscillation SYMÉTRIQUE dans le cap → filtered CENTRÉ, PAS enveloppe -
void test_symmetric_oscillation_converges_to_center_ph(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // pH alterne 7.3/7.7 (amplitude 0.4 < cap 0.5) autour de 7.5.
  for (int i = 0; i < 120; ++i) {
    TEST_ASSERT_TRUE(f.addSample((i % 2 == 0) ? 7.3f : 7.7f, t));
    t += 1000;
  }
  // Le lissé se cale au CENTRE (≈7.5), pas sur une enveloppe (7.3 ou 7.7).
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.10f, 7.50f, f.filtered(),
      "oscillation symetrique => valeur centree ~7.5");
  TEST_ASSERT_TRUE_MESSAGE(f.filtered() > 7.4f && f.filtered() < 7.6f,
      "filtered NE colle PAS a l'enveloppe");
  // Amplitude sous le cap → régulation NON inhibée.
  TEST_ASSERT_FALSE_MESSAGE(f.unstable(), "amplitude < cap => stable");
}

// --- T2 : oscillation amplitude > cap → unstable true, filtered reste centré --
void test_oscillation_over_cap_unstable_but_centered_ph(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // ±0.3 pH → 7.2/7.8, amplitude 0.6 > cap 0.5 : inhibition attendue.
  for (int i = 0; i < 40; ++i) {
    TEST_ASSERT_TRUE(f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t));
    t += 1000;
  }
  TEST_ASSERT_TRUE_MESSAGE(f.unstable(), "amplitude > cap => inhibition (unstable)");
  // MAIS la valeur n'est pas figée : elle reste l'estimateur central ~7.5.
  TEST_ASSERT_FALSE(isnan(f.filtered()));
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.10f, 7.50f, f.filtered(),
      "valeur reste centree malgre l'instabilite");
}

// --- T3 : spike DUR isolé → écarté, quasi aucun effet, PAS d'instabilité ------
void test_isolated_hard_spike_rejected_ph(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 1000; }
  float before = f.filtered();
  float dispBefore = f.dispersion();
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, before);
  // Spike +2.0 pH (|9.0−7.0|=2.0 > spike 1.0) → écarté.
  TEST_ASSERT_FALSE(f.addSample(9.00f, t));
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());          // valeur inchangée
  TEST_ASSERT_EQUAL_FLOAT(dispBefore, f.dispersion());    // dispersion non gonflée
  TEST_ASSERT_EQUAL_UINT8(0, f.consecutiveRejects());     // n'incrémente PAS l'instabilité
  TEST_ASSERT_FALSE(f.unstable());
  TEST_ASSERT_EQUAL_UINT8(1, f.rejectedCount());          // diagnostic uniquement
}

// --- Un saut LARGE mais < seuil de spike est ACCEPTÉ (oscillation légitime) ---
// Non-régression du diagnostic feature-057 : on ne rejette QUE l'aberration
// franche, jamais la transition d'oscillation soutenue.
void test_large_step_within_threshold_accepted(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE_MESSAGE(f.addSample(7.90f, t), "saut +0.9 (<1.0) doit etre accepte");
  t += 1000;
  TEST_ASSERT_TRUE_MESSAGE(f.addSample(6.10f, t), "saut -0.9 doit etre accepte");
}

// =============================================================================
// Aberrations / fail-closed
// =============================================================================

// --- Rejet NaN ----------------------------------------------------------------
void test_reject_nan(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(7.0f, t); t += 1000; }
  float before = f.filtered();
  TEST_ASSERT_FALSE(f.addSample(NAN, t));
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());
  TEST_ASSERT_EQUAL_UINT8(1, f.consecutiveRejects());  // NaN compte comme instabilité
}

// --- Rejet hors plage plausible ----------------------------------------------
void test_reject_out_of_range(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  TEST_ASSERT_FALSE(f.addSample(-1.0f, t));   // < 0
  TEST_ASSERT_FALSE(f.addSample(15.0f, t));   // > 14
  TEST_ASSERT_EQUAL_UINT8(2, f.rejectedCount());
  SensorFilter g(orpCfg());
  TEST_ASSERT_FALSE(g.addSample(-2000.0f, t));
  TEST_ASSERT_FALSE(g.addSample(2000.0f, t));
}

// --- T4a : rafale de garbage (NaN) sur filtre neuf → jamais ready, unstable ---
void test_garbage_burst_never_ready_ph(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_FALSE(f.addSample(NAN, t));
    t += 1000;
  }
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "fenetre jamais alimentee => fail-closed");
  TEST_ASSERT_TRUE_MESSAGE(f.unstable(), "rejets consecutifs => instable");
  TEST_ASSERT_TRUE(isnan(f.filtered()));
}

// --- T4b : spikes DURS répétés après stabilisation → âge grandit → non-ready --
void test_repeated_spikes_age_out_not_ready(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 15; ++i) { f.addSample(7.00f, t); t += 1000; }
  uint32_t lastValid = t - 1000;  // 14000
  TEST_ASSERT_TRUE(f.ready(lastValid));
  float before = f.filtered();
  // Spikes durs (9.5) : écartés, ne rafraîchissent PAS lastValid → l'âge grandit.
  for (uint32_t ts = 15000; ts <= 40000; ts += 1000) {
    TEST_ASSERT_FALSE(f.addSample(9.50f, ts));
  }
  TEST_ASSERT_EQUAL_UINT8(0, f.consecutiveRejects());  // le spike ne compte pas
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());       // valeur inchangée
  // Âge = 40000 − 14000 = 26000 > maxAgeMs 20000 → dosage fail-closed.
  TEST_ASSERT_FALSE_MESSAGE(f.ready(40000), "spikes repetes => stale => non pret");
}

// --- Instabilité : unstable() après maxConsecutiveRejects (10) hors-plage -----
void test_unstable_after_consecutive_rejects(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 15; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.unstable());
  // 9 rejets hors-plage consécutifs → pas encore instable.
  for (int i = 0; i < 9; ++i) { f.addSample(20.0f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.unstable());
  // 10e → instable.
  f.addSample(20.0f, t);
  TEST_ASSERT_TRUE(f.unstable());
  // Une mesure valide remet consecutiveRejects à 0 → stable de nouveau.
  f.addSample(7.00f, t);
  TEST_ASSERT_FALSE(f.unstable());
}

// =============================================================================
// Découplage valeur / drapeau (contrat central feature-058)
// =============================================================================

// --- T6 : latch EMI posé → filtered CONTINUE de bouger (jamais figée) ---------
void test_value_keeps_moving_despite_latch(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // Oscillation > cap jusqu'au latch collant.
  for (int i = 0; i < 25; ++i) { f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.unstableLatched());
  float centered = f.filtered();  // ~7.5
  // Malgré le latch, on pousse un nouveau palier à 8.0 (saut 0.5 < spike 1.0).
  for (int i = 0; i < 30; ++i) { f.addSample(8.00f, t); t += 1000; }
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "le latch reste pose (collant)");
  TEST_ASSERT_TRUE_MESSAGE(f.filtered() > centered + 0.20f,
      "filtered doit suivre le nouveau palier, PAS rester figee sur latch");
  TEST_ASSERT_FLOAT_WITHIN(0.10f, 8.00f, f.filtered());
}

// --- Le latch dispersion est COLLANT : levé uniquement par reset() ------------
void test_dispersion_latch_sticky_until_reset(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 25; ++i) { f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.unstableLatched());
  // 40 mesures parfaitement stables : dispersion retombe à 0, MAIS latch persiste.
  for (int i = 0; i < 40; ++i) { f.addSample(7.50f, t); t += 1000; }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, f.dispersion());
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "latch persiste malgre dispersion nulle");
  TEST_ASSERT_TRUE(f.unstable());
  // Seul reset() lève le latch.
  f.reset();
  TEST_ASSERT_FALSE(f.unstableLatched());
  TEST_ASSERT_FALSE(f.unstable());
}

// =============================================================================
// ORP (cap 100 mV, spike 300 mV) — mêmes propriétés (T8)
// =============================================================================

void test_symmetric_oscillation_converges_to_center_orp(void) {
  SensorFilter f(orpCfg());
  uint32_t t = 0;
  // 640/720 autour de 680, amplitude 80 < cap 100.
  for (int i = 0; i < 120; ++i) {
    TEST_ASSERT_TRUE(f.addSample((i % 2 == 0) ? 640.0f : 720.0f, t));
    t += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(15.0f, 680.0f, f.filtered(), "ORP centre ~680");
  TEST_ASSERT_FALSE_MESSAGE(f.unstable(), "amplitude 80 < cap 100 => stable");
}

void test_oscillation_over_cap_unstable_but_centered_orp(void) {
  SensorFilter f(orpCfg());
  uint32_t t = 0;
  // 620/740, amplitude 120 > cap 100.
  for (int i = 0; i < 40; ++i) {
    TEST_ASSERT_TRUE(f.addSample((i % 2 == 0) ? 620.0f : 740.0f, t));
    t += 1000;
  }
  TEST_ASSERT_TRUE_MESSAGE(f.unstable(), "amplitude 120 > cap 100 => instable");
  TEST_ASSERT_FALSE(isnan(f.filtered()));
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(15.0f, 680.0f, f.filtered(), "ORP reste centre");
}

void test_isolated_hard_spike_rejected_orp(void) {
  SensorFilter f(orpCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(680.0f, t); t += 1000; }
  float before = f.filtered();
  // Spike +400 mV (|1080−680|=400 > spike 300) → écarté.
  TEST_ASSERT_FALSE(f.addSample(1080.0f, t));
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());
  TEST_ASSERT_EQUAL_UINT8(0, f.consecutiveRejects());
  // Un +200 mV (< spike 300) est en revanche ACCEPTÉ (transition légitime).
  TEST_ASSERT_TRUE(f.addSample(880.0f, t));
}

// =============================================================================
// Divers / couverture
// =============================================================================

// --- reset() → re-warmup complet ---------------------------------------------
void test_reset_triggers_rewarmup(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(7.0f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.ready(t - 1000));
  f.reset();
  TEST_ASSERT_FALSE(f.ready(t));
  TEST_ASSERT_TRUE(isnan(f.filtered()));
  TEST_ASSERT_TRUE(isnan(f.dispersion()));
  TEST_ASSERT_EQUAL_UINT8(0, f.rejectedCount());
  for (int i = 0; i < 12; ++i) { f.addSample(7.0f, t); t += 1000; TEST_ASSERT_FALSE(f.ready(t)); }
  f.addSample(7.0f, t);
  TEST_ASSERT_TRUE(f.ready(t));
}

// --- raw() reflète le dernier brut, même rejeté (spike dur) -------------------
void test_raw_reflects_last_sample_even_if_rejected(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));
  TEST_ASSERT_EQUAL_FLOAT(7.00f, f.raw());
  for (int i = 0; i < 13; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.addSample(9.50f, t));  // spike dur rejeté
  TEST_ASSERT_EQUAL_FLOAT(9.50f, f.raw());
}

// --- dispersion() NaN tant que la fenêtre n'est pas pleine, valeur ensuite -----
void test_dispersion_nan_until_window_full(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 12; ++i) {
    f.addSample(7.00f + 0.02f * (i % 2), t);  // léger va-et-vient
    t += 1000;
    TEST_ASSERT_TRUE_MESSAGE(isnan(f.dispersion()), "dispersion NaN < fenetre pleine");
  }
  f.addSample(7.00f, t);
  TEST_ASSERT_FALSE_MESSAGE(isnan(f.dispersion()), "dispersion definie a fenetre pleine");
}

// --- centralWindow > capacité (99) clampé à kSensorFilterCentralWindowMax (16) -
void test_central_window_clamped_to_capacity(void) {
  SensorFilter::Config cfg = phCfg();
  cfg.centralWindow = 99;  // > 16 → clampé
  SensorFilter f(cfg);
  uint32_t t = 0;
  // Il faut désormais 16 échantillons pour remplir la fenêtre.
  for (int i = 0; i < 15; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "pas pret avant 16 echantillons (fenetre clampee)");
    t += 1000;
  }
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));  // 16e
  TEST_ASSERT_TRUE(f.ready(t));
  // Pas de débordement au-delà de la capacité.
  for (int i = 0; i < 20; ++i) { TEST_ASSERT_TRUE(f.addSample(7.00f, t)); t += 1000; }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, f.median());
}

// =============================================================================
// Détection capteur figé — FrozenDetector + intégration (feature-022)
// =============================================================================
// Config pH avec détection ACTIVÉE : frozenSamples 30, frozenEpsilon 0.0005.
static SensorFilter::Config frozenPhCfg() {
  SensorFilter::Config cfg = phCfg();
  cfg.frozenSamples = kSensorFrozenSamples;    // 30
  cfg.frozenEpsilon = kSensorFrozenEpsilonPh;  // 0.0005
  return cfg;
}

void test_frozen_constants_values(void) {
  TEST_ASSERT_EQUAL_UINT16(30, kSensorFrozenSamples);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0005f, kSensorFrozenEpsilonPh);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.05f, kSensorFrozenEpsilonOrp);
}

// --- 30 lectures identiques → figé ET ready false (fenêtre déjà pleine à 13) ---
void test_frozen_30_identical_readings_blocks_ready(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 29; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    TEST_ASSERT_FALSE(f.frozen());
    t += 1000;
  }
  TEST_ASSERT_TRUE(f.ready(t - 1000));  // fenêtre pleine, pas encore figé
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));
  TEST_ASSERT_TRUE_MESSAGE(f.frozen(), "30 lectures identiques => capteur fige");
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "fige => ready false (fail-closed)");
}

// --- 29 identiques + 1 toggle de 1 LSB (0.001) → jamais figé ------------------
void test_frozen_one_lsb_toggle_breaks_run(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 29; ++i) { f.addSample(7.000f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.frozen());
  TEST_ASSERT_TRUE(f.addSample(7.001f, t));  // 0.001 > epsilon 0.0005 → ré-ancre
  t += 1000;
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(), "un toggle 1 LSB casse le run");
  for (int i = 0; i < 28; ++i) { f.addSample(7.001f, t); t += 1000; TEST_ASSERT_FALSE(f.frozen()); }
  TEST_ASSERT_TRUE(f.ready(t - 1000));
}

// --- Fenêtre figé non pleine (29) → pas figé ----------------------------------
void test_frozen_window_not_full_29_samples(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 29; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.frozen());
  TEST_ASSERT_TRUE(f.ready(t - 1000));
}

// --- Sortie d'état figé immédiate (1 lecture vivante) sans re-warmup -----------
void test_frozen_exit_immediate_no_rewarmup(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  TEST_ASSERT_FALSE(f.ready(t - 1000));
  // 7.01 : hors bande figé (>epsilon) mais << spike → acceptée. Fenêtre déjà pleine.
  TEST_ASSERT_TRUE(f.addSample(7.01f, t));
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(), "sortie immediate des la 1re lecture vivante");
  TEST_ASSERT_TRUE_MESSAGE(f.ready(t), "ready() true au meme tick (pas de re-warmup)");
}

// --- Frozen PERSISTE pendant une rafale de rejets (aucun n'alimente le détecteur)
void test_frozen_persists_through_reject_burst(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  // Rafale : NaN, hors-plage (15.0), spike dur (9.5). Aucun n'alimente le détecteur.
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_FALSE(f.addSample(NAN, t));   t += 1000; TEST_ASSERT_TRUE(f.frozen());
    TEST_ASSERT_FALSE(f.addSample(15.0f, t)); t += 1000; TEST_ASSERT_TRUE(f.frozen());
    TEST_ASSERT_FALSE(f.addSample(9.50f, t)); t += 1000;
    TEST_ASSERT_TRUE_MESSAGE(f.frozen(), "frozen persiste pendant la rafale de rejets");
  }
  TEST_ASSERT_FALSE(f.ready(t));
}

// --- reset() lève l'état figé -------------------------------------------------
void test_frozen_cleared_by_reset(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  f.reset();
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(), "reset() leve l'etat fige");
  for (int i = 0; i < 29; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.frozen());
}

// --- FrozenDetector : étendue == epsilon → run cassé (comparaison stricte <) ---
void test_frozen_detector_extent_equal_epsilon_breaks_run(void) {
  FrozenDetector d(2, 0.5f);
  d.addSample(100.0f);
  d.addSample(100.5f);  // étendue == epsilon → PAS < → ré-ancrage
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "etendue == epsilon casse le run");
  FrozenDetector d2(2, 0.5f);
  d2.addSample(100.0f);
  d2.addSample(100.25f);  // 0.25 < 0.5 → figé
  TEST_ASSERT_TRUE(d2.frozen());
}

// --- Non-régression : config héritée (frozen désactivé) → jamais figé ---------
void test_frozen_disabled_with_legacy_config(void) {
  SensorFilter f(phCfg());  // frozenSamples value-initialisé à 0
  uint32_t t = 0;
  for (int i = 0; i < 100; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    TEST_ASSERT_FALSE(f.frozen());
    t += 1000;
  }
  TEST_ASSERT_TRUE(f.ready(t - 1000));
}

// --- FrozenDetector samples=0 → détection désactivée --------------------------
void test_frozen_detector_samples_zero_never_frozen(void) {
  FrozenDetector d(0, 0.0005f);
  for (int i = 0; i < 1000; ++i) d.addSample(7.00f);
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "samples=0 desactive la detection");
}

// --- FrozenDetector : compteur sature, pas de wrap au-delà de 65535 -----------
void test_frozen_detector_run_count_saturates_no_overflow(void) {
  FrozenDetector d(30, 0.5f);
  for (int i = 0; i < 70000; ++i) {
    d.addSample(100.0f);
    if (i >= 29) TEST_ASSERT_TRUE_MESSAGE(d.frozen(), "le compteur doit saturer, pas wrapper");
  }
  TEST_ASSERT_TRUE(d.frozen());
  d.reset();
  TEST_ASSERT_FALSE(d.frozen());
}

int main(int, char **) {
  UNITY_BEGIN();
  // Verrou constantes.
  RUN_TEST(test_constants_values);
  // Warmup / ready / bases.
  RUN_TEST(test_warmup_not_ready_until_window_full);
  RUN_TEST(test_ready_false_when_stale);
  RUN_TEST(test_median_window_13);
  RUN_TEST(test_clean_stable_signal);
  RUN_TEST(test_ema_converges_to_stable_value);
  RUN_TEST(test_warmup_half_full_no_premature_judgement);
  // Tendance centrale (feature-058).
  RUN_TEST(test_symmetric_oscillation_converges_to_center_ph);
  RUN_TEST(test_oscillation_over_cap_unstable_but_centered_ph);
  RUN_TEST(test_isolated_hard_spike_rejected_ph);
  RUN_TEST(test_large_step_within_threshold_accepted);
  // Aberrations / fail-closed.
  RUN_TEST(test_reject_nan);
  RUN_TEST(test_reject_out_of_range);
  RUN_TEST(test_garbage_burst_never_ready_ph);
  RUN_TEST(test_repeated_spikes_age_out_not_ready);
  RUN_TEST(test_unstable_after_consecutive_rejects);
  // Découplage valeur / drapeau.
  RUN_TEST(test_value_keeps_moving_despite_latch);
  RUN_TEST(test_dispersion_latch_sticky_until_reset);
  // ORP.
  RUN_TEST(test_symmetric_oscillation_converges_to_center_orp);
  RUN_TEST(test_oscillation_over_cap_unstable_but_centered_orp);
  RUN_TEST(test_isolated_hard_spike_rejected_orp);
  // Divers / couverture.
  RUN_TEST(test_reset_triggers_rewarmup);
  RUN_TEST(test_raw_reflects_last_sample_even_if_rejected);
  RUN_TEST(test_dispersion_nan_until_window_full);
  RUN_TEST(test_central_window_clamped_to_capacity);
  // Détection capteur figé (feature-022).
  RUN_TEST(test_frozen_constants_values);
  RUN_TEST(test_frozen_30_identical_readings_blocks_ready);
  RUN_TEST(test_frozen_one_lsb_toggle_breaks_run);
  RUN_TEST(test_frozen_window_not_full_29_samples);
  RUN_TEST(test_frozen_exit_immediate_no_rewarmup);
  RUN_TEST(test_frozen_persists_through_reject_burst);
  RUN_TEST(test_frozen_cleared_by_reset);
  RUN_TEST(test_frozen_detector_extent_equal_epsilon_breaks_run);
  RUN_TEST(test_frozen_disabled_with_legacy_config);
  RUN_TEST(test_frozen_detector_samples_zero_never_frozen);
  RUN_TEST(test_frozen_detector_run_count_saturates_no_overflow);
  return UNITY_END();
}
