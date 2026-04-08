#include "logger.h"
#include <time.h>

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

  // Bufferiser pour la persistance (hors DEBUG)
  if (_persistEnabled && level != LogLevel::DEBUG) {
    char timeBuf[20] = "????-??-??T??:??:??";
    time_t now = time(nullptr);
    if (now > 1609459200L) {
      struct tm t;
      localtime_r(&now, &t);
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &t);
    }
    String line = String(timeBuf) + " " + getLevelString(level) + " " + message + "\n";
    _persistBuffer.push_back(line);
  }

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

void Logger::setPersistenceFs(fs::FS* fs) {
  _persistFs = fs;
  _persistEnabled = true;
  _lastFlushMs = millis();

  // Écrire un marqueur de démarrage dans le fichier existant
  File f = fs->open("/system.log", "a");
  if (f) {
    char timeBuf[20] = "????-??-??T??:??:??";
    time_t now = time(nullptr);
    if (now > 1609459200L) {
      struct tm t;
      localtime_r(&now, &t);
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &t);
    }
    f.printf("--- DÉMARRAGE %s ---\n", timeBuf);
    f.close();
  }
}

void Logger::update() {
  if (!_persistEnabled) return;
  if (millis() - _lastFlushMs >= kFlushIntervalMs) {
    flushToDisk();
  }
}

void Logger::flushToDisk() {
  if (!_persistEnabled || !_persistFs) return;

  // Échanger le buffer sous mutex pour libérer rapidement
  std::vector<String> toWrite;
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  toWrite.swap(_persistBuffer);
  if (_mutex) xSemaphoreGive(_mutex);

  if (toWrite.empty()) {
    _lastFlushMs = millis();
    return;
  }

  // Écrire les entrées en append
  File f = _persistFs->open("/system.log", "a");
  if (!f) f = _persistFs->open("/system.log", "w");
  if (!f) {
    // Remettre le buffer si l'écriture échoue
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _persistBuffer.insert(_persistBuffer.begin(), toWrite.begin(), toWrite.end());
    if (_mutex) xSemaphoreGive(_mutex);
    return;
  }
  for (const String& line : toWrite) {
    f.print(line);
  }
  size_t fileSize = f.size();
  f.close();

  // Rotation si le fichier dépasse 32KB : garder les 24 derniers KB
  if (fileSize > kMaxLogFileBytes) {
    File rf = _persistFs->open("/system.log", "r");
    if (rf) {
      size_t sz = rf.size();
      // Sauter les premiers octets pour ne garder que kRotateKeepBytes
      rf.seek(sz > kRotateKeepBytes ? sz - kRotateKeepBytes : 0);
      // Avancer jusqu'à la prochaine fin de ligne pour éviter une ligne tronquée
      while (rf.available() && rf.peek() != '\n') rf.read();
      if (rf.available()) rf.read();  // Consommer le \n

      String kept = rf.readString();
      rf.close();

      File wf = _persistFs->open("/system.log", "w");
      if (wf) {
        wf.print(kept);
        wf.close();
      }
    }
  }

  _lastFlushMs = millis();
}
