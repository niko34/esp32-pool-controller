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
  if (authCfg.enabled && token != authCfg.apiToken) {
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
  StaticJson<1024> doc;
  doc["type"] = "sensor_data";
  JsonObject d = doc["data"].to<JsonObject>();

  // feature-021 : pH publié avec 3 décimales (l'EZO rend 3 décimales fiables, cf. spec ligne 247).
  // ORP reste en entier (mV) — la résolution physique du capteur ne justifie pas de décimales.
  // Les champs *_raw / ph_voltage_mv ont été supprimés Pass 4a : l'EZO calibre en interne,
  // il n'existe pas de "valeur brute" séparée côté firmware.
  // Lectures cachées en variables locales pour éviter une race entre le check `isnan` et le `round`.
  float orpVal = sensors.getOrp();
  float phVal  = sensors.getPh();
  float tVal   = sensors.getTemperature();
  // T° eau brute (sans offset utilisateur) : exposée pour permettre à l'UI de calibration
  // de calculer un nouvel offset à partir d'une référence externe sans dépendre de la
  // formule firmware. NaN si sonde "eau" non identifiée.
  float tRawWater = sensors.getWaterTemperatureRaw();
  if (!isnan(orpVal))     d["orp"] = orpVal;                                    else d["orp"] = nullptr;
  if (!isnan(phVal))      d["ph"]  = round(phVal * 1000.0f) / 1000.0f;          else d["ph"] = nullptr;
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

  d["time_synced"]   = (time(nullptr) >= kMinValidEpoch);
  d["uptime_ms"]     = millis();
  d["reset_reason"]  = getResetReason();
  // Statut MQTT poussé toutes les 5s pour rafraîchir le badge UI sans reload (feature-015).
  // Lit connectedAtomic (atomic relaxed) — pas de mutex nécessaire (cf. feature-014 IT2).
  d["mqtt_connected"] = mqttManager.isConnected();

  String out;
  // +64 octets feature-020 (temperature_circuit + sondes_identified + sondes_detected)
  // +80 octets feature-024 (phSlopeAcid/Base/Zero/AgeMs)
  out.reserve(896);
  serializeJson(doc, out);
  return out;
}

String WsManager::_buildConfigJson() const {
  StaticJson<2048> doc;
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
  d["regulation_mode"]  = mqttCfg.regulationMode;
  d["ph_correction_type"] = mqttCfg.phCorrectionType;
  d["time_use_ntp"]     = mqttCfg.timeUseNtp;
  d["ntp_server"]       = mqttCfg.ntpServer;
  d["manual_time"]      = mqttCfg.manualTimeIso;
  d["timezone_id"]      = mqttCfg.timezoneId;
  d["filtration_enabled"] = filtrationCfg.enabled;
  d["filtration_mode"]  = filtrationCfg.mode;
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
  d["max_ph_ml_per_day"]      = safetyLimits.maxPhMinusMlPerDay;
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
  d["auth_cors_origins"]   = authCfg.corsAllowedOrigins;
  d["time_current"]        = getCurrentTimeISO();

  String out;
  out.reserve(2048);
  serializeJson(doc, out);
  return out;
}
