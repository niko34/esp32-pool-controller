// =============================================================================
// Tests natifs — PLANCHERS GLISSANTS, fetch_min, médiane approchée, lignes de
// journal (feature-122, lot C-1a)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
//
// CE QUI EST SOUS TEST :
//   T2 — un minimum n'est JAMAIS oublié (heapFloorUpdate), la fenêtre se
//        réarme, le tour de rôle visite bien les DEUX masques, et le
//        `fetch_min` tient sous DEUX ÉCRIVAINS ENTRELACÉS ;
//   T5 — la médiane approchée par seaux dyadiques et son BIAIS (surestimation
//        bornée par la largeur du seau), frontières exactes comprises ;
//   T7 — les pires cas des lignes de journal, vérifiés PAR CONSTRUCTION depuis
//        le format réellement écrit dans src/main.cpp (ce sont des char[] de
//        pile : une troncature serait SILENCIEUSE), et l'intégrité de la ligne
//        « Heap 8bits: » dont dépendent 152 relevés historiques.
//
// ⚠️ Le `fetch_min` est testé À TRAVERS la fonction que le firmware appelle
// (heapAtomicFetchMin, src/heap_caps_logic.h), avec un atomique SCRIPTÉ qui joue
// le second écrivain. Aucun modèle parallèle n'est écrit ici : ce dépôt en porte
// déjà un (oomProbeTakeTokenModel), et un modèle orphelin produit des tests
// verts qui n'attestent de rien.
// =============================================================================

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap_caps_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// T2.a — UN MINIMUM N'EST JAMAIS OUBLIÉ
// ===========================================================================
// Le cas limite de la spec est explicite : « Le minimum doit être retenu par un
// plancher glissant, jamais par un instantané ». Un échantillonnage à 2 s rate
// par construction une pointe de quelques millisecondes ; seul le repliement
// par minimum la conserve.

void test_floor_keeps_the_deepest_trough_not_the_last_sample(void) {
  HeapFloorSlot s = {kHeapFloorEmpty, 0};

  TEST_ASSERT_TRUE_MESSAGE(heapFloorUpdate(s, 40000, kHeapCtxWsClients),
                           "Un plancher VIDE doit accepter le premier echantillon.");
  TEST_ASSERT_EQUAL_UINT32(40000u, s.minBytes);

  TEST_ASSERT_TRUE_MESSAGE(heapFloorUpdate(s, 12000, kHeapCtxHttpBurst),
                           "Un echantillon plus BAS doit abaisser le plancher.");
  TEST_ASSERT_EQUAL_UINT32(12000u, s.minBytes);

  // Le creux passe, la mémoire remonte : c'est ICI que se joue tout le lot.
  TEST_ASSERT_FALSE_MESSAGE(heapFloorUpdate(s, 51000, kHeapCtxDosing),
                            "Un echantillon plus HAUT ne doit RIEN changer.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(12000u, s.minBytes,
                                   "LE CREUX DE 12 000 DOIT SURVIVRE. Si cette valeur vaut 51 000, "
                                   "le plancher a ete remplace par un INSTANTANE et le "
                                   "dimensionnement d'une reserve DMA (feature-123) reposerait sur "
                                   "un minimum trop optimiste.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kHeapCtxHttpBurst, s.ctxAtMin,
                                  "Le contexte publie doit etre celui du CREUX (rafale HTTP), pas "
                                  "celui du dernier echantillon (dosage) : sinon le plancher ne "
                                  "dit pas QUEL evenement l'a produit (A5).");
}

void test_floor_equal_sample_does_not_rewrite_the_context(void) {
  // `>=` et non `>` : à valeur égale, le PREMIER contexte est conservé. Sans
  // cela, un régime calme qui repasse par la même valeur volerait l'attribution
  // du creux à l'événement qui l'a réellement produit.
  HeapFloorSlot s = {kHeapFloorEmpty, 0};
  heapFloorUpdate(s, 20000, kHeapCtxMqttDiscovery);
  TEST_ASSERT_FALSE(heapFloorUpdate(s, 20000, kHeapCtxWsClients));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kHeapCtxMqttDiscovery, s.ctxAtMin,
                                  "A valeur egale, le contexte du premier creux est garde.");
}

void test_floor_accepts_zero_and_keeps_it(void) {
  // Cas dégénéré mais réel : un tas peut annoncer 0 octet libre pour un masque
  // qu'aucune région ne sert. 0 est une VALEUR, pas une absence — et
  // kHeapFloorEmpty (0xFFFFFFFF) est l'absence.
  HeapFloorSlot s = {kHeapFloorEmpty, 0};
  TEST_ASSERT_TRUE(heapFloorUpdate(s, 0, kHeapCtxDosing));
  TEST_ASSERT_EQUAL_UINT32(0u, s.minBytes);
  TEST_ASSERT_FALSE_MESSAGE(heapFloorUpdate(s, 1, 0),
                            "Rien n'est plus bas que 0 : le plancher ne bouge plus.");
  TEST_ASSERT_EQUAL_UINT32(0u, s.minBytes);
}

// ===========================================================================
// T2.b — LA FENÊTRE SE RÉARME, LE PLANCHER DEPUIS-BOOT NE SE RÉARME PAS
// ===========================================================================
// Deux planchers par masque, et c'est volontaire : celui depuis le boot sature
// après le premier creux profond (utile pour le pire cas absolu, aveugle à
// l'évolution) ; celui de fenêtre, réarmé après chaque report, dit si les creux
// s'AGGRAVENT d'une période de trace à l'autre.

void test_window_rearms_while_boot_floor_survives(void) {
  HeapFloorSlot boot = {kHeapFloorEmpty, 0};
  HeapFloorSlot window = {kHeapFloorEmpty, 0};

  // Fenêtre 1 : creux profond.
  const uint32_t serie1[] = {45000, 9000, 30000};
  for (int i = 0; i < 3; ++i) {
    heapFloorUpdate(boot, serie1[i], kHeapCtxHttpBurst);
    heapFloorUpdate(window, serie1[i], kHeapCtxHttpBurst);
  }
  TEST_ASSERT_EQUAL_UINT32(9000u, boot.minBytes);
  TEST_ASSERT_EQUAL_UINT32(9000u, window.minBytes);

  // Report, puis RÉARMEMENT de la seule fenêtre (ce que fait
  // heapFloorsRearmWindow : store(kHeapFloorEmpty) sur les planchers de fenêtre).
  window.minBytes = kHeapFloorEmpty;
  window.ctxAtMin = 0;

  // Fenêtre 2 : régime plus calme.
  const uint32_t serie2[] = {48000, 26000};
  for (int i = 0; i < 2; ++i) {
    heapFloorUpdate(boot, serie2[i], kHeapCtxWsClients);
    heapFloorUpdate(window, serie2[i], kHeapCtxWsClients);
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(26000u, window.minBytes,
                                   "La fenetre rearmee doit rapporter le creux de la PERIODE "
                                   "COURANTE (26 000), sinon on ne verrait jamais une amelioration "
                                   "ni une aggravation.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(9000u, boot.minBytes,
                                   "Le plancher DEPUIS-BOOT ne se rearme JAMAIS : le pire cas "
                                   "absolu de 9 000 o est ce qui calibrera la reserve.");
}

// ===========================================================================
// T2.c — LE TOUR DE RÔLE VISITE BIEN LES DEUX MASQUES
// ===========================================================================
// Un seul créneau O(blocs) est libre par échantillon (plafond du gate
// pool-chemistry : 1 parcours/s). Le tour de rôle est ce qui permet de suivre
// 0x080C ET 0x1804 sans dépasser ce plafond — mais s'il restait bloqué sur le
// premier, la classe d'échec observée en production (0x080C) ne serait jamais
// mesurée et le lot ne servirait à rien.

void test_round_robin_visits_both_walk_masks_alternately(void) {
  const uint8_t ringSize = 2;
  uint8_t cursor = 0;
  int visits[2] = {0, 0};
  int sequence[6];
  for (int i = 0; i < 6; ++i) {
    sequence[i] = cursor;
    visits[cursor]++;
    cursor = heapMaskRoundRobinNext(cursor, ringSize);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, visits[0], "Le masque 0 doit etre visite 3 fois sur 6.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, visits[1],
                                "Le masque 1 (0x1804, la candidate) doit etre visite 3 fois sur 6. "
                                "Un curseur bloque a 0 le laisserait a JAMAIS non mesure.");
  const int attendu[6] = {0, 1, 0, 1, 0, 1};
  for (int i = 0; i < 6; ++i) {
    char msg[160];
    snprintf(msg, sizeof(msg), "Echantillon %d : alternance strict attendue (0,1,0,1...).", i);
    TEST_ASSERT_EQUAL_INT_MESSAGE(attendu[i], sequence[i], msg);
  }
}

void test_round_robin_is_robust_to_corrupted_cursor_and_empty_ring(void) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, heapMaskRoundRobinNext(0, 0),
                                  "Anneau vide : retour a 0, jamais d'index hors bornes.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, heapMaskRoundRobinNext(200, 2),
                                  "Curseur hors bornes (RAM corrompue) : repli sur 0.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, heapMaskRoundRobinNext(255, 2),
                                  "Pas de debordement de l'increment uint8.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, heapMaskRoundRobinNext(0, 1),
                                  "Anneau d'un seul masque : le curseur reste a 0.");
}

