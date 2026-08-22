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

// -----------------------------------------------------------------------------
// v2.20.3 — configs AVEC sortie de secours ARMÉE (bug-filtre-ph-verrouille)
// -----------------------------------------------------------------------------
// ATTENTION : les configs ci-dessus (init agrégat courte) laissent rearmCoherence
// à 0, ce qui DÉSACTIVE la sortie de secours (rétrocompatibilité voulue). Tout
// test du correctif DOIT donc renseigner explicitement les 3 champs de fin, sinon
// il exercerait un filtre où le correctif est INERTE et passerait au vert pour une
// mauvaise raison. Ces helpers reprennent les valeurs de production (constants.h,
// arbitrage pool-chemistry) : cohérence 0,20 pH / 40 mV, bande admissible
// [6,50 ; 8,20] pH et [300 ; 950] mV.
static SensorFilter::Config phCfgRearm() {
  SensorFilter::Config cfg = phCfg();
  cfg.rearmCoherence = kPhFilterRearmCoherence;
  cfg.rearmMin = kPhFilterRearmMin;
  cfg.rearmMax = kPhFilterRearmMax;
  return cfg;
}

static SensorFilter::Config orpCfgRearm() {
  SensorFilter::Config cfg = orpCfg();
  cfg.rearmCoherence = kOrpFilterRearmCoherence;
  cfg.rearmMin = kOrpFilterRearmMin;
  cfg.rearmMax = kOrpFilterRearmMax;
  return cfg;
}

// Cadence de lecture réelle des EZO (5 s) : rend les bornes de TEMPS assertables.
static const uint32_t kDt = 5000;

// Ouvre la porte d'alimentation comme en production : front montant d'eau, puis
// plancher de gel interne kSensorFilterGateFloorMs écoulé. Draine le drapeau de
// ré-amorçage émis par cette première ouverture.
static void openGate(SensorFilter& f, uint32_t& t) {
  f.setContext(true, true, t);              // front montant → plancher armé
  t += kSensorFilterGateFloorMs;
  f.setContext(true, true, t);              // plancher écoulé → porte ouverte
  f.rearmedSinceLastPoll();
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
  TEST_ASSERT_EQUAL_UINT8(12, kSensorFilterLatchClearCycles);  // v2.8.4 auto-levée
  TEST_ASSERT_EQUAL_UINT8(10, kSensorFilterMaxConsecutiveRejects);
  // Asymétrie voulue : rapide à poser (3), lent à lever (12).
  TEST_ASSERT_GREATER_THAN_UINT8(kSensorFilterDispersionLatchCycles, kSensorFilterLatchClearCycles);
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

// --- T4b : spikes DURS répétés mais INCOHÉRENTS → âge grandit → non-ready -----
//
// RÉARBITRÉ en v2.20.3 (spec bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue).
// La version d'origine injectait 26 spikes **identiques** à 9,50 et exigeait que le
// filtre reste non prêt indéfiniment : elle encodait très exactement le défaut que
// l'on corrige. 26 rejets identiques sont PARFAITEMENT COHÉRENTS entre eux, donc
// ils décrivent une NOUVELLE valeur de la grandeur, pas du bruit — c'est la
// situation qui a laissé le bassin sans aucune régulation ≥ 3 h en production.
// La décision `pool-chemistry` de la sortie de secours (kSensorFilterSpikeRearmCount
// = 12 rejets consécutifs contenus dans une bande <= rearmCoherence) rend cette
// exigence CADUQUE : elle est donc réécrite, PAS supprimée ni affaiblie.
//
// Ce que le cas « reste non prêt » doit désormais prouver : des spikes MUTUELLEMENT
// INCOHÉRENTS (du vrai bruit) ne doivent JAMAIS ouvrir la sortie de secours, et le
// filtre doit rester fail-closed par vieillissement, exactement comme avant.
// La contrepartie (spikes cohérents ⇒ ré-amorçage BORNÉ) est couverte par
// test_incident_replay_sustained_step_recovers_bounded_ph.
//
// Choix des valeurs : médiane 6,60 ; spikes alternés 7,70 / 8,00. Les deux sont
// > spikeReject (1,0) de la médiane ET dans la bande admissible [6,50 ; 8,20] :
// la SEULE chose qui empêche le ré-amorçage est leur incohérence mutuelle
// (0,30 > 0,20). rearmDenied() == 0 le prouve — la garde de bande n'est même
// jamais consultée, la série n'atteignant jamais 12.
// Config phCfgRearm() (et non phCfg()) : avec rearmCoherence = 0 la sortie de
// secours serait INERTE et ce test passerait pour une mauvaise raison.
void test_repeated_spikes_age_out_not_ready(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 15; ++i) { f.addSample(6.60f, t); t += 1000; }
  uint32_t lastValid = t - 1000;  // 14000
  TEST_ASSERT_TRUE(f.ready(lastValid));
  float before = f.filtered();
  // Spikes durs INCOHÉRENTS : écartés, ne rafraîchissent PAS lastValid → âge ↗.
  int n = 0;
  for (uint32_t ts = 15000; ts <= 40000; ts += 1000, ++n) {
    TEST_ASSERT_FALSE(f.addSample((n % 2 == 0) ? 7.70f : 8.00f, ts));
  }
  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(kSensorFilterSpikeRearmCount, n,
      "le scenario doit depasser N=12 rejets, sinon il ne prouve rien");
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "des spikes incoherents ne doivent JAMAIS ouvrir la sortie de secours");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.rearmDenied(),
      "serie jamais complete => aucune garde consultee, aucun refus a alarmer");
  TEST_ASSERT_EQUAL_UINT8(0, f.consecutiveRejects());  // le spike ne compte pas
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());       // valeur inchangée
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.60f, f.median()); // médiane inchangée
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
  // Oscillation > cap jusqu'au latch collant (centre ~7.5).
  for (int i = 0; i < 25; ++i) { f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.unstableLatched());
  float centered = f.filtered();  // ~7.5
  // Nouveau palier oscillant CENTRÉ sur 8.0 (7.7/8.3, amplitude 0.6 > cap 0.5) : le
  // signal reste bruité → la dispersion demeure > cap → PAS d'auto-levée (v2.8.4),
  // MAIS filtered() suit la remontée du centre (VALEUR découplée du DRAPEAU).
  for (int i = 0; i < 60; ++i) { f.addSample((i % 2 == 0) ? 7.7f : 8.3f, t); t += 1000; }
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "signal encore bruite => latch reste pose");
  TEST_ASSERT_TRUE_MESSAGE(f.filtered() > centered + 0.20f,
      "filtered doit suivre le nouveau palier, PAS rester figee sur latch");
  TEST_ASSERT_FLOAT_WITHIN(0.15f, 8.00f, f.filtered());
}

