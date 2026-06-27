#include "pump_controller.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "uart_protocol.h"
#include "dosing_logic.h"
#include <esp_task_wdt.h>
#include <time.h>

PumpControllerClass PumpController;

// pool-chemistry (feature-036) : verrou de complétude de la table énum→String FR
// du switch de canDose(). BurstPer15Min DOIT rester la dernière valeur de
// DoseRefusal ; si l'énum évolue, ce static_assert force la relecture du mapping.
static_assert(static_cast<int>(DoseRefusal::BurstPer15Min) == 14,
              "DoseRefusal modifié : mettre à jour le switch énum→String FR de canDose()");

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

  // feature-025 (pool-chemistry) : coefficients par défaut "P temporisée pure".
  // pH garde les défauts du struct (Kp=8, Ki=0, Kd=0). ORP plus conservateur :
  // Kp=0.3 (l'ORP dépend du pH, de la T° et du stabilisant). Kd=0 IMPÉRATIF.
  orpPID.kp = 0.3f;
  orpPID.ki = 0.0f;
  orpPID.kd = 0.0f;
}

// Applique les paramètres PID selon la vitesse de régulation configurée.
//
// feature-025 (pool-chemistry, NON NÉGOCIABLE) : régulation "P temporisée pure".
//   - Ki = 0 et Kd = 0 IMPÉRATIFS quelle que soit la vitesse choisie.
//   - Seul Kp module la vitesse (slow / normal / fast).
//   - pH plus réactif (Kp 4..12), ORP plus conservateur (Kp 0.15..0.5) car l'ORP
//     dépend du pH, de la T° et du stabilisant.
// Les anciennes valeurs Ki≠0 / Kd≠0 sont volontairement supprimées : avec une mesure
// filtrée (médiane + EMA) et une pause mélange, un terme dérivé amplifierait le bruit
// résiduel et un terme intégral provoquerait un windup malgré l'anti-windup strict.
void PumpControllerClass::applyRegulationSpeed() {
  const String& speed = mqttCfg.regulationSpeed;
  if (speed == "slow") {
    phPID.kp  = 4.0f;
    orpPID.kp = 0.15f;
  } else if (speed == "fast") {
    phPID.kp  = 12.0f;
    orpPID.kp = 0.5f;
  } else {
    // "normal" (défaut)
    phPID.kp  = 8.0f;
    orpPID.kp = 0.3f;
  }
  // Ki/Kd toujours forcés à 0 (P temporisée pure — pool-chemistry feature-025).
  phPID.ki  = 0.0f;  phPID.kd  = 0.0f;
  orpPID.ki = 0.0f;  orpPID.kd = 0.0f;
  systemLogger.info("PID régulation (P temporisée): vitesse=" + speed +
    " Kp_pH=" + String(phPID.kp, 2) +
    " Kp_ORP=" + String(orpPID.kp, 2) +
    " Ki=0 Kd=0");
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


// COQUILLE (feature-036) : conserve le reset journalier de cyclesToday et le
// warning "Limite cycles atteinte" (effets de bord) ; délègue le verdict pur à
// shouldStartDosingPure().
bool PumpControllerClass::shouldStartDosing(float error, float startThreshold, DosingState& state, unsigned long now) {
  // Réinitialiser le compteur de cycles chaque jour
  if (state.cyclesDayStart == 0 || now - state.cyclesDayStart >= 86400000UL) {
    state.cyclesToday = 0;
    state.cyclesDayStart = now;
  }

  // Effet de bord conservé : warning edge-triggered quand la limite est atteinte.
  if (state.cyclesToday >= pumpProtection.maxCyclesPerDay) {
    static unsigned long lastWarning = 0;
    if (now - lastWarning > 3600000) {  // Log toutes les heures
      systemLogger.warning("Limite cycles atteinte: " + String(state.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay));
      lastWarning = now;
    }
  }

  return shouldStartDosingPure(error, startThreshold, state.cyclesToday,
                               pumpProtection.maxCyclesPerDay);
}

// COQUILLE (feature-036) : calcule runTimeMs (lastStartTime>0) puis délègue à
// shouldContinueDosingPure(). Si lastStartTime==0, on neutralise la garde temps
// minimum (runTimeMs = minInjectionTimeMs → `<` faux), comportement identique
// à l'origine où le bloc temps minimum était entièrement sauté.
bool PumpControllerClass::shouldContinueDosing(float error, float stopThreshold, DosingState& state, unsigned long now) {
  unsigned long runTimeMs = (state.lastStartTime > 0)
      ? (now - state.lastStartTime)
      : pumpProtection.minInjectionTimeMs;
  return shouldContinueDosingPure(error, stopThreshold, runTimeMs,
                                  pumpProtection.minInjectionTimeMs);
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

// =============================================================================
// feature-025 — Pause mélange hydraulique post-injection (pool-chemistry)
// =============================================================================
// Géré par timestamps (AUCUN delay()). Gate indépendante (OR) dans canDose(),
// distincte du timer post-calibration. Écrits/lus en loopTask uniquement.

void PumpControllerClass::notifyPhDose(uint32_t nowMs) {
  _mixingEndMs[0] = nowMs + (uint32_t)kPhMixingDelayMs;
}

void PumpControllerClass::notifyOrpDose(uint32_t nowMs) {
  _mixingEndMs[1] = nowMs + (uint32_t)kOrpMixingDelayMs;
}

bool PumpControllerClass::isPhMixingDelayActive(uint32_t nowMs) const {
  if (_mixingEndMs[0] == 0) return false;
  // Arithmétique non signée : (end - now) > 0 si encore actif (gère wrap millis()).
  return (int32_t)(_mixingEndMs[0] - nowMs) > 0;
}

bool PumpControllerClass::isOrpMixingDelayActive(uint32_t nowMs) const {
  if (_mixingEndMs[1] == 0) return false;
  return (int32_t)(_mixingEndMs[1] - nowMs) > 0;
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

// canDose(int) — COQUILLE (feature-036). Collecte les entrées depuis les globals
// puis délègue la décision à evaluateDose() (logique pure testable en natif).
// Fail-closed strict, ordre des gardes validé pool-chemistry feature-021 reproduit
// à l'identique dans dosing_logic.cpp. Le formatage français des causes de refus
// (au mot près, valeurs runtime réinjectées) reste ici, dans la coquille.
bool PumpControllerClass::canDose(int pumpIndex) {
  // Garde structurelle (feature-036, condition pool-chemistry n°1) : index invalide.
  // RESTE dans la coquille, AVANT evaluateDose, SANS log (comportement actuel).
  if (pumpIndex < 0 || pumpIndex > 1) return false;

  // --- Collecte des entrées (globals, I²C cache, millis(), historique anti-rafale) ---
  uint32_t nowMs = (uint32_t)millis();
  int limitMin = (pumpIndex == 0) ? mqttCfg.phInjectionLimitMinutes
                                  : mqttCfg.orpInjectionLimitMinutes;

  DoseInputs in;
  // 1. Watchdog actif. esp_task_wdt_status(NULL) == ESP_OK si la tâche courante
  // est inscrite au watchdog, ESP_ERR_NOT_FOUND sinon.
  in.watchdogActive = (esp_task_wdt_status(NULL) == ESP_OK);
  // 2. Filtration (mode continu accepté sans relais filtration ON).
  in.continuousMode = (mqttCfg.regulationMode == "continu");
  in.filtrationRunning = filtration.isRunning();
  // 3. Lecture FILTRÉE (feature-025 : le PID auto consomme la mesure filtrée).
  in.reading = (pumpIndex == 0) ? sensors.getPhFiltered()
                                : sensors.getOrpFiltered();
  // 3b/3c. État du filtre capteur (warmup / instabilité).
  in.filterReady = (pumpIndex == 0) ? sensors.isPhFilterReady()
                                    : sensors.isOrpFilterReady();
  in.filterUnstable = (pumpIndex == 0) ? sensors.isPhFilterUnstable()
                                       : sensors.isOrpFilterUnstable();
  // 4. Calibration (cache, pas d'I²C bloquant dans la boucle ; -1 si injoignable).
  in.calPoints = (pumpIndex == 0) ? sensors.getPhCalibrationPointsCached()
                                  : sensors.getOrpCalibrationPointsCached();
  in.requiredPoints = (pumpIndex == 0) ? 2 : 1;
  // 5/5b. Stabilisation post-cal et pause mélange hydraulique (gates indépendantes).
  in.stabilizationActive = isStabilizationTimerActive(pumpIndex);
  in.mixingActive = (pumpIndex == 0) ? isPhMixingDelayActive(nowMs)
                                     : isOrpMixingDelayActive(nowMs);
  // 6. Mode régulation = automatic.
  const String& mode = (pumpIndex == 0) ? mqttCfg.phRegulationMode
                                        : mqttCfg.orpRegulationMode;
  in.modeAutomatic = (mode == "automatic");
  // 7. Limite journalière.
  in.dailyInjectedMl = (pumpIndex == 0) ? safetyLimits.dailyPhInjectedMl
                                        : safetyLimits.dailyOrpInjectedMl;
  in.maxDailyMl = (pumpIndex == 0) ? safetyLimits.maxPhMinusMlPerDay
                                   : safetyLimits.maxChlorineMlPerDay;
  // 8. Limite horaire (0 = pas de limite).
  in.usedMs = (pumpIndex == 0) ? phDosingState.usedMs : orpDosingState.usedMs;
  in.hourlyLimitMs = (limitMin > 0) ? (unsigned long)limitMin * 60000UL : 0UL;
  // 9. Anti-cycling journalier.
  in.cyclesToday = (pumpIndex == 0) ? phDosingState.cyclesToday
                                    : orpDosingState.cyclesToday;
  in.maxCyclesPerDay = pumpProtection.maxCyclesPerDay;
  // 10. Anti-rafale court terme (condition pool-chemistry n°4 : countRecentDosingCycles
  // RESTE dans la coquille, seul le comparatif est dans evaluateDose).
  in.cyclesLastMin = countRecentDosingCycles(pumpIndex, 60000);
  in.maxCyclesPerMin = kMaxDosingCyclesPerMinute;
  in.cyclesLast15Min = countRecentDosingCycles(pumpIndex, 900000);
  in.maxCyclesPer15Min = kMaxDosingCyclesPer15Min;

  // --- Décision pure ---
  DoseDecision decision = evaluateDose(in);

  if (decision.cause == DoseRefusal::None) {
    resetRefusalLogState(pumpIndex);
    return true;
  }

  // --- Mapping énum → cause française (au mot près, identique à l'historique) ---
  // ATTENTION pool-chemistry (feature-036) : ce switch DOIT couvrir tous les cas
  // de DoseRefusal sauf None. Toute nouvelle valeur ajoutée à l'énum sans entrée
  // ici déclenchera le static_assert ci-dessous (complétude verrouillée). Les
  // chaînes sont des COPIES exactes du canDose() d'origine — ne pas reformuler.
  String cause;
  switch (decision.cause) {
    case DoseRefusal::WatchdogInactive:
      cause = "watchdog inactif";
      break;
    case DoseRefusal::FiltrationOff:
      cause = "filtration arrêtée";
      break;
    case DoseRefusal::ReadingNaN:
      cause = "lecture pH/ORP filtrée indisponible (NaN)";
      break;
    case DoseRefusal::FilterNotReady:
      cause = "filtre capteur non prêt (warmup / EZO injoignable)";
      break;
    case DoseRefusal::FilterUnstable:
      cause = "capteur instable (rejets consécutifs)";
      break;
    case DoseRefusal::CalibrationInsufficient:
      cause = String("calibration insuffisante (cal=") + String(in.calPoints) +
              ", requis=" + String(in.requiredPoints) + ")";
      break;
    case DoseRefusal::StabilizationActive:
      cause = "stabilisation post-calibration en cours";
      break;
    case DoseRefusal::MixingActive:
      cause = "pause mélange en cours";
      break;
    case DoseRefusal::ModeNotAutomatic:
      cause = String("mode régulation != automatic (") + mode + ")";
      break;
    case DoseRefusal::DailyLimit:
      cause = (pumpIndex == 0) ? "limite journalière pH atteinte"
                               : "limite journalière ORP atteinte";
      break;
    case DoseRefusal::HourlyLimit:
      cause = "limite horaire atteinte";
      break;
    case DoseRefusal::CyclesPerDay:
      cause = "limite cycles/jour atteinte";
      break;
    case DoseRefusal::BurstPerMinute:
      cause = "anti-rafale : " + String(in.cyclesLastMin) +
              " cycles dans la dernière minute";
      break;
    case DoseRefusal::BurstPer15Min:
      cause = "anti-rafale : " + String(in.cyclesLast15Min) +
              " cycles dans les 15 dernières minutes";
      break;
    case DoseRefusal::None:
      // Inatteignable (traité plus haut) — présent pour la complétude du switch.
      break;
  }

  logRefusalOnce(pumpIndex, cause);
  return false;
}

float PumpControllerClass::computePID(PIDController& pid, float error, unsigned long now,
                                      float deadband, bool freezeIntegral,
                                      float minFlow, float maxFlow) {
  // Coquille (feature-037) : gestion du temps + de l'état PID. Le cœur du calcul
  // (proportionnel + anti-windup + bornage final) est délégué à computePidPure().
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

  // Délégation au cœur pur : l'état PID est passé en paramètre et renvoyé.
  // computePidPure renvoie le débit FINAL déjà borné [minFlow, maxFlow].
  PidResult r = computePidPure(pid.kp, pid.ki, pid.kd, error, pid.lastError,
                               pid.integral, dt, pid.integralMax, deadband,
                               minFlow, maxFlow, freezeIntegral);

  pid.integral = r.integral;
  pid.lastError = r.lastError;
  pid.lastTime = now;

  return r.flow;
}

// DEAD CODE : non appelé dans le chemin de dosage (cf. feature-037) — conservé pour usage futur
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
    // feature-025 : le PID auto consomme la mesure FILTRÉE (médiane + EMA).
    // canDose(0) a déjà garanti isPhFilterReady() && !isPhFilterUnstable() → non NaN.
    float phValue = sensors.getPhFiltered();
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
        systemLogger.info("Démarrage dosage pH (auto): pH=" + String(sensors.getPhFiltered(), 2) +
          " cible=" + String(mqttCfg.phTarget, 2) +
          " erreur=" + String(error, 3) +
          " (cycle " + String(phDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
      }
    }

    float flow = 0.0f;
    if (shouldDose) {
      // feature-025 (B6) : geler l'intégrale si la sortie sature (terme P seul
      // ≥ débit max → toute accumulation serait du windup). deadband = seuil start.
      bool phSaturated = (phPID.kp * error >= phPumpControl.maxFlowMlPerMin);
      // Calcul PID — computePID renvoie le débit FINAL borné (feature-037 : le
      // bornage [minFlow, maxFlow] est appliqué dans computePidPure, plus de
      // constrain externe). minFlow évite flow=0 à la première invocation du PID.
      flow = computePID(phPID, error, now,
                        pumpProtection.phStartThreshold, phSaturated,
                        phPumpControl.minFlowMlPerMin, phPumpControl.maxFlowMlPerMin);
    } else {
      // Arrêt du dosage
      if (phDosingState.active) {
        phDosingState.lastStopTime = now;
        // feature-025 : pause mélange hydraulique armée à l'ARRÊT de l'injection
        // (homogénéisation post-dose), une fois la dose réellement versée.
        notifyPhDose((uint32_t)now);
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
    // feature-025 : le PID auto consomme la mesure FILTRÉE (médiane + EMA).
    // canDose(1) a déjà garanti isOrpFilterReady() && !isOrpFilterUnstable() → non NaN.
    float orpValue = sensors.getOrpFiltered();
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
        systemLogger.info("Démarrage dosage ORP (auto): ORP=" + String(sensors.getOrpFiltered(), 0) + "mV" +
          " cible=" + String(mqttCfg.orpTarget, 0) + "mV" +
          " erreur=" + String(error, 0) + "mV" +
          " (cycle " + String(orpDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
      }
    }

    float flow = 0.0f;
    if (shouldDose) {
      // feature-025 (B6) : geler l'intégrale si la sortie sature. deadband = seuil start.
      bool orpSaturated = (orpPID.kp * error >= orpPumpControl.maxFlowMlPerMin);
      // Calcul PID — computePID renvoie le débit FINAL borné (feature-037 : le
      // bornage [minFlow, maxFlow] est appliqué dans computePidPure, plus de
      // constrain externe). minFlow évite flow=0 à la première invocation du PID.
      flow = computePID(orpPID, error, now,
                        pumpProtection.orpStartThreshold, orpSaturated,
                        orpPumpControl.minFlowMlPerMin, orpPumpControl.maxFlowMlPerMin);
    } else {
      // Arrêt du dosage
      if (orpDosingState.active) {
        orpDosingState.lastStopTime = now;
        // feature-025 : pause mélange hydraulique armée à l'ARRÊT de l'injection
        // (homogénéisation post-dose), une fois la dose réellement versée.
        notifyOrpDose((uint32_t)now);
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
