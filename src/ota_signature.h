#ifndef OTA_SIGNATURE_H
#define OTA_SIGNATURE_H

// =============================================================================
// Vérification cryptographique de la signature OTA (feature-151 D1, décision
// Q1 du verdict security-reviewer P1) : RSA-3072, PKCS#1 v1.5 sur SHA-256,
// via mbedtls_pk_verify — clé publique EMBARQUÉE (src/constants.h,
// kOtaSigningPublicKeyDer). NON testable en natif (mbedtls indisponible côté
// hôte) : la logique pure (grammaire du manifeste, base64, plancher de
// version, combinaison du verdict) vit dans ota_manifest_logic.*,
// ota_base64_logic.* et ota_authenticity_logic.*.
//
// ⚠️ Temps constant SANS OBJET ici (point établi par l'audit P1) : la
// vérification se fait à CLÉ PUBLIQUE, aucun secret ne circule côté
// vérificateur — ne pas réintroduire cette exigence par réflexe.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#include "ota_authenticity_logic.h"  // OtaCryptoVerdict

/**
 * @brief Vérifie une signature RSA-3072/PKCS#1 v1.5/SHA-256 sur `data`.
 *
 * Hache `data` (SHA-256, via OtaStreamHasher — ota_integrity.h, déjà
 * compatible mbedtls 2.x/3.x) puis vérifie `signature` avec la clé publique
 * embarquée.
 *
 * @param data          Octets sur lesquels porte la signature — DOIVENT être
 *                       le manifeste RE-SÉRIALISÉ (serializeOtaManifest),
 *                       jamais la chaîne brute reçue telle quelle.
 * @param dataLen        Longueur de `data`.
 * @param signature      Signature brute (384 octets pour RSA-3072).
 * @param signatureLen   Longueur de `signature`.
 * @return kValid si la signature est authentique ; kInvalid si elle est
 *         cryptographiquement rejetée (clé différente, données altérées) ;
 *         kImpossible si l'outillage lui-même a échoué (clé embarquée
 *         inparsable, allocation mbedtls refusée) — DISTINCT de kInvalid
 *         (condition 9 du verdict security-reviewer P1).
 */
OtaCryptoVerdict otaVerifySignature(const uint8_t* data, size_t dataLen, const uint8_t* signature,
                                    size_t signatureLen);

#endif  // OTA_SIGNATURE_H
