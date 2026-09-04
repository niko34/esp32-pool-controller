// =============================================================================
// measure_validity_logic — Validité des mesures pH/ORP PURE
// (feature-061, réécrite par la feature-091 v2.34.0 : purge ADAPTATIVE)
// =============================================================================
// Voir measure_validity_logic.h pour les règles et invariants. Compile en
// natif (pio test -e native) ET sur cible — aucune dépendance Arduino.
// =============================================================================

#include "measure_validity_logic.h"

#include <stddef.h>  // offsetof

namespace {

// Arrondi seconde INFÉRIEURE (ms → s) pour les durées écoulées.
uint32_t floorSeconds(uint32_t ms) { return ms / 1000UL; }

// Clôture de la purge courante : latch durée + motif, ouverture de la porte.
void openGate(MeasureValidityState& st, uint32_t nowMs, PurgeEnd why) {
  st.lastPurgeDurationS = floorSeconds(nowMs - st.purgeStartMs);
  st.lastPurgeEnd = why;
  st.purging = false;
  st.valid = true;
  st.event = MeasureValidityEvent::kOpened;
}

}  // namespace

void measureValidityTick(MeasureValidityState& st, const MeasureValidityInputs& in) {
  st.event = MeasureValidityEvent::kNone;

  // Règle 1 : eau absente → porte fermée, rien ne court.
  if (!in.waterPresent) {
    if (st.waterPresent || st.valid || st.purging) {
      st.event = MeasureValidityEvent::kWaterDrop;
    }
    st.waterPresent = false;
    st.valid = false;
    st.purging = false;
    st.recloseArmed = false;
    st.waivedCycle = false;
    st.repurgeCount = 0;
    return;
  }

  // Règle 2 : front montant → nouvelle purge (ou dispense immédiate, C3).
  if (!st.waterPresent) {
    st.waterPresent = true;
    st.waterRiseMs = in.nowMs;
    st.purgeStartMs = in.nowMs;
    st.repurgeCount = 0;
    st.recloseArmed = true;
    st.waivedCycle = false;
    st.purging = true;
    st.valid = false;
    st.event = MeasureValidityEvent::kPurgeStarted;
    if (in.purgeWaived) {
      st.waivedCycle = true;
      openGate(st, in.nowMs, PurgeEnd::kWaived);  // durée 0, porte ouverte
    }
    return;
  }

  // Règle 3 : purge en cours → plancher + stabilité, ou plafond.
  if (st.purging) {
    const uint32_t elapsed = in.nowMs - st.purgeStartMs;  // wrap-safe
    if (elapsed >= in.floorMs && in.stable) {
      openGate(st, in.nowMs,
               st.repurgeCount > 0 ? PurgeEnd::kRepurge : PurgeEnd::kStability);
      return;
    }
    if (elapsed >= in.capMs) {
      // C2 : ouverture fail-safe ; la re-fermeture est désarmée jusqu'au
      // prochain front montant (sinon boucle fermer/ouvrir sur une sonde bruyante).
      st.recloseArmed = false;
      openGate(st, in.nowMs, PurgeEnd::kCap);
      return;
    }
    return;  // purge continue
  }

  // Règle 4 : porte ouverte → surveillance de re-fermeture (C1).
  if (st.valid && st.recloseArmed && in.unstable) {
    st.valid = false;
    st.purging = true;
    st.purgeStartMs = in.nowMs;
    if (st.repurgeCount < 255) st.repurgeCount++;
    st.event = MeasureValidityEvent::kReclosed;
  }
}

uint32_t measureValidityElapsedS(const MeasureValidityState& st, uint32_t nowMs) {
  if (!st.purging) return 0;
  return floorSeconds(nowMs - st.purgeStartMs);
}

bool measureValidityCirculationSettled(const MeasureValidityState& st, uint32_t nowMs,
                                       uint32_t floorMs) {
  if (!st.waterPresent) return false;
  if (st.waivedCycle) return true;
  return (nowMs - st.waterRiseMs) >= floorMs;
}

