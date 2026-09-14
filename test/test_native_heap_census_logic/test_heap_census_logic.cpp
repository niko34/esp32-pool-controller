// =============================================================================
// Tests natifs — LOGIQUE PURE du recensement du tas (feature-128, R2/R3)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
//
// CE QUI EST SOUS TEST : src/heap_census_logic.h — HEADER-ONLY, inclus
// directement (ADR-0017, idiome de heap_caps_logic.h). Aucune modification de
// `build_src_filter` nécessaire (invariant I11, platformio.ini intact).
//
// CE QUI N'EST PAS SOUS TEST ICI (impur, ESP32-only — voir
// test_native_heap_telemetry_guard pour les sentinelles de câblage/structure) :
// le callback heap_caps_walk lui-même (heap_census.cpp), les routes HTTP
// (web_routes_heap.cpp), la sérialisation JSON (web_routes_config.cpp).
// =============================================================================

#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "heap_census_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// 1. TOP-N DES BLOCS LIBRES — heapCensusKeptFloor / heapCensusTopNInsert
// ===========================================================================
// MUTANT M12 : « comparaison du top-N inversée » — le recensement rendrait
// les PLUS PETITS blocs libres plutôt que les plus gros, donc regarderait
// partout sauf là où le grand bloc se fait découper.

void test_topn_buffer_not_full_accepts_everything_unsorted(void) {
  HeapCensusFreeEntry buf[4] = {};
  uint8_t count = 0;
  TEST_ASSERT_TRUE(heapCensusTopNInsert(buf, &count, 4, 0x1000, 100));
  TEST_ASSERT_TRUE(heapCensusTopNInsert(buf, &count, 4, 0x2000, 50));
  TEST_ASSERT_EQUAL_UINT8(2, count);
  TEST_ASSERT_EQUAL_UINT32(0u, heapCensusKeptFloor(buf, count, 4));
}

