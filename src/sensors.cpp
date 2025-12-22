#include "sensors.h"
#include "config.h"
#include "logger.h"
#include "mqtt_manager.h"
#include <sys/time.h>
#include <EEPROM.h>
#include <Wire.h>

SensorManager sensors;

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
  systemLogger.info("Capteur de température DS18B20 initialisé sur GPIO 5");

  // IMPORTANT: Sur ESP32, l'EEPROM doit être initialisé avant utilisation
  // La librairie DFRobot_PH utilise les adresses 0-7 (8 bytes)
  EEPROM.begin(512);  // Allouer 512 bytes pour l'EEPROM

  // Initialiser le capteur pH DFRobot
  phSensor.begin();

  // Commit les changements EEPROM (nécessaire sur ESP32)
  EEPROM.commit();

  systemLogger.info("Capteur pH DFRobot SEN0161-V2 initialisé");

  if (simulationCfg.enabled) {
    initializeSimulation();
  }

  systemLogger.info("Gestionnaire de capteurs initialisé (mode " +
                    String(simulationCfg.enabled ? "SIMULATION" : "RÉEL") + ")");
}

void SensorManager::initializeSimulation() {
  phValue = simulationCfg.initialPh;
  orpValue = simulationCfg.initialOrp;
  tempValue = simulationCfg.initialTemp;
  sensorsInitialized = true;

  if (simulationCfg.overrideClock) {
    struct tm tmInfo;
    memset(&tmInfo, 0, sizeof(tmInfo));
    tmInfo.tm_year = 124; // 2024
    tmInfo.tm_mon = 0;
    tmInfo.tm_mday = 1;
    tmInfo.tm_hour = 8;
    tmInfo.tm_min = 0;
    time_t base = mktime(&tmInfo);
    if (base > 0) {
      struct timeval tv{.tv_sec = base, .tv_usec = 0};
      settimeofday(&tv, nullptr);
      simulatedTimeOffsetMs = 0;
      systemLogger.info("Horloge simulation initialisée");
    }
  }
}

