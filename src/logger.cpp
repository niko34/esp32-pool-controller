#include "logger.h"

Logger systemLogger;

// Constructeur : pré-alloue le buffer pour éviter les réallocations
Logger::Logger() {
  logs.reserve(MAX_LOGS);
}

// Initialise le mutex FreeRTOS — appeler depuis setup() avant de démarrer le web server
void Logger::begin() {
  if (!_mutex) {
    _mutex = xSemaphoreCreateMutex();
  }
}

void Logger::log(LogLevel level, const String& message) {
  LogEntry entry;
  entry.timestamp = millis();
  entry.level = level;
  entry.message = message;

  // Capture du callback hors section critique pour éviter un deadlock
  // si le callback tente lui-même de logger.
  std::function<void(const LogEntry&)> cb = nullptr;

  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  if (logs.size() < MAX_LOGS) {
    logs.push_back(entry);
  } else {
    logs[currentIndex] = entry;
    currentIndex = (currentIndex + 1) % MAX_LOGS;
    bufferFull = true;
  }
  cb = _logCallback;
  if (_mutex) xSemaphoreGive(_mutex);

  // Push WebSocket temps réel (si callback enregistré) — hors mutex
  if (cb) cb(entry);

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

  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);

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

  if (_mutex) xSemaphoreGive(_mutex);
  return result;
}

void Logger::clear() {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  logs.clear();
  currentIndex = 0;
  bufferFull = false;
  if (_mutex) xSemaphoreGive(_mutex);
}

size_t Logger::getLogCount() {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  size_t count = bufferFull ? MAX_LOGS : logs.size();
  if (_mutex) xSemaphoreGive(_mutex);
  return count;
}
