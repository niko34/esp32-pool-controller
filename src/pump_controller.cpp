#include "pump_controller.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "dosing_logic.h"
#include "schedule_logic.h"
#include "mqtt_manager.h"  // feature-067 : alertes MQTT des gardes pH
#include "measure_validity.h"  // v2.20.3 (C6) : distinguer purge en cours et capteur muet
#include "ph_guard_logic.h"    // v2.25.0 : gardes pH + diagnostic sonde (logique pure)
#include "ph_offset.h"         // v2.25.0 : applyPhOffset() — pH corrigé des gardes
#include "ph_actuator_limit_logic.h"  // feature-086 : phActuatorUndersized() (pure)
#include <esp_task_wdt.h>
#include <string.h>  // strlcpy / strnlen / memcpy — tampon de cause de refus (R2)
#include <time.h>

PumpControllerClass PumpController;

// pool-chemistry (feature-036) : verrou de complétude de la table énum→String FR
// du switch de canDose().
//
// v2.25.1 — le verrou porte désormais sur CHAQUE valeur, plus seulement sur la
// dernière : depuis que les codes sont SÉRIALISÉS vers /data et le WebSocket
// (`ph/orpDoseBlockedCode`), une insertion au MILIEU de l'énum décalerait la
// table de traduction du client et lui ferait afficher une AUTRE cause — c'est
// exactement la classe de défaut corrigée par cette version. Un tel décalage
// compile sans bruit ; seuls ces static_assert l'attrapent.
// Règle : ne jamais renuméroter, ne jamais insérer au milieu, toujours ajouter
// à la fin (et ajouter ici l'assertion correspondante + le case du switch).
static_assert(static_cast<int>(DoseRefusal::None) == 0, "DoseRefusal::None renuméroté");
static_assert(static_cast<int>(DoseRefusal::WatchdogInactive) == 1, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FiltrationOff) == 2, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::ReadingNaN) == 3, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FilterNotReady) == 4, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FilterUnstable) == 5, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::CalibrationInsufficient) == 6, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::StabilizationActive) == 7, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::MixingActive) == 8, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::ModeNotAutomatic) == 9, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::DailyLimit) == 10, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::HourlyLimit) == 11, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::CyclesPerDay) == 12, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::BurstPerMinute) == 13, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::BurstPer15Min) == 14, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::ReadingNaNWarmup) == 15, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::ReadingNaNFrozen) == 16, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::ReadingNaNStale) == 17, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::ReadingNaNGateClosed) == 18, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FilterWarmup) == 19, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FilterFrozen) == 20, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FilterStale) == 21, "DoseRefusal renuméroté");
static_assert(static_cast<int>(DoseRefusal::FilterGateClosed) == 22, "DoseRefusal renuméroté");
// La sentinelle « cause posée par une garde » ne doit jamais collisionner avec
// une valeur de l'énum (elle serait traduite en cause de canDose côté client).
static_assert(static_cast<int>(DoseRefusal::FilterGateClosed) < kDoseBlockedCodeGuard,
              "kDoseBlockedCodeGuard collisionne avec DoseRefusal : relever la sentinelle");

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
// feature-025 (pool-chemistry) : régulation P temporisée, Kd = 0 IMPÉRATIF sur les
// deux chimies — avec une mesure filtrée (médiane + EMA) et une pause mélange, un
// terme dérivé n'amplifierait que le bruit résiduel.
//
// v2.26.0 (gate pool-chemistry, GO sous conditions) : Ki LEVÉ sur le SEUL pH.
//   - pH  : Kp = kPhKp*, Ki = kPhKi* = Kp/Ti, integralMax = kPhIntegralMax*
//           (dimensionné PAR L'AUTORITÉ U_I,max = 6 mL/min — cf. constants.h).
//           integralMax VARIE avec la vitesse, comme Kp : l'autorité est
//           constante, seule la vitesse de charge de l'intégrale change.
//   - ORP : Ki = 0 INCHANGÉ (hors périmètre — l'ORP dépend du pH, de la T° et du
//           stabilisant ; son droop n'est pas le défaut diagnostiqué).
//
// APPELÉE DEPUIS LE CONTEXTE WEB ASYNC (web_routes_config.cpp au changement de
// `regulation_speed`), pas seulement depuis begin(). Deux conséquences traitées ici :
//   (a) un changement de vitesse change Ki d'un facteur allant jusqu'à 2,25 — servir
//       l'intégrale ACQUISE avec le nouveau Ki produirait un terme jusqu'à 13,5
//       mL/min, soit plus du double de l'autorité posée. L'intégrale est donc
//       REMISE À ZÉRO en même temps que les coefficients.
//       Coût assumé : la charge est ré-identifiée en Ti (4 à 8 h). Changer de
//       vitesse de régulation est justement demander une autre dynamique.
//
//       ⚠ CORRECTION DE REVUE (v2.26.0, bloquant n°1) — « purger AVANT d'écrire
//       Ki » NE SUFFIT PAS, contrairement à ce que disait la version précédente de
//       ce commentaire. `phIntegralTick()` LIT `st.integral`, calcule, puis
//       RÉÉCRIT `st.integral` : si la tâche web exécute la purge ENTIÈRE entre ces
//       deux instants, loopTask ressuscite l'intégrale d'avant (jusqu'à 16 200
//       pH·s) et la sert avec le NOUVEAU Ki — 13,5 mL/min, soit 2,25× l'autorité
//       posée. Aggravant : cette demande dépasse le débit soutenable, donc
//       l'accumulation est GELÉE, le delta positif n'est pas appliqué et le clamp
//       sur le nouveau `integralMax` (7 200) n'est JAMAIS exécuté — la valeur hors
//       autorité persiste jusqu'à décharge ou reset sec.
//       Le patron du projet est celui de resetPhPauseGuard() : la tâche web POSE
//       un drapeau atomique, loopTask RÉSOUT. Aucune écriture de `phPID` ni de
//       `_phIntegral` depuis un contexte async — cf. applyRegulationSpeedNow().
//   (b) le client envoie `regulation_speed` à CHAQUE enregistrement des réglages,
//       y compris inchangé. Purger l'intégrale à chaque appel reproduirait le
//       défaut C2 (six heures d'accumulation effacées par un clic sans rapport) :
//       on ne purge donc QUE si les coefficients changent RÉELLEMENT.

// Traduction de la vitesse configurée en CODE POD (0 = slow, 1 = normal,
// 2 = fast). La `String` mqttCfg.regulationSpeed n'est lue QUE dans le contexte
// qui l'écrit (la tâche web, sous configMutex) : loopTask ne consomme ensuite que
// ce code 8 bits, jamais la String — même raison que le tampon de cause de refus
// (revue v2.25.0), une String réassignée par une autre tâche se lit comme du tas
// potentiellement libéré.
static uint8_t regulationSpeedCode(const String& speed) {
  if (speed == "slow") return 0;
  if (speed == "fast") return 2;
  return 1;  // "normal" (défaut)
}

void PumpControllerClass::applyRegulationSpeed() {
  // Contexte QUELCONQUE (web async compris) : on ne fait que photographier la
  // vitesse demandée et poser la demande. L'application effective a lieu dans
  // update(), sur loopTask.
  _regulationSpeedCode.store(regulationSpeedCode(mqttCfg.regulationSpeed));
  _phTuningChangeRequested.store(true);
}

// APPLICATION EFFECTIVE — loopTask UNIQUEMENT (begin(), ou update() après
// consommation de `_phTuningChangeRequested`). Seul écrivain de phPID/_phIntegral
// sur ce chemin : plus aucun entrelacement possible avec phIntegralTick().
void PumpControllerClass::applyRegulationSpeedNow() {
  const uint8_t code = _regulationSpeedCode.load();
  const char* speedName;
  float newPhKp, newPhKi, newPhIntegralMax, newOrpKp;
  if (code == 0) {
    speedName = "slow";
    newPhKp = kPhKpSlow;   newPhKi = kPhKiSlow;   newPhIntegralMax = kPhIntegralMaxSlow;
    newOrpKp = 0.15f;
  } else if (code == 2) {
    speedName = "fast";
    newPhKp = kPhKpFast;   newPhKi = kPhKiFast;   newPhIntegralMax = kPhIntegralMaxFast;
    newOrpKp = 0.5f;
  } else {
    // "normal" (défaut)
    speedName = "normal";
    newPhKp = kPhKpNormal; newPhKi = kPhKiNormal; newPhIntegralMax = kPhIntegralMaxNormal;
    newOrpKp = 0.3f;
  }

  // (a) + (b) : purge SEULEMENT sur changement réel, et AVANT les coefficients.
  const bool phTuningChanged = (newPhKp != phPID.kp) || (newPhKi != phPID.ki) ||
                               (newPhIntegralMax != phPID.integralMax);
  if (phTuningChanged && phPID.integral != 0.0f) {
    char msg[112];
    snprintf(msg, sizeof(msg),
             "Intégrale pH remise à zéro (changement de vitesse de régulation) : "
             "%.0f pH·s abandonnés", phPID.integral);
    systemLogger.info(msg);
  }
  if (phTuningChanged) {
    phPID.integral = 0.0f;
    phPID.lastError = 0.0f;
    phPID.lastTime = 0;  // dt reconstruit au prochain tick (pas de dt synthétisé)
    // Le module pur porte l'intégrale, l'épisode d'oubli, la base de temps et
    // l'épisode d'alerte : un seul appel les remet tous dans l'état de boot.
    phIntegralReset(_phIntegral);
    // feature-084 (R2) : une restauration NVS encore EN ATTENTE de confirmation
    // a été validée contre l'ANCIEN contexte (ancien ki, ancien plafond). Elle
    // devient caduque au même titre que l'intégrale elle-même — le contrôle de
    // contexte n'a lieu qu'au chargement, il faut donc l'invalider ici.
    phPersistConfirmAbandon(_phIntegralConfirm);
  }

  phPID.kp = newPhKp;
  phPID.integralMax = newPhIntegralMax;
  phPID.ki = newPhKi;
  phPID.kd = 0.0f;

  orpPID.kp = newOrpKp;
  // ORP : Ki/Kd toujours forcés à 0 (P temporisée pure — feature-025 inchangée).
  orpPID.ki = 0.0f;
  orpPID.kd = 0.0f;

  char line[176];
  snprintf(line, sizeof(line),
           "PID régulation: vitesse=%s Kp_pH=%.2f Ki_pH=%.6f (Ti=%.1fh, "
           "intMax=%.0f pH·s, autorité=%.1f mL/min) Kp_ORP=%.2f Ki_ORP=0 Kd=0",
           speedName, phPID.kp, phPID.ki, phPID.kp / phPID.ki / 3600.0f,
           phPID.integralMax, kPhIntegralAuthorityMlPerMin, orpPID.kp);
  systemLogger.info(line);
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
  // begin() s'exécute sur loopTask, AVANT le démarrage du serveur web : on
  // photographie la vitesse configurée et on applique IMMÉDIATEMENT (sans quoi le
  // PID resterait aux défauts de structure — Ki=0, intMax=50 — jusqu'au premier
  // update()). La demande différée est consommée au passage.
  applyRegulationSpeed();
  _phTuningChangeRequested.store(false);
  applyRegulationSpeedNow();
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

// =============================================================================
// v2.26.0 — FERMETURE de session de dosage pH (point de passage UNIQUE)
// =============================================================================
// Cf. l'en-tête de déclaration (pump_controller.h) pour le POURQUOI. Ici :
// idempotence (no-op si aucune session ouverte), armement de la pause mélange et
// journalisation. AUCUNE décision de dosage — l'appelant a déjà tranché.
void PumpControllerClass::closePhDosingSession(unsigned long now, const char* reason) {
  if (!phDosingState.active) return;  // no-op : aucune session à refermer

  phDosingState.active = false;
  phDosingState.lastStopTime = now;
  _phSessionFromAuto = false;  // plus de session automatique ouverte
  // feature-025 : la pause mélange s'arme à la FIN DE SESSION (homogénéisation
  // post-dose) — jamais entre deux pouls d'une même session.
  notifyPhDose((uint32_t)now);

  const unsigned long runTimeS =
      (_phSessionStartMs != 0) ? ((uint32_t)now - _phSessionStartMs) / 1000UL
                                : (now - phDosingState.lastStartTime) / 1000UL;
  systemLogger.info("Arrêt dosage pH (auto): session=" + String(runTimeS) + "s" +
    " pompe-ON=" + String(_sessionOnMs[0] / 1000UL) + "s" +
    " vol≈" + String(_sessionVolMl[0], 1) + "mL" +
    " total jour=" + String(safetyLimits.dailyPhInjectedMl, 0) + "/" +
    String(safetyLimits.maxPhMlPerDay, 0) + "mL" +
    " motif=" + reason);
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

// =============================================================================
// R2 (revue v2.25.0) — tampon FIXE de la cause de refus
// =============================================================================
// Tronque `dst` (déjà écrit, NUL-terminé) au dernier caractère UTF-8 COMPLET.
// Les messages sont en français : couper à l'octet laisserait une séquence
// multi-octets incomplète, donc un JSON non décodable par le client — le
// symptôme exact déjà rencontré sur les charges MQTT (cf. publishAlert).
// Ne fait rien si la chaîne se termine sur un caractère complet.
static void clampReasonUtf8(char* dst) {
  size_t len = strlen(dst);
  if (len == 0) return;
  // Reculer jusqu'au premier octet du DERNIER caractère (les octets de
  // continuation valent 10xxxxxx).
  size_t start = len - 1;
  while (start > 0 && (static_cast<unsigned char>(dst[start]) & 0xC0) == 0x80) start--;
  const unsigned char lead = static_cast<unsigned char>(dst[start]);
  size_t expected;
  if ((lead & 0x80) == 0x00)      expected = 1;   // ASCII
  else if ((lead & 0xE0) == 0xC0) expected = 2;
  else if ((lead & 0xF0) == 0xE0) expected = 3;
  else if ((lead & 0xF8) == 0xF0) expected = 4;
  else { dst[start] = '\0'; return; }  // octet de tête invalide (lecture déchirée)
  if (start + expected > len) dst[start] = '\0';  // séquence tronquée : on la retire
}

// Écriture BORNÉE du tampon de cause (seul point d'écriture). strlcpy garantit la
// terminaison ; clampReasonUtf8 rattrape une éventuelle coupe multi-octets.
void PumpControllerClass::storeRefusalCause(int pumpIndex, const char* cause) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  if (cause == nullptr) { _lastRefusalCause[pumpIndex][0] = '\0'; return; }
  strlcpy(_lastRefusalCause[pumpIndex], cause, kDoseBlockedReasonMaxLen);
  clampReasonUtf8(_lastRefusalCause[pumpIndex]);
}

// Copie bornée vers le tampon de l'APPELANT (contexte async_tcp autorisé).
// La lecture de la source est plafonnée à sa capacité (strnlen) : une lecture
// déchirée qui n'aurait pas encore vu l'octet NUL ne peut pas sortir du tampon.
// Reste possible sur une lecture déchirée : une jonction ancien/nouveau message
// au MILIEU de la chaîne. Sans conséquence — le décodeur du client remplace
// l'octet douteux (U+FFFD) sur un affichage qui sera corrigé au rafraîchissement
// suivant ; aucune mémoire libérée n'est lue, c'est tout l'objet du tampon fixe.
static void copyReasonBounded(const char* src, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) return;
  const size_t srcLen = strnlen(src, kDoseBlockedReasonMaxLen);
  size_t n = (srcLen < outSize - 1) ? srcLen : outSize - 1;
  memcpy(out, src, n);
  out[n] = '\0';
  clampReasonUtf8(out);
}

void PumpControllerClass::copyPhDoseBlockedReason(char* out, size_t outSize) const {
  copyReasonBounded(_lastRefusalCause[0], out, outSize);
}

void PumpControllerClass::copyOrpDoseBlockedReason(char* out, size_t outSize) const {
  copyReasonBounded(_lastRefusalCause[1], out, outSize);
}

