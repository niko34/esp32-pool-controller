#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ==== Configuration des broches ====
// MOSFET IRLZ44N - 1 pin PWM par pompe (Gate control)
#define PUMP1_PWM_PIN 25     // PWM Gate pompe 1 (via MOSFET IRLZ44N)
#define PUMP2_PWM_PIN 26     // PWM Gate pompe 2 (via MOSFET IRLZ44N)
#define PUMP1_CHANNEL 0
#define PUMP2_CHANNEL 1
#define PUMP_PWM_FREQ 20000   // 20kHz PWM
#define PUMP_PWM_RES_BITS 8  // 8-bit resolution (0-255)

// Capteurs analogiques (ADS1115 via I2C)
#define PH_SENSOR_PIN 35     // GPIO pour capteur pH (ADC1_7)
#define ORP_SENSOR_PIN 34    // GPIO pour capteur ORP (ADC1_6)

// Capteurs et actionneurs
#define TEMP_SENSOR_PIN 5
#define FILTRATION_RELAY_PIN 18
#define LIGHTING_RELAY_PIN 19           // Relais pour éclairage piscine
#define PASSWORD_RESET_BUTTON_PIN 4     // GPIO4 - Bouton reset mot de passe (actif bas, pull-up interne)
#define BUILTIN_LED_PIN 2               // GPIO2 - LED intégrée ESP32

// ==== Constantes ====
constexpr float PH_DEADBAND = 0.01f;      // Zone morte réduite : 7.2 à 7.21 (±0.01)
constexpr float ORP_DEADBAND = 2.0f;      // Zone morte réduite : ±2mV
constexpr uint8_t MAX_PWM_DUTY = (1 << PUMP_PWM_RES_BITS) - 1;
constexpr uint8_t MIN_ACTIVE_DUTY = 20;

// ==== Home Assistant ====
const char* const HA_DEVICE_ID = "poolcontroller";
const char* const HA_DEVICE_NAME = "Pool Controller";
const char* const HA_DISCOVERY_PREFIX = "homeassistant";

// ==== Structures de configuration ====
struct MqttConfig {
  String server = "192.168.1.10";
  int port = 1883;
  String topic = "pool/sensors";
  String username = "";
  String password = "";
  bool enabled = false;
  float phTarget = 7.2f;
  float orpTarget = 650.0f;
  bool phEnabled = true;
  bool orpEnabled = true;
  int phPump = 1;
  int orpPump = 2;
  int phInjectionLimitSeconds = 60;
  int orpInjectionLimitSeconds = 60;
  bool timeUseNtp = true;
  String ntpServer = "pool.ntp.org";
  String manualTimeIso = "";
  String timezoneId = "europe_paris";

  // Calibration pH (géré par DFRobot_PH en EEPROM)
  // La librairie DFRobot_PH gère automatiquement:
  // - Calibration 1 point (pH neutre, pH 7.0)
  // - Calibration 2 points (pH acide 4.0 + pH alcalin 9.18)
  // - Compensation automatique de température
  String phCalibrationDate = "";      // Date de dernière calibration (ISO 8601)
  float phCalibrationTemp = NAN;      // Température lors de la calibration (°C)

  // Calibration ORP (1 ou 2 points)
  // Formule appliquée: ORP_final = (ORP_brut * slope) + offset
  // Calibration 1 point: slope=1.0, offset calculé
  // Calibration 2 points: slope et offset calculés à partir de 2 solutions de référence
  float orpCalibrationOffset = 0.0f;  // Offset de calibration ORP (mV)
  float orpCalibrationSlope = 1.0f;   // Pente/gain de calibration ORP (sans unité)
  String orpCalibrationDate = "";     // Date de calibration ORP (ISO 8601)
  float orpCalibrationReference = 0.0f; // Valeur de référence utilisée (mV)
  float orpCalibrationTemp = NAN;     // Température lors de la calibration ORP (°C)

  // Calibration Température DS18B20
  // Formule appliquée: Temp_final = Temp_brut + offset
  float tempCalibrationOffset = 0.0f; // Offset de calibration température (°C)
  String tempCalibrationDate = "";    // Date de calibration température (ISO 8601)
};

struct FiltrationConfig {
  String mode = "auto"; // auto, manual, off
  String start = "08:00";
  String end = "20:00";
  bool hasAutoReference = false;
  float autoReferenceTemp = 24.0f;
};

struct LightingConfig {
  bool enabled = false;                // Éclairage ON/OFF manuel
  uint8_t brightness = 255;            // Luminosité PWM (0-255)
  bool scheduleEnabled = false;        // Programmation activée/désactivée
  String startTime = "20:00";          // Heure de début (HH:MM)
  String endTime = "23:00";            // Heure de fin (HH:MM)
};

