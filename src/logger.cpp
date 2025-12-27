#include "logger.h"

Logger systemLogger;

// Constructeur : pré-alloue le buffer pour éviter les réallocations
Logger::Logger() {
  logs.reserve(MAX_LOGS);
}

void Logger::log(LogLevel level, const String& message) {
  LogEntry entry;
  entry.timestamp = millis();
  entry.level = level;
  entry.message = message;

  if (logs.size() < MAX_LOGS) {
    logs.push_back(entry);
  } else {
    logs[currentIndex] = entry;
    currentIndex = (currentIndex + 1) % MAX_LOGS;
    bufferFull = true;
  }

  // Affichage série avec préfixe niveau
  Serial.print("[");
  Serial.print(getLevelString(level));
  Serial.print("] ");
  Serial.println(message);
}

void Logger::debug(const String& message) {
  log(LogLevel::DEBUG, message);
}

void Logger::info(const String& message) {
  log(LogLevel::INFO, message);
}

void Logger::warning(const String& message) {
  log(LogLevel::WARNING, message);
}

void Logger::error(const String& message) {
  log(LogLevel::ERROR, message);
}

void Logger::critical(const String& message) {
  log(LogLevel::CRITICAL, message);
}

String Logger::getLevelString(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO: return "INFO";
    case LogLevel::WARNING: return "WARN";
    case LogLevel::ERROR: return "ERROR";
    case LogLevel::CRITICAL: return "CRIT";
    default: return "UNKNOWN";
  }
}

std::vector<LogEntry> Logger::getRecentLogs(size_t count) {
  std::vector<LogEntry> result;

  if (!bufferFull) {
    // Buffer pas encore plein, retourner les derniers n éléments
    size_t start = logs.size() > count ? logs.size() - count : 0;
    for (size_t i = start; i < logs.size(); i++) {
      result.push_back(logs[i]);
    }
  } else {
    // Buffer circulaire plein
    size_t itemsToReturn = count < MAX_LOGS ? count : MAX_LOGS;
    size_t startIndex = (currentIndex + MAX_LOGS - itemsToReturn) % MAX_LOGS;

    for (size_t i = 0; i < itemsToReturn; i++) {
      size_t idx = (startIndex + i) % MAX_LOGS;
      result.push_back(logs[idx]);
    }
  }

  return result;
}

void Logger::clear() {
  logs.clear();
  currentIndex = 0;
  bufferFull = false;
}

size_t Logger::getLogCount() {
  return bufferFull ? MAX_LOGS : logs.size();
}