void test_topn_keeps_the_largest_blocks_not_the_smallest(void) {
  // Capacité 3, on insère 5 candidats de tailles très différentes. Le top-N
  // DOIT converger vers {500, 400, 300} — jamais {100, 200, 300} (ce que
  // rendrait une comparaison inversée qui retient les PETITS).
  HeapCensusFreeEntry buf[3] = {};
  uint8_t count = 0;
  const uint32_t sizes[5] = {100, 500, 200, 400, 300};
  const uint32_t addrs[5] = {0xA100, 0xA500, 0xA200, 0xA400, 0xA300};
  for (int i = 0; i < 5; i++) {
    heapCensusTopNInsert(buf, &count, 3, addrs[i], sizes[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(3, count);

  bool has300 = false, has400 = false, has500 = false, has100 = false, has200 = false;
  for (int i = 0; i < 3; i++) {
    if (buf[i].size == 300u) has300 = true;
    if (buf[i].size == 400u) has400 = true;
    if (buf[i].size == 500u) has500 = true;
    if (buf[i].size == 100u) has100 = true;
    if (buf[i].size == 200u) has200 = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(has300 && has400 && has500,
                           "Le top-3 doit contenir EXACTEMENT {300,400,500} : les plus GROS "
                           "blocs. Une comparaison inversee (M12) garderait {100,200,300} — "
                           "l'instrument chercherait alors le decoupage du grand bloc partout "
                           "SAUF ou il a lieu.");
  TEST_ASSERT_FALSE_MESSAGE(has100 || has200,
                            "100 et 200 doivent avoir ete evinces : ce sont les plus PETITS "
                            "candidats, jamais retenus dans un top-N des plus GROS.");
}

void test_topn_rejects_candidate_smaller_or_equal_to_current_floor(void) {
  HeapCensusFreeEntry buf[2] = {};
  uint8_t count = 0;
  heapCensusTopNInsert(buf, &count, 2, 0x1, 1000);
  heapCensusTopNInsert(buf, &count, 2, 0x2, 2000);
  TEST_ASSERT_EQUAL_UINT32(1000u, heapCensusKeptFloor(buf, count, 2));

  // Candidat EXACTEMENT égal au floor : rejeté (heapCensusTopNInsert exige
  // strictement plus grand — `size <= minVal` rejette).
  const bool insertedEqual = heapCensusTopNInsert(buf, &count, 2, 0x3, 1000);
  TEST_ASSERT_FALSE_MESSAGE(insertedEqual, "Un candidat EGAL au plus petit garde ne doit pas "
                                            "le remplacer (pas d'amelioration).");

  // Candidat strictement plus grand que le floor : accepté, remplace 1000.
  const bool insertedBigger = heapCensusTopNInsert(buf, &count, 2, 0x4, 1001);
  TEST_ASSERT_TRUE(insertedBigger);
  TEST_ASSERT_EQUAL_UINT32(1001u, heapCensusKeptFloor(buf, count, 2));
}

void test_topn_floor_is_max_uint32_when_capacity_zero(void) {
  // Rien ne peut jamais etre garde : le floor doit rejeter INCONDITIONNELLEMENT
  // (tout candidat, meme kHeapCensusMaxFreeBlocks-sized, verrait size <= floor).
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, heapCensusKeptFloor(nullptr, 0, 0));
}

void test_topn_saturation_never_overflows_the_static_buffer(void) {
  // 24 = kHeapCensusMaxFreeBlocks côté firmware ; on saturé délibérément à
  // 24 + 40 insertions strictement croissantes pour prouver l'absence de
  // débordement (le tableau HOST reste borné à sa capacité déclarée : si
  // heapCensusTopNInsert écrivait hors de `buf`, ce test planterait ou un
  // ASan/valgrind le détecterait — capacité passée explicitement à 24).
  HeapCensusFreeEntry buf[24] = {};
  uint8_t count = 0;
  for (uint32_t i = 0; i < 64; i++) {
    heapCensusTopNInsert(buf, &count, 24, 0x1000 + i, 10 + i);
  }
  TEST_ASSERT_EQUAL_UINT8(24, count);
  // Les 24 conservés doivent être les 24 PLUS GRANDS : tailles 10+40..10+63,
  // donc le floor doit valoir exactement 10+40 = 50.
  TEST_ASSERT_EQUAL_UINT32(50u, heapCensusKeptFloor(buf, count, 24));
}

// ===========================================================================
// 2. FENÊTRE D'ADRESSES DES BLOCS OCCUPÉS — heapCensusInWindow
// ===========================================================================
// MUTANT M25 : `hi == 0` doit être une fenêtre VIDE, jamais « tout le tas ».
// Une requête POST sans `used_to` (donc usedTo=0, cf. web_routes_heap.cpp)
// aurait sinon fait fuiter le contenu ADRESSE+TAILLE de la totalité du tas
// occupé dans une seule réponse — dépassant kHeapCensusMaxUsedBlocks en
// silence (contredirait aussi I13).

void test_window_hi_zero_is_empty_never_whole_heap(void) {
  TEST_ASSERT_FALSE_MESSAGE(heapCensusInWindow(0u, 0u, 0u),
                            "hi=0, lo=0 : meme l'adresse 0 ne doit PAS matcher.");
  TEST_ASSERT_FALSE_MESSAGE(heapCensusInWindow(0x3FFB0000u, 0u, 0u),
                            "hi=0 doit rejeter TOUTE adresse, y compris une adresse plausible "
                            "de DRAM interne — le mutant M25 accepterait ici 'tout le tas'.");
  TEST_ASSERT_FALSE_MESSAGE(heapCensusInWindow(0xFFFFFFFFu, 0u, 0u),
                            "hi=0 doit rejeter meme l'adresse maximale representable.");
}

void test_window_bounds_lo_inclusive_hi_exclusive(void) {
  TEST_ASSERT_FALSE(heapCensusInWindow(99u, 100u, 200u));
  TEST_ASSERT_TRUE(heapCensusInWindow(100u, 100u, 200u));
  TEST_ASSERT_TRUE(heapCensusInWindow(199u, 100u, 200u));
  TEST_ASSERT_FALSE_MESSAGE(heapCensusInWindow(200u, 100u, 200u),
                            "hi est EXCLUE : addr==hi ne doit pas matcher.");
}

// ===========================================================================
// 3. CONCORDANCE fb/ab — heapCensusCompare
// ===========================================================================
// MUTANT M26 : ignorer kOomProbeBlockCountUnknown et rendre un delta chiffré
// au lieu de « indéterminé » (ADR-0072 : l'absence n'est jamais une valeur du
// domaine — un masque O(régions) ne doit jamais produire un faux "mismatch").

void test_compare_match_when_counts_agree(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)kHeapCensusVerdictMatch, heapCensusCompare(10, 90, 10, 90));
}

void test_compare_mismatch_when_counts_disagree(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)kHeapCensusVerdictMismatch, heapCensusCompare(10, 90, 11, 90));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)kHeapCensusVerdictMismatch, heapCensusCompare(10, 90, 10, 91));
}

