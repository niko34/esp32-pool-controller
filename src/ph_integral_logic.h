#ifndef PH_INTEGRAL_LOGIC_H
#define PH_INTEGRAL_LOGIC_H

// =============================================================================
// ph_integral_logic — TERME INTÉGRAL pH, logique PURE (v2.26.0, correction)
// =============================================================================
// Extraction du terme intégral pH hors de la coquille ESP32 (patron ADR-0017 :
// pas de millis() interne — le temps est un PARAMÈTRE ; pas d'allocation ; pas
// d'Arduino.h ; compilable en natif).
//
// POURQUOI CE FICHIER EXISTE
// --------------------------
// Les mécanismes C2 (oubli / reset sec), C3 (gel d'anti-windup), C4 (péremption)
// et C5 (surveillance de saturation) vivaient dans PumpControllerClass::update().
// Aucun test ne les atteignait : les mutations « restaurer le reset à −0,01 » et
// « supprimer la décroissance » laissaient la suite native intégralement verte.
// Le défaut corrigé ici — l'intégrale sans voie de décharge — était EXACTEMENT
// dans la condition de garde de C2. Cette zone doit être testable, donc pure.
//
// LE DÉFAUT CORRIGÉ
// -----------------
// L'ancienne coquille n'évaluait le terme intégral que dans la branche
// `shouldDose`, et ne le déchargeait que sous `error < 0`. Or l'hystérésis de
// dosage arrête la session à `phStopThreshold` (0,01 pH) : le régime établi se
// parque à `consigne + 0,01`, erreur POSITIVE, et la branche d'oubli ne
// s'exécutait jamais. Il n'existait AUCUNE voie de décharge pour une erreur dans
// ]0 ; startThreshold[ — l'intégrale montait à son plafond puis y restait, ou
// entrait en cycle limite (charge → sur-correction → reset sec → recharge).
//
// LA CORRECTION : INTÉGRATION SIGNÉE SUR (erreur − seuil d'ARRÊT)
// ---------------------------------------------------------------
//     dI/dt = (erreur − phStopThreshold)
//
// évaluée à CHAQUE tick automatique (plus seulement quand on dose), la SORTIE
// restant gatée par `shouldDose` et par la bande morte de computePidPure.
// La décharge devient STRUCTURELLE : dès que l'erreur passe sous le seuil
// d'arrêt, l'intégrande devient négatif et l'intégrale décroît d'elle-même.
// L'équilibre s'établit donc à `consigne + phStopThreshold`, c'est-à-dire au
// point exact où l'hystérésis de dosage coupe : le correcteur et l'ordonnanceur
// visent enfin le MÊME point de fonctionnement.
//
// L'alternative « appliquer la décroissance exponentielle dans la bande morte »
// a été REJETÉE : à demi-vie d'une heure, le rapport accumulation/décharge est
// d'environ 30 en défaveur de la charge — l'intégrale ne pourrait plus tenir la
// charge basique mesurée (2,86 mL/min) et lever Ki deviendrait inutile.
//
// CE QUI RESTE INCHANGÉ (exigence explicite du gate pool-chemistry)
//   - le SEUIL du reset sec, `error < -(startThreshold + kPhIntegralResetMargin)` ;
//   - l'OUBLI exponentiel sous consigne (error < 0), demi-vie 1 h par pas fixes ;
//   - la PÉREMPTION à 24 h sans évaluation.
//
// feature-084 (sujet 2) — LE RESET SEC EST TEMPORISÉ
// ---------------------------------------------------
// Le seuil ne bouge pas, son DÉCLENCHEMENT devient conditionné à une tenue de
// kPhIntegralResetHoldMs de temps de BRANCHE (même base que la charge). Un
// transitoire — une pluie suffit — n'efface donc plus deux jours et demi
// d'apprentissage. Pendant l'attente la protection n'est pas suspendue mais
// GRADUÉE : `error < 0` fait tourner l'oubli exponentiel ET la décharge signée.
// Cf. constants.h, kPhIntegralResetHoldMs, pour la justification chimique.
//
// Conséquence sur l'ORDRE des étapes : la base de temps (ex-étape 2) passe
// DEVANT le reset sec (ex-étape 1), puisque c'est elle qui produit le `dt` que
// la tenue accumule. Aucun autre effet — après une purge, la base de temps est
// réarmée exactement comme avant (pas de péremption fantôme).
//
// feature-084 (sujet 3) — LA PERSISTANCE NVS N'EST PAS ICI
// --------------------------------------------------------
// L'intégrale est désormais persistée (ADR-0052 amendé), sous sept conditions
// cumulatives R1-R7 décrites dans constants.h. RIEN de tout cela ne vit dans ce
// fichier : les DÉCISIONS sont dans `ph_persist_logic.h` (module pur voisin, lui
// aussi testable en natif), le transport NVS dans config.cpp, et le câblage dans
// pump_controller.cpp. Ce module-ci ne voit qu'un `st.integral` déjà posé,
// exactement comme après un reset.
// =============================================================================