// --- Pas d'auto-levée tant que le bruit EMI PERSISTE ; reset() force la levée --
// (v2.8.4 : le latch n'est plus collant à vie — il s'auto-lève après N fenêtres
// PROPRES — mais tant que la dispersion reste > cap, la clean streak ne monte
// jamais → aucune levée intempestive. Non-régression du chemin reset().)
void test_latch_persists_while_noise_persists(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 25; ++i) { f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.unstableLatched());
  // Bruit maintenu (amplitude > cap) sur 60 fenêtres : la clean streak reste à 0.
  for (int i = 0; i < 60; ++i) { f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.dispersionCleanStreak(),
      "bruit persistant => clean streak jamais accumulee");
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "pas d'auto-levee tant que le bruit persiste");
  TEST_ASSERT_TRUE(f.unstable());
  // reset() force la levée immédiate.
  f.reset();
  TEST_ASSERT_FALSE(f.unstableLatched());
  TEST_ASSERT_FALSE(f.unstable());
}

// =============================================================================
// Auto-levée du latch EMI (feature v2.8.4)
// =============================================================================
// Contrat pool-chemistry : latch posé rapide (3 fenêtres en dépassement), levé
// lent (kSensorFilterLatchClearCycles=12 fenêtres PLEINES PROPRES consécutives —
// dispersion <= cap ET AUCUN rejet). TOUT rejet (NaN / hors-plage / spike dur)
// OU toute fenêtre en dépassement remet la clean streak à 0 (anti-flapping).

// Pose le latch EMI par oscillation pH 7.2/7.8 (amplitude 0.6 > cap 0.5).
// Borne de sécurité 40 échantillons ; met à jour t (référence).
static void setPhLatch(SensorFilter& f, uint32_t& t) {
  for (int i = 0; i < 40 && !f.unstableLatched(); ++i) {
    f.addSample((i % 2 == 0) ? 7.2f : 7.8f, t);
    t += 1000;
  }
}

// Injecte un signal stable 7.50 jusqu'à ce que la clean streak atteigne `target`
// (bornée à 100 itérations : purge de la fenêtre de 13 + montée de la streak).
static void injectCleanUntilStreak(SensorFilter& f, uint32_t& t, uint8_t target) {
  int guard = 0;
  while (f.dispersionCleanStreak() < target && guard < 100) {
    f.addSample(7.50f, t);
    t += 1000;
    ++guard;
  }
}

// --- 1) Set puis auto-levée après exactement 12 fenêtres propres --------------
void test_latch_auto_clears_after_clean_windows(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  setPhLatch(f, t);
  TEST_ASSERT_TRUE_MESSAGE(f.unstable(), "oscillation > cap => unstable");
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "3 fenetres en depassement => latch pose");

  // La fenêtre de 13 doit d'abord se purger des valeurs d'oscillation avant que la
  // dispersion retombe <= cap ; la clean streak ne compte qu'ensuite.
  injectCleanUntilStreak(f, t, kSensorFilterLatchClearCycles - 1);  // 11
  TEST_ASSERT_EQUAL_UINT8(kSensorFilterLatchClearCycles - 1, f.dispersionCleanStreak());
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "11 fenetres propres => pas encore leve");

  // 12ᵉ fenêtre propre consécutive → auto-levée.
  TEST_ASSERT_TRUE(f.addSample(7.50f, t)); t += 1000;
  TEST_ASSERT_FALSE_MESSAGE(f.unstableLatched(), "12 fenetres propres => latch auto-leve");
  TEST_ASSERT_FALSE_MESSAGE(f.unstable(), "latch leve + dispersion nulle => stable");
}

// --- 2) Anti-flap : un spike dur au 11ᵉ propre remet la streak à 0 ------------
void test_latch_clear_streak_reset_by_hard_spike(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  setPhLatch(f, t);
  TEST_ASSERT_TRUE(f.unstableLatched());

  injectCleanUntilStreak(f, t, 11);
  TEST_ASSERT_EQUAL_UINT8(11, f.dispersionCleanStreak());
  TEST_ASSERT_TRUE(f.unstableLatched());

  // Spike DUR (|9.50 − médiane ~7.50| = 2.0 > spike 1.0) → écarté, casse la streak.
  TEST_ASSERT_FALSE(f.addSample(9.50f, t)); t += 1000;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.dispersionCleanStreak(),
      "un spike dur ecarte doit remettre la clean streak a 0");
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "latch toujours pose apres spike");

  // Le buffer est resté propre (spike non intégré) : il faut de nouveau 12 propres.
  injectCleanUntilStreak(f, t, 11);
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "11 propres apres spike => encore pose");
  TEST_ASSERT_TRUE(f.addSample(7.50f, t)); t += 1000;
  TEST_ASSERT_FALSE_MESSAGE(f.unstableLatched(), "12 propres consecutives apres spike => leve");
}

// --- 3) NaN / hors-plage pendant la streak → remise à 0 (latch conservé) ------
void test_latch_clear_streak_reset_by_nan_and_out_of_range(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  setPhLatch(f, t);

  injectCleanUntilStreak(f, t, 5);
  TEST_ASSERT_EQUAL_UINT8(5, f.dispersionCleanStreak());
  TEST_ASSERT_FALSE(f.addSample(NAN, t)); t += 1000;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.dispersionCleanStreak(), "NaN casse la streak");
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "latch conserve apres NaN");

  injectCleanUntilStreak(f, t, 5);
  TEST_ASSERT_EQUAL_UINT8(5, f.dispersionCleanStreak());
  TEST_ASSERT_FALSE(f.addSample(20.0f, t)); t += 1000;  // > maxValue 14
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.dispersionCleanStreak(), "hors-plage casse la streak");
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "latch conserve apres hors-plage");
}

