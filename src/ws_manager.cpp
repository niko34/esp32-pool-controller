#include <WiFi.h>
#include <time.h>
#include <esp_system.h>
#include "ws_manager.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "sensors.h"
#include "filtration.h"
#include "pump_controller.h"
#include "web_routes_control.h"
#include "mqtt_manager.h"
#include "lighting.h"
#include "auth.h"
#include "json_compat.h"

static const char* getResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_EXT:       return "EXTERNAL";
    default:                return "UNKNOWN";
  }
}

WsManager wsManager;

// =============================================================================
// begin / update
// =============================================================================

void WsManager::begin(AsyncWebServer* server) {
  _ws = new AsyncWebSocket("/ws");
  _ws->onEvent([this](AsyncWebSocket* ws, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    _onEvent(ws, client, type, arg, data, len);
  });
  server->addHandler(_ws);

  systemLogger.info("WebSocket démarré sur /ws (push capteurs toutes les 5s)");
}

void WsManager::update() {
  if (!_ws) return;
  _ws->cleanupClients(4);  // Max 4 clients WS simultanés pour préserver les sockets lwIP

  if (_authenticatedClients.empty()) return;

  if (_pendingInitialPush) {
    _pendingInitialPush = false;
    _pendingConfigBroadcast = false;  // initial push couvre déjà la config
    broadcastSensorData();
    broadcastConfig();
    _lastSensorPush = millis();
    return;
  }

  if (_pendingConfigBroadcast) {
    _pendingConfigBroadcast = false;
    broadcastConfig();
  }

  if (millis() - _lastSensorPush >= kSensorPushIntervalMs) {
    broadcastSensorData();
    _lastSensorPush = millis();
  }
}

void WsManager::requestConfigBroadcast() {
  _pendingConfigBroadcast = true;
}

bool WsManager::hasClients() const {
  return _ws && _ws->count() > 0;
}

// =============================================================================
// Événements WebSocket
// =============================================================================

void WsManager::_onEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                          AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    _onClientConnect(client, reinterpret_cast<AsyncWebServerRequest*>(arg));
  } else if (type == WS_EVT_DATA) {
    _onData(client, data, len);
  } else if (type == WS_EVT_DISCONNECT) {
    _authenticatedClients.erase(client->id());
  }
}

void WsManager::_onClientConnect(AsyncWebSocketClient* client, AsyncWebServerRequest* request) {
  if (!authCfg.enabled) {
    // Pas d'auth : client immédiatement autorisé
    _authenticatedClients.insert(client->id());
    _pendingInitialPush = true;
  }
  // Si auth activée : attendre le message {"type":"auth","token":"..."} dans _onData
}

void WsManager::_onData(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
  StaticJson<256> doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;
  if (doc["type"] != "auth") return;

  String token = doc["token"] | "";
  // feature-028 : comparaison à temps constant (même exigence que l'auth HTTP)
  if (authCfg.enabled && !authManager.secureTokenEquals(token)) {
    systemLogger.warning("[WS] Token rejeté");
    client->close();
    return;
  }
  _authenticatedClients.insert(client->id());
  _pendingInitialPush = true;
}

// =============================================================================
// Broadcasts
// =============================================================================

void WsManager::broadcastSensorData() {
  if (!_ws || _ws->count() == 0) return;
  _ws->textAll(_buildSensorJson());
}

void WsManager::broadcastConfig() {
  if (!_ws || _ws->count() == 0) return;
  _ws->textAll(_buildConfigJson());
}

void WsManager::broadcastLog(const LogEntry& entry) {
  if (!_ws || _ws->count() == 0) return;
  StaticJson<192> doc;
  doc["type"] = "log";
  JsonObject d = doc["data"].to<JsonObject>();
  d["timestamp"] = entry.timestamp;
  d["level"] = systemLogger.getLevelString(entry.level);
  d["message"] = entry.message;
  String out;
  out.reserve(192);
  serializeJson(doc, out);
  _ws->textAll(out);
}

