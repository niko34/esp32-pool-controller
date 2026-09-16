#include "logger.h"
#include "config.h"
#include "constants.h"
#include "fs_guard.h"  // bug-100 : point d'admission unique des fopen internes (ADR-0070)
#include "logger_logic.h"  // bug-099 : décisions PURES de réconciliation du .tmp (ADR-0017)
#include "utf8_clamp.h"    // feature-129 : clamp UTF-8 partagé (copyEntryByIndex)
#include <atomic>
#include <time.h>
#include <string.h>
#include <esp_task_wdt.h>

// =============================================================================
// Arène circulaire (feature-129, option B′, phase C) — TROIS objets `.bss`
// remplacent le `std::vector<LogEntry>` de `String` qui fractionnait le plus
// gros bloc libre du tas 8 bits (A1). Total attendu : 21 216 o.
//   sLogArena[kLogArenaBytes]      16 384 o — texte des messages, FIFO d'octets
//   sLogSlots[kMaxLogEntries]       4 000 o — descripteurs POD (20 o × 200)
//   sFlushLine[kLogPersistLineMax]    832 o — propriété EXCLUSIVE du flusher
// `log()` n'exécute donc plus aucune allocation dynamique : un `memcpy` sous
// le mutex existant, vers un tampon de taille FIXE réservé au link.
// =============================================================================
static char sLogArena[kLogArenaBytes];
static LogSlot sLogSlots[kMaxLogEntries];
static char sFlushLine[kLogPersistLineMax];

// bug-100 : flushs de journal refusés pour famine mémoire depuis le boot.
// `relaxed` : flushToDisk() s'exécute SUR LA TÂCHE QUI LOGUE (loopTask,
// mqttTask, async_tcp…) — il n'existe pas de « tâche du logger ».
static std::atomic<uint32_t> sLoggerFopenDeferrals{0};

uint32_t loggerFopenDeferralCount() {
  return sLoggerFopenDeferrals.load(std::memory_order_relaxed);
}

// feature-129 (phase C, C5/C6) — compteurs de l'arène. Jamais journalisés
// dans leur propre chemin d'échec (ferme la récursion systemLogger →
// broadcastLog → _heapAllowsPush → systemLogger, cf. heap_metrics.h).
static std::atomic<uint32_t> sLogArenaTruncations{0};
static std::atomic<uint32_t> sLogArenaEvictions{0};
static std::atomic<uint32_t> sLogPersistOverruns{0};
// P2 pool-chemistry (feature-129) : détecteur d'invariant qui survit en
// production. Compte les déclenchements du repli d'urgence de
// logArenaWriteOverlapsLive() — DOIT rester à 0 en fonctionnement normal ;
// toute valeur non nulle signale que logArenaPlan()/logRingEvictCount() ont
// laissé passer un recouvrement que ce filet indépendant a rattrapé.
static std::atomic<uint32_t> sLogArenaOverlapGuardTrips{0};
// Miroirs atomiques de _slotLiveCount/_arenaUsed (état protégé par le mutex
// du logger) : lisibles depuis n'importe quelle tâche (routes HTTP) SANS
// prendre ce mutex — même patron que heapLargestFreeBlock8Bit.
static std::atomic<uint32_t> sLogEntriesCurrentMirror{0};
static std::atomic<uint32_t> sLogArenaBytesUsedMirror{0};

uint32_t logArenaTruncations() { return sLogArenaTruncations.load(std::memory_order_relaxed); }
uint32_t logArenaEvictions()   { return sLogArenaEvictions.load(std::memory_order_relaxed); }
uint32_t logPersistOverruns()  { return sLogPersistOverruns.load(std::memory_order_relaxed); }
uint32_t logEntriesCurrent()   { return sLogEntriesCurrentMirror.load(std::memory_order_relaxed); }
uint32_t logArenaBytesUsed()   { return sLogArenaBytesUsedMirror.load(std::memory_order_relaxed); }
uint32_t logArenaOverlapGuardTrips() { return sLogArenaOverlapGuardTrips.load(std::memory_order_relaxed); }

Logger systemLogger;

// Constructeur : plus rien à pré-allouer (tout est `.bss` statique, zéro-
// initialisé par le runtime C++ avant main()).
Logger::Logger() {}

// Initialise le mutex FreeRTOS — appeler depuis setup() avant de démarrer le web server
void Logger::begin() {
  if (!_mutex) {
    _mutex = xSemaphoreCreateMutex();
  }
}

