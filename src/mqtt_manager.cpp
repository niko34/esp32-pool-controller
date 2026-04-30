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
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>

using mqtt_internal::OutboundMsg;
using mqtt_internal::InboundCmd;
using mqtt_internal::InboundCmdType;

MqttManager mqttManager;

MqttManager::MqttManager() : mqtt(wifiClient) {}

// ============================================================================
// Initialisation
// ============================================================================

void MqttManager::begin() {
  mqtt.setServer(mqttCfg.server.c_str(), mqttCfg.port);
  mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
    mqttManager.messageCallback(topic, payload, length);
  });
  mqtt.setSocketTimeout(2);  // 2s max de gel par tentative ratée — voir ADR-0010
  mqtt.setKeepAlive(60);     // Mosquitto applique 1.5×keepalive = 90s de tolérance
  mqtt.setBufferSize(1024);
  wifiClient.setTimeout(kMqttClientConnectTimeoutSec);  // unité = secondes (cf. constants.h)
  refreshTopics();

  // Création des queues — allouées une fois, jamais libérées (cycle de vie = vie du firmware)
  outQueue = xQueueCreate(kMqttOutQueueLength, sizeof(OutboundMsg));
  inQueue  = xQueueCreate(kMqttInQueueLength,  sizeof(InboundCmd));
  if (outQueue == nullptr || inQueue == nullptr) {
    systemLogger.critical("MQTT: échec création queues FreeRTOS");
    return;
  }

  // Création de la tâche dédiée — voir ADR-0011
  // Pinned core 0 : loopTask est sur core 1, on répartit la charge réseau.
  BaseType_t ok = xTaskCreatePinnedToCore(
      &MqttManager::mqttTaskFunction,
      "mqttTask",
      kMqttTaskStackSize,
      this,
      kMqttTaskPriority,
      &taskHandle,
      kMqttTaskCore);
  if (ok != pdPASS) {
    systemLogger.critical("MQTT: échec xTaskCreatePinnedToCore (mqttTask)");
    taskHandle = nullptr;
    return;
  }

  systemLogger.info("Gestionnaire MQTT initialisé (mqttTask core=" + String(kMqttTaskCore) +
                    " prio=" + String(kMqttTaskPriority) +
                    " stack=" + String(kMqttTaskStackSize) + ")");
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
  topics.phStockLowState = base + "/ph_stock_low";
  topics.orpStockLowState = base + "/orp_stock_low";
  topics.phRemainingState = base + "/ph_remaining_ml";
  topics.orpRemainingState = base + "/orp_remaining_ml";
  topics.phTargetState = base + "/ph_target";
  topics.orpTargetState = base + "/orp_target";
  topics.phTargetCommand = base + "/ph_target/set";
  topics.orpTargetCommand = base + "/orp_target/set";
  topics.phRegulationModeState  = base + "/ph_regulation_mode";
  topics.phDailyTargetMlState   = base + "/ph_daily_target_ml";
  topics.orpRegulationModeState = base + "/orp_regulation_mode";
  topics.orpDailyTargetMlState  = base + "/orp_daily_target_ml";
  topics.alertsTopic = base + "/alerts";
  topics.logsTopic = base + "/logs";
  topics.statusTopic = base + "/status";
  topics.diagnosticTopic = base + "/diagnostic";
}

// ============================================================================
// Tâche dédiée (core 0) — pilote unique de mqtt.connect() / mqtt.loop() / mqtt.publish()
// ============================================================================

void MqttManager::mqttTaskFunction(void* pvParameters) {
  MqttManager* self = static_cast<MqttManager*>(pvParameters);
  // Inscription au watchdog : la tâche doit reset toutes les < 30s.
  esp_task_wdt_add(NULL);
  systemLogger.info("mqttTask démarrée (core=" + String(xPortGetCoreID()) +
                    " prio=" + String(uxTaskPriorityGet(NULL)) + ")");
  self->taskLoop();
  // taskLoop ne retourne que sur shutdownForRestart()
  esp_task_wdt_delete(NULL);
  vTaskDelete(NULL);
}

