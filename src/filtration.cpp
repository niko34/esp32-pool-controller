#include "filtration.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "mqtt_manager.h"
#include "uart_protocol.h"
#include "pump_controller.h"
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
  if (modeLower != "auto" && modeLower != "manual" && modeLower != "off" && modeLower != "force") {
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

  float referenceTemp = sensors.getTemperature();
  if (isnan(referenceTemp)) return; // Pas de température valide, on conserve le planning actuel

  // Deadband 1°C : évite les recalculs et écritures NVS pour des variations mineures
  if (!isnan(_lastScheduledTemp) && fabsf(referenceTemp - _lastScheduledTemp) < 1.0f) return;

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
  _lastScheduledTemp = referenceTemp;
  systemLogger.info("Planning auto: " + String(referenceTemp, 1) + "°C → " + filtrationCfg.start + "-" + filtrationCfg.end);
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
  if (start == end) return false;  // Plage invalide → pas de filtration
  if (start < end) {
    return now >= start && now < end;
  }
  return now >= start || now < end;
}

void FiltrationManager::update() {
  // Si la fonction filtration est désactivée, arrêter le relais
  if (!filtrationCfg.enabled) {
    if (state.running || relayState) {
      state.running = false;

      state.scheduleComputedThisCycle = false;
      state.startedAtMs = 0;
      if (relayState) {
        digitalWrite(FILTRATION_RELAY_PIN, LOW);
        relayState = false;
        publishState();
      }
    }
    return;
  }

  // Expiration des forcages après 4 heures (sécurité si l'utilisateur oublie de désactiver)
  constexpr unsigned long kForceTimeoutMs = 4UL * 3600000UL;

  if (filtrationCfg.forceOn) {
    if (state.forceOnStartMs == 0) state.forceOnStartMs = millis();
    if (millis() - state.forceOnStartMs >= kForceTimeoutMs) {
      filtrationCfg.forceOn = false;
      state.forceOnStartMs = 0;
      systemLogger.warning("ForceOn expiré (timeout 4h), retour au mode normal");
    }
  } else {
    state.forceOnStartMs = 0;
  }

  if (filtrationCfg.forceOff) {
    if (state.forceOffStartMs == 0) state.forceOffStartMs = millis();
    if (millis() - state.forceOffStartMs >= kForceTimeoutMs) {
      filtrationCfg.forceOff = false;
      state.forceOffStartMs = 0;
      systemLogger.warning("ForceOff expiré (timeout 4h), retour au mode normal");
    }
  } else {
    state.forceOffStartMs = 0;
  }

  ensureTimesValid();
  int nowMinutes = 0;
  bool haveTime = getCurrentMinutesOfDay(nowMinutes);
  String mode = filtrationCfg.mode;
  mode.toLowerCase();

  bool runTarget = false;
  int startMin = timeStringToMinutes(filtrationCfg.start);
  int endMin = timeStringToMinutes(filtrationCfg.end);

  if (filtrationCfg.forceOn) {
    runTarget = true;
  } else if (filtrationCfg.forceOff) {
    runTarget = false;
  } else if (mode == "manual" || mode == "auto") {
    if (haveTime) {
      runTarget = isMinutesInRange(nowMinutes, startMin, endMin);
    } else {
      // Time unavailable: keep current state to avoid false stop/start during OTA.
      runTarget = state.running;
    }
  }

  bool wasRunning = state.running;
  if (!wasRunning && runTarget) {
    state.running = true;
    state.startedAtMs = millis();
    state.scheduleComputedThisCycle = false;
    systemLogger.info("Démarrage filtration");
    // En mode piloté : armer le timer de stabilisation au démarrage de la filtration.
    // On n'arme le timer que si la filtration a été arrêtée suffisamment longtemps
    // (durée >= délai de stabilisation) pour éviter les boucles sur un arrêt/redémarrage
    // fugace (glitch, sauvegarde config, etc.).
    if (mqttCfg.regulationMode == "pilote") {
      unsigned long pauseMs = (state.lastStoppedAtMs == 0)
                                ? 0xFFFFFFFFUL
                                : (millis() - state.lastStoppedAtMs);
      unsigned long stabilizationMs = (unsigned long)mqttCfg.stabilizationDelayMin * 60000UL;
      if (pauseMs > stabilizationMs) {
        PumpController.armStabilizationTimer();
      }
    }
  }

  bool runNow = state.running;
  unsigned long elapsed = runNow ? millis() - state.startedAtMs : 0;

  // Après 5 min de filtration en mode auto, recalculer avec la température réelle
  // (l'eau stagnante dans les tuyauteries peut fausser la mesure initiale)
  if (runNow && mode == "auto" && elapsed >= 300000UL && !state.scheduleComputedThisCycle) {
    computeAutoSchedule();
    saveMqttConfig();
    state.scheduleComputedThisCycle = true;

    if (haveTime) {
      startMin = timeStringToMinutes(filtrationCfg.start);
      endMin = timeStringToMinutes(filtrationCfg.end);
      // Ne pas annuler un forceOn : si l'utilisateur a forcé le démarrage
      // (ex: avant la plage horaire calculée), le planning ne doit pas l'arrêter.
      if (!filtrationCfg.forceOn && !isMinutesInRange(nowMinutes, startMin, endMin)) {
        runTarget = false;
      }
    }
    publishState();
  }

  if (runNow && !runTarget) {
    state.running = false;
    state.scheduleComputedThisCycle = false;
    state.startedAtMs = 0;
    systemLogger.info("Arrêt filtration");
    state.lastStoppedAtMs = millis();
    if (mqttCfg.regulationMode == "pilote") {
      PumpController.clearStabilizationTimer();
    }
  } else {
    state.running = runTarget;
  }

  bool relayShouldBeOn = state.running && (filtrationCfg.mode != "off");
  if (relayShouldBeOn != relayState) {
    digitalWrite(FILTRATION_RELAY_PIN, relayShouldBeOn ? HIGH : LOW);
    relayState = relayShouldBeOn;
    publishState();
    if (authCfg.screenEnabled) uartProtocol.sendEventBool("event", "filtration_changed", "running", relayState);
  }
}

void FiltrationManager::publishState() {
  mqttManager.publishFiltrationState();
}
