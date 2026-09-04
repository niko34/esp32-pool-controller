// =============================================================================
// measure_validity — Coquille impure de la validité des mesures pH/ORP
// (feature-061 ; purge ADAPTATIVE par la feature-091, v2.34.0)
// — voir measure_validity.h
// =============================================================================

#include "measure_validity.h"

#include <esp_system.h>  // esp_reset_reason
#include <time.h>

#include "config.h"
#include "constants.h"
#include "filtration.h"
#include "logger.h"
#include "mqtt_manager.h"  // publishAlert (plafond de purge)
#include "sensors.h"       // bruts EZO pour le détecteur de stabilité

MeasureValidityManager measureValidity;

namespace {

// Mémoire RTC de continuité (C3) : survit à un reset logiciel / panic /
// watchdog, PAS à une coupure secteur (RAM RTC aléatoire ⇒ magic/CRC faux ⇒
// fail-closed). Même patron que heap_ring.cpp / oom_guard.cpp.
RTC_NOINIT_ATTR OutageRtcRecord sOutageRtc;

// Paramètres du détecteur de stabilité — depuis constants.h (une seule source).
constexpr PurgeStabilityParams kStabParams = {
    kPurgeSamplesPerMin, kPurgeWindowMin,
    kPurgeStablePhRange, kPurgeStableOrpRange,
    kPurgeRecloseRangePh, kPurgeRecloseRangeOrp,
};

// Cadence de rafraîchissement de l'heure vive en RAM RTC.
constexpr uint32_t kOutageRtcRefreshMs = 1000UL;

// Formatage NaN-safe : une mesure absente se lit "n/a", jamais "nan"/"0.00".
String fmtOrNa(float v, unsigned int decimals) {
  return isnan(v) ? String("n/a") : String(v, decimals);
}

// Plage min→max d'une voie du détecteur (journal de fin de purge).
String rangeOrNa(const PurgeChannelState& ch, unsigned int decimals) {
  const float lo = purgeChannelRawMin(ch);
  const float hi = purgeChannelRawMax(ch);
  if (isnan(lo) || isnan(hi)) return String("n/a");
  return String(lo, decimals) + "→" + String(hi, decimals);
}

OutageResetKind resetKindFromEsp(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_SW:       return OutageResetKind::kSoftware;
    case ESP_RST_PANIC:    return OutageResetKind::kPanic;
    case ESP_RST_INT_WDT:  return OutageResetKind::kIntWdt;
    case ESP_RST_TASK_WDT: return OutageResetKind::kTaskWdt;
    default:               return OutageResetKind::kOther;
  }
}

// Heure système valide (posée par rtcManager.applyToSystem() ou NTP).
bool wallClockValid(uint32_t& epochOut) {
  const time_t t = time(nullptr);
  if (t < kMinValidEpoch) return false;
  epochOut = (uint32_t)t;
  return true;
}

}  // namespace

// =============================================================================
// begin() — décision de dispense de la première purge (C3)
// =============================================================================
void MeasureValidityManager::begin() {
  // Copie locale de l'enregistrement RTC AVANT toute écriture (la décision
  // porte sur ce qu'a laissé le firmware précédent).
  OutageRtcRecord before = sOutageRtc;

  uint32_t epochNow = 0;
  const bool timeValid = wallClockValid(epochNow);

  OutageContinuityInputs in;
  in.record = &before;
  in.reset = resetKindFromEsp(esp_reset_reason());
  in.timeValid = timeValid;
  in.epochNow = epochNow;
  in.shortOutageS = kShortOutageS;
  in.waiverMinIntervalS = kOutageWaiverMinIntervalS;
  const OutageContinuityVerdict v = evaluateOutageContinuity(in);

  _waiverPending = v.waive;
  _bootOutageS = v.outageS;

  // Ré-initialisation de la mémoire si elle n'était pas intacte (power-on) ;
  // sinon on conserve lastWaiverEpoch (limite « une dispense par heure »).
  if (!outageRecordIntact(before)) {
    memset(&sOutageRtc, 0, sizeof(sOutageRtc));
  }
  if (v.waive && timeValid) {
    sOutageRtc.lastWaiverEpoch = epochNow;
  }
  sOutageRtc.waterPresent = 0;  // sera rafraîchi au premier update()
  outageRecordSeal(sOutageRtc);

  if (v.waive) {
    systemLogger.info("[STAB] Coupure " + String(v.outageS) +
                      " s (reset logiciel, eau présente avant) → purge de reprise DISPENSÉE");
  } else if (v.deny == OutageDeny::kMemory) {
    // Cas nominal du démarrage à froid : pas d'alarme, simple trace.
    systemLogger.info("[STAB] Démarrage à froid (mémoire RTC absente) → purge complète à la reprise d'eau");
  } else {
    const String dur = (v.outageS > 0 || v.deny == OutageDeny::kTooLong)
                           ? String(v.outageS) + " s"
                           : String("de durée inconnue");
    systemLogger.info("[STAB] Coupure " + dur + " (" + String(outageDenyLabel(v.deny)) +
                      ") → purge complète à la reprise d'eau");
  }
}

