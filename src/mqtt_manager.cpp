#include "mqtt_manager.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "lighting.h"
#include "version.h"
#include "pump_controller.h"
#include "ws_manager.h"  // feature bug-sync-ws-config : notifier l'UI d'un changement config via MQTT
#include "schedule_logic.h"  // feature-051 : validation HH:MM (timeStringToMinutes)
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>

using mqtt_internal::OutboundMsg;
using mqtt_internal::InboundCmd;
using mqtt_internal::InboundCmdType;

namespace {
// feature-027 : warn throttlé (max 1/min par site — lastWarnMs = statique locale du site)
// sur timeout de prise du configMutex.
void warnConfigMutexTimeout(unsigned long& lastWarnMs, const char* site) {
  unsigned long nowMs = millis();
  if (lastWarnMs == 0 || nowMs - lastWarnMs >= kMutexTimeoutWarnThrottleMs) {
    systemLogger.warning(String("[MQTT] ") + site + ": timeout mutex config — opération sautée");
    lastWarnMs = nowMs;
  }
}

// feature-050 : validation stricte d'un payload numérique (pattern 009 : payload
// vide/texte → warning + ignore côté appelant). Accepte "150", "150.0", "-5"
// (HA number publie str(float) → "150.0") ; refuse "", "abc", "12a".
bool isNumericPayload(const String& s) {
  if (s.length() == 0) return false;
  unsigned int i = (s[0] == '-' || s[0] == '+') ? 1u : 0u;
  bool digitSeen = false;
  for (; i < s.length(); ++i) {
    if (s[i] >= '0' && s[i] <= '9') { digitSeen = true; continue; }
    if (s[i] == '.') continue;
    return false;
  }
  return digitSeen;
}
}  // namespace

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
  topics.temperatureCircuitState = base + "/temperature_circuit";  // feature-020
  topics.phState = base + "/ph";
  topics.orpState = base + "/orp";
  topics.filtrationState = base + "/filtration_state";
  topics.filtrationModeState = base + "/filtration_mode";
  topics.filtrationModeCommand = base + "/filtration_mode/set";
  topics.filtrationCommand = base + "/filtration/set";
  topics.filtrationStartState = base + "/filtration_start";       // feature-051
  topics.filtrationEndState = base + "/filtration_end";           // feature-051
  topics.filtrationStartCommand = base + "/filtration_start/set"; // feature-051
  topics.filtrationEndCommand = base + "/filtration_end/set";     // feature-051
  topics.lightingState = base + "/lighting_state";
  topics.lightingCommand = base + "/lighting/set";
  topics.lightingScheduleState = base + "/lighting_schedule";        // feature-052
  topics.lightingScheduleCommand = base + "/lighting_schedule/set";  // feature-052
  topics.lightingStartState = base + "/lighting_start";             // feature-052
  topics.lightingEndState = base + "/lighting_end";                 // feature-052
  topics.lightingStartCommand = base + "/lighting_start/set";       // feature-052
  topics.lightingEndCommand = base + "/lighting_end/set";           // feature-052
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
  // feature-009 : commandes HA de changement de mode de régulation
  topics.phRegulationModeCommand  = base + "/ph_regulation_mode/set";
  topics.orpRegulationModeCommand = base + "/orp_regulation_mode/set";
  topics.alertsTopic = base + "/alerts";
  topics.logsTopic = base + "/logs";
  topics.statusTopic = base + "/status";
  topics.diagnosticTopic = base + "/diagnostic";
  // feature-021 : alertes calibration / capteur stale + états cal points
  topics.alertsCalibrationTopic = base + "/alerts/calibration_required";
  topics.alertsSensorStaleTopic = base + "/alerts/sensor_stale";
  // feature-022 : alerte capteur pH/ORP figé (variance nulle)
  topics.alertsSensorFrozenTopic = base + "/alerts/sensor_frozen";
  // feature-022 Passe 1 : états binaires ON/OFF par capteur (stale OU figé)
  topics.phSensorProblemState  = base + "/ph_sensor_problem";
  topics.orpSensorProblemState = base + "/orp_sensor_problem";
  topics.phCalPointsState       = base + "/ph_cal_points";
  topics.orpCalPointsState      = base + "/orp_cal_points";
  // feature-024 : pente sonde pH (3 topics retain).
  topics.phSlopeAcidState       = base + "/ph_slope_acid";
  topics.phSlopeBaseState       = base + "/ph_slope_base";
  topics.phSlopeZeroState       = base + "/ph_slope_zero";
  // feature-025 : chaîne de filtrage pH/ORP (médiane + EMA + rejet pics).
  topics.phRawState                 = base + "/ph_raw";
  topics.phMedianState              = base + "/ph_median";
  topics.phFilteredState            = base + "/ph_filtered";
  topics.phFilterReadyState         = base + "/ph_filter_ready";
  topics.phFilterUnstableState      = base + "/ph_filter_unstable";
  topics.phRejectedCountState       = base + "/ph_rejected_count";
  topics.orpRawState                = base + "/orp_raw";
  topics.orpMedianState             = base + "/orp_median";
  topics.orpFilteredState           = base + "/orp_filtered";
  topics.orpFilterReadyState        = base + "/orp_filter_ready";
  topics.orpFilterUnstableState     = base + "/orp_filter_unstable";
  topics.orpRejectedCountState      = base + "/orp_rejected_count";
  topics.phMixingDelayActiveState   = base + "/ph_mixing_delay_active";
  topics.orpMixingDelayActiveState  = base + "/orp_mixing_delay_active";
  // feature-050 : cumuls journaliers injectés + commandes volume quotidien + reboot.
  topics.phDailyMlState             = base + "/ph_daily_ml";
  topics.orpDailyMlState            = base + "/orp_daily_ml";
  topics.phDailyTargetMlCommand     = base + "/ph_daily_target_ml/set";
  topics.orpDailyTargetMlCommand    = base + "/orp_daily_target_ml/set";
  topics.rebootCommand              = base + "/reboot/set";
  // feature-053 : Mode Boost (switch ON/OFF).
  topics.boostState                 = base + "/boost";
  topics.boostCommand               = base + "/boost/set";
  // feature-056 : mode d'installation + signal filtration externe.
  topics.installModeState           = base + "/install_mode";
  topics.installModeCommand         = base + "/install_mode/set";
  topics.filtrationExternalStateCommand = base + "/filtration_external_state/set";
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
    mqtt.subscribe(topics.phRegulationModeCommand.c_str());
    mqtt.subscribe(topics.orpRegulationModeCommand.c_str());
    // feature-050 : volume quotidien programmé (pH/ORP) + bouton reboot
    mqtt.subscribe(topics.phDailyTargetMlCommand.c_str());
    mqtt.subscribe(topics.orpDailyTargetMlCommand.c_str());
    mqtt.subscribe(topics.rebootCommand.c_str());
    // feature-051 : heures de filtration
    mqtt.subscribe(topics.filtrationStartCommand.c_str());
    mqtt.subscribe(topics.filtrationEndCommand.c_str());
    // feature-052 : programmation + heures d'éclairage
    mqtt.subscribe(topics.lightingScheduleCommand.c_str());
    mqtt.subscribe(topics.lightingStartCommand.c_str());
    mqtt.subscribe(topics.lightingEndCommand.c_str());
    mqtt.subscribe(topics.boostCommand.c_str());  // feature-053
    // feature-056 : mode d'installation + signal filtration externe
    mqtt.subscribe(topics.installModeCommand.c_str());
    mqtt.subscribe(topics.filtrationExternalStateCommand.c_str());

    discoveryPublished = false;
    esp_task_wdt_reset();
    publishDiscovery();              // ~34 publish — long si CPL lossy
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
  // feature-051 : heures courantes (recalculées par la température en mode auto)
  enqueueOutbound(topics.filtrationStartState, filtrationCfg.start, true);
  enqueueOutbound(topics.filtrationEndState, filtrationCfg.end, true);
}

