#include "lighting.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "mqtt_manager.h"
#include "schedule_logic.h"
#include <time.h>

LightingManager lighting;

void LightingManager::begin() {
  pinMode(kLightingRelayPin, OUTPUT);
  digitalWrite(kLightingRelayPin, LOW);
  ensureTimesValid();
  systemLogger.info("Gestionnaire d'éclairage initialisé");
}

void LightingManager::ensureTimesValid() {
  auto normalize = [](String value, const char* fallback) {
    if (value.length() < 5 || value.charAt(2) != ':') return String(fallback);
    int hh = value.substring(0, 2).toInt();
    int mm = value.substring(3, 5).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return String(fallback);
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", hh, mm);
    return String(buffer);
  };

  lightingCfg.startTime = normalize(lightingCfg.startTime, "20:00");
  lightingCfg.endTime = normalize(lightingCfg.endTime, "23:00");
}

bool LightingManager::getCurrentMinutesOfDay(int& minutes) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return true;
}

void LightingManager::setManualOn() {
  lightingCfg.enabled = true;
  state.manualOverride = true;
  state.manualSetAtMs = millis();
  systemLogger.info("Éclairage manuel: ON");
  publishState();
}

void LightingManager::setManualOff() {
  lightingCfg.enabled = false;
  state.manualOverride = true;
  state.manualSetAtMs = millis();
  systemLogger.info("Éclairage manuel: OFF");
  publishState();
}

void LightingManager::update() {
  // Si la fonction éclairage est désactivée, éteindre
  if (!lightingCfg.featureEnabled) {
    if (relayState) {
      digitalWrite(kLightingRelayPin, LOW);
      relayState = false;
      publishState();
    }
    return;
  }

  ensureTimesValid();

  // Collecte des entrées pour la décision pure (schedule_logic).
  bool manualOverride = state.manualOverride;
  bool enabledFlag = lightingCfg.enabled;
  bool scheduleEnabled = lightingCfg.scheduleEnabled;
  int nowMinutes = 0;
  bool haveTime = getCurrentMinutesOfDay(nowMinutes);
  int startMin = timeStringToMinutes(lightingCfg.startTime.c_str());
  int endMin = timeStringToMinutes(lightingCfg.endTime.c_str());

  bool shouldBeOn = decideLightingOn(manualOverride, enabledFlag, scheduleEnabled,
                                     haveTime, nowMinutes, startMin, endMin,
                                     relayState);

  // Mettre à jour le relais si nécessaire
  if (shouldBeOn != relayState) {
    digitalWrite(kLightingRelayPin, shouldBeOn ? HIGH : LOW);
    relayState = shouldBeOn;
    systemLogger.info(shouldBeOn ? "Éclairage allumé" : "Éclairage éteint");
    publishState();
  }
}

void LightingManager::publishState() {
  mqttManager.publishLightingState();
}
