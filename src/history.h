#ifndef HISTORY_H
#define HISTORY_H

#include <Arduino.h>
#include <vector>
#include <LittleFS.h>
#include "constants.h"

enum Granularity : uint8_t {
  RAW = 0,      // Point de données brut (5 minutes)
  HOURLY = 1,   // Moyenne horaire
  DAILY = 2     // Moyenne journalière
};

struct DataPoint {
  unsigned long timestamp;
  float ph;
  float orp;
  float temperature;
  bool filtrationActive;
  bool phDosing;
  bool orpDosing;
  Granularity granularity;
};

class HistoryManager {
private:
  // Limites de stockage
  static const size_t MAX_RAW_POINTS = kMaxRawDataPoints;      // 6h de données brutes (5 min)
  static const size_t MAX_HOURLY_POINTS = kMaxHourlyDataPoints;  // 15 jours de moyennes horaires
  static const size_t MAX_DAILY_POINTS = kMaxDailyDataPoints;    // 75 jours de moyennes journalières
  static const unsigned long RAW_MAX_AGE = 21600UL;        // 6 heures (en secondes)
  static const unsigned long HOURLY_MAX_AGE = 1296000UL;   // 15 jours (en secondes)
  static const unsigned long DAILY_MAX_AGE = 7776000UL;    // 90 jours (en secondes)

  std::vector<DataPoint> memoryBuffer;
  unsigned long lastSave = 0;
  unsigned long lastRecord = 0;
  const unsigned long RECORD_INTERVAL = 300000; // 5 minutes
  const unsigned long SAVE_INTERVAL = 300000; // 5 minutes
  bool historyEnabled = true;
  bool legacyHistoryPending = false;
  unsigned long legacyMaxTimestamp = 0;

  void saveToFile();
  void loadFromFile();
  void consolidateData();
  void migrateLegacyHistory(unsigned long nowEpoch);

public:
  void begin();
  void update();
  void recordDataPoint();

  std::vector<DataPoint> getLastHours(int hours);
  std::vector<DataPoint> getLastDay();
  std::vector<DataPoint> getAllData();
  bool importData(const std::vector<DataPoint>& dataPoints);
  void clearHistory();
};

extern HistoryManager history;

#endif // HISTORY_H
