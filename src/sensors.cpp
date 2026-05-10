#include "sensors.h"

#include <Preferences.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "constants.h"
#include "logger.h"
#include "pump_controller.h"  // armStabilizationTimer() après calibration EZO

SensorManager sensors;

namespace {

// =============================================================================
// Helpers DS18B20 — Conservés tels quels (feature-020)
// =============================================================================

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

// Helper local : formate une adresse ROM 1-Wire en hex majuscule sans séparateur.
// Doublon léger avec web_helpers::formatRomHex pour rester autoportant côté logs.
String romHex(const uint8_t addr[kSondeAddrLen]) {
  char buf[2 * kSondeAddrLen + 1];
  for (size_t i = 0; i < kSondeAddrLen; ++i) {
    snprintf(&buf[i * 2], 3, "%02X", addr[i]);
  }
  buf[2 * kSondeAddrLen] = '\0';
  return String(buf);
}

const char* sondeRoleLabel(SondeRole r) {
  switch (r) {
    case SondeRole::Water:   return "eau";
    case SondeRole::Circuit: return "circuit";
    case SondeRole::Unknown:
    default:                 return "non identifiée";
  }
}

// Température de référence sécuritaire si la sonde "eau" n'est pas disponible.
// Erreur de mesure pH < 0.1 dans la plage 15-30 °C piscine (cf. spec ligne 312).
constexpr float kEzoFallbackTempC = 25.0f;

}  // namespace

// =============================================================================
// Construction / destruction
// =============================================================================

SensorManager::SensorManager() : oneWire(kTempSensorPin), tempSensor(&oneWire) {
  for (size_t i = 0; i < kMaxDs18b20Sondes; ++i) {
    memset(_sondes[i].addr, 0, kSondeAddrLen);
    _sondes[i].lastTempRaw = NAN;
    _sondes[i].present = false;
    _sondes[i].role = SondeRole::Unknown;
  }
}

SensorManager::~SensorManager() {
  if (_ezoQueue != nullptr) {
    vQueueDelete(_ezoQueue);
    _ezoQueue = nullptr;
  }
}

// =============================================================================
// begin() — Initialisation matériel
// =============================================================================

void SensorManager::begin() {
  // Bus I²C partagé : DS3231 + EZO pH + EZO ORP (cf. constants.h kI2cSdaPin/kI2cSclPin)
  Wire.begin();

  // Création de la queue FreeRTOS pour les commandes longues (calibration).
  _ezoQueue = xQueueCreate(kEzoQueueLen, sizeof(EzoCmdRequest));
  if (_ezoQueue == nullptr) {
    systemLogger.error("Sensors : échec création queue EZO (mémoire insuffisante)");
  }

  // ----- DS18B20 (inchangé feature-020) -----
  tempSensor.begin();

  uint8_t deviceCount = tempSensor.getDeviceCount();
  if (deviceCount == 0) {
    systemLogger.warning("DS18B20 non détecté sur GPIO " + String(kTempSensorPin) +
                         " - vérifier câblage et résistance pull-up 4.7kΩ");
  } else {
    systemLogger.info("OneWire: " + String(deviceCount) + " sonde(s) DS18B20 détectée(s) sur GPIO " +
                      String(kTempSensorPin));
    if (deviceCount > kMaxDs18b20Sondes) {
      systemLogger.warning("Trop de sondes détectées (" + String(deviceCount) +
                           ") - seules les " + String(kMaxDs18b20Sondes) + " premières seront prises en compte");
    }
  }

  tempSensor.setWaitForConversion(false);
  g_ds18b20ResolutionBits = 12;
  tempSensor.setResolution(g_ds18b20ResolutionBits);
  g_ds18b20ConversionMs = ds18b20ConversionTimeMsForResolution(g_ds18b20ResolutionBits);

  _detectedCount = 0;
  uint8_t scanLimit = (deviceCount > kMaxDs18b20Sondes) ? (uint8_t)kMaxDs18b20Sondes : deviceCount;
  for (uint8_t i = 0; i < scanLimit; ++i) {
    uint8_t addr[kSondeAddrLen];
    if (tempSensor.getAddress(addr, i)) {
      memcpy(_sondes[_detectedCount].addr, addr, kSondeAddrLen);
      _sondes[_detectedCount].lastTempRaw = NAN;
      _sondes[_detectedCount].present = true;
      _sondes[_detectedCount].role = SondeRole::Unknown;
      _detectedCount++;
    } else {
      systemLogger.warning("DS18B20 index " + String(i) + " : impossible de lire l'adresse ROM");
    }
  }

  _loadSondeIdentificationFromNvs();

  for (uint8_t i = 0; i < _detectedCount; ++i) {
    systemLogger.info("  - sonde[" + String(i) + "] = " + romHex(_sondes[i].addr) +
                      " (" + String(sondeRoleLabel(_sondes[i].role)) + ")");
  }

  systemLogger.info("Capteur de température DS18B20 initialisé sur GPIO " + String(kTempSensorPin) +
                    " (" + String(g_ds18b20ResolutionBits) + "-bit, conv=" +
                    String(g_ds18b20ConversionMs) + "ms)");

  // ----- Atlas EZO pH / ORP -----
  // AC5 (résilience EZO débranché) : on ne bloque pas le boot si un EZO est muet.
  // Une simple lecture I (info) suffit à confirmer la présence du module.
  String fwInfo;
  if (_phEzo.readInfo(fwInfo)) {
    systemLogger.info("EZO pH détecté : " + fwInfo);
    _ezoEverResponded = true;
    // feature-024 : 1ʳᵉ query Slope,? différée via la queue EZO.
    // Sera traitée au prochain tick _processEzoQueue() — n'allonge pas le boot.
    enqueuePhSlopeQuery();
  } else {
    systemLogger.warning("EZO pH non détecté à l'adresse 0x" + String(kEzoPhAddress, HEX) +
                         " - lectures pH désactivées tant que la sonde n'est pas connectée");
  }

  if (_orpEzo.readInfo(fwInfo)) {
    systemLogger.info("EZO ORP détecté : " + fwInfo);
    _ezoEverResponded = true;
  } else {
    systemLogger.warning("EZO ORP non détecté à l'adresse 0x" + String(kEzoOrpAddress, HEX) +
                         " - lectures ORP désactivées tant que la sonde n'est pas connectée");
  }

  // Lecture initiale du nombre de points de calibration (cache).
  // pool-chemistry condition #2 : si Cal,? injoignable → -1 → régulation auto inhibée.
  _phCalCachedPoints = _phEzo.queryCalPoints();
  _orpCalCachedPoints = _orpEzo.queryCalPoints();

  if (_phCalCachedPoints <= 0) {
    systemLogger.critical("EZO pH non calibré (Cal,?=" + String(_phCalCachedPoints) +
                          ") — régulation pH automatique inhibée jusqu'à calibration");
  } else {
    systemLogger.info("EZO pH calibration : " + String(_phCalCachedPoints) + " point(s)");
  }
  if (_orpCalCachedPoints <= 0) {
    systemLogger.critical("EZO ORP non calibré (Cal,?=" + String(_orpCalCachedPoints) +
                          ") — régulation ORP automatique inhibée jusqu'à calibration");
  } else {
    systemLogger.info("EZO ORP calibration : " + String(_orpCalCachedPoints) + " point(s)");
  }

  systemLogger.info("Gestionnaire de capteurs initialisé (DS18B20 + Atlas EZO)");
}

