#include "config.h"
#include "constants.h"
#include "logger.h"
#include <Preferences.h>

// Définition des variables globales
MqttConfig mqttCfg;
FiltrationConfig filtrationCfg;
LightingConfig lightingCfg;
AuthConfig authCfg;
PumpControlParams phPumpControl  = {kPumpMinFlowMlPerMin, kPumpMaxFlowMlPerMin, kPhMaxError};
PumpControlParams orpPumpControl = {kPumpMinFlowMlPerMin, kPumpMaxFlowMlPerMin, kOrpMaxError};
SafetyLimits safetyLimits;
PumpProtection pumpProtection;
ProductConfig productCfg;
bool productConfigDirty = false;

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
  prefs.putString("reg_mode", mqttCfg.regulationMode);
  prefs.putInt("stab_delay", mqttCfg.stabilizationDelayMin);
  prefs.putString("reg_speed", mqttCfg.regulationSpeed);
  prefs.putString("ph_corr_type", mqttCfg.phCorrectionType);

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
  prefs.putBool("temp_enabled", mqttCfg.temperatureEnabled);

  // Temps
  prefs.putBool("time_use_ntp", mqttCfg.timeUseNtp);
  prefs.putString("ntp_server", mqttCfg.ntpServer);
  prefs.putString("manual_time", mqttCfg.manualTimeIso);
  prefs.putString("timezone_id", mqttCfg.timezoneId);

  // Filtration
  prefs.putBool("filt_enabled", filtrationCfg.enabled);
  prefs.putString("filt_mode", filtrationCfg.mode);
  prefs.putString("filt_start", filtrationCfg.start);
  prefs.putString("filt_end", filtrationCfg.end);


  // Éclairage
  prefs.putBool("light_feat_en", lightingCfg.featureEnabled);
  prefs.putBool("light_enabled", lightingCfg.enabled);
  prefs.putUChar("light_bright", lightingCfg.brightness);
  prefs.putBool("light_sched_en", lightingCfg.scheduleEnabled);
  prefs.putString("light_start", lightingCfg.startTime);
  prefs.putString("light_end", lightingCfg.endTime);

  // Authentification
  prefs.putBool("auth_enabled", authCfg.enabled);
  prefs.putBool("auth_force_cfg", authCfg.forceWifiConfig);
  prefs.putBool("wizard_done", authCfg.wizardCompleted);
  prefs.putBool("disable_ap_boot", authCfg.disableApOnBoot);
  prefs.putString("auth_password", authCfg.adminPassword);
  prefs.putString("auth_token", authCfg.apiToken);
  prefs.putString("auth_ap_pwd", authCfg.apPassword);
  prefs.putString("auth_cors", authCfg.corsAllowedOrigins);
  prefs.putBool("sensor_logs", authCfg.sensorLogsEnabled);

  // Puissance maximale des pompes
  prefs.putUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  prefs.putUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  prefs.putFloat("pump_max_flow", mqttCfg.pumpMaxFlowMlPerMin);
  prefs.putUInt("pump_pause_min", mqttCfg.minPauseBetweenMin);

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
  mqttCfg.regulationMode = prefs.getString("reg_mode", "pilote");
  mqttCfg.stabilizationDelayMin = prefs.getInt("stab_delay", 5);
  mqttCfg.regulationSpeed = prefs.getString("reg_speed", "normal");
  mqttCfg.phCorrectionType = prefs.getString("ph_corr_type", "ph_minus");

  // Calibration ORP - 1 ou 2 points
  mqttCfg.orpCalibrationOffset = prefs.getFloat("orp_cal_off", mqttCfg.orpCalibrationOffset);
  mqttCfg.orpCalibrationSlope = prefs.getFloat("orp_cal_slope", 1.0f);
  mqttCfg.orpCalibrationDate = prefs.getString("orp_cal_date", "");
  mqttCfg.orpCalibrationReference = prefs.getFloat("orp_cal_ref", 0.0f);
  mqttCfg.orpCalibrationTemp = prefs.getFloat("orp_cal_temp", NAN);

  // Calibration Température
  mqttCfg.tempCalibrationOffset = prefs.getFloat("temp_cal_off", 0.0f);
  mqttCfg.tempCalibrationDate = prefs.getString("temp_cal_date", "");
  mqttCfg.temperatureEnabled = prefs.getBool("temp_enabled", mqttCfg.temperatureEnabled);

  // Temps
  mqttCfg.timeUseNtp = prefs.getBool("time_use_ntp", mqttCfg.timeUseNtp);
  mqttCfg.ntpServer = prefs.getString("ntp_server", mqttCfg.ntpServer);
  mqttCfg.manualTimeIso = prefs.getString("manual_time", mqttCfg.manualTimeIso);
  mqttCfg.timezoneId = prefs.getString("timezone_id", mqttCfg.timezoneId);

  // Filtration
  filtrationCfg.enabled = prefs.getBool("filt_enabled", filtrationCfg.enabled);
  filtrationCfg.mode = prefs.getString("filt_mode", filtrationCfg.mode);
  filtrationCfg.start = prefs.getString("filt_start", filtrationCfg.start);
  filtrationCfg.end = prefs.getString("filt_end", filtrationCfg.end);


  // Éclairage
  lightingCfg.featureEnabled = prefs.getBool("light_feat_en", lightingCfg.featureEnabled);
  lightingCfg.enabled = prefs.getBool("light_enabled", lightingCfg.enabled);
  lightingCfg.brightness = prefs.getUChar("light_bright", lightingCfg.brightness);
  lightingCfg.scheduleEnabled = prefs.getBool("light_sched_en", lightingCfg.scheduleEnabled);
  lightingCfg.startTime = prefs.getString("light_start", lightingCfg.startTime);
  lightingCfg.endTime = prefs.getString("light_end", lightingCfg.endTime);

  // Authentification
  authCfg.enabled = prefs.getBool("auth_enabled", authCfg.enabled);
  authCfg.forceWifiConfig = prefs.getBool("auth_force_cfg", authCfg.forceWifiConfig);
  authCfg.wizardCompleted = prefs.getBool("wizard_done", false);
  authCfg.disableApOnBoot = prefs.getBool("disable_ap_boot", false);
  authCfg.adminPassword = prefs.getString("auth_password", authCfg.adminPassword);
  authCfg.apiToken = prefs.getString("auth_token", authCfg.apiToken);
  authCfg.apPassword = prefs.getString("auth_ap_pwd", authCfg.apPassword);
  authCfg.corsAllowedOrigins = prefs.getString("auth_cors", authCfg.corsAllowedOrigins);
  authCfg.sensorLogsEnabled = prefs.getBool("sensor_logs", false);

  // Puissance maximale des pompes
  mqttCfg.pump1MaxDutyPct = prefs.getUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  mqttCfg.pump2MaxDutyPct = prefs.getUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  mqttCfg.pumpMaxFlowMlPerMin = prefs.getFloat("pump_max_flow", mqttCfg.pumpMaxFlowMlPerMin);
  phPumpControl.maxFlowMlPerMin = mqttCfg.pumpMaxFlowMlPerMin;
  orpPumpControl.maxFlowMlPerMin = mqttCfg.pumpMaxFlowMlPerMin;
  mqttCfg.minPauseBetweenMin = prefs.getUInt("pump_pause_min", mqttCfg.minPauseBetweenMin);
  pumpProtection.minPauseBetweenMs = mqttCfg.minPauseBetweenMin * 60000UL;

  // Limites de sécurité
  safetyLimits.maxPhMinusMlPerDay = prefs.getFloat("max_ph_ml", safetyLimits.maxPhMinusMlPerDay);
  safetyLimits.maxChlorineMlPerDay = prefs.getFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);

  prefs.end();

  sanitizePumpSelection();
  ensureTimezoneValid();
  applyTimezoneEnv();

  systemLogger.info("Configuration chargée depuis NVS");
}

