#ifndef OTA_AUTHENTICITY_LOGIC_H
#define OTA_AUTHENTICITY_LOGIC_H

// =============================================================================
// Combinateur PUR du verdict d'AUTHENTICITÉ OTA (feature-151 D1, verdict
// security-reviewer P1, condition 9). Header sans dépendance
// Arduino/mbedtls → testable en natif (ADR-0017).
//
// ⛔ D1 N'APPELLE JAMAIS otaAuthenticityIsRejected() POUR REFUSER — il ne
// fait que choisir le NIVEAU de journalisation. D3 (hors périmètre de ce
// lot) sera le PREMIER appelant à conditionner un refus sur ce verdict — le
// combinateur lui-même ne change pas d'une virgule entre D1 et D3
// (condition 10 : le diff D1→D3 ne touche QUE le prédicat de décision).
//
// La vérification cryptographique elle-même (mbedtls, non testable en natif)
// vit dans src/ota_signature.*. Son résultat est injecté ici en PARAMÈTRE
// (OtaCryptoVerdict) pour que ce combinateur reste pur et mutable par tests.
// =============================================================================

#include <stdint.h>

#include "ota_manifest_logic.h"

// Verdict final D'AUTHENTICITÉ — SIX issues de refus DISTINCTES (condition 9)
// + une issue de succès. Chacune doit être journalisée séparément : sans
// cette granularité, D2/D4 ne sauraient pas distinguer un défaut d'outillage
// (kVerificationImpossible), une clé différente (kInvalid), un retour en
// arrière (kBelowFloor) ou une simple absence (kAbsent).
enum class OtaAuthenticityVerdict : uint8_t {
  kAbsent = 0,                  // manifeste ou signature non fournis par l'appelant
  kMalformed = 1,               // manifeste hors grammaire, ou signature base64 invalide
  kVerificationImpossible = 2,  // échec de l'outillage crypto (allocation, clé inparsable)
  kInvalid = 3,                 // signature cryptographiquement invalide (clé différente, altération)
  kKindMismatch = 4,            // genre signé par le manifeste != genre attendu par l'appelant
  kBelowFloor = 5,              // version signée strictement inférieure au plancher
  // ⚠️ kValid prouve que LE MANIFESTE a été signé par la bonne clé et décrit
  // le bon genre/une version acceptable — PAS que l'image reçue est CELLE que
  // le manifeste décrit. Cette liaison manifeste ↔ octets réels est un verdict
  // SÉPARÉ (voir OtaManifestBindingVerdict, ota_manifest_binding_logic.h,
  // B1) : un appelant qui ne consulte QUE ce verdict-ci ne doit pas conclure
  // à l'authenticité de l'image elle-même (revue de sécurité, 2026-10-18 —
  // le libellé promettait auparavant « authenticité et cohérence intégrales »,
  // ce qui était faux tant que la liaison n'était vérifiée nulle part).
  kValid = 6,                   // signature + genre + plancher valides (hors liaison à l'image)
};

// Résultat de la vérification cryptographique SEULE — calculé ailleurs
// (src/ota_signature.cpp, mbedtls) et transmis ici en paramètre.
enum class OtaCryptoVerdict : uint8_t {
  kValid = 0,
  kInvalid = 1,
  kImpossible = 2,
};

/**
 * @brief Combine les verdicts partiels en UN verdict d'authenticité final.
 *
 * Ordre de priorité FIXE (absent > malformé > crypto > genre > plancher >
 * valide) : `cryptoVerdict`, `parsedKind`, `major`/`minor`/`patch` ne sont
 * consultés QUE si manifestPresent && signaturePresent &&
 * parseVerdict == kOk && signatureDecodeOk (sinon la signature n'a jamais pu
 * être calculée, et ces champs ne sont pas garantis renseignés par l'appelant).
 *
 * @param manifestPresent   Le paramètre manifeste a été fourni par l'appelant.
 * @param signaturePresent  Le paramètre signature a été fourni par l'appelant.
 * @param parseVerdict      Verdict de parseOtaManifest() sur le manifeste reçu.
 * @param signatureDecodeOk true si le décodage base64 + la longueur de la
 *                          signature (384 o, RSA-3072) sont conformes.
 * @param cryptoVerdict     Verdict de otaVerifySignature() (ota_signature.h).
 * @param parsedKind        Genre extrait du manifeste (valide seulement si parseVerdict==kOk).
 * @param expectedKind      Genre attendu par la route appelante (type d'upload/URL).
 * @param major,minor,patch Version extraite du manifeste (valide seulement si parseVerdict==kOk).
 * @param floorMajor,floorMinor,floorPatch Plancher anti-retour en arrière (D1 : figé à 0.0.0).
 */
OtaAuthenticityVerdict combineOtaAuthenticityVerdict(
    bool manifestPresent, bool signaturePresent, OtaManifestParseVerdict parseVerdict,
    bool signatureDecodeOk, OtaCryptoVerdict cryptoVerdict, OtaManifestKind parsedKind,
    OtaManifestKind expectedKind, uint32_t major, uint32_t minor, uint32_t patch,
    uint32_t floorMajor, uint32_t floorMinor, uint32_t floorPatch);

// true si ce verdict DOIT être refusé (tout sauf kValid). ⛔ En D1, cette
// fonction sert UNIQUEMENT à choisir le niveau de journal (warning vs info) —
// jamais à produire un `return` de refus. Voir l'avertissement d'en-tête.
bool otaAuthenticityIsRejected(OtaAuthenticityVerdict verdict);

// Libellé FR stable (journalisation), jamais nul : "absente" | "malformée" |
// "vérification impossible" | "invalide" | "genre discordant" |
// "sous le plancher" | "valide".
const char* otaAuthenticityVerdictLabel(OtaAuthenticityVerdict verdict);

#endif  // OTA_AUTHENTICITY_LOGIC_H
