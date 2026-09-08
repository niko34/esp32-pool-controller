#ifndef HEAP_METRICS_H
#define HEAP_METRICS_H

#include <stdint.h>

#include "constants.h"        // kHeapFloorMaskCount (dimension du tableau de planchers)
#include "heap_caps_logic.h"  // masques, planchers et contextes — module PUR (ADR-0017)

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
// `ctxSurPlace` : bits kHeapCtx* que l'appelant connaît de façon CERTAINE parce
// qu'il est lui-même dans ce régime (0 s'il n'en sait rien). Ils sont fusionnés
// avec le contexte échantillonné, qui a jusqu'à 2 s de retard — retard qui, le
// 08/09/2026, a fait enregistrer un creux de 2 932 o avec « aucun régime ».
void heapNoteMaxAlloc8Sample(uint32_t maxAlloc8, uint8_t ctxSurPlace = 0);

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

/**
 * @brief Variante du site `async_tcp` : plancher 0x0804 + compteur de GET servis.
 *
 * Appelée depuis le FILTRE de la garde 503 (web_server.cpp), qui a DÉJÀ mesuré
 * `maxAlloc8` pour prendre sa décision : aucun parcours de tas neuf, un
 * `fetch_min` et un `fetch_add` relâchés. C'est ce qui donne au plancher 0x0804
 * une résolution à la REQUÊTE plutôt qu'à l'échantillon de 2 s — une rafale de
 * chargement d'interface dure moins qu'un intervalle d'échantillonnage, donc un
 * plancher purement périodique la raterait (cas limite « pointe non capturée »).
 *
 * Le compteur de GET alimente le bit de contexte kHeapCtxHttpBurst (A5).
 * Aucun log, aucune String, aucune allocation : contrat de `async_tcp`.
 */
void heapNoteHttpGetSample(uint32_t maxAlloc8);

/**
 * @brief Nombre cumulé d'évaluations du filtre de la garde 503 (≈ GET servis).
 * Monotone, lu par la loop pour en dériver un delta par échantillon.
 */
uint32_t heapHttpGetCount();

// ===========================================================================
// PLANCHERS GLISSANTS MULTI-MASQUES (feature-122, lot C-1a) — MESURE SEULE
// ===========================================================================
// Cinq emplacements : les quatre masques de la dérivation (0x0804, 0x080C,
// 0x1000, 0x1804) et le tas PSRAM. Chacun garde son minimum DEPUIS LE BOOT (avec
// le contexte qui l'a produit) et sur FENÊTRE (réarmée après chaque report).
//
// ⚠️ AUCUNE GARDE NE LIT CES PLANCHERS. Le refus 503, la garde d'ouverture de
// fichiers et le parachute OOM restent sur 0x0804 / kStaticFileMinAllocBytes.
// Basculer une garde sur 0x1804 appartient à feature-123, avec son ADR.
//
// COÛT : voir kHeapFloorWalksPerSample — 2 parcours O(blocs) par échantillon,
// soit 1/s, le plafond du gate pool-chemistry. Surcoût NET par rapport à l'état
// antérieur : +1 parcours et +1 agrégat toutes les 2 s (les deux appels 0x0804
// de heap_ring sont désormais fondus en un seul heap_caps_get_info).

/** Ordre des emplacements — c'est aussi celui des bits de `HeapFloors::pred`. */
constexpr uint8_t kHeapFloorSlotInternal8 = 0;         // 0x0804 (référence des gardes)
constexpr uint8_t kHeapFloorSlotInternalDma8 = 1;      // 0x080C (classe d'échec observée)
constexpr uint8_t kHeapFloorSlotDefault = 2;           // 0x1000 (voit la PSRAM)
constexpr uint8_t kHeapFloorSlotInternal8Default = 3;  // 0x1804 (candidate C-1b)
constexpr uint8_t kHeapFloorSlotSpiram = 4;            // SPIRAM (A6)

/**
 * @brief État publié d'un masque : dernier échantillon + planchers.
 *
 * `lastLargest` / `minLargest*` valent `kOomProbeBlockUnknown` pour les masques
 * à chemin O(régions) (0x1000 et SPIRAM) : leur « plus gros bloc » n'est PAS
 * mesuré et ne doit JAMAIS être affiché comme un bloc réel.
 * `kHeapFloorEmpty` = aucun échantillon pour ce plancher.
 */
struct HeapFloorEntry {
  uint32_t mask;
  uint32_t lastFree;
  uint32_t lastLargest;
  uint32_t minFreeSinceBoot;
  uint32_t minLargestSinceBoot;
  uint32_t minFreeWindow;
  uint32_t minLargestWindow;
  uint8_t ctxAtMinFree;     // bits kHeapCtx* au creux de « libre »
  uint8_t ctxAtMinLargest;  // bits kHeapCtx* au creux de « plus gros bloc »
  uint32_t samples;         // échantillons pris pour CE masque (tour de rôle)
};

/**
 * @brief Instantané complet des planchers, prêt à publier (A4).
 *
 * `pred` = les 12 bits de la DÉRIVATION `(all_caps & mask) == mask`, calculés par
 * heapMaskSelectsHeap() : bit (masque × 3 + tas), masques dans l'ordre des
 * `kHeapFloorSlot*` 0..3, tas dans l'ordre {DRAM interne, PSRAM, réserve DMA}.
 * C'est une PRÉDICTION arithmétique, pas une observation : aucune réserve n'est
 * posée par ce lot. Elle est publiée pour être confrontée aux planchers relevés.
 */