void SensorManager::update() {
  static unsigned long lastModeLog = 0;
  unsigned long now = millis();

  // Log du mode toutes les 10 secondes
  if (now - lastModeLog >= 10000) {
    Serial.printf("[SENSOR MODE] %s | ORP Pin: %d | pH Pin: %d\n",
                  simulationCfg.enabled ? "SIMULATION" : "REAL",
                  mqttCfg.orpSensorPin, mqttCfg.phSensorPin);
    lastModeLog = now;
  }

  if (simulationCfg.enabled) {
    updateSimulation(millis());
  } else {
    readRealSensors();
  }
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
  // Déclencher une requête de lecture (non-bloquant)
  static unsigned long lastTempRequest = 0;
  static bool tempRequested = false;

  if (!tempRequested || (now - lastTempRequest >= 1000)) {
    tempSensor.requestTemperatures();
    tempRequested = true;
    lastTempRequest = now;
  }

  // Lire la température (la conversion prend ~750ms pour 12-bit)
  float measuredTemp = tempSensor.getTempCByIndex(0);

  if (measuredTemp != DEVICE_DISCONNECTED_C && measuredTemp > -55.0f && measuredTemp < 125.0f) {
    // Arrondir à 1 décimale
    tempValue = roundf(measuredTemp * 10.0f) / 10.0f;
  } else {
    tempValue = NAN;
    if (now - lastTempDebugLog >= 5000) {
      systemLogger.warning("DS18B20 non détecté ou température invalide");
    }
  }

  // Debug température
  if (now - lastTempDebugLog >= 5000) {
    Serial.printf("[TEMP DEBUG] DS18B20 GPIO 5 | Temp=%.2f°C\n", tempValue);
    lastTempDebugLog = now;
  }

  // Vérifier si l'intervalle de lecture des capteurs est atteint
  if (now - lastSensorRead < SENSOR_READ_INTERVAL) {
    return; // Trop tôt, ne pas lire les capteurs pH/ORP
  }
  lastSensorRead = now;

  // ========== Lecture ORP via ADS1115 canal A1 ==========
  if (mqttCfg.orpSensorPin >= 0) {
    // Filtrage médian avec échantillonnage réduit
    // L'ADS1115 à 8 SPS (125ms/échantillon) fait déjà un filtrage interne précis
    // 3 échantillons = ~375ms total, suffisant pour éliminer les pics de bruit
    const int numSamples = 3;
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
      // Pas de delay - l'ADS1115 prend déjà ~125ms par lecture à 8 SPS
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

    // Conversion voltage -> ORP
    // Module ORP: 0-3.3V = 0-2000mV ORP
    // ORP (mV) = (voltage / 3300) * 2000
    float rawOrpValue = (voltage / 3300.0f) * 2000.0f;

    // Appliquer l'offset de calibration et arrondir au mV
    orpValue = roundf(rawOrpValue + mqttCfg.orpCalibrationOffset);

    // Debug: afficher les valeurs ORP toutes les 2 secondes
    if (now - lastOrpDebugLog >= 2000) {
      float voltageMin = ads.computeVolts(minVal) * 1000.0f;
      float voltageMax = ads.computeVolts(maxVal) * 1000.0f;
      float voltageAvg = ads.computeVolts(sum / numSamples) * 1000.0f;
      Serial.printf("[ORP DEBUG] ADS1115 A1 | Median ADC=%d | Avg ADC=%d | Min=%d Max=%d | V=%.1f (avg=%.1f min=%.1f max=%.1f) | ORP=%.1f mV\n",
                    rawAdc, (int)(sum/numSamples), minVal, maxVal,
                    voltage, voltageAvg, voltageMin, voltageMax, orpValue);
      lastOrpDebugLog = now;
    }
  } else {
    orpValue = NAN;  // Pas de capteur configuré
  }

  // ========== Lecture pH via ADS1115 canal A0 avec filtrage médian ==========
  if (mqttCfg.phSensorPin >= 0) {
    // Filtrage médian avec échantillonnage réduit
    // L'ADS1115 à 8 SPS (125ms/échantillon) fait déjà un filtrage interne précis
    // 3 échantillons = ~375ms total, suffisant pour éliminer les pics de bruit
    const int numSamples = 3;  // Nombre impair pour médiane
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
      Serial.printf("[pH DEBUG] ADS1115 A0 | Median ADC=%d | Min=%d Max=%d | V=%.1f (min=%.1f max=%.1f) | Temp=%.1f°C | pH=%.2f\n",
                    rawAdc, minVal, maxVal, voltage, voltageMin, voltageMax, temperature, phValue);
      lastPhDebugLog = now;
    }
  } else {
    phValue = NAN;  // Pas de capteur configuré
  }

  sensorsInitialized = true;
}

void SensorManager::updateSimulation(unsigned long now) {
  if (!sensorsInitialized) {
    initializeSimulation();
    lastSimulationUpdateMs = now;
    return;
  }

  if (lastSimulationUpdateMs == 0) {
    lastSimulationUpdateMs = now;
    return;
  }

  unsigned long deltaMs = now - lastSimulationUpdateMs;
  if (deltaMs < 100) return; // Update every 100ms for smoother simulation

  lastSimulationUpdateMs = now;

  // 1. Mise à jour de l'horloge accélérée
  updateAcceleratedClock(deltaMs);

  // 2. Calculer le temps écoulé en temps simulé
  float accel = max(0.1f, simulationCfg.timeAcceleration);
  float minutesSimulated = (deltaMs / 60000.0f) * accel;
  float hoursSimulated = minutesSimulated / 60.0f;

  if (minutesSimulated <= 0.0f) return;

  bool valuesChanged = false;

  // 3. Injection de produits chimiques (ajout dans le réservoir "tampon")
  applyChemicalInjection(minutesSimulated);

  // 4. Dynamique de mélange progressif (transfert du tampon vers la piscine)
  float oldPh = phValue;
  float oldOrp = orpValue;
  applyMixingDynamics(hoursSimulated);

  // 5. Dérive naturelle (évaporation, UV, etc.)
  applyNaturalDrift(hoursSimulated);

  // 6. Limiter les valeurs dans les plages physiques
  phValue = constrain(phValue, 0.0f, 14.0f);
  orpValue = constrain(orpValue, 0.0f, 1000.0f);

  // 7. Publier si les valeurs ont changé de manière significative
  if (fabsf(phValue - oldPh) > 0.001f || fabsf(orpValue - oldOrp) > 0.1f) {
    publishValues();
  }

  // Log de debug périodique (toutes les 30 secondes)
  static unsigned long lastSimLog = 0;
  if (millis() - lastSimLog > 30000) {
    systemLogger.info("SIM: pH=" + String(phValue, 3) +
                     " (Δ" + String(phValue - oldPh, 4) + ")" +
                     " dose=" + String(phDoseActive ? "ON" : "OFF") +
                     " flow=" + String(phCurrentFlowMlPerMin, 1) + "ml/min" +
                     " pending=" + String(phPendingEffect, 4));
    lastSimLog = millis();
  }
}

