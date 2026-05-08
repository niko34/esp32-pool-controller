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
  // Paramètres ajustés pour un système avec inertie
  float kp = 15.0f;    // Proportionnel: réaction à l'erreur actuelle
  float ki = 0.1f;     // Intégral: correction lente des erreurs persistantes
  float kd = 5.0f;     // Dérivé: anticipation (freine si descend rapidement)
  float integral = 0.0f;
  float lastError = 0.0f;
  unsigned long lastTime = 0;
  float integralMax = 50.0f; // Anti-windup réduit
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
  void refreshDosingState(DosingState& state, unsigned long now);

  float computePID(PIDController& pid, float error, unsigned long now);
  float computeFlowFromError(float error, float deadband, const PumpControlParams& params);
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
  String _lastRefusalCause[2];

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

  // Vérifie si le dosage est autorisé pour la pompe `pumpIndex` (0 = pH, 1 = ORP).
  // Ordre des gardes (validé pool-chemistry feature-021) — fail-closed strict :
  //   1. Watchdog actif
  //   2. Filtration en marche (ou mode continu)
  //   3. Lecture pH/ORP non NaN (cond #1 stale + cond #5 bus dégradé)
  //   4. EZO calibré (cond #2 : pH ≥ 2 points, ORP ≥ 1 point ; -1 → bloqué)
  //   5. Pas de stabilisation post-cal en cours (cond #3)
  //   6. Mode régulation = automatic
  //   7. Limite journalière non atteinte
  //   8. Limite horaire non atteinte
  //   9. Anti-cycling : cyclesToday < maxCyclesPerDay
  //  10. Anti-rafale court terme (ring buffer 1 min / 15 min, Pass 3.5)
  // Log "edge-triggered" : 1 entrée info par transition de cause de refus.
  bool canDose(int pumpIndex);

  // Test manuel des pompes (à utiliser avec précaution)
  void setManualPump(int pumpIndex, uint8_t duty);
};

extern PumpControllerClass PumpController;

#endif // PUMP_CONTROLLER_H
