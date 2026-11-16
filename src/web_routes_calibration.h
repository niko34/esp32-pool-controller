#ifndef WEB_ROUTES_CALIBRATION_H
#define WEB_ROUTES_CALIBRATION_H

#include <ESPAsyncWebServer.h>

// Déclarations des handlers pour les routes de calibration
void setupCalibrationRoutes(AsyncWebServer* server);

// bug-112 : annonce les tentatives occultées (loop() uniquement, WARNING).
void drainCalibrationUnauthWarnLogs();

#endif // WEB_ROUTES_CALIBRATION_H
