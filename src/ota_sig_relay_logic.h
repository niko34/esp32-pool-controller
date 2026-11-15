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

// =============================================================================
// bug-176 (ADR-0123) : décision PURE du relais à la demande GET /ota-sig.
// Un handler HTTP ouvre AU PLUS UNE session TLS sortante ; /check-update ne
// relaie plus rien, il MÉMORISE les URL, et la page demande chaque signature
// juste avant usage. Ce module ne contient que de la décision (testable en
// natif, ADR-0017) : aucune E/S, aucun état.
//
// ⭐ Condition S2 : « absent » n'est émis QUE si la réponse GitHub a été
// analysée AVEC SUCCÈS et ne contient pas d'asset de ce nom (kCheckedNoAsset).
// Tout autre doute est « unavailable » AVEC son motif — sinon le refus D3-c de
// feature-151 frapperait au hasard.
// =============================================================================

enum class OtaSigRelayStatus : uint8_t {
  kAbsent = 0,       // release analysée, aucun asset .sig de ce nom
  kDeferred = 1,     // URL mémorisée, signature à demander via /ota-sig
  kRelayed = 2,      // signature récupérée et validée
  kUnavailable = 3,  // doute : voir OtaSigRelayReason
};

// Énumération FERMÉE (revue de sécurité) : jamais de texte libre dans `reason`.
enum class OtaSigRelayReason : uint8_t {
  kNone = 0,
  kNoAsset = 1,     // réservé à kAbsent
  kNoCheck = 2,     // /check-update jamais exécuté, échoué, ou emplacement vidé
  kUrlTooLong = 3,  // URL ou tag trop long pour l'emplacement mémorisé
  kHost = 4,        // hôte hors liste blanche
  kMemory = 5,      // garde TLS fermée (mémoire)
  kTime = 6,        // horloge non synchronisée
  kHttp = 7,        // begin()/GET en échec ou code HTTP != 200
  kTimeout = 8,     // lecture du corps hors délai
  kGrammar = 9,     // corps trop long ou hors grammaire
  kThrottled = 10,  // intervalle minimal entre deux sessions non écoulé
};

struct OtaSigRelayDecision {
  OtaSigRelayStatus status;
  OtaSigRelayReason reason;
};

// Code machine stable du statut : "absent" | "deferred" | "relayed" | "unavailable".
const char* otaSigRelayStatusCode(OtaSigRelayStatus s);
// Code machine stable du motif : "" | "no_asset" | "no_check" | "url_too_long" |
// "host" | "memory" | "time" | "http" | "timeout" | "grammar" | "throttled".
const char* otaSigRelayReasonCode(OtaSigRelayReason r);

// Paramètre `kind` de /ota-sig : égalité EXACTE, sensible à la casse, sans
// préfixe ni espace. "firmware" -> 0, "filesystem" -> 1. `value` n'est pas
// nécessairement NUL-terminé. N'écrit dans *kindIndexOut que sur succès.
bool otaSigKindFromParam(const char* value, size_t len, uint8_t* kindIndexOut);

// Ce que /check-update a su, pour un genre, de la release analysée.
enum class OtaSigSourceState : uint8_t {
  kNeverChecked = 0,  // état initial / /check-update échoué : on ne sait RIEN
  kCheckedNoAsset = 1,  // réponse GitHub analysée avec succès, pas d'asset .sig
  kTooLong = 2,         // asset présent mais URL/tag trop long pour l'emplacement
  kHostRefused = 3,     // asset présent mais hôte hors liste blanche
  kPresent = 4,         // URL + tag mémorisés
};

struct OtaSigSourceDecision {
  OtaSigRelayDecision decision;
  bool proceed;  // vrai seulement pour kPresent : une session TLS est permise
};

// Décision AVANT toute connexion, à partir de l'état mémorisé. Seul
// kCheckedNoAsset donne kAbsent (S2) ; kPresent donne kDeferred + proceed.
OtaSigSourceDecision otaSigSourceDecide(OtaSigSourceState state);

// Issue d'une tentative de relais effectivement lancée.
enum class OtaSigFetchOutcome : uint8_t {
  kValidated = 0,
  kHostRefused,
  kGateClosed,
  kBeginFailed,
  kHttpError,
  kBodyTooLong,
  kTimedOut,
  kBadGrammar,
};

// Issue de relais -> (status, reason). Seule kValidated donne kRelayed.
OtaSigRelayDecision otaSigRelayDecide(OtaSigFetchOutcome outcome);

// Anti-déni (S4) : vrai si une nouvelle session doit être REFUSÉE (sans
// connexion) parce que la précédente est trop récente. `hasLast` faux = aucune
// session encore tentée. Arithmétique non signée : sûre au repliement de millis().
bool otaSigThrottled(bool hasLast, uint32_t lastAttemptMs, uint32_t nowMs, uint32_t minIntervalMs);

#endif  // OTA_SIG_RELAY_LOGIC_H
