#ifndef FOPEN_GUARD_LOGIC_H
#define FOPEN_GUARD_LOGIC_H

// =============================================================================
// fopen_guard_logic — Admission des `fopen` INTERNES : décision PURE (v2.42.x)
// =============================================================================
// bug-100 / ADR-0070. Le firmware ouvre 14 fichiers pour son PROPRE compte. Sur
// ESP32, `fopen()` fait allouer à newlib un verrou récursif de 92 o
// (caps = 0x804 = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) ; si ce `malloc`
// échoue, `lock_init_generic` appelle **abort()** — il n'y a AUCUN code de
// retour à tester. Le parachute OOM (std::set_new_handler) est inopérant ici PAR
// CONSTRUCTION : c'est `malloc(3)`, pas `operator new`. La seule défense est de
// ne pas entrer dans `fopen`.
//
// Trois décisions, aucune collecte, aucun effet :
//   1. fopenGuardShouldDefer() ...... faut-il refuser cette ouverture ?
//   2. fopenGuardNeedsFreshMeasure() faut-il PAYER un parcours du tas pour
//      décider ? C'est ici que vit l'ASYMÉTRIE MESURE/DÉCISION (cf. plus bas).
//   3. fopenGuardDrain() ............ machine à états de l'épisode, évaluée
//      depuis la loop : quelques lignes par épisode, jamais une par refus.
//
// INVARIANT (ADR-0017) : ce module ne fait QUE décider à partir de valeurs déjà
// collectées. La mesure (heapLargestFreeBlock8Bit, millis), les compteurs
// atomiques et l'émission des lignes restent dans la coquille impure fs_guard.cpp.
//
// ⚠️ CONTRAINTE DE SÛRETÉ (ADR-0067, reconduite par ADR-0070 §f) : logger.cpp
// flushe EN SYNCHRONE dans la tâche appelante sur ERROR et CRITICAL →
// open("/system.log","a") → fopen() → exactement l'abort() prévenu. Tout chemin
// de garde est donc WARN/INFO uniquement. Ce header, purement arithmétique, ne
// peut structurellement rien émettre.
//
// CONTRAINTE DE PORTABILITÉ : pas d'Arduino.h, pas de FreeRTOS, pas d'ESP.*, pas
// de String. En-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// ---------------------------------------------------------------------------
// 1. Prédicat de refus
// ---------------------------------------------------------------------------
/**
 * @brief Faut-il différer/refuser cette ouverture de fichier ?
 *
 * @param maxAlloc8      heapLargestFreeBlock8Bit() — plus gros bloc contigu du
 *                       pool MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT, c'est-à-dire
 *                       EXACTEMENT le pool où est servi le verrou qui aborte
 *                       (caps 0x804 relevés au coredump du 05/09/2026).
 * @param minAllocBytes  seuil de la CLASSE DE CONTENTION du site (ADR-0070 §b) :
 *                       8 192 (classe A, contention TCP possible) ou 5 120
 *                       (classe B, sites de boot). Le seuil reste un PARAMÈTRE :
 *                       c'est ce qui permet la partition par classe sans
 *                       dupliquer les signatures.
 *                       0 = « admission déjà prononcée en amont » → jamais de
 *                       refus (cf. kFsGuardAlreadyAdmitted, fs_guard.h).
 * @return true si l'ouverture doit être refusée.
 *
 * Frontière STRICTE : `< seuil` refuse, `>= seuil` ouvre.
 */
bool fopenGuardShouldDefer(uint32_t maxAlloc8, uint32_t minAllocBytes);

// ---------------------------------------------------------------------------
// 2. État d'épisode + asymétrie mesure/décision
// ---------------------------------------------------------------------------
/**
 * @brief État de l'épisode de famine courant. Détenu par la coquille impure.
 *
 * Certains champs sont écrits au POINT DE DÉCISION (donc depuis n'importe quelle
 * tâche : loopTask, async_tcp, mqttTask…), d'autres depuis la loop qui draine.
 * Aucun verrou n'est pris, et c'est SÛR par construction : une lecture périmée
 * ne peut produire qu'un REFUS périmé (coût : une sauvegarde différée), jamais
 * une AUTORISATION périmée (coût : un abort()). Toute autorisation passe par une
 * mesure fraîche — cf. fopenGuardNeedsFreshMeasure().
 */
