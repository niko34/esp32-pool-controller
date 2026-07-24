#ifndef LOGS_EXPORT_CHUNK_H
#define LOGS_EXPORT_CHUNK_H

// =============================================================================
// logs_export_chunk — Report de ligne PUR pour l'export chunked /download-logs
// =============================================================================
// Bug-crash-getlogs-badalloc : /download-logs construisait toute la réponse
// (fichier persistant + 200 entrées RAM) dans le cbuf contigu d'un
// AsyncResponseStream → std::bad_alloc → abort() sur heap fragmenté. La route
// passe en beginChunkedResponse : le callback remplit un buffer TCP borné, et
// une ligne formatée peut être plus longue que la place restante dans le
// buffer → ce module gère le report (« carry ») du reliquat de ligne entre
// deux appels du callback. Extrait en header pur pour être testable en natif
// (style ADR-0017, cf. sensor_trace_page.h).
//
// La capacité de ligne est un paramètre template (N) : l'appelant firmware
// instancie LogLineCarry<kLogExportLineMaxBytes> (constants.h) — aucune
// duplication de constante, et le header reste pur/autonome (testable en
// natif avec un N arbitraire, sans Arduino).
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stddef.h>/<stdint.h>
// uniquement, fonctions inline (header-only, rien à ajouter au build natif).
// =============================================================================

#include <stddef.h>
#include <stdint.h>

// Ligne en cours d'émission : `line[0..len)` = contenu formaté, `pos` = nombre
// d'octets déjà émis. Carry vide/épuisé quand pos == len.
template <size_t N>
struct LogLineCarry {
  char line[N];
  size_t len = 0;
  size_t pos = 0;
};

// Émet dans `buf` (capacité `maxLen`) ce qui reste de la ligne en cours ;
// retourne le nombre d'octets écrits ; `c.pos` avance d'autant.
// GARANTIE de progression du flux chunked : ne retourne jamais 0 si du carry
// reste (pos < len) ET maxLen > 0 — sinon AsyncWebServer interpréterait le 0
// comme la fin de la réponse.
template <size_t N>
inline size_t emitCarry(LogLineCarry<N>& c, uint8_t* buf, size_t maxLen) {
  if (c.pos >= c.len || maxLen == 0) return 0;
  size_t remaining = c.len - c.pos;
  size_t n = (remaining < maxLen) ? remaining : maxLen;
  for (size_t i = 0; i < n; i++) {
    buf[i] = static_cast<uint8_t>(c.line[c.pos + i]);
  }
  c.pos += n;
  return n;
}

#endif  // LOGS_EXPORT_CHUNK_H
