#include "ota_kind_authority_logic.h"

OtaKindResolution resolveOtaEffectiveKind(OtaManifestKind callerKind, OtaManifestKind signedKind,
                                          bool signedKindUsable, bool authenticityValid) {
  OtaKindResolution result{callerKind, false};
  if (!signedKindUsable) {
    return result;
  }
  // Désaccord calculé HORS kValid aussi (seul signal observable neuf de D3-a).
  result.disagreement = (signedKind != callerKind);
  // ⛔ L'autorité n'est accordée qu'à une signature authentique.
  if (authenticityValid) {
    result.effectiveKind = signedKind;
  }
  return result;
}
