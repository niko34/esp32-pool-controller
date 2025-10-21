#ifndef FILTRATION_H
#define FILTRATION_H

#include <Arduino.h>
#include "config.h"

struct FiltrationRuntime {
  bool running = false;
  bool scheduleComputedThisCycle = false;
  unsigned long startedAtMs = 0;
  float cycleMaxTemp = -INFINITY;
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
