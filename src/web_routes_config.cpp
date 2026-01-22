#include "web_routes_config.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "sensors.h"
#include "filtration.h"
#include "lighting.h"
#include "mqtt_manager.h"
#include "pump_controller.h"
#include "logger.h"
#include "version.h"
#include "json_compat.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <AsyncJson.h>
#include <esp_ota_ops.h>

// Fonction externe déclarée dans main.cpp pour effacer les credentials WiFi
extern void resetWiFiSettings();

// Contexte pour les buffers de configuration (partagé avec web_server)
static std::map<AsyncWebServerRequest*, std::vector<uint8_t>>* g_configBuffers = nullptr;
static std::map<AsyncWebServerRequest*, bool>* g_configErrors = nullptr;

// Variables globales pour la reconnexion WiFi asynchrone
static bool g_wifiReconnectRequested = false;
static String g_wifiReconnectSsid = "";
static String g_wifiReconnectPassword = "";
static unsigned long g_wifiReconnectRequestedTime = 0;

static bool wifiConfigAllowed() {
  wifi_mode_t mode = WiFi.getMode();
  // Autoriser la configuration WiFi uniquement en mode AP (pas de WiFi configuré)
  // ou si on n'est pas connecté en mode STA
  if (mode == WIFI_MODE_AP) {
    return true;
  }
  // En mode STA ou APSTA, autoriser seulement si pas connecté
  return !WiFi.isConnected();
}

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
  StaticJson<2048> doc;
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
  doc["lighting_schedule_enabled"] = lightingCfg.scheduleEnabled;
  doc["lighting_start_time"] = lightingCfg.startTime;
  doc["lighting_end_time"] = lightingCfg.endTime;
  doc["wifi_ssid"] = WiFi.SSID();

  // Déterminer l'IP à afficher selon le mode WiFi
  wifi_mode_t mode = WiFi.getMode();
  String ipAddress;
  if (mode == WIFI_MODE_AP) {
    // En mode AP uniquement, afficher l'IP de l'AP
    ipAddress = WiFi.softAPIP().toString();
  } else if (mode == WIFI_MODE_APSTA) {
    // En mode AP+STA, afficher l'IP STA si connecté, sinon l'IP AP
    if (WiFi.isConnected()) {
      ipAddress = WiFi.localIP().toString();
    } else {
      ipAddress = WiFi.softAPIP().toString();
    }
  } else {
    // En mode STA, afficher l'IP STA
    ipAddress = WiFi.localIP().toString();
  }
  doc["wifi_ip"] = ipAddress;

  doc["wifi_mode"] = mode == WIFI_MODE_AP ? "AP" : (mode == WIFI_MODE_APSTA ? "AP+STA" : "STA");
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

  // Options de développement
  doc["sensor_logs_enabled"] = authCfg.sensorLogsEnabled;

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
  if (!doc["lighting_schedule_enabled"].isNull()) lightingCfg.scheduleEnabled = doc["lighting_schedule_enabled"];
  if (!doc["lighting_start_time"].isNull()) lightingCfg.startTime = doc["lighting_start_time"].as<String>();
  if (!doc["lighting_end_time"].isNull()) lightingCfg.endTime = doc["lighting_end_time"].as<String>();

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

  // Options de développement
  if (!doc["sensor_logs_enabled"].isNull()) {
    authCfg.sensorLogsEnabled = doc["sensor_logs_enabled"];
    systemLogger.info(String("Logs des sondes: ") + (authCfg.sensorLogsEnabled ? "activés" : "désactivés"));
  }

  // Note: Le token API n'est pas modifiable via /save-config
  // Il doit être regénéré via une route dédiée pour éviter les modifications accidentelles

  sanitizePumpSelection();
  filtration.ensureTimesValid();
  lighting.ensureTimesValid();
  ensureTimezoneValid();
  applyTimezoneEnv();

  if (filtrationCfg.mode.equalsIgnoreCase("auto")) {
    filtration.computeAutoSchedule();
  }

  // Appliquer immédiatement la nouvelle configuration de filtration
  filtration.update();

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
  StaticJson<128> doc;
  doc["time"] = getCurrentTimeISO();
  doc["time_use_ntp"] = mqttCfg.timeUseNtp;
  doc["timezone_id"] = mqttCfg.timezoneId;

  sendJsonResponse(request, doc);
}

