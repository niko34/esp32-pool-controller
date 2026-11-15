#include "ota_recovery_logic.h"

OtaRecoveryReason decideOtaRecoveryReason(const OtaRecoveryInputs& in) {
  // 1. Marqueur : prime sur tout, y compris sur un montage réussi. Ignoré s'il
  //    n'a pas pu être lu (repli : comportement d'aujourd'hui).
  if (in.markerReadable && in.fsUpdateInFlight) {
    return OtaRecoveryReason::kFsUpdateInterrupted;
  }
  // 2. Filesystem non monté : fait établi, pas une sonde.
  if (!in.fsMounted) {
    return OtaRecoveryReason::kMountFailed;
  }
  // 3. Monté : l'index absent n'arme le secours que si la sonde a VRAIMENT eu lieu.
  if (in.indexProbed && !in.indexPresent) {
    return OtaRecoveryReason::kIndexMissing;
  }
  return OtaRecoveryReason::kNone;
}

const char* otaRecoveryReasonLabel(OtaRecoveryReason reason) {
  switch (reason) {
    case OtaRecoveryReason::kNone:                return "aucun";
    case OtaRecoveryReason::kMountFailed:         return "montage du filesystem échoué";
    case OtaRecoveryReason::kFsUpdateInterrupted: return "mise à jour du filesystem interrompue";
    case OtaRecoveryReason::kIndexMissing:        return "index.html absent";
  }
  return "inconnu";
}

OtaRecoveryMarkerAction decideOtaRecoveryMarkerAction(bool isFilesystemUpdate,
                                                      OtaRecoveryUpdatePhase phase) {
  // S3 : ni pose ni levée hors genre FILESYSTEM.
  if (!isFilesystemUpdate) return OtaRecoveryMarkerAction::kNone;
  return (phase == OtaRecoveryUpdatePhase::kBeforeUnmount) ? OtaRecoveryMarkerAction::kArm
                                                           : OtaRecoveryMarkerAction::kDisarm;
}