#include <math.h>
#include <stdint.h>

#include "constants.h"  // kPhIntegral* — source unique des seuils (convention projet)

// Valeur de SATURATION de la tenue d'anomalie (`anomalyHeldMs`), qui sert aussi
// de SENTINELLE « plus aucun rappel dû » pour `nextCriticalMs` : les deux champs
// comptent des millisecondes CUMULÉES (jamais un millis() absolu), la saturation
// est donc la seule fin de course possible. Cf. l'étape 7 de phIntegralTick.
constexpr uint32_t kPhIntegralHoldNever = 0xFFFFFFFFu;

// -----------------------------------------------------------------------------
// État persistant du terme intégral pH.
// -----------------------------------------------------------------------------
// POD sans allocation, vit dans PumpControllerClass. Écrit/lu depuis la SEULE
// tâche loop (patron _stabilizationEndMs) — aucun mutex requis.
struct PhIntegralState {
  float    integral       = 0.0f;  // pH·s accumulés (signé)
  uint32_t lastTickMs     = 0;     // dernier tick automatique (0 = jamais évalué)
  uint32_t pendingMs      = 0;     // temps observé pas encore intégré (reliquat)
  uint32_t decayMs        = 0;     // dernier pas d'oubli appliqué (0 = pas d'épisode)
  uint32_t resetHoldMs    = 0;     // feature-084 : tenue CUMULÉE de l'excursion
                                   // franche sous la consigne (temps de branche).
                                   // Remise à zéro à la moindre remontée.
  bool     satActive      = false; // hystérésis de saturation : épisode ouvert
  uint32_t anomalyHeldMs  = 0;     // tenue CUMULÉE de l'anomalie (temps de branche auto)
  uint32_t nextCriticalMs = 0;     // prochaine échéance de RAPPEL critical
  bool     warnSent       = false; // warning déjà émis pour CET épisode
  bool     critSent       = false; // critical déjà émis pour CET épisode
};

// Réglages du correcteur au moment du tick (vitesse de régulation + seuils
// utilisateur). Passés en paramètre : ce ne sont pas des constantes firmware.
struct PhIntegralTuning {
  float kp;              // gain proportionnel (mL/min par pH)
  float ki;              // gain intégral (mL/min par pH·s)
  float integralMax;     // plafond d'anti-windup (pH·s), bornage SYMÉTRIQUE
  float startThreshold;  // pumpProtection.phStartThreshold
  float stopThreshold;   // pumpProtection.phStopThreshold — CIBLE de l'intégration
};

// Entrées mesurées du tick.
struct PhIntegralInputs {
  uint32_t nowMs;             // millis() fourni par la coquille
  float    error;             // erreur SIGNÉE (positive = il faut doser)
  float    ampFlow;           // débit MESURÉ à l'amplitude calibrée (mL/min)
  int      budgetMinPerHour;  // limite horaire d'injection (min/h ; 0 = illimitée)
  bool     quotaNearLimit;    // quota journalier au-delà de la fraction de gel
  // Quota de CYCLES journalier épuisé (cyclesToday >= maxCyclesPerDay).
  // Défaut `false` : une coquille qui ne renseigne pas ce champ ne gèle pas —
  // c'est l'ancien comportement, et le champ n'est pas un critère de sécurité
  // par lui-même (il ne fait que suspendre une accumulation stérile).
  bool     cycleQuotaExhausted = false;
  // feature-085 — débit du FEEDFORWARD réellement appliqué ce tour (mL/min ;
  // 0 hors session). Retranché du débit soutenable MOYEN vu par le PI : c'est la
  // DIMENSION correcte de l'anti-windup (condition bloquante n°1 du visa).
  // Passer `ampFlow − ff` à la place serait presque un no-op — le soutenable
  // vaudrait (ampFlow − ff) × budget/60, soit un retrait de 8,3 % du FF
  // seulement à 5 min/h : le gel resterait quasi aveugle à la part d'actionneur
  // que le FF consomme réellement, et l'intégrale se chargerait sur une marge
  // qui n'existe pas. Défaut 0 : une coquille sans FF garde l'ancien calcul.
  float    ffMlPerMin = 0.0f;
};

