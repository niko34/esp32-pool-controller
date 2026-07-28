#ifndef WS_MANAGER_H
#define WS_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <atomic>
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

  // À utiliser depuis un handler HTTP (tâche AsyncTCP) : marque le broadcast
  // pour exécution dans la main loop. Évite l'allocation d'un StaticJson<2048>
  // sur la pile de la tâche AsyncTCP qui a peu de stack.
  void requestConfigBroadcast();

  bool hasClients() const;
  size_t clientCount() const;  // Nombre de clients WS connectés (diagnostic)
  uint32_t skippedPushCount() const;  // Cumul des pushes sautés par la garde heap (diagnostic)

private:
  AsyncWebSocket* _ws = nullptr;
  unsigned long _lastSensorPush = 0;
  bool _pendingInitialPush = false;
  bool _pendingConfigBroadcast = false;
  std::set<uint32_t> _authenticatedClients;
  static constexpr unsigned long kSensorPushIntervalMs = 5000;

  // Compteur de pushes WS sautés par la garde heap (bug-badalloc-ws-broadcast).
  // atomic relaxed (pattern connectedAtomic) : incrémenté depuis loopTask ET
  // potentiellement depuis le contexte de l'appelant du log (broadcastLog via un
  // futur callback logger) ; lu dans update() (loopTask).
  std::atomic<uint32_t> _skippedPushes{0};
  uint32_t _lastWarnedSkipped = 0;   // Valeur du compteur au dernier warn émis
  unsigned long _lastSkipWarnMs = 0; // Instant du dernier warn (0 = jamais)

  // Garde heap commune des broadcasts (bug-badalloc-ws-broadcast) : false si le plus
  // gros bloc contigu est sous kWsPushMinAllocBytes (le push doit être sauté).
  // INVARIANT : ne logge JAMAIS — voir le commentaire d'implémentation.
  bool _heapAllowsPush();

  void _onEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                AwsEventType type, void* arg, uint8_t* data, size_t len);
  void _onClientConnect(AsyncWebSocketClient* client, AsyncWebServerRequest* request);
  void _onData(AsyncWebSocketClient* client, uint8_t* data, size_t len);

  String _buildSensorJson() const;
  String _buildConfigJson() const;
};

extern WsManager wsManager;

#endif // WS_MANAGER_H
