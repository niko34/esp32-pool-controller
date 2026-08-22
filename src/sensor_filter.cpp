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

  // v2.20.3 : reset() est le « gros marteau » (calibration, reprise manuelle) — il
  // remet aussi la porte et la sortie de secours à neuf. La porte sera recalculée
  // au prochain setContext() ; on repart donc d'un état ouvert (comportement
  // antérieur pour un appelant qui n'utilise pas setContext()).
  _gateClosed = false;
  _lastWaterPresent = false;
  _gateFloorArmed = false;
  _gateFloorUntilMs = 0;
  _spikeCount = 0;
  _spikeMin = NAN;
  _spikeMax = NAN;
  _spikeGaps = 0;
  _lastWasGap = false;
  // Sans cela, /debug/sensor_filter_state pouvait afficher après un reset manuel
  // une valeur héritée d'un ré-amorçage antérieur (relevé en revue de tests).
  _rejectedBeforeRearm = 0;
  _lastRearmMs = 0;
  _hasRearmed = false;
  _rearmedFlag = false;
  _rearmDenied = 0;
}

// -----------------------------------------------------------------------------
// _noteSpikeSeriesGap() — tolérance aux trous (v2.20.3)
// -----------------------------------------------------------------------------
// Trou de lecture (NaN ou hors plage) survenu PENDANT une série de rejets
// cohérents. Condition BLOQUANTE du gate pool-chemistry (v2.20.3) : remettre la
// série à zéro sur le moindre trou rendait la sortie de secours inatteignable
// pour une sonde ratant un cycle sur douze — c'est-à-dire que l'incident
// d'origine se serait reproduit à l'identique. Règle chiffrée par le gate :
// au plus 2 trous par série, JAMAIS 2 consécutifs. Un trou ne fait PAS avancer
// _spikeCount (l'évidence doit rester acquise par des rejets réels).
void SensorFilter::_noteSpikeSeriesGap() {
  if (_spikeCount == 0) return;  // pas de série en cours : rien à préserver
  if (_lastWasGap || _spikeGaps >= 2) {
    // 2 trous consécutifs, ou 3ᵉ trou de la série → l'évidence n'est plus
    // continue : on repart de zéro (fail-closed).
    _spikeCount = 0;
    _spikeMin = NAN;
    _spikeMax = NAN;
    _spikeGaps = 0;
  } else {
    ++_spikeGaps;
  }
  _lastWasGap = true;
}

// -----------------------------------------------------------------------------
// rearm() — ré-amorçage de l'ESTIMATEUR seul (v2.20.3)
// -----------------------------------------------------------------------------
// Vide l'estimateur (buffer, EMA, streak de dispersion propre) pour le faire
// re-converger sur les mesures à venir, SANS toucher :
//   - au FrozenDetector (feature-022) : une sonde figée le reste, et le restera
//     tant qu'elle n'aura pas fourni d'échantillon hors bande ;
//   - au latch EMI (feature-058) : une ambiance EMI soutenue continue d'inhiber
//     le dosage — re-converger ne doit JAMAIS blanchir un défaut de mesure.
// Conséquence voulue : après rearm(), _bufCount = 0 → ready() = false pendant tout
// le warmup (centralWindow échantillons) → dosage fail-closed pendant la reprise.
void SensorFilter::rearm() {
  for (uint8_t i = 0; i < kSensorFilterCentralWindowMax; ++i) _buffer[i] = NAN;
  _bufIdx = 0;
  _bufCount = 0;
  _filtered = NAN;
  _dispersionCleanStreak = 0;

  // Bookkeeping de la sortie de secours : la série de rejets a rempli son office.
  _spikeCount = 0;
  _spikeMin = NAN;
  _spikeMax = NAN;
  _spikeGaps = 0;
  _lastWasGap = false;
  _rearmDenied = 0;
  // Compteur de rejets remis à zéro (revue pool-chemistry, point (a)) : sans
  // cela /debug/sensor_filter_state affichait encore `rejected=255` après un
  // ré-amorçage réussi, et l'exploitant ne pouvait pas distinguer un filtre
  // réparé d'un filtre encore verrouillé. La valeur d'avant est reportée par la
  // coquille dans le journal de ré-amorçage.
  _rejectedBeforeRearm = _rejectedCount;
  _rejectedCount = 0;
  _rearmedFlag = true;  // drainé par rearmedSinceLastPoll() (journalisation coquille)
}

