#ifndef PH_PERSIST_LOGIC_H
#define PH_PERSIST_LOGIC_H

// =============================================================================
// ph_persist_logic — PERSISTANCE NVS de l'intégrale pH, logique PURE
// =============================================================================
// Extraction hors de la coquille ESP32 (patron ADR-0017 : pas de millis(), pas
// de time(), pas de NVS, pas d'Arduino.h — le temps, l'heure et l'instantané
// sont des PARAMÈTRES ; compilable en natif ; constants.h seule dépendance).
//
// POURQUOI CE FICHIER EXISTE
// --------------------------
// Les sept garanties R1-R7 qui conditionnent l'amendement de l'ADR-0052
// vivaient dans config.cpp et pump_controller.cpp. Or NI L'UN NI L'AUTRE n'est
// dans le `build_src_filter` de `env:native` : R1 (écrêtage), R2 (CRC, schéma,
// empreinte de contexte), R3 (péremption), R4 (confirmation différée), R5
// (cadence d'écriture) et R7 (anti-boucle) étaient donc HORS D'ATTEINTE de la
// suite de tests. Trois mutations survivaient : supprimer l'écrêtage R1,
// supprimer la vérification de CRC, rendre le prédicat permissif.
//
// C'est exactement la situation qui avait précédé l'extraction de
// ph_integral_logic.h : les conditions C2-C5 vivaient dans la coquille, aucune
// mutation ne les tuait — et le défaut réellement présent était dans la garde de
// C2. L'extraction l'avait révélé. On applique la même règle ici.
//
// RÉPARTITION
//   - ph_persist_logic.h (ce fichier) : TOUTES les décisions.
//   - config.cpp : la lecture/écriture NVS du blob, rien d'autre.
//   - pump_controller.cpp : le câblage — fournir le temps, l'heure et la mesure,
//     appliquer le verdict, journaliser.
// =============================================================================

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"  // kPhIntegral*, kMinValidEpoch — source unique des seuils

// -----------------------------------------------------------------------------
// L'INSTANTANÉ NVS (20 octets, POD)
// -----------------------------------------------------------------------------
// Écrit tel quel dans un namespace NVS dédié (`pool-phint`, clé `snap`). La
// structure est ICI et non dans config.h : son schéma, son empreinte et sa
// validation sont de la logique, pas du transport.
struct PhIntegralSnapshot {
  uint8_t  schema        = 0;    // R2 : kPhIntegralPersistSchema attendu
  uint8_t  restoreStreak = 0;    // R7 : restaurations sans écriture intercalée
  uint16_t reserved      = 0;    // alignement (jamais interprété)
  uint32_t context       = 0;    // R2 : empreinte vitesse/produit/consigne/plafond
  uint32_t epoch         = 0;    // R3 : date d'écriture (time(), secondes)
  float    value         = 0.0f; // intégrale persistée (pH·s)
  uint32_t crc           = 0;    // R2 : contrôle d'intégrité des 16 octets amont
};

/**
 * @brief Empreinte FNV-1a 32 bits d'un bloc mémoire.
 *
 * FNV-1a plutôt qu'un CRC32 tabulé : ~30 octets de flash contre ~1 Ko de table,
 * sur une ressource occupée à 97 %. On ne se protège pas d'un adversaire mais
 * d'un secteur NVS à demi écrit ou d'une structure relue avec un autre schéma —
 * une empreinte non cryptographique y suffit très largement.
 */
inline uint32_t phPersistFnv1a(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t h = 2166136261UL;  // offset basis FNV-1a
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619UL;  // prime FNV-1a
  }
  return h;
}

/** @brief Empreinte d'intégrité de l'instantané : les 16 octets AVANT `crc`. */
inline uint32_t phPersistSnapshotCrc(const PhIntegralSnapshot& s) {
  return phPersistFnv1a(&s, offsetof(PhIntegralSnapshot, crc));
}

/**
 * @brief Scelle l'instantané avant écriture : normalise `reserved`, pose `crc`.
 *
 * `reserved` n'est jamais interprété mais DOIT être déterministe, sans quoi deux
 * écritures de la même valeur produiraient des empreintes différentes selon les
 * octets de pile laissés dans le trou d'alignement.
 */
inline void phPersistSealSnapshot(PhIntegralSnapshot& s) {
  s.reserved = 0;
  s.crc = phPersistSnapshotCrc(s);
}

/**
 * @brief R2 (volet intégrité) : l'enregistrement relu est-il exploitable ?
 *
 * @param s          Structure relue
 * @param bytesRead  Nombre d'octets effectivement rendus par la NVS
 * @return true si la taille est EXACTE et l'empreinte concordante. Une taille
 *         inattendue = enregistrement d'un autre schéma ou tronqué : rejet net,
 *         jamais de réinterprétation partielle (fail-closed).
 */
inline bool phPersistSnapshotIntact(const PhIntegralSnapshot& s, size_t bytesRead) {
  if (bytesRead != sizeof(PhIntegralSnapshot)) return false;
  return phPersistSnapshotCrc(s) == s.crc;
}

