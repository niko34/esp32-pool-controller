#include "sensor_filter.h"

#include <math.h>  // NAN, isnan, fabsf — header C uniquement (module PUR, ADR-0017)

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
// SensorFilter — implémentation (feature-025 → feature-058)
// =============================================================================

SensorFilter::SensorFilter(const Config& config)
    : _cfg(config), _frozenDetector(config.frozenSamples, config.frozenEpsilon) {
  // Borne la fenêtre médiane longue à la capacité physique du buffer (sécurité).
  if (_cfg.centralWindow == 0) _cfg.centralWindow = 1;
  if (_cfg.centralWindow > kSensorFilterCentralWindowMax) {
    _cfg.centralWindow = kSensorFilterCentralWindowMax;
  }
  reset();
}

void SensorFilter::reset() {
  for (uint8_t i = 0; i < kSensorFilterCentralWindowMax; ++i) _buffer[i] = NAN;
  _bufIdx = 0;
  _bufCount = 0;
  _raw = NAN;
  _filtered = NAN;
  _validCount = 0;
  _rejectedCount = 0;
  _consecutiveRejects = 0;
  _lastValidMs = 0;
  _hasValid = false;

  // Latch EMI (feature-058) : compteur de fenêtres en dépassement + latch collant.
  _dispersionBreachStreak = 0;
  _dispersionCleanStreak = 0;  // v2.8.4 : streak d'auto-levée
  _unstableLatched = false;

  // feature-022 : réinitialise aussi le détecteur figé (pool-chemistry condition #2).
  _frozenDetector.reset();
}

bool SensorFilter::addSample(float raw, uint32_t nowMs) {
  _raw = raw;

  // ----- Rejet : NaN (compte comme instabilité) -----
  if (isnan(raw)) {
    if (_rejectedCount < 255) ++_rejectedCount;
    if (_consecutiveRejects < 255) ++_consecutiveRejects;
    _dispersionCleanStreak = 0;  // pool-chemistry : tout rejet casse la streak d'auto-levée
    return false;
  }

  // ----- Rejet : hors plage plausible (compte comme instabilité) -----
  if (raw < _cfg.minValue || raw > _cfg.maxValue) {
    if (_rejectedCount < 255) ++_rejectedCount;
    if (_consecutiveRejects < 255) ++_consecutiveRejects;
    _dispersionCleanStreak = 0;  // pool-chemistry : tout rejet casse la streak d'auto-levée
    return false;
  }

  // ----- Rejet : spike DUR isolé (vraie aberration, feature-058) -----
  // |raw − médiane| > spikeReject : point unique franchement hors de la fenêtre.
  // ÉCARTÉ (n'alimente pas le buffer → EXCLU du calcul de dispersion), NE compte
  // PAS comme instabilité (n'incrémente pas consecutiveRejects) et NE met PAS à
  // jour lastValid → l'âge grandit : un spike répété rend le filtre non prêt (C4).
  // Contrairement à l'ancien rejet de « saut » maxStep, ce seuil est LARGE : il
  // ne rejette QUE l'aberration franche, jamais l'oscillation légitime soutenue.
  if (_bufCount > 0) {
    const float med = _computeMedian();
    if (!isnan(med) && fabsf(raw - med) > _cfg.spikeReject) {
      if (_rejectedCount < 255) ++_rejectedCount;  // diagnostic seulement
      // pool-chemistry (CONDITION BLOQUANTE) : un spike dur est EXCLU du buffer donc
      // invisible à la dispersion. Sans ce reset, un capteur crachant des spikes
      // accumulerait la clean streak (dispersion propre entre deux spikes) et
      // lèverait le latch à tort → on casse la streak sur tout spike écarté.
      _dispersionCleanStreak = 0;
      return false;
    }
  }

  // ----- Mesure acceptée : alimentation du filtre -----
  _consecutiveRejects = 0;

  // feature-022 : détection capteur figé — alimentée UNIQUEMENT par les bruts
  // ACCEPTÉS (les rejets ci-dessus n'y passent jamais → frozen PERSISTE pendant
  // une rafale de rejets, pool-chemistry condition #4).
  _frozenDetector.addSample(raw);

  // Buffer circulaire pour la médiane longue (base du calcul de dispersion).
  _buffer[_bufIdx] = raw;
  _bufIdx = (_bufIdx + 1) % _cfg.centralWindow;
  if (_bufCount < _cfg.centralWindow) ++_bufCount;

  // filtered() = EMA(médiane longue) UNIQUEMENT quand la fenêtre est PLEINE (C2) :
  // une fenêtre demi-pleine sous-estimerait la dispersion (→ jugement d'instabilité
  // trop laxiste) et sortirait une valeur exploitable prématurée (→ dosage
  // prématuré interdit). NaN tant que la fenêtre n'est pas pleine. La valeur n'est
  // JAMAIS figée ni ré-amorcée par une instabilité (C5) : elle suit toujours la
  // médiane longue recalculée.
  if (_bufCount >= _cfg.centralWindow) {
    const float med = _computeMedian();
    if (isnan(_filtered)) {
      _filtered = med;  // amorçage à la 1ʳᵉ fenêtre pleine
    } else {
      _filtered = _cfg.emaAlpha * med + (1.0f - _cfg.emaAlpha) * _filtered;
    }

    // Dispersion de la fenêtre PLEINE (brut accepté) → latch EMI si dépassement
    // SOUTENU (N fenêtres consécutives). Un pic de dispersion ponctuel inhibe déjà
    // la régulation via unstable() sans latcher ; seul un bruit EMI persistant
    // pose le latch collant (levé uniquement par reset()/recalibration).
    const float disp = _computeDispersion();
    if (!isnan(disp) && disp > _cfg.maxDispersion) {
      if (_dispersionBreachStreak < 255) ++_dispersionBreachStreak;
      _dispersionCleanStreak = 0;  // fenêtre en dépassement → streak propre cassée
      if (_dispersionBreachStreak >= kSensorFilterDispersionLatchCycles) {
        _unstableLatched = true;
      }
    } else {
      // Fenêtre PLEINE et PROPRE (disp <= cap) : on atteint ce bloc uniquement après
      // une mesure ACCEPTÉE (les rejets NaN/hors-plage/spike ont return avant, en
      // remettant _dispersionCleanStreak=0) → la streak ne compte QUE des fenêtres
      // pleines sans aucun rejet (pool-chemistry, anti-flapping).
      _dispersionBreachStreak = 0;
      if (_dispersionCleanStreak < 255) ++_dispersionCleanStreak;

      // Auto-levée du latch EMI collant (feature v2.8.4) : après N fenêtres propres
      // consécutives, le bruit EMI est jugé résorbé → on relâche le latch. Asymétrie
      // voulue (rapide à poser, lent à lever). La journalisation « latch EMI levé »
      // est déléguée à la coquille (SensorManager) qui détecte la transition
      // unstableLatched() true→false : le module reste 100 % pur (pas d'Arduino.h /
      // String / logger ici, ADR-0017) et ne peut donc pas logger lui-même.
      if (_unstableLatched &&
          _dispersionCleanStreak >= kSensorFilterLatchClearCycles) {
        _unstableLatched = false;
      }
    }
  }

  if (_validCount < 255) ++_validCount;
  _lastValidMs = nowMs;
  _hasValid = true;
  return true;
}

