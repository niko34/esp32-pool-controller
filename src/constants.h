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
//
// Note : kUart2RxPin (16) et kUart2TxPin (17) sont définis dans
// src/uart_transport.h — ne pas dupliquer ici.

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
constexpr size_t kMaxRawDataPoints = 72;                  // 6h de données brutes (intervalle 5min)
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

constexpr uint8_t  kAutoHealDebounceChecks = 3;           // Ticks consécutifs sous seuil avant restart (anti oscillation)
constexpr uint32_t kAutoHealMinUptimeMs = 180000;         // 3 min - uptime minimal avant tout restart auto
constexpr uint8_t  kAutoHealMaxReboots = 3;               // Plafond de reboots auto dans la fenêtre → mode maintenance
constexpr uint32_t kAutoHealRebootWindowMs = 900000;      // 15 min - fenêtre anti-boucle (tumbling, ancrée au 1er reboot)

// Disjoncteur de boot-loop de l'init history (v2.10.0, feature-059) : un drapeau
// NVS (crash_cnt, namespace hist_meta) est posé avant le montage de la partition
// history et levé après init complète. Si le boot précédent a crashé pendant
// cette fenêtre kHistoryInitCrashThreshold fois consécutives, la partition est
// effacée avant remontage (corruption littlefs → panic non rattrapable à la 1ʳᵉ
// écriture). Indépendant de kAutoHealMaxReboots et du mode maintenance.
constexpr uint8_t kHistoryInitCrashThreshold = 2;             // Crashs consécutifs pendant l'init → effacement partition history
constexpr const char* kHistoryCrashCountNvsKey = "crash_cnt"; // Clé NVS du compteur (namespace hist_meta, clé part_sz intacte)

// Garde heap pour les réponses volumineuses des journaux (/get-logs, /download-logs).
// v2.8.2 : sous forte fragmentation du tas (gros bloc contigu trop petit), la
// construction de la réponse peut échouer. On refuse alors proprement (503) plutôt
// que de tenter une allocation qui dégraderait la réponse. Comparé au plus gros
// bloc contigu libre (heap_caps_get_largest_free_block), pas au heap total.
// Couvre le pire cas /get-logs (~200 entrées × ~120 o ≈ 24 Ko) + marge TCP.
constexpr size_t kMinHeapForLogsResponse = 24576;         // 24KB - Bloc contigu min pour servir les logs

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
constexpr float   kPhFilterSpikeReject           = 1.0f;     // pH : spike dur isolé écarté
constexpr float   kOrpFilterSpikeReject          = 300.0f;   // mV : spike dur isolé écarté

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
// échantillon (pH brut/lissé, ORP brut/lissé, T° eau). 300 × 24 o ≈ 7 Ko.
// Fenêtre glissante ≈ 25 min à kPhOrpSensorIntervalMs (5 s). Diagnostic pur.
constexpr size_t kSensorTraceBufferSize          = 300;
// Pagination de GET /debug/sensor_trace (bug-crash-sensor-trace-badalloc) :
// 50 échantillons/page ≈ 3,5 Ko de JSON — jamais d'allocation contiguë > ~4 Ko
// dans AsyncResponseStream (le heap fragmenté peut descendre sous 8 Ko, ADR-0029).
constexpr size_t kSensorTraceMaxSamplesPerPage   = 50;
// Marge de sécurité entre la taille mesurée de la réponse (measureJson) et le
// plus gros bloc libre (ESP.getMaxAllocHeap()) : en dessous → 503 propre au
// lieu d'un std::bad_alloc → abort() sur la tâche async_tcp.
constexpr size_t kSensorTraceHeapMarginBytes     = 4096;

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