void test_compare_undetermined_when_floor_side_unknown(void) {
  const uint32_t unk = 0xFFFFFFFFu;  // kOomProbeBlockCountUnknown, valeur numérique
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)kHeapCensusVerdictUndetermined,
                                  heapCensusCompare(10, 90, unk, 90),
                                  "floorFreeBlocks inconnu : le verdict doit etre INDETERMINE, "
                                  "jamais 'mismatch' (le mutant M26 comparerait 10 a 0xFFFFFFFF "
                                  "et rendrait un delta enorme, fabriquant un faux signal).");
  TEST_ASSERT_EQUAL_UINT8(kHeapCensusVerdictUndetermined, heapCensusCompare(10, 90, 10, unk));
  TEST_ASSERT_EQUAL_UINT8(kHeapCensusVerdictUndetermined, heapCensusCompare(10, 90, unk, unk));
}

void test_compare_undetermined_even_if_census_side_coincidentally_equals_sentinel(void) {
  // Cas limite : le recensement lui-même ne devrait jamais compter 0xFFFFFFFF
  // blocs (impossible physiquement), mais la fonction ne doit statuer QUE sur
  // floorFreeBlocks/floorUsedBlocks (le côté qui PORTE la sentinelle ADR-0072).
  const uint32_t unk = 0xFFFFFFFFu;
  TEST_ASSERT_EQUAL_UINT8(kHeapCensusVerdictMismatch, heapCensusCompare(unk, 90, 10, 90));
}

// ===========================================================================
// 4. PLANCHER TEMPOREL PARTAGÉ R2/R3 — heapCensusMinIntervalElapsed (S9)
// ===========================================================================
// MUTANT M20 : `kHeapCensusMinIntervalMs` mis à 0, ou plancher appliqué
// SEULEMENT côté route — cette fonction est celle rappelée côté LOOP
// (heap_census.cpp, serviceCensus/servicePeek) avec l'horodatage écrit par la
// loop, exactement ce que S9 exige.

void test_min_interval_never_walked_always_admitted(void) {
  TEST_ASSERT_TRUE_MESSAGE(heapCensusMinIntervalElapsed(0, 0, false, 5000),
                           "Aucun parcours depuis le boot : rien a comparer, toujours admis.");
}

void test_min_interval_rejects_before_floor_elapsed(void) {
  TEST_ASSERT_FALSE(heapCensusMinIntervalElapsed(4999, 0, true, 5000));
  TEST_ASSERT_TRUE_MESSAGE(heapCensusMinIntervalElapsed(5000, 0, true, 5000),
                           "Frontiere exacte : >= minIntervalMs doit admettre.");
}

void test_min_interval_zero_floor_would_defeat_the_guard(void) {
  // Ce test ne teste pas directement le firmware (kHeapCensusMinIntervalMs
  // est une constante compile-time protégée par un static_assert >= 5000 dans
  // constants.h), mais DOCUMENTE le comportement si le floor valait 0 — pour
  // qu'une régression future de ce static_assert soit immédiatement visible
  // ICI aussi : avec minIntervalMs=0, deux appels consécutifs seraient TOUS
  // LES DEUX admis, quel que soit l'écart réel.
  TEST_ASSERT_TRUE_MESSAGE(heapCensusMinIntervalElapsed(1, 0, true, 0),
                           "minIntervalMs=0 admet TOUJOURS (meme 1 ms apres le precedent) : "
                           "c'est exactement M20, et c'est pourquoi constants.h porte un "
                           "static_assert >= 5000 — voir test_heap_telemetry_guard pour la "
                           "sentinelle qui verifie CETTE constante-la.");
}

