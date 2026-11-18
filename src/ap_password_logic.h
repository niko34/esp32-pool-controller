#ifndef AP_PASSWORD_LOGIC_H
#define AP_PASSWORD_LOGIC_H

// =============================================================================
// ap_password_logic.h — fabrication et contrôle des mots de passe (bug-187)
// =============================================================================
// Logique PURE (ADR-0017) : aucun Arduino, aucune allocation, aucun journal.
// Testable nativement — c'est la seule façon de prouver l'absence de biais de
// tirage et le comportement des boucles de rejet, qu'aucun essai sur matériel
// ne montrerait.
//
// ⭐ DEUX RÔLES, DÉLIBÉRÉMENT ASYMÉTRIQUES — c'est le point à ne pas confondre :
//
//   1. FABRIQUER le mot de passe du point d'accès, celui qui part sur
//      l'ÉTIQUETTE. Il est tiré dans un alphabet RESTREINT (72 symboles), parce
//      qu'il sera recopié à la main puis tapé sur un téléphone. Sont exclus :
//        - `I` `l` `1` `O` `0` `o` — ils se confondent à la lecture ;
//        - `^` — ⛔ TOUCHE MORTE sur un clavier français : `^` puis `a` donne
//          `â`. Un client saisissant depuis un ordinateur échouerait sans
//          jamais comprendre pourquoi.
//
//   2. CONTRÔLER un mot de passe CHOISI par l'utilisateur. ⛔ Celui-là n'est PAS
//      restreint à l'alphabet ci-dessus : un gestionnaire de mots de passe
//      produit ce qu'il veut, et refuser un mot de passe solide parce qu'il
//      contient un `^` serait absurde. Le contrôle porte sur les FAMILLES
//      présentes, pas sur une liste de symboles autorisés.
//
// ⚠️ Confondre les deux donnerait soit une étiquette illisible, soit un refus
//    incompréhensible. Les deux jeux de constantes sont donc séparés.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

// --- Alphabet de FABRICATION (72 symboles) -----------------------------------
// ⚠️ Toute modification change l'entropie : la recalculer et la consigner.
inline const char* apPwdGenLower() { return "abcdefghijkmnpqrstuvwxyz"; }  // 24, sans l o
inline const char* apPwdGenUpper() { return "ABCDEFGHJKLMNPQRSTUVWXYZ"; }  // 24, sans I O
inline const char* apPwdGenDigit() { return "23456789"; }                  //  8, sans 0 1
inline const char* apPwdGenSpecial() { return "#?!@$%&*-'+()_[]"; }        // 16, sans ^

inline constexpr size_t kApPwdGenLowerN = 24;
inline constexpr size_t kApPwdGenUpperN = 24;
inline constexpr size_t kApPwdGenDigitN = 8;
inline constexpr size_t kApPwdGenSpecialN = 16;
inline constexpr size_t kApPwdGenAlphabetN =
    kApPwdGenLowerN + kApPwdGenUpperN + kApPwdGenDigitN + kApPwdGenSpecialN;  // 72

// Longueur du mot de passe FABRIQUÉ. 12 × log2(72) = 74,0 bits nominaux ;
// 73,5 bits réels une fois retirée la contrainte des quatre familles (le tirage
// par rejet n'accepte que 69,8 % des tirages, ce qui coûte 0,52 bit).
inline constexpr size_t kApPwdGenLength = 12;

// --- Bornes de SAISIE --------------------------------------------------------
// ⛔ WPA2 impose 8 caractères au minimum et 63 au maximum : en dessous de 8,
// `WiFi.softAP()` REFUSE la phrase (AP.cpp:217) — et une phrase VIDE crée un
// réseau OUVERT sans la moindre erreur (AP.cpp:236-240). Ces bornes ne sont donc
// pas du confort : elles empêchent un point d'accès sans chiffrement.
inline constexpr size_t kApPwdWpaFloor = 8;    // plancher DUR de WPA2 (AP.cpp:217)
inline constexpr size_t kApPwdMinLength = 12;  // > minimum WPA2 : elle s'attaque HORS LIGNE
inline constexpr size_t kApPwdMaxLength = 63;  // plafond WPA2

// ⛔ LA GARDE DU SITE D'APPEL `softAP()`, et pas seulement un message dans le journal.
// Défaut relevé en revue P2 : le code annonçait « le point d'acces NE DEMARRERA PAS »
// alors que RIEN ne l'en empêchait. Une phrase vide crée un réseau OUVERT, en silence,
// et un point d'accès ouvert sur un appareil vierge, c'est le droit anonyme de choisir
// le mot de passe administrateur offert à qui passe à portée radio.
inline bool apPwdUsableAsWpaKey(size_t n) { return n >= kApPwdWpaFloor && n <= kApPwdMaxLength; }

