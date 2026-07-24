// =============================================================================
// Tests unitaires natifs — Admission de page de GET /get-logs
// (logs_page, v2.10.4, bug-crash-getlogs-badalloc-audit-routes)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de l'admission PURE header-only
// src/logs_page.h (ADR-0017) :
//   - logsPageAdmit(included, ts, lastIncludedTs, softLimit, hardCap)
//       → {include, stop}
//
// Règles verrouillées (source : en-tête du module, ordre strict) :
//   1. included < softLimit            → include, continue ;
//   2. included >= hardCap             → stop (coupe même une queue même-ts) ;
//   3. softLimit <= included < hardCap → include ssi ts == lastIncludedTs
//      (queue même-milliseconde : le curseur ?since= est STRICTEMENT supérieur,
//      exclure ces entrées les perdrait entre deux pages) ; sinon stop.
//
// Cas dégénéré assumé (documenté dans le module) : > hardCap entrées dans la
// même ms → coupe quand même (règle 2 prime sur la queue même-ts).
// Valeurs firmware : kLogsMaxEntriesPerPage=30 (soft), kLogsPageHardCap=60.
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include "logs_page.h"

void setUp(void) {}
void tearDown(void) {}

static constexpr size_t kSoft = 30;
static constexpr size_t kHard = 60;

// ---------------------------------------------------------------------------
// 1. Règle 1 — sous la limite souple : toujours inclus
// ---------------------------------------------------------------------------

void test_first_entry_included(void) {
  // lastIncludedTs indéfini quand included==0 : la règle 1 prime, jamais consulté.
  LogsPageDecision d = logsPageAdmit(0, 1000UL, 0UL, kSoft, kHard);
  TEST_ASSERT_TRUE(d.include);
  TEST_ASSERT_FALSE(d.stop);
}

void test_below_soft_limit_included_regardless_of_ts(void) {
  LogsPageDecision d = logsPageAdmit(kSoft - 1, 5000UL, 1000UL, kSoft, kHard);
  TEST_ASSERT_TRUE(d.include);
  TEST_ASSERT_FALSE(d.stop);
}

// ---------------------------------------------------------------------------
// 2. Règle 3 — entre soft et hard : queue même-timestamp incluse, sinon stop
// ---------------------------------------------------------------------------

void test_at_soft_limit_same_ts_tail_included(void) {
  LogsPageDecision d = logsPageAdmit(kSoft, 1000UL, 1000UL, kSoft, kHard);
  TEST_ASSERT_TRUE(d.include);
  TEST_ASSERT_FALSE(d.stop);
}

void test_at_soft_limit_new_ts_stops(void) {
  LogsPageDecision d = logsPageAdmit(kSoft, 1001UL, 1000UL, kSoft, kHard);
  TEST_ASSERT_FALSE(d.include);
  TEST_ASSERT_TRUE(d.stop);
}

void test_tail_keeps_including_while_same_ts(void) {
  // Toute la zone [soft, hard) accepte la queue même-ts.
  for (size_t inc = kSoft; inc < kHard; ++inc) {
    LogsPageDecision d = logsPageAdmit(inc, 42UL, 42UL, kSoft, kHard);
    TEST_ASSERT_TRUE(d.include);
    TEST_ASSERT_FALSE(d.stop);
  }
}

void test_tail_ts_lower_than_last_also_stops(void) {
  // ts différent (même inférieur — ne devrait pas arriver, ring chronologique)
  // → stop : seule l'ÉGALITÉ stricte prolonge la page.
  LogsPageDecision d = logsPageAdmit(kSoft, 999UL, 1000UL, kSoft, kHard);
  TEST_ASSERT_FALSE(d.include);
  TEST_ASSERT_TRUE(d.stop);
}

// ---------------------------------------------------------------------------
// 3. Règle 2 — hard cap : coupe inconditionnelle
// ---------------------------------------------------------------------------

void test_hard_cap_stops_even_same_ts_tail(void) {
  LogsPageDecision d = logsPageAdmit(kHard, 1000UL, 1000UL, kSoft, kHard);
  TEST_ASSERT_FALSE(d.include);
  TEST_ASSERT_TRUE(d.stop);
}

void test_beyond_hard_cap_stops(void) {
  LogsPageDecision d = logsPageAdmit(kHard + 5, 1000UL, 1000UL, kSoft, kHard);
  TEST_ASSERT_FALSE(d.include);
  TEST_ASSERT_TRUE(d.stop);
}

// ---------------------------------------------------------------------------
// 4. Simulation de pagination complète (invariant appelant)
// ---------------------------------------------------------------------------

