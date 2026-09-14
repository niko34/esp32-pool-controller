#ifndef HEAP_CENSUS_H
#define HEAP_CENSUS_H

#include <stdint.h>

#include "constants.h"          // kHeapCensus*, kHeapPeek*
#include "heap_census_logic.h"  // structures + arithmétique PURE (ADR-0017)

// =============================================================================
// heap_census — recensement du tas (R2) + lecture de contenu (R3), feature-128
// =============================================================================
// CONTEXTE : la fenêtre de mesure du 12-13/09 a établi QUE la fragmentation est
// réelle sans établir QUI découpe le grand bloc contigu. Cet instrument recense
// le tas interne 8 bits bloc par bloc (adresses + tailles, R2) et permet de
// lire le contenu d'un bloc désigné (R3, non livré en production) pour
// identifier une allocation par son CONTENU plutôt que par sa taille (le
// premier suspect, `logger`, alloue des `String` de taille VARIABLE — aucune
// constante à retrouver par grep).
//
// ---------------------------------------------------------------------------
// RÉGIME DE VERROUILLAGE — ÉTABLI PAR DÉSASSEMBLAGE (nm libheap.a), à NE PAS
// RECONTRÔLER :
//   heap_caps_walk -> multi_heap_walk -> multi_heap_internal_lock ->
//   xPortEnterCriticalTimeout ... tlsf_walk_pool (LE CALLBACK UTILISATEUR EST
//   APPELÉ ICI) ... multi_heap_internal_unlock -> vPortExitCritical.
//
// LE CALLBACK S'EXÉCUTE INTERRUPTIONS MASQUÉES PENDANT TOUT LE PARCOURS. C'est
// un SPINLOCK INTER-CŒURS (multi_heap_internal_lock est un portMUX partagé) :
// une tâche de L'AUTRE CŒUR qui tente d'allouer pendant le parcours ATTEND
// elle aussi, ses propres interruptions masquées le temps de l'attente. Sans
// conséquence pour ce projet (aucun ISR, aucun bit-banging sur ce cœur), mais
// la propriété est écrite ici plutôt que supposée (exigence pool-chemistry).
//
// Pire cas ESTIMÉ (pas mesuré sur CE parcours précis — extrapolé depuis le
// parcours de référence 0x0804 instrumenté par heapFloorsSample(), même
// régime de verrouillage, cf. multi_heap_get_info_impl en tête de
// heap_metrics.h) : ~1,0-1,1 ms au nombre de blocs actuel, ~1,5 ms si ce
// nombre croît de 50 % — d'où kHeapCensusMaxDurationUs = 1500. ⚠️ ADR-0073 :
// ne PAS recopier ce chiffre comme une mesure figée — la base dérive (wmax
// +38 % en six jours de relevés, constants.h) ; c'est PRÉCISÉMENT pour ça que
// C1 chronomètre et s'auto-désactive au lieu de se fier à cette estimation
// (voir plus bas).
//
// ---------------------------------------------------------------------------
// PROTOCOLE EN DEUX TEMPS — le parcours n'a JAMAIS lieu sur async_tcp
// ---------------------------------------------------------------------------
// Idiome déjà en place dans ce projet : POST /debug/sensor_filter_reset ->
// requestFilterReset() consommé en tête de SensorManager::update()
// (web_routes_debug.cpp). Machine à états LOCK-FREE (CAS, std::atomic<uint8_t>) :
//
//   IDLE -> REQUESTED   : posé par async_tcp (heapCensusRequest / heapPeekRequest)
//   REQUESTED -> READY  : posé par la LOOP (heapCensusServiceLoop) — SEUL site
//                         qui appelle heap_caps_walk (invariant du projet).
//   READY -> READING    : posé par async_tcp au début de la sérialisation GET
//   READING -> IDLE     : posé par async_tcp une fois la réponse envoyée
//   READY -> IDLE       : posé par la LOOP (heapCensusServiceLoop, TROISIÈME
//                         transition pilotée par elle, revue M1) si l'instantané
//                         est périmé (kHeapCensusStaleMs) et que PERSONNE ne
//                         l'a lu — sans cette transition, un POST sans GET qui
//                         suit (script interrompu, coupure WiFi) verrouille
//                         DÉFINITIVEMENT R2/R3 en 409, la péremption n'étant
//                         sinon évaluée QUE dans heapCensusBeginRead().
//
// Aucun verrou nouveau : les transitions sont des CAS sur un unique atomique
// par machine (une pour R2, une pour R3). L'instantané est donc DÉJÀ GELÉ
// quand la route sérialise — les allocations de la réponse HTTP (JsonDocument,
// AsyncResponseStream) ne perturbent plus la mesure, et R3 revalide son adresse
// contre un parcours FRAIS à chaque lecture plutôt que contre un instantané
// vieillissant.
//
// ---------------------------------------------------------------------------
// C1 (gate pool-chemistry, BLOQUANT) — chronomètre + auto-désactivation
// ---------------------------------------------------------------------------
// esp_timer_get_time() (JAMAIS le compteur de cycles CCOUNT — sans objet ici,
// la mesure est locale à un seul appel, aucune migration de cœur à corriger).
// Patron de oom_guard.cpp:408-418 (heapIntegrityAdmitNextRun -> sProbeDisabled)
// : au premier parcours dont la durée dépasse kHeapCensusMaxDurationUs, R2 ET
// R3 sont désactivés DÉFINITIVEMENT (le drapeau n'est jamais réarmé — la base
// est mouvante, cf. constants.h, un dépassement ponctuel ne prouve rien sur le
// prochain, mais la spec exige que l'instrument se coupe lui-même plutôt que
// de se fier à une estimation périmée).
//
// C4 : un WARN + l'exposition de `censusMaxUs`/du drapeau de désactivation
// sont attendus côté MQTT/route de lecture (hors périmètre de ce module — ce
// fichier expose heapCensusMaxWalkUs()/heapCensusIsDisabled(), à lire par
// l'appelant). ⚠️ COMPATIBLE avec S13 (zéro systemLogger dans TOUT le code
// neuf, routes ET loop) : le WARN de C4 est un ÉVÉNEMENT UNIQUE d'auto-
// désactivation posé par ce module — mais ce module LUI-MÊME n'appelle jamais
// systemLogger (S13 l'interdit explicitement, y compris ici). L'émission du
// log, si elle a lieu, est la responsabilité d'un appelant HORS de ce module
// (pas async_tcp, pas un chemin d'erreur HTTP) : ce fichier se contente
// d'exposer l'état pour qu'un tel appelant existe ailleurs. Les deux
// conditions ne sont donc PAS contradictoires : S13 porte sur LES CHEMINS DE
// CE MODULE, C4 sur un site d'émission qui n'en fait pas partie.
//
// ---------------------------------------------------------------------------
// C2 (gate pool-chemistry, BLOQUANT) — mémoire des tampons
// ---------------------------------------------------------------------------
// Les tampons statiques (~1,4 Ko) restent en .bss INTERNE (RAM classique).
// `EXT_RAM_BSS_ATTR` EST INTERDIT ICI : écrire en PSRAM depuis une section à
// interruptions masquées gonflerait la durée du parcours d'un facteur
// décisif (accès PSRAM nettement plus lent que la RAM interne, et c'est
// précisément le budget que C1 borne à 1,5 ms).
//
// ---------------------------------------------------------------------------
// S8 (sécurité, BLOQUANT) — R2 ne contient AUCUN contenu
// ---------------------------------------------------------------------------
// Le callback de recensement ne déréférence QUE blk.ptr[0..7] (canari +
// alloc_size du poisoning léger), et SEULEMENT SI used == true — en poisoning
// léger, un bloc LIBRE ne porte aucun en-tête fiable à cette adresse (le tas
// peut y avoir écrit un lien de liste libre TLSF). HeapCensusFreeEntry /
// HeapCensusUsedEntry (heap_census_logic.h) ne portent que des adresses et des
// tailles — jamais un octet de contenu.
//
// ---------------------------------------------------------------------------
// S13 (sécurité, BLOQUANT) — zéro systemLogger dans tout le code neuf
// ---------------------------------------------------------------------------
// Aucun #include "logger.h" dans heap_census.h/.cpp. Tous les échecs (busy,
// trop tôt, désactivé, budget épuisé, canari non conforme) se disent par un
// CODE DE RETOUR (enum), jamais par un log — la fenêtre forensique du journal
// persistant tombe en ~27 requêtes (logger.h:50-51), une route de diagnostic
// ne doit jamais pouvoir la consommer.
// =============================================================================

