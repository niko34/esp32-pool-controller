#include "web_routes_control.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "pump_controller.h"
#include "filtration.h"
#include "lighting.h"
#include "logger.h"
#include "sensors.h"   // v2.20.3 (D1) : requestFilterReset() — reset filtre différé
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
// -----------------------------------------------------------------------------

// Lit le paramètre optionnel ?duty=N des routes de TEST de pompe (/pumpN/on).
// Défaut MAX_PWM_DUTY : comportement historique strictement inchangé sans le paramètre.
// Borné à [0, MAX_PWM_DUTY] et volontairement PAS borné par MIN_ACTIVE_DUTY — le but est
// justement de sonder SOUS le plancher pour trouver où la pompe cesse de tourner
// (bug-dosage-fantome-duty-non-calibre : le régulateur commande 82/255 pour une erreur de
// 0,8 pH, valeur où la pompe ne tourne que par intermittence alors que le modèle
// duty↔débit la croit à 6,4 ml/min).
static uint8_t readTestDutyParam(AsyncWebServerRequest* req) {
  const AsyncWebParameter* p = req->getParam("duty", true);
  if (p == nullptr) p = req->getParam("duty");
  if (p == nullptr) return MAX_PWM_DUTY;
  long v = p->value().toInt();
  if (v < 0) v = 0;
  if (v > (long)MAX_PWM_DUTY) v = MAX_PWM_DUTY;
  return (uint8_t)v;
}

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
  // Auto-guérison v2.9.1 (condition pool-chemistry C2) : en mode maintenance, le
  // dosage est suspendu. On refuse le démarrage ICI, AVANT que le handler n'arme
  // manualInject*.active, sinon creditManualInjectionFloor() créditerait à échéance
  // un volume « promis » alors qu'aucun produit n'a coulé (sur-comptage du registre
  // journalier). Point commun aux deux handlers /ph|orp/inject/start.
  if (PumpController.isMaintenanceInhibited()) {
    req->send(409, "application/json",
              "{\"error\":\"maintenance_mode\",\"message\":\"Mode maintenance actif (trop de redémarrages auto) — dosage suspendu. Redémarrez le contrôleur pour réactiver.\"}");
    return false;
  }

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

// =============================================================================
// Itération 2 (bug-dosage-fantome, W3) : plan débit/duty d'une injection
// manuelle VOLUMÉE. Le modèle linéaire duty↔débit est MORT (mesure au banc du
// 2026-08-23 : ×1,9 de sous-estimation à duty 82, ×1,5 de surestimation à 255)
// — plus JAMAIS d'interpolation. L'ancien calcInjectFlow estimait la durée sur
// ce modèle réinitialisé → durée fausse, sur-injection physique possible.
// Les deux SEULS points où le débit est CONNU :
//   - amplitude configurée == duty calibré (pesée A) → débit pesé, duty calibré ;
//   - sinon (pompe jamais caractérisée, ou amplitude ≠ pesée) → duty FORCÉ à
//     255 et débit = kPumpNFlow255MlPerMin (pesée B) : durée volume/débit
//     exacte, au lieu d'une durée devinée sur un débit fictif ;
//   - état incohérent (duty calibré posé mais débit pesé invalide — normalement
//     impossible : garde /save-config + assainissement NVS) → refus (ok=false),
//     le handler répond 409 flow_uncharacterized.
// =============================================================================
struct ManualInjectPlan {
  bool ok;             // false = aucun débit connu pour ce chemin → 409
  float flowMlPerMin;  // débit CONNU au duty commandé
  uint8_t duty;        // duty réellement commandé (calibré ou 255)
};

static ManualInjectPlan planManualInject(int pumpIdx) {
  ManualInjectPlan plan = {false, 0.0f, 0};
  if (pumpIdx != 0 && pumpIdx != 1) return plan;
  const uint8_t calDuty  = (pumpIdx == 0) ? mqttCfg.pump1CalibratedDuty
                                          : mqttCfg.pump2CalibratedDuty;
  const float pulseFlow  = (pumpIdx == 0) ? mqttCfg.pump1PulseFlowMlPerMin
                                          : mqttCfg.pump2PulseFlowMlPerMin;
  const float flow255    = (pumpIdx == 0) ? kPump1Flow255MlPerMin
                                          : kPump2Flow255MlPerMin;
  const uint8_t ampDuty  = PumpController.getPumpAmplitudeDuty(pumpIdx);
  if (calDuty != 0 && ampDuty == calDuty) {
    // Ce chemin exige le débit pesé : sans pesée plausible → refus explicite,
    // jamais de repli silencieux sur un débit deviné.
    const bool flowValid = isfinite(pulseFlow) && pulseFlow > 0.0f &&
                           pulseFlow <= kPulseFlowMaxMlPerMin;
    if (!flowValid) return plan;
    plan.ok = true;
    plan.flowMlPerMin = pulseFlow;
    plan.duty = calDuty;
    return plan;
  }
  plan.ok = true;
  plan.flowMlPerMin = flow255;
  plan.duty = MAX_PWM_DUTY;
  return plan;
}

