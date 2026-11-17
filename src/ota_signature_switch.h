#ifndef OTA_SIGNATURE_SWITCH_H
#define OTA_SIGNATURE_SWITCH_H

// =============================================================================
// Interrupteur de signature OTA — enveloppe NVS + miroir RAM (feature-151 D3-b2)
// =============================================================================
// D3-c : vocabulaire utilisateur « autorisation temporaire de mise à jour non signée »
// (« activée » / « retirée »). Les sites de web_routes_ota.cpp LISENT la décision — une
// autorisation activée LÈVE le refus d'authenticité (jamais la liaison d'empreinte) — et
// CONSOMMENT l'autorisation (« premier flash réussi »).
//
// Stockage : UNE clé U32 (échéance, epoch UTC) dans le namespace `pool-ota` (celui de
// D3-b1), ⛔ JAMAIS via config.cpp ni son mutex (interdit depuis `async_tcp`).
// L'armement est DÉRIVÉ : clé présente ET horloge valide ET now < échéance ET
// échéance - now <= kOtaSignatureSwitchWindowSec (C1).
//
// Appelants (outillés par tools/check_ota_signature_switch.sh) :
//   - Arm      : UN seul, la route POST /ota-signature-switch/arm (C6) ;
//   - Peek     : UNIQUEMENT mqtt_manager.cpp (C6) ;
//   - Read     : web_routes_ota.cpp (2 sites) + routes de l'interrupteur ;
//   - Consume  : web_routes_ota.cpp (3 sites EXACTEMENT, après Update.end(true)).
// =============================================================================

#include <stdint.h>

#include "ota_signature_switch_logic.h"

enum class OtaSwitchArmResult : uint8_t { kArmed = 0, kNoClock = 1, kWriteFailed = 2 };

/** @brief Lit la NVS une fois (setup, avant server->begin()). Miroir fermé par défaut. */
void otaSignatureSwitchInit();

/**
 * @brief Décision FAISANT AUTORITÉ : RELIT la NVS (lecture positive : isKey + type U32,
 *        C5), honore le verrou RAM « fermé » (C7). Appelable depuis `async_tcp`.
 */
OtaSignatureSwitchDecision otaSignatureSwitchRead();

/**
 * @brief Miroir O(1), sans E/S, pour mqttTask. ⛔ Jamais utilisé pour décider d'un flash.
 *        reason == kLocked : désarmement échoué, une échéance peut subsister en NVS (MQTT
 *        reste alors ON, C7 ; l'échéance résiduelle vient de otaSignatureSwitchResidualEpoch()).
 */
OtaSignatureSwitchDecision otaSignatureSwitchPeek();

/**
 * @brief Arme pour kOtaSignatureSwitchWindowSec. ⛔ UN seul appelant : la route d'armement (C6).
 *        kArmed seulement si la valeur a été écrite ET relue en NVS ; sinon l'état
 *        n'est PAS modifié. `untilOut` n'est écrit qu'en cas de succès.
 */
OtaSwitchArmResult otaSignatureSwitchArm(uint32_t* untilOut);

/**
 * @brief Désarme. Verrou RAM « fermé » posé AVANT l'écriture NVS ; repli à 0 si l'effacement
 *        échoue (C7). false si la NVS garde peut-être une échéance (CRITICAL journalisé).
 */
bool otaSignatureSwitchClear();

/**
 * @brief « Premier flash réussi » : désarme (mêmes garanties que Clear). 3 sites exactement.
 *        `sessionWasWaived` = décision CAPTURÉE au début de la session (C8) : si elle était
 *        « levée » et que l'interrupteur n'est plus armé (expiré/désarmé pendant le transfert),
 *        un WARNING le journalise. Ne révoque rien ; ne sert jamais à accorder.
 */
void otaSignatureSwitchConsume(bool sessionWasWaived);

/** Valeur de otaSignatureSwitchResidualEpoch() quand l'état de la NVS est INCONNU (begin() en échec). */
constexpr uint32_t kOtaSwitchResidualUnknown = 0xFFFFFFFFu;

/**
 * @brief Échéance résiduelle en NVS après un désarmement raté : 0 = aucune (constaté),
 *        kOtaSwitchResidualUnknown = INCONNUE (NVS inaccessible : ne se lit jamais « rien »),
 *        autre = échéance (epoch UTC, s). Pour le 500 et le GET « verrouillé ».
 */
uint32_t otaSignatureSwitchResidualEpoch();

/** Un redémarrage ré-armerait-il l'interrupteur ? (le verrou est en RAM, la NVS survit) */
enum class OtaSwitchRearm : uint8_t { kNo = 0, kYes = 1, kUnknown = 2 };
OtaSwitchRearm otaSignatureSwitchRearmsOnReboot();

#endif  // OTA_SIGNATURE_SWITCH_H
