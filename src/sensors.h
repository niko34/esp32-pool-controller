#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "atlas_ezo.h"
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

// =============================================================================
// SensorManager — Pilotage capteurs (DS18B20 + Atlas EZO pH/ORP)
// =============================================================================
//
// Architecture (feature-021) :
//   - DS18B20 : multi-sondes (eau + circuit), inchangé feature-020
//   - pH / ORP : modules Atlas EZO Embedded I²C (kEzoPhAddress / kEzoOrpAddress)
//     Calibration stockée DANS le module EZO (NVS interne), pas en NVS ESP32.
//     Compensation T° envoyée via "RT,<temp>" avant chaque "R" (cf. AtlasEzoSensor).
//
// Concurrence :
//   - update() est appelé depuis loopTask (core 1).
//   - getPh() / getOrp() peuvent être appelés depuis loopTask (pump_controller)
//     ou des handlers async (core 0). Les caches `_lastPh` / `_lastOrp` sont
//     des `float` 32 bits alignés : lecture/écriture atomique sur ESP32 (Xtensa
//     LX6) — pas de mutex dédié nécessaire pour ces variables scalaires.
//   - Les commandes longues (calibration, ~1-2 s par appel I²C) sont
//     sérialisées via une queue FreeRTOS (`_ezoQueue`) traitée dans update().
//     Les handlers async appellent `enqueue*()` (< 1 ms) et l'UI observe la
//     transition via WS.
// =============================================================================

class SensorManager {
public:
  // Type de commande EZO différée (file `_ezoQueue`)
  enum class EzoCmdKind {
    CalibratePhMid,    // Cal,mid,7.00
    CalibratePhLow,    // Cal,low,4.00
    CalibrateOrp,      // Cal,<referenceMv>
    ClearPhCal,        // Cal,clear sur pH
    ClearOrpCal        // Cal,clear sur ORP
  };

  // Requête de commande EZO posée par les handlers async, traitée par update()
  struct EzoCmdRequest {
    EzoCmdKind kind;
    float arg;  // Pour CalibrateOrp : valeur de référence en mV. Sinon ignoré.
  };

  SensorManager();
  ~SensorManager();

  void begin();
  void update(); // Appelé dans loop() — non bloquant côté HTTP

  // ===== API capteurs Atlas EZO (pH / ORP) =====
  // NaN si lecture stale (dernière lecture valide > kSensorStaleTimeoutMs).
  // Atomique (float aligné, ESP32 Xtensa).
  float getPh() const;
  // NaN si lecture stale (dernière lecture valide > kSensorStaleTimeoutMs).
  float getOrp() const;
  // Nombre de points de calibration mémorisés dans l'EZO.
  // -1 si module injoignable (erreur I²C ou bus indisponible),
  // 0..3 sinon (Atlas pH supporte 1pt/2pts/3pts, ORP 1pt seul).
  // ⚠️ Lecture I²C bloquante (~900 ms) — réservé aux routes HTTP de diagnostic
  // et à mqttTask. Pour les chemins chauds (loopTask: ws_manager, pump_controller),
  // utiliser `getPhCalibrationPointsCached()` qui retourne `_phCalCachedPoints`
  // sans accès au bus.
  int getPhCalibrationPoints();
  int getOrpCalibrationPoints();

  // Variantes "cache only" — pas d'accès I²C, retourne la dernière valeur connue
  // (mise à jour en begin() puis à chaque calibration via _processEzoQueue).
  // Utilisable depuis n'importe quel contexte temps réel.
  int getPhCalibrationPointsCached() const { return _phCalCachedPoints; }
  int getOrpCalibrationPointsCached() const { return _orpCalCachedPoints; }

  // ===== Enqueue de commandes longues (handlers async safe, < 1 ms) =====
  // Renvoient true si la commande a été placée dans la queue, false sinon
  // (queue pleine ou non initialisée).
  bool enqueueCalibratePhMid();         // Calibration point milieu (pH 7.00)
  bool enqueueCalibratePhLow();         // Calibration point bas (pH 4.00)
  bool enqueueCalibrateOrp(float referenceMv);  // Calibration ORP (référence en mV)
  bool enqueueClearPhCalibration();
  bool enqueueClearOrpCalibration();

