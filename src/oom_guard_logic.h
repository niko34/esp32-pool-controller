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
//   - décision de réarmement (hystérésis kOomRearmMinMaxAllocBytes) et choix du
//     palier nominal/repli (oomRearmTargetBytes, v2.18.8) ;
//   - one-shot du log CRITICAL différé (drainé depuis la loop) ;
//   - encodage/décodage/validation/verdict du post-mortem d'allocation RTC
//     (32 octets, v2.18.8) et cadence du contrôle d'intégrité du tas ;
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

// ---------------------------------------------------------------------------
// Réarmement à DEUX PALIERS (bug-parachute-oom-jamais-rearme, v2.18.8)
// ---------------------------------------------------------------------------
// Défaut n°1 confirmé le 13/08/2026 : avec un seuil unique de 32 Ko alors que le
// plus gros bloc réel vaut 27-30 Ko, la réserve n'était JAMAIS ré-allouée après
// sa première consommation (parachute one-shot). Deux paliers désormais :
//   - NOMINAL (kOomReserveBytes / kOomRearmMinMaxAllocBytes) ;
//   - REPLI, plus petit (kOomFallbackReserveBytes /
//     kOomFallbackRearmMinMaxAllocBytes) : « toujours armé » vaut mieux que
//     « jamais réarmé ».
// `armedBytes` = taille de la réserve ACTUELLEMENT armée (0 = aucune réserve —
// l'appelant DOIT la dériver du POINTEUR de réserve, seule autorité, cf.
// oomShouldRearm). Retour = taille à allouer, 0 = NE RIEN FAIRE.
// Le surclassement repli → nominal est autorisé (une réserve de 2 Ko est
// remplacée par 12 Ko dès que le tas le permet) ; le déclassement nominal →
// repli est INTERDIT (jamais d'oscillation, jamais de dégradation silencieuse).
inline uint32_t oomRearmTargetBytes(uint32_t armedBytes, uint32_t maxAllocBytes,
                                   uint32_t nominalBytes, uint32_t nominalMinMaxAlloc,
                                   uint32_t fallbackBytes, uint32_t fallbackMinMaxAlloc) {
  if (armedBytes >= nominalBytes) return 0;  // déjà au meilleur palier : rien à faire
  if (maxAllocBytes >= nominalMinMaxAlloc) return nominalBytes;  // (ré)armement ou surclassement
  if (armedBytes > 0) return 0;              // repli déjà armé et tas insuffisant pour le nominal
  if (maxAllocBytes >= fallbackMinMaxAlloc) return fallbackBytes;  // aucune réserve → palier de repli
  return 0;                                  // tas trop dégradé, même pour le repli
}

// One-shot du log CRITICAL « réserve OOM consommée » : émis en DIFFÉRÉ depuis
// la loop, une seule fois par épisode (comparaison compteur d'épisodes vs
// compteur déjà journalisé — les compteurs ne décroissent jamais).
inline bool oomShouldLogRelease(uint32_t releaseCount, uint32_t loggedCount) {
  return releaseCount != loggedCount;
}

