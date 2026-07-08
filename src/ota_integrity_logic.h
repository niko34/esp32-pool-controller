#ifndef OTA_INTEGRITY_LOGIC_H
#define OTA_INTEGRITY_LOGIC_H

// Module pur de vérification d'intégrité OTA (feature-026).
// Headers C uniquement (<stddef.h>/<stdint.h>) : testable en natif sans libc++.
// PAS de Arduino/String/mbedtls/FreeRTOS ici — le hachage matériel vit dans
// src/ota_integrity.* ; ce module ne fait que parser/comparer/formater.
//
// Format d'entrée : le champ `digest` des assets de l'API GitHub releases,
// de la forme "sha256:<64 caractères hexadécimaux>".

#include <stddef.h>
#include <stdint.h>

// Parse une empreinte "sha256:<64hex>" vers 32 octets binaires.
// - Préfixe "sha256:" OBLIGATOIRE, casse indifférente (préfixe et hex).
// - Refuse : pointeur nul, préfixe absent/incomplet, longueur != 64 hex,
//   caractère non hexadécimal, caractères en trop après les 64 hex.
// Retourne true et remplit out[32] si l'empreinte est valide (fail-closed sinon).
bool parseSha256Digest(const char* digestStr, uint8_t out[32]);

// Compare deux empreintes SHA-256 de 32 octets en temps constant
// (pas de sortie anticipée — évite toute fuite temporelle, coût négligeable).
// Pointeur nul → false (fail-closed).
bool sha256Equal(const uint8_t a[32], const uint8_t b[32]);

// Formate une empreinte binaire de 32 octets en hexadécimal MINUSCULE
// terminé par '\0'. `out` doit faire au moins 65 octets (outLen >= 65) ;
// sinon out[0] = '\0' (chaîne vide, jamais de débordement).
void sha256ToHex(const uint8_t hash[32], char* out, size_t outLen);

#endif // OTA_INTEGRITY_LOGIC_H