// =============================================================================
// update() — un tick de la machine d'état
// =============================================================================
void MeasureValidityManager::update() {
  // Collecte (loopTask uniquement — resolveWaterPresence() consommé en lecture
  // seule, jamais depuis un contexte async).
  const WaterPresence wp = filtration.resolveWaterPresence();
  const bool water = wp.waterPresent;
  const uint32_t now = (uint32_t)millis();

  // 1) Détecteur de stabilité : une lecture des BRUTS à la cadence EZO tant que
  //    l'eau est présente (purge en cours OU surveillance de re-fermeture).
  if (water && (now - _lastSampleMs) >= (uint32_t)kPhOrpSensorIntervalMs) {
    _lastSampleMs = now;
    purgeStabilityAddSample(_stab, kStabParams, sensors.getPhRaw(), sensors.getOrpRaw());
  }
  const PurgeStabilityVerdict sv = purgeStabilityEvaluate(_stab, kStabParams);

  // 2) Machine d'état pure. La dispense n'est présentée qu'au PREMIER tick
  //    (eau déjà présente au boot ⇒ front montant vu à ce tick) : un front
  //    ultérieur purge normalement.
  MeasureValidityInputs in;
  in.waterPresent = water;
  in.nowMs = now;
  in.stable = sv.stable;
  in.unstable = sv.unstable;
  in.purgeWaived = _waiverPending;
  in.floorMs = kPurgeFloorMs;
  in.capMs = kPurgeCapMs;
  measureValidityTick(_state, in);
  _waiverPending = false;

  // 3) Journal des transitions + remise à zéro du détecteur à chaque début de
  //    purge (chaque purge est jugée sur ses propres minutes).
  switch (_state.event) {
    case MeasureValidityEvent::kPurgeStarted:
      purgeStabilityReset(_stab);
      systemLogger.info("[STAB] Reprise de circulation — purge des mesures pH/ORP "
                        "(plancher " + String(kPurgeFloorMs / 60000UL) +
                        " min, fin sur stabilité, plafond " +
                        String(kPurgeCapMs / 60000UL) + " min) ; bruts : pH=" +
                        fmtOrNa(sensors.getPhRaw(), 3) + " ORP=" +
                        fmtOrNa(sensors.getOrpRaw(), 1) + " mV (eau stagnante)");
      break;
    case MeasureValidityEvent::kReclosed:
      // C1 : les bruts ont bougé de plus de 2× le seuil après l'ouverture.
      systemLogger.warning("[STAB] Re-purge n°" + String(_state.repurgeCount) +
                           " — mesures redevenues instables après ouverture (étendue pH " +
                           fmtOrNa(sv.phRange, 3) + " / ORP " + fmtOrNa(sv.orpRange, 1) +
                           " mV) : porte refermée jusqu'à nouvelle stabilité");
      purgeStabilityReset(_stab);
      break;
    case MeasureValidityEvent::kOpened:
      logPurgeEnd(now);
      // Le détecteur repart à vide : la surveillance de re-fermeture ne juge
      // que des minutes postérieures à l'ouverture.
      purgeStabilityReset(_stab);
      break;
    case MeasureValidityEvent::kWaterDrop:
    case MeasureValidityEvent::kNone:
    default:
      break;
  }

  // 4) Mémoire RTC de continuité (C3) : heure vive + eau présente, ~1 s.
  if ((now - _lastRtcRefreshMs) >= kOutageRtcRefreshMs) {
    _lastRtcRefreshMs = now;
    refreshRtcRecord(water);
  }

  // 5) Publication du verdict — section critique MINIMALE (copie de struct).
  MeasureValiditySnapshot snap;
  snap.waterPresent = water;
  snap.measuresValid = _state.valid;
  snap.circulationSettled = measureValidityCirculationSettled(_state, now, kPurgeFloorMs);
  snap.stabElapsedS = measureValidityElapsedS(_state, now);
  snap.lastPurgeDurationS = _state.lastPurgeDurationS;
  snap.lastPurgeEnd = static_cast<uint8_t>(_state.lastPurgeEnd);
  snap.purgeCount = _state.repurgeCount;
  portENTER_CRITICAL(&_mux);
  _cache = snap;
  portEXIT_CRITICAL(&_mux);
}

