// =============================================================================
// Tests unitaires natifs — Offset logiciel de mesure pH (ph_offset, feature-067)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
// On teste le COMPORTEMENT observable de la logique PURE de src/ph_offset.h
// (ADR-0017, comme dosing_logic / measure_validity_logic) :
//   - applyPhOffset(raw, offset)  → mesure corrigée, NaN in → NaN out
//   - isPhOffsetValid(offset)     → offset exploitable (SALUBRITÉ de domaine
//     uniquement depuis feature-168, PAS une borne de réglage, cf. D1.3)
//
// Le fichier couvre quatre familles :
//
//   D1 — Application, salubrité, propagation NaN, et surtout INVARIANCE PAR
//        TRANSLATION du filtre réel (src/sensor_filter) : filtre(x + c) =
//        filtre(x) + c. C'est la justification mathématique de la décision
//        d'architecture « offset appliqué EN SORTIE des accesseurs » — elle est
//        démontrée ici sur le filtre de production, pas supposée.
//
//   D2 — Non-régression du filtre pH : compteur de rejets, figé, périmé et
//        porte STRICTEMENT identiques avec et sans offset. Le filtre ne voit
//        que le brut ; l'offset ne peut donc pas déplacer un seuil de sécurité.
//
//   D3 — ARTICULATION offset ↔ seuils de sécurité (feature-168, ADR-0098) : les
//        seuils chimiques portent sur le pH CORRIGÉ, les deux filets de dernier
//        recours sur le pH BRUT, FIXES (5,50 / 8,50) et INDÉPENDANTS de
//        l'offset depuis feature-168 (ils ne DÉRIVENT plus de kPhOffsetMax, qui
//        a disparu). La machine à états elle-même
//        (PumpControllerClass::isPhGuardBlocking / isOrpInhibitedByPh) n'est PAS
//        pure — voir la note en fin de fichier. L'INDÉPENDANCE du filet brut
//        lui-même (quel que soit l'offset) est verrouillée dans
//        test_native_ph_guard (revision C11 du gate pool-chemistry) ; ici on ne
//        teste QUE ce qui met l'offset en jeu CÔTÉ MESURE.
//
//   D4 — HORODATAGE DE JUSTESSE (feature-156, étendu feature-168/C10/C12) :
//        isPhOffsetStale(offset, offsetSeq, calSeq, offsetEpoch, nowEpoch,
//        confirmIntervalDays, unusualAbs) et nextJustnessSeq(a, b). Deux causes
//        INDÉPENDANTES de caducité : (a) correction antérieure au dernier
//        étalonnage réussi (feature-156, inchangée) ; (b) correction INHABITUELLE
//        non reconfirmée depuis plus de confirmIntervalDays jours (feature-168,
//        C10) — isPhOffsetExpired() isolément, fail-open sur horloge non fiable.
//
// Chaîne de production reproduite (src/sensors.cpp + pump_controller.cpp,
// feature-152) :
//   EZO → _lastPh (brut) → _phFilter.addSample(brut, now) → value(now)
//         (médiane + lissage, NaN si warmup/figé/périmé/porte fermée)
//       → getPhFiltered() = applyPhOffset(value(now), offset)   [/data, WS, MQTT, dosage]
//       → getPhValueRaw() = value(now)                          [butées pH BRUT]
//       → pump_controller RECALCULE applyPhOffset(getPhValueRaw(), offset)
//         pour les seuils chimiques [pH CORRIGÉ]
// =============================================================================

#include <unity.h>
#include <math.h>   // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdio.h>  // snprintf (messages des balayages D3)

#include "ph_guard_logic.h"  // phGuardBlocksBelow / phGuardBlocksAbove (D3)
#include "ph_offset.h"
#include "sensor_filter.h"

void setUp(void) {}
void tearDown(void) {}

// Config filtre pH calquée sur constants.h (même ordre positionnel que
// SensorFilter::Config, 7 champs depuis feature-152) : plage physique [0;14],
// médiane kSensorFilterMedianWindow, lissage kSensorFilterEmaTauMs, fraîcheur
// kSensorFilterMaxAgeMs, détecteur figé kSensorFrozenSamples/EpsilonPh.
static SensorFilter::Config phCfg() {
  return SensorFilter::Config{
      /*minValue*/ kPhFilterMin,
      /*maxValue*/ kPhFilterMax,
      /*medianWindow*/ kSensorFilterMedianWindow,
      /*emaTauMs*/ kSensorFilterEmaTauMs,
      /*maxAgeMs*/ kSensorFilterMaxAgeMs,
      /*frozenSamples*/ kSensorFrozenSamples,
      /*frozenEpsilon*/ kSensorFrozenEpsilonPh};
}

// =============================================================================
// D1.1 — Application de l'offset : positif, négatif, nul
// =============================================================================

// Offset nul = identité stricte, sur toute la plage utile (aucune dérive
// numérique introduite quand l'utilisateur ne configure rien).
void test_zero_offset_is_identity(void) {
  const float samples[] = {0.0f, 6.80f, 7.00f, 7.20f, 7.60f, 14.0f};
  for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    TEST_ASSERT_EQUAL_FLOAT(samples[i], applyPhOffset(samples[i], 0.0f));
  }
}

// Cas nominal de la spec : 7.00 + 0.30 = 7.30 au centième près.
void test_positive_offset_applied_to_hundredth(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 7.30f, applyPhOffset(7.00f, 0.30f));
}

void test_negative_offset_applied_to_hundredth(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 6.70f, applyPhOffset(7.00f, -0.30f));
}

// Pas de saisie = 0,01 pH : la correction la plus fine doit être visible.
void test_offset_hundredth_step_is_visible(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 7.01f, applyPhOffset(7.00f, 0.01f));
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 6.98f, applyPhOffset(6.99f, -0.01f));
}

