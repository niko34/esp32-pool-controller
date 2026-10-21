#ifndef OTA_MANIFEST_LOGIC_H
#define OTA_MANIFEST_LOGIC_H

// =============================================================================
// Logique PURE de la grammaire du MANIFESTE de signature OTA (feature-151 D1,
// condition 4 du verdict security-reviewer P1). Header sans dépendance
// Arduino/mbedtls → testable en natif (ADR-0017).
//
// Grammaire STRICTE, EXACTEMENT 4 jetons séparés par UN SEUL 0x20 :
//   pool-ota/1 <firmware|filesystem> <MAJ.MIN.PATCH> sha256:<64 hex minuscules>
// ⛔ Aucun LF, aucun espace de tête/queue, aucun espace double. Genre = l'un
// des DEUX littéraux exacts (casse sensible : "Firmware" est refusé). Version
// \d{1,5}\.\d{1,5}\.\d{1,5}, ⛔ sans préfixe "v", ⛔ sans zéro de tête (sauf la
// valeur "0" elle-même). Hex de 64 caractères STRICTEMENT minuscules.
//
// ⭐ L'appareil ne vérifie JAMAIS la signature sur la chaîne reçue telle
// quelle : il la PARSE puis la RE-SÉRIALISE depuis les champs extraits
// (serializeOtaManifest) — c'est sur CES octets canoniques que porte la
// signature (voir ota_signature.h). Ce découpage est ce qui permet à D3 de
// n'ajouter qu'un `if` de refus sans toucher à cette grammaire (condition 10).
// =============================================================================

#include <stddef.h>
#include <stdint.h>

// Jeton magique fixe en tête du manifeste (littéral, casse sensible).
constexpr const char* kOtaManifestMagic = "pool-ota/1";

// Taille maximale d'un manifeste (brut en entrée, ou canonique re-sérialisé).
// Marge large : la ligne la plus longue possible mesure 111 octets
// ("pool-ota/1 filesystem 99999.99999.99999 sha256:" + 64 hex) ; un brut plus
// long que cette borne est refusé sans même être tokenisé.
constexpr size_t kOtaManifestMaxLen = 160;

// Nature du firmware déclarée par le manifeste.
enum class OtaManifestKind : uint8_t {
  kFirmware = 0,
  kFilesystem = 1,
};

// Champs extraits d'un manifeste valide.
struct OtaManifestFields {
  OtaManifestKind kind;
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  char sha256Hex[65];  // 64 hex minuscules + terminateur NUL
};

enum class OtaManifestParseVerdict : uint8_t {
  kOk = 0,
  kMalformed = 1,
};

/**
 * @brief Parse STRICTEMENT un manifeste "pool-ota/1 ...".
 *
 * `raw` doit être terminé par NUL. Toute déviation à la grammaire (jeton en
 * trop ou manquant, espace double/de tête/de queue, LF ou tout caractère de
 * contrôle, genre non reconnu ou mal casé, version mal formée ou à zéro de
 * tête, hex non conforme) → kMalformed. `out` n'est rempli QUE si kOk.
 *
 * @param raw Chaîne brute reçue (peut être nullptr → kMalformed).
 * @param out [out] Champs extraits, valides seulement si retour == kOk.
 * @return kOk ou kMalformed.
 */
OtaManifestParseVerdict parseOtaManifest(const char* raw, OtaManifestFields* out);

/**
 * @brief Re-sérialise un manifeste CANONIQUE depuis des champs déjà parsés.
 *
 * C'est sur CES octets, jamais sur la chaîne brute reçue, que la signature
 * doit être vérifiée (voir le commentaire d'en-tête du fichier).
 *
 * @param fields Champs source (supposés déjà valides, ex. issus de parseOtaManifest).
 * @param out    Buffer de sortie, doit faire au moins kOtaManifestMaxLen octets.
 * @param outLen Taille de `out`.
 * @return Longueur écrite (hors NUL), ou 0 si `out`/`outLen` insuffisant
 *         (défensif — ne devrait jamais se produire avec kOtaManifestMaxLen).
 */
size_t serializeOtaManifest(const OtaManifestFields& fields, char* out, size_t outLen);

// Libellé littéral du genre ("firmware" | "filesystem"), jamais nul.
const char* otaManifestKindLiteral(OtaManifestKind kind);

/**
 * @brief Compare une version (MAJ.MIN.PATCH) à un plancher, lexicographique.
 *
 * ⚠️ feature-151 D1 : le PLANCHER lui-même n'est pas encore activé (persistance
 * NVS et endpoints d'administration = D3/D5, hors périmètre D1). Cette
 * fonction pure existe pour être appelée DÈS D1 avec un plancher figé à
 * 0.0.0 (jamais franchi en pratique), afin que D3 réutilise EXACTEMENT ce
 * même prédicat (condition 10 : octet pour octet identique).
 *
 * @return true si (major.minor.patch) < (floorMajor.floorMinor.floorPatch).
 */
bool otaVersionBelowFloor(uint32_t major, uint32_t minor, uint32_t patch,
                          uint32_t floorMajor, uint32_t floorMinor, uint32_t floorPatch);

#endif  // OTA_MANIFEST_LOGIC_H
