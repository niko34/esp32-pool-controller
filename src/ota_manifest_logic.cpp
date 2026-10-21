#include "ota_manifest_logic.h"

#include <stdio.h>   // snprintf (module pur mais testé en natif, cf. log_arena_logic.cpp)
#include <string.h>  // memcmp, strnlen

namespace {

// Longueur maximale de chaîne brute explorée : au-delà, la chaîne n'est de
// toute façon pas un manifeste valide (kOtaManifestMaxLen - 1 caractères
// utiles au maximum). strnlen borné : jamais de lecture au-delà si `raw`
// n'est pas terminé par NUL dans cette fenêtre.
constexpr size_t kScanLimit = kOtaManifestMaxLen;

bool isDigitAscii(char c) { return c >= '0' && c <= '9'; }

// Valide un composant de version : 1 à 5 chiffres, AUCUN zéro de tête sauf
// si le composant est EXACTEMENT "0". Écrit la valeur dans `out` (uint32_t,
// 5 chiffres max ⇒ ne déborde jamais).
bool parseVersionComponent(const char* start, size_t len, uint32_t* out) {
  if (len == 0 || len > 5) {
    return false;
  }
  if (len > 1 && start[0] == '0') {
    return false;  // zéro de tête interdit (sauf "0" seul)
  }
  uint32_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    if (!isDigitAscii(start[i])) {
      return false;
    }
    value = value * 10 + static_cast<uint32_t>(start[i] - '0');
  }
  *out = value;
  return true;
}

// Parse "MAJ.MIN.PATCH" STRICT : exactement 2 points, ni de tête ni de queue,
// aucun point consécutif (composant vide).
bool parseVersionToken(const char* token, size_t len, uint32_t* major, uint32_t* minor,
                       uint32_t* patch) {
  size_t dotPositions[2];
  size_t dotCount = 0;
  for (size_t i = 0; i < len; ++i) {
    if (token[i] == '.') {
      if (dotCount >= 2) {
        return false;  // trop de points
      }
      dotPositions[dotCount++] = i;
    }
  }
  if (dotCount != 2) {
    return false;
  }
  if (dotPositions[0] == 0 || dotPositions[1] == dotPositions[0] + 1 ||
      dotPositions[1] == len - 1) {
    return false;  // composant vide (tête, milieu, ou queue)
  }
  return parseVersionComponent(token, dotPositions[0], major) &&
         parseVersionComponent(token + dotPositions[0] + 1,
                               dotPositions[1] - dotPositions[0] - 1, minor) &&
         parseVersionComponent(token + dotPositions[1] + 1, len - dotPositions[1] - 1, patch);
}

// Valide le jeton "sha256:<64 hex minuscules>" STRICT (préfixe exact,
// longueur exacte, caractères 0-9a-f uniquement — jamais A-F).
bool parseHexToken(const char* token, size_t len, char outHex[65]) {
  static const char prefix[] = "sha256:";
  static const size_t prefixLen = 7;
  static const size_t hexLen = 64;
  if (len != prefixLen + hexLen) {
    return false;
  }
  if (memcmp(token, prefix, prefixLen) != 0) {
    return false;
  }
  const char* hex = token + prefixLen;
  for (size_t i = 0; i < hexLen; ++i) {
    char c = hex[i];
    bool isLowerHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!isLowerHex) {
      return false;
    }
    outHex[i] = c;
  }
  outHex[hexLen] = '\0';
  return true;
}

}  // namespace

