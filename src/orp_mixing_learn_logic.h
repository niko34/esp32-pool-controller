#ifndef ORP_MIXING_LEARN_LOGIC_H
#define ORP_MIXING_LEARN_LOGIC_H

// =============================================================================
// orp_mixing_learn_logic — APPRENTISSAGE de la pause mélange ORP (feature-202)
// =============================================================================
// Logique PURE (ADR-0017) : pas d'Arduino.h, pas de millis(), pas de time(), pas
// de NVS. Le temps et la mesure sont des PARAMÈTRES ; header-only.
//
// MÉCANISME (trois phrases)
//   1. Après chaque injection de chlore, on compare la mesure filtrée à celle du
//      jalon PRÉCÉDENT, une fois toutes les W = kOrpMixingDelayMs. Dès qu'elle
//      ne monte plus, la montée est terminée : l'observation vaut k × W.
//   2. La durée en vigueur est le MAXIMUM des 3 dernières observations propres,
//      écrêté à [kOrpMixingDelayMs, kOrpMixingDelayLearnedMaxMs]. Ni médiane ni
//      moyenne (chaque observation est biaisée COURT : maximum seul cohérent).
//   3. Observation perturbée (propreté, mesure invalide) ou sans fin au plafond :
//      JETÉE, jamais enregistrée comme « très longue ».
//
// ⛔ W est la CONSTANTE kOrpMixingDelayMs, JAMAIS la durée en vigueur : si W
// suivait la durée apprise, toute observation serait mécaniquement >= la durée
// courante et la pause ne pourrait plus jamais redescendre (cliquet).
// =============================================================================

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"
#include "ph_persist_logic.h"  // phPersistFnv1a UNIQUEMENT

constexpr uint32_t kOrpMixingWindowMs = (uint32_t)kOrpMixingDelayMs;  // W

struct OrpMixingObservation {
  bool     active     = false;
  uint32_t nextMarkMs = 0;     // instant du prochain jalon (= ouverture + k·W)
  uint8_t  marks      = 0;     // jalons franchis depuis la fin d'injection
  float    refOrp     = 0.0f;  // mesure au jalon PRÉCÉDENT (k=0 ⇒ fin d'injection)
};

struct OrpMixingLearnStore {
  uint32_t obsMs[kOrpMixingLearnObsCount] = {0, 0, 0};  // anneau, [2] = plus récente
  uint8_t  count = 0;                                    // saturé à 255
};

struct OrpMixingTickInputs {
  uint32_t nowMs = 0;
  float    orpFiltered = 0.0f;  // NaN = non représentatif
  bool     clean = false;       // conditions de propreté réunies CE TICK
};

enum class OrpMixingOutcome : uint8_t { kIdle, kWatching, kClosed, kDiscarded };
enum class OrpMixingDiscard : uint8_t { kNone, kDirty, kInvalidMeasure, kCeiling };

struct OrpMixingTickVerdict {
  OrpMixingOutcome outcome = OrpMixingOutcome::kIdle;
  uint32_t         observedMs = 0;
  OrpMixingDiscard discard = OrpMixingDiscard::kNone;
};

/** @brief Ouvre l'observation à la FIN d'une injection (mesure `orpAtEnd`, NaN toléré :
 *  le tick jettera alors l'observation comme « mesure invalide »). */
inline void orpMixingOpen(OrpMixingObservation& o, uint32_t nowMs, float orpAtEnd) {
  o.active = true;
  o.nextMarkMs = nowMs + kOrpMixingWindowMs;
  o.marks = 0;
  o.refOrp = orpAtEnd;
}

/** @brief Un pas d'observation. Wrap-safe (arithmétique signée sur millis()). */
inline OrpMixingTickVerdict orpMixingTick(OrpMixingObservation& o, const OrpMixingTickInputs& in) {
  OrpMixingTickVerdict v;
  if (!o.active) return v;  // kIdle

  auto discard = [&](OrpMixingDiscard why) {
    o.active = false;
    v.outcome = OrpMixingOutcome::kDiscarded;
    v.discard = why;
    return v;
  };
  if (!in.clean) return discard(OrpMixingDiscard::kDirty);
  if (isnan(in.orpFiltered) || isnan(o.refOrp)) return discard(OrpMixingDiscard::kInvalidMeasure);

  v.outcome = OrpMixingOutcome::kWatching;
  if ((int32_t)(in.nowMs - o.nextMarkMs) < 0) return v;  // jalon pas encore atteint

  ++o.marks;  // un jalon franchi
  if ((uint32_t)o.marks * kOrpMixingWindowMs > kOrpMixingDelayLearnedMaxMs) {
    return discard(OrpMixingDiscard::kCeiling);  // ⛔ jamais « très longue »
  }
  if (in.orpFiltered > o.refOrp) {  // montée toujours en cours
    o.refOrp = in.orpFiltered;
    o.nextMarkMs += kOrpMixingWindowMs;
    return v;  // kWatching
  }
  o.active = false;
  v.outcome = OrpMixingOutcome::kClosed;
  v.observedMs = (uint32_t)o.marks * kOrpMixingWindowMs;  // k × W, k ∈ [1, 6]
  return v;
}

