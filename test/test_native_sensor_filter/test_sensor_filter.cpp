// =============================================================================
// Tests unitaires natifs — SensorFilter, chaîne de mesure à 5 étages
// (feature-152, réécriture intégrale de cette suite — l'ancienne, 2201 lignes,
// testait spikeReject/rearm/latch/dispersion : mécanismes tous RETIRÉS)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
//
// Rappel de la conception (src/sensor_filter.h) :
//   1. lecture (coquille)
//   2. rejet de l'absurde : NaN / hors plage PHYSIQUE, comparaison STRICTE
//   3. médiane glissante (N = kSensorFilterMedianWindow) PUIS lissage
//      exponentiel de constante de temps FIXE (kSensorFilterEmaTauMs, ms),
//      α recalculé à CHAQUE échantillon depuis dt RÉELLEMENT écoulé
//   4. offset utilisateur (hors de ce module, ph_offset.h)
//   5. fraîcheur : pas de mesure ACCEPTÉE depuis maxAgeMs ⇒ vidage
//
// 🔑 LA MÉDIANE EST LE REJET D'ABERRATIONS : (N−1)/2 aberrations consécutives
// écartées PAR CONSTRUCTION, (N+1)/2 échantillons pour qu'un échelon légitime
// traverse la fenêtre — QUOI QU'IL ARRIVE. Aucun verrou, aucun état collant.
//
// Carte des livrables couverts dans ce fichier :
//   D1 — traversée d'un échelon légitime en EXACTEMENT (N+1)/2 échantillons
//   D2 — NON-VERROUILLAGE : séquences hostiles (rafales, alternance, NaN
//        intercalés, hors-plage, porte battante, échelon pH 4→8,46 réel du
//        11/10, détecteur de sonde figée inclus dans l'espace d'entrées)
//   D3 — indépendance à la cadence (même trace à 5000 ms et 3000 ms)
//   D5 — vérification (jamais choix) sur signal réel, fixture f097
//   Sécurité nommée par le gate : sonde figée, rail ORP ±1019,9 (comparaison
//        STRICTE), trou de fraîcheur, mode commun (bornes hors de ce module)
//
// D4 (validation par mutation) est un exercice MANUEL (casser sensor_filter.cpp
// puis restaurer, md5sum à l'appui) : voir le rapport de la session, pas ce
// fichier — une suite ne peut pas prouver qu'elle tue un mutant qu'on ne lui
// soumet jamais.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ indisponible sur l'hôte)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>  // listes {..} dans les boucles for-range (C++ pur, pas libc++)

#include "sensor_filter.h"  // inclut constants.h

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// Configs de référence — CALQUÉES sur sensors.cpp (production), même ordre
// positionnel que SensorFilter::Config (7 champs depuis feature-152).
// =============================================================================

static SensorFilter::Config phCfg() {
  return SensorFilter::Config{
      /*minValue*/ kPhFilterMin, /*maxValue*/ kPhFilterMax,
      /*medianWindow*/ kSensorFilterMedianWindow,
      /*emaTauMs*/ kSensorFilterEmaTauMs,
      /*maxAgeMs*/ kSensorFilterMaxAgeMs,
      /*frozenSamples*/ kSensorFrozenSamples,
      /*frozenEpsilon*/ kSensorFrozenEpsilonPh};
}

static SensorFilter::Config orpCfg() {
  return SensorFilter::Config{
      /*minValue*/ kOrpFilterMin, /*maxValue*/ kOrpFilterMax,
      /*medianWindow*/ kSensorFilterMedianWindow,
      /*emaTauMs*/ kSensorFilterEmaTauMs,
      /*maxAgeMs*/ kSensorFilterMaxAgeMs,
      /*frozenSamples*/ kSensorFrozenSamples,
      /*frozenEpsilon*/ kSensorFrozenEpsilonOrp};
}

// Détecteur figé désactivé (frozenSamples=0) : isole les étages 2/3/5 des
// tests qui ne concernent PAS la sonde figée (évite qu'une longue séquence de
// valeurs répétées ne déclenche accidentellement frozen() dans un test qui
// n'en parle pas).
static SensorFilter::Config phCfgNoFrozen() {
  SensorFilter::Config c = phCfg();
  c.frozenSamples = 0;
  return c;
}

// Médiane à 1 échantillon = la médiane EST le brut : isole le lissage
// exponentiel PUR (D3, indépendance à la cadence) du lag sample-based de la
// médiane, qui lui ne dépend QUE du nombre d'échantillons (jamais du temps).
static SensorFilter::Config emaOnlyCfg() {
  SensorFilter::Config c = phCfgNoFrozen();
  c.medianWindow = 1;
  return c;
}

