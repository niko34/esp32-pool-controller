#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>

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
constexpr size_t kMinFreeHeapBytes = 10000;               // Seuil critique mémoire disponible

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
// SENSOR FILTER CONSTANTS - Lissage mesures pH/ORP (feature-025)
// ============================================================================
// Chaîne de filtrage : rejet aberrant → médiane courte → EMA lente.
// Centralisé ici pour ajustement terrain. Buffer FIXE (pas d'alloc dynamique).
// Validation pool-chemistry feature-025 (conditions non négociables).

constexpr uint8_t kSensorFilterMedianWindow      = 7;        // Taille buffer médian (impair, FIXE)
constexpr float   kPhEmaAlpha                    = 0.10f;    // Coefficient EMA pH (lissage lent)
constexpr float   kOrpEmaAlpha                   = 0.08f;    // Coefficient EMA ORP (lissage lent)
constexpr float   kPhFilterMaxStep               = 0.15f;    // Saut max pH/lecture (rejet au-delà)
constexpr float   kOrpFilterMaxStep              = 50.0f;    // Saut max ORP/lecture (mV, rejet au-delà)
constexpr float   kPhFilterMin                   = 0.0f;     // Plage plausible pH min
constexpr float   kPhFilterMax                   = 14.0f;    // Plage plausible pH max
constexpr float   kOrpFilterMin                  = -1000.0f; // Plage plausible ORP min (mV)
constexpr float   kOrpFilterMax                  = 1500.0f;  // Plage plausible ORP max (mV)
constexpr uint8_t kSensorFilterWarmupSamples     = 5;        // Mesures valides avant filtre prêt
constexpr uint8_t kSensorFilterMaxConsecutiveRejects = 10;   // Rejets consécutifs → capteur instable
// Re-synchronisation : un changement réel et DURABLE (> maxStep maintenu) ne doit pas
// figer le filtre indéfiniment. Au-delà de ce seuil de rejets consécutifs, on conclut
// à un vrai changement et on ré-amorce le filtre sur la médiane des derniers bruts rejetés.
// 12 cycles × 5 s/cycle ≈ 60 s. STRICTEMENT > kSensorFilterMaxConsecutiveRejects (10, seuil "instable")
// ET > kSensorFilterMedianWindow (7) pour garantir un mini-buffer de rejets plein → médiane d'amorçage fiable.
// Le dosage est de toute façon bloqué dès 10 rejets (unstable) puis pendant le re-warmup (ready=false).
constexpr uint8_t  kSensorFilterResyncRejects     = 12;      // Rejets consécutifs → re-sync (≈60 s, feature-033)
// Anti-boucle : un capteur qui re-sync en boucle = défaut EMI, pas un vrai changement.
// Au-delà de ce nombre de re-sync sur la fenêtre glissante → latch "instable" jusqu'à reset().
constexpr uint8_t  kSensorFilterMaxResyncPerWindow = 3;      // Re-sync max avant latch instable
constexpr uint32_t kSensorFilterResyncWindowMs    = 600000;  // 10 min — fenêtre glissante anti-boucle
// Âge max de la dernière mesure valide pour que le filtre soit considéré "prêt".
// Au-delà, ready() repasse false (mesure trop ancienne → fail-closed dosage).
// 4 × kPhOrpSensorIntervalMs (5 s) = 20 s, cohérent avec kSensorStaleTimeoutMs.
constexpr uint32_t kSensorFilterMaxAgeMs         = 20000;    // 20 s

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
constexpr const char* kMdnsHostname = "poolcontroller2";       // Nom d'hôte mDNS (sans suffixe)
constexpr const char* kMdnsFullHost = "poolcontroller2.local"; // FQDN mDNS (utilisé par UI/HTTP/WS pour les liens)

// Wi-Fi
constexpr uint32_t kWifiConnectTimeoutMs = 15000;          // Timeout connexion Wi-Fi (ms)

// ============================================================================
// TIME CONSTANTS - Valeurs temporelles de référence
// ============================================================================

// Epoch minimal considéré comme valide (heure synchronisée NTP/RTC)
constexpr time_t kMinValidEpoch = 1700000000;  // 14 nov. 2023

#endif // CONSTANTS_H
