// =============================================================================
// pulse_dosing_logic — Dosage en proportionnel de TEMPS, logique PURE (ADR-0017)
// =============================================================================
// Voir pulse_dosing_logic.h pour le modèle complet (option B+C du gate
// pool-chemistry, bug-dosage-fantome-duty-non-calibre itération 2).
// AUCUN Arduino.h ici : compile en natif (pio test -e native).

#include "pulse_dosing_logic.h"
#include "constants.h"  // kPulseFlowMaxMlPerMin (plausibilité) — header pur (stdint/time)

PulseRefusal evaluatePulseGate(uint8_t ampDuty, uint8_t calibratedDuty,
                               float pulseFlowMlPerMin, uint8_t minActiveDuty) {
  // 1. Jamais caractérisée (défaut usine 0) : fail-closed absolu. On ne remplace
  //    pas un modèle faux par une valeur devinée (verdict P4 : refuser plutôt
  //    que compter faux).
  if (calibratedDuty == 0) return PulseRefusal::FlowUncharacterized;
  // 2. Débit mesuré implausible : NaN/inf, nul/négatif, ou au-delà de ce qu'une
  //    doseuse de piscine peut débiter (kPulseFlowMaxMlPerMin).
  if (!isfinite(pulseFlowMlPerMin) || pulseFlowMlPerMin <= 0.0f ||
      pulseFlowMlPerMin > kPulseFlowMaxMlPerMin) {
    return PulseRefusal::FlowUncharacterized;
  }
  // 3. Condition B2 : l'amplitude ne doit JAMAIS être sous le plancher de
  //    rotation (MIN_ACTIVE_DUTY = Dmin mesuré + marge) — refus explicite,
  //    jamais une dégradation silencieuse (piège « régulation morte »).
  if (ampDuty < minActiveDuty) return PulseRefusal::AmplitudeBelowMinActive;
  // 4. Le débit n'est mesuré QU'À l'amplitude calibrée : toute autre amplitude
  //    rend le volume inconnu → non caractérisé.
  if (ampDuty != calibratedDuty) return PulseRefusal::FlowUncharacterized;
  return PulseRefusal::None;
}

PulsePlan planPulse(float meanFlowMlPerMin, float ampFlowMlPerMin,
                    uint32_t periodMs, uint32_t minPulseMs) {
  // Entrées invalides → refus fail-closed (pulseMs = 0, jamais de pouls fantôme).
  if (!isfinite(meanFlowMlPerMin) || meanFlowMlPerMin <= 0.0f ||
      !isfinite(ampFlowMlPerMin) || ampFlowMlPerMin <= 0.0f || periodMs == 0) {
    return {true, 0};
  }
  // Fraction ≥ 100 % : la demande dépasse ce que l'amplitude peut délivrer →
  // pompe continue sur toute la période (le pouls EST la période).
  if (meanFlowMlPerMin >= ampFlowMlPerMin) return {false, periodMs};

  // pulseMs = periodMs × meanFlow / ampFlow, arrondi au ms le plus proche.
  float pulseF = (float)periodMs * (meanFlowMlPerMin / ampFlowMlPerMin);
  uint32_t pulseMs = (uint32_t)(pulseF + 0.5f);

  // Seuil C : sous le pouls minimal (B3 ≥ 5 s), le débit d'un pouls trop court
  // n'est pas reproductible (transitoires de démarrage dominants) → REFUS.
  // C'est un deadband normal : on attend que l'erreur justifie un pouls franc.
  if (pulseMs < minPulseMs) return {true, 0};
  if (pulseMs > periodMs) pulseMs = periodMs;  // borne haute (défensif)
  return {false, pulseMs};
}

