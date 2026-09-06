#include "logger.h"
#include "config.h"
#include "constants.h"
#include "fs_guard.h"  // bug-100 : point d'admission unique des fopen internes (ADR-0070)
#include <atomic>
#include <time.h>

// bug-100 : flushs de journal refusés pour famine mémoire depuis le boot.
// `relaxed` : flushToDisk() s'exécute SUR LA TÂCHE QUI LOGUE (loopTask,
// mqttTask, async_tcp…) — il n'existe pas de « tâche du logger ».
static std::atomic<uint32_t> sLoggerFopenDeferrals{0};

uint32_t loggerFopenDeferralCount() {
  return sLoggerFopenDeferrals.load(std::memory_order_relaxed);
}

Logger systemLogger;

// Constructeur : pré-alloue le buffer pour éviter les réallocations
Logger::Logger() {
  logs.reserve(MAX_LOGS);
  // Pré-allouer le buffer de persistance tôt (heap frais) pour éviter les réallocations
  // tardives qui causeraient std::bad_alloc si le heap est fragmenté
  _persistBuffer.reserve(kMaxPersistBufferEntries);
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

  // feature-027 : prise bornée. Timeout → entrée perdue (politique silencieuse :
  // JAMAIS de systemLogger dans les chemins d'échec — récursion). Serial reste exécuté.
  const bool locked = (_mutex == nullptr) ||
                      (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) == pdTRUE);
  if (locked) {
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
      // Capacité fixe : supprimer la plus ancienne plutôt que de réallouer (évite std::bad_alloc)
      if (_persistBuffer.size() >= kMaxPersistBufferEntries) {
        _persistBuffer.erase(_persistBuffer.begin());
      }
      _persistBuffer.push_back(line);
    }

    if (_mutex) xSemaphoreGive(_mutex);
  } else {
    _droppedLogs++;  // Entrée perdue (buffer RAM + persistance)
  }

  // Push WebSocket temps réel (si callback enregistré) — hors mutex
  if (cb) cb(entry);

  // Flush immédiat sur ERROR/CRITICAL pour survivre aux crashes imminents
  if (_persistEnabled && (level == LogLevel::ERROR || level == LogLevel::CRITICAL)) {
    flushToDisk();
  }

  // Affichage série avec préfixe niveau — ligne complète en UNE écriture :
  // chaque write HAL est atomique (UART_MUTEX_LOCK) mais une séquence de print
  // peut s'entrelacer entre tâches (fragments tronqués observés, bug-spam-sensor-problem-serial).
  // Buffer pile : la console doit rester fonctionnelle même sous heap critique.
  char consoleLine[kLoggerConsoleLineMax];
  int written = snprintf(consoleLine, sizeof(consoleLine), "[%s] %s\r\n",
                         getLevelString(level).c_str(), message.c_str());
  if (written > 0) {
    size_t lineLen = static_cast<size_t>(written);
    if (lineLen >= sizeof(consoleLine)) {  // tronqué : garantir la fin de ligne
      consoleLine[sizeof(consoleLine) - 3] = '\r';
      consoleLine[sizeof(consoleLine) - 2] = '\n';
      lineLen = sizeof(consoleLine) - 1;
    }
    Serial.write(reinterpret_cast<const uint8_t*>(consoleLine), lineLen);
  }
}

void Logger::debug(const String& message) {
  if (!authCfg.debugLogsEnabled) return;  // feature-017 : court-circuit firmware si DEBUG désactivé
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

void Logger::forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                              const std::function<bool(const LogEntry&)>& visitor) const {
  if (!visitor) return;

  // feature-027 : timeout → aucune itération (silencieux, l'appelant peut réessayer)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;
  }

  // v2.8.2 : parcours EN PLACE, aucune copie. Le visiteur reçoit une référence
  // const sur chaque entrée et ne fait que des écritures mémoire rapides (contrat).
  // bug-crash-getlogs-badalloc : visiteur → false = arrêt anticipé (pagination).
  if (!bufferFull) {
    // Buffer pas encore plein : les dernières n entrées
    size_t start = logs.size() > count ? logs.size() - count : 0;
    for (size_t i = start; i < logs.size(); i++) {
      const LogEntry& e = logs[i];
      if (sinceTimestamp == 0 || e.timestamp > sinceTimestamp) {
        if (!visitor(e)) break;
      }
    }
  } else {
    // Buffer circulaire plein
    size_t itemsToReturn = count < MAX_LOGS ? count : MAX_LOGS;
    size_t startIndex = (currentIndex + MAX_LOGS - itemsToReturn) % MAX_LOGS;
    for (size_t i = 0; i < itemsToReturn; i++) {
      size_t idx = (startIndex + i) % MAX_LOGS;
      const LogEntry& e = logs[idx];
      if (sinceTimestamp == 0 || e.timestamp > sinceTimestamp) {
        if (!visitor(e)) break;
      }
    }
  }

  if (_mutex) xSemaphoreGive(_mutex);
}

