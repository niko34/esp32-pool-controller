#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <time.h>  // time_t (kMinValidEpoch) — self-contained : ne dépend plus d'Arduino.h en amont

// =============================================================================
// GPIO PIN ASSIGNMENTS - PCB v2
// =============================================================================
// Mapping matériel pour le circuit imprimé v2. Voir feature-019 et
// docs/adr/0012-pcb-v2-gpio-mapping.md.
// Bascule unidirectionnelle : ce code n'est plus compatible avec le PCB v1.

// Pins actifs
constexpr uint8_t kBuiltinLedPin           = 2;   // LED bleue interne (status)
constexpr uint8_t kTempSensorPin           = 5;   // OneWire DS18B20 (eau + circuit, 2 sondes)
constexpr uint8_t kI2cSdaPin               = 21;  // I2C SDA — DS3231 + EZO pH + EZO ORP (default Arduino-ESP32)
constexpr uint8_t kI2cSclPin               = 22;  // I2C SCL — DS3231 + EZO pH + EZO ORP (default Arduino-ESP32)
constexpr uint8_t kPumpPhPin               = 25;  // Pompe doseuse pH (PWM via ledc)
constexpr uint8_t kFiltrationRelayPin      = 26;  // Relais filtration (actif haut)
constexpr uint8_t kLightingRelayPin        = 27;  // Relais éclairage (actif haut)
constexpr uint8_t kPumpOrpPin              = 33;  // Pompe doseuse ORP/chlore (PWM via ledc)
constexpr uint8_t kFactoryResetButtonPin   = 35;  // Bouton factory reset (input-only, pull-up externe 10kΩ — actif bas)

// Pins réservés feature-future — déclarés mais pas de pinMode() actif dans cette feature
constexpr uint8_t kRtcSqwPin               = 23;  // RTC DS3231 SQW (open-drain, INPUT_PULLUP attendu, future feature)
constexpr uint8_t kCtnAuxPin               = 32;  // CTN_AUX MOSFET 12V tableau (OUTPUT actif haut, future feature)
constexpr uint8_t kRtcIntPin               = 36;  // RTC DS3231 INT (input-only, pull-up externe 10kΩ, future feature)

// ============================================================================
// TIMING CONSTANTS - Intervalles et délais temporels
// ============================================================================

// Delays système
constexpr unsigned long kSerialInitDelayMs = 3000;        // Attente après Serial.begin()
constexpr unsigned long kLoopDelayMs = 10;                // Délai minimal entre cycles loop()
constexpr unsigned long kOtaYieldDelayMs = 1;             // Délai yield() pendant OTA

// Watchdog
constexpr unsigned long kWatchdogTimeoutSec = 30;         // Timeout watchdog en secondes

// Intervalles de mise à jour
constexpr unsigned long kMqttPublishIntervalMs = 10000;   // 10s - Publication état MQTT
constexpr unsigned long kHealthCheckIntervalMs = 60000;   // 60s - Vérification santé système
constexpr unsigned long kDiagnosticPublishIntervalMs = 300000; // 5min - Publication diagnostic MQTT

// Tâche dédiée MQTT (cf. ADR-0011) — isole les blocages réseau (TCP retransmits, DNS lwip)
// de la régulation pH/ORP et de la filtration. Voir docs/subsystems/mqtt-manager.md.
constexpr uint32_t kMqttTaskStackSize       = 8192;       // 8 KB - publishDiscovery() sérialise 17 JSON consécutifs
constexpr uint32_t kMqttTaskPriority        = 2;          // Bas, > IDLE, < tiT (lwip) et async_tcp
constexpr int      kMqttTaskCore            = 0;          // Core 0 (loopTask sur core 1) — répartit la charge réseau
constexpr uint32_t kMqttOutQueueLength      = 32;         // File sortante (publish) — ~3s de débit nominal
constexpr uint32_t kMqttInQueueLength       = 16;         // File entrante (commandes HA)
constexpr uint32_t kMqttTaskLoopTimeoutMs   = 100;        // Timeout xQueueReceive dans mqttTask (cadence mqtt.loop())
constexpr uint32_t kMqttOfflineFlushMs      = 1000;       // Timeout flush "status=offline" avant ESP.restart() (OTA)
constexpr uint32_t kMqttClientConnectTimeoutSec = 2;      // WiFiClient::setTimeout attend des SECONDES (Arduino-ESP32 6.9.0 — WiFiClient.cpp:327, _timeout = seconds*1000). 2 s borne SO_SNDTIMEO/SO_RCVTIMEO sur le client TCP de PubSubClient.
constexpr uint32_t kMqttSocketSendTimeoutMs = 500;        // SO_SNDTIMEO socket TCP — borne write() à 500 ms (PINGREQ ~100 ms suffit, publish massif borné). Voir feature-014 IT5 / ADR-0011.

// Intervalles capteurs (voir aussi sensors.cpp pour détails internes)
constexpr unsigned long kTempSensorIntervalMs = 2000;     // 2s - Lecture température DS18B20
constexpr unsigned long kPhOrpSensorIntervalMs = 5000;    // 5s - Lecture pH/ORP

// Délais de redémarrage
constexpr unsigned long kRestartAfterOtaDelayMs = 3000;   // 3s - Attente avant restart après OTA
constexpr unsigned long kRestartApModeDelayMs = 1000;     // 1s - Attente avant restart en mode AP

// Timeouts mutex
constexpr unsigned long kI2cMutexTimeoutMs = 2000;        // 2s - Timeout acquisition mutex I2C
constexpr unsigned long kConfigMutexTimeoutMs = 1000;     // 1s - Timeout acquisition mutex config
// feature-027 : bornage des prises de mutex (plus aucun portMAX_DELAY applicatif)
constexpr unsigned long kHistoryMutexTimeoutMs = 2000;    // 2s - Pire détenteur : consolidation + saveToFile LittleFS (~1-1,5 s)
constexpr unsigned long kLoggerMutexTimeoutMs = 100;      // 100ms - Sections critiques RAM pures (buffer circulaire logs)
constexpr size_t kLoggerConsoleLineMax = 512;             // Buffer pile d'émission console (1 ligne = 1 Serial.write atomique)
constexpr unsigned long kMutexTimeoutWarnThrottleMs = 60000; // 60s - Max 1 warn/min/site sur timeout mutex (statique locale par site)

// Sécurité - Factory reset bouton
constexpr unsigned long kFactoryResetButtonHoldMs = 10000; // 10s - Maintien bouton pour factory reset

// Sécurité chimique - Injection manuelle
// Borne supérieure de la durée d'une injection manuelle volumée (POST /ph/inject/start
// ou /orp/inject/start). Au-delà de 10 min, c'est probablement un usage non
// supervisé qui mérite d'être stoppé par sécurité — l'utilisateur peut toujours
// relancer une 2ᵉ injection si besoin réel de plus.
// Valeur précédente non bornée explicitement : 3600s = 1h, jugé trop long par
// pool-chemistry (risque de surdosage si filtration s'arrête en milieu de cycle).
constexpr int kManualInjectMaxDurationS = 600;             // 10 min

// Sécurité - Rate limiting
// 120 req/min = 2 req/s moyenne — couvre la navigation UI active normale
// (page /params ouverte + clics + polling /get-config + /data + /coredump/info).
// Reste une protection efficace contre les bots brute-force (auth) et DOS accidentel.
// Ancienne valeur 30 : trop basse, déclenchait des "Rate limit dépassé" en usage normal
// dès qu'on naviguait dans Paramètres → Diagnostic ou qu'on cliquait plusieurs boutons rapidement.
constexpr uint16_t kMaxRequestsPerMinute = 120;           // Limite globale par IP
constexpr unsigned long kRateLimitWindowMs = 60000;       // Fenêtre de rate limit (1 min)

