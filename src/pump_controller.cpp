#include "pump_controller.h"
#include "config.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "uart_protocol.h"
#include <esp_task_wdt.h>

PumpControllerClass PumpController;

PumpControllerClass::PumpControllerClass() {
  pumps[0] = {PUMP1_PWM_PIN, PUMP1_CHANNEL};
  pumps[1] = {PUMP2_PWM_PIN, PUMP2_CHANNEL};
}

// Applique les paramètres PID selon la vitesse de régulation configurée
void PumpControllerClass::applyRegulationSpeed() {
  const String& speed = mqttCfg.regulationSpeed;
  if (speed == "slow") {
    phPID.kp = 3.0f;  phPID.ki = 0.05f;  phPID.kd = 12.0f;
    orpPID.kp = 3.0f; orpPID.ki = 0.05f; orpPID.kd = 12.0f;
  } else if (speed == "fast") {
    phPID.kp = 12.0f;  phPID.ki = 0.2f;  phPID.kd = 4.0f;
    orpPID.kp = 12.0f; orpPID.ki = 0.2f; orpPID.kd = 4.0f;
  } else {
    // "normal" (défaut)
    phPID.kp = 6.0f;  phPID.ki = 0.1f;  phPID.kd = 8.0f;
    orpPID.kp = 6.0f; orpPID.ki = 0.1f; orpPID.kd = 8.0f;
  }
  systemLogger.info("PID régulation: vitesse=" + speed +
    " Kp=" + String(phPID.kp, 1) +
    " Ki=" + String(phPID.ki, 2) +
    " Kd=" + String(phPID.kd, 1));
}

