#ifndef WEB_HELPERS_H
#define WEB_HELPERS_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <stdint.h>

// Fonctions helper partagées entre tous les modules de routes
void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc);
void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message);
void sendRawJsonResponse(AsyncWebServerRequest* request, String& json);
String getCurrentTimeISO();

// Helpers adresse ROM 1-Wire 64 bits (feature-020)
// Format : 16 caractères hex majuscules sans séparateur, ex. "28FF1A2B3C4D5E6F".
String formatRomHex(const uint8_t addr[8]);
// Renvoie false si la chaîne n'est pas exactement 16 chars hex valides.
bool parseRomHex(const String& hex, uint8_t addr[8]);

#endif // WEB_HELPERS_H