// ===========================================================================
// T2.d — LE `fetch_min` SOUS DEUX ÉCRIVAINS ENTRELACÉS
// ===========================================================================
// MOTIF (condition 1 du gate pool-chemistry) : l'idiome de course « bénigne »
// du plancher 0x0804 n'était documenté que pour UN SEUL écrivain (loopTask).
// Depuis ce lot, il est aussi alimenté depuis `async_tcp` (filtre de la garde
// 503, web_server.cpp) et depuis n'importe quelle tâche ouvrant un fichier
// (fs_guard.cpp). Avec deux écrivains, un `load`-`compare`-`store` PERD un
// minimum au lieu de le reporter.
//
// COMMENT ON LE PROUVE SANS FIL D'EXÉCUTION : un atomique SCRIPTÉ qui joue le
// second écrivain à l'instant précis où il fait mal — entre notre `load` et
// notre tentative d'écriture, quelle que soit la primitive employée (`store`
// pour un load-compare-store, `compare_exchange_weak` pour un fetch_min). Les
// deux implémentations subissent donc EXACTEMENT le même entrelacement ; seule
// la bonne survit. Un test à vrais fils serait probabiliste ; celui-ci est
// déterministe.

namespace {
// Faux atomique : compte les opérations et injecte l'écriture d'un concurrent
// AVANT la première tentative d'écriture, qu'elle soit un store ou un CAS.
struct ScriptedAtomicU32 {
  uint32_t value = 0;
  uint32_t concurrentValue = 0;  // ce que le second écrivain pose
  bool concurrentPending = false;
  int loads = 0;
  int stores = 0;
  int casCalls = 0;

  // Le second écrivain s'exécute ICI, une seule fois, juste avant que nous
  // n'écrivions : c'est la fenêtre exacte du minimum perdu.
  void runConcurrentWriterOnce() {
    if (!concurrentPending) return;
    concurrentPending = false;
    value = concurrentValue;
  }

  uint32_t load(int) {
    ++loads;
    return value;
  }
  void store(uint32_t v, int) {
    ++stores;
    runConcurrentWriterOnce();
    value = v;  // ÉCRASEMENT inconditionnel : la faute du load-compare-store
  }
  bool compare_exchange_weak(uint32_t& expected, uint32_t desired, int, int) {
    ++casCalls;
    runConcurrentWriterOnce();
    if (value == expected) {
      value = desired;
      return true;
    }
    expected = value;  // le contrat de <atomic> : `expected` est rafraîchi
    return false;
  }
};
const int kRelaxedStub = 0;  // l'ordre mémoire est un paramètre de patron
}  // namespace

void test_fetch_min_does_not_lose_a_deeper_concurrent_trough(void) {
  // SCÉNARIO DU MINIMUM PERDU : le plancher vaut 30 000. L'écrivain A (nous)
  // veut y poser 20 000. L'écrivain B glisse 12 000 avant notre écriture.
  // Le résultat CORRECT est 12 000 : nous n'avons rien à y faire.
  ScriptedAtomicU32 a;
  a.value = 30000;
  a.concurrentValue = 12000;
  a.concurrentPending = true;

  heapAtomicFetchMin(a, 20000u, kRelaxedStub);

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      12000u, a.value,
      "LE CREUX DE 12 000 DU SECOND ECRIVAIN A ETE EFFACE. C'est le defaut exact que la "
      "condition 1 du gate pool-chemistry visait : avec un load-compare-store, A charge 30 000, "
      "B charge 30 000, A stocke 12 000, B stocke 20 000 — et le plus profond des deux creux "
      "disparait. Le plancher 0x0804 a maintenant TROIS ecrivains (loop, garde 503 sur "
      "async_tcp, garde d'ouverture de fichiers).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, a.stores,
                                "Un fetch_min n'appelle JAMAIS store() : un store inconditionnel "
                                "est precisement ce qui perd le minimum d'un concurrent.");
}

void test_fetch_min_retries_and_wins_when_the_concurrent_trough_is_shallower(void) {
  // Variante symétrique : le concurrent pose 20 000, nous voulons 12 000. Le CAS
  // échoue (la valeur a changé), la boucle RECHARGE et réessaie. Résultat
  // attendu : 12 000, et DEUX tentatives de CAS.
  ScriptedAtomicU32 a;
  a.value = 30000;
  a.concurrentValue = 20000;
  a.concurrentPending = true;

  heapAtomicFetchMin(a, 12000u, kRelaxedStub);

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(12000u, a.value,
                                   "Notre creux de 12 000 doit finir par etre pose malgre "
                                   "l'interference : la boucle reessaie apres avoir recharge.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, a.casCalls,
                                "Exactement deux tentatives : la premiere echoue (valeur changee "
                                "sous nos pieds), la seconde reussit. Zero tentative signifierait "
                                "que la boucle n'a pas ete reevaluee.");
}

void test_fetch_min_is_a_no_op_when_the_sample_is_higher(void) {
  ScriptedAtomicU32 a;
  a.value = 15000;
  heapAtomicFetchMin(a, 40000u, kRelaxedStub);
  TEST_ASSERT_EQUAL_UINT32(15000u, a.value);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, a.casCalls,
                                "Aucune ecriture ni tentative si l'echantillon est plus haut : le "
                                "cout du fetch_min est nul en regime normal.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, a.loads, "Un seul load suffit alors.");
}

