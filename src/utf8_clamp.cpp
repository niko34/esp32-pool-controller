#include "utf8_clamp.h"
#include <string.h>

size_t utf8ClampLen(const char* s, size_t len) {
  if (len == 0) return 0;

  // Reculer jusqu'au premier octet du DERNIER caractère (les octets de
  // continuation valent 10xxxxxx, soit (b & 0xC0) == 0x80).
  size_t start = len - 1;
  while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;

  const unsigned char lead = static_cast<unsigned char>(s[start]);
  size_t expected;
  if ((lead & 0x80) == 0x00)      expected = 1;   // ASCII
  else if ((lead & 0xE0) == 0xC0) expected = 2;
  else if ((lead & 0xF0) == 0xE0) expected = 3;
  else if ((lead & 0xF8) == 0xF0) expected = 4;
  else return start;  // octet de tête invalide (lecture déchirée) : on le retire

  if (start + expected > len) return start;  // séquence tronquée : on la retire
  return len;  // le dernier caractère est complet, rien à couper
}

void utf8ClampInPlace(char* dst) {
  const size_t len = strlen(dst);
  dst[utf8ClampLen(dst, len)] = '\0';
}
