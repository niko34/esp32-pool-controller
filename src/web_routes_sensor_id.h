#ifndef WEB_ROUTES_SENSOR_ID_H
#define WEB_ROUTES_SENSOR_ID_H

#include <ESPAsyncWebServer.h>

// Endpoints d'identification des sondes DS18B20 (feature-020).
// - GET  /sensors/onewire/scan      : liste les sondes détectées + T° + rôle
// - POST /sensors/onewire/identify  : marque une sonde comme "eau" ou "circuit"
// - POST /sensors/onewire/reset     : efface l'identification NVS
void setupSensorIdRoutes(AsyncWebServer* server);

#endif // WEB_ROUTES_SENSOR_ID_H
