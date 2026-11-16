#ifndef WEB_ROUTES_OTA_SWITCH_H
#define WEB_ROUTES_OTA_SWITCH_H

#include <ESPAsyncWebServer.h>

// feature-151 D3-b2 : routes d'ADMINISTRATION de l'interrupteur de signature OTA.
// Fichier ISOLÉ de web_routes_ota.cpp exprès : rien ici ne touche à l'upload
// (outillé par tools/check_ota_signature_switch.sh, C12).
//   POST /ota-signature-switch/arm  — arme (jeton admin, auth ACTIVE exigée, C3)
//   GET|POST /ota-signature-switch  — état ; POST désarme puis rend l'état
void setupOtaSignatureSwitchRoutes(AsyncWebServer* server);

#endif  // WEB_ROUTES_OTA_SWITCH_H