// --- 4) Fenêtre non pleine → aucun incrément de clean streak ------------------
void test_clean_streak_only_counts_full_windows(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // Fenêtre de 13 en cours de remplissage : pas de dispersion → pas d'incrément.
  for (int i = 0; i < 12; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.50f, t));
    t += 1000;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.dispersionCleanStreak(),
        "aucun increment sur fenetre non pleine");
    TEST_ASSERT_FALSE(f.unstableLatched());
  }
  // 13ᵉ → fenêtre pleine et propre → streak = 1.
  TEST_ASSERT_TRUE(f.addSample(7.50f, t)); t += 1000;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, f.dispersionCleanStreak(),
      "premiere fenetre pleine propre => streak 1");
}

// --- 5) reset() = levée immédiate + clean streak à 0 --------------------------
void test_latch_cleared_immediately_by_reset(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  setPhLatch(f, t);
  injectCleanUntilStreak(f, t, 4);  // streak partielle en cours
  TEST_ASSERT_TRUE(f.unstableLatched());
  TEST_ASSERT_EQUAL_UINT8(4, f.dispersionCleanStreak());
  f.reset();
  TEST_ASSERT_FALSE_MESSAGE(f.unstableLatched(), "reset() leve le latch immediatement");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.dispersionCleanStreak(), "reset() remet la clean streak a 0");
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

// =============================================================================
// v2.20.3 — PORTE D'ALIMENTATION + SORTIE DE SECOURS
// (spec bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue)
// =============================================================================
// Rappel du défaut corrigé : la médiane ne pouvant évoluer QUE via des
// échantillons acceptés, un échelon LÉGITIME durable au-delà de spikeReject
// verrouillait le filtre à vie (ready() = false ⇒ dosage pH ET chlore à l'arrêt,
// ≥ 3 h en production, seul reset() en sortait).
//
// TOUS les tests de cette section utilisent phCfgRearm()/orpCfgRearm() : avec une
// init agrégat courte (rearmCoherence = 0) la sortie de secours est INERTE.

// --- T1 (CRITÈRE D'ACCEPTATION CENTRAL) : rejeu de l'incident de production ----
// Valeurs réelles relevées sur /debug/sensor_filter_state (firmware 2.20.0) :
// médiane interne 4,793 (offset ADR-0039 déduit) face à un brut 6,957 → écart
// 2,164 pH, très au-dessus de kPhFilterSpikeReject = 1,0.
// Ce qui est asserté est la BORNE de récupération, pas seulement l'état final :
// 12 rejets cohérents (sortie de secours) + 13 échantillons de warmup = 25.
void test_incident_replay_sustained_step_recovers_bounded_ph(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;

  // 1) Médiane établie à 4,79 (eau stagnante acide près du point d'injection).
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(4.79f, t)); t += kDt; }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.79f, f.median());
  TEST_ASSERT_TRUE(f.ready(t - kDt));

  // 2) Échelon SOUTENU à 6,96 : chaque échantillon est écarté comme « spike dur »…
  //    jusqu'à ce que la sortie de secours constate qu'ils décrivent tous la MÊME
  //    nouvelle valeur.
  const int kBound = (int)kSensorFilterSpikeRearmCount + (int)kSensorFilterCentralWindow;
  int samples = 0;
  bool recovered = false;
  for (int i = 0; i < 200; ++i) {
    f.addSample(6.96f, t);
    ++samples;
    // Critère de récupération : prêt ET l'estimateur SUIT la nouvelle valeur.
    // ready() seul ne suffirait pas — pendant les premiers rejets le filtre reste
    // « prêt » sur la médiane PÉRIMÉE 4,79 tant que maxAgeMs (20 s) n'est pas
    // écoulé. C'est le comportement antérieur, inchangé, et c'est précisément ce
    // qui rendait l'incident indolore vu du drapeau de disponibilité.
    if (f.ready(t) && fabsf(f.filtered() - 6.96f) <= 0.05f) { recovered = true; break; }
    if (samples == (int)kSensorFilterSpikeRearmCount) {
      // Au 12ᵉ rejet, la sortie de secours vient de vider l'estimateur : le filtre
      // est explicitement NON PRÊT (warmup) → dosage fail-closed pendant la reprise.
      TEST_ASSERT_TRUE_MESSAGE(isnan(f.filtered()),
          "re-amorcage au 12e rejet => estimateur vide, dosage fail-closed");
      TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "warmup en cours => non pret");
    }
    t += kDt;
  }
  TEST_ASSERT_TRUE_MESSAGE(recovered,
      "le filtre DOIT redevenir pret ET suivre : le verrouillage a vie est le bug corrige");
  TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(kBound, samples,
      "recuperation BORNEE : 12 rejets coherents + 13 echantillons de warmup");
  TEST_ASSERT_EQUAL_INT_MESSAGE(25, samples,
      "borne exacte attendue (verrou de regression sur le cout de la reprise)");

  // 3) Et il SUIT désormais la grandeur : la médiane de stagnation est oubliée.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 6.96f, f.filtered(),
      "apres re-amorcage le filtre suit la valeur reelle");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 6.96f, f.median());
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
      "un re-amorcage a bien eu lieu (drapeau a journaliser par la coquille)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.rearmDenied(), "aucune garde n'a refuse");
}

// --- T1 miroir ORP : même piège, logique partagée (spikeReject 300 mV) --------
void test_incident_replay_sustained_step_recovers_bounded_orp(void) {
  SensorFilter f(orpCfgRearm());
  uint32_t t = 0;
  // Médiane figée à 250 mV (eau stagnante), échelon soutenu à 620 mV : écart
  // 370 mV > 300 mV → verrouillage sans le correctif.
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(250.0f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));

  const int kBound = (int)kSensorFilterSpikeRearmCount + (int)kSensorFilterCentralWindow;
  int samples = 0;
  bool recovered = false;
  for (int i = 0; i < 200; ++i) {
    f.addSample(620.0f, t);
    ++samples;
    if (f.ready(t) && fabsf(f.filtered() - 620.0f) <= 5.0f) { recovered = true; break; }
    t += kDt;
  }
  TEST_ASSERT_TRUE_MESSAGE(recovered, "filtre ORP : meme sortie de secours");
  TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(kBound, samples, "recuperation ORP bornee");
  TEST_ASSERT_EQUAL_INT_MESSAGE(25, samples, "borne exacte ORP : 12 rejets + 13 warmup");
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 620.0f, f.filtered());
}

