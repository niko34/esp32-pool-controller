#include "history.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include <ArduinoJson.h>
#include <map>
#include <algorithm>

HistoryManager history;

namespace {
fs::LittleFSFS historyFs;
fs::FS* historyStore = &LittleFS;
const char* historyFilePath = "/history.json";
}  // namespace

void HistoryManager::begin() {
  if (historyFs.begin(true, "/history", 5, "history")) {
    historyStore = &historyFs;
    historyFilePath = "/history/history.json";
    systemLogger.info("Partition historique dédiée montée");
  } else {
    historyStore = &LittleFS;
    historyFilePath = "/history.json";
    systemLogger.warning("Partition historique dédiée indisponible, fallback LittleFS");
  }

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

  // Consolider les données toutes les heures
  if (now - lastConsolidation >= CONSOLIDATION_INTERVAL) {
    consolidateData();
    lastConsolidation = now;
  }

  // Sauvegarder sur fichier toutes les heures
  if (now - lastSave >= 3600000) {
    saveToFile();
    lastSave = now;
  }
}

void HistoryManager::recordDataPoint() {
  DataPoint point;
  point.timestamp = millis() / kMillisToSeconds; // Secondes depuis démarrage
  point.ph = sensors.getPh();
  point.orp = sensors.getOrp();
  point.temperature = sensors.getTemperature();
  point.filtrationActive = filtration.isRunning();
  point.phDosing = PumpController.isPhDosing();
  point.orpDosing = PumpController.isOrpDosing();
  point.granularity = RAW;

  memoryBuffer.push_back(point);

  // Limiter le nombre de points RAW
  size_t rawCount = 0;
  for (const auto& p : memoryBuffer) {
    if (p.granularity == RAW) rawCount++;
  }

  if (rawCount > MAX_RAW_POINTS) {
    // Supprimer les points RAW en excès (les plus anciens)
    // Utilise erase-remove idiom pour éviter O(n²)
    size_t toRemove = rawCount - MAX_RAW_POINTS;
    size_t removed = 0;
    memoryBuffer.erase(
      std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
        [&removed, toRemove](const DataPoint& p) {
          if (removed < toRemove && p.granularity == RAW) {
            removed++;
            return true;
          }
          return false;
        }),
      memoryBuffer.end()
    );
  }
}

void HistoryManager::saveToFile() {
  File f = historyStore->open(historyFilePath, "w");
  if (!f) {
    systemLogger.error("Impossible de sauvegarder l'historique");
    return;
  }

  JsonDocument doc;
  JsonArray data = doc["data"].to<JsonArray>();

  // Sauvegarder tous les points (déjà optimisés par consolidation)
  for (const auto& point : memoryBuffer) {
    JsonObject obj = data.add<JsonObject>();
    obj["t"] = point.timestamp;
    obj["p"] = serialized(String(point.ph, 2));
    obj["o"] = serialized(String(point.orp, 1));
    if (!isnan(point.temperature)) {
      obj["T"] = serialized(String(point.temperature, 1));
    }
    obj["f"] = point.filtrationActive;
    obj["d"] = point.phDosing || point.orpDosing;
    obj["g"] = static_cast<uint8_t>(point.granularity);
  }

  if (serializeJson(doc, f) == 0) {
    systemLogger.error("Échec sérialisation historique");
  }

  f.close();
  systemLogger.debug("Historique sauvegardé (" + String(data.size()) + " points)");
}

void HistoryManager::loadFromFile() {
  if (!historyStore->exists(historyFilePath)) {
    systemLogger.info("Aucun historique existant");
    return;
  }

  File f = historyStore->open(historyFilePath, "r");
  if (!f) {
    systemLogger.error("Impossible de charger l'historique");
    return;
  }

  JsonDocument doc;
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
    dp.granularity = static_cast<Granularity>(point["g"] | 0);
    memoryBuffer.push_back(dp);
  }

  systemLogger.info("Historique chargé (" + String(memoryBuffer.size()) + " points)");
}

