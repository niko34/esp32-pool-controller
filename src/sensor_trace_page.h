#ifndef SENSOR_TRACE_PAGE_H
#define SENSOR_TRACE_PAGE_H

// =============================================================================
// sensor_trace_page — Clamp de pagination PUR pour GET /debug/sensor_trace
// =============================================================================
// Bug-crash-sensor-trace-badalloc : la route sérialisait les 300 échantillons
// du ring buffer en une seule réponse (~20 Ko contigus dans le cbuf de
// AsyncResponseStream) → std::bad_alloc → abort() sur heap fragmenté. La route
// est désormais paginée ; ce module calcule les bornes effectives [offset, n)
// d'une page à partir de paramètres de query non fiables. Extrait en header
// pur pour être testable en natif (style ADR-0017, cf. history_guard_logic.h).
//
// INVARIANT : ce module ne fait QUE clamper des tailles. La lecture lock-free
// du ring buffer et la sérialisation JSON restent dans sensors.cpp /
// web_routes_debug.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stddef.h> uniquement,
// fonctions inline (header-only, aucun .cpp à ajouter au build natif).
// =============================================================================

#include <stddef.h>

// Bornes effectives d'une page : `offset` = index du premier échantillon
// (chronologique, 0 = le plus ancien), `n` = nombre d'échantillons à retourner.
struct TracePageBounds {
  size_t offset;
  size_t n;
};

// Clamp pur des paramètres de pagination. Règles :
//   - offset clampé à [0, total] (offset == total → page vide, n = 0) ;
//   - count == 0 (paramètre absent ou explicite) → maxPerPage demandé ;
//   - n = min(count demandé, maxPerPage, échantillons restants après offset).
// Jamais de débordement : offset + n <= total, n <= maxPerPage, quel que soit
// l'input (y compris SIZE_MAX). Le glissement du ring buffer entre deux pages
// est toléré (route de diagnostic, cf. contrat SensorTraceSample).
inline TracePageBounds computeTracePage(size_t total, size_t offset,
                                        size_t count, size_t maxPerPage) {
  TracePageBounds b;
  b.offset = (offset > total) ? total : offset;
  size_t wanted = (count == 0) ? maxPerPage : count;
  if (wanted > maxPerPage) wanted = maxPerPage;
  size_t remaining = total - b.offset;
  b.n = (wanted > remaining) ? remaining : wanted;
  return b;
}

#endif  // SENSOR_TRACE_PAGE_H
