#include "web_server.h"
#include "config.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include "mqtt_manager.h"
#include "history.h"
#include "version.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPAsyncWiFiManager.h>
#include <Update.h>
#include <esp_ota_ops.h>

WebServerManager webServer;

WebServerManager::WebServerManager() : server(80), dns(nullptr) {}

bool WebServerManager::validatePhValue(float value) {
  return value >= 0.0f && value <= 14.0f;
}

bool WebServerManager::validateOrpValue(float value) {
  return value >= 0.0f && value <= 2000.0f;
}

bool WebServerManager::validateInjectionLimit(int seconds) {
  return seconds >= 0 && seconds <= 3600;
}

bool WebServerManager::validatePumpNumber(int pump) {
  return pump == 1 || pump == 2;
}

void WebServerManager::begin(DNSServer* dnsServer) {
  dns = dnsServer;

  // Ajouter les en-têtes CORS pour toutes les requêtes
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  setupRoutes();
  server.begin();
  systemLogger.info("Serveur Web démarré sur port 80");
}

void WebServerManager::setupRoutes() {
  // Handler CORS OPTIONS pour toutes les routes
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) {
      req->send(200);
    } else {
      req->send(404, "text/plain", "Not found");
    }
  });

  server.on("/data", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetData(req); });
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/config.html", "text/html");
  });
  server.on("/get-config", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetConfig(req); });
  server.on("/get-logs", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetLogs(req); });
  server.on("/time-now", HTTP_GET, [this](AsyncWebServerRequest *req) { handleTimeNow(req); });
  server.on("/reboot-ap", HTTP_POST, [this](AsyncWebServerRequest *req) { handleRebootAp(req); });
  server.on("/export-csv", HTTP_GET, [this](AsyncWebServerRequest *req) { handleExportCsv(req); });
  server.on("/get-system-info", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetSystemInfo(req); });

  server.on("/save-config", HTTP_POST,
    [](AsyncWebServerRequest *req) { req->send(200, "text/plain", "OK"); },
    nullptr,
    [this](AsyncWebServerRequest *req, uint8_t* data, size_t len, size_t, size_t) {
      handleSaveConfig(req, data, len, 0, 0);
    }
  );

  // Routes de calibration pH (DFRobot SEN0161-V2)
  server.on("/calibrate_ph_neutral", HTTP_POST, [this](AsyncWebServerRequest *req) {
    sensors.calibratePhNeutral();

    // Obtenir la date/heure actuelle au format ISO 8601
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      char buffer[25];
      strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
      mqttCfg.phCalibrationDate = String(buffer);
    } else {
      // Fallback: utiliser un timestamp si l'heure n'est pas disponible
      mqttCfg.phCalibrationDate = String(millis());
    }

    mqttCfg.phCalibrationTemp = sensors.getTemperature();
    saveMqttConfig();

    DynamicJsonDocument doc(128);
    doc["success"] = true;
    doc["temperature"] = mqttCfg.phCalibrationTemp;
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  server.on("/calibrate_ph_acid", HTTP_POST, [this](AsyncWebServerRequest *req) {
    sensors.calibratePhAcid();

    // Obtenir la date/heure actuelle au format ISO 8601
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      char buffer[25];
      strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
      mqttCfg.phCalibrationDate = String(buffer);
    } else {
      // Fallback: utiliser un timestamp si l'heure n'est pas disponible
      mqttCfg.phCalibrationDate = String(millis());
    }

    mqttCfg.phCalibrationTemp = sensors.getTemperature();
    saveMqttConfig();

    DynamicJsonDocument doc(128);
    doc["success"] = true;
    doc["temperature"] = mqttCfg.phCalibrationTemp;
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  server.on("/clear_ph_calibration", HTTP_POST, [this](AsyncWebServerRequest *req) {
    sensors.clearPhCalibration();
    mqttCfg.phCalibrationDate = "";
    mqttCfg.phCalibrationTemp = NAN;
    saveMqttConfig();

    DynamicJsonDocument doc(64);
    doc["success"] = true;
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  // Routes pour test manuel des pompes
  server.on("/pump1/on", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(0, MAX_PWM_DUTY); // Pompe 1 à fond
    req->send(200, "text/plain", "OK");
  });

  server.on("/pump1/off", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(0, 0); // Pompe 1 arrêtée
    req->send(200, "text/plain", "OK");
  });

  server.on("/pump2/on", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(1, MAX_PWM_DUTY); // Pompe 2 à fond
    req->send(200, "text/plain", "OK");
  });

  server.on("/pump2/off", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(1, 0); // Pompe 2 arrêtée
    req->send(200, "text/plain", "OK");
  });

  // Handler générique pour les routes avec paramètres dans l'URL
  server.onNotFound([this](AsyncWebServerRequest *req) {
    String url = req->url();

    // Gérer /pump1/duty/:duty
    if (req->method() == HTTP_POST && url.startsWith("/pump1/duty/")) {
      String dutyStr = url.substring(12); // Après "/pump1/duty/"
      int duty = dutyStr.toInt();
      if (duty < 0) duty = 0;
      if (duty > 255) duty = 255;
      PumpController.setManualPump(0, duty);
      req->send(200, "text/plain", "OK");
      return;
    }

    // Gérer /pump2/duty/:duty
    if (req->method() == HTTP_POST && url.startsWith("/pump2/duty/")) {
      String dutyStr = url.substring(12); // Après "/pump2/duty/"
      int duty = dutyStr.toInt();
      if (duty < 0) duty = 0;
      if (duty > 255) duty = 255;
      PumpController.setManualPump(1, duty);
      req->send(200, "text/plain", "OK");
      return;
    }

    // 404 pour les autres routes non trouvées
    req->send(404, "text/plain", "Not Found");
  });

  // Route pour mise à jour OTA (firmware ou filesystem)
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      bool success = !Update.hasError();
      AsyncWebServerResponse *response = req->beginResponse(200, "text/plain", success ? "OK" : "FAIL");
      response->addHeader("Connection", "close");
      req->send(response);
      if (success) {
        delay(1000);
        ESP.restart();
      }
    },
    [this](AsyncWebServerRequest *req, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      handleOtaUpdate(req, filename, index, data, len, final);
    }
  );

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

