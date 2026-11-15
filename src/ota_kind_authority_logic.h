#ifndef OTA_KIND_AUTHORITY_LOGIC_H
#define OTA_KIND_AUTHORITY_LOGIC_H

// =============================================================================
// Résolution PURE du genre EFFECTIF d'une image OTA (feature-151 D3-a).
// Header sans dépendance Arduino/mbedtls → testable en natif (ADR-0017).
//
// Le genre SIGNÉ (celui du manifeste) ne fait autorité QUE si l'authenticité
// est établie (kValid). Le DÉSACCORD entre le genre de l'appelant et le genre
// signé est en revanche calculé dès que le genre signé est exploitable, y
// compris hors kValid : c'est un signal d'observation, pas une décision.
//
// ⚠️ En D3-a c'est un no-op prouvable : combineOtaAuthenticityVerdict rend
// déjà kKindMismatch quand les genres diffèrent, donc kValid implique
// « genre signé == genre appelant ». La fonction existe pour le drapeau de
// désaccord et pour qu'il n'y ait qu'UN site de résolution.
// =============================================================================

#include "ota_manifest_logic.h"

struct OtaKindResolution {
  OtaManifestKind effectiveKind;  // genre à utiliser pour la suite de la route
  bool disagreement;              // genre signé exploitable ET différent de celui de l'appelant
};

/**
 * @param callerKind         Genre déduit par la route (type d'upload / URL).
 * @param signedKind         Genre déclaré par le manifeste (valide si signedKindUsable).
 * @param signedKindUsable   Le manifeste a PARSÉ (genre exploitable).
 * @param authenticityValid  Verdict d'authenticité == kValid.
 */
OtaKindResolution resolveOtaEffectiveKind(OtaManifestKind callerKind, OtaManifestKind signedKind,
                                          bool signedKindUsable, bool authenticityValid);

#endif  // OTA_KIND_AUTHORITY_LOGIC_H
