#include "mqtt_manager.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "lighting.h"
#include "version.h"
#include "pump_controller.h"
#include <ArduinoJson.h>

MqttManager mqttManager;

MqttManager::MqttManager() : mqtt(wifiClient) {}

void MqttManager::begin() {
  mqtt.setServer(mqttCfg.server.c_str(), mqttCfg.port);
  mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
    mqttManager.messageCallback(topic, payload, length);
  });
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(1024);
  wifiClient.setTimeout(5000);
  refreshTopics();
  systemLogger.info("Gestionnaire MQTT initialisé");
}

void MqttManager::refreshTopics() {
  String base = mqttCfg.topic;
  base.trim();
  if (base.length() == 0) base = "pool/sensors";
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (base.length() == 0) base = "pool/sensors";

  topics.base = base;
  topics.temperatureState = base + "/temperature";
  topics.phState = base + "/ph";
  topics.orpState = base + "/orp";
  topics.filtrationState = base + "/filtration_state";
  topics.filtrationModeState = base + "/filtration_mode";
  topics.filtrationModeCommand = base + "/filtration_mode/set";
  topics.filtrationCommand = base + "/filtration/set";
  topics.lightingState = base + "/lighting_state";
  topics.lightingCommand = base + "/lighting/set";
  topics.phDosageState = base + "/ph_dosage";
  topics.orpDosageState = base + "/orp_dosage";
  topics.phDosingState = base + "/ph_dosing";
  topics.orpDosingState = base + "/orp_dosing";
  topics.phLimitState = base + "/ph_limit";
  topics.orpLimitState = base + "/orp_limit";
  topics.phTargetState = base + "/ph_target";
  topics.orpTargetState = base + "/orp_target";
  topics.phTargetCommand = base + "/ph_target/set";
  topics.orpTargetCommand = base + "/orp_target/set";
  topics.alertsTopic = base + "/alerts";
  topics.logsTopic = base + "/logs";
  topics.statusTopic = base + "/status";
  topics.diagnosticTopic = base + "/diagnostic";
}

void MqttManager::update() {
  if (reconnectRequested) {
    reconnectRequested = false;
    // Déconnecter d'abord si déjà connecté pour forcer une reconnexion avec les nouveaux paramètres
    if (mqtt.connected()) {
      disconnect();
    }
    if (mqttCfg.enabled) {
      connect();
    }
  }

  if (!mqtt.connected() && mqttCfg.enabled) {
    connect(); // connect() gère son propre rate-limit (5s entre les tentatives)
  }

  if (mqtt.connected()) {
    mqtt.loop();
  }
}

void MqttManager::connect() {
  if (!mqttCfg.enabled || mqttCfg.server.length() == 0 || !WiFi.isConnected()) {
    return;
  }

  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastAttempt < 5000) return;
  lastAttempt = now;

  systemLogger.info("Tentative connexion MQTT...");
  refreshTopics();

  // Préparer LWT (Last Will Testament)
  const char* lwtTopic = topics.statusTopic.c_str();
  const char* lwtMessage = "offline";
  uint8_t lwtQos = 1;
  bool lwtRetain = true;

  bool connected = false;

  if (mqttCfg.username.length() > 0) {
    connected = mqtt.connect("ESP32PoolController",
                            mqttCfg.username.c_str(),
                            mqttCfg.password.c_str(),
                            lwtTopic, lwtQos, lwtRetain, lwtMessage);
  } else {
    connected = mqtt.connect("ESP32PoolController",
                            lwtTopic, lwtQos, lwtRetain, lwtMessage);
  }

  if (connected) {
    systemLogger.info("MQTT connecté !");

    // Publier immédiatement le status online
    publishStatus("online");

    mqtt.subscribe(topics.filtrationModeCommand.c_str());
    mqtt.subscribe(topics.filtrationCommand.c_str());
    mqtt.subscribe(topics.lightingCommand.c_str());
    mqtt.subscribe(topics.phTargetCommand.c_str());
    mqtt.subscribe(topics.orpTargetCommand.c_str());
    discoveryPublished = false;
    publishDiscovery();
    publishAllStates();
    publishDiagnostic();
  } else {
    systemLogger.error("MQTT échec, code=" + String(mqtt.state()));
  }
}

