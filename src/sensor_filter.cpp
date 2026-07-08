#include "sensor_filter.h"

#include <math.h>

#include "logger.h"  // systemLogger (module indépendant, pas de dépendance circulaire)

// =============================================================================
// FrozenDetector — implémentation (feature-022 Passe 2)
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
// SensorFilter — implémentation (feature-025)
// =============================================================================

SensorFilter::SensorFilter(const Config& config)
    : _cfg(config), _frozenDetector(config.frozenSamples, config.frozenEpsilon) {
  // Borne la fenêtre médiane à la capacité physique du buffer (sécurité).
  if (_cfg.medianWindow == 0) _cfg.medianWindow = 1;
  if (_cfg.medianWindow > kSensorFilterMedianWindow) {
    _cfg.medianWindow = kSensorFilterMedianWindow;
  }
  reset();
}

void SensorFilter::reset() {
  for (uint8_t i = 0; i < kSensorFilterMedianWindow; ++i) _buffer[i] = NAN;
  _bufIdx = 0;
  _bufCount = 0;
  _raw = NAN;
  _filtered = NAN;
  _validCount = 0;
  _rejectedCount = 0;
  _consecutiveRejects = 0;
  _lastValidMs = 0;
  _hasValid = false;

  // Re-sync : mini-buffer de bruts rejetés + état anti-boucle latché.
  for (uint8_t i = 0; i < kSensorFilterMedianWindow; ++i) _rejBuffer[i] = NAN;
  _rejIdx = 0;
  _rejCount = 0;
  for (uint8_t i = 0; i < kSensorFilterMaxResyncPerWindow; ++i) _resyncTimestamps[i] = 0;
  _resyncCount = 0;
  _unstableLatched = false;

  // feature-022 : réinitialise aussi le détecteur figé (pool-chemistry condition #2).
  _frozenDetector.reset();
}

bool SensorFilter::addSample(float raw, uint32_t nowMs) {
  _raw = raw;

  // En warmup tant que le nombre de mesures valides n'a pas atteint le seuil.
  // Pendant le warmup, on n'applique PAS la garde de saut (maxStep) : le filtre
  // doit pouvoir converger vers une valeur initiale éloignée de l'état par défaut.
  const bool warmingUp = (_validCount < _cfg.warmupSamples);

  // ----- Rejet : NaN -----
  if (isnan(raw)) {
    if (_rejectedCount < 255) ++_rejectedCount;
    if (_consecutiveRejects < 255) ++_consecutiveRejects;
    return false;
  }

  // ----- Rejet : hors plage plausible -----
  if (raw < _cfg.minValue || raw > _cfg.maxValue) {
    if (_rejectedCount < 255) ++_rejectedCount;
    if (_consecutiveRejects < 255) ++_consecutiveRejects;
    return false;
  }

  // ----- Rejet : saut instantané excessif (hors warmup) -----
  if (!warmingUp && !isnan(_filtered)) {
    if (fabsf(raw - _filtered) > _cfg.maxStep) {
      if (_rejectedCount < 255) ++_rejectedCount;  // cumulatif diagnostic, jamais remis à 0 par re-sync
      if (_consecutiveRejects < 255) ++_consecutiveRejects;

      // Mémorise ce brut rejeté (mini-buffer circulaire) pour l'amorçage de re-sync.
      _rejBuffer[_rejIdx] = raw;
      _rejIdx = (_rejIdx + 1) % _cfg.medianWindow;
      if (_rejCount < _cfg.medianWindow) ++_rejCount;

      // Saut DURABLE : au-delà du seuil de re-sync, on conclut à un vrai changement
      // (et non un pic isolé) et on ré-amorce le filtre. Ré-amorçage sur la MÉDIANE
      // des derniers bruts rejetés afin d'ignorer un éventuel pic final.
      if (_consecutiveRejects >= kSensorFilterResyncRejects) {
        const float seed = _computeRejectedMedian();
        const float oldFiltered = _filtered;

        // Comptabilise la re-sync sur la fenêtre glissante (anti-boucle EMI).
        // Purge les timestamps trop anciens (> kSensorFilterResyncWindowMs).
        uint8_t kept = 0;
        for (uint8_t i = 0; i < _resyncCount; ++i) {
          if ((uint32_t)(nowMs - _resyncTimestamps[i]) <= kSensorFilterResyncWindowMs) {
            _resyncTimestamps[kept++] = _resyncTimestamps[i];
          }
        }
        _resyncCount = kept;
        if (_resyncCount < kSensorFilterMaxResyncPerWindow) {
          _resyncTimestamps[_resyncCount++] = nowMs;
        } else {
          // Décale (FIFO) pour conserver le timestamp courant dans la fenêtre.
          for (uint8_t i = 1; i < kSensorFilterMaxResyncPerWindow; ++i) {
            _resyncTimestamps[i - 1] = _resyncTimestamps[i];
          }
          _resyncTimestamps[kSensorFilterMaxResyncPerWindow - 1] = nowMs;
        }

        // Ré-amorçage : retour warmup autour de la médiane des bruts rejetés.
        for (uint8_t i = 0; i < kSensorFilterMedianWindow; ++i) _buffer[i] = NAN;
        _bufIdx = 0;
        _bufCount = 0;
        _filtered = NAN;
        _validCount = 0;          // → repasse en warmup (les cycles suivants ignorent maxStep)
        _consecutiveRejects = 0;  // reset des consécutifs, mais _rejectedCount conservé
        // Le mini-buffer de rejetés est purgé : il a servi à l'amorçage.
        for (uint8_t i = 0; i < kSensorFilterMedianWindow; ++i) _rejBuffer[i] = NAN;
        _rejIdx = 0;
        _rejCount = 0;

        systemLogger.warning(String("[SensorFilter] re-sync : changement durable détecté, ") +
                             "ancienne valeur=" + String(oldFiltered, 3) +
                             " → médiane d'amorçage=" + String(seed, 3) +
                             " (re-sync " + String(_resyncCount) + "/" +
                             String(kSensorFilterMaxResyncPerWindow) + " dans la fenêtre)");

        // Anti-boucle latché : trop de re-sync dans la fenêtre → défaut EMI probable.
        if (_resyncCount >= kSensorFilterMaxResyncPerWindow) {
          _unstableLatched = true;
          systemLogger.critical(String("[SensorFilter] latch instable : ") +
                                String(_resyncCount) + " re-sync en " +
                                String(kSensorFilterResyncWindowMs / 1000) +
                                " s (EMI suspecté) — dosage bloqué jusqu'à reset/calibration");
        }
      }
      return false;
    }
  }

  // ----- Mesure acceptée : alimentation du filtre -----
  _consecutiveRejects = 0;

  // 0) feature-022 : détection capteur figé — alimentée UNIQUEMENT par les bruts
  //    ACCEPTÉS (les rejets ci-dessus n'y passent jamais → frozen PERSISTE pendant
  //    une rafale de rejets, pool-chemistry condition #4).
  _frozenDetector.addSample(raw);

  // 1) Buffer circulaire pour la médiane.
  _buffer[_bufIdx] = raw;
  _bufIdx = (_bufIdx + 1) % _cfg.medianWindow;
  if (_bufCount < _cfg.medianWindow) ++_bufCount;

  // 2) EMA sur la médiane courante (la médiane absorbe déjà un pic isolé).
  float med = _computeMedian();
  if (isnan(_filtered)) {
    _filtered = med;  // amorçage à la 1ʳᵉ médiane disponible
  } else {
    _filtered = _cfg.emaAlpha * med + (1.0f - _cfg.emaAlpha) * _filtered;
  }

  if (_validCount < 255) ++_validCount;
  _lastValidMs = nowMs;
  _hasValid = true;
  return true;
}