void MqttManager::publishLightingState() {
  enqueueOutbound(topics.lightingState, lighting.isOn() ? "ON" : "OFF", true);
  // feature-052 : programmation + heures d'éclairage (miroir filtration)
  enqueueOutbound(topics.lightingScheduleState, lightingCfg.scheduleEnabled ? "ON" : "OFF", true);
  enqueueOutbound(topics.lightingStartState, lightingCfg.startTime, true);
  enqueueOutbound(topics.lightingEndState, lightingCfg.endTime, true);
}

void MqttManager::publishDosingState() {
  enqueueOutbound(topics.phDosingState,  PumpController.isPhDosing()  ? "ON" : "OFF", true);
  enqueueOutbound(topics.orpDosingState, PumpController.isOrpDosing() ? "ON" : "OFF", true);
  enqueueOutbound(topics.phLimitState,   safetyLimits.phLimitReached  ? "ON" : "OFF", true);
  enqueueOutbound(topics.orpLimitState,  safetyLimits.orpLimitReached ? "ON" : "OFF", true);
}

void MqttManager::publishBoostState() {
  // feature-053 : reflète l'état effectif du Boost (isBoostActive expire à minuit
  // même si le flag persisté n'a pas encore été nettoyé). Retain pour HA.
  enqueueOutbound(topics.boostState, isBoostActive(time(nullptr)) ? "ON" : "OFF", true);
}

void MqttManager::publishProductState() {
  // feature-027 : timeout → snapshot sauté, republication naturelle au prochain cycle (retain)
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnProductMs = 0;
    warnConfigMutexTimeout(sWarnProductMs, "publishProductState");
    return;
  }
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
  // feature-027 : timeout → snapshot sauté, republication naturelle au prochain cycle (retain)
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnTargetMs = 0;
    warnConfigMutexTimeout(sWarnTargetMs, "publishTargetState");
    return;
  }
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
  float tCircuit = sensors.getCircuitTemperature();   // feature-020
  float ph = sensors.getPh();
  float orp = sensors.getOrp();

  // Tous les publish passent par safePublish() : reset wdt + check connected interne.
  // Plus besoin de garde-fous intermédiaires ni de wdt reset explicites — voir IT4 / ADR-0011.
  if (!isnan(t)) {
    String p = String(t, 1);
    safePublish(topics.temperatureState.c_str(), p.c_str(), true);
  }
  if (!isnan(tCircuit)) {
    String p = String(tCircuit, 1);
    safePublish(topics.temperatureCircuitState.c_str(), p.c_str(), true);
  }
  // feature-021 spec ligne 247 : pH publié avec 3 décimales (l'EZO rend 3 décimales fiables)
  if (!isnan(ph)) {
    String p = String(ph, 3);
    safePublish(topics.phState.c_str(), p.c_str(), true);
  }
  if (!isnan(orp)) {
    String p = String(orp, 0);  // ORP entier (mV)
    safePublish(topics.orpState.c_str(), p.c_str(), true);
  }

  // Filtration / lighting / dosing
  safePublish(topics.filtrationModeState.c_str(), filtrationCfg.mode.c_str(), true);
  safePublish(topics.filtrationState.c_str(), filtration.isRunning() ? "ON" : "OFF", true);
  safePublish(topics.filtrationStartState.c_str(), filtrationCfg.start.c_str(), true);  // feature-051
  safePublish(topics.filtrationEndState.c_str(), filtrationCfg.end.c_str(), true);      // feature-051
  safePublish(topics.lightingState.c_str(), lighting.isOn() ? "ON" : "OFF", true);
  safePublish(topics.lightingScheduleState.c_str(), lightingCfg.scheduleEnabled ? "ON" : "OFF", true);  // feature-052
  safePublish(topics.lightingStartState.c_str(), lightingCfg.startTime.c_str(), true);                  // feature-052
  safePublish(topics.lightingEndState.c_str(), lightingCfg.endTime.c_str(), true);                      // feature-052

  safePublish(topics.boostState.c_str(), isBoostActive(time(nullptr)) ? "ON" : "OFF", true);  // feature-053

  // feature-056 : mode d'installation (managed/powered/external, retain)
  safePublish(topics.installModeState.c_str(), installModeToString(mqttCfg.installMode), true);

  safePublish(topics.phDosingState.c_str(),  PumpController.isPhDosing()  ? "ON" : "OFF", true);
  safePublish(topics.orpDosingState.c_str(), PumpController.isOrpDosing() ? "ON" : "OFF", true);
  safePublish(topics.phLimitState.c_str(),   safetyLimits.phLimitReached  ? "ON" : "OFF", true);
  safePublish(topics.orpLimitState.c_str(),  safetyLimits.orpLimitReached ? "ON" : "OFF", true);

  // feature-050 : cumuls journaliers injectés (mL, 1 décimale, retain, dédupliqués
  // par valeur mémorisée — slots 14/15). Remis à 0 à minuit par le pump_controller.
  safePublishDedup(14, topics.phDailyMlState.c_str(),  String(safetyLimits.dailyPhInjectedMl, 1));
  safePublishDedup(15, topics.orpDailyMlState.c_str(), String(safetyLimits.dailyOrpInjectedMl, 1));

  // Product / target sous configMutex — snapshot puis publish hors verrou.
  // feature-027 : snapshot atomique ou rien. Timeout → seul ce bloc est sauté
  // (les publish précédents restent envoyés, les suivants s'exécutent) ;
  // republication naturelle au prochain cycle (retain).
  float phRemaining, orpRemaining;
  bool phStockLow, orpStockLow;
  float phT, orpT;
  String phMode, orpMode;
  int phDaily, orpDaily;
  bool snapshotOk = true;
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    snapshotOk = false;
    static unsigned long sWarnAllStatesMs = 0;
    warnConfigMutexTimeout(sWarnAllStatesMs, "publishAllStatesInternal");
  }
  if (snapshotOk) {
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

  // feature-021 : statut calibration EZO + alertes (cf. cond #4 pool-chemistry).
  publishCalibrationStatusInternal();

  // feature-025 : chaîne de filtrage pH/ORP (raw/median/filtered/ready/unstable/rejected).
  publishFilterStatesInternal();
}

// =============================================================================
// feature-025 : publication des états de la chaîne de filtrage pH/ORP
// =============================================================================
//
// Topics retain=true, dédupliqués (anti-spam) : on ne republie un topic que si
// sa valeur arrondie a changé depuis la dernière publication (cache _lastFilterPub).
// Suit le rythme MQTT existant (appelé depuis publishAllStatesInternal, cadencé par
// publishStatesRequested toutes les kMqttPublishIntervalMs).
// Valeurs NaN/indisponibles : non publiées (le retain précédent reste valable côté HA).
void MqttManager::safePublishDedup(int cacheIdx, const char* topic, const String& payload) {
  if (cacheIdx >= 0 && cacheIdx < kDedupCacheSlots && _lastFilterPub[cacheIdx] == payload) return;
  if (safePublish(topic, payload.c_str(), true) && cacheIdx >= 0 && cacheIdx < kDedupCacheSlots) {
    _lastFilterPub[cacheIdx] = payload;
  }
}

