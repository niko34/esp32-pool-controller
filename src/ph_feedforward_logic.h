#ifndef PH_FEEDFORWARD_LOGIC_H
#define PH_FEEDFORWARD_LOGIC_H

// =============================================================================
// ph_feedforward_logic — DOSE DE BASE APPRISE (feedforward) pH, logique PURE
// =============================================================================
// feature-085 (v2.28.0). Patron ADR-0017 : pas de millis() interne — le temps
// est un PARAMÈTRE ; pas d'allocation ; pas d'Arduino.h ; compilable en natif.
//
// POURQUOI CE MODULE EXISTE
// -------------------------
// Le PI redécouvre par tâtonnement une charge parfaitement PRÉVISIBLE : le
// dégazage de CO₂ (~80 % de la charge basique quotidienne) est stable d'un jour
// à l'autre, et l'appareil le connaît déjà — c'est la médiane des mL/min
// journaliers de l'anneau de charge (feature-084, ph_persist_logic.h). Pourtant
// l'intégrale met 2,4 jours de filtration à la retrouver, et le moindre à-coup
// l'efface. Le feedforward injecte la charge CONNUE ; le PI ne traite plus que
// l'écart résiduel :
//
//     débit = FF(charge apprise) + kp·e + ki·∫(e − seuil)
//
// CE QUE LE FF EST — ET N'EST PAS
// -------------------------------
//   - un DÉBIT CONSTANT (mL/min), jamais un accumulateur : aucun état volumique,
//     donc aucune « dette » à rattraper après un reboot ou une suspension (A7 —
//     redémarrage en pleine séance ⇒ pas de rafale de rattrapage) ;
//   - un TERME ADDITIF dans le calcul de débit de la branche automatique, JAMAIS
//     une seconde branche de dosage (condition bloquante n°1 du gate) : un seul
//     chemin vers la pompe, une seule comptabilité de volume, un seul jeu de
//     gardes — le FF passe par computePID/planPulse/tickPulse comme le PI ;
//   - fondé sur l'HISTORIQUE VOLUMÉTRIQUE, pas sur l'erreur instantanée : il
//     survit aux perturbations (pluie, appoint) qui déclenchent le reset sec de
//     l'intégrale — c'est son second argument de robustesse ;
//   - APPRIS, jamais saisi : aucun paramètre de volume de bassin, de TAC ni de
//     concentration de produit n'entre ici (A9 — exigence utilisateur).
//
// A8 — STABILITÉ PAR HYSTÉRÉSIS : STRUCTURELLE, RIEN À CODER
// ----------------------------------------------------------
// Le FF ne vit QUE sous `shouldDose` (session de dosage automatique ouverte).
// C'est l'hystérésis de session existante (démarrage à phStartThreshold, arrêt à
// phStopThreshold) qui borne toute oscillation : un FF qui sur-corrigerait fait
// retomber l'erreur sous le seuil d'ARRÊT, la session se ferme, et le FF cesse
// AVEC elle — il ne peut pas entretenir un cycle limite à lui seul, puisque sa
// condition d'existence est précisément la session qu'il vient de fermer. La
// réouverture exige ensuite une erreur remontée à phStartThreshold : la bande
// [stop ; start] absorbe le rebond. Ne PAS ajouter d'hystérésis interne ici —
// elle ferait double emploi et créerait un second point de réglage.
//
// A6 — SONDE FIGÉE : corollaire structurel. Une sonde figée ferme la porte de
// validité (isPhFilterReady() == false) ⇒ canDose() refuse ⇒ aucune session ⇒
// FF = 0. Le latch « mesure invalide » ci-dessous double cette garantie en
// EXPLICITE : même si une session survivait d'un tick à la bascule, le FF est
// retenu dès que la mesure n'est plus digne de confiance.
//
// RÉPARTITION (même règle que ph_integral_logic / ph_persist_logic)
//   - ce fichier : TOUTES les décisions (plafonds, latch, quota, historique) ;
//   - pump_controller.cpp : le câblage — fournir l'erreur signée, la validité de
//     mesure, l'empreinte de contexte, la charge observée, le débit soutenable,
//     l'état de session et le temps ; appliquer le verdict ; journaliser.
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"         // kPhFf*, kPhIntegral* — source unique des seuils
#include "ph_persist_logic.h"  // PhPersistLoadObservation (médiane 7 j, feature-084)

