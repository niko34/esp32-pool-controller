#ifndef OTA_MANIFEST_BINDING_LOGIC_H
#define OTA_MANIFEST_BINDING_LOGIC_H

// =============================================================================
// Liaison PURE manifeste ↔ image (feature-151 D1, lot B1 — revue de sécurité
// du 2026-10-18). Header sans dépendance Arduino/mbedtls → testable en natif
// (ADR-0017).
//
// ⚠️ CE QUE combineOtaAuthenticityVerdict() (ota_authenticity_logic.h) NE
// PROUVE PAS : qu'un manifeste signé par la bonne clé DÉCRIT CETTE image
// précise. Un manifeste authentiquement signé pour l'image A, envoyé avec
// l'image B et un paramètre sha256/digest=<B>, obtient un verdict
// d'authenticité « valide » alors que la signature n'autorise AUCUNE image en
// particulier (B1). Ce module comble CE trou, en comparant l'empreinte
// DÉCLARÉE par le manifeste (OtaManifestFields::sha256Hex, 64 hex minuscules
// + NUL) à une empreinte 32 octets obtenue INDÉPENDAMMENT :
//   - le paramètre sha256 (/update) ou digest (/download-update) déjà fourni
//     par l'appelant ;
//   - l'empreinte RÉELLEMENT calculée sur les octets écrits en flash.
//
// ⚠️ Le décodage hex→binaire du champ du manifeste est fait ICI (en
// réutilisant parseOtaSha256Param, ota_integrity_logic.h — même format, sans
// préfixe), PAS dans web_routes_ota.cpp : la garde structurelle
// test_native_ota_order_guard (G1, feature-118) exige que l'appel à
// parseOtaSha256Param() reste un site UNIQUE dans web_routes_ota.cpp — en
// dupliquer l'appel pour un second usage y romprait cette garde sans y
// apporter d'invariant supplémentaire.
//
// ⛔ JOURNALISE SEULEMENT (D1) : comme ota_authenticity_logic.h, ce verdict ne
// produit AUCUN refus — voir web_routes_ota.cpp, logOtaManifestBindingVerdict.
// D3 (hors périmètre) sera le premier appelant à conditionner un refus dessus.
// =============================================================================

#include <stdint.h>

enum class OtaManifestBindingVerdict : uint8_t {
  kSkipped = 0,  // Rien à comparer (manifeste non authentifiable, décodage impossible, ou empreinte de référence indisponible)
  kBound = 1,    // L'empreinte déclarée par le manifeste correspond à l'empreinte comparée
  kUnbound = 2,  // Divergence : la signature ne lie PAS cette image précise
};

/**
 * @brief Compare l'empreinte déclarée par un manifeste PARSÉ à une empreinte
 *        obtenue indépendamment.
 *
 * @param manifestFieldsValid true si `manifestSha256Hex` provient d'un
 *        manifeste qui a PASSÉ parseOtaManifest (kOk) — sinon kSkipped.
 * @param manifestSha256Hex   64 hex minuscules + NUL (OtaManifestFields::
 *        sha256Hex), valide seulement si `manifestFieldsValid` (peut être
 *        nullptr sinon).
 * @param comparedDigestValid true si `comparedDigest` est disponible
 *        (paramètre fourni et décodé, ou empreinte réellement calculée).
 * @param comparedDigest      32 octets bruts à comparer, valides seulement si
 *        `comparedDigestValid`.
 * @return kSkipped si l'une des deux empreintes est indisponible (ou si, par
 *         défense, le décodage du hex échouait — ne devrait jamais survenir
 *         puisque parseOtaManifest a déjà validé ce format strictement),
 *         sinon kBound ou kUnbound selon la comparaison (temps constant, via
 *         sha256Equal — ota_integrity_logic.h).
 */
OtaManifestBindingVerdict otaManifestBindingVerdict(bool manifestFieldsValid,
                                                    const char* manifestSha256Hex,
                                                    bool comparedDigestValid,
                                                    const uint8_t comparedDigest[32]);

// true si ce verdict DOIT être refusé (kUnbound uniquement — kSkipped n'est
// pas une anomalie, juste une comparaison non applicable). ⛔ En D1, sert
// UNIQUEMENT à choisir le niveau de journal (warning vs info).
bool otaManifestBindingIsDivergent(OtaManifestBindingVerdict verdict);

// Libellé FR stable (journalisation), jamais nul :
// "non applicable" | "liée" | "NON liée".
const char* otaManifestBindingVerdictLabel(OtaManifestBindingVerdict verdict);

#endif  // OTA_MANIFEST_BINDING_LOGIC_H
