#include "pump_controller.h"
#include "config.h"
#include "logger.h"
#include "sensors.h"
#include <esp_task_wdt.h>

PumpControllerClass PumpController;

PumpControllerClass::PumpControllerClass() {
  pumps[0] = {PUMP1_PWM_PIN, PUMP1_CHANNEL};
  pumps[1] = {PUMP2_PWM_PIN, PUMP2_CHANNEL};
}

void PumpControllerClass::begin() {
  for (int i = 0; i < 2; ++i) {
    // MOSFET IRLZ44N: Configuration PWM sur Gate
    // Logic-level MOSFET compatible 3.3V ESP32
    ledcSetup(pumps[i].channel, PUMP_PWM_FREQ, PUMP_PWM_RES_BITS);
    ledcAttachPin(pumps[i].pwmPin, pumps[i].channel);
    ledcWrite(pumps[i].channel, 0);  // Pompe arrêtée au démarrage
  }
  systemLogger.info("Contrôleur de pompes MOSFET IRLZ44N initialisé");
}

void PumpControllerClass::applyPumpDuty(int index, uint8_t duty) {
  duty = duty > MAX_PWM_DUTY ? MAX_PWM_DUTY : duty;
  if (pumpDuty[index] == duty) return;

  pumpDuty[index] = duty;

  // MOSFET IRLZ44N: Contrôle simple via PWM sur Gate
  // duty=0 → MOSFET OFF (pompe arrêtée)
  // duty>0 → MOSFET ON proportionnel (pompe tourne)
  ledcWrite(pumps[index].channel, duty);
}

void PumpControllerClass::refreshDosingState(DosingState& state, unsigned long now) {
  if (state.windowStart == 0) {
    state.windowStart = now;
    state.lastTimestamp = now;
  }

  // Réinitialiser la fenêtre toutes les heures
  if (now - state.windowStart >= 3600000UL) {
    state.windowStart = now;
    state.usedMs = 0;
  }

  // Accumuler le temps d'injection
  if (state.active) {
    unsigned long delta = now - state.lastTimestamp;
    state.usedMs += delta;
    if (state.usedMs > 3600000UL) state.usedMs = 3600000UL;
  }

  state.lastTimestamp = now;
}


bool PumpControllerClass::shouldStartDosing(float error, float startThreshold, DosingState& state, unsigned long now) {
  // Réinitialiser le compteur de cycles chaque jour
  if (state.cyclesDayStart == 0 || now - state.cyclesDayStart >= 86400000UL) {
    state.cyclesToday = 0;
    state.cyclesDayStart = now;
  }

  // 1. Vérifier le nombre de cycles par jour
  if (state.cyclesToday >= pumpProtection.maxCyclesPerDay) {
    static unsigned long lastWarning = 0;
    if (now - lastWarning > 3600000) {  // Log toutes les heures
      systemLogger.warning("Limite cycles atteinte: " + String(state.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay));
      lastWarning = now;
    }
    return false;
  }

  // 2. Vérifier la pause minimum entre injections
  if (state.lastStopTime > 0) {
    unsigned long timeSinceStop = now - state.lastStopTime;
    if (timeSinceStop < pumpProtection.minPauseBetweenMs) {
      return false;  // Trop tôt pour redémarrer
    }
  }

  // 3. Vérifier le seuil de démarrage (hystérésis)
  if (error > startThreshold) {
    return true;
  }

  return false;
}

bool PumpControllerClass::shouldContinueDosing(float error, float stopThreshold, DosingState& state, unsigned long now) {
  // 1. Forcer le temps minimum d'injection
  if (state.lastStartTime > 0) {
    unsigned long runTime = now - state.lastStartTime;
    if (runTime < pumpProtection.minInjectionTimeMs) {
      return true;  // Force à continuer minimum 30s
    }
  }

  // 2. Continuer si au-dessus du seuil d'arrêt (hystérésis)
  if (error > stopThreshold) {
    return true;
  }

  return false;  // Arrêter
}

float PumpControllerClass::computePID(PIDController& pid, float error, unsigned long now) {
  if (pid.lastTime == 0) {
    pid.lastTime = now;
    pid.lastError = error;
    return 0.0f;
  }

  float dt = (now - pid.lastTime) / 1000.0f; // en secondes
  if (dt <= 0.0f || dt > 10.0f) { // Éviter les divisions par zéro et les deltas aberrants
    pid.lastTime = now;
    return 0.0f;
  }

  // Calcul PID
  float proportional = pid.kp * error;

  pid.integral += error * dt;
  // Anti-windup
  if (pid.integral > pid.integralMax) pid.integral = pid.integralMax;
  if (pid.integral < -pid.integralMax) pid.integral = -pid.integralMax;
  float integralTerm = pid.ki * pid.integral;

  float derivative = pid.kd * (error - pid.lastError) / dt;

  float output = proportional + integralTerm + derivative;

  pid.lastError = error;
  pid.lastTime = now;

  return output > 0.0f ? output : 0.0f;
}