// -----------------------------------------------------------------------------
// État persistant du feedforward (POD, loopTask uniquement — aucun mutex).
// -----------------------------------------------------------------------------
struct PhFfState {
  // LATCH de sécurité (condition bloquante n°3 de la spec) : une fois posé, le
  // FF vaut 0 jusqu'à l'ouverture de la session AUTOMATIQUE suivante. Posé par :
  // erreur défavorable TENUE (kPhFfLowHoldMs), mesure invalide, changement de
  // contexte de régulation (produit / consigne / vitesse / plafond).
  bool     held          = false;
  uint32_t lastTickMs    = 0;  // base de temps de branche (0 = jamais évalué)
  // Tenue CUMULÉE de l'erreur défavorable (temps de branche, ADR-0049 : on
  // accumule des durées OBSERVÉES depuis un armement, jamais de comparaison à
  // une échéance future). Remise à zéro à la moindre remontée CONSTATÉE.
  uint32_t adverseHeldMs = 0;
  uint32_t lastCtxHash   = 0;      // empreinte de contexte du tick précédent
  bool     ctxArmed      = false;  // lastCtxHash renseigné (1er tick exclu)
  bool     wasSessionOpen = false; // front d'ouverture de session (réarmement)
};

// -----------------------------------------------------------------------------
// Code d'état, exposé à /data + WS (observabilité). CONTRAT FIGÉ, comme
// DoseRefusal : ne jamais renuméroter, ne jamais insérer au milieu — le client
// traduit ces valeurs en libellés.
// -----------------------------------------------------------------------------
enum class PhFfStatus : uint8_t {
  kInactive  = 0,  // aucune session automatique ouverte — le FF ne vit pas
  kActive    = 1,  // FF appliqué (le débit peut valoir 0,00 si la charge
                   // observée est nulle : c'est une observation légitime)
  kHeld      = 2,  // retenu par le latch de sécurité (réarmé à la session suivante)
  kNoHistory = 3,  // moins de kPhIntegralRestoreMinHistoryDays jours exploitables
  kQuota     = 4,  // quota journalier ≥ kPhFfDailyQuotaFraction × limite
};

// Cause du latch posé CE tick (journalisation par la coquille ; kNone sinon).
enum class PhFfLatchCause : uint8_t {
  kNone = 0,
  kAdverseError,   // erreur signée < −kPhFfAdverseErrorPh tenue kPhFfLowHoldMs
  kMeasureInvalid, // mesure périmée / figée / instable / non finie
  kContext,        // changement de produit, consigne, vitesse ou plafond
  kProductChange,  // tenue prolongée : l'anneau contient des mL de l'autre produit
};

// Verdict du tick — la coquille applique et journalise, elle ne décide pas.
struct PhFfVerdict {
  float          ffMlPerMin = 0.0f;  // débit de base à ADDITIONNER au PI
  PhFfStatus     status     = PhFfStatus::kInactive;
  PhFfLatchCause latched    = PhFfLatchCause::kNone;  // latch posé CE tick
  bool           released   = false;                  // latch réarmé CE tick
};

/**
 * @brief DOSE DE BASE APPRISE : min(charge observée, fraction du soutenable).
 *
 * feature-092 (A1) — VRAIE FACTORISATION du § 4 de phFeedforwardTick : mêmes
 * gardes `isfinite`/`> 0`, même `min`, même plafond
 * `kPhFfMaxSustainableFraction × soutenable`. Le tick appelle CETTE fonction ;
 * il n'existe donc plus deux écritures de la même règle susceptibles de
 * diverger. C'est aussi le nombre que le journal annonce à l'ouverture de
 * session (« dose de base apprise 1.41 … plafond 2.60 ») et celui que la carte
 * « Débit d'injection » doit pouvoir lire HORS session — d'où son extraction.
 *
 * FAIL-CLOSED : charge ou soutenable non finis / non positifs ⇒ 0 (pompe non
 * caractérisée, cache non amorcé). Le `min` garantit qu'il reste TOUJOURS au
 * moins (1 − kPhFfMaxSustainableFraction) du soutenable au PI.
 *
 * ATTENTION — CE QUE CETTE FONCTION NE DIT PAS : elle ne teste PAS la
 * profondeur d'historique (`obs.days`). Un appelant qui veut distinguer
 * « 0,00 mL/min appris » (observation légitime, 3 jours sans injection) de
 * « rien d'appris » doit tester `obs.days` LUI-MÊME — c'est exactement ce que
 * fait le tick (garde kNoHistory, § 4) et ce que fait l'accesseur de lecture.
 *
 * @param obs                 Charge observée (médiane 7 j) + profondeur d'historique
 * @param sustainableMlPerMin Débit moyen soutenable COURANT (phSustainableAvgFlow)
 * @return Débit de base (mL/min), 0 en fail-closed
 */