// ============================================================================
// MEMORY & BUFFER CONSTANTS - Limites mémoire et buffers
// ============================================================================

// Limites de buffers
constexpr size_t kMaxConfigSizeBytes = 16384;             // 16KB - Taille max configuration JSON
constexpr size_t kMaxLogEntries = 200;                    // Nombre max d'entrées logs

// Historique de données
// 8h de données brutes (intervalle 5 min). 96 et non 72 (= 6h pile) depuis
// bug-history-doublons-troncature (revue 2.18.1) : la consolidation par créneau
// COMPLET exige qu'un point survive jusqu'à ~7h (fin de son heure + RAW_MAX_AGE
// 6h) — à 72, l'éviction affamait l'agrégation horaire. Impact budget partition
// (ADR-0033) : +24 points ≈ +2 Ko ≈ 1 bloc (pire cas 14/16, garde
// historySaveAdmit inchangée en filet).
constexpr size_t kMaxRawDataPoints = 96;
constexpr size_t kMaxHourlyDataPoints = 168;              // 7 jours de moyennes horaires (64KB partition)
constexpr size_t kMaxDailyDataPoints = 75;                // 75 jours de moyennes journalières

// Seuils mémoire
constexpr size_t kMinFreeHeapBytes = 10000;               // Seuil critique mémoire disponible (alerte « Mémoire faible » — INCHANGÉ)

// Auto-guérison mémoire (v2.9.1) — redémarrage propre sur fragmentation du tas.
// Le gel observé (web mort + logging arrêté, loopTask vivant) est causé par un plus
// gros bloc contigu qui s'effondre alors que le heap total reste correct.
// kRestartMaxAllocHeapBytes est comparé à ESP.getMaxAllocHeap() (plus gros bloc), et
// est DISTINCT de kMinFreeHeapBytes (alerte sur heap total, conservée telle quelle).
constexpr uint32_t kRestartMaxAllocHeapBytes = 8192;      // Plus gros bloc contigu < seuil → restart auto-guérison
constexpr uint32_t kLowHeapWarnBytes = 20000;             // Plus gros bloc < seuil → WARN + flush disque one-shot
constexpr unsigned long kHeapLogIntervalMs = 300000;      // 5min - cadence mini de la trace INFO « Heap: … » (anti-spam journal)
constexpr uint32_t kHeapLogDropBytes = 8192;              // Émet aussi la trace Heap si maxBloc chute de + de 8 Ko depuis la dernière émission
// Cadence PROPRE de la trace de calibrage « Heap 8bits: … » (checkHeapHealth).
// v2.20.2 : 5 min → 30 min. À 5 min elle coûtait ~2,3 Ko/h sur un journal
// persistant de 8 Ko et participait à ramener la rétention à ~30 min, ce qui a
// détruit le post-mortem du crash du 15/08. À 30 min : ~0,24 Ko/h.
constexpr unsigned long kHeap8LogIntervalMs = 1800000;    // 30min - cadence de la trace INFO « Heap 8bits: … »

constexpr uint8_t  kAutoHealDebounceChecks = 3;           // Ticks consécutifs sous seuil avant restart (anti oscillation)
constexpr uint32_t kAutoHealMinUptimeMs = 180000;         // 3 min - uptime minimal avant tout restart auto
constexpr uint8_t  kAutoHealMaxReboots = 3;               // Plafond de reboots auto dans la fenêtre → mode maintenance
constexpr uint32_t kAutoHealRebootWindowMs = 900000;      // 15 min - fenêtre anti-boucle (tumbling, ancrée au 1er reboot)

// Échantillonnage heap adaptatif (bug-badalloc-ws-broadcast) : quand maxBloc passe
// sous kLowHeapWarnBytes, la cadence de checkHeapHealth() passe de
// kHealthCheckIntervalMs (60 s) à cette valeur → le débounce (3 ticks) confirme un
// heap critique en ~15 s au lieu de 180 s, avant que le crash ne gagne la course.
constexpr uint32_t kHeapCheckFastIntervalMs = 5000;       // 5s - cadence rapprochée sous seuil d'alerte

// Garde heap des broadcasts WebSocket (bug-badalloc-ws-broadcast) : textAll() alloue
// un shared buffer (makeSharedBuffer) SANS rattraper std::bad_alloc → abort(). Sous
// ce seuil de plus gros bloc contigu, le push est SAUTÉ (l'UI rate un cycle de 5 s,
// sans coût UX) et le débounce auto-guérison se joue.
// bug-heap-toctou (coredump 30/07) : relevé 8192 → 16384 = marge 2× vs
// kRestartMaxAllocHeapBytes (8192). L'ancien alignement des deux seuils créait une
// zone grise 8-16 Ko où les pushes étaient autorisés ET l'auto-guérison ne s'armait
// pas — le TOCTOU (async_tcp consomme le bloc mesuré entre le check et l'alloc de
// textAll) suffisait alors à provoquer le bad_alloc. Avec 16384, les pushes
// s'arrêtent AVANT la zone rouge et l'auto-guérison garde sa fenêtre d'action.
constexpr uint32_t kWsPushMinAllocBytes = 16384;          // Plus gros bloc < seuil → push WS sauté
constexpr uint32_t kWsSkipWarnThrottleMs = 30000;         // 30s - throttle du warn « pushes WS sautés »

// Buffer STATIQUE (.bss) de construction des trames WS sortantes (bug-heap-toctou A2) :
// « réserver une fois, réutiliser toujours » — remplace la String temporaire par push
// (churn heap G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s). Dimensionné pour le
// plus gros payload (config ~2,3 Ko mesuré) + marge de croissance. Une trame qui ne
// tient pas est SAUTÉE (jamais tronquée) et comptée (_oversizedFrames).
constexpr size_t kWsFrameBufBytes = 3072;                 // Taille du buffer statique de trame WS

// Récolte des clients WS zombies (investigation churn-heap §G1/G2) : un client qui
// n'ACK plus conserve une file jusqu'à 32 buffers ~1,8 Ko connexion ouverte —
// générateur n°1 du churn heap. L'état capteurs étant idempotent (chaque push est un
// snapshot complet), un client qui n'a pas consommé 4 snapshots (~20 s) est gelé →
// fermeture. En complément : keepalive PING auto pour que les zombies meurent par
// timeout ACK, et setCloseClientOnQueueFull(true) (fermeture par le fork à la
// saturation de sa file interne).
// (Récolte des clients gelés : bornée côté fork via -DWS_MAX_QUEUED_MESSAGES=5,
// platformio.ini + setCloseClientOnQueueFull — pas de constante applicative.)
constexpr uint16_t kWsKeepAlivePeriodS = 30;              // 30s - période PING auto (keepAlivePeriod, unité: secondes)

// (kWsSensorJsonReserveBytes supprimé — bug-heap-toctou A2 : les trames WS sont
// construites dans le buffer statique kWsFrameBufBytes, plus de String par push.)

// Disjoncteur de boot-loop de l'init history (v2.10.0, feature-059) : un drapeau
// NVS (crash_cnt, namespace hist_meta) est posé avant le montage de la partition
// history et levé après init complète. Si le boot précédent a crashé pendant
// cette fenêtre kHistoryInitCrashThreshold fois consécutives, la partition est
// effacée avant remontage (corruption littlefs → panic non rattrapable à la 1ʳᵉ
// écriture). Indépendant de kAutoHealMaxReboots et du mode maintenance.
constexpr uint8_t kHistoryInitCrashThreshold = 2;             // Crashs consécutifs pendant l'init → effacement partition history
constexpr const char* kHistoryCrashCountNvsKey = "crash_cnt"; // Clé NVS du compteur (namespace hist_meta, clé part_sz intacte)

