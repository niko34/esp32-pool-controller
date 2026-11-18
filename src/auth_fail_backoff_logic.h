#ifndef AUTH_FAIL_BACKOFF_LOGIC_H
#define AUTH_FAIL_BACKOFF_LOGIC_H

// =============================================================================
// auth_fail_backoff_logic — ralentissement progressif des ÉCHECS de mot de passe
// (bug-189), logique PURE (ADR-0017, testable en natif). Aucune allocation, aucun
// journal (ADR-0082), aucune dépendance Arduino.
// =============================================================================
// TROIS PORTES, à capacité FIXE partout (feature-131) :
//   1. Porte PAR IP    : kAuthFailIpSlots créneaux, remplis SUR ÉCHEC. ⛔ Aucune
//                        éviction : créneaux pleins => l'adresse retombe sur la globale.
//   2. Porte GLOBALE   : un seul état, toutes adresses confondues.
//   3. Table d'EXEMPTION : kAuthFailExemptSlots adresses ayant PROUVÉ le mot de passe.
//                        Elles échappent à la porte globale. ⭐ Chacune porte SA PROPRE
//                        porte par IP, indépendante des créneaux : sans cela, saturer
//                        les créneaux laisserait une adresse exemptée sans aucune limite.
//                        Éviction de la PLUS ANCIENNE réussite, par compteur monotone,
//                        ⛔ jamais par millis().
//
// VERDICT : refus si la porte IP refuse, OU si l'adresse n'est pas exemptée et que la
// porte globale refuse. Retry-After = le PLUS GRAND des délais applicables.
//
// ARITHMÉTIQUE DE TEMPS (condition 1 de P1) : ⛔ pas d'échéance absolue (« nextAllowedMs »)
// — au repliement de millis() elle bloquerait jusqu'à 49,7 jours. On ne garde que
// lastFailMs et on juge sur l'écart NON SIGNÉ now - lastFailMs ; le décalage est borné
// AVANT le <<. L'attente restante vaut donc au plus le plafond, PAR CONSTRUCTION.
//
// RÉSIDUS ASSUMÉS (condition 2 de P1) :
//  - Un REFUS ne modifie aucun état (sinon l'attaquant repousserait lastFailMs sans fin).
//    Conséquence : une porte qui se libère laisse passer UN essai, et un attaquant qui
//    frappe plus vite que le propriétaire le consomme avant lui (famine, G1). C'est la
//    table d'exemption qui réduit ce risque ; elle ne le supprime pas.
//  - L'état est en RAM : un redémarrage rend la franchise et VIDE la table d'exemption
//    (cas de la page de secours OTA : redémarrage => table vide).
//  - Un état resté intact plus de 49,7 jours peut sembler « récent » après repliement :
//    il refuse alors au plus UN plafond (60 s), jamais davantage.
//  - Le compteur d'ordre de la table d'exemption se replie après 2^32 réussites ; la
//    comparaison d'âge est faite sur l'écart non signé, donc l'ordre reste correct.
//  - Un échec d'une adresse EXEMPTÉE alimente SA porte, pas la globale : elle est bornée
//    par la sienne et ne doit pas ralentir les autres.
//  - Une adresse inconnue (ip == 0) n'est JAMAIS exemptée.
//  - Concurrence : les handlers /auth/* s'exécutent tous sur la tâche async_tcp ; l'état
//    n'est donc ni verrouillé ni atomique. Contrôle, comparaison et comptage doivent se
//    faire dans la MÊME invocation du handler.
// =============================================================================

#include <stdint.h>
#include "constants.h"

// Une porte : nombre d'échecs récents et date (millis) du dernier.
struct AuthFailGate {
  uint8_t failures = 0;     // saturant à 255
  uint32_t lastFailMs = 0;  // ⚠️ seule date conservée : jamais d'échéance absolue
};

// Paramètres d'une porte (toutes les durées en ms).
struct AuthFailPolicy {
  uint8_t franchise = 0;
  uint32_t baseMs = 0;
  uint8_t maxShift = 0;
  uint32_t capMs = 0;
  uint32_t decayMs = 0;
};

inline AuthFailPolicy authFailIpPolicy() {
  AuthFailPolicy p;
  p.franchise = kAuthFailIpFranchise;
  p.baseMs = static_cast<uint32_t>(kAuthFailIpBaseMs);
  p.maxShift = kAuthFailIpMaxShift;
  p.capMs = static_cast<uint32_t>(kAuthFailIpCapMs);
  p.decayMs = static_cast<uint32_t>(kAuthFailIpDecayMs);
  return p;
}

