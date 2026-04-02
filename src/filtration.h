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

  bool getCurrentMinutesOfDay(int& minutes);
  int timeStringToMinutes(const String& value);
  bool isMinutesInRange(int now, int start, int end);

public:
  void begin();
  void update();

  bool isRunning() const { return state.running; }
  bool getRelayState() const { return relayState; }

  void computeAutoSchedule();
  void ensureTimesValid();
  void publishState();
};

extern FiltrationManager filtration;

#endif // FILTRATION_H
