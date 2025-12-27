#include "web_helpers.h"

void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc) {
  String json;
  serializeJson(doc, json);
  AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message) {
  StaticJsonDocument<128> doc;
  doc["error"] = message;
  String json;
  serializeJson(doc, json);
  AsyncWebServerResponse* response = request->beginResponse(code, "application/json", json);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

String getCurrentTimeISO() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    return String(buffer);
  }
  return "unavailable";
}
