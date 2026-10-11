#include "sensor_filter.h"

#include <math.h>  // NAN, isnan, fabsf — header C uniquement (module PUR, ADR-0017)

// =============================================================================
// FrozenDetector — implémentation (feature-022, conservée par feature-152)
// =============================================================================

void FrozenDetector::addSample(float x) {
  if (_samples == 0) return;  // Détection désactivée
  if (isnan(x)) return;       // Sécurité : n'alimente jamais le run avec NaN

  if (_runCount == 0) {
    // Premier échantillon depuis reset() : ancre le run.
    _runMin = x;
    _runMax = x;
    _runCount = 1;
    return;
  }

  const float newMin = (x < _runMin) ? x : _runMin;
  const float newMax = (x > _runMax) ? x : _runMax;
  if ((newMax - newMin) < _epsilon) {
    // Échantillon dans la bande : étend les bornes, allonge le run.
    _runMin = newMin;
    _runMax = newMax;
    if (_runCount < UINT16_MAX) ++_runCount;
  } else {
    // Hors bande : le capteur vit → sortie IMMÉDIATE, ré-ancrage du run sur x.
    _runMin = x;
    _runMax = x;
    _runCount = 1;
  }
}

// =============================================================================
// SensorFilter — implémentation (feature-152 : 5 étages, aucun état collant)
// =============================================================================

SensorFilter::SensorFilter(const Config& config)
    : _cfg(config), _frozenDetector(config.frozenSamples, config.frozenEpsilon) {
  // Borne la fenêtre médiane à la capacité physique du buffer (sécurité).
  if (_cfg.medianWindow == 0) _cfg.medianWindow = 1;
  if (_cfg.medianWindow > kSensorFilterMedianWindowMax) {
    _cfg.medianWindow = kSensorFilterMedianWindowMax;
  }
  reset();
}

void SensorFilter::reset() {
  _clearWindow();
  _raw = NAN;
  _rejectedCount = 0;
  _lastValidMs = 0;
  _hasValid = false;
  _gateClosed = false;
  _frozenDetector.reset();
}

void SensorFilter::_clearWindow() {
  for (uint8_t i = 0; i < kSensorFilterMedianWindowMax; ++i) _buffer[i] = NAN;
  _bufIdx = 0;
  _bufCount = 0;
  _filtered = NAN;
}

// -----------------------------------------------------------------------------
// setContext() — porte d'alimentation SIMPLIFIÉE + trou de fraîcheur (A4/C4)
// -----------------------------------------------------------------------------
void SensorFilter::setContext(bool waterPresent, bool measuresValid, uint32_t nowMs) {
  // C4 — trou de fraîcheur : aucune mesure ACCEPTÉE depuis plus de maxAgeMs ⇒
  // vidage IMMÉDIAT, que la porte soit ouverte ou fermée. N'efface PAS
  // _lastValidMs/_hasValid : un silence qui perdure doit continuer à se
  // signaler par stale(), pas se re-présenter en warmup à chaque cycle tant
  // que le silence dure.
  if (_hasValid && (nowMs - _lastValidMs) > _cfg.maxAgeMs) {
    _clearWindow();
  }

  // A4 — porte simplifiée : à la fermeture on vide la fenêtre, à l'ouverture
  // on la laisse se remplir naturellement (aucune action ici). Ni plancher de
  // gel, ni ré-amorçage explicite : le prochain échantillon ACCEPTÉ redate
  // lui-même l'horloge via addSample().
  const bool closed = (!waterPresent) || (!measuresValid);
  if (closed && !_gateClosed) {
    _clearWindow();
  }
  _gateClosed = closed;
}

