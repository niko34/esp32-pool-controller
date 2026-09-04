// =============================================================================
// purge_stability_logic — Détecteur PUR de stabilité des bruts pH/ORP
// (feature-091, v2.34.0)
// =============================================================================
// Voir purge_stability_logic.h pour les règles. Compile en natif ET sur cible —
// aucune dépendance Arduino, aucune allocation.
// =============================================================================

#include "purge_stability_logic.h"

#include <math.h>  // isnan, NAN

namespace {

// Bornage des paramètres aux tampons statiques (un paramètre hors borne ne
// doit jamais déborder — on le ramène silencieusement dans la borne).
uint8_t clampSamples(uint8_t v) {
  if (v < 1) return 1;
  if (v > kPurgeStabilityMaxSamplesPerMin) return kPurgeStabilityMaxSamplesPerMin;
  return v;
}
uint8_t clampWindow(uint8_t v) {
  if (v < 1) return 1;
  if (v > kPurgeStabilityMaxWindowMin) return kPurgeStabilityMaxWindowMin;
  return v;
}

void channelReset(PurgeChannelState& ch) {
  ch.minuteCount = 0;
  ch.medianCount = 0;
  ch.medianHead = 0;
  ch.everSampled = false;
  ch.rawMin = NAN;
  ch.rawMax = NAN;
  // Les tampons n'ont pas besoin d'être effacés : les compteurs bornent la lecture.
}

// Une lecture valide pour une voie : min/max, minute en cours, médiane si pleine.
void channelAdd(PurgeChannelState& ch, float v, uint8_t samplesPerMin, uint8_t windowMin) {
  if (!ch.everSampled) {
    ch.everSampled = true;
    ch.rawMin = v;
    ch.rawMax = v;
  } else {
    if (v < ch.rawMin) ch.rawMin = v;
    if (v > ch.rawMax) ch.rawMax = v;
  }
  if (ch.minuteCount < kPurgeStabilityMaxSamplesPerMin) {
    ch.minuteBuf[ch.minuteCount++] = v;
  }
  if (ch.minuteCount >= samplesPerMin) {
    const float med = purgeStabilityMedian(ch.minuteBuf, ch.minuteCount);
    ch.minuteCount = 0;
    ch.medians[ch.medianHead] = med;
    ch.medianHead = (uint8_t)((ch.medianHead + 1) % windowMin);
    if (ch.medianCount < windowMin) ch.medianCount++;
  }
}

// Fenêtre pleine ? (bornée au paramètre courant : si la fenêtre a été réduite
// entre deux appels, un anneau plus rempli reste « plein »).
bool channelFull(const PurgeChannelState& ch, uint8_t windowMin) {
  return ch.medianCount >= windowMin;
}

}  // namespace

float purgeStabilityMedian(const float* v, uint8_t n) {
  if (n == 0) return NAN;
  if (n > kPurgeStabilityMaxSamplesPerMin) n = kPurgeStabilityMaxSamplesPerMin;
  float tmp[kPurgeStabilityMaxSamplesPerMin];
  for (uint8_t i = 0; i < n; ++i) tmp[i] = v[i];
  // Tri par insertion (n ≤ 60 : trivial, sans récursion ni allocation).
  for (uint8_t i = 1; i < n; ++i) {
    const float key = tmp[i];
    int j = (int)i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      --j;
    }
    tmp[j + 1] = key;
  }
  if (n & 1U) return tmp[n / 2];
  return 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
}

void purgeStabilityReset(PurgeStabilityState& st) {
  channelReset(st.ph);
  channelReset(st.orp);
}

void purgeStabilityAddSample(PurgeStabilityState& st, const PurgeStabilityParams& p,
                             float phRaw, float orpRaw) {
  const uint8_t samples = clampSamples(p.samplesPerMin);
  const uint8_t window = clampWindow(p.windowMin);
  if (!isnan(phRaw)) channelAdd(st.ph, phRaw, samples, window);
  if (!isnan(orpRaw)) channelAdd(st.orp, orpRaw, samples, window);
}

float purgeChannelRange(const PurgeChannelState& ch) {
  if (ch.medianCount == 0) return NAN;
  float lo = ch.medians[0];
  float hi = ch.medians[0];
  for (uint8_t i = 1; i < ch.medianCount && i < kPurgeStabilityMaxWindowMin; ++i) {
    if (ch.medians[i] < lo) lo = ch.medians[i];
    if (ch.medians[i] > hi) hi = ch.medians[i];
  }
  return hi - lo;
}

uint8_t purgeChannelMinutes(const PurgeChannelState& ch) { return ch.medianCount; }
float purgeChannelRawMin(const PurgeChannelState& ch) { return ch.everSampled ? ch.rawMin : NAN; }
float purgeChannelRawMax(const PurgeChannelState& ch) { return ch.everSampled ? ch.rawMax : NAN; }

PurgeStabilityVerdict purgeStabilityEvaluate(const PurgeStabilityState& st,
                                             const PurgeStabilityParams& p) {
  const uint8_t window = clampWindow(p.windowMin);
  PurgeStabilityVerdict v;
  v.phRange = purgeChannelRange(st.ph);
  v.orpRange = purgeChannelRange(st.orp);

  const bool phFull = channelFull(st.ph, window);
  const bool orpFull = channelFull(st.orp, window);
  v.anyFull = phFull || orpFull;

  // Une voie jamais lue ne participe pas ; une voie lue mais incomplète n'est
  // ni stable ni instable (bloque l'ouverture, ne referme pas).
  bool allStable = true;
  bool anyUnstable = false;
  if (st.ph.everSampled) {
    if (!phFull || !(v.phRange <= p.phStableRange)) allStable = false;
    if (phFull && v.phRange > p.phRecloseRange) anyUnstable = true;
  }
  if (st.orp.everSampled) {
    if (!orpFull || !(v.orpRange <= p.orpStableRange)) allStable = false;
    if (orpFull && v.orpRange > p.orpRecloseRange) anyUnstable = true;
  }
  v.stable = v.anyFull && allStable;
  v.unstable = anyUnstable;
  return v;
}
