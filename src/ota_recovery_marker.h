#ifndef OTA_RECOVERY_MARKER_H
#define OTA_RECOVERY_MARKER_H

// =============================================================================
// Marqueur persistant + état du déclencheur de la page de secours OTA
// (feature-151 D3-b1)
// =============================================================================
// Le marqueur « une mise à jour du FILESYSTEM est en cours » vit en NVS
// (namespace dédié `pool-ota`, clé `fsupd`), à 0x9000 : HORS de la partition
// `spiffs`, il SURVIT donc à sa destruction. C'est le seul signal d'un montage
// RÉUSSI sur un contenu incohérent, que ni le montage ni la sonde d'index ne voient.
//
// ⛔ ASYMÉTRIE VOULUE, vérifiée par tools/check_degraded_mode_absent.sh :
//   - l'interface d'ÉCRITURE (otaRecoveryOnUpdatePhase, otaRecoveryNoteTransition)
//     est appelée par web_routes_ota.cpp ;
//   - l'interface de LECTURE (otaRecoveryActive) n'est appelée QUE par
//     web_routes_recovery.cpp, UNE fois. Ni auth.*, ni otaUploadAuthorized, ni
//     REQUIRE_AUTH, ni le prédicat de signature ne la lisent : le secours n'est
//     PAS un mode, aucune décision de sécurité n'en dépend.
//
// ÉCHEC VERS LE COMPORTEMENT D'AUJOURD'HUI : une écriture NVS en échec laisse la
// copie RAM inchangée (l'entrée est ignorée), une lecture NVS en échec rend le
// marqueur « illisible » (jamais « en cours »).
// =============================================================================

#include "ota_recovery_logic.h"

/** @brief Lit le marqueur NVS une fois (setup). À appeler avant server->begin(). */
void otaRecoveryInit();

/**
 * @brief Pose ou lève le marqueur selon le genre et la phase (S3). Sans effet pour
 *        une image firmware. Pose/levée = écriture NVS (watchdog nourri autour) et
 *        marque l'état « à réévaluer ». Appelable depuis `async_tcp`.
 */
void otaRecoveryOnUpdatePhase(bool isFilesystemUpdate, OtaRecoveryUpdatePhase phase);

/** @brief Le montage a changé (remontage réussi OU raté) : marque l'état à réévaluer. */
void otaRecoveryNoteTransition();

/**
 * @brief Réévalue le déclencheur si une transition l'a invalidé — LOOP uniquement
 *        (sonde d'index = fopen gardé, journal). Réévalué à chaque transition
 *        (S4) : le secours se lève / s'arme SANS redémarrage.
 */
void otaRecoveryDrain();

/** @brief Lecture O(1), sans E/S, sans allocation : la page de secours est-elle due ? */
bool otaRecoveryActive();

/** @brief Motif courant (kNone si inactive). Sans E/S. */
OtaRecoveryReason otaRecoveryReason();

#endif  // OTA_RECOVERY_MARKER_H
