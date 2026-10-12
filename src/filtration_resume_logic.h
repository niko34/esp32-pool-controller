#ifndef FILTRATION_RESUME_LOGIC_H
#define FILTRATION_RESUME_LOGIC_H

// =============================================================================
// filtration_resume_logic — Reprise de la durée de filtration PURE (feature-158)
// =============================================================================
// La durée de filtration SERVIE (filtration_elapsed_s) repartait de zéro à
// chaque redémarrage logiciel, même quand la pompe n'avait jamais cessé de
// tourner — alors que le firmware SAVAIT déjà (measure_validity) que l'eau
// n'avait pas cessé de circuler.
//
// ⚠️ LA FRONTIÈRE, OPPOSABLE EN REVUE (verdict pool-chemistry, feature-158) :
// `measure_validity` est le PRODUCTEUR UNIQUE du verdict de continuité de
// l'eau (OutageContinuityVerdict, measure_validity_logic.h). Ce module-ci EST
// UN CONSOMMATEUR : il ne persiste et ne décide jamais du verdict lui-même,
// uniquement d'une DONNÉE (le nombre de secondes RÉELLEMENT mesurées de
// filtration avant la coupure). Aucune comparaison de durée de coupure
// (`outageS`, `kWaterStopGraceS`) ne doit apparaître ici ni dans
// `filtration.{h,cpp}` — le test de non-régression est un grep de ces trois
// jetons sur `src/filtration.*`, qui doit rendre zéro occurrence hors
// commentaire explicatif.
//
// CONTRAINTE (style ADR-0017, comme measure_validity_logic/ph_tune_logic) :
// pas d'Arduino.h, pas de FreeRTOS, pas de <String>. `uint32_t` EXPLICITE
// PARTOUT (jamais `unsigned long`, qui vaut 64 bits en natif — un vecteur de
// repliement de millis() écrit avec lui ne prouverait rien).
// =============================================================================

#include <stdint.h>

#include "dosing_logic.h"            // InstallMode (enum pur)
#include "measure_validity_logic.h"  // OutageContinuityVerdict (producteur unique)

// =============================================================================
// Enregistrement RTC scellé (16 o, MÊME PATRON que OutageRtcRecord —
// measure_validity_logic.h) : porte la durée MESURÉE du cycle de filtration
// courant à travers un redémarrage logiciel. RTC_NOINIT_ATTR côté coquille
// (filtration.cpp) : survit à un reset logiciel/panic/watchdog, PAS à une
// coupure secteur (RAM RTC aléatoire ⇒ magic/CRC faux ⇒ fail-closed).
// =============================================================================

// "FLTR" — magic PROPRE à la filtration (ne jamais confondre avec
// kOutageRtcMagic "PURG" de measure_validity : deux enregistrements distincts,
// deux producteurs distincts).
constexpr uint32_t kFiltrationRtcMagic = 0x464C5452UL;

struct FiltrationRtcRecord {
  uint32_t magic;              // kFiltrationRtcMagic
  // Condition 1 du gate — IMPOSÉ MOT POUR MOT :
  // La coupure n'est JAMAIS créditée : ce compteur ne porte que des secondes
  // RÉELLEMENT mesurées, pompe commandée ON et appareil vivant. Sous-estimer
  // est sans danger, sur-estimer lève une garde de dosage.
  uint32_t carryS;
  // Condition 2 du gate — preuve PROPRE à la filtration, jamais déduite du
  // `waterPresent` d'un verdict de continuité (présumé en modes externes, il
  // ne dit rien du relais).
  uint8_t filtrationRunning;    // 1 = pompe COMMANDÉE ON au dernier rafraîchissement
  uint8_t installMode;          // InstallMode au dernier rafraîchissement
  uint8_t reserved[2];          // 0
  uint32_t crc;                 // FNV-1a des octets précédents
};
static_assert(sizeof(FiltrationRtcRecord) == 16, "FiltrationRtcRecord : padding inattendu");