static void handleRebootAp(AsyncWebServerRequest* request);  // Forward declaration

static void handleGetSystemInfo(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Buffer statique : ~24 champs (version, chip, memory, WiFi, uptime) = 1024 bytes
  StaticJson<1024> doc;

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

  // Routes Wi-Fi publiques (utilisées depuis la page de login en mode AP)
  server->on("/wifi/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    wifi_mode_t mode = WiFi.getMode();
    String modeLabel = (mode == WIFI_MODE_AP) ? "AP" : (mode == WIFI_MODE_APSTA ? "AP+STA" : "STA");
    doc["mode"] = modeLabel;
    doc["connected"] = WiFi.isConnected();
    doc["ssid"] = WiFi.SSID();
    doc["ap_ssid"] = WiFi.softAPSSID();
    doc["ap_ip"] = WiFi.softAPIP().toString();
    sendJsonResponse(request, doc);
  });

  server->on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Permettre le scan si:
    // 1. wifiConfigAllowed() == true (mode AP ou WiFi non connecté) OU
    // 2. L'utilisateur est authentifié (accès depuis les paramètres)
    bool allowed = wifiConfigAllowed();
    if (!allowed) {
      // Vérifier l'authentification pour les utilisateurs connectés en WiFi
      if (!authManager.checkAuth(request, RouteProtection::WRITE)) {
        return;
      }
    }

    if (!authManager.checkRateLimit(request)) {
      authManager.sendRateLimitExceeded(request);
      return;
    }

    int count = WiFi.scanNetworks(false, true);
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
      JsonObject item = networks.add<JsonObject>();
      item["ssid"] = WiFi.SSID(i);
      item["rssi"] = WiFi.RSSI(i);
      item["channel"] = WiFi.channel(i);
      item["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    sendJsonResponse(request, doc);
  });

  auto* wifiConnectHandler = new AsyncCallbackJsonWebHandler(
    "/wifi/connect",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      // Permettre la configuration WiFi si:
      // 1. wifiConfigAllowed() == true (mode AP ou WiFi non connecté) OU
      // 2. L'utilisateur est authentifié (accès depuis les paramètres)
      bool allowed = wifiConfigAllowed();
      if (!allowed) {
        // Vérifier l'authentification pour les utilisateurs connectés en WiFi
        if (!authManager.checkAuth(request, RouteProtection::WRITE)) {
          return;
        }
      }
      if (!authManager.checkRateLimit(request)) {
        authManager.sendRateLimitExceeded(request);
        return;
      }

      JsonObject root = json.as<JsonObject>();
      String ssid = root["ssid"] | "";
      String password = root["password"] | "";

      if (ssid.isEmpty()) {
        sendErrorResponse(request, 400, "SSID required");
        return;
      }

      // Enregistrer les credentials pour reconnexion asynchrone
      systemLogger.info("Configuration WiFi demandée depuis l'UI: " + ssid);
      g_wifiReconnectSsid = ssid;
      g_wifiReconnectPassword = password;
      g_wifiReconnectRequested = true;
      g_wifiReconnectRequestedTime = millis();

      // Retourner immédiatement une réponse (la connexion se fera de manière asynchrone)
      JsonDocument doc;
      doc["accepted"] = true;
      doc["message"] = "WiFi connection request accepted, connecting asynchronously";
      sendJsonResponse(request, doc);
    });
  wifiConnectHandler->setMethod(HTTP_POST);
  server->addHandler(wifiConnectHandler);

  // Route: Déconnecter du WiFi et effacer les credentials
  server->on("/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest* request) {
    // Permettre la déconnexion WiFi si:
    // 1. wifiConfigAllowed() == true (mode AP ou WiFi non connecté) OU
    // 2. L'utilisateur est authentifié (accès depuis les paramètres)
    bool allowed = wifiConfigAllowed();
    if (!allowed) {
      // Vérifier l'authentification pour les utilisateurs connectés en WiFi
      if (!authManager.checkAuth(request, RouteProtection::CRITICAL)) {
        return;
      }
    }

    systemLogger.info("Déconnexion WiFi demandée depuis l'UI");

    // Déconnecter et effacer les credentials WiFi
    WiFi.disconnect(true, true);  // disconnect(wifioff=true, eraseap=true)
    delay(100);

    // Repasser en mode AP uniquement
    WiFi.mode(WIFI_MODE_AP);

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "WiFi disconnected and credentials erased";
    sendJsonResponse(request, doc);
  });

  server->on("/wifi/ap/disable", HTTP_POST, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::CRITICAL);

    // Vérifier qu'il y a des credentials WiFi configurés dans la NVS
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    String ssid = String((char*)wifi_config.sta.ssid);

    if (ssid.length() == 0) {
      sendErrorResponse(request, 400, "No WiFi credentials configured");
      return;
    }

    // Activer le flag pour désactiver l'AP au prochain démarrage
    authCfg.disableApOnBoot = true;
    authCfg.forceWifiConfig = false;
    saveMqttConfig();

    if (WiFi.isConnected()) {
      systemLogger.info("Flag disableApOnBoot activé - WiFi connecté - Redémarrage programmé");
    } else {
      systemLogger.info("Flag disableApOnBoot activé - WiFi configuré mais pas encore connecté - Redémarrage programmé");
    }

    // Envoyer la réponse avant le redémarrage
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "ESP32 redémarrage en mode STA uniquement";
    doc["restarting"] = true;
    sendJsonResponse(request, doc);

    // Redémarrer après un court délai pour permettre à la réponse d'être envoyée
    delay(500);
    ESP.restart();
  });

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

