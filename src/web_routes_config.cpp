#include "web_routes_config.h"
#include "web_helpers.h"
#include "config.h"
#include "constants.h"
#include "auth.h"
#include "ws_manager.h"
#include "sensors.h"
#include "filtration.h"
#include "lighting.h"
#include "mqtt_manager.h"
#include "pump_controller.h"
#include "dosing_logic.h"  // feature-060 : clampCyclesPerDay (clamp serveur obligatoire)
#include "ph_offset.h"     // feature-067 : isPhOffsetValid (validation serveur de ph_offset)
#include "ph_guard_logic.h"  // v2.25.0 : phModeChangeNeedsAck (gate fail-closed du mode pH)
#include "logger.h"
#include "version.h"
#include "json_compat.h"
#include "heap_metrics.h"  // v2.19.0 : mémoire RÉELLEMENT allouable (8 bits) exposée par /get-system-info
#include "oom_guard.h"     // v2.20.2 : état d'inscription du new_handler exposé par /get-system-info
#include "rtc_manager.h"
#include <sys/time.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <AsyncJson.h>
#include <esp_ota_ops.h>

// Fonctions externes déclarées dans main.cpp
extern void resetWiFiSettings();
extern void applyTimeConfig();

// Contexte pour les buffers de configuration (partagé avec web_server)
static std::map<AsyncWebServerRequest*, std::vector<uint8_t>>* g_configBuffers = nullptr;
static std::map<AsyncWebServerRequest*, bool>* g_configErrors = nullptr;

// Message d'erreur SPÉCIFIQUE associé à une requête refusée (feature-067, revue).
// POURQUOI : handleSaveConfig est le handler de CORPS ; il s'exécute AVANT le
// handler de requête. Un `request->send(400, ...)` émis ici est ensuite ÉCRASÉ
// par le `req->send(200, "OK")` du lambda de complétion — l'utilisateur verrait
// « Sauvegarde réussie » alors que rien n'a été appliqué. La convention du
// fichier est donc de POSER UN DRAPEAU et de laisser la complétion répondre ;
// cette map préserve en plus le libellé précis (sans elle, on retomberait sur le
// générique « Invalid JSON configuration », inexploitable pour l'utilisateur).
static std::map<AsyncWebServerRequest*, String> g_configErrorMessages;

// Code HTTP du refus associé à une requête (v2.25.0). Absent = 400 (le seul code
// de refus qui existait avant). Le gate d'acquittement de la bascule de sécurité
// pH répond 409 Conflict : ce n'est pas une valeur invalide (400) mais un état
// serveur qui interdit la transition tant qu'elle n'est pas acquittée.
// MÊME CONTRAINTE que g_configErrorMessages : c'est la COMPLÉTION qui répond.
// Un `request->send(409, ...)` posé ici serait remplacé par le `req->send(200,
// "OK")` du lambda de complétion (WebRequest.cpp : send() delete l'ancienne
// réponse et la remplace) — le navigateur verrait « enregistré » sur un refus,
// et l'UI ne pourrait pas revenir en arrière.
static std::map<AsyncWebServerRequest*, int> g_configErrorStatus;

// Variables globales pour la reconnexion WiFi asynchrone
static bool g_wifiReconnectRequested = false;
static String g_wifiReconnectSsid = "";
static String g_wifiReconnectPassword = "";
static unsigned long g_wifiReconnectRequestedTime = 0;

static bool wifiConfigAllowed() {
  wifi_mode_t mode = WiFi.getMode();
  // Autoriser la configuration WiFi uniquement en mode AP (pas de WiFi configuré)
  // ou si on n'est pas connecté en mode STA
  if (mode == WIFI_MODE_AP) {
    return true;
  }
  // En mode STA ou APSTA, autoriser seulement si pas connecté
  return !WiFi.isConnected();
}

void initConfigContext(
  std::map<AsyncWebServerRequest*, std::vector<uint8_t>>* configBuffers,
  std::map<AsyncWebServerRequest*, bool>* configErrors
) {
  g_configBuffers = configBuffers;
  g_configErrors = configErrors;
}

// feature-088 : VALEURS D'USINE des réglages de régulation, sérialisées depuis
// la source UNIQUE (regulation_defaults.h, via config.h). Partagé entre
// /get-config (dialogue avant/après de l'UI — qui ne doit JAMAIS porter ces
// valeurs en constantes JS) et la réponse de /reset-regulation-defaults.
// Mêmes clés que les champs de config correspondants de /get-config.
// feature-090 : paramétré par la TAILLE DE BASSIN — sérialise la colonne
// correspondante de kRegulationDefaultsBySize.
static void fillRegulationDefaults(JsonObject o, PoolSize size) {
  const RegulationDefaults& d = regulationDefaultsFor(size);
  o["ph_target"] = d.phTarget;
  o["orp_target"] = d.orpTarget;
  o["ph_limit_minutes"] = d.phInjectionLimitMin;
  o["orp_limit_minutes"] = d.orpInjectionLimitMin;
  // feature-091 : stabilization_delay_min RETIRÉ (7 réglages) — la purge est
  // désormais pilotée par la stabilité des mesures (plancher/plafond constants).
  o["ph_daily_target_ml"] = d.phDailyTargetMl;
  o["orp_daily_target_ml"] = d.orpDailyTargetMl;
  o["regulation_speed"] = d.regulationSpeed;
}

