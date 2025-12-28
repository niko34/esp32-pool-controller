#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <vector>
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

public:
  Logger();  // Constructeur pour pré-allouer le buffer

  void log(LogLevel level, const String& message);
  void debug(const String& message);
  void info(const String& message);
  void warning(const String& message);
  void error(const String& message);
  void critical(const String& message);

  String getLevelString(LogLevel level);
  std::vector<LogEntry> getRecentLogs(size_t count = 50);
  void clear();
  size_t getLogCount();
};

extern Logger systemLogger;

#endif // LOGGER_H