void MqttManager::taskLoop() {
  while (!taskShouldStop.load(std::memory_order_relaxed)) {
    esp_task_wdt_reset();
    // Single source of truth pour l'UI : connectedAtomic suit l'état canonique de la lib à chaque tour.
    connectedAtomic.store(mqtt.connected(), std::memory_order_relaxed);

    // 1) Reconnect/connect géré ici (pas depuis loopTask)
    if (reconnectRequested) {
      reconnectRequested = false;
      if (mqtt.connected()) {
        mqtt.disconnect();
        // connectedAtomic mis à jour au prochain tour par le store canonique.
        systemLogger.info("MQTT déconnecté (reconnect demandé)");
      }
      if (mqttCfg.enabled) {
        connectInTask();
      }
    }

    static bool wasConnected = false;
    if (!mqtt.connected() && mqttCfg.enabled) {
      if (wasConnected) {
        systemLogger.warning("MQTT déconnecté détecté — état=" + String(mqtt.state()));
        wasConnected = false;
      }
      connectInTask();  // rate-limit interne 5s + backoff exponentiel
    } else if (mqtt.connected()) {
      wasConnected = true;
      mqtt.loop();  // peut bloquer brièvement (setSocketTimeout=2s) — isolé de loopTask
    }

    // 2) Publications périodiques signalées depuis loopTask via flags atomiques.
    //    On lit/snapshot les états sous configMutex DANS la tâche pour éviter
    //    que loopTask n'attende sur un mutex pendant qu'on publie ~15 messages.
    if (publishStatesRequested.exchange(false, std::memory_order_relaxed)) {
      publishAllStatesInternal();
    }
    if (publishDiagnosticRequested.exchange(false, std::memory_order_relaxed)) {
      publishDiagnosticInternal();
    }

    // 3) Drainer la queue sortante (publish unitaires depuis publishAlert/publishStatus/etc.)
    drainOutQueue();

    // 4) Attente courte avant la prochaine itération.
    //    Pas de vTaskDelay direct : on attend sur la queue sortante avec timeout
    //    pour réveiller la tâche dès qu'un nouveau message est posté.
    OutboundMsg peek;
    if (xQueuePeek(outQueue, &peek, pdMS_TO_TICKS(kMqttTaskLoopTimeoutMs)) == pdTRUE) {
      // Message disponible — la prochaine itération drainera. Pas de pop ici.
    }
  }
}

