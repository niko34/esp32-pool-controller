# Subsystem — `logger`

- **Fichiers** : [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- **Singleton** : `extern Logger systemLogger;`

## Rôle

Log central du firmware avec buffer circulaire en RAM + persistance sur la partition `history` + push temps réel via WebSocket. Niveaux : `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`.

## API publique

```cpp
void begin();
void log(LogLevel level, const String& message);
void debug(const String& message);
void info(const String& message);
void warning(const String& message);
void error(const String& message);
void critical(const String& message);

void setLogCallback(std::function<void(const LogEntry&)>);  // push WS temps réel
std::vector<LogEntry> getRecentLogs(size_t count = 50);
void clear();
size_t getLogCount();

// Persistance
void setPersistenceFs(fs::FS* fs);
void update();              // flush différé
void flushToDisk();         // flush immédiat
```

## Buffer circulaire RAM

`kMaxLogEntries = 200` ([`constants.h:46`](../../src/constants.h:46)). Stocké dans un `std::vector<LogEntry>` préalloué. Index circulaire `currentIndex`, flag `bufferFull` pour savoir si le buffer a fait au moins un tour.

## Persistance sur LittleFS (partition history)

Activable via `setPersistenceFs(LittleFS*)` après montage de la partition `history`. Paramètres :
- Flush périodique : **10 min** (`kFlushIntervalMs = 600000`)
- Taille max du fichier : **32 KB** (`kMaxLogFileBytes`)
- Rotation : quand le fichier dépasse 32 KB, tronqué aux **24 derniers KB** (`kRotateKeepBytes = 24576`).

Le tampon `_persistBuffer` (std::vector<String>) accumule entre deux flushes.

## Push WebSocket

`setLogCallback(cb)` permet à `ws_manager` de s'abonner. À chaque `log()`, le callback est invoqué **immédiatement** avec l'entrée, ce qui déclenche un push WS `{type: "log", timestamp, level, message}`.

## Concurrence

Mutex FreeRTOS (`_mutex`) protège :
- `logs` vector (lecture/écriture concurrente entre tâches et handlers web)
- `_persistBuffer` (flush concurrent)

## Endpoint HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Lire les logs récents | `GET /get-logs` | READ |
| Télécharger les logs persistés | `GET /download-logs` | READ |

## Usage typique

```cpp
systemLogger.info("Filtration started");
systemLogger.warning("pH out of range: " + String(ph, 2));
systemLogger.error("I2C timeout on ADS1115");
systemLogger.critical("Heap below 10KB, forcing restart");
```

## Cas limites

- **Mutex non initialisé** : `log()` avant `begin()` est silencieusement ignoré.
- **Partition history pleine** : `flushToDisk()` échoue, les logs restent en RAM jusqu'à rotation.
- **Callback non défini** : simplement pas de push WS, logs conservés en RAM quand même.
- **Heap critique** : le log reste fonctionnel tant que le vector préalloué peut absorber les `String`.

## Fichiers liés

- [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- [`src/constants.h:46`](../../src/constants.h:46) — `kMaxLogEntries`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — abonnement callback
