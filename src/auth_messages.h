#ifndef AUTH_MESSAGES_H
#define AUTH_MESSAGES_H

// =============================================================================
// Phrases des refus d'authentification (bug-200, v2.98.0) — module PUR, header-only
// (ADR-0017, patron de ota_refusal_messages.h) : aucune dépendance Arduino, donc
// testable en natif. Ce sont des CHAÎNES : aucun seuil, aucun verdict, aucune garde.
//
// Style du fichier d'origine (web_routes_auth.cpp) : français SANS accents.
// ⛔ Ne jamais recopier le mot de passe reçu dans une phrase.
// ⚠️ Les trois « Invalid JSON » des routes restent EN ANGLAIS (décision utilisateur) :
// ils ne s'affichent que sur requête malformée et servent au diagnostic d'un outil.
// ⚠️ « Authentification requise » (kDenyUnauth) n'est jamais affichée : authFetch
// redirige vers /login.html?expired=1 avant de lire le corps (data/app.js).
// =============================================================================

#include "ap_password_logic.h"
#include "auth_bootstrap_logic.h"

/**
 * @brief Phrase de refus de /auth/ap-password (clé du point d'accès) pour un verdict.
 *        kOk -> "" (pas de refus). « famille manquante » dit « du point d'acces » :
 *        elle diffère volontairement de celle de /auth/change-password.
 */
inline const char* apPwdMessage(ApPwdVerdict v) {
  switch (v) {
    case ApPwdVerdict::kOk:
      return "";
    case ApPwdVerdict::kTooShort:
      return "Le mot de passe du point d'acces doit faire au moins 12 caracteres";
    case ApPwdVerdict::kTooLong:
      return "Le mot de passe du point d'acces ne peut pas depasser 63 caracteres";
    case ApPwdVerdict::kBadCharacter:
      return "Le mot de passe du point d'acces n'accepte ni accent, ni caractere "
             "invisible, ni espace en debut ou en fin";
    case ApPwdVerdict::kMissingFamily:
    default:
      return "Le mot de passe du point d'acces doit contenir au moins une minuscule, "
             "une majuscule, un chiffre et un caractere special";
  }
}

/**
 * @brief Phrase de refus de /auth/change-password (mot de passe de l'appareil).
 *        `newVerdict` n'est lu que pour kDenyWeak (il dit CE qui manque).
 *        kAllow -> "".
 */
inline const char* pwdChangeMessage(PwdChangeVerdict v, ApPwdVerdict newVerdict) {
  switch (v) {
    case PwdChangeVerdict::kAllow:
      return "";
    case PwdChangeVerdict::kDenyUnauth:
      return "Authentification requise";
    case PwdChangeVerdict::kDenyBadCurrent:
      return "Mot de passe actuel incorrect";
    case PwdChangeVerdict::kDenyTooShort:
      return "Le nouveau mot de passe doit faire au moins 8 caracteres";
    case PwdChangeVerdict::kDenyWeak:
      if (newVerdict == ApPwdVerdict::kTooLong) {
        return "Le mot de passe ne peut pas depasser 128 caracteres";
      }
      if (newVerdict == ApPwdVerdict::kBadCharacter) {
        return "Le mot de passe n'accepte ni caractere invisible, ni espace "
               "en debut ou en fin";
      }
      return "Le mot de passe doit contenir au moins une minuscule, une majuscule, "
             "un chiffre et un caractere special";
    case PwdChangeVerdict::kDenySame:
    default:
      return "Le nouveau mot de passe doit etre different de l'actuel";
  }
}

#endif  // AUTH_MESSAGES_H
