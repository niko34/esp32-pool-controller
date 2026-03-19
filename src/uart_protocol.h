#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Couche protocole UART
// Parse les lignes JSON reçues depuis uart_transport,
// dispatche vers uart_commands, et fournit les helpers d'envoi.

class UartProtocol {
public:
  // Appelé par UartTransport quand une ligne complète est reçue
  void onLineReceived(const String& line);

  // Helpers d'envoi (appelés par uart_commands ou les modules)
  void sendAck(const String& cmd);
  void sendError(const String& cmd, const String& message);
  void sendErrorNoCmd(const String& message);

  // Envoi d'un document JSON quelconque
  void sendJson(JsonDocument& doc);

  // Envoi d'un événement asynchrone vers l'écran
  // type   : "event" ou "alarm"
  // event  : "filtration_changed", "dosing_changed", "alarm_raised", etc.
  // Passe un JsonObject optionnel pour les données additionnelles
  // Usage : appelez sendEvent puis remplissez le JsonObject retourné avant
  //         d'appeler finishEvent() — ou utilisez les variantes ci-dessous.
  void sendEventSimple(const char* type, const char* event);
  void sendEventBool(const char* type, const char* event, const char* key, bool value);
  void sendEventStr(const char* type, const char* event, const char* key, const String& value);
  void sendAlarmRaised(const char* code, const char* message);
  void sendAlarmCleared(const char* code);

  // Événements spécialisés (sans dépendance ArduinoJson dans l'appelant)
  void sendDosingEvent(bool phActive, bool orpActive);
};

extern UartProtocol uartProtocol;

#endif // UART_PROTOCOL_H