// Logge la cause de refus uniquement à la transition (1 seule entrée par changement
// de cause). Évite le spam quand canDose() est appelé chaque cycle de update().
void PumpControllerClass::logRefusalOnce(int pumpIndex, const String& cause,
                                         DoseRefusal code) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  const uint8_t newCode = static_cast<uint8_t>(code);
  // Code posé AVANT le texte : un lecteur async qui verrait le nouveau texte
  // avec l'ancien code afficherait une cause périmée ; dans l'ordre inverse il
  // voit au pire le nouveau code avec l'ancien texte, et le code fait foi.
  _lastRefusalCode[pumpIndex] = newCode;

  // v2.25.1 — PANNES de mesure : le filtre n'est pas « en train de chauffer », la
  // sonde ne répond plus (figée, ou plus aucune mesure acceptée depuis maxAgeMs).
  // La régulation est à l'arrêt et rien ne le signale hors de l'interface web.
  const bool sensorFault = (code == DoseRefusal::FilterFrozen ||
                            code == DoseRefusal::FilterStale ||
                            code == DoseRefusal::ReadingNaNFrozen ||
                            code == DoseRefusal::ReadingNaNStale);
  const uint32_t nowMs = (uint32_t)millis();
  // ÉPISODE = période de panne CONTINUE, sans considération de la NATURE de la
  // panne. La fenêtre n'est donc redatée que sur la transition non-panne → panne
  // (revue : elle l'était sur tout changement de code, y compris panne → panne).
  // Un EZO agonisant alterne FilterFrozen ↔ FilterStale par phases plus courtes
  // que la temporisation : la date de début était repoussée à chaque bascule et le
  // droit d'alerter réarmé → le `warning` et l'alerte MQTT ne partaient JAMAIS
  // alors que la régulation était à l'arrêt. Le correctif anti-panne-silencieuse
  // introduisait donc une panne silencieuse.
  const bool wasFault = _sensorFaultActive[pumpIndex];
  if (sensorFault && !wasFault) {
    _sensorFaultSinceMs[pumpIndex] = nowMs;   // début du nouvel épisode
    _sensorFaultAlerted[pumpIndex] = false;   // droit d'alerter réarmé (1 fois / épisode)
  } else if (!sensorFault) {
    // Le refus courant n'est plus une panne de mesure : l'épisode est clos. Une
    // panne ultérieure devra ré-attendre la temporisation complète.
    _sensorFaultSinceMs[pumpIndex] = 0;
    _sensorFaultAlerted[pumpIndex] = false;
  }
  // Cas restant (sensorFault && wasFault) : panne qui CHANGE DE NATURE pendant
  // l'épisode → on conserve la date de début ET `_sensorFaultAlerted`, donc
  // l'alerte part bien à 60 s de panne cumulée continue, et une seule fois.
  _sensorFaultActive[pumpIndex] = sensorFault;

  // La comparaison porte sur la forme RÉELLEMENT stockable (bornée + clampée) :
  // comparer le texte intégral à un tampon tronqué rendrait la transition
  // toujours vraie → une entrée de journal à CHAQUE tick (spam) pour une cause
  // inchangée. Tampon de pile (loopTask), aucune allocation.
  char next[kDoseBlockedReasonMaxLen];
  strlcpy(next, cause.c_str(), sizeof(next));
  clampReasonUtf8(next);
  if (strcmp(_lastRefusalCause[pumpIndex], next) != 0) {
    systemLogger.info(String("[Dosage ") + (pumpIndex == 0 ? "pH" : "ORP") +
                      "] Refus : " + cause);
    strlcpy(_lastRefusalCause[pumpIndex], next, kDoseBlockedReasonMaxLen);
  }

  // Escalade `warning` + alerte MQTT dédiée, UNE SEULE FOIS par épisode, après
  // kDoseSensorFaultAlertDelayMs de panne CONTINUE (la nature de la panne peut
  // varier à l'intérieur de l'épisode, cf. ci-dessus). Le délai écarte le
  // transitoire de ré-amorçage du filtre à la reprise de circulation, où l'âge de
  // la dernière mesure acceptée est encore celui d'avant la coupure.
  // Aucune allocation hors de cette branche (franchie au plus une fois par
  // épisode) : arithmétique non signée, wrap millis() sans effet.
  if (sensorFault && !_sensorFaultAlerted[pumpIndex] &&
      (nowMs - _sensorFaultSinceMs[pumpIndex]) >= kDoseSensorFaultAlertDelayMs) {
    const String chem = (pumpIndex == 0) ? "pH" : "ORP";
    const String msg = "[Dosage " + chem + "] Refus : " + cause +
                       " — régulation " + chem +
                       " à l'arrêt depuis " +
                       String((nowMs - _sensorFaultSinceMs[pumpIndex]) / 1000UL) +
                       " s : vérifier la sonde";
    // UNE SEULE trace de journal (revue) : publishAlert() journalise déjà le
    // message en `warning` (« Alerte: <type> - <message> »), y compris quand MQTT
    // est déconnecté. Un systemLogger.warning(msg) ici produisait deux lignes
    // quasi identiques par épisode dans une partition `history` de 64 Ko.
    mqttManager.publishAlert(
        (pumpIndex == 0) ? "ph_dose_sensor_fault" : "orp_dose_sensor_fault", msg);
    _sensorFaultAlerted[pumpIndex] = true;
  }
  // D1 : canDose() prend la propriété de la chaîne — inconditionnellement, même
  // quand le texte est inchangé : c'est bien lui qui refuse MAINTENANT, et la
  // relâche d'une garde ne doit pas effacer ce refus. Simple écriture d'un octet.
  _refusalCauseFromGuard[pumpIndex] = false;
}

void PumpControllerClass::resetRefusalLogState(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  _lastRefusalCause[pumpIndex][0] = '\0';
  _lastRefusalCode[pumpIndex] = kDoseBlockedCodeNone;  // v2.25.1 : code ⇔ texte
  // Dosage de nouveau autorisé : l'épisode de panne (s'il y en avait un) est
  // clos — le prochain devra ré-attendre la temporisation avant d'alerter.
  _sensorFaultSinceMs[pumpIndex] = 0;
  _sensorFaultAlerted[pumpIndex] = false;
  _sensorFaultActive[pumpIndex] = false;
  _refusalCauseFromGuard[pumpIndex] = false;
}

// D1 — pose une cause de refus au nom d'une GARDE de sécurité (surveillance
// pH/ORP ou escalade). Prend la propriété de la chaîne : seule une garde pourra
// l'effacer, et uniquement à sa relâche.
void PumpControllerClass::setGuardRefusalCause(int pumpIndex, const String& cause) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  // v2.25.1 : sentinelle « cause de garde » — ces messages portent des valeurs
  // runtime (seuils, mesures) et ne sont pas énumérables ; le client les affiche
  // via le TEXTE. Posée AVANT le texte (cf. logRefusalOnce) pour qu'un lecteur
  // async ne puisse jamais associer un texte de garde à un code de canDose().
  _lastRefusalCode[pumpIndex] = kDoseBlockedCodeGuard;
  storeRefusalCause(pumpIndex, cause.c_str());
  _refusalCauseFromGuard[pumpIndex] = true;
}

// D1 — efface la cause SI une garde en est propriétaire. Appelée depuis les
// branches de RELÂCHE des deux surveillances, qui tournent à chaque tick hors de
// toute conjonction : c'est ce qui garantit que la chaîne reflète l'état COURANT
// même en mode `manual`, où canDose() n'est jamais appelé et n'effaçait donc
// jamais rien.
// Un refus posé par canDose() (filtration arrêtée, quota, anti-rafale…) n'est
// JAMAIS touché ici : il décrit un blocage réel dont la garde ne sait rien.
// No-op strict hors propriété garde → aucune écriture de String, donc aucune
// allocation récurrente dans le chemin du tick.
void PumpControllerClass::clearGuardRefusalCause(int pumpIndex) {
  if (pumpIndex < 0 || pumpIndex > 1) return;
  if (!_refusalCauseFromGuard[pumpIndex]) return;
  _refusalCauseFromGuard[pumpIndex] = false;
  _lastRefusalCause[pumpIndex][0] = '\0';
  _lastRefusalCode[pumpIndex] = kDoseBlockedCodeNone;  // v2.25.1 : code ⇔ texte
}

// =============================================================================
// Gardes de sécurité pH (conditions pool-chemistry) — refonte v2.25.0
// =============================================================================
// Refonte « seuils fixes dérivés » : le plancher CONTINU asservi à l'offset est
// abandonné. Il était juste, mais sa complexité (formule asservie, garde de
// cohérence, marge structurelle) a elle-même désactivé la régulation en
// production le 25/08/2026. Les seuils sont désormais FIXES et dérivés de la
// plage de consigne (cf. constants.h, source unique).
//
// DEUX RÉFÉRENTIELS, choisis délibérément seuil par seuil :
//   - les seuils CHIMIQUES portent sur le pH CORRIGÉ (brut + offset), parce que
//     l'offset est une correction de MESURE : une fois appliqué, la valeur EST le
//     pH de l'eau, et un seuil chimique doit porter sur le pH réel ;
//   - les deux BUTÉES de dernier recours portent sur le pH BRUT, pour rester
//     valides quand l'offset lui-même est faux (cf. ph_offset.h / ADR-0039).
//
// Le pH corrigé est RECALCULÉ ici, applyPhOffset(getPhFilteredRaw(), offset), et
// non lu via getPhFiltered() : getPhFilteredRaw() est le seul accesseur qui porte
// la garde d'âge du filtre (pool-chemistry P2 — une EMA périmée ne doit jamais
// déclencher un refus fantôme). Ajouter cette garde d'âge à getPhFiltered()
// casserait au contraire le contrat de /data, du WebSocket et de MQTT.
//
// Appelées EN AMONT de canDose() dans update() (loopTask) : quand elles bloquent,
// canDose() n'est pas évalué et `_lastRefusalCause` porte la cause de la garde.
//
// SÉPARATION SURVEILLANCE / DÉCISION (contre-visite C1) : la DÉTECTION vit dans
// tickPhGuardSurveillance() / tickOrpPhInhibitSurveillance(), appelées
// inconditionnellement à chaque tick d'update() ; isPhGuardBlocking() et
// isOrpInhibitedByPh() ne font plus que rendre le verdict du tick. Motif : ces
// deux dernières sont appelées DANS une conjonction `&&` d'éligibilité, donc pas
// appelées du tout dès qu'un terme amont est faux (quota journalier épuisé, mode
// `manual` — l'état où l'escalade laisse justement l'appareil). Y laisser la
// surveillance revenait à promettre des alertes que le court-circuit supprimait
// exactement quand elles comptaient.

// Surveillance pH INCONDITIONNELLE (contre-visite C1) — détection, hystérésis,
// logs et alertes MQTT. Ne compte RIEN, n'escalade JAMAIS : un appareil laissé en
// mode `manual` s'auto-escaladerait sinon, alors qu'il ne refuse aucun dosage.
// Aucune transaction I²C : getPhFilteredRaw() et getPhOffset() lisent des caches.
void PumpControllerClass::tickPhGuardSurveillance() {
  // --- Snapshot du produit de correction, lu UNE FOIS par tick ---
  // Changer de produit change de seuil ET de direction : les drapeaux
  // d'hystérésis pH deviennent caducs et sont réarmés. Le compteur de refus
  // n'est PAS réinitialisé — une anomalie durable ne doit pas s'effacer d'un
  // simple changement de réglage.
  const bool isPhPlus = (mqttCfg.phCorrectionType == "ph_plus");
  const uint8_t corrKind = isPhPlus ? 1 : 0;
  if (corrKind != _phCorrectionKind) {
    _phCorrectionKind = corrKind;
    _phLowBlockActive = false;
    _phHighBlockActive = false;
    _phRawHardBlockActive = false;
    _phRawHardHighBlockActive = false;
    // R3 (revue v2.25.0) : le latch de refus NON ENCORE COMPTABILISÉ doit être
    // réarmé avec les drapeaux. Sans cela, un refus latché sous `ph_minus`
    // (plancher / butée basse) était consommé APRÈS le passage à `ph_plus` et
    // comptait pour un franchissement qui n'a jamais eu lieu dans le nouveau
    // référentiel. Le COMPTEUR des 3 refus / 24 h et sa fenêtre ne sont PAS
    // touchés (invariant du gate : une anomalie durable ne s'efface pas d'un
    // changement de réglage).
    _phGuardRefusalPending = false;
    systemLogger.info(String("[Dosage pH] Produit de correction : ") +
                      (isPhPlus ? "pH+ (base)" : "pH- (acide)") +
                      " — gardes de seuil réarmées (compteur de refus conservé)");
  }

  const float phRaw = sensors.getPhFilteredRaw();
  // Mesure indisponible (garde d'âge du filtre, pool-chemistry P2) : les gardes
  // NaN existantes (canDose / filtre non prêt) traitent déjà le cas — ne pas
  // fabriquer ici de refus fantôme, ni d'ALERTE fantôme, ni de refus à compter.
  if (isnan(phRaw)) {
    _phLowBlockActive = false;
    _phHighBlockActive = false;
    _phRawHardBlockActive = false;
    _phRawHardHighBlockActive = false;
    _phLowWarnActive = false;
    _phScheduledAlertSent = false;
    _phGuardBlockingNow = false;
    _phGuardRefusalPending = false;
    // D1 : aucune garde pH ne bloque (mesure indisponible = pas de verdict) —
    // une cause de garde encore affichée serait périmée. Le refus RÉEL de ce
    // cas (NaN / filtre non prêt) est posé par canDose(), qui en est
    // propriétaire et n'est donc pas écrasé ici.
    clearGuardRefusalCause(0);
    return;
  }
  const float offset = sensors.getPhOffset();
  const float ph = applyPhOffset(phRaw, offset);  // pH CORRIGÉ = pH de l'eau

  // --- Alerte NON BLOQUANTE d'eau acide (signal précoce, condition du gate) ---
  // Le seuil de BLOCAGE est descendu à 6,70 pour ne plus jamais mordre en
  // fonctionnement nominal ; ce signal à 7,00 reste, lui, le premier avertisseur.
  const bool lowWarn = phGuardBlocksBelow(ph, kPhLowWarn, _phLowWarnActive);
  if (lowWarn && !_phLowWarnActive) {
    _phLowWarnActive = true;
    const String msg = "pH " + String(ph, 2) + " sous " + String(kPhLowWarn, 2) +
                       " — eau agressive (liner, joints, parties métalliques). "
                       "Dosage encore autorisé jusqu'à " + String(kPhFloorCorrected, 2) +
                       " ; contrôler le pH au réactif.";
    systemLogger.warning("[Dosage pH] " + msg);
    mqttManager.publishAlert("ph_low_warning", msg);
  } else if (!lowWarn && _phLowWarnActive) {
    _phLowWarnActive = false;
    systemLogger.info("[Dosage pH] pH remonté à " + String(ph, 2) +
                      " — alerte d'eau acide levée");
  }

  // --- Gardes BLOQUANTES, selon le produit déclaré ---
  bool blocked = false;
  bool newRefusal = false;  // transition « ne bloquait pas » → « bloque » sur ce tick

  if (isPhPlus) {
    // Produit BASIQUE : seule une sur-alcalinisation est possible. Durcissement
    // v2.25.0 — le pH+ n'était gardé par AUCUN seuil jusqu'ici.
    // Contre-visite C3 : la butée BRUTE haute est évaluée d'abord, symétriquement
    // à la butée brute basse du produit acide — c'est le seul signal disponible
    // quand l'offset est faux, et sa cause de refus doit primer dans le journal.
    const bool rawHardHigh =
        phGuardBlocksAbove(phRaw, kPhCeilingRawHard, _phRawHardHighBlockActive);
    if (rawHardHigh && !_phRawHardHighBlockActive) {
      _phRawHardHighBlockActive = true;
      newRefusal = true;
      const String msg = "pH BRUT " + String(phRaw, 2) + " au-dessus de la butée de sécurité " +
                         String(kPhCeilingRawHard, 2) + " (offset=" + String(offset, 2) +
                         ") — injection de pH+ refusée quelle que soit la valeur "
                         "corrigée : offset négatif probablement erroné, contrôler "
                         "le pH au réactif";
      systemLogger.critical("[Dosage pH] " + msg);
      mqttManager.publishAlert("ph_guard", msg);
      setGuardRefusalCause(0, "pH brut au-dessus de la butée de sécurité (" + String(phRaw, 2) +
                              " > " + String(kPhCeilingRawHard, 2) + ")");
    } else if (!rawHardHigh && _phRawHardHighBlockActive) {
      _phRawHardHighBlockActive = false;
      systemLogger.info("[Dosage pH] pH brut redescendu à " + String(phRaw, 2) + " (butée " +
                        String(kPhCeilingRawHard, 2) + " − hystérésis " +
                        String(kPhGuardReleaseHysteresis, 2) +
                        ") — butée brute haute levée");
    }

    const bool high = phGuardBlocksAbove(ph, kPhCeilingCorrected, _phHighBlockActive);
    if (high && !_phHighBlockActive) {
      _phHighBlockActive = true;
      newRefusal = true;
      const String msg = "pH " + String(ph, 2) + " au-dessus du plafond de sécurité " +
                         String(kPhCeilingCorrected, 2) + " (offset=" + String(offset, 2) +
                         ") — injection de pH+ refusée : au-delà, le chlore est peu "
                         "actif et la régulation ORP est inhibée";
      systemLogger.critical("[Dosage pH] " + msg);
      mqttManager.publishAlert("ph_guard", msg);
      // La butée brute, plus grave, ne doit pas être écrasée si elle bloque aussi.
      if (!_phRawHardHighBlockActive) {
        setGuardRefusalCause(0, "pH au-dessus du plafond de sécurité (" + String(ph, 2) +
                                " > " + String(kPhCeilingCorrected, 2) + ")");
      }
    } else if (!high && _phHighBlockActive) {
      _phHighBlockActive = false;
      systemLogger.info("[Dosage pH] pH redescendu à " + String(ph, 2) + " (plafond " +
                        String(kPhCeilingCorrected, 2) + " − hystérésis " +
                        String(kPhGuardReleaseHysteresis, 2) +
                        ") — injection de pH+ de nouveau possible");
    }
    blocked = rawHardHigh || high;
  } else {
    // Produit ACIDE : seuil chimique bas sur le CORRIGÉ + butée sur le BRUT.
    // La butée brute est évaluée d'abord : c'est le seul signal disponible quand
    // l'offset est faux, et sa cause de refus doit primer dans le journal.
    const bool rawHard = phGuardBlocksBelow(phRaw, kPhFloorRawHard, _phRawHardBlockActive);
    if (rawHard && !_phRawHardBlockActive) {
      _phRawHardBlockActive = true;
      newRefusal = true;
      const String msg = "pH BRUT " + String(phRaw, 2) + " sous la butée de sécurité " +
                         String(kPhFloorRawHard, 2) + " (offset=" + String(offset, 2) +
                         ") — injection d'acide refusée quelle que soit la valeur "
                         "corrigée : offset positif probablement erroné, contrôler "
                         "le pH au réactif";
      systemLogger.critical("[Dosage pH] " + msg);
      mqttManager.publishAlert("ph_guard", msg);
      setGuardRefusalCause(0, "pH brut sous la butée de sécurité (" + String(phRaw, 2) +
                              " < " + String(kPhFloorRawHard, 2) + ")");
    } else if (!rawHard && _phRawHardBlockActive) {
      _phRawHardBlockActive = false;
      systemLogger.info("[Dosage pH] pH brut remonté à " + String(phRaw, 2) + " (butée " +
                        String(kPhFloorRawHard, 2) + " + hystérésis " +
                        String(kPhGuardReleaseHysteresis, 2) +
                        ") — butée brute levée");
    }

    const bool low = phGuardBlocksBelow(ph, kPhFloorCorrected, _phLowBlockActive);
    if (low && !_phLowBlockActive) {
      _phLowBlockActive = true;
      newRefusal = true;
      const String msg = "pH " + String(ph, 2) + " sous le plancher de sécurité " +
                         String(kPhFloorCorrected, 2) + " (offset=" + String(offset, 2) +
                         ") — injection d'acide refusée";
      systemLogger.critical("[Dosage pH] " + msg);
      mqttManager.publishAlert("ph_guard", msg);
      // Cause exposée au WS (canDose n'est pas appelé tant que la garde bloque).
      // La butée brute, plus grave, ne doit pas être écrasée si elle bloque aussi.
      if (!_phRawHardBlockActive) {
        setGuardRefusalCause(0, "pH sous le plancher de sécurité (" + String(ph, 2) +
                                " < " + String(kPhFloorCorrected, 2) + ")");
      }
    } else if (!low && _phLowBlockActive) {
      _phLowBlockActive = false;
      systemLogger.info("[Dosage pH] pH remonté à " + String(ph, 2) + " (plancher " +
                        String(kPhFloorCorrected, 2) + " + hystérésis " +
                        String(kPhGuardReleaseHysteresis, 2) +
                        ") — injection d'acide de nouveau possible");
    }
    blocked = rawHard || low;
  }

  // --- Verdict du tick, consommé par la DÉCISION (isPhGuardBlocking) ---
  _phGuardBlockingNow = blocked;
  if (!blocked) {
    _phScheduledAlertSent = false;  // un nouvel épisode alertera immédiatement
    // Épisode terminé sans qu'aucune branche de dosage n'ait été éligible : il
    // n'y a eu AUCUN refus réel, le latch est abandonné sans rien comptabiliser.
    _phGuardRefusalPending = false;
    // D1 (défaut CORRIGÉ ici) : la RELÂCHE efface la cause de garde. C'est le
    // seul point du firmware qui tourne à chaque tick quel que soit le mode —
    // `canDose(0)` ne le fait qu'en mode `automatic` (court-circuit `&&`), donc
    // en mode `manual` la chaîne restait figée sur un blocage révolu, y compris
    // le message d'escalade, alors que c'est précisément le mode où l'UI expose
    // l'injection manuelle. Inclut donc aussi l'effacement du message
    // « régulation pH basculée en manuel » : une fois la butée levée, il ne
    // décrit plus un refus en cours (la trace de l'escalade est conservée à part,
    // via setPhAutoManualTrace + la bannière d'acquittement).
    clearGuardRefusalCause(0);
    return;
  }
  // Latch armé au plus UNE fois par tick — comme l'ex-`newRefusal` : deux
  // franchissements simultanés (butée brute + seuil corrigé) valent un seul
  // refus, exactement comme avant la séparation surveillance / décision.
  if (newRefusal) _phGuardRefusalPending = true;
}