// *** CRITÈRE D'ACCEPTATION DE LA SPEC *** — l'ancienne borne (±0,50) a
// disparu : +1,00 est désormais une correction ORDINAIRE, appliquée à la
// lettre. C'est le défaut le plus coûteux du lot (feature-168) si un écrêtage
// résiduel traînait ailleurs : il ramènerait +1,00 à +0,50 en silence.
void test_offset_plus_one_is_applied_exactly(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 8.00f, applyPhOffset(7.00f, 1.00f));
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(1.00f), "+1,00 doit etre une correction valide");
}

// L'offset est ADDITIF et symétrique : appliquer +c puis −c revient au brut.
void test_offset_is_reversible(void) {
  const float raw = 7.13f;
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, raw,
                           applyPhOffset(applyPhOffset(raw, 0.37f), -0.37f));
}

// VERROU DE COMPORTEMENT : applyPhOffset ne borne PAS le résultat à [0 ; 14].
// C'est voulu — le bornage de plausibilité appartient au filtre, qui travaille
// sur le BRUT en amont. Documenté ici pour qu'un futur clamp soit un choix
// explicite et non un effet de bord.
void test_result_is_not_clamped_to_ph_scale(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 14.40f, applyPhOffset(13.90f, 0.50f));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, -0.40f, applyPhOffset(0.10f, -0.50f));
}

// =============================================================================
// D1.2 — Propagation NaN (CRITÈRE DE SÉCURITÉ)
// =============================================================================
// Une mesure absente (capteur stale, filtre non amorcé, EZO injoignable) ne doit
// JAMAIS devenir une valeur : sinon l'offset lèverait les gardes `isnan` qui
// protègent le dosage. Les offsets choisis ici n'ont plus de « bornes » dont ils
// seraient représentatifs (feature-168) : 0,50 reste le seuil INHABITUEL
// (kPhOffsetUnusualAbs), 14,0 l'extrémité de la SALUBRITÉ de domaine
// (kPhOffsetScaleSpan) — deux valeurs réellement nommées, pas des bornes de
// réglage disparues.

void test_nan_raw_stays_nan_whatever_the_offset(void) {
  const float offsets[] = {0.0f, 0.30f, -0.30f, kPhOffsetUnusualAbs,
                            -kPhOffsetUnusualAbs, kPhOffsetScaleSpan, -kPhOffsetScaleSpan};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    TEST_ASSERT_TRUE_MESSAGE(isnan(applyPhOffset(NAN, offsets[i])),
                             "mesure absente + offset => doit rester NaN");
  }
}

// Symétrique : un offset corrompu (NaN) ne doit pas DÉTRUIRE une mesure valide.
// Fail-safe : on retourne le brut intact plutôt que de propager le NaN.
void test_nan_offset_is_neutralized_and_preserves_raw(void) {
  TEST_ASSERT_EQUAL_FLOAT(7.13f, applyPhOffset(7.13f, NAN));
  TEST_ASSERT_EQUAL_FLOAT(6.80f, applyPhOffset(6.80f, NAN));
}

void test_nan_raw_and_nan_offset_stays_nan(void) {
  TEST_ASSERT_TRUE(isnan(applyPhOffset(NAN, NAN)));
}

// Un offset infini (valeur corrompue en NVS) ne fabrique pas non plus de mesure
// à partir d'une absence de mesure.
void test_nan_raw_with_infinite_offset_stays_nan(void) {
  TEST_ASSERT_TRUE(isnan(applyPhOffset(NAN, INFINITY)));
  TEST_ASSERT_TRUE(isnan(applyPhOffset(NAN, -INFINITY)));
}

// =============================================================================
// D1.3 — isPhOffsetValid : SALUBRITÉ DE DOMAINE (feature-168), plus une borne
// =============================================================================
// ⚠️ CHANGEMENT DE NATURE (feature-168, condition C3 du gate) : avant cette
// spec, isPhOffsetValid() REFUSAIT tout |offset| > 0,50 — une borne de réglage.
// Depuis, elle n'écarte plus que NaN, l'infini, ou une valeur si extravagante
// (> 14 pH, l'échelle entière) qu'elle ne peut être qu'une corruption ou une
// faute de saisie (kPhOffsetScaleSpan). Un |offset| "inhabituel" (> 0,50,
// kPhOffsetUnusualAbs) n'est PLUS refusé ici : il est accepté et signalé
// AILLEURS (avertissement UI, log + alerte MQTT) — cf. ph_offset.h.

void test_valid_offset_inside_range(void) {
  TEST_ASSERT_TRUE(isPhOffsetValid(0.0f));
  TEST_ASSERT_TRUE(isPhOffsetValid(0.30f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-0.30f));
  TEST_ASSERT_TRUE(isPhOffsetValid(0.01f));
}

// L'ANCIENNE borne ±0,50 est désormais une correction tout à fait ORDINAIRE :
// aucun refus, aucun écrêtage — exactement comme n'importe quelle autre valeur
// du domaine. Verrou de non-régression : si un futur commit réintroduit un
// refus ou un clamp à 0,50, CE test doit casser.
void test_former_bound_of_050_is_now_an_ordinary_value(void) {
  TEST_ASSERT_TRUE(isPhOffsetValid(0.50f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-0.50f));
  TEST_ASSERT_TRUE(isPhOffsetValid(0.51f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-0.51f));
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(0.80f), "0,80 n'est plus hors domaine");
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(13.9f), "13,9 reste dans l'echelle pH");
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(-0.90f), "-0,90 reste dans l'echelle pH");
}

// Frontières EXACTES de la salubrité de domaine : ±14,0 (kPhOffsetScaleSpan)
// INCLUSES, ±14,1 REFUSÉES. (14,0 est exactement représentable en binaire : la
// comparaison est donc stricte et déterministe.)
void test_exact_scale_span_bounds_are_accepted(void) {
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(kPhOffsetScaleSpan), "+14,0 doit etre accepte");
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(-kPhOffsetScaleSpan), "-14,0 doit etre accepte");
  TEST_ASSERT_TRUE(isPhOffsetValid(13.9f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-13.9f));
}

