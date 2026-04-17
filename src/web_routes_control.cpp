#include "web_routes_control.h"
#include "web_helpers.h"
#include "config.h"
#include "auth.h"
#include "pump_controller.h"
#include "lighting.h"
#include "logger.h"
#include <Arduino.h>

ManualInjectState manualInjectPh;
ManualInjectState manualInjectOrp;

int manualInjectRemainingS(const ManualInjectState& s) {
  if (!s.active) return 0;
  unsigned long elapsed = millis() - s.startMs;
  if (elapsed >= s.durationMs) return 0;
  return (int)((s.durationMs - elapsed) / 1000UL);
}

void updateManualInject() {
  if (manualInjectPh.active && millis() - manualInjectPh.startMs >= manualInjectPh.durationMs) {
    int pumpIdx = mqttCfg.phPump - 1;
    PumpController.setManualPump(pumpIdx, 0);
    manualInjectPh.active = false;
    systemLogger.info("[Injection] pH arrêtée automatiquement (fin de durée)");
  }
  if (manualInjectOrp.active && millis() - manualInjectOrp.startMs >= manualInjectOrp.durationMs) {
    int pumpIdx = mqttCfg.orpPump - 1;
    PumpController.setManualPump(pumpIdx, 0);
    manualInjectOrp.active = false;
    systemLogger.info("[Injection] ORP arrêtée automatiquement (fin de durée)");
  }
}

void setupControlRoutes(AsyncWebServer* server) {
  // Routes pour test manuel des pompes - PROTÉGÉES
  server->on("/pump1/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(0, MAX_PWM_DUTY);
    systemLogger.info("[Test] Pompe 1 démarrée en mode manuel");
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump1/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(0, 0);
    systemLogger.info("[Test] Pompe 1 arrêtée");
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(1, MAX_PWM_DUTY);
    systemLogger.info("[Test] Pompe 2 démarrée en mode manuel");
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(1, 0);
    systemLogger.info("[Test] Pompe 2 arrêtée");
    req->send(200, "text/plain", "OK");
  });

  // Injection manuelle pH — démarre la pompe pH à la puissance configurée
  // Paramètre optionnel : ?duration=N (secondes, défaut 60, max 3600)
  server->on("/ph/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int durationS = 60;
    if (req->hasParam("duration")) durationS = req->getParam("duration")->value().toInt();
    if (durationS <= 0) durationS = 60;
    if (durationS > 3600) durationS = 3600;
    int pumpIdx = mqttCfg.phPump - 1;
    uint8_t dutyPct = (pumpIdx == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
    uint8_t duty = (uint8_t)((dutyPct * MAX_PWM_DUTY) / 100);
    PumpController.setManualPump(pumpIdx, duty);
    manualInjectPh.active = true; manualInjectPh.startMs = millis(); manualInjectPh.durationMs = (unsigned long)durationS * 1000UL;
    systemLogger.info("[Injection] pH démarrée " + String(durationS) + "s (pompe " + String(mqttCfg.phPump) + ", duty=" + String(duty) + ")");
    req->send(200, "text/plain", "OK");
  });

  server->on("/ph/inject/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.phPump - 1;
    PumpController.setManualPump(pumpIdx, 0);
    manualInjectPh.active = false;
    systemLogger.info("[Injection] pH arrêtée manuellement");
    req->send(200, "text/plain", "OK");
  });

  // Injection manuelle ORP — démarre la pompe ORP à la puissance configurée
  // Paramètre optionnel : ?duration=N (secondes, défaut 60, max 3600)
  server->on("/orp/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int durationS = 60;
    if (req->hasParam("duration")) durationS = req->getParam("duration")->value().toInt();
    if (durationS <= 0) durationS = 60;
    if (durationS > 3600) durationS = 3600;
    int pumpIdx = mqttCfg.orpPump - 1;
    uint8_t dutyPct = (pumpIdx == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
    uint8_t duty = (uint8_t)((dutyPct * MAX_PWM_DUTY) / 100);
    PumpController.setManualPump(pumpIdx, duty);
    manualInjectOrp.active = true; manualInjectOrp.startMs = millis(); manualInjectOrp.durationMs = (unsigned long)durationS * 1000UL;
    systemLogger.info("[Injection] ORP démarrée " + String(durationS) + "s (pompe " + String(mqttCfg.orpPump) + ", duty=" + String(duty) + ")");
    req->send(200, "text/plain", "OK");
  });

  server->on("/orp/inject/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.orpPump - 1;
    PumpController.setManualPump(pumpIdx, 0);
    manualInjectOrp.active = false;
    systemLogger.info("[Injection] ORP arrêtée manuellement");
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
