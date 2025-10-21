#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==== Configuration des broches ====
#define ORP_PIN 34
#define PH_PIN 35
#define PUMP1_PWM_PIN 25
#define PUMP1_IN1_PIN 32
#define PUMP1_IN2_PIN 33
#define PUMP2_PWM_PIN 26
#define PUMP2_IN1_PIN 18
#define PUMP2_IN2_PIN 19
#define PUMP1_CHANNEL 0
#define PUMP2_CHANNEL 1
#define PUMP_PWM_FREQ 1000
#define PUMP_PWM_RES_BITS 8
#define TEMP_SENSOR_PIN 4
#define FILTRATION_RELAY_PIN 27

// ==== Constantes ====
constexpr float PH_DEADBAND = 0.05f;
constexpr float ORP_DEADBAND = 5.0f;
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
  bool phEnabled = false;
  bool orpEnabled = false;
  int phPump = 1;
  int orpPump = 2;
  int phInjectionLimitSeconds = 60;
  int orpInjectionLimitSeconds = 60;
  bool timeUseNtp = true;
  String ntpServer = "pool.ntp.org";
  String manualTimeIso = "";
  String timezoneId = "europe_paris";
};

struct FiltrationConfig {
  String mode = "auto"; // auto, manual, off
  String start = "08:00";
  String end = "20:00";
  bool hasAutoReference = false;
  float autoReferenceTemp = 24.0f;
};

struct SimulationConfig {
  bool enabled = true;
  float poolVolumeM3 = 50.0f;
  float phMinusVolumePerDeltaMl = 300.0f;
  float phMinusDelta = 0.1f;
  float phMinusReferenceVolumeM3 = 10.0f;
  float phPumpRateMlPerMin = 30.0f;
  float filtrationFlowM3PerHour = 16.0f;
  float initialPh = 8.5f;
  float initialOrp = 650.0f;
  float initialTemp = 24.0f;
  float timeAcceleration = 360.0f; // 360.0f pour 1h -> 1 min
  float phReversionFactor = 0.01f;
  float orpPumpRateMlPerMin = 30.0f;
  float orpEffectMvPerMl = 0.005f;
  float orpReversionFactor = 0.01f;
  bool overrideClock = true;
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

// ==== Fonctions de gestion ====
void saveMqttConfig();
void loadMqttConfig();
void applyMqttConfig();
void sanitizePumpSelection();
int sanitizePumpNumber(int pumpNumber, int defaultValue);
int pumpIndexFromNumber(int pumpNumber);

#endif // CONFIG_H
