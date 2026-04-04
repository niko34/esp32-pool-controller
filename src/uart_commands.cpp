#include "uart_commands.h"
#include "uart_protocol.h"
#include "json_compat.h"
#include "config.h"
#include "constants.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include "lighting.h"
#include "mqtt_manager.h"
#include "auth.h"
#include "version.h"
#include <WiFi.h>
#include <time.h>

UartCommands uartCommands;

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void UartCommands::dispatch(const char* cmd, JsonVariant data) {
  if (strcmp(cmd, "ping") == 0) {
    handlePing();
  } else if (strcmp(cmd, "get_info") == 0) {
    handleGetInfo();
  } else if (strcmp(cmd, "get_status") == 0) {
    handleGetStatus();
  } else if (strcmp(cmd, "get_config") == 0) {
    handleGetConfig();
  } else if (strcmp(cmd, "get_alarms") == 0) {
    handleGetAlarms();
  } else if (strcmp(cmd, "get_network_status") == 0) {
    handleGetNetworkStatus();
  } else if (strcmp(cmd, "set_config") == 0) {
    handleSetConfig(data);
  } else if (strcmp(cmd, "save_config") == 0) {
    handleSaveConfig();
  } else if (strcmp(cmd, "run_action") == 0) {
    handleRunAction(data);
  } else if (strcmp(cmd, "get_setup_status") == 0) {
    handleGetSetupStatus();
  } else if (strcmp(cmd, "complete_wizard") == 0) {
    handleCompleteWizard();
  } else if (strcmp(cmd, "wifi_scan") == 0) {
    handleWifiScan();
  } else if (strcmp(cmd, "wifi_connect") == 0) {
    handleWifiConnect(data);
  } else if (strcmp(cmd, "change_password") == 0) {
    handleChangePassword(data);
  } else {
    uartProtocol.sendError(cmd, "unknown command");
  }
}

// ---------------------------------------------------------------------------
// Commandes de lecture
// ---------------------------------------------------------------------------

void UartCommands::handlePing() {
  StaticJson<32> doc;
  doc["type"] = "pong";
  uartProtocol.sendJson(doc);
}

void UartCommands::handleGetInfo() {
  StaticJson<192> doc;
  doc["type"] = "info";
  JsonObject d = doc["data"].to<JsonObject>();
  d["firmware"] = FIRMWARE_VERSION;
  d["build_date"] = FIRMWARE_BUILD_DATE;
  d["build_time"] = FIRMWARE_BUILD_TIME;
  d["board"] = "esp32_pool_controller";
  d["uptime_s"] = millis() / 1000UL;
  d["free_heap"] = ESP.getFreeHeap();
  uartProtocol.sendJson(doc);
}

void UartCommands::handleGetStatus() {
  // Utilisation d'un document dynamique : ~25 champs
  JsonDocument doc;
  doc["type"] = "status";
  JsonObject d = doc["data"].to<JsonObject>();

  // Capteurs
  float ph = sensors.getPh();
  float orp = sensors.getOrp();
  float temp = sensors.getTemperature();

  if (!isnan(ph)) {
    d["ph"] = round(ph * 10.0f) / 10.0f;
  } else {
    d["ph"] = nullptr;
  }
  if (!isnan(orp)) {
    d["orp"] = round(orp);
  } else {
    d["orp"] = nullptr;
  }
  if (!isnan(temp)) {
    d["temperature"] = round(temp * 10.0f) / 10.0f;
  } else {
    d["temperature"] = nullptr;
  }

  // Filtration
  d["filtration_running"] = filtration.isRunning();
  d["filtration_mode"] = filtrationCfg.mode;

  // Dosage
  d["ph_dosing"] = PumpController.isPhDosing();
  d["orp_dosing"] = PumpController.isOrpDosing();
  d["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  d["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  d["ph_limit_reached"] = safetyLimits.phLimitReached;
  d["orp_limit_reached"] = safetyLimits.orpLimitReached;

  // Éclairage
  d["lighting_on"] = lighting.isOn();

  // Réseau
  d["wifi_connected"] = WiFi.isConnected();
  if (WiFi.isConnected()) {
    d["wifi_ssid"] = WiFi.SSID();
    d["wifi_ip"] = WiFi.localIP().toString();
    d["wifi_rssi"] = WiFi.RSSI();
  } else {
    d["wifi_ssid"] = nullptr;
    d["wifi_ip"] = nullptr;
    d["wifi_rssi"] = nullptr;
  }
  d["mqtt_connected"] = mqttManager.isConnected();

  // Heure système
  time_t nowEpoch = time(nullptr);
  bool timeSynced = (nowEpoch > 1609459200);
  d["time_synced"] = timeSynced;
  if (timeSynced) {
    char timeBuf[32];
    struct tm ti;
    localtime_r(&nowEpoch, &ti);
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &ti);
    d["time_current"] = timeBuf;
  } else {
    d["time_current"] = nullptr;
  }

  uartProtocol.sendJson(doc);
}

