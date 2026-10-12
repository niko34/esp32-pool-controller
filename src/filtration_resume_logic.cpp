// =============================================================================
// filtration_resume_logic — Reprise de la durée de filtration PURE (feature-158)
// =============================================================================
// Voir filtration_resume_logic.h pour la frontière et les invariants. Compile
// en natif (pio test -e native) ET sur cible — aucune dépendance Arduino.
// =============================================================================

#include "filtration_resume_logic.h"

#include <stddef.h>  // offsetof

uint32_t filtrationRtcFnv1a(const void* data, uint32_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t h = 2166136261UL;
  for (uint32_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

uint32_t filtrationRtcCrc(const FiltrationRtcRecord& r) {
  return filtrationRtcFnv1a(&r, (uint32_t)offsetof(FiltrationRtcRecord, crc));
}

void filtrationRtcSeal(FiltrationRtcRecord& r) {
  r.magic = kFiltrationRtcMagic;
  r.reserved[0] = 0;
  r.reserved[1] = 0;
  r.crc = filtrationRtcCrc(r);
}

bool filtrationRtcIntact(const FiltrationRtcRecord& r) {
  return r.magic == kFiltrationRtcMagic && r.crc == filtrationRtcCrc(r);
}

FiltrationResumeResult evaluateFiltrationResume(const FiltrationResumeInputs& in) {
  FiltrationResumeResult out;
  out.carryS = 0;
  out.motif = FiltrationResumeMotif::kNoRecord;
  out.coupureDureeS = 0;

  // Condition 13 du gate — drapeau "verdict produit" explicite : si
  // filtration.begin() consomme avant que measure_validity.begin() n'ait
  // tourné, on refuse plutôt que de deviner.
  if (in.verdict == nullptr) {
    out.motif = FiltrationResumeMotif::kVerdictNotReady;
    return out;
  }

  // Condition 2 du gate — preuve PROPRE à la filtration : JAMAIS déduite du
  // `waterPresent` d'un verdict de continuité (OutageContinuityVerdict n'a
  // d'ailleurs pas ce champ — seul `waive`/`outageS`/`deny` y figurent).
  if (in.record == nullptr || !filtrationRtcIntact(*in.record)) {
    out.motif = FiltrationResumeMotif::kNoRecord;
    return out;
  }
  const FiltrationRtcRecord& r = *in.record;

  if (r.filtrationRunning != 1) {
    out.motif = FiltrationResumeMotif::kNotRunning;
    return out;
  }
  const bool recordedManaged =
      static_cast<InstallMode>(r.installMode) == InstallMode::ManagedFiltration;
  const bool currentlyManaged = in.installMode == InstallMode::ManagedFiltration;
  if (!recordedManaged || !currentlyManaged) {
    out.motif = FiltrationResumeMotif::kNotManaged;
    return out;
  }

  // Condition 3 du gate — consommation UNE SEULE FOIS, et seulement si ce
  // boot vient de décider de (re)démarrer la filtration : c'est ce qui exclut
  // le Boost par construction (il ne repasse jamais par begin()).
  if (!in.initialState) {
    // ⚠️ INATTEIGNABLE DEPUIS begin() AUJOURD'HUI : le seul appelant y pose
    // `initialState = true` en dur, donc la ligne de journal correspondante ne
    // peut pas s'imprimer. Ce refus protège un FUTUR appelant, et le test M4 le
    // verrouille. Relevé en revue le 2026-10-12 — ne pas le supprimer en le
    // croyant mort, et ne pas s'étonner de ne jamais voir son libellé.
    out.motif = FiltrationResumeMotif::kNotInitialRun;
    return out;
  }

  // ⛔ On CONSOMME le verdict, on n'en recalcule aucun : `waive` est la SEULE
  // information lue ici. Aucune comparaison de durée de coupure (`outageS`
  // face à une grâce) n'a lieu dans ce module.
  if (!in.verdict->waive) {
    out.motif = FiltrationResumeMotif::kDenied;
    out.coupureDureeS = in.verdict->outageS;  // journal uniquement
    return out;
  }

  // Condition 5 du gate — borne de vraisemblance : retour à ZÉRO, jamais
  // écrêtage (un report qu'on ne croit plus n'est pas réduit, il est jeté).
  if (r.carryS > kFiltrationResumeMaxPlausibleS) {
    out.motif = FiltrationResumeMotif::kImplausible;
    out.coupureDureeS = in.verdict->outageS;  // journal uniquement
    return out;
  }

  out.carryS = r.carryS;
  out.motif = FiltrationResumeMotif::kResumed;
  out.coupureDureeS = in.verdict->outageS;  // journal uniquement
  return out;
}

uint32_t computeFiltrationElapsedMs(uint32_t nowMs, uint32_t startedAtMs, uint32_t carryMs) {
  // Soustraction non signée 32 bits : wrap-safe par construction (arithmétique
  // modulaire).
  // ⚠️ CE QUI PORTE RÉELLEMENT LA SÛRETÉ EST LE TYPE DE RETOUR, pas celui des
  // opérandes — vérifié par mutation le 2026-10-12 : élargir la soustraction en
  // int64_t ne casse AUCUN vecteur, parce que la troncature finale vers uint32_t
  // ramène le même résultat modulo 2^32. Mutant ÉQUIVALENT, pas un trou de test.
  // ⛔ Le point fragile est donc le TYPE DE RETOUR 32 bits, et la conversion du
  // site d'appel (`filtration.h`, `(unsigned long)` — 32 bits sur ESP32).
  // ⚠️ ET AUCUN VECTEUR NATIF NE LE PROUVE : `TEST_ASSERT_EQUAL_UINT32` tronque
  // ses DEUX opérandes avant de comparer, donc un retour élargi passerait les
  // quatre vecteurs `test_elapsed_*` sans broncher. Relevé en revue le
  // 2026-10-12 — c'est une protection par LECTURE, pas par test : ne pas se
  // fier à la suite verte pour autoriser un élargissement ici.
  // ⛔ Et ne jamais « protéger » cette soustraction par un `if (nowMs <
  // startedAtMs)` : ce réflexe DÉTRUIT le repliement au lieu de le gérer — le
  // mutant correspondant tue bien 2 vecteurs, il n'est pas théorique.
  const uint32_t raw = nowMs - startedAtMs;
  return raw + carryMs;
}

const char* filtrationResumeMotifLabel(FiltrationResumeMotif m) {
  switch (m) {
    case FiltrationResumeMotif::kNoRecord:       return "démarrage à froid ou mémoire corrompue";
    case FiltrationResumeMotif::kNotRunning:     return "filtration non active avant la coupure";
    case FiltrationResumeMotif::kNotManaged:     return "mode d'installation non géré";
    case FiltrationResumeMotif::kNotInitialRun:  return "démarrage forcé (pas le premier de l'uptime)";
    case FiltrationResumeMotif::kVerdictNotReady:return "verdict de continuité non disponible";
    case FiltrationResumeMotif::kDenied:         return "continuité de l'eau refusée";
    case FiltrationResumeMotif::kImplausible:    return "report invraisemblable";
    case FiltrationResumeMotif::kResumed:
    default:                                     return "reprise";
  }
}
