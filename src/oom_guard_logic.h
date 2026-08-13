#ifndef OOM_GUARD_LOGIC_H
#define OOM_GUARD_LOGIC_H

// =============================================================================
// oom_guard_logic — Logique PURE du parachute OOM global
// =============================================================================
// bug-oom-accept-http-durcissement (crash du 11/08/2026) : std::bad_alloc non
// rattrapé dans async_tcp (operator new(356) à l'accept HTTP) → abort. Le
// parachute pré-alloue une réserve d'urgence (kOomReserveBytes) libérée par le
// std::set_new_handler à la première famine — le new en cours est alors
// RE-TENTÉ par libstdc++ (contrat new_handler) et réussit au lieu d'aborter.
//
// Ce module contient la logique pure testable en natif (ADR-0017, modèle
// heap_ring_logic.h) :
//   - machine à états Armed/Released/Depleted du parachute ;
//   - décision du handler à l'échec d'allocation (libérer / se désenregistrer) ;
//   - décision de réarmement (hystérésis kOomRearmMinMaxAllocBytes) ;
//   - one-shot du log CRITICAL différé (drainé depuis la loop) ;
//   - prédicat de garde TLS sortante (PSRAM LIBRE + heap), PARTAGÉ par
//     /check-update et /download-update (web_routes_ota.cpp) — aucune logique
//     dupliquée.
// La coquille impure (malloc/free, std::set_new_handler, std::atomic,
// heap_caps_register_failed_alloc_callback, systemLogger) vit dans oom_guard.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas d'ESP.* — <stdint.h>
// uniquement, fonctions inline (header-only, rien à ajouter au build natif).
// La PSRAM LIBRE est donc MESURÉE par la route (ESP.getFreePsram()) et passée
// en paramètre, jamais lue ici. C'est bien la PSRAM LIBRE, et non la taille
// installée (ESP.getPsramSize(), constante et jamais décroissante) : avec
// SPIRAM_USE_MALLOC les allocations > 4 Ko partent en PSRAM, donc une PSRAM
// installée mais SATURÉE ne protège de rien — mbedTLS retomberait sur le heap
// interne, exactement le régime que la v2.18.5 a prouvé insuffisant.
// =============================================================================

#include <stdint.h>

// États du parachute (stockés côté impur dans un std::atomic<uint8_t>).
enum : uint8_t {
  kOomStateArmed = 0,     // Réserve allouée, handler actif — régime nominal
  kOomStateReleased = 1,  // Réserve libérée (famine absorbée), réarmement possible
  kOomStateDepleted = 2,  // Réserve déjà consommée : handler désenregistré
                          // (le new suivant échoue en bad_alloc = comportement
                          // d'avant le parachute) — réarmable par la loop
};

// Action à exécuter par le new_handler (décidée PUREMENT, exécutée côté impur).
struct OomHandlerAction {
  bool freeReserve;        // true → free() de la réserve (déjà extraite atomiquement)
  bool unregisterHandler;  // true → std::set_new_handler(nullptr)
  uint8_t nextState;       // Nouvel état du parachute
};

// Décision du handler à l'échec d'un operator new. `reserveAvailable` = la
// réserve a été extraite atomiquement (exchange non nul) par l'appelant :
//   - réserve disponible → la libérer (le new est re-tenté et réussit) ;
//   - réserve déjà consommée → se désenregistrer puis rendre la main : le new
//     suivant lève bad_alloc (comportement actuel, filet coredump conservé).
// AUCUNE allocation, AUCUN mutex, AUCUN log dans ce chemin.
inline OomHandlerAction oomOnAllocFailed(bool reserveAvailable) {
  OomHandlerAction a;
  if (reserveAvailable) {
    a.freeReserve = true;
    a.unregisterHandler = false;
    a.nextState = kOomStateReleased;
  } else {
    a.freeReserve = false;
    a.unregisterHandler = true;
    a.nextState = kOomStateDepleted;
  }
  return a;
}

