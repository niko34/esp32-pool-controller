#ifndef WEB_ROUTES_AUTH_H
#define WEB_ROUTES_AUTH_H

#include <ESPAsyncWebServer.h>

// Configure les routes d'authentification
void setupAuthRoutes(AsyncWebServer* server);

// bug-183 : annonce les échecs d'authentification occultés par le bornage.
// À appeler depuis la loop() UNIQUEMENT (jamais async_tcp) : WARNING.
void drainAuthRoutesWarnLogs();

#endif // WEB_ROUTES_AUTH_H
