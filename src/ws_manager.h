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
  uint32_t skippedPushCount() const;  // Cumul des pushes sautés (garde heap OU bad_alloc rattrapé)
  uint32_t oversizedFrameCount() const;  // Cumul des trames > kWsFrameBufBytes (push sauté, jamais tronqué)

private:
  AsyncWebSocket* _ws = nullptr;
  unsigned long _lastSensorPush = 0;
  bool _pendingInitialPush = false;
  bool _pendingConfigBroadcast = false;
  std::set<uint32_t> _authenticatedClients;
  static constexpr unsigned long kSensorPushIntervalMs = 5000;

  // Compteur de pushes WS sautés par la garde heap (bug-badalloc-ws-broadcast) OU
  // par le catch de std::bad_alloc autour de textAll (bug-heap-toctou A1 — filet
  // TOCTOU). atomic relaxed (pattern connectedAtomic) : incrémenté depuis loopTask
  // ET potentiellement depuis le contexte de l'appelant du log (broadcastLog via un
  // futur callback logger) ; lu dans update() (loopTask).
  std::atomic<uint32_t> _skippedPushes{0};
  uint32_t _lastWarnedSkipped = 0;   // Valeur du compteur au dernier warn émis
  unsigned long _lastSkipWarnMs = 0; // Instant du dernier warn (0 = jamais)

  // Compteur de trames dépassant le buffer statique kWsFrameBufBytes (bug-heap-toctou
  // A2) : push sauté (JAMAIS tronqué) — signale qu'il faut agrandir la constante.
  // Même patron atomic relaxed + warn throttlé dédié dans update().
  std::atomic<uint32_t> _oversizedFrames{0};
  uint32_t _lastWarnedOversized = 0;      // Valeur du compteur au dernier warn émis
  unsigned long _lastOversizedWarnMs = 0; // Instant du dernier warn (0 = jamais)

  // Garde heap commune des broadcasts (bug-badalloc-ws-broadcast) : false si le plus
  // gros bloc contigu RÉELLEMENT allouable (heapLargestFreeBlock8Bit, bug-080/v2.33.0)
  // est sous kWsPushMinAllocBytes (le push doit être sauté).
  // INVARIANT : ne logge JAMAIS — voir le commentaire d'implémentation.
  bool _heapAllowsPush();

  void _onEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                AwsEventType type, void* arg, uint8_t* data, size_t len);
  void _onClientConnect(AsyncWebSocketClient* client, AsyncWebServerRequest* request);
  void _onData(AsyncWebSocketClient* client, uint8_t* data, size_t len);

  // Construction des trames dans un buffer fourni (bug-heap-toctou A2 : buffer
  // statique kWsFrameBufBytes, plus de String par push). Retour : longueur du JSON
  // écrit (hors NUL), ou 0 si measureJson()+1 > cap — l'appelant SAUTE le push et
  // incrémente _oversizedFrames (jamais de troncature).
  size_t _buildSensorJson(char* buf, size_t cap) const;
  size_t _buildConfigJson(char* buf, size_t cap) const;
};

extern WsManager wsManager;

#endif // WS_MANAGER_H