// Garde d'espace de saveToFile (bug-nospc-partition-history, ADR-0033) : le pire
// cas d'occupation de la partition 64 Ko (réécriture copy-on-write de
// /history.json + /system.log + .tmp de rotation + métadonnées) dépassait la
// capacité → NOSPC structurel puis div/0 littlefs (bug amont, issue #1063).
// L'ancien fichier est supprimé AVANT réécriture (divise le pic COW par 2) et
// l'admission compare libre + taille de l'ancien fichier à taille JSON + marge
// (historySaveAdmit, module pur).
constexpr size_t kHistorySaveSpaceMarginBytes = 8192;           // 2 blocs de 4096 o : compaction métadonnées near-full + arrondi bloc
constexpr unsigned long kHistorySaveWarnThrottleMs = 3600000UL; // 1 h - throttle des warn « partition pleine » et de l'INFO occupation FS

// Clés NVS du diagnostic NOSPC + dump de preuve (namespace hist_meta existant) :
constexpr const char* kHistoryNospcCountNvsKey  = "nospc_cnt"; // Compteur cumulé de saves refusés/échoués (uint32)
constexpr const char* kHistoryLastOkEpochNvsKey = "last_ok";   // Epoch du dernier save réussi (écrit sur échec UNIQUEMENT — usure NVS)
constexpr const char* kHistoryDumpCrc0NvsKey    = "dmp_crc0";  // CRC32 du bloc 0 dumpé par le disjoncteur (A1)
constexpr const char* kHistoryDumpCrc1NvsKey    = "dmp_crc1";  // CRC32 du bloc 1 dumpé par le disjoncteur (A1)
constexpr const char* kHistoryDumpHeadNvsKey    = "dmp_head";  // Blob : 16 premiers octets du bloc 0 (A1)

// Marge commune des gardes heap HTTP (pattern v2.10.1, généralisé par
// bug-crash-getlogs-badalloc) : la taille RÉELLE de la réponse (measureJson /
// String::reserve) + cette marge doit tenir dans le plus gros bloc contigu libre
// (ESP.getMaxAllocHeap()), sinon 503 propre — jamais de std::bad_alloc → abort().
// Remplace l'ancien seuil FIXE kMinHeapForLogsResponse (24 Ko), indépendant de la
// taille réelle de la réponse, donc insuffisant (2 coredumps le prouvent).
constexpr size_t kHttpHeapMarginBytes = 4096;

// Pagination de GET /get-logs (bug-crash-getlogs-badalloc) : page souple de 30
// entrées (~4 Ko de JSON au pire), extensible jusqu'au plafond dur UNIQUEMENT pour
// une queue d'entrées partageant la même milliseconde que la dernière incluse —
// le curseur ?since= étant strictement supérieur, les exclure les perdrait
// définitivement entre deux pages (règles d'admission : logs_page.h).
constexpr size_t kLogsMaxEntriesPerPage = 30;             // Limite souple (taille de page visée)
constexpr size_t kLogsPageHardCap = 60;                   // Plafond dur (coupe même une queue même-ms)

// Export /download-logs en flux chunked : longueur max d'une ligne formatée
// (horodatage + niveau + message — tronquée avec « … » au-delà). Passée en
// paramètre template à LogLineCarry<N> (logs_export_chunk.h, header pur).
constexpr size_t kLogExportLineMaxBytes = 256;

// Pire cas d'un point JSON de /get-history : mesuré 113-120 o sur le format
// réel (clés fixes 82 o + valeurs : timestamp 10, ph 4, orp 4, temp 4, 2 bools,
// granularity) — revue v2.10.4. bug-heap-toctou A3 : sert désormais de capacité
// du carry (LogLineCarry<N>) de la réponse CHUNKED — un point (ou l'en-tête/pied
// JSON) doit tenir dedans, jamais de JSON partiel émis.
constexpr size_t kHistoryPointJsonMaxBytes = 128;

// /get-history (bug-history-doublons-troncature, itération 2) : instantané
// filtré du buffer copié en UNE prise de mutex à l'ouverture de la requête
// (handler HTTP, tâche async_tcp). Timeout court : un 503 rare vaut mieux que
// 2 s d'async_tcp bloquée sur une consolidation. Remplace l'accès point par
// point sous mutex (kHistoryPointReadTimeoutMs/kHistoryChunkMaxRetries retirés)
// dont les timeouts en cours de flux tronquaient la réponse.
constexpr unsigned long kHistorySnapshotTimeoutMs = 100;

// bug-points-futurs : tolérance au-delà de « maintenant » avant de considérer
// un timestamp comme impossible (purgé au chargement). 1 h absorbe un décalage
// d'horloge bénin sans laisser passer les excursions franches.
constexpr unsigned long kHistoryFutureToleranceSeconds = 3600;

// Ring d'instrumentation heap en RAM RTC noinit (bug-heap-toctou A4) : cinétique
// d'effondrement du heap dans les ~4 dernières minutes avant un reset anormal
// (le tick santé 5-60 s la rate). 128 entrées × 8 o = 1 Ko RTC noinit ; encodage
// pur dans heap_ring_logic.h, coquille impure dans heap_ring.cpp.
constexpr size_t kHeapRingEntries = 128;                  // Capacité du ring (128 × 8 o = 1 Ko RTC noinit)
constexpr unsigned long kHeapRingSampleIntervalMs = 2000; // 2s - cadence d'échantillonnage depuis loop()
constexpr size_t kHeapRingDumpMaxEntries = 32;            // Entrées dumpées au boot post-crash (~64 s de cinétique)

// ============================================================================
// PARACHUTE OOM GLOBAL (bug-oom-accept-http-durcissement, crash du 11/08/2026)
// ============================================================================
// Coredump du 11/08 : std::bad_alloc non rattrapé dans async_tcp (operator
// new(356) à l'accept HTTP) → std::terminate → abort. Le parachute pré-alloue
// une réserve d'urgence au plus TÔT du setup (avant toute fragmentation) ; le
// std::set_new_handler la libère à la première famine → le new en cours est
// RE-TENTÉ et réussit (contrat new_handler) au lieu d'aborter. Logique pure :
// oom_guard_logic.h ; glue Arduino : oom_guard.{h,cpp}.

// Taille de la réserve d'urgence. 12 Ko : très au-dessus des petites allocs
// vitales (accept HTTP ~356 o, trames WS, String logs) et assez gros pour
// laisser à l'auto-guérison (kRestartMaxAllocHeapBytes, débounce ~15 s) le
// temps de déclencher un reboot PROPRE pendant la famine.
constexpr size_t kOomReserveBytes = 12288;

// Hystérésis de réarmement : la réserve est libérée quand maxBloc est ≈ épuisé
// (échec d'un new), mais n'est RE-allouée que lorsque le plus gros bloc contigu
// repasse au-dessus de ce seuil, pour ne pas osciller libération/réallocation
// autour du seuil de famine.
//
// bug-parachute-oom-jamais-rearme (2ᵉ crash du 13/08/2026, uptime 12,9 h) :
// ERREUR DE CALIBRAGE de la v2.18.5 — le seuil valait 32768 o alors que le plus
// gros bloc contigu de CET appareil en régime établi vaut 27-30 Ko (mesures
// terrain : 29 684 o à 33 min d'uptime, 27 584 o à 12,9 h). Le seuil était donc
// AU-DESSUS du régime normal : une fois la réserve consommée, elle n'était
// JAMAIS ré-allouée et le handler restait désenregistré → parachute one-shot en
// pratique, ce qui a laissé passer le bad_alloc de 156 o du 13/08 (tas pourtant
// comptabilisé sain : free=81 408 o, maxB=27 584 o).
// Nouvelle valeur = 2 × kOomReserveBytes : atteignable dans le régime réel tout
// en gardant une marge d'un bloc entier au-dessus de la taille à allouer.
//
// ÉCHELLE COMPLÈTE des seuils « plus gros bloc contigu » (ordre croissant).
// ATTENTION v2.20.2 : les seuils ne sont PLUS tous exprimés dans la même métrique.
// Seul le réarmement du parachute a basculé sur la mesure 8 bits RÉELLE
// (heapLargestFreeBlock8Bit) ; les autres restent sur la métrique BRUTE
// ESP.getMaxAllocHeap() par décision NO GO explicite du gate pool-chemistry —
// les basculer avec les seuils actuels rendrait le mode maintenance (= DOSAGE
// SUSPENDU) atteignable en régime normal, puisque 8 192 > minMaxBloc8 = 3 060.
//   [8 bits]    2 048 o (kOomFallbackRearmMinMaxAllocBytes) → réserve de REPLI
//   [8 bits]   24 576 o (kOomRearmMinMaxAllocBytes)         → réserve NOMINALE
//   [brute]     8 192 o (kRestartMaxAllocHeapBytes)         → reboot d'auto-guérison
//   [brute]    16 384 o (kWsPushMinAllocBytes)              → pushes WS sautés
//   [brute]    20 000 o (kLowHeapWarnBytes)                 → WARN heap + flush one-shot
constexpr uint32_t kOomRearmMinMaxAllocBytes = 24576;
constexpr unsigned long kOomRearmCheckIntervalMs = 10000;  // 10 s - cadence lente du réarmement (loop)