// DÉCISION pH (contre-visite C1) : rend le verdict posé par la surveillance du
// tick et n'y ajoute QUE ce qui relève d'un refus RÉEL. Appelée sous conjonction
// `&&` — c'est précisément pourquoi elle ne détecte plus rien.
//
// `countRefusal` = true depuis la branche AUTOMATIQUE seulement : là, un refus
// alimente le compteur 24 h et peut escalader jusqu'à la bascule fail-closed en
// manuel. La branche PROGRAMMÉE passe false — elle refuse tout autant (c'est un
// filet chimique, pas une boucle de régulation) mais ne compte ni n'escalade :
// une consigne volumétrique choisie par l'utilisateur ne doit pas fermer la
// régulation. Elle signale à la place, en warn + alerte MQTT à débit limité,
// sans quoi un refus permanent y serait un no-op SILENCIEUX.
bool PumpControllerClass::isPhGuardBlocking(bool countRefusal) {
  if (!_phGuardBlockingNow) return false;

  // --- Comptabilisation : mode AUTOMATIQUE uniquement (condition du gate) ---
  // Les refus de BUTÉE BRUTE y sont inclus : ils sont le SEUL signal d'un offset
  // faux, donc précisément ce que le compteur doit faire remonter.
  // Le latch est consommé ici (et non relu comme une transition) : le
  // franchissement a pu survenir alors que cette branche était inéligible
  // (quota épuisé) — le refus n'en est pas moins réel dès qu'elle le redevient.
  if (countRefusal && _phGuardRefusalPending) {
    _phGuardRefusalPending = false;
    const uint32_t nowMs = (uint32_t)millis();
    if (_phGuardRefusals == 0 ||
        (nowMs - _phGuardWindowStart) >= kPhGuardRefusalWindowMs) {
      _phGuardWindowStart = nowMs;
      _phGuardRefusals = 0;
    }
    if (_phGuardRefusals < 255) _phGuardRefusals++;
    if (_phGuardRefusals >= kPhGuardRefusalMax &&
        mqttCfg.phRegulationMode == "automatic") {
      // Répétition = anomalie durable (sonde morte, offset à l'envers, produit
      // mal déclaré), pas un incident ponctuel : on ferme la régulation pH
      // (fail-closed) et on le dit.
      mqttCfg.phRegulationMode = "manual";
      // INVARIANT DU PROJET (revue) : `ph_enabled` est le miroir de « mode !=
      // manual » (ws_manager.cpp, web_routes_config.cpp, mqtt_manager.cpp). Le
      // laisser à true ici persisterait en NVS un état incohérent : l'UI et Home
      // Assistant afficheraient la régulation pH ACTIVE après un basculement de
      // sécurité, exactement quand il faut que l'utilisateur voie qu'elle ne
      // l'est plus.
      mqttCfg.phEnabled = false;
      // v2.25.0 : le message NE PRÉSUME PLUS de la cause. L'ancien texte
      // (« sonde pH à recalibrer ou remplacer ») a induit l'utilisateur en erreur
      // le 2026-08-25 alors que le diagnostic EZO était nominal (pente acide
      // 101,0 %, base 100,0 %, zéro −1,03 mV) et que le vrai coupable était un
      // décalage de référence EXTERNE. On interroge donc le diagnostic EZO et on
      // formule en conséquence.
      const float slopeAcid = sensors.getPhSlopeAcid();
      const float slopeBase = sensors.getPhSlopeBase();
      const float zeroMv    = sensors.getPhSlopeZero();
      const String diag = "(pente acide " + String(slopeAcid, 1) + " %, base " +
                          String(slopeBase, 1) + " %, zéro " + String(zeroMv, 2) + " mV)";
      String alert = String(kPhGuardRefusalMax) +
                     " refus de garde pH en 24 h — régulation pH basculée en manuel. ";
      if (isPhProbeHealthy(slopeAcid, slopeBase, zeroMv)) {
        alert += "Diagnostic sonde EZO NORMAL " + diag +
                 " : la sonde n'est pas en cause. Vérifier la cohérence consigne / "
                 "offset / produit déclaré, puis un décalage de référence EXTERNE "
                 "(courants de fuite de la pompe de filtration, équipotentialité du "
                 "bassin). Contrôler le pH au réactif avant toute intervention sur "
                 "la sonde.";
      } else {
        alert += "Diagnostic sonde EZO hors norme ou indisponible " + diag +
                 " : vérifier la calibration et l'état de la sonde, puis un éventuel "
                 "décalage de référence externe (courants de fuite, équipotentialité). "
                 "Contrôler le pH au réactif.";
      }
      // v2.20.3 (C4) : PERSISTER la cause de la bascule AVANT saveMqttConfig(),
      // pour qu'elle parte dans la même transaction NVS que le mode lui-même.
      // Le journal système est un ring buffer : après quelques heures il ne reste
      // AUCUNE trace de l'escalade (incident 2026-08-16 — impossible de dire si
      // le mode `manual` venait de l'escalade ou d'une action humaine).
      setPhAutoManualTrace(alert);
      saveMqttConfig();
      systemLogger.critical("[Dosage pH] " + alert);
      mqttManager.publishAlert("ph_guard", alert);
      setGuardRefusalCause(0, "régulation pH basculée en manuel (refus de garde répétés)");
    }
  }

  // --- Mode PROGRAMMÉ : signalement périodique, sans compteur ni escalade ---
  if (!countRefusal) {
    const uint32_t nowMs = (uint32_t)millis();
    if (!_phScheduledAlertSent ||
        (nowMs - _phScheduledAlertMs) >= kPhGuardScheduledAlertIntervalMs) {
      _phScheduledAlertSent = true;
      _phScheduledAlertMs = nowMs;
      // R2 : `_lastRefusalCause[0]` est un tampon char[] — String(...) explicite
      // (concaténation littéral + char* impossible sans elle).
      const String msg = "Dosage pH programmé refusé — " +
                         String(_lastRefusalCause[0]) +
                         ". Le volume quotidien ne sera pas injecté tant que la garde "
                         "tient ; la régulation n'est PAS basculée en manuel dans ce "
                         "mode. Contrôler le pH au réactif, l'offset et le produit "
                         "déclaré.";
      systemLogger.warning("[Dosage pH] " + msg);
      mqttManager.publishAlert("ph_guard_scheduled", msg);
    }
  }
  return true;
}

// Surveillance INCONDITIONNELLE de l'inhibition ORP sur pH élevé (condition #3).
//
// Contre-visite C1 : cette détection vivait dans isOrpInhibitedByPh(), appelée
// sous conjonction `&&`. Quota chlore journalier atteint (`orpSafetyOk` faux) et
// l'alerte « pH élevé, chlore inhibé » — la SEULE qui désigne la vraie cause du
// chlore qui ne monte pas — ne partait jamais. Elle est donc évaluée ici, à
// chaque tick, hors de toute conjonction.
//
// La politique `requireKnownPh` (ex-paramètre) est dérivée du mode ORP COURANT :
// la branche automatique de update() n'est atteignable que si le mode EST
// "automatic", et la branche programmée que si le mode EST "scheduled" — le
// verdict rendu à chaque décision est donc bien celui de sa propre politique.
// true pour la régulation AUTOMATIQUE (fail-closed sur pH inconnu), false pour le
// dosage PROGRAMMÉ (comportement historique préservé) et pour tout autre mode.
//
// POURQUOI LE FAIL-CLOSED EN AUTO (pool-chemistry P2, correctif bloquant) :
// `canDose(1)` ne consulte JAMAIS le pH — ce filet est donc l'UNIQUE garde du
// couplage pH/chlore. Si la sonde pH est morte alors que l'eau est réellement
// au-dessus de 7,60, le chlore y est peu actif (fraction HClO effondrée) et
// l'asservissement ORP compenserait en surdosant. Un pH inconnu doit donc
// suspendre la chloration automatique, pas la laisser courir à l'aveugle.
//
// v2.25.0 : le seuil chimique kPhOrpInhibit porte désormais sur le pH CORRIGÉ
// (la fraction de HClO dépend du pH RÉEL de l'eau), doublé d'une butée BRUTE
// inconditionnelle à kPhCeilingRawHard contre un offset négatif saisi à tort.

// Gravité CROISSANTE des motifs d'inhibition ORP (R2, anti-rafale d'alertes).
// L'échelle n'est PAS l'ordre des valeurs de `_orpPhInhibitKind` — celles-ci sont
// des étiquettes, pas un rang :
//   1 = mesure ABSENTE (kind 1 pH indisponible, kind 2 stabilisation) — le motif
//       le moins grave, c'est une suspension prudentielle sur une donnée manquante ;
//   2 = seuil CORRIGÉ franchi (kind 0) — l'eau EST au-dessus du plafond ;
//   3 = butée BRUTE haute franchie (kind 3) — le plus grave, il vaut quel que
//       soit l'offset.
// 0 est réservé à la sentinelle « épisode vierge » de `_orpPhAlertedSeverity` :
// toute gravité réelle la dépasse, donc la première alerte d'un épisode part
// toujours.
static inline uint8_t orpPhInhibitSeverity(uint8_t kind) {
  switch (kind) {
    case 3:  return 3;  // butée BRUTE haute
    case 0:  return 2;  // seuil CORRIGÉ
    default: return 1;  // 1 = pH indisponible, 2 = stabilisation des mesures
  }
}

