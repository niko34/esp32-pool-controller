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
#include "ws_manager.h"
#include "dosing_logic.h"
#include <Arduino.h>
#include <esp_task_wdt.h>

// Handle de la loopTask Arduino (défini dans le core, cores/esp32/main.cpp).
// Les handlers AsyncWebServer tournent dans la tâche async_tcp, qui n'est PAS
// abonnée à la TWDT : esp_task_wdt_status(NULL) y renverrait toujours NOT_FOUND
// (→ refus systématique). La garde watchdog porte sur la tâche qui exécute le
// dosage (loopTask, abonnée via esp_task_wdt_add(NULL) dans setup()).
extern TaskHandle_t loopTaskHandle;

// =============================================================================
// Sécurité chimique (pool-chemistry validation 2026-05-11)
// =============================================================================
// Toute activation directe d'une pompe (test bench /pumpN/on) nécessite la
// présence d'eau (resolveWaterPresence().waterPresent) : filtration commandée ON
// en mode d'installation "managed", présumée en "powered", signalée récente en
// "external" (feature-056).
// Sans circulation d'eau, l'acide/le chlore injecté reste local au point de
// retour → surdosage massif dans une zone confinée → corrosion + risque sanitaire.
// Cette garde est cohérente avec PumpController::canDose() qui applique la
// même règle pour la régulation auto.
// feature-006 : pour les injections volumées (/ph|orp/inject/start), cette garde
// est absorbée par evaluateManualInject() (cause FiltrationOff) — ce helper ne
// sert plus qu'aux routes de test de pompes. Réponse au format JSON structuré,
// identique à celui des refus d'injection manuelle. Garde JAMAIS affaiblie.
static bool injectionAllowedOrReject(AsyncWebServerRequest* req, const char* tag) {
  // feature-056 : source UNIQUE resolveWaterPresent() (Managed/Powered/External).
  if (filtration.resolveWaterPresence().waterPresent) return true;
  systemLogger.critical(String("[Sécurité] ") + tag +
                        " refusé : filtration arrêtée (sécurité chimique : pas de circulation = surdosage local)");
  req->send(409, "application/json",
            "{\"error\":\"filtration_off\",\"message\":\"Filtration arrêtée — injection refusée pour sécurité chimique (pas de circulation = surdosage local).\"}");
  return false;
}