// ---------------------------------------------------------------------------
// R2 — Recensement (adresses + tailles uniquement)
// ---------------------------------------------------------------------------

/** @brief État de la machine à états du recensement (R2). */
enum HeapCensusState : uint8_t {
  kHeapCensusIdle = 0,
  kHeapCensusRequested = 1,
  kHeapCensusReady = 2,
  kHeapCensusReading = 3,
};

/** @brief Verdict de heapCensusRequest() — code de retour, jamais un log (S13). */
enum HeapCensusRequestResult : uint8_t {
  kHeapCensusReqQueued = 0,     // IDLE -> REQUESTED accepté
  kHeapCensusReqBusy = 1,       // Un cycle est déjà en cours (409)
  kHeapCensusReqTooSoon = 2,    // Plancher kHeapCensusMinIntervalMs non écoulé (429)
  kHeapCensusReqDisabled = 3,   // Auto-désactivé définitivement, C1 (410)
};

/** @brief Verdict de heapCensusBeginRead(). */
enum HeapCensusReadResult : uint8_t {
  kHeapCensusReadOk = 0,         // *outSnap valide jusqu'à heapCensusEndRead()
  kHeapCensusReadNotReady = 1,   // Toujours IDLE/REQUESTED : rien à lire (409)
  kHeapCensusReadStale = 2,      // READY mais kHeapCensusStaleMs dépassé (410)
  kHeapCensusReadBusy = 3,       // Déjà en lecture (concurrence route, improbable)
};

