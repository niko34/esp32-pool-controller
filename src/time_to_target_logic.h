#ifndef TIME_TO_TARGET_LOGIC_H
#define TIME_TO_TARGET_LOGIC_H

// =============================================================================
// time_to_target_logic — TEMPS D'ATTEINTE DE CIBLE, logique PURE (feature-096)
// =============================================================================
// Patron ADR-0017 : aucun millis(), aucun time(), aucune NVS, aucun Arduino.h,
// aucune allocation. Le temps, l'heure, la mesure et les seuils sont des
// PARAMÈTRES ; le module est compilé en natif (`env:native`).
//
// CE QUE MESURE CE MODULE
// -----------------------
// Le temps mis par une régulation pour ATTEINDRE et TENIR sa cible, compté en
// MINUTES DE FILTRATION (compteur monotone `SafetyLimits::filtTotalMin`, jamais
// en minutes murales : une nuit sans circulation ne fait pas vieillir une
// journée de régulation).
//
// C'est un INSTRUMENT PASSIF. Aucune de ses sorties n'est lue par
// pump_controller : le sens est unique, de la régulation vers l'indicateur.
// Fermer la boucle (adapter un gain sur le KPI) exigerait une spec et un
// nouveau passage par pool-chemistry.
//
// RÉPARTITION
//   - ce fichier : TOUTES les décisions (machine d'états, garde de
//     vraisemblance, tenue, clôture, anneau 7 jours, sceaux) ;
//   - time_to_target.cpp : le câblage (mesures, heure, filtration, NVS, RTC,
//     journal) ;
//   - config.cpp : le transport NVS, rien d'autre.
// =============================================================================

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"  // kTimeToTarget*, kTttMaxPlausibleE0*

// Voies instrumentées — index FIGÉ (il indexe l'anneau NVS archivé).
constexpr uint8_t kTttChannelPh    = 0;
constexpr uint8_t kTttChannelOrp   = 1;
constexpr uint8_t kTttChannelCount = 2;

// Schéma de l'anneau NVS : toute évolution de `TttDay` ou de `TttHistory`
// l'incrémente (un enregistrement d'un autre schéma est REJETÉ, jamais relu).
constexpr uint8_t kTttHistorySchema = 1;

// Magic de la mémoire RTC — PROPRE à cet instrument ("KPIT"). Il ne réutilise
// NI kOutageRtcMagic (feature-091) NI aucune structure de la feature-093 : deux
// organes qui partageraient un magic se reliraient mutuellement au premier
// changement de taille.
constexpr uint32_t kTttRtcMagic = 0x4B504954UL;

// -----------------------------------------------------------------------------
// ENCODAGE FIGÉ DÈS LE PREMIER OTA
// -----------------------------------------------------------------------------
// `state` et `flags` sont archivés en NVS et servis à l'UI : leurs valeurs sont
// un CONTRAT. Ils vivent ici, à côté de la structure qu'ils décrivent (patron
// PhFfStatus / ph_feedforward_logic.h) et non dans constants.h, parce qu'ils ne
// sont pas des réglages mais la définition d'un format.

// État de la journée. 4-255 RÉSERVÉS (une pastille archivée d'un firmware futur
// doit rester lisible par un firmware plus ancien).
constexpr uint8_t kTttStateIdle    = 0;  // pas encore de départ aujourd'hui
constexpr uint8_t kTttStateRunning = 1;  // chronomètre en cours
constexpr uint8_t kTttStateReached = 2;  // cible atteinte et tenue
constexpr uint8_t kTttStateMissed  = 3;  // journée close sans tenue accomplie

// Drapeaux — CHAMP DE BITS (et non énumération) : une journée peut cumuler
// plusieurs circonstances, et les bits 4-7 restent disponibles sans casser les
// pastilles déjà archivées.
constexpr uint8_t kTttFlagTargetChanged = 0x01;  // consigne CONFIGURÉE modifiée
constexpr uint8_t kTttFlagBoost         = 0x02;  // boost ORP actif dans la journée
constexpr uint8_t kTttFlagUnsettled     = 0x04;  // purge ouverte au PLAFOND (091)
constexpr uint8_t kTttFlagPowerCycle    = 0x08;  // reprise après coupure secteur
// Bit 4 — RÉGULATION NON AUTOMATIQUE. La journée dit « INTERROMPUE », pas
// « non atteinte » : elle porte ses minutes et son e0, mais doit être EXCLUE
// des moyennes de comparaison de versions (une semaine de maintenance ne doit
// pas ressembler à une régression de la régulation).
//
// RÈGLE DE PARTAGE — ce que le régulateur CHOISIT compte ; ce qu'on lui IMPOSE
// de l'extérieur porte le bit :
//   - AUCUN bit (vraie non-atteinte, c'est précisément ce qu'on mesure) : quota
//     journalier atteint, limite horaire, actionneur sous-dimensionné (ADR-0056) ;
//   - BIT 4 (débranchement par l'opérateur) : mode manuel, mode maintenance,
//     assistant de mesure de débit.
constexpr uint8_t kTttFlagOperatorOff   = 0x10;

