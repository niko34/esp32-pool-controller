#include "config.h"
#include "constants.h"
#include "logger.h"
#include "ph_offset.h"  // feature-067 : isPhOffsetValid() (logique pure)
#include "ph_guard_logic.h"  // v2.25.0 : roundPhOffsetToStep() (logique pure)
#include <Preferences.h>
#include <time.h>

// Définition des variables globales
MqttConfig mqttCfg;
FiltrationConfig filtrationCfg;
LightingConfig lightingCfg;
AuthConfig authCfg;
// bug-dosage-fantome itération 2 : le modèle linéaire duty↔débit est retiré
// (kPumpMinFlowMlPerMin / kPumpMaxFlowMlPerMin supprimés). Ces params ne
// servent plus qu'à l'ESTIMATION de l'injection manuelle volumée
// (web_routes_control, refonte prévue par l'agent web) : minFlow 0 (plus de
// clamp vers le haut), maxFlow = débit MESURÉ à 255 de la pompe par défaut.
PumpControlParams phPumpControl  = {0.0f, kPump1Flow255MlPerMin, kPhMaxError};
PumpControlParams orpPumpControl = {0.0f, kPump2Flow255MlPerMin, kOrpMaxError};
SafetyLimits safetyLimits;
PumpProtection pumpProtection;
ProductConfig productCfg;
bool productConfigDirty = false;
BoostState boostState;

// Mutex pour protection concurrence
SemaphoreHandle_t configMutex = nullptr;
SemaphoreHandle_t i2cMutex = nullptr;

void initConfigMutexes() {
  configMutex = xSemaphoreCreateRecursiveMutex();  // Récursif : saveConfig() peut être appelé dans une section déjà sous mutex
  i2cMutex = xSemaphoreCreateMutex();

  if (configMutex == nullptr || i2cMutex == nullptr) {
    systemLogger.critical("Échec création mutex!");
  } else {
    systemLogger.info("Mutex de concurrence initialisés");
  }
}

// feature-056 : sérialisation STABLE du mode d'installation.
const char* installModeToString(InstallMode mode) {
  switch (mode) {
    case InstallMode::PoweredByFiltration: return "powered";
    case InstallMode::ExternalFiltration:  return "external";
    case InstallMode::ManagedFiltration:
    default:                                return "managed";
  }
}

InstallMode installModeFromString(const char* s, InstallMode fallback) {
  if (s == nullptr) return fallback;
  if (strcmp(s, "managed") == 0)  return InstallMode::ManagedFiltration;
  if (strcmp(s, "powered") == 0)  return InstallMode::PoweredByFiltration;
  if (strcmp(s, "external") == 0) return InstallMode::ExternalFiltration;
  return fallback;
}

const TimezoneInfo* findTimezoneById(const String& id) {
  for (const auto& tz : TIMEZONES) {
    if (id.equalsIgnoreCase(tz.id)) {
      return &tz;
    }
  }
  return nullptr;
}

const TimezoneInfo* defaultTimezone() {
  return &TIMEZONES[0];
}

void ensureTimezoneValid() {
  if (!findTimezoneById(mqttCfg.timezoneId)) {
    mqttCfg.timezoneId = defaultTimezone()->id;
  }
}

const TimezoneInfo* currentTimezone() {
  ensureTimezoneValid();
  const TimezoneInfo* tz = findTimezoneById(mqttCfg.timezoneId);
  return tz ? tz : defaultTimezone();
}

void applyTimezoneEnv() {
  const TimezoneInfo* tz = currentTimezone();
  setenv("TZ", tz->posix, 1);
  tzset();
}

int pumpIndexFromNumber(int pumpNumber) {
  return (pumpNumber == 2) ? 1 : 0;
}

int sanitizePumpNumber(int pumpNumber, int defaultValue) {
  return (pumpNumber == 1 || pumpNumber == 2) ? pumpNumber : defaultValue;
}

void sanitizePumpSelection() {
  mqttCfg.phPump = sanitizePumpNumber(mqttCfg.phPump, 1);
  mqttCfg.orpPump = sanitizePumpNumber(mqttCfg.orpPump, 2);
}

// =============================================================================
// v2.20.3 (C4) — trace de la bascule AUTOMATIQUE de la régulation pH en manuel
// =============================================================================
// Ces deux helpers ne persistent RIEN par eux-mêmes : ils mutent la config en
// RAM, l'appelant enchaîne sur saveMqttConfig() (déjà présent sur les deux
// chemins concernés). Cela évite une écriture NVS supplémentaire et garantit
// que la trace et le mode partent dans la MÊME transaction.
void setPhAutoManualTrace(const String& reason) {
  const time_t now = time(nullptr);
  // Heure non synchronisée au moment de la bascule : on garde la raison (qui
  // porte l'essentiel du diagnostic) et on laisse l'epoch à 0 → l'UI affiche
  // « date inconnue » plutôt qu'une date de 1970 trompeuse.
  mqttCfg.phAutoManualEpoch = (now >= kMinValidEpoch) ? (uint32_t)now : 0;
  mqttCfg.phAutoManualReason = reason;
}

void clearPhAutoManualTrace() {
  if (mqttCfg.phAutoManualEpoch == 0 && mqttCfg.phAutoManualReason.length() == 0) return;
  systemLogger.info("[Config] Bascule automatique pH → manuel acquittée par "
                    "l'utilisateur (trace effacée) : " + mqttCfg.phAutoManualReason);
  mqttCfg.phAutoManualEpoch = 0;
  mqttCfg.phAutoManualReason = "";
}

