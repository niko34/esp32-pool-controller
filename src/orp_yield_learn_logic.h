#ifndef ORP_YIELD_LEARN_LOGIC_H
#define ORP_YIELD_LEARN_LOGIC_H

// =============================================================================
// orp_yield_learn_logic — RENDEMENT du chlore appris sur le bassin (feature-203)
// =============================================================================
// Logique PURE (ADR-0017) : pas d'Arduino.h, pas de millis(), pas de time(), pas
// de NVS. Header-only.
//
// MÉCANISME
//   1. Un ÉPISODE s'ouvre à la 1re injection de chlore, cumule tout le volume, et
//      se ferme sur le VERDICT de l'observation de feature-202 (la montée est finie) :
//      le plateau est la mesure filtrée au sommet. Rendement = (plateau - ouverture)
//      / volume, en mV/mL.
//   2. Médiane de 3 épisodes (ni moyenne, ni minimum). Aucun rejet d'aberrant : la
//      médiane de 3 en encaisse un, l'alerte de sous-dosage rattrape le reste.
//   3. Budget d'épisode = marge x manque_courant / rendement, appliqué UNIQUEMENT si
//      le manque courant n'excède pas la médiane des manques déjà observés.
//
// ⛔ Ce budget n'est PAS une limite de sécurité (les plafonds protègent, le reste
// informe) : il ne fait qu'ARRÊTER PLUS TÔT, jamais plus tard.
// =============================================================================

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"
#include "ph_persist_logic.h"  // phPersistFnv1a UNIQUEMENT

struct OrpYieldEpisode {
  bool     open = false;
  bool     dirty = false;       // ouvert sans mesure/manque exploitable : sera JETÉ
  float    volMl = 0.0f;        // volume chlore cumulé depuis l'ouverture
  float    openOrp = NAN;       // mesure filtrée d'AVANT la 1re commande
  uint16_t openDeficitMv = 0;   // manque à l'ouverture (mV)
};

struct OrpYieldStore {
  float    yieldMvPerMl[kOrpYieldLearnObsCount] = {0.0f, 0.0f, 0.0f};  // [2] = plus récent
  uint16_t openDeficitMv[kOrpYieldLearnObsCount] = {0, 0, 0};
  uint8_t  count = 0;  // saturé à 255
};

enum class OrpYieldKind : uint8_t { kCredited, kDiscarded };
enum class OrpYieldReason : uint8_t { kNone, kDirty, kNoRise, kTooSmall, kInvalid };

struct OrpYieldVerdict {
  OrpYieldKind   kind = OrpYieldKind::kDiscarded;
  OrpYieldReason reason = OrpYieldReason::kInvalid;
  float          yieldMvPerMl = 0.0f;  // écrêté si crédité
  bool           capped = false;       // écrêtage HAUT appliqué
  uint16_t       openDeficitMv = 0;
};

/** @brief Ouvre l'épisode. `deficitMv` = consigne - mesure d'ouverture. Mesure NaN ou
 *  manque <= 0 ⇒ épisode marqué sale (JETÉ à la clôture, aucune division). */
inline void orpYieldOpen(OrpYieldEpisode& e, float openOrp, float deficitMv) {
  e.open = true;
  e.volMl = 0.0f;
  e.openOrp = openOrp;
  e.dirty = !(isfinite(openOrp) && isfinite(deficitMv) && deficitMv > 0.0f);
  float d = (deficitMv > 0.0f && isfinite(deficitMv)) ? deficitMv : 0.0f;
  if (d > 65535.0f) d = 65535.0f;
  e.openDeficitMv = (uint16_t)d;
}

inline void orpYieldAddVolume(OrpYieldEpisode& e, float volMl) {
  if (volMl > 0.0f && isfinite(volMl)) e.volMl += volMl;
}

inline void orpYieldReset(OrpYieldEpisode& e) { e = OrpYieldEpisode(); }

/** @brief Ferme l'épisode au PLATEAU. Jeté si sale, montée <= 0, ou volume sous la
 *  plus petite dose que l'actionneur sache délivrer. Remet l'épisode à zéro. */