// --- T2 : transition purge → valide, la médiane de stagnation ne fait PAS -----
//     autorité sur les mesures représentatives suivantes.
void test_stagnation_median_never_authoritative_ph(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  openGate(f, t);

  // Régime établi, eau en circulation : filtre convergé sur 6,80.
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(6.80f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));
  const float medBefore = f.median();

  // Arrêt de filtration : la coquille ferme la porte (eau absente).
  f.setContext(false, true, t);
  TEST_ASSERT_TRUE_MESSAGE(f.gateClosed(), "eau absente => porte fermee");

  // 40 échantillons d'eau STAGNANTE très acide (4,79) : AUCUN ne doit entrer.
  for (int i = 0; i < 40; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(f.addSample(4.79f, t),
        "porte fermee => aucune ingestion de mesure non representative");
    t += kDt;
    f.setContext(false, true, t);
  }
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(medBefore, f.median(),
      "la stagnation ne deplace PAS la mediane (elle n'existe pas pour le filtre)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(4.79f, f.raw(),
      "le brut reste publie pour le diagnostic, meme porte fermee");
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "porte fermee => age ↗ => dosage fail-closed");

  // Reprise de circulation : eau présente mais mesures NON représentatives (purge).
  f.setContext(true, false, t);
  TEST_ASSERT_TRUE_MESSAGE(f.gateClosed(), "purge en cours => porte encore fermee");
  t += 300000;  // 5 min de stabilisation (réglage utilisateur)

  // Fin de stabilisation : la porte s'ouvre ET ré-amorce l'estimateur.
  f.setContext(true, true, t);
  TEST_ASSERT_FALSE_MESSAGE(f.gateClosed(), "mesures representatives => porte ouverte");
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "reouverture => re-amorcage");
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.filtered()), "estimateur re-amorce => warmup complet");

  // Les mesures représentatives valent 8,10, soit |8,10 − 6,80| = 1,30 > spikeReject :
  // sans le ré-amorçage elles seraient rejetées UNE À UNE par la médiane d'avant
  // l'arrêt. Ici elles sont acceptées d'emblée, warmup 13 échantillons.
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(8.10f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.10f, f.filtered());
}

// --- T2 miroir ORP ------------------------------------------------------------
void test_stagnation_median_never_authoritative_orp(void) {
  SensorFilter f(orpCfgRearm());
  uint32_t t = 0;
  openGate(f, t);
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(700.0f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));

  f.setContext(false, true, t);
  for (int i = 0; i < 40; ++i) {
    TEST_ASSERT_FALSE(f.addSample(250.0f, t));  // stagnation
    t += kDt;
    f.setContext(false, true, t);
  }
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 700.0f, f.median());

  f.setContext(true, false, t);
  t += 300000;
  f.setContext(true, true, t);
  TEST_ASSERT_TRUE(f.rearmedSinceLastPoll());
  // |350 − 700| = 350 mV > 300 mV : rejeté à vie sans ré-amorçage.
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(350.0f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 350.0f, f.filtered());
}

// --- T3 : non-régression feature-058 — une aberration ISOLÉE reste rejetée ----
//     et n'ouvre JAMAIS la sortie de secours (une mesure acceptée casse la série).
void test_isolated_spikes_never_rearm_ph(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(7.00f, t); t += kDt; }
  const float before = f.filtered();

  // 30 aberrations isolées, chacune encadrée par une mesure saine.
  for (int i = 0; i < 30; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(f.addSample(8.30f, t), "aberration isolee => rejetee");
    t += kDt;
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    t += kDt;
  }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "aberrations isolees => AUCUN re-amorcage (feature-058 preservee)");
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before, f.filtered(),
      "la valeur filtree reste celle du signal sain");
  TEST_ASSERT_TRUE_MESSAGE(f.ready(t - kDt), "le filtre reste disponible");
}

// --- T3 miroir ORP ------------------------------------------------------------
void test_isolated_spikes_never_rearm_orp(void) {
  SensorFilter f(orpCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(680.0f, t); t += kDt; }
  for (int i = 0; i < 30; ++i) {
    TEST_ASSERT_FALSE(f.addSample(1050.0f, t));  // |1050−680| = 370 > 300
    t += kDt;
    TEST_ASSERT_TRUE(f.addSample(680.0f, t));
    t += kDt;
  }
  TEST_ASSERT_FALSE(f.rearmedSinceLastPoll());
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 680.0f, f.filtered());
}

// --- T5 (GARANTIE P8) : le ré-amorçage ne MASQUE PAS une sonde HS -------------
// Une sonde figée sur une valeur plausible et éloignée de la médiane produit 12
// rejets parfaitement cohérents → elle OBTIENT son ré-amorçage. Ce qui doit alors
// la rattraper est le détecteur figé (feature-022), que rearm() ne touche pas :
// frozen() se re-déclenche en 30 échantillons acceptés (150 s à 5 s de cadence).
static SensorFilter::Config frozenPhCfgRearm() {
  SensorFilter::Config cfg = phCfgRearm();
  cfg.frozenSamples = kSensorFrozenSamples;    // 30
  cfg.frozenEpsilon = kSensorFrozenEpsilonPh;  // 0.0005
  return cfg;
}

void test_rearm_does_not_mask_frozen_probe(void) {
  SensorFilter f(frozenPhCfgRearm());
  uint32_t t = 0;
  // Signal VIVANT autour de 6,60 (toggle 1 LSB → le détecteur figé ne se pose pas).
  for (int i = 0; i < 13; ++i) {
    TEST_ASSERT_TRUE(f.addSample((i % 2) ? 6.601f : 6.600f, t));
    t += kDt;
  }
  TEST_ASSERT_TRUE(f.ready(t - kDt));
  TEST_ASSERT_FALSE(f.frozen());

  // La sonde se fige à 7,80 : cohérent (amplitude nulle), dans la bande admissible,
  // et |7,80 − 6,60| = 1,20 > spikeReject → 12 rejets puis ré-amorçage accordé.
  for (int i = 0; i < (int)kSensorFilterSpikeRearmCount; ++i) {
    TEST_ASSERT_FALSE(f.addSample(7.80f, t));
    t += kDt;
  }
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
      "serie coherente et plausible => re-amorcage (on ne peut pas distinguer ici)");
  const uint32_t tRearm = t;

  // …mais le détecteur figé, INTACT, la rattrape en 30 échantillons acceptés.
  int accepted = 0;
  bool frozenAgain = false;
  for (int i = 0; i < 100; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.80f, t));
    ++accepted;
    if (f.frozen()) { frozenAgain = true; break; }
    t += kDt;
  }
  TEST_ASSERT_TRUE_MESSAGE(frozenAgain,
      "une sonde HS DOIT etre re-detectee apres re-amorcage (garantie P8)");
  TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE((int)kSensorFrozenSamples, accepted,
      "re-detection en 30 echantillons au plus");
  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(150000UL, t - tRearm,
      "re-detection en 150 s au plus a la cadence de 5 s");
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t),
      "sonde figee => non pret => dosage fail-closed, le re-amorcage n'a rien blanchi");
}

