#ifndef DOSING_WINDOW_LOGIC_H
#define DOSING_WINDOW_LOGIC_H

// =============================================================================
// dosing_window_logic — Survie du budget horaire de dosage au redémarrage,
// logique PURE (feature-136, gate pool-chemistry du 2026-10-08, GO sous 12
// conditions)
// =============================================================================
// Patron ADR-0017 (ph_persist_logic.h / measure_validity_logic.h) : ni
// millis(), ni time(), ni NVS, ni Arduino.h — le temps et l'heure sont des
// PARAMÈTRES ; compilable en natif ; constants.h seule dépendance projet.
//
// CE QUE CE MODULE DÉCIDE
// ------------------------
// Deux supports redondants couvrent le même périmètre (usedMs/windowStart,
// cyclesToday/cyclesDayStart, pause mélange, ring anti-rafale) :
//   - T2 (RAM RTC, RTC_NOINIT_ATTR) : survit à un reset logiciel/panic/WDT,
//     PAS à une coupure secteur. L'écoulé se mesure par un compteur MONOTONE
//     (voir dosing_window.cpp — esp_rtc_get_time_us(), condition C7), SANS
//     dépendre de l'horloge murale.
//   - T1 (NVS, namespace `pool-dose`) : ancrée sur l'epoch murale, survit à
//     tout, mais exige une heure valide pour être interprétée.
// Quand les deux sont exploitables, c'est le PLUS DÉFAVORABLE qui est retenu
// (usedMs max, cyclesToday max, pause mélange la plus longue) — jamais la
// moyenne, jamais le plus récent (gate pool-chemistry).
// Quand aucune des deux n'est exploitable : T3, fail-closed option (d) —
// usedMs posé à la limite pour UNE SEULE fenêtre, puis retour au nominal.
//
// RÉPARTITION
//   - dosing_window_logic.h (ce fichier) : TOUTES les décisions.
//   - dosing_window.cpp : RTC_NOINIT, NVS (Preferences), esp_reset_reason(),
//     esp_rtc_get_time_us(), journalisation.
//   - pump_controller.cpp : le câblage — fournit l'état courant, applique le
//     verdict à `DosingState` au boot (applyRestoredDosingWindow()).
// =============================================================================

#include <stdint.h>
#include <stddef.h>

#include "constants.h"  // kDosingWindow*, kDosingCycleHistorySize, kPhMixingDelayMs...

// -----------------------------------------------------------------------------
// T2 — INSTANTANÉ RAM RTC (RTC_NOINIT_ATTR)
// -----------------------------------------------------------------------------
// AUCUN INITIALISEUR DE MEMBRE (piège déjà documenté sur TttLive,
// time_to_target_logic.h:124) : un initialiseur rendrait la structure
// dynamiquement initialisée au démarrage, et la RAM RTC serait réécrite à
// CHAQUE reset — l'inverse exact du but de RTC_NOINIT_ATTR.
//
// 220 octets (condition C9 — le plan annonçait 48 à tort ; le ring seul
// pèse 160 octets, pas 80 : `uint32_t[2][kDosingCycleHistorySize]`). Recommandé
// par le gate : le ring ne vit QU'ICI, jamais en NVS (T1 n'en porte pas —
// usure et taille, sans bénéfice chimique : une rafale redevient inoffensive
// en 15 min quoi qu'il arrive).
struct DosingWindowRtcRecord {
  uint32_t magic;
  uint32_t version;                 // kDosingWindowRtcSchema — rejet si différent
  uint32_t usedMs[2];                // [0]=pH [1]=ORP — budget horaire consommé (ms)
  uint32_t windowElapsedMs[2];       // ms écoulés DANS la fenêtre horaire À L'ÉCRITURE
  uint32_t cyclesDayElapsedMs[2];    // ms écoulés depuis cyclesDayStart À L'ÉCRITURE
  uint32_t mixingRemainingMs[2];     // ms restants de pause mélange À L'ÉCRITURE (0=inactif)
  uint32_t rtcCounterAtWriteUsLo;    // esp_rtc_get_time_us() à l'écriture (C7), 32 bits bas
  uint32_t rtcCounterAtWriteUsHi;    // ... 32 bits hauts (pas de uint64_t : alignement explicite)
  uint32_t ringAgeMs[2][kDosingCycleHistorySize];  // âge (ms) de chaque entrée à l'écriture
  uint8_t  cyclesToday[2];
  uint8_t  pumpOnAtWrite[2];         // C5 : pompe énergisée À L'INSTANT de l'écriture
  uint8_t  ringIdx[2];               // index d'écriture courant du ring
  uint8_t  reserved[2];              // 0 — complète à un multiple de 4 avant le CRC
  uint32_t crc;                      // FNV-1a des octets précédents
};
static_assert(sizeof(DosingWindowRtcRecord) == 220,
              "DosingWindowRtcRecord : 220 octets attendus (schéma courant) — "
              "tout changement de taille exige d'incrémenter kDosingWindowRtcSchema");

