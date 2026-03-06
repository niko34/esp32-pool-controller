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

  float orpValue = NAN;
  float phValue = NAN;
  float phVoltageMv = NAN;  // Tension brute ADS1115 canal A0 (avant calibration pH)
  float tempValue = NAN;
  float tempRawValue = NAN;
  bool sensorsInitialized = false;
  bool adsAvailable = false;   // true si ADS1115 détecté au démarrage
  bool _phCalibrated = false;  // true si EEPROM diffère des valeurs par défaut

  void readRealSensors();

  // Helper: lecture médiane depuis ADS1115 avec filtrage
  // Retourne la valeur médiane, et optionnellement les stats (min, max, sum)
  int16_t readMedianAdsChannel(uint8_t channel, int numSamples = 3,
                                int16_t* outMin = nullptr, int16_t* outMax = nullptr, int32_t* outSum = nullptr);

public:
  SensorManager();
  ~SensorManager();

  void begin();
  void update(); // Appelé dans loop() - non bloquant

  // Getters
  float getOrp() const { return orpValue; }
  float getPh() const { return phValue; }
  float getPhVoltageMv() const { return phVoltageMv; }
  float getTemperature() const { return tempValue; }
  bool isInitialized() const { return sensorsInitialized; }
  bool isPhCalibrated() const { return _phCalibrated; }

  // Getters pour valeurs brutes (sans offset de calibration)
  float getRawOrp() const;
  float getRawPh() const;
  float getRawTemperature() const { return tempRawValue; }

  // Calibration pH (DFRobot_PH)
  void calibratePhNeutral();         // Calibration point neutre (pH 7.0)
  void calibratePhAcid();            // Calibration point acide (pH 4.0)
  void calibratePhAlkaline();        // Calibration point alcalin (pH 9.18)
  void clearPhCalibration();         // Effacer calibration

  void detectAdsIfNeeded();

  // Recalcul des valeurs calibrées (température, ORP) après modification de l'offset
  void recalculateCalibratedValues();

  // Publication des valeurs
  void publishValues();
};

extern SensorManager sensors;

#endif // SENSORS_H