void MqttManager::disconnect() {
  if (mqtt.connected()) {
    mqtt.disconnect();
    systemLogger.info("MQTT déconnecté");
  }
}

void MqttManager::publishSensorState(const String& topic, const String& payload, bool retain) {
  if (!mqtt.connected() || topic.length() == 0) return;
  if (!mqtt.publish(topic.c_str(), payload.c_str(), retain)) {
    systemLogger.warning("Échec publication MQTT: " + topic);
  }
}

void MqttManager::publishAllStates() {
  if (!mqtt.connected()) return;

  if (!isnan(sensors.getTemperature())) {
    publishSensorState(topics.temperatureState, String(sensors.getTemperature(), 1));
  }
  if (!isnan(sensors.getPh())) {
    publishSensorState(topics.phState, String(sensors.getPh(), 1));
  }
  if (!isnan(sensors.getOrp())) {
    publishSensorState(topics.orpState, String(sensors.getOrp(), 1));
  }
  publishFiltrationState();
  publishLightingState();
  publishDosingState();
  publishTargetState();
}

void MqttManager::publishFiltrationState() {
  if (!mqtt.connected()) return;
  publishSensorState(topics.filtrationModeState, filtrationCfg.mode);
  publishSensorState(topics.filtrationState, filtration.isRunning() ? "ON" : "OFF");
}

void MqttManager::publishLightingState() {
  if (!mqtt.connected()) return;
  publishSensorState(topics.lightingState, lighting.isOn() ? "ON" : "OFF");
}

void MqttManager::publishDosingState() {
  if (!mqtt.connected()) return;
  publishSensorState(topics.phDosingState,  PumpController.isPhDosing()  ? "ON" : "OFF");
  publishSensorState(topics.orpDosingState, PumpController.isOrpDosing() ? "ON" : "OFF");
  publishSensorState(topics.phLimitState,   safetyLimits.phLimitReached  ? "ON" : "OFF");
  publishSensorState(topics.orpLimitState,  safetyLimits.orpLimitReached ? "ON" : "OFF");
}

void MqttManager::publishTargetState() {
  if (!mqtt.connected()) return;
  publishSensorState(topics.phTargetState,  String(mqttCfg.phTarget,  1));
  publishSensorState(topics.orpTargetState, String(mqttCfg.orpTarget, 0));
}

void MqttManager::publishAlert(const String& alertType, const String& message) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["type"] = alertType;
  doc["message"] = message;
  doc["timestamp"] = millis();
  String payload;
  serializeJson(doc, payload);
  publishSensorState(topics.alertsTopic, payload, false);
  systemLogger.warning("Alerte: " + alertType + " - " + message);
}

void MqttManager::publishLog(const String& logMessage) {
  if (!mqtt.connected()) return;
  publishSensorState(topics.logsTopic, logMessage, false);
}

void MqttManager::messageCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  String cmd;
  for (unsigned int i = 0; i < length; ++i) {
    cmd += static_cast<char>(payload[i]);
  }
  cmd.trim();

  if (topicStr == topics.filtrationModeCommand) {
    cmd.toLowerCase();
    if (cmd == "auto" || cmd == "manual" || cmd == "force" || cmd == "off") {
      if (filtrationCfg.mode != cmd) {
        filtrationCfg.mode = cmd;
        filtration.ensureTimesValid();
        if (filtrationCfg.mode == "auto") {
          filtration.computeAutoSchedule();
        }
        saveMqttConfig();
        systemLogger.info("Mode filtration changé: " + cmd);
      }
      publishFiltrationState();
    }
  } else if (topicStr == topics.filtrationCommand) {
    cmd.toUpperCase();
    if (cmd == "ON") {
      filtrationCfg.forceOn = true;
      filtrationCfg.forceOff = false;
      systemLogger.info("Filtration forcée ON (MQTT)");
    } else if (cmd == "OFF") {
      filtrationCfg.forceOn = false;
      filtrationCfg.forceOff = true;
      systemLogger.info("Filtration forcée OFF (MQTT)");
    }
    // Ne pas publier ici : filtration.update() va changer le relais
    // et appeler publishState() une fois l'état réel mis à jour.
  } else if (topicStr == topics.lightingCommand) {
    cmd.toUpperCase();
    if (cmd == "ON") {
      lighting.setManualOn();
    } else if (cmd == "OFF") {
      lighting.setManualOff();
    }
    publishLightingState();
  } else if (topicStr == topics.phTargetCommand) {
    float value = cmd.toFloat();
    if (value >= 6.0f && value <= 8.5f) {
      mqttCfg.phTarget = value;
      saveMqttConfig();
      publishTargetState();
      systemLogger.info("Consigne pH changée via MQTT: " + String(value, 1));
    } else {
      systemLogger.warning("Consigne pH invalide (MQTT): " + cmd);
    }
  } else if (topicStr == topics.orpTargetCommand) {
    float value = cmd.toFloat();
    if (value >= 400.0f && value <= 900.0f) {
      mqttCfg.orpTarget = value;
      saveMqttConfig();
      publishTargetState();
      systemLogger.info("Consigne ORP changée via MQTT: " + String(value, 0));
    } else {
      systemLogger.warning("Consigne ORP invalide (MQTT): " + cmd);
    }
  }
}

