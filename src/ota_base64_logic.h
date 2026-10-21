#ifndef OTA_BASE64_LOGIC_H
#define OTA_BASE64_LOGIC_H

// =============================================================================
// Décodeur base64 STRICT, PUR (feature-151 D1, condition 4). Header sans
// dépendance Arduino/mbedtls → testable en natif (ADR-0017).
//
// Alphabet standard RFC 4648 (+ / avec padding =). Utilisé pour décoder le
// paramètre `signature` (signature RSA-3072, 384 octets bruts → 512
// caractères base64 SANS padding, 384 étant un multiple de 3).
//
// Sans allocation : écrit dans un buffer fourni par l'appelant.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Décode une chaîne base64 STRICTE vers des octets bruts.
 *
 * Refuse (retourne 0, AUCUNE écriture garantie au-delà de ce qui a déjà été
 * validé) :
 *   - longueur d'entrée non multiple de 4 ;
 *   - caractère hors alphabet (hormis '=' en position de padding) ;
 *   - padding mal placé (seuls les 0, 1 ou 2 derniers caractères peuvent
 *     être '=', jamais au milieu) ;
 *   - sortie décodée qui ne tiendrait pas dans `outCapacity`.
 *
 * @param in           Chaîne base64 (pas nécessairement terminée par NUL).
 * @param inLen         Longueur de `in`.
 * @param out           Buffer de sortie.
 * @param outCapacity   Taille de `out`.
 * @return Nombre d'octets décodés (> 0 en cas de succès), ou 0 en cas d'échec.
 */
size_t otaBase64Decode(const char* in, size_t inLen, uint8_t* out, size_t outCapacity);

#endif  // OTA_BASE64_LOGIC_H
