#ifndef WS_MANAGER_H
#define WS_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <set>
#include "logger.h"

// Gère le WebSocket /ws : authentification, push temps réel (capteurs, config, logs)
class WsManager {
public:
  void begin(AsyncWebServer* server);
  void update();  // À appeler dans loop() : cleanup + push capteurs toutes les 5s

  void broadcastSensorData();
  void broadcastConfig();
  void broadcastLog(const LogEntry& entry);

  bool hasClients() const;

private:
  AsyncWebSocket* _ws = nullptr;
  unsigned long _lastSensorPush = 0;
  bool _pendingInitialPush = false;
  std::set<uint32_t> _authenticatedClients;
  static constexpr unsigned long kSensorPushIntervalMs = 5000;

  void _onEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                AwsEventType type, void* arg, uint8_t* data, size_t len);
  void _onClientConnect(AsyncWebSocketClient* client, AsyncWebServerRequest* request);
  void _onData(AsyncWebSocketClient* client, uint8_t* data, size_t len);

  String _buildSensorJson() const;
  String _buildConfigJson() const;
};

extern WsManager wsManager;

#endif // WS_MANAGER_H