inline float phFeedforwardLearnedFlow(const PhPersistLoadObservation& obs,
                                      float sustainableMlPerMin) {
  if (isfinite(obs.loadMlPerMin) && obs.loadMlPerMin > 0.0f &&
      isfinite(sustainableMlPerMin) && sustainableMlPerMin > 0.0f) {
    const float cap = kPhFfMaxSustainableFraction * sustainableMlPerMin;
    return (obs.loadMlPerMin < cap) ? obs.loadMlPerMin : cap;
  }
  return 0.0f;
}

/**
 * @brief STATUT RÉELLEMENT SERVI à /data + WS, depuis l'instantané du tick.
 *
 * feature-092 (A2). L'instantané `_ffStatus` n'est rafraîchi que lorsque
 * update() atteint le tick FF (eau présente, hors purge/OTA/maintenance) : il
 * peut donc être PÉRIMÉ. Deux traductions, et deux seulement :
 *
 *   1. `kActive` hors session ⇒ `kInactive` (traduction existante depuis
 *      feature-085 : le FF ne vit que sous une session automatique) ;
 *   2. `kNoHistory` alors que l'anneau porte désormais
 *      ≥ kPhIntegralRestoreMinHistoryDays journées exploitables ⇒ `kInactive`.
 *      C'est le défaut du 03/09 : entre l'archivage de minuit (3ᵉ journée) et le
 *      premier tick du matin, l'interface restait figée sur « en apprentissage »
 *      pendant sept heures alors que la dose était apprise.
 *
 * `kHeld` (2) et `kQuota` (4) ne sont JAMAIS traduits : ils restent vrais entre
 * deux ticks (un latch ne se lève qu'à l'ouverture de session suivante, un quota
 * qu'au rollover de minuit) et ils portent la seule explication utile —
 * POURQUOI le FF ne s'appliquera pas.
 *
 * FAIL-CLOSED : `learnedFinite == false` (aucune dose de base connue : pompe non
 * caractérisée, soutenable indisponible) ⇒ `kInactive`. Servir « actif » avec une
 * valeur indisponible ferait afficher « appliquée » à côté d'un tiret.
 * PRÉCÉDENCE, dans cet ordre exact : kNoHistory d'abord (sinon la garde
 * fail-closed effacerait le « en apprentissage », qui est PRÉCISÉMENT le cas
 * d'une valeur non finie), puis 2/4 tels quels, puis 1 hors session, puis
 * fail-closed.
 *
 * @param raw           Instantané brut du tick (`(uint8_t)PhFfStatus`)
 * @param sessionOpen   Session de dosage pH AUTOMATIQUE ouverte MAINTENANT
 * @param historyDays   Profondeur d'historique COURANTE (obs.days, cache relu à
 *                      chaque clôture de journée — c'est lui qui périme le 3)
 * @param learnedFinite La dose de base apprise est-elle disponible ?
 * @return Code PhFfStatus à sérialiser (contrat FIGÉ, aucun code ajouté)
 */
inline uint8_t phFeedforwardServedStatus(uint8_t raw, bool sessionOpen,
                                         uint8_t historyDays, bool learnedFinite) {
  if (raw == (uint8_t)PhFfStatus::kNoHistory) {
    return (historyDays >= kPhIntegralRestoreMinHistoryDays)
               ? (uint8_t)PhFfStatus::kInactive
               : (uint8_t)PhFfStatus::kNoHistory;
  }
  if (raw == (uint8_t)PhFfStatus::kHeld || raw == (uint8_t)PhFfStatus::kQuota) {
    return raw;  // vrais entre deux ticks — jamais traduits
  }
  if (raw == (uint8_t)PhFfStatus::kActive && !sessionOpen) {
    return (uint8_t)PhFfStatus::kInactive;
  }
  if (!learnedFinite) return (uint8_t)PhFfStatus::kInactive;
  return raw;
}

/** @brief Remet le feedforward dans son état de sortie de boot. */
inline void phFeedforwardReset(PhFfState& st) {
  st = PhFfState();
}

