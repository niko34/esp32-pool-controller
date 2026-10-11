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
#include "flow_assist_logic.h"
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
  const float flow255    = kPumpFlow255MlPerMin;  // majorant de flotte (v2.25.2)
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

// =============================================================================
// feature-075 : session « assistant de mesure du débit » (une seule pompe à la
// fois). État écrit par les handlers async (/flow-assist/*) et lu/écrit par le
// tick de timeout en loopTask (updateManualInject) — course BÉNIGNE assumée
// (patron manualInject* : champs 8/32 bits, écritures atomiques Xtensa, pire
// cas une décision différée d'un tour de loop, fail-closed côté suspension :
// le timeout serveur kFlowAssistSuspendTimeoutMs rétablit toujours le dosage).
// Les durées/duty des marches sont FIXÉS SERVEUR (jamais acceptés du client) :
//   - purge  : duty 255 (seul débit connu hors calibration), kFlowAssistPurgeDurationS ;
//   - mesure : amplitude d'exploitation (getPumpAmplitudeDuty), kFlowAssistMeasureDurationS.
// Le véhicule d'exécution est le chemin manualInject* existant (minuterie
// serveur + crédit plancher à échéance) — AUCUNE nouvelle mécanique de pompe.
// -----------------------------------------------------------------------------
struct FlowAssistSession {
  bool active = false;                          // session ouverte (suspension posée)
  int pumpIdx = -1;                             // pompe PHYSIQUE (0/1)
  FlowAssistPhase phase = FlowAssistPhase::kPurge;  // dernière phase lancée
  bool runOwned = false;                        // la marche manualInject en cours est celle de l'assistant
  bool aborted = false;                         // dernière marche interrompue par le SERVEUR (perte de filtration)
  uint8_t purgesDone = 0;                       // purges consommées dans la session
  uint32_t lastTouchMs = 0;                     // réarmé à CHAQUE requête /flow-assist/*
};
static FlowAssistSession sFlowAssist;

// Chimie de la pompe PHYSIQUE en assistant (résout l'affectation mqttCfg.phPump).
static bool flowAssistPumpIsPh(int pumpIdx) {
  return pumpIdx == (mqttCfg.phPump - 1);
}

// Débit MAJORANT à duty 255 — base du crédit de sécurité (surestimer est
// fail-safe, P4 : jamais zéro sur pompe alimentée). Depuis v2.25.2 c'est une
// constante de FLOTTE unique (kPumpFlow255MlPerMin) : plus de dispatch par
// index physique, qui attribuait la mesure d'un exemplaire à un numéro de pompe.

// Arrêt ANTICIPÉ d'une marche d'assistant (stop utilisateur, exit, timeout).
// Condition gate 6 (vérifiée sur le code) — PRÉMISSE CORRIGÉE EN v2.61.0
// (bug-115) : sous suspension flow-assist, `update()` appelle désormais
// settlePumpAccounting() (donc accountPumpVolume()) à CHAQUE tour, y compris
// sur le chemin perte-filtration — la comptabilité PAR TICK n'est plus sautée.
// Le crédit-plancher à échéance (creditManualInjectionFloor, fin NATURELLE
// dans updateManualInject) devient de ce fait un PLANCHER RAREMENT MORDANT sur
// ce chemin. ⚠️ Revue bug-115 : « no-op » SURÉVALUAIT. creditManualInjectionFloor
// est un max() au MAJORANT flow255 — quand le tick a crédité au débit CALIBRÉ
// (plus faible), le plancher remonte encore `daily`. Surcompte, donc fail-safe,
// mais pas neutre
// (le tick a déjà tout crédité) — conservé ici en DÉFENSE EN PROFONDEUR, hors
// périmètre de bug-115. Historique inchangé : l'arrêt anticipé
// « sans crédit » du chemin manualInject classique laisserait un volume NUL sur
// une pompe qui a réellement tourné (violation P4). Adaptation : on coupe la
// pompe ICI (arrêt immédiat), on RAMÈNE creditMl au majorant PRORATA du temps
// écoulé (flow255 × elapsed — majorant exact pour la purge à 255, majorant
// conservateur pour la mesure à amplitude ≤ 255), puis on raccourcit durationMs
// à elapsed : la « fin naturelle » du prochain tour d'updateManualInject exécute
// creditManualInjectionFloor en loopTask (seul contexte autorisé) et nettoie.
// Ordre d'écriture VOULU : creditMl AVANT durationMs (le déclencheur) — si la
// loop passe entre les deux, elle voit l'ancienne durée et créditera au tour
// suivant avec le creditMl déjà proraté (course bénigne).
static bool flowAssistStopRun() {
  if (!sFlowAssist.runOwned || sFlowAssist.pumpIdx < 0) return false;
  const bool isPh = flowAssistPumpIsPh(sFlowAssist.pumpIdx);
  ManualInjectState& mi = isPh ? manualInjectPh : manualInjectOrp;
  if (!mi.active) return false;
  PumpController.setManualPump(sFlowAssist.pumpIdx, 0);  // coupure immédiate
  unsigned long elapsedMs = millis() - mi.startMs;
  if (elapsedMs > mi.durationMs) elapsedMs = mi.durationMs;
  mi.creditMl = kPumpFlow255MlPerMin * (float)elapsedMs / 60000.0f;
  mi.durationMs = elapsedMs;  // échéance immédiate → crédit plancher au prochain tour de loop
  systemLogger.info(String("[Assistant débit] Marche arrêtée par anticipation (pompe ") +
                    String(sFlowAssist.pumpIdx + 1) + ", " + String(elapsedMs / 1000UL) +
                    " s écoulées, crédit " + String(mi.creditMl, 1) + " mL au majorant)");
  return true;
}

