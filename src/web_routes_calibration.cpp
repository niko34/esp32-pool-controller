#include "web_routes_calibration.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "sensors.h"
#include "json_compat.h"
#include "logger.h"

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
}
