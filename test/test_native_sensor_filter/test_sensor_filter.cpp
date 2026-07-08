// =============================================================================
// Tests unitaires natifs — SensorFilter (feature-025)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel Atlas EZO.
// On teste le COMPORTEMENT observable (warmup, médiane, EMA, rejet de pic,
// reset, instabilité, NaN/hors-plage, suivi de dérive) via l'API publique,
// pas l'implémentation interne.
//
// Config de référence calquée sur les valeurs réelles du projet (constants.h) :
//   pH  : min 0  max 14    step 0.15  alpha 0.10  window 7  warmup 5  maxRej 10
//   ORP : min -1000 max 1500 step 50  alpha 0.08  window 7  warmup 5  maxRej 10
// =============================================================================

#include <unity.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include "sensor_filter.h"

static SensorFilter::Config phCfg() {
  return SensorFilter::Config{
      /*minValue*/ 0.0f,
      /*maxValue*/ 14.0f,
      /*maxStep*/ 0.15f,
      /*emaAlpha*/ 0.10f,
      /*medianWindow*/ 7,
      /*warmupSamples*/ 5,
      /*maxConsecutiveRejects*/ 10,
      /*maxAgeMs*/ 20000};
}

static SensorFilter::Config orpCfg() {
  return SensorFilter::Config{
      -1000.0f, 1500.0f, 50.0f, 0.08f, 7, 5, 10, 20000};
}

void setUp(void) {}
void tearDown(void) {}

// --- Warmup : not ready avant warmupSamples (5) mesures valides --------------
void test_warmup_not_ready_until_5_valid_samples(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  TEST_ASSERT_FALSE(f.ready(t));
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.0f, t));
    t += 1000;
    TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "ready() ne doit pas etre vrai avant 5 mesures");
  }
  // 5e mesure valide → ready bascule vrai (mesure recente).
  TEST_ASSERT_TRUE(f.addSample(7.0f, t));
  TEST_ASSERT_TRUE(f.ready(t));
}

// --- ready() repasse false si mesure trop ancienne (EZO injoignable / AC13) --
void test_ready_false_when_stale(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 5; ++i) { f.addSample(7.0f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.ready(t));
  // 21 s sans nouvelle mesure valide (> maxAgeMs 20 s) → plus prêt.
  TEST_ASSERT_FALSE(f.ready(t + 21000));
}

// --- Médiane sur fenêtre 7 -----------------------------------------------------
// Vérifie le calcul de la médiane sur les 7 derniers échantillons acceptés.
// Contrainte : hors warmup (à partir du 6e), |raw - filtered| doit rester ≤ 0.15
// (maxStep pH), sinon l'échantillon est rejeté. On garde donc toutes les valeurs
// dans une bande de ±0.06 autour de 7.00 → toutes acceptées, médiane déterministe.
void test_median_window_7(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // 7 valeurs dans [6.97 .. 7.03], désordonnées. Triées : 6.97 6.98 6.99 7.00
  // 7.01 7.02 7.03 → médiane (4e) = 7.00. Chaque valeur reste à <0.15 du filtré.
  float vals[7] = {7.00f, 7.03f, 6.97f, 7.01f, 6.99f, 7.02f, 6.98f};
  for (int i = 0; i < 7; ++i) {
    TEST_ASSERT_TRUE(f.addSample(vals[i], t));
    t += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.00f, f.median());
}

// --- EMA : convergence vers une valeur stable -------------------------------
void test_ema_converges_to_stable_value(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 60; ++i) { f.addSample(7.20f, t); t += 1000; }
  // Après de nombreuses mesures identiques, filtré ≈ valeur d'entrée.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.20f, f.filtered());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.20f, f.median());
}

// --- Rejet pic isolé pH (>0.15 vs filtered) : addSample false, filtré inchangé
void test_reject_isolated_spike_ph(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 1000; }
  float before = f.filtered();
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, before);
  // Pic +0.30 (> 0.15) → rejeté.
  TEST_ASSERT_FALSE(f.addSample(7.30f, t));
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());  // filtré strictement inchangé
  TEST_ASSERT_EQUAL_UINT8(1, f.rejectedCount());
}