void test_just_outside_scale_span_is_refused(void) {
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(14.01f), "+14,01 doit etre refuse");
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(-14.01f), "-14,01 doit etre refuse");
  TEST_ASSERT_FALSE(isPhOffsetValid(14.1f));
  TEST_ASSERT_FALSE(isPhOffsetValid(-14.1f));
}

void test_nan_offset_is_invalid(void) {
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(NAN), "NaN ne doit jamais etre exploitable");
}

// Salubrité stricte : NVS corrompue, POST malveillant, cast raté — plus
// aucune valeur FINIE du domaine pH n'est "absurde" en soi (c'est le choix de
// la spec), seules NaN/infini/une valeur hors échelle le restent.
void test_absurd_offsets_are_invalid(void) {
  TEST_ASSERT_FALSE(isPhOffsetValid(14.1f));
  TEST_ASSERT_FALSE(isPhOffsetValid(-14.1f));
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(1e30f), "1e30 doit etre refuse (salubrite)");
  TEST_ASSERT_FALSE(isPhOffsetValid(-1e30f));
  TEST_ASSERT_FALSE(isPhOffsetValid(1e9f));
  TEST_ASSERT_FALSE(isPhOffsetValid(-1e9f));
  TEST_ASSERT_FALSE(isPhOffsetValid(INFINITY));
  TEST_ASSERT_FALSE(isPhOffsetValid(-INFINITY));
}

// Valeurs ACCEPTÉES explicitement citées par les critères d'acceptation de la
// spec (feature-168) : +13,9 / +1,00 / −0,90.
void test_spec_accepted_values(void) {
  TEST_ASSERT_TRUE(isPhOffsetValid(13.9f));
  TEST_ASSERT_TRUE(isPhOffsetValid(1.00f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-0.90f));
}

// Les deux constantes NOMMÉES qui remplacent les anciennes bornes de réglage —
// ⚠️ NE PAS les confondre : kPhOffsetUnusualAbs est un SIGNAL (pas un refus),
// kPhOffsetScaleSpan est la SEULE salubrité restante. Elles ne sont plus
// symétriques par construction d'une borne de réglage (il n'y en a plus) : ce
// sont deux grandeurs de nature différente qui, numériquement, sont chacune
// symétriques en elles-mêmes (±0,50 et ±14,0).
void test_offset_named_constants(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, kPhOffsetUnusualAbs);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 14.0f, kPhOffsetScaleSpan);
  TEST_ASSERT_TRUE_MESSAGE(kPhOffsetUnusualAbs < kPhOffsetScaleSpan,
                           "le signal doit rester bien EN DECA de la salubrite");
}

// =============================================================================
// D1.4 — INVARIANCE PAR TRANSLATION du filtre réel : filtre(x + c) = filtre(x) + c
// =============================================================================
// Justification de la décision d'architecture (offset EN SORTIE) : appliquer
// l'offset après le filtre donne le MÊME résultat que l'appliquer avant, donc on
// choisit la sortie (prise en compte immédiate + filtre sur signal brut).
// Démontré ici sur le module de production, pas sur un modèle.

// Séquence pH réaliste, 30 échantillons, loin des bornes de plausibilité
// [0 ; 14] pour que la translation ne heurte aucun bornage physique (étage 2).
static float seqAt(int i) {
  const float pattern[10] = {7.00f, 7.08f, 6.95f, 7.12f, 7.03f,
                             6.98f, 7.15f, 7.05f, 6.92f, 7.10f};
  return pattern[i % 10];
}

// Cœur de la démonstration : deux filtres, l'un nourri en brut, l'autre nourri
// en brut + c. À chaque pas, filtreB.value(t) == filtreA.value(t) + c, ce qui
// est exactement applyPhOffset(filtreA.value(t), c).
static void assertTranslationInvariance(float c) {
  SensorFilter fa(phCfg());
  SensorFilter fb(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) {
    const float raw = seqAt(i);
    const bool acceptedA = fa.addSample(raw, t);
    const bool acceptedB = fb.addSample(raw + c, t);
    // Même décision d'acceptation : l'étage 2 (NaN / hors plage PHYSIQUE) et la
    // médiane ne comparent jamais à un état interne dépendant de l'historique
    // dans une bande relative — ils sont donc insensibles à une translation.
    TEST_ASSERT_EQUAL_MESSAGE(acceptedA, acceptedB,
                              "acceptation identique sous translation");
    const float va = fa.value(t);
    const float vb = fb.value(t);
    if (!isnan(va)) {
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
          1e-4f, vb, applyPhOffset(va, c),
          "filtre(x+c) doit valoir filtre(x)+c");
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
          1e-4f, fb.median(), applyPhOffset(fa.median(), c),
          "mediane(x+c) doit valoir mediane(x)+c");
    } else {
      // Warmup : les deux filtres sont NaN au même instant (même amorçage).
      TEST_ASSERT_TRUE(isnan(vb));
    }
    t += 1000;
  }
}

void test_filter_translation_invariance_positive_offset(void) {
  assertTranslationInvariance(0.30f);
}

void test_filter_translation_invariance_negative_offset(void) {
  assertTranslationInvariance(-0.30f);
}

// Offsets REPRÉSENTATIFS d'une correction large (feature-168 : il n'y a plus de
// "bornes" dont ±0,50 serait l'extrémité) — on reste dans [0;14] après
// translation pour ne heurter aucun bornage PHYSIQUE du filtre (les échantillons
// sont ~7,00 ± 0,15).
void test_filter_translation_invariance_large_offset(void) {
  assertTranslationInvariance(1.00f);
  assertTranslationInvariance(-1.00f);
}

