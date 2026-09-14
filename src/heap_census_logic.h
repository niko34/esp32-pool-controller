#ifndef HEAP_CENSUS_LOGIC_H
#define HEAP_CENSUS_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#include "constants.h"  // kHeapCensus*, kOomProbeBlockCountUnknown (ADR-0072) — auto-suffisant, sans Arduino

// =============================================================================
// heap_census_logic — module PUR du recensement du tas (feature-128, R2/R3)
// =============================================================================
// ADR-0017, idiome de heap_caps_logic.h : aucune dépendance Arduino / FreeRTOS /
// FS / esp_heap_caps ici. HEADER-ONLY, DÉLIBÉRÉMENT : `build_src_filter` de
// `env:native` (platformio.ini) ne liste que des .cpp — un fichier header-only
// n'a donc RIEN à y ajouter, `platformio.ini` reste intact (invariant I11). Les
// tests natifs incluent ce header directement.
//
// RÉPARTITION DU TRAVAIL AVEC LE CALLBACK IMPUR (heap_census.cpp) :
// Le callback `heap_caps_walk` s'exécute interruptions masquées (spinlock
// INTER-CŒURS, cf. heap_census.h en tête) : il ne doit faire QUE des lectures
// mémoire minimales et des comparaisons entières. Ce fichier fournit cette
// arithmétique en fonctions `inline` (candidates à l'inlining réel par le
// compilateur, contrairement à un appel de fonction .cpp séparée) ; le
// callback ne fait JAMAIS lui-même le tri complet — il délègue à
// `heapCensusTopNInsert`, qui reste bornée par `capacity` (24 au pire).
// =============================================================================

// ---------------------------------------------------------------------------
// Blocs — les structures que publient le callback ET les routes
// ---------------------------------------------------------------------------
// S8 : AUCUN champ d'octets de contenu ici — adresse et taille(s) seulement.

/** @brief Un bloc LIBRE retenu dans le top-N approché. */
struct HeapCensusFreeEntry {
  uint32_t addr;  // walker_block_info_t.ptr, brut (avant tout header poison)
  uint32_t size;  // walker_block_info_t.size (taille TLSF du bloc, header compris)
};

/** @brief Un bloc OCCUPÉ retenu car son adresse tombe dans la fenêtre demandée. */
struct HeapCensusUsedEntry {
  uint32_t addr;       // walker_block_info_t.ptr, brut
  uint32_t size;       // walker_block_info_t.size (taille TLSF, header poison compris)
  uint32_t allocSize;  // poison_head_t.alloc_size (octets réellement demandés),
                       // ou kHeapCensusAllocSizeUnknown si canari non conforme (ADR-0072)
};

// ---------------------------------------------------------------------------
// Top-N approché des blocs LIBRES (S8 : aucun contenu, adresses/tailles seules)
// ---------------------------------------------------------------------------
// Le callback NE TRIE PAS pendant le parcours : il rejette en DEUX
// INSTRUCTIONS (comparaison + `return`) tout candidat sous le seuil `floor`
// courant (voir heap_census.h), et n'appelle l'insertion ci-dessous QUE pour un
// candidat qui a franchi ce seuil — un événement rare une fois le tampon
// stabilisé sur les plus gros trous. Le tri qui en résulte est donc GROSSIER
// (approché), pas un top-N exact instant par instant : suffisant pour un
// diagnostic, et c'est un choix délibéré pour ne jamais faire de tri complet
// sous verrou.

/**
 * @brief Seuil de rejet courant (« floor ») du top-N de blocs libres.
 *
 * Tampon pas encore plein (`count < capacity`) : tout est accepté, floor = 0.
 * Tampon plein : floor = la plus PETITE taille actuellement gardée — tout
 * candidat de taille inférieure ou égale ne peut pas améliorer le top-N et est
 * rejeté sans y regarder de plus près.
 *
 * @param entries  Tampon courant (peut être nullptr si capacity == 0).
 * @param count    Nombre d'entrées valides dans `entries` (<= capacity).
 * @param capacity Capacité du tampon (kHeapCensusMaxFreeBlocks côté firmware).
 * @return Le seuil de rejet, en octets.
 */
