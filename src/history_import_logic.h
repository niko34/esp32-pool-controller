#ifndef HISTORY_IMPORT_LOGIC_H
#define HISTORY_IMPORT_LOGIC_H

// =============================================================================
// history_import_logic — les trois règles de l'import d'historique par lots
// (bug-101 / bug-113, v2.48.0). Décisions PURES, ADR-0017.
// =============================================================================
// Ce module ne contient QUE les trois points où une erreur PERD DES DONNÉES :
//   1. historyImportSameSlot()     — quel point entrant écrase quel point vivant
//   2. historyImportTrimPlan()     — combien évincer par granularité après fusion
//   3. historyImportModeIsReplace() — « remplacer » ou « ajouter »
// Tout le reste (mutex, tri, écriture LittleFS, journal) reste dans la coquille
// impure history.cpp.
//
// CONTRAINTE DE PORTABILITÉ (identique à history_logic.h) : en-têtes C
// uniquement, pas d'Arduino.h, pas de FreeRTOS, pas de String, pas de <vector>.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include "constants.h"  // kMaxRawDataPoints / kMaxHourlyDataPoints / kMaxDailyDataPoints

// ---------------------------------------------------------------------------
// 1. Règle de collision : (timestamp, granularité)
// ---------------------------------------------------------------------------
// Deux points occupent le MÊME créneau si et seulement si leur timestamp ET leur
// granularité coïncident. Le point entrant ÉCRASE alors le point vivant —
// TOUTES GRANULARITÉS, RAW COMPRISE.
//
// POURQUOI L'ÉCRASEMENT ET NON LA FUSION (le choix est structurant, ne pas le
// « corriger ») :
//  - `dedupAggregatedPoints()` NE COUVRE PAS l'import. history.cpp fait
//    `if (p.granularity != RAW)` : aucun point RAW n'y est jamais fusionné, et
//    un export en porte jusqu'à kMaxRawDataPoints (96). Un lot RAW réenvoyé —
//    reprise après coupure réseau, clic sur « importer » deux fois — DOUBLERAIT
//    en silence. On ne peut donc pas s'appuyer sur le dédoublonnage existant.
//  - `historyMergeMeanFinite` fait une moyenne ÉQUIPONDÉRÉE. Sur une reprise, on
//    veut la valeur de la sauvegarde TELLE QUELLE, pas une moyenne entre la
//    sauvegarde et le vivant : la moyenne fabriquerait une mesure qui n'a jamais
//    existé, et la rendrait dépendante du NOMBRE de renvois.
//  - L'écrasement donne l'IDEMPOTENCE EXACTE : réenvoyer un lot n fois laisse le
//    même état qu'un seul envoi. C'est précisément ce qui rend une reprise sûre.
bool historyImportSameSlot(uint32_t tsA, uint8_t granA, uint32_t tsB, uint8_t granB);

// ---------------------------------------------------------------------------
// 2. Plan d'éviction après fusion
// ---------------------------------------------------------------------------
// Combien de points évincer par granularité pour revenir sous les plafonds.
// Les plafonds sont ceux de constants.h — source unique, jamais recopiés.
// Les trois granularités sont traitées : un lot importé apporte des points RAW
// (jusqu'à 96 dans un export), or le pas 4 de `consolidateData()` ne borne que
// HOURLY et DAILY — le bornage RAW vit dans `recordDataPoint()`, que l'import
// ne traverse pas.
struct HistoryImportTrimPlan {
  size_t rawToRemove = 0;
  size_t hourlyToRemove = 0;
  size_t dailyToRemove = 0;

  // Vrai si rien n'est à évincer (permet à la coquille de sauter le parcours).
  bool empty() const {
    return rawToRemove == 0 && hourlyToRemove == 0 && dailyToRemove == 0;
  }
};

// `raw` / `hourly` / `daily` = effectifs COURANTS après fusion du lot.
// Soustractions gardées (jamais d'underflow size_t) : un effectif sous le
// plafond donne 0, pas SIZE_MAX.
HistoryImportTrimPlan historyImportTrimPlan(size_t raw, size_t hourly, size_t daily);

// ---------------------------------------------------------------------------
// 3. Mode d'import : « remplacer » (défaut) ou « ajouter »
// ---------------------------------------------------------------------------
// ⚠️ LE DÉFAUT EST `replace`, ET C'EST LA SEULE VALEUR SÛRE.
// C'est LE point de ce module où une erreur détruit des données : un défaut
// `append` transformerait « je restaure une sauvegarde propre » en « je mélange
// la sauvegarde avec l'historique vivant », sans le dire à personne. Et il
// casserait le contrat des clients antérieurs à v2.48.0, qui n'envoient AUCUN
// champ `mode` et attendent le remplacement (comportement de `importData`).
//
// Une valeur INCONNUE retombe aussi sur `replace` : un client qui écrit
// « apend » ou « ADD » doit obtenir le comportement historique, jamais un
// mélange silencieux.
//
// Seul `append` (à la casse près) déclenche l'ajout. La tolérance de casse est
// délibérée et va dans le sens de la sûreté : « Append » interprété en `replace`
// écraserait l'historique vivant de quelqu'un qui demandait explicitement un
// ajout. On ne tolère RIEN d'autre — ni espaces, ni abréviation.
//
//   mode == nullptr        -> true  (champ absent)
//   mode == ""             -> true
//   mode == "replace"      -> true
//   mode == "append"/"APPEND"/"Append" -> false
//   toute autre valeur     -> true
bool historyImportModeIsReplace(const char* mode);

#endif  // HISTORY_IMPORT_LOGIC_H
