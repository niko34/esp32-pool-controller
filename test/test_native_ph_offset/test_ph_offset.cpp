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
//   D2 — Non-régression du filtre pH : compteur de rejets, figé, périmé et
//        porte STRICTEMENT identiques avec et sans offset. Le filtre ne voit
//        que le brut ; l'offset ne peut donc pas déplacer un seuil de sécurité.
//        feature-152 : le filtre lisse, il ne juge plus — il n'y a donc plus
//        de latch EMI ni de rejet de pic par seuil de déviation (la médiane
//        écarte déjà les aberrations par construction). La suite
//        test_native_sensor_filter reste la source de vérité du module pur
//        (ADR-0017, aucune notion d'offset) ; ici on ne vérifie que la
//        NON-DÉPENDANCE des signaux de diagnostic à l'offset.
//
//   D3 — ARTICULATION offset ↔ seuils de sécurité (refonte v2.25.0) : les seuils
//        chimiques portent sur le pH CORRIGÉ, les deux butées de dernier recours
//        sur le pH BRUT. La machine à états elle-même
//        (PumpControllerClass::isPhGuardBlocking / isOrpInhibitedByPh) n'est PAS
//        pure — voir la note en fin de fichier. Les gardes pures elles-mêmes sont
//        couvertes par test_native_ph_guard ; ici on ne teste QUE ce qui met
//        l'offset en jeu.
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
  PhPipeline maxOffset(kPhOffsetMax);
  uint32_t t0 = 0, t1 = 0, t2 = 0;
  runHarshScenario(noOffset, t0);
  runHarshScenario(withOffset, t1);
  runHarshScenario(maxOffset, t2);
  TEST_ASSERT_EQUAL_UINT32(t0, t1);
  TEST_ASSERT_EQUAL_UINT32(t0, t2);

  // Compteurs de rejets : STRICTEMENT égaux (comparaison entière, pas de tolérance) —
  // exactement 3 (NaN, 20.0, -3.0), identiquement quel que soit l'offset.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, noOffset.filter.rejectedCount(),
                                  "3 rejets attendus : NaN, 20.0, -3.0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(noOffset.filter.rejectedCount(),
                                  withOffset.filter.rejectedCount(),
                                  "compteur de rejets ne doit pas dependre de l'offset");
  TEST_ASSERT_EQUAL_UINT8(noOffset.filter.rejectedCount(), maxOffset.filter.rejectedCount());

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
  PhPipeline p(kPhOffsetMax);
  uint32_t t = 0;
  for (int i = 0; i < (int)kSensorFilterMedianWindow - 1; ++i) {
    p.ingest(7.00f, t);
    TEST_ASSERT_TRUE_MESSAGE(isnan(p.corrected(t)),
                             "warmup + offset => la regulation ne doit voir aucune valeur");
    t += 1000;
  }
  p.ingest(7.00f, t);  // dernier échantillon → fenêtre pleine
  TEST_ASSERT_FALSE(isnan(p.corrected(t)));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.50f, p.corrected(t));
}

// =============================================================================
// D3 — Articulation offset ↔ seuils de sécurité (refonte v2.25.0)
// =============================================================================
// Les seuils CHIMIQUES portent désormais sur le pH CORRIGÉ (une fois l'offset
// appliqué, la valeur EST le pH de l'eau) ; les deux BUTÉES de dernier recours
// portent sur le pH BRUT, pour rester valides quand l'offset est FAUX. Ce bloc
// teste ce que l'offset change — les gardes elles-mêmes (hystérésis, NaN,
// frontières) sont couvertes par test_native_ph_guard.

// Le seuil chimique porte sur le CORRIGÉ : le pH RÉEL protégé dans le bassin est
// donc CONSTANT (6,70) par construction, et c'est la LECTURE BRUTE du seuil qui
// se déplace de −offset. C'est l'inverse exact de l'ancien plancher asservi, qui
// déplaçait le seuil pour garder le pH réel constant — même propriété chimique,
// obtenue sans aucune arithmétique de seuil.
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

// La butée BRUTE est dérivée de l'offset MAXIMAL : elle borne le dégât d'un
// offset POSITIF saisi à tort, sans jamais mordre avant le seuil corrigé pour un
// offset légitime (6,70 − 0,50 = 6,20). Une butée plus haute (6,40 avait été
// proposée) rendrait le seuil corrigé inopérant à offset +0,50.
void test_raw_hard_floor_is_derived_from_the_max_offset(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.20f, kPhFloorRawHard);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, kPhFloorCorrected - kPhOffsetMax, kPhFloorRawHard);
  TEST_ASSERT_TRUE_MESSAGE(kPhFloorRawHard < kPhFloorCorrected,
                           "la butee brute doit rester SOUS le seuil corrige");
  // Symétrique côté haut : butée brute = inhibition ORP + offset max.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.10f, kPhCeilingRawHard);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, kPhOrpInhibit + kPhOffsetMax, kPhCeilingRawHard);
  // EXIGENCE PIRE CAS côté haut, exact symétrique de celle du bas : la butée
  // brute haute ne doit pas être SOUS le plafond corrigé ramené à l'offset le
  // plus défavorable (le plus NÉGATIF, qui abaisse le corrigé à brut donné).
  // Avec l'ancien plafond 7,70 l'exigence valait 8,20 et n'était PAS satisfaite ;
  // depuis kPhHighMargin = 0,10 elle vaut 8,10 et l'est à l'ÉGALITÉ. La tolérance
  // absorbe l'ulp entre deux expressions flottantes distinctes.
  TEST_ASSERT_TRUE_MESSAGE(
      kPhCeilingRawHard >= kPhCeilingCorrected - kPhOffsetMin - 1e-4f,
      "butee brute haute sous le plafond corrige au pire offset : elle mordrait "
      "la premiere et rendrait le plafond inoperant");
  TEST_ASSERT_TRUE_MESSAGE(kPhCeilingCorrected > kPhFloorCorrected,
                           "fenetre de dosage corrigee vide");
}