inline uint32_t heapCensusKeptFloor(const HeapCensusFreeEntry* entries, uint8_t count,
                                     uint8_t capacity) {
  if (entries == nullptr || capacity == 0u) return 0xFFFFFFFFu;  // rien ne peut être gardé
  if (count < capacity) return 0u;  // tampon pas plein : tout candidat est accepté
  uint32_t floor = entries[0].size;
  for (uint8_t i = 1; i < count; i++) {
    if (entries[i].size < floor) floor = entries[i].size;
  }
  return floor;
}

/**
 * @brief Insertion grossière d'un candidat dans le top-N de blocs libres.
 *
 * Tampon pas plein : ajoute en fin de tampon (ordre non trié). Tampon plein :
 * remplace l'entrée actuellement la plus petite SI le candidat est
 * strictement plus grand (sinon rejet). Coût borné par `capacity`
 * (kHeapCensusMaxFreeBlocks = 24) : un balayage linéaire est trivial à cette
 * échelle, y compris interruptions masquées.
 *
 * @param entries  Tampon à modifier (in/out).
 * @param count    Nombre d'entrées valides (in/out).
 * @param capacity Capacité du tampon.
 * @param addr     Adresse du bloc candidat.
 * @param size     Taille du bloc candidat.
 * @return true si le tampon a été modifié (candidat retenu).
 */
inline bool heapCensusTopNInsert(HeapCensusFreeEntry* entries, uint8_t* count, uint8_t capacity,
                                  uint32_t addr, uint32_t size) {
  if (entries == nullptr || count == nullptr || capacity == 0u) return false;
  if (*count < capacity) {
    entries[*count] = HeapCensusFreeEntry{addr, size};
    (*count)++;
    return true;
  }
  uint8_t minIdx = 0;
  uint32_t minVal = entries[0].size;
  for (uint8_t i = 1; i < capacity; i++) {
    if (entries[i].size < minVal) {
      minVal = entries[i].size;
      minIdx = i;
    }
  }
  if (size <= minVal) return false;
  entries[minIdx] = HeapCensusFreeEntry{addr, size};
  return true;
}

// ---------------------------------------------------------------------------
// Fenêtre d'adresses des blocs OCCUPÉS (R2, comportement attendu de la spec)
// ---------------------------------------------------------------------------

/**
 * @brief Une adresse tombe-t-elle dans la fenêtre [lo, hi) demandée ?
 *
 * ⚠️ `hi == 0` ⇒ fenêtre VIDE, JAMAIS « tout le tas » — une requête sans borne
 * haute explicite ne doit RIEN faire remonter plutôt que de tout parcourir
 * silencieusement (comportement de garde, pas un raccourci pratique).
 *
 * @param addr Adresse du bloc (walker_block_info_t.ptr).
 * @param lo   Première adresse incluse.
 * @param hi   Première adresse EXCLUE (0 = fenêtre vide par convention).
 */
inline bool heapCensusInWindow(uint32_t addr, uint32_t lo, uint32_t hi) {
  if (hi == 0u) return false;
  return (addr >= lo) && (addr < hi);
}

// ---------------------------------------------------------------------------
// Concordance avec les planchers déjà échantillonnés (A2, I2)
// ---------------------------------------------------------------------------
// I2 : aucun heap_caps_get_info() neuf sur le chemin de la sonde — la
// comparaison se fait contre `HeapFloorEntry::lastFreeBlocks` /
// `lastAllocatedBlocks` (heap_metrics.h), déjà mesurés par heapFloorsSample()
// et lus SANS parcours par heapFloorsRead().

/** @brief Verdict de heapCensusCompare(). */
enum HeapCensusVerdict : uint8_t {
  kHeapCensusVerdictMatch = 0,         // Les compteurs concordent
  kHeapCensusVerdictMismatch = 1,      // Les compteurs DIVERGENT (les deux sont mesurés)
  kHeapCensusVerdictUndetermined = 2,  // L'un des deux porte la sentinelle ADR-0072 : rien à comparer
};

