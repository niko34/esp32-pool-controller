#ifndef HISTORY_H
#define HISTORY_H

#include <Arduino.h>
#include <vector>
#include <LittleFS.h>
#include <freertos/semphr.h>
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
  static const size_t MAX_RAW_POINTS = kMaxRawDataPoints;      // 8h de données brutes (5 min, cf. constants.h)
  static const size_t MAX_HOURLY_POINTS = kMaxHourlyDataPoints;  // 15 jours de moyennes horaires
  static const size_t MAX_DAILY_POINTS = kMaxDailyDataPoints;    // 75 jours de moyennes journalières
  static const unsigned long RAW_MAX_AGE = 21600UL;        // 6 heures (en secondes)
  static const unsigned long HOURLY_MAX_AGE = 1296000UL;   // 15 jours (en secondes)
  static const unsigned long DAILY_MAX_AGE = 7776000UL;    // 90 jours (en secondes)

  std::vector<DataPoint> memoryBuffer;
  SemaphoreHandle_t _mutex = nullptr;
  unsigned long lastSave = 0;
  unsigned long lastRecord = 0;
  const unsigned long RECORD_INTERVAL = 300000; // 5 minutes
  const unsigned long SAVE_INTERVAL = 300000; // 5 minutes
  bool historyEnabled = true;
  // feature-059 : partition effacée par le disjoncteur de boot-loop pendant begin()
  // (consommé par setup() pour publier l'alerte MQTT différée history_partition_reset).
  bool _partitionAutoErased = false;
  bool legacyHistoryPending = false;
  unsigned long legacyMaxTimestamp = 0;
  bool _preNtpPending = false;  // Points enregistrés avant sync NTP (timestamps uptime provisoires)
  // bug-nospc : epoch du dernier saveToFile réussi (0 si horloge non synchro).
  // Tenu en RAM ; persisté en NVS (last_ok) UNIQUEMENT sur échec de save — usure NVS.
  unsigned long _lastOkEpoch = 0;

  void saveToFile();
  void loadFromFile();
  void consolidateData();
  // bug-history-doublons-troncature (A2) : fusionne les agrégats ADJACENTS de
  // même (timestamp, granularité ≠ RAW) dans memoryBuffer — champs moyennés
  // équipondérés (historyMergeMeanFinite), booléens en OR. PRÉ-REQUIS : buffer
  // trié par timestamp (appelé après chaque std::sort). Assainit aussi, au
  // chargement, les doublons persistés par les versions ≤ 2.18.0.
  void dedupAggregatedPoints();
  void migrateLegacyHistory(unsigned long nowEpoch);
  void _applyPreNtpCorrection(unsigned long ntpEpoch, unsigned long uptimeSec);

public:
  void begin();
  void update();
  void recordDataPoint();

  std::vector<DataPoint> getLastHours(int hours);
  std::vector<DataPoint> getLastDay();
  std::vector<DataPoint> getAllData();
  // bug-heap-toctou A3 : copie d'UN point sous mutex COURT (kHistoryPointReadTimeoutMs)
  // pour le callback chunked de /get-history (tâche async_tcp — jamais le mutex tenu
  // entre deux chunks). chronoIdx = index direct dans memoryBuffer (vecteur unique
  // trié par timestamp). Retour false : fin de buffer (timedOut=false) OU timeout
  // mutex (timedOut=true → l'appelant émet un espace de maintien de flux et
  // réessaie au rappel suivant, cf. bug-history-doublons-troncature B1).
  bool getPointByIndex(size_t chronoIdx, DataPoint& out, bool& timedOut);
  bool importData(const std::vector<DataPoint>& dataPoints);
  // feature-027 : retourne false si le mutex n'a pas pu être pris (historique occupé)
  bool clearHistory();
  // feature-059 : true si le disjoncteur de boot-loop a effacé la partition
  // history pendant begin() (alerte MQTT publiée ensuite depuis setup()).
  bool partitionWasAutoErased() const { return _partitionAutoErased; }
};

extern HistoryManager history;

#endif // HISTORY_H
