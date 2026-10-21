#include "ota_signature.h"

#include <mbedtls/bignum.h>
#include <mbedtls/pk.h>

#include "constants.h"
#include "ota_integrity.h"  // OtaStreamHasher — réutilisé (déjà compatible mbedtls 2.x/3.x)

OtaCryptoVerdict otaVerifySignature(const uint8_t* data, size_t dataLen, const uint8_t* signature,
                                    size_t signatureLen) {
  if (signature == nullptr || signatureLen == 0) {
    return OtaCryptoVerdict::kImpossible;
  }

  // Hachage SHA-256 des octets CANONIQUES (re-sérialisés par l'appelant) —
  // réutilise OtaStreamHasher plutôt que de dupliquer les macros de
  // compatibilité mbedtls 2.x/3.x déjà centralisées dans ota_integrity.cpp.
  uint8_t hash[32];
  OtaStreamHasher hasher;
  hasher.begin();
  hasher.update(data, dataLen);
  hasher.finish(hash);

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);

  // Clé publique EMBARQUÉE au build (constants.h) — ne devrait JAMAIS échouer
  // à parser, mais fail-closed plutôt que supposer : condition 9 du verdict
  // security-reviewer P1 (« vérification impossible », distincte d'une
  // signature simplement invalide).
  int rc = mbedtls_pk_parse_public_key(&pk, kOtaSigningPublicKeyDer, kOtaSigningPublicKeyDerLen);
  if (rc != 0) {
    mbedtls_pk_free(&pk);
    return OtaCryptoVerdict::kImpossible;
  }

  rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, signatureLen);
  mbedtls_pk_free(&pk);

  if (rc == 0) {
    return OtaCryptoVerdict::kValid;
  }
  // Distinction fine (condition 9) : un échec d'ALLOCATION pendant la
  // vérification (tas interne épuisé, par exemple) n'est PAS la même chose
  // qu'une signature authentiquement invalide — le premier cas doit pouvoir
  // être désigné comme un problème d'OUTILLAGE/ressources, pas une
  // falsification, sous peine de masquer un tas dégradé derrière un message
  // qui accuserait à tort la clé ou l'image.
  if (rc == MBEDTLS_ERR_MPI_ALLOC_FAILED || rc == MBEDTLS_ERR_PK_ALLOC_FAILED) {
    return OtaCryptoVerdict::kImpossible;
  }
  return OtaCryptoVerdict::kInvalid;
}