void saveMqttConfig() {
  // feature-027 : prise bornée (miroir de saveDailyCounters). Timeout → config RAM
  // appliquée mais persistance NVS perdue — signalé en error (throttlé 1/min).
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sLastErrMs = 0;
    unsigned long nowMs = millis();
    if (sLastErrMs == 0 || nowMs - sLastErrMs >= kMutexTimeoutWarnThrottleMs) {
      systemLogger.error("[Config] saveMqttConfig: mutex timeout — config non persistée (RAM appliquée)");
      sLastErrMs = nowMs;
    }
    return;
  }
  Preferences prefs;
  if (!prefs.begin("poolctrl", false)) {
    systemLogger.error("Échec ouverture NVS pour sauvegarde");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
    return;
  }

  // MQTT
  prefs.putString("mqtt_server", mqttCfg.server);
  prefs.putInt("mqtt_port", mqttCfg.port);
  prefs.putString("mqtt_topic", mqttCfg.topic);
  prefs.putString("mqtt_user", mqttCfg.username);
  prefs.putString("mqtt_pass", mqttCfg.password);
  prefs.putBool("mqtt_enabled", mqttCfg.enabled);

  // Régulation pH
  prefs.putFloat("ph_target", mqttCfg.phTarget);
  prefs.putBool("ph_enabled", mqttCfg.phEnabled);
  prefs.putInt("ph_pump", mqttCfg.phPump);
  prefs.putInt("ph_limit_min", mqttCfg.phInjectionLimitMinutes);
  prefs.putString("ph_reg_mode", mqttCfg.phRegulationMode);
  prefs.putInt("ph_daily_ml", mqttCfg.phDailyTargetMl);
  // v2.20.3 (C4) : trace persistée de la bascule AUTOMATIQUE pH → manual.
  // Clés ≤ 15 caractères (contrainte NVS). Absentes sur une config existante →
  // relues à 0 / "" (aucune bascule enregistrée) : rétrocompatible.
  prefs.putULong("ph_am_epoch", mqttCfg.phAutoManualEpoch);
  prefs.putString("ph_am_reason", mqttCfg.phAutoManualReason);

  // feature-021 (Pass 4a) : calibration pH/ORP entièrement gérée par les modules
  // Atlas EZO (NVS interne au module). Aucune clé NVS pH/ORP côté ESP32.

  // Régulation ORP
  prefs.putFloat("orp_target", mqttCfg.orpTarget);
  prefs.putBool("orp_enabled", mqttCfg.orpEnabled);
  prefs.putInt("orp_pump", mqttCfg.orpPump);
  prefs.putInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
  prefs.putString("orp_reg_mode", mqttCfg.orpRegulationMode);
  prefs.putInt("orp_daily_ml", mqttCfg.orpDailyTargetMl);
  // feature-056 : mode d'installation (remplace reg_mode + filt_enabled).
  prefs.putString("install_mode", installModeToString(mqttCfg.installMode));
  prefs.putInt("stab_delay", mqttCfg.stabilizationDelayMin);
  prefs.putString("reg_speed", mqttCfg.regulationSpeed);
  prefs.putString("ph_corr_type", mqttCfg.phCorrectionType);

  // Calibration Température
  prefs.putFloat("temp_cal_off", mqttCfg.tempCalibrationOffset);
  prefs.putString("temp_cal_date", mqttCfg.tempCalibrationDate);
  prefs.putBool("temp_enabled", mqttCfg.temperatureEnabled);

  // feature-067 : offset logiciel de mesure pH (clé ≤ 15 caractères)
  prefs.putFloat("ph_offset", mqttCfg.phOffset);

  // Temps
  prefs.putBool("time_use_ntp", mqttCfg.timeUseNtp);
  prefs.putString("ntp_server", mqttCfg.ntpServer);
  prefs.putString("manual_time", mqttCfg.manualTimeIso);
  prefs.putString("timezone_id", mqttCfg.timezoneId);

  // Filtration (feature-056 : filt_enabled absorbé par install_mode)
  prefs.putString("filt_mode", filtrationCfg.mode);
  prefs.putString("filt_start", filtrationCfg.start);
  prefs.putString("filt_end", filtrationCfg.end);


  // Éclairage
  prefs.putBool("light_feat_en", lightingCfg.featureEnabled);
  prefs.putBool("light_enabled", lightingCfg.enabled);
  prefs.putUChar("light_bright", lightingCfg.brightness);
  prefs.putBool("light_sched_en", lightingCfg.scheduleEnabled);
  prefs.putString("light_start", lightingCfg.startTime);
  prefs.putString("light_end", lightingCfg.endTime);

  // Authentification
  prefs.putBool("auth_enabled", authCfg.enabled);
  prefs.putBool("auth_force_cfg", authCfg.forceWifiConfig);
  prefs.putBool("wizard_done", authCfg.wizardCompleted);
  prefs.putBool("disable_ap_boot", authCfg.disableApOnBoot);
  prefs.putString("auth_password", authCfg.adminPassword);
  prefs.putString("auth_token", authCfg.apiToken);
  prefs.putString("auth_ap_pwd", authCfg.apPassword);
  prefs.putBool("sensor_logs", authCfg.sensorLogsEnabled);
  prefs.putBool("debug_logs", authCfg.debugLogsEnabled);
  // v2.17.1 : la clé NVS "screen_enabled" n'est plus écrite ni lue (support
  // écran retiré) — la clé résiduelle des appareils existants est inoffensive.

  // Amplitude des pompes (itération 2 : % → duty du plateau d'impulsion)
  prefs.putUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  prefs.putUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  // Caractérisation mesurée (proportionnel de temps, option B+C). Clés ≤ 15 car.
  prefs.putUChar("p1_cal_duty", mqttCfg.pump1CalibratedDuty);
  prefs.putFloat("p1_pulse_flow", mqttCfg.pump1PulseFlowMlPerMin);
  prefs.putUChar("p2_cal_duty", mqttCfg.pump2CalibratedDuty);
  prefs.putFloat("p2_pulse_flow", mqttCfg.pump2PulseFlowMlPerMin);
  // feature-074 : drapeaux « pesée individuelle » (false = valeur usine de type)
  prefs.putBool("p1_flow_weighed", mqttCfg.pump1FlowWeighed);
  prefs.putBool("p2_flow_weighed", mqttCfg.pump2FlowWeighed);
  // Purge de l'ancienne clé du modèle linéaire (pump_max_flow) à la première
  // sauvegarde : AUCUNE conversion — c'est le modèle faux qu'on retire.
  prefs.remove("pump_max_flow");

  // Limites de sécurité
  prefs.putFloat("max_ph_ml", safetyLimits.maxPhMlPerDay);
  prefs.putFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);
  prefs.putUInt("max_cycles_day", pumpProtection.maxCyclesPerDay);  // feature-060 : garde 9 configurable

  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
  systemLogger.info("Configuration sauvegardée dans NVS");
}

