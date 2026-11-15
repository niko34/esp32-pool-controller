#include "ota_recovery_marker.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <atomic>
#include <esp_task_wdt.h>

#include "constants.h"
#include "fs_guard.h"
#include "logger.h"

namespace {

// Copie RAM du marqueur + sa lisibilité. Écrits depuis `async_tcp` (pose/levée)
// et au setup ; lus par la loop. Atomiques relaxés : aucune donnée n'est publiée
// à travers eux, une lecture périmée ne coûte qu'une réévaluation de plus.
std::atomic<bool> sMarkerValue{false};
std::atomic<bool> sMarkerReadable{false};
// « À réévaluer » : posé par toute transition, consommé par otaRecoveryDrain().
std::atomic<bool> sDirty{true};
// Résultat de la dernière évaluation, lu O(1) par les routes de page.
std::atomic<uint8_t> sReason{static_cast<uint8_t>(OtaRecoveryReason::kNone)};
// Dernier motif JOURNALISÉ (loop uniquement) et date de la dernière sonde refusée.
OtaRecoveryReason sLoggedReason = OtaRecoveryReason::kNone;
uint32_t sLastProbeAttemptMs = 0;
bool sProbeRetryPending = false;

// Écriture NVS. true si la valeur est PERSISTÉE. Watchdog nourri autour : une
// écriture flash peut durer plusieurs ms, et `async_tcp` est abonnée au WDT.
bool writeMarker(bool value) {
  esp_task_wdt_reset();
  Preferences prefs;
  bool ok = false;
  if (prefs.begin(kOtaRecoveryNvsNamespace, false)) {
    ok = (prefs.putBool(kOtaRecoveryNvsKey, value) == sizeof(uint8_t));
    prefs.end();
  }
  esp_task_wdt_reset();
  return ok;
}

}  // namespace

void otaRecoveryInit() {
  Preferences prefs;
  // begin en lecture/écriture : en lecture seule, un namespace encore inexistant
  // (cas NOMINAL d'un appareil jamais mis à jour) ferait échouer begin() et
  // serait pris pour une NVS illisible.
  if (prefs.begin(kOtaRecoveryNvsNamespace, false)) {
    sMarkerValue.store(prefs.getBool(kOtaRecoveryNvsKey, false), std::memory_order_relaxed);
    sMarkerReadable.store(true, std::memory_order_relaxed);
    prefs.end();
  } else {
    sMarkerReadable.store(false, std::memory_order_relaxed);
    systemLogger.warning("Marqueur de secours OTA illisible (NVS) — ignoré");
  }
  sDirty.store(true, std::memory_order_relaxed);
}

void otaRecoveryOnUpdatePhase(bool isFilesystemUpdate, OtaRecoveryUpdatePhase phase) {
  const OtaRecoveryMarkerAction action = decideOtaRecoveryMarkerAction(isFilesystemUpdate, phase);
  if (action == OtaRecoveryMarkerAction::kNone) return;

  const bool target = (action == OtaRecoveryMarkerAction::kArm);
  // La copie RAM suit ce que la NVS contient RÉELLEMENT : une pose ratée est
  // ignorée (jamais de faux secours), une levée ratée laisse le marqueur posé.
  if (writeMarker(target)) {
    sMarkerValue.store(target, std::memory_order_relaxed);
    sMarkerReadable.store(true, std::memory_order_relaxed);
  }
  sDirty.store(true, std::memory_order_relaxed);
}

void otaRecoveryNoteTransition() {
  sDirty.store(true, std::memory_order_relaxed);
}

void otaRecoveryDrain() {
  if (!sDirty.load(std::memory_order_relaxed)) {
    // Admission refusée à la sonde précédente : on retente, bornée dans le temps.
    if (!sProbeRetryPending || (millis() - sLastProbeAttemptMs) < kOtaRecoveryReprobeMs) return;
  }
  sDirty.store(false, std::memory_order_relaxed);

  OtaRecoveryInputs in{};
  in.fsMounted = (LittleFS.mountpoint() != nullptr);
  in.markerReadable = sMarkerReadable.load(std::memory_order_relaxed);
  in.fsUpdateInFlight = sMarkerValue.load(std::memory_order_relaxed);
  in.indexProbed = false;
  in.indexPresent = false;
  sProbeRetryPending = false;

  // Sonde d'index seulement si elle peut changer la décision : FS monté et
  // marqueur non décisif (le marqueur prime, cf. decideOtaRecoveryReason).
  if (in.fsMounted && !(in.markerReadable && in.fsUpdateInFlight)) {
    bool probed = false;
    in.indexPresent = fsPathResolvableIfHeapAllows(&LittleFS, kOtaRecoveryProbePath,
                                                   kStaticFileMinAllocBytes, &probed);
    in.indexProbed = probed;
    if (!probed) {
      // Famine mémoire : ne PAS armer (repli), mais retenter plus tard.
      sProbeRetryPending = true;
      sLastProbeAttemptMs = millis();
    }
  }

  const OtaRecoveryReason reason = decideOtaRecoveryReason(in);
  sReason.store(static_cast<uint8_t>(reason), std::memory_order_relaxed);

  if (reason != sLoggedReason) {
    char ligne[96];
    if (reason == OtaRecoveryReason::kNone) {
      systemLogger.info("Page de secours OTA levée : interface web de nouveau servie");
    } else {
      snprintf(ligne, sizeof(ligne), "Page de secours OTA ACTIVE : %s",
               otaRecoveryReasonLabel(reason));
      systemLogger.warning(ligne);
    }
    sLoggedReason = reason;
  }
}

OtaRecoveryReason otaRecoveryReason() {
  return static_cast<OtaRecoveryReason>(sReason.load(std::memory_order_relaxed));
}

bool otaRecoveryActive() {
  return otaRecoveryReason() != OtaRecoveryReason::kNone;
}
