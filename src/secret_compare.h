#ifndef SECRET_COMPARE_H
#define SECRET_COMPARE_H

#include <stddef.h>
#include <stdint.h>

// =============================================================================
// secret_compare — comparaison d'octets SANS sortie anticipée (bug-183), PURE
// =============================================================================
// Module sans dépendance Arduino (testable en natif, ADR-0017), sans allocation.
//
// Rend vrai si les `n` octets de `a` et de `b` sont identiques. Le temps ne
// dépend que de `n` : OR des XOR, ⛔ aucun `return` dans la boucle.
//
// ⚠️ À appeler sur deux EMPREINTES de même largeur (SHA-256 des deux côtés,
// voir AuthManager::passwordMatches), pas sur les secrets bruts :
//   Le temps ne dépend plus de la RELATION entre candidat et secret. Il dépend
//   encore de la LONGUEUR de chacun (coût du hachage), mais c'est un décalage
//   CONSTANT, identique à chaque requête, que l'attaquant n'a aucune référence
//   pour calibrer : il ne lui apprend rien sur le contenu.
// ⛔ Ne PAS la remplacer par `memcmp` ni `==` : ils s'arrêtent au premier écart.
inline bool fixedWidthEquals(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

#endif  // SECRET_COMPARE_H
