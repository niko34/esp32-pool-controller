#ifndef WEB_ROUTES_OTA_H
#define WEB_ROUTES_OTA_H

#include <ESPAsyncWebServer.h>

// Initialisation du contexte OTA (pointeurs vers variables de restart)
void initOtaContext(bool* restartRequested, unsigned long* restartRequestedTime);

// Déclarations des handlers pour les routes OTA
void setupOtaRoutes(AsyncWebServer* server);

// bug-112 : annonce les tentatives d'upload occultées (loop() uniquement, WARNING).
void drainOtaUnauthWarnLogs();

#endif // WEB_ROUTES_OTA_H
