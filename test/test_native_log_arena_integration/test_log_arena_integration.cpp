// =============================================================================
// Tests d'intégration natifs — arène circulaire du journal (feature-129, phase D)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / LittleFS.
//
// OBJET : `src/logger.cpp` (Logger::log(), phase C) compose TROIS fonctions
// pures (logArenaPlan, logRingEvictCount, logTruncatePlan) et une comptabilité
// manuelle (_arenaHead/_arenaTail/_arenaUsed, compteurs C6) qu'embedded-
// developer a validée « par simulation manuelle, consignée en commentaire »,
// sans test natif de l'INTÉGRATION multi-appels. `logger.cpp` lui-même n'est
// PAS compilable en natif (Arduino.h, FreeRTOS, FS.h) — voir platformio.ini
// (`-include test/native_shim/logger_shim.h` neutralise `logger.h`).
//
// Ce fichier introduit `ArenaHarness<Cap, NSlots>`, une COPIE FIDÈLE de la
// séquence d'opérations de `Logger::log()` (src/logger.cpp:91-169) et de
// `Logger::_flushOnePass()` (src/logger.cpp:591-616), réduite au strict
// nécessaire (pas de mutex — natif mono-thread —, pas de wallSec, pas de
// callback WS, pas d'I/O disque) et construite UNIQUEMENT à partir des
// fonctions pures déjà couvertes en phase B (logArenaPlan, logRingEvictCount,
// logTruncatePlan, logShouldPersist, logNextFlushSlot).
//
// ⚠️ PORTÉE HONNÊTE (à lire avant de compter un mutant comme tué) : ce harnais
// teste la FORMULE que logger.cpp implémente, PAS le fichier logger.cpp
// lui-même (impossible à compiler en natif). Toute divergence future entre
// `ArenaHarness` et `Logger::log()`/`_flushOnePass()` réintroduirait le trou
// SANS que cette suite le détecte — c'est un point de revue explicite pour
// `code-reviewer` (comparer les deux blocs ligne à ligne à chaque modification
// de logger.cpp), et le risque résiduel est de la même famille que celui déjà
// documenté dans la spec pour `LogArenaView`/ArduinoJson (non détectable en
// natif, jamais compté comme "tué" au sens strict d'ADR-0073).
//
// =============================================================================
// ✅ POST-MORTEM — DÉFAUT TROUVÉ LE 16/09/2026, CORRIGÉ LE MÊME JOUR.
//    CETTE SUITE EST VERTE. Un rouge ici signale une RÉGRESSION RÉELLE.
// =============================================================================
// ⚠️ Ce bandeau a d'abord été écrit « SUITE ACTUELLEMENT ROUGE, VOLONTAIREMENT
// — ACTION REQUISE ». Il a été réécrit sur demande de `pool-chemistry` (P2) :
// laisser un test rouge décrit comme attendu, sur le chemin qui porte les
// gardes de dosage, aurait fait lire la prochaine régression comme normale.
// Un test rouge n'est jamais un état de repos.
//
// CE QUI S'EST PASSÉ
// `test_integration_sequence_longue_multiples_tours_vs_modele_deque` échouait
// de façon déterministe (graine 0xF129D) à l'itération 508 : le message
// seq=429, écrit au premier tour de l'arène à l'offset 0 et JAMAIS évincé,
// était SILENCIEUSEMENT ÉCRASÉ par seq=508 — deux créneaux vivants qui se
// recouvrent, la violation de l'invariant le plus fondamental d'un anneau.
//
// LA RACINE, dans `logArenaPlan()` (log_arena_logic.cpp), branche `wrapped` :
//
//     room = (tail + cap - plan.writeOff) % cap;   // writeOff == 0 si wrapped
//
// mesurait la place libre depuis la DESTINATION au lieu de `head`, la position
// AVANT la décision de saut. Tant que `tail` n'avait pas avancé depuis un saut
// précédent — régime parfaitement réaliste dès que les messages avoisinent la
// moyenne visée par kLogArenaBytes/kMaxLogEntries — `room` était réévalué comme
// « presque toute la capacité » alors que l'espace réellement libre,
// `(tail + cap - head) % cap`, pouvait être minuscule. `evictBytes` sortait à 0
// et l'écriture partait droit sur un message vivant.
//
// Preuve numérique conservée (trace réelle, cap=16384) :
//   iter=429  seq=429 wrapped=1 writeOff=0 tailDrop=40 → écrit à [0,158)
//   iter=507  (fin)   head=16255 tail=16310 used=16289  (seq429 TOUJOURS vivant)
//   iter=508  seq=508 wrapped=1 writeOff=0 tailDrop=129 evictCount=0
//             room calculée = 16310 (FAUSSE) · room réelle = 55
//             → AUCUNE éviction, écriture directe sur seq429 : CORRUPTION.
//             `used` grimpait à 16425 > kLogArenaBytes(16384) — second symptôme.
//
// PORTÉE QU'AVAIT LE DÉFAUT : n'importe quel message de garde chimique (jusqu'à
// ~630 o, cf. C1) affiché ou en cours de lecture (WS, /get-logs,
// /download-logs) pouvait voir son contenu remplacé par du texte SANS RAPPORT,
// silencieusement. Et les cinq compteurs de l'arène restaient cohérents : rien
// dans l'instrumentation du lot ne l'aurait montré.
//
// POURQUOI AUCUN TEST UNITAIRE NE POUVAIT LE TROUVER : le défaut n'apparaît
// qu'après un état accumulé — deux tours d'arène dont le premier n'a pas encore
// été rattrapé par les évictions. Pire, le test unitaire `test_plan_saut_de_
// queue_compte_dans_evictbytes` (phase B) attendait une valeur DÉRIVÉE de la
// formule fautive : il confirmait le défaut au lieu de le révéler, et restait
// vert. Seule une comparaison à un modèle de référence INDÉPENDANT pouvait
// trancher. C'est la raison d'être de cette suite.
//
// NE PAS affaiblir ni retirer le test ci-dessous, et ne jamais dériver une
// valeur attendue de ce que le code calcule.
// =============================================================================
// `test_integration_sequence_longue_multiples_tours_vs_modele_deque` ci-dessous
// échoue de façon déterministe (graine fixe 0xF129D) à l'itération 508 : le
// message seq=429 (écrit au premier "tour" de l'arène, à l'offset 0) est
// SILENCIEUSEMENT ÉCRASÉ par le message seq=508 alors qu'il est TOUJOURS VIVANT
// (jamais évincé) — violation de l'invariant le plus fondamental d'un anneau
// circulaire : deux créneaux vivants ne doivent JAMAIS se recouvrir.
//
// CE N'EST PAS UN BUG DE CE TEST — c'est `logArenaPlan()` elle-même
// (log_arena_logic.cpp) qui est en cause. Racine : dans la branche `wrapped`,
// le calcul de `room` utilise `plan.writeOff` (toujours 0 après un wrap) comme
// point de départ :
//
//     room = (tail + cap - plan.writeOff) % cap;   // writeOff == 0 si wrapped
//
// au lieu de `head` (la position AVANT la décision de wrap). Tant que `tail`
// n'a pas avancé depuis un wrap PRÉCÉDENT (aucune éviction déclenchée par
// pression d'octets — régime réaliste si les messages sont proches de la
// moyenne visée par kLogArenaBytes/kMaxLogEntries), `room` est réévalué comme
// « (presque) toute la capacité », alors que l'espace RÉELLEMENT libre est
// `(tail + cap - head) % cap`, potentiellement minuscule. `evictBytes` en
// sortie peut donc valoir 0 alors qu'un message VIVANT occupe déjà l'offset 0
// où l'on s'apprête à écrire.
//
// Preuve numérique (issue de la trace, `used`/offsets réels, cap=16384) :
//   iter=429  seq=429 wrapped=1 writeOff=0 tailDrop=40 → écrit à [0,158)
//   iter=507  (fin)   head=16255 tail=16310 used=16289  (seq429 TOUJOURS vivant)
//   iter=508  seq=508 wrapped=1 writeOff=0 tailDrop=129 evictCount=0
//             room calculée = tail(16310) = 16310 (FAUSSE)
//             room réelle   = (tail+cap-head)%cap = 55
//             → AUCUNE éviction, écriture directe sur seq429 : CORRUPTION.
//             `used` grimpe à 16425 > kLogArenaBytes(16384) — second symptôme,
//             la somme d'octets vivants dépasse la capacité physique.
//
// Portée réelle : n'importe quel message de garde chimique (jusqu'à ~630 o,
// cf. C1) encore affiché ou en cours de lecture (WS temps réel, /get-logs,
// /download-logs) peut voir son contenu remplacé par du texte SANS RAPPORT,
// silencieusement, dès qu'un second tour d'arène croise un premier tour dont
// la queue n'a pas encore été rattrapée par les évictions.
//
// ACTION REQUISE (hors périmètre test-engineer) : renvoyer à `architect` +
// `embedded-developer`, validation obligatoire par `pool-chemistry` (le
// journal porte les gardes de dosage). NE PAS affaiblir ni retirer le test
// ci-dessous pour faire repasser la suite au vert — c'est le test qui a raison.
// =============================================================================