// =============================================================================
// Construction JSON
// =============================================================================

String WsManager::_buildSensorJson() const {
  // Buffer +64 octets vs version 1 sonde : champs temperature_circuit / sondes_identified / sondes_detected (feature-020)
  // feature-024 : +4 champs phSlope* (~80 octets) → bump à 1024.
  // feature-025 : +14 champs filtre pH/ORP + mixing/blocked (~300 octets) → bump à 1408.
  // feature-006 : +2 champs ph/orp_stab_remaining_s (~64 octets) → bump à 1472.
  // feature-011 : +2 champs ph/orp_scheduled_flow_ml_per_min (~80 octets) → bump à 1536.
  // feature-053 : +2 champs boost_active/boost_until (~48 octets) → bump à 1600.
  // feature-055 : +2 booléens boost_filtration_extended/boost_chlorine_boosted → bump à 1664.
  // feature-056 : +install_mode/water_present/filtration_state_source/_stale +
  //   filtration_ext_known/_on/_age_s (~200 octets) → bump à 1920.
  // v2.19.1 : +ph/orp_mix_remaining_s (~64 octets, observabilité pause mélange) → bump à 2048.
  StaticJson<2048> doc;
  doc["type"] = "sensor_data";
  JsonObject d = doc["data"].to<JsonObject>();

  // feature-021 : pH publié avec 3 décimales (l'EZO rend 3 décimales fiables, cf. spec ligne 247).
  // ORP reste en entier (mV) — la résolution physique du capteur ne justifie pas de décimales.
  // feature-025 : la "valeur courante" UI/MQTT (champs ph/orp) reflète désormais la valeur
  // FILTRÉE (getPhFiltered/getOrpFiltered), pas le brut. getPh()/getOrp() firmware restent
  // bruts (rétrocompat scheduled/diagnostic), mais l'utilisateur affiche la mesure lissée.
  // Le brut reste exposé séparément via phRaw/orpRaw pour diagnostic EMI. Si le filtre n'est
  // pas encore amorcé (NaN filtré), on retombe sur le brut pour ne pas afficher "--" au boot.
  // Lectures cachées en variables locales pour éviter une race entre le check `isnan` et le `round`.
  float orpRaw = sensors.getOrpRaw();
  float phRaw  = sensors.getPhRaw();
  float orpFiltered = sensors.getOrpFiltered();
  float phFiltered  = sensors.getPhFiltered();
  float orpMedian = sensors.getOrpMedian();
  float phMedian  = sensors.getPhMedian();
  // Valeur principale = filtrée si disponible, sinon brut (warmup), sinon null.
  float orpVal = !isnan(orpFiltered) ? orpFiltered : orpRaw;
  float phVal  = !isnan(phFiltered)  ? phFiltered  : phRaw;
  float tVal   = sensors.getTemperature();
  // T° eau brute (sans offset utilisateur) : exposée pour permettre à l'UI de calibration
  // de calculer un nouvel offset à partir d'une référence externe sans dépendre de la
  // formule firmware. NaN si sonde "eau" non identifiée.
  float tRawWater = sensors.getWaterTemperatureRaw();
  if (!isnan(orpVal))     d["orp"] = orpVal;                                    else d["orp"] = nullptr;
  if (!isnan(phVal))      d["ph"]  = round(phVal * 1000.0f) / 1000.0f;          else d["ph"] = nullptr;
  // feature-025 : champs filtre — null si NaN/indisponible (EZO débranché → UI sans crash).
  if (!isnan(phRaw))      d["phRaw"] = round(phRaw * 1000.0f) / 1000.0f;        else d["phRaw"] = nullptr;
  if (!isnan(phMedian))   d["phMedian"] = round(phMedian * 1000.0f) / 1000.0f;  else d["phMedian"] = nullptr;
  if (!isnan(phFiltered)) d["phFiltered"] = round(phFiltered * 1000.0f) / 1000.0f; else d["phFiltered"] = nullptr;
  d["phFilterReady"]    = sensors.isPhFilterReady();
  d["phFilterUnstable"] = sensors.isPhFilterUnstable();
  d["phRejectedCount"]  = sensors.getPhRejectedCount();
  if (!isnan(orpRaw))      d["orpRaw"] = round(orpRaw);                          else d["orpRaw"] = nullptr;
  if (!isnan(orpMedian))   d["orpMedian"] = round(orpMedian);                    else d["orpMedian"] = nullptr;
  if (!isnan(orpFiltered)) d["orpFiltered"] = round(orpFiltered);               else d["orpFiltered"] = nullptr;
  d["orpFilterReady"]    = sensors.isOrpFilterReady();
  d["orpFilterUnstable"] = sensors.isOrpFilterUnstable();
  d["orpRejectedCount"]  = sensors.getOrpRejectedCount();
  // Pause mélange hydraulique active (post-injection) + raison de blocage dosage.
  uint32_t nowMs = millis();
  d["phMixingDelayActive"]  = PumpController.isPhMixingDelayActive(nowMs);
  d["orpMixingDelayActive"] = PumpController.isOrpMixingDelayActive(nowMs);
  // Secondes restantes de pause mélange par pompe (observabilité widget dashboard).
  d["ph_mix_remaining_s"]  = PumpController.getMixingRemainingS(0);
  d["orp_mix_remaining_s"] = PumpController.getMixingRemainingS(1);
  String phBlocked  = PumpController.getPhDoseBlockedReason();
  String orpBlocked = PumpController.getOrpDoseBlockedReason();
  if (phBlocked.length() > 0)  d["phDoseBlockedReason"]  = phBlocked;  else d["phDoseBlockedReason"]  = nullptr;
  if (orpBlocked.length() > 0) d["orpDoseBlockedReason"] = orpBlocked; else d["orpDoseBlockedReason"] = nullptr;
  if (!isnan(tVal))       d["temperature"] = tVal;                              else d["temperature"] = nullptr;
  if (!isnan(tRawWater))  d["temperature_raw"] = round(tRawWater * 100.0f) / 100.0f; else d["temperature_raw"] = nullptr;
  // feature-020 : 2ᵉ sonde DS18B20 "circuit" + indicateurs identification
  float tc = sensors.getCircuitTemperature();
  if (!isnan(tc))                          d["temperature_circuit"] = round(tc * 10.0f) / 10.0f; else d["temperature_circuit"] = nullptr;
  d["sondes_identified"] = sensors.areSondesIdentified();
  d["sondes_detected"]   = sensors.getDetectedSondeCount();

  // feature-021 : statut calibration EZO (lecture cache, pas d'I²C dans le chemin WS).
  d["phCalPoints"]  = sensors.getPhCalibrationPointsCached();
  d["orpCalPoints"] = sensors.getOrpCalibrationPointsCached();

  // feature-024 : pente sonde pH (cache lu sans I²C).
  // Arrondis : pentes à 1 décimale (résolution EZO), zéro à 2 décimales (mV).
  // null si jamais lu OU bus dégradé (NaN), l'UI affiche alors "—".
  float slopeAcid = sensors.getPhSlopeAcid();
  float slopeBase = sensors.getPhSlopeBase();
  float slopeZero = sensors.getPhSlopeZero();
  if (!isnan(slopeAcid))  d["phSlopeAcid"] = round(slopeAcid * 10.0f) / 10.0f;     else d["phSlopeAcid"] = nullptr;
  if (!isnan(slopeBase))  d["phSlopeBase"] = round(slopeBase * 10.0f) / 10.0f;     else d["phSlopeBase"] = nullptr;
  if (!isnan(slopeZero))  d["phSlopeZero"] = round(slopeZero * 100.0f) / 100.0f;   else d["phSlopeZero"] = nullptr;
  // phSlopeAgeMs : null si jamais lu (cohérent avec phSlope* nullables), sinon ms écoulés.
  uint32_t slopeAge = sensors.getPhSlopeAgeMs();
  if (slopeAge == UINT32_MAX) d["phSlopeAgeMs"] = nullptr;
  else                        d["phSlopeAgeMs"] = slopeAge;

  d["filtration_running"]  = filtration.isRunning();
  d["filtration_force_on"] = filtrationCfg.forceOn;
  d["filtration_force_off"] = filtrationCfg.forceOff;
  d["ph_dosing"]           = PumpController.isPhDosing();
  d["orp_dosing"]         = PumpController.isOrpDosing();
  d["ph_used_ms"]         = PumpController.getPhUsedMs();
  d["orp_used_ms"]        = PumpController.getOrpUsedMs();
  d["stabilization_remaining_s"] = PumpController.getStabilizationRemainingS();
  // feature-006 : stabilisation PAR POMPE LOGIQUE (0=pH, 1=ORP), miroir exact
  // de la garde manuelle firmware (manualInjectGuardOrReject). Le champ global
  // ci-dessus (max des 2) est conservé pour compat (badge global, ancien front).
  d["ph_stab_remaining_s"]  = PumpController.getStabilizationRemainingS(0);
  d["orp_stab_remaining_s"] = PumpController.getStabilizationRemainingS(1);
  // feature-011 : débit moyen planifié en mode "Programmée" (mL/min sur la plage de
  // filtration restante). NAN firmware = hors mode scheduled / hors plage / heure
  // invalide → null explicite côté WS (l'UI affiche "—").
  float phSchedFlow  = PumpController.getPhScheduledPlannedFlow();
  float orpSchedFlow = PumpController.getOrpScheduledPlannedFlow();
  if (!isnan(phSchedFlow))  d["ph_scheduled_flow_ml_per_min"]  = round(phSchedFlow * 10.0f) / 10.0f;  else d["ph_scheduled_flow_ml_per_min"]  = nullptr;
  if (!isnan(orpSchedFlow)) d["orp_scheduled_flow_ml_per_min"] = round(orpSchedFlow * 10.0f) / 10.0f; else d["orp_scheduled_flow_ml_per_min"] = nullptr;
  d["ph_daily_ml"]        = safetyLimits.dailyPhInjectedMl;
  d["orp_daily_ml"]       = safetyLimits.dailyOrpInjectedMl;
  d["ph_limit_reached"]   = safetyLimits.phLimitReached;
  d["orp_limit_reached"]  = safetyLimits.orpLimitReached;

  // Volumes produits
  d["ph_tracking_enabled"]  = productCfg.phTrackingEnabled;
  d["ph_remaining_ml"]      = max(0.0f, productCfg.phContainerVolumeMl - productCfg.phTotalInjectedMl);
  d["ph_container_ml"]      = productCfg.phContainerVolumeMl;
  d["ph_alert_threshold_ml"]= productCfg.phAlertThresholdMl;
  d["orp_tracking_enabled"] = productCfg.orpTrackingEnabled;
  d["orp_remaining_ml"]     = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  d["orp_container_ml"]     = productCfg.orpContainerVolumeMl;
  d["orp_alert_threshold_ml"]= productCfg.orpAlertThresholdMl;

  d["ph_inject_remaining_s"]  = manualInjectRemainingS(manualInjectPh);
  d["orp_inject_remaining_s"] = manualInjectRemainingS(manualInjectOrp);

  d["lighting_enabled"] = lighting.isOn();  // état réel du relais, pas lightingCfg.enabled

  // feature-053 : Mode Boost — état effectif (isBoostActive expire à minuit) + epoch
  // d'expiration (0 si inactif). Le client calcule le temps restant.
  {
    time_t nowEpoch = time(nullptr);
    d["boost_active"] = isBoostActive(nowEpoch);
    d["boost_until"]  = (long)boostState.untilEpoch;
    // feature-055 : leviers réellement actifs du Boost (affichage persistant du widget)
    // feature-056 : filtration prolongée seulement si PC gère la filtration (Managed).
    d["boost_filtration_extended"] = (mqttCfg.installMode == InstallMode::ManagedFiltration);
    d["boost_chlorine_boosted"]    = (mqttCfg.orpRegulationMode == "automatic");
  }

  // feature-056 : présence d'eau résolue (source UNIQUE) + mode d'installation.
  // Hooks pour l'UI (source/fraîcheur du signal filtration) — champs affinables
  // par web-ui-developer.
  {
    WaterPresence wp = filtration.resolveWaterPresence();
    d["install_mode"]  = installModeToString(mqttCfg.installMode);
    d["water_present"] = wp.waterPresent;
    d["filtration_state_stale"] = wp.stale;
    const char* src = "commanded";
    switch (wp.source) {
      case WaterSource::FiltrationCommanded: src = "commanded"; break;
      case WaterSource::PoweredAssumed:      src = "powered";   break;
      case WaterSource::ExternalSignal:      src = "external";  break;
    }
    d["filtration_state_source"] = src;
    // feature-056 : détail du signal externe pour la pill UI (mode external) —
    // distingue « Arrêtée » (OFF connu) de « Aucun signal » (boot) et donne l'âge.
    bool extOn = false, extKnown = false;
    uint32_t extLastMs = 0;
    filtration.getExternalState(extOn, extLastMs, extKnown);
    d["filtration_ext_known"] = extKnown;
    d["filtration_ext_on"]    = extOn;
    d["filtration_ext_age_s"] = extKnown ? (uint32_t)(((uint32_t)millis() - extLastMs) / 1000UL) : 0;
  }

  d["time_synced"]   = (time(nullptr) >= kMinValidEpoch);
  d["uptime_ms"]     = millis();
  d["reset_reason"]  = getResetReason();
  // Statut MQTT poussé toutes les 5s pour rafraîchir le badge UI sans reload (feature-015).
  // Lit connectedAtomic (atomic relaxed) — pas de mutex nécessaire (cf. feature-014 IT2).
  d["mqtt_connected"] = mqttManager.isConnected();

  String out;
  // +64 octets feature-020 (temperature_circuit + sondes_identified + sondes_detected)
  // +80 octets feature-024 (phSlopeAcid/Base/Zero/AgeMs)
  // +300 octets feature-025 (filtre pH/ORP + mixing/blocked)
  // +80 octets feature-011 (ph/orp_scheduled_flow_ml_per_min)
  out.reserve(1600);
  serializeJson(doc, out);
  return out;
}

