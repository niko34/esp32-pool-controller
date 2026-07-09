#ifndef PUMP_CONTROLLER_H
#define PUMP_CONTROLLER_H

#include <Arduino.h>
#include <atomic>
#include "config.h"
#include "constants.h"

struct PumpDriver {
  int pwmPin;    // Pin PWM Gate MOSFET (IRLZ44N)
  int channel;   // Canal LEDC pour PWM
};

struct DosingState {
  unsigned long windowStart = 0;
  unsigned long usedMs = 0;
  bool active = false;
  unsigned long lastTimestamp = 0;
  unsigned long lastSafetyTimestamp = 0; // suivi ml injectés (sécurité)
  
  // Variables pour protection anti-cycling
  unsigned long lastStartTime = 0;      // Moment du dernier démarrage
  unsigned long lastStopTime = 0;       // Moment du dernier arrêt
  unsigned int cyclesToday = 0;         // Nombre de démarrages aujourd'hui
  unsigned long cyclesDayStart = 0;     // Timestamp du début du jour des cycles
};

struct PIDController {
  // feature-025 (pool-chemistry) : régulation P temporisée par défaut.
  //   - pH  : Kp=8, Ki=0, Kd=0
  //   - ORP : Kp=0.3, Ki=0, Kd=0 (override en construction PumpController)
  // Kd=0 IMPÉRATIF (pas d'amplification du bruit résiduel). Ki=0 → intégrale inerte
  // (integralMax conservé mais sans effet). Les valeurs ci-dessous sont les défauts pH ;
  // l'ORP est ajusté dans le constructeur de PumpControllerClass.
  float kp = 8.0f;     // Proportionnel : réaction à l'erreur actuelle
  float ki = 0.0f;     // Intégral : désactivé (anti-windup strict, P pure)
  float kd = 0.0f;     // Dérivé : désactivé (bruit)
  float integral = 0.0f;
  float lastError = 0.0f;
  unsigned long lastTime = 0;
  float integralMax = 50.0f; // Anti-windup (inerte avec Ki=0, conservé pour réactivation future)
};

class PumpControllerClass {
private:
  PumpDriver pumps[2];
  uint8_t pumpDuty[2] = {0, 0};
  bool manualMode[2] = {false, false};  // Mode test manuel par pompe
  bool otaInProgress = false;

  DosingState phDosingState;
  DosingState orpDosingState;

  PIDController phPID;
  PIDController orpPID;

  void applyPumpDuty(int index, uint8_t duty);
  // feature-006 (condition pool-chemistry #2) : `manualActive` = une injection
  // MANUELLE consomme aussi le budget horaire usedMs (budget PARTAGÉ auto+manuel).
  // Accumulation si state.active || manualActive (OR unique → pas de double comptage).
  void refreshDosingState(DosingState& state, unsigned long now, bool manualActive);

  // feature-025 : `deadband` = seuil de démarrage existant (phStartThreshold /
  // orpStartThreshold) ; `freezeIntegral` gèle l'accumulation intégrale (anti-windup
  // strict : filtre non prêt, canDose==false, pause mélange, saturation, mesure
  // rejetée/instable, erreur dans deadband). Le gel est appliqué AVANT le calcul.
  // feature-037 : le cœur du calcul (proportionnel + anti-windup + bornage final)
  // est délégué à computePidPure() (src/dosing_logic.*, testable en natif). La
  // coquille gère le temps (dt depuis millis), l'état PID et renvoie le débit
  // FINAL déjà borné [minFlow, maxFlow] (plus de constrain externe chez l'appelant).
  float computePID(PIDController& pid, float error, unsigned long now,
                   float deadband, bool freezeIntegral,
                   float minFlow, float maxFlow);
  float   dutyToFlow(const PumpControlParams& params, uint8_t duty);
  uint8_t flowToDuty(const PumpControlParams& params, float flowMlPerMin);

  bool checkSafetyLimits(bool isPhPump);
  // Reset journalier (RTC/NTP local minuit, ou fallback millis() 24h).
  // Doit être appelé en permanence depuis update(), AVANT le check de filtration,
  // pour que les compteurs se réinitialisent même quand la filtration est arrêtée.
  void tickDailyRollover();
  void updateSafetyTracking(bool isPhPump, float flowMlPerMin, unsigned long deltaMs);