// --- T8a : borne basse — 11 rejets cohérents ne déclenchent RIEN --------------
void test_eleven_coherent_rejects_do_not_rearm(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
  for (int i = 0; i < (int)kSensorFilterSpikeRearmCount - 1; ++i) {
    TEST_ASSERT_FALSE(f.addSample(6.96f, t));
    t += kDt;
  }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(), "11 < N=12 => rien");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 4.79f, f.median(), "mediane encore intacte");
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
  // Le 12ᵉ, et seulement lui, déclenche.
  TEST_ASSERT_FALSE(f.addSample(6.96f, t));
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "12e rejet coherent => re-amorcage");
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.median()), "buffer vide apres re-amorcage");
}

// --- T8b : un spike INCOHÉRENT remet le compteur de série à 0 -----------------
void test_incoherent_spike_resets_coherent_streak(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  // Intrus incohérent (|9,50 − 6,96| = 2,54 > 0,20) → la série se ré-ancre sur lui.
  TEST_ASSERT_FALSE(f.addSample(9.50f, t)); t += kDt;
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "le 12e echantillon incoherent ne doit PAS declencher");
  // 11 nouveaux rejets cohérents : si le compteur n'avait pas été remis à 0, on
  // aurait déjà ré-amorcé.
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "compteur remis a 0 par l'intrus => 11 coherents ne suffisent pas");
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.79f, f.median());
  TEST_ASSERT_FALSE(f.addSample(6.96f, t));
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "12e coherent => re-amorcage");
}

// --- T8c : 2ᵉ ré-amorçage dans les 30 min REFUSÉ (rearmDenied == 1) -----------
void test_second_rearm_within_cooldown_denied(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
  for (int i = 0; i < 12; ++i) { f.addSample(6.96f, t); t += kDt; }   // ré-amorçage #1
  TEST_ASSERT_TRUE(f.rearmedSinceLastPoll());
  const uint32_t rearmMs = t - kDt;
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));

  // 2ᵉ échelon cohérent ET dans la bande (8,10 ; |8,10 − 6,96| = 1,14 > 1,0),
  // moins de 30 min après le premier ré-amorçage.
  for (int i = 0; i < 12; ++i) { TEST_ASSERT_FALSE(f.addSample(8.10f, t)); t += kDt; }
  TEST_ASSERT_TRUE_MESSAGE((t - rearmMs) < kSensorFilterRearmCooldownMs,
      "le scenario doit rester dans la fenetre de cooldown");
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(), "2e re-amorcage < 30 min => refuse");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, f.rearmDenied(), "motif collant = cooldown");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 6.96f, f.median(), "mediane inchangee (refus)");
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t),
      "refus => filtre non pret => dosage fail-closed, la coquille DOIT alarmer");

  // Cooldown écoulé : la même évidence est alors acceptée.
  t += kSensorFilterRearmCooldownMs;
  for (int i = 0; i < 12; ++i) { TEST_ASSERT_FALSE(f.addSample(8.10f, t)); t += kDt; }
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "cooldown ecoule => autorise");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.rearmDenied(), "rearm() efface le motif de refus");
}

// --- T8d : cible HORS BANDE admissible → jamais de ré-amorçage (fail-closed) --
void test_rearm_denied_out_of_admissible_band(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(7.00f, t); t += kDt; }
  // Sonde qui part à 9,50 : parfaitement cohérente, mais hors [6,50 ; 8,20] → une
  // telle valeur n'est pas un etat de bassin plausible.
  for (int i = 0; i < 12; ++i) { TEST_ASSERT_FALSE(f.addSample(9.50f, t)); t += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "hors bande => JAMAIS de re-amorcage (une sonde HS n'impose pas sa valeur)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, f.rearmDenied(), "motif collant = hors bande");
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.00f, f.median());
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "fail-closed maintenu");
  // Insister ne débloque rien : 36 rejets de plus, toujours refusé.
  for (int i = 0; i < 36; ++i) { TEST_ASSERT_FALSE(f.addSample(9.50f, t)); t += kDt; }
  TEST_ASSERT_FALSE(f.rearmedSinceLastPoll());
  TEST_ASSERT_EQUAL_UINT8(2, f.rearmDenied());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.00f, f.median());
}

// --- T9 : stabilizationDelayMin = 0 → le plancher interne de 60 s protège quand ---
//     même (verdict P7 : le gel est piloté par waterPresent, pas par le réglage).
void test_gate_floor_applies_with_zero_stabilization_delay(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  openGate(f, t);
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(6.80f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));

  // Arrêt de filtration.
  f.setContext(false, true, t);
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_FALSE(f.addSample(4.79f, t));
    t += kDt;
    f.setContext(false, true, t);
  }

  // Reprise : délai de stabilisation utilisateur = 0 → measuresValid vaut true DÈS
  // le front montant. Le plancher interne doit malgré tout tenir la porte fermée.
  const uint32_t resume = t;
  f.setContext(true, true, resume);
  TEST_ASSERT_TRUE_MESSAGE(f.gateClosed(),
      "plancher interne de 60 s applique meme avec stabilisation = 0");
  TEST_ASSERT_FALSE_MESSAGE(f.addSample(4.79f, resume),
      "l'eau de purge n'entre pas davantage");

  const uint32_t justBefore = resume + kSensorFilterGateFloorMs - 1000;
  f.setContext(true, true, justBefore);
  TEST_ASSERT_TRUE_MESSAGE(f.gateClosed(), "encore ferme a 59 s");
  TEST_ASSERT_FALSE(f.addSample(4.79f, justBefore));

  t = resume + kSensorFilterGateFloorMs;
  f.setContext(true, true, t);
  TEST_ASSERT_FALSE_MESSAGE(f.gateClosed(), "porte ouverte a 60 s");
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "ouverture => re-amorcage");
  // La médiane de stagnation n'a jamais existé : 8,10 (écart 1,30 avec l'ancienne
  // médiane 6,80) est suivi sans aucun rejet.
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(8.10f, t)); t += kDt; }
  TEST_ASSERT_TRUE(f.ready(t - kDt));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.10f, f.filtered());
}

