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

// Interprète EXACTEMENT kHashBytes paires hexadécimales suivies du
// terminateur nul (aucun caractère supplémentaire accepté). S'arrête au
// premier caractère invalide (dont '\0') : aucune lecture au-delà du
// terminateur n'est possible. Partagé par parseSha256Digest (après le
// préfixe "sha256:") et parseOtaSha256Param (repli format brut, sans
// préfixe) — feature-118 lot A.
// ⚠️ Écrit dans un TAMPON LOCAL, recopié vers `out` SEULEMENT en cas de succès
// (revue code-reviewer, feature-118 lot A). L'ancienne version remplissait
// `out` octet par octet et sortait au premier nibble invalide : elle laissait
// donc `g_uploadExpected` PARTIELLEMENT écrasé sur un refus « malformée »,
// alors que le contrat de l'en-tête annonce l'inverse. Inoffensif en lot A
// (retour immédiat + garde `g_uploadHasExpected`), mais le lot B s'appuiera
// sur ce contrat : on rend le contrat VRAI plutôt que de documenter le piège.
// Coût : 32 octets de pile, sans allocation.
static bool parseHexDigest(const char* hex, uint8_t out[32]) {
  uint8_t staged[kHashBytes];
  for (size_t i = 0; i < kHashBytes; ++i) {
    int hi = hexNibble(hex[2 * i]);
    if (hi < 0) {
      return false;
    }
    int lo = hexNibble(hex[2 * i + 1]);
    if (lo < 0) {
      return false;
    }
    staged[i] = (uint8_t)((hi << 4) | lo);
  }
  // Rien ne doit suivre les 64 hex (refuse "<64hex>garbage").
  if (hex[2 * kHashBytes] != '\0') {
    return false;
  }
  for (size_t i = 0; i < kHashBytes; ++i) {
    out[i] = staged[i];
  }
  return true;
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
  return parseHexDigest(digestStr + kPrefixLen, out);
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

// =============================================================================
// feature-118 lot A
// =============================================================================

OtaSha256ParamVerdict parseOtaSha256Param(bool present, const char* digestStr, uint8_t out[32]) {
  if (!present || digestStr == NULL) {
    return OtaSha256ParamVerdict::kAbsent;
  }
  // Format brut "<64hex>" essayé D'ABORD (c'est celui de l'upload manuel
  // historique), PUIS le format préfixé "sha256:<64hex>" en repli (format de
  // l'API GitHub releases) — voir la note d'équivalence dans le header.
  if (parseHexDigest(digestStr, out)) {
    return OtaSha256ParamVerdict::kParsed;
  }
  if (parseSha256Digest(digestStr, out)) {
    return OtaSha256ParamVerdict::kParsed;
  }
  return OtaSha256ParamVerdict::kMalformed;
}

OtaUploadIntegrityVerdict otaUploadIntegrityVerdict(OtaSha256ParamVerdict paramVerdict,
                                                    const uint8_t expected[32],
                                                    const uint8_t computed[32]) {
  switch (paramVerdict) {
    case OtaSha256ParamVerdict::kAbsent:
      return OtaUploadIntegrityVerdict::kAbsent;
    case OtaSha256ParamVerdict::kMalformed:
      return OtaUploadIntegrityVerdict::kMalformed;
    case OtaSha256ParamVerdict::kParsed:
      return sha256Equal(expected, computed) ? OtaUploadIntegrityVerdict::kConform
                                              : OtaUploadIntegrityVerdict::kNonConform;
  }
  return OtaUploadIntegrityVerdict::kAbsent;  // Inatteignable — silence un warning -Wreturn-type
}

const char* otaUploadIntegrityVerdictLabel(OtaUploadIntegrityVerdict verdict) {
  switch (verdict) {
    case OtaUploadIntegrityVerdict::kAbsent:
      return "absente";
    case OtaUploadIntegrityVerdict::kMalformed:
      return "malformée";
    case OtaUploadIntegrityVerdict::kConform:
      return "conforme";
    case OtaUploadIntegrityVerdict::kNonConform:
      return "non conforme";
  }
  return "absente";  // Inatteignable — silence un warning -Wreturn-type
}

bool otaUploadIntegrityIsRejected(OtaUploadIntegrityVerdict verdict) {
  return verdict != OtaUploadIntegrityVerdict::kConform;
}
