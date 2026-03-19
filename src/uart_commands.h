#ifndef UART_COMMANDS_H
#define UART_COMMANDS_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Dispatch des commandes UART et construction des réponses.
// Toutes les commandes mappent les données réelles du contrôleur
// sans modifier la logique métier.

class UartCommands {
public:
  // Appelé par UartProtocol avec la commande et le data JSON optionnel
  void dispatch(const char* cmd, JsonVariant data);

private:
  void handlePing();
  void handleGetInfo();
  void handleGetStatus();
  void handleGetConfig();
  void handleGetAlarms();
  void handleGetNetworkStatus();
  void handleSetConfig(JsonVariant data);
  void handleSaveConfig();
  void handleRunAction(JsonVariant data);
  // Assistant de mise en service (wizard IHM)
  void handleGetSetupStatus();
  void handleCompleteWizard();
  // Commandes wizard écran : réseau et sécurité
  void handleWifiScan();
  void handleWifiConnect(JsonVariant data);
  void handleChangePassword(JsonVariant data);

  // Actions individuelles
  void actionPumpTest(JsonVariant data);
  void actionPumpStop(JsonVariant data);
  void actionLightingOn();
  void actionLightingOff();
  void actionFiltrationMode(JsonVariant data);
  void actionCalibratePhNeutral();
  void actionCalibratePhAcid();
  void actionClearPhCalibration();
  void actionAckAlarm(JsonVariant data);
};

extern UartCommands uartCommands;

#endif // UART_COMMANDS_H