// =============================================================================
// update() — Boucle de lecture périodique (appelée depuis loopTask)
// =============================================================================

void SensorManager::update() {
  // 1) Lecture DS18B20 (gère son propre timing de conversion)
  _readDs18b20s();

  // 2) Lecture pH/ORP via EZO (cadencée à kPhOrpSensorIntervalMs = 5 s)
  static unsigned long lastEzoRead = 0;
  unsigned long now = millis();
  if (now - lastEzoRead >= kPhOrpSensorIntervalMs) {
    lastEzoRead = now;
    // Compensation T° : sonde "eau" si identifiée, sinon fallback 25 °C (cf. spec).
    float tempC = getWaterTemperature();
    if (isnan(tempC)) tempC = kEzoFallbackTempC;
    _readEzoSensors(tempC);
  }

  // 3) Détection de stale (log critical une fois à la transition)
  _checkStaleAndLog();

  // 4) Traitement d'au plus 1 commande EZO de la queue (calibration ~1-2 s)
  _processEzoQueue();

  // 5) feature-024 : re-query Slope,? automatique toutes les 24h.
  // Conditions : 1ʳᵉ query déjà réussie (_phSlopeQueriedMs != 0), pas de query
  // en attente (_phSlopeQueryPending=false), et délai écoulé.
  //
  // BUG FIX : `now` doit être rafraîchi APRÈS _processEzoQueue() qui peut bloquer
  // ~900 ms sur une commande I²C. Sans ce rafraîchissement, si le handler met à
  // jour _phSlopeQueriedMs = millis() à T+900ms, on a now=T < _phSlopeQueriedMs
  // → underflow uint32_t sur (now - _phSlopeQueriedMs) → ~4.3 milliards → >= 24h
  // → ré-enqueue immédiate → boucle infinie de query Slope toutes les ~secondes.
  unsigned long nowAfterQueue = millis();
  if (_phSlopeQueriedMs != 0 && !_phSlopeQueryPending &&
      nowAfterQueue >= _phSlopeQueriedMs &&  // garde anti-underflow
      (nowAfterQueue - _phSlopeQueriedMs) >= kPhSlopeQueryIntervalMs) {
    enqueuePhSlopeQuery();
  }
}

// =============================================================================
// Lecture DS18B20 (multi-sondes feature-020) — Logique inchangée
// =============================================================================