// -----------------------------------------------------------------------------
// R2 — EMPREINTE DE CONTEXTE
// -----------------------------------------------------------------------------
// Une intégrale n'a de sens que pour le quadruplet qui l'a chargée : le
// changement de PRODUIT inverse le signe de l'erreur, la VITESSE change ki ET le
// plafond, la CONSIGNE déplace le point d'équilibre, et le PLAFOND fixe l'échelle
// de la valeur elle-même. Toute différence ⇒ zéro.
struct PhIntegralContext {
  uint8_t speedCode   = 0;      // code de vitesse de régulation (slow/normal/fast)
  bool    phPlus      = false;  // true = correction ph_plus (signe d'erreur inversé)
  float   target      = 0.0f;   // consigne pH
  float   integralMax = 0.0f;   // plafond d'anti-windup courant
};

/** @brief Empreinte du contexte, schéma compris (une bascule de schéma la change). */
inline uint32_t phPersistContextHash(const PhIntegralContext& ctx) {
  struct __attribute__((packed)) Blob {
    uint8_t schema;
    uint8_t speedCode;
    uint8_t correctionKind;
    uint8_t pad;
    float   target;
    float   integralMax;
  } blob;
  blob.schema = kPhIntegralPersistSchema;
  blob.speedCode = ctx.speedCode;
  blob.correctionKind = ctx.phPlus ? 1 : 0;
  blob.pad = 0;
  blob.target = ctx.target;
  blob.integralMax = ctx.integralMax;
  return phPersistFnv1a(&blob, sizeof(blob));
}

// -----------------------------------------------------------------------------
// R1 — LE PLAFOND CHIMIQUE DE RESTAURATION
// -----------------------------------------------------------------------------
// Version CORRIGÉE par le réexamen du gate pool-chemistry (30/08/2026) : le
// forfait « 30 % du plafond » ne vaut que pour l'installation de référence.
// Écrit en fraction, l'équilibre vaut f* = (charge − kp·stop) / autorité, soit
// 30 % à charge = 1,88 mL/min : TOUT bassin sous cette charge (plus petit,
// bâché, TAC bas, acide plus concentré, hors saison) verrait la butée passer
// AU-DESSUS de son propre équilibre, et l'argument « aucune restauration ne peut
// surdoser seule » tomberait. Cf. constants.h pour le chiffrage complet.
//
// Le plafond retenu est donc le MINIMUM de la butée absolue et d'une fraction de
// l'équilibre RÉELLEMENT OBSERVÉ sur CE bassin.

/**
 * @brief Charge acide observée du bassin, servant de base au plafond R1.
 *
 * `loadMlPerMin` : médiane des volumes pH journaliers sur
 * kPhIntegralRestoreHistoryWindowDays, rapportée aux minutes de filtration du
 * jour. `days` : nombre de jours EXPLOITABLES réellement disponibles — c'est lui
 * qui arme le fail-closed, pas la valeur.
 */
struct PhPersistLoadObservation {
  float   loadMlPerMin = 0.0f;
  uint8_t days         = 0;
};

/** @brief Verdict du calcul de plafond — les termes intermédiaires sont exposés
 *         pour la journalisation (R6) et pour le diagnostic. */
struct PhPersistCapVerdict {
  float capPhS         = 0.0f;   // plafond RETENU (0 ⇒ aucune restauration)
  float absoluteCapPhS = 0.0f;   // butée 0,30 × integralMax
  float loadCapPhS     = 0.0f;   // 0,50 × équilibre observé
  float equilibriumPhS = 0.0f;   // I* observé = (charge − kp·stop) / ki
  bool  loadGoverns    = false;  // true : c'est la charge observée qui borne
  bool  historyShort   = false;  // true : moins de kPhIntegralRestoreMinHistoryDays
};

/**
 * @brief R1 — plafond de restauration, en pH·s.
 *
 *     plafond = min( kPhIntegralRestoreMaxFraction × integralMax ,
 *                    kPhIntegralRestoreLoadFraction × (charge − kp × stop) / ki )
 *
 * Le premier terme est une BUTÉE ABSOLUE, jamais dépassée. Le second est la
 * vérification À L'EXÉCUTION que le static_assert (xi) de constants.h ne peut pas
 * faire : il est calculé sur la charge de CE bassin, et non sur celle de
 * l'installation de référence.
 *
 * FAIL-CLOSED, dans l'ordre — chacun de ces cas rend un plafond NUL, donc aucune
 * restauration :
 *   - moins de kPhIntegralRestoreMinHistoryDays jours d'historique exploitable
 *     (un appareil neuf ne restaure rien, c'est le comportement voulu) ;
 *   - réglages non armés ou non finis (ki ≤ 0, integralMax ≤ 0, kp/stop absurdes) ;
 *   - charge observée non finie ou négative ;
 *   - équilibre observé nul ou négatif : la charge n'excède même pas ce que le
 *     terme proportionnel fournit au seuil d'arrêt — l'intégrale n'a rien à
 *     apprendre, et il n'y a donc rien à restaurer.
 *
 * @param integralMax    Plafond d'anti-windup COURANT (pH·s)
 * @param kp             Gain proportionnel courant (mL/min par pH)
 * @param ki             Gain intégral courant (mL/min par pH·s)
 * @param stopThreshold  Seuil d'ARRÊT de l'hystérésis de dosage (pH)
 * @param obs            Charge observée + profondeur d'historique
 */