// --- Les quatre familles -----------------------------------------------------
enum ApPwdFamily : uint8_t {
  kApPwdFamilyLower = 1 << 0,
  kApPwdFamilyUpper = 1 << 1,
  kApPwdFamilyDigit = 1 << 2,
  kApPwdFamilySpecial = 1 << 3,
};
inline constexpr uint8_t kApPwdFamilyAll =
    kApPwdFamilyLower | kApPwdFamilyUpper | kApPwdFamilyDigit | kApPwdFamilySpecial;

// Classe UN caractère. ⛔ Volontairement large pour le CONTRÔLE (rôle 2) : tout
// caractère ASCII imprimable qui n'est ni lettre ni chiffre compte comme
// spécial, y compris l'espace. Rend 0 pour un caractère hors ASCII imprimable
// (32..126) — ⚠️ un octet ≥ 128 n'est PAS une famille, c'est un refus.
inline uint8_t apPwdFamilyOf(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u < 32 || u > 126) return 0;
  if (u >= 'a' && u <= 'z') return kApPwdFamilyLower;
  if (u >= 'A' && u <= 'Z') return kApPwdFamilyUpper;
  if (u >= '0' && u <= '9') return kApPwdFamilyDigit;
  return kApPwdFamilySpecial;
}

// Familles présentes dans `s` (n caractères). ⚠️ Rend 0xFF si un caractère est
// hors ASCII imprimable : l'appelant doit traiter ce cas comme un REFUS, jamais
// comme « toutes les familles sont là ».
inline constexpr uint8_t kApPwdFamiliesInvalid = 0xFF;

inline uint8_t apPwdFamiliesPresent(const char* s, size_t n) {
  if (s == nullptr) return kApPwdFamiliesInvalid;
  uint8_t seen = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t f = apPwdFamilyOf(s[i]);
    if (f == 0) return kApPwdFamiliesInvalid;
    seen = static_cast<uint8_t>(seen | f);
  }
  return seen;
}

// --- Verdict de contrôle d'un mot de passe CHOISI ----------------------------
// Un verdict NOMMÉ plutôt qu'un booléen : l'interface doit pouvoir dire CE QUI
// MANQUE pendant la frappe, et le firmware doit pouvoir journaliser la cause
// sans jamais recopier la valeur.
enum class ApPwdVerdict : uint8_t {
  kOk = 0,
  kTooShort,
  kTooLong,
  kBadCharacter,   // hors ASCII imprimable (32..126)
  kMissingFamily,  // au moins une des quatre familles manque
};

// ⭐ TROIS paramètres, et chacun répond à une contrainte DIFFÉRENTE — les confondre
// a déjà produit un défaut, relevé en revue de code :
//
//   `minLength`     L'appareil : 8, car le ralentissement de bug-189 le protège
//                   (1 essai/minute). La clé Wi-Fi : 12, car elle s'attaque HORS
//                   LIGNE après capture et qu'aucun ralentissement ne s'y applique.
//   `maxLength`     L'appareil : large. La clé Wi-Fi : 63, plafond DUR de WPA2.
//   `allowNonAscii` ⛔ LE PIÈGE. WPA2 ne transporte que de l'ASCII imprimable, donc
//                   la clé Wi-Fi doit le refuser. ⚠️ Mais le mot de passe de
//                   l'APPAREIL voyage en JSON : rien n'y interdit un accent, et le
//                   refuser serait absurde pour un utilisateur français — « Été2024! »
//                   est un bon mot de passe. Un octet non ASCII est alors TOLÉRÉ mais
//                   ne compte pour AUCUNE famille : il ne permet pas de contourner la
//                   règle avec « ééééééé ».
// ⛔ Les caractères de CONTRÔLE (< 32, et 127) restent refusés dans les deux cas :
//    invisibles, ils ne servent qu'à produire des pannes indiagnosticables.
inline ApPwdVerdict apPwdCheck(const char* s, size_t n, size_t minLength, size_t maxLength,
                               bool allowNonAscii) {
  if (s == nullptr) return ApPwdVerdict::kBadCharacter;
  if (n < minLength) return ApPwdVerdict::kTooShort;
  if (n > maxLength) return ApPwdVerdict::kTooLong;
  // ⛔ Espace en tête ou en fin : REFUSÉ dans les deux cas. WPA2 l'accepte, et c'est
  // précisément le problème — il est INVISIBLE. Collé depuis un gestionnaire ou laissé
  // par une sélection, il produit un refus que personne ne peut diagnostiquer, ni sur
  // l'étiquette ni à l'écran. Les espaces INTÉRIEURS restent licites.
  if (s[0] == ' ' || s[n - 1] == ' ') return ApPwdVerdict::kBadCharacter;
  uint8_t seen = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char u = static_cast<unsigned char>(s[i]);
    if (u < 32 || u == 127) return ApPwdVerdict::kBadCharacter;  // contrôle : jamais
    if (u > 126) {
      if (!allowNonAscii) return ApPwdVerdict::kBadCharacter;
      continue;  // toléré, mais ne compte pour aucune famille
    }
    seen = static_cast<uint8_t>(seen | apPwdFamilyOf(s[i]));
  }
  if ((seen & kApPwdFamilyAll) != kApPwdFamilyAll) return ApPwdVerdict::kMissingFamily;
  return ApPwdVerdict::kOk;
}