// -----------------------------------------------------------------------------
// setContext() — porte d'alimentation (v2.20.3)
// -----------------------------------------------------------------------------
void SensorFilter::setContext(bool waterPresent, bool measuresValid, uint32_t nowMs) {
  // Front montant d'eau → plancher de gel : la chambre de mesure contient encore
  // l'eau STAGNANTE (potentiellement très acide au voisinage du point d'injection).
  // On l'applique même si la coquille déclare déjà les mesures représentatives
  // (délai de stabilisation utilisateur réglé à 0).
  if (waterPresent && !_lastWaterPresent) {
    _gateFloorUntilMs = nowMs + kSensorFilterGateFloorMs;
    _gateFloorArmed = true;
  }
  _lastWaterPresent = waterPresent;

  bool closed = (!waterPresent) || (!measuresValid);
  if (!closed && _gateFloorArmed) {
    // Comparaison signée sur la différence non signée : robuste au wrap millis().
    if ((int32_t)(nowMs - _gateFloorUntilMs) < 0) {
      closed = true;      // plancher encore actif
    } else {
      _gateFloorArmed = false;  // plancher écoulé
    }
  }

  // Transition fermé → ouvert : les mesures redeviennent représentatives. La
  // médiane construite AVANT la fermeture (ou héritée de la stagnation) ne doit
  // pas leur faire autorité → on ré-amorce l'estimateur.
  if (_gateClosed && !closed) {
    rearm();
  }
  _gateClosed = closed;
}

bool SensorFilter::gateClosed() const { return _gateClosed; }

bool SensorFilter::rearmedSinceLastPoll() {
  const bool v = _rearmedFlag;
  _rearmedFlag = false;
  return v;
}

uint8_t SensorFilter::rearmDenied() const { return _rearmDenied; }

