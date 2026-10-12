#include "config.h"
#include "constants.h"
#include "logger.h"
#include "ph_offset.h"  // feature-067 : isPhOffsetValid() (logique pure)
#include "temp_compensation_logic.h"  // bug-114 : clampTempCalOffset() (logique pure)
#include "ph_guard_logic.h"  // v2.25.0 : roundPhOffsetToStep() (logique pure)
#include "ph_integral_logic.h"  // v2.27.0 : phSustainableAvgFlow() (logique pure)
#include "ph_actuator_limit_logic.h"  // feature-086 : phActuatorLimitStreak() (logique pure)
#include "temp_mode_logic.h"  // feature-140 : rescueActivePure()/rescueDeadlinePure() (logique pure)
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
// clamp vers le haut), maxFlow = MAJORANT de flotte à duty 255 — et non un
// débit « mesuré » : depuis la v2.25.2 c'est kPumpFlow255MlPerMin (74,0 =
// max mesuré 68,0 + 8 % de dispersion), commun aux deux pompes.
// DETTE (revue v2.25.2) : PumpControlParams est une structure MORTE —
// phPumpControl/orpPumpControl sont déclarées et initialisées ici, mais lues
// NULLE PART dans src/ ni test/. À supprimer lors d'un passage sur ce fichier.
PumpControlParams phPumpControl  = {0.0f, kPumpFlow255MlPerMin, kPhMaxError};
PumpControlParams orpPumpControl = {0.0f, kPumpFlow255MlPerMin, kOrpMaxError};
SafetyLimits safetyLimits;
PumpProtection pumpProtection;
ProductConfig productCfg;
bool productConfigDirty = false;
BoostState boostState;
RescueState rescueState;