/**
 * @brief Instantané figé d'un recensement abouti.
 *
 * Lu UNIQUEMENT entre heapCensusBeginRead() (READY -> READING) et
 * heapCensusEndRead() (READING -> IDLE) : aucune lecture déchirée possible,
 * la loop n'écrit ce buffer QUE dans l'état REQUESTED.
 */
struct HeapCensusSnapshot {
  uint32_t seq;                // Générations de recensements aboutis depuis le boot (1 = 1er)
  uint32_t tookAtMs;           // millis() loop au moment du parcours
  // heapStart/heapEnd : ENVELOPPE (min/max) de TOUTES les régions parcourues
  // (`heap_caps_walk(0x0804, …)` visite potentiellement PLUSIEURS tas portant
  // ces caps — esp_heap_caps.h:468-470, "of the heap in which the block is
  // located", au SINGULIER, par bloc). ⚠️ CETTE UNION N'EST PAS CONTIGUË : il
  // peut exister des adresses dans [heapStart, heapEnd) qui n'appartiennent à
  // AUCUNE région réelle (trou ENTRE deux tas). `regionCount` (ci-dessous) dit
  // combien de régions distinctes composent cette enveloppe — 1 = enveloppe
  // fiable, >1 = l'opérant doit savoir qu'il regarde une union trouée avant
  // de choisir usedFrom/usedTo sur la seule foi de ces deux bornes.
  uint32_t heapStart;
  uint32_t heapEnd;
  uint32_t regionCount;         // Nb de régions (tas) distinctes visitées par CE parcours
  uint32_t usedFrom;           // Bornes demandées pour CE parcours (écho, diagnostic)
  uint32_t usedTo;
  uint32_t freeBlockCount;     // Nb TOTAL de blocs libres vus pendant CE parcours (agrégat A2)
  uint32_t usedBlockCount;     // Nb TOTAL de blocs occupés vus pendant CE parcours (agrégat A2)
  uint32_t usedInWindowCount;  // Nb de blocs occupés dont l'adresse tombe dans [usedFrom, usedTo)
  uint8_t freeReturned;        // Entrées valides dans freeBlocks[] (top-N approché)
  uint8_t usedReturned;        // Entrées valides dans usedBlocks[] (fenêtre demandée)
  bool usedTruncated;          // I13 : true si usedInWindowCount > kHeapCensusMaxUsedBlocks (jamais silencieux)
  // A2 : concordance avec fb/ab de heapFloorsRead() — LUS après le parcours,
  // jamais reparcourus (I2 : aucun heap_caps_get_info() neuf sur ce chemin).
  uint32_t floorFreeBlocks;    // Écho de HeapFloorEntry::lastFreeBlocks (0x0804) au moment du parcours
  uint32_t floorUsedBlocks;    // Écho de HeapFloorEntry::lastAllocatedBlocks (0x0804)
  uint8_t concordanceVerdict;  // HeapCensusVerdict (heap_census_logic.h)
  HeapCensusFreeEntry freeBlocks[kHeapCensusMaxFreeBlocks];
  HeapCensusUsedEntry usedBlocks[kHeapCensusMaxUsedBlocks];
};

