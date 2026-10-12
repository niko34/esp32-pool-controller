// =============================================================================
// measure_validity — Coquille impure de la validité des mesures pH/ORP
// (feature-061 ; purge ADAPTATIVE par la feature-091, v2.34.0 ; RETRAIT de
// l'analyse de stabilité par la feature-139, v2.62.0)
// — voir measure_validity.h
// =============================================================================

#include "measure_validity.h"

#include <esp_system.h>  // esp_reset_reason
#include <time.h>

#include "config.h"
#include "constants.h"
#include "filtration.h"
#include "logger.h"

MeasureValidityManager measureValidity;

namespace {

// Mémoire RTC de continuité (C3) : survit à un reset logiciel / panic /
// watchdog, PAS à une coupure secteur (RAM RTC aléatoire ⇒ magic/CRC faux ⇒
// fail-closed). Même patron que heap_ring.cpp / oom_guard.cpp.
RTC_NOINIT_ATTR OutageRtcRecord sOutageRtc;

// Cadence de rafraîchissement de l'heure vive en RAM RTC.
constexpr uint32_t kOutageRtcRefreshMs = 1000UL;

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
  in.graceS = kWaterStopGraceS;
  const OutageContinuityVerdict v = evaluateOutageContinuity(in);

  // feature-158 (condition 13 du gate) : mémorise le verdict déjà calculé
  // pour qu'un consommateur (filtration.begin()) puisse le LIRE sans qu'un
  // second calcul n'existe nulle part. Le drapeau est posé ICI, dans begin(),
  // AVANT tout appel à filtration.begin() (voir l'ordre commenté des deux
  // côtés dans main.cpp).
  _bootVerdict = v;
  _bootVerdictReady = true;

  // Ré-initialisation de la mémoire si elle n'était pas intacte (power-on).
  if (!outageRecordIntact(before)) {
    memset(&sOutageRtc, 0, sizeof(sOutageRtc));
  }
  sOutageRtc.waterPresent = 0;  // sera rafraîchi au premier update()
  outageRecordSeal(sOutageRtc);