struct HeapFloors {
  HeapFloorEntry masks[kHeapFloorMaskCount];
  uint32_t sampleCount;  // appels à heapFloorsSample() depuis le boot
  // Condition 5 du gate pool-chemistry : DURÉE OBSERVÉE d'un parcours O(blocs)
  // de référence (0x0804), et non le seuil d'autolimitation d'une autre sonde.
  // ⚠️ NE PAS confondre avec `oom_probe_max_us` (chemin d'échec d'allocation) :
  // autre fréquence, autre état de tas, autre grandeur.
  // `walkMedianUs` est une médiane APPROCHÉE, rendue comme borne supérieure de
  // seau (surestimation bornée par la largeur du seau) — cf.
  // heapWalkMedianUpperUs. Jamais remises à zéro : la médiane gagne en sens avec
  // le nombre d'échantillons.
  uint32_t walkMaxUs;     // pire parcours 0x0804 depuis le boot
  uint32_t walkMedianUs;  // médiane approchée, kHeapWalkOverflowUs = hors échelle
  uint16_t pred;          // 12 bits de la table de sélection
  uint8_t ctxSeen;      // bits vus depuis le DERNIER réarmement de fenêtre (30 min)
  uint8_t ctxSeenBoot;  // bits vus depuis le DÉMARRAGE — jamais réarmé
  uint8_t witness;        // bits du témoin 4096/4095 (A7), cf. kHeapWitness*
};

/**
 * @brief Prend l'échantillon multi-masques et met à jour tous les planchers.
 *
 * À appeler UNIQUEMENT depuis la loop (heapRingSample, cadence
 * kHeapRingSampleIntervalMs) : écrivain unique du tableau de planchers.
 *
 * Mesure exactement kHeapFloorWalksPerSample masques à parcours O(blocs) — la
 * référence 0x0804 à chaque tour, puis UN masque en tour de rôle — plus les deux
 * masques à chemin O(régions). Sans log, sans allocation, sans verrou.
 *
 * @param ctx Bits kHeapCtx* observés par l'appelant (purement observationnels).
 * @return La sonde 0x0804 du tour, pour que l'appelant n'en refasse pas une
 *         (l'anneau, le miroir OOM et le plancher historique la réutilisent).
 */
HeapProbe heapFloorsSample(uint8_t ctx);

/**
 * @brief Lecture des planchers — SANS effet de bord, sans parcours de tas.
 *
 * Ne lit que des atomiques déjà rangées (condition 2 du gate pool-chemistry) :
 * appelable depuis un handler AsyncWebServer sans risque de quatre parcours de
 * tas interruptions coupées sur le chemin même de la famine.
 */
HeapFloors heapFloorsRead();

/**
 * @brief Réarme les planchers de FENÊTRE (les minima depuis-boot sont gardés).
 *
 * Appelée par la trace agrégée de checkHeapHealth() après report, donc depuis la
 * loop — même tâche que heapFloorsSample(). Course bénigne et documentée : un
 * échantillon pris entre la lecture et le réarmement est attribué à la fenêtre
 * SUIVANTE, jamais perdu pour le plancher depuis-boot.
 */
void heapFloorsRearmWindow();

// ---------------------------------------------------------------------------
// Témoin 4096/4095 (A7) — le lot B-1 est-il décoratif ?
// ---------------------------------------------------------------------------
// Deux `malloc()` au setup, libérés IMMÉDIATEMENT, l'un de 4096 o (attendu en
// PSRAM depuis kPsramExtmemThresholdBytes = 4095), l'autre de 4095 o (attendu
// interne dans les DEUX configurations = témoin de CONTRÔLE). Le couple donne
// une signature non ambiguë : si les deux tombent du même côté, la politique
// d'allocation n'a aucun effet.
constexpr uint8_t kHeapWitnessSkipped = 1u << 0;      // pas de PSRAM → témoin sauté
constexpr uint8_t kHeapWitness4096External = 1u << 1; // le 4096 o est tombé en RAM externe
constexpr uint8_t kHeapWitness4095External = 1u << 2; // le 4095 o est tombé en RAM externe (ANOMALIE)
constexpr uint8_t kHeapWitnessDisagreement = 1u << 3; // esp_ptr_external_ram ≠ esp_psram_check_ptr_addr
constexpr uint8_t kHeapWitness4096Failed = 1u << 4;   // malloc(4096) a échoué
constexpr uint8_t kHeapWitness4095Failed = 1u << 5;   // malloc(4095) a échoué
constexpr uint8_t kHeapWitnessNoPsramApi = 1u << 6;   // esp_psram_check_ptr_addr indisponible

/**
 * @brief Mémorise le résultat du témoin (aucun log : appelé AVANT
 *        systemLogger.begin(), idiome de main.cpp:127-131).
 */
void heapWitnessRecord(uint8_t bits);

/** @brief Bits du témoin 4096/4095 (0 = témoin jamais exécuté). */
uint8_t heapWitnessBits();

#endif  // HEAP_METRICS_H