inline OrpYieldVerdict orpYieldClose(OrpYieldEpisode& e, float plateau) {
  OrpYieldVerdict v;
  const float vol = e.volMl;
  const float rise = plateau - e.openOrp;
  v.openDeficitMv = e.openDeficitMv;
  const bool wasDirty = e.dirty || !e.open;
  orpYieldReset(e);
  if (wasDirty) { v.reason = OrpYieldReason::kDirty; return v; }
  if (!isfinite(rise) || !isfinite(vol)) { v.reason = OrpYieldReason::kInvalid; return v; }
  if (!(rise > 0.0f)) { v.reason = OrpYieldReason::kNoRise; return v; }  // ⛔ aucune division
  if (vol < kPulseMinVolumeMl) { v.reason = OrpYieldReason::kTooSmall; return v; }
  float y = rise / vol;
  if (!isfinite(y) || !(y > 0.0f)) { v.reason = OrpYieldReason::kInvalid; return v; }
  if (y > kOrpYieldMaxMvPerMl) { y = kOrpYieldMaxMvPerMl; v.capped = true; }
  v.kind = OrpYieldKind::kCredited;
  v.reason = OrpYieldReason::kNone;
  v.yieldMvPerMl = y;
  return v;
}

/** @brief Enregistre un épisode CRÉDITÉ (anneau de 3, le plus récent en [2]). */
inline void orpYieldRecord(OrpYieldStore& s, const OrpYieldVerdict& v) {
  if (v.kind != OrpYieldKind::kCredited) return;
  for (uint8_t i = 0; i + 1 < kOrpYieldLearnObsCount; ++i) {
    s.yieldMvPerMl[i] = s.yieldMvPerMl[i + 1];
    s.openDeficitMv[i] = s.openDeficitMv[i + 1];
  }
  s.yieldMvPerMl[kOrpYieldLearnObsCount - 1] = v.yieldMvPerMl;
  s.openDeficitMv[kOrpYieldLearnObsCount - 1] = v.openDeficitMv;
  if (s.count < 255) ++s.count;
}

inline bool orpYieldIsLearned(const OrpYieldStore& s) { return s.count >= kOrpYieldLearnObsCount; }

// Valeur du milieu de trois (kOrpYieldLearnObsCount == 3, verrouillé par static_assert).
inline float orpYieldMid3(float a, float b, float c) {
  if (a > b) { const float t = a; a = b; b = t; }
  if (b > c) { const float t = b; b = c; c = t; }
  if (a > b) { b = a; }
  return b;
}

/** @brief Médiane des 3 rendements ; NaN sous 3 épisodes crédités. */
inline float orpYieldMedian(const OrpYieldStore& s) {
  if (!orpYieldIsLearned(s)) return NAN;
  return orpYieldMid3(s.yieldMvPerMl[0], s.yieldMvPerMl[1], s.yieldMvPerMl[2]);
}

/** @brief Médiane des 3 manques à l'ouverture (mV) ; NaN sous 3 épisodes. */
inline float orpYieldDeficitMedian(const OrpYieldStore& s) {
  if (!orpYieldIsLearned(s)) return NAN;
  return orpYieldMid3((float)s.openDeficitMv[0], (float)s.openDeficitMv[1],
                      (float)s.openDeficitMv[2]);
}

/** @brief Budget de volume (mL) pour le manque COURANT. NaN (= PAS DE BUDGET, comportement
 *  d'avant) si : rendement non appris, manque indéfini/<= 0, ou manque HORS du domaine
 *  mesuré (> médiane des manques observés). ⛔ Jamais 0 par défaut d'entrée. */
inline float orpYieldBudgetMl(float deficitMv, const OrpYieldStore& s) {
  const float y = orpYieldMedian(s);
  if (!isfinite(y) || !(y > 0.0f)) return NAN;
  if (!isfinite(deficitMv) || !(deficitMv > 0.0f)) return NAN;
  const float dom = orpYieldDeficitMedian(s);
  if (!isfinite(dom) || deficitMv > dom) return NAN;  // hors domaine appris
  const float b = kOrpYieldBudgetMargin * deficitMv / y;
  return isfinite(b) ? b : NAN;
}

/** @brief true ⇔ le budget arrête l'injection : fini ET volume atteint. */
inline bool orpYieldBudgetReached(float budgetMl, float volMl) {
  return isfinite(budgetMl) && volMl >= budgetMl;
}

/** @brief Empreinte de contexte : {schéma, pompe ORP, consigne CONFIGURÉE}. ⛔ Jamais la
 *  consigne effective (le Mode Boost effacerait l'apprentissage à chaque activation). */
inline uint32_t orpYieldContextHash(float configuredTarget, uint8_t orpPump) {
  struct __attribute__((packed)) Blob {
    uint8_t schema;
    uint8_t pump;
    uint16_t pad;
    float   target;
  } blob;
  blob.schema = kOrpYieldLearnSchema;
  blob.pump = orpPump;
  blob.pad = 0;
  blob.target = configuredTarget;
  return phPersistFnv1a(&blob, sizeof(blob));
}