// -----------------------------------------------------------------------------
// UNE JOURNÉE ARCHIVÉE (12 octets, POD — aucun trou d'alignement)
// -----------------------------------------------------------------------------
struct TttDay {
  uint32_t date    = 0;    // AAAAMMJJ (0 = case vide)
  float    e0      = 0.0f; // écart initial SIGNÉ (valeur − consigne) à t0
  uint16_t minutes = 0;    // minutes de FILTRATION (borné à 1440 à l'insertion)
  uint8_t  state   = kTttStateIdle;
  uint8_t  flags   = 0;
};
static_assert(sizeof(TttDay) == 12, "TttDay : 12 octets attendus (schéma 1)");

// -----------------------------------------------------------------------------
// L'ÉTAT VIVANT D'UNE VOIE (28 octets, POD — logé en RAM RTC)
// -----------------------------------------------------------------------------
// `t0FiltMin` et `holdStartFiltMin` sont DEUX champs distincts, et c'est le
// cœur de la définition : une sortie de bande remet la TENUE à zéro, JAMAIS le
// chronomètre. Les confondre ferait repartir le temps d'atteinte à chaque
// oscillation autour de la consigne.
//
// AUCUN initialiseur de membre par défaut, VOLONTAIREMENT : cette structure est
// logée en RAM RTC (RTC_NOINIT_ATTR). Un initialiseur la rendrait non triviale,
// donc DYNAMIQUEMENT initialisée au démarrage — l'état vivant serait effacé à
// chaque reset, c'est-à-dire exactement ce que la mémoire RTC doit éviter. Les
// utilisateurs écrivent `TttLive x = {};` ou passent par tttLiveReset().
struct TttLive {
  uint32_t t0FiltMin;         // compteur de filtration à t0 — jamais réécrit
  uint32_t holdStartFiltMin;  // compteur de filtration au début de la tenue
  float    e0;                // écart initial signé
  float    watchTarget;       // consigne CONFIGURÉE photographiée à t0
  uint32_t date;              // jour civil du chronomètre (0 = aucun)
  uint16_t minutes;           // instantané servi à l'UI (recalculé)
  uint8_t  state;             // kTttState*
  uint8_t  flags;             // champ de bits kTttFlag*
  uint8_t  holding;           // 1 = tenue en cours
  uint8_t  pad[3];            // normalisés au scellement RTC
};
static_assert(sizeof(TttLive) == 28, "TttLive : padding inattendu");

// -----------------------------------------------------------------------------
// L'ANNEAU NVS — 7 journées × 2 régulations, UN seul blob
// -----------------------------------------------------------------------------
// `t0FiltMin[]` est le SEUL champ irrécupérable de l'état vivant : tout le reste
// se recalcule (minutes = filtTotalMin − t0FiltMin, et `filtTotalMin` survit
// déjà en NVS). Il est écrit À L'INSTANT OÙ t0 EST POSÉ, en même temps que
// l'entrée « en cours » du jour ; la clôture réécrit l'entrée et le remet à 0.
// D'où EXACTEMENT deux écritures par jour et par régulation.
//
// `serieStamp` (condition C6 du gate) : la valeur de kPhDecisionWindowMs à
// l'écriture. Le zéro du chronomètre pH est INDEXÉ sur cette fenêtre — la
// mesure ne peut pas commencer avant que le signal lent existe. Toute
// modification future de la fenêtre crée donc une DISCONTINUITÉ DE SÉRIE entre
// les journées archivées ; l'estampille la rend lisible plutôt que silencieuse.
struct TttHistory {
  uint8_t  schema = kTttHistorySchema;
  uint8_t  pad0 = 0;  // alignement — normalisés au scellement, jamais interprétés
  uint8_t  pad1 = 0;
  uint8_t  pad2 = 0;
  uint32_t serieStamp = 0;                  // C6 : kPhDecisionWindowMs à l'écriture
  uint32_t t0FiltMin[kTttChannelCount] = {0, 0};  // départ du jour EN COURS (0 = aucun)
  TttDay   days[kTttChannelCount][kTimeToTargetHistoryDays];
  uint32_t crc = 0;                         // FNV-1a des octets AMONT
};
static_assert(sizeof(TttHistory) == 188, "TttHistory : 188 octets attendus (schéma 1)");

