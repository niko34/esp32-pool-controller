#ifndef PH_OFFSET_H
#define PH_OFFSET_H

// =============================================================================
// ph_offset — Offset logiciel de mesure pH, logique PURE (feature-067)
// =============================================================================
// Correction utilisateur du décalage d'implantation de la sonde pH :
//   pH corrigé = pH brut EZO + offset
// La calibration électrochimique du module Atlas EZO n'est JAMAIS touchée.
//
// Point d'application (décision architect) : EN SORTIE des accesseurs de
// SensorManager, jamais à l'ingestion du filtre. Le filtre pH (src/sensor_filter)
// continue de travailler sur le signal BRUT, et un changement d'offset est donc
// pris en compte IMMÉDIATEMENT (pas dilué sur la fenêtre de lissage).
// NB (feature-152) : cette phrase citait « latch EMI, seuils de rejet de pics et
// dispersion » — ces trois mécanismes N'EXISTENT PLUS. Le point d'application de
// l'offset, lui, est inchangé : c'est tout ce que ce paragraphe énonce.
//
// CONTRAINTE (style ADR-0017, comme dosing_logic / measure_validity_logic) :
// pas d'Arduino.h, pas de FreeRTOS, pas de String — en-têtes C uniquement, plus
// constants.h qui est self-contained (<stdint.h>/<time.h>) et porte les bornes
// (source unique des seuils, convention projet). Testable en natif.
// =============================================================================

#include <stdint.h>
#include <math.h>

#include "constants.h"  // kPhOffsetScaleSpan (self-contained, sans Arduino)

// Applique l'offset utilisateur à une mesure pH BRUTE.
//
// CONTRAT — NaN in → NaN out : l'offset ne doit JAMAIS fabriquer une mesure à
// partir de rien (mesure indisponible, capteur stale, filtre non amorcé), sinon
// il lèverait les gardes `isnan` qui protègent le dosage.
// Un offset lui-même NaN est neutralisé (retour du brut) : fail-safe.
//
// @param raw    Mesure pH brute (NaN si indisponible)
// @param offset Offset utilisateur en unités pH (0.0 = neutre)
// @return       Mesure corrigée, ou NaN si `raw` est NaN
inline float applyPhOffset(float raw, float offset) {
  if (isnan(raw)) return raw;
  if (isnan(offset)) return raw;
  return raw + offset;
}

// ⚠️ ANCIENNEMENT une borne de RÉGLAGE (feature-067 → feature-155), ce n'en
// est PLUS une depuis feature-168 (décision utilisateur du 2026-10-16, GO sous
// conditions du gate pool-chemistry, condition C3) : la correction de mesure
// pH accepte désormais TOUTE valeur finie de l'échelle pH, sans écrêtage ni
// refus lié à son amplitude. Ce test ne protège plus que la SALUBRITÉ de
// domaine — NaN, infini, ou une valeur si extravagante (ex. 1e30) qu'elle ne
// peut être qu'une corruption ou une faute de saisie, jamais une correction
// réelle. kPhOffsetScaleSpan (= 14,0, l'échelle pH entière) n'est PAS un seuil
// chimique : ADR-0100, ce n'est jamais réécrit côté client.
// ⚠️ MISE EN GARDE CONSERVÉE (feature-155) : un |offset| élevé N'EST PAS, en
// soi, une suspicion de sonde. L'incident du 2026-08-25 a vu une sonde SAINE
// (pente 101/100, zéro −1,03) déclarée à remplacer alors que la cause était un
// décalage EXTERNE (courants de fuite de la pompe de filtration). Un écart
// large a d'AUTRES causes au moins aussi probables : test de référence périmé,
// lecture à la mauvaise échelle, étalonnage à refaire. Ne pas désigner un
// coupable qu'on n'a pas identifié — ni ici, ni dans un message dérivé. C'est
// désormais kPhOffsetUnusualAbs (constants.h) qui porte le SIGNAL (pas le
// refus) d'un défaut matériel possible.
inline bool isPhOffsetValid(float offset) {
  // isfinite() couvre déjà NaN ET infini (tous deux → false) : formule exacte
  // de la condition C3 du gate, pas de test isnan() redondant devant.
  return isfinite(offset) && (fabsf(offset) <= kPhOffsetScaleSpan);
}

// =============================================================================
// feature-156 — HORODATAGE DE JUSTESSE (correction manuelle vs étalonnage)
// =============================================================================
// Un étalonnage EZO modifie le zéro ET la pente de la sonde : une correction
// manuelle posée AVANT ce dernier étalonnage compensait un état de la sonde
// qui n'existe plus — elle est CADUQUE. `seq` (pas `epoch`) fait foi pour cet
// ordre : voir config.h (champs phOffset*/phCal*) pour la justification
// complète (indépendance à l'horloge et au redémarrage).

