#include "schedule_logic.h"

#include <string.h>

// =============================================================================
// schedule_logic — implémentation PURE (feature-038, characterization refactor)
// =============================================================================
// IMPORTANT : NE PAS modifier les seuils, l'ordre des comparaisons ni les
// arrondis. Tout doit rester strictement identique à filtration.cpp d'origine.

// Parse les `digits` premiers chiffres décimaux d'une sous-chaîne, à la manière
// de String::substring(...).toInt() pour une entrée normalisée "HH:MM" : on lit
// les chiffres consécutifs et on s'arrête au premier non-chiffre. Sur "HH:MM"
// les positions [0,1] et [3,4] sont garanties contenir 2 caractères (longueur
// >= 5 vérifiée par l'appelant) → équivalent à substring(a,b).toInt().
static int parseTwoDigits(const char* s) {
  int value = 0;
  for (int i = 0; i < 2; i++) {
    char c = s[i];
    if (c < '0' || c > '9') break;
    value = value * 10 + (c - '0');
  }
  return value;
}

int timeStringToMinutes(const char* hhmm) {
  if (hhmm == nullptr) return -1;
  if (strlen(hhmm) < 5 || hhmm[2] != ':') return -1;
  int hh = parseTwoDigits(&hhmm[0]);
  int mm = parseTwoDigits(&hhmm[3]);
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

bool isMinutesInRange(int now, int start, int end) {
  if (start == -1 || end == -1) return false;
  if (start == end) return false;  // Plage invalide → pas de filtration
  if (start < end) {
    return now >= start && now < end;
  }
  return now >= start || now < end;
}

ScheduleWindow computeAutoWindow(float tempC, float pivotHour) {
  if (tempC < 0) tempC = 0;
  float durationHours = tempC / 2.0f;
  if (durationHours < 1.0f) durationHours = 1.0f;
  if (durationHours > 24.0f) durationHours = 24.0f;

  float startHour = pivotHour - (durationHours / 2.0f);
  float endHour = startHour + durationHours;

  // wrap dans [0, 24)
  while (startHour < 0) startHour += 24.0f;
  while (startHour >= 24.0f) startHour -= 24.0f;
  while (endHour < 0) endHour += 24.0f;
  while (endHour >= 24.0f) endHour -= 24.0f;

  // Conversion heure flottante → minutes avec arrondi/carry identique à l'ancien
  // float→"HH:MM"→re-parse.
  auto toMinutes = [](float hour) -> int {
    int h = static_cast<int>(hour);
    int m = static_cast<int>(round((hour - h) * 60.0f));
    if (m >= 60) { m -= 60; h = (h + 1) % 24; }
    return h * 60 + m;
  };

  return { toMinutes(startHour), toMinutes(endHour) };
}

bool decideFiltrationRun(const char* mode, bool forceOn, bool forceOff,
                         bool haveTime, int nowMin, int startMin, int endMin,
                         bool currentlyRunning) {
  if (forceOn) {
    return true;
  } else if (forceOff) {
    return false;
  } else if (strcmp(mode, "manual") == 0 || strcmp(mode, "auto") == 0) {
    if (haveTime) {
      return isMinutesInRange(nowMin, startMin, endMin);
    } else {
      // Heure indisponible : on conserve l'état courant pour éviter un faux
      // start/stop pendant un OTA ou une perte RTC (condition pool-chemistry).
      return currentlyRunning;
    }
  }
  return false;
}
