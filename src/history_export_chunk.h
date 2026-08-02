#ifndef HISTORY_EXPORT_CHUNK_H
#define HISTORY_EXPORT_CHUNK_H

// =============================================================================
// history_export_chunk — Logique PURE de l'export chunked /get-history
// =============================================================================
// Bug-heap-toctou (A3) : /get-history construisait la réponse entière dans une
// String contiguë (~40 Ko épinglés à chaque ouverture du dashboard) — générateur
// de fond de la fragmentation heap. La route passe en beginChunkedResponse
// (patron /download-logs, ADR-0032 mécanisme b) : ce module fournit la logique
// pure testable en natif (style ADR-0017) :
//   - historyRangeCutoff : borne temporelle du filtre ?range= (sémantique EXACTE
//     de HistoryManager::getLastHours — 0 = tout renvoyer) ;
//   - formatHistoryPointJson : un point au format JSON EXACT de l'ancienne
//     concaténation String (mêmes champs, mêmes arrondis, virgule de séparation).
// Le report entre deux chunks réutilise LogLineCarry<N>/emitCarry de
// logs_export_chunk.h TELS QUELS (l'appelant firmware instancie
// LogLineCarry<kHistoryPointJsonMaxBytes>).
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stddef.h>/<stdint.h>/
// <stdio.h>/<math.h> uniquement, fonctions inline (header-only, rien à ajouter
// au build_src_filter natif).
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// Borne temporelle basse du filtre ?range= (epoch, secondes). Sémantique EXACTE
// de HistoryManager::getLastHours :
//   - hours <= 0 (mode "all")            → 0 = tout renvoyer ;
//   - nowEpoch == 0 (heure indisponible) → 0 = tout renvoyer ;
//   - nowEpoch < hours*3600 (underflow)  → 0 = tout renvoyer ;
//   - sinon nowEpoch - hours*3600.
// 3600UL = secondes par heure (miroir de kSecondsPerHour, non incluse ici pour
// garder le header pur/autonome — fait physique, pas un réglage).
inline unsigned long historyRangeCutoff(int hours, unsigned long nowEpoch) {
  if (hours <= 0 || nowEpoch == 0) return 0;
  unsigned long rangeSeconds = (unsigned long)hours * 3600UL;
  if (nowEpoch < rangeSeconds) return 0;
  return nowEpoch - rangeSeconds;
}

// Formate UN point d'historique au format JSON EXACT de l'ancienne route
// (web_routes_data.cpp, concaténation String) :
//   {"timestamp":T,"ph":7.2,"orp":650,"temperature":24.6,
//    "filtration":true,"dosing":false,"granularity":0}
// - ph / temperature : arrondi au dixième puis 1 décimale (String(x,1) ≡ %.1f) ;
// - orp : entier arrondi ;
// - valeur non finie (NaN/inf) → null ;
// - first == false → virgule de séparation en tête.
// Retour : longueur écrite dans `out` (hors NUL), ou 0 si le point formaté ne
// tient pas dans `cap` — JAMAIS de JSON partiel émis (point sauté par l'appelant).
inline size_t formatHistoryPointJson(char* out, size_t cap, unsigned long ts,
                                     float ph, float orp, float temp,
                                     bool filtration, bool dosing,
                                     uint8_t granularity, bool first) {
  if (out == nullptr || cap == 0) return 0;

  char phBuf[16];
  char orpBuf[16];
  char tempBuf[16];
  if (isfinite(ph)) {
    snprintf(phBuf, sizeof(phBuf), "%.1f", (double)(roundf(ph * 10.0f) / 10.0f));
  } else {
    snprintf(phBuf, sizeof(phBuf), "null");
  }
  if (isfinite(orp)) {
    snprintf(orpBuf, sizeof(orpBuf), "%d", (int)roundf(orp));
  } else {
    snprintf(orpBuf, sizeof(orpBuf), "null");
  }
  if (isfinite(temp)) {
    snprintf(tempBuf, sizeof(tempBuf), "%.1f", (double)(roundf(temp * 10.0f) / 10.0f));
  } else {
    snprintf(tempBuf, sizeof(tempBuf), "null");
  }

  int n = snprintf(out, cap,
                   "%s{\"timestamp\":%lu,\"ph\":%s,\"orp\":%s,\"temperature\":%s,"
                   "\"filtration\":%s,\"dosing\":%s,\"granularity\":%u}",
                   first ? "" : ",", ts, phBuf, orpBuf, tempBuf,
                   filtration ? "true" : "false", dosing ? "true" : "false",
                   (unsigned)granularity);
  if (n < 0 || (size_t)n >= cap) return 0;  // ne tient pas : rien d'émis
  return (size_t)n;
}

#endif  // HISTORY_EXPORT_CHUNK_H
