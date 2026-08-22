// =============================================================================
// Tests unitaires natifs — Offset logiciel de mesure pH (ph_offset, feature-067)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
// On teste le COMPORTEMENT observable de la logique PURE de src/ph_offset.h
// (ADR-0017, comme dosing_logic / measure_validity_logic) :
//   - applyPhOffset(raw, offset)  → mesure corrigée, NaN in → NaN out
//   - isPhOffsetValid(offset)     → offset exploitable (numérique ET dans bornes)
//
// Le fichier couvre trois familles :
//
//   D1 — Application, bornes, propagation NaN, et surtout INVARIANCE PAR
//        TRANSLATION du filtre réel (src/sensor_filter) : filtre(x + c) =
//        filtre(x) + c. C'est la justification mathématique de la décision
//        d'architecture « offset appliqué EN SORTIE des accesseurs » — elle est
//        démontrée ici sur le filtre de production, pas supposée.
//
//   D2 — Non-régression du filtre pH : latch EMI, rejet de pics et compteurs de
//        rejets STRICTEMENT identiques avec et sans offset. Le filtre ne voit
//        que le brut ; l'offset ne peut donc pas déplacer un seuil de sécurité.
//        (La suite test_native_sensor_filter reste inchangée : le module pur
//        n'a aucune notion d'offset, ADR-0017.)
//
//   D3 — Invariants de CONSTANTES des filets de sécurité pH brut (planchers
//        d'injection d'acide, inhibition ORP). La machine à états elle-même
//        (PumpControllerClass::isPhRawFloorBlocking / isOrpInhibitedByPhRaw)
//        n'est PAS pure — voir la note en fin de fichier.
//
// Chaîne de production reproduite (src/sensors.cpp) :
//   EZO → _lastPh (brut) → _phFilter.addSample(brut) → filtered() (brut lissé)
//       → getPhFiltered() = applyPhOffset(filtered(), offset)   [régulation]
//       → getPhFilteredRaw() = filtered()                       [filets pH brut]
// =============================================================================

#include <unity.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)

#include "ph_offset.h"
#include "sensor_filter.h"

void setUp(void) {}
void tearDown(void) {}

// Config filtre pH calquée sur constants.h (même ordre positionnel que la suite
// test_native_sensor_filter) : min 0, max 14, spike 1.0, α 0.10, fenêtre 13,
// maxRejets 10, âge max 20 s, cap dispersion 0.5.
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

// Aux bornes autorisées, la correction vaut exactement ±0,50 pH.
void test_offset_at_bounds_shifts_by_half_ph(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 7.50f, applyPhOffset(7.00f, kPhOffsetMax));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 6.50f, applyPhOffset(7.00f, kPhOffsetMin));
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
// protègent le dosage, et le système doserait sur une mesure fabriquée.

void test_nan_raw_stays_nan_whatever_the_offset(void) {
  const float offsets[] = {0.0f, 0.30f, -0.30f, kPhOffsetMin, kPhOffsetMax};
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
// D1.3 — isPhOffsetValid : bornes exactes, NaN, valeurs absurdes
// =============================================================================

void test_valid_offset_inside_range(void) {
  TEST_ASSERT_TRUE(isPhOffsetValid(0.0f));
  TEST_ASSERT_TRUE(isPhOffsetValid(0.30f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-0.30f));
  TEST_ASSERT_TRUE(isPhOffsetValid(0.01f));
}

// Frontières INCLUSES : ±0,50 exactement est accepté (0,5 est exactement
// représentable en binaire, la comparaison est donc stricte et déterministe).
void test_exact_bounds_are_accepted(void) {
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(kPhOffsetMax), "+0.50 doit etre accepte");
  TEST_ASSERT_TRUE_MESSAGE(isPhOffsetValid(kPhOffsetMin), "-0.50 doit etre accepte");
  TEST_ASSERT_TRUE(isPhOffsetValid(0.50f));
  TEST_ASSERT_TRUE(isPhOffsetValid(-0.50f));
}

// Un centième au-delà → refusé des deux côtés.
void test_just_outside_bounds_is_refused(void) {
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(0.501f), "+0.501 doit etre refuse");
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(-0.501f), "-0.501 doit etre refuse");
  TEST_ASSERT_FALSE(isPhOffsetValid(0.51f));
  TEST_ASSERT_FALSE(isPhOffsetValid(-0.51f));
}

