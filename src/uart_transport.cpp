#include "uart_transport.h"
#include "uart_protocol.h"
#include "logger.h"

UartTransport uartTransport;

void UartTransport::begin() {
  Serial2.begin(kUart2Baud, SERIAL_8N1, kUart2RxPin, kUart2TxPin);
  _pos = 0;
  _overflow = false;
  systemLogger.info("UART2 démarré (RX=GPIO" + String(kUart2RxPin) +
                    ", TX=GPIO" + String(kUart2TxPin) +
                    ", " + String(kUart2Baud) + " bps)");
}

void UartTransport::update() {
  while (Serial2.available()) {
    int c = Serial2.read();
    if (c < 0) break;

    if (c == '\n' || c == '\r') {
      if (!_overflow && _pos > 0) {
        _buf[_pos] = '\0';
        processLine(_buf, _pos);
      }
      _pos = 0;
      _overflow = false;
    } else {
      if (_pos >= kUartMaxLineLen) {
        // Ligne trop longue : on ignore jusqu'au prochain \n
        if (!_overflow) {
          systemLogger.warning("UART2: ligne trop longue, ignorée");
          _overflow = true;
        }
      } else {
        _buf[_pos++] = static_cast<char>(c);
      }
    }
  }
}

void UartTransport::sendLine(const String& line) {
  Serial2.print(line);
  Serial2.print('\n');
}

void UartTransport::processLine(const char* line, size_t /*len*/) {
  uartProtocol.onLineReceived(String(line));
}