void PumpControllerClass::tickOrpPhInhibitSurveillance() {
  const bool requireKnownPh = (mqttCfg.orpRegulationMode == "automatic");
  const float phRaw = sensors.getPhFilteredRaw();

  if (isnan(phRaw)) {
    if (!requireKnownPh) {
      // Dosage programmé (ou tout autre mode) : aveugle à la mesure par
      // conception, on ne change rien.
      _orpPhInhibitActive = false;
      _orpPhInhibitKind = 0;
      _orpPhInhibitNow = false;
      // R2 : levée de l'inhibition = FIN d'épisode → la mémoire anti-rafale est
      // effacée ici aussi, sinon un motif déjà signalé resterait inhibable à vie.
      _orpPhAlertedSeverity = 0;
      _orpPhCauseKind = 255;
      // D1 : aucune inhibition ORP dans ce mode — une cause de garde encore
      // affichée serait périmée (canDose(1) n'est pas appelé hors automatique).
      clearGuardRefusalCause(1);
      return;
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
    // Motif de mesure absente, réévalué à chaque tick : 2 = purge de la chambre
    // de mesure (état NORMAL), 1 = capteur muet / filtre non amorcé (anomalie).
    // getSnapshot() lit un cache sous spinlock — aucune transaction I²C.
    bool waterPresent = false, measuresValid = false;
    uint32_t remainingS = 0;
    measureValidity.getSnapshot(waterPresent, measuresValid, remainingS);
    const uint8_t nanKind = measuresValid ? 1 : 2;

    if (!_orpPhInhibitActive) {
      // POSE d'un nouvel épisode. La mesure absente est le motif le MOINS grave
      // (gravité 1) : un vrai franchissement de seuil survenant ensuite reste
      // admis comme MONTÉE en gravité et produira sa vraie cause — c'est le
      // durcissement de la passe précédente, préservé par construction ici.
      _orpPhInhibitActive = true;
      _orpPhInhibitKind = nanKind;
      _orpPhAlertedSeverity = orpPhInhibitSeverity(nanKind);
      if (nanKind == 2) {
        systemLogger.info("[Dosage ORP] Stabilisation des mesures en cours — dosage "
                          "chlore automatique suspendu (reprise automatique dès que "
                          "les mesures sont de nouveau représentatives)");
      } else {
        systemLogger.info("[Dosage ORP] pH indisponible (capteur muet ou filtre non "
                          "amorcé) — dosage chlore automatique suspendu par sécurité");
      }
    }
    // R2 (recommandation du gate) — CAUSE DE GARDE PÉRIMÉE. Après un passage à
    // NaN sous un latch de motif de seuil, la cause publiée continuait d'annoncer
    // l'ancienne valeur pH alors que la mesure n'existe plus. Seul le LIBELLÉ est
    // rafraîchi : ni le verdict (`_orpPhInhibitNow`), ni le motif latché
    // (`_orpPhInhibitKind`, qui porte le message de levée et le badge de butée
    // brute), ni la mémoire d'épisode ne sont touchés — une mesure qui disparaît
    // n'est pas une re-qualification, et surtout pas une DÉGRADATION admise.
    // Edge-triggered sur `_orpPhCauseKind` : un seul String par transition, aucune
    // allocation par tour de loop.
    if (_orpPhCauseKind != nanKind) {
      setGuardRefusalCause(1, nanKind == 2
                                  ? "stabilisation des mesures — dosage chlore suspendu"
                                  : "pH indisponible — dosage chlore suspendu");
      _orpPhCauseKind = nanKind;
    }
    _orpPhInhibitNow = true;
    return;
  }

  const float offset = sensors.getPhOffset();
  const float ph = applyPhOffset(phRaw, offset);  // pH CORRIGÉ = pH de l'eau

  // Deux seuils en OU logique : ils vont dans la MÊME direction (haut) et
  // décrivent le même état « chloration suspendue » — la levée exige donc que les
  // DEUX soient franchement retombés, ce qui est fail-closed.
  //
  // R1 (revue v2.25.0) : la mémoire d'hystérésis n'est PAS `_orpPhInhibitActive`.
  // Ce dernier est un latch d'inhibition que les branches NaN ci-dessus arment
  // pour des motifs étrangers à tout seuil (kind 1 = pH indisponible, kind 2 =
  // stabilisation) : le passer ici en `wasBlocking` faisait croire à l'hystérésis
  // qu'elle SORTAIT d'un blocage de seuil jamais franchi, et maintenait
  // l'inhibition du chlore dans toute la bande [seuil − hystérésis, seuil] —
  // indéfiniment, à chaque reprise de filtration (pH 7,57 sous un seuil de 7,60
  // jamais atteint).
  //
  // R2 (contre-visite v2.25.0) : et ce n'est pas UNE mémoire mais DEUX, une PAR
  // SEUIL. Un drapeau unique écrit par `(rawCeil || corrCeil)` neutralisait
  // l'hystérésis du seuil le moins souvent armé : tant que le seuil CORRIGÉ le
  // maintenait à true, la butée BRUTE redevenait un simple comparateur à
  // 8,05 (= 8,10 − hystérésis) et re-qualifiait le motif à chaque oscillation.
  // Chaque mémoire est donc passée en `wasBlocking` à SA SEULE comparaison, dans
  // SON SEUL référentiel.
  const bool rawCeil  = phGuardBlocksAbove(phRaw, kPhCeilingRawHard, _orpPhRawThresholdActive);
  const bool corrCeil = phGuardBlocksAbove(ph, kPhOrpInhibit, _orpPhCorrThresholdActive);
  // Mémoires d'hystérésis : écrites ICI et NULLE PART AILLEURS, chacune depuis le
  // résultat de SA propre comparaison (jamais depuis le OU des deux, jamais
  // depuis une branche NaN — cf. R1 ci-dessus).
  _orpPhRawThresholdActive  = rawCeil;
  _orpPhCorrThresholdActive = corrCeil;

  if (!rawCeil && !corrCeil) {
    if (_orpPhInhibitActive) {
      // v2.20.3 (C6) : le message de levée doit refléter la cause réellement
      // journalisée à la pose, sinon « redescendu à 7,10 » sort après un simple
      // warmup où le pH n'était jamais monté.
      if (_orpPhInhibitKind == 2) {
        systemLogger.info("[Dosage ORP] Stabilisation terminée, pH de nouveau "
                          "mesuré (" + String(ph, 2) +
                          ") — dosage chlore de nouveau possible");
      } else if (_orpPhInhibitKind == 1) {
        systemLogger.info("[Dosage ORP] pH de nouveau disponible (" + String(ph, 2) +
                          ") — dosage chlore de nouveau possible");
      } else if (_orpPhInhibitKind == 3) {
        systemLogger.info("[Dosage ORP] pH brut redescendu à " + String(phRaw, 2) +
                          " (butée " + String(kPhCeilingRawHard, 2) +
                          ") — dosage chlore de nouveau possible");
      } else {
        systemLogger.info("[Dosage ORP] pH redescendu à " + String(ph, 2) +
                          " — dosage chlore de nouveau possible");
      }
    }
    _orpPhInhibitActive = false;
    _orpPhInhibitKind = 0;
    _orpPhInhibitNow = false;
    // R2 : SEUL point (avec la levée du mode non automatique ci-dessus) où la
    // mémoire anti-rafale d'épisode est effacée. Elle DOIT l'être ici et pas
    // avant : l'effacer sur une simple retombée de motif rouvrirait la porte au
    // battement d'alertes que cette mémoire ferme.
    _orpPhAlertedSeverity = 0;
    _orpPhCauseKind = 255;
    // D1 (défaut CORRIGÉ ici) : la RELÂCHE efface la cause de garde ORP.
    // Symétrique du pH : hors mode `automatic`, canDose(1) n'est jamais appelé,
    // la chaîne restait donc figée sur une inhibition révolue.
    clearGuardRefusalCause(1);
    return;
  }

  // Nature de l'inhibition IMPOSÉE PAR LES SEUILS sur ce tick. La butée BRUTE
  // prime : elle vaut quel que soit l'offset, donc elle est la plus grave.
  const uint8_t thresholdKind = rawCeil ? 3 : 0;

  // ===== R2 : UNE alerte par GRAVITÉ et par ÉPISODE, jamais en dégradation ====
  // Ancien edge-trigger : `!_orpPhInhibitActive || _orpPhInhibitKind != thresholdKind`.
  // Il admettait la re-qualification DANS LES DEUX SENS. Avec la butée brute qui
  // s'arme à 8,10 et se relâche à 8,05 pendant que le seuil corrigé tient en
  // continu, un bassin à pH 8,03-8,12 (offset 0,00) faisait basculer le motif
  // 3 ↔ 0 à chaque mesure : un critical + une alerte MQTT par oscillation, avec
  // DEUX messages contradictoires en alternance. Sur la partition `history` de
  // 64 Ko, ce battement chasse aussi tout le reste du journal.
  //
  // Règle retenue : la re-qualification n'est admise que vers un motif PLUS GRAVE
  // (butée brute > seuil corrigé > mesure absente), et une gravité déjà signalée
  // dans l'épisode ne réémet rien. La comparaison stricte `>` sur la gravité code
  // les deux exigences d'un coup. La mémoire n'est effacée qu'à la LEVÉE de
  // l'inhibition (deux points, plus haut).
  //
  // Le durcissement de la passe précédente est PRÉSERVÉ : un vrai franchissement
  // pendant une inhibition de motif « mesure absente » (gravité 1) est une MONTÉE
  // en gravité (2 ou 3 > 1) — il produit donc bien son critical, son alerte MQTT
  // et sa vraie cause, au lieu de laisser l'UI annoncer « stabilisation des
  // mesures » sur une eau réellement au-dessus du plafond.
  const uint8_t severity = orpPhInhibitSeverity(thresholdKind);
  const bool escalate = (severity > _orpPhAlertedSeverity);
  // Symétrique de la recommandation « cause périmée » traitée dans la branche NaN :
  // au RETOUR d'un interlude sans mesure, la cause exposée décrit encore
  // « stabilisation » ou « pH indisponible » alors qu'un seuil bloque de nouveau.
  // Le libellé est alors re-posé SANS alerter (la gravité a déjà été signalée dans
  // cet épisode). Cadré aux seuls libellés non-seuil (255, 1, 2) : une simple
  // oscillation 3 ↔ 0 ne re-pose RIEN, sinon on remplacerait le battement
  // d'alertes par un battement de causes.
  const bool refreshLabel = (_orpPhCauseKind != 0 && _orpPhCauseKind != 3);
  if (escalate || refreshLabel) {
    if (escalate) _orpPhAlertedSeverity = severity;
    if (rawCeil) {
      if (escalate) {
        // Butée BRUTE : la mesure capteur elle-même est au-dessus de la limite,
        // aucun offset ne peut le justifier. Seule une MONTÉE en gravité
        // re-qualifie le motif latché et journalise.
        _orpPhInhibitKind = 3;
        // Recommandation du gate : ne pas affirmer « au-dessus de la butée 8,10 »
        // quand la mesure est retombée DANS la bande de relâche — l'hystérésis
        // peut rester armée alors que le latch a été effacé (passage en mode non
        // automatique, mesure indisponible). La bande est donc libellée telle
        // quelle. Et l'offset n'est mis en cause que s'il est réellement NÉGATIF :
        // à offset ≥ 0 le pH corrigé est au moins égal au brut, l'eau est
        // réellement haute et accuser l'offset est un contresens (c'est
        // l'accusation que recevait le bassin à offset 0,00).
        String band;
        if (phRaw > kPhCeilingRawHard) {
          band = " au-dessus de la butée " + String(kPhCeilingRawHard, 2);
        } else {
          band = " maintenu dans la bande d'hystérésis de la butée " +
                 String(kPhCeilingRawHard, 2) + " (relâche à " +
                 String(kPhCeilingRawHard - kPhGuardReleaseHysteresis, 2) + ")";
        }
        const String diag = (offset < 0.0f)
                                ? String("offset négatif probablement erroné, contrôler "
                                         "le pH au réactif")
                                : String("l'eau est réellement au-dessus de la butée : "
                                         "contrôler le pH au réactif et le faire baisser");
        const String msg = "pH BRUT " + String(phRaw, 2) + band + " (offset=" +
                           String(offset, 2) +
                           ") — dosage ORP suspendu quelle que soit la valeur corrigée : " +
                           diag;
        systemLogger.critical("[Dosage ORP] " + msg);
        mqttManager.publishAlert("orp_ph_inhibit", msg);
      }
      // Cause exposée au WS (canDose(1) n'est pas appelé tant que l'inhibition dure).
      setGuardRefusalCause(1, "pH brut en butée haute (" + String(phRaw, 2) +
                              ") — dosage chlore suspendu");
      _orpPhCauseKind = 3;
    } else {
      if (escalate) {
        _orpPhInhibitKind = 0;  // v2.20.3 (C6) : vrai franchissement de seuil
        // Même libellé de bande que ci-dessus, et pour la même raison : « pH 7,58
        // > 7,60 » est faux et fait douter l'utilisateur de tout le message.
        String band;
        if (ph > kPhOrpInhibit) {
          band = " > " + String(kPhOrpInhibit, 2);
        } else {
          band = " maintenu dans la bande d'hystérésis du seuil " +
                 String(kPhOrpInhibit, 2) + " (relâche à " +
                 String(kPhOrpInhibit - kPhGuardReleaseHysteresis, 2) + ")";
        }
        const String msg = "pH " + String(ph, 2) + band + " (offset=" + String(offset, 2) +
                           ") — chlore peu actif à ce pH : dosage ORP suspendu "
                           "(faire baisser le pH)";
        systemLogger.critical("[Dosage ORP] " + msg);
        mqttManager.publishAlert("orp_ph_inhibit", msg);
      }
      setGuardRefusalCause(1, "pH trop élevé (" + String(ph, 2) +
                              ") — dosage chlore suspendu");
      _orpPhCauseKind = 0;
    }
  }
  // Latch d'inhibition posé HORS du bloc d'alerte : une gravité déjà signalée (ou
  // une dégradation, non re-qualifiée) n'émet rien mais l'inhibition, elle, court
  // toujours — c'est `(rawCeil || corrCeil)` qui la commande, jamais l'alerte.
  _orpPhInhibitActive = true;
  _orpPhInhibitNow = true;
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
  // v2.25.1 — état INTERNE du filtre : sert UNIQUEMENT à nommer la cause des
  // gardes 3/3b (aucune garde ajoutée, aucun verdict modifié). Deux lectures de
  // cache, aucun I²C.
  in.filterState = (pumpIndex == 0) ? sensors.getPhFilterState()
                                    : sensors.getOrpFilterState();
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
    // v2.25.1 — LIBELLÉS DÉCONFLATÉS. Les deux anciennes phrases mélangeaient
    // chacune plusieurs causes physiques (« warmup / EZO injoignable ») : l'UI en
    // extrayait un fragment et pouvait présenter une panne matérielle comme un
    // état normal. Chaque libellé ne décrit plus QU'UNE cause ; les deux causes
    // génériques ci-dessous disent explicitement qu'elles ne savent pas.
    case DoseRefusal::ReadingNaN:
      cause = "mesure filtrée indisponible (état du filtre indéterminé)";
      break;
    case DoseRefusal::FilterNotReady:
      cause = "filtre capteur non prêt (état du filtre indéterminé)";
      break;
    case DoseRefusal::ReadingNaNWarmup:
      cause = "mesure filtrée indisponible : fenêtre de lissage en cours";
      break;
    case DoseRefusal::ReadingNaNFrozen:
      cause = "mesure filtrée indisponible : sonde figée";
      break;
    case DoseRefusal::ReadingNaNStale:
      cause = "mesure filtrée indisponible : plus aucune mesure acceptée";
      break;
    case DoseRefusal::ReadingNaNGateClosed:
      cause = "mesure filtrée indisponible : mesures non représentatives";
      break;
    case DoseRefusal::FilterWarmup:
      cause = "filtre capteur en cours de remplissage (stabilisation)";
      break;
    case DoseRefusal::FilterFrozen:
      cause = "sonde figée (valeur constante) — filtre non prêt";
      break;
    case DoseRefusal::FilterStale:
      cause = "plus aucune mesure acceptée par le filtre (sonde muette)";
      break;
    case DoseRefusal::FilterGateClosed:
      cause = "mesures non représentatives (purge / circulation à l'arrêt)";
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

  logRefusalOnce(pumpIndex, cause, decision.cause);
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
// v2.26.0 — TERME INTÉGRAL pH : coquille du module pur ph_integral_logic.h
// =============================================================================
// Toute la logique (charge signée, oubli, reset sec, péremption, hystérésis de
// saturation, paliers et rappel) est dans src/ph_integral_logic.h, testable en
// natif. Les deux fonctions ci-dessous ne font que fournir le temps et appliquer
// le verdict.

// C4 — PÉREMPTION. La branche automatique n'est pas toujours atteinte : en mode
// `off`/`manual`, sous une garde bloquante durable ou filtration coupée, le tick
// cesse et l'intégrale reste figée telle quelle. Sans purge, elle serait servie
// intacte au retour en automatique, des jours plus tard, sur un bassin devenu
// étranger à la mesure qui l'a chargée.
void PumpControllerClass::tickPhIntegralExpiry(unsigned long now) {
  const float before = _phIntegral.integral;
  if (!phIntegralExpire(_phIntegral, (uint32_t)now)) return;

  // snprintf plutôt qu'une chaîne d'`operator+` : à messages identiques, chaque
  // concaténation génère son propre appel et coûte de la FLASH, ressource la plus
  // contrainte du projet (97 % occupés). Même patron pour tous les messages du
  // terme intégral.
  char msg[112];
  snprintf(msg, sizeof(msg),
           "Intégrale pH périmée (aucune évaluation depuis %lu h) : %.0f pH·s purgés",
           kPhIntegralStaleResetMs / 3600000UL, before);
  systemLogger.info(msg);

  phPID.integral = 0.0f;  // miroir servi par computePidPure et les accesseurs
  phPID.lastError = 0.0f;
  phPID.lastTime = 0;     // dt reconstruit au prochain tick (jamais synthétisé)
}

// Application du verdict d'un tick — journalisation et alertes UNIQUEMENT.
//
// C5 : « la régulation demande son maximum SANS atteindre la consigne » est la
// classe de défaut qui a coûté une saison à ce projet (bidon vide, tube
// désamorcé, fuite, sonde figée), et elle est INVISIBLE sans ce point de mesure.
// Le module pur la qualifie en deux termes (demande saturée ET erreur toujours
// au-dessus du seuil d'ARRÊT), la fait mûrir en temps de branche automatique, et
// REDIT le critical toutes les kPhIntegralSaturationReminderMs : l'ancienne
// version latchait une seule alerte par épisode, puis se taisait définitivement
// sur un état qui, lui, ne cessait pas.
void PumpControllerClass::applyPhIntegralVerdict(const PhIntegralVerdict& verdict,
                                                 float error) {
  // TAMPON DIMENSIONNÉ SUR LE PLUS LONG MESSAGE (le critical) — correction de
  // revue v2.26.0. À 240 octets, le compilateur signalait une troncature CERTAINE
  // (« format string expands to at least 321 ») : la phrase était coupée en plein
  // milieu de sa partie ACTIONNABLE — quoi vérifier sur l'installation — et
  // l'alerte MQTT partait amputée du même coup. Le message critical vaut ~352
  // octets une fois les accents comptés en UTF-8 (2 octets chacun) ; 448 laisse
  // près de 100 octets de marge pour des valeurs numériques inhabituelles.
  char msg[448];

  if (verdict.event == PhIntegralEvent::kDryReset && verdict.integralBefore != 0.0f) {
    // feature-084 : la tenue est rappelée dans le message — une purge sèche est
    // désormais la conclusion d'une excursion SOUTENUE, pas la réaction à un
    // à-coup. Sans cette précision, le journal laisserait croire au contraire.
    snprintf(msg, sizeof(msg),
             "Intégrale pH purgée (erreur %.3f — pH franchement au-delà de la "
             "consigne depuis plus de %lu min) : %.0f pH·s abandonnés",
             error, kPhIntegralResetHoldMs / 60000UL, verdict.integralBefore);
    systemLogger.info(msg);
  }

  if (verdict.emitClear) {
    snprintf(msg, sizeof(msg),
             "Intégrale pH sortie de saturation (demande %.2f mL/min pour %.2f "
             "mL/min soutenables) — alerte levée",
             verdict.demandMlPerMin, verdict.sustainableMlPerMin);
    systemLogger.info(msg);
  }

  // Les deux paliers partageaient ~80 % de leur texte, dupliqué en flash (ressource
  // la plus contrainte du projet, 97 % occupés). Le PRÉFIXE COMMUN est désormais un
  // littéral UNIQUE, écrit une fois ; seule la queue diffère. La longueur écrite
  // est reprise pour poursuivre dans le même tampon, ce qui évite aussi un second
  // format « toujours tronqué » : la taille restante est variable, donc bornée à
  // l'exécution et non par une constante trop petite.
  if (verdict.emitCritical || verdict.emitWarn) {
    const unsigned long heldHours =
        verdict.emitCritical
            ? (unsigned long)(verdict.anomalyHeldMs / 3600000UL)
            : (unsigned long)(kPhIntegralSaturationWarnMs / 3600000UL);
    int n = snprintf(msg, sizeof(msg),
                     "Régulation pH saturée depuis plus de %lu h (%.2f mL/min "
                     "demandés pour %.2f mL/min soutenables",
                     heldHours, verdict.demandMlPerMin,
                     verdict.sustainableMlPerMin);
    // snprintf rend la longueur qu'il AURAIT écrite : bornage obligatoire avant
    // de s'en servir comme décalage (troncature = n ≥ sizeof(msg)).
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(msg) - 1) n = (int)sizeof(msg) - 1;
    const size_t room = sizeof(msg) - (size_t)n;

    if (verdict.emitCritical) {
      snprintf(msg + n, room,
               ", terme intégral %.2f mL/min) — la régulation demande son maximum "
               "SANS atteindre la consigne : vérifier le niveau et l'amorçage du "
               "bidon, l'étanchéité du circuit de dosage, la sonde pH et la limite "
               "horaire d'injection",
               getPhIntegralTerm());
      systemLogger.critical(msg);
    } else {
      snprintf(msg + n, room,
               ") — la correction n'a pas d'effet mesurable sur le pH");
      systemLogger.warning(msg);
    }
    mqttManager.publishAlert("ph_integral_saturated", msg);
  }
}

// --- C5 : accesseurs LECTURE SEULE exposés à /data et au WS ------------------
float PumpControllerClass::getPhIntegral() const {
  const float v = phPID.integral;
  return isfinite(v) ? v : NAN;
}

float PumpControllerClass::getPhIntegralTerm() const {
  const float v = phPID.ki * phPID.integral;
  return isfinite(v) ? v : NAN;
}

float PumpControllerClass::getPhIntegralPct() const {
  // Remplissage de l'ANTI-WINDUP (% de integralMax, valeur absolue) — grandeur
  // d'observation, distincte du prédicat d'alerte C5 qui, lui, porte sur le
  // rapport « demande / débit soutenable » (cf. ph_integral_logic.h).
  return phIntegralFillPct(phPID.integral, phPID.integralMax);
}

// =============================================================================
// feature-084 (sujet 1) — CADENCE : le tick sort de la conjonction de dosage
// =============================================================================
// Justification complète et tableau état par état : cf. l'en-tête de
// phIntegralAccumulationAllowed() dans pump_controller.h.
//
// PRÉCONDITION D'APPEL, structurelle et non testée ici : cette fonction n'est
// appelée qu'APRÈS la garde « présence d'eau + stabilisation post-cal +
// assistant de débit » d'update(), qui rend la main. Filtration coupée, eau
// absente, OTA et mode maintenance sont donc exclus GRATUITEMENT — c'est
// précisément ce qui permet de ne pas rappeler resolveWaterPresence() ici (un
// second appel par tour de loop pour une réponse déjà connue). Déplacer l'appel
// en amont de cette garde romprait l'exigence chimique « pas d'accumulation en
// eau stagnante » : le point d'appel est donc à préserver.
bool PumpControllerClass::phIntegralAccumulationAllowed() {
  // 1. WATCHDOG. Même exigence que canDose() : aucune autorité de dosage, même
  // en cours de constitution, ne s'arme sans chien de garde actif.
  if (esp_task_wdt_status(NULL) != ESP_OK) return false;

  // 2. MODE DE RÉGULATION. En `manual`/`off` le dosage est durablement
  // impossible : accumuler y constituerait une charge sans contrepartie, qui
  // serait libérée d'un bloc au retour en automatique.
  if (mqttCfg.phRegulationMode != "automatic") return false;

  // 3. GARDE pH BLOQUANTE — fail-closed. On lit le VERDICT du tick courant, posé
  // par tickPhGuardSurveillance() quelques lignes plus haut : isPhGuardBlocking()
  // COMPTE les refus et peut escalader, un prédicat d'entretien ne doit avoir
  // AUCUN effet de bord. (L'escalade des 3 refus, elle, bascule la régulation en
  // `manual` et est donc déjà couverte par le point 2.)
  if (_phGuardBlockingNow) return false;

  // 4. PORTE DE VALIDITÉ DE LA MESURE — rejet EXPLICITE, jamais délégué au
  // `isfinite(error)` interne du module pur. Ce dernier ne voit qu'un flottant :
  // une sonde FIGÉE sur une valeur haute rend une erreur parfaitement finie et
  // constante, exactement le cas qui chargerait l'intégrale jusqu'au plafond.
  if (!sensors.isPhFilterReady()) return false;    // warmup, gate fermée, figée, périmée
  if (sensors.isPhFilterUnstable()) return false;  // rejets consécutifs
  if (!isfinite(sensors.getPhFiltered())) return false;

  // 5. CALIBRATION (2 points requis sur le pH) : sans elle l'erreur n'a pas de
  // sens physique, l'intégrer reviendrait à apprendre un décalage d'instrument.
  if (sensors.getPhCalibrationPointsCached() < 2) return false;

  // 6. STABILISATION POST-CALIBRATION. Défense en profondeur : update() a déjà
  // rendu la main dans ce cas (garde globale des deux pompes), mais le prédicat
  // doit rester vrai isolément — c'est ce qui autorise à le relire seul.
  if (isStabilizationTimerActive(0)) return false;

  // NE SONT VOLONTAIREMENT PAS TESTÉS ICI (le tableau du gate) : pause mélange,
  // limite horaire, quota journalier, anti-cycling et plafond de session. Ce sont
  // des suspensions RYTHMIQUES du dosage — l'eau circule, la mesure est valide,
  // la charge du bassin continue d'agir. Le sur-crédit d'une pause mélange est
  // borné à 15 min × 0,15 pH = 135 pH·s, soit 0,8 % du plafond ; et les deux gels
  // d'anti-windup (demande ≥ soutenable, quota à 80 %) restent pleinement actifs.
  return true;
}

// =============================================================================
// feature-085 — DOSE DE BASE APPRISE (feedforward) : la coquille du module pur
// =============================================================================
// Toutes les DÉCISIONS (plafond d'actionneur, quota, historique minimal, latch,
// réarmement) sont dans ph_feedforward_logic.h. Ici : fournir les entrées,
// appliquer le verdict, journaliser les transitions. loopTask uniquement.
float PumpControllerClass::tickPhFeedforward(unsigned long now, float signedError,
                                             float sustainableMlPerMin) {
  // --- Recommandation du gate : TENUE PRODUIT ---------------------------------
  // Détection par front du changement de phCorrectionType. Snapshot local (comme
  // _phCorrectionKind de la surveillance) : on ne relit la String qu'une fois.
  const uint8_t kind = (mqttCfg.phCorrectionType == "ph_plus") ? 1 : 0;
  if (_ffCorrectionKind == 255) {
    _ffCorrectionKind = kind;  // boot : on adopte sans tenir (anneau supposé homogène)
  } else if (kind != _ffCorrectionKind) {
    _ffCorrectionKind = kind;
    _ffProductChangeHold = true;
    _ffProductHoldFreshDay = true;  // la 1re clôture archivera une journée MIXTE
    systemLogger.info("Feedforward pH retenu : changement de TYPE de produit "
                      "(pH- <-> pH+) — dose de base suspendue jusqu'à une journée "
                      "entière archivée sous le nouveau régime");
  }

  // Porte de validité de la MESURE, évaluée ici (caches capteur, aucun I²C) :
  // mêmes critères que le §4 de phIntegralAccumulationAllowed — une sonde figée
  // ou périmée ferme le filtre, une valeur non finie n'est pas une mesure.
  const bool measureValid = sensors.isPhFilterReady() &&
                            !sensors.isPhFilterUnstable() &&
                            isfinite(sensors.getPhFiltered());

  // Session de dosage pH AUTOMATIQUE ouverte — état du tour PRÉCÉDENT (le latch
  // se réarme donc au 2e tick d'une session neuve : un tour de retard, assumé et
  // borné, dans le sens sûr — jamais un FF servi avant l'ouverture réelle).
  const bool sessionOpen = phDosingState.active && _phSessionFromAuto;

  const PhFfVerdict v = phFeedforwardTick(
      _phFf, signedError, measureValid, phIntegralContextHash(),
      phIntegralObservedLoad(), sustainableMlPerMin, sessionOpen, (uint32_t)now,
      safetyLimits.dailyPhInjectedMl, safetyLimits.maxPhMlPerDay,
      _ffProductChangeHold);

  // --- Journalisation par TRANSITION (jamais de spam par tour) ----------------
  if (v.latched != PhFfLatchCause::kNone &&
      v.latched != PhFfLatchCause::kProductChange) {  // la tenue produit a son log dédié
    const char* cause =
        (v.latched == PhFfLatchCause::kAdverseError)
            ? "pH franchement au-delà de la consigne depuis plus de 30 min"
            : (v.latched == PhFfLatchCause::kMeasureInvalid)
                  ? "mesure pH invalide"
                  : "réglages de régulation modifiés";
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Feedforward pH retenu (%s) — dose de base suspendue jusqu'à la "
             "prochaine session",
             cause);
    systemLogger.info(msg);
  }
  if (v.released) {
    systemLogger.info("Feedforward pH réarmé (ouverture de session)");
  }
  // Premier tick ACTIF de la session : trace du débit de base retenu (une seule
  // ligne par session — le log d'arrêt résume déjà les volumes).
  if (v.ffMlPerMin > 0.0f && !_ffAnnounced) {
    _ffAnnounced = true;
    char msg[144];
    snprintf(msg, sizeof(msg),
             "Dosage pH : dose de base apprise %.2f mL/min (charge observée "
             "%.2f mL/min sur %u jour(s), plafond %.2f)",
             v.ffMlPerMin, phIntegralObservedLoad().loadMlPerMin,
             (unsigned)phIntegralObservedLoad().days,
             kPhFfMaxSustainableFraction * sustainableMlPerMin);
    systemLogger.info(msg);
  }
  if (!sessionOpen) _ffAnnounced = false;

  // Instantanés d'observabilité (/data + WS via les accesseurs publics).
  _ffFlowMlPerMin = v.ffMlPerMin;
  _ffStatus = (uint8_t)v.status;
  return v.ffMlPerMin;
}

