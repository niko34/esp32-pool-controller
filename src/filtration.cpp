#include "filtration.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "mqtt_manager.h"
#include "uart_protocol.h"
#include "pump_controller.h"
#include "schedule_logic.h"
#include <time.h>

// Formate des minutes depuis minuit en chaîne "HH:MM" (helper local coquille).
static String minutesToTimeString(int minutes) {
  int h = (minutes / 60) % 24;
  int m = minutes % 60;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", h, m);
  return String(buffer);
}

FiltrationManager filtration;

void FiltrationManager::begin() {
  pinMode(kFiltrationRelayPin, OUTPUT);
  ensureTimesValid();

  // Calculer et appliquer l'état initial du relais immédiatement.
  // Pré-requis satisfaits dans main.cpp : config et heure RTC chargées avant cet appel.
  // Cela évite une coupure prolongée de la pompe de filtration au reboot :
  // sans ce calcul, le relais resterait LOW pendant tout le setup (WiFi inclus, jusqu'à ~18s)
  // avant que filtration.update() dans loop() ne le remette dans son état correct.
  // feature-056 : le relais n'est piloté que dans le mode ManagedFiltration.
  bool initialState = false;
  if (mqttCfg.installMode == InstallMode::ManagedFiltration) {
    String mode = filtrationCfg.mode;
    mode.toLowerCase();
    if (mode != "off") {
      if (filtrationCfg.forceOn) {
        initialState = true;
      } else if (!filtrationCfg.forceOff && (mode == "manual" || mode == "auto")) {
        int nowMinutes = 0;
        if (getCurrentMinutesOfDay(nowMinutes)) {
          int startMin = timeStringToMinutes(filtrationCfg.start);
          int endMin = timeStringToMinutes(filtrationCfg.end);
          initialState = isMinutesInRange(nowMinutes, startMin, endMin);
        }
      }
    }
  }

  digitalWrite(kFiltrationRelayPin, initialState ? HIGH : LOW);
  relayState = initialState;
  state.running = initialState;
  if (initialState) {
    state.startedAtMs = millis();
    systemLogger.info("Gestionnaire de filtration initialisé (filtration maintenue active)");
  } else {
    systemLogger.info("Gestionnaire de filtration initialisé");
  }
}

void FiltrationManager::ensureTimesValid() {
  String modeLower = filtrationCfg.mode;
  modeLower.toLowerCase();
  if (modeLower != "auto" && modeLower != "manual" && modeLower != "off" && modeLower != "force") {
    filtrationCfg.mode = "auto";
  }

  auto normalize = [](String value, const char* fallback) {
    if (value.length() < 5 || value.charAt(2) != ':') return String(fallback);
    int hh = value.substring(0, 2).toInt();
    int mm = value.substring(3, 5).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return String(fallback);
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", hh, mm);
    return String(buffer);
  };

  filtrationCfg.start = normalize(filtrationCfg.start, "08:00");
  filtrationCfg.end = normalize(filtrationCfg.end, "20:00");
}

void FiltrationManager::computeAutoSchedule() {
  if (!filtrationCfg.mode.equalsIgnoreCase("auto")) return;

  float referenceTemp = sensors.getTemperature();
  if (isnan(referenceTemp)) return; // Pas de température valide, on conserve le planning actuel

  // Deadband 1°C : évite les recalculs et écritures NVS pour des variations mineures
  if (!isnan(_lastScheduledTemp) && fabsf(referenceTemp - _lastScheduledTemp) < 1.0f) return;

  // Calcul du créneau (logique pure) puis formatage en "HH:MM" pour la config.
  ScheduleWindow w = computeAutoWindow(referenceTemp, kFiltrationPivotHour);
  filtrationCfg.start = minutesToTimeString(w.startMin);
  filtrationCfg.end = minutesToTimeString(w.endMin);
  ensureTimesValid();
  _lastScheduledTemp = referenceTemp;
  systemLogger.info("Planning auto: " + String(referenceTemp, 1) + "°C → " + filtrationCfg.start + "-" + filtrationCfg.end);
}

bool FiltrationManager::getCurrentMinutesOfDay(int& minutes) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return true;
}