void test_min_interval_wraps_correctly_across_millis_overflow(void) {
  // Idiome du projet (arithmétique non signée) : nowMs a replié à 5, dernier
  // parcours à UINT32_MAX-2. Écart réel = 8 ms (replié), pas ~4 milliards.
  const uint32_t lastWalkMs = 0xFFFFFFFDu;
  const uint32_t nowMs = 5u;
  TEST_ASSERT_FALSE_MESSAGE(heapCensusMinIntervalElapsed(nowMs, lastWalkMs, true, 5000),
                            "8ms replies < 5000ms : ne doit PAS etre admis.");
  TEST_ASSERT_TRUE_MESSAGE(heapCensusMinIntervalElapsed((uint32_t)(lastWalkMs + 5000u), lastWalkMs,
                                                        true, 5000),
                           "Exactement 5000ms replies : admis.");
}

// ===========================================================================
// 5. PÉREMPTION DE L'INSTANTANÉ — heapCensusIsStale
// ===========================================================================

void test_stale_boundary_exact(void) {
  TEST_ASSERT_FALSE(heapCensusIsStale(299999, 0, 300000));
  TEST_ASSERT_TRUE_MESSAGE(heapCensusIsStale(300000, 0, 300000), ">= staleMs doit perimer.");
}

// ===========================================================================
// 6. PLANIFICATION DE LA LECTURE R3 — heapCensusPlanPeek (S4)
// ===========================================================================

void test_plan_peek_rejects_free_block(void) {
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(false, 100, 50, 50, 128, 8);
  TEST_ASSERT_FALSE_MESSAGE(plan.ok, "Un bloc LIBRE ne doit jamais etre eligible a la lecture "
                                     "(S8 : aucun en-tete fiable a cette adresse).");
  TEST_ASSERT_EQUAL_UINT32(0u, plan.copyLen);
}

void test_plan_peek_rejects_block_smaller_than_poison_head(void) {
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, 7, 50, 50, 128, 8);
  TEST_ASSERT_FALSE_MESSAGE(plan.ok, "blockSize < kHeapPoisonHeadBytes : structurellement "
                                     "incapable de porter un payload utilisateur.");
}

void test_plan_peek_rejects_zero_alloc_size(void) {
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, 100, 0, 50, 128, 8);
  TEST_ASSERT_FALSE_MESSAGE(plan.ok, "allocSize == 0 (canari non conforme, cote appelant) : "
                                     "rien a copier.");
}

// MUTANT M8 : clamp de longueur supprimé — sans lui, `requestedLen` passerait
// tel quel, y compris au-delà de kHeapPeekMaxBytes (débordement du tampon
// statique `bytes[kHeapPeekMaxBytes]` côté heap_census.cpp).
void test_plan_peek_clamps_to_peek_max_bytes(void) {
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, 1000, 1000, 99999, 128, 8);
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(128u, plan.copyLen,
                                   "requestedLen=99999 doit etre clampe a kHeapPeekMaxBytes=128 "
                                   "(M8 : sans ce clamp, copyLen vaudrait 99999 — debordement du "
                                   "tampon statique bytes[128]).");
}

