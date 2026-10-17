#include "web_routes_calibration.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "sensors.h"
#include "json_compat.h"
#include "logger.h"
#include "pump_controller.h"   // bug-160 (volet 2) : interruption d'entrée en étalonnage
#include "web_routes_control.h"  // bug-160 : manualInjectPh/Orp (interruption à l'entrée, gate 5 C6)
#include "mqtt_manager.h"      // bug-160 : mqttManager.publishRescueState() (ORP → manual)
#include "cal_manual_switch_logic.h"  // bug-160 (volet 2, REFONTE) : décision PURE (ADR-0017)

// =============================================================================
// Routes de calibration capteurs Atlas EZO (feature-021 — Pass 4a)
// =============================================================================
// Toutes les routes répondent immédiatement (< 1 ms) en plaçant la commande
// dans la queue FreeRTOS de SensorManager. La commande est exécutée par
// loopTask via _processEzoQueue() en ~900 ms (transaction I²C bloquante).
// L'UI observe l'avancement via les champs WS phCalPoints / orpCalPoints
// rafraîchis à chaque cycle (cf. ws_manager.cpp).
// =============================================================================

// =============================================================================
// Garde d'authentification du CORPS des routes de calibration (v2.46.1)
// =============================================================================
// PIÈGE — identique à celui corrigé sur /save-config (cf. saveConfigBodyAuthorized()
// dans web_routes_config.cpp) : ESPAsyncWebServer exécute le handler de CORPS AVANT
// le lambda de requête qui porte REQUIRE_AUTH. Un relecteur qui voit la macro trois
// lignes au-dessus croit la route protégée, alors que le corps a déjà tout fait.
// La macro n'est donc PAS redondante avec cette garde, et cette garde ne remplace
// pas la macro : REQUIRE_AUTH produit le 401 propre, la garde empêche l'effet de bord.
//
// Ici l'effet de bord n'est pas une simple écriture : la commande part dans la queue
// FreeRTOS de SensorManager et loopTask l'exécute ~900 ms APRÈS le départ du 401. Un
// « Cal,clear » ainsi passé est IRRÉVERSIBLE (il n'existe pas de dé-calibration) et
// fausse la mesure pH/ORP dont dépend le dosage d'acide et de chlore : la conséquence
// est chimique, pas seulement un accès. D'où l'exigence : PREMIÈRE instruction du
// handler de corps, avant tout parse et avant tout enqueue.
//
// Écart assumé avec le site de référence : PAS d'exemption isFirstBootDetected().
// Aucune des trois routes n'appartient au flux de premier démarrage (l'interface les
// appelle via authFetch depuis la page réglages ; /calibrate_clear n'a aucun appelant
// d'interface) — l'exemption y serait un trou, pas une compatibilité.
//
// Prédicat volontairement identique à celui de checkAuth() moins le rate limiting :
// toute requête acceptée par REQUIRE_AUTH est acceptée ici, donc une calibration
// légitime passe. Pas de sendAuthRequired() ni de checkRateLimit() ici : une seule
// réponse au client, un seul décompte de quota par requête.
static bool calibrationBodyAuthorized(AsyncWebServerRequest* request) {
  if (!authManager.isEnabled()) return true;  // authentification désactivée
  return authManager.checkTokenAuth(request) || authManager.checkBasicAuth(request);
}

// IP d'origine, sans jamais déréférencer un client nul : le client peut avoir disparu
// entre l'arrivée du corps et la journalisation — un log ne doit pas faire tomber
// l'appareil.
static String clientIpOf(AsyncWebServerRequest* request) {
  if (request == nullptr || request->client() == nullptr) return "IP inconnue";
  return request->client()->remoteIP().toString();
}

