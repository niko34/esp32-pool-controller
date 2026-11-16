#ifndef AUTH_LOG_THROTTLE_LOGIC_H
#define AUTH_LOG_THROTTLE_LOGIC_H

// =============================================================================
// auth_log_throttle_logic — bornage PUR du WARN « accès non autorisé » (bug-112)
// =============================================================================
// Décide UNIQUEMENT si une ligne de journal doit être émise. Ce module ne répond
// pas à HTTP, ne décide pas d'une authentification, n'émet rien (ADR-0017) :
// le 401, le 429 et le `return false` de checkRateLimit() partent TOUJOURS,
// quel que soit le verdict. Seul le JOURNAL est borné.
//
// MOTIF : un client sans identifiant purgeait le journal (8 192 o, 6 144 o
// conservés) en ~27 requêtes, ~13 s, à ~230 o persistés par refus.
// Borner en requêtes ne suffit pas : une requête / 60 s, chacune dans une
// fenêtre neuve, purgeait le journal en une heure. D'où le RECUL EXPONENTIEL :
//   fenêtre = base << shift ; shift +1 à chaque ligne émise (plafonné) ;
//   décroissance -1 par `decayMs` SANS tentative (jamais au drain).
// Lignes attendues par clé : t = 0, 1, 3, 7, 15, 31, 63 min ... (base 60 s).
//
// SANS ÉTAT PROPRE : chaque site déclare sa statique. Le compteur d'occultés et
// les drapeaux de masquage, partagés avec la loop (drain), vivent dans la
// coquille impure sous forme d'atomiques : ce module ne reçoit que des VALEURS.
// Zéro allocation, zéro String : <stdint.h> seul (portable en natif).
// =============================================================================

#include <stdint.h>

// État POD détenu par un site, écrit par la seule tâche async_tcp.
struct AuthLogThrottleState {
  bool everLogged;         // faux tant qu'aucune ligne émise (PAS de sentinelle 0)
  uint32_t lastWarnMs;     // date de la dernière ligne émise
  uint32_t lastAttemptMs;  // date de la dernière tentative (émise ou occultée)
  uint8_t shift;           // exposant du recul courant
  uint32_t firstUrlHash;   // clé de la dernière ligne émise (masquage de routes)
  uint32_t firstIpV4;      // idem pour l'IP
};

// Verdict d'une tentative.
struct AuthLogThrottleVerdict {
  bool emit;        // journaliser cette tentative ?
  bool otherRoute;  // (si occultée) l'URL diffère de la dernière émise
  bool otherIp;     // (si occultée) l'IP diffère de la dernière émise
};

// Hachage FNV-1a 32 bits d'une chaîne C, sans allocation. nullptr -> hash du vide.
inline uint32_t authLogThrottleHash(const char* s) {
  uint32_t h = 2166136261u;
  if (s == nullptr) return h;
  for (; *s != '\0'; ++s) {
    h ^= static_cast<uint8_t>(*s);
    h *= 16777619u;
  }
  return h;
}

// Incrément SATURANT du compteur d'occultés : jamais de repliement à 0.
inline uint16_t authLogThrottleSatInc(uint16_t v) {
  return (v == UINT16_MAX) ? v : static_cast<uint16_t>(v + 1);
}

// Fenêtre courante en ms. `maxShift` est borné à 15 (base 60 s << 15 tient
// largement dans 32 bits ; au-delà, le décalage déborderait).
inline uint32_t authLogThrottleWindowMs(uint32_t baseMs, uint8_t shift, uint8_t maxShift) {
  if (maxShift > 15) maxShift = 15;
  if (shift > maxShift) shift = maxShift;
  return baseMs << shift;
}