String WsManager::_buildConfigJson() const {
  // feature-053 : +2 champs boost_active/boost_until → marge portée à 2304.
  StaticJson<2304> doc;
  doc["type"] = "config";
  JsonObject d = doc["data"].to<JsonObject>();

  d["server"]           = mqttCfg.server;
  d["port"]             = mqttCfg.port;
  d["topic"]            = mqttCfg.topic;
  d["username"]         = mqttCfg.username;
  d["password"]         = mqttCfg.password.length() > 0 ? "******" : "";
  d["enabled"]          = mqttCfg.enabled;
  d["mqtt_connected"]   = mqttManager.isConnected();
  d["ph_target"]        = mqttCfg.phTarget;
  d["orp_target"]       = mqttCfg.orpTarget;
  d["ph_enabled"]       = mqttCfg.phEnabled;  // dérivé : true si mode != manual
  d["ph_regulation_mode"] = mqttCfg.phRegulationMode;
  d["ph_daily_target_ml"] = mqttCfg.phDailyTargetMl;
  d["ph_pump"]          = mqttCfg.phPump;
  d["orp_enabled"]      = mqttCfg.orpEnabled;  // miroir : true si orpRegulationMode != manual
  d["orp_regulation_mode"] = mqttCfg.orpRegulationMode;
  d["orp_daily_target_ml"] = mqttCfg.orpDailyTargetMl;
  d["max_orp_ml_per_day"]  = safetyLimits.maxChlorineMlPerDay;
  // feature-021 : orp_cal_valid dérivé de Cal,? (cache) plutôt que d'une date NVS supprimée.
  d["orp_cal_valid"]    = sensors.getOrpCalibrationPointsCached() >= 1;
  d["orp_pump"]         = mqttCfg.orpPump;
  d["pump1_max_duty_pct"] = mqttCfg.pump1MaxDutyPct;
  d["pump2_max_duty_pct"] = mqttCfg.pump2MaxDutyPct;
  d["ph_limit_minutes"] = mqttCfg.phInjectionLimitMinutes;
  d["orp_limit_minutes"] = mqttCfg.orpInjectionLimitMinutes;
  d["install_mode"]     = installModeToString(mqttCfg.installMode);  // feature-056
  d["ph_correction_type"] = mqttCfg.phCorrectionType;
  d["time_use_ntp"]     = mqttCfg.timeUseNtp;
  d["ntp_server"]       = mqttCfg.ntpServer;
  d["manual_time"]      = mqttCfg.manualTimeIso;
  d["timezone_id"]      = mqttCfg.timezoneId;
  d["filtration_mode"]  = filtrationCfg.mode;  // feature-056 : filtration_enabled → install_mode
  d["filtration_start"] = filtrationCfg.start;
  d["filtration_end"]   = filtrationCfg.end;
  d["filtration_running"] = filtration.isRunning();
  d["lighting_feature_enabled"] = lightingCfg.featureEnabled;
  d["lighting_enabled"] = lightingCfg.enabled;
  d["lighting_brightness"] = lightingCfg.brightness;
  d["lighting_schedule_enabled"] = lightingCfg.scheduleEnabled;
  d["lighting_start_time"] = lightingCfg.startTime;
  d["lighting_end_time"]   = lightingCfg.endTime;
  d["wifi_ssid"]        = WiFi.SSID();

  wifi_mode_t mode = WiFi.getMode();
  String ip;
  if (mode == WIFI_MODE_AP)         ip = WiFi.softAPIP().toString();
  else if (mode == WIFI_MODE_APSTA) ip = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  else                              ip = WiFi.localIP().toString();
  d["wifi_ip"]          = ip;
  d["wifi_mode"]        = mode == WIFI_MODE_AP ? "AP" : (mode == WIFI_MODE_APSTA ? "AP+STA" : "STA");
  d["mdns_host"]        = kMdnsFullHost;
  d["max_ph_ml_per_day"]      = safetyLimits.maxPhMlPerDay;
  d["max_chlorine_ml_per_day"]= safetyLimits.maxChlorineMlPerDay;
  // feature-021 : statut calibration EZO depuis le cache Cal,? (lecture sans I²C).
  // ph_cal_valid = au moins 1 point calibré ; les détails (mid/low) sont dans phCalPoints.
  d["ph_cal_valid"]     = sensors.getPhCalibrationPointsCached() >= 1;
  d["ph_cal_points"]    = sensors.getPhCalibrationPointsCached();
  d["orp_cal_points"]   = sensors.getOrpCalibrationPointsCached();
  d["temp_calibration_offset"]   = mqttCfg.tempCalibrationOffset;
  d["temp_calibration_date"]     = mqttCfg.tempCalibrationDate;
  d["temperature_enabled"]       = mqttCfg.temperatureEnabled;
  d["auth_enabled"]        = authCfg.enabled;
  d["sensor_logs_enabled"] = authCfg.sensorLogsEnabled;
  d["debug_logs_enabled"]  = authCfg.debugLogsEnabled;
  d["auth_password"]       = authCfg.adminPassword.length() > 0 ? "******" : "";
  d["auth_token"]          = authCfg.apiToken.length() > 8 ? (authCfg.apiToken.substring(0, 8) + "...") : "";
  d["time_current"]        = getCurrentTimeISO();
  // feature-053 : Mode Boost (état effectif + epoch d'expiration, 0 si inactif).
  d["boost_active"]        = isBoostActive(time(nullptr));
  d["boost_until"]         = (long)boostState.untilEpoch;

  String out;
  out.reserve(2048);
  serializeJson(doc, out);
  return out;
}
