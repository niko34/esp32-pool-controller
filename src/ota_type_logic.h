#ifndef OTA_TYPE_LOGIC_H
#define OTA_TYPE_LOGIC_H

// =============================================================================
// Logique PURE du contrôle de cohérence « type de mise à jour ↔ image reçue »
// (bug-echec-ota-indechiffrable-et-noyade-du-journal, 2.20.1). Header sans
// dépendance Arduino → testable en natif (ADR-0017). Consommé par
// src/web_routes_ota.cpp :
//   - inferOtaKindFromFilename : déduit le type à partir du NOM du fichier
//     envoyé (repli quand le formulaire ne fournit pas `update_type`) ;
//   - checkOtaFirstByte        : pré-contrôle de l'octet magique ESP32 sur le
//     tout premier octet reçu, AVANT le moindre effet de bord (inhibition des
//     pompes, démontage LittleFS, Update.begin()).
//
// RÉSERVE BLOQUANTE — équivalence avec le contrôle du framework
// -------------------------------------------------------------
// Ce pré-contrôle est prouvablement équivalent à celui de
// UpdateClass::_writeBuffer() (qui vérifie ESP_IMAGE_HEADER_MAGIC sur le
// premier octet du premier tampon quand la commande est U_FLASH) SOUS LA
// CONDITION que le projet n'appelle JAMAIS Update.setupCrypt()/setCryptKey()
// — vrai aujourd'hui, aucune occurrence dans src/. Avec le chiffrement actif,
// le bloc crypto s'applique EN AMONT du contrôle magique et l'erreur réellement
// produite par le framework devient « Decryption error », pas « Wrong Magic
// Byte » : notre octet 0 ne serait plus l'octet contrôlé.
// Ne PAS conserver si Update.setupCrypt() est un jour appelé.
// Corollaire : ne JAMAIS parser Update.errorString() pour décider — c'est ce
// pré-contrôle, et lui seul, qui produit le message « pas un firmware ».
// =============================================================================

#include <stddef.h>
#include <stdint.h>

// Octet magique d'en-tête d'image ESP32 (ESP_IMAGE_HEADER_MAGIC).
constexpr uint8_t kEspImageMagic = 0xE9;

// Nature déduite d'une image OTA à partir de son nom de fichier.
enum class OtaImageKind : uint8_t {
  kFirmware,    // Image applicative (firmware.bin, *.bin non reconnu comme FS)
  kFilesystem,  // Image LittleFS/SPIFFS
  kUnknown      // Nom absent, vide ou non concluant
};

// Verdict du pré-contrôle du premier octet reçu.
enum class OtaTypeVerdict : uint8_t {
  kAccept,                // Cohérent (ou indécidable sans risque) → poursuivre
  kRejectNotFirmware,     // Type « firmware » demandé mais l'image n'en est pas un → refus
  kWarnLooksLikeFirmware  // Type « filesystem » demandé sur une image d'allure firmware → avertir
};

