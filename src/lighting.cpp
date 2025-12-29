#include "lighting.h"
#include "config.h"
#include "logger.h"
#include <time.h>

LightingManager lighting;

void LightingManager::begin() {
  pinMode(LIGHTING_RELAY_PIN, OUTPUT);
  digitalWrite(LIGHTING_RELAY_PIN, LOW);
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

int LightingManager::timeStringToMinutes(const String& value) {
  if (value.length() < 5 || value.charAt(2) != ':') return -1;
  int hh = value.substring(0, 2).toInt();
  int mm = value.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

bool LightingManager::isMinutesInRange(int now, int start, int end) {
  if (start == -1 || end == -1) return false;
  if (start == end) return true;
  if (start < end) {
    return now >= start && now < end;
  }
  return now >= start || now < end;
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
  ensureTimesValid();

  bool shouldBeOn = false;

  // Si contrôle manuel actif, utiliser lightingCfg.enabled
  if (state.manualOverride) {
    shouldBeOn = lightingCfg.enabled;
  }
  // Si programmation activée, vérifier l'horaire
  else if (lightingCfg.scheduleEnabled) {
    int nowMinutes = 0;
    bool haveTime = getCurrentMinutesOfDay(nowMinutes);

    if (haveTime) {
      int startMin = timeStringToMinutes(lightingCfg.startTime);
      int endMin = timeStringToMinutes(lightingCfg.endTime);
      shouldBeOn = isMinutesInRange(nowMinutes, startMin, endMin);
    }
  }
  // Sinon, utiliser lightingCfg.enabled
  else {
    shouldBeOn = lightingCfg.enabled;
  }

  // Mettre à jour le relais si nécessaire
  if (shouldBeOn != relayState) {
    digitalWrite(LIGHTING_RELAY_PIN, shouldBeOn ? HIGH : LOW);
    relayState = shouldBeOn;
    systemLogger.info(shouldBeOn ? "Éclairage allumé" : "Éclairage éteint");
    publishState();
  }
}

void LightingManager::publishState() {
  // Appelé par mqtt_manager pour éviter dépendance circulaire
}