inline AuthFailPolicy authFailGlobalPolicy() {
  AuthFailPolicy p;
  p.franchise = kAuthFailGlobalFranchise;
  p.baseMs = static_cast<uint32_t>(kAuthFailGlobalBaseMs);
  p.maxShift = kAuthFailGlobalMaxShift;
  p.capMs = static_cast<uint32_t>(kAuthFailGlobalCapMs);
  p.decayMs = static_cast<uint32_t>(kAuthFailGlobalDecayMs);
  return p;
}

// Échecs effectifs après pardon par écoulement du temps (écart NON SIGNÉ).
inline uint8_t authFailEffective(const AuthFailGate& g, uint32_t now, const AuthFailPolicy& p) {
  if (g.failures == 0) return 0;
  const uint32_t elapsed = now - g.lastFailMs;
  const uint32_t forgiven = (p.decayMs > 0) ? (elapsed / p.decayMs) : 0;
  return (forgiven >= g.failures) ? 0 : static_cast<uint8_t>(g.failures - forgiven);
}

// Attente imposée après le dernier échec, pour `eff` échecs effectifs. 0 dans la franchise.
inline uint32_t authFailWaitMs(uint8_t eff, const AuthFailPolicy& p) {
  if (eff <= p.franchise) return 0;
  uint8_t shift = static_cast<uint8_t>(eff - p.franchise - 1);
  if (shift > p.maxShift) shift = p.maxShift;
  if (shift > 31) shift = 31;  // borné AVANT le <<
  const uint64_t w = static_cast<uint64_t>(p.baseMs) << shift;
  return (w > p.capMs) ? p.capMs : static_cast<uint32_t>(w);
}

// Millisecondes restantes avant le prochain essai admis (0 = admis). <= capMs toujours.
inline uint32_t authFailRemainingMs(const AuthFailGate& g, uint32_t now, const AuthFailPolicy& p) {
  const uint32_t wait = authFailWaitMs(authFailEffective(g, now, p), p);
  if (wait == 0) return 0;
  const uint32_t elapsed = now - g.lastFailMs;
  return (elapsed < wait) ? (wait - elapsed) : 0;
}

// Enregistre un échec dans une porte (pardon appliqué d'abord, incrément saturant).
inline void authFailGateRecord(AuthFailGate& g, uint32_t now, const AuthFailPolicy& p) {
  const uint8_t eff = authFailEffective(g, now, p);
  g.failures = (eff == UINT8_MAX) ? eff : static_cast<uint8_t>(eff + 1);
  g.lastFailMs = now;
}

// État complet des trois portes.
struct AuthFailBackoff {
  struct Slot {
    bool used = false;
    uint32_t ip = 0;
    AuthFailGate gate;
  };
  struct Exempt {
    bool used = false;
    uint32_t ip = 0;
    uint32_t seq = 0;  // ordre de réussite, monotone ; ⛔ jamais une date
    AuthFailGate gate;  // SA PROPRE porte par IP
  };
  AuthFailGate global;
  Slot slots[kAuthFailIpSlots];
  Exempt exempt[kAuthFailExemptSlots];
  uint32_t seqCounter = 0;
};

// Verdict : refus immédiat + délai annoncé.
struct AuthFailVerdict {
  bool refuse = false;
  uint32_t retryAfterMs = 0;
  // Secondes annoncées (arrondi au-dessus, au moins 1 si refus).
  uint32_t retryAfterSec() const {
    if (!refuse) return 0;
    const uint32_t s = (retryAfterMs + 999u) / 1000u;  // retryAfterMs <= plafond : pas de débordement
    return (s > 0) ? s : 1u;
  }
};

namespace auth_fail_detail {
inline int findExempt(const AuthFailBackoff& s, uint32_t ip) {
  if (ip == 0) return -1;  // adresse inconnue : jamais exemptée
  for (int i = 0; i < kAuthFailExemptSlots; i++) {
    if (s.exempt[i].used && s.exempt[i].ip == ip) return i;
  }
  return -1;
}
inline int findSlot(const AuthFailBackoff& s, uint32_t ip) {
  for (int i = 0; i < kAuthFailIpSlots; i++) {
    if (s.slots[i].used && s.slots[i].ip == ip) return i;
  }
  return -1;
}
}  // namespace auth_fail_detail