// UN tick d'entretien. Porte aussi la CONFIRMATION (R4) d'une restauration NVS
// en attente : c'est le seul endroit où l'on dispose à la fois d'une mesure
// validée, de l'erreur signée et d'une base de temps de branche.
void PumpControllerClass::tickPhIntegralAuto(unsigned long now, float error,
                                             float ampFlow, int limitMin,
                                             bool allowed, float ffMlPerMin) {
  if (!allowed) {
    // R4 exige une confirmation CONTINUE : toute interruption la remet à zéro.
    phPersistConfirmInterrupt(_phIntegralConfirm);
    return;
  }

  // --- R4 : CONFIRMATION PAR LA MESURE d'une restauration en attente ---------
  // La valeur relue en NVS n'a jamais été appliquée : elle attend que le bassin
  // confirme lui-même qu'une charge acide est bien présente. Évaluée AVANT le
  // tick, pour disposer de `lastTickMs` d'avant et donc du dt de branche.
  // TOUTE la décision est dans le module pur ; ici, on fournit et on applique.
  // Garde d'ENTRÉE : sans candidate en attente, la machine rendrait `kIdle` — on
  // évite de recalculer le plafond R1 à chaque tour de loop pour rien.
  if (_phIntegralConfirm.candidate > 0.0f) {
    const uint32_t last = _phIntegral.lastTickMs;
    PhPersistConfirmInputs cin;
    cin.epochNow = (uint32_t)time(nullptr);
    cin.dtMs = (last == 0) ? 0u : (uint32_t)((uint32_t)now - last);
    cin.error = error;
    cin.startThreshold = pumpProtection.phStartThreshold;
    // R1 réappliquée à l'INSTANT DE L'APPLICATION : le plafond COURANT fait foi
    // (plafond d'anti-windup et charge observée ont pu changer depuis le boot).
    const PhPersistCapVerdict capNow = phIntegralRestoreCap();
    cin.capPhS = capNow.capPhS;

    const PhPersistConfirmVerdict cv = phPersistConfirmTick(_phIntegralConfirm, cin);
    if (cv.outcome == PhPersistConfirmOutcome::kExpired) {
      systemLogger.warning("Intégrale pH : restauration abandonnée — instantané "
                           "périmé avant confirmation");
    } else if (cv.outcome == PhPersistConfirmOutcome::kNoCap) {
      // Cause DISTINCTE de la péremption : la tenue était acquise, c'est le
      // plafond R1 qui est retombé à zéro entre le boot et la confirmation
      // (historique de charge vidé, ki passé à zéro, changement de vitesse).
      systemLogger.warning("Intégrale pH : restauration abandonnée — plafond de "
                           "restauration retombé à zéro avant confirmation");
    } else if (cv.outcome == PhPersistConfirmOutcome::kApplied) {
      _phIntegral.integral = cv.restored;
      phPID.integral = cv.restored;
      _phIntegralSavedValue = cv.restored;
      // R6 (recommandation du gate) : la CHARGE OBSERVÉE et le plafond retenu
      // sont journalisés — sans eux, on ne saurait pas dire, après coup, si la
      // restauration a été bornée par la butée absolue ou par la chimie du bassin.
      const PhPersistLoadObservation obs = phIntegralObservedLoad();
      char msg[232];
      snprintf(msg, sizeof(msg),
               "Intégrale pH restaurée : %.0f pH·s (%.0f %% du plafond), "
               "confirmée par %lu min d'écart soutenu ; plafond %.0f pH·s, "
               "charge observée %.2f mL/min sur %u jour(s), équilibre %.0f pH·s",
               cv.restored, phIntegralFillPct(cv.restored, phPID.integralMax),
               kPhIntegralRestoreConfirmMs / 60000UL, capNow.capPhS,
               (double)obs.loadMlPerMin, (unsigned)obs.days,
               capNow.equilibriumPhS);
      systemLogger.info(msg);
    }
  }

  PhIntegralInputs in;
  in.nowMs = (uint32_t)now;
  in.error = error;
  in.ampFlow = ampFlow;
  in.budgetMinPerHour = limitMin;
  // C3 (conservé) — gel sur QUOTA JOURNALIER : couvre la sonde bloquée sur une
  // valeur HAUTE (plage et fraîcheur valides, filtre « prêt », erreur constante
  // que rien ne résorbe). Le volume déjà injecté est le seul témoin fiable de
  // cette divergence. Quota nul/absent (limite désactivée) → pas de gel.
  const float dailyLimit = safetyLimits.maxPhMlPerDay;
  in.quotaNearLimit =
      (dailyLimit > 0.0f) &&
      (safetyLimits.dailyPhInjectedMl > kPhIntegralFreezeDailyFraction * dailyLimit);
  // Gate pool-chemistry (30/08/2026) : le quota de CYCLES ne se relève qu'au
  // basculement de 24 h, tout comme le quota volumique — il appartient donc au
  // gel, et non aux suspensions rythmiques. Sans lui, un anti-cycling épuisé en
  // matinée laissait l'intégrale charger jusqu'au soir sur une erreur que
  // l'actionneur n'avait plus le droit de résorber.
  in.cycleQuotaExhausted =
      (pumpProtection.maxCyclesPerDay > 0) &&
      (phDosingState.cyclesToday >= (unsigned int)pumpProtection.maxCyclesPerDay);
  // feature-085 : le soutenable vu par le PI est réduit de la part du FF —
  // dimension traitée DANS le module pur (cf. PhIntegralInputs::ffMlPerMin).
  in.ffMlPerMin = ffMlPerMin;

  PhIntegralTuning tun;
  tun.kp = phPID.kp;
  tun.ki = phPID.ki;
  tun.integralMax = phPID.integralMax;
  tun.startThreshold = pumpProtection.phStartThreshold;
  tun.stopThreshold = pumpProtection.phStopThreshold;

  const PhIntegralVerdict verdict = phIntegralTick(_phIntegral, in, tun);
  phPID.integral = _phIntegral.integral;  // miroir servi par computePidPure

  // feature-084 (R4, gate pool-chemistry du 30/08/2026) — LA PURGE SÈCHE EST UNE
  // RUPTURE DE SENS, au même titre que le changement de vitesse, la bascule de
  // produit et le passage en mode Programmée, qui abandonnent tous la candidate.
  // Elle ne le faisait pas. Séquence atteignable DANS la fenêtre R3 de 6 h :
  // candidate en attente → pluie acide → purge sèche (l'excursion déclare
  // l'accumulation caduque) → le pH remonte dans l'après-midi → la candidate se
  // confirme et RECHARGE l'autorité que la purge venait précisément d'invalider.
  // Une restauration ne doit jamais survivre à la purge de ce qu'elle restaure.
  if (verdict.event == PhIntegralEvent::kDryReset) {
    phPersistConfirmAbandon(_phIntegralConfirm);
  }

  applyPhIntegralVerdict(verdict, error);
}

// =============================================================================
// feature-084 (sujet 3) — PERSISTANCE NVS : la coquille de l'instantané
// =============================================================================
// Le module pur ne sait rien de tout ceci (ni NVS, ni time()) : il ne voit qu'un
// `st.integral` déjà posé, exactement comme après un reset. Les sept garanties
// R1-R7 sont décrites dans constants.h ; on indique ici QUI les applique.

// R2 — EMPREINTE DE CONTEXTE. Une intégrale n'a de sens que pour le quadruplet
// (vitesse de régulation, produit correcteur, consigne, plafond) qui l'a chargée.
// Le calcul lui-même est PUR (phPersistContextHash) : ici on ne fait que réunir
// les quatre grandeurs depuis la configuration courante.
uint32_t PumpControllerClass::phIntegralContextHash() const {
  PhIntegralContext ctx;
  ctx.speedCode = _regulationSpeedCode.load();
  ctx.phPlus = (mqttCfg.phCorrectionType == "ph_plus");
  ctx.target = mqttCfg.phTarget;
  ctx.integralMax = phPID.integralMax;
  return phPersistContextHash(ctx);
}

// -----------------------------------------------------------------------------
// R1 — CHARGE ACIDE OBSERVÉE (v2.27.0 : l'historique existe enfin)
// -----------------------------------------------------------------------------
// Le gate exige « médiane des volumes pH journaliers sur 7 jours ÷ minutes de
// filtration du jour », avec moins de kPhIntegralRestoreMinHistoryDays jours
// exploitables ⇒ AUCUNE restauration. Jusqu'à la v2.26.0 cette donnée n'existait
// nulle part (`pool-daily` n'a que le jour courant, `history.*` ne stocke aucun
// volume) et la fonction rendait `days = 0` : le fail-closed rendait la
// persistance INERTE. Elle est désormais alimentée par l'anneau NVS de 7
// journées (config.cpp / ph_persist_logic.h).
//
// LECTURE MISE EN CACHE, jamais faite ici : phIntegralObservedLoad() est appelée
// à chaque tour d'update() par phIntegralRestoreCap(), et une lecture NVS par
// tour de loop serait inacceptable. Le cache ne bouge que lorsque l'anneau bouge
// — au démarrage et à la clôture d'une journée.
void PumpControllerClass::refreshPhLoadObservation() {
  // Le filet de vraisemblance de l'anneau SUIT les réglages (limite horaire,
  // débit d'amplitude calibré) : un seuil figé écarterait toutes les journées dès
  // que l'utilisateur relève `phInjectionLimitMinutes`, et la restauration
  // mourrait en silence. Source unique partagée avec le prédicat de saturation.
  _phLoadObs = readPhLoadObservation(phSustainableFlowFromConfig());
  // feature-086 : la série de journées saturées est relue au MÊME rythme que la
  // médiane (démarrage différé + clôture de journée) — l'anneau ne bouge qu'à
  // ces instants, et c'est la garantie « jamais de NVS dans update() ».
  _phLimitStreakDays = readPhActuatorLimitStreak(phSustainableFlowFromConfig());
}

