#ifndef HEAP_METRICS_H
#define HEAP_METRICS_H

#include <stdint.h>

// =============================================================================
// heap_metrics — mesure de la mémoire RÉELLEMENT allouable (v2.19.0)
// =============================================================================
// POURQUOI CE MODULE (bug-metrique-heap-inclut-iram-32bits) :
// `ESP.getFreeHeap()` et `ESP.getMaxAllocHeap()` interrogent le pool
// `MALLOC_CAP_INTERNAL`. Or MALLOC_CAP_INTERNAL englobe AUSSI l'IRAM qui n'est
// accessible qu'en accès 32 bits alignés. Cette IRAM ne peut PAS servir à un
// `malloc()`/`new` ordinaire : l'allocateur exige alors `MALLOC_CAP_8BIT`
// (accès octet). Conséquence observée en production : un « plus gros bloc »
// figé à ~27,5 Ko pendant des jours (reliquat d'IRAM, constant à travers les
// redémarrages) pendant que des allocations de 92 o échouaient réellement —
// la famine 8 bits était réelle, l'indicateur ne la voyait pas.
//
// Les fonctions ci-dessous mesurent le pool `MALLOC_CAP_INTERNAL |
// MALLOC_CAP_8BIT`, c'est-à-dire exactement ce qu'un `malloc()`/`new` peut
// obtenir. Les variantes « interne total » sont conservées pour rendre l'écart
// entre les deux familles visible dans le diagnostic.
//
// CONTRAINTES D'IMPLÉMENTATION (à préserver) :
//  - aucun log, aucune allocation, aucun `String` → appelable depuis
//    `WsManager::_heapAllowsPush()`, qui ne doit JAMAIS journaliser (récursion :
//    systemLogger → broadcastLog → _heapAllowsPush → systemLogger…) ;
//  - `heap_caps_*` s'exécute interruptions coupées : appels rares et groupés
//    (un instantané plutôt que quatre appels dispersés).
//
// INTERDIT ABSOLU : ne JAMAIS appeler ces fonctions depuis `oomFailedAllocHook`
// ni depuis le new_handler (`oom_guard.cpp`). Quand ces chemins s'exécutent, le
// verrou du composant heap est DÉJÀ détenu par l'allocateur : toute mesure y
// serait un appel réentrant, donc un deadlock. Ces chemins lisent exclusivement
// le miroir atomique alimenté par `oomGuardNoteHeapSample()`.

/**
 * @brief Plus gros bloc contigu réellement allouable par malloc/new (octets).
 *
 * Pool `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` : exclut l'IRAM 32 bits, qui
 * gonfle artificiellement `ESP.getMaxAllocHeap()`.
 * Sans log ni allocation — utilisable depuis les chemins critiques (hors
 * chemin d'échec d'allocation, cf. interdit ci-dessus).
 */
uint32_t heapLargestFreeBlock8Bit();

/**
 * @brief Mémoire libre totale réellement allouable par malloc/new (octets).
 *
 * Pool `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. Sans log ni allocation.
 */
uint32_t heapFree8Bit();

/**
 * @brief Instantané des deux familles de métriques mémoire.
 *
 * `free8`/`maxAlloc8` = mémoire réellement allouable (8 bits) ;
 * `freeInternal`/`maxAllocInternal` = pool interne TOTAL (IRAM 32 bits
 * comprise), tel que le rapportent `ESP.getFreeHeap()`/`ESP.getMaxAllocHeap()`.
 * L'écart entre les deux familles est la mesure du piège décrit en tête de
 * fichier.
 */
struct HeapMetrics {
  uint32_t free8;             // Libre allouable (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
  uint32_t maxAlloc8;         // Plus gros bloc allouable (idem)
  uint32_t freeInternal;      // Libre pool interne total (IRAM 32 bits comprise)
  uint32_t maxAllocInternal;  // Plus gros bloc pool interne total
};

/**
 * @brief Prend les quatre mesures en une fois (sans log ni allocation).
 * @return Instantané cohérent à l'échelle de l'appel.
 */
HeapMetrics heapMetricsSnapshot();

// ---------------------------------------------------------------------------
// Suivi des CREUX (revue v2.19.0) — c'est la donnée qui calibrera les seuils
// ---------------------------------------------------------------------------
// Les seuils de 2.19.1 se déduiront du PIRE creux observé, pas d'un instantané.
// Or les creux 8 bits durent quelques SECONDES (l'anneau montre des oscillations
// de ~45 Ko entre deux échantillons de 2 s) : les échantillonner à la cadence de
// la trace (5 min) ou du contrôle de santé (60 s) les raterait presque tous et
// donnerait un minimum trop optimiste — donc un seuil d'auto-guérison trop haut,
// qui déclencherait des redémarrages en régime normal (et, au bout de trois,
// suspendrait le dosage). Le suivi est donc alimenté par heapRingSample(), à la
// cadence de 2 s, avec les valeurs DÉJÀ mesurées (coût nul, aucune mesure
// supplémentaire interruptions coupées).

/**
 * @brief Enregistre un échantillon pour le suivi des creux (appelé par
 *        heapRingSample(), cadence 2 s, avec une valeur déjà mesurée).
 * Sans log ni allocation.
 */
void heapNoteMaxAlloc8Sample(uint32_t maxAlloc8);

/**
 * @brief Plus petit « plus gros bloc » 8 bits observé DEPUIS LE BOOT (octets).
 * UINT32_MAX si aucun échantillon. Sature après le premier creux profond :
 * utile pour le pire cas absolu, insuffisant pour suivre une évolution.
 */
uint32_t heapMinMaxAlloc8SinceBoot();

/**
 * @brief Plus petit « plus gros bloc » 8 bits observé DEPUIS LE DERNIER APPEL,
 *        puis remet la fenêtre à zéro (octets). UINT32_MAX si aucun échantillon.
 * Complète la valeur depuis-boot : permet de voir si les creux s'aggravent
 * d'une fenêtre de trace à l'autre.
 */
uint32_t heapTakeMinMaxAlloc8Window();

#endif  // HEAP_METRICS_H