int FiltrationManager::timeStringToMinutes(const String& value) {
  // Délègue à la fonction pure (schedule_logic).
  return ::timeStringToMinutes(value.c_str());
}

bool FiltrationManager::isMinutesInRange(int now, int start, int end) {
  // Délègue à la fonction pure (schedule_logic).
  return ::isMinutesInRange(now, start, end);
}

void FiltrationManager::update() {
  // feature-056 : le relais filtration n'est piloté QUE dans le mode
  // « PoolController pilote la filtration » (ManagedFiltration). Dans les modes
  // PoweredByFiltration / ExternalFiltration, le relais reste INERTE (jamais
  // énergisé) et tout forçage (Boost / UI / MQTT) est SANS EFFET — signalé par un
  // warning throttlé. Le timer de stabilisation lié à la filtration est de facto
  // désactivé (le bloc d'arming ci-dessous n'est jamais atteint).
  if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
    if (filtrationCfg.forceOn || filtrationCfg.forceOff || isBoostActive(time(nullptr))) {
      static unsigned long sLastForceWarnMs = 0;
      unsigned long nowMs = millis();
      if (sLastForceWarnMs == 0 || nowMs - sLastForceWarnMs >= kMutexTimeoutWarnThrottleMs) {
        systemLogger.warning("[Filtration] Forçage ignoré : relais non piloté dans ce mode d'installation");
        sLastForceWarnMs = nowMs;
      }
    }
    if (state.running || relayState) {
      state.running = false;
      state.scheduleComputedThisCycle = false;
      state.startedAtMs = 0;
      if (relayState) {
        digitalWrite(kFiltrationRelayPin, LOW);
        relayState = false;
        publishState();
      }
    }
    return;
  }

  // Expiration des forcages après 4 heures (sécurité si l'utilisateur oublie de désactiver)
  constexpr unsigned long kForceTimeoutMs = 4UL * 3600000UL;

  if (filtrationCfg.forceOn) {
    if (state.forceOnStartMs == 0) state.forceOnStartMs = millis();
    if (millis() - state.forceOnStartMs >= kForceTimeoutMs) {
      filtrationCfg.forceOn = false;
      state.forceOnStartMs = 0;
      systemLogger.warning("ForceOn expiré (timeout 4h), retour au mode normal");
    }
  } else {
    state.forceOnStartMs = 0;
  }

  if (filtrationCfg.forceOff) {
    if (state.forceOffStartMs == 0) state.forceOffStartMs = millis();
    if (millis() - state.forceOffStartMs >= kForceTimeoutMs) {
      filtrationCfg.forceOff = false;
      state.forceOffStartMs = 0;
      systemLogger.warning("ForceOff expiré (timeout 4h), retour au mode normal");
    }
  } else {
    state.forceOffStartMs = 0;
  }

  ensureTimesValid();
  int nowMinutes = 0;
  bool haveTime = getCurrentMinutesOfDay(nowMinutes);
  String mode = filtrationCfg.mode;
  mode.toLowerCase();

  int startMin = timeStringToMinutes(filtrationCfg.start);
  int endMin = timeStringToMinutes(filtrationCfg.end);

  // Décision marche/arrêt (logique pure). `mode` est déjà en minuscules.
  // feature-053 : le Mode Boost force la filtration (turnover maximal) via un
  // chemin DÉDIÉ, prioritaire et indépendant du forceOn/kForceTimeoutMs.
  bool boostForce = isBoostActive(time(nullptr));
  bool runTarget = decideFiltrationRun(boostForce, mode.c_str(), filtrationCfg.forceOn,
                                       filtrationCfg.forceOff, haveTime,
                                       nowMinutes, startMin, endMin,
                                       state.running);

  bool wasRunning = state.running;
  if (!wasRunning && runTarget) {
    state.running = true;
    state.startedAtMs = millis();
    state.scheduleComputedThisCycle = false;
    systemLogger.info("Démarrage filtration");
    // Mode ManagedFiltration (garanti ici) : armer le timer de stabilisation au
    // démarrage de la filtration. On n'arme le timer que si la filtration a été
    // arrêtée suffisamment longtemps (durée >= délai de stabilisation) pour éviter
    // les boucles sur un arrêt/redémarrage fugace (glitch, sauvegarde config, etc.).
    unsigned long pauseMs = (state.lastStoppedAtMs == 0)
                              ? 0xFFFFFFFFUL
                              : (millis() - state.lastStoppedAtMs);
    unsigned long stabilizationMs = (unsigned long)mqttCfg.stabilizationDelayMin * 60000UL;
    if (pauseMs > stabilizationMs) {
      PumpController.armStabilizationTimer();
    }
  }

  bool runNow = state.running;
  unsigned long elapsed = runNow ? millis() - state.startedAtMs : 0;

  // Après 5 min de filtration en mode auto, recalculer avec la température réelle
  // (l'eau stagnante dans les tuyauteries peut fausser la mesure initiale)
  if (runNow && mode == "auto" && elapsed >= 300000UL && !state.scheduleComputedThisCycle) {
    computeAutoSchedule();
    saveMqttConfig();
    state.scheduleComputedThisCycle = true;

    if (haveTime) {
      startMin = timeStringToMinutes(filtrationCfg.start);
      endMin = timeStringToMinutes(filtrationCfg.end);
      // Ne pas annuler un forceOn : si l'utilisateur a forcé le démarrage
      // (ex: avant la plage horaire calculée), le planning ne doit pas l'arrêter.
      if (!filtrationCfg.forceOn && !isMinutesInRange(nowMinutes, startMin, endMin)) {
        runTarget = false;
      }
    }
    publishState();
  }

  if (runNow && !runTarget) {
    state.running = false;
    state.scheduleComputedThisCycle = false;
    state.startedAtMs = 0;
    systemLogger.info("Arrêt filtration");
    state.lastStoppedAtMs = millis();
    PumpController.clearStabilizationTimer();
  } else {
    state.running = runTarget;
  }

  bool relayShouldBeOn = state.running && (filtrationCfg.mode != "off");
  if (relayShouldBeOn != relayState) {
    digitalWrite(kFiltrationRelayPin, relayShouldBeOn ? HIGH : LOW);
    relayState = relayShouldBeOn;
    publishState();
    if (authCfg.screenEnabled) uartProtocol.sendEventBool("event", "filtration_changed", "running", relayState);
  }
}