void MqttManager::publishFilterStatesInternal() {
  if (!mqtt.connected()) return;

  // Lectures atomiques (float scalaires / bool) — pas d'I²C ici.
  float phRaw = sensors.getPhRaw();
  float phMed = sensors.getPhMedian();
  float phFil = sensors.getPhFiltered();
  float orpRaw = sensors.getOrpRaw();
  float orpMed = sensors.getOrpMedian();
  float orpFil = sensors.getOrpFiltered();
  uint32_t nowMs = millis();

  // pH (3 décimales, alignement WS/REST) — uniquement si non NaN.
  if (!isnan(phRaw)) safePublishDedup(0, topics.phRawState.c_str(),      String(phRaw, 3));
  if (!isnan(phMed)) safePublishDedup(1, topics.phMedianState.c_str(),   String(phMed, 3));
  if (!isnan(phFil)) safePublishDedup(2, topics.phFilteredState.c_str(), String(phFil, 3));
  safePublishDedup(3, topics.phFilterReadyState.c_str(),    sensors.isPhFilterReady()    ? "ON" : "OFF");
  safePublishDedup(4, topics.phFilterUnstableState.c_str(), sensors.isPhFilterUnstable() ? "ON" : "OFF");
  safePublishDedup(5, topics.phRejectedCountState.c_str(),  String(sensors.getPhRejectedCount()));

  // ORP (entier mV) — uniquement si non NaN.
  if (!isnan(orpRaw)) safePublishDedup(6, topics.orpRawState.c_str(),      String(orpRaw, 0));
  if (!isnan(orpMed)) safePublishDedup(7, topics.orpMedianState.c_str(),   String(orpMed, 0));
  if (!isnan(orpFil)) safePublishDedup(8, topics.orpFilteredState.c_str(), String(orpFil, 0));
  safePublishDedup(9,  topics.orpFilterReadyState.c_str(),    sensors.isOrpFilterReady()    ? "ON" : "OFF");
  safePublishDedup(10, topics.orpFilterUnstableState.c_str(), sensors.isOrpFilterUnstable() ? "ON" : "OFF");
  safePublishDedup(11, topics.orpRejectedCountState.c_str(),  String(sensors.getOrpRejectedCount()));

  // Pause mélange hydraulique active (post-injection).
  safePublishDedup(12, topics.phMixingDelayActiveState.c_str(),  PumpController.isPhMixingDelayActive(nowMs)  ? "ON" : "OFF");
  safePublishDedup(13, topics.orpMixingDelayActiveState.c_str(), PumpController.isOrpMixingDelayActive(nowMs) ? "ON" : "OFF");
}

