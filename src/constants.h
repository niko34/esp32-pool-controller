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

// Sécurité - Factory reset bouton
constexpr unsigned long kFactoryResetButtonHoldMs = 10000; // 10s - Maintien bouton pour factory reset

// Sécurité - Rate limiting
constexpr uint16_t kMaxRequestsPerMinute = 30;            // Limite globale par IP
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

// Conversion temps
constexpr unsigned long kMillisToSeconds = 1000;          // Conversion ms → s
constexpr unsigned long kSecondsPerMinute = 60;           // Secondes par minute
constexpr unsigned long kMillisToMinutes = 60000;         // Conversion ms → min
constexpr unsigned long kSecondsPerHour = 3600;           // Secondes par heure

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

// ============================================================================
// NETWORK CONSTANTS - Paramètres réseau
// ============================================================================

// Ports réseau
constexpr uint16_t kHttpServerPort = 80;                  // Port serveur HTTP
constexpr uint16_t kMdnsHttpPort = 80;                    // Port mDNS pour HTTP

// mDNS
constexpr const char* kMdnsHostname = "poolcontroller";   // Nom d'hôte mDNS

// Wi-Fi
constexpr uint32_t kWifiConnectTimeoutMs = 15000;          // Timeout connexion Wi-Fi (ms)

// ============================================================================
// TIME CONSTANTS - Valeurs temporelles de référence
// ============================================================================

// Epoch minimal considéré comme valide (heure synchronisée NTP/RTC)
constexpr time_t kMinValidEpoch = 1700000000;  // 14 nov. 2023

#endif // CONSTANTS_H