void test_fetch_min_from_empty_floor_takes_the_first_sample(void) {
  ScriptedAtomicU32 a;
  a.value = 0xFFFFFFFFu;  // UINT32_MAX = plancher vide (idiome de heap_metrics.cpp)
  heapAtomicFetchMin(a, 31000u, kRelaxedStub);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(31000u, a.value,
                                   "Le premier echantillon doit s'inscrire dans un plancher vide.");
}

void test_fetch_min_two_writers_interleaved_over_a_long_series(void) {
  // Entrelacement RÉPÉTÉ : à chaque tour, un concurrent pose une valeur tirée en
  // alternance au-dessus puis en dessous de la nôtre. À la fin, le plancher doit
  // valoir le MINIMUM GLOBAL de tout ce qui a été proposé par les deux
  // écrivains — c'est la définition d'un plancher glissant.
  ScriptedAtomicU32 a;
  a.value = 0xFFFFFFFFu;
  uint32_t globalMin = 0xFFFFFFFFu;
  for (int i = 0; i < 200; ++i) {
    const uint32_t nous = (uint32_t)(50000 - i * 137);
    const uint32_t lui = (uint32_t)(50000 - ((i * 71) % 300) * 149);
    if (nous < globalMin) globalMin = nous;
    if (lui < globalMin) globalMin = lui;
    a.concurrentValue = lui;
    a.concurrentPending = true;
    heapAtomicFetchMin(a, nous, kRelaxedStub);
    // Le concurrent qui n'a pas eu à s'exécuter (aucune écriture de notre part)
    // écrit tout de même : les deux écrivains sont indépendants.
    if (a.concurrentPending) {
      a.concurrentPending = false;
      if (lui < a.value) a.value = lui;
    }
  }
  char msg[256];
  snprintf(msg, sizeof(msg),
           "Apres 200 tours a deux ecrivains entrelaces, le plancher doit valoir le minimum "
           "global (%u) et non une valeur plus haute laissee par un ecrasement. Obtenu %u.",
           (unsigned)globalMin, (unsigned)a.value);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(globalMin, a.value, msg);
}

// ===========================================================================
// T2.e — LA FENÊTRE DE CONTEXTE « RAFALE DE DÉCOUVERTE » (A5)
// ===========================================================================

void test_discovery_context_window(void) {
  const uint32_t w = 60000;  // kMqttDiscoveryCtxWindowMs
  TEST_ASSERT_FALSE_MESSAGE(heapCtxDiscoveryWindowActive(100000, 0, w),
                            "lastMarkMs == 0 = JAMAIS de rafale : millis() ne vaut 0 que pendant "
                            "la premiere milliseconde du boot, ou le WiFi n'est pas monte.");
  TEST_ASSERT_TRUE_MESSAGE(heapCtxDiscoveryWindowActive(100000, 100000, w),
                           "L'instant du repere lui-meme est dans la fenetre.");
  TEST_ASSERT_TRUE(heapCtxDiscoveryWindowActive(159999, 100000, w));
  TEST_ASSERT_FALSE_MESSAGE(heapCtxDiscoveryWindowActive(160000, 100000, w),
                            "Fenetre STRICTE : a 60 000 ms pile, le marquage cesse.");
  // Repliement de millis() (49,7 j) : le piège maison — une comparaison signée
  // ou un `now > mark + w` casserait ici. Trois défauts réels de ce dépôt
  // viennent de ce motif.
  TEST_ASSERT_TRUE_MESSAGE(heapCtxDiscoveryWindowActive(1000u, 0xFFFFF000u, w),
                           "Au repliement de millis(), la soustraction non signee reste juste : "
                           "0x1000 + 1000 ms d'ecart, donc DANS la fenetre.");
  TEST_ASSERT_FALSE_MESSAGE(heapCtxDiscoveryWindowActive(0xFFFFF000u + 60000u, 0xFFFFF000u, w),
                            "Et elle expire correctement de l'autre cote du repliement.");
}

void test_context_bits_are_distinct_single_bits(void) {
  // Les quatre bits doivent rester disjoints : ils sont publiés en hexa dans le
  // journal (« c=0x06 » = rafale HTTP + dosage) et lus à la main.
  const uint8_t bits[4] = {kHeapCtxWsClients, kHeapCtxHttpBurst, kHeapCtxDosing,
                           kHeapCtxMqttDiscovery};
  uint8_t ou = 0;
  for (int i = 0; i < 4; ++i) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Le bit de contexte %d doit etre une puissance de deux.", i);
    TEST_ASSERT_TRUE_MESSAGE(bits[i] != 0 && (bits[i] & (uint8_t)(bits[i] - 1)) == 0, msg);
    snprintf(msg, sizeof(msg), "Le bit de contexte %d chevauche un autre.", i);
    TEST_ASSERT_TRUE_MESSAGE((ou & bits[i]) == 0, msg);
    ou = (uint8_t)(ou | bits[i]);
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0Fu, ou,
                                  "Les quatre bits doivent tenir dans le quartet bas : le journal "
                                  "les imprime en %02X et l'octet de contexte est un uint8_t.");
}

// ===========================================================================
// T5 — LA MÉDIANE APPROCHÉE ET SON BIAIS
// ===========================================================================
// Condition 5 du gate : le plafond de coût doit s'adosser à une durée OBSERVÉE.
// Sans tri, sans allocation, sans flottant : histogramme à 7 seaux dyadiques,
// et la valeur rendue est la BORNE SUPÉRIEURE du seau contenant le rang médian
// — donc une SURESTIMATION bornée par la largeur du seau. Le biais est du bon
// côté pour un plafond (on ne se croit jamais plus rapide qu'on n'est), mais il
// faut le connaître avant de comparer aux 300 µs de kOomProbeWarnDurationUs.

void test_bucket_index_boundaries(void) {
  struct Cas {
    uint32_t us;
    uint8_t bucket;
  };
  const Cas cas[] = {{0, 0},    {1, 0},     {31, 0},   {32, 1},   {63, 1},    {64, 2},
                     {84, 2},   {127, 2},   {128, 3},  {255, 3},  {256, 4},   {511, 4},
                     {512, 5},  {1023, 5},  {1024, 6}, {5000, 6}, {65535, 6}, {1000000, 6}};
  const int n = (int)(sizeof(cas) / sizeof(cas[0]));
  for (int i = 0; i < n; ++i) {
    char msg[192];
    snprintf(msg, sizeof(msg),
             "%u us doit tomber dans le seau %u (bornes superieures : 32/64/128/256/512/1024, "
             "puis hors echelle). Les FRONTIERES sont exclusives en haut : une duree egale a la "
             "borne appartient au seau SUIVANT.",
             (unsigned)cas[i].us, (unsigned)cas[i].bucket);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(cas[i].bucket, heapWalkBucketIndex(cas[i].us), msg);
  }
}

void test_median_of_the_84us_landmark_reports_128(void) {
  // Le repère du dossier : ~84 µs (docs/subsystems/oom-guard.md). Le seau est
  // [64, 128) et la médiane est donc RAPPORTÉE à 128 µs.
  uint32_t counts[kHeapWalkBucketCount] = {0, 0, 0, 0, 0, 0, 0};
  counts[heapWalkBucketIndex(84)] = 150;
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      128u, heapWalkMedianUpperUs(counts, kHeapWalkBucketCount),
      "Autour du repere de 84 us, la mediane doit rapporter 128 us — pas 84. C'est la borne "
      "SUPERIEURE du seau, une surestimation d'au plus 63 us ici, a garder en tete quand on "
      "compare aux 300 us du gate.");
}

