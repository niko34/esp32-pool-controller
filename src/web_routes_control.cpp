#include "web_routes_control.h"
#include "web_helpers.h"
#include "config.h"
#include "auth.h"
#include "pump_controller.h"
#include "lighting.h"
#include "logger.h"
#include <Arduino.h>

void setupControlRoutes(AsyncWebServer* server) {
  // Routes pour test manuel des pompes - PROTÉGÉES
  server->on("/pump1/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(0, MAX_PWM_DUTY); // Pompe 1 à fond
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump1/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(0, 0); // Pompe 1 arrêtée
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(1, MAX_PWM_DUTY); // Pompe 2 à fond
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(1, 0); // Pompe 2 arrêtée
    req->send(200, "text/plain", "OK");
  });

  // Routes pour contrôle de l'éclairage (relais) - PROTÉGÉES
  server->on("/lighting/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    lighting.setManualOn();
    saveMqttConfig();
    req->send(200, "text/plain", "OK");
  });

  server->on("/lighting/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    lighting.setManualOff();
    saveMqttConfig();
    req->send(200, "text/plain", "OK");
  });
}

// Handler pour routes dynamiques des pompes - à appeler depuis le onNotFound principal
// PROTÉGÉ par authentification
bool handleDynamicPumpRoutes(AsyncWebServerRequest* req) {
  String url = req->url();

  // Gérer /pump1/duty/:duty
  if (req->method() == HTTP_POST && url.startsWith("/pump1/duty/")) {
    if (!authManager.checkAuth(req, RouteProtection::WRITE)) {
      return true; // Route traitée (auth échouée)
    }
    String dutyStr = url.substring(12); // Après "/pump1/duty/"
    int duty = dutyStr.toInt();
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;
    PumpController.setManualPump(0, duty);
    req->send(200, "text/plain", "OK");
    return true;
  }

  // Gérer /pump2/duty/:duty
  if (req->method() == HTTP_POST && url.startsWith("/pump2/duty/")) {
    if (!authManager.checkAuth(req, RouteProtection::WRITE)) {
      return true; // Route traitée (auth échouée)
    }
    String dutyStr = url.substring(12); // Après "/pump2/duty/"
    int duty = dutyStr.toInt();
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;
    PumpController.setManualPump(1, duty);
    req->send(200, "text/plain", "OK");
    return true;
  }

  return false;
}
