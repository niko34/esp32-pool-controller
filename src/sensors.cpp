#include "sensors.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "mqtt_manager.h"
#include <sys/time.h>
#include <EEPROM.h>
#include <Wire.h>


SensorManager sensors;

namespace {
// DS18B20 conversion time depends on resolution.
// Typical max conversion times (datasheet):
// 9-bit: 93.75ms, 10-bit: 187.5ms, 11-bit: 375ms, 12-bit: 750ms
uint16_t ds18b20ConversionTimeMsForResolution(uint8_t resolutionBits) {
  switch (resolutionBits) {
    case 9:  return 94;
    case 10: return 188;
    case 11: return 375;
    case 12:
    default: return 750;
  }
}

// Defaults (set in begin())
uint8_t g_ds18b20ResolutionBits = 12;
uint16_t g_ds18b20ConversionMs = 750;
} // namespace

SensorManager::SensorManager() : oneWire(5), tempSensor(&oneWire) {}

SensorManager::~SensorManager() {}

void SensorManager::begin() {
  // Initialiser I2C (SDA=21, SCL=22 par défaut sur ESP32)
  Wire.begin();

  // Initialiser l'ADS1115
  if (!ads.begin()) {
    systemLogger.error("ADS1115 non détecté sur le bus I2C !");
  } else {
    systemLogger.info("ADS1115 initialisé avec succès");

    // Configuration du gain de l'ADS1115
    // GAIN_ONE = +/- 4.096V (1 bit = 0.125mV pour ADS1115 16 bits)
    // Obligatoire pour les modules pH/ORP qui sortent 0-3.3V
    // GAIN_TWO (+/- 2.048V) saturerait au-dessus de 2.048V
    ads.setGain(GAIN_ONE);

    // Configuration du data rate pour précision maximale
    // 8 SPS (samples per second) = mesure la plus précise et stable
    // Temps de conversion: ~125ms par échantillon
    ads.setDataRate(RATE_ADS1115_8SPS);

    systemLogger.info("ADS1115 configuré : Gain=±4.096V (0.125mV/bit), Data Rate=8 SPS");
  }

  // Initialiser le capteur de température DS18B20 sur GPIO 5
  tempSensor.begin();

  // Lecture non-bloquante : requestTemperatures() ne doit pas attendre la conversion
  tempSensor.setWaitForConversion(false);

  // Configurer la résolution (impacte directement le temps de conversion)
  g_ds18b20ResolutionBits = 12; // 9, 10, 11 ou 12
  tempSensor.setResolution(g_ds18b20ResolutionBits);
  g_ds18b20ConversionMs = ds18b20ConversionTimeMsForResolution(g_ds18b20ResolutionBits);

  systemLogger.info("Capteur de température DS18B20 initialisé sur GPIO 5 (" +
                    String(g_ds18b20ResolutionBits) + "-bit, conv=" +
                    String(g_ds18b20ConversionMs) + "ms)");

  // IMPORTANT: Sur ESP32, l'EEPROM doit être initialisé avant utilisation
  // La librairie DFRobot_PH utilise les adresses 0-7 (8 bytes)
  EEPROM.begin(512);  // Allouer 512 bytes pour l'EEPROM

  // Initialiser le capteur pH DFRobot
  phSensor.begin();

  // Commit les changements EEPROM (nécessaire sur ESP32)
  EEPROM.commit();

  systemLogger.info("Capteur pH DFRobot SEN0161-V2 initialisé");
  systemLogger.info("Gestionnaire de capteurs initialisé");
}

void SensorManager::update() {
  // Protéger l'accès I2C contre collisions avec calibrations (handlers web async)
  // Utilise tryTake (non-bloquant) pour ne pas ralentir la loop
  if (xSemaphoreTake(i2cMutex, 0) == pdTRUE) {
    readRealSensors();
    xSemaphoreGive(i2cMutex);
  }
  // Si mutex indisponible, skip cette itération (la prochaine loop réessaiera)
}