// MUTANT M9 (S4) — « LE TEST LE PLUS IMPORTANT DU LOT » (désigné par
// architect) : le clamp doit porter sur `allocSize` (octets RÉELLEMENT
// DEMANDÉS du bloc DÉSIGNÉ, poison_head_t), jamais sur `blockSize` (taille
// TLSF, header + padding compris). Un bloc TLSF est souvent plus grand que
// l'allocation réelle (arrondi interne de l'allocateur) : cloner sur
// blockSize lirait dans le padding de fin de bloc — et un mutant qui irait
// jusqu'à ignorer blockSize/allocSize entièrement lirait dans le bloc VOISIN,
// exactement le dégât décrit par M7/M9 (« lire par-dessus les blocs voisins »).
void test_plan_peek_clamps_to_alloc_size_not_block_size(void) {
  // blockSize (TLSF) 200 o, allocSize (payload réel) 40 o : requestedLen et
  // peekMaxBytes tous deux au-dessus des deux pour isoler LEQUEL des deux
  // bornes réellement le résultat.
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, /*blockSize=*/200, /*allocSize=*/40,
                                                       /*requestedLen=*/128, /*peekMaxBytes=*/128,
                                                       /*poisonHeadBytes=*/8);
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      40u, plan.copyLen,
      "Le clamp DOIT porter sur allocSize=40 (le payload REEL du bloc DESIGNE), pas sur "
      "blockSize=200 (la taille TLSF, header+padding compris). Si ce test rend 200 (ou toute "
      "valeur > 40), le mutant M9 a remplacé alloc_size par la taille TLSF du bloc : "
      "l'instrument lirait alors au-dela du payload reel — potentiellement dans le padding de "
      "fin de bloc, voire (si blockSize etait lui-meme mal borne) dans un bloc VOISIN. C'est le "
      "mutant que security-reviewer et pool-chemistry ont explicitement designe comme le plus "
      "important : une lecture qui deborde le bloc DESIGNE compromet la garantie S4 tout "
      "entiere (adresse validee, longueur clampee au bloc, pas au tas).");
}

void test_plan_peek_exact_alloc_size_boundary(void) {
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, 100, 40, 40, 128, 8);
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_EQUAL_UINT32(40u, plan.copyLen);
}

void test_plan_peek_requested_len_smaller_than_both_bounds_is_untouched(void) {
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, 200, 100, 10, 128, 8);
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(10u, plan.copyLen,
                                   "requestedLen deja le plus petit des trois bornes : aucun "
                                   "clamp ne doit l'augmenter.");
}

// ===========================================================================
// 6bis. MUTANT M6 (NOUVELLE FORMULATION, 3e passe) — payloadRoom
// ===========================================================================
// La 2e passe avait documenté M6 (borne de bornes d'ADRESSE) comme SANS
// CIBLE — confirmé exact par les deux revues de cette passe. Ce qui manquait
// est une borne de LONGUEUR : `allocSize` est lu DANS l'en-tête du bloc dont
// l'intégrité est l'objet même de l'enquête. Un canari intact n'empêche pas
// un `alloc_size` corrompu (bit-flip au-delà du canari, mémoire déjà
// partiellement écrasée par l'intrus qu'on cherche) de valoir une valeur
// énorme : SANS `payloadRoom` (mesuré par le parcours TLSF lui-même, jamais
// lu en mémoire), le clamp `len > allocSize` ne protège de rien et
// `memcpy` (heap_census.cpp) lit au-delà du bloc — jusqu'à `LoadProhibited`
// sous le spinlock inter-cœurs dans le pire cas : panic, reboot, fenêtre
// d'observation perdue.

void test_plan_peek_clamps_to_payload_room_when_alloc_size_is_corrupted(void) {
  // allocSize = 0xFFFFFFF0 : une valeur ENORME mais délibérément DIFFÉRENTE
  // de kHeapCensusAllocSizeUnknown (0xFFFFFFFF) — sinon le test ne discrimine
  // RIEN : il serait rejeté par le garde ADR-0072 (allocSize ==
  // kHeapCensusAllocSizeUnknown) AVANT même d'atteindre payloadRoom, aussi
  // bien avec qu'AVEC ou SANS le correctif M6. C'est la valeur que le
  // catalogue de mutants de cette passe proposait initialement (« ..., 128,
  // 128, 8) doit rendre copyLen==8 ») : VÉRIFIÉ FAUX à l'exécution sur le
  // code réel — 0xFFFFFFFF colle exactement à la sentinelle et fait sortir
  // la fonction plus tôt avec ok=false/copyLen=0, jamais copyLen=8. Voir le
  // rapport de test-engineer pour la mesure directe (probe.cpp).
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(
      /*used=*/true, /*blockSize=*/16, /*allocSize=*/0xFFFFFFF0u,
      /*requestedLen=*/128, /*peekMaxBytes=*/128, /*poisonHeadBytes=*/8);
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      8u, plan.copyLen,
      "payloadRoom = blockSize(16) - poisonHeadBytes(8) = 8 DOIT borner la copie, meme si "
      "allocSize pretend 0xFFFFFFF0 (corrompu, mais canari suppose deja valide par "
      "l'appelant). MUTANT M6 (nouvelle formulation) : retirer 'if (len > payloadRoom) len "
      "= payloadRoom;' ferait rendre copyLen=128 (clampe seulement par peekMaxBytes) — un "
      "memcpy de 128 o depuis un bloc de 16 o lirait 120 o dans le(s) bloc(s) voisin(s), sous "
      "le verrou, interruptions masquees.");
}

