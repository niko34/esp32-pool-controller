#include "web_helpers.h"
#include <memory>

// Envoi via chunked response avec un shared_ptr<String> capturé dans la lambda.
// Évite le bug de AsyncBasicResponse qui mute _content à chaque ACK TCP via
// String::substring() — sur grosse réponse (> 1 packet TCP) et heap fragmenté,
// substring() peut renvoyer un String à buffer null et planter en LoadProhibited.
// Avec chunked + shared_ptr, le String reste immuable et vit jusqu'à fin de l'envoi.
static void sendJsonChunked(AsyncWebServerRequest* request, int code, std::shared_ptr<String> json) {
  AsyncWebServerResponse* response = request->beginChunkedResponse(
    "application/json",
    [json](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
      const size_t total = json->length();
      if (index >= total) return 0;
      size_t remaining = total - index;
      size_t toCopy = remaining < maxLen ? remaining : maxLen;
      memcpy(buffer, json->c_str() + index, toCopy);
      return toCopy;
    });
  response->setCode(code);
  request->send(response);
}

void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc) {
  auto json = std::make_shared<String>();
  serializeJson(doc, *json);
  sendJsonChunked(request, 200, json);
}

void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message) {
  auto json = std::make_shared<String>();
  JsonDocument doc;
  doc["error"] = message;
  serializeJson(doc, *json);
  sendJsonChunked(request, code, json);
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
