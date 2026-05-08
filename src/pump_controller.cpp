#include "pump_controller.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "uart_protocol.h"
#include <esp_task_wdt.h>
#include <time.h>

PumpControllerClass PumpController;

// Définitions des membres statiques
bool PumpControllerClass::_dailyLoaded = false;
bool PumpControllerClass::_dailyCountersDirty = false;
unsigned long PumpControllerClass::_lastDailySaveMs = 0;
bool PumpControllerClass::_phWasActive = false;
bool PumpControllerClass::_orpWasActive = false;

PumpControllerClass::PumpControllerClass() {
  // Convention pool-chemistry : pumps[0] = pH, pumps[1] = ORP/chlore.
  pumps[0] = {kPumpPhPin,  PUMP1_CHANNEL};
  pumps[1] = {kPumpOrpPin, PUMP2_CHANNEL};
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
    // Logic-level MOSFET compatible 3.3V ESP32.
    // Force OUTPUT en amont de ledcAttachPin : sur PCB v2, GPIO25 (DAC1/ADC2)
    // et GPIO33 (ADC1) restent muets si LEDC est attaché sans pinMode préalable.
    pinMode(pumps[i].pwmPin, OUTPUT);
    digitalWrite(pumps[i].pwmPin, LOW);
    ledcSetup(pumps[i].channel, PUMP_PWM_FREQ, PUMP_PWM_RES_BITS);
    ledcAttachPin(pumps[i].pwmPin, pumps[i].channel);
    ledcWrite(pumps[i].channel, 0);  // Pompe arrêtée au démarrage
    systemLogger.info("Pompe " + String(i + 1) + " : GPIO=" + String(pumps[i].pwmPin) +
                      " canal LEDC=" + String(pumps[i].channel));
  }
  applyRegulationSpeed();
  systemLogger.info("Contrôleur de pompes initialisé");
  systemLogger.info("Config pH: cible=" + String(mqttCfg.phTarget, 2) +
    " seuil=" + String(pumpProtection.phStartThreshold, 2) +
    " limite=" + String(mqttCfg.phInjectionLimitMinutes) + "min/h" +
    " max=" + String(safetyLimits.maxPhMinusMlPerDay, 0) + "mL/j");
  systemLogger.info("Config ORP: cible=" + String(mqttCfg.orpTarget, 0) + "mV" +
    " seuil=" + String(pumpProtection.orpStartThreshold, 0) + "mV" +
    " limite=" + String(mqttCfg.orpInjectionLimitMinutes) + "min/h" +
    " max=" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL/j");
  systemLogger.info("Puissance: P1=" + String(mqttCfg.pump1MaxDutyPct) + "% P2=" + String(mqttCfg.pump2MaxDutyPct) + "%");
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

  // 2. Vérifier le seuil de démarrage (hystérésis)
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

// Arme le timer pour une pompe spécifique. Sélectionne la durée selon l'index :
//   - pH (0)  : kStabilizationDurationPhMs (post-cal) ou stabilizationDelayMin (override)
//   - ORP (1) : kStabilizationDurationOrpMs (post-cal) ou stabilizationDelayMin (override)
// Si l'utilisateur a configuré `mqttCfg.stabilizationDelayMin > 0`, son override prime
// (il a explicitement choisi une durée). Sinon, on applique la durée chimique par défaut.
void PumpControllerClass::armStabilizationTimer(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;

  int delayMin = mqttCfg.stabilizationDelayMin;
  unsigned long durationMs;
  if (delayMin > 0) {
    durationMs = (unsigned long)delayMin * 60000UL;
  } else {
    durationMs = (pumpIndex == 0) ? kStabilizationDurationPhMs
                                  : kStabilizationDurationOrpMs;
  }
  _stabilizationEndMs[pumpIndex] = millis() + durationMs;
  systemLogger.info(String("[Dosage] Stabilisation pompe ") +
                    (pumpIndex == 0 ? "pH" : "ORP") +
                    " : injection suspendue " + String(durationMs / 60000UL) + " min " +
                    String((durationMs % 60000UL) / 1000UL) + " s");
}

// Surcharge legacy : arme les 2 pompes simultanément (cas filtration / continu / minuit).
// Délègue à armStabilizationTimer(int) pour conserver le comportement par-pompe.
// Désactive le log info de la version par-pompe pour produire un seul log condensé.
void PumpControllerClass::armStabilizationTimer() {
  int delayMin = mqttCfg.stabilizationDelayMin;
  unsigned long durationPhMs  = (delayMin > 0) ? ((unsigned long)delayMin * 60000UL) : kStabilizationDurationPhMs;
  unsigned long durationOrpMs = (delayMin > 0) ? ((unsigned long)delayMin * 60000UL) : kStabilizationDurationOrpMs;
  uint32_t now = millis();
  _stabilizationEndMs[0] = now + durationPhMs;
  _stabilizationEndMs[1] = now + durationOrpMs;
  systemLogger.info(String("[Dosage] Stabilisation pompes pH+ORP : injection suspendue ") +
                    String(durationPhMs / 60000UL) + " min (pH) / " +
                    String(durationOrpMs / 60000UL) + " min (ORP)");
}

bool PumpControllerClass::isStabilizationTimerActive(int pumpIndex) const {
  if (pumpIndex < 0 || pumpIndex > 1) return false;
  uint32_t end = _stabilizationEndMs[pumpIndex];
  if (end == 0) return false;
  return millis() < end;
}

void PumpControllerClass::clearStabilizationTimer() {
  _stabilizationEndMs[0] = 0;
  _stabilizationEndMs[1] = 0;
}

unsigned long PumpControllerClass::getStabilizationRemainingS() const {
  unsigned long now = millis();
  unsigned long maxRem = 0;
  for (int i = 0; i < 2; ++i) {
    if (_stabilizationEndMs[i] == 0) continue;
    if (now >= _stabilizationEndMs[i]) continue;
    unsigned long rem = (_stabilizationEndMs[i] - now) / 1000UL;
    if (rem > maxRem) maxRem = rem;
  }
  return maxRem;
}

// Logge la cause de refus uniquement à la transition (1 seule entrée par changement
// de cause). Évite le spam quand canDose() est appelé chaque cycle de update().
void PumpControllerClass::logRefusalOnce(int pumpIndex, const String& cause) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  if (_lastRefusalCause[pumpIndex] != cause) {
    systemLogger.info(String("[Dosage ") + (pumpIndex == 0 ? "pH" : "ORP") +
                      "] Refus : " + cause);
    _lastRefusalCause[pumpIndex] = cause;
  }
}