void SensorManager::readRealSensors() {
  unsigned long now = millis();
  static unsigned long lastOrpDebugLog = 0;
  static unsigned long lastPhDebugLog = 0;
  static unsigned long lastTempDebugLog = 0;
  static unsigned long lastSensorRead = 0;

  // Limiter la fréquence de lecture des capteurs pH/ORP à toutes les 5 secondes
  // Cela évite de bloquer l'ESP32 trop souvent avec les lectures ADS1115
  const unsigned long SENSOR_READ_INTERVAL = 5000; // 5 secondes

  // ========== Lecture température DS18B20 sur GPIO 5 ==========
  // Non-bloquant: on déclenche une conversion, puis on lit après le temps de conversion.
  static unsigned long lastTempRequest = 0;
  static bool tempRequested = false;
  static unsigned long lastTempRead = 0;

  // Temps de conversion max dépend de la résolution configurée.
  // On ajoute une petite marge pour être sûr.
  const unsigned long TEMP_CONVERSION_MS = (unsigned long)g_ds18b20ConversionMs + 50;
  const unsigned long TEMP_REQUEST_INTERVAL_MS = 2000; // ne pas relancer trop souvent

  // 1) Lancer une conversion si aucune n'est en cours et si l'intervalle est passé
  if (!tempRequested && (now - lastTempRequest >= TEMP_REQUEST_INTERVAL_MS)) {
    tempSensor.requestTemperatures();
    tempRequested = true;
    lastTempRequest = now;
  }

  // 2) Lire la température seulement si la conversion a eu le temps de se terminer
  if (tempRequested && (now - lastTempRequest >= TEMP_CONVERSION_MS)) {
    float measuredTemp = tempSensor.getTempCByIndex(0);

    if (measuredTemp != DEVICE_DISCONNECTED_C && measuredTemp > -55.0f && measuredTemp < 125.0f) {
      // Stocker la valeur brute arrondie à 1 décimale
      tempRawValue = roundf(measuredTemp * 10.0f) / 10.0f;
      // Appliquer l'offset de calibration
      // Formule: Temp_final = Temp_brut + offset
      tempValue = tempRawValue + mqttCfg.tempCalibrationOffset;
    } else {
      tempValue = NAN;
      tempRawValue = NAN;
      systemLogger.warning("DS18B20 non détecté ou température invalide");
    }

    tempRequested = false; // prêt pour une nouvelle conversion
    lastTempRead = now;
  }

  // Debug température (toutes les 5 secondes)
  if (now - lastTempDebugLog >= 5000) {
    char logMsg[150];
    if (!isnan(tempValue)) {
      snprintf(logMsg, sizeof(logMsg),
               "Temp: %.2f°C | res=%dbit | age=%lums",
               tempValue, (int)g_ds18b20ResolutionBits, (unsigned long)(now - lastTempRead));
      Serial.printf("[TEMP DEBUG] DS18B20 GPIO 5 | %s\n", logMsg);
      systemLogger.debug(logMsg);
    } else {
      snprintf(logMsg, sizeof(logMsg),
               "Temp: NaN | res=%dbit | conversion=%s",
               (int)g_ds18b20ResolutionBits, tempRequested ? "EN COURS" : "IDLE");
      Serial.printf("[TEMP DEBUG] DS18B20 GPIO 5 | %s\n", logMsg);
      systemLogger.warning(logMsg);
    }
    lastTempDebugLog = now;
  }

  // Vérifier si l'intervalle de lecture des capteurs est atteint
  if (now - lastSensorRead < SENSOR_READ_INTERVAL) {
    return; // Trop tôt, ne pas lire les capteurs pH/ORP
  }
  lastSensorRead = now;

  // ========== Lecture ORP via ADS1115 canal A1 ==========
  {
    // Filtrage médian avec échantillonnage réduit
    // L'ADS1115 à 8 SPS fait déjà un filtrage interne précis
    // Nombre impair d'échantillons pour filtrage médian robuste
    const int numSamples = kNumSensorSamples;
    int16_t samples[numSamples];
    int32_t sum = 0;
    int16_t minVal = 32767;
    int16_t maxVal = -32768;

    // Collecte des échantillons depuis l'ADS1115 canal A1
    for (int i = 0; i < numSamples; i++) {
      int16_t reading = ads.readADC_SingleEnded(1);  // A1 pour ORP
      samples[i] = reading;
      sum += reading;
      if (reading < minVal) minVal = reading;
      if (reading > maxVal) maxVal = reading;
      // Pas de delay - l'ADS1115 à 8 SPS prend déjà kAds1115SampleTimeMs par lecture
    }

    // Tri des échantillons pour filtre médian
    for (int i = 0; i < numSamples - 1; i++) {
      for (int j = i + 1; j < numSamples; j++) {
        if (samples[i] > samples[j]) {
          int16_t temp = samples[i];
          samples[i] = samples[j];
          samples[j] = temp;
        }
      }
    }

    // Utiliser la médiane (plus robuste que la moyenne)
    int16_t rawAdc = samples[numSamples / 2];

    // Conversion ADC -> Voltage en utilisant la fonction de la bibliothèque
    // qui tient compte automatiquement du gain configuré
    float voltage = ads.computeVolts(rawAdc) * 1000.0f;  // Convertir V en mV

    // IMPORTANT: tu as un pont diviseur (R2=2.2k en haut, R3=10k en bas) pour passer ~0-4V -> ~0-3.28V.
    // Donc la tension mesurée par l'ADS1115 est la tension APRES diviseur.
    // On reconstruit la tension réelle de sortie du module ORP avant conversion ORP.
    constexpr float ORP_R_TOP_OHMS = 2200.0f;   // R2
    constexpr float ORP_R_BOTTOM_OHMS = 10000.0f; // R3
    constexpr float ORP_DIVIDER_GAIN = (ORP_R_TOP_OHMS + ORP_R_BOTTOM_OHMS) / ORP_R_BOTTOM_OHMS; // ~1.22
    float orpModuleVoltage_mV = voltage * ORP_DIVIDER_GAIN;

    // Avertissement si on s'approche de la pleine échelle du PGA (GAIN_ONE -> ~4096mV)
    if (fabsf(voltage) > 4050.0f) {
      systemLogger.warning("ORP: tension proche de la saturation ADS1115 (" + String(voltage, 1) + " mV). Vérifier VDD ADS1115 / diviseur de tension.");
    }

    // Avertissement si la tension reconstruite dépasse la plage attendue du module (~0-4V)
    if (orpModuleVoltage_mV < -50.0f || orpModuleVoltage_mV > 4100.0f) {
      systemLogger.warning("ORP: tension module inattendue (" + String(orpModuleVoltage_mV, 1) + " mV). Vérifier le pont diviseur / alim du module.");
    }

    // Approximation courante de ces modules : ORP(mV) ≈ 2000mV - Vout(mV)
    // => Vout=0mV -> +2000mV, Vout=2000mV -> 0mV, Vout=4000mV -> -2000mV
    // Note: Cette formule est une approximation et peut varier selon le module utilisé
    float rawOrpValue = 2000.0f - orpModuleVoltage_mV;

    // Appliquer la calibration (slope + offset) et arrondir au mV
    // Formule: ORP_final = (ORP_brut * slope) + offset
    // Calibration 1 point: slope=1.0, offset calculé
    // Calibration 2 points: slope et offset calculés depuis 2 solutions de référence
    orpValue = roundf((rawOrpValue * mqttCfg.orpCalibrationSlope) + mqttCfg.orpCalibrationOffset);

    // Debug: afficher les valeurs ORP toutes les 5 secondes
    if (now - lastOrpDebugLog >= 5000) {
      float voltageMin = ads.computeVolts(minVal) * 1000.0f;
      float voltageMax = ads.computeVolts(maxVal) * 1000.0f;
      float voltageAvg = ads.computeVolts(sum / numSamples) * 1000.0f;

      // Log vers Serial et système de logs
      char logMsg[200];
      snprintf(logMsg, sizeof(logMsg),
               "ORP: ADC=%d (avg=%d min=%d max=%d) | Vads=%.1fmV (avg=%.1f min=%.1f max=%.1f) | Vmod=%.1fmV | ORP=%.1fmV",
               rawAdc, (int)(sum/numSamples), minVal, maxVal,
               voltage, voltageAvg, voltageMin, voltageMax, orpModuleVoltage_mV, orpValue);
      Serial.printf("[ORP DEBUG] ADS1115 A1 | %s\n", logMsg);
      systemLogger.debug(logMsg);

      lastOrpDebugLog = now;
    }
  }

  // ========== Lecture pH via ADS1115 canal A0 avec filtrage médian ==========
  {
    // Filtrage médian avec échantillonnage réduit
    // L'ADS1115 à 8 SPS (125ms/échantillon) fait déjà un filtrage interne précis
    // 3 échantillons = ~375ms total, suffisant pour éliminer les pics de bruit
    const int numSamples = kNumSensorSamples;  // Nombre impair pour médiane
    int16_t samples[numSamples];

    for (int i = 0; i < numSamples; i++) {
      samples[i] = ads.readADC_SingleEnded(0);  // A0 pour pH
      // Pas de delay - l'ADS1115 prend déjà ~125ms par lecture à 8 SPS
    }

    // Tri par sélection pour trouver la médiane
    for (int i = 0; i < numSamples - 1; i++) {
      for (int j = i + 1; j < numSamples; j++) {
        if (samples[i] > samples[j]) {
          int16_t temp = samples[i];
          samples[i] = samples[j];
          samples[j] = temp;
        }
      }
    }

    // Utiliser la médiane (plus robuste que la moyenne)
    int16_t rawAdc = samples[numSamples / 2];

    // Conversion ADC -> Voltage en utilisant la fonction de la bibliothèque
    // qui tient compte automatiquement du gain configuré
    float voltage = ads.computeVolts(rawAdc) * 1000.0f;  // Convertir V en mV

    // Utiliser la température pour la compensation (si disponible)
    float temperature = isnan(tempValue) ? 25.0f : tempValue;

    // Calculer le pH avec calibration automatique et compensation de température
    // Arrondir à 1 décimale
    phValue = roundf(phSensor.readPH(voltage, temperature) * 10.0f) / 10.0f;

    // Debug: afficher les valeurs pH toutes les 5 secondes
    if (now - lastPhDebugLog >= 5000) {
      int16_t minVal = samples[0];
      int16_t maxVal = samples[numSamples - 1];
      float voltageMin = ads.computeVolts(minVal) * 1000.0f;
      float voltageMax = ads.computeVolts(maxVal) * 1000.0f;

      // Log vers Serial et système de logs
      char logMsg[200];
      snprintf(logMsg, sizeof(logMsg),
               "pH: ADC=%d (min=%d max=%d) | V=%.1fmV (min=%.1f max=%.1f) | Temp=%.1f°C | pH=%.2f",
               rawAdc, minVal, maxVal, voltage, voltageMin, voltageMax, temperature, phValue);
      Serial.printf("[pH DEBUG] ADS1115 A0 | %s\n", logMsg);
      systemLogger.debug(logMsg);

      lastPhDebugLog = now;
    }
  }

  sensorsInitialized = true;
}