// Réserve de REPLI (2ᵉ palier) : si le bloc nominal de 12 Ko n'est pas allouable,
// le parachute s'arme quand même avec un bloc plus petit — « toujours armé »
// vaut mieux que « jamais réarmé » (défaut n°1 du 13/08). Les petites allocations
// vitales observées dans les trois crashs tiennent très largement dedans (accept
// HTTP 356 o, AsyncFileResponse 156 o, __cxa_allocate_exception 100 o).
// Le surclassement repli → nominal est fait par la loop dès que maxBloc repasse
// au-dessus de kOomRearmMinMaxAllocBytes ; JAMAIS l'inverse (pas d'oscillation).
//
// RECALIBRAGE v2.20.2 (bug-famine-8bits, bloc E1) — seuil 12288 → 2048 et taille
// 2048 → 1024. Les deux valeurs étaient calées sur ESP.getMaxAllocHeap() (FAUSSE :
// IRAM 32 bits comprise, figée à 27 636 o) alors que le tick lit désormais
// heapLargestFreeBlock8Bit(). Régime 8 bits RÉEL mesuré sur 5 h :
// maxBloc8 ∈ [4 084 ; 9 204], minMaxBloc8_boot = 3 060.
// INVARIANT C2 imposé par le gate pool-chemistry, vérifié ici :
//   (a) seuil de réarmement 2 048 < 3 060  → le palier de repli reste armable même
//       au pire creux observé depuis le boot (aucun trou de protection) ;
//   (b) taille allouée 1 024 ≤ 2 048 / 2   → allouer la réserve ne peut pas à soi
//       seul faire retomber le tas sous son propre seuil de réarmement ;
//   (c) 2 048 ≤ 4 084 = min(maxBloc8 observé en régime) → armable en pratique.
// kOomRearmMinMaxAllocBytes reste à 24 576 : DÉLIBÉRÉMENT inatteignable dans le
// régime fragmenté actuel (donc plus aucun malloc(12288) répété toutes les 10 s),
// mais le réarmement nominal automatique repart de lui-même si la fragmentation
// disparaît.
constexpr size_t kOomFallbackReserveBytes = 1024;
constexpr uint32_t kOomFallbackRearmMinMaxAllocBytes = 2048;

// Backoff du WARN différé « allocation heap échouée » (callback diagnostic
// heap_caps_register_failed_alloc_callback, mode flag-seul — drainé par loop).
// v2.20.2 : remplace le throttle FIXE kOomFailWarnThrottleMs (30 s), qui produisait
// ~120 WARN/h et faisait tourner le journal persistant (8 Ko) en 25-35 min — le
// post-mortem du crash du 15/08 a été détruit par notre propre journalisation.
// Escalade 30 s → 5 min → 30 min (saturation), retour au palier initial après une
// heure sans nouvel échec. Logique pure : oomFailWarnNextDelayMs().
constexpr unsigned long kOomFailWarnDelayInitialMs = 30000;    // 30 s - 1er palier (signal immédiat)
constexpr unsigned long kOomFailWarnDelayMidMs = 300000;       // 5 min - 2e palier
constexpr unsigned long kOomFailWarnDelayMaxMs = 1800000;      // 30 min - palier de saturation
constexpr unsigned long kOomFailWarnDelayResetMs = 3600000;    // 1 h sans échec → retour au palier initial

// ---------------------------------------------------------------------------
// Post-mortem d'allocation en RAM RTC + verdict (bug-parachute-oom-jamais-rearme)
// ---------------------------------------------------------------------------
// Départage les deux hypothèses laissées ouvertes par le crash du 13/08 :
//   (a) consommateur très rapide dans l'angle mort de 2 s de l'anneau heap,
//   (b) corruption du tas (comptabilité saine mais l'allocateur refuse 156 o).
// Règle du verdict (oomPostMortemVerdict, module pur) : une famine capable de
// refuser une PETITE allocation refuse TOUT — donc beaucoup d'échecs et une
// comptabilité écroulée. À l'inverse, 1-2 échecs isolés avec un plus gros bloc
// annoncé au moins kOomCorruptionSaneBlockRatio × la taille demandée est
// incohérent avec une famine → suspicion de corruption.
constexpr uint32_t kOomCorruptionMaxFails = 8;        // Au-delà : famine (l'allocateur refuse en série)
constexpr uint32_t kOomCorruptionSaneBlockRatio = 4;  // maxBloc ≥ 4 × taille demandée = comptabilité saine
// Fenêtre de RAFALE (revue v2.18.8) : le compteur d'échecs est cumulé sur tout
// le boot. Sans borne temporelle, quelques échecs bénins étalés sur des heures
// concluraient « famine » sur le scénario même que ce verdict doit trancher
// (13/08 : 2 échecs, tas sain). Une vraie famine refuse tout en quelques
// secondes — 60 s couvre largement l'épisode du 11/08 (~14 s).
constexpr uint16_t kOomCorruptionBurstSpanS = 60;     // Échecs nombreux ET resserrés → famine

// Contrôle d'intégrité périodique du tas (heap_caps_check_integrity_all) —
// détecteur direct de l'hypothèse (b). Cadence LENTE (5 min) : le parcours de
// tous les blocs coûte du temps CPU et prend les verrous de l'allocateur.
// AUTOLIMITATION (condition pool-chemistry C3) : WARN au-delà de
// kHeapIntegrityWarnDurationUs, désactivation DÉFINITIVE (+ WARN one-shot)
// au-delà de kHeapIntegrityMaxDurationUs — le contrôle ne doit jamais devenir
// lui-même une source de gigue pour la régulation ni pour le watchdog.
constexpr unsigned long kHeapIntegrityCheckIntervalMs = 300000; // 5 min - cadence du contrôle d'intégrité
constexpr uint32_t kHeapIntegrityMaxDurationUs = 20000;         // 20 ms - au-delà : contrôle désactivé définitivement
constexpr uint32_t kHeapIntegrityWarnDurationUs = 5000;         // 5 ms - au-delà : WARN (contrôle conservé)