// --- Rejet pic isolé ORP (>50 mV vs filtered) -------------------------------
void test_reject_isolated_spike_orp(void) {
  SensorFilter f(orpCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(680.0f, t); t += 1000; }
  float before = f.filtered();
  TEST_ASSERT_FALSE(f.addSample(780.0f, t));  // +100 mV > 50
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());
}

// --- reset() → re-warmup (ready repasse false) ------------------------------
void test_reset_triggers_rewarmup(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 6; ++i) { f.addSample(7.0f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.ready(t));
  f.reset();
  TEST_ASSERT_FALSE(f.ready(t));
  TEST_ASSERT_TRUE(isnan(f.filtered()));
  TEST_ASSERT_EQUAL_UINT8(0, f.rejectedCount());
  // Il faut de nouveau 5 mesures valides pour redevenir prêt.
  for (int i = 0; i < 4; ++i) { f.addSample(7.0f, t); t += 1000; TEST_ASSERT_FALSE(f.ready(t)); }
  f.addSample(7.0f, t);
  TEST_ASSERT_TRUE(f.ready(t));
}

// --- Instabilité : unstable() après maxConsecutiveRejects (10) rejets --------
void test_unstable_after_consecutive_rejects(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.unstable());
  // 9 rejets consécutifs (pic +1.0) → pas encore instable.
  for (int i = 0; i < 9; ++i) { f.addSample(8.0f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.unstable());
  // 10e rejet consécutif → instable.
  f.addSample(8.0f, t);
  TEST_ASSERT_TRUE(f.unstable());
  // Une mesure valide remet le compteur consécutif à 0 → stable de nouveau.
  f.addSample(7.00f, t);
  TEST_ASSERT_FALSE(f.unstable());
}

// --- Rejet NaN ---------------------------------------------------------------
void test_reject_nan(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 6; ++i) { f.addSample(7.0f, t); t += 1000; }
  float before = f.filtered();
  TEST_ASSERT_FALSE(f.addSample(NAN, t));
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());
  TEST_ASSERT_EQUAL_UINT8(1, f.rejectedCount());
}

// --- Rejet hors plage plausible ---------------------------------------------
void test_reject_out_of_range(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // En warmup, pas de garde de saut, mais la garde de plage s'applique toujours.
  TEST_ASSERT_FALSE(f.addSample(-1.0f, t));   // < 0
  TEST_ASSERT_FALSE(f.addSample(15.0f, t));   // > 14
  TEST_ASSERT_EQUAL_UINT8(2, f.rejectedCount());
  // ORP : -2000 et +2000 mV hors [-1000, 1500].
  SensorFilter g(orpCfg());
  TEST_ASSERT_FALSE(g.addSample(-2000.0f, t));
  TEST_ASSERT_FALSE(g.addSample(2000.0f, t));
}

// --- Dérive lente réelle suivie progressivement (pas figée) -----------------
void test_slow_drift_is_tracked(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // Warmup à 7.00.
  for (int i = 0; i < 5; ++i) { f.addSample(7.00f, t); t += 1000; }
  // Dérive lente +0.01 pH/mesure pendant 100 mesures (chaque pas << maxStep).
  float v = 7.00f;
  for (int i = 0; i < 100; ++i) {
    v += 0.01f;
    TEST_ASSERT_TRUE(f.addSample(v, t));  // jamais rejeté (pas < step)
    t += 1000;
  }
  // La valeur filtrée doit avoir suivi la dérive (proche de la valeur courante),
  // donc nettement au-dessus de la valeur initiale : pas figée à 7.00.
  TEST_ASSERT_TRUE_MESSAGE(f.filtered() > 7.5f, "le filtre doit suivre la derive lente");
  TEST_ASSERT_FLOAT_WITHIN(0.2f, v, f.filtered());
}