  // Fonctions anti-cycling
  bool shouldStartDosing(float error, float startThreshold, DosingState& state, unsigned long now);
  bool shouldContinueDosing(float error, float stopThreshold, DosingState& state, unsigned long now);

  // Timer de stabilisation par pompe (index 0 = pH, 1 = ORP).
  // La cinétique chimique différente justifie une fenêtre indépendante par sonde
  // (cf. kStabilizationDurationPhMs / kStabilizationDurationOrpMs et
  // pool-chemistry condition #3).
  uint32_t _stabilizationEndMs[2] = {0, 0};

  // Cache de la dernière cause de refus pour log "edge-triggered" (1 seule entrée
  // par transition de cause). Évite le spam quand canDose() est appelé chaque cycle.
  // feature-025 : également exposé via getPhDoseBlockedReason()/getOrpDoseBlockedReason()
  // pour le WS (chaîne vide si dosage autorisé). Écrit/lu en loopTask uniquement.
  String _lastRefusalCause[2];

  // feature-025 : pause mélange hydraulique post-injection (pool-chemistry).
  // _mixingEndMs[0]=pH, [1]=ORP. Armé par notifyPhDose()/notifyOrpDose() à l'ARRÊT
  // d'une injection (homogénéisation post-dose) — bloque ainsi le cycle suivant sans
  // interrompre l'injection en cours. Gate indépendante (OR) dans canDose(), distincte du timer post-cal.
  // Écrits/lus en loopTask uniquement → pas de mutex (cohérent avec _stabilizationEndMs).
  uint32_t _mixingEndMs[2] = {0, 0};

  // Helpers internes pour le log de refus (canDose).
  void logRefusalOnce(int pumpIndex, const String& cause);
  void resetRefusalLogState(int pumpIndex);

  // Ring buffer anti-rafale (pool-chemistry Pass 3.5) — timestamps de start
  // de cycle (millis) par pompe. Index circulaire, 0 = pH, 1 = ORP.
  // Capacité 20 entrées : couvre largement la fenêtre 15 min à raison de
  // kMaxDosingCyclesPer15Min = 20 cycles max.
  uint32_t _dosingCycleHistory[2][kDosingCycleHistorySize] = {{0}, {0}};
  size_t _dosingCycleHistoryIdx[2] = {0, 0};

  // Enregistre un nouveau timestamp de démarrage de cycle dans le ring buffer.
  // À appeler EXACTEMENT au moment où le PWM passe de 0 → >0 (pas dans canDose,
  // qui ne fait que valider).
  void recordDosingCycleStart(int pumpIndex);

  // Compte les cycles dans la fenêtre [now - windowMs, now] pour la pompe.
  int countRecentDosingCycles(int pumpIndex, uint32_t windowMs) const;

  // Flags de reset demandés depuis des tâches externes (web handlers)
  // Résolus au début de update() pour éviter les races inter-core
  std::atomic<bool> _resetRequested{false};
  std::atomic<bool> _phPauseResetRequested{false};

  // feature-006 : démarrages de cycle MANUELS demandés depuis les handlers web
  // async (requestManualCycleRecord). Consommés en tête d'update() (loopTask,
  // AVANT les branches de régulation) : recordDosingCycleStart + cyclesToday++
  // + compteurs NVS dirty. Le ring anti-rafale et cyclesToday restent ainsi
  // écrits par UNE seule tâche (pas de mutex). Index 0 = pH, 1 = ORP.
  std::atomic<uint8_t> _manualCycleStartPending[2] = {{0}, {0}};

  // feature-011 : état de la répartition scheduled par fenêtres de 15 min
  // (kScheduledWindowMinutes). windowIdx = fenêtre courante (nowMin/15, -1 =
  // aucune → recalcul forcé) ; stopTargetMl = cumul journalier cible d'arrêt
  // de la fenêtre courante ; plannedFlow = débit moyen planifié restant
  // (diagnostic WS ; NAN hors mode scheduled / hors plage / heure invalide).
  // Écrits/lus en loopTask uniquement (cohérent avec _stabilizationEndMs).
  int _phSchedWindowIdx = -1;
  float _phSchedStopTargetMl = 0.0f;
  float _phSchedPlannedFlow = NAN;
  int _orpSchedWindowIdx = -1;
  float _orpSchedStopTargetMl = 0.0f;
  float _orpSchedPlannedFlow = NAN;