void MqttManager::publishDiscovery() {
  if (!mqtt.connected() || discoveryPublished) return;

  JsonDocument doc;
  const String discoveryBase = String(HA_DISCOVERY_PREFIX) + "/";

  auto makeDevice = [&](JsonObject device) {
    device["name"] = HA_DEVICE_NAME;
    device["manufacturer"] = "ESP32";
    device["model"] = "Pool Controller";
    JsonArray ids = device["identifiers"].to<JsonArray>();
    ids.add(HA_DEVICE_ID);
  };

  auto publishConfig = [&](const String& configTopic) {
    String payload;
    serializeJson(doc, payload);
    bool ok = mqtt.publish(configTopic.c_str(), payload.c_str(), true);
    systemLogger.info("Discovery " + configTopic + (ok ? " OK" : " FAILED"));
    doc.clear();
  };

  // Température
  String topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_temperature/config";
  doc["name"] = "Piscine Température";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_temperature";
  doc["state_topic"] = topics.temperatureState;
  doc["device_class"] = "temperature";
  doc["unit_of_measurement"] = "°C";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // pH
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph/config";
  doc["name"] = "Piscine pH";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph";
  doc["state_topic"] = topics.phState;
  doc["unit_of_measurement"] = "pH";
  doc["icon"] = "mdi:water";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // ORP
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_orp/config";
  doc["name"] = "Piscine ORP";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp";
  doc["state_topic"] = topics.orpState;
  doc["unit_of_measurement"] = "mV";
  doc["icon"] = "mdi:flash";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Filtration
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_filtration/config";
  doc["name"] = "Filtration Active";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_filtration";
  doc["state_topic"] = topics.filtrationState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "running";
  doc["icon"] = "mdi:water-pump";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Mode filtration
  topic = discoveryBase + "select/" + HA_DEVICE_ID + "_filtration_mode/config";
  doc["name"] = "Mode Filtration";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_filtration_mode";
  doc["state_topic"] = topics.filtrationModeState;
  doc["command_topic"] = topics.filtrationModeCommand;
  doc["icon"] = "mdi:water-pump";
  JsonArray options = doc["options"].to<JsonArray>();
  options.add("auto");
  options.add("manual");
  options.add("force");
  options.add("off");
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Switch filtration ON/OFF
  topic = discoveryBase + "switch/" + HA_DEVICE_ID + "_filtration_switch/config";
  doc["name"] = "Filtration Marche/Arrêt";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_filtration_switch";
  doc["state_topic"] = topics.filtrationState;
  doc["command_topic"] = topics.filtrationCommand;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["state_on"] = "ON";
  doc["state_off"] = "OFF";
  doc["icon"] = "mdi:water-pump";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Switch éclairage ON/OFF
  topic = discoveryBase + "switch/" + HA_DEVICE_ID + "_lighting/config";
  doc["name"] = "Éclairage Piscine";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_lighting";
  doc["state_topic"] = topics.lightingState;
  doc["command_topic"] = topics.lightingCommand;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["state_on"] = "ON";
  doc["state_off"] = "OFF";
  doc["icon"] = "mdi:pool";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Dosage pH actif
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_ph_dosing/config";
  doc["name"] = "Dosage pH Actif";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_dosing";
  doc["state_topic"] = topics.phDosingState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:water-plus";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Dosage ORP actif
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_orp_dosing/config";
  doc["name"] = "Dosage Chlore Actif";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_dosing";
  doc["state_topic"] = topics.orpDosingState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:flask";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Limite pH atteinte
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_ph_limit/config";
  doc["name"] = "Limite Journalière pH";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_limit";
  doc["state_topic"] = topics.phLimitState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "problem";
  doc["icon"] = "mdi:alert";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Limite ORP atteinte
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_orp_limit/config";
  doc["name"] = "Limite Journalière Chlore";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_limit";
  doc["state_topic"] = topics.orpLimitState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "problem";
  doc["icon"] = "mdi:alert";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Consigne pH
  topic = discoveryBase + "number/" + HA_DEVICE_ID + "_ph_target/config";
  doc["name"] = "Consigne pH";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_target";
  doc["state_topic"] = topics.phTargetState;
  doc["command_topic"] = topics.phTargetCommand;
  doc["min"] = 6.0;
  doc["max"] = 8.5;
  doc["step"] = 0.1;
  doc["unit_of_measurement"] = "pH";
  doc["icon"] = "mdi:water";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Consigne ORP
  topic = discoveryBase + "number/" + HA_DEVICE_ID + "_orp_target/config";
  doc["name"] = "Consigne ORP";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_target";
  doc["state_topic"] = topics.orpTargetState;
  doc["command_topic"] = topics.orpTargetCommand;
  doc["min"] = 400;
  doc["max"] = 900;
  doc["step"] = 10;
  doc["unit_of_measurement"] = "mV";
  doc["icon"] = "mdi:flash";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Binary sensor status
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_status/config";
  doc["name"] = "Contrôleur Status";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_status";
  doc["state_topic"] = topics.statusTopic;
  doc["payload_on"] = "online";
  doc["payload_off"] = "offline";
  doc["device_class"] = "connectivity";
  doc["icon"] = "mdi:wifi-check";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  discoveryPublished = true;
  systemLogger.info("Home Assistant discovery publié");
}

