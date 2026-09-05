// =============================================================================
// Tests unitaires natifs — Logique pure de l'export chunked /get-history
// (history_export_chunk, bug-heap-toctou A3)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/history_export_chunk.h
// (ADR-0017) :
//   - historyRangeCutoff : borne du filtre ?range=, sémantique EXACTE de
//     HistoryManager::getLastHours (0 = tout renvoyer) ;
//   - formatHistoryPointJson : format JSON octet pour octet identique à
//     l'ancienne concaténation String (arrondis, null sur NaN, virgule de
//     séparation, JAMAIS de JSON partiel si cap trop petit).
// =============================================================================

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "constants.h"  // kHistoryPointJsonMaxBytes — capacité RÉELLE du carry
#include "history_export_chunk.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 1. historyRangeCutoff — sémantique getLastHours
// ---------------------------------------------------------------------------

void test_cutoff_now_epoch_zero_returns_all(void) {
  // Heure indisponible → 0 = tout renvoyer (comme getLastHours avec nowEpoch==0)
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 0));
}

void test_cutoff_now_smaller_than_range_returns_all(void) {
  // now < range (epoch aberrant, ex. uptime provisoire) → 0 = tout renvoyer
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 3600UL));       // 1h < 24h
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 86399UL));      // limite - 1
}

void test_cutoff_boundary_now_equals_range(void) {
  // now == range exactement : pas d'underflow → cutoff 0 (borne incluse)
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(24, 86400UL));
}

void test_cutoff_nominal(void) {
  // Nominal : cutoff = now - hours*3600
  unsigned long now = 1700000000UL;
  TEST_ASSERT_EQUAL_UINT32(1700000000UL - 86400UL,
                           (uint32_t)historyRangeCutoff(24, now));       // 24h
  TEST_ASSERT_EQUAL_UINT32(1700000000UL - 7UL * 86400UL,
                           (uint32_t)historyRangeCutoff(24 * 7, now));   // 7d
}

void test_cutoff_hours_zero_or_negative_returns_all(void) {
  // hours <= 0 = mode "all" (aucun filtre)
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(0, 1700000000UL));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)historyRangeCutoff(-5, 1700000000UL));
}

// ---------------------------------------------------------------------------
// 2. formatHistoryPointJson — contrat octet pour octet
// ---------------------------------------------------------------------------

void test_format_nominal_first(void) {
  char out[192];
  // feature-095 : filtrée pH au centième (7.176 → 7.18), filtrée ORP en entier.
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000000UL,
                                    7.234f, 650.4f, 24.56f,
                                    true, false, 0, /*first=*/true,
                                    7.176f, 648.6f);
  const char* expected =
      "{\"timestamp\":1700000000,\"ph\":7.2,\"phFiltered\":7.18,\"orp\":650,"
      "\"orpFiltered\":649,\"temperature\":24.6,"
      "\"filtration\":true,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_not_first_has_leading_comma(void) {
  char out[192];
  // Agrégat HOURLY : aucune filtrée (feature-095) → clés présentes, valeur null.
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000300UL,
                                    7.05f, 649.6f, 24.0f,
                                    false, true, 1, /*first=*/false);
  // 7.05*10 = 70.5 → roundf = 71 → 7.1 ; 649.6 → 650 ; 24.0 → "24.0"
  const char* expected =
      ",{\"timestamp\":1700000300,\"ph\":7.1,\"phFiltered\":null,\"orp\":650,"
      "\"orpFiltered\":null,\"temperature\":24.0,"
      "\"filtration\":false,\"dosing\":true,\"granularity\":1}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_nan_becomes_null(void) {
  // NaN (mesures exclues, feature-061) → null explicite pour ph/orp/temperature
  char out[192];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000600UL,
                                    NAN, NAN, NAN,
                                    true, false, 2, /*first=*/true,
                                    NAN, NAN);
  const char* expected =
      "{\"timestamp\":1700000600,\"ph\":null,\"phFiltered\":null,\"orp\":null,"
      "\"orpFiltered\":null,\"temperature\":null,"
      "\"filtration\":true,\"dosing\":false,\"granularity\":2}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_infinity_becomes_null(void) {
  // isfinite() couvre aussi ±inf (même garde que l'ancienne route)
  char out[192];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000900UL,
                                    INFINITY, -INFINITY, 24.5f,
                                    false, false, 0, /*first=*/true,
                                    INFINITY, -INFINITY);
  const char* expected =
      "{\"timestamp\":1700000900,\"ph\":null,\"phFiltered\":null,\"orp\":null,"
      "\"orpFiltered\":null,\"temperature\":24.5,"
      "\"filtration\":false,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_rounding_rules(void) {
  // pH/temp : arrondi au dixième (roundf(x*10)/10 puis 1 décimale) ;
  // ORP : entier arrondi au plus proche.
  char out[192];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1UL,
                                    7.249f, 650.5f, 24.94f,
                                    false, false, 0, true,
                                    7.2449f, 650.5f);
  // 7.249*10 = 72.49 → 72 → 7.2 ; 650.5 → 651 (roundf half away) ; 24.94 → 24.9
  // filtrée : 7.2449*100 = 724.49 → 724 → 7.24 (le centième, pas le dixième)
  const char* expected =
      "{\"timestamp\":1,\"ph\":7.2,\"phFiltered\":7.24,\"orp\":651,"
      "\"orpFiltered\":651,\"temperature\":24.9,"
      "\"filtration\":false,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_format_cap_too_small_returns_zero(void) {
  // Le point ne tient pas dans cap → 0, JAMAIS de JSON partiel émis
  char out[32];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1700000000UL,
                                    7.2f, 650.0f, 24.5f,
                                    true, false, 0, true);
  TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_format_cap_zero_or_null_returns_zero(void) {
  char out[8];
  TEST_ASSERT_EQUAL_size_t(0, formatHistoryPointJson(out, 0, 1UL, 7.0f, 650.0f,
                                                     24.0f, false, false, 0, true));
  TEST_ASSERT_EQUAL_size_t(0, formatHistoryPointJson(nullptr, 8, 1UL, 7.0f, 650.0f,
                                                     24.0f, false, false, 0, true));
}

