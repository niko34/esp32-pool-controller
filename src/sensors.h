#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DFRobot_PH.h>

class SensorManager {
private:
  OneWire* oneWire;
  DallasTemperature* tempSensor;
  DFRobot_PH phSensor;

  float orpValue = 0.0f;
  float phValue = 0.0f;
  float tempValue = NAN;
  bool sensorsInitialized = false;

  unsigned long lastTempRequest = 0;
  bool tempRequestPending = false;
  static const unsigned long TEMP_CONVERSION_TIME = 750; // ms pour DS18B20

  // Variables pour simulation - Horloge accélérée
  unsigned long lastSimulationUpdateMs = 0;
  bool simulationClockInitialized = false;
  double simulationClockMs = 0.0;
  int64_t simulatedTimeOffsetMs = 0;

  // Variables pour simulation - État actuel
  bool phDoseActive = false;
  bool orpDoseActive = false;
  float phCurrentFlowMlPerMin = 0.0f;
  float orpCurrentFlowMlPerMin = 0.0f;

  // Variables pour simulation - Modèle d'inertie avec mélange progressif
  // Le produit injecté se trouve dans un "réservoir tampon" qui se mélange progressivement
  float phPendingEffect = 0.0f;        // Effet du pH- en attente de mélange (en unités pH)
  float orpPendingEffect = 0.0f;       // Effet du chlore en attente de mélange (en mV)

  // Fonctions de simulation
  void updateAcceleratedClock(unsigned long deltaMs);
  void applyNaturalDrift(float hoursElapsed);
  void applyChemicalInjection(float minutesElapsed);
  void applyMixingDynamics(float hoursElapsed);

  void updateSimulation(unsigned long now);
  void readRealSensors();
  void initializeSimulation();

public:
  SensorManager();
  ~SensorManager();

  void begin();
  void update(); // Appelé dans loop() - non bloquant

  // Getters
  float getOrp() const { return orpValue; }
  float getPh() const { return phValue; }
  float getTemperature() const { return tempValue; }
  bool isInitialized() const { return sensorsInitialized; }

  // Getters pour valeurs brutes (sans offset de calibration)
  float getRawOrp() const;
  float getRawPh() const;

  // Getters pour simulation - effets en attente
  float getPhPendingEffect() const { return phPendingEffect; }
  float getOrpPendingEffect() const { return orpPendingEffect; }

  // Setters pour simulation
  void setPhDoseActive(bool active, float flowMlPerMin = 0.0f);
  void setOrpDoseActive(bool active, float flowMlPerMin = 0.0f);

  // Calibration pH (DFRobot_PH)
  void calibratePhNeutral();         // Calibration point neutre (pH 7.0)
  void calibratePhAcid();            // Calibration point acide (pH 4.0)
  void calibratePhAlkaline();        // Calibration point alcalin (pH 9.18)
  void clearPhCalibration();         // Effacer calibration

  // Publication des valeurs
  void publishValues();
};

extern SensorManager sensors;

#endif // SENSORS_H