// =============================================================================
// Arbitrages tranchés par le gate pool-chemistry (v2.20.3)
// =============================================================================
// Ces tests étaient des tests de CARACTÉRISATION : ils verrouillaient le
// comportement observé afin de le porter au gate. Le gate a statué sur les 4
// points — (a) et (c) et (d) ont changé le code de production, (b) est confirmé
// tel quel. Chaque test ci-dessous atteste désormais la décision RETENUE et
// rappelle en commentaire quelle décision l'a rendu caduc.

// --- (1) rearm() REMET rejectedCount à 0, et publie la valeur d'avant ---------
// ARBITRAGE (gate pool-chemistry v2.20.3, point (a)) : ce test attestait
// l'inverse — « rearm() conserve rejectedCount » — et le gate l'a rendu caduc.
// Motif retenu : /debug/sensor_filter_state affichait encore rejected=255 après
// un ré-amorçage RÉUSSI, l'exploitant ne distinguait donc pas un filtre RÉPARÉ
// d'un filtre encore VERROUILLÉ. La valeur d'avant n'est pas perdue : elle est
// publiée par rejectedBeforeRearm(), que la coquille journalise pour tracer
// l'ampleur du verrouillage levé.
void test_rearm_resets_rejected_count_and_publishes_previous(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.rejectedBeforeRearm(),
      "aucun re-amorcage encore survenu => rien a reporter");
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
  for (int i = 0; i < 12; ++i) { f.addSample(6.96f, t); t += kDt; }
  TEST_ASSERT_TRUE(f.rearmedSinceLastPoll());
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.rejectedCount(),
      "re-amorcage reussi => compteur remis a zero (filtre repare, pas verrouille)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(12, f.rejectedBeforeRearm(),
      "la valeur d'avant reste lisible pour le journal de re-amorcage");
  // Le compteur repart bien de zéro : un rejet postérieur le remet à 1, il ne
  // reprend pas là où il s'était arrêté.
  TEST_ASSERT_FALSE(f.addSample(NAN, t));
  TEST_ASSERT_EQUAL_UINT8(1, f.rejectedCount());
  // reset() (le « gros marteau ») remet évidemment le compteur à zéro lui aussi.
  f.reset();
  TEST_ASSERT_EQUAL_UINT8(0, f.rejectedCount());
}

// --- (2) reset() efface le cooldown → ré-amorçage immédiat possible -----------
// Cohérent : reset() vide déjà le buffer, donc la sonde impose de toute façon sa
// valeur aux 13 échantillons suivants ; conserver le cooldown ne protégerait rien.
void test_reset_clears_rearm_cooldown(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
  for (int i = 0; i < 12; ++i) { f.addSample(6.96f, t); t += kDt; }   // ré-amorçage #1
  TEST_ASSERT_TRUE(f.rearmedSinceLastPoll());

  f.reset();  // action manuelle (calibration / reprise UI)
  for (int i = 0; i < 13; ++i) { TEST_ASSERT_TRUE(f.addSample(4.79f, t)); t += kDt; }
  for (int i = 0; i < 12; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
      "reset() remet le cooldown a neuf : 2e re-amorcage autorise sans attendre 30 min");
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
}

// --- (3) la garde de bande juge le CENTRE de la série, pas le dernier brut ----
// ARBITRAGE (gate pool-chemistry v2.20.3, point (c)) : ce test attestait que
// seule la 12ᵉ valeur brute était confrontée à [rearmMin ; rearmMax], ce qui
// laissait fuir hors bande une série entière pourvu que son DERNIER échantillon
// rentre dans la bande. Le gate a tranché pour le centre (_spikeMin+_spikeMax)/2 :
// même disponibilité, fuite hors bande divisée par deux (rearmCoherence/2 au lieu
// de rearmCoherence). Les deux cas ci-dessous sont volontairement SYMÉTRIQUES :
// ils ne peuvent PAS être satisfaits tous les deux par la règle « dernier brut ».
void test_band_guard_checks_series_center_not_last_reject(void) {
  // Cas A — centre HORS bande, dernier brut DANS la bande => REFUS.
  // Série [8,20 ; 8,35] : centre 8,275 > 8,20 = rearmMax, alors que le 12ᵉ brut
  // (8,20) est pile sur la borne haute. L'ancienne règle aurait ré-amorcé.
  {
    SensorFilter f(phCfgRearm());
    uint32_t t = 0;
    for (int i = 0; i < 13; ++i) { f.addSample(7.00f, t); t += kDt; }
    for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(8.35f, t)); t += kDt; }
    TEST_ASSERT_FALSE(f.addSample(8.20f, t));
    TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
        "centre 8,275 hors bande => refus, meme si le 12e brut (8,20) y est");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, f.rearmDenied(), "motif collant = hors bande");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 7.00f, f.median(), "mediane intacte (refus)");
  }
  // Cas B — centre DANS la bande, dernier brut HORS bande => ACCORD.
  // Série [8,10 ; 8,25] : centre 8,175 <= 8,20, alors que le 12ᵉ brut (8,25) est
  // hors bande. L'ancienne règle aurait refusé. Preuve que c'est bien le centre,
  // et non le dernier brut, qui décide.
  {
    SensorFilter f(phCfgRearm());
    uint32_t t = 0;
    for (int i = 0; i < 13; ++i) { f.addSample(7.00f, t); t += kDt; }
    for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(8.10f, t)); t += kDt; }
    TEST_ASSERT_FALSE(f.addSample(8.25f, t));
    TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
        "centre 8,175 dans la bande => re-amorcage, meme si le 12e brut (8,25) n'y est pas");
    TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
    TEST_ASSERT_TRUE_MESSAGE(isnan(f.median()), "buffer vide apres re-amorcage");
  }
}