// feature-075 (P4, anomalie test-engineer) : crédit d'une marche d'ASSISTANT
// interrompue par la PERTE DE FILTRATION. Hors assistant, l'abandon historique
// ne crédite pas (« l'intégration réelle fait foi » : accountPumpVolume a déjà
// compté chaque tick) — comportement STRICTEMENT inchangé.
// PRÉMISSE CORRIGÉE EN v2.61.0 (bug-115) : sous suspension flow-assist, la
// comptabilité par tick n'est PLUS sautée — `update()` appelle désormais
// settlePumpAccounting() (donc accountPumpVolume()) y compris sur le chemin
// perte-filtration. Le crédit prorata ci-dessous devient donc un PLANCHER
// redondant avec le tick déjà crédité — conservé en DÉFENSE EN PROFONDEUR
// (hors périmètre de bug-115, cf. creditManualInjectionFloor : `if (daily <
// floorMl)` ne mord plus que si le tick a crédité au débit calibré, inférieur au
// majorant — voir la nuance en tête de fichier). Crédit au MAJORANT
// prorata (flow255 × temps écoulé), appliqué
// DIRECTEMENT via creditManualInjectionFloor : on est en loopTask
// (updateManualInject), le seul contexte autorisé — pas besoin du détour
// « durationMs raccourci » de flowAssistStopRun (réservé aux handlers async).
// À appeler AVANT la remise à zéro de startCumulMl/creditMl du chemin d'abandon.
static void flowAssistCreditAbortedRun(bool isPh, const ManualInjectState& mi,
                                       unsigned long now) {
  if (!sFlowAssist.runOwned) return;                        // pas une marche d'assistant
  if (flowAssistPumpIsPh(sFlowAssist.pumpIdx) != isPh) return;  // autre chimie
  unsigned long elapsedMs = now - mi.startMs;
  if (elapsedMs > mi.durationMs) elapsedMs = mi.durationMs;
  const float creditMl =
      kPumpFlow255MlPerMin * (float)elapsedMs / 60000.0f;
  PumpController.creditManualInjectionFloor(isPh ? 0 : 1, mi.startCumulMl, creditMl);
  sFlowAssist.runOwned = false;  // marche close et créditée
  // Revue feature-075 : signaler l'interruption SERVEUR à l'UI (/data). Sans ce
  // drapeau, l'échéance du compte à rebours client routerait vers la saisie du
  // volume avec une mesure PARTIELLE → débit sous-estimé → sur-injection.
  sFlowAssist.aborted = true;
  systemLogger.info("[Assistant débit] Marche interrompue (perte de filtration) — crédit " +
                    String(creditMl, 1) + " mL au majorant (" +
                    String(elapsedMs / 1000UL) + " s écoulées)");
}

