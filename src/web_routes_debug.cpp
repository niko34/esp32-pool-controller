#include "web_routes_debug.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "logger.h"
#include "sensors.h"

// =============================================================================
// Endpoints HTTP
// =============================================================================

void setupDebugRoutes(AsyncWebServer* server) {
  if (server == nullptr) return;

  // ---------- POST /debug/ph_slope_refresh ----------
  // feature-024 : force une re-query Slope,? sur l'EZO pH (utile pour valider
  // une nouvelle calibration sans attendre le cycle 24h).
  // Réponse 200 si la commande a été enfilée, 503 si la queue est saturée.
  server->on("/debug/ph_slope_refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
    bool ok = sensors.enqueuePhSlopeQuery();
    if (ok) {
      req->send(200, "application/json", "{\"success\":true,\"queued\":true}");
    } else {
      req->send(503, "application/json", "{\"error\":\"queue full or already pending\"}");
    }
  });

  // ---------- POST /debug/sensor_filter_reset ----------
  // feature-025 : reset des filtres pH/ORP (médiane + EMA). Repasse les 2 filtres
  // en warmup → dosage auto bloqué jusqu'à kSensorFilterWarmupSamples mesures valides.
  // Utile après un débranchement EZO, un test, ou une recalibration manuelle.
  server->on("/debug/sensor_filter_reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    sensors.resetPhFilter();
    sensors.resetOrpFilter();
    systemLogger.info("[Debug] Filtres pH/ORP réinitialisés (warmup)");
    req->send(200, "application/json", "{\"success\":true,\"reset\":[\"ph\",\"orp\"]}");
  });

  // ---------- GET /debug/sensor_filter_state ----------
  // feature-025 : état brut JSON des filtres pH/ORP (diagnostic). Lecture lock-free
  // des getters SensorManager (contexte handler async — valeurs scalaires atomiques).
  server->on("/debug/sensor_filter_state", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonObject ph = doc["ph"].to<JsonObject>();
    float phRaw = sensors.getPhRaw();
    float phMed = sensors.getPhMedian();
    float phFil = sensors.getPhFiltered();
    if (!isnan(phRaw)) ph["raw"] = round(phRaw * 1000.0f) / 1000.0f; else ph["raw"] = nullptr;
    if (!isnan(phMed)) ph["median"] = round(phMed * 1000.0f) / 1000.0f; else ph["median"] = nullptr;
    if (!isnan(phFil)) ph["filtered"] = round(phFil * 1000.0f) / 1000.0f; else ph["filtered"] = nullptr;
    ph["ready"] = sensors.isPhFilterReady();
    ph["unstable"] = sensors.isPhFilterUnstable();
    ph["rejected"] = sensors.getPhRejectedCount();

    JsonObject orp = doc["orp"].to<JsonObject>();
    float orpRaw = sensors.getOrpRaw();
    float orpMed = sensors.getOrpMedian();
    float orpFil = sensors.getOrpFiltered();
    if (!isnan(orpRaw)) orp["raw"] = round(orpRaw); else orp["raw"] = nullptr;
    if (!isnan(orpMed)) orp["median"] = round(orpMed); else orp["median"] = nullptr;
    if (!isnan(orpFil)) orp["filtered"] = round(orpFil); else orp["filtered"] = nullptr;
    orp["ready"] = sensors.isOrpFilterReady();
    orp["unstable"] = sensors.isOrpFilterUnstable();
    orp["rejected"] = sensors.getOrpRejectedCount();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });
}