void test_format_fits_in_firmware_carry_capacity(void) {
  // Pire cas réaliste (timestamp 10 chiffres, valeurs négatives, 2 filtrées) :
  // doit tenir dans kHistoryPointJsonMaxBytes=192 (capacité du carry firmware,
  // relevée de 128 par feature-095 — décompte détaillé dans constants.h).
  char out[192];
  size_t n = formatHistoryPointJson(out, sizeof(out), 4294967295UL,
                                    -1.25f, -999.9f, -12.34f,
                                    true, true, 2, /*first=*/false,
                                    -999.99f, -9999.0f);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < 192);
  // Longueur mesurée sur ce jeu : 156 o. Le pire cas absolu du format
  // (163 o : ph/temp à -999.9, granularité 255) reste sous 192.
  TEST_ASSERT_EQUAL_size_t(156, n);
}

// ---------------------------------------------------------------------------
// 3. feature-095 — la lissée sur le fil : contrat de clé, précision, budget
// ---------------------------------------------------------------------------

void test_format_filtered_absent_while_raw_present(void) {
  // LE cas nouveau de feature-095 : le brut existe, la filtrée non (filtre non
  // amorcé au démarrage, ou point relu d'un /history.json écrit par ≤ 2.37).
  // Contrat du fil : la clé RESTE présente et vaut null — jamais omise, jamais
  // repliée sur le brut (un repli afficherait une « filtrée » qui n'a jamais
  // existé, et masquerait un filtre non amorcé).
  char out[kHistoryPointJsonMaxBytes];
  size_t n = formatHistoryPointJson(out, sizeof(out), 1757000000UL,
                                    7.24f, 651.0f, 25.1f,
                                    true, false, 0, /*first=*/true,
                                    NAN, NAN);
  const char* expected =
      "{\"timestamp\":1757000000,\"ph\":7.2,\"phFiltered\":null,\"orp\":651,"
      "\"orpFiltered\":null,\"temperature\":25.1,"
      "\"filtration\":true,\"dosing\":false,\"granularity\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  // La clé est bien là, et sa valeur est null : les deux moitiés du contrat.
  TEST_ASSERT_NOT_NULL(strstr(out, "\"phFiltered\":null"));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"orpFiltered\":null"));
}

void test_format_filtered_keeps_the_centieme(void) {
  // Le dixième du brut masquerait l'essentiel de la dynamique de la régulation :
  // 7.126 et 7.134 sont deux consignes DIFFÉRENTES pour le PID, mais un seul et
  // même « 7.1 ». La filtrée doit donc sortir au centième.
  char out[kHistoryPointJsonMaxBytes];
  formatHistoryPointJson(out, sizeof(out), 1UL, 7.13f, 650.0f, 24.0f,
                         false, false, 0, true, 7.126f, 650.0f);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"phFiltered\":7.13"));
  formatHistoryPointJson(out, sizeof(out), 1UL, 7.13f, 650.0f, 24.0f,
                         false, false, 0, true, 7.134f, 650.0f);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"phFiltered\":7.13"));
  formatHistoryPointJson(out, sizeof(out), 1UL, 7.13f, 650.0f, 24.0f,
                         false, false, 0, true, 7.175f, 650.0f);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"phFiltered\":7.18"));  // pas "7.2"
}

