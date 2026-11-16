#include "ota_signature_switch_logic.h"

OtaSignatureSwitchDecision decideOtaSignatureSwitch(const OtaSignatureSwitchInputs& in,
                                                    uint32_t windowSec) {
  OtaSignatureSwitchDecision d{OtaSignatureRequirement::kRequired, OtaSwitchReason::kNoEntry, 0, 0};
  if (!in.nvsReadable) {
    d.reason = OtaSwitchReason::kUnreadable;
    return d;
  }
  if (!in.entryPresent) {
    d.reason = OtaSwitchReason::kNoEntry;
    return d;
  }
  if (!in.clockValid) {
    d.reason = OtaSwitchReason::kNoClock;
    return d;
  }
  if (in.expiryEpoch == 0) {  // C1 : valeur de repli d'un désarmement, ou corruption
    d.reason = OtaSwitchReason::kInvalid;
    return d;
  }
  if (in.nowEpoch >= in.expiryEpoch) {
    d.reason = OtaSwitchReason::kExpired;
    return d;
  }
  const uint32_t remaining = in.expiryEpoch - in.nowEpoch;  // > 0, sans repliement
  if (remaining > windowSec) {  // C1 : borne HAUTE
    d.reason = OtaSwitchReason::kInvalid;
    return d;
  }
  d.requirement = OtaSignatureRequirement::kWaived;
  d.reason = OtaSwitchReason::kArmed;
  d.remainingSec = remaining;
  d.untilEpoch = in.expiryEpoch;
  return d;
}

const char* otaSwitchReasonCode(OtaSwitchReason r) {
  switch (r) {
    case OtaSwitchReason::kNoEntry: return "no_entry";
    case OtaSwitchReason::kUnreadable: return "nvs_unreadable";
    case OtaSwitchReason::kNoClock: return "no_clock";
    case OtaSwitchReason::kExpired: return "expired";
    case OtaSwitchReason::kArmed: return "armed";
    case OtaSwitchReason::kInvalid: return "invalid_expiry";
    case OtaSwitchReason::kLocked: return "locked";
  }
  return "no_entry";
}