struct FopenGuardEpisode {
  // ⚠️ DEUX DRAPEAUX, PAS UN — bug fs_guard-episode-start-inatteignable.
  // `active` est écrit par fopenGuardNoteRefusal(), APPELÉE au POINT DE
  // DÉCISION par fsGuardAdmit(), depuis N'IMPORTE QUELLE tâche : c'est le cache
  // DU REFUS (rôle inchangé). `reported` n'appartient qu'au DRAIN (loop) : « ai-je
  // déjà émis l'ouverture de cet épisode ? ». Avant le correctif d'origine, le
  // drain testait `!st.active` pour détecter le front montant — or `active` est
  // TOUJOURS déjà vrai quand le drain s'exécute, puisque fsGuardAdmit() l'a posé
  // avant lui. La branche EpisodeStart était donc structurellement inatteignable.
  // Le correctif suivant a déplacé le front montant dans la coquille impure
  // (`fs_guard.cpp`) : la RÈGLE y était de nouveau hors de portée de la
  // mutation — mêmes symptômes, autre adresse. `fopenGuardNoteRefusal()`
  // rapatrie la règle dans ce module pur : c'est elle, et elle seule, qui pose
  // `active`/`episodeStartMs` sur le front montant.
  bool     active = false;            // un épisode de refus est ouvert (cache DU REFUS)
  bool     reported = false;          // le DRAIN a déjà émis l'ouverture de CET épisode
  bool     probeFloorArmed = false;   // dernier relevé < 2 × seuil → cache autorisé
  bool     escalated = false;         // l'escalade de l'épisode a déjà été émise
  uint32_t drainedDeferrals = 0;      // dernière valeur du compteur vue par la loop
  uint32_t deferralsAtStart = 0;      // valeur du compteur juste AVANT l'épisode
  // Posé par fopenGuardNoteRefusal() sur le FRONT MONTANT de `active` (donc au
  // 1er refus réel, pas au 1er drain). Le drain ne fait que le LIRE pour
  // chiffrer l'épisode qu'il clôt.
  uint32_t episodeStartMs = 0;        // millis du 1er refus de l'épisode
  uint32_t lastDeferralMs = 0;        // millis du dernier refus (point de décision)
  uint32_t lastProbeMs = 0;           // millis du dernier parcours du tas payé
};

/**
 * @brief Note un refus au POINT DE DÉCISION (fsGuardAdmit(), n'importe quelle
 * tâche) et pose le FRONT MONTANT de l'épisode.
 *
 * Seule autorité sur `episodeStartMs`/`active` : `episodeStartMs` n'est
 * stampé QUE si `st.active` était encore faux (donc au tout premier refus de
 * l'épisode, jamais au premier drain — cf. bug
 * fs_guard-episode-start-inatteignable). `active` est ensuite posé
 * inconditionnellement et `lastDeferralMs` mis à jour, que ce soit le front
 * montant ou un refus ultérieur du même épisode.
 *
 * À appeler sur les DEUX chemins de refus de fsGuardAdmit() : le chemin
 * mesuré ET le chemin servi par le cache. Sur le chemin en cache, l'appel est
 * un NO-OP sur `active`/`episodeStartMs` (le cache n'est armé que si un
 * épisode est déjà ouvert, donc `st.active` y est vrai par construction) —
 * seul `lastDeferralMs` y change, comme avant l'extraction.
 *
 * @param st     état d'épisode, mis à jour en place
 * @param nowMs  millis() au moment du refus
 * @return true si c'était le FRONT MONTANT (premier refus de l'épisode).
 */
bool fopenGuardNoteRefusal(FopenGuardEpisode& st, uint32_t nowMs);

/**
 * @brief Durée écoulée depuis le début de l'épisode couvert par `st`.
 *
 * N'a de sens qu'au moment où l'appelant clôt l'épisode (FopenGuardAction::
 * EpisodeEnd) : `episodeStartMs` survit à la clôture (fopenGuardDrain() ne le
 * réinitialise pas) précisément pour permettre ce calcul après coup.
 * Soustraction non signée : juste au repliement de millis() (24,8 j), comme
 * le reste du module.
 */
uint32_t fopenGuardEpisodeDurationMs(const FopenGuardEpisode& st, uint32_t nowMs);