// Événement ponctuel du tick (pour la journalisation par la coquille).
enum class PhIntegralEvent : uint8_t {
  kNone = 0,
  kDryReset,     // purge sèche : excursion FRANCHE de l'autre côté de la consigne
  kDecayClosed,  // plancher de 1 pH·s atteint : épisode d'oubli fermé
};

// Verdict du tick — la coquille applique, elle ne décide pas.
struct PhIntegralVerdict {
  PhIntegralEvent event = PhIntegralEvent::kNone;
  float integralBefore = 0.0f;       // valeur AVANT le tick (message de purge)
  bool  frozen = false;              // accumulation POSITIVE gelée (C3)
  bool  saturated = false;           // épisode de saturation ouvert (hystérésis)
  bool  emitWarn = false;            // palier warning franchi
  bool  emitCritical = false;        // palier critical franchi OU rappel dû
  bool  emitClear = false;           // épisode alerté terminé
  float demandMlPerMin = 0.0f;       // kp·e + ki·I, intégrale AVANT ce tick
  float sustainableMlPerMin = 0.0f;  // débit moyen réellement TENABLE
  uint32_t anomalyHeldMs = 0;        // tenue cumulée de l'anomalie
};

/**
 * @brief Remet le terme intégral pH dans son état de sortie de boot.
 *
 * Efface l'intégrale, l'épisode d'oubli, la base de temps et tout épisode
 * d'alerte en cours. Utilisé aux points de rupture de sens : changement de
 * vitesse de régulation, bascule `ph_minus` ↔ `ph_plus` (le signe de l'erreur
 * s'inverse), reset demandé depuis le web, péremption.
 */
inline void phIntegralReset(PhIntegralState& st) {
  st = PhIntegralState();
}

/**
 * @brief Débit moyen que l'actionneur peut TENIR, budget horaire compris.
 *
 * `ampFlow` est le débit pesé au banc PENDANT un pouls ; il n'est atteignable en
 * MOYENNE que si la pompe peut tourner toute la période. Or le budget horaire
 * (`phInjectionLimitMinutes`, défaut 5 min/h) borne la fraction de temps de
 * marche : le débit moyen soutenable vaut `ampFlow × minutes/60`.
 *
 * C'est CE plafond, et non `ampFlow`, que la demande PID peut réellement
 * dépasser — d'où son usage comme prédicat de saturation (cf. phIntegralTick).
 *
 * @param ampFlow           Débit mesuré à l'amplitude calibrée (mL/min)
 * @param budgetMinPerHour  Limite horaire en minutes (≤ 0 ou ≥ 60 → illimitée)
 * @return Débit moyen soutenable (mL/min) ; 0 si l'amplitude est invalide
 *         (pompe non caractérisée) — l'appelant doit alors traiter en fail-closed.
 */
inline float phSustainableAvgFlow(float ampFlow, int budgetMinPerHour) {
  if (!isfinite(ampFlow) || ampFlow <= 0.0f) return 0.0f;
  if (budgetMinPerHour <= 0 || budgetMinPerHour >= 60) return ampFlow;
  return ampFlow * (float)budgetMinPerHour / 60.0f;
}

/**
 * @brief Remplissage de l'anti-windup, en % du plafond (valeur ABSOLUE).
 *
 * Exposé à /data et au WebSocket. NaN si le plafond est nul ou l'intégrale non
 * finie. Valeur absolue : une saturation négative est tout aussi anormale.
 */
inline float phIntegralFillPct(float integral, float integralMax) {
  if (!(integralMax > 0.0f) || !isfinite(integral)) return NAN;
  return fabsf(integral) * 100.0f / integralMax;
}

