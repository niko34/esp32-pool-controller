// =============================================================================
// measure_validity_logic — Validité des mesures pH/ORP PURE
// (feature-061, réécrite par la feature-091 v2.34.0 : purge ADAPTATIVE ;
// RETRAIT de l'analyse de stabilité par la feature-139, v2.62.0)
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

  // Règle 1 : eau absente → porte fermée, rien ne court. Datation de l'arrêt
  // (C2, feature-139) : au front descendant on arme waterStopMs ; à chaque
  // tick suivant pendant l'absence, on LATCH stopGraceExceeded dès que la
  // grâce est dépassée — ce latch est monotone (ne redescend jamais tant que
  // l'eau reste absente), ce qui rend le repliement de millis() inoffensif.
  if (!in.waterPresent) {
    if (st.waterPresent) {
      // front DESCENDANT : nouvel arrêt, nouvelle datation.
      st.waterStopMs = in.nowMs;
      st.haveWaterStop = true;
      st.stopGraceExceeded = false;
    }
    // FRONTIÈRE : `>=`, pas `>` — à durée == graceMs PILE, l'arrêt est déjà jugé
    // LONG (sens prudent, fail-closed). DOIT rester aligné MOT POUR MOT avec
    // evaluateOutageContinuity() (section C3 plus bas, measure_validity_logic.h) :
    // sinon un même arrêt de durée exactement graceS donnerait un verdict
    // différent selon qu'un reboot s'est intercalé ou non — exactement ce que
    // l'arbitrage 1 (feature-139) interdit.
    if (st.haveWaterStop && !st.stopGraceExceeded &&
        (uint32_t)(in.nowMs - st.waterStopMs) >= in.graceMs) {
      st.stopGraceExceeded = true;
    }
    if (st.waterPresent || st.valid || st.purging) {
      st.event = MeasureValidityEvent::kWaterDrop;
    }
    st.waterPresent = false;
    st.valid = false;
    st.purging = false;
    st.waivedCycle = false;
    return;
  }

  // Règle 2 : front montant → nouvelle purge (ou dispense immédiate si
  // l'arrêt qui vient de se terminer était datable ET court — C2/C3).
  if (!st.waterPresent) {
    st.waterPresent = true;
    st.waterRiseMs = in.nowMs;
    st.purgeStartMs = in.nowMs;
    st.waivedCycle = false;
    st.purging = true;
    st.valid = false;
    st.event = MeasureValidityEvent::kPurgeStarted;
    // BLOQUANT 2 (code-reviewer, 2e passage) : `stopGraceExceeded` n'est
    // REMESURÉ qu'en règle 1 (eau absente) — entre `begin()` (qui le fige au
    // boot sur un `outageS` daté AVANT le setup Wi-Fi/NTP/FS) et ce premier
    // `update()`, des dizaines de secondes peuvent s'écouler SANS qu'aucun
    // tick « eau absente » ne passe dessus (l'eau est déjà présente au tout
    // premier tick). Un arrêt de 175 s suivi de 30 s de démarrage serait donc
    // dispensé à tort si on ne faisait confiance qu'au latch. On REMESURE donc
    // ICI AUSSI, en plus du latch (jamais à sa place : le latch reste ce qui
    // rend un arrêt de plus de 49,7 j inoffensif face au repliement de
    // millis() — la remesure seule, sans latch, ne le garantirait pas).
    // Frontière `<` stricte, alignée avec evaluateOutageContinuity() (C3).
    const bool skip = st.haveWaterStop && !st.stopGraceExceeded &&
                      (uint32_t)(in.nowMs - st.waterStopMs) < in.graceMs;
    if (skip) {
      st.waivedCycle = true;
      openGate(st, in.nowMs, PurgeEnd::kWaived);  // durée 0, porte ouverte
    }
    return;
  }

  // Règle 3 : purge en cours → ouverture dès que le plancher est écoulé.
  // Plus de condition d'étendue, plus de re-fermeture, plus de plafond
  // (feature-139, v2.62.0).
  if (st.purging) {
    const uint32_t elapsed = in.nowMs - st.purgeStartMs;  // wrap-safe
    if (elapsed >= in.floorMs) {
      openGate(st, in.nowMs, PurgeEnd::kStability);
    }
    return;
  }

  // Porte déjà ouverte : rien à faire tant que l'eau reste présente.
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
    case PurgeEnd::kWaived:    return "dispensée";
    case PurgeEnd::kNone:
    default:                   return "aucune";
  }
}

// =============================================================================
// C3 — continuité à travers un redémarrage
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

  // L'eau devait être présente juste avant la coupure (sinon rien n'est en
  // train de « s'arrêter » : pas d'arrêt à porter à travers le reboot).
  if (r.waterPresent != 1) {
    v.deny = OutageDeny::kNoWater;
    return v;
  }

  // Coupure courte uniquement. FRONTIÈRE (doit rester alignée MOT POUR MOT
  // avec le latch de measureValidityTick(), règle 1, measure_validity_logic.h) :
  // à durée == graceS PILE, ce N'EST PAS un arrêt court (fail-closed, sens
  // prudent) — `>=` et non `>`. Un même arrêt doit donner le MÊME verdict
  // qu'il ait traversé un reboot ou non (arbitrage 1, feature-139) ; les deux
  // comparaisons ont déjà divergé une fois sur cette frontière, ne les
  // redésynchronise pas.
  if (v.outageS >= in.graceS) {
    v.deny = OutageDeny::kTooLong;
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
    default:                       return "inconnue";
  }
}
