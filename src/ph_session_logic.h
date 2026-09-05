#ifndef PH_SESSION_LOGIC_H
#define PH_SESSION_LOGIC_H

// =============================================================================
// ph_session_logic — PORTE DE SESSION pH, logique PURE (feature-094, v2.36.0)
// =============================================================================
// Patron ADR-0017 : pas de millis() interne — le temps est un PARAMÈTRE ; pas
// d'allocation ; pas d'Arduino.h ; compilable en natif.
//
// POURQUOI CE MODULE EXISTE
// -------------------------
// Près de la consigne, ce n'est plus l'état du bassin qui ouvre et ferme les
// sessions de dosage, c'est le BRUIT DE MESURE. Mesuré le 03-04/09/2026 sur la
// filtrée (celle que lit la régulation) : σ = 0,043 sur 60 s, crête-à-crête
// 15 min ≈ 0,150 — pour une bande d'hystérésis de 0,05 (démarrage) / 0,01
// (arrêt). Le bruit est donc PLUS LARGE que la bande de décision.
//
// Conséquence mesurée : des sessions de 6-10 min hachées par des pauses de
// mélange de 15 min, un rapport cyclique effectif tombé à ~1/3 près de la
// consigne, et un feedforward (dose de base apprise, feature-085) retenu puis
// réarmé toutes les 15 min — donc servi un tiers du temps alors qu'il couvre une
// charge CONTINUE.
//
// Caractérisation décisive du gate pool-chemistry : τc ≈ 50 s, période dominante
// ≈ 5 min. C'est un processus quasi BLANC, pas une oscillation lente : moyenner
// marche. Une fenêtre de 10 min ramène σ à ≈ 0,017 pour ≈ 7 min de retard.
//
// LES DEUX LEVIERS RETENUS (le gate en a refusé un troisième)
// ----------------------------------------------------------
//   L1 — SIGNAL LENT (phDecisionTick) : la DÉCISION d'ouvrir/fermer lit une
//        moyenne glissante de 10 min de la filtrée. La régulation continue
//        (P, I, feedforward) continue de lire la filtrée INSTANTANÉE : on ne
//        ralentit pas la boucle, on stabilise sa porte.
//   L3 — DURÉE MINIMALE DE SESSION (kPhSessionMinWallMs, 30 min MURALES) : une
//        session ouverte ne se referme pas sur un creux de bruit. En temps
//        MURAL et non pompe-ON : au rapport cyclique mesuré (3,4-5,3 %), 30 min
//        de pompe-ON dureraient ~10 h murales.
//   L2 — ÉLARGIR LA BANDE : REFUSÉ par le gate. Couvrir le bruit brut exigerait
//        0,17 pH ; avec de l'acide SEUL, le bassin ne saurait pas remonter.
//
// UN SEUL POINT DE VÉRITÉ (le cœur du lot)
// ----------------------------------------
// phSessionGate() produit la PORTE **et** la BANDE MORTE depuis le MÊME signal.
// La régression v2.26.0 (« la session ne se referme jamais ») était née de la
// divergence entre ces deux décisions, écrites à deux endroits différents de
// pump_controller.cpp : le PI se parquait exactement au point de coupure de
// l'hystérésis. Les rendre indissociables rend ce défaut structurellement
// impossible à réintroduire.
//
// CE QUE CE MODULE NE FAIT PAS — GARDES PRIORITAIRES (condition C1 du gate)
// -------------------------------------------------------------------------
// Plancher acide, butée brute, capteur instable, filtration absente, limite
// horaire, quota journalier, maxCyclesPerDay, anti-rafale, OTA, maintenance,
// watchdog : ces gardes ferment la session dans le MÊME tour, arment la pause
// de mélange et journalisent, QUELLE QUE SOIT la durée minimale. Elles vivent
// en amont (canDose / evaluateDose) ou hors branche (closePhDosingSession) et
// ne passent JAMAIS par ici. `minSessionMs` n'est consommé que dans la branche
// hystérésis ci-dessous, jamais face à une fermeture de garde.
//
// RÉPARTITION (même règle que ph_integral_logic / ph_feedforward_logic)
//   - ce fichier : la décision (signal lent, porte, bande morte, régime de débit) ;
//   - pump_controller.cpp : le câblage — fournir la mesure, le temps et l'état de
//     session, appliquer le verdict, plafonner le débit, journaliser.
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"  // kPhDoorCloseMarginPh, kPhSessionUndershootFloorPh

