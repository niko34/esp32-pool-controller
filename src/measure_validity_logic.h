#ifndef MEASURE_VALIDITY_LOGIC_H
#define MEASURE_VALIDITY_LOGIC_H

// =============================================================================
// measure_validity_logic — Validité des mesures pH/ORP PURE (feature-061)
// =============================================================================
// Décision « les mesures pH/ORP sont-elles représentatives de l'eau du bassin ? »
// extraite en module pur pour être testable en natif (sans ESP32, sans FreeRTOS,
// sans Arduino). Style ADR-0017, comme dosing_logic / history_guard_logic.
//
// Les sondes sont montées dans le circuit de filtration : filtration arrêtée =
// eau stagnante autour des sondes → la valeur dérive localement et ne reflète
// plus le bassin. À la reprise de la circulation, un délai de purge (l'eau
// stagnante de la chambre de mesure doit être chassée) est appliqué avant de
// redéclarer les mesures valides.
//
// RÈGLES (ordre strict, priorité décroissante) :
//   1. eau absente                       → invalide, remaining = 0 (pas de
//      compte à rebours : on ignore quand la circulation reprendra) ;
//   2. délai de purge == 0               → valide, remaining = 0 (purge
//      désactivée : valide dès la reprise) ;
//   3. front montant jamais observé (waterRiseMs == 0, sentinelle) → invalide,
//      remaining = délai complet arrondi seconde SUP (conservateur : sans
//      horodatage de reprise on applique la purge entière — la coquille arme
//      toujours le front, ce cas ne survient pas en pratique) ;
//   4. (nowMs - waterRiseMs) < délai     → invalide, remaining = restant
//      arrondi seconde SUP (soustraction non signée wrap-safe) ;
//   5. sinon                             → valide, remaining = 0.
//
// FRONTIÈRES figées par les tests : elapsed == délai - 1 → invalide (remaining
// = 1 s) ; elapsed == délai → valide ; elapsed == délai + 1 → valide.
//
// INVARIANT : ce module ne fait QUE décider à partir de valeurs déjà collectées.
// La collecte (resolveWaterPresence(), millis(), mqttCfg.stabilizationDelayMin),
// la détection du front montant et les logs restent dans la coquille impure
// measure_validity.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. On
// utilise l'en-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// Entrées de la décision — toutes fournies par l'appelant (aucune API Arduino).
struct MeasureValidityInputs {
  bool waterPresent;      // resolveWaterPresence().waterPresent (source UNIQUE feature-056)
  uint32_t nowMs;         // millis() courant
  uint32_t waterRiseMs;   // millis() du dernier front montant d'eau ; 0 = jamais
                          // observé (sentinelle — la coquille écrit toujours ≥ 1)
  uint32_t purgeDelayMs;  // délai de purge (mqttCfg.stabilizationDelayMin * 60000) ; 0 = désactivé
};

// Verdict de validité des mesures pH/ORP.
struct MeasureValidityResult {
  bool valid;                        // true = mesures représentatives du bassin
  uint32_t stabilizationRemainingS;  // secondes restantes de purge, arrondi
                                     // seconde SUP ; 0 si valide OU eau absente
};

// Décision pure de validité (voir règles en tête de fichier).
MeasureValidityResult evaluateMeasureValidity(const MeasureValidityInputs& in);

#endif  // MEASURE_VALIDITY_LOGIC_H