static void handleGetConfig(AsyncWebServerRequest* request) {
  // Vérifier si l'utilisateur est authentifié pour accéder aux données sensibles
  // Note: On ne bloque PAS la requête (route publique pour premier démarrage),
  // mais on vérifie l'authentification pour savoir quelles données révéler
  // On utilise checkTokenAuth/checkBasicAuth directement pour ne pas bloquer avec sendAuthRequired()
  bool isAuthenticated = authManager.checkTokenAuth(request) || authManager.checkBasicAuth(request);

  // Buffer statique : ~55 champs (configs MQTT, pH, ORP, calibration, WiFi, boost, etc.)
  // feature-053 : +2 champs boost_active/boost_until → marge portée à 2304.
  // feature-067 : +1 champ ph_offset (~24 octets) → marge portée à 2368.
  // v2.20.3 (C5) : +ph_auto_manual_epoch (~34 o) et +ph_auto_manual_reason
  // (clé 24 o + raison bornée par le libellé firmware, ~140 o max) → +200 o
  // → marge portée à 2624. La raison n'est JAMAIS saisie par l'utilisateur
  // (littéral construit par pump_controller), sa longueur est donc bornée.
  // Itération 2 (bug-dosage-fantome) : +4 champs de caractérisation pompes
  // (~130 o au pire) → marge portée à 2752.
  // feature-074 : +2 drapeaux pumpN_flow_weighed (~56 o) → marge portée à 2816.
  // v2.25.0 (B1) : +6 bornes de réglage pH (~30 o chacune) → marge portée à 3072.
  // feature-088 : +objet regulation_defaults (8 valeurs, ~130 o) → marge 3328.
  // feature-090 : +pool_size (~22 o) et +regulation_defaults_by_size (3 colonnes
  // de 8 valeurs, ~410 o) → marge portée à 3776.
  // feature-091 (v2.34.0) : stabilization_delay_min RETIRÉ — regulation_defaults
  // et chacune des 3 colonnes passent de 8 à 7 valeurs (≈ −65 o au total), et la
  // clé de premier niveau disparaît. La marge 3776 est donc CONSERVÉE TELLE
  // QUELLE : elle ne fait que s'élargir. Ne pas la réduire sans recompter.
  StaticJson<3776> doc;
  doc["server"] = mqttCfg.server;
  doc["port"] = mqttCfg.port;
  doc["topic"] = mqttCfg.topic;
  doc["username"] = mqttCfg.username;
  // SÉCURITÉ: Ne jamais envoyer les mots de passe en clair (même si authentifié)
  doc["password"] = mqttCfg.password.length() > 0 ? "******" : "";
  doc["enabled"] = mqttCfg.enabled;
  doc["mqtt_connected"] = mqttManager.isConnected();
  doc["ph_target"] = roundf(mqttCfg.phTarget * 100.0f) / 100.0f;
  doc["orp_target"] = roundf(mqttCfg.orpTarget);
  doc["ph_enabled"] = mqttCfg.phEnabled;
  doc["ph_regulation_mode"] = mqttCfg.phRegulationMode;
  doc["ph_daily_target_ml"] = mqttCfg.phDailyTargetMl;
  doc["ph_pump"] = mqttCfg.phPump;
  doc["orp_enabled"] = mqttCfg.orpEnabled;  // miroir : true si orpRegulationMode != manual
  doc["orp_regulation_mode"] = mqttCfg.orpRegulationMode;
  doc["orp_daily_target_ml"] = mqttCfg.orpDailyTargetMl;
  doc["max_orp_ml_per_day"] = safetyLimits.maxChlorineMlPerDay;
  // feature-021 : orp_cal_valid dérivé du cache Cal,? EZO (plus de date NVS).
  doc["orp_cal_valid"] = sensors.getOrpCalibrationPointsCached() >= 1;
  doc["orp_pump"] = mqttCfg.orpPump;
  doc["pump1_max_duty_pct"] = mqttCfg.pump1MaxDutyPct;
  doc["pump2_max_duty_pct"] = mqttCfg.pump2MaxDutyPct;
  // Itération 2 (bug-dosage-fantome) : pump_max_flow_ml_per_min SUPPRIMÉ (modèle
  // linéaire retiré). À la place : caractérisation MESURÉE par pompe (pesée au
  // banc). calibrated_duty == 0 = pompe jamais caractérisée → tout dosage
  // automatique/programmé REFUSÉ (fail-closed, cf. src/pulse_dosing_logic.h).
  doc["pump1_calibrated_duty"] = mqttCfg.pump1CalibratedDuty;
  doc["pump1_pulse_flow_ml_per_min"] = mqttCfg.pump1PulseFlowMlPerMin;
  doc["pump2_calibrated_duty"] = mqttCfg.pump2CalibratedDuty;
  doc["pump2_pulse_flow_ml_per_min"] = mqttCfg.pump2PulseFlowMlPerMin;
  // feature-074 : provenance du débit — false = calibration de type (valeur
  // usine du modèle de pompe), true = pesée individuelle de CETTE pompe.
  // Sert à l'indication discrète « valeur usine / pesé » de l'UI.
  doc["pump1_flow_weighed"] = mqttCfg.pump1FlowWeighed;
  doc["pump2_flow_weighed"] = mqttCfg.pump2FlowWeighed;
  doc["ph_limit_minutes"] = mqttCfg.phInjectionLimitMinutes;
  doc["orp_limit_minutes"] = mqttCfg.orpInjectionLimitMinutes;
  doc["max_cycles_per_day"] = pumpProtection.maxCyclesPerDay;  // feature-060 : garde 9 configurable (10-60)
  doc["install_mode"] = installModeToString(mqttCfg.installMode);  // feature-056
  // feature-091 : stabilization_delay_min n'est plus exposé (réglage inerte,
  // conservé en NVS pour la compatibilité descendante — cf. A8 de la spec).
  doc["regulation_speed"] = mqttCfg.regulationSpeed;
  // feature-090 : taille du bassin (présélection des défauts, jamais une
  // entrée de calcul). Un client ancien ignore la clé ; un client nouveau face
  // à un vieux firmware masque le sélecteur (détection par présence du champ).
  doc["pool_size"] = poolSizeToString(mqttCfg.poolSize);
  doc["ph_correction_type"] = mqttCfg.phCorrectionType;
  doc["time_use_ntp"] = mqttCfg.timeUseNtp;
  doc["ntp_server"] = mqttCfg.ntpServer;
  doc["manual_time"] = mqttCfg.manualTimeIso;
  doc["timezone_id"] = mqttCfg.timezoneId;
  doc["filtration_mode"] = filtrationCfg.mode;  // feature-056 : filtration_enabled → install_mode
  doc["filtration_start"] = filtrationCfg.start;
  doc["filtration_end"] = filtrationCfg.end;
  doc["filtration_running"] = filtration.isRunning();
  doc["lighting_feature_enabled"] = lightingCfg.featureEnabled;
  doc["lighting_enabled"] = lightingCfg.enabled;
  doc["lighting_brightness"] = lightingCfg.brightness;
  doc["lighting_schedule_enabled"] = lightingCfg.scheduleEnabled;
  doc["lighting_start_time"] = lightingCfg.startTime;
  doc["lighting_end_time"] = lightingCfg.endTime;
  doc["wifi_ssid"] = WiFi.SSID();

  // Déterminer l'IP à afficher selon le mode WiFi
  wifi_mode_t mode = WiFi.getMode();
  String ipAddress;
  if (mode == WIFI_MODE_AP) {
    // En mode AP uniquement, afficher l'IP de l'AP
    ipAddress = WiFi.softAPIP().toString();
  } else if (mode == WIFI_MODE_APSTA) {
    // En mode AP+STA, afficher l'IP STA si connecté, sinon l'IP AP
    if (WiFi.isConnected()) {
      ipAddress = WiFi.localIP().toString();
    } else {
      ipAddress = WiFi.softAPIP().toString();
    }
  } else {
    // En mode STA, afficher l'IP STA
    ipAddress = WiFi.localIP().toString();
  }
  doc["wifi_ip"] = ipAddress;

  doc["wifi_mode"] = mode == WIFI_MODE_AP ? "AP" : (mode == WIFI_MODE_APSTA ? "AP+STA" : "STA");
  doc["mdns_host"] = kMdnsFullHost;
  doc["max_ph_ml_per_day"] = safetyLimits.maxPhMlPerDay;
  doc["max_chlorine_ml_per_day"] = safetyLimits.maxChlorineMlPerDay;
  // feature-021 : Calibration capteurs Atlas EZO — stockée dans le module
  // (NVS interne EZO), exposée ici via le cache Cal,?.
  // Les champs voltage / offset / slope / date NVS legacy ont été supprimés.
  doc["ph_cal_valid"]   = sensors.getPhCalibrationPointsCached() >= 1;
  doc["ph_cal_points"]  = sensors.getPhCalibrationPointsCached();
  doc["orp_cal_points"] = sensors.getOrpCalibrationPointsCached();

  // feature-067 : offset logiciel de mesure pH (pH corrigé = pH brut EZO + offset).
  // Ne remplace PAS la calibration EZO (ph_cal_points ci-dessus) — champ distinct.
  doc["ph_offset"] = mqttCfg.phOffset;

  // v2.25.0 (B1) — BORNES DE RÉGLAGE pH servies au client.
  // L'interface construit ses contrôles (positions de la consigne, pas de
  // l'incrémenteur d'offset) à partir de CES valeurs : plus aucune borne de
  // sécurité n'est écrite en dur dans data/app.js, donc plus de miroir à
  // maintenir. Élargir la plage ou changer le pas se fait ici (constants.h),
  // sans retouche du frontend.
  // VOLONTAIREMENT ABSENTES de la trame WebSocket : ce sont des constantes de
  // compilation, elles n'ont pas à consommer le budget d'une trame émise toutes
  // les quelques secondes. app.js conserve donc les dernières bornes reçues.
  doc["ph_target_min"]  = kPhTargetMin;
  doc["ph_target_max"]  = kPhTargetMax;
  doc["ph_target_step"] = kPhTargetStep;
  doc["ph_offset_min"]  = kPhOffsetMin;
  doc["ph_offset_max"]  = kPhOffsetMax;
  doc["ph_offset_step"] = kPhOffsetStep;

  // v2.20.3 (C5) : bascule AUTOMATIQUE de la régulation pH en manuel, persistée.
  // `ph_auto_manual_epoch` == 0 → aucune bascule automatique en attente
  // d'acquittement (cas de tous les appareils avant 2.20.3 : clé NVS absente).
  // L'UI n'affiche l'encart que si epoch != 0 OU si la raison est non vide.
  doc["ph_auto_manual_epoch"]  = mqttCfg.phAutoManualEpoch;
  doc["ph_auto_manual_reason"] = mqttCfg.phAutoManualReason;

  // Données de calibration Température (offset utilisateur DS18B20)
  doc["temp_calibration_offset"] = mqttCfg.tempCalibrationOffset;
  doc["temp_calibration_date"] = mqttCfg.tempCalibrationDate;
  doc["temperature_enabled"] = mqttCfg.temperatureEnabled;

  // Configuration d'authentification
  doc["auth_enabled"] = authCfg.enabled;

  // Options de développement
  doc["sensor_logs_enabled"] = authCfg.sensorLogsEnabled;
  doc["debug_logs_enabled"] = authCfg.debugLogsEnabled;

  // SÉCURITÉ: Masquer les credentials si non authentifié
  if (isAuthenticated) {
    // Utilisateur authentifié : montrer password et token masqués (indication qu'ils existent)
    doc["auth_password"] = authCfg.adminPassword.length() > 0 ? "******" : "";
    doc["auth_token"] = authCfg.apiToken.length() > 8 ? (authCfg.apiToken.substring(0, 8) + "...") : "";
  } else {
    // Utilisateur non authentifié : ne pas révéler si credentials sont configurés
    doc["auth_password"] = "******";
    doc["auth_token"] = "********...";
  }

  // Suivi volumes produits
  float phRemaining = max(0.0f, productCfg.phContainerVolumeMl - productCfg.phTotalInjectedMl);
  float orpRemaining = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  doc["ph_tracking_enabled"] = productCfg.phTrackingEnabled;
  doc["ph_container_ml"] = productCfg.phContainerVolumeMl;
  doc["ph_total_injected_ml"] = productCfg.phTotalInjectedMl;
  doc["ph_remaining_ml"] = phRemaining;
  doc["ph_alert_threshold_ml"] = productCfg.phAlertThresholdMl;
  doc["orp_tracking_enabled"] = productCfg.orpTrackingEnabled;
  doc["orp_container_ml"] = productCfg.orpContainerVolumeMl;
  doc["orp_total_injected_ml"] = productCfg.orpTotalInjectedMl;
  doc["orp_remaining_ml"] = orpRemaining;
  doc["orp_alert_threshold_ml"] = productCfg.orpAlertThresholdMl;

  // Heure actuelle (pour l'affichage dans la configuration)
  doc["time_current"] = getCurrentTimeISO();

  // feature-053 : Mode Boost (état effectif + epoch d'expiration, 0 si inactif).
  doc["boost_active"] = isBoostActive(time(nullptr));
  doc["boost_until"] = (long)boostState.untilEpoch;

  // feature-088 : valeurs d'usine des réglages de régulation — le dialogue de
  // confirmation avant/après de l'UI lit le FIRMWARE, jamais des constantes JS.
  // feature-090 : `regulation_defaults` reflète la colonne de la taille
  // COURANTE (contrat 088 intact : mêmes clés, à plat) ; le dialogue de
  // CHANGEMENT de taille lit lui `regulation_defaults_by_size`, qui porte les
  // trois colonnes — l'UI n'a ainsi jamais à deviner une colonne côté JS.
  fillRegulationDefaults(doc["regulation_defaults"].to<JsonObject>(), mqttCfg.poolSize);
  {
    JsonObject bySize = doc["regulation_defaults_by_size"].to<JsonObject>();
    fillRegulationDefaults(bySize["small"].to<JsonObject>(), PoolSize::Small);
    fillRegulationDefaults(bySize["medium"].to<JsonObject>(), PoolSize::Medium);
    fillRegulationDefaults(bySize["large"].to<JsonObject>(), PoolSize::Large);
  }

  sendJsonResponse(request, doc);
}

// Acquittement STRICT (v2.25.0) : seule la valeur JSON booléenne `true` compte.
// ArduinoJson convertit TOUTE chaîne en `true` via as<bool>() (VariantData.hpp,
// `default: return true`) — y compris "false" : un client maladroit acquitterait
// une sécurité chimique par accident. On exige donc le type booléen.
static bool isExplicitJsonTrue(JsonVariantConst v) {
  return v.is<bool>() && v.as<bool>();
}

// IP d'origine d'une requête, sans jamais déréférencer un client nul (revue).
// Le client peut avoir disparu entre l'arrivée du corps et la journalisation
// (fermeture navigateur, timeout TCP) : un log ne doit pas faire tomber l'appareil.
static String clientIpOf(AsyncWebServerRequest* request) {
  if (request == nullptr || request->client() == nullptr) return "IP inconnue";
  return request->client()->remoteIP().toString();
}

// Autorisation du CORPS de /save-config (v2.25.0, revue — bloquant n°1).
//
// TROU PRÉEXISTANT, sur TOUTE la route (pas seulement le pH) : REQUIRE_AUTH n'est
// posé que dans le lambda de COMPLÉTION, qui s'exécute APRÈS handleSaveConfig.
// Un POST non authentifié voyait donc son corps intégralement appliqué (mutations
// mqttCfg + saveMqttConfig() en NVS) avant que le 401 ne parte — le refus n'était
// qu'un affichage. Avec le gate d'acquittement pH, cela devenait un contournement
// direct d'une sécurité chimique ({"ph_auto_manual_ack":true,...} effaçait la trace
// et rouvrait la régulation).
//
// On reproduit ici la décision de AuthManager::checkAuth SANS ses effets de bord :
//   - pas de checkRateLimit() → le quota kMaxRequestsPerMinute reste compté UNE
//     fois par requête (dans la complétion), sinon chaque POST en aurait consommé 2 ;
//   - pas de sendAuthRequired() → c'est la complétion qui émet le 401 (une réponse
//     posée ici serait de toute façon remplacée par la sienne).
// L'exemption « premier démarrage » (wizard) est conservée à l'identique.
static bool saveConfigBodyAuthorized(AsyncWebServerRequest* request) {
  if (!authManager.isEnabled()) return true;            // authentification désactivée
  if (authManager.isFirstBootDetected()) return true;   // wizard de premier démarrage
  return authManager.checkTokenAuth(request) || authManager.checkBasicAuth(request);
}

