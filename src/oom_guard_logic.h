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
//   - prédicat de garde bicritère TLS, PARTAGÉ avec /check-update
//     (web_routes_ota.cpp) — aucune logique dupliquée.
// La coquille impure (malloc/free, std::set_new_handler, std::atomic,
// heap_caps_register_failed_alloc_callback, systemLogger) vit dans oom_guard.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stdint.h> uniquement,
// fonctions inline (header-only, rien à ajouter au build natif).
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

// Prédicat de garde BICRITÈRE avant une session TLS (partagé : /check-update).
// La session TLS GitHub coûte ~58 Ko répartis → exige heap TOTAL suffisant ET
// un plus gros bloc contigu décent. Refus si l'UN des deux critères manque.
inline bool oomTlsGateOk(uint32_t freeHeapBytes, uint32_t maxAllocBytes,
                         uint32_t minFreeBytes, uint32_t minMaxAllocBytes) {
  return freeHeapBytes >= minFreeBytes && maxAllocBytes >= minMaxAllocBytes;
}

#endif  // OOM_GUARD_LOGIC_H
