// =============================================================================
// Tests unitaires natifs — Synthèse d'une rafale de découverte HA
// (discovery_report_logic, feature-109)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / MQTT / systemLogger.
// On teste le COMPORTEMENT observable du header pur src/discovery_report_logic.h
// (ADR-0017) : la ligne UNIQUE qui remplace 54 lignes INFO.
//
// ENJEU : cette ligne est désormais la SEULE trace d'une rafale. Une ligne
// plausible mais fausse (rang décalé de un, « interrompue » posé au lieu d'être
// calculé, queue de topic tronquée du mauvais côté) est PIRE qu'une ligne
// absente : on la croirait. Toutes les assertions portent donc sur le contenu
// RÉEL d'un char[] pré-empoisonné — jamais sur un String, dont le stub natif
// (test/native_shim/logger_shim.h) ne stocke rien et validerait n'importe quoi.
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "discovery_report_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Outils de test
// ---------------------------------------------------------------------------

// Motif d'empoisonnement : si le formateur n'écrit RIEN (cas d'un String stub),
// le buffer reste plein de 'Z' non terminé et toute assertion de contenu meurt.
static const char kPoison = 'Z';

// Arène instrumentée : le buffer utile est suivi d'une zone canari. Toute
// écriture au-delà de `usable` est détectable — c'est ainsi qu'on prouve que
// `bufLen` est réellement honoré par snprintf et non ignoré.
static const size_t kArenaBytes = 512;
static char gArena[kArenaBytes];

static void arenaPoison(void) { memset(gArena, kPoison, kArenaBytes); }

static bool arenaCanaryIntact(size_t usable) {
  for (size_t i = usable; i < kArenaBytes; i++) {
    if (gArena[i] != kPoison) return false;
  }
  return true;
}

// Le buffer doit être terminé par NUL DANS ses `usable` premiers octets.
static bool arenaNulTerminatedWithin(size_t usable) {
  for (size_t i = 0; i < usable; i++) {
    if (gArena[i] == '\0') return true;
  }
  return false;
}

// Topic réaliste : préfixe identique pour toutes les entités, discriminant en
// queue. C'est exactement la forme produite par mqtt_manager.cpp.
static void makeTopic(char* dst, size_t dstLen, const char* leaf) {
  snprintf(dst, dstLen, "homeassistant/sensor/pool_controller_%s/config", leaf);
}

// Rafale nominale de `n` entités, échecs décrits par un prédicat d'index 1-based.
typedef bool (*FailPredicate)(uint16_t index1);

static void runBurst(DiscoveryTally& t, uint16_t n, FailPredicate failAt) {
  char topic[96];
  for (uint16_t i = 1; i <= n; i++) {
    char leaf[32];
    snprintf(leaf, sizeof(leaf), "entity_%u", (unsigned)i);
    makeTopic(topic, sizeof(topic), leaf);
    const bool ok = (failAt == nullptr) ? true : !failAt(i);
    discoveryTallyRecord(t, topic, ok);
  }
}

static bool failFrom13(uint16_t i) { return i >= 13; }
static bool failSparse(uint16_t i) { return i == 9 || i == 20; }
static bool failAll(uint16_t i) { (void)i; return true; }

// ---------------------------------------------------------------------------
// 1. Rafale parfaite 54/54 — la forme nominale, celle qui sera lue 99 % du temps
// ---------------------------------------------------------------------------

void test_burst_perfect_54_of_54(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 3);
  runBurst(t, 54, nullptr);
  t.durationMs = 1240;

  TEST_ASSERT_EQUAL_UINT16(54, t.attempted);
  TEST_ASSERT_EQUAL_UINT16(54, t.published);
  TEST_ASSERT_EQUAL_UINT16(0, t.failed);
  TEST_ASSERT_EQUAL_UINT16(0, t.firstFailIndex);
  TEST_ASSERT_FALSE(discoveryTallyInterrupted(t));

  arenaPoison();
  const int need = discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);

  // Format verrouillé caractère par caractère : le contenu réel du char[].
  TEST_ASSERT_EQUAL_STRING("Découverte HA #3 : 54/54 entités, 0 échec, 1240 ms", gArena);
  TEST_ASSERT_EQUAL_INT((int)strlen(gArena), need);
  TEST_ASSERT_TRUE(need < (int)kDiscoverySummaryLineMax);  // pas de troncature
  // Aucun vestige du vocabulaire d'échec dans une rafale saine.
  TEST_ASSERT_NULL(strstr(gArena, "interrompue"));
  TEST_ASSERT_NULL(strstr(gArena, "épars"));
}