// Comparaison de suffixe insensible à la casse. `suffix` doit être en
// minuscules. Implémentation locale : ni strcasecmp ni String ici (header pur).
inline bool otaFilenameEndsWithNoCase(const char* name, size_t nameLen, const char* suffix) {
  size_t suffixLen = 0;
  while (suffix[suffixLen] != '\0') {
    ++suffixLen;
  }
  if (suffixLen == 0 || suffixLen > nameLen) {
    return false;
  }
  const char* tail = name + (nameLen - suffixLen);
  for (size_t i = 0; i < suffixLen; ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != suffix[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Déduit la nature d'une image OTA à partir de son nom de fichier.
 *
 * Reconnaît les noms produits par build_fs.sh et par les releases GitHub, avec
 * ou sans préfixe de version : `littlefs.bin`, `spiffs.bin`, `filesystem.bin`,
 * `v2.20.1.littlefs.bin`, `pool.spiffs.bin`, `pool.fs.bin`. Le NOM NU
 * `littlefs.bin` doit matcher : c'était précisément le trou de la détection
 * historique, qui n'acceptait que les suffixes précédés d'un point.
 * Insensible à la casse. Chemin éventuel ignoré (seul le nom de base compte).
 *
 * @param filename Nom du fichier (peut être nullptr ou vide).
 * @return kFilesystem, kFirmware, ou kUnknown si le nom n'est pas concluant.
 */
inline OtaImageKind inferOtaKindFromFilename(const char* filename) {
  if (filename == nullptr) {
    return OtaImageKind::kUnknown;
  }

  // Longueur + isolement du nom de base (les navigateurs n'envoient normalement
  // pas de chemin, mais certains clients HTTP le font).
  size_t len = 0;
  while (filename[len] != '\0') {
    ++len;
  }
  size_t start = 0;
  for (size_t i = 0; i < len; ++i) {
    if (filename[i] == '/' || filename[i] == '\\') {
      start = i + 1;
    }
  }
  const char* name = filename + start;
  const size_t nameLen = len - start;
  if (nameLen == 0) {
    return OtaImageKind::kUnknown;
  }

  // Suffixes « système de fichiers ». `littlefs.bin` et `spiffs.bin` sans point
  // couvrent aussi les variantes préfixées (`v2.20.1.littlefs.bin`).
  // `fs.bin` n'est JAMAIS listé nu : ce serait un suffixe de `refs.bin`,
  // `defs.bin`, `mytoolfs.bin`… — d'où l'exigence d'un séparateur devant.
  // Le mirroir JS (data/app.js, detectUpdateType) ancre la même règle.
  static const char* const kFsSuffixes[] = {
      "littlefs.bin", "spiffs.bin", "filesystem.bin",
      ".fs.bin", "-fs.bin", "_fs.bin"};
  for (size_t i = 0; i < sizeof(kFsSuffixes) / sizeof(kFsSuffixes[0]); ++i) {
    if (otaFilenameEndsWithNoCase(name, nameLen, kFsSuffixes[i])) {
      return OtaImageKind::kFilesystem;
    }
  }
  // Nom de base exactement `fs.bin` (rien devant : le séparateur est le début
  // du nom lui-même).
  if (nameLen == 6 && otaFilenameEndsWithNoCase(name, nameLen, "fs.bin")) {
    return OtaImageKind::kFilesystem;
  }

  // Tout autre binaire est présumé firmware (comportement historique du repli).
  if (otaFilenameEndsWithNoCase(name, nameLen, ".bin")) {
    return OtaImageKind::kFirmware;
  }
  return OtaImageKind::kUnknown;
}

/**
 * @brief Pré-contrôle du premier octet reçu contre le type de mise à jour demandé.
 *
 * @param isFirmwareCmd true si la commande est U_FLASH (firmware), false si U_SPIFFS.
 * @param firstByte Premier octet du flux (à n'appeler que si au moins 1 octet reçu).
 * @return kAccept              : poursuivre ;
 *         kRejectNotFirmware   : refuser AVANT tout effet de bord ;
 *         kWarnLooksLikeFirmware : avertir mais POURSUIVRE — une image LittleFS
 *         n'a pas de magique fiable à l'offset 0, un octet 0xE9 y est possible
 *         par coïncidence : refuser dans ce sens produirait des faux positifs.
 */
inline OtaTypeVerdict checkOtaFirstByte(bool isFirmwareCmd, uint8_t firstByte) {
  if (isFirmwareCmd) {
    return (firstByte == kEspImageMagic) ? OtaTypeVerdict::kAccept
                                         : OtaTypeVerdict::kRejectNotFirmware;
  }
  return (firstByte == kEspImageMagic) ? OtaTypeVerdict::kWarnLooksLikeFirmware
                                       : OtaTypeVerdict::kAccept;
}

#endif  // OTA_TYPE_LOGIC_H
