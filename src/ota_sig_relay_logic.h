#ifndef OTA_SIG_RELAY_LOGIC_H
#define OTA_SIG_RELAY_LOGIC_H

// =============================================================================
// Parse BORNÉ d'un fichier `.sig` (2 lignes) relayé par /check-update
// (feature-151 D3-a). Header sans dépendance Arduino/mbedtls → natif (ADR-0017).
//
// Grammaire : ligne 1 = manifeste canonique (parseOtaManifest), ligne 2 =
// signature base64 de EXACTEMENT 512 caractères [A-Za-z0-9+/] décodant en
// EXACTEMENT 384 octets. LF ou CRLF final toléré (UN seul), 2 lignes exactement.
//
// 🔴 TROISIÈME COPIE de cette grammaire, après le shell
// (tools/check_release_artefacts.sh, contrôle S2) et le JS (parseSigFile,
// data/app.js). ⛔ La suite native DOIT reprendre la matrice de vecteurs de
// test/sig_parse_vectors.js (ADR-0099) : une divergence silencieuse entre les
// trois copies est le risque n°1 de ce lot.
//
// ⛔ Entrée = flux RÉSEAU : non terminée par NUL, longueur explicite `len`,
// refus AVANT toute tokenisation si len > kOtaSigFetchMaxBytes.
// ⛔ Réutilise parseOtaManifest / serializeOtaManifest / otaBase64Decode sans
// les modifier. Le payload relayé est le manifeste RE-SÉRIALISÉ (canonical),
// jamais les octets bruts reçus.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#include "constants.h"  // kOtaSigFetchMaxBytes, kOtaSignatureBase64Len
#include "ota_manifest_logic.h"

static_assert(kOtaSigFetchMaxBytes >= kOtaManifestMaxLen + kOtaSignatureBase64Len + 2,
              "kOtaSigFetchMaxBytes doit contenir manifeste + signature base64 + 2 sauts de "
              "ligne (feature-151 D3-a)");

enum class OtaSigParseVerdict : uint8_t {
  kOk = 0,
  kEmpty = 1,      // aucun octet
  kTooLong = 2,    // len > kOtaSigFetchMaxBytes
  kLines = 3,      // pas exactement 2 lignes non vides
  kManifest = 4,   // ligne 1 hors grammaire (ou octet NUL)
  kSignature = 5,  // ligne 2 hors grammaire (ou octet NUL)
};

struct OtaSigRelayPayload {
  OtaManifestFields fields;
  char canonical[kOtaManifestMaxLen];              // manifeste re-sérialisé (NUL-terminé)
  size_t canonicalLen;
  char signatureBase64[kOtaSignatureBase64Len + 1];  // ligne 2 validée (NUL-terminée)
};

/**
 * @param raw  Octets reçus (pas nécessairement terminés par NUL).
 * @param len  Nombre d'octets valides dans `raw`.
 * @param out  [out] rempli QUE si kOk.
 */
OtaSigParseVerdict parseOtaSigFile(const char* raw, size_t len, OtaSigRelayPayload* out);

// Libellé FR stable (journal), jamais nul : "ok" | "vide" | "trop long" |
// "lignes" | "manifeste" | "signature".
const char* otaSigParseVerdictLabel(OtaSigParseVerdict v);

#endif  // OTA_SIG_RELAY_LOGIC_H
