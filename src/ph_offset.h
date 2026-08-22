#ifndef PH_OFFSET_H
#define PH_OFFSET_H

// =============================================================================
// ph_offset — Offset logiciel de mesure pH, logique PURE (feature-067)
// =============================================================================
// Correction utilisateur du décalage d'implantation de la sonde pH :
//   pH corrigé = pH brut EZO + offset
// La calibration électrochimique du module Atlas EZO n'est JAMAIS touchée.
//
// Point d'application (décision architect) : EN SORTIE des accesseurs de
// SensorManager, jamais à l'ingestion du filtre. Le filtre pH (src/sensor_filter)
// continue de travailler sur le signal BRUT : latch EMI, seuils de rejet de pics
// et dispersion restent inchangés, et un changement d'offset est pris en compte
// IMMÉDIATEMENT (pas dilué sur la fenêtre de lissage).
//
// CONTRAINTE (style ADR-0017, comme dosing_logic / measure_validity_logic) :
// pas d'Arduino.h, pas de FreeRTOS, pas de String — en-têtes C uniquement, plus
// constants.h qui est self-contained (<stdint.h>/<time.h>) et porte les bornes
// (source unique des seuils, convention projet). Testable en natif.
// =============================================================================

#include <stdint.h>
#include <math.h>

#include "constants.h"  // kPhOffsetMin / kPhOffsetMax (self-contained, sans Arduino)

// Applique l'offset utilisateur à une mesure pH BRUTE.
//
// CONTRAT — NaN in → NaN out : l'offset ne doit JAMAIS fabriquer une mesure à
// partir de rien (mesure indisponible, capteur stale, filtre non amorcé), sinon
// il lèverait les gardes `isnan` qui protègent le dosage.
// Un offset lui-même NaN est neutralisé (retour du brut) : fail-safe.
//
// @param raw    Mesure pH brute (NaN si indisponible)
// @param offset Offset utilisateur en unités pH (0.0 = neutre)
// @return       Mesure corrigée, ou NaN si `raw` est NaN
inline float applyPhOffset(float raw, float offset) {
  if (isnan(raw)) return raw;
  if (isnan(offset)) return raw;
  return raw + offset;
}

// True si l'offset est exploitable : numérique ET dans la plage autorisée
// [kPhOffsetMin, kPhOffsetMax]. Bornes arbitrées avec pool-chemistry : au-delà
// de ±0,50 pH, c'est la sonde ou la calibration EZO qui est en cause, pas
// l'implantation — et l'écart entre pH régulé et pH réel deviendrait dangereux.
inline bool isPhOffsetValid(float offset) {
  if (isnan(offset)) return false;
  return (offset >= kPhOffsetMin) && (offset <= kPhOffsetMax);
}

#endif // PH_OFFSET_H
