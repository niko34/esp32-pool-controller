#include "cal_manual_switch_logic.h"

bool calSwitchShouldApplyEnter(bool safetyTraceActive) {
  return !safetyTraceActive;
}

bool calSwitchShouldRestore(bool wasApplied, bool safetyTraceActiveNow) {
  if (!wasApplied) return false;
  if (safetyTraceActiveNow) return false;
  return true;
}

bool calSwitchAlertDue(uint32_t elapsedS, uint32_t sinceLastAlertS,
                       uint32_t firstDelayS, uint32_t repeatS) {
  if (elapsedS < firstDelayS) return false;
  // Sentinelle « jamais encore alertée » : la première échéance suffit.
  if (sinceLastAlertS == UINT32_MAX) return true;
  return sinceLastAlertS >= repeatS;
}
