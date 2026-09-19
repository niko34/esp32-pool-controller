#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <functional>
#include <freertos/semphr.h>
#include <FS.h>
#include <atomic>
#include "constants.h"
// feature-129 (phase A) : LogLevel, LogSlot (POD 20 o, `.bss`, phase C) et
// kLogFlagTruncated vivent dans le module PUR (aucune dépendance Arduino),
// réexportés ici pour tout le reste du firmware. Ne rien redéfinir ici.
#include "log_arena_logic.h"

/**
 * @brief Vue non-propriétaire sur UN message de journal, DESTINÉE AU CALLBACK
 *        temps réel (WsManager::broadcastLog) et à `copyEntryByIndex()`.
 *
 * `message`/`len` ne pointent JAMAIS dans `sLogArena` (logger.cpp, phase C) :
 * ils pointent soit dans le tampon de l'APPELANT de `log()` (callback, valide
 * pendant tout l'appel synchrone du callback — l'appelant est bloqué dans
 * `log()`), soit dans le tampon fourni par l'appelant de `copyEntryByIndex()`
 * (valide tant que ce tampon vit). C'est la condition C3 (feature-129) :
 * aucune vue non propriétaire de l'arène ne doit survivre hors mutex.
 */
struct LogRecord {
  uint32_t timestamp = 0;   // millis() au moment de l'écriture
  uint32_t wallSec = 0;     // secondes epoch LOCALES (0 = horloge non fixée)
  LogLevel level = LogLevel::INFO;
  uint8_t flags = 0;        // kLogFlagTruncated, etc.
  const char* message = nullptr;
  size_t len = 0;
};

/**
 * @brief Vue non-propriétaire sur un message de journal DANS `sLogArena`.
 *
 * Type DISTINCT de `LogRecord` (AUCUNE relation d'héritage) : produit
 * UNIQUEMENT par les lecteurs internes exécutés SOUS MUTEX
 * (`forEachRecentLog`) — `message` n'est valide QUE pendant l'appel du
 * visiteur. Passer un `LogArenaView` là où un `LogRecord` est attendu (ou
 * inversement) exige une construction EXPLICITE champ par champ, jamais une
 * conversion implicite : c'est ce qui rend visible en revue toute tentative
 * de faire fuir un pointeur d'arène hors mutex (condition C3, feature-129).
 */
struct LogArenaView {
  uint32_t timestamp = 0;
  uint32_t wallSec = 0;
  LogLevel level = LogLevel::INFO;
  uint8_t flags = 0;
  const char* message = nullptr;  // pointe dans sLogArena — SOUS MUTEX SEULEMENT
  size_t len = 0;
};

/**
 * @brief Nombre de flushs de journal refusés pour FAMINE MÉMOIRE depuis le boot
 *        (bug-100, ADR-0070). Les lignes ne sont pas perdues : le curseur de
 *        flush n'avance pas, la tentative est reportée au prochain passage.
 */
uint32_t loggerFopenDeferralCount();

// =============================================================================
// Compteurs de l'arène circulaire (feature-129, phase C, C5/C6) — atomiques,
// jamais journalisés dans leur propre chemin d'échec (ferme la récursion
// systemLogger → broadcastLog → _heapAllowsPush → systemLogger).
// =============================================================================

/** @brief Messages tronqués (dépassement de kLogMessageMaxBytes) depuis le boot. */
uint32_t logArenaTruncations();
/** @brief Créneaux (LogSlot) évincés de l'arène depuis le boot, tous motifs confondus. */
uint32_t logArenaEvictions();
/** @brief Créneaux évincés AVANT d'avoir été persistés alors qu'ils auraient dû
 *  l'être (C6) — DISTINCT de logArenaEvictions() : une ligne PERDUE pour
 *  /system.log, pas seulement une éviction RAM ordinaire. */