// L'enregistrement RTC ne porte que le VOLATIL (progression de tenue, état
// courant). Il survit à un reset logiciel, jamais à une coupure secteur — la
// RAM RTC y est aléatoire, donc magic/CRC faux, donc reprise par la NVS.
// (Sans initialiseur de membre, pour la même raison que TttLive.)
struct TttRtcRecord {
  uint32_t magic;  // kTttRtcMagic
  TttLive  live[kTttChannelCount];
  uint32_t crc;    // FNV-1a des octets AMONT
};
static_assert(sizeof(TttRtcRecord) == 64, "TttRtcRecord : padding inattendu");

// -----------------------------------------------------------------------------
// SCEAUX (FNV-1a 32 bits — patron ph_persist_logic / measure_validity_logic)
// -----------------------------------------------------------------------------
// FNV-1a plutôt qu'un CRC32 tabulé : ~30 octets de flash contre ~1 Ko de table,
// sur une ressource occupée à 98,7 %. On ne se protège pas d'un adversaire mais
// d'un secteur NVS à demi écrit ou d'une RAM RTC aléatoire.
uint32_t tttFnv1a(const void* data, uint32_t len);

uint32_t tttHistoryCrc(const TttHistory& h);
// Scelle avant écriture : schéma courant, octets d'alignement normalisés,
// estampille de série posée, empreinte calculée.
void tttHistorySeal(TttHistory& h, uint32_t serieStamp);
// Taille EXACTE, schéma courant, empreinte concordante — sinon rejet net
// (fail-closed : anneau vide, aucune journée servie). Jamais de réinterprétation.
bool tttHistoryIntact(const TttHistory& h, size_t bytesRead);

uint32_t tttRtcCrc(const TttRtcRecord& r);
void tttRtcSeal(TttRtcRecord& r);
bool tttRtcIntact(const TttRtcRecord& r);

// -----------------------------------------------------------------------------
// ANNEAU : insertion et relecture
// -----------------------------------------------------------------------------
// La journée la plus récente est en days[chan][N-1] (l'ajout décale à gauche).
// Une date DÉJÀ présente est REMPLACÉE : c'est ce qui permet à la clôture de
// réécrire l'entrée « en cours » posée à t0 sans doubler la journée.
// @return true si l'anneau a changé (donc s'il faut l'écrire). Date nulle
//         refusée : sans horloge, une journée n'est pas datable.
bool tttHistoryAppend(TttHistory& h, uint8_t chan, const TttDay& d);

// Entrée d'une date donnée, ou nullptr. Sert à la reprise après coupure
// secteur : retrouver l'entrée « en cours » du jour civil courant.
const TttDay* tttHistoryFind(const TttHistory& h, uint8_t chan, uint32_t date);

// RATTRAPAGE DES JOURNÉES ORPHELINES (revue feature-096, bloquant n°1).
//
// Une entrée est archivée dès la pose de t0, à l'état « en cours » et 0 minute :
// c'est ce qui rend le t0 récupérable après une coupure secteur. Mais si
// l'appareil reste éteint plus d'une journée civile, la reprise ne cherche que
// la date DU JOUR — l'entrée de la veille resterait figée « en cours, 0 min »
// jusqu'à sortir de l'anneau au bout de 7 jours. L'interface afficherait
// « 0h00 » et annoncerait « journée en cours » pour une journée révolue : un
// faux positif sur la métrique qui arbitre les comparaisons de versions.
//
// On clôt donc ces journées en NON ATTEINTE, avec le drapeau de coupure. Leurs
// minutes sont irrécupérables : laisser 0 est la seule valeur honnête — mieux
// vaut une journée manquante qu'une journée fausse.
//
// TROIS GARDES, chacune couverte par mutation :
//   - `today == 0` (heure invalide) ⇒ on ne touche à RIEN. On ne sait pas quel
//     jour on est, donc rien n'est « révolu » : sans cette garde, un démarrage
//     sans NTP détruirait sept journées d'observation.
//   - `d.date != today` ⇒ la journée EN COURS est épargnée.
//   - `d.state == kTttStateRunning` ⇒ une journée ATTEINTE n'est jamais
//     rouverte en non-atteinte (ce serait fabriquer une régression apparente).
//
// N'écrit pas en NVS : la resynchronisation se fera à la prochaine archive.
// @return nombre de journées closes (0 = anneau inchangé).
uint8_t tttHistoryCloseOrphans(TttHistory& h, uint32_t today);