void MqttManager::connectInTask() {
  if (!mqttCfg.enabled || mqttCfg.server.length() == 0 || !WiFi.isConnected()) {
    return;
  }
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastAttempt < _reconnectDelay) return;
  lastAttempt = now;

  systemLogger.info("Tentative connexion MQTT (délai=" + String(_reconnectDelay / 1000) + "s)...");
  refreshTopics();

  // Pré-résolution DNS séparée du connect TCP (cf. ADR-0010).
  // Court-circuit IP : si server est déjà une IP, skip lwip dns_*.
  IPAddress brokerIp;
  if (!brokerIp.fromString(mqttCfg.server)) {
    if (!WiFi.hostByName(mqttCfg.server.c_str(), brokerIp)) {
      constexpr unsigned long kMqttMaxReconnectDelayMs = 120000UL;
      _reconnectDelay = min(_reconnectDelay * 2, kMqttMaxReconnectDelayMs);
      systemLogger.error("MQTT échec DNS pour '" + mqttCfg.server + "' — prochaine tentative dans " +
                         String(_reconnectDelay / 1000) + "s");
      return;
    }
  }
  mqtt.setServer(brokerIp, mqttCfg.port);

  esp_task_wdt_reset();  // juste avant l'appel bloquant

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
  esp_task_wdt_reset();  // borne le pire cas connect/CONNACK même en cas d'échec

  if (connected) {
    _reconnectDelay = 5000;

    // Borne le write() TCP à kMqttSocketSendTimeoutMs (500 ms) — le PINGREQ keepalive
    // a le temps de partir (vs IT4 O_NONBLOCK qui pouvait le faire échouer silencieusement
    // si le send buffer était plein → broker exceeded_timeout).
    int fd = wifiClient.fd();
    if (fd >= 0) {
      struct timeval tv;
      tv.tv_sec = kMqttSocketSendTimeoutMs / 1000;
      tv.tv_usec = (kMqttSocketSendTimeoutMs % 1000) * 1000;
      if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        systemLogger.warning("MQTT: échec setsockopt SO_SNDTIMEO (errno=" + String(errno) + ")");
      }
    }

    connectedAtomic.store(true, std::memory_order_relaxed);
    systemLogger.info("MQTT connecté !");

    // Status online : direct (on est dans la tâche) — court-circuite outQueue
    safePublish(topics.statusTopic.c_str(), "online", true);

    mqtt.subscribe(topics.filtrationModeCommand.c_str());
    mqtt.subscribe(topics.filtrationCommand.c_str());
    mqtt.subscribe(topics.lightingCommand.c_str());
    mqtt.subscribe(topics.phTargetCommand.c_str());
    mqtt.subscribe(topics.orpTargetCommand.c_str());

    discoveryPublished = false;
    esp_task_wdt_reset();
    publishDiscovery();              // 17 publish — long si CPL lossy
    esp_task_wdt_reset();
    publishAllStatesInternal();      // ~15 publish
    esp_task_wdt_reset();
    publishDiagnosticInternal();     // 1 publish JSON ~400c
    esp_task_wdt_reset();
  } else {
    constexpr unsigned long kMqttMaxReconnectDelayMs = 120000UL;
    _reconnectDelay = min(_reconnectDelay * 2, kMqttMaxReconnectDelayMs);
    systemLogger.error("MQTT échec, code=" + String(mqtt.state()) +
                       " — prochaine tentative dans " + String(_reconnectDelay / 1000) + "s");
  }
}

// Wrapper unique pour tout mqtt.publish() depuis mqttTask. Reset wdt + check
// mqtt.connected() + délégation. Le socket TCP est configuré avec SO_SNDTIMEO=500ms
// (cf. connectInTask, kMqttSocketSendTimeoutMs), donc mqtt.publish() retourne false
// après au plus 500 ms si le send buffer reste plein, sans bloquer mqttTask. Le
// keepalive PINGREQ PubSubClient redevient fiable (vs IT4 O_NONBLOCK où EAGAIN
// silencieux pouvait le perdre → broker exceeded_timeout). Voir feature-014 IT5 / ADR-0011.
bool MqttManager::safePublish(const char* topic, const char* payload, bool retain) {
  esp_task_wdt_reset();
  if (!mqtt.connected()) return false;
  return mqtt.publish(topic, payload, retain);
}

void MqttManager::drainOutQueue() {
  // Limite le nombre de publish par itération pour laisser respirer mqtt.loop()
  // et la consommation des sockets entrantes (callbacks HA).
  constexpr int kMaxPublishPerIter = 8;
  if (outQueue == nullptr) return;

  for (int i = 0; i < kMaxPublishPerIter; ++i) {
    OutboundMsg msg;
    if (xQueueReceive(outQueue, &msg, 0) != pdTRUE) {
      return;  // queue vide
    }
    if (!mqtt.connected()) {
      // On drop silencieusement : impossible de publier sans connexion.
      // Les états seront republiés au prochain publishAllStatesInternal()
      // lors de la reconnexion.
      continue;
    }
    if (!safePublish(msg.topic, msg.payload, msg.retain)) {
      // Peut devenir bruyant en cas de coupure réseau, debug-level approprié.
      systemLogger.debug("MQTT publish drop: " + String(msg.topic));
    }
  }
}

// ============================================================================
// Update legacy — no-op (toute la logique vit dans mqttTask)
// ============================================================================

void MqttManager::update() {
  // Conservé pour préserver les call sites historiques (main.cpp). La logique de
  // connect/loop/publish vit désormais dans mqttTask. Voir ADR-0011.
  // Note : si reconnectRequested a été posé par requestReconnect() depuis loopTask,
  // mqttTask le détectera à sa prochaine itération (< 100 ms).
}