// -----------------------------------------------------------------------------
// L1 — SIGNAL LENT DE DÉCISION
// -----------------------------------------------------------------------------
// Moyenne glissante du PREMIER ORDRE (constante de temps `tauMs`) de la mesure
// FILTRÉE. Le pas de temps est fourni par l'appelant, jamais supposé : la
// cadence d'appel (tour de loop) et la cadence de rafraîchissement de la sonde
// (5 s) diffèrent, et un filtre à α fixe donnerait une fenêtre effective
// dépendante de la charge CPU.
//
// État POD, loopTask uniquement (écrivain unique, aucun mutex) — même contrat
// que PhFfState / PhIntegralState.
struct PhDecisionFilter {
  float    value    = NAN;    // moyenne glissante (pH) ; NAN = non amorcée
  uint32_t lastMs   = 0;      // instant du dernier tick (base de dt, wrap-safe)
  // Temps CUMULÉ depuis le (ré-)amorçage, saturé à `tauMs`. Sans lui, `primed`
  // ne pourrait pas distinguer « fenêtre pleine » de « premier échantillon » :
  // `lastMs` sert de base à dt et ne peut pas porter aussi l'instant d'armement.
  uint32_t warmupMs = 0;
  bool     primed   = false;  // fenêtre PLEINE d'échantillons post-reprise
};

/** @brief Ré-amorce le signal lent : plus AUCUNE décision jusqu'à ce que la
 *         fenêtre soit de nouveau pleine (`primed`).
 *
 * Appelé à chaque reprise de filtration (la filtrée redevient NAN le temps que
 * la médiane se reconstruise, cf. SensorFilter::rearm) et sur tout trou
 * d'évaluation. Le transitoire de reprise du 04/09/2026 a duré 40 min : décider
 * sur une moyenne qui contient des valeurs de stagnation reviendrait à doser sur
 * une mesure non représentative (condition C2 du gate).
 */
inline void phDecisionRearm(PhDecisionFilter& st) {
  st.value    = NAN;
  st.lastMs   = 0;
  st.warmupMs = 0;
  st.primed   = false;
}

/**
 * @brief UN tick du signal lent de décision.
 *
 * @param st        État persistant, modifié en place
 * @param filtered  Mesure FILTRÉE courante (pH) ; non finie ⇒ ré-amorçage
 * @param nowMs     millis() fourni par la coquille
 * @param tauMs     Constante de temps de la fenêtre (kPhDecisionWindowMs)
 *
 * Enchaînement :
 *   1. mesure non finie ⇒ ré-amorçage (reprise de filtration, sonde muette) ;
 *   2. filtre non amorcé ⇒ on ARME sur l'échantillon courant, sans décider ;
 *   3. dt aberrant (> kPhIntegralMaxTickDtMs) ⇒ ré-amorçage : un trou
 *      d'évaluation donnerait α ≈ 1, c'est-à-dire une « moyenne » réduite au
 *      dernier échantillon, avec `primed` encore vrai — exactement la décision
 *      sur bruit que ce module supprime ;
 *   4. sinon : moyenne glissante α = dt/τ, et maturation de la fenêtre.
 */
inline void phDecisionTick(PhDecisionFilter& st, float filtered, uint32_t nowMs,
                           uint32_t tauMs) {
  if (!isfinite(filtered) || tauMs == 0) {
    phDecisionRearm(st);
    return;
  }
  if (!isfinite(st.value)) {
    st.value    = filtered;
    st.lastMs   = nowMs;
    st.warmupMs = 0;
    st.primed   = false;
    return;
  }

  const uint32_t dt = (uint32_t)(nowMs - st.lastMs);  // non signé ⇒ wrap-safe
  if (dt == 0) return;                                // deux tours dans la même ms
  if (dt > kPhIntegralMaxTickDtMs) {
    phDecisionRearm(st);
    return;
  }
  st.lastMs = nowMs;

  float alpha = (float)dt / (float)tauMs;
  if (alpha > 1.0f) alpha = 1.0f;
  st.value += alpha * (filtered - st.value);

  // Maturation de la fenêtre — saturation d'addition, jamais de repliement.
  if (!st.primed) {
    const uint32_t room = tauMs - st.warmupMs;
    st.warmupMs += (dt < room) ? dt : room;
    st.primed = (st.warmupMs >= tauMs);
  }
}

