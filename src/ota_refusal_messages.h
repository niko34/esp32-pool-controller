#ifndef OTA_REFUSAL_MESSAGES_H
#define OTA_REFUSAL_MESSAGES_H

// =============================================================================
// Refus d'une image OTA non authentique (feature-151 D3-c) — DÉCISION et PHRASE
// séparées PAR LE TYPE (ADR-0017 : module pur, testable en natif).
//
// ⭐ otaRefusalDecide() NE REÇOIT AUCUN paramètre de page : le marqueur `ui` envoyé par
// l'interface ne peut donc PAS atteindre la décision. Il ne change que la PHRASE
// (otaRefusalPhrase). Cette séparation est impossible à violer sans changer la signature.
//
// ⚠️ Les phrases sont des CONSTANTES mesurées et validées (la plus longue : 190 caractères,
// 199 OCTETS). Tout tampon qui les reçoit doit faire >= 256 octets : un char[200] tronquerait
// la phrase 8. Ne pas les raccourcir ni les réécrire.
// =============================================================================

#include <stdint.h>

#include "ota_authenticity_logic.h"

// Décision de refus. `code` : identifiant machine (JSON de /download-update seulement).
struct OtaRefusalDecision {
  bool rejected;      // true : l'envoi doit être refusé AVANT toute écriture
  int http;           // statut HTTP (0 si non refusé)
  const char* code;   // code JSON stable ("" si non refusé)
};

// Où le refus est prononcé : PRÉCOCE (rien n'a été écrit) ou TARDIF (l'écriture a commencé).
enum class OtaRefusalSite : uint8_t {
  kEarly = 0,           // avant toute écriture : « Rien n'a été modifié » est VRAI
  kLateFirmware = 1,    // liaison rompue après écriture firmware (image de secours perdue)
  kLateFilesystem = 2,  // liaison rompue après effacement de la partition interface
};

/**
 * @brief Décision de refus pour un verdict d'authenticité. Fonction du verdict SEUL.
 *        kValid -> non refusé. Table : kAbsent 403, kMalformed 400, kInvalid 403,
 *        kVerificationImpossible 503, kKindMismatch 415 (409 est pris par « envoi concurrent »),
 *        kBelowFloor 403 (inatteignable : plancher figé à 0.0.0).
 */
OtaRefusalDecision otaRefusalDecide(OtaAuthenticityVerdict verdict);

/**
 * @brief Phrase utilisateur constante (UTF-8, jamais nulle).
 * @param pageAnnouncedVersion true si le marqueur `ui` était PRÉSENT (page à jour) : ne change
 *        QUE la phrase de kAbsent (phrase 1 contre phrase 2 « page périmée »). Lu par présence,
 *        jamais comparé, jamais stocké, jamais journalisé.
 * @param site kEarly : phrases 1 à 6 selon le verdict (« Rien n'a été modifié » y est vrai).
 *        kLateFirmware / kLateFilesystem : phrases 7 / 8, `verdict` ignoré (jamais « Rien n'a
 *        été modifié » : l'écriture a commencé, défaut G1).
 */
const char* otaRefusalPhrase(OtaAuthenticityVerdict verdict, bool pageAnnouncedVersion,
                             OtaRefusalSite site);

#endif  // OTA_REFUSAL_MESSAGES_H
