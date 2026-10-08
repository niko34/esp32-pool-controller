// =============================================================================
// Tests unitaires natifs — Détecteur de température figée (bug-142)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32/DS18B20.
//
// Contexte : le FrozenDetector générique (feature-022, sensor_filter.h) est
// PARTAGÉ avec pH et ORP (kSensorFrozenSamples = 30, couvert par
// test_native_sensor_filter). Ce fichier couvre SPÉCIFIQUEMENT la voie
// température, qui l'utilise directement (src/sensors.h : _waterTempFrozen),
// avec sa propre fenêtre kTempFrozenSamples = 43200 (24 h à 2 s) — jusqu'ici
// NON testée en natif (bug-142).
//
// On ne touche PAS à FrozenDetector ni à SensorFilter : seules les constantes
// kTempFrozen* et le FrozenDetector générique, déjà existant, sont exercés.
//
// D1 — épingle kTempFrozenSamples / kTempFrozenEpsilonC / kTempFrozenWindowSec.
// D2 — dérive réaliste QUANTIFIÉE (pas DS18B20 0.0625 °C, un pas toutes les
//      1h15 = 2250 lectures à 2 s) : ne déclenche JAMAIS sur 50 000 lectures.
// D3 — sonde morte (valeur strictement constante) : frozen() faux à 43199,
//      vrai à 43200 — pas avant.
// =============================================================================

#include <unity.h>
#include <math.h>
#include "constants.h"
#include "sensor_filter.h"

void setUp(void) {}
void tearDown(void) {}

// Pas de quantification du DS18B20 12 bits (0.0625 °C = 1 LSB).
static const float kDs18b20StepC = 0.0625f;

// Dérive la plus rapide mesurée en production le 2026-10-07 : 0.05 °C/h, soit
// 1h15 (4500 s) pour franchir un pas de quantification. À la cadence réelle
// kTempFrozenSamplePeriodMs (2 s), cela représente 2250 lectures entre deux pas.
static const uint32_t kStepEveryNSamples = 2250;

// =============================================================================
// D1 — Verrou de régression sur les valeurs de constants.h
// =============================================================================
void test_temp_frozen_constants_pinned(void) {
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(43200, kTempFrozenSamples,
      "kTempFrozenSamples doit rester a 24h (43200 lectures a 2s) - bug-142");
  // Épingle le NON-changement de l'epsilon : c'est un invariant du lot (inerte
  // sous 0.0625, mais ne doit pas être touché à l'occasion de ce correctif).
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.03f, kTempFrozenEpsilonC,
      "kTempFrozenEpsilonC est INERTE (bug-142) : ne doit pas bouger ici");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(86400UL, kTempFrozenWindowSec,
      "fenetre derivee doit valoir 24h (86400 s)");
}

// =============================================================================
// D2 — Dérive réaliste quantifiée : jamais de faux positif (cœur du bug-142)
// =============================================================================
// ⚠️ Alimente le détecteur avec des valeurs QUANTIFIÉES (multiples exacts de
// 0.0625), PAS une rampe continue : une rampe continue testerait un signal qui
// n'existe pas (le DS18B20 ne rend jamais une valeur intermédiaire) et
// laisserait survivre le défaut corrigé par bug-142 (fenêtre trop courte face
// à un palier de quantification qui dure plus longtemps qu'elle).
void test_realistic_drift_quantized_never_triggers(void) {
  FrozenDetector d(kTempFrozenSamples, kTempFrozenEpsilonC);
  float value = 20.0f;  // multiple exact de 0.0625 (= 320 * 0.0625)
  const uint32_t totalSamples = 50000;
  for (uint32_t i = 0; i < totalSamples; ++i) {
    if (i > 0 && (i % kStepEveryNSamples) == 0) {
      value += kDs18b20StepC;  // franchit un pas de quantification (1h15)
    }
    d.addSample(value);
    TEST_ASSERT_FALSE_MESSAGE(d.frozen(),
        "une derive realiste (pas DS18B20 toutes les 1h15) ne doit JAMAIS "
        "declencher le detecteur sur la fenetre 24h");
  }
}

// =============================================================================
// D3 — Sonde morte : valeur strictement constante, frozen() au 43200e, pas avant
// =============================================================================
void test_dead_sensor_frozen_exactly_at_threshold_not_before(void) {
  FrozenDetector d(kTempFrozenSamples, kTempFrozenEpsilonC);
  const float value = 20.3f;  // valeur plausible, strictement constante

  // 43199 lectures identiques → PAS encore figé.
  for (uint32_t i = 0; i < (uint32_t)kTempFrozenSamples - 1; ++i) {
    d.addSample(value);
    TEST_ASSERT_FALSE_MESSAGE(d.frozen(),
        "ne doit pas se declarer fige avant d'avoir atteint le seuil exact");
  }
  TEST_ASSERT_FALSE_MESSAGE(d.frozen(), "43199 lectures : pas encore fige");

  // 43200e lecture identique → figé.
  d.addSample(value);
  TEST_ASSERT_TRUE_MESSAGE(d.frozen(),
      "43200 lectures identiques (24h) => sonde morte, doit etre figee");
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_temp_frozen_constants_pinned);
  RUN_TEST(test_realistic_drift_quantized_never_triggers);
  RUN_TEST(test_dead_sensor_frozen_exactly_at_threshold_not_before);
  return UNITY_END();
}
