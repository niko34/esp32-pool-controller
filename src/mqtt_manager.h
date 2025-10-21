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
  String phDosageState;
  String orpDosageState;
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
  void publishAlert(const String& alertType, const String& message);
  void publishLog(const String& logMessage);
  void publishStatus(const String& status);
  void publishDiagnostic();

  // Getters
  const MqttTopics& getTopics() const { return topics; }
};

extern MqttManager mqttManager;

#endif // MQTT_MANAGER_H
