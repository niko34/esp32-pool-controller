#ifndef CAL_MANUAL_SWITCH_LOGIC_H
#define CAL_MANUAL_SWITCH_LOGIC_H

#include <stdint.h>

// =============================================================================
// cal_manual_switch_logic — bascule de régulation en Manuel pendant un
// étalonnage, PURE (ADR-0017). bug-160 (volet 2, REFONTE 2026-10-13, décision
// utilisateur) : remplace le mécanisme de bail auto-expirant
// (cal_hold_logic.{h,cpp}, ABANDONNÉ) par une bascule EXPLICITE pH+ORP vers
// "manual" à l'entrée dans l'écran de calibration, restaurée par un GESTE
// HUMAIN (bouton « Fermer ») — PAS auto-libérante. Gate pool-chemistry
// GATE 5 : GO sous 8 conditions.
//
// « On échange un oubli qu'on VOIT (mode Manuel affiché au tableau de bord)
// contre une panne qu'on ne voit PAS (minuterie coincée, invisible). »
//
// ⚠️ C2 (piège central, gate 5) : cette logique ne doit JAMAIS pouvoir
// acquitter la trace de sécurité phAutoManualEpoch/phAutoManualReason
// (feature-067, escalade de plancher acide). Elle n'a d'ailleurs AUCUN moyen
// de le faire : aucune fonction ci-dessous ne prend ni ne renvoie quoi que ce
// soit qui porte cette action. Un futur mutant qui voudrait acquitter cette
// trace à la fermeture de l'écran de calibration ne peut l'introduire QUE
// côté coquille (src/web_routes_calibration.cpp) — jamais ici.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. En-
// tête C <stdint.h> uniquement (pas <cstdint>, libc++ indisponible en natif).
// =============================================================================

// Dois-je appliquer la bascule pH à l'ENTRÉE dans l'écran de calibration ?
// (gate 5, C3) : ⛔ JAMAIS si une trace de sécurité phAutoManualEpoch/Reason
// est DÉJÀ active — le pH est alors DÉJÀ "manual" sous VERROU DE SÉCURITÉ, et
// ce lot ne doit ni le toucher ni mémoriser ce "manual" comme un "mode
// antérieur" à restaurer plus tard (ce qui effacerait l'escalade réelle).
// Toujours vrai côté ORP (aucun verrou équivalent — vérifié le 2026-10-13 :
// zéro résultat sur `grep -r "orp_am_\|orpAutoManual" src/`), donc appelée
// côté ORP avec `safetyTraceActive=false` au site d'appel.
bool calSwitchShouldApplyEnter(bool safetyTraceActive);

// Dois-je restaurer le mode ANTÉRIEUR à la FERMETURE de l'écran ? (gate 5, C3)
//   wasApplied          : CETTE session avait-elle réellement basculé cette
//                         boucle à l'entrée (calSwitchShouldApplyEnter avait
//                         répondu true) ? Si non, rien à restaurer — ⛔ ne
//                         JAMAIS inventer une restauration pour une boucle
//                         jamais touchée.
//   safetyTraceActiveNow : la trace de sécurité phAutoManualEpoch/Reason est-
//                         elle active MAINTENANT (a pu apparaître PENDANT la
//                         session de calibration, par exemple une escalade de
//                         plancher acide déclenchée pendant que l'écran était
//                         ouvert) ? Si oui, le verrou de sécurité PRIME
//                         TOUJOURS sur la restauration — ⛔ ne restaure jamais
//                         le pH par-dessus une escalade de sécurité réelle.
bool calSwitchShouldRestore(bool wasApplied, bool safetyTraceActiveNow);

// Alerte d'oubli (gate 5, C4 — BLOQUANTE) : premier avertissement après
// `firstDelayS` (2 h en prod) écoulées depuis l'ENTRÉE, puis RÉPÉTÉ toutes les
// `repeatS` (6 h en prod) tant que la bascule reste ouverte — « on fabrique
// DÉLIBÉRÉMENT des bascules, il faut donc fermer le risque d'oubli ».
//   elapsedS       : temps écoulé depuis l'ENTRÉE dans l'écran de calibration.
//   sinceLastAlertS : temps écoulé depuis la DERNIÈRE alerte émise pour CETTE
//                     session ; UINT32_MAX = sentinelle « aucune encore » —
//                     jamais un entier qui pourrait légitimement être atteint
//                     par une vraie durée écoulée (l'appelant ne doit JAMAIS
//                     passer une durée réelle égale à UINT32_MAX).
bool calSwitchAlertDue(uint32_t elapsedS, uint32_t sinceLastAlertS,
                       uint32_t firstDelayS, uint32_t repeatS);

#endif  // CAL_MANUAL_SWITCH_LOGIC_H