// feature-075 (contre-visite gate, P4) : les injections manuelles CLASSIQUES
// (/ph|orp/inject/start) sont REFUSÉES tant qu'une session d'assistant est
// active.
// PRÉMISSE CORRIGÉE EN v2.61.0 (bug-115) : l'intégration par tick n'est plus
// sautée sous suspension flow-assist (update() règle désormais la
// comptabilité à CHAQUE sortie, cf. settlePumpAccounting()) — une injection
// classique sur l'AUTRE chimie, interrompue par perte de filtration, ne
// serait donc plus créditée 0. Le refus 409 ci-dessous N'EST PAS LEVÉ pour
// autant : hors périmètre de bug-115 (défense en profondeur), il reste la
// politique voulue — on ne généralise pas le croisement manuel classique /
// assistant pour un gain devenu marginal. Le test /pumpN/on?duty= n'est pas
// concerné (hors injection minutée, pas de crédit plancher). Retourne false +
// 409 si refus.
static bool flowAssistAllowsManualInjectOrReject(AsyncWebServerRequest* req) {
  if (!sFlowAssist.active) return true;
  req->send(409, "application/json",
            "{\"error\":\"assist_active\",\"message\":\"Mesure du débit en cours — réessayez après la fin de l'assistant.\"}");
  return false;
}

