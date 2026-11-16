#include "ota_version_request_logic.h"

#include <stdio.h>

namespace {

// Lit 1 à 6 chiffres décimaux ; avance `p`. false si aucun chiffre ou plus de 6.
bool readNumber(const char*& p, uint32_t* out) {
  uint32_t v = 0;
  int n = 0;
  while (*p >= '0' && *p <= '9') {
    if (++n > 6) return false;
    v = v * 10u + static_cast<uint32_t>(*p - '0');
    ++p;
  }
  if (n == 0) return false;
  *out = v;
  return true;
}

}  // namespace

bool otaParseVersionTag(const char* tag, uint32_t* maj, uint32_t* min, uint32_t* pat) {
  if (tag == nullptr) return false;
  const char* p = tag;
  if (*p == 'v') ++p;
  uint32_t a, b, c;
  if (!readNumber(p, &a) || *p++ != '.') return false;
  if (!readNumber(p, &b) || *p++ != '.') return false;
  if (!readNumber(p, &c) || *p != '\0') return false;
  *maj = a;
  *min = b;
  *pat = c;
  return true;
}

const char* otaSelectVersionReference(const char* storedTag, const char* paramValue,
                                      bool* fromParam) {
  if (fromParam != nullptr) *fromParam = false;
  if (storedTag != nullptr && storedTag[0] != '\0') return storedTag;
  if (paramValue != nullptr && paramValue[0] != '\0') {
    if (fromParam != nullptr) *fromParam = true;
    return paramValue;
  }
  return nullptr;
}

OtaVersionAgreement otaCompareRequestedVersion(bool haveReference, const char* referenceTag,
                                               bool manifestFieldsValid, uint32_t maj,
                                               uint32_t min, uint32_t pat,
                                               OtaParsedVersion* refOut) {
  if (!haveReference || referenceTag == nullptr) return OtaVersionAgreement::kNoReference;
  if (!manifestFieldsValid) return OtaVersionAgreement::kNoReference;
  OtaParsedVersion ref{};
  if (!otaParseVersionTag(referenceTag, &ref.major, &ref.minor, &ref.patch)) {
    return OtaVersionAgreement::kReferenceUnparsable;
  }
  if (refOut != nullptr) *refOut = ref;
  return (ref.major == maj && ref.minor == min && ref.patch == pat)
             ? OtaVersionAgreement::kAgree
             : OtaVersionAgreement::kDisagree;
}

void otaFormatVersion(char* out, size_t cap, uint32_t maj, uint32_t min, uint32_t pat) {
  if (out == nullptr || cap == 0) return;
  snprintf(out, cap, "%lu.%lu.%lu", static_cast<unsigned long>(maj),
           static_cast<unsigned long>(min), static_cast<unsigned long>(pat));
}