void MqttManager::connect() {
  // Préserve l'API : déclenche une reconnexion via mqttTask.
  requestReconnect();
}

void MqttManager::disconnect() {
  // Appelable depuis loopTask. La déconnexion réelle est portée par mqttTask
  // au prochain reconnectRequested (qui détectera connected==true et déconnectera).
  // Pour les cas synchrones (shutdown OTA), passer par shutdownForRestart().
  reconnectRequested = false;
  if (mqtt.connected()) {
    mqtt.disconnect();
    connectedAtomic.store(false, std::memory_order_relaxed);
    systemLogger.info("MQTT déconnecté");
  }
}

// ============================================================================
// Producteurs (depuis loopTask) — non-bloquants
// ============================================================================

void MqttManager::enqueueOutbound(const String& topic, const String& payload, bool retain) {
  if (outQueue == nullptr || topic.length() == 0) return;
  OutboundMsg msg;
  // Tronque si dépassement (les payloads MQTT sont bornés à kMaxPayloadLen=384).
  // En pratique aucun topic ne dépasse ~80c, aucun payload ~300c (publishDiagnostic).
  strlcpy(msg.topic,   topic.c_str(),   sizeof(msg.topic));
  strlcpy(msg.payload, payload.c_str(), sizeof(msg.payload));
  msg.retain = retain;

  if (xQueueSend(outQueue, &msg, 0) != pdTRUE) {
    // Queue pleine : drop le plus ancien (best-effort) et retente.
    OutboundMsg dropped;
    if (xQueueReceive(outQueue, &dropped, 0) == pdTRUE) {
      noteDropEdgeTriggered();
    }
    xQueueSend(outQueue, &msg, 0);  // si ça échoue à nouveau, on perd ce message
  }
}

void MqttManager::noteDropEdgeTriggered() {
  // Edge-triggered : un seul WARN par fenêtre de 5s pour éviter le spam log.
  droppedSinceLastWarn++;
  unsigned long now = millis();
  if (now - lastDropWarnMs >= 5000) {
    systemLogger.warning("MQTT outQueue saturée — " + String(droppedSinceLastWarn) +
                         " message(s) abandonné(s)");
    droppedSinceLastWarn = 0;
    lastDropWarnMs = now;
  }
}

void MqttManager::publishSensorState(const String& topic, const String& payload, bool retain) {
  // Devient un producteur — non-bloquant. Le check connected() est fait DANS mqttTask
  // pour éviter une race entre l'enqueue et la déconnexion : dropper côté consommateur
  // si pas connecté est plus simple et thread-safe.
  enqueueOutbound(topic, payload, retain);
}

// publishAllStates / publishDiagnostic depuis loopTask = simple signal atomique.
// La sérialisation et les snapshots sont faits dans mqttTask.
void MqttManager::publishAllStates() {
  publishStatesRequested.store(true, std::memory_order_relaxed);
}

void MqttManager::publishDiagnostic() {
  publishDiagnosticRequested.store(true, std::memory_order_relaxed);
}

// Les méthodes "atomiques" (un seul publish, payload simple) restent disponibles
// depuis loopTask : elles enfilent directement le message dans outQueue.
// Elles peuvent aussi être appelées depuis mqttTask (lors de la reconnexion par exemple).
void MqttManager::publishFiltrationState() {
  enqueueOutbound(topics.filtrationModeState, filtrationCfg.mode, true);
  enqueueOutbound(topics.filtrationState, filtration.isRunning() ? "ON" : "OFF", true);
}

void MqttManager::publishLightingState() {
  enqueueOutbound(topics.lightingState, lighting.isOn() ? "ON" : "OFF", true);
}

void MqttManager::publishDosingState() {
  enqueueOutbound(topics.phDosingState,  PumpController.isPhDosing()  ? "ON" : "OFF", true);
  enqueueOutbound(topics.orpDosingState, PumpController.isOrpDosing() ? "ON" : "OFF", true);
  enqueueOutbound(topics.phLimitState,   safetyLimits.phLimitReached  ? "ON" : "OFF", true);
  enqueueOutbound(topics.orpLimitState,  safetyLimits.orpLimitReached ? "ON" : "OFF", true);
}

