#include "ota_manifest_binding_logic.h"

#include "ota_integrity_logic.h"  // parseOtaSha256Param, sha256Equal — modules purs

OtaManifestBindingVerdict otaManifestBindingVerdict(bool manifestFieldsValid,
                                                    const char* manifestSha256Hex,
                                                    bool comparedDigestValid,
                                                    const uint8_t comparedDigest[32]) {
  if (!manifestFieldsValid || !comparedDigestValid || manifestSha256Hex == nullptr ||
      comparedDigest == nullptr) {
    return OtaManifestBindingVerdict::kSkipped;
  }

  // Décodage LOCAL — jamais exposé à l'appelant : le format (64 hex
  // minuscules, sans préfixe) est le même que le paramètre `sha256` de
  // /update (feature-118), déjà validé strictement par parseOtaManifest avant
  // que `manifestFieldsValid` ne soit vrai. Un échec ici ne devrait donc
  // jamais survenir ; traité en kSkipped (jamais un faux "lié").
  uint8_t manifestDigest[32];
  if (parseOtaSha256Param(true, manifestSha256Hex, manifestDigest) !=
      OtaSha256ParamVerdict::kParsed) {
    return OtaManifestBindingVerdict::kSkipped;
  }

  return sha256Equal(manifestDigest, comparedDigest) ? OtaManifestBindingVerdict::kBound
                                                     : OtaManifestBindingVerdict::kUnbound;
}

bool otaManifestBindingIsDivergent(OtaManifestBindingVerdict verdict) {
  return verdict == OtaManifestBindingVerdict::kUnbound;
}

const char* otaManifestBindingVerdictLabel(OtaManifestBindingVerdict verdict) {
  switch (verdict) {
    case OtaManifestBindingVerdict::kSkipped:
      return "non applicable";
    case OtaManifestBindingVerdict::kBound:
      return "liée";
    case OtaManifestBindingVerdict::kUnbound:
      return "NON liée";
  }
  return "non applicable";  // Inatteignable — silence un warning -Wreturn-type
}
