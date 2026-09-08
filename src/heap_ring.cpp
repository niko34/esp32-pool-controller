#include <Arduino.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>  // xTaskGetTickCount — même horloge que le chemin d'échec OOM
#include "heap_ring.h"
#include "heap_ring_logic.h"
#include "constants.h"
#include "logger.h"
#include "ws_manager.h"
#include "oom_guard.h"  // oomGuardNoteHeapSample (miroir du dernier échantillon)
#include "heap_metrics.h"  // mesure 8 bits = mémoire réellement allouable (v2.19.0)
#include "heap_caps_logic.h"  // bits de contexte + fenêtre de rafale (PUR, ADR-0017)
#include "pump_controller.h"  // isAnyPumpEnergized (bit de contexte « dosage », LECTURE SEULE)
#include "mqtt_manager.h"     // lastDiscoveryMarkMs (bit de contexte « rafale HA »)

// =============================================================================
// Stockage RTC noinit (bug-heap-toctou A4)
// =============================================================================
// Magic DISTINCT de kAutoHealRtcMagic (0xA5C0FFEE, main.cpp) : les deux états
// RTC noinit sont indépendants (compteur de reboots vs ring d'instrumentation).
// Budget RTC noinit : 4 (magic) + 2 + 2 (tête/remplissage) + 1024 (ring) = 1032 o.
//
// MAGIC BUMPÉ EN v2.19.0 (0xB10C4EA9 → 0xB10C4EAA) : depuis cette version, les
// entrées de l'anneau contiennent la mesure 8 bits (mémoire réellement
// allouable) et non plus la mesure « interne totale » (IRAM 32 bits comprise).
// Une trame écrite par un firmware 2.18.x et relue ici serait affichée avec les
// nouveaux libellés (« maxB8= », « free8= ») alors qu'elle porte l'ANCIENNE
// métrique : un chiffre faux et crédible. Perdre la cinétique du boot de
// bascule est acceptable ; afficher une valeur trompeuse ne l'est pas.
static constexpr uint32_t kHeapRingRtcMagic = 0xB10C4EAAu;

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

  // Mesures faites UNE SEULE FOIS ici : elles alimentent l'anneau ET le miroir
  // du post-mortem OOM (bug-parachute-oom-jamais-rearme). La mesure s'exécute
  // interruptions coupées → on ne l'appelle pas plus souvent, et la cadence de
  // 2 s reste INCHANGÉE (accélérer l'échantillonnage injecterait de la gigue
  // dans le phénomène mesuré).
  //
  // v2.19.0 : source basculée sur la métrique 8 bits (heap_metrics.h) = la
  // mémoire qu'un malloc/new peut réellement obtenir. Purement OBSERVATIONNEL
  // ici — aucune décision de reboot ne dépend de l'anneau ni du miroir.
  //
  // feature-122 (A1/A5) : les DEUX appels 0x0804 d'avant
  // (heapLargestFreeBlock8Bit + heapFree8Bit) sont fondus en UN seul
  // heap_caps_get_info à l'intérieur de heapFloorsSample(), qui rend la sonde de
  // référence à l'appelant. Le créneau ainsi libéré porte UN masque
  // supplémentaire en tour de rôle.
  // COÛT RÉEL, ÉNONCÉ SANS ARRONDI (le « net nul » du plan ne tient que si l'on
  // suppose heapFree8Bit() coûteuse, ce qu'elle n'est pas) :
  //   avant  : 1 parcours O(blocs) + 1 agrégat O(régions) par échantillon ;
  //   après  : 2 parcours O(blocs) + 2 agrégats O(régions) par échantillon.
  // Soit +1 parcours toutes les 2 s → 1 parcours/s, EXACTEMENT le plafond fixé
  // par le gate pool-chemistry. Ne pas accélérer la cadence, ne pas ajouter un
  // 3ᵉ masque interne : les deux violeraient ce plafond (static_assert dans
  // constants.h et heap_metrics.cpp).
  const uint32_t nowMs = (uint32_t)millis();
  uint32_t uptimeS = nowMs / 1000UL;

  // ----- Bits de CONTEXTE (A5) — purement OBSERVATIONNELS -------------------
  // Sans eux, un plancher ne dit pas QUEL événement l'a produit. ⚠️ Aucun de ces
  // bits n'est relu par une décision (ni dosage, ni reboot, ni refus 503), et
  // aucune lecture de `filtration`/`sensors` n'est ajoutée ici.
  // « HTTP » : delta du compteur de GET servis depuis l'échantillon précédent —
  // même idiome que sLastSkippedPushes ci-dessus (statique locale, loopTask).
  static uint32_t sLastHttpGetCount = 0;
  const uint32_t httpGets = heapHttpGetCount();
  const uint32_t httpDelta = httpGets - sLastHttpGetCount;
  sLastHttpGetCount = httpGets;

  uint8_t ctx = 0;
  if (wsManager.clientCount() > 0) ctx |= kHeapCtxWsClients;
  if (httpDelta > 0) ctx |= kHeapCtxHttpBurst;
  // isAnyPumpEnergized() est inline, lit deux uint8_t, sans verrou (course
  // bénigne documentée dans pump_controller.h) — patron déjà employé à
  // main.cpp:394. LECTURE SEULE : aucun chemin de dosage n'est touché.
  if (PumpController.isAnyPumpEnergized()) ctx |= kHeapCtxDosing;
  if (heapCtxDiscoveryWindowActive(nowMs, mqttManager.lastDiscoveryMarkMs(),
                                   kMqttDiscoveryCtxWindowMs)) {
    ctx |= kHeapCtxMqttDiscovery;
  }

  const HeapProbe reference = heapFloorsSample(ctx);
  uint32_t maxAllocBytes = reference.largestBlockBytes;
  uint32_t freeHeapBytes = reference.freeBytes;

  heapRingEncode(&sHeapRingData[(size_t)sHeapRingHead * kHeapRingEntryBytes],
                 uptimeS,
                 maxAllocBytes,
                 freeHeapBytes,
                 (uint32_t)wsManager.clientCount(),
                 delta);
  sHeapRingHead = (uint16_t)heapRingNextHead(sHeapRingHead, kHeapRingEntries);
  if (sHeapRingFilled < kHeapRingEntries) sHeapRingFilled++;

  // Miroir atomique consommé par le chemin d'échec d'allocation (qui n'a PAS le
  // droit de mesurer le tas lui-même : le verrou du composant heap est détenu
  // par l'allocateur quand le callback est appelé).
  // HORLOGE : on passe l'uptime en TICKS FreeRTOS, PAS millis() (revue v2.18.8).
  // Le chemin d'échec ne peut lire que xTaskGetTickCount() (aucun verrou) ; deux
  // sources différentes dériveraient de quelques secondes et feraient sous-flouer
  // le calcul d'âge sur 16 bits au dump (« t-65530 s »). L'anneau lui-même reste
  // horodaté en millis(), qui n'est comparé qu'à lui-même.
  uint32_t uptimeTickS = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
  oomGuardNoteHeapSample(uptimeTickS, freeHeapBytes, maxAllocBytes);

  // Suivi des CREUX à la cadence de 2 s (revue v2.19.0) : ce sont eux qui
  // calibreront les seuils de 2.19.1. Les échantillonner plus rarement (60 s du
  // contrôle de santé, 5 min de la trace) raterait des creux qui ne durent que
  // quelques secondes et donnerait un minimum trop optimiste — donc un seuil
  // d'auto-guérison trop haut, qui redémarrerait l'appareil en régime normal.
  // Valeur DÉJÀ mesurée ci-dessus : aucune mesure supplémentaire.
  heapNoteMaxAlloc8Sample(maxAllocBytes);
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
  // ancienne à la plus récente : « t-XXXs maxB8=… free8=… ws=… skip=… ».
  // Libellés suffixés « 8 » depuis v2.19.0 : le journal doit être auto-descriptif
  // pour qu'on ne confonde jamais, en relisant d'anciens logs, la mesure 8 bits
  // (réellement allouable) avec l'ancienne mesure « interne totale ».
  char line[224];
  size_t lineLen = 0;
  for (size_t i = 0; i < n; i++) {
    HeapRingSampleFields f = heapRingDecode(
        &sHeapRingData[heapRingIndexOfLast(head, n, kHeapRingEntries, i) *
                       kHeapRingEntryBytes]);
    int w = snprintf(line + lineLen, sizeof(line) - lineLen,
                     "%st-%us maxB8=%u free8=%u ws=%u skip=%u",
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