void SensorManager::updateAcceleratedClock(unsigned long deltaMs) {
  if (!simulationCfg.overrideClock) return;

  float accel = max(0.1f, simulationCfg.timeAcceleration);
  simulatedTimeOffsetMs += static_cast<int64_t>(deltaMs * (accel - 1.0f));

  int64_t offsetSec = simulatedTimeOffsetMs / 1000;
  if (offsetSec != 0) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec += offsetSec;
    tv.tv_usec += (simulatedTimeOffsetMs % 1000) * 1000;
    if (tv.tv_usec >= 1000000) {
      tv.tv_sec += 1;
      tv.tv_usec -= 1000000;
    }
    settimeofday(&tv, nullptr);
    simulatedTimeOffsetMs %= 1000;
  }
  simulationClockMs += static_cast<double>(deltaMs) * accel;
}

void SensorManager::applyChemicalInjection(float minutesElapsed) {
  // Injection de pH- (acide) -> ajoute un effet négatif en attente
  if (phDoseActive && phCurrentFlowMlPerMin > 0.0f) {
    float mlInjected = phCurrentFlowMlPerMin * minutesElapsed;
    float litersInjected = mlInjected / 1000.0f;

    // Calcul de l'effet théorique pour 10m³, puis ajusté au volume réel
    float effectFor10m3 = litersInjected * simulationCfg.phMinusEffectPerLiter;
    float effectForPool = effectFor10m3 * (10.0f / simulationCfg.poolVolumeM3);

    // Ajoute à l'effet en attente (négatif car c'est du pH-)
    phPendingEffect += effectForPool;

    // Log de debug (toutes les 10 secondes environ)
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 10000) {
      systemLogger.debug("pH- injection: " + String(mlInjected, 2) + "ml, effet=" + String(effectForPool, 4) +
                        ", pending=" + String(phPendingEffect, 4) + ", pH=" + String(phValue, 2));
      lastLog = millis();
    }
  }

  // Injection de chlore -> ajoute un effet positif en attente
  if (orpDoseActive && orpCurrentFlowMlPerMin > 0.0f) {
    float mlInjected = orpCurrentFlowMlPerMin * minutesElapsed;
    float litersInjected = mlInjected / 1000.0f;

    // Calcul de l'effet théorique pour 10m³, puis ajusté au volume réel
    float effectFor10m3 = litersInjected * simulationCfg.chlorineEffectPerLiter;
    float effectForPool = effectFor10m3 * (10.0f / simulationCfg.poolVolumeM3);

    // Ajoute à l'effet en attente (positif)
    orpPendingEffect += effectForPool;
  }
}

