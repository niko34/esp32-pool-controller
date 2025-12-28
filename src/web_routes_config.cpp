#include "web_routes_config.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "sensors.h"
#include "filtration.h"
#include "mqtt_manager.h"
#include "pump_controller.h"
#include "logger.h"
#include "version.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

// Fonction externe déclarée dans main.cpp pour effacer les credentials WiFi
extern void resetWiFiSettings();

// Contexte pour les buffers de configuration (partagé avec web_server)
static std::map<AsyncWebServerRequest*, std::vector<uint8_t>>* g_configBuffers = nullptr;
static std::map<AsyncWebServerRequest*, bool>* g_configErrors = nullptr;

void initConfigContext(
  std::map<AsyncWebServerRequest*, std::vector<uint8_t>>* configBuffers,
  std::map<AsyncWebServerRequest*, bool>* configErrors
) {
  g_configBuffers = configBuffers;
  g_configErrors = configErrors;
}

static void handleGetConfig(AsyncWebServerRequest* request) {
  // Vérifier si l'utilisateur est authentifié pour accéder aux données sensibles
  // Note: On ne bloque PAS la requête (route publique pour premier démarrage),
  // mais on vérifie l'authentification pour savoir quelles données révéler
  // On utilise checkTokenAuth/checkBasicAuth directement pour ne pas bloquer avec sendAuthRequired()
  bool isAuthenticated = authManager.checkTokenAuth(request) || authManager.checkBasicAuth(request);

  // Buffer statique : ~53 champs (configs MQTT, pH, ORP, calibration, WiFi, etc.) = 2048 bytes
  StaticJsonDocument<2048> doc;
  doc["server"] = mqttCfg.server;
  doc["port"] = mqttCfg.port;
  doc["topic"] = mqttCfg.topic;
  doc["username"] = mqttCfg.username;
  // SÉCURITÉ: Ne jamais envoyer les mots de passe en clair (même si authentifié)
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

  // Données de calibration pH
  doc["ph_calibration_date"] = mqttCfg.phCalibrationDate;
  if (!isnan(mqttCfg.phCalibrationTemp)) {
    doc["ph_calibration_temp"] = mqttCfg.phCalibrationTemp;
  }

  // Données de calibration ORP
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

  // Configuration d'authentification
  doc["auth_enabled"] = authCfg.enabled;

  // SÉCURITÉ: Masquer les credentials si non authentifié
  if (isAuthenticated) {
    // Utilisateur authentifié : montrer password et token masqués (indication qu'ils existent)
    doc["auth_password"] = authCfg.adminPassword.length() > 0 ? "******" : "";
    doc["auth_token"] = authCfg.apiToken.length() > 8 ? (authCfg.apiToken.substring(0, 8) + "...") : "";
    doc["auth_cors_origins"] = authCfg.corsAllowedOrigins; // Montrer la config CORS complète
  } else {
    // Utilisateur non authentifié : ne pas révéler si credentials sont configurés
    doc["auth_password"] = "******";
    doc["auth_token"] = "********...";
    doc["auth_cors_origins"] = ""; // Masquer la config CORS
  }

  // Heure actuelle (pour l'affichage dans la configuration)
  doc["time_current"] = getCurrentTimeISO();

  sendJsonResponse(request, doc);
}