/**
 * @brief UN tick du feedforward pH — à appeler à chaque tour de la branche
 *        d'entretien (même point de passage que le tick intégral, AVANT lui :
 *        l'anti-windup du PI retranche le débit rendu ici de son soutenable).
 *
 * Enchaînement, dans cet ordre exact :
 *   1. base de temps de branche (dt aberrant rejeté, jamais synthétisé) ;
 *   2. RÉARMEMENT du latch au front d'ouverture de session (sauf tenue produit) ;
 *   3. latchs : contexte changé, mesure invalide, erreur défavorable TENUE ;
 *   4. calcul du débit : min(charge observée, fraction du soutenable), gaté par
 *      l'historique minimal, le quota journalier et la session ouverte.
 *
 * @param st                  État persistant, modifié en place
 * @param signedError         Erreur SIGNÉE (positive = il faut doser) — le même
 *                            signe pour ph_minus et ph_plus (condition n°3 du
 *                            visa : le déclencheur du latch est `< −0,05` en
 *                            erreur SIGNÉE, donc valable pour les deux produits)
 * @param measureValid        Porte de validité de la mesure (filtre prêt, stable,
 *                            valeur finie) — évaluée par la coquille
 * @param ctxHash             Empreinte de contexte COURANTE (phIntegralContextHash :
 *                            vitesse, produit, consigne, plafond)
 * @param obs                 Charge observée (médiane 7 j) + profondeur d'historique
 * @param sustainableMlPerMin Débit moyen soutenable COURANT (phSustainableAvgFlow)
 * @param sessionOpen         Session de dosage pH AUTOMATIQUE ouverte
 * @param nowMs               millis() fourni par la coquille
 * @param dailyInjectedMl     Volume pH déjà injecté aujourd'hui (mL)
 * @param maxDailyMl          Limite journalière configurée (mL ; ≤ 0 = illimitée)
 * @param productChangeHold   Tenue prolongée du latch après changement de produit
 *                            (recommandation du gate) : tant qu'aucune journée
 *                            n'a été archivée sous le nouveau régime, l'anneau
 *                            contient des mL de l'AUTRE produit et la médiane ne
 *                            décrit pas la charge courante
 * @return Verdict à appliquer/journaliser par la coquille
 */