// =============================================================================
// (4) Tolérance aux TROUS de lecture dans une série de rejets cohérents
// =============================================================================
// ARBITRAGE (gate pool-chemistry v2.20.3, point (d), BLOQUANT) : ces tests
// attestaient qu'un NaN (ou un hors-plage) remettait la série à zéro, présenté
// comme un choix de sûreté « évidence CONTINUE ». Le gate l'a rendu caduc :
// « perdre toute régulation par un trou de lecture est inacceptable » — une sonde
// ratant un cycle sur douze pendant un échelon LÉGITIME ne ré-amorçait jamais,
// c'est-à-dire que l'incident d'origine n'était pas corrigé.
// Règle chiffrée retenue, exercée par les 5 tests qui suivent :
//   * un trou n'incrémente PAS la série, mais ne la remet PAS à zéro ;
//   * au plus 2 trous par série ; JAMAIS 2 consécutifs ;
//   * le 3ᵉ trou ou 2 trous consécutifs remettent la série à zéro ;
//   * un échantillon ACCEPTÉ remet toujours la série à zéro (inchangé).
// NB : tous ces tests utilisent phCfgRearm() — avec phCfg() la sortie de secours
// serait INERTE (rearmCoherence = 0) et ils passeraient pour une mauvaise raison.

// Amorce un filtre pH sur 13 mesures acceptées à 4,79 (médiane 4,79). Les rejets
// de spike qui suivent se font à 6,96 : |6,96 − 4,79| = 2,17 > spikeReject 1,0,
// et 6,96 est dans la bande admissible [6,50 ; 8,20] → seule la longueur de la
// série décide du ré-amorçage.
static void primeAt479(SensorFilter& f, uint32_t& t) {
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
}

// --- (4a) un trou ISOLÉ ne casse plus la série --------------------------------
void test_isolated_gap_does_not_break_coherent_reject_streak(void) {
  // Variante NaN : l'EZO reste muet un cycle.
  {
    SensorFilter f(phCfgRearm());
    uint32_t t = 0;
    primeAt479(f, t);
    for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
    TEST_ASSERT_FALSE(f.addSample(NAN, t)); t += kDt;
    TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
        "le trou ne compte pas comme 12e rejet");
    TEST_ASSERT_FALSE(f.addSample(6.96f, t));
    TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
        "la serie a SURVECU au trou : le 12e rejet REEL re-amorce");
  }
  // Variante hors plage plausible : lecture aberrante (< minValue) un cycle.
  {
    SensorFilter f(phCfgRearm());
    uint32_t t = 0;
    primeAt479(f, t);
    for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
    TEST_ASSERT_FALSE(f.addSample(-5.0f, t)); t += kDt;   // hors [0 ; 14]
    TEST_ASSERT_FALSE(f.rearmedSinceLastPoll());
    TEST_ASSERT_FALSE(f.addSample(6.96f, t));
    TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
        "un hors-plage est un trou au meme titre qu'un NaN");
  }
}

// --- (4b) 1 trou puis 11 rejets cohérents → ré-amorce -------------------------
// Scénario nommément exigé par le gate : le trou survient TÔT dans la série (dès
// le 2ᵉ cycle) et les 11 rejets qui suivent la mènent à son terme (12 rejets
// RÉELS au total). Sans la tolérance, ce cas ne ré-amorçait jamais.
void test_one_gap_then_eleven_rejects_rearms(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  primeAt479(f, t);
  TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt;   // rejet #1 : série amorcée
  TEST_ASSERT_FALSE(f.addSample(NAN, t)); t += kDt;     // trou isolé
  for (int i = 0; i < 10; ++i) {
    TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt;
    TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
        "11 rejets reels au plus => pas encore de re-amorcage");
  }
  TEST_ASSERT_FALSE(f.addSample(6.96f, t));            // rejet réel #12
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(),
      "12 rejets REELS malgre un trou => re-amorcage (exigence bloquante du gate)");
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.median()), "buffer vide apres re-amorcage");
}

// --- (4c) 3 trous NON consécutifs → série remise à zéro, refus ----------------
void test_three_gaps_in_series_deny_rearm(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  primeAt479(f, t);
  // R,G,R,G,R,G : le 3ᵉ trou dépasse le budget (2) → série remise à zéro.
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt;
    TEST_ASSERT_FALSE(f.addSample(NAN, t));   t += kDt;
  }
  // 11 rejets cohérents APRÈS la remise à zéro. Si les 3 rejets d'avant avaient
  // été conservés, on aurait ré-amorcé au cours de cette boucle.
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "3e trou => serie remise a zero => 11 rejets ne suffisent pas");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, f.rearmDenied(),
      "serie jamais complete => aucune garde consultee, aucun refus a alarmer");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 4.79f, f.median(), "mediane intacte");
  // Le 12ᵉ de la NOUVELLE série, lui, ré-amorce : le compte a bien repris de zéro.
  TEST_ASSERT_FALSE(f.addSample(6.96f, t));
  TEST_ASSERT_TRUE(f.rearmedSinceLastPoll());
}

// --- (4d) 2 trous CONSÉCUTIFS → série remise à zéro, refus --------------------
// Deux cycles muets d'affilée : l'évidence n'est plus continue, on refuse même si
// le budget de trous (2) n'est pas dépassé.
void test_two_consecutive_gaps_deny_rearm(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  primeAt479(f, t);
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE(f.addSample(NAN, t)); t += kDt;   // trou 1 (toléré)
  TEST_ASSERT_FALSE(f.addSample(NAN, t)); t += kDt;   // trou 2 CONSÉCUTIF → RAZ
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "2 trous consecutifs => serie remise a zero => 11 rejets ne suffisent pas");
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.79f, f.median());
  TEST_ASSERT_FALSE(f.addSample(6.96f, t));
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "12e de la nouvelle serie => re-amorcage");
}

// --- (4e) non-régression : un échantillon ACCEPTÉ remet toujours la série à 0 --
// La tolérance ne concerne QUE les trous. Une mesure acceptée prouve que la
// médiane courante fait encore autorité → les rejets d'avant ne décrivent plus
// un échelon soutenu et ne doivent pas se cumuler avec ceux d'après.
void test_accepted_sample_resets_reject_series(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  primeAt479(f, t);
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_TRUE_MESSAGE(f.addSample(4.80f, t), "proche de la mediane => ACCEPTE");
  t += kDt;
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "un echantillon accepte remet la serie a zero : 22 rejets ne se cumulent pas");
  TEST_ASSERT_EQUAL_UINT8(0, f.rearmDenied());
  TEST_ASSERT_FALSE(f.addSample(6.96f, t));
  TEST_ASSERT_TRUE_MESSAGE(f.rearmedSinceLastPoll(), "12e de la nouvelle serie => re-amorcage");
}