// Décision de réarmement (loop, cadence kOomRearmCheckIntervalMs) — hystérésis :
// la réserve est libérée quand maxBloc ≈ épuisé, ré-allouée SEULEMENT quand le
// plus gros bloc contigu repasse au-dessus de rearmMinMaxAllocBytes (32 Ko),
// pour ne pas osciller autour du seuil de famine.
// Le critère est l'ABSENCE DE RÉSERVE, pas l'état : si le handler consomme la
// réserve entre l'exchange du réarmement et le store(Armed) (course revue
// v2.18.5), l'état peut afficher Armed avec une réserve nulle — fonder la
// décision sur le pointeur garantit la convergence au tick suivant.
inline bool oomShouldRearm(bool reserveAbsent, uint32_t maxAllocBytes,
                           uint32_t rearmMinMaxAllocBytes) {
  return reserveAbsent && maxAllocBytes >= rearmMinMaxAllocBytes;
}

// One-shot du log CRITICAL « réserve OOM consommée » : émis en DIFFÉRÉ depuis
// la loop, une seule fois par épisode (comparaison compteur d'épisodes vs
// compteur déjà journalisé — les compteurs ne décroissent jamais).
inline bool oomShouldLogRelease(uint32_t releaseCount, uint32_t loggedCount) {
  return releaseCount != loggedCount;
}

// Motifs de refus de la garde TLS sortante (retour de oomTlsGateReason).
enum : uint8_t {
  kOomTlsGateOk = 0,        // Session TLS autorisée
  kOomTlsGateNoPsram = 1,   // PSRAM libre absente/insuffisante (module sans PSRAM
                            // ou PSRAM saturée) → refus matériel
  kOomTlsGateLowMemory = 2, // PSRAM libre suffisante mais heap momentanément dégradé → réessayable
};

// Prédicat de garde avant TOUTE session TLS sortante (partagé : /check-update,
// /download-update). Renvoie un MOTIF (pas un booléen) : l'appelant doit
// distinguer un refus définitif (matériel) d'un refus temporaire (mémoire).
//
// 1) PSRAM LIBRE d'abord, FAIL-CLOSED (bug-garde-tls-trop-permissive-sans-psram) :
//    mbedTLS réclame plusieurs blocs contigus d'environ 16 Ko. Sans PSRAM, le
//    régime NORMAL d'un WROOM après quelques minutes (mesuré en v2.18.5 :
//    free=87708 o, maxBloc=34804 o) PASSAIT la garde bicritère puis affamait le
//    tas jusqu'à refuser une allocation de 100 o pendant le handshake — la
//    fenêtre exacte du crash bad_alloc du 11/08/2026. Relever le seuil maxBloc
//    (48-50 Ko) ne serait pas une protection : le parachute OOM réserve 12 Ko,
//    donc le maxBloc nominal d'un WROOM tombe de ~49 à ~37 Ko ; un tel seuil ne
//    passerait QUE parachute DÉSARMÉ, c'est-à-dire précisément quand le filet
//    anti-bad_alloc est absent. D'où : PSRAM requise, sinon upload manuel.
//    Le critère porte sur la PSRAM LIBRE (`psramFreeBytes`), pas sur la taille
//    installée : cette dernière est constante et ne décroît jamais, donc un
//    module PSRAM saturé (tout consommé par SPIRAM_USE_MALLOC) ouvrirait quand
//    même la garde alors que mbedTLS retomberait sur la RAM interne — le régime
//    de crash exact. Sur un module de 4 Mo, exiger 1 Mo LIBRE couvre à la fois
//    l'absence de PSRAM et sa saturation.
// 2) Ensuite seulement, le bicritère heap historique (ADR-0032) : heap TOTAL
//    suffisant ET plus gros bloc contigu décent. Seuils INCLUSIFS (>=).
inline uint8_t oomTlsGateReason(uint32_t psramFreeBytes, uint32_t freeHeapBytes,
                                uint32_t maxAllocBytes, uint32_t minPsramBytes,
                                uint32_t minFreeBytes, uint32_t minMaxAllocBytes) {
  if (psramFreeBytes < minPsramBytes) return kOomTlsGateNoPsram;
  if (freeHeapBytes < minFreeBytes || maxAllocBytes < minMaxAllocBytes) {
    return kOomTlsGateLowMemory;
  }
  return kOomTlsGateOk;
}

#endif  // OOM_GUARD_LOGIC_H
