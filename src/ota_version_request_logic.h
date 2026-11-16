#ifndef OTA_VERSION_REQUEST_LOGIC_H
#define OTA_VERSION_REQUEST_LOGIC_H

// =============================================================================
// Liaison « genre + version » — logique PURE (feature-151 D3-b2, ADR-0017)
// =============================================================================
// Compare la version DÉCLARÉE par le manifeste signé à la version que la requête
// est CENSÉE porter. ⛔ AUCUNE décision d'acceptation ni de refus n'en dépend dans ce
// lot : le résultat n'est que JOURNALISÉ.
//
// ⚠️ La référence vient du tag mémorisé par /check-update, donc de GitHub — la chaîne
// dont la feature se défie : ce n'est PAS un contrôle d'authenticité. Elle est
// aussi périmée après un envoi direct (ota_update.sh) : un « désaccord » peut être
// un faux positif. À ne promouvoir en refus qu'avec une référence indépendante.
//
// Fichier distinct de ota_manifest_logic.cpp (grammaire) : celui-ci reste intact.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

enum class OtaVersionAgreement : uint8_t {
  kNoReference = 0,         // rien à comparer (silencieux)
  kReferenceUnparsable = 1, // référence présente mais illisible
  kAgree = 2,
  kDisagree = 3
};

struct OtaParsedVersion {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
};

/**
 * @brief Parse STRICT « v2.89.0 » ou « 2.89.0 » : trois entiers décimaux (1 à 6 chiffres)
 *        séparés par des points, rien après. false sinon (sorties intactes).
 */
bool otaParseVersionTag(const char* tag, uint32_t* maj, uint32_t* min, uint32_t* pat);

/**
 * @brief Choisit la référence : le tag MÉMORISÉ prime TOUJOURS ; le paramètre facultatif
 *        `expected_version` ne sert que si AUCUN tag n'est mémorisé (jamais prioritaire, C9).
 *        Rend nullptr si aucune référence. `fromParam` indique l'origine.
 */
const char* otaSelectVersionReference(const char* storedTag, const char* paramValue,
                                      bool* fromParam);

/**
 * @brief Compare. `refOut` (facultatif) reçoit les entiers PARSÉS de la référence (kAgree /
 *        kDisagree seulement) : c'est d'eux que le journal doit être re-sérialisé, jamais de
 *        la chaîne brute (C9). Sans champs de manifeste valides : kNoReference.
 */
OtaVersionAgreement otaCompareRequestedVersion(bool haveReference, const char* referenceTag,
                                               bool manifestFieldsValid, uint32_t maj,
                                               uint32_t min, uint32_t pat,
                                               OtaParsedVersion* refOut);

/** @brief Écrit « M.m.p » dans `out` (tronque proprement, toujours NUL-terminé). */
void otaFormatVersion(char* out, size_t cap, uint32_t maj, uint32_t min, uint32_t pat);

#endif  // OTA_VERSION_REQUEST_LOGIC_H
