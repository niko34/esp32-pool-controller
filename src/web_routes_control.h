#ifndef WEB_ROUTES_CONTROL_H
#define WEB_ROUTES_CONTROL_H

#include <ESPAsyncWebServer.h>

// Déclarations des handlers pour les routes de contrôle
void setupControlRoutes(AsyncWebServer* server);

// Handler pour routes dynamiques des pompes (à appeler depuis onNotFound)
bool handleDynamicPumpRoutes(AsyncWebServerRequest* req);

#endif // WEB_ROUTES_CONTROL_H
