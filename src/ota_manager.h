#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>
#include <ArduinoOTA.h>

class OTAManager {
private:
  bool otaEnabled = false;

public:
  OTAManager();
  void begin();
  void handle();
  void setPassword(const String& password);
  bool isEnabled() const { return otaEnabled; }
};

extern OTAManager otaManager;

#endif // OTA_MANAGER_H
