#include "history_guard_logic.h"

// =============================================================================
// history_guard_logic — implémentation PURE (v2.10.0)
// =============================================================================
// Aucune dépendance runtime : logique déterministe testable en natif.

bool isCrashResetKind(HistoryResetKind kind) {
  // Seules les causes « crash » comptent pour le disjoncteur : exception CPU
  // (panic) et watchdogs. POWERON/SW/EXT/BROWNOUT/DEEPSLEEP sont des resets
  // propres — un drapeau resté posé dans ces cas ne doit PAS être compté.
  switch (kind) {
    case HistoryResetKind::Panic:
    case HistoryResetKind::IntWdt:
    case HistoryResetKind::TaskWdt:
    case HistoryResetKind::OtherWdt:
      return true;
    default:
      return false;
  }
}

HistoryGuardDecision evaluateHistoryGuard(const HistoryGuardInputs& in) {
  // 1. Pas de drapeau : l'init précédente s'est terminée proprement (crash_cnt
  //    remis à 0 en fin de begin()) ou premier boot → montage normal, la
  //    coquille pose le drapeau (compteur→1) avant de monter.
  if (in.crashCount == 0) {
    return HistoryGuardDecision::Proceed;
  }

  // 2. Drapeau présent mais reset propre (POWERON, soft reset, OTA, brownout,
  //    EN...) : on ne compte pas — réarmement simple (compteur→1) et montage.
  //    Un panic hors fenêtre d'init ne passe jamais ici (drapeau absent).
  if (!in.resetWasCrash) {
    return HistoryGuardDecision::Proceed;
  }

  // 3. Seuil atteint (frontière >=) : au moins eraseThreshold crashs consécutifs
  //    ont eu lieu pendant la fenêtre d'init history → effacer la partition
  //    AVANT montage (disponibilité de l'appareil > conservation de l'historique).
  if (in.crashCount >= in.eraseThreshold) {
    return HistoryGuardDecision::ErasePartition;
  }

  // 4. Crash pendant l'init mais sous le seuil : incrémenter (compteur+1 posé)
  //    et retenter le montage normalement.
  return HistoryGuardDecision::CountCrash;
}
