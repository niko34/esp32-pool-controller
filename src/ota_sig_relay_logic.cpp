#include "ota_sig_relay_logic.h"

#include <string.h>

#include "ota_base64_logic.h"

namespace {
bool isBase64Char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
         c == '/';
}
}  // namespace

OtaSigParseVerdict parseOtaSigFile(const char* raw, size_t len, OtaSigRelayPayload* out) {
  if (raw == nullptr || out == nullptr || len == 0) return OtaSigParseVerdict::kEmpty;
  // ⛔ Borne dure AVANT toute tokenisation (entrée tierce).
  if (len > kOtaSigFetchMaxBytes) return OtaSigParseVerdict::kTooLong;

  // Retire UN SEUL terminateur final (CRLF ou LF) — miroir de parseSigFile (JS).
  size_t end = len;
  if (raw[end - 1] == '\n') {
    --end;
    if (end > 0 && raw[end - 1] == '\r') --end;
  }

  // Première fin de ligne : un seul LF autorisé dans le reste.
  const void* firstLf = memchr(raw, '\n', end);
  if (firstLf == nullptr) return OtaSigParseVerdict::kLines;
  const size_t p = static_cast<size_t>(static_cast<const char*>(firstLf) - raw);
  size_t line1End = p;
  if (line1End > 0 && raw[line1End - 1] == '\r') --line1End;  // CRLF entre les 2 lignes
  const size_t line2Start = p + 1;
  if (memchr(raw + line2Start, '\n', end - line2Start) != nullptr) return OtaSigParseVerdict::kLines;
  const size_t line2Len = end - line2Start;
  if (line1End == 0 || line2Len == 0) return OtaSigParseVerdict::kLines;

  // Ligne 1 : copie bornée terminée par NUL (parseOtaManifest exige un C-string).
  // ⛔ Un NUL embarqué tronquerait silencieusement la ligne : refusé.
  if (line1End >= kOtaManifestMaxLen || memchr(raw, '\0', line1End) != nullptr) {
    return OtaSigParseVerdict::kManifest;
  }
  char line1[kOtaManifestMaxLen];
  memcpy(line1, raw, line1End);
  line1[line1End] = '\0';
  OtaManifestFields fields{};
  if (parseOtaManifest(line1, &fields) != OtaManifestParseVerdict::kOk) {
    return OtaSigParseVerdict::kManifest;
  }

  // Ligne 2 : 512 caractères de l'alphabet standard, SANS '=' (384 o = multiple de 3).
  if (line2Len != kOtaSignatureBase64Len) return OtaSigParseVerdict::kSignature;
  const char* line2 = raw + line2Start;
  for (size_t i = 0; i < line2Len; ++i) {
    if (!isBase64Char(line2[i])) return OtaSigParseVerdict::kSignature;
  }
  uint8_t sig[kOtaSignatureMaxBytes];
  if (otaBase64Decode(line2, line2Len, sig, sizeof(sig)) != kOtaSignatureMaxBytes) {
    return OtaSigParseVerdict::kSignature;
  }

  out->fields = fields;
  out->canonicalLen = serializeOtaManifest(fields, out->canonical, sizeof(out->canonical));
  if (out->canonicalLen == 0) return OtaSigParseVerdict::kManifest;  // défensif
  memcpy(out->signatureBase64, line2, line2Len);
  out->signatureBase64[line2Len] = '\0';
  return OtaSigParseVerdict::kOk;
}

const char* otaSigParseVerdictLabel(OtaSigParseVerdict v) {
  switch (v) {
    case OtaSigParseVerdict::kOk: return "ok";
    case OtaSigParseVerdict::kEmpty: return "vide";
    case OtaSigParseVerdict::kTooLong: return "trop long";
    case OtaSigParseVerdict::kLines: return "lignes";
    case OtaSigParseVerdict::kManifest: return "manifeste";
    case OtaSigParseVerdict::kSignature: return "signature";
  }
  return "inconnu";
}