// ===========================================================================
// POST-MORTEM D'ALLOCATION (bug-parachute-oom-jamais-rearme, v2.18.8)
// ===========================================================================
// Défaut n°2 du 13/08 : le CRITICAL « réserve OOM consommée » est émis en
// DIFFÉRÉ par la loop → perdu quand l'abort suit immédiatement, et le journal
// persistant d'avant crash est perdu aussi (flush toutes les 10 min). Seule la
// RAM RTC noinit survit : on y enregistre l'état AU MOMENT EXACT de l'échec
// depuis le callback heap_caps / le new_handler (stores bruts uniquement, cf.
// oom_guard.cpp), et on le dumpe au boot suivant.
//
// FORMAT DE LA ZONE (32 octets, little-endian) :
//   [0..3]   u32 magic          — kOomPostMortemMagic (0xF00DFA12)
//   [4..7]   u32 failCount      — échecs d'allocation cumulés du boot
//   [8..11]  u32 firstSize      — taille demandée au 1er échec (octets)
//   [12..15] u32 lastSize       — taille demandée au dernier échec (octets)
//   [16..19] u32 lastCaps       — masque heap_caps du dernier échec
//   [20..21] u16 firstUptimeS   — uptime (s) du 1er échec, modulo 65536
//   [22..23] u16 lastUptimeS    — uptime (s) du dernier échec, modulo 65536
//   [24..25] u16 ringFree >> 8  — heap libre 8 bits du dernier échantillon de l'anneau (quantum 256 o)
//   [26..27] u16 ringMaxB >> 6  — plus gros bloc 8 bits du dernier échantillon (quantum 64 o)
//   [28..29] u16 ringUptimeS    — uptime (s) de cet échantillon (→ âge = last - ring)
//   [30]     u8  flags          — bits kOomPmFlag* (réserve consommée, handler, intégrité)
//   [31]     u8  releaseCount   — épisodes de libération de la réserve (saturé 255)
// Les deux champs « ring » proviennent d'un miroir atomique de l'anneau heap
// (valeurs DÉJÀ mesurées à la cadence 2 s) : le chemin d'échec ne mesure JAMAIS
// le tas lui-même (interdiction absolue de réentrer dans l'allocateur).

// MAGIC BUMPÉ EN v2.19.0 (0xF00DFA11 → 0xF00DFA12) : les champs « ring » de la
// zone portent désormais la mesure 8 bits (mémoire réellement allouable). Une
// zone écrite par un firmware 2.18.x contient l'ANCIENNE métrique (IRAM 32 bits
// comprise) ; la relire avec les nouveaux libellés (« free8= », « maxB8= »)
// afficherait un chiffre faux et crédible. Perdre le post-mortem du boot de
// bascule est acceptable ; afficher une valeur trompeuse ne l'est pas.
constexpr uint32_t kOomPostMortemMagic = 0xF00DFA12u;  // DISTINCT de l'anneau (0xB10C4EAA) et de l'auto-heal (0xA5C0FFEE)
constexpr uint32_t kOomPostMortemBytes = 32;           // Taille de la zone RTC noinit

// Drapeaux (octet 30) — posés au plus tôt pour survivre à un abort immédiat.
constexpr uint8_t kOomPmFlagNominalUsed        = 0x01;  // bit0 : réserve NOMINALE consommée
constexpr uint8_t kOomPmFlagFallbackUsed       = 0x02;  // bit1 : réserve de REPLI consommée
constexpr uint8_t kOomPmFlagHandlerUnregistered = 0x04; // bit2 : new_handler désenregistré (bad_alloc laissé suivre)
constexpr uint8_t kOomPmFlagIntegrityFailed    = 0x08;  // bit3 : contrôle d'intégrité du tas ÉCHOUÉ
constexpr uint8_t kOomPmFlagIntegrityNeverRun  = 0x10;  // bit4 : contrôle d'intégrité jamais exécuté

// Plafonds de plausibilité du décodage (RAM RTC potentiellement corrompue).
constexpr uint32_t kOomPmMaxSizeBytes = 4194304u;  // 4 Mio : au-delà, champ insensé
constexpr uint32_t kOomPmMaxFailCount = 1000000u;  // 1e6 échecs : au-delà, champ insensé

struct OomPostMortemFields {
  uint32_t magic;
  uint32_t failCount;
  uint32_t firstSize;
  uint32_t lastSize;
  uint32_t lastCaps;
  uint16_t firstUptimeS;
  uint16_t lastUptimeS;
  uint32_t ringFreeBytes;      // Reconstruit (quantum 256 o)
  uint32_t ringMaxAllocBytes;  // Reconstruit (quantum 64 o)
  uint16_t ringUptimeS;
  uint8_t  flags;
  uint8_t  releaseCount;
};

