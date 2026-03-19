#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H

#include <Arduino.h>

// Couche transport UART (Serial2)
// Gère la réception non-bloquante ligne par ligne et l'émission.
// Toute la logique métier est dans uart_protocol / uart_commands.

constexpr int kUart2RxPin = 16;
constexpr int kUart2TxPin = 17;
constexpr uint32_t kUart2Baud = 115200;
constexpr size_t kUartMaxLineLen = 512;

class UartTransport {
public:
  void begin();
  // À appeler dans loop() - non bloquant
  void update();
  // Envoie une ligne JSON sur Serial2 (ajoute \n)
  void sendLine(const String& line);

private:
  char _buf[kUartMaxLineLen + 1];
  size_t _pos = 0;
  bool _overflow = false;  // ligne trop longue : ignorer jusqu'au prochain \n

  void processLine(const char* line, size_t len);
};

extern UartTransport uartTransport;

#endif // UART_TRANSPORT_H