void loadMqttConfig() {
  Preferences prefs;
  if (!prefs.begin("poolctrl", true)) {  // true = read-only
    systemLogger.warning("NVS vide, création avec valeurs par défaut");
    saveMqttConfig();
    return;
  }

  // MQTT
  mqttCfg.server = prefs.getString("mqtt_server", mqttCfg.server);
  mqttCfg.port = prefs.getInt("mqtt_port", mqttCfg.port);
  mqttCfg.topic = prefs.getString("mqtt_topic", mqttCfg.topic);
  mqttCfg.username = prefs.getString("mqtt_user", "");
  mqttCfg.password = prefs.getString("mqtt_pass", "");
  mqttCfg.enabled = prefs.getBool("mqtt_enabled", mqttCfg.enabled);

  // Régulation pH
  mqttCfg.phTarget = prefs.getFloat("ph_target", mqttCfg.phTarget);
  // v2.25.0 — bornes de consigne pH appliquées AU CHARGEMENT (défense en
  // profondeur : c'est ici qu'on détecte une NVS corrompue ou écrite par un
  // firmware antérieur, le curseur de l'UI ne pouvant plus produire de valeur
  // hors plage). Une installation existante peut porter en NVS une consigne hors
  // [kPhTargetMin, kPhTargetMax] — firmware antérieur, ancienne commande MQTT qui
  // clampait à 6,0-8,5, consigne héritée à 7,60 abaissée à 7,50 par cette
  // refonte. On la CLAMPE et on la RÉÉCRIT immédiatement : une consigne trop
  // basse conduit l'eau vers la zone corrosive, une consigne trop haute inhibe la
  // régulation ORP en permanence (kPhOrpInhibit). Réécriture NVS tout de suite
  // pour que l'anomalie ne resurgisse pas au prochain boot si aucune sauvegarde
  // n'intervient d'ici là.
  if (!isfinite(mqttCfg.phTarget) ||
      mqttCfg.phTarget < kPhTargetMin || mqttCfg.phTarget > kPhTargetMax) {
    const float rejected = mqttCfg.phTarget;
    // Valeur non finie : on ne peut rien clamper de sensé → borne basse, qui est
    // aussi la valeur par défaut de la structure.
    mqttCfg.phTarget = isfinite(rejected)
                           ? fminf(fmaxf(rejected, kPhTargetMin), kPhTargetMax)
                           : kPhTargetMin;
    const String clampMsg = "[Config] Consigne pH NVS hors plage (" +
                            String(rejected, 2) + ") — ramenée à " +
                            String(mqttCfg.phTarget, 2) + " (plage autorisée " +
                            String(kPhTargetMin, 2) + " - " + String(kPhTargetMax, 2) +
                            "). Vérifier le réglage de consigne pH.";
    // Valeur finie hors plage = réglage hérité (typiquement 7,60 → 7,50) : c'est
    // une migration attendue, pas un défaut. Valeur non finie = NVS réellement
    // corrompue : là, on hurle.
    if (isfinite(rejected)) {
      systemLogger.warning(clampMsg);
    } else {
      systemLogger.critical(clampMsg);
    }
    prefs.end();
    Preferences w;
    if (w.begin("poolctrl", false)) {
      w.putFloat("ph_target", mqttCfg.phTarget);
      w.end();
    } else {
      systemLogger.critical("[Config] Réécriture NVS de la consigne pH impossible "
                            "— clamp appliqué en RAM seulement (l'anomalie NVS "
                            "reviendra au prochain boot)");
    }
    // Réouverture en lecture : son échec est SILENCIEUX par nature — tout le
    // reste de loadMqttConfig() retomberait alors sur les défauts de structure
    // (modes de régulation, limites de dosage, caractérisation des pompes…) sans
    // que rien ne le signale. On continue malgré tout, car sortir ici sauterait
    // sanitizePumpSelection() / ensureTimezoneValid() / applyTimezoneEnv() pour
    // le MÊME jeu de défauts, en moins cohérent — mais on le dit fort.
    if (!prefs.begin("poolctrl", true)) {
      systemLogger.critical("[Config] Réouverture NVS en lecture ÉCHOUÉE après le "
                            "clamp de la consigne pH — le reste de la configuration "
                            "(régulation, limites de dosage, pompes) repart des "
                            "VALEURS PAR DÉFAUT pour ce boot. Vérifier la NVS et "
                            "redémarrer avant toute régulation automatique.");
    }
  }
  mqttCfg.phEnabled = prefs.getBool("ph_enabled", mqttCfg.phEnabled);
  mqttCfg.phPump = prefs.getInt("ph_pump", mqttCfg.phPump);
  // Migration NVS : ph_limit_sec (secondes) → ph_limit_min (minutes)
  if (prefs.isKey("ph_limit_sec")) {
    int oldSec = prefs.getInt("ph_limit_sec", 300);
    mqttCfg.phInjectionLimitMinutes = max(1, oldSec / 60);
    systemLogger.info("Migration NVS ph_limit_sec=" + String(oldSec) + "s → ph_limit_min=" + String(mqttCfg.phInjectionLimitMinutes) + "min");
    prefs.end();
    if (prefs.begin("poolctrl", false)) {
      prefs.putInt("ph_limit_min", mqttCfg.phInjectionLimitMinutes);
      prefs.remove("ph_limit_sec");
      prefs.end();
    }
    prefs.begin("poolctrl", true);
  } else {
    mqttCfg.phInjectionLimitMinutes = prefs.getInt("ph_limit_min", mqttCfg.phInjectionLimitMinutes);
  }

  // Migration ph_enabled → ph_regulation_mode
  if (prefs.isKey("ph_reg_mode")) {
    mqttCfg.phRegulationMode = prefs.getString("ph_reg_mode", "automatic");
  } else {
    mqttCfg.phRegulationMode = mqttCfg.phEnabled ? "automatic" : "manual";
    systemLogger.info("Migration ph_enabled → ph_regulation_mode: " + mqttCfg.phRegulationMode);
  }
  mqttCfg.phDailyTargetMl = prefs.getInt("ph_daily_ml", 0);
  if (mqttCfg.phRegulationMode != "automatic" &&
      mqttCfg.phRegulationMode != "scheduled" &&
      mqttCfg.phRegulationMode != "manual") {
    mqttCfg.phRegulationMode = "automatic";
  }
  mqttCfg.phEnabled = (mqttCfg.phRegulationMode != "manual");

  // v2.20.3 (C4) : trace de la bascule automatique pH → manual. Clés absentes
  // (config antérieure à 2.20.3) → 0 / "" = aucune bascule enregistrée.
  // Cohérence défensive : la trace n'a de sens QUE si le mode est réellement
  // `manual` — sinon on la purge en RAM (un write-back aurait lieu au prochain
  // saveMqttConfig, inutile de forcer une écriture NVS au boot).
  mqttCfg.phAutoManualEpoch  = prefs.getULong("ph_am_epoch", 0);
  mqttCfg.phAutoManualReason = prefs.getString("ph_am_reason", "");
  if (mqttCfg.phRegulationMode != "manual") {
    mqttCfg.phAutoManualEpoch = 0;
    mqttCfg.phAutoManualReason = "";
  } else if (mqttCfg.phAutoManualEpoch != 0 || mqttCfg.phAutoManualReason.length() > 0) {
    systemLogger.critical("[Config] Régulation pH en MANUEL suite à une bascule "
                          "automatique non acquittée : " + mqttCfg.phAutoManualReason);
  }

  // feature-021 (Pass 4a) : purge silencieuse des anciennes clés de calibration
  // pH/ORP. La calibration est désormais entièrement stockée dans les modules
  // Atlas EZO (commandes Cal,? / Cal,mid / Cal,low / Cal,<ref>).
  bool needPurge =
      prefs.isKey("ph_cal_date") || prefs.isKey("ph_cal_temp") ||
      prefs.isKey("orp_cal_off") || prefs.isKey("orp_cal_slope") ||
      prefs.isKey("orp_cal_date") || prefs.isKey("orp_cal_ref") ||
      prefs.isKey("orp_cal_temp");
  if (needPurge) {
    prefs.end();
    Preferences cleanup;
    if (cleanup.begin("poolctrl", false /*RW*/)) {
      cleanup.remove("ph_cal_date");
      cleanup.remove("ph_cal_temp");
      cleanup.remove("orp_cal_off");
      cleanup.remove("orp_cal_slope");
      cleanup.remove("orp_cal_date");
      cleanup.remove("orp_cal_ref");
      cleanup.remove("orp_cal_temp");
      cleanup.end();
      systemLogger.info("Migration NVS feature-021 : clés ph_cal_*/orp_cal_* purgées (calibration EZO interne)");
    }
    prefs.begin("poolctrl", true);
  }

  // Régulation ORP
  mqttCfg.orpTarget = prefs.getFloat("orp_target", mqttCfg.orpTarget);
  mqttCfg.orpEnabled = prefs.getBool("orp_enabled", mqttCfg.orpEnabled);
  mqttCfg.orpPump = prefs.getInt("orp_pump", mqttCfg.orpPump);
  // Migration NVS : orp_limit_sec (secondes) → orp_limit_min (minutes)
  if (prefs.isKey("orp_limit_sec")) {
    int oldSec = prefs.getInt("orp_limit_sec", 600);
    mqttCfg.orpInjectionLimitMinutes = max(1, oldSec / 60);
    systemLogger.info("Migration NVS orp_limit_sec=" + String(oldSec) + "s → orp_limit_min=" + String(mqttCfg.orpInjectionLimitMinutes) + "min");
    prefs.end();
    if (prefs.begin("poolctrl", false)) {
      prefs.putInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
      prefs.remove("orp_limit_sec");
      prefs.end();
    }
    prefs.begin("poolctrl", true);
  } else {
    mqttCfg.orpInjectionLimitMinutes = prefs.getInt("orp_limit_min", mqttCfg.orpInjectionLimitMinutes);
  }
  // Migration orp_enabled → orp_regulation_mode
  if (prefs.isKey("orp_reg_mode")) {
    mqttCfg.orpRegulationMode = prefs.getString("orp_reg_mode", "automatic");
  } else {
    mqttCfg.orpRegulationMode = mqttCfg.orpEnabled ? "automatic" : "manual";
    systemLogger.info("Migration orp_enabled → orp_regulation_mode: " + mqttCfg.orpRegulationMode);
  }
  mqttCfg.orpDailyTargetMl = prefs.getInt("orp_daily_ml", 0);
  if (mqttCfg.orpRegulationMode != "automatic" &&
      mqttCfg.orpRegulationMode != "scheduled" &&
      mqttCfg.orpRegulationMode != "manual") {
    mqttCfg.orpRegulationMode = "automatic";
  }
  mqttCfg.orpEnabled = (mqttCfg.orpRegulationMode != "manual");

  // feature-056 : mode d'installation. Clé "install_mode" présente → parse ;
  // sinon migration one-shot depuis l'ancien schéma (reg_mode + filt_enabled)
  // puis write-back (anciennes clés laissées inertes en NVS).
  if (prefs.isKey("install_mode")) {
    String im = prefs.getString("install_mode", "managed");
    mqttCfg.installMode = installModeFromString(im.c_str(), InstallMode::ManagedFiltration);
  } else {
    String oldReg = prefs.getString("reg_mode", "pilote");
    bool oldFiltEnabled = prefs.getBool("filt_enabled", true);
    mqttCfg.installMode = migrateInstallMode(oldReg.c_str(), oldFiltEnabled);
    prefs.end();  // réouverture en écriture pour le write-back one-shot
    Preferences w;
    if (w.begin("poolctrl", false)) {
      w.putString("install_mode", installModeToString(mqttCfg.installMode));
      w.end();
      systemLogger.info("Migration NVS install_mode : reg_mode=" + oldReg +
                        " filt_enabled=" + String(oldFiltEnabled ? 1 : 0) + " → " +
                        String(installModeToString(mqttCfg.installMode)));
    }
    prefs.begin("poolctrl", true);
  }
  mqttCfg.stabilizationDelayMin = prefs.getInt("stab_delay", 5);
  mqttCfg.regulationSpeed = prefs.getString("reg_speed", "normal");
  mqttCfg.phCorrectionType = prefs.getString("ph_corr_type", "ph_minus");

  // feature-021 (Pass 4a) : calibration ORP gérée par le module Atlas EZO
  // (commande Cal,<ref>) — aucune lecture NVS ESP32.

  // Calibration Température
  mqttCfg.tempCalibrationOffset = prefs.getFloat("temp_cal_off", 0.0f);
  mqttCfg.tempCalibrationDate = prefs.getString("temp_cal_date", "");
  mqttCfg.temperatureEnabled = prefs.getBool("temp_enabled", mqttCfg.temperatureEnabled);

  // feature-067 : offset logiciel de mesure pH. Défaut 0.00 si clé absente
  // (appareils existants). Garde fail-safe : une valeur NVS hors bornes
  // (corruption, ancienne plage plus large) est neutralisée à 0.00 plutôt
  // qu'appliquée — un offset aberrant décale la consigne EFFECTIVE de dosage.
  mqttCfg.phOffset = prefs.getFloat("ph_offset", 0.0f);
  if (!isPhOffsetValid(mqttCfg.phOffset)) {
    systemLogger.warning("[Config] Offset pH NVS hors bornes (" +
                         String(mqttCfg.phOffset, 2) + ") — remis à 0.00");
    mqttCfg.phOffset = 0.0f;
  }
  // v2.25.0 — MIGRATION : l'offset devient un curseur au pas de kPhOffsetStep
  // (0,1 pH). Une installation existante a pu enregistrer une valeur libre
  // (0,47 saisie au clavier) : on l'arrondit au plus proche multiple du pas, ce
  // qui déplace la correction de 0,05 pH au maximum, soit le bruit résiduel du
  // filtre — aucun effet mesurable sur la régulation. Sans cet arrondi, le
  // curseur afficherait une position qui ne correspond pas à la valeur appliquée.
  // La réécriture NVS est DIFFÉRÉE après prefs.end() (voir plus bas) : rouvrir la
  // partition en écriture au milieu de la lecture obligerait à refermer puis
  // réouvrir `prefs`, dont l'échec silencieux ferait repartir tout le reste de la
  // configuration sur les valeurs par défaut.
  const float phOffsetRounded = roundPhOffsetToStep(mqttCfg.phOffset);
  bool phOffsetNeedsRewrite = false;
  if (phOffsetRounded != mqttCfg.phOffset) {
    systemLogger.info("[Config] Migration offset pH au pas de " +
                      String(kPhOffsetStep, 2) + " : " + String(mqttCfg.phOffset, 2) +
                      " → " + String(phOffsetRounded, 2));
    mqttCfg.phOffset = phOffsetRounded;
    phOffsetNeedsRewrite = true;
  }

  // Temps
  mqttCfg.timeUseNtp = prefs.getBool("time_use_ntp", mqttCfg.timeUseNtp);
  mqttCfg.ntpServer = prefs.getString("ntp_server", mqttCfg.ntpServer);
  mqttCfg.manualTimeIso = prefs.getString("manual_time", mqttCfg.manualTimeIso);
  mqttCfg.timezoneId = prefs.getString("timezone_id", mqttCfg.timezoneId);

  // Filtration (feature-056 : filt_enabled absorbé par install_mode ci-dessus)
  filtrationCfg.mode = prefs.getString("filt_mode", filtrationCfg.mode);
  filtrationCfg.start = prefs.getString("filt_start", filtrationCfg.start);
  filtrationCfg.end = prefs.getString("filt_end", filtrationCfg.end);


  // Éclairage
  lightingCfg.featureEnabled = prefs.getBool("light_feat_en", lightingCfg.featureEnabled);
  lightingCfg.enabled = prefs.getBool("light_enabled", lightingCfg.enabled);
  lightingCfg.brightness = prefs.getUChar("light_bright", lightingCfg.brightness);
  lightingCfg.scheduleEnabled = prefs.getBool("light_sched_en", lightingCfg.scheduleEnabled);
  lightingCfg.startTime = prefs.getString("light_start", lightingCfg.startTime);
  lightingCfg.endTime = prefs.getString("light_end", lightingCfg.endTime);

  // Authentification
  authCfg.enabled = prefs.getBool("auth_enabled", authCfg.enabled);
  authCfg.forceWifiConfig = prefs.getBool("auth_force_cfg", authCfg.forceWifiConfig);
  authCfg.wizardCompleted = prefs.getBool("wizard_done", false);
  authCfg.disableApOnBoot = prefs.getBool("disable_ap_boot", false);
  authCfg.adminPassword = prefs.getString("auth_password", authCfg.adminPassword);
  authCfg.apiToken = prefs.getString("auth_token", authCfg.apiToken);
  authCfg.apPassword = prefs.getString("auth_ap_pwd", authCfg.apPassword);
  authCfg.sensorLogsEnabled = prefs.getBool("sensor_logs", false);
  authCfg.debugLogsEnabled = prefs.getBool("debug_logs", false);

  // Amplitude des pompes (itération 2 : % → duty du plateau d'impulsion)
  mqttCfg.pump1MaxDutyPct = prefs.getUChar("pump1_max_duty", mqttCfg.pump1MaxDutyPct);
  mqttCfg.pump2MaxDutyPct = prefs.getUChar("pump2_max_duty", mqttCfg.pump2MaxDutyPct);
  // Caractérisation des pompes. feature-074 (condition C1) : clé ABSENTE
  // (appareil neuf / NVS vierge) → CALIBRATION DE TYPE (kPumpTypeCalDuty /
  // kPumpTypePulseFlowMlPerMin, valeur usine du modèle de pompe) — le dosage
  // fonctionne au premier flash. Clé PRÉSENTE à 0 (effacement explicite ou
  // appareil flashé avant 2.23.0 jamais calibré) → 0 est LU tel quel et reste
  // fail-closed (pas de conversion silencieuse vers le défaut de type).
  // SURTOUT PAS de conversion depuis l'ancienne clé pump_max_flow (modèle
  // linéaire faux, purgé au prochain save).
  mqttCfg.pump1CalibratedDuty = prefs.getUChar("p1_cal_duty", kPumpTypeCalDuty);
  mqttCfg.pump1PulseFlowMlPerMin = prefs.getFloat("p1_pulse_flow", kPumpTypePulseFlowMlPerMin);
  mqttCfg.pump2CalibratedDuty = prefs.getUChar("p2_cal_duty", kPumpTypeCalDuty);
  mqttCfg.pump2PulseFlowMlPerMin = prefs.getFloat("p2_pulse_flow", kPumpTypePulseFlowMlPerMin);
  // feature-074 : drapeau « pesée individuelle » (défaut false = valeur usine).
  mqttCfg.pump1FlowWeighed = prefs.getBool("p1_flow_weighed", false);
  mqttCfg.pump2FlowWeighed = prefs.getBool("p2_flow_weighed", false);
  // Garde fail-closed : une valeur NVS implausible (corruption) est neutralisée
  // à 0 (non caractérisé) plutôt qu'utilisée pour créditer un volume fictif.
  // Le drapeau weighed est aussi rabattu : une pesée corrompue n'en est plus une.
  if (!isfinite(mqttCfg.pump1PulseFlowMlPerMin) || mqttCfg.pump1PulseFlowMlPerMin < 0.0f ||
      mqttCfg.pump1PulseFlowMlPerMin > kPulseFlowMaxMlPerMin) {
    systemLogger.warning("[Config] p1_pulse_flow NVS implausible (" +
                         String(mqttCfg.pump1PulseFlowMlPerMin, 1) + ") — pompe 1 non caractérisée");
    mqttCfg.pump1PulseFlowMlPerMin = 0.0f;
    mqttCfg.pump1CalibratedDuty = 0;
    mqttCfg.pump1FlowWeighed = false;
  }
  if (!isfinite(mqttCfg.pump2PulseFlowMlPerMin) || mqttCfg.pump2PulseFlowMlPerMin < 0.0f ||
      mqttCfg.pump2PulseFlowMlPerMin > kPulseFlowMaxMlPerMin) {
    systemLogger.warning("[Config] p2_pulse_flow NVS implausible (" +
                         String(mqttCfg.pump2PulseFlowMlPerMin, 1) + ") — pompe 2 non caractérisée");
    mqttCfg.pump2PulseFlowMlPerMin = 0.0f;
    mqttCfg.pump2CalibratedDuty = 0;
    mqttCfg.pump2FlowWeighed = false;
  }

  // Limites de sécurité
  safetyLimits.maxPhMlPerDay = prefs.getFloat("max_ph_ml", safetyLimits.maxPhMlPerDay);
  safetyLimits.maxChlorineMlPerDay = prefs.getFloat("max_cl_ml", safetyLimits.maxChlorineMlPerDay);

  // feature-060 : limite cycles/jour (garde 9). Clé absente ⇒ défaut struct (48),
  // sans write-back de migration. Valeur NVS hors bornes (corrompue / firmware
  // antérieur) ⇒ clampée [kCyclesPerDayMin, kCyclesPerDayMax] + warn.
  uint32_t rawCyclesPerDay = prefs.getUInt("max_cycles_day", pumpProtection.maxCyclesPerDay);
  bool cyclesClamped = false;
  pumpProtection.maxCyclesPerDay = clampCyclesPerDay(static_cast<long>(rawCyclesPerDay), &cyclesClamped);
  if (cyclesClamped) {
    systemLogger.warning("max_cycles_day NVS hors bornes (" + String(rawCyclesPerDay) +
                         ") — clampé à " + String(pumpProtection.maxCyclesPerDay));
  }

  prefs.end();

  // v2.25.0 — réécriture différée de l'offset pH arrondi au pas (cf. plus haut).
  // Faite APRÈS la fermeture du handle de lecture : aucune réouverture en lecture
  // n'est nécessaire ensuite, donc aucun risque de repartir sur les défauts.
  if (phOffsetNeedsRewrite) {
    Preferences w;
    if (w.begin("poolctrl", false)) {
      w.putFloat("ph_offset", mqttCfg.phOffset);
      w.end();
    } else {
      systemLogger.warning("[Config] Réécriture NVS de l'offset pH arrondi "
                           "impossible — arrondi appliqué en RAM seulement "
                           "(la migration sera rejouée au prochain boot)");
    }
  }

  sanitizePumpSelection();
  ensureTimezoneValid();
  applyTimezoneEnv();

  systemLogger.info("Configuration chargée depuis NVS");
}

