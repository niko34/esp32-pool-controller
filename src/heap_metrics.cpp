#include "heap_metrics.h"

#include <atomic>
#include <esp_heap_caps.h>

#include "constants.h"  // kOomProbeBlockUnknown (sentinelle « plus gros bloc non renseigné »)
#include "oom_guard_logic.h"  // oomProbeUsesRegionScan (sélection de branche, PURE — ADR-0017)

// La sélection de branche de la sonde est décidée par un prédicat PUR (testable
// en natif, cf. ADR-0017), qui recopie un bit MALLOC_CAP_* sans inclure
// esp_heap_caps.h. Cette égalité est le seul point de contact : si l'IDF changeait
// la valeur du bit, c'est la COMPILATION du firmware qui casse, pas un test natif
// silencieusement devenu faux.
// L'assert épingle MALLOC_CAP_**INTERNAL**, et non plus MALLOC_CAP_SPIRAM :
// l'invariant qui protège n'est pas « ce masque demande-t-il de la PSRAM » mais
// « ce masque peut-il ATTEINDRE le tas PSRAM ». Épingler le bit SPIRAM est en
// partie ce qui a laissé passer le défaut bloquant relevé en revue (caps
// MALLOC_CAP_DEFAULT = 0x1000 pour tout malloc()/new ordinaire).
static_assert(kOomProbeInternalCapBit == (uint32_t)MALLOC_CAP_INTERNAL,
              "kOomProbeInternalCapBit doit valoir exactement MALLOC_CAP_INTERNAL : "
              "la borne a priori du pire cas PSRAM de la sonde en depend (seul un "
              "masque garantissant la RAM interne autorise le parcours O(blocs)).");

// Capacités du pool « réellement allouable » : mémoire INTERNE (pas de PSRAM,
// qui a ses propres contraintes) ET accessible en accès OCTET — c'est
// exactement ce qu'exige un malloc()/new ordinaire. Voir heap_metrics.h pour la
// raison pour laquelle MALLOC_CAP_INTERNAL seul est trompeur (IRAM 32 bits).
static constexpr uint32_t kHeapCaps8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

uint32_t heapLargestFreeBlock8Bit() {
  return (uint32_t)heap_caps_get_largest_free_block(kHeapCaps8Bit);
}

uint32_t heapFree8Bit() {
  return (uint32_t)heap_caps_get_free_size(kHeapCaps8Bit);
}

uint32_t heapCaps8BitMask() {
  return kHeapCaps8Bit;
}

HeapProbe heapProbeForCaps(uint32_t caps) {
  HeapProbe p;
  if (oomProbeUsesRegionScan(caps)) {
    // Condition 2 du gate pool-chemistry (bug-080 B-2) : BORNE A PRIORI. Sur
    // 4 Mo de PSRAM, le parcours O(blocs) de `largest_free_block` n'a pas de
    // pire cas connu — et il s'exécute interruptions coupées. On ne SAUTE pas la
    // sonde (interdit : les 12 sites d'appel du hook doivent tous rendre un
    // verdict), on change d'ALGORITHME : agrégats O(régions), plus gros bloc
    // déclaré non renseigné. Le verdict du post-mortem sait le lire ainsi.
    p.freeBytes = (uint32_t)heap_caps_get_free_size(caps);
    p.largestBlockBytes = kOomProbeBlockUnknown;
    return p;
  }
  // UN seul parcours : heap_caps_get_info() livre les deux grandeurs.
  multi_heap_info_t info;
  heap_caps_get_info(&info, caps);
  p.freeBytes = (uint32_t)info.total_free_bytes;
  p.largestBlockBytes = (uint32_t)info.largest_free_block;
  return p;
}

HeapMetrics heapMetricsSnapshot() {
  // Quatre appels heap_caps_* consécutifs : chacun s'exécute interruptions
  // coupées, d'où le regroupement en un instantané unique plutôt que des appels
  // dispersés dans les appelants. Aucun log, aucune allocation ici.
  HeapMetrics m;
  m.free8 = (uint32_t)heap_caps_get_free_size(kHeapCaps8Bit);
  m.maxAlloc8 = (uint32_t)heap_caps_get_largest_free_block(kHeapCaps8Bit);
  m.freeInternal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m.maxAllocInternal = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  return m;
}

// ---------------------------------------------------------------------------
// Suivi des creux (revue v2.19.0)
// ---------------------------------------------------------------------------
// Alimenté par heapRingSample() toutes les 2 s avec une valeur DÉJÀ mesurée :
// aucune mesure supplémentaire, aucun coût. Atomiques car la lecture (loopTask,
// trace de calibrage) et l'écriture (loopTask également aujourd'hui) doivent
// rester sûres si l'échantillonnage migrait un jour vers une autre tâche.
static std::atomic<uint32_t> sMinMaxAlloc8SinceBoot{UINT32_MAX};
static std::atomic<uint32_t> sMinMaxAlloc8Window{UINT32_MAX};

void heapNoteMaxAlloc8Sample(uint32_t maxAlloc8) {
  uint32_t boot = sMinMaxAlloc8SinceBoot.load(std::memory_order_relaxed);
  if (maxAlloc8 < boot) sMinMaxAlloc8SinceBoot.store(maxAlloc8, std::memory_order_relaxed);
  uint32_t win = sMinMaxAlloc8Window.load(std::memory_order_relaxed);
  if (maxAlloc8 < win) sMinMaxAlloc8Window.store(maxAlloc8, std::memory_order_relaxed);
}

uint32_t heapMinMaxAlloc8SinceBoot() {
  return sMinMaxAlloc8SinceBoot.load(std::memory_order_relaxed);
}

uint32_t heapTakeMinMaxAlloc8Window() {
  // exchange : la fenêtre repart à vide pour la prochaine période de trace.
  // Course bénigne et documentée (revue) : un échantillon chargé juste avant
  // l'exchange et rangé juste après n'est PAS perdu — il est attribué à la
  // fenêtre SUIVANTE. Sans effet à l'échelle d'une fenêtre de 5 min
  // (≈ 150 échantillons à 2 s).
  return sMinMaxAlloc8Window.exchange(UINT32_MAX, std::memory_order_relaxed);
}