void WebServerManager::handleGetData(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(512);

  // ORP
  if (!isnan(sensors.getOrp())) {
    doc["orp"] = sensors.getOrp();
  } else {
    doc["orp"] = nullptr;
  }

  // pH
  if (!isnan(sensors.getPh())) {
    doc["ph"] = round(sensors.getPh() * 10.0f) / 10.0f;
  } else {
    doc["ph"] = nullptr;
  }

  // ORP raw
  if (!isnan(sensors.getRawOrp())) {
    doc["orp_raw"] = sensors.getRawOrp();
  } else {
    doc["orp_raw"] = nullptr;
  }

  // pH raw
  if (!isnan(sensors.getRawPh())) {
    doc["ph_raw"] = round(sensors.getRawPh() * 10.0f) / 10.0f;
  } else {
    doc["ph_raw"] = nullptr;
  }

  // Température
  if (!isnan(sensors.getTemperature())) {
    doc["temperature"] = sensors.getTemperature();
  } else {
    doc["temperature"] = nullptr;
  }
  doc["filtration_running"] = filtration.isRunning();
  doc["ph_dosing"] = PumpController.isPhDosing();
  doc["orp_dosing"] = PumpController.isOrpDosing();
  doc["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  doc["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  doc["ph_limit_reached"] = safetyLimits.phLimitReached;
  doc["orp_limit_reached"] = safetyLimits.orpLimitReached;

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleGetConfig(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(1024);
  doc["server"] = mqttCfg.server;
  doc["port"] = mqttCfg.port;
  doc["topic"] = mqttCfg.topic;
  doc["username"] = mqttCfg.username;
  // SÉCURITÉ: Ne jamais envoyer le mot de passe en clair
  doc["password"] = mqttCfg.password.length() > 0 ? "******" : "";
  doc["enabled"] = mqttCfg.enabled;
  doc["mqtt_connected"] = mqttManager.isConnected();
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
  doc["timezone_id"] = mqttCfg.timezoneId;
  doc["filtration_mode"] = filtrationCfg.mode;
  doc["filtration_start"] = filtrationCfg.start;
  doc["filtration_end"] = filtrationCfg.end;
  doc["filtration_has_reference"] = filtrationCfg.hasAutoReference;
  doc["filtration_reference_temp"] = filtrationCfg.autoReferenceTemp;
  doc["filtration_running"] = filtration.isRunning();
  doc["wifi_ssid"] = WiFi.SSID();
  doc["wifi_ip"] = WiFi.localIP().toString();
  doc["max_ph_ml_per_day"] = safetyLimits.maxPhMinusMlPerDay;
  doc["max_chlorine_ml_per_day"] = safetyLimits.maxChlorineMlPerDay;
  doc["ph_sensor_pin"] = PH_SENSOR_PIN;
  doc["orp_sensor_pin"] = ORP_SENSOR_PIN;

  // Données de calibration pH (DFRobot_PH)
  doc["ph_calibration_date"] = mqttCfg.phCalibrationDate;
  if (!isnan(mqttCfg.phCalibrationTemp)) {
    doc["ph_calibration_temp"] = mqttCfg.phCalibrationTemp;
  }
  doc["ph_cal_valid"] = !mqttCfg.phCalibrationDate.isEmpty();

  // Données de calibration ORP (1 ou 2 points)
  doc["orp_calibration_offset"] = mqttCfg.orpCalibrationOffset;
  doc["orp_calibration_slope"] = mqttCfg.orpCalibrationSlope;
  doc["orp_calibration_date"] = mqttCfg.orpCalibrationDate;
  doc["orp_calibration_reference"] = mqttCfg.orpCalibrationReference;
  if (!isnan(mqttCfg.orpCalibrationTemp)) {
    doc["orp_calibration_temp"] = mqttCfg.orpCalibrationTemp;
  }

  // Heure actuelle (pour l'affichage dans la configuration)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    doc["time_current"] = String(buffer);
  } else {
    doc["time_current"] = "unavailable";
  }

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleSaveConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "text/plain", "Invalid JSON");
    systemLogger.error("Configuration JSON invalide reçue");
    return;
  }

  // Validation et application avec logs
  if (doc.containsKey("server")) mqttCfg.server = doc["server"].as<String>();
  if (doc.containsKey("port")) mqttCfg.port = doc["port"];
  if (doc.containsKey("topic")) mqttCfg.topic = doc["topic"].as<String>();
  if (doc.containsKey("username")) mqttCfg.username = doc["username"].as<String>();

  // Mot de passe: ne mettre à jour que s'il n'est pas masqué
  if (doc.containsKey("password")) {
    String pwd = doc["password"].as<String>();
    if (pwd != "******" && pwd.length() > 0) {
      mqttCfg.password = pwd;
    }
  }

  if (doc.containsKey("enabled")) mqttCfg.enabled = doc["enabled"];

  // Validation pH target
  if (doc.containsKey("ph_target")) {
    float phTarget = doc["ph_target"];
    if (validatePhValue(phTarget)) {
      mqttCfg.phTarget = phTarget;
    } else {
      systemLogger.warning("Valeur pH target invalide ignorée: " + String(phTarget));
    }
  }

  // Validation ORP target
  if (doc.containsKey("orp_target")) {
    float orpTarget = doc["orp_target"];
    if (validateOrpValue(orpTarget)) {
      mqttCfg.orpTarget = orpTarget;
    } else {
      systemLogger.warning("Valeur ORP target invalide ignorée: " + String(orpTarget));
    }
  }

  if (doc.containsKey("ph_enabled")) mqttCfg.phEnabled = doc["ph_enabled"];
  if (doc.containsKey("orp_enabled")) mqttCfg.orpEnabled = doc["orp_enabled"];

  // Validation pump numbers
  if (doc.containsKey("ph_pump")) {
    int pump = doc["ph_pump"];
    if (validatePumpNumber(pump)) {
      mqttCfg.phPump = pump;
    }
  }
  if (doc.containsKey("orp_pump")) {
    int pump = doc["orp_pump"];
    if (validatePumpNumber(pump)) {
      mqttCfg.orpPump = pump;
    }
  }

  // Validation injection limits
  if (doc.containsKey("ph_limit_seconds")) {
    int limit = doc["ph_limit_seconds"];
    if (validateInjectionLimit(limit)) {
      mqttCfg.phInjectionLimitSeconds = limit;
    }
  }
  if (doc.containsKey("orp_limit_seconds")) {
    int limit = doc["orp_limit_seconds"];
    if (validateInjectionLimit(limit)) {
      mqttCfg.orpInjectionLimitSeconds = limit;
    }
  }

  if (doc.containsKey("time_use_ntp")) mqttCfg.timeUseNtp = doc["time_use_ntp"];
  if (doc.containsKey("ntp_server")) mqttCfg.ntpServer = doc["ntp_server"].as<String>();
  if (doc.containsKey("timezone_id")) mqttCfg.timezoneId = doc["timezone_id"].as<String>();
  if (doc.containsKey("filtration_mode")) filtrationCfg.mode = doc["filtration_mode"].as<String>();
  if (doc.containsKey("filtration_start")) filtrationCfg.start = doc["filtration_start"].as<String>();
  if (doc.containsKey("filtration_end")) filtrationCfg.end = doc["filtration_end"].as<String>();

  // Limites de sécurité
  if (doc.containsKey("max_ph_ml_per_day")) {
    safetyLimits.maxPhMinusMlPerDay = doc["max_ph_ml_per_day"];
  }
  if (doc.containsKey("max_chlorine_ml_per_day")) {
    safetyLimits.maxChlorineMlPerDay = doc["max_chlorine_ml_per_day"];
  }

  // Pins des capteurs sont maintenant des defines (PH_SENSOR_PIN, ORP_SENSOR_PIN)

  // Données de calibration pH (DFRobot_PH gère en EEPROM)
  if (doc.containsKey("ph_calibration_date")) {
    mqttCfg.phCalibrationDate = doc["ph_calibration_date"].as<String>();
  }
  if (doc.containsKey("ph_calibration_temp")) {
    mqttCfg.phCalibrationTemp = doc["ph_calibration_temp"];
  }

  // Données de calibration ORP (1 ou 2 points)
  if (doc.containsKey("orp_calibration_offset")) {
    mqttCfg.orpCalibrationOffset = doc["orp_calibration_offset"];
  }
  if (doc.containsKey("orp_calibration_slope")) {
    mqttCfg.orpCalibrationSlope = doc["orp_calibration_slope"];
  }
  if (doc.containsKey("orp_calibration_date")) {
    mqttCfg.orpCalibrationDate = doc["orp_calibration_date"].as<String>();
  }
  if (doc.containsKey("orp_calibration_reference")) {
    mqttCfg.orpCalibrationReference = doc["orp_calibration_reference"];
  }
  if (doc.containsKey("orp_calibration_temp")) {
    mqttCfg.orpCalibrationTemp = doc["orp_calibration_temp"];
  }

  sanitizePumpSelection();
  filtration.ensureTimesValid();
  ensureTimezoneValid();
  applyTimezoneEnv();

  if (filtrationCfg.mode.equalsIgnoreCase("auto")) {
    filtration.computeAutoSchedule();
  }

  PumpController.resetDosingStates();
  saveMqttConfig();
  mqttManager.requestReconnect();

  systemLogger.info("Configuration mise à jour via interface web");
  request->send(200, "text/plain", "Configuration saved");
}

