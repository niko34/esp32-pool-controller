#ifndef OTA_INTEGRITY_H
#define OTA_INTEGRITY_H

// Hacheur SHA-256 incrémental pour les flux OTA (feature-026).
// Enveloppe fine autour de mbedtls_sha256 (accélération matérielle ESP32
// via le port mbedtls de l'ESP-IDF). NON testable en natif — la logique
// pure (parse/compare/format) vit dans src/ota_integrity_logic.*.
//
// Usage : instance LOCALE au handler (pas d'état global partagé) :
//   OtaStreamHasher hasher;
//   hasher.begin();
//   hasher.update(chunk, len);   // par chunk, coût O(len), non bloquant
//   hasher.finish(out32);        // empreinte finale (32 octets)
// begin() réarme le contexte : une même instance est réutilisable.

#include <stddef.h>
#include <stdint.h>
#include <mbedtls/sha256.h>

class OtaStreamHasher {
public:
  OtaStreamHasher();
  ~OtaStreamHasher();

  // (Ré)initialise le contexte de hachage. À appeler avant le premier update().
  void begin();

  // Ajoute un chunk au hachage incrémental. data nul ou len 0 → no-op.
  void update(const uint8_t* data, size_t len);

  // Finalise et écrit l'empreinte SHA-256 (32 octets) dans out.
  void finish(uint8_t out[32]);

private:
  mbedtls_sha256_context ctx_;
};

#endif // OTA_INTEGRITY_H
