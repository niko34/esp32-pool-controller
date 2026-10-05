#ifndef TEMP_MODE_LOGIC_H
#define TEMP_MODE_LOGIC_H

// =============================================================================
// temp_mode_logic — Mode ORP EFFECTIF + chloration de secours, PURS (feature-140)
// =============================================================================
// Patron ADR-0017 : aucun millis(), aucun Arduino.h, aucune NVS, aucune
// allocation. Le temps (epoch) et tous les seuils sont des PARAMÈTRES ; module
// compilé en natif (`env:native`).
//
// POURQUOI CE MODULE. Le mode Programmée (`orp_regulation_mode = "scheduled"`,
// feature-011) dose un volume quotidien SANS regarder l'ORP — exactement ce
// qu'il faut quand les sondes pH/ORP sont déclarées non fiables (eau trouble,
// boue). La chloration de secours (feature-140) est une SURCOUCHE non
// destructive qui emprunte cette branche EXISTANTE sans jamais réécrire la NVS
// (`orp_regulation_mode`). Deux conséquences qui exigent cette logique pure :
//
//   1. Une surcouche qui agit sans toucher la NVS crée une DIVERGENCE entre
//      « ce que l'utilisateur a choisi » (NVS) et « ce qui doit gouverner le
//      dosage MAINTENANT » (mode EFFECTIF). Toute décision de dosage doit lire
//      le second ; toute publication d'état de configuration (HTTP/MQTT/WS)
//      continue de lire le premier, INCHANGÉ (règle opposable de la spec) :
//        décision de dosage => mode EFFECTIF ; publication => NVS.
//   2. Le volume quotidien effectif doit composer TROIS choses — le volume fixe
//      de secours, l'amplification du Mode Boost (feature-053) si les deux
//      coexistent, et les deux plafonds (limite utilisateur, plafond dur) — sans
//      jamais laisser l'un déjouer l'autre (cf. effectiveOrpDailyTargetMlPure).
// =============================================================================

#include <stdint.h>
#include <time.h>
#include <string.h>

// Mode de régulation ORP tel qu'il doit RÉELLEMENT gouverner le dosage. Ne pas
// confondre avec la chaîne NVS brute (mqttCfg.orpRegulationMode) : c'est elle
// qui ALIMENTE orpModeFromString(), mais la décision se prend sur la valeur
// EFFECTIVE rendue par effectiveOrpModePure().
enum class OrpMode : uint8_t {
  Automatic = 0,
  Scheduled = 1,
  Manual = 2,
};

// Conversion NVS ("automatic"/"scheduled"/"manual") -> enum. Toute chaîne NON
// reconnue (NVS corrompue, chaîne vide) retombe sur Manual — fail-closed :
// un contenu NVS inattendu ne doit jamais être lu comme une autorisation
// d'automatisme ou de dosage programmé.
OrpMode orpModeFromString(const char* s);

// Mode EFFECTIF = celui qui gouverne CHAQUE décision de dosage ORP (combien,
// quand, quelle garde). Règle :
//   - nvsMode == Manual  : toujours Manual. Condition #5 du gate pool-chemistry
//     (feature-140, gate 1) : la chloration de secours est REFUSÉE à
//     l'activation en mode Manuel et désarmée si l'utilisateur bascule en
//     Manuel pendant le mode — rescueActive ne devrait donc structurellement
//     jamais être vrai ici, mais le fail-closed est explicite malgré tout.
//   - rescueActive (et nvsMode != Manual) : Scheduled, QUELLE QUE SOIT la
//     valeur NVS — c'est ce qui ouvre la branche aveugle existante
//     (pump_controller.cpp, branche `scheduled`) même si l'utilisateur est resté
//     en "automatic" avec un pH NaN. Sans cette bascule, `requireKnownPh`
//     resterait vrai (dérivé de la NVS brute) et la feature serait morte à
//     l'arrivée (cf. correction de cadrage du 2026-10-05 dans la spec).
//   - sinon : nvsMode INCHANGÉ — non-régression stricte (D3) : Boost seul, ou
//     un mode scheduled/manual choisi par l'utilisateur SANS secours, se
//     comportent exactement comme avant cette feature.
OrpMode effectiveOrpModePure(OrpMode nvsMode, bool rescueActive);

// Chloration de secours active À L'INSTANT `now` ? Condition #6 du gate
// pool-chemistry : FAIL-CLOSED tant que l'heure n'est pas synchronisée
// (`now < minValidEpoch`) — INVERSE du Boost (isBoostActive), qui préserve
// l'état persisté sans heure valide. Motif : le secours dose à l'aveugle pour
// 48h d'affilée ; le risque d'un mode « actif pour toujours » sans contrôle
// d'échéance est jugé plus grave que celui d'une interruption transitoire au
// redémarrage.
bool rescueActivePure(bool active, time_t now, time_t untilEpoch, time_t minValidEpoch);

// Échéance (epoch) d'une activation démarrée à `now`, pour `durationS` secondes
// (48h glissantes, condition #1 du gate — PAS un ancrage sur minuit local :
// l'expiration coïnciderait avec le rollover du compteur journalier, et une
// activation à 10h ne recevrait qu'une fraction de son volume avant de mourir
// au reset).
time_t rescueDeadlinePure(time_t now, time_t durationS);

