#ifndef PUMP_CONTROLLER_H
#define PUMP_CONTROLLER_H

#include <Arduino.h>
#include "config.h"

struct PumpDriver {
  int pwmPin;    // Pin PWM Gate MOSFET (IRLZ44N)
  int channel;   // Canal LEDC pour PWM
};

struct DosingState {
  unsigned long windowStart = 0;
  unsigned long usedMs = 0;
  unsigned long lastTimestamp = 0;
  bool active = false;

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

  DosingState phDosingState;
  DosingState orpDosingState;

  PIDController phPID;
  PIDController orpPID;

  void applyPumpDuty(int index, uint8_t duty);
  void refreshDosingState(DosingState& state, unsigned long now);

  float computePID(PIDController& pid, float error, unsigned long now);
  float computeFlowFromError(float error, float deadband, const PumpControlParams& params);
  uint8_t flowToDuty(const PumpControlParams& params, float flowMlPerMin);

  bool checkSafetyLimits(bool isPhPump);
  void updateSafetyTracking(bool isPhPump, float flowMlPerMin, unsigned long deltaMs);

  // Fonctions anti-cycling
  bool shouldStartDosing(float error, float startThreshold, DosingState& state, unsigned long now);
  bool shouldContinueDosing(float error, float stopThreshold, DosingState& state, unsigned long now);

public:
  PumpControllerClass();

  void begin();
  void update();
  void stopAll();

  // Getters pour l'état
  bool isPhDosing() const { return phDosingState.active; }
  bool isOrpDosing() const { return orpDosingState.active; }
  unsigned long getPhUsedMs() const { return phDosingState.usedMs; }
  unsigned long getOrpUsedMs() const { return orpDosingState.usedMs; }

  // Setters pour PID tuning
  void setPhPID(float kp, float ki, float kd);
  void setOrpPID(float kp, float ki, float kd);

  // Reset des états
  void resetDosingStates();
};

extern PumpControllerClass PumpController;

#endif // PUMP_CONTROLLER_H