// Garde des sessions TLS sortantes GitHub (/check-update, /download-update),
// évaluée AVANT la création de WiFiClientSecure — prédicat pur oomTlsGateReason.
//
// CRITÈRE 1 (fail-closed) : PSRAM LIBRE requise — la mesure passée au prédicat
// est ESP.getFreePsram(), PAS ESP.getPsramSize() (taille installée, constante et
// jamais décroissante : une PSRAM saturée ouvrirait la garde alors que mbedTLS
// retomberait sur la RAM interne). Mesure terrain du 12/08/2026 sur WROOM-32
// sans PSRAM en v2.18.5 : free=87708 o et maxBloc=34804 o PASSAIENT la garde
// bicritère, puis le handshake mbedTLS (qui réclame plusieurs blocs contigus
// d'environ 16 Ko) fragmentait le tas jusqu'à faire échouer une allocation de
// 100 o — soit la fenêtre exacte du crash bad_alloc du 11/08. Sans PSRAM cet
// état est le régime NORMAL après quelques minutes : la mise à jour en ligne est
// refusée d'emblée (BOM PCB v2 = WROVER-E : 8 Mo de PSRAM installés, ~4 Mo
// adressables par la fenêtre de cache), l'upload manuel de fichier reste
// disponible. Seuil à 1 Mo de LIBRE : large sur les ~4 Mo utilisables
// (avec SPIRAM_USE_MALLOC les allocations > 4 Ko y partent), rejette 0, une
// PSRAM résiduelle inutilisable ET une PSRAM saturée.
// NOTE : valeur JAMAIS mesurée sur WROVER réel — à confirmer à la mise en
// service (relever le getFreePsram() nominal avant de figer ce seuil).
constexpr uint32_t kOtaTlsMinPsramBytes = 1048576;         // 1 Mo - PSRAM LIBRE minimale pour tenter TLS sortant
// CRITÈRE 2 : bicritère heap historique (ADR-0032) — la session TLS GitHub coûte
// ~58 Ko répartis (2×16 Ko de buffers I/O + parse X509 + budget JSON 8 Ko) →
// exige heap TOTAL suffisant ET un plus gros bloc contigu décent. Valeurs
// INCHANGÉES depuis la v2.18.5 : elles n'ont plus à porter seules la protection
// (le critère PSRAM le fait) — à RE-MESURER au swap WROVER, où les buffers
// mbedTLS > 4 Ko partent en PSRAM.
constexpr uint32_t kCheckUpdateMinFreeHeapBytes = 65536;   // Heap libre total minimal pour tenter TLS
constexpr uint32_t kCheckUpdateMinMaxAllocBytes = 20480;   // Plus gros bloc contigu minimal pour tenter TLS

// Release notes GitHub relayées telles quelles par /check-update : le corps de
// release n'est pas borné côté GitHub → tronquer (suffixe « … ») pour borner la
// réponse HTTP construite en RAM.
constexpr size_t kOtaReleaseNotesMaxChars = 2048;

// Budget NET d'allocations du JsonDocument de /check-update (allocateur
// plafonné avec crédit à la libération, bug-checkupdate-heap-tls / ADR-0032).
// La réponse GitHub est parsée en flux avec filtre (7 champs retenus) : pic
// NET ~5-6 Ko pour une release normale (corps ≤ ~2,5 Ko + doublement transitoire
// du buffer de chaîne + pool de variants ~1 Ko) ; 8 Ko couvre large. Au-delà →
// NoMemory → 500 propre au lieu d'une consommation non bornée.
constexpr size_t kOtaCheckJsonBudgetBytes = 8192;

// ============================================================================
// SENSOR CONSTANTS - Paramètres capteurs
// ============================================================================

// Échantillonnage et filtrage
constexpr int kNumSensorSamples = 3;                      // Nombre d'échantillons pour médiane (impair)

// ADS1115 timing (8 SPS = 125ms par échantillon)
constexpr unsigned long kAds1115SampleTimeMs = 125;       // Temps par échantillon à 8 SPS
constexpr unsigned long kAds1115ThreeSamplesMs = 375;     // Temps pour 3 échantillons

// DS18B20 - Sondes OneWire (feature-020)
constexpr size_t kMaxDs18b20Sondes = 2;                   // 2 sondes : eau piscine + circuit interne
constexpr size_t kSondeAddrLen = 8;                       // Adresse ROM 1-Wire 64 bits

// Clés NVS pour les adresses ROM des sondes (8 octets binaires, putBytes/getBytes)
// Stockées dans le namespace "poolctrl" comme le reste de la config.
constexpr const char* kNvsKeyOwWaterAddr = "ow_water_addr";    // Adresse sonde eau piscine
constexpr const char* kNvsKeyOwCircuitAddr = "ow_circuit_addr"; // Adresse sonde circuit interne

// Conversion temps
constexpr unsigned long kMillisToSeconds = 1000;          // Conversion ms → s
constexpr unsigned long kSecondsPerMinute = 60;           // Secondes par minute
constexpr unsigned long kMillisToMinutes = 60000;         // Conversion ms → min
constexpr unsigned long kSecondsPerHour = 3600;           // Secondes par heure

// ============================================================================
// ATLAS EZO CONSTANTS - Modules Atlas Scientific EZO Embedded I²C (PCB v2)
// ============================================================================
// Voir feature-021 (migration ADS1115 → EZO) et docs/adr/0012-pcb-v2-gpio-mapping.md.
// Bus I²C partagé avec DS3231 (kI2cSdaPin / kI2cSclPin).

constexpr uint8_t  kEzoPhAddress              = 0x63;     // EZO pH I²C address (default Atlas)
constexpr uint8_t  kEzoOrpAddress             = 0x62;     // EZO ORP I²C address (default Atlas)
constexpr uint32_t kEzoReadDelayMs            = 900;      // Délai après commande R (lecture)
constexpr uint32_t kEzoCalDelayMs             = 900;      // Délai après commande Cal,*
constexpr uint32_t kEzoRtDelayMs              = 600;      // Délai après commande RT,<temp>
constexpr uint32_t kSensorStaleTimeoutMs      = 20000;    // 20 s : timeout lecture pH/ORP stale (pool-chemistry condition #1)
constexpr int      kEzoBusFailMaxConsecutive  = 2;        // 2 échecs consécutifs I²C → blocage dosage (pool-chemistry condition #5)
constexpr unsigned long kPhSlopeQueryIntervalMs = 86400000UL; // 24h - re-query Slope,? auto (feature-024 pente sonde pH)

// ============================================================================
// SENSOR FILTER CONSTANTS - Lissage mesures pH/ORP (feature-025 → feature-058)
// ============================================================================
// Chaîne de filtrage (feature-058, tendance centrale) :
//   brut → rejet vraie aberration (NaN / hors-plage / spike DUR isolé)
//        → médiane sur fenêtre LONGUE → EMA lente.
// La valeur filtrée = estimateur central COURANT, jamais figée (feature-058) :
//   l'oscillation légitime soutenue n'est PLUS rejetée (fini le rejet de « saut »
//   maxStep qui écartait un côté de l'enveloppe et faisait coller/geler la valeur).
// L'instabilité (inhibition dosage) est DÉCOUPLÉE de la valeur : elle est pilotée
// par la DISPERSION de la fenêtre brute acceptée (max−min), le latch EMI et les
// rejets consécutifs — voir SensorFilter::unstable().
// Centralisé ici pour ajustement terrain. Buffer FIXE (pas d'alloc dynamique).
// Validation pool-chemistry feature-058 (conditions C1..C6 non négociables).

// --- Fenêtre médiane LONGUE (tendance centrale) ---
// Capacité physique du buffer (dimensionne float _buffer[]). FIXE, jamais dépassée.
constexpr uint8_t kSensorFilterCentralWindowMax  = 16;       // Capacité physique buffer médian (FIXE)
// Fenêtre logique effective (identique pH et ORP). À 5 s/échantillon, retard médian
// ≈ (13/2)×5 ≈ 30 s, soit +15 s vs l'ancienne fenêtre de 7 (C6). Latence tolérée :
// pH/ORP varient lentement → réponse plus lente mais plus prudente (ne chasse pas le bruit).
constexpr uint8_t kSensorFilterCentralWindow     = 13;       // Fenêtre médiane logique (retard ≈30 s, +15 s vs v1)