void test_median_bias_is_an_overestimate_bounded_by_bucket_width(void) {
  struct Cas {
    uint32_t echantillon;
    uint32_t rapporte;
    uint32_t biaisMax;
    const char* note;
  };
  const Cas cas[] = {
      {64, 128, 64, "plancher exact du seau [64,128) : biais MAXIMAL (64 us)"},
      {127, 128, 64, "sommet du seau : biais minimal (1 us)"},
      {128, 256, 128, "FRONTIERE : 128 appartient au seau SUIVANT, rapporte 256"},
      {31, 32, 32, "premier seau [0,32)"},
      {32, 64, 32, "frontiere du premier seau"},
      {1023, 1024, 512, "dernier seau borne"},
  };
  const int n = (int)(sizeof(cas) / sizeof(cas[0]));
  for (int i = 0; i < n; ++i) {
    uint32_t counts[kHeapWalkBucketCount] = {0, 0, 0, 0, 0, 0, 0};
    counts[heapWalkBucketIndex(cas[i].echantillon)] = 7;
    const uint32_t got = heapWalkMedianUpperUs(counts, kHeapWalkBucketCount);
    char msg[320];
    snprintf(msg, sizeof(msg), "Echantillons a %u us : mediane rapportee attendue %u us (%s).",
             (unsigned)cas[i].echantillon, (unsigned)cas[i].rapporte, cas[i].note);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(cas[i].rapporte, got, msg);
    snprintf(msg, sizeof(msg),
             "Le biais doit etre une SURESTIMATION (%u >= %u) bornee par la largeur du seau "
             "(<= %u us).",
             (unsigned)got, (unsigned)cas[i].echantillon, (unsigned)cas[i].biaisMax);
    TEST_ASSERT_TRUE_MESSAGE(got >= cas[i].echantillon &&
                                 (got - cas[i].echantillon) <= cas[i].biaisMax,
                             msg);
  }
}

void test_median_out_of_scale(void) {
  uint32_t counts[kHeapWalkBucketCount] = {0, 0, 0, 0, 0, 0, 0};
  counts[kHeapWalkBucketCount - 1] = 3;
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      kHeapWalkOverflowUs, heapWalkMedianUpperUs(counts, kHeapWalkBucketCount),
      "Mediane dans le dernier seau : 65535 = « >= 1024 us, hors echelle ». Cette valeur doit "
      "rester reconnaissable dans le journal, jamais confondue avec une duree mesuree.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(65535u, kHeapWalkOverflowUs,
                                   "La sentinelle hors echelle est 65535.");
}

void test_median_rank_and_no_sample_cases(void) {
  uint32_t vide[kHeapWalkBucketCount] = {0, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, heapWalkMedianUpperUs(vide, kHeapWalkBucketCount),
                                   "Aucun echantillon → 0, pas une duree inventee.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, heapWalkMedianUpperUs(nullptr, kHeapWalkBucketCount),
                                   "Pointeur nul → 0 (robustesse, jamais de dereferencement).");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, heapWalkMedianUpperUs(vide, 0),
                                   "Zero seau → 0.");

  // Rang médian arrondi AU-DESSUS : total impair puis pair.
  uint32_t c1[kHeapWalkBucketCount] = {1, 2, 0, 0, 0, 0, 0};  // total 3, rang 2
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(64u, heapWalkMedianUpperUs(c1, kHeapWalkBucketCount),
                                   "3 echantillons : le rang median est le 2e, dans le seau "
                                   "[32,64) → 64.");
  uint32_t c2[kHeapWalkBucketCount] = {1, 1, 0, 0, 0, 0, 0};  // total 2, rang 1
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(32u, heapWalkMedianUpperUs(c2, kHeapWalkBucketCount),
                                   "2 echantillons : rang 1 (arrondi au-dessus de 1,5 → 2e "
                                   "moitie exclue), donc le premier seau.");

  // ET LA RAISON D'ÊTRE DE LA MÉDIANE : un pic isolé ne doit pas la déplacer.
  // C'est ce qui distingue `walkMedianUs` de `walkMaxUs` — le pire cas est
  // rapporté à côté, pas à la place.
  uint32_t c3[kHeapWalkBucketCount] = {9, 0, 0, 0, 0, 0, 1};  // 9 rapides, 1 hors echelle
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(32u, heapWalkMedianUpperUs(c3, kHeapWalkBucketCount),
                                   "9 parcours rapides et 1 pic hors echelle : la MEDIANE reste "
                                   "dans le premier seau. Si elle suivait le pic, elle ne "
                                   "servirait a rien face a walkMaxUs.");
}

void test_bucket_upper_bounds_are_dyadic_and_ordered(void) {
  const uint32_t attendu[kHeapWalkBucketCount] = {32u, 64u, 128u, 256u, 512u, 1024u, 65535u};
  for (uint8_t i = 0; i < kHeapWalkBucketCount; ++i) {
    char msg[160];
    snprintf(msg, sizeof(msg), "Borne superieure du seau %u : attendu %u us.", (unsigned)i,
             (unsigned)attendu[i]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(attendu[i], kHeapWalkBucketUpperUs[i], msg);
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(7, kHeapWalkBucketCount,
                                  "7 seaux = 28 octets de compteurs. Changer ce nombre change la "
                                  "resolution de la mediane ET l'empreinte RAM.");
}

// ===========================================================================
// T7 — LES LIGNES DE JOURNAL, VÉRIFIÉES PAR CONSTRUCTION
// ===========================================================================
// Ce sont des `char[]` de PILE : un dépassement serait tronqué SILENCIEUSEMENT
// par snprintf, et le relevé perdrait ses derniers champs sans que rien ne le
// signale. Les pires cas ne se prennent donc pas sur parole : on lit le FORMAT
// réellement écrit dans src/main.cpp et on le reconstruit avec des valeurs
// saturées.

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_heap_floors");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i == 0) {
      if (!rooted[0]) continue;
      snprintf(path, sizeof(path), "%s", rooted);
    } else if (i == 1) {
      snprintf(path, sizeof(path), "%s", rel);
    } else {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4000000L) {
      fclose(fp);
      continue;
    }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) {
      fclose(fp);
      continue;
    }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    *outLen = (long)got;
    return buf;
  }
  return NULL;
}

static int countOccurrences(const char* hay, const char* needle) {
  int n = 0;
  const char* p = hay;
  const size_t len = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) {
    ++n;
    p += len;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Depouilleur de commentaires + son AUTO-TEST anti-leurre
// ---------------------------------------------------------------------------
// MOTIF, releve en revue le 08/09/2026 : `test_journal_context_fields_are_bytes`
// lisait la source BRUTE avec un `>= 1`. Elle etait donc satisfiable par une
// simple MENTION en commentaire — le defaut EXACT de bug-114, decouvert la
// veille dans ce meme depot, et reproduit ici par le test qui devait s'en
// premunir. Copie conforme du depouilleur de test_native_logger_logic.
static void stripCppComments(char* s) {
  bool inLine = false, inBlock = false, inStr = false, inChr = false;
  for (size_t i = 0; s[i] != '\0'; ++i) {
    if (inLine) {
      if (s[i] == '\n') inLine = false; else s[i] = ' ';
    } else if (inBlock) {
      if (s[i] == '*' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = false; }
      else if (s[i] != '\n') s[i] = ' ';
    } else if (inStr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '"') inStr = false;
    } else if (inChr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '\'') inChr = false;
    } else if (s[i] == '/' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inLine = true; }
    else if (s[i] == '/' && s[i + 1] == '*') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = true; }
    else if (s[i] == '"') inStr = true;
    else if (s[i] == '\'') inChr = true;
  }
}

