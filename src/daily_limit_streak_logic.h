#ifndef DAILY_LIMIT_STREAK_LOGIC_H
#define DAILY_LIMIT_STREAK_LOGIC_H

// =============================================================================
// daily_limit_streak_logic — Prédicat PUR de l'alerte « plafond journalier
// atteint J jours consécutifs » (feature-152, C6 ; extraction C1 gate
// pool-chemistry P2)
// =============================================================================
// ADR-0017, patron ph_actuator_limit_logic.h / dose_available_logic.h :
// header-only, inline, AUCUNE dépendance Arduino/FreeRTOS — compilable et
// testable en natif. La coquille impure (tickDailyRollover(), pump_controller.cpp)
// fournit `limitReachedToday` (safetyLimits.ph/orpLimitReached AU MOMENT DU
// ROLLOVER, avant leur remise à zéro) et possède l'état `streak` (persistance
// et conséquences : voir le commentaire au point d'appel).
//
// Motif du gate, mot pour mot : « une alerte non testée est une promesse. »
// Ce module isole le SEUL calcul (incrément/remise à zéro + franchissement de
// seuil) pour le rendre démontrable par des tests natifs, indépendamment de
// `systemLogger`/`mqttManager`/NVS.
// =============================================================================

#include <stdint.h>

// Verdict d'un tour de clôture de journée pour UNE voie (pH ou ORP).
struct DailyLimitStreakResult {
  uint8_t streak;  // nouvelle valeur de la streak après ce jour (0..255, sature)
  bool alert;      // true ssi `streak` atteint/dépasse le seuil CE tour
};

// Incrémente la streak si le plafond journalier a été atteint le jour qui se
// termine (`limitReachedToday`), la remet à 0 sinon — UN jour non saturé
// rompt la série, elle ne se « pardonne » pas partiellement. Sature à 255
// (ne reboucle JAMAIS à 0 par débordement, même logique que saturatingInc()
// de dosing_logic.h) : un compteur figé à 255 est un aveu lisible, un
// rebouclage à 0 serait un faux « plus d'alerte » silencieux.
//
// `alert` est vrai dès que la NOUVELLE streak atteint `alertThresholdDays` —
// pas seulement au franchissement : la coquille n'appelle cette fonction
// qu'une fois par jour calendaire (tickDailyRollover() ne rollover qu'au
// changement de date), donc `alert` vrai correspond déjà à « une fois par
// jour où la condition tient », sans edge-triggering supplémentaire ici.
// `alertThresholdDays == 0` désactiverait l'alerte (jamais le cas en
// production, kDailyLimitStreakAlertDays = 3, mais la fonction reste TOTALE).
//
// Pure, sans effet de bord : ne bloque rien, ne dose rien (même esprit que
// phActuatorUndersized() — diagnostic/alerte seule).
inline DailyLimitStreakResult evaluateDailyLimitStreak(bool limitReachedToday,
                                                       uint8_t streak,
                                                       uint8_t alertThresholdDays) {
  const uint8_t next = limitReachedToday
      ? (uint8_t)((streak < 255) ? (streak + 1) : streak)
      : 0;
  const bool alert = (alertThresholdDays > 0) && (next >= alertThresholdDays);
  return DailyLimitStreakResult{next, alert};
}

#endif  // DAILY_LIMIT_STREAK_LOGIC_H
