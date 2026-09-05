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
// écrit depuis le chemin d'échec en stores bruts, dumpé au boot suivant,
// + contrôle d'intégrité périodique du tas.
//
// bug-080 lot B-2 : la zone passe de 32 à 48 o (magic bumpé en 0xF00DFA13) pour
// accueillir la SONDE prise au moment EXACT du refus — double mesure (caps de
// l'échec + référence INTERNAL|8BIT) qui remplace la confrontation à un
// échantillon d'anneau périmé de 2 s. Budget RAM RTC noinit du projet :
// auto-heal 9 o + anneau heap 1032 o + post-mortem 48 o = 1089 o sur 8192.

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

/**
 * @brief true si la réserve d'urgence armée réside bien en RAM INTERNE.
 *
 * Condition C1 du gate pool-chemistry de bug-080 B-1 : depuis la bascule PSRAM
 * (`heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)`), l'emplacement
 * de la réserve n'est plus une évidence mais un INVARIANT À OBSERVER — libérer
 * 12 Ko de PSRAM pendant une famine de RAM interne ne sauverait rien, et le
 * parachute serait mort sans que rien ne le dise. La réponse est LUE
 * (`esp_ptr_internal()` sur le pointeur publié), jamais supposée.
 *
 * @return false si aucune réserve n'est armée, ou si elle a atterri en PSRAM
 *         (ANOMALIE : les trois sites d'allocation passent par
 *         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT explicites).
 *
 * Lecture seule : ni allocation, ni log, ni parcours de tas — sûr depuis un
 * handler AsyncWebServer (`/get-system-info`) comme depuis la loop.
 */
bool oomGuardReserveIsInternalRam();

/**
 * @brief Consomme (drain one-shot) le drapeau « aucune réserve armable ».
 *
 * bug-080 B-1, revue priorité 1. `oomGuardLoopTick()` LÈVE ce drapeau quand
 * aucun palier n'a pu être armé depuis `kOomNoReserveCriticalDelayMs`, et
 * journalise lui-même en `critical()`. Il ne publie PAS l'alerte MQTT : cela
 * allouerait (JsonDocument + String) au moment précis où le tas vient de
 * refuser 1 Ko trente fois de suite, écrirait le message une SECONDE fois dans
 * un journal de 8 Ko dont la rétention est de 25-35 min, et ferait dépendre le
 * filet de sécurité de plus bas niveau du sous-système applicatif le plus haut.
 *
 * `mqtt_manager` appelle donc ceci depuis SA tâche, qui alloue déjà et connaît
 * son état de connexion. Alerte perdue si MQTT est absent : le `critical()` du
 * journal reste la source de vérité.
 *
 * @return true une seule fois par épisode ; le compteur repart dès qu'une
 *         réserve est de nouveau armée.
 */
bool oomGuardConsumeNoReserveAlert();

/**
 * @brief true si le std::set_new_handler du parachute est actuellement inscrit.
 *
 * Miroir atomique (v2.20.2, bloc A) : passe à false au moment exact où
 * oomNewHandler se désinscrit (2ᵉ famine sans réserve), repasse à true dès que
 * oomGuardLoopTick() le ré-inscrit — ce qui n'a lieu QUE si une réserve est armée
 * (condition C1 du gate pool-chemistry : un handler sans réserve ne libère rien).
 *
 * Exposé pour le diagnostic (`/get-system-info`) : sans lui, l'appareil peut
 * tourner des heures SANS filet anti-bad_alloc sans que rien ne le signale —
 * exactement le régime des crashs des 13 et 15/08/2026. Lecture seule, sans log
 * ni allocation.
 */
bool oomGuardHandlerRegistered();

// Miroir atomique du dernier échantillon de l'anneau heap (heap_ring.cpp, cadence
// kHeapRingSampleIntervalMs). Les valeurs passées sont celles DÉJÀ mesurées par
// l'anneau : aucun parcours de tas supplémentaire. Ce miroir est la SEULE source
// de « free / maxBloc » du post-mortem — le chemin d'échec d'allocation ne
// mesure JAMAIS le tas lui-même (réentrance allocateur interdite, ADR-0035).
void oomGuardNoteHeapSample(uint32_t uptimeS, uint32_t freeBytes, uint32_t maxAllocBytes);

// Dump du post-mortem d'allocation du boot PRÉCÉDENT (zone RTC noinit de
// kOomPostMortemBytes octets) : DEUX lignes — la comptabilité de l'épisode + le
// verdict (famine mémoire / famine DMA / suspicion de corruption / indéterminé),
// puis la SONDE prise au moment exact du refus (ou la raison de son absence).
// Lit l'INSTANTANÉ pris par initOomGuard() (la zone RTC, elle, a déjà été
// invalidée à ce moment-là et appartient au boot COURANT). À appeler dans
// setup(), juste après heapRingDumpIfCrash().
void oomPostMortemDumpIfBoot();

/**
 * @brief Compteurs d'observabilité de la sonde du refus (bug-080 B-2).
 *
 * Condition 4 du gate pool-chemistry : « rien de silencieux ». Le max observé,
 * le nombre de sondes et les caps sondées doivent être lisibles depuis le
 * diagnostic, sans attendre un crash. Lecture seule : ni allocation, ni log, ni
 * parcours de tas — sûr depuis un handler AsyncWebServer comme depuis la loop.
 */
struct OomProbeStats {
  uint32_t count;           // Sondes prises depuis le boot
  uint32_t lastDurationUs;  // Durée du dernier COUPLE (2 parcours) — 0xFFFF = invalide (migration de cœur)
  uint32_t maxDurationUs;   // Pire durée valide observée
  uint32_t lastCaps;        // Masque heap_caps de la dernière sonde
  bool enabled;             // false = sonde DÉSACTIVÉE définitivement (durée > kOomProbeMaxDurationUs)
};
OomProbeStats oomGuardProbeStats();

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