#include <unity.h>
#include <string.h>

#include <deque>
#include <random>
#include <string>
#include <vector>

#include "constants.h"       // kLogArenaBytes, kMaxLogEntries, kLogMessageMaxBytes, kLogTruncTail
#include "log_arena_logic.h"
#include "utf8_clamp.h"

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// ArenaHarness — mirroir de Logger::log() / Logger::_flushOnePass()
// =============================================================================
template <size_t Cap, size_t NSlots>
struct ArenaHarness {
  char arena[Cap] = {};
  LogSlot slots[NSlots] = {};
  uint16_t head = 0;
  uint16_t tail = 0;
  uint16_t used = 0;
  size_t oldestIdx = 0;
  size_t writeIdx = 0;
  size_t liveCount = 0;
  uint32_t nextSeq = 0;
  uint32_t flushCursor = 0;
  uint32_t persistEnableSeq = 0;

  // Compteurs (mirroir des atomiques C5/C6 de logger.cpp)
  uint32_t truncations = 0;
  uint32_t evictions = 0;
  uint32_t overruns = 0;  // C6 : logPersistOverruns
  // P2 pool-chemistry (feature-129) : mirroir de sLogArenaOverlapGuardTrips.
  // DOIT rester à 0 dans toute la suite ci-dessous -- logArenaPlan() est
  // désormais correcte (post-mortem 16/09/2026), donc ce filet indépendant ne
  // doit JAMAIS avoir à intervenir en fonctionnement normal. Une valeur non
  // nulle ici signalerait une régression de logArenaPlan()/logRingEvictCount().
  uint32_t overlapGuardTrips = 0;

