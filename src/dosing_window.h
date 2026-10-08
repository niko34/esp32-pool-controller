#ifndef DOSING_WINDOW_H
#define DOSING_WINDOW_H

// =============================================================================
// dosing_window — Coquille impure de la survie du budget horaire de dosage
// (feature-136, gate pool-chemistry du 2026-10-08)
// =============================================================================
// Collecte les entrées (RTC_NOINIT_ATTR, NVS `pool-dose`, esp_reset_reason(),
// esp_rtc_get_time_us()), délègue TOUTES les décisions à dosing_window_logic.h
// (module pur, testable en natif) et journalise. Patron measure_validity.* /
// time_to_target.* (ADR-0017).
//
// BOOT : begin() DOIT être appelé APRÈS rtcManager.applyToSystem() (l'heure
// système doit être valide pour la voie T1), AVANT PumpController::begin()
// (qui consomme restored() dans applyRestoredDosingWindow()).
// =============================================================================

#include <Arduino.h>
#include "dosing_window_logic.h"

// Entrées IMPURES fournies par pump_controller à CHAQUE tick, pour UNE voie
// (0 = pH, 1 = ORP) — copie directe de l'état courant de DosingState et des
// champs voisins (_mixingEndMs, _dosingCycleHistory). Aucune décision ici.
struct DosingWindowChannelSnapshot {
  uint32_t usedMs = 0;
  uint32_t windowStart = 0;        // DosingState.windowStart (0 = jamais ouverte)
  uint16_t cyclesToday = 0;        // DosingState.cyclesToday
  uint32_t cyclesDayStart = 0;     // DosingState.cyclesDayStart (0 = jamais ouvert)
  uint32_t mixingEndMs = 0;        // _mixingEndMs[ch] (0 = inactif)
  bool pumpOnNow = false;          // pompe énergisée CE tour (pumpDuty[ch] > 0)
  const uint32_t* ring = nullptr;  // _dosingCycleHistory[ch] — kDosingCycleHistorySize entrées
  uint32_t ringIdx = 0;            // _dosingCycleHistoryIdx[ch]
};

struct DosingWindowSnapshot {
  DosingWindowChannelSnapshot ch[2];  // [0] = pH, [1] = ORP
};

// État restauré d'UNE voie, prêt à appliquer tel quel à DosingState (+ ring).
struct DosingWindowRestoredChannel {
  uint32_t usedMs = 0;
  uint32_t windowStart = 0;
  uint16_t cyclesToday = 0;
  uint32_t cyclesDayStart = 0;
  uint32_t mixingEndMs = 0;  // à affecter à _mixingEndMs[ch]
  // Ring restauré (domaine millis() COURANT) — 0 = slot vide. T2 UNIQUEMENT
  // (recommandation C9 : pas de ring en NVS) ; tous à 0 si T2 indisponible.
  uint32_t ring[kDosingCycleHistorySize] = {0};
  uint32_t ringIdx = 0;
  DosingWindowSource source = DosingWindowSource::kFailClosed;
};

class DosingWindowManager {
 public:
  // Lit T2 (RAM RTC) + T1 (NVS), résout chaque voie (dosing_window_logic.h) et
  // journalise CE QUI a été restauré et SA SOURCE (C11). À appeler une seule
  // fois, APRÈS rtcManager.applyToSystem(), AVANT PumpController::begin().
  void begin();

  // L'état restauré par begin(), à consommer par
  // PumpControllerClass::applyRestoredDosingWindow(). ch invalide → voie 0.
  const DosingWindowRestoredChannel& restored(int ch) const {
    return _restored[(ch == 1) ? 1 : 0];
  }

  // Hook 1/3 (update(), inconditionnel, bon marché) : rafraîchit T2 (RAM RTC).
  // Cadencé en interne (kDosingWindowRtcRefreshMs, patron measure_validity).
  void refreshRtc(uint32_t nowMs, const DosingWindowSnapshot& snap);

  // Hooks 2/3 + 3/3 (update()) : écrit T1 (NVS) si dû — cadence dédiée 30 s
  // PENDANT pompe-ON (`anyPumpOnNow`) + événement ponctuel (`event` = true à
  // la transition pompe-ON ↔ pompe-OFF, posée par l'appelant). Prend
  // `configMutex` EN INTERNE (jamais pendant que l'appelant le tient) ; à
  // appeler UNIQUEMENT depuis loopTask (jamais un callback AsyncWebServer, ni
  // MQTT).
  // Renvoie false si l'écriture n'a pas pu être TENTÉE (horloge invalide,
  // timeout mutex) : l'appelant NE DOIT PAS effacer `event` dans ce cas
  // (retentative au tour suivant). true si rien n'était dû ce tour-ci OU si
  // l'écriture a été tentée (y compris en cas d'échec d'ouverture NVS).
  bool persistNvsIfDue(uint32_t nowMs, const DosingWindowSnapshot& snap,
                       bool anyPumpOnNow, bool event);

 private:
  DosingWindowRestoredChannel _restored[2];
  uint32_t _lastRtcRefreshMs = 0;
  uint32_t _lastNvsSaveMs = 0;
};

extern DosingWindowManager dosingWindow;

#endif  // DOSING_WINDOW_H