PhPersistLoadObservation PumpControllerClass::phIntegralObservedLoad() const {
  // Cache non encore amorcé (avant le premier loadDailyCounters différé) :
  // `days = 0` par défaut, donc fail-closed — jamais de valeur inventée.
  return _phLoadObs;
}

// =============================================================================
// feature-086 — LIMITE D'ACTIONNEUR pH : accesseurs de diagnostic (lecture seule)
// =============================================================================
// AUCUN de ces quatre accesseurs n'est consommé par une décision de dosage : ils
// rendent visible la CONSÉQUENCE de la limite horaire (plafond de débit moyen,
// maximum journalier), ils n'ouvrent aucun chemin nouveau vers les pompes.

float PumpControllerClass::getPhSustainableMlPerMin() const {
  // Recalcul à la demande depuis la source UNIQUE (config.cpp) : une
  // multiplication, pas de cache à invalider quand l'utilisateur change la
  // limite horaire ou recalibre la pompe.
  const float s = phSustainableFlowFromConfig();
  // 0 (pompe non caractérisée) ou valeur non finie ⇒ NAN : le transport /data et
  // WS traduit NAN en null — on affiche l'indisponibilité, on n'invente rien.
  if (!isfinite(s) || s <= 0.0f) return NAN;
  return s;
}

float PumpControllerClass::getPhDailyMaxMl() const {
  const float sustainable = getPhSustainableMlPerMin();
  if (isnan(sustainable)) return NAN;
  // Minutes PROGRAMMÉES du planning de filtration (pas les minutes effectives :
  // le maximum annoncé est celui de la journée telle que planifiée — modes
  // `auto` et `manual`, alignés sur decideFiltrationRun). −1 = mode `off`/
  // `force` ou plage invalide ⇒ indisponible.
  const int scheduledMin = filtration.scheduledMinutesToday();
  if (scheduledMin < 0) return NAN;
  return sustainable * (float)scheduledMin;
}

bool PumpControllerClass::isPhLimitedByHourlyBudget() const {
  // Conjonction VOULUE (spec, livrable A2) : l'épisode de saturation C5 est tenu
  // en temps de branche automatique — sur panne de sonde, la branche ne tourne
  // plus et le drapeau resterait FIGÉ à sa dernière valeur. Exiger une mesure pH
  // valide (getPhFilteredRaw porte la garde d'âge du filtre, NaN si périmée)
  // évite de servir ce verdict gelé : « borné par la limite horaire » doit
  // rester distinct de « panne de sonde » et de « bidon vide ».
  return _phIntegral.satActive && !isnan(sensors.getPhFilteredRaw());
}

bool PumpControllerClass::isPhUndersizedSuspected() const {
  // anomalyNow = épisode C5 OUVERT : `anomalyHeldMs` est remis à zéro par le
  // module pur dès que l'anomalie cesse (demande désaturée OU erreur rentrée
  // sous le seuil d'arrêt) — c'est le prédicat exact, sans double hystérésis.
  //
  // GARDE MESURE VALIDE (condition bloquante n°1 du visa pool-chemistry :
  // l'alerte ne peut PAS s'émettre avec une mesure inexploitable) : la remise à
  // zéro d'`anomalyHeldMs` n'a lieu QUE quand la branche d'entretien tourne avec
  // une erreur exploitable — sur mesure invalide (panne de sonde), la branche
  // s'arrête et la tenue GÈLE à sa dernière valeur > 0. Sans cette conjonction,
  // une montée du streak 2→3 à la clôture de journée pendant une panne de sonde
  // émettrait l'alerte sur un épisode d'anomalie FIGÉ. Prédicat STRICTEMENT
  // identique à celui d'isPhLimitedByHourlyBudget (pas une variante) :
  // getPhFilteredRaw porte la garde d'âge du filtre, NaN si périmée.
  return phActuatorUndersized(_phLimitStreakDays,
                              _phIntegral.anomalyHeldMs > 0) &&
         !isnan(sensors.getPhFilteredRaw());
}

// Journalisation PAR TRANSITION de l'alerte de sous-dimensionnement (montée ET
// retombée). Un tour d'update() coûte ici deux lectures et une comparaison.
void PumpControllerClass::tickPhUndersizedAlert() {
  // MÊME verdict que le champ publié (/data + WS) : le log passe par l'accesseur
  // lui-même — garde mesure-valide comprise — pour que journal et champ ne
  // puissent pas diverger. Une panne de sonde fait donc aussi RETOMBER l'alerte
  // (fail-closed, condition n°1 du visa) ; elle se réarmera d'elle-même si la
  // conjonction redevient vraie au retour de la mesure.
  const bool active = isPhUndersizedSuspected();
  if (active == _phUndersizedActive) return;
  _phUndersizedActive = active;
  if (active) {
    // Diagnostic d'installation, pas anomalie de fonctionnement (spec A3) :
    // warning — visible à distance, sans le côté « incident » d'un critical.
    char msg[240];
    snprintf(msg, sizeof(msg),
             "[Dosage pH] Sous-dimensionnement suspecté : %u jour(s) "
             "consécutif(s) au maximum délivrable (débit soutenable "
             "%.2f mL/min) et pH toujours au-dessus de la consigne — la limite "
             "horaire d'injection borne le débit sous le besoin du bassin",
             (unsigned)_phLimitStreakDays,
             (double)phSustainableFlowFromConfig());
    systemLogger.warning(msg);
  } else {
    systemLogger.info("[Dosage pH] Suspicion de sous-dimensionnement levée : "
                      "la série de journées saturées est interrompue ou "
                      "l'erreur pH est rentrée (épisode de saturation clos)");
  }
}

// -----------------------------------------------------------------------------
// R1 — DÉNOMINATEUR : minutes de circulation EFFECTIVES de la journée
// -----------------------------------------------------------------------------
// La charge se mesure en mL/min, pas en mL/jour : sans ce compteur, une journée à
// 4 h et une journée à 10 h de filtration seraient jugées identiques à volume
// égal. La source est le CACHE de présence d'eau (measureValidity), lui-même
// alimenté par resolveWaterPresence() — donc valable dans les trois modes
// d'installation (Managed, Powered, External), et pas seulement quand ce
// contrôleur pilote lui-même le relais.
//
// Appelé hors de toute conjonction d'éligibilité : compter la circulation n'est
// pas décider de doser. Les dt aberrants ne sont pas crédités, mais le seuil est
// CELUI DU COMPTAGE (kPhLoadMinuteMaxTickDtMs, 60 s) et non celui de l'intégrale
// (10 s) : ici, un dt écarté TRONQUE le dénominateur alors que le numérateur,
// lui, est crédité par accountPumpVolume sur le delta ENTIER — et un
// dénominateur tronqué SURÉVALUE la charge mL/min, donc le plafond R1. Le sens
// du fail-closed est inversé par rapport à la charge de l'intégrale ; le seuil
// l'est donc aussi. Le plafond de 1440 min/jour reste le garde-fou d'ensemble.
void PumpControllerClass::tickDailyFiltrationMinutes(unsigned long now) {
  const uint32_t last = _waterMinuteTickMs;
  _waterMinuteTickMs = (uint32_t)now;
  if (last == 0) return;  // premier tour : pas de dt de référence
  const uint32_t dt = (uint32_t)now - last;  // non signé ⇒ wrap-safe
  if (dt == 0 || dt > kPhLoadMinuteMaxTickDtMs) return;
  if (!measureValidity.waterPresent()) return;

  _waterMinuteAccumMs += dt;
  while (_waterMinuteAccumMs >= 60000UL) {
    _waterMinuteAccumMs -= 60000UL;
    if (safetyLimits.dailyFiltrationMinutes < 1440UL) {
      safetyLimits.dailyFiltrationMinutes++;
    }
    // Persistance différée par le point de flush EXISTANT (30 s), remonté AVANT
    // les sorties anticipées d'update() : aucune écriture NVS supplémentaire
    // n'est déclenchée ici, et un redémarrage ne peut perdre qu'une minute.
    _dailyCountersDirty = true;
  }
}

// R1 — plafond COURANT : min(butée absolue, moitié de l'équilibre observé). Le
// calcul est PUR ; la coquille ne fait que présenter les réglages du moment.
PhPersistCapVerdict PumpControllerClass::phIntegralRestoreCap() const {
  return phPersistRestoreCap(phPID.integralMax, phPID.kp, phPID.ki,
                             pumpProtection.phStopThreshold,
                             phIntegralObservedLoad());
}

// Lecture DIFFÉRÉE de l'instantané. Une seule tentative aboutie par boot ; on
// attend une heure valide (R3 : un âge incalculable vaut un âge périmé) puis on
// renonce définitivement après kPhIntegralRestoreArmWindowMs — sans quoi une
// installation hors ligne relirait la NVS à chaque tour de loop.
void PumpControllerClass::loadPhIntegralSnapshotDeferred(unsigned long now) {
  if (_phIntegralPersistLoaded) return;

  const time_t epochNow = time(nullptr);
  if (epochNow < kMinValidEpoch) {
    if (now < kPhIntegralRestoreArmWindowMs) return;  // laisser NTP/RTC arriver
    _phIntegralPersistLoaded = true;
    systemLogger.warning("Intégrale pH : restauration abandonnée — heure non "
                         "synchronisée (âge de l'instantané incalculable)");
    return;
  }
  _phIntegralPersistLoaded = true;

  PhIntegralSnapshot snap;
  if (!loadPhIntegralSnapshot(snap)) return;  // aucun instantané valide : silence

  // v2.27.0 (revue) : le cache de charge a pu être amorcé AVANT que l'heure ne
  // soit synchronisée, et la fenêtre d'âge de l'anneau exige la date du jour —
  // sans elle, aucun échantillon n'est retenu. On le rafraîchit ICI, au seul
  // instant du boot où l'horloge est prouvée valide. UNE lecture NVS par
  // démarrage, jamais dans la boucle.
  refreshPhLoadObservation();

  // TOUTES les règles (R1, R2, R3, R7) sont appliquées par le module PUR, dans
  // un ordre fixé et testable. La coquille n'ajoute AUCUN critère : en ajouter un
  // ici le rendrait invisible aux tests, ce qui est exactement le défaut que
  // l'extraction corrige.
  const PhPersistCapVerdict cap = phIntegralRestoreCap();
  const PhPersistRestoreVerdict v =
      phPersistEvaluateRestore(snap, phIntegralContextHash(), (uint32_t)epochNow,
                               cap.capPhS);

  if (!v.accepted) {
    // R6 — au rejet, le MOTIF est journalisé. Un rejet muet rendrait la
    // persistance indébogable : on ne saurait pas distinguer « rien à restaurer »
    // de « restauration refusée », précisément l'ambiguïté qui ferait douter du
    // mécanisme au premier redémarrage inexpliqué.
    const char* reject = phPersistRejectLabel(v.reject);
    if (reject != nullptr) {
      char msg[176];
      snprintf(msg, sizeof(msg),
               "Intégrale pH : instantané NVS REJETÉ (%s) — la régulation repart "
               "de zéro", reject);
      systemLogger.warning(msg);
    } else if (v.reject == PhPersistReject::kNoCap) {
      // Plafond NUL : état nominal d'un appareil dont la charge acide n'est pas
      // encore observée (moins de 3 jours d'historique) — `info`, pas `warning`.
      char msg[208];
      snprintf(msg, sizeof(msg),
               "Intégrale pH : instantané NVS non restauré — plafond de "
               "restauration nul (%u jour(s) d'historique de charge sur %u "
               "requis) ; la régulation repart de zéro",
               (unsigned)phIntegralObservedLoad().days,
               (unsigned)kPhIntegralRestoreMinHistoryDays);
      systemLogger.info(msg);
    }
    return;
  }

  // R7 — l'ESSAI est tracé en NVS AVANT toute confirmation : c'est le nombre de
  // démarrages qui tentent une restauration qu'il faut borner, pas le nombre de
  // restaurations effectivement appliquées (une boucle de reboot ne laisserait
  // jamais le temps d'en confirmer une seule). Incrément SATURÉ (module pur).
  snap.restoreStreak = phPersistNextStreak(snap.restoreStreak);
  savePhIntegralSnapshot(snap);

  _phIntegralConfirm.candidate = v.candidate;
  _phIntegralConfirm.epoch = snap.epoch;
  _phIntegralConfirm.heldMs = 0;
  // La valeur de référence de la cadence d'écriture est celle du DISQUE : sans
  // cela, la première comparaison R5 se ferait contre 0 et réécrirait aussitôt.
  _phIntegralSavedValue = snap.value;
  _phIntegralSaveMs = now;

  // R6 (recommandation du gate) : la charge observée et l'origine du plafond
  // figurent dans le message — c'est la seule trace qui permettra, après coup, de
  // savoir sur quelle chimie la restauration a été bornée.
  char msg[240];
  snprintf(msg, sizeof(msg),
           "Intégrale pH : instantané NVS retenu — %.0f pH·s écrits il y a "
           "%lu min, plafonnés à %.0f pH·s par %s ; application DIFFÉRÉE "
           "jusqu'à confirmation par la mesure (%lu min d'écart soutenu)",
           snap.value, (unsigned long)(v.ageS / 60UL), v.candidate,
           cap.loadGoverns ? "la charge observée" : "la butée absolue",
           kPhIntegralRestoreConfirmMs / 60000UL);
  systemLogger.info(msg);
}

