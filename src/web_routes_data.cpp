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
#include <esp_heap_caps.h>

namespace {
bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}
}  // namespace

static void handleGetData(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);
  // Buffer statique pour éviter la fragmentation du heap
  // Taille estimée : ~16 champs × 30 bytes + overhead (feature-020 ajoute 3 champs)
  // feature-025 : +14 champs filtre pH/ORP → bump du buffer.
  StaticJson<1152> doc;

  // feature-025 : la "valeur courante" (champs ph/orp) reflète la valeur FILTRÉE
  // (médiane + EMA), alignée sur le WS. Le brut reste exposé séparément (phRaw/orpRaw).
  // Fallback sur le brut si le filtre n'est pas encore amorcé (warmup).
  float orpRaw = sensors.getOrpRaw();
  float phRaw  = sensors.getPhRaw();
  float orpMed = sensors.getOrpMedian();
  float phMed  = sensors.getPhMedian();
  float orpFil = sensors.getOrpFiltered();
  float phFil  = sensors.getPhFiltered();
  float orpVal = !isnan(orpFil) ? orpFil : orpRaw;
  float phVal  = !isnan(phFil)  ? phFil  : phRaw;

  // ORP (valeur courante = filtrée)
  if (!isnan(orpVal)) {
    doc["orp"] = round(orpVal);
  } else {
    doc["orp"] = nullptr;
  }

  // pH (feature-021 : 3 décimales fiables EZO — alignement WS / MQTT / REST).
  if (!isnan(phVal)) {
    doc["ph"] = round(phVal * 1000.0f) / 1000.0f;
  } else {
    doc["ph"] = nullptr;
  }

  // feature-025 : chaîne de filtrage exposée pour la chip d'état + ligne brute UI.
  if (!isnan(phRaw)) doc["phRaw"] = round(phRaw * 1000.0f) / 1000.0f; else doc["phRaw"] = nullptr;
  if (!isnan(phMed)) doc["phMedian"] = round(phMed * 1000.0f) / 1000.0f; else doc["phMedian"] = nullptr;
  if (!isnan(phFil)) doc["phFiltered"] = round(phFil * 1000.0f) / 1000.0f; else doc["phFiltered"] = nullptr;
  doc["phFilterReady"]    = sensors.isPhFilterReady();
  doc["phFilterUnstable"] = sensors.isPhFilterUnstable();
  doc["phRejectedCount"]  = sensors.getPhRejectedCount();
  if (!isnan(orpRaw)) doc["orpRaw"] = round(orpRaw); else doc["orpRaw"] = nullptr;
  if (!isnan(orpMed)) doc["orpMedian"] = round(orpMed); else doc["orpMedian"] = nullptr;
  if (!isnan(orpFil)) doc["orpFiltered"] = round(orpFil); else doc["orpFiltered"] = nullptr;
  doc["orpFilterReady"]    = sensors.isOrpFilterReady();
  doc["orpFilterUnstable"] = sensors.isOrpFilterUnstable();
  doc["orpRejectedCount"]  = sensors.getOrpRejectedCount();

  // feature-034 : points de calibration EZO (déjà publiés en WS, mais ABSENTS de /data —
  // nécessaires au polling de calibration qui interroge /data activement, Safari mettant
  // le WebSocket en pause sur un onglet non focalisé).
  doc["phCalPoints"]  = sensors.getPhCalibrationPointsCached();
  doc["orpCalPoints"] = sensors.getOrpCalibrationPointsCached();

  // Température (offset utilisateur appliqué dans getTemperature())
  if (!isnan(sensors.getTemperature())) {
    doc["temperature"] = sensors.getTemperature();
  } else {
    doc["temperature"] = nullptr;
  }

  // feature-020 : T° circuit (2ᵉ sonde DS18B20) + indicateurs identification
  float tCircuit = sensors.getCircuitTemperature();
  if (!isnan(tCircuit)) {
    doc["temperature_circuit"] = round(tCircuit * 10.0f) / 10.0f;
  } else {
    doc["temperature_circuit"] = nullptr;
  }
  doc["sondes_identified"] = sensors.areSondesIdentified();
  doc["sondes_detected"]   = sensors.getDetectedSondeCount();

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

  // v2.8.2 : garde heap — sous forte fragmentation, refuser proprement (503) plutôt
  // que de construire une réponse volumineuse (cf. crash /logs std::bad_alloc).
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinHeapForLogsResponse) {
    request->send(503, "text/plain; charset=utf-8",
                  "Mémoire insuffisante — réessayez plus tard\n");
    return;
  }

  // Calculer l'epoch de démarrage pour reconstruire les timestamps absolus
  time_t nowEpoch = time(nullptr);
  unsigned long nowMs = millis();
  bool hasAbsoluteTime = (nowEpoch > 1609459200L);
  long bootEpoch = hasAbsoluteTime ? (long)(nowEpoch - nowMs / 1000) : 0;

  // v2.8.2 : réponse en flux (plus de grosse String contiguë). Le StreamString sous-
  // jacent grandit par realloc (pas d'operator new → aucun std::bad_alloc / abort).
  AsyncResponseStream* res = request->beginResponseStream("text/plain; charset=utf-8");
  res->addHeader("Content-Disposition", "attachment; filename=\"pool_logs.txt\"");
  res->addHeader("Cache-Control", "no-cache");

  // En-tête
  res->print("# Pool Controller — Journal système\n");
  if (hasAbsoluteTime) {
    char exportTime[24];
    struct tm t;
    localtime_r(&nowEpoch, &t);
    strftime(exportTime, sizeof(exportTime), "%Y-%m-%d %H:%M:%S", &t);
    res->print("# Exporté le : ");
    res->print(exportTime);
  } else {
    res->print("# Exporté le boot+");
    res->print(nowMs / 1000);
    res->print("s (heure non synchronisée)");
  }
  res->print("\n# Format: [YYYY-MM-DD HH:MM:SS] NIVEAU : message\n");

  // ---- Logs persistants (boots précédents) — copie par blocs, hors mutex logger ----
  fs::FS* pfs = systemLogger.getPersistenceFs();
  if (pfs) {
    File f = pfs->open("/system.log", "r");
    if (f && f.size() > 0) {
      res->print("\n# === Historique persistant ===\n");
      uint8_t buf[256];
      while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n > 0) res->write(buf, n);
      }
      f.close();
      res->print("# === Fin historique persistant ===\n");
    } else if (f) {
      f.close();
    }
  }

  // ---- Logs RAM (session courante) — flux via visiteur (aucune copie) ----
  res->print("\n# === Session courante ===\n");
  systemLogger.forEachRecentLog(kMaxLogEntries, 0,
    [res, hasAbsoluteTime, bootEpoch](const LogEntry& entry) {
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
      res->print("[");
      res->print(timeBuf);
      res->print("] ");
      res->print(systemLogger.getLevelString(entry.level));
      res->print(" : ");
      res->print(entry.message);
      res->print("\n");
    });

  request->send(res);
}

