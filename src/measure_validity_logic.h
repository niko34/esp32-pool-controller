#ifndef MEASURE_VALIDITY_LOGIC_H
#define MEASURE_VALIDITY_LOGIC_H

// =============================================================================
// measure_validity_logic — Validité des mesures pH/ORP PURE
// (feature-061, réécrite par la feature-091 v2.34.0 : purge ADAPTATIVE ;
// RETRAIT de l'analyse de stabilité par la feature-139, v2.62.0)
// =============================================================================
// Décision « les mesures pH/ORP sont-elles représentatives de l'eau du bassin ? »
// extraite en module pur pour être testable en natif (sans ESP32, sans FreeRTOS,
// sans Arduino). Style ADR-0017, comme dosing_logic / ph_persist_logic.
//
// Les sondes sont montées dans le circuit de filtration : filtration arrêtée =
// eau stagnante autour des sondes → la valeur dérive localement et ne reflète
// plus le bassin. À la reprise de la circulation, une PURGE est appliquée
// (l'eau stagnante de la chambre de mesure doit être chassée) avant de
// redéclarer les mesures valides.
//
// RETRAIT feature-139 (gate pool-chemistry du 2026-10-08) : l'ancienne purge
// ADAPTATIVE attendait en plus que les bruts pH/ORP soient STABLES
// (purge_stability_logic, désormais supprimé), avec re-fermeture (C1) et
// plafond fail-safe (C2). Ce mécanisme est un détecteur d'ÉTENDUE —
// structurellement aveugle à une mesure DURABLEMENT fausse (qui ne bouge pas),
// et ses seuils n'avaient jamais été calés que sur un seul bassin, une seule
// fois. Il ne reste qu'un PLANCHER fixe (kPurgeFloorMs) : de la physique de
// renouvellement d'eau en conduite, valable partout.
//
// MACHINE D'ÉTAT (une transition par tick, priorité décroissante) :
//   1. eau absente            → porte FERMÉE, pas de purge. Datation de l'arrêt
//                               (C2, feature-139) : au front descendant,
//                               waterStopMs = ce tick ; à chaque tick suivant,
//                               si l'arrêt ATTEINT OU dépasse graceMs, LATCH
//                               stopGraceExceeded (monotone — ne redescend
//                               jamais tant que l'eau reste absente).
//                               FRONTIÈRE (`>=`, pas `>`) : à durée == graceMs
//                               PILE, c'est déjà un arrêt LONG — sens prudent
//                               (fail-closed), et DOIT rester aligné avec
//                               evaluateOutageContinuity() (C3 plus bas), qui
//                               compare aussi avec `>=` sur son graceS en
//                               secondes ;
//   2. front montant d'eau    → PURGE démarre (t0 = ce tick). Si l'arrêt qui
//                               vient de se terminer était datable ET COURT
//                               → porte OUVERTE immédiatement, motif kWaived,
//                               waivedCycle = true, durée 0. « COURT » teste
//                               LE LATCH *ET* REMESURE (haveWaterStop &&
//                               !stopGraceExceeded && (nowMs − waterStopMs) <
//                               graceMs) : le latch seul ne voit passer aucun
//                               tick « eau absente » entre begin() (qui le
//                               fige au boot) et ce tout premier update() —
//                               le setup (Wi-Fi/NTP/FS) peut y ajouter des
//                               dizaines de secondes. La remesure s'AJOUTE au
//                               latch, elle ne le remplace pas (BLOQUANT 2,
//                               code-reviewer 2e passage) ;
//   3. purge en cours         → OUVERTURE si écoulé ≥ floorMs, motif kStability.
//                               Plus de condition d'étendue, plus de
//                               re-fermeture, plus de plafond (feature-139).
//
// UN SEUL CHEMIN DE DISPENSE (arbitrage pool-chemistry, feature-139) : que
// l'arrêt d'eau ait traversé un redémarrage ou non, c'est TOUJOURS la même
// comparaison « durée d'absence < graceMs » qui décide. La mémoire RTC de la
// coquille (section C3 plus bas) ne fait que PORTER waterStopMs à travers un
// reboot — ce n'est pas une branche de décision séparée.
//
// Toutes les soustractions de temps sont NON SIGNÉES (wrap de millis() sûr).
//
// INVARIANT : ce module ne fait QUE décider à partir de valeurs déjà collectées.
// La collecte (resolveWaterPresence(), millis(), mémoire RTC de coupure) et les
// logs restent dans la coquille impure measure_validity.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. On
// utilise l'en-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// Motif de fin de la DERNIÈRE purge terminée — valeurs figées : exposées telles
// quelles dans /data et le WebSocket (`measures_purge_last_end`).
// ⚠️ kCap(2) et kRepurge(4) : RETIRÉS v2.62 (feature-139, plus de plafond ni de
// re-fermeture) — plus JAMAIS émis. Valeurs RÉSERVÉES, ne pas renuméroter : un
// consommateur externe qui testait `=== 2` doit cesser de matcher, pas devenir
// faux-positif sur une autre cause.
enum class PurgeEnd : uint8_t {
  kNone = 0,       // aucune purge terminée depuis le boot
  kStability = 1,  // ouverte par stabilité (purge initiale)
  // kCap = 2,     // retiré v2.62, plus jamais émis — réservé
  kWaived = 3,     // dispensée (arrêt d'eau court, C3)
  // kRepurge = 4, // retiré v2.62, plus jamais émis — réservé
};

