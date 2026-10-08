#include "sensors.h"

#include <Preferences.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "constants.h"
#include "logger.h"
#include "measure_validity.h"  // v2.20.3 : porte d'alimentation des filtres pH/ORP
#include "mqtt_manager.h"      // v2.20.3 : publishAlert() sur refus de ré-amorçage
#include "ph_offset.h"        // feature-067 : applyPhOffset() / isPhOffsetValid()
#include "pump_controller.h"  // armStabilizationTimer() après calibration EZO
#include "slope_retry_logic.h"  // v2.21.2 : shouldRetryBootSlope() (logique pure)

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
  // feature-067 : charger l'offset pH persisté AVANT toute lecture (begin() est
  // appelé après loadMqttConfig() dans setup()).
  refreshPhOffsetCache();

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
    // v2.21.2 : gate C2 des retentatives Slope,? de boot — jamais de retry
    // sans module détecté (chaque tentative bloque loopTask ~900 ms).
    _phEzoDetected = true;
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
  // 0) v2.20.3 : consommation des demandes de reset différées (posées depuis un
  //    handler async / MQTT via requestFilterReset()). Le reset s'exécute ici,
  //    dans le SEUL contexte loopTask autorisé à écrire dans un SensorFilter.
  if (_phFilterResetRequested) {
    _phFilterResetRequested = false;
    _executeRequestedReset(true, _phResetOrigin);
  }
  if (_orpFilterResetRequested) {
    _orpFilterResetRequested = false;
    _executeRequestedReset(false, _orpResetOrigin);
  }

  // 1) Lecture DS18B20 (gère son propre timing de conversion)
  _readDs18b20s();

  // 2) Lecture pH/ORP via EZO (cadencée à kPhOrpSensorIntervalMs = 5 s)
  static unsigned long lastEzoRead = 0;
  unsigned long now = millis();
  if (now - lastEzoRead >= kPhOrpSensorIntervalMs) {
    lastEzoRead = now;
    // Compensation T° (bug-114) : cascade BORNÉE, jamais de suspension.
    // Voir _resolveEzoCompensationTemp() — et surtout l'avertissement de
    // temp_compensation_logic.h : ne pas envoyer `RT` laisserait l'EZO garder
    // sa dernière température interne, invisible et indatable.
    float tempC = _resolveEzoCompensationTemp((uint32_t)now);
    _readEzoSensors(tempC);
  }

  // 3) Détection de stale (log critical une fois à la transition)
  _checkStaleAndLog();

  // 3bis) feature-022 : détection capteur figé (logs edge-triggered)
  _checkFrozenAndLog();

  // 3ter) v2.20.3 (C1) : alarme « filtre non prêt durablement » — l'absence
  // TOTALE de régulation ne doit jamais être silencieuse (incident 2026-08-16 :
  // ≥ 3 h sans dosage pH NI chlore, détecté par hasard par l'utilisateur).
  _checkFilterNotReadyAlarm((uint32_t)millis());

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

  // 5bis) v2.21.2 (bug-slope-boot-perdue) : retentative BORNÉE de la Slope,?
  // de boot tant qu'AUCUNE pente n'a jamais été lue (_phSlopeQueriedMs == 0 —
  // le cycle 24 h ci-dessus est inopérant dans cet état). Décision pure dans
  // slope_retry_logic.h : gates C1 (borne dure) et C2 (module détecté) inclus.
  if (shouldRetryBootSlope(_phSlopeQueriedMs, _phSlopeQueryPending, _phEzoDetected,
                           _phSlopeBootAttempts, kPhSlopeBootRetryMaxAttempts,
                           _phSlopeBootLastAttemptMs, (uint32_t)nowAfterQueue,
                           kPhSlopeBootRetryIntervalMs)) {
    // Gate C1 (pool-chemistry) : décompter la tentative AVANT l'enqueue, MÊME
    // si la file est pleine — borne dure de kPhSlopeBootRetryMaxAttempts
    // transactions I²C (~900 ms de loopTask chacune), compteur JAMAIS réarmé.
    _phSlopeBootAttempts++;
    _phSlopeBootLastAttemptMs = (uint32_t)nowAfterQueue;
    bool queued = enqueuePhSlopeQuery();
    systemLogger.info("EZO pH : Slope,? de boot retentée (" +
                      String(_phSlopeBootAttempts) + "/" +
                      String(kPhSlopeBootRetryMaxAttempts) +
                      (queued ? ")" : ") — file EZO pleine, tentative décomptée"));
  } else if (_phSlopeQueriedMs == 0 && !_phSlopeQueryPending && _phEzoDetected &&
             _phSlopeBootAttempts >= kPhSlopeBootRetryMaxAttempts &&
             !_phSlopeBootExhaustedLogged) {
    // Warning UNIQUE à l'épuisement : les kPhSlopeBootRetryMaxAttempts
    // retentatives ont toutes été consommées sans qu'une pente soit jamais lue.
    _phSlopeBootExhaustedLogged = true;
    systemLogger.warning("EZO pH : pente toujours jamais lue après " +
                         String(kPhSlopeBootRetryMaxAttempts) +
                         " retentatives de boot — abandon (cycle 24 h inopérant, "
                         "utiliser POST /debug/ph_slope_refresh)");
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
  // bug-142 : cadence alignée sur kTempFrozenSamplePeriodMs (constants.h) — le
  // static_assert qui protège la fenêtre du détecteur figé T° suppose cette
  // valeur, ne pas les laisser diverger.
  const unsigned long TEMP_REQUEST_INTERVAL_MS = kTempFrozenSamplePeriodMs;

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
          // feature-022 : alimente le détecteur figé T° eau avec la valeur BRUTE
          // (non arrondie au 0.1 °C). bug-142 : le bruit de quantification ne
          // casse le run QUE si la valeur vraie longe une frontière de pas
          // (0.0625 °C) — loin d'une frontière, cas normal d'une masse d'eau de
          // plusieurs dizaines de m³, le DS18B20 rend une constante parfaite
          // pendant des heures sans que ce soit une panne. C'est pour ça que la
          // fenêtre (kTempFrozenSamples), et non l'epsilon, est le vrai levier.
          // Seules les lectures VALIDES alimentent le détecteur (frozen persiste
          // pendant une rafale de lectures invalides — cohérent condition #4).
          if (_sondes[i].role == SondeRole::Water) {
            _waterTempFrozen.addSample(measuredTemp);
          }
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
        // bug-114 : offset BORNÉ — clamp **3/4** (QUATRE sites au total, liste
        // dans config.cpp / clamp 2/4). Celui-ci borne le cache `tempValue`
        // consommé par l'affichage, MQTT et l'historique. Il n'alimente PAS
        // `RT` : ce rôle appartient au clamp **4/4**, plus bas dans ce même
        // fichier (getWaterTemperature()). Ne pas fusionner les deux.
        tempValue = tempRawValue + clampTempCalOffset(mqttCfg.tempCalibrationOffset);
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
// bug-114 — Température de compensation envoyée à l'EZO pH : cascade bornée
// =============================================================================
//
// ⛔ NE JAMAIS « SUSPENDRE LA COMPENSATION » pour traiter une température
//    douteuse. C'est la simplification qui vient naturellement à l'esprit, et
//    elle est fausse : ne pas envoyer `RT` fait retomber la lecture sur `R`, et
//    le module EZO applique alors SA dernière température interne — une valeur
//    silencieusement périmée, que le firmware ne peut ni voir ni dater. Le repli
//    ci-dessous coûte au maximum ± 0,01 pH (verdict pool-chemistry du
//    2026-09-07) et reste, lui, entièrement visible.
//
// Cascade :
//   1. mesure courante si plausible ([0 ; 40] °C, cf. kEzoCompTemp*) ;
//   2. sinon dernière valeur plausible, si son âge est sous la fenêtre stale
//      existante (kSensorStaleTimeoutMs = 20 s, soit 10 lectures DS18B20) ;
//   3. sinon kEzoFallbackTempC = 25 °C.
//
// Journalisation : `warning` à chaque substitution — immédiatement à la
// transition d'étage, puis rappelé toutes les kEzoCompSubstitutionLogIntervalMs
// tant qu'elle dure (sans throttle, la réévaluation toutes les 5 s produirait
// 12 messages/minute). `critical` one-shot si le repli au défaut dépasse
// kEzoCompFallbackCriticalMs — un repli qui DURE n'est plus un incident de
// lecture, c'est une panne de sonde.
float SensorManager::_resolveEzoCompensationTemp(uint32_t nowMs) {
  const float measured = getWaterTemperature();  // brut + offset BORNÉ

  // Âge de l'étage 2. `_lastPlausibleCompMs == 0` = jamais alimenté → âge
  // maximal, l'étage sera refusé par la comparaison à la fenêtre stale.
  const uint32_t age = (_lastPlausibleCompMs == 0)
                           ? 0xFFFFFFFFu
                           : (uint32_t)(nowMs - _lastPlausibleCompMs);

  const TempCompDecision d = resolveCompensationTemp(
      measured, _lastPlausibleCompTempC, age, kSensorStaleTimeoutMs);

  // Mémoriser la valeur courante comme référence de l'étage 2 pour les cycles
  // suivants (uniquement si elle vient bien de la mesure).
  if (d.stage == TempCompStage::Current) {
    _lastPlausibleCompTempC = d.tempC;
    _lastPlausibleCompMs = (nowMs == 0) ? 1 : nowMs;  // 0 réservé à « jamais »
  }

  // ----- Journalisation des substitutions -----
  const bool substituted = (d.stage != TempCompStage::Current);
  const bool stageChanged = (d.stage != _compStageLogged);
  const bool dueForReminder =
      substituted && (nowMs - _compSubstitutionLastLogMs >= kEzoCompSubstitutionLogIntervalMs);

  if (substituted && (stageChanged || dueForReminder)) {
    systemLogger.warning(String("Compensation T° pH substituée (") +
                         tempCompStageLabel(d.stage) + ") : mesure " +
                         (isnan(measured) ? String("NaN") : String(measured, 1)) +
                         " °C hors [" + String(kEzoCompTempMinC, 0) + " ; " +
                         String(kEzoCompTempMaxC, 0) + "] °C → RT," +
                         String(d.tempC, 1));
    _compSubstitutionLastLogMs = nowMs;
  } else if (!substituted && stageChanged) {
    systemLogger.info("Compensation T° pH : retour sur la mesure courante (RT," +
                      String(d.tempC, 1) + ")");
  }
  _compStageLogged = d.stage;

  // ----- Alarme « repli au défaut DURABLE » -----
  if (d.stage == TempCompStage::Fallback) {
    if (_compFallbackSinceMs == 0) _compFallbackSinceMs = (nowMs == 0) ? 1 : nowMs;
    if (!_compFallbackCriticalLogged &&
        (nowMs - _compFallbackSinceMs) >= kEzoCompFallbackCriticalMs) {
      systemLogger.critical(String("Compensation T° pH figée sur le repli ") +
                            String(kEzoFallbackTempC, 1) + " °C depuis plus de " +
                            String(kEzoCompFallbackCriticalMs / 60000UL) +
                            " min — sonde eau indisponible ou hors plage (erreur pH ≤ 0,01)");
      _compFallbackCriticalLogged = true;
    }
  } else {
    _compFallbackSinceMs = 0;
    _compFallbackCriticalLogged = false;
  }

  // État exposé (accesseurs const consommés par /data, WS, MQTT).
  _ezoCompTempC = d.tempC;
  _ezoCompStage = d.stage;
  return d.tempC;
}

// =============================================================================
// Lecture pH / ORP via Atlas EZO
// =============================================================================

void SensorManager::_readEzoSensors(float tempC) {
  unsigned long now = millis();

  // v2.20.3 — PORTE d'alimentation des filtres : appliquée AVANT tout addSample()
  // de ce cycle. Placée en tête (et non dans chaque branche de lecture réussie)
  // pour que l'état de porte reste à jour même pendant une panne de bus I²C.
  _updateFilterGates((uint32_t)now);

  // ----- pH -----
  float ph = NAN;
  if (_phEzo.readSingle(ph, tempC)) {
    _lastPh = ph;
    _lastPhMs = now;
    _phI2cFailStreak = 0;
    // feature-025 : alimenter le filtre pH (médiane + EMA + rejet pics).
    // En cas de fail-streak I²C ci-dessous, on N'alimente PAS → le filtre devient
    // non prêt par âge (kSensorFilterMaxAgeMs) et canDose() bloque (fail-closed).
    // v2.8.4 : journaliser la transition du latch EMI (le module pur ne peut pas
    // logger). Pose = bruit soutenu ; levée = signal stable N fenêtres (auto).
    {
      bool wasLatched = _phFilter.unstableLatched();
      _phFilter.addSample(ph, (uint32_t)now);
      bool nowLatched = _phFilter.unstableLatched();
      if (nowLatched && !wasLatched)
        systemLogger.warning("Filtre pH : latch EMI posé (oscillation soutenue > seuil) — dosage inhibé");
      else if (!nowLatched && wasLatched)
        systemLogger.info("Filtre pH : latch EMI levé (signal stable) — dosage à nouveau possible si mesure OK");
    }
    // v2.20.3 : sortie de secours — ré-amorçage effectif ou REFUSÉ (fail-closed).
    _checkRearmAndLog(true);
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
    // feature-025 : alimenter le filtre ORP (cf. commentaire pH ci-dessus).
    // v2.8.4 : transition du latch EMI ORP (cf. pH ci-dessus).
    {
      bool wasLatched = _orpFilter.unstableLatched();
      _orpFilter.addSample(orp, (uint32_t)now);
      bool nowLatched = _orpFilter.unstableLatched();
      if (nowLatched && !wasLatched)
        systemLogger.warning("Filtre ORP : latch EMI posé (oscillation soutenue > seuil) — dosage inhibé");
      else if (!nowLatched && wasLatched)
        systemLogger.info("Filtre ORP : latch EMI levé (signal stable) — dosage à nouveau possible si mesure OK");
    }
    // v2.20.3 : sortie de secours — ré-amorçage effectif ou REFUSÉ (fail-closed).
    _checkRearmAndLog(false);
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

  // Trace capteur (feature-057) : on enregistre TOUJOURS (même si pH ou ORP = NaN),
  // pour visualiser les trous de lecture aussi clairement que les valeurs.
  // Contexte loopTask, non bloquant, aucun accès I²C ajouté.
  // feature-067 : le pH brut ET le pH filtré consignés restent BRUTS (membres
  // internes) ; seul phCorrected porte l'offset logiciel → la trace permet de
  // dire à tout instant ce qui vient de l'EZO et ce qui vient de la correction.
  _recordSensorTrace(_lastPh, applyPhOffset(_lastPh, _phOffset), _phFilter.filtered(),
                     _lastOrp, _orpFilter.filtered(), tempC);
}

// =============================================================================
// v2.20.3 — Porte d'alimentation des filtres (bug-filtre-ph-verrouille)
// =============================================================================
// Le filtre ne doit PAS ingérer des échantillons que le système lui-même a
// déclarés non représentatifs (eau à l'arrêt, purge de la chambre de mesure) :
// sinon sa médiane converge vers la valeur de l'eau STAGNANTE puis acquiert
// autorité sur les mesures représentatives suivantes, qu'elle rejette une à une
// comme aberrantes → verrouillage définitif, plus aucune régulation.
//
// Le verdict vient de `measureValidity` (mis à jour dans loop() AVANT
// sensors.update()) : on lit le CACHE sous spinlock, jamais filtration
// directement. La journalisation est edge-triggered ici — le module pur ne peut
// pas logger (ADR-0017), même pattern que le latch EMI.
void SensorManager::_updateFilterGates(uint32_t nowMs) {
  const MeasureValiditySnapshot mv = measureValidity.getSnapshot();
  const bool waterPresent = mv.waterPresent;
  const bool measuresValid = mv.measuresValid;

  const bool phWasClosed = _phFilter.gateClosed();
  const bool orpWasClosed = _orpFilter.gateClosed();

  _phFilter.setContext(waterPresent, measuresValid, nowMs);
  _orpFilter.setContext(waterPresent, measuresValid, nowMs);

  // Les deux filtres partagent le même contexte → une seule paire de logs.
  const bool nowClosed = _phFilter.gateClosed();
  if (nowClosed && !phWasClosed) {
    systemLogger.info(
        "Filtres pH/ORP : alimentation GELÉE — mesures non représentatives (" +
        String(waterPresent ? (mv.purgeCount > 0 ? "re-purge en cours" : "purge en cours")
                            : "circulation arrêtée") +
        ") — dosage auto bloqué tant que la porte est fermée");
  } else if (!nowClosed && phWasClosed) {
    systemLogger.info(
        "Filtres pH/ORP : alimentation REPRISE — mesures représentatives, filtres "
        "ré-amorcés (warmup en cours, la médiane de stagnation est abandonnée)");
  }
  (void)orpWasClosed;  // même contexte que le pH — conservé pour la lisibilité

  // Le ré-amorçage induit par la réouverture de la porte est LÉGITIME et déjà
  // journalisé ci-dessus : on draine le drapeau pour que _checkRearmAndLog() ne
  // l'attribue pas à tort à la sortie de secours sur rejets.
  _phFilter.rearmedSinceLastPoll();
  _orpFilter.rearmedSinceLastPoll();
}

// =============================================================================
// v2.20.3 — Sortie de secours : journalisation du ré-amorçage / de son refus
// =============================================================================
// Un ré-amorçage est un événement de sécurité chimique (le filtre abandonne sa
// médiane) → warning. Un REFUS est plus grave : le filtre RESTE verrouillé, donc
// il n'y a plus AUCUNE régulation → critical + alerte MQTT visible, une seule
// fois par épisode (rearmDenied() est collant).
void SensorManager::_checkRearmAndLog(bool isPh) {
  SensorFilter& filter = isPh ? _phFilter : _orpFilter;
  bool& deniedLogged = isPh ? _phRearmDeniedLogged : _orpRearmDeniedLogged;
  const char* label = isPh ? "pH" : "ORP";
  const unsigned int decimals = isPh ? 3 : 1;

  if (filter.rearmedSinceLastPoll()) {
    systemLogger.warning(
        "Filtre " + String(label) + " : RÉ-AMORÇAGE après " +
        String(kSensorFilterSpikeRearmCount) +
        " rejets cohérents autour de " + String(filter.raw(), decimals) +
        " — la grandeur mesurée a franchement changé, la médiane figée est "
        "abandonnée (warmup en cours, dosage auto bloqué)");
  }

  const uint8_t denied = filter.rearmDenied();
  if (denied != 0 && !deniedLogged) {
    const String motif =
        (denied == 1)
            ? String("délai anti-répétition de ") +
                  String(kSensorFilterRearmCooldownMs / 60000UL) +
                  " min non écoulé"
            : String("valeur hors bande admissible");
    const String msg =
        "Filtre " + String(label) + " : ré-amorçage REFUSÉ (" + motif +
        ") sur brut " + String(filter.raw(), decimals) +
        " — le filtre reste NON PRÊT, régulation " + String(label) +
        " à l'arrêt : vérifier la sonde";
    systemLogger.critical(msg);
    mqttManager.publishAlert(isPh ? "ph_filter_rearm_denied"
                                  : "orp_filter_rearm_denied",
                             msg);
    deniedLogged = true;
  } else if (denied == 0 && deniedLogged) {
    deniedLogged = false;  // épisode terminé (ré-amorçage réussi ou reset)
  }
}

// =============================================================================
// v2.20.3 (C1) — Alarme « filtre non prêt durablement » (bug-filtre-ph-verrouille)
// =============================================================================
// Un filtre non prêt ALORS QUE la filtration tourne et que les mesures sont
// déclarées représentatives = plus AUCUNE régulation sur ce paramètre. C'est
// exactement l'état dans lequel l'installation est restée ≥ 3 h le 2026-08-16,
// sans la moindre alarme. Verdict pool-chemistry P5 :
//   - déclenchement après kSensorFilterNotReadyAlarmMs (5 min) CONTINUES ;
//   - la purge de la chambre de mesure (measuresValid == false) et la
//     circulation arrêtée ne comptent JAMAIS (états bloquants légitimes) ;
//   - critical + alerte MQTT à la pose, répétition toutes les
//     kSensorFilterNotReadyRepeatMs (30 min) tant que la panne dure, info à la
//     levée. Edge-triggered de bout en bout : jamais de spam.
void SensorManager::_checkFilterNotReadyAlarm(uint32_t nowMs) {
  const MeasureValiditySnapshot mv = measureValidity.getSnapshot();
  const bool representative = mv.waterPresent && mv.measuresValid;

  _evalNotReadyAlarm(true,  representative && !isPhFilterReady(),  nowMs);
  _evalNotReadyAlarm(false, representative && !isOrpFilterReady(), nowMs);
}

void SensorManager::_evalNotReadyAlarm(bool isPh, bool blocked, uint32_t nowMs) {
  uint32_t& since     = isPh ? _phNotReadySinceMs      : _orpNotReadySinceMs;
  bool&     alarmed   = isPh ? _phNotReadyAlarmed      : _orpNotReadyAlarmed;
  uint32_t& lastAlarm = isPh ? _phNotReadyLastAlarmMs  : _orpNotReadyLastAlarmMs;
  const char* label   = isPh ? "pH" : "ORP";

  if (!blocked) {
    if (alarmed) {
      alarmed = false;
      systemLogger.info("Filtre " + String(label) +
                        " : de nouveau PRÊT — régulation " + String(label) +
                        " automatique réarmée (alerte levée)");
    }
    since = 0;  // l'épisode doit être CONTINU : toute interruption remet à zéro
    return;
  }

  if (since == 0) {
    // Sentinelle : `since == 0` signifie « pas d'épisode ». Au tout premier
    // millis() (valeur 0 possible juste après boot), on décale d'1 ms pour ne
    // pas confondre les deux — erreur d'horodatage bornée à 1 ms.
    since = (nowMs != 0) ? nowMs : 1;
    return;
  }
  if ((nowMs - since) < kSensorFilterNotReadyAlarmMs) return;
  if (alarmed && (nowMs - lastAlarm) < kSensorFilterNotReadyRepeatMs) return;

  const uint32_t minutes = (nowMs - since) / 60000UL;
  String cause;
  if (isPh) {
    if (isPhSensorFrozen())            cause = "sonde figée";
    else if (_phFilter.rearmDenied())  cause = "ré-amorçage refusé (sonde suspecte)";
    else if (isPhFilterUnstable())     cause = "capteur instable (bruit/EMI)";
    else                               cause = "fenêtre de mesure non remplie";
  } else {
    if (isOrpSensorFrozen())           cause = "sonde figée";
    else if (_orpFilter.rearmDenied()) cause = "ré-amorçage refusé (sonde suspecte)";
    else if (isOrpFilterUnstable())    cause = "capteur instable (bruit/EMI)";
    else                               cause = "fenêtre de mesure non remplie";
  }
  const uint8_t rejected = isPh ? getPhRejectedCount() : getOrpRejectedCount();
  const String msg =
      "Filtre " + String(label) + " NON PRÊT depuis " + String(minutes) +
      " min alors que la filtration tourne et que les mesures sont "
      "représentatives — régulation " + String(label) +
      " À L'ARRÊT (cause probable : " + cause + ", rejets=" + String(rejected) +
      "). Vérifier la sonde ; réinitialisation du filtre possible depuis l'interface.";
  systemLogger.critical(msg);
  mqttManager.publishAlert(isPh ? "ph_filter_not_ready" : "orp_filter_not_ready", msg);
  alarmed = true;
  lastAlarm = nowMs;
}

// =============================================================================
// Trace capteur (ring buffer RAM, feature-057)
// =============================================================================

void SensorManager::_recordSensorTrace(float phRaw, float phCorrected, float phFiltered,
                                       float orpRaw, float orpFiltered,
                                       float tempEau) {
  SensorTraceSample& s = _traceBuffer[_traceIdx];
  s.ms          = millis();
  s.phRaw       = phRaw;
  s.phCorrected = phCorrected;
  s.phFiltered  = phFiltered;
  s.orpRaw      = orpRaw;
  s.orpFiltered = orpFiltered;
  s.tempEau     = tempEau;
  _traceIdx = (_traceIdx + 1) % kSensorTraceBufferSize;
  if (_traceCount < kSensorTraceBufferSize) ++_traceCount;
}

size_t SensorManager::getSensorTraceCount() const {
  return _traceCount;
}

void SensorManager::appendSensorTraceJson(JsonArray out, size_t offset,
                                          size_t count) const {
  // Snapshot local en tête : _traceCount/_traceIdx peuvent être écrits par la
  // loop pendant l'itération (contrat lock-free, cf. sensors.h). On fige les
  // bornes une fois — pire cas : la page rate le tout dernier échantillon.
  size_t total = _traceCount;
  size_t idx0  = _traceIdx;
  // Re-clamp défensif : l'appelant passe des bornes déjà clampées
  // (computeTracePage), mais le buffer peut avoir glissé entre-temps.
  if (offset > total) offset = total;
  if (count > total - offset) count = total - offset;
  // Parcours dans l'ordre chronologique (plus ancien d'abord).
  // Buffer plein : le plus ancien est à idx0. Sinon : 0.
  size_t start = (total == kSensorTraceBufferSize) ? idx0 : 0;
  for (size_t i = offset; i < offset + count; ++i) {
    size_t idx = (start + i) % kSensorTraceBufferSize;
    const SensorTraceSample& s = _traceBuffer[idx];
    JsonObject o = out.add<JsonObject>();
    o["t"] = s.ms;
    if (!isnan(s.phRaw))       o["phRaw"]       = roundf(s.phRaw * 1000.0f) / 1000.0f;
    else                       o["phRaw"]       = nullptr;
    // feature-067 : pH corrigé (brut + offset) exposé À CÔTÉ du brut.
    if (!isnan(s.phCorrected)) o["phCorrected"] = roundf(s.phCorrected * 1000.0f) / 1000.0f;
    else                       o["phCorrected"] = nullptr;
    if (!isnan(s.phFiltered))  o["phFiltered"]  = roundf(s.phFiltered * 1000.0f) / 1000.0f;
    else                       o["phFiltered"]  = nullptr;
    if (!isnan(s.orpRaw))      o["orpRaw"]      = roundf(s.orpRaw * 10.0f) / 10.0f;
    else                       o["orpRaw"]      = nullptr;
    if (!isnan(s.orpFiltered)) o["orpFiltered"] = roundf(s.orpFiltered * 10.0f) / 10.0f;
    else                       o["orpFiltered"] = nullptr;
    if (!isnan(s.tempEau))     o["tempEau"]     = roundf(s.tempEau * 10.0f) / 10.0f;
    else                       o["tempEau"]     = nullptr;
  }
}

void SensorManager::clearSensorTrace() {
  _traceIdx = 0;
  _traceCount = 0;
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
// feature-022 Passe 2 — Détection capteur figé : logs edge-triggered
// =============================================================================
// Nommage explicite [SENSOR_FROZEN] (pool-chemistry condition #3) : la garde
// mutualisée FilterNotReady qui bloque le dosage ne dit pas la cause — c'est
// CE log qui porte le diagnostic. critical pour pH/ORP (dosage inhibé),
// warning pour la température. Log info à la levée.
// bug-142 [A8] : « warning-only » ne vaut que pour le DOSAGE — aucun chemin de
// `pump_controller` ne dépend de isTemperatureFrozen(). Le drapeau n'est pas
// inerte pour autant : il gate le planning de filtration (filtration.cpp:162
// bloque tout recalcul, filtration.cpp:405 rejette les échantillons de T°).

void SensorManager::_checkFrozenAndLog() {
  // pH — transition vers figé → critical, levée → info
  const bool phFrozen = _phFilter.frozen();
  if (phFrozen && !_phFrozenLogged) {
    systemLogger.critical("[SENSOR_FROZEN] Capteur pH figé : " +
                          String(kSensorFrozenSamples) + " lectures dans une bande < " +
                          String(kSensorFrozenEpsilonPh, 4) + " autour de " +
                          String(_phFilter.raw(), 3) +
                          " — régulation pH auto inhibée (filtre non prêt)");
    _phFrozenLogged = true;
  } else if (!phFrozen && _phFrozenLogged) {
    systemLogger.info("[SENSOR_FROZEN] Capteur pH à nouveau vivant — détection figée levée");
    _phFrozenLogged = false;
  }

  // ORP — idem
  const bool orpFrozen = _orpFilter.frozen();
  if (orpFrozen && !_orpFrozenLogged) {
    systemLogger.critical("[SENSOR_FROZEN] Capteur ORP figé : " +
                          String(kSensorFrozenSamples) + " lectures dans une bande < " +
                          String(kSensorFrozenEpsilonOrp, 2) + " mV autour de " +
                          String(_orpFilter.raw(), 1) + " mV" +
                          " — régulation ORP auto inhibée (filtre non prêt)");
    _orpFrozenLogged = true;
  } else if (!orpFrozen && _orpFrozenLogged) {
    systemLogger.info("[SENSOR_FROZEN] Capteur ORP à nouveau vivant — détection figée levée");
    _orpFrozenLogged = false;
  }

  // Température eau — warning-only pour le DOSAGE uniquement (aucun chemin
  // pump_controller) ; le drapeau gate néanmoins le planning de filtration
  // (filtration.cpp:162 et :405, bug-126/bug-142).
  const bool tempFrozen = _waterTempFrozen.frozen();
  if (tempFrozen && !_tempFrozenLogged) {
    systemLogger.warning("[SENSOR_FROZEN] Sonde T° eau figée : même valeur (" +
                         String(getWaterTemperatureRaw(), 1) + " °C) sur " +
                         String(kTempFrozenSamples) + " lectures consécutives (~" +
                         String(kTempFrozenWindowHours) + " h) — vérifier la sonde : "
                         "connecteur, câble, immersion. Le dosage n'est pas affecté ; "
                         "le planning de filtration n'est plus recalculé.");
    _tempFrozenLogged = true;
  } else if (!tempFrozen && _tempFrozenLogged) {
    systemLogger.info("[SENSOR_FROZEN] Sonde T° eau à nouveau vivante — détection figée levée");
    _tempFrozenLogged = false;
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
        resetPhFilter();  // feature-025 : warmup obligatoire après cal réussie
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
        resetPhFilter();  // feature-025 : warmup obligatoire après cal réussie
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
        resetOrpFilter();  // feature-025 : warmup obligatoire après cal réussie
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
        resetPhFilter();  // feature-025 : warmup obligatoire après clear réussi
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
        resetOrpFilter();  // feature-025 : warmup obligatoire après clear réussi
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
        // v2.21.2 (bug-slope-boot-perdue) : un échec alors qu'AUCUNE pente n'a
        // jamais été lue depuis le boot doit se voir au journal dès la PREMIÈRE
        // occurrence (avant : échec isolé MUET, streak=1 < kEzoBusFailMaxConsecutive).
        // UNE seule ligne (flag jamais réarmé) — les retentatives sont déjà
        // journalisées en info par update().
        if (_phSlopeQueriedMs == 0 && !_phSlopeBootFirstFailLogged) {
          _phSlopeBootFirstFailLogged = true;
          systemLogger.warning("EZO pH slope : échec de la Slope,? de boot "
                               "(aucune pente jamais lue) — retentative automatique");
        }
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

// feature-067 : getPh() renvoie la valeur CORRIGÉE (brut + offset logiciel).
// Le brut EZO reste accessible via getPhRaw() (règle : seuls les `*Raw` sont bruts).
// NaN in → NaN out : la fenêtre stale est portée par getPhRaw().
float SensorManager::getPh() const {
  return applyPhOffset(getPhRaw(), _phOffset);
}

float SensorManager::getOrp() const {
  if (isnan(_lastOrp)) return NAN;
  if (millis() - _lastOrpMs > kSensorStaleTimeoutMs) return NAN;
  return _lastOrp;
}

// =============================================================================
// feature-025 — Getters filtre pH / ORP (lock-free, contexte loopTask)
// =============================================================================
// getOrpRaw() retourne la même valeur brute que getOrp() (avec la même fenêtre
// stale) — exposé explicitement pour le diagnostic UI/MQTT.
//
// feature-067 — côté pH, `*Raw` n'est PLUS un alias : getPhRaw() est la mesure
// EZO SANS offset (même fenêtre stale que getPh()), getPhFilteredRaw() est la
// sortie du filtre SANS offset. Les autres accesseurs pH sont CORRIGÉS.

// Dernière mesure pH BRUTE (aucun offset), NaN si absente ou stale.
// Source unique de la fenêtre stale pH : getPh() s'appuie dessus.
float SensorManager::getPhRaw() const {
  if (isnan(_lastPh)) return NAN;
  if (millis() - _lastPhMs > kSensorStaleTimeoutMs) return NAN;
  return _lastPh;
}
float SensorManager::getPhMedian() const { return applyPhOffset(_phFilter.median(), _phOffset); }
float SensorManager::getPhFiltered() const { return applyPhOffset(_phFilter.filtered(), _phOffset); }
// Filets de sécurité chimique uniquement (planchers pool-chemistry) : sortie du
// filtre telle que produite par le capteur, insensible à un offset erroné.
// GARDE D'ÂGE OBLIGATOIRE (pool-chemistry P2) : `_phFilter.filtered()` n'expire
// JAMAIS de lui-même, seul `ready()` porte la fraîcheur. Sans cette garde, une
// EMA périmée (EZO muet) pourrait déclencher un faux plancher et consommer le
// compteur des 3 refus jusqu'à basculer la régulation en manuel sur une donnée
// fantôme — ou, pire, masquer un vrai franchissement. NaN = « pH inconnu », que
// chaque filet interprète selon sa propre logique fail-closed.
float SensorManager::getPhFilteredRaw() const {
  return _phFilter.ready(millis()) ? _phFilter.filtered() : NAN;
}
bool SensorManager::isPhFilterReady() const { return _phFilter.ready(millis()); }
bool SensorManager::isPhFilterUnstable() const { return _phFilter.unstable(); }
uint8_t SensorManager::getPhRejectedCount() const { return _phFilter.rejectedCount(); }

// v2.20.6 : la grandeur qui décide du drapeau d'instabilité, enfin observable.
float SensorManager::getPhDispersion() const { return _phFilter.dispersion(); }
float SensorManager::getOrpDispersion() const { return _orpFilter.dispersion(); }
uint8_t SensorManager::getPhDispersionCleanStreak() const {
  return _phFilter.dispersionCleanStreak();
}
uint8_t SensorManager::getOrpDispersionCleanStreak() const {
  return _orpFilter.dispersionCleanStreak();
}

float SensorManager::getOrpRaw() const { return getOrp(); }
float SensorManager::getOrpMedian() const { return _orpFilter.median(); }
float SensorManager::getOrpFiltered() const { return _orpFilter.filtered(); }
bool SensorManager::isOrpFilterReady() const { return _orpFilter.ready(millis()); }
bool SensorManager::isOrpFilterUnstable() const { return _orpFilter.unstable(); }
uint8_t SensorManager::getOrpRejectedCount() const { return _orpFilter.rejectedCount(); }

// v2.25.1 — état interne du filtre (discriminant des causes de refus de dosage).
// UN SEUL snapshot de millis() par appel : les quatre primitives doivent décrire
// le MÊME instant, sinon la cause affichée pourrait contredire le refus.
DoseFilterState SensorManager::getPhFilterState() const {
  const uint32_t nowMs = (uint32_t)millis();
  return resolveFilterState(_phFilter.frozen(), _phFilter.gateClosed(),
                            _phFilter.stale(nowMs), _phFilter.warmingUp());
}

DoseFilterState SensorManager::getOrpFilterState() const {
  const uint32_t nowMs = (uint32_t)millis();
  return resolveFilterState(_orpFilter.frozen(), _orpFilter.gateClosed(),
                            _orpFilter.stale(nowMs), _orpFilter.warmingUp());
}

// feature-022 Passe 2 — getters capteur figé (lock-free : simple comparaison
// d'entiers 16 bits, même contrat de concurrence que is*FilterUnstable()).
bool SensorManager::isPhSensorFrozen() const { return _phFilter.frozen(); }
bool SensorManager::isOrpSensorFrozen() const { return _orpFilter.frozen(); }
bool SensorManager::isTemperatureFrozen() const { return _waterTempFrozen.frozen(); }

// feature-067 — Rafraîchissement de la copie locale de l'offset pH.
// Appelée au boot et après CHAQUE enregistrement de config : la prise en compte
// est immédiate (l'offset est appliqué en sortie, pas dans la fenêtre de lissage).
// Aucun mutex : écriture d'un float aligné (atomique Xtensa), cf. contrat sensors.h.
void SensorManager::refreshPhOffsetCache() {
  float requested = mqttCfg.phOffset;
  // Fail-safe : une valeur hors bornes (ou NaN) ne doit jamais être appliquée à
  // la mesure qui pilote la régulation — on neutralise à 0.00.
  if (!isPhOffsetValid(requested)) {
    systemLogger.warning("Offset pH hors bornes (" + String(requested, 2) +
                         ") — offset appliqué remis à 0.00");
    requested = 0.0f;
  }
  if (requested == _phOffset) return;  // rien à journaliser
  // Journalisation exigée par pool-chemistry (condition #8) : traçabilité de
  // tout changement d'entrée de la régulation pH.
  systemLogger.info("Offset pH modifié: ancien=" + String(_phOffset, 2) +
                    " nouveau=" + String(requested, 2));
  _phOffset = requested;
}

void SensorManager::resetPhFilter() {
  _phFilter.reset();
  // v2.20.3 (C1) : un reset relance un warmup légitime (~65 s) → on désarme
  // l'alarme et on repart sur une fenêtre de kSensorFilterNotReadyAlarmMs
  // complète. Sans cela l'alarme se reposerait immédiatement après chaque
  // réparation, exactement au moment où l'utilisateur vient d'agir.
  _phNotReadySinceMs = 0;
  _phNotReadyAlarmed = false;
  systemLogger.info("Filtre pH réinitialisé — warmup en cours (dosage auto pH bloqué)");
}

void SensorManager::resetOrpFilter() {
  _orpFilter.reset();
  _orpNotReadySinceMs = 0;   // idem pH (cf. commentaire ci-dessus)
  _orpNotReadyAlarmed = false;
  systemLogger.info("Filtre ORP réinitialisé — warmup en cours (dosage auto ORP bloqué)");
}

// v2.20.3 — Demande DIFFÉRÉE de réinitialisation (appelable hors loopTask).
// Les `volatile bool` sont des drapeaux idempotents : une double demande depuis
// deux contextes ne produit qu'un seul reset. Aucun mutex nécessaire (écriture
// d'un bool aligné, atomique sur Xtensa LX6 — même contrat que _lastPh).
bool SensorManager::requestFilterReset(bool ph, bool orp, const char* origin) {
  if (!ph && !orp) return false;
  if (origin == nullptr) origin = "inconnue";
  // Origine posée AVANT le drapeau : loopTask ne peut donc jamais lire une
  // origine périmée pour une demande fraîche (ordre d'écriture significatif).
  if (ph)  { _phResetOrigin = origin;  _phFilterResetRequested = true; }
  if (orp) { _orpResetOrigin = origin; _orpFilterResetRequested = true; }
  return true;
}

// =============================================================================
// v2.20.3 (D4) — Exécution + journalisation d'une réinitialisation demandée
// =============================================================================
// Verdict pool-chemistry P6 : un reset manuel EFFACE la détection de sonde figée
// (feature-022) et le latch EMI (feature-058). Il doit donc laisser une trace de
// ce qu'il a détruit, sans quoi un utilisateur peut masquer une sonde HS à
// répétition sans que rien ne le signale. `frozen()` vrai au moment du reset →
// critical (on vient d'effacer un diagnostic matériel), sinon warning.
void SensorManager::_executeRequestedReset(bool isPh, const char* origin) {
  const char* label = isPh ? "pH" : "ORP";
  const unsigned int decimals = isPh ? 3 : 1;

  // --- État AVANT (celui qui va être détruit) ---
  // Médianes lues via les accesseurs publics (donc CORRIGÉES côté pH, ADR-0039) :
  // la trace doit correspondre à ce que l'utilisateur voyait dans l'interface.
  const float medianBefore  = isPh ? getPhMedian() : getOrpMedian();
  const uint8_t rejected    = isPh ? getPhRejectedCount() : getOrpRejectedCount();
  const bool latched        = isPh ? isPhFilterLatched() : isOrpFilterLatched();
  const bool frozenBefore   = isPh ? isPhSensorFrozen() : isOrpSensorFrozen();
  const bool readyBefore    = isPh ? isPhFilterReady() : isOrpFilterReady();

  if (isPh) resetPhFilter(); else resetOrpFilter();

  // --- État APRÈS (le reset vide le buffer → médiane NaN, warmup) ---
  const float medianAfter = isPh ? getPhMedian() : getOrpMedian();

  String msg = "Filtre " + String(label) +
               " : RÉINITIALISATION MANUELLE (origine : " + String(origin) +
               ") — médiane " +
               (isnan(medianBefore) ? String("--") : String(medianBefore, decimals)) +
               " → " +
               (isnan(medianAfter) ? String("--") : String(medianAfter, decimals)) +
               ", rejets=" + String(rejected) +
               ", latch EMI=" + String(latched ? "oui" : "non") +
               ", prêt=" + String(readyBefore ? "oui" : "non") +
               ", sonde figée=" + String(frozenBefore ? "OUI" : "non");
  if (frozenBefore) {
    systemLogger.critical(
        msg + " — la DÉTECTION DE SONDE FIGÉE vient d'être effacée : si la sonde "
              "est réellement HS, le défaut sera de nouveau masqué au prochain cycle");
  } else {
    systemLogger.warning(msg);
  }
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
// Enqueue de commandes — appelé par les handlers async (Pass 4)
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
  // bug-114 : offset BORNÉ — clamp **4/4 — ÉMISSION `RT`**. C'est LE site qui
  // alimente réellement la commande de compensation de l'EZO pH : cette valeur
  // remonte à _resolveEzoCompensationTemp(), qui la fait partir sur le bus I²C
  // dans `RT,<tempC>`. Les clamps 1/4 (écriture, web_routes_config.cpp), 2/4
  // (chargement NVS, config.cpp) et 3/4 (cache `tempValue`, plus haut) ne
  // couvrent PAS ce chemin ; celui-ci ne doit donc jamais être supprimé comme
  // doublon. Sans lui, un offset non borné (+1000 ⇒ `RT,1025.0`) fausse le pH
  // d'environ +0,15 sans toucher la calibration — donc invisible aux gardes
  // chimiques, qui sont calculées sur la mesure — et un offset très négatif
  // fait tendre la température de compensation vers 0 K, où l'erreur n'est
  // plus bornée du tout.
  return raw + clampTempCalOffset(mqttCfg.tempCalibrationOffset);
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
