#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <map>
#include <vector>
#include "ws_manager.h"

class WebServerManager {
private:
  AsyncWebServer* server;
  DNSServer* dns;

  bool restartApRequested = false;
  bool restartRequested = false;
  unsigned long restartRequestedTime = 0;

  // Buffer pour accumulation des données de configuration chunkées
  std::map<AsyncWebServerRequest*, std::vector<uint8_t>> configBuffers;
  std::map<AsyncWebServerRequest*, bool> configErrors;

  void setupRoutes();

  // Drain du compteur de refus de la garde 503 « fichiers statiques » (bug-080,
  // lot B-3). Appelé depuis update(), donc depuis la LOOP — jamais depuis
  // `async_tcp`, où toute émission de log serait un fopen() potentiel et donc
  // le crash même que la garde prévient. Émet au plus 2 lignes WARN/INFO par
  // épisode (front montant, front descendant) ; jamais d'ERROR ni de CRITICAL.
  void drainLowHeapGuard();

public:
  WebServerManager();

  void begin(AsyncWebServer* webServer, DNSServer* dnsServer);
  void update();

  bool isRestartApRequested() const { return restartApRequested; }
  void clearRestartRequest() { restartApRequested = false; }
};

extern WebServerManager webServer;

#endif // WEB_SERVER_H