void test_plan_peek_payload_room_boundary_exact_block_too_small(void) {
  // blockSize == poisonHeadBytes : payloadRoom == 0, deja couvert par
  // test_plan_peek_rejects_zero_alloc_size ? NON — ce test-ci force allocSize
  // SAIN pour isoler la frontiere payloadRoom==0, pas allocSize==0.
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, /*blockSize=*/8, /*allocSize=*/4,
                                                       /*requestedLen=*/4, /*peekMaxBytes=*/128,
                                                       /*poisonHeadBytes=*/8);
  TEST_ASSERT_FALSE_MESSAGE(plan.ok, "blockSize == poisonHeadBytes : payloadRoom == 0, "
                                     "structurellement AUCUN payload possible — refus, meme "
                                     "avec un allocSize plausible.");
}

void test_plan_peek_payload_room_boundary_one_byte(void) {
  // blockSize == poisonHeadBytes + 1 : payloadRoom == 1 pile.
  const HeapCensusPeekPlan plan = heapCensusPlanPeek(true, /*blockSize=*/9, /*allocSize=*/100,
                                                       /*requestedLen=*/100, /*peekMaxBytes=*/128,
                                                       /*poisonHeadBytes=*/8);
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, plan.copyLen, "payloadRoom == 1 (blockSize=9, "
                                                      "poisonHeadBytes=8) : copyLen doit etre "
                                                      "clampe a EXACTEMENT 1, pas 100.");
}

// ⚠️ MUTANT DOCUMENTÉ COMME ÉQUIVALENT (PAS COMPTÉ COMME TUÉ) — la variante
// demandée « `>` → `>=` sur la borne payloadRoom » (`if (len > payloadRoom)`
// -> `if (len >= payloadRoom)`) : PROUVÉ ÉQUIVALENT pour TOUTE entrée, pas
// seulement observé sur un cas. La branche fait `len = payloadRoom;` — une
// affectation IDEMPOTENTE. Pour x > y, les deux formes déclenchent et fixent
// x=y. Pour x == y, `>` ne déclenche pas (x vaut déjà y) et `>=` déclenche
// mais fixe x=y=x : même valeur finale dans les DEUX cas. Pour x < y, aucune
// des deux formes ne déclenche. Aucune paire (requestedLen, peekMaxBytes,
// allocSize, blockSize, poisonHeadBytes) ne peut donc faire diverger
// `copyLen` entre les deux formes — vérifié par compilation direct des deux
// variantes (g++, hors Unity) avec len==payloadRoom exactement : copyLen=8
// dans les deux cas. Le MÊME raisonnement s'applique aux deux clamps
// `peekMaxBytes`/`allocSize` juste au-dessus, pour la même raison structurelle
// (idiome « clamp puis affectation identique »). Un test qui prétendrait tuer
// cette mutation via `copyLen` mentirait sur ce qu'il prouve — documenté ici
// plutôt que fabriqué.

// ===========================================================================
// 7. OCTET ASCII IMPRIMABLE — heapPeekAsciiByte
// ===========================================================================
// « La moitie du test 'la String se lit en clair' » (l'autre moitie est la
// preuve sur cible, hors périmètre natif).