constexpr float   kPhEmaAlpha                    = 0.10f;    // Coefficient EMA pH (lissage lent)
constexpr float   kOrpEmaAlpha                   = 0.08f;    // Coefficient EMA ORP (lissage lent)
constexpr float   kPhFilterMin                   = 0.0f;     // Plage plausible pH min
constexpr float   kPhFilterMax                   = 14.0f;    // Plage plausible pH max
constexpr float   kOrpFilterMin                  = -1000.0f; // Plage plausible ORP min (mV)
constexpr float   kOrpFilterMax                  = 1500.0f;  // Plage plausible ORP max (mV)
constexpr uint8_t kSensorFilterMaxConsecutiveRejects = 10;   // Rejets NaN/hors-plage consécutifs → instable

// --- Dispersion (drapeau d'instabilité, C1/C3) ---
// Si (max − min) de la fenêtre brute ACCEPTÉE et PLEINE dépasse ce cap → unstable().
// Caps maintenus SOUS l'amplitude EMI observée (feature-057) pour préserver
// l'inhibition actuelle de la régulation (C1) — ne PAS assouplir.
constexpr float   kPhFilterMaxDispersion         = 0.5f;     // pH : dispersion fenêtre → instable
constexpr float   kOrpFilterMaxDispersion        = 100.0f;   // mV : dispersion fenêtre → instable

// --- Spike DUR isolé (vraie aberration, C3) ---
// Un point unique tel que |brut − médiane| dépasse ce seuil est ÉCARTÉ (n'alimente
// pas le buffer, donc EXCLU du calcul de dispersion) et ne compte PAS comme instabilité.
// Bien au-dessus de l'amplitude d'oscillation légitime : ne rejette que l'aberration franche.
// MAINTENUS EN L'ÉTAT (verdict pool-chemistry P4, bug-filtre-ph-verrouille) : le
// seuil de rejet n'est PAS la cause du verrouillage — c'était l'absence de sortie
// de secours. L'élargir dégraderait la protection contre les vraies aberrations
// (feature-058) sans rien résoudre. La sortie de secours ci-dessous suffit.
constexpr float   kPhFilterSpikeReject           = 1.0f;     // pH : spike dur isolé écarté
constexpr float   kOrpFilterSpikeReject          = 300.0f;   // mV : spike dur isolé écarté

// --- Sortie de secours : ré-amorçage sur rejets de spike SOUTENUS ET COHÉRENTS ---
// (bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue, v2.20.3)
// Un échantillon rejeté comme spike n'alimente pas le buffer → la médiane ne peut
// plus évoluer → tout échantillon suivant est rejeté à son tour : le filtre se
// verrouille DÉFINITIVEMENT sur une médiane périmée (incident 2026-08-16, dosage
// pH ET chlore à l'arrêt ≥ 3 h). Le critère qui distingue une aberration réelle
// d'un échelon légitime de la grandeur mesurée est la COHÉRENCE MUTUELLE des
// rejets : N rejets successifs proches les uns des autres décrivent une nouvelle
// valeur, pas du bruit. Trois gardes cumulatives (pool-chemistry) encadrent le
// ré-amorçage pour qu'une sonde réellement HS reste fail-closed.
constexpr uint8_t  kSensorFilterSpikeRearmCount   = 12;        // Rejets cohérents consécutifs avant ré-amorçage (≈60 s à 5 s)
constexpr uint32_t kSensorFilterRearmCooldownMs   = 1800000UL; // 30 min entre deux ré-amorçages (anti-chasse au bruit)
// Plancher de gel de l'alimentation du filtre après un front montant d'eau : même
// si la coquille déclare déjà les mesures représentatives (délai de stabilisation
// utilisateur à 0), on ne laisse JAMAIS l'eau stagnante de la chambre de mesure
// entrer dans le filtre pendant la première minute de circulation.
constexpr uint32_t kSensorFilterGateFloorMs       = 60000UL;   // 60 s

// Cohérence exigée entre les N rejets (amplitude max−min de la série rejetée).
// Largement sous le seuil de spike : une série DISPERSÉE est du bruit, pas un échelon.
constexpr float    kPhFilterRearmCoherence        = 0.20f;     // pH
constexpr float    kOrpFilterRearmCoherence       = 40.0f;     // mV
// Bande admissible de la nouvelle valeur cible : hors de cette bande, la mesure
// n'est plus physiquement plausible pour un bassin → on REFUSE de ré-amorcer
// (fail-closed : sonde suspecte), on alarme, et le dosage reste bloqué.
// ATTENTION — ces bornes sont sur l'échelle BRUTE : le filtre travaille en amont
// de l'offset logiciel, qui n'est appliqué qu'en sortie d'accesseur (ADR-0039).
// La bande effective en pH CORRIGÉ est donc [6.50 + ph_offset, 8.20 + ph_offset].
constexpr float    kPhFilterRearmMin              = 6.50f;     // pH BRUT
constexpr float    kPhFilterRearmMax              = 8.20f;     // pH BRUT
constexpr float    kOrpFilterRearmMin             = 300.0f;    // mV
constexpr float    kOrpFilterRearmMax             = 950.0f;    // mV

// --- Alarme « filtre non prêt durablement » (passe 2) ---
// Un filtre non prêt alors que la filtration tourne = AUCUNE régulation. Au-delà
// de ce délai → critical + alerte visible, répétée périodiquement tant que dure
// la panne (l'absence totale de régulation ne doit jamais être silencieuse).
constexpr uint32_t kSensorFilterNotReadyAlarmMs   = 300000UL;  // 5 min
constexpr uint32_t kSensorFilterNotReadyRepeatMs  = 1800000UL; // 30 min
// Anti-abus de la réinitialisation manuelle exposée hors debug (passe 2) : un
// reset relance un warmup complet (dosage bloqué), il ne doit pas être spammé.
constexpr uint32_t kSensorFilterUiResetMinIntervalMs = 600000UL; // 10 min

// --- Latch EMI (persistance de l'inhibition) ---
// Un bruit de niveau EMI SOUTENU (dispersion > cap sur N fenêtres pleines consécutives)
// pose un latch "instable" collant, levé UNIQUEMENT par reset()/recalibration. Distingue
// un pic de dispersion ponctuel (inhibition transitoire) d'un défaut EMI persistant.
constexpr uint8_t kSensorFilterDispersionLatchCycles = 3;    // Fenêtres en dépassement avant latch EMI
// Auto-levée du latch EMI (feature v2.8.4) : nombre de fenêtres PLEINES consécutives
// PROPRES (disp <= cap ET zéro rejet) requis avant de relâcher le latch collant.
// 12 fenêtres ≈ 60 s ; + la purge de la fenêtre médiane (~13 échantillons ≈ 65 s)
// → ≈ 2 min de signal propre avant reprise du dosage. Asymétrie VOULUE vs
// kSensorFilterDispersionLatchCycles=3 : rapide à poser (défaut EMI), lent à lever
// (anti-flapping — N < 6 rouvrirait le dosage sur une accalmie transitoire d'EMI).
// Même N pour pH et ORP.
constexpr uint8_t kSensorFilterLatchClearCycles  = 12;      // Fenêtres propres avant auto-levée latch EMI
// Âge max de la dernière mesure valide pour que le filtre soit considéré "prêt".
// Au-delà, ready() repasse false (mesure trop ancienne → fail-closed dosage).
// 4 × kPhOrpSensorIntervalMs (5 s) = 20 s, cohérent avec kSensorStaleTimeoutMs.
constexpr uint32_t kSensorFilterMaxAgeMs         = 20000;    // 20 s

