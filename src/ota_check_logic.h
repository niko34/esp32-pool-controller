#ifndef OTA_CHECK_LOGIC_H
#define OTA_CHECK_LOGIC_H

// =============================================================================
// Logique PURE du parsing borné de /check-update (bug-checkupdate-heap-tls,
// ADR-0032). Header sans dépendance Arduino/ArduinoJson → testable en natif
// (ADR-0017). Consommé par src/web_routes_ota.cpp :
//   - AllocBudget       : plafond NET d'allocations pour l'allocateur
//                         ArduinoJson personnalisé — admit() réserve,
//                         release() re-crédite (revue 2.16.1 : ArduinoJson
//                         fait croître ses chaînes par doublements successifs
//                         puis rétrécit ; un comptage monotone facturerait
//                         ~4× la taille utile ET pourrait refuser un realloc
//                         de rétrécissement, que la lib suppose infaillible) ;
//   - utf8SafeCutLen    : point de coupe des release notes qui ne scinde
//                         jamais un caractère UTF-8 multi-octets (accents,
//                         emojis des notes en français).
//   - classifyOtaCheckFailure : étape à laquelle un échec `/check-update` a
//                         eu lieu (bug-165) — un code HTTPClient négatif
//                         (ex. -1) couvre À LA FOIS un TCP refusé/DNS et un
//                         échec de handshake TLS ; sans distinction la
//                         réponse HTTP reste muette alors que l'appelant
//                         (WiFiClientSecure::lastError) connaît déjà la
//                         cause précise.
// =============================================================================

#include <stddef.h>

// Plafond NET d'allocations. Invariant : used <= cap.
struct AllocBudget {
  size_t cap;
  size_t used;

  explicit AllocBudget(size_t capBytes) : cap(capBytes), used(0) {}

  // true si `n` octets supplémentaires tiennent dans le budget (et les réserve).
  // Écrit pour être insensible au débordement : compare à l'espace restant.
  bool admit(size_t n) {
    if (n > cap - used) return false;
    used += n;
    return true;
  }

  // Re-crédite `n` octets (bloc libéré ou rétréci). Défensif : borné à `used`
  // pour préserver l'invariant même sur un crédit incohérent.
  void release(size_t n) {
    used = (n <= used) ? used - n : 0;
  }
};

// Longueur de coupe sûre : la plus grande longueur <= maxLen qui ne scinde pas
// un caractère UTF-8. Les octets de continuation UTF-8 valent 0b10xxxxxx : on
// recule tant que l'octet à la position de coupe en est un, pour retomber sur
// un début de caractère (la coupe se fait AVANT lui).
inline size_t utf8SafeCutLen(const char* s, size_t len, size_t maxLen) {
  if (len <= maxLen) return len;
  size_t cut = maxLen;
  while (cut > 0 &&
         (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return cut;
}

// Étape à laquelle un échec `/check-update` a eu lieu, pour que la réponse
// HTTP NOMME la cause au lieu d'un code numérique opaque (bug-165).
enum class OtaCheckFailureStep {
  kTlsHandshake,  // échec du handshake TLS (certificat, chaîne de confiance...)
  kConnection,    // httpCode == -1 SANS erreur TLS : résolution de nom ou TCP refusé
  kHttpOther,     // tout autre code HTTPClient négatif (délai dépassé, encodage...)
};

// Classifie un échec à partir du code HTTPClient (`httpCode`, toujours < 0
// ici) et du code d'erreur TLS éventuel (`tlsErrorCode`, 0 si absent — valeur
// renvoyée par `WiFiClientSecure::lastError()`). Pure, sans dépendance
// Arduino : testable en natif et par mutation.
//
// ⚠️ httpCode == -1 (HTTPC_ERROR_CONNECTION_REFUSED) est ambigu par
// construction : WiFiClientSecure::connect() renvoie 0 aussi bien pour un TCP
// refusé/DNS en échec QUE pour un handshake TLS avorté — HTTPClient ne fait
// pas la différence. tlsErrorCode est le seul signal qui lève l'ambiguïté.
inline OtaCheckFailureStep classifyOtaCheckFailure(int httpCode, int tlsErrorCode) {
  if (tlsErrorCode != 0) {
    return OtaCheckFailureStep::kTlsHandshake;
  }
  if (httpCode == -1) {
    return OtaCheckFailureStep::kConnection;
  }
  return OtaCheckFailureStep::kHttpOther;
}

#endif  // OTA_CHECK_LOGIC_H