float SensorFilter::_computeMedian() const {
  if (_bufCount == 0) return NAN;

  // Copie locale (sur la pile) des échantillons valides puis tri par insertion.
  // Capacité bornée par kSensorFilterMedianWindow → aucune alloc dynamique.
  float tmp[kSensorFilterMedianWindow];
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

float SensorFilter::_computeRejectedMedian() const {
  if (_rejCount == 0) return _raw;  // fallback : échantillon courant si aucun rejeté mémorisé

  float tmp[kSensorFilterMedianWindow];
  for (uint8_t i = 0; i < _rejCount; ++i) tmp[i] = _rejBuffer[i];

  for (uint8_t i = 1; i < _rejCount; ++i) {
    float key = tmp[i];
    int j = (int)i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      --j;
    }
    tmp[j + 1] = key;
  }

  if (_rejCount & 1) {
    return tmp[_rejCount / 2];
  }
  return 0.5f * (tmp[_rejCount / 2 - 1] + tmp[_rejCount / 2]);
}

float SensorFilter::raw() const { return _raw; }

float SensorFilter::median() const { return _computeMedian(); }

float SensorFilter::filtered() const { return _filtered; }

bool SensorFilter::ready(uint32_t nowMs) const {
  // Warmup atteint ET au moins une mesure valide ET mesure récente ET non figé.
  if (_validCount < _cfg.warmupSamples) return false;
  if (!_hasValid) return false;
  if (ageMs(nowMs) > _cfg.maxAgeMs) return false;
  // feature-022 : capteur figé → non prêt (fail-closed via la garde FilterNotReady
  // existante de evaluateDose — pas de nouvelle valeur DoseRefusal).
  if (_frozenDetector.frozen()) return false;
  return true;
}

bool SensorFilter::frozen() const { return _frozenDetector.frozen(); }

bool SensorFilter::unstable() const {
  // Instable si trop de rejets consécutifs OU latch anti-boucle EMI posé.
  return (_consecutiveRejects >= _cfg.maxConsecutiveRejects) || _unstableLatched;
}

uint8_t SensorFilter::rejectedCount() const { return _rejectedCount; }

uint8_t SensorFilter::consecutiveRejects() const { return _consecutiveRejects; }

uint8_t SensorFilter::resyncCount() const { return _resyncCount; }

bool SensorFilter::unstableLatched() const { return _unstableLatched; }

uint32_t SensorFilter::ageMs(uint32_t nowMs) const {
  if (!_hasValid) return UINT32_MAX;
  // Arithmétique non signée : gère correctement le wrap millis() (~49.7 j).
  return nowMs - _lastValidMs;
}