// =============================================================================
// feature-168 (C10) — DEUXIÈME CAUSE DE CADUCITÉ, INDÉPENDANTE DE L'ÉTALONNAGE
// =============================================================================
// Une correction INHABITUELLE (|offset| > unusualAbs) non reconfirmée depuis
// plus de confirmIntervalDays jours est elle aussi caduque : elle compense un
// décalage d'INSTALLATION (courants de fuite de la pompe de filtration) qui
// peut avoir changé depuis. Non bloquant, conforme à la décision utilisateur
// du 2026-10-16 (« je ne veux pas bloquer »).
//
// True ssi : (a) une correction non nulle en vigueur est ANTÉRIEURE au dernier
// étalonnage réussi — donc caduque au sens feature-156 (`offsetSeq`/`calSeq`,
// les séquences des deux écrivains uniques, config.h) ; une correction nulle
// (0,00 = inerte) n'est jamais caduque par cette voie, et deux séquences
// ÉGALES (aucun des deux événements ne s'est jamais produit après l'autre, y
// compris 0 == 0 au premier démarrage) ne sont PAS une préséance de
// l'étalonnage ;
// OU (b) l'offset est INHABITUEL et n'a pas été reconfirmé depuis plus de
// `confirmIntervalDays` jours (feature-168, C10).
//
// Fail-open sur horloge non fiable (paramètre EXPLICITE, pas un appel time()
// masqué — cette fonction reste pure et testable en natif, ADR-0017) :
// `offsetEpoch == 0` (date inconnue à la pose) ou `nowEpoch == 0` (horloge pas
// encore synchronisée) ⇒ cause (b) jamais levée. Une horloge qui serait
// repartie en arrière (nowEpoch < offsetEpoch, ex. RTC non recalée) fail-open
// aussi : mieux vaut ne pas signaler qu'afficher un âge négatif.
//
// @param offset               Offset APPLIQUÉ (mqttCfg.phOffset)
// @param offsetSeq            Séquence de la dernière correction (feature-156)
// @param calSeq               Séquence du dernier étalonnage (feature-156)
// @param offsetEpoch          Horodatage de la correction (0 = date inconnue)
// @param nowEpoch             Horodatage courant (0 = horloge non synchronisée)
// @param confirmIntervalDays  Péremption en jours (kPhOffsetConfirmIntervalDays)
// @param unusualAbs           Seuil d'« inhabituel » (kPhOffsetUnusualAbs)
inline bool isPhOffsetExpired(float offset, uint32_t offsetEpoch, uint32_t nowEpoch,
                              uint32_t confirmIntervalDays, float unusualAbs) {
  if (offsetEpoch == 0 || nowEpoch == 0) return false;     // horloge non fiable
  if (nowEpoch < offsetEpoch) return false;                // horloge retour arriere
  if (fabsf(offset) <= unusualAbs) return false;            // pas "inhabituel"
  const uint32_t ageDays = (nowEpoch - offsetEpoch) / 86400UL;
  return ageDays > confirmIntervalDays;
}

// Cause (a) SEULE : la correction a ete posee AVANT le dernier etalonnage, elle
// compense un etat de sonde qui n'existe plus.
//
// ⚠️ Extraite en fonction propre sur revue (feature-168) : l'expression vivait
// en DEUX exemplaires — ici et dans phOffsetStaleCause() (config.cpp), qui
// choisit le libelle affiche. C'est le piege « meme notion, deux expressions »
// d'un cran en dessous de celui que phOffsetStaleNow() venait d'eliminer : la
// cause (a) aurait pu evoluer ici et laisser le libelle afficher autre chose
// que le verdict. ⛔ Un seul endroit, desormais.
inline bool isPhOffsetSupersededByCalibration(float offset, uint32_t offsetSeq,
                                              uint32_t calSeq) {
  return (offset != 0) && (offsetSeq < calSeq);
}

inline bool isPhOffsetStale(float offset, uint32_t offsetSeq, uint32_t calSeq,
                            uint32_t offsetEpoch, uint32_t nowEpoch,
                            uint32_t confirmIntervalDays, float unusualAbs) {
  if (isPhOffsetSupersededByCalibration(offset, offsetSeq, calSeq)) {
    return true;                                           // cause (a) : calibration
  }
  return isPhOffsetExpired(offset, offsetEpoch, nowEpoch, confirmIntervalDays,
                            unusualAbs);                   // cause (b) : expiration
}

// Prochaine séquence « justesse », toujours strictement supérieure aux deux
// séquences passées en argument — insensible à l'ordre des arguments
// (monotone quel que soit celui des deux événements qui vient de se produire).
inline uint32_t nextJustnessSeq(uint32_t a, uint32_t b) {
  return (a > b ? a : b) + 1;
}

#endif // PH_OFFSET_H