void test_nan_offset_is_invalid(void) {
  TEST_ASSERT_FALSE_MESSAGE(isPhOffsetValid(NAN), "NaN ne doit jamais etre exploitable");
}

// Valeurs absurdes : NVS corrompue, POST malveillant, cast raté.
void test_absurd_offsets_are_invalid(void) {
  TEST_ASSERT_FALSE(isPhOffsetValid(1.0f));
  TEST_ASSERT_FALSE(isPhOffsetValid(-1.0f));
  TEST_ASSERT_FALSE(isPhOffsetValid(14.0f));
  TEST_ASSERT_FALSE(isPhOffsetValid(1e9f));
  TEST_ASSERT_FALSE(isPhOffsetValid(-1e9f));
  TEST_ASSERT_FALSE(isPhOffsetValid(INFINITY));
  TEST_ASSERT_FALSE(isPhOffsetValid(-INFINITY));
}

// Verrou de régression sur les bornes elles-mêmes : elles ont été arbitrées avec
// pool-chemistry (au-delà de ±0,50, c'est la sonde/calibration EZO en cause).
// Tout élargissement doit casser ce test et repasser par pool-chemistry.
void test_offset_bounds_constants(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.50f, kPhOffsetMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, kPhOffsetMax);
  TEST_ASSERT_TRUE_MESSAGE(kPhOffsetMin < kPhOffsetMax, "plage d'offset non vide");
  // Plage symétrique : aucun biais implicite vers l'acidification.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, kPhOffsetMin + kPhOffsetMax);
}

// =============================================================================
// D1.4 — INVARIANCE PAR TRANSLATION du filtre réel : filtre(x + c) = filtre(x) + c
// =============================================================================
// Justification de la décision d'architecture (offset EN SORTIE) : appliquer
// l'offset après le filtre donne le MÊME résultat que l'appliquer avant, donc on
// choisit la sortie (prise en compte immédiate + filtre sur signal brut).
// Démontré ici sur le module de production, pas sur un modèle.

// Séquence pH réaliste : oscillation modérée sous le cap de dispersion (0.5),
// 30 échantillons, loin des bornes de plausibilité [0 ; 14] pour que la
// translation ne heurte aucun bornage.
static float seqAt(int i) {
  const float pattern[10] = {7.00f, 7.08f, 6.95f, 7.12f, 7.03f,
                             6.98f, 7.15f, 7.05f, 6.92f, 7.10f};
  return pattern[i % 10];
}

// Cœur de la démonstration : deux filtres, l'un nourri en brut, l'autre nourri
// en brut + c. À chaque pas, filtreB.filtered() == filtreA.filtered() + c, ce
// qui est exactement applyPhOffset(filtreA.filtered(), c).
static void assertTranslationInvariance(float c) {
  SensorFilter fa(phCfg());
  SensorFilter fb(phCfg());
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) {
    const float raw = seqAt(i);
    const bool acceptedA = fa.addSample(raw, t);
    const bool acceptedB = fb.addSample(raw + c, t);
    // Même décision d'acceptation : les seuils du filtre sont RELATIFS (|raw −
    // médiane|), donc insensibles à une translation.
    TEST_ASSERT_EQUAL_MESSAGE(acceptedA, acceptedB,
                              "acceptation identique sous translation");
    if (!isnan(fa.filtered())) {
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
          1e-4f, fb.filtered(), applyPhOffset(fa.filtered(), c),
          "filtre(x+c) doit valoir filtre(x)+c");
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
          1e-4f, fb.median(), applyPhOffset(fa.median(), c),
          "mediane(x+c) doit valoir mediane(x)+c");
    } else {
      // Warmup : les deux filtres sont NaN au même instant (même amorçage).
      TEST_ASSERT_TRUE(isnan(fb.filtered()));
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

void test_filter_translation_invariance_at_bounds(void) {
  assertTranslationInvariance(kPhOffsetMax);
  assertTranslationInvariance(kPhOffsetMin);
}

// Corollaire opérationnel : un changement d'offset est pris en compte
// IMMÉDIATEMENT en sortie, alors qu'appliqué à l'ENTRÉE il serait dilué sur
// toute la fenêtre de lissage (EMA α=0.10). C'est LE gain de la décision
// d'architecture, et il est mesurable.
void test_offset_change_is_immediate_at_output(void) {
  SensorFilter out(phCfg());   // offset appliqué en SORTIE (production)
  SensorFilter in(phCfg());    // offset appliqué en ENTRÉE (variante écartée)
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) { out.addSample(7.00f, t); in.addSample(7.00f, t); t += 1000; }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.00f, out.filtered());

  // L'utilisateur saisit +0.30 : côté SORTIE, l'effet est complet au tick suivant.
  out.addSample(7.00f, t);
  in.addSample(7.00f + 0.30f, t);
  t += 1000;
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.005f, 7.30f, applyPhOffset(out.filtered(), 0.30f),
                                   "offset en sortie => effet complet immediat");
  // Côté ENTRÉE, la valeur n'a quasiment pas bougé (dilution médiane + EMA).
  TEST_ASSERT_TRUE_MESSAGE(in.filtered() < 7.10f,
                           "offset en entree => effet dilue sur la fenetre de lissage");
}

