#include "sensors.h"
#include "config.h"
#include "logger.h"
#include "mqtt_manager.h"
#include <sys/time.h>

SensorManager sensors;

SensorManager::SensorManager() : oneWire(nullptr), tempSensor(nullptr) {}

SensorManager::~SensorManager() {
  delete tempSensor;
  delete oneWire;
}

void SensorManager::begin() {
  oneWire = new OneWire(TEMP_SENSOR_PIN);
  tempSensor = new DallasTemperature(oneWire);
  tempSensor->begin();

  // Configuration de l'atténuation de l'ADC pour une meilleure précision
  // ADC_11db = 0-3.3V (par défaut mais explicite)
  if (mqttCfg.orpSensorPin >= 0) {
    analogSetPinAttenuation(mqttCfg.orpSensorPin, ADC_11db);
  }
  if (mqttCfg.phSensorPin >= 0) {
    analogSetPinAttenuation(mqttCfg.phSensorPin, ADC_11db);
  }

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

  // Lecture non-bloquante de la température
  if (!tempRequestPending) {
    tempSensor->requestTemperatures();
    tempRequestPending = true;
    lastTempRequest = now;
  } else if (now - lastTempRequest >= TEMP_CONVERSION_TIME) {
    float measuredTemp = tempSensor->getTempCByIndex(0);
    if (measuredTemp > -55.0f && measuredTemp < 125.0f) {
      tempValue = measuredTemp;
    } else {
      tempValue = NAN;
      systemLogger.warning("Température hors limites: " + String(measuredTemp));
    }
    tempRequestPending = false;
  }

  // Lecture analogique ORP et pH (seulement si les capteurs sont configurés)
  if (mqttCfg.orpSensorPin >= 0) {
    // Filtrage multi-échantillons pour réduire le bruit WiFi de l'ADC ESP32
    const int numSamples = 20;  // Plus d'échantillons pour mieux filtrer
    int samples[numSamples];
    int sum = 0;
    int minVal = 4095;
    int maxVal = 0;

    // Collecte des échantillons
    for (int i = 0; i < numSamples; i++) {
      int reading = analogRead(mqttCfg.orpSensorPin);
      samples[i] = reading;
      sum += reading;
      if (reading < minVal) minVal = reading;
      if (reading > maxVal) maxVal = reading;
      delayMicroseconds(200); // Pause pour éviter les lectures consécutives
    }

    // Tri des échantillons pour filtre médian
    for (int i = 0; i < numSamples - 1; i++) {
      for (int j = i + 1; j < numSamples; j++) {
        if (samples[i] > samples[j]) {
          int temp = samples[i];
          samples[i] = samples[j];
          samples[j] = temp;
        }
      }
    }

    // Utiliser la médiane (plus robuste que la moyenne contre valeurs aberrantes)
    int rawOrp = samples[numSamples / 2];

    // Conversion pour module ORP 0-3.3V = 0-2000mV
    // Tension mesurée: (rawOrp / 4095) * 3.3V
    // ORP en mV: (Tension / 3.3V) * 2000mV = (rawOrp / 4095) * 2000
    float rawOrpValue = (rawOrp / 4095.0f) * 2000.0f;
    // Appliquer l'offset de calibration
    orpValue = rawOrpValue + mqttCfg.orpCalibrationOffset;

    // Debug: afficher les valeurs ORP toutes les 2 secondes
    if (now - lastOrpDebugLog >= 2000) {
      float voltage = (rawOrp / 4095.0f) * 3.3f;
      float voltageMin = (minVal / 4095.0f) * 3.3f;
      float voltageMax = (maxVal / 4095.0f) * 3.3f;
      float voltageAvg = (sum / (float)numSamples / 4095.0f) * 3.3f;
      Serial.printf("[ORP DEBUG] GPIO=%d | Median ADC=%d | Avg ADC=%d | Min=%d Max=%d | V=%.3f (avg=%.3f min=%.3f max=%.3f) | ORP=%.1f mV\n",
                    mqttCfg.orpSensorPin, rawOrp, sum/numSamples, minVal, maxVal,
                    voltage, voltageAvg, voltageMin, voltageMax, orpValue);
      lastOrpDebugLog = now;
    }
  } else {
    orpValue = NAN;  // Pas de capteur configuré
  }

  if (mqttCfg.phSensorPin >= 0) {
    // Moyennage sur plusieurs lectures pour réduire le bruit de l'ADC ESP32
    const int numSamples = 10;
    int sum = 0;

    for (int i = 0; i < numSamples; i++) {
      sum += analogRead(mqttCfg.phSensorPin);
      delayMicroseconds(100);
    }

    int rawPh = sum / numSamples;

    // Conversion basique (à calibrer selon vos capteurs)
    float rawPhValue = (rawPh / 4095.0f) * 14.0f;
    // Appliquer l'offset de calibration
    phValue = rawPhValue + mqttCfg.phCalibrationOffset;
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
  return phValue - mqttCfg.phCalibrationOffset;
}

void SensorManager::publishValues() {
  if (!sensorsInitialized) return;

  // Cette fonction est appelée par mqtt_manager
  // On la laisse vide ici pour éviter la dépendance circulaire
}