void saveProductConfig() {
  // feature-027 : prise bornée. Timeout → return SANS clear productConfigDirty
  // (retry naturel au prochain passage) + warn throttlé 1/min.
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sLastWarnMs = 0;
    unsigned long nowMs = millis();
    if (sLastWarnMs == 0 || nowMs - sLastWarnMs >= kMutexTimeoutWarnThrottleMs) {
      systemLogger.warning("[Config] saveProductConfig: mutex timeout — sauvegarde reportée");
      sLastWarnMs = nowMs;
    }
    return;
  }
  Preferences prefs;
  if (!prefs.begin("pool_prod", false)) {
    systemLogger.error("Échec ouverture NVS produits");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
    return;
  }
  prefs.putBool("ph_track_en", productCfg.phTrackingEnabled);
  prefs.putFloat("ph_cont_ml", productCfg.phContainerVolumeMl);
  prefs.putFloat("ph_inj_ml", productCfg.phTotalInjectedMl);
  prefs.putFloat("ph_alert_ml", productCfg.phAlertThresholdMl);
  prefs.putBool("orp_track_en", productCfg.orpTrackingEnabled);
  prefs.putFloat("orp_cont_ml", productCfg.orpContainerVolumeMl);
  prefs.putFloat("orp_inj_ml", productCfg.orpTotalInjectedMl);
  prefs.putFloat("orp_alert_ml", productCfg.orpAlertThresholdMl);
  prefs.end();
  productConfigDirty = false;
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
  systemLogger.debug("Config produits sauvegardée");
}

