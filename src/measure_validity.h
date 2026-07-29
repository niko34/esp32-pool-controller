#ifndef MEASURE_VALIDITY_H
#define MEASURE_VALIDITY_H

// =============================================================================
// measure_validity — Coquille impure de la validité des mesures pH/ORP
// (feature-061)
// =============================================================================
// Collecte les entrées (resolveWaterPresence(), millis(), délai de purge
// mqttCfg.stabilizationDelayMin), détecte le front montant d'eau, appelle la
// logique pure (measure_validity_logic) et met en cache le verdict.
//
// Le cache est lu depuis DEUX contextes : loopTask (history.recordDataPoint,
// push WS) et async_tcp (/data) → snapshot cohérent sous spinlock portMUX
// (pattern _externalMux de filtration : section critique minimale, jamais de
// mutex bloquant en contexte async). Les consommateurs lisent le CACHE, jamais
// resolveWaterPresence() directement.
//
// BOOT : eau présente au premier update() → front montant armé au boot → délai
// de purge complet appliqué (conservateur : on ignore depuis quand l'eau
// circule ni ce qui a stagné pendant l'extinction).
// =============================================================================

#include <Arduino.h>
#include "measure_validity_logic.h"

class MeasureValidityManager {
public:
  // À appeler depuis la loop (avant history.update() / push WS) : détecte le
  // front montant d'eau, évalue la logique pure et met le verdict en cache.
  void update();

  // Snapshot COHÉRENT des trois sorties (une seule section critique) — à
  // préférer quand plusieurs champs sont sérialisés dans un même payload.
  void getSnapshot(bool& waterPresent, bool& measuresValid,
                   uint32_t& stabilizationRemainingS) const;

  // Getters unitaires (chacun sous spinlock — cohérence non garantie ENTRE eux).
  bool waterPresent() const;
  bool measuresValid() const;
  uint32_t stabilizationRemainingS() const;

private:
  // --- État interne (loopTask uniquement) ---
  bool _lastWaterPresent = false;  // dernier état d'eau vu (front montant au boot si eau présente)
  uint32_t _waterRiseMs = 0;       // millis() du dernier front montant ; 0 = jamais (sentinelle)
  bool _wasStabilizing = false;    // purge en cours au tick précédent (log de fin)

  // --- Cache des sorties (partagé loopTask / async_tcp) ---
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  bool _cachedWaterPresent = false;
  bool _cachedValid = false;             // faux au boot tant qu'update() n'a pas tourné (conservateur)
  uint32_t _cachedRemainingS = 0;
};

extern MeasureValidityManager measureValidity;

#endif  // MEASURE_VALIDITY_H
