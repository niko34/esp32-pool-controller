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

// Itération en place des logs récents, SANS copie (v2.8.2)
void forEachRecentLog(size_t count, uint32_t sinceTimestamp,
                      const std::function<void(const LogEntry&)>& visitor);

void clear();      // vide uniquement le buffer RAM circulaire
void clearAll();   // vide RAM + _persistBuffer + supprime /system.log et /system.log.tmp
size_t getLogCount();

// Persistance
void setPersistenceFs(fs::FS* fs);
void update();              // flush différé
void flushToDisk();         // flush immédiat
```

## Toggle DEBUG runtime

`Logger::debug(const String&)` est conditionné par le booléen `authCfg.debugLogsEnabled` (default `false`, persisté en NVS sous la clé `debug_logs`). Le check est posé en **toute première ligne** de la fonction (early return) — aucune allocation `String`, aucun lock mutex, aucun push WS, aucune écriture fichier n'a lieu quand le toggle est désactivé.

```cpp
void Logger::debug(const String& msg) {
    if (!authCfg.debugLogsEnabled) return;  // early return — pas de coût
    log(LogLevel::DEBUG, msg);
}
```

Les autres niveaux (`info`, `warning`, `error`, `critical`) **ne sont pas affectés** par ce toggle.

Activation utilisateur : Paramètres → Avancé → card Logs → switch « Logs DEBUG activés ». Effet immédiat (pas de redémarrage). La valeur est lue à chaque appel à `debug()` depuis la variable globale `authCfg`, sans mutex (lecture booléenne atomique sur ESP32).

Default `false` choisi pour alléger le buffer en production : les logs `DEBUG` (`Diagnostic publié`, `Consolidation terminée: N points`, `MQTT publish drop`, etc.) ne saturent plus les 200 entrées de `kMaxLogEntries`. L'utilisateur active explicitement le switch en cas de session de diagnostic.

Le filtre UI `#log_level_debug` de la page Logs reste indépendant : il filtre l'affichage navigateur des entrées DEBUG déjà produites par le firmware. Les deux mécanismes sont complémentaires :

| Mécanisme | Effet |
|---|---|
| Switch « Logs DEBUG activés » (firmware) | Décide si `Logger::debug()` **produit** les entrées |
| Filtre `#log_level_debug` (UI) | Décide si l'UI **affiche** les entrées DEBUG produites |

## Buffer circulaire RAM

`kMaxLogEntries = 200` ([`constants.h:46`](../../src/constants.h:46)). Stocké dans un `std::vector<LogEntry>` préalloué. Index circulaire `currentIndex`, flag `bufferFull` pour savoir si le buffer a fait au moins un tour.

## Lecture des logs récents — `forEachRecentLog` (v2.8.2)

```cpp
void forEachRecentLog(size_t count, uint32_t sinceTimestamp,
                      const std::function<void(const LogEntry&)>& visitor);
```

Itère les `count` entrées les plus récentes (dont le `timestamp > sinceTimestamp`) **en place, sous mutex, sans copie**. Le `visitor` est appelé pour chaque entrée dans l'ordre chronologique, avec une **référence const** sur l'entrée stockée dans le buffer circulaire — aucune allocation, aucun `std::vector<LogEntry>` intermédiaire.

**Contrat mutex** : le `_mutex` est tenu pendant toute l'itération. Le `visitor` s'exécute donc **sous mutex** ; il doit rester court et ne **jamais** rappeler `systemLogger` (risque de deadlock / récursion). Les handlers HTTP `handleGetLogs` / `handleDownloadLogs` écrivent directement dans un `AsyncResponseStream` depuis le visitor.

### Contexte : correctif du crash `/logs` (`std::bad_alloc`)

Cette méthode **remplace** l'ancienne `std::vector<LogEntry> getRecentLogs(size_t)`. L'ancienne API **deep-copiait** tout le buffer (y compris les `String`) dans un vecteur retourné par valeur : sous fragmentation du tas, `operator new` levait `std::bad_alloc`, non rattrapée → `abort()` → PANIC → reboot. L'appareil devenait injoignable en HTTP alors que MQTT continuait de fonctionner (diagnostiqué via coredump). En itérant sans copie et en streamant la réponse, `forEachRecentLog` supprime cette allocation massive.

### Garde heap `kMinHeapForLogsResponse`

Constante `kMinHeapForLogsResponse = 24576` (24 Ko, [`constants.h`](../../src/constants.h)). Avant de servir `/get-logs` ou `/download-logs`, les handlers vérifient `heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)` (plus gros bloc contigu libre) ; s'il est en dessous du seuil, ils renvoient **503** (`{"error":"low_memory"}` en JSON pour `/get-logs`, message texte pour `/download-logs`) sans tenter la moindre allocation.

> ⚠️ **Limitation connue** : `/download-logs` peut **tronquer** l'export (jamais planter) si le tas est très fragmenté et que le buffer de flux ne peut plus grandir. Amélioration *chunked* prévue ultérieurement.