void MqttManager::publishProductState() {
  if (configMutex) xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
  float phRemaining  = max(0.0f, productCfg.phContainerVolumeMl  - productCfg.phTotalInjectedMl);
  float orpRemaining = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  bool phStockLow  = productCfg.phTrackingEnabled  && productCfg.phAlertThresholdMl  > 0 && phRemaining  <= productCfg.phAlertThresholdMl;
  bool orpStockLow = productCfg.orpTrackingEnabled && productCfg.orpAlertThresholdMl > 0 && orpRemaining <= productCfg.orpAlertThresholdMl;
  if (configMutex) xSemaphoreGiveRecursive(configMutex);

  enqueueOutbound(topics.phStockLowState,   phStockLow  ? "ON" : "OFF", true);
  enqueueOutbound(topics.orpStockLowState,  orpStockLow ? "ON" : "OFF", true);
  enqueueOutbound(topics.phRemainingState,  String(phRemaining,  0), true);
  enqueueOutbound(topics.orpRemainingState, String(orpRemaining, 0), true);
}

void MqttManager::publishTargetState() {
  if (configMutex) xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
  float phT = mqttCfg.phTarget;
  float orpT = mqttCfg.orpTarget;
  String phMode = mqttCfg.phRegulationMode;
  int phDaily = mqttCfg.phDailyTargetMl;
  String orpMode = mqttCfg.orpRegulationMode;
  int orpDaily = mqttCfg.orpDailyTargetMl;
  if (configMutex) xSemaphoreGiveRecursive(configMutex);

  enqueueOutbound(topics.phTargetState,  String(phT,  1), true);
  enqueueOutbound(topics.orpTargetState, String(orpT, 0), true);
  enqueueOutbound(topics.phRegulationModeState,  phMode, true);
  enqueueOutbound(topics.phDailyTargetMlState,   String(phDaily), true);
  enqueueOutbound(topics.orpRegulationModeState, orpMode, true);
  enqueueOutbound(topics.orpDailyTargetMlState,  String(orpDaily), true);
}

void MqttManager::publishAlert(const String& alertType, const String& message) {
  JsonDocument doc;
  doc["type"] = alertType;
  doc["message"] = message;
  doc["timestamp"] = millis();
  String payload;
  serializeJson(doc, payload);
  enqueueOutbound(topics.alertsTopic, payload, false);
  systemLogger.warning("Alerte: " + alertType + " - " + message);
}

void MqttManager::publishLog(const String& logMessage) {
  enqueueOutbound(topics.logsTopic, logMessage, false);
}

void MqttManager::publishStatus(const String& status) {
  enqueueOutbound(topics.statusTopic, status, true);
  systemLogger.info("Status MQTT: " + status);
}

// ============================================================================
// Implémentations internes (mqttTask uniquement) — snapshots sous mutex
// ============================================================================

