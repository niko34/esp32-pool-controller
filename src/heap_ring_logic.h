#ifndef HEAP_RING_LOGIC_H
#define HEAP_RING_LOGIC_H

// =============================================================================
// heap_ring_logic — Encodage PUR des entrées du ring heap RTC noinit
// =============================================================================
// Bug-heap-toctou (A4) : le tick santé (5-60 s) rate la cinétique d'effondrement
// du heap (quelques secondes). Un ring de kHeapRingEntries × 8 octets en RAM RTC
// noinit (survit à ESP.restart/panic, pas au power-on) échantillonne toutes les
// 2 s ; au boot post-crash, les dernières entrées sont dumpées dans le journal.
// Ce module contient la logique pure testable en natif (ADR-0017) :
//   - encodage/décodage d'une entrée 8 octets (little-endian, quantifiée) ;
//   - arithmétique d'index du ring (tête, itération des N dernières entrées) ;
//   - âge d'un échantillon (arithmétique modulaire u16).
// La coquille impure (RTC_NOINIT_ATTR, ESP.get*Heap, systemLogger) vit dans
// heap_ring.cpp.
//
// FORMAT D'UNE ENTRÉE (8 octets, little-endian) :
//   [0..1] u16 uptimeS        — uptime en secondes, modulo 65536 (~18,2 h)
//   [2..3] u16 maxAlloc >> 6  — plus gros bloc contigu, quantum 64 o (plafond ~4,19 Mo)
//   [4..5] u16 freeHeap >> 8  — heap libre total, quantum 256 o (plafond ~16,7 Mo)
//   [6]    u8  wsClients      — clients WS connectés (saturé à 255)
//   [7]    u8  skippedDelta   — pushes WS sautés depuis l'échantillon précédent (saturé à 255)
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stddef.h>/<stdint.h>
// uniquement, fonctions inline (header-only, rien à ajouter au build natif).
// =============================================================================

#include <stddef.h>
#include <stdint.h>

constexpr size_t kHeapRingEntryBytes = 8;

// Champs décodés d'une entrée (valeurs reconstruites : perte = quantum d'encodage).
struct HeapRingSampleFields {
  uint16_t uptimeS;         // Uptime (s) modulo 65536 à l'échantillonnage
  uint32_t maxAllocBytes;   // Plus gros bloc contigu (arrondi au quantum 64 o inférieur)
  uint32_t freeHeapBytes;   // Heap libre total (arrondi au quantum 256 o inférieur)
  uint8_t  wsClients;       // Clients WS (saturé 255)
  uint8_t  skippedDelta;    // Pushes sautés depuis l'échantillon précédent (saturé 255)
};

// Encode une entrée dans `out` (kHeapRingEntryBytes octets). Saturations :
// maxAlloc/freeHeap plafonnés à la dynamique u16 post-quantum, wsClients et
// skippedDelta à 255, uptimeS tronqué modulo 65536.
inline void heapRingEncode(uint8_t* out, uint32_t uptimeS, uint32_t maxAllocBytes,
                           uint32_t freeHeapBytes, uint32_t wsClients,
                           uint32_t skippedDelta) {
  uint16_t up = (uint16_t)(uptimeS & 0xFFFFu);
  uint32_t ma = maxAllocBytes >> 6;
  if (ma > 0xFFFFu) ma = 0xFFFFu;
  uint32_t fh = freeHeapBytes >> 8;
  if (fh > 0xFFFFu) fh = 0xFFFFu;
  uint8_t ws = (wsClients > 0xFFu) ? 0xFFu : (uint8_t)wsClients;
  uint8_t sk = (skippedDelta > 0xFFu) ? 0xFFu : (uint8_t)skippedDelta;
  out[0] = (uint8_t)(up & 0xFFu);
  out[1] = (uint8_t)(up >> 8);
  out[2] = (uint8_t)(ma & 0xFFu);
  out[3] = (uint8_t)((ma >> 8) & 0xFFu);
  out[4] = (uint8_t)(fh & 0xFFu);
  out[5] = (uint8_t)((fh >> 8) & 0xFFu);
  out[6] = ws;
  out[7] = sk;
}

// Décode une entrée depuis `in` (kHeapRingEntryBytes octets).
inline HeapRingSampleFields heapRingDecode(const uint8_t* in) {
  HeapRingSampleFields f;
  f.uptimeS = (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
  f.maxAllocBytes = ((uint32_t)(in[2] | ((uint32_t)in[3] << 8))) << 6;
  f.freeHeapBytes = ((uint32_t)(in[4] | ((uint32_t)in[5] << 8))) << 8;
  f.wsClients = in[6];
  f.skippedDelta = in[7];
  return f;
}

// Position de tête suivante après écriture à `head` (head = PROCHAINE position
// d'écriture, invariant du ring).
inline size_t heapRingNextHead(size_t head, size_t capacity) {
  return (head + 1 >= capacity) ? 0 : head + 1;
}

// Nombre d'entrées à dumper : min(remplissage, plafond demandé).
inline size_t heapRingDumpCount(size_t filled, size_t maxEntries) {
  return (filled < maxEntries) ? filled : maxEntries;
}

// Index absolu de la i-ème entrée (0 = la plus ANCIENNE) parmi les `n` dernières
// écrites. `head` = prochaine position d'écriture ; les n dernières entrées
// occupent [head-n .. head-1] modulo capacity. Préconditions : n <= capacity,
// i < n, head < capacity.
inline size_t heapRingIndexOfLast(size_t head, size_t n, size_t capacity, size_t i) {
  return (head + capacity - n + i) % capacity;
}

// Âge (s) d'un échantillon par rapport au plus récent — arithmétique modulaire
// u16 : correct même si uptimeS a bouclé entre les deux (fenêtre < 65536 s).
inline uint16_t heapRingAgeS(uint16_t newestUptimeS, uint16_t entryUptimeS) {
  return (uint16_t)(newestUptimeS - entryUptimeS);
}

#endif  // HEAP_RING_LOGIC_H