void WebServerManager::handleGetLogs(AsyncWebServerRequest* request) {
  auto logs = systemLogger.getRecentLogs(50);
  DynamicJsonDocument doc(4096);
  JsonArray logsArray = doc.createNestedArray("logs");

  for (const auto& entry : logs) {
    JsonObject logObj = logsArray.createNestedObject();
    logObj["timestamp"] = entry.timestamp;
    logObj["level"] = systemLogger.getLevelString(entry.level);
    logObj["message"] = entry.message;
  }

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleTimeNow(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(256);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    doc["time"] = String(buffer);
  } else {
    doc["time"] = "unavailable";
  }
  doc["time_use_ntp"] = mqttCfg.timeUseNtp;
  doc["timezone_id"] = mqttCfg.timezoneId;

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleRebootAp(AsyncWebServerRequest* request) {
  restartApRequested = true;
  request->send(200, "text/plain", "Restart scheduled");
  systemLogger.warning("Redémarrage en mode AP demandé");
}

void WebServerManager::handleExportCsv(AsyncWebServerRequest* request) {
  String csv;
  history.exportCSV(csv);

  // Générer un nom de fichier avec timestamp
  struct tm timeinfo;
  char filename[64];
  if (getLocalTime(&timeinfo, 0)) {
    strftime(filename, sizeof(filename), "pool_history_%Y%m%d_%H%M%S.csv", &timeinfo);
  } else {
    snprintf(filename, sizeof(filename), "pool_history.csv");
  }

  // Envoyer avec headers pour téléchargement
  AsyncWebServerResponse* response = request->beginResponse(200, "text/csv", csv);
  response->addHeader("Content-Disposition", "attachment; filename=\"" + String(filename) + "\"");
  request->send(response);

  systemLogger.info("Export CSV généré: " + String(filename));
}

void WebServerManager::handleGetSystemInfo(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(2048);

  // Version firmware
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["build_date"] = FIRMWARE_BUILD_DATE;
  doc["build_time"] = FIRMWARE_BUILD_TIME;

  // Informations ESP32
  doc["chip_model"] = ESP.getChipModel();
  doc["chip_revision"] = ESP.getChipRevision();
  doc["chip_cores"] = ESP.getChipCores();
  doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();

  // Mémoire
  doc["free_heap"] = ESP.getFreeHeap();
  doc["heap_size"] = ESP.getHeapSize();
  doc["free_psram"] = ESP.getFreePsram();
  doc["psram_size"] = ESP.getPsramSize();

  // Flash
  doc["flash_size"] = ESP.getFlashChipSize();
  doc["flash_speed"] = ESP.getFlashChipSpeed();

  // Partition OTA
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    doc["ota_partition"] = running->label;
    doc["ota_partition_size"] = running->size;
  }

  // Système de fichiers
  doc["fs_total_bytes"] = LittleFS.totalBytes();
  doc["fs_used_bytes"] = LittleFS.usedBytes();
  doc["fs_free_bytes"] = LittleFS.totalBytes() - LittleFS.usedBytes();

  // WiFi
  doc["wifi_ssid"] = WiFi.SSID();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["wifi_ip"] = WiFi.localIP().toString();
  doc["wifi_mac"] = WiFi.macAddress();

  // Uptime
  unsigned long uptime = millis() / 1000;
  doc["uptime_seconds"] = uptime;
  doc["uptime_days"] = uptime / 86400;
  doc["uptime_hours"] = (uptime % 86400) / 3600;
  doc["uptime_minutes"] = (uptime % 3600) / 60;

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleOtaUpdate(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (!index) {
    systemLogger.info("Début mise à jour OTA: " + filename);

    // Déterminer le type de mise à jour
    int cmd = U_FLASH; // Par défaut: firmware

    // Si le fichier se termine par .bin.gz ou .bin, c'est le firmware
    // Si le fichier se termine par .littlefs.bin ou .spiffs.bin, c'est le filesystem
    if (filename.endsWith(".littlefs.bin") || filename.endsWith(".spiffs.bin") || filename.endsWith(".fs.bin")) {
      cmd = U_SPIFFS;
      systemLogger.info("Type de mise à jour: Filesystem");
    } else {
      systemLogger.info("Type de mise à jour: Firmware");
    }

    // Démarrer la mise à jour
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      Update.printError(Serial);
      systemLogger.error("Erreur démarrage OTA: " + String(Update.errorString()));
    }
  }

  // Écrire les données
  if (len) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      systemLogger.error("Erreur écriture OTA");
    } else {
      // Log progression toutes les 100KB
      static size_t lastLog = 0;
      if (index - lastLog >= 102400) {
        unsigned int percent = (index + len) * 100 / Update.size();
        systemLogger.info("Progression OTA: " + String(percent) + "%");
        lastLog = index;
      }
    }
  }

  // Finaliser
  if (final) {
    if (Update.end(true)) {
      systemLogger.info("Mise à jour OTA réussie. Redémarrage...");
    } else {
      Update.printError(Serial);
      systemLogger.error("Erreur finalisation OTA: " + String(Update.errorString()));
    }
  }
}

void WebServerManager::update() {
  if (restartApRequested) {
    restartApRequested = false;
    systemLogger.critical("Redémarrage en mode Point d'accès");
    delay(1000);
    ESP.restart();
  }
}