inline PhPersistCapVerdict phPersistRestoreCap(float integralMax, float kp, float ki,
                                               float stopThreshold,
                                               const PhPersistLoadObservation& obs) {
  PhPersistCapVerdict v;
  v.historyShort = (obs.days < kPhIntegralRestoreMinHistoryDays);

  if (!(integralMax > 0.0f) || !isfinite(integralMax)) return v;
  v.absoluteCapPhS = kPhIntegralRestoreMaxFraction * integralMax;

  if (v.historyShort) return v;  // fail-closed : charge inconnue ⇒ pas de plafond
  if (!(ki > 0.0f) || !isfinite(ki)) return v;
  if (!isfinite(kp) || kp < 0.0f) return v;
  if (!isfinite(stopThreshold) || stopThreshold < 0.0f) return v;
  if (!isfinite(obs.loadMlPerMin) || obs.loadMlPerMin <= 0.0f) return v;

  // I* — remplissage d'ÉQUILIBRE : la valeur vers laquelle l'intégrale converge
  // quand la boucle tient sa consigne. C'est aussi la frontière convergence /
  // cycle limite : au-dessus, la décharge ne vaut plus que 0,013 mL/min par
  // heure (six jours sans retour). On reste donc dans sa moitié basse.
  const float equilibrium = (obs.loadMlPerMin - kp * stopThreshold) / ki;
  if (!isfinite(equilibrium) || equilibrium <= 0.0f) return v;
  v.equilibriumPhS = equilibrium;
  v.loadCapPhS = kPhIntegralRestoreLoadFraction * equilibrium;

  if (v.loadCapPhS < v.absoluteCapPhS) {
    v.capPhS = v.loadCapPhS;
    v.loadGoverns = true;
  } else {
    v.capPhS = v.absoluteCapPhS;
  }
  return v;
}

/**
 * @brief Écrêtage R1 proprement dit — à réappliquer À L'INSTANT DE L'APPLICATION.
 *
 * Le plafond courant fait foi, jamais celui photographié au boot : entre la
 * lecture NVS et la confirmation par la mesure, la vitesse de régulation a pu
 * changer, et la charge observée aussi.
 *
 * @return La valeur écrêtée ; 0 si le plafond est nul/négatif ou la valeur
 *         non exploitable (rien à restaurer).
 */
inline float phPersistClampRestore(float value, float capPhS) {
  if (!(capPhS > 0.0f) || !isfinite(capPhS)) return 0.0f;
  if (!isfinite(value) || value <= 0.0f) return 0.0f;
  return (value > capPhS) ? capPhS : value;
}

// -----------------------------------------------------------------------------
// R3 — PÉREMPTION
// -----------------------------------------------------------------------------

/** @brief L'horloge murale est-elle exploitable ? Sans elle, l'âge de
 *         l'instantané est INCALCULABLE — et un âge incalculable vaut un âge
 *         périmé (fail-closed). */
inline bool phPersistClockUsable(uint32_t epochNow) {
  return epochNow >= (uint32_t)kMinValidEpoch;
}

/**
 * @brief R3 : l'instantané est-il encore frais ?
 *
 * Réévalué EN CONTINU, et pas seulement au boot : une candidate qui attend sa
 * confirmation peut mourir de vieillesse entre-temps, elle n'est pas « gelée » à
 * son âge de lecture. Un horodatage nul ou situé dans le futur (horloge reculée)
 * est traité comme périmé.
 */
inline bool phPersistFresh(uint32_t epochWritten, uint32_t epochNow) {
  if (!phPersistClockUsable(epochNow)) return false;
  if (epochWritten == 0) return false;
  if (epochNow < epochWritten) return false;
  return (epochNow - epochWritten) <= kPhIntegralPersistMaxAgeS;
}

// -----------------------------------------------------------------------------
// R7 — ANTI-BOUCLE DE BOOT
// -----------------------------------------------------------------------------

/** @brief La persistance est-elle DÉSARMÉE par trop de restaurations d'affilée ? */
inline bool phPersistStreakExhausted(uint8_t streak) {
  return streak >= kPhIntegralRestoreMaxStreak;
}

/** @brief Incrément SATURÉ du compteur : un repliement à 255 réarmerait
 *         silencieusement la persistance, c'est-à-dire annulerait R7. */
inline uint8_t phPersistNextStreak(uint8_t streak) {
  return (streak < 255u) ? (uint8_t)(streak + 1u) : streak;
}

// -----------------------------------------------------------------------------
// DÉCISION DE RESTAURATION (R1 + R2 + R3 + R7)
// -----------------------------------------------------------------------------

enum class PhPersistReject : uint8_t {
  kNone = 0,
  kSchema,     // enregistrement d'un autre schéma
  kStreak,     // R7 : anti-boucle de démarrage
  kContext,    // R2 : réglages de régulation modifiés depuis l'écriture
  kEpoch,      // horodatage nul, futur, ou horloge inexploitable
  kStale,      // R3 : instantané périmé
  kValue,      // valeur nulle ou non finie
  kNoCap,      // R1 : plafond nul (historique trop court, ki inerte…)
};