## Persistance sur LittleFS (partition history)

Activable via `setPersistenceFs(LittleFS*)` après montage de la partition `history`. Paramètres :
- Flush périodique : **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et le coredump couvrent les crashes.
- **Flush immédiat** sur `ERROR` et `CRITICAL` : `flushToDisk()` est appelé directement sans attendre l'intervalle périodique.
- Taille max du fichier : **16 KB** (`kMaxLogFileBytes`) — réduit depuis 32 KB pour respecter le budget de la partition `history` (64 KB).
- Rotation : quand le fichier dépasse 16 KB, tronqué aux **12 derniers KB** (`kRotateKeepBytes = 12288`).

Le tampon `_persistBuffer` (std::vector<String>) accumule entre deux flushes. Sa capacité est **bornée à 100 entrées** (`kMaxPersistBufferEntries`) et pré-allouée dans le constructeur (`reserve(kMaxPersistBufferEntries)`) pour éviter les `std::bad_alloc` en cas de heap fragmenté en production. Les entrées au-delà de 100 sont silencieusement ignorées (les logs restent dans le buffer RAM circulaire).

## Push WebSocket

`setLogCallback(cb)` permet à `ws_manager` de s'abonner. À chaque `log()`, le callback est invoqué **immédiatement** avec l'entrée, ce qui déclenche un push WS `{type: "log", timestamp, level, message}`.

## Concurrence

Mutex FreeRTOS (`_mutex`) protège :
- `logs` vector (lecture/écriture concurrente entre tâches et handlers web)
- `_persistBuffer` (flush concurrent)

### Timeouts mutex bornés (feature-027, v2.11.1)

Les **7 prises de mutex** du logger sont bornées par `kLoggerMutexTimeoutMs = 100 ms` ([`constants.h`](../../src/constants.h)) — valeur courte car toutes les sections critiques du logger sont en **RAM pure** (buffer circulaire, `_persistBuffer`), jamais d'I/O sous mutex.

**Politique silencieuse anti-récursion** : les chemins d'échec du logger n'appellent **JAMAIS** `systemLogger` (un log qui échoue à logger son propre échec = récursion). Seul un `Serial.printf` de secours existe sur le chemin de réinsertion de `flushToDisk()`.

Politique d'échec par site :

