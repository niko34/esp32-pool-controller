#ifndef OTA_DIGEST_SOURCE_LOGIC_H
#define OTA_DIGEST_SOURCE_LOGIC_H

// =============================================================================
// Choix PUR de la source de l'empreinte SHA-256 attendue (feature-151 D3-a,
// condition 3 : source unique). Header-only, aucune dépendance → testable en
// natif (ADR-0017).
//
// L'empreinte du MANIFESTE fait autorité seulement si l'authenticité est
// valide (kValid) ET que son hex est exploitable ; sinon on garde le paramètre
// de l'appelant (comportement historique). `divergent` signale que les deux
// empreintes sont disponibles et DIFFÈRENT (comparaison d'hex INSENSIBLE à la
// casse), indépendamment de la source retenue.
// =============================================================================

#include <stdint.h>

enum class OtaExpectedDigestSource : uint8_t {
  kCallerParam = 0,
  kManifest = 1,
};

struct OtaDigestSourceDecision {
  OtaExpectedDigestSource source;
  bool divergent;
};

namespace ota_digest_source_detail {
constexpr unsigned kHexLen = 64;
inline char lower(char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }
// Égalité des deux chaînes de 64 hex, insensible à la casse. Chaîne plus
// courte/plus longue ou nullptr → false.
inline bool hexEqualIgnoreCase(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  for (unsigned i = 0; i < kHexLen; ++i) {
    if (a[i] == '\0' || b[i] == '\0') return false;
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return a[kHexLen] == '\0' && b[kHexLen] == '\0';
}
}  // namespace ota_digest_source_detail

inline OtaDigestSourceDecision chooseOtaExpectedDigestSource(bool authenticityValid,
                                                             bool manifestDigestValid,
                                                             const char* manifestHex,
                                                             bool callerDigestValid,
                                                             const char* callerHex) {
  OtaDigestSourceDecision d{OtaExpectedDigestSource::kCallerParam, false};
  const bool manifestUsable = manifestDigestValid && manifestHex != nullptr;
  if (authenticityValid && manifestUsable) {
    d.source = OtaExpectedDigestSource::kManifest;
  }
  if (manifestUsable && callerDigestValid && callerHex != nullptr) {
    d.divergent = !ota_digest_source_detail::hexEqualIgnoreCase(manifestHex, callerHex);
  }
  return d;
}

#endif  // OTA_DIGEST_SOURCE_LOGIC_H
