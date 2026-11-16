#ifndef AUTH_LOG_THROTTLE_SITE_H
#define AUTH_LOG_THROTTLE_SITE_H

// =============================================================================
// auth_log_throttle_site — coquille ATOMIQUE d'un site de refus (bug-112)
// =============================================================================
// Factorise ce que les trois unités (auth.cpp, web_routes_calibration.cpp,
// web_routes_ota.cpp) répétaient : état POD du module pur + miroirs atomiques lus
// par le drain de la loop(). Seul `std::atomic` (portable en natif) : ni Arduino,
// ni String, ni allocation. Chaque unité ne déclare plus que SA statique.
//
// PROPRIÉTÉ DES CHAMPS
//   st                      : écrit par async_tcp UNIQUEMENT (via authLogSiteAttempt)
//   suppressed, flags       : async_tcp (CAS saturant / set), loop (exchange(0))
//   lastAttemptMs/lastWarnMs/shift : PUBLIÉS par async_tcp pour le drain
//                             (le drain ne lit que ces copies : une seule source côté loop)
//   drainedAtMs/drainPending: écrits par le drain, repris par async_tcp qui applique
//                             le drain comme une ligne (authLogThrottleFoldDrain)
// Le drain n'écrit donc jamais l'état du throttle ; c'est async_tcp qui le fait.
// =============================================================================

#include <atomic>
#include <stdint.h>
#include "auth_log_throttle_logic.h"

struct AuthLogThrottleSite {
  AuthLogThrottleState st{};
  std::atomic<uint16_t> suppressed{0};
  std::atomic<bool> otherRoutes{false};
  std::atomic<bool> otherIps{false};
  std::atomic<uint32_t> lastAttemptMs{0};
  std::atomic<uint32_t> lastWarnMs{0};
  std::atomic<uint8_t> shift{0};
  std::atomic<uint32_t> drainedAtMs{0};
  std::atomic<bool> drainPending{false};
};

// Ce qu'une tentative ou un drain demande d'écrire dans le journal.
struct AuthLogSiteReport {
  bool emit;          // une ligne doit être écrite
  uint16_t occulted;  // occultés repris dans cette ligne
  bool otherRoutes;   // parmi eux, d'autres routes
  bool otherIps;      // parmi eux, d'autres IP
};

// Une tentative (async_tcp). Muette => aucune allocation, `emit` faux.
inline AuthLogSiteReport authLogSiteAttempt(AuthLogThrottleSite& site, uint32_t nowMs, uint32_t urlHash,
                                            uint32_t ipV4, uint32_t baseMs, uint8_t maxShift,
                                            uint32_t decayMs) {
  // Reprise d'un drain émis depuis la dernière tentative : il compte comme une ligne.
  if (site.drainPending.exchange(false, std::memory_order_acquire)) {
    authLogThrottleFoldDrain(site.st, site.drainedAtMs.load(std::memory_order_relaxed), maxShift);
  }
  const AuthLogThrottleVerdict v = authLogThrottleStep(site.st, nowMs, urlHash, ipV4, baseMs, maxShift, decayMs);
  site.lastAttemptMs.store(site.st.lastAttemptMs, std::memory_order_relaxed);
  site.lastWarnMs.store(site.st.lastWarnMs, std::memory_order_relaxed);
  site.shift.store(site.st.shift, std::memory_order_relaxed);

  AuthLogSiteReport r = {false, 0, false, false};
  if (!v.emit) {
    // Incrément saturant (jamais de repliement à 0).
    uint16_t cur = site.suppressed.load(std::memory_order_relaxed);
    while (cur != UINT16_MAX &&
           !site.suppressed.compare_exchange_weak(cur, authLogThrottleSatInc(cur), std::memory_order_relaxed)) {
    }
    if (v.otherRoute) site.otherRoutes.store(true, std::memory_order_relaxed);
    if (v.otherIp) site.otherIps.store(true, std::memory_order_relaxed);
    return r;
  }
  r.emit = true;
  r.occulted = site.suppressed.exchange(0, std::memory_order_relaxed);
  r.otherRoutes = site.otherRoutes.exchange(false, std::memory_order_relaxed);
  r.otherIps = site.otherIps.exchange(false, std::memory_order_relaxed);
  return r;
}

// Le drain (loop UNIQUEMENT). Borné par le même recul que l'émission.
// N'écrit ni `st`, ni lastWarnMs, ni everLogged, ni shift : il publie seulement
// `drainedAtMs` + `drainPending`, repris par la tentative suivante.
inline AuthLogSiteReport authLogSiteDrain(AuthLogThrottleSite& site, uint32_t nowMs, uint32_t baseMs,
                                          uint8_t maxShift, uint32_t quietMs) {
  AuthLogSiteReport r = {false, 0, false, false};
  // Charges en LOCALES, `suppressed` en premier : l'ordre d'évaluation des arguments
  // d'une fonction n'est pas spécifié par le langage.
  const uint16_t suppressed = site.suppressed.load(std::memory_order_relaxed);
  const uint32_t lastAttempt = site.lastAttemptMs.load(std::memory_order_relaxed);
  const uint32_t lastWarn = site.lastWarnMs.load(std::memory_order_relaxed);
  const uint8_t shift = site.shift.load(std::memory_order_relaxed);
  if (!authLogThrottleDrainDueBounded(suppressed, nowMs, lastAttempt, quietMs, lastWarn, shift, baseMs,
                                      maxShift)) {
    return r;
  }
  r.occulted = site.suppressed.exchange(0, std::memory_order_relaxed);
  if (r.occulted == 0) return r;
  r.otherRoutes = site.otherRoutes.exchange(false, std::memory_order_relaxed);
  r.otherIps = site.otherIps.exchange(false, std::memory_order_relaxed);
  site.drainedAtMs.store(nowMs, std::memory_order_relaxed);
  site.drainPending.store(true, std::memory_order_release);
  r.emit = true;
  return r;
}

#endif  // AUTH_LOG_THROTTLE_SITE_H