// Corps d'une fonction, borne par sa SIGNATURE : une sentinelle qui compte au
// niveau du FICHIER serait satisfaite par une occurrence dans une autre
// fonction. Renvoie NULL si la signature est absente.
static char* extraitCorps(const char* src, const char* signature) {
  const char* p = strstr(src, signature);
  if (p == NULL) return NULL;
  const char* ouvre = strchr(p, '{');
  if (ouvre == NULL) return NULL;
  int profondeur = 0;
  const char* q = ouvre;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++profondeur;
    else if (*q == '}') { --profondeur; if (profondeur == 0) { ++q; break; } }
  }
  const size_t n = (size_t)(q - ouvre);
  char* corps = (char*)malloc(n + 1);
  if (corps == NULL) return NULL;
  memcpy(corps, ouvre, n);
  corps[n] = '\0';
  return corps;
}

static char* readProjectCode(const char* rel) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src != NULL) stripCppComments(src);
  return src;
}

// L'outil est verifie AVANT que les sentinelles s'en servent.
void test_le_depouilleur_desarme_le_leurre(void) {
  char sonde[] =
      "// uint8_t ctxAtMinFree;\n"
      "/* uint8_t ctxSeen; */\n"
      "uint32_t ctxAtMinFree;\n";
  const int brut = countOccurrences(sonde, "uint8_t ctxAtMinFree;");
  stripCppComments(sonde);
  const int code = countOccurrences(sonde, "uint8_t ctxAtMinFree;");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, brut,
      "La sonde doit porter le leurre EN COMMENTAIRE avant depouillement.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, code,
      "APRES depouillement, la mention en commentaire ne doit PLUS compter. Si "
      "cette assertion tombe, le depouilleur est casse et TOUTES les sentinelles "
      "de cette suite deviennent decoratives.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(sonde, "uint32_t ctxAtMinFree;"),
      "Le depouillement ne doit pas toucher au CODE reel.");
  // Preuve que le depouilleur OPERE sur un fichier REEL du projet, et pas
  // seulement sur une sonde fabriquee : apres depouillement, plus AUCUN `//` ne
  // subsiste. Ce jeton est choisi parce qu'il est present a coup sur dans tout
  // fichier commente du depot, contrairement a un nom de champ donne — mon
  // premier essai visait `ctxAtMinFree`, que heap_metrics.h ne cite PAS dans ses
  // commentaires, et l'assertion tombait pour une raison sans rapport avec le
  // depouilleur.
  long n = 0;
  char* reel = readProjectFile("src/heap_metrics.h", &n);
  TEST_ASSERT_NOT_NULL(reel);
  const int brutReel = countOccurrences(reel, "//");
  stripCppComments(reel);
  const int codeReel = countOccurrences(reel, "//");
  free(reel);
  char msg[360];
  snprintf(msg, sizeof(msg),
      "Le depouilleur doit OPERER sur src/heap_metrics.h reel : %d marqueurs "
      "`//` avant, %d apres. Attendu : plus d'un avant, zero apres. A zero des "
      "le depart, le fichier n'a pas de commentaire et la preuve ne vaut rien ; "
      "non nul apres, le depouilleur ne fonctionne pas sur du code reel et "
      "TOUTES les sentinelles de cette suite sont decoratives.",
      brutReel, codeReel);
  TEST_ASSERT_TRUE_MESSAGE(brutReel > 0 && codeReel == 0, msg);
}

// Concatène les littéraux de chaîne d'un `snprintf(<buf>, sizeof(<buf>), "..."
// "...", ...)`. Retourne un buffer alloué (le format ASSEMBLÉ, tel que le voit
// le compilateur), ou NULL.
static char* extractSnprintfFormat(const char* src, const char* bufName) {
  char needle[128];
  snprintf(needle, sizeof(needle), "snprintf(%s,", bufName);
  const char* p = strstr(src, needle);
  if (p == NULL) return NULL;
  const char* q = strchr(p, '"');
  if (q == NULL) return NULL;
  char* out = (char*)malloc(2048);
  if (out == NULL) return NULL;
  size_t n = 0;
  while (q != NULL && *q == '"') {
    ++q;  // entre dans le littéral
    while (*q != '\0' && *q != '"') {
      if (*q == '\\' && q[1] != '\0') {
        // Les seules échappées attendues ici : aucune. On les recopie telles
        // quelles, ce qui SURESTIME d'un caractère — donc du bon côté.
        if (n < 2046) out[n++] = *q;
        ++q;
      }
      if (*q == '\0' || *q == '"') break;
      if (n < 2046) out[n++] = *q;
      ++q;
    }
    if (*q == '"') ++q;
    // Littéral suivant ? (concaténation implicite)
    while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') ++q;
  }
  out[n] = '\0';
  return out;
}

// Taille déclarée d'un `char <name>[NNN];`.
static int extractBufferSize(const char* src, const char* name) {
  char needle[128];
  snprintf(needle, sizeof(needle), "char %s[", name);
  const char* p = strstr(src, needle);
  if (p == NULL) return -1;
  p += strlen(needle);
  return atoi(p);
}

// Longueur PIRE CAS du texte produit par ce format :
//   %u    → 10 caractères (4294967295) ;
//   %02X  → 2 caractères  (FF) — les arguments sont des uint8_t, ce que le test
//           test_journal_context_fields_are_bytes vérifie séparément ;
//   tout autre caractère → 1.
// Renvoie -1 si un spécificateur inattendu apparaît : mieux vaut un test rouge
// qu'un pire cas faux.
static int worstCaseLength(const char* fmt, int* outU, int* outX) {
  int len = 0, nu = 0, nx = 0;
  for (const char* p = fmt; *p != '\0'; ++p) {
    if (*p != '%') {
      ++len;
      continue;
    }
    if (strncmp(p, "%u", 2) == 0) {
      len += 10;
      ++nu;
      p += 1;
    } else if (strncmp(p, "%02X", 4) == 0) {
      len += 2;
      ++nx;
      p += 3;
    } else {
      return -1;
    }
  }
  if (outU) *outU = nu;
  if (outX) *outX = nx;
  return len;
}

