#ifndef WS_CLIENT_REGISTRY_LOGIC_H
#define WS_CLIENT_REGISTRY_LOGIC_H

// =============================================================================
// ws_client_registry_logic — registre PUR des clients WS authentifiés
// =============================================================================
// bug-081 : remplace le `std::set<uint32_t>` de WsManager. Un `set` dédoublonne
// par construction ; un tableau fixe ne le fait pas — d'où l'exigence
// d'idempotence explicite ci-dessous (condition bloquante #1, security-reviewer).
//
// CONCURRENCE (patron ADR-0017, cf. heap_ring_logic.h) : `insert()` est appelé
// depuis le contexte async_tcp (WS_EVT_CONNECT / WS_EVT_DATA, auth réussie) ;
// `erase()` est appelé soit depuis async_tcp (WS_EVT_DISCONNECT direct), soit
// RÉENTRANT depuis loopTask (fermeture synchrone déclenchée PENDANT notre
// propre boucle d'envoi — voir le commentaire d'implémentation de
// WsManager::_sendToAuthenticated). Les deux peuvent donc viser le MÊME slot :
// toute prise/libération se fait par compare_exchange (CAS), jamais par un
// store simple (condition bloquante #2).
//
// INVARIANT : id == 0 signifie « slot libre ». Aucun client WS réel ne porte
// l'id 0 (AsyncWebSocket les attribue à partir de 1, compteur global du fork).
//
// CONTRAINTE module pur (ADR-0017) : pas d'Arduino.h, pas de FreeRTOS —
// <stddef.h>/<stdint.h>/<atomic> uniquement, header-only (rien à ajouter au
// build natif ni à platformio.ini).
// =============================================================================

#include <atomic>
#include <stddef.h>
#include <stdint.h>

enum class WsRegistryInsert : uint8_t {
  kInserted,       // id absent, slot libre trouvé et pris
  kAlreadyPresent, // id déjà présent (idempotent — aucune mutation)
  kFull,           // id absent, aucun slot libre (registre saturé)
};

template <size_t N>
class WsClientRegistry {
 public:
  WsClientRegistry() {
    for (size_t i = 0; i < N; ++i) _slots[i].store(0, std::memory_order_relaxed);
  }

  // IDEMPOTENT par id (condition bloquante #1) : un id déjà présent renvoie
  // kAlreadyPresent sans consommer de slot ni muter l'état. Sans cette
  // garantie, un seul client qui renvoie {"type":"auth"} N fois peut saturer
  // le registre et faire refuser des clients légitimes suivants.
  WsRegistryInsert insert(uint32_t id) {
    if (id == 0) return WsRegistryInsert::kFull;  // garde défensive : 0 = slot libre

    // Passe 1 : déjà présent ? (lecture seule, pas de mutation)
    for (size_t i = 0; i < N; ++i) {
      if (_slots[i].load(std::memory_order_acquire) == id) {
        return WsRegistryInsert::kAlreadyPresent;
      }
    }

    // Passe 2 : prise d'un slot libre par CAS (condition bloquante #2 — jamais
    // un store simple, insert() et erase() peuvent viser le même slot).
    for (size_t i = 0; i < N; ++i) {
      uint32_t expected = 0;
      if (_slots[i].compare_exchange_strong(expected, id, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return WsRegistryInsert::kInserted;
      }
      // Course insert/insert sur le même id (deux trames "auth" quasi
      // simultanées côté async_tcp) : un autre appel a pu poser CE id
      // pendant notre passe 1. Rattrapé ici plutôt que raté.
      if (expected == id) return WsRegistryInsert::kAlreadyPresent;
    }
    return WsRegistryInsert::kFull;
  }

  // Libère le slot portant `id`, s'il existe. CAS id -> 0 (condition #2).
  bool erase(uint32_t id) {
    if (id == 0) return false;
    for (size_t i = 0; i < N; ++i) {
      uint32_t expected = id;
      if (_slots[i].compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return true;
      }
    }
    return false;
  }

  // Copie non bloquante des ids actifs dans `out[0..cap)`. Tronque à `cap` si
  // le registre en contient plus (ne devrait jamais arriver : N == cap côté
  // appelant par construction). Renvoie le nombre d'ids écrits.
  size_t snapshot(uint32_t* out, size_t cap) const {
    size_t n = 0;
    for (size_t i = 0; i < N && n < cap; ++i) {
      uint32_t v = _slots[i].load(std::memory_order_acquire);
      if (v != 0) out[n++] = v;
    }
    return n;
  }

  bool empty() const {
    for (size_t i = 0; i < N; ++i) {
      if (_slots[i].load(std::memory_order_acquire) != 0) return false;
    }
    return true;
  }

  size_t count() const {
    size_t n = 0;
    for (size_t i = 0; i < N; ++i) {
      if (_slots[i].load(std::memory_order_acquire) != 0) ++n;
    }
    return n;
  }

  // Révocation immédiate (bug-081, condition bloquante #3) : vide le registre
  // d'un coup, par stores atomiques simples — sûr depuis N'IMPORTE QUEL
  // contexte (handler HTTP sur async_tcp compris), aucune section critique.
  // N'agit PAS sur les sockets elles-mêmes : c'est à l'appelant de fermer les
  // connexions (closeAll) depuis loopTask, cf. WsManager::revokeAllAuthenticated.
  void clear() {
    for (size_t i = 0; i < N; ++i) _slots[i].store(0, std::memory_order_release);
  }

 private:
  std::atomic<uint32_t> _slots[N];
};

#endif  // WS_CLIENT_REGISTRY_LOGIC_H
