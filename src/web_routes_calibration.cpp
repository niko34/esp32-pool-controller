#include "web_routes_calibration.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "sensors.h"
#include "json_compat.h"
#include "uart_protocol.h"

// =============================================================================
// Routes de calibration capteurs Atlas EZO (feature-021 — Pass 4a)
// =============================================================================
// Toutes les routes répondent immédiatement (< 1 ms) en plaçant la commande
// dans la queue FreeRTOS de SensorManager. La commande est exécutée par
// loopTask via _processEzoQueue() en ~900 ms (transaction I²C bloquante).
// L'UI observe l'avancement via les champs WS phCalPoints / orpCalPoints
// rafraîchis à chaque cycle (cf. ws_manager.cpp).
// =============================================================================

namespace {

// Diffuse l'event de fin de calibration sur l'écran LVGL si activé.
// Note : l'event est envoyé immédiatement à la mise en queue (ack rapide), pas à
// la fin de l'exécution I²C. L'écran observe la transition via les champs
// phCalPoints / orpCalPoints publiés en parallèle.
void notifyScreenCalibrationQueued(const char* sensor) {
  if (authCfg.screenEnabled) {
    uartProtocol.sendEventStr("event", "calibration_queued", "sensor", sensor);
  }
}

}  // namespace

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
          if (ok) notifyScreenCalibrationQueued("ph_mid");
        } else if (strcmp(step, "low") == 0) {
          ok = sensors.enqueueCalibratePhLow();
          if (ok) notifyScreenCalibrationQueued("ph_low");
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
        notifyScreenCalibrationQueued("orp");

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
