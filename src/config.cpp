#include "config.h"
#include "constants.h"
#include "logger.h"
#include <Preferences.h>
#include <time.h>

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
  configMutex = xSemaphoreCreateRecursiveMutex();  // Récursif : saveConfig() peut être appelé dans une section déjà sous mutex
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
  if (configMutex) xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
  Preferences prefs;
  if (!prefs.begin("poolctrl", false)) {
    systemLogger.error("Échec ouverture NVS pour sauvegarde");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
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
  prefs.putInt("ph_limit_min", mqttCfg.phInjectionLimitMinutes);
  prefs.putString("ph_reg_mode", mqttCfg.phRegulationMode);
  prefs.putInt("ph_daily_ml", mqttCfg.phDailyTargetMl);

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
  prefs.putInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
  prefs.putString("orp_reg_mode", mqttCfg.orpRegulationMode);
  prefs.putInt("orp_daily_ml", mqttCfg.orpDailyTargetMl);
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
  prefs.putBool("debug_logs", authCfg.debugLogsEnabled);
  prefs.putBool("screen_enabled", authCfg.screenEnabled);

  // Puissance maximale des pompes
  prefs.putUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  prefs.putUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  prefs.putFloat("pump_max_flow", mqttCfg.pumpMaxFlowMlPerMin);

  // Limites de sécurité
  prefs.putFloat("max_ph_ml", safetyLimits.maxPhMinusMlPerDay);
  prefs.putFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);

  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
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
  // Migration NVS : ph_limit_sec (secondes) → ph_limit_min (minutes)
  if (prefs.isKey("ph_limit_sec")) {
    int oldSec = prefs.getInt("ph_limit_sec", 300);
    mqttCfg.phInjectionLimitMinutes = max(1, oldSec / 60);
    systemLogger.info("Migration NVS ph_limit_sec=" + String(oldSec) + "s → ph_limit_min=" + String(mqttCfg.phInjectionLimitMinutes) + "min");
    prefs.end();
    if (prefs.begin("poolctrl", false)) {
      prefs.putInt("ph_limit_min", mqttCfg.phInjectionLimitMinutes);
      prefs.remove("ph_limit_sec");
      prefs.end();
    }
    prefs.begin("poolctrl", true);
  } else {
    mqttCfg.phInjectionLimitMinutes = prefs.getInt("ph_limit_min", mqttCfg.phInjectionLimitMinutes);
  }

  // Migration ph_enabled → ph_regulation_mode
  if (prefs.isKey("ph_reg_mode")) {
    mqttCfg.phRegulationMode = prefs.getString("ph_reg_mode", "automatic");
  } else {
    mqttCfg.phRegulationMode = mqttCfg.phEnabled ? "automatic" : "manual";
    systemLogger.info("Migration ph_enabled → ph_regulation_mode: " + mqttCfg.phRegulationMode);
  }
  mqttCfg.phDailyTargetMl = prefs.getInt("ph_daily_ml", 0);
  if (mqttCfg.phRegulationMode != "automatic" &&
      mqttCfg.phRegulationMode != "scheduled" &&
      mqttCfg.phRegulationMode != "manual") {
    mqttCfg.phRegulationMode = "automatic";
  }
  mqttCfg.phEnabled = (mqttCfg.phRegulationMode != "manual");

  // Calibration pH (DFRobot_PH stocke ses données en EEPROM)
  mqttCfg.phCalibrationDate = prefs.getString("ph_cal_date", "");
  mqttCfg.phCalibrationTemp = prefs.getFloat("ph_cal_temp", NAN);

  // Régulation ORP
  mqttCfg.orpTarget = prefs.getFloat("orp_target", mqttCfg.orpTarget);
  mqttCfg.orpEnabled = prefs.getBool("orp_enabled", mqttCfg.orpEnabled);
  mqttCfg.orpPump = prefs.getInt("orp_pump", mqttCfg.orpPump);
  // Migration NVS : orp_limit_sec (secondes) → orp_limit_min (minutes)
  if (prefs.isKey("orp_limit_sec")) {
    int oldSec = prefs.getInt("orp_limit_sec", 600);
    mqttCfg.orpInjectionLimitMinutes = max(1, oldSec / 60);
    systemLogger.info("Migration NVS orp_limit_sec=" + String(oldSec) + "s → orp_limit_min=" + String(mqttCfg.orpInjectionLimitMinutes) + "min");
    prefs.end();
    if (prefs.begin("poolctrl", false)) {
      prefs.putInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
      prefs.remove("orp_limit_sec");
      prefs.end();
    }
    prefs.begin("poolctrl", true);
  } else {
    mqttCfg.orpInjectionLimitMinutes = prefs.getInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
  }
  // Migration orp_enabled → orp_regulation_mode
  if (prefs.isKey("orp_reg_mode")) {
    mqttCfg.orpRegulationMode = prefs.getString("orp_reg_mode", "automatic");
  } else {
    mqttCfg.orpRegulationMode = mqttCfg.orpEnabled ? "automatic" : "manual";
    systemLogger.info("Migration orp_enabled → orp_regulation_mode: " + mqttCfg.orpRegulationMode);
  }
  mqttCfg.orpDailyTargetMl = prefs.getInt("orp_daily_ml", 0);
  if (mqttCfg.orpRegulationMode != "automatic" &&
      mqttCfg.orpRegulationMode != "scheduled" &&
      mqttCfg.orpRegulationMode != "manual") {
    mqttCfg.orpRegulationMode = "automatic";
  }
  mqttCfg.orpEnabled = (mqttCfg.orpRegulationMode != "manual");

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
  authCfg.debugLogsEnabled = prefs.getBool("debug_logs", false);
  authCfg.screenEnabled = prefs.getBool("screen_enabled", false);

  // Puissance maximale des pompes
  mqttCfg.pump1MaxDutyPct = prefs.getUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  mqttCfg.pump2MaxDutyPct = prefs.getUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  mqttCfg.pumpMaxFlowMlPerMin = prefs.getFloat("pump_max_flow", mqttCfg.pumpMaxFlowMlPerMin);
  phPumpControl.maxFlowMlPerMin = mqttCfg.pumpMaxFlowMlPerMin;
  orpPumpControl.maxFlowMlPerMin = mqttCfg.pumpMaxFlowMlPerMin;

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
  if (configMutex) xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
  Preferences prefs;
  if (!prefs.begin("pool_prod", false)) {
    systemLogger.error("Échec ouverture NVS produits");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
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
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
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

/**
 * Persiste les compteurs journaliers de dosage en NVS (namespace "pool-daily").
 * Appelé : au démarrage d'une injection, toutes les 30s pendant l'injection,
 * et au reset journalier (minuit local).
 */
void saveDailyCounters() {
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("[Config] saveDailyCounters: mutex timeout");
    return;
  }
  Preferences prefs;
  prefs.begin("pool-daily", false);
  prefs.putFloat("ph_daily_ml", safetyLimits.dailyPhInjectedMl);
  prefs.putFloat("orp_daily_ml", safetyLimits.dailyOrpInjectedMl);
  prefs.putString("daily_date", safetyLimits.currentDayDate);
  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
}

