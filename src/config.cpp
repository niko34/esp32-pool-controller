#include "config.h"
#include "logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// Définition des variables globales
MqttConfig mqttCfg;
FiltrationConfig filtrationCfg;
SimulationConfig simulationCfg;
PumpControlParams phPumpControl = {5.2f, 90.0f, 1.0f};
PumpControlParams orpPumpControl = {5.2f, 90.0f, 200.0f};
SafetyLimits safetyLimits;
PumpProtection pumpProtection;

const TimezoneInfo* findTimezoneById(const String& id) {
  for (const auto& tz : TIMEZONES) {
    if (id.equalsIgnoreCase(tz.id)) {
      return &tz;
    }
  }
  return nullptr;
}

const TimezoneInfo* defaultTimezone() {
  return &TIMEZONES[0];
}

void ensureTimezoneValid() {
  if (!findTimezoneById(mqttCfg.timezoneId)) {
    mqttCfg.timezoneId = defaultTimezone()->id;
  }
}

const TimezoneInfo* currentTimezone() {
  ensureTimezoneValid();
  const TimezoneInfo* tz = findTimezoneById(mqttCfg.timezoneId);
  return tz ? tz : defaultTimezone();
}

void applyTimezoneEnv() {
  const TimezoneInfo* tz = currentTimezone();
  setenv("TZ", tz->posix, 1);
  tzset();
}

int pumpIndexFromNumber(int pumpNumber) {
  return (pumpNumber == 2) ? 1 : 0;
}

int sanitizePumpNumber(int pumpNumber, int defaultValue) {
  return (pumpNumber == 1 || pumpNumber == 2) ? pumpNumber : defaultValue;
}

void sanitizePumpSelection() {
  mqttCfg.phPump = sanitizePumpNumber(mqttCfg.phPump, 1);
  mqttCfg.orpPump = sanitizePumpNumber(mqttCfg.orpPump, 2);
}

void saveMqttConfig() {
  File f = LittleFS.open("/mqtt.json", "w");
  if (!f) {
    systemLogger.error("Échec ouverture fichier mqtt.json pour sauvegarde");
    return;
  }

  DynamicJsonDocument doc(1024);
  doc["server"] = mqttCfg.server;
  doc["port"] = mqttCfg.port;
  doc["topic"] = mqttCfg.topic;
  doc["username"] = mqttCfg.username;
  doc["password"] = mqttCfg.password;
  doc["enabled"] = mqttCfg.enabled;
  doc["ph_target"] = mqttCfg.phTarget;
  doc["orp_target"] = mqttCfg.orpTarget;
  doc["ph_enabled"] = mqttCfg.phEnabled;
  doc["ph_pump"] = mqttCfg.phPump;
  doc["orp_enabled"] = mqttCfg.orpEnabled;
  doc["orp_pump"] = mqttCfg.orpPump;
  doc["ph_limit_seconds"] = mqttCfg.phInjectionLimitSeconds;
  doc["orp_limit_seconds"] = mqttCfg.orpInjectionLimitSeconds;
  doc["time_use_ntp"] = mqttCfg.timeUseNtp;
  doc["ntp_server"] = mqttCfg.ntpServer;
  doc["manual_time"] = mqttCfg.manualTimeIso;
  doc["timezone_id"] = mqttCfg.timezoneId;
  doc["filtration_mode"] = filtrationCfg.mode;
  doc["filtration_start"] = filtrationCfg.start;
  doc["filtration_end"] = filtrationCfg.end;
  doc["filtration_has_reference"] = filtrationCfg.hasAutoReference;
  doc["filtration_reference_temp"] = filtrationCfg.autoReferenceTemp;

  // Sauvegarder les limites de sécurité
  doc["max_ph_ml_per_day"] = safetyLimits.maxPhMinusMlPerDay;
  doc["max_chlorine_ml_per_day"] = safetyLimits.maxChlorineMlPerDay;

  if (serializeJsonPretty(doc, f) == 0) {
    systemLogger.error("Échec sérialisation JSON config");
  } else {
    systemLogger.info("Configuration sauvegardée avec succès");
  }

  f.close();
}

void loadMqttConfig() {
  if (!LittleFS.exists("/mqtt.json")) {
    systemLogger.warning("Fichier mqtt.json inexistant, création avec valeurs par défaut");
    saveMqttConfig();
    return;
  }

  File f = LittleFS.open("/mqtt.json", "r");
  if (!f) {
    systemLogger.error("Impossible d'ouvrir mqtt.json");
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, f);
  f.close();

  if (error) {
    systemLogger.error("Erreur parsing JSON: " + String(error.c_str()));
    return;
  }

  mqttCfg.server = doc["server"] | mqttCfg.server;
  mqttCfg.port = doc["port"] | mqttCfg.port;
  mqttCfg.topic = doc["topic"] | mqttCfg.topic;
  mqttCfg.username = doc["username"] | "";
  mqttCfg.password = doc["password"] | "";
  mqttCfg.enabled = doc["enabled"] | mqttCfg.enabled;
  mqttCfg.phTarget = doc["ph_target"] | mqttCfg.phTarget;
  mqttCfg.orpTarget = doc["orp_target"] | mqttCfg.orpTarget;
  mqttCfg.phEnabled = doc["ph_enabled"] | mqttCfg.phEnabled;
  mqttCfg.phPump = doc["ph_pump"] | mqttCfg.phPump;
  mqttCfg.orpEnabled = doc["orp_enabled"] | mqttCfg.orpEnabled;
  mqttCfg.orpPump = doc["orp_pump"] | mqttCfg.orpPump;
  mqttCfg.phInjectionLimitSeconds = doc["ph_limit_seconds"] | mqttCfg.phInjectionLimitSeconds;
  mqttCfg.orpInjectionLimitSeconds = doc["orp_limit_seconds"] | mqttCfg.orpInjectionLimitSeconds;
  mqttCfg.timeUseNtp = doc["time_use_ntp"] | mqttCfg.timeUseNtp;
  mqttCfg.ntpServer = doc["ntp_server"] | mqttCfg.ntpServer;
  mqttCfg.manualTimeIso = doc["manual_time"] | mqttCfg.manualTimeIso;
  mqttCfg.timezoneId = doc["timezone_id"] | mqttCfg.timezoneId;
  filtrationCfg.mode = doc["filtration_mode"] | filtrationCfg.mode;
  filtrationCfg.start = doc["filtration_start"] | filtrationCfg.start;
  filtrationCfg.end = doc["filtration_end"] | filtrationCfg.end;
  filtrationCfg.hasAutoReference = doc["filtration_has_reference"] | filtrationCfg.hasAutoReference;
  filtrationCfg.autoReferenceTemp = doc["filtration_reference_temp"] | filtrationCfg.autoReferenceTemp;

  safetyLimits.maxPhMinusMlPerDay = doc["max_ph_ml_per_day"] | safetyLimits.maxPhMinusMlPerDay;
  safetyLimits.maxChlorineMlPerDay = doc["max_chlorine_ml_per_day"] | safetyLimits.maxChlorineMlPerDay;

  sanitizePumpSelection();
  ensureTimezoneValid();
  applyTimezoneEnv();

  systemLogger.info("Configuration chargée avec succès");
}

void applyMqttConfig() {
  // Cette fonction sera implémentée dans mqtt_manager.cpp
}
