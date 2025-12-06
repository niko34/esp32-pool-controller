#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==== Configuration des broches ====
// MOSFET IRLZ44N - 1 pin PWM par pompe (Gate control)
#define PUMP1_PWM_PIN 25     // PWM Gate pompe 1 (via MOSFET IRLZ44N)
#define PUMP2_PWM_PIN 26     // PWM Gate pompe 2 (via MOSFET IRLZ44N)
#define PUMP1_CHANNEL 0
#define PUMP2_CHANNEL 1
#define PUMP_PWM_FREQ 1000   // 1kHz PWM
#define PUMP_PWM_RES_BITS 8  // 8-bit resolution (0-255)
#define TEMP_SENSOR_PIN 4
#define FILTRATION_RELAY_PIN 27

// ==== Constantes ====
constexpr float PH_DEADBAND = 0.01f;      // Zone morte réduite : 7.2 à 7.21 (±0.01)
constexpr float ORP_DEADBAND = 2.0f;      // Zone morte réduite : ±2mV
constexpr uint8_t MAX_PWM_DUTY = (1 << PUMP_PWM_RES_BITS) - 1;
constexpr uint8_t MIN_ACTIVE_DUTY = 20;
constexpr float SIM_MAX_STEP_MINUTES = 0.25f;

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
  bool phEnabled = true;              // Activé par défaut pour la simulation
  bool orpEnabled = true;             // Activé par défaut pour la simulation
  int phPump = 1;
  int orpPump = 2;
  int phInjectionLimitSeconds = 60;
  int orpInjectionLimitSeconds = 60;
  bool timeUseNtp = true;
  String ntpServer = "pool.ntp.org";
  String manualTimeIso = "";
  String timezoneId = "europe_paris";
  int phSensorPin = 35;               // GPIO pour capteur pH (-1 = pas de capteur)
  int orpSensorPin = 34;              // GPIO pour capteur ORP (-1 = pas de capteur)

  // Calibration
  float phCalibrationOffset = 0.0f;   // Offset de calibration pH
  float orpCalibrationOffset = 0.0f;  // Offset de calibration ORP (mV)
};

struct FiltrationConfig {
  String mode = "auto"; // auto, manual, off
  String start = "08:00";
  String end = "20:00";
  bool hasAutoReference = false;
  float autoReferenceTemp = 24.0f;
};

struct SimulationConfig {
  bool enabled = false;

  // Paramètres physiques de la piscine
  float poolVolumeM3 = 50.0f;                    // Volume de la piscine en m³
  float filtrationFlowM3PerHour = 16.0f;         // Débit de filtration en m³/h

  // Paramètres pH- (acide)
  float phPumpRateMlPerMin = 30.0f;              // Débit pompe pH- (ml/min)
  float phMinusEffectPerLiter = -2.0f;           // Effet de 1L de pH- sur le pH pour 10m³
  float phMixingTimeConstant = 0.3f;             // Constante de temps mélange (en cycles de filtration)

  // Paramètres Chlore (ORP)
  float orpPumpRateMlPerMin = 30.0f;             // Débit pompe chlore (ml/min)
  float chlorineEffectPerLiter = 300.0f;         // Effet de 1L de chlore sur ORP (mV) pour 10m³
  float orpMixingTimeConstant = 0.3f;            // Constante de temps mélange (en cycles de filtration)

  // Dérive naturelle (évaporation, UV, baignade, etc.)
  // Le système dérive vers un point d'équilibre naturel
  float phNaturalEquilibrium = 8.0f;             // pH d'équilibre naturel (sans traitement, eau calcaire)
  float phDriftSpeed = 0.02f;                    // Vitesse de dérive (0-1, 0.02 = lent)
  float orpNaturalEquilibrium = 650.0f;          // ORP d'équilibre naturel (sans chlore, baisse naturelle)
  float orpDriftSpeed = 0.03f;                   // Vitesse de dérive vers équilibre

  // Valeurs initiales
  float initialPh = 7.8f;                        // pH initial (plus bas que l'équilibre)
  float initialOrp = 600.0f;                     // ORP initial (plus haut que l'équilibre)
  float initialTemp = 24.0f;

  // Accélération temporelle
  float timeAcceleration = 60.0f;               // 360x = 1h réelle en 10 secondes
  bool overrideClock = true;                     // Accélère l'horloge système
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
extern SimulationConfig simulationCfg;
extern PumpControlParams phPumpControl;
extern PumpControlParams orpPumpControl;
extern SafetyLimits safetyLimits;
extern PumpProtection pumpProtection;

// ==== Fonctions de gestion ====
void saveMqttConfig();
void loadMqttConfig();
void applyMqttConfig();
void sanitizePumpSelection();
int sanitizePumpNumber(int pumpNumber, int defaultValue);
int pumpIndexFromNumber(int pumpNumber);

#endif // CONFIG_H
