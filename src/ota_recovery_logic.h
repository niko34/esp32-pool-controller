#ifndef OTA_RECOVERY_LOGIC_H
#define OTA_RECOVERY_LOGIC_H

// =============================================================================
// Décision PURE du déclencheur de la page de secours OTA (feature-151 D3-b1).
// Header sans dépendance Arduino, zéro allocation → testable en natif (ADR-0017).
//
// La page de secours remplace `/` et `/index.html` quand l'interface web ne peut
// plus être servie : filesystem non monté, mise à jour du filesystem interrompue
// (marqueur NVS), ou `index.html` absent d'un filesystem pourtant monté.
//
// ⚠️ Les entrées sont des FAITS que l'APPELANT établit. Cette fonction ne vaut
// que ce que valent ses entrées : c'est l'appelant qui doit sonder `index.html.gz`
// AVANT `index.html` (minify.js n'écrit que les variantes .gz) — voir
// fsPathResolvableIfHeapAllows() de fs_guard.h et tools/check_fs_probe_paths.sh.
// =============================================================================

#include <stdint.h>

enum class OtaRecoveryReason : uint8_t {
  kNone = 0,              // interface servie normalement
  kMountFailed,           // LittleFS non monté
  kFsUpdateInterrupted,   // marqueur NVS : une mise à jour du filesystem n'a pas abouti
  kIndexMissing,          // monté, mais ni index.html.gz ni index.html
};

struct OtaRecoveryInputs {
  bool fsMounted;          // le filesystem est monté
  bool indexProbed;        // la sonde d'index a RÉELLEMENT eu lieu (admission mémoire obtenue)
  bool indexPresent;       // valide seulement si indexProbed : l'index existe (.gz ou nu)
  bool markerReadable;     // la lecture du marqueur NVS a abouti
  bool fsUpdateInFlight;   // valeur du marqueur ; valide seulement si markerReadable
};

/**
 * @brief Décide si la page de secours doit être rendue, et pourquoi.
 *
 * RÈGLES DE REPLI — « échouer vers le comportement d'aujourd'hui, jamais vers
 * un faux secours » : `indexProbed == false` et `markerReadable == false`
 * n'arment JAMAIS le secours. Une famine mémoire ou une NVS illisible ne doivent
 * pas masquer l'interface d'un appareil sain.
 *
 * PRIORITÉ : le marqueur prime sur tout le reste (cause la plus informative, et
 * seul signal d'un montage RÉUSSI sur un contenu incohérent). En particulier
 * `kFsUpdateInterrupted` prime sur `kIndexMissing`.
 */
OtaRecoveryReason decideOtaRecoveryReason(const OtaRecoveryInputs& in);

/** @brief Libellé de journal du motif (littéral .rodata, jamais NULL). */
const char* otaRecoveryReasonLabel(OtaRecoveryReason reason);

// -----------------------------------------------------------------------------
// Pose / levée du marqueur « mise à jour du filesystem en cours » (condition S3)
// -----------------------------------------------------------------------------
// Le point de succès de handleOtaUpdate est PARTAGÉ entre les deux genres
// d'image. Un succès de mise à jour FIRMWARE ne doit JAMAIS lever le marqueur du
// FILESYSTEM (défaut trouvé en revue P1). La décision de genre vit donc ICI, en
// logique pure et testable, plutôt que dans un `if` de chaque site d'appel.
enum class OtaRecoveryUpdatePhase : uint8_t {
  kBeforeUnmount,  // juste avant LittleFS.end() : la partition va être détruite
  kSucceeded,      // l'image a été écrite ET validée jusqu'au bout
};

enum class OtaRecoveryMarkerAction : uint8_t { kNone = 0, kArm, kDisarm };

/**
 * @brief Action à appliquer au marqueur pour ce moment d'une mise à jour.
 * Genre FIRMWARE ⇒ toujours kNone, quelle que soit la phase.
 */
OtaRecoveryMarkerAction decideOtaRecoveryMarkerAction(bool isFilesystemUpdate,
                                                      OtaRecoveryUpdatePhase phase);

#endif  // OTA_RECOVERY_LOGIC_H