void test_new_journal_lines_fit_their_stack_buffers(void) {
  long len = 0;
  char* src = readProjectFile("src/main.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/main.cpp INTROUVABLE.");

  struct Ligne {
    const char* nom;
    int tailleAttendue;   // char[NNN] déclaré
    int pireCasAttendu;   // caractères, hors NUL
    const char* role;
  };
  const Ligne lignes[] = {
      {"ligne2", 160, 142, "planchers DEPUIS LE BOOT + contexte au creux + temoin de bascule PSRAM"},
      {"ligne3", 176, 163, "planchers de FENETRE + contextes vus + duree du parcours"},
  };
  const int n = (int)(sizeof(lignes) / sizeof(lignes[0]));
  for (int i = 0; i < n; ++i) {
    char msg[512];
    const int taille = extractBufferSize(src, lignes[i].nom);
    snprintf(msg, sizeof(msg), "Declaration `char %s[...]` introuvable dans src/main.cpp.",
             lignes[i].nom);
    TEST_ASSERT_TRUE_MESSAGE(taille > 0, msg);
    snprintf(msg, sizeof(msg), "`%s` doit rester declare a %d octets (%s).", lignes[i].nom,
             lignes[i].tailleAttendue, lignes[i].role);
    TEST_ASSERT_EQUAL_INT_MESSAGE(lignes[i].tailleAttendue, taille, msg);

    char* fmt = extractSnprintfFormat(src, lignes[i].nom);
    snprintf(msg, sizeof(msg), "Format du snprintf de `%s` introuvable.", lignes[i].nom);
    TEST_ASSERT_NOT_NULL_MESSAGE(fmt, msg);

    int nu = 0, nx = 0;
    const int pire = worstCaseLength(fmt, &nu, &nx);
    snprintf(msg, sizeof(msg),
             "Specificateur inattendu dans le format de `%s` : le pire cas n'est plus calculable, "
             "donc plus verifiable. Ajouter sa largeur a worstCaseLength() avant de continuer.",
             lignes[i].nom);
    TEST_ASSERT_TRUE_MESSAGE(pire >= 0, msg);

    // (a) LA VÉRIFICATION QUI COMPTE : ça tient, NUL compris.
    snprintf(msg, sizeof(msg),
             "`%s` : pire cas %d caracteres + NUL = %d octets, pour un tampon de %d. snprintf "
             "TRONQUE EN SILENCE : le releve perdrait ses derniers champs (%d valeurs %%u a 10 "
             "chiffres, %d octets de contexte en %%02X) sans qu'aucune alerte ne le dise.",
             lignes[i].nom, pire, pire + 1, taille, nu, nx);
    TEST_ASSERT_TRUE_MESSAGE(pire + 1 <= taille, msg);

    // (b) LE CLIQUET : le pire cas est ÉPINGLÉ. Ajouter un champ sans re-mesurer
    // fait rougir ici — c'est le seul endroit où la marge est vérifiée.
    snprintf(msg, sizeof(msg),
             "`%s` : pire cas mesure %d caracteres, attendu %d. Le commentaire de production "
             "annonce %d OCTETS (NUL compris) ; si un champ a ete ajoute ou retire, re-mesurer et "
             "mettre a jour LES DEUX (ce test et le commentaire).",
             lignes[i].nom, pire, lignes[i].pireCasAttendu, lignes[i].pireCasAttendu + 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(lignes[i].pireCasAttendu, pire, msg);
    free(fmt);
  }
  free(src);
}

void test_the_historic_heap_8bits_line_is_untouched(void) {
  // 152 relevés `Heap 8bits:` dans les journaux capturés (07 et 08/09, deux
  // versions, trois redémarrages) : c'est la SEULE série longue du projet, et
  // toute l'analyse de bug-080 s'y adosse. Son format ne doit pas bouger — ni
  // l'ordre, ni les noms de champs, ni le tampon.
  long len = 0;
  char* src = readProjectFile("src/main.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/main.cpp INTROUVABLE.");

  const char* attendu =
      "Heap 8bits: libre8=%u maxBloc8=%u minBoot=%u minFen=%u ws=%u psram=%u";
  char msg[512];
  snprintf(msg, sizeof(msg),
           "Le format de la ligne « Heap 8bits: » a change. 152 releves historiques en dependent "
           "(la seule serie longue du projet) : tout renommage ou reordonnancement de champ casse "
           "la comparaison avec l'existant. Format attendu, litteral : \"%s\".",
           attendu);
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, attendu) == 1, msg);

  char* fmt = extractSnprintfFormat(src, "ligne");
  TEST_ASSERT_NOT_NULL_MESSAGE(fmt, "Format du snprintf de `ligne` introuvable.");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(attendu, fmt,
                                   "Le premier snprintf sur `ligne` doit rester CELUI de la serie "
                                   "historique.");
  const int taille = extractBufferSize(src, "ligne");
  TEST_ASSERT_EQUAL_INT_MESSAGE(128, taille,
                                "`ligne` doit rester un char[128] : le lot a ajoute DEUX lignes "
                                "separees precisement pour ne pas avoir a recalculer ce pire cas.");
  const int pire = worstCaseLength(fmt, nullptr, nullptr);
  snprintf(msg, sizeof(msg),
           "Pire cas de la ligne historique : %d caracteres (annonce ~117) pour un char[128]. "
           "Elle doit rester en marge SANS avoir ete elargie par ce lot.",
           pire);
  TEST_ASSERT_EQUAL_INT_MESSAGE(117, pire, msg);
  TEST_ASSERT_TRUE_MESSAGE(pire + 1 <= taille, msg);
  free(fmt);
  free(src);
}

// ---------------------------------------------------------------------------
// Les trois correctifs du relevé du 08/09/2026
// ---------------------------------------------------------------------------

// (1) Le tour de rôle ne doit plus etre mesure a CHAQUE echantillon. Condition 5
// du gate pool-chemistry, declenchee par la mesure sur cible : 516 us au pire,
// mediane <= 512, contre les ~84 us supposes et les 300 us de la condition.
// La mesure FINE doit alimenter le plancher PAR MASQUE, pas seulement les deux
// compteurs historiques. Defaut du 08/09/2026, revele par l'experience elle-meme :
// apres 117 requetes provoquees, le journal annoncait minBoot=4084 (ancien
// compteur) et hf.i8.mb annoncait 7924 (plancher par masque) — deux mesures du
// MEME plancher, divergentes de pres du double. Et c'est la seconde qui devait
// dimensionner la reserve DMA de feature-123, le lot irreversible.
// Le regime CERTAIN doit etre pose par l'appelant, pas herite d'un
// echantillonnage vieux de 2 s. Defaut mesure le 08/09/2026 a 23:17 : le
// plancher fin a enregistre un creux de 2 932 o avec ctxAtMin = 0x00 — aucun
// regime — alors que le creux s'est produit en pleine rafale HTTP. Le plancher
// grossier, lui, avait note 0x02. Un plancher sans son regime ne repond pas a la
// question posee.
void test_le_regime_certain_est_pose_par_l_appelant(void) {
  char* code = readProjectCode("src/heap_metrics.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/heap_metrics.cpp introuvable.");
  char* corps = extraitCorps(code, "void heapNoteHttpGetSample(");
  TEST_ASSERT_NOT_NULL_MESSAGE(corps, "heapNoteHttpGetSample introuvable.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "kHeapCtxHttpBurst") >= 1,
      "heapNoteHttpGetSample doit poser kHeapCtxHttpBurst SUR PLACE. Ce site EST "
      "le filtre de la garde 503 : la rafale HTTP y est une certitude, pas une "
      "hypothese. Sans ce bit, le creux est attribue au contexte echantillonne — "
      "vieux de 2 s — et le 08/09 cela a produit un creux de 2 932 o etiquete "
      "« aucun regime ».");
  free(corps);

  char* fin = extraitCorps(code, "void heapNoteMaxAlloc8Sample(");
  free(code);
  TEST_ASSERT_NOT_NULL_MESSAGE(fin, "heapNoteMaxAlloc8Sample introuvable.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(fin, "ctxSurPlace") >= 1,
      "heapNoteMaxAlloc8Sample doit FUSIONNER le regime certain avec le contexte "
      "echantillonne, pas choisir l'un ou l'autre : l'appelant connait la rafale "
      "HTTP, l'echantillonnage connait le dosage et la decouverte MQTT.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(fin, "sFloorCtxSeenBoot.fetch_or") >= 1,
      "Le regime certain doit aussi rejoindre les accumulateurs : sinon un creux "
      "correctement attribue au plancher ne serait pas compte dans ctxb.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(fin, "heapProbeForCaps("),
      "Toujours AUCUN parcours de tas ici : ce chemin s'execute sur async_tcp "
      "pendant la famine (condition 2 du gate pool-chemistry).");
  free(fin);
}

void test_la_mesure_fine_alimente_le_plancher_par_masque(void) {
  char* code = readProjectCode("src/heap_metrics.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/heap_metrics.cpp introuvable.");
  char* corps = extraitCorps(code, "void heapNoteMaxAlloc8Sample(");
  free(code);
  TEST_ASSERT_NOT_NULL_MESSAGE(corps, "heapNoteMaxAlloc8Sample introuvable.");

  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "heapFloorNoteFineLargest(") >= 1,
      "heapNoteMaxAlloc8Sample doit pousser vers le plancher PAR MASQUE via "
      "heapFloorNoteFineLargest. Sans cela, les creux pris AU MOMENT D'UNE "
      "REQUETE (web_server.cpp, fs_guard.cpp) n'alimentent que les compteurs "
      "historiques : le plancher par masque ne voit plus que l'echantillonnage a "
      "2 s et SOUS-ESTIME. Mesure du 08/09 : 4 084 contre 7 924 annonces.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "kHeapFloorSlotInternal8") >= 1,
      "La poussee fine mesure le masque 0x0804 : elle doit viser CE slot. La "
      "diriger vers 0x080C ou 0x1804 attribuerait a ces masques une valeur qui "
      "n'est pas la leur — heaps(0x080C) est un SOUS-ENSEMBLE, donc 0x0804 les "
      "borne superieurement, il ne les mesure pas.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(corps, "heapProbeForCaps("),
      "AUCUN parcours de tas ne doit etre ajoute ici : cette fonction est appelee "
      "depuis async_tcp, pendant la famine. `maxAlloc8` est deja mesure par "
      "l'appelant. Condition 2 du gate pool-chemistry.");
  free(corps);
}

