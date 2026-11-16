#ifndef NATIVE_ARDUINO_SHIM_H
#define NATIVE_ARDUINO_SHIM_H

// =============================================================================
// Shim Arduino minimal pour compiler SensorFilter en natif (feature-025 tests).
// SensorFilter n'utilise de <Arduino.h> que : types entiers, NAN, isnan, fabsf.
// On ne mocke RIEN du comportement métier — ce sont les fonctions standard C.
//
// IMPORTANT : on n'utilise QUE des en-têtes C (<math.h>, <stdint.h>, <stddef.h>).
// La libc++ (<cmath>, <cstdint>) n'est pas disponible sur l'hôte de CI/dev
// (Command Line Tools sans libc++ utilisable) → on reste en C pur, ce qui suffit
// largement pour SensorFilter et garde le test compilable partout.
// NB (D3-b2) : cette contrainte vaut pour les SHIMS eux-mêmes. src/ota_signature_switch.cpp,
// compilé en natif, inclut <atomic> (libc++/libstdc++) : sans objet sur g++/Linux (hôte
// actuel), mais un hôte sans bibliothèque C++ ne le compilerait pas.
// =============================================================================

#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>   // constants.h utilise time_t (kMinValidEpoch)

#endif // NATIVE_ARDUINO_SHIM_H