// -----------------------------------------------------------------------------
// PORTE DE SESSION — entrées / sortie
// -----------------------------------------------------------------------------
// INITIALISEURS DE MEMBRE OBLIGATOIRES (même doctrine que DoseInputs) : un site
// de collecte — ou une fixture de test — qui oublierait un champ retombe sur un
// défaut FAIL-CLOSED (`decisionReady = false` ⇒ porte fermée, `minSessionMs = 0`
// ⇒ aucune prolongation), jamais sur une valeur indéterminée.
struct PhGateIn {
  bool     wasOpen       = false;  // session pH ouverte à l'entrée du tour
  float    liveError     = NAN;    // erreur signée sur la filtrée INSTANTANÉE (P/I/FF)
  float    decisionError = NAN;    // erreur signée sur le SIGNAL LENT (décision seule)
  bool     decisionReady = false;  // PhDecisionFilter::primed
  float    startThr      = kPhStartThresholdDefault;  // pumpProtection.phStartThreshold
  float    stopThr       = kPhStopThreshold;          // pumpProtection.phStopThreshold
  uint32_t sessionOnMs   = 0;      // temps POMPE-ON cumulé de la session
  uint32_t sessionWallMs = 0;      // durée MURALE de la session (0 = base inconnue)
  uint32_t minSessionMs  = 0;      // kPhSessionMinWallMs (30 min murales)
};

// Motif du verdict — encode AUSSI le régime de débit exigé par la condition C7
// du gate (voir phSessionGate). Contrat INTERNE (ni /data, ni WS, ni MQTT dans
// ce lot) : ces valeurs ne sont pas sérialisées, mais l'énumération sert de
// point de vérité unique entre le module et sa coquille.
constexpr uint8_t kPhGateClosedIdle       = 0;  // fermée : erreur sous le seuil d'ouverture
constexpr uint8_t kPhGateOpened           = 1;  // OUVERTURE ce tour (lent > startThr)
constexpr uint8_t kPhGateHoldFull         = 2;  // ouverte, débit PLEIN (lent > stopThr)
constexpr uint8_t kPhGateHoldCapped       = 3;  // ouverte, débit PLAFONNÉ au feedforward (C7)
constexpr uint8_t kPhGateHoldZero         = 4;  // ouverte, débit NUL, porte maintenue (C7)
constexpr uint8_t kPhGateClosedResolved   = 5;  // fermeture : erreur résorbée, durée minimale écoulée
constexpr uint8_t kPhGateClosedUndershoot = 6;  // fermeture IMMÉDIATE : sous-dépassement (C6)
constexpr uint8_t kPhGateClosedNotReady   = 7;  // signal lent non prêt (fenêtre en cours de remplissage)

struct PhGateOut {
  bool    open     = false;  // la session doit-elle être ouverte / maintenue ?
  float   deadband = kPhStartThresholdDefault;  // bande morte de computePidPure
  uint8_t reason   = kPhGateClosedIdle;         // motif + régime de débit (kPhGate*)
};