struct PhPersistRestoreVerdict {
  bool            accepted  = false;
  PhPersistReject reject    = PhPersistReject::kNone;
  float           candidate = 0.0f;  // valeur ÉCRÊTÉE, à confirmer par la mesure
  uint32_t        ageS      = 0;     // âge de l'instantané (journalisation)
};

/**
 * @brief Libellé français du motif de rejet, pour la journalisation (R6).
 *
 * nullptr = REJET SILENCIEUX volontaire :
 *   - kNone : il n'y a pas de rejet ;
 *   - kNoCap : plafond nul. C'est l'état NOMINAL d'un appareil neuf (moins de
 *     3 jours d'historique de charge) ou d'un ki inerte ; en faire un
 *     avertissement à chaque démarrage noierait les vrais rejets. La coquille
 *     journalise ce cas séparément, en `info`, avec la charge observée.
 */
inline const char* phPersistRejectLabel(PhPersistReject reject) {
  switch (reject) {
    case PhPersistReject::kSchema:  return "schéma d'enregistrement différent";
    case PhPersistReject::kStreak:  return "trop de restaurations consécutives "
                                           "(anti-boucle de démarrage)";
    case PhPersistReject::kContext: return "réglages de régulation modifiés "
                                           "depuis l'écriture";
    case PhPersistReject::kEpoch:   return "horodatage incohérent";
    case PhPersistReject::kStale:   return "instantané périmé";
    case PhPersistReject::kValue:   return "valeur nulle ou non finie";
    default:                        return nullptr;
  }
}

/**
 * @brief Faut-il retenir cet instantané comme CANDIDAT à la restauration ?
 *
 * Applique, dans cet ordre exact : schéma (R2), anti-boucle (R7), empreinte de
 * contexte (R2), horodatage puis péremption (R3), plausibilité de la valeur,
 * enfin écrêtage au plafond chimique (R1). L'ordre importe : un enregistrement
 * d'un autre schéma ne doit pas être jugé sur son contexte, qui n'a pas le même
 * sens.
 *
 * ATTENTION : un verdict `accepted` n'autorise PAS à appliquer la valeur. Elle
 * reste CANDIDATE jusqu'à confirmation par la mesure (R4, phPersistConfirmTick).
 *
 * @param s            Instantané relu (intégrité déjà vérifiée par l'appelant)
 * @param contextHash  Empreinte du contexte COURANT
 * @param epochNow     Heure murale courante (secondes)
 * @param capPhS       Plafond R1 courant, rendu par phPersistRestoreCap()
 */
inline PhPersistRestoreVerdict phPersistEvaluateRestore(const PhIntegralSnapshot& s,
                                                        uint32_t contextHash,
                                                        uint32_t epochNow,
                                                        float capPhS) {
  PhPersistRestoreVerdict v;

  if (s.schema != kPhIntegralPersistSchema) {
    v.reject = PhPersistReject::kSchema;
    return v;
  }
  if (phPersistStreakExhausted(s.restoreStreak)) {
    v.reject = PhPersistReject::kStreak;
    return v;
  }
  if (s.context != contextHash) {
    v.reject = PhPersistReject::kContext;
    return v;
  }
  if (!phPersistClockUsable(epochNow) || s.epoch == 0 || epochNow < s.epoch) {
    v.reject = PhPersistReject::kEpoch;
    return v;
  }
  v.ageS = epochNow - s.epoch;
  if (v.ageS > kPhIntegralPersistMaxAgeS) {
    v.reject = PhPersistReject::kStale;
    return v;
  }
  if (!isfinite(s.value) || s.value <= 0.0f) {
    v.reject = PhPersistReject::kValue;
    return v;
  }

  const float clamped = phPersistClampRestore(s.value, capPhS);
  if (!(clamped > 0.0f)) {
    v.reject = PhPersistReject::kNoCap;
    return v;
  }

  v.accepted = true;
  v.candidate = clamped;
  return v;
}

// -----------------------------------------------------------------------------
// R4 — MACHINE DE CONFIRMATION PAR LA MESURE
// -----------------------------------------------------------------------------
// La valeur relue n'est JAMAIS appliquée au boot : elle attend que le bassin
// confirme lui-même qu'une charge acide est bien présente — erreur au-dessus du
// seuil de DÉMARRAGE pendant kPhIntegralRestoreConfirmMs CONSÉCUTIVES de temps de
// BRANCHE. Un pH déjà à la consigne au retour du courant ⇒ candidate abandonnée.

struct PhPersistConfirmState {
  float    candidate = 0.0f;  // valeur en attente (0 = aucune candidate)
  uint32_t epoch     = 0;     // date d'écriture de la candidate (R3)
  uint32_t heldMs    = 0;     // tenue CONTINUE de l'écart, en temps de branche
};

struct PhPersistConfirmInputs {
  uint32_t epochNow       = 0;    // heure murale (péremption réévaluée en continu)
  uint32_t dtMs           = 0;    // dt de BRANCHE depuis le tick précédent
  float    error          = 0.0f; // erreur signée courante
  float    startThreshold = 0.0f; // seuil de DÉMARRAGE de l'hystérésis
  float    capPhS         = 0.0f; // plafond R1 COURANT (réappliqué à l'application)
};