/**
 * @brief POST (appelé depuis async_tcp) : demande un recensement. IDLE -> REQUESTED.
 *
 * `usedTo == 0` demande une fenêtre VIDE (heapCensusInWindow) : le recensement
 * a quand même lieu (agrégats freeBlockCount/usedBlockCount remplis), mais
 * aucun bloc occupé n'est publié en détail.
 *
 * S9 : le plancher temporel est revérifié côté loop (heapCensusServiceLoop) —
 * ce contrôle ici est une réponse RAPIDE (429 sans même poser REQUESTED), pas
 * l'unique garde.
 */
HeapCensusRequestResult heapCensusRequest(uint32_t usedFrom, uint32_t usedTo);

/** @brief État courant, lecture atomique sans effet de bord. */
HeapCensusState heapCensusGetState();

/**
 * @brief Consommée en tête de loop (main.cpp, juste après heapRingSample()).
 *
 * REQUESTED -> READY : SEUL site du projet qui appelle heap_caps_walk pour ce
 * module. Sans effet si l'état est IDLE/READY/READING, si le module est
 * désactivé (C1), ou si le plancher temporel n'est pas écoulé (S9 : revérifié
 * ICI avec l'horodatage ÉCRIT PAR LA LOOP, pas seulement celui lu côté route).
 *
 * Sert aussi R3 (heapPeekServiceLoop interne) si POOL_HEAP_PEEK est défini —
 * un seul site de parcours pour tout le module (invariant du plan).
 */
void heapCensusServiceLoop();

/**
 * @brief READY -> READING : verrouille l'instantané pour la sérialisation.
 * @param outSnap Reçoit l'adresse de l'instantané (valide jusqu'à heapCensusEndRead()).
 */
HeapCensusReadResult heapCensusBeginRead(const HeapCensusSnapshot** outSnap);

/** @brief READING -> IDLE. À appeler après sérialisation, y compris en cas d'erreur. */
void heapCensusEndRead();

// --- Observabilité (C4) — lue par l'appelant, jamais journalisée ICI (S13) ---
uint32_t heapCensusLastWalkUs();  // Durée du DERNIER parcours abouti (R2 ou R3)
uint32_t heapCensusMaxWalkUs();   // Pire parcours observé depuis le boot (R2 ou R3)
bool heapCensusIsDisabled();      // true si C1 a désactivé DÉFINITIVEMENT le module

