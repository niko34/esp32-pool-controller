#ifndef WEB_ROUTES_DEBUG_H
#define WEB_ROUTES_DEBUG_H

#include <ESPAsyncWebServer.h>

// Endpoints de debug temporaire pour diagnostiquer une oscillation pH (feature-021).
//
// - GET  /debug/ph_trace        : retourne le ring buffer pH/ORP/tempC (~25 min, 5 s/cycle)
// - POST /debug/ph_trace_clear  : vide le ring buffer
// - POST /debug/wifi_pause      : coupe le WiFi pendant N secondes (1..120) puis le rétablit
// - GET  /debug/wifi_pause      : statut de la pause en cours
//
// L'enregistrement des samples se fait côté Sensors::_readEzoSensors() — ces routes
// ne font qu'exposer/contrôler la trace.
void setupDebugRoutes(AsyncWebServer* server);

// Doit être appelée régulièrement depuis loopTask (ou tâche dédiée).
// Gère la transition WiFi ON ↔ OFF selon l'état du flag de pause.
void updateWifiPauseLoop();

#endif // WEB_ROUTES_DEBUG_H