// Nombres dérivés de la largeur retenue — verrouillés une fois ci-dessous,
// jamais recopiés en dur ensuite (si N change, ces dérivés suivent).
static const uint8_t kN = kSensorFilterMedianWindow;
static const int kStepSamples = (kN + 1) / 2;     // échantillons pour traverser un échelon
static const int kAbsorbSamples = (kN - 1) / 2;   // aberrations consécutives absorbées

void test_locked_median_window_is_nine(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(9, kSensorFilterMedianWindow,
      "verrou de non-derive : les derives kStepSamples/kAbsorbSamples de cette "
      "suite supposent N=9 (5 et 4)");
  TEST_ASSERT_EQUAL_INT(5, kStepSamples);
  TEST_ASSERT_EQUAL_INT(4, kAbsorbSamples);
}

// Remplit la fenêtre avec N échantillons légitimes — bruit minimal (> epsilon
// du détecteur figé) pour ne jamais déclencher frozen() par construction du
// test lui-même (un capteur VIVANT ne reste jamais rigoureusement constant).
static void warmUp(SensorFilter& f, uint32_t& t, float base) {
  for (uint8_t i = 0; i < kN; ++i) {
    const float jitter = (i % 2 == 0) ? 0.001f : -0.001f;
    f.addSample(base + jitter, t);
    t += 1000;
  }
}

// =============================================================================
// FrozenDetector — tests unitaires (classe pure, bas niveau)
// =============================================================================

void test_frozen_disabled_when_samples_zero(void) {
  FrozenDetector d(0, 0.01f);
  for (int i = 0; i < 1000; ++i) d.addSample(7.00f);
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "samples=0 => detection desactivee");
}

void test_frozen_ignores_nan_samples(void) {
  FrozenDetector d(3, 0.01f);
  d.addSample(7.00f);
  d.addSample(NAN);  // ne doit jamais casser ni alimenter le run
  d.addSample(7.00f);
  d.addSample(7.00f);
  TEST_ASSERT_TRUE(d.frozen());
}

void test_frozen_toggle_one_lsb_breaks_the_run(void) {
  // epsilon = 0.0005 (½ LSB EZO pH) : un toggle de 1 LSB (0.001) CASSE le run,
  // même répété indéfiniment (condition pool-chemistry #1).
  FrozenDetector d(5, 0.0005f);
  for (int i = 0; i < 1000; ++i) {
    d.addSample((i % 2 == 0) ? 7.000f : 7.001f);
    TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "un toggle de 1 LSB ne doit JAMAIS geler");
  }
}

void test_frozen_exact_threshold_and_reset(void) {
  FrozenDetector d(5, 0.01f);
  for (int i = 0; i < 4; ++i) { d.addSample(7.00f); TEST_ASSERT_FALSE(d.frozen()); }
  d.addSample(7.00f);  // 5e dans la bande
  TEST_ASSERT_TRUE(d.frozen());
  d.reset();
  TEST_ASSERT_FALSE(d.frozen());
}

void test_frozen_out_of_band_resumes_immediately_no_ack(void) {
  // C'est la propriété de sécurité centrale (gate pool-chemistry, condition #2) :
  // sortie IMMÉDIATE dès UNE lecture hors bande, SANS acquittement ni délai.
  FrozenDetector d(30, 0.0005f);
  for (int i = 0; i < 30; ++i) d.addSample(7.000f);
  TEST_ASSERT_TRUE(d.frozen());
  d.addSample(7.100f);  // largement hors bande
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "une seule lecture hors bande leve le gel, sans ACK");
}

// =============================================================================
// Construction / clamp de Config (sécurité structurelle)
// =============================================================================

void test_config_median_window_zero_clamped_to_one(void) {
  SensorFilter::Config c = phCfg();
  c.medianWindow = 0;
  SensorFilter f(c);
  uint32_t t = 0;
  f.addSample(7.00f, t);
  TEST_ASSERT_FALSE_MESSAGE(f.warmingUp(), "fenetre clampee a 1 : pleine des le 1er echantillon");
}