  struct EvictedVictim {
    uint32_t seq;
    LogLevel level;
    uint16_t len;
  };

  struct WriteResult {
    uint32_t seq = 0;
    bool truncated = false;
    uint8_t flags = 0;
    size_t evictCount = 0;
    std::vector<EvictedVictim> evictedSlots;
  };

  // Mirroir EXACT de src/logger.cpp:91-169 (Logger::log()), moins le mutex,
  // wallSec, le callback WS et la sortie console — aucun de ces éléments ne
  // touche à la comptabilité d'arène que ce harnais vérifie.
  WriteResult write(const char* src, size_t srcLen, LogLevel level) {
    WriteResult r;

    bool truncated = false;
    const size_t keep = logTruncatePlan(src, srcLen, kLogMessageMaxBytes, truncated);
    const size_t tailLen = truncated ? strlen(kLogTruncTail) : 0;
    const size_t finalLen = keep + tailLen;
    if (truncated) truncations++;

    const LogArenaPlan plan =
        logArenaPlan(static_cast<uint16_t>(Cap), head, tail, used, static_cast<uint16_t>(finalLen));

    size_t evictCount = 0;
    if (plan.evictBytes > 0) {
      evictCount = logRingEvictCount(slots, NSlots, oldestIdx, plan.evictBytes);
      if (evictCount > liveCount) evictCount = liveCount;
    }
    if (liveCount - evictCount >= NSlots) {
      evictCount = liveCount - NSlots + 1;
    }
    for (size_t i = 0; i < evictCount; ++i) {
      const LogSlot& victim = slots[oldestIdx];
      r.evictedSlots.push_back({victim.seq, static_cast<LogLevel>(victim.level), victim.len});
      if (victim.seq >= flushCursor &&
          logShouldPersist(victim.seq, persistEnableSeq, static_cast<LogLevel>(victim.level))) {
        overruns++;
      }
      used = static_cast<uint16_t>(used - victim.len);
      oldestIdx = (oldestIdx + 1) % NSlots;
      --liveCount;
    }
    if (evictCount > 0) evictions += evictCount;
    r.evictCount = evictCount;

    // P2 pool-chemistry (feature-129) : mirroir EXACT du filet indépendant de
    // src/logger.cpp (logArenaWriteOverlapsLive, AVANT le memcpy). Voir le
    // commentaire de tête de ce fichier -- toute divergence future entre ce
    // mirroir et logger.cpp rouvrirait le trou sans témoin.
    uint16_t writeOff = plan.writeOff;
    if (logArenaWriteOverlapsLive(slots, NSlots, oldestIdx, liveCount, writeOff,
                                  static_cast<uint16_t>(finalLen))) {
      overlapGuardTrips++;
      oldestIdx = writeIdx;
      liveCount = 0;
      used = 0;
      writeOff = 0;
    }

    memcpy(&arena[writeOff], src, keep);
    if (truncated) memcpy(&arena[writeOff + keep], kLogTruncTail, tailLen);

    if (liveCount == 0) oldestIdx = writeIdx;
    LogSlot& slot = slots[writeIdx];
    slot.timestamp = 0;
    slot.wallSec = 0;
    slot.seq = nextSeq++;
    slot.off = writeOff;
    slot.len = static_cast<uint16_t>(finalLen);
    slot.level = static_cast<uint8_t>(level);
    slot.flags = truncated ? kLogFlagTruncated : 0;
    writeIdx = (writeIdx + 1) % NSlots;
    ++liveCount;

    used = static_cast<uint16_t>(used + finalLen);
    head = static_cast<uint16_t>(writeOff + finalLen);
    tail = slots[oldestIdx].off;

    r.seq = slot.seq;
    r.truncated = truncated;
    r.flags = slot.flags;
    return r;
  }

