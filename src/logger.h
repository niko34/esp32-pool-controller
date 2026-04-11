#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <vector>
#include <functional>
#include <freertos/semphr.h>
#include <FS.h>
#include "constants.h"

enum class LogLevel {
  DEBUG,
  INFO,
  WARNING,
  ERROR,
  CRITICAL
};

struct LogEntry {
  unsigned long timestamp;
  LogLevel level;
  String message;
};

class Logger {
private:
  static const size_t MAX_LOGS = kMaxLogEntries;
  std::vector<LogEntry> logs;
  size_t currentIndex = 0;
  bool bufferFull = false;
  std::function<void(const LogEntry&)> _logCallback = nullptr;
  SemaphoreHandle_t _mutex = nullptr;

  // Persistance sur LittleFS (partition history)
  fs::FS* _persistFs = nullptr;
  bool _persistEnabled = false;
  std::vector<String> _persistBuffer;
  unsigned long _lastFlushMs = 0;
  static constexpr unsigned long kFlushIntervalMs = 600000UL;   // 10min
  static constexpr size_t kMaxLogFileBytes = 32768;             // 32KB
  static constexpr size_t kRotateKeepBytes = 24576;             // Garde les 24 derniers KB

public:
  Logger();  // Constructeur pour pré-allouer le buffer
  void begin();  // Initialise le mutex FreeRTOS (appeler depuis setup())

  void log(LogLevel level, const String& message);
  void debug(const String& message);
  void info(const String& message);
  void warning(const String& message);
  void error(const String& message);
  void critical(const String& message);

  // Callback appelé à chaque nouveau log (utilisé par WsManager pour le push temps réel)
  void setLogCallback(std::function<void(const LogEntry&)> cb) { _logCallback = cb; }

  String getLevelString(LogLevel level);
  std::vector<LogEntry> getRecentLogs(size_t count = 50);
  void clear();
  size_t getLogCount();

  // Persistance LittleFS
  void setPersistenceFs(fs::FS* fs);  // Appeler après montage de la partition history
  void update();                       // Appeler depuis la loop principale
  void flushToDisk();                  // Forcer un flush immédiat
  fs::FS* getPersistenceFs() const { return _persistFs; }
};

extern Logger systemLogger;

#endif // LOGGER_H
