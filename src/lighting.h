#ifndef LIGHTING_H
#define LIGHTING_H

#include <Arduino.h>
#include "config.h"
#include "schedule_logic.h"  // feature-162 : LightingMode (seule autorité)

class LightingManager {
private:
  bool relayState = false;

  bool getCurrentMinutesOfDay(int& minutes);

public:
  void begin();
  void update();

  bool isOn() const { return relayState; }
  bool getRelayState() const { return relayState; }

  // feature-162 : remplace setManualOn()/setManualOff()/clearManualOverride()
  // (ADR-0107/ADR-0108 — un état visible et persistant plutôt qu'une
  // suspension invisible). Écrit lightingCfg.mode (jeton canonique via
  // lightingModeToken()) et republie immédiatement l'état MQTT — NE PERSISTE
  // PAS en NVS elle-même : l'appelant appelle saveMqttConfig() (un seul
  // chemin d'écriture, cf. schedule_logic.h).
  void setMode(LightingMode mode);
  void ensureTimesValid();
  void publishState();
};

extern LightingManager lighting;

#endif // LIGHTING_H
