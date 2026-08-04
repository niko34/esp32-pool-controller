#include "ota_integrity.h"

#include <mbedtls/version.h>

// Compat migration core 3 (feature-063) : mbedtls 3.x (IDF 5) a supprimé les
// variantes *_ret — les fonctions de base retournent désormais un int.
// Macros localisées à ce .cpp (seul consommateur de l'API sha256 streaming).
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
#define POOL_SHA256_STARTS(ctx, is224) mbedtls_sha256_starts((ctx), (is224))
#define POOL_SHA256_UPDATE(ctx, d, l)  mbedtls_sha256_update((ctx), (d), (l))
#define POOL_SHA256_FINISH(ctx, out)   mbedtls_sha256_finish((ctx), (out))
#else
#define POOL_SHA256_STARTS(ctx, is224) mbedtls_sha256_starts_ret((ctx), (is224))
#define POOL_SHA256_UPDATE(ctx, d, l)  mbedtls_sha256_update_ret((ctx), (d), (l))
#define POOL_SHA256_FINISH(ctx, out)   mbedtls_sha256_finish_ret((ctx), (out))
#endif

OtaStreamHasher::OtaStreamHasher() {
  mbedtls_sha256_init(&ctx_);
}

OtaStreamHasher::~OtaStreamHasher() {
  mbedtls_sha256_free(&ctx_);
}

void OtaStreamHasher::begin() {
  // free + init : réarme proprement le contexte, ce qui permet de réutiliser
  // la même instance pour une nouvelle tentative (upload manuel /update).
  mbedtls_sha256_free(&ctx_);
  mbedtls_sha256_init(&ctx_);
  // is224 = 0 → SHA-256. Macro compat mbedtls 2.28 (*_ret) / 3.x.
  POOL_SHA256_STARTS(&ctx_, 0);
}

void OtaStreamHasher::update(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) {
    return;
  }
  POOL_SHA256_UPDATE(&ctx_, data, len);
}

void OtaStreamHasher::finish(uint8_t out[32]) {
  POOL_SHA256_FINISH(&ctx_, out);
}
