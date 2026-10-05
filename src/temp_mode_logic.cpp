#include "temp_mode_logic.h"
#include "constants.h"  // kRescueChlorineDailyMinMl (feature-141, borne chimique figée)

OrpMode orpModeFromString(const char* s) {
  if (s == nullptr) return OrpMode::Manual;
  if (strcmp(s, "automatic") == 0) return OrpMode::Automatic;
  if (strcmp(s, "scheduled") == 0) return OrpMode::Scheduled;
  if (strcmp(s, "manual") == 0) return OrpMode::Manual;
  return OrpMode::Manual;  // fail-closed : chaîne NVS non reconnue
}

OrpMode effectiveOrpModePure(OrpMode nvsMode, bool rescueActive) {
  if (nvsMode == OrpMode::Manual) return OrpMode::Manual;
  if (rescueActive) return OrpMode::Scheduled;
  return nvsMode;
}

bool rescueActivePure(bool active, time_t now, time_t untilEpoch, time_t minValidEpoch) {
  if (now < minValidEpoch) return false;  // fail-closed, condition #6 du gate
  return active && now < untilEpoch;
}

time_t rescueDeadlinePure(time_t now, time_t durationS) {
  return now + durationS;
}

float effectiveOrpDailyTargetMlPure(float nvsDailyTargetMl, bool rescueActive,
                                     bool boostActive, float rescueDailyMl,
                                     float boostFactor, float maxChlorineMlPerDay,
                                     float rescueHardCapMl) {
  if (!rescueActive) return nvsDailyTargetMl;

  float volume = rescueDailyMl;
  if (boostActive) {
    volume *= boostFactor;  // amplification EN PREMIER (condition #4)
  }
  if (maxChlorineMlPerDay > 0.0f && volume > maxChlorineMlPerDay) {
    volume = maxChlorineMlPerDay;  // limite utilisateur souveraine (condition #3)
  }
  if (volume > rescueHardCapMl) {
    volume = rescueHardCapMl;  // plafond dur appliqué EN DERNIER (condition #4)
  }
  return volume;
}

uint8_t rescueLiftReasonVisiblePure(uint8_t reasonCode, uint32_t nowMs,
                                     uint32_t setAtMs, uint32_t visibleMs) {
  if (reasonCode == 0) return 0;
  if ((uint32_t)(nowMs - setAtMs) >= visibleMs) return 0;
  return reasonCode;
}

float clampRescueDailyMlPure(float requestedMl, float userDailyLimitMl, float hardCapMl) {
  float volume = requestedMl;
  if (volume < kRescueChlorineDailyMinMl) {
    volume = kRescueChlorineDailyMinMl;  // plancher FIGÉ, appliqué EN PREMIER
  }
  float ceiling = hardCapMl;
  if (userDailyLimitMl > 0.0f && userDailyLimitMl < ceiling) {
    ceiling = userDailyLimitMl;  // la limite utilisateur reste souveraine
  }
  if (volume > ceiling) {
    volume = ceiling;
  }
  return volume;
}

float resolveRescueDailyMlPure(float storedMl, float orpDailyTargetMl,
                                float sizeDefaultMl, float fallbackMl,
                                float userDailyLimitMl, float hardCapMl) {
  float requested = storedMl;
  const bool outOfBounds = !(storedMl >= kRescueChlorineDailyMinMl && storedMl <= hardCapMl);
  if (outOfBounds) {
    if (orpDailyTargetMl > 0.0f) {
      requested = orpDailyTargetMl;
    } else if (sizeDefaultMl > 0.0f) {
      requested = sizeDefaultMl;  // étage SAUTÉ si sizeDefaultMl == 0 (chemin MQTT)
    } else {
      requested = fallbackMl;  // repli final, JAMAIS 0
    }
  }
  return clampRescueDailyMlPure(requested, userDailyLimitMl, hardCapMl);
}
