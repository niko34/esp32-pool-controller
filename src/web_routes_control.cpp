#include "web_routes_control.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "pump_controller.h"
#include "filtration.h"
#include "lighting.h"
#include "logger.h"
#include "mqtt_manager.h"
#include <Arduino.h>

// =============================================================================
// Sécurité chimique (pool-chemistry validation 2026-05-11)
// =============================================================================
// Toute injection manuelle volumée OU activation directe d'une pompe (test bench)
// nécessite que la filtration soit active, SAUF en mode régulation "continu"
// où l'alimentation du contrôleur suit la filtration de toute façon.
// Sans circulation d'eau, l'acide/le chlore injecté reste local au point de
// retour → surdosage massif dans une zone confinée → corrosion + risque sanitaire.
// Cette garde est cohérente avec PumpController::canDose() qui applique la
// même règle pour la régulation auto.
static bool injectionAllowedOrReject(AsyncWebServerRequest* req, const char* tag) {
  if (mqttCfg.regulationMode == "continu") return true;  // alim suit filtration
  if (filtration.isRunning()) return true;
  systemLogger.critical(String("[Sécurité] ") + tag +
                        " refusé : filtration arrêtée (sécurité chimique : pas de circulation = surdosage local)");
  req->send(409, "text/plain", "filtration arrêtée — injection refusée pour sécurité chimique");
  return false;
}

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

// Sécurité chimique : vérifie qu'une injection volumée doit continuer.
// Garde filtration cohérente avec canDose() côté régulation auto.
static bool filtrationOkForInjection() {
  return mqttCfg.regulationMode == "continu" || filtration.isRunning();
}

void updateManualInject() {
  unsigned long now = millis();

  // ========== pH ==========
  if (manualInjectPh.active) {
    // 1. Arrêt fin de durée (cas nominal)
    if (now - manualInjectPh.startMs >= manualInjectPh.durationMs) {
      PumpController.setManualPump(mqttCfg.phPump - 1, 0);
      manualInjectPh.active = false;
      manualInjectPh.requestedVolumeMl = 0.0f;
      systemLogger.info("[Injection] pH arrêtée automatiquement (fin de durée)");
    }
    // 2. Arrêt sécurité chimique : filtration arrêtée pendant l'injection
    //    (pool-chemistry condition #1 : pas de circulation = surdosage local)
    else if (!filtrationOkForInjection()) {
      PumpController.setManualPump(mqttCfg.phPump - 1, 0);
      manualInjectPh.active = false;
      manualInjectPh.requestedVolumeMl = 0.0f;
      systemLogger.critical("[Injection] pH INTERROMPUE — filtration arrêtée (sécurité chimique)");
      mqttManager.publishAlert("ph_injection_aborted",
                               "Injection pH interrompue : filtration arrêtée pendant l'injection. Relancer manuellement après reprise filtration.");
    }
  }

  // ========== ORP ==========
  if (manualInjectOrp.active) {
    if (now - manualInjectOrp.startMs >= manualInjectOrp.durationMs) {
      PumpController.setManualPump(mqttCfg.orpPump - 1, 0);
      manualInjectOrp.active = false;
      manualInjectOrp.requestedVolumeMl = 0.0f;
      systemLogger.info("[Injection] ORP arrêtée automatiquement (fin de durée)");
    }
    else if (!filtrationOkForInjection()) {
      PumpController.setManualPump(mqttCfg.orpPump - 1, 0);
      manualInjectOrp.active = false;
      manualInjectOrp.requestedVolumeMl = 0.0f;
      systemLogger.critical("[Injection] ORP INTERROMPUE — filtration arrêtée (sécurité chimique)");
      mqttManager.publishAlert("orp_injection_aborted",
                               "Injection ORP/chlore interrompue : filtration arrêtée pendant l'injection. Relancer manuellement après reprise filtration.");
    }
  }
}

void setupControlRoutes(AsyncWebServer* server) {
  // Routes pour test manuel des pompes - PROTÉGÉES
  // Sécurité chimique (pool-chemistry validation 2026-05-11) : démarrage refusé
  // si la filtration n'est pas active (sauf mode "continu" où l'alim suit la
  // filtration). Pas de bornage de durée ici (test bench) — l'utilisateur doit
  // arrêter manuellement via /pumpN/off OU passer par /ph/inject/start qui borne.
  server->on("/pump1/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    if (!injectionAllowedOrReject(req, "Test pompe 1")) return;
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
    if (!injectionAllowedOrReject(req, "Test pompe 2")) return;
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
  // Fallback legacy : ?duration=N (secondes, borné à kManualInjectMaxDurationS)
  // Sécurité chimique : démarrage refusé si filtration arrêtée (sauf mode continu).
  server->on("/ph/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    if (!injectionAllowedOrReject(req, "Injection pH")) return;
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
    if (durationS > kManualInjectMaxDurationS) durationS = kManualInjectMaxDurationS;
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
  // Fallback legacy : ?duration=N (secondes, borné à kManualInjectMaxDurationS)
  // Sécurité chimique : démarrage refusé si filtration arrêtée (sauf mode continu).
  server->on("/orp/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    if (!injectionAllowedOrReject(req, "Injection ORP")) return;
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
    if (durationS > kManualInjectMaxDurationS) durationS = kManualInjectMaxDurationS;
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
