// =============================================================================
// measure_validity — Coquille impure de la validité des mesures pH/ORP
// (feature-061) — voir measure_validity.h
// =============================================================================

#include "measure_validity.h"
#include "filtration.h"
#include "config.h"
#include "logger.h"
#include "sensors.h"  // v2.20.3 (lot E) : instrumentation des bruts pendant la purge

MeasureValidityManager measureValidity;

namespace {

// Formatage NaN-safe pour l'instrumentation de stabilisation (lot E) : une mesure
// absente doit se lire "n/a" et jamais "nan" ou "0.00" (qui se confondrait avec
// une vraie valeur dans le journal).
String fmtOrNa(float v, unsigned int decimals) {
  return isnan(v) ? String("n/a") : String(v, decimals);
}

String deltaOrNa(float before, float after, unsigned int decimals) {
  if (isnan(before) || isnan(after)) return String("n/a");
  const float d = after - before;
  return (d >= 0.0f ? String("+") : String("")) + String(d, decimals);
}

}  // namespace

void MeasureValidityManager::update() {
  // Collecte (loopTask uniquement — resolveWaterPresence() consommé en lecture
  // seule, jamais depuis un contexte async).
  WaterPresence wp = filtration.resolveWaterPresence();
  bool water = wp.waterPresent;
  uint32_t now = (uint32_t)millis();
  // Délai de purge = délai de stabilisation utilisateur (même sémantique que la
  // garde dosage : temps de renouvellement de l'eau dans la chambre de mesure).
  int delayMin = mqttCfg.stabilizationDelayMin;
  uint32_t purgeDelayMs = (delayMin > 0) ? (uint32_t)delayMin * 60000UL : 0UL;

  // Front montant d'eau (y compris au premier update() si eau déjà présente :
  // délai complet appliqué au boot — conservateur, cf. en-tête).
  if (water && !_lastWaterPresent) {
    // 0 est la sentinelle « jamais » de la logique pure → armer à 1 minimum.
    _waterRiseMs = (now == 0) ? 1UL : now;
    // v2.20.3 (lot E) — instrumentation : on mémorise les BRUTS à l'instant de la
    // reprise de circulation pour pouvoir comparer, en fin de purge, l'eau qui
    // stagnait dans la chambre de mesure et l'eau du bassin. C'est ce delta qui
    // tranchera l'origine des excursions observées (stagnation acide au voisinage
    // du point d'injection CONTRE défaillance de sonde) — question NON tranchée.
    _phRawAtRise = sensors.getPhRaw();    // BRUT EZO, sans offset (NaN si stale)
    _orpRawAtRise = sensors.getOrpRaw();  // BRUT EZO (NaN si stale)
    if (purgeDelayMs > 0) {
      systemLogger.info("Reprise circulation — stabilisation des mesures pH/ORP " +
                        String(delayMin) + " min");
      systemLogger.info("[STAB] Bruts à la reprise : pH=" + fmtOrNa(_phRawAtRise, 3) +
                        " ORP=" + fmtOrNa(_orpRawAtRise, 1) + " mV (eau stagnante)");
    }
  }
  _lastWaterPresent = water;

  MeasureValidityInputs in;
  in.waterPresent = water;
  in.nowMs = now;
  in.waterRiseMs = _waterRiseMs;
  in.purgeDelayMs = purgeDelayMs;
  MeasureValidityResult r = evaluateMeasureValidity(in);

  // Log de fin de stabilisation (transition purge → valide uniquement ; une
  // retombée d'eau pendant la purge n'émet rien).
  if (_wasStabilizing && r.valid) {
    systemLogger.info("Stabilisation terminée — mesures pH/ORP de nouveau représentatives");
    // v2.20.3 (lot E) — bruts de fin de purge + DELTA vs reprise. Un delta pH
    // fortement négatif à la reprise (eau stagnante plus acide que le bassin)
    // confirmerait l'hypothèse de stagnation au point d'injection ; un delta nul
    // avec une valeur aberrante orienterait vers une défaillance de sonde.
    const float phNow = sensors.getPhRaw();
    const float orpNow = sensors.getOrpRaw();
    systemLogger.info("[STAB] Bruts en fin de purge : pH=" + fmtOrNa(phNow, 3) +
                      " (delta " + deltaOrNa(_phRawAtRise, phNow, 3) + ") ORP=" +
                      fmtOrNa(orpNow, 1) + " mV (delta " +
                      deltaOrNa(_orpRawAtRise, orpNow, 1) + ")");
  }
  _wasStabilizing = water && !r.valid;

  // Publication du verdict — section critique MINIMALE (3 copies).
  portENTER_CRITICAL(&_mux);
  _cachedWaterPresent = water;
  _cachedValid = r.valid;
  _cachedRemainingS = r.stabilizationRemainingS;
  portEXIT_CRITICAL(&_mux);
}

void MeasureValidityManager::getSnapshot(bool& waterPresent, bool& measuresValid,
                                         uint32_t& stabilizationRemainingS) const {
  portENTER_CRITICAL(&_mux);
  waterPresent = _cachedWaterPresent;
  measuresValid = _cachedValid;
  stabilizationRemainingS = _cachedRemainingS;
  portEXIT_CRITICAL(&_mux);
}

bool MeasureValidityManager::waterPresent() const {
  portENTER_CRITICAL(&_mux);
  bool v = _cachedWaterPresent;
  portEXIT_CRITICAL(&_mux);
  return v;
}

bool MeasureValidityManager::measuresValid() const {
  portENTER_CRITICAL(&_mux);
  bool v = _cachedValid;
  portEXIT_CRITICAL(&_mux);
  return v;
}

uint32_t MeasureValidityManager::stabilizationRemainingS() const {
  portENTER_CRITICAL(&_mux);
  uint32_t v = _cachedRemainingS;
  portEXIT_CRITICAL(&_mux);
  return v;
}