void MqttManager::publishAllStatesInternal() {
  if (!mqtt.connected()) return;

  // Capteurs : lectures atomiques côté firmware (float scalaires)
  float t = sensors.getTemperature();
  float ph = sensors.getPh();
  float orp = sensors.getOrp();

  // Tous les publish passent par safePublish() : reset wdt + check connected interne.
  // Plus besoin de garde-fous intermédiaires ni de wdt reset explicites — voir IT4 / ADR-0011.
  if (!isnan(t)) {
    String p = String(t, 1);
    safePublish(topics.temperatureState.c_str(), p.c_str(), true);
  }
  if (!isnan(ph)) {
    String p = String(ph, 1);
    safePublish(topics.phState.c_str(), p.c_str(), true);
  }
  if (!isnan(orp)) {
    String p = String(orp, 1);
    safePublish(topics.orpState.c_str(), p.c_str(), true);
  }

  // Filtration / lighting / dosing
  safePublish(topics.filtrationModeState.c_str(), filtrationCfg.mode.c_str(), true);
  safePublish(topics.filtrationState.c_str(), filtration.isRunning() ? "ON" : "OFF", true);
  safePublish(topics.lightingState.c_str(), lighting.isOn() ? "ON" : "OFF", true);

  safePublish(topics.phDosingState.c_str(),  PumpController.isPhDosing()  ? "ON" : "OFF", true);
  safePublish(topics.orpDosingState.c_str(), PumpController.isOrpDosing() ? "ON" : "OFF", true);
  safePublish(topics.phLimitState.c_str(),   safetyLimits.phLimitReached  ? "ON" : "OFF", true);
  safePublish(topics.orpLimitState.c_str(),  safetyLimits.orpLimitReached ? "ON" : "OFF", true);

  // Product / target sous configMutex — snapshot puis publish hors verrou.
  float phRemaining, orpRemaining;
  bool phStockLow, orpStockLow;
  float phT, orpT;
  String phMode, orpMode;
  int phDaily, orpDaily;
  if (configMutex) xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
  phRemaining  = max(0.0f, productCfg.phContainerVolumeMl  - productCfg.phTotalInjectedMl);
  orpRemaining = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  phStockLow   = productCfg.phTrackingEnabled  && productCfg.phAlertThresholdMl  > 0 && phRemaining  <= productCfg.phAlertThresholdMl;
  orpStockLow  = productCfg.orpTrackingEnabled && productCfg.orpAlertThresholdMl > 0 && orpRemaining <= productCfg.orpAlertThresholdMl;
  phT = mqttCfg.phTarget;
  orpT = mqttCfg.orpTarget;
  phMode = mqttCfg.phRegulationMode;
  phDaily = mqttCfg.phDailyTargetMl;
  orpMode = mqttCfg.orpRegulationMode;
  orpDaily = mqttCfg.orpDailyTargetMl;
  if (configMutex) xSemaphoreGiveRecursive(configMutex);

  safePublish(topics.phStockLowState.c_str(),   phStockLow  ? "ON" : "OFF", true);
  safePublish(topics.orpStockLowState.c_str(),  orpStockLow ? "ON" : "OFF", true);
  safePublish(topics.phRemainingState.c_str(),  String(phRemaining,  0).c_str(), true);
  safePublish(topics.orpRemainingState.c_str(), String(orpRemaining, 0).c_str(), true);

  safePublish(topics.phTargetState.c_str(),  String(phT,  1).c_str(), true);
  safePublish(topics.orpTargetState.c_str(), String(orpT, 0).c_str(), true);
  safePublish(topics.phRegulationModeState.c_str(),  phMode.c_str(), true);
  safePublish(topics.phDailyTargetMlState.c_str(),   String(phDaily).c_str(), true);
  safePublish(topics.orpRegulationModeState.c_str(), orpMode.c_str(), true);
  safePublish(topics.orpDailyTargetMlState.c_str(),  String(orpDaily).c_str(), true);
}

void MqttManager::publishDiagnosticInternal() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["uptime_ms"] = millis();
  doc["uptime_min"] = millis() / kMillisToMinutes;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["heap_size"] = ESP.getHeapSize();
  doc["min_free_heap"] = ESP.getMinFreeHeap();
  doc["wifi_"] = WiFi.SSID();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["wifi_quality"] = constrain(map(WiFi.RSSI(), -100, -50, 0, 100), 0, 100);
  doc["ip_address"] = WiFi.localIP().toString();
  doc["sensors_initialized"] = sensors.isInitialized();
  doc["ph_value"] = round(sensors.getPh() * 10.0f) / 10.0f;
  doc["orp_value"] = sensors.getOrp();
  doc["temperature"] = sensors.getTemperature();
  doc["ph_dosing_active"] = PumpController.isPhDosing();
  doc["orp_dosing_active"] = PumpController.isOrpDosing();
  doc["ph_used_ms"] = PumpController.getPhUsedMs();
  doc["orp_used_ms"] = PumpController.getOrpUsedMs();
  doc["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  doc["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  doc["ph_limit_reached"] = safetyLimits.phLimitReached;
  doc["orp_limit_reached"] = safetyLimits.orpLimitReached;
  doc["filtration_running"] = filtration.isRunning();
  doc["filtration_mode"] = filtrationCfg.mode;
  doc["ph_target"] = mqttCfg.phTarget;
  doc["orp_target"] = mqttCfg.orpTarget;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["build_timestamp"] = __DATE__ " " __TIME__;

  // Stack high-water-mark de la tâche (utile pour caler kMqttTaskStackSize en prod)
  if (taskHandle) {
    doc["mqtt_task_stack_hwm"] = uxTaskGetStackHighWaterMark(taskHandle);
  }

  String payload;
  serializeJson(doc, payload);
  safePublish(topics.diagnosticTopic.c_str(), payload.c_str(), true);
  systemLogger.debug("Diagnostic publié");
}