// =============================================================================
// Re-synchronisation + anti-boucle latché (correctif terrain calibration→retour)
// =============================================================================
// Constantes attendues (constants.h) :
//   kSensorFilterResyncRejects        = 12  (seuil re-sync, ≈60 s — feature-033)
//   kSensorFilterMaxConsecutiveRejects= 10  (instable)
//   kSensorFilterMaxResyncPerWindow   = 3   (anti-boucle)
//   kSensorFilterResyncWindowMs       = 600000
// Les timestamps `now` sont injectés (déterministe, pas de millis() réel).

// --- Cas 0 : sanity des constantes (verrou de régression) -------------------
void test_resync_constants_values(void) {
  TEST_ASSERT_EQUAL_UINT8(12, kSensorFilterResyncRejects);
  TEST_ASSERT_EQUAL_UINT8(10, kSensorFilterMaxConsecutiveRejects);
  TEST_ASSERT_EQUAL_UINT8(3, kSensorFilterMaxResyncPerWindow);
  TEST_ASSERT_EQUAL_UINT32(600000u, kSensorFilterResyncWindowMs);
  // Invariants structurels : le seuil de re-sync doit rester STRICTEMENT
  // au-dessus du seuil "instable" (sinon plus jamais d'état instable observable)
  // ET au-dessus de la fenêtre médiane (sinon mini-buffer de rejets non plein).
  TEST_ASSERT_GREATER_THAN_UINT8(kSensorFilterMaxConsecutiveRejects, kSensorFilterResyncRejects);
  TEST_ASSERT_GREATER_THAN_UINT8(kSensorFilterMedianWindow, kSensorFilterResyncRejects);
}

// --- Cas A : PIC ISOLÉ (< 24 cycles) → rejeté, filtré figé, PAS de re-sync ---
void test_caseA_isolated_burst_no_resync(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 5000; }
  float before = f.filtered();
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, before);

  // 5 mesures aberrantes (+0.50 > maxStep) — bien en dessous du seuil de re-sync.
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_FALSE(f.addSample(7.50f, t));
    t += 5000;
  }
  // _filtered STRICTEMENT inchangé, aucune re-sync déclenchée.
  TEST_ASSERT_EQUAL_FLOAT(before, f.filtered());
  TEST_ASSERT_EQUAL_UINT8(0, f.resyncCount());
  TEST_ASSERT_FALSE(f.unstableLatched());

  // Retour à la normale : la mesure est de nouveau acceptée, filtre vivant.
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 7.00f, f.filtered());
}

// --- Cas B : CHANGEMENT DURABLE → re-sync puis re-warmup (BUG TERRAIN) -------
// Scénario : sonde calibrée/stabilisée à ~4.871, puis plongée durablement à ~4.0
// (écart 0.871 > maxStep 0.15). On prouve que le filtre ne reste PAS figé à vie.
void test_caseB_durable_change_triggers_resync_and_rewarmup(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  const float kCal = 4.871f;
  const float kNew = 4.0f;

  // Warmup + stabilisation à 4.871.
  for (int i = 0; i < 20; ++i) { f.addSample(kCal, t); t += 5000; }
  TEST_ASSERT_TRUE(f.ready(t));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kCal, f.filtered());
  float frozen = f.filtered();

  // (1) Les (kResync-1) premières mesures à 4.0 sont rejetées, _filtered FIGÉ.
  for (int i = 0; i < (int)kSensorFilterResyncRejects - 1; ++i) {
    TEST_ASSERT_FALSE(f.addSample(kNew, t));
    TEST_ASSERT_EQUAL_FLOAT(frozen, f.filtered());  // figé pendant le rejet
    t += 5000;
  }
  TEST_ASSERT_EQUAL_UINT8(kSensorFilterResyncRejects - 1, f.consecutiveRejects());
  TEST_ASSERT_EQUAL_UINT8(0, f.resyncCount());

  // (2) kResync-ième rejet consécutif (12e) → re-sync : retour warmup.
  TEST_ASSERT_FALSE(f.addSample(kNew, t));
  t += 5000;
  TEST_ASSERT_EQUAL_UINT8(1, f.resyncCount());
  // (3) ready() repasse false pendant le re-warmup.
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "ready doit retomber false apres re-sync");
  TEST_ASSERT_FALSE(f.unstableLatched());

  // (4) warmupSamples mesures à 4.0 → convergence vers 4.0 et ready() true.
  for (int i = 0; i < (int)phCfg().warmupSamples; ++i) {
    TEST_ASSERT_TRUE(f.addSample(kNew, t));
    t += 5000;
  }
  TEST_ASSERT_TRUE_MESSAGE(f.ready(t), "le filtre doit redevenir pret apres re-warmup");
  TEST_ASSERT_FLOAT_WITHIN(0.05f, kNew, f.filtered());
}

