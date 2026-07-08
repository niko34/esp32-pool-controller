#ifndef FILTRATION_H
#define FILTRATION_H

#include <Arduino.h>
#include "config.h"

struct FiltrationRuntime {
  bool running = false;
  bool scheduleComputedThisCycle = false;
  unsigned long startedAtMs = 0;
  unsigned long lastStoppedAtMs = 0;   // Horodatage du dernier arrêt (0 = jamais arrêtée)
  unsigned long forceOnStartMs = 0;    // Quand forceOn a été activé (0 = inactif)
  unsigned long forceOffStartMs = 0;   // Quand forceOff a été activé (0 = inactif)
};

class FiltrationManager {
private:
  FiltrationRuntime state;
  bool relayState = false;
  float _lastScheduledTemp = NAN;  // Température du dernier calcul auto (deadband 1°C)

  // feature-056 : état de la filtration EXTERNE (mode ExternalFiltration).
  // Triplet protégé par un spinlock portMUX (PAS un mutex FreeRTOS : écrit par
  // le handler HTTP et le callback MQTT async → un mutex bloquant y est proscrit).
  // Section critique MINIMALE (condition pool-chemistry #5) : on ne copie que les
  // 3 champs sous lock, l'évaluation et les logs se font HORS lock. Jamais persisté
  // en NVS (condition #3 : boot toujours OFF/known=false → fail-safe).
  mutable portMUX_TYPE _externalMux = portMUX_INITIALIZER_UNLOCKED;
  bool _externalOn = false;       // dernier état signalé
  uint32_t _externalLastMs = 0;   // millis() de réception du dernier signal
  bool _externalKnown = false;    // au moins un signal reçu depuis le boot

  bool getCurrentMinutesOfDay(int& minutes);
  int timeStringToMinutes(const String& value);
  bool isMinutesInRange(int now, int start, int end);

public:
  void begin();
  void update();

  bool isRunning() const { return state.running; }
  bool getRelayState() const { return relayState; }

  // feature-056 : signal d'état de la filtration externe. setExternalState est
  // NON bloquant (spinlock, horodatage millis() interne) — sûr depuis un handler
  // HTTP ou un callback MQTT async. getExternalState copie le triplet sous lock.
  void setExternalState(bool running);
  void getExternalState(bool& on, uint32_t& lastMs, bool& known) const;

  // feature-056 : résolution de la présence d'eau selon le mode d'installation.
  // SOURCE UNIQUE consommée par toutes les gardes de dosage (canDose, injection
  // manuelle, monitor injection). Fail-closed strict.
  WaterPresence resolveWaterPresence() const;

  void computeAutoSchedule();
  void ensureTimesValid();
  void publishState();
};

extern FiltrationManager filtration;

#endif // FILTRATION_H