void SensorManager::_readDs18b20s() {
  unsigned long now = millis();
  static unsigned long lastTempDebugLog = 0;
  static unsigned long lastTempRequest = 0;
  static bool tempRequested = false;
  static unsigned long lastTempRead = 0;

  const unsigned long TEMP_CONVERSION_MS = (unsigned long)g_ds18b20ConversionMs + 50;
  const unsigned long TEMP_REQUEST_INTERVAL_MS = 2000;

  // 1) Lancer une conversion si aucune n'est en cours et si l'intervalle est passé.
  //    On prend le mutex I²C uniquement le temps de l'envoi (la conversion se fait
  //    en arrière-plan côté DS18B20, pas besoin de tenir le bus).
  if (!tempRequested && (now - lastTempRequest >= TEMP_REQUEST_INTERVAL_MS)) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      tempSensor.requestTemperatures();
      xSemaphoreGive(i2cMutex);
      tempRequested = true;
      lastTempRequest = now;
    }
  }

  // 2) Lecture après le délai de conversion. Multi-sondes par adresse ROM.
  if (tempRequested && (now - lastTempRequest >= TEMP_CONVERSION_MS)) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      bool anyValidRead = false;
      for (uint8_t i = 0; i < _detectedCount; ++i) {
        float measuredTemp = tempSensor.getTempC(_sondes[i].addr);
        // 85.0 °C = power-on reset value DS18B20 (conversion incomplète).
        bool valid = (measuredTemp != DEVICE_DISCONNECTED_C &&
                      measuredTemp > -55.0f && measuredTemp < 125.0f &&
                      measuredTemp != 85.0f);
        if (valid) {
          _sondes[i].lastTempRaw = roundf(measuredTemp * 10.0f) / 10.0f;
          anyValidRead = true;
        } else {
          _sondes[i].lastTempRaw = NAN;
          if (authCfg.sensorLogsEnabled) {
            systemLogger.warning("DS18B20 " + romHex(_sondes[i].addr) +
                                 " : lecture invalide (déconnectée ou T° hors plage)");
          }
        }
      }
      xSemaphoreGive(i2cMutex);

      // Mise à jour des champs rétrocompat tempRawValue/tempValue (alias eau).
      int waterIdx = _findSondeIndexByRole(SondeRole::Water);
      if (waterIdx >= 0 && !isnan(_sondes[waterIdx].lastTempRaw)) {
        tempRawValue = _sondes[waterIdx].lastTempRaw;
        tempValue = tempRawValue + mqttCfg.tempCalibrationOffset;
      } else {
        // Fallback gracieux : 1ʳᵉ sonde présente sans offset (rôle inconnu).
        tempRawValue = NAN;
        tempValue = NAN;
        for (uint8_t i = 0; i < _detectedCount; ++i) {
          if (!isnan(_sondes[i].lastTempRaw)) {
            tempRawValue = _sondes[i].lastTempRaw;
            tempValue = tempRawValue;
            break;
          }
        }
      }

      if (!anyValidRead && _detectedCount > 0 && authCfg.sensorLogsEnabled) {
        systemLogger.warning("Aucune sonde DS18B20 n'a fourni de lecture valide ce cycle");
      }

      tempRequested = false;
      lastTempRead = now;
    }
  }

  // Debug température (toutes les 5 s, si activé)
  if (authCfg.sensorLogsEnabled && now - lastTempDebugLog >= 5000) {
    char logMsg[150];
    if (!isnan(tempValue)) {
      snprintf(logMsg, sizeof(logMsg),
               "Temp: %.2f°C | res=%dbit | age=%lums",
               tempValue, (int)g_ds18b20ResolutionBits, (unsigned long)(now - lastTempRead));
      systemLogger.debug(logMsg);
    } else {
      snprintf(logMsg, sizeof(logMsg),
               "Temp: NaN | res=%dbit | conversion=%s",
               (int)g_ds18b20ResolutionBits, tempRequested ? "EN COURS" : "IDLE");
      systemLogger.warning(logMsg);
    }
    lastTempDebugLog = now;
  }
}

// =============================================================================
// Lecture pH / ORP via Atlas EZO
// =============================================================================

