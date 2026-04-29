#include "web_helpers.h"
#include <memory>

// Envoi via AsyncCallbackResponse avec Content-Length connu.
// Évite le bug de AsyncBasicResponse qui mute _content via String::substring()
// à chaque ACK TCP — sur grosse réponse et heap fragmenté, substring() peut
// renvoyer un String à buffer null et planter en LoadProhibited.
// Le shared_ptr<String> garde le contenu vivant jusqu'à la fin du transfert,
// et Content-Length permet à lwIP de fermer le socket dès le dernier octet.
static void sendJsonBuffered(AsyncWebServerRequest* request, int code, std::shared_ptr<String> json) {
  size_t len = json->length();
  AsyncWebServerResponse* response = request->beginResponse(
    "application/json", len,
    [json](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
      size_t remaining = json->length() - index;
      size_t toCopy = remaining < maxLen ? remaining : maxLen;
      memcpy(buffer, json->c_str() + index, toCopy);
      return toCopy;
    });
  if (code != 200) response->setCode(code);
  request->send(response);
}

void sendRawJsonResponse(AsyncWebServerRequest* request, String& json) {
  // std::move évite la copie du buffer — json devient invalide après l'appel
  auto ptr = std::make_shared<String>(std::move(json));
  sendJsonBuffered(request, 200, ptr);
}

void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc) {
  auto json = std::make_shared<String>();
  serializeJson(doc, *json);
  sendJsonBuffered(request, 200, json);
}

void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message) {
  auto json = std::make_shared<String>();
  JsonDocument doc;
  doc["error"] = message;
  serializeJson(doc, *json);
  sendJsonBuffered(request, code, json);
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