float PumpControllerClass::computeFlowFromError(float error, float deadband, const PumpControlParams& params) {
  float delta = error - deadband;
  if (delta <= 0.0f) return 0.0f;

  float normalized = delta / params.maxError;
  if (normalized < 0.0f) normalized = 0.0f;
  if (normalized > 1.0f) normalized = 1.0f;

  return params.minFlowMlPerMin + normalized * (params.maxFlowMlPerMin - params.minFlowMlPerMin);
}

uint8_t PumpControllerClass::flowToDuty(const PumpControlParams& params, float flowMlPerMin) {
  if (flowMlPerMin <= 0.0f) return 0;
  if (flowMlPerMin < params.minFlowMlPerMin) flowMlPerMin = params.minFlowMlPerMin;
  if (flowMlPerMin > params.maxFlowMlPerMin) flowMlPerMin = params.maxFlowMlPerMin;

  float normalized = (flowMlPerMin - params.minFlowMlPerMin) / (params.maxFlowMlPerMin - params.minFlowMlPerMin);
  if (normalized < 0.0f) normalized = 0.0f;
  if (normalized > 1.0f) normalized = 1.0f;

  uint8_t duty = MIN_ACTIVE_DUTY + static_cast<uint8_t>(roundf(normalized * (MAX_PWM_DUTY - MIN_ACTIVE_DUTY)));
  if (duty > MAX_PWM_DUTY) duty = MAX_PWM_DUTY;

  return duty;
}

bool PumpControllerClass::checkSafetyLimits(bool isPhPump) {
  unsigned long now = millis();

  // Vérifier si on change de jour (toutes les 24h)
  if (safetyLimits.dayStartTimestamp == 0) {
    safetyLimits.dayStartTimestamp = now;
  }

  if (now - safetyLimits.dayStartTimestamp >= 86400000UL) { // 24h en ms
    // Nouveau jour, réinitialiser les compteurs
    safetyLimits.dailyPhInjectedMl = 0;
    safetyLimits.dailyOrpInjectedMl = 0;
    safetyLimits.phLimitReached = false;
    safetyLimits.orpLimitReached = false;
    safetyLimits.dayStartTimestamp = now;
    systemLogger.info("Réinitialisation compteurs journaliers de sécurité");
  }

  if (isPhPump) {
    if (safetyLimits.dailyPhInjectedMl >= safetyLimits.maxPhMinusMlPerDay) {
      if (!safetyLimits.phLimitReached) {
        systemLogger.critical("LIMITE JOURNALIÈRE pH- ATTEINTE: " + String(safetyLimits.dailyPhInjectedMl) + " ml");
        safetyLimits.phLimitReached = true;
      }
      return false;
    }
  } else {
    if (safetyLimits.dailyOrpInjectedMl >= safetyLimits.maxChlorineMlPerDay) {
      if (!safetyLimits.orpLimitReached) {
        systemLogger.critical("LIMITE JOURNALIÈRE CHLORE ATTEINTE: " + String(safetyLimits.dailyOrpInjectedMl) + " ml");
        safetyLimits.orpLimitReached = true;
      }
      return false;
    }
  }

  return true;
}

void PumpControllerClass::updateSafetyTracking(bool isPhPump, float flowMlPerMin, unsigned long deltaMs) {
  if (deltaMs == 0 || flowMlPerMin <= 0.0f) return;

  float injectedMl = (flowMlPerMin / 60000.0f) * deltaMs;

  if (isPhPump) {
    safetyLimits.dailyPhInjectedMl += static_cast<unsigned long>(injectedMl);
  } else {
    safetyLimits.dailyOrpInjectedMl += static_cast<unsigned long>(injectedMl);
  }
}