void SensorManager::_readEzoSensors(float tempC) {
  unsigned long now = millis();

  // ----- pH -----
  float ph = NAN;
  if (_phEzo.readSingle(ph, tempC)) {
    _lastPh = ph;
    _lastPhMs = now;
    _phI2cFailStreak = 0;
    _phStaleLogged = false;
    _phI2cDegradedLogged = false;
    _ezoEverResponded = true;
    // Correctif Pass 3.5 (pool-chemistry) : si le cache cal_points a été invalidé
    // à -1 par une période de bus dégradé (ou n'a jamais été initialisé), on le
    // rafraîchit dès qu'un retour de bus est confirmé. Strictement borné à -1 →
    // un seul Cal,? est émis (et non chaque cycle, ce qui gèlerait loopTask 900 ms).
    if (_phCalCachedPoints == -1) {
      int pts = _phEzo.queryCalPoints();
      if (pts >= 0) {
        _phCalCachedPoints = pts;
        systemLogger.info("EZO pH : cache calibration rafraîchi (points=" +
                          String(pts) + ")");
      }
    }
    if (authCfg.sensorLogsEnabled) {
      char buf[80];
      snprintf(buf, sizeof(buf), "EZO pH: %.2f (T=%.1f°C)", ph, tempC);
      systemLogger.debug(buf);
    }
  } else {
    _phI2cFailStreak++;
    if (_phI2cFailStreak == kEzoBusFailMaxConsecutive) {
      // Logger une seule fois quand on franchit le seuil — au-delà, silence
      // pour ne pas inonder les logs en cas de débranchement durable.
      systemLogger.warning("EZO pH : " + String(_phI2cFailStreak) +
                           " échecs I²C consécutifs — dosage pH bloqué");
    }
    // Cond #5 pool-chemistry : invalider explicitement la lecture si bus dégradé.
    // Sans ça, getPh() pourrait retourner une valeur "fraîche" pendant la fenêtre
    // stale (20 s) alors que le bus I²C est en panne durable. canDose() doit
    // bloquer immédiatement → on fait passer _lastPh à NaN.
    if (_phI2cFailStreak >= kEzoBusFailMaxConsecutive) {
      _lastPh = NAN;
      // Correctif Pass 3.5 (pool-chemistry) : invalider aussi le cache cal_points.
      // Sinon, après une coupure I²C transitoire, canDose() pourrait reposer sur
      // un cache obsolète (ex. cal effacée pendant la coupure côté EZO). On
      // remet -1 → canDose() bloque jusqu'à un Cal,? réussi au retour de bus.
      _phCalCachedPoints = -1;
      // feature-024 : cohérence avec _phCalCachedPoints — invalider aussi le
      // cache pente. La pente n'a aucun sens tant que le bus EZO pH est dégradé.
      _phSlopeAcid = NAN;
      _phSlopeBase = NAN;
      _phSlopeZero = NAN;
      if (!_phI2cDegradedLogged) {
        systemLogger.critical("EZO pH : bus I²C dégradé (" + String(_phI2cFailStreak) +
                              " échecs) — lecture invalidée, régulation auto inhibée");
        _phI2cDegradedLogged = true;
      }
    }
  }

  // ----- ORP -----
  float orp = NAN;
  if (_orpEzo.readSingle(orp, tempC)) {
    _lastOrp = orp;
    _lastOrpMs = now;
    _orpI2cFailStreak = 0;
    _orpStaleLogged = false;
    _orpI2cDegradedLogged = false;
    _ezoEverResponded = true;
    // Correctif Pass 3.5 : rafraîchissement borné du cache cal_points ORP
    // (idem pH, cf. commentaire ci-dessus).
    if (_orpCalCachedPoints == -1) {
      int pts = _orpEzo.queryCalPoints();
      if (pts >= 0) {
        _orpCalCachedPoints = pts;
        systemLogger.info("EZO ORP : cache calibration rafraîchi (points=" +
                          String(pts) + ")");
      }
    }
    if (authCfg.sensorLogsEnabled) {
      char buf[80];
      snprintf(buf, sizeof(buf), "EZO ORP: %.0f mV", orp);
      systemLogger.debug(buf);
    }
  } else {
    _orpI2cFailStreak++;
    if (_orpI2cFailStreak == kEzoBusFailMaxConsecutive) {
      systemLogger.warning("EZO ORP : " + String(_orpI2cFailStreak) +
                           " échecs I²C consécutifs — dosage ORP bloqué");
    }
    // Cond #5 pool-chemistry : invalider la lecture ORP en cas de bus dégradé.
    if (_orpI2cFailStreak >= kEzoBusFailMaxConsecutive) {
      _lastOrp = NAN;
      // Correctif Pass 3.5 (pool-chemistry) : invalider aussi le cache cal_points
      // ORP. Cohérent avec pH ci-dessus → canDose() refuse jusqu'à un Cal,?
      // réussi au retour de bus.
      _orpCalCachedPoints = -1;
      if (!_orpI2cDegradedLogged) {
        systemLogger.critical("EZO ORP : bus I²C dégradé (" + String(_orpI2cFailStreak) +
                              " échecs) — lecture invalidée, régulation auto inhibée");
        _orpI2cDegradedLogged = true;
      }
    }
  }

  // Trace debug : on enregistre TOUJOURS (même si ph ou orp = NaN), pour visualiser
  // les trous de lecture aussi clairement que les valeurs.
  _recordPhDebugSample(_lastPh, _lastOrp, tempC);
}

// =============================================================================
// Trace debug pH (ring buffer en RAM)
// =============================================================================

void SensorManager::_recordPhDebugSample(float ph, float orp, float tempC) {
  PhDebugSample& s = _phDebugBuffer[_phDebugIdx];
  s.ms    = millis();
  s.ph    = ph;
  s.orp   = orp;
  s.tempC = tempC;
  _phDebugIdx = (_phDebugIdx + 1) % kPhDebugBufferSize;
  if (_phDebugCount < kPhDebugBufferSize) ++_phDebugCount;
}

