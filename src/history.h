#ifndef HISTORY_H
#define HISTORY_H

#include <Arduino.h>
#include <vector>
#include <LittleFS.h>

struct DataPoint {
  unsigned long timestamp;
  float ph;
  float orp;
  float temperature;
  bool filtrationActive;
  bool phDosing;
  bool orpDosing;
};

class HistoryManager {
private:
  static const size_t MAX_MEMORY_POINTS = 288; // 24h à 5min intervalle
  std::vector<DataPoint> memoryBuffer;
  unsigned long lastSave = 0;
  unsigned long lastRecord = 0;
  const unsigned long RECORD_INTERVAL = 300000; // 5 minutes

  void saveToFile();
  void loadFromFile();
  void pruneOldData();

public:
  void begin();
  void update();
  void recordDataPoint();

  std::vector<DataPoint> getLastHours(int hours);
  std::vector<DataPoint> getLastDay();
  void exportCSV(String& output);
  void clearHistory();
};

extern HistoryManager history;

#endif // HISTORY_H