// =============================================================================
// D2 — Non-régression du filtre pH : le filtre ne voit QUE le brut
// =============================================================================
// La chaîne de production nourrit le filtre avec le BRUT, quel que soit l'offset.
// On vérifie que TOUS les signaux de sécurité (latch EMI, rejets de pics,
// compteurs, ready, dispersion) sont STRICTEMENT identiques offset ou pas, et
// que seule la valeur exposée à la régulation est décalée.

// Pipeline de production : ingestion brute, lecture corrigée en sortie.
struct PhPipeline {
  SensorFilter filter;
  float offset;
  explicit PhPipeline(float o) : filter(phCfg()), offset(o) {}
  bool ingest(float raw, uint32_t t) { return filter.addSample(raw, t); }
  float corrected() const { return applyPhOffset(filter.filtered(), offset); }   // getPhFiltered()
  float rawFiltered() const { return filter.filtered(); }                        // getPhFilteredRaw()
};

// Scénario dur commun : oscillation EMI (pose du latch), spikes durs isolés,
// NaN, hors-plage, puis retour au calme.
// (oscillation 7.2/7.8 = amplitude 0.6 > cap 0.5 ; il faut la fenêtre pleine (13)
// PUIS 3 fenêtres consécutives en dépassement pour poser le latch → 20 pas.)
static void runHarshScenario(PhPipeline& p, uint32_t& t) {
  for (int i = 0; i < 20; ++i) { p.ingest((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }  // latch EMI
  p.ingest(9.50f, t); t += 1000;   // spike dur isolé
  p.ingest(NAN, t);   t += 1000;   // mesure absente
  p.ingest(20.0f, t); t += 1000;   // hors plage [0 ; 14]
  p.ingest(-3.0f, t); t += 1000;   // hors plage
  for (int i = 0; i < 20; ++i) { p.ingest(7.50f, t); t += 1000; }  // retour au calme
}

void test_filter_flags_identical_with_and_without_offset(void) {
  PhPipeline noOffset(0.0f);
  PhPipeline withOffset(0.30f);
  PhPipeline maxOffset(kPhOffsetMax);
  uint32_t t0 = 0, t1 = 0, t2 = 0;
  runHarshScenario(noOffset, t0);
  runHarshScenario(withOffset, t1);
  runHarshScenario(maxOffset, t2);

  // Latch EMI : posé/levé à l'identique.
  TEST_ASSERT_EQUAL_MESSAGE(noOffset.filter.unstableLatched(),
                            withOffset.filter.unstableLatched(),
                            "latch EMI ne doit pas dependre de l'offset");
  TEST_ASSERT_EQUAL(noOffset.filter.unstableLatched(), maxOffset.filter.unstableLatched());
  TEST_ASSERT_EQUAL(noOffset.filter.unstable(), withOffset.filter.unstable());
  TEST_ASSERT_EQUAL(noOffset.filter.unstable(), maxOffset.filter.unstable());

  // Compteurs de rejets : STRICTEMENT égaux (comparaison entière, pas de tolérance).
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(noOffset.filter.rejectedCount(),
                                  withOffset.filter.rejectedCount(),
                                  "compteur de rejets ne doit pas dependre de l'offset");
  TEST_ASSERT_EQUAL_UINT8(noOffset.filter.rejectedCount(), maxOffset.filter.rejectedCount());
  TEST_ASSERT_EQUAL_UINT8(noOffset.filter.consecutiveRejects(),
                          withOffset.filter.consecutiveRejects());
  TEST_ASSERT_EQUAL_UINT8(noOffset.filter.dispersionCleanStreak(),
                          withOffset.filter.dispersionCleanStreak());

  // ready() et figé : mêmes verdicts de sécurité.
  TEST_ASSERT_EQUAL(noOffset.filter.ready(t0 - 1000), withOffset.filter.ready(t1 - 1000));
  TEST_ASSERT_EQUAL(noOffset.filter.frozen(), withOffset.filter.frozen());

  // Diagnostic : le brut lissé est identique, seule la valeur corrigée bouge.
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, noOffset.rawFiltered(), withOffset.rawFiltered());
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, noOffset.rawFiltered() + 0.30f, withOffset.corrected());
}

