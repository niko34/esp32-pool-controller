#include "web_routes_data.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include "constants.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include "web_routes_control.h"
#include "logger.h"
#include "history.h"
#include "json_compat.h"
#include <AsyncJson.h>
#include <time.h>

namespace {
bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}
}  // namespace

static void handleGetData(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);
  // Buffer statique pour éviter la fragmentation du heap
  // Taille estimée : ~13 champs × 30 bytes + overhead = 512 bytes
  StaticJson<768> doc;

  // ORP
  if (!isnan(sensors.getOrp())) {
    doc["orp"] = sensors.getOrp();
  } else {
    doc["orp"] = nullptr;
  }

  // pH
  if (!isnan(sensors.getPh())) {
    doc["ph"] = round(sensors.getPh() * 10.0f) / 10.0f;
  } else {
    doc["ph"] = nullptr;
  }

  // ORP raw
  if (!isnan(sensors.getRawOrp())) {
    doc["orp_raw"] = sensors.getRawOrp();
  } else {
    doc["orp_raw"] = nullptr;
  }

  // pH raw
  if (!isnan(sensors.getRawPh())) {
    doc["ph_raw"] = round(sensors.getRawPh() * 10.0f) / 10.0f;
  } else {
    doc["ph_raw"] = nullptr;
  }

  // Tension brute pH en mV (utile pendant calibration, avant toute correction)
  if (!isnan(sensors.getPhVoltageMv())) {
    doc["ph_voltage_mv"] = round(sensors.getPhVoltageMv() * 10.0f) / 10.0f;
  } else {
    doc["ph_voltage_mv"] = nullptr;
  }

  // Température
  if (!isnan(sensors.getTemperature())) {
    doc["temperature"] = sensors.getTemperature();
  } else {
    doc["temperature"] = nullptr;
  }

  // Température brute (sans calibration)
  if (!isnan(sensors.getRawTemperature())) {
    doc["temperature_raw"] = sensors.getRawTemperature();
  } else {
    doc["temperature_raw"] = nullptr;
  }

  doc["filtration_running"] = filtration.isRunning();
  doc["ph_dosing"] = PumpController.isPhDosing();
  doc["orp_dosing"] = PumpController.isOrpDosing();
  doc["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  doc["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  doc["ph_limit_reached"] = safetyLimits.phLimitReached;
  doc["orp_limit_reached"] = safetyLimits.orpLimitReached;
  doc["ph_inject_remaining_s"]  = manualInjectRemainingS(manualInjectPh);
  doc["orp_inject_remaining_s"] = manualInjectRemainingS(manualInjectOrp);

  doc["ph_tracking_enabled"]  = productCfg.phTrackingEnabled;
  doc["ph_remaining_ml"]      = max(0.0f, productCfg.phContainerVolumeMl  - productCfg.phTotalInjectedMl);
  doc["ph_alert_threshold_ml"]= productCfg.phAlertThresholdMl;
  doc["orp_tracking_enabled"] = productCfg.orpTrackingEnabled;
  doc["orp_remaining_ml"]     = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  doc["orp_alert_threshold_ml"]= productCfg.orpAlertThresholdMl;

  time_t nowEpoch = time(nullptr);
  doc["time_synced"] = isTimeValid(nowEpoch);

  sendJsonResponse(request, doc);
}