uint32_t logPersistOverruns();
/** @brief Nombre d'entrées actuellement vivantes dans l'arène (≤ kMaxLogEntries). */
uint32_t logEntriesCurrent();
/** @brief Octets actuellement occupés dans sLogArena (≤ kLogArenaBytes). */
uint32_t logArenaBytesUsed();
/** @brief Déclenchements du repli d'urgence de logArenaWriteOverlapsLive()
 *  (P2 pool-chemistry, feature-129) — DOIT rester à 0 en fonctionnement
 *  normal. Toute valeur non nulle signale que logArenaPlan()/
 *  logRingEvictCount() ont laissé passer un recouvrement que ce filet
 *  indépendant, exécuté juste avant le memcpy, a rattrapé en évinçant TOUT
 *  plutôt que d'écrire par-dessus un message vivant. */
uint32_t logArenaOverlapGuardTrips();

class Logger {
private:
  std::function<void(const LogRecord&)> _logCallback = nullptr;
  SemaphoreHandle_t _mutex = nullptr;
  // feature-027 : compteur d'entrées perdues sur timeout mutex (diagnostic, best-effort)
  uint32_t _droppedLogs = 0;

  // ===========================================================================
  // Arène circulaire (feature-129, option B′, phase C) — les objets `.bss`
  // (sLogArena/sLogSlots/sFlushLine) vivent en statique de fichier dans
  // logger.cpp ; la classe ne porte que les INDEX qui décrivent leur état.
  // ===========================================================================
  uint16_t _arenaHead = 0;    // prochain offset d'écriture dans sLogArena
  uint16_t _arenaTail = 0;    // offset de début du plus ancien message vivant
  uint16_t _arenaUsed = 0;    // octets RÉELLEMENT occupés (lève l'ambiguïté anneau vide/plein)
  size_t _slotOldestIdx = 0;  // index (dans sLogSlots) du créneau le plus ancien vivant
  size_t _slotWriteIdx = 0;   // index où sera écrit le PROCHAIN créneau
  size_t _slotLiveCount = 0;  // nombre de créneaux vivants (<= kMaxLogEntries)
  uint32_t _nextSeq = 0;      // numéro d'ordre ABSOLU (jamais réinitialisé)

  // Persistance sur LittleFS (partition history)
  fs::FS* _persistFs = nullptr;
  bool _persistEnabled = false;
  uint32_t _persistEnableSeq = 0;  // C7 : seq au moment de l'activation (rattrapage démarrage)
  uint32_t _flushCursor = 0;       // prochain seq à flusher vers /system.log
  std::atomic_flag _flushInFlight = ATOMIC_FLAG_INIT;
  std::atomic<bool> _flushRedo{false};
  unsigned long _lastFlushMs = 0;
  static constexpr unsigned long kFlushIntervalMs = 600000UL;   // 10 min (flush immédiat sur ERROR/CRITICAL)
  static constexpr size_t kMaxLogFileBytes = 8192;              // 8KB - budget partition history 64KB : pire cas log 2-3 blocs au lieu de 4-5 (ADR-0033)
  static constexpr size_t kRotateKeepBytes = 6144;              // Garde les 6 derniers KB à la rotation (ADR-0033)
  // bug-099 : chemins du journal persistant. Ici et NON dans constants.h — le
  // précédent (kMaxLogFileBytes, kRotateKeepBytes) est déjà posé, et le module
  // pur logger_logic n'a pas besoin des chemins pour décider.
  static constexpr const char* kSystemLogPath = "/system.log";
  static constexpr const char* kSystemLogTmpPath = "/system.log.tmp";

  // bug-099 : réconcilie le `.tmp` de rotation au démarrage (promotion du
  // survivant, ou effacement de l'orphelin). Appelée EN TÊTE de
  // setPersistenceFs(), avant toute ouverture du journal.
  void reconcileLogTmp();

  // Secondes epoch LOCALES (offset ajouté) au moment de l'appel, 0 si horloge
  // non fixée — voir logger.cpp pour le motif (logFormatHeader est un module
  // pur EN UTC NAIF, feature-129).
  uint32_t _localWallSec() const;

  // Un passage de flush complet (jusqu'à rattrapage ou timeout/refus). Appelé
  // en boucle par flushToDisk() tant qu'un passage supplémentaire est dû
  // (_flushRedo).
  void _flushOnePass();

public:
  Logger();  // Constructeur (plus rien à pré-allouer : tout est `.bss` statique)
  void begin();  // Initialise le mutex FreeRTOS (appeler depuis setup())