// Une tentative arrive. Met à jour l'état et rend le verdict.
// Toutes les comparaisons temporelles sont des soustractions NON SIGNÉES
// (immunité au repliement de millis() à 49,7 j).
inline AuthLogThrottleVerdict authLogThrottleStep(AuthLogThrottleState& s,
                                                  uint32_t nowMs,
                                                  uint32_t urlHash,
                                                  uint32_t ipV4,
                                                  uint32_t baseMs,
                                                  uint8_t maxShift,
                                                  uint32_t decayMs) {
  // Décroissance : -1 par `decayMs` écoulé depuis la dernière tentative.
  if (s.shift > 0 && decayMs > 0) {
    const uint32_t steps = (nowMs - s.lastAttemptMs) / decayMs;
    s.shift = (steps >= s.shift) ? 0 : static_cast<uint8_t>(s.shift - steps);
  }
  s.lastAttemptMs = nowMs;

  AuthLogThrottleVerdict v = {false, false, false};
  const bool due = !s.everLogged ||
                   (nowMs - s.lastWarnMs) >= authLogThrottleWindowMs(baseMs, s.shift, maxShift);
  if (!due) {
    v.otherRoute = (urlHash != s.firstUrlHash);
    v.otherIp = (ipV4 != s.firstIpV4);
    return v;
  }

  // Émission. La 1re ligne ne relève pas le recul : la fenêtre suivante reste la
  // base, d'où la séquence 0, 1, 3, 7, 15 ... min.
  if (s.everLogged && s.shift < maxShift) s.shift++;
  s.everLogged = true;
  s.lastWarnMs = nowMs;
  s.firstUrlHash = urlHash;
  s.firstIpV4 = ipV4;
  v.emit = true;
  return v;
}

// Écart temporel NON SIGNÉ, rejeté s'il est « négatif » : une tentative intercalée
// entre la lecture de millis() et celle de la date publiée donne un écart voisin de
// 2^32, qui ne doit jamais passer pour un long silence (course async_tcp / loop).
inline bool authLogThrottleElapsedAtLeast(uint32_t nowMs, uint32_t thenMs, uint32_t minMs) {
  const uint32_t d = nowMs - thenMs;
  return d < 0x80000000u && d >= minMs;
}

// Drain (loop) : faut-il annoncer les occultés ? Silence de `quietMs` depuis la
// dernière tentative ET au moins un occulté. Ne modifie AUCUN état du throttle.
// (Sans borne de recul : voir authLogThrottleDrainDueBounded, seule utilisée en
// production — celle-ci reste le prédicat de silence.)
inline bool authLogThrottleDrainDue(uint16_t suppressed, uint32_t nowMs,
                                    uint32_t lastAttemptMs, uint32_t quietMs) {
  return suppressed > 0 && authLogThrottleElapsedAtLeast(nowMs, lastAttemptMs, quietMs);
}

// Drain BORNÉ par le MÊME recul que l'émission (bug-112 P2/G1) : en plus du silence,
// la fenêtre `base << shift` doit être écoulée depuis la dernière ligne (`lastWarnMs`,
// `shift` publiés par async_tcp). Sans cela le drain serait un émetteur neuf, hors
// recul, atteignable sans identifiants.
inline bool authLogThrottleDrainDueBounded(uint16_t suppressed, uint32_t nowMs,
                                           uint32_t lastAttemptMs, uint32_t quietMs,
                                           uint32_t lastWarnMs, uint8_t shift,
                                           uint32_t baseMs, uint8_t maxShift) {
  return authLogThrottleDrainDue(suppressed, nowMs, lastAttemptMs, quietMs) &&
         authLogThrottleElapsedAtLeast(nowMs, lastWarnMs, authLogThrottleWindowMs(baseMs, shift, maxShift));
}

// Un drain émis COMPTE comme une ligne : il consomme la fenêtre et relève le recul,
// exactement comme une émission ordinaire. Appliqué par async_tcp (seul écrivain de
// l'état) à la tentative suivante — le drain, lui, n'écrit jamais l'état.
// `everLogged` n'est pas modifié (un drain ne suppose qu'un occulté, donc déjà vrai).
inline void authLogThrottleFoldDrain(AuthLogThrottleState& s, uint32_t drainedAtMs, uint8_t maxShift) {
  if (!s.everLogged) return;
  s.lastWarnMs = drainedAtMs;
  if (s.shift < maxShift) s.shift++;
}

// Accord français : « 1 refus non journalisé » / « 2 refus non journalisés ».
inline const char* authLogThrottlePlural(uint16_t n, const char* singular, const char* plural) {
  return n > 1 ? plural : singular;
}

#endif  // AUTH_LOG_THROTTLE_LOGIC_H