// Événement du tick, pour le journal de la coquille (une seule transition/tick).
enum class MeasureValidityEvent : uint8_t {
  kNone = 0,
  kWaterDrop,      // front descendant : porte fermée
  kPurgeStarted,   // front montant : purge démarrée
  kOpened,         // porte ouverte (voir state.lastPurgeEnd pour le motif)
};

// Entrées de la décision — toutes fournies par l'appelant (aucune API Arduino).
struct MeasureValidityInputs {
  bool waterPresent;   // resolveWaterPresence().waterPresent (source UNIQUE feature-056)
  uint32_t nowMs;      // millis() courant
  uint32_t floorMs;    // kPurgeFloorMs — plancher de purge
  uint32_t graceMs;    // kWaterStopGraceS * 1000 — grâce d'arrêt d'eau court (C2, feature-139)
};

// État persistant entre deux ticks — possédé par la coquille. Zéro-initialisé =
// « boot, eau jamais vue » (FAIL-CLOSED : haveWaterStop == false ⇒ un arrêt non
// daté n'est jamais traité comme court).
struct MeasureValidityState {
  bool waterPresent;          // dernier état d'eau évalué (détection de fronts)
  bool valid;                 // porte OUVERTE = mesures représentatives
  bool purging;               // purge en cours (eau présente, porte fermée)
  bool waivedCycle;           // le cycle d'eau courant a été ouvert par dispense
  uint32_t waterRiseMs;       // millis() du dernier front montant
  uint32_t purgeStartMs;      // millis() du début de la purge courante
  uint32_t lastPurgeDurationS;// durée de la dernière purge terminée (s)
  PurgeEnd lastPurgeEnd;      // motif de fin de la dernière purge terminée
  MeasureValidityEvent event; // événement du DERNIER tick (journal)

  // --- Datation de l'arrêt d'eau courant (C2, feature-139) ---
  uint32_t waterStopMs;       // millis() du dernier front descendant daté
  bool haveWaterStop;         // waterStopMs est valide (daté par ce module OU
                              // porté depuis la mémoire RTC par la coquille au boot)
  bool stopGraceExceeded;     // LATCH monotone : l'arrêt courant a dépassé graceMs
                              // (rend le repliement de millis() inoffensif : sans ce
                              // latch, un arrêt > 49,7 j repasserait pour COURT)
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

// Libellé court du motif de fin (journal) : "stabilité", "dispensée", "aucune".
const char* purgeEndLabel(PurgeEnd e);

// =============================================================================
// C3 — CONTINUITÉ À TRAVERS UN REDÉMARRAGE (datation de l'arrêt d'eau portée
// à travers un reboot — feature-139, v2.62.0)
// =============================================================================
// La coquille maintient en RAM RTC (RTC_NOINIT_ATTR : survit à un reset
// logiciel, panic ou watchdog — PAS à une coupure secteur) un enregistrement
// {heure vive, eau présente}, rafraîchi ~1 fois par seconde et scellé par un
// FNV-1a. Au boot, la décision PURE ci-dessous dit si l'arrêt d'eau en cours
// (s'il y en a un) peut être considéré COURT — ce n'est PAS une branche de
// décision séparée : son seul rôle est de permettre à la coquille de
// pré-semer waterStopMs/haveWaterStop du module pur ci-dessus dans le
// référentiel millis() post-boot, pour que la MÊME comparaison « durée <
// graceMs » s'applique, qu'il y ait eu reboot ou non (arbitrage pool-chemistry,
// feature-139). FAIL-CLOSED : le moindre doute (mémoire absente ou corrompue,
// motif de reset matériel, heure invalide, arrêt trop long) ⇒ purge complète.
//
// ⚠️ Le quota « une dispense par heure » (kOutageWaiverMinIntervalS) a été
// RETIRÉ (feature-139, arbitrage pool-chemistry) : avec un critère de durée
// explicite, ce quota ne bloquait plus que le cas légitime (deux arrêts
// d'entretien dans la même heure). Le risque résiduel (détecteur d'eau qui
// bat ⇒ dispenses répétées) est BORNÉ par les plafonds horaire/journalier de
// dosage, pas par une garde ici.

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
  uint8_t waterPresent;      // 1 = eau présente au dernier rafraîchissement
  uint8_t reserved[3];       // 0
  uint32_t crc;              // FNV-1a des octets précédents
};
static_assert(sizeof(OutageRtcRecord) == 16, "OutageRtcRecord : padding inattendu");

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
  kTooLong,        // coupure >= graceS (FRONTIÈRE : égalité stricte = arrêt
                   // LONG, alignée sur le latch de measureValidityTick ci-dessus)
};

struct OutageContinuityInputs {
  const OutageRtcRecord* record;  // enregistrement RTC (nullptr = absent)
  OutageResetKind reset;          // motif de reset du boot courant
  bool timeValid;                 // heure système valide au moment de la décision
  uint32_t epochNow;              // heure courante (s)
  uint32_t graceS;                // kWaterStopGraceS
};

struct OutageContinuityVerdict {
  bool waive;         // true = l'arrêt d'eau en cours au boot peut être traité COURT
  uint32_t outageS;   // durée de coupure calculée (0 si incalculable)
  OutageDeny deny;    // raison du refus (kNone si accordée)
};

// Décision pure (voir règles en tête de section).
OutageContinuityVerdict evaluateOutageContinuity(const OutageContinuityInputs& in);

// Libellé du refus (journal), ex. "mémoire RTC absente".
const char* outageDenyLabel(OutageDeny d);

#endif  // MEASURE_VALIDITY_LOGIC_H