void PumpControllerClass::begin() {
  for (int i = 0; i < 2; ++i) {
    // MOSFET IRLZ44N: Configuration PWM sur Gate
    // Logic-level MOSFET compatible 3.3V ESP32
    ledcSetup(pumps[i].channel, PUMP_PWM_FREQ, PUMP_PWM_RES_BITS);
    ledcAttachPin(pumps[i].pwmPin, pumps[i].channel);
    ledcWrite(pumps[i].channel, 0);  // Pompe arrêtée au démarrage
  }
  applyRegulationSpeed();
  systemLogger.info("Contrôleur de pompes initialisé");
  systemLogger.info("Config pH: cible=" + String(mqttCfg.phTarget, 2) +
    " seuil=" + String(pumpProtection.phStartThreshold, 2) +
    " limite=" + String(mqttCfg.phInjectionLimitSeconds) + "s/h" +
    " max=" + String(safetyLimits.maxPhMinusMlPerDay, 0) + "mL/j");
  systemLogger.info("Config ORP: cible=" + String(mqttCfg.orpTarget, 0) + "mV" +
    " seuil=" + String(pumpProtection.orpStartThreshold, 0) + "mV" +
    " limite=" + String(mqttCfg.orpInjectionLimitSeconds) + "s/h" +
    " max=" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL/j");
  systemLogger.info("Pause inter-injections: " + String(mqttCfg.minPauseBetweenMin) + "min" +
    " puissance: P1=" + String(mqttCfg.pump1MaxDutyPct) + "% P2=" + String(mqttCfg.pump2MaxDutyPct) + "%");
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

void PumpControllerClass::resetPhPauseGuard() {
  // Demande différée : résolution dans update() sur la tâche loop (évite la race inter-core)
  _phPauseResetRequested.store(true);
}

void PumpControllerClass::armStabilizationTimer() {
  int delayMin = mqttCfg.stabilizationDelayMin;
  if (delayMin <= 0) {
    _stabilizationEndMs = 0;
    return;
  }
  _stabilizationEndMs = millis() + (unsigned long)delayMin * 60000UL;
  systemLogger.info(String("[Dosage] Stabilisation : injection suspendue ") + delayMin + " min");
}

void PumpControllerClass::clearStabilizationTimer() {
  _stabilizationEndMs = 0;
}

unsigned long PumpControllerClass::getStabilizationRemainingS() const {
  if (_stabilizationEndMs == 0) return 0;
  unsigned long now = millis();
  if (now >= _stabilizationEndMs) return 0;
  return (_stabilizationEndMs - now) / 1000UL;
}

bool PumpControllerClass::canDose() {
  // Délai de stabilisation en cours : bloquer toute injection
  if (_stabilizationEndMs > 0) {
    if (millis() < _stabilizationEndMs) return false;
    _stabilizationEndMs = 0;  // Timer expiré
    systemLogger.info("[Dosage] Stabilisation terminée, dosage autorisé");
  }
  // En mode continu : toujours OK (l'alimentation du contrôleur suit la filtration)
  if (mqttCfg.regulationMode == "continu") {
    return true;
  }
  // En mode piloté : uniquement si la filtration est active
  return filtration.isRunning();
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

float PumpControllerClass::dutyToFlow(const PumpControlParams& params, uint8_t duty) {
  if (duty < MIN_ACTIVE_DUTY) return 0.0f;
  float normalized = (float)(duty - MIN_ACTIVE_DUTY) / (MAX_PWM_DUTY - MIN_ACTIVE_DUTY);
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
        String corrType = (mqttCfg.phCorrectionType == "ph_plus") ? "pH+" : "pH-";
        systemLogger.critical("LIMITE JOURNALIÈRE " + corrType + " ATTEINTE: " + String(safetyLimits.dailyPhInjectedMl) + " ml");
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
    safetyLimits.dailyPhInjectedMl += injectedMl;
    if (productCfg.phTrackingEnabled) {
      productCfg.phTotalInjectedMl += injectedMl;
      productConfigDirty = true;
    }
  } else {
    safetyLimits.dailyOrpInjectedMl += injectedMl;
    if (productCfg.orpTrackingEnabled) {
      productCfg.orpTotalInjectedMl += injectedMl;
      productConfigDirty = true;
    }
  }
}

void PumpControllerClass::update() {
  unsigned long now = millis();

  // Résoudre les demandes de reset issues de tâches externes (web handlers)
  // Exécuté ici, sur la tâche loop, pour éviter toute race inter-core
  if (_resetRequested.exchange(false)) {
    phDosingState = {};
    orpDosingState = {};
    phPID = {};
    orpPID = {};
  }
  if (_phPauseResetRequested.exchange(false)) {
    phDosingState.lastStopTime = 0;
    phPID.integral = 0.0f;
    phPID.lastError = 0.0f;
    phPID.lastTime = 0;
  }

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

  // Vérifier si le dosage est autorisé (mode régulation + état filtration)
  if (!canDose()) {
    // Arrêter le dosage en cours si la filtration s'arrête
    if (phDosingState.active || orpDosingState.active) {
      systemLogger.info("Dosage suspendu (filtration arrêtée ou mode piloté)");
      phDosingState.active = false;
      orpDosingState.active = false;
    }
    // Ne pas arrêter les pompes en mode manuel (test développement)
    if (!manualMode[0]) applyPumpDuty(0, 0);
    if (!manualMode[1]) applyPumpDuty(1, 0);
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

  // Log une seule fois quand la limite horaire est atteinte
  static bool phWindowLimitLogged = false;
  if (!phLimitOk && phDosingState.active && !phWindowLimitLogged) {
    systemLogger.warning("Limite horaire pH atteinte: " + String(phLimitSec) + "s/h consommées — dosage suspendu jusqu'au prochain cycle");
    phWindowLimitLogged = true;
  } else if (phLimitOk) {
    phWindowLimitLogged = false;
  }

  static bool orpWindowLimitLogged = false;
  if (!orpLimitOk && orpDosingState.active && !orpWindowLimitLogged) {
    systemLogger.warning("Limite horaire ORP atteinte: " + String(orpLimitSec) + "s/h consommées — dosage suspendu jusqu'au prochain cycle");
    orpWindowLimitLogged = true;
  } else if (orpLimitOk) {
    orpWindowLimitLogged = false;
  }

  // Vérifier les limites de sécurité journalières
  bool phSafetyOk = checkSafetyLimits(true);
  bool orpSafetyOk = checkSafetyLimits(false);

  // Contrôle pH
  if (mqttCfg.phEnabled && phLimitOk && phSafetyOk) {
    float phValue = sensors.getPh();
    float effectivePh = phValue;

    // Calcul de l'erreur selon le type de correction
    // pH- (acide) : dose quand pH > cible → error = pH - cible (positive si pH trop haut)
    // pH+ (base)  : dose quand pH < cible → error = cible - pH (positive si pH trop bas)
    float error;
    if (mqttCfg.phCorrectionType == "ph_plus") {
      error = mqttCfg.phTarget - effectivePh;
    } else {
      error = effectivePh - mqttCfg.phTarget;
    }
    
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
        systemLogger.info("Démarrage dosage pH: pH=" + String(sensors.getPh(), 2) +
          " cible=" + String(mqttCfg.phTarget, 2) +
          " erreur=" + String(error, 3) +
          " (cycle " + String(phDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
      }
    }

    float flow = 0.0f;
    if (shouldDose) {
      // Calcul PID — constrain avec minimum pour éviter flow=0 à la première invocation du PID
      float pidOutput = computePID(phPID, error, now);
      flow = constrain(pidOutput, phPumpControl.minFlowMlPerMin, phPumpControl.maxFlowMlPerMin);
    } else {
      // Arrêt du dosage
      if (phDosingState.active) {
        phDosingState.lastStopTime = now;
        unsigned long runTime = (now - phDosingState.lastStartTime) / 1000;
        float volumeMl = (phFlow * runTime) / 60.0f;
        systemLogger.info("Arrêt dosage pH: durée=" + String(runTime) + "s" +
          " vol≈" + String(volumeMl, 1) + "mL" +
          " total jour=" + String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(safetyLimits.maxPhMinusMlPerDay, 0) + "mL" +
          " — pause " + String(mqttCfg.minPauseBetweenMin) + "min");
      }
      
      // Reset PID si erreur négative (pH hors plage de correction)
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

    // Erreur positive = ORP trop bas → injecter du chlore pour monter l'ORP
    float error = mqttCfg.orpTarget - effectiveOrp;

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
        systemLogger.info("Démarrage dosage ORP: ORP=" + String(sensors.getOrp(), 0) + "mV" +
          " cible=" + String(mqttCfg.orpTarget, 0) + "mV" +
          " erreur=" + String(error, 0) + "mV" +
          " (cycle " + String(orpDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
      }
    }

    float flow = 0.0f;
    if (shouldDose) {
      // Calcul PID — constrain avec minimum pour éviter flow=0 à la première invocation du PID
      float pidOutput = computePID(orpPID, error, now);
      flow = constrain(pidOutput, orpPumpControl.minFlowMlPerMin, orpPumpControl.maxFlowMlPerMin);
    } else {
      // Arrêt du dosage
      if (orpDosingState.active) {
        orpDosingState.lastStopTime = now;
        unsigned long runTime = (now - orpDosingState.lastStartTime) / 1000;
        float volumeMl = (orpFlow * runTime) / 60.0f;
        systemLogger.info("Arrêt dosage ORP: durée=" + String(runTime) + "s" +
          " vol≈" + String(volumeMl, 1) + "mL" +
          " total jour=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL" +
          " — pause " + String(mqttCfg.minPauseBetweenMin) + "min");
      }
      
      // Reset PID si erreur négative (ORP trop haut, au-dessus de la cible)
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

  // Appliquer la puissance maximale configurée (ne s'applique pas au mode test manuel)
  uint8_t maxDuty0 = (uint8_t)((mqttCfg.pump1MaxDutyPct * MAX_PWM_DUTY) / 100);
  uint8_t maxDuty1 = (uint8_t)((mqttCfg.pump2MaxDutyPct * MAX_PWM_DUTY) / 100);
  if (desiredDuty[0] > maxDuty0) desiredDuty[0] = maxDuty0;
  if (desiredDuty[1] > maxDuty1) desiredDuty[1] = maxDuty1;

  // Appliquer les valeurs de duty (sauf pour les pompes en mode manuel)
  for (int i = 0; i < 2; ++i) {
    if (!manualMode[i]) {
      applyPumpDuty(i, desiredDuty[i]);
    }
  }

  // Mettre à jour le tracking de sécurité (ml injectés)
  // Le flow est scalé par le ratio duty réel / duty demandé pour tenir compte
  // de la puissance maximale configurée (pump1MaxDutyPct / pump2MaxDutyPct).
  // IMPORTANT: ne pas utiliser lastTimestamp (mis à jour par refreshDosingState), sinon delta≈0.
  if (phActive) {
    if (phDosingState.lastSafetyTimestamp == 0) {
      phDosingState.lastSafetyTimestamp = now;
    }
    unsigned long delta = now - phDosingState.lastSafetyTimestamp;
    int phIdx = pumpIndexFromNumber(mqttCfg.phPump);
    float phEffectiveFlow = dutyToFlow(phPumpControl, desiredDuty[phIdx]);
    updateSafetyTracking(true, phEffectiveFlow, delta);
    phDosingState.lastSafetyTimestamp = now;
  } else {
    phDosingState.lastSafetyTimestamp = 0;
  }

  if (orpActive) {
    if (orpDosingState.lastSafetyTimestamp == 0) {
      orpDosingState.lastSafetyTimestamp = now;
    }
    unsigned long delta = now - orpDosingState.lastSafetyTimestamp;
    int orpIdx = pumpIndexFromNumber(mqttCfg.orpPump);
    float orpEffectiveFlow = dutyToFlow(orpPumpControl, desiredDuty[orpIdx]);
    updateSafetyTracking(false, orpEffectiveFlow, delta);
    orpDosingState.lastSafetyTimestamp = now;
  } else {
    orpDosingState.lastSafetyTimestamp = 0;
  }

  // Envoyer un événement UART si l'état de dosage a changé
  if (phDosingState.active != phActive || orpDosingState.active != orpActive) {
    if (authCfg.screenEnabled) uartProtocol.sendDosingEvent(phActive, orpActive);
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
  // Demande différée : résolution dans update() sur la tâche loop (évite la race inter-core)
  _resetRequested.store(true);
  systemLogger.info("États de dosage réinitialisés (demande)");
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