  struct FlushStep {
    uint32_t seq;
    LogLevel level;
    bool written;
  };

  // Mirroir de la boucle interne de src/logger.cpp:591-616 (Logger::_flushOnePass),
  // réduit à l'avancement du curseur — aucune écriture disque (hors de portée
  // de cette suite, qui ne vérifie QUE l'arène).
  std::vector<FlushStep> flushPass(size_t maxSteps) {
    std::vector<FlushStep> out;
    for (size_t i = 0; i < maxSteps; ++i) {
      const size_t idx = logNextFlushSlot(slots, NSlots, oldestIdx, liveCount, flushCursor);
      if (idx == NSlots) break;
      const LogSlot& slot = slots[idx];
      const bool doWrite =
          logShouldPersist(slot.seq, persistEnableSeq, static_cast<LogLevel>(slot.level));
      out.push_back({slot.seq, static_cast<LogLevel>(slot.level), doWrite});
      flushCursor = slot.seq + 1;  // logger.cpp avance TOUJOURS, filtrée ou pas
    }
    return out;
  }
};

// =============================================================================
// Trou n°2 — scénario PETIT et VÉRIFIABLE À LA MAIN : éviction qui dépasse le
// curseur de flush, mutant #13 (logPersistOverruns).
// =============================================================================
// Arène minuscule (50 o, 5 créneaux) pour un calcul reproductible ligne à
// ligne dans ce commentaire :
//   1) write("AAAAAAAAAA", INFO) -> seq0, len=10, used=10, head=10
//   2) write("BBBBBBBBBB", INFO) -> seq1, len=10, used=20, head=20
//   3) write("CCCCCCCCCC", INFO) -> seq2, len=10, used=30, head=30
//   4) flushPass(1) -> consomme seq0 (flushCursor=1) : seq0 est désormais
//      FLUSHÉ, seq1/seq2 ne le sont PAS encore.
//   5) write(25 o, INFO) -> len=25. contiguousFreeAtEnd = 50-30 = 20 < 25 =>
//      WRAP, writeOff=0, tailDrop=20. room se mesure depuis HEAD, jamais
//      depuis writeOff : (tail(0)+cap(50)-head(30))%50 = 20.
//      need = 25+20 = 45, evictBytes = 45-20 = 25.
//      logRingEvictCount(oldest=0, bytesNeeded=45) parcourt slots[0..4] (n=5) :
//      10+10+10+0+0 = 30 < 45 tout du long, boucle épuisée -> count=5, clampé
//      à liveCount=3 : les TROIS créneaux (A,B,C) sont évincés.
//   Overrun attendu : seq0 (déjà flushé, seq0 < flushCursor(1)) NE compte PAS ;
//   seq1 et seq2 (seq >= 1, INFO donc logShouldPersist vrai) COMPTENT : 2.
void test_integration_eviction_devant_le_curseur_overrun_egale_deux(void) {
  ArenaHarness<50, 5> h;

  h.write("AAAAAAAAAA", 10, LogLevel::INFO);
  h.write("BBBBBBBBBB", 10, LogLevel::INFO);
  h.write("CCCCCCCCCC", 10, LogLevel::INFO);

  auto flushed = h.flushPass(1);
  TEST_ASSERT_EQUAL_size_t(1, flushed.size());
  TEST_ASSERT_EQUAL_UINT32(0, flushed[0].seq);
  TEST_ASSERT_EQUAL_UINT32(1, h.flushCursor);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, h.overruns, "rien n'a encore été évincé");

  std::string big(25, 'D');
  auto res = h.write(big.data(), big.size(), LogLevel::INFO);

  TEST_ASSERT_EQUAL_size_t_MESSAGE(3, res.evictCount, "A, B et C doivent tous être évincés (calcul en tête de test)");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      2, h.overruns,
      "mutant #13 : seq0 (déjà flushé) ne doit PAS compter, seq1/seq2 "
      "(non flushés, INFO) DOIVENT compter -- overrun attendu = 2, pas 3 "
      "(compte tout) ni 0 (ne compte rien)");
}

