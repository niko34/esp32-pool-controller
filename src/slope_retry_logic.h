#ifndef SLOPE_RETRY_LOGIC_H
#define SLOPE_RETRY_LOGIC_H

// =============================================================================
// slope_retry_logic — Retentative de la requête Slope,? de BOOT, logique PURE
// (bug-slope-boot-perdue-et-data-incomplet, v2.21.2)
// =============================================================================
// Cause racine : la requête Slope,? enfilée au boot (sensors.cpp, begin())
// pouvait échouer EN SILENCE (échec isolé, streak=1 < kEzoBusFailMaxConsecutive
// donc jamais journalisé) et n'était JAMAIS retentée avant le cycle 24 h
// (kPhSlopeQueryIntervalMs exige _phSlopeQueriedMs != 0). Résultat : ~20 min
// et plus de « Pente non disponible » après chaque reboot.
//
// Ce module décide si update() doit ré-enfiler une Slope,? de boot. Bornes de
// sécurité (visa pool-chemistry GO SOUS CONDITIONS) :
//   - C1 : borne DURE de maxAttempts transactions (compteur incrémenté par
//     l'appelant à CHAQUE tentative, même si l'enqueue échoue, jamais remis à
//     zéro) — chaque Slope,? bloque loopTask ~900 ms (I²C synchrone).
//   - C2 : jamais de retry sans module EZO pH détecté (ezoDetected), sinon
//     5 × 900 ms de blocage loopTask pour rien sur un bus muet.
//
// CONTRAINTE (style ADR-0017, comme src/ph_offset.h) : pas d'Arduino.h, pas de
// FreeRTOS, pas de String — en-têtes C uniquement. Testable en natif.
// =============================================================================

#include <stdint.h>

// Décide si une retentative de la requête Slope,? de boot doit être enfilée.
//
// Retourne true SEULEMENT si TOUTES les conditions sont réunies :
//   - queriedMs == 0     : aucune Slope,? n'a JAMAIS réussi depuis le boot
//                          (dès la 1ʳᵉ réussite, le cycle 24 h prend le relais) ;
//   - !pending           : pas de query déjà en file (anti-doublon) ;
//   - ezoDetected        : module EZO pH détecté au boot (gate C2) ;
//   - attempts < maxAttempts : borne dure de transactions (gate C1) ;
//   - cadencement        : première tentative (attempts == 0, immédiate — le
//                          module vient probablement de finir son init), OU
//                          nowMs − lastAttemptMs >= intervalMs.
//
// Garde anti-underflow (encodée ICI, cf. bug historique du cycle 24 h dans
// update()) : si nowMs < lastAttemptMs — possible quand le timestamp a été posé
// APRÈS l'échantillonnage de nowMs, ou au wrap de millis() (~49 j, hors fenêtre
// de boot) — la soustraction non signée donnerait ~4,3 milliards et validerait
// l'intervalle à tort : on refuse et on attend le prochain cycle.
//
// @param queriedMs     millis() de la dernière Slope,? RÉUSSIE (0 = jamais)
// @param pending       true si une query est déjà en file (anti-doublon)
// @param ezoDetected   true si l'EZO pH a répondu au readInfo() de boot
// @param attempts      Nombre de retentatives déjà décomptées (jamais remis à 0)
// @param maxAttempts   Borne dure de retentatives (kPhSlopeBootRetryMaxAttempts)
// @param lastAttemptMs millis() de la dernière retentative (0 = aucune)
// @param nowMs         millis() courant
// @param intervalMs    Intervalle mini entre retentatives (kPhSlopeBootRetryIntervalMs)
// @return              true si l'appelant doit enfiler une Slope,? maintenant
inline bool shouldRetryBootSlope(uint32_t queriedMs, bool pending, bool ezoDetected,
                                 uint8_t attempts, uint8_t maxAttempts,
                                 uint32_t lastAttemptMs, uint32_t nowMs,
                                 uint32_t intervalMs) {
  if (queriedMs != 0) return false;       // Une pente a déjà été lue : rien à faire
  if (pending) return false;              // Query déjà en file
  if (!ezoDetected) return false;         // Gate C2 : module absent → jamais de retry
  if (attempts >= maxAttempts) return false;  // Gate C1 : borne dure atteinte
  if (attempts == 0) return true;         // Première retentative : immédiate
  if (nowMs < lastAttemptMs) return false;    // Garde anti-underflow (wrap-safe)
  return (nowMs - lastAttemptMs) >= intervalMs;
}

#endif // SLOPE_RETRY_LOGIC_H
