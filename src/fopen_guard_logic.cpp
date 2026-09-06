#include "fopen_guard_logic.h"

// =============================================================================
// fopen_guard_logic — implémentation PURE (bug-100, ADR-0070)
// =============================================================================
// Aucun include hors le header : ni Arduino, ni FreeRTOS, ni String. Compilé
// tel quel par [env:native] pour être testé et validé par mutation.
// =============================================================================

bool fopenGuardShouldDefer(uint32_t maxAlloc8, uint32_t minAllocBytes) {
  // Frontière STRICTE. `minAllocBytes == 0` (kFsGuardAlreadyAdmitted) n'est
  // jamais refusé : l'admission a alors été prononcée en amont, sur une mesure
  // fraîche, au point où l'opération pouvait encore ne rien faire.
  return maxAlloc8 < minAllocBytes;
}

bool fopenGuardNeedsFreshMeasure(const FopenGuardEpisode& st, uint32_t nowMs,
                                 uint32_t quietMs, uint32_t probeFloorMs) {
  // Hors épisode : AUTORISER est le verdict probable, donc la mesure est due.
  // C'est le sens strict d'ADR-0058:99 — on ne décide jamais d'ouvrir sur une
  // valeur d'âge inconnu.
  if (!st.active) return true;

  // Épisode ouvert mais dernier relevé confortable (>= 2 × seuil) : le cache
  // n'est PAS armé. On re-mesure à chaque décision, ce qui interdit tout refus
  // périmé en régime normal — le cache ne coûte que là où il rapporte.
  if (!st.probeFloorArmed) return true;

  // Plancher DUR entre deux parcours TLSF : sous ce plancher, l'appelant refuse
  // sans mesurer. Refus périmé = sauvegarde différée ; c'est le côté bon marché
  // de l'asymétrie.
  if ((nowMs - st.lastProbeMs) < probeFloorMs) return false;

  // Rafale en cours (des refus arrivent encore) : inutile de payer le tas, la
  // famine est manifestement toujours là. On ne re-mesure qu'après `quietMs`
  // de silence, c'est-à-dire quand une SORTIE d'épisode devient plausible.
  return (nowMs - st.lastDeferralMs) >= quietMs;
}

FopenGuardAction fopenGuardDrain(FopenGuardEpisode& st, uint32_t deferCount,
                                 uint32_t nowMs, uint32_t maxAlloc8,
                                 uint32_t threshold, uint32_t escalateAfter) {
  if (deferCount != st.drainedDeferrals) {
    if (!st.active) {
      st.active = true;
      st.escalated = false;
      st.deferralsAtStart = st.drainedDeferrals;  // baseline = dernier état connu
      st.episodeStartMs = nowMs;
      st.drainedDeferrals = deferCount;
      st.lastDeferralMs = nowMs;
      return FopenGuardAction::EpisodeStart;
    }
    st.drainedDeferrals = deferCount;
    st.lastDeferralMs = nowMs;

    // Escalade : une seule fois par épisode, au N-ième refus cumulé. Soustraction
    // non signée — juste même si le compteur se replie (uint32 monotone).
    if (!st.escalated && escalateAfter != 0 &&
        (deferCount - st.deferralsAtStart) >= escalateAfter) {
      st.escalated = true;
      return FopenGuardAction::Escalate;
    }
    return FopenGuardAction::None;
  }

  // Fermeture : elle est une AUTORISATION (« on peut rouvrir »), elle exige donc
  // une mesure fraîche. La coquille passe 0 quand elle n'a pas mesuré, et 0 est
  // toujours sous le seuil : aucun épisode ne peut se clore sur une valeur
  // périmée. `episodeStartMs` / `deferralsAtStart` restent intacts pour que
  // l'appelant puisse chiffrer l'épisode qu'il vient de clore.
  if (st.active && !fopenGuardShouldDefer(maxAlloc8, threshold)) {
    st.active = false;
    st.probeFloorArmed = false;
    return FopenGuardAction::EpisodeEnd;
  }
  return FopenGuardAction::None;
}