static void handleSaveConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  if (g_configBuffers == nullptr || g_configErrors == nullptr) {
    systemLogger.error("Config context non initialisé!");
    return;
  }

  // Accumuler les données chunkées
  if (index == 0) {
    // Premier chunk, créer ou réinitialiser le buffer
    (*g_configBuffers)[request].clear();
    (*g_configErrors)[request] = false; // Pas d'erreur pour l'instant

    // Vérifier la taille totale
    if (total > kMaxConfigSizeBytes) {
      systemLogger.error("Configuration trop volumineuse: " + String(total) + " bytes (max " + String(kMaxConfigSizeBytes) + ")");
      (*g_configErrors)[request] = true;
      return;
    }

    if (total > 0) {
      (*g_configBuffers)[request].reserve(total);
    }
  }

  // Ajouter les données au buffer
  if (len > 0) {
    (*g_configBuffers)[request].insert((*g_configBuffers)[request].end(), data, data + len);
  }

  // Si ce n'est pas le dernier chunk, ne rien faire
  if (index + len < total) {
    return;
  }

  // Dernier chunk reçu, traiter toutes les données accumulées
  std::vector<uint8_t>& buffer = (*g_configBuffers)[request];

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buffer.data(), buffer.size());

  if (error != DeserializationError::Ok) {
    systemLogger.error("Configuration JSON invalide reçue: " + String(error.c_str()));
    (*g_configErrors)[request] = true; // Marquer l'erreur
    return; // Le handler principal renverra 400
  }

  // Protéger l'accès concurrent aux configurations (web async vs loop)
  if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.error("Timeout acquisition configMutex dans handleSaveConfig");
    (*g_configErrors)[request] = true;
    return;
  }

  // Validation et application avec logs
  if (!doc["server"].isNull()) mqttCfg.server = doc["server"].as<String>();
  if (!doc["port"].isNull()) mqttCfg.port = doc["port"];
  if (!doc["topic"].isNull()) mqttCfg.topic = doc["topic"].as<String>();
  if (!doc["username"].isNull()) mqttCfg.username = doc["username"].as<String>();

  // Mot de passe: ne mettre à jour que si différent de "******"
  if (!doc["password"].isNull()) {
    String receivedPassword = doc["password"].as<String>();
    if (receivedPassword != "******") {
      mqttCfg.password = receivedPassword;
    }
  }

  if (!doc["enabled"].isNull()) mqttCfg.enabled = doc["enabled"];
  if (!doc["ph_target"].isNull()) mqttCfg.phTarget = doc["ph_target"];
  if (!doc["orp_target"].isNull()) mqttCfg.orpTarget = doc["orp_target"];
  if (!doc["ph_enabled"].isNull()) mqttCfg.phEnabled = doc["ph_enabled"];
  if (!doc["orp_enabled"].isNull()) mqttCfg.orpEnabled = doc["orp_enabled"];
  if (!doc["ph_pump"].isNull()) mqttCfg.phPump = doc["ph_pump"];
  if (!doc["orp_pump"].isNull()) mqttCfg.orpPump = doc["orp_pump"];
  if (!doc["ph_limit_seconds"].isNull()) mqttCfg.phInjectionLimitSeconds = doc["ph_limit_seconds"];
  if (!doc["orp_limit_seconds"].isNull()) mqttCfg.orpInjectionLimitSeconds = doc["orp_limit_seconds"];
  if (!doc["time_use_ntp"].isNull()) mqttCfg.timeUseNtp = doc["time_use_ntp"];
  if (!doc["ntp_server"].isNull()) mqttCfg.ntpServer = doc["ntp_server"].as<String>();
  if (!doc["manual_time"].isNull()) mqttCfg.manualTimeIso = doc["manual_time"].as<String>();
  if (!doc["timezone_id"].isNull()) mqttCfg.timezoneId = doc["timezone_id"].as<String>();
  if (!doc["filtration_mode"].isNull()) filtrationCfg.mode = doc["filtration_mode"].as<String>();
  if (!doc["filtration_start"].isNull()) filtrationCfg.start = doc["filtration_start"].as<String>();
  if (!doc["filtration_end"].isNull()) filtrationCfg.end = doc["filtration_end"].as<String>();
  if (!doc["filtration_has_reference"].isNull()) filtrationCfg.hasAutoReference = doc["filtration_has_reference"];
  if (!doc["filtration_reference_temp"].isNull()) filtrationCfg.autoReferenceTemp = doc["filtration_reference_temp"];
  if (!doc["max_ph_ml_per_day"].isNull()) safetyLimits.maxPhMinusMlPerDay = doc["max_ph_ml_per_day"];
  if (!doc["max_chlorine_ml_per_day"].isNull()) safetyLimits.maxChlorineMlPerDay = doc["max_chlorine_ml_per_day"];
  if (!doc["lighting_enabled"].isNull()) lightingCfg.enabled = doc["lighting_enabled"];
  if (!doc["lighting_brightness"].isNull()) lightingCfg.brightness = doc["lighting_brightness"];

  // Calibration ORP
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

  // Configuration d'authentification
  if (!doc["auth_enabled"].isNull()) {
    authCfg.enabled = doc["auth_enabled"];
    authManager.setEnabled(authCfg.enabled);
  }

  // Mot de passe admin: ne mettre à jour que si différent de "******"
  if (!doc["auth_password"].isNull()) {
    String receivedPassword = doc["auth_password"].as<String>();
    if (receivedPassword != "******" && receivedPassword.length() > 0) {
      authCfg.adminPassword = receivedPassword;
      authManager.setPassword(authCfg.adminPassword);
      systemLogger.info("Mot de passe administrateur modifié");
    }
  }

  // Configuration CORS
  if (!doc["auth_cors_origins"].isNull()) {
    authCfg.corsAllowedOrigins = doc["auth_cors_origins"].as<String>();
    systemLogger.info("Configuration CORS mise à jour: " + authCfg.corsAllowedOrigins);
  }

  // Note: Le token API n'est pas modifiable via /save-config
  // Il doit être regénéré via une route dédiée pour éviter les modifications accidentelles

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