// Corollaire opérationnel : un changement d'offset est pris en compte
// IMMÉDIATEMENT en sortie, alors qu'appliqué à l'ENTRÉE il serait dilué sur
// toute la fenêtre de lissage (EMA α=0.10). C'est LE gain de la décision
// d'architecture, et il est mesurable.
void test_offset_change_is_immediate_at_output(void) {
  SensorFilter out(phCfg());   // offset appliqué en SORTIE (production)
  SensorFilter in(phCfg());    // offset appliqué en ENTRÉE (variante écartée)
  uint32_t t = 0;
  // Bruit minimal (> epsilon du détecteur figé) : 30 échantillons STRICTEMENT
  // identiques déclencheraient kSensorFrozenSamples (30) et rendraient value()
  // NaN — un capteur VIVANT ne reste jamais rigoureusement constant.
  for (int i = 0; i < 30; ++i) {
    const float jitter = (i % 2 == 0) ? 0.001f : -0.001f;
    out.addSample(7.00f + jitter, t);
    in.addSample(7.00f + jitter, t);
    t += 1000;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, out.value(t));

  // L'utilisateur saisit +0.30 : côté SORTIE, l'effet est complet au tick suivant.
  out.addSample(7.00f, t);
  in.addSample(7.00f + 0.30f, t);
  t += 1000;
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.005f, 7.30f, applyPhOffset(out.value(t), 0.30f),
                                   "offset en sortie => effet complet immediat");
  // Côté ENTRÉE, la valeur n'a quasiment pas bougé (dilution médiane + lissage).
  TEST_ASSERT_TRUE_MESSAGE(in.value(t) < 7.10f,
                           "offset en entree => effet dilue sur la fenetre de lissage");
}

// =============================================================================
// D2 — Non-régression du filtre pH : le filtre ne voit QUE le brut
// =============================================================================
// La chaîne de production nourrit le filtre avec le BRUT, quel que soit l'offset.
// On vérifie que TOUS les signaux de diagnostic (rejets, figé, périmé, porte)
// sont STRICTEMENT identiques offset ou pas, et que seule la valeur exposée à
// la régulation est décalée. feature-152 : il n'y a plus de latch ni de rejet
// de pic par seuil de déviation — la médiane absorbe déjà les aberrations par
// construction, sans état collant à comparer.

// Pipeline de production : ingestion brute, lecture corrigée en sortie.
struct PhPipeline {
  SensorFilter filter;
  float offset;
  explicit PhPipeline(float o) : filter(phCfg()), offset(o) {}
  bool ingest(float raw, uint32_t t) { return filter.addSample(raw, t); }
  float corrected(uint32_t now) const { return applyPhOffset(filter.value(now), offset); }  // getPhFiltered()
  float rawValue(uint32_t now) const { return filter.value(now); }                           // getPhValueRaw()
};

// Scénario dur commun : oscillation légitime, NaN, hors-plage physique, puis
// retour au calme. (feature-152 : l'oscillation 7.2/7.8 ne provoque plus aucun
// rejet — elle est dans la plage physique [0;14] — seuls NaN et le hors-plage
// stricte sont rejetés, identiquement quel que soit l'offset.)
static void runHarshScenario(PhPipeline& p, uint32_t& t) {
  for (int i = 0; i < 20; ++i) { p.ingest((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }  // oscillation légitime
  p.ingest(9.50f, t); t += 1000;   // valeur extrême mais dans la plage physique
  p.ingest(NAN, t);   t += 1000;   // mesure absente → rejetée
  p.ingest(20.0f, t); t += 1000;   // hors plage [0 ; 14] → rejetée
  p.ingest(-3.0f, t); t += 1000;   // hors plage → rejetée
  for (int i = 0; i < 20; ++i) { p.ingest(7.50f, t); t += 1000; }  // retour au calme
}

void test_filter_flags_identical_with_and_without_offset(void) {
  PhPipeline noOffset(0.0f);
  PhPipeline withOffset(0.30f);
  PhPipeline largeOffset(1.00f);  // feature-168 : plus de "max", valeur large représentative
  uint32_t t0 = 0, t1 = 0, t2 = 0;
  runHarshScenario(noOffset, t0);
  runHarshScenario(withOffset, t1);
  runHarshScenario(largeOffset, t2);
  TEST_ASSERT_EQUAL_UINT32(t0, t1);
  TEST_ASSERT_EQUAL_UINT32(t0, t2);

  // Compteurs de rejets : STRICTEMENT égaux (comparaison entière, pas de tolérance) —
  // exactement 3 (NaN, 20.0, -3.0), identiquement quel que soit l'offset.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, noOffset.filter.rejectedCount(),
                                  "3 rejets attendus : NaN, 20.0, -3.0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(noOffset.filter.rejectedCount(),
                                  withOffset.filter.rejectedCount(),
                                  "compteur de rejets ne doit pas dependre de l'offset");
  TEST_ASSERT_EQUAL_UINT8(noOffset.filter.rejectedCount(), largeOffset.filter.rejectedCount());

  // Figé, périmé, porte : mêmes verdicts de sécurité, offset ou pas.
  TEST_ASSERT_EQUAL(noOffset.filter.frozen(), withOffset.filter.frozen());
  TEST_ASSERT_EQUAL(noOffset.filter.stale(t0), withOffset.filter.stale(t1));
  TEST_ASSERT_EQUAL(noOffset.filter.gateClosed(), withOffset.filter.gateClosed());
  TEST_ASSERT_EQUAL(noOffset.filter.warmingUp(), withOffset.filter.warmingUp());

  // Diagnostic : la valeur officielle brute est identique, seule la valeur
  // corrigée bouge.
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, noOffset.rawValue(t0), withOffset.rawValue(t1));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, noOffset.rawValue(t0) + 0.30f, withOffset.corrected(t1));
}