/**
 * @brief Péremption : purge l'intégrale après kPhIntegralStaleResetMs sans tick.
 *
 * Appelée à CHAQUE tour d'update(), hors de toute conjonction d'éligibilité : en
 * mode `off`/`manual`, sous une garde bloquante durable ou filtration coupée, le
 * tick automatique cesse et l'intégrale resterait figée des jours avant d'être
 * servie intacte sur un bassin devenu étranger à la mesure qui l'a chargée.
 *
 * Soustraction non signée → wrap-safe (millis() boucle à ~49,7 j, bien au-delà
 * des 24 h comparées).
 *
 * @return true si la purge a effectivement abandonné une intégrale non nulle
 *         (la coquille journalise alors) ; false sinon.
 */
inline bool phIntegralExpire(PhIntegralState& st, uint32_t nowMs) {
  if (st.lastTickMs == 0) return false;  // jamais évalué : rien à périmer
  if ((uint32_t)(nowMs - st.lastTickMs) < kPhIntegralStaleResetMs) return false;
  const bool discarded = (st.integral != 0.0f);
  phIntegralReset(st);
  return discarded;
}

/**
 * @brief UN tick automatique du terme intégral pH.
 *
 * À appeler à chaque tour de la branche AUTOMATIQUE, que l'on dose ou non — c'est
 * précisément ce que l'ancienne coquille ne faisait pas. Ne calcule AUCUN débit :
 * la sortie reste produite par computePidPure(), gatée par `shouldDose` et par la
 * bande morte. Ce module ne fait qu'ENTRETENIR l'intégrale et la surveiller.
 *
 * Enchaînement, dans cet ordre exact :
 *   1. base de temps : reliquat `pendingMs`, rejet des dt aberrants ;
 *   2. RESET SEC TEMPORISÉ (excursion franche TENUE) — court-circuite tout ;
 *   3. demande `kp·e + ki·I` avec l'intégrale AVANT intégration, d'où le gel
 *      d'anti-windup (intégration conditionnelle classique) ;
 *   4. CHARGE SIGNÉE sur `(erreur − stopThreshold)`, par quanta ;
 *   5. OUBLI exponentiel sous consigne ;
 *   6. hystérésis de saturation ;
 *   7. anomalie « saturée ET erreur au-dessus du seuil d'arrêt », paliers et rappel.
 *
 * @param st   État persistant, modifié en place
 * @param in   Entrées du tick (temps, erreur, actionneur, quota)
 * @param tun  Réglages du correcteur
 * @return Verdict à appliquer/journaliser par la coquille
 */
