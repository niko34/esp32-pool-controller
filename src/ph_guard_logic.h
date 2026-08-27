#ifndef PH_GUARD_LOGIC_H
#define PH_GUARD_LOGIC_H

// =============================================================================
// ph_guard_logic — Gardes de sécurité pH, logique PURE (v2.25.0)
// =============================================================================
// Décisions des gardes de dosage pH extraites sous une forme testable en natif.
//
// Trois idées gouvernent ce fichier :
//
//   1. DEUX RÉFÉRENTIELS, JAMAIS MÉLANGÉS. Les seuils CHIMIQUES portent sur le pH
//      CORRIGÉ (brut + offset), parce que l'offset est une correction de mesure :
//      une fois appliqué, la valeur EST le pH de l'eau. Seules les deux BUTÉES de
//      dernier recours portent sur le pH BRUT — elles doivent rester valides même
//      si l'offset est faux (cf. src/ph_offset.h, ADR-0039). Les fonctions
//      ci-dessous sont donc AGNOSTIQUES du référentiel : c'est l'appelant qui
//      choisit la grandeur ET le seuil, cohérents entre eux.
//
//   2. DEUX DIRECTIONS. Un produit acide (`ph_minus`) ne peut violer qu'un seuil
//      BAS, un produit basique (`ph_plus`) qu'un seuil HAUT. D'où deux fonctions
//      symétriques plutôt qu'une seule paramétrée par un signe : au point d'appel,
//      le sens de la comparaison doit se lire, pas se déduire.
//
//   3. Un |offset| large n'est PAS un aveu de sonde suspecte : il peut corriger
//      un décalage de référence EXTERNE (courants de fuite de la pompe de
//      filtration). C'est le diagnostic EZO (pentes + zéro) qui distingue une
//      sonde saine d'une sonde dérivée — d'où isPhProbeHealthy().
//
// POLITIQUE NaN (uniforme, et redite fonction par fonction) : une entrée NaN ne
// produit JAMAIS de refus. Les gardes NaN existantes (filtre non prêt, canDose)
// traitent déjà le cas ; fabriquer un refus ici ajouterait un refus FANTÔME au
// compteur des 3 refus / 24 h, jusqu'à fermer la régulation sur une donnée qui
// n'existe pas.
//
// CONTRAINTE (style ADR-0017, comme dosing_logic / ph_offset) : pas d'Arduino.h,
// pas de FreeRTOS, pas de String — en-têtes C uniquement, plus constants.h qui
// est self-contained et porte les seuils (source unique, convention projet).
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"  // kPhGuardReleaseHysteresis, kPhOffset*, seuils EZO

// Garde de seuil BAS, AVEC hystérésis de relâche.
// Usage : refus d'ACIDIFIER (produit `ph_minus`) sous kPhFloorCorrected sur le pH
// CORRIGÉ, et butée brute basse kPhFloorRawHard sur le pH BRUT.
//
// Entrée en blocage : `value < threshold`.
// Sortie de blocage : seulement à `value >= threshold + kPhGuardReleaseHysteresis`.
// Sans cette hystérésis, une mesure qui oscille autour du seuil produit une rafale
// de transitions bloque/relâche, chacune comptabilisée comme un refus → bascule
// fail-closed de la régulation sur un incident unique.
//
// NaN (mesure ou seuil) → false : aucun refus fantôme, aucun incrément du
// compteur de refus.
//
// @param value       Mesure pH, dans le MÊME référentiel que `threshold`
// @param threshold   Seuil applicable
// @param wasBlocking État précédent de la garde (true si elle bloquait déjà)
// @return            true si le dosage doit être refusé
inline bool phGuardBlocksBelow(float value, float threshold, bool wasBlocking) {
  if (isnan(value) || isnan(threshold)) return false;
  if (wasBlocking) {
    // Relâche retardée : il faut remonter FRANCHEMENT au-dessus du seuil.
    return value < (threshold + kPhGuardReleaseHysteresis);
  }
  return value < threshold;
}

// Garde de seuil HAUT, AVEC hystérésis de relâche — miroir exact de
// phGuardBlocksBelow().
// Usage : refus d'ALCALINISER (produit `ph_plus`) au-dessus de
// kPhCeilingCorrected sur le pH CORRIGÉ, inhibition du dosage ORP au-dessus de
// kPhOrpInhibit sur le pH CORRIGÉ, et butée brute haute kPhCeilingRawHard sur le
// pH BRUT.
//
// Entrée en blocage : `value > threshold`.
// Sortie de blocage : seulement à `value <= threshold − kPhGuardReleaseHysteresis`.
//
// NaN (mesure ou seuil) → false : aucun refus fantôme, aucun incrément du
// compteur de refus. Attention : l'inhibition ORP automatique impose SON PROPRE
// fail-closed sur pH inconnu (pool-chemistry P2) — il est traité chez l'appelant,
// AVANT cette fonction, parce qu'il relève d'une politique et non d'un seuil.
//
// @param value       Mesure pH, dans le MÊME référentiel que `threshold`
// @param threshold   Seuil applicable
// @param wasBlocking État précédent de la garde (true si elle bloquait déjà)
// @return            true si le dosage doit être refusé
inline bool phGuardBlocksAbove(float value, float threshold, bool wasBlocking) {
  if (isnan(value) || isnan(threshold)) return false;
  if (wasBlocking) {
    // Relâche retardée : il faut redescendre FRANCHEMENT sous le seuil.
    return value > (threshold - kPhGuardReleaseHysteresis);
  }
  return value > threshold;
}