size_t SensorManager::getPhDebugSampleCount() const {
  return _phDebugCount;
}

void SensorManager::getPhDebugSamplesJson(JsonArray out) const {
  // Parcours dans l'ordre chronologique (plus ancien d'abord).
  // Si buffer plein : on commence à l'index actuel (= position du plus ancien).
  // Sinon : on commence à 0.
  size_t start = (_phDebugCount == kPhDebugBufferSize) ? _phDebugIdx : 0;
  for (size_t i = 0; i < _phDebugCount; ++i) {
    size_t idx = (start + i) % kPhDebugBufferSize;
    const PhDebugSample& s = _phDebugBuffer[idx];
    JsonObject o = out.add<JsonObject>();
    o["t"] = s.ms;
    if (!isnan(s.ph))    o["ph"]    = roundf(s.ph * 1000.0f) / 1000.0f;
    else                 o["ph"]    = nullptr;
    if (!isnan(s.orp))   o["orp"]   = roundf(s.orp * 10.0f) / 10.0f;
    else                 o["orp"]   = nullptr;
    if (!isnan(s.tempC)) o["tempC"] = roundf(s.tempC * 10.0f) / 10.0f;
    else                 o["tempC"] = nullptr;
  }
}

void SensorManager::clearPhDebugBuffer() {
  _phDebugIdx = 0;
  _phDebugCount = 0;
}

// =============================================================================
// Détection stale + log critical à la transition (pool-chemistry condition #1)
// =============================================================================

void SensorManager::_checkStaleAndLog() {
  uint32_t now = millis();

  // pH : log critical UNE FOIS quand la dernière lecture valide dépasse le seuil
  if (!isnan(_lastPh) && !_phStaleLogged &&
      (now - _lastPhMs > kSensorStaleTimeoutMs)) {
    systemLogger.critical("EZO pH : lectures stale > " +
                          String(kSensorStaleTimeoutMs / 1000) +
                          "s — régulation auto inhibée");
    _phStaleLogged = true;
  }

  // ORP idem
  if (!isnan(_lastOrp) && !_orpStaleLogged &&
      (now - _lastOrpMs > kSensorStaleTimeoutMs)) {
    systemLogger.critical("EZO ORP : lectures stale > " +
                          String(kSensorStaleTimeoutMs / 1000) +
                          "s — régulation auto inhibée");
    _orpStaleLogged = true;
  }
}

// =============================================================================
// Queue de commandes EZO — exécution asynchrone (depuis update() dans loopTask)
// =============================================================================

void SensorManager::_processEzoQueue() {
  if (_ezoQueue == nullptr) return;

  EzoCmdRequest req;
  // Réception non bloquante : 0 tick. Au plus 1 commande par cycle pour ne pas
  // monopoliser loopTask (chaque commande peut prendre 900 ms côté I²C).
  if (xQueueReceive(_ezoQueue, &req, 0) != pdTRUE) {
    return;
  }

  _executeEzoCmd(req);

  // Watchdog : la commande de calibration peut bloquer ~1 s (delay 900 ms +
  // wire transactions). On reset après pour rester dans la fenêtre 30 s.
  esp_task_wdt_reset();
}

