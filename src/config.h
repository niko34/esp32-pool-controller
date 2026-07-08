#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "dosing_logic.h"  // feature-056 : enum InstallMode (logique pure partagée)

// ==== Configuration des canaux PWM pompes ====
// MOSFET IRLZ44N - PWM sur Gate, pins définis dans constants.h (PCB v2)
#define PUMP1_CHANNEL 0
#define PUMP2_CHANNEL 1
#define PUMP_PWM_FREQ 20000   // 20kHz PWM
#define PUMP_PWM_RES_BITS 8  // 8-bit resolution (0-255)

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
  float pumpMaxFlowMlPerMin = 90.0f; // Débit maximal pompes (ml/min)
  int phInjectionLimitMinutes = 5;    // Max 5 min d'injection par fenêtre d'1h
  int orpInjectionLimitMinutes = 10;  // Max 10 min d'injection par fenêtre d'1h
  // feature-056 : mode d'installation (remplace regulationMode ET
  // filtrationCfg.enabled). Décrit le câblage réel et pilote la présence d'eau,
  // le relais filtration et l'horizon de répartition. Défaut = Managed (ancien
  // "pilote" + filtration gérée).
  InstallMode installMode = InstallMode::ManagedFiltration;
  int stabilizationDelayMin = 5;     // Délai de stabilisation avant dosage (minutes)
  String regulationSpeed = "normal"; // Vitesse de correction PID : "slow", "normal", "fast"
  String phCorrectionType = "ph_minus";  // "ph_minus" (acide) ou "ph_plus" (base)
  String phRegulationMode = "automatic"; // "automatic" / "scheduled" / "manual"
  int phDailyTargetMl = 0;               // Volume quotidien cible (mL) pour le mode programmée
  String orpRegulationMode = "automatic"; // "automatic" / "scheduled" / "manual"
  int orpDailyTargetMl = 0;               // Volume quotidien cible (mL) pour le mode Programmée ORP
  bool timeUseNtp = true;
  String ntpServer = "pool.ntp.org";
  String manualTimeIso = "";
  String timezoneId = "europe_paris";

  // feature-021 (Pass 4a) : calibration pH/ORP entièrement déléguée aux modules
  // Atlas EZO (commande Cal,? + cache _phCalCachedPoints / _orpCalCachedPoints
  // dans SensorManager). Aucun champ de calibration pH/ORP n'est plus persisté
  // en NVS côté ESP32. Les anciens champs phCalibrationDate/Temp,
  // orpCalibrationOffset/Slope/Date/Reference/Temp ont été supprimés.

  // Calibration Température DS18B20
  // Formule appliquée: Temp_final = Temp_brut + offset
  float tempCalibrationOffset = 0.0f; // Offset de calibration température (°C)
  String tempCalibrationDate = "";    // Date de calibration température (ISO 8601)
  bool temperatureEnabled = true;     // Fonction température activée/désactivée
};

struct FiltrationConfig {
  // feature-056 : le champ `enabled` a été absorbé par mqttCfg.installMode
  // (relais piloté ssi installMode == ManagedFiltration).
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
  bool forceWifiConfig = false;   // Forcer l'affichage du bouton Wi-Fi sur l'écran login
  bool wizardCompleted = false;   // Wizard de configuration initiale complété (persiste au redémarrage)
  bool disableApOnBoot = false;   // Désactiver le mode AP au prochain redémarrage (pour transition WiFi)
  bool sensorLogsEnabled = false; // Logs détaillés des sondes (pH, ORP, Temp) activés/désactivés
  bool debugLogsEnabled = false;  // Logs DEBUG (firmware + UI) activés/désactivés
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
  float maxPhMlPerDay = 300.0f;
  float maxChlorineMlPerDay = 500.0f;
  float dailyPhInjectedMl = 0.0f;
  float dailyOrpInjectedMl = 0.0f;
  unsigned long dayStartTimestamp = 0;
  bool phLimitReached = false;
  bool orpLimitReached = false;
  char currentDayDate[9] = {};  // YYYYMMDD\0 — date locale du dernier reset journalier
};

struct PumpProtection {
  // Protection anti-cycling pour prolonger la durée de vie des pompes
  unsigned long minInjectionTimeMs = 30000;      // 30s minimum par injection
  float phStartThreshold = 0.05f;                // Démarre dosage si erreur pH > 0.05
  float phStopThreshold = 0.01f;                 // Continue dosage si erreur pH > 0.01
  float orpStartThreshold = 15.0f;               // Démarre dosage si erreur ORP > 15mV (feature-025 : deadband effectif ORP ±15 mV)
  float orpStopThreshold = 2.0f;                 // Continue dosage si erreur ORP > 2mV
  unsigned int maxCyclesPerDay = 20;             // Max 20 démarrages par jour
};

// feature-053 : état du Mode Boost (surchloration temporaire du jour).
// Surcouche NON destructive : ne modifie PAS la config de régulation. Persisté
// en NVS dédié (saveBoostState/loadBoostState) pour survivre à un reboot dans
// la journée ; auto-expirant au prochain minuit local (tickBoostExpiry).
struct BoostState {
  bool active = false;      // Boost actif
  time_t untilEpoch = 0;    // Instant d'expiration (prochain minuit local)
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

// feature-056 : sérialisation STABLE du mode d'installation (JSON/WS/UART/NVS).
const char* installModeToString(InstallMode mode);
InstallMode installModeFromString(const char* s, InstallMode fallback);

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
extern BoostState boostState;

void saveProductConfig();
void loadProductConfig();
void saveDailyCounters();
void loadDailyCounters();

// ==== Mode Boost (feature-053) ====
// Persistance NVS DÉDIÉE (namespace poolctrl, clés boost_active/boost_until) —
// PAS via saveMqttConfig, pour éviter l'usure NVS d'une réécriture complète.
void saveBoostState();
void loadBoostState();  // À appeler au boot ; si expiré au chargement → inactif.
// Active le boost jusqu'au prochain minuit local. REFUSE si l'heure n'est pas
// synchronisée (time() < kMinValidEpoch, condition pool-chemistry #4) : sans
// heure valide, l'expiration à minuit ne peut être calculée.
void startBoost();
// Désactive le boost et persiste immédiatement.
void stopBoost();
// True si le boost est actif à l'instant `now`. Si `now` < kMinValidEpoch
// (heure non synchro) → renvoie boostState.active SANS expirer (condition #4).
bool isBoostActive(time_t now);
// Expire le boost au passage de minuit. À appeler EN TÊTE de tickDailyRollover,
// AVANT tout reset de compteur (condition pool-chemistry #3).
void tickBoostExpiry(time_t now);

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

// feature-021 : la calibration pH 2 points est gérée par le module Atlas EZO.
// Les fonctions calculatePhCalibration() / isPhCalibrationValid() ont été
// supprimées (utiliser sensors.getPhCalibrationPointsCached() à la place).

#endif // CONFIG_H
