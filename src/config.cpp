#include "config.h"
#include "logger.h"
#include <Preferences.h>

// Définition des variables globales
MqttConfig mqttCfg;
FiltrationConfig filtrationCfg;
LightingConfig lightingCfg;
AuthConfig authCfg;
PumpControlParams phPumpControl = {5.2f, 90.0f, 1.0f};
PumpControlParams orpPumpControl = {5.2f, 90.0f, 200.0f};
SafetyLimits safetyLimits;
PumpProtection pumpProtection;

// Mutex pour protection concurrence
SemaphoreHandle_t configMutex = nullptr;
SemaphoreHandle_t i2cMutex = nullptr;

void initConfigMutexes() {
  configMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();

  if (configMutex == nullptr || i2cMutex == nullptr) {
    systemLogger.critical("Échec création mutex!");
  } else {
    systemLogger.info("Mutex de concurrence initialisés");
  }
}

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
  Preferences prefs;
  if (!prefs.begin("poolctrl", false)) {
    systemLogger.error("Échec ouverture NVS pour sauvegarde");
    return;
  }

  // MQTT
  prefs.putString("mqtt_server", mqttCfg.server);
  prefs.putInt("mqtt_port", mqttCfg.port);
  prefs.putString("mqtt_topic", mqttCfg.topic);
  prefs.putString("mqtt_user", mqttCfg.username);
  prefs.putString("mqtt_pass", mqttCfg.password);
  prefs.putBool("mqtt_enabled", mqttCfg.enabled);

  // Régulation pH
  prefs.putFloat("ph_target", mqttCfg.phTarget);
  prefs.putBool("ph_enabled", mqttCfg.phEnabled);
  prefs.putInt("ph_pump", mqttCfg.phPump);
  prefs.putInt("ph_limit_sec", mqttCfg.phInjectionLimitSeconds);

  // Calibration pH (DFRobot_PH stocke ses données en EEPROM)
  // On garde juste la date et température pour l'interface utilisateur
  prefs.putString("ph_cal_date", mqttCfg.phCalibrationDate);
  if (!isnan(mqttCfg.phCalibrationTemp)) {
    prefs.putFloat("ph_cal_temp", mqttCfg.phCalibrationTemp);
  }

  // Régulation ORP
  prefs.putFloat("orp_target", mqttCfg.orpTarget);
  prefs.putBool("orp_enabled", mqttCfg.orpEnabled);
  prefs.putInt("orp_pump", mqttCfg.orpPump);
  prefs.putInt("orp_limit_sec", mqttCfg.orpInjectionLimitSeconds);

  // Calibration ORP - 1 ou 2 points
  prefs.putFloat("orp_cal_off", mqttCfg.orpCalibrationOffset);
  prefs.putFloat("orp_cal_slope", mqttCfg.orpCalibrationSlope);
  prefs.putString("orp_cal_date", mqttCfg.orpCalibrationDate);
  prefs.putFloat("orp_cal_ref", mqttCfg.orpCalibrationReference);
  if (!isnan(mqttCfg.orpCalibrationTemp)) {
    prefs.putFloat("orp_cal_temp", mqttCfg.orpCalibrationTemp);
  }

  // Calibration Température
  prefs.putFloat("temp_cal_off", mqttCfg.tempCalibrationOffset);
  prefs.putString("temp_cal_date", mqttCfg.tempCalibrationDate);

  // Temps
  prefs.putBool("time_use_ntp", mqttCfg.timeUseNtp);
  prefs.putString("ntp_server", mqttCfg.ntpServer);
  prefs.putString("manual_time", mqttCfg.manualTimeIso);
  prefs.putString("timezone_id", mqttCfg.timezoneId);

  // Filtration
  prefs.putString("filt_mode", filtrationCfg.mode);
  prefs.putString("filt_start", filtrationCfg.start);
  prefs.putString("filt_end", filtrationCfg.end);
  prefs.putBool("filt_has_ref", filtrationCfg.hasAutoReference);
  prefs.putFloat("filt_ref_temp", filtrationCfg.autoReferenceTemp);

  // Éclairage
  prefs.putBool("light_enabled", lightingCfg.enabled);
  prefs.putUChar("light_bright", lightingCfg.brightness);
  prefs.putBool("light_sched_en", lightingCfg.scheduleEnabled);
  prefs.putString("light_start", lightingCfg.startTime);
  prefs.putString("light_end", lightingCfg.endTime);

  // Authentification
  prefs.putBool("auth_enabled", authCfg.enabled);
  prefs.putBool("auth_force_wifi_config", authCfg.forceWifiConfig);
  prefs.putString("auth_password", authCfg.adminPassword);
  prefs.putString("auth_token", authCfg.apiToken);
  prefs.putString("auth_cors", authCfg.corsAllowedOrigins);

  // Limites de sécurité
  prefs.putFloat("max_ph_ml", safetyLimits.maxPhMinusMlPerDay);
  prefs.putFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);

  prefs.end();
  systemLogger.info("Configuration sauvegardée dans NVS");
}