void SensorManager::_executeEzoCmd(const EzoCmdRequest& req) {
  bool ok = false;
  switch (req.kind) {
    case EzoCmdKind::CalibratePhMid: {
      systemLogger.info("EZO pH : calibration point milieu (pH 7.00) en cours...");
      ok = _phEzo.calibrate("mid,7.00");
      if (ok) {
        _phCalCachedPoints = _phEzo.queryCalPoints();
        PumpController.armStabilizationTimer(0);  // Stabilisation pH post-cal
        systemLogger.info("EZO pH : calibration mid OK (points=" + String(_phCalCachedPoints) + ")");
        // feature-024 : refresh pente post-calibration.
        enqueuePhSlopeQuery();
      } else {
        systemLogger.error("EZO pH : calibration mid échouée");
      }
      break;
    }
    case EzoCmdKind::CalibratePhLow: {
      systemLogger.info("EZO pH : calibration point bas (pH 4.00) en cours...");
      ok = _phEzo.calibrate("low,4.00");
      if (ok) {
        _phCalCachedPoints = _phEzo.queryCalPoints();
        PumpController.armStabilizationTimer(0);  // Stabilisation pH post-cal
        systemLogger.info("EZO pH : calibration low OK (points=" + String(_phCalCachedPoints) + ")");
        // feature-024 : refresh pente post-calibration.
        enqueuePhSlopeQuery();
      } else {
        systemLogger.error("EZO pH : calibration low échouée");
      }
      break;
    }
    case EzoCmdKind::CalibrateOrp: {
      // Cal,<ref> sur EZO ORP attend la valeur de référence en mV (entier).
      char arg[16];
      snprintf(arg, sizeof(arg), "%d", (int)roundf(req.arg));
      systemLogger.info("EZO ORP : calibration référence " + String(arg) + " mV en cours...");
      ok = _orpEzo.calibrate(arg);
      if (ok) {
        _orpCalCachedPoints = _orpEzo.queryCalPoints();
        PumpController.armStabilizationTimer(1);  // Stabilisation ORP post-cal
        systemLogger.info("EZO ORP : calibration OK (points=" + String(_orpCalCachedPoints) + ")");
      } else {
        systemLogger.error("EZO ORP : calibration échouée");
      }
      break;
    }
    case EzoCmdKind::ClearPhCal: {
      systemLogger.info("EZO pH : effacement calibration en cours...");
      ok = _phEzo.clearCalibration();
      if (ok) {
        _phCalCachedPoints = _phEzo.queryCalPoints();
        systemLogger.info("EZO pH : calibration effacée (points=" + String(_phCalCachedPoints) + ")");
        // feature-024 : la pente n'a plus de sens après un Cal,clear — refresh
        // pour récupérer les valeurs par défaut EZO (typiquement 100/100/0).
        enqueuePhSlopeQuery();
      } else {
        systemLogger.error("EZO pH : effacement calibration échoué");
      }
      break;
    }
    case EzoCmdKind::ClearOrpCal: {
      systemLogger.info("EZO ORP : effacement calibration en cours...");
      ok = _orpEzo.clearCalibration();
      if (ok) {
        _orpCalCachedPoints = _orpEzo.queryCalPoints();
        systemLogger.info("EZO ORP : calibration effacée (points=" + String(_orpCalCachedPoints) + ")");
      } else {
        systemLogger.error("EZO ORP : effacement calibration échoué");
      }
      break;
    }
    case EzoCmdKind::QueryPhSlope: {
      // feature-024 — re-query Slope,? sur l'EZO pH.
      // Le flag _phSlopeQueryPending est levé en début de handler pour permettre
      // une nouvelle demande de re-query sans attendre la fin du traitement.
      _phSlopeQueryPending = false;
      PhSlopeInfo info{NAN, NAN, NAN};
      if (_phEzo.querySlope(info)) {
        _phSlopeAcid = info.acidPct;
        _phSlopeBase = info.basePct;
        _phSlopeZero = info.zeroOffsetMv;
        _phSlopeQueriedMs = millis();
        _phSlopeFailStreak = 0;
        systemLogger.info("EZO pH slope : acide=" + String(info.acidPct, 1) +
                          "% base=" + String(info.basePct, 1) + "% zéro=" +
                          (isnan(info.zeroOffsetMv) ? String("N/A")
                                                    : String(info.zeroOffsetMv, 2) + "mV"));
      } else {
        _phSlopeFailStreak++;
        if (_phSlopeFailStreak >= kEzoBusFailMaxConsecutive) {
          // Cohérence avec _phCalCachedPoints : invalider à NaN après seuil.
          _phSlopeAcid = NAN;
          _phSlopeBase = NAN;
          _phSlopeZero = NAN;
          // _phSlopeQueriedMs reste à sa dernière valeur — l'âge sera détecté
          // comme stale par l'UI (Pass B), inutile de remettre à 0 ici.
          systemLogger.warning("EZO pH slope : " + String(_phSlopeFailStreak) +
                               " échecs Slope,? consécutifs — cache invalidé");
        }
      }
      break;
    }
  }
}

// =============================================================================
// Getters publics — pH / ORP (avec fenêtre stale)
// =============================================================================

float SensorManager::getPh() const {
  if (isnan(_lastPh)) return NAN;
  if (millis() - _lastPhMs > kSensorStaleTimeoutMs) return NAN;
  return _lastPh;
}

float SensorManager::getOrp() const {
  if (isnan(_lastOrp)) return NAN;
  if (millis() - _lastOrpMs > kSensorStaleTimeoutMs) return NAN;
  return _lastOrp;
}

int SensorManager::getPhCalibrationPoints() {
  // Rafraîchit à la demande pour les routes de diagnostic (peut prendre ~900 ms).
  // La régulation s'appuie sur le cache `_phCalCachedPoints` mis à jour en begin()
  // et après chaque calibration → pas de lecture I²C dans le chemin chaud.
  int pts = _phEzo.queryCalPoints();
  if (pts >= 0) {
    _phCalCachedPoints = pts;
  }
  return pts;
}

int SensorManager::getOrpCalibrationPoints() {
  int pts = _orpEzo.queryCalPoints();
  if (pts >= 0) {
    _orpCalCachedPoints = pts;
  }
  return pts;
}

bool SensorManager::isInitialized() const {
  // Considéré initialisé si au moins un EZO a déjà répondu (boot ou lecture)
  // ET qu'au moins une lecture pH ou ORP valide est en cache.
  return _ezoEverResponded && (!isnan(_lastPh) || !isnan(_lastOrp));
}