// Encode la zone dans `out` (kOomPostMortemBytes octets). Saturations : tailles
// et compteurs plafonnés à leur dynamique, uptimes tronqués modulo 65536.
inline void oomPostMortemEncode(uint8_t* out, const OomPostMortemFields& f) {
  uint32_t rf = f.ringFreeBytes >> 8;
  if (rf > 0xFFFFu) rf = 0xFFFFu;
  uint32_t rm = f.ringMaxAllocBytes >> 6;
  if (rm > 0xFFFFu) rm = 0xFFFFu;
  const uint32_t words[] = {f.magic, f.failCount, f.firstSize, f.lastSize, f.lastCaps};
  for (uint32_t w = 0; w < 5u; w++) {
    out[w * 4 + 0] = (uint8_t)(words[w] & 0xFFu);
    out[w * 4 + 1] = (uint8_t)((words[w] >> 8) & 0xFFu);
    out[w * 4 + 2] = (uint8_t)((words[w] >> 16) & 0xFFu);
    out[w * 4 + 3] = (uint8_t)((words[w] >> 24) & 0xFFu);
  }
  out[20] = (uint8_t)(f.firstUptimeS & 0xFFu);
  out[21] = (uint8_t)(f.firstUptimeS >> 8);
  out[22] = (uint8_t)(f.lastUptimeS & 0xFFu);
  out[23] = (uint8_t)(f.lastUptimeS >> 8);
  out[24] = (uint8_t)(rf & 0xFFu);
  out[25] = (uint8_t)((rf >> 8) & 0xFFu);
  out[26] = (uint8_t)(rm & 0xFFu);
  out[27] = (uint8_t)((rm >> 8) & 0xFFu);
  out[28] = (uint8_t)(f.ringUptimeS & 0xFFu);
  out[29] = (uint8_t)(f.ringUptimeS >> 8);
  out[30] = f.flags;
  out[31] = f.releaseCount;
}

// Décode la zone depuis `in` (kOomPostMortemBytes octets). Aucune validation
// ici : voir oomPostMortemValid().
inline OomPostMortemFields oomPostMortemDecode(const uint8_t* in) {
  OomPostMortemFields f;
  uint32_t words[5];
  for (uint32_t w = 0; w < 5u; w++) {
    words[w] = (uint32_t)in[w * 4 + 0] | ((uint32_t)in[w * 4 + 1] << 8) |
               ((uint32_t)in[w * 4 + 2] << 16) | ((uint32_t)in[w * 4 + 3] << 24);
  }
  f.magic = words[0];
  f.failCount = words[1];
  f.firstSize = words[2];
  f.lastSize = words[3];
  f.lastCaps = words[4];
  f.firstUptimeS = (uint16_t)(in[20] | ((uint16_t)in[21] << 8));
  f.lastUptimeS = (uint16_t)(in[22] | ((uint16_t)in[23] << 8));
  f.ringFreeBytes = ((uint32_t)(in[24] | ((uint32_t)in[25] << 8))) << 8;
  f.ringMaxAllocBytes = ((uint32_t)(in[26] | ((uint32_t)in[27] << 8))) << 6;
  f.ringUptimeS = (uint16_t)(in[28] | ((uint16_t)in[29] << 8));
  f.flags = in[30];
  f.releaseCount = in[31];
  return f;
}

// Plausibilité de la zone décodée (la RAM RTC n'est pas initialisée au power-on
// et peut être corrompue) : magic EXACT, uptimes cohérents (le dernier échec ne
// précède pas le premier), tailles ≤ 4 Mio, compteur d'échecs ≤ 1e6.
inline bool oomPostMortemValid(const OomPostMortemFields& f) {
  if (f.magic != kOomPostMortemMagic) return false;
  // PAS de contrôle « lastUptimeS >= firstUptimeS » (revue v2.18.8) : les
  // uptimes sont tronqués sur 16 bits (repliement toutes les 18 h 12), or
  // l'appareil tourne des JOURS. Un premier échec bénin situé avant un
  // repliement rendrait la zone « invalide » et le post-mortem ne serait
  // JAMAIS émis — précisément au moment où il sert. Les durées affichées
  // utilisent l'arithmétique modulo (oomPostMortemSpanS), qui reste correcte.
  if (f.firstSize > kOomPmMaxSizeBytes || f.lastSize > kOomPmMaxSizeBytes) return false;
  if (f.failCount > kOomPmMaxFailCount) return false;
  return true;
}