// ⚠️ Fuseau horaire (feature-129) : logFormatHeader() (log_arena_logic.cpp)
// fait une décomposition calendaire PURE, EN UTC NAIF, à dessein (pas de
// dépendance à l'état global TZ). C'est ICI, dans la coquille IMPURE, que
// l'offset local est ajouté — SANS timegm() (absente de ce toolchain,
// toolchain-xtensa-esp-elf 14.2.0 / picolibc restreint, vérifié à la
// compilation) : `gmtime_r()` décompose `now` en UTC, puis `mktime()`
// réinterprète CES CHAMPS UTC comme s'ils étaient LOCAUX (tm_isdst=-1, laisse
// mktime résoudre l'heure d'été — même patron que config.cpp:1208) pour
// obtenir l'offset local courant. `wallSec = now + offset` transporte cet
// offset jusqu'au module pur sans qu'il ait besoin de connaître TZ. Résultat
// visible identique à l'ancien code (localtime_r direct dans le formatage) :
// sans ce point, /system.log daterait tout en UTC et l'écart de 2 h ne se
// verrait dans aucun test natif.
uint32_t Logger::_localWallSec() const {
  time_t now = time(nullptr);
  if (now <= 1609459200L) return 0;  // horloge non fixée (même seuil que l'ancien code)
  struct tm utcTm;
  gmtime_r(&now, &utcTm);
  utcTm.tm_isdst = -1;
  const time_t asIfLocal = mktime(&utcTm);
  if (asIfLocal == static_cast<time_t>(-1)) return 0;  // défensif
  const time_t wallSec = now + (now - asIfLocal);
  return (wallSec > 0) ? static_cast<uint32_t>(wallSec) : 0;
}