void test_config_median_window_above_capacity_clamped(void) {
  SensorFilter::Config c = phCfg();
  c.medianWindow = 250;  // > kSensorFilterMedianWindowMax
  SensorFilter f(c);
  uint32_t t = 0;
  for (uint8_t i = 0; i < kSensorFilterMedianWindowMax; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FALSE_MESSAGE(f.warmingUp(),
      "une demande hors capacite physique doit etre bornee, pas deborder le buffer fixe");
}

// =============================================================================
// Étage 2 — rejet de l'absurde (NaN / hors plage PHYSIQUE), comparaison STRICTE
// =============================================================================

void test_stage2_nan_rejected(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  TEST_ASSERT_FALSE(f.addSample(NAN, t));
  TEST_ASSERT_EQUAL_UINT8(1, f.rejectedCount());
}

void test_stage2_ph_strict_bounds(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  // Pile aux bornes : REJETÉ (comparaison stricte <= / >=).
  TEST_ASSERT_FALSE_MESSAGE(f.addSample(0.0f, t), "0.0 pile sur minValue doit etre rejete");
  TEST_ASSERT_FALSE_MESSAGE(f.addSample(14.0f, t), "14.0 pile sur maxValue doit etre rejete");
  // Juste à l'intérieur : accepté.
  TEST_ASSERT_TRUE(f.addSample(0.001f, t));
  TEST_ASSERT_TRUE(f.addSample(13.999f, t));
  // Hors plage : rejeté.
  TEST_ASSERT_FALSE(f.addSample(-0.1f, t));
  TEST_ASSERT_FALSE(f.addSample(14.1f, t));
}

// Rail ORP ±1019,9 — valeur de SATURATION du module EZO-ORP (matériel) :
// rejetée, PAS lissée. Mutation nommée par le gate : passer la comparaison en
// `<=`/`>=` doit tuer ce test (la valeur de saturation serait alors acceptée
// et entrerait dans la médiane comme une mesure).
void test_orp_rail_exact_saturation_rejected_not_smoothed(void) {
  SensorFilter f(orpCfg());
  uint32_t t = 0;
  warmUp(f, t, 400.0f);
  const float before = f.value(t - 1000);
  for (int i = 0; i < 20; ++i) {
    const bool accepted = f.addSample(kOrpFilterMax, t);  // exactement +1019.9
    TEST_ASSERT_FALSE_MESSAGE(accepted, "+1019.9 (saturation module) doit etre REJETE, pas lisse");
    t += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, before, f.value(t - 1000),
      "une serie de valeurs de saturation rejetees ne doit PAS deplacer la valeur officielle");
  // Symétrique côté bas.
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_FALSE(f.addSample(kOrpFilterMin, t));  // exactement -1019.9
    t += 1000;
  }
}

// =============================================================================
// D1 — traversée d'un échelon légitime en EXACTEMENT (N+1)/2 échantillons
// =============================================================================

void test_D1_step_traverses_in_exactly_kStepSamples(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  TEST_ASSERT_FALSE(f.warmingUp());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, f.value(t - 1000));

  // Échelon légitime : 10.00 à partir de maintenant. Tant que moins de
  // kStepSamples échantillons sont entrés, la médiane reste dominée par
  // l'ancien régime (majorité des kAbsorbSamples+... restants) : la valeur ne
  // doit PAS encore avoir rejoint 10.00. Au kStepSamples-ième, c'est fait.
  for (int i = 1; i < kStepSamples; ++i) {
    f.addSample(10.00f, t);
    t += 1000;
    TEST_ASSERT_TRUE_MESSAGE(f.median() < 9.5f,
        "la mediane ne doit pas avoir bascule avant kStepSamples echantillons");
  }
  f.addSample(10.00f, t);  // kStepSamples-ième échantillon du nouveau régime
  t += 1000;
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 10.00f, f.median(),
      "la mediane doit avoir totalement bascule en EXACTEMENT kStepSamples echantillons");
}

void test_D1_kAbsorbSamples_consecutive_aberrations_absorbed(void) {
  // (N-1)/2 = 4 aberrations consécutives, PAR CONSTRUCTION, sans aucun seuil.
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  for (int i = 0; i < kAbsorbSamples; ++i) {
    f.addSample(13.9f, t);  // aberration extrême mais dans la plage physique
    t += 1000;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 7.00f, f.median(),
        "jusqu'a kAbsorbSamples aberrations consecutives, la mediane doit tenir");
  }
  // La (kAbsorbSamples+1)-ième fait basculer la médiane (majorité atteinte).
  f.addSample(13.9f, t);
  TEST_ASSERT_FALSE_MESSAGE(fabsf(f.median() - 7.00f) < 0.05f,
      "une (kAbsorbSamples+1)-ieme aberration doit, elle, faire bouger la mediane");
}

// =============================================================================
// D2 — NON-VERROUILLAGE : aucune séquence hostile ne laisse le filtre sans
// valeur plus de kStepSamples échantillons après le retour de mesures valides
// =============================================================================
// Démarre toujours d'une fenêtre DÉJÀ PLEINE (cas qui compte : la sonde était
// en service normal avant la perturbation — c'est l'incident réel du 11/10).
// Les scénarios qui VIDENT la fenêtre par construction (porte fermée, trou de
// fraîcheur) sont des cas RECONNUS et testés séparément plus bas : leur
// reprise prend kN échantillons (un warmup complet), ce qui n'est PAS un
// verrou — c'est le mécanisme documenté (A4/C4), pas une panne.