  // Persistance des compteurs journaliers en NVS
  static bool _dailyLoaded;          // Chargement différé effectué (attend NTP)
  static bool _dailyCountersDirty;   // Indique qu'une sauvegarde est en attente
  static unsigned long _lastDailySaveMs; // Timestamp dernier flush NVS
  static bool _phWasActive;          // État pH précédent (détection démarrage injection)
  static bool _orpWasActive;         // État ORP précédent

public:
  PumpControllerClass();

  void begin();
  void update();
  void stopAll();
  void setOtaInProgress(bool inProgress);

  // Getters pour l'état
  bool isPhDosing() const { return phDosingState.active; }
  bool isOrpDosing() const { return orpDosingState.active; }
  unsigned long getPhUsedMs() const { return phDosingState.usedMs; }
  unsigned long getOrpUsedMs() const { return orpDosingState.usedMs; }

  // ===== feature-053 : valeurs ORP EFFECTIVES (surcouche Mode Boost) =====
  // Surcouche NON destructive : renvoie la cible / limite journalière consommées
  // par la régulation. Effet STRICTEMENT réservé au mode ORP "automatic" et au
  // boost actif (isBoostActive) ; sinon renvoie la valeur normale de config.
  // Consommées par la régulation ORP automatic ET exposées à l'UI/WS/MQTT.
  float getEffectiveOrpTarget() const;
  float getEffectiveMaxChlorineMlPerDay() const;

  // Applique les paramètres PID selon mqttCfg.regulationSpeed
  void applyRegulationSpeed();

  // Setters pour PID tuning
  void setPhPID(float kp, float ki, float kd);
  void setOrpPID(float kp, float ki, float kd);

  // Reset des états
  void resetDosingStates();

  // Réinitialise la garde anti-cycling du pH (ex: changement de type de correction)
  void resetPhPauseGuard();

  // Arme le timer de stabilisation pour une pompe donnée (0 = pH, 1 = ORP).
  // Bloque le dosage pendant la durée appropriée :
  //   - kStabilizationDurationPhMs / kStabilizationDurationOrpMs après calibration EZO,
  //   - mqttCfg.stabilizationDelayMin (override utilisateur) pour les armings legacy
  //     (filtration, mode continu, passage de minuit) — applique la même durée aux 2 pompes.
  void armStabilizationTimer(int pumpIndex);

  // Surcharge legacy (sans paramètre) : conservée pour les sites d'arming "globaux"
  // (filtration, boot mode continu, rollover minuit). Arme les 2 pompes simultanément
  // avec la durée utilisateur `mqttCfg.stabilizationDelayMin`.
  void armStabilizationTimer();

  // True si le timer de stabilisation est encore actif pour la pompe donnée.
  bool isStabilizationTimerActive(int pumpIndex) const;

  // Réinitialise le timer de stabilisation des deux pompes (ex: arrêt filtration)
  void clearStabilizationTimer();

  // Retourne les secondes restantes de stabilisation max sur les 2 pompes
  // (0 si les deux sont expirés). Utilisé par l'UI WS pour afficher le compte à rebours.
  unsigned long getStabilizationRemainingS() const;

  // feature-006 : secondes restantes de stabilisation pour UNE pompe (0 si expirée
  // ou index invalide). ASYMÉTRIE ASSUMÉE (condition pool-chemistry #5) : la boucle
  // AUTO suspend les 2 pompes dès qu'UNE stabilisation est active (garde globale
  // d'update()), alors que l'injection MANUELLE n'est bloquée que par la fenêtre
  // de SA pompe — les fenêtres sont indépendantes par sonde
  // (kStabilizationDurationPhMs / kStabilizationDurationOrpMs, cinétiques différentes).
  unsigned long getStabilizationRemainingS(int pumpIndex) const;

  // ===== feature-006 : compteurs exposés aux gardes d'injection manuelle =====
  // Appelés depuis le handler web async (hors loopTask). Lecture SNAPSHOT sans
  // mutex : les compteurs sont écrits en loopTask uniquement et les lectures
  // 32 bits sont atomiques sur ESP32 → course BÉNIGNE (au pire une valeur en
  // retard d'un tour de loop). Les deux getters AJOUTENT le pending
  // _manualCycleStartPending non encore consommé : deux starts manuels
  // rapprochés se voient mutuellement (fail-closed).

  // Cycles du jour (auto + manuel), pending manuel inclus. 0 si index invalide.
  unsigned int getCyclesToday(int pumpIndex) const;