// La ligne unique doit peser un ordre de grandeur de moins que les 54 lignes
// remplacées (98 o chacune, ≈ 5 292 o mesurés le 06/09).
void test_summary_line_is_two_orders_lighter_than_the_burst(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 4);
  runBurst(t, 54, nullptr);
  t.durationMs = 1240;
  arenaPoison();
  discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  const size_t msg = strlen(gArena);
  TEST_ASSERT_TRUE(msg > 0);
  // Message + préfixe de persistance du logger (19 o d'horodatage, 2 espaces,
  // niveau, saut de ligne) doit rester très en deçà d'une seule ligne × 54.
  TEST_ASSERT_TRUE(msg + 26 < 150);
  TEST_ASSERT_TRUE((msg + 26) * 20 < 54 * 98);  // ≥ 20× plus léger
}

// ---------------------------------------------------------------------------
// 2. Rafale interrompue à la 13e — 12 publiées, 42 échecs CONTIGUS
// ---------------------------------------------------------------------------

void test_burst_interrupted_at_13th(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 4);
  runBurst(t, 54, failFrom13);
  t.durationMs = 310;

  TEST_ASSERT_EQUAL_UINT16(54, t.attempted);
  TEST_ASSERT_EQUAL_UINT16(12, t.published);   // meurt si published++ ignore !ok
  TEST_ASSERT_EQUAL_UINT16(42, t.failed);
  TEST_ASSERT_EQUAL_UINT16(13, t.firstFailIndex);  // rang 1-based du PREMIER
  TEST_ASSERT_TRUE(discoveryTallyInterrupted(t));

  arenaPoison();
  discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_EQUAL_STRING(
      "Découverte HA #4 : 12/54 entités, 42 échecs, interrompue à la 13e "
      "(sistant/sensor/pool_controller_entity_13/config), 310 ms",
      gArena);
  TEST_ASSERT_NULL(strstr(gArena, "épars"));
}

// Le premier topic fautif est celui de la 13e entité, pas celui de la 54e :
// c'est l'entité où le lien est tombé qui a une valeur diagnostique.
void test_interrupted_names_the_first_failing_topic_not_the_last(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 4);
  runBurst(t, 54, failFrom13);
  TEST_ASSERT_NOT_NULL(strstr(t.firstFailTopic, "entity_13/config"));
  TEST_ASSERT_NULL(strstr(t.firstFailTopic, "entity_54"));
  TEST_ASSERT_NULL(strstr(t.firstFailTopic, "entity_14"));
}

// ---------------------------------------------------------------------------
// 3. Échecs ÉPARS non contigus — le prédicat doit être CALCULÉ, pas cosmétique
// ---------------------------------------------------------------------------

void test_burst_sparse_failures_are_not_interruption(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 7);
  runBurst(t, 54, failSparse);  // échecs en 9 et 20, le reste passe
  t.durationMs = 1180;

  TEST_ASSERT_EQUAL_UINT16(54, t.attempted);
  TEST_ASSERT_EQUAL_UINT16(52, t.published);
  TEST_ASSERT_EQUAL_UINT16(2, t.failed);
  TEST_ASSERT_EQUAL_UINT16(9, t.firstFailIndex);
  // failed(2) != attempted(54) - 9 + 1 = 46 → la queue n'est PAS en échec.
  TEST_ASSERT_FALSE(discoveryTallyInterrupted(t));

  arenaPoison();
  discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_EQUAL_STRING(
      "Découverte HA #7 : 52/54 entités, 2 échecs épars, 1er à la 9e "
      "(ssistant/sensor/pool_controller_entity_9/config), 1180 ms",
      gArena);
  // Dire « interrompue » ici serait un diagnostic FAUX et crédible.
  TEST_ASSERT_NULL(strstr(gArena, "interrompue"));
}