void PumpControllerClass::resetRefusalLogState(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  _lastRefusalCause[pumpIndex] = "";
}

// Ring buffer anti-rafale (pool-chemistry Pass 3.5).
// Enregistre l'instant `millis()` du démarrage d'un cycle de dosage pour la
// pompe `pumpIndex`. Index circulaire borné par kDosingCycleHistorySize.
void PumpControllerClass::recordDosingCycleStart(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  uint32_t now = millis();
  _dosingCycleHistory[pumpIndex][_dosingCycleHistoryIdx[pumpIndex]] = now;
  _dosingCycleHistoryIdx[pumpIndex] =
      (_dosingCycleHistoryIdx[pumpIndex] + 1) % kDosingCycleHistorySize;
}

// Compte les démarrages de cycle survenus dans la fenêtre [now-windowMs, now]
// pour la pompe donnée. Itération O(kDosingCycleHistorySize) — négligeable
// (20 comparaisons) appelée au plus 2 fois par tour de update().
int PumpControllerClass::countRecentDosingCycles(int pumpIndex, uint32_t windowMs) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  uint32_t now = millis();
  int count = 0;
  for (size_t i = 0; i < kDosingCycleHistorySize; ++i) {
    uint32_t ts = _dosingCycleHistory[pumpIndex][i];
    // ts == 0 = slot vide (jamais utilisé). On ignore.
    // Cas wrap millis() (49.7 jours) : (now - ts) déborde correctement en
    // arithmétique non-signée → la fenêtre reste cohérente au passage 0xFFFFFFFF.
    if (ts != 0 && (now - ts) <= windowMs) count++;
  }
  return count;
}