// Sentinelle d'un slot de ring VIDE à l'écriture (jamais utilisé) — très
// supérieure à kDosingWindowRingDiscardMs, donc toujours écartée au restore
// quel que soit l'écoulé ajouté (condition C6).
constexpr uint32_t kDosingWindowRingEmptySentinel = 0xFFFFFFFFu;

// -----------------------------------------------------------------------------
// T1 — INSTANTANÉ NVS (namespace `pool-dose`)
// -----------------------------------------------------------------------------
// STRUCTURELLEMENT SYMÉTRIQUE à T2 (mêmes grandeurs « élapsé à l'écriture » +
// UNE ancre unique d'écoulé réel) plutôt qu'ancrée directement sur deux epochs
// indépendants : `epochAtWrite` est LA seule ancre murale, exactement comme
// `rtcCounterAtWriteUs*` l'est pour T2 — la même durée de coupure
// (epochNow − epochAtWrite) s'applique alors IDENTIQUEMENT à la fenêtre
// horaire ET au jour des cycles, sans dupliquer le calcul d'écoulé par anchor.
// Pas de ring (recommandation C9 du gate — aucun bénéfice chimique, coût NVS
// superflu : une rafale redevient inoffensive en 15 min quoi qu'il arrive).
struct DosingWindowNvsRecord {
  uint32_t magic                         = 0;
  uint32_t version                       = 0;         // kDosingWindowNvsSchema
  uint32_t usedMs[2]                     = {0, 0};
  uint32_t windowElapsedAtWriteMs[2]     = {0, 0};     // ms écoulés dans la fenêtre À L'ÉCRITURE
  uint32_t cyclesDayElapsedAtWriteMs[2]  = {0, 0};     // ms écoulés depuis cyclesDayStart À L'ÉCRITURE
  uint32_t mixingRemainingMs[2]          = {0, 0};     // ms restants de pause mélange À L'ÉCRITURE (0=inactif)
  uint32_t epochAtWrite                  = 0;          // heure murale (s) À L'ÉCRITURE — ancre UNIQUE
  uint8_t  cyclesToday[2]                = {0, 0};
  uint8_t  pumpOnAtWrite[2]              = {0, 0};     // C5
  uint8_t  reserved[4]                   = {0, 0, 0, 0};  // 0 — multiple de 4 avant le CRC
  uint32_t crc                           = 0;
};
static_assert(sizeof(DosingWindowNvsRecord) == 56,
              "DosingWindowNvsRecord : 56 octets attendus (schéma courant) — "
              "tout changement de taille exige d'incrémenter kDosingWindowNvsSchema");