/**
 * @brief Compare les agrégats du recensement à ceux du plancher 0x0804.
 *
 * ADR-0072 : si `floorFreeBlocks` ou `floorUsedBlocks` porte la sentinelle
 * « non renseigné » (kOomProbeBlockCountUnknown — masque à chemin O(régions),
 * ou plancher jamais échantillonné), le verdict est INDÉTERMINÉ — jamais un
 * faux « mismatch » ni un faux « match ».
 *
 * @param censusFreeBlocks Nombre de blocs libres vus par CE recensement.
 * @param censusUsedBlocks Nombre de blocs occupés vus par CE recensement.
 * @param floorFreeBlocks  HeapFloorEntry::lastFreeBlocks du masque 0x0804.
 * @param floorUsedBlocks  HeapFloorEntry::lastAllocatedBlocks du masque 0x0804.
 */
inline uint8_t heapCensusCompare(uint32_t censusFreeBlocks, uint32_t censusUsedBlocks,
                                  uint32_t floorFreeBlocks, uint32_t floorUsedBlocks) {
  if (floorFreeBlocks == kOomProbeBlockCountUnknown ||
      floorUsedBlocks == kOomProbeBlockCountUnknown) {
    return (uint8_t)kHeapCensusVerdictUndetermined;
  }
  const bool match =
      (censusFreeBlocks == floorFreeBlocks) && (censusUsedBlocks == floorUsedBlocks);
  return (uint8_t)(match ? kHeapCensusVerdictMatch : kHeapCensusVerdictMismatch);
}

// ---------------------------------------------------------------------------
// Plancher temporel entre deux parcours (S9, PARTAGÉ recensement + lecture)
// ---------------------------------------------------------------------------

/**
 * @brief Le plancher `kHeapCensusMinIntervalMs` est-il respecté ?
 *
 * S9 : cette fonction est appelée DU CÔTÉ QUI CONSOMME REQUESTED->READY (la
 * loop, heap_census.cpp), sur un horodatage ÉCRIT PAR LA LOOP — pas
 * uniquement dans la route. Mutation attendue : `minIntervalMs = 0` rend cette
 * fonction toujours vraie, donc un test de la machine à états qui vérifie
 * l'espacement de deux parcours doit alors échouer.
 *
 * Arithmétique NON SIGNÉE : correcte au repliement de millis() (idiome du
 * projet, cf. heapCtxDiscoveryWindowActive). `everWalked = false` = aucun
 * parcours depuis le boot -> toujours admis (pas de plancher à respecter
 * contre une référence qui n'existe pas).
 */
inline bool heapCensusMinIntervalElapsed(uint32_t nowMs, uint32_t lastWalkMs, bool everWalked,
                                          uint32_t minIntervalMs) {
  if (!everWalked) return true;
  return (uint32_t)(nowMs - lastWalkMs) >= minIntervalMs;
}

/**
 * @brief L'instantané READY est-il périmé (kHeapCensusStaleMs) ?
 * Arithmétique non signée, même idiome que ci-dessus.
 */
inline bool heapCensusIsStale(uint32_t nowMs, uint32_t tookAtMs, uint32_t staleMs) {
  return (uint32_t)(nowMs - tookAtMs) >= staleMs;
}

// ---------------------------------------------------------------------------
// R3 — planification PUREMENT ARITHMÉTIQUE de la lecture (S4)
// ---------------------------------------------------------------------------
// Ce module ne déréférence JAMAIS un pointeur de tas : les valeurs (used,
// blockSize, allocSize) sont déjà LUES par le callback impur (heap_census.cpp)
// avant l'appel. Ici, uniquement de l'arithmétique bornée.

/** @brief Verdict + longueur de heapCensusPlanPeek(). */
struct HeapCensusPeekPlan {
  bool ok;          // false : bloc non éligible, rien à copier
  uint32_t copyLen;  // Longueur à copier (0 si !ok)
};

