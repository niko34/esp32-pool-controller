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
BoostState boostState;

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

// feature-056 : sérialisation STABLE du mode d'installation.
const char* installModeToString(InstallMode mode) {
  switch (mode) {
    case InstallMode::PoweredByFiltration: return "powered";
    case InstallMode::ExternalFiltration:  return "external";
    case InstallMode::ManagedFiltration:
    default:                                return "managed";
  }
}

InstallMode installModeFromString(const char* s, InstallMode fallback) {
  if (s == nullptr) return fallback;
  if (strcmp(s, "managed") == 0)  return InstallMode::ManagedFiltration;
  if (strcmp(s, "powered") == 0)  return InstallMode::PoweredByFiltration;
  if (strcmp(s, "external") == 0) return InstallMode::ExternalFiltration;
  return fallback;
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
  // feature-027 : prise bornée (miroir de saveDailyCounters). Timeout → config RAM
  // appliquée mais persistance NVS perdue — signalé en error (throttlé 1/min).
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sLastErrMs = 0;
    unsigned long nowMs = millis();
    if (sLastErrMs == 0 || nowMs - sLastErrMs >= kMutexTimeoutWarnThrottleMs) {
      systemLogger.error("[Config] saveMqttConfig: mutex timeout — config non persistée (RAM appliquée)");
      sLastErrMs = nowMs;
    }
    return;
  }
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

  // feature-021 (Pass 4a) : calibration pH/ORP entièrement gérée par les modules
  // Atlas EZO (NVS interne au module). Aucune clé NVS pH/ORP côté ESP32.

  // Régulation ORP
  prefs.putFloat("orp_target", mqttCfg.orpTarget);
  prefs.putBool("orp_enabled", mqttCfg.orpEnabled);
  prefs.putInt("orp_pump", mqttCfg.orpPump);
  prefs.putInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
  prefs.putString("orp_reg_mode", mqttCfg.orpRegulationMode);
  prefs.putInt("orp_daily_ml", mqttCfg.orpDailyTargetMl);
  // feature-056 : mode d'installation (remplace reg_mode + filt_enabled).
  prefs.putString("install_mode", installModeToString(mqttCfg.installMode));
  prefs.putInt("stab_delay", mqttCfg.stabilizationDelayMin);
  prefs.putString("reg_speed", mqttCfg.regulationSpeed);
  prefs.putString("ph_corr_type", mqttCfg.phCorrectionType);

  // Calibration Température
  prefs.putFloat("temp_cal_off", mqttCfg.tempCalibrationOffset);
  prefs.putString("temp_cal_date", mqttCfg.tempCalibrationDate);
  prefs.putBool("temp_enabled", mqttCfg.temperatureEnabled);

  // Temps
  prefs.putBool("time_use_ntp", mqttCfg.timeUseNtp);
  prefs.putString("ntp_server", mqttCfg.ntpServer);
  prefs.putString("manual_time", mqttCfg.manualTimeIso);
  prefs.putString("timezone_id", mqttCfg.timezoneId);

  // Filtration (feature-056 : filt_enabled absorbé par install_mode)
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
  prefs.putBool("sensor_logs", authCfg.sensorLogsEnabled);
  prefs.putBool("debug_logs", authCfg.debugLogsEnabled);
  prefs.putBool("screen_enabled", authCfg.screenEnabled);

  // Puissance maximale des pompes
  prefs.putUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  prefs.putUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  prefs.putFloat("pump_max_flow", mqttCfg.pumpMaxFlowMlPerMin);

  // Limites de sécurité
  prefs.putFloat("max_ph_ml", safetyLimits.maxPhMlPerDay);
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

  // feature-021 (Pass 4a) : purge silencieuse des anciennes clés de calibration
  // pH/ORP. La calibration est désormais entièrement stockée dans les modules
  // Atlas EZO (commandes Cal,? / Cal,mid / Cal,low / Cal,<ref>).
  bool needPurge =
      prefs.isKey("ph_cal_date") || prefs.isKey("ph_cal_temp") ||
      prefs.isKey("orp_cal_off") || prefs.isKey("orp_cal_slope") ||
      prefs.isKey("orp_cal_date") || prefs.isKey("orp_cal_ref") ||
      prefs.isKey("orp_cal_temp");
  if (needPurge) {
    prefs.end();
    Preferences cleanup;
    if (cleanup.begin("poolctrl", false /*RW*/)) {
      cleanup.remove("ph_cal_date");
      cleanup.remove("ph_cal_temp");
      cleanup.remove("orp_cal_off");
      cleanup.remove("orp_cal_slope");
      cleanup.remove("orp_cal_date");
      cleanup.remove("orp_cal_ref");
      cleanup.remove("orp_cal_temp");
      cleanup.end();
      systemLogger.info("Migration NVS feature-021 : clés ph_cal_*/orp_cal_* purgées (calibration EZO interne)");
    }
    prefs.begin("poolctrl", true);
  }

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

  // feature-056 : mode d'installation. Clé "install_mode" présente → parse ;
  // sinon migration one-shot depuis l'ancien schéma (reg_mode + filt_enabled)
  // puis write-back (anciennes clés laissées inertes en NVS).
  if (prefs.isKey("install_mode")) {
    String im = prefs.getString("install_mode", "managed");
    mqttCfg.installMode = installModeFromString(im.c_str(), InstallMode::ManagedFiltration);
  } else {
    String oldReg = prefs.getString("reg_mode", "pilote");
    bool oldFiltEnabled = prefs.getBool("filt_enabled", true);
    mqttCfg.installMode = migrateInstallMode(oldReg.c_str(), oldFiltEnabled);
    prefs.end();  // réouverture en écriture pour le write-back one-shot
    Preferences w;
    if (w.begin("poolctrl", false)) {
      w.putString("install_mode", installModeToString(mqttCfg.installMode));
      w.end();
      systemLogger.info("Migration NVS install_mode : reg_mode=" + oldReg +
                        " filt_enabled=" + String(oldFiltEnabled ? 1 : 0) + " → " +
                        String(installModeToString(mqttCfg.installMode)));
    }
    prefs.begin("poolctrl", true);
  }
  mqttCfg.stabilizationDelayMin = prefs.getInt("stab_delay", 5);
  mqttCfg.regulationSpeed = prefs.getString("reg_speed", "normal");
  mqttCfg.phCorrectionType = prefs.getString("ph_corr_type", "ph_minus");

  // feature-021 (Pass 4a) : calibration ORP gérée par le module Atlas EZO
  // (commande Cal,<ref>) — aucune lecture NVS ESP32.

  // Calibration Température
  mqttCfg.tempCalibrationOffset = prefs.getFloat("temp_cal_off", 0.0f);
  mqttCfg.tempCalibrationDate = prefs.getString("temp_cal_date", "");
  mqttCfg.temperatureEnabled = prefs.getBool("temp_enabled", mqttCfg.temperatureEnabled);

  // Temps
  mqttCfg.timeUseNtp = prefs.getBool("time_use_ntp", mqttCfg.timeUseNtp);
  mqttCfg.ntpServer = prefs.getString("ntp_server", mqttCfg.ntpServer);
  mqttCfg.manualTimeIso = prefs.getString("manual_time", mqttCfg.manualTimeIso);
  mqttCfg.timezoneId = prefs.getString("timezone_id", mqttCfg.timezoneId);

  // Filtration (feature-056 : filt_enabled absorbé par install_mode ci-dessus)
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
  safetyLimits.maxPhMlPerDay = prefs.getFloat("max_ph_ml", safetyLimits.maxPhMlPerDay);
  safetyLimits.maxChlorineMlPerDay = prefs.getFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);

  prefs.end();

  sanitizePumpSelection();
  ensureTimezoneValid();
  applyTimezoneEnv();

  systemLogger.info("Configuration chargée depuis NVS");
}

