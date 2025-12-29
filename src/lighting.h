#ifndef LIGHTING_H
#define LIGHTING_H

#include <Arduino.h>
#include "config.h"

struct LightingRuntime {
  bool manualOverride = false;  // True si contrôle manuel actif
  unsigned long manualSetAtMs = 0;
};

class LightingManager {
private:
  LightingRuntime state;
  bool relayState = false;

  bool getCurrentMinutesOfDay(int& minutes);
  int timeStringToMinutes(const String& value);
  bool isMinutesInRange(int now, int start, int end);

public:
  void begin();
  void update();

  bool isOn() const { return relayState; }
  bool getRelayState() const { return relayState; }

  void setManualOn();
  void setManualOff();
  void ensureTimesValid();
  void publishState();
};

extern LightingManager lighting;

#endif // LIGHTING_H
