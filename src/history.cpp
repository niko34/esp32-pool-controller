#include "history.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include <ArduinoJson.h>
#include <map>
#include <algorithm>
#include <time.h>
#include <Preferences.h>

HistoryManager history;

namespace {
fs::LittleFSFS historyFs;
fs::FS* historyStore = &LittleFS;
const char* historyFilePath = "/history.json";
constexpr time_t kMinValidEpoch = 1609459200;  // 2021-01-01
unsigned long lastKnownEpoch = 0;
bool warnedUnsynced = false;
bool warnedEstimated = false;

bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}

void loadClockPrefs() {
  Preferences prefs;
  if (prefs.begin("clock", true)) {
    lastKnownEpoch = prefs.getULong("epoch", 0);
    prefs.end();
  }
}

void saveClockPrefs(unsigned long epoch) {
  Preferences prefs;
  if (prefs.begin("clock", false)) {
    prefs.putULong("epoch", epoch);
    prefs.end();
  }
}

unsigned long getCurrentEpoch(bool* synced, bool* estimated) {
  time_t nowEpoch = time(nullptr);
  if (isTimeValid(nowEpoch)) {
    if (synced) *synced = true;
    if (estimated) *estimated = false;
    lastKnownEpoch = static_cast<unsigned long>(nowEpoch);
    saveClockPrefs(lastKnownEpoch);
    return lastKnownEpoch;
  }

  if (synced) *synced = false;
  if (lastKnownEpoch > 0) {
    if (estimated) *estimated = true;
    unsigned long sinceBoot = millis() / kMillisToSeconds;
    return lastKnownEpoch + sinceBoot;
  }

  if (estimated) *estimated = false;
  return 0;
}
}  // namespace

void HistoryManager::begin() {
  if (historyFs.begin(true, "/history", 5, "history")) {
    historyStore = &historyFs;
    historyFilePath = "/history.json";
    systemLogger.info("Partition historique dédiée montée");
  }
  else {
    systemLogger.info("Partition historique absente. Gestionnaire d'historique en erreur.");
    historyEnabled = false;
    return;
  }

  loadClockPrefs();
  loadFromFile();
  systemLogger.info("Gestionnaire d'historique initialisé");
}

void HistoryManager::update() {
  if (!historyEnabled) return;
  unsigned long now = millis();

  // Enregistrer un point toutes les 5 minutes
  if (now - lastRecord >= RECORD_INTERVAL) {
    recordDataPoint();
    lastRecord = now;
  }

  // Sauvegarder sur fichier toutes les heures
  if (now - lastSave >= SAVE_INTERVAL) {
    consolidateData();
    saveToFile();
    lastSave = now;
  }
}

void HistoryManager::recordDataPoint() {
  if (!historyEnabled) return;
  bool synced = false;
  bool estimated = false;
  unsigned long nowEpoch = getCurrentEpoch(&synced, &estimated);
  if (nowEpoch == 0) {
    if (!warnedUnsynced) {
      systemLogger.warning("Horloge non synchronisée, historique ignoré");
      warnedUnsynced = true;
    }
    return;
  }
  if (synced) {
    migrateLegacyHistory(nowEpoch);
  }
  if (!synced && estimated && !warnedEstimated) {
    systemLogger.warning("Horloge non synchronisée, historique estimé depuis la dernière heure connue");
    warnedEstimated = true;
  }

  DataPoint point;
  point.timestamp = nowEpoch;
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
  if (!historyEnabled) return;
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
  if (!historyEnabled) return;
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
  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;

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

  if (!memoryBuffer.empty()) {
    for (const auto& p : memoryBuffer) {
      if (p.timestamp > legacyMaxTimestamp) legacyMaxTimestamp = p.timestamp;
    }
    if (legacyMaxTimestamp > 0 && legacyMaxTimestamp < static_cast<unsigned long>(kMinValidEpoch)) {
      legacyHistoryPending = true;
      systemLogger.warning("Historique legacy détecté (timestamps uptime)");
      time_t nowEpoch = time(nullptr);
      if (isTimeValid(nowEpoch)) {
        migrateLegacyHistory(static_cast<unsigned long>(nowEpoch));
      }
    }
  }

  systemLogger.info("Historique chargé (" + String(memoryBuffer.size()) + " points)");
}

void HistoryManager::migrateLegacyHistory(unsigned long nowEpoch) {
  if (!legacyHistoryPending || legacyMaxTimestamp == 0) return;

  for (auto& p : memoryBuffer) {
    unsigned long delta = legacyMaxTimestamp - p.timestamp;
    p.timestamp = nowEpoch - delta;
  }

  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;
  systemLogger.warning("Historique legacy converti en epoch");
  saveToFile();
}

std::vector<DataPoint> HistoryManager::getLastHours(int hours) {
  std::vector<DataPoint> result;
  if (memoryBuffer.empty()) return result;

  bool synced = false;
  bool estimated = false;
  unsigned long nowEpoch = getCurrentEpoch(&synced, &estimated);
  if (nowEpoch == 0) {
    return memoryBuffer;
  }
  if (synced) {
    migrateLegacyHistory(nowEpoch);
  }

  unsigned long now = nowEpoch;
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

bool HistoryManager::importData(const std::vector<DataPoint>& dataPoints) {
  if (!historyEnabled) return false;
  if (dataPoints.empty()) return false;

  memoryBuffer = dataPoints;
  std::sort(memoryBuffer.begin(), memoryBuffer.end(),
    [](const DataPoint& a, const DataPoint& b) {
      return a.timestamp < b.timestamp;
    });

  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;
  lastSave = millis();
  lastRecord = millis();
  saveToFile();
  systemLogger.info("Historique importé (" + String(memoryBuffer.size()) + " points)");
  return true;
}

void HistoryManager::consolidateData() {
  if (!historyEnabled) return;
  bool synced = false;
  bool estimated = false;
  unsigned long now = getCurrentEpoch(&synced, &estimated);
  if (now == 0) {
    if (!warnedUnsynced) {
      systemLogger.warning("Horloge non synchronisée, consolidation ignorée");
      warnedUnsynced = true;
    }
    return;
  }
  if (synced) {
    migrateLegacyHistory(now);
  }
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
  if (!historyEnabled) return;
  memoryBuffer.clear();
  historyStore->remove(historyFilePath);
  systemLogger.warning("Historique effacé");
}
