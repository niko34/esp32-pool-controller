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

/**
 * @brief Nombre de flushs de journal refusés pour FAMINE MÉMOIRE depuis le boot
 *        (bug-100, ADR-0070). Les lignes ne sont pas perdues : le buffer est
 *        réinséré et la tentative reportée au prochain `kFlushIntervalMs`.
 */
uint32_t loggerFopenDeferralCount();

class Logger {
private:
  static const size_t MAX_LOGS = kMaxLogEntries;
  std::vector<LogEntry> logs;
  size_t currentIndex = 0;
  bool bufferFull = false;
  std::function<void(const LogEntry&)> _logCallback = nullptr;
  SemaphoreHandle_t _mutex = nullptr;
  // feature-027 : compteur d'entrées perdues sur timeout mutex (diagnostic, best-effort)
  uint32_t _droppedLogs = 0;

  // Persistance sur LittleFS (partition history)
  fs::FS* _persistFs = nullptr;
  bool _persistEnabled = false;
  std::vector<String> _persistBuffer;
  unsigned long _lastFlushMs = 0;
  static constexpr unsigned long kFlushIntervalMs = 600000UL;   // 10 min (flush immédiat sur ERROR/CRITICAL)
  static constexpr size_t kMaxPersistBufferEntries = 100;       // Capacité fixe, pré-allouée dans le constructeur
  static constexpr size_t kMaxLogFileBytes = 8192;              // 8KB - budget partition history 64KB : pire cas log 2-3 blocs au lieu de 4-5 (ADR-0033)
  static constexpr size_t kRotateKeepBytes = 6144;              // Garde les 6 derniers KB à la rotation (ADR-0033)

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

  // v2.8.2 : itération sous mutex SANS copie (remplace getRecentLogs qui copiait
  // tout le buffer dans un std::vector<LogEntry> — copie profonde des String →
  // std::bad_alloc → abort sous fragmentation du tas). Le visiteur est appelé une
  // fois par entrée récente (dans l'ordre chronologique), sous le mutex du logger.
  // Filtre optionnel : n'appelle le visiteur que pour les entrées dont le timestamp
  // est strictement supérieur à sinceTimestamp (0 = pas de filtre).
  // bug-crash-getlogs-badalloc : le visiteur retourne un bool — false = arrêt
  // anticipé de l'itération (pagination /get-logs : une seule prise de mutex
  // par page, jamais de double passage).
  // CONTRAINTE : le visiteur ne doit faire QUE des écritures mémoire rapides
  // (allocations BORNÉES tolérées, ex. JsonDocument d'une page ≤ ~4 Ko) — pas
  // d'I/O bloquante, pas de yield — car il s'exécute sous le mutex (scope minimal).
  void forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                        const std::function<bool(const LogEntry&)>& visitor) const;

  // bug-crash-getlogs-badalloc : copie UNE entrée sous mutex (index chronologique,
  // 0 = la plus ancienne). Retourne false si idx >= nombre d'entrées OU sur
  // timeout mutex (kLoggerMutexTimeoutMs) — indistinguables pour l'appelant
  // (export best-effort). Utilisé par le callback chunked de /download-logs :
  // le ring peut tourner entre deux appels (doublon/saut tolérés, diagnostic).
  bool getLogEntryByIndex(size_t chronoIdx, LogEntry& out) const;
  void clear();        // Vide uniquement le buffer RAM circulaire
  void clearAll();     // Vide RAM + buffer pending + supprime /system.log sur le FS persistant
  size_t getLogCount();

  // Persistance LittleFS
  void setPersistenceFs(fs::FS* fs);  // Appeler après montage de la partition history
  void update();                       // Appeler depuis la loop principale
  void flushToDisk();                  // Forcer un flush immédiat
  fs::FS* getPersistenceFs() const { return _persistFs; }
};

extern Logger systemLogger;

#endif // LOGGER_H