void test_le_tour_de_role_est_divise_pour_tenir_la_condition_5(void) {
  char* code = readProjectCode("src/heap_metrics.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/heap_metrics.cpp introuvable.");
  char* corps = extraitCorps(code, "HeapProbe heapFloorsSample(");
  free(code);
  TEST_ASSERT_NOT_NULL_MESSAGE(corps, "heapFloorsSample introuvable (code depouille).");

  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "kHeapFloorWalkEverySamples") >= 1,
      "heapFloorsSample doit DIVISER la cadence du tour de role par "
      "kHeapFloorWalkEverySamples. Sans ce diviseur, le masque supplementaire est "
      "mesure a chaque echantillon = 1 parcours/s, or la mesure sur cible du "
      "08/09 (516 us au pire, mediane <= 512) a declenche la condition 5 du gate "
      "pool-chemistry, qui prescrit de porter le tour de role a 4 s.");
  // ⚠️ Le TEXTE EXACT de la garde, et non la seule presence du jeton : le
  // 08/09/2026, la premiere version de cette sentinelle a SURVECU au
  // remplacement de `if (walkThisSample)` par `if (true)` — les deux jetons
  // qu'elle comptait restaient presents en amont. Une sentinelle qui survit a
  // l'annulation du correctif qu'elle garde est pire qu'absente.
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "if (walkThisSample)") >= 1,
      "Le parcours du tour de role doit etre garde par `if (walkThisSample)`. Un "
      "heapProbeForCaps inconditionnel — ou un `if (true)` — ramene la cadence a "
      "1 parcours/s et viole le plafond du gate pool-chemistry.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "heapFloorWalkDue(") >= 1
                        && countOccurrences(corps, "heapFloorWalkNextDivider(") >= 1,
      "La decision doit passer par les fonctions PURES heapFloorWalkDue et "
      "heapFloorWalkNextDivider — sinon la regle n'est testable que par son "
      "texte, et l'experience du 08/09 montre que cela ne suffit pas.");
  free(corps);
}

// La REGLE du diviseur, testee comme logique pure : c'est elle qui survit a une
// reecriture du site d'appel.
void test_la_regle_du_diviseur_de_cadence(void) {
  // every = 2 : un echantillon sur deux
  TEST_ASSERT_TRUE_MESSAGE(heapFloorWalkDue(0), "divider 0 => on mesure.");
  TEST_ASSERT_FALSE_MESSAGE(heapFloorWalkDue(1), "divider 1 => on saute.");
  uint8_t d = 0;
  int mesures = 0;
  for (int i = 0; i < 10; ++i) {
    if (heapFloorWalkDue(d)) ++mesures;
    d = heapFloorWalkNextDivider(d, 2);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, mesures,
      "Sur 10 echantillons a every=2, le tour de role doit etre mesure 5 fois — "
      "soit 0,75 parcours/s au total avec la reference. A 10, le diviseur est "
      "inoperant et le plafond du gate est viole.");
  // every = 1 : comportement d'avant le correctif, a ne pas reintroduire
  d = 0; mesures = 0;
  for (int i = 0; i < 10; ++i) {
    if (heapFloorWalkDue(d)) ++mesures;
    d = heapFloorWalkNextDivider(d, 1);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(10, mesures,
      "every=1 doit reproduire l'ancien comportement (mesure a chaque "
      "echantillon) : le test documente ce que le correctif a change.");
  // Replis : diviseur nul, et valeur hors bornes (RAM corrompue)
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, heapFloorWalkNextDivider(0, 0),
      "every=0 ne doit pas diviser par zero.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, heapFloorWalkNextDivider(200, 2),
      "Un diviseur hors bornes doit se replier sur 0 (donc « mesurer »), jamais "
      "se figer sur « sauter » : perdre la mesure serait pire que la refaire.");
}

// (2) ctxSeenBoot ne doit JAMAIS etre rearme : c'est toute sa raison d'etre.
void test_le_contexte_depuis_le_boot_nest_jamais_rearme(void) {
  char* code = readProjectCode("src/heap_metrics.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* corps = extraitCorps(code, "void heapFloorsRearmWindow(");
  free(code);
  TEST_ASSERT_NOT_NULL_MESSAGE(corps, "heapFloorsRearmWindow introuvable.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(corps, "sFloorCtxSeenBoot"),
      "heapFloorsRearmWindow NE DOIT PAS toucher sFloorCtxSeenBoot. Le releve du "
      "08/09 a montre que `ctx` seul, remis a zero toutes les 30 min, se lisait "
      "comme « un seul regime jamais rencontre en 8 h 49 » — faux. Reamer "
      "l'accumulateur depuis-boot reintroduirait exactement ce contresens.");
  free(corps);
}

// (3) Les durees de parcours doivent etre PUBLIEES, pas seulement mesurees.
void test_les_durees_de_parcours_sont_exposees_par_la_route(void) {
  char* code = readProjectCode("src/web_routes_config.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/web_routes_config.cpp introuvable.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(code, "walkMaxUs") >= 1
                        && countOccurrences(code, "walkMedianUs") >= 1,
      "La route doit publier walkMaxUs ET walkMedianUs. Elles etaient MESUREES "
      "des le premier jour mais jamais exposees : le releve du 08/09 n'a pu les "
      "lire que dans le journal, donc la condition 5 du gate pool-chemistry "
      "n'etait satisfaite que par accident.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(code, "ctxSeenBoot") >= 1,
      "La route doit publier ctxSeenBoot a cote de ctxSeen, sinon le contresens "
      "du releve du 08/09 se reproduit a la lecture.");
  free(code);
}

void test_journal_context_fields_are_bytes(void) {
  // Le pire cas de 142 caractères SUPPOSE que les quatre champs imprimés en
  // %02X valent au plus 0xFF. Élargis à 32 bits, ils imprimeraient jusqu'à 8
  // caractères chacun : 142 + 4 × 6 = 166 > 159 → TRONCATURE SILENCIEUSE. Cette
  // sentinelle attache donc le chiffre de T7 aux TYPES qui le garantissent.
  // ⚠️ readProjectCode et NON readProjectFile : la source est DEPOUILLEE de ses
  // commentaires. heap_metrics.h cite ses propres champs dans sa doc, donc une
  // lecture brute etait satisfaite par la MENTION seule — defaut de bug-114,
  // corrige ici le 08/09/2026 sur relevé de revue.
  char* src = readProjectCode("src/heap_metrics.h");
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/heap_metrics.h INTROUVABLE.");
  const char* champs[] = {"uint8_t ctxAtMinFree;", "uint8_t ctxAtMinLargest;", "uint8_t ctxSeen;",
                          "uint8_t witness;"};
  const int n = (int)(sizeof(champs) / sizeof(champs[0]));
  for (int i = 0; i < n; ++i) {
    char msg[448];
    snprintf(msg, sizeof(msg),
             "src/heap_metrics.h doit declarer `%s`. Elargir ce champ a 32 bits porterait le pire "
             "cas de `ligne2` de 143 a 167 octets pour un tampon de 160 : snprintf tronquerait EN "
             "SILENCE, et le releve perdrait le temoin de bascule PSRAM — le champ le plus a droite.",
             champs[i]);
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, champs[i]) >= 1, msg);
  }
  free(src);
}