// =============================================================================
// Trou n°2 — mutant #14 : kLogFlagTruncated non posé
// =============================================================================
void test_integration_message_trop_long_leve_le_drapeau_truncated(void) {
  ArenaHarness<kLogArenaBytes, kMaxLogEntries> h;
  std::string longMsg(kLogMessageMaxBytes + 200, 'x');
  auto res = h.write(longMsg.data(), longMsg.size(), LogLevel::WARNING);
  TEST_ASSERT_TRUE(res.truncated);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      kLogFlagTruncated, res.flags,
      "mutant #14 : le drapeau kLogFlagTruncated DOIT être posé sur le "
      "LogSlot quand le message a été coupé -- sinon /system.log et l'API "
      "présentent un refus d'injection tronqué comme complet");
}

void test_integration_message_court_ne_leve_pas_truncated(void) {
  ArenaHarness<kLogArenaBytes, kMaxLogEntries> h;
  auto res = h.write("court", 5, LogLevel::INFO);
  TEST_ASSERT_FALSE(res.truncated);
  TEST_ASSERT_EQUAL_UINT8(0, res.flags);
}

// =============================================================================
// Trou n°2 — mutant #15 : logShouldPersist appelée avec seq/persistEnableSeq
// permutés au site de décision d'écriture du flush (logger.cpp:595).
// =============================================================================
// persistEnableSeq=0 : la "règle normale" (level != DEBUG) s'applique dès la
// première entrée (seq=0 n'est jamais < persistEnableSeq=0). Un site muté
// (arguments permutés) évaluerait `persistEnableSeq(0) < seq_reel` -- VRAI dès
// que seq_reel > 0 -- et retomberait à tort sur la règle de rattrapage
// (WARNING+ seulement), rejetant les entrées INFO qui doivent être acceptées.
void test_integration_flush_ecrit_info_quand_persistenableseq_zero(void) {
  ArenaHarness<kLogArenaBytes, kMaxLogEntries> h;
  h.write("premier message", 15, LogLevel::INFO);
  h.write("deuxieme message", 16, LogLevel::DEBUG);
  h.write("troisieme message", 17, LogLevel::INFO);

  auto steps = h.flushPass(10);
  TEST_ASSERT_EQUAL_size_t(3, steps.size());
  TEST_ASSERT_TRUE_MESSAGE(
      steps[0].written,
      "mutant #15 (permutation seq/persistEnableSeq) : seq0 (INFO) doit être "
      "écrit -- persistEnableSeq=0 place tout de suite sur la règle normale");
  TEST_ASSERT_FALSE(steps[1].written);  // DEBUG : toujours refusé, indépendant du mutant
  TEST_ASSERT_TRUE_MESSAGE(
      steps[2].written,
      "mutant #15 : seq2 (INFO) doit être écrit -- un argument permuté le "
      "rejetterait (retomberait sur le filtre de rattrapage WARNING+)");
}