void saveProductConfig() {
  // feature-027 : prise bornée. Timeout → return SANS clear productConfigDirty
  // (retry naturel au prochain passage) + warn throttlé 1/min.
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sLastWarnMs = 0;
    unsigned long nowMs = millis();
    if (sLastWarnMs == 0 || nowMs - sLastWarnMs >= kMutexTimeoutWarnThrottleMs) {
      systemLogger.warning("[Config] saveProductConfig: mutex timeout — sauvegarde reportée");
      sLastWarnMs = nowMs;
    }
    return;
  }
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

// ==== Calibration pH/ORP ====
// feature-021 : entièrement gérée par les modules Atlas EZO (commandes Cal,?,
// Cal,mid, Cal,low, Cal,<ref>). Plus de calculs offset/slope ni d'EEPROM côté
// ESP32. Voir SensorManager::enqueueCalibrate*() et getPhCalibrationPointsCached().

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
  if (phMl > safetyLimits.maxPhMlPerDay * 1.1f) {
    systemLogger.critical("[Config] NVS ph_daily_ml hors plage: " + String(phMl, 0) + " mL — remis à limite");
    phMl = safetyLimits.maxPhMlPerDay;
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

// =============================================================================
// Mode Boost (feature-053) — surchloration temporaire du jour
// =============================================================================
// Persistance NVS DÉDIÉE (namespace poolctrl, clés boost_active/boost_until) —
// écriture ciblée pour éviter l'usure NVS d'une réécriture complète de la config.

void saveBoostState() {
  // Prise de mutex bornée (miroir de saveDailyCounters).
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("[Config] saveBoostState: mutex timeout — état boost non persisté");
    return;
  }
  Preferences prefs;
  if (!prefs.begin("poolctrl", false)) {
    systemLogger.error("Échec ouverture NVS pour état boost");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
    return;
  }
  prefs.putBool("boost_active", boostState.active);
  prefs.putLong64("boost_until", (int64_t)boostState.untilEpoch);
  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
}

