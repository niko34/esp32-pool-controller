#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <map>
#include <vector>

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

public:
  WebServerManager();

  void begin(AsyncWebServer* webServer, DNSServer* dnsServer);
  void update();

  bool isRestartApRequested() const { return restartApRequested; }
  void clearRestartRequest() { restartApRequested = false; }
};

extern WebServerManager webServer;

#endif // WEB_SERVER_H
