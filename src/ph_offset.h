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
// continue de travailler sur le signal BRUT, et un changement d'offset est donc
// pris en compte IMMÉDIATEMENT (pas dilué sur la fenêtre de lissage).
// NB (feature-152) : cette phrase citait « latch EMI, seuils de rejet de pics et
// dispersion » — ces trois mécanismes N'EXISTENT PLUS. Le point d'application de
// l'offset, lui, est inchangé : c'est tout ce que ce paragraphe énonce.
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
// de ±0,50 pH, l'écart ne s'explique plus par l'implantation, et l'écart entre
// pH régulé et pH réel deviendrait dangereux.
// ⚠️ FORMULATION CORRIGÉE (feature-155) : cette phrase disait « c'est la sonde
// ou la calibration EZO qui est en cause ». ÇA MET LA SONDE EN CAUSE, et c'est
// très exactement le travers de l'incident du 2026-08-25 — une sonde SAINE
// (pente 101/100, zéro −1,03) déclarée à remplacer alors que la cause était un
// décalage EXTERNE (courants de fuite de la pompe de filtration). Un écart de
// plus de 0,5 pH a d'AUTRES causes au moins aussi probables : test de référence
// périmé, lecture à la mauvaise échelle, étalonnage à refaire. Ne pas désigner
// un coupable qu'on n'a pas identifié — ni ici, ni dans un message dérivé.
// Le jumeau de cette phrase (web_routes_config.cpp) a été corrigé dans le même
// lot ; celui-ci avait été signalé par l'implémenteur comme hors périmètre.
inline bool isPhOffsetValid(float offset) {
  if (isnan(offset)) return false;
  return (offset >= kPhOffsetMin) && (offset <= kPhOffsetMax);
}

// =============================================================================
// feature-156 — HORODATAGE DE JUSTESSE (correction manuelle vs étalonnage)
// =============================================================================
// Un étalonnage EZO modifie le zéro ET la pente de la sonde : une correction
// manuelle posée AVANT ce dernier étalonnage compensait un état de la sonde
// qui n'existe plus — elle est CADUQUE. `seq` (pas `epoch`) fait foi pour cet
// ordre : voir config.h (champs phOffset*/phCal*) pour la justification
// complète (indépendance à l'horloge et au redémarrage).

// True ssi une correction non nulle en vigueur est ANTÉRIEURE au dernier
// étalonnage réussi — donc caduque. `offset` est la valeur APPLIQUÉE
// (mqttCfg.phOffset), `offsetSeq`/`calSeq` les séquences des deux écrivains
// uniques (config.h). Une correction nulle (0,00 = inerte) n'est jamais
// caduque : il n'y a rien à annuler. Deux séquences ÉGALES (aucun des deux
// événements ne s'est jamais produit après l'autre, y compris 0 == 0 au
// premier démarrage) ne sont PAS une préséance de l'étalonnage : faux.
inline bool isPhOffsetStale(float offset, uint32_t offsetSeq, uint32_t calSeq) {
  return (offset != 0) && (offsetSeq < calSeq);
}

// Prochaine séquence « justesse », toujours strictement supérieure aux deux
// séquences passées en argument — insensible à l'ordre des arguments
// (monotone quel que soit celui des deux événements qui vient de se produire).
inline uint32_t nextJustnessSeq(uint32_t a, uint32_t b) {
  return (a > b ? a : b) + 1;
}

#endif // PH_OFFSET_H