  // Cycles dans la fenêtre glissante [now - windowMs, now] (ring anti-rafale
  // PARTAGÉ avec l'auto), pending manuel inclus. 0 si index invalide.
  int getRecentCycles(int pumpIndex, uint32_t windowMs) const;

  // Demande l'enregistrement d'un démarrage de cycle MANUEL (thread-safe,
  // appelable depuis un handler async). Consommé en tête d'update() :
  // recordDosingCycleStart (ring anti-rafale partagé) + cyclesToday++ + NVS dirty.
  void requestManualCycleRecord(int pumpIndex);

  // Crédit plancher du registre journalier à la fin NATURELLE d'une injection
  // manuelle volumée (bug sous-compte v2.9.1). `idx` logique : 0 = pH, 1 = ORP.
  // À appeler en loopTask uniquement (updateManualInject).
  // Propriété : le registre de sécurité ne sous-compte jamais un volume promis
  // injecté en entier ; peut sur-compter de quelques dixièmes de mL
  // (conservateur) ; jamais appelé sur arrêt anticipé (stop manuel /
  // interruption filtration : l'intégration réelle fait foi).
  void creditManualInjectionFloor(int idx, float startCumulMl, float creditMl);

  // Vérifie si le dosage est autorisé pour la pompe `pumpIndex` (0 = pH, 1 = ORP).
  // Ordre des gardes (validé pool-chemistry feature-021) — fail-closed strict :
  //   1. Watchdog actif
  //   2. Filtration en marche (ou mode continu)
  //   3. Lecture pH/ORP FILTRÉE non NaN (feature-025 ; cond #1 stale + cond #5 bus dégradé)
  //   3b. Filtre capteur prêt (feature-025 : warmup / EZO injoignable → fail-closed)
  //   3c. Capteur non instable (feature-025 : rejets consécutifs → fail-closed)
  //   4. EZO calibré (cond #2 : pH ≥ 2 points, ORP ≥ 1 point ; -1 → bloqué)
  //   5. Pas de stabilisation post-cal en cours (cond #3)
  //   5b. Pas de pause mélange hydraulique active (feature-025 — gate indépendante)
  //   6. Mode régulation = automatic
  //   7. Limite journalière non atteinte
  //   8. Limite horaire non atteinte
  //   9. Anti-cycling : cyclesToday < maxCyclesPerDay
  //  10. Anti-rafale court terme (ring buffer 1 min / 15 min, Pass 3.5)
  // Log "edge-triggered" : 1 entrée info par transition de cause de refus.
  bool canDose(int pumpIndex);

  // ===== feature-025 : pause mélange hydraulique post-injection =====
  // À appeler au démarrage d'une injection (à côté de recordDosingCycleStart).
  // Arme _mixingEndMs[pompe] = nowMs + kPhMixingDelayMs / kOrpMixingDelayMs.
  // Géré uniquement par timestamps (AUCUN delay()).
  void notifyPhDose(uint32_t nowMs);
  void notifyOrpDose(uint32_t nowMs);
  bool isPhMixingDelayActive(uint32_t nowMs) const;
  bool isOrpMixingDelayActive(uint32_t nowMs) const;
  // Secondes restantes de pause mélange par pompe ([0]=pH, [1]=ORP) — observabilité UI.
  unsigned long getMixingRemainingS(int pumpIndex) const;

  // ===== feature-025 : raison de blocage dosage exposée au WS =====
  // Chaîne vide si dosage autorisé, sinon la dernière cause de refus de canDose().
  String getPhDoseBlockedReason() const { return _lastRefusalCause[0]; }
  String getOrpDoseBlockedReason() const { return _lastRefusalCause[1]; }

  // ===== feature-011 : débit moyen planifié du mode scheduled (mL/min) =====
  // Débit moyen restant de la répartition (remaining / horizonMinutes).
  // NAN hors mode scheduled, hors plage de filtration, ou heure locale invalide
  // (le WS traduit NAN en null). Rafraîchi à chaque tour d'update() (loopTask).
  float getPhScheduledPlannedFlow() const { return _phSchedPlannedFlow; }
  float getOrpScheduledPlannedFlow() const { return _orpSchedPlannedFlow; }

  // Test manuel des pompes (à utiliser avec précaution)
  void setManualPump(int pumpIndex, uint8_t duty);
};

extern PumpControllerClass PumpController;

#endif // PUMP_CONTROLLER_H