// =============================================================================
// feature-006 : gardes d'injection manuelle (coquille de evaluateManualInject)
// =============================================================================
// Collecte les entrées (SNAPSHOT sans mutex — cf. pump_controller.h : compteurs
// écrits en loopTask uniquement, lectures 32 bits atomiques, pending inclus),
// appelle la décision PURE evaluateManualInject() (src/dosing_logic.*), et
// formate le refus en 409 JSON structuré :
//   {"error":"<code>","message":"<français>","seconds_remaining"?:N,"remaining_ml"?:N}
// `isPh` : true = pH (index logique 0), false = ORP (index logique 1) — index des
// compteurs/stabilisation du PumpController, PAS le n° de pompe physique.
// `effectiveMl`/`durationS` : volume/durée POST-clamp kManualInjectMaxDurationS
// (condition pool-chemistry #3 : évaluer ce qui sera VRAIMENT injecté).
static bool manualInjectGuardOrReject(AsyncWebServerRequest* req, bool isPh,
                                      float effectiveMl, int durationS) {
  const int logicalIdx = isPh ? 0 : 1;

  ManualInjectInputs in = {};
  in.watchdogActive = (esp_task_wdt_status(loopTaskHandle) == ESP_OK);
  in.waterPresent = filtration.resolveWaterPresence().waterPresent;  // feature-056 : source UNIQUE
  unsigned long stabS = PumpController.getStabilizationRemainingS(logicalIdx);
  in.stabilizationActive = (stabS > 0);
  in.stabilizationRemainingS = (uint32_t)stabS;
  in.alreadyInjecting = isPh ? manualInjectPh.active : manualInjectOrp.active;
  in.requestedMl = effectiveMl;
  in.dailyInjectedMl = isPh ? safetyLimits.dailyPhInjectedMl : safetyLimits.dailyOrpInjectedMl;
  in.maxDailyMl = isPh ? safetyLimits.maxPhMlPerDay : safetyLimits.maxChlorineMlPerDay;
  in.usedMs = (uint32_t)(isPh ? PumpController.getPhUsedMs() : PumpController.getOrpUsedMs());
  int limitMin = isPh ? mqttCfg.phInjectionLimitMinutes : mqttCfg.orpInjectionLimitMinutes;
  in.hourlyLimitMs = (limitMin > 0) ? (uint32_t)limitMin * 60000UL : 0;  // 0 = illimité
  in.requestedDurationMs = (uint32_t)durationS * 1000UL;
  in.cyclesToday = PumpController.getCyclesToday(logicalIdx);
  in.maxCyclesPerDay = pumpProtection.maxCyclesPerDay;
  in.cyclesLastMin = PumpController.getRecentCycles(logicalIdx, 60000UL);
  in.maxCyclesPerMin = kMaxDosingCyclesPerMinute;
  in.cyclesLast15Min = PumpController.getRecentCycles(logicalIdx, 900000UL);
  in.maxCyclesPer15Min = kMaxDosingCyclesPer15Min;

  ManualInjectDecision d = evaluateManualInject(in);
  if (d.allowed) return true;

  // Mapping enum → code API + message français. BurstPerMinute et BurstPer15Min
  // partagent le code "burst_limit" (même remède côté opérateur : patienter).
  const char* code = "unknown";
  String msg = "Injection refusée (cause inconnue).";
  JsonDocument doc;
  switch (d.cause) {
    case ManualInjectRefusal::WatchdogInactive:
      code = "watchdog_inactive";
      msg = "Watchdog inactif — injection bloquée (sécurité).";
      break;
    case ManualInjectRefusal::FiltrationOff:
      code = "filtration_off";
      msg = "Filtration arrêtée — injection refusée pour sécurité chimique (pas de circulation = surdosage local).";
      break;
    case ManualInjectRefusal::StabilizationActive:
      code = "stabilization_in_progress";
      msg = "Stabilisation post-calibration en cours — réessayer dans " + String(in.stabilizationRemainingS) + " s.";
      doc["seconds_remaining"] = in.stabilizationRemainingS;
      break;
    case ManualInjectRefusal::AlreadyInjecting:
      code = "already_injecting";
      msg = "Une injection manuelle est déjà en cours — l'arrêter avant d'en relancer une.";
      break;
    case ManualInjectRefusal::DailyLimit: {
      // Reliquat arrondi VERS LE BAS (floorf, jamais lroundf) : le message ne
      // doit jamais promettre plus que ce que la garde acceptera (v2.9.2 —
      // lroundf affichait « reste 11 mL » pour un reliquat réel de 10,6).
      long remainingFloor = (long)floorf(d.remainingMl);
      if (remainingFloor < 0) remainingFloor = 0;
      code = "daily_limit";
      msg = "Limite journalière atteinte — reste disponible aujourd'hui : " + String(remainingFloor) + " mL.";
      doc["remaining_ml"] = remainingFloor;
      break;
    }
    case ManualInjectRefusal::HourlyLimit:
      code = "hourly_limit";
      msg = "Limite horaire d'injection atteinte — réessayer plus tard.";
      break;
    case ManualInjectRefusal::MaxCyclesPerDay:
      code = "max_cycles";
      msg = "Nombre maximal de démarrages de pompe atteint pour aujourd'hui.";
      break;
    case ManualInjectRefusal::BurstPerMinute:
    case ManualInjectRefusal::BurstPer15Min:
      code = "burst_limit";
      msg = "Trop de démarrages rapprochés (anti-rafale) — patienter avant de réessayer.";
      break;
    case ManualInjectRefusal::None:
      break;  // impossible (d.allowed == true traité plus haut)
  }
  doc["error"] = code;
  doc["message"] = msg;

  systemLogger.critical(String("[Sécurité] Injection ") + (isPh ? "pH" : "ORP") +
                        " manuelle refusée : " + msg);

  String out;
  serializeJson(doc, out);
  req->send(409, "application/json", out);
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
// feature-056 (condition pool-chemistry #7) : re-sourcé sur resolveWaterPresent()
// → une injection manuelle volumée en cours est interrompue (CRITICAL) si l'eau
// est perdue, y compris quand le signal externe (mode External) devient périmé.
static bool filtrationOkForInjection() {
  return filtration.resolveWaterPresence().waterPresent;
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
      // Fin NATURELLE : crédit plancher du volume promis (bug sous-compte
      // v2.9.1 — l'intégration débit×temps perd la dernière tranche).
      PumpController.creditManualInjectionFloor(0, manualInjectPh.startCumulMl,
                                                manualInjectPh.creditMl);
      manualInjectPh.startCumulMl = 0.0f;
      manualInjectPh.creditMl = 0.0f;
      systemLogger.info("[Injection] pH arrêtée automatiquement (fin de durée) — cumul crédité à " +
                        String(safetyLimits.dailyPhInjectedMl, 1) + " mL");
    }
    // 2. Arrêt sécurité chimique : filtration arrêtée pendant l'injection
    //    (pool-chemistry condition #1 : pas de circulation = surdosage local)
    else if (!filtrationOkForInjection()) {
      PumpController.setManualPump(mqttCfg.phPump - 1, 0);
      manualInjectPh.active = false;
      manualInjectPh.requestedVolumeMl = 0.0f;
      // Arrêt ANTICIPÉ : pas de crédit (l'intégration réelle fait foi) — hygiène.
      manualInjectPh.startCumulMl = 0.0f;
      manualInjectPh.creditMl = 0.0f;
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
      // Fin NATURELLE : crédit plancher du volume promis (cf. cas pH).
      PumpController.creditManualInjectionFloor(1, manualInjectOrp.startCumulMl,
                                                manualInjectOrp.creditMl);
      manualInjectOrp.startCumulMl = 0.0f;
      manualInjectOrp.creditMl = 0.0f;
      systemLogger.info("[Injection] ORP arrêtée automatiquement (fin de durée) — cumul crédité à " +
                        String(safetyLimits.dailyOrpInjectedMl, 1) + " mL");
    }
    else if (!filtrationOkForInjection()) {
      PumpController.setManualPump(mqttCfg.orpPump - 1, 0);
      manualInjectOrp.active = false;
      manualInjectOrp.requestedVolumeMl = 0.0f;
      // Arrêt ANTICIPÉ : pas de crédit (l'intégration réelle fait foi) — hygiène.
      manualInjectOrp.startCumulMl = 0.0f;
      manualInjectOrp.creditMl = 0.0f;
      systemLogger.critical("[Injection] ORP INTERROMPUE — filtration arrêtée (sécurité chimique)");
      mqttManager.publishAlert("orp_injection_aborted",
                               "Injection ORP/chlore interrompue : filtration arrêtée pendant l'injection. Relancer manuellement après reprise filtration.");
    }
  }
}