void UartCommands::handleGetConfig() {
  // Document dynamique : ~35 champs
  JsonDocument doc;
  doc["type"] = "config";
  JsonObject d = doc["data"].to<JsonObject>();

  // Régulation
  d["ph_target"] = mqttCfg.phTarget;
  d["orp_target"] = mqttCfg.orpTarget;
  d["ph_enabled"] = mqttCfg.phEnabled;
  d["orp_enabled"] = mqttCfg.orpEnabled;
  d["ph_pump"] = mqttCfg.phPump;
  d["orp_pump"] = mqttCfg.orpPump;
  d["regulation_mode"] = mqttCfg.regulationMode;
  d["ph_correction_type"] = mqttCfg.phCorrectionType;
  d["ph_injection_limit_s"] = mqttCfg.phInjectionLimitSeconds;
  d["orp_injection_limit_s"] = mqttCfg.orpInjectionLimitSeconds;

  // Filtration
  d["filtration_enabled"] = filtrationCfg.enabled;
  d["filtration_mode"] = filtrationCfg.mode;
  d["filtration_start"] = filtrationCfg.start;
  d["filtration_end"] = filtrationCfg.end;

  // Éclairage
  d["lighting_feature_enabled"] = lightingCfg.featureEnabled;
  d["lighting_enabled"] = lightingCfg.enabled;
  d["lighting_brightness"] = lightingCfg.brightness;
  d["lighting_schedule_enabled"] = lightingCfg.scheduleEnabled;
  d["lighting_start_time"] = lightingCfg.startTime;
  d["lighting_end_time"] = lightingCfg.endTime;

  // Limites de sécurité
  d["max_ph_ml_per_day"] = safetyLimits.maxPhMinusMlPerDay;
  d["max_chlorine_ml_per_day"] = safetyLimits.maxChlorineMlPerDay;

  // Temps / NTP
  d["time_use_ntp"] = mqttCfg.timeUseNtp;
  d["ntp_server"] = mqttCfg.ntpServer;
  d["timezone_id"] = mqttCfg.timezoneId;

  // Calibration
  d["ph_calibration_date"] = mqttCfg.phCalibrationDate;
  d["orp_calibration_date"] = mqttCfg.orpCalibrationDate;
  d["orp_calibration_offset"] = mqttCfg.orpCalibrationOffset;
  d["orp_calibration_slope"] = mqttCfg.orpCalibrationSlope;
  d["temp_calibration_date"] = mqttCfg.tempCalibrationDate;
  d["temp_calibration_offset"] = mqttCfg.tempCalibrationOffset;
  d["temperature_enabled"] = mqttCfg.temperatureEnabled;

  uartProtocol.sendJson(doc);
}