#if defined(POOL_HEAP_PEEK) && POOL_HEAP_PEEK == 1
// ===========================================================================
// R3 — Lecture de contenu, NON LIVRÉ EN PRODUCTION (ADR-0081)
// ===========================================================================
// Cette macro n'est DÉFINIE dans AUCUN fichier versionné (ni platformio.ini,
// ni constants.h, ni un .h) : build de diagnostic uniquement, fabriqué par une
// variable d'environnement passée à la ligne de commande `pio run` (jamais
// écrite ici en toutes lettres — S2/ADR-0081 : la sentinelle CI cherche cette
// définition dans les fichiers versionnés hors docs/specs/*.md, où la commande
// exacte est documentée).
//
// S3 (rappel pour l'appelant, la route) : SANS AUTHENTIFICATION RENFORCÉE, R3
// EST NO GO. Chaque handler R3 doit refuser 403 SANS LOG si
// !authManager.isEnabled() OU authManager.isFirstBootDetected(), PUIS
// REQUIRE_AUTH(CRITICAL) — hors périmètre de ce module (routes).

/** @brief Verdict de heapPeekRequest() — code de retour, jamais un log (S13). */
enum HeapPeekRequestResult : uint8_t {
  kHeapPeekReqQueued = 0,           // IDLE -> REQUESTED accepté
  kHeapPeekReqBusy = 1,             // Un cycle est déjà en cours (409)
  kHeapPeekReqTooSoon = 2,          // Plancher PARTAGÉ avec R2 non écoulé (429)
  kHeapPeekReqDisabled = 3,         // Auto-désactivé définitivement, C1 (410)
  kHeapPeekReqBudgetExhausted = 4,  // kHeapPeekMaxBytesPerBoot épuisé (403 jusqu'au reboot)
  kHeapPeekReqInvalidLen = 5,       // len == 0 (rejet précoce, aucun parcours posé)
};

/** @brief Verdict de heapPeekBeginRead(). */
enum HeapPeekReadResult : uint8_t {
  kHeapPeekReadOk = 0,
  kHeapPeekReadNotReady = 1,  // (409)
  kHeapPeekReadStale = 2,     // (410)
  kHeapPeekReadBusy = 3,
};

/**
 * @brief Résultat figé d'une lecture de contenu.
 *
 * S4 : `len` est DÉJÀ clampé à min(requestedLen, kHeapPeekMaxBytes, allocSize
 * du bloc désigné, budget restant). Aucun octet de contenu au-delà de `len`
 * n'est garanti initialisé — l'appelant ne doit publier que `bytes[0..len)`.
 */
struct HeapPeekSnapshot {
  uint32_t seq;
  uint32_t tookAtMs;
  uint32_t addr;         // Adresse demandée (écho)
  uint32_t allocSize;    // poison_head_t.alloc_size du bloc désigné, ou kHeapCensusAllocSizeUnknown
  uint32_t len;           // Octets effectivement copiés dans bytes[]
  bool found;              // Un bloc OCCUPÉ existe exactement à `addr` (walker_block_info_t.ptr)
  bool canaryValid;        // Canari conforme AVANT la copie (S4)
  bool stable;              // Canari toujours conforme APRÈS la copie (S4)
  bool budgetClamped;       // I13 : `len` a été réduit par le budget restant (jamais silencieux)
  uint8_t bytes[kHeapPeekMaxBytes];
};

/**
 * @brief POST (async_tcp) : demande la lecture de kHeapPeekMaxBytes au plus
 * à partir de `addr + kHeapPoisonHeadBytes`. IDLE -> REQUESTED.
 *
 * `addr` DOIT être l'adresse BRUTE d'un bloc telle que publiée par R2
 * (`HeapCensusUsedEntry::addr`, walker_block_info_t.ptr) — S4 : la validation
 * définitive a lieu contre un parcours FRAIS, côté loop, pas ici.
 */
HeapPeekRequestResult heapPeekRequest(uint32_t addr, uint32_t len);

HeapCensusState heapPeekGetState();

/** @brief READY -> READING. */
HeapPeekReadResult heapPeekBeginRead(const HeapPeekSnapshot** outSnap);

/** @brief READING -> IDLE. */
void heapPeekEndRead();

/** @brief Octets restants dans le budget cumulé depuis le boot (kHeapPeekMaxBytesPerBoot). */
uint32_t heapPeekBudgetRemaining();

#endif  // POOL_HEAP_PEEK

#endif  // HEAP_CENSUS_H