// Volume quotidien EFFECTIF de la voie ORP — compose la chloration de secours
// et le Mode Boost. Conditions #2/#3/#4 du gate pool-chemistry :
//   - secours INACTIF : nvsDailyTargetMl inchangé (D3 : Boost seul reste
//     inerte sur le volume scheduled/manual, EXACTEMENT comme avant — cf.
//     pump_controller.cpp, "scheduled/manual ne voit JAMAIS la valeur
//     boostée").
//   - secours ACTIF : part de rescueDailyMl (1500), amplifié par le Boost
//     (x1,5) SI boostActive — l'amplification est appliquée EN PREMIER, les
//     DEUX plafonds (limite utilisateur si > 0, plafond dur 2000) sont
//     appliqués EN DERNIER. Un clamp au plafond dur AVANT l'amplification
//     laisserait passer 1500 x 1,5 = 2250 mL, AU-DESSUS du plafond dur —
//     c'est exactement le chemin de contournement que la condition #4 exige de
//     fermer, fermé ici par l'ORDRE des opérations.
float effectiveOrpDailyTargetMlPure(float nvsDailyTargetMl, bool rescueActive,
                                     bool boostActive, float rescueDailyMl,
                                     float boostFactor, float maxChlorineMlPerDay,
                                     float rescueHardCapMl);

// Code de raison de fin EFFECTIF, publié à parité /data <-> WS (`rescue_lift`,
// RescueLiftReason, config.h). Extrait de config.cpp (getRescueLiftReasonCode)
// pour rendre testable en natif la fenêtre de VISIBILITÉ et son arithmétique de
// repliement de millis() : `reasonCode` et `setAtMs` sont déjà en RAM côté
// appelant (sRescueLiftReason / sRescueLiftReasonAtMs), ce module ne fait QUE
// la décision pure "la fenêtre est-elle encore ouverte ?". Arithmétique NON
// SIGNÉE, wrap-safe au repliement de millis() (49,7 jours) : `reasonCode` != 0
// redevient 0 dès que `nowMs - setAtMs >= visibleMs`, même après un repliement.
// uint32_t PLUTÔT QUE `unsigned long` (même motif que pulse_dosing_logic.h,
// dosing_logic.h::shouldRolloverByMillis) : `unsigned long` fait 32 bits sur
// ESP32 (où millis() vit) mais 64 bits sur l'hôte natif Linux x86_64 — un
// repliement à 2^32 ne se reproduirait PAS si ce module acceptait `unsigned
// long` et était testé sur l'hôte. uint32_t fixe la largeur, identique sur les
// deux architectures ; config.cpp (ESP32) convertit silencieusement sans perte
// puisque `unsigned long` y est DÉJÀ un uint32_t.
uint8_t rescueLiftReasonVisiblePure(uint8_t reasonCode, uint32_t nowMs,
                                     uint32_t setAtMs, uint32_t visibleMs);

// =============================================================================
// feature-141 — Volume de la chloration de secours SAISI à l'activation
// =============================================================================
// Deux fonctions, deux usages distincts — ne JAMAIS les confondre :
//   - clampRescueDailyMlPure : une valeur EXPLICITEMENT fournie (saisie
//     utilisateur, ou toute valeur déjà résolue) n'est QUE bornée. 10 mL saisis
//     redeviennent 100, jamais remplacés par un défaut : c'est le test manuel
//     #3 de la spec (« Saisir 10 : ramené à 100 »).
//   - resolveRescueDailyMlPure : une valeur ABSENTE ou hors bornes (stockage
//     NVS corrompu, ou aucune valeur fournie à l'activation) est REMPLACÉE par
//     la chaîne de pré-remplissage, qui se termine par le MÊME clamp — zéro
//     duplication de la logique de bornage.
// Ce clamp de SAISIE est DISTINCT des clamps C3/C4 de
// effectiveOrpDailyTargetMlPure ci-dessus : il s'applique UNE SEULE FOIS, à
// l'armement, et ne voit JAMAIS le Boost. L'introduire dans
// effectiveOrpDailyTargetMlPure déplacerait l'ordre des opérations et
// rouvrirait C4 (cf. note en tête de cette fonction) : 1900 mL saisis + Boost
// (x1,5 = 2850) doivent rester plafonnés à 2000, jamais à 100.

// Borne une valeur de volume quotidien de secours : plancher
// kRescueChlorineDailyMinMl (100 mL, FIGÉ — pas un paramètre, borne chimique
// non négociable, verdict pool-chemistry feature-141), PUIS plafond
// min(userDailyLimitMl si > 0, hardCapMl) — la limite utilisateur reste
// SOUVERAINE même sous le plancher de 100 (elle peut l'abaisser plus bas que
// 100 si l'utilisateur a configuré une limite journalière plus stricte).
float clampRescueDailyMlPure(float requestedMl, float userDailyLimitMl, float hardCapMl);

// Résout le volume quotidien de secours à utiliser quand `storedMl` est
// ABSENT ou hors bornes (condition #2 du gate pool-chemistry feature-141:
// « un secours armé à 0 mL est un mode qui ment » — jamais 0). Chaîne, par
// ordre : orpDailyTargetMl (> 0) -> sizeDefaultMl (> 0, sinon étage SAUTÉ,
// cf. chemin MQTT qui ne consulte jamais PoolSize, invariant feature-090) ->
// fallbackMl (JAMAIS 0). Se termine par UN SEUL appel à
// clampRescueDailyMlPure, qu'une valeur de la chaîne ait été retenue ou que
// `storedMl` soit déjà valide.
float resolveRescueDailyMlPure(float storedMl, float orpDailyTargetMl,
                                float sizeDefaultMl, float fallbackMl,
                                float userDailyLimitMl, float hardCapMl);

#endif  // TEMP_MODE_LOGIC_H
