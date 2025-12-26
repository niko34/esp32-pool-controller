#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <DFRobot_PH.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>

class SensorManager {
private:
  DFRobot_PH phSensor;
  Adafruit_ADS1115 ads;  // ADC 16 bits I2C
  OneWire oneWire;
  DallasTemperature tempSensor;

  float orpValue = 0.0f;
  float phValue = 0.0f;
  float tempValue = NAN;
  float tempRawValue = NAN;
  bool sensorsInitialized = false;

  void readRealSensors();

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
  float getRawTemperature() const { return tempRawValue; }

  // Calibration pH (DFRobot_PH)
  void calibratePhNeutral();         // Calibration point neutre (pH 7.0)
  void calibratePhAcid();            // Calibration point acide (pH 4.0)
  void calibratePhAlkaline();        // Calibration point alcalin (pH 9.18)
  void clearPhCalibration();         // Effacer calibration

  // Recalcul des valeurs calibrées (température, ORP) après modification de l'offset
  void recalculateCalibratedValues();

  // Publication des valeurs
  void publishValues();
};

extern SensorManager sensors;

#endif // SENSORS_H