// --- Alerte de sous-dosage chronique (C4) ------------------------------------
/** @brief Suite des sessions arrêtées par le budget. Une session arrêtée par le budget
 *  l'allonge ; une mesure qui atteint la consigne la remet à zéro. */
inline uint8_t orpYieldStreakNext(uint8_t streak, bool stoppedByBudget, bool reachedTarget) {
  if (reachedTarget) return 0;
  if (stoppedByBudget && streak < 255) return (uint8_t)(streak + 1);
  return streak;
}

/** @brief Sous-dosage chronique : les kOrpYieldLearnObsCount dernières sessions ont
 *  toutes été arrêtées par le budget sans que la consigne soit atteinte. */
inline bool orpYieldUndersupply(uint8_t streak) { return streak >= kOrpYieldLearnObsCount; }

// -----------------------------------------------------------------------------
// INSTANTANÉ NVS (32 octets, POD) — namespace `pool-dose`, clé `orpyld`
// -----------------------------------------------------------------------------
struct OrpYieldSnapshot {
  uint8_t  schema = kOrpYieldLearnSchema;
  uint8_t  count = 0;
  uint16_t reserved = 0;   // NORMALISÉ au scellement
  uint32_t context = 0;    // empreinte FNV-1a
  float    yieldMvPerMl[kOrpYieldLearnObsCount] = {0.0f, 0.0f, 0.0f};
  uint16_t openDeficitMv[kOrpYieldLearnObsCount] = {0, 0, 0};
  uint16_t reserved2 = 0;
  uint32_t crc = 0;        // FNV-1a des 28 octets AMONT
};
static_assert(sizeof(OrpYieldSnapshot) == 32,
              "OrpYieldSnapshot : 32 octets attendus — incrémenter kOrpYieldLearnSchema si changé");

inline uint32_t orpYieldSnapshotCrc(const OrpYieldSnapshot& s) {
  return phPersistFnv1a(&s, offsetof(OrpYieldSnapshot, crc));
}

inline void orpYieldSealSnapshot(OrpYieldSnapshot& s) {
  s.reserved = 0;
  s.reserved2 = 0;
  s.crc = orpYieldSnapshotCrc(s);
}

/** @brief Taille EXACTE + schéma + CRC + rendements finis, positifs et écrêtés. */
inline bool orpYieldSnapshotIntact(const OrpYieldSnapshot& s, size_t bytesRead) {
  if (bytesRead != sizeof(OrpYieldSnapshot)) return false;
  if (s.schema != kOrpYieldLearnSchema) return false;
  if (orpYieldSnapshotCrc(s) != s.crc) return false;
  const uint8_t n = s.count < kOrpYieldLearnObsCount ? s.count : kOrpYieldLearnObsCount;
  for (uint8_t i = kOrpYieldLearnObsCount - n; i < kOrpYieldLearnObsCount; ++i) {
    const float y = s.yieldMvPerMl[i];
    if (!isfinite(y) || !(y > 0.0f) || y > kOrpYieldMaxMvPerMl) return false;
  }
  return true;
}

inline OrpYieldSnapshot orpYieldSnapshotFromStore(const OrpYieldStore& st, uint32_t context) {
  OrpYieldSnapshot s;
  s.count = st.count;
  s.context = context;
  for (uint8_t i = 0; i < kOrpYieldLearnObsCount; ++i) {
    s.yieldMvPerMl[i] = st.yieldMvPerMl[i];
    s.openDeficitMv[i] = st.openDeficitMv[i];
  }
  orpYieldSealSnapshot(s);
  return s;
}

inline OrpYieldStore orpYieldStoreFromSnapshot(const OrpYieldSnapshot& s) {
  OrpYieldStore st;
  st.count = s.count;
  for (uint8_t i = 0; i < kOrpYieldLearnObsCount; ++i) {
    st.yieldMvPerMl[i] = s.yieldMvPerMl[i];
    st.openDeficitMv[i] = s.openDeficitMv[i];
  }
  return st;
}

inline const char* orpYieldReasonLabel(OrpYieldReason r) {
  switch (r) {
    case OrpYieldReason::kDirty:    return "épisode perturbé";
    case OrpYieldReason::kNoRise:   return "mesure sans montée";
    case OrpYieldReason::kTooSmall: return "volume sous la plus petite dose";
    case OrpYieldReason::kInvalid:  return "mesure non représentative";
    default:                        return "aucun";
  }
}

#endif  // ORP_YIELD_LEARN_LOGIC_H