  // feature-129 (phase C) : `log()` n'exécute AUCUNE allocation dynamique —
  // capture src/len AVANT le mutex, `memcpy` sous mutex vers sLogArena.
  void log(LogLevel level, const char* message, size_t len);
  void log(LogLevel level, const String& message);  // délègue : log(level, m.c_str(), m.length())
  void debug(const char* message);
  void debug(const String& message);
  void info(const char* message);
  void info(const String& message);
  void warning(const char* message);
  void warning(const String& message);
  void error(const char* message);
  void error(const String& message);
  void critical(const char* message);
  void critical(const String& message);

  // Callback appelé à chaque nouveau log (utilisé par WsManager pour le push temps réel).
  // ⚠️ Reçoit un LogRecord dont `message` pointe dans le tampon de l'APPELANT de
  // log() — JAMAIS dans sLogArena (condition C3, feature-129). Ne pas retenir
  // le pointeur au-delà de l'appel du callback.
  void setLogCallback(std::function<void(const LogRecord&)> cb) { _logCallback = cb; }

  // feature-130 (ADR-0082) : getLevelString() supprimée — utiliser
  // logLevelName(level) (log_arena_logic.h, réexportée ci-dessus), qui
  // renvoie un `const char*` littéral au lieu d'une `String` allouée.

  // v2.8.2 : itération sous mutex SANS copie du texte (le visiteur reçoit un
  // pointeur DANS sLogArena, valide SOUS MUTEX SEULEMENT — feature-129 phase C).
  // Filtre optionnel : n'appelle le visiteur que pour les entrées dont le timestamp
  // est strictement supérieur à sinceTimestamp (0 = pas de filtre).
  // bug-crash-getlogs-badalloc : le visiteur retourne un bool — false = arrêt
  // anticipé de l'itération (pagination /get-logs : une seule prise de mutex
  // par page, jamais de double passage).
  // CONTRAINTE : le visiteur ne doit faire QUE des écritures mémoire rapides
  // (allocations BORNÉES tolérées, ex. JsonDocument d'une page ≤ ~4 Ko) — pas
  // d'I/O bloquante, pas de yield — car il s'exécute sous le mutex (scope minimal).
  // ⚠️ Tout stockage du `message` du LogArenaView DOIT en faire une COPIE
  // IMMÉDIATE (ex. `JsonString(v.message, v.len)`, jamais `o["x"] = v.message`
  // — ArduinoJson stocke un `const char*` PAR RÉFÉRENCE, sérialisé APRÈS la
  // libération du mutex).
  void forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                        const std::function<bool(const LogArenaView&)>& visitor) const;

  // bug-crash-getlogs-badalloc / feature-129 (phase C) : copie UNE entrée sous
  // mutex (index chronologique, 0 = la plus ancienne) DANS le tampon fourni par
  // l'appelant — jamais de pointeur vers sLogArena rendu à l'appelant. Le
  // message est clampé UTF-8 si `bufLen` est trop petit pour le contenir
  // (marqueur de troncature ADDITIONNEL porté par l'appelant, cf.
  // web_routes_data.cpp). Retourne false si idx >= nombre d'entrées, si
  // buf/bufLen est invalide, OU sur timeout mutex (kLoggerMutexTimeoutMs) —
  // indistinguables pour l'appelant (export best-effort). Utilisé par le
  // callback chunked de /download-logs : le ring peut tourner entre deux
  // appels (doublon/saut tolérés, diagnostic).
  bool copyEntryByIndex(size_t chronoIdx, LogRecord& out, char* buf, size_t bufLen) const;
  void clear();        // Vide uniquement le buffer RAM circulaire
  void clearAll();     // Vide RAM + supprime /system.log sur le FS persistant
  size_t getLogCount();

  // Persistance LittleFS
  void setPersistenceFs(fs::FS* fs);  // Appeler après montage de la partition history
  void update();                       // Appeler depuis la loop principale
  void flushToDisk();                  // Forcer un flush immédiat
  fs::FS* getPersistenceFs() const { return _persistFs; }
};

extern Logger systemLogger;

#endif // LOGGER_H