inline PhFfVerdict phFeedforwardTick(PhFfState& st, float signedError,
                                     bool measureValid, uint32_t ctxHash,
                                     const PhPersistLoadObservation& obs,
                                     float sustainableMlPerMin, bool sessionOpen,
                                     uint32_t nowMs, float dailyInjectedMl,
                                     float maxDailyMl, bool productChangeHold) {
  PhFfVerdict v;

  // --- 1. BASE DE TEMPS DE BRANCHE (même règle que le tick intégral) ---------
  // Un trou d'évaluation (mode changé, filtration coupée) est REJETÉ, jamais
  // crédité d'un bloc : la tenue d'erreur défavorable ne mûrit qu'en temps
  // réellement observé (ADR-0049).
  uint32_t elapsed = 0;
  bool dtUsable = false;
  if (st.lastTickMs == 0) {
    st.lastTickMs = nowMs;  // premier tick : on ARME, on n'accumule pas
  } else {
    elapsed = (uint32_t)(nowMs - st.lastTickMs);  // non signé ⇒ wrap-safe
    st.lastTickMs = nowMs;
    dtUsable = (elapsed <= kPhIntegralMaxTickDtMs);
  }

  // --- 2. RÉARMEMENT AU FRONT D'OUVERTURE DE SESSION --------------------------
  // « Réarmé à l'ouverture de session suivante » : le front montant suffit — un
  // latch posé EN COURS de session tient jusqu'à la fin de celle-ci (pas de
  // front tant qu'elle reste ouverte), puis la réouverture réarme. La tenue
  // produit (productChangeHold) PRIME : elle n'est levée que par la coquille,
  // à l'archivage d'une journée entière sous le nouveau régime.
  const bool sessionOpening = sessionOpen && !st.wasSessionOpen;
  st.wasSessionOpen = sessionOpen;
  if (sessionOpening && st.held && !productChangeHold) {
    st.held = false;
    st.adverseHeldMs = 0;
    v.released = true;
  }

  // --- 3. LATCHS DE SÉCURITÉ (condition bloquante n°3 de la spec) -------------
  // 3a. Tenue produit : l'anneau mélange encore les deux produits.
  if (productChangeHold && !st.held) {
    st.held = true;
    v.latched = PhFfLatchCause::kProductChange;
  }

  // 3b. Changement de CONTEXTE (produit, consigne, vitesse, plafond) : la charge
  // apprise l'a été pour un autre point de fonctionnement — même règle R2 que la
  // persistance de l'intégrale. Détection par FRONT (empreinte du tick précédent),
  // le premier tick ne fait qu'armer la référence.
  if (st.ctxArmed && ctxHash != st.lastCtxHash && !st.held) {
    st.held = true;
    v.latched = PhFfLatchCause::kContext;
  }
  st.lastCtxHash = ctxHash;
  st.ctxArmed = true;

  // 3c. MESURE INVALIDE : un feedforward est la seule source de dosage qui ne
  // dépend PAS de l'erreur mesurée — c'est sa force et son danger. Sans mesure
  // digne de confiance, plus personne ne peut constater qu'il dose à tort :
  // fail-closed immédiat, sans tenue.
  if (!measureValid) {
    st.adverseHeldMs = 0;  // l'erreur n'a plus de sens : la tenue non plus
    if (!st.held) {
      st.held = true;
      v.latched = PhFfLatchCause::kMeasureInvalid;
    }
  } else if (signedError < -kPhFfAdverseErrorPh) {
    // 3d. ERREUR DÉFAVORABLE TENUE (« fausse pluie » de la spec : pH − 0,3 ⇒ le
    // FF doit s'annuler en < 30 min). L'erreur signée est FRANCHEMENT de l'autre
    // côté de la consigne : la charge apprise ne décrit plus l'état courant
    // (apport externe, pluie, produit dans l'eau). Tenue en temps de branche,
    // saturation d'addition — jamais de repliement.
    if (dtUsable) {
      const uint32_t room = 0xFFFFFFFFu - st.adverseHeldMs;
      st.adverseHeldMs += (elapsed < room) ? elapsed : room;
    }
    if (st.adverseHeldMs >= kPhFfLowHoldMs && !st.held) {
      st.held = true;
      v.latched = PhFfLatchCause::kAdverseError;
    }
  } else {
    st.adverseHeldMs = 0;  // remontée constatée : la tenue repart de zéro
  }

  // --- 4. CALCUL DU DÉBIT ------------------------------------------------------
  // Ordre des états : le latch (sécurité) prime sur tout ; historique et quota
  // sont rendus même hors session (observabilité : l'UI peut dire POURQUOI le FF
  // ne s'appliquera pas, avant même qu'une session s'ouvre).
  if (st.held) {
    v.status = PhFfStatus::kHeld;
    return v;
  }

  // Historique insuffisant : moins de kPhIntegralRestoreMinHistoryDays journées
  // exploitables ⇒ la médiane ne fait pas foi (même fail-closed que le plafond
  // de restauration R1 — un appareil neuf ne dose rien « de mémoire »).
  if (obs.days < kPhIntegralRestoreMinHistoryDays) {
    v.status = PhFfStatus::kNoHistory;
    return v;
  }

  // PLAFOND QUOTA JOURNALIER (condition bloquante n°2 du visa). 60 % du
  // soutenable N'EST PAS 60 % de la limite journalière : quota 300 mL/j avec
  // 12 h de filtration ⇒ 0,42 mL/min suffisent à consommer 100 % du quota, or le
  // plafond de soutenable autorise ~1,30 mL/min. Sans ce gel, le FF viderait
  // seul le quota et le PI n'aurait plus AUCUNE marge pour l'écart résiduel.
  // Limite ≤ 0 = désactivée par l'utilisateur : pas de gel (comme partout).
  if (maxDailyMl > 0.0f && isfinite(dailyInjectedMl) &&
      dailyInjectedMl >= kPhFfDailyQuotaFraction * maxDailyMl) {
    v.status = PhFfStatus::kQuota;
    return v;
  }

  // A8 : le FF ne VIT que sous une session automatique ouverte (cf. en-tête).
  if (!sessionOpen) {
    v.status = PhFfStatus::kInactive;
    return v;
  }

  // FF = min(charge observée, fraction du soutenable) — feature-092 (A1) : la
  // règle vit dans phFeedforwardLearnedFlow() ci-dessus, appelée ICI et par
  // l'accesseur de lecture (la carte « Débit d'injection » doit lire la dose
  // apprise HORS session). Une seule écriture, donc aucune divergence possible ;
  // fail-closed et plafond inchangés, à résultat strictement identique.
  v.status = PhFfStatus::kActive;
  v.ffMlPerMin = phFeedforwardLearnedFlow(obs, sustainableMlPerMin);
  return v;
}

#endif  // PH_FEEDFORWARD_LOGIC_H
