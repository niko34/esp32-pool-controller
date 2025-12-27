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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

WebServerManager webServer;

WebServerManager::WebServerManager() : server(nullptr), dns(nullptr) {}

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

void WebServerManager::begin(AsyncWebServer* webServer, DNSServer* dnsServer) {
  server = webServer;
  dns = dnsServer;

  if (!server) {
    systemLogger.error("WebServerManager: serveur web non fourni");
    return;
  }

  // Ajouter les en-têtes CORS pour toutes les requêtes
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  setupRoutes();

  // Note: Ne pas appeler server->begin() ici car AsyncWiFiManager
  // a déjà appelé begin() sur le serveur lors de l'autoConnect().
  // Appeler begin() deux fois peut causer des conflits.
  systemLogger.info("Routes du serveur Web configurées");
}

void WebServerManager::setupRoutes() {
  server->on("/data", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetData(req); });
  server->on("/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/config.html", "text/html");
  });
  server->on("/get-config", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetConfig(req); });
  server->on("/get-logs", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetLogs(req); });
  server->on("/get-history", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetHistory(req); });
  server->on("/time-now", HTTP_GET, [this](AsyncWebServerRequest *req) { handleTimeNow(req); });
  server->on("/reboot-ap", HTTP_POST, [this](AsyncWebServerRequest *req) { handleRebootAp(req); });
  server->on("/get-system-info", HTTP_GET, [this](AsyncWebServerRequest *req) { handleGetSystemInfo(req); });
  server->on("/check-update", HTTP_GET, [this](AsyncWebServerRequest *req) { handleCheckUpdate(req); });
  server->on("/download-update", HTTP_POST, [this](AsyncWebServerRequest *req) { handleDownloadUpdate(req); });

  server->on("/save-config", HTTP_POST,
    [this](AsyncWebServerRequest *req) {
      // Vérifier si une erreur s'est produite pendant le traitement
      bool hasError = configErrors[req];

      // Nettoyer les buffers et états
      configBuffers.erase(req);
      configErrors.erase(req);

      if (hasError) {
        req->send(400, "text/plain", "Invalid JSON configuration");
      } else {
        req->send(200, "text/plain", "OK");
      }
    },
    nullptr,
    [this](AsyncWebServerRequest *req, uint8_t* data, size_t len, size_t index, size_t total) {
      handleSaveConfig(req, data, len, index, total);
    }
  );

  // Routes de calibration pH (DFRobot SEN0161-V2)
  server->on("/calibrate_ph_neutral", HTTP_POST, [this](AsyncWebServerRequest *req) {
    // Protéger l'accès I2C (évite collision avec sensors.update())
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      req->send(503, "application/json", "{\"error\":\"I2C busy\"}");
      return;
    }

    sensors.calibratePhNeutral();
    xSemaphoreGive(i2cMutex);

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

    // Buffer statique : 1 champ (calibration_date) = 128 bytes
    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["temperature"] = mqttCfg.phCalibrationTemp;
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  server->on("/calibrate_ph_acid", HTTP_POST, [this](AsyncWebServerRequest *req) {
    // Protéger l'accès I2C (évite collision avec sensors.update())
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      req->send(503, "application/json", "{\"error\":\"I2C busy\"}");
      return;
    }

    sensors.calibratePhAcid();
    xSemaphoreGive(i2cMutex);

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

    // Buffer statique : 1 champ (calibration_date) = 128 bytes
    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["temperature"] = mqttCfg.phCalibrationTemp;
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  server->on("/clear_ph_calibration", HTTP_POST, [this](AsyncWebServerRequest *req) {
    sensors.clearPhCalibration();
    mqttCfg.phCalibrationDate = "";
    mqttCfg.phCalibrationTemp = NAN;
    saveMqttConfig();

    JsonDocument doc;
    doc["success"] = true;
    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
  });

  // Routes pour test manuel des pompes
  server->on("/pump1/on", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(0, MAX_PWM_DUTY); // Pompe 1 à fond
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump1/off", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(0, 0); // Pompe 1 arrêtée
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/on", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(1, MAX_PWM_DUTY); // Pompe 2 à fond
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/off", HTTP_POST, [this](AsyncWebServerRequest *req) {
    PumpController.setManualPump(1, 0); // Pompe 2 arrêtée
    req->send(200, "text/plain", "OK");
  });

  // Routes pour contrôle de l'éclairage (relais)
  server->on("/lighting/on", HTTP_POST, [this](AsyncWebServerRequest *req) { handleLightingOn(req); });
  server->on("/lighting/off", HTTP_POST, [this](AsyncWebServerRequest *req) { handleLightingOff(req); });

  // Handler global pour CORS OPTIONS et routes dynamiques
  server->onNotFound([this](AsyncWebServerRequest *req) {
    // Gérer CORS OPTIONS
    if (req->method() == HTTP_OPTIONS) {
      req->send(200);
      return;
    }

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

  // Route pour mise à jour OTA (firmware ou filesystem) - utilisée par l'interface web
  server->on("/update", HTTP_POST,
    [this](AsyncWebServerRequest *req) {
      bool success = !Update.hasError();
      AsyncWebServerResponse *response = req->beginResponse(200, "text/plain", success ? "OK" : "FAIL");
      response->addHeader("Connection", "close");
      req->send(response);
      if (success) {
        // Planifier le redémarrage dans update() pour ne pas bloquer
        restartRequested = true;
        restartRequestedTime = millis();
      }
    },
    [this](AsyncWebServerRequest *req, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      handleOtaUpdate(req, filename, index, data, len, final);
    }
  );

  server->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

void WebServerManager::handleGetData(AsyncWebServerRequest* request) {
  // Buffer statique pour éviter la fragmentation du heap
  // Taille estimée : ~13 champs × 30 bytes + overhead = 512 bytes
  StaticJsonDocument<512> doc;

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

  // Température brute (sans calibration)
  if (!isnan(sensors.getRawTemperature())) {
    doc["temperature_raw"] = sensors.getRawTemperature();
  } else {
    doc["temperature_raw"] = nullptr;
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
  // Buffer statique : ~53 champs (configs MQTT, pH, ORP, calibration, WiFi, etc.) = 2048 bytes
  StaticJsonDocument<2048> doc;
  doc["server"] = mqttCfg.server;
  doc["port"] = mqttCfg.port;
  doc["topic"] = mqttCfg.topic;
  doc["username"] = mqttCfg.username;
  // SÉCURITÉ: Ne jamais envoyer les mots de passe en clair
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
  doc["manual_time"] = mqttCfg.manualTimeIso;
  doc["timezone_id"] = mqttCfg.timezoneId;
  doc["filtration_mode"] = filtrationCfg.mode;
  doc["filtration_start"] = filtrationCfg.start;
  doc["filtration_end"] = filtrationCfg.end;
  doc["filtration_has_reference"] = filtrationCfg.hasAutoReference;
  doc["filtration_reference_temp"] = filtrationCfg.autoReferenceTemp;
  doc["filtration_running"] = filtration.isRunning();
  doc["lighting_enabled"] = lightingCfg.enabled;
  doc["lighting_brightness"] = lightingCfg.brightness;
  doc["wifi_ssid"] = WiFi.SSID();
  doc["wifi_ip"] = WiFi.localIP().toString();
  doc["wifi_mode"] = WiFi.getMode() == WIFI_MODE_AP ? "AP" : "STA";
  doc["mdns_host"] = "poolcontroller.local";
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

  // Données de calibration Température
  doc["temp_calibration_offset"] = mqttCfg.tempCalibrationOffset;
  doc["temp_calibration_date"] = mqttCfg.tempCalibrationDate;

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

void WebServerManager::handleSaveConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Limite de sécurité: 16KB max pour éviter épuisement RAM
  const size_t MAX_CONFIG_SIZE = 16384;

  // Accumuler les données chunkées
  if (index == 0) {
    // Premier chunk, créer ou réinitialiser le buffer
    configBuffers[request].clear();
    configErrors[request] = false; // Pas d'erreur pour l'instant

    // Vérifier la taille totale
    if (total > MAX_CONFIG_SIZE) {
      systemLogger.error("Configuration trop volumineuse: " + String(total) + " bytes (max " + String(MAX_CONFIG_SIZE) + ")");
      configErrors[request] = true;
      return;
    }

    if (total > 0) {
      configBuffers[request].reserve(total);
    }
  }

  // Ajouter les données au buffer
  if (len > 0) {
    configBuffers[request].insert(configBuffers[request].end(), data, data + len);
  }

  // Si ce n'est pas le dernier chunk, ne rien faire
  if (index + len < total) {
    return;
  }

  // Dernier chunk reçu, traiter toutes les données accumulées
  std::vector<uint8_t>& buffer = configBuffers[request];

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buffer.data(), buffer.size());

  if (error != DeserializationError::Ok) {
    systemLogger.error("Configuration JSON invalide reçue: " + String(error.c_str()));
    configErrors[request] = true; // Marquer l'erreur
    return; // Le handler principal renverra 400
  }

  // Protéger l'accès concurrent aux configurations (web async vs loop)
  if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    systemLogger.error("Timeout acquisition configMutex dans handleSaveConfig");
    configErrors[request] = true;
    return;
  }

  // Validation et application avec logs
  if (!doc["server"].isNull()) mqttCfg.server = doc["server"].as<String>();
  if (!doc["port"].isNull()) mqttCfg.port = doc["port"];
  if (!doc["topic"].isNull()) mqttCfg.topic = doc["topic"].as<String>();
  if (!doc["username"].isNull()) mqttCfg.username = doc["username"].as<String>();

  // Mots de passe: ne mettre à jour que s'ils ne sont pas masqués
  if (!doc["password"].isNull()) {
    String pwd = doc["password"].as<String>();
    if (pwd != "******" && pwd.length() > 0) {
      mqttCfg.password = pwd;
    }
  }

  if (!doc["enabled"].isNull()) mqttCfg.enabled = doc["enabled"];

  // Validation pH target
  if (!doc["ph_target"].isNull()) {
    float phTarget = doc["ph_target"];
    if (validatePhValue(phTarget)) {
      mqttCfg.phTarget = phTarget;
    } else {
      systemLogger.warning("Valeur pH target invalide ignorée: " + String(phTarget));
    }
  }

  // Validation ORP target
  if (!doc["orp_target"].isNull()) {
    float orpTarget = doc["orp_target"];
    if (validateOrpValue(orpTarget)) {
      mqttCfg.orpTarget = orpTarget;
    } else {
      systemLogger.warning("Valeur ORP target invalide ignorée: " + String(orpTarget));
    }
  }

  if (!doc["ph_enabled"].isNull()) mqttCfg.phEnabled = doc["ph_enabled"];
  if (!doc["orp_enabled"].isNull()) mqttCfg.orpEnabled = doc["orp_enabled"];

  // Validation pump numbers
  if (!doc["ph_pump"].isNull()) {
    int pump = doc["ph_pump"];
    if (validatePumpNumber(pump)) {
      mqttCfg.phPump = pump;
    }
  }
  if (!doc["orp_pump"].isNull()) {
    int pump = doc["orp_pump"];
    if (validatePumpNumber(pump)) {
      mqttCfg.orpPump = pump;
    }
  }

  // Validation injection limits
  if (!doc["ph_limit_seconds"].isNull()) {
    int limit = doc["ph_limit_seconds"];
    if (validateInjectionLimit(limit)) {
      mqttCfg.phInjectionLimitSeconds = limit;
    }
  }
  if (!doc["orp_limit_seconds"].isNull()) {
    int limit = doc["orp_limit_seconds"];
    if (validateInjectionLimit(limit)) {
      mqttCfg.orpInjectionLimitSeconds = limit;
    }
  }

  if (!doc["time_use_ntp"].isNull()) mqttCfg.timeUseNtp = doc["time_use_ntp"];
  if (!doc["ntp_server"].isNull()) mqttCfg.ntpServer = doc["ntp_server"].as<String>();
  if (!doc["manual_time"].isNull()) mqttCfg.manualTimeIso = doc["manual_time"].as<String>();
  if (!doc["timezone_id"].isNull()) mqttCfg.timezoneId = doc["timezone_id"].as<String>();
  if (!doc["filtration_mode"].isNull()) filtrationCfg.mode = doc["filtration_mode"].as<String>();
  if (!doc["filtration_start"].isNull()) filtrationCfg.start = doc["filtration_start"].as<String>();
  if (!doc["filtration_end"].isNull()) filtrationCfg.end = doc["filtration_end"].as<String>();

  // Limites de sécurité
  if (!doc["max_ph_ml_per_day"].isNull()) {
    safetyLimits.maxPhMinusMlPerDay = doc["max_ph_ml_per_day"];
  }
  if (!doc["max_chlorine_ml_per_day"].isNull()) {
    safetyLimits.maxChlorineMlPerDay = doc["max_chlorine_ml_per_day"];
  }

  // Pins des capteurs sont maintenant des defines (PH_SENSOR_PIN, ORP_SENSOR_PIN)

  // Données de calibration pH (DFRobot_PH gère en EEPROM)
  if (!doc["ph_calibration_date"].isNull()) {
    mqttCfg.phCalibrationDate = doc["ph_calibration_date"].as<String>();
  }
  if (!doc["ph_calibration_temp"].isNull()) {
    mqttCfg.phCalibrationTemp = doc["ph_calibration_temp"];
  }

  // Données de calibration ORP (1 ou 2 points)
  if (!doc["orp_calibration_offset"].isNull()) {
    mqttCfg.orpCalibrationOffset = doc["orp_calibration_offset"];
  }
  if (!doc["orp_calibration_slope"].isNull()) {
    mqttCfg.orpCalibrationSlope = doc["orp_calibration_slope"];
  }
  if (!doc["orp_calibration_date"].isNull()) {
    mqttCfg.orpCalibrationDate = doc["orp_calibration_date"].as<String>();
  }
  if (!doc["orp_calibration_reference"].isNull()) {
    mqttCfg.orpCalibrationReference = doc["orp_calibration_reference"];
  }
  if (!doc["orp_calibration_temp"].isNull()) {
    mqttCfg.orpCalibrationTemp = doc["orp_calibration_temp"];
  }

  // Données de calibration Température
  if (!doc["temp_calibration_offset"].isNull()) {
    mqttCfg.tempCalibrationOffset = doc["temp_calibration_offset"];
  }
  if (!doc["temp_calibration_date"].isNull()) {
    mqttCfg.tempCalibrationDate = doc["temp_calibration_date"].as<String>();
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

  // Recalculer les valeurs calibrées (température, ORP) avec les nouveaux offsets
  sensors.recalculateCalibratedValues();

  // Libérer le mutex
  xSemaphoreGive(configMutex);

  systemLogger.info("Configuration mise à jour via interface web");
  // La réponse sera envoyée par le handler principal
}

void WebServerManager::handleGetLogs(AsyncWebServerRequest* request) {
  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long sinceTimestamp = 0;
  if (request->hasParam("since")) {
    String sinceParam = request->getParam("since")->value();
    sinceTimestamp = sinceParam.toInt();
  }

  auto logs = systemLogger.getRecentLogs(50);
  JsonDocument doc;
  JsonArray logsArray = doc["logs"].to<JsonArray>();

  for (const auto& entry : logs) {
    // Si since est spécifié, filtrer les logs plus anciens
    if (sinceTimestamp > 0 && entry.timestamp <= sinceTimestamp) {
      continue;
    }

    JsonObject logObj = logsArray.add<JsonObject>();
    logObj["timestamp"] = entry.timestamp;
    logObj["level"] = systemLogger.getLevelString(entry.level);
    logObj["message"] = entry.message;
  }

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleGetHistory(AsyncWebServerRequest* request) {
  // Support paramètre optionnel ?range=24h|7d|30d|all
  String range = "all";
  if (request->hasParam("range")) {
    range = request->getParam("range")->value();
  }

  std::vector<DataPoint> data;

  if (range == "24h") {
    data = history.getLastHours(24);
  } else if (range == "7d") {
    data = history.getLastHours(24 * 7);
  } else if (range == "30d") {
    data = history.getLastHours(24 * 30);
  } else {
    data = history.getAllData();
  }

  JsonDocument doc;
  JsonArray historyArray = doc["history"].to<JsonArray>();

  for (const auto& point : data) {
    JsonObject obj = historyArray.add<JsonObject>();
    obj["timestamp"] = point.timestamp;

    if (!isnan(point.ph)) {
      obj["ph"] = round(point.ph * 10.0f) / 10.0f;
    } else {
      obj["ph"] = nullptr;
    }

    if (!isnan(point.orp)) {
      obj["orp"] = round(point.orp);
    } else {
      obj["orp"] = nullptr;
    }

    if (!isnan(point.temperature)) {
      obj["temperature"] = round(point.temperature * 10.0f) / 10.0f;
    } else {
      obj["temperature"] = nullptr;
    }

    obj["filtration"] = point.filtrationActive;
    obj["dosing"] = point.phDosing || point.orpDosing;
    obj["granularity"] = static_cast<uint8_t>(point.granularity);
  }

  doc["count"] = historyArray.size();
  doc["range"] = range;

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebServerManager::handleTimeNow(AsyncWebServerRequest* request) {
  // Buffer statique : 3 champs (time, time_use_ntp, timezone_id) = 128 bytes
  StaticJsonDocument<128> doc;
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
  restartRequestedTime = millis();
  request->send(200, "text/plain", "Restart scheduled");
  systemLogger.warning("Redémarrage en mode AP demandé");
}


void WebServerManager::handleGetSystemInfo(AsyncWebServerRequest* request) {
  // Buffer statique : ~24 champs (version, chip, memory, WiFi, uptime) = 1024 bytes
  StaticJsonDocument<1024> doc;

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

    // Si le fichier se termine par .littlefs.bin ou .spiffs.bin ou .fs.bin, c'est le filesystem
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
  // Gérer le redémarrage après OTA (attendre 3s pour que la réponse HTTP soit envoyée)
  if (restartRequested && (millis() - restartRequestedTime >= 3000)) {
    restartRequested = false;
    systemLogger.critical("Redémarrage après mise à jour OTA");
    ESP.restart();
  }

  // Gérer le redémarrage en mode AP (attendre 1s)
  if (restartApRequested && (millis() - restartRequestedTime >= 1000)) {
    restartApRequested = false;
    systemLogger.critical("Redémarrage en mode Point d'accès");
    ESP.restart();
  }
}

void WebServerManager::handleCheckUpdate(AsyncWebServerRequest* request) {
  systemLogger.info("Vérification des mises à jour GitHub...");

  WiFiClientSecure client;
  client.setInsecure(); // Pour GitHub HTTPS (pas de validation du certificat)

  HTTPClient https;

  // Utiliser l'API GitHub pour récupérer la dernière release
  const char* apiUrl = "https://api.github.com/repos/niko34/esp32-pool-controller/releases/latest";

  if (!https.begin(client, apiUrl)) {
    systemLogger.error("Impossible de se connecter à GitHub");
    request->send(500, "application/json", "{\"error\":\"Connection failed\"}");
    return;
  }

  https.addHeader("User-Agent", "ESP32-Pool-Controller");

  int httpCode = https.GET();

  if (httpCode != HTTP_CODE_OK) {
    systemLogger.error("Erreur HTTP GitHub: " + String(httpCode));
    https.end();

    // Cas spécial : 404 signifie qu'aucune release n'existe
    if (httpCode == 404) {
      JsonDocument response;
      response["current_version"] = FIRMWARE_VERSION;
      response["latest_version"] = FIRMWARE_VERSION;
      response["update_available"] = false;
      response["no_release"] = true;
      response["message"] = "Aucune release disponible sur GitHub";

      String json;
      serializeJson(response, json);

      systemLogger.info("Aucune release GitHub trouvée");
      request->send(200, "application/json", json);
      return;
    }

    // Autres erreurs HTTP
    String errorMsg = "{\"error\":\"GitHub API error\",\"code\":" + String(httpCode) + "}";
    request->send(500, "application/json", errorMsg);
    return;
  }

  String payload = https.getString();
  https.end();

  // Parser la réponse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    systemLogger.error("Erreur parsing JSON GitHub");
    request->send(500, "application/json", "{\"error\":\"JSON parse error\"}");
    return;
  }

  String latestVersion = doc["tag_name"].as<String>();
  String currentVersion = FIRMWARE_VERSION;

  // Retirer le 'v' si présent au début de la version GitHub
  if (latestVersion.startsWith("v") || latestVersion.startsWith("V")) {
    latestVersion = latestVersion.substring(1);
  }

  bool updateAvailable = (latestVersion != currentVersion);

  // Chercher les assets (firmware.bin et littlefs.bin)
  String firmwareUrl = "";
  String filesystemUrl = "";

  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"].as<String>();
    if (name == "firmware.bin") {
      firmwareUrl = asset["browser_download_url"].as<String>();
    } else if (name == "littlefs.bin") {
      filesystemUrl = asset["browser_download_url"].as<String>();
    }
  }

  // Construire la réponse
  JsonDocument response;
  response["current_version"] = currentVersion;
  response["latest_version"] = latestVersion;
  response["update_available"] = updateAvailable;
  response["release_name"] = doc["name"].as<String>();
  response["release_notes"] = doc["body"].as<String>();
  response["published_at"] = doc["published_at"].as<String>();
  response["firmware_url"] = firmwareUrl;
  response["filesystem_url"] = filesystemUrl;

  String json;
  serializeJson(response, json);

  systemLogger.info("Version actuelle: " + currentVersion + ", Dernière version: " + latestVersion);
  request->send(200, "application/json", json);
}

void WebServerManager::handleDownloadUpdate(AsyncWebServerRequest* request) {
  // Récupérer l'URL du fichier à télécharger
  if (!request->hasParam("url", true)) {
    request->send(400, "application/json", "{\"error\":\"Missing URL parameter\"}");
    return;
  }

  String url = request->getParam("url", true)->value();

  // Paramètre optionnel pour contrôler le redémarrage
  bool shouldRestart = true;
  if (request->hasParam("restart", true)) {
    String restartParam = request->getParam("restart", true)->value();
    shouldRestart = (restartParam == "true" || restartParam == "1");
  }

  // Déterminer le type de mise à jour en fonction de l'URL
  int cmd = U_FLASH; // Par défaut: firmware
  if (url.indexOf("littlefs") >= 0 || url.indexOf("filesystem") >= 0) {
    cmd = U_SPIFFS;
    systemLogger.info("Téléchargement mise à jour filesystem depuis GitHub");
  } else {
    systemLogger.info("Téléchargement mise à jour firmware depuis GitHub");
  }

  // Créer un client HTTPS
  WiFiClientSecure client;
  client.setInsecure(); // Pas de validation du certificat pour GitHub

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    systemLogger.error("Impossible de se connecter à GitHub pour téléchargement");
    request->send(500, "application/json", "{\"error\":\"Connection failed\"}");
    return;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    systemLogger.error("Erreur HTTP téléchargement: " + String(httpCode));
    http.end();
    request->send(500, "application/json", "{\"error\":\"Download failed\"}");
    return;
  }

  int contentLength = http.getSize();

  if (contentLength <= 0) {
    systemLogger.error("Taille fichier invalide");
    http.end();
    request->send(500, "application/json", "{\"error\":\"Invalid file size\"}");
    return;
  }

  systemLogger.info("Taille du fichier: " + String(contentLength) + " octets");

  // Démarrer la mise à jour OTA
  if (!Update.begin(contentLength, cmd)) {
    systemLogger.error("Erreur démarrage OTA: " + String(Update.errorString()));
    http.end();
    request->send(500, "application/json", "{\"error\":\"OTA begin failed\"}");
    return;
  }

  // Lire et écrire les données par blocs
  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[512];

  while (http.connected() && (written < contentLength)) {
    size_t available = stream->available();

    if (available) {
      int c = stream->readBytes(buff, min(available, sizeof(buff)));

      if (c > 0) {
        if (Update.write(buff, c) != c) {
          systemLogger.error("Erreur écriture OTA");
          Update.abort();
          http.end();
          request->send(500, "application/json", "{\"error\":\"OTA write failed\"}");
          return;
        }
        written += c;

        // Log de progression tous les 100KB
        if (written % 102400 == 0 || written == contentLength) {
          unsigned int percent = (written * 100) / contentLength;
          systemLogger.info("Téléchargement: " + String(percent) + "%");
        }
      }
    }
    delay(1);
  }

  http.end();

  // Finaliser la mise à jour
  if (Update.end(true)) {
    if (shouldRestart) {
      systemLogger.info("Mise à jour GitHub réussie! Redémarrage...");
      request->send(200, "application/json", "{\"status\":\"success\"}");
      // Planifier le redémarrage dans update() pour ne pas bloquer
      restartRequested = true;
      restartRequestedTime = millis();
    } else {
      systemLogger.info("Mise à jour GitHub réussie (sans redémarrage)");
      request->send(200, "application/json", "{\"status\":\"success\"}");
    }
  } else {
    systemLogger.error("Erreur finalisation OTA: " + String(Update.errorString()));
    request->send(500, "application/json", "{\"error\":\"OTA finalization failed\"}");
  }
}

void WebServerManager::handleLightingOn(AsyncWebServerRequest* request) {
  lightingCfg.enabled = true;
  digitalWrite(LIGHTING_RELAY_PIN, HIGH);
  saveMqttConfig();

  systemLogger.info("Éclairage activé");
  request->send(200, "text/plain", "OK");
}

void WebServerManager::handleLightingOff(AsyncWebServerRequest* request) {
  lightingCfg.enabled = false;
  digitalWrite(LIGHTING_RELAY_PIN, LOW);
  saveMqttConfig();

  systemLogger.info("Éclairage désactivé");
  request->send(200, "text/plain", "OK");
}