// Journal « [STAB] » de fin de purge : durée, min/max des bruts, motif. Au
// plafond (C2) : alerte MQTT `measures_purge_timeout` + warning.
void MeasureValidityManager::logPurgeEnd(uint32_t /*nowMs*/) {
  const uint32_t dur = _state.lastPurgeDurationS;
  const String durStr = String(dur / 60UL) + " min " + String(dur % 60UL) + " s";
  const String ranges = "pH " + rangeOrNa(_stab.ph, 3) + " ; ORP " +
                        rangeOrNa(_stab.orp, 1) + " mV";

  switch (_state.lastPurgeEnd) {
    case PurgeEnd::kWaived:
      systemLogger.info("[STAB] Purge dispensée (coupure " + String(_bootOutageS) +
                        " s) — mesures pH/ORP représentatives immédiatement");
      break;
    case PurgeEnd::kCap: {
      const String msg = "Purge des mesures ouverte au PLAFOND (" + durStr +
                         ") sans stabilité (" + ranges +
                         ") — sonde bruyante ou transitoire long ; régulation reprise, à surveiller";
      systemLogger.warning("[STAB] " + msg);
      mqttManager.publishAlert("measures_purge_timeout", msg);
      break;
    }
    case PurgeEnd::kRepurge:
      systemLogger.info("[STAB] Fin de re-purge n°" + String(_state.repurgeCount) +
                        " (" + durStr + ", " + ranges + ") — mesures de nouveau représentatives");
      break;
    case PurgeEnd::kStability:
    default:
      systemLogger.info("[STAB] Fin de purge par stabilité (" + durStr + ", " + ranges +
                        ") — mesures pH/ORP représentatives");
      break;
  }
}

// Rafraîchit l'enregistrement RTC (heure vive, eau présente) — UNIQUEMENT si
// l'heure système est valide : une heure fausse rendrait la durée de coupure
// incalculable au prochain boot (et donc la dispense refusée, fail-closed).
void MeasureValidityManager::refreshRtcRecord(bool water) {
  uint32_t epochNow = 0;
  if (!wallClockValid(epochNow)) return;
  sOutageRtc.liveEpoch = epochNow;
  sOutageRtc.waterPresent = water ? 1 : 0;
  outageRecordSeal(sOutageRtc);
}

// =============================================================================
// Accès au cache (loopTask / async_tcp)
// =============================================================================
MeasureValiditySnapshot MeasureValidityManager::getSnapshot() const {
  portENTER_CRITICAL(&_mux);
  const MeasureValiditySnapshot s = _cache;
  portEXIT_CRITICAL(&_mux);
  return s;
}

bool MeasureValidityManager::waterPresent() const {
  portENTER_CRITICAL(&_mux);
  const bool v = _cache.waterPresent;
  portEXIT_CRITICAL(&_mux);
  return v;
}

bool MeasureValidityManager::measuresValid() const {
  portENTER_CRITICAL(&_mux);
  const bool v = _cache.measuresValid;
  portEXIT_CRITICAL(&_mux);
  return v;
}

bool MeasureValidityManager::isCirculationSettled() const {
  portENTER_CRITICAL(&_mux);
  const bool v = _cache.circulationSettled;
  portEXIT_CRITICAL(&_mux);
  return v;
}