/** @brief Enregistre une observation CLOSE (anneau de 3, la plus récente en [2]). */
inline void orpMixingRecord(OrpMixingLearnStore& s, uint32_t observedMs) {
  for (uint8_t i = 0; i + 1 < kOrpMixingLearnObsCount; ++i) s.obsMs[i] = s.obsMs[i + 1];
  s.obsMs[kOrpMixingLearnObsCount - 1] = observedMs;
  if (s.count < 255) ++s.count;
}

/** @brief Durée en vigueur : exactement W sous 3 observations, sinon le MAXIMUM
 *  (nuls/absurdes rejetés) écrêté VERS LE HAUT à W et au plafond. */
inline uint32_t orpMixingEffectiveDelayMs(const OrpMixingLearnStore& s) {
  if (s.count < kOrpMixingLearnObsCount) return kOrpMixingWindowMs;
  uint32_t best = 0;
  for (uint8_t i = 0; i < kOrpMixingLearnObsCount; ++i) {
    const uint32_t v = s.obsMs[i];
    if (v == 0 || v > kOrpMixingDelayLearnedMaxMs) continue;  // nul / absurde
    if (v > best) best = v;
  }
  if (best < kOrpMixingWindowMs) best = kOrpMixingWindowMs;  // plancher (ceinture-bretelles)
  if (best > kOrpMixingDelayLearnedMaxMs) best = kOrpMixingDelayLearnedMaxMs;
  return best;
}

inline bool orpMixingIsLearned(const OrpMixingLearnStore& s) {
  return s.count >= kOrpMixingLearnObsCount;
}

/** @brief A6 : le détecteur de sonde figée reste plus court que la pause EN VIGUEUR. */
inline bool orpMixingFrozenInvariantHolds(uint32_t effectiveDelayMs) {
  return (uint32_t)kSensorFrozenSamples * (uint32_t)kPhOrpSensorIntervalMs <= effectiveDelayMs;
}

inline const char* orpMixingDiscardLabel(OrpMixingDiscard d) {
  switch (d) {
    case OrpMixingDiscard::kDirty:          return "fenêtre perturbée";
    case OrpMixingDiscard::kInvalidMeasure: return "mesure non représentative";
    case OrpMixingDiscard::kCeiling:        return "montée sans fin au plafond";
    default:                                return "aucun";
  }
}

// -----------------------------------------------------------------------------
// INSTANTANÉ NVS (20 octets, POD) — namespace `pool-dose`, clé `orpmix`
// ⛔ Ni empreinte de contexte, ni epoch, ni péremption : grandeur hydraulique.
// -----------------------------------------------------------------------------
struct OrpMixingSnapshot {
  uint8_t  schema = kOrpMixingLearnSchema;
  uint8_t  count = 0;
  uint16_t reserved = 0;  // NORMALISÉ au scellement
  uint32_t obsMs[kOrpMixingLearnObsCount] = {0, 0, 0};
  uint32_t crc = 0;       // FNV-1a des 16 octets AMONT
};
static_assert(sizeof(OrpMixingSnapshot) == 20,
              "OrpMixingSnapshot : 20 octets attendus — incrémenter kOrpMixingLearnSchema si changé");

inline uint32_t orpMixingSnapshotCrc(const OrpMixingSnapshot& s) {
  return phPersistFnv1a(&s, offsetof(OrpMixingSnapshot, crc));
}

inline void orpMixingSealSnapshot(OrpMixingSnapshot& s) {
  s.reserved = 0;
  s.crc = orpMixingSnapshotCrc(s);
}

/** @brief Taille EXACTE + schéma + CRC, sinon rejet net (store vide ⇒ 20 min). */
inline bool orpMixingSnapshotIntact(const OrpMixingSnapshot& s, size_t bytesRead) {
  if (bytesRead != sizeof(OrpMixingSnapshot)) return false;
  if (s.schema != kOrpMixingLearnSchema) return false;
  return orpMixingSnapshotCrc(s) == s.crc;
}

inline OrpMixingSnapshot orpMixingSnapshotFromStore(const OrpMixingLearnStore& st) {
  OrpMixingSnapshot s;
  s.count = st.count;
  for (uint8_t i = 0; i < kOrpMixingLearnObsCount; ++i) s.obsMs[i] = st.obsMs[i];
  orpMixingSealSnapshot(s);
  return s;
}

inline OrpMixingLearnStore orpMixingStoreFromSnapshot(const OrpMixingSnapshot& s) {
  OrpMixingLearnStore st;
  st.count = s.count;
  for (uint8_t i = 0; i < kOrpMixingLearnObsCount; ++i) st.obsMs[i] = s.obsMs[i];
  return st;
}

#endif  // ORP_MIXING_LEARN_LOGIC_H
