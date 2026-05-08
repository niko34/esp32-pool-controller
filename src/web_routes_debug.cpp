#include "web_routes_debug.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "logger.h"
#include "sensors.h"

namespace {

// État interne de la pause WiFi.
// 0 = pas de pause active. Sinon = millis() de fin programmée.
// Lecture/écriture par loopTask uniquement (handler async pose un flag pendingPauseSec).
volatile uint32_t g_wifiPauseEndMs = 0;
// Demande de pause posée par le handler async, traitée par updateWifiPauseLoop().
volatile uint32_t g_pendingPauseSec = 0;
// Vrai si on a déjà coupé le WiFi pendant la pause active (évite double WIFI_OFF).
volatile bool g_wifiCutInProgress = false;

constexpr uint32_t kMaxPauseSeconds = 120;
constexpr uint32_t kMinPauseSeconds = 1;

}  // namespace

// =============================================================================
// updateWifiPauseLoop() — appelée depuis loopTask
// =============================================================================

void updateWifiPauseLoop() {
  uint32_t now = millis();

  // 1) Une demande de pause est-elle en attente ?
  if (g_pendingPauseSec > 0 && g_wifiPauseEndMs == 0) {
    uint32_t sec = g_pendingPauseSec;
    g_pendingPauseSec = 0;
    g_wifiPauseEndMs = now + sec * 1000UL;
    systemLogger.warning(String("[Debug] Coupure WiFi pour ") + sec + " s — debug oscillation pH");
    WiFi.disconnect(false, false);  // déconnecte sans effacer la config
    WiFi.mode(WIFI_OFF);
    g_wifiCutInProgress = true;
    return;
  }

  // 2) Pause active : surveille l'expiration
  if (g_wifiPauseEndMs != 0 && now >= g_wifiPauseEndMs) {
    g_wifiPauseEndMs = 0;
    g_wifiCutInProgress = false;
    systemLogger.info("[Debug] Reconnexion WiFi après pause debug");
    WiFi.mode(WIFI_STA);
    WiFi.begin();  // utilise la config NVS existante
  }
}

// =============================================================================
// Endpoints HTTP
// =============================================================================

void setupDebugRoutes(AsyncWebServer* server) {
  if (server == nullptr) return;

  // ---------- GET /debug/ph_trace ----------
  server->on("/debug/ph_trace", HTTP_GET, [](AsyncWebServerRequest* req) {
    // ~300 samples × 4 champs ≈ 12 KB JSON max — taille raisonnable
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    JsonDocument doc;
    doc["count"]     = sensors.getPhDebugSampleCount();
    doc["interval_ms"] = (uint32_t)kPhOrpSensorIntervalMs;
    doc["now"]       = (uint32_t)millis();
    JsonArray arr = doc["samples"].to<JsonArray>();
    sensors.getPhDebugSamplesJson(arr);
    serializeJson(doc, *res);
    req->send(res);
  });

  // ---------- POST /debug/ph_trace_clear ----------
  server->on("/debug/ph_trace_clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    sensors.clearPhDebugBuffer();
    systemLogger.info("[Debug] Trace pH vidée");
    req->send(200, "application/json", "{\"success\":true}");
  });

  // ---------- POST /debug/wifi_pause ----------
  // Body JSON : {"seconds": <1..120>}
  server->on("/debug/wifi_pause", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t /*index*/, size_t /*total*/) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
      }
      uint32_t sec = doc["seconds"] | 0u;
      if (sec < kMinPauseSeconds || sec > kMaxPauseSeconds) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"seconds must be %u..%u\"}",
                 (unsigned)kMinPauseSeconds, (unsigned)kMaxPauseSeconds);
        req->send(400, "application/json", msg);
        return;
      }
      if (g_wifiPauseEndMs != 0 || g_pendingPauseSec != 0) {
        req->send(409, "application/json", "{\"error\":\"pause already active\"}");
        return;
      }
      g_pendingPauseSec = sec;
      char resp[96];
      snprintf(resp, sizeof(resp),
               "{\"success\":true,\"queued\":true,\"seconds\":%u}", (unsigned)sec);
      req->send(200, "application/json", resp);
    });

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

  // ---------- GET /debug/wifi_pause ----------
  server->on("/debug/wifi_pause", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["active"] = (g_wifiPauseEndMs != 0);
    if (g_wifiPauseEndMs != 0) {
      uint32_t now = millis();
      doc["remaining_ms"] = (g_wifiPauseEndMs > now) ? (g_wifiPauseEndMs - now) : 0u;
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });
}
