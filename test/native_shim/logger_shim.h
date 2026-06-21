#ifndef NATIVE_LOGGER_SHIM_H
#define NATIVE_LOGGER_SHIM_H

// =============================================================================
// Shim natif pour logger.h (feature-025 tests).
//
// PROBLÈME : src/sensor_filter.cpp inclut "logger.h" pour tracer les re-sync et
// le latch instable. Or src/logger.h tire <vector>, <freertos/semphr.h>, <FS.h>
// — indisponibles sur l'hôte de test natif (pas de libc++).
//
// SOLUTION (harness only, ZÉRO modification de la prod) : ce header est force-inclus
// en premier via build_flags (-include) et définit le garde LOGGER_H, ce qui
// neutralise totalement le corps de src/logger.h quand sensor_filter.cpp l'inclut.
// On fournit ici un `String` minimal et un `systemLogger` no-op suffisants pour
// que les appels systemLogger.warning(...) / .critical(...) compilent et tournent.
//
// On reste en C pur (pas de std::string) pour rester compilable sans libc++.
// =============================================================================

#define LOGGER_H  // neutralise src/logger.h (son corps devient vide)

// Ce header est force-inclus dans TOUTES les TU, y compris les .c de Unity.
// Le contenu C++ (classes) ne doit donc apparaître qu'en compilation C++.
#ifdef __cplusplus

#include <math.h>
#include <stdint.h>
#include <stddef.h>

// --- String minimal : juste assez pour les concaténations de logs ----------
// SensorFilter fait : String("...") + "txt" + String(float,3) + String(uint8_t)...
class String {
 public:
  String() {}
  String(const char*) {}
  String(float, int = 2) {}
  String(double, int = 2) {}
  String(int) {}
  String(unsigned int) {}
  String(long) {}
  String(unsigned long) {}
  String(unsigned char) {}
  String operator+(const char*) const { return String(); }
  String operator+(const String&) const { return String(); }
};
inline String operator+(const char*, const String&) { return String(); }

// --- Logger no-op ----------------------------------------------------------
class NativeLoggerStub {
 public:
  void debug(const String&) {}
  void info(const String&) {}
  void warning(const String&) {}
  void error(const String&) {}
  void critical(const String&) {}
};

static NativeLoggerStub systemLogger;

#endif  // __cplusplus

#endif  // NATIVE_LOGGER_SHIM_H