// canDose(int) — fail-closed strict, ordre validé pool-chemistry feature-021.
// Toute condition non remplie → return false avec log edge-triggered.
bool PumpControllerClass::canDose(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return false;

  // 1. Watchdog actif (le plus critique — sans wdt, un crash dans la boucle
  // de dosage laisse la pompe en marche indéfiniment).
  // esp_task_wdt_status(NULL) retourne ESP_OK si la tâche courante est
  // inscrite au watchdog, ESP_ERR_NOT_FOUND sinon.
  if (esp_task_wdt_status(NULL) != ESP_OK) {
    logRefusalOnce(pumpIndex, "watchdog inactif");
    return false;
  }

  // 2. Filtration en marche : eau circule devant la sonde.
  // En mode continu (alimentation 24/7), on accepte sans filtration ;
  // sinon on impose le relais filtration ON.
  if (mqttCfg.regulationMode != "continu" && !filtration.isRunning()) {
    logRefusalOnce(pumpIndex, "filtration arrêtée");
    return false;
  }

  // 3. Cond #1/#5 pool-chemistry : lecture stale ou bus I²C dégradé → NaN.
  float reading = (pumpIndex == 0) ? sensors.getPh() : sensors.getOrp();
  if (isnan(reading)) {
    logRefusalOnce(pumpIndex, "lecture pH/ORP stale ou indisponible (NaN)");
    return false;
  }

  // 4. Cond #2 pool-chemistry : EZO calibré OU joignable.
  // Cal,? renvoie -1 si EZO injoignable → on bloque (fail-closed).
  // pH demande ≥ 2 points (mid + low), ORP ≥ 1 point.
  // Lecture du cache (pas d'I²C dans la boucle pump_controller : ce serait un
  // appel bloquant ~900 ms à chaque cycle update()).
  int calPoints = (pumpIndex == 0)
      ? sensors.getPhCalibrationPointsCached()
      : sensors.getOrpCalibrationPointsCached();
  int requiredPoints = (pumpIndex == 0) ? 2 : 1;
  if (calPoints < requiredPoints) {
    logRefusalOnce(pumpIndex, String("calibration insuffisante (cal=") +
                              String(calPoints) + ", requis=" +
                              String(requiredPoints) + ")");
    return false;
  }

  // 5. Cond #3 pool-chemistry : stabilisation post-cal en cours pour cette pompe.
  if (isStabilizationTimerActive(pumpIndex)) {
    logRefusalOnce(pumpIndex, "stabilisation post-calibration en cours");
    return false;
  }

  // 6. Mode régulation = automatic (les modes scheduled / manual sont gérés en amont
  // dans update(), mais on protège ici aussi par défense en profondeur).
  const String& mode = (pumpIndex == 0) ? mqttCfg.phRegulationMode
                                        : mqttCfg.orpRegulationMode;
  if (mode != "automatic") {
    logRefusalOnce(pumpIndex, String("mode régulation != automatic (") + mode + ")");
    return false;
  }

  // 7. Limite journalière non atteinte.
  if (pumpIndex == 0) {
    if (safetyLimits.dailyPhInjectedMl >= safetyLimits.maxPhMinusMlPerDay) {
      logRefusalOnce(pumpIndex, "limite journalière pH atteinte");
      return false;
    }
  } else {
    if (safetyLimits.dailyOrpInjectedMl >= safetyLimits.maxChlorineMlPerDay) {
      logRefusalOnce(pumpIndex, "limite journalière ORP atteinte");
      return false;
    }
  }

  // 8. Limite horaire (minutes/h consommées).
  // Cohérent avec phLimitOk/orpLimitOk dans update() — recalcul ici pour autonomie.
  int limitMin = (pumpIndex == 0) ? mqttCfg.phInjectionLimitMinutes
                                  : mqttCfg.orpInjectionLimitMinutes;
  if (limitMin > 0) {
    unsigned long limitMs = (unsigned long)limitMin * 60000UL;
    unsigned long usedMs = (pumpIndex == 0) ? phDosingState.usedMs
                                            : orpDosingState.usedMs;
    if (usedMs >= limitMs) {
      logRefusalOnce(pumpIndex, "limite horaire atteinte");
      return false;
    }
  }

  // 9. Anti-rafale : nombre de cycles aujourd'hui dépasse maxCyclesPerDay.
  // (Pas exactement kMaxRequestsPerMinute mentionné dans la spec — on utilise la
  // garde anti-cycling existante côté pool-chemistry, plus pertinente ici.)
  unsigned int cyclesToday = (pumpIndex == 0) ? phDosingState.cyclesToday
                                              : orpDosingState.cyclesToday;
  if (cyclesToday >= pumpProtection.maxCyclesPerDay) {
    logRefusalOnce(pumpIndex, "limite cycles/jour atteinte");
    return false;
  }

  // 10. Anti-rafale court terme (pool-chemistry Pass 3.5) — protège contre les
  // emballements PID sur fenêtres glissantes 1 min et 15 min.
  // Complémentaire de la garde maxCyclesPerDay : un PID instable pourrait
  // démarrer 30 cycles en 5 minutes sans dépasser le quota journalier.
  int cyclesInLastMin = countRecentDosingCycles(pumpIndex, 60000);
  if (cyclesInLastMin >= kMaxDosingCyclesPerMinute) {
    logRefusalOnce(pumpIndex, "anti-rafale : " + String(cyclesInLastMin) +
                              " cycles dans la dernière minute");
    return false;
  }
  int cyclesInLast15Min = countRecentDosingCycles(pumpIndex, 900000);
  if (cyclesInLast15Min >= kMaxDosingCyclesPer15Min) {
    logRefusalOnce(pumpIndex, "anti-rafale : " + String(cyclesInLast15Min) +
                              " cycles dans les 15 dernières minutes");
    return false;
  }

  // Toutes les gardes sont passées : reset le log de refus et autorise.
  resetRefusalLogState(pumpIndex);
  return true;
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

// Reset journalier des compteurs ml + flags limite — appelé depuis update() avant canDose()
// pour que la réinitialisation à minuit se produise même quand la filtration est arrêtée.
// Sans cela, le compteur affichait la valeur de la veille tant que la filtration n'avait pas tourné.
void PumpControllerClass::tickDailyRollover() {
  unsigned long now = millis();

  time_t epochNow = time(nullptr);
  if (epochNow >= kMinValidEpoch) {
    struct tm timeinfo;
    localtime_r(&epochNow, &timeinfo);
    char todayStr[9];
    strftime(todayStr, sizeof(todayStr), "%Y%m%d", &timeinfo);
    if (strlen(safetyLimits.currentDayDate) > 0 && strcmp(safetyLimits.currentDayDate, todayStr) != 0) {
      systemLogger.info("Reset journalier (minuit local) — pH=" +
        String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(safetyLimits.maxPhMinusMlPerDay, 0) +
        " mL, ORP=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(safetyLimits.maxChlorineMlPerDay, 0) + " mL");
      safetyLimits.dailyPhInjectedMl  = 0;
      safetyLimits.dailyOrpInjectedMl = 0;
      safetyLimits.phLimitReached     = false;
      safetyLimits.orpLimitReached    = false;
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      saveDailyCounters();
      armStabilizationTimer();  // mitigation risque double quota au passage de minuit
    } else if (strlen(safetyLimits.currentDayDate) == 0) {
      // Première initialisation : date NTP devient disponible.
      // Reset également dayStartTimestamp pour invalider un éventuel timer fallback
      // accumulé depuis le boot — évite un reset parasite si NTP repart en panne plus tard.
      strlcpy(safetyLimits.currentDayDate, todayStr, sizeof(safetyLimits.currentDayDate));
      safetyLimits.dayStartTimestamp = 0;
    }
  } else {
    // Fallback millis() quand heure non synchronisée
    if (safetyLimits.dayStartTimestamp == 0) {
      safetyLimits.dayStartTimestamp = now;
    }
    if (now - safetyLimits.dayStartTimestamp >= 86400000UL) {
      safetyLimits.dailyPhInjectedMl  = 0;
      safetyLimits.dailyOrpInjectedMl = 0;
      safetyLimits.phLimitReached     = false;
      safetyLimits.orpLimitReached    = false;
      safetyLimits.dayStartTimestamp  = now;
      saveDailyCounters();  // persister le reset fallback
      armStabilizationTimer();
      systemLogger.info("Réinitialisation compteurs journaliers (fallback 24h)");
    }
  }
}

bool PumpControllerClass::checkSafetyLimits(bool isPhPump) {
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
  _dailyCountersDirty = true;
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

  // Chargement différé des compteurs journaliers (attend que NTP/RTC soit synchronisé)
  if (!_dailyLoaded) {
    loadDailyCounters();
    _dailyLoaded = true;
  }

  // Reset journalier — appelé AVANT canDose() pour que les compteurs se réinitialisent
  // à minuit même quand la filtration est arrêtée.
  tickDailyRollover();

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
    // Respect du mode manuel (test développement) — cohérent avec le bloc canDose() ci-dessous
    if (!manualMode[0]) applyPumpDuty(0, 0);
    if (!manualMode[1]) applyPumpDuty(1, 0);
    return;
  }

  // Garde structurelle "globale" : filtration + stabilisation post-cal + mode continu.
  // Note importante : on ne peut pas appeler canDose(0/1) ici parce qu'il filtre AUSSI
  // sur le mode régulation (== "automatic"), or la branche `scheduled` doit pouvoir
  // tourner même quand le capteur est NaN (intentionnellement aveugle, cf. spec).
  // La garde "automatic + capteur valide + calibration" est appliquée plus bas dans
  // les branches dédiées (canDose(0) et canDose(1)) — défense en profondeur.
  bool filtrationOk = (mqttCfg.regulationMode == "continu") || filtration.isRunning();
  bool stabilizationActive = isStabilizationTimerActive(0) ||
                             isStabilizationTimerActive(1);
  if (!filtrationOk || stabilizationActive) {
    // Arrêter le dosage en cours si la filtration s'arrête / stabilisation active
    if (phDosingState.active || orpDosingState.active) {
      const char* reason = !filtrationOk ? "filtration arrêtée ou mode piloté"
                                         : "stabilisation post-cal en cours";
      systemLogger.info(String("Dosage suspendu (") + reason + ")");
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

  // Calcul des limites d'injection (en minutes configurées → ms)
  int phLimitMin = mqttCfg.phInjectionLimitMinutes;
  if (phLimitMin < 0) phLimitMin = 0;
  unsigned long phLimitMs = static_cast<unsigned long>(phLimitMin) * 60000UL;

  int orpLimitMin = mqttCfg.orpInjectionLimitMinutes;
  if (orpLimitMin < 0) orpLimitMin = 0;
  unsigned long orpLimitMs = static_cast<unsigned long>(orpLimitMin) * 60000UL;

  bool phLimitOk = (phLimitMs == 0) || (phDosingState.usedMs < phLimitMs);
  bool orpLimitOk = (orpLimitMs == 0) || (orpDosingState.usedMs < orpLimitMs);

  // Log une seule fois quand la limite horaire est atteinte
  static bool phWindowLimitLogged = false;
  if (!phLimitOk && phDosingState.active && !phWindowLimitLogged) {
    systemLogger.warning("Limite horaire pH atteinte: " + String(phLimitMin) + "min/h consommées — dosage suspendu jusqu'au prochain cycle");
    phWindowLimitLogged = true;
  } else if (phLimitOk) {
    phWindowLimitLogged = false;
  }

  static bool orpWindowLimitLogged = false;
  if (!orpLimitOk && orpDosingState.active && !orpWindowLimitLogged) {
    systemLogger.warning("Limite horaire ORP atteinte: " + String(orpLimitMin) + "min/h consommées — dosage suspendu jusqu'au prochain cycle");
    orpWindowLimitLogged = true;
  } else if (orpLimitOk) {
    orpWindowLimitLogged = false;
  }

  // Vérifier les limites de sécurité journalières
  bool phSafetyOk = checkSafetyLimits(true);
  bool orpSafetyOk = checkSafetyLimits(false);

  // Contrôle pH — branche automatique gardée en profondeur par canDose(0).
  // canDose(0) vérifie watchdog, filtration, stale/NaN, calibration, stabilisation,
  // mode automatic, limites journalière/horaire, anti-cycling.
  const String& phMode = mqttCfg.phRegulationMode;
  if (phMode == "automatic" && phLimitOk && phSafetyOk && canDose(0)) {
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
        // Anti-rafale Pass 3.5 : on enregistre le timestamp de start dans le
        // ring buffer pour les fenêtres glissantes 1 min / 15 min (cf. canDose()).
        recordDosingCycleStart(0);
        systemLogger.info("Démarrage dosage pH (auto): pH=" + String(sensors.getPh(), 2) +
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
        // Estimer le volume depuis le duty PWM de la dernière itération active
        float lastFlow = dutyToFlow(phPumpControl, pumpDuty[pumpIndexFromNumber(mqttCfg.phPump)]);
        float volumeMl = (lastFlow * runTime) / 60.0f;
        systemLogger.info("Arrêt dosage pH (auto): durée=" + String(runTime) + "s" +
          " vol≈" + String(volumeMl, 1) + "mL" +
          " total jour=" + String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(safetyLimits.maxPhMinusMlPerDay, 0) + "mL");
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
  } else if (phMode == "scheduled" && phLimitOk && phSafetyOk && mqttCfg.phDailyTargetMl > 0) {
    // Mode Programmée : intentionnellement aveugle à la valeur mesurée du pH.
    // L'utilisateur a choisi un volume quotidien fixe indépendamment de la mesure ;
    // un capteur déréglé ou en cours de remplacement ne doit pas bloquer l'injection.
    // Seules les gardes volumétriques (phLimitOk, phSafetyOk, maxPhMinusMlPerDay)
    // et structurelles (canDose(), débit configuré) s'appliquent.
    // phTarget n'est pas utilisé dans cette branche et ne doit pas l'être.
    {
      float currentPh = sensors.getPh();
      static bool phOutOfRangeLogged = false;
      bool phOutOfRange = isnan(currentPh) || currentPh < 4.0f || currentPh > 10.0f;
      if (phOutOfRange && !phOutOfRangeLogged) {
        const String phValStr = isnan(currentPh) ? "NaN" : String(currentPh, 2);
        systemLogger.warning("[Scheduled] Capteur pH hors plage (" + phValStr + ") — dosage programmé maintenu");
        phOutOfRangeLogged = true;
      } else if (!phOutOfRange && phOutOfRangeLogged) {
        systemLogger.info("[Scheduled] Capteur pH revenu dans la plage normale");
        phOutOfRangeLogged = false;
      }
      // Plafonner à la limite journalière de sécurité
      float effectiveDailyMl = static_cast<float>(mqttCfg.phDailyTargetMl);
      static bool phCappedLogged = false;
      if (safetyLimits.maxPhMinusMlPerDay > 0.0f && effectiveDailyMl > safetyLimits.maxPhMinusMlPerDay) {
        if (!phCappedLogged) {
          systemLogger.warning("[Scheduled] phDailyTargetMl (" + String(mqttCfg.phDailyTargetMl) +
            "mL) dépasse maxPhMinusMlPerDay (" + String(safetyLimits.maxPhMinusMlPerDay, 0) + "mL) — plafonné");
          phCappedLogged = true;
        }
        effectiveDailyMl = safetyLimits.maxPhMinusMlPerDay;
      } else {
        phCappedLogged = false;
      }
      float effectiveFlowMlPerMin = phPumpControl.maxFlowMlPerMin *
                                    (static_cast<float>(mqttCfg.pump1MaxDutyPct) / 100.0f);
      if (effectiveFlowMlPerMin < phPumpControl.minFlowMlPerMin)
        effectiveFlowMlPerMin = phPumpControl.minFlowMlPerMin;
      static bool phZeroFlowLogged = false;
      if (effectiveFlowMlPerMin <= 0.0f) {
        if (!phZeroFlowLogged) {
          systemLogger.critical("[Scheduled] Débit pompe pH non configuré (0 mL/min) — dosage bloqué");
          phZeroFlowLogged = true;
        }
      } else {
        phZeroFlowLogged = false;
        // Injecter si le quota journalier n'est pas atteint (la limite horaire phLimitOk est déjà vérifiée en condition d'entrée)
        bool shouldDose = safetyLimits.dailyPhInjectedMl < effectiveDailyMl;
        if (shouldDose && !phDosingState.active) {
          phDosingState.lastStartTime = now;
          // Anti-rafale Pass 3.5 : la branche scheduled n'incrémente pas cyclesToday,
          // mais elle peut redémarrer plusieurs fois en cas de quota approchant la
          // limite — on enregistre quand même chaque start pour cohérence.
          recordDosingCycleStart(0);
          systemLogger.info("[Scheduled] Démarrage dosage pH : quota=" + String(static_cast<int>(effectiveDailyMl)) +
            "mL, injecté=" + String(safetyLimits.dailyPhInjectedMl, 0) + "mL");
        } else if (!shouldDose && phDosingState.active) {
          phDosingState.lastStopTime = now;
          systemLogger.info("[Scheduled] Quota journalier pH atteint (" +
            String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(static_cast<int>(effectiveDailyMl)) +
            "mL) — dosage suspendu jusqu'à demain");
        }
        if (shouldDose) {
          int index = pumpIndexFromNumber(mqttCfg.phPump);
          uint8_t duty = flowToDuty(phPumpControl, effectiveFlowMlPerMin);
          if (duty > desiredDuty[index]) desiredDuty[index] = duty;
          if (duty > 0) { phActive = true; phFlow = effectiveFlowMlPerMin; }
        }
      }
    }
    // Reset PID pour éviter le windup au retour en mode automatique
    phPID.integral = 0.0f;
    phPID.lastError = 0.0f;
    phPID.lastTime = 0;
  }

  // Contrôle ORP — dispatch sur orpRegulationMode, gardé en profondeur par canDose(1).
  const String& orpMode = mqttCfg.orpRegulationMode;
  if (orpMode == "automatic" && orpLimitOk && orpSafetyOk && canDose(1)) {
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
        // Anti-rafale Pass 3.5 : timestamp de start pour les fenêtres glissantes.
        recordDosingCycleStart(1);
        systemLogger.info("Démarrage dosage ORP (auto): ORP=" + String(sensors.getOrp(), 0) + "mV" +
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
        // Estimer le volume depuis le duty PWM de la dernière itération active
        float lastFlow = dutyToFlow(orpPumpControl, pumpDuty[pumpIndexFromNumber(mqttCfg.orpPump)]);
        float volumeMl = (lastFlow * runTime) / 60.0f;
        systemLogger.info("Arrêt dosage ORP (auto): durée=" + String(runTime) + "s" +
          " vol≈" + String(volumeMl, 1) + "mL" +
          " total jour=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL");
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
  } else if (orpMode == "scheduled" && orpLimitOk && orpSafetyOk && mqttCfg.orpDailyTargetMl > 0) {
    // Mode Programmée : intentionnellement aveugle à la valeur mesurée de l'ORP.
    // L'utilisateur a choisi un volume quotidien fixe de chlore indépendamment de la mesure ;
    // un capteur déréglé ou en cours de remplacement ne doit pas bloquer l'injection.
    // Seules les gardes volumétriques (orpLimitOk, orpSafetyOk, maxChlorineMlPerDay)
    // et structurelles (canDose(), débit configuré) s'appliquent.
    // orpTarget n'est pas utilisé dans cette branche et ne doit pas l'être.
    {
      float currentOrp = sensors.getOrp();
      static bool orpOutOfRangeLogged = false;
      bool orpOutOfRange = isnan(currentOrp) || currentOrp < 0.0f || currentOrp > 1500.0f;
      if (orpOutOfRange && !orpOutOfRangeLogged) {
        const String orpValStr = isnan(currentOrp) ? "NaN" : String(currentOrp, 0);
        systemLogger.warning("[Scheduled ORP] Capteur ORP hors plage (" + orpValStr + "mV) — dosage programmé maintenu");
        orpOutOfRangeLogged = true;
      } else if (!orpOutOfRange && orpOutOfRangeLogged) {
        systemLogger.info("[Scheduled ORP] Capteur ORP revenu dans la plage normale");
        orpOutOfRangeLogged = false;
      }
      // Plafonner à la limite journalière de sécurité
      float effectiveDailyMl = static_cast<float>(mqttCfg.orpDailyTargetMl);
      static bool orpCappedLogged = false;
      if (safetyLimits.maxChlorineMlPerDay > 0.0f && effectiveDailyMl > safetyLimits.maxChlorineMlPerDay) {
        if (!orpCappedLogged) {
          systemLogger.warning("[Scheduled ORP] orpDailyTargetMl (" + String(mqttCfg.orpDailyTargetMl) +
            "mL) dépasse maxChlorineMlPerDay (" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL) — plafonné");
          orpCappedLogged = true;
        }
        effectiveDailyMl = safetyLimits.maxChlorineMlPerDay;
      } else {
        orpCappedLogged = false;
      }
      float effectiveFlowMlPerMin = orpPumpControl.maxFlowMlPerMin *
                                    (static_cast<float>(mqttCfg.pump2MaxDutyPct) / 100.0f);
      if (effectiveFlowMlPerMin < orpPumpControl.minFlowMlPerMin)
        effectiveFlowMlPerMin = orpPumpControl.minFlowMlPerMin;
      static bool orpZeroFlowLogged = false;
      if (effectiveFlowMlPerMin <= 0.0f) {
        if (!orpZeroFlowLogged) {
          systemLogger.critical("[Scheduled ORP] Débit pompe ORP non configuré (0 mL/min) — dosage bloqué");
          orpZeroFlowLogged = true;
        }
      } else {
        orpZeroFlowLogged = false;
        // Injecter si le quota journalier n'est pas atteint (la limite horaire orpLimitOk est déjà vérifiée en condition d'entrée)
        bool shouldDose = safetyLimits.dailyOrpInjectedMl < effectiveDailyMl;
        if (shouldDose && !orpDosingState.active) {
          orpDosingState.lastStartTime = now;
          // Anti-rafale Pass 3.5 : enregistre le start pour les fenêtres glissantes.
          recordDosingCycleStart(1);
          systemLogger.info("[Scheduled ORP] Démarrage dosage ORP : quota=" + String(static_cast<int>(effectiveDailyMl)) +
            "mL, injecté=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "mL");
        } else if (!shouldDose && orpDosingState.active) {
          orpDosingState.lastStopTime = now;
          systemLogger.info("[Scheduled ORP] Quota journalier ORP atteint (" +
            String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(static_cast<int>(effectiveDailyMl)) +
            "mL) — dosage suspendu jusqu'à demain");
        }
        if (shouldDose) {
          int index = pumpIndexFromNumber(mqttCfg.orpPump);
          uint8_t duty = flowToDuty(orpPumpControl, effectiveFlowMlPerMin);
          if (duty > desiredDuty[index]) desiredDuty[index] = duty;
          if (duty > 0) { orpActive = true; orpFlow = effectiveFlowMlPerMin; }
        }
      }
    }
    // Reset PID pour éviter le windup au retour en mode automatique
    orpPID.integral = 0.0f;
    orpPID.lastError = 0.0f;
    orpPID.lastTime = 0;
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
  // Couvre à la fois la régulation automatique ET l'injection manuelle.
  // IMPORTANT: ne pas utiliser lastTimestamp (mis à jour par refreshDosingState), sinon delta≈0.
  int phIdx  = pumpIndexFromNumber(mqttCfg.phPump);
  int orpIdx = pumpIndexFromNumber(mqttCfg.orpPump);
  bool phManualActive  = manualMode[phIdx]  && pumpDuty[phIdx]  > 0;
  bool orpManualActive = manualMode[orpIdx] && pumpDuty[orpIdx] > 0;

  if (phActive || phManualActive) {
    if (phDosingState.lastSafetyTimestamp == 0) {
      phDosingState.lastSafetyTimestamp = now;
    }
    unsigned long delta = now - phDosingState.lastSafetyTimestamp;
    float phEffectiveFlow = phManualActive
      ? dutyToFlow(phPumpControl, pumpDuty[phIdx])
      : dutyToFlow(phPumpControl, desiredDuty[phIdx]);
    updateSafetyTracking(true, phEffectiveFlow, delta);
    phDosingState.lastSafetyTimestamp = now;
  } else {
    phDosingState.lastSafetyTimestamp = 0;
  }

  if (orpActive || orpManualActive) {
    if (orpDosingState.lastSafetyTimestamp == 0) {
      orpDosingState.lastSafetyTimestamp = now;
    }
    unsigned long delta = now - orpDosingState.lastSafetyTimestamp;
    float orpEffectiveFlow = orpManualActive
      ? dutyToFlow(orpPumpControl, pumpDuty[orpIdx])
      : dutyToFlow(orpPumpControl, desiredDuty[orpIdx]);
    updateSafetyTracking(false, orpEffectiveFlow, delta);
    orpDosingState.lastSafetyTimestamp = now;
  } else {
    orpDosingState.lastSafetyTimestamp = 0;
  }

  // Sauvegarde NVS des compteurs journaliers

  // Sauvegarde immédiate au démarrage d'une injection pH (transition false → true)
  bool phNowActive = phActive;
  if (phNowActive && !_phWasActive) {
    saveDailyCounters();
    _dailyCountersDirty = false;
    _lastDailySaveMs = now;
  }
  _phWasActive = phNowActive;

  // Sauvegarde immédiate au démarrage d'une injection ORP (transition false → true)
  bool orpNowActive = orpActive;
  if (orpNowActive && !_orpWasActive) {
    saveDailyCounters();
    _dailyCountersDirty = false;
    _lastDailySaveMs = now;
  }
  _orpWasActive = orpNowActive;

  // Flush périodique toutes les 30s si des données ont changé
  if (_dailyCountersDirty && (now - _lastDailySaveMs >= 30000UL)) {
    saveDailyCounters();
    _dailyCountersDirty = false;
    _lastDailySaveMs = now;
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
