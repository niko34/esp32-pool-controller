#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
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
  void handleGetData(AsyncWebServerRequest* request);
  void handleGetConfig(AsyncWebServerRequest* request);
  void handleSaveConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void handleGetLogs(AsyncWebServerRequest* request);
  void handleGetHistory(AsyncWebServerRequest* request);
  void handleTimeNow(AsyncWebServerRequest* request);
  void handleRebootAp(AsyncWebServerRequest* request);
  void handleGetSystemInfo(AsyncWebServerRequest* request);
  void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final);
  void handleCheckUpdate(AsyncWebServerRequest* request);
  void handleDownloadUpdate(AsyncWebServerRequest* request);

  // Éclairage LED
  void handleLightingOn(AsyncWebServerRequest* request);
  void handleLightingOff(AsyncWebServerRequest* request);

  // Validation des entrées
  bool validatePhValue(float value);
  bool validateOrpValue(float value);
  bool validateInjectionLimit(int seconds);
  bool validatePumpNumber(int pump);

  // Helpers pour réduire duplication
  void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc);
  void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message);
  String getCurrentTimeISO();

public:
  WebServerManager();

  void begin(AsyncWebServer* webServer, DNSServer* dnsServer);
  void update();

  bool isRestartApRequested() const { return restartApRequested; }
  void clearRestartRequest() { restartApRequested = false; }
};

extern WebServerManager webServer;

#endif // WEB_SERVER_H