// Un échec isolé au tout DERNIER rang est arithmétiquement contigu à la fin :
// c'est bien une interruption (le lien est tombé à la dernière entité).
void test_single_failure_at_last_index_is_interruption(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 8);
  for (uint16_t i = 1; i <= 54; i++) discoveryTallyRecord(t, "t/54", i != 54);
  TEST_ASSERT_EQUAL_UINT16(54, t.firstFailIndex);
  TEST_ASSERT_EQUAL_UINT16(1, t.failed);
  TEST_ASSERT_TRUE(discoveryTallyInterrupted(t));

  arenaPoison();
  discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_NOT_NULL(strstr(gArena, "53/54 entités, 1 échec, interrompue à la 54e"));
}

// Un échec isolé en MILIEU de rafale n'est pas une interruption — le mutant
// `interrupted := failed > 0` meurt ici avec un singulier correct.
void test_single_failure_mid_burst_is_sparse(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 9);
  for (uint16_t i = 1; i <= 54; i++) discoveryTallyRecord(t, "topic/mid", i != 30);
  TEST_ASSERT_FALSE(discoveryTallyInterrupted(t));
  arenaPoison();
  discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_NOT_NULL(strstr(gArena, "1 échec épars, 1er à la 30e"));
  TEST_ASSERT_NULL(strstr(gArena, "échecs"));  // singulier respecté
}

// ---------------------------------------------------------------------------
// 4. Échec TOTAL dès la 1re entité — 0/54, interruption, ordinal « 1re »
// ---------------------------------------------------------------------------

void test_burst_total_failure_from_first_entity(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 11);
  runBurst(t, 54, failAll);
  t.durationMs = 42;

  TEST_ASSERT_EQUAL_UINT16(54, t.attempted);
  TEST_ASSERT_EQUAL_UINT16(0, t.published);
  TEST_ASSERT_EQUAL_UINT16(54, t.failed);
  TEST_ASSERT_EQUAL_UINT16(1, t.firstFailIndex);
  TEST_ASSERT_TRUE(discoveryTallyInterrupted(t));

  arenaPoison();
  discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_NOT_NULL(strstr(gArena, "0/54 entités, 54 échecs, interrompue à la 1re"));
  TEST_ASSERT_NOT_NULL(strstr(gArena, "entity_1/config"));
  // « 1e » serait une faute d'ordinal : on exige « 1re ».
  TEST_ASSERT_EQUAL_STRING("re", discoveryOrdinalSuffix(1));
  TEST_ASSERT_EQUAL_STRING("e", discoveryOrdinalSuffix(2));
  TEST_ASSERT_EQUAL_STRING("e", discoveryOrdinalSuffix(13));
}

// ---------------------------------------------------------------------------
// 5. Rafale VIDE (attempted == 0) — jamais le silence
// ---------------------------------------------------------------------------

void test_empty_tally_produces_a_line_not_silence(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 12);
  TEST_ASSERT_EQUAL_UINT16(0, t.attempted);
  TEST_ASSERT_FALSE(discoveryTallyInterrupted(t));  // 0 échec → pas d'interruption

  arenaPoison();
  const int need = discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_TRUE(need > 0);
  TEST_ASSERT_EQUAL_STRING("Découverte HA #12 : 0/0 entités, 0 échec, 0 ms", gArena);
}

// Le cas « lien MQTT absent » : rafale non TENTÉE, ligne dédiée et explicite.
void test_abort_line_is_emitted_and_names_the_cause(void) {
  arenaPoison();
  const int need = discoveryFormatAbort(gArena, kDiscoverySummaryLineMax, 12);
  TEST_ASSERT_EQUAL_STRING(
      "Découverte HA #12 : abandonnée avant publication (lien MQTT absent)", gArena);
  TEST_ASSERT_EQUAL_INT((int)strlen(gArena), need);
  TEST_ASSERT_TRUE(need < (int)kDiscoverySummaryLineMax);
}

// La ligne d'abandon consomme un numéro de rafale : deux abandons successifs
// ne produisent pas deux lignes identiques.
void test_abort_lines_differ_by_burst_seq(void) {
  char a[kDiscoverySummaryLineMax];
  char b[kDiscoverySummaryLineMax];
  memset(a, kPoison, sizeof(a));
  memset(b, kPoison, sizeof(b));
  discoveryFormatAbort(a, sizeof(a), 12);
  discoveryFormatAbort(b, sizeof(b), 13);
  TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
  TEST_ASSERT_NOT_NULL(strstr(a, "#12 "));
  TEST_ASSERT_NOT_NULL(strstr(b, "#13 "));
}