PulseCarryPlan planPulseWithCarry(float meanFlowMlPerMin, float ampFlowMlPerMin,
                                  float flow255MlPerMin, uint32_t periodMs,
                                  uint32_t minPulseMs, uint32_t kickMs,
                                  float carryMl) {
  // Régime ≥ plancher : délégation STRICTE à planPulse (comportement INCHANGÉ).
  // F2 : le régime proportionnel reprend ses droits → cumul ABANDONNÉ (une
  // demande servie en direct ne doit laisser aucune dette derrière elle).
  PulsePlan base = planPulse(meanFlowMlPerMin, ampFlowMlPerMin, periodMs, minPulseMs);
  if (!base.refused) return {false, base.pulseMs, false, 0.0f};

  // planPulse a refusé : distinguer les entrées INVALIDES (refus fail-closed,
  // reset de l'accumulateur — F1 : une demande disparue ne laisse pas de dette)
  // du seuil C (demande vivante et calculable, mais sous le pouls minimal).
  if (!isfinite(meanFlowMlPerMin) || meanFlowMlPerMin <= 0.0f ||
      !isfinite(ampFlowMlPerMin) || ampFlowMlPerMin <= 0.0f || periodMs == 0) {
    return {true, 0, false, 0.0f};
  }

  // V_pouls de référence KICK-INCLUS (décision gate n°1) : volume RÉEL d'une
  // impulsion minimale (kick à flow255 + plateau à ampFlow), utilisé pour le
  // SEUIL de tir ET le DÉCRÉMENT — jamais le nominal ampFlow × minPulseMs.
  float vPulseMl = pulseDeltaVolumeMl(0, kickMs, 0, minPulseMs,
                                      flow255MlPerMin, ampFlowMlPerMin);
  if (!isfinite(vPulseMl) || vPulseMl <= 0.0f) {
    return {true, 0, false, 0.0f};  // référence incalculable → fail-closed
  }

  // Reset défensif de l'accumulateur d'entrée : non fini, négatif, ou déjà ≥
  // V_pouls (violation de l'invariant de sortie) → on repart de zéro plutôt
  // que de porter une dette suspecte (jamais de rafale de rattrapage).
  if (!isfinite(carryMl) || carryMl < 0.0f || carryMl >= vPulseMl) carryMl = 0.0f;

  // Accumulation : volume demandé sur la période (mL = mL/min × ms ÷ 60000).
  carryMl += meanFlowMlPerMin * (float)periodMs / 60000.0f;

  // Tir dès que le cumul ATTEINT V_pouls (égalité incluse) : UNE impulsion
  // minimale part cette période (jamais deux — F3), le résidu est conservé.
  if (carryMl >= vPulseMl) {
    carryMl -= vPulseMl;
    // Invariant de sortie carryMl < V_pouls : garanti tant que flow255 ≥
    // ampFlow (l'incrément d'une période sous-plancher est alors < V_pouls) ;
    // si une calibration atypique le casse, écrêtage fail-closed à zéro.
    if (carryMl >= vPulseMl) carryMl = 0.0f;
    return {false, minPulseMs, true, carryMl};
  }
  // Sous le seuil : période d'ACCUMULATION — pas d'impulsion (pulseMs = 0,
  // pompe OFF via tickPulse), la séance reste OUVERTE côté appelant.
  return {false, 0, false, carryMl};
}

PulseTick tickPulse(const PulseState& st, uint32_t nowMs, uint32_t periodMs,
                    uint32_t pulseMs, uint32_t kickMs) {
  PulseState n = st;
  if (periodMs == 0) return {false, false, n};  // garde défensive : tout OFF

  // Frontière de période — arithmétique NON SIGNÉE : (nowMs - periodStartMs)
  // reste correct au wrap de millis() (uint32 modulaire).
  uint32_t elapsed = nowMs - n.periodStartMs;
  if (elapsed >= periodMs) {
    // Nouvelle période, réancrée à nowMs : la dérive absorbée est bornée par un
    // tour de loop (~10-100 ms), négligeable devant periodMs = 60 s.
    n.periodStartMs = nowMs;
    n.pulseStartMs = nowMs;
    n.pulsing = (pulseMs > 0);
  }

  bool pumpOn = false;
  bool kickPhase = false;
  if (n.pulsing) {
    uint32_t pulseElapsed = nowMs - n.pulseStartMs;  // wrap-safe
    if (pulseElapsed >= pulseMs) {
      n.pulsing = false;  // pouls terminé → pompe OFF jusqu'à la prochaine période
    } else {
      pumpOn = true;
      kickPhase = (pulseElapsed < kickMs);  // kick pleine puissance en tête de pouls
    }
  }
  return {pumpOn, kickPhase, n};
}

float pulseDeltaVolumeMl(uint32_t pulseStartMs, uint32_t kickMs,
                         uint32_t t0, uint32_t t1,
                         float flow255MlPerMin, float ampFlowMlPerMin) {
  uint32_t deltaMs = t1 - t0;  // wrap-safe (t1 ≥ t0 modulo 2^32)
  if (deltaMs == 0) return 0.0f;
  // Défensif : des débits invalides ne créditent rien plutôt que du NaN dans
  // les compteurs de sécurité (la garde evaluatePulseGate a déjà filtré en amont).
  if (!isfinite(flow255MlPerMin) || flow255MlPerMin < 0.0f) flow255MlPerMin = 0.0f;
  if (!isfinite(ampFlowMlPerMin) || ampFlowMlPerMin < 0.0f) ampFlowMlPerMin = 0.0f;

  // Positions RELATIVES au début du pouls (non signées → wrap-safe). Si t0
  // précède pulseStartMs (appel hors contrat), r0 devient très grand : tout le
  // delta est crédité au débit d'amplitude — conservateur, jamais zéro (P4).
  uint32_t r0 = t0 - pulseStartMs;
  uint32_t r1 = r0 + deltaMs;

  // Recouvrement de [r0, r1) avec la fenêtre de kick [0, kickMs).
  uint32_t kickPartMs = 0;
  if (r0 < kickMs) {
    uint32_t end = (r1 < kickMs) ? r1 : kickMs;
    kickPartMs = end - r0;
  }
  uint32_t plateauPartMs = deltaMs - kickPartMs;

  // mL = (mL/min ÷ 60000 ms/min) × ms — indépendant des instants de tick.
  return (flow255MlPerMin * (float)kickPartMs +
          ampFlowMlPerMin * (float)plateauPartMs) / 60000.0f;
}
