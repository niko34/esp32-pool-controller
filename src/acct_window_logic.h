#ifndef ACCT_WINDOW_LOGIC_H
#define ACCT_WINDOW_LOGIC_H

#include <stdint.h>

// =============================================================================
// acct_window_logic — fenêtre de comptabilité pompe, PURE (ADR-0017)
// =============================================================================
// bug-115 — Module header-only, testable nativement sans FreeRTOS ni Arduino.
//
// POURQUOI CE MODULE. accountPumpVolume() (pump_controller.cpp) n'était
// appelée QUE depuis la queue nominale d'update() : toute sortie anticipée
// (OTA, maintenance, capteurs non initialisés, eau absente / stabilisation /
// assistant de débit) revenait AVANT elle. `_acctPumpOn[]` et
// `lastSafetyTimestamp` restaient alors RÉMANENTS pendant toute la fenêtre
// aveugle — à la reprise, `deltaMs` valait la durée ENTIÈRE de la coupure,
// créditée en bloc au débit plateau, et le redémarrage physique (prevOn resté
// vrai) n'était jamais compté.
//
// Le correctif (bug-115, v2.61.0) appelle désormais accountPumpVolume() à
// CHAQUE sortie, via le point de passage unique settlePumpAccounting()
// (pump_controller.cpp). Ce module PUR porte la décision qui rend la
// rémanence STRUCTURELLEMENT IMPOSSIBLE, quel que soit l'appelant :
//   - `nextOn` est TOUJOURS posé à `onNow` (jamais figé sur un ancien état) ;
//   - `nextTimestampMs` ne survit jamais à une pompe coupée (`onNow == false`
//     => 0, jamais l'ancien timestamp).
// Un appel depuis une sortie anticipée règle donc son propre delta — qu'il
// soit nul (pompe déjà éteinte), partiel (coupure en plein pouls) ou complet
// (pompe restée en manuel pendant la fenêtre, cf. `flowAssistSuspend`) — puis
// repart propre pour le tour suivant.
//
// Arithmétique NON SIGNÉE, exacte à travers le repliement de millis()
// (24,8 j, défaut déjà rencontré sur ce projet) : `(uint32_t)(nowMs -
// lastTsMs)` reste juste même si `nowMs` a replié par rapport à `lastTsMs`.
// Aucune comparaison `now < lastTsMs`, aucune branche de rattrapage.
struct AcctWindow {
  bool creditDelta;          // true : un volume doit être crédité ce tour
  uint32_t deltaMs;          // durée du delta à créditer (0 si creditDelta == false)
  bool nextOn;               // nouvel état de _acctPumpOn[chem] pour le tour suivant
  uint32_t nextTimestampMs;  // nouveau lastSafetyTimestamp pour le tour suivant
};

// Évalue la fenêtre de comptabilité d'UN tour d'accountPumpVolume().
//   prevOn      : pompe alimentée au tour PRÉCÉDENT (_acctPumpOn[chem])
//   lastTsMs    : lastSafetyTimestamp au tour précédent (0 = aucun crédit en attente)
//   onNow       : pompe alimentée CE tour (pumpDuty[physIdx] > 0, après applyPumpDuty)
//   nowMs       : millis() du tour courant
//
// creditDelta = prevOn && lastTsMs != 0 && (uint32_t)(nowMs - lastTsMs) > 0
// deltaMs     = creditDelta ? (uint32_t)(nowMs - lastTsMs) : 0
// nextOn      = onNow            <- TOUJOURS défini : la rémanence devient
//                                   structurellement impossible (mutant à
//                                   tuer : `nextOn = prevOn || onNow`, qui
//                                   ferait disparaître le démarrage de reprise)
// nextTimestampMs = onNow ? nowMs : 0   (mutant à tuer : `= nowMs`
//                                        inconditionnel, qui ferait réapparaître
//                                        le volume fantôme)
inline AcctWindow evaluateAcctWindow(bool prevOn, uint32_t lastTsMs, bool onNow,
                                      uint32_t nowMs) {
  AcctWindow w;
  w.creditDelta = prevOn && lastTsMs != 0 && (uint32_t)(nowMs - lastTsMs) > 0;
  w.deltaMs = w.creditDelta ? (uint32_t)(nowMs - lastTsMs) : 0;
  w.nextOn = onNow;
  w.nextTimestampMs = onNow ? nowMs : 0;
  return w;
}

#endif  // ACCT_WINDOW_LOGIC_H