void test_walk_nominal_distinct_timestamps(void) {
  // 200 entrées à timestamps distincts → pages de exactement kSoft entrées.
  size_t included = 0;
  unsigned long lastTs = 0;
  size_t i = 0;
  for (; i < 200; ++i) {
    unsigned long ts = 1000UL + i;
    LogsPageDecision d = logsPageAdmit(included, ts, lastTs, kSoft, kHard);
    if (d.stop) break;
    if (d.include) { included++; lastTs = ts; }
  }
  TEST_ASSERT_EQUAL_size_t(kSoft, included);
  TEST_ASSERT_EQUAL_size_t(kSoft, i);  // arrêt exactement à la 31e candidate
}

void test_walk_same_ms_burst_at_boundary(void) {
  // 29 entrées distinctes puis une rafale de 10 entrées dans la même ms :
  // la page inclut 29 + 10 = 39 entrées (la rafale à cheval sur la limite
  // souple est embarquée entière — aucune perte pour le curseur).
  size_t included = 0;
  unsigned long lastTs = 0;
  for (size_t i = 0; i < 29; ++i) {
    LogsPageDecision d = logsPageAdmit(included, 1000UL + i, lastTs, kSoft, kHard);
    TEST_ASSERT_TRUE(d.include);
    included++; lastTs = 1000UL + i;
  }
  for (size_t i = 0; i < 10; ++i) {
    LogsPageDecision d = logsPageAdmit(included, 5000UL, lastTs, kSoft, kHard);
    TEST_ASSERT_TRUE(d.include);
    included++; lastTs = 5000UL;
  }
  TEST_ASSERT_EQUAL_size_t(39, included);
  // L'entrée suivante (ts différent) ferme la page.
  LogsPageDecision d = logsPageAdmit(included, 5001UL, lastTs, kSoft, kHard);
  TEST_ASSERT_TRUE(d.stop);
}

void test_walk_degenerate_all_same_ms_cut_at_hard_cap(void) {
  // 100 entrées dans la même ms : coupe à kHard exactement (cas dégénéré assumé).
  size_t included = 0;
  unsigned long lastTs = 0;
  for (size_t i = 0; i < 100; ++i) {
    LogsPageDecision d = logsPageAdmit(included, 7777UL, lastTs, kSoft, kHard);
    if (d.stop) break;
    if (d.include) { included++; lastTs = 7777UL; }
  }
  TEST_ASSERT_EQUAL_size_t(kHard, included);
}

// ---------------------------------------------------------------------------
// 5. Cas dégénérés de configuration
// ---------------------------------------------------------------------------

void test_soft_zero_first_entry_consults_rule3(void) {
  // softLimit=0 : la règle 1 ne s'applique jamais ; included=0 avec
  // lastIncludedTs=0 et ts==0 → include (égalité) ; ts!=0 → stop.
  LogsPageDecision same = logsPageAdmit(0, 0UL, 0UL, 0, kHard);
  TEST_ASSERT_TRUE(same.include);
  LogsPageDecision diff = logsPageAdmit(0, 1UL, 0UL, 0, kHard);
  TEST_ASSERT_TRUE(diff.stop);
}

void test_hard_equals_soft_no_tail(void) {
  // hardCap == softLimit : aucune queue même-ts possible (règle 2 prime dès soft).
  LogsPageDecision d = logsPageAdmit(kSoft, 1000UL, 1000UL, kSoft, kSoft);
  TEST_ASSERT_FALSE(d.include);
  TEST_ASSERT_TRUE(d.stop);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  // 1. Règle 1
  RUN_TEST(test_first_entry_included);
  RUN_TEST(test_below_soft_limit_included_regardless_of_ts);
  // 2. Règle 3 (queue même-ts)
  RUN_TEST(test_at_soft_limit_same_ts_tail_included);
  RUN_TEST(test_at_soft_limit_new_ts_stops);
  RUN_TEST(test_tail_keeps_including_while_same_ts);
  RUN_TEST(test_tail_ts_lower_than_last_also_stops);
  // 3. Règle 2 (hard cap)
  RUN_TEST(test_hard_cap_stops_even_same_ts_tail);
  RUN_TEST(test_beyond_hard_cap_stops);
  // 4. Simulations de pagination
  RUN_TEST(test_walk_nominal_distinct_timestamps);
  RUN_TEST(test_walk_same_ms_burst_at_boundary);
  RUN_TEST(test_walk_degenerate_all_same_ms_cut_at_hard_cap);
  // 5. Configurations dégénérées
  RUN_TEST(test_soft_zero_first_entry_consults_rule3);
  RUN_TEST(test_hard_equals_soft_no_tail);
  return UNITY_END();
}