void UartCommands::handleGetAlarms() {
  JsonDocument doc;
  doc["type"] = "alarms";
  JsonArray arr = doc["data"].to<JsonArray>();

  if (safetyLimits.phLimitReached) {
    JsonObject a = arr.add<JsonObject>();
    a["code"] = "PH_LIMIT";
    a["message"] = "Limite journalière pH atteinte";
  }
  if (safetyLimits.orpLimitReached) {
    JsonObject a = arr.add<JsonObject>();
    a["code"] = "ORP_LIMIT";
    a["message"] = "Limite journalière ORP/chlore atteinte";
  }

  // Valeurs capteurs aberrantes
  float ph = sensors.getPh();
  float orp = sensors.getOrp();
  float temp = sensors.getTemperature();

  if (!isnan(ph) && (ph < 5.0f || ph > 9.0f)) {
    JsonObject a = arr.add<JsonObject>();
    a["code"] = "PH_ABNORMAL";
    a["message"] = "Valeur pH anormale: " + String(ph, 1);
  }
  if (!isnan(orp) && (orp < 400.0f || orp > 900.0f)) {
    JsonObject a = arr.add<JsonObject>();
    a["code"] = "ORP_ABNORMAL";
    a["message"] = "Valeur ORP anormale: " + String(orp, 0) + " mV";
  }
  if (!isnan(temp) && (temp < 5.0f || temp > 40.0f)) {
    JsonObject a = arr.add<JsonObject>();
    a["code"] = "TEMP_ABNORMAL";
    a["message"] = "Température anormale: " + String(temp, 1) + " °C";
  }

  uartProtocol.sendJson(doc);
}

void UartCommands::handleGetNetworkStatus() {
  StaticJson<256> doc;
  doc["type"] = "network_status";
  JsonObject d = doc["data"].to<JsonObject>();

  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_STA) {
    d["wifi_mode"] = "STA";
  } else if (mode == WIFI_MODE_AP) {
    d["wifi_mode"] = "AP";
  } else if (mode == WIFI_MODE_APSTA) {
    d["wifi_mode"] = "AP+STA";
  } else {
    d["wifi_mode"] = "OFF";
  }

  d["connected"] = WiFi.isConnected();
  d["ssid"] = WiFi.isConnected() ? WiFi.SSID() : String("");
  d["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  d["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  d["mqtt_connected"] = mqttManager.isConnected();

  uartProtocol.sendJson(doc);
}

// ---------------------------------------------------------------------------
// Commandes d'écriture
// ---------------------------------------------------------------------------