// Journalisation THROTTLÉE des tentatives non authentifiées (1/min/route).
// Le quota kMaxRequestsPerMinute ne borne pas cet émetteur : checkRateLimit() est
// appelé par checkAuth() dans le handler de REQUÊTE, donc APRÈS le corps — le corps
// tourne pour chaque requête reçue, quota dépassé ou non. Cf. le motif détaillé de
// kCalibrationAuthWarnThrottleMs (constants.h).
// INVARIANT : quand le site est muet, AUCUNE String n'est construite — sortie avant
// toute concaténation. Ce chemin est atteignable sans authentification et s'exécute
// en RAM interne pendant une famine mémoire active : y allouer serait le vrai coût.
static void logUnauthorizedCalibrationBody(const char* route,
                                           AsyncWebServerRequest* request,
                                           unsigned long& lastWarnMs,
                                           uint16_t& suppressedCount) {
  const unsigned long now = millis();
  // lastWarnMs == 0 == « jamais journalisé » : sans ce cas, la première tentative
  // des 60 premières secondes de vie de l'appareil serait avalée. La comparaison
  // par soustraction non signée reste immunisée au repliement de millis() (49,7 j).
  if (lastWarnMs != 0 && (now - lastWarnMs) < kCalibrationAuthWarnThrottleMs) {
    if (suppressedCount < UINT16_MAX) suppressedCount++;
    return;  // MUET : zéro allocation
  }

  String msg = String("[Calibration] ") + route + " NON AUTHENTIFIÉ depuis " +
               clientIpOf(request) + " — corps ignoré (aucune commande EZO mise en file)";
  if (suppressedCount > 0) {
    msg += " ; et " + String(suppressedCount) + " tentatives non journalisées";
    suppressedCount = 0;
  }
  systemLogger.warning(msg);
  lastWarnMs = now;
}