void setupControlRoutes(AsyncWebServer* server) {
  // Routes pour test manuel des pompes - PROTÉGÉES
  // Sécurité chimique (pool-chemistry validation 2026-05-11, feature-056) :
  // démarrage refusé sans présence d'eau (resolveWaterPresence().waterPresent,
  // selon le mode d'installation). Pas de bornage de durée ici (test bench) — l'utilisateur doit
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
  // feature-006 : gardes de sécurité chimique via evaluateManualInject()
  // (watchdog, filtration, stabilisation, double start, limites journalière/
  // horaire, cycles/jour, anti-rafale) → 409 JSON structuré en cas de refus.
  server->on("/ph/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.phPump - 1;
    uint8_t dutyPct = (pumpIdx == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
    float flow = calcInjectFlow(dutyPct, phPumpControl);
    // Garde : sans débit configuré, volumeMl/flow = inf et effectiveMl = 0
    // passerait trivialement la garde journalière → refus explicite.
    if (flow <= 0.0f) { req->send(400, "text/plain", "débit pompe non configuré"); return; }
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
    // Écrêtage au reliquat journalier (bug v2.9.2) : si la demande dépasse ce
    // qui reste disponible aujourd'hui, on la ramène au reliquat au lieu de
    // laisser la garde refuser (« reste 11 mL » puis refus de 11 mL). Durée
    // arrondie VERS LE BAS (floor volontaire) : le volume effectif ne doit
    // jamais re-déborder la limite. Si le reliquat est trop petit pour 1 s de
    // pompe, on n'écrête PAS → la garde refuse daily_limit comme avant.
    bool clamped = false;
    int clampedDurationS = 0;
    float remaining = 0.0f;
    if (safetyLimits.maxPhMlPerDay > 0) {
      remaining = safetyLimits.maxPhMlPerDay - safetyLimits.dailyPhInjectedMl;
      if (volumeMl > remaining && remaining > 0.0f) {
        int clampedS = (int)((remaining / flow) * 60.0f);  // FLOOR volontaire
        if (clampedS >= 1) {
          systemLogger.info("[Injection] pH : volume écrêté de " + String(volumeMl, 1) +
                            " à " + String(remaining, 1) + " mL (reliquat journalier)");
          durationS = clampedS;
          clampedDurationS = clampedS;
          volumeMl = remaining;
          clamped = true;
        }
      }
    }
    if (durationS > kManualInjectMaxDurationS) durationS = kManualInjectMaxDurationS;
    // Volume effectif POST-clamp : si la durée a été plafonnée, le volume
    // réellement injecté est flow×durée (condition pool-chemistry #3 : les
    // gardes évaluent ce qui sera VRAIMENT injecté, pas la demande initiale).
    float effectiveMl = flow * durationS / 60.0f;
    if (!manualInjectGuardOrReject(req, /*isPh=*/true, effectiveMl, durationS)) return;
    // Injection acceptée : enregistrer le démarrage de cycle MANUEL (ring
    // anti-rafale + cyclesToday partagés avec l'auto, consommé en loopTask).
    PumpController.requestManualCycleRecord(0);
    uint8_t duty = (uint8_t)((dutyPct * MAX_PWM_DUTY) / 100);
    PumpController.setManualPump(pumpIdx, duty);
    manualInjectPh.active = true;
    manualInjectPh.startMs = millis();
    manualInjectPh.durationMs = (unsigned long)durationS * 1000UL;
    manualInjectPh.requestedVolumeMl = volumeMl;
    // Crédit plancher (bug sous-compte v2.9.1) : mémoriser le cumul au départ
    // et le volume promis. Écritures float 32 bits depuis le handler async,
    // lues ensuite en loopTask — même pattern bénin que startMs/durationMs.
    manualInjectPh.startCumulMl = safetyLimits.dailyPhInjectedMl;
    // Crédit de fin d'injection :
    // - écrêté au reliquat SANS re-plafonnement par kManualInjectMaxDurationS
    //   → créditer le reliquat ENTIER : le plancher porte le cumul exactement
    //   à la limite journalière → latch *_limit_reached + badge. Sur-compte
    //   borné à < 1 s de débit (floor de la durée), conservateur — validé spec.
    // - re-plafonné par kManualInjectMaxDurationS (reliquat > 10 min de pompe)
    //   → le volume réellement injecté est effectiveMl < remaining : le crédit
    //   ne doit JAMAIS excéder ce qui a pu être injecté → effectiveMl.
    manualInjectPh.creditMl =
        (clamped && durationS == clampedDurationS) ? remaining : effectiveMl;
    systemLogger.info("[Injection] pH démarrée " + String(durationS) + "s pour " + String(volumeMl, 1) + "mL (débit=" + String(flow, 1) + "mL/min, pompe " + String(mqttCfg.phPump) + ")");
    req->send(200, "text/plain", "OK");
  });

  server->on("/ph/inject/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.phPump - 1;
    PumpController.setManualPump(pumpIdx, 0);
    manualInjectPh.active = false;
    // Arrêt anticipé : pas de crédit plancher (intégration réelle) — hygiène.
    manualInjectPh.startCumulMl = 0.0f;
    manualInjectPh.creditMl = 0.0f;
    systemLogger.info("[Injection] pH arrêtée manuellement");
    req->send(200, "text/plain", "OK");
  });

  // Injection manuelle ORP — démarre la pompe ORP à la puissance configurée
  // Paramètre préféré : ?volume=N (mL, max 2000)
  // Fallback legacy : ?duration=N (secondes, borné à kManualInjectMaxDurationS)
  // feature-006 : gardes de sécurité chimique via evaluateManualInject()
  // (watchdog, filtration, stabilisation, double start, limites journalière/
  // horaire, cycles/jour, anti-rafale) → 409 JSON structuré en cas de refus.
  server->on("/orp/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.orpPump - 1;
    uint8_t dutyPct = (pumpIdx == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
    float flow = calcInjectFlow(dutyPct, orpPumpControl);
    // Garde : sans débit configuré, volumeMl/flow = inf et effectiveMl = 0
    // passerait trivialement la garde journalière → refus explicite.
    if (flow <= 0.0f) { req->send(400, "text/plain", "débit pompe non configuré"); return; }
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
    // Écrêtage au reliquat journalier (bug v2.9.2, cf. route pH) : durée FLOOR
    // pour ne jamais re-déborder ; reliquat < 1 s de pompe → pas d'écrêtage,
    // la garde refuse daily_limit comme avant.
    bool clamped = false;
    int clampedDurationS = 0;
    float remaining = 0.0f;
    if (safetyLimits.maxChlorineMlPerDay > 0) {
      remaining = safetyLimits.maxChlorineMlPerDay - safetyLimits.dailyOrpInjectedMl;
      if (volumeMl > remaining && remaining > 0.0f) {
        int clampedS = (int)((remaining / flow) * 60.0f);  // FLOOR volontaire
        if (clampedS >= 1) {
          systemLogger.info("[Injection] ORP : volume écrêté de " + String(volumeMl, 1) +
                            " à " + String(remaining, 1) + " mL (reliquat journalier)");
          durationS = clampedS;
          clampedDurationS = clampedS;
          volumeMl = remaining;
          clamped = true;
        }
      }
    }
    if (durationS > kManualInjectMaxDurationS) durationS = kManualInjectMaxDurationS;
    // Volume effectif POST-clamp (condition pool-chemistry #3, cf. route pH).
    float effectiveMl = flow * durationS / 60.0f;
    if (!manualInjectGuardOrReject(req, /*isPh=*/false, effectiveMl, durationS)) return;
    // Injection acceptée : enregistrer le démarrage de cycle MANUEL (ring
    // anti-rafale + cyclesToday partagés avec l'auto, consommé en loopTask).
    PumpController.requestManualCycleRecord(1);
    uint8_t duty = (uint8_t)((dutyPct * MAX_PWM_DUTY) / 100);
    PumpController.setManualPump(pumpIdx, duty);
    manualInjectOrp.active = true;
    manualInjectOrp.startMs = millis();
    manualInjectOrp.durationMs = (unsigned long)durationS * 1000UL;
    manualInjectOrp.requestedVolumeMl = volumeMl;
    // Crédit plancher (bug sous-compte v2.9.1) : cf. route pH — pattern
    // d'écriture async → lecture loopTask identique à startMs/durationMs.
    manualInjectOrp.startCumulMl = safetyLimits.dailyOrpInjectedMl;
    // Crédit de fin d'injection (cf. route pH) : reliquat ENTIER si écrêté
    // sans re-plafonnement durée (latch exact de la limite, sur-compte < 1 s
    // de débit, conservateur) ; sinon effectiveMl — le crédit ne doit JAMAIS
    // excéder ce qui a pu être réellement injecté.
    manualInjectOrp.creditMl =
        (clamped && durationS == clampedDurationS) ? remaining : effectiveMl;
    systemLogger.info("[Injection] ORP démarrée " + String(durationS) + "s pour " + String(volumeMl, 1) + "mL (débit=" + String(flow, 1) + "mL/min, pompe " + String(mqttCfg.orpPump) + ")");
    req->send(200, "text/plain", "OK");
  });

  server->on("/orp/inject/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.orpPump - 1;
    PumpController.setManualPump(pumpIdx, 0);
    manualInjectOrp.active = false;
    // Arrêt anticipé : pas de crédit plancher (intégration réelle) — hygiène.
    manualInjectOrp.startCumulMl = 0.0f;
    manualInjectOrp.creditMl = 0.0f;
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

  // feature-053 : Mode Boost (surchloration temporaire du jour, auto-off à minuit).
  // start/stopBoost prennent configMutex en interne (saveBoostState) et ne font que
  // time()/mktime + une petite écriture NVS dédiée — même profil que les routes de
  // contrôle existantes (lighting/on → saveMqttConfig écrit aussi depuis le handler
  // async). Appel direct, cohérent avec le pattern dominant des routes de contrôle.
  // startBoost REFUSE si l'heure n'est pas synchronisée → on renvoie alors 409.
  server->on("/boost/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    startBoost();
    if (!boostState.active) {
      // Refus fail-closed : heure non synchronisée (expiration minuit indéterminable).
      req->send(409, "application/json",
                "{\"error\":\"time_not_synced\",\"message\":\"Boost impossible : horloge non synchronisée (expiration à minuit indéterminable).\"}");
      return;
    }
    mqttManager.publishBoostState();   // reflète immédiatement l'état sur HA
    wsManager.requestConfigBroadcast(); // resync UI web (pattern bug-sync)
    // feature-054 : indiquer à l'UI quels leviers sont réellement actifs
    // (filtration prolongée seulement si gérée ; chlore relevé seulement si ORP automatic).
    const bool filtrationExtended = (mqttCfg.installMode == InstallMode::ManagedFiltration);  // feature-056
    const bool chlorineBoosted = (mqttCfg.orpRegulationMode == "automatic");
    String out = String("{\"boost_active\":true,\"boost_until\":") +
                 String((long)boostState.untilEpoch) +
                 ",\"filtration_extended\":" + (filtrationExtended ? "true" : "false") +
                 ",\"chlorine_boosted\":" + (chlorineBoosted ? "true" : "false") + "}";
    req->send(200, "application/json", out);
  });

  server->on("/boost/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    stopBoost();
    mqttManager.publishBoostState();
    wsManager.requestConfigBroadcast();
    req->send(200, "application/json", "{\"boost_active\":false,\"boost_until\":0}");
  });

  // feature-056 : signal d'état de la filtration EXTERNE (mode ExternalFiltration).
  // Handler NON bloquant : setExternalState écrit le triplet {on,lastMs,known} sous
  // spinlock (horodatage millis() interne), aucune persistance NVS (condition
  // pool-chemistry #3 : boot toujours OFF/known=false → fail-safe). Le paramètre
  // `running` est accepté en corps de formulaire OU en query (true/false/1/0/on/off).
  server->on("/filtration/external-state", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    auto* p = req->getParam("running", true);   // corps (form-urlencoded)
    if (!p) p = req->getParam("running");        // fallback query string
    if (!p) {
      req->send(400, "application/json",
                "{\"error\":\"missing_param\",\"message\":\"Paramètre 'running' requis (true/false).\"}");
      return;
    }
    String v = p->value();
    v.toLowerCase();
    bool running = (v == "true" || v == "1" || v == "on");
    filtration.setExternalState(running);
    wsManager.requestConfigBroadcast();  // resync UI (pattern bug-sync)
    req->send(200, "application/json",
              String("{\"external_state\":") + (running ? "true" : "false") + "}");
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