// Combien d'échantillons de `validValue` faut-il, après une perturbation, pour
// que value() redevienne non-NaN ? Retourne -1 si jamais atteint sous maxTry.
static int samplesToRecover(SensorFilter& f, uint32_t& t, float validValue, int maxTry) {
  for (int i = 1; i <= maxTry; ++i) {
    f.addSample(validValue, t);
    t += 1000;
    if (!isnan(f.value(t))) return i;
  }
  return -1;
}

static void assertRecoversWithinStep(SensorFilter& f, uint32_t& t, float validValue,
                                     const char* scenario) {
  const int n = samplesToRecover(f, t, validValue, kN + 2);
  char msg[160];
  snprintf(msg, sizeof(msg), "%s : recuperation en %d echantillons (attendu <= %d)",
           scenario, n, kStepSamples);
  TEST_ASSERT_TRUE_MESSAGE(n >= 1 && n <= kStepSamples, msg);
}

void test_D2_burst_of_nan_then_recovery(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  for (int i = 0; i < 12; ++i) { f.addSample(NAN, t); t += 1000; }  // rafale, > kN
  TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t - 1000)),
      "une rafale de NaN est REJETEE, elle ne doit jamais vider la fenetre");
  assertRecoversWithinStep(f, t, 7.00f, "rafale de NaN (deja a jour)");
}

void test_D2_out_of_range_values_then_recovery(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  const float hostile[] = {20.0f, -3.0f, 15.0f, -1.0f, 999.0f};
  for (float v : hostile) { f.addSample(v, t); t += 1000; }
  TEST_ASSERT_FALSE(isnan(f.value(t - 1000)));
  assertRecoversWithinStep(f, t, 7.00f, "hors-plage repete");
}

void test_D2_nan_interleaved_with_out_of_range(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  const float hostile[] = {NAN, 20.0f, NAN, -5.0f, NAN, 999.0f, NAN};
  for (float v : hostile) { f.addSample(v, t); t += 1000; }
  assertRecoversWithinStep(f, t, 7.00f, "NaN intercales avec hors-plage");
}

// « Porte battante » : alternance de deux valeurs légitimes EN PLEINE FENÊTRE.
// Aucune des deux n'est une aberration (toutes deux physiquement plausibles) :
// la valeur officielle NE DOIT JAMAIS tomber à NaN pendant l'alternance — elle
// n'a aucune raison de se perdre, la médiane absorbe l'oscillation.
void test_D2_door_banging_mid_window_never_drops_to_nan(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  for (int i = 0; i < 24; ++i) {
    f.addSample((i % 2 == 0) ? 4.0f : 10.0f, t);
    t += 1000;
    TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t)),
        "une oscillation de valeurs LEGITIMES ne doit jamais produire un trou");
  }
}

// L'INCIDENT RÉEL DU 11/10/2026 : sonde plongée en étalon pH 4, puis replongée
// en bassin (pH 8,46). 141 rejets avaient figé l'ancienne médiane à 4,62 —
// cette traversée doit désormais s'achever en EXACTEMENT kStepSamples
// échantillons, sans aucune intervention.
void test_D2_real_incident_ph4_to_ph846_step(void) {
  // NOTE : la MÉDIANE (stage 3a) traverse en EXACTEMENT kStepSamples — c'est
  // l'ancien défaut corrigé (141 rejets, médiane figée à 4,62). La VALEUR
  // OFFICIELLE (stage 3a+3b, value()) porte en plus le lissage exponentiel
  // (τ=45s) : un choix DÉLIBÉRÉ et SÉPARÉ, qui la fait progresser plus
  // lentement vers 8,46 — ce n'est PAS un verrou, juste le lissage voulu. Ce
  // que ce test verrouille : AUCUN trou (NaN) pendant toute la traversée, et
  // la médiane bascule bien en kStepSamples, PLUS JAMAIS figée.
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 4.0f);  // sonde dans l'étalon pH 4
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, f.median());
  TEST_ASSERT_FALSE(isnan(f.value(t - 1000)));

  for (int i = 1; i < kStepSamples; ++i) {
    f.addSample(8.46f, t);
    t += 1000;
    TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t)),
        "aucun trou pendant la traversee : c'est exactement le defaut corrige "
        "(141 rejets, mediane figee a 4,62, le 11/10)");
  }
  TEST_ASSERT_TRUE_MESSAGE(f.median() < 8.0f,
      "avant kStepSamples echantillons, la mediane ne doit pas encore avoir bascule");
  f.addSample(8.46f, t);
  t += 1000;
  TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t)), "AUCUN blocage, AUCUNE intervention requise");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 8.46f, f.median(),
      "la MEDIANE doit avoir totalement bascule en EXACTEMENT kStepSamples echantillons");
  TEST_ASSERT_TRUE_MESSAGE(f.value(t) > 4.0f,
      "la valeur officielle doit avoir commence a progresser vers 8,46 (lissage en cours)");
}