// --- Cas C : RÉ-AMORÇAGE SUR MÉDIANE des bruts rejetés (pas un pic final) ----
// On rejette (kResync-1) mesures à ~4.0 puis le déclencheur est un transitoire
// isolé à 6.0. La médiane d'amorçage doit ressortir proche de 4.0, pas 6.0.
// Note : kSensorFilterResyncRejects (12) > kSensorFilterMedianWindow (7) garantit
// que le mini-buffer de rejets est plein de 4.0 avant le pic final.
void test_caseC_reseed_on_median_of_rejected(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 5000; }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, f.filtered());

  // (kResync-1) rejets à 4.0 (le mini-buffer ne garde que les 7 derniers → tous 4.0).
  for (int i = 0; i < (int)kSensorFilterResyncRejects - 1; ++i) {
    f.addSample(4.00f, t);
    t += 5000;
  }
  // Rejet déclencheur : transitoire isolé à 6.0 → déclenche la re-sync.
  f.addSample(6.00f, t);
  t += 5000;
  TEST_ASSERT_EQUAL_UINT8(1, f.resyncCount());

  // Après re-sync, première mesure de re-warmup amorce le filtre.
  // La médiane des 7 derniers bruts rejetés = {4,4,4,4,4,4,6} → 4.0 (et non 6.0).
  TEST_ASSERT_TRUE(f.addSample(4.00f, t));
  TEST_ASSERT_FLOAT_WITHIN(0.6f, 4.00f, f.filtered());
  TEST_ASSERT_TRUE(f.filtered() < 5.0f);  // surtout : PAS amorcé sur le pic 6.0
}

// --- Cas D : ANTI-BOUCLE LATCHÉ → unstable() reste true sauf reset() ---------
void test_caseD_resync_loop_latches_unstable(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;

  // Helper : amène le filtre stable à `base`, puis force une re-sync vers `next`
  // (kSensorFilterResyncRejects rejets consécutifs). Tout dans la fenêtre
  // glissante (incréments courts) : 3 re-sync tiennent dans kResyncWindowMs.
  auto stabilize = [&](float v, int n) {
    for (int i = 0; i < n; ++i) { f.addSample(v, t); t += 5000; }
  };
  auto forceResync = [&](float from, float to) {
    stabilize(from, 8);  // re-stabilise le filtre sur `from`
    for (int i = 0; i < (int)kSensorFilterResyncRejects; ++i) {
      f.addSample(to, t); t += 5000;
    }
  };

  forceResync(7.0f, 4.0f);   // re-sync 1
  TEST_ASSERT_EQUAL_UINT8(1, f.resyncCount());
  TEST_ASSERT_FALSE(f.unstableLatched());

  forceResync(4.0f, 7.0f);   // re-sync 2
  TEST_ASSERT_EQUAL_UINT8(2, f.resyncCount());
  TEST_ASSERT_FALSE(f.unstableLatched());

  forceResync(7.0f, 4.0f);   // re-sync 3 → latch (>= kSensorFilterMaxResyncPerWindow)
  TEST_ASSERT_EQUAL_UINT8(3, f.resyncCount());
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "3 re-sync dans la fenetre doivent latcher");
  TEST_ASSERT_TRUE(f.unstable());

  // Mesures de nouveau stables : le latch NE se lève PAS tout seul.
  stabilize(4.0f, 30);
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "latch persistant malgre mesures stables");
  TEST_ASSERT_TRUE(f.unstable());

  // Seul reset() lève le latch.
  f.reset();
  TEST_ASSERT_FALSE(f.unstableLatched());
  TEST_ASSERT_FALSE(f.unstable());
  TEST_ASSERT_EQUAL_UINT8(0, f.resyncCount());
}