// R5 — ÉCRITURE BORNÉE. Appelée au point de sauvegarde différée de fin
// d'update() (loopTask), à côté du flush des compteurs journaliers : au plus une
// écriture par demi-heure ET seulement si la valeur a bougé de plus de 5 % du
// plafond. Aucun mutex : la charge utile appartient à loopTask seule
// (cf. l'en-tête de savePhIntegralSnapshot dans config.h).
void PumpControllerClass::tickPhIntegralPersistence(unsigned long now) {
  if (!_phIntegralPersistLoaded) return;  // lecture pas encore soldée

  // La DÉCISION d'écrire (cadence, amplitude, heure exploitable) est PURE : la
  // coquille ne fait que présenter l'état courant et exécuter le verdict.
  const time_t epochNow = time(nullptr);
  const float value = _phIntegral.integral;

  PhPersistWriteInputs win;
  win.nowMs = (uint32_t)now;
  win.lastSaveMs = _phIntegralSaveMs;
  win.epochNow = (uint32_t)epochNow;
  win.value = value;
  win.savedValue = _phIntegralSavedValue;
  win.integralMax = phPID.integralMax;
  if (!phPersistShouldWrite(win)) return;

  PhIntegralSnapshot snap;
  snap.schema = kPhIntegralPersistSchema;
  snap.restoreStreak = 0;  // R7 : une écriture NORMALE réarme la persistance
  snap.context = phIntegralContextHash();
  snap.epoch = (uint32_t)epochNow;
  snap.value = value;
  savePhIntegralSnapshot(snap);

  _phIntegralSavedValue = value;
  _phIntegralSaveMs = now;
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
// Débit majorant à 255 : constante de FLOTTE unique depuis v2.25.2
// (kPumpFlow255MlPerMin) — le dispatch par index physique a été supprimé, il
// figeait la mesure d'un exemplaire sur un numéro de pompe.

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
      // v2.27.0 (feature-084, R1) : ARCHIVAGE de la journée qui se termine dans
      // l'anneau de charge acide — UNE écriture NVS par jour, exactement au point
      // où les compteurs journaliers sont déjà remis à zéro (aucun second point
      // de basculement n'est créé). AVANT la remise à zéro, évidemment.
      recordPhLoadDay(safetyLimits.currentDayDate, safetyLimits.dailyPhInjectedMl,
                      safetyLimits.dailyFiltrationMinutes,
                      phSustainableFlowFromConfig());
      refreshPhLoadObservation();  // le plafond R1 suit la nouvelle médiane
      // feature-085 — TENUE PRODUIT du feedforward, levée en DEUX temps : la
      // journée qui vient d'être archivée est MIXTE si le changement de produit
      // a eu lieu en cours de journée (_ffProductHoldFreshDay la saute) ; la
      // clôture SUIVANTE archive une journée entière sous le nouveau régime et
      // lève la tenue. Chemin DATE uniquement : le rollover fallback 24 h
      // n'archive rien, il ne compte donc pas comme journée archivée.
      if (_ffProductChangeHold) {
        if (_ffProductHoldFreshDay) {
          _ffProductHoldFreshDay = false;  // journée mixte : on attend la suivante
        } else {
          _ffProductChangeHold = false;
          systemLogger.info("Feedforward pH : tenue de changement de produit "
                            "levée (une journée entière archivée sous le "
                            "nouveau régime) — réarmement à la prochaine session");
        }
      }
      resetScheduledSplit();  // feature-011 : reliquat scheduled + réarmement fenêtres
      safetyLimits.dailyPhInjectedMl  = 0;
      safetyLimits.dailyOrpInjectedMl = 0;
      safetyLimits.dailyFiltrationMinutes = 0;
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
      // v2.27.0 : AUCUN archivage sur ce chemin — sans horloge valide la journée
      // n'est pas datable, donc pas exploitable. Le compteur est remis à zéro
      // comme les volumes, pour ne pas cumuler deux « jours » dans le même
      // dénominateur (fail-closed : l'anneau reste plus court, jamais plus faux).
      safetyLimits.dailyFiltrationMinutes = 0;
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
                      : kPumpFlow255MlPerMin;  // sinon MAJORANT (fail-safe)
        volMl = f * (float)deltaMs / 60000.0f;
      } else {
        volMl = pulseDeltaVolumeMl(_pulseState[chem].pulseStartMs, kPulseKickMs,
                                   (uint32_t)st.lastSafetyTimestamp, (uint32_t)now,
                                   kPumpFlow255MlPerMin, pumpPulseFlow(physIdx));
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
    // v2.26.0 (revue, bloquant n°2) : une réinitialisation demandée alors qu'une
    // session pH est OUVERTE la refermait en silence (`phDosingState = {}`), sans
    // armer la pause mélange — le produit déjà injecté n'avait donc jamais son
    // temps d'homogénéisation. Fermeture rituelle AVANT l'effacement.
    closePhDosingSession(now, "réinitialisation des états de dosage");
    phDosingState = {};
    orpDosingState = {};
    phPID = {};
    orpPID = {};
    // v2.26.0 : `= {}` restaure les DÉFAUTS DE STRUCTURE (Kp=8, Ki=0,
    // integralMax=50 — et Kp=8 aussi pour l'ORP, dont le 0,3 vient du
    // constructeur). Sans ré-application, une réinitialisation des états de
    // dosage désarmerait SILENCIEUSEMENT le terme intégral pH (Ki=0, plafond
    // inerte) et laisserait l'ORP avec un gain 26× trop agressif jusqu'au
    // prochain enregistrement de la vitesse de régulation. Les coefficients sont
    // donc reposés depuis la configuration, seule source de vérité.
    phIntegralReset(_phIntegral);
    // Application DIRECTE : on est déjà sur loopTask, et le code de vitesse a été
    // photographié au dernier enregistrement de configuration (jamais la String).
    applyRegulationSpeedNow();
  }
  // v2.26.0 (revue, bloquant n°1) : changement de vitesse de régulation demandé
  // depuis la tâche web. L'écriture des coefficients ET la purge de l'intégrale
  // ont lieu ICI, sur loopTask — donc jamais entrelacées avec phIntegralTick().
  if (_phTuningChangeRequested.exchange(false)) {
    applyRegulationSpeedNow();
  }
  if (_phPauseResetRequested.exchange(false)) {
    // Purge OBLIGATOIRE et conservée (verdict du gate v2.26.0) : ce reset est
    // demandé au changement ph_minus ↔ ph_plus (web_routes_config.cpp), qui
    // INVERSE le signe de l'erreur. Une intégrale chargée pour acidifier
    // deviendrait une demande d'alcaliniser de même amplitude.
    phDosingState.lastStopTime = 0;
    phPID.integral = 0.0f;
    phPID.lastError = 0.0f;
    phPID.lastTime = 0;
    phIntegralReset(_phIntegral);
    // feature-084 (R2) : le changement de produit INVERSE le signe de l'erreur —
    // une restauration en attente, chargée pour acidifier, deviendrait une
    // demande d'alcaliniser de même amplitude. Invalidée avec l'intégrale.
    phPersistConfirmAbandon(_phIntegralConfirm);
  }

  // Chargement différé des compteurs journaliers (attend que NTP/RTC soit synchronisé)
  if (!_dailyLoaded) {
    loadDailyCounters();
    _dailyLoaded = true;
    // v2.27.0 (R1) : amorçage du cache de charge observée, APRÈS loadDailyCounters
    // — celui-ci a pu clôturer la veille et donc modifier l'anneau.
    refreshPhLoadObservation();
  }

  // Reset journalier — appelé AVANT canDose() pour que les compteurs se réinitialisent
  // à minuit même quand la filtration est arrêtée.
  tickDailyRollover();

  // v2.27.0 (R1) : dénominateur de la charge observée. APRÈS le rollover, pour que
  // les minutes archivées soient bien celles de la journée close et que le résidu
  // de la minute en cours soit crédité au nouveau jour.
  tickDailyFiltrationMinutes(now);

  // Flush périodique des compteurs journaliers, toutes les 30 s si quelque chose
  // a changé.
  //
  // PLACÉ ICI, EN AMONT DE TOUTES LES SORTIES ANTICIPÉES (v2.27.0, revue). Il
  // vivait en fin d'update(), derrière quatre `return` : OTA, mode maintenance,
  // capteurs non initialisés, stabilisation / assistant de débit. Pendant ces
  // épisodes l'eau CIRCULE et les minutes s'accumulent (tickDailyFiltrationMinutes
  // est juste au-dessus, hors de toute conjonction d'éligibilité), mais
  // `_dailyCountersDirty` n'était jamais consommé — et l'OTA comme le mode
  // maintenance se terminent par un REDÉMARRAGE. Les minutes étaient donc perdues
  // alors que le volume, lui, ne bougeait pas : une journée de 480 min de
  // circulation dont 360 en maintenance et 120 en régulation à 5 mL était relue
  // 5/120 = 0,042 mL/min au lieu de 0,0104, soit une charge observée QUATRE FOIS
  // trop grande — donc un plafond de restauration R1 quatre fois trop généreux.
  //
  // Le bloc ne dépend que de `_dailyCountersDirty` et `_lastDailySaveMs` : aucune
  // dépendance à phActive/orpActive, contrairement aux deux flushs de TRANSITION
  // (démarrage d'injection pH / ORP) qui, eux, restent en fin d'update().
  if (_dailyCountersDirty && (now - _lastDailySaveMs >= 30000UL)) {
    saveDailyCounters();
    _dailyCountersDirty = false;
    _lastDailySaveMs = now;
  }

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
    // v2.26.0 (revue, bloquant n°2) : setOtaInProgress() coupe les pompes depuis
    // le contexte où il est appelé (souvent async) mais ne referme PLUS la session
    // lui-même. Le rituel complet (état, pause mélange, log d'arrêt) a lieu ICI,
    // sur loopTask, au premier tour d'OTA. Idempotent : no-op aux tours suivants.
    closePhDosingSession(now, "OTA en cours");
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    return;
  }

  // Auto-guérison v2.9.1 (condition pool-chemistry C2) : mode maintenance →
  // dosage entièrement suspendu (auto, manuel ET scheduled), pompes forcées à 0
  // à chaque cycle. Gate identique à OTA, placée AVANT toute évaluation de dosage.
  if (maintenanceInhibit) {
    // v2.26.0 (revue, bloquant n°2) : cette sortie ne touchait PAS l'état de
    // session — une session pH ouverte au moment de la bascule en maintenance
    // restait « active » jusqu'au reboot, sans pause mélange, sans log d'arrêt, et
    // `isPhDosing()` mentait à l'UI. La fermeture est idempotente : elle ne
    // journalise qu'au premier tour de maintenance.
    closePhDosingSession(now, "mode maintenance");
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    return;
  }

  // ===== v2.26.0 : hygiène (C4) du terme intégral pH ==========================
  // Point d'appel choisi pour n'être sous AUCUNE conjonction d'éligibilité : ni le
  // mode de régulation, ni les limites, ni canDose() ne peuvent le court-circuiter
  // — une intégrale chargée doit être purgée même quand la régulation est en
  // `manual` ou qu'une garde bloque, c'est-à-dire dans les états où elle est
  // justement laissée à l'abandon.
  // Placé APRÈS les sorties OTA et mode maintenance (aucun dosage possible, et un
  // flash ne doit pas être perturbé), mais AVANT les sorties « capteurs non
  // initialisés » / « eau absente » : c'est précisément une filtration coupée
  // durablement qui rend la péremption nécessaire.
  //
  // La SURVEILLANCE de saturation (C5), elle, a migré DANS la branche automatique
  // : son prédicat inclut désormais l'erreur (« demande son maximum SANS atteindre
  // la consigne »), grandeur qui n'existe que là où la mesure est validée. Rien
  // n'est perdu au passage — la tenue de l'anomalie se compte en temps de branche
  // automatique, si bien qu'un séjour en `manual` SUSPEND l'épisode au lieu de le
  // faire mûrir sur une erreur périmée (ou de crier au loup sur un appareil qui,
  // en `manual`, ne régule tout simplement pas).
  tickPhIntegralExpiry(now);

  // feature-086 : franchissements de l'alerte de sous-dimensionnement (montée ET
  // retombée), journalisés par TRANSITION. Placé au même point que la péremption
  // C4, hors de toute conjonction d'éligibilité : la RETOMBÉE doit être vue même
  // quand la branche automatique n'est plus atteinte (c'est la purge C4
  // elle-même qui referme l'épisode C5 après un hiver sans filtration).
  tickPhUndersizedAlert();

  // feature-084 (sujet 3) : lecture DIFFÉRÉE de l'instantané NVS. Placée ici, en
  // aval des sorties OTA et maintenance (une lecture NVS ne doit pas s'inviter au
  // milieu d'un flash) mais en amont de la garde de présence d'eau : une candidate
  // peut être RETENUE filtration coupée — elle ne sera de toute façon APPLIQUÉE
  // qu'après confirmation par la mesure (R4), qui exige la circulation.
  loadPhIntegralSnapshotDeferred(now);

  // feature-084 (R4, gate pool-chemistry du 30/08/2026) : le mode `manual` n'est
  // pas une suspension rythmique mais une RUPTURE DE SENS — l'opérateur dose à la
  // main, hors de toute mesure et de toute conduite automatique. Une candidate
  // NVS en attente a été validée contre une conduite qui n'existe plus : elle est
  // ABANDONNÉE, non simplement interrompue (une interruption la laisserait se
  // confirmer telle quelle au retour en automatique). Même traitement que le mode
  // Programmée. Placé ICI, EN AMONT de la garde de présence d'eau : une session
  // manuelle passée filtration coupée ne doit pas non plus lui laisser la vie
  // sauve.
  if (_phIntegralConfirm.candidate > 0.0f &&
      mqttCfg.phRegulationMode == "manual") {
    phPersistConfirmAbandon(_phIntegralConfirm);
    systemLogger.info("Intégrale pH : restauration en attente abandonnée — "
                      "régulation pH en mode manuel");
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
    // v2.26.0 (revue, bloquant n°2) : `sensors.isInitialized()` N'EST PAS MONOTONE
    // — il vaut `_ezoEverResponded && (!isnan(_lastPh) || !isnan(_lastOrp))`. Une
    // perte I²C des DEUX EZO en cours de session la refermait donc ici en silence,
    // sans pause mélange ni trace : le dosage pouvait reprendre dès le retour des
    // sondes, sur une eau non homogénéisée. C'est la condition n°1 du gate, laissée
    // ouverte sur ce chemin. La fermeture rituelle (pause mélange + log) est
    // idempotente : un seul log par épisode.
    closePhDosingSession(now, "capteurs non initialisés (EZO injoignables)");
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
      // v2.26.0 : cette suspension FERME la session pH — le produit déjà injecté
      // n'a pas été homogénéisé, la pause mélange doit donc être armée ici aussi.
      // Armée directement (pas via closePhDosingSession) pour ne pas doubler le
      // log de suspension ci-dessus, qui couvre déjà les deux chimies.
      // ORP inchangé : hors périmètre du gate v2.26.0 (bande morte pH).
      if (phDosingState.active) notifyPhDose((uint32_t)now);
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

  // ===== SURVEILLANCE des gardes pH / ORP (contre-visite C1) =================
  // Point d'appel choisi pour n'être sous AUCUN court-circuit : il précède
  // TOUTES les conjonctions d'éligibilité (mode de régulation, limite horaire,
  // limite journalière, canDose) et n'est lui-même gardé par aucune condition
  // dans ce bloc. Un pH qui s'effondre produit donc son warn 7,00, son critical
  // 6,70 et ses alertes MQTT même quand le quota journalier est épuisé, même en
  // mode `manual` — c'est-à-dire dans l'état exact où l'escalade laisse
  // l'appareil, et où l'ancienne implémentation devenait muette.
  //
  // Les seules sorties d'update() en amont (OTA, mode maintenance, capteurs non
  // initialisés, eau absente, stabilisation post-cal, assistant de débit) sont
  // volontairement conservées : la sonde y est hors circulation ou la mesure non
  // représentative — surveiller là produirait de FAUSSES alertes, et l'appareil
  // n'y dose de toute façon rien. L'ordre surveillance → décision est garanti
  // dans le même tick, aucune décision n'est prise sur un verdict périmé.
  //
  // Ces deux appels ne comptent RIEN et n'escaladent JAMAIS (cf. leur en-tête) :
  // le compteur des 3 refus / 24 h reste alimenté par les seuls refus RÉELS de la
  // branche automatique, via isPhGuardBlocking(true).
  tickPhGuardSurveillance();
  tickOrpPhInhibitSurveillance();

  // ===== feature-084 (sujet 1) : ENTRETIEN DU TERME INTÉGRAL pH ==============
  // Le tick vivait DANS la conjonction d'éligibilité de la branche automatique,
  // donc sous `canDose(0)` : il ne tournait que ~25 % du temps de filtration —
  // ni pendant les pauses de mélange, ni pendant les suspensions de limite
  // horaire, alors que dans ces états l'eau circule, la mesure est valide et la
  // charge du bassin continue d'agir. `Ti` = 6 h de branche valait ~24 h réelles
  // et la convergence demandait 2,4 jours.
  //
  // Il rejoint donc tickPhGuardSurveillance() ci-dessus, hors de toute
  // conjonction : ENTRETENIR l'intégrale n'est pas DÉCIDER de doser — la sortie
  // reste intégralement gatée par `shouldDose` et par la bande morte de
  // computePidPure, plus bas. Le prédicat exact est dans
  // phIntegralAccumulationAllowed() (pump_controller.h pour le tableau du gate).
  //
  // ORDRE IMPÉRATIF : APRÈS tickPhGuardSurveillance(), dont le prédicat consomme
  // le verdict `_phGuardBlockingNow` du tick courant, et APRÈS la garde de
  // présence d'eau (qui a rendu la main plus haut si la filtration est coupée).
  // TÂCHE IMPÉRATIVE : loopTask — `_phIntegral` n'a pas d'autre écrivain.
  //
  // L'ERREUR SIGNÉE est calculée ICI, hors branche : elle n'a donc plus la
  // garantie de finitude que `canDose(0)` apportait en amont. C'est le prédicat
  // qui la porte désormais, par un rejet EXPLICITE de la porte de validité (§4
  // de phIntegralAccumulationAllowed). `phAmpFlow` et `phLimitMin` sont remontés
  // au-dessus de ce point pour la même raison : sans eux, le gel d'anti-windup
  // sur débit soutenable deviendrait aveugle (fail-closed → gel permanent).
  const int phPumpIdx = pumpIndexFromNumber(mqttCfg.phPump);
  const float phAmpFlow = pumpPulseFlow(phPumpIdx);  // débit MESURÉ à l'amplitude
  // pH- (acide) : dose quand pH > cible → error = pH - cible (positive si pH trop haut)
  // pH+ (base)  : dose quand pH < cible → error = cible - pH (positive si pH trop bas)
  const float phMeasured = sensors.getPhFiltered();
  const float phSignedError = (mqttCfg.phCorrectionType == "ph_plus")
                                  ? (mqttCfg.phTarget - phMeasured)
                                  : (phMeasured - mqttCfg.phTarget);
  // ===== feature-085 : DOSE DE BASE APPRISE (feedforward) ====================
  // Évalué AVANT le tick intégral — ORDRE IMPÉRATIF (condition n°4 du visa) :
  // l'anti-windup du PI retranche le débit FF de son soutenable dans le MÊME
  // tour ; l'inverser ferait geler l'intégrale sur la marge du tour précédent.
  // Le débit rendu n'est PAS une décision de dosage : il n'est consommé que par
  // la branche automatique sous `shouldDose`, via le chemin existant (un seul
  // chemin vers la pompe, une seule comptabilité — condition n°1 du gate).
  const float phFfFlow = tickPhFeedforward(
      now, phSignedError, phSustainableAvgFlow(phAmpFlow, phLimitMin));
  tickPhIntegralAuto(now, phSignedError, phAmpFlow, phLimitMin,
                     phIntegralAccumulationAllowed(), phFfFlow);

  // Contrôle pH — branche automatique gardée en profondeur par canDose(0).
  // canDose(0) vérifie watchdog, filtration, stale/NaN, calibration, stabilisation,
  // mode automatic, limites journalière/horaire, anti-cycling.
  // pool-chemistry #2 : isPhGuardBlocking() est évalué AVANT canDose(0) — quand
  // une garde bloque, aucune évaluation de dosage n'a lieu et la cause de refus
  // exposée au WS est celle de la garde. `true` = les refus de CETTE branche
  // (automatique) alimentent le compteur des 3 refus / 24 h et peuvent escalader.
  // Contre-visite C1 : cet appel ne DÉTECTE plus rien (la surveillance ci-dessus
  // l'a fait, hors court-circuit) — il ne fait que consommer le verdict du tick
  // et comptabiliser le refus RÉEL de cette branche.
  const String& phMode = mqttCfg.phRegulationMode;
  // v2.26.0 — FERMETURE DE SESSION HORS BRANCHE. Ces deux témoins servent au bloc
  // de garde placé en fin d'update() : `phWasActiveAtEntry` retient qu'une session
  // pH était ouverte AVANT l'évaluation de ce tour, `phSessionClosed` dit si la
  // branche automatique s'est chargée de la refermer proprement. Quand la branche
  // est court-circuitée (limite horaire, quota journalier, garde pH, canDose), la
  // session se refermait silencieusement — sans pause mélange ni log.
  const bool phWasActiveAtEntry = phDosingState.active;
  bool phSessionClosed = false;
  if (phMode == "automatic" && phLimitOk && phSafetyOk && !isPhGuardBlocking(true) && canDose(0)) {
    // feature-025 : le PID auto consomme la mesure FILTRÉE (médiane + EMA).
    // canDose(0) a déjà garanti isPhFilterReady() && !isPhFilterUnstable() → non NaN.
    // feature-084 : l'erreur signée est calculée UNE SEULE FOIS par tour, en
    // amont de cette branche (le terme intégral en a besoin hors conjonction).
    // On reprend ici EXACTEMENT la même valeur — deux formules pour une seule
    // grandeur finiraient par diverger.
    const float error = phSignedError;

    // Déterminer si on doit doser (avec protection anti-cycling)
    bool shouldDose = false;
    // bug-dosage-fantome (défaut n°2) : la DÉCISION reste ici, la
    // COMPTABILISATION du cycle est différée après le calcul du débit (bloc
    // « comptabilisation différée » plus bas).
    const bool phWasActiveTick = phDosingState.active;

    // v2.26.0 — PLAFOND DE SESSION (condition bloquante n°1 du gate). Motif de
    // fermeture, mémorisé pour le log d'arrêt ; nullptr = pas de plafonnement.
    const char* phCapReason = nullptr;

    if (phWasActiveTick) {
      // Déjà en cours : vérifier si on continue. Itération 2 : la garde de
      // temps minimum raisonne en temps POMPE-ON cumulé de la session.
      shouldDose = shouldContinueDosing(error, pumpProtection.phStopThreshold, _sessionOnMs[0]);

      // ===== PLAFOND DE SESSION — restaure la pause mélange =====================
      // LE DÉFAUT REFERMÉ ICI. Depuis l'alignement de la bande morte sur
      // l'hystérésis (bande = phStopThreshold session ouverte), le PI se parque
      // exactement au point de coupure de l'hystérésis : l'erreur ne repasse plus
      // franchement sous phStopThreshold, shouldContinueDosing répond « oui »
      // indéfiniment, la session ne se referme JAMAIS. Conséquences mesurées :
      // notifyPhDose() n'est plus jamais appelé — la pause mélange CESSE
      // D'EXISTER —, aucun log d'arrêt n'est produit, et le compteur de cycles
      // tombe à ~1/jour. On dose alors en continu sur une eau dont
      // l'homogénéisation n'est plus jamais attendue.
      //
      // On ne revient PAS sur l'alignement (sans lui le point fixe de
      // l'intégration signée est inatteignable et l'intégrale remonte à 100 % du
      // plafond) : on borne la session, ce qui rend l'armement de la pause
      // GARANTI et périodique. Deux critères — cf. constants.h pour le
      // dimensionnement :
      //   - pompe-ON cumulé : borne le VOLUME injecté entre deux homogénéisations
      //     (critère chimique, se déclenche d'autant plus vite qu'on corrige fort) ;
      //   - durée murale : borne la PÉREMPTION de l'hypothèse d'homogénéité, y
      //     compris en régime établi où la pompe ne tourne que ~10 % du temps.
      // Le plafond est évalué APRÈS shouldContinueDosing et le RECOUVRE : il prime
      // donc sur le forçage de temps minimum d'injection — sans conflit possible,
      // le plafond valant deux ordres de grandeur de plus (static_assert dédié).
      if (shouldDose) {
        const uint32_t sessionWallMs = (uint32_t)now - _phSessionStartMs;
        if (_sessionOnMs[0] >= kPhSessionMaxOnMs) {
          phCapReason = "plafond de session (pompe-ON) — pause mélange";
        } else if (_phSessionStartMs != 0 && sessionWallMs >= kPhSessionMaxWallMs) {
          phCapReason = "plafond de session (durée) — pause mélange";
        }
        if (phCapReason != nullptr) shouldDose = false;
      }
    } else {
      // Arrêté : vérifier si on démarre
      shouldDose = shouldStartDosing(error, pumpProtection.phStartThreshold, phDosingState, now);
    }
    const bool phStartDecided = !phWasActiveTick && shouldDose;

    // --- Itération 2 (option B+C) : caractérisation de la pompe pH affectée ---
    // feature-084 : `phPumpIdx` et `phAmpFlow` sont remontés au-dessus de la
    // branche (le terme intégral en a besoin pour son gel d'anti-windup).
    const uint8_t phAmpDuty = getPumpAmplitudeDuty(phPumpIdx);
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

    // ===== feature-084 — LE TICK INTÉGRAL N'EST PLUS ICI ======================
    // Il a migré HORS de cette conjonction, juste après tickPhGuardSurveillance()
    // (cf. le bloc « ENTRETIEN DU TERME INTÉGRAL pH » plus haut). Motif : gardé
    // par `canDose(0)`, il ne tournait que ~25 % du temps de filtration — muet
    // pendant les pauses de mélange et les suspensions de limite horaire, où la
    // charge du bassin continue pourtant d'agir sur la mesure.
    //
    // NE PAS LE RÉINTRODUIRE ICI : il doit rester UN SEUL point d'évaluation par
    // tour. Deux appels compteraient le même dt deux fois — et la seconde charge
    // se ferait, elle, sous la conjonction, donc de façon intermittente.
    //
    // La SORTIE reste gatée exactement comme avant, par `shouldDose` et par la
    // bande morte de computePidPure : entretenir l'intégrale n'autorise aucune
    // injection.

    float flow = 0.0f;  // demande de débit MOYEN (mL/min), réalisée par le pouls
    if (shouldDose) {
      // computePID renvoie le débit FINAL borné [0, ampFlow] — itération 2 : le
      // plancher minFlow (clamp vers le haut du modèle linéaire) est SUPPRIMÉ ;
      // feature-073 : sous le pouls minimal la demande est servie par
      // l'ACCUMULATEUR de planPulseWithCarry (plus de deadband de refus).
      //
      // `freezeIntegral = true` INCONDITIONNEL : l'intégrale appartient désormais
      // au module pur, qui l'a déjà entretenue ci-dessus pour ce tick. Laisser
      // computePidPure intégrer une seconde fois compterait le même dt deux fois,
      // et le ferait sur `error` au lieu de `error − stopThreshold`. Le cœur pur
      // ne SERT donc plus que le terme acquis (kp·e + ki·I, borné).
      //
      // BANDE MORTE ALIGNÉE SUR L'HYSTÉRÉSIS (condition de validité de la
      // correction ci-dessus). La bande morte passée à computePidPure valait
      // TOUJOURS phStartThreshold (0,05), y compris pendant une session déjà
      // ouverte : la sortie retombait donc à zéro dès 0,05, alors que
      // shouldContinueDosing déclare poursuivre jusqu'à phStopThreshold (0,01).
      // La bande ]0,01 ; 0,05] était une session VIVANTE à débit NUL, et le seuil
      // d'arrêt réellement appliqué était 0,05 — pas 0,01.
      // Conséquence directe sur l'intégrale : le régime établi se parquant à 0,05,
      // l'intégrande (erreur − 0,01) restait POSITIF en permanence et l'intégrale
      // remontait à son plafond malgré la décharge structurelle. Le point fixe
      // visé n'était tout simplement pas atteignable par la sortie.
      // On applique donc le MÊME seuil que la décision d'hystérésis : démarrage à
      // phStartThreshold, poursuite jusqu'à phStopThreshold. Vérifié en boucle
      // fermée : l'équilibre s'établit à consigne + 0,01, l'intégrale converge
      // vers la charge (≈ 46 % du plafond) au lieu de saturer, et le volume total
      // injecté est INCHANGÉ — à l'équilibre il vaut la charge du bassin, quelle
      // que soit la position de la bande morte (bilan de matière).
      const float phDeadband = phWasActiveTick ? pumpProtection.phStopThreshold
                                               : pumpProtection.phStartThreshold;
      // feature-085 : le PI ne dispose que de l'amplitude RESTANTE après le
      // feedforward — les deux termes sont des débits INSTANTANÉS, la
      // soustraction est donc dimensionnellement correcte ici (le pendant MOYEN,
      // pour l'anti-windup, est traité dans phIntegralTick via ffMlPerMin).
      float phPiMaxFlow = phAmpFlow - phFfFlow;
      if (phPiMaxFlow < 0.0f) phPiMaxFlow = 0.0f;
      flow = computePID(phPID, error, now,
                        phDeadband, /*freezeIntegral=*/true,
                        0.0f, phPiMaxFlow);
      // TERME ADDITIF (condition n°1 du gate) : le FF s'ajoute au PI, la somme
      // reste clampée à l'amplitude — redondant avec la réduction ci-dessus
      // (PI ≤ ampFlow − ff ⇒ somme ≤ ampFlow), gardé en défense en profondeur.
      // Un FF > 0 avec un PI encore muet (premier tick, dt en reconstruction)
      // produit un débit non nul : la charge connue est couverte dès la
      // première période d'impulsion, c'est tout l'objet de la feature.
      flow += phFfFlow;
      if (flow > phAmpFlow) flow = phAmpFlow;
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
          _phSessionStartMs = now32;  // base du critère MURAL du plafond de session
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
            flow, phAmpFlow, kPumpFlow255MlPerMin, kPulsePeriodMs,
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
                                                       kPumpFlow255MlPerMin, phAmpFlow), 1) +
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
      // v2.26.0 (revue, bloquant n°2) : PROPRIÉTÉ de la session — c'est la branche
      // AUTOMATIQUE qui la porte. Consommé par la garde de fermeture hors branche
      // en fin d'update() (cf. _phSessionFromAuto).
      _phSessionFromAuto = true;
    }

    // --- Fin de session (erreur résorbée, gate, entrées de plan invalides…) ---
    // feature-073 : une demande vivante sous le plancher ne ferme PLUS la
    // séance (régime d'accumulation, pompe OFF entre les tirs).
    if (phWasActiveTick && !phActive) {
      // v2.26.0 : la fermeture (état + pause mélange + log) est CENTRALISÉE dans
      // closePhDosingSession() — même rituel ici et sur les chemins de suspension
      // hors branche, plus aucune fin de session silencieuse.
      closePhDosingSession(now, phCapReason != nullptr ? phCapReason
                                                       : "erreur résorbée");
      phSessionClosed = true;
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
             mqttCfg.phDailyTargetMl > 0 && !isPhGuardBlocking(false)) {
    // pool-chemistry #2 : le mode Programmée est aveugle à la mesure PAR
    // CONCEPTION, mais les gardes pH s'y appliquent QUAND MÊME — ce sont des
    // filets de sécurité chimique, pas une boucle de régulation. `false` = ces
    // refus ne sont ni comptés ni escaladés (une consigne volumétrique choisie
    // par l'utilisateur ne doit pas fermer la régulation) ; ils sont signalés
    // en warn + alerte MQTT à débit limité.
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
              _phSessionStartMs = (uint32_t)now;  // base du plafond de session (mode auto)
              _pulseState[0] = {(uint32_t)now, (uint32_t)now, true};
            }
            bool kick = ((uint32_t)now - _pulseState[0].pulseStartMs) < kPulseKickMs;
            uint8_t duty = kick ? MAX_PWM_DUTY : phSchedAmpDuty;
            if (duty > desiredDuty[phSchedIdx]) desiredDuty[phSchedIdx] = duty;
            phActive = true;
            phFlow = effectiveFlowMlPerMin;
            // v2.26.0 : la session appartient désormais au mode PROGRAMMÉ, qui
            // journalise ses propres arrêts de fenêtre — la garde de fermeture
            // hors branche ne doit pas la doubler à chaque fin de fenêtre.
            _phSessionFromAuto = false;
          }

          _phSchedWindowIdx = dec.windowIndex;
          _phSchedStopTargetMl = dec.stopTargetMl;
          _phSchedPlannedFlow = dec.plannedFlowMlPerMin;
        }
      }
    }
    // Reset PID pour éviter le windup au retour en mode automatique. CONSERVÉ
    // tel quel (verdict du gate v2.26.0) : cette branche est hors PID — elle dose
    // une quantité planifiée, aveugle à la mesure. Une intégrale entretenue
    // pendant des jours de mode Programmée serait servie d'un bloc au retour en
    // automatique, sans jamais avoir été confrontée à une erreur mesurée.
    phPID.integral = 0.0f;
    phPID.lastError = 0.0f;
    phPID.lastTime = 0;
    phIntegralReset(_phIntegral);  // état PUR remis à zéro avec son miroir
    // feature-084 (R2/R4) : idem pour une restauration NVS en attente. Le mode
    // Programmée a dosé À L'AVEUGLE pendant ce temps ; la charge décrite par
    // l'instantané ne dit plus rien de l'état du bassin au retour en automatique.
    phPersistConfirmAbandon(_phIntegralConfirm);
  }

  // Contrôle ORP — dispatch sur orpRegulationMode, gardé en profondeur par canDose(1).
  // feature-067 (pool-chemistry #3) : inhibition ORP sur pH élevé, évaluée AVANT
  // canDose(1) (même patron que le plancher acide côté pH). Contre-visite C1 :
  // la DÉTECTION et l'alerte « pH élevé, chlore inhibé » ont été faites plus haut
  // par tickOrpPhInhibitSurveillance(), hors conjonction — elles partent donc même
  // quand le quota chlore journalier est atteint (orpSafetyOk faux), cas où cette
  // alerte est justement la seule à désigner la vraie cause.
  const String& orpMode = mqttCfg.orpRegulationMode;
  if (orpMode == "automatic" && orpLimitOk && orpSafetyOk && !isOrpInhibitedByPh() && canDose(1)) {
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
            flow, orpAmpFlow, kPumpFlow255MlPerMin, kPulsePeriodMs,
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
                                                       kPumpFlow255MlPerMin, orpAmpFlow), 1) +
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
             mqttCfg.orpDailyTargetMl > 0 && !isOrpInhibitedByPh()) {
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

  // ===== v2.26.0 — FERMETURE DE SESSION pH HORS BRANCHE AUTOMATIQUE ===========
  // Le rituel de fin de session (pause mélange + log) ne vivait QUE dans la
  // branche automatique. Or celle-ci est court-circuitée par la limite horaire,
  // la limite journalière, une garde pH ou canDose() : la session se refermait
  // alors en silence sur le `phDosingState.active = phActive` de fin d'update(),
  // sans jamais armer la pause mélange sur le produit déjà injecté ni laisser de
  // trace au journal. C'est le second volet de la condition bloquante n°1.
  //
  // Placé AVANT la libération du train d'impulsions ci-dessous : le log d'arrêt
  // consomme encore _sessionOnMs / _sessionVolMl / _phSessionStartMs.
  //
  // v2.26.0 (revue, bloquant n°2) — L'EXCLUSION PORTE SUR LA PROPRIÉTÉ DE LA
  // SESSION, PLUS SUR LE MODE COURANT. La condition testait `phMode != "scheduled"`
  // : une bascule automatique → programmée EN COURS DE SESSION s'excluait alors
  // elle-même, et la session ouverte par l'automatique se refermait muette au
  // `phDosingState.active = phActive` de fin d'update() — sans pause mélange. Le
  // témoin `_phSessionFromAuto` dit QUI a ouvert la session : une session
  // automatique est refermée rituellement quel que soit le mode devenu courant,
  // tandis qu'une session PROGRAMMÉE garde son propre journal de fin de fenêtre
  // (comportement inchangé, hors périmètre). Les bascules vers `manual` ou `off`
  // étaient déjà couvertes et le restent.
  if (phWasActiveAtEntry && !phActive && !phSessionClosed && _phSessionFromAuto) {
    closePhDosingSession(now, "branche automatique suspendue (garde, limite, quota ou mode)");
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
    _phSessionStartMs = 0;  // plus de session ouverte : base du plafond invalidée
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

  // feature-084 (R5) : instantané NVS de l'intégrale pH — au plus une écriture
  // par demi-heure ET seulement au-delà de 5 % de variation du plafond. Même
  // point de passage que le flush ci-dessus (usure NVS maîtrisée, loopTask).
  tickPhIntegralPersistence(now);

  phDosingState.active = phActive;
  orpDosingState.active = orpActive;
}

// Mode maintenance — voir l'en-tête pour le motif du flush NVS à l'entrée.
void PumpControllerClass::setMaintenanceInhibit(bool inhibit) {
  const bool wasInhibited = maintenanceInhibit;
  maintenanceInhibit = inhibit;
  if (inhibit && !wasInhibited) {
    _dailyCountersDirty = false;
    saveDailyCounters();  // borne la perte à l'instant de la bascule
  }
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
    // ===== v2.26.0 (revue, bloquant n°2) : FERMETURE DE SESSION pH DÉLÉGUÉE =====
    // Ce setter est appelé depuis la tâche WEB ASYNC (routes OTA) autant que
    // depuis loopTask (ArduinoOTA). Il refermait la session pH sur place
    // (`phDosingState.active = false`) SANS armer la pause mélange : une OTA
    // AVORTÉE ne reboote pas, et le dosage reprenait alors sur une eau qui n'avait
    // jamais eu son temps d'homogénéisation.
    // On ne referme donc PLUS ici : la coupure des pompes (ci-dessous) est
    // immédiate et c'est elle qui est critique ; le RITUEL de fermeture (état +
    // pause mélange + log) est laissé à la garde `otaInProgress` d'update(), au
    // tour de loop suivant. closePhDosingSession() reste ainsi appelée depuis
    // loopTask UNIQUEMENT, comme son contrat l'exige.
    orpDosingState.active = false;
    orpDosingState.lastStopTime = now;
    applyPumpDuty(0, 0);
    applyPumpDuty(1, 0);
    systemLogger.warning("Arrêt pompes dosage (OTA en cours)");
  }
}

// ATTENTION (v2.26.0) : ce setter ne touche PAS `integralMax`, qui est dimensionné
// PAR L'AUTORITÉ (U_I,max / ki) dans applyRegulationSpeed(). Poser un Ki ici sans
// reposer integralMax donne une régulation « avec intégrale » incapable de fournir
// la charge — l'échec silencieux exact que la v2.26.0 corrige. Aucun appelant
// aujourd'hui : la vitesse de régulation est la SEULE voie de réglage du PID.
void PumpControllerClass::setPhPID(float kp, float ki, float kd) {
  phPID.kp = kp;
  phPID.ki = ki;
  phPID.kd = kd;
  systemLogger.info("PID pH configuré: Kp=" + String(kp) + " Ki=" + String(ki) +
                    " Kd=" + String(kd) + " (integralMax INCHANGÉ=" +
                    String(phPID.integralMax, 0) + " pH·s)");
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