// Le détecteur de sonde figée DANS L'ESPACE D'ENTRÉES DE D2 (recommandation du
// gate pool-chemistry) : c'est désormais le SEUL chemin restant vers « pas de
// valeur » durable — il doit, lui aussi, se lever sans verrou.
void test_D2_frozen_probe_then_single_live_reading_recovers_immediately(void) {
  SensorFilter f(phCfg());  // détecteur figé ACTIF ici
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  for (int i = 0; i < kSensorFrozenSamples; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.value(t)), "sonde figee => valeur officielle NaN (dosage refuse)");
  // UNE lecture hors bande => reprise IMMÉDIATE, sans acquittement.
  f.addSample(7.10f, t);
  t += 1000;
  TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t)),
      "une seule lecture vivante doit suffire a lever le gel, sans ACK ni delai");
}

// Scénario COMPOSITE : rafale + alternance + NaN + hors-plage + sonde figée,
// tous enchaînés sur la MÊME fenêtre. Le test central du lot : peu importe la
// combinaison, le retour de mesures valides rend une valeur en <= kStepSamples
// échantillons (hors cas de la sonde figée, qui reprend en 1).
void test_D2_composite_hostile_sequence_never_locks(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);

  // 1) rafale de NaN
  for (int i = 0; i < 6; ++i) { f.addSample(NAN, t); t += 1000; }
  // 2) hors-plage
  for (float v : {20.0f, -3.0f}) { f.addSample(v, t); t += 1000; }
  // 3) porte battante légitime
  for (int i = 0; i < 10; ++i) { f.addSample((i % 2 == 0) ? 5.0f : 9.0f, t); t += 1000; }
  TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t)),
      "a ce stade aucune sequence n'a du vider la fenetre : valeur toujours presente");
  // 4) sonde figée (30 lectures bit-identiques)
  for (int i = 0; i < kSensorFrozenSamples; ++i) { f.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_TRUE(isnan(f.value(t)));
  // 5) retour de mesures valides : UNE VALEUR DIFFÉRENTE de celle qui a gelé
  // (7.50, hors bande epsilon) — reprendre avec la MÊME valeur ne lèverait
  // jamais le gel (ce n'est pas une "reprise", c'est la continuation du gel).
  assertRecoversWithinStep(f, t, 7.50f, "composite : rafale+hors-plage+alternance+sonde figee");
}

// Cas reconnu (PAS gouverné par le bound D2, documenté séparément) : la porte
// se ferme en pleine fenêtre. On vide — c'est voulu (A4), reprise = kN.
void test_D2_gate_closing_clears_and_refills_in_kN_not_a_lock(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  f.setContext(/*waterPresent*/ false, /*measuresValid*/ true, t);  // porte FERMÉE
  TEST_ASSERT_TRUE_MESSAGE(f.gateClosed(), "porte doit etre fermee");
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.value(t)), "porte fermee => pas de valeur (vidage immediat)");
  f.setContext(true, true, t);  // réouverture : aucune action, se remplit naturellement
  int filled = 0;
  for (int i = 0; i < kN; ++i) {
    f.addSample(7.00f, t);
    t += 1000;
    if (!isnan(f.value(t))) { filled = i + 1; break; }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(kN, filled,
      "reouverture de porte : warmup complet (kN), PAS un verrou, c'est le mecanisme documente");
}

// =============================================================================
// D3 — indépendance à la cadence : même comportement en échantillons, mêmes
// durées en millisecondes, à 5000 ms et à 3000 ms. La cadence de PRODUCTION
// reste 5000 ms (kPhOrpSensorIntervalMs, C1 du gate) — 3000 ms est une
// MUTATION DE TEST, jamais une valeur livrée.
// =============================================================================

void test_D3_step_traversal_identical_in_samples_at_5000ms(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  for (uint8_t i = 0; i < kN; ++i) { f.addSample(7.00f, t); t += 5000; }
  for (int i = 1; i < kStepSamples; ++i) { f.addSample(10.00f, t); t += 5000; }
  TEST_ASSERT_TRUE(f.median() < 9.5f);
  f.addSample(10.00f, t);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.00f, f.median());
}

void test_D3_step_traversal_identical_in_samples_at_3000ms(void) {
  // MUTATION DE TEST (cadence 3000 ms) : le nombre d'échantillons pour
  // traverser l'échelon doit rester EXACTEMENT kStepSamples, inchangé.
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  for (uint8_t i = 0; i < kN; ++i) { f.addSample(7.00f, t); t += 3000; }
  for (int i = 1; i < kStepSamples; ++i) { f.addSample(10.00f, t); t += 3000; }
  TEST_ASSERT_TRUE(f.median() < 9.5f);
  f.addSample(10.00f, t);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.00f, f.median());
}