// Filtre non amorcé (warmup) + offset : la valeur exposée à la régulation reste
// NaN — l'offset ne doit pas amorcer une mesure inexistante (garde fail-closed).
void test_warmup_with_offset_still_exposes_nan(void) {
  PhPipeline p(1.00f);
  uint32_t t = 0;
  for (int i = 0; i < (int)kSensorFilterMedianWindow - 1; ++i) {
    p.ingest(7.00f, t);
    TEST_ASSERT_TRUE_MESSAGE(isnan(p.corrected(t)),
                             "warmup + offset => la regulation ne doit voir aucune valeur");
    t += 1000;
  }
  p.ingest(7.00f, t);  // dernier échantillon → fenêtre pleine
  TEST_ASSERT_FALSE(isnan(p.corrected(t)));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.00f, p.corrected(t));
}

// =============================================================================
// D3 — Articulation offset ↔ seuils de sécurité (feature-168)
// =============================================================================
// Les seuils CHIMIQUES portent sur le pH CORRIGÉ (une fois l'offset appliqué,
// la valeur EST le pH de l'eau) ; les deux FILETS de dernier recours portent
// sur le pH BRUT, FIXES (5,50 / 8,50) et INDÉPENDANTS de l'offset — ils ne
// dérivent plus d'une borne d'offset (ADR-0098). Ce bloc teste ce que l'offset
// change côté MESURE ; l'INDÉPENDANCE du filet brut lui-même est verrouillée
// dans test_native_ph_guard (c'est là que vit l'invariant balayé sur tout le
// domaine, cf. revision C11 du gate).

// Le seuil chimique porte sur le CORRIGÉ : le pH RÉEL protégé dans le bassin est
// donc CONSTANT (6,70) par construction, et c'est la LECTURE BRUTE du seuil qui
// se déplace de −offset.
void test_corrected_floor_protects_a_constant_real_ph(void) {
  for (int i = -50; i <= 50; ++i) {
    const float offset = (float)i * 0.01f;
    // Lecture brute à laquelle le seuil corrigé se déclenche.
    const float brutAuSeuil = kPhFloorCorrected - offset;
    char msg[144];
    snprintf(msg, sizeof(msg), "offset=%+.2f brut au seuil=%.4f",
             (double)offset, (double)brutAuSeuil);
    // PRÉ-CONDITION FLOTTANTE : l'aller-retour brut → corrigé doit être stable à
    // beaucoup mieux que le centième, sinon les deux verdicts ci-dessous ne
    // testeraient pas la frontière annoncée. (Erreur théorique ≈ 2 ulp ≈ 1e-6.)
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, kPhFloorCorrected,
                                     applyPhOffset(brutAuSeuil, offset), msg);
    // Un centième plus acide : blocage, quel que soit l'offset.
    TEST_ASSERT_TRUE_MESSAGE(
        phGuardBlocksBelow(applyPhOffset(brutAuSeuil - 0.01f, offset), kPhFloorCorrected, false), msg);
    // Un centième moins acide : pas de blocage, quel que soit l'offset.
    TEST_ASSERT_FALSE_MESSAGE(
        phGuardBlocksBelow(applyPhOffset(brutAuSeuil + 0.01f, offset), kPhFloorCorrected, false), msg);
  }
}

// *** C1 DU GATE (feature-168) *** — les filets BRUTS sont désormais les
// constantes à justification chimique PROPRE, ÉGALES aux deux absolus, sans
// AUCUNE trace d'offset dans leur définition.
void test_raw_hard_floor_equals_the_absolute_chemical_threshold(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 5.50f, kPhFloorRawHard);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhRawFloorAbsolute, kPhFloorRawHard);
  TEST_ASSERT_TRUE_MESSAGE(kPhFloorRawHard < kPhFloorCorrected,
                           "le filet brut doit rester SOUS le seuil corrige nominal");
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.50f, kPhCeilingRawHard);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kPhRawCeilingAbsolute, kPhCeilingRawHard);
  TEST_ASSERT_TRUE_MESSAGE(kPhCeilingCorrected < kPhCeilingRawHard,
                           "le filet brut haut doit rester AU-DESSUS du plafond corrige nominal");
  TEST_ASSERT_TRUE_MESSAGE(kPhCeilingCorrected > kPhFloorCorrected,
                           "fenetre de dosage corrigee vide");
}

// Un offset positif GRAND (au-delà de l'ancienne borne ±0,50, désormais
// ordinaire) rend le seuil corrigé trop permissif en LECTURE BRUTE, mais le
// filet brut — FIXE, INDÉPENDANT de l'offset — continue de refuser à 5,50 pile,
// quelle que soit la valeur corrigée affichée.
void test_wrong_positive_offset_is_bounded_by_the_fixed_raw_floor(void) {
  const float offsets[] = {0.10f, 0.30f, 0.50f, 0.80f, 1.00f, 5.0f};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    const float offset = offsets[i];
    char msg[144];
    snprintf(msg, sizeof(msg), "offset=%+.2f", (double)offset);
    // Juste sous le filet : refus, quelle que soit la valeur AFFICHÉE (rassurante).
    TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(5.49f, kPhFloorRawHard, false), msg);
    // Et la valeur affichée est bien, elle, trompeuse (plus l'offset est grand,
    // plus elle est éloignée du refus) : c'est tout l'objet du filet brut FIXE.
    TEST_ASSERT_TRUE_MESSAGE(applyPhOffset(5.49f, offset) > 5.49f, msg);
  }
  // Cas de référence : offset +1,00, eau réelle à 5,49 (sous le filet).
  // L'affichage annonce 6,49, et c'est le BRUT qui tranche — invariant du filet,
  // pas de la consigne.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 6.49f, applyPhOffset(5.49f, 1.00f));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(5.49f, kPhFloorRawHard, false));
}

