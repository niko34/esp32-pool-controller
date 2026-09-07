#ifndef TEMP_COMPENSATION_LOGIC_H
#define TEMP_COMPENSATION_LOGIC_H

// =============================================================================
// temp_compensation_logic — Bornes de la compensation T° du pH, logique PURE
// (bug-114)
// =============================================================================
// Deux décisions, une seule règle chacune, toutes deux hors de tout contexte
// Arduino/FreeRTOS :
//   1. borner l'offset de calibration de la sonde eau ;
//   2. choisir la température envoyée dans `RT,<tempC>` à l'EZO pH, par une
//      CASCADE de repli.
//
// CONTRAINTE (style ADR-0017, comme ph_offset.h / dosing_logic.h) : pas
// d'Arduino.h, pas de FreeRTOS, pas de String — en-têtes C uniquement, plus
// constants.h qui est self-contained (<stdint.h>/<time.h>) et porte les bornes
// (source unique des seuils, convention projet). Donc testable en natif.
//
// ⛔ L'OPTION « SUSPENDRE LA COMPENSATION » N'EXISTE PAS — et c'est exactement
//    la « simplification » que quelqu'un tentera en relisant ce fichier.
//    Ne pas envoyer `RT` fait retomber la lecture sur `R` : le module EZO
//    CONSERVE alors sa dernière température interne, silencieusement périmée,
//    et cette valeur devient INVISIBLE et INDATABLE côté firmware. C'est pire
//    que n'importe quel repli. Il y a donc TOUJOURS une valeur envoyée, et
//    l'étage de la cascade qui l'a produite est toujours exposé.
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"  // kTempCalOffset*, kEzoCompTemp*, kEzoFallbackTempC

// Étage de la cascade qui a fourni la température de compensation effectivement
// envoyée à l'EZO pH. Sert au diagnostic : c'est le seul moyen de distinguer une
// valeur COURANTE d'un REPLI (et, croisé avec l'offset, d'une valeur FIGÉE).
enum class TempCompStage : uint8_t {
  Current      = 0,  // Mesure courante, plausible → aucune substitution
  LastPlausible = 1, // Substitution : dernière valeur plausible, encore fraîche
  Fallback     = 2   // Substitution : kEzoFallbackTempC (dernier recours)
};

// Résultat de la cascade.
struct TempCompDecision {
  float tempC;           // Température à envoyer dans `RT,<tempC>` (jamais NaN)
  TempCompStage stage;   // Étage qui l'a produite
};

// Borne l'offset utilisateur de la sonde eau à [kTempCalOffsetMin,
// kTempCalOffsetMax]. Un offset NaN est neutralisé à 0 (fail-safe : mieux vaut
// pas de correction qu'une correction indéterminée).
//
// Appliqué à QUATRE sites, à dessein — ce n'est PAS de la redondance : chacun
// couvre une provenance que les autres ne voient pas. Compte relevé par
// `grep -rn clampTempCalOffset src/` le 2026-09-07 (ADR-0073 : le nombre porte
// sa méthode, sinon il se recopie au lieu de se refaire).
//
//   1/4  web_routes_config.cpp — écriture par la route de configuration
//   2/4  config.cpp            — chargement NVS (une version antérieure
//                                n'imposait aucune borne ; sans ce clamp,
//                                /get-config afficherait une valeur que le
//                                firmware n'applique pas — piège documenté
//                                « config d'usine ≠ config vivante »)
//   3/4  sensors.cpp           — émission de la valeur de MESURE (affichage,
//                                MQTT, historique). N'alimente PAS `RT`.
//   4/4  sensors.cpp           — ÉMISSION `RT` : le seul site qui alimente la
//                                commande de compensation envoyée à la sonde,
//                                via getWaterTemperature(). Les trois autres
//                                ne couvrent pas ce chemin.
//
// ⚠️ NE PAS FUSIONNER 3/4 et 4/4, et n'en supprimer aucun comme doublon : la
// sentinelle de test_native_temp_compensation vise chacun DANS SA FONCTION, sur
// code dépouillé de ses commentaires — une suppression fait rougir la suite.
// Cette liste a elle-même annoncé « DEUX » puis « TROIS » avant d'être grepée.
inline float clampTempCalOffset(float offset) {
  if (isnan(offset)) return 0.0f;
  if (offset < kTempCalOffsetMin) return kTempCalOffsetMin;
  if (offset > kTempCalOffsetMax) return kTempCalOffsetMax;
  return offset;
}

// True si `offset` est DÉJÀ dans les bornes (donc clampTempCalOffset() est
// l'identité). Permet à l'appelant de journaliser l'écrêtage — l'utilisateur
// doit savoir que sa valeur a été bornée.
inline bool isTempCalOffsetInRange(float offset) {
  if (isnan(offset)) return false;
  return (offset >= kTempCalOffsetMin) && (offset <= kTempCalOffsetMax);
}

// True si `tempC` est une température de compensation PLAUSIBLE pour un bassin,
// c'est-à-dire numérique et dans [kEzoCompTempMinC, kEzoCompTempMaxC].
// Les bornes et leurs motifs sont dans constants.h : ne pas les resserrer sans
// repasser par `pool-chemistry` — rejeter une température VRAIE dégrade la
// compensation au lieu de la protéger.
inline bool isCompensationTempPlausible(float tempC) {
  if (isnan(tempC)) return false;
  return (tempC >= kEzoCompTempMinC) && (tempC <= kEzoCompTempMaxC);
}

// Résout la température de compensation par CASCADE :
//   1. `measuredC` si elle est plausible                     → Current
//   2. sinon `lastPlausibleC` si elle est plausible ET que son âge est sous la
//      fenêtre stale `staleMs`                               → LastPlausible
//   3. sinon kEzoFallbackTempC                               → Fallback
//
// @param measuredC        Mesure courante (T° eau + offset borné) ; NaN accepté
// @param lastPlausibleC   Dernière valeur ayant passé l'étape 1 ; NaN accepté
// @param lastPlausibleAgeMs Âge de `lastPlausibleC` en ms (millis() − horodatage)
// @param staleMs          Fenêtre de fraîcheur au-delà de laquelle l'étape 2 est
//                         refusée (kSensorStaleTimeoutMs côté firmware)
// @return                 Valeur à envoyer + étage l'ayant produite (jamais NaN)
inline TempCompDecision resolveCompensationTemp(float measuredC,
                                                float lastPlausibleC,
                                                uint32_t lastPlausibleAgeMs,
                                                uint32_t staleMs) {
  if (isCompensationTempPlausible(measuredC)) {
    return TempCompDecision{measuredC, TempCompStage::Current};
  }
  if (isCompensationTempPlausible(lastPlausibleC) && lastPlausibleAgeMs <= staleMs) {
    return TempCompDecision{lastPlausibleC, TempCompStage::LastPlausible};
  }
  return TempCompDecision{kEzoFallbackTempC, TempCompStage::Fallback};
}

// Libellé français de l'étage, pour la journalisation et l'exposition.
inline const char* tempCompStageLabel(TempCompStage stage) {
  switch (stage) {
    case TempCompStage::Current:       return "mesure";
    case TempCompStage::LastPlausible: return "derniere_valeur_plausible";
    case TempCompStage::Fallback:      return "repli_defaut";
    default:                           return "inconnu";
  }
}

#endif  // TEMP_COMPENSATION_LOGIC_H