static void handleTimeNow(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Buffer statique : 3 champs (time, time_use_ntp, timezone_id) = 128 bytes
  StaticJsonDocument<128> doc;
  doc["time"] = getCurrentTimeISO();
  doc["time_use_ntp"] = mqttCfg.timeUseNtp;
  doc["timezone_id"] = mqttCfg.timezoneId;

  sendJsonResponse(request, doc);
}

static void handleRebootAp(AsyncWebServerRequest* request);  // Forward declaration

static void handleGetSystemInfo(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Buffer statique : ~24 champs (version, chip, memory, WiFi, uptime) = 1024 bytes
  StaticJsonDocument<1024> doc;

  // Version firmware
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["build_date"] = FIRMWARE_BUILD_DATE;
  doc["build_time"] = FIRMWARE_BUILD_TIME;

  // Informations ESP32
  doc["chip_model"] = ESP.getChipModel();
  doc["chip_revision"] = ESP.getChipRevision();
  doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["heap_size"] = ESP.getHeapSize();
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
  unsigned long uptime = millis() / kMillisToSeconds;
  doc["uptime_seconds"] = uptime;
  doc["uptime_days"] = uptime / 86400;
  doc["uptime_hours"] = (uptime % 86400) / kSecondsPerHour;
  doc["uptime_minutes"] = (uptime % kSecondsPerHour) / kSecondsPerMinute;

  sendJsonResponse(request, doc);
}

void setupConfigRoutes(AsyncWebServer* server, bool* restartApRequested, unsigned long* restartRequestedTime) {
  server->on("/get-config", HTTP_GET, handleGetConfig);
  server->on("/time-now", HTTP_GET, handleTimeNow);
  server->on("/get-system-info", HTTP_GET, handleGetSystemInfo);

  // Route /save-config - PROTÉGÉE (CRITICAL)
  server->on("/save-config", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      REQUIRE_AUTH(req, RouteProtection::CRITICAL);

      if (g_configErrors == nullptr || g_configBuffers == nullptr) {
        req->send(500, "text/plain", "Config context error");
        return;
      }

      // Vérifier si une erreur s'est produite pendant le traitement
      bool hasError = (*g_configErrors)[req];

      // Nettoyer les buffers et états
      g_configBuffers->erase(req);
      g_configErrors->erase(req);

      if (hasError) {
        req->send(400, "text/plain", "Invalid JSON configuration");
      } else {
        req->send(200, "text/plain", "OK");
      }
    },
    nullptr,
    handleSaveConfig
  );

  // Route reboot - PROTÉGÉE (CRITICAL) - Redémarrage normal
  server->on("/reboot", HTTP_POST, [restartApRequested, restartRequestedTime](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    *restartApRequested = true;
    *restartRequestedTime = millis();
    req->send(200, "text/plain", "Restart scheduled");
    systemLogger.warning("Redémarrage demandé depuis l'interface web");
  });

  // Route reboot-ap - PROTÉGÉE (CRITICAL)
  // Efface les credentials WiFi puis redémarre pour forcer le mode AP
  server->on("/reboot-ap", HTTP_POST, [restartApRequested, restartRequestedTime](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    systemLogger.warning("Redémarrage en mode AP demandé");

    // Effacer les credentials WiFi AVANT de planifier le redémarrage
    resetWiFiSettings();

    *restartApRequested = true;
    *restartRequestedTime = millis();
    req->send(200, "text/plain", "WiFi reset - AP mode will start after restart");
  });
}
