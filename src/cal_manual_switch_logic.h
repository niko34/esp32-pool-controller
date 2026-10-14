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
// Correctif 2026-10-13 (v2.76.2, bug-160 suite) : l'entrée en calibration
// alors qu'une boucle était DÉJÀ en "manual" (choix utilisateur) annonçait à
// tort « mise en Manuel » et proposait de « rétablir la régulation » — sans
// qu'aucune bascule n'ait eu lieu. calSwitchShouldApplyEnter() prend
// désormais le mode courant en compte.
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
// (gate 5, C3 + bug-160 correctif 2026-10-13 v2) : ⛔ JAMAIS si une trace de
// sécurité phAutoManualEpoch/Reason est DÉJÀ active — le pH est alors DÉJÀ
// "manual" sous VERROU DE SÉCURITÉ, et ce lot ne doit ni le toucher ni
// mémoriser ce "manual" comme un "mode antérieur" à restaurer plus tard (ce
// qui effacerait l'escalade réelle).
// ⛔ JAMAIS non plus si la boucle est DÉJÀ en "manual" (choix utilisateur,
// hors verrou de sécurité) : « rien n'a changé » ne doit ni s'afficher comme
// "a été mise en Manuel", ni armer l'alerte d'oubli C4, ni mémoriser ce
// "manual" comme mode antérieur (il n'y a rien à restaurer).
// Côté ORP (aucun verrou de sécurité équivalent — vérifié le 2026-10-13 :
// zéro résultat sur `grep -r "orp_am_\|orpAutoManual" src/`), le site d'appel
// passe `safetyTraceActive=false` mais PASSE le mode courant réel dans
// `alreadyManual`.
bool calSwitchShouldApplyEnter(bool safetyTraceActive, bool alreadyManual);

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

// =============================================================================
// bug-161 (gate pool-chemistry C2, BLOQUANTE) — mode verrouillé pendant une
// session d'étalonnage
// =============================================================================
// Le mode de régulation (pH OU ORP) peut-il être changé par /save-config
// PENDANT qu'une session d'étalonnage est OUVERTE (mqttCfg.calSwitchActive) ?
// bug-160 bascule la régulation en "manual" à l'ENTRÉE en étalonnage et la
// restaure au mode ANTÉRIEUR mémorisé à la SORTIE. SANS ce refus, un
// changement de mode reçu PENDANT la session serait accepté... puis
// SILENCIEUSEMENT ÉCRASÉ à la sortie par la valeur mémorisée à l'entrée — un
// réglage accepté puis annulé sans un mot, exactement le motif « l'écran
// ment » que ce projet traque.
// Fail-closed STRICT : refuse ssi la session est ouverte ET qu'un changement
// RÉEL est demandé (modeWouldChange = mode demandé != mode courant, calculé
// par l'appelant — jamais recalculé ici, l'appelant seul connaît le format
// des chaînes de mode). Ne JAMAIS appeler sur la seule PRÉSENCE de la clé
// dans la requête : l'interface renvoie systématiquement le mode courant dans
// chaque payload de sauvegarde, ce qui bloquerait tout autre réglage pendant
// une session d'étalonnage.
// ⛔ Cette fonction REFUSE, elle ne réconcilie JAMAIS : elle ne prend ni ne
// renvoie rien qui pourrait mettre à jour ph/orpCalSwitchPrevMode — un second
// chemin d'écriture vers cette mémoire de sécurité serait exactement le piège
// que C2 interdit (cf. en-tête de ce fichier, note C2 sur phAutoManual*).
bool calSwitchBlocksModeChange(bool calSwitchActive, bool modeWouldChange);

#endif  // CAL_MANUAL_SWITCH_LOGIC_H
