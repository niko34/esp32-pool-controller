#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <DFRobot_PH.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "constants.h"

// Rôle attribué à une sonde DS18B20 (feature-020)
enum class SondeRole : uint8_t {
  Unknown = 0, // Sonde détectée mais non identifiée par l'utilisateur
  Water,       // Sonde eau de la piscine (consommée par compensation pH/ORP)
  Circuit      // Sonde de circuit interne (T° boîtier électronique)
};

// Informations d'une sonde DS18B20 détectée sur le bus OneWire (feature-020)
struct SondeInfo {
  uint8_t addr[kSondeAddrLen]; // Adresse ROM 1-Wire 64 bits (unique par sonde)
  float lastTempRaw;           // Dernière température brute lue (°C, NaN si erreur)
  bool present;                // true si sonde détectée et lue avec succès
  SondeRole role;              // Rôle attribué (eau, circuit, ou inconnu)
};

class SensorManager {
private:
  DFRobot_PH phSensor;
  Adafruit_ADS1115 ads;  // ADC 16 bits I2C
  OneWire oneWire;
  DallasTemperature tempSensor;

  float orpValue = NAN;
  float phValue = NAN;
  float phVoltageMv = NAN;  // Tension brute ADS1115 canal A0 (avant calibration pH)
  // Note : tempValue/tempRawValue sont conservés pour rétrocompat (alias eau)
  // mais sont alimentés depuis _sondes[].lastTempRaw via getWaterTemperature().
  float tempValue = NAN;
  float tempRawValue = NAN;
  bool sensorsInitialized = false;
  bool adsAvailable = false;   // true si ADS1115 détecté au démarrage
  bool _phCalibrated = false;  // true si EEPROM diffère des valeurs par défaut

  // ===== Architecture 2 sondes DS18B20 (feature-020) =====
  SondeInfo _sondes[kMaxDs18b20Sondes];
  uint8_t _detectedCount = 0;    // Nombre de sondes physiquement présentes (0..kMaxDs18b20Sondes)

  // Charge les adresses identifiées depuis NVS et les matche avec les sondes détectées.
  void _loadSondeIdentificationFromNvs();
  // Sauvegarde une adresse identifiée en NVS sous la clé spécifiée.
  bool _saveSondeAddrToNvs(const char* nvsKey, const uint8_t addr[kSondeAddrLen]);
  // Recherche dans _sondes l'index de la sonde portant ce rôle (-1 si aucun).
  int _findSondeIndexByRole(SondeRole role) const;
  // Recherche dans _sondes l'index de la sonde ayant cette adresse (-1 si aucun).
  int _findSondeIndexByAddr(const uint8_t addr[kSondeAddrLen]) const;

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
  // getTemperature() reste un alias rétrocompat de la T° eau, avec fallback
  // gracieux sur la 1ʳᵉ sonde présente si l'identification n'a pas été faite.
  float getTemperature() const;
  bool isInitialized() const { return sensorsInitialized; }
  bool isPhCalibrated() const { return _phCalibrated; }

  // Getters pour valeurs brutes (sans offset de calibration)
  float getRawOrp() const;
  float getRawPh() const;
  float getRawTemperature() const { return tempRawValue; }

  // ===== API 2 sondes DS18B20 (feature-020) =====
  // Retourne la T° calibrée de la sonde "eau" (offset utilisateur appliqué).
  // NaN si sonde "eau" non identifiée OU non détectée.
  float getWaterTemperature() const;
  // Retourne la T° brute de la sonde "circuit" (PAS d'offset, calibration usine seule).
  // NaN si sonde "circuit" non identifiée OU non détectée.
  float getCircuitTemperature() const;
  // true si les 2 rôles (eau ET circuit) sont attribués à des sondes détectées.
  bool areSondesIdentified() const;
  // Nombre de sondes DS18B20 physiquement présentes sur le bus OneWire (0..kMaxDs18b20Sondes).
  int getDetectedSondeCount() const { return _detectedCount; }
  // Copie les adresses détectées et indique pour chacune si elle est identifiée (matched=true).
  // addrs et matched doivent pouvoir contenir kMaxDs18b20Sondes entrées.
  void getDetectedSondeAddresses(uint8_t addrs[kMaxDs18b20Sondes][kSondeAddrLen],
                                 bool matched[kMaxDs18b20Sondes]) const;
  // Identifie la sonde portant `addr` comme "eau" (isWater=true) ou "circuit" (isWater=false).
  // Auto-permutation : si une autre sonde a déjà ce rôle, elle bascule à l'autre rôle.
  // Retourne false si l'adresse ne correspond à aucune sonde détectée.
  bool identifySonde(const uint8_t addr[kSondeAddrLen], bool isWater);
  // Efface les 2 adresses NVS et marque toutes les sondes comme non identifiées.
  void resetSondeIdentification();
  // Renvoie le rôle d'une sonde donnée pour la lecture sensors_data (utilitaire pour les routes web).
  SondeRole getSondeRole(uint8_t index) const {
    return (index < kMaxDs18b20Sondes) ? _sondes[index].role : SondeRole::Unknown;
  }
  float getSondeTempRaw(uint8_t index) const {
    return (index < kMaxDs18b20Sondes) ? _sondes[index].lastTempRaw : NAN;
  }

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
