#ifndef PULSE_DOSING_LOGIC_H
#define PULSE_DOSING_LOGIC_H

// =============================================================================
// pulse_dosing_logic — Dosage en proportionnel de TEMPS, logique PURE (ADR-0017)
// =============================================================================
// bug-dosage-fantome-duty-non-calibre, itération 2 (défaut n°1) — option (B)+(C)
// du gate pool-chemistry.
//
// LE MODÈLE LINÉAIRE duty↔débit EST MORT (mesure au banc du 2026-08-23) : à
// duty 82 le modèle créditait 6,4 ml/min quand la pompe débite 12,0 ; à 255 il
// supposait 90 ml/min pour 61,0 mesurés. Une péristaltique n'a PAS un débit
// proportionnel au rapport cyclique en bas de plage : sous un seuil elle cale,
// autour de ce seuil son débit est erratique.
//
// NOUVEAU MODÈLE — proportionnel de temps à amplitude CALIBRÉE :
//   * la pompe ne tourne QU'À une amplitude unique, celle où son débit a été
//     MESURÉ au banc (pumpNCalibratedDuty / pumpNPulseFlowMlPerMin) ;
//   * la consigne de débit MOYEN est réalisée par la fraction du temps de
//     marche : un pouls de pulseMs par période de periodMs, avec
//     pulseMs = periodMs × meanFlow / ampFlow ;
//   * chaque pouls démarre par un « kick » à pleine puissance (duty 255,
//     kickMs) pour vaincre le couple de décollage (symptôme terrain : rotation
//     entretenue OK à 82, mais NON-démarrage à froid au même duty) ;
//   * sous un pouls minimal (minPulseMs, condition B3 ≥ 5 s) la consigne est
//     REFUSÉE (seuil C : deadband de refus, on attend que l'erreur grossisse)
//     plutôt que réalisée par un pouls trop court au débit non reproductible ;
//   * une pompe jamais caractérisée (calibratedDuty == 0) ou une amplitude
//     différente de celle calibrée REFUSE tout dosage (fail-closed) : on ne
//     crédite jamais un volume qu'on ne sait pas estimer (P4).
//
// CONTRAINTE ADR-0017 : pas d'Arduino.h, pas de FreeRTOS, en-têtes C uniquement
// (<stdint.h>/<math.h>) — le module compile en natif (pio test -e native).
// Toute l'arithmétique temporelle est en uint32_t NON SIGNÉ (wrap millis sûr).
// =============================================================================

#include <stdint.h>
#include <math.h>

// Cause de refus de l'ordonnanceur d'impulsions. None = dosage possible.
enum class PulseRefusal : uint8_t {
  None,
  BelowMinPulse,           // seuil C : pouls calculé < minPulseMs (deadband NORMAL, log info)
  AmplitudeBelowMinActive, // B2 : amplitude sous le plancher de rotation (critical)
  FlowUncharacterized      // pompe jamais caractérisée, débit invalide, ou amplitude ≠ calibrée (critical)
};

// Garde de caractérisation (évaluée AVANT tout calcul de pouls) :
//   1. calibratedDuty == 0            → FlowUncharacterized (défaut usine, fail-closed)
//   2. pulseFlowMlPerMin invalide     → FlowUncharacterized (NaN/inf, ≤ 0, ou
//      > kPulseFlowMaxMlPerMin — plausibilité d'une doseuse de piscine)
//   3. ampDuty < minActiveDuty        → AmplitudeBelowMinActive (B2 : jamais
//      dégrader en silence sous le seuil de rotation — refus + critical)
//   4. ampDuty != calibratedDuty      → FlowUncharacterized (le débit n'est
//      mesuré QU'À l'amplitude calibrée ; toute autre amplitude = volume inconnu)
// NB : BelowMinPulse n'est JAMAIS retourné ici (c'est le verdict de planPulse).
PulseRefusal evaluatePulseGate(uint8_t ampDuty, uint8_t calibratedDuty,
                               float pulseFlowMlPerMin, uint8_t minActiveDuty);

// Plan d'une période : durée du pouls réalisant le débit moyen demandé.
//   pulseMs = periodMs × meanFlow / ampFlow, arrondi, clampé [minPulseMs..periodMs].
//   refused (seuil C) si le pouls calculé est < minPulseMs (pulseMs alors 0,
//   fail-closed) ou si une entrée est invalide (meanFlow/ampFlow ≤ 0 ou non finis).
//   meanFlow ≥ ampFlow (fraction 100 %) → pouls = période entière (pompe continue).
struct PulsePlan {
  bool refused;      // true = ne pas injecter cette période (seuil C / entrée invalide)
  uint32_t pulseMs;  // durée du pouls (0 si refused)
};
PulsePlan planPulse(float meanFlowMlPerMin, float ampFlowMlPerMin,
                    uint32_t periodMs, uint32_t minPulseMs);