  // ===== Diagnostic / état =====
  // True si au moins un EZO a répondu au boot ET qu'au moins une lecture pH
  // ou ORP valide est disponible (cohérent avec gestion fallback EZO débranché).
  bool isInitialized() const;

  // ===== Trace debug pH (ring buffer en RAM, hors-spec, retirable) =====
  // Capture chaque cycle EZO (pH + ORP + tempC envoyé). Sert au diagnostic d'oscillation.
  // Buffer glissant ~25 min à kPhOrpSensorIntervalMs = 5 s.
  struct PhDebugSample {
    uint32_t ms;     // millis() au moment de la lecture
    float    ph;     // NaN si lecture échouée
    float    orp;    // NaN si lecture échouée
    float    tempC;  // T° envoyée à l'EZO pour compensation
  };
  static constexpr size_t kPhDebugBufferSize = 300;
  // Sérialise les échantillons valides dans l'ordre chronologique (plus ancien d'abord).
  size_t getPhDebugSampleCount() const;
  // Remplit `out` avec les samples sous forme JSON `[{t, ph, orp, tempC}, ...]`.
  // `out` doit déjà être un JsonArray dans un JsonDocument suffisamment grand.
  void getPhDebugSamplesJson(JsonArray out) const;
  // Vide le ring buffer (utile pour démarrer une fenêtre d'observation propre).
  void clearPhDebugBuffer();

  // ===== API DS18B20 — Température (feature-020, inchangé) =====
  // Alias rétrocompat de la T° eau, avec fallback gracieux sur la 1ʳᵉ sonde
  // présente tant que l'identification utilisateur n'a pas été faite.
  float getTemperature() const;
  // Retourne la T° calibrée de la sonde "eau" (offset utilisateur appliqué).
  // NaN si sonde "eau" non identifiée OU non détectée.
  float getWaterTemperature() const;
  // Retourne la T° BRUTE de la sonde "eau" (sans `mqttCfg.tempCalibrationOffset`).
  // Utile pour l'UI de calibration T° qui doit calculer un nouvel offset à partir
  // d'une référence externe — évite la formule fragile `calibrated - offset_précédent`.
  // NaN si sonde "eau" non identifiée OU non détectée.
  float getWaterTemperatureRaw() const;
  // Retourne la T° brute de la sonde "circuit" (PAS d'offset, calibration usine).
  float getCircuitTemperature() const;
  bool areSondesIdentified() const;
  int getDetectedSondeCount() const { return _detectedCount; }
  void getDetectedSondeAddresses(uint8_t addrs[kMaxDs18b20Sondes][kSondeAddrLen],
                                 bool matched[kMaxDs18b20Sondes]) const;
  bool identifySonde(const uint8_t addr[kSondeAddrLen], bool isWater);
  void resetSondeIdentification();
  SondeRole getSondeRole(uint8_t index) const {
    return (index < kMaxDs18b20Sondes) ? _sondes[index].role : SondeRole::Unknown;
  }
  float getSondeTempRaw(uint8_t index) const {
    return (index < kMaxDs18b20Sondes) ? _sondes[index].lastTempRaw : NAN;
  }

private:
  // ===== Capteurs DS18B20 =====
  OneWire oneWire;
  DallasTemperature tempSensor;

  SondeInfo _sondes[kMaxDs18b20Sondes];
  uint8_t _detectedCount = 0;

  // Cache rétrocompat eau (alimenté par readDs18b20s())
  float tempValue = NAN;
  float tempRawValue = NAN;

  // ===== Capteurs Atlas EZO =====
  AtlasEzoSensor _phEzo{kEzoPhAddress, "EZO pH"};
  AtlasEzoSensor _orpEzo{kEzoOrpAddress, "EZO ORP"};

