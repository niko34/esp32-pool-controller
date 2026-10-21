#include "ota_base64_logic.h"

namespace {

// -1 si `c` n'appartient pas à l'alphabet base64 standard.
int8_t decodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

size_t otaBase64Decode(const char* in, size_t inLen, uint8_t* out, size_t outCapacity) {
  if (in == nullptr || out == nullptr || inLen == 0 || (inLen % 4) != 0) {
    return 0;
  }

  // Padding : SEULS les 1 ou 2 derniers caractères de la chaîne ENTIÈRE
  // peuvent être '=' — déduit ici des deux derniers octets, puis VÉRIFIÉ
  // caractère par caractère ci-dessous (tout '=' ailleurs est rejeté par la
  // boucle, qui ne l'autorise qu'aux emplacements calculés ici).
  size_t padCount = 0;
  if (in[inLen - 1] == '=') {
    padCount = 1;
    if (inLen >= 2 && in[inLen - 2] == '=') {
      padCount = 2;
    }
  }

  const size_t outLen = (inLen / 4) * 3 - padCount;
  if (outLen > outCapacity) {
    return 0;
  }

  size_t outIdx = 0;
  for (size_t i = 0; i < inLen; i += 4) {
    const bool isLastGroup = (i + 4 == inLen);
    int vals[4];
    for (size_t j = 0; j < 4; ++j) {
      const char c = in[i + j];
      const bool isPadSlot = isLastGroup &&
                             ((padCount == 1 && j == 3) || (padCount == 2 && (j == 2 || j == 3)));
      if (c == '=') {
        if (!isPadSlot) {
          return 0;  // '=' hors de la position de padding calculée → rejeté
        }
        vals[j] = 0;
      } else {
        const int8_t v = decodeChar(c);
        if (v < 0) {
          return 0;  // caractère hors alphabet
        }
        vals[j] = v;
      }
    }
    const uint8_t b0 = static_cast<uint8_t>((vals[0] << 2) | (vals[1] >> 4));
    const uint8_t b1 = static_cast<uint8_t>(((vals[1] & 0x0F) << 4) | (vals[2] >> 2));
    const uint8_t b2 = static_cast<uint8_t>(((vals[2] & 0x03) << 6) | vals[3]);
    if (outIdx < outLen) out[outIdx++] = b0;
    if (outIdx < outLen) out[outIdx++] = b1;
    if (outIdx < outLen) out[outIdx++] = b2;
  }
  return outLen;
}
