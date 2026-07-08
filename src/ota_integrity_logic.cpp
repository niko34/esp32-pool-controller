#include "ota_integrity_logic.h"

// Longueurs du format "sha256:<64hex>" — dupliquées volontairement ici
// (module pur : pas d'include de constants.h qui tire Arduino.h).
static const size_t kPrefixLen = 7;   // strlen("sha256:")
static const size_t kHashBytes = 32;  // SHA-256 = 32 octets = 64 hex

// Valeur d'un caractère hexadécimal (casse indifférente), -1 si invalide.
static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseSha256Digest(const char* digestStr, uint8_t out[32]) {
  if (digestStr == NULL || out == NULL) {
    return false;
  }

  // Préfixe "sha256:" obligatoire, comparé en casse indifférente.
  static const char prefix[] = "sha256:";
  for (size_t i = 0; i < kPrefixLen; ++i) {
    char c = digestStr[i];
    if (c == '\0') {
      return false;  // Chaîne trop courte pour contenir le préfixe
    }
    char lower = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    if (lower != prefix[i]) {
      return false;
    }
  }

  // Exactement 64 caractères hexadécimaux après le préfixe.
  // On s'arrête au premier caractère invalide (dont '\0') : aucune lecture
  // au-delà du terminateur n'est possible.
  const char* hex = digestStr + kPrefixLen;
  for (size_t i = 0; i < kHashBytes; ++i) {
    int hi = hexNibble(hex[2 * i]);
    if (hi < 0) {
      return false;
    }
    int lo = hexNibble(hex[2 * i + 1]);
    if (lo < 0) {
      return false;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }

  // Rien ne doit suivre les 64 hex (refuse "sha256:<64hex>garbage").
  if (hex[2 * kHashBytes] != '\0') {
    return false;
  }

  return true;
}

bool sha256Equal(const uint8_t a[32], const uint8_t b[32]) {
  if (a == NULL || b == NULL) {
    return false;
  }
  // Comparaison en temps constant : on accumule les différences sans
  // sortie anticipée (32 octets, coût négligeable).
  uint8_t diff = 0;
  for (size_t i = 0; i < kHashBytes; ++i) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

void sha256ToHex(const uint8_t hash[32], char* out, size_t outLen) {
  if (out == NULL || outLen == 0) {
    return;
  }
  if (hash == NULL || outLen < 2 * kHashBytes + 1) {
    out[0] = '\0';  // Buffer trop petit : chaîne vide, jamais de débordement
    return;
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < kHashBytes; ++i) {
    out[2 * i]     = digits[hash[i] >> 4];
    out[2 * i + 1] = digits[hash[i] & 0x0F];
  }
  out[2 * kHashBytes] = '\0';
}