static void handleDownloadLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  auto logs = systemLogger.getRecentLogs(kMaxLogEntries);

  String output;
  output.reserve(logs.size() * 100 + 1024);

  // Calculer l'epoch de démarrage pour reconstruire les timestamps absolus
  time_t nowEpoch = time(nullptr);
  unsigned long nowMs = millis();
  bool hasAbsoluteTime = (nowEpoch > 1609459200L);
  long bootEpoch = hasAbsoluteTime ? (long)(nowEpoch - nowMs / 1000) : 0;

  // En-tête
  output += "# Pool Controller — Journal système\n";
  if (hasAbsoluteTime) {
    char exportTime[24];
    struct tm t;
    localtime_r(&nowEpoch, &t);
    strftime(exportTime, sizeof(exportTime), "%Y-%m-%d %H:%M:%S", &t);
    output += "# Exporté le : ";
    output += exportTime;
  } else {
    output += "# Exporté le boot+";
    output += String(nowMs / 1000);
    output += "s (heure non synchronisée)";
  }
  output += "\n# Format: [YYYY-MM-DD HH:MM:SS] NIVEAU : message\n";

  // ---- Logs persistants (boots précédents) ----
  fs::FS* pfs = systemLogger.getPersistenceFs();
  if (pfs) {
    File f = pfs->open("/system.log", "r");
    if (f && f.size() > 0) {
      output += "\n# === Historique persistant ===\n";
      while (f.available()) {
        output += f.readStringUntil('\n');
        output += "\n";
        // Éviter de dépasser la RAM : flush par blocs de 8KB
        if (output.length() > 8192) {
          // On continue d'accumuler — la String gère la réallocation
          // Sur ESP32 avec 300KB+ de heap libre c'est acceptable
        }
      }
      f.close();
      output += "# === Fin historique persistant ===\n";
    }
  }

  // ---- Logs RAM (session courante) ----
  output += "\n# === Session courante ===\n";
  for (const auto& entry : logs) {
    char timeBuf[24];
    if (hasAbsoluteTime) {
      time_t entryEpoch = bootEpoch + (long)(entry.timestamp / 1000);
      struct tm t;
      localtime_r(&entryEpoch, &t);
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &t);
    } else {
      unsigned long totalSec = entry.timestamp / 1000;
      snprintf(timeBuf, sizeof(timeBuf), "+%02lu:%02lu:%02lu",
               totalSec / 3600, (totalSec % 3600) / 60, totalSec % 60);
    }
    output += "[";
    output += timeBuf;
    output += "] ";
    output += systemLogger.getLevelString(entry.level);
    output += " : ";
    output += entry.message;
    output += "\n";
  }

  AsyncWebServerResponse* resp = request->beginResponse(200, "text/plain; charset=utf-8", output);
  resp->addHeader("Content-Disposition", "attachment; filename=\"pool_logs.txt\"");
  resp->addHeader("Cache-Control", "no-cache");
  request->send(resp);
}

static void handleGetLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long sinceTimestamp = 0;
  if (request->hasParam("since")) {
    String sinceParam = request->getParam("since")->value();
    sinceTimestamp = sinceParam.toInt();
  }

  auto logs = systemLogger.getRecentLogs(kMaxLogEntries);

  // Sérialisation manuelle pour éviter la limite du JsonDocument avec 200 entrées
  String json;
  json.reserve(logs.size() * 120 + 32);
  json = "{\"uptime_ms\":";
  json += String(millis());
  json += ",\"logs\":[";

  bool first = true;
  for (const auto& entry : logs) {
    if (sinceTimestamp > 0 && entry.timestamp <= sinceTimestamp) continue;

    if (!first) json += ",";
    first = false;

    // Échapper le message pour JSON (tous les caractères problématiques)
    String escaped;
    escaped.reserve(entry.message.length() + 8);
    for (char c : entry.message) {
      if      (c == '\\') escaped += "\\\\";
      else if (c == '"')  escaped += "\\\"";
      else if (c == '\n') escaped += "\\n";
      else if (c == '\r') { /* skip */ }
      else if (c == '\t') escaped += "\\t";
      else if ((uint8_t)c < 0x20) { /* skip autres ctrl */ }
      else escaped += c;
    }

    json += "{\"timestamp\":";
    json += String(entry.timestamp);
    json += ",\"level\":\"";
    json += systemLogger.getLevelString(entry.level);
    json += "\",\"message\":\"";
    json += escaped;
    json += "\"}";
  }
  json += "]}";

  request->send(200, "application/json", json);
}