enum class PhPersistConfirmOutcome : uint8_t {
  kIdle = 0,  // aucune candidate en attente
  kWaiting,   // en attente : tenue en cours, ou remise à zéro (écart retombé)
  kExpired,   // R3 : candidate périmée avant confirmation — abandonnée
  kApplied,   // confirmée : `restored` est à appliquer
  kNoCap,     // R1 : tenue ACQUISE mais plafond COURANT nul — rien à appliquer
};

struct PhPersistConfirmVerdict {
  PhPersistConfirmOutcome outcome = PhPersistConfirmOutcome::kIdle;
  float                   restored = 0.0f;  // valeur écrêtée à appliquer
};

/** @brief Une interruption du temps de branche rompt la CONTINUITÉ exigée par R4
 *         (mode manuel, garde bloquante, mesure invalide) : la tenue repart de
 *         zéro, mais la candidate survit. */
inline void phPersistConfirmInterrupt(PhPersistConfirmState& st) {
  st.heldMs = 0;
}

/** @brief Abandon DÉFINITIF de la candidate — aux points de rupture de sens
 *         (changement de vitesse, bascule ph_minus/ph_plus, passage en mode
 *         Programmée) : elle a été validée contre un contexte qui n'existe plus. */
inline void phPersistConfirmAbandon(PhPersistConfirmState& st) {
  st = PhPersistConfirmState();
}

/**
 * @brief UN tick de la machine de confirmation R4.
 *
 * À n'appeler QUE lorsque l'accumulation est autorisée (mesure validée,
 * filtration active, mode automatique) : c'est ce qui donne son sens à « temps de
 * branche ». Hors de ces états, appeler phPersistConfirmInterrupt().
 *
 * @param st  État de la machine, modifié en place
 * @param in  Entrées du tick
 * @return Le verdict ; `kApplied` porte la valeur écrêtée à appliquer.
 */
inline PhPersistConfirmVerdict phPersistConfirmTick(PhPersistConfirmState& st,
                                                    const PhPersistConfirmInputs& in) {
  PhPersistConfirmVerdict v;
  if (!(st.candidate > 0.0f)) return v;  // kIdle : rien en attente

  // R3 réévaluée à chaque tick : la candidate meurt de vieillesse si la
  // confirmation tarde. Elle n'est pas figée à son âge de lecture.
  if (!phPersistFresh(st.epoch, in.epochNow)) {
    phPersistConfirmAbandon(st);
    v.outcome = PhPersistConfirmOutcome::kExpired;
    return v;
  }

  v.outcome = PhPersistConfirmOutcome::kWaiting;

  // L'écart doit être franchement positif : au-dessus du seuil de DÉMARRAGE, pas
  // seulement du seuil d'arrêt. Restaurer une autorité de dosage alors que le
  // bassin est déjà à sa consigne serait précisément l'« autorisation de dosage
  // pré-chargée » que l'ADR-0052 interdisait.
  if (!isfinite(in.error) || !(in.error > in.startThreshold)) {
    st.heldMs = 0;  // continuité rompue
    return v;
  }

  // Même base de temps que la charge de l'intégrale : les dt aberrants (trou
  // d'évaluation) ne sont pas crédités, jamais synthétisés.
  if (in.dtMs > 0 && in.dtMs <= kPhIntegralMaxTickDtMs) st.heldMs += in.dtMs;

  if (st.heldMs >= kPhIntegralRestoreConfirmMs) {
    // R1 réappliquée ICI, sur le plafond COURANT : entre la lecture et la
    // confirmation, la vitesse de régulation ou la charge observée ont pu changer.
    v.restored = phPersistClampRestore(st.candidate, in.capPhS);
    phPersistConfirmAbandon(st);
    // Deux abandons de causes DIFFÉRENTES : périmée (R3, plus haut) et plafond
    // retombé à zéro (R1, ici). Les confondre ferait journaliser « périmé » un
    // refus de plafond, et le diagnostic partirait sur l'horloge au lieu de la
    // charge observée.
    v.outcome = (v.restored > 0.0f) ? PhPersistConfirmOutcome::kApplied
                                    : PhPersistConfirmOutcome::kNoCap;
  }
  return v;
}

// -----------------------------------------------------------------------------
// R5 — CADENCE D'ÉCRITURE
// -----------------------------------------------------------------------------

struct PhPersistWriteInputs {
  uint32_t nowMs       = 0;     // millis() courant
  uint32_t lastSaveMs  = 0;     // millis du dernier flush (0 = jamais écrit)
  uint32_t epochNow    = 0;     // heure murale
  float    value       = 0.0f;  // intégrale courante
  float    savedValue  = 0.0f;  // dernière valeur persistée
  float    integralMax = 0.0f;  // plafond courant (échelle du seuil d'amplitude)
};

/**
 * @brief R5 : faut-il écrire l'instantané maintenant ?
 *
 * Au plus une écriture par kPhIntegralPersistMinIntervalMs ET seulement si la
 * valeur a bougé de plus de kPhIntegralPersistMinDeltaFrac du plafond depuis la
 * dernière. Ordre de grandeur : ~2 écritures par jour de filtration en régime
 * établi, à comparer aux 100 000 cycles garantis d'un secteur NVS.
 *
 * Sans heure valide on n'écrit PAS : l'âge serait incalculable au prochain boot
 * et l'enregistrement y serait de toute façon rejeté (R3) — autant épargner le
 * cycle d'écriture.
 *
 * Soustraction non signée sur `nowMs` → wrap-safe (millis() boucle à ~49,7 j).
 */