  // Cache des dernières lectures valides — accédés sans mutex.
  // Atomique CHAMP PAR CHAMP (float 32 bits aligné, instructions L32I/S32I single-cycle
  // sur Xtensa LX6) mais PAS atomique sur la paire (_lastPh, _lastPhMs) : un getter peut
  // lire la valeur récente avec l'horodatage précédent (ou inverse) pendant 1 cycle si
  // _readEzoSensors() écrit entre les 2 lectures. Impact maximal : 1 cycle (~5 s) de
  // fausse alerte stale ou inverse — fail-safe acceptable pour la régulation chimique.
  float _lastPh = NAN;
  float _lastOrp = NAN;
  uint32_t _lastPhMs = 0;
  uint32_t _lastOrpMs = 0;

  // Compteurs d'échecs I²C consécutifs (pool-chemistry condition #5)
  int _phI2cFailStreak = 0;
  int _orpI2cFailStreak = 0;

  // Flags pour ne logger critical qu'à la transition vers stale
  bool _phStaleLogged = false;
  bool _orpStaleLogged = false;

  // Flags pour ne logger critical qu'une seule fois quand le bus I²C franchit
  // le seuil `kEzoBusFailMaxConsecutive` (pool-chemistry condition #5).
  // Remis à false dès la 1ʳᵉ lecture réussie suivante.
  bool _phI2cDegradedLogged = false;
  bool _orpI2cDegradedLogged = false;

  // Cache des points de calibration EZO (mis à jour en begin() puis après chaque
  // calibration). Permet à getPhCalibrationPointsCached() / getOrpCalibrationPointsCached()
  // d'être appelés sans bloquer le bus I²C dans le chemin chaud (loopTask, WS).
  // Valeur -1 : EZO injoignable ou bus I²C dégradé (fail-streak ≥ kEzoBusFailMaxConsecutive).
  // canDose(int) bloque le dosage tant que la valeur reste -1 (pool-chemistry condition #2/#5).
  // Le cache est rafraîchi opportunément à la 1ʳᵉ lecture réussie suivante.
  int _phCalCachedPoints = -1;   // -1 = bus down/inconnu, 0..3 sinon
  int _orpCalCachedPoints = -1;  // -1 = bus down/inconnu, 0..1 sinon

  // True si au moins un EZO a répondu (au moins une fois) — utilisé par isInitialized()
  bool _ezoEverResponded = false;

  // ===== Queue FreeRTOS pour commandes longues =====
  static constexpr UBaseType_t kEzoQueueLen = 4;
  QueueHandle_t _ezoQueue = nullptr;

  // ===== Trace debug pH (ring buffer en RAM) =====
  PhDebugSample _phDebugBuffer[kPhDebugBufferSize] = {};
  size_t _phDebugIdx = 0;     // prochain slot d'écriture (modulo kPhDebugBufferSize)
  size_t _phDebugCount = 0;   // nombre d'entrées valides (jusqu'à kPhDebugBufferSize)

  // ===== Helpers privés =====
  void _readEzoSensors(float tempC);   // Lecture pH puis ORP, mise à jour caches
  void _recordPhDebugSample(float ph, float orp, float tempC);
  void _readDs18b20s();                // Lecture multi-sondes DS18B20
  void _processEzoQueue();             // Dépile au plus 1 commande par cycle
  void _executeEzoCmd(const EzoCmdRequest& req);
  void _checkStaleAndLog();            // Détection stale → log critical (1 fois)

  // Helpers DS18B20 (feature-020) — inchangés
  void _loadSondeIdentificationFromNvs();
  bool _saveSondeAddrToNvs(const char* nvsKey, const uint8_t addr[kSondeAddrLen]);
  int _findSondeIndexByRole(SondeRole role) const;
  int _findSondeIndexByAddr(const uint8_t addr[kSondeAddrLen]) const;
};

extern SensorManager sensors;

#endif // SENSORS_H