// Fonction pour traiter les reconnexions WiFi asynchrones
// À appeler régulièrement depuis loop() pour ne pas bloquer le serveur web
void processWifiReconnectIfNeeded() {
  // Si aucune reconnexion n'est demandée, rien à faire
  if (!g_wifiReconnectRequested) {
    return;
  }

  // Attendre un petit délai pour que la réponse HTTP soit bien envoyée
  if (millis() - g_wifiReconnectRequestedTime < 100) {
    return;
  }

  // Marquer comme traité pour éviter de refaire plusieurs fois
  g_wifiReconnectRequested = false;

  systemLogger.info("Démarrage reconnexion WiFi asynchrone: " + g_wifiReconnectSsid);

  // Déterminer le mode WiFi actuel
  wifi_mode_t mode = WiFi.getMode();
  wifi_mode_t initialMode = mode; // Sauvegarder le mode initial pour y revenir en cas d'échec

  unsigned long startTime = millis();
  unsigned long timeout = 15000; // Timeout de 15 secondes pour la connexion initiale

  // Gestion des modes WiFi selon le contexte :
  // - Mode AP uniquement : passer en APSTA pour garder l'AP actif pendant la connexion (wizard/première config)
  // - Mode STA : si déjà connecté, déconnecter d'abord pour forcer la sauvegarde NVS
  // - Mode APSTA : si déjà connecté, passer en STA (sortir du mode secours), sinon conserver APSTA (connexion en cours)
  if (mode == WIFI_MODE_AP) {
    systemLogger.info("Mode AP détecté, passage temporaire en APSTA pour garder l'AP actif");
    WiFi.mode(WIFI_AP_STA);
  } else if (mode == WIFI_MODE_STA) {
    // En mode STA, si on est déjà connecté, il faut déconnecter pour forcer la sauvegarde NVS
    if (WiFi.isConnected()) {
      systemLogger.info("Mode STA avec connexion active, déconnexion pour forcer la sauvegarde NVS");
      WiFi.disconnect(false, false); // Déconnecter sans éteindre le WiFi ni effacer la config NVS

      // Attendre la déconnexion effective
      startTime = millis();
      while (WiFi.isConnected() && (millis() - startTime) < timeout) {
        delay(100);
      }
      delay(200); // Laisser le temps à la déconnexion de se stabiliser
    } else {
      systemLogger.info("Mode STA sans connexion, prêt pour nouvelle connexion");
    }
  } else if (mode == WIFI_MODE_APSTA) {
    // Si on est déjà connecté en APSTA (mode secours après échecs), passer en STA pour la nouvelle connexion
    // Si on n'est pas encore connecté, c'est qu'on est en train de se connecter (wizard), garder APSTA
    if (WiFi.isConnected()) {
      systemLogger.info("Mode APSTA détecté avec connexion active (mode secours), passage en STA pour nouvelle connexion");
      WiFi.disconnect(false, false); // Déconnecter sans effacer la config

      // Attendre la déconnexion effective (WiFi.disconnect n'est pas synchrone)
      startTime = millis();
      while (WiFi.isConnected() && (millis() - startTime) < timeout) {
        delay(100);
      }

      WiFi.mode(WIFI_MODE_STA);
      delay(200); // Laisser le temps au changement de mode de s'appliquer
    } else {
      systemLogger.info("Mode APSTA détecté sans connexion (configuration initiale), on conserve APSTA");
      // Ne rien faire, garder le mode APSTA pendant la connexion initiale
    }
  }

  // Lancer la connexion WiFi (en mode STA ou APSTA selon le cas)
  systemLogger.info("=== DEBUG Reconnexion WiFi ===");
  systemLogger.info("SSID: '" + g_wifiReconnectSsid + "' (longueur: " + String(g_wifiReconnectSsid.length()) + ")");
  systemLogger.info("Password: '" + g_wifiReconnectPassword + "' (longueur: " + String(g_wifiReconnectPassword.length()) + ")");
  systemLogger.info("==============================");

  // Sauvegarder les anciens credentials AVANT de tenter la connexion
  // Car WiFi.begin() peut corrompre la NVS même avec persistent(false)
  wifi_config_t old_wifi_config;
  memset(&old_wifi_config, 0, sizeof(wifi_config_t));
  esp_wifi_get_config(WIFI_IF_STA, &old_wifi_config);
  String oldSsid = String((char*)old_wifi_config.sta.ssid);
  systemLogger.info("Anciens credentials sauvegardés: SSID='" + oldSsid + "'");

  // Démarrer la connexion WiFi SANS persistence automatique
  // On sauvegarde manuellement dans la NVS seulement si la connexion réussit
  systemLogger.info("Tentative de connexion WiFi (sauvegarde NVS uniquement si succès)...");

  // Désactiver la persistence automatique pour éviter d'écraser les anciens credentials
  WiFi.persistent(false);

  // Tenter la connexion au nouveau réseau
  WiFi.begin(g_wifiReconnectSsid.c_str(), g_wifiReconnectPassword.c_str());

  startTime = millis();

  while (!WiFi.isConnected() && (millis() - startTime) < timeout) {
    delay(100);
  }

  if (WiFi.isConnected()) {
    systemLogger.info("Connexion WiFi réussie! IP: " + WiFi.localIP().toString());

    // Sauvegarder les nouveaux credentials dans la NVS
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    strncpy((char*)wifi_config.sta.ssid, g_wifiReconnectSsid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, g_wifiReconnectPassword.c_str(), sizeof(wifi_config.sta.password) - 1);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err == ESP_OK) {
      systemLogger.info("Credentials WiFi sauvegardés dans la NVS");
    } else {
      systemLogger.error("Erreur sauvegarde NVS: " + String(esp_err_to_name(err)));
    }
  } else {
    systemLogger.warning("Échec connexion WiFi - restauration des anciens credentials dans la NVS");

    // Restaurer les anciens credentials dans la NVS
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &old_wifi_config);
    if (err == ESP_OK) {
      systemLogger.info("Anciens credentials restaurés: SSID='" + oldSsid + "'");
    } else {
      systemLogger.error("Erreur restauration NVS: " + String(esp_err_to_name(err)));
    }

    // Si on était en mode AP au départ, revenir en mode AP (pas APSTA)
    if (initialMode == WIFI_MODE_AP) {
      systemLogger.info("Retour au mode AP après échec de connexion");
      WiFi.mode(WIFI_MODE_AP);
      delay(200);
    }
  }

  // Effacer les credentials pour la sécurité
  g_wifiReconnectSsid = "";
  g_wifiReconnectPassword = "";
}