void test_format_absolute_worst_case_fits_in_the_real_carry(void) {
  // PIRE CAS ABSOLU du format, confronté à la constante RÉELLE du firmware
  // (kHistoryPointJsonMaxBytes) et non à un littéral recopié : virgule de tête,
  // timestamp 10 chiffres, ph/temperature au plus long ("-999.9"), filtrée pH
  // au plus long ("-999.99"), ORP et ORP filtrée à 4 chiffres signés ("-9999"),
  // deux drapeaux "false", granularité 255.
  // Si quelqu'un ramène la constante à sa valeur d'avant feature-095 (128), la
  // capacité ne suffit plus : n == 0, le point serait SAUTÉ en production
  // (trou de courbe) — ce test meurt alors.
  char out[kHistoryPointJsonMaxBytes];
  size_t n = formatHistoryPointJson(out, kHistoryPointJsonMaxBytes, 4294967295UL,
                                    -999.9f, -9999.0f, -999.9f,
                                    false, false, 255, /*first=*/false,
                                    -999.99f, -9999.0f);
  TEST_ASSERT_TRUE_MESSAGE(n > 0,
      "pire cas du format au-dessus de kHistoryPointJsonMaxBytes : "
      "les points seraient sautes en production");
  TEST_ASSERT_EQUAL_size_t(163, n);          // décompte de constants.h, mesuré
  TEST_ASSERT_TRUE(n < kHistoryPointJsonMaxBytes);
  // Marge résiduelle documentée (192 - 163) : toute clé ajoutée au format doit
  // refaire ce décompte.
  TEST_ASSERT_EQUAL_size_t(29, kHistoryPointJsonMaxBytes - n);
}

void test_format_cap_boundary_is_exact(void) {
  // Frontière exacte du « jamais de JSON partiel » : snprintf a besoin d'un
  // octet pour le NUL, donc cap == longueur → refus, cap == longueur+1 → écrit.
  char probe[kHistoryPointJsonMaxBytes];
  size_t len = formatHistoryPointJson(probe, sizeof(probe), 1757000000UL,
                                      7.24f, 651.0f, 25.1f, true, false, 0,
                                      true, 7.19f, 650.4f);
  TEST_ASSERT_TRUE(len > 0);

  char tight[kHistoryPointJsonMaxBytes];
  memset(tight, 'X', sizeof(tight));
  TEST_ASSERT_EQUAL_size_t(0, formatHistoryPointJson(tight, len, 1757000000UL,
                                                     7.24f, 651.0f, 25.1f, true,
                                                     false, 0, true, 7.19f, 650.4f));
  TEST_ASSERT_EQUAL_size_t(len, formatHistoryPointJson(tight, len + 1, 1757000000UL,
                                                       7.24f, 651.0f, 25.1f, true,
                                                       false, 0, true, 7.19f, 650.4f));
}

void test_format_worst_case_never_emits_partial_json_at_128(void) {
  // Ce que ferait EXACTEMENT le firmware si la capacité redevenait trop petite :
  // retour 0 et tampon laissé sans JSON exploitable — le point est sauté, mais
  // la réponse reste un JSON valide (invariant de bug-heap-toctou A3).
  char out[128];
  size_t n = formatHistoryPointJson(out, sizeof(out), 4294967295UL,
                                    -999.9f, -9999.0f, -999.9f,
                                    false, false, 255, false, -999.99f, -9999.0f);
  TEST_ASSERT_EQUAL_size_t(0, n);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cutoff_now_epoch_zero_returns_all);
  RUN_TEST(test_cutoff_now_smaller_than_range_returns_all);
  RUN_TEST(test_cutoff_boundary_now_equals_range);
  RUN_TEST(test_cutoff_nominal);
  RUN_TEST(test_cutoff_hours_zero_or_negative_returns_all);
  RUN_TEST(test_format_nominal_first);
  RUN_TEST(test_format_not_first_has_leading_comma);
  RUN_TEST(test_format_nan_becomes_null);
  RUN_TEST(test_format_infinity_becomes_null);
  RUN_TEST(test_format_rounding_rules);
  RUN_TEST(test_format_cap_too_small_returns_zero);
  RUN_TEST(test_format_cap_zero_or_null_returns_zero);
  RUN_TEST(test_format_fits_in_firmware_carry_capacity);
  // feature-095
  RUN_TEST(test_format_filtered_absent_while_raw_present);
  RUN_TEST(test_format_filtered_keeps_the_centieme);
  RUN_TEST(test_format_absolute_worst_case_fits_in_the_real_carry);
  RUN_TEST(test_format_cap_boundary_is_exact);
  RUN_TEST(test_format_worst_case_never_emits_partial_json_at_128);
  return UNITY_END();
}