void MqttManager::publishStatus(const String& status) {
  if (!mqtt.connected()) return;
  publishSensorState(topics.statusTopic, status, true);
  systemLogger.info("Status MQTT: " + status);
}

void MqttManager::publishDiagnostic() {
  if (!mqtt.connected()) return;

  JsonDocument doc;

  // Informations système
  doc["uptime_ms"] = millis();
  doc["uptime_min"] = millis() / kMillisToMinutes;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["heap_size"] = ESP.getHeapSize();
  doc["min_free_heap"] = ESP.getMinFreeHeap();

  // WiFi
  doc["wifi_"] = WiFi.SSID();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["wifi_quality"] = constrain(map(WiFi.RSSI(), -100, -50, 0, 100), 0, 100);
  doc["ip_address"] = WiFi.localIP().toString();

  // Capteurs
  doc["sensors_initialized"] = sensors.isInitialized();
  doc["ph_value"] = round(sensors.getPh() * 10.0f) / 10.0f;
  doc["orp_value"] = sensors.getOrp();
  doc["temperature"] = sensors.getTemperature();

  // Dosage
  doc["ph_dosing_active"] = PumpController.isPhDosing();
  doc["orp_dosing_active"] = PumpController.isOrpDosing();
  doc["ph_used_ms"] = PumpController.getPhUsedMs();
  doc["orp_used_ms"] = PumpController.getOrpUsedMs();

  // Sécurité
  doc["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  doc["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  doc["ph_limit_reached"] = safetyLimits.phLimitReached;
  doc["orp_limit_reached"] = safetyLimits.orpLimitReached;

  // Filtration
  doc["filtration_running"] = filtration.isRunning();
  doc["filtration_mode"] = filtrationCfg.mode;

  // Configuration
  doc["ph_target"] = mqttCfg.phTarget;
  doc["orp_target"] = mqttCfg.orpTarget;

  // Version
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["build_timestamp"] = __DATE__ " " __TIME__;

  String payload;
  serializeJson(doc, payload);
  publishSensorState(topics.diagnosticTopic, payload, true);

  systemLogger.debug("Diagnostic publié");
}