const char* purgeEndLabel(PurgeEnd e) {
  switch (e) {
    case PurgeEnd::kStability: return "stabilité";
    case PurgeEnd::kCap:       return "plafond";
    case PurgeEnd::kWaived:    return "dispensée";
    case PurgeEnd::kRepurge:   return "re-purge";
    case PurgeEnd::kNone:
    default:                   return "aucune";
  }
}

// =============================================================================
// C3 — continuité à travers une coupure courte
// =============================================================================

uint32_t outageFnv1a(const void* data, uint32_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t h = 2166136261UL;
  for (uint32_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

uint32_t outageRecordCrc(const OutageRtcRecord& r) {
  return outageFnv1a(&r, (uint32_t)offsetof(OutageRtcRecord, crc));
}

void outageRecordSeal(OutageRtcRecord& r) {
  r.magic = kOutageRtcMagic;
  r.reserved[0] = 0;
  r.reserved[1] = 0;
  r.reserved[2] = 0;
  r.crc = outageRecordCrc(r);
}

bool outageRecordIntact(const OutageRtcRecord& r) {
  return r.magic == kOutageRtcMagic && r.crc == outageRecordCrc(r);
}

OutageContinuityVerdict evaluateOutageContinuity(const OutageContinuityInputs& in) {
  OutageContinuityVerdict v;
  v.waive = false;
  v.outageS = 0;
  v.deny = OutageDeny::kNone;

  // Mémoire RTC absente ou corrompue (power-on : RAM RTC aléatoire).
  if (in.record == nullptr || !outageRecordIntact(*in.record)) {
    v.deny = OutageDeny::kMemory;
    return v;
  }
  const OutageRtcRecord& r = *in.record;

  // Motif de reset : seul un redémarrage LOGICIEL prouve que l'alimentation n'a
  // pas été coupée (et donc que la filtration externe n'a pas été interrompue).
  if (in.reset == OutageResetKind::kOther) {
    v.deny = OutageDeny::kResetKind;
    return v;
  }

  // Heure : sans heure valide, la durée de coupure est incalculable ; une heure
  // courante antérieure à l'heure vive mémorisée = horloge incohérente.
  if (!in.timeValid || in.epochNow < r.liveEpoch) {
    v.deny = OutageDeny::kClock;
    return v;
  }
  v.outageS = in.epochNow - r.liveEpoch;

  // L'eau devait être présente juste avant la coupure.
  if (r.waterPresent != 1) {
    v.deny = OutageDeny::kNoWater;
    return v;
  }

  // Coupure courte uniquement.
  if (v.outageS > in.shortOutageS) {
    v.deny = OutageDeny::kTooLong;
    return v;
  }

  // Au plus une dispense par intervalle (une boucle de reboots ne doit pas
  // enchaîner les dispenses : la 2e purge est complète).
  if (r.lastWaiverEpoch != 0 &&
      (in.epochNow < r.lastWaiverEpoch ||
       in.epochNow - r.lastWaiverEpoch < in.waiverMinIntervalS)) {
    v.deny = OutageDeny::kRateLimited;
    return v;
  }

  v.waive = true;
  return v;
}

const char* outageDenyLabel(OutageDeny d) {
  switch (d) {
    case OutageDeny::kNone:        return "accordée";
    case OutageDeny::kMemory:      return "mémoire RTC absente ou corrompue";
    case OutageDeny::kNoWater:     return "eau absente avant la coupure";
    case OutageDeny::kResetKind:   return "reset matériel (coupure secteur possible)";
    case OutageDeny::kClock:       return "heure invalide ou incohérente";
    case OutageDeny::kTooLong:     return "coupure trop longue";
    case OutageDeny::kRateLimited: return "dispense déjà accordée il y a moins d'une heure";
    default:                       return "inconnue";
  }
}