// --- Cas E : reset() réinitialise resyncCount, latch, mini-buffer rejetés ----
void test_caseE_reset_clears_resync_state(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) { f.addSample(7.00f, t); t += 5000; }
  // Provoque une re-sync (kSensorFilterResyncRejects rejets durables).
  for (int i = 0; i < (int)kSensorFilterResyncRejects; ++i) {
    f.addSample(4.00f, t); t += 5000;
  }
  TEST_ASSERT_EQUAL_UINT8(1, f.resyncCount());

  f.reset();
  TEST_ASSERT_EQUAL_UINT8(0, f.resyncCount());
  TEST_ASSERT_FALSE(f.unstableLatched());
  TEST_ASSERT_EQUAL_UINT8(0, f.rejectedCount());
  TEST_ASSERT_EQUAL_UINT8(0, f.consecutiveRejects());
  TEST_ASSERT_TRUE(isnan(f.filtered()));
  TEST_ASSERT_FALSE(f.ready(t));

  // Après reset, le mini-buffer rejetés est vide : un nouveau cycle complet
  // de re-sync (kSensorFilterResyncRejects rejets) repart de zéro sans contamination.
  for (int i = 0; i < 5; ++i) { f.addSample(5.00f, t); t += 5000; }  // nouveau warmup à 5.0
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.00f, f.filtered());
}

// =============================================================================
// Couverture résiduelle (zones non atteintes par les cas A–E)
// =============================================================================

// --- Cas F : medianWindow > capacité (99) clampé à kSensorFilterMedianWindow --
// Couvre le clamp L14-16 du constructeur. Assert indirect : le filtre se comporte
// comme une fenêtre de 7 (warmup + médiane cohérente), aucun débordement.
void test_caseF_median_window_clamped_to_capacity(void) {
  SensorFilter::Config cfg = phCfg();
  cfg.medianWindow = 99;  // > kSensorFilterMedianWindow (7) → doit être borné
  SensorFilter f(cfg);
  uint32_t t = 0;
  // Warmup : 5 valides suffisent (warmupSamples=5), comme une fenêtre normale.
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    t += 1000;
  }
  TEST_ASSERT_TRUE(f.ready(t));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, f.median());
  // Injecter au-delà de 7 échantillons reste stable (pas d'écriture hors buffer).
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    t += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, f.median());
}

// --- Cas G : raw() reflète le dernier brut, y compris si rejeté --------------
// Couvre le getter raw() (L199). _raw est mis à jour AVANT tout rejet (L42).
void test_caseG_raw_reflects_last_sample_even_if_rejected(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // Échantillon accepté.
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));
  TEST_ASSERT_EQUAL_FLOAT(7.00f, f.raw());
  t += 1000;
  // Warmup terminé pour activer la garde de saut.
  for (int i = 0; i < 5; ++i) { f.addSample(7.00f, t); t += 1000; }
  // Échantillon rejeté (pic > maxStep) : raw() reflète quand même la valeur brute.
  TEST_ASSERT_FALSE(f.addSample(9.50f, t));
  TEST_ASSERT_EQUAL_FLOAT(9.50f, f.raw());
}

// --- Cas H : médiane des rejetés, branche PAIRE (medianWindow=6) --------------
// Couvre L196 (0.5*(tmp[n/2-1]+tmp[n/2])). Avec medianWindow=6, _rejCount plafonne
// à 6 (pair) au moment de la re-sync → branche paire de _computeRejectedMedian().
void test_caseH_rejected_median_even_window(void) {
  SensorFilter::Config cfg = phCfg();
  cfg.medianWindow = 6;  // PAIR
  SensorFilter f(cfg);
  uint32_t t = 0;
  // Warmup + stabilisation à 7.00.
  for (int i = 0; i < 10; ++i) { f.addSample(7.00f, t); t += 5000; }
  TEST_ASSERT_TRUE(f.ready(t));

  // 12 rejets consécutifs hors maxStep (saut durable vers 4.0) → re-sync.
  // Le mini-buffer (taille 6, pair) se remplit de 4.0 avant le déclenchement.
  for (int i = 0; i < (int)kSensorFilterResyncRejects; ++i) {
    f.addSample(4.00f, t);
    t += 5000;
  }
  TEST_ASSERT_EQUAL_UINT8(1, f.resyncCount());
  // Re-sync → retour warmup : ready() retombe false.
  TEST_ASSERT_FALSE(f.ready(t));

  // Ré-amorçage sur la médiane paire des bruts rejetés {4,4,4,4,4,4} = 4.0.
  TEST_ASSERT_TRUE(f.addSample(4.00f, t));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 4.00f, f.filtered());
}

