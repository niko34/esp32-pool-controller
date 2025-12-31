#include "web_routes_data.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include "logger.h"
#include "history.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <time.h>

namespace {
constexpr time_t kMinValidEpoch = 1609459200;  // 2021-01-01

bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}
}  // namespace

static void handleGetData(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);
  // Buffer statique pour éviter la fragmentation du heap
  // Taille estimée : ~13 champs × 30 bytes + overhead = 512 bytes
  StaticJsonDocument<512> doc;

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

  time_t nowEpoch = time(nullptr);
  doc["time_synced"] = isTimeValid(nowEpoch);

  sendJsonResponse(request, doc);
}

static void handleGetLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long sinceTimestamp = 0;
  if (request->hasParam("since")) {
    String sinceParam = request->getParam("since")->value();
    sinceTimestamp = sinceParam.toInt();
  }

  auto logs = systemLogger.getRecentLogs(50);
  JsonDocument doc;
  JsonArray logsArray = doc["logs"].to<JsonArray>();

  for (const auto& entry : logs) {
    // Si since est spécifié, filtrer les logs plus anciens
    if (sinceTimestamp > 0 && entry.timestamp <= sinceTimestamp) {
      continue;
    }

    JsonObject logObj = logsArray.add<JsonObject>();
    logObj["timestamp"] = entry.timestamp;
    logObj["level"] = systemLogger.getLevelString(entry.level);
    logObj["message"] = entry.message;
  }

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

static void handleGetHistory(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?range=24h|7d|30d|all
  String range = "all";
  if (request->hasParam("range")) {
    range = request->getParam("range")->value();
  }

  std::vector<DataPoint> data;

  if (range == "24h") {
    data = history.getLastHours(24);
  } else if (range == "7d") {
    data = history.getLastHours(24 * 7);
  } else if (range == "30d") {
    data = history.getLastHours(24 * 30);
  } else {
    data = history.getAllData();
  }

  JsonDocument doc;
  JsonArray historyArray = doc["history"].to<JsonArray>();

  for (const auto& point : data) {
    JsonObject obj = historyArray.add<JsonObject>();
    obj["timestamp"] = point.timestamp;

    if (!isnan(point.ph)) {
      obj["ph"] = round(point.ph * 10.0f) / 10.0f;
    } else {
      obj["ph"] = nullptr;
    }

    if (!isnan(point.orp)) {
      obj["orp"] = round(point.orp);
    } else {
      obj["orp"] = nullptr;
    }

    if (!isnan(point.temperature)) {
      obj["temperature"] = round(point.temperature * 10.0f) / 10.0f;
    } else {
      obj["temperature"] = nullptr;
    }

    obj["filtration"] = point.filtrationActive;
    obj["dosing"] = point.phDosing || point.orpDosing;
    obj["granularity"] = static_cast<uint8_t>(point.granularity);
  }

  doc["count"] = historyArray.size();
  doc["range"] = range;

  sendJsonResponse(request, doc);
}

void setupDataRoutes(AsyncWebServer* server) {
  server->on("/data", HTTP_GET, handleGetData);
  server->on("/get-logs", HTTP_GET, handleGetLogs);
  server->on("/get-history", HTTP_GET, handleGetHistory);

  auto* importHandler = new AsyncCallbackJsonWebHandler(
    "/history/import",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      REQUIRE_AUTH(request, RouteProtection::WRITE);

      JsonObject root = json.as<JsonObject>();
      if (!root.containsKey("history") || !root["history"].is<JsonArray>()) {
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
        if (!item.containsKey("timestamp")) {
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
}