void setupCalibrationRoutes(AsyncWebServer* server) {
  // ===========================================================================
  // POST /calibrate_ph — payload {"step": "mid" | "low"}
  // ===========================================================================
  // Met en file une commande Cal,mid,7.00 ou Cal,low,4.00 vers l'EZO pH.
  // Réponse 200 immédiate avec {success:true, queued:true, step}.
  // 400 si payload invalide, 503 si queue saturée.
  server->on(
      "/calibrate_ph", HTTP_POST,
      [](AsyncWebServerRequest* req) {
        REQUIRE_AUTH(req, RouteProtection::WRITE);
        // Body handler ci-dessous fait le travail. Si on arrive ici sans body
        // (Content-Length: 0), on retourne une 400.
        // Note : ESPAsyncWebServer appelle ce lambda APRÈS le body handler,
        // donc on doit gérer le cas où onBody n'a rien fait (req->_tempObject
        // non utilisé ici, on s'en remet au body handler).
      },
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        // Garde en PREMIÈRE instruction (cf. calibrationBodyAuthorized) : le
        // REQUIRE_AUTH visible juste au-dessus s'exécute APRÈS ce handler, la
        // commande EZO serait déjà en file quand le 401 part.
        if (!calibrationBodyAuthorized(req)) {
          // Statiques locales AU SITE : un throttle par route (cf. constants.h).
          static unsigned long lastWarnMs = 0;
          static uint16_t suppressedCount = 0;
          if (index == 0) {
            logUnauthorizedCalibrationBody("/calibrate_ph", req, lastWarnMs, suppressedCount);
          }
          return;
        }

        // Buffer simple : la payload attendue fait < 64 octets.
        if (index != 0 || len != total) {
          // On ne supporte pas le chunked ici (payload minuscule, pas de raison)
          sendErrorResponse(req, 400, "payload too large or chunked");
          return;
        }

        StaticJson<128> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          sendErrorResponse(req, 400, String("bad json: ") + err.c_str());
          return;
        }

        const char* step = doc["step"] | "";
        bool ok = false;
        if (strcmp(step, "mid") == 0) {
          ok = sensors.enqueueCalibratePhMid();
        } else if (strcmp(step, "low") == 0) {
          ok = sensors.enqueueCalibratePhLow();
        } else {
          sendErrorResponse(req, 400, "step must be 'mid' or 'low'");
          return;
        }

        if (!ok) {
          sendErrorResponse(req, 503, "calibration queue saturée — réessayer dans 1s");
          return;
        }

        StaticJson<128> resp;
        resp["success"] = true;
        resp["queued"] = true;
        resp["step"] = step;
        sendJsonResponse(req, resp);
      });

  // ===========================================================================
  // POST /calibrate_orp — payload {"reference": <mV float>}
  // ===========================================================================
  // Met en file une commande Cal,<ref> vers l'EZO ORP.
  // Plage acceptée : 0..1000 mV (couvre les standards usuels 225 / 470 / 650 et les kits 0 mV).
  server->on(
      "/calibrate_orp", HTTP_POST,
      [](AsyncWebServerRequest* req) {
        REQUIRE_AUTH(req, RouteProtection::WRITE);
      },
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        // Garde en PREMIÈRE instruction (cf. calibrationBodyAuthorized) : le
        // REQUIRE_AUTH visible juste au-dessus s'exécute APRÈS ce handler, la
        // commande EZO serait déjà en file quand le 401 part.
        if (!calibrationBodyAuthorized(req)) {
          // Statiques locales AU SITE : un throttle par route (cf. constants.h).
          static unsigned long lastWarnMs = 0;
          static uint16_t suppressedCount = 0;
          if (index == 0) {
            logUnauthorizedCalibrationBody("/calibrate_orp", req, lastWarnMs, suppressedCount);
          }
          return;
        }

        if (index != 0 || len != total) {
          sendErrorResponse(req, 400, "payload too large or chunked");
          return;
        }

        StaticJson<128> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          sendErrorResponse(req, 400, String("bad json: ") + err.c_str());
          return;
        }

        float referenceMv = doc["reference"] | NAN;
        if (isnan(referenceMv) || referenceMv < 0.0f || referenceMv > 1000.0f) {
          sendErrorResponse(req, 400, "reference must be 0..1000 mV");
          return;
        }

        bool ok = sensors.enqueueCalibrateOrp(referenceMv);
        if (!ok) {
          sendErrorResponse(req, 503, "calibration queue saturée — réessayer dans 1s");
          return;
        }

        StaticJson<128> resp;
        resp["success"] = true;
        resp["queued"] = true;
        resp["reference"] = referenceMv;
        sendJsonResponse(req, resp);
      });

  // ===========================================================================
  // POST /calibrate_clear — payload {"sensor": "ph" | "orp"}
  // ===========================================================================
  // Efface la calibration EZO (utile pour debug / recalibration complète).
  // Recommandé par pool-chemistry pour les tests Cal,clear.
  server->on(
      "/calibrate_clear", HTTP_POST,
      [](AsyncWebServerRequest* req) {
        REQUIRE_AUTH(req, RouteProtection::WRITE);
      },
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        // Garde en PREMIÈRE instruction (cf. calibrationBodyAuthorized) : le
        // REQUIRE_AUTH visible juste au-dessus s'exécute APRÈS ce handler, la
        // commande EZO serait déjà en file quand le 401 part.
        if (!calibrationBodyAuthorized(req)) {
          // Statiques locales AU SITE : un throttle par route (cf. constants.h).
          static unsigned long lastWarnMs = 0;
          static uint16_t suppressedCount = 0;
          if (index == 0) {
            logUnauthorizedCalibrationBody("/calibrate_clear", req, lastWarnMs, suppressedCount);
          }
          return;
        }

        if (index != 0 || len != total) {
          sendErrorResponse(req, 400, "payload too large or chunked");
          return;
        }

        StaticJson<128> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          sendErrorResponse(req, 400, String("bad json: ") + err.c_str());
          return;
        }

        const char* sensor = doc["sensor"] | "";
        bool ok = false;
        if (strcmp(sensor, "ph") == 0) {
          ok = sensors.enqueueClearPhCalibration();
        } else if (strcmp(sensor, "orp") == 0) {
          ok = sensors.enqueueClearOrpCalibration();
        } else {
          sendErrorResponse(req, 400, "sensor must be 'ph' or 'orp'");
          return;
        }

        if (!ok) {
          sendErrorResponse(req, 503, "calibration queue saturée — réessayer dans 1s");
          return;
        }

        StaticJson<128> resp;
        resp["success"] = true;
        resp["queued"] = true;
        resp["sensor"] = sensor;
        sendJsonResponse(req, resp);
      });

  // ===========================================================================
  // POST /calibration/enter — bug-160 (volet 2, REFONTE 2026-10-13)
  // ===========================================================================
  // Décision utilisateur : le mécanisme de bail auto-expirant (cal_hold_logic,
  // ABANDONNÉ) est remplacé par une bascule EXPLICITE de la régulation pH+ORP
  // en "manual" à l'ENTRÉE dans l'écran de calibration, restaurée par un GESTE
  // HUMAIN (POST /calibration/exit, bouton « Fermer ») — PAS auto-libérante.
  // Gate pool-chemistry GATE 5 : GO sous 8 conditions. « On échange un oubli
  // qu'on VOIT (mode Manuel affiché au tableau de bord) contre une panne
  // qu'on ne voit PAS (minuterie coincée, invisible). »
  //
  // C3 (gate 5) : ⛔ si une trace de sécurité phAutoManualEpoch/Reason
  // (feature-067, escalade de plancher acide) est DÉJÀ active, le pH n'est
  // PAS touché (déjà "manual" sous verrou de sécurité) — seul l'ORP bascule.
  // Aucun verrou équivalent n'existe côté ORP (vérifié le 2026-10-13 : zéro
  // résultat sur `grep -r "orp_am_\|orpAutoManual" src/`).
  //
  // C6 (gate 5, « le seul trou que le bail couvrait ») : interrompt
  // IMMÉDIATEMENT toute injection MANUELLE en cours (hors configMutex — même
  // patron que /ph|orp/inject/stop) + demande l'interruption one-shot d'une
  // session AUTOMATIQUE/PROGRAMMÉE déjà ouverte (PumpController, consommée au
  // tour suivant de update()).
  //
  // C7 : sous configMutex, persisté (saveMqttConfig()), journalisé avec
  // l'origine de la requête authentifiée (patron clientIpOf — ce projet n'a
  // pas de notion d'identité utilisateur au-delà de l'authentification).
  //
  // Idempotente : un second appel pendant une session déjà ouverte ne réécrit
  // PAS le mode antérieur mémorisé (sinon un second appel capturerait
  // "manual" lui-même comme mode à restaurer).
  server->on("/calibration/enter", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);

    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
      systemLogger.error("[Etalonnage] Timeout configMutex dans /calibration/enter");
      sendErrorResponse(req, 503, "configuration occupee, reessayer");
      return;
    }

    bool phSwitched = mqttCfg.phCalSwitchApplied;
    bool orpSwitched = mqttCfg.orpCalSwitchApplied;

    // feature-163 (gate pool-chemistry G1) : la GARDE DE DOSAGE s'ouvre
    // INCONDITIONNELLEMENT, y compris quand pH et ORP sont DÉJÀ en "manual" et
    // qu'aucune bascule de mode n'a lieu ci-dessous (calSwitchActive resterait
    // alors faux — c'est le trou trouvé par le gate). calSessionOpen est le
    // SEUL prédicat lu par evaluateDose/evaluateManualInject/
    // evaluateScheduledDose : aucun dosage ne doit passer tant qu'il est vrai.
    const bool calSessionWasOpen = mqttCfg.calSessionOpen;
    mqttCfg.calSessionOpen = true;
    if (!calSessionWasOpen) {
      systemLogger.info("[Etalonnage] Garde de dosage activée (session de "
                        "calibration ouverte) depuis " + clientIpOf(req));
    }

    if (!mqttCfg.calSwitchActive) {
      const bool phSafetyTraceActive = (mqttCfg.phAutoManualReason.length() > 0) ||
                                       (mqttCfg.phAutoManualEpoch != 0);
      const bool phAlreadyManual = (mqttCfg.phRegulationMode == "manual");
      const bool orpAlreadyManual = (mqttCfg.orpRegulationMode == "manual");
      const bool applyPh = calSwitchShouldApplyEnter(phSafetyTraceActive, phAlreadyManual);
      // ORP : aucun verrou de sécurité équivalent → seul le mode courant compte.
      const bool applyOrp = calSwitchShouldApplyEnter(false, orpAlreadyManual);

      // bug-160 (correctif 2026-10-13 v2) : si NI le pH NI l'ORP ne basculent
      // (les deux étaient déjà en "manual"), aucune session de bascule ne
      // s'ouvre — ⛔ sinon calSwitchActive=true armerait l'alerte d'oubli C4
      // (tickCalSwitchAlert) pour un état que l'utilisateur avait lui-même
      // choisi, et le bandeau/`/data` afficheraient une session « active »
      // qui n'a rien basculé.
      if (applyPh || applyOrp) {
        const time_t nowEpoch = time(nullptr);
        mqttCfg.calSwitchActive = true;
        mqttCfg.calSwitchEpoch = (nowEpoch >= kMinValidEpoch) ? (uint32_t)nowEpoch : 0;
        mqttCfg.calSwitchLastAlertEpoch = 0;
        mqttCfg.calSwitchAlertCount = 0;

        mqttCfg.phCalSwitchApplied = applyPh;
        if (applyPh) {
          mqttCfg.phCalSwitchPrevMode = mqttCfg.phRegulationMode;
          mqttCfg.phRegulationMode = "manual";
          mqttCfg.phEnabled = false;
        }

        mqttCfg.orpCalSwitchApplied = applyOrp;
        if (applyOrp) {
          mqttCfg.orpCalSwitchPrevMode = mqttCfg.orpRegulationMode;
          mqttCfg.orpRegulationMode = "manual";
          mqttCfg.orpEnabled = false;
          // feature-140 (condition #5 du gate pool-chemistry) : bascule en
          // Manuel pendant la chloration de secours => désarmement AUTOMATIQUE
          // (sinon double dosage : l'étalonnage ET le secours). Même patron que
          // /save-config (web_routes_config.cpp).
          if (isRescueChlorineActive(time(nullptr))) {
            stopRescueChlorine(RescueLiftReason::Manual);
            mqttManager.publishRescueState();
            systemLogger.warning("[Secours] Désarmée : entrée en étalonnage (bascule Manuel)");
          }
        }

        phSwitched = applyPh;
        orpSwitched = applyOrp;

        systemLogger.info("[Etalonnage] Entrée dans l'écran de calibration depuis " +
                          clientIpOf(req) + " : régulation pH " +
                          (applyPh ? "basculée en Manuel"
                                   : (phSafetyTraceActive
                                          ? "déjà verrouillée en Manuel (sécurité), inchangée"
                                          : "déjà en Manuel, inchangée")) +
                          " ; ORP " +
                          (applyOrp ? "basculée en Manuel" : "déjà en Manuel, inchangée"));
      } else {
        phSwitched = false;
        orpSwitched = false;
        systemLogger.info("[Etalonnage] Entrée dans l'écran de calibration depuis " +
                          clientIpOf(req) +
                          " : pH et ORP déjà en Manuel, aucune bascule");
      }
    }

    // feature-163 (gate G1) : SAUVEGARDE UNIQUE, inconditionnelle — persiste
    // calSessionOpen même quand ni pH ni ORP ne basculent (le cas qui a motivé
    // ce lot). Déplacée hors du bloc `if (applyPh || applyOrp)` ci-dessus (qui
    // ne sauvegardait auparavant QUE les champs de bascule de mode).
    saveMqttConfig();

    xSemaphoreGiveRecursive(configMutex);

    // C6 — interruption IMMÉDIATE de toute injection MANUELLE en cours. Hors
    // configMutex (manualInjectPh/Orp ne sont pas protégés par lui, même
    // patron que /ph|orp/inject/stop).
    if (manualInjectPh.active) {
      PumpController.setManualPump(mqttCfg.phPump - 1, 0);
      manualInjectPh.active = false;
      manualInjectPh.startCumulMl = 0.0f;
      manualInjectPh.creditMl = 0.0f;
      systemLogger.info("[Injection] pH interrompue (entrée en étalonnage)");
    }
    if (manualInjectOrp.active) {
      PumpController.setManualPump(mqttCfg.orpPump - 1, 0);
      manualInjectOrp.active = false;
      manualInjectOrp.startCumulMl = 0.0f;
      manualInjectOrp.creditMl = 0.0f;
      systemLogger.info("[Injection] ORP interrompue (entrée en étalonnage)");
    }
    // Interruption one-shot d'une session AUTOMATIQUE/PROGRAMMÉE déjà ouverte
    // (consommée au tour suivant de PumpController::update()).
    PumpController.requestCalibrationEntryInterrupt();

    StaticJson<128> resp;
    resp["success"] = true;
    resp["ph_switched"] = phSwitched;
    resp["orp_switched"] = orpSwitched;
    sendJsonResponse(req, resp);
  });

  // ===========================================================================
  // POST /calibration/exit — bug-160 (volet 2, REFONTE 2026-10-13)
  // ===========================================================================
  // Restaure le mode ANTÉRIEUR de chaque boucle (gate 5, C5) — ⛔ JAMAIS
  // "automatic" en dur : "scheduled" doit redevenir "scheduled". ⛔ N'acquitte
  // JAMAIS phAutoManualEpoch/Reason (gate 5, C2 — piège central) : si cette
  // trace de sécurité est active (apparue à l'entrée OU pendant la session),
  // le pH reste en Manuel et seul l'ORP est restauré.
  // Idempotente : appelée sans session ouverte (double clic, jamais ouverte),
  // répond 200 sans rien muter.
  // feature-163 (gate pool-chemistry G1) : referme la GARDE DE DOSAGE
  // (calSessionOpen) INCONDITIONNELLEMENT, y compris quand calSwitchActive est
  // resté faux (cas « pH+ORP déjà manual » à l'entrée, cf. /calibration/enter)
  // — calSessionOpen est le SEUL prédicat lu par les trois fonctions pures de
  // dosage, il doit donc se refermer peu importe si une bascule de mode a eu
  // lieu ou non.
  server->on("/calibration/exit", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);

    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
      systemLogger.error("[Etalonnage] Timeout configMutex dans /calibration/exit");
      sendErrorResponse(req, 503, "configuration occupee, reessayer");
      return;
    }

    const bool calSessionWasOpen = mqttCfg.calSessionOpen;
    mqttCfg.calSessionOpen = false;

    if (!mqttCfg.calSwitchActive) {
      // Trou du gate (G1) : calSwitchActive peut être resté faux pendant toute
      // la session (pH+ORP déjà "manual" à l'entrée) — calSessionOpen est
      // alors la SEULE trace à refermer et à persister ici.
      if (calSessionWasOpen) {
        saveMqttConfig();
        systemLogger.info("[Etalonnage] Garde de dosage désactivée (fermeture "
                          "de l'écran de calibration) depuis " + clientIpOf(req));
      }
      xSemaphoreGiveRecursive(configMutex);
      StaticJson<128> resp;
      resp["success"] = true;
      resp["restored"] = false;
      sendJsonResponse(req, resp);
      return;
    }

    const bool phSafetyTraceActiveNow = (mqttCfg.phAutoManualReason.length() > 0) ||
                                        (mqttCfg.phAutoManualEpoch != 0);
    const bool restorePh = calSwitchShouldRestore(mqttCfg.phCalSwitchApplied, phSafetyTraceActiveNow);
    // ORP : aucun verrou de sécurité équivalent → safetyTraceActiveNow=false.
    const bool restoreOrp = calSwitchShouldRestore(mqttCfg.orpCalSwitchApplied, false);

    if (restorePh) {
      mqttCfg.phRegulationMode = mqttCfg.phCalSwitchPrevMode;
      mqttCfg.phEnabled = (mqttCfg.phRegulationMode != "manual");
    }
    if (restoreOrp) {
      mqttCfg.orpRegulationMode = mqttCfg.orpCalSwitchPrevMode;
      mqttCfg.orpEnabled = (mqttCfg.orpRegulationMode != "manual");
    }

    systemLogger.info("[Etalonnage] Fermeture de l'écran de calibration depuis " +
                      clientIpOf(req) + " : régulation pH " +
                      (restorePh ? ("rétablie en " + mqttCfg.phRegulationMode)
                                 : String("laissée en Manuel (verrou de sécurité actif)")) +
                      " ; ORP " +
                      (restoreOrp ? ("rétablie en " + mqttCfg.orpRegulationMode)
                                  : String("laissée en Manuel")));

    mqttCfg.calSwitchActive = false;
    mqttCfg.phCalSwitchApplied = false;
    mqttCfg.phCalSwitchPrevMode = "";
    mqttCfg.orpCalSwitchApplied = false;
    mqttCfg.orpCalSwitchPrevMode = "";
    mqttCfg.calSwitchEpoch = 0;
    mqttCfg.calSwitchLastAlertEpoch = 0;
    mqttCfg.calSwitchAlertCount = 0;

    saveMqttConfig();

    StaticJson<128> resp;
    resp["success"] = true;
    resp["restored"] = true;
    resp["ph_mode"] = mqttCfg.phRegulationMode;
    resp["orp_mode"] = mqttCfg.orpRegulationMode;

    xSemaphoreGiveRecursive(configMutex);

    sendJsonResponse(req, resp);
  });
}