// Un offset POSITIF saisi à tort (la sonde lit juste, l'utilisateur a saisi
// +0,50 sans raison) rend le seuil corrigé trop permissif : l'eau réelle peut
// descendre jusqu'à la BUTÉE BRUTE avant tout refus. C'est le compromis ASSUMÉ de
// la refonte — la butée n'est plus un plancher chimique, mais la borne du dégât
// qu'un offset menteur peut causer. On la verrouille explicitement : 6,20 brut,
// jamais moins, quel que soit l'offset déclaré.
void test_wrong_positive_offset_is_bounded_by_the_raw_hard_floor(void) {
  const float offsets[] = {0.10f, 0.30f, kPhOffsetMax, 0.80f, 5.0f};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    const float offset = offsets[i];
    char msg[144];
    snprintf(msg, sizeof(msg), "offset menteur=%+.2f", (double)offset);
    // Juste sous la butée : refus, quelle que soit la valeur AFFICHÉE (rassurante).
    TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksBelow(6.19f, kPhFloorRawHard, false), msg);
    // Et la valeur affichée est bien, elle, trompeuse : c'est tout l'objet de la butée.
    TEST_ASSERT_TRUE_MESSAGE(applyPhOffset(6.19f, offset) > kPhFloorCorrected - 0.51f, msg);
  }
  // Cas de référence : offset +0,50 menteur, eau réelle à 6,19. L'affichage
  // annonce 6,69 (rassurant à un centième près du seuil) et c'est le BRUT qui
  // tranche.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 6.69f, applyPhOffset(6.19f, kPhOffsetMax));
  TEST_ASSERT_TRUE(phGuardBlocksBelow(6.19f, kPhFloorRawHard, false));
}

// Miroir : un offset NÉGATIF saisi à tort masquerait une eau réellement basique
// (chlore inactif, régulation ORP qui surdose). La butée brute HAUTE la rattrape
// à 8,10, quelle que soit la valeur corrigée affichée.
void test_wrong_negative_offset_is_bounded_by_the_raw_hard_ceiling(void) {
  const float brut = 8.15f;
  const float affiche = applyPhOffset(brut, kPhOffsetMin);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 7.65f, affiche);
  TEST_ASSERT_TRUE(phGuardBlocksAbove(brut, kPhCeilingRawHard, false));
  // Aucun offset ne peut abaisser la butée : elle ne regarde QUE le brut.
  TEST_ASSERT_TRUE(phGuardBlocksAbove(brut, kPhCeilingRawHard, false));
  TEST_ASSERT_FALSE(phGuardBlocksAbove(8.05f, kPhCeilingRawHard, false));
  // Depuis l'abaissement du plafond (kPhHighMargin 0,20 → 0,10), la valeur
  // AFFICHÉE elle-même (7,65) refuse le pH+ : l'ancien plafond 7,70 l'autorisait
  // encore, dans une eau où les deux gardes ORP suspendaient déjà le chlore.
  TEST_ASSERT_TRUE_MESSAGE(phGuardBlocksAbove(affiche, kPhCeilingCorrected, false),
                           "7,65 corrige doit refuser d'ALCALINISER");
  TEST_ASSERT_TRUE(phGuardBlocksAbove(affiche, kPhOrpInhibit, false));
}

// La fenêtre de dosage est NON VIDE : entre le refus d'acidifier (6,70) et
// l'inhibition ORP (7,60), toute la plage de consigne [7,20 ; 7,50] est libre,
// quel que soit l'offset — sinon tout dosage serait mort-né.
void test_dosing_window_is_never_empty_whatever_the_offset(void) {
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
      // Le PLAFOND d'alcalinisation est désormais ALIGNÉ sur l'inhibition ORP
      // (7,60) : la consigne haute 7,50 doit rester dosable en pH+ malgré cet
      // abaissement, sinon un bassin à TAC bas ne pourrait plus être remonté.
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksAbove(corrige, kPhCeilingCorrected, false), msg);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksBelow(brut, kPhFloorRawHard, false), msg);
      TEST_ASSERT_FALSE_MESSAGE(phGuardBlocksAbove(brut, kPhCeilingRawHard, false), msg);
    }
  }
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
//   - câblage des branches d'update() AVANT canDose().
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
  // D2 — non-régression filtre (feature-152 : plus de latch/rejet de pic à tester)
  RUN_TEST(test_filter_flags_identical_with_and_without_offset);
  RUN_TEST(test_warmup_with_offset_still_exposes_nan);
  // D3 — articulation offset / seuils de sécurité (v2.25.0)
  RUN_TEST(test_corrected_floor_protects_a_constant_real_ph);
  RUN_TEST(test_raw_hard_floor_is_derived_from_the_max_offset);
  RUN_TEST(test_wrong_positive_offset_is_bounded_by_the_raw_hard_floor);
  RUN_TEST(test_wrong_negative_offset_is_bounded_by_the_raw_hard_ceiling);
  RUN_TEST(test_dosing_window_is_never_empty_whatever_the_offset);
  RUN_TEST(test_refusal_counter_constants);
  return UNITY_END();
}