// Ce qui se CHRONOMÈTRE (fraîcheur, kSensorFilterMaxAgeMs) ne dépend JAMAIS de
// la cadence utilisée pour y parvenir : seul le temps RÉEL écoulé compte.
void test_D3_freshness_threshold_identical_in_ms_regardless_of_cadence(void) {
  for (uint32_t step : {uint32_t(3000), uint32_t(5000)}) {
    SensorFilter f(phCfgNoFrozen());
    uint32_t t = 0;
    f.addSample(7.00f, t);
    const uint32_t lastValid = t;
    TEST_ASSERT_FALSE(f.stale(lastValid + kSensorFilterMaxAgeMs));       // frontière : pas encore
    TEST_ASSERT_TRUE(f.stale(lastValid + kSensorFilterMaxAgeMs + 1));    // 1 ms de plus : perime
    (void)step;  // la cadence n'intervient nulle part dans ce calcul : c'est le point
  }
}

// LE TEST QUI VALIDE m3 (D4) : la convergence du lissage exponentiel, mesurée
// à un même temps ÉCOULÉ (ms), doit être LA MÊME à 5000 ms et à 3000 ms — ce
// que seul un α recalculé depuis dt RÉEL (et non un α constant par cycle)
// permet. medianWindow=1 isole le lissage pur du lag sample-based de la
// médiane (qui, lui, est légitimement différent en ms selon la cadence).
static float emaValueAfter(uint32_t cadenceMs, uint32_t elapsedTargetMs, float v0, float v1) {
  SensorFilter f(emaOnlyCfg());
  uint32_t t = 0;
  f.addSample(v0, t);  // amorçage : filtered = v0 sans lissage (1ere fenetre pleine)
  while (t < elapsedTargetMs) {
    t += cadenceMs;
    f.addSample(v1, t);
  }
  return f.value(t);
}

void test_D3_ema_convergence_identical_in_ms_at_5000_vs_3000(void) {
  // Points de comparaison multiples de 15000 (PPCM de 5000 et 3000) : les deux
  // cadences atteignent exactement ces instants avec un nombre entier de pas.
  const uint32_t checkpoints[] = {15000, 30000, 60000, 150000};
  for (uint32_t cp : checkpoints) {
    const float at5000 = emaValueAfter(5000, cp, 7.00f, 8.00f);
    const float at3000 = emaValueAfter(3000, cp, 7.00f, 8.00f);
    char msg[128];
    snprintf(msg, sizeof(msg),
             "convergence EMA a t=%u ms : 5000ms=%.4f vs 3000ms=%.4f (doivent concorder)",
             (unsigned)cp, (double)at5000, (double)at3000);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, at5000, at3000, msg);
  }
}

// =============================================================================
// Sécurité nommée par le gate — sonde figée (version SensorFilter complète,
// au-delà du FrozenDetector nu testé plus haut)
// =============================================================================

void test_security_frozen_probe_30_bit_identical_blocks_dosage(void) {
  SensorFilter f(phCfg());
  uint32_t t = 0;
  warmUp(f, t, 6.80f);
  for (int i = 0; i < kSensorFrozenSamples; ++i) { f.addSample(6.80f, t); t += 1000; }
  TEST_ASSERT_TRUE(f.frozen());
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.value(t)),
      "sonde figee => valeur officielle NaN => evaluateDose() refuse (ReadingNaNFrozen)");
}