void Logger::log(LogLevel level, const char* message, size_t len) {
  // ⚠️ Capturés AVANT toute prise de mutex (condition C3, feature-129) : le
  // callback WS reçoit CES pointeurs, jamais un pointeur dans sLogArena.
  const char* src = (message != nullptr) ? message : "";
  const size_t srcLen = (message != nullptr) ? len : 0;

  const uint32_t nowMs = millis();
  std::function<void(const LogRecord&)> cb = nullptr;
  bool haveEntry = false;
  uint32_t entryWallSec = 0;
  uint8_t entryFlags = 0;

  const bool locked = (_mutex == nullptr) ||
                      (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) == pdTRUE);
  if (locked) {
    const uint32_t wallSec = _localWallSec();

    // C1/C2 : troncature (si nécessaire) AVANT écriture, marqueur ASCII réservé
    // AVANT le clamp UTF-8 — logTruncatePlan() (log_arena_logic) fait tout ça.
    bool truncated = false;
    const size_t keep = logTruncatePlan(src, srcLen, kLogMessageMaxBytes, truncated);
    const size_t tailLen = truncated ? strlen(kLogTruncTail) : 0;
    const size_t finalLen = keep + tailLen;
    if (truncated) sLogArenaTruncations.fetch_add(1, std::memory_order_relaxed);

    // C4 : décision d'écriture PURE (où écrire, combien évincer).
    const LogArenaPlan plan = logArenaPlan(kLogArenaBytes, _arenaHead, _arenaTail,
                                           _arenaUsed, static_cast<uint16_t>(finalLen));

    // --- Éviction OCTETS (C4), PUIS éviction DESCRIPTEURS (kMaxLogEntries,
    //     contrainte indépendante de logArenaPlan qui ne raisonne qu'en octets) ---
    size_t evictCount = 0;
    if (plan.evictBytes > 0) {
      evictCount = logRingEvictCount(sLogSlots, kMaxLogEntries, _slotOldestIdx, plan.evictBytes);
      if (evictCount > _slotLiveCount) evictCount = _slotLiveCount;  // défensif
    }
    if (_slotLiveCount - evictCount >= kMaxLogEntries) {
      evictCount = _slotLiveCount - kMaxLogEntries + 1;
    }
    for (size_t i = 0; i < evictCount; ++i) {
      const LogSlot& victim = sLogSlots[_slotOldestIdx];
      // C6 : une ligne évincée AVANT d'avoir été flushée, alors qu'elle AURAIT
      // dû l'être, est une ligne PERDUE pour /system.log — distinct d'une
      // éviction RAM ordinaire (logArenaEvictions).
      if (victim.seq >= _flushCursor &&
          logShouldPersist(victim.seq, _persistEnableSeq, static_cast<LogLevel>(victim.level))) {
        sLogPersistOverruns.fetch_add(1, std::memory_order_relaxed);
      }
      _arenaUsed = static_cast<uint16_t>(_arenaUsed - victim.len);
      _slotOldestIdx = (_slotOldestIdx + 1) % kMaxLogEntries;
      --_slotLiveCount;
    }
    if (evictCount > 0) sLogArenaEvictions.fetch_add(evictCount, std::memory_order_relaxed);

    // --- Détecteur d'invariant qui survit en production (P2 pool-chemistry,
    //     feature-129) : AVANT tout memcpy, vérifier que le plan d'écriture
    //     ne recoupe aucun créneau ENCORE VIVANT. logArenaPlan() a déjà
    //     décidé l'éviction ci-dessus ; ce test est un filet INDÉPENDANT,
    //     pas une redite — l'incident du 16/09/2026 est passé inaperçu des
    //     cinq compteurs de l'arène alors qu'il recouvrait des messages
    //     vivants. Repli sûr si l'invariant est violé : on évince TOUT
    //     plutôt que d'écrire par-dessus un message de garde — perdre le
    //     journal vaut mieux que le falsifier (ADR-0072, un message de garde
    //     remplacé par un autre est pire que son absence).
    uint16_t writeOff = plan.writeOff;
    if (logArenaWriteOverlapsLive(sLogSlots, kMaxLogEntries, _slotOldestIdx,
                                  _slotLiveCount, writeOff, static_cast<uint16_t>(finalLen))) {
      sLogArenaOverlapGuardTrips.fetch_add(1, std::memory_order_relaxed);
      _slotOldestIdx = _slotWriteIdx;  // plus aucun créneau vivant
      _slotLiveCount = 0;
      _arenaUsed = 0;
      writeOff = 0;  // arène vidée : l'offset 0 est toujours sûr (finalLen <= kLogArenaBytes)
    }

    // --- Écriture : memcpy direct depuis `src` (tampon de l'appelant) vers
    //     l'arène — AUCUN tampon intermédiaire de kLogMessageMaxBytes. ---
    memcpy(&sLogArena[writeOff], src, keep);
    if (truncated) {
      memcpy(&sLogArena[writeOff + keep], kLogTruncTail, tailLen);
    }

    if (_slotLiveCount == 0) _slotOldestIdx = _slotWriteIdx;
    LogSlot& slot = sLogSlots[_slotWriteIdx];
    slot.timestamp = nowMs;
    slot.wallSec = wallSec;
    slot.seq = _nextSeq++;
    slot.off = writeOff;
    slot.len = static_cast<uint16_t>(finalLen);
    slot.level = static_cast<uint8_t>(level);
    slot.flags = truncated ? kLogFlagTruncated : 0;
    _slotWriteIdx = (_slotWriteIdx + 1) % kMaxLogEntries;
    ++_slotLiveCount;

    _arenaUsed = static_cast<uint16_t>(_arenaUsed + finalLen);
    _arenaHead = static_cast<uint16_t>(writeOff + finalLen);
    _arenaTail = sLogSlots[_slotOldestIdx].off;  // toujours valide : liveCount >= 1 ici

    sLogEntriesCurrentMirror.store(static_cast<uint32_t>(_slotLiveCount), std::memory_order_relaxed);
    sLogArenaBytesUsedMirror.store(_arenaUsed, std::memory_order_relaxed);

    cb = _logCallback;
    entryWallSec = wallSec;
    entryFlags = slot.flags;
    haveEntry = true;

    if (_mutex) xSemaphoreGive(_mutex);
  } else {
    _droppedLogs++;  // Entrée perdue (RAM + persistance)
  }

  // Push WebSocket temps réel (si callback enregistré) — HORS MUTEX.
  // ⚠️ rec.message pointe dans le tampon de l'APPELANT, jamais dans sLogArena.
  // Le faire pointer dans l'arène rouvrirait une lecture déchirée hors mutex
  // (condition C3, feature-129) : une autre tâche peut réécrire ces octets
  // pendant la sérialisation WS. Ce qui rend ça sûr : pendant le callback, la
  // tâche appelante est BLOQUÉE dans log() — elle ne peut donc pas réécrire
  // son propre tampon `src`.
  if (cb && haveEntry) {
    LogRecord rec;
    rec.timestamp = nowMs;
    rec.wallSec = entryWallSec;
    rec.level = level;
    // Le drapeau de troncature décrit la COPIE STOCKÉE dans l'arène, pas ce
    // que reçoit le callback : celui-ci lit le tampon appelant, donc le texte
    // INTÉGRAL, même quand la copie d'arène a été coupée à
    // kLogMessageMaxBytes. Propager entryFlags ici ferait afficher « tronqué »
    // un message complet — et, sur un refus d'injection, laisserait croire
    // qu'il manque une information qui est bien là. C'est /get-logs, qui lit
    // l'arène, qui porte le drapeau (champ "trunc").
    rec.flags = 0;
    rec.message = src;   // tampon APPELANT — JAMAIS sLogArena
    rec.len = srcLen;    // longueur ORIGINALE, cohérente avec flags = 0
    cb(rec);
  }

  // Flush immédiat sur ERROR/CRITICAL pour survivre aux crashes imminents
  if (_persistEnabled && (level == LogLevel::ERROR || level == LogLevel::CRITICAL)) {
    flushToDisk();
  }

  // Affichage série avec préfixe niveau — ligne complète en UNE écriture :
  // chaque write HAL est atomique (UART_MUTEX_LOCK) mais une séquence de print
  // peut s'entrelacer entre tâches (fragments tronqués observés, bug-spam-sensor-problem-serial).
  // Buffer pile : la console doit rester fonctionnelle même sous heap critique.
  // `src` est NUL-terminé (contrat des surcharges publiques : String::c_str()
  // ou littéral/char[] déjà terminés) — %s reste valide ici.
  char consoleLine[kLoggerConsoleLineMax];
  int written = snprintf(consoleLine, sizeof(consoleLine), "[%s] %s\r\n",
                         getLevelString(level).c_str(), src);
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

