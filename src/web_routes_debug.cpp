#include "web_routes_debug.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>

#include "config.h"   // i2cMutex
#include "constants.h"
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

  // ---------- POST /debug/ezo_command ----------
  // Envoie une commande Atlas EZO arbitraire et retourne la réponse parsée.
  // Body JSON : {"addr": 98, "cmd": "Status", "delay_ms": 900}
  //   addr     : 8..119 (décimal, 0x08..0x77)
  //   cmd      : commande ASCII (max 30 caractères, ex. "I", "Status", "R", "Slope,?")
  //   delay_ms : optionnel, défaut 900 (R/Cal/I), 600 pour les commandes courtes
  // Réponse : {"success", "addr", "cmd", "status_code", "response", "raw_hex"}
  //   status_code : 1=succès, 2=erreur syntaxe, 254=pas prêt, 255=pas de data, 0=timeout
  server->on("/debug/ezo_command", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t /*index*/, size_t /*total*/) {
      JsonDocument body;
      DeserializationError perr = deserializeJson(body, data, len);
      if (perr) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
      }
      int addrInt = body["addr"] | 0;
      const char* cmd = body["cmd"] | "";
      uint32_t delayMs = body["delay_ms"] | 900u;

      if (addrInt < 0x08 || addrInt > 0x77) {
        req->send(400, "application/json", "{\"error\":\"addr must be 8..119\"}");
        return;
      }
      size_t cmdLen = strlen(cmd);
      if (cmdLen == 0 || cmdLen > 30) {
        req->send(400, "application/json", "{\"error\":\"cmd must be 1..30 chars\"}");
        return;
      }
      if (delayMs < 50 || delayMs > 5000) {
        req->send(400, "application/json", "{\"error\":\"delay_ms must be 50..5000\"}");
        return;
      }
      uint8_t addr = static_cast<uint8_t>(addrInt);

      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
        req->send(503, "application/json", "{\"error\":\"i2c bus busy\"}");
        return;
      }

      // 1) Envoi de la commande
      Wire.beginTransmission(addr);
      Wire.write(reinterpret_cast<const uint8_t*>(cmd), cmdLen);
      uint8_t txErr = Wire.endTransmission();

      if (txErr != 0) {
        xSemaphoreGive(i2cMutex);
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"i2c tx failed err=%u (no device at 0x%02X?)\"}",
                 (unsigned)txErr, addr);
        req->send(500, "application/json", msg);
        return;
      }

      // 2) Délai obligatoire avant lecture
      delay(delayMs);

      // 3) Lecture de la réponse
      static constexpr size_t kMaxResp = 32;
      uint8_t rawBytes[kMaxResp] = {0};
      size_t received = Wire.requestFrom(static_cast<int>(addr),
                                         static_cast<int>(kMaxResp));
      size_t rawCount = 0;
      uint8_t statusCode = 0;
      char respText[kMaxResp + 1] = {0};
      size_t textIdx = 0;

      if (received > 0 && Wire.available()) {
        statusCode = Wire.read();
        rawBytes[rawCount++] = statusCode;
        while (Wire.available() && rawCount < kMaxResp) {
          uint8_t b = Wire.read();
          rawBytes[rawCount++] = b;
          if (b == 0) break;  // null terminator Atlas
          if (textIdx < kMaxResp) respText[textIdx++] = static_cast<char>(b);
        }
        // Drain
        while (Wire.available()) (void)Wire.read();
      }
      respText[textIdx] = '\0';

      xSemaphoreGive(i2cMutex);

      // 4) Construire la réponse JSON
      JsonDocument doc;
      doc["success"] = true;
      char addrHex[8];
      snprintf(addrHex, sizeof(addrHex), "0x%02X", addr);
      doc["addr"] = addrHex;
      doc["cmd"] = cmd;
      doc["delay_ms"] = delayMs;
      doc["status_code"] = statusCode;
      const char* statusLabel = "unknown";
      switch (statusCode) {
        case 1:   statusLabel = "success"; break;
        case 2:   statusLabel = "syntax error"; break;
        case 254: statusLabel = "not ready"; break;
        case 255: statusLabel = "no data"; break;
        case 0:   statusLabel = "no response"; break;
      }
      doc["status_label"] = statusLabel;
      doc["response"] = respText;
      JsonArray hex = doc["raw_hex"].to<JsonArray>();
      for (size_t i = 0; i < rawCount; ++i) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", rawBytes[i]);
        hex.add(b);
      }
      String out;
      serializeJson(doc, out);

      systemLogger.info(String("[Debug EZO] ") + addrHex + " cmd=\"" + cmd +
                        "\" status=" + statusCode + " resp=\"" + respText + "\"");
      req->send(200, "application/json", out);
    });

  // ---------- POST /debug/ezo_factory?addr=<N> ----------
  // Envoie la commande "Factory" à un module Atlas EZO sur le bus I²C ISO.
  // Restaure les paramètres par défaut du module (calibration, adresse I²C,
  // baud rate UART, compensation T°). Le mode de communication (I²C vs UART)
  // est PRÉSERVÉ. Le firmware EZO n'est pas touché.
  //
  // Usage : curl -X POST "http://<host>/debug/ezo_factory?addr=98"
  // (98 décimal = 0x62 hexadécimal = adresse ORP par défaut)
  // (99 décimal = 0x63 = adresse pH par défaut)
  //
  // Après l'appel : couper l'alim ESP32, rallumer, recalibrer le module.
  server->on("/debug/ezo_factory", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("addr")) {
      req->send(400, "application/json", "{\"error\":\"missing 'addr' query param (decimal)\"}");
      return;
    }
    int addrInt = req->getParam("addr")->value().toInt();
    if (addrInt < 0x08 || addrInt > 0x77) {
      req->send(400, "application/json", "{\"error\":\"addr must be 8..119 (0x08..0x77)\"}");
      return;
    }
    uint8_t addr = static_cast<uint8_t>(addrInt);

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
      req->send(503, "application/json", "{\"error\":\"i2c bus busy\"}");
      return;
    }

    Wire.beginTransmission(addr);
    Wire.write(reinterpret_cast<const uint8_t*>("Factory"), 7);
    uint8_t err = Wire.endTransmission();

    xSemaphoreGive(i2cMutex);

    if (err != 0) {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "{\"error\":\"i2c transmission failed err=%u (no device at 0x%02X?)\"}",
               (unsigned)err, addr);
      req->send(500, "application/json", msg);
      return;
    }

    systemLogger.warning(String("[Debug] Commande Factory envoyée à 0x") +
                         String(addr, HEX) + " — couper/rallumer l'alim puis recalibrer");
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"addr\":\"0x%02X\",\"note\":\"power-cycle ESP32 then recalibrate\"}",
             addr);
    req->send(200, "application/json", resp);
  });
}