// Le rejet de pic est ÉVALUÉ SUR LE BRUT : un spike dur reste écarté à
// l'identique, et il n'est pas « rattrapé » par l'offset. Vérifié pas à pas.
void test_spike_rejection_identical_with_offset(void) {
  PhPipeline a(0.0f), b(kPhOffsetMax);
  uint32_t t = 0;
  const float seq[] = {7.00f, 7.02f, 6.98f, 7.01f, 6.99f, 7.03f, 7.00f,
                       6.97f, 7.04f, 7.00f, 6.96f, 7.02f, 7.01f,
                       9.50f,   // spike dur (|9.50 − 7.00| = 2.5 > 1.0)
                       7.00f, 4.00f, 7.00f};
  for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
    const bool ra = a.ingest(seq[i], t);
    const bool rb = b.ingest(seq[i], t);
    TEST_ASSERT_EQUAL_MESSAGE(ra, rb, "acceptation/rejet identique avec offset");
    TEST_ASSERT_EQUAL_UINT8(a.filter.rejectedCount(), b.filter.rejectedCount());
    t += 1000;
  }
  TEST_ASSERT_TRUE_MESSAGE(a.filter.rejectedCount() >= 2, "les 2 spikes durs sont bien rejetes");
}

// Latch EMI : l'offset ne le pose ni ne le lève, même appliqué en cours de route
// (l'utilisateur peut changer l'offset à chaud pendant une instabilité).
void test_latch_unaffected_by_offset_change_mid_run(void) {
  PhPipeline p(0.0f);
  uint32_t t = 0;
  // Même borne de sécurité que la suite sensor_filter : on injecte jusqu'à la
  // pose du latch (fenêtre pleine + 3 fenêtres en dépassement), max 40 pas.
  for (int i = 0; i < 40 && !p.filter.unstableLatched(); ++i) {
    p.ingest((i % 2 == 0) ? 7.2f : 7.8f, t);
    t += 1000;
  }
  TEST_ASSERT_TRUE_MESSAGE(p.filter.unstableLatched(), "oscillation > cap => latch pose");

  const uint8_t rejectsBefore = p.filter.rejectedCount();
  p.offset = kPhOffsetMax;  // changement à chaud
  TEST_ASSERT_TRUE_MESSAGE(p.filter.unstableLatched(), "changement d'offset ne leve pas le latch");
  TEST_ASSERT_EQUAL_UINT8(rejectsBefore, p.filter.rejectedCount());

  // Et l'inhibition dosage (unstable) reste vraie tant que le bruit persiste.
  for (int i = 0; i < 6; ++i) { p.ingest((i % 2 == 0) ? 7.2f : 7.8f, t); t += 1000; }
  TEST_ASSERT_TRUE(p.filter.unstable());
}