// ---------------------------------------------------------------------------
// 6. Troncature d'un topic long — la QUEUE est conservée, NUL garanti
// ---------------------------------------------------------------------------

void test_long_topic_keeps_the_tail_and_drops_the_head(void) {
  // 80 caractères exactement, discriminant en fin de chaîne.
  const char* kLong =
      "homeassistant/binary_sensor/pool_ctrl_xxxxxxxxxxxx_orp_sensor_problem/config_v2";
  TEST_ASSERT_EQUAL_INT((int)(79), (int)(strlen(kLong)));

  char dst[kDiscoveryFailTopicMax];
  memset(dst, kPoison, sizeof(dst));
  discoveryCopyTopicTail(dst, sizeof(dst), kLong);

  // Terminaison NUL obligatoire, longueur bornée à dstLen-1.
  TEST_ASSERT_EQUAL_INT((int)(kDiscoveryFailTopicMax - 1), (int)(strlen(dst)));
  TEST_ASSERT_EQUAL_CHAR('\0', dst[kDiscoveryFailTopicMax - 1]);

  // La QUEUE — le discriminant — survit intégralement.
  TEST_ASSERT_NOT_NULL(strstr(dst, "_orp_sensor_problem/config_v2"));
  const char* expectedTail = kLong + (strlen(kLong) - (kDiscoveryFailTopicMax - 1));
  TEST_ASSERT_EQUAL_STRING(expectedTail, dst);

  // La TÊTE — le remplissage commun à toutes les entités — a bien disparu.
  TEST_ASSERT_NULL(strstr(dst, "homeassistant"));
  TEST_ASSERT_NULL(strstr(dst, "binary_sensor"));
}

void test_topic_shorter_than_buffer_is_copied_verbatim(void) {
  char dst[kDiscoveryFailTopicMax];
  memset(dst, kPoison, sizeof(dst));
  discoveryCopyTopicTail(dst, sizeof(dst), "ha/x/config");
  TEST_ASSERT_EQUAL_STRING("ha/x/config", dst);
}

void test_topic_exactly_buffer_minus_one_is_not_truncated(void) {
  char src[kDiscoveryFailTopicMax];
  memset(src, 'a', kDiscoveryFailTopicMax - 1);
  src[kDiscoveryFailTopicMax - 1] = '\0';
  src[0] = 'H';  // marqueur de tête : doit survivre si aucune troncature
  char dst[kDiscoveryFailTopicMax];
  memset(dst, kPoison, sizeof(dst));
  discoveryCopyTopicTail(dst, sizeof(dst), src);
  TEST_ASSERT_EQUAL_STRING(src, dst);
  TEST_ASSERT_EQUAL_CHAR('H', dst[0]);
}

void test_copy_topic_tail_edge_cases(void) {
  char dst[8];
  memset(dst, kPoison, sizeof(dst));
  discoveryCopyTopicTail(dst, sizeof(dst), nullptr);  // nullptr toléré
  TEST_ASSERT_EQUAL_STRING("", dst);

  memset(dst, kPoison, sizeof(dst));
  discoveryCopyTopicTail(dst, sizeof(dst), "");
  TEST_ASSERT_EQUAL_STRING("", dst);

  // dstLen == 0 : ne doit RIEN écrire (pas même un NUL).
  memset(gArena, kPoison, kArenaBytes);
  discoveryCopyTopicTail(gArena, 0, "abcdef");
  TEST_ASSERT_TRUE(arenaCanaryIntact(0));

  // dstLen == 1 : uniquement le NUL.
  memset(gArena, kPoison, kArenaBytes);
  discoveryCopyTopicTail(gArena, 1, "abcdef");
  TEST_ASSERT_EQUAL_CHAR('\0', gArena[0]);
  TEST_ASSERT_TRUE(arenaCanaryIntact(1));

  // Troncature serrée : 3 octets utiles → les 2 DERNIERS caractères.
  memset(gArena, kPoison, kArenaBytes);
  discoveryCopyTopicTail(gArena, 3, "abcdef");
  TEST_ASSERT_EQUAL_STRING("ef", gArena);
  TEST_ASSERT_TRUE(arenaCanaryIntact(3));
}