void loadProductConfig() {
  Preferences prefs;
  if (!prefs.begin("pool_prod", true)) {
    return; // Pas encore de config produits, utiliser les valeurs par défaut
  }
  productCfg.phTrackingEnabled = prefs.getBool("ph_track_en", productCfg.phTrackingEnabled);
  productCfg.phContainerVolumeMl = prefs.getFloat("ph_cont_ml", productCfg.phContainerVolumeMl);
  productCfg.phTotalInjectedMl = prefs.getFloat("ph_inj_ml", productCfg.phTotalInjectedMl);
  productCfg.phAlertThresholdMl = prefs.getFloat("ph_alert_ml", productCfg.phAlertThresholdMl);
  productCfg.orpTrackingEnabled = prefs.getBool("orp_track_en", productCfg.orpTrackingEnabled);
  productCfg.orpContainerVolumeMl = prefs.getFloat("orp_cont_ml", productCfg.orpContainerVolumeMl);
  productCfg.orpTotalInjectedMl = prefs.getFloat("orp_inj_ml", productCfg.orpTotalInjectedMl);
  productCfg.orpAlertThresholdMl = prefs.getFloat("orp_alert_ml", productCfg.orpAlertThresholdMl);
  prefs.end();
  systemLogger.info("Config produits chargée depuis NVS");
}