  // ARBITRAGE 1 (feature-139) : UN SEUL CHEMIN DE DISPENSE. La mémoire RTC ne
  // fait que PORTER l'horodatage de l'arrêt à travers le reboot, dans le
  // référentiel millis() post-boot (soustraction non signée, wrap-safe) — la
  // décision « court ou non » est ensuite prise par measureValidityTick(),
  // exactement comme pour un arrêt sans reboot.
  if (v.waive) {
    _state.haveWaterStop = true;
    _state.stopGraceExceeded = false;
    _state.waterStopMs = (uint32_t)millis() - (v.outageS * 1000UL);
    systemLogger.info("[STAB] Arrêt d'eau " + String(v.outageS) +
                      " s (reset logiciel, eau présente avant) → purge de reprise DISPENSÉE");
  } else if (v.deny == OutageDeny::kMemory) {
    // Cas nominal du démarrage à froid : pas d'alarme, simple trace.
    systemLogger.info("[STAB] Démarrage à froid (mémoire RTC absente) → purge complète à la reprise d'eau");
  } else {
    const String dur = (v.outageS > 0 || v.deny == OutageDeny::kTooLong)
                           ? String(v.outageS) + " s"
                           : String("de durée inconnue");
    systemLogger.info("[STAB] Arrêt d'eau " + dur + " (" + String(outageDenyLabel(v.deny)) +
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

  // Capture AVANT tick (feature-139, C5) : pour journaliser, à la reprise,
  // combien de temps l'eau a réellement été absente — dispense ou non.
  const bool wasPresent = _state.waterPresent;
  const uint32_t prevWaterStopMs = _state.waterStopMs;
  const bool prevHaveWaterStop = _state.haveWaterStop;

  // Machine d'état pure.
  MeasureValidityInputs in;
  in.waterPresent = water;
  in.nowMs = now;
  in.floorMs = kPurgeFloorMs;
  in.graceMs = kWaterStopGraceS * 1000UL;
  measureValidityTick(_state, in);

  // Journal des transitions.
  switch (_state.event) {
    case MeasureValidityEvent::kPurgeStarted:
      systemLogger.info("[STAB] Reprise de circulation — purge des mesures pH/ORP "
                        "(plancher " + String(kPurgeFloorMs / 60000UL) + " min)");
      break;
    case MeasureValidityEvent::kOpened:
      logPurgeEnd(now);
      break;
    case MeasureValidityEvent::kWaterDrop:
    case MeasureValidityEvent::kNone:
    default:
      break;
  }

  // feature-139 (arbitrage 3, C5) : la grandeur qui manquait pour réviser
  // kWaterStopGraceS sur des données réelles. Journalisée à CHAQUE reprise
  // d'eau datable, dispense ou non.
  if (!wasPresent && water && prevHaveWaterStop) {
    const uint32_t stopS = (now - prevWaterStopMs) / 1000UL;
    systemLogger.info("[STAB] Reprise après arrêt de " + String(stopS) + " s — attente " +
                      (_state.waivedCycle ? "dispensée" : "complète"));
  }

  // Mémoire RTC de continuité (C3) : heure vive + eau présente, ~1 s.
  if ((now - _lastRtcRefreshMs) >= kOutageRtcRefreshMs) {
    _lastRtcRefreshMs = now;
    refreshRtcRecord(water);
  }

  // Publication du verdict — section critique MINIMALE (copie de struct).
  MeasureValiditySnapshot snap;
  snap.waterPresent = water;
  snap.measuresValid = _state.valid;
  snap.circulationSettled = measureValidityCirculationSettled(_state, now, kPurgeFloorMs);
  snap.stabElapsedS = measureValidityElapsedS(_state, now);
  snap.lastPurgeDurationS = _state.lastPurgeDurationS;
  snap.lastPurgeEnd = static_cast<uint8_t>(_state.lastPurgeEnd);
  portENTER_CRITICAL(&_mux);
  _cache = snap;
  portEXIT_CRITICAL(&_mux);
}

// Journal « [STAB] » de fin de purge : durée et motif.
void MeasureValidityManager::logPurgeEnd(uint32_t /*nowMs*/) {
  const uint32_t dur = _state.lastPurgeDurationS;
  const String durStr = String(dur / 60UL) + " min " + String(dur % 60UL) + " s";

  switch (_state.lastPurgeEnd) {
    case PurgeEnd::kWaived:
      // B2 (gate pool-chemistry, 2e passage) : _bootOutageS n'était posé qu'au
      // boot (begin()) et était donc FAUX pour toute dispense survenant en
      // cours de marche (arrêt d'eau court sans reboot) — il affichait
      // contradictoirement "arrêt 0 s" juste à côté du log "[STAB] Reprise
      // après arrêt de N s" (ci-dessous, update()) qui porte la durée MESURÉE.
      // Pas de nombre ici plutôt qu'un nombre faux : la durée exacte est
      // journalisée juste après par le log "[STAB] Reprise après arrêt de N s"
      // (même tick, update() ci-dessous).
      systemLogger.info("[STAB] Purge dispensée — mesures pH/ORP représentatives immédiatement");
      break;
    case PurgeEnd::kStability:
    default:
      systemLogger.info("[STAB] Fin de purge (" + durStr + ") — mesures pH/ORP représentatives");
      break;
  }
}

// Rafraîchit l'enregistrement RTC (heure vive, eau présente) — UNIQUEMENT si
// l'heure système est valide : une heure fausse rendrait la durée d'arrêt
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

// feature-158 (condition 13) : pas de section critique — appelée uniquement
// pendant setup(), avant que loopTask/async_tcp n'existent (même contexte que
// begin()), donc sans concurrence possible à ce stade.
bool MeasureValidityManager::bootContinuityVerdict(OutageContinuityVerdict& out) const {
  if (!_bootVerdictReady) return false;
  out = _bootVerdict;
  return true;
}