// -----------------------------------------------------------------------------
// LA MACHINE D'ÉTATS
// -----------------------------------------------------------------------------

struct TttInputs {
  uint32_t date             = 0;    // AAAAMMJJ ; 0 = heure INEXPLOITABLE
  uint32_t filtTotalMin     = 0;    // compteur MONOTONE de minutes de filtration
  float    value            = NAN;  // grandeur de DÉCISION (NAN = absence)
  float    target           = NAN;  // consigne EFFECTIVE — celle qui JUGE
  float    configuredTarget = NAN;  // consigne CONFIGURÉE — celle qu'on SURVEILLE
  float    band             = NAN;  // seuil de DÉMARRAGE vivant (bande d'atteinte)
  float    maxPlausibleE0   = 0.0f; // garde de vraisemblance sur e0
  bool     filtrationRunning = false;
  bool     dayEnd           = false;  // la filtration est arrêtée POUR LA JOURNÉE
  // La régulation de CETTE voie est-elle aux commandes ? false = débranchée par
  // l'OPÉRATEUR (manuel, maintenance, assistant de débit) ⇒ clôture immédiate
  // avec le bit 4. Les brides que la régulation s'impose elle-même (quota,
  // limite horaire, actionneur saturé) ne touchent PAS à ce drapeau.
  bool     automatic        = true;
  bool     boostActive      = false;  // surcouche ORP active (bit 1, pas bit 0)
  bool     unsettled        = false;  // purge ouverte au plafond (bit 2)
};

enum class TttEvent : uint8_t {
  kNone = 0,
  kStarted,      // t0 posé — une écriture NVS
  kReached,      // tenue accomplie — la seconde et dernière écriture NVS
  kMissed,       // journée close sans tenue — la seconde et dernière écriture
  kImplausible,  // e0 hors plage : REFUS de démarrer (aucun t0, aucune écriture)
};

struct TttVerdict {
  TttEvent event    = TttEvent::kNone;
  TttDay   entry;                  // entrée à archiver (kStarted/kReached/kMissed)
  float    residual = NAN;         // écart résiduel à la clôture (NAN = indisponible)
  float    rejected = NAN;         // e0 refusé par la garde (kImplausible)
};

// Minutes de filtration écoulées depuis t0, bornées à 1440 (un uint16 ne porte
// pas plus qu'une journée, et une journée n'a pas plus de 1440 minutes).
uint16_t tttElapsedMinutes(const TttLive& st, uint32_t filtTotalMin);

// Remet la voie à l'état 0 (aucun départ) — bascule de jour, heure perdue.
void tttLiveReset(TttLive& st);

// Photographie de l'état vivant sous forme d'entrée archivable.
TttDay tttEntryFromLive(const TttLive& st);

/**
 * @brief UN tick de la machine d'états (cadence 1 s côté coquille).
 *
 * Enchaînement, dans cet ordre exact :
 *   1. heure inexploitable ⇒ suspension (état 0), RIEN n'est daté ;
 *   2. bascule de jour civil ⇒ la journée en cours se clôt en « non atteinte »
 *      (cas de la filtration manuelle qui tourne la nuit) ;
 *   3. drapeaux vivants (consigne CONFIGURÉE modifiée, boost, purge au plafond) ;
 *   4. fin de journée de filtration ⇒ clôture en « non atteinte » ;
 *   5. état 0 ⇒ tentative de départ, sous garde de vraisemblance ;
 *   6. état 1 ⇒ tenue dans la bande, puis atteinte.
 *
 * Une mesure NON FINIE vaut SORTIE DE BANDE (C3 du gate) : jamais 0, jamais
 * « dans la bande ». Elle empêche le départ et remet la tenue à zéro, sans
 * jamais arrêter le chronomètre.
 *
 * @param st État vivant de la voie, modifié en place
 * @param in Entrées du tick
 * @return L'événement du tick (au plus UN par tick) et l'entrée à archiver.
 */
TttVerdict tttTick(TttLive& st, const TttInputs& in);

#endif  // TIME_TO_TARGET_LOGIC_H