static void handleGetHistory(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?range=24h|3d|7d|30d|all
  String range = "all";
  if (request->hasParam("range")) {
    range = request->getParam("range")->value();
  }

  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long since = 0;
  if (request->hasParam("since")) {
    since = request->getParam("since")->value().toInt();
  }

  std::vector<DataPoint> data;

  if (range == "24h") {
    data = history.getLastHours(24);
  } else if (range == "3d") {
    data = history.getLastHours(24 * 3);
  } else if (range == "7d") {
    data = history.getLastHours(24 * 7);
  } else if (range == "30d") {
    data = history.getLastHours(24 * 30);
  } else {
    data = history.getAllData();
  }

  // Sérialisation manuelle pour éviter la limite mémoire du JsonDocument
  // avec plusieurs centaines de points (360 horaires + 75 journaliers)
  String json;
  json.reserve(data.size() * 80 + 64);
  json = "{\"range\":\"";
  json += range;
  json += "\",\"count\":0,\"history\":[";

  size_t count = 0;
  bool first = true;
  for (const auto& point : data) {
    if (since > 0 && point.timestamp <= since) continue;
    if (!first) json += ",";
    first = false;
    count++;

    json += "{\"timestamp\":";
    json += String(point.timestamp);
    json += ",\"ph\":";
    json += (!isfinite(point.ph))  ? "null" : String(round(point.ph * 10.0f) / 10.0f, 1);
    json += ",\"orp\":";
    json += (!isfinite(point.orp)) ? "null" : String((int)round(point.orp));
    json += ",\"temperature\":";
    json += (!isfinite(point.temperature)) ? "null" : String(round(point.temperature * 10.0f) / 10.0f, 1);
    json += ",\"filtration\":";
    json += point.filtrationActive ? "true" : "false";
    json += ",\"dosing\":";
    json += (point.phDosing || point.orpDosing) ? "true" : "false";
    json += ",\"granularity\":";
    json += String(static_cast<uint8_t>(point.granularity));
    json += "}";
  }
  json += "],\"count\":";
  json += String(count);
  json += "}";

  request->send(200, "application/json", json);
}

void setupDataRoutes(AsyncWebServer* server) {
  server->on("/data", HTTP_GET, handleGetData);
  server->on("/get-logs", HTTP_GET, handleGetLogs);
  server->on("/download-logs", HTTP_GET, handleDownloadLogs);
  server->on("/get-history", HTTP_GET, handleGetHistory);

  auto* importHandler = new AsyncCallbackJsonWebHandler(
    "/history/import",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      REQUIRE_AUTH(request, RouteProtection::WRITE);

      JsonObject root = json.as<JsonObject>();
      if (!root["history"].is<JsonArray>()) {
        sendErrorResponse(request, 400, "Format invalide: champ history manquant");
        return;
      }

      JsonArray items = root["history"].as<JsonArray>();
      if (items.isNull() || items.size() == 0) {
        sendErrorResponse(request, 400, "Historique vide");
        return;
      }

      std::vector<DataPoint> imported;
      imported.reserve(items.size());

      for (JsonObject item : items) {
        if (!item["timestamp"].is<unsigned long>()) {
          continue;
        }
        unsigned long ts = item["timestamp"] | 0;
        if (ts == 0) {
          continue;
        }

        DataPoint dp;
        dp.timestamp = ts;
        dp.ph = item["ph"].isNull() ? NAN : item["ph"].as<float>();
        dp.orp = item["orp"].isNull() ? NAN : item["orp"].as<float>();
        dp.temperature = item["temperature"].isNull() ? NAN : item["temperature"].as<float>();
        dp.filtrationActive = item["filtration"] | false;
        dp.phDosing = item["dosing"] | false;
        dp.orpDosing = false;
        uint8_t granularity = item["granularity"] | 0;
        dp.granularity = granularity <= DAILY ? static_cast<Granularity>(granularity) : RAW;

        imported.push_back(dp);
      }

      if (imported.empty()) {
        sendErrorResponse(request, 400, "Aucune donnée valide à importer");
        return;
      }

      if (!history.importData(imported)) {
        sendErrorResponse(request, 500, "Impossible d'importer l'historique");
        return;
      }

      JsonDocument doc;
      doc["status"] = "success";
      doc["count"] = imported.size();
      sendJsonResponse(request, doc);
    });
  importHandler->setMethod(HTTP_POST);
  server->addHandler(importHandler);

  server->on("/history/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::WRITE);
    history.clearHistory();

    JsonDocument doc;
    doc["status"] = "success";
    sendJsonResponse(request, doc);
  });
}
