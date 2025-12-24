#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

class WebServerManager {
private:
  AsyncWebServer server;
  DNSServer* dns;

  bool restartApRequested = false;

  void setupRoutes();
  void handleGetData(AsyncWebServerRequest* request);
  void handleGetConfig(AsyncWebServerRequest* request);
  void handleSaveConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void handleGetLogs(AsyncWebServerRequest* request);
  void handleTimeNow(AsyncWebServerRequest* request);
  void handleRebootAp(AsyncWebServerRequest* request);
  void handleExportCsv(AsyncWebServerRequest* request);
  void handleGetSystemInfo(AsyncWebServerRequest* request);
  void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final);
  void handleCheckUpdate(AsyncWebServerRequest* request);
  void handleDownloadUpdate(AsyncWebServerRequest* request);

  // Validation des entrées
  bool validatePhValue(float value);
  bool validateOrpValue(float value);
  bool validateInjectionLimit(int seconds);
  bool validatePumpNumber(int pump);

public:
  WebServerManager();

  void begin(DNSServer* dnsServer);
  void update();

  bool isRestartApRequested() const { return restartApRequested; }
  void clearRestartRequest() { restartApRequested = false; }
};

extern WebServerManager webServer;

#endif // WEB_SERVER_H