// Miroir : un offset NÉGATIF saisi à tort masquerait une eau réellement basique
// (chlore inactif, régulation ORP qui surdose). Le filet brut HAUT la rattrape
// à 8,50 pile, quelle que soit la valeur corrigée affichée.
void test_wrong_negative_offset_is_bounded_by_the_fixed_raw_ceiling(void) {
  const float brut = 8.55f;
  const float affiche = applyPhOffset(brut, -0.90f);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 7.65f, affiche);
  TEST_ASSERT_TRUE(phGuardBlocksAbove(brut, kPhCeilingRawHard, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(8.45f, kPhCeilingRawHard, false));
  // La valeur AFFICHÉE elle-même (7,55) refuse déjà le pH+ au titre du seuil
  // corrigé (7,60) — DEUX gardes voient donc cette eau, chacune sur son
  // référentiel.
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(affiche, kPhCeilingCorrected, false),
                           "7,55 corrige doit refuser d'ALCALINISER");
  TEST_ASSERT_TRUE(phGuardBlocksAbove(affiche, kPhOrpInhibit, false));
}

// La fenêtre de dosage NOMINALE (offset dans la zone non-impasse) est NON
// VIDE : entre le refus d'acidifier (6,70) et l'inhibition ORP (7,60), toute la
// plage de consigne [7,20 ; 7,50] est libre. ⚠️ Ce n'est PLUS vrai pour TOUT
// offset depuis feature-168 (au-delà des seuils d'impasse, le filet brut mord
// AVANT le seuil corrigé — c'est le comportement VOULU, signalé par une alerte
// dédiée) : le balayage est donc borné à la zone sans impasse,
// ]kPhOffsetImpasseThresholdHigh ; kPhOffsetImpasseThreshold[.
void test_dosing_window_is_never_empty_in_the_non_impasse_zone(void) {
  // Pré-condition : la zone non-impasse doit être non vide et contenir au
  // moins [-0,50 ; 0,50] (l'ancienne plage de réglage complète).
  TEST_ASSERT_TRUE(kPhOffsetImpasseThresholdHigh < -0.50f);
  TEST_ASSERT_TRUE(kPhOffsetImpasseThreshold > 0.50f);
  for (int i = -50; i <= 50; ++i) {
    const float offset = (float)i * 0.01f;
    for (int c = 720; c <= 750; c += 10) {
      const float consigne = (float)c * 0.01f;   // pH RÉEL visé
      const float brut = consigne - offset;      // lecture capteur correspondante
      const float corrige = applyPhOffset(brut, offset);
      char msg[160];
      snprintf(msg, sizeof(msg), "offset=%+.2f consigne=%.2f brut=%.4f",
               (double)offset, (double)consigne, (double)brut);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksBelow(corrige, kPhFloorCorrected, false), msg);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksAbove(corrige, kPhOrpInhibit, false), msg);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksAbove(corrige, kPhCeilingCorrected, false), msg);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksBelow(brut, kPhFloorRawHard, false), msg);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksAbove(brut, kPhCeilingRawHard, false), msg);
    }
  }
}

// *** CRITÈRE D'ACCEPTATION DE LA SPEC *** — à offset +1,00 / consigne 7,20, le
// filet brut ne mord JAMAIS en bande nominale : 7,20 − 1,00 = 6,20 > 5,50.
void test_offset_plus_one_consigne_720_never_hits_the_raw_floor(void) {
  const float offset = 1.00f;
  const float brut = kPhTargetMin - offset;  // 6,20
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 6.20f, brut);
  TEST_ASSERT_TRUE_MESSAGE(brut > kPhFloorRawHard,
                           "6,20 doit rester AU-DESSUS du filet brut 5,50");
  TEST_ASSERT_FALSE(phGuardBlocksBelow(brut, kPhFloorRawHard, false));
  TEST_ASSERT_FALSE(phGuardBlocksBelow(kPhTargetMin, kPhFloorCorrected, false));
}

// =============================================================================
// D4 — HORODATAGE DE JUSTESSE (feature-156, étendu feature-168 C10/C12)
// =============================================================================
// Le verdict « correction caduque » engage le dosage : une correction NON NULLE
// posée AVANT le dernier étalonnage réussi (cause a) OU INHABITUELLE non
// reconfirmée depuis plus de confirmIntervalDays jours (cause b, feature-168)
// est caduque. `seq` (entier croissant), PAS `epoch`, fait foi pour l'ORDRE
// étalonnage/correction (cause a) — epoch ne sert qu'à dater la cause (b).

// Paramètres NOMINAUX de la cause (b), neutres pour ne tester QUE la cause (a) :
// horloge jamais synchronisée (epoch=0 des deux côtés) ⇒ cause (b) fail-open.
static const uint32_t kNoEpoch = 0;
static const uint32_t kConfirmDays = 30;
static const float kUnusual = 0.50f;