/**
 * @brief Calcule la longueur à copier pour R3, sans toucher la mémoire.
 *
 * S4 : longueur clampée à min(requestedLen, kHeapPeekMaxBytes, allocSize,
 * payloadRoom) — `allocSize` est la taille RÉELLEMENT DEMANDÉE du bloc
 * DÉSIGNÉ (poison_head_t, pas la taille TLSF arrondie), condition explicite
 * du gate. ⚠️ `payloadRoom = blockSize - poisonHeadBytes` est une borne
 * STRUCTURELLE, INDÉPENDANTE de tout contenu lu dans l'en-tête du bloc
 * examiné (revue code-reviewer/B1) : `allocSize` vit DANS le bloc dont
 * l'intégrité est précisément l'objet de l'enquête (fragmentation, corruption
 * suspectée). Un canari intact ne garantit PAS que `alloc_size` est sain —
 * seul TLSF (via `blockSize`, mesuré par le parcours lui-même, pas lu en
 * mémoire) donne une borne dont l'intégrité ne dépend pas du bloc examiné.
 * Sans `payloadRoom`, un `alloc_size` corrompu (canari intact, valeur folle)
 * ferait lire au-delà du bloc — potentiellement jusqu'à `LoadProhibited` sous
 * le spinlock inter-cœurs, interruptions masquées : panic, reboot, fenêtre
 * d'observation perdue.
 *
 * @param used        Le bloc désigné par l'adresse est-il occupé ?
 * @param blockSize   Taille TLSF du bloc (walker_block_info_t.size) — mesurée
 *        par le parcours, jamais lue dans l'en-tête du bloc.
 * @param allocSize   poison_head_t.alloc_size du bloc, DÉJÀ validé par le
 *        canari — mais son intégrité N'EST PAS garantie par le canari seul
 *        (borne `payloadRoom` ci-dessous, pas une confiance aveugle).
 * @param requestedLen Longueur demandée par le client.
 * @param peekMaxBytes kHeapPeekMaxBytes.
 * @param poisonHeadBytes kHeapPoisonHeadBytes — un bloc plus petit que l'en-tête
 *        poison ne peut structurellement pas porter de payload utilisateur.
 */
inline HeapCensusPeekPlan heapCensusPlanPeek(bool used, uint32_t blockSize, uint32_t allocSize,
                                              uint32_t requestedLen, uint32_t peekMaxBytes,
                                              uint32_t poisonHeadBytes) {
  HeapCensusPeekPlan plan{false, 0u};
  if (!used) return plan;
  if (blockSize < poisonHeadBytes) return plan;
  // ADR-0072 : rejet explicite de la sentinelle « non renseigné » — redondant
  // avec `allocSize == 0u` ci-dessous une fois `payloadRoom` posé (aucun
  // `alloc_size` sain ne vaut 0xFFFFFFFF), mais à coût nul et ça documente
  // l'intention sur un chemin qui lira de la mémoire brute.
  if (allocSize == kHeapCensusAllocSizeUnknown) return plan;
  if (allocSize == 0u) return plan;
  const uint32_t payloadRoom = blockSize - poisonHeadBytes;
  if (payloadRoom == 0u) return plan;
  uint32_t len = requestedLen;
  if (len > peekMaxBytes) len = peekMaxBytes;
  if (len > allocSize) len = allocSize;
  if (len > payloadRoom) len = payloadRoom;  // borne STRUCTURELLE, indépendante de l'en-tête lu
  plan.ok = true;
  plan.copyLen = len;
  return plan;
}

/**
 * @brief Un octet de contenu en représentation ASCII imprimable, ou '.'.
 * Plage [0x20, 0x7F) — imprimable 7 bits, convention terminal usuelle.
 */
inline char heapPeekAsciiByte(uint8_t b) {
  return (b >= 0x20u && b < 0x7Fu) ? (char)b : '.';
}

#endif  // HEAP_CENSUS_LOGIC_H
