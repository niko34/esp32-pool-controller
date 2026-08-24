#include "pump_controller.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "dosing_logic.h"
#include "schedule_logic.h"
#include "mqtt_manager.h"  // feature-067 : alertes MQTT des planchers pH bruts
#include "measure_validity.h"  // v2.20.3 (C6) : distinguer purge en cours et capteur muet
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
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    // Migration core 3 (feature-063) : ledcAttachChannel remplace
    // ledcSetup+ledcAttachPin en conservant le canal LEDC explicite.
    // Fréquence 20 kHz et résolution 8 bits STRICTEMENT identiques (sécurité
    // chimique : le duty des pompes doseuses ne doit pas changer d'échelle).
    if (!ledcAttachChannel(pumps[i].pwmPin, PUMP_PWM_FREQ, PUMP_PWM_RES_BITS,
                           pumps[i].channel)) {
      // Prescription P1 (pool-chemistry) : échec d'attache = pompe INUTILISABLE
      // jusqu'au reboot. Fail-closed : aucun ledcWriteChannel sur un canal non
      // attaché ; applyPumpDuty() refusera toute mutation (duty reste 0 à vie).
      pumpUsable[i] = false;
      systemLogger.critical("Échec attache LEDC pompe " + String(i + 1) +
                            " (GPIO=" + String(pumps[i].pwmPin) +
                            ", canal=" + String(pumps[i].channel) +
                            ") — pompe déclarée INUTILISABLE jusqu'au reboot");
    } else {
      ledcWriteChannel(pumps[i].channel, 0);  // Pompe arrêtée au démarrage
    }
#else
    ledcSetup(pumps[i].channel, PUMP_PWM_FREQ, PUMP_PWM_RES_BITS);
    ledcAttachPin(pumps[i].pwmPin, pumps[i].channel);
    ledcWrite(pumps[i].channel, 0);  // Pompe arrêtée au démarrage
#endif
    systemLogger.info("Pompe " + String(i + 1) + " : GPIO=" + String(pumps[i].pwmPin) +
                      " canal LEDC=" + String(pumps[i].channel));
  }
  applyRegulationSpeed();
  systemLogger.info("Contrôleur de pompes initialisé");
  systemLogger.info("Config pH: cible=" + String(mqttCfg.phTarget, 2) +
    " seuil=" + String(pumpProtection.phStartThreshold, 2) +
    " limite=" + String(mqttCfg.phInjectionLimitMinutes) + "min/h" +
    " max=" + String(safetyLimits.maxPhMlPerDay, 0) + "mL/j");
  systemLogger.info("Config ORP: cible=" + String(mqttCfg.orpTarget, 0) + "mV" +
    " seuil=" + String(pumpProtection.orpStartThreshold, 0) + "mV" +
    " limite=" + String(mqttCfg.orpInjectionLimitMinutes) + "min/h" +
    " max=" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL/j");
  systemLogger.info("Puissance: P1=" + String(mqttCfg.pump1MaxDutyPct) + "% P2=" + String(mqttCfg.pump2MaxDutyPct) + "%");
}

// feature-064 : accesseur LECTURE SEULE de l'utilisabilité pompe — sert
// uniquement à la visibilité (alerte MQTT pump_unusable, ADR-0034). Ne réactive
// JAMAIS une pompe : le flag est posé une seule fois dans begin(), reboot requis.
bool PumpControllerClass::isPumpUsable(int index) const {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Core 3 : reflète le flag fail-closed posé par begin() en cas d'échec
  // ledcAttachChannel(). Index hors bornes → false (fail-closed).
  return (index == 0 || index == 1) && pumpUsable[index];
#else
  // Core 2 : ledcSetup/ledcAttachPin ne signalent pas d'échec — la pompe est
  // toujours réputée utilisable (aucune alerte possible, comportement inchangé).
  return index == 0 || index == 1;
#endif
}

void PumpControllerClass::applyPumpDuty(int index, uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Prescription P1 (pool-chemistry, feature-063) : garde en PREMIÈRE ligne,
  // AVANT le cache pumpDuty et toute mutation — une pompe inutilisable (échec
  // d'attache LEDC au boot) garde pumpDuty[index] == 0 à vie.
  if (!pumpUsable[index]) return;
#endif
  duty = duty > MAX_PWM_DUTY ? MAX_PWM_DUTY : duty;
  if (pumpDuty[index] == duty) return;

  pumpDuty[index] = duty;

  // MOSFET IRLZ44N: Contrôle simple via PWM sur Gate
  // duty=0 → MOSFET OFF (pompe arrêtée)
  // duty>0 → MOSFET ON proportionnel (pompe tourne)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Core 3 : ledcWrite() attend désormais un PIN — ledcWriteChannel conserve
  // l'adressage par canal (aucun changement d'échelle 0-255).
  ledcWriteChannel(pumps[index].channel, duty);
#else
  ledcWrite(pumps[index].channel, duty);
#endif
}