// Contrôle AVANT toute comparaison. Ne modifie aucun état.
inline AuthFailVerdict authFailCheck(const AuthFailBackoff& s, uint32_t ip, uint32_t now,
                                     const AuthFailPolicy& ipPolicy, const AuthFailPolicy& globalPolicy) {
  AuthFailVerdict v;
  uint32_t wait = 0;
  const int e = auth_fail_detail::findExempt(s, ip);
  if (e >= 0) {
    // Exemptée : sa propre porte seulement, la globale ne s'applique pas.
    wait = authFailRemainingMs(s.exempt[e].gate, now, ipPolicy);
  } else {
    const int sl = auth_fail_detail::findSlot(s, ip);
    if (sl >= 0) wait = authFailRemainingMs(s.slots[sl].gate, now, ipPolicy);
    const uint32_t g = authFailRemainingMs(s.global, now, globalPolicy);
    if (g > wait) wait = g;  // le plus grand des délais applicables
  }
  v.refuse = (wait > 0);
  v.retryAfterMs = wait;
  return v;
}

// Enregistre un échec (uniquement kDenyCredentials / kDenyBadCurrent : condition 4).
inline void authFailRecordFailure(AuthFailBackoff& s, uint32_t ip, uint32_t now,
                                  const AuthFailPolicy& ipPolicy, const AuthFailPolicy& globalPolicy) {
  const int e = auth_fail_detail::findExempt(s, ip);
  if (e >= 0) {
    authFailGateRecord(s.exempt[e].gate, now, ipPolicy);  // sa porte, pas la globale
    return;
  }
  authFailGateRecord(s.global, now, globalPolicy);
  int sl = auth_fail_detail::findSlot(s, ip);
  if (sl < 0) {
    // Créneau libre, ou dont les échecs sont entièrement pardonnés (état équivalent à
    // vierge : ce n'est pas une éviction). Sinon : table pleine, la globale suffit.
    for (int i = 0; i < kAuthFailIpSlots; i++) {
      if (!s.slots[i].used || authFailEffective(s.slots[i].gate, now, ipPolicy) == 0) {
        sl = i;
        s.slots[i].used = true;
        s.slots[i].ip = ip;
        s.slots[i].gate = AuthFailGate();
        break;
      }
    }
  }
  if (sl >= 0) authFailGateRecord(s.slots[sl].gate, now, ipPolicy);
}

// Identité PROUVÉE : tout kAllow de /auth/login, ou de /auth/change-password (mot de
// passe actuel, jeton valide ou prise de possession d'un appareil vierge). Libère le créneau de l'adresse et l'inscrit dans la table d'exemption
// (la plus ancienne réussite est évincée si elle est pleine). Une ip inconnue n'est pas inscrite.
inline void authFailRecordPasswordProof(AuthFailBackoff& s, uint32_t ip) {
  const int sl = auth_fail_detail::findSlot(s, ip);
  if (sl >= 0) s.slots[sl] = AuthFailBackoff::Slot();
  if (ip == 0) return;
  int e = auth_fail_detail::findExempt(s, ip);
  if (e < 0) {
    // Entrée libre sinon la plus ancienne (écart non signé : robuste au repliement du compteur).
    int oldest = 0;
    e = -1;
    for (int i = 0; i < kAuthFailExemptSlots; i++) {
      if (!s.exempt[i].used) { e = i; break; }
      if (static_cast<uint32_t>(s.exempt[oldest].seq - s.exempt[i].seq) < 0x80000000u && i != oldest) oldest = i;
    }
    if (e < 0) e = oldest;
  }
  s.seqCounter++;
  s.exempt[e].used = true;
  s.exempt[e].ip = ip;
  s.exempt[e].seq = s.seqCounter;
  s.exempt[e].gate = AuthFailGate();  // une preuve de mot de passe remet SA porte à zéro
}

// À tout changement de mot de passe : les preuves antérieures ne valent plus.
inline void authFailClearExemptions(AuthFailBackoff& s) {
  for (int i = 0; i < kAuthFailExemptSlots; i++) s.exempt[i] = AuthFailBackoff::Exempt();
}

#endif  // AUTH_FAIL_BACKOFF_LOGIC_H