void UartCommands::handleSetConfig(JsonVariant data) {
  if (data.isNull() || !data.is<JsonObject>()) {
    uartProtocol.sendError("set_config", "missing data object");
    return;
  }

  bool changed = false;
  String errorMsg;

  if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    uartProtocol.sendError("set_config", "config busy, retry");
    return;
  }

  // --- Régulation pH ---
  if (data["ph_target"].is<float>() || data["ph_target"].is<int>()) {
    float v = data["ph_target"].as<float>();
    if (v < 5.0f || v > 9.0f) {
      errorMsg = "ph_target hors plage [5.0-9.0]";
    } else {
      mqttCfg.phTarget = v;
      changed = true;
    }
  }
  if (data["orp_target"].is<float>() || data["orp_target"].is<int>()) {
    float v = data["orp_target"].as<float>();
    if (v < 200.0f || v > 900.0f) {
      if (errorMsg.isEmpty()) errorMsg = "orp_target hors plage [200-900]";
    } else {
      mqttCfg.orpTarget = v;
      changed = true;
    }
  }
  if (data["ph_enabled"].is<bool>()) {
    mqttCfg.phEnabled = data["ph_enabled"].as<bool>();
    changed = true;
  }
  if (data["orp_enabled"].is<bool>()) {
    mqttCfg.orpEnabled = data["orp_enabled"].as<bool>();
    changed = true;
  }
  if (data["regulation_mode"].is<const char*>()) {
    String v = data["regulation_mode"].as<String>();
    if (v == "continu" || v == "pilote") {
      mqttCfg.regulationMode = v;
      changed = true;
    } else {
      if (errorMsg.isEmpty()) errorMsg = "regulation_mode invalide";
    }
  }
  if (data["ph_correction_type"].is<const char*>()) {
    String v = data["ph_correction_type"].as<String>();
    if (v == "ph_minus" || v == "ph_plus") {
      mqttCfg.phCorrectionType = v;
      changed = true;
    } else {
      if (errorMsg.isEmpty()) errorMsg = "ph_correction_type invalide";
    }
  }

  // --- Filtration ---
  if (data["filtration_mode"].is<const char*>()) {
    String v = data["filtration_mode"].as<String>();
    if (v == "auto" || v == "manual" || v == "off") {
      filtrationCfg.mode = v;
      changed = true;
    } else {
      if (errorMsg.isEmpty()) errorMsg = "filtration_mode invalide";
    }
  }
  if (data["filtration_enabled"].is<bool>()) {
    filtrationCfg.enabled = data["filtration_enabled"].as<bool>();
    changed = true;
  }
  if (data["filtration_start"].is<const char*>()) {
    filtrationCfg.start = data["filtration_start"].as<String>();
    changed = true;
  }
  if (data["filtration_end"].is<const char*>()) {
    filtrationCfg.end = data["filtration_end"].as<String>();
    changed = true;
  }

  // --- Éclairage ---
  if (data["lighting_enabled"].is<bool>()) {
    lightingCfg.enabled = data["lighting_enabled"].as<bool>();
    changed = true;
  }
  if (data["lighting_brightness"].is<int>()) {
    int v = constrain(data["lighting_brightness"].as<int>(), 0, 255);
    lightingCfg.brightness = static_cast<uint8_t>(v);
    changed = true;
  }
  if (data["lighting_schedule_enabled"].is<bool>()) {
    lightingCfg.scheduleEnabled = data["lighting_schedule_enabled"].as<bool>();
    changed = true;
  }
  if (data["lighting_start_time"].is<const char*>()) {
    lightingCfg.startTime = data["lighting_start_time"].as<String>();
    changed = true;
  }
  if (data["lighting_end_time"].is<const char*>()) {
    lightingCfg.endTime = data["lighting_end_time"].as<String>();
    changed = true;
  }

  // --- Limites de sécurité ---
  if (data["max_ph_ml_per_day"].is<float>() || data["max_ph_ml_per_day"].is<int>()) {
    float v = data["max_ph_ml_per_day"].as<float>();
    if (v > 0.0f) {
      safetyLimits.maxPhMinusMlPerDay = v;
      changed = true;
    } else {
      if (errorMsg.isEmpty()) errorMsg = "max_ph_ml_per_day doit être > 0";
    }
  }
  if (data["max_chlorine_ml_per_day"].is<float>() || data["max_chlorine_ml_per_day"].is<int>()) {
    float v = data["max_chlorine_ml_per_day"].as<float>();
    if (v > 0.0f) {
      safetyLimits.maxChlorineMlPerDay = v;
      changed = true;
    } else {
      if (errorMsg.isEmpty()) errorMsg = "max_chlorine_ml_per_day doit être > 0";
    }
  }

  // --- Temps / NTP ---
  if (data["time_use_ntp"].is<bool>()) {
    mqttCfg.timeUseNtp = data["time_use_ntp"].as<bool>();
    changed = true;
  }
  if (data["ntp_server"].is<const char*>()) {
    String v = data["ntp_server"].as<String>();
    if (v.length() > 0 && v.length() <= 64) {
      mqttCfg.ntpServer = v;
      changed = true;
    }
  }
  if (data["timezone_id"].is<const char*>()) {
    String v = data["timezone_id"].as<String>();
    if (v.length() > 0 && v.length() <= 64) {
      mqttCfg.timezoneId = v;
      changed = true;
    }
  }

  xSemaphoreGiveRecursive(configMutex);

  if (!errorMsg.isEmpty()) {
    uartProtocol.sendError("set_config", errorMsg);
    return;
  }
  if (!changed) {
    uartProtocol.sendError("set_config", "no valid field found");
    return;
  }

  saveMqttConfig();
  uartProtocol.sendAck("set_config");
}

