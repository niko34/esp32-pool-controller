#ifndef HEAP_RING_H
#define HEAP_RING_H

// =============================================================================
// heap_ring — Ring d'instrumentation heap en RAM RTC noinit (bug-heap-toctou A4)
// =============================================================================
// Coquille impure du module pur heap_ring_logic.h : stockage RTC_NOINIT_ATTR
// (survit à ESP.restart/panic/WDT, PAS au power-on), échantillonnage depuis la
// loop (kHeapRingSampleIntervalMs), dump dans le journal au boot post-crash.
// Budget : kHeapRingEntries × 8 o = 1 Ko RTC noinit + magic/tête (~1032 o).

// Réinitialise l'état du ring (tête/remplissage à zéro) et pose le magic.
// Appelée automatiquement par heapRingSample() au 1er échantillon après un
// dump/power-on (ré-armement) — exposée pour les diagnostics futurs.
void heapRingInit();

// Échantillonne maxBloc/heap libre/clients WS/pushes sautés dans le ring.
// À appeler depuis loop() (loopTask) toutes les kHeapRingSampleIntervalMs.
void heapRingSample();

// Au boot : si le magic est valide (le ring appartient au boot précédent) ET que
// le reset n'est pas un POWERON → dump des kHeapRingDumpMaxEntries dernières
// entrées dans systemLogger (cinétique d'effondrement du heap avant le reset).
// Le magic est invalidé dans tous les cas — ré-armé au 1er heapRingSample().
// À appeler dans setup() APRÈS systemLogger.begin() et AVANT le WiFi.
void heapRingDumpIfCrash();

// Dernier masque de contexte (bits kHeapCtx*) composé par heapRingSample(),
// à ~2 s près (feature-144). Accesseur dédié pour que le détecteur de chute
// de main.cpp PUISSE lire le contexte SANS recomposer le masque : le bit
// kHeapCtxHttpBurst est un delta CONSOMMATEUR (sLastHttpGetCount, statique
// locale de heap_ring.cpp) — un second lecteur volerait le delta et
// aveuglerait le ring. std::atomic, memory_order_relaxed : lu depuis
// checkHeapHealth() (loopTask), écrit depuis heapRingSample() (même tâche) —
// aucune garantie d'ordonnancement requise au-delà de l'atomicité.
uint8_t heapRingLastCtx();

#endif  // HEAP_RING_H