/**
 * Restaure les compteurs journaliers depuis NVS au démarrage.
 * Compare la date sauvegardée avec la date courante (si NTP/RTC valide).
 * Si nouveau jour : remet les compteurs à 0 et sauvegarde.
 * Si heure non synchronisée : restaure conservativement pour ne pas perdre les comptages.
 */
void loadDailyCounters() {
  Preferences prefs;
  prefs.begin("pool-daily", true);
  bool hasDate = prefs.isKey("daily_date");
  float phMl   = prefs.getFloat("ph_daily_ml", 0.0f);
  float orpMl  = prefs.getFloat("orp_daily_ml", 0.0f);
  String savedDate = prefs.getString("daily_date", "");
  prefs.end();

  if (!hasDate) {
    systemLogger.info("[Config] loadDailyCounters: aucune donnée NVS, démarrage à 0");
    return;
  }

  // Validation des valeurs lues
  if (phMl < 0.0f) phMl = 0.0f;
  if (orpMl < 0.0f) orpMl = 0.0f;
  if (phMl > safetyLimits.maxPhMinusMlPerDay * 1.1f) {
    systemLogger.critical("[Config] NVS ph_daily_ml hors plage: " + String(phMl, 0) + " mL — remis à limite");
    phMl = safetyLimits.maxPhMinusMlPerDay;
  }
  if (orpMl > safetyLimits.maxChlorineMlPerDay * 1.1f) {
    systemLogger.critical("[Config] NVS orp_daily_ml hors plage: " + String(orpMl, 0) + " mL — remis à limite");
    orpMl = safetyLimits.maxChlorineMlPerDay;
  }

  time_t now = time(nullptr);
  if (now >= kMinValidEpoch) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char todayStr[9];
    strftime(todayStr, sizeof(todayStr), "%Y%m%d", &timeinfo);

    if (savedDate == todayStr) {
      safetyLimits.dailyPhInjectedMl  = phMl;
      safetyLimits.dailyOrpInjectedMl = orpMl;
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      systemLogger.info("[Config] Compteurs journaliers restaurés: pH=" + String(phMl, 0) + " mL, ORP=" + String(orpMl, 0) + " mL");
    } else {
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      saveDailyCounters();
      systemLogger.info("[Config] Nouveau jour détecté — compteurs remis à 0");
    }
  } else {
    // Heure non synchronisée : restore conservatif (sécurité)
    safetyLimits.dailyPhInjectedMl  = phMl;
    safetyLimits.dailyOrpInjectedMl = orpMl;
    strlcpy(safetyLimits.currentDayDate, savedDate.c_str(), sizeof(safetyLimits.currentDayDate));
    systemLogger.info("[Config] Heure non synchro — compteurs restaurés conservativement");
  }
}
