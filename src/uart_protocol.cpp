#include "uart_protocol.h"
#include "uart_transport.h"
#include "uart_commands.h"
#include "logger.h"
#include "json_compat.h"

UartProtocol uartProtocol;

void UartProtocol::onLineReceived(const String& line) {
  // Ignorer les lignes vides
  if (line.length() == 0) return;
  systemLogger.info("[UART] RX: " + line.substring(0, 80));

  // Buffer de parsing (commandes les plus longues ~256 octets, set_config avec data)
  StaticJson<512> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    sendErrorNoCmd("invalid json: " + String(err.c_str()));
    return;
  }

  if (!doc["cmd"].is<const char*>()) {
    sendErrorNoCmd("missing cmd field");
    return;
  }

  const char* cmd = doc["cmd"];
  JsonVariant data = doc["data"];

  uartCommands.dispatch(cmd, data);
}

void UartProtocol::sendJson(JsonDocument& doc) {
  String out;
  out.reserve(256);
  serializeJson(doc, out);
  uartTransport.sendLine(out);
}

void UartProtocol::sendAck(const String& cmd) {
  StaticJson<64> doc;
  doc["type"] = "ack";
  doc["cmd"] = cmd;
  sendJson(doc);
}

void UartProtocol::sendError(const String& cmd, const String& message) {
  StaticJson<128> doc;
  doc["type"] = "error";
  doc["cmd"] = cmd;
  doc["message"] = message;
  sendJson(doc);
}

void UartProtocol::sendErrorNoCmd(const String& message) {
  StaticJson<128> doc;
  doc["type"] = "error";
  doc["message"] = message;
  sendJson(doc);
}

void UartProtocol::sendEventSimple(const char* type, const char* event) {
  StaticJson<64> doc;
  doc["type"] = type;
  doc["event"] = event;
  sendJson(doc);
}

void UartProtocol::sendEventBool(const char* type, const char* event, const char* key, bool value) {
  StaticJson<96> doc;
  doc["type"] = type;
  doc["event"] = event;
  doc["data"][key] = value;
  sendJson(doc);
}

void UartProtocol::sendEventStr(const char* type, const char* event, const char* key, const String& value) {
  StaticJson<128> doc;
  doc["type"] = type;
  doc["event"] = event;
  doc["data"][key] = value;
  sendJson(doc);
}

void UartProtocol::sendAlarmRaised(const char* code, const char* message) {
  StaticJson<128> doc;
  doc["type"] = "alarm";
  doc["event"] = "raised";
  doc["data"]["code"] = code;
  doc["data"]["message"] = message;
  sendJson(doc);
}

void UartProtocol::sendAlarmCleared(const char* code) {
  StaticJson<96> doc;
  doc["type"] = "alarm";
  doc["event"] = "cleared";
  doc["data"]["code"] = code;
  sendJson(doc);
}

void UartProtocol::sendDosingEvent(bool phActive, bool orpActive) {
  StaticJson<96> doc;
  doc["type"] = "event";
  doc["event"] = "dosing_changed";
  doc["data"]["ph"] = phActive;
  doc["data"]["orp"] = orpActive;
  sendJson(doc);
}