struct AuthConfig {
  bool enabled = true;            // Authentification activée/désactivée
  String adminPassword = "admin"; // Mot de passe administrateur (HTTP Basic Auth)
  String apiToken = "";           // Token API pour intégrations (généré au boot si vide)
  String corsAllowedOrigins = ""; // Origines CORS autorisées (séparées par virgules, vide = désactivé)
  bool forceWifiConfig = false;   // Forcer l'affichage du bouton Wi-Fi sur l'écran login
  bool wizardCompleted = false;   // Wizard de configuration initiale complété (persiste au redémarrage)
  bool disableApOnBoot = false;   // Désactiver le mode AP au prochain redémarrage (pour transition WiFi)
  bool sensorLogsEnabled = false; // Logs détaillés des sondes (pH, ORP, Temp) activés/désactivés
};

struct PumpControlParams {
  float minFlowMlPerMin;
  float maxFlowMlPerMin;
  float maxError;
};

struct SafetyLimits {
  float maxPhMinusMlPerDay = 500.0f;
  float maxChlorineMlPerDay = 300.0f;
  unsigned long dailyPhInjectedMl = 0;
  unsigned long dailyOrpInjectedMl = 0;
  unsigned long dayStartTimestamp = 0;
  bool phLimitReached = false;
  bool orpLimitReached = false;
};

struct PumpProtection {
  // Protection anti-cycling pour prolonger la durée de vie des pompes
  unsigned long minInjectionTimeMs = 30000;      // 30s minimum par injection
  unsigned long minPauseBetweenMs = 300000;      // 5min pause minimum entre injections
  float phStartThreshold = 0.05f;                // Démarre dosage si erreur pH > 0.05
  float phStopThreshold = 0.01f;                 // Continue dosage si erreur pH > 0.01
  float orpStartThreshold = 10.0f;               // Démarre dosage si erreur ORP > 10mV
  float orpStopThreshold = 2.0f;                 // Continue dosage si erreur ORP > 2mV
  unsigned int maxCyclesPerDay = 200;            // Max 200 démarrages par jour
};

struct TimezoneInfo {
  const char* id;
  const char* label;
  const char* posix;
};

// ==== Fuseaux horaires disponibles ====
static const TimezoneInfo TIMEZONES[] = {
  {"europe_paris", "Europe/Paris (UTC+1/UTC+2)", "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:00"},
  {"utc", "UTC", "UTC0"},
  {"america_new_york", "America/New_York (UTC-5/UTC-4)", "EST+5EDT,M3.2.0/02:00:00,M11.1.0/02:00:00"},
  {"america_los_angeles", "America/Los_Angeles (UTC-8/UTC-7)", "PST+8PDT,M3.2.0/02:00:00,M11.1.0/02:00:00"},
  {"asia_tokyo", "Asia/Tokyo (UTC+9)", "JST-9"},
  {"australia_sydney", "Australia/Sydney (UTC+10/UTC+11)", "AEST-10AEDT,M10.1.0/02:00:00,M4.1.0/03:00:00"}
};

const TimezoneInfo* findTimezoneById(const String& id);
const TimezoneInfo* defaultTimezone();
const TimezoneInfo* currentTimezone();
void ensureTimezoneValid();
void applyTimezoneEnv();

// ==== Variables globales de configuration ====
extern MqttConfig mqttCfg;
extern FiltrationConfig filtrationCfg;
extern LightingConfig lightingCfg;
extern AuthConfig authCfg;
extern PumpControlParams phPumpControl;
extern PumpControlParams orpPumpControl;
extern SafetyLimits safetyLimits;
extern PumpProtection pumpProtection;

// ==== Mutex pour protection concurrence ====
// Protège l'accès aux configurations partagées entre loop() et handlers async
extern SemaphoreHandle_t configMutex;
// Protège l'accès au bus I2C partagé entre sensors.update() et calibrations
extern SemaphoreHandle_t i2cMutex;

// ==== Fonctions de gestion ====
void initConfigMutexes();  // Initialise les mutex (à appeler dans setup())
void saveMqttConfig();
void loadMqttConfig();
void applyMqttConfig();
void sanitizePumpSelection();
int sanitizePumpNumber(int pumpNumber, int defaultValue);
int pumpIndexFromNumber(int pumpNumber);

// Fonctions de calibration pH à 2 points
void calculatePhCalibration();  // Calcule gain et offset depuis les 2 points
bool isPhCalibrationValid();    // Vérifie si la calibration 2 points est valide

#endif // CONFIG_H
