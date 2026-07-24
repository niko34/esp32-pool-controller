#ifndef LOGS_PAGE_H
#define LOGS_PAGE_H

// =============================================================================
// logs_page — Admission de page PURE pour GET /get-logs
// =============================================================================
// Bug-crash-getlogs-badalloc : la route sérialisait jusqu'à 200 entrées de log
// en une seule réponse (~20-40 Ko contigus dans le cbuf d'AsyncResponseStream,
// resize() = new[] throwant) → std::bad_alloc → abort() sur heap fragmenté.
// La route est désormais paginée via le curseur ?since= existant ; ce module
// décide, pour chaque entrée visitée, si elle entre dans la page courante.
// Extrait en header pur pour être testable en natif (style ADR-0017, cf.
// sensor_trace_page.h).
//
// Règles d'admission (dans l'ordre) :
//   1. included < softLimit               → include, continue.
//   2. included >= hardCap                → stop (coupe même une queue même-ts).
//   3. softLimit <= included < hardCap    → include SEULEMENT si ts est égal au
//      timestamp de la dernière entrée incluse (queue même-milliseconde : le
//      curseur ?since= est STRICTEMENT supérieur, exclure ces entrées les
//      perdrait définitivement entre deux pages) ; sinon stop.
//
// Cas dégénéré assumé : plus de hardCap entrées partageant la même milliseconde
// → la règle 2 coupe quand même ; les entrées même-ms résiduelles sont perdues
// pour le curseur (elles restent visibles dans /download-logs). En pratique le
// logger ne produit jamais 60 entrées dans la même ms.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stddef.h> uniquement,
// fonctions inline (header-only, aucun .cpp à ajouter au build natif).
// =============================================================================

#include <stddef.h>

// Décision d'admission : include = ajouter l'entrée à la page ; stop = arrêter
// l'itération (page pleine → l'appelant marque la réponse `truncated`).
struct LogsPageDecision {
  bool include;
  bool stop;
};

// Admission d'une entrée dans la page. `included` = nombre d'entrées déjà
// incluses, `ts` = timestamp de l'entrée candidate, `lastIncludedTs` =
// timestamp de la dernière entrée incluse (indéfini si included == 0 — jamais
// consulté dans ce cas, règle 1 prime).
inline LogsPageDecision logsPageAdmit(size_t included, unsigned long ts,
                                      unsigned long lastIncludedTs,
                                      size_t softLimit, size_t hardCap) {
  LogsPageDecision d;
  if (included < softLimit) {
    d.include = true;
    d.stop = false;
  } else if (included >= hardCap) {
    d.include = false;
    d.stop = true;
  } else if (ts == lastIncludedTs) {
    d.include = true;
    d.stop = false;
  } else {
    d.include = false;
    d.stop = true;
  }
  return d;
}

#endif  // LOGS_PAGE_H