void test_stale_offset_null_and_anterior_is_false(void) {
  // Offset nul (inerte) : rien à annuler, jamais caduc, même antérieur.
  TEST_ASSERT_FALSE(isPhOffsetStale(0.0f, 1, 5, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
}

void test_stale_offset_nonzero_and_anterior_is_true(void) {
  // Correction posée (seq=1) avant un étalonnage plus récent (seq=5) : caduque.
  TEST_ASSERT_TRUE(isPhOffsetStale(0.30f, 1, 5, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
  TEST_ASSERT_TRUE(isPhOffsetStale(-0.10f, 2, 3, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
}

void test_stale_offset_nonzero_and_posterior_is_false(void) {
  // Correction posée APRÈS le dernier étalonnage : toujours à jour (cause a).
  TEST_ASSERT_FALSE(isPhOffsetStale(0.30f, 9, 5, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
}

void test_stale_equal_sequences_is_false(void) {
  // Séquences ÉGALES : aucune préséance de l'étalonnage sur la correction —
  // pas caduque. Cas limite explicitement exigé (ni l'un ni l'autre n'est
  // strictement postérieur).
  TEST_ASSERT_FALSE(isPhOffsetStale(0.30f, 4, 4, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
}

void test_stale_both_zero_is_false(void) {
  // Aucun des deux événements ne s'est jamais produit (appareil neuf, ou
  // config antérieure à feature-156 sans ces clés NVS) : jamais caduc, même
  // avec un offset non nul hérité d'une ancienne version.
  TEST_ASSERT_FALSE(isPhOffsetStale(0.20f, 0, 0, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
}

void test_stale_first_calibration_after_update_with_inherited_offset(void) {
  // RÉTROCOMPATIBILITÉ (feature-156) : une installation EXISTANTE avant cette
  // version a ses 4 champs à 0 (offsetSeq=0, calSeq=0), y compris avec une
  // correction non nulle en vigueur depuis des mois — test_stale_both_zero_is_false
  // couvre déjà ce premier redémarrage : pas de fausse alerte.
  // Ici : l'étape SUIVANTE, le premier étalonnage posé APRÈS la mise à jour.
  TEST_ASSERT_TRUE(isPhOffsetStale(0.30f, 0, 1, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
  // Et si la correction héritée était nulle, elle reste jamais caduque (rien
  // à annuler), même avec le même franchissement de séquence.
  TEST_ASSERT_FALSE(isPhOffsetStale(0.0f, 0, 1, kNoEpoch, kNoEpoch, kConfirmDays, kUnusual));
}

void test_next_justness_seq_is_max_plus_one(void) {
  TEST_ASSERT_EQUAL_UINT32(6, nextJustnessSeq(5, 3));
  TEST_ASSERT_EQUAL_UINT32(6, nextJustnessSeq(3, 5));  // monotone quel que soit l'ordre
  TEST_ASSERT_EQUAL_UINT32(1, nextJustnessSeq(0, 0));  // premier événement de l'appareil
  TEST_ASSERT_EQUAL_UINT32(101, nextJustnessSeq(100, 0));
  TEST_ASSERT_EQUAL_UINT32(101, nextJustnessSeq(0, 100));
}

// =============================================================================
// D4bis — feature-168 (C10) : DEUXIÈME CAUSE DE CADUCITÉ, isPhOffsetExpired
// =============================================================================
// Indépendante de l'étalonnage : une correction INHABITUELLE (|offset| > 0,50)
// non reconfirmée depuis plus de 30 jours est caduque, QUEL QUE SOIT l'état de
// l'étalonnage (seq ne joue aucun rôle ici, epoch seul).

// Fail-open : epoch == 0 (date inconnue) ⇒ jamais caduc par cette cause, quelle
// que soit l'ampleur de l'offset.
void test_expired_epoch_zero_never_caduc(void) {
  TEST_ASSERT_FALSE(isPhOffsetExpired(1.00f, 0, 1000000000UL, kConfirmDays, kUnusual));
}

// Fail-open : horloge pas encore synchronisée côté "maintenant" (nowEpoch==0).
void test_expired_now_epoch_zero_never_caduc(void) {
  TEST_ASSERT_FALSE(isPhOffsetExpired(1.00f, 1000000000UL, 0, kConfirmDays, kUnusual));
}

// Un offset DANS la zone habituelle (|offset| <= 0,50) n'est jamais caduc par
// cette cause, même très ancien.
void test_expired_offset_usual_never_caduc(void) {
  const uint32_t posed = 1000000000UL;
  const uint32_t farLater = posed + 365UL * 86400UL;  // un an plus tard
  TEST_ASSERT_FALSE(isPhOffsetExpired(0.50f, posed, farLater, kConfirmDays, kUnusual));
  TEST_ASSERT_FALSE(isPhOffsetExpired(-0.50f, posed, farLater, kConfirmDays, kUnusual));
}

// *** LES QUATRE COMBINAISONS EXIGÉES PAR LA SPEC *** — 29 j / 31 j croisés
// avec |offset| 0,40 (habituel, jamais caduc) / 1,00 (inhabituel, caduc
// seulement au-delà du délai).
void test_expired_four_combinations_29_31_days_x_040_100(void) {
  const uint32_t posed = 1000000000UL;
  const uint32_t after29 = posed + 29UL * 86400UL;
  const uint32_t after31 = posed + 31UL * 86400UL;

  // 29 jours, offset habituel (0,40) : jamais caduc (ni le délai, ni l'ampleur).
  TEST_ASSERT_FALSE_MESSAGE(
      isPhOffsetExpired(0.40f, posed, after29, kConfirmDays, kUnusual),
      "29j / 0,40 : ni le delai ni l'ampleur ne justifient la caducite");
  // 29 jours, offset inhabituel (1,00) : délai PAS encore dépassé, pas caduc.
  TEST_ASSERT_FALSE_MESSAGE(
      isPhOffsetExpired(1.00f, posed, after29, kConfirmDays, kUnusual),
      "29j / 1,00 : le delai de 30j n'est pas encore depasse");
  // 31 jours, offset habituel (0,40) : jamais caduc par cette cause (l'ampleur
  // manque), quel que soit le délai.
  TEST_ASSERT_FALSE_MESSAGE(
      isPhOffsetExpired(0.40f, posed, after31, kConfirmDays, kUnusual),
      "31j / 0,40 : l'ampleur ne declenche jamais cette cause");
  // 31 jours, offset inhabituel (1,00) : délai dépassé ET ampleur inhabituelle
  // ⇒ caduc.
  TEST_ASSERT_TRUE_MESSAGE(
      isPhOffsetExpired(1.00f, posed, after31, kConfirmDays, kUnusual),
      "31j / 1,00 : delai depasse ET offset inhabituel => caduc");
}

// Horloge repartie en arrière (RTC non recalée) : fail-open, pas de caducité
// signalée sur une donnée temporelle qui n'a pas de sens.
void test_expired_clock_went_backwards_is_false(void) {
  TEST_ASSERT_FALSE(isPhOffsetExpired(1.00f, 2000000000UL, 1000000000UL, kConfirmDays, kUnusual));
}

void test_refusal_counter_constants(void) {
  TEST_ASSERT_EQUAL_UINT8(3, kPhGuardRefusalMax);
  TEST_ASSERT_EQUAL_UINT32(86400000UL, kPhGuardRefusalWindowMs);  // 24 h
  // La fenêtre doit rester représentable sur 32 bits en ms (arithmétique millis
  // wrap-safe) : 24 h << 49,7 jours.
  TEST_ASSERT_TRUE(kPhGuardRefusalWindowMs < 4294967295UL);
}

// =============================================================================
// NOTE DE COUVERTURE (D3) — ce qui n'est PAS testable ici
// =============================================================================
// PumpControllerClass::isPhGuardBlocking() et ::isOrpInhibitedByPh()
// (src/pump_controller.cpp) ne sont PAS des fonctions pures : elles lisent des
// membres d'état (_phLowBlockActive, _phHighBlockActive, _phRawHardBlockActive,
// _orpPhInhibitActive, _phGuardRefusals, _phGuardWindowStart, _lastRefusalCause),
// des singletons (sensors, mqttCfg, mqttManager, systemLogger), millis(), et
// écrivent la config (saveMqttConfig()). Les extraire en pur supposerait un
// refactor de PumpControllerClass hors périmètre (et un passage pool-chemistry
// supplémentaire).
// Restent donc à couvrir par pool-chemistry + tests on-target :
//   - transition d'état de chaque garde (log critical + alerte MQTT une seule fois),
//   - retour dans le domaine sûr (levée + log info) avec la bonne cause,
//   - fenêtre glissante 24 h et bascule en mode manuel au 3ᵉ refus AUTOMATIQUE,
//   - mode Programmée : refus signalé (warn + alerte à débit limité), JAMAIS compté,
//   - réarmement des drapeaux au changement de produit, compteur conservé,
//   - câblage des branches d'update() AVANT canDose(),
//   - EXCLUSION du compteur des refus de filet brut SEUL en zone d'impasse
//     (C5/C5bis) — isPhRegulationImpasseLow/High sont couvertes en pur dans
//     test_native_ph_guard, mais leur BRANCHEMENT au compteur vit dans
//     pump_controller.cpp (impur).
// =============================================================================

int main(int, char**) {
  UNITY_BEGIN();
  // D1.1 — application
  RUN_TEST(test_zero_offset_is_identity);
  RUN_TEST(test_positive_offset_applied_to_hundredth);
  RUN_TEST(test_negative_offset_applied_to_hundredth);
  RUN_TEST(test_offset_hundredth_step_is_visible);
  RUN_TEST(test_offset_plus_one_is_applied_exactly);
  RUN_TEST(test_offset_is_reversible);
  RUN_TEST(test_result_is_not_clamped_to_ph_scale);
  // D1.2 — NaN
  RUN_TEST(test_nan_raw_stays_nan_whatever_the_offset);
  RUN_TEST(test_nan_offset_is_neutralized_and_preserves_raw);
  RUN_TEST(test_nan_raw_and_nan_offset_stays_nan);
  RUN_TEST(test_nan_raw_with_infinite_offset_stays_nan);
  // D1.3 — validité (salubrite de domaine, feature-168)
  RUN_TEST(test_valid_offset_inside_range);
  RUN_TEST(test_former_bound_of_050_is_now_an_ordinary_value);
  RUN_TEST(test_exact_scale_span_bounds_are_accepted);
  RUN_TEST(test_just_outside_scale_span_is_refused);
  RUN_TEST(test_nan_offset_is_invalid);
  RUN_TEST(test_absurd_offsets_are_invalid);
  RUN_TEST(test_spec_accepted_values);
  RUN_TEST(test_offset_named_constants);
  // D1.4 — invariance par translation
  RUN_TEST(test_filter_translation_invariance_positive_offset);
  RUN_TEST(test_filter_translation_invariance_negative_offset);
  RUN_TEST(test_filter_translation_invariance_large_offset);
  RUN_TEST(test_offset_change_is_immediate_at_output);
  // D2 — non-régression filtre (feature-152 : plus de latch/rejet de pic à tester)
  RUN_TEST(test_filter_flags_identical_with_and_without_offset);
  RUN_TEST(test_warmup_with_offset_still_exposes_nan);
  // D3 — articulation offset / seuils de sécurité (feature-168)
  RUN_TEST(test_corrected_floor_protects_a_constant_real_ph);
  RUN_TEST(test_raw_hard_floor_equals_the_absolute_chemical_threshold);
  RUN_TEST(test_wrong_positive_offset_is_bounded_by_the_fixed_raw_floor);
  RUN_TEST(test_wrong_negative_offset_is_bounded_by_the_fixed_raw_ceiling);
  RUN_TEST(test_dosing_window_is_never_empty_in_the_non_impasse_zone);
  RUN_TEST(test_offset_plus_one_consigne_720_never_hits_the_raw_floor);
  RUN_TEST(test_refusal_counter_constants);
  // D4 — horodatage de justesse (feature-156, cause a)
  RUN_TEST(test_stale_offset_null_and_anterior_is_false);
  RUN_TEST(test_stale_offset_nonzero_and_anterior_is_true);
  RUN_TEST(test_stale_offset_nonzero_and_posterior_is_false);
  RUN_TEST(test_stale_equal_sequences_is_false);
  RUN_TEST(test_stale_both_zero_is_false);
  RUN_TEST(test_stale_first_calibration_after_update_with_inherited_offset);
  RUN_TEST(test_next_justness_seq_is_max_plus_one);
  // D4bis — péremption (feature-168, C10, cause b)
  RUN_TEST(test_expired_epoch_zero_never_caduc);
  RUN_TEST(test_expired_now_epoch_zero_never_caduc);
  RUN_TEST(test_expired_offset_usual_never_caduc);
  RUN_TEST(test_expired_four_combinations_29_31_days_x_040_100);
  RUN_TEST(test_expired_clock_went_backwards_is_false);
  return UNITY_END();
}
