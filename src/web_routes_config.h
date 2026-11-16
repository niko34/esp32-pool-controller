#ifndef WEB_ROUTES_CONFIG_H
#define WEB_ROUTES_CONFIG_H

#include <ESPAsyncWebServer.h>
#include <map>
#include <vector>

// Initialisation du contexte de configuration (buffers partagés)
void initConfigContext(
  std::map<AsyncWebServerRequest*, std::vector<uint8_t>>* configBuffers,
  std::map<AsyncWebServerRequest*, bool>* configErrors
);

// Déclarations des handlers pour les routes de configuration
void setupConfigRoutes(AsyncWebServer* server, bool* restartApRequested, unsigned long* restartRequestedTime);

// Fonction pour traiter les reconnexions WiFi asynchrones (à appeler dans loop())
void processWifiReconnectIfNeeded();

// bug-112 : annonce les refus de corps occultés (loop() uniquement, WARNING).
void drainConfigUnauthWarnLogs();

#endif // WEB_ROUTES_CONFIG_H