// --- Cas I : 4e re-sync → décalage FIFO des timestamps (L94-97) ---------------
// Pousse au-delà du cas D : 4 re-sync dans la même fenêtre. À la 4e, _resyncCount
// est déjà à kSensorFilterMaxResyncPerWindow (3) → branche else (FIFO) exécutée.
void test_caseI_fourth_resync_shifts_timestamps_fifo(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;

  auto stabilize = [&](float v, int n) {
    for (int i = 0; i < n; ++i) { f.addSample(v, t); t += 5000; }
  };
  auto forceResync = [&](float from, float to) {
    stabilize(from, 8);
    for (int i = 0; i < (int)kSensorFilterResyncRejects; ++i) {
      f.addSample(to, t); t += 5000;
    }
  };

  forceResync(7.0f, 4.0f);   // re-sync 1
  forceResync(4.0f, 7.0f);   // re-sync 2
  forceResync(7.0f, 4.0f);   // re-sync 3 → latch
  TEST_ASSERT_EQUAL_UINT8(3, f.resyncCount());
  TEST_ASSERT_TRUE(f.unstableLatched());

  // 4e re-sync : _resyncCount déjà à 3 → décalage FIFO (L94-97). Tous les nowMs
  // restent < kSensorFilterResyncWindowMs d'écart (incréments de 5000 ms).
  TEST_ASSERT_LESS_THAN_UINT32(kSensorFilterResyncWindowMs, t);
  forceResync(4.0f, 7.0f);   // re-sync 4
  TEST_ASSERT_EQUAL_UINT8(3, f.resyncCount());  // plafonné, pas incrémenté
  TEST_ASSERT_TRUE_MESSAGE(f.unstableLatched(), "latch persiste apres la 4e re-sync");
  TEST_ASSERT_TRUE(f.unstable());
}

// =============================================================================
// Détection capteur figé — FrozenDetector + intégration SensorFilter (feature-022)
// =============================================================================
// Config pH avec détection ACTIVÉE (constants.h réels) :
//   frozenSamples = kSensorFrozenSamples (30), frozenEpsilon = kSensorFrozenEpsilonPh (0.0005).
// Les configs phCfg()/orpCfg() existantes (init positionnelle courte) laissent
// frozenSamples/frozenEpsilon à 0 → détection DÉSACTIVÉE (non-régression, cas N).

static SensorFilter::Config frozenPhCfg() {
  SensorFilter::Config cfg = phCfg();
  cfg.frozenSamples = kSensorFrozenSamples;    // 30
  cfg.frozenEpsilon = kSensorFrozenEpsilonPh;  // 0.0005
  return cfg;
}

// --- Verrou de régression : valeurs des constantes frozen ---------------------
void test_frozen_constants_values(void) {
  TEST_ASSERT_EQUAL_UINT16(30, kSensorFrozenSamples);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0005f, kSensorFrozenEpsilonPh);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.05f, kSensorFrozenEpsilonOrp);
}

// --- Cas (a) : 30 lectures strictement identiques → frozen ET ready false ----
void test_frozen_30_identical_readings_blocks_ready(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  // 29 premières lectures identiques : pas encore figé (fenêtre non pleine).
  for (int i = 0; i < 29; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    TEST_ASSERT_FALSE(f.frozen());
    t += 1000;
  }
  // Post-warmup (5 valides) et pas encore figé → prêt.
  TEST_ASSERT_TRUE(f.ready(t));
  // 30e lecture identique → figé, et ready() retombe false (fail-closed).
  TEST_ASSERT_TRUE(f.addSample(7.00f, t));
  TEST_ASSERT_TRUE_MESSAGE(f.frozen(), "30 lectures identiques doivent declarer le capteur fige");
  TEST_ASSERT_FALSE_MESSAGE(f.ready(t), "ready() doit etre false quand le capteur est fige");
}