void applyMqttConfig() {
  // Cette fonction sera implémentée dans mqtt_manager.cpp
}

// ==== Calibration pH/ORP ====
// feature-021 : entièrement gérée par les modules Atlas EZO (commandes Cal,?,
// Cal,mid, Cal,low, Cal,<ref>). Plus de calculs offset/slope ni d'EEPROM côté
// ESP32. Voir SensorManager::enqueueCalibrate*() et getPhCalibrationPointsCached().

/**
 * Persiste les compteurs journaliers de dosage en NVS (namespace "pool-daily").
 * Appelé : au démarrage d'une injection, toutes les 30s pendant l'injection,
 * et au reset journalier (minuit local).
 */
void saveDailyCounters() {
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("[Config] saveDailyCounters: mutex timeout");
    return;
  }
  Preferences prefs;
  prefs.begin("pool-daily", false);
  prefs.putFloat("ph_daily_ml", safetyLimits.dailyPhInjectedMl);
  prefs.putFloat("orp_daily_ml", safetyLimits.dailyOrpInjectedMl);
  prefs.putString("daily_date", safetyLimits.currentDayDate);
  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
}

/**
 * Restaure les compteurs journaliers depuis NVS au démarrage.
 * Compare la date sauvegardée avec la date courante (si NTP/RTC valide).
 * Si nouveau jour : remet les compteurs à 0 et sauvegarde.
 * Si heure non synchronisée : restaure conservativement pour ne pas perdre les comptages.
 */
