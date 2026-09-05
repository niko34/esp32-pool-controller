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
// RESTRICTION D'APPEL DEPUIS LES CHEMINS D'ÉCHEC — REQUALIFIÉE (bug-080 B-2)
// ---------------------------------------------------------------------------
// Ce fichier a longtemps parlé de « DEADLOCK ». C'était FACTUELLEMENT FAUX, et
// cette erreur de justification a bloqué pendant trois cycles l'instrumentation
// décisive du diagnostic. Désassemblage de `libheap.a` (revue B-1) :
//  - `heap_caps.c.obj` ne contient AUCUNE relocation vers
//    `multi_heap_internal_lock` / `_unlock` — le verrou du tas vit ENTIÈREMENT
//    dans `multi_heap.c.obj` ;
//  - `heap_caps_alloc_failed` (d'où part le callback) est statique en `.iram1.0`
//    et n'est atteinte qu'APRÈS LE RETOUR COMPLET de `heap_caps_malloc_base`.
// Aucun verrou de tas n'est donc détenu quand le callback s'exécute : mesurer
// depuis là n'est pas réentrant.
//
// L'interdit reste néanmoins valable, sous sa VRAIE qualification : c'est un
// COÛT INTERRUPTIONS COUPÉES, pas un risque de réentrance. Un parcours TLSF
// (`largest_free_block`) s'exécute interruptions coupées ; le chemin d'échec
// peut être déclenché par rafales de centaines par seconde ; et il est
// appelable depuis une ISR. Mesurer sans borne y transformerait un diagnostic
// en source de gigue pour la régulation et le watchdog.
//
// RÈGLE : depuis `oomFailedAllocHook` / le new_handler, seule `heapProbeForCaps()`
// est autorisée, et seulement via l'entonnoir borné d'`oom_guard.cpp` (jamais en
// ISR, un échantillon par épisode, plancher de `kOomProbeMinIntervalMs`, durée
// mesurée, désactivation définitive au-delà de `kOomProbeMaxDurationUs`).
// `heapLargestFreeBlock8Bit()`, `heapFree8Bit()` et `heapMetricsSnapshot()`
// restent INTERDITES dans ces chemins : elles ne sont bornées par rien.
// Pour tout le reste, ces chemins lisent le miroir atomique alimenté par
// `oomGuardNoteHeapSample()`.

/**
 * @brief Instantané du tas pour un masque de capacités donné (un seul parcours).
 *
 * `heap_caps_get_info()` livre `total_free_bytes` ET `largest_free_block` en UN
 * parcours, là où `heap_caps_get_free_size()` + `heap_caps_get_largest_free_block()`
 * en feraient deux.
 *
 * BORNE A PRIORI (condition 2 du gate pool-chemistry, bug-080 B-2) : si
 * `caps & MALLOC_CAP_SPIRAM`, le parcours O(blocs) est REMPLACÉ par les agrégats
 * O(régions) (`heap_caps_get_free_size`) et `largestBlockBytes` vaut
 * `kOomProbeBlockUnknown` (« non renseigné »). La sonde n'est JAMAIS sautée —
 * elle change d'algorithme — de sorte que le pire cas est borné AVANT coup, et
 * non seulement rattrapé après par l'autolimitation.
 *
 * Sans log, sans allocation, sans verrou pris par nous.
 */
struct HeapProbe {
  uint32_t freeBytes;          // Libre total pour ces caps
  uint32_t largestBlockBytes;  // Plus gros bloc contigu, ou kOomProbeBlockUnknown
};
HeapProbe heapProbeForCaps(uint32_t caps);

/**
 * @brief Masque des capacités « réellement allouable par malloc/new ».
 * `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` — la RÉFÉRENCE de la double mesure de
 * la sonde, à confronter aux caps (plus restrictives) de l'échec observé.
 */
uint32_t heapCaps8BitMask();

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