inline bool phPersistShouldWrite(const PhPersistWriteInputs& in) {
  if (!phPersistClockUsable(in.epochNow)) return false;
  if (in.lastSaveMs != 0 &&
      (uint32_t)(in.nowMs - in.lastSaveMs) < kPhIntegralPersistMinIntervalMs) {
    return false;
  }
  const float span = kPhIntegralPersistMinDeltaFrac * in.integralMax;
  if (!(span > 0.0f) || !isfinite(span)) return false;
  if (!isfinite(in.value)) return false;
  return fabsf(in.value - in.savedValue) > span;
}

// -----------------------------------------------------------------------------
// R1 (suite) — L'HISTORIQUE DE CHARGE : anneau de 7 journées
// -----------------------------------------------------------------------------
// Ce que `phPersistRestoreCap` consomme (PhPersistLoadObservation) n'existait
// nulle part : `pool-daily` n'a que le compteur du JOUR COURANT, écrasé au
// changement de date, et `history.*` ne stocke que pH/ORP/température. La
// fonction rendait donc `days = 0` et le fail-closed interdisait TOUTE
// restauration. C'est cet anneau qui l'alimente.
//
// La minute de filtration est INDISPENSABLE : la charge se mesure en mL/min, pas
// en mL/jour. Un jour à 4 h et un jour à 10 h de circulation qui injectent le
// même volume ne décrivent pas la même charge — le second est 2,5 fois moins
// chargé. Sans le dénominateur, la médiane serait celle d'une grandeur qui
// dépend surtout de l'horaire de filtration.
//
// TOUT est ici, et rien dans la coquille : celle-ci ne fait que fournir le blob
// relu et l'échantillon du jour qui se termine.

/** @brief Une journée close (8 octets, POD). `date` = 0 ⇒ case vide. */
struct PhLoadDaySample {
  uint32_t date    = 0;  // AAAAMMJJ en entier (0 = case vide)
  uint16_t ml      = 0;  // volume pH injecté sur la journée (mL, arrondi)
  uint16_t minutes = 0;  // minutes de filtration EFFECTIVES de la journée
};

/**
 * @brief L'anneau lui-même (64 octets, POD) — un seul blob NVS, une écriture/jour.
 *
 * `days[N-1]` est la journée la PLUS RÉCENTE (l'ajout décale vers la gauche).
 * L'ordre n'importe pas au calcul — une médiane s'en moque — mais il donne un
 * point de comparaison stable pour la déduplication d'une même date.
 */
struct PhLoadHistory {
  uint8_t  schema = kPhLoadHistorySchema;
  uint8_t  pad0 = 0;  // alignement — normalisés au scellement, jamais interprétés
  uint8_t  pad1 = 0;
  uint8_t  pad2 = 0;
  PhLoadDaySample days[kPhIntegralRestoreHistoryWindowDays];
  uint32_t crc = 0;  // empreinte des octets AMONT (même FNV-1a que l'instantané)
};

/** @brief Empreinte d'intégrité de l'anneau : les octets AVANT `crc`. */
inline uint32_t phLoadHistoryCrc(const PhLoadHistory& h) {
  return phPersistFnv1a(&h, offsetof(PhLoadHistory, crc));
}

/** @brief Scelle l'anneau avant écriture : schéma courant, remplissage
 *         déterministe des octets d'alignement, empreinte posée. */
inline void phLoadHistorySeal(PhLoadHistory& h) {
  h.schema = kPhLoadHistorySchema;
  h.pad0 = 0;
  h.pad1 = 0;
  h.pad2 = 0;
  h.crc = phLoadHistoryCrc(h);
}

/**
 * @brief L'anneau relu est-il exploitable ? Taille EXACTE, schéma courant,
 *        empreinte concordante — sinon rejet net (fail-closed : historique vide,
 *        `days = 0`, aucune restauration). Jamais de réinterprétation partielle.
 */
inline bool phLoadHistoryIntact(const PhLoadHistory& h, size_t bytesRead) {
  if (bytesRead != sizeof(PhLoadHistory)) return false;
  if (h.schema != kPhLoadHistorySchema) return false;
  return phLoadHistoryCrc(h) == h.crc;
}

/**
 * @brief Ajoute une journée close à l'anneau (décalage à gauche, la plus
 *        ancienne sort).
 *
 * Si UNE QUELCONQUE des cases porte déjà cette date, elle est REMPLACÉE au lieu
 * d'être doublée. Ne comparer qu'à la case la plus récente ne suffit pas : un
 * recul d'horloge (correction NTP à cheval sur minuit) rouvre puis reclôt une
 * date déjà archivée UNE CASE PLUS LOIN, et la même journée serait alors comptée
 * deux fois dans la médiane sur 7 — c'est-à-dire pondérée double.
 *
 * @return true si l'anneau a changé (donc s'il faut l'écrire). Une date nulle est
 *         refusée : sans horloge valide, une journée n'est pas datable.
 */
