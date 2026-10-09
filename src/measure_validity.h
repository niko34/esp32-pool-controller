#ifndef MEASURE_VALIDITY_H
#define MEASURE_VALIDITY_H

// =============================================================================
// measure_validity — Coquille impure de la validité des mesures pH/ORP
// (feature-061 ; purge ADAPTATIVE par la feature-091, v2.34.0 ; RETRAIT de
// l'analyse de stabilité par la feature-139, v2.62.0)
// =============================================================================
// Collecte les entrées (resolveWaterPresence(), millis(), mémoire RTC de
// continuité), appelle la logique pure (measure_validity_logic) et met en
// cache le verdict.
//
// Le cache est lu depuis DEUX contextes : loopTask (history.recordDataPoint,
// push WS, pump_controller) et async_tcp (/data) → snapshot cohérent sous
// spinlock portMUX (pattern _externalMux de filtration : section critique
// minimale, jamais de mutex bloquant en contexte async). Les consommateurs
// lisent le CACHE, jamais resolveWaterPresence() directement.
//
// BOOT : eau présente au premier update() → front montant vu au boot → purge
// COMPLÈTE (on ignore ce qui a stagné pendant l'extinction)… SAUF si begin() a
// établi qu'il s'agit d'un arrêt d'eau COURT (STRICTEMENT < kWaterStopGraceS —
// à durée égale, c'est un arrêt LONG, voir la frontière commentée dans
// measure_validity_logic.{h,cpp} ; reset logiciel, eau présente avant, heure
// valide, mémoire RTC intacte — C3 pool-chemistry) : la purge du premier front
// est alors DISPENSÉE. begin() DOIT
// être appelé APRÈS rtcManager.applyToSystem() (l'heure système doit être
// valide pour mesurer l'arrêt), et AVANT le premier update().
// =============================================================================

#include <Arduino.h>
#include "measure_validity_logic.h"

// Instantané COHÉRENT des sorties (une seule section critique) — sérialisé tel
// quel dans /data et le WebSocket (parité obligatoire, cf. ws_manager.cpp).
struct MeasureValiditySnapshot {
  bool waterPresent;             // eau présente (source unique feature-056, en cache)
  bool measuresValid;            // porte OUVERTE : mesures représentatives du bassin
  bool circulationSettled;       // C4 : eau présente depuis ≥ kPurgeFloorMs (ou cycle dispensé)
  uint32_t stabElapsedS;         // secondes écoulées dans la purge courante (0 hors purge)
  uint32_t lastPurgeDurationS;   // durée de la dernière purge terminée (s)
  uint8_t lastPurgeEnd;          // PurgeEnd : 0 aucune / 1 stabilité / 3 dispensée (2 et 4 retirés v2.62)
};

class MeasureValidityManager {
public:
  // Décision de dispense de la première purge (C3) : lit esp_reset_reason() et
  // la mémoire RTC, journalise « arrêt N s → purge dispensée / complète ».
  // APRÈS rtcManager.applyToSystem(), AVANT le premier update().
  void begin();

  // À appeler depuis la loop (avant history.update() / push WS) : fait avancer
  // la machine d'état pure, rafraîchit la mémoire RTC (~1 s) et met le verdict
  // en cache.
  void update();

  // Snapshot cohérent (une seule section critique).
  MeasureValiditySnapshot getSnapshot() const;

  // Getters unitaires (chacun sous spinlock — cohérence non garantie ENTRE eux).
  bool waterPresent() const;
  bool measuresValid() const;
  // C4 pool-chemistry : autorisation de la branche PROGRAMMÉE (aveugle) —
  // l'eau de la conduite a été renouvelée (≥ kPurgeFloorMs de circulation, ou
  // cycle dispensé). Ne dit RIEN de la validité des mesures.
  bool isCirculationSettled() const;

private:
  void logPurgeEnd(uint32_t nowMs);
  void refreshRtcRecord(bool water);

  // --- État interne (loopTask uniquement) ---
  MeasureValidityState _state = {};       // machine d'état pure (zéro = boot)
  uint32_t _lastRtcRefreshMs = 0;         // cadence de rafraîchissement RTC

  // --- Cache des sorties (partagé loopTask / async_tcp) ---
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  MeasureValiditySnapshot _cache = {};    // measuresValid faux au boot (conservateur)
};

extern MeasureValidityManager measureValidity;

#endif  // MEASURE_VALIDITY_H