// =============================================================================
// Trou de fraîcheur (5 min) puis retour — C4, pool-chemistry
// =============================================================================
// NaN pendant l'absence, PAS de médiane bâtie à cheval (vidage immédiat), et
// reprise en EXACTEMENT kN échantillons (un warmup complet) une fois les
// mesures valides de retour — _clearWindow() remet _bufCount à 0, donc la
// fenêtre doit se REMPLIR entièrement avant de redevenir non-NaN. (Ce N est
// le nombre pertinent ici, pas (N+1)/2 : il n'y a plus de fenêtre PARTIELLEMENT
// pleine à faire basculer, elle est VIDE.)
void test_security_freshness_gap_5min_then_return(void) {
  SensorFilter f(phCfgNoFrozen());
  uint32_t t = 0;
  warmUp(f, t, 7.00f);
  TEST_ASSERT_FALSE(isnan(f.value(t - 1000)));

  const uint32_t gapMs = 5UL * 60UL * 1000UL;  // 5 min >> kSensorFilterMaxAgeMs (20 s)
  t += gapMs;
  f.setContext(true, true, t);  // la porte reste ouverte : seule la fraîcheur tranche
  TEST_ASSERT_TRUE_MESSAGE(isnan(f.value(t)), "silence > maxAgeMs => NaN, sans mediane a cheval");

  int filled = 0;
  for (int i = 0; i < kN; ++i) {
    f.addSample(7.00f, t);
    t += 1000;
    if (!isnan(f.value(t))) { filled = i + 1; break; }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(kN, filled,
      "apres un vidage par peremption, la reprise exige un warmup COMPLET (kN), "
      "pas kStepSamples : la fenetre est VIDE, pas partiellement pleine");
}

// =============================================================================
// Mode commun (feature-137) — TROU RÉSIDUEL HONNÊTE, documenté par le gate
// =============================================================================
// Une perturbation de mode commun produit des séries FAUSSES MAIS COHÉRENTES :
// exactement ce qu'une médiane ne peut PAS distinguer d'une vraie mesure. Ce
// test PROUVE le constat du gate (le filtre ne protège pas ce cas) plutôt que
// de le laisser implicite — c'est aux plafonds de DOSE (dosing_logic, déjà
// couverts par test_native_dosing) de borner le dommage, PAS à ce module.
void test_security_common_mode_is_smoothed_not_rejected_by_design(void) {
  SensorFilter f(orpCfg());
  uint32_t t = 0;
  // 60 échantillons faux mais cohérents : un ORP plausible qui dérive lentement
  // (mode commun), jamais hors plage, jamais identique au bit (pas de gel).
  float v = 450.0f;
  for (int i = 0; i < 60; ++i) {
    v += 0.5f;  // dérive lente et cohérente
    TEST_ASSERT_TRUE_MESSAGE(f.addSample(v, t),
        "une derive lente et coherente est, par construction, ACCEPTEE : "
        "le filtre ne peut pas la distinguer d'une vraie mesure");
    t += 1000;
  }
  TEST_ASSERT_FALSE_MESSAGE(isnan(f.value(t - 1000)),
      "aucun mecanisme de ce module ne bloque le mode commun : c'est le "
      "plafond de DOSE (dosing_logic) qui borne le dommage, voir test_native_dosing");
}

// =============================================================================
// D5 — VÉRIFICATION sur signal réel (fixture f097), JAMAIS un choix de valeur
// =============================================================================
// La fixture sert à VÉRIFIER que la conception tient sur du bruit réel, pas à
// calibrer quoi que ce soit : aucune constante de ce fichier n'est dérivée
// d'une mesure sur cette fixture (exigence de généricité, feature-152).

struct FixtureRow { uint32_t tMs; float phRaw; float orpRaw; };

static int loadFixture(FixtureRow* out, int cap) {
  const char* prefixes[4] = {"", "../", "../../", "../../../"};
  char path[256];
  FILE* fp = NULL;
  for (int i = 0; i < 4 && fp == NULL; ++i) {
    snprintf(path, sizeof(path), "%stest/fixtures/f097_purge_noise_20260920.csv", prefixes[i]);
    fp = fopen(path, "rb");
  }
  if (fp == NULL) return -1;
  char line[256];
  int n = 0;
  while (n < cap && fgets(line, sizeof(line), fp) != NULL) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    if (strncmp(line, "t_ms", 4) == 0) continue;  // en-tête
    unsigned long tMs = 0;
    float phRaw = 0, orpRaw = 0, phF = 0, orpF = 0, temp = 0;
    const int got = sscanf(line, "%lu,%f,%f,%f,%f,%f", &tMs, &phRaw, &orpRaw, &phF, &orpF, &temp);
    if (got != 6) continue;
    out[n].tMs = (uint32_t)tMs;
    out[n].phRaw = phRaw;
    out[n].orpRaw = orpRaw;
    ++n;
  }
  fclose(fp);
  return n;
}

// Ballottement = moyenne des |différences| entre échantillons consécutifs —
// une mesure GÉNÉRIQUE de bruit résiduel, pas un seuil choisi sur la fixture.
static float meanAbsConsecutiveDiff(const float* v, int n) {
  if (n < 2) return 0.0f;
  float sum = 0.0f;
  int count = 0;
  for (int i = 1; i < n; ++i) {
    if (isnan(v[i]) || isnan(v[i - 1])) continue;
    sum += fabsf(v[i] - v[i - 1]);
    ++count;
  }
  return count > 0 ? sum / (float)count : 0.0f;
}

