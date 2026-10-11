#ifndef OTA_INTEGRITY_LOGIC_H
#define OTA_INTEGRITY_LOGIC_H

// Module pur de vérification d'intégrité OTA (feature-026).
// Headers C uniquement (<stddef.h>/<stdint.h>) : testable en natif sans libc++.
// PAS de Arduino/String/mbedtls/FreeRTOS ici — le hachage matériel vit dans
// src/ota_integrity.* ; ce module ne fait que parser/comparer/formater.
//
// Format d'entrée : le champ `digest` des assets de l'API GitHub releases,
// de la forme "sha256:<64 caractères hexadécimaux>".

#include <stddef.h>
#include <stdint.h>

// Parse une empreinte "sha256:<64hex>" vers 32 octets binaires.
// - Préfixe "sha256:" OBLIGATOIRE, casse indifférente (préfixe et hex).
// - Refuse : pointeur nul, préfixe absent/incomplet, longueur != 64 hex,
//   caractère non hexadécimal, caractères en trop après les 64 hex.
// Retourne true et remplit out[32] si l'empreinte est valide (fail-closed sinon).
bool parseSha256Digest(const char* digestStr, uint8_t out[32]);

// Compare deux empreintes SHA-256 de 32 octets en temps constant
// (pas de sortie anticipée — évite toute fuite temporelle, coût négligeable).
// Pointeur nul → false (fail-closed).
bool sha256Equal(const uint8_t a[32], const uint8_t b[32]);

// Formate une empreinte binaire de 32 octets en hexadécimal MINUSCULE
// terminé par '\0'. `out` doit faire au moins 65 octets (outLen >= 65) ;
// sinon out[0] = '\0' (chaîne vide, jamais de débordement).
void sha256ToHex(const uint8_t hash[32], char* out, size_t outLen);

// =============================================================================
// feature-118 : verdict d'authenticité de l'empreinte `sha256` de POST
// /update. Lot A (journalisation seule) a introduit ce prédicat ; lot B le
// transforme en DÉCISION (kAbsent et kNonConform sont désormais refusés —
// kMalformed l'était déjà depuis feature-026). Découpé en DEUX pas pour
// coller aux DEUX moments du handler :
//   1) parseOtaSha256Param, évalué à index==0 (dès réception du paramètre) :
//      tranche ABSENT/MALFORMED sans connaître encore l'empreinte CALCULÉE
//      sur le flux (le hachage incrémental n'est terminé qu'à `final`).
//   2) otaUploadIntegrityVerdict, évalué à `final` : combine ce résultat avec
//      la comparaison à l'empreinte calculée pour trancher CONFORME/NON
//      CONFORME.
// Les deux sont PURS (mêmes contraintes d'en-têtes que ci-dessus).
// =============================================================================

// Verdict du PARAMÈTRE `sha256` seul (avant toute comparaison au flux reçu).
enum class OtaSha256ParamVerdict : uint8_t {
  kAbsent = 0,     // Paramètre non fourni par l'appelant
  kMalformed = 1,  // Fourni mais ne parse dans AUCUN des deux formats acceptés
  kParsed = 2      // Fourni et parsé — `out` rempli (32 octets)
};

// present   : request->hasParam("sha256", true) côté appelant.
// digestStr : valeur brute du paramètre ; ignorée si !present (peut être NULL).
// out       : rempli SEULEMENT si le retour est kParsed.
// Essaie D'ABORD le format brut "<64hex>" (celui envoyé historiquement par
// l'upload manuel), PUIS le format préfixé "sha256:<64hex>" (celui de l'API
// GitHub releases) — comportement ACCEPTÉ strictement identique à l'ancien
// code en ligne de web_routes_ota.cpp (feature-026) : l'ordre y était inversé
// (préfixe d'abord, repli brut ensuite) mais aboutit au même verdict pour
// toute entrée, le second essai de l'ancien code équivalant exactement au
// premier essai ici (prouvé par relecture, pas de test de non-régression
// dédié nécessaire au-delà des vecteurs de parseSha256Digest).
OtaSha256ParamVerdict parseOtaSha256Param(bool present, const char* digestStr, uint8_t out[32]);

// Verdict COMPLET d'authenticité. Lot B (feature-118) : kAbsent est refusé à
// index==0 (voir handleOtaUpdate) — il n'atteint donc plus non plus ce point
// au moment du `final`, AVANT que `computed` existe ; seul kNonConform y est
// encore tranché, via otaUploadIntegrityIsRejected ci-dessous (kMalformed
// était déjà refusé depuis feature-026, refus déplacé à index==0 en lot A —
// inchangé par ce lot).
enum class OtaUploadIntegrityVerdict : uint8_t {
  kAbsent = 0,
  kMalformed = 1,
  kConform = 2,
  kNonConform = 3
};

// Combine paramVerdict (ci-dessus) avec la comparaison expected/computed
// (ignorée sauf si paramVerdict == kParsed, où expected/computed ne doivent
// pas être NULL). Pur.
OtaUploadIntegrityVerdict otaUploadIntegrityVerdict(OtaSha256ParamVerdict paramVerdict,
                                                    const uint8_t expected[32],
                                                    const uint8_t computed[32]);

// Libellé FR stable (journalisation) : "absente" | "malformée" | "conforme" |
// "non conforme". Jamais nul.
const char* otaUploadIntegrityVerdictLabel(OtaUploadIntegrityVerdict verdict);

// true si ce verdict DOIT être refusé (feature-118 lot B), c.-à-d. tout sauf
// kConform. Anciennement `otaUploadWouldLotBReject` (lot A : journalisation
// seule, « lot B aurait refusé ») — renommé lot B car ce n'est plus une
// projection, c'est la DÉCISION effective appliquée par handleOtaUpdate.
bool otaUploadIntegrityIsRejected(OtaUploadIntegrityVerdict verdict);

#endif // OTA_INTEGRITY_LOGIC_H