// --- (4f) non-régression : un trou ne fait PAS avancer le compte de la série ---
// 11 rejets + 1 trou != 12 rejets. La tolérance PRÉSERVE l'évidence acquise, elle
// n'en FABRIQUE pas : seuls des rejets réels comptent vers kSensorFilterSpikeRearmCount.
void test_gap_does_not_advance_spike_count(void) {
  SensorFilter f(phCfgRearm());
  uint32_t t = 0;
  primeAt479(f, t);
  for (int i = 0; i < 11; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE(f.addSample(NAN, t)); t += kDt;     // trou #1 : ne compte pas
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(), "11 rejets + 1 trou != 12");
  TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt;   // rejet réel #12
  TEST_ASSERT_TRUE(f.rearmedSinceLastPoll());
  // Cas extrême : QUE des trous, jamais de rejet réel → aucune série, aucun
  // ré-amorçage possible (une sonde muette n'impose rien).
  SensorFilter g(phCfgRearm());
  uint32_t u = 0;
  primeAt479(g, u);
  for (int i = 0; i < 60; ++i) { TEST_ASSERT_FALSE(g.addSample(NAN, u)); u += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(g.rearmedSinceLastPoll(),
      "que des trous => aucune evidence => jamais de re-amorcage");
  TEST_ASSERT_EQUAL_UINT8(0, g.rearmDenied());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.79f, g.median());
}

// --- Verrou de régression sur les constantes du correctif ---------------------
void test_rearm_constants_values(void) {
  TEST_ASSERT_EQUAL_UINT8(12, kSensorFilterSpikeRearmCount);
  TEST_ASSERT_EQUAL_UINT32(1800000UL, kSensorFilterRearmCooldownMs);   // 30 min
  TEST_ASSERT_EQUAL_UINT32(60000UL, kSensorFilterGateFloorMs);         // 60 s
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.20f, kPhFilterRearmCoherence);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 40.0f, kOrpFilterRearmCoherence);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.50f, kPhFilterRearmMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.20f, kPhFilterRearmMax);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 300.0f, kOrpFilterRearmMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 950.0f, kOrpFilterRearmMax);
  // La sortie de secours doit rester PLUS LENTE que le warmup n'est long : sinon
  // elle chasserait le bruit plus vite que le filtre ne se stabilise.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT8(kSensorFilterCentralWindow - 1,
                                     kSensorFilterSpikeRearmCount);
}

// --- Non-régression : sans les 3 champs, la sortie de secours est INERTE -------
// (rétrocompatibilité des init agrégées courtes — c'est ce qui permet aux tests
// historiques de rester valides.)
void test_rearm_disabled_with_legacy_config(void) {
  SensorFilter f(phCfg());  // rearmCoherence value-initialisé à 0
  uint32_t t = 0;
  for (int i = 0; i < 13; ++i) { f.addSample(4.79f, t); t += kDt; }
  for (int i = 0; i < 60; ++i) { TEST_ASSERT_FALSE(f.addSample(6.96f, t)); t += kDt; }
  TEST_ASSERT_FALSE_MESSAGE(f.rearmedSinceLastPoll(),
      "rearmCoherence = 0 => sortie de secours desactivee (comportement anterieur)");
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.79f, f.median());
  TEST_ASSERT_FALSE(f.ready(t));
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
  RUN_TEST(test_latch_persists_while_noise_persists);
  // Auto-levée du latch EMI (v2.8.4).
  RUN_TEST(test_latch_auto_clears_after_clean_windows);
  RUN_TEST(test_latch_clear_streak_reset_by_hard_spike);
  RUN_TEST(test_latch_clear_streak_reset_by_nan_and_out_of_range);
  RUN_TEST(test_clean_streak_only_counts_full_windows);
  RUN_TEST(test_latch_cleared_immediately_by_reset);
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
  // v2.20.3 — porte d'alimentation + sortie de secours.
  RUN_TEST(test_rearm_constants_values);
  RUN_TEST(test_incident_replay_sustained_step_recovers_bounded_ph);   // T1 (central)
  RUN_TEST(test_incident_replay_sustained_step_recovers_bounded_orp);  // T7
  RUN_TEST(test_stagnation_median_never_authoritative_ph);             // T2
  RUN_TEST(test_stagnation_median_never_authoritative_orp);            // T7
  RUN_TEST(test_isolated_spikes_never_rearm_ph);                       // T3
  RUN_TEST(test_isolated_spikes_never_rearm_orp);                      // T7
  RUN_TEST(test_rearm_does_not_mask_frozen_probe);                     // T5 (P8)
  RUN_TEST(test_eleven_coherent_rejects_do_not_rearm);                 // T8a
  RUN_TEST(test_incoherent_spike_resets_coherent_streak);              // T8b
  RUN_TEST(test_second_rearm_within_cooldown_denied);                  // T8c
  RUN_TEST(test_rearm_denied_out_of_admissible_band);                  // T8d
  RUN_TEST(test_gate_floor_applies_with_zero_stabilization_delay);     // T9
  RUN_TEST(test_rearm_disabled_with_legacy_config);
  // Arbitrages tranchés par le gate pool-chemistry (v2.20.3).
  RUN_TEST(test_rearm_resets_rejected_count_and_publishes_previous);    // (a)
  RUN_TEST(test_reset_clears_rearm_cooldown);                          // (b) confirmé
  RUN_TEST(test_band_guard_checks_series_center_not_last_reject);      // (c)
  // (d) tolérance aux trous de lecture — condition BLOQUANTE du gate.
  RUN_TEST(test_isolated_gap_does_not_break_coherent_reject_streak);   // 4a
  RUN_TEST(test_one_gap_then_eleven_rejects_rearms);                   // 4b
  RUN_TEST(test_three_gaps_in_series_deny_rearm);                      // 4c
  RUN_TEST(test_two_consecutive_gaps_deny_rearm);                      // 4d
  RUN_TEST(test_accepted_sample_resets_reject_series);                 // 4e
  RUN_TEST(test_gap_does_not_advance_spike_count);                     // 4f
  return UNITY_END();
}