std::vector<DataPoint> HistoryManager::getLastHours(int hours) {
  std::vector<DataPoint> result;
  if (memoryBuffer.empty()) return result;

  unsigned long now = millis() / kMillisToSeconds;
  unsigned long rangeSeconds = hours * kSecondsPerHour;
  if (now < rangeSeconds) {
    return memoryBuffer;
  }
  unsigned long cutoff = now - rangeSeconds;

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

std::vector<DataPoint> HistoryManager::getAllData() {
  return memoryBuffer;
}

void HistoryManager::consolidateData() {
  unsigned long now = millis() / kMillisToSeconds;
  systemLogger.debug("Début consolidation historique");

  // 1. Supprimer les données trop anciennes (> 90 jours)
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [now](const DataPoint& p) {
        unsigned long age = now - p.timestamp; // age en secondes
        return age > DAILY_MAX_AGE;
      }),
    memoryBuffer.end()
  );

  // 2. Convertir les points RAW > 6h en moyennes horaires
  std::vector<DataPoint> hourlyPoints;

  // Grouper les points RAW par heure
  std::map<unsigned long, std::vector<DataPoint>> hourlyGroups;

  for (const auto& point : memoryBuffer) {
    if (point.granularity == RAW) {
      unsigned long age = now - point.timestamp; // age en secondes
      if (age > RAW_MAX_AGE) {
        // Grouper par heure (arrondir timestamp à l'heure)
        unsigned long hourTimestamp = (point.timestamp / kSecondsPerHour) * kSecondsPerHour;
        hourlyGroups[hourTimestamp].push_back(point);
      }
    }
  }

  // Créer les moyennes horaires
  for (const auto& group : hourlyGroups) {
    if (group.second.empty()) continue;

    DataPoint avgPoint;
    avgPoint.timestamp = group.first;
    avgPoint.ph = 0;
    avgPoint.orp = 0;
    avgPoint.temperature = 0;
    avgPoint.filtrationActive = false;
    avgPoint.phDosing = false;
    avgPoint.orpDosing = false;
    avgPoint.granularity = HOURLY;

    int validCount = 0;
    int filtrationCount = 0;
    int phDosingCount = 0;
    int orpDosingCount = 0;

    for (const auto& p : group.second) {
      if (!isnan(p.ph)) {
        avgPoint.ph += p.ph;
        validCount++;
      }
      if (!isnan(p.orp)) avgPoint.orp += p.orp;
      if (!isnan(p.temperature)) avgPoint.temperature += p.temperature;
      if (p.filtrationActive) filtrationCount++;
      if (p.phDosing) phDosingCount++;
      if (p.orpDosing) orpDosingCount++;
    }

    if (validCount > 0) {
      avgPoint.ph /= validCount;
      avgPoint.orp /= validCount;
      avgPoint.temperature /= validCount;
      avgPoint.filtrationActive = (filtrationCount > group.second.size() / 2);
      avgPoint.phDosing = (phDosingCount > 0);
      avgPoint.orpDosing = (orpDosingCount > 0);

      hourlyPoints.push_back(avgPoint);
    }
  }

  // Supprimer les points RAW qui ont été convertis en horaires
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [now](const DataPoint& p) {
        unsigned long age = now - p.timestamp;
        return p.granularity == RAW && age > RAW_MAX_AGE;
      }),
    memoryBuffer.end()
  );

  // Ajouter les nouveaux points horaires
  memoryBuffer.insert(memoryBuffer.end(), hourlyPoints.begin(), hourlyPoints.end());

  // 3. Convertir les points HOURLY > 15 jours en moyennes journalières
  std::vector<DataPoint> dailyPoints;
  std::map<unsigned long, std::vector<DataPoint>> dailyGroups;

  for (const auto& point : memoryBuffer) {
    if (point.granularity == HOURLY) {
      unsigned long age = now - point.timestamp; // age en secondes
      if (age > HOURLY_MAX_AGE) {
        // Grouper par jour (arrondir timestamp au jour)
        unsigned long dayTimestamp = (point.timestamp / 86400) * 86400;
        dailyGroups[dayTimestamp].push_back(point);
      }
    }
  }

  // Créer les moyennes journalières
  for (const auto& group : dailyGroups) {
    if (group.second.empty()) continue;

    DataPoint avgPoint;
    avgPoint.timestamp = group.first;
    avgPoint.ph = 0;
    avgPoint.orp = 0;
    avgPoint.temperature = 0;
    avgPoint.filtrationActive = false;
    avgPoint.phDosing = false;
    avgPoint.orpDosing = false;
    avgPoint.granularity = DAILY;

    int validCount = 0;
    int filtrationCount = 0;
    int phDosingCount = 0;
    int orpDosingCount = 0;

    for (const auto& p : group.second) {
      if (!isnan(p.ph)) {
        avgPoint.ph += p.ph;
        validCount++;
      }
      if (!isnan(p.orp)) avgPoint.orp += p.orp;
      if (!isnan(p.temperature)) avgPoint.temperature += p.temperature;
      if (p.filtrationActive) filtrationCount++;
      if (p.phDosing) phDosingCount++;
      if (p.orpDosing) orpDosingCount++;
    }

    if (validCount > 0) {
      avgPoint.ph /= validCount;
      avgPoint.orp /= validCount;
      avgPoint.temperature /= validCount;
      avgPoint.filtrationActive = (filtrationCount > group.second.size() / 2);
      avgPoint.phDosing = (phDosingCount > 0);
      avgPoint.orpDosing = (orpDosingCount > 0);

      dailyPoints.push_back(avgPoint);
    }
  }

  // Supprimer les points HOURLY qui ont été convertis en journaliers
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [now](const DataPoint& p) {
        unsigned long age = now - p.timestamp;
        return p.granularity == HOURLY && age > HOURLY_MAX_AGE;
      }),
    memoryBuffer.end()
  );

  // Ajouter les nouveaux points journaliers
  memoryBuffer.insert(memoryBuffer.end(), dailyPoints.begin(), dailyPoints.end());

  // 4. Limiter le nombre de points par granularité
  size_t hourlyCount = 0, dailyCount = 0;
  for (const auto& p : memoryBuffer) {
    if (p.granularity == HOURLY) hourlyCount++;
    if (p.granularity == DAILY) dailyCount++;
  }

  // Supprimer les points horaires en excès (les plus anciens)
  // Utilise erase-remove idiom pour éviter O(n²)
  if (hourlyCount > MAX_HOURLY_POINTS) {
    size_t toRemove = hourlyCount - MAX_HOURLY_POINTS;
    size_t removed = 0;
    memoryBuffer.erase(
      std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
        [&removed, toRemove](const DataPoint& p) {
          if (removed < toRemove && p.granularity == HOURLY) {
            removed++;
            return true;
          }
          return false;
        }),
      memoryBuffer.end()
    );
  }

  // Supprimer les points journaliers en excès (les plus anciens)
  // Utilise erase-remove idiom pour éviter O(n²)
  if (dailyCount > MAX_DAILY_POINTS) {
    size_t toRemove = dailyCount - MAX_DAILY_POINTS;
    size_t removed = 0;
    memoryBuffer.erase(
      std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
        [&removed, toRemove](const DataPoint& p) {
          if (removed < toRemove && p.granularity == DAILY) {
            removed++;
            return true;
          }
          return false;
        }),
      memoryBuffer.end()
    );
  }

  // Trier par timestamp
  std::sort(memoryBuffer.begin(), memoryBuffer.end(),
    [](const DataPoint& a, const DataPoint& b) {
      return a.timestamp < b.timestamp;
    });

  systemLogger.info("Consolidation terminée: " + String(memoryBuffer.size()) + " points");
  saveToFile();
}

void HistoryManager::clearHistory() {
  memoryBuffer.clear();
  historyStore->remove(historyFilePath);
  systemLogger.warning("Historique effacé");
}
