#include <Arduino.h>
#include <esp_system.h>
#include "heap_ring.h"
#include "heap_ring_logic.h"
#include "constants.h"
#include "logger.h"
#include "ws_manager.h"

// =============================================================================
// Stockage RTC noinit (bug-heap-toctou A4)
// =============================================================================
// Magic DISTINCT de kAutoHealRtcMagic (0xA5C0FFEE, main.cpp) : les deux états
// RTC noinit sont indépendants (compteur de reboots vs ring d'instrumentation).
// Budget RTC noinit : 4 (magic) + 2 + 2 (tête/remplissage) + 1024 (ring) = 1032 o.
static constexpr uint32_t kHeapRingRtcMagic = 0xB10C4EA9u;

RTC_NOINIT_ATTR static uint32_t sHeapRingMagic;
RTC_NOINIT_ATTR static uint16_t sHeapRingHead;    // Prochaine position d'écriture
RTC_NOINIT_ATTR static uint16_t sHeapRingFilled;  // Nombre d'entrées valides
RTC_NOINIT_ATTR static uint8_t  sHeapRingData[kHeapRingEntries * kHeapRingEntryBytes];

// Libellé court du motif de reset pour l'en-tête du dump (les cas « crash » utiles).
static const char* heapRingResetLabel(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_EXT:       return "EXTERNAL";
    default:                return "UNKNOWN";
  }
}

void heapRingInit() {
  sHeapRingHead = 0;
  sHeapRingFilled = 0;
  sHeapRingMagic = kHeapRingRtcMagic;
}

void heapRingSample() {
  // Ré-armement au 1er échantillon : magic absent (power-on, ou invalidé par le
  // dump du boot courant) OU bornes insensées (RAM RTC corrompue) → repartir à vide.
  if (sHeapRingMagic != kHeapRingRtcMagic ||
      sHeapRingHead >= kHeapRingEntries || sHeapRingFilled > kHeapRingEntries) {
    heapRingInit();
  }

  // Delta de pushes WS sautés depuis l'échantillon précédent (compteur cumulé →
  // delta borné/saturé 255 à l'encodage). Statique locale : loopTask uniquement.
  static uint32_t sLastSkippedPushes = 0;
  uint32_t skipped = wsManager.skippedPushCount();
  uint32_t delta = skipped - sLastSkippedPushes;
  sLastSkippedPushes = skipped;

  heapRingEncode(&sHeapRingData[(size_t)sHeapRingHead * kHeapRingEntryBytes],
                 (uint32_t)(millis() / 1000UL),
                 ESP.getMaxAllocHeap(),
                 ESP.getFreeHeap(),
                 (uint32_t)wsManager.clientCount(),
                 delta);
  sHeapRingHead = (uint16_t)heapRingNextHead(sHeapRingHead, kHeapRingEntries);
  if (sHeapRingFilled < kHeapRingEntries) sHeapRingFilled++;
}

void heapRingDumpIfCrash() {
  if (sHeapRingMagic != kHeapRingRtcMagic) return;  // power-on ou 1er boot : rien

  // Le ring appartient au boot PRÉCÉDENT : invalidé quoi qu'il arrive (dump émis
  // ou non) — ré-armé au 1er heapRingSample() du boot courant.
  sHeapRingMagic = 0;

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON) return;  // reset propre secteur : pas de dump

  size_t head = sHeapRingHead;
  size_t filled = sHeapRingFilled;
  if (head >= kHeapRingEntries || filled > kHeapRingEntries || filled == 0) {
    return;  // RAM RTC incohérente malgré le magic : dump abandonné
  }

  size_t n = heapRingDumpCount(filled, kHeapRingDumpMaxEntries);

  // Référence temporelle : uptime du DERNIER échantillon avant le reset (t-0s).
  HeapRingSampleFields newest = heapRingDecode(
      &sHeapRingData[heapRingIndexOfLast(head, n, kHeapRingEntries, n - 1) *
                     kHeapRingEntryBytes]);

  // bug-oom-accept-http-durcissement (A7) : uptime ABSOLU du dernier
  // échantillon avant reset (modulo 65536 s ≈ 18,2 h — quantum d'encodage du
  // ring) + valeur brute esp_reset_reason — corrélation temporelle a posteriori
  // avec le journal persistant et les coredumps.
  systemLogger.warning("Cinétique heap avant reset (" +
                       String(heapRingResetLabel(reason)) + ", reset_reason=" +
                       String((int)reason) + ") : " + String(n) +
                       " échantillon(s), pas ~" +
                       String(kHeapRingSampleIntervalMs / 1000UL) +
                       " s (t-0s = dernier avant reset, uptime=" +
                       String(newest.uptimeS) + " s mod 65536)");

  // Format compact, 4 entrées par ligne (~8 lignes pour 32 entrées), de la plus
  // ancienne à la plus récente : « t-XXXs maxB=… free=… ws=… skip=… ».
  char line[224];
  size_t lineLen = 0;
  for (size_t i = 0; i < n; i++) {
    HeapRingSampleFields f = heapRingDecode(
        &sHeapRingData[heapRingIndexOfLast(head, n, kHeapRingEntries, i) *
                       kHeapRingEntryBytes]);
    int w = snprintf(line + lineLen, sizeof(line) - lineLen,
                     "%st-%us maxB=%u free=%u ws=%u skip=%u",
                     (lineLen > 0) ? " | " : "",
                     (unsigned)heapRingAgeS(newest.uptimeS, f.uptimeS),
                     (unsigned)f.maxAllocBytes, (unsigned)f.freeHeapBytes,
                     (unsigned)f.wsClients, (unsigned)f.skippedDelta);
    if (w > 0) {
      lineLen += ((size_t)w < sizeof(line) - lineLen) ? (size_t)w
                                                      : sizeof(line) - lineLen - 1;
    }
    if ((i + 1) % 4 == 0 || i + 1 == n) {
      systemLogger.info(String(line));
      lineLen = 0;
      line[0] = '\0';
    }
  }
}