void test_window_rearm_touches_only_the_window_floors(void) {
  // Sentinelle appariée au test de comportement ci-dessus : le réarmement doit
  // remettre à vide les planchers de FENÊTRE, et JAMAIS ceux depuis le boot.
  // Un réarmement trop large effacerait le pire cas absolu — la grandeur même
  // qui calibrera la réserve DMA de feature-123.
  long len = 0;
  char* src = readProjectFile("src/heap_metrics.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/heap_metrics.cpp INTROUVABLE.");
  const char* p = strstr(src, "void heapFloorsRearmWindow(");
  TEST_ASSERT_NOT_NULL_MESSAGE(p, "heapFloorsRearmWindow introuvable.");
  const char* fin = strstr(p, "\n}");
  TEST_ASSERT_NOT_NULL_MESSAGE(fin, "Corps de heapFloorsRearmWindow illisible.");
  const size_t n = (size_t)(fin - p);
  char* corps = (char*)malloc(n + 1);
  memcpy(corps, p, n);
  corps[n] = '\0';

  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "minFreeWindow.store(kHeapFloorEmpty") >= 1,
                           "Le rearmement doit remettre minFreeWindow a kHeapFloorEmpty.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(corps, "minLargestWindow.store(kHeapFloorEmpty") >= 1,
                           "Le rearmement doit remettre minLargestWindow a kHeapFloorEmpty.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(corps, "minFreeBoot"),
                                "Le rearmement ne doit PAS toucher au plancher depuis-boot : c'est "
                                "le pire cas absolu, il ne se rearme jamais.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(corps, "minLargestBoot"),
                                "Idem pour le plus gros bloc depuis-boot.");
  free(corps);
  free(src);
}

void test_unknown_largest_block_never_enters_a_floor(void) {
  // Les masques à chemin O(régions) (0x1000 et SPIRAM) ne MESURENT pas leur plus
  // gros bloc : la sentinelle kOomProbeBlockUnknown ne doit jamais être repliée
  // dans un plancher, sous peine d'afficher un bloc libre de 4 194 240 o — faux
  // et parfaitement crédible, le mode de défaillance exact que ce dossier
  // combat (parachute en PSRAM, métrique IRAM-inclusive d'ADR-0058).
  long len = 0;
  char* src = readProjectFile("src/heap_metrics.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/heap_metrics.cpp INTROUVABLE.");
  const char* p = strstr(src, "void heapFloorNoteSlot(");
  TEST_ASSERT_NOT_NULL_MESSAGE(p, "heapFloorNoteSlot introuvable.");
  const char* garde = strstr(p, "probe.largestBlockBytes != kOomProbeBlockUnknown");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      garde,
      "heapFloorNoteSlot doit garder l'inscription du « plus gros bloc » derriere "
      "`probe.largestBlockBytes != kOomProbeBlockUnknown`. Sans cette garde, les planchers de "
      "0x1000 et de la PSRAM afficheraient un bloc contigu inexistant.");
  free(src);
}

int main(int, char**) {
  UNITY_BEGIN();
  // T2 — planchers
  RUN_TEST(test_floor_keeps_the_deepest_trough_not_the_last_sample);
  RUN_TEST(test_floor_equal_sample_does_not_rewrite_the_context);
  RUN_TEST(test_floor_accepts_zero_and_keeps_it);
  RUN_TEST(test_window_rearms_while_boot_floor_survives);
  RUN_TEST(test_round_robin_visits_both_walk_masks_alternately);
  RUN_TEST(test_round_robin_is_robust_to_corrupted_cursor_and_empty_ring);
  // T2 — fetch_min à deux écrivains
  RUN_TEST(test_fetch_min_does_not_lose_a_deeper_concurrent_trough);
  RUN_TEST(test_fetch_min_retries_and_wins_when_the_concurrent_trough_is_shallower);
  RUN_TEST(test_fetch_min_is_a_no_op_when_the_sample_is_higher);
  RUN_TEST(test_fetch_min_from_empty_floor_takes_the_first_sample);
  RUN_TEST(test_fetch_min_two_writers_interleaved_over_a_long_series);
  // T2 — contexte (A5)
  RUN_TEST(test_discovery_context_window);
  RUN_TEST(test_context_bits_are_distinct_single_bits);
  // T5 — médiane approchée
  RUN_TEST(test_bucket_index_boundaries);
  RUN_TEST(test_median_of_the_84us_landmark_reports_128);
  RUN_TEST(test_median_bias_is_an_overestimate_bounded_by_bucket_width);
  RUN_TEST(test_median_out_of_scale);
  RUN_TEST(test_median_rank_and_no_sample_cases);
  RUN_TEST(test_bucket_upper_bounds_are_dyadic_and_ordered);
  // T7 — lignes de journal
  RUN_TEST(test_new_journal_lines_fit_their_stack_buffers);
  RUN_TEST(test_the_historic_heap_8bits_line_is_untouched);
  RUN_TEST(test_le_depouilleur_desarme_le_leurre);
  RUN_TEST(test_le_regime_certain_est_pose_par_l_appelant);
  RUN_TEST(test_la_mesure_fine_alimente_le_plancher_par_masque);
  RUN_TEST(test_le_tour_de_role_est_divise_pour_tenir_la_condition_5);
  RUN_TEST(test_la_regle_du_diviseur_de_cadence);
  RUN_TEST(test_le_contexte_depuis_le_boot_nest_jamais_rearme);
  RUN_TEST(test_les_durees_de_parcours_sont_exposees_par_la_route);
  RUN_TEST(test_journal_context_fields_are_bytes);
  // Sentinelles de périmètre
  RUN_TEST(test_window_rearm_touches_only_the_window_floors);
  RUN_TEST(test_unknown_largest_block_never_enters_a_floor);
  return UNITY_END();
}