// Arrondit un offset pH au plus proche multiple de kPhOffsetStep (0,1 pH) et le
// borne à [kPhOffsetMin, kPhOffsetMax].
//
// Partagée par la route de sauvegarde (le curseur UI ne produit déjà que des
// multiples du pas, mais l'API et MQTT restent ouverts) et par la migration NVS
// des installations existantes, qui ont pu enregistrer une valeur libre (0,47).
// L'écart maximal introduit est de 0,05 pH, soit le bruit résiduel du filtre :
// aucun effet mesurable sur la régulation.
//
// Offset NaN → 0,00 (neutre), cohérent avec applyPhOffset() qui neutralise déjà
// un offset NaN. Le résultat n'est jamais −0,0 (il s'afficherait « -0.00 »).
//
// @param offset Offset utilisateur en unités pH
// @return       Offset arrondi au pas et borné
inline float roundPhOffsetToStep(float offset) {
  if (isnan(offset)) return 0.0f;
  float clamped = offset;
  if (clamped < kPhOffsetMin) clamped = kPhOffsetMin;
  if (clamped > kPhOffsetMax) clamped = kPhOffsetMax;
  float snapped = roundf(clamped / kPhOffsetStep) * kPhOffsetStep;
  // Re-bornage : l'arrondi flottant peut dépasser d'un ulp la borne d'origine.
  if (snapped < kPhOffsetMin) snapped = kPhOffsetMin;
  if (snapped > kPhOffsetMax) snapped = kPhOffsetMax;
  if (snapped == 0.0f) return 0.0f;  // neutralise le zéro négatif
  return snapped;
}

// True si le diagnostic électrochimique EZO décrit une sonde pH SAINE :
// les deux pentes dans [kPhSlopeHealthyMinPct, kPhSlopeHealthyMaxPct] ET un zéro
// dont la valeur absolue ne dépasse pas kPhZeroHealthyMaxMv.
//
// Toute valeur NaN (query Slope,? jamais réussie, bus I²C dégradé, firmware EZO
// qui ne rapporte pas le zéro) → false : on ne DÉCLARE PAS une sonde saine sans
// preuve. Le seul usage est la formulation du message d'escalade, jamais une
// autorisation de dosage.
//
// @param slopeAcid % de la pente Nernst côté acide (idéal 100)
// @param slopeBase % de la pente Nernst côté base (idéal 100)
// @param zeroMv    Décalage du point isopotentiel en mV (idéal 0)
inline bool isPhProbeHealthy(float slopeAcid, float slopeBase, float zeroMv) {
  if (isnan(slopeAcid) || isnan(slopeBase) || isnan(zeroMv)) return false;
  if (slopeAcid < kPhSlopeHealthyMinPct || slopeAcid > kPhSlopeHealthyMaxPct) return false;
  if (slopeBase < kPhSlopeHealthyMinPct || slopeBase > kPhSlopeHealthyMaxPct) return false;
  return fabsf(zeroMv) <= kPhZeroHealthyMaxMv;
}

// True si un changement de mode de régulation pH exige un ACQUITTEMENT explicite
// de l'utilisateur : tant qu'une trace de bascule automatique existe, la sortie
// de `manual` vers `automatic` ne peut se faire que par un acte délibéré
// (fail-closed — l'incident du 2026-08-25 a vu une simple consultation de l'app
// réarmer la régulation et effacer la trace).
//
// Retour false pour tout ce qui n'est PAS une sortie de manual : entrer en
// manual, rester dans le même mode, ou absence de trace.
//
// @param traceActive  Une trace de bascule automatique non acquittée existe
// @param curIsManual  Mode courant == "manual"
// @param newIsManual  Mode demandé == "manual"
// @param ack          Acquittement explicite fourni par la requête
inline bool phModeChangeNeedsAck(bool traceActive, bool curIsManual,
                                 bool newIsManual, bool ack) {
  if (!traceActive) return false;
  if (!curIsManual) return false;   // pas de bascule en cours à protéger
  if (newIsManual) return false;    // reste en manual : rien à acquitter
  return !ack;
}

#endif // PH_GUARD_LOGIC_H