void loadDailyCounters() {
  Preferences prefs;
  prefs.begin("pool-daily", true);
  bool hasDate = prefs.isKey("daily_date");
  float phMl   = prefs.getFloat("ph_daily_ml", 0.0f);
  float orpMl  = prefs.getFloat("orp_daily_ml", 0.0f);
  String savedDate = prefs.getString("daily_date", "");
  prefs.end();

  if (!hasDate) {
    systemLogger.info("[Config] loadDailyCounters: aucune donnée NVS, démarrage à 0");
    return;
  }

  // Validation des valeurs lues
  if (phMl < 0.0f) phMl = 0.0f;
  if (orpMl < 0.0f) orpMl = 0.0f;
  if (phMl > safetyLimits.maxPhMlPerDay * 1.1f) {
    systemLogger.critical("[Config] NVS ph_daily_ml hors plage: " + String(phMl, 0) + " mL — remis à limite");
    phMl = safetyLimits.maxPhMlPerDay;
  }
  if (orpMl > safetyLimits.maxChlorineMlPerDay * 1.1f) {
    systemLogger.critical("[Config] NVS orp_daily_ml hors plage: " + String(orpMl, 0) + " mL — remis à limite");
    orpMl = safetyLimits.maxChlorineMlPerDay;
  }

  time_t now = time(nullptr);
  if (now >= kMinValidEpoch) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char todayStr[9];
    strftime(todayStr, sizeof(todayStr), "%Y%m%d", &timeinfo);

    if (savedDate == todayStr) {
      safetyLimits.dailyPhInjectedMl  = phMl;
      safetyLimits.dailyOrpInjectedMl = orpMl;
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      systemLogger.info("[Config] Compteurs journaliers restaurés: pH=" + String(phMl, 0) + " mL, ORP=" + String(orpMl, 0) + " mL");
    } else {
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      saveDailyCounters();
      systemLogger.info("[Config] Nouveau jour détecté — compteurs remis à 0");
    }
  } else {
    // Heure non synchronisée : restore conservatif (sécurité)
    safetyLimits.dailyPhInjectedMl  = phMl;
    safetyLimits.dailyOrpInjectedMl = orpMl;
    strlcpy(safetyLimits.currentDayDate, savedDate.c_str(), sizeof(safetyLimits.currentDayDate));
    systemLogger.info("[Config] Heure non synchro — compteurs restaurés conservativement");
  }
}