void saveProductConfig() {
  Preferences prefs;
  if (!prefs.begin("pool_prod", false)) {
    systemLogger.error("Échec ouverture NVS produits");
    return;
  }
  prefs.putBool("ph_track_en", productCfg.phTrackingEnabled);
  prefs.putFloat("ph_cont_ml", productCfg.phContainerVolumeMl);
  prefs.putFloat("ph_inj_ml", productCfg.phTotalInjectedMl);
  prefs.putFloat("ph_alert_ml", productCfg.phAlertThresholdMl);
  prefs.putBool("orp_track_en", productCfg.orpTrackingEnabled);
  prefs.putFloat("orp_cont_ml", productCfg.orpContainerVolumeMl);
  prefs.putFloat("orp_inj_ml", productCfg.orpTotalInjectedMl);
  prefs.putFloat("orp_alert_ml", productCfg.orpAlertThresholdMl);
  prefs.end();
  productConfigDirty = false;
  systemLogger.debug("Config produits sauvegardée");
}

void loadProductConfig() {
  Preferences prefs;
  if (!prefs.begin("pool_prod", true)) {
    return; // Pas encore de config produits, utiliser les valeurs par défaut
  }
  productCfg.phTrackingEnabled = prefs.getBool("ph_track_en", productCfg.phTrackingEnabled);
  productCfg.phContainerVolumeMl = prefs.getFloat("ph_cont_ml", productCfg.phContainerVolumeMl);
  productCfg.phTotalInjectedMl = prefs.getFloat("ph_inj_ml", productCfg.phTotalInjectedMl);
  productCfg.phAlertThresholdMl = prefs.getFloat("ph_alert_ml", productCfg.phAlertThresholdMl);
  productCfg.orpTrackingEnabled = prefs.getBool("orp_track_en", productCfg.orpTrackingEnabled);
  productCfg.orpContainerVolumeMl = prefs.getFloat("orp_cont_ml", productCfg.orpContainerVolumeMl);
  productCfg.orpTotalInjectedMl = prefs.getFloat("orp_inj_ml", productCfg.orpTotalInjectedMl);
  productCfg.orpAlertThresholdMl = prefs.getFloat("orp_alert_ml", productCfg.orpAlertThresholdMl);
  prefs.end();
  systemLogger.info("Config produits chargée depuis NVS");
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
