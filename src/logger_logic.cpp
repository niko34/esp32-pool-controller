#include "logger_logic.h"

LogTmpRecovery evaluateLogTmpRecovery(bool admitted, bool systemLogPresent) {
  // Admission refusée → ABSTENTION, sans repli. Il n'existe pas de « moitié de
  // décision » ici : sans admission on n'a pas sondé le FS, donc `logPresent`
  // ne veut rien dire, et un repli vers l'effacement détruirait le survivant
  // dans l'état « journal absent + .tmp présent ».
  if (!admitted) return LogTmpRecovery::Abstain;

  // Journal courant présent → un `.tmp` éventuel est un déchet de rotation
  // interrompue avant le `remove` : la rotation se rejouera d'elle-même.
  // Journal courant absent → un `.tmp` éventuel est le SURVIVANT de la fenêtre
  // remove→rename, il détient l'unique copie des derniers kilooctets.
  return systemLogPresent ? LogTmpRecovery::Discard : LogTmpRecovery::Promote;
}

LogTmpRecovery classifyLogTmpOutcome(LogTmpRecovery intent, bool fsCallSucceeded) {
  switch (intent) {
    case LogTmpRecovery::Promote:
    case LogTmpRecovery::Discard:
      // Échec de l'appel FS = le `.tmp` n'existait pas (cas nominal, de loin le
      // plus fréquent) → aucun fait à rapporter, donc SILENCE.
      return fsCallSucceeded ? intent : LogTmpRecovery::None;

    case LogTmpRecovery::Abstain:
    case LogTmpRecovery::None:
    default:
      // Aucun appel FS n'a été tenté : `fsCallSucceeded` n'a pas de sens pour
      // ces états, ils traversent inchangés.
      return intent;
  }
}
