#ifndef OTA_SIGNATURE_SWITCH_LOGIC_H
#define OTA_SIGNATURE_SWITCH_LOGIC_H

// =============================================================================
// Interrupteur de signature OTA — décision PURE (feature-151 D3-b2, ADR-0017)
// =============================================================================
// Fonction UNIQUE de décision « la signature est-elle exigée ? ». Aucune E/S,
// aucune horloge cachée : tout est en ENTRÉE. Compilable en natif.
//
// ÉCHEC VERS « EXIGÉE » : 5 branches sur 6 rendent kRequired. Seule une échéance
// POSITIVEMENT lue, plausible, dans la fenêtre, avec une horloge valide, rend
// kWaived.
//
// ⛔ Les entrées ne contiennent NI état de montage, NI origine de requête, NI
// verdict de signature : l'interrupteur ne dépend d'aucun d'eux (B2/B3).
// ⛔ L'APPELANT peut mentir sur nvsReadable/entryPresent (leçon de la sonde
// /index.html de D3-b1) : c'est lui qui est audité (ota_signature_switch.cpp).
// =============================================================================

#include <stdint.h>

enum class OtaSignatureRequirement : uint8_t { kRequired = 0, kWaived = 1 };

enum class OtaSwitchReason : uint8_t {
  kNoEntry = 0,     // aucune échéance en NVS (état nominal)
  kUnreadable = 1,  // NVS illisible
  kNoClock = 2,     // heure non valide : on ne peut pas dater l'échéance
  kExpired = 3,     // échéance atteinte ou dépassée
  kArmed = 4,       // armé, dans la fenêtre
  kInvalid = 5,     // échéance 0 ou HORS fenêtre (> kOtaSignatureSwitchWindowSec) — C1
  kLocked = 6       // verrou RAM « fermé » (désarmement/consommation en cours ou échoué) — C7
};

struct OtaSignatureSwitchInputs {
  bool nvsReadable;      // la NVS a été ouverte avec succès
  bool entryPresent;     // lecture POSITIVE : la clé existe ET est de type U32
  uint32_t expiryEpoch;  // échéance (s, UTC) — JAMAIS millis()
  bool clockValid;       // time(nullptr) >= kMinValidEpoch
  uint32_t nowEpoch;
};

struct OtaSignatureSwitchDecision {
  OtaSignatureRequirement requirement;
  OtaSwitchReason reason;
  uint32_t remainingSec;  // 0 hors kArmed
  uint32_t untilEpoch;    // échéance (epoch UTC) en kArmed, 0 sinon
};

/**
 * @brief Décision unique. kWaived si et seulement si : NVS lisible, entrée présente,
 *        heure valide, échéance non nulle, now < échéance ET (échéance - now) <=
 *        windowSec (borne HAUTE, C1 : une horloge fausse vers le futur à l'armement
 *        ne doit jamais produire un armement durable).
 */
OtaSignatureSwitchDecision decideOtaSignatureSwitch(const OtaSignatureSwitchInputs& in,
                                                    uint32_t windowSec);

/** @brief Code stable (journal, JSON, MQTT). */
const char* otaSwitchReasonCode(OtaSwitchReason r);

#endif  // OTA_SIGNATURE_SWITCH_LOGIC_H