bool Logger::getLogEntryByIndex(size_t chronoIdx, LogEntry& out) const {
  // bug-crash-getlogs-badalloc : copie d'UNE seule entrée (petite String) sous
  // mutex — jamais de copie du buffer entier. Timeout → false (best-effort).
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return false;
  }
  bool ok = false;
  size_t total = bufferFull ? MAX_LOGS : logs.size();
  if (chronoIdx < total) {
    // Index chronologique 0 = la plus ancienne. Buffer plein : currentIndex est
    // la prochaine position d'écriture, donc aussi l'entrée la plus ancienne.
    size_t idx = bufferFull ? (currentIndex + chronoIdx) % MAX_LOGS : chronoIdx;
    out = logs[idx];
    ok = true;
  }
  if (_mutex) xSemaphoreGive(_mutex);
  return ok;
}

void Logger::clear() {
  // feature-027 : timeout → no-op silencieux (rien n'est perdu : l'utilisateur
  // peut simplement re-cliquer — _droppedLogs ne compte que les entrées perdues)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;
  }
  logs.clear();
  currentIndex = 0;
  bufferFull = false;
  if (_mutex) xSemaphoreGive(_mutex);
}

void Logger::clearAll() {
  // Vider RAM + buffer pending sous mutex
  // feature-027 : timeout → return AVANT toute modif (ne pas supprimer le fichier
  // si la RAM n'a pas été vidée)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;
  }
  logs.clear();
  currentIndex = 0;
  bufferFull = false;
  _persistBuffer.clear();
  if (_mutex) xSemaphoreGive(_mutex);

  // Supprimer le fichier persistant (et son éventuel .tmp de rotation)
  if (_persistFs) {
    _persistFs->remove("/system.log");
    _persistFs->remove("/system.log.tmp");
  }
}

size_t Logger::getLogCount() {
  // feature-027 : timeout → 0 (silencieux)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return 0;
  }
  size_t count = bufferFull ? MAX_LOGS : logs.size();
  if (_mutex) xSemaphoreGive(_mutex);
  return count;
}