float SensorManager::getRawOrp() const {
  // Inverse de la calibration: ORP_final = (raw × slope) + offset
  // Donc: raw = (ORP_final - offset) / slope
  if (mqttCfg.orpCalibrationSlope == 0.0f) {
    // Protection division par zéro - fallback
    return orpValue - mqttCfg.orpCalibrationOffset;
  }
  return (orpValue - mqttCfg.orpCalibrationOffset) / mqttCfg.orpCalibrationSlope;
}

float SensorManager::getRawPh() const {
  // Avec DFRobot_PH, la librairie gère la calibration en interne
  // On retourne la valeur calibrée directement
  return phValue;
}

void SensorManager::recalculateCalibratedValues() {
  // Recalculer la température calibrée à partir de la valeur brute
  if (!isnan(tempRawValue)) {
    tempValue = tempRawValue + mqttCfg.tempCalibrationOffset;
  }

  // Note: On ne recalcule PAS le pH ici car la bibliothèque DFRobot_PH
  // gère sa propre calibration dans l'EEPROM. Recalculer avec une ancienne
  // tension après une nouvelle calibration pourrait donner des valeurs incohérentes.
  // Le pH sera mis à jour naturellement lors de la prochaine lecture du capteur.

  // Recalculer l'ORP calibré à partir de la valeur brute
  // Formule: ORP_final = (ORP_brut × slope) + offset
  float rawOrp = getRawOrp();
  if (!isnan(rawOrp)) {
    orpValue = (rawOrp * mqttCfg.orpCalibrationSlope) + mqttCfg.orpCalibrationOffset;
  }
}