OtaManifestParseVerdict parseOtaManifest(const char* raw, OtaManifestFields* out) {
  if (raw == nullptr || out == nullptr) {
    return OtaManifestParseVerdict::kMalformed;
  }

  const size_t len = strnlen(raw, kScanLimit);
  if (len == 0 || len >= kScanLimit) {
    return OtaManifestParseVerdict::kMalformed;  // vide, ou trop long/non terminé dans la fenêtre
  }

  // Localise EXACTEMENT 3 espaces simples (0x20). Un caractère de contrôle ou
  // non-ASCII (LF, CR, TAB...) n'est PAS rejeté ICI par un test dédié — c'est
  // délibéré et PROUVÉ par mutation (feature-151 D1) : à n'importe quelle
  // position, un tel octet fait déjà échouer le validateur du jeton qui le
  // contient (memcmp EXACT pour le magique et le genre, classe de caractères
  // stricte pour la version et le hex) ; un contrôle générique ici serait du
  // code mort, invérifiable par les tests (vérifié : le désactiver ne fait
  // mourir AUCUN cas). Le piège "LF final" (echo vs printf) reste fermé : un
  // LF en fin de chaîne allonge le dernier jeton au-delà de sa longueur
  // EXACTE attendue (parseHexToken), donc refusé par ce contrôle de longueur.
  // Initialisé à zéro (revue de sécurité, 2026-10-18) : jamais LU non
  // initialisé sur le chemin normal (spaceCount borne toute lecture), mais un
  // tableau non initialisé rend le comportement du mutant qui déborderait
  // cette garde INDÉFINI plutôt que déterministe — une mutation sur
  // `spaceCount != 3` devenait alors NON CONCLUANTE (le mutant pouvait
  // survivre par chance mémoire). Coût nul, mutation redevenue concluante.
  size_t spacePos[3] = {0, 0, 0};
  size_t spaceCount = 0;
  for (size_t i = 0; i < len; ++i) {
    if (raw[i] == ' ') {
      if (spaceCount >= 3) {
        return OtaManifestParseVerdict::kMalformed;  // 5e jeton (4e espace détecté)
      }
      spacePos[spaceCount++] = i;
    }
  }
  if (spaceCount != 3) {
    return OtaManifestParseVerdict::kMalformed;  // jeton manquant (ou en trop, cas < 3 restant)
  }
  // ⚠️ Aucun contrôle explicite ici pour l'espace de tête/de queue ou les
  // espaces consécutifs — PROUVÉ redondant par mutation (feature-151 D1) :
  // chacun de ces cas produit un jeton de longueur 0 (magique, genre, version
  // ou hex selon la position), et CHAQUE validateur de jeton ci-dessous exige
  // une longueur EXACTE non nulle (magique/genre par memcmp, version/hex par
  // leur propre contrôle de longueur) — un jeton vide y échoue
  // systématiquement. Un contrôle dédié ici serait du code mort,
  // invérifiable par les tests (désactivé puis testé : aucun cas ne meurt).
  const char* magicTok = raw;
  const size_t magicLen = spacePos[0];
  const char* kindTok = raw + spacePos[0] + 1;
  const size_t kindLen = spacePos[1] - spacePos[0] - 1;
  const char* versionTok = raw + spacePos[1] + 1;
  const size_t versionLen = spacePos[2] - spacePos[1] - 1;
  const char* hexTok = raw + spacePos[2] + 1;
  const size_t hexTokLen = len - spacePos[2] - 1;

  // Jeton 1 : magique littéral, casse sensible, longueur exacte.
  size_t expectedMagicLen = 0;
  while (kOtaManifestMagic[expectedMagicLen] != '\0') {
    ++expectedMagicLen;
  }
  if (magicLen != expectedMagicLen || memcmp(magicTok, kOtaManifestMagic, magicLen) != 0) {
    return OtaManifestParseVerdict::kMalformed;
  }

  // Jeton 2 : genre, UN DES DEUX littéraux exacts, casse sensible
  // ("Firmware" est refusé — mutation de validation exigée par la spec).
  OtaManifestKind kind;
  if (kindLen == 8 && memcmp(kindTok, "firmware", 8) == 0) {
    kind = OtaManifestKind::kFirmware;
  } else if (kindLen == 10 && memcmp(kindTok, "filesystem", 10) == 0) {
    kind = OtaManifestKind::kFilesystem;
  } else {
    return OtaManifestParseVerdict::kMalformed;
  }

  // Jeton 3 : version stricte.
  uint32_t major, minor, patch;
  if (!parseVersionToken(versionTok, versionLen, &major, &minor, &patch)) {
    return OtaManifestParseVerdict::kMalformed;
  }

  // Jeton 4 : empreinte sha256 stricte.
  char hexOut[65];
  if (!parseHexToken(hexTok, hexTokLen, hexOut)) {
    return OtaManifestParseVerdict::kMalformed;
  }

  // Tout a validé : remplissage de `out` — JAMAIS avant ce point (fail-closed,
  // même discipline que parseHexDigest dans ota_integrity_logic.cpp : pas
  // d'écriture partielle sur un refus).
  out->kind = kind;
  out->major = major;
  out->minor = minor;
  out->patch = patch;
  memcpy(out->sha256Hex, hexOut, sizeof(out->sha256Hex));
  return OtaManifestParseVerdict::kOk;
}

size_t serializeOtaManifest(const OtaManifestFields& fields, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return 0;
  }
  const char* kindLit = otaManifestKindLiteral(fields.kind);
  int n = snprintf(out, outLen, "%s %s %u.%u.%u sha256:%s", kOtaManifestMagic, kindLit,
                   static_cast<unsigned>(fields.major), static_cast<unsigned>(fields.minor),
                   static_cast<unsigned>(fields.patch), fields.sha256Hex);
  if (n < 0 || static_cast<size_t>(n) >= outLen) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

const char* otaManifestKindLiteral(OtaManifestKind kind) {
  switch (kind) {
    case OtaManifestKind::kFirmware:
      return "firmware";
    case OtaManifestKind::kFilesystem:
      return "filesystem";
  }
  return "firmware";  // Inatteignable — silence un warning -Wreturn-type
}

bool otaVersionBelowFloor(uint32_t major, uint32_t minor, uint32_t patch, uint32_t floorMajor,
                          uint32_t floorMinor, uint32_t floorPatch) {
  if (major != floorMajor) return major < floorMajor;
  if (minor != floorMinor) return minor < floorMinor;
  return patch < floorPatch;
}