inline bool phLoadHistoryAppend(PhLoadHistory& h, const PhLoadDaySample& s) {
  if (s.date == 0) return false;
  const uint8_t n = kPhIntegralRestoreHistoryWindowDays;
  for (uint8_t i = 0; i < n; ++i) {
    if (h.days[i].date == s.date) {
      h.days[i] = s;  // même journée re-clôturée : on écrase, on n'empile pas
      return true;
    }
  }
  for (uint8_t i = 0; i + 1 < n; ++i) h.days[i] = h.days[i + 1];
  h.days[n - 1] = s;
  return true;
}

/**
 * @brief Numéro de JOUR CALENDAIRE d'une date AAAAMMJJ (0 = date inexploitable).
 *
 * L'anneau ne porte que des dates ; sans conversion, « 20260301 − 20260228 » vaut
 * 73 et « 20270101 − 20261231 » vaut 8 870. Or c'est un ÉCART EN JOURS qui décide
 * de la fenêtre d'observation. La conversion est celle de Howard Hinnant
 * (days_from_civil) : purement arithmétique, donc exacte aux fins de mois comme
 * aux années bissextiles (règle des 400 ans comprise), et testable en natif.
 *
 * L'origine est arbitraire (0000-03-01) : seule la DIFFÉRENCE est utilisée.
 */
inline uint32_t phLoadDayNumber(uint32_t date) {
  const uint32_t y = date / 10000u;
  const uint32_t m = (date / 100u) % 100u;
  const uint32_t d = date % 100u;
  // Rejet net de tout ce qui n'est pas une date plausible (fail-closed : une date
  // illisible ⇒ échantillon écarté, jamais réinterprété).
  if (y < 1970u || y > 9999u || m < 1u || m > 12u || d < 1u || d > 31u) return 0;

  const uint32_t yy = y - ((m <= 2u) ? 1u : 0u);  // l'année commence en mars
  const uint32_t era = yy / 400u;
  const uint32_t yoe = yy - era * 400u;                 // année dans l'ère [0,399]
  const uint32_t mp  = (m > 2u) ? (m - 3u) : (m + 9u);  // mars = 0
  const uint32_t doy = (153u * mp + 2u) / 5u + d - 1u;  // jour dans l'année [0,365]
  const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097u + doe;
}

/**
 * @brief La journée est-elle DANS la fenêtre d'observation, vue de `today` ?
 *
 * POURQUOI CETTE FENÊTRE EXISTE. L'anneau garde les sept dernières journées
 * ARCHIVÉES, pas les sept derniers jours : un appareil arrêté deux mois retrouve
 * au démarrage des journées d'été intactes. Quatre journées estivales à
 * 2,86 mL/min et trois journées d'automne à 1,00 mL/min donnent une médiane de
 * 2,86 — la valeur d'une saison révolue — soit un plafond de 3 753 pH·s contre
 * 2 484 pH·s d'équilibre réel : on restaurerait 1,5 fois l'équilibre courant,
 * exactement ce que le gate déclare inacceptable (constants.h).
 *
 * FAIL-CLOSED SANS CONTREPARTIE : écarter des échantillons ne peut que réduire
 * `days`, donc faire retomber l'historique sous kPhIntegralRestoreMinHistoryDays,
 * donc annuler le plafond. Une date FUTURE (horloge reculée) est écartée au même
 * titre : son âge n'est pas calculable.
 */
inline bool phLoadDayWithinWindow(uint32_t date, uint32_t today) {
  const uint32_t a = phLoadDayNumber(date);
  const uint32_t b = phLoadDayNumber(today);
  if (a == 0 || b == 0) return false;  // pas d'horloge ⇒ pas d'âge ⇒ pas d'échantillon
  if (a > b) return false;             // journée datée dans le futur
  return (b - a) <= (uint32_t)kPhIntegralRestoreHistoryWindowDays;
}

/**
 * @brief Débit soutenable RETENU pour le filet, avec repli documenté.
 *
 * Le seuil de vraisemblance est une FONCTION des réglages : débit d'amplitude
 * calibré × limite horaire configurée (phSustainableAvgFlow, ph_integral_logic.h).
 * La coquille le calcule et le passe ; ce helper ne fait qu'appliquer le repli
 * quand il n'est PAS calculable — pompe non caractérisée, configuration pas
 * encore chargée, valeur non finie.
 *
 * Le repli n'est pas un seuil nominal : c'est le réglage par défaut de
 * l'installation de référence, employé faute de mieux.
 */
inline float phLoadSustainableOrFallback(float sustainableMlPerMin) {
  if (!isfinite(sustainableMlPerMin) || sustainableMlPerMin <= 0.0f) {
    return kPhLoadSustainableFallbackMlPerMin;
  }
  return sustainableMlPerMin;
}

