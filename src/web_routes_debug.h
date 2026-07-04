#ifndef WEB_ROUTES_DEBUG_H
#define WEB_ROUTES_DEBUG_H

#include <ESPAsyncWebServer.h>

// Endpoints de diagnostic capteurs :
//
// - POST /debug/ph_slope_refresh     : force une re-query Slope,? sur l'EZO pH (feature-024)
// - POST /debug/sensor_filter_reset  : repasse les filtres pH/ORP en warmup (feature-025)
// - GET  /debug/sensor_filter_state  : état brut JSON des filtres pH/ORP (feature-025)
void setupDebugRoutes(AsyncWebServer* server);

#endif // WEB_ROUTES_DEBUG_H