static void handleGetLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long sinceTimestamp = 0;
  if (request->hasParam("since")) {
    String sinceParam = request->getParam("since")->value();
    sinceTimestamp = sinceParam.toInt();
  }

  // v2.8.2 : garde heap — sous forte fragmentation du tas, refuser proprement (503)
  // au lieu de tenter une allocation. C'est le site du crash historique : l'ancien
  // getRecentLogs() copiait tout le buffer dans un std::vector<LogEntry> (operator new
  // → std::bad_alloc → abort). On itère désormais sans copie + réponse en flux.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinHeapForLogsResponse) {
    request->send(503, "application/json", "{\"error\":\"low_memory\"}");
    return;
  }

  // Réponse en flux : plus de std::vector copié, plus de String géante contiguë.
  // Le StreamString grandit par realloc (jamais operator new → jamais d'abort).
  AsyncResponseStream* res = request->beginResponseStream("application/json");
  res->print("{\"uptime_ms\":");
  res->print(millis());
  res->print(",\"logs\":[");

  bool first = true;
  systemLogger.forEachRecentLog(kMaxLogEntries, sinceTimestamp,
    [res, &first](const LogEntry& entry) {
      if (!first) res->print(',');
      first = false;

      res->print("{\"timestamp\":");
      res->print(entry.timestamp);
      res->print(",\"level\":\"");
      res->print(systemLogger.getLevelString(entry.level));
      res->print("\",\"message\":\"");

      // Échappement JSON écrit directement dans le flux (aucune String temporaire
      // allouée sous le mutex logger — écritures mémoire rapides uniquement).
      for (char c : entry.message) {
        if      (c == '\\') res->print("\\\\");
        else if (c == '"')  res->print("\\\"");
        else if (c == '\n') res->print("\\n");
        else if (c == '\r') { /* skip */ }
        else if (c == '\t') res->print("\\t");
        else if ((uint8_t)c < 0x20) { /* skip autres ctrl */ }
        else res->write((uint8_t)c);
      }

      res->print("\"}");
    });

  res->print("]}");
  request->send(res);
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

  sendRawJsonResponse(request, json);
}

void setupDataRoutes(AsyncWebServer* server) {
  server->on("/data", HTTP_GET, handleGetData);
  server->on("/get-logs", HTTP_GET, handleGetLogs);
  server->on("/download-logs", HTTP_GET, handleDownloadLogs);
  server->on("/logs", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::WRITE);
    systemLogger.clearAll();
    systemLogger.info("Logs effacés (RAM + fichier persistant)");
    JsonDocument doc;
    doc["success"] = true;
    sendJsonResponse(request, doc);
  });
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
    // feature-027 : clearHistory() peut échouer si le mutex historique est occupé
    if (!history.clearHistory()) {
      sendErrorResponse(request, 503, "Historique occupé — réessayer");
      return;
    }

    JsonDocument doc;
    doc["status"] = "success";
    sendJsonResponse(request, doc);
  });
}