// Le topic mémorisé par discoveryTallyRecord passe par la même troncature de
// queue : un topic de 80 o ne déborde pas firstFailTopic.
void test_record_truncates_long_topic_by_tail(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 20);
  const char* kLong =
      "homeassistant/binary_sensor/pool_ctrl_xxxxxxxxxxxx_orp_sensor_problem/config_v2";
  discoveryTallyRecord(t, kLong, false);
  TEST_ASSERT_EQUAL_INT((int)(kDiscoveryFailTopicMax - 1), (int)(strlen(t.firstFailTopic)));
  TEST_ASSERT_NOT_NULL(strstr(t.firstFailTopic, "_orp_sensor_problem/config_v2"));
  TEST_ASSERT_NULL(strstr(t.firstFailTopic, "homeassistant"));
}

// ---------------------------------------------------------------------------
// 7. burstSeq distinct entre deux rafales rapprochées
// ---------------------------------------------------------------------------

void test_two_identical_bursts_produce_distinguishable_lines(void) {
  char lineA[kDiscoverySummaryLineMax];
  char lineB[kDiscoverySummaryLineMax];
  memset(lineA, kPoison, sizeof(lineA));
  memset(lineB, kPoison, sizeof(lineB));

  DiscoveryTally a;
  discoveryTallyInit(a, 41);
  runBurst(a, 54, nullptr);
  a.durationMs = 1240;
  discoveryFormatSummary(lineA, sizeof(lineA), a);

  DiscoveryTally b;
  discoveryTallyInit(b, 42);
  runBurst(b, 54, nullptr);
  b.durationMs = 1240;  // rafale rigoureusement identique, sauf le numéro
  discoveryFormatSummary(lineB, sizeof(lineB), b);

  TEST_ASSERT_NOT_EQUAL(0, strcmp(lineA, lineB));
  TEST_ASSERT_NOT_NULL(strstr(lineA, "#41 "));
  TEST_ASSERT_NOT_NULL(strstr(lineB, "#42 "));
}

// Init remet TOUT à zéro : une rafale ne peut pas hériter du topic fautif de la
// précédente (un diagnostic faux et crédible).
void test_init_clears_previous_burst_state(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 1);
  runBurst(t, 10, failAll);
  TEST_ASSERT_TRUE(strlen(t.firstFailTopic) > 0);

  discoveryTallyInit(t, 2);
  TEST_ASSERT_EQUAL_UINT16(0, t.attempted);
  TEST_ASSERT_EQUAL_UINT16(0, t.published);
  TEST_ASSERT_EQUAL_UINT16(0, t.failed);
  TEST_ASSERT_EQUAL_UINT16(0, t.firstFailIndex);
  TEST_ASSERT_EQUAL_UINT32(0, t.durationMs);
  TEST_ASSERT_EQUAL_UINT32(2, t.burstSeq);
  TEST_ASSERT_EQUAL_STRING("", t.firstFailTopic);
}

// ---------------------------------------------------------------------------
// 8. bufLen trop court — écriture BORNÉE, zéro débordement
// ---------------------------------------------------------------------------

void test_short_buffer_bounds_the_write_summary(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 4);
  runBurst(t, 54, failFrom13);
  t.durationMs = 310;

  // Longueur complète de référence (obtenue avec un buffer suffisant).
  char full[kDiscoverySummaryLineMax];
  memset(full, kPoison, sizeof(full));
  const int fullLen = discoveryFormatSummary(full, sizeof(full), t);
  TEST_ASSERT_TRUE(fullLen > 32);

  for (size_t usable = 1; usable <= 40; usable++) {
    arenaPoison();
    const int need = discoveryFormatSummary(gArena, usable, t);
    // Convention snprintf : la valeur rendue est la taille REQUISE, invariante.
    TEST_ASSERT_EQUAL_INT(fullLen, need);
    TEST_ASSERT_TRUE(need >= (int)usable);  // troncature signalée
    // Rien au-delà de `usable` : le canari est intact.
    TEST_ASSERT_TRUE(arenaCanaryIntact(usable));
    TEST_ASSERT_TRUE(arenaNulTerminatedWithin(usable));
    TEST_ASSERT_TRUE(strlen(gArena) <= usable - 1);
    // Le début écrit reste un préfixe EXACT de la ligne complète.
    TEST_ASSERT_EQUAL_INT(0, strncmp(gArena, full, strlen(gArena)));
  }
}