// Refus 409 commun aux deux routes d'injection volumée quand aucun débit n'est
// connu (planManualInject().ok == false). Code aligné sur PulseRefusal::
// FlowUncharacterized côté régulation.
static void sendFlowUncharacterized(AsyncWebServerRequest* req, const char* tag) {
  systemLogger.critical(String("[Sécurité] ") + tag +
                        " refusée : débit non caractérisé (pesée requise)");
  req->send(409, "application/json",
            "{\"error\":\"flow_uncharacterized\",\"message\":\"Débit de la pompe non caractérisé — effectuez la pesée (Réglages → Avancé → Configuration des pompes) avant une injection volumée.\"}");
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
  // Paramètre OPTIONNEL ?duty=N (0-255), défaut MAX_PWM_DUTY : sert à CARACTÉRISER
  // le seuil de rotation réel de la pompe. Motif (bug-dosage-fantome-duty-non-calibre) :
  // le régulateur commande des duty au ras de MIN_ACTIVE_DUTY (82/255 pour une erreur
  // de 0,8 pH) où la pompe ne tourne que par intermittence, alors que le modèle
  // duty↔débit la croit à 6,4 ml/min. Sans balayage possible depuis l'interface, la
  // valeur du plancher ne pourrait être que devinée.
  // Sûreté : ce paramètre ne peut que RÉDUIRE la puissance par rapport au 255 déjà
  // atteignable sans lui, et toutes les gardes d'injection restent en amont.
  server->on("/pump1/on", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    if (!injectionAllowedOrReject(req, "Test pompe 1")) return;
    uint8_t duty = readTestDutyParam(req);
    PumpController.setManualPump(0, duty);
    systemLogger.info("[Test] Pompe 1 démarrée en mode manuel (duty=" + String(duty) + ")");
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
    uint8_t duty = readTestDutyParam(req);   // cf. commentaire de /pump1/on
    PumpController.setManualPump(1, duty);
    systemLogger.info("[Test] Pompe 2 démarrée en mode manuel (duty=" + String(duty) + ")");
    req->send(200, "text/plain", "OK");
  });

  server->on("/pump2/off", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    PumpController.setManualPump(1, 0);
    systemLogger.info("[Test] Pompe 2 arrêtée");
    req->send(200, "text/plain", "OK");
  });

  // Injection manuelle pH — démarre la pompe pH au duty CALIBRÉ (pesée) si
  // l'amplitude configurée y correspond, sinon à 255 (seul autre débit connu).
  // Paramètre préféré : ?volume=N (mL, max 2000)
  // Fallback legacy : ?duration=N (secondes, borné à kManualInjectMaxDurationS)
  // feature-006 : gardes de sécurité chimique via evaluateManualInject()
  // (watchdog, filtration, stabilisation, double start, limites journalière/
  // horaire, cycles/jour, anti-rafale) → 409 JSON structuré en cas de refus.
  server->on("/ph/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.phPump - 1;
    // W3 : débit/duty issus des SEULS points mesurés (pesée ou 255) — jamais
    // d'interpolation. plan.ok garantit flow > 0 (les gardes volumeMl/flow et
    // effectiveMl restent bien définies).
    ManualInjectPlan plan = planManualInject(pumpIdx);
    if (!plan.ok) { sendFlowUncharacterized(req, "Injection pH manuelle"); return; }
    float flow = plan.flowMlPerMin;
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
    PumpController.setManualPump(pumpIdx, plan.duty);
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
    systemLogger.info("[Injection] pH démarrée " + String(durationS) + "s pour " + String(volumeMl, 1) + "mL (débit=" + String(flow, 1) + "mL/min, duty=" + String(plan.duty) + ", pompe " + String(mqttCfg.phPump) + ")");
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

  // Injection manuelle ORP — démarre la pompe ORP au duty CALIBRÉ (pesée) si
  // l'amplitude configurée y correspond, sinon à 255 (seul autre débit connu).
  // Paramètre préféré : ?volume=N (mL, max 2000)
  // Fallback legacy : ?duration=N (secondes, borné à kManualInjectMaxDurationS)
  // feature-006 : gardes de sécurité chimique via evaluateManualInject()
  // (watchdog, filtration, stabilisation, double start, limites journalière/
  // horaire, cycles/jour, anti-rafale) → 409 JSON structuré en cas de refus.
  server->on("/orp/inject/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    int pumpIdx = mqttCfg.orpPump - 1;
    // W3 : cf. route pH — débit/duty issus des seuls points mesurés.
    ManualInjectPlan plan = planManualInject(pumpIdx);
    if (!plan.ok) { sendFlowUncharacterized(req, "Injection ORP manuelle"); return; }
    float flow = plan.flowMlPerMin;
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
    PumpController.setManualPump(pumpIdx, plan.duty);
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
    systemLogger.info("[Injection] ORP démarrée " + String(durationS) + "s pour " + String(volumeMl, 1) + "mL (débit=" + String(flow, 1) + "mL/min, duty=" + String(plan.duty) + ", pompe " + String(mqttCfg.orpPump) + ")");
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

  // ---------- POST /reset-sensor-filter?sensor=ph|orp ----------
  // v2.20.3 (D1, bug-filtre-ph-verrouille) : réinitialisation manuelle du filtre
  // pH ou ORP, hors des routes de debug. Sortie de secours utilisateur quand le
  // filtre reste verrouillé (incident 2026-08-16 : la SEULE porte de sortie était
  // POST /debug/sensor_filter_reset, absente de l'interface, et elle sacrifiait
  // aussi le filtre ORP sain).
  //
  // Gardes (verdict pool-chemistry P6) :
  //   - auth ADMIN (RouteProtection::CRITICAL) : un reset relance un warmup et
  //     efface la détection de sonde figée + le latch EMI ;
  //   - SÉLECTIF par capteur : réparer le pH ne touche jamais l'ORP ;
  //   - 1 reset par capteur par kSensorFilterUiResetMinIntervalMs (10 min) → 429 ;
  //   - AUCUN accès direct au filtre depuis ce handler : requestFilterReset()
  //     pose un drapeau `volatile` consommé en tête de SensorManager::update(),
  //     dans le SEUL contexte loopTask autorisé à écrire dans un SensorFilter
  //     (contrat sensor_filter.h). Le journal de l'état détruit (D4) est émis
  //     là-bas, au moment de l'exécution réelle.
  server->on("/reset-sensor-filter", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);
    auto* p = req->getParam("sensor", true);   // corps (form-urlencoded)
    if (!p) p = req->getParam("sensor");        // fallback query string
    String sensorName = p ? p->value() : String();
    sensorName.toLowerCase();
    const bool isPh  = (sensorName == "ph");
    const bool isOrp = (sensorName == "orp");
    if (!isPh && !isOrp) {
      req->send(400, "application/json",
                "{\"error\":\"invalid_sensor\",\"message\":\"Param\\u00e8tre 'sensor' "
                "requis : ph ou orp.\"}");
      return;
    }

    // Anti-abus : compteurs statiques (handlers async sérialisés sur la tâche
    // async_tcp — pas de course entre deux requêtes). 0 = jamais réinitialisé.
    static uint32_t sLastResetMs[2] = {0, 0};
    const uint8_t idx = isPh ? 0 : 1;
    const uint32_t nowMs = (uint32_t)millis();
    if (sLastResetMs[idx] != 0 &&
        (nowMs - sLastResetMs[idx]) < kSensorFilterUiResetMinIntervalMs) {
      const uint32_t waitS =
          (kSensorFilterUiResetMinIntervalMs - (nowMs - sLastResetMs[idx])) / 1000UL;
      req->send(429, "application/json",
                String("{\"error\":\"too_soon\",\"retry_after_s\":") + waitS +
                    ",\"message\":\"Un seul r\\u00e9armement par capteur toutes les 10 "
                    "minutes (le filtre doit d'abord se r\\u00e9amorcer).\"}");
      return;
    }

    if (!sensors.requestFilterReset(isPh, isOrp, "interface web")) {
      req->send(500, "application/json", "{\"error\":\"queue_failed\"}");
      return;
    }
    sLastResetMs[idx] = (nowMs != 0) ? nowMs : 1;  // 0 = sentinelle « jamais »
    req->send(200, "application/json",
              String("{\"queued\":true,\"sensor\":\"") + (isPh ? "ph" : "orp") + "\"}");
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
