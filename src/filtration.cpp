#include "filtration.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include <time.h>

FiltrationManager filtration;

void FiltrationManager::begin() {
  pinMode(FILTRATION_RELAY_PIN, OUTPUT);
  digitalWrite(FILTRATION_RELAY_PIN, LOW);
  ensureTimesValid();
  systemLogger.info("Gestionnaire de filtration initialisé");
}

void FiltrationManager::ensureTimesValid() {
  String modeLower = filtrationCfg.mode;
  modeLower.toLowerCase();
  if (modeLower != "auto" && modeLower != "manual" && modeLower != "off") {
    filtrationCfg.mode = "auto";
  }

  auto normalize = [](String value, const char* fallback) {
    if (value.length() < 5 || value.charAt(2) != ':') return String(fallback);
    int hh = value.substring(0, 2).toInt();
    int mm = value.substring(3, 5).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return String(fallback);
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", hh, mm);
    return String(buffer);
  };

  filtrationCfg.start = normalize(filtrationCfg.start, "08:00");
  filtrationCfg.end = normalize(filtrationCfg.end, "20:00");
}

void FiltrationManager::computeAutoSchedule() {
  if (!filtrationCfg.mode.equalsIgnoreCase("auto")) return;

  float referenceTemp = filtrationCfg.hasAutoReference ? filtrationCfg.autoReferenceTemp : sensors.getTemperature();
  if (isnan(referenceTemp)) {
    if (!filtrationCfg.hasAutoReference) {
      filtrationCfg.start = "08:00";
      filtrationCfg.end = "20:00";
      return;
    }
    referenceTemp = filtrationCfg.autoReferenceTemp;
  }

  if (referenceTemp < 0) referenceTemp = 0;
  float durationHours = referenceTemp / 2.0f;
  if (durationHours < 1.0f) durationHours = 1.0f;
  if (durationHours > 24.0f) durationHours = 24.0f;

  float startHour = kFiltrationPivotHour - (durationHours / 2.0f);
  float endHour = startHour + durationHours;

  auto wrap = [](float hour) {
    while (hour < 0) hour += 24.0f;
    while (hour >= 24.0f) hour -= 24.0f;
    return hour;
  };

  startHour = wrap(startHour);
  endHour = wrap(endHour);

  auto toTimeString = [](float hour) {
    int h = static_cast<int>(hour);
    int m = static_cast<int>(round((hour - h) * 60.0f));
    if (m >= 60) { m -= 60; h = (h + 1) % 24; }
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", h, m);
    return String(buffer);
  };

  filtrationCfg.start = toTimeString(startHour);
  filtrationCfg.end = toTimeString(endHour);
  ensureTimesValid();
}

bool FiltrationManager::getCurrentMinutesOfDay(int& minutes) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return true;
}

int FiltrationManager::timeStringToMinutes(const String& value) {
  if (value.length() < 5 || value.charAt(2) != ':') return -1;
  int hh = value.substring(0, 2).toInt();
  int mm = value.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

bool FiltrationManager::isMinutesInRange(int now, int start, int end) {
  if (start == -1 || end == -1) return false;
  if (start == end) return true;
  if (start < end) {
    return now >= start && now < end;
  }
  return now >= start || now < end;
}

void FiltrationManager::update() {
  ensureTimesValid();
  int nowMinutes = 0;
  bool haveTime = getCurrentMinutesOfDay(nowMinutes);
  String mode = filtrationCfg.mode;
  mode.toLowerCase();

  bool runTarget = false;
  int startMin = timeStringToMinutes(filtrationCfg.start);
  int endMin = timeStringToMinutes(filtrationCfg.end);

  if (mode == "manual" || mode == "auto") {
    if (haveTime) runTarget = isMinutesInRange(nowMinutes, startMin, endMin);
  }

  bool wasRunning = state.running;
  if (!wasRunning && runTarget) {
    state.running = true;
    state.startedAtMs = millis();
    state.cycleMaxTemp = -INFINITY;
    state.scheduleComputedThisCycle = false;
    systemLogger.info("Démarrage filtration");
  }

  bool runNow = state.running;
  unsigned long elapsed = runNow ? millis() - state.startedAtMs : 0;

  if (runNow) {
    if (elapsed >= 600000UL && !isnan(sensors.getTemperature())) {
      state.cycleMaxTemp = (state.cycleMaxTemp == -INFINITY)
        ? sensors.getTemperature()
        : max(state.cycleMaxTemp, sensors.getTemperature());
    }

    if (mode == "auto" && elapsed >= 600000UL && state.cycleMaxTemp > -INFINITY && !state.scheduleComputedThisCycle) {
      filtrationCfg.autoReferenceTemp = state.cycleMaxTemp;
      filtrationCfg.hasAutoReference = true;
      computeAutoSchedule();
      saveMqttConfig();
      systemLogger.info("Référence auto filtration: " + String(filtrationCfg.autoReferenceTemp) + "°C");
      state.scheduleComputedThisCycle = true;

      if (haveTime) {
        startMin = timeStringToMinutes(filtrationCfg.start);
        endMin = timeStringToMinutes(filtrationCfg.end);
        if (!isMinutesInRange(nowMinutes, startMin, endMin)) {
          runTarget = false;
        }
      }
      publishState();
    }
  }

  if (runNow && !runTarget) {
    state.running = false;
    state.cycleMaxTemp = -INFINITY;
    state.scheduleComputedThisCycle = false;
    state.startedAtMs = 0;
    systemLogger.info("Arrêt filtration");
  } else {
    state.running = runTarget;
  }

  bool relayShouldBeOn = state.running && (filtrationCfg.mode != "off");
  if (relayShouldBeOn != relayState) {
    digitalWrite(FILTRATION_RELAY_PIN, relayShouldBeOn ? HIGH : LOW);
    relayState = relayShouldBeOn;
    publishState();
  }
}

void FiltrationManager::publishState() {
  // Appelé par mqtt_manager pour éviter dépendance circulaire
}