void loadMqttConfig() {
  Preferences prefs;
  if (!prefs.begin("poolctrl", true)) {  // true = read-only
    systemLogger.warning("NVS vide, création avec valeurs par défaut");
    saveMqttConfig();
    return;
  }

  // MQTT
  mqttCfg.server = prefs.getString("mqtt_server", mqttCfg.server);
  mqttCfg.port = prefs.getInt("mqtt_port", mqttCfg.port);
  mqttCfg.topic = prefs.getString("mqtt_topic", mqttCfg.topic);
  mqttCfg.username = prefs.getString("mqtt_user", "");
  mqttCfg.password = prefs.getString("mqtt_pass", "");
  mqttCfg.enabled = prefs.getBool("mqtt_enabled", mqttCfg.enabled);

  // Régulation pH
  mqttCfg.phTarget = prefs.getFloat("ph_target", mqttCfg.phTarget);
  mqttCfg.phEnabled = prefs.getBool("ph_enabled", mqttCfg.phEnabled);
  mqttCfg.phPump = prefs.getInt("ph_pump", mqttCfg.phPump);
  mqttCfg.phInjectionLimitSeconds = prefs.getInt("ph_limit_sec", mqttCfg.phInjectionLimitSeconds);

  // Calibration pH (DFRobot_PH stocke ses données en EEPROM)
  mqttCfg.phCalibrationDate = prefs.getString("ph_cal_date", "");
  mqttCfg.phCalibrationTemp = prefs.getFloat("ph_cal_temp", NAN);

  // Régulation ORP
  mqttCfg.orpTarget = prefs.getFloat("orp_target", mqttCfg.orpTarget);
  mqttCfg.orpEnabled = prefs.getBool("orp_enabled", mqttCfg.orpEnabled);
  mqttCfg.orpPump = prefs.getInt("orp_pump", mqttCfg.orpPump);
  mqttCfg.orpInjectionLimitSeconds = prefs.getInt("orp_limit_sec", mqttCfg.orpInjectionLimitSeconds);

  // Calibration ORP - 1 ou 2 points
  mqttCfg.orpCalibrationOffset = prefs.getFloat("orp_cal_off", mqttCfg.orpCalibrationOffset);
  mqttCfg.orpCalibrationSlope = prefs.getFloat("orp_cal_slope", 1.0f);
  mqttCfg.orpCalibrationDate = prefs.getString("orp_cal_date", "");
  mqttCfg.orpCalibrationReference = prefs.getFloat("orp_cal_ref", 0.0f);
  mqttCfg.orpCalibrationTemp = prefs.getFloat("orp_cal_temp", NAN);

  // Calibration Température
  mqttCfg.tempCalibrationOffset = prefs.getFloat("temp_cal_off", 0.0f);
  mqttCfg.tempCalibrationDate = prefs.getString("temp_cal_date", "");

  // Temps
  mqttCfg.timeUseNtp = prefs.getBool("time_use_ntp", mqttCfg.timeUseNtp);
  mqttCfg.ntpServer = prefs.getString("ntp_server", mqttCfg.ntpServer);
  mqttCfg.manualTimeIso = prefs.getString("manual_time", mqttCfg.manualTimeIso);
  mqttCfg.timezoneId = prefs.getString("timezone_id", mqttCfg.timezoneId);

  // Filtration
  filtrationCfg.mode = prefs.getString("filt_mode", filtrationCfg.mode);
  filtrationCfg.start = prefs.getString("filt_start", filtrationCfg.start);
  filtrationCfg.end = prefs.getString("filt_end", filtrationCfg.end);
  filtrationCfg.hasAutoReference = prefs.getBool("filt_has_ref", filtrationCfg.hasAutoReference);
  filtrationCfg.autoReferenceTemp = prefs.getFloat("filt_ref_temp", filtrationCfg.autoReferenceTemp);

  // Éclairage
  lightingCfg.enabled = prefs.getBool("light_enabled", lightingCfg.enabled);
  lightingCfg.brightness = prefs.getUChar("light_bright", lightingCfg.brightness);
  lightingCfg.scheduleEnabled = prefs.getBool("light_sched_en", lightingCfg.scheduleEnabled);
  lightingCfg.startTime = prefs.getString("light_start", lightingCfg.startTime);
  lightingCfg.endTime = prefs.getString("light_end", lightingCfg.endTime);

  // Authentification
  authCfg.enabled = prefs.getBool("auth_enabled", authCfg.enabled);
  authCfg.forceWifiConfig = prefs.getBool("auth_force_wifi_config", authCfg.forceWifiConfig);
  authCfg.adminPassword = prefs.getString("auth_password", authCfg.adminPassword);
  authCfg.apiToken = prefs.getString("auth_token", authCfg.apiToken);
  authCfg.corsAllowedOrigins = prefs.getString("auth_cors", authCfg.corsAllowedOrigins);

  // Limites de sécurité
  safetyLimits.maxPhMinusMlPerDay = prefs.getFloat("max_ph_ml", safetyLimits.maxPhMinusMlPerDay);
  safetyLimits.maxChlorineMlPerDay = prefs.getFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);

  prefs.end();

  sanitizePumpSelection();
  ensureTimezoneValid();
  applyTimezoneEnv();

  systemLogger.info("Configuration chargée depuis NVS");
}

void applyMqttConfig() {
  // Cette fonction sera implémentée dans mqtt_manager.cpp
}

// ==== Fonctions de calibration pH (DFRobot_PH) ====
// La calibration est maintenant gérée par la librairie DFRobot_PH
// qui stocke les données directement en EEPROM

void calculatePhCalibration() {
  // Fonction conservée pour compatibilité mais non utilisée
  // La calibration est gérée par DFRobot_PH
}

bool isPhCalibrationValid() {
  // Vérifier si une calibration a été effectuée
  return !mqttCfg.phCalibrationDate.isEmpty();
}