// feature-146 (revue code-reviewer) : voir le commentaire de la déclaration
// (config.h). Buffer de pile, AUCUNE String allouée.
FiltrationMode parseFiltrationModeFrom(const String& raw) {
  char buf[8];
  const char* src = raw.c_str();
  size_t i = 0;
  for (; i < sizeof(buf) - 1 && src[i] != '\0'; i++) {
    char c = src[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[i] = '\0';
  return parseFiltrationMode(buf);
}

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

// =============================================================================
// feature-156 — horodatage de justesse (correction manuelle pH vs étalonnage)
// =============================================================================
// setPhOffsetStamped() : écrivain UNIQUE de phOffset/phOffsetEpoch/phOffsetSeq.
// Mute la RAM seulement — l'APPELANT persiste (même protocole que
// setPhAutoManualTrace ci-dessus). Appelée avec configMutex DÉJÀ détenu par les
// deux routes concernées (handleTunePhOffset, branche ph_offset de
// /save-config) : ne le reprend pas, un mutex récursif le permettrait mais ce
// n'est pas le contrat voulu ici (symétrie avec le patron existant).
// « Garder la correction » (B11, écran de réglage) ré-envoie la MÊME route
// avec la valeur COURANTE : ce ré-envoi re-estampille (phOffsetSeq devient
// > phCalSeq) et ÉTEINT le verdict de caducité durablement — c'est voulu, pas
// une redondance à simplifier : c'est la manière dont l'utilisateur confirme
// qu'il assume la valeur après l'avoir vue signalée comme périmée.
void setPhOffsetStamped(float offset, const char* source) {
  const time_t now = time(nullptr);
  const float oldOffset = mqttCfg.phOffset;
  mqttCfg.phOffset = offset;
  // Patron EXACT de setPhAutoManualTrace : epoch = 0 si l'heure n'est pas
  // synchronisée (« date inconnue » plutôt qu'un 1970 trompeur). Sert à
  // L'AFFICHAGE SEUL — c'est phOffsetSeq qui fait foi pour l'ordre.
  mqttCfg.phOffsetEpoch = (now >= kMinValidEpoch) ? (uint32_t)now : 0;
  mqttCfg.phOffsetSeq = nextJustnessSeq(mqttCfg.phOffsetSeq, mqttCfg.phCalSeq);
  systemLogger.info("[pH] Offset re-estampillé (" + String(source) + ") : " +
                    String(oldOffset, 2) + " → " + String(offset, 2) +
                    " (seq=" + String(mqttCfg.phOffsetSeq) + ")");
}

// markPhCalibrationStamped() : écrivain UNIQUE de phCalEpoch/phCalSeq. Appelée
// depuis loopTask (sensors.cpp, _executeEzoCmd, branches CalibratePhMid/Low
// ET ClearPhCal, cf. sensors.cpp), donc HORS section critique : elle prend
// configMutex elle-même et persiste dans la même transaction. Sur timeout :
// error journalisé, AUCUNE mutation — ne jamais laisser un seq posé sans sa
// persistance (un redémarrage perdrait alors un étalonnage « plus récent » que
// ce qu'il est réellement, et un verdict de caducité pourrait rester figé à
// tort).
void markPhCalibrationStamped() {
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.error("[pH] markPhCalibrationStamped: timeout mutex — étalonnage NON horodaté");
    return;
  }
  const time_t now = time(nullptr);
  mqttCfg.phCalEpoch = (now >= kMinValidEpoch) ? (uint32_t)now : 0;
  mqttCfg.phCalSeq = nextJustnessSeq(mqttCfg.phOffsetSeq, mqttCfg.phCalSeq);
  saveMqttConfig();  // mutex récursif : persistance dans la même section critique
  // Garde symétrique de la prise : si configMutex est nul, on n'a rien pris —
  // donner un sémaphore nul est un comportement indéfini (relevé en revue).
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
  systemLogger.info("[pH] Étalonnage horodaté (seq=" + String(mqttCfg.phCalSeq) + ")");
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
  // feature-090 : taille du bassin (présélection des défauts — JAMAIS une
  // entrée de calcul de dosage). Sérialisation stable "small"/"medium"/"large".
  prefs.putString("pool_size", poolSizeToString(mqttCfg.poolSize));
  prefs.putString("ph_corr_type", mqttCfg.phCorrectionType);

  // Calibration Température
  prefs.putFloat("temp_cal_off", mqttCfg.tempCalibrationOffset);
  prefs.putString("temp_cal_date", mqttCfg.tempCalibrationDate);
  prefs.putBool("temp_enabled", mqttCfg.temperatureEnabled);

  // feature-067 : offset logiciel de mesure pH (clé ≤ 15 caractères)
  prefs.putFloat("ph_offset", mqttCfg.phOffset);
  // feature-156 : horodatage de justesse (clés ≤ 15 caractères). Défauts 0 sur
  // une NVS existante sans ces clés → rétrocompatible (cf. config.h).
  prefs.putULong("ph_offset_ep", mqttCfg.phOffsetEpoch);
  prefs.putULong("ph_offset_sq", mqttCfg.phOffsetSeq);
  prefs.putULong("ph_cal_ep", mqttCfg.phCalEpoch);
  prefs.putULong("ph_cal_sq", mqttCfg.phCalSeq);

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
  mqttCfg.phDailyTargetMl = prefs.getInt("ph_daily_ml", kDefaultPhDailyTargetMl);
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
  mqttCfg.orpDailyTargetMl = prefs.getInt("orp_daily_ml", kDefaultOrpDailyTargetMl);
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
  // feature-088 : défauts de tuning relus depuis la source UNIQUE (regulation_defaults.h)
  mqttCfg.stabilizationDelayMin = prefs.getInt("stab_delay", kDefaultStabilizationDelayMin);
  mqttCfg.regulationSpeed = prefs.getString("reg_speed", kDefaultRegulationSpeed);
  // feature-090 : taille du bassin. Clé absente (flotte existante) ou valeur
  // corrompue → Medium (poolSizeFromString), la colonne des défauts v2.30.0.
  // On ne fait ICI que CHARGER le champ : JAMAIS d'application des défauts au
  // boot — les réglages NVS existants restent intouchés par une mise à jour.
  mqttCfg.poolSize = poolSizeFromString(prefs.getString("pool_size", "medium").c_str());
  mqttCfg.phCorrectionType = prefs.getString("ph_corr_type", "ph_minus");

  // feature-021 (Pass 4a) : calibration ORP gérée par le module Atlas EZO
  // (commande Cal,<ref>) — aucune lecture NVS ESP32.

  // Calibration Température
  // bug-114 — ÉCRÊTAGE au chargement, à ±5 °C. Clamp **2/4** : il y a QUATRE
  // sites d'appel de clampTempCalOffset() (compte vérifié par
  // `grep -rn clampTempCalOffset src/`, 2026-09-07), aucun n'étant un doublon —
  // ils couvrent des PROVENANCES distinctes de la valeur :
  //   1/4 écriture      → web_routes_config.cpp (saisie de l'opérateur) ;
  //   2/4 chargement    → ICI : valeur HÉRITÉE d'une version qui n'imposait
  //                       aucune borne, jamais réenregistrée depuis ;
  //   3/4 cache T° eau  → sensors.cpp, `tempValue` (affichage / MQTT / histo) ;
  //   4/4 ÉMISSION `RT` → sensors.cpp, getWaterTemperature() — LE site qui
  //                       alimente la commande de compensation de l'EZO pH.
  //                       C'est celui dont la suppression fausserait le pH.
  // Motif spécifique de CELUI-CI : sans lui, `/get-config` annoncerait un
  // offset que le firmware n'applique PAS (il serait borné plus tard, à
  // l'émission). C'est exactement le piège « config d'usine ≠ config vivante »
  // qui a fait dérailler le diagnostic chiffré du 2026-09-06 — et la règle du
  // dépôt « toujours lire /get-config avant tout diagnostic chiffré »
  // deviendrait alors un piège au lieu d'une protection. La valeur EXPOSÉE doit
  // être la valeur APPLIQUÉE.
  // Journalisé une seule fois, au boot (ce bloc n'est parcouru qu'au chargement
  // de la configuration) : une NVS polluée est une information utile au
  // démarrage, pas un message à répéter.
  {
    const float storedTempOffset = prefs.getFloat("temp_cal_off", 0.0f);
    mqttCfg.tempCalibrationOffset = clampTempCalOffset(storedTempOffset);
    if (!isTempCalOffsetInRange(storedTempOffset)) {
      systemLogger.warning("NVS : offset température hors plage (" +
                           String(storedTempOffset, 2) + " °C) écrêté à " +
                           String(mqttCfg.tempCalibrationOffset, 2) +
                           " °C — bornes " + String(kTempCalOffsetMin, 1) +
                           " à +" + String(kTempCalOffsetMax, 1) + " °C");
    }
  }
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
  // feature-156 : horodatage de justesse. Clés absentes (config antérieure à
  // 2.72.0) → 0 partout = « aucun événement connu », verdict de caducité
  // toujours faux par construction (isPhOffsetStale). AUCUN estampillage
  // n'a lieu ici — seule une LECTURE brute des valeurs NVS (cf. contrainte
  // écrite sur setPhOffsetStamped/markPhCalibrationStamped : ne jamais
  // écrire ces 4 champs dans loadConfig()).
  mqttCfg.phOffsetEpoch = prefs.getULong("ph_offset_ep", 0);
  mqttCfg.phOffsetSeq   = prefs.getULong("ph_offset_sq", 0);
  mqttCfg.phCalEpoch    = prefs.getULong("ph_cal_ep", 0);
  mqttCfg.phCalSeq      = prefs.getULong("ph_cal_sq", 0);
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

// =============================================================================
// feature-088 (v2.31.0) — RESTAURATION DES VALEURS D'USINE DE RÉGULATION
// =============================================================================
// La LISTE des champs copiés est le template PUR applyRegulationDefaults()
// (regulation_defaults.h), prouvable par mutation en natif — ici il ne reste
// que le transport : mutex, persistance NVS et journal avant → après.
// feature-090 : les valeurs restaurées sont celles de la TAILLE DE BASSIN
// COURANTE (mqttCfg.poolSize) — la taille elle-même n'est PAS modifiée.
bool restoreRegulationDefaults() {
  // Prise bornée (miroir de saveMqttConfig). Timeout → restauration ANNULÉE en
  // bloc : aucun champ n'est modifié, jamais d'application partielle — et le
  // retour false permet à la route de répondre une ERREUR au lieu d'un faux
  // succès (revue v2.31.0 : une réponse mensongère sur une route CRITICAL de
  // régulation est inacceptable, même sans risque chimique).
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.error("[Config] restoreRegulationDefaults: timeout mutex — restauration annulée");
    return false;
  }

  // feature-090 : colonne CIBLE = celle de la taille de bassin courante (lue
  // sous mutex — c'est une référence constexpr vers la table, jamais invalidée).
  const PoolSize size = mqttCfg.poolSize;
  const RegulationDefaults& d = regulationDefaultsFor(size);

  // Instantané AVANT — condition 1 du visa pool-chemistry (leçon phAutoManual :
  // sans la valeur précédente, un incident est indiagnosticable).
  const float  oldPhTarget   = mqttCfg.phTarget;
  const float  oldOrpTarget  = mqttCfg.orpTarget;
  const int    oldPhLimit    = mqttCfg.phInjectionLimitMinutes;
  const int    oldOrpLimit   = mqttCfg.orpInjectionLimitMinutes;
  // feature-091 : le délai de stabilisation n'est plus un réglage (7 champs).
  const int    oldPhDailyMl  = mqttCfg.phDailyTargetMl;
  const int    oldOrpDailyMl = mqttCfg.orpDailyTargetMl;
  char oldSpeed[8];  // "slow"/"normal"/"fast" — copie sans String (budget flash)
  strlcpy(oldSpeed, mqttCfg.regulationSpeed.c_str(), sizeof(oldSpeed));

  // Niveau du journal décidé sur l'état AVANT copie (prédicat PUR) : warning si
  // la restauration AUGMENTE la capacité de dosage (consigne ORP qui monte,
  // limite horaire pH/ORP qui monte — ex. bassin < 20 m³ volontairement
  // redescendu à 5 min/h), info sinon.
  const bool raisesDosing =
      regulationDefaultsRaiseDosing(d, oldOrpTarget, oldPhLimit, oldOrpLimit);

  // Copie des 7 champs de tuning de la colonne de la taille courante — RIEN
  // d'autre (invariants du visa : modes, enabled, phCorrectionType, phOffset,
  // trace phAutoManual*, calibration et sélection des pompes, SafetyLimits
  // ET poolSize intouchés).
  applyRegulationDefaults(mqttCfg, size);

  saveMqttConfig();  // mutex récursif : persistance dans la même section critique
  xSemaphoreGiveRecursive(configMutex);

  // Journal COMPACT (condition 1 du visa) : UN message, les 7 champs lisibles
  // avant → après (un champ inchangé s'y lit x → x — l'état complet d'avant
  // restauration reste ainsi toujours reconstituable depuis le journal).
  // UN SEUL snprintf : nettement plus compact en flash (budget ≤ 2 Ko) que sept
  // segments conditionnels ou des concaténations String.
  char msg[352];
  snprintf(msg, sizeof(msg),
           "[Config] Défauts de régulation (taille %s) restaurés%s : pH %.2f→%.2f ; "
           "ORP %.0f→%.0f mV ; limites pH %d→%d / ORP %d→%d min/h ; "
           "prog. pH %d→%d / ORP %d→%d mL/j ; vitesse %s→%s",
           poolSizeToString(size),
           raisesDosing ? " (dosage AUGMENTÉ)" : "",
           (double)oldPhTarget, (double)d.phTarget,
           (double)oldOrpTarget, (double)d.orpTarget,
           oldPhLimit, d.phInjectionLimitMin,
           oldOrpLimit, d.orpInjectionLimitMin,
           oldPhDailyMl, d.phDailyTargetMl,
           oldOrpDailyMl, d.orpDailyTargetMl,
           oldSpeed, d.regulationSpeed);
  systemLogger.log(raisesDosing ? LogLevel::WARNING : LogLevel::INFO, msg);
  return true;
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
  // feature-110 : compteurs de démarrages + hygiène de mesure. Persistés avec le
  // volume et remis à zéro avec lui (web_routes_config, blocs *_reset_container)
  // — les deux grandeurs ne sont comparables qu'à cette condition. Clés NVS
  // ≤ 15 caractères (limite Preferences).
  prefs.putUInt("ph_starts", productCfg.phStartCount);
  prefs.putUInt("ph_anom", productCfg.phAcctAnomalyCount);
  prefs.putFloat("ph_anom_ml", productCfg.phAcctAnomalyMl);
  prefs.putUInt("orp_starts", productCfg.orpStartCount);
  prefs.putUInt("orp_anom", productCfg.orpAcctAnomalyCount);
  prefs.putFloat("orp_anom_ml", productCfg.orpAcctAnomalyMl);
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
  // feature-110 : absents en NVS sur un appareil qui monte de version → défauts
  // de structure (0), c'est-à-dire « campagne démarrée à ce boot ». Le volume
  // déjà cumulé, lui, est conservé : le premier relevé après montée de version
  // porte donc un N tronqué et n'est pas exploitable — c'est assumé, et c'est
  // pourquoi le mode d'emploi du relevé demande de repartir d'un reset.
  productCfg.phStartCount = prefs.getUInt("ph_starts", productCfg.phStartCount);
  productCfg.phAcctAnomalyCount = prefs.getUInt("ph_anom", productCfg.phAcctAnomalyCount);
  productCfg.phAcctAnomalyMl = prefs.getFloat("ph_anom_ml", productCfg.phAcctAnomalyMl);
  productCfg.orpStartCount = prefs.getUInt("orp_starts", productCfg.orpStartCount);
  productCfg.orpAcctAnomalyCount = prefs.getUInt("orp_anom", productCfg.orpAcctAnomalyCount);
  productCfg.orpAcctAnomalyMl = prefs.getFloat("orp_anom_ml", productCfg.orpAcctAnomalyMl);
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

// =============================================================================
// feature-084 (sujet 3) — INSTANTANÉ NVS DE L'INTÉGRALE pH
// =============================================================================
// L'ADR-0052 interdisait toute persistance de l'intégrale. Le gate
// pool-chemistry du 30/08/2026 l'autorise sous SEPT conditions cumulatives
// (R1-R7, cf. constants.h) ; l'ADR est amendé en conséquence.
//
// ICI, IL N'Y A QUE DU TRANSPORT — ouvrir la NVS, lire/écrire 20 octets, la
// refermer. TOUTES les décisions, y compris le calcul et la VÉRIFICATION de
// l'empreinte d'intégrité, vivent dans `ph_persist_logic.h`. Motif : config.cpp
// n'est pas dans le `build_src_filter` de `env:native`, donc tout ce qui décide
// ici est hors d'atteinte des tests — et c'est précisément dans une zone de ce
// genre que le défaut de la garde C2 s'était logé.
bool loadPhIntegralSnapshot(PhIntegralSnapshot& out) {
  PhIntegralSnapshot raw;
  Preferences prefs;
  if (!prefs.begin("pool-phint", true)) return false;  // namespace jamais écrit
  const size_t read = prefs.getBytes("snap", &raw, sizeof(raw));
  prefs.end();
  // Taille inattendue ou empreinte discordante ⇒ rejet net, jamais de
  // réinterprétation partielle (fail-closed R2). Le prédicat est PUR et testé.
  if (!phPersistSnapshotIntact(raw, read)) return false;
  out = raw;
  return true;
}

void savePhIntegralSnapshot(PhIntegralSnapshot& s) {
  phPersistSealSnapshot(s);  // normalise `reserved`, pose `crc` (logique pure)
  Preferences prefs;
  if (!prefs.begin("pool-phint", false)) {
    systemLogger.warning("[Config] Instantané intégrale pH : ouverture NVS refusée");
    return;
  }
  prefs.putBytes("snap", &s, sizeof(s));
  prefs.end();
}

// =============================================================================
// v2.27.0 (feature-084, R1) — ANNEAU NVS DE L'HISTORIQUE DE CHARGE ACIDE
// =============================================================================
// Sept journées {date, mL pH, minutes de filtration} dans UN blob de 64 octets,
// rangé dans le namespace EXISTANT `pool-daily` (clé `ph_load7`, 8 caractères —
// la NVS en tolère 15) : aucun namespace supplémentaire à provisionner, et
// l'anneau vit au plus près des compteurs dont il archive la clôture.
//
// UNE écriture par jour, hors de toute transaction critique : elle n'a lieu qu'au
// basculement de date, jamais pendant une injection.
//
// ICI, IL N'Y A QUE DU TRANSPORT. Schéma, intégrité, insertion, critère de jour
// exploitable et médiane vivent dans `ph_persist_logic.h` — config.cpp n'est pas
// dans le `build_src_filter` de `env:native`, donc tout ce qui déciderait ici
// serait hors d'atteinte des tests.
bool loadPhLoadHistory(PhLoadHistory& out) {
  PhLoadHistory raw;
  Preferences prefs;
  if (!prefs.begin("pool-daily", true)) return false;  // namespace jamais écrit
  const size_t read = prefs.getBytes("ph_load7", &raw, sizeof(raw));
  prefs.end();
  // Deux issues, deux traitements. ABSENCE (0 octet) = cas nominal d'un appareil
  // qui n'a pas encore clôturé sa première journée : silence. CORRUPTION (des
  // octets, mais taille, schéma ou empreinte discordants) = sept journées
  // d'observation qui disparaissent : cela ne doit pas se produire sans trace,
  // c'est le choix déjà fait pour l'instantané.
  if (read == 0) return false;
  if (!phLoadHistoryIntact(raw, read)) {  // prédicat PUR et testé
    systemLogger.warning("[Charge pH] Anneau NVS illisible (taille, schéma ou "
                         "empreinte) — historique vidé, plus de restauration "
                         "d'intégrale avant 3 nouvelles journées");
    return false;
  }
  out = raw;
  return true;
}

void savePhLoadHistory(PhLoadHistory& h) {
  phLoadHistorySeal(h);  // schéma, octets d'alignement, empreinte (logique pure)
  Preferences prefs;
  if (!prefs.begin("pool-daily", false)) {
    systemLogger.warning("[Charge pH] Historique de charge : ouverture NVS refusée");
    return;
  }
  prefs.putBytes("ph_load7", &h, sizeof(h));
  prefs.end();
}

// =============================================================================
// feature-096 (v2.37.0) — ANNEAU NVS DU TEMPS D'ATTEINTE DE CIBLE
// =============================================================================
// Namespace `pool-kpi`, clé `ttt7` : 188 octets, DEUX écritures par jour et par
// régulation (une au départ, une à la clôture). Le motif du namespace disjoint
// de `pool-daily` est développé dans config.h (moindre privilège : cet
// instrument ne doit pas détenir de handle d'écriture sur les compteurs de
// dosage). ICI, IL N'Y A QUE DU TRANSPORT.
bool loadTttHistory(TttHistory& out) {
  TttHistory raw;
  Preferences prefs;
  if (!prefs.begin("pool-kpi", true)) return false;  // namespace jamais écrit
  const size_t read = prefs.getBytes("ttt7", &raw, sizeof(raw));
  prefs.end();
  // ABSENCE (0 octet) = cas nominal d'un appareil qui n'a pas encore archivé de
  // journée : silence. CORRUPTION = sept journées d'observation qui
  // disparaissent, cela ne doit pas se produire sans trace.
  if (read == 0) return false;
  if (!tttHistoryIntact(raw, read)) {  // prédicat PUR et testé
    systemLogger.warning("[KPI] Anneau NVS illisible (taille, schéma ou empreinte) "
                         "— mémoire des 7 jours vidée");
    return false;
  }
  out = raw;
  return true;
}

bool saveTttHistory(TttHistory& h, uint32_t serieStamp) {
  tttHistorySeal(h, serieStamp);  // schéma, alignement, estampille, empreinte
  Preferences prefs;
  if (!prefs.begin("pool-kpi", false)) return false;  // C4 : fail-closed, sans retry
  const size_t written = prefs.putBytes("ttt7", &h, sizeof(h));
  prefs.end();
  return written == sizeof(h);
}

/**
 * Date du jour au format AAAAMMJJ, ou 0 si l'horloge n'est pas exploitable.
 * C'est l'ORIGINE de la fenêtre d'âge de l'anneau : sans elle, aucun échantillon
 * n'est retenu (fail-closed, cf. phLoadDayWithinWindow).
 */
// feature-096 : n'est plus `static` — l'indicateur de temps d'atteinte date ses
// journées avec CETTE fonction plutôt qu'avec une seconde, écrite ailleurs et
// susceptible d'en diverger (fuseau, seuil d'epoch valide).
uint32_t currentLoadDateNumber() {
  time_t now = time(nullptr);
  if (now < kMinValidEpoch) return 0;
  struct tm ti;
  localtime_r(&now, &ti);
  return (uint32_t)(ti.tm_year + 1900) * 10000UL +
         (uint32_t)(ti.tm_mon + 1) * 100UL + (uint32_t)ti.tm_mday;
}

float phSustainableFlowFromConfig() {
  const int idx = pumpIndexFromNumber(mqttCfg.phPump);
  const float amp = (idx == 0) ? mqttCfg.pump1PulseFlowMlPerMin
                               : mqttCfg.pump2PulseFlowMlPerMin;
  // Même fonction PURE que le prédicat de saturation de l'intégrale : le filet de
  // vraisemblance de l'anneau et le gel d'anti-windup parlent ainsi du MÊME débit.
  return phSustainableAvgFlow(amp, mqttCfg.phInjectionLimitMinutes);
}

// feature-092 : MIROIR EXACT de la fonction ci-dessus pour la voie ORP — pompe
// ORP configurée, limite horaire ORP. AUCUNE logique neuve : `phSustainableAvgFlow`
// est déjà PARAMÉTRÉE (ph_integral_logic.h) et ne porte de « ph » que dans son
// nom, hérité de son premier appelant. Le chlore n'a ni intégrale ni feedforward,
// mais il a la MÊME limite horaire, donc le même plafond de débit moyen : sans
// ce plafond affiché, la carte « Débit d'injection ORP » ne pourrait pas dire
// « sur X possibles ». LECTURE SEULE : aucune décision de dosage ne l'appelle.
float orpSustainableFlowFromConfig() {
  const int idx = pumpIndexFromNumber(mqttCfg.orpPump);
  const float amp = (idx == 0) ? mqttCfg.pump1PulseFlowMlPerMin
                               : mqttCfg.pump2PulseFlowMlPerMin;
  return phSustainableAvgFlow(amp, mqttCfg.orpInjectionLimitMinutes);
}

PhPersistLoadObservation readPhLoadObservation(float sustainableMlPerMin) {
  PhLoadHistory h;                    // vide par défaut ⇒ days = 0 (fail-closed)
  loadPhLoadHistory(h);               // anneau absent ou corrompu : on garde vide
  // Deux bornes, toutes deux fournies par l'appelant : la date du jour BORNE
  // l'âge des échantillons (l'anneau garde les 7 dernières journées ARCHIVÉES,
  // qui ne sont les 7 derniers JOURS que si l'appareil a tourné sans
  // interruption), le débit soutenable BORNE leur vraisemblance.
  return phLoadObservation(h, currentLoadDateNumber(), sustainableMlPerMin);
}

uint8_t readPhActuatorLimitStreak(float sustainableMlPerMin) {
  PhLoadHistory h;      // vide par défaut ⇒ série 0 (fail-closed)
  loadPhLoadHistory(h); // anneau absent ou corrompu : on garde vide
  // Mêmes bornes que readPhLoadObservation : la date du jour ancre la série (le
  // jour archivé le plus récent doit dater d'hier ou d'aujourd'hui), le débit
  // soutenable borne la vraisemblance ET fixe le maximum délivrable par journée.
  return phActuatorLimitStreak(h, currentLoadDateNumber(), sustainableMlPerMin);
}

void recordPhLoadDay(const char* date8, float phMl, uint32_t filtrationMinutes,
                     float sustainableMlPerMin) {
  if (date8 == nullptr) return;
  // Sans horloge valide, la journée n'est pas datable — donc pas archivable.
  const uint32_t date = (uint32_t)strtoul(date8, nullptr, 10);
  if (date < 19700101UL) return;

  PhLoadDaySample s;
  s.date = date;
  s.minutes = (filtrationMinutes > 65535UL) ? 65535u : (uint16_t)filtrationMinutes;
  float ml = phMl;
  if (!(ml > 0.0f)) ml = 0.0f;  // NaN et négatifs ⇒ 0 (journée à charge nulle)
  s.ml = (ml > 65535.0f) ? 65535u : (uint16_t)lroundf(ml);

  PhLoadHistory h;
  loadPhLoadHistory(h);
  if (!phLoadHistoryAppend(h, s)) return;
  savePhLoadHistory(h);  // l'UNIQUE écriture NVS quotidienne de l'anneau

  // Observabilité : le volume et les minutes réellement archivés, plus l'état de
  // l'historique qui en résulte — c'est cette profondeur qui arme (ou non) la
  // restauration au prochain démarrage.
  const PhPersistLoadObservation obs =
      phLoadObservation(h, currentLoadDateNumber(), sustainableMlPerMin);
  char msg[192];
  snprintf(msg, sizeof(msg),
           "[Charge pH] Journée %lu archivée : %u mL en %u min de circulation ; "
           "%u/%u jour(s) exploitable(s), charge médiane %.2f mL/min",
           (unsigned long)date, (unsigned)s.ml, (unsigned)s.minutes,
           (unsigned)obs.days, (unsigned)kPhIntegralRestoreMinHistoryDays,
           (double)obs.loadMlPerMin);
  systemLogger.info(msg);
}

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
  // v2.27.0 : dénominateur de la charge observée. Aucune transaction NVS
  // supplémentaire n'est CRÉÉE (le champ voyage dans celle-ci), mais le coût doit
  // être assumé plutôt que nié : `_dailyCountersDirty` est désormais posé à
  // CHAQUE minute de circulation (pump_controller.cpp), donc le flush périodique
  // 30 s se déclenche tant que l'eau tourne — de l'ordre de 420 à 600
  // transactions `pool-daily` par jour contre 70 à 100 auparavant, soit un
  // facteur 5 à 6. C'est le prix ASSUMÉ de la cohérence : numérateur (mL) et
  // dénominateur (minutes) sont persistés ENSEMBLE, si bien qu'une coupure ne
  // perd au plus qu'une minute de CHACUN — le rapport mL/min, lui, est préservé.
  // L'usure reste très en deçà des 100 000 cycles garantis d'un secteur, la NVS
  // étant par ailleurs à nivellement d'usure et ignorant les réécritures à
  // valeur identique.
  prefs.putUInt("filt_min", safetyLimits.dailyFiltrationMinutes);
  // v2.34.0 (feature-093, R3) : compteur MONOTONE de filtration — base de temps
  // de la fraîcheur de l'instantané d'intégrale pH. Même transaction, aucun
  // coût NVS supplémentaire (réécriture à valeur identique ignorée par la NVS).
  prefs.putUInt("filt_total", safetyLimits.filtTotalMin);
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
  // v2.27.0 : absente d'une NVS antérieure → 0 (la journée en cours comptera
  // moins de minutes qu'elle n'en a vécu — jamais plus).
  //
  // AU-DELÀ DE 24 h la valeur est IMPOSSIBLE (le compteur sature à 1440) :
  // l'enregistrement est corrompu. Il en est fait DEUX usages, qui appellent deux
  // traitements opposés — chacun choisi du côté fail-closed, c'est-à-dire du côté
  // qui SOUS-estime la charge observée :
  //   - journée CLOSE à archiver → sentinelle 0 : la journée devient NON
  //     ARCHIVABLE (phLoadDayUsable la rejette), elle ne pollue pas la médiane ;
  //     l'écrêter à 1440 fabriquerait au contraire un échantillon de toutes
  //     pièces ;
  //   - journée EN COURS à reprendre → dénominateur SATURÉ à 1440 : la journée
  //     est déjà « pleine », donc sa charge sera minimale. La mettre à zéro
  //     rouvrirait exactement le défaut que corrige le comptage — un numérateur
  //     complet (les mL, eux, sont restaurés) sur un dénominateur tronqué, soit
  //     une charge SURÉVALUÉE.
  uint32_t filtMin = prefs.getUInt("filt_min", 0);
  // v2.34.0 (feature-093, R3) : compteur MONOTONE, relu QUELLE QUE SOIT la date
  // et l'état de l'horloge — il ne se remet jamais à zéro. Absent d'une NVS
  // antérieure → 0 : le premier instantané écrit portera 0 et l'âge en filtration
  // se calculera normalement à partir de là (perte d'une restauration au premier
  // boot après mise à jour, conséquence acceptée par la spec).
  safetyLimits.filtTotalMin = prefs.getUInt("filt_total", 0);
  prefs.end();
  const bool filtMinCorrupt = (filtMin > 1440UL);
  const uint32_t filtMinRunning = filtMinCorrupt ? 1440UL : filtMin;
  const uint32_t filtMinArchive = filtMinCorrupt ? 0UL : filtMin;

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
      safetyLimits.dailyFiltrationMinutes = filtMinRunning;
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      systemLogger.info("[Config] Compteurs journaliers restaurés: pH=" + String(phMl, 0) + " mL, ORP=" + String(orpMl, 0) + " mL");
    } else {
      // v2.27.0 (feature-084, R1) : ce basculement de date EST une clôture de
      // journée — l'appareil était éteint à minuit. La journée `savedDate`
      // rejoint donc l'anneau de charge, avec le volume et les minutes qui
      // avaient été persistés pour elle. C'est le second des DEUX points de
      // basculement existants (l'autre est tickDailyRollover) ; aucun n'est
      // créé pour l'occasion, et phLoadHistoryAppend déduplique si les deux
      // clôturent la même date.
      recordPhLoadDay(savedDate.c_str(), phMl, filtMinArchive,
                      phSustainableFlowFromConfig());
      safetyLimits.dailyFiltrationMinutes = 0;
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      saveDailyCounters();
      systemLogger.info("[Config] Nouveau jour détecté — compteurs remis à 0");
    }
  } else {
    // Heure non synchronisée : restore conservatif (sécurité)
    safetyLimits.dailyPhInjectedMl  = phMl;
    safetyLimits.dailyOrpInjectedMl = orpMl;
    safetyLimits.dailyFiltrationMinutes = filtMinRunning;  // journée reprise, pas close
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
  // feature-146 : le Boost modifie le créneau de filtration (surchloration
  // dont la filtration forcée n'est que le SUPPORT) — un mode Arrêt n'a pas
  // de circulation, donc pas de dosage possible. Autorisé dans tous les
  // autres modes (Auto, Programme, Marche). Entonnoir unique (HTTP, MQTT, UI).
  // (revue code-reviewer) Gardé par installMode == ManagedFiltration : en
  // Powered/External (feature-056) le mode de filtration est INERTE — un
  // "off" résiduel ne doit pas interdire le Boost alors que l'eau circule 24/7.
  if (mqttCfg.installMode == InstallMode::ManagedFiltration &&
      parseFiltrationModeFrom(filtrationCfg.mode) == FiltrationMode::Arret) {
    systemLogger.warning("[Boost] Activation refusée : filtration à l'arrêt (mode Arrêt)");
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
  // feature-140 : le mode EFFECTIF, pas la NVS. Avec la chloration de secours
  // active, la NVS peut encore dire "automatic" alors que le dosage est passé en
  // programmé — le Boost est alors inerte sur le chlore SANS que ce log le dise.
  // L'utilisateur armerait le Boost en croyant surchlorer, sans aucun signal.
  // C'est précisément l'honnêteté d'effet que feature-055 a instaurée.
  if (mqttCfg.orpRegulationMode != "automatic" || isRescueChlorineActive(time(nullptr))) {
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

// =============================================================================
// Chloration de secours (feature-140) — surcouche temporaire du mode scheduled
// =============================================================================
// Persistance NVS DÉDIÉE (namespace poolctrl, clés rescue_active/rescue_until) —
// écriture ciblée, même motif que le Boost (éviter l'usure d'une réécriture
// complète de la config générale).

void saveRescueState() {
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.warning("[Config] saveRescueState: mutex timeout — état secours non persisté");
    return;
  }
  Preferences prefs;
  if (!prefs.begin("poolctrl", false)) {
    systemLogger.error("Échec ouverture NVS pour état chloration de secours");
    if (configMutex) xSemaphoreGiveRecursive(configMutex);
    return;
  }
  prefs.putBool("rescue_active", rescueState.active);
  prefs.putLong64("rescue_until", (int64_t)rescueState.untilEpoch);
  // feature-141 : volume quotidien ARMÉ, MÊME transaction NVS que les deux
  // champs ci-dessus (même prefs.begin/end). uint16 : 0-65535, largement
  // suffisant (plafond dur 2000) — 0 = absent, convention lue par
  // resolveRescueDailyMlPure (cf. loadRescueState ci-dessous).
  prefs.putUShort("rescue_ml", (uint16_t)lroundf(rescueState.dailyMl));
  prefs.end();
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
}

void loadRescueState() {
  Preferences prefs;
  if (!prefs.begin("poolctrl", true)) {
    return;  // Pas de config, secours inactif par défaut
  }
  rescueState.active = prefs.getBool("rescue_active", false);
  rescueState.untilEpoch = (time_t)prefs.getLong64("rescue_until", 0);
  rescueState.dailyMl = (float)prefs.getUShort("rescue_ml", 0);
  prefs.end();

  // Condition #2 du gate pool-chemistry (feature-141, 2026-10-05) : absent
  // (0, sentinelle), nul ou hors bornes ⇒ repli sur la CHAÎNE de
  // pré-remplissage, JAMAIS sur 0 (« un secours armé à 0 mL est un mode qui
  // ment »). Étage taille de bassin INCLUS : ceci est une RESTAURATION
  // générique au boot, pas le chemin MQTT sans volume (invariant feature-090,
  // cf. startRescueChlorine) — rescueDailyMlForDisplay() est le seul lecteur
  // de la table par taille, réutilisé ici tel quel.
  rescueState.dailyMl = rescueDailyMlForDisplay();

  // Condition #6 du gate (fail-closed) : on NE force PAS rescueState.active à
  // false ici si l'heure est invalide — isRescueChlorineActive() le traduit
  // déjà en « inactif » tant que l'heure n'est pas synchronisée, et redevient
  // vrai de lui-même dès le retour de l'heure si l'échéance n'est pas dépassée
  // (NTP reprend son cours normalement en quelques secondes au boot).
  time_t now = time(nullptr);
  if (rescueState.active && now >= kMinValidEpoch && now >= rescueState.untilEpoch) {
    rescueState.active = false;
    rescueState.untilEpoch = 0;
    systemLogger.info("[Secours] État chargé expiré (48h dépassées) — chloration de secours inactive");
  } else if (rescueState.active) {
    systemLogger.info("[Secours] État restauré : chloration de secours active jusqu'à epoch " +
                       String((long)rescueState.untilEpoch) + " (volume " +
                       String(rescueState.dailyMl, 0) + " mL/j)");
  }
}

// feature-141 : volume quotidien de secours à AFFICHER — SEUL lecteur de
// kRescueChlorineDailyBySizeMl dans tout le firmware (voir config.h). Résout
// rescueState.dailyMl courant (ARMÉ si actif, dernier stocké ou 0 sinon) via
// la chaîne de pré-remplissage, étage taille INCLUS.
float rescueDailyMlForDisplay() {
  const float sizeDefault = kRescueChlorineDailyBySizeMl[static_cast<uint8_t>(mqttCfg.poolSize)];
  return resolveRescueDailyMlPure(rescueState.dailyMl,
                                   static_cast<float>(mqttCfg.orpDailyTargetMl),
                                   sizeDefault, kRescueChlorineDailyMl,
                                   safetyLimits.maxChlorineMlPerDay,
                                   kRescueChlorineDailyHardCapMl);
}

// Complément feature-140 (2026-10-05) : raison de FIN publiée à parité
// /data ↔ WS (`rescue_lift`), RAM UNIQUEMENT — voir RescueLiftReason, config.h.
// Statiques de TRADUCTION (fichier), pas de la persistance : aucun lien avec
// RescueState ni la NVS, jamais lues par save/loadRescueState.
static uint8_t sRescueLiftReason = 0;
static unsigned long sRescueLiftReasonAtMs = 0;

static void setRescueLiftReason(RescueLiftReason reason) {
  sRescueLiftReason = static_cast<uint8_t>(reason);
  sRescueLiftReasonAtMs = millis();
}

uint8_t getRescueLiftReasonCode(unsigned long nowMs) {
  // Délègue entièrement à temp_mode_logic.h (PUR), testé nativement (D1,
  // test-engineer feature-140) : ici, uniquement la collecte des deux statiques
  // RAM ci-dessus. Comportement INCHANGÉ (même formule, même idiome
  // qu'avant extraction — _rescueStableSinceMs, pump_controller.cpp).
  // uint32_t explicite (voir temp_mode_logic.h) : `unsigned long` EST déjà un
  // uint32_t sur ESP32 (cible de ce fichier) — conversion sans perte.
  return rescueLiftReasonVisiblePure(sRescueLiftReason, (uint32_t)nowMs,
                                      (uint32_t)sRescueLiftReasonAtMs,
                                      (uint32_t)kRescueLiftReasonVisibleMs);
}

void startRescueChlorine(float requestedDailyMl) {
  // Condition #5 du gate : mode Manuel incompatible (double dosage — "je dose
  // moi-même" + injection aveugle). Lu en NVS : à l'activation, le secours
  // n'est pas encore actif, donc mode EFFECTIF == mode NVS (effectiveOrpModePure
  // avec rescueActive=false renvoie nvsMode inchangé).
  if (mqttCfg.orpRegulationMode == "manual") {
    systemLogger.warning("[Secours] Activation refusée : régulation du chlore en mode Manuel");
    return;
  }
  time_t now = time(nullptr);
  // Condition #6 : sans heure synchronisée, l'échéance 48h ne peut être
  // calculée de façon fiable → refus (fail-closed), comme le Boost (minuit).
  if (now < kMinValidEpoch) {
    systemLogger.warning("[Secours] Activation refusée : heure non synchronisée (échéance 48h indéterminable)");
    return;
  }

  // feature-141 : résolution du volume quotidien, SOURCE selon ce que
  // l'appelant a fourni.
  float resolvedMl;
  const char* provenance;
  if (requestedDailyMl > 0.0f) {
    // Valeur EXPLICITEMENT saisie (route HTTP ?ml=) : seulement BORNÉE, jamais
    // réinterprétée par la chaîne — 10 mL saisis redeviennent 100, pas un défaut.
    resolvedMl = clampRescueDailyMlPure(requestedDailyMl, safetyLimits.maxChlorineMlPerDay,
                                         kRescueChlorineDailyHardCapMl);
    provenance = "saisi";
  } else {
    // Aucune valeur fournie (commande MQTT ON, ou ?ml= absent/non numérique) :
    // chaîne SANS étage taille de bassin — invariant feature-090, PoolSize
    // n'est consultée QUE par rescueDailyMlForDisplay() (pré-remplissage du
    // dialogue), jamais sur ce chemin de résolution serveur. Décision tranchée
    // du 2026-10-05 : pas d'héritage de la dernière valeur saisie (storedMl=0
    // force la chaîne, sizeDefaultMl=0 saute l'étage taille).
    resolvedMl = resolveRescueDailyMlPure(0.0f, static_cast<float>(mqttCfg.orpDailyTargetMl),
                                           0.0f, kRescueChlorineDailyMl,
                                           safetyLimits.maxChlorineMlPerDay,
                                           kRescueChlorineDailyHardCapMl);
    provenance = (mqttCfg.orpDailyTargetMl > 0) ? "cible programmée" : "défaut";
  }

  rescueState.untilEpoch = rescueDeadlinePure(now, kRescueChlorineDurationS);
  rescueState.active = true;
  rescueState.dailyMl = resolvedMl;
  saveRescueState();
  // Une nouvelle activation efface toute raison de fin encore visible d'un
  // cycle précédent (ré-armement rapproché) : le secours est de nouveau actif,
  // il n'y a plus de "fin récente" à rapporter.
  setRescueLiftReason(RescueLiftReason::None);
  systemLogger.info("[Secours] Chloration de secours activée jusqu'à epoch " +
                     String((long)rescueState.untilEpoch) + " (48h, volume " +
                     String(resolvedMl, 0) + " mL/j, provenance : " + provenance + ")");
  // feature-141 (recommandation gate 2) : SEUL cas où le mode s'arme sous le
  // plancher d'efficacité SANS aucun signal. La limite journalière de
  // l'utilisateur est souveraine — c'est une borne d'EXPOSITION, le plancher de
  // 100 une borne d'EFFICACITÉ, et la première l'emporte (refuser l'armement
  // serait pire : la boucle ORP doserait alors sur une mesure reconnue non
  // fiable, sans plafond de maintien). Mais le toast de clamp côté interface ne
  // se déclenche pas ici, saisie et valeur retenue étant déjà égales : sans ce
  // warning, un secours qui ne désinfecte pas s'armerait en silence.
  if (resolvedMl < kRescueChlorineDailyMinMl) {
    systemLogger.warning("[Secours] Volume armé " + String(resolvedMl, 0) +
                         " mL/j SOUS le plancher d'efficacité " +
                         String(kRescueChlorineDailyMinMl, 0) +
                         " mL/j — imposé par votre limite journalière. La "
                         "désinfection maintenue sera partielle.");
  }
}

void stopRescueChlorine(RescueLiftReason reason) {
  rescueState.active = false;
  rescueState.untilEpoch = 0;
  saveRescueState();
  setRescueLiftReason(reason);
  switch (reason) {
    case RescueLiftReason::AutoLift:
      // Pas de log ici : PumpControllerClass::tickRescueChlorineAutoLift()
      // émet DÉJÀ le libellé L6 (mot à mot, imposé par le gate) juste avant
      // d'appeler cette fonction — un second log serait redondant.
      break;
    case RescueLiftReason::Expired48h:
      systemLogger.info("[Secours] Chloration de secours expirée (48h écoulées) — retour au mode configuré");
      break;
    case RescueLiftReason::Manual:
    default:
      systemLogger.info("[Secours] Chloration de secours désactivée");
      break;
  }
}

bool isRescueChlorineActive(time_t now) {
  return rescueActivePure(rescueState.active, now, rescueState.untilEpoch, kMinValidEpoch);
}

void tickRescueChlorineExpiry(time_t now) {
  // Expire la chloration de secours 48h après activation. Mutualisé avec
  // tickDailyRollover (appelé à chaque tour), SANS lien avec le passage de
  // minuit — la durée du secours est glissante (condition #1 du gate).
  if (rescueState.active && now >= kMinValidEpoch && now >= rescueState.untilEpoch) {
    stopRescueChlorine(RescueLiftReason::Expired48h);
  }
}