void Logger::log(LogLevel level, const String& message) {
  log(level, message.c_str(), message.length());
}

void Logger::debug(const char* message) {
  if (!authCfg.debugLogsEnabled) return;  // feature-017 : court-circuit firmware si DEBUG désactivé
  log(LogLevel::DEBUG, message, message ? strlen(message) : 0);
}
void Logger::debug(const String& message) {
  if (!authCfg.debugLogsEnabled) return;
  log(LogLevel::DEBUG, message);
}

void Logger::info(const char* message) {
  log(LogLevel::INFO, message, message ? strlen(message) : 0);
}
void Logger::info(const String& message) {
  log(LogLevel::INFO, message);
}

void Logger::warning(const char* message) {
  log(LogLevel::WARNING, message, message ? strlen(message) : 0);
}
void Logger::warning(const String& message) {
  log(LogLevel::WARNING, message);
}

void Logger::error(const char* message) {
  log(LogLevel::ERROR, message, message ? strlen(message) : 0);
}
void Logger::error(const String& message) {
  log(LogLevel::ERROR, message);
}

void Logger::critical(const char* message) {
  log(LogLevel::CRITICAL, message, message ? strlen(message) : 0);
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
                              const std::function<bool(const LogArenaView&)>& visitor) const {
  if (!visitor) return;

  // feature-027 : timeout → aucune itération (silencieux, l'appelant peut réessayer)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;
  }

  // v2.8.2 / feature-129 (phase C) : parcours EN PLACE, aucune copie du texte.
  // Le visiteur reçoit un LogArenaView (pointeur DANS sLogArena) — valide
  // SOUS CE MUTEX SEULEMENT. bug-crash-getlogs-badalloc : visiteur → false =
  // arrêt anticipé (pagination /get-logs).
  const size_t itemsToReturn = (count < _slotLiveCount) ? count : _slotLiveCount;
  const size_t startOffset = _slotLiveCount - itemsToReturn;  // les plus RÉCENTES
  for (size_t i = 0; i < itemsToReturn; ++i) {
    const size_t idx = (_slotOldestIdx + startOffset + i) % kMaxLogEntries;
    const LogSlot& slot = sLogSlots[idx];
    if (sinceTimestamp != 0 && slot.timestamp <= sinceTimestamp) continue;
    LogArenaView view;
    view.timestamp = slot.timestamp;
    view.wallSec = slot.wallSec;
    view.level = static_cast<LogLevel>(slot.level);
    view.flags = slot.flags;
    view.message = &sLogArena[slot.off];
    view.len = slot.len;
    if (!visitor(view)) break;
  }

  if (_mutex) xSemaphoreGive(_mutex);
}