// =============================================================================
// feature-073 — Accumulateur de dosage sous le pouls minimal (amende le seuil C
// de l'ADR-0043 : la demande sous le plancher n'est plus REFUSÉE, elle est
// CUMULÉE de période en période jusqu'à valoir une impulsion minimale réelle).
// =============================================================================
// Plan d'une période AVEC accumulateur (entrée/sortie : carryMl, possédé par la
// coquille, un par produit, non persistant).
//
// Sémantique par régime :
//   * demande ≥ plancher (planPulse ne refuse pas) : délégation STRICTE à
//     planPulse — comportement INCHANGÉ — et carryMl remis à 0 (F2 : le régime
//     proportionnel reprend ses droits, le cumul est abandonné) ;
//   * demande vivante mais SOUS le plancher (ex-seuil C) : le volume demandé
//     sur la période (meanFlow × periodMs / 60000) est ajouté à carryMl ; dès
//     que le cumul ATTEINT (égalité incluse) le volume RÉEL d'une impulsion
//     minimale V_pouls, UNE impulsion de minPulseMs part (pulseMs = minPulseMs,
//     carryFired = true), le résidu est conservé — jamais deux pouls par
//     période (F3), invariant de sortie : carryMl < V_pouls ;
//   * V_pouls est KICK-INCLUS (décision gate n°1) : pulseDeltaVolumeMl(0,
//     kickMs, 0, minPulseMs, flow255, ampFlow) — le volume qu'un pouls minimal
//     délivre VRAIMENT, jamais le nominal ampFlow × minPulseMs ;
//   * entrées invalides (demande nulle/non finie, ampFlow invalide, période 0)
//     ou carryMl d'entrée suspect (non fini, négatif, ≥ V_pouls) → refus /
//     reset défensif fail-closed : l'accumulateur ne porte jamais de dette
//     douteuse (aucune rafale de rattrapage possible).
//
// refused = false avec pulseMs = 0 signifie « période d'ACCUMULATION » : la
// séance reste ouverte côté appelant, la pompe reste OFF via tickPulse.
// PURE (ADR-0017) : stdint/math uniquement, testable en natif.
struct PulseCarryPlan {
  bool refused;       // true = entrées invalides, ne pas injecter (fail-closed)
  uint32_t pulseMs;   // durée du pouls (0 = accumulation en cours, pompe OFF)
  bool carryFired;    // true = pouls déclenché PAR l'accumulateur (minPulseMs)
  float carryMl;      // accumulateur en sortie (à recopier par la coquille)
};
PulseCarryPlan planPulseWithCarry(float meanFlowMlPerMin, float ampFlowMlPerMin,
                                  float flow255MlPerMin, uint32_t periodMs,
                                  uint32_t minPulseMs, uint32_t kickMs,
                                  float carryMl);

// État persistant du train d'impulsions (possédé par la coquille, un par pompe).
// Amorçage d'une session : { nowMs, nowMs, true } (le pouls démarre immédiatement).
struct PulseState {
  uint32_t periodStartMs;  // ancre de la période courante (millis)
  uint32_t pulseStartMs;   // début du pouls courant (référence du kick ET de la
                           // comptabilité pulseDeltaVolumeMl)
  bool pulsing;            // pouls en cours (pompe commandée ON)
};

// Verdict d'un tick d'ordonnanceur.
struct PulseTick {
  bool pumpOn;      // commander la pompe ON ce tour
  bool kickPhase;   // ON à pleine puissance (duty 255) — début de pouls
  PulseState next;  // état à recopier par la coquille
};

// Un pas d'ordonnanceur, wrap-safe (arithmétique non signée sur millis) :
//   * (nowMs - periodStartMs) >= periodMs → nouvelle période, réancrée à nowMs
//     (la dérive absorbée est ≤ un tour de loop) et nouveau pouls (si pulseMs > 0) ;
//   * pouls en cours : pumpOn tant que (nowMs - pulseStartMs) < pulseMs,
//     kickPhase tant que (nowMs - pulseStartMs) < kickMs.
// periodMs == 0 → tout OFF (garde défensive).
PulseTick tickPulse(const PulseState& st, uint32_t nowMs, uint32_t periodMs,
                    uint32_t pulseMs, uint32_t kickMs);

// Volume EXACT (mL) délivré sur l'intervalle [t0, t1] d'un pouls démarré à
// pulseStartMs : la part de l'intervalle recouvrant le kick [pulseStart,
// pulseStart+kickMs) est créditée au débit pleine puissance (flow255MlPerMin,
// MESURÉ à 255), le reste au débit d'amplitude (ampFlowMlPerMin, mesuré à
// l'amplitude calibrée). Indépendant du jitter de loop : seule la géométrie du
// pouls compte, pas les instants d'échantillonnage. Wrap-safe (positions
// relatives non signées). Pré-condition appelant : la pompe était ON sur tout
// [t0, t1] et t0 ≥ pulseStartMs (sinon la part hors kick est créditée plateau —
// conservateur, jamais zéro).
float pulseDeltaVolumeMl(uint32_t pulseStartMs, uint32_t kickMs,
                         uint32_t t0, uint32_t t1,
                         float flow255MlPerMin, float ampFlowMlPerMin);

#endif // PULSE_DOSING_LOGIC_H