// Durée écoulée entre le PREMIER et le DERNIER échec, en secondes, en
// arithmétique modulo 16 bits (les uptimes se replient toutes les 18 h 12).
inline uint16_t oomPostMortemSpanS(const OomPostMortemFields& f) {
  return (uint16_t)(f.lastUptimeS - f.firstUptimeS);
}

// Verdict d'attribution du crash.
enum : uint8_t {
  kOomVerdictUndetermined = 0,         // Pas assez d'éléments (aucun échec, pas d'échantillon d'anneau)
  kOomVerdictStarvation = 1,           // Famine mémoire réelle (le tas était bien à sec)
  kOomVerdictCorruptionSuspected = 2,  // Comptabilité saine mais l'allocateur refuse → corruption suspectée
};

// Départage famine (a) et corruption (b) à partir de la zone décodée.
// RAISONNEMENT : une famine capable de refuser une PETITE allocation refuse
// TOUT — elle se traduit par de NOMBREUX échecs (> maxFails) et/ou par une
// comptabilité écroulée (plus gros bloc du même ordre que la taille demandée).
// À l'inverse, 1-2 échecs isolés alors que l'anneau annonce un plus gros bloc
// ≥ saneBlockRatio × la taille demandée est incohérent avec une famine (cas du
// 13/08 : 156 o refusés avec maxB=27 584 o annoncé, soit 176×) → corruption
// suspectée.
inline uint8_t oomPostMortemVerdict(const OomPostMortemFields& f, uint32_t maxFails,
                                    uint32_t saneBlockRatio, uint16_t burstSpanS) {
  if (f.failCount == 0 || f.lastSize == 0) return kOomVerdictUndetermined;
  // Aucun échantillon d'anneau (boot très précoce) : rien à confronter.
  if (f.ringMaxAllocBytes == 0 && f.ringFreeBytes == 0) return kOomVerdictUndetermined;
  // Famine sur le NOMBRE d'échecs : uniquement s'ils forment une RAFALE (revue
  // v2.18.8). Le compteur est cumulé sur tout le boot : sans fenêtre, quelques
  // échecs bénins étalés sur des heures concluraient « famine » sur le scénario
  // même que ce verdict doit trancher (13/08 : 2 échecs, tas sain).
  if (f.failCount > maxFails && oomPostMortemSpanS(f) <= burstSpanS) {
    return kOomVerdictStarvation;
  }
  // Division plutôt que multiplication : pas de débordement sur lastSize géant.
  bool saneAccounting = (saneBlockRatio == 0) ||
                        (f.ringMaxAllocBytes / saneBlockRatio >= f.lastSize);
  if (!saneAccounting) return kOomVerdictStarvation;
  return kOomVerdictCorruptionSuspected;
}

// ---------------------------------------------------------------------------
// Contrôle d'intégrité périodique du tas — cadence + autolimitation (C3)
// ---------------------------------------------------------------------------
// Cadence lente : premier passage dès que l'appelant est éligible (`everRun`
// faux), puis toutes les `intervalMs`. `enabled` porte la désactivation
// définitive (durée excessive ou corruption déjà détectée).
inline bool heapIntegrityShouldRun(bool enabled, bool everRun, uint32_t nowMs,
                                   uint32_t lastRunMs, uint32_t intervalMs) {
  if (!enabled) return false;
  if (!everRun) return true;
  return (uint32_t)(nowMs - lastRunMs) >= intervalMs;
}

// Autolimitation : le contrôle n'est réadmis que si son parcours est resté sous
// le plafond (kHeapIntegrityMaxDurationUs). Un tas devenu si gros/fragmenté que
// le parcours dure plus longtemps ne doit PAS être re-parcouru (gigue watchdog
// et régulation) — désactivation définitive côté appelant.
inline bool heapIntegrityAdmitNextRun(uint32_t durationUs, uint32_t maxDurationUs) {
  return durationUs <= maxDurationUs;
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