// =============================================================================
// P2 pool-chemistry (feature-129, condition bloquante de clôture) — le filet
// indépendant sauve réellement l'intégrité quand l'état amont est corrompu.
// =============================================================================
// logArenaPlan() est correcte depuis le 16/09/2026 : ce test ne cherche PAS à
// la reprendre en défaut. Il vérifie que le filet POSÉ APRÈS elle (P2) rattrape
// une incohérence d'ORIGINE DIFFÉRENTE -- ici une dérive de `used` simulée
// directement sur l'état du harnais (le bookkeeping manuel de `_arenaUsed`
// pourrait un jour diverger ailleurs que dans logArenaPlan, ex. un futur
// correctif touchant l'éviction). Avec `used` corrompu à 0 alors qu'un
// créneau [0,10) est TOUJOURS déclaré vivant (liveCount=1), logArenaPlan()
// calcule `room = cap` (branche `used == 0`) et ne demande AUCUNE éviction :
// sans le filet, le memcpy suivant recouvrirait ce créneau en silence.
void test_integration_filet_sauve_l_integrite_sur_etat_corrompu(void) {
  ArenaHarness<50, 5> h;

  // Créneau [0,10) déclaré vivant "à la main" (simule une dérive de used
  // sans passer par write(), pour isoler la cause du recouvrement).
  h.slots[0].off = 0;
  h.slots[0].len = 10;
  h.slots[0].seq = 0;
  h.slots[0].level = static_cast<uint8_t>(LogLevel::CRITICAL);
  h.liveCount = 1;
  h.oldestIdx = 0;
  h.writeIdx = 0;
  h.nextSeq = 1;
  h.head = 0;
  h.tail = 0;
  h.used = 0;  // <-- la corruption : devrait valoir 10, pas 0

  TEST_ASSERT_EQUAL_UINT32(0, h.overlapGuardTrips);

  std::string msg(10, 'Y');
  auto res = h.write(msg.data(), msg.size(), LogLevel::INFO);

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      1, h.overlapGuardTrips,
      "used=0 alors qu'un créneau [0,10) est vivant : logArenaPlan() ne "
      "demande aucune éviction (room=cap), SEUL le filet indépendant "
      "empêche le recouvrement -- il doit s'être déclenché exactement une "
      "fois");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(
      1, h.liveCount,
      "repli sûr : tous les créneaux vivants sont évincés puis le nouveau "
      "message est écrit -- un seul créneau vivant doit subsister (le "
      "nouveau), jamais deux qui se recouvrent");
  TEST_ASSERT_EQUAL_UINT32(res.seq, h.slots[h.oldestIdx].seq);
  std::string readBack(&h.arena[h.slots[h.oldestIdx].off], h.slots[h.oldestIdx].len);
  TEST_ASSERT_TRUE_MESSAGE(
      readBack == msg,
      "le contenu relu doit être le NOUVEAU message -- perdre le journal "
      "vaut mieux que le falsifier (ADR-0072), mais ce qui EST écrit doit "
      "être intact, jamais un mélange de l'ancien et du nouveau");
}