/**
 * @brief Porte de session pH ET bande morte du PI, décidées ENSEMBLE.
 *
 * Ordre des décisions (aucune ne peut être réordonnée sans nouveau passage
 * pool-chemistry) :
 *
 *   1. SIGNAL LENT NON PRÊT (C2) — fenêtre en cours de remplissage après une
 *      reprise de filtration : aucune ouverture, et fermeture d'une éventuelle
 *      session en cours (fail-closed : sans signal digne de confiance, on ne
 *      dose pas). La durée minimale ne s'y oppose pas.
 *
 *   2. PLANCHER DE SOUS-DÉPASSEMENT (C6) — `decisionError < −0,05` sur le signal
 *      LENT : le bassin est franchement passé de l'autre côté de la consigne, ce
 *      n'est plus du bruit. Fermeture IMMÉDIATE, qui PRIME sur la durée minimale.
 *      Le seuil est le MÊME que celui du latch défavorable du feedforward
 *      (kPhFfAdverseErrorPh, static_assert d'identité) : deux organes qui
 *      constatent le même événement physique ne doivent pas le constater à deux
 *      valeurs différentes.
 *
 *   3. HYSTÉRÉSIS sur le signal LENT :
 *        - fermée  → ouverture ssi `lent > startThr` (INCHANGÉ, C3) ;
 *        - ouverte → maintien tant que `lent > stopThr − kPhDoorCloseMarginPh`
 *          (C4 : la porte ferme à −0,02, la marge n'affecte PAS la bande
 *          d'actionnement) ; sinon PROLONGATION tant que la durée minimale
 *          murale n'est pas écoulée (C5, L3).
 *
 *   4. RÉGIME DE DÉBIT (C7) — sans lui, `flow += phFfFlow` servirait le
 *      feedforward AVEUGLÉMENT pendant toute la prolongation, seul vrai danger
 *      de L3 :
 *        - `lent ≤ stopThr` → débit total PLAFONNÉ au feedforward seul (le PI est
 *          déjà annulé par la bande morte ; le plafond interdit que PI+FF le
 *          dépasse) ;
 *        - `lent ≤ 0`       → débit NUL, porte maintenue.
 *
 * BANDE MORTE (C3) : `wasOpen ? stopThr : startThr` — position et rôle
 * strictement inchangés (déplacement du point d'équilibre = 0,000 pH). Elle
 * s'applique à l'erreur INSTANTANÉE dans computePidPure ; `liveError` n'est ici
 * qu'à titre documentaire (le module ne décide RIEN dessus : c'est précisément
 * ce qu'on lui retire).
 */
inline PhGateOut phSessionGate(const PhGateIn& in) {
  PhGateOut out;
  // C3 — MÊME choix de bande morte qu'avant feature-094, rendu indissociable du
  // verdict de porte : un seul appel, un seul point de vérité.
  out.deadband = in.wasOpen ? in.stopThr : in.startThr;
  out.open     = false;
  out.reason   = kPhGateClosedIdle;

  // --- 1. Signal lent non prêt (C2) -----------------------------------------
  if (!in.decisionReady || !isfinite(in.decisionError)) {
    out.reason = kPhGateClosedNotReady;
    return out;
  }

  // --- 2. Plancher de sous-dépassement (C6), prime sur la durée minimale -----
  if (in.wasOpen && in.decisionError < -kPhSessionUndershootFloorPh) {
    out.reason = kPhGateClosedUndershoot;
    return out;
  }

  // --- 3. Hystérésis sur le signal lent --------------------------------------
  if (!in.wasOpen) {
    // Ouverture : le signal lent est au-dessus du seuil de DÉMARRAGE, donc très
    // au-dessus du seuil d'arrêt — régime de débit PLEIN par construction, le
    // point 4 n'a rien à trancher.
    out.open   = (in.decisionError > in.startThr);
    out.reason = out.open ? kPhGateOpened : kPhGateClosedIdle;
    return out;
  }

  const bool aboveClose = (in.decisionError > (in.stopThr - kPhDoorCloseMarginPh));
  if (!aboveClose) {
    // PROLONGATION (C5/L3) : la durée minimale est MURALE. Le second terme est
    // une défense en profondeur — une session qui a déjà tourné `minSessionMs`
    // de POMPE-ON a, a fortiori, dépassé la durée minimale murale ; il ferme la
    // prolongation même si la base de temps murale avait été perdue.
    out.open = (in.sessionWallMs < in.minSessionMs) &&
               (in.sessionOnMs < in.minSessionMs);
    if (!out.open) {
      out.reason = kPhGateClosedResolved;
      return out;
    }
  } else {
    out.open = true;
  }

  // --- 4. Régime de débit (C7) ------------------------------------------------
  if (in.decisionError <= 0.0f) {
    out.reason = kPhGateHoldZero;
  } else if (in.decisionError <= in.stopThr) {
    out.reason = kPhGateHoldCapped;
  } else {
    out.reason = kPhGateHoldFull;
  }
  return out;
}

#endif  // PH_SESSION_LOGIC_H