bool SensorFilter::addSample(float raw, uint32_t nowMs) {
  _raw = raw;

  // ----- Porte fermée : sortie en tête (A4) -----
  // Mesure mémorisée pour le diagnostic (_raw ci-dessus) mais elle ne touche
  // NI le buffer, NI le détecteur figé, NI les compteurs : elle n'existe pas
  // pour le filtre tant que la porte est fermée.
  if (_gateClosed) return false;

  // ----- Étage 2 : rejet de l'absurde (NaN / hors plage PHYSIQUE) -----
  // Comparaison STRICTE (C3, pool-chemistry) : la valeur de SATURATION du
  // module (exactement kOrpFilterMin/Max côté ORP) est rejetée, pas lissée.
  if (isnan(raw)) {
    if (_rejectedCount < 255) ++_rejectedCount;
    return false;
  }
  if (raw <= _cfg.minValue || raw >= _cfg.maxValue) {
    if (_rejectedCount < 255) ++_rejectedCount;
    return false;
  }

  // ----- Mesure acceptée : alimentation du filtre -----
  // Détecteur de sonde figée (C2) — alimenté UNIQUEMENT par les bruts ACCEPTÉS
  // (les rejets ci-dessus n'y passent jamais : frozen() persiste pendant une
  // rafale de rejets).
  _frozenDetector.addSample(raw);

  // Étage 3a : buffer circulaire de la médiane.
  _buffer[_bufIdx] = raw;
  _bufIdx = (_bufIdx + 1) % _cfg.medianWindow;
  if (_bufCount < _cfg.medianWindow) ++_bufCount;

  // Étage 3b : lissage exponentiel UNIQUEMENT une fois la fenêtre PLEINE — une
  // fenêtre partielle donnerait une médiane biaisée par construction et une
  // valeur prématurée (dosage prématuré interdit). NaN tant que non pleine.
  if (_bufCount >= _cfg.medianWindow) {
    const float med = _computeMedian();
    if (isnan(_filtered)) {
      _filtered = med;  // amorçage à la 1ʳᵉ fenêtre pleine (ou après un vidage)
    } else {
      // τ FIXE en TEMPS (D3, indépendance à la cadence) : α recalculé depuis
      // le temps RÉELLEMENT écoulu depuis la dernière mesure ACCEPTÉE —
      // jamais un compteur de cycles déguisé en durée.
      const uint32_t dt = nowMs - _lastValidMs;
      const float alpha = (float)dt / (_cfg.emaTauMs + (float)dt);
      _filtered = alpha * med + (1.0f - alpha) * _filtered;
    }
  }

  _lastValidMs = nowMs;
  _hasValid = true;
  return true;
}

float SensorFilter::_computeMedian() const {
  if (_bufCount == 0) return NAN;

  // Copie locale (sur la pile) des échantillons valides puis tri par insertion.
  // Capacité bornée par kSensorFilterMedianWindowMax → aucune alloc dynamique.
  float tmp[kSensorFilterMedianWindowMax];
  for (uint8_t i = 0; i < _bufCount; ++i) tmp[i] = _buffer[i];

  for (uint8_t i = 1; i < _bufCount; ++i) {
    float key = tmp[i];
    int j = (int)i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      --j;
    }
    tmp[j + 1] = key;
  }

  if (_bufCount & 1) {
    return tmp[_bufCount / 2];
  }
  return 0.5f * (tmp[_bufCount / 2 - 1] + tmp[_bufCount / 2]);
}

float SensorFilter::raw() const { return _raw; }

float SensorFilter::median() const { return _computeMedian(); }

float SensorFilter::value(uint32_t nowMs) const {
  if (warmingUp()) return NAN;
  if (_frozenDetector.frozen()) return NAN;
  if (stale(nowMs)) return NAN;
  return _filtered;
}

bool SensorFilter::stale(uint32_t nowMs) const {
  if (!_hasValid) return true;
  return ageMs(nowMs) > _cfg.maxAgeMs;
}

uint32_t SensorFilter::ageMs(uint32_t nowMs) const {
  if (!_hasValid) return UINT32_MAX;
  // Arithmétique non signée : gère correctement le wrap millis() (~49.7 j).
  return nowMs - _lastValidMs;
}