void loadBoostState() {
  Preferences prefs;
  if (!prefs.begin("poolctrl", true)) {
    return;  // Pas de config, boost inactif par défaut
  }
  boostState.active = prefs.getBool("boost_active", false);
  boostState.untilEpoch = (time_t)prefs.getLong64("boost_until", 0);
  prefs.end();

  // Si expiré au chargement (heure valide et minuit déjà passé) → inactif.
  time_t now = time(nullptr);
  if (boostState.active && now >= kMinValidEpoch && now >= boostState.untilEpoch) {
    boostState.active = false;
    systemLogger.info("[Boost] État chargé expiré (minuit dépassé) — boost inactif");
  } else if (boostState.active) {
    systemLogger.info("[Boost] État restauré : boost actif jusqu'à epoch " + String((long)boostState.untilEpoch));
  }
}

void startBoost() {
  time_t now = time(nullptr);
  // Condition pool-chemistry #4 : sans heure synchronisée, l'expiration à minuit
  // ne peut être calculée → refus (fail-closed).
  if (now < kMinValidEpoch) {
    systemLogger.warning("[Boost] Activation refusée : heure non synchronisée (expiration minuit indéterminable)");
    return;
  }
  // Calcul du prochain minuit local (l'environnement TZ est appliqué par applyTimezoneEnv).
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  timeinfo.tm_hour = 0;
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;
  timeinfo.tm_mday += 1;  // Minuit du jour suivant
  timeinfo.tm_isdst = -1; // Laisse mktime résoudre l'heure d'été
  time_t midnight = mktime(&timeinfo);

  // Écrire untilEpoch AVANT active : un lecteur concurrent (loopTask) voyant
  // active=true a alors toujours un untilEpoch valide (cohérence sans mutex sur
  // les champs — la paire bool+time_t est alignée, R/W atomiques Xtensa).
  boostState.untilEpoch = midnight;
  boostState.active = true;
  saveBoostState();
  systemLogger.info("[Boost] Activé jusqu'au prochain minuit local (epoch " + String((long)midnight) + ")");
  // feature-054 : signaler les leviers inertes selon la config, pour que
  // l'utilisateur sache ce que le Boost fait réellement (couvre HTTP ET HA).
  if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
    systemLogger.warning("[Boost] Filtration non gérée par PoolController — le Boost NE prolonge PAS la filtration");
  }
  if (mqttCfg.orpRegulationMode != "automatic") {
    systemLogger.warning("[Boost] Régulation ORP non automatique — le Boost NE relève PAS le chlore (seule la filtration est prolongée, si gérée)");
  }
}

void stopBoost() {
  boostState.active = false;
  boostState.untilEpoch = 0;
  saveBoostState();
  systemLogger.info("[Boost] Désactivé");
}

bool isBoostActive(time_t now) {
  // Condition pool-chemistry #4 : heure non synchronisée → ne pas expirer,
  // renvoyer l'état persisté tel quel (on ne peut pas comparer à untilEpoch).
  if (now < kMinValidEpoch) {
    return boostState.active;
  }
  return boostState.active && now < boostState.untilEpoch;
}

void tickBoostExpiry(time_t now) {
  // Expire le boost au passage de minuit. Appelé EN TÊTE de tickDailyRollover,
  // AVANT tout reset de compteur (condition pool-chemistry #3). Ne fait rien si
  // l'heure n'est pas valide (condition #4 : pas d'expiration sans heure fiable).
  if (boostState.active && now >= kMinValidEpoch && now >= boostState.untilEpoch) {
    boostState.active = false;
    boostState.untilEpoch = 0;
    saveBoostState();
    systemLogger.info("[Boost] Expiré au passage de minuit — retour au comportement normal");
  }
}