static void handleSaveConfig(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  if (g_configBuffers == nullptr || g_configErrors == nullptr) {
    systemLogger.error("Config context non initialisé!");
    return;
  }

  // Corps non authentifié : on n'accumule RIEN et on n'applique RIEN. Sortie
  // AVANT toute écriture dans les maps — poser un drapeau ici créerait un nœud
  // que la complétion (qui rend la main sur REQUIRE_AUTH) n'effacerait jamais et
  // qu'aucun onDisconnect n'aurait encore été enregistré pour purger : fuite non
  // bornée, atteignable sans authentification. Le 401 est émis par la complétion.
  if (!saveConfigBodyAuthorized(request)) {
    if (index == 0) {
      systemLogger.warning("[Config] /save-config NON AUTHENTIFIÉ depuis " +
                           clientIpOf(request) + " — corps ignoré (aucune modification)");
    }
    return;
  }

  // Accumuler les données chunkées
  if (index == 0) {
    // Premier chunk, créer ou réinitialiser le buffer
    (*g_configBuffers)[request].clear();
    (*g_configErrors)[request] = false; // Pas d'erreur pour l'instant

    // Nettoyage si la connexion est abandonnée avant complétion (timeout, fermeture navigateur)
    // g_configErrorMessages DOIT figurer ici (revue) : REQUIRE_AUTH rend la main
    // AVANT la purge de la complétion, donc une requête non authentifiée portant
    // un ph_offset invalide laisserait sinon un nœud + String jamais libérés —
    // fuite non bornée et atteignable SANS authentification.
    request->onDisconnect([request]() {
      if (g_configBuffers) g_configBuffers->erase(request);
      if (g_configErrors) g_configErrors->erase(request);
      g_configErrorMessages.erase(request);
      g_configErrorStatus.erase(request);
    });

    // Vérifier la taille totale
    if (total > kMaxConfigSizeBytes) {
      systemLogger.error("Configuration trop volumineuse: " + String(total) + " bytes (max " + String(kMaxConfigSizeBytes) + ")");
      (*g_configErrors)[request] = true;
      return;
    }

    if (total > 0) {
      (*g_configBuffers)[request].reserve(total);
    }
  }

  // Ajouter les données au buffer
  if (len > 0) {
    (*g_configBuffers)[request].insert((*g_configBuffers)[request].end(), data, data + len);
  }

  // Si ce n'est pas le dernier chunk, ne rien faire
  if (index + len < total) {
    return;
  }

  // Dernier chunk reçu, traiter toutes les données accumulées
  std::vector<uint8_t>& buffer = (*g_configBuffers)[request];

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buffer.data(), buffer.size());

  if (error != DeserializationError::Ok) {
    systemLogger.error("Configuration JSON invalide reçue: " + String(error.c_str()));
    (*g_configErrors)[request] = true; // Marquer l'erreur
    return; // Le handler principal renverra 400
  }

  // Protéger l'accès concurrent aux configurations (web async vs loop)
  if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.error("Timeout acquisition configMutex dans handleSaveConfig");
    (*g_configErrors)[request] = true;
    return;
  }

  // ===========================================================================
  // v2.25.0 (B2) — GATE fail-closed : réarmement de la régulation pH
  // ===========================================================================
  // Tant qu'une trace de bascule AUTOMATIQUE existe (phAutoManualReason /
  // phAutoManualEpoch), la sortie de `manual` exige un acte DÉLIBÉRÉ :
  // `ph_auto_manual_ack: true` dans le même POST. Incident du 2026-08-25 : une
  // simple consultation de l'app a réémis `ph_regulation_mode: "automatic"`
  // (repli en dur du client sur un `window._config` périmé), rouvert la
  // régulation et effacé la trace dans la même seconde.
  //
  // CE BLOC DOIT RESTER LE PREMIER : il précède TOUTE mutation de mqttCfg. Un
  // refus survenant après des écritures partielles laisserait la config RAM
  // divergente de la NVS (aucun saveMqttConfig() n'est atteint sur ce chemin),
  // et l'écart ne serait visible qu'au prochain redémarrage.
  {
    const bool traceActive = (mqttCfg.phAutoManualReason.length() > 0) ||
                             (mqttCfg.phAutoManualEpoch != 0);
    // Champ absent ou non booléen → false : jamais d'acquittement implicite.
    const bool ack = isExplicitJsonTrue(doc["ph_auto_manual_ack"]);
    const bool curIsManual = (mqttCfg.phRegulationMode == "manual");
    // Mode DEMANDÉ : le mode courant si la clé est absente ou invalide (la
    // requête ne changerait alors rien — même verdict que le bloc d'application).
    String requestedMode = mqttCfg.phRegulationMode;
    if (!doc["ph_regulation_mode"].isNull()) {
      const String m = doc["ph_regulation_mode"].as<String>();
      if (m == "automatic" || m == "scheduled" || m == "manual") requestedMode = m;
    }
    if (phModeChangeNeedsAck(traceActive, curIsManual, requestedMode == "manual", ack)) {
      const String reason = mqttCfg.phAutoManualReason;  // copie AVANT le give
      xSemaphoreGiveRecursive(configMutex);
      // B5 : l'origine de la requête est journalisée sur les DEUX issues
      // (refus ici, acceptation plus bas) — sans elle, l'incident du 25/08 était
      // indiagnosticable (ni corps ni IP dans le journal).
      systemLogger.warning(
          "[Config] /save-config REFUSÉ (409) depuis " +
          clientIpOf(request) + " : passage manual → " +
          requestedMode + " sans acquittement de la bascule de sécurité pH. "
          "Trace active : " + reason);
      // Drapeau + message + code : c'est la complétion qui répond (cf. le
      // commentaire de g_configErrorMessages). Ne PAS répondre ni purger ici.
      (*g_configErrors)[request] = true;
      g_configErrorStatus[request] = 409;
      g_configErrorMessages[request] =
        "{\"error\":\"Bascule de s\\u00e9curit\\u00e9 pH non acquitt\\u00e9e\","
        "\"message\":\"Bascule de s\\u00e9curit\\u00e9 pH non acquitt\\u00e9e \\u2014 "
        "acquittez l'alerte sur la page pH avant de r\\u00e9activer la r\\u00e9gulation.\"}";
      return;
    }
  }

  // ===========================================================================
  // v2.25.0 — Bornes de la CONSIGNE pH (fenêtre chimique d'exploitation)
  // ===========================================================================
  // La consigne n'était validée NULLE PART sur ce chemin : un `ph_target` de 6,0
  // (ou une chaîne, convertie en 0 par ArduinoJson) était persisté tel quel et
  // pilotait ensuite la régulation. Bornes [kPhTargetMin ; kPhTargetMax] =
  // [7,20 ; 7,50], IDENTIQUES pour `ph_minus` et `ph_plus` (le type de produit
  // choisit la pompe, pas la fenêtre de pH de l'eau) — justification chimique
  // complète dans constants.h.
  //
  // `.as<float>()` est OBLIGATOIRE : sans lui, une chaîne JSON ("7.2") se
  // convertirait en 0 et passerait pour une consigne franchement corrosive.
  //
  // DÉFENSE EN PROFONDEUR : depuis la v2.25.0 l'interface web n'expose plus
  // qu'un contrôle segmenté (saisie hors plage impossible), mais l'API HTTP et
  // MQTT restent ouverts — cette validation est le seul garde-fou de ces chemins.
  //
  // REFUS explicite, JAMAIS d'écrêtage silencieux (patron ph_offset) : écrêter
  // ferait croire à l'utilisateur que sa consigne a été prise en compte alors
  // que la régulation viserait une autre valeur.
  //
  // CE BLOC PRÉCÈDE TOUTE MUTATION de mqttCfg, pour la même raison que le gate
  // ci-dessus : un refus après écritures partielles laisserait la config RAM
  // divergente de la NVS (aucun saveMqttConfig() n'est atteint sur ce chemin).
  if (!doc["ph_target"].isNull()) {
    const float requestedPhTarget = doc["ph_target"].as<float>();
    if (!isfinite(requestedPhTarget) ||
        requestedPhTarget < kPhTargetMin || requestedPhTarget > kPhTargetMax) {
      xSemaphoreGiveRecursive(configMutex);
      // Bornes formatées à la française (virgule décimale) à partir des
      // constantes : le message ne peut pas dériver des valeurs appliquées.
      String minStr(kPhTargetMin, 2); minStr.replace('.', ',');
      String maxStr(kPhTargetMax, 2); maxStr.replace('.', ',');
      systemLogger.warning("Consigne pH refusée (hors plage " + minStr + "-" + maxStr +
                           ") : " + String(requestedPhTarget, 2) + " depuis " +
                           clientIpOf(request));
      // Drapeau + message : c'est le lambda de complétion qui répondra (400 par
      // défaut, cf. le commentaire de g_configErrorMessages). Ne PAS répondre ici.
      (*g_configErrors)[request] = true;
      g_configErrorMessages[request] =
        String("{\"error\":\"Consigne pH hors plage autoris\\u00e9e\",\"message\":\""
               "Consigne pH hors plage autoris\\u00e9e (") + minStr +
        " \\u00e0 " + maxStr + "). "
        "En dessous, l'eau devient agressive pour le liner, les joints et les "
        "parties m\\u00e9talliques ; au-dessus, la r\\u00e9gulation du chlore "
        "serait inhib\\u00e9e en permanence.\"}";
      return;
    }
    // ALIGNEMENT SUR LE PAS (kPhTargetStep, v2.25.0). Dans la plage, une valeur
    // hors grille (7,35 envoyée par un script, ou héritée d'un firmware
    // antérieur) est chimiquement acceptable : on ne la REFUSE pas, on la cale
    // sur la position la plus proche. L'écart introduit est au pire un demi-pas
    // (0,05 pH), soit le bruit résiduel du filtre — sans effet sur la régulation,
    // alors qu'une consigne hors grille ne correspondrait à AUCUNE position du
    // contrôle de l'interface (état d'affichage ambigu).
    // La valeur alignée est réinjectée dans le document : le bloc d'application
    // plus bas reste l'unique point d'écriture de mqttCfg.phTarget.
    float snappedPhTarget = roundf(requestedPhTarget / kPhTargetStep) * kPhTargetStep;
    if (snappedPhTarget < kPhTargetMin) snappedPhTarget = kPhTargetMin;
    if (snappedPhTarget > kPhTargetMax) snappedPhTarget = kPhTargetMax;
    if (fabsf(snappedPhTarget - requestedPhTarget) > 0.001f) {
      systemLogger.info("Consigne pH alignée sur le pas de " + String(kPhTargetStep, 2) +
                        " : " + String(requestedPhTarget, 2) + " → " +
                        String(snappedPhTarget, 2));
    }
    doc["ph_target"] = snappedPhTarget;
  }

  // ===========================================================================
  // feature-090 — TAILLE DU BASSIN : présélection des défauts de régulation
  // ===========================================================================
  // INVARIANT ABSOLU : la taille ne sert qu'à appliquer la colonne de défauts
  // correspondante (regulation_defaults.h) — elle n'entre JAMAIS dans un calcul
  // de dosage, de débit ou de quota. Changement de taille (valeur valide ET
  // différente de la courante) → les 8 réglages de tuning reçoivent les défauts
  // de la NOUVELLE taille, par le MÊME chemin que la restauration 088 :
  //   - écritures de mqttCfg sous configMutex (déjà pris par ce handler) ;
  //   - persistance par le saveMqttConfig() de fin de handler (clé pool_size
  //     incluse) ;
  //   - retune PID UNIQUEMENT via le hook DIFFÉRÉ applyRegulationSpeed()
  //     (drapeau atomique consommé par update() dans loopTask — JAMAIS
  //     d'écriture de coefficients PID depuis la tâche web, invariant v2.26.0 ;
  //     le hash de contexte de la 088 purge l'intégrale si la vitesse change).
  // Ce bloc PRÉCÈDE le parse des clés individuelles : des clés de tuning
  // présentes dans le MÊME POST raffinent donc les défauts qui viennent d'être
  // appliqués (sémantique de fusion JSON). Valeur inconnue ou taille
  // inchangée : aucune modification (un vieux client qui n'envoie pas la clé
  // ne change rien).
  if (!doc["pool_size"].isNull()) {
    const String ps = doc["pool_size"].as<String>();
    if (ps == "small" || ps == "medium" || ps == "large") {
      const PoolSize newSize = poolSizeFromString(ps.c_str());
      if (newSize != mqttCfg.poolSize) {
        const RegulationDefaults& d = regulationDefaultsFor(newSize);
        // Instantané AVANT + niveau du journal — MÊME format et même patron
        // que restoreRegulationDefaults (condition 1 du visa pool-chemistry) :
        // WARNING si la colonne cible AUGMENTE la capacité de dosage (passage
        // à une taille supérieure : limites 5 → 10 → 15 min/h).
        const float oldPhTarget   = mqttCfg.phTarget;
        const float oldOrpTarget  = mqttCfg.orpTarget;
        const int   oldPhLimit    = mqttCfg.phInjectionLimitMinutes;
        const int   oldOrpLimit   = mqttCfg.orpInjectionLimitMinutes;
        // feature-091 : plus de « stab. » dans le récapitulatif — le délai de
        // stabilisation n'est plus un réglage (purge adaptative par stabilité).
        const int   oldPhDailyMl  = mqttCfg.phDailyTargetMl;
        const int   oldOrpDailyMl = mqttCfg.orpDailyTargetMl;
        char oldSpeed[8];  // "slow"/"normal"/"fast" — copie sans String
        strlcpy(oldSpeed, mqttCfg.regulationSpeed.c_str(), sizeof(oldSpeed));
        const char* oldSizeStr = poolSizeToString(mqttCfg.poolSize);
        const bool raisesDosing =
            regulationDefaultsRaiseDosing(d, oldOrpTarget, oldPhLimit, oldOrpLimit);

        mqttCfg.poolSize = newSize;
        applyRegulationDefaults(mqttCfg, newSize);
        PumpController.applyRegulationSpeed();  // hook DIFFÉRÉ (invariant 088)

        char msg[384];
        snprintf(msg, sizeof(msg),
                 "[Config] Taille de bassin %s → %s%s : pH %.2f→%.2f ; "
                 "ORP %.0f→%.0f mV ; limites pH %d→%d / ORP %d→%d min/h ; "
                 "prog. pH %d→%d / ORP %d→%d mL/j ; vitesse %s→%s",
                 oldSizeStr, poolSizeToString(newSize),
                 raisesDosing ? " (dosage AUGMENTÉ)" : "",
                 (double)oldPhTarget, (double)d.phTarget,
                 (double)oldOrpTarget, (double)d.orpTarget,
                 oldPhLimit, d.phInjectionLimitMin,
                 oldOrpLimit, d.orpInjectionLimitMin,
                 oldPhDailyMl, d.phDailyTargetMl,
                 oldOrpDailyMl, d.orpDailyTargetMl,
                 oldSpeed, d.regulationSpeed);
        systemLogger.log(raisesDosing ? LogLevel::WARNING : LogLevel::INFO, msg);
      }
    }
  }

  // Snapshot pour détecter si la config MQTT a réellement changé (feature-018).
  // Évite les reconnect/republish discovery inutiles sur les saves de paramètres
  // non-MQTT (modes régulation, NTP, logs, etc.).
  const String oldMqttServer   = mqttCfg.server;
  const int    oldMqttPort     = mqttCfg.port;
  const String oldMqttTopic    = mqttCfg.topic;
  const String oldMqttUsername = mqttCfg.username;
  const String oldMqttPassword = mqttCfg.password;
  const bool   oldMqttEnabled  = mqttCfg.enabled;

  // Validation et application avec logs
  if (!doc["server"].isNull()) mqttCfg.server = doc["server"].as<String>();
  if (!doc["port"].isNull()) mqttCfg.port = doc["port"];
  if (!doc["topic"].isNull()) mqttCfg.topic = doc["topic"].as<String>();
  if (!doc["username"].isNull()) mqttCfg.username = doc["username"].as<String>();

  // Mot de passe: ne mettre à jour que si différent de "******"
  if (!doc["password"].isNull()) {
    String receivedPassword = doc["password"].as<String>();
    if (receivedPassword != "******") {
      mqttCfg.password = receivedPassword;
    }
  }

  if (!doc["enabled"].isNull()) mqttCfg.enabled = doc["enabled"];
  // Valeur déjà VALIDÉE et ALIGNÉE sur kPhTargetStep plus haut : ce point n'est
  // atteint que si la consigne est dans la fenêtre autorisée.
  if (!doc["ph_target"].isNull()) mqttCfg.phTarget = doc["ph_target"].as<float>();
  if (!doc["orp_target"].isNull()) mqttCfg.orpTarget = doc["orp_target"];
  // v2.25.0 (B7) : `ph_enabled` n'est plus ACCEPTÉ en écriture. C'est un DÉRIVÉ
  // pur du mode (`mode != "manual"`, invariant projet rappelé dans
  // pump_controller.cpp) : le laisser écrivable ouvrait un second chemin capable
  // de rouvrir la régulation en contournant le gate ci-dessus, et de désynchroniser
  // le couple mode/enabled persisté en NVS. Le champ reste exposé en LECTURE
  // (/get-config, trame WS config, Home Assistant) — aucune rupture de contrat
  // pour les clients ; une clé envoyée par un vieux client est simplement ignorée
  // (même patron que screen_enabled, v2.17.1).
  //
  // v2.25.0 (B2) : acquittement DÉLIBÉRÉ de la trace de bascule automatique.
  // Sémantique : `ph_auto_manual_ack: true` SEUL efface la trace et LAISSE le
  // mode en manuel (la bannière acquitte, elle ne réarme pas). Associé à un
  // `ph_regulation_mode`, il autorise en plus la transition (gate ci-dessus).
  if (isExplicitJsonTrue(doc["ph_auto_manual_ack"])) {
    clearPhAutoManualTrace();  // no-op si aucune trace ; persisté en fin de handler
  }
  if (!doc["ph_regulation_mode"].isNull()) {
    String regMode = doc["ph_regulation_mode"].as<String>();
    if (regMode == "automatic" || regMode == "scheduled" || regMode == "manual") {
      // REVUE 2.20.3 (bloquant n°1) : n'acquitter QUE sur un CHANGEMENT réel.
      // L'interface renvoie systématiquement le mode courant dans le payload
      // (data/app.js, getConfigPayload) : tester la seule PRÉSENCE de la clé
      // faisait acquitter la bascule de sécurité à la moindre sauvegarde des
      // réglages, effaçant la trace NVS et la bannière alors que la régulation
      // reste en manuel — c'est-à-dire que le critère « la bascule est
      // persistée » tombait sans que personne ne le voie. Même patron que MQTT.
      const bool modeChanged = (regMode != mqttCfg.phRegulationMode);
      mqttCfg.phRegulationMode = regMode;
      mqttCfg.phEnabled = (regMode != "manual");
      if (modeChanged) {
        // Un HUMAIN reprend la main sur le mode → la trace de la dernière
        // bascule AUTOMATIQUE est acquittée (elle ne décrit plus l'état
        // courant). Persistée par le saveMqttConfig() de fin de handler.
        clearPhAutoManualTrace();
        // B5 (v2.25.0) : l'origine est journalisée sur les DEUX issues du gate.
        systemLogger.info("Mode régulation pH changé: " + regMode + " (depuis " +
                          clientIpOf(request) + ")");
      }
    }
  }
  if (!doc["ph_daily_target_ml"].isNull()) {
    int dailyMl = doc["ph_daily_target_ml"].as<int>();
    if (dailyMl < 0) dailyMl = 0;
    if (safetyLimits.maxPhMlPerDay > 0.0f &&
        dailyMl > static_cast<int>(safetyLimits.maxPhMlPerDay)) {
      xSemaphoreGiveRecursive(configMutex);
      request->send(400, "application/json",
        "{\"error\":\"ph_daily_target_ml d\\u00e9passe la limite journali\\u00e8re\"}");
      g_configBuffers->erase(request);
      g_configErrors->erase(request);
      return;
    }
    mqttCfg.phDailyTargetMl = dailyMl;
  }
  if (!doc["orp_enabled"].isNull()) mqttCfg.orpEnabled = doc["orp_enabled"];
  if (!doc["orp_regulation_mode"].isNull()) {
    String orpRegMode = doc["orp_regulation_mode"].as<String>();
    if (orpRegMode == "automatic" || orpRegMode == "scheduled" || orpRegMode == "manual") {
      mqttCfg.orpRegulationMode = orpRegMode;
      mqttCfg.orpEnabled = (orpRegMode != "manual");
      systemLogger.info("Mode régulation ORP changé: " + orpRegMode);
    }
  }
  if (!doc["orp_daily_target_ml"].isNull()) {
    int orpDailyMl = doc["orp_daily_target_ml"].as<int>();
    if (orpDailyMl < 0) orpDailyMl = 0;
    if (safetyLimits.maxChlorineMlPerDay > 0.0f &&
        orpDailyMl > static_cast<int>(safetyLimits.maxChlorineMlPerDay)) {
      xSemaphoreGiveRecursive(configMutex);
      request->send(400, "application/json",
        "{\"error\":\"orp_daily_target_ml d\\u00e9passe la limite journali\\u00e8re\"}");
      g_configBuffers->erase(request);
      g_configErrors->erase(request);
      return;
    }
    mqttCfg.orpDailyTargetMl = orpDailyMl;
  }
  if (!doc["ph_pump"].isNull()) mqttCfg.phPump = doc["ph_pump"];
  if (!doc["orp_pump"].isNull()) mqttCfg.orpPump = doc["orp_pump"];
  // Itération 2 (bug-dosage-fantome) : plancher kPumpAmpMinPct (40 %) — le duty
  // du plateau d'impulsion ne doit JAMAIS redescendre sous le seuil de rotation
  // (40 % × 255 ≈ 102 counts ≥ MIN_ACTIVE_DUTY = 98). Le min/max HTML ne suffit
  // pas : clamp serveur obligatoire (même patron que max_cycles_per_day).
  // feature-074 : l'amplitude est GELÉE côté UI (curseur retiré, le frontend
  // n'envoie plus ces clés) mais le champ API reste vivant + validé [40..100]
  // pour les cas exceptionnels (précédent screen_enabled : surface UI retirée,
  // contrat serveur conservé).
  if (!doc["pump1_max_duty_pct"].isNull()) mqttCfg.pump1MaxDutyPct = constrain((int)doc["pump1_max_duty_pct"], (int)kPumpAmpMinPct, 100);
  if (!doc["pump2_max_duty_pct"].isNull()) mqttCfg.pump2MaxDutyPct = constrain((int)doc["pump2_max_duty_pct"], (int)kPumpAmpMinPct, 100);
  // Itération 2 (bug-dosage-fantome) : pump_max_flow_ml_per_min SUPPRIMÉ — la
  // clé est IGNORÉE si un client l'envoie encore (rétrocompat, aucun champ cible).
  // À la place : caractérisation MESURÉE par pompe (pesée au banc). Validation
  // serveur SANS écrêtage silencieux (patron ph_offset) : écrêter une pesée
  // fausserait la comptabilité de volume injecté — refus HTTP 400 explicite.
  //   - pN_calibrated_duty ∈ {0} ∪ [102..255] (102 = kPumpAmpMinPct × 255 / 100,
  //     plancher de rotation) ; 0 = EFFACER la calibration : le débit associé
  //     est remis à 0 et la pompe redevient non caractérisée (dosage auto REFUSÉ,
  //     fail-closed — cf. pulse_dosing_logic.h) ;
  //   - pN_pulse_flow_ml_per_min ∈ ]0..kPulseFlowMaxMlPerMin] (plausibilité
  //     d'une doseuse de piscine, même garde qu'au chargement NVS).
  // feature-074 : le frontend n'envoie PLUS le duty — un débit valide reçu SANS
  // clé duty est une pesée à l'amplitude COURANTE : le serveur déduit
  // cal_duty = pumpNMaxDutyPct × MAX_PWM_DUTY / 100 (l'amplitude est traitée
  // plus haut dans ce handler, donc à jour) et pose le drapeau weighed. Un duty
  // explicite reste accepté (rétrocompat vieux client) et pose aussi weighed ;
  // l'effacement (duty=0) remet flow=0 ET weighed=false.
  {
    constexpr int kCalDutyMin = ((int)kPumpAmpMinPct * (int)MAX_PWM_DUTY) / 100;  // 102
    struct PumpCalKeys {
      const char* dutyKey;
      const char* flowKey;
      uint8_t* duty;
      float* flow;
      bool* weighed;
      const uint8_t* maxDutyPct;
      int num;
    };
    const PumpCalKeys pumps[2] = {
      {"pump1_calibrated_duty", "pump1_pulse_flow_ml_per_min",
       &mqttCfg.pump1CalibratedDuty, &mqttCfg.pump1PulseFlowMlPerMin,
       &mqttCfg.pump1FlowWeighed, &mqttCfg.pump1MaxDutyPct, 1},
      {"pump2_calibrated_duty", "pump2_pulse_flow_ml_per_min",
       &mqttCfg.pump2CalibratedDuty, &mqttCfg.pump2PulseFlowMlPerMin,
       &mqttCfg.pump2FlowWeighed, &mqttCfg.pump2MaxDutyPct, 2},
    };
    for (const PumpCalKeys& p : pumps) {
      // duty == 0 dans la même requête = intention d'EFFACER : le débit envoyé
      // conjointement (même à 0) est ignoré, l'effacement prime.
      const bool clearing = !doc[p.dutyKey].isNull() && doc[p.dutyKey].as<int>() == 0;
      bool flowChanged = false;
      bool flowSubmitted = false;
      if (!clearing && !doc[p.flowKey].isNull()) {
        float flow = doc[p.flowKey].as<float>();
        if (!isfinite(flow) || flow <= 0.0f || flow > kPulseFlowMaxMlPerMin) {
          xSemaphoreGiveRecursive(configMutex);
          systemLogger.warning("Pesée pompe " + String(p.num) +
                               " refusée : débit implausible " + String(flow, 1) + " ml/min");
          (*g_configErrors)[request] = true;
          g_configErrorMessages[request] =
            "{\"error\":\"D\\u00e9bit mesur\\u00e9 invalide : la valeur doit \\u00eatre "
            "sup\\u00e9rieure \\u00e0 0 et au plus 200 ml/min\"}";
          return;
        }
        if (flow != *p.flow) flowChanged = true;
        *p.flow = flow;
        flowSubmitted = true;
      }
      if (!doc[p.dutyKey].isNull()) {
        int duty = doc[p.dutyKey].as<int>();
        if (duty != 0 && (duty < kCalDutyMin || duty > (int)MAX_PWM_DUTY)) {
          xSemaphoreGiveRecursive(configMutex);
          systemLogger.warning("Pesée pompe " + String(p.num) +
                               " refusée : duty calibré hors plage " + String(duty));
          (*g_configErrors)[request] = true;
          g_configErrorMessages[request] =
            "{\"error\":\"Duty calibr\\u00e9 invalide : 0 (effacer la calibration) "
            "ou une valeur entre 102 et 255\"}";
          return;
        }
        if (duty == 0) {
          if (*p.duty != 0) {
            systemLogger.warning("Calibration pompe " + String(p.num) +
                                 " effacée — pompe non caractérisée (dosage auto refusé)");
          }
          *p.flow = 0.0f;   // effacement : le débit pesé n'a plus de sens
          *p.weighed = false;  // feature-074 : plus de pesée en vigueur
        } else if ((uint8_t)duty != *p.duty || flowChanged) {
          systemLogger.info("Pesée pompe " + String(p.num) + " enregistrée : duty " +
                            String(duty) + ", débit " + String(*p.flow, 1) + " ml/min");
        }
        *p.duty = (uint8_t)duty;
        if (duty != 0) *p.weighed = true;  // feature-074 : rétrocompat vieux client
      } else if (flowSubmitted) {
        // feature-074 : pesée « nouveau client » — le duty calibré est déduit
        // de l'amplitude d'exploitation courante. pumpNMaxDutyPct ∈ [40..100]
        // (clamp serveur plus haut) ⇒ duty déduit ∈ [102..255], toujours valide.
        const uint8_t deducedDuty =
            (uint8_t)(((int)*p.maxDutyPct * (int)MAX_PWM_DUTY) / 100);
        if (deducedDuty != *p.duty || flowChanged) {
          systemLogger.info("Pesée pompe " + String(p.num) + " enregistrée : duty " +
                            String(deducedDuty) + " (déduit de l'amplitude " +
                            String(*p.maxDutyPct) + " %), débit " +
                            String(*p.flow, 1) + " ml/min");
        }
        *p.duty = deducedDuty;
        *p.weighed = true;
      }
    }
  }
  if (!doc["ph_limit_minutes"].isNull()) mqttCfg.phInjectionLimitMinutes = constrain((int)doc["ph_limit_minutes"], 1, 60);
  if (!doc["orp_limit_minutes"].isNull()) mqttCfg.orpInjectionLimitMinutes = constrain((int)doc["orp_limit_minutes"], 1, 60);
  // feature-060 : garde 9 anti-cycling configurable. Clamp serveur OBLIGATOIRE
  // via clampCyclesPerDay (verdict pool-chemistry) — le min/max HTML ne suffit pas.
  if (!doc["max_cycles_per_day"].isNull()) {
    long requestedCycles = doc["max_cycles_per_day"].as<long>();
    bool cyclesClamped = false;
    unsigned int effectiveCycles = clampCyclesPerDay(requestedCycles, &cyclesClamped);
    if (cyclesClamped) {
      systemLogger.warning("max_cycles_per_day hors bornes : " + String(requestedCycles) +
                           " → " + String(effectiveCycles));
    }
    if (effectiveCycles != pumpProtection.maxCyclesPerDay) {
      systemLogger.warning("Limite cycles/jour modifiée : " + String(pumpProtection.maxCyclesPerDay) +
                           " → " + String(effectiveCycles));
      pumpProtection.maxCyclesPerDay = effectiveCycles;
    }
  }
  // feature-056 : mode d'installation (remplace regulation_mode + filtration_enabled).
  if (!doc["install_mode"].isNull()) {
    String im = doc["install_mode"].as<String>();
    if (im == "managed" || im == "powered" || im == "external") {
      mqttCfg.installMode = installModeFromString(im.c_str(), mqttCfg.installMode);
    }
  }
  // feature-091 : la clé stabilization_delay_min est ACCEPTÉE SILENCIEUSEMENT
  // (un client ancien / le wizard peut encore l'envoyer) mais n'a plus aucun
  // effet — le délai de purge est piloté par la stabilité des mesures.
  if (!doc["regulation_speed"].isNull()) {
    String speed = doc["regulation_speed"].as<String>();
    if (speed == "slow" || speed == "normal" || speed == "fast") {
      mqttCfg.regulationSpeed = speed;
      PumpController.applyRegulationSpeed();
    }
  }
  if (!doc["ph_correction_type"].isNull()) {
    String corrType = doc["ph_correction_type"].as<String>();
    if (corrType == "ph_minus" || corrType == "ph_plus") {
      if (corrType != mqttCfg.phCorrectionType) {
        mqttCfg.phCorrectionType = corrType;
        PumpController.resetPhPauseGuard();
      }
    }
  }
  if (!doc["time_use_ntp"].isNull()) mqttCfg.timeUseNtp = doc["time_use_ntp"];
  if (!doc["ntp_server"].isNull()) mqttCfg.ntpServer = doc["ntp_server"].as<String>();
  if (!doc["manual_time"].isNull()) {
    String newManualTime = doc["manual_time"].as<String>();
    // Si l'heure manuelle a changé et n'est pas vide, l'appliquer au système et au RTC
    if (newManualTime.length() > 0 && newManualTime != mqttCfg.manualTimeIso) {
      mqttCfg.manualTimeIso = newManualTime;

      // Parser l'ISO 8601 (format: "YYYY-MM-DDTHH:MM:SS" ou "YYYY-MM-DD HH:MM:SS")
      int year, month, day, hour, minute, second;
      if (sscanf(newManualTime.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6 ||
          sscanf(newManualTime.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {

        // Créer un struct tm et convertir en epoch
        struct tm timeinfo = {};
        timeinfo.tm_year = year - 1900;
        timeinfo.tm_mon = month - 1;
        timeinfo.tm_mday = day;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = second;
        time_t epoch = mktime(&timeinfo);

        if (epoch > 0) {
          // Appliquer au système
          struct timeval tv;
          tv.tv_sec = epoch;
          tv.tv_usec = 0;
          settimeofday(&tv, nullptr);

          // Mettre à jour le RTC
          if (rtcManager.isAvailable()) {
            rtcManager.setTimeFromEpoch(epoch);
            systemLogger.info("Heure manuelle appliquée au système et au RTC: " + newManualTime);
          } else {
            systemLogger.info("Heure manuelle appliquée au système: " + newManualTime);
          }
        }
      }
    } else {
      mqttCfg.manualTimeIso = newManualTime;
    }
  }
  if (!doc["timezone_id"].isNull()) mqttCfg.timezoneId = doc["timezone_id"].as<String>();
  bool filtrationConfigChanged = false;
  // feature-056 : filtration_enabled retiré (absorbé par install_mode).
  String prevFiltrationMode = filtrationCfg.mode;
  bool scheduleChanged = false;
  if (!doc["filtration_mode"].isNull()) { filtrationCfg.mode = doc["filtration_mode"].as<String>(); scheduleChanged = true; filtrationConfigChanged = true; }
  if (!doc["filtration_start"].isNull()) { filtrationCfg.start = doc["filtration_start"].as<String>(); scheduleChanged = true; filtrationConfigChanged = true; }
  if (!doc["filtration_end"].isNull()) { filtrationCfg.end = doc["filtration_end"].as<String>(); scheduleChanged = true; filtrationConfigChanged = true; }
  // Quand le mode ou les horaires changent, effacer les overrides manuels pour que le planning prenne effet immédiatement
  if (scheduleChanged && doc["filtration_force_on"].isNull() && doc["filtration_force_off"].isNull()) {
    filtrationCfg.forceOn = false;
    filtrationCfg.forceOff = false;
  }
  if (!doc["filtration_force_on"].isNull()) {
    filtrationCfg.forceOn = doc["filtration_force_on"].as<bool>();
    if (filtrationCfg.forceOn) filtrationCfg.forceOff = false;
    filtrationConfigChanged = true;
  }
  if (!doc["filtration_force_off"].isNull()) {
    filtrationCfg.forceOff = doc["filtration_force_off"].as<bool>();
    if (filtrationCfg.forceOff) filtrationCfg.forceOn = false;
    filtrationConfigChanged = true;
  }
  if (!doc["max_ph_ml_per_day"].isNull()) safetyLimits.maxPhMlPerDay = doc["max_ph_ml_per_day"];
  if (!doc["max_chlorine_ml_per_day"].isNull()) safetyLimits.maxChlorineMlPerDay = doc["max_chlorine_ml_per_day"];
  if (!doc["lighting_feature_enabled"].isNull()) lightingCfg.featureEnabled = doc["lighting_feature_enabled"];
  if (!doc["lighting_enabled"].isNull()) lightingCfg.enabled = doc["lighting_enabled"];
  if (!doc["lighting_brightness"].isNull()) lightingCfg.brightness = doc["lighting_brightness"];
  if (!doc["lighting_schedule_enabled"].isNull()) {
    lightingCfg.scheduleEnabled = doc["lighting_schedule_enabled"];
    // Quand la programmation est activée, annuler le contrôle manuel pour que le planning prenne effet
    if (lightingCfg.scheduleEnabled) lighting.clearManualOverride();
  }
  if (!doc["lighting_start_time"].isNull()) lightingCfg.startTime = doc["lighting_start_time"].as<String>();
  if (!doc["lighting_end_time"].isNull()) lightingCfg.endTime = doc["lighting_end_time"].as<String>();

  // feature-021 : la calibration ORP est désormais stockée dans le module Atlas EZO
  // (commande Cal,<ref>). Plus de champs orp_calibration_* à persister côté ESP32.
  // L'UI utilise POST /calibrate_orp + WS phCalPoints/orpCalPoints pour le suivi.

  // feature-067 : offset logiciel de mesure pH (pH corrigé = pH brut EZO + offset).
  // Validation SERVEUR obligatoire, refus HTTP 400 SANS écrêtage silencieux : un
  // offset erroné décale l'entrée de la régulation et peut conduire à surdoser
  // l'acide. Champ absent ou non numérique → paramètre inchangé (patron existant).
  // La prise en compte est immédiate via sensors.refreshPhOffsetCache().
  // INTERDIT ici : requestReset() / resetDosingStates() / resetPhPauseGuard() —
  // ils remettraient à zéro cyclesToday ou lastStopTime, ce qui permettrait de
  // contourner les limites journalières et la pause anti-cycling par simples
  // enregistrements répétés. L'offset ne touche AUCUN compteur de dosage.
  if (doc["ph_offset"].is<float>()) {
    float requestedPhOffset = doc["ph_offset"].as<float>();
    if (!isPhOffsetValid(requestedPhOffset)) {
      xSemaphoreGiveRecursive(configMutex);
      // Plage citée à partir des constantes (jamais de littéral dans le texte) :
      // le message ne peut pas dériver des bornes réellement appliquées.
      String oMinStr(kPhOffsetMin, 2); oMinStr.replace('.', ',');
      String oMaxStr(kPhOffsetMax, 2); oMaxStr.replace('.', ',');
      systemLogger.warning("Offset pH refusé (hors plage " + oMinStr + " à +" + oMaxStr +
                           ") : " + String(requestedPhOffset, 2) + " depuis " +
                           clientIpOf(request));
      // Drapeau + message : c'est le lambda de complétion qui répondra (voir le
      // commentaire de g_configErrorMessages). Ne PAS répondre ni purger ici.
      (*g_configErrors)[request] = true;
      g_configErrorMessages[request] =
        String("{\"error\":\"Offset pH hors plage autoris\\u00e9e\",\"message\":\""
               "Offset pH hors plage autoris\\u00e9e (") + oMinStr + " \\u00e0 +" + oMaxStr +
        " pH). Au-del\\u00e0, c'est la sonde ou sa calibration EZO qui est en cause, "
        "pas son implantation.\"}";
      return;
    }
    // v2.25.0 (B2) — Alignement sur kPhOffsetStep par la MÊME fonction que la
    // migration NVS (roundPhOffsetToStep, ph_guard_logic.h) : source unique, donc
    // aucune divergence possible entre une valeur reçue par l'API et une valeur
    // relue au démarrage. L'écart maximal introduit est un demi-pas (0,05 pH),
    // sous le bruit résiduel du filtre. Ce n'est PAS un écrêtage silencieux : la
    // valeur hors plage a déjà été refusée ci-dessus.
    const float snappedPhOffset = roundPhOffsetToStep(requestedPhOffset);
    if (fabsf(snappedPhOffset - requestedPhOffset) > 0.001f) {
      systemLogger.info("Offset pH aligné sur le pas de " + String(kPhOffsetStep, 2) +
                        " : " + String(requestedPhOffset, 2) + " → " +
                        String(snappedPhOffset, 2));
    }
    mqttCfg.phOffset = snappedPhOffset;
    sensors.refreshPhOffsetCache();  // journalise ancien/nouveau si changement
  }

  // Données de calibration Température
  if (!doc["temp_calibration_offset"].isNull()) {
    mqttCfg.tempCalibrationOffset = doc["temp_calibration_offset"];
  }
  if (!doc["temp_calibration_date"].isNull()) {
    mqttCfg.tempCalibrationDate = doc["temp_calibration_date"].as<String>();
  }
  if (!doc["temperature_enabled"].isNull()) {
    mqttCfg.temperatureEnabled = doc["temperature_enabled"];
  }

  // Configuration d'authentification
  if (!doc["auth_enabled"].isNull()) {
    authCfg.enabled = doc["auth_enabled"];
    authManager.setEnabled(authCfg.enabled);
  }

  // Mot de passe admin: ne mettre à jour que si différent de "******"
  if (!doc["auth_password"].isNull()) {
    String receivedPassword = doc["auth_password"].as<String>();
    if (receivedPassword != "******" && receivedPassword.length() > 0) {
      authCfg.adminPassword = receivedPassword;
      authManager.setPassword(authCfg.adminPassword);
      systemLogger.info("Mot de passe administrateur modifié");
    }
  }

  // Options de développement
  if (!doc["sensor_logs_enabled"].isNull()) {
    authCfg.sensorLogsEnabled = doc["sensor_logs_enabled"];
    systemLogger.info(String("Logs des sondes: ") + (authCfg.sensorLogsEnabled ? "activés" : "désactivés"));
  }
  if (!doc["debug_logs_enabled"].isNull()) {
    authCfg.debugLogsEnabled = doc["debug_logs_enabled"];
    systemLogger.info(String("Logs DEBUG: ") + (authCfg.debugLogsEnabled ? "activés" : "désactivés"));
  }
  // refactor-retrait-toggle-ecran (v2.17.1) : champ screen_enabled retiré du
  // contrat /get-config//set-config — un client legacy qui l'envoie encore est
  // simplement ignoré (aucune erreur).

  // Note: Le token API n'est pas modifiable via /save-config
  // Il doit être regénéré via une route dédiée pour éviter les modifications accidentelles

  sanitizePumpSelection();
  filtration.ensureTimesValid();
  lighting.ensureTimesValid();
  ensureTimezoneValid();
  applyTimezoneEnv();
  applyTimeConfig();  // Appliquer la config NTP si activée

  // Calculer le planning auto uniquement si le mode VIENT DE PASSER à "auto"
  // (transition depuis un autre mode). Si le mode était déjà "auto", on ne
  // recalcule pas pour ne pas écraser les horaires que l'utilisateur vient
  // d'enregistrer. Le recalcul périodique se fait dans filtration.update().
  bool modeChangedToAuto = filtrationCfg.mode.equalsIgnoreCase("auto") &&
                           !prevFiltrationMode.equalsIgnoreCase("auto");
  if (modeChangedToAuto) {
    filtration.computeAutoSchedule();
  }

  // Appliquer immédiatement la nouvelle configuration de filtration
  // (seulement si un paramètre de filtration a changé, pour éviter de déclencher
  // involontairement un démarrage de filtration et une temporisation de stabilisation
  // lors d'une sauvegarde sans lien avec la filtration, ex: changement de type pH)
  if (filtrationConfigChanged) {
    filtration.update();
  }

  // Suivi volumes produits
  if (!doc["ph_tracking_enabled"].isNull()) {
    productCfg.phTrackingEnabled = doc["ph_tracking_enabled"].as<bool>();
    productConfigDirty = true;
  }
  if (!doc["orp_tracking_enabled"].isNull()) {
    productCfg.orpTrackingEnabled = doc["orp_tracking_enabled"].as<bool>();
    productConfigDirty = true;
  }
  if (!doc["ph_container_ml"].isNull()) {
    productCfg.phContainerVolumeMl = max(0.0f, doc["ph_container_ml"].as<float>());
    productConfigDirty = true;
  }
  if (!doc["ph_alert_threshold_ml"].isNull()) {
    productCfg.phAlertThresholdMl = max(0.0f, doc["ph_alert_threshold_ml"].as<float>());
    productConfigDirty = true;
  }
  if (doc["ph_reset_container"] == true) {
    productCfg.phTotalInjectedMl = 0.0f;
    productConfigDirty = true;
    systemLogger.info("Bidon pH réinitialisé");
  }
  if (!doc["orp_container_ml"].isNull()) {
    productCfg.orpContainerVolumeMl = max(0.0f, doc["orp_container_ml"].as<float>());
    productConfigDirty = true;
  }
  if (!doc["orp_alert_threshold_ml"].isNull()) {
    productCfg.orpAlertThresholdMl = max(0.0f, doc["orp_alert_threshold_ml"].as<float>());
    productConfigDirty = true;
  }
  if (doc["orp_reset_container"] == true) {
    productCfg.orpTotalInjectedMl = 0.0f;
    productConfigDirty = true;
    systemLogger.info("Bidon chlore réinitialisé");
  }
  if (productConfigDirty) {
    saveProductConfig();
  }

  saveMqttConfig();

  // Reconnect MQTT uniquement si un champ MQTT a réellement changé (feature-018).
  // Évite les disconnect/reconnect et la republication des messages discovery
  // pour les saves de paramètres non-MQTT.
  bool mqttChanged = (mqttCfg.server   != oldMqttServer)   ||
                     (mqttCfg.port     != oldMqttPort)     ||
                     (mqttCfg.topic    != oldMqttTopic)    ||
                     (mqttCfg.username != oldMqttUsername) ||
                     (mqttCfg.password != oldMqttPassword) ||
                     (mqttCfg.enabled  != oldMqttEnabled);
  if (mqttChanged) {
    systemLogger.info("MQTT reconnect demandé (config MQTT modifiée)");
    mqttManager.requestReconnect();
  }

  // feature-021 : pH/ORP calibrés en interne par les modules EZO ; seul l'offset
  // température utilisateur est appliqué côté firmware (alias eau dans
  // SensorManager::_readDs18b20s()). Pas besoin de recalcul global ici.

  // Libérer le mutex
  xSemaphoreGiveRecursive(configMutex);

  // v2.25.0 (refonte seuils fixes dérivés) : la garde de cohérence
  // consigne / offset / plancher est SUPPRIMÉE. Les seuils ne dépendent plus de
  // l'offset et la marge consigne ↔ seuil bas vaut kPhSafetyMargin (0,50 pH) par
  // construction, verrouillée par static_assert : il n'y a plus d'impasse
  // arithmétique possible à détecter au moment de la sauvegarde.

  systemLogger.info("Configuration mise à jour via interface web");
  // La réponse sera envoyée par le handler principal
}

static void handleTimeNow(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Buffer statique : 3 champs (time, time_use_ntp, timezone_id) = 128 bytes
  StaticJson<128> doc;
  doc["time"] = getCurrentTimeISO();
  doc["time_use_ntp"] = mqttCfg.timeUseNtp;
  doc["timezone_id"] = mqttCfg.timezoneId;

  sendJsonResponse(request, doc);
}

static void handleRebootAp(AsyncWebServerRequest* request);  // Forward declaration

static void handleGetSystemInfo(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // 30 champs (version, chip, mémoire, WiFi, uptime). Charge utile mesurée avec
  // measureJson() sur des valeurs pires cas (compteurs 32 bits saturés, SSID de
  // 32 caractères) : ~748 octets, contre 716 avant `oom_handler_registered`
  // (v2.20.2) et 628 avant les trois champs mémoire v2.19.0.
  // PRÉCISION (revue v2.19.0) : en ArduinoJson 7, StaticJson<N> n'est plus qu'un
  // JsonDocument à capacité ÉLASTIQUE — le N ne borne rien et aucune troncature
  // silencieuse n'est possible. Le chiffre ci-dessus documente donc la taille de
  // la RÉPONSE (utile pour le budget réseau et la garde heap en amont), pas un
  // remplissage de tampon. Ne pas le lire comme « 716 sur 1024 ».
  StaticJson<1024> doc;

  // Version firmware
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["build_date"] = FIRMWARE_BUILD_DATE;
  doc["build_time"] = FIRMWARE_BUILD_TIME;

  // Informations ESP32
  doc["chip_model"] = ESP.getChipModel();
  doc["chip_revision"] = ESP.getChipRevision();
  doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
  doc["free_heap"] = ESP.getFreeHeap();

  // v2.19.0 (bug-metrique-heap-inclut-iram-32bits) : `free_heap` ci-dessus vaut
  // heap_caps_get_free_size(MALLOC_CAP_INTERNAL) — pool qui englobe l'IRAM
  // accessible en 32 bits SEULEMENT, donc inutilisable par un malloc()/new
  // ordinaire. On expose EN PLUS la métrique 8 bits, seule représentative de la
  // mémoire réellement allouable, ainsi que le plus gros bloc des deux familles :
  // l'écart entre `max_alloc_8bit` et `max_alloc_internal` chiffre le piège.
  // Ajout PUREMENT ADDITIF : `free_heap` et `heap_size` gardent leur sémantique
  // d'origine (aucune rupture pour un client existant).
  const HeapMetrics heap = heapMetricsSnapshot();
  doc["free_heap_8bit"] = heap.free8;
  doc["max_alloc_8bit"] = heap.maxAlloc8;
  doc["max_alloc_internal"] = heap.maxAllocInternal;

  // v2.20.2 (bug-famine-8bits-parachute-mort-et-gardes-aveugles) : le
  // new_handler du parachute anti-bad_alloc se DÉSINSCRIT lui-même à la 2ᵉ
  // famine, et sa ré-inscription se trouvait derrière un `return` — l'appareil
  // de production tournait donc SANS filet ~2 h après le boot, sans que rien ne
  // le signale (le journal, lui, s'écrasait en 25-35 min). Ce booléen est le
  // témoin permanent, vérifiable on-target à tout instant et indépendant des
  // logs. Lecture d'un miroir atomique : ni allocation, ni log, ni parcours de
  // tas — sûr dans un handler AsyncWebServer. Ajout PUREMENT ADDITIF.
  doc["oom_handler_registered"] = oomGuardHandlerRegistered();

  // feature-066 : PSRAM (0 = absente, module WROOM — l'UI affiche « absente »)
  doc["psram_size"] = ESP.getPsramSize();
  doc["psram_free"] = ESP.getFreePsram();
  doc["heap_size"] = ESP.getHeapSize();
  doc["flash_size"] = ESP.getFlashChipSize();
  doc["flash_speed"] = ESP.getFlashChipSpeed();

  // Partition OTA
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    doc["ota_partition"] = running->label;
    doc["ota_partition_size"] = running->size;
    doc["sketch_size"] = ESP.getSketchSize();
  }

  // Système de fichiers
  doc["fs_total_bytes"] = LittleFS.totalBytes();
  doc["fs_used_bytes"] = LittleFS.usedBytes();
  doc["fs_free_bytes"] = LittleFS.totalBytes() - LittleFS.usedBytes();

  // WiFi
  doc["wifi_ssid"] = WiFi.SSID();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["wifi_ip"] = WiFi.localIP().toString();
  doc["wifi_mac"] = WiFi.macAddress();

  // Uptime
  unsigned long uptime = millis() / kMillisToSeconds;
  doc["uptime_seconds"] = uptime;
  doc["uptime_days"] = uptime / 86400;
  doc["uptime_hours"] = (uptime % 86400) / kSecondsPerHour;
  doc["uptime_minutes"] = (uptime % kSecondsPerHour) / kSecondsPerMinute;

  sendJsonResponse(request, doc);
}

void setupConfigRoutes(AsyncWebServer* server, bool* restartApRequested, unsigned long* restartRequestedTime) {
  server->on("/get-config", HTTP_GET, handleGetConfig);
  server->on("/time-now", HTTP_GET, handleTimeNow);
  server->on("/get-system-info", HTTP_GET, handleGetSystemInfo);

  // Routes Wi-Fi publiques (utilisées depuis la page de login en mode AP)
  server->on("/wifi/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    wifi_mode_t mode = WiFi.getMode();
    String modeLabel = (mode == WIFI_MODE_AP) ? "AP" : (mode == WIFI_MODE_APSTA ? "AP+STA" : "STA");
    doc["mode"] = modeLabel;
    doc["connected"] = WiFi.isConnected();
    doc["ssid"] = WiFi.SSID();
    doc["ap_ssid"] = WiFi.softAPSSID();
    doc["ap_ip"] = WiFi.softAPIP().toString();
    sendJsonResponse(request, doc);
  });

  server->on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Permettre le scan si:
    // 1. wifiConfigAllowed() == true (mode AP ou WiFi non connecté) OU
    // 2. L'utilisateur est authentifié (accès depuis les paramètres)
    bool allowed = wifiConfigAllowed();
    if (!allowed) {
      // Vérifier l'authentification pour les utilisateurs connectés en WiFi
      if (!authManager.checkAuth(request, RouteProtection::WRITE)) {
        return;
      }
    }

    if (!authManager.checkRateLimit(request)) {
      authManager.sendRateLimitExceeded(request);
      return;
    }

    int count = WiFi.scanNetworks(false, true);
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
      JsonObject item = networks.add<JsonObject>();
      item["ssid"] = WiFi.SSID(i);
      item["rssi"] = WiFi.RSSI(i);
      item["channel"] = WiFi.channel(i);
      item["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    sendJsonResponse(request, doc);
  });

  auto* wifiConnectHandler = new AsyncCallbackJsonWebHandler(
    "/wifi/connect",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      // Permettre la configuration WiFi si:
      // 1. wifiConfigAllowed() == true (mode AP ou WiFi non connecté) OU
      // 2. L'utilisateur est authentifié (accès depuis les paramètres)
      bool allowed = wifiConfigAllowed();
      if (!allowed) {
        // Vérifier l'authentification pour les utilisateurs connectés en WiFi
        if (!authManager.checkAuth(request, RouteProtection::WRITE)) {
          return;
        }
      }
      if (!authManager.checkRateLimit(request)) {
        authManager.sendRateLimitExceeded(request);
        return;
      }

      JsonObject root = json.as<JsonObject>();
      String ssid = root["ssid"] | "";
      String password = root["password"] | "";

      if (ssid.isEmpty()) {
        sendErrorResponse(request, 400, "SSID required");
        return;
      }

      // Enregistrer les credentials pour reconnexion asynchrone
      systemLogger.info("Configuration WiFi demandée depuis l'UI: " + ssid);
      g_wifiReconnectSsid = ssid;
      g_wifiReconnectPassword = password;
      g_wifiReconnectRequested = true;
      g_wifiReconnectRequestedTime = millis();

      // Retourner immédiatement une réponse (la connexion se fera de manière asynchrone)
      JsonDocument doc;
      doc["accepted"] = true;
      doc["message"] = "WiFi connection request accepted, connecting asynchronously";
      sendJsonResponse(request, doc);
    });
  wifiConnectHandler->setMethod(HTTP_POST);
  server->addHandler(wifiConnectHandler);

  // Route: Déconnecter du WiFi et effacer les credentials
  server->on("/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest* request) {
    // Permettre la déconnexion WiFi si:
    // 1. wifiConfigAllowed() == true (mode AP ou WiFi non connecté) OU
    // 2. L'utilisateur est authentifié (accès depuis les paramètres)
    bool allowed = wifiConfigAllowed();
    if (!allowed) {
      // Vérifier l'authentification pour les utilisateurs connectés en WiFi
      if (!authManager.checkAuth(request, RouteProtection::CRITICAL)) {
        return;
      }
    }

    systemLogger.info("Déconnexion WiFi demandée depuis l'UI");

    // Déconnecter et effacer les credentials WiFi
    WiFi.disconnect(true, true);  // disconnect(wifioff=true, eraseap=true)
    delay(100);

    // Repasser en mode AP uniquement
    WiFi.mode(WIFI_MODE_AP);

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "WiFi disconnected and credentials erased";
    sendJsonResponse(request, doc);
  });

  server->on("/wifi/ap/disable", HTTP_POST, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::CRITICAL);

    // Vérifier qu'il y a des credentials WiFi configurés dans la NVS
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    String ssid = String((char*)wifi_config.sta.ssid);

    if (ssid.length() == 0) {
      sendErrorResponse(request, 400, "No WiFi credentials configured");
      return;
    }

    // Activer le flag pour désactiver l'AP au prochain démarrage
    authCfg.disableApOnBoot = true;
    authCfg.forceWifiConfig = false;
    saveMqttConfig();

    if (WiFi.isConnected()) {
      systemLogger.info("Flag disableApOnBoot activé - WiFi connecté - Redémarrage programmé");
    } else {
      systemLogger.info("Flag disableApOnBoot activé - WiFi configuré mais pas encore connecté - Redémarrage programmé");
    }

    // Envoyer la réponse avant le redémarrage
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "ESP32 redémarrage en mode STA uniquement";
    doc["restarting"] = true;
    sendJsonResponse(request, doc);

    // Redémarrer après un court délai pour permettre à la réponse d'être envoyée
    delay(500);
    saveDailyCounters();  // v2.27.0 : ne pas perdre le dénominateur de charge pH
    mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline avant restart
    ESP.restart();
  });

  // Route /save-config - PROTÉGÉE (CRITICAL)
  server->on("/save-config", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      REQUIRE_AUTH(req, RouteProtection::CRITICAL);

      if (g_configErrors == nullptr || g_configBuffers == nullptr) {
        g_configErrorMessages.erase(req);  // pas de nœud orphelin sur ce chemin
        g_configErrorStatus.erase(req);
        req->send(500, "text/plain", "Config context error");
        return;
      }

      // Vérifier si une erreur s'est produite pendant le traitement
      bool hasError = (*g_configErrors)[req];

      // Message spécifique éventuel (feature-067) : relevé AVANT la purge.
      String errorBody;
      auto itMsg = g_configErrorMessages.find(req);
      if (itMsg != g_configErrorMessages.end()) errorBody = itMsg->second;

      // Code de refus (v2.25.0) : 400 par défaut, 409 pour le gate pH.
      int errorStatus = 400;
      auto itStatus = g_configErrorStatus.find(req);
      if (itStatus != g_configErrorStatus.end()) errorStatus = itStatus->second;

      // Nettoyer les buffers et états
      g_configBuffers->erase(req);
      g_configErrors->erase(req);
      g_configErrorMessages.erase(req);
      g_configErrorStatus.erase(req);

      if (hasError) {
        if (errorBody.length() > 0) {
          req->send(errorStatus, "application/json", errorBody);
        } else {
          req->send(400, "text/plain", "Invalid JSON configuration");
        }
      } else {
        req->send(200, "text/plain", "OK");
        // Exécuté en tâche AsyncTCP (~8 KB stack). _buildConfigJson() alloue
        // un StaticJson<2048> sur la pile : on diffère le broadcast à la main
        // loop pour éviter le PANIC stack overflow.
        wsManager.requestConfigBroadcast();
      }
    },
    nullptr,
    handleSaveConfig
  );

  // Route /reset-regulation-defaults — PROTÉGÉE (CRITICAL, le niveau de
  // /save-config : même pouvoir d'écriture sur la config de régulation).
  // feature-088 : restaure les 8 réglages de TUNING aux valeurs d'usine
  // (regulation_defaults.h) — depuis la feature-090, celles de la TAILLE DE
  // BASSIN COURANTE (mqttCfg.poolSize). SANS corps, donc AUCUN parsing JSON : aucune
  // valeur ne transite par le client, elle ne peut pas diverger de la source.
  // Ne touche JAMAIS : modes de régulation, ph/orpEnabled, phCorrectionType,
  // phOffset, trace phAutoManual*, calibration/sélection des pompes,
  // SafetyLimits (périmètre arbitré par pool-chemistry).
  server->on("/reset-regulation-defaults", HTTP_POST, [](AsyncWebServerRequest* req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    // Origine journalisée (leçon de l'incident du 25/08 : sans IP, une écriture
    // de config est indiagnosticable) ; le détail avant → après champ par champ
    // est journalisé par restoreRegulationDefaults().
    systemLogger.info("Reset régulation: " + clientIpOf(req));
    // Revue v2.31.0 : sur timeout du mutex, RIEN n'a été modifié — répondre
    // une ERREUR franche (503) et NE PAS retuner ni broadcaster. Un
    // success:true mensonger sur une route CRITICAL de régulation ferait
    // afficher « restauré » à l'UI pour une restauration jamais appliquée.
    if (!restoreRegulationDefaults()) {  // écrit sous configMutex + saveMqttConfig()
      req->send(503, "application/json",
                "{\"success\":false,\"error\":\"Configuration occupée, réessayez\"}");
      return;
    }

    // Retune PID par le MÊME hook DIFFÉRÉ que /save-config (regulation_speed) :
    // applyRegulationSpeed() ne fait que poser le drapeau atomique
    // _phTuningChangeRequested, consommé par update() dans loopTask — AUCUNE
    // écriture de phPID/_phIntegral depuis ce handler (invariant v2.26.0).
    PumpController.applyRegulationSpeed();

    // Réponse : les valeurs appliquées, depuis la source unique (mêmes clés
    // que l'objet regulation_defaults de /get-config). feature-090 : la
    // colonne est celle de la TAILLE COURANTE — contrat JSON inchangé.
    StaticJson<512> doc;
    doc["success"] = true;
    fillRegulationDefaults(doc["regulation_defaults"].to<JsonObject>(), mqttCfg.poolSize);
    sendJsonResponse(req, doc);

    // Broadcast WS config DIFFÉRÉ à la main loop (patron /save-config) : les
    // autres clients voient les nouvelles valeurs sans risquer le stack
    // overflow de _buildConfigJson() en tâche AsyncTCP.
    wsManager.requestConfigBroadcast();
  });

  // Route reboot - PROTÉGÉE (CRITICAL) - Redémarrage normal
  server->on("/reboot", HTTP_POST, [restartApRequested, restartRequestedTime](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    *restartApRequested = true;
    *restartRequestedTime = millis();
    req->send(200, "text/plain", "Restart scheduled");
    systemLogger.warning("Redémarrage demandé depuis l'interface web");
  });

  // Route reboot-ap - PROTÉGÉE (CRITICAL)
  // Efface les credentials WiFi puis redémarre pour forcer le mode AP
  server->on("/reboot-ap", HTTP_POST, [restartApRequested, restartRequestedTime](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    systemLogger.warning("Redémarrage en mode AP demandé");

    // Effacer les credentials WiFi AVANT de planifier le redémarrage
    resetWiFiSettings();

    *restartApRequested = true;
    *restartRequestedTime = millis();
    req->send(200, "text/plain", "WiFi reset - AP mode will start after restart");
  });

  // Route factory-reset - PROTÉGÉE (CRITICAL)
  // Efface toutes les données NVS et redémarre (retour à l'assistant de configuration)
  server->on("/factory-reset", HTTP_POST, [restartApRequested, restartRequestedTime](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    systemLogger.critical("Réinitialisation usine demandée depuis l'interface web");

    // Effacer TOUTE la partition NVS (WiFi, config, calibrations, produits, etc.)
    resetWiFiSettings();

    *restartApRequested = true;
    *restartRequestedTime = millis();
    req->send(200, "application/json", "{\"status\":\"ok\"}");
  });
}