// =============================================================================
// Mode Boost (feature-053) — surchloration temporaire du jour
// =============================================================================
// Persistance NVS DÉDIÉE (namespace poolctrl, clés boost_active/boost_until) —
// écriture ciblée pour éviter l'usure NVS d'une réécriture complète de la config.

void saveBoostState() {
  // Prise de mutex bornée (miroir de saveDailyCounters).
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("[Config] saveBoostState: mutex timeout — état boost non persisté");
    return;
  }
  Preferences prefs;
  if (!prefs.begin("poolctrl", false)) {
    systemLogger.error("Échec ouverture NVS pour état boost");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
    return;
  }
  prefs.putBool("boost_active", boostState.active);
  prefs.putLong64("boost_until", (int64_t)boostState.untilEpoch);
  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
}

void loadBoostState() {
  Preferences prefs;
  if (!prefs.begin("poolctrl", true)) {
    return;  // Pas de config, boost inactif par défaut
  }
  boostState.active = prefs.getBool("boost_active", false);
  boostState.untilEpoch = (time_t)prefs.getLong64("boost_until", 0);
  prefs.end();

  // Si expiré au chargement (heure valide et minuit déjà passé) → inactif.
  time_t now = time(nullptr);
  if (boostState.active && now >= kMinValidEpoch && now >= boostState.untilEpoch) {
    boostState.active = false;
    systemLogger.info("[Boost] État chargé expiré (minuit dépassé) — boost inactif");
  } else if (boostState.active) {
    systemLogger.info("[Boost] État restauré : boost actif jusqu'à epoch " + String((long)boostState.untilEpoch));
  }
}

void startBoost() {
  time_t now = time(nullptr);
  // Condition pool-chemistry #4 : sans heure synchronisée, l'expiration à minuit
  // ne peut être calculée → refus (fail-closed).
  if (now < kMinValidEpoch) {
    systemLogger.warning("[Boost] Activation refusée : heure non synchronisée (expiration minuit indéterminable)");
    return;
  }
  // Calcul du prochain minuit local (l'environnement TZ est appliqué par applyTimezoneEnv).
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  timeinfo.tm_hour = 0;
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;
  timeinfo.tm_mday += 1;  // Minuit du jour suivant
  timeinfo.tm_isdst = -1; // Laisse mktime résoudre l'heure d'été
  time_t midnight = mktime(&timeinfo);

  // Écrire untilEpoch AVANT active : un lecteur concurrent (loopTask) voyant
  // active=true a alors toujours un untilEpoch valide (cohérence sans mutex sur
  // les champs — la paire bool+time_t est alignée, R/W atomiques Xtensa).
  boostState.untilEpoch = midnight;
  boostState.active = true;
  saveBoostState();
  systemLogger.info("[Boost] Activé jusqu'au prochain minuit local (epoch " + String((long)midnight) + ")");
  // feature-054 : signaler les leviers inertes selon la config, pour que
  // l'utilisateur sache ce que le Boost fait réellement (couvre HTTP ET HA).
  if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
    systemLogger.warning("[Boost] Filtration non gérée par PoolController — le Boost NE prolonge PAS la filtration");
  }
  if (mqttCfg.orpRegulationMode != "automatic") {
    systemLogger.warning("[Boost] Régulation ORP non automatique — le Boost NE relève PAS le chlore (seule la filtration est prolongée, si gérée)");
  }
}

void stopBoost() {
  boostState.active = false;
  boostState.untilEpoch = 0;
  saveBoostState();
  systemLogger.info("[Boost] Désactivé");
}

bool isBoostActive(time_t now) {
  // Condition pool-chemistry #4 : heure non synchronisée → ne pas expirer,
  // renvoyer l'état persisté tel quel (on ne peut pas comparer à untilEpoch).
  if (now < kMinValidEpoch) {
    return boostState.active;
  }
  return boostState.active && now < boostState.untilEpoch;
}

void tickBoostExpiry(time_t now) {
  // Expire le boost au passage de minuit. Appelé EN TÊTE de tickDailyRollover,
  // AVANT tout reset de compteur (condition pool-chemistry #3). Ne fait rien si
  // l'heure n'est pas valide (condition #4 : pas d'expiration sans heure fiable).
  if (boostState.active && now >= kMinValidEpoch && now >= boostState.untilEpoch) {
    boostState.active = false;
    boostState.untilEpoch = 0;
    saveBoostState();
    systemLogger.info("[Boost] Expiré au passage de minuit — retour au comportement normal");
  }
}
