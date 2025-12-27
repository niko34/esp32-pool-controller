#ifndef WEB_HELPERS_H
#define WEB_HELPERS_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// Fonctions helper partagées entre tous les modules de routes
void sendJsonResponse(AsyncWebServerRequest* request, JsonDocument& doc);
void sendErrorResponse(AsyncWebServerRequest* request, int code, const String& message);
String getCurrentTimeISO();

#endif // WEB_HELPERS_H