// =============================================================================
// Enqueue de commandes — appelé par handlers async (Pass 4) ou UART (uart_commands)
// =============================================================================

bool SensorManager::enqueueCalibratePhMid() {
  if (_ezoQueue == nullptr) return false;
  EzoCmdRequest req{EzoCmdKind::CalibratePhMid, 0.0f};
  return xQueueSend(_ezoQueue, &req, 0) == pdTRUE;
}

bool SensorManager::enqueueCalibratePhLow() {
  if (_ezoQueue == nullptr) return false;
  EzoCmdRequest req{EzoCmdKind::CalibratePhLow, 0.0f};
  return xQueueSend(_ezoQueue, &req, 0) == pdTRUE;
}

bool SensorManager::enqueueCalibrateOrp(float referenceMv) {
  if (_ezoQueue == nullptr) return false;
  EzoCmdRequest req{EzoCmdKind::CalibrateOrp, referenceMv};
  return xQueueSend(_ezoQueue, &req, 0) == pdTRUE;
}

bool SensorManager::enqueueClearPhCalibration() {
  if (_ezoQueue == nullptr) return false;
  EzoCmdRequest req{EzoCmdKind::ClearPhCal, 0.0f};
  return xQueueSend(_ezoQueue, &req, 0) == pdTRUE;
}

bool SensorManager::enqueueClearOrpCalibration() {
  if (_ezoQueue == nullptr) return false;
  EzoCmdRequest req{EzoCmdKind::ClearOrpCal, 0.0f};
  return xQueueSend(_ezoQueue, &req, 0) == pdTRUE;
}

// =============================================================================
// feature-024 — pente sonde pH : enqueue + getters
// =============================================================================

bool SensorManager::enqueuePhSlopeQuery() {
  if (_ezoQueue == nullptr) return false;
  // Anti-doublon : si une query est déjà en file, ne pas en empiler une seconde.
  // Le flag est levé par le handler QueryPhSlope dès le début de son traitement.
  if (_phSlopeQueryPending) return true;  // succès "noop" — caller satisfait
  EzoCmdRequest req{EzoCmdKind::QueryPhSlope, 0.0f};
  if (xQueueSend(_ezoQueue, &req, 0) != pdTRUE) {
    return false;  // queue pleine — pas de flag posé
  }
  _phSlopeQueryPending = true;
  return true;
}

float SensorManager::getPhSlopeAcid() const { return _phSlopeAcid; }
float SensorManager::getPhSlopeBase() const { return _phSlopeBase; }
float SensorManager::getPhSlopeZero() const { return _phSlopeZero; }

uint32_t SensorManager::getPhSlopeAgeMs() const {
  if (_phSlopeQueriedMs == 0) return UINT32_MAX;
  uint32_t now = millis();
  // Sécurise un éventuel overflow (millis() roule tous les ~49.7 jours).
  return (now >= _phSlopeQueriedMs) ? (now - _phSlopeQueriedMs) : 0;
}

// =============================================================================
// API DS18B20 (feature-020) — Inchangé
// =============================================================================

int SensorManager::_findSondeIndexByRole(SondeRole role) const {
  for (uint8_t i = 0; i < _detectedCount; ++i) {
    if (_sondes[i].role == role) return (int)i;
  }
  return -1;
}

int SensorManager::_findSondeIndexByAddr(const uint8_t addr[kSondeAddrLen]) const {
  for (uint8_t i = 0; i < _detectedCount; ++i) {
    if (memcmp(_sondes[i].addr, addr, kSondeAddrLen) == 0) return (int)i;
  }
  return -1;
}

void SensorManager::_loadSondeIdentificationFromNvs() {
  Preferences prefs;
  if (!prefs.begin("poolctrl", true /*readonly*/)) {
    systemLogger.warning("OneWire ID: NVS poolctrl indisponible (lecture)");
    return;
  }

  uint8_t waterAddr[kSondeAddrLen];
  uint8_t circuitAddr[kSondeAddrLen];
  size_t waterLen = prefs.getBytes(kNvsKeyOwWaterAddr, waterAddr, kSondeAddrLen);
  size_t circuitLen = prefs.getBytes(kNvsKeyOwCircuitAddr, circuitAddr, kSondeAddrLen);
  prefs.end();

  if (waterLen == kSondeAddrLen) {
    int idx = _findSondeIndexByAddr(waterAddr);
    if (idx >= 0) {
      _sondes[idx].role = SondeRole::Water;
    } else {
      systemLogger.warning("OneWire ID: sonde 'eau' (" + romHex(waterAddr) +
                           ") non détectée - identification à refaire");
    }
  }

  if (circuitLen == kSondeAddrLen) {
    int idx = _findSondeIndexByAddr(circuitAddr);
    if (idx >= 0) {
      _sondes[idx].role = SondeRole::Circuit;
    } else {
      systemLogger.warning("OneWire ID: sonde 'circuit' (" + romHex(circuitAddr) +
                           ") non détectée - identification à refaire");
    }
  }
}