// =============================================================================
// Trou n°2 — séquence longue, modèle de référence, tailles réelles
// (kLogArenaBytes=16384, kMaxLogEntries=200)
// =============================================================================
// Graine FIXE (reproductible) — plusieurs milliers d'itérations pour forcer de
// nombreux tours COMPLETS de l'arène ET du tableau de créneaux, des évictions
// en rafale (gros messages), des troncatures, et des passages de flush
// partiels intercalés (le curseur avance PENDANT que l'arène continue de
// tourner, exactement le régime qui produit log_persist_overrun en
// production).
void test_integration_sequence_longue_multiples_tours_vs_modele_deque(void) {
  ArenaHarness<kLogArenaBytes, kMaxLogEntries> h;

  struct RefMsg {
    uint32_t seq;
    std::string text;
  };
  std::deque<RefMsg> model;

  std::mt19937 rng(0xF129Du);  // feature-129, graine fixe : reproductible
  std::uniform_int_distribution<int> pctDist(0, 99);
  std::uniform_int_distribution<int> smallLenDist(1, 300);
  std::uniform_int_distribution<int> bigLenDist(static_cast<int>(kLogMessageMaxBytes) + 1,
                                                 static_cast<int>(kLogMessageMaxBytes) * 2);
  std::uniform_int_distribution<int> levelDist(0, 4);
  std::uniform_int_distribution<int> flushStepsDist(1, 5);
  std::uniform_int_distribution<int> fillCharDist('a', 'z');

  const int kIterations = 15000;  // ~16384/≈150o moyen : plusieurs dizaines de tours d'arène
                                   // et de tableau de créneaux (200) sur cette longueur.

  for (int iter = 0; iter < kIterations; ++iter) {
    const bool big = pctDist(rng) < 8;  // ~8% de messages qui déclenchent la troncature (C2/C4)
    const size_t len = big ? static_cast<size_t>(bigLenDist(rng)) : static_cast<size_t>(smallLenDist(rng));
    std::string content;
    content.reserve(len);
    const char fillChar = static_cast<char>(fillCharDist(rng));
    for (size_t i = 0; i < len; ++i) content.push_back(fillChar);
    const LogLevel level = static_cast<LogLevel>(levelDist(rng));

    const uint32_t overrunsBefore = h.overruns;
    const uint32_t flushCursorBefore = h.flushCursor;
    const uint32_t persistEnableSeqBefore = h.persistEnableSeq;

    auto res = h.write(content.data(), content.size(), level);

    // --- Mutant #13, vérifié à CHAQUE itération (pas seulement le scénario
    //     minimal ci-dessus) : le compte d'overrun mesuré doit égaler le
    //     compte attendu, recalculé INDÉPENDAMMENT à partir des victimes
    //     réellement évincées par CET appel.
    uint32_t expectedDelta = 0;
    for (const auto& victim : res.evictedSlots) {
      if (victim.seq >= flushCursorBefore &&
          logShouldPersist(victim.seq, persistEnableSeqBefore, victim.level)) {
        ++expectedDelta;
      }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(overrunsBefore + expectedDelta, h.overruns,
                                      "mutant #13 : compte d'overrun divergent à cette itération");

    // --- P2 pool-chemistry : le filet indépendant NE DOIT JAMAIS avoir à
    //     intervenir sur cette séquence, puisque logArenaPlan()/
    //     logRingEvictCount() sont correctes depuis le 16/09/2026. S'il se
    //     déclenche ici, c'est qu'une régression de géométrie vient d'être
    //     réintroduite -- exactement le scénario que ce filet doit rattraper
    //     EN PRODUCTION, mais qui doit rester silencieux dans cette suite.
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, h.overlapGuardTrips,
        "le filet logArenaWriteOverlapsLive s'est déclenché : régression de "
        "géométrie dans logArenaPlan()/logRingEvictCount() (voir le "
        "post-mortem du 16/09/2026 en tête de ce fichier)");

    // --- Mutant #14, vérifié à chaque itération.
    bool truncatedExpected = false;
    const size_t keep = logTruncatePlan(content.data(), content.size(), kLogMessageMaxBytes, truncatedExpected);
    TEST_ASSERT_EQUAL_MESSAGE(truncatedExpected, res.truncated, "drapeau truncated divergent de logTruncatePlan");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(truncatedExpected ? kLogFlagTruncated : 0, res.flags,
                                      "mutant #14 : LogSlot::flags divergent de la troncature réelle");

    // --- Modèle de référence : retirer les évincés (FIFO, front), pousser le nouveau.
    for (size_t i = 0; i < res.evictCount; ++i) {
      TEST_ASSERT_FALSE_MESSAGE(model.empty(), "éviction sans entrée correspondante dans le modèle (désync)");
      model.pop_front();
    }
    std::string stored(content.data(), keep);
    if (truncatedExpected) stored += kLogTruncTail;
    model.push_back({res.seq, stored});

    // --- Nombre d'entrées vivantes jamais au-delà de kMaxLogEntries.
    TEST_ASSERT_TRUE_MESSAGE(h.liveCount <= kMaxLogEntries, "kMaxLogEntries dépassé");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(model.size(), h.liveCount, "nombre d'entrées vivantes divergent du modèle");

    // --- _arenaUsed == somme EXACTE des longueurs vivantes, recalculée en
    //     parcourant les créneaux indépendamment du compteur incrémental.
    uint32_t sumLen = 0;
    for (size_t i = 0; i < h.liveCount; ++i) {
      const size_t idx = (h.oldestIdx + i) % kMaxLogEntries;
      sumLen += h.slots[idx].len;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sumLen, h.used, "_arenaUsed diverge de la somme réelle des octets vivants");

    // --- C4 : aucun message jamais scindé par la frontière de fin d'arène.
    for (size_t i = 0; i < h.liveCount; ++i) {
      const size_t idx = (h.oldestIdx + i) % kMaxLogEntries;
      const uint32_t end = static_cast<uint32_t>(h.slots[idx].off) + h.slots[idx].len;
      TEST_ASSERT_TRUE_MESSAGE(end <= kLogArenaBytes, "message scindé par la frontière de fin d'arène (C4)");
    }

    // --- _arenaTail == offset du plus ancien créneau vivant.
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(h.slots[h.oldestIdx].off, h.tail, "_arenaTail diverge du plus ancien créneau vivant");

    // --- Contenu relu == modèle, dans l'ordre chronologique : aucun octet
    //     déchiré, aucun décalage, aucune ligne dupliquée ni sautée.
    for (size_t i = 0; i < h.liveCount; ++i) {
      const size_t idx = (h.oldestIdx + i) % kMaxLogEntries;
      const LogSlot& slot = h.slots[idx];
      std::string readBack(&h.arena[slot.off], slot.len);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(model[i].seq, slot.seq, "seq relu diverge du modèle (ligne dupliquée/sautée)");
      TEST_ASSERT_TRUE_MESSAGE(readBack == model[i].text, "contenu relu différent du modèle (octet déchiré ou décalage)");
    }

    // --- Passage de flush partiel intercalé, ~1 itération sur 5 : exerce le
    //     mutant #15 et le régime "éviction devant le curseur" à grande échelle.
    if (iter % 5 == 0) {
      const uint32_t persistEnableSeqAtFlush = h.persistEnableSeq;
      auto steps = h.flushPass(static_cast<size_t>(flushStepsDist(rng)));
      for (const auto& step : steps) {
        const bool expectedWritten = logShouldPersist(step.seq, persistEnableSeqAtFlush, step.level);
        TEST_ASSERT_EQUAL_MESSAGE(expectedWritten, step.written,
                                    "mutant #15 : décision d'écriture du flush divergente de logShouldPersist");
      }
    }
  }
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_integration_eviction_devant_le_curseur_overrun_egale_deux);
  RUN_TEST(test_integration_message_trop_long_leve_le_drapeau_truncated);
  RUN_TEST(test_integration_message_court_ne_leve_pas_truncated);
  RUN_TEST(test_integration_flush_ecrit_info_quand_persistenableseq_zero);
  RUN_TEST(test_integration_filet_sauve_l_integrite_sur_etat_corrompu);
  RUN_TEST(test_integration_sequence_longue_multiples_tours_vs_modele_deque);

  return UNITY_END();
}
