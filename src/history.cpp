#include "history.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include <ArduinoJson.h>

HistoryManager history;

void HistoryManager::begin() {
  loadFromFile();
  systemLogger.info("Gestionnaire d'historique initialisé");
}

void HistoryManager::update() {
  unsigned long now = millis();

  // Enregistrer un point toutes les 5 minutes
  if (now - lastRecord >= RECORD_INTERVAL) {
    recordDataPoint();
    lastRecord = now;
  }

  // Sauvegarder sur fichier toutes les heures
  if (now - lastSave >= 3600000) {
    saveToFile();
    lastSave = now;
  }
}

void HistoryManager::recordDataPoint() {
  DataPoint point;
  point.timestamp = millis() / 1000; // Secondes depuis démarrage
  point.ph = sensors.getPh();
  point.orp = sensors.getOrp();
  point.temperature = sensors.getTemperature();
  point.filtrationActive = filtration.isRunning();
  point.phDosing = PumpController.isPhDosing();
  point.orpDosing = PumpController.isOrpDosing();

  if (memoryBuffer.size() >= MAX_MEMORY_POINTS) {
    memoryBuffer.erase(memoryBuffer.begin()); // FIFO
  }

  memoryBuffer.push_back(point);
}

void HistoryManager::saveToFile() {
  File f = LittleFS.open("/history.json", "w");
  if (!f) {
    systemLogger.error("Impossible de sauvegarder l'historique");
    return;
  }

  DynamicJsonDocument doc(16384); // ~16KB pour historique
  JsonArray data = doc.createNestedArray("data");

  // Sauvegarder seulement les dernières 24h
  size_t startIdx = memoryBuffer.size() > 288 ? memoryBuffer.size() - 288 : 0;

  for (size_t i = startIdx; i < memoryBuffer.size(); i++) {
    JsonObject point = data.createNestedObject();
    point["t"] = memoryBuffer[i].timestamp;
    point["p"] = serialized(String(memoryBuffer[i].ph, 2));
    point["o"] = serialized(String(memoryBuffer[i].orp, 1));
    if (!isnan(memoryBuffer[i].temperature)) {
      point["T"] = serialized(String(memoryBuffer[i].temperature, 1));
    }
    point["f"] = memoryBuffer[i].filtrationActive;
    point["d"] = memoryBuffer[i].phDosing || memoryBuffer[i].orpDosing;
  }

  if (serializeJson(doc, f) == 0) {
    systemLogger.error("Échec sérialisation historique");
  }

  f.close();
  systemLogger.debug("Historique sauvegardé (" + String(data.size()) + " points)");
}

void HistoryManager::loadFromFile() {
  if (!LittleFS.exists("/history.json")) {
    systemLogger.info("Aucun historique existant");
    return;
  }

  File f = LittleFS.open("/history.json", "r");
  if (!f) {
    systemLogger.error("Impossible de charger l'historique");
    return;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, f);
  f.close();

  if (error) {
    systemLogger.error("Erreur parsing historique: " + String(error.c_str()));
    return;
  }

  JsonArray data = doc["data"];
  memoryBuffer.clear();

  for (JsonObject point : data) {
    DataPoint dp;
    dp.timestamp = point["t"];
    dp.ph = point["p"].as<float>();
    dp.orp = point["o"].as<float>();
    dp.temperature = point["T"] | NAN;
    dp.filtrationActive = point["f"];
    dp.phDosing = point["d"];
    dp.orpDosing = false;
    memoryBuffer.push_back(dp);
  }

  systemLogger.info("Historique chargé (" + String(memoryBuffer.size()) + " points)");
}

std::vector<DataPoint> HistoryManager::getLastHours(int hours) {
  std::vector<DataPoint> result;
  if (memoryBuffer.empty()) return result;

  unsigned long cutoff = (millis() / 1000) - (hours * 3600);

  for (const auto& point : memoryBuffer) {
    if (point.timestamp >= cutoff) {
      result.push_back(point);
    }
  }

  return result;
}

std::vector<DataPoint> HistoryManager::getLastDay() {
  return getLastHours(24);
}

void HistoryManager::exportCSV(String& output) {
  output = "Timestamp,pH,ORP(mV),Temperature(C),Filtration,Dosing\n";

  for (const auto& point : memoryBuffer) {
    output += String(point.timestamp) + ",";
    output += String(point.ph, 2) + ",";
    output += String(point.orp, 1) + ",";
    output += isnan(point.temperature) ? "N/A" : String(point.temperature, 1);
    output += ",";
    output += point.filtrationActive ? "ON" : "OFF";
    output += ",";
    output += (point.phDosing || point.orpDosing) ? "ACTIVE" : "IDLE";
    output += "\n";
  }
}

void HistoryManager::clearHistory() {
  memoryBuffer.clear();
  LittleFS.remove("/history.json");
  systemLogger.warning("Historique effacé");
}