bool SensorManager::_saveSondeAddrToNvs(const char* nvsKey, const uint8_t addr[kSondeAddrLen]) {
  Preferences prefs;
  if (!prefs.begin("poolctrl", false /*RW*/)) {
    systemLogger.error("OneWire ID: échec ouverture NVS poolctrl en écriture");
    return false;
  }
  size_t written = prefs.putBytes(nvsKey, addr, kSondeAddrLen);
  prefs.end();
  if (written != kSondeAddrLen) {
    systemLogger.error("OneWire ID: écriture NVS " + String(nvsKey) + " incomplète (" +
                       String(written) + "/" + String(kSondeAddrLen) + ")");
    return false;
  }
  return true;
}

float SensorManager::getTemperature() const {
  float waterT = getWaterTemperature();
  if (!isnan(waterT)) return waterT;
  for (uint8_t i = 0; i < _detectedCount; ++i) {
    if (!isnan(_sondes[i].lastTempRaw)) {
      return _sondes[i].lastTempRaw;
    }
  }
  return NAN;
}

float SensorManager::getWaterTemperature() const {
  int idx = _findSondeIndexByRole(SondeRole::Water);
  if (idx < 0) return NAN;
  float raw = _sondes[idx].lastTempRaw;
  if (isnan(raw)) return NAN;
  return raw + mqttCfg.tempCalibrationOffset;
}

float SensorManager::getWaterTemperatureRaw() const {
  int idx = _findSondeIndexByRole(SondeRole::Water);
  if (idx < 0) return NAN;
  return _sondes[idx].lastTempRaw;
}

float SensorManager::getCircuitTemperature() const {
  int idx = _findSondeIndexByRole(SondeRole::Circuit);
  if (idx < 0) return NAN;
  return _sondes[idx].lastTempRaw;
}

bool SensorManager::areSondesIdentified() const {
  return _findSondeIndexByRole(SondeRole::Water) >= 0 &&
         _findSondeIndexByRole(SondeRole::Circuit) >= 0;
}

void SensorManager::getDetectedSondeAddresses(uint8_t addrs[kMaxDs18b20Sondes][kSondeAddrLen],
                                              bool matched[kMaxDs18b20Sondes]) const {
  for (size_t i = 0; i < kMaxDs18b20Sondes; ++i) {
    if (i < _detectedCount) {
      memcpy(addrs[i], _sondes[i].addr, kSondeAddrLen);
      matched[i] = (_sondes[i].role != SondeRole::Unknown);
    } else {
      memset(addrs[i], 0, kSondeAddrLen);
      matched[i] = false;
    }
  }
}

bool SensorManager::identifySonde(const uint8_t addr[kSondeAddrLen], bool isWater) {
  int targetIdx = _findSondeIndexByAddr(addr);
  if (targetIdx < 0) {
    systemLogger.warning("OneWire ID: adresse " + romHex(addr) + " non détectée - identification refusée");
    return false;
  }

  SondeRole newRole = isWater ? SondeRole::Water : SondeRole::Circuit;
  SondeRole otherRole = isWater ? SondeRole::Circuit : SondeRole::Water;
  const char* newKey = isWater ? kNvsKeyOwWaterAddr : kNvsKeyOwCircuitAddr;
  const char* otherKey = isWater ? kNvsKeyOwCircuitAddr : kNvsKeyOwWaterAddr;

  for (uint8_t i = 0; i < _detectedCount; ++i) {
    if ((int)i == targetIdx) continue;
    if (_sondes[i].role == newRole) {
      _sondes[i].role = otherRole;
      _saveSondeAddrToNvs(otherKey, _sondes[i].addr);
      systemLogger.info("Sonde " + romHex(_sondes[i].addr) + " permutée " +
                        String(sondeRoleLabel(newRole)) + " -> " + String(sondeRoleLabel(otherRole)) +
                        " (suite à identification de " + romHex(addr) + " comme " +
                        String(sondeRoleLabel(newRole)) + ")");
    }
  }

  _sondes[targetIdx].role = newRole;
  if (!_saveSondeAddrToNvs(newKey, addr)) {
    return false;
  }
  systemLogger.info("Sonde " + romHex(addr) + " identifiée comme " + String(sondeRoleLabel(newRole)));
  return true;
}

void SensorManager::resetSondeIdentification() {
  Preferences prefs;
  if (prefs.begin("poolctrl", false /*RW*/)) {
    prefs.remove(kNvsKeyOwWaterAddr);
    prefs.remove(kNvsKeyOwCircuitAddr);
    prefs.end();
  } else {
    systemLogger.error("OneWire ID: échec ouverture NVS pour reset identification");
  }

  for (uint8_t i = 0; i < _detectedCount; ++i) {
    _sondes[i].role = SondeRole::Unknown;
  }
  systemLogger.info("Identification des sondes DS18B20 réinitialisée (NVS effacé)");
}