// =============================================================================
// feature-021 : publication edge-triggered alerte calibration + sensor_stale
// =============================================================================
//
// Topics retenus (retain=true) :
//   - {base}/ph_cal_points / {base}/orp_cal_points : entier (-1..3)
//   - {base}/alerts/calibration_required : JSON si pH<2 OU ORP<1 ; payload vide sinon
//   - {base}/alerts/sensor_stale         : JSON si pH OU ORP NaN ; payload vide sinon
//
// La méthode est idempotente : on republie l'état des cal points à chaque appel
// (il s'agit d'entiers retain), mais on ne publie l'alerte que sur transition
// (cache _lastPhCalPoints / _lastOrpCalPoints / _lastSensorStale) pour limiter
// le trafic MQTT et le bruit dans les logs HA.
//
// Appelée :
//   - À chaque publishAllStatesInternal() (cadencé par publishStatesRequested,
//     posé toutes les kMqttPublishIntervalMs depuis loopTask).
//   - Lors d'une (re)connexion MQTT (connectInTask) — état initial après reboot.
void MqttManager::publishCalibrationStatusInternal() {
  if (!mqtt.connected()) return;

  // Lectures via cache (mises à jour en begin() puis à chaque calibration EZO).
  // Pas d'appel I²C ici : on évite ~1.8 s de bus monopolisé par cycle MQTT (10 s).
  // En cas de désynchro improbable cache vs réalité, le prochain cycle de
  // calibration ou un boot resync remettra les valeurs à jour.
  int phCal  = sensors.getPhCalibrationPointsCached();
  int orpCal = sensors.getOrpCalibrationPointsCached();
  bool phStale  = isnan(sensors.getPh());
  bool orpStale = isnan(sensors.getOrp());

  // 1) États bruts cal points (toujours retain — HA peut filtrer -1)
  safePublish(topics.phCalPointsState.c_str(),  String(phCal).c_str(),  true);
  safePublish(topics.orpCalPointsState.c_str(), String(orpCal).c_str(), true);

  // 2) Alerte calibration_required — edge-triggered sur transition cal points
  bool needsCal = (phCal < 2) || (orpCal < 1);
  bool calChanged = (phCal != _lastPhCalPoints) || (orpCal != _lastOrpCalPoints);
  if (calChanged) {
    if (needsCal) {
      JsonDocument doc;
      doc["type"] = "calibration_required";
      doc["phCalPoints"]  = phCal;
      doc["orpCalPoints"] = orpCal;
      doc["timestamp"]    = millis();
      String payload;
      serializeJson(doc, payload);
      safePublish(topics.alertsCalibrationTopic.c_str(), payload.c_str(), true);
      systemLogger.warning("MQTT alerte calibration_required publiée (pH=" +
                           String(phCal) + ", ORP=" + String(orpCal) + ")");
    } else {
      // Clear retain : payload vide
      safePublish(topics.alertsCalibrationTopic.c_str(), "", true);
      systemLogger.info("MQTT alerte calibration_required clearée (calibration OK)");
    }
    _lastPhCalPoints  = phCal;
    _lastOrpCalPoints = orpCal;
  }

  // 3) Alerte sensor_stale — edge-triggered sur transition NaN
  bool isStale = phStale || orpStale;
  if (isStale != _lastSensorStale) {
    if (isStale) {
      JsonDocument doc;
      doc["type"] = "sensor_stale";
      doc["phStale"]   = phStale;
      doc["orpStale"]  = orpStale;
      doc["timestamp"] = millis();
      String payload;
      serializeJson(doc, payload);
      safePublish(topics.alertsSensorStaleTopic.c_str(), payload.c_str(), true);
      systemLogger.warning(String("MQTT alerte sensor_stale publiée (pH=") +
                           (phStale ? "NaN" : "OK") + ", ORP=" +
                           (orpStale ? "NaN" : "OK") + ")");
    } else {
      safePublish(topics.alertsSensorStaleTopic.c_str(), "", true);
      systemLogger.info("MQTT alerte sensor_stale clearée");
    }
    _lastSensorStale = isStale;
  }

  // 3bis) feature-022 : alerte sensor_frozen — edge-triggered sur transition figé.
  // Calquée sur le bloc sensor_stale ci-dessus (JSON retain, clear payload vide).
  // La température N'EST PAS dans ce payload : sévérité différente (warning-only,
  // aucun impact dosage) — seuls pH/ORP figés inhibent la régulation auto.
  bool phFrozen  = sensors.isPhSensorFrozen();
  bool orpFrozen = sensors.isOrpSensorFrozen();
  bool isFrozen = phFrozen || orpFrozen;
  if (isFrozen != _lastSensorFrozen) {
    if (isFrozen) {
      JsonDocument doc;
      doc["type"] = "sensor_frozen";
      doc["phFrozen"]  = phFrozen;
      doc["orpFrozen"] = orpFrozen;
      doc["timestamp"] = millis();
      String payload;
      serializeJson(doc, payload);
      safePublish(topics.alertsSensorFrozenTopic.c_str(), payload.c_str(), true);
      systemLogger.warning(String("MQTT alerte sensor_frozen publiée (pH=") +
                           (phFrozen ? "FIGÉ" : "OK") + ", ORP=" +
                           (orpFrozen ? "FIGÉ" : "OK") + ")");
    } else {
      safePublish(topics.alertsSensorFrozenTopic.c_str(), "", true);
      systemLogger.info("MQTT alerte sensor_frozen clearée");
    }
    _lastSensorFrozen = isFrozen;
  }

  // 3ter) feature-022 Passe 1 : états binaires par capteur — ON si stale (NaN)
  // OU figé, OFF sinon. Synthèse directement consommable par HA (binary_sensor
  // device_class "problem") sans parser les JSON d'alerte. Réutilise phStale/
  // orpStale (bloc 3) et phFrozen/orpFrozen (bloc 3bis). Dédupliqué par état
  // mémorisé (-1 = jamais publié → force la 1ʳᵉ publication), retain=true.
  int8_t phProblem  = (phStale || phFrozen)   ? 1 : 0;
  int8_t orpProblem = (orpStale || orpFrozen) ? 1 : 0;
  if (phProblem != _lastPhSensorProblem) {
    safePublish(topics.phSensorProblemState.c_str(), phProblem ? "ON" : "OFF", true);
    systemLogger.info(String("MQTT ph_sensor_problem → ") + (phProblem ? "ON" : "OFF"));
    _lastPhSensorProblem = phProblem;
  }
  if (orpProblem != _lastOrpSensorProblem) {
    safePublish(topics.orpSensorProblemState.c_str(), orpProblem ? "ON" : "OFF", true);
    systemLogger.info(String("MQTT orp_sensor_problem → ") + (orpProblem ? "ON" : "OFF"));
    _lastOrpSensorProblem = orpProblem;
  }

  // 4) feature-024 : pente sonde pH — publication edge-triggered.
  // Publié UNIQUEMENT après la 1ʳᵉ query Slope,? réussie (NaN check).
  // Chaque query réussie suivante (24h ou post-cal) re-publie si la valeur
  // arrondie a changé — évite le bruit MQTT pour des oscillations <0.1%.
  float slopeAcid = sensors.getPhSlopeAcid();
  float slopeBase = sensors.getPhSlopeBase();
  float slopeZero = sensors.getPhSlopeZero();
  if (!isnan(slopeAcid)) {
    float rounded = round(slopeAcid * 10.0f) / 10.0f;
    if (isnan(_lastPhSlopeAcidPub) || rounded != _lastPhSlopeAcidPub) {
      safePublish(topics.phSlopeAcidState.c_str(), String(rounded, 1).c_str(), true);
      _lastPhSlopeAcidPub = rounded;
    }
  }
  if (!isnan(slopeBase)) {
    float rounded = round(slopeBase * 10.0f) / 10.0f;
    if (isnan(_lastPhSlopeBasePub) || rounded != _lastPhSlopeBasePub) {
      safePublish(topics.phSlopeBaseState.c_str(), String(rounded, 1).c_str(), true);
      _lastPhSlopeBasePub = rounded;
    }
  }
  if (!isnan(slopeZero)) {
    float rounded = round(slopeZero * 100.0f) / 100.0f;
    if (isnan(_lastPhSlopeZeroPub) || rounded != _lastPhSlopeZeroPub) {
      safePublish(topics.phSlopeZeroState.c_str(), String(rounded, 2).c_str(), true);
      _lastPhSlopeZeroPub = rounded;
    }
  }
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
  // feature-021 : pH avec 3 décimales (cf. spec ligne 247 — l'EZO rend 3 décimales fiables)
  doc["ph_value"] = round(sensors.getPh() * 1000.0f) / 1000.0f;
  doc["orp_value"] = sensors.getOrp();
  doc["temperature"] = sensors.getTemperature();
  doc["temperature_circuit"] = sensors.getCircuitTemperature();  // feature-020
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
  else if (topicStr == topics.phRegulationModeCommand)  cmd.type = InboundCmdType::PhRegulationMode;
  else if (topicStr == topics.orpRegulationModeCommand) cmd.type = InboundCmdType::OrpRegulationMode;
  else if (topicStr == topics.phDailyTargetMlCommand)   cmd.type = InboundCmdType::PhDailyTarget;
  else if (topicStr == topics.orpDailyTargetMlCommand)  cmd.type = InboundCmdType::OrpDailyTarget;
  else if (topicStr == topics.rebootCommand)            cmd.type = InboundCmdType::Reboot;
  else if (topicStr == topics.filtrationStartCommand)   cmd.type = InboundCmdType::FiltrationStart;
  else if (topicStr == topics.filtrationEndCommand)     cmd.type = InboundCmdType::FiltrationEnd;
  else if (topicStr == topics.lightingScheduleCommand)  cmd.type = InboundCmdType::LightingSchedule;
  else if (topicStr == topics.lightingStartCommand)     cmd.type = InboundCmdType::LightingStart;
  else if (topicStr == topics.lightingEndCommand)       cmd.type = InboundCmdType::LightingEnd;
  else if (topicStr == topics.boostCommand)             cmd.type = InboundCmdType::Boost;
  else if (topicStr == topics.installModeCommand)       cmd.type = InboundCmdType::InstallMode;
  else if (topicStr == topics.filtrationExternalStateCommand)   cmd.type = InboundCmdType::FiltrationExternalState;
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
  // feature-050 : redémarrage différé demandé via MQTT/HA — même séquence propre que
  // la route POST /reboot (web_routes_config.cpp) : flag consommé côté loopTask après
  // kRestartApModeDelayMs, puis shutdownForRestart() (flush status=offline + arrêt
  // mqttTask, ADR-0011) et ESP.restart(). Jamais de restart direct au moment du drain.
  if (_rebootPending && millis() - _rebootRequestedAtMs >= kRestartApModeDelayMs) {
    _rebootPending = false;
    systemLogger.critical("Redémarrage (commande MQTT/HA)");
    shutdownForRestart();
    ESP.restart();
  }

  if (inQueue == nullptr) return;

  // bug-sync-ws-config : une commande HA qui modifie la config doit notifier les
  // clients WebSocket (sinon l'UI ne voit le changement qu'au reload). Le flag est
  // posé pour toute commande traitée sauf Reboot, et consommé une fois en fin de
  // drain (broadcast idempotent, coalescé). drainCommandQueue tourne sur loopTask,
  // comme la consommation du flag WS → écriture du booléen sûre.
  bool needConfigBroadcast = false;

  // Limite par tour pour éviter de saturer loopTask en cas de rafale.
  constexpr int kMaxCmdPerIter = 8;
  for (int i = 0; i < kMaxCmdPerIter; ++i) {
    InboundCmd cmd;
    if (xQueueReceive(inQueue, &cmd, 0) != pdTRUE) break;  // file vide → sortie unique

    if (cmd.type != InboundCmdType::Reboot) needConfigBroadcast = true;

    String payloadStr(cmd.payload);
    payloadStr.trim();

    switch (cmd.type) {
      case InboundCmdType::FiltrationMode: {
        payloadStr.toLowerCase();
        if (payloadStr == "auto" || payloadStr == "manual" || payloadStr == "force" || payloadStr == "off") {
          // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
          // resync HA sur l'état réel via publishFiltrationState().
          if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
            static unsigned long sWarnFiltModeMs = 0;
            warnConfigMutexTimeout(sWarnFiltModeMs, "cmd filtration_mode");
            publishFiltrationState();
            break;
          }
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
        // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
        // resync HA sur l'état réel du relais.
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnFiltOnOffMs = 0;
          warnConfigMutexTimeout(sWarnFiltOnOffMs, "cmd filtration_onoff");
          publishFiltrationState();
          break;
        }
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
          // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
          // resync HA sur la consigne réelle via publishTargetState().
          if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
            static unsigned long sWarnPhTargetMs = 0;
            warnConfigMutexTimeout(sWarnPhTargetMs, "cmd ph_target");
            publishTargetState();
            break;
          }
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
          // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
          // resync HA sur la consigne réelle via publishTargetState().
          if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
            static unsigned long sWarnOrpTargetMs = 0;
            warnConfigMutexTimeout(sWarnOrpTargetMs, "cmd orp_target");
            publishTargetState();
            break;
          }
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
      case InboundCmdType::PhRegulationMode: {
        // feature-009 : miroir exact de la logique web_routes_config.cpp (ADR-0004 :
        // phEnabled dérivé du mode — "manual" désactive la régulation).
        payloadStr.toLowerCase();
        if (payloadStr == "automatic" || payloadStr == "scheduled" || payloadStr == "manual") {
          // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
          // resync HA sur le mode réel via publishTargetState().
          if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
            static unsigned long sWarnPhRegModeMs = 0;
            warnConfigMutexTimeout(sWarnPhRegModeMs, "cmd ph_regulation_mode");
            publishTargetState();
            break;
          }
          const bool changed = (mqttCfg.phRegulationMode != payloadStr);
          if (changed) {
            mqttCfg.phRegulationMode = payloadStr;
            mqttCfg.phEnabled = (payloadStr != "manual");
            saveMqttConfig();
          }
          xSemaphoreGiveRecursive(configMutex);
          publishTargetState();
          if (changed) {
            systemLogger.info("Mode régulation pH changé (MQTT): " + payloadStr);
          }
        } else {
          systemLogger.warning("Mode régulation pH invalide (MQTT): " + payloadStr);
        }
        break;
      }
      case InboundCmdType::OrpRegulationMode: {
        payloadStr.toLowerCase();
        if (payloadStr == "automatic" || payloadStr == "scheduled" || payloadStr == "manual") {
          // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
          // resync HA sur le mode réel via publishTargetState().
          if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
            static unsigned long sWarnOrpRegModeMs = 0;
            warnConfigMutexTimeout(sWarnOrpRegModeMs, "cmd orp_regulation_mode");
            publishTargetState();
            break;
          }
          const bool changed = (mqttCfg.orpRegulationMode != payloadStr);
          if (changed) {
            mqttCfg.orpRegulationMode = payloadStr;
            mqttCfg.orpEnabled = (payloadStr != "manual");
            saveMqttConfig();
          }
          xSemaphoreGiveRecursive(configMutex);
          publishTargetState();
          if (changed) {
            systemLogger.info("Mode régulation ORP changé (MQTT): " + payloadStr);
          }
        } else {
          systemLogger.warning("Mode régulation ORP invalide (MQTT): " + payloadStr);
        }
        break;
      }
      case InboundCmdType::PhDailyTarget: {
        // feature-050 : volume quotidien pH (mode programmée) — pattern PhTarget.
        if (!isNumericPayload(payloadStr)) {
          systemLogger.warning("Volume quotidien pH invalide (MQTT): " + payloadStr);
          break;
        }
        int value = payloadStr.toInt();
        if (value < 0) value = 0;  // clamp négatif → 0 (0 = désactivé)
        // feature-027 : timeout → commande abandonnée (rien modifié avant le take),
        // resync HA sur la valeur réelle via publishTargetState().
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnPhDailyMs = 0;
          warnConfigMutexTimeout(sWarnPhDailyMs, "cmd ph_daily_target_ml");
          publishTargetState();
          break;
        }
        // Condition pool-chemistry feature-050 : validation contre la limite VIVE
        // (maxPhMlPerDay) lue au moment du drain — refuse toute cible au-dessus
        // du plafond journalier de sécurité configuré.
        const int maxMl = (int)safetyLimits.maxPhMlPerDay;
        if (safetyLimits.maxPhMlPerDay > 0 && value > maxMl) {
          xSemaphoreGiveRecursive(configMutex);
          systemLogger.warning("Volume quotidien pH refusé (MQTT): " + String(value) +
                               " mL > limite journalière " + String(maxMl) + " mL");
          publishTargetState();  // resync HA sur la valeur réelle
          break;
        }
        const bool changed = (mqttCfg.phDailyTargetMl != value);
        if (changed) {
          mqttCfg.phDailyTargetMl = value;
          saveMqttConfig();
        }
        xSemaphoreGiveRecursive(configMutex);
        publishTargetState();
        if (changed) {
          systemLogger.info("Volume quotidien pH changé (MQTT): " + String(value) + " mL");
        }
        break;
      }
      case InboundCmdType::OrpDailyTarget: {
        // feature-050 : volume quotidien ORP (mode programmée) — symétrique pH.
        if (!isNumericPayload(payloadStr)) {
          systemLogger.warning("Volume quotidien Chlore invalide (MQTT): " + payloadStr);
          break;
        }
        int value = payloadStr.toInt();
        if (value < 0) value = 0;  // clamp négatif → 0 (0 = désactivé)
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnOrpDailyMs = 0;
          warnConfigMutexTimeout(sWarnOrpDailyMs, "cmd orp_daily_target_ml");
          publishTargetState();
          break;
        }
        // Condition pool-chemistry feature-050 : limite VIVE maxChlorineMlPerDay
        // lue au moment du drain.
        const int maxMl = (int)safetyLimits.maxChlorineMlPerDay;
        if (safetyLimits.maxChlorineMlPerDay > 0 && value > maxMl) {
          xSemaphoreGiveRecursive(configMutex);
          systemLogger.warning("Volume quotidien Chlore refusé (MQTT): " + String(value) +
                               " mL > limite journalière " + String(maxMl) + " mL");
          publishTargetState();  // resync HA sur la valeur réelle
          break;
        }
        const bool changed = (mqttCfg.orpDailyTargetMl != value);
        if (changed) {
          mqttCfg.orpDailyTargetMl = value;
          saveMqttConfig();
        }
        xSemaphoreGiveRecursive(configMutex);
        publishTargetState();
        if (changed) {
          systemLogger.info("Volume quotidien Chlore changé (MQTT): " + String(value) + " mL");
        }
        break;
      }
      case InboundCmdType::Reboot: {
        // feature-050 : tout payload accepté (le bouton HA envoie "PRESS").
        // Redémarrage DIFFÉRÉ : flag consommé en tête de drainCommandQueue au
        // prochain tour de loop, après kRestartApModeDelayMs — même séquence
        // propre que la route POST /reboot (flush MQTT offline puis restart).
        systemLogger.warning("Redémarrage demandé via MQTT/HA");
        _rebootPending = true;
        _rebootRequestedAtMs = millis();
        break;
      }
      case InboundCmdType::FiltrationStart:
      case InboundCmdType::FiltrationEnd: {
        // feature-051 : heure de filtration (HH:MM). Validation via timeStringToMinutes
        // (-1 = format invalide). Écrit sous configMutex, efface les overrides manuels
        // (comme /save-config quand le planning change), applique via filtration.update(),
        // persiste, republie. En mode auto les heures seront recalculées par la
        // température au prochain update() → l'état republié reflète la valeur réelle.
        const bool isStart = (cmd.type == InboundCmdType::FiltrationStart);
        const char* label = isStart ? "début" : "fin";
        if (timeStringToMinutes(payloadStr.c_str()) < 0) {
          systemLogger.warning(String("Heure filtration ") + label + " invalide (MQTT): " + payloadStr);
          publishFiltrationState();  // resync HA sur la valeur réelle
          break;
        }
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnFiltTimeMs = 0;
          warnConfigMutexTimeout(sWarnFiltTimeMs, "cmd filtration_start/end");
          publishFiltrationState();
          break;
        }
        String& target = isStart ? filtrationCfg.start : filtrationCfg.end;
        if (target != payloadStr) {
          target = payloadStr;
          filtrationCfg.forceOn = false;   // le planning reprend effet immédiatement
          filtrationCfg.forceOff = false;
          saveMqttConfig();
          systemLogger.info(String("Heure filtration ") + label + " changée (MQTT): " + payloadStr);
        }
        xSemaphoreGiveRecursive(configMutex);
        filtration.update();          // applique le nouveau planning (recalcul si mode auto)
        publishFiltrationState();
        break;
      }
      case InboundCmdType::LightingSchedule: {
        // feature-052 : programmation éclairage (ON/OFF). Miroir de FiltrationOnOff :
        // écrit lightingCfg.scheduleEnabled sous configMutex, persiste, applique via
        // lighting.update(), republie. Payload invalide → warning + resync HA.
        payloadStr.toUpperCase();
        if (payloadStr != "ON" && payloadStr != "OFF") {
          systemLogger.warning("Programmation éclairage invalide (MQTT): " + payloadStr);
          publishLightingState();  // resync HA sur la valeur réelle
          break;
        }
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnLightSchedMs = 0;
          warnConfigMutexTimeout(sWarnLightSchedMs, "cmd lighting_schedule");
          publishLightingState();
          break;
        }
        const bool wanted = (payloadStr == "ON");
        if (lightingCfg.scheduleEnabled != wanted) {
          lightingCfg.scheduleEnabled = wanted;
          saveMqttConfig();
          systemLogger.info(String("Programmation éclairage ") + (wanted ? "activée" : "désactivée") + " (MQTT)");
        }
        xSemaphoreGiveRecursive(configMutex);
        lighting.update();
        publishLightingState();
        break;
      }
      case InboundCmdType::LightingStart:
      case InboundCmdType::LightingEnd: {
        // feature-052 : heure d'éclairage (HH:MM). Miroir de FiltrationStart/End :
        // validation via timeStringToMinutes (-1 = invalide), écriture sous configMutex,
        // persistance, application via lighting.update(), republish.
        const bool isStart = (cmd.type == InboundCmdType::LightingStart);
        const char* label = isStart ? "début" : "fin";
        if (timeStringToMinutes(payloadStr.c_str()) < 0) {
          systemLogger.warning(String("Heure éclairage ") + label + " invalide (MQTT): " + payloadStr);
          publishLightingState();  // resync HA sur la valeur réelle
          break;
        }
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnLightTimeMs = 0;
          warnConfigMutexTimeout(sWarnLightTimeMs, "cmd lighting_start/end");
          publishLightingState();
          break;
        }
        String& target = isStart ? lightingCfg.startTime : lightingCfg.endTime;
        if (target != payloadStr) {
          target = payloadStr;
          saveMqttConfig();
          systemLogger.info(String("Heure éclairage ") + label + " changée (MQTT): " + payloadStr);
        }
        xSemaphoreGiveRecursive(configMutex);
        lighting.update();
        publishLightingState();
        break;
      }
      case InboundCmdType::Boost: {
        // feature-053 : Mode Boost (ON/OFF). start/stopBoost prennent configMutex en
        // interne (saveBoostState) et s'exécutent ici sur loopTask — jamais d'appel
        // MQTT direct. startBoost refuse si l'heure n'est pas synchronisée (log warning).
        // Republication de l'état réel via publishBoostState (resync HA sur payload
        // invalide ou refus). Le broadcast WS config est assuré par needConfigBroadcast.
        payloadStr.toUpperCase();
        if (payloadStr == "ON") {
          startBoost();
          systemLogger.info("[Boost] Activé via MQTT/HA");
        } else if (payloadStr == "OFF") {
          stopBoost();
          systemLogger.info("[Boost] Désactivé via MQTT/HA");
        } else {
          systemLogger.warning("Commande Boost invalide (MQTT): " + payloadStr);
        }
        publishBoostState();
        break;
      }
      case InboundCmdType::InstallMode: {
        // feature-056 : mode d'installation (managed/powered/external). Miroir de la
        // route /save-config : parse strict, écrit sous configMutex, persiste, republie.
        payloadStr.toLowerCase();
        InstallMode parsed = installModeFromString(payloadStr.c_str(), mqttCfg.installMode);
        if (payloadStr != installModeToString(parsed)) {
          systemLogger.warning("Mode d'installation invalide (MQTT): " + payloadStr);
          enqueueOutbound(topics.installModeState, installModeToString(mqttCfg.installMode), true);
          break;
        }
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
          static unsigned long sWarnInstallModeMs = 0;
          warnConfigMutexTimeout(sWarnInstallModeMs, "cmd install_mode");
          enqueueOutbound(topics.installModeState, installModeToString(mqttCfg.installMode), true);
          break;
        }
        const bool changed = (mqttCfg.installMode != parsed);
        if (changed) {
          mqttCfg.installMode = parsed;
          saveMqttConfig();
        }
        xSemaphoreGiveRecursive(configMutex);
        if (changed) {
          filtration.update();  // applique l'inertie du relais selon le nouveau mode
          systemLogger.info("Mode d'installation changé (MQTT): " + payloadStr);
        }
        enqueueOutbound(topics.installModeState, installModeToString(mqttCfg.installMode), true);
        break;
      }
      case InboundCmdType::FiltrationExternalState: {
        // feature-056 : signal d'état de la filtration externe (mode ExternalFiltration).
        // Même effet que POST /filtration/external-state. setExternalState est thread-safe
        // (spinlock interne, horodatage millis()) — appel direct sûr depuis loopTask.
        payloadStr.toUpperCase();
        if (payloadStr == "ON") {
          filtration.setExternalState(true);
        } else if (payloadStr == "OFF") {
          filtration.setExternalState(false);
        } else {
          systemLogger.warning("Signal filtration externe invalide (MQTT): " + payloadStr);
        }
        break;
      }
    }
  }

  // bug-sync-ws-config : notifier l'UI web du changement de config appliqué via HA
  // (broadcast WS "config" au prochain cycle, ≤ 5 s). Sans ça, l'UI ne voit le
  // changement qu'au rechargement de page.
  if (needConfigBroadcast) wsManager.requestConfigBroadcast();
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

  // Température circuit (feature-020 : 2ᵉ sonde DS18B20 sur PCB v2)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_temperature_circuit/config";
  doc["name"] = "Piscine Température Circuit";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_temperature_circuit";
  doc["state_topic"] = topics.temperatureCircuitState;
  doc["device_class"] = "temperature";
  doc["unit_of_measurement"] = "°C";
  doc["state_class"] = "measurement";
  doc["icon"] = "mdi:chip";
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
  // bug-ha-filtration-mode-labels : libellés français dans HA, protocole MQTT
  // inchangé sur le fil. value_template traduit l'état brut publié → libellé ;
  // command_template retraduit le libellé choisi → valeur brute envoyée au /set
  // (le handler drainCommandQueue continue de recevoir auto/manual/force/off).
  // Sémantique : manual = « Programmation » (créneau à heures fixées), force =
  // « Manuel » (contrôle ON/OFF sans planning) — d'où la levée d'ambiguïté.
  JsonArray options = doc["options"].to<JsonArray>();
  options.add("Auto");
  options.add("Programmation");
  options.add("Manuel");
  options.add("Désactivé");
  doc["value_template"] =
    "{{ {'auto':'Auto','manual':'Programmation','force':'Manuel','off':'Désactivé'}.get(value, value) }}";
  doc["command_template"] =
    "{{ {'Auto':'auto','Programmation':'manual','Manuel':'force','Désactivé':'off'}[value] }}";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);  // publishConfig fait doc.clear() → pas de fuite vers les blocs suivants

  // feature-051 : heures de filtration (text éditable HH:MM). Effet réel en mode
  // Programmation ; en mode Auto elles sont recalculées par la température.
  topic = discoveryBase + "text/" + HA_DEVICE_ID + "_filtration_start/config";
  doc["name"] = "Filtration début";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_filtration_start";
  doc["state_topic"] = topics.filtrationStartState;
  doc["command_topic"] = topics.filtrationStartCommand;
  doc["pattern"] = "^([01][0-9]|2[0-3]):[0-5][0-9]$";
  doc["min"] = 5;
  doc["max"] = 5;
  doc["icon"] = "mdi:clock-start";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  topic = discoveryBase + "text/" + HA_DEVICE_ID + "_filtration_end/config";
  doc["name"] = "Filtration fin";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_filtration_end";
  doc["state_topic"] = topics.filtrationEndState;
  doc["command_topic"] = topics.filtrationEndCommand;
  doc["pattern"] = "^([01][0-9]|2[0-3]):[0-5][0-9]$";
  doc["min"] = 5;
  doc["max"] = 5;
  doc["icon"] = "mdi:clock-end";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // Mode régulation pH (feature-009)
  topic = discoveryBase + "select/" + HA_DEVICE_ID + "_ph_regulation_mode/config";
  doc["name"] = "Mode Régulation pH";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_regulation_mode";
  doc["state_topic"] = topics.phRegulationModeState;
  doc["command_topic"] = topics.phRegulationModeCommand;
  doc["icon"] = "mdi:ph";
  // bug-ha-regulation-mode-labels : libellés français dans HA (comme le mode
  // filtration), protocole sur le fil inchangé (automatic/scheduled/manual).
  {
    JsonArray phRegOptions = doc["options"].to<JsonArray>();
    phRegOptions.add("Automatique");
    phRegOptions.add("Programmée");
    phRegOptions.add("Manuelle");
  }
  doc["value_template"] =
    "{{ {'automatic':'Automatique','scheduled':'Programmée','manual':'Manuelle'}.get(value, value) }}";
  doc["command_template"] =
    "{{ {'Automatique':'automatic','Programmée':'scheduled','Manuelle':'manual'}[value] }}";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);  // publishConfig fait doc.clear() → pas de fuite value/command_template

  // Mode régulation ORP (feature-009)
  topic = discoveryBase + "select/" + HA_DEVICE_ID + "_orp_regulation_mode/config";
  doc["name"] = "Mode Régulation ORP";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_regulation_mode";
  doc["state_topic"] = topics.orpRegulationModeState;
  doc["command_topic"] = topics.orpRegulationModeCommand;
  doc["icon"] = "mdi:flash";
  // bug-ha-regulation-mode-labels : idem pH (protocole inchangé sur le fil)
  {
    JsonArray orpRegOptions = doc["options"].to<JsonArray>();
    orpRegOptions.add("Automatique");
    orpRegOptions.add("Programmée");
    orpRegOptions.add("Manuelle");
  }
  doc["value_template"] =
    "{{ {'automatic':'Automatique','scheduled':'Programmée','manual':'Manuelle'}.get(value, value) }}";
  doc["command_template"] =
    "{{ {'Automatique':'automatic','Programmée':'scheduled','Manuelle':'manual'}[value] }}";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);  // publishConfig fait doc.clear() → pas de fuite value/command_template

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

  // feature-053 : switch Mode Boost (surchloration temporaire du jour, auto-off à minuit)
  topic = discoveryBase + "switch/" + HA_DEVICE_ID + "_boost/config";
  doc["name"] = "Boost";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_boost";
  doc["state_topic"] = topics.boostState;
  doc["command_topic"] = topics.boostCommand;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["state_on"] = "ON";
  doc["state_off"] = "OFF";
  doc["icon"] = "mdi:rocket-launch";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-056 : Mode d'installation (select managed/powered/external). Libellés FR
  // dans HA, protocole inchangé sur le fil (managed/powered/external) via
  // value_template/command_template — même pattern que le select Mode Filtration.
  topic = discoveryBase + "select/" + HA_DEVICE_ID + "_install_mode/config";
  doc["name"] = "Mode d'installation";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_install_mode";
  doc["state_topic"] = topics.installModeState;
  doc["command_topic"] = topics.installModeCommand;
  doc["icon"] = "mdi:pipe-valve";
  {
    JsonArray installOpts = doc["options"].to<JsonArray>();
    installOpts.add("PoolController pilote");
    installOpts.add("Alimenté par filtration");
    installOpts.add("Filtration externe");
  }
  doc["value_template"] =
    "{{ {'managed':'PoolController pilote','powered':'Alimenté par filtration','external':'Filtration externe'}.get(value, value) }}";
  doc["command_template"] =
    "{{ {'PoolController pilote':'managed','Alimenté par filtration':'powered','Filtration externe':'external'}[value] }}";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);  // publishConfig fait doc.clear() → pas de fuite value/command_template

  // feature-056 : signal d'état de la filtration EXTERNE (mode ExternalFiltration).
  // Switch optimiste (pas de state_topic → assumed_state) : permet à une automation
  // HA de signaler l'état réel de la filtration tierce vers le contrôleur. Le
  // contrôleur applique la garde « présence d'eau » sur ce signal (timeout fail-safe).
  topic = discoveryBase + "switch/" + HA_DEVICE_ID + "_filtration_external/config";
  doc["name"] = "Signal filtration externe";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_filtration_external";
  doc["command_topic"] = topics.filtrationExternalStateCommand;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["optimistic"] = true;
  doc["icon"] = "mdi:water-sync";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // bug-ha-eclairage-select : programmation éclairage exposée en SELECT
  // (Programmation/Désactivé) pour cohérence avec le select Mode Filtration.
  // Protocole inchangé sur le fil (ON/OFF) : value_template/command_template
  // traduisent seulement l'affichage HA. Backend booléen + handler LightingSchedule
  // (reçoit ON/OFF) inchangés.
  // Nettoyage migration : retirer l'ancien switch orphelin (feature-052) de HA.
  safePublish((discoveryBase + "switch/" + HA_DEVICE_ID + "_lighting_schedule/config").c_str(), "", true);
  topic = discoveryBase + "select/" + HA_DEVICE_ID + "_lighting_schedule/config";
  doc["name"] = "Mode Éclairage";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_lighting_schedule";
  doc["state_topic"] = topics.lightingScheduleState;
  doc["command_topic"] = topics.lightingScheduleCommand;
  doc["icon"] = "mdi:calendar-clock";
  {
    JsonArray lightOpts = doc["options"].to<JsonArray>();
    lightOpts.add("Programmation");
    lightOpts.add("Désactivé");
  }
  doc["value_template"] =
    "{{ {'ON':'Programmation','OFF':'Désactivé'}.get(value, value) }}";
  doc["command_template"] =
    "{{ {'Programmation':'ON','Désactivé':'OFF'}[value] }}";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);  // publishConfig fait doc.clear() → pas de fuite value/command_template

  // feature-052 : heures d'éclairage (text éditable HH:MM). Effet réel en mode Programmation.
  topic = discoveryBase + "text/" + HA_DEVICE_ID + "_lighting_start/config";
  doc["name"] = "Éclairage début";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_lighting_start";
  doc["state_topic"] = topics.lightingStartState;
  doc["command_topic"] = topics.lightingStartCommand;
  doc["pattern"] = "^([01][0-9]|2[0-3]):[0-5][0-9]$";
  doc["min"] = 5;
  doc["max"] = 5;
  doc["icon"] = "mdi:clock-start";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  topic = discoveryBase + "text/" + HA_DEVICE_ID + "_lighting_end/config";
  doc["name"] = "Éclairage fin";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_lighting_end";
  doc["state_topic"] = topics.lightingEndState;
  doc["command_topic"] = topics.lightingEndCommand;
  doc["pattern"] = "^([01][0-9]|2[0-3]):[0-5][0-9]$";
  doc["min"] = 5;
  doc["max"] = 5;
  doc["icon"] = "mdi:clock-end";
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

  // feature-022 : problème capteur pH (stale OU figé)
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_ph_sensor_problem/config";
  doc["name"] = "Capteur pH — problème";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_sensor_problem";
  doc["state_topic"] = topics.phSensorProblemState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["device_class"] = "problem";
  doc["icon"] = "mdi:alert";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-022 : problème capteur ORP (stale OU figé)
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_orp_sensor_problem/config";
  doc["name"] = "Capteur ORP — problème";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_sensor_problem";
  doc["state_topic"] = topics.orpSensorProblemState;
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

  // feature-050 : cumul pH injecté aujourd'hui (mL — retombe à 0 à minuit)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_daily_ml/config";
  doc["name"] = "Dosage pH aujourd'hui";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_daily_ml";
  doc["state_topic"] = topics.phDailyMlState;
  doc["unit_of_measurement"] = "mL";
  doc["icon"] = "mdi:beaker-outline";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-050 : cumul chlore injecté aujourd'hui (mL — retombe à 0 à minuit)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_orp_daily_ml/config";
  doc["name"] = "Dosage Chlore aujourd'hui";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_daily_ml";
  doc["state_topic"] = topics.orpDailyMlState;
  doc["unit_of_measurement"] = "mL";
  doc["icon"] = "mdi:beaker-outline";
  doc["state_class"] = "measurement";
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

  // feature-050 : volume quotidien pH programmé (mode Programmée) — commandable.
  // Borne haute discovery = 2000 mL ; la limite VIVE maxPhMlPerDay est revalidée
  // au drain de la commande ET plafonne de toute façon le dosage en aval.
  topic = discoveryBase + "number/" + HA_DEVICE_ID + "_ph_daily_target/config";
  doc["name"] = "Volume quotidien pH";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_daily_target";
  doc["state_topic"] = topics.phDailyTargetMlState;
  doc["command_topic"] = topics.phDailyTargetMlCommand;
  doc["min"] = 0;
  doc["max"] = 2000;
  doc["step"] = 10;
  doc["unit_of_measurement"] = "mL";
  doc["mode"] = "box";
  doc["icon"] = "mdi:beaker-plus-outline";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-050 : volume quotidien Chlore programmé — symétrique pH.
  topic = discoveryBase + "number/" + HA_DEVICE_ID + "_orp_daily_target/config";
  doc["name"] = "Volume quotidien Chlore";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_daily_target";
  doc["state_topic"] = topics.orpDailyTargetMlState;
  doc["command_topic"] = topics.orpDailyTargetMlCommand;
  doc["min"] = 0;
  doc["max"] = 2000;
  doc["step"] = 10;
  doc["unit_of_measurement"] = "mL";
  doc["mode"] = "box";
  doc["icon"] = "mdi:beaker-plus-outline";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-050 : bouton redémarrage (séquence propre — cf. drainCommandQueue)
  topic = discoveryBase + "button/" + HA_DEVICE_ID + "_reboot/config";
  doc["name"] = "Redémarrer";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_reboot";
  doc["command_topic"] = topics.rebootCommand;
  doc["payload_press"] = "PRESS";
  doc["device_class"] = "restart";
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

  // feature-021 : nb points calibration pH (entier -1..3, -1 = EZO injoignable)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_cal_points/config";
  doc["name"] = "Piscine pH Points Calibrés";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_cal_points";
  doc["state_topic"] = topics.phCalPointsState;
  doc["icon"] = "mdi:numeric";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-021 : nb points calibration ORP (entier -1..1)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_orp_cal_points/config";
  doc["name"] = "Piscine ORP Points Calibrés";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_cal_points";
  doc["state_topic"] = topics.orpCalPointsState;
  doc["icon"] = "mdi:numeric";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-024 : pente sonde pH — % côté acide (idéal 100%)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_slope_acid/config";
  doc["name"] = "Piscine pH Pente Acide";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_slope_acid";
  doc["state_topic"] = topics.phSlopeAcidState;
  doc["unit_of_measurement"] = "%";
  doc["icon"] = "mdi:angle-acute";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-024 : pente sonde pH — % côté base (idéal 100%)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_slope_base/config";
  doc["name"] = "Piscine pH Pente Base";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_slope_base";
  doc["state_topic"] = topics.phSlopeBaseState;
  doc["unit_of_measurement"] = "%";
  doc["icon"] = "mdi:angle-obtuse";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-024 : pente sonde pH — décalage zéro en mV (idéal 0 mV).
  // Peut rester NaN/non publié si le firmware EZO ne rapporte pas le 3ᵉ float.
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_slope_zero/config";
  doc["name"] = "Piscine pH Décalage Zéro";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_slope_zero";
  doc["state_topic"] = topics.phSlopeZeroState;
  doc["unit_of_measurement"] = "mV";
  doc["icon"] = "mdi:sine-wave";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-025 : sonde pH brut (mesure Atlas non filtrée — diagnostic EMI)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_raw/config";
  doc["name"] = "Piscine pH Brut";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_raw";
  doc["state_topic"] = topics.phRawState;
  doc["unit_of_measurement"] = "pH";
  doc["icon"] = "mdi:water-outline";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-025 : sonde pH filtré (médiane + EMA — valeur de régulation)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_ph_filtered/config";
  doc["name"] = "Piscine pH Filtré";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_filtered";
  doc["state_topic"] = topics.phFilteredState;
  doc["unit_of_measurement"] = "pH";
  doc["icon"] = "mdi:water-check";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-025 : sonde ORP brut (mV)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_orp_raw/config";
  doc["name"] = "Piscine ORP Brut";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_raw";
  doc["state_topic"] = topics.orpRawState;
  doc["unit_of_measurement"] = "mV";
  doc["icon"] = "mdi:flash-outline";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-025 : sonde ORP filtré (mV — valeur de régulation)
  topic = discoveryBase + "sensor/" + HA_DEVICE_ID + "_orp_filtered/config";
  doc["name"] = "Piscine ORP Filtré";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_filtered";
  doc["state_topic"] = topics.orpFilteredState;
  doc["unit_of_measurement"] = "mV";
  doc["icon"] = "mdi:flash-alert";
  doc["state_class"] = "measurement";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-025 : binary_sensor Filtre pH prêt
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_ph_filter_ready/config";
  doc["name"] = "Piscine Filtre pH Prêt";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_ph_filter_ready";
  doc["state_topic"] = topics.phFilterReadyState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:filter-check";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  // feature-025 : binary_sensor Filtre ORP prêt
  topic = discoveryBase + "binary_sensor/" + HA_DEVICE_ID + "_orp_filter_ready/config";
  doc["name"] = "Piscine Filtre ORP Prêt";
  doc["unique_id"] = String(HA_DEVICE_ID) + "_orp_filter_ready";
  doc["state_topic"] = topics.orpFilterReadyState;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:filter-check";
  makeDevice(doc["device"].to<JsonObject>());
  publishConfig(topic);

  discoveryPublished = true;
  systemLogger.info("Home Assistant discovery publié");
}
