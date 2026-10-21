#include "ota_authenticity_logic.h"

OtaAuthenticityVerdict combineOtaAuthenticityVerdict(
    bool manifestPresent, bool signaturePresent, OtaManifestParseVerdict parseVerdict,
    bool signatureDecodeOk, OtaCryptoVerdict cryptoVerdict, OtaManifestKind parsedKind,
    OtaManifestKind expectedKind, uint32_t major, uint32_t minor, uint32_t patch,
    uint32_t floorMajor, uint32_t floorMinor, uint32_t floorPatch) {
  if (!manifestPresent || !signaturePresent) {
    return OtaAuthenticityVerdict::kAbsent;
  }
  if (parseVerdict != OtaManifestParseVerdict::kOk || !signatureDecodeOk) {
    return OtaAuthenticityVerdict::kMalformed;
  }
  switch (cryptoVerdict) {
    case OtaCryptoVerdict::kImpossible:
      return OtaAuthenticityVerdict::kVerificationImpossible;
    case OtaCryptoVerdict::kInvalid:
      return OtaAuthenticityVerdict::kInvalid;
    case OtaCryptoVerdict::kValid:
      break;
  }
  // Signature authentique : reste à vérifier la cohérence genre + plancher.
  if (parsedKind != expectedKind) {
    return OtaAuthenticityVerdict::kKindMismatch;
  }
  if (otaVersionBelowFloor(major, minor, patch, floorMajor, floorMinor, floorPatch)) {
    return OtaAuthenticityVerdict::kBelowFloor;
  }
  return OtaAuthenticityVerdict::kValid;
}

bool otaAuthenticityIsRejected(OtaAuthenticityVerdict verdict) {
  return verdict != OtaAuthenticityVerdict::kValid;
}

const char* otaAuthenticityVerdictLabel(OtaAuthenticityVerdict verdict) {
  switch (verdict) {
    case OtaAuthenticityVerdict::kAbsent:
      return "absente";
    case OtaAuthenticityVerdict::kMalformed:
      return "malformée";
    case OtaAuthenticityVerdict::kVerificationImpossible:
      return "vérification impossible";
    case OtaAuthenticityVerdict::kInvalid:
      return "invalide";
    case OtaAuthenticityVerdict::kKindMismatch:
      return "genre discordant";
    case OtaAuthenticityVerdict::kBelowFloor:
      return "sous le plancher";
    case OtaAuthenticityVerdict::kValid:
      return "valide";
  }
  return "absente";  // Inatteignable — silence un warning -Wreturn-type
}
