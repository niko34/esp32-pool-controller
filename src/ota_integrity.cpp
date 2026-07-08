#include "ota_integrity.h"

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
  // API mbedtls 2.28 (IDF 4.4) : variantes *_ret. is224 = 0 → SHA-256.
  mbedtls_sha256_starts_ret(&ctx_, 0);
}

void OtaStreamHasher::update(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) {
    return;
  }
  mbedtls_sha256_update_ret(&ctx_, data, len);
}

void OtaStreamHasher::finish(uint8_t out[32]) {
  mbedtls_sha256_finish_ret(&ctx_, out);
}