// --- Cas (b) : 29 identiques + 1 excursion de 1 LSB (0.001) → jamais figé ----
void test_frozen_one_lsb_toggle_breaks_run(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  // 29 identiques (run = 29).
  for (int i = 0; i < 29; ++i) { f.addSample(7.000f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.frozen());
  // Excursion de 1 LSB EZO pH (0.001 > epsilon 0.0005) → le run se ré-ancre.
  TEST_ASSERT_TRUE(f.addSample(7.001f, t));
  t += 1000;
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(), "un toggle de 1 LSB doit casser le run");
  // 29 lectures de plus sur la nouvelle valeur : run = 30 seulement au 30e post-ancrage.
  for (int i = 0; i < 28; ++i) {
    f.addSample(7.001f, t);
    t += 1000;
    TEST_ASSERT_FALSE(f.frozen());  // run max = 29 (ré-ancré) : jamais figé
  }
  TEST_ASSERT_TRUE(f.ready(t));
}

// --- Cas (c) : fenêtre non pleine (29 identiques) → pas figé ------------------
void test_frozen_window_not_full_29_samples(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 29; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.frozen());
  TEST_ASSERT_TRUE(f.ready(t));
}

// --- Cas (d) : sortie d'état — 1 lecture vivante → frozen false + ready true
//     AU MÊME TICK (pas de re-warmup) ------------------------------------------
void test_frozen_exit_immediate_no_rewarmup(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  TEST_ASSERT_FALSE(f.ready(t));
  // Lecture valide hors bande (0.01 > epsilon, mais << maxStep → acceptée).
  TEST_ASSERT_TRUE(f.addSample(7.01f, t));
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(), "sortie immediate des la 1re lecture vivante");
  TEST_ASSERT_TRUE_MESSAGE(f.ready(t), "ready() doit redevenir true au meme tick (pas de re-warmup)");
}

// --- Cas (e) : rafale de rejets pendant l'état figé → frozen PERSISTE ---------
// (pool-chemistry condition #4 : les rejets n'alimentent pas le détecteur)
void test_frozen_persists_through_reject_burst(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  // Rafale de 9 rejets (< kSensorFilterResyncRejects 12 : pas de re-sync) :
  // 3 NaN, 3 hors plage, 3 sauts excessifs. Frozen doit persister à chaque tick.
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_FALSE(f.addSample(NAN, t));      t += 1000;
    TEST_ASSERT_TRUE(f.frozen());
    TEST_ASSERT_FALSE(f.addSample(15.0f, t));    t += 1000;  // > maxValue 14
    TEST_ASSERT_TRUE(f.frozen());
    TEST_ASSERT_FALSE(f.addSample(8.0f, t));     t += 1000;  // saut +1.0 > maxStep 0.15
    TEST_ASSERT_TRUE_MESSAGE(f.frozen(), "frozen doit persister pendant une rafale de rejets");
  }
  TEST_ASSERT_FALSE(f.ready(t));
}

// --- Cas (f) : reset() → frozen false ------------------------------------------
void test_frozen_cleared_by_reset(void) {
  SensorFilter f(frozenPhCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  f.reset();
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(), "reset() doit lever l'etat fige");
  // Et le run repart de zéro : 29 lectures identiques ne suffisent pas.
  for (int i = 0; i < 29; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE(f.frozen());
}

// --- Cas (g) : borne — étendue exactement == epsilon → run CASSÉ (strict <) ---
// FrozenDetector unitaire avec valeurs exactement représentables en float32
// (0.5, 100.0, 100.5, 100.25) pour éviter tout artefact d'arrondi.
void test_frozen_detector_extent_equal_epsilon_breaks_run(void) {
  FrozenDetector d(2, 0.5f);
  d.addSample(100.0f);   // ancre (run = 1)
  d.addSample(100.5f);   // étendue == 0.5 == epsilon → PAS < epsilon → ré-ancrage
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "etendue == epsilon doit casser le run (comparaison stricte <)");
  // Contre-épreuve : étendue 0.25 < 0.5 → run = 2 → figé.
  FrozenDetector d2(2, 0.5f);
  d2.addSample(100.0f);
  d2.addSample(100.25f);
  TEST_ASSERT_TRUE(d2.frozen());
}