void SensorManager::applyMixingDynamics(float hoursElapsed) {
  // Calcul du temps de cycle de filtration (temps pour faire circuler tout le volume)
  float cycleTimeHours = simulationCfg.poolVolumeM3 / simulationCfg.filtrationFlowM3PerHour;
  if (cycleTimeHours < 0.1f) cycleTimeHours = 0.1f;

  // Fraction de cycle écoulée
  float cyclesFraction = hoursElapsed / cycleTimeHours;

  // Calcul de la constante de transfert (basée sur la constante de temps de mélange)
  // Une constante de 0.5 signifie que 63% du produit est mélangé après 0.5 cycle
  float phTransferRate = 1.0f - expf(-cyclesFraction / max(0.01f, simulationCfg.phMixingTimeConstant));
  float orpTransferRate = 1.0f - expf(-cyclesFraction / max(0.01f, simulationCfg.orpMixingTimeConstant));

  // Transfert progressif de l'effet en attente vers les valeurs réelles
  float phTransferred = phPendingEffect * phTransferRate;
  float orpTransferred = orpPendingEffect * orpTransferRate;

  phValue += phTransferred;
  orpValue += orpTransferred;

  // Réduit l'effet en attente
  phPendingEffect -= phTransferred;
  orpPendingEffect -= orpTransferred;

  // Log de debug (toutes les 10 secondes environ)
  static unsigned long lastMixLog = 0;
  if (millis() - lastMixLog > 10000 && fabsf(phTransferred) > 0.001f) {
    systemLogger.debug("Mélange pH: transfert=" + String(phTransferred, 4) +
                      ", pending=" + String(phPendingEffect, 4) +
                      ", rate=" + String(phTransferRate * 100, 1) + "%");
    lastMixLog = millis();
  }
}

void SensorManager::applyNaturalDrift(float hoursElapsed) {
  // Dérive exponentielle vers l'équilibre naturel
  // Formule : valeur += (équilibre - valeur) × vitesse × temps
  // Cela crée un retour progressif vers l'équilibre (asymptotique)

  // Le pH dérive vers son équilibre naturel (généralement légèrement basique)
  float phDrift = (simulationCfg.phNaturalEquilibrium - phValue) * simulationCfg.phDriftSpeed * hoursElapsed;
  phValue += phDrift;

  // L'ORP dérive vers son équilibre naturel (généralement bas sans chlore)
  float orpDrift = (simulationCfg.orpNaturalEquilibrium - orpValue) * simulationCfg.orpDriftSpeed * hoursElapsed;
  orpValue += orpDrift;
}

void SensorManager::setPhDoseActive(bool active, float flowMlPerMin) {
  phDoseActive = active;
  phCurrentFlowMlPerMin = flowMlPerMin;
}

void SensorManager::setOrpDoseActive(bool active, float flowMlPerMin) {
  orpDoseActive = active;
  orpCurrentFlowMlPerMin = flowMlPerMin;
}

float SensorManager::getRawOrp() const {
  return orpValue - mqttCfg.orpCalibrationOffset;
}

float SensorManager::getRawPh() const {
  // Avec DFRobot_PH, la librairie gère la calibration en interne
  // On retourne la valeur calibrée directement
  return phValue;
}

void SensorManager::publishValues() {
  if (!sensorsInitialized) return;

  // Cette fonction est appelée par mqtt_manager
  // On la laisse vide ici pour éviter la dépendance circulaire
}

// ========== Calibration pH (DFRobot_PH) ==========

void SensorManager::calibratePhNeutral() {
  if (mqttCfg.phSensorPin >= 0) {
    // Lire la tension actuelle depuis l'ADS1115 canal A0
    int16_t rawAdc = ads.readADC_SingleEnded(0);
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

    systemLogger.info("Calibration pH point neutre (7.0) effectuée à " + String(temperature, 1) + "°C (ADC: " + String(rawAdc) + ", voltage: " + String(voltage, 2) + " mV)");
  }
}

void SensorManager::calibratePhAcid() {
  if (mqttCfg.phSensorPin >= 0) {
    // Lire la tension actuelle depuis l'ADS1115 canal A0
    int16_t rawAdc = ads.readADC_SingleEnded(0);
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

    systemLogger.info("Calibration pH point acide (4.0) effectuée à " + String(temperature, 1) + "°C (ADC: " + String(rawAdc) + ", voltage: " + String(voltage, 2) + " mV)");
  }
}

void SensorManager::calibratePhAlkaline() {
  if (mqttCfg.phSensorPin >= 0) {
    // Note: DFRobot_PH ne supporte que 2 points (4.0 et 7.0)
    // Pour pH 9.18, utiliser la calibration 2 points standard
    systemLogger.warning("DFRobot_PH ne supporte que calibration 2 points (pH 4.0 et 7.0) - utilisez calibratePhAcid() et calibratePhNeutral()");
  }
}

void SensorManager::clearPhCalibration() {
  if (mqttCfg.phSensorPin >= 0) {
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
}