bool Logger::copyEntryByIndex(size_t chronoIdx, LogRecord& out, char* buf, size_t bufLen) const {
  if (buf == nullptr || bufLen == 0) return false;
  // bug-crash-getlogs-badalloc / feature-129 : copie d'UNE seule entrée, sous
  // mutex, DANS le tampon de l'appelant — jamais de pointeur d'arène rendu.
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return false;
  }
  bool ok = false;
  if (chronoIdx < _slotLiveCount) {
    // Index chronologique 0 = la plus ancienne.
    const size_t idx = (_slotOldestIdx + chronoIdx) % kMaxLogEntries;
    const LogSlot& slot = sLogSlots[idx];
    size_t n = slot.len;
    bool clamped = false;
    if (n > bufLen - 1) {  // réserve toujours l'octet NUL du tampon appelant
      n = utf8ClampLen(&sLogArena[slot.off], bufLen - 1);
      clamped = true;
    }
    memcpy(buf, &sLogArena[slot.off], n);
    buf[n] = '\0';
    out.timestamp = slot.timestamp;
    out.wallSec = slot.wallSec;
    out.level = static_cast<LogLevel>(slot.level);
    out.flags = slot.flags | (clamped ? kLogFlagTruncated : 0);
    out.message = buf;  // pointe dans LE TAMPON DE L'APPELANT — jamais l'arène
    out.len = n;
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
  _slotOldestIdx = 0;
  _slotWriteIdx = 0;
  _slotLiveCount = 0;
  _arenaHead = 0;
  _arenaTail = 0;
  _arenaUsed = 0;
  sLogEntriesCurrentMirror.store(0, std::memory_order_relaxed);
  sLogArenaBytesUsedMirror.store(0, std::memory_order_relaxed);
  if (_mutex) xSemaphoreGive(_mutex);
}

void Logger::clearAll() {
  // Vider RAM sous mutex
  // feature-027 : timeout → return AVANT toute modif (ne pas supprimer le fichier
  // si la RAM n'a pas été vidée)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;
  }
  _slotOldestIdx = 0;
  _slotWriteIdx = 0;
  _slotLiveCount = 0;
  _arenaHead = 0;
  _arenaTail = 0;
  _arenaUsed = 0;
  _flushCursor = _nextSeq;  // plus rien à flusher de ce qu'on vient d'effacer
  sLogEntriesCurrentMirror.store(0, std::memory_order_relaxed);
  sLogArenaBytesUsedMirror.store(0, std::memory_order_relaxed);
  if (_mutex) xSemaphoreGive(_mutex);

  // Supprimer le fichier persistant (et son éventuel .tmp de rotation)
  if (_persistFs) {
    _persistFs->remove(kSystemLogPath);
    _persistFs->remove(kSystemLogTmpPath);
  }
}

size_t Logger::getLogCount() {
  // feature-027 : timeout → 0 (silencieux)
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return 0;
  }
  size_t count = _slotLiveCount;
  if (_mutex) xSemaphoreGive(_mutex);
  return count;
}

void Logger::reconcileLogTmp() {
  if (!_persistFs) return;

  // bug-100 / ADR-0070 — CLASSE B (kInternalFopenMinAllocBytes = 5 120), et non
  // A : en classe A on s'abstiendrait dans toute la bande 5 120–8 192 où le FS
  // est parfaitement sain, alors qu'AU BOOT « plus tard » N'EXISTE PAS. Une
  // abstention ici ne se rejoue jamais : le `.tmp` resterait orphelin à vie, et
  // dans l'état « journal absent » c'est la trace de la coupure qu'on perdrait.
  // L'absence de contention TCP est prouvée par l'ordre de setup() :
  // history.begin() (main.cpp:271) précède mqttManager.begin() et
  // webServer.begin(). UNE seule admission pour la sonde et l'appel FS.
  const bool admitted = fsGuardAdmit(kInternalFopenMinAllocBytes);

  // Court-circuit volontaire : sans admission, on ne sonde même pas — un
  // `exists()` OUVRE le fichier (cf. fs_guard.h), ce serait le fopen évité.
  const bool logPresent =
      admitted && existsIfHeapAllows(_persistFs, kSystemLogPath, kFsGuardAlreadyAdmitted);

  const LogTmpRecovery intent = evaluateLogTmpRecovery(admitted, logPresent);

  // La présence du `.tmp` n'est PAS sondée : elle se déduit du code de retour.
  bool fsCallSucceeded = false;
  switch (intent) {
    case LogTmpRecovery::Promote:
      // Journal absent + `.tmp` présent : coupure entre le remove() et le
      // rename() de la rotation. Le `.tmp` est complet et validé (tmpOk),
      // il détient l'unique copie des kRotateKeepBytes derniers octets.
      fsCallSucceeded = _persistFs->rename(kSystemLogTmpPath, kSystemLogPath);
      break;
    case LogTmpRecovery::Discard:
      // Journal courant présent : il fait foi, le `.tmp` est un déchet. On ne
      // touche NI au journal, NI à /history.json.
      fsCallSucceeded = _persistFs->remove(kSystemLogTmpPath);
      break;
    default:
      break;  // Abstain : aucun effet de bord, aucun appel FS.
  }

  switch (classifyLogTmpOutcome(intent, fsCallSucceeded)) {
    case LogTmpRecovery::Promote:
      // WARNING et JAMAIS error()/critical() : ceux-là flushent en synchrone
      // (logger.cpp, log()) donc rouvriraient /system.log PENDANT la
      // réconciliation (ADR-0033, anti-flush-storm). Pas de throttle : au plus
      // une ligne par démarrage, la rareté est structurelle.
      warning("Journal systeme recupere depuis le fichier temporaire de rotation "
              "(coupure pendant une rotation) — les dernieres lignes sont conservees");
      break;
    case LogTmpRecovery::Discard:
      warning("Fichier temporaire de rotation orphelin efface au demarrage — "
              "le journal courant est intact, la rotation se rejouera");
      break;
    default:
      // None (rien à réconcilier, premier boot compris) ET Abstain : SILENCE.
      // L'abstention reste MUETTE volontairement — history.cpp:332 émet déjà
      // son WARN de famine juste après, et l'arène RESTE la ressource sous
      // pression. L'observabilité durable de ce refus est fsGuardDeferralCount(),
      // pas une ligne de journal.
      break;
  }
}

