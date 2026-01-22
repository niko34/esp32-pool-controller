#ifndef JSON_COMPAT_H
#define JSON_COMPAT_H

#include <ArduinoJson.h>

// Alias pour StaticJsonDocument avec suppression du warning deprecated
// StaticJsonDocument est utilisé pour l'optimisation mémoire sur ESP32
// (allocation sur la stack plutôt que sur le heap)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

template<size_t N>
using StaticJson = StaticJsonDocument<N>;

#pragma GCC diagnostic pop

#endif // JSON_COMPAT_H