void UartCommands::handleSaveConfig() {
  saveMqttConfig();
  uartProtocol.sendAck("save_config");
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void UartCommands::handleRunAction(JsonVariant data) {
  if (data.isNull() || !data["action"].is<const char*>()) {
    uartProtocol.sendError("run_action", "missing action field");
    return;
  }

  const char* action = data["action"];

  if (strcmp(action, "pump_test") == 0) {
    actionPumpTest(data);
  } else if (strcmp(action, "pump_stop") == 0) {
    actionPumpStop(data);
  } else if (strcmp(action, "lighting_on") == 0) {
    actionLightingOn();
  } else if (strcmp(action, "lighting_off") == 0) {
    actionLightingOff();
  } else if (strcmp(action, "filtration_mode") == 0) {
    actionFiltrationMode(data);
  } else if (strcmp(action, "calibrate_ph_neutral") == 0) {
    actionCalibratePhNeutral();
  } else if (strcmp(action, "calibrate_ph_acid") == 0) {
    actionCalibratePhAcid();
  } else if (strcmp(action, "clear_ph_calibration") == 0) {
    actionClearPhCalibration();
  } else if (strcmp(action, "ack_alarm") == 0) {
    actionAckAlarm(data);
  } else {
    uartProtocol.sendError("run_action", String("unknown action: ") + action);
  }
}

void UartCommands::actionPumpTest(JsonVariant data) {
  if (!data["pump"].is<int>()) {
    uartProtocol.sendError("run_action", "pump_test: missing pump (1 or 2)");
    return;
  }
  int pumpNum = data["pump"].as<int>();
  if (pumpNum != 1 && pumpNum != 2) {
    uartProtocol.sendError("run_action", "pump_test: pump must be 1 or 2");
    return;
  }
  int duty = 255;
  if (data["duty"].is<int>()) {
    duty = constrain(data["duty"].as<int>(), 0, 255);
  }
  int idx = pumpNum - 1;
  PumpController.setManualPump(idx, static_cast<uint8_t>(duty));
  uartProtocol.sendAck("run_action");
}

void UartCommands::actionPumpStop(JsonVariant data) {
  if (!data["pump"].is<int>()) {
    uartProtocol.sendError("run_action", "pump_stop: missing pump (1 or 2)");
    return;
  }
  int pumpNum = data["pump"].as<int>();
  if (pumpNum != 1 && pumpNum != 2) {
    uartProtocol.sendError("run_action", "pump_stop: pump must be 1 or 2");
    return;
  }
  PumpController.setManualPump(pumpNum - 1, 0);
  uartProtocol.sendAck("run_action");
}

void UartCommands::actionLightingOn() {
  lighting.setManualOn();
  uartProtocol.sendAck("run_action");
}

void UartCommands::actionLightingOff() {
  lighting.setManualOff();
  uartProtocol.sendAck("run_action");
}

void UartCommands::actionFiltrationMode(JsonVariant data) {
  if (!data["mode"].is<const char*>()) {
    uartProtocol.sendError("run_action", "filtration_mode: missing mode (auto/manual/off)");
    return;
  }
  String mode = data["mode"].as<String>();
  if (mode != "auto" && mode != "manual" && mode != "off") {
    uartProtocol.sendError("run_action", "filtration_mode: mode invalide");
    return;
  }
  filtrationCfg.mode = mode;
  saveMqttConfig();
  uartProtocol.sendAck("run_action");
  // Notifier l'écran du changement de mode
  uartProtocol.sendEventStr("event", "mode_changed", "mode", mode);
}

void UartCommands::actionCalibratePhNeutral() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    uartProtocol.sendError("run_action", "calibrate_ph_neutral: bus I2C occupé");
    return;
  }
  sensors.calibratePhNeutral();
  xSemaphoreGive(i2cMutex);
  uartProtocol.sendAck("run_action");
  // L'event calibration_done est envoyé depuis web_routes_calibration aussi,
  // mais ici on l'envoie directement
  StaticJson<96> doc;
  doc["type"] = "event";
  doc["event"] = "calibration_done";
  doc["data"]["sensor"] = "ph_neutral";
  uartProtocol.sendJson(doc);
}

void UartCommands::actionCalibratePhAcid() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    uartProtocol.sendError("run_action", "calibrate_ph_acid: bus I2C occupé");
    return;
  }
  sensors.calibratePhAcid();
  xSemaphoreGive(i2cMutex);
  uartProtocol.sendAck("run_action");
  StaticJson<96> doc;
  doc["type"] = "event";
  doc["event"] = "calibration_done";
  doc["data"]["sensor"] = "ph_acid";
  uartProtocol.sendJson(doc);
}