void Logger::setPersistenceFs(fs::FS* fs) {
  _persistFs = fs;
  _persistEnabled = true;
  _lastFlushMs = millis();

  // bug-099 — ⚠️ ANCRAGE : ce bloc doit rester AVANT les lignes suivantes,
  // et l'ordre n'est pas cosmétique.
  //  1. `openIfHeapAllows(fs, kSystemLogPath, "a", ...)` juste dessous : le mode
  //     "a" CRÉE /system.log s'il est absent. Réconcilier après lui ferait
  //     toujours répondre « présent » à la sonde → la branche Promote devient
  //     INATTEIGNABLE et le survivant part en Discard : le journal est détruit
  //     par le code censé le sauver.
  //  2. `flushToDisk()` en fin de fonction : il peut relancer une rotation,
  //     donc recréer un `.tmp` — on ne réconcilie pas par-dessus.
  // Les warnings sont émis ici, APRÈS `_persistEnabled = true` et AVANT ce
  // flushToDisk(), pour être persistés dans le même appel.
  reconcileLogTmp();

  // C7 (feature-129, condition bloquante pool-chemistry) : RATTRAPAGE des
  // lignes de démarrage. Sans ceci, toutes les entrées écrites avant ce point
  // (dont le CRITICAL de config.cpp:379-380 — régulation pH fermée au
  // redémarrage, et pourquoi) ne sont JAMAIS persistées : `_flushCursor`
  // partait de 0 mais `logShouldPersist()` refusait tout seq < son ancienne
  // valeur. En fixant `_persistEnableSeq` au nombre d'entrées déjà poussées et
  // en réarmant `_flushCursor` à 0, le flush qui suit rattrape TOUT le
  // démarrage, filtré à WARNING/ERROR/CRITICAL (logShouldPersist) — sinon un
  // boot verse plus de kMaxLogFileBytes d'INFO et rotationne immédiatement,
  // emportant les lignes de sécurité qu'on vient d'écrire.
  _persistEnableSeq = _nextSeq;
  _flushCursor = 0;

  // Écrire un marqueur de démarrage dans le fichier existant.
  // bug-100 / ADR-0070 — CLASSE A : setPersistenceFs() est appelé aujourd'hui
  // depuis loopTask au boot, mais RIEN ne le garantit ; on ne parie pas sur
  // l'absence de contention TCP pour un site qu'un futur appelant pourrait
  // déplacer. Refus → marqueur perdu, jamais d'abort().
  File f = openIfHeapAllows(fs, kSystemLogPath, "a", kStaticFileMinAllocBytes);
  if (f) {
    char timeBuf[20] = "???\?-?\?-??T??:??:??";
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

  // Flush immédiat des logs accumulés (dont le rattrapage C7 ci-dessus) avant
  // le montage de la partition. Sans cela, les logs du démarrage resteraient
  // en RAM pendant 10 minutes.
  flushToDisk();
}

void Logger::update() {
  if (!_persistEnabled) return;
  if (millis() - _lastFlushMs >= kFlushIntervalMs) {
    flushToDisk();
  }
}

// Anti-chevauchement (bug ERROR synchrone vs flush périodique) : un flush
// déjà en vol (sur UNE AUTRE tâche) ne se superpose jamais à un second — les
// deux écriraient dans le MÊME sFlushLine. Ce risque est déjà présent
// aujourd'hui (le `swap` de l'ancien `_persistBuffer` le masquait sans le
// supprimer) ; ce drapeau garantit désormais l'exclusivité de sFlushLine.
void Logger::flushToDisk() {
  if (!_persistEnabled || !_persistFs) return;

  if (_flushInFlight.test_and_set(std::memory_order_acquire)) {
    // Un flush tourne déjà : on signale qu'un passage supplémentaire est dû,
    // et on rend la main immédiatement (jamais de flush concurrent).
    _flushRedo.store(true, std::memory_order_relaxed);
    return;
  }
  // Recommandation P2 pool-chemistry (feature-129) : boucle BORNÉE. Sous
  // rafale d'ERROR/CRITICAL venue d'une AUTRE tâche, _flushRedo peut rester
  // vrai en continu et enchaîner les passages complets (ouverture, écriture,
  // fermeture, stat) sur la tâche qui détient le drapeau — potentiellement
  // async_tcp ou mqttTask. esp_task_wdt_reset() n'est compté QUE dans
  // _flushOnePass (tous les 32 lignes) : une succession de passages COURTS ne
  // le déclenche jamais. On borne à kLoggerFlushRedoMaxPasses et on réarme le
  // watchdog à CHAQUE tour de boucle, pas seulement à l'intérieur d'un passage.
  size_t passes = 0;
  do {
    _flushRedo.store(false, std::memory_order_relaxed);
    _flushOnePass();
    esp_task_wdt_reset();
    ++passes;
  } while (_flushRedo.load(std::memory_order_relaxed) && passes < kLoggerFlushRedoMaxPasses);
  _flushInFlight.clear(std::memory_order_release);
}

void Logger::_flushOnePass() {
  // 0) Rien à drainer ? Vérification RAPIDE sous mutex, SANS admission FS —
  //    le cas courant (cadence de la loop, rien de neuf depuis le dernier
  //    passage) ne doit mesurer ni ouvrir quoi que ce soit.
  bool anythingToFlush;
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
    return;  // feature-027 : timeout → retente au prochain appel
  }
  anythingToFlush = (logNextFlushSlot(sLogSlots, kMaxLogEntries, _slotOldestIdx,
                                       _slotLiveCount, _flushCursor) != kMaxLogEntries);
  if (_mutex) xSemaphoreGive(_mutex);
  if (!anythingToFlush) {
    _lastFlushMs = millis();
    return;
  }

  // 1) Admission mémoire UNIQUE (classe A, 8 192) pour les ouvertures qui
  //    suivent — même motif que l'ancien code (6 ouvertures deviennent 1 mesure).
  if (!fsGuardAdmit(kStaticFileMinAllocBytes)) {
    sLoggerFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    // Serial UNIQUEMENT : journaliser ici rappellerait flushToDisk() (récursion
    // sur ERROR/CRITICAL) et rouvrirait le chemin même que l'on ferme.
    Serial.println("[LOGGER] Famine memoire : flush disque differe");
    // Le curseur n'a pas bougé : rien n'est perdu, le prochain appel reprendra
    // exactement là (l'arène EST la file d'attente désormais — plus de
    // _persistBuffer séparé à réinsérer, C6).
    _lastFlushMs = millis();
    return;
  }

  // 2) Ouverture UNIQUE, écriture LIGNE PAR LIGNE via le curseur, fermeture.
  File f = openIfHeapAllows(_persistFs, kSystemLogPath, "a", kFsGuardAlreadyAdmitted);
  if (!f) f = openIfHeapAllows(_persistFs, kSystemLogPath, "w", kFsGuardAlreadyAdmitted);
  if (!f) {
    Serial.println("[LOGGER] ERREUR: impossible d'ouvrir /system.log pour écriture");
    _lastFlushMs = millis();  // le curseur n'a pas bougé : rien perdu, on retentera
    return;
  }

  size_t linesSinceWdt = 0;
  for (;;) {
    size_t lineLen = 0;
    bool haveLine = false;

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(kLoggerMutexTimeoutMs)) != pdTRUE) {
      break;  // timeout : le curseur pointe déjà sur la suite, on s'arrête là
    }
    for (;;) {
      // ⚠️ MUTANT #10 (phase B) : la candidature est `seq >= _flushCursor`
      // (logNextFlushSlot), et l'avancement est `seq + 1` — jamais l'inverse,
      // sous peine de ligne dupliquée (avancement en dessous) ou sautée
      // (comparaison stricte à la candidature).
      const size_t idx = logNextFlushSlot(sLogSlots, kMaxLogEntries, _slotOldestIdx,
                                          _slotLiveCount, _flushCursor);
      if (idx == kMaxLogEntries) break;  // rattrapé : rien de plus à flusher
      const LogSlot& slot = sLogSlots[idx];
      if (!logShouldPersist(slot.seq, _persistEnableSeq, static_cast<LogLevel>(slot.level))) {
        _flushCursor = slot.seq + 1;  // filtrée (DEBUG, ou rattrapage sous WARNING) : pas écrite
        continue;
      }
      // En-tête + message COPIÉS dans sFlushLine (mutex tenu) — AUCUN tampon
      // de kLogMessageMaxBytes sur la pile : le message est lu DIRECTEMENT
      // depuis sLogArena vers sFlushLine (propriété exclusive du flusher).
      const int hn = logFormatHeader(sFlushLine, sizeof(sFlushLine), slot.wallSec,
                                     static_cast<LogLevel>(slot.level));
      size_t hLen = (hn > 0 && static_cast<size_t>(hn) < sizeof(sFlushLine))
                        ? static_cast<size_t>(hn) : sizeof(sFlushLine) - 1;
      size_t msgLen = slot.len;
      // ⚠️ L'ordre des deux tests n'est pas cosmétique. `sizeof - hLen - 2`
      // s'évalue en `size_t` NON SIGNÉ : si le repli défensif ci-dessus avait
      // porté hLen à sizeof-1, la soustraction passerait sous zéro et donnerait
      // un memcpy de ~4 Go. Une branche écrite POUR protéger transformerait
      // alors une condition bénigne en faute fatale. Inatteignable aujourd'hui
      // (logFormatHeader rend au plus 26), mais la garde ne coûte rien.
      if (hLen + 2 >= sizeof(sFlushLine)) {
        msgLen = 0;
      } else if (hLen + msgLen + 2 > sizeof(sFlushLine)) {  // +1 '\n' + 1 NUL
        msgLen = sizeof(sFlushLine) - hLen - 2;
      }
      memcpy(&sFlushLine[hLen], &sLogArena[slot.off], msgLen);
      sFlushLine[hLen + msgLen] = '\n';
      lineLen = hLen + msgLen + 1;
      _flushCursor = slot.seq + 1;
      haveLine = true;
      break;
    }
    if (_mutex) xSemaphoreGive(_mutex);
    if (!haveLine) break;  // drain terminé

    // Écriture disque HORS MUTEX.
    f.write(reinterpret_cast<const uint8_t*>(sFlushLine), lineLen);
    if ((++linesSinceWdt % 32) == 0) esp_task_wdt_reset();
  }
  f.close();  // Fermer avant de lire la taille (LittleFS finalise les métadonnées au close)

  // 3) Relire la taille après close pour avoir la valeur réelle, puis rotation
  //    si besoin — repris tel quel de l'ancien code, indépendant de la
  //    méthode d'écriture ci-dessus.
  {
    File fsize = openIfHeapAllows(_persistFs, kSystemLogPath, "r", kFsGuardAlreadyAdmitted);
    if (!fsize) { _lastFlushMs = millis(); return; }
    size_t fileSize = fsize.size();
    fsize.close();

    // Rotation au-delà de kMaxLogFileBytes : on ne garde que les
    // kRotateKeepBytes derniers octets. AUCUN chiffre en dur dans ce
    // commentaire — il a annoncé « 32KB / 24 derniers KB » pendant des mois
    // alors que les constantes valaient le quart de cela (bug-099).
    // Passe par un fichier temporaire pour éviter la perte de données si le
    // heap est insuffisant — mais l'enchaînement remove(log) + rename(tmp→log)
    // ci-dessous N'EST PAS atomique : la réconciliation de boot
    // (reconcileLogTmp) est ce qui rattrape une coupure dans cette fenêtre.
    if (fileSize > kMaxLogFileBytes) {
      File rf = openIfHeapAllows(_persistFs, kSystemLogPath, "r", kFsGuardAlreadyAdmitted);
      if (rf) {
        size_t sz = rf.size();
        size_t skipTo = sz > kRotateKeepBytes ? sz - kRotateKeepBytes : 0;
        rf.seek(skipTo);
        // Avancer jusqu'à la prochaine fin de ligne pour éviter une ligne tronquée
        while (rf.available() && rf.peek() != '\n') rf.read();
        if (rf.available()) rf.read();  // Consommer le \n

        // Écrire dans un fichier temporaire d'abord
        File wf = openIfHeapAllows(_persistFs, kSystemLogTmpPath, "w", kFsGuardAlreadyAdmitted);
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
          File check = openIfHeapAllows(_persistFs, kSystemLogTmpPath, "r", kFsGuardAlreadyAdmitted);
          bool tmpOk = check && check.size() > 0;
          if (check) check.close();

          if (tmpOk) {
            _persistFs->remove(kSystemLogPath);
            _persistFs->rename(kSystemLogTmpPath, kSystemLogPath);
          } else {
            _persistFs->remove(kSystemLogTmpPath);
          }
        } else {
          rf.close();
        }
      }
    }
  }

  _lastFlushMs = millis();
}