// --- Tirage SANS BIAIS -------------------------------------------------------
// ⛔ `octet % 72` serait BIAISÉ : 256 = 3×72 + 40, donc les 40 premiers symboles
// sortiraient 4 fois sur 256 et les 32 autres seulement 3 fois. Le biais est
// petit mais il est MESURABLE, et un attaquant qui le connaît réduit l'espace.
// ⇒ On REJETTE les octets ≥ 216 (= 3×72). Taux d'acceptation : 84,4 %.
inline constexpr uint8_t kApPwdByteLimit = 216;

inline bool apPwdByteAccepted(uint8_t b) { return b < kApPwdByteLimit; }

inline size_t apPwdIndexFromByte(uint8_t b) {
  return static_cast<size_t>(b % kApPwdGenAlphabetN);
}

// Le symbole d'un indice, dans l'ordre : minuscules, majuscules, chiffres,
// spéciaux. Rend '\0' si l'indice déborde — ⛔ jamais un caractère arbitraire.
inline char apPwdSymbolAt(size_t idx) {
  if (idx < kApPwdGenLowerN) return apPwdGenLower()[idx];
  idx -= kApPwdGenLowerN;
  if (idx < kApPwdGenUpperN) return apPwdGenUpper()[idx];
  idx -= kApPwdGenUpperN;
  if (idx < kApPwdGenDigitN) return apPwdGenDigit()[idx];
  idx -= kApPwdGenDigitN;
  if (idx < kApPwdGenSpecialN) return apPwdGenSpecial()[idx];
  return '\0';
}

// --- Fabrication -------------------------------------------------------------
// `nextByte` fournit l'aléa. ⛔ AUCUN fournisseur par défaut (condition C4 de la
// revue P1) : le seul fournisseur de production est `esp_random()`, et il doit
// être passé EXPLICITEMENT — un défaut silencieux finirait par servir en vrai.
//
// ⛔ BOUCLES BORNÉES, ÉCHEC EXPLICITE. Le danger n'est pas théorique : rendre
// une chaîne VIDE ferait démarrer le point d'accès SANS CHIFFREMENT, et
// `/wifi/connect` est ouvert sans jeton dès que l'appareil est en mode AP.
// ⇒ En cas d'échec, `out` est laissé VIDE et la fonction rend false ; l'appelant
//   DOIT conserver l'ancien mot de passe.
//
// Budgets : 69,8 % des tirages de 12 caractères portent les quatre familles,
// soit 1,43 tirage en moyenne. 64 tentatives laissent une marge de 10^-32.
inline constexpr size_t kApPwdMaxAttempts = 64;
inline constexpr size_t kApPwdMaxByteDraws = 512;  // ≈ 14 par caractère, 36× la moyenne

template <typename NextByte>
inline bool apPwdGenerate(NextByte nextByte, char* out, size_t outCapacity) {
  if (out == nullptr || outCapacity < kApPwdGenLength + 1) return false;
  out[0] = '\0';

  for (size_t attempt = 0; attempt < kApPwdMaxAttempts; ++attempt) {
    char buf[kApPwdGenLength + 1];
    size_t written = 0;
    size_t draws = 0;
    bool starved = false;

    while (written < kApPwdGenLength) {
      if (draws++ >= kApPwdMaxByteDraws) { starved = true; break; }
      const uint8_t b = nextByte();
      if (!apPwdByteAccepted(b)) continue;  // rejet : c'est CE rejet qui ôte le biais
      const char c = apPwdSymbolAt(apPwdIndexFromByte(b));
      if (c == '\0') { starved = true; break; }  // ⛔ ne peut pas arriver, on refuse quand même
      buf[written++] = c;
    }
    if (starved) break;  // fournisseur défaillant : inutile d'insister

    buf[kApPwdGenLength] = '\0';
    if (apPwdFamiliesPresent(buf, kApPwdGenLength) == kApPwdFamilyAll) {
      for (size_t i = 0; i <= kApPwdGenLength; ++i) out[i] = buf[i];
      return true;
    }
    // Rejet du mot de passe ENTIER. ⛔ Ne JAMAIS « corriger » en remplaçant un
    // caractère par un symbole de la famille manquante : la position du
    // correctif serait prévisible et l'entropie s'effondrerait.
  }

  out[0] = '\0';
  return false;
}

// --- Reconnaissance de l'ANCIENNE forme --------------------------------------
// Avant bug-187 : 8 caractères hexadécimaux en majuscules. ⚠️ Ce prédicat ne
// sert PAS à régénérer quoi que ce soit — l'utilisateur a tranché le 2026-11-18
// qu'aucun appareil installé ne serait régénéré, pour que l'étiquette ne mente
// jamais. Il sert à DIRE à son propriétaire que sa clé est faible et qu'il peut
// en choisir une autre.
inline bool apPwdIsLegacyForm(const char* s, size_t n) {
  if (s == nullptr || n != 8) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

#endif  // AP_PASSWORD_LOGIC_H