void Logger::setPersistenceFs(fs::FS* fs) {
  _persistFs = fs;
  _persistEnabled = true;
  _lastFlushMs = millis();

  // Écrire un marqueur de démarrage dans le fichier existant.
  // bug-100 / ADR-0070 — CLASSE A : setPersistenceFs() est appelé aujourd'hui
  // depuis loopTask au boot, mais RIEN ne le garantit ; on ne parie pas sur
  // l'absence de contention TCP pour un site qu'un futur appelant pourrait
  // déplacer. Refus → marqueur perdu, jamais d'abort().
  File f = openIfHeapAllows(fs, "/system.log", "a", kStaticFileMinAllocBytes);
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
  } else {
    // Logguer en Serial uniquement (pas de récursion possible ici)
    Serial.println("[LOGGER] ERREUR: impossible d'ouvrir /system.log au démarrage");
  }

  // Flush immédiat des logs accumulés avant le montage de la partition
  // Sans cela, les logs du démarrage restent en RAM pendant 10 minutes
  flushToDisk();
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
  // feature-027 : timeout → return SANS toucher _lastFlushMs (retente au tour suivant)
  std::vector<String> toWrite;
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;
  }
  toWrite.swap(_persistBuffer);
  if (_mutex) xSemaphoreGive(_mutex);

  if (toWrite.empty()) {
    _lastFlushMs = millis();
    return;
  }

  // bug-100 / ADR-0070 — ADMISSION MÉMOIRE UNIQUE, classe A (8 192).
  // Placée ICI, et pas ailleurs, pour trois raisons cumulées :
  //  1. APRÈS le retour anticipé `toWrite.empty()` → zéro mesure quand il n'y a
  //     rien à écrire (le cas courant, à la cadence de la loop) ;
  //  2. APRÈS le xSemaphoreGive ci-dessus → JAMAIS de parcours de tas
  //     (interruptions coupées) sous un mutex tenu ;
  //  3. UNE seule fois pour les SIX ouvertures qui suivent (append, repli,
  //     relecture de taille, source de rotation, .tmp en écriture, .tmp en
  //     relecture) : 6 mesures deviennent 1.
  // Classe A et non B : flushToDisk() s'exécute SUR LA TÂCHE QUI LOGUE — il
  // n'existe pas de « tâche du logger » — et logger.cpp:71-73 le déclenche en
  // synchrone sur chaque ERROR/CRITICAL, donc potentiellement depuis async_tcp
  // pendant qu'une réponse est en vol.
  if (!fsGuardAdmit(kStaticFileMinAllocBytes)) {
    sLoggerFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    // Serial UNIQUEMENT : journaliser ici rappellerait flushToDisk() (récursion
    // sur ERROR/CRITICAL) et rouvrirait le chemin même que l'on ferme.
    Serial.println("[LOGGER] Famine memoire : flush disque differe");
    // Réinsertion du buffer non flushé — même discipline que le chemin d'échec
    // d'ouverture ci-dessous : JAMAIS de réinsertion sans mutex.
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
      _droppedLogs += toWrite.size();
      Serial.printf("[LOGGER] Timeout mutex — %u ligne(s) de log perdue(s)\n",
                    (unsigned)toWrite.size());
      _lastFlushMs = millis();
      return;
    }
    _persistBuffer.insert(_persistBuffer.begin(), toWrite.begin(), toWrite.end());
    if (_mutex) xSemaphoreGive(_mutex);
    // Report explicite : nouvelle tentative au prochain kFlushIntervalMs. Sans
    // cela, update() rappellerait flushToDisk() à la fréquence de la loop
    // pendant toute la famine.
    _lastFlushMs = millis();
    return;
  }

  // Écrire les entrées en append. Admission déjà prononcée ci-dessus : les six
  // ouvertures qui suivent passent par le helper SANS re-mesurer.
  File f = openIfHeapAllows(_persistFs, "/system.log", "a", kFsGuardAlreadyAdmitted);
  if (!f) f = openIfHeapAllows(_persistFs, "/system.log", "w", kFsGuardAlreadyAdmitted);
  if (!f) {
    Serial.println("[LOGGER] ERREUR: impossible d'ouvrir /system.log pour écriture");
    // Remettre le buffer si l'écriture échoue
    // feature-027 : JAMAIS de réinsertion sans mutex — timeout → lignes perdues proprement
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
      _droppedLogs += toWrite.size();
      Serial.printf("[LOGGER] Timeout mutex — %u ligne(s) de log perdue(s)\n",
                    (unsigned)toWrite.size());
      return;
    }
    _persistBuffer.insert(_persistBuffer.begin(), toWrite.begin(), toWrite.end());
    if (_mutex) xSemaphoreGive(_mutex);
    return;
  }
  for (const String& line : toWrite) {
    f.print(line);
  }
  f.close();  // Fermer avant de lire la taille (LittleFS finalise les métadonnées au close)

  // Relire la taille après close pour avoir la valeur réelle
  {
    File fsize = openIfHeapAllows(_persistFs, "/system.log", "r", kFsGuardAlreadyAdmitted);
    if (!fsize) { _lastFlushMs = millis(); return; }
    size_t fileSize = fsize.size();
    fsize.close();

    // Rotation si le fichier dépasse 32KB : garder les 24 derniers KB
    // Utilise un fichier temporaire pour éviter la perte de données si le heap est insuffisant
    if (fileSize > kMaxLogFileBytes) {
      File rf = openIfHeapAllows(_persistFs, "/system.log", "r", kFsGuardAlreadyAdmitted);
      if (rf) {
        size_t sz = rf.size();
        size_t skipTo = sz > kRotateKeepBytes ? sz - kRotateKeepBytes : 0;
        rf.seek(skipTo);
        // Avancer jusqu'à la prochaine fin de ligne pour éviter une ligne tronquée
        while (rf.available() && rf.peek() != '\n') rf.read();
        if (rf.available()) rf.read();  // Consommer le \n

        // Écrire dans un fichier temporaire d'abord
        File wf = openIfHeapAllows(_persistFs, "/system.log.tmp", "w", kFsGuardAlreadyAdmitted);
        if (wf) {
          // Copie par blocs de 512 bytes pour ne pas allouer 24KB d'un coup
          uint8_t buf[512];
          while (rf.available()) {
            size_t n = rf.read(buf, sizeof(buf));
            if (n > 0) wf.write(buf, n);
          }
          wf.close();
          rf.close();

          // Remplacer seulement si le fichier temporaire n'est pas vide
          File check = openIfHeapAllows(_persistFs, "/system.log.tmp", "r", kFsGuardAlreadyAdmitted);
          bool tmpOk = check && check.size() > 0;
          if (check) check.close();

          if (tmpOk) {
            _persistFs->remove("/system.log");
            _persistFs->rename("/system.log.tmp", "/system.log");
          } else {
            _persistFs->remove("/system.log.tmp");
          }
        } else {
          rf.close();
        }
      }
    }
  }

  _lastFlushMs = millis();
}