void PumpControllerClass::update() {
  unsigned long now = millis();

  if (otaInProgress) {
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    return;
  }

  refreshDosingState(phDosingState, now);
  refreshDosingState(orpDosingState, now);

  if (!sensors.isInitialized()) {
    phDosingState.active = false;
    orpDosingState.active = false;
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    return;
  }

  uint8_t desiredDuty[2] = {0, 0};
  bool phActive = false;
  bool orpActive = false;
  float phFlow = 0.0f;
  float orpFlow = 0.0f;

  // Calcul des limites d'injection avec accélération simulation
  int phLimitSec = mqttCfg.phInjectionLimitSeconds;
  if (phLimitSec < 0) phLimitSec = 0;
  unsigned long phLimitMs = static_cast<unsigned long>(phLimitSec) * 1000UL;

  int orpLimitSec = mqttCfg.orpInjectionLimitSeconds;
  if (orpLimitSec < 0) orpLimitSec = 0;
  unsigned long orpLimitMs = static_cast<unsigned long>(orpLimitSec) * 1000UL;

  bool phLimitOk = (phLimitMs == 0) || (phDosingState.usedMs < phLimitMs);
  bool orpLimitOk = (orpLimitMs == 0) || (orpDosingState.usedMs < orpLimitMs);

  // Vérifier les limites de sécurité journalières
  bool phSafetyOk = checkSafetyLimits(true);
  bool orpSafetyOk = checkSafetyLimits(false);

  // Contrôle pH
  if (mqttCfg.phEnabled && phLimitOk && phSafetyOk) {
    float phValue = sensors.getPh();
    float effectivePh = phValue;

    float error = effectivePh - mqttCfg.phTarget;
    
    // Déterminer si on doit doser (avec protection anti-cycling)
    bool shouldDose = false;
    
    if (phDosingState.active) {
      // Déjà en cours : vérifier si on continue
      shouldDose = shouldContinueDosing(error, pumpProtection.phStopThreshold, phDosingState, now);
    } else {
      // Arrêté : vérifier si on démarre
      shouldDose = shouldStartDosing(error, pumpProtection.phStartThreshold, phDosingState, now);
      
      if (shouldDose) {
        // Démarrage d'un nouveau cycle
        phDosingState.lastStartTime = now;
        phDosingState.cyclesToday++;
        systemLogger.info("Démarrage dosage pH (cycle " + String(phDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
      }
    }
    
    float flow = 0.0f;
    if (shouldDose) {
      // Calcul PID
      float pidOutput = computePID(phPID, error, now);
      flow = constrain(pidOutput, 0.0f, phPumpControl.maxFlowMlPerMin);
      
      // Appliquer un débit minimum si actif
      if (flow > 0.0f && flow < phPumpControl.minFlowMlPerMin) {
        flow = phPumpControl.minFlowMlPerMin;
      }
    } else {
      // Arrêt du dosage
      if (phDosingState.active) {
        phDosingState.lastStopTime = now;
        unsigned long runTime = (now - phDosingState.lastStartTime) / 1000;
        systemLogger.info("Arrêt dosage pH (durée: " + String(runTime) + "s)");
      }
      
      // Reset PID si erreur négative (pH trop bas)
      if (error < -pumpProtection.phStopThreshold) {
        phPID.integral = 0.0f;
        phPID.lastError = 0.0f;
        phPID.lastTime = 0;
      }
    }

    if (flow > 0.0f) {
      int index = pumpIndexFromNumber(mqttCfg.phPump);
      uint8_t duty = flowToDuty(phPumpControl, flow);
      if (duty > desiredDuty[index]) desiredDuty[index] = duty;
      if (duty > 0) {
        phActive = true;
        phFlow = flow;
      }
    }
  }

  // Contrôle ORP
  if (mqttCfg.orpEnabled && orpLimitOk && orpSafetyOk) {
    float orpValue = sensors.getOrp();
    float effectiveOrp = orpValue;

    float error = effectiveOrp - mqttCfg.orpTarget;
    
    // Déterminer si on doit doser (avec protection anti-cycling)
    bool shouldDose = false;
    
    if (orpDosingState.active) {
      // Déjà en cours : vérifier si on continue
      shouldDose = shouldContinueDosing(error, pumpProtection.orpStopThreshold, orpDosingState, now);
    } else {
      // Arrêté : vérifier si on démarre
      shouldDose = shouldStartDosing(error, pumpProtection.orpStartThreshold, orpDosingState, now);
      
      if (shouldDose) {
        // Démarrage d'un nouveau cycle
        orpDosingState.lastStartTime = now;
        orpDosingState.cyclesToday++;
        systemLogger.info("Démarrage dosage ORP (cycle " + String(orpDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
      }
    }
    
    float flow = 0.0f;
    if (shouldDose) {
      // Calcul PID
      float pidOutput = computePID(orpPID, error, now);
      flow = constrain(pidOutput, 0.0f, orpPumpControl.maxFlowMlPerMin);
      
      // Appliquer un débit minimum si actif
      if (flow > 0.0f && flow < orpPumpControl.minFlowMlPerMin) {
        flow = orpPumpControl.minFlowMlPerMin;
      }
    } else {
      // Arrêt du dosage
      if (orpDosingState.active) {
        orpDosingState.lastStopTime = now;
        unsigned long runTime = (now - orpDosingState.lastStartTime) / 1000;
        systemLogger.info("Arrêt dosage ORP (durée: " + String(runTime) + "s)");
      }
      
      // Reset PID si erreur négative (ORP trop haut)
      if (error < -pumpProtection.orpStopThreshold) {
        orpPID.integral = 0.0f;
        orpPID.lastError = 0.0f;
        orpPID.lastTime = 0;
      }
    }

    if (flow > 0.0f) {
      int index = pumpIndexFromNumber(mqttCfg.orpPump);
      uint8_t duty = flowToDuty(orpPumpControl, flow);
      if (duty > desiredDuty[index]) desiredDuty[index] = duty;
      if (duty > 0) {
        orpActive = true;
        orpFlow = flow;
      }
    }
  }

  // Appliquer les valeurs de duty (sauf pour les pompes en mode manuel)
  for (int i = 0; i < 2; ++i) {
    if (!manualMode[i]) {
      applyPumpDuty(i, desiredDuty[i]);
    }
  }

  // Mettre à jour le tracking de sécurité (ml injectés)
  // IMPORTANT: ne pas utiliser lastTimestamp (mis à jour par refreshDosingState), sinon delta≈0.
  if (phActive) {
    if (phDosingState.lastSafetyTimestamp == 0) {
      phDosingState.lastSafetyTimestamp = now;
    }
    unsigned long delta = now - phDosingState.lastSafetyTimestamp;
    updateSafetyTracking(true, phFlow, delta);
    phDosingState.lastSafetyTimestamp = now;
  } else {
    // Éviter de compter une longue période OFF au prochain démarrage
    phDosingState.lastSafetyTimestamp = 0;
  }

  if (orpActive) {
    if (orpDosingState.lastSafetyTimestamp == 0) {
      orpDosingState.lastSafetyTimestamp = now;
    }
    unsigned long delta = now - orpDosingState.lastSafetyTimestamp;
    updateSafetyTracking(false, orpFlow, delta);
    orpDosingState.lastSafetyTimestamp = now;
  } else {
    orpDosingState.lastSafetyTimestamp = 0;
  }

  phDosingState.active = phActive;
  orpDosingState.active = orpActive;
}

void PumpControllerClass::stopAll() {
  applyPumpDuty(0, 0);
  applyPumpDuty(1, 0);
  systemLogger.warning("Arrêt d'urgence de toutes les pompes");
}

void PumpControllerClass::setOtaInProgress(bool inProgress) {
  if (otaInProgress == inProgress) return;
  otaInProgress = inProgress;

  if (otaInProgress) {
    unsigned long now = millis();
    manualMode[0] = false;
    manualMode[1] = false;
    phDosingState.active = false;
    orpDosingState.active = false;
    phDosingState.lastStopTime = now;
    orpDosingState.lastStopTime = now;
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    systemLogger.warning("Arrêt pompes dosage (OTA en cours)");
  }
}

void PumpControllerClass::setPhPID(float kp, float ki, float kd) {
  phPID.kp = kp;
  phPID.ki = ki;
  phPID.kd = kd;
  systemLogger.info("PID pH configuré: Kp=" + String(kp) + " Ki=" + String(ki) + " Kd=" + String(kd));
}

void PumpControllerClass::setOrpPID(float kp, float ki, float kd) {
  orpPID.kp = kp;
  orpPID.ki = ki;
  orpPID.kd = kd;
  systemLogger.info("PID ORP configuré: Kp=" + String(kp) + " Ki=" + String(ki) + " Kd=" + String(kd));
}

void PumpControllerClass::resetDosingStates() {
  phDosingState = {};
  orpDosingState = {};
  phPID = {};
  orpPID = {};
  systemLogger.info("États de dosage réinitialisés");
}

void PumpControllerClass::setManualPump(int pumpIndex, uint8_t duty) {
  if (pumpIndex < 0 || pumpIndex >= 2) {
    systemLogger.error("Index de pompe invalide: " + String(pumpIndex));
    return;
  }

  duty = duty > MAX_PWM_DUTY ? MAX_PWM_DUTY : duty;

  // Activer/désactiver le mode manuel
  manualMode[pumpIndex] = (duty > 0);

  applyPumpDuty(pumpIndex, duty);

  if (duty > 0) {
    systemLogger.info("Test manuel pompe " + String(pumpIndex + 1) + " activée (duty=" + String(duty) + ")");
  } else {
    systemLogger.info("Test manuel pompe " + String(pumpIndex + 1) + " désactivée");
  }
}