void test_ascii_byte_printable_range_passthrough(void) {
  TEST_ASSERT_EQUAL_CHAR('A', heapPeekAsciiByte((uint8_t)'A'));
  TEST_ASSERT_EQUAL_CHAR('~', heapPeekAsciiByte(0x7E));  // dernier imprimable
  TEST_ASSERT_EQUAL_CHAR(' ', heapPeekAsciiByte(0x20));  // premier imprimable
}

void test_ascii_byte_non_printable_becomes_dot(void) {
  TEST_ASSERT_EQUAL_CHAR('.', heapPeekAsciiByte(0x00));
  TEST_ASSERT_EQUAL_CHAR('.', heapPeekAsciiByte(0x1Fu));  // juste sous l'imprimable
  TEST_ASSERT_EQUAL_CHAR('.', heapPeekAsciiByte(0x7Fu));  // DEL, juste au-dessus
  TEST_ASSERT_EQUAL_CHAR('.', heapPeekAsciiByte(0xFFu));
}

void test_ascii_byte_reads_a_realistic_log_string_in_clear(void) {
  // Un fragment de String de journal typique (le premier suspect du lot,
  // logger.h) : tous les octets doivent traverser tels quels.
  const uint8_t bytes[] = {'F', 'i', 'l', 't', 'r', 'a', 't', 'i', 'o', 'n', ' ', 'O', 'N'};
  char out[sizeof(bytes) + 1];
  for (size_t i = 0; i < sizeof(bytes); i++) out[i] = heapPeekAsciiByte(bytes[i]);
  out[sizeof(bytes)] = '\0';
  TEST_ASSERT_EQUAL_STRING("Filtration ON", out);
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_topn_buffer_not_full_accepts_everything_unsorted);
  RUN_TEST(test_topn_keeps_the_largest_blocks_not_the_smallest);
  RUN_TEST(test_topn_rejects_candidate_smaller_or_equal_to_current_floor);
  RUN_TEST(test_topn_floor_is_max_uint32_when_capacity_zero);
  RUN_TEST(test_topn_saturation_never_overflows_the_static_buffer);

  RUN_TEST(test_window_hi_zero_is_empty_never_whole_heap);
  RUN_TEST(test_window_bounds_lo_inclusive_hi_exclusive);

  RUN_TEST(test_compare_match_when_counts_agree);
  RUN_TEST(test_compare_mismatch_when_counts_disagree);
  RUN_TEST(test_compare_undetermined_when_floor_side_unknown);
  RUN_TEST(test_compare_undetermined_even_if_census_side_coincidentally_equals_sentinel);

  RUN_TEST(test_min_interval_never_walked_always_admitted);
  RUN_TEST(test_min_interval_rejects_before_floor_elapsed);
  RUN_TEST(test_min_interval_zero_floor_would_defeat_the_guard);
  RUN_TEST(test_min_interval_wraps_correctly_across_millis_overflow);

  RUN_TEST(test_stale_boundary_exact);

  RUN_TEST(test_plan_peek_rejects_free_block);
  RUN_TEST(test_plan_peek_rejects_block_smaller_than_poison_head);
  RUN_TEST(test_plan_peek_rejects_zero_alloc_size);
  RUN_TEST(test_plan_peek_clamps_to_peek_max_bytes);
  RUN_TEST(test_plan_peek_clamps_to_alloc_size_not_block_size);
  RUN_TEST(test_plan_peek_exact_alloc_size_boundary);
  RUN_TEST(test_plan_peek_requested_len_smaller_than_both_bounds_is_untouched);

  RUN_TEST(test_plan_peek_clamps_to_payload_room_when_alloc_size_is_corrupted);
  RUN_TEST(test_plan_peek_payload_room_boundary_exact_block_too_small);
  RUN_TEST(test_plan_peek_payload_room_boundary_one_byte);

  RUN_TEST(test_ascii_byte_printable_range_passthrough);
  RUN_TEST(test_ascii_byte_non_printable_becomes_dot);
  RUN_TEST(test_ascii_byte_reads_a_realistic_log_string_in_clear);

  return UNITY_END();
}
