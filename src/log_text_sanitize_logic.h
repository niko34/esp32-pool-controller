#ifndef LOG_TEXT_SANITIZE_LOGIC_H
#define LOG_TEXT_SANITIZE_LOGIC_H

// =============================================================================
// log_text_sanitize_logic — assainissement PUR d'un texte EXTERNE avant journal
// =============================================================================
// bug-179 : un client non authentifié choisit l'URL (ex. /get-config/<b>X</b>),
// un client MQTT choisit la charge, un point d'accès choisit son SSID. Ces textes
// finissaient bruts dans le journal, que l'interface affiche : injection.
//
// RÈGLE : liste blanche POSITIVE recopiée telle quelle
//   A-Z a-z 0-9 espace - . _ ~ / : ? = @ , ; + ( ) *
// tout le reste -> %XX hexa MAJUSCULE (dont '%', '<', '>', '&', '"', '\'', '\\',
// octets < 0x20, 0x7F et TOUT octet >= 0x80).
// '%' étant lui-même échappé en %25, la fonction est INJECTIVE : la charge se
// reconstitue à l'identique depuis le journal (valeur forensique).
//
// DÉBORDEMENT : coupe sur une frontière d'échappement (jamais au milieu d'un %XX)
// puis suffixe "~<srcLen>o" portant la longueur d'ORIGINE.
// ⚠️ RÉSERVE, relevée par l'audit P2 : '~' est en liste blanche, donc un texte
//    qui finit lui-même par "~12o" IMITE le suffixe de troncature. Sans gravité
//    (aucune exécution, aucune perte de bornage) mais l'injectivité annoncée vaut
//    pour une sortie NON TRONQUÉE : au-delà, seul le préfixe est reconstructible.
//    ⛔ Ne pas retirer '~' de la liste blanche pour « corriger » : il sert aux URL.
//
// ZÉRO allocation (ADR-0082) : tampon fourni par l'appelant, sur la pile.
// Aucune dépendance Arduino : compilable en natif.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

namespace log_sanitize_detail {
inline bool isPlain(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case ' ': case '-': case '.': case '_': case '~': case '/': case ':': case '?':
    case '=': case '@': case ',': case ';': case '+': case '(': case ')': case '*':
      return true;
    default:
      return false;
  }
}
inline size_t encLen(unsigned char c) { return isPlain(c) ? 1 : 3; }
}  // namespace log_sanitize_detail

/**
 * Assainit src[0..srcLen) vers out (NUL-terminé TOUJOURS si outCap >= 1).
 * @return nombre d'octets écrits, NUL exclu.
 */
inline size_t logSanitizeExternalText(const char* src, size_t srcLen, char* out, size_t outCap) {
  using namespace log_sanitize_detail;
  if (out == nullptr || outCap == 0) return 0;
  if (src == nullptr) srcLen = 0;
  static const char kHex[] = "0123456789ABCDEF";
  const size_t room = outCap - 1;

  // Passe 1 : la sortie complète tient-elle ?
  size_t total = 0;
  for (size_t i = 0; i < srcLen && total <= room; i++) total += encLen(static_cast<unsigned char>(src[i]));
  const bool fits = (total <= room);

  // Suffixe "~<srcLen>o" (chiffres calculés sans snprintf).
  char suffix[16];
  size_t sl = 0;
  if (!fits) {
    char digits[12];
    size_t nd = 0;
    size_t v = srcLen;
    do { digits[nd++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v > 0 && nd < sizeof(digits));
    suffix[sl++] = '~';
    while (nd > 0) suffix[sl++] = digits[--nd];
    suffix[sl++] = 'o';
    if (sl > room) sl = 0;  // tampon minuscule : pas de place pour le suffixe
  }
  const size_t limit = fits ? room : room - sl;

  size_t w = 0;
  for (size_t i = 0; i < srcLen; i++) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    const size_t n = encLen(c);
    if (w + n > limit) break;  // frontière d'échappement : jamais de %XX coupé
    if (n == 1) {
      out[w++] = static_cast<char>(c);
    } else {
      out[w++] = '%';
      out[w++] = kHex[c >> 4];
      out[w++] = kHex[c & 0x0F];
    }
  }
  for (size_t k = 0; k < sl; k++) out[w++] = suffix[k];
  out[w] = '\0';
  return w;
}

#endif  // LOG_TEXT_SANITIZE_LOGIC_H