// Filtre non amorcé (warmup) + offset : la valeur exposée à la régulation reste
// NaN — l'offset ne doit pas amorcer une mesure inexistante (garde fail-closed).
void test_warmup_with_offset_still_exposes_nan(void) {
  PhPipeline p(kPhOffsetMax);
  uint32_t t = 0;
  for (int i = 0; i < 12; ++i) {  // 12 < fenêtre centrale 13
    p.ingest(7.00f, t);
    TEST_ASSERT_TRUE_MESSAGE(isnan(p.corrected()),
                             "warmup + offset => la regulation ne doit voir aucune valeur");
    t += 1000;
  }
  p.ingest(7.00f, t);  // 13ᵉ → fenêtre pleine
  TEST_ASSERT_FALSE(isnan(p.corrected()));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.50f, p.corrected());
}

// =============================================================================
// D3 — Invariants de constantes des filets de sécurité pH BRUT (feature-067)
// =============================================================================
// Ces gardes (pump_controller) raisonnent sur le pH BRUT lissé, donc restent
// valides même si l'offset saisi est faux. Seuls les INVARIANTS de conception
// des seuils sont testables en pur ici (voir note de couverture en fin de
// fichier pour la machine à états).

void test_raw_floor_constants_are_coherent(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.80f, kPhRawDosingFloor);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.90f, kPhRawDosingFloorWide);
  // Palier ÉLARGI strictement plus prudent : plus l'offset est grand, plus
  // l'incertitude sur le pH réel est grande → marge de sécurité plus haute.
  TEST_ASSERT_TRUE_MESSAGE(kPhRawDosingFloorWide > kPhRawDosingFloor,
                           "le palier large doit etre plus conservateur");
  // Le seuil de bascule est DANS la plage d'offset autorisée : les deux paliers
  // sont donc réellement atteignables (sinon l'un serait du code mort).
  TEST_ASSERT_TRUE(kPhOffsetWideThreshold > 0.0f);
  TEST_ASSERT_TRUE_MESSAGE(kPhOffsetWideThreshold < kPhOffsetMax,
                           "sinon le palier large ne serait jamais selectionne");
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f, kPhOffsetWideThreshold);
}

// Choix du palier selon |offset| — règle de sélection (fabsf(offset) > seuil),
// frontières incluses côté palier normal.
void test_floor_tier_selection_by_absolute_offset(void) {
  struct { float offset; float expected; } cases[] = {
      {0.00f, kPhRawDosingFloor},
      {0.29f, kPhRawDosingFloor},
      {0.30f, kPhRawDosingFloor},      // frontière : > strict → palier normal
      {-0.30f, kPhRawDosingFloor},     // le signe n'entre pas en compte
      {0.31f, kPhRawDosingFloorWide},
      {-0.31f, kPhRawDosingFloorWide},
      {kPhOffsetMax, kPhRawDosingFloorWide},
      {kPhOffsetMin, kPhRawDosingFloorWide},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const float tier = (fabsf(cases[i].offset) > kPhOffsetWideThreshold)
                           ? kPhRawDosingFloorWide
                           : kPhRawDosingFloor;
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cases[i].expected, tier);
  }
}

// L'inhibition ORP se déclenche bien AU-DESSUS des planchers acide : les deux
// filets encadrent une fenêtre de dosage non vide (sinon tout dosage serait mort).
void test_orp_inhibit_threshold_is_above_acid_floors(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.60f, kPhRawOrpInhibit);
  TEST_ASSERT_TRUE_MESSAGE(kPhRawOrpInhibit > kPhRawDosingFloorWide,
                           "fenetre de dosage non vide entre plancher et inhibition ORP");
}

// Un offset extrême ne doit pas pouvoir masquer un pH brut dangereux : même à
// l'offset maximal, le pH CORRIGÉ affiché reste dans une fenêtre où le plancher
// brut (6.80/6.90) protège encore contre la sur-acidification.
void test_max_offset_cannot_hide_dangerous_raw_ph(void) {
  const float dangerousRaw = 6.79f;  // sous le plancher normal
  const float displayed = applyPhOffset(dangerousRaw, kPhOffsetMax);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 7.29f, displayed);
  // Le filet, lui, regarde le BRUT : il bloque, quelle que soit la valeur affichée.
  TEST_ASSERT_TRUE_MESSAGE(dangerousRaw < kPhRawDosingFloorWide,
                           "le plancher doit bloquer sur le brut, pas sur l'affiche");
}

