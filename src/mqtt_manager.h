#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

struct MqttTopics {
  String base;
  String temperatureState;
  String phState;
  String orpState;
  String filtrationState;
  String filtrationModeState;
  String filtrationModeCommand;
  String filtrationCommand;    // ON/OFF switch command
  String lightingState;        // État éclairage
  String lightingCommand;      // ON/OFF switch command
  String phDosageState;
  String orpDosageState;
  String phDosingState;        // Pompe pH active (ON/OFF)
  String orpDosingState;       // Pompe ORP active (ON/OFF)
  String phLimitState;         // Limite journalière pH atteinte (ON/OFF)
  String orpLimitState;        // Limite journalière ORP atteinte (ON/OFF)
  String phStockLowState;      // Stock pH faible (ON/OFF)
  String orpStockLowState;     // Stock ORP faible (ON/OFF)
  String phRemainingState;     // Volume pH restant (ml)
  String orpRemainingState;    // Volume ORP restant (ml)
  String phTargetState;        // Consigne pH (état)
  String orpTargetState;       // Consigne ORP (état)
  String phTargetCommand;      // Consigne pH (commande)
  String orpTargetCommand;     // Consigne ORP (commande)
  String alertsTopic;
  String logsTopic;
  String statusTopic;          // LWT et status
  String diagnosticTopic;      // Diagnostic détaillé
};

class MqttManager {
private:
  WiFiClient wifiClient;
  PubSubClient mqtt;

  MqttTopics topics;
  bool discoveryPublished = false;
  bool reconnectRequested = false;
  unsigned long lastAttempt = 0;

  void publishDiscovery();
  void refreshTopics();

public:
  MqttManager();

  void begin();
  void update();
  void connect();
  void disconnect();
  void messageCallback(char* topic, byte* payload, unsigned int length);

  bool isConnected() { return mqtt.connected(); }
  void requestReconnect() { reconnectRequested = true; }

  // Publication
  void publishSensorState(const String& topic, const String& payload, bool retain = true);
  void publishAllStates();
  void publishFiltrationState();
  void publishLightingState();
  void publishDosingState();
  void publishProductState();
  void publishTargetState();
  void publishAlert(const String& alertType, const String& message);
  void publishLog(const String& logMessage);
  void publishStatus(const String& status);
  void publishDiagnostic();

  // Getters
  const MqttTopics& getTopics() const { return topics; }
};

extern MqttManager mqttManager;

#endif // MQTT_MANAGER_H