| Site | Sur timeout |
|---|---|
| `log()` | Entrée perdue (RAM + persistance) + `_droppedLogs++`. Le push WS n'a pas lieu (callback non capturé) |
| `forEachRecentLog()` | Aucune itération (le `visitor` n'est pas appelé — le client réessaiera) |
| `clear()` | No-op — rien n'est perdu, l'utilisateur peut re-cliquer. **Ne compte pas dans `_droppedLogs`** (corrigé post-revue : le compteur ne trace que des *entrées* perdues) |
| `clearAll()` | Return **avant** toute modification — le fichier `/system.log` n'est pas supprimé si la RAM n'a pas pu être vidée (pas d'état incohérent) |
| `getLogCount()` | Retourne `0` |
| `flushToDisk()` — swap du buffer | Return **sans toucher `_lastFlushMs`** → retente naturellement au prochain `update()` |
| `flushToDisk()` — réinsertion après échec d'écriture fichier | Lignes perdues proprement (`_droppedLogs += n`) + message `Serial` — jamais de réinsertion sans mutex |

**`_droppedLogs`** (`uint32_t`, membre privé) : compteur diagnostic des entrées perdues sur timeout. Il est **volontairement write-only** — aucun endpoint, aucun topic MQTT, aucun getter ne l'expose (mineure consignée en revue, assumée) : l'exposer créerait une API pour un événement qui ne doit jamais se produire en nominal ; il reste lisible au debugger / dans un coredump si un diagnostic de contention devient nécessaire.

## Endpoint HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Lire les logs récents | `GET /get-logs` | READ |
| Télécharger les logs persistés | `GET /download-logs` | READ |
| Effacer les logs côté ESP32 (RAM + fichier persistant) | `DELETE /logs` | WRITE |

`DELETE /logs` invoque `systemLogger.clearAll()` puis émet un INFO `Logs effacés (RAM + fichier persistant)` pour tracer l'action. Réponse JSON : `{"success": true}`.

## Logs WiFi

`setupWiFi()` ([`src/main.cpp`](../../src/main.cpp)) installe un handler `WiFi.onEvent()` après `WiFi.setSleep(false)` qui émet les messages suivants via `systemLogger` :

| Événement Arduino | Niveau | Message |
|---|---|---|
| `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` | WARN | `WiFi déconnecté (reason=N)` |
| `ARDUINO_EVENT_WIFI_STA_CONNECTED` | INFO | `WiFi associé à l'AP` |
| `ARDUINO_EVENT_WIFI_STA_GOT_IP` | INFO | `WiFi IP obtenue: x.x.x.x RSSI=-NNdBm` |
| `ARDUINO_EVENT_WIFI_STA_LOST_IP` | WARN | `WiFi IP perdue` |

Ces traces servent au diagnostic des blackouts réseau (ping qui ne répond plus, app web injoignable). La séquence observée renseigne sur la cause :

- **Drop Wi-Fi côté AP/routeur** : `DISCONNECTED → CONNECTED → GOT_IP`.
- **DHCP renew** : `LOST_IP → GOT_IP` (l'IP peut être identique ou différente).
- **Problème côté broker MQTT, firewall ou applicatif** : aucun log WiFi pendant le blackout, mais `mqtt_manager` enchaîne ses tentatives de reconnexion.

### Codes de raison Wi-Fi courants

Le `reason=N` du message `WiFi déconnecté` correspond aux constantes `WIFI_REASON_*` de l'IDF :

| Code | Constante | Cause typique |
|---|---|---|
| 4 | `AUTH_EXPIRE` | Timeout authentification |
| 8 | `ASSOC_LEAVE` | ESP32 a quitté volontairement (rare) |
| 15 | `4WAY_HANDSHAKE_TIMEOUT` | Problème WPA |
| 200, 201, 202 | `BEACON_TIMEOUT`, `NO_AP_FOUND`, `AUTH_FAIL` | RSSI faible / AP perdu |

Liste complète : [`esp_wifi_types.h`](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types.h).

### Ré-annonce mDNS sur changement d'IP

Depuis la v2.8.1, `poolcontroller.local` est ré-annoncé sur **tout changement d'IP**, pas seulement lors d'une reconnexion manuelle. Le handler `ARDUINO_EVENT_WIFI_STA_GOT_IP` compare la nouvelle IP à la dernière annoncée (`mdnsLastAnnouncedIp`) ; si elle diffère, il pose le drapeau `mdnsReannounceRequested`. Ce drapeau est consommé dans `checkSystemHealth()` (contexte `loopTask`, car MDNS/lwIP n'est pas thread-safe vis-à-vis de la tâche événements WiFi), qui exécute `MDNS.end()` + `begin()` + `addService(http)`. Une déconnexion (`STA_DISCONNECTED`) invalide l'IP mémorisée.

Ceci couvre le **roaming CPL/mesh sans perte de WiFi** : auparavant, la ré-annonce n'avait lieu que sur reconnexion, si bien que `poolcontroller.local` pouvait pointer vers une IP périmée (appareil injoignable en HTTP alors que MQTT continuait de fonctionner).

| Message | Niveau | Quand |
|---|---|---|
| `mDNS ré-annoncé sur x.x.x.x (changement d'IP)` | INFO | Après ré-annonce effective dans `checkSystemHealth()` |
| `Échec ré-annonce mDNS` | WARN | `MDNS.begin()` a échoué lors de la ré-annonce |

> Le getter `WsManager::clientCount()` (introduit en v2.8.1) reste disponible. Le log de diagnostic périodique `[Diag]` qui l'utilisait a été **retiré en v2.8.2** (temporaire, il avait servi au diagnostic du crash `/logs`).

## Logs MQTT

`MqttManager` ([`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)) émet les messages suivants pour le diagnostic des reconnexions :

| Message | Niveau | Quand |
|---|---|---|
| `MQTT déconnecté détecté — état=N` | WARN | Edge-triggered — au **front** de la transition `connecté → déconnecté`. `N` = `PubSubClient::state()` |
| `Tentative connexion MQTT (délai=Ns)...` | INFO | Avant chaque tentative `connect()` (rate-limit 5 s mini, jusqu'à 120 s en backoff) |
| `MQTT connecté !` | INFO | Connexion réussie |
| `MQTT échec DNS pour 'host' — prochaine tentative dans Ns` | ERROR | `WiFi.hostByName()` a échoué (hostname uniquement, pas IP littérale) |
| `MQTT échec, code=N — prochaine tentative dans Ns` | ERROR | Handshake CONNECT applicatif refusé (auth, protocole, etc.) |
| `MQTT déconnecté` | INFO | Appel explicite à `disconnect()` |

Le log `MQTT déconnecté détecté` permet de cibler immédiatement la cause d'une perte de connexion. Voir [`mqtt-manager.md`](mqtt-manager.md) section « Diagnostic — Codes d'état PubSubClient » pour la table des codes.

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
- **`_persistBuffer` saturé** : au-delà de `kMaxPersistBufferEntries = 100` entrées, les nouvelles entrées ne sont pas accumulées pour le prochain flush (elles restent dans le buffer RAM circulaire). Situation possible uniquement si le flush est bloqué pendant un temps anormalement long.
- **Callback non défini** : simplement pas de push WS, logs conservés en RAM quand même.
- **Heap critique** : le log reste fonctionnel tant que le vector préalloué peut absorber les `String`.

## Fichiers liés

- [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- [`src/constants.h:46`](../../src/constants.h:46) — `kMaxLogEntries`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — abonnement callback