// FNV-1a 32 bits (même algorithme que outageFnv1a, measure_validity_logic) —
// dupliqué à dessein : ce module reste autonome, sans dépendance croisée sur
// la coquille de measure_validity.
uint32_t filtrationRtcFnv1a(const void* data, uint32_t len);
uint32_t filtrationRtcCrc(const FiltrationRtcRecord& r);
void filtrationRtcSeal(FiltrationRtcRecord& r);
bool filtrationRtcIntact(const FiltrationRtcRecord& r);

// Condition 5 du gate — borne de VRAISEMBLANCE, pas un plafond de confort :
// au-delà, le report entier devient suspect (boucle de redémarrage
// anormalement longue, horloge aberrante) ⇒ retour à ZÉRO, JAMAIS écrêtage.
// Décision PRODUIT (ADR-0098) : aucune installation ne fait courir un même
// cycle de filtration au-delà d'une journée pleine sans que l'utilisateur ou
// le planning ne l'arrête — 24 h est la limite de plausibilité physique d'un
// cycle continu, pas une valeur calée sur un bassin particulier.
constexpr uint32_t kFiltrationResumeMaxPlausibleS = 86400UL;  // 24 h

// Motif de la décision (journal, condition 9 du gate).
enum class FiltrationResumeMotif : uint8_t {
  kNoRecord = 0,      // mémoire RTC absente ou corrompue (démarrage à froid)
  kNotRunning,        // la pompe n'était pas commandée ON avant la coupure
  kNotManaged,        // mode d'installation (mémorisé ou courant) non géré
  kNotInitialRun,      // condition 3 : ce n'est pas le tout premier démarrage de l'uptime (exclut le Boost)
  kVerdictNotReady,   // condition 13 : measure_validity n'a pas encore produit son verdict
  kDenied,            // measure_validity a refusé la dispense (verdict.waive == false)
  kImplausible,       // condition 5 : report au-delà de la borne de vraisemblance
  kResumed,           // report accordé
};

// Entrées de la décision — consommées UNE SEULE FOIS par filtration.begin()
// (condition 3 du gate), jamais recalculées pendant la marche.
struct FiltrationResumeInputs {
  const FiltrationRtcRecord* record;        // nullptr = absent/non lu
  const OutageContinuityVerdict* verdict;   // nullptr = condition 13 : non produit
  InstallMode installMode;                  // mode d'installation ACTUEL (boot courant)
  bool initialState;                        // decideFiltrationRun() au boot courant
};

struct FiltrationResumeResult {
  uint32_t carryS;                 // secondes à créditer (0 si aucun report)
  FiltrationResumeMotif motif;
  // UNIQUEMENT pour le journal (condition 9) — jamais réinjecté dans un
  // calcul : afficher la durée de coupure n'est pas la comparer.
  uint32_t coupureDureeS;
};

// Décision pure (voir la frontière en tête de fichier). `in.verdict` est LU,
// jamais recalculé : c'est tout l'objet de ce module.
FiltrationResumeResult evaluateFiltrationResume(const FiltrationResumeInputs& in);

// Libellé court du motif (journal), pour la ligne "Reprise à zéro (<motif>)".
const char* filtrationResumeMotifLabel(FiltrationResumeMotif m);

// =============================================================================
// Durée écoulée du cycle courant — arithmétique WRAP-SAFE (feature-158, A5/B2)
// =============================================================================
// Extrait de filtration.h (FiltrationManager::filtrationElapsedMs()), qui ne
// peut pas être exercé par les tests natifs (dépendance Arduino.h/millis()).
// `uint32_t` EXPLICITE PARTOUT — jamais `unsigned long` (64 bits en natif, qui
// rendrait tout vecteur de repliement inopérant). Soustraction non signée 32
// bits : le repliement de `millis()` à 2^32 ms se résorbe naturellement par
// arithmétique modulaire, SI ET SEULEMENT SI le calcul reste en 32 bits de
// bout en bout.
// Le test `!running` (sentinelle de l'accesseur public) reste dans
// filtration.h : c'est de l'état, pas de l'arithmétique.
uint32_t computeFiltrationElapsedMs(uint32_t nowMs, uint32_t startedAtMs, uint32_t carryMs);

#endif  // FILTRATION_RESUME_LOGIC_H
