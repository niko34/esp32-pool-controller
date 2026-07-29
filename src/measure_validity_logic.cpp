// =============================================================================
// measure_validity_logic — Validité des mesures pH/ORP PURE (feature-061)
// =============================================================================
// Voir measure_validity_logic.h pour les règles et invariants. Compile en
// natif (pio test -e native) ET sur cible — aucune dépendance Arduino.
// =============================================================================

#include "measure_validity_logic.h"

namespace {
// Arrondi seconde SUPÉRIEURE (ms → s). 1 ms restante → 1 s affichée : l'UI ne
// montre jamais « 0 s restante » sur un état encore invalide.
uint32_t ceilSeconds(uint32_t ms) {
  return (ms + 999UL) / 1000UL;
}
}  // namespace

MeasureValidityResult evaluateMeasureValidity(const MeasureValidityInputs& in) {
  // Règle 1 : eau absente → mesures non représentatives, pas de compte à rebours.
  if (!in.waterPresent) {
    return { false, 0 };
  }

  // Règle 2 : purge désactivée (délai 0) → valide dès la reprise.
  if (in.purgeDelayMs == 0) {
    return { true, 0 };
  }

  // Règle 3 : front montant jamais observé (sentinelle 0) → conservateur,
  // délai complet (n'arrive pas en pratique : la coquille arme toujours ≥ 1).
  if (in.waterRiseMs == 0) {
    return { false, ceilSeconds(in.purgeDelayMs) };
  }

  // Règle 4 : purge en cours. Soustraction non signée wrap-safe (cohérente au
  // passage de millis() par 0xFFFFFFFF).
  uint32_t elapsed = in.nowMs - in.waterRiseMs;
  if (elapsed < in.purgeDelayMs) {
    return { false, ceilSeconds(in.purgeDelayMs - elapsed) };
  }

  // Règle 5 : purge écoulée → mesures valides.
  return { true, 0 };
}