void SensorManager::publishValues() {
  if (!sensorsInitialized) return;

  // Cette fonction est appelée par mqtt_manager
  // On la laisse vide ici pour éviter la dépendance circulaire
}

// ========== Calibration pH (DFRobot_PH) ==========

void SensorManager::calibratePhNeutral() {
  // Lire la tension actuelle depuis l'ADS1115 canal A0 (filtre médian)
  const int numSamples = kNumSensorSamples;
  int16_t samples[numSamples];

  for (int i = 0; i < numSamples; i++) {
    samples[i] = ads.readADC_SingleEnded(0);
    // Pas de delay - l'ADS1115 prend déjà ~125ms par lecture à 8 SPS
  }

  // Tri pour obtenir la médiane
  for (int i = 0; i < numSamples - 1; i++) {
    for (int j = i + 1; j < numSamples; j++) {
      if (samples[i] > samples[j]) {
        int16_t tmp = samples[i];
        samples[i] = samples[j];
        samples[j] = tmp;
      }
    }
  }

  int16_t rawAdc = samples[numSamples / 2];
  int16_t minVal = samples[0];
  int16_t maxVal = samples[numSamples - 1];

  // Conversion en mV en utilisant la fonction de la bibliothèque
  float voltage = ads.computeVolts(rawAdc) * 1000.0f;

  // Utiliser la température pour la compensation
  float temperature = isnan(tempValue) ? 25.0f : tempValue;

  // Processus de calibration DFRobot_PH en 3 étapes:
  // 1. Entrer en mode calibration
  phSensor.calibration(voltage, temperature, (char*)"enterph");

  // 2. Calibrer (reconnaît automatiquement pH 7.0)
  phSensor.calibration(voltage, temperature, (char*)"calph");

  // 3. Sauvegarder et sortir
  phSensor.calibration(voltage, temperature, (char*)"exitph");

  // IMPORTANT: Sur ESP32, commit les changements EEPROM
  EEPROM.commit();

  systemLogger.info("Calibration pH point neutre (7.0) effectuée à " + String(temperature, 1) + "°C (ADC med=" + String(rawAdc) + ", min=" + String(minVal) + ", max=" + String(maxVal) + ", V=" + String(voltage, 2) + " mV)");
}