void test_short_buffer_bounds_the_write_abort(void) {
  for (size_t usable = 1; usable <= 40; usable++) {
    arenaPoison();
    const int need = discoveryFormatAbort(gArena, usable, 7);
    TEST_ASSERT_TRUE(need >= (int)usable);
    TEST_ASSERT_TRUE(arenaCanaryIntact(usable));
    TEST_ASSERT_TRUE(arenaNulTerminatedWithin(usable));
  }
}

// bufLen == 0 et buf == nullptr : aucune écriture, retour 0.
void test_zero_length_and_null_buffer_are_no_ops(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 5);
  runBurst(t, 3, nullptr);

  arenaPoison();
  TEST_ASSERT_EQUAL_INT(0, discoveryFormatSummary(gArena, 0, t));
  TEST_ASSERT_TRUE(arenaCanaryIntact(0));

  arenaPoison();
  TEST_ASSERT_EQUAL_INT(0, discoveryFormatAbort(gArena, 0, 5));
  TEST_ASSERT_TRUE(arenaCanaryIntact(0));

  TEST_ASSERT_EQUAL_INT(0, discoveryFormatSummary(nullptr, 64, t));
  TEST_ASSERT_EQUAL_INT(0, discoveryFormatAbort(nullptr, 64, 5));
}

// Le pire cas réaliste (topic tronqué à 47 o, 3 chiffres partout) tient dans
// kDiscoverySummaryLineMax : la ligne livrée n'est jamais tronquée en prod.
void test_worst_case_line_fits_the_production_buffer(void) {
  DiscoveryTally t;
  discoveryTallyInit(t, 4294967295u);
  char longTopic[128];
  memset(longTopic, 'q', sizeof(longTopic) - 1);
  longTopic[sizeof(longTopic) - 1] = '\0';
  for (uint16_t i = 1; i <= 999; i++) discoveryTallyRecord(t, longTopic, i < 500);
  t.durationMs = 4294967295u;

  arenaPoison();
  const int need = discoveryFormatSummary(gArena, kDiscoverySummaryLineMax, t);
  TEST_ASSERT_TRUE(need > 0);
  TEST_ASSERT_TRUE(need < (int)kDiscoverySummaryLineMax);
  TEST_ASSERT_EQUAL_INT((int)strlen(gArena), need);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_burst_perfect_54_of_54);
  RUN_TEST(test_summary_line_is_two_orders_lighter_than_the_burst);
  RUN_TEST(test_burst_interrupted_at_13th);
  RUN_TEST(test_interrupted_names_the_first_failing_topic_not_the_last);
  RUN_TEST(test_burst_sparse_failures_are_not_interruption);
  RUN_TEST(test_single_failure_at_last_index_is_interruption);
  RUN_TEST(test_single_failure_mid_burst_is_sparse);
  RUN_TEST(test_burst_total_failure_from_first_entity);
  RUN_TEST(test_empty_tally_produces_a_line_not_silence);
  RUN_TEST(test_abort_line_is_emitted_and_names_the_cause);
  RUN_TEST(test_abort_lines_differ_by_burst_seq);
  RUN_TEST(test_long_topic_keeps_the_tail_and_drops_the_head);
  RUN_TEST(test_topic_shorter_than_buffer_is_copied_verbatim);
  RUN_TEST(test_topic_exactly_buffer_minus_one_is_not_truncated);
  RUN_TEST(test_copy_topic_tail_edge_cases);
  RUN_TEST(test_record_truncates_long_topic_by_tail);
  RUN_TEST(test_two_identical_bursts_produce_distinguishable_lines);
  RUN_TEST(test_init_clears_previous_burst_state);
  RUN_TEST(test_short_buffer_bounds_the_write_summary);
  RUN_TEST(test_short_buffer_bounds_the_write_abort);
  RUN_TEST(test_zero_length_and_null_buffer_are_no_ops);
  RUN_TEST(test_worst_case_line_fits_the_production_buffer);
  return UNITY_END();
}
