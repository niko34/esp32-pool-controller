#ifndef OOM_GUARD_H
#define OOM_GUARD_H

#include <stddef.h>
#include <stdint.h>

// =============================================================================
// oom_guard — Parachute OOM global (glue Arduino du module pur oom_guard_logic.h)
// =============================================================================
// bug-oom-accept-http-durcissement (crash du 11/08/2026) : réserve d'urgence
// kOomReserveBytes pré-allouée au plus tôt du setup + std::set_new_handler qui
// la libère à la première famine (le new en cours est re-tenté et réussit) +
// heap_caps_register_failed_alloc_callback en mode flag-seul (diagnostic).
// Les logs (CRITICAL « réserve OOM consommée », WARN échecs heap_caps) sont
// émis en DIFFÉRÉ depuis la loop — JAMAIS depuis le handler/callback.
//
// bug-parachute-oom-jamais-rearme (crash du 13/08/2026, v2.18.8) : réarmement à
// deux paliers (nominal/repli) + POST-MORTEM d'allocation en RAM RTC noinit
// (32 o) écrit depuis le chemin d'échec en stores bruts, dumpé au boot suivant,
// + contrôle d'intégrité périodique du tas. Budget RAM RTC noinit du projet :
// auto-heal 9 o + anneau heap 1032 o + post-mortem 32 o = 1073 o sur 8192.

// Pré-alloue la réserve d'urgence (palier NOMINAL kOomReserveBytes, à défaut
// palier de REPLI kOomFallbackReserveBytes — v2.18.8), enregistre le
// new_handler et le callback diagnostic heap_caps. À appeler au PLUS TÔT du
// setup (avant toute fragmentation) — n'émet AUCUN log (systemLogger pas encore
// initialisé). NE TOUCHE PAS la zone post-mortem RTC du boot précédent (elle
// n'est lue/invalidée que par oomPostMortemDumpIfBoot()).
void initOomGuard();

// Tick de la loop : drain des flags posés par le handler/callback (émission
// différée des logs) + réarmement de la réserve à cadence lente
// (kOomRearmCheckIntervalMs) selon le palier atteignable (oomRearmTargetBytes :
// nominal ≥ kOomRearmMinMaxAllocBytes, repli ≥ kOomFallbackRearmMinMaxAllocBytes,
// surclassement repli→nominal possible). À appeler depuis loop() (loopTask).
void oomGuardLoopTick();

// true si la réserve d'urgence est actuellement allouée (diagnostic — permet
// au setup de journaliser l'état réel de l'armement).
bool oomGuardReserveArmed();

// Taille (octets) de la réserve RÉELLEMENT armée, 0 si aucune. Permet au setup
// de journaliser le palier obtenu (nominal ou repli) — lecture seule.
size_t oomGuardReserveArmedBytes();

// Miroir atomique du dernier échantillon de l'anneau heap (heap_ring.cpp, cadence
// kHeapRingSampleIntervalMs). Les valeurs passées sont celles DÉJÀ mesurées par
// l'anneau : aucun parcours de tas supplémentaire. Ce miroir est la SEULE source
// de « free / maxBloc » du post-mortem — le chemin d'échec d'allocation ne
// mesure JAMAIS le tas lui-même (réentrance allocateur interdite, ADR-0035).
void oomGuardNoteHeapSample(uint32_t uptimeS, uint32_t freeBytes, uint32_t maxAllocBytes);

// Dump du post-mortem d'allocation du boot PRÉCÉDENT (zone RTC noinit de 32 o) :
// une ligne unique donnant l'état au moment EXACT de l'échec + le verdict
// (famine / suspicion de corruption). Lit l'INSTANTANÉ pris par initOomGuard()
// (la zone RTC, elle, a déjà été invalidée à ce moment-là et appartient au boot
// COURANT). À appeler dans setup(), juste après heapRingDumpIfCrash().
void oomPostMortemDumpIfBoot();

// Contrôle d'intégrité périodique du tas (heap_caps_check_integrity_all, cadence
// kHeapIntegrityCheckIntervalMs, autolimitation kHeapIntegrityMaxDurationUs).
// L'APPELANT est responsable des gardes de sécurité chimique (condition
// pool-chemistry C4 : pas de dosage/injection/pompe énergisée, pas d'OTA, pas de
// mode maintenance, uptime ≥ kAutoHealMinUptimeMs) et doit l'appeler APRÈS
// PumpController.update() (condition C2).
// Retourne true UNE SEULE FOIS, au tick où une corruption est détectée : à
// l'appelant d'exécuter alors le chemin d'arrêt propre. Tout contrôle ultérieur
// est définitivement désactivé.
bool oomGuardIntegrityTick();

// Enregistre le résultat d'un contrôle d'intégrité : lève bit4 (« jamais
// exécuté ») et pose bit3 dans les drapeaux du post-mortem si le contrôle a
// échoué (écrit alors la zone RTC pour que l'information survive au reboot).
void oomGuardNoteIntegrityResult(bool ok);

#endif  // OOM_GUARD_H
