#include "web_routes_control.h"
#include "web_helpers.h"
#include "config.h"
#include "auth.h"
#include "pump_controller.h"
#include "lighting.h"
#include "logger.h"
#include <Arduino.h>

// Calcule le débit effectif (mL/min) pour un maxDutyPct donné et les params de pompe
static float calcInjectFlow(uint8_t maxDutyPct, const PumpControlParams& params) {
  uint8_t duty = (uint8_t)((maxDutyPct * MAX_PWM_DUTY) / 100);
  if (duty < MIN_ACTIVE_DUTY) duty = MIN_ACTIVE_DUTY;
  float normalized = (float)(duty - MIN_ACTIVE_DUTY) / (float)(MAX_PWM_DUTY - MIN_ACTIVE_DUTY);
  return params.minFlowMlPerMin + normalized * (params.maxFlowMlPerMin - params.minFlowMlPerMin);
}

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
  // Paramètre préféré : ?volume=N (mL, max 2000)
  // Fallback legacy : ?duration=N (secondes)
  server->on("/ph/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.phPump - 1;
    uint8_t dutyPct = (pumpIdx == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
    float flow = calcInjectFlow(dutyPct, phPumpControl);
    float volumeMl = 0.0f;
    int durationS = 0;
    if (req->hasParam("volume")) {
      volumeMl = req->getParam("volume")->value().toFloat();
      if (volumeMl <= 0.0f || volumeMl > 2000.0f) { req->send(400, "text/plain", "volume invalide (1-2000 mL)"); return; }
      durationS = (int)((volumeMl / flow) * 60.0f + 0.5f);
    } else if (req->hasParam("duration")) {
      durationS = req->getParam("duration")->value().toInt();
      volumeMl = flow * durationS / 60.0f;
    } else {
      req->send(400, "text/plain", "parametre volume manquant"); return;
    }
    if (durationS < 1) durationS = 1;
    if (durationS > 3600) durationS = 3600;
    uint8_t duty = (uint8_t)((dutyPct * MAX_PWM_DUTY) / 100);
    PumpController.setManualPump(pumpIdx, duty);
    manualInjectPh.active = true;
    manualInjectPh.startMs = millis();
    manualInjectPh.durationMs = (unsigned long)durationS * 1000UL;
    manualInjectPh.requestedVolumeMl = volumeMl;
    systemLogger.info("[Injection] pH démarrée " + String(durationS) + "s pour " + String(volumeMl, 0) + "mL (débit=" + String(flow, 1) + "mL/min, pompe " + String(mqttCfg.phPump) + ")");
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
  // Paramètre préféré : ?volume=N (mL, max 2000)
  // Fallback legacy : ?duration=N (secondes)
  server->on("/orp/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.orpPump - 1;
    uint8_t dutyPct = (pumpIdx == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
    float flow = calcInjectFlow(dutyPct, orpPumpControl);
    float volumeMl = 0.0f;
    int durationS = 0;
    if (req->hasParam("volume")) {
      volumeMl = req->getParam("volume")->value().toFloat();
      if (volumeMl <= 0.0f || volumeMl > 2000.0f) { req->send(400, "text/plain", "volume invalide (1-2000 mL)"); return; }
      durationS = (int)((volumeMl / flow) * 60.0f + 0.5f);
    } else if (req->hasParam("duration")) {
      durationS = req->getParam("duration")->value().toInt();
      volumeMl = flow * durationS / 60.0f;
    } else {
      req->send(400, "text/plain", "parametre volume manquant"); return;
    }
    if (durationS < 1) durationS = 1;
    if (durationS > 3600) durationS = 3600;
    uint8_t duty = (uint8_t)((dutyPct * MAX_PWM_DUTY) / 100);
    PumpController.setManualPump(pumpIdx, duty);
    manualInjectOrp.active = true;
    manualInjectOrp.startMs = millis();
    manualInjectOrp.durationMs = (unsigned long)durationS * 1000UL;
    manualInjectOrp.requestedVolumeMl = volumeMl;
    systemLogger.info("[Injection] ORP démarrée " + String(durationS) + "s pour " + String(volumeMl, 0) + "mL (débit=" + String(flow, 1) + "mL/min, pompe " + String(mqttCfg.orpPump) + ")");
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