void PumpControllerClass::refreshDosingState(DosingState& state, unsigned long now, bool pumpOn) {
  if (state.windowStart == 0) {
    state.windowStart = now;
    state.lastTimestamp = now;
  }

  // Réinitialiser la fenêtre toutes les heures
  if (now - state.windowStart >= 3600000UL) {
    state.windowStart = now;
    state.usedMs = 0;
  }

  // Accumuler le temps d'injection.
  // feature-006 (condition pool-chemistry #2) : budget horaire usedMs PARTAGÉ
  // auto + manuel. Itération 2 (bug-dosage-fantome) : le prédicat est désormais
  // POMPE-ON (pompe physiquement alimentée — pouls auto, scheduled OU manuel),
  // plus la session : entre deux pouls, le budget horaire n'est pas consommé.
  // UN seul point d'accumulation → PAS de double comptage possible.
  if (pumpOn) {
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

// COQUILLE (feature-036) : délègue à shouldContinueDosingPure().
// Itération 2 (bug-dosage-fantome) : la garde de temps minimum d'injection
// raisonne en temps POMPE-ON cumulé de la session (_sessionOnMs), plus en temps
// mural depuis lastStartTime — en proportionnel de temps, la pompe ne tourne
// qu'une fraction de la session, le « temps minimum » doit compter du produit
// réellement poussé. Flaggé pour contre-visite du gate pool-chemistry.
bool PumpControllerClass::shouldContinueDosing(float error, float stopThreshold,
                                               unsigned long sessionOnMs) {
  return shouldContinueDosingPure(error, stopThreshold, sessionOnMs,
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

// Secondes restantes de pause mélange pour UNE pompe ([0]=pH, [1]=ORP), 0 si
// inactive. Observabilité UI (feature-025 : affichage widget). Wrap-safe millis().
unsigned long PumpControllerClass::getMixingRemainingS(int pumpIndex) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  if (_mixingEndMs[pumpIndex] == 0) return 0;
  uint32_t now = (uint32_t)millis();
  int32_t rem = (int32_t)(_mixingEndMs[pumpIndex] - now);
  return rem > 0 ? (unsigned long)rem / 1000UL : 0;
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

// feature-006 : secondes restantes de stabilisation pour UNE pompe.
// ASYMÉTRIE ASSUMÉE (condition pool-chemistry #5) : l'AUTO suspend les 2 pompes
// dès qu'une stabilisation est active (garde globale d'update()), le MANUEL n'est
// bloqué que par la fenêtre de SA pompe (fenêtres indépendantes par sonde,
// kStabilizationDurationPhMs / kStabilizationDurationOrpMs).
unsigned long PumpControllerClass::getStabilizationRemainingS(int pumpIndex) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  uint32_t end = _stabilizationEndMs[pumpIndex];
  if (end == 0) return 0;
  unsigned long now = millis();
  if (now >= end) return 0;
  return (end - now) / 1000UL;
}

// =============================================================================
// feature-006 — Compteurs exposés aux gardes d'injection manuelle
// =============================================================================
// Appelés depuis le handler web async (hors loopTask). Lecture SNAPSHOT sans
// mutex : cyclesToday et le ring anti-rafale sont écrits en loopTask uniquement,
// et une lecture 32 bits alignée est atomique sur ESP32 → course BÉNIGNE (au
// pire une valeur en retard d'un tour de loop). Le pending atomique est AJOUTÉ
// au résultat pour que deux starts manuels rapprochés (avant consommation par
// update()) se voient mutuellement (fail-closed).

unsigned int PumpControllerClass::getCyclesToday(int pumpIndex) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  unsigned int cycles = (pumpIndex == 0) ? phDosingState.cyclesToday
                                         : orpDosingState.cyclesToday;
  return cycles + _manualCycleStartPending[pumpIndex].load();
}

int PumpControllerClass::getRecentCycles(int pumpIndex, uint32_t windowMs) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  return countRecentDosingCycles(pumpIndex, windowMs) +
         static_cast<int>(_manualCycleStartPending[pumpIndex].load());
}

// Demande d'enregistrement d'un démarrage de cycle MANUEL depuis un handler
// async. Simple incrément atomique ; la matérialisation (ring anti-rafale +
// cyclesToday + NVS dirty) est faite en tête d'update(), sur loopTask.
void PumpControllerClass::requestManualCycleRecord(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  _manualCycleStartPending[pumpIndex].fetch_add(1);
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

// =============================================================================
// feature-067 — Filets de sécurité sur le pH BRUT (conditions pool-chemistry)
// =============================================================================
// L'offset logiciel pH (feature-067) déplace l'entrée de la régulation : un
// offset erroné fait réguler le bassin à un pH réel différent de la consigne.
// Ces deux gardes travaillent donc sur sensors.getPhFilteredRaw() — la mesure
// capteur SANS offset — et restent valides même si l'offset est faux.
// Appelées EN AMONT de canDose() dans update() (loopTask) : quand elles bloquent,
// canDose() n'est pas évalué et `_lastRefusalCause` porte la cause du plancher.

// Condition #2 + #7 : plancher dur d'injection d'acide.
bool PumpControllerClass::isPhRawFloorBlocking() {
  // Produit basique (pH+) : aucune acidification possible → plancher sans objet.
  if (mqttCfg.phCorrectionType == "ph_plus") {
    _phRawFloorActive = false;
    return false;
  }
  const float phRaw = sensors.getPhFilteredRaw();
  // Mesure indisponible : les gardes NaN existantes (canDose / filtre non prêt)
  // traitent déjà le cas — ne pas ajouter de refus fantôme ni compter un refus.
  if (isnan(phRaw)) {
    _phRawFloorActive = false;
    return false;
  }
  const float offset = sensors.getPhOffset();
  // Palier élargi quand l'offset est large : l'incertitude sur le pH réel croît
  // avec |offset|, la marge de sécurité doit croître avec elle.
  const float floorPh = (fabsf(offset) > kPhOffsetWideThreshold)
                            ? kPhRawDosingFloorWide
                            : kPhRawDosingFloor;
  if (phRaw >= floorPh) {
    if (_phRawFloorActive) {
      systemLogger.info("[Dosage pH] pH brut remonté à " + String(phRaw, 2) +
                        " (plancher " + String(floorPh, 2) + ") — injection d'acide de nouveau possible");
      _phRawFloorActive = false;
    }
    return false;
  }

  // --- Sous le plancher : refus, log critical + alerte MQTT à la TRANSITION ---
  if (!_phRawFloorActive) {
    _phRawFloorActive = true;
    const String msg = "pH brut " + String(phRaw, 2) + " sous le plancher de sécurité " +
                       String(floorPh, 2) + " (offset=" + String(offset, 2) +
                       ") — injection d'acide refusée";
    systemLogger.critical("[Dosage pH] " + msg);
    mqttManager.publishAlert("ph_raw_floor", msg);
    // Cause exposée au WS (canDose n'est pas appelé tant que le plancher bloque).
    _lastRefusalCause[0] = "pH brut sous plancher de sécurité (" + String(phRaw, 2) +
                           " < " + String(floorPh, 2) + ")";

    // Condition #7 : compteur de refus sur fenêtre glissante de 24 h.
    const uint32_t nowMs = (uint32_t)millis();
    if (_phRawFloorRefusals == 0 ||
        (nowMs - _phRawFloorWindowStart) >= kPhRawFloorRefusalWindowMs) {
      _phRawFloorWindowStart = nowMs;
      _phRawFloorRefusals = 0;
    }
    if (_phRawFloorRefusals < 255) _phRawFloorRefusals++;
    if (_phRawFloorRefusals >= kPhRawFloorRefusalMax &&
        mqttCfg.phRegulationMode != "manual") {
      // Répétition = sonde morte ou offset saisi à l'envers, pas un incident
      // ponctuel : on ferme la régulation pH (fail-closed) et on le dit.
      mqttCfg.phRegulationMode = "manual";
      // INVARIANT DU PROJET (revue) : `ph_enabled` est le miroir de « mode !=
      // manual » (ws_manager.cpp, web_routes_config.cpp, mqtt_manager.cpp). Le
      // laisser à true ici persisterait en NVS un état incohérent : l'UI et Home
      // Assistant afficheraient la régulation pH ACTIVE après un basculement de
      // sécurité, exactement quand il faut que l'utilisateur voie qu'elle ne
      // l'est plus.
      mqttCfg.phEnabled = false;
      const String alert = String(kPhRawFloorRefusalMax) +
                           " refus de plancher pH en 24 h — sonde pH à recalibrer ou "
                           "remplacer ; régulation pH basculée en manuel";
      // v2.20.3 (C4) : PERSISTER la cause de la bascule AVANT saveMqttConfig(),
      // pour qu'elle parte dans la même transaction NVS que le mode lui-même.
      // Le journal système est un ring buffer : après quelques heures il ne reste
      // AUCUNE trace de l'escalade (incident 2026-08-16 — impossible de dire si
      // le mode `manual` venait de l'escalade ou d'une action humaine).
      setPhAutoManualTrace(alert);
      saveMqttConfig();
      systemLogger.critical("[Dosage pH] " + alert);
      mqttManager.publishAlert("ph_raw_floor", alert);
      _lastRefusalCause[0] = "régulation pH basculée en manuel (refus plancher répétés)";
    }
  }
  return true;
}

// Condition #3 : inhibition du dosage ORP sur pH brut élevé.
// `requireKnownPh` = true pour la régulation AUTOMATIQUE (fail-closed sur pH
// inconnu), false pour le dosage PROGRAMMÉ (comportement historique préservé).
//
// POURQUOI LE FAIL-CLOSED EN AUTO (pool-chemistry P2, correctif bloquant) :
// `canDose(1)` ne consulte JAMAIS le pH — ce filet est donc l'UNIQUE garde du
// couplage pH/chlore. Si la sonde pH est morte alors que l'eau est réellement
// au-dessus de 7,60, le chlore y est peu actif (fraction HClO effondrée) et
// l'asservissement ORP compenserait en surdosant. Un pH inconnu doit donc
// suspendre la chloration automatique, pas la laisser courir à l'aveugle.
bool PumpControllerClass::isOrpInhibitedByPhRaw(bool requireKnownPh) {
  const float phRaw = sensors.getPhFilteredRaw();

  if (isnan(phRaw)) {
    if (!requireKnownPh) {
      // Dosage programmé : aveugle à la mesure par conception, on ne change rien.
      _orpPhRawInhibitActive = false;
      _orpPhRawInhibitKind = 0;
      return false;
    }
    // Auto : pH inconnu → inhibition. Journalisé en INFO (situation transitoire
    // normale au démarrage ou pendant un warmup de filtre), le CRITICAL reste
    // réservé à un vrai franchissement de seuil.
    //
    // v2.20.3 (C6) : le système sait DÉJÀ pourquoi le pH est indisponible quand
    // c'est lui qui l'a décidé. Si `measure_validity` déclare les mesures non
    // représentatives (purge de la chambre de mesure après une reprise de
    // circulation), on le dit ainsi — annoncer un « capteur muet » à CHAQUE
    // reprise de filtration transforme un état normal en fausse alerte et noie
    // le vrai défaut (incident 2026-08-16 : ce log est le seul indice qui a été
    // émis pendant les 3 h sans régulation, et il était trompeur).
    if (!_orpPhRawInhibitActive) {
      _orpPhRawInhibitActive = true;
      bool waterPresent = false, measuresValid = false;
      uint32_t remainingS = 0;
      measureValidity.getSnapshot(waterPresent, measuresValid, remainingS);
      if (!measuresValid) {
        _orpPhRawInhibitKind = 2;
        systemLogger.info("[Dosage ORP] Stabilisation des mesures en cours — dosage "
                          "chlore automatique suspendu (reprise automatique dès que "
                          "les mesures sont de nouveau représentatives)");
        _lastRefusalCause[1] = "stabilisation des mesures — dosage chlore suspendu";
      } else {
        _orpPhRawInhibitKind = 1;
        systemLogger.info("[Dosage ORP] pH brut indisponible (capteur muet ou filtre "
                          "non amorcé) — dosage chlore automatique suspendu par sécurité");
        _lastRefusalCause[1] = "pH brut indisponible — dosage chlore suspendu";
      }
    }
    return true;
  }

  if (phRaw <= kPhRawOrpInhibit) {
    if (_orpPhRawInhibitActive) {
      // v2.20.3 (C6) : le message de levée doit refléter la cause réellement
      // journalisée à la pose, sinon « redescendu à 7,10 » sort après un simple
      // warmup où le pH n'était jamais monté.
      if (_orpPhRawInhibitKind == 2) {
        systemLogger.info("[Dosage ORP] Stabilisation terminée, pH brut de nouveau "
                          "mesuré (" + String(phRaw, 2) +
                          ") — dosage chlore de nouveau possible");
      } else if (_orpPhRawInhibitKind == 1) {
        systemLogger.info("[Dosage ORP] pH brut de nouveau disponible (" +
                          String(phRaw, 2) + ") — dosage chlore de nouveau possible");
      } else {
        systemLogger.info("[Dosage ORP] pH brut redescendu à " + String(phRaw, 2) +
                          " — dosage chlore de nouveau possible");
      }
    }
    _orpPhRawInhibitActive = false;
    _orpPhRawInhibitKind = 0;
    return false;
  }

  if (!_orpPhRawInhibitActive) {
    _orpPhRawInhibitActive = true;
    _orpPhRawInhibitKind = 0;  // v2.20.3 (C6) : vrai franchissement de seuil
    const String msg = "pH brut " + String(phRaw, 2) + " > " + String(kPhRawOrpInhibit, 2) +
                       " (offset=" + String(sensors.getPhOffset(), 2) +
                       ") — chlore peu actif à ce pH : dosage ORP suspendu (faire baisser le pH)";
    systemLogger.critical("[Dosage ORP] " + msg);
    mqttManager.publishAlert("orp_ph_raw_inhibit", msg);
    // Cause exposée au WS (canDose(1) n'est pas appelé tant que l'inhibition dure).
    _lastRefusalCause[1] = "pH brut trop élevé (" + String(phRaw, 2) +
                           ") — dosage chlore suspendu";
  }
  return true;
}

// Ring buffer anti-rafale (pool-chemistry Pass 3.5).
// Enregistre l'instant `millis()` du démarrage d'un cycle de dosage pour la
// pompe `pumpIndex`. Index circulaire borné par kDosingCycleHistorySize.
void PumpControllerClass::recordDosingCycleStart(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  uint32_t now = millis();
  // COQUILLE (feature-039) : délègue au pur ; réassigner le retour est impératif
  // (sinon index figé = écriture toujours sur le même slot = emballement pompe).
  _dosingCycleHistoryIdx[pumpIndex] = recordCycleTimestamp(
      _dosingCycleHistory[pumpIndex], _dosingCycleHistoryIdx[pumpIndex],
      kDosingCycleHistorySize, now);
}

// Compte les démarrages de cycle survenus dans la fenêtre [now-windowMs, now]
// pour la pompe donnée. Itération O(kDosingCycleHistorySize) — négligeable
// (20 comparaisons) appelée au plus 2 fois par tour de update().
int PumpControllerClass::countRecentDosingCycles(int pumpIndex, uint32_t windowMs) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  uint32_t now = millis();
  // COQUILLE (feature-039) : délègue au pur (gestion wrap millis, frontière <=).
  return countCyclesInWindow(_dosingCycleHistory[pumpIndex], kDosingCycleHistorySize, now, windowMs);
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
  // 2. Présence d'eau (feature-056) : source UNIQUE resolveWaterPresent() selon
  // le mode d'installation (Managed=commandé, Powered=présumé, External=signal frais).
  in.waterPresent = filtration.resolveWaterPresence().waterPresent;
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
  // feature-053 : la limite journalière ORP est la valeur EFFECTIVE (boostée si
  // Mode Boost actif ET orpMode==automatic ; sinon normale — le getter self-gate).
  in.maxDailyMl = (pumpIndex == 0) ? safetyLimits.maxPhMlPerDay
                                   : getEffectiveMaxChlorineMlPerDay();
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

// =============================================================================
// Itération 2 (bug-dosage-fantome, option B+C) — caractérisation des pompes
// =============================================================================
// dutyToFlow()/flowToDuty() SUPPRIMÉS : le modèle linéaire duty↔débit est mort
// (mesure au banc 2026-08-23 : ×1,9 de sous-estimation à duty 82, ×1,5 de
// surestimation à 255). Le débit moyen est réalisé par proportionnel de TEMPS
// à une amplitude UNIQUE dont le débit a été MESURÉ (pulse_dosing_logic).

// Helpers file-static — index de pompe PHYSIQUE (0 = pompe 1, 1 = pompe 2).
static uint8_t pumpCalibratedDuty(int physIdx) {
  return (physIdx == 0) ? mqttCfg.pump1CalibratedDuty : mqttCfg.pump2CalibratedDuty;
}
static float pumpPulseFlow(int physIdx) {
  return (physIdx == 0) ? mqttCfg.pump1PulseFlowMlPerMin : mqttCfg.pump2PulseFlowMlPerMin;
}
static float pumpFlow255(int physIdx) {
  return (physIdx == 0) ? kPump1Flow255MlPerMin : kPump2Flow255MlPerMin;
}

// Amplitude courante du plateau d'impulsion : pumpNMaxDutyPct × 255 / 100.
uint8_t PumpControllerClass::getPumpAmplitudeDuty(int pumpIndex) const {
  if (pumpIndex < 0 || pumpIndex > 1) return 0;
  uint16_t pct = (pumpIndex == 0) ? mqttCfg.pump1MaxDutyPct : mqttCfg.pump2MaxDutyPct;
  if (pct > 100) pct = 100;
  return (uint8_t)((pct * MAX_PWM_DUTY) / 100);
}

// Badge « débit non caractérisé » (UI/WS) : reflète la garde de caractérisation
// (evaluatePulseGate). Index invalide → true (fail-closed).
bool PumpControllerClass::isPumpFlowUncharacterized(int pumpIndex) const {
  if (pumpIndex < 0 || pumpIndex > 1) return true;
  return evaluatePulseGate(getPumpAmplitudeDuty(pumpIndex),
                           pumpCalibratedDuty(pumpIndex),
                           pumpPulseFlow(pumpIndex),
                           MIN_ACTIVE_DUTY) != PulseRefusal::None;
}

// =============================================================================
// Itération 3 — calendrier d'impulsion exposé à l'UI/WS (lecture seule)
// =============================================================================
// chem 0 = pH, 1 = ORP. Course bénigne assumée (patron isAnyPumpEnergized /
// getPumpAmplitudeDuty) : les champs sont écrits par loopTask (update) et lus
// ici en contexte web async ; lectures 8/32 bits atomiques sur Xtensa, pire cas
// un instantané en retard d'un tour de loop. Aucun setter, aucun effet dosage.

// Session de dosage en cours (auto OU scheduled). false si index invalide.
bool PumpControllerClass::isPulseSessionActive(int chem) const {
  if (chem < 0 || chem > 1) return false;
  return _pulseSession[chem];
}

// Durée du pouls planifié pour la période courante (ms). 0 si index invalide.
uint32_t PumpControllerClass::getPulseMs(int chem) const {
  if (chem < 0 || chem > 1) return 0;
  return _pulseMs[chem];
}

// Position dans la période d'impulsion courante : nowMs − periodStartMs en
// arithmétique uint32 NON SIGNÉE (wrap-safe au rollover de millis), clampée
// [0..kPulsePeriodMs]. 0 si session inactive ou index invalide.
uint32_t PumpControllerClass::getPulsePhaseMs(int chem, uint32_t nowMs) const {
  if (chem < 0 || chem > 1) return 0;
  if (!_pulseSession[chem]) return 0;
  uint32_t phase = nowMs - _pulseState[chem].periodStartMs;
  if (phase > kPulsePeriodMs) phase = kPulsePeriodMs;
  return phase;
}

// feature-053 : cible ORP effective (surcouche Mode Boost). Délègue à la logique
// pure ; l'effet est réservé au mode ORP "automatic" ET au boost actif.
float PumpControllerClass::getEffectiveOrpTarget() const {
  bool boost = isBoostActive(time(nullptr));
  bool autoMode = (mqttCfg.orpRegulationMode == "automatic");
  return effectiveOrpTargetPure(mqttCfg.orpTarget, boost, autoMode,
                                kBoostOrpDeltaMv, kBoostOrpCeilingMv);
}

// feature-053 : limite journalière chlore effective (surcouche Mode Boost).
// Le mode scheduled/manual voit toujours la limite NORMALE (le pur gère le gate
// via autoMode — condition pool-chemistry #1).
float PumpControllerClass::getEffectiveMaxChlorineMlPerDay() const {
  bool boost = isBoostActive(time(nullptr));
  bool autoMode = (mqttCfg.orpRegulationMode == "automatic");
  return effectiveMaxChlorinePure(safetyLimits.maxChlorineMlPerDay, boost, autoMode,
                                  kBoostDailyFactor, kBoostDailyHardCapMl);
}

// Reset journalier des compteurs ml + flags limite — appelé depuis update() avant canDose()
// pour que la réinitialisation à minuit se produise même quand la filtration est arrêtée.
// Sans cela, le compteur affichait la valeur de la veille tant que la filtration n'avait pas tourné.
void PumpControllerClass::tickDailyRollover() {
  unsigned long now = millis();

  // feature-053 (condition pool-chemistry #3) : expirer le boost au passage de
  // minuit AVANT tout reset de compteur, dans les DEUX branches de rollover.
  tickBoostExpiry(time(nullptr));

  // feature-011 (A6) : au passage de minuit, tracer le reliquat scheduled non
  // injecté (pas de rattrapage J+1 — décision produit : le reliquat est PERDU)
  // et réarmer la répartition (_*SchedWindowIdx=-1 → recalcul du volume à la
  // première fenêtre du nouveau jour). Appelé AVANT le reset des compteurs
  // dans les deux chemins de rollover (date NTP et fallback 24 h millis).
  auto resetScheduledSplit = [this]() {
    if (mqttCfg.phRegulationMode == "scheduled" && mqttCfg.phDailyTargetMl > 0) {
      float eff = static_cast<float>(mqttCfg.phDailyTargetMl);
      if (safetyLimits.maxPhMlPerDay > 0.0f && eff > safetyLimits.maxPhMlPerDay) {
        eff = safetyLimits.maxPhMlPerDay;
      }
      if (safetyLimits.dailyPhInjectedMl < eff) {
        systemLogger.info("[Scheduled] Reliquat pH perdu au passage de minuit : " +
          String(eff - safetyLimits.dailyPhInjectedMl, 0) + "mL non injectés (" +
          String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(eff, 0) + "mL)");
      }
    }
    if (mqttCfg.orpRegulationMode == "scheduled" && mqttCfg.orpDailyTargetMl > 0) {
      float eff = static_cast<float>(mqttCfg.orpDailyTargetMl);
      if (safetyLimits.maxChlorineMlPerDay > 0.0f && eff > safetyLimits.maxChlorineMlPerDay) {
        eff = safetyLimits.maxChlorineMlPerDay;
      }
      if (safetyLimits.dailyOrpInjectedMl < eff) {
        systemLogger.info("[Scheduled ORP] Reliquat ORP perdu au passage de minuit : " +
          String(eff - safetyLimits.dailyOrpInjectedMl, 0) + "mL non injectés (" +
          String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(eff, 0) + "mL)");
      }
    }
    _phSchedWindowIdx = -1;
    _phSchedStopTargetMl = 0.0f;
    _orpSchedWindowIdx = -1;
    _orpSchedStopTargetMl = 0.0f;
  };

  time_t epochNow = time(nullptr);
  if (epochNow >= kMinValidEpoch) {
    struct tm timeinfo;
    localtime_r(&epochNow, &timeinfo);
    char todayStr[9];
    strftime(todayStr, sizeof(todayStr), "%Y%m%d", &timeinfo);
    if (shouldRolloverByDate(safetyLimits.currentDayDate, todayStr)) {
      systemLogger.info("Reset journalier (minuit local) — pH=" +
        String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(safetyLimits.maxPhMlPerDay, 0) +
        " mL, ORP=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(safetyLimits.maxChlorineMlPerDay, 0) + " mL");
      resetScheduledSplit();  // feature-011 : reliquat scheduled + réarmement fenêtres
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
    if (shouldRolloverByMillis((uint32_t)safetyLimits.dayStartTimestamp, (uint32_t)now)) {
      resetScheduledSplit();  // feature-011 : reliquat scheduled + réarmement fenêtres
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
    if (safetyLimits.dailyPhInjectedMl >= safetyLimits.maxPhMlPerDay) {
      if (!safetyLimits.phLimitReached) {
        String corrType = (mqttCfg.phCorrectionType == "ph_plus") ? "pH+" : "pH-";
        systemLogger.critical("LIMITE JOURNALIÈRE " + corrType + " ATTEINTE: " + String(safetyLimits.dailyPhInjectedMl) + " ml");
        safetyLimits.phLimitReached = true;
      }
      return false;
    } else if (safetyLimits.phLimitReached) {
      // Dé-latch : la condition n'est plus vraie (limite augmentée ou compteurs réinitialisés)
      safetyLimits.phLimitReached = false;
      systemLogger.info("Limite journalière pH levée (limite augmentée ou compteurs réinitialisés) — dosage à nouveau autorisé");
    }
  } else {
    // feature-053 : plafond journalier EFFECTIF (boosté si Mode Boost actif ET
    // orpMode==automatic ; le getter renvoie la limite normale sinon → le mode
    // scheduled/manual ne voit JAMAIS la valeur boostée, condition #1).
    float maxCl = getEffectiveMaxChlorineMlPerDay();
    if (safetyLimits.dailyOrpInjectedMl >= maxCl) {
      if (!safetyLimits.orpLimitReached) {
        systemLogger.critical("LIMITE JOURNALIÈRE CHLORE ATTEINTE: " + String(safetyLimits.dailyOrpInjectedMl) + " ml");
        safetyLimits.orpLimitReached = true;
      }
      return false;
    } else if (safetyLimits.orpLimitReached) {
      // Dé-latch : la condition n'est plus vraie (limite augmentée ou compteurs réinitialisés)
      safetyLimits.orpLimitReached = false;
      systemLogger.info("Limite journalière chlore levée (limite augmentée ou compteurs réinitialisés) — dosage à nouveau autorisé");
    }
  }

  return true;
}

// Crédite un VOLUME (mL) aux compteurs de sécurité journaliers + suivi produit.
// Itération 2 : remplace updateSafetyTracking(flow, delta) — le volume est
// calculé en amont par accountPumpVolume (pulseDeltaVolumeMl exact pour les
// pouls, majorant pour le manuel), plus jamais depuis le modèle linéaire.
void PumpControllerClass::creditInjectedVolume(bool isPhPump, float volumeMl) {
  if (!(volumeMl > 0.0f)) return;  // rejette aussi NaN (défensif)

  if (isPhPump) {
    safetyLimits.dailyPhInjectedMl += volumeMl;
    if (productCfg.phTrackingEnabled) {
      productCfg.phTotalInjectedMl += volumeMl;
      productConfigDirty = true;
    }
  } else {
    safetyLimits.dailyOrpInjectedMl += volumeMl;
    if (productCfg.orpTrackingEnabled) {
      productCfg.orpTotalInjectedMl += volumeMl;
      productConfigDirty = true;
    }
  }
  _dailyCountersDirty = true;
}

// Comptabilité par delta de tick (itération 2, P4). Attribution ARRIÈRE : le
// volume du delta [lastSafetyTimestamp, now] est celui de la commande posée au
// tour PRÉCÉDENT (_acctPumpOn/_acctManual) — la pompe garde son duty entre deux
// ticks. Exactitude indépendante du jitter de loop : pulseDeltaVolumeMl intègre
// la géométrie du pouls (kick à flow255 mesuré + plateau au débit calibré), pas
// les instants d'échantillonnage. Manuel (?duty=N) : crédit au MAJORANT flow255
// (surestimer est fail-safe), SAUF duty == calibratedDuty → débit mesuré.
// Jamais zéro sur une pompe alimentée (P4).
void PumpControllerClass::accountPumpVolume(int chem, int physIdx,
                                            unsigned long now, bool manualActive) {
  DosingState& st = (chem == 0) ? phDosingState : orpDosingState;

  if (_acctPumpOn[chem] && st.lastSafetyTimestamp != 0) {
    unsigned long deltaMs = now - st.lastSafetyTimestamp;
    if (deltaMs > 0) {
      float volMl;
      if (_acctManual[chem]) {
        uint8_t calDuty = pumpCalibratedDuty(physIdx);
        float pulseFlow = pumpPulseFlow(physIdx);
        float f = (calDuty != 0 && _acctManualDuty[chem] == calDuty && pulseFlow > 0.0f)
                      ? pulseFlow          // duty calibré → débit MESURÉ
                      : pumpFlow255(physIdx);  // sinon MAJORANT (fail-safe)
        volMl = f * (float)deltaMs / 60000.0f;
      } else {
        volMl = pulseDeltaVolumeMl(_pulseState[chem].pulseStartMs, kPulseKickMs,
                                   (uint32_t)st.lastSafetyTimestamp, (uint32_t)now,
                                   pumpFlow255(physIdx), pumpPulseFlow(physIdx));
        // Cumuls de session (log d'arrêt + garde temps minimum pompe-ON).
        if (_pulseSession[chem]) {
          _sessionVolMl[chem] += volMl;
          _sessionOnMs[chem] += deltaMs;
        }
      }
      creditInjectedVolume(chem == 0, volMl);
    }
  }

  // État pour le PROCHAIN tour : pumpDuty reflète la commande appliquée CE tour.
  bool onNow = (pumpDuty[physIdx] > 0);
  st.lastSafetyTimestamp = onNow ? now : 0;
  _acctPumpOn[chem] = onNow;
  _acctManual[chem] = manualActive;
  _acctManualDuty[chem] = manualActive ? pumpDuty[physIdx] : 0;
}

// Crédit plancher fin d'injection manuelle volumée (bug sous-compte v2.9.1).
// Exécuté en loopTask (via updateManualInject) : mêmes règles d'accès aux
// compteurs que updateSafetyTracking (pas de mutex, écrivain unique).
// NE touche PAS au suivi produit productCfg.*TotalInjectedMl : le suivi de
// stock reste sur l'intégration réelle débit×temps ; seul le registre de
// SÉCURITÉ journalier est planché (fail-safe chimique, jamais de sous-compte).
void PumpControllerClass::creditManualInjectionFloor(int idx, float startCumulMl, float creditMl) {
  if (creditMl <= 0.0f) return;  // 0 = pas de crédit dû
  float& daily = (idx == 0) ? safetyLimits.dailyPhInjectedMl
                            : safetyLimits.dailyOrpInjectedMl;
  // Rollover minuit pendant l'injection (daily < startCumulMl) : crédit du
  // volume ENTIER au jour nouveau — choix conservateur (sur-compte possible).
  float floorMl = (daily < startCumulMl) ? creditMl : (startCumulMl + creditMl);
  if (daily < floorMl) {
    systemLogger.info(String("[Sécurité] Compteur journalier ") + (idx == 0 ? "pH" : "ORP") +
                      " ajusté de " + String(daily, 1) + " à " + String(floorMl, 1) +
                      " mL (crédit fin d'injection manuelle)");
    daily = floorMl;
    _dailyCountersDirty = true;  // flush NVS différé (30 s)
  }
}

void PumpControllerClass::update() {
  unsigned long now = millis();

  // feature-011 : par défaut le débit planifié scheduled est indéfini (NAN →
  // null côté WS). Seule la branche scheduled le renseigne, quand elle
  // s'exécute avec une heure valide et un horizon > 0. Toute sortie anticipée
  // (OTA, capteurs non initialisés, filtration arrêtée, changement de mode,
  // limite horaire/journalière) le laisse donc à NAN. Les index de fenêtre
  // (_*SchedWindowIdx), eux, persistent entre les tours.
  _phSchedPlannedFlow = NAN;
  _orpSchedPlannedFlow = NAN;

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

  // feature-006 : consommer les démarrages de cycle MANUELS demandés depuis les
  // handlers web async (requestManualCycleRecord). Résolu ici, en loopTask, AVANT
  // les branches de régulation : le ring anti-rafale et cyclesToday restent écrits
  // par une SEULE tâche (pas de mutex). Placé avant tout early-return pour ne
  // jamais laisser un pending bloqué.
  for (int i = 0; i < 2; ++i) {
    uint8_t pending = _manualCycleStartPending[i].exchange(0);
    if (pending == 0) continue;
    DosingState& st = (i == 0) ? phDosingState : orpDosingState;
    // Même rollover 24 h que shouldStartDosing : sans lui, des cycles purement
    // manuels (aucun passage par la branche auto) ne seraient jamais remis à zéro.
    if (st.cyclesDayStart == 0 || now - st.cyclesDayStart >= 86400000UL) {
      st.cyclesToday = 0;
      st.cyclesDayStart = now;
    }
    for (uint8_t k = 0; k < pending; ++k) {
      recordDosingCycleStart(i);  // ring anti-rafale PARTAGÉ avec l'auto
      st.cyclesToday++;           // compteur cycles/jour PARTAGÉ avec l'auto
    }
    _dailyCountersDirty = true;   // flush NVS différé (30 s) des compteurs
  }

  if (otaInProgress) {
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    return;
  }

  // Auto-guérison v2.9.1 (condition pool-chemistry C2) : mode maintenance →
  // dosage entièrement suspendu (auto, manuel ET scheduled), pompes forcées à 0
  // à chaque cycle. Gate identique à OTA, placée AVANT toute évaluation de dosage.
  if (maintenanceInhibit) {
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    return;
  }

  // feature-075 : snapshot du flag de suspension « assistant de mesure du
  // débit » (volatile, écrit par le handler web async) + log info ONE-SHOT à
  // chaque transition (pose/levée). La fermeture effective passe par le chemin
  // EXISTANT de la perte de filtration (garde structurelle plus bas) : états
  // de dosage AUTO et SCHEDULED refermés (dosingState.active = false), pompes
  // non-manuelles forcées à 0 ; le drapeau de session pouls périmé est absorbé
  // à la reprise (« reprise après suspension hors-branche », compteurs de
  // session remis à zéro). Le mode MANUEL — véhicule de l'assistant (purge et
  // marche de mesure minutées) — reste pleinement fonctionnel.
  const bool flowAssistSuspend = _flowAssistSuspend;
  if (flowAssistSuspend != _flowAssistSuspendPrev) {
    _flowAssistSuspendPrev = flowAssistSuspend;
    if (flowAssistSuspend) {
      systemLogger.info("Dosage automatique suspendu (assistant de mesure du débit)");
    } else {
      systemLogger.info("Dosage automatique rétabli (fin de l'assistant de mesure du débit)");
    }
  }

  // feature-006 (condition pool-chemistry #2) : budget horaire usedMs PARTAGÉ
  // auto + manuel. Itération 2 (bug-dosage-fantome) : le prédicat devient
  // POMPE-ON — pumpDuty reflète ici la commande appliquée au tour PRÉCÉDENT
  // (attribution arrière du delta écoulé), toutes origines confondues (pouls
  // auto, scheduled, manuel). Entre deux pouls d'une session, le budget horaire
  // n'est PAS consommé.
  int phBudgetIdx  = pumpIndexFromNumber(mqttCfg.phPump);
  int orpBudgetIdx = pumpIndexFromNumber(mqttCfg.orpPump);
  refreshDosingState(phDosingState, now, pumpDuty[phBudgetIdx] > 0);
  refreshDosingState(orpDosingState, now, pumpDuty[orpBudgetIdx] > 0);

  if (!sensors.isInitialized()) {
    phDosingState.active = false;
    orpDosingState.active = false;
    // Respect du mode manuel (test développement) — cohérent avec le bloc canDose() ci-dessous
    if (!manualMode[0]) applyPumpDuty(0, 0);
    if (!manualMode[1]) applyPumpDuty(1, 0);
    return;
  }

  // Garde structurelle "globale" : présence d'eau + stabilisation post-cal.
  // Note importante : on ne peut pas appeler canDose(0/1) ici parce qu'il filtre AUSSI
  // sur le mode régulation (== "automatic"), or la branche `scheduled` doit pouvoir
  // tourner même quand le capteur est NaN (intentionnellement aveugle, cf. spec).
  // La garde "automatic + capteur valide + calibration" est appliquée plus bas dans
  // les branches dédiées (canDose(0) et canDose(1)) — défense en profondeur.
  // feature-056 : source UNIQUE resolveWaterPresent() (Managed/Powered/External).
  bool filtrationOk = filtration.resolveWaterPresence().waterPresent;
  bool stabilizationActive = isStabilizationTimerActive(0) ||
                             isStabilizationTimerActive(1);
  // feature-075 : la suspension « assistant de mesure du débit » emprunte ce
  // MÊME chemin de fermeture (aucune duplication) — mêmes garanties que la
  // perte de filtration : auto + scheduled refermés, manuel préservé.
  if (!filtrationOk || stabilizationActive || flowAssistSuspend) {
    // Arrêter le dosage en cours si l'eau n'est plus présente / stabilisation
    // active / assistant de mesure du débit en cours
    if (phDosingState.active || orpDosingState.active) {
      const char* reason = !filtrationOk ? "eau absente (filtration arrêtée / signal externe périmé)"
                         : stabilizationActive ? "stabilisation post-cal en cours"
                                               : "assistant de mesure du débit";
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
  // feature-067 (pool-chemistry #2) : isPhRawFloorBlocking() est évalué AVANT
  // canDose(0) — quand le plancher bloque, aucune évaluation de dosage n'a lieu
  // et la cause de refus exposée au WS est celle du plancher.
  const String& phMode = mqttCfg.phRegulationMode;
  if (phMode == "automatic" && phLimitOk && phSafetyOk && !isPhRawFloorBlocking() && canDose(0)) {
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
    // bug-dosage-fantome (défaut n°2) : la DÉCISION reste ici, la
    // COMPTABILISATION du cycle est différée après le calcul du débit (bloc
    // « comptabilisation différée » plus bas).
    const bool phWasActiveTick = phDosingState.active;

    if (phWasActiveTick) {
      // Déjà en cours : vérifier si on continue. Itération 2 : la garde de
      // temps minimum raisonne en temps POMPE-ON cumulé de la session.
      shouldDose = shouldContinueDosing(error, pumpProtection.phStopThreshold, _sessionOnMs[0]);
    } else {
      // Arrêté : vérifier si on démarre
      shouldDose = shouldStartDosing(error, pumpProtection.phStartThreshold, phDosingState, now);
    }
    const bool phStartDecided = !phWasActiveTick && shouldDose;

    // --- Itération 2 (option B+C) : caractérisation de la pompe pH affectée ---
    const int phPumpIdx = pumpIndexFromNumber(mqttCfg.phPump);
    const uint8_t phAmpDuty = getPumpAmplitudeDuty(phPumpIdx);
    const float phAmpFlow = pumpPulseFlow(phPumpIdx);  // débit MESURÉ à l'amplitude
    bool phGateRefused = false;  // pompe non caractérisée / amplitude sous plancher
    // feature-073 : refus du PLAN = entrées invalides uniquement (fail-closed) —
    // la consigne sous le pouls minimal est désormais ACCUMULÉE, plus refusée.
    bool phPlanRefused = false;

    if (shouldDose) {
      // Garde de caractérisation AVANT tout calcul : une pompe jamais pesée
      // (cal_duty=0), un débit implausible ou une amplitude ≠ calibrée REFUSENT
      // le dosage (fail-closed P4 : on ne crédite jamais un volume inconnu).
      PulseRefusal gate = evaluatePulseGate(phAmpDuty, pumpCalibratedDuty(phPumpIdx),
                                            phAmpFlow, MIN_ACTIVE_DUTY);
      if (gate != PulseRefusal::None) {
        phGateRefused = true;
        shouldDose = false;  // termine aussi une éventuelle session en cours
        // État de config persistant → critical THROTTLÉ (30 min), pas de spam.
        static unsigned long phGateCritMs = 0;
        if (phGateCritMs == 0 || now - phGateCritMs >= kPulseRefusalLogIntervalMs) {
          phGateCritMs = now;
          if (gate == PulseRefusal::AmplitudeBelowMinActive) {
            // Condition B2 : jamais dégrader en silence sous le seuil de rotation.
            systemLogger.critical("[Dosage pH] Amplitude pompe (" + String(phAmpDuty) +
              "/255) sous le plancher de rotation (" + String(MIN_ACTIVE_DUTY) +
              ") — dosage automatique BLOQUÉ (relever la puissance pompe et recalibrer)");
          } else {
            systemLogger.critical("[Dosage pH] Pompe non caractérisée (duty calibré=" +
              String(pumpCalibratedDuty(phPumpIdx)) + ", amplitude=" + String(phAmpDuty) +
              ", débit=" + String(phAmpFlow, 1) +
              " mL/min) — dosage automatique BLOQUÉ (pesée au banc requise)");
          }
        }
      }
    }

    float flow = 0.0f;  // demande de débit MOYEN (mL/min), réalisée par le pouls
    if (shouldDose) {
      // feature-025 (B6) : geler l'intégrale si la sortie sature (terme P seul
      // ≥ débit d'amplitude → toute accumulation serait du windup).
      bool phSaturated = (phPID.kp * error >= phAmpFlow);
      // computePID renvoie le débit FINAL borné [0, ampFlow] — itération 2 : le
      // plancher minFlow (clamp vers le haut du modèle linéaire) est SUPPRIMÉ ;
      // feature-073 : sous le pouls minimal la demande est servie par
      // l'ACCUMULATEUR de planPulseWithCarry (plus de deadband de refus).
      flow = computePID(phPID, error, now,
                        pumpProtection.phStartThreshold, phSaturated,
                        0.0f, phAmpFlow);
    } else {
      // Reset PID si erreur négative (pH hors plage de correction)
      if (error < -pumpProtection.phStopThreshold) {
        phPID.integral = 0.0f;
        phPID.lastError = 0.0f;
        phPID.lastTime = 0;
      }
    }

    // --- Ordonnanceur d'impulsions (proportionnel de temps, pulse_dosing_logic) ---
    const uint32_t now32 = (uint32_t)now;
    if (flow > 0.0f) {
      // Plan par PÉRIODE : recalculé à l'entrée de session et à chaque frontière
      // de période (la demande PID courante fixe la durée du prochain pouls).
      bool newPeriod = !_pulseSession[0] ||
                       (now32 - _pulseState[0].periodStartMs) >= kPulsePeriodMs;
      if (newPeriod) {
        // Ouverture / reprise de séance AVANT le plan (feature-073) : la séance
        // s'ouvre AUSSI en régime d'accumulation (demande vivante sous le
        // plancher — décision gate n°2 : une seule séance, jamais une par pouls)
        // et l'accumulateur repart de zéro à chaque ouverture (décision n°3 :
        // aucune dette héritée). `phStartDecided` couvre aussi la reprise après
        // une suspension hors-branche (filtration/OTA) qui aurait laissé un
        // drapeau de session périmé — les compteurs repartent alors de zéro.
        if (!_pulseSession[0] || phStartDecided) {
          _pulseSession[0] = true;
          _sessionOnMs[0] = 0;
          _sessionVolMl[0] = 0.0f;
          _pulseState[0] = {now32, now32, true};
          _pulseCarryMl[0] = 0.0f;
          _carryPulseLogged[0] = false;
        }
        // feature-073 : sous le plancher (ex-refus seuil C), la demande est
        // CUMULÉE de période en période ; dès que le cumul vaut le volume RÉEL
        // d'un pouls minimal (kick inclus), une impulsion de kPulseMinMs part
        // par le chemin STANDARD (tickPulse, usedMs, volume pesé — aucun
        // passe-droit sur les gardes, réévaluées à chaque tour en amont).
        // ≥ plancher : planPulse INTACT, accumulateur abandonné (F2).
        PulseCarryPlan plan = planPulseWithCarry(
            flow, phAmpFlow, pumpFlow255(phPumpIdx), kPulsePeriodMs,
            kPulseMinMs, kPulseKickMs, _pulseCarryMl[0]);
        _pulseCarryMl[0] = plan.carryMl;
        if (plan.refused) {
          // Entrées invalides (fail-closed, accumulateur remis à zéro par le
          // plan) : pas d'impulsion, la séance se referme au tick — cas
          // structurellement improbable ici (flow > 0 et ampFlow validé par la
          // gate). Ne nourrit pas le streak no-flow (inondation type 2.20.2).
          phPlanRefused = true;
          flow = 0.0f;
        } else {
          // pulseMs == 0 = période d'ACCUMULATION : séance MAINTENUE (flow
          // reste > 0, le latch phActive tient), pompe OFF via tickPulse.
          _pulseMs[0] = plan.pulseMs;
          if (plan.carryFired && !_carryPulseLogged[0]) {
            // A3 : PREMIER tir d'accumulateur de la séance seulement (les
            // suivants sont silencieux — cadence régulière, le log d'arrêt de
            // séance résume le volume total).
            _carryPulseLogged[0] = true;
            systemLogger.info("[Dosage pH] Dosage par accumulation : impulsion minimale de " +
              String(kPulseMinMs / 1000UL) + " s (" +
              String(plan.carryMl + pulseDeltaVolumeMl(0, kPulseKickMs, 0, kPulseMinMs,
                                                       pumpFlow255(phPumpIdx), phAmpFlow), 1) +
              " mL cumulés, résidu " + String(plan.carryMl, 1) + " mL)");
          }
        }
      }
    }

    if (flow > 0.0f && _pulseSession[0]) {
      PulseTick tk = tickPulse(_pulseState[0], now32, kPulsePeriodMs,
                               _pulseMs[0], kPulseKickMs);
      _pulseState[0] = tk.next;
      if (tk.pumpOn) {
        // Kick à pleine puissance en tête de pouls (couple de décollage), puis
        // plateau à l'amplitude CALIBRÉE — plus jamais de duty issu d'un modèle.
        uint8_t duty = tk.kickPhase ? MAX_PWM_DUTY : phAmpDuty;
        if (duty > desiredDuty[phPumpIdx]) desiredDuty[phPumpIdx] = duty;
      }
      // `active` = SESSION en cours (la pompe n'est alimentée que pendant les
      // pouls) — les impulsions internes ne comptent ni cycle ni anti-rafale.
      phActive = true;
      phFlow = flow;
    }

    // --- Fin de session (erreur résorbée, gate, entrées de plan invalides…) ---
    // feature-073 : une demande vivante sous le plancher ne ferme PLUS la
    // séance (régime d'accumulation, pompe OFF entre les tirs).
    if (phWasActiveTick && !phActive) {
      phDosingState.lastStopTime = now;
      // feature-025 : pause mélange hydraulique armée à la FIN DE SESSION
      // (homogénéisation post-dose) — jamais entre deux pouls d'une session.
      notifyPhDose((uint32_t)now);
      unsigned long runTime = (now - phDosingState.lastStartTime) / 1000;
      systemLogger.info("Arrêt dosage pH (auto): session=" + String(runTime) + "s" +
        " pompe-ON=" + String(_sessionOnMs[0] / 1000UL) + "s" +
        " vol≈" + String(_sessionVolMl[0], 1) + "mL" +
        " total jour=" + String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(safetyLimits.maxPhMlPerDay, 0) + "mL");
    }

    // --- Comptabilisation différée du démarrage de cycle (bug-dosage-fantome,
    // défaut n°2) ---
    // Le premier tour d'un cycle rend un débit NUL quand le cycle précédent est
    // loin (computePID rejette dt > 10 s). Comptabiliser sur la seule décision
    // faisait donc : cycle compté mais `active` non latché → le tour suivant
    // repassait par shouldStartDosing et comptait un SECOND cycle (2 lignes de
    // journal, 2 entrées dans le ring anti-rafale, budget cycles/jour de moitié).
    // On comptabilise ici, sur le MÊME prédicat que le latch de `active`
    // (phActive), donc « un cycle compté ⇔ un cycle réellement démarré ».
    // Prescription pool-chemistry P5 : ne PAS synthétiser de dt.
    // SIGNAL À NE PAS PERDRE (revue 2.20.5) : avant ce correctif, une régulation
    // qui décidait de doser sans jamais produire de débit émettait un log de
    // démarrage à CHAQUE tour — bruit involontaire, mais c'est lui qui a révélé
    // le bug. Désormais elle serait totalement silencieuse.
    //
    // ATTENTION au piège que ce garde-fou a lui-même introduit : « décision sans
    // débit » est le cas NORMAL au premier tick d'un cycle (computePID rend 0 tant
    // que dt > 10 s). Alerter dessus produirait un avertissement à chaque cycle,
    // soit ~24/jour en fonctionnement sain — c'est-à-dire l'inondation du journal
    // corrigée en 2.20.2. On n'alerte donc que si la situation PERSISTE sur
    // plusieurs tours consécutifs, ce qui n'arrive pas en régime normal (le tour
    // suivant produit un débit).
    // feature-073 : un refus de plan (phPlanRefused, entrées invalides) reste
    // exclu du streak — fail-closed déjà silencieux par conception ; le régime
    // sous-plancher, lui, laisse désormais phActive=true (accumulation) et ne
    // passe donc jamais ici. Idem pour le refus de gate (phGateRefused) : il a
    // déjà son critical dédié. Le streak reste réservé aux VRAIS cas anormaux
    // (PID muet plusieurs tours).
    static uint16_t phNoFlowStreak = 0;
    static unsigned long phNoFlowWarnMs = 0;
    if (phStartDecided && !phActive && !phPlanRefused && !phGateRefused) {
      if (phNoFlowStreak < 0xFFFF) ++phNoFlowStreak;
      if (phNoFlowStreak >= kDosingNoFlowWarnStreak &&
          (phNoFlowWarnMs == 0 || now - phNoFlowWarnMs >= kDosingNoFlowWarnIntervalMs)) {
        phNoFlowWarnMs = now;
        systemLogger.warning(
            "[Dosage pH] décision de doser SANS débit commandé depuis " +
            String(phNoFlowStreak) + " tours — régulation sans effet (boucle trop "
            "lente, ou consigne sous le plancher de duty). Erreur=" + String(error, 3));
      }
    } else if (phActive) {
      phNoFlowStreak = 0;  // un débit a bien été commandé : rien à signaler
    }

    if (evaluateCycleStartAccounting(phWasActiveTick, phStartDecided, phActive)
            .countCycleStart) {
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
  } else if (phMode == "scheduled" && phLimitOk && phSafetyOk &&
             mqttCfg.phDailyTargetMl > 0 && !isPhRawFloorBlocking()) {
    // feature-067 (pool-chemistry #2) : le mode Programmée est aveugle à la
    // mesure PAR CONCEPTION, mais le plancher acide s'y applique QUAND MÊME —
    // c'est un filet de sécurité chimique, pas une boucle de régulation.
    // Mode Programmée (feature-011) : répartition du volume quotidien par
    // fenêtres de kScheduledWindowMinutes (15 min) sur l'horizon de filtration
    // restant, borné à minuit. Intentionnellement aveugle à la valeur mesurée
    // du pH : l'utilisateur a choisi un volume quotidien fixe indépendamment de
    // la mesure ; un capteur déréglé ne doit pas bloquer l'injection. Seules
    // les gardes volumétriques (phLimitOk, phSafetyOk, maxPhMlPerDay) et
    // structurelles (débit configuré, heure valide, anti-rafale) s'appliquent.
    // phTarget n'est pas utilisé dans cette branche et ne doit pas l'être.
    // COQUILLE : collecte des entrées + application ; la décision est déléguée
    // à evaluateScheduledDose() (dosing_logic, pur, testable en natif).
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
      // Log "plafonné" conservé en coquille — le plafonnement lui-même est
      // AUSSI appliqué dans evaluateScheduledDose (via maxDailyMl) ;
      // effectiveDailyMl ne sert plus ici qu'aux logs.
      float effectiveDailyMl = static_cast<float>(mqttCfg.phDailyTargetMl);
      static bool phCappedLogged = false;
      if (safetyLimits.maxPhMlPerDay > 0.0f && effectiveDailyMl > safetyLimits.maxPhMlPerDay) {
        if (!phCappedLogged) {
          systemLogger.warning("[Scheduled] phDailyTargetMl (" + String(mqttCfg.phDailyTargetMl) +
            "mL) dépasse maxPhMlPerDay (" + String(safetyLimits.maxPhMlPerDay, 0) + "mL) — plafonné");
          phCappedLogged = true;
        }
        effectiveDailyMl = safetyLimits.maxPhMlPerDay;
      } else {
        phCappedLogged = false;
      }
      // Itération 2 (option B+C) : garde de caractérisation évaluée AVANT le
      // critical « débit non configuré » existant (message sinon trompeur : le
      // vrai problème est l'absence de pesée, pas une config à 0). Le mode
      // Programmée dose désormais à l'amplitude CALIBRÉE en continu (kick au
      // démarrage de session) : le débit effectif est le débit MESURÉ à cette
      // amplitude — plus de modèle linéaire ni de clamp vers le haut (supprimés).
      const int phSchedIdx = pumpIndexFromNumber(mqttCfg.phPump);
      const uint8_t phSchedAmpDuty = getPumpAmplitudeDuty(phSchedIdx);
      const float phSchedAmpFlow = pumpPulseFlow(phSchedIdx);
      const bool phSchedBlocked =
          evaluatePulseGate(phSchedAmpDuty, pumpCalibratedDuty(phSchedIdx),
                            phSchedAmpFlow, MIN_ACTIVE_DUTY) != PulseRefusal::None;
      // Débit effectif : UNE SEULE variable pour le bornage horaire, la durée
      // d'injection et le duty (condition pool-chemistry n°5).
      float effectiveFlowMlPerMin = phSchedBlocked ? 0.0f : phSchedAmpFlow;
      static bool phZeroFlowLogged = false;
      if (phSchedBlocked) {
        // État de config persistant → critical THROTTLÉ (30 min).
        static unsigned long phSchedGateCritMs = 0;
        if (phSchedGateCritMs == 0 || now - phSchedGateCritMs >= kPulseRefusalLogIntervalMs) {
          phSchedGateCritMs = now;
          systemLogger.critical("[Scheduled] Pompe pH non caractérisée (duty calibré=" +
            String(pumpCalibratedDuty(phSchedIdx)) + ", amplitude=" + String(phSchedAmpDuty) +
            ", débit=" + String(phSchedAmpFlow, 1) +
            " mL/min) — dosage programmé BLOQUÉ (pesée au banc requise)");
        }
      } else if (effectiveFlowMlPerMin <= 0.0f) {
        // Défensif : ne devrait plus arriver (la gate garantit un débit > 0).
        if (!phZeroFlowLogged) {
          systemLogger.critical("[Scheduled] Débit pompe pH non configuré (0 mL/min) — dosage bloqué");
          phZeroFlowLogged = true;
        }
      } else {
        phZeroFlowLogged = false;

        // Heure locale (même pattern que tickDailyRollover) : heure invalide →
        // aucune injection (fail-closed, comportement historique), warning UNE fois.
        int nowMin = -1;
        time_t epochNow = time(nullptr);
        static bool phNoTimeLogged = false;
        if (epochNow >= kMinValidEpoch) {
          struct tm timeinfo;
          localtime_r(&epochNow, &timeinfo);
          nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
          phNoTimeLogged = false;
        } else if (!phNoTimeLogged) {
          systemLogger.warning("[Scheduled] Heure locale indisponible — dosage pH programmé suspendu");
          phNoTimeLogged = true;
        }

        if (nowMin >= 0) {
          // Horizon de répartition : minutes restantes de la plage de filtration
          // (bornées à minuit), ou fin de journée hors mode Managed (eau 24/7 en
          // Powered ; horizon plein en External, borné par la garde présence d'eau).
          int horizonMinutes;
          if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
            horizonMinutes = 1440 - nowMin;
          } else {
            horizonMinutes = remainingRangeMinutes(nowMin,
                timeStringToMinutes(filtrationCfg.start.c_str()),
                timeStringToMinutes(filtrationCfg.end.c_str()));
          }

          ScheduledDoseInputs sin;
          sin.nowMin = nowMin;
          sin.horizonMinutes = horizonMinutes;
          sin.windowMinutes = kScheduledWindowMinutes;
          sin.dailyTargetMl = static_cast<float>(mqttCfg.phDailyTargetMl);
          sin.maxDailyMl = safetyLimits.maxPhMlPerDay;
          sin.dailyInjectedMl = safetyLimits.dailyPhInjectedMl;
          sin.effectiveFlowMlPerMin = effectiveFlowMlPerMin;
          sin.usedMs = (uint32_t)phDosingState.usedMs;
          sin.hourlyLimitMs = (uint32_t)phLimitMs;
          sin.minInjectionTimeMs = (uint32_t)pumpProtection.minInjectionTimeMs;
          sin.watchdogActive = (esp_task_wdt_status(NULL) == ESP_OK);
          sin.prevWindowIndex = _phSchedWindowIdx;
          sin.prevStopTargetMl = _phSchedStopTargetMl;

          ScheduledDoseDecision dec = evaluateScheduledDose(sin);

          // Recommandation pool-chemistry : warning edge-triggered quand le
          // volume de fenêtre est tronqué par le budget horaire restant (le
          // recalcul n'a lieu qu'à l'entrée d'une nouvelle fenêtre).
          static bool phBudgetTruncLogged = false;
          if (dec.windowIndex >= 0 && dec.windowIndex != _phSchedWindowIdx &&
              horizonMinutes > 0 && phLimitMs > 0) {
            int nWin = horizonMinutes / kScheduledWindowMinutes;
            if (nWin < 1) nWin = 1;
            float remainingMl = effectiveDailyMl - safetyLimits.dailyPhInjectedMl;
            if (remainingMl < 0.0f) remainingMl = 0.0f;
            float vRaw = remainingMl / static_cast<float>(nWin);
            float budgetMl = (phDosingState.usedMs >= phLimitMs) ? 0.0f
                : (static_cast<float>(phLimitMs - phDosingState.usedMs) / 60000.0f) *
                      effectiveFlowMlPerMin;
            if (vRaw > budgetMl + 0.01f) {
              if (!phBudgetTruncLogged) {
                systemLogger.warning("[Scheduled] Volume de fenêtre pH tronqué par le budget horaire (" +
                  String(vRaw, 1) + "mL → " + String(budgetMl, 1) + "mL) — report aux fenêtres suivantes");
                phBudgetTruncLogged = true;
              }
            } else {
              phBudgetTruncLogged = false;
            }
          }

          // Condition pool-chemistry n°1 : consulter le ring anti-rafale AVANT
          // tout DÉMARRAGE (transition inactive → active). Mêmes fenêtres et
          // seuils que canDose() (ring PARTAGÉ auto + manuel + scheduled).
          bool wantDose = dec.doseNow;
          static bool phSchedBurstLogged = false;
          if (wantDose && !phDosingState.active) {
            int cyclesLastMin = countRecentDosingCycles(0, 60000);
            int cyclesLast15Min = countRecentDosingCycles(0, 900000);
            if (cyclesLastMin >= kMaxDosingCyclesPerMinute ||
                cyclesLast15Min >= kMaxDosingCyclesPer15Min) {
              if (!phSchedBurstLogged) {
                systemLogger.info("[Scheduled] Démarrage pH différé (anti-rafale : " +
                  String(cyclesLastMin) + " cycles/1min, " +
                  String(cyclesLast15Min) + " cycles/15min)");
                phSchedBurstLogged = true;
              }
              wantDose = false;
            } else {
              phSchedBurstLogged = false;
            }
          }

          if (wantDose && !phDosingState.active) {
            phDosingState.lastStartTime = now;
            // Exemption cyclesToday CONSERVÉE (R4, arbitrage pool-chemistry) :
            // le cadencement structurel (1 démarrage / fenêtre 15 min) + le
            // ring anti-rafale bornent déjà les démarrages. On enregistre
            // quand même chaque start dans le ring pour cohérence.
            recordDosingCycleStart(0);
            systemLogger.info("[Scheduled] Démarrage dosage pH : fenêtre n°" + String(dec.windowIndex) +
              ", volume fenêtre=" + String(dec.stopTargetMl - safetyLimits.dailyPhInjectedMl, 1) +
              "mL, injecté=" + String(safetyLimits.dailyPhInjectedMl, 0) + "/" +
              String(effectiveDailyMl, 0) + "mL");
          } else if (!wantDose && phDosingState.active) {
            phDosingState.lastStopTime = now;
            if (safetyLimits.dailyPhInjectedMl >= effectiveDailyMl) {
              systemLogger.info("[Scheduled] Quota journalier pH atteint (" +
                String(safetyLimits.dailyPhInjectedMl, 0) + "/" + String(effectiveDailyMl, 0) +
                "mL) — dosage suspendu jusqu'à demain");
            } else {
              systemLogger.info("[Scheduled] Arrêt dosage pH : fenêtre n°" + String(dec.windowIndex) +
                " terminée (injecté=" + String(safetyLimits.dailyPhInjectedMl, 0) + "/" +
                String(effectiveDailyMl, 0) + "mL) — reprise à la prochaine fenêtre");
            }
          }

          if (wantDose) {
            // Itération 2 : amplitude CALIBRÉE en continu + kick au démarrage
            // de session. La géométrie (pulseStartMs) est enregistrée pour que
            // la comptabilité crédite le kick au débit 255 mesuré (exact).
            // Condition n°5 préservée : effectiveFlowMlPerMin (= débit mesuré à
            // l'amplitude) sert au bornage horaire, à la durée ET à la commande.
            // `!active` couvre la reprise après une suspension hors-branche
            // (drapeau de session périmé) : le kick repart et les compteurs
            // de session sont remis à zéro.
            if (!_pulseSession[0] || !phDosingState.active) {
              _pulseSession[0] = true;
              _sessionOnMs[0] = 0;
              _sessionVolMl[0] = 0.0f;
              _pulseState[0] = {(uint32_t)now, (uint32_t)now, true};
            }
            bool kick = ((uint32_t)now - _pulseState[0].pulseStartMs) < kPulseKickMs;
            uint8_t duty = kick ? MAX_PWM_DUTY : phSchedAmpDuty;
            if (duty > desiredDuty[phSchedIdx]) desiredDuty[phSchedIdx] = duty;
            phActive = true;
            phFlow = effectiveFlowMlPerMin;
          }

          _phSchedWindowIdx = dec.windowIndex;
          _phSchedStopTargetMl = dec.stopTargetMl;
          _phSchedPlannedFlow = dec.plannedFlowMlPerMin;
        }
      }
    }
    // Reset PID pour éviter le windup au retour en mode automatique
    phPID.integral = 0.0f;
    phPID.lastError = 0.0f;
    phPID.lastTime = 0;
  }

  // Contrôle ORP — dispatch sur orpRegulationMode, gardé en profondeur par canDose(1).
  // feature-067 (pool-chemistry #3) : inhibition ORP sur pH BRUT élevé, évaluée
  // AVANT canDose(1) (même patron que le plancher acide côté pH).
  const String& orpMode = mqttCfg.orpRegulationMode;
  if (orpMode == "automatic" && orpLimitOk && orpSafetyOk && !isOrpInhibitedByPhRaw(true) && canDose(1)) {
    // feature-025 : le PID auto consomme la mesure FILTRÉE (médiane + EMA).
    // canDose(1) a déjà garanti isOrpFilterReady() && !isOrpFilterUnstable() → non NaN.
    float orpValue = sensors.getOrpFiltered();
    float effectiveOrp = orpValue;

    // feature-053 : la régulation vise la cible ORP EFFECTIVE (relevée par le
    // Mode Boost si actif ; sinon identique à mqttCfg.orpTarget).
    float orpTargetEff = getEffectiveOrpTarget();

    // Erreur positive = ORP trop bas → injecter du chlore pour monter l'ORP
    float error = orpTargetEff - effectiveOrp;

    // Déterminer si on doit doser (avec protection anti-cycling)
    bool shouldDose = false;
    // bug-dosage-fantome (défaut n°2) : symétrique du pH — la DÉCISION reste
    // ici, la COMPTABILISATION du cycle attend un débit réellement non nul.
    const bool orpWasActiveTick = orpDosingState.active;

    if (orpWasActiveTick) {
      // Déjà en cours : vérifier si on continue. Itération 2 : garde de temps
      // minimum en temps POMPE-ON cumulé de la session.
      shouldDose = shouldContinueDosing(error, pumpProtection.orpStopThreshold, _sessionOnMs[1]);
    } else {
      // Arrêté : vérifier si on démarre
      shouldDose = shouldStartDosing(error, pumpProtection.orpStartThreshold, orpDosingState, now);
    }
    const bool orpStartDecided = !orpWasActiveTick && shouldDose;

    // --- Itération 2 (option B+C) : caractérisation de la pompe ORP affectée ---
    const int orpPumpIdx = pumpIndexFromNumber(mqttCfg.orpPump);
    const uint8_t orpAmpDuty = getPumpAmplitudeDuty(orpPumpIdx);
    const float orpAmpFlow = pumpPulseFlow(orpPumpIdx);  // débit MESURÉ à l'amplitude
    bool orpGateRefused = false;  // pompe non caractérisée / amplitude sous plancher
    // feature-073 : refus du PLAN = entrées invalides uniquement (fail-closed) —
    // la consigne sous le pouls minimal est désormais ACCUMULÉE, plus refusée.
    bool orpPlanRefused = false;

    if (shouldDose) {
      // Garde de caractérisation AVANT tout calcul (fail-closed P4) — voir le
      // commentaire détaillé de la branche pH.
      PulseRefusal gate = evaluatePulseGate(orpAmpDuty, pumpCalibratedDuty(orpPumpIdx),
                                            orpAmpFlow, MIN_ACTIVE_DUTY);
      if (gate != PulseRefusal::None) {
        orpGateRefused = true;
        shouldDose = false;
        static unsigned long orpGateCritMs = 0;
        if (orpGateCritMs == 0 || now - orpGateCritMs >= kPulseRefusalLogIntervalMs) {
          orpGateCritMs = now;
          if (gate == PulseRefusal::AmplitudeBelowMinActive) {
            systemLogger.critical("[Dosage ORP] Amplitude pompe (" + String(orpAmpDuty) +
              "/255) sous le plancher de rotation (" + String(MIN_ACTIVE_DUTY) +
              ") — dosage automatique BLOQUÉ (relever la puissance pompe et recalibrer)");
          } else {
            systemLogger.critical("[Dosage ORP] Pompe non caractérisée (duty calibré=" +
              String(pumpCalibratedDuty(orpPumpIdx)) + ", amplitude=" + String(orpAmpDuty) +
              ", débit=" + String(orpAmpFlow, 1) +
              " mL/min) — dosage automatique BLOQUÉ (pesée au banc requise)");
          }
        }
      }
    }

    float flow = 0.0f;  // demande de débit MOYEN (mL/min), réalisée par le pouls
    if (shouldDose) {
      // feature-025 (B6) : geler l'intégrale si la sortie sature.
      bool orpSaturated = (orpPID.kp * error >= orpAmpFlow);
      // computePID renvoie le débit FINAL borné [0, ampFlow] — plancher minFlow
      // SUPPRIMÉ ; feature-073 : sous le pouls minimal la demande est servie
      // par l'ACCUMULATEUR de planPulseWithCarry (plus de deadband de refus).
      flow = computePID(orpPID, error, now,
                        pumpProtection.orpStartThreshold, orpSaturated,
                        0.0f, orpAmpFlow);
    } else {
      // Reset PID si erreur négative (ORP trop haut, au-dessus de la cible)
      if (error < -pumpProtection.orpStopThreshold) {
        orpPID.integral = 0.0f;
        orpPID.lastError = 0.0f;
        orpPID.lastTime = 0;
      }
    }

    // --- Ordonnanceur d'impulsions (symétrique de la branche pH) ---
    const uint32_t now32 = (uint32_t)now;
    if (flow > 0.0f) {
      bool newPeriod = !_pulseSession[1] ||
                       (now32 - _pulseState[1].periodStartMs) >= kPulsePeriodMs;
      if (newPeriod) {
        // Ouverture / reprise de séance AVANT le plan (feature-073) — voir le
        // commentaire détaillé de la branche pH : séance ouverte aussi en
        // régime d'accumulation, accumulateur remis à zéro à l'ouverture ;
        // orpStartDecided couvre la reprise après suspension hors-branche.
        if (!_pulseSession[1] || orpStartDecided) {
          _pulseSession[1] = true;
          _sessionOnMs[1] = 0;
          _sessionVolMl[1] = 0.0f;
          _pulseState[1] = {now32, now32, true};
          _pulseCarryMl[1] = 0.0f;
          _carryPulseLogged[1] = false;
        }
        // feature-073 : accumulation sous le plancher (ex-seuil C), tir d'une
        // impulsion minimale par le chemin STANDARD dès que le cumul vaut le
        // volume kick-inclus d'un pouls min — voir la branche pH.
        PulseCarryPlan plan = planPulseWithCarry(
            flow, orpAmpFlow, pumpFlow255(orpPumpIdx), kPulsePeriodMs,
            kPulseMinMs, kPulseKickMs, _pulseCarryMl[1]);
        _pulseCarryMl[1] = plan.carryMl;
        if (plan.refused) {
          // Entrées invalides (fail-closed) — ne nourrit pas le streak no-flow.
          orpPlanRefused = true;
          flow = 0.0f;
        } else {
          // pulseMs == 0 = période d'ACCUMULATION : séance maintenue, pompe OFF.
          _pulseMs[1] = plan.pulseMs;
          if (plan.carryFired && !_carryPulseLogged[1]) {
            // A3 : premier tir d'accumulateur de la séance seulement.
            _carryPulseLogged[1] = true;
            systemLogger.info("[Dosage ORP] Dosage par accumulation : impulsion minimale de " +
              String(kPulseMinMs / 1000UL) + " s (" +
              String(plan.carryMl + pulseDeltaVolumeMl(0, kPulseKickMs, 0, kPulseMinMs,
                                                       pumpFlow255(orpPumpIdx), orpAmpFlow), 1) +
              " mL cumulés, résidu " + String(plan.carryMl, 1) + " mL)");
          }
        }
      }
    }

    if (flow > 0.0f && _pulseSession[1]) {
      PulseTick tk = tickPulse(_pulseState[1], now32, kPulsePeriodMs,
                               _pulseMs[1], kPulseKickMs);
      _pulseState[1] = tk.next;
      if (tk.pumpOn) {
        uint8_t duty = tk.kickPhase ? MAX_PWM_DUTY : orpAmpDuty;
        if (duty > desiredDuty[orpPumpIdx]) desiredDuty[orpPumpIdx] = duty;
      }
      orpActive = true;  // session en cours (pompe ON pendant les pouls seulement)
      orpFlow = flow;
    }

    // --- Fin de session (symétrique de la branche pH) ---
    if (orpWasActiveTick && !orpActive) {
      orpDosingState.lastStopTime = now;
      // feature-025 : pause mélange armée à la FIN DE SESSION, jamais entre pouls.
      notifyOrpDose((uint32_t)now);
      unsigned long runTime = (now - orpDosingState.lastStartTime) / 1000;
      systemLogger.info("Arrêt dosage ORP (auto): session=" + String(runTime) + "s" +
        " pompe-ON=" + String(_sessionOnMs[1] / 1000UL) + "s" +
        " vol≈" + String(_sessionVolMl[1], 1) + "mL" +
        " total jour=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(safetyLimits.maxChlorineMlPerDay, 0) + "mL");
    }

    // --- Comptabilisation différée du démarrage de cycle (bug-dosage-fantome,
    // défaut n°2) --- Voir le commentaire détaillé de la branche pH : même
    // invariant (cycle compté ⇔ cycle latché actif), aucun dt synthétisé.
    // Symétrique du WARN de la branche pH — y compris la garde de PERSISTANCE :
    // « décision sans débit » est normal au premier tick, alerter dessus
    // inonderait le journal. Voir le commentaire détaillé côté pH.
    // feature-073 : les refus de plan (entrées invalides) / de gate ne
    // nourrissent PAS le streak, et le régime sous-plancher laisse orpActive à
    // true (accumulation) — voir le commentaire de la branche pH.
    static uint16_t orpNoFlowStreak = 0;
    static unsigned long orpNoFlowWarnMs = 0;
    if (orpStartDecided && !orpActive && !orpPlanRefused && !orpGateRefused) {
      if (orpNoFlowStreak < 0xFFFF) ++orpNoFlowStreak;
      if (orpNoFlowStreak >= kDosingNoFlowWarnStreak &&
          (orpNoFlowWarnMs == 0 || now - orpNoFlowWarnMs >= kDosingNoFlowWarnIntervalMs)) {
        orpNoFlowWarnMs = now;
        systemLogger.warning(
            "[Dosage ORP] décision de doser SANS débit commandé depuis " +
            String(orpNoFlowStreak) + " tours — régulation sans effet (boucle trop "
            "lente, ou consigne sous le plancher de duty). Erreur=" + String(error, 1) + " mV");
      }
    } else if (orpActive) {
      orpNoFlowStreak = 0;
    }

    if (evaluateCycleStartAccounting(orpWasActiveTick, orpStartDecided, orpActive)
            .countCycleStart) {
      orpDosingState.lastStartTime = now;
      orpDosingState.cyclesToday++;
      // Anti-rafale Pass 3.5 : timestamp de start pour les fenêtres glissantes.
      recordDosingCycleStart(1);
      systemLogger.info("Démarrage dosage ORP (auto): ORP=" + String(sensors.getOrpFiltered(), 0) + "mV" +
        " cible=" + String(orpTargetEff, 0) + "mV" +
        " erreur=" + String(error, 0) + "mV" +
        " (cycle " + String(orpDosingState.cyclesToday) + "/" + String(pumpProtection.maxCyclesPerDay) + ")");
    }
  } else if (orpMode == "scheduled" && orpLimitOk && orpSafetyOk &&
             mqttCfg.orpDailyTargetMl > 0 && !isOrpInhibitedByPhRaw(false)) {
    // feature-067 (pool-chemistry #3) : l'inhibition sur pH brut élevé s'applique
    // AUSSI au mode Programmée — injecter du chlore à pH > 7,6 est inefficace et
    // conduit au surdosage (filet de sécurité, pas régulation sur la mesure).
    // Mode Programmée (feature-011) : répartition du volume quotidien par
    // fenêtres de kScheduledWindowMinutes (15 min) sur l'horizon de filtration
    // restant, borné à minuit. Intentionnellement aveugle à la valeur mesurée
    // de l'ORP : l'utilisateur a choisi un volume quotidien fixe de chlore
    // indépendamment de la mesure ; un capteur déréglé ne doit pas bloquer
    // l'injection. Seules les gardes volumétriques (orpLimitOk, orpSafetyOk,
    // maxChlorineMlPerDay) et structurelles (débit configuré, heure valide,
    // anti-rafale) s'appliquent. orpTarget n'est pas utilisé dans cette branche
    // et ne doit pas l'être. COQUILLE symétrique de la branche pH : collecte
    // des entrées + application ; décision déléguée à evaluateScheduledDose().
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
      // Log "plafonné" conservé en coquille — le plafonnement lui-même est
      // AUSSI appliqué dans evaluateScheduledDose (via maxDailyMl) ;
      // effectiveDailyMl ne sert plus ici qu'aux logs.
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
      // Itération 2 (option B+C) : garde de caractérisation AVANT le critical
      // « débit non configuré » (voir la branche scheduled pH). Amplitude
      // CALIBRÉE en continu + kick au démarrage, débit MESURÉ à l'amplitude.
      const int orpSchedIdx = pumpIndexFromNumber(mqttCfg.orpPump);
      const uint8_t orpSchedAmpDuty = getPumpAmplitudeDuty(orpSchedIdx);
      const float orpSchedAmpFlow = pumpPulseFlow(orpSchedIdx);
      const bool orpSchedBlocked =
          evaluatePulseGate(orpSchedAmpDuty, pumpCalibratedDuty(orpSchedIdx),
                            orpSchedAmpFlow, MIN_ACTIVE_DUTY) != PulseRefusal::None;
      // Débit effectif : UNE SEULE variable pour le bornage horaire, la durée
      // d'injection et le duty (condition pool-chemistry n°5).
      float effectiveFlowMlPerMin = orpSchedBlocked ? 0.0f : orpSchedAmpFlow;
      static bool orpZeroFlowLogged = false;
      if (orpSchedBlocked) {
        static unsigned long orpSchedGateCritMs = 0;
        if (orpSchedGateCritMs == 0 || now - orpSchedGateCritMs >= kPulseRefusalLogIntervalMs) {
          orpSchedGateCritMs = now;
          systemLogger.critical("[Scheduled ORP] Pompe ORP non caractérisée (duty calibré=" +
            String(pumpCalibratedDuty(orpSchedIdx)) + ", amplitude=" + String(orpSchedAmpDuty) +
            ", débit=" + String(orpSchedAmpFlow, 1) +
            " mL/min) — dosage programmé BLOQUÉ (pesée au banc requise)");
        }
      } else if (effectiveFlowMlPerMin <= 0.0f) {
        // Défensif : ne devrait plus arriver (la gate garantit un débit > 0).
        if (!orpZeroFlowLogged) {
          systemLogger.critical("[Scheduled ORP] Débit pompe ORP non configuré (0 mL/min) — dosage bloqué");
          orpZeroFlowLogged = true;
        }
      } else {
        orpZeroFlowLogged = false;

        // Heure locale (même pattern que tickDailyRollover) : heure invalide →
        // aucune injection (fail-closed, comportement historique), warning UNE fois.
        int nowMin = -1;
        time_t epochNow = time(nullptr);
        static bool orpNoTimeLogged = false;
        if (epochNow >= kMinValidEpoch) {
          struct tm timeinfo;
          localtime_r(&epochNow, &timeinfo);
          nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
          orpNoTimeLogged = false;
        } else if (!orpNoTimeLogged) {
          systemLogger.warning("[Scheduled ORP] Heure locale indisponible — dosage ORP programmé suspendu");
          orpNoTimeLogged = true;
        }

        if (nowMin >= 0) {
          // Horizon de répartition : minutes restantes de la plage de filtration
          // (bornées à minuit), ou fin de journée hors mode Managed (eau 24/7 en
          // Powered ; horizon plein en External, borné par la garde présence d'eau).
          int horizonMinutes;
          if (mqttCfg.installMode != InstallMode::ManagedFiltration) {
            horizonMinutes = 1440 - nowMin;
          } else {
            horizonMinutes = remainingRangeMinutes(nowMin,
                timeStringToMinutes(filtrationCfg.start.c_str()),
                timeStringToMinutes(filtrationCfg.end.c_str()));
          }

          ScheduledDoseInputs sin;
          sin.nowMin = nowMin;
          sin.horizonMinutes = horizonMinutes;
          sin.windowMinutes = kScheduledWindowMinutes;
          sin.dailyTargetMl = static_cast<float>(mqttCfg.orpDailyTargetMl);
          sin.maxDailyMl = safetyLimits.maxChlorineMlPerDay;
          sin.dailyInjectedMl = safetyLimits.dailyOrpInjectedMl;
          sin.effectiveFlowMlPerMin = effectiveFlowMlPerMin;
          sin.usedMs = (uint32_t)orpDosingState.usedMs;
          sin.hourlyLimitMs = (uint32_t)orpLimitMs;
          sin.minInjectionTimeMs = (uint32_t)pumpProtection.minInjectionTimeMs;
          sin.watchdogActive = (esp_task_wdt_status(NULL) == ESP_OK);
          sin.prevWindowIndex = _orpSchedWindowIdx;
          sin.prevStopTargetMl = _orpSchedStopTargetMl;

          ScheduledDoseDecision dec = evaluateScheduledDose(sin);

          // Recommandation pool-chemistry : warning edge-triggered quand le
          // volume de fenêtre est tronqué par le budget horaire restant (le
          // recalcul n'a lieu qu'à l'entrée d'une nouvelle fenêtre).
          static bool orpBudgetTruncLogged = false;
          if (dec.windowIndex >= 0 && dec.windowIndex != _orpSchedWindowIdx &&
              horizonMinutes > 0 && orpLimitMs > 0) {
            int nWin = horizonMinutes / kScheduledWindowMinutes;
            if (nWin < 1) nWin = 1;
            float remainingMl = effectiveDailyMl - safetyLimits.dailyOrpInjectedMl;
            if (remainingMl < 0.0f) remainingMl = 0.0f;
            float vRaw = remainingMl / static_cast<float>(nWin);
            float budgetMl = (orpDosingState.usedMs >= orpLimitMs) ? 0.0f
                : (static_cast<float>(orpLimitMs - orpDosingState.usedMs) / 60000.0f) *
                      effectiveFlowMlPerMin;
            if (vRaw > budgetMl + 0.01f) {
              if (!orpBudgetTruncLogged) {
                systemLogger.warning("[Scheduled ORP] Volume de fenêtre ORP tronqué par le budget horaire (" +
                  String(vRaw, 1) + "mL → " + String(budgetMl, 1) + "mL) — report aux fenêtres suivantes");
                orpBudgetTruncLogged = true;
              }
            } else {
              orpBudgetTruncLogged = false;
            }
          }

          // Condition pool-chemistry n°1 : consulter le ring anti-rafale AVANT
          // tout DÉMARRAGE (transition inactive → active). Mêmes fenêtres et
          // seuils que canDose() (ring PARTAGÉ auto + manuel + scheduled).
          bool wantDose = dec.doseNow;
          static bool orpSchedBurstLogged = false;
          if (wantDose && !orpDosingState.active) {
            int cyclesLastMin = countRecentDosingCycles(1, 60000);
            int cyclesLast15Min = countRecentDosingCycles(1, 900000);
            if (cyclesLastMin >= kMaxDosingCyclesPerMinute ||
                cyclesLast15Min >= kMaxDosingCyclesPer15Min) {
              if (!orpSchedBurstLogged) {
                systemLogger.info("[Scheduled ORP] Démarrage ORP différé (anti-rafale : " +
                  String(cyclesLastMin) + " cycles/1min, " +
                  String(cyclesLast15Min) + " cycles/15min)");
                orpSchedBurstLogged = true;
              }
              wantDose = false;
            } else {
              orpSchedBurstLogged = false;
            }
          }

          if (wantDose && !orpDosingState.active) {
            orpDosingState.lastStartTime = now;
            // Exemption cyclesToday CONSERVÉE (R4, arbitrage pool-chemistry) :
            // le cadencement structurel (1 démarrage / fenêtre 15 min) + le
            // ring anti-rafale bornent déjà les démarrages. On enregistre
            // quand même chaque start dans le ring pour cohérence.
            recordDosingCycleStart(1);
            systemLogger.info("[Scheduled ORP] Démarrage dosage ORP : fenêtre n°" + String(dec.windowIndex) +
              ", volume fenêtre=" + String(dec.stopTargetMl - safetyLimits.dailyOrpInjectedMl, 1) +
              "mL, injecté=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" +
              String(effectiveDailyMl, 0) + "mL");
          } else if (!wantDose && orpDosingState.active) {
            orpDosingState.lastStopTime = now;
            if (safetyLimits.dailyOrpInjectedMl >= effectiveDailyMl) {
              systemLogger.info("[Scheduled ORP] Quota journalier ORP atteint (" +
                String(safetyLimits.dailyOrpInjectedMl, 0) + "/" + String(effectiveDailyMl, 0) +
                "mL) — dosage suspendu jusqu'à demain");
            } else {
              systemLogger.info("[Scheduled ORP] Arrêt dosage ORP : fenêtre n°" + String(dec.windowIndex) +
                " terminée (injecté=" + String(safetyLimits.dailyOrpInjectedMl, 0) + "/" +
                String(effectiveDailyMl, 0) + "mL) — reprise à la prochaine fenêtre");
            }
          }

          if (wantDose) {
            // Itération 2 : amplitude CALIBRÉE en continu + kick au démarrage
            // de session (voir la branche scheduled pH). Condition n°5
            // préservée : effectiveFlowMlPerMin = débit mesuré à l'amplitude.
            // Cf. branche scheduled pH : `!active` couvre la reprise après
            // suspension hors-branche (kick + compteurs de session à zéro).
            if (!_pulseSession[1] || !orpDosingState.active) {
              _pulseSession[1] = true;
              _sessionOnMs[1] = 0;
              _sessionVolMl[1] = 0.0f;
              _pulseState[1] = {(uint32_t)now, (uint32_t)now, true};
            }
            bool kick = ((uint32_t)now - _pulseState[1].pulseStartMs) < kPulseKickMs;
            uint8_t duty = kick ? MAX_PWM_DUTY : orpSchedAmpDuty;
            if (duty > desiredDuty[orpSchedIdx]) desiredDuty[orpSchedIdx] = duty;
            orpActive = true;
            orpFlow = effectiveFlowMlPerMin;
          }

          _orpSchedWindowIdx = dec.windowIndex;
          _orpSchedStopTargetMl = dec.stopTargetMl;
          _orpSchedPlannedFlow = dec.plannedFlowMlPerMin;
        }
      }
    }
    // Reset PID pour éviter le windup au retour en mode automatique
    orpPID.integral = 0.0f;
    orpPID.lastError = 0.0f;
    orpPID.lastTime = 0;
  }

  // Itération 2 (condition B1 du gate) : le clamp PWM final sur pumpNMaxDutyPct
  // est SUPPRIMÉ — pumpNMaxDutyPct définit désormais directement l'AMPLITUDE du
  // plateau (le plafond est structurel), et le kick de décollage (255, 400 ms)
  // doit pouvoir passer : l'écrêter reproduirait le non-démarrage à froid.
  // desiredDuty ne prend plus que trois valeurs : 0, amplitude calibrée, 255.

  // Appliquer les valeurs de duty (sauf pour les pompes en mode manuel)
  for (int i = 0; i < 2; ++i) {
    if (!manualMode[i]) {
      applyPumpDuty(i, desiredDuty[i]);
    }
  }

  // Fin de session hors branche (canDose refusé en cours de session, changement
  // de mode, limite atteinte…) : libérer l'état du train d'impulsions pour que
  // la prochaine session reparte proprement (kick + compteurs de session à 0).
  // feature-073 : l'accumulateur sous-plancher est PERDU à toute fermeture de
  // séance (demande nulle, refus de gate/canDose, limite, suspension — voulu :
  // une demande disparue ne laisse aucune dette), et le flag de log A3 réarmé.
  if (!phActive) {
    _pulseSession[0] = false;
    _pulseCarryMl[0] = 0.0f;
    _carryPulseLogged[0] = false;
  }
  if (!orpActive) {
    _pulseSession[1] = false;
    _pulseCarryMl[1] = 0.0f;
    _carryPulseLogged[1] = false;
  }

  // Comptabilité produit (itération 2, P4) : volume par delta de tick, calculé
  // par pulseDeltaVolumeMl (kick au débit 255 MESURÉ + plateau au débit CALIBRÉ,
  // exact et indépendant du jitter de loop) pour l'auto/scheduled ; MAJORANT
  // flow255 pour le manuel (?duty=N), sauf duty == calibratedDuty → débit
  // mesuré. Jamais zéro sur une pompe alimentée.
  int phIdx  = pumpIndexFromNumber(mqttCfg.phPump);
  int orpIdx = pumpIndexFromNumber(mqttCfg.orpPump);
  bool phManualActive  = manualMode[phIdx]  && pumpDuty[phIdx]  > 0;
  bool orpManualActive = manualMode[orpIdx] && pumpDuty[orpIdx] > 0;
  accountPumpVolume(0, phIdx, now, phManualActive);
  accountPumpVolume(1, orpIdx, now, orpManualActive);

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
  // Auto-guérison v2.9.1 (condition pool-chemistry C2) : setManualPump est le point
  // de passage UNIQUE de TOUTE activation manuelle directe (routes web pumpN/on,
  // inject/start). En mode maintenance, cette voie pilote
  // sinon ledcWrite() immédiatement depuis la tâche web et énergiserait la pompe
  // jusqu'au prochain update(). On la ferme AVANT tout accès manualMode/applyPumpDuty.
  // Log throttlé (une fois par boot ; le mode maintenance ne se lève qu'au reboot).
  if (maintenanceInhibit) {
    static bool refusalLogged = false;
    if (!refusalLogged) {
      systemLogger.warning("Commande manuelle pompe ignorée : mode maintenance actif (dosage suspendu)");
      refusalLogged = true;
    }
    return;
  }

  if (pumpIndex < 0 || pumpIndex >= 2) {
    systemLogger.error("Index de pompe invalide: " + String(pumpIndex));
    return;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Prescription P1 (pool-chemistry, feature-063) : pompe déclarée inutilisable
  // au boot (échec attache LEDC) → refus de toute commande manuelle. Défense en
  // profondeur (applyPumpDuty refuse déjà), avec log throttlé une fois par boot
  // (l'état pumpUsable ne se lève qu'au reboot).
  if (!pumpUsable[pumpIndex]) {
    static bool unusableRefusalLogged = false;
    if (!unusableRefusalLogged) {
      systemLogger.warning("Commande manuelle pompe " + String(pumpIndex + 1) +
                           " ignorée : pompe inutilisable (échec attache LEDC au boot)");
      unusableRefusalLogged = true;
    }
    return;
  }
#endif

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