inline PhIntegralVerdict phIntegralTick(PhIntegralState& st,
                                        const PhIntegralInputs& in,
                                        const PhIntegralTuning& tun) {
  PhIntegralVerdict v;
  v.integralBefore = st.integral;

  const bool errorUsable = isfinite(in.error);

  // --- 0. GRANDEURS D'OBSERVATION, calculées AVANT toute sortie anticipée -----
  // (correction v2.26.0, défaut mineur n°2) : le reset sec rendait le verdict
  // sans jamais renseigner `demandMlPerMin` / `sustainableMlPerMin`. Quand il
  // clôturait un épisode d'alerte, la coquille journalisait donc « sortie de
  // saturation (0,00 mL/min pour 0,00 soutenables) » — un message faux, et
  // trompeur au moment précis où l'on cherche à comprendre ce qui s'est passé.
  // Le débit soutenable est indépendant de l'intégrale : il est calculé une fois
  // ici et sert aussi bien au message qu'au prédicat de gel de l'étape 3.
  //
  // feature-085 — LE FEEDFORWARD EST RETRANCHÉ ICI, en mL/min MOYENS des deux
  // côtés : le soutenable que le PI peut réellement occuper est ce qui reste
  // après la part du FF (condition bloquante n°4 de la spec : anti-windup sur
  // « demande totale − FF »). Toutes les grandeurs en aval — prédicat de gel
  // (étape 3), hystérésis de saturation (étape 6), anomalie C5 (étape 7) et
  // messages — portent sur la demande du SEUL PI comparée à SA part : c'est
  // cohérent, le FF n'étant pas une demande d'erreur mais une charge connue.
  // Borne à 0 : un FF qui consommerait tout (impossible par construction, il est
  // plafonné à kPhFfMaxSustainableFraction < 1) dégénérerait en gel fail-closed.
  float sustainable = phSustainableAvgFlow(in.ampFlow, in.budgetMinPerHour);
  if (isfinite(in.ffMlPerMin) && in.ffMlPerMin > 0.0f) {
    sustainable -= in.ffMlPerMin;
    if (sustainable < 0.0f) sustainable = 0.0f;
  }
  v.sustainableMlPerMin = sustainable;

  // --- 1. BASE DE TEMPS ------------------------------------------------------
  // Aucun temps n'est SYNTHÉTISÉ : un trou d'évaluation (mesure invalide, mode
  // changé, filtration coupée) est REJETÉ, jamais crédité d'un bloc. Le reliquat
  // sous le quantum est reporté au tour suivant, donc aucun temps n'est perdu
  // non plus tant que la branche tourne.
  //
  // feature-084 : cette étape passe DEVANT le reset sec, qui a maintenant besoin
  // du `dt` pour accumuler sa tenue. Le reset réarme lui-même `lastTickMs` après
  // la purge, comme avant — l'ordre n'a aucun autre effet observable.
  uint32_t elapsed = 0;
  bool dtUsable = false;
  if (st.lastTickMs == 0) {
    st.lastTickMs = in.nowMs;  // premier tick : on ARME, on n'intègre pas
    st.pendingMs = 0;
  } else {
    elapsed = (uint32_t)(in.nowMs - st.lastTickMs);
    st.lastTickMs = in.nowMs;
    if (elapsed <= kPhIntegralMaxTickDtMs) {
      dtUsable = true;
      st.pendingMs += elapsed;
    } else {
      st.pendingMs = 0;  // trou d'évaluation : le reliquat n'a plus de sens
    }
  }

  // --- 2. RESET SEC TEMPORISÉ (feature-084) ----------------------------------
  // Le seul rôle qu'aucune décharge progressive ne remplit : déclarer
  // l'accumulation CADUQUE sur une excursion FRANCHE de l'autre côté de la
  // consigne (surcorrection réelle, apport externe, changement de produit) —
  // −0,15 avec les défauts, soit trois fois le bruit résiduel du filtre. Le
  // SEUIL est conservé à l'identique (exigence du gate).
  //
  // Ce qui change : la purge n'est plus INSTANTANÉE. L'excursion doit être
  // CONTINUE pendant kPhIntegralResetHoldMs de TEMPS DE BRANCHE, et toute
  // remontée au-dessus du seuil CONSTATÉE PAR UN TICK remet la tenue à zéro.
  // Motif : un transitoire de −0,15 (une pluie légèrement acide suffit) effaçait
  // deux jours et demi d'apprentissage, sans commune mesure avec le trouble
  // observé.
  //
  // PRÉCISION QUI COMPTE — « temps de branche » veut dire ce qu'il dit. Quand
  // phIntegralAccumulationAllowed() est faux (mode manuel, filtration coupée,
  // mesure invalide), phIntegralTick n'est PAS appelé : `resetHoldMs` est alors
  // GELÉ, pas remis à zéro. Une excursion de 10 min interrompue par une heure de
  // filtration à l'arrêt reprend donc à 10 min, et non à zéro. La direction reste
  // sûre — geler ne fait que RETARDER une purge, et une purge n'ajoute jamais
  // d'autorité de dosage — mais la remise à zéro exige un tick, pas une simple
  // absence d'excursion.
  //
  // CE DIFFÉRÉ NE SUSPEND AUCUNE PROTECTION. Pendant l'attente on tombe dans les
  // étapes 4 et 5 avec une erreur franchement négative : la décharge signée ET
  // l'oubli exponentiel travaillent tous les deux (−16 % d'intégrale sur les 15
  // minutes). L'étape 7 lève par ailleurs immédiatement toute alerte en cours,
  // l'anomalie exigeant une erreur POSITIVE. Seule la purge SÈCHE est différée,
  // le temps de vérifier que l'excursion est réelle et non un à-coup.
  const bool deepNegative =
      errorUsable && in.error < -(tun.startThreshold + kPhIntegralResetMargin);
  if (!deepNegative) {
    st.resetHoldMs = 0;  // remontée : la tenue repart de zéro
  } else {
    if (dtUsable) {
      // Saturation d'addition, comme la tenue d'anomalie : jamais de repliement.
      const uint32_t room = kPhIntegralHoldNever - st.resetHoldMs;
      st.resetHoldMs += (elapsed < room) ? elapsed : room;
    }
    if (st.resetHoldMs >= kPhIntegralResetHoldMs) {
      const bool wasAlerting = (st.warnSent || st.critSent);
      phIntegralReset(st);
      st.lastTickMs = in.nowMs;  // le tick a bien eu lieu : pas de péremption fantôme
      v.event = PhIntegralEvent::kDryReset;
      v.emitClear = wasAlerting;
      // Demande APRÈS la purge (intégrale nulle) : c'est l'état que décrit le
      // message « alerte levée ». Publier la demande d'avant la purge dirait
      // « saturé » dans la phrase même qui annonce la sortie de saturation.
      v.demandMlPerMin = tun.kp * in.error;
      return v;
    }
  }

  // --- 3. DEMANDE et GEL D'ANTI-WINDUP (C3) ---------------------------------
  // La demande est évaluée avec l'intégrale telle qu'elle est AVANT l'intégration
  // de ce tick (intégration conditionnelle classique) : on ne gèle jamais sur une
  // valeur qu'on vient soi-même de produire.
  //
  // Le prédicat porte sur le débit moyen SOUTENABLE (budget horaire compris), pas
  // sur le débit d'amplitude : comparer 8 mL/min de demande à ~25 mL/min
  // d'amplitude ne se déclenchait JAMAIS, et le gel « sur sonde saturée » exigé
  // par le gate ne protégeait donc de rien. Avec 5 min/h par défaut, le débit
  // soutenable vaut ~2 mL/min : c'est LUI que la demande dépasse réellement, et
  // tout ce qui s'accumule au-delà est du windup pur — un volume que
  // l'actionneur ne délivrera jamais.
  //
  // Fail-closed : amplitude invalide (pompe non caractérisée) ou demande non
  // finie ⇒ gel. Le second motif (quota journalier) est CONSERVÉ tel quel : il
  // couvre la sonde bloquée sur une valeur haute, que rien d'autre ne borne.
  //
  // TROISIÈME MOTIF (gate pool-chemistry, 30/08/2026) — QUOTA DE CYCLES ÉPUISÉ.
  // L'anti-cycling était traité comme une suspension rythmique, alors qu'il ne se
  // relève qu'au basculement de 24 h, exactement comme le quota volumique.
  // Épuisé à 10 h du matin, il laissait l'intégrale charger NEUF HEURES sur une
  // erreur que l'actionneur n'a plus le droit de résorber — de l'ordre de
  // 1 300 pH·s de windup pur, servis d'un bloc au premier cycle du lendemain.
  //
  // Le gel, et SURTOUT PAS le prédicat d'accumulation : geler ne bloque que la
  // direction qui aggrave (delta positif), la voie de DÉCHARGE reste ouverte —
  // l'oubli exponentiel et la décharge signée continuent de travailler. Fermer
  // le prédicat, lui, figerait l'intégrale à sa valeur haute.
  const float demand = tun.kp * in.error + tun.ki * st.integral;
  v.demandMlPerMin = demand;

  const bool outputSaturated =
      !(sustainable > 0.0f) || !isfinite(demand) || (demand >= sustainable);
  const bool freeze =
      outputSaturated || in.quotaNearLimit || in.cycleQuotaExhausted;
  v.frozen = freeze;

  // --- 4. CHARGE SIGNÉE ------------------------------------------------------
  // Par QUANTA d'une seconde (kPhIntegralChargeQuantumMs) et non à la cadence de
  // loop : à 16 200 pH·s, l'ulp du flottant vaut ~2e-3, si bien qu'un incrément
  // de quelques millisecondes × 0,01 pH (5e-5) serait purement et simplement
  // PERDU à l'addition. Le quantum garantit un incrément représentable.
  //
  // Le gel ne bloque que la direction qui AGGRAVE la saturation : une décharge
  // (delta négatif) reste toujours autorisée. C'est l'anti-windup correct, et
  // c'est strictement plus sûr que l'ancien gel total.
  if (errorUsable) {
    uint32_t steps = st.pendingMs / kPhIntegralChargeQuantumMs;
    if (steps > 0) {
      st.pendingMs -= steps * kPhIntegralChargeQuantumMs;
      const float dtSec =
          (float)steps * ((float)kPhIntegralChargeQuantumMs / 1000.0f);
      const float delta = (in.error - tun.stopThreshold) * dtSec;
      if (!(delta > 0.0f && freeze)) {
        st.integral += delta;
        if (st.integral > tun.integralMax) st.integral = tun.integralMax;
        if (st.integral < -tun.integralMax) st.integral = -tun.integralMax;
      }
    }
  } else {
    st.pendingMs = 0;  // mesure non finie : aucun temps ne doit être crédité
  }

  // --- 5. OUBLI EXPONENTIEL SOUS CONSIGNE (C2, inchangé) --------------------
  // Complète la décharge signée quand le pH part FRANCHEMENT sous la consigne
  // (bassin bâché, charge basique disparue) : à −0,02 pH la décharge signée ne
  // vaut que 108 pH·s/h, l'oubli à demi-vie d'une heure fait le reste.
  // Par pas fixes, sans powf() (2,3 Ko de flash sur une ressource à 97 %), et
  // borné à kPhIntegralDecayMaxSteps pour ne jamais faire tourner une longue
  // boucle dans update().
  //
  // ASYMÉTRIE VOULUE (relevée en test v2.26.0, conservée en connaissance de
  // cause) : cet oubli n'est PAS conditionné à `dtUsable`. Sa base de temps est
  // `st.decayMs`, indépendante du dt de charge. Conséquence : une branche
  // automatique qui ticke à plus de kPhIntegralMaxTickDtMs (10 s) voit toutes ses
  // CHARGES rejetées mais garde sa DÉCHARGE. C'est le sens fail-safe : la
  // décharge n'est active que sous `error < 0`, c'est-à-dire quand le pH est déjà
  // passé de l'autre côté de la consigne — abandonner de l'autorité de dosage y
  // est toujours le côté sûr de l'erreur. Symétriser (rejeter aussi la décharge)
  // reviendrait à conserver une autorisation de dosage pendant que le bassin
  // dérive dans la direction opposée, précisément lorsque la boucle est déjà
  // dégradée. On garde donc l'asymétrie.
  if (errorUsable && in.error < 0.0f && st.integral != 0.0f) {
    if (st.decayMs == 0) {
      st.decayMs = in.nowMs;
    } else {
      const uint32_t decayDtMs = (uint32_t)(in.nowMs - st.decayMs);
      uint32_t decaySteps = decayDtMs / kPhIntegralDecayStepMs;
      if (decaySteps > 0) {
        st.decayMs += decaySteps * kPhIntegralDecayStepMs;
        if (decaySteps > kPhIntegralDecayMaxSteps) decaySteps = kPhIntegralDecayMaxSteps;
        for (uint32_t i = 0; i < decaySteps; ++i) {
          st.integral *= kPhIntegralDecayStepFactor;
        }
        // Sous 1 pH·s la contribution est < 1e-3 mL/min : on ferme l'épisode
        // plutôt que de traîner un résidu exponentiel indéfiniment.
        if (fabsf(st.integral) < 1.0f) {
          st.integral = 0.0f;
          st.decayMs = 0;
          v.event = PhIntegralEvent::kDecayClosed;
        }
      }
    }
  } else {
    st.decayMs = 0;  // erreur revenue dans le domaine positif : épisode désarmé
  }

  // --- 6. HYSTÉRÉSIS DE SATURATION ------------------------------------------
  // Entrée à kPhIntegralSaturationPct de ce qui est soutenable, sortie seulement
  // à kPhIntegralSaturationClearPct. Sans cette bande, une demande qui oscille
  // autour du seuil ouvrirait et refermerait l'épisode en rafale, et le rappel
  // périodique se réarmerait à chaque oscillation.
  //
  // LE SEUIL PORTE SUR LA DEMANDE, JAMAIS SUR LE REMPLISSAGE (invariant du gate,
  // verrouillé par test natif). Une tentative de v2.26.0 y a ajouté un second
  // prédicat « |I| ≥ 95 % de integralMax » pour couvrir un budget horaire
  // ILLIMITÉ : elle est ABANDONNÉE, et pour deux raisons.
  //   1. Le budget horaire ne peut PAS être illimité — `phInjectionLimitMinutes`
  //      est borné à [1 ; 60] par la route de configuration et par la migration
  //      NVS. Le débit soutenable reste donc strictement inférieur à l'amplitude
  //      pesée, et le prédicat sur la demande garde toute sa portée.
  //   2. Remplissage et saturation sont DEUX GRANDEURS. Une intégrale au plafond
  //      avec une demande à 24 % du soutenable est une régulation qui MARCHE (elle
  //      tient la charge sans forcer) : alerter dessus dirait « vérifiez le bidon »
  //      sur une installation saine, c'est-à-dire exactement le faux positif que
  //      C5 doit éviter pour rester crédible. L'autorité, elle, est déjà bornée
  //      par le clamp sur integralMax — le remplissage n'a pas besoin d'alerte,
  //      il est publié tel quel (`ph_integral_pct`) pour le diagnostic.
  if (sustainable > 0.0f && isfinite(demand)) {
    const float ratioPct = demand * 100.0f / sustainable;
    if (!st.satActive) {
      if (ratioPct >= kPhIntegralSaturationPct) st.satActive = true;
    } else if (ratioPct < kPhIntegralSaturationClearPct) {
      st.satActive = false;
    }
  } else {
    st.satActive = false;  // actionneur non caractérisé : le gate le signale déjà
  }
  v.saturated = st.satActive;

  // --- 7. ANOMALIE, PALIERS ET RAPPEL (C5) ----------------------------------
  // « La régulation demande son maximum SANS atteindre la consigne » se dit en
  // DEUX termes, pas un : demande saturée ET erreur encore au-dessus du seuil
  // d'ARRÊT. Une demande saturée avec l'erreur déjà rentrée sous le seuil est un
  // transitoire de fin de correction, pas un défaut.
  //
  // La tenue se compte en temps de BRANCHE AUTOMATIQUE (somme des dt observés),
  // jamais en temps mural : un passage en `manual`, une garde bloquante ou un
  // hiver sans filtration SUSPENDENT l'épisode au lieu de le faire mûrir tout
  // seul — c'est ce qui permet d'évaluer l'anomalie ici, là où l'erreur existe.
  const bool anomaly =
      st.satActive && errorUsable && (in.error > tun.stopThreshold);

  if (!anomaly) {
    v.emitClear = (st.warnSent || st.critSent);
    st.anomalyHeldMs = 0;
    st.nextCriticalMs = 0;
    st.warnSent = false;
    st.critSent = false;
    return v;
  }

  if (dtUsable) {
    // Saturation d'addition : une tenue de 49 jours ne doit pas repartir à zéro.
    const uint32_t room = kPhIntegralHoldNever - st.anomalyHeldMs;
    st.anomalyHeldMs += (elapsed < room) ? elapsed : room;
  }
  v.anomalyHeldMs = st.anomalyHeldMs;

  // Critical d'abord : il couvre le palier inférieur. RÉPÉTÉ tous les
  // kPhIntegralSaturationReminderMs — un latch « une seule alerte par épisode »
  // laissait un silence PERMANENT sur un état anormal qui, lui, ne cesse pas.
  //
  // `kPhIntegralHoldNever` (0xFFFFFFFF) est la SENTINELLE « plus aucun rappel » :
  // la tenue étant elle-même saturée à cette valeur, une échéance qui l'atteint ne
  // doit plus jamais être considérée comme échue (sinon `>=` redeviendrait vrai à
  // chaque tour — le repliement corrigé ci-dessous).
  const bool reminderDue = st.critSent &&
                           st.nextCriticalMs != kPhIntegralHoldNever &&
                           st.anomalyHeldMs >= st.nextCriticalMs;
  if (st.anomalyHeldMs >= kPhIntegralSaturationCriticalMs &&
      (!st.critSent || reminderDue)) {
    st.critSent = true;
    st.warnSent = true;
    // SATURATION de l'échéance, comme la tenue elle-même (correction v2.26.0,
    // défaut mineur n°3) : `anomalyHeldMs + reminder` REPLIAIT dès que la tenue
    // approchait 0xFFFFFFFF. L'échéance repartait alors DERRIÈRE la tenue, la
    // condition `anomalyHeldMs >= nextCriticalMs` devenait vraie à chaque tour, et
    // une anomalie tenue 49,7 jours produisait un critical + une alerte MQTT à
    // CHAQUE tour de loop. Saturée, l'échéance devient inatteignable : le rappel
    // cesse au lieu de s'emballer — le critical initial, lui, a bien été émis.
    const uint32_t room = kPhIntegralHoldNever - st.anomalyHeldMs;
    st.nextCriticalMs = (kPhIntegralSaturationReminderMs < room)
                            ? (st.anomalyHeldMs + kPhIntegralSaturationReminderMs)
                            : kPhIntegralHoldNever;
    v.emitCritical = true;
    return v;
  }

  if (!st.warnSent && st.anomalyHeldMs >= kPhIntegralSaturationWarnMs) {
    st.warnSent = true;
    v.emitWarn = true;
  }
  return v;
}

#endif  // PH_INTEGRAL_LOGIC_H