/**
 * @brief Faut-il payer un parcours du tas pour décider ?
 *
 * ⚠️ C'EST LE CŒUR DE LA CONCEPTION (ADR-0070 §e). L'asymétrie :
 *
 *   > **Autoriser** exige une mesure fraîche. **Refuser** n'en exige aucune.
 *   > Une autorisation périmée coûte un `abort()`.
 *   > Un refus périmé coûte une sauvegarde différée de 5 min.
 *
 * ADR-0058:99 verrouille que toute garde heap lit `heapLargestFreeBlock8Bit()`,
 * et ADR-0058:79 rejette NOMMÉMENT la mutualisation avec l'échantillon 2 s (des
 * effondrements de ~45 Ko ont été observés entre deux échantillons). Or le coût
 * d'un parcours TLSF CROÎT avec la fragmentation : il est maximal exactement
 * dans le régime pour lequel la garde existe. On ne met donc en cache QUE LE
 * REFUS : la fraîcheur est intégralement respectée là où elle décide quelque
 * chose, et le coût s'effondre dans le régime qui produit les rafales.
 *
 * @param st            état d'épisode
 * @param nowMs         millis()
 * @param quietMs       kInternalFopenEpisodeQuietMs — silence de refus exigé
 *                      avant d'envisager de re-mesurer (donc de sortir)
 * @param probeFloorMs  kInternalFopenProbeMinIntervalMs — plancher DUR entre
 *                      deux parcours de tas
 * @return false ⇒ l'appelant REFUSE sans mesurer (strictement conservateur).
 *
 * Le plancher n'est armé que si le dernier relevé était sous 2 × seuil
 * (`probeFloorArmed`) : en régime confortable, chaque décision re-mesure et le
 * cache ne peut produire aucun refus périmé.
 * Soustractions non signées : justes au repliement de millis() (24,8 j).
 */
bool fopenGuardNeedsFreshMeasure(const FopenGuardEpisode& st, uint32_t nowMs,
                                 uint32_t quietMs, uint32_t probeFloorMs);

// ---------------------------------------------------------------------------
// 3. Machine à états du bornage du journal (exécutée depuis la loop)
// ---------------------------------------------------------------------------
enum class FopenGuardAction : uint8_t {
  None = 0,      // rien à journaliser
  EpisodeStart,  // front montant   → WARN (jamais error/critical)
  Escalate,      // N refus consécutifs → alerte MQTT + compteurs retenus
  EpisodeEnd     // front descendant → INFO + critical() DIFFÉRÉ
};

/**
 * @brief Drain du compteur de refus + transitions d'épisode.
 *
 * @param st            état d'épisode (mis à jour en place)
 * @param deferCount    valeur courante du compteur cumulé de refus
 * @param nowMs         millis()
 * @param maxAlloc8     mesure du tas, PERTINENTE seulement si
 *                      fopenGuardNeedsFreshMeasure() a renvoyé true ; sinon
 *                      passer 0 (0 < seuil ⇒ aucune fermeture d'épisode, donc
 *                      aucune autorisation prononcée sans mesure fraîche)
 * @param threshold     seuil de la classe la plus exigeante en vigueur
 * @param escalateAfter kInternalFopenEscalateAfterRefusals
 * @return l'action à journaliser, une seule par appel.
 *
 * Règles, dans cet ordre :
 *   a) compteur qui a bougé → l'épisode vit ; si le drain ne l'a pas encore
 *      RAPPORTÉ (`!st.reported`, et non `!st.active` — cf. FopenGuardEpisode),
 *      on émet l'ouverture (EpisodeStart). UN seul WARN, même pour 200 refus.
 *      `episodeStartMs` n'est PAS posé ici : fopenGuardNoteRefusal() l'a déjà
 *      fait, appelée par fsGuardAdmit() au premier refus réel.
 *   b) épisode déjà rapporté, seuil d'escalade atteint et pas encore escaladé →
 *      Escalate (une seule fois par épisode).
 *   c) épisode ouvert (`st.active`) et tas rétabli (mesure FRAÎCHE >= seuil) →
 *      EpisodeEnd, qui remet `active` ET `reported` à false (sans quoi le
 *      prochain épisode ne serait jamais détecté). `episodeStartMs` et
 *      `deferralsAtStart` ne sont PAS remis à zéro : la coquille en tire la
 *      durée et le total de l'épisode qu'elle vient de clore.
 *   d) sinon → None.
 */
FopenGuardAction fopenGuardDrain(FopenGuardEpisode& st, uint32_t deferCount,
                                 uint32_t nowMs, uint32_t maxAlloc8,
                                 uint32_t threshold, uint32_t escalateAfter);

#endif  // FOPEN_GUARD_LOGIC_H