// Trace capteur (ring buffer RAM, feature-057) : chaque cycle EZO enregistre un
// échantillon (pH brut/corrigé/lissé, ORP brut/lissé, T° eau).
// feature-067 : +1 champ float (phCorrected) → 300 × 28 o ≈ 8,4 Ko (+1,2 Ko).
// Fenêtre glissante ≈ 25 min à kPhOrpSensorIntervalMs (5 s). Diagnostic pur.
constexpr size_t kSensorTraceBufferSize          = 300;
// Pagination de GET /debug/sensor_trace (bug-crash-sensor-trace-badalloc) :
// 50 échantillons/page ≈ 4,5 Ko de JSON depuis feature-067 (champ phCorrected) —
// pas d'allocation contiguë démesurée dans AsyncResponseStream (le heap
// fragmenté peut descendre sous 8 Ko, ADR-0029) ; la garde measureJson +
// kSensorTraceHeapMarginBytes reste le filet en cas de heap trop fragmenté.
constexpr size_t kSensorTraceMaxSamplesPerPage   = 50;
// Marge de sécurité entre la taille mesurée de la réponse (measureJson) et le
// plus gros bloc libre (ESP.getMaxAllocHeap()) : en dessous → 503 propre au
// lieu d'un std::bad_alloc → abort() sur la tâche async_tcp.
// Alias historique (v2.10.1) de la marge commune kHttpHeapMarginBytes — même
// valeur, zéro diff comportemental (bug-crash-getlogs-badalloc).
constexpr size_t kSensorTraceHeapMarginBytes     = kHttpHeapMarginBytes;

// ============================================================================
// OFFSET LOGICIEL DE MESURE pH (feature-067)
// ============================================================================
// pH corrigé = pH brut EZO + phOffset, appliqué EN SORTIE des accesseurs
// (cf. src/ph_offset.h). La calibration EZO n'est jamais touchée.

// Bornes de l'offset — arbitrage utilisateur validé sous conditions par
// pool-chemistry. Justification chimique : l'offset décale la consigne EFFECTIVE
// (cible 7,20 avec offset +0,40 → l'eau réelle est régulée à 6,80). ±0,50 pH
// couvre un décalage d'implantation de sonde ; au-delà, le défaut vient de la
// sonde ou de la calibration EZO et l'écart pH régulé / pH réel devient
// dangereux (corrosion côté bas, chlore inactif côté haut).
constexpr float kPhOffsetMin = -0.50f;
constexpr float kPhOffsetMax =  0.50f;

// --- Plancher dur d'injection d'acide sur le pH filtré BRUT (pool-chemistry #2) ---
// Filet de sécurité contre un offset POSITIF erroné : la régulation croit doser
// vers 7,20 alors que l'eau réelle est déjà sous 6,90. Sous ~6,8 l'eau devient
// agressive (attaque du liner, des joints, des parties métalliques et du filtre).
// Sous le plancher applicable, TOUTE injection d'acide est refusée, quelle que
// soit la valeur corrigée. Deux paliers : plus l'offset est large, plus la marge
// doit être grande (l'incertitude sur le pH réel croît avec |offset|).
// Inerte à offset 0 : à 6,80 brut la régulation acide n'a de toute façon rien à
// corriger (cible usuelle 7,0-7,4).
constexpr float kPhOffsetWideThreshold = 0.30f;  // Bascule de palier sur |offset|
constexpr float kPhRawDosingFloor      = 6.80f;  // |offset| <= kPhOffsetWideThreshold
constexpr float kPhRawDosingFloorWide  = 6.90f;  // |offset| >  kPhOffsetWideThreshold

// --- Inhibition du dosage ORP sur pH brut élevé (pool-chemistry #3) ---
// Filet de sécurité symétrique contre un offset NÉGATIF erroné : la régulation
// laisse alors le pH réel monter. Au-dessus de ~7,6 la fraction d'acide
// hypochloreux (HClO, forme active du chlore) s'effondre (< 25 %) : l'ORP mesuré
// chute à concentration de chlore constante et la régulation ORP compense en
// SURDOSANT le chlore. On suspend donc le dosage ORP tant que le pH filtré BRUT
// dépasse ce seuil — le vrai correctif est de faire baisser le pH.
constexpr float kPhRawOrpInhibit = 7.60f;

// --- Compteur de refus plancher (pool-chemistry #7) ---
// Un plancher qui se déclenche à répétition signe une sonde pH morte ou un
// offset saisi à l'envers, pas un incident ponctuel : log critical explicite +
// bascule fail-closed de la régulation pH en manuel.
constexpr uint8_t  kPhRawFloorRefusalMax      = 3;           // Refus tolérés dans la fenêtre
constexpr uint32_t kPhRawFloorRefusalWindowMs = 86400000UL;  // 24 h — fenêtre glissante

// Pause mélange hydraulique après injection (pool-chemistry feature-025).
// Distincte du timer post-calibration (_stabilizationEndMs). Gates indépendantes (OR).
// Empêche un surdosage avant homogénéisation du bassin. Gérée par timestamps (pas de delay()).
constexpr unsigned long kPhMixingDelayMs         = 900000UL;  // 15 min — pause mélange pH
constexpr unsigned long kOrpMixingDelayMs        = 1200000UL; // 20 min — pause mélange ORP

// ============================================================================
// SENSOR FROZEN DETECTION — Détection capteur figé (feature-022 Passe 2)
// ============================================================================
// Un module EZO peut répondre sans erreur I²C mais retourner indéfiniment la
// MÊME valeur (électronique interne figée, sonde HS côté BNC...). La panne
// franche (NaN / stale / bus dégradé) est déjà couverte ailleurs ; ici on
// détecte la variance nulle : N lectures ACCEPTÉES consécutives contenues dans
// une bande < epsilon → capteur figé → ready() = false → dosage bloqué
// (garde FilterNotReady existante, fail-closed).
//
// Choix d'epsilon = ½ LSB du capteur (validation pool-chemistry, condition #1) :
//   Un capteur VIVANT bruite d'au moins ±1 LSB (quantification + bruit sonde +
//   bruit thermique). Avec epsilon = ½ LSB, un simple toggle de 1 LSB — même en
//   arithmétique float32 — CASSE le run : aucune eau réelle, même parfaitement
//   stable, ne peut être déclarée figée tant que la dernière décimale bouge.
//   Consigne terrain : en cas de faux positifs, DURCIR N (allonger la fenêtre),
//   ne JAMAIS élargir epsilon.
constexpr uint16_t kSensorFrozenSamples    = 30;      // 30 lectures valides à 5 s = 2,5 min
constexpr float    kSensorFrozenEpsilonPh  = 0.0005f; // ½ LSB EZO pH (résolution 0.001 pH)
constexpr float    kSensorFrozenEpsilonOrp = 0.05f;   // ½ LSB EZO ORP (résolution 0.1 mV)
// Température : DS18B20 12 bits, LSB = 0.0625 °C → epsilon 0.03 < ½ LSB.
// Fenêtre longue (900 lectures à 2 s = 30 min) : l'eau d'un bassin varie
// lentement, mais jamais à 0.0625 °C près sur 30 min avec la sonde immergée.
// Warning-only : aucun impact dosage (effets réels limités à la compensation
// EZO pH et au planning auto de filtration).
constexpr uint16_t kTempFrozenSamples  = 900;   // 900 lectures valides à 2 s = 30 min
constexpr float    kTempFrozenEpsilonC = 0.03f; // < ½ LSB DS18B20 12 bits (0.0625/2)

// ============================================================================
// FILTRATION CONSTANTS - Paramètres filtration
// ============================================================================

// Pivot horaire pour calcul durée filtration
constexpr float kFiltrationPivotHour = 13.0f;             // 13h - Pivot pour calcul durée selon température

// feature-056 : mode d'installation « Filtration externe signalée ». Fraîcheur
// max du dernier signal d'état (HTTP/MQTT) avant fail-safe OFF (dosage refusé).
constexpr uint32_t kExternalFiltrationStaleMs = 180000UL;  // 3 min — fraîcheur max signal filtration externe (fail-safe OFF au-delà)

// ============================================================================
// PUMP CONSTANTS - Paramètres pompes doseuses
// ============================================================================