// ============================================================================
// Réception : messageCallback (s'exécute dans mqttTask via mqtt.loop())
// → poste les commandes dans inQueue. JAMAIS d'action directe sur les actuateurs ici.
// ============================================================================

void MqttManager::messageCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  InboundCmd cmd;
  cmd.payload[0] = '\0';

  // Copie du payload tronqué
  size_t copyLen = (length < sizeof(cmd.payload) - 1) ? length : sizeof(cmd.payload) - 1;
  for (size_t i = 0; i < copyLen; ++i) {
    cmd.payload[i] = static_cast<char>(payload[i]);
  }
  cmd.payload[copyLen] = '\0';

  if      (topicStr == topics.filtrationModeCommand) cmd.type = InboundCmdType::FiltrationMode;
  else if (topicStr == topics.filtrationCommand)     cmd.type = InboundCmdType::FiltrationOnOff;
  else if (topicStr == topics.lightingCommand)       cmd.type = InboundCmdType::Lighting;
  else if (topicStr == topics.phTargetCommand)       cmd.type = InboundCmdType::PhTarget;
  else if (topicStr == topics.orpTargetCommand)      cmd.type = InboundCmdType::OrpTarget;
  else return;

  if (inQueue == nullptr) return;
  if (xQueueSend(inQueue, &cmd, 0) != pdTRUE) {
    systemLogger.warning("MQTT inQueue saturée — commande HA abandonnée");
  }
}

// ============================================================================
// Drainage des commandes HA — appelé depuis loopTask à chaque tour de loop()
// Toutes les actions sur les actuateurs s'exécutent ici, sous configMutex.
// ============================================================================

void MqttManager::drainCommandQueue() {
  if (inQueue == nullptr) return;

  // Limite par tour pour éviter de saturer loopTask en cas de rafale.
  constexpr int kMaxCmdPerIter = 8;
  for (int i = 0; i < kMaxCmdPerIter; ++i) {
    InboundCmd cmd;
    if (xQueueReceive(inQueue, &cmd, 0) != pdTRUE) return;

    String payloadStr(cmd.payload);
    payloadStr.trim();

    switch (cmd.type) {
      case InboundCmdType::FiltrationMode: {
        payloadStr.toLowerCase();
        if (payloadStr == "auto" || payloadStr == "manual" || payloadStr == "force" || payloadStr == "off") {
          xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
          if (filtrationCfg.mode != payloadStr) {
            filtrationCfg.mode = payloadStr;
            filtration.ensureTimesValid();
            if (filtrationCfg.mode == "auto") {
              filtration.computeAutoSchedule();
            }
            saveMqttConfig();
            systemLogger.info("Mode filtration changé: " + payloadStr);
          }
          xSemaphoreGiveRecursive(configMutex);
          publishFiltrationState();
        }
        break;
      }
      case InboundCmdType::FiltrationOnOff: {
        payloadStr.toUpperCase();
        xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
        if (payloadStr == "ON") {
          filtrationCfg.forceOn = true;
          filtrationCfg.forceOff = false;
          systemLogger.info("Filtration forcée ON (MQTT)");
        } else if (payloadStr == "OFF") {
          filtrationCfg.forceOn = false;
          filtrationCfg.forceOff = true;
          systemLogger.info("Filtration forcée OFF (MQTT)");
        }
        xSemaphoreGiveRecursive(configMutex);
        // Pas de publish ici : filtration.update() va publier après changement réel du relais.
        break;
      }
      case InboundCmdType::Lighting: {
        payloadStr.toUpperCase();
        if (payloadStr == "ON") {
          lighting.setManualOn();
        } else if (payloadStr == "OFF") {
          lighting.setManualOff();
        }
        publishLightingState();
        break;
      }
      case InboundCmdType::PhTarget: {
        float value = payloadStr.toFloat();
        if (value >= 6.0f && value <= 8.5f) {
          xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
          mqttCfg.phTarget = value;
          saveMqttConfig();
          xSemaphoreGiveRecursive(configMutex);
          publishTargetState();
          systemLogger.info("Consigne pH changée via MQTT: " + String(value, 1));
        } else {
          systemLogger.warning("Consigne pH invalide (MQTT): " + payloadStr);
        }
        break;
      }
      case InboundCmdType::OrpTarget: {
        float value = payloadStr.toFloat();
        if (value >= 400.0f && value <= 900.0f) {
          xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
          mqttCfg.orpTarget = value;
          saveMqttConfig();
          xSemaphoreGiveRecursive(configMutex);
          publishTargetState();
          systemLogger.info("Consigne ORP changée via MQTT: " + String(value, 0));
        } else {
          systemLogger.warning("Consigne ORP invalide (MQTT): " + payloadStr);
        }
        break;
      }
    }
  }
}

