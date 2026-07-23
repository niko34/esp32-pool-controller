#include "mqtt_topics_logic.h"

#include <string.h>

// =============================================================================
// mqtt_topics_logic — implémentation PURE (bug-race-refreshtopics-uaf)
// =============================================================================
// Aucune dépendance runtime : logique déterministe testable en natif.

namespace {
// Mêmes caractères que isspace()/String::trim() Arduino — sans <ctype.h> pour
// éviter tout comportement dépendant de la locale.
bool isSpaceChar(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
}  // namespace

void normalizeMqttBase(const char* raw, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) return;

  size_t begin = 0;
  size_t end = 0;  // exclusif
  if (raw != nullptr) {
    end = strlen(raw);
    // 1. Trim des blancs en tête et en queue.
    while (begin < end && isSpaceChar(raw[begin])) ++begin;
    while (end > begin && isSpaceChar(raw[end - 1])) --end;
    // 2. Suppression des '/' finaux (multiples). Pas de re-trim ensuite —
    //    comportement identique à l'ancien code inline de refreshTopics().
    while (end > begin && raw[end - 1] == '/') --end;
  }

  size_t len = end - begin;
  if (len == 0) {
    // 3. Vide ou réduit à rien → fallback.
    len = sizeof(kMqttDefaultBase) - 1;
    if (len > outLen - 1) len = outLen - 1;
    memcpy(out, kMqttDefaultBase, len);
    out[len] = '\0';
    return;
  }

  if (len > outLen - 1) len = outLen - 1;  // troncature silencieuse
  memcpy(out, raw + begin, len);
  out[len] = '\0';
}