void test_refusal_counter_constants(void) {
  TEST_ASSERT_EQUAL_UINT8(3, kPhRawFloorRefusalMax);
  TEST_ASSERT_EQUAL_UINT32(86400000UL, kPhRawFloorRefusalWindowMs);  // 24 h
  // La fenêtre doit rester représentable sur 32 bits en ms (arithmétique millis
  // wrap-safe) : 24 h << 49,7 jours.
  TEST_ASSERT_TRUE(kPhRawFloorRefusalWindowMs < 4294967295UL);
}

// =============================================================================
// NOTE DE COUVERTURE (D3) — ce qui n'est PAS testable ici
// =============================================================================
// PumpControllerClass::isPhRawFloorBlocking() et ::isOrpInhibitedByPhRaw()
// (src/pump_controller.cpp) ne sont PAS des fonctions pures : elles lisent des
// membres d'état (_phRawFloorActive, _phRawFloorRefusals, _phRawFloorWindowStart,
// _lastRefusalCause), des singletons (sensors, mqttCfg, mqttManager,
// systemLogger), millis(), et écrivent la config (saveMqttConfig()). Les
// extraire en pur supposerait un refactor de PumpControllerClass hors périmètre
// feature-067 (et un passage pool-chemistry supplémentaire).
// Restent donc à couvrir par pool-chemistry + tests on-target :
//   - transition d'état du plancher (log critical + alerte MQTT une seule fois),
//   - remontée au-dessus du plancher (levée + log info),
//   - fenêtre glissante 24 h et bascule en mode manuel au 3ᵉ refus,
//   - court-circuit "ph_plus" (plancher acide sans objet),
//   - câblage des 4 branches d'update() AVANT canDose().
// =============================================================================

int main(int, char**) {
  UNITY_BEGIN();
  // D1.1 — application
  RUN_TEST(test_zero_offset_is_identity);
  RUN_TEST(test_positive_offset_applied_to_hundredth);
  RUN_TEST(test_negative_offset_applied_to_hundredth);
  RUN_TEST(test_offset_hundredth_step_is_visible);
  RUN_TEST(test_offset_at_bounds_shifts_by_half_ph);
  RUN_TEST(test_offset_is_reversible);
  RUN_TEST(test_result_is_not_clamped_to_ph_scale);
  // D1.2 — NaN
  RUN_TEST(test_nan_raw_stays_nan_whatever_the_offset);
  RUN_TEST(test_nan_offset_is_neutralized_and_preserves_raw);
  RUN_TEST(test_nan_raw_and_nan_offset_stays_nan);
  RUN_TEST(test_nan_raw_with_infinite_offset_stays_nan);
  // D1.3 — validité
  RUN_TEST(test_valid_offset_inside_range);
  RUN_TEST(test_exact_bounds_are_accepted);
  RUN_TEST(test_just_outside_bounds_is_refused);
  RUN_TEST(test_nan_offset_is_invalid);
  RUN_TEST(test_absurd_offsets_are_invalid);
  RUN_TEST(test_offset_bounds_constants);
  // D1.4 — invariance par translation
  RUN_TEST(test_filter_translation_invariance_positive_offset);
  RUN_TEST(test_filter_translation_invariance_negative_offset);
  RUN_TEST(test_filter_translation_invariance_at_bounds);
  RUN_TEST(test_offset_change_is_immediate_at_output);
  // D2 — non-régression filtre
  RUN_TEST(test_filter_flags_identical_with_and_without_offset);
  RUN_TEST(test_spike_rejection_identical_with_offset);
  RUN_TEST(test_latch_unaffected_by_offset_change_mid_run);
  RUN_TEST(test_warmup_with_offset_still_exposes_nan);
  // D3 — invariants des filets pH brut
  RUN_TEST(test_raw_floor_constants_are_coherent);
  RUN_TEST(test_floor_tier_selection_by_absolute_offset);
  RUN_TEST(test_orp_inhibit_threshold_is_above_acid_floors);
  RUN_TEST(test_max_offset_cannot_hide_dangerous_raw_ph);
  RUN_TEST(test_refusal_counter_constants);
  return UNITY_END();
}