void UartCommands::actionClearPhCalibration() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
    uartProtocol.sendError("run_action", "clear_ph_calibration: bus I2C occupé");
    return;
  }
  sensors.clearPhCalibration();
  xSemaphoreGive(i2cMutex);
  uartProtocol.sendAck("run_action");
}

// ---------------------------------------------------------------------------
// Assistant de mise en service (wizard IHM écran)
// ---------------------------------------------------------------------------

void UartCommands::handleGetSetupStatus() {
  StaticJson<128> doc;
  doc["type"] = "setup_status";
  JsonObject d = doc["data"].to<JsonObject>();
  d["wizard_completed"] = authCfg.wizardCompleted;
  d["first_boot"]       = authManager.isFirstBootDetected();
  uartProtocol.sendJson(doc);
}

void UartCommands::handleCompleteWizard() {
  // Marque le wizard comme terminé et persiste en NVS
  authManager.clearFirstBootFlag();
  uartProtocol.sendAck("complete_wizard");
}

// ---------------------------------------------------------------------------
// Commandes wizard écran : réseau et sécurité
// ---------------------------------------------------------------------------

void UartCommands::handleWifiScan() {
  // Scan asynchrone : vTaskDelay cède le CPU entre chaque sondage,
  // évitant de bloquer Core 1 (pompes, capteurs) pendant le scan.
  WiFi.scanNetworks(true, false);  // async=true, show_hidden=false

  unsigned long scanStart = millis();
  int n = WIFI_SCAN_RUNNING;
  while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
    if (millis() - scanStart > 6000) {
      WiFi.scanDelete();
      uartProtocol.sendError("wifi_scan", "scan timeout");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (n == WIFI_SCAN_FAILED) {
    uartProtocol.sendError("wifi_scan", "scan failed");
    return;
  }

  JsonDocument doc;
  doc["type"] = "wifi_scan_result";
  JsonArray networks = doc["data"]["networks"].to<JsonArray>();

  for (int i = 0; i < n && i < 20; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"]   = WiFi.SSID(i);
    net["rssi"]   = WiFi.RSSI(i);
    net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  uartProtocol.sendJson(doc);
}

void UartCommands::handleWifiConnect(JsonVariant data) {
  if (data.isNull() || !data["ssid"].is<const char*>()) {
    uartProtocol.sendError("wifi_connect", "missing ssid");
    return;
  }
  const char* ssid     = data["ssid"];
  const char* password = data["password"] | "";

  WiFi.begin(ssid, strlen(password) > 0 ? password : nullptr);
  uartProtocol.sendAck("wifi_connect");
}

void UartCommands::handleChangePassword(JsonVariant data) {
  if (data.isNull() || !data["new_password"].is<const char*>()) {
    uartProtocol.sendError("change_password", "missing new_password");
    return;
  }
  const char* newPwd = data["new_password"];
  if (strlen(newPwd) < 8) {
    uartProtocol.sendError("change_password", "password too short (min 8 chars)");
    return;
  }
  authCfg.adminPassword = String(newPwd);
  authManager.setPassword(authCfg.adminPassword);
  authManager.regenerateApiToken();
  authCfg.apiToken = authManager.getApiToken();
  saveMqttConfig();
  uartProtocol.sendAck("change_password");
}

// ---------------------------------------------------------------------------

void UartCommands::actionAckAlarm(JsonVariant data) {
  if (!data["code"].is<const char*>()) {
    uartProtocol.sendError("run_action", "ack_alarm: missing code");
    return;
  }
  const char* code = data["code"];

  if (strcmp(code, "PH_LIMIT") == 0) {
    safetyLimits.phLimitReached = false;
    uartProtocol.sendAck("run_action");
    uartProtocol.sendAlarmCleared("PH_LIMIT");
  } else if (strcmp(code, "ORP_LIMIT") == 0) {
    safetyLimits.orpLimitReached = false;
    uartProtocol.sendAck("run_action");
    uartProtocol.sendAlarmCleared("ORP_LIMIT");
  } else {
    uartProtocol.sendError("run_action", String("ack_alarm: code inconnu: ") + code);
  }
}