float SensorFilter::_computeMedian() const {
  if (_bufCount == 0) return NAN;

  // Copie locale (sur la pile) des échantillons valides puis tri par insertion.
  // Capacité bornée par kSensorFilterCentralWindowMax → aucune alloc dynamique.
  float tmp[kSensorFilterCentralWindowMax];
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

float SensorFilter::_computeDispersion() const {
  // C2 : la dispersion n'est jugée que sur une fenêtre PLEINE. Un buffer partiel
  // sous-estimerait max−min → risque de juger "stable" un signal bruité → dosage
  // prématuré interdit. NaN tant que la fenêtre n'est pas pleine.
  if (_bufCount < _cfg.centralWindow) return NAN;

  float mn = _buffer[0];
  float mx = _buffer[0];
  for (uint8_t i = 1; i < _bufCount; ++i) {
    if (_buffer[i] < mn) mn = _buffer[i];
    if (_buffer[i] > mx) mx = _buffer[i];
  }
  return mx - mn;
}

float SensorFilter::raw() const { return _raw; }

float SensorFilter::median() const { return _computeMedian(); }

float SensorFilter::filtered() const { return _filtered; }

float SensorFilter::dispersion() const { return _computeDispersion(); }

bool SensorFilter::ready(uint32_t nowMs) const {
  // C2 : fenêtre centrale PLEINE requise (valeur exploitable) ET mesure récente
  // ET non figé. Tant que la fenêtre n'est pas pleine → filtered() = NaN.
  if (_bufCount < _cfg.centralWindow) return false;
  if (!_hasValid) return false;
  if (ageMs(nowMs) > _cfg.maxAgeMs) return false;
  // feature-022 : capteur figé → non prêt (fail-closed via la garde FilterNotReady
  // existante de evaluateDose — pas de nouvelle valeur DoseRefusal).
  if (_frozenDetector.frozen()) return false;
  return true;
}

bool SensorFilter::frozen() const { return _frozenDetector.frozen(); }

bool SensorFilter::unstable() const {
  // DRAPEAU d'inhibition dosage — DÉCOUPLÉ de la valeur filtrée (C3/C5) :
  //   (b) latch EMI collant (bruit soutenu) ;
  //   (c) rejets NaN/hors-plage consécutifs (capteur qui débite du garbage) ;
  //   (a) dispersion de la fenêtre PLEINE > cap (oscillation de niveau EMI, C1).
  if (_unstableLatched) return true;
  if (_consecutiveRejects >= _cfg.maxConsecutiveRejects) return true;
  const float disp = _computeDispersion();
  if (!isnan(disp) && disp > _cfg.maxDispersion) return true;
  return false;
}

uint8_t SensorFilter::rejectedCount() const { return _rejectedCount; }

uint8_t SensorFilter::consecutiveRejects() const { return _consecutiveRejects; }

bool SensorFilter::unstableLatched() const { return _unstableLatched; }

uint8_t SensorFilter::dispersionCleanStreak() const { return _dispersionCleanStreak; }

uint32_t SensorFilter::ageMs(uint32_t nowMs) const {
  if (!_hasValid) return UINT32_MAX;
  // Arithmétique non signée : gère correctement le wrap millis() (~49.7 j).
  return nowMs - _lastValidMs;
}
