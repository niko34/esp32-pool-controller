#ifndef MEASURE_VALIDITY_LOGIC_H
#define MEASURE_VALIDITY_LOGIC_H

// =============================================================================
// measure_validity_logic — Validité des mesures pH/ORP PURE
// (feature-061, réécrite par la feature-091 v2.34.0 : purge ADAPTATIVE)
// =============================================================================
// Décision « les mesures pH/ORP sont-elles représentatives de l'eau du bassin ? »
// extraite en module pur pour être testable en natif (sans ESP32, sans FreeRTOS,
// sans Arduino). Style ADR-0017, comme dosing_logic / ph_persist_logic.
//
// Les sondes sont montées dans le circuit de filtration : filtration arrêtée =
// eau stagnante autour des sondes → la valeur dérive localement et ne reflète
// plus le bassin. À la reprise de la circulation, une PURGE est appliquée
// (l'eau stagnante de la chambre de mesure doit être chassée, puis le
// transitoire de mélange doit s'éteindre) avant de redéclarer les mesures
// valides. Depuis la feature-091, la purge n'est plus une minuterie fixe : elle
// se termine quand les bruts sont STABLES (purge_stability_logic), encadrée par
// un plancher et un plafond.
//
// MACHINE D'ÉTAT (une transition par tick, priorité décroissante) :
//   1. eau absente            → porte FERMÉE, pas de purge, compteur de
//                               re-purges remis à zéro au front descendant ;
//   2. front montant d'eau    → PURGE démarre (t0 = ce tick). Si une DISPENSE
//                               est présentée (`purgeWaived`, coupure courte C3)
//                               → porte OUVERTE immédiatement, motif kWaived,
//                               durée 0 ;
//   3. purge en cours         → OUVERTURE si (écoulé ≥ floorMs ET stable), motif
//                               kStability (ou kRepurge si c'est une re-purge) ;
//                               sinon OUVERTURE si écoulé ≥ capMs, motif kCap
//                               (fail-safe : la coquille alerte) et la
//                               surveillance de re-fermeture est DÉSARMÉE
//                               jusqu'au prochain front montant (C2) ;
//   4. porte ouverte          → si surveillance armée ET `unstable` → RE-FERMETURE
//                               (C1) : nouvelle purge (t0 = ce tick),
//                               repurgeCount++, jusqu'à plancher + stabilité.
//
// La DISPENSE n'est honorée qu'au front montant du tick où elle est présentée :
// la coquille ne la présente qu'au premier tick après le boot (eau déjà
// présente ⇒ front montant vu au 1er tick) — un front ultérieur purge normalement.
//
// Toutes les soustractions de temps sont NON SIGNÉES (wrap de millis() sûr).
//
// INVARIANT : ce module ne fait QUE décider à partir de valeurs déjà collectées.
// La collecte (resolveWaterPresence(), millis(), détecteur de stabilité, mémoire
// RTC de coupure) et les logs restent dans la coquille impure measure_validity.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. On
// utilise l'en-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// Motif de fin de la DERNIÈRE purge terminée — valeurs figées : exposées telles
// quelles dans /data et le WebSocket (`measures_purge_last_end`).
enum class PurgeEnd : uint8_t {
  kNone = 0,       // aucune purge terminée depuis le boot
  kStability = 1,  // ouverte par stabilité (purge initiale)
  kCap = 2,        // ouverte au plafond (fail-safe, alerte)
  kWaived = 3,     // dispensée (coupure courte, C3)
  kRepurge = 4,    // ouverte par stabilité après une RE-purge (C1)
};

// Événement du tick, pour le journal de la coquille (une seule transition/tick).
enum class MeasureValidityEvent : uint8_t {
  kNone = 0,
  kWaterDrop,      // front descendant : porte fermée
  kPurgeStarted,   // front montant : purge démarrée
  kOpened,         // porte ouverte (voir state.lastPurgeEnd pour le motif)
  kReclosed,       // re-fermeture (C1) : nouvelle purge démarrée
};

// Entrées de la décision — toutes fournies par l'appelant (aucune API Arduino).
struct MeasureValidityInputs {
  bool waterPresent;   // resolveWaterPresence().waterPresent (source UNIQUE feature-056)
  uint32_t nowMs;      // millis() courant
  bool stable;         // purgeStabilityEvaluate().stable (fenêtre pleine et calme)
  bool unstable;       // purgeStabilityEvaluate().unstable (étendue > 2× seuil)
  bool purgeWaived;    // dispense présentée pour un front montant à CE tick (C3)
  uint32_t floorMs;    // kPurgeFloorMs — plancher de purge
  uint32_t capMs;      // kPurgeCapMs — plafond fail-safe
};

// État persistant entre deux ticks — possédé par la coquille. Zéro-initialisé =
// « boot, eau jamais vue ».
struct MeasureValidityState {
  bool waterPresent;          // dernier état d'eau évalué (détection de fronts)
  bool valid;                 // porte OUVERTE = mesures représentatives
  bool purging;               // purge en cours (eau présente, porte fermée)
  bool recloseArmed;          // surveillance de re-fermeture active (C2 : false après plafond)
  bool waivedCycle;           // le cycle d'eau courant a été ouvert par dispense
  uint8_t repurgeCount;       // re-purges du cycle d'eau courant (C1)
  uint32_t waterRiseMs;       // millis() du dernier front montant
  uint32_t purgeStartMs;      // millis() du début de la purge courante (front ou re-fermeture)
  uint32_t lastPurgeDurationS;// durée de la dernière purge terminée (s)
  PurgeEnd lastPurgeEnd;      // motif de fin de la dernière purge terminée
  MeasureValidityEvent event; // événement du DERNIER tick (journal)
};