// Débits pompes (ml/min) — à ajuster selon le modèle de pompe utilisé
constexpr float kPumpMinFlowMlPerMin  =  5.2f;   // Débit minimal (duty minimum actif)
constexpr float kPumpMaxFlowMlPerMin  = 90.0f;   // Débit maximal (duty 100%)

// Erreur max pour normalisation PID
constexpr float kPhMaxError           =  1.0f;   // Erreur pH maximale (unités pH)
constexpr float kOrpMaxError          = 200.0f;  // Erreur ORP maximale (mV)

// Durées de stabilisation post-calibration EZO (pool-chemistry condition #3).
// Validation pool-chemistry feature-021 :
//   - pH  : équilibre membrane verre + jonction Ag/AgCl ≈ 5 min
//   - ORP : équilibre Pt/Ag ≈ 3 min (cinétique plus rapide que pH)
// Si `mqttCfg.stabilizationDelayMin > 0`, l'override utilisateur prend la priorité
// pour les armings legacy (filtration, mode continu). Les armings post-calibration
// EZO utilisent toujours ces durées spécifiques (la cinétique chimique l'impose).
constexpr unsigned long kStabilizationDurationPhMs  = 300000UL;  // 5 min — pH post-cal
constexpr unsigned long kStabilizationDurationOrpMs = 180000UL;  // 3 min — ORP post-cal

// Anti-rafale dosage chimique (pool-chemistry feature-021, Pass 3.5).
// Limite le nombre de DÉMARRAGES de cycle de dosage par fenêtre glissante
// (indépendant des limites journalières/horaires existantes). Couvre les cas
// PID instable, oscillation autour de la cible, capteur bruité, etc.
constexpr uint8_t kMaxDosingCyclesPerMinute = 6;     // 1 cycle / 10s max
constexpr uint8_t kMaxDosingCyclesPer15Min  = 20;    // anti-emballement PID
constexpr size_t  kDosingCycleHistorySize   = 20;    // ring buffer (couvre 15min)

// Bornes dures de la garde 9 anti-cycling (maxCyclesPerDay configurable,
// feature-060 — verdict pool-chemistry 2026-07-29, GO sous conditions).
// Appliquées par clamp CÔTÉ SERVEUR (route de config) ET au chargement NVS
// via clampCyclesPerDay() (dosing_logic). Ne JAMAIS élargir sans nouveau
// passage pool-chemistry.
constexpr unsigned int kCyclesPerDayMin = 10;  // plancher : en dessous, la garde bloque un régime d'entretien sain
constexpr unsigned int kCyclesPerDayMax = 60;  // plafond : sous la cadence physique ~70/j imposée par la pause mélange

// Répartition du volume quotidien du mode "scheduled" (feature-011).
// Le volume restant est réparti uniformément par fenêtres de 15 min sur
// l'horizon de filtration restant, borné à minuit (les compteurs journaliers
// se réinitialisent à minuit). Décision pure : evaluateScheduledDose()
// (src/dosing_logic.*) ; horizon : remainingRangeMinutes() (src/schedule_logic.*).
constexpr int kScheduledWindowMinutes = 15;  // Fenêtre de répartition scheduled (windowIndex = nowMin / 15)

// ============================================================================
// BOOST CONSTANTS - Mode Boost / surchloration temporaire (feature-053)
// ============================================================================
// Surcouche temporaire NON destructive activable d'un geste, auto-expirant au
// prochain minuit local. N'a d'effet chimique que si la régulation ORP est en
// mode "automatic" (cible + limite journalière). Bornes FIGÉES par pool-chemistry
// (feature-053, double passage) — ne JAMAIS élargir sans nouveau passage.

// +60 mV sur la cible ORP : relève sensiblement le résidu chloré (effet réel)
// tout en restant dans la plage physiologique d'une eau baignable une fois le
// pic redescendu. Marge choisie prudente vs. la borne dure ORP.
constexpr float kBoostOrpDeltaMv = 60.0f;

// Plafond ABSOLU de la cible ORP effective boostée : 850 mV (PAS 900). Le code
// lève l'alerte orpAbnormal dès orp > 900 mV ; plafonner la cible à 850 laisse
// une marge de sécurité de 50 mV avant cette alerte et évite de piloter vers un
// ORP jugé anormal.
constexpr float kBoostOrpCeilingMv = 850.0f;

// ×1,5 sur la limite journalière chlore pendant le boost : donne un budget réel
// de surchloration sur la journée sans multiplier le risque de surdosage. Effet
// borné en dur par kBoostDailyHardCapMl ci-dessous.
constexpr float kBoostDailyFactor = 1.5f;

// Plafond dur ABSOLU de la limite journalière chlore effective boostée : 1000 mL.
// Quel que soit maxChlorineMlPerDay × facteur, la limite effective ne dépasse
// jamais cette valeur — garde-fou anti-surdosage même si l'utilisateur a une
// limite normale déjà élevée.
constexpr float kBoostDailyHardCapMl = 1000.0f;

// ============================================================================
// OTA INTEGRITY CONSTANTS - Vérification d'intégrité SHA-256 (feature-026)
// ============================================================================

// Empreinte SHA-256 : 32 octets = 64 caractères hexadécimaux.
// Les buffers d'affichage doivent faire kOtaSha256HexLen + 1 (terminateur).
constexpr size_t kOtaSha256HexLen = 64;

// Préfixe des digests d'assets GitHub (champ `digest` de l'API releases).
constexpr const char* kOtaSha256Prefix = "sha256:";

// ============================================================================
// NETWORK CONSTANTS - Paramètres réseau
// ============================================================================

// Ports réseau
constexpr uint16_t kHttpServerPort = 80;                  // Port serveur HTTP
constexpr uint16_t kMdnsHttpPort = 80;                    // Port mDNS pour HTTP

// mDNS
constexpr const char* kMdnsHostname = "poolcontroller";       // Nom d'hôte mDNS (sans suffixe)
constexpr const char* kMdnsFullHost = "poolcontroller.local"; // FQDN mDNS (utilisé par UI/HTTP/WS pour les liens)
// Debounce ré-annonce mDNS (v2.9.1) : en double NAT CPL, l'IP peut osciller et
// déclencher un churn MDNS.end()/begin() coûteux à chaque changement. On borne la
// fréquence de ré-annonce pour ignorer les oscillations transitoires.
constexpr unsigned long kMdnsReannounceMinIntervalMs = 30000; // 30s - intervalle mini entre 2 ré-annonces mDNS

// Wi-Fi
constexpr uint32_t kWifiConnectTimeoutMs = 15000;          // Timeout connexion Wi-Fi (ms)

// Watchdog de connectivité (v2.9.2) : sonde ICMP active vers la passerelle pour
// détecter un gel de la pile lwIP (web/mDNS/ping ICMP morts alors que le WiFi reste
// associé, heap sain) que le watchdog de tâche ne voit pas (thread tcpip lwIP non
// surveillé). Indépendant de MQTT (souvent désactivé) : le signal de vie vient du
// ping ICMP passerelle (+ succès NTP en défense en profondeur).
constexpr uint32_t kNetworkProbeIntervalMs = 30000;   // 30s - cadence sonde ICMP passerelle
constexpr uint32_t kNetworkStallSoftMs     = 120000;  // 2 min - stall → soft recover (reconnexion WiFi)
constexpr uint32_t kNetworkStallRebootMs   = 300000;  // 5 min - stall → reboot fail-safe
constexpr uint32_t kNetworkProbeTimeoutMs  = 2000;    // 2s - timeout echo ICMP

// ============================================================================
// TIME CONSTANTS - Valeurs temporelles de référence
// ============================================================================

// Epoch minimal considéré comme valide (heure synchronisée NTP/RTC)
constexpr time_t kMinValidEpoch = 1700000000;  // 14 nov. 2023

#endif // CONSTANTS_H
