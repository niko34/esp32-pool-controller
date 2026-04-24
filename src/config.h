#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ==== Configuration des broches ====
// MOSFET IRLZ44N - 1 pin PWM par pompe (Gate control)
#define PUMP1_PWM_PIN 18     // PWM Gate pompe 1 (via MOSFET IRLZ44N)
#define PUMP2_PWM_PIN 19     // PWM Gate pompe 2 (via MOSFET IRLZ44N)
#define PUMP1_CHANNEL 0
#define PUMP2_CHANNEL 1
#define PUMP_PWM_FREQ 20000   // 20kHz PWM
#define PUMP_PWM_RES_BITS 8  // 8-bit resolution (0-255)

// Capteurs et actionneurs
#define TEMP_SENSOR_PIN 5
#define FILTRATION_RELAY_PIN 25
#define LIGHTING_RELAY_PIN 26           // Relais pour éclairage piscine
#define FACTORY_RESET_BUTTON_PIN 32    // GPIO32 - Bouton factory reset (actif haut, pull-down interne)
#define BUILTIN_LED_PIN 2               // GPIO2 - LED intégrée ESP32

// ==== Constantes ====
constexpr float PH_DEADBAND = 0.01f;      // Zone morte réduite : 7.2 à 7.21 (±0.01)
constexpr float ORP_DEADBAND = 2.0f;      // Zone morte réduite : ±2mV
constexpr uint8_t MAX_PWM_DUTY = (1 << PUMP_PWM_RES_BITS) - 1;
constexpr uint8_t MIN_ACTIVE_DUTY = 80;

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
  uint8_t pump1MaxDutyPct = 50;    // Puissance maximale pompe 1 en régulation (0-100 %)
  uint8_t pump2MaxDutyPct = 50;    // Puissance maximale pompe 2 en régulation (0-100 %)
  uint32_t minPauseBetweenMin = 30; // Pause min entre deux injections (minutes)
  float pumpMaxFlowMlPerMin = 90.0f; // Débit maximal pompes (ml/min)
  int phInjectionLimitSeconds = 300;   // Max 5 min d'injection par fenêtre d'1h
  int orpInjectionLimitSeconds = 600;  // Max 10 min d'injection par fenêtre d'1h
  String regulationMode = "pilote";  // "continu" ou "pilote"
  int stabilizationDelayMin = 5;     // Délai de stabilisation avant dosage (minutes)
  String regulationSpeed = "normal"; // Vitesse de correction PID : "slow", "normal", "fast"
  String phCorrectionType = "ph_minus";  // "ph_minus" (acide) ou "ph_plus" (base)
  String phRegulationMode = "automatic"; // "automatic" / "scheduled" / "manual"
  int phDailyTargetMl = 0;               // Volume quotidien cible (mL) pour le mode programmée
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
  bool temperatureEnabled = true;     // Fonction température activée/désactivée
};

struct FiltrationConfig {
  bool enabled = true;  // Fonction filtration activée/désactivée
  String mode = "auto"; // auto, manual, off
  String start = "08:00";
  String end = "20:00";
  bool forceOn = false;  // Forçage ON temporaire (non persisté, remis à false au redémarrage)
  bool forceOff = false; // Forçage OFF temporaire (non persisté, remis à false au redémarrage)
};

struct LightingConfig {
  bool featureEnabled = true;          // Fonction éclairage activée/désactivée (masque l'UI)
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
  String apPassword = "";         // Mot de passe réseau WiFi AP (généré au premier boot si vide)
  String corsAllowedOrigins = ""; // Origines CORS autorisées (séparées par virgules, vide = désactivé)
  bool forceWifiConfig = false;   // Forcer l'affichage du bouton Wi-Fi sur l'écran login
  bool wizardCompleted = false;   // Wizard de configuration initiale complété (persiste au redémarrage)
  bool disableApOnBoot = false;   // Désactiver le mode AP au prochain redémarrage (pour transition WiFi)
  bool sensorLogsEnabled = false; // Logs détaillés des sondes (pH, ORP, Temp) activés/désactivés
  bool screenEnabled = false;     // Écran LVGL externe (ESP32 dédié via UART2) activé/désactivé
};

struct PumpControlParams {
  float minFlowMlPerMin;
  float maxFlowMlPerMin;
  float maxError;
};

struct ProductConfig {
  bool phTrackingEnabled = false;         // Suivi volume pH activé
  float phContainerVolumeMl = 20000.0f;   // Volume bidon pH (ml), défaut 20L
  float phTotalInjectedMl = 0.0f;         // Total injecté depuis dernier reset (ml)
  float phAlertThresholdMl = 2000.0f;     // Seuil d'alerte volume restant (ml), défaut 2L

  bool orpTrackingEnabled = false;        // Suivi volume chlore activé
  float orpContainerVolumeMl = 20000.0f;  // Volume bidon chlore (ml)
  float orpTotalInjectedMl = 0.0f;        // Total injecté depuis dernier reset (ml)
  float orpAlertThresholdMl = 2000.0f;    // Seuil d'alerte volume restant (ml)
};

struct SafetyLimits {
  float maxPhMinusMlPerDay = 300.0f;
  float maxChlorineMlPerDay = 500.0f;
  float dailyPhInjectedMl = 0.0f;
  float dailyOrpInjectedMl = 0.0f;
  unsigned long dayStartTimestamp = 0;
  bool phLimitReached = false;
  bool orpLimitReached = false;
};

struct PumpProtection {
  // Protection anti-cycling pour prolonger la durée de vie des pompes
  unsigned long minInjectionTimeMs = 30000;      // 30s minimum par injection
  unsigned long minPauseBetweenMs = 1800000;     // 30min pause minimum entre injections
  float phStartThreshold = 0.05f;                // Démarre dosage si erreur pH > 0.05
  float phStopThreshold = 0.01f;                 // Continue dosage si erreur pH > 0.01
  float orpStartThreshold = 10.0f;               // Démarre dosage si erreur ORP > 10mV
  float orpStopThreshold = 2.0f;                 // Continue dosage si erreur ORP > 2mV
  unsigned int maxCyclesPerDay = 20;             // Max 20 démarrages par jour
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
extern ProductConfig productCfg;
extern bool productConfigDirty;

void saveProductConfig();
void loadProductConfig();

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
