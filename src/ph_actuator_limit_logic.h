#ifndef PH_ACTUATOR_LIMIT_LOGIC_H
#define PH_ACTUATOR_LIMIT_LOGIC_H

// =============================================================================
// ph_actuator_limit_logic — SOUS-DIMENSIONNEMENT DE L'ACTIONNEUR pH, logique PURE
// =============================================================================
// feature-086 (points 1-2) — patron ADR-0017 : pas d'Arduino.h, pas de millis(),
// pas de NVS ; l'anneau relu, la date du jour et le débit soutenable sont des
// PARAMÈTRES. Compilable en natif, seule dépendance : constants.h (seuils) et
// ph_persist_logic.h (structure de l'anneau + prédicats de jour, déjà purs).
//
// CE QUE CE MODULE DÉTECTE. Le visa pool-chemistry du 30/08/2026 établit que le
// bassin de référence est limité par l'ACTIONNEUR, pas par le correcteur : le
// maximum physiquement délivrable (~911 mL/j à 5 min/h sur 7 h de filtration)
// est sous le besoin de neutralisation (~1 800 mL/j). Aucun réglage du PID ne
// peut combler ce déficit. Le symptôme observable est une SÉRIE de journées où
// le volume délivré colle au maximum possible PENDANT QUE l'erreur pH reste
// positive (épisode C5 ouvert) : c'est cette conjonction que l'on rend visible.
//
// C'est une information de DIAGNOSTIC, jamais une décision : AUCUNE fonction de
// ce module n'est consommée par un chemin de dosage — lecture seule stricte,
// exposée à /data, au WebSocket et au journal.
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"        // kPhActuatorLimit* — source unique des seuils
#include "ph_persist_logic.h" // PhLoadHistory, phLoadDayNumber, phLoadDayUsable

/**
 * @brief Série de journées calendaires CONSÉCUTIVES saturées par l'actionneur.
 *
 * Parcourt l'anneau du jour archivé le plus récent (`days[N-1]`, invariant de
 * phLoadHistoryAppend) vers le plus ancien, et compte les journées qui
 * satisfont TOUTES les conditions :
 *   - la plus récente date d'HIER ou d'AUJOURD'HUI (un appareil arrêté deux
 *     mois retrouve des journées d'été intactes : une série ancienne ne décrit
 *     plus le régime courant — série 0, même logique que la fenêtre d'âge R1) ;
 *   - journées calendaires consécutives via phLoadDayNumber (arithmétique de
 *     dates exacte, années bissextiles comprises) — un TROU calendaire (journée
 *     non archivée : horloge invalide, appareil éteint) CASSE la série ;
 *   - journée EXPLOITABLE au sens de phLoadDayUsable (≥ 2 h de circulation,
 *     fenêtre d'âge, rapport mL/min physiquement plausible) — une journée non
 *     exploitable CASSE la série, elle ne la traverse pas ;
 *   - volume délivré ≥ kPhActuatorLimitDayFillRatio × soutenable × minutes
 *     RÉELLEMENT archivées (pas les minutes programmées : le maximum d'une
 *     journée est ce que la pompe pouvait délivrer pendant la circulation
 *     effective).
 *
 * Débit soutenable ≤ 0 ou non fini (pompe non caractérisée, config non chargée)
 * ⇒ série 0 : on n'invente pas un maximum, donc pas d'alerte (cas limite de la
 * spec). Date du jour inexploitable (horloge non synchronisée) ⇒ série 0, même
 * fail-closed que phLoadDayWithinWindow.
 *
 * @param h                   Anneau de charge relu (7 journées archivées).
 * @param today               Date du jour AAAAMMJJ (0 = horloge inexploitable).
 * @param sustainableMlPerMin Débit moyen soutenable COURANT (mL/min).
 * @return Nombre de journées consécutives saturées (0..7), la plus récente
 *         datant d'hier ou d'aujourd'hui.
 */
inline uint8_t phActuatorLimitStreak(const PhLoadHistory& h, uint32_t today,
                                     float sustainableMlPerMin) {
  if (!isfinite(sustainableMlPerMin) || sustainableMlPerMin <= 0.0f) return 0;
  const uint32_t todayNum = phLoadDayNumber(today);
  if (todayNum == 0) return 0;  // pas d'horloge ⇒ pas d'âge ⇒ pas de série

  const uint8_t n = kPhIntegralRestoreHistoryWindowDays;
  uint8_t streak = 0;
  uint32_t expectedNum = 0;  // numéro de jour attendu pour prolonger la série

  for (uint8_t k = 0; k < n; ++k) {
    const PhLoadDaySample& d = h.days[n - 1 - k];  // du plus récent au plus ancien
    const uint32_t dayNum = phLoadDayNumber(d.date);
    if (dayNum == 0) break;  // case vide ou date illisible : série close

    if (streak == 0) {
      // Ancrage : le jour archivé le plus récent doit dater d'hier ou
      // d'aujourd'hui (aujourd'hui possible si la journée vient d'être close
      // par un recul d'horloge ; hier est le cas nominal post-minuit).
      if (dayNum > todayNum || (todayNum - dayNum) > 1u) break;
    } else if (dayNum != expectedNum) {
      break;  // trou calendaire : la série ne se recolle pas par-dessus
    }

    // Journée représentative (mêmes critères que la médiane R1) ET délivré au
    // niveau du maximum possible de SES minutes de circulation.
    if (!phLoadDayUsable(d, today, sustainableMlPerMin)) break;
    const float maxDeliverableMl = sustainableMlPerMin * (float)d.minutes;
    if ((float)d.ml < kPhActuatorLimitDayFillRatio * maxDeliverableMl) break;

    ++streak;
    expectedNum = dayNum - 1u;
  }
  return streak;
}

/**
 * @brief L'installation est-elle SUSPECTÉE sous-dimensionnée en autorité de
 *        dosage pH ?
 *
 * Conjonction des deux symptômes exigés par la spec (livrable A3) :
 *   - `streakDays` journées consécutives saturées (≥ kPhActuatorLimitConsecutiveDays) —
 *     le volume délivré colle au maximum possible depuis N jours ;
 *   - `anomalyNow` : épisode C5 OUVERT (saturation de la demande AVEC erreur pH
 *     encore positive, hystérésis du module intégral) — sans lui, une série de
 *     journées pleines avec un pH À la consigne est une installation JUSTE
 *     dimensionnée, pas sous-dimensionnée, et l'alerte se réarme d'elle-même au
 *     changement de produit ou de saison (cas limites de la spec).
 *
 * Diagnostic pur : ne bloque rien, ne dose rien.
 */
inline bool phActuatorUndersized(uint8_t streakDays, bool anomalyNow) {
  return streakDays >= kPhActuatorLimitConsecutiveDays && anomalyNow;
}

#endif  // PH_ACTUATOR_LIMIT_LOGIC_H