// --- Cas (h) : non-régression — config existante (frozen désactivé, champs 0) -
// Avec phCfg() (init positionnelle courte : frozenSamples=0), même 100 lectures
// strictement identiques ne doivent JAMAIS déclarer le capteur figé, et ready()
// reste true : le comportement des 21 tests existants est préservé.
void test_frozen_disabled_with_legacy_config(void) {
  SensorFilter f(phCfg());  // frozenSamples/frozenEpsilon value-initialisés à 0
  uint32_t t = 0;
  for (int i = 0; i < 100; ++i) {
    TEST_ASSERT_TRUE(f.addSample(7.00f, t));
    TEST_ASSERT_FALSE(f.frozen());
    t += 1000;
  }
  TEST_ASSERT_TRUE(f.ready(t));
}

// --- Cas (i1) : FrozenDetector samples=0 → jamais figé -------------------------
void test_frozen_detector_samples_zero_never_frozen(void) {
  FrozenDetector d(0, 0.0005f);
  for (int i = 0; i < 1000; ++i) d.addSample(7.00f);
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "samples=0 doit desactiver la detection");
}

// --- Cas (i2) : saturation du compteur — > 65535 échantillons sans overflow ---
// Si _runCount wrappait (uint16_t), frozen() retomberait false autour de 65536.
// On injecte 70000 échantillons identiques et on vérifie que frozen() reste true
// en continu au-delà du point de wrap.
void test_frozen_detector_run_count_saturates_no_overflow(void) {
  FrozenDetector d(30, 0.5f);
  for (int i = 0; i < 70000; ++i) {
    d.addSample(100.0f);
    if (i >= 29) {
      // Dès le 30e échantillon et pour TOUJOURS (y compris après 65535).
      TEST_ASSERT_TRUE_MESSAGE(d.frozen(), "le compteur doit saturer, pas wrapper");
    }
  }
  TEST_ASSERT_TRUE(d.frozen());
  // reset() unitaire du détecteur.
  d.reset();
  TEST_ASSERT_FALSE(d.frozen());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_warmup_not_ready_until_5_valid_samples);
  RUN_TEST(test_ready_false_when_stale);
  RUN_TEST(test_median_window_7);
  RUN_TEST(test_ema_converges_to_stable_value);
  RUN_TEST(test_reject_isolated_spike_ph);
  RUN_TEST(test_reject_isolated_spike_orp);
  RUN_TEST(test_reset_triggers_rewarmup);
  RUN_TEST(test_unstable_after_consecutive_rejects);
  RUN_TEST(test_reject_nan);
  RUN_TEST(test_reject_out_of_range);
  RUN_TEST(test_slow_drift_is_tracked);
  // Re-sync + anti-boucle (correctif terrain).
  RUN_TEST(test_resync_constants_values);
  RUN_TEST(test_caseA_isolated_burst_no_resync);
  RUN_TEST(test_caseB_durable_change_triggers_resync_and_rewarmup);
  RUN_TEST(test_caseC_reseed_on_median_of_rejected);
  RUN_TEST(test_caseD_resync_loop_latches_unstable);
  RUN_TEST(test_caseE_reset_clears_resync_state);
  // Couverture résiduelle.
  RUN_TEST(test_caseF_median_window_clamped_to_capacity);
  RUN_TEST(test_caseG_raw_reflects_last_sample_even_if_rejected);
  RUN_TEST(test_caseH_rejected_median_even_window);
  RUN_TEST(test_caseI_fourth_resync_shifts_timestamps_fifo);
  // Détection capteur figé (feature-022 Passe 2).
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
