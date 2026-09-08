#ifndef LOGGER_LOGIC_H
#define LOGGER_LOGIC_H

// =============================================================================
// logger_logic — Réconciliation du `.tmp` de rotation au démarrage : PUR
// =============================================================================
// bug-099. La rotation du journal (logger.cpp) enchaîne DEUX opérations
// LittleFS qui ne sont pas atomiques entre elles : `remove(log)` puis
// `rename(tmp → log)`. Une coupure secteur entre les deux laisse le journal
// courant ABSENT et le `.tmp` porteur de l'UNIQUE copie des derniers kilooctets
// de journal — ce `.tmp` est complet et validé (il n'est renommé que sous
// `tmpOk`, taille > 0 après fermeture). L'effacer inconditionnellement au boot
// DÉTRUIRAIT donc le journal au lieu de nettoyer un déchet.
//
// D'où DEUX décisions séparées, et non une seule :
//   1. evaluateLogTmpRecovery() ... l'INTENTION, prise avant tout effet de bord,
//      à partir de l'admission mémoire et de la présence du journal courant.
//   2. classifyLogTmpOutcome() .... le FAIT, une fois l'appel FS exécuté.
//
// ⚠️ POURQUOI LA PRÉSENCE DU `.tmp` N'EST **PAS** UNE ENTRÉE : la sonder
// coûterait une ouverture de fichier de plus (`exists()` OUVRE le fichier, cf.
// fs_guard.h) pour une information que `rename()`/`remove()` retournent déjà.
// Le `.tmp` absent se lit dans l'ÉCHEC de l'appel FS — c'est le rôle exact de
// classifyLogTmpOutcome(), qui ramène alors le fait à `None` (silence).
//
// INVARIANT (ADR-0017) : ce module ne fait QUE décider. Aucune mesure de tas,
// aucun accès FS, aucune émission de ligne — la coquille impure logger.cpp
// garde tout cela.
//
// CONTRAINTE DE PORTABILITÉ : pas d'Arduino.h, pas de FreeRTOS, pas de FS.h,
// pas de String. En-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

/**
 * @brief Les quatre issues possibles de la réconciliation du `.tmp` de rotation.
 *
 * `None` est un FAIT, jamais une intention : il ne peut sortir que de
 * classifyLogTmpOutcome(), et signifie « il n'y avait rien à réconcilier ».
 */
enum class LogTmpRecovery : uint8_t {
  None = 0,     ///< Rien à faire (aucun `.tmp`) — SILENCE, premier boot compris.
  Promote = 1,  ///< Le `.tmp` est le survivant : `rename(tmp → log)` + WARN.
  Discard = 2,  ///< Le journal courant tient : effacer le `.tmp` orphelin + WARN.
  Abstain = 3   ///< Admission mémoire refusée : ni promotion, ni effacement.
};

/**
 * @brief Décide l'INTENTION de réconciliation, avant tout effet de bord.
 *
 * @param admitted          l'admission mémoire a-t-elle été accordée ? Si
 *                          `false`, `systemLogPresent` est IGNORÉ : on ne peut
 *                          pas avoir sondé le FS sans admission, et l'on ne
 *                          replie JAMAIS une abstention vers un effacement.
 * @param systemLogPresent  le journal courant existe-t-il ?
 * @return `Abstain` si non admis ; sinon `Discard` (le journal courant tient,
 *         le `.tmp` éventuel est un déchet) ou `Promote` (journal absent : le
 *         `.tmp` éventuel détient l'unique copie).
 *
 * @note Ne rend JAMAIS `None` : la présence du `.tmp` n'est pas connue à ce
 *       stade, l'intention porte sur ce qu'il faudrait faire S'IL existe.
 */
LogTmpRecovery evaluateLogTmpRecovery(bool admitted, bool systemLogPresent);

/**
 * @brief Transforme l'intention en FAIT, à la lumière du code de retour FS.
 *
 * @param intent           l'intention rendue par evaluateLogTmpRecovery().
 * @param fsCallSucceeded  le `rename()`/`remove()` a-t-il réussi ? Un échec
 *                         signifie, en pratique, que le `.tmp` était absent.
 * @return `None` si l'appel FS a échoué sur une intention active (rien à
 *         réconcilier → silence) ; l'intention elle-même sinon. `Abstain` et
 *         `None` traversent inchangés : aucun appel FS n'a été tenté, le code
 *         de retour ne veut rien dire pour eux.
 */
LogTmpRecovery classifyLogTmpOutcome(LogTmpRecovery intent, bool fsCallSucceeded);

#endif  // LOGGER_LOGIC_H