// ============================================================================
// Arrêt propre avant ESP.restart() — publie status=offline avec timeout court.
// ============================================================================

void MqttManager::shutdownForRestart() {
  if (taskHandle == nullptr) return;

  systemLogger.info("MQTT shutdown — flush status=offline");

  // Demande à mqttTask de s'arrêter et publie le status=offline.
  // On ne peut PAS publier directement depuis loopTask sans risquer le blocage qu'on
  // cherche justement à éviter — donc on enfile dans outQueue, on laisse mqttTask
  // drainer pendant kMqttOfflineFlushMs, puis on stoppe la tâche proprement.
  enqueueOutbound(topics.statusTopic, "offline", true);

  unsigned long deadline = millis() + kMqttOfflineFlushMs;
  while (millis() < deadline) {
    if (uxQueueMessagesWaiting(outQueue) == 0) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  taskShouldStop.store(true, std::memory_order_relaxed);
  // Attendre que la tâche sorte de taskLoop() — borne 200 ms supplémentaires.
  unsigned long stopDeadline = millis() + 200;
  while (millis() < stopDeadline && eTaskGetState(taskHandle) != eDeleted) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // Si toujours en vie, on laisse — vTaskDelete sera appelé en sortie de mqttTaskFunction.
  systemLogger.info("MQTT shutdown terminé");
}

// ============================================================================
// publishDiscovery — exécuté UNIQUEMENT depuis mqttTask (lors de la connect réussie)
// ============================================================================

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
    // safePublish() retourne false silencieusement si déconnecté, et reset le wdt avant publish.
    String payload;
    serializeJson(doc, payload);
    bool ok = safePublish(configTopic.c_str(), payload.c_str(), true);
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

  // Stock pH faible
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_ph_stock_low/config";
  doc["name"] = "Stock pH Faible";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_stock_low";
  doc["state_topic"] = topics.phStockLowState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "problem";
  doc["icon"] = "mdi:bottle-tonic-outline";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Stock ORP faible
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_orp_stock_low/config";
  doc["name"] = "Stock Chlore Faible";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_stock_low";
  doc["state_topic"] = topics.orpStockLowState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "problem";
  doc["icon"] = "mdi:bottle-tonic-outline";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Volume pH restant
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_remaining/config";
  doc["name"] = "Volume pH Restant";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_remaining";
  doc["state_topic"] = topics.phRemainingState;
  doc["unit_of_measurement"] = "mL";
  doc["icon"] = "mdi:cup-water";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Volume ORP restant
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_orp_remaining/config";
  doc["name"] = "Volume Chlore Restant";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_remaining";
  doc["state_topic"] = topics.orpRemainingState;
  doc["unit_of_measurement"] = "mL";
  doc["icon"] = "mdi:cup-water";
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