// Fonction pour traiter les reconnexions WiFi asynchrones
// À appeler régulièrement depuis loop() pour ne pas bloquer le serveur web
void processWifiReconnectIfNeeded() {
  // Si aucune reconnexion n'est demandée, rien à faire
  if (!g_wifiReconnectRequested) {
    return;
  }

  // Attendre un petit délai pour que la réponse HTTP soit bien envoyée
  if (millis() - g_wifiReconnectRequestedTime < 100) {
    return;
  }

  // Marquer comme traité pour éviter de refaire plusieurs fois
  g_wifiReconnectRequested = false;

  systemLogger.info("Démarrage reconnexion WiFi asynchrone: " + g_wifiReconnectSsid);

  // Déterminer le mode WiFi actuel
  wifi_mode_t mode = WiFi.getMode();
  wifi_mode_t initialMode = mode; // Sauvegarder le mode initial pour y revenir en cas d'échec

  unsigned long startTime = millis();
  unsigned long timeout = 15000; // Timeout de 15 secondes pour la connexion initiale

  // Gestion des modes WiFi selon le contexte :
  // - Mode AP uniquement : passer en APSTA pour garder l'AP actif pendant la connexion (wizard/première config)
  // - Mode STA : si déjà connecté, déconnecter d'abord pour forcer la sauvegarde NVS
  // - Mode APSTA : si déjà connecté, passer en STA (sortir du mode secours), sinon conserver APSTA (connexion en cours)
  if (mode == WIFI_MODE_AP) {
    systemLogger.info("Mode AP détecté, passage temporaire en APSTA pour garder l'AP actif");
    WiFi.mode(WIFI_AP_STA);
  } else if (mode == WIFI_MODE_STA) {
    // En mode STA, si on est déjà connecté, il faut déconnecter pour forcer la sauvegarde NVS
    if (WiFi.isConnected()) {
      systemLogger.info("Mode STA avec connexion active, déconnexion pour forcer la sauvegarde NVS");
      WiFi.disconnect(false, false); // Déconnecter sans éteindre le WiFi ni effacer la config NVS

      // Attendre la déconnexion effective
      startTime = millis();
      while (WiFi.isConnected() && (millis() - startTime) < timeout) {
        delay(100);
      }
      delay(200); // Laisser le temps à la déconnexion de se stabiliser
    } else {
      systemLogger.info("Mode STA sans connexion, prêt pour nouvelle connexion");
    }
  } else if (mode == WIFI_MODE_APSTA) {
    // Si on est déjà connecté en APSTA (mode secours après échecs), passer en STA pour la nouvelle connexion
    // Si on n'est pas encore connecté, c'est qu'on est en train de se connecter (wizard), garder APSTA
    if (WiFi.isConnected()) {
      systemLogger.info("Mode APSTA détecté avec connexion active (mode secours), passage en STA pour nouvelle connexion");
      WiFi.disconnect(false, false); // Déconnecter sans effacer la config

      // Attendre la déconnexion effective (WiFi.disconnect n'est pas synchrone)
      startTime = millis();
      while (WiFi.isConnected() && (millis() - startTime) < timeout) {
        delay(100);
      }

      WiFi.mode(WIFI_MODE_STA);
      delay(200); // Laisser le temps au changement de mode de s'appliquer
    } else {
      systemLogger.info("Mode APSTA détecté sans connexion (configuration initiale), on conserve APSTA");
      // Ne rien faire, garder le mode APSTA pendant la connexion initiale
    }
  }

  // Lancer la connexion WiFi (en mode STA ou APSTA selon le cas)
  systemLogger.info("=== DEBUG Reconnexion WiFi ===");
  systemLogger.info("SSID: '" + g_wifiReconnectSsid + "' (longueur: " + String(g_wifiReconnectSsid.length()) + ")");
  systemLogger.info("Password: (longueur: " + String(g_wifiReconnectPassword.length()) + ")");
  systemLogger.info("==============================");

  // Sauvegarder les anciens credentials AVANT de tenter la connexion
  // Car WiFi.begin() peut corrompre la NVS même avec persistent(false)
  wifi_config_t old_wifi_config;
  memset(&old_wifi_config, 0, sizeof(wifi_config_t));
  esp_wifi_get_config(WIFI_IF_STA, &old_wifi_config);
  String oldSsid = String((char*)old_wifi_config.sta.ssid);
  systemLogger.info("Anciens credentials sauvegardés: SSID='" + oldSsid + "'");

  // Démarrer la connexion WiFi SANS persistence automatique
  // On sauvegarde manuellement dans la NVS seulement si la connexion réussit
  systemLogger.info("Tentative de connexion WiFi (sauvegarde NVS uniquement si succès)...");

  // Désactiver la persistence automatique pour éviter d'écraser les anciens credentials
  WiFi.persistent(false);

  // Tenter la connexion au nouveau réseau
  WiFi.begin(g_wifiReconnectSsid.c_str(), g_wifiReconnectPassword.c_str());

  startTime = millis();

  while (!WiFi.isConnected() && (millis() - startTime) < timeout) {
    delay(100);
  }

  if (WiFi.isConnected()) {
    systemLogger.info("Connexion WiFi réussie! IP: " + WiFi.localIP().toString());

    // Sauvegarder les nouveaux credentials dans la NVS
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    strncpy((char*)wifi_config.sta.ssid, g_wifiReconnectSsid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, g_wifiReconnectPassword.c_str(), sizeof(wifi_config.sta.password) - 1);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err == ESP_OK) {
      systemLogger.info("Credentials WiFi sauvegardés dans la NVS");
    } else {
      systemLogger.error("Erreur sauvegarde NVS: " + String(esp_err_to_name(err)));
    }
  } else {
    systemLogger.warning("Échec connexion WiFi - restauration des anciens credentials dans la NVS");

    // Restaurer les anciens credentials dans la NVS
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &old_wifi_config);
    if (err == ESP_OK) {
      systemLogger.info("Anciens credentials restaurés: SSID='" + oldSsid + "'");
    } else {
      systemLogger.error("Erreur restauration NVS: " + String(esp_err_to_name(err)));
    }

    // Si on était en mode AP au départ, revenir en mode AP (pas APSTA)
    if (initialMode == WIFI_MODE_AP) {
      systemLogger.info("Retour au mode AP après échec de connexion");
      WiFi.mode(WIFI_MODE_AP);
      delay(200);
    }
  }

  // Effacer les credentials pour la sécurité
  g_wifiReconnectSsid = "";
  g_wifiReconnectPassword = "";
}