void FiltrationManager::publishState() {
  mqttManager.publishFiltrationState();
}

// =============================================================================
// feature-056 — état de la filtration externe (mode ExternalFiltration)
// =============================================================================

void FiltrationManager::setExternalState(bool running) {
  // Horodatage AVANT la section critique ; log APRÈS (section critique minimale).
  uint32_t now = (uint32_t)millis();
  portENTER_CRITICAL(&_externalMux);
  _externalOn = running;
  _externalLastMs = now;
  _externalKnown = true;
  portEXIT_CRITICAL(&_externalMux);
  systemLogger.info(String("[Filtration externe] État signalé : ") + (running ? "ON" : "OFF"));
}

void FiltrationManager::getExternalState(bool& on, uint32_t& lastMs, bool& known) const {
  portENTER_CRITICAL(&_externalMux);
  on = _externalOn;
  lastMs = _externalLastMs;
  known = _externalKnown;
  portEXIT_CRITICAL(&_externalMux);
}

WaterPresence FiltrationManager::resolveWaterPresence() const {
  WaterPresenceInputs in = {};
  in.mode = mqttCfg.installMode;
  in.filtrationCommandedOn = state.running;
  // Copie minimale du triplet externe sous lock ; âge calculé HORS lock,
  // wrap-safe (soustraction non signée, cohérente au passage 0xFFFFFFFF).
  bool on = false, known = false;
  uint32_t lastMs = 0;
  getExternalState(on, lastMs, known);
  uint32_t now = (uint32_t)millis();
  in.externalSignalOn = on;
  in.externalSignalKnown = known;
  in.externalSignalAgeMs = known ? (now - lastMs) : 0;
  in.externalStaleMs = kExternalFiltrationStaleMs;
  return resolveWaterPresent(in);
}