FlowAssistStatus getFlowAssistStatus() {
  FlowAssistStatus st;
  st.purgesLeft = kFlowAssistMaxPurgesPerSession;
  if (!sFlowAssist.active) return st;
  st.active = true;
  st.pumpNumber = sFlowAssist.pumpIdx + 1;
  st.aborted = sFlowAssist.aborted;
  st.purgesLeft = (sFlowAssist.purgesDone >= kFlowAssistMaxPurgesPerSession)
                      ? 0 : (int)(kFlowAssistMaxPurgesPerSession - sFlowAssist.purgesDone);
  if (sFlowAssist.runOwned) {
    const ManualInjectState& mi = flowAssistPumpIsPh(sFlowAssist.pumpIdx)
                                      ? manualInjectPh : manualInjectOrp;
    if (mi.active) {
      st.phase = (sFlowAssist.phase == FlowAssistPhase::kPurge) ? "purge" : "measure";
      st.remainingS = manualInjectRemainingS(mi);
    }
  }
  return st;
}

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

  // ========== feature-075 : timeout serveur de la session d'assistant ==========
  // Un assistant abandonné (onglet fermé, WiFi coupé) ne laisse JAMAIS la
  // régulation suspendue indéfiniment : expiration wrap-safe sur lastTouchMs,
  // réarmé à chaque requête /flow-assist/*. En pratique aucune marche ne peut
  // être en cours ici (durée max 120 s, très inférieure au timeout 15 min
  // réarmé au démarrage de la marche) — l'arrêt
  // est défensif : il raccourcit durationMs, et les blocs pH/ORP ci-dessous
  // exécutent la fin naturelle (crédit plancher) dans ce MÊME appel.
  if (sFlowAssist.active &&
      isFlowAssistSessionExpired(sFlowAssist.lastTouchMs, (uint32_t)now,
                                 kFlowAssistSuspendTimeoutMs)) {
    flowAssistStopRun();
    PumpController.setFlowAssistSuspend(false);
    sFlowAssist = FlowAssistSession{};
    systemLogger.warning("[Assistant débit] Session expirée (15 min sans interaction) — "
                         "dosage automatique rétabli");
  }

  // feature-075 (revue, course timeout/start) : le bloc d'expiration ci-dessus
  // (loopTask) et un /flow-assist/start concurrent (tâche async) peuvent
  // s'entrelacer — le reset multi-champs de sFlowAssist n'est PAS atomique et
  // le setFlowAssistSuspend(false) du timeout peut écraser le true que le
  // handler vient de poser → session vivante SANS suspension (fail-open).
  // Réconciliation IDEMPOTENTE : une session active exige la suspension. La
  // fenêtre résiduelle (~1 tour de loop, ≈100 ms) est bénigne : la régulation
  // ne peut pas armer de pouls significatif en un tour, et la suspension est
  // rétablie ici au tour suivant (fail-closed).
  if (sFlowAssist.active && !PumpController.isFlowAssistSuspended()) {
    PumpController.setFlowAssistSuspend(true);
  }

  // feature-075 : la marche de l'assistant s'est terminée (fin naturelle,
  // arrêt anticipé déjà traité, ou interruption filtration) → libérer
  // l'attribution pour que /data ne rapporte plus de phase en cours.
  if (sFlowAssist.runOwned) {
    const ManualInjectState& s = flowAssistPumpIsPh(sFlowAssist.pumpIdx)
                                     ? manualInjectPh : manualInjectOrp;
    if (!s.active) sFlowAssist.runOwned = false;
  }

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
      // feature-075 (P4) : SAUF marche d'assistant — sous suspension flow-assist
      // l'intégration par tick est sautée, crédit majorant prorata obligatoire
      // (cf. flowAssistCreditAbortedRun ; sans effet hors assistant).
      flowAssistCreditAbortedRun(/*isPh=*/true, manualInjectPh, now);
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
      // feature-075 (P4) : SAUF marche d'assistant — crédit majorant prorata
      // (intégration par tick sautée sous suspension, cf. cas pH).
      flowAssistCreditAbortedRun(/*isPh=*/false, manualInjectOrp, now);
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
    // feature-075 : refus PRÉCOCE (avant toute mutation) pendant l'assistant
    // de mesure du débit — cf. flowAssistAllowsManualInjectOrReject (P4).
    if (!flowAssistAllowsManualInjectOrReject(req)) return;
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
    // feature-075 : refus PRÉCOCE (avant toute mutation) pendant l'assistant
    // de mesure du débit — cf. flowAssistAllowsManualInjectOrReject (P4).
    if (!flowAssistAllowsManualInjectOrReject(req)) return;
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
    // feature-146 (revue code-reviewer) : startBoost() a désormais DEUX causes
    // de refus (heure non synchronisée, mode Arrêt) mais ne distingue pas
    // laquelle à l'appelant. On teste la cause mode AVANT l'appel pour renvoyer
    // le motif réel — sinon un refus "mode Arrêt" renvoyait le message
    // "horloge non synchronisée", mensonger. Même condition que startBoost()
    // (config.cpp) : ManagedFiltration uniquement, le mode est INERTE ailleurs.
    if (mqttCfg.installMode == InstallMode::ManagedFiltration &&
        parseFiltrationModeFrom(filtrationCfg.mode) == FiltrationMode::Arret) {
      req->send(409, "application/json",
                "{\"error\":\"filtration_stopped\",\"message\":\"Boost impossible : filtration à l'arrêt (mode Arrêt).\"}");
      return;
    }
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
    // feature-140 : verdict DÉRIVÉ sur le mode EFFECTIF (pas la NVS brute) —
    // même motif que ws_manager.cpp:883 (boost_chlorine_boosted).
    const bool filtrationExtended = (mqttCfg.installMode == InstallMode::ManagedFiltration);  // feature-056
    const bool chlorineBoosted = (PumpController.effectiveOrpMode() == OrpMode::Automatic);
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

  // feature-140 : Chloration de secours (surcouche temporaire du mode
  // `scheduled`, 48h glissantes). Décalque du Boost ci-dessus : startRescueChlorine
  // prend configMutex en interne (saveRescueState), REFUSE si l'heure n'est pas
  // synchronisée (échéance 48h indéterminable, condition #6 du gate) ET si la
  // régulation ORP (NVS) est en mode Manuel (condition #5 : double dosage).
  // Le refus Manuel est vérifié AVANT l'appel (startRescueChlorine le refuse
  // aussi en interne, mais sans distinguer la cause pour cette réponse HTTP).
  server->on("/rescue/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    if (mqttCfg.orpRegulationMode == "manual") {
      // L8 — libellé IMPOSÉ par le gate pool-chemistry (feature-140), repris
      // MOT À MOT : ne pas reformuler sans nouveau passage du gate.
      req->send(409, "application/json",
                "{\"error\":\"mode_manual\",\"message\":\"Chloration de secours indisponible : "
                "la régulation du chlore est en mode Manuel. Repassez en Automatique ou "
                "Programmée pour l'utiliser.\"}");
      return;
    }
    // feature-141 : volume quotidien SAISI à l'activation (`ml`, entier mL).
    // Absent ou non numérique ⇒ 0.0f (= non fourni) : startRescueChlorine()
    // résout alors via la chaîne de pré-remplissage. AUCUN rejet 4xx pour une
    // valeur hors bornes — le CLAMP SERVEUR (condition bloquante du gate) la
    // RAMÈNE dans les bornes, il ne refuse jamais une saisie.
    float requestedMl = 0.0f;
    if (req->hasParam("ml")) {
      requestedMl = req->getParam("ml")->value().toFloat();
    }
    startRescueChlorine(requestedMl);
    if (!rescueState.active) {
      // Refus fail-closed : heure non synchronisée (échéance 48h indéterminable).
      req->send(409, "application/json",
                "{\"error\":\"time_not_synced\",\"message\":\"Chloration de secours impossible : "
                "horloge non synchronisée (échéance 48h indéterminable).\"}");
      return;
    }
    mqttManager.publishRescueState();   // reflète immédiatement l'état sur HA
    wsManager.requestConfigBroadcast(); // resync UI web (pattern bug-sync)
    String out = String("{\"rescue_active\":true,\"rescue_until\":") +
                 String((long)rescueState.untilEpoch) +
                 ",\"rescue_ml\":" + String((long)lroundf(rescueState.dailyMl)) + "}";
    req->send(200, "application/json", out);
  });

  server->on("/rescue/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    stopRescueChlorine(RescueLiftReason::Manual);  // arrêt manuel (bouton UI)
    mqttManager.publishRescueState();
    wsManager.requestConfigBroadcast();
    req->send(200, "application/json", "{\"rescue_active\":false,\"rescue_until\":0}");
  });

  // ---------- POST /reset-sensor-filter?sensor=ph|orp ----------
  // v2.20.3 (D1, bug-filtre-ph-verrouille) : réinitialisation manuelle du filtre
  // pH ou ORP, hors des routes de debug. Sortie de secours utilisateur quand le
  // filtre reste verrouillé (incident 2026-08-16 : la SEULE porte de sortie était
  // POST /debug/sensor_filter_reset, absente de l'interface, et elle sacrifiait
  // aussi le filtre ORP sain).
  //
  // ⚠️ ÉTAT APRÈS feature-152 (relevé en revue de code, 2026-10-11) — À TRANCHER :
  // cette route est doublement orpheline. (1) Plus AUCUN appelant dans data/ : le
  // bouton « Réarmer le filtre » a été retiré avec la refonte. (2) Sa raison
  // d'être a disparu : elle existait parce qu'un filtre pouvait rester VERROUILLÉ
  // (incident 2026-08-16) — or la chaîne ne compte plus aucun verrou collant
  // (2 → 0), et toute cause de blocage se résorbe désormais d'elle-même.
  // Elle est CONSERVÉE en l'état, délibérément et sans la rebrancher : c'est une
  // route publique sous auth administrateur, la retirer est un changement de
  // contrat d'API qui appartient à une décision produit, pas à ce lot. Elle garde
  // un usage légitime en diagnostic (curl) : vider la fenêtre et la détection de
  // sonde figée sans redémarrer l'appareil.
  //
  // Gardes (verdict pool-chemistry P6) :
  //   - auth ADMIN (RouteProtection::CRITICAL) : un reset relance le remplissage
  //     de la fenêtre et efface la détection de sonde figée (le « latch EMI »
  //     qu'évoquait cette ligne n'existe plus — feature-152) ;
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

  // ==========================================================================
  // feature-075 : assistant de mesure du débit des pompes doseuses
  // ==========================================================================
  // POST /flow-assist/start?pump=1|2&phase=purge|measure (corps OU query)
  // Ouvre/rejoint la session (une seule pompe à la fois), suspend le dosage
  // automatique+programmé (setFlowAssistSuspend), et lance une marche MINUTÉE
  // SERVEUR par le chemin manualInject* existant. Duty et durée FIXÉS SERVEUR,
  // jamais acceptés du client (interdit brief) :
  //   - purge  : duty 255 / kFlowAssistPurgeDurationS (eau claire → circuit normal) ;
  //   - mesure : amplitude d'exploitation / kFlowAssistMeasureDurationS.
  // TOUTES les gardes d'injection manuelle s'appliquent (manualInjectGuardOrReject :
  // watchdog, présence d'eau, stabilisation, double start, limites journalière/
  // horaire, cycles/jour, anti-rafale) avec le volume MAJORANT flow255 × durée —
  // la marche est CRÉDITÉE au registre de sécurité (condition gate 6) par le
  // crédit-plancher à échéance (creditManualInjectionFloor), seul mécanisme
  // actif sous suspension (cf. flowAssistStopRun pour la démonstration).
  server->on("/flow-assist/start", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);

    // --- Paramètres (corps form-urlencoded, fallback query) ---
    auto* pPump = req->getParam("pump", true);
    if (!pPump) pPump = req->getParam("pump");
    auto* pPhase = req->getParam("phase", true);
    if (!pPhase) pPhase = req->getParam("phase");
    const int pumpNum = pPump ? pPump->value().toInt() : 0;
    String phaseStr = pPhase ? pPhase->value() : String();
    phaseStr.toLowerCase();
    const bool isPurge = (phaseStr == "purge");
    if ((pumpNum != 1 && pumpNum != 2) || (!isPurge && phaseStr != "measure")) {
      req->send(400, "application/json",
                "{\"error\":\"invalid_param\",\"message\":\"Paramètres requis : pump=1|2 et phase=purge|measure.\"}");
      return;
    }
    const int pumpIdx = pumpNum - 1;
    const FlowAssistPhase phase = isPurge ? FlowAssistPhase::kPurge
                                          : FlowAssistPhase::kMeasure;

    // --- Verrou « une pompe à la fois » ---
    if (sFlowAssist.active && sFlowAssist.pumpIdx != pumpIdx) {
      req->send(409, "application/json",
                "{\"error\":\"assist_busy\",\"message\":\"Un assistant de mesure est déjà en cours sur l'autre pompe.\"}");
      return;
    }
    // Toute requête de l'assistant réarme le timeout de session — y compris un
    // start REFUSÉ plus bas (l'utilisateur est manifestement devant l'écran).
    if (sFlowAssist.active) sFlowAssist.lastTouchMs = (uint32_t)millis();

    // --- Verdict pur (quota de purges, marche déjà en cours) ---
    const bool isPh = flowAssistPumpIsPh(pumpIdx);
    ManualInjectState& mi = isPh ? manualInjectPh : manualInjectOrp;
    const uint8_t purgesDone = sFlowAssist.active ? sFlowAssist.purgesDone : 0;
    FlowAssistRefusal verdict = canStartFlowAssistPhase(
        phase, purgesDone, kFlowAssistMaxPurgesPerSession,
        /*sessionActive=*/true /* start ouvre la session */, mi.active);
    if (verdict == FlowAssistRefusal::kPumpBusy) {
      req->send(409, "application/json",
                "{\"error\":\"already_injecting\",\"message\":\"Une marche de pompe est déjà en cours — l'arrêter avant d'en relancer une.\"}");
      return;
    }
    if (verdict == FlowAssistRefusal::kPurgeQuota) {
      req->send(409, "application/json",
                "{\"error\":\"purge_limit\",\"message\":\"Quota de purges de la session atteint (3) — passez à la suite ou quittez l'assistant.\"}");
      return;
    }

    // --- Duty/durée FIXÉS SERVEUR ---
    uint8_t duty;
    uint32_t durS;
    if (isPurge) {
      duty = MAX_PWM_DUTY;                      // 255 : seul débit connu hors calibration
      durS = kFlowAssistPurgeDurationS;
    } else {
      duty = PumpController.getPumpAmplitudeDuty(pumpIdx);  // amplitude EXACTE d'exploitation
      durS = kFlowAssistMeasureDurationS;
      if (duty == 0) {
        req->send(409, "application/json",
                  "{\"error\":\"invalid_amplitude\",\"message\":\"Amplitude d'exploitation nulle — mesure impossible.\"}");
        return;
      }
    }

    // --- Crédit de sécurité au MAJORANT (P4 : jamais zéro sur pompe alimentée).
    // flow255 majore le débit réel à tout duty ≤ 255 : le registre journalier ne
    // sous-compte jamais, et la garde daily_limit évalue ce majorant (le coût
    // budget est annoncé à l'utilisateur en étape 1 de l'assistant). ---
    const float creditMl = kPumpFlow255MlPerMin * (float)durS / 60.0f;

    // --- Gardes COMPLÈTES du chemin manuel (présence d'eau incluse) ---
    if (!manualInjectGuardOrReject(req, isPh, creditMl, (int)durS)) return;

    // --- Acceptée : cycle manuel comptabilisé, suspension posée, marche armée ---
    PumpController.requestManualCycleRecord(isPh ? 0 : 1);
    PumpController.setFlowAssistSuspend(true);
    PumpController.setManualPump(pumpIdx, duty);
    mi.active = true;
    mi.startMs = millis();
    mi.durationMs = (unsigned long)durS * 1000UL;
    mi.requestedVolumeMl = creditMl;
    // Crédit plancher à échéance (patron /ph|orp/inject/start) : cumul au départ
    // + volume promis, consommés à la fin NATURELLE en loopTask. Sous suspension
    // c'est LE mécanisme de crédit (comptabilité par tick sautée).
    mi.startCumulMl = isPh ? safetyLimits.dailyPhInjectedMl
                           : safetyLimits.dailyOrpInjectedMl;
    mi.creditMl = creditMl;

    if (!sFlowAssist.active) {
      sFlowAssist.active = true;
      sFlowAssist.pumpIdx = pumpIdx;
      sFlowAssist.purgesDone = 0;
      systemLogger.info("[Assistant débit] Session ouverte (pompe " + String(pumpNum) +
                        ") — dosage automatique suspendu");
    }
    if (isPurge) sFlowAssist.purgesDone++;
    sFlowAssist.phase = phase;
    sFlowAssist.runOwned = true;
    sFlowAssist.aborted = false;  // réarmement : nouvelle marche, ancien signalement caduc
    sFlowAssist.lastTouchMs = (uint32_t)millis();

    const int purgesLeft = (sFlowAssist.purgesDone >= kFlowAssistMaxPurgesPerSession)
                               ? 0 : (int)(kFlowAssistMaxPurgesPerSession - sFlowAssist.purgesDone);
    systemLogger.info(String("[Assistant débit] ") + (isPurge ? "Purge" : "Mesure") +
                      " démarrée : pompe " + String(pumpNum) + ", " + String(durS) +
                      " s, duty " + String(duty) + ", crédit majorant " +
                      String(creditMl, 1) + " mL");
    req->send(200, "application/json",
              String("{\"duration_s\":") + String(durS) + ",\"phase\":\"" +
              (isPurge ? "purge" : "measure") + "\",\"purges_left\":" +
              String(purgesLeft) + "}");
  });

  // POST /flow-assist/stop : arrêt anticipé de la marche en cours (la SESSION
  // est conservée — suspension maintenue, l'utilisateur reste dans l'assistant).
  // Crédit prorata au majorant via flowAssistStopRun (P4, cf. commentaire).
  server->on("/flow-assist/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    bool stopped = false;
    if (sFlowAssist.active) {
      sFlowAssist.lastTouchMs = (uint32_t)millis();
      stopped = flowAssistStopRun();
      if (stopped) sFlowAssist.runOwned = false;  // marche close (crédit déjà armé)
    }
    int purgesLeft = (int)kFlowAssistMaxPurgesPerSession;
    if (sFlowAssist.active) {
      purgesLeft = (sFlowAssist.purgesDone >= kFlowAssistMaxPurgesPerSession)
                       ? 0 : (int)(kFlowAssistMaxPurgesPerSession - sFlowAssist.purgesDone);
    }
    req->send(200, "application/json",
              String("{\"stopped\":") + (stopped ? "true" : "false") +
              ",\"purges_left\":" + String(purgesLeft) + "}");
  });

  // POST /flow-assist/exit : arrêt de la marche éventuelle + LEVÉE de la
  // suspension + fermeture de la session (sortie normale, Annuler, Terminer).
  // Idempotent : sans session, ne fait que confirmer le rétablissement.
  server->on("/flow-assist/exit", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    if (sFlowAssist.active) {
      flowAssistStopRun();  // crédit prorata armé, consommé en loopTask
      systemLogger.info("[Assistant débit] Session fermée (pompe " +
                        String(sFlowAssist.pumpIdx + 1) +
                        ") — dosage automatique rétabli");
    }
    // INVARIANT (re-revue) : session fermée AVANT la levée de la suspension —
    // la réconciliation fail-closed de updateManualInject (session active sans
    // suspension → re-suspend) s'exécute en loopTask (autre cœur) : dans
    // l'ordre inverse, elle pourrait s'intercaler entre la levée et le reset,
    // re-poser la suspension, puis le reset effacerait la session → suspension
    // collée définitivement (le timeout exige active, plus rien ne la lève).
    sFlowAssist = FlowAssistSession{};
    PumpController.setFlowAssistSuspend(false);
    req->send(200, "application/json", "{\"exited\":true}");
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