/**
 * @brief Le rapport mL/min de cette journée est-il PHYSIQUEMENT possible ?
 *
 * L'actionneur ne peut pas DÉLIVRER plus que son débit moyen soutenable. Un
 * rapport au-dessus n'est donc pas l'observation d'un bassin très chargé : c'est
 * la PREUVE qu'il manque des minutes au dénominateur — 1 000 mL en 130 min
 * annoncées, soit 7,7 mL/min à 5 min/h, décrit une journée dont la circulation a
 * été comptée pour un quart. La journée est ÉCARTÉE, jamais rapiécée.
 *
 * ATTENTION — le seuil SUIT LA CONFIGURATION. À 15 min/h le délivré légitime
 * atteint ~6,5 mL/min : un seuil figé sur les 5 min/h par défaut écarterait
 * alors TOUTES les journées et tuerait la restauration en silence, précisément
 * quand l'utilisateur applique la recommandation du gate de relever la limite
 * horaire. D'où le paramètre.
 *
 * `minutes == 0` est écarté au même titre : le rapport n'existe pas.
 *
 * @param sustainableMlPerMin Débit moyen soutenable COURANT (mL/min) ; ≤ 0 ou
 *                            non fini ⇒ repli kPhLoadSustainableFallbackMlPerMin.
 */
inline bool phLoadDayRatePlausible(const PhLoadDaySample& d,
                                   float sustainableMlPerMin) {
  if (d.minutes == 0) return false;
  const float rate = (float)d.ml / (float)d.minutes;
  const float ceiling =
      kPhLoadDayMaxRateFactor * phLoadSustainableOrFallback(sustainableMlPerMin);
  return rate <= ceiling;
}

/**
 * @brief Une journée est-elle REPRÉSENTATIVE de la charge du bassin AUJOURD'HUI ?
 *
 * Trois critères CUMULATIFS :
 *   - au moins kPhLoadDayMinFiltrationMin minutes de circulation (justification
 *     chiffrée dans constants.h) ;
 *   - au plus kPhIntegralRestoreHistoryWindowDays jours d'âge (phLoadDayWithinWindow) ;
 *   - un rapport mL/min PHYSIQUEMENT atteignable (phLoadDayRatePlausible).
 *
 * Les trois n'écartent QUE par le haut. Le volume n'entre pas dans le critère de
 * représentativité — une journée à volume nul avec une circulation suffisante
 * est une observation légitime de charge faible, et l'écarter tirerait la médiane
 * VERS LE HAUT, c'est-à-dire du mauvais côté.
 *
 * @param today Date du jour (AAAAMMJJ) ; 0 = horloge inexploitable ⇒ AUCUNE
 *              journée n'est retenue (fail-closed).
 * @param sustainableMlPerMin Débit moyen soutenable COURANT (mL/min).
 */
inline bool phLoadDayUsable(const PhLoadDaySample& d, uint32_t today,
                            float sustainableMlPerMin) {
  if (d.date == 0 || d.minutes < kPhLoadDayMinFiltrationMin) return false;
  if (!phLoadDayRatePlausible(d, sustainableMlPerMin)) return false;
  return phLoadDayWithinWindow(d.date, today);
}

/**
 * @brief LA CHARGE OBSERVÉE : médiane des mL/min des journées exploitables.
 *
 * MÉDIANE et non moyenne : une seule journée aberrante — purge, incident,
 * assistant de mesure de débit, mise en service — suffirait à déplacer une
 * moyenne sur 7 valeurs de plus de 10 %, et le plafond de restauration avec elle.
 * La médiane encaisse jusqu'à trois journées folles sur sept sans bouger.
 *
 * `days` compte les journées EXPLOITABLES, pas les cases remplies : c'est lui qui
 * arme le fail-closed de phPersistRestoreCap (moins de
 * kPhIntegralRestoreMinHistoryDays ⇒ plafond nul ⇒ aucune restauration).
 *
 * Tri par insertion sur au plus 7 éléments, dans un tableau de PILE : aucune
 * allocation, coût négligeable, appelé au plus une fois par jour.
 *
 * @param today Date du jour (AAAAMMJJ), qui borne l'ÂGE des échantillons retenus.
 *              0 (horloge inexploitable) ⇒ days = 0 ⇒ plafond nul.
 * @param sustainableMlPerMin Débit moyen soutenable COURANT (mL/min), qui borne
 *              la VRAISEMBLANCE des échantillons retenus.
 */
inline PhPersistLoadObservation phLoadObservation(const PhLoadHistory& h,
                                                  uint32_t today,
                                                  float sustainableMlPerMin) {
  PhPersistLoadObservation obs;
  const uint8_t n = kPhIntegralRestoreHistoryWindowDays;
  float rates[kPhIntegralRestoreHistoryWindowDays];
  uint8_t count = 0;

  for (uint8_t i = 0; i < n; ++i) {
    const PhLoadDaySample& d = h.days[i];
    if (!phLoadDayUsable(d, today, sustainableMlPerMin)) continue;
    const float rate = (float)d.ml / (float)d.minutes;  // minutes > 0 par le critère
    uint8_t k = count;
    while (k > 0 && rates[k - 1] > rate) {
      rates[k] = rates[k - 1];
      --k;
    }
    rates[k] = rate;
    ++count;
  }

  obs.days = count;
  if (count == 0) return obs;
  obs.loadMlPerMin = (count & 1u)
                         ? rates[count / 2]
                         : 0.5f * (rates[count / 2 - 1] + rates[count / 2]);
  return obs;
}

#endif  // PH_PERSIST_LOGIC_H