// -----------------------------------------------------------------------------
// FNV-1a 32 bits (même algorithme que ph_persist_logic / measure_validity_logic
// / time_to_target_logic — chaque module pur du projet porte sa propre copie,
// patron établi : ~30 octets de flash contre le couplage d'un en-tête partagé).
// -----------------------------------------------------------------------------
inline uint32_t dosingWindowFnv1a(const void* data, uint32_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t h = 2166136261UL;
  for (uint32_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

inline uint32_t dosingWindowRtcCrc(const DosingWindowRtcRecord& r) {
  return dosingWindowFnv1a(&r, (uint32_t)offsetof(DosingWindowRtcRecord, crc));
}
inline void dosingWindowRtcSeal(DosingWindowRtcRecord& r) {
  r.reserved[0] = 0;
  r.reserved[1] = 0;
  r.crc = dosingWindowRtcCrc(r);
}
// Taille EXACTE requise (condition C9 : rejet sur taille exacte, jamais de
// réinterprétation partielle) + empreinte concordante.
inline bool dosingWindowRtcIntact(const DosingWindowRtcRecord& r, size_t bytesRead) {
  if (bytesRead != sizeof(DosingWindowRtcRecord)) return false;
  // ⚠️ Revue 2026-10-08 : la magie était ÉCRITE mais jamais COMPARÉE, alors que
  // constants.h l'annonce comme « premier filtre bon marché ». Le FNV-1a couvre
  // la magie, donc un enregistrement scellé par un AUTRE module de la RAM RTC —
  // elle est partagée par heap_ring, oom_guard, time_to_target, measure_validity
  // et l'auto-guérison — resterait AUTO-COHÉRENT et passerait le CRC. La magie
  // est ce qui distingue « notre structure » de « une structure valide d'autrui ».
  if (r.magic != kDosingWindowRtcMagic) return false;
  if (r.version != kDosingWindowRtcSchema) return false;
  return dosingWindowRtcCrc(r) == r.crc;
}

inline uint32_t dosingWindowNvsCrc(const DosingWindowNvsRecord& r) {
  return dosingWindowFnv1a(&r, (uint32_t)offsetof(DosingWindowNvsRecord, crc));
}
inline void dosingWindowNvsSeal(DosingWindowNvsRecord& r) {
  for (uint8_t& b : r.reserved) b = 0;
  r.crc = dosingWindowNvsCrc(r);
}
inline bool dosingWindowNvsIntact(const DosingWindowNvsRecord& r, size_t bytesRead) {
  if (bytesRead != sizeof(DosingWindowNvsRecord)) return false;
  // Même motif que pour la RAM RTC ci-dessus (revue 2026-10-08).
  if (r.magic != kDosingWindowNvsMagic) return false;
  if (r.version != kDosingWindowNvsSchema) return false;
  return dosingWindowNvsCrc(r) == r.crc;
}

// -----------------------------------------------------------------------------
// C2/C3 — REBASAGE D'UNE ANCRE TEMPORELLE À TRAVERS UNE COUPURE
// -----------------------------------------------------------------------------
// AUCUN millis() brut n'est jamais persisté : seules des DURÉES (ms écoulés
// dans la fenêtre à l'écriture, ms écoulés pendant la coupure) traversent le
// reboot. Ce helper reconstruit une ancre dans le domaine millis() COURANT.
//
// Formule exacte (condition C3) : si la coupure couvre le reste de la période
// (outageElapsedMs >= periodMs - elapsedAtWriteMs), la fenêtre/le jour est
// PÉRIMÉ(E) → neuf. Sinon l'ancre est reconstruite par SOUSTRACTION NON
// SIGNÉE — le sous-débordement est LÉGITIME (cf. pump_controller.cpp,
// commentaire C2/C3 du restore) : `now - elapsedAtWrite - outage` peut très
// bien déborder sous 0 et revenir par le haut (arithmétique uint32_t), le
// calcul `now2 - anchor` restera exact à travers ce repliement.
//
// `unknownElapsed` (C4/C7) : l'écoulé de la coupure est INCALCULABLE (horloge
// invalide pour T1, compteur monotone n'ayant pas survécu pour T2) mais
// l'instantané lui-même reste intact. On NE BASCULE PAS en péremption : la
// valeur est restaurée INTÉGRALEMENT, l'ancre redevient `now` (une fenêtre
// pleine recommence, conservateur — jamais un budget neuf gratuit).
struct DosingWindowAnchorRebase {
  bool resetToFresh = true;   // true : période considérée neuve (valeur → 0)
  uint32_t anchorMs = 0;      // valide seulement si !resetToFresh
};

inline DosingWindowAnchorRebase dosingWindowRebaseAnchor(uint32_t elapsedAtWriteMs,
                                                         uint32_t outageElapsedMs,
                                                         bool unknownElapsed,
                                                         uint32_t periodMs,
                                                         uint32_t nowMs) {
  DosingWindowAnchorRebase r;
  if (unknownElapsed) {
    r.resetToFresh = false;
    r.anchorMs = (nowMs != 0) ? nowMs : 1u;  // collision de sentinelle (0 = "jamais ouverte")
    return r;
  }
  // elapsedAtWriteMs est garanti < periodMs par construction de l'appelant
  // (sinon la fenêtre aurait déjà tourné AVANT l'écriture).
  const uint32_t remaining = (elapsedAtWriteMs < periodMs) ? (periodMs - elapsedAtWriteMs) : 0u;
  if (outageElapsedMs >= remaining) {
    r.resetToFresh = true;
    r.anchorMs = 0;
    return r;
  }
  r.resetToFresh = false;
  const uint32_t anchor = nowMs - elapsedAtWriteMs - outageElapsedMs;  // sous-débordement légitime
  r.anchorMs = (anchor != 0) ? anchor : 1u;  // collision de sentinelle
  return r;
}

// -----------------------------------------------------------------------------
// C6 — RING ANTI-RAFALE : rebasage d'UNE entrée
// -----------------------------------------------------------------------------
// Renvoie 0 (= slot vide, convention de recordCycleTimestamp/countCyclesInWindow,
// dosing_logic.cpp) si l'entrée est devenue plus vieille que la fenêtre de
// 15 min (kDosingWindowRingDiscardMs) — qu'elle l'ait été dès l'écriture ou à
// cause de la coupure. `unknownElapsed` (C6) : toute entrée QUI EXISTAIT
// (slot non vide à l'écriture) est conservée à l'ÂGE 0 — le plus défavorable,
// il se résout de lui-même en 15 min. Un slot VIDE à l'écriture
// (kDosingWindowRingEmptySentinel) reste vide dans tous les cas : un écoulé
// inconnu ne fabrique jamais un cycle qui n'a pas eu lieu.
inline uint32_t dosingWindowRestoreRingEntry(uint32_t ageAtWriteMs, uint32_t outageElapsedMs,
                                             bool unknownElapsed, uint32_t nowMs) {
  if (ageAtWriteMs == kDosingWindowRingEmptySentinel) return 0u;  // vide, le reste à vie
  uint32_t age;
  if (unknownElapsed) {
    age = 0u;  // C6 : pire cas, indépendant de ageAtWriteMs (slot NON vide)
  } else {
    const uint32_t sum = ageAtWriteMs + outageElapsedMs;
    age = (sum < ageAtWriteMs) ? 0xFFFFFFFFu : sum;  // saturation anti-overflow (défensif)
  }
  if (age >= kDosingWindowRingDiscardMs) return 0u;  // > 15 min, écarté (slot vide)
  const uint32_t ts = nowMs - age;
  return (ts != 0u) ? ts : 1u;  // collision de sentinelle (0 = slot vide)
}

// -----------------------------------------------------------------------------
// FUSION T1/T2 — LA DÉCISION COMPLÈTE D'UNE VOIE (pH ou ORP)
// -----------------------------------------------------------------------------

// Source retenue (condition C11 — journalisée telle quelle).
enum class DosingWindowSource : uint8_t {
  kFailClosed = 0,  // T3 : ni T1 ni T2 exploitable
  kRtc        = 1,  // T2 seul exploitable
  kNvs        = 2,  // T1 seul exploitable
  kWorst      = 3,  // les deux exploitables : le plus défavorable retenu
};

// Entrées PURES d'UNE source (T1 ou T2) pour UNE voie, déjà normalisées par la
// coquille (lecture RTC_NOINIT / NVS, calcul de l'écoulé). `usedMs` porte déjà
// le forfait conservateur T1 (+kDosingWindowPersistIntervalMs si pompe-ON à
// l'écriture, écrêté à l'heure — cf. dosing_window.cpp) : ce module n'a pas à
// le recalculer.
struct DosingWindowSourceInputs {
  bool available = false;
  uint32_t usedMs = 0;
  uint32_t windowElapsedAtWriteMs = 0;      // 0 pour T1 (ancre absolue epoch)
  uint32_t cyclesDayElapsedAtWriteMs = 0;   // 0 pour T1
  uint32_t outageElapsedMs = 0;             // ignoré si unknownElapsed
  bool unknownElapsed = false;
  uint16_t cyclesToday = 0;
  uint32_t mixingRemainingMs = 0;
  bool pumpOnAtWrite = false;               // C5
};

struct DosingWindowChannelVerdict {
  uint32_t usedMs = 0;
  uint32_t windowStart = 0;      // ancre, domaine millis() COURANT (jamais 0)
  uint16_t cyclesToday = 0;
  uint32_t cyclesDayStart = 0;   // ancre, domaine millis() COURANT (jamais 0)
  uint32_t mixingEndMs = 0;      // 0 = inactif ; sinon domaine millis() COURANT
  DosingWindowSource source = DosingWindowSource::kFailClosed;
};

/**
 * @brief Résout l'état restauré d'UNE voie (pH ou ORP) à partir des deux
 *        sources (T2 RAM RTC, T1 NVS), déjà normalisées par la coquille.
 *
 * ORDRE DES RÈGLES (gate pool-chemistry, 2026-10-08) :
 *   - aucune source exploitable → T3, fail-closed option (d) : `usedMs` posé à
 *     la limite d'UNE SEULE fenêtre (kDosingWindowHourMs), `cyclesToday` à 0
 *     (l'anti-cycling n'est pas le risque gardé ici — hors périmètre du gate) ;
 *   - une seule source exploitable → elle fait foi ;
 *   - les deux exploitables → LE PLUS DÉFAVORABLE par grandeur (`usedMs`,
 *     `cyclesToday`, pause mélange), jamais la moyenne ni la plus récente.
 *     L'ancre (`windowStart`/`cyclesDayStart`) suit la source qui a fourni la
 *     grandeur retenue — mélanger l'ancre d'une source avec la valeur de
 *     l'autre produirait une paire incohérente (une ancre n'a de sens
 *     qu'avec l'écoulé qui l'a calculée) ;
 *   - C5 (pompe-ON à l'écriture, SUR L'UNE OU L'AUTRE source) : pause mélange
 *     réarmée PLEINEMENT (`fullMixingDelayMs`), prioritaire sur tout calcul de
 *     reliquat — le produit a été injecté, notifyPhDose() n'a jamais tourné.
 *
 * @param fullMixingDelayMs kPhMixingDelayMs ou kOrpMixingDelayMs selon la voie.
 */
inline DosingWindowChannelVerdict dosingWindowResolveChannel(
    const DosingWindowSourceInputs& t2, const DosingWindowSourceInputs& t1,
    uint32_t fullMixingDelayMs, uint32_t nowMs) {
  DosingWindowChannelVerdict out;
  const uint32_t safeNow = (nowMs != 0) ? nowMs : 1u;

  if (!t2.available && !t1.available) {
    out.usedMs = kDosingWindowHourMs;
    out.windowStart = safeNow;
    out.cyclesToday = 0;
    out.cyclesDayStart = safeNow;
    out.mixingEndMs = 0;
    out.source = DosingWindowSource::kFailClosed;
    return out;
  }

  DosingWindowAnchorRebase windowT2, windowT1, dayT2, dayT1;
  if (t2.available) {
    windowT2 = dosingWindowRebaseAnchor(t2.windowElapsedAtWriteMs, t2.outageElapsedMs,
                                        t2.unknownElapsed, kDosingWindowHourMs, nowMs);
    dayT2 = dosingWindowRebaseAnchor(t2.cyclesDayElapsedAtWriteMs, t2.outageElapsedMs,
                                     t2.unknownElapsed, kDosingWindowDayMs, nowMs);
  }
  if (t1.available) {
    windowT1 = dosingWindowRebaseAnchor(t1.windowElapsedAtWriteMs, t1.outageElapsedMs,
                                        t1.unknownElapsed, kDosingWindowHourMs, nowMs);
    dayT1 = dosingWindowRebaseAnchor(t1.cyclesDayElapsedAtWriteMs, t1.outageElapsedMs,
                                     t1.unknownElapsed, kDosingWindowDayMs, nowMs);
  }

  const uint32_t usedMsT2 = (t2.available && !windowT2.resetToFresh) ? t2.usedMs : 0u;
  const uint32_t usedMsT1 = (t1.available && !windowT1.resetToFresh) ? t1.usedMs : 0u;
  bool usedFromT2;
  if (!t2.available) usedFromT2 = false;
  else if (!t1.available) usedFromT2 = true;
  else usedFromT2 = (usedMsT2 >= usedMsT1);

  out.usedMs = usedFromT2 ? usedMsT2 : usedMsT1;
  out.windowStart = usedFromT2
      ? (windowT2.resetToFresh ? safeNow : windowT2.anchorMs)
      : (windowT1.resetToFresh ? safeNow : windowT1.anchorMs);

  const uint16_t cyclesT2 = (t2.available && !dayT2.resetToFresh) ? t2.cyclesToday : (uint16_t)0u;
  const uint16_t cyclesT1 = (t1.available && !dayT1.resetToFresh) ? t1.cyclesToday : (uint16_t)0u;
  bool cyclesFromT2;
  if (!t2.available) cyclesFromT2 = false;
  else if (!t1.available) cyclesFromT2 = true;
  else cyclesFromT2 = (cyclesT2 >= cyclesT1);

  out.cyclesToday = cyclesFromT2 ? cyclesT2 : cyclesT1;
  out.cyclesDayStart = cyclesFromT2
      ? (dayT2.resetToFresh ? safeNow : dayT2.anchorMs)
      : (dayT1.resetToFresh ? safeNow : dayT1.anchorMs);

  uint32_t remT2 = 0, remT1 = 0;
  if (t2.available) {
    remT2 = t2.unknownElapsed
        ? t2.mixingRemainingMs
        : ((t2.mixingRemainingMs > t2.outageElapsedMs) ? (t2.mixingRemainingMs - t2.outageElapsedMs) : 0u);
  }
  if (t1.available) {
    remT1 = t1.unknownElapsed
        ? t1.mixingRemainingMs
        : ((t1.mixingRemainingMs > t1.outageElapsedMs) ? (t1.mixingRemainingMs - t1.outageElapsedMs) : 0u);
  }
  uint32_t rem = (remT2 > remT1) ? remT2 : remT1;

  const bool pumpOnAtWrite = (t2.available && t2.pumpOnAtWrite) || (t1.available && t1.pumpOnAtWrite);
  if (pumpOnAtWrite) rem = fullMixingDelayMs;  // C5 : réarmement PLEIN, prioritaire
  out.mixingEndMs = (rem > 0u) ? (nowMs + rem) : 0u;

  out.source = (t2.available && t1.available) ? DosingWindowSource::kWorst
             : (t2.available ? DosingWindowSource::kRtc : DosingWindowSource::kNvs);
  return out;
}

// Libellé français de la source retenue, pour la journalisation (C11).
inline const char* dosingWindowSourceLabel(DosingWindowSource s) {
  switch (s) {
    case DosingWindowSource::kRtc:   return "RAM RTC (T2)";
    case DosingWindowSource::kNvs:   return "NVS (T1)";
    case DosingWindowSource::kWorst: return "le plus défavorable (T1+T2)";
    case DosingWindowSource::kFailClosed:
    default:                         return "fail-closed (aucune source exploitable, T3)";
  }
}

#endif  // DOSING_WINDOW_LOGIC_H