void SensorManager::calibratePhAcid() {
  // Lire la tension actuelle depuis l'ADS1115 canal A0 (filtre médian)
  const int numSamples = kNumSensorSamples;
  int16_t samples[numSamples];

  for (int i = 0; i < numSamples; i++) {
    samples[i] = ads.readADC_SingleEnded(0);
    // Pas de delay - l'ADS1115 prend déjà ~125ms par lecture à 8 SPS
  }

  // Tri pour obtenir la médiane
  for (int i = 0; i < numSamples - 1; i++) {
    for (int j = i + 1; j < numSamples; j++) {
      if (samples[i] > samples[j]) {
        int16_t tmp = samples[i];
        samples[i] = samples[j];
        samples[j] = tmp;
      }
    }
  }

  int16_t rawAdc = samples[numSamples / 2];
  int16_t minVal = samples[0];
  int16_t maxVal = samples[numSamples - 1];

  // Conversion en mV en utilisant la fonction de la bibliothèque
  float voltage = ads.computeVolts(rawAdc) * 1000.0f;

  // Utiliser la température pour la compensation
  float temperature = isnan(tempValue) ? 25.0f : tempValue;

  // Processus de calibration DFRobot_PH en 3 étapes:
  // 1. Entrer en mode calibration
  phSensor.calibration(voltage, temperature, (char*)"enterph");

  // 2. Calibrer (reconnaît automatiquement pH 4.0)
  phSensor.calibration(voltage, temperature, (char*)"calph");

  // 3. Sauvegarder et sortir
  phSensor.calibration(voltage, temperature, (char*)"exitph");

  // IMPORTANT: Sur ESP32, commit les changements EEPROM
  EEPROM.commit();

  systemLogger.info("Calibration pH point acide (4.0) effectuée à " + String(temperature, 1) + "°C (ADC med=" + String(rawAdc) + ", min=" + String(minVal) + ", max=" + String(maxVal) + ", V=" + String(voltage, 2) + " mV)");
}

void SensorManager::calibratePhAlkaline() {
  // Note: DFRobot_PH ne supporte que 2 points (4.0 et 7.0)
  // Pour pH 9.18, utiliser la calibration 2 points standard
  systemLogger.warning("DFRobot_PH ne supporte que calibration 2 points (pH 4.0 et 7.0) - utilisez calibratePhAcid() et calibratePhNeutral()");
}

void SensorManager::clearPhCalibration() {
  // Effacer l'EEPROM utilisé par DFRobot_PH (adresses 0-7)
  for (int i = 0; i < 8; i++) {
    EEPROM.write(i, 0xFF);
  }
  EEPROM.commit();

  // Réinitialiser le capteur pH avec les valeurs par défaut
  phSensor.begin();
  EEPROM.commit();

  systemLogger.info("Calibration pH effacée - réinitialisée aux valeurs par défaut");
}
