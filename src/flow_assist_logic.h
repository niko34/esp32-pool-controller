#ifndef FLOW_ASSIST_LOGIC_H
#define FLOW_ASSIST_LOGIC_H

// =============================================================================
// Logique PURE de la session « assistant de mesure du débit » (feature-075).
// Header sans dépendance Arduino → testable en natif (ADR-0017).
//
// Consommé par src/web_routes_control.cpp (agent web) : l'état de session
// (pompe en assistant, compteur de purges, dernier contact) vit dans la route ;
// SEULES les décisions sont ici. Les durées et bornes viennent de constants.h
// (kFlowAssistPurgeDurationS, kFlowAssistMeasureDurationS,
//  kFlowAssistMaxPurgesPerSession, kFlowAssistSuspendTimeoutMs).
//
// Invariants (verdict gate feature-075) :
//   - une SEULE pompe en assistant à la fois (sessionActive porte ce verrou) ;
//   - purge bornée à kFlowAssistMaxPurgesPerSession par session ;
//   - timeout de suspension RÉARMÉ à chaque interaction (lastTouchMs) —
//     l'expiration est wrap-safe (arithmétique uint32 non signée).
// =============================================================================

#include <stdint.h>

// Phase minutée demandée par l'assistant.
enum class FlowAssistPhase : uint8_t {
  kPurge,    // purge des tuyaux à l'eau claire (duty 255 — seul débit connu)
  kMeasure,  // marche de mesure à l'amplitude d'exploitation
};

// Cause de refus du démarrage d'une phase (kNone = autorisé).
enum class FlowAssistRefusal : uint8_t {
  kNone,             // démarrage autorisé
  kSessionInactive,  // aucune session d'assistant ouverte (ou expirée)
  kPumpBusy,         // une marche minutée est déjà en cours (purge ou mesure)
  kPurgeQuota,       // quota de purges de la session épuisé
};

/**
 * @brief Session d'assistant expirée ? (timeout de suspension du dosage auto)
 *
 * Wrap-safe : différence uint32 NON SIGNÉE (nowMs - lastTouchMs), valide au
 * rollover de millis(). `lastTouchMs` est réarmé à CHAQUE interaction de
 * l'assistant — une session vivante ne peut donc pas expirer sous l'utilisateur.
 *
 * @param lastTouchMs millis() de la dernière interaction (réarmé par requête).
 * @param nowMs       millis() courant.
 * @param timeoutMs   kFlowAssistSuspendTimeoutMs.
 * @return true si le timeout est atteint (fermer la session, lever la suspension).
 */
inline bool isFlowAssistSessionExpired(uint32_t lastTouchMs, uint32_t nowMs,
                                       uint32_t timeoutMs) {
  return (uint32_t)(nowMs - lastTouchMs) >= timeoutMs;
}

/**
 * @brief Verdict de démarrage d'une phase minutée (purge ou mesure).
 *
 * Ordre des gardes (fail-closed) : session ouverte → pompe libre → quota purge.
 * Ne vérifie PAS l'auth ni l'inhibition maintenance : ces gardes existantes
 * restent dans la route (setManualPump refuse déjà en mode maintenance).
 *
 * @param phase         phase demandée.
 * @param purgesDone    purges déjà effectuées dans la session.
 * @param maxPurges     kFlowAssistMaxPurgesPerSession.
 * @param sessionActive session d'assistant ouverte (et non expirée).
 * @param pumpBusy      une marche minutée (purge/mesure) est déjà en cours.
 * @return kNone si autorisé, sinon la cause de refus.
 */
inline FlowAssistRefusal canStartFlowAssistPhase(FlowAssistPhase phase,
                                                 uint8_t purgesDone,
                                                 uint8_t maxPurges,
                                                 bool sessionActive,
                                                 bool pumpBusy) {
  if (!sessionActive) return FlowAssistRefusal::kSessionInactive;
  if (pumpBusy) return FlowAssistRefusal::kPumpBusy;
  if (phase == FlowAssistPhase::kPurge && purgesDone >= maxPurges) {
    return FlowAssistRefusal::kPurgeQuota;
  }
  return FlowAssistRefusal::kNone;
}

#endif  // FLOW_ASSIST_LOGIC_H