bool SensorFilter::addSample(float raw, uint32_t nowMs) {
  _raw = raw;

  // ----- PORTE FERMÉE (v2.20.3) : sortie EN TÊTE -----
  // Mesure déclarée NON REPRÉSENTATIVE par le système (eau à l'arrêt, purge de la
  // chambre de mesure en cours). Elle est mémorisée pour le diagnostic (_raw
  // ci-dessus) mais ne touche NI le buffer, NI le FrozenDetector, NI aucun
  // compteur : elle n'existe pas pour le filtre. L'âge de la dernière mesure
  // valide continue de grandir → ready() = false → dosage fail-closed pendant
  // toute la purge, ce qui est le comportement voulu.
  if (_gateClosed) return false;

  // ----- Rejet : NaN (compte comme instabilité) -----
  if (isnan(raw)) {
    if (_rejectedCount < 255) ++_rejectedCount;
    if (_consecutiveRejects < 255) ++_consecutiveRejects;
    _dispersionCleanStreak = 0;  // pool-chemistry : tout rejet casse la streak d'auto-levée
    _noteSpikeSeriesGap();       // v2.20.3 : trou TOLÉRÉ (cf. _noteSpikeSeriesGap)
    return false;
  }

  // ----- Rejet : hors plage plausible (compte comme instabilité) -----
  if (raw < _cfg.minValue || raw > _cfg.maxValue) {
    if (_rejectedCount < 255) ++_rejectedCount;
    if (_consecutiveRejects < 255) ++_consecutiveRejects;
    _dispersionCleanStreak = 0;  // pool-chemistry : tout rejet casse la streak d'auto-levée
    _noteSpikeSeriesGap();       // v2.20.3 : trou TOLÉRÉ (cf. branche NaN ci-dessus)
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

      // ----- SORTIE DE SECOURS (v2.20.3) : rejets SOUTENUS et COHÉRENTS -----
      // Sans ceci, la médiane ne pouvant évoluer qu'avec des échantillons acceptés,
      // un échelon légitime durable > spikeReject verrouille le filtre POUR
      // TOUJOURS (incident de production : plus aucune régulation pendant ≥ 3 h).
      // On mesure la cohérence MUTUELLE des rejets : une série resserrée décrit une
      // NOUVELLE valeur ; une série dispersée est du bruit et ré-ancre la série.
      _lastWasGap = false;  // rejet RÉEL : la continuité de l'évidence reprend
      if (_spikeCount == 0) {
        _spikeMin = raw;
        _spikeMax = raw;
        _spikeCount = 1;
        _spikeGaps = 0;
      } else {
        const float mn = (raw < _spikeMin) ? raw : _spikeMin;
        const float mx = (raw > _spikeMax) ? raw : _spikeMax;
        if ((mx - mn) <= _cfg.rearmCoherence) {
          _spikeMin = mn;
          _spikeMax = mx;
          if (_spikeCount < 255) ++_spikeCount;
        } else {
          // Incohérent avec la série en cours → ré-ancrage immédiat sur raw : du
          // bruit dispersé n'atteindra jamais le compte de ré-amorçage.
          _spikeMin = raw;
          _spikeMax = raw;
          _spikeCount = 1;
          _spikeGaps = 0;
        }
      }

      // rearmCoherence == 0 → sortie de secours DÉSACTIVÉE (init agrégat courte).
      if (_cfg.rearmCoherence > 0.0f &&
          _spikeCount >= kSensorFilterSpikeRearmCount) {
        // TROIS gardes CUMULATIVES (pool-chemistry) — une sonde réellement HS ne
        // doit jamais pouvoir imposer sa valeur au filtre.
        const bool cooldownOk =
            !_hasRearmed ||
            ((nowMs - _lastRearmMs) >= kSensorFilterRearmCooldownMs);
        // Bande jugée sur le CENTRE de la série, pas sur le dernier brut reçu
        // (revue pool-chemistry, point (c)) : le centre représente la valeur
        // vers laquelle on s'apprête à converger, et la fuite hors bande est
        // ainsi divisée par deux à disponibilité identique.
        const float target = (_spikeMin + _spikeMax) * 0.5f;
        const bool bandOk = (target >= _cfg.rearmMin && target <= _cfg.rearmMax);
        const bool coherenceOk = ((_spikeMax - _spikeMin) <= _cfg.rearmCoherence);

        if (!cooldownOk) {
          _rearmDenied = 1;  // collant : la coquille alarme (critical + alerte)
        } else if (!bandOk) {
          _rearmDenied = 2;  // valeur non plausible pour un bassin → fail-closed
        } else if (coherenceOk) {
          // Les N échantillons rejetés sont JETÉS, jamais ré-injectés dans le
          // buffer : le filtre re-converge uniquement sur les mesures À VENIR,
          // après un warmup complet (dosage bloqué pendant la re-convergence).
          rearm();
          _lastRearmMs = nowMs;
          _hasRearmed = true;
        }
        // Dans tous les cas : la série a été jugée, on repart d'une série neuve
        // (rearm() l'a déjà fait en cas de succès — idempotent). L'état de
        // tolérance aux trous est remis à zéro ICI aussi (revue 2.20.3) : il est
        // aujourd'hui inoffensif de l'omettre, mais seul un raisonnement non
        // local le montre. L'invariant « pas de série ⇒ pas d'état de trou »
        // doit tenir localement.
        _spikeCount = 0;
        _spikeMin = NAN;
        _spikeMax = NAN;
        _spikeGaps = 0;
        _lastWasGap = false;
      }
      return false;
    }
  }

  // ----- Mesure acceptée : alimentation du filtre -----
  _consecutiveRejects = 0;
  // v2.20.3 : une mesure acceptée invalide la série de rejets en cours (elle n'est
  // plus « consécutive ») — sinon deux rafales séparées se cumuleraient.
  _spikeCount = 0;
  _spikeMin = NAN;
  _spikeMax = NAN;
  _spikeGaps = 0;
  _lastWasGap = false;

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