void test_D5_fixture_f097_smoothing_quality_does_not_regress(void) {
  static FixtureRow rows[512];
  const int n = loadFixture(rows, 512);
  TEST_ASSERT_TRUE_MESSAGE(n > 250, "fixture f097 introuvable ou tronquee (attendu ~300 lignes)");

  SensorFilter f(phCfg());  // config de PRODUCTION, détecteur figé ACTIF
  static float rawSeries[512];
  static float filteredSeries[512];
  int nanCount = 0;
  for (int i = 0; i < n; ++i) {
    f.addSample(rows[i].phRaw, rows[i].tMs);
    rawSeries[i] = rows[i].phRaw;
    filteredSeries[i] = f.value(rows[i].tMs);
    if (isnan(filteredSeries[i])) ++nanCount;
  }

  // VÉRIFICATION (pas un choix) : le lissage réduit le ballottement sur un
  // signal réel — propriété générique de toute médiane+EMA, pas un chiffre
  // emprunté à cette fixture.
  const float rawBallot = meanAbsConsecutiveDiff(rawSeries, n);
  const float filteredBallot = meanAbsConsecutiveDiff(filteredSeries, n);
  char msg[160];
  snprintf(msg, sizeof(msg), "ballottement brut=%.4f lisse=%.4f (le lisse doit etre nettement inferieur)",
           (double)rawBallot, (double)filteredBallot);
  TEST_ASSERT_TRUE_MESSAGE(filteredBallot < rawBallot, msg);
  TEST_ASSERT_TRUE_MESSAGE(filteredBallot > 0.0f,
      "le lisse ne doit pas etre degenere (constant) : il doit suivre le signal");

  // VÉRIFICATION : seul le warmup initial (kN-1 échantillons) est NaN — « eau
  // établie, porte ouverte » (en-tête fixture) ne doit produire AUCUN autre trou.
  TEST_ASSERT_EQUAL_INT_MESSAGE(kN - 1, nanCount,
      "seul le warmup initial doit etre NaN sur ce signal etabli, porte ouverte");

  // VÉRIFICATION : le bruit réel (épisode purge f097, mémoire projet : dispersion
  // mesurée ~0,36-0,40 pH) ne doit JAMAIS déclencher le détecteur de sonde figée
  // — l'epsilon ½ LSB est, par construction, cassé par n'importe quel bruit vivant.
  TEST_ASSERT_FALSE_MESSAGE(f.frozen(),
      "du bruit de sonde REEL ne doit jamais etre pris pour une sonde figee");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_locked_median_window_is_nine);

  // FrozenDetector (bas niveau)
  RUN_TEST(test_frozen_disabled_when_samples_zero);
  RUN_TEST(test_frozen_ignores_nan_samples);
  RUN_TEST(test_frozen_toggle_one_lsb_breaks_the_run);
  RUN_TEST(test_frozen_exact_threshold_and_reset);
  RUN_TEST(test_frozen_out_of_band_resumes_immediately_no_ack);

  // Config / construction
  RUN_TEST(test_config_median_window_zero_clamped_to_one);
  RUN_TEST(test_config_median_window_above_capacity_clamped);

  // Étage 2
  RUN_TEST(test_stage2_nan_rejected);
  RUN_TEST(test_stage2_ph_strict_bounds);
  RUN_TEST(test_orp_rail_exact_saturation_rejected_not_smoothed);

  // D1
  RUN_TEST(test_D1_step_traverses_in_exactly_kStepSamples);
  RUN_TEST(test_D1_kAbsorbSamples_consecutive_aberrations_absorbed);

  // D2 — non-verrouillage
  RUN_TEST(test_D2_burst_of_nan_then_recovery);
  RUN_TEST(test_D2_out_of_range_values_then_recovery);
  RUN_TEST(test_D2_nan_interleaved_with_out_of_range);
  RUN_TEST(test_D2_door_banging_mid_window_never_drops_to_nan);
  RUN_TEST(test_D2_real_incident_ph4_to_ph846_step);
  RUN_TEST(test_D2_frozen_probe_then_single_live_reading_recovers_immediately);
  RUN_TEST(test_D2_composite_hostile_sequence_never_locks);
  RUN_TEST(test_D2_gate_closing_clears_and_refills_in_kN_not_a_lock);

  // D3 — indépendance à la cadence
  RUN_TEST(test_D3_step_traversal_identical_in_samples_at_5000ms);
  RUN_TEST(test_D3_step_traversal_identical_in_samples_at_3000ms);
  RUN_TEST(test_D3_freshness_threshold_identical_in_ms_regardless_of_cadence);
  RUN_TEST(test_D3_ema_convergence_identical_in_ms_at_5000_vs_3000);

  // Sécurité nommée par le gate
  RUN_TEST(test_security_frozen_probe_30_bit_identical_blocks_dosage);
  RUN_TEST(test_security_freshness_gap_5min_then_return);
  RUN_TEST(test_security_common_mode_is_smoothed_not_rejected_by_design);

  // D5 — vérification sur signal réel
  RUN_TEST(test_D5_fixture_f097_smoothing_quality_does_not_regress);

  return UNITY_END();
}