// Un tick de la machine d'état (voir règles en tête de fichier).
void measureValidityTick(MeasureValidityState& st, const MeasureValidityInputs& in);

// Secondes écoulées dans la purge courante (0 si pas de purge en cours).
uint32_t measureValidityElapsedS(const MeasureValidityState& st, uint32_t nowMs);

// C4 pool-chemistry — « circulation établie » : l'eau est présente depuis au
// moins floorMs (ou le cycle a été dispensé : la conduite n'a pas été vidée).
// Condition d'AUTORISATION de la branche de dosage PROGRAMMÉ (aveugle), qui ne
// dépend pas des mesures mais doit attendre que l'eau de la conduite ait été
// renouvelée pour que le produit injecté soit emporté au bassin.
bool measureValidityCirculationSettled(const MeasureValidityState& st, uint32_t nowMs,
                                       uint32_t floorMs);

// Libellé court du motif de fin (journal) : "stabilité", "plafond", "dispensée",
// "re-purge", "aucune".
const char* purgeEndLabel(PurgeEnd e);

// =============================================================================
// C3 — CONTINUITÉ À TRAVERS UNE COUPURE COURTE (dispense de purge)
// =============================================================================
// La coquille maintient en RAM RTC (RTC_NOINIT_ATTR : survit à un reset
// logiciel, panic ou watchdog — PAS à une coupure secteur) un enregistrement
// {heure vive, eau présente, dernière dispense}, rafraîchi ~1 fois par seconde
// et scellé par un FNV-1a. Au boot, la décision PURE ci-dessous dit si la purge
// du premier front montant peut être dispensée. FAIL-CLOSED : le moindre doute
// (mémoire absente ou corrompue, motif de reset matériel, heure invalide,
// coupure trop longue, dispense trop récente) ⇒ purge complète.

// Motif de reset abstrait (la coquille traduit esp_reset_reason()).
enum class OutageResetKind : uint8_t {
  kOther = 0,   // power-on, brownout, externe, inconnu… ⇒ jamais dispensé
  kSoftware,    // ESP_RST_SW (OTA, redémarrage demandé)
  kPanic,       // ESP_RST_PANIC
  kIntWdt,      // ESP_RST_INT_WDT
  kTaskWdt,     // ESP_RST_TASK_WDT
};

constexpr uint32_t kOutageRtcMagic = 0x50555247UL;  // "PURG"

// Enregistrement RTC (20 octets, tous champs explicites : pas de padding
// implicite dans le CRC).
struct OutageRtcRecord {
  uint32_t magic;            // kOutageRtcMagic
  uint32_t liveEpoch;        // dernière heure murale vue (s, ≥ kMinValidEpoch)
  uint32_t lastWaiverEpoch;  // heure de la dernière dispense accordée ; 0 = aucune
  uint8_t waterPresent;      // 1 = eau présente au dernier rafraîchissement
  uint8_t reserved[3];       // 0
  uint32_t crc;              // FNV-1a des octets précédents
};
static_assert(sizeof(OutageRtcRecord) == 20, "OutageRtcRecord : padding inattendu");

// FNV-1a 32 bits sur `len` octets (même algorithme que ph_persist_logic).
uint32_t outageFnv1a(const void* data, uint32_t len);
// CRC de l'enregistrement (tous les champs avant `crc`).
uint32_t outageRecordCrc(const OutageRtcRecord& r);
// Scelle : pose le magic et le CRC.
void outageRecordSeal(OutageRtcRecord& r);
// Magic ET CRC valides.
bool outageRecordIntact(const OutageRtcRecord& r);

// Raison du REFUS de dispense (journal).
enum class OutageDeny : uint8_t {
  kNone = 0,       // dispense accordée
  kMemory,         // mémoire RTC absente/corrompue (power-on, 1er boot)
  kNoWater,        // l'eau n'était pas présente avant la coupure
  kResetKind,      // motif de reset matériel (coupure secteur possible)
  kClock,          // heure invalide au boot, ou antérieure à l'heure vive
  kTooLong,        // coupure > shortOutageS
  kRateLimited,    // une dispense a déjà été accordée il y a < waiverMinIntervalS
};

struct OutageContinuityInputs {
  const OutageRtcRecord* record;  // enregistrement RTC (nullptr = absent)
  OutageResetKind reset;          // motif de reset du boot courant
  bool timeValid;                 // heure système valide au moment de la décision
  uint32_t epochNow;              // heure courante (s)
  uint32_t shortOutageS;          // kShortOutageS
  uint32_t waiverMinIntervalS;    // kOutageWaiverMinIntervalS
};

struct OutageContinuityVerdict {
  bool waive;         // true = dispenser la purge du premier front montant
  uint32_t outageS;   // durée de coupure calculée (0 si incalculable)
  OutageDeny deny;    // raison du refus (kNone si accordée)
};

// Décision pure (voir règles en tête de section).
OutageContinuityVerdict evaluateOutageContinuity(const OutageContinuityInputs& in);

// Libellé du refus (journal), ex. "mémoire RTC absente".
const char* outageDenyLabel(OutageDeny d);

#endif  // MEASURE_VALIDITY_LOGIC_H
