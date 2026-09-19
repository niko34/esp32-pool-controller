// =============================================================================
// Tests unitaires natifs — history_persist_format (feature-131, v2.53.0)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du module pur src/history_persist_format.h
// (ADR-0017) : format EXACT de /history.json (clés courtes, omission des
// non-finis, arrondi dtostrf bit-à-bit).
//
// Sections :
//   (a) ORACLE dtostrf — historyFormatFixed() est un PORTAGE de dtostrf(),
//       pas un snprintf("%.*f") : sweep exhaustif + cas de bascule nommés.
//   (b) golden par point — formatPersistedHistoryPoint(), champ par champ.
//   (c) dépassement de capacité — retour 0, jamais lu par l'appelant.
//   (d)+E4 — document complet, comparaison OCTET À OCTET avec l'ancien format
//       (JsonDocument + String(x,n), 2.52.2) via test/fixtures/history_reference.json,
//       généré par tools/gen_history_reference_json.cpp.
// =============================================================================

#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constants.h"
#include "history_persist_format.h"

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// (a) ORACLE dtostrf
// =============================================================================
// Transcription FIDÈLE de dtostrf() (framework-arduinoespressif32/cores/esp32/
// stdlib_noniso.c), SÉPARÉE du port testé (historyFormatFixed) : deux
// implémentations indépendantes du MÊME algorithme, comparées en égalité de
// chaîne stricte. Si un jour le port de history_persist_format.h se met à
// diverger de dtostrf (ex. mutant : remplacé par snprintf), cette section le
// détecte immédiatement.
static void dtostrfOracle(double number, unsigned int prec, char* out) {
  bool negative = false;
  if (isnan(number)) { strcpy(out, "nan"); return; }
  if (isinf(number)) { strcpy(out, "inf"); return; }
  char* w = out;
  int fillme = static_cast<int>(prec) + 2;  // width = prec + 2 (idiome String(double,prec))
  if (prec > 0) fillme -= static_cast<int>(prec + 1);
  if (number < 0.0) {
    negative = true;
    fillme--;
    number = -number;
  }
  double rounding = 2.0;
  for (unsigned int i = 0; i < prec; ++i) rounding *= 10.0;
  rounding = 1.0 / rounding;
  number += rounding;
  double tenpow = 1.0;
  unsigned int digitcount = 1;
  while (number >= 10.0 * tenpow) {
    tenpow *= 10.0;
    digitcount++;
  }
  number /= tenpow;
  fillme -= static_cast<int>(digitcount);
  while (fillme-- > 0) *w++ = ' ';
  if (negative) *w++ = '-';
  unsigned int totalDigits = digitcount + prec;
  int8_t digit = 0;
  while (totalDigits-- > 0) {
    digit = static_cast<int8_t>(number);
    if (digit > 9) digit = 9;
    *w++ = static_cast<char>('0' | digit);
    if (totalDigits == prec && prec > 0) *w++ = '.';
    number -= digit;
    number *= 10.0;
  }
  *w = '\0';
}

// Compare le port (historyFormatFixed) à l'oracle sur `count` valeurs
// équiréparties [lo, hi] avec un pas de `stepDen` (ex. 1/4096 pour le pH).
// Retourne le nombre de valeurs pour lesquelles port != oracle (attendu : 0).
static int sweepPortVsOracle(long iLo, long iHi, double den, unsigned int prec) {
  int mismatches = 0;
  char port[64], oracle[64];
  for (long i = iLo; i <= iHi; ++i) {
    const double v = static_cast<double>(i) / den;
    const size_t n = historyFormatFixed(port, sizeof(port), v, prec);
    dtostrfOracle(v, prec, oracle);
    if (n == 0 || strcmp(port, oracle) != 0) {
      mismatches++;
    }
  }
  return mismatches;
}

void test_oracle_matches_port_ph_sweep_0_a_14_pas_1_4096(void) {
  // pH 0..14, pas 1/4096 (résolution du convertisseur EZO), 2 décimales.
  const int mismatches = sweepPortVsOracle(0, 14L * 4096L, 4096.0, 2);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches,
      "historyFormatFixed diverge de dtostrf sur le sweep pH — port infidele");
}

void test_oracle_matches_port_temp_sweep_moins55_a_125_pas_0_0625(void) {
  // Température -55..125 °C, pas 0,0625 °C (pas réel DS18B20), 1 décimale.
  const int mismatches = sweepPortVsOracle(-55L * 16L, 125L * 16L, 16.0, 1);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches,
      "historyFormatFixed diverge de dtostrf sur le sweep temperature — port infidele");
}

void test_oracle_matches_port_orp_sweep_pm2000_pas_1_32(void) {
  // ORP -2000..2000 mV, pas 1/32 mV, 1 décimale.
  const int mismatches = sweepPortVsOracle(-2000L * 32L, 2000L * 32L, 32.0, 1);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches,
      "historyFormatFixed diverge de dtostrf sur le sweep ORP — port infidele");
}

// Cas de BASCULE nommés (architecte, feature-131) : le port doit rester
// identique à dtostrf ET, sur ces points précis, DIVERGER de snprintf("%.*f")
// — c'est la preuve chiffrée de la raison d'être du port.
void test_bascule_moins_54_75_temperature_prec1(void) {
  char port[64], oracle[64], naive[64];
  const size_t n = historyFormatFixed(port, sizeof(port), -54.75, 1);
  dtostrfOracle(-54.75, 1, oracle);
  snprintf(naive, sizeof(naive), "%.1f", -54.75);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("-54.7", port);
  TEST_ASSERT_EQUAL_STRING(oracle, port);
  TEST_ASSERT_EQUAL_STRING("-54.8", naive);
  TEST_ASSERT_TRUE_MESSAGE(strcmp(port, naive) != 0,
      "le cas de bascule -54,75 doit diverger entre dtostrf et printf");
}

void test_bascule_24_25_temperature_prec1(void) {
  char port[64], oracle[64], naive[64];
  const size_t n = historyFormatFixed(port, sizeof(port), 24.25, 1);
  dtostrfOracle(24.25, 1, oracle);
  snprintf(naive, sizeof(naive), "%.1f", 24.25);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("24.3", port);
  TEST_ASSERT_EQUAL_STRING(oracle, port);
  TEST_ASSERT_EQUAL_STRING("24.2", naive);
  TEST_ASSERT_TRUE_MESSAGE(strcmp(port, naive) != 0,
      "le cas de bascule 24,25 doit diverger entre dtostrf et printf");
}

void test_bascule_0_125_ph_prec2(void) {
  // À prec=1, 0,125 ne bascule PAS (dtostrf et printf donnent "0.1" tous les
  // deux) — la bascule n'apparaît qu'au CENTIÈME (prec=2, échelle du pH).
  char port[64], oracle[64], naive[64];
  const size_t n = historyFormatFixed(port, sizeof(port), 0.125, 2);
  dtostrfOracle(0.125, 2, oracle);
  snprintf(naive, sizeof(naive), "%.2f", 0.125);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("0.13", port);
  TEST_ASSERT_EQUAL_STRING(oracle, port);
  TEST_ASSERT_EQUAL_STRING("0.12", naive);
  TEST_ASSERT_TRUE_MESSAGE(strcmp(port, naive) != 0,
      "le cas de bascule 0,125 (prec=2) doit diverger entre dtostrf et printf");
}

void test_divergence_dtostrf_vs_printf_sweep_temperature_est_174_sur_2881(void) {
  // Verrouille le chiffre cité par l'architecte (header history_persist_format.h)
  // et par la spec : 174/2881 = 6,0 % de divergence dtostrf vs printf sur le
  // pas réel du DS18B20. Caractérisation, pas un invariant du port lui-même —
  // si ce nombre bouge, c'est que le sweep ou le comportement libc a changé,
  // ce qui mérite d'être vu.
  int diverg = 0;
  char port[64], naive[64];
  const long lo = -55L * 16L, hi = 125L * 16L;
  for (long i = lo; i <= hi; ++i) {
    const double v = static_cast<double>(i) / 16.0;
    historyFormatFixed(port, sizeof(port), v, 1);
    snprintf(naive, sizeof(naive), "%.1f", v);
    if (strcmp(port, naive) != 0) diverg++;
  }
  TEST_ASSERT_EQUAL_INT(2881, static_cast<int>(hi - lo + 1));
  TEST_ASSERT_EQUAL_INT(174, diverg);
}

// -----------------------------------------------------------------------
// historyFormatFixed — dépassement de capacité et garde-fous
// -----------------------------------------------------------------------
void test_fixed_cap_exact_minus_one_returns_zero(void) {
  char full[64];
  const size_t len = historyFormatFixed(full, sizeof(full), -54.75, 1);
  TEST_ASSERT_TRUE(len > 0);
  char tight[64];
  TEST_ASSERT_EQUAL_size_t(0, historyFormatFixed(tight, len, -54.75, 1));
  TEST_ASSERT_EQUAL_size_t(len, historyFormatFixed(tight, len + 1, -54.75, 1));
}

void test_fixed_prec_over_max_returns_zero(void) {
  char out[64];
  TEST_ASSERT_EQUAL_size_t(0, historyFormatFixed(out, sizeof(out), 1.0, kHistoryFormatFixedMaxPrec + 1));
}

void test_fixed_null_or_zero_cap_returns_zero(void) {
  char out[8];
  TEST_ASSERT_EQUAL_size_t(0, historyFormatFixed(out, 0, 1.0, 1));
  TEST_ASSERT_EQUAL_size_t(0, historyFormatFixed(nullptr, 8, 1.0, 1));
}

// =============================================================================
// (b) formatPersistedHistoryPoint — golden par point
// =============================================================================

void test_point_raw_tout_fini_premier_point(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), /*first=*/true,
                                               1757000000UL, 7.23f, 650.4f, 24.6f,
                                               7.18f, 648.6f, /*filtration=*/true,
                                               /*doseMask=*/0, /*granularity=*/0);
  const char* expected =
      "{\"t\":1757000000,\"p\":7.23,\"o\":650.4,\"T\":24.6,\"pf\":7.18,\"of\":648.6,"
      "\"f\":true,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_point_non_premier_a_virgule_de_tete(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), /*first=*/false,
                                               1757000300UL, 7.20f, 651.0f, 24.5f,
                                               NAN, NAN, false, 0, 0);
  const char* expected =
      ",{\"t\":1757000300,\"p\":7.20,\"o\":651.0,\"T\":24.5,\"f\":false,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_point_omission_ph_non_fini(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               NAN, 650.0f, 24.0f, 7.1f, 649.0f,
                                               true, 0, 0);
  const char* expected =
      "{\"t\":1,\"o\":650.0,\"T\":24.0,\"pf\":7.10,\"of\":649.0,\"f\":true,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL(strstr(out, "\"p\":"));
}

void test_point_omission_orp_non_fini(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               7.1f, NAN, 24.0f, 7.1f, 649.0f,
                                               true, 0, 0);
  const char* expected =
      "{\"t\":1,\"p\":7.10,\"T\":24.0,\"pf\":7.10,\"of\":649.0,\"f\":true,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL(strstr(out, "\"o\":"));
}

void test_point_omission_temp_non_finie(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               7.1f, 650.0f, NAN, 7.1f, 649.0f,
                                               true, 0, 0);
  const char* expected =
      "{\"t\":1,\"p\":7.10,\"o\":650.0,\"pf\":7.10,\"of\":649.0,\"f\":true,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL(strstr(out, "\"T\":"));
}

void test_point_omission_pf_non_finie(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               7.1f, 650.0f, 24.0f, NAN, 649.0f,
                                               true, 0, 0);
  const char* expected =
      "{\"t\":1,\"p\":7.10,\"o\":650.0,\"T\":24.0,\"of\":649.0,\"f\":true,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL(strstr(out, "\"pf\":"));
}

void test_point_omission_of_non_finie(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               7.1f, 650.0f, 24.0f, 7.1f, NAN,
                                               true, 0, 0);
  const char* expected =
      "{\"t\":1,\"p\":7.10,\"o\":650.0,\"T\":24.0,\"pf\":7.10,\"f\":true,\"d\":0,\"g\":0}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL(strstr(out, "\"of\":"));
}

void test_point_pf_of_absents_sur_hourly_meme_si_finis(void) {
  // feature-095 : la filtrée n'est PERSISTÉE que sur les points RAW (g=0).
  // Ici phFiltered/orpFiltered SONT finis, mais granularity=1 (HOURLY) : ils
  // doivent rester ABSENTS. Mutant F3 n°3 (émettre pf/of sur HOURLY) : ce
  // test doit mourir si la garde `granularity == RAW` disparaît.
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               7.1f, 650.0f, 24.0f, 7.99f, 999.0f,
                                               true, 1, /*granularity=*/1);
  const char* expected = "{\"t\":1,\"p\":7.10,\"o\":650.0,\"T\":24.0,\"f\":true,\"d\":1,\"g\":1}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL_MESSAGE(strstr(out, "\"pf\":"),
      "pf persiste sur un agregat HOURLY : budget de partition non tenu (feature-095)");
  TEST_ASSERT_NULL_MESSAGE(strstr(out, "\"of\":"),
      "of persiste sur un agregat HOURLY : budget de partition non tenu (feature-095)");
}

void test_point_pf_of_absents_sur_daily_meme_si_finis(void) {
  char out[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                               7.1f, 650.0f, 24.0f, 7.99f, 999.0f,
                                               false, 0, /*granularity=*/2);
  const char* expected = "{\"t\":1,\"p\":7.10,\"o\":650.0,\"T\":24.0,\"f\":false,\"d\":0,\"g\":2}";
  TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_NULL(strstr(out, "\"pf\":"));
  TEST_ASSERT_NULL(strstr(out, "\"of\":"));
}

void test_point_doseMask_sweep_0_a_7(void) {
  // bug-098 : masque, pas booléen — vérifié sur toutes les valeurs 0..7
  // (kHistDosePh | kHistDoseOrp | kHistDoseUnknownChannel).
  for (unsigned int mask = 0; mask <= 7; ++mask) {
    char out[kHistoryPersistPointMaxBytes];
    const size_t n = formatPersistedHistoryPoint(out, sizeof(out), true, 1UL,
                                                 NAN, NAN, NAN, NAN, NAN,
                                                 false, static_cast<uint8_t>(mask), 0);
    char expected[64];
    snprintf(expected, sizeof(expected), "{\"t\":1,\"f\":false,\"d\":%u,\"g\":0}", mask);
    TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
    TEST_ASSERT_EQUAL_STRING(expected, out);
  }
}

void test_point_filtration_true_false(void) {
  char outTrue[kHistoryPersistPointMaxBytes];
  char outFalse[kHistoryPersistPointMaxBytes];
  formatPersistedHistoryPoint(outTrue, sizeof(outTrue), true, 1UL, NAN, NAN, NAN,
                              NAN, NAN, true, 0, 0);
  formatPersistedHistoryPoint(outFalse, sizeof(outFalse), true, 1UL, NAN, NAN, NAN,
                              NAN, NAN, false, 0, 0);
  TEST_ASSERT_NOT_NULL(strstr(outTrue, "\"f\":true"));
  TEST_ASSERT_NULL(strstr(outTrue, "\"f\":false"));
  TEST_ASSERT_NOT_NULL(strstr(outFalse, "\"f\":false"));
}

void test_point_ordre_des_cles_t_p_o_T_pf_of_f_d_g(void) {
  char out[kHistoryPersistPointMaxBytes];
  formatPersistedHistoryPoint(out, sizeof(out), true, 1UL, 7.1f, 650.0f, 24.0f,
                              7.1f, 649.0f, true, 3, 0);
  const char* keys[] = {"\"t\":", "\"p\":", "\"o\":", "\"T\":", "\"pf\":",
                        "\"of\":", "\"f\":", "\"d\":", "\"g\":"};
  const char* prevPos = out;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const char* pos = strstr(prevPos, keys[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(pos, "cle attendue absente — ordre invalide");
    prevPos = pos + strlen(keys[i]);
  }
}

// =============================================================================
// (c) formatPersistedHistoryPoint — dépassement de capacité
// =============================================================================

void test_point_cap_trop_petit_retourne_zero_jamais_lu(void) {
  char full[kHistoryPersistPointMaxBytes];
  const size_t fullLen = formatPersistedHistoryPoint(full, sizeof(full), true, 1757000000UL,
                                                      7.23f, 650.4f, 24.6f, 7.18f, 648.6f,
                                                      true, 7, 0);
  TEST_ASSERT_TRUE(fullLen > 0);
  // À chaque capacité insuffisante (0 .. fullLen), retour 0 SANS EXCEPTION —
  // l'appelant (emitPersistedJson) ne doit JAMAIS pouvoir lire une longueur
  // non nulle pour un buffer qui ne tient pas le point entier.
  for (size_t cap = 0; cap <= fullLen; ++cap) {
    char tight[kHistoryPersistPointMaxBytes];
    memset(tight, 0xAA, sizeof(tight));
    const size_t n = formatPersistedHistoryPoint(tight, cap, true, 1757000000UL,
                                                 7.23f, 650.4f, 24.6f, 7.18f, 648.6f,
                                                 true, 7, 0);
    TEST_ASSERT_EQUAL_size_t(0, n);
  }
  // Et la capacité exacte (fullLen + 1, pour le NUL) réussit.
  char exact[kHistoryPersistPointMaxBytes];
  const size_t n = formatPersistedHistoryPoint(exact, fullLen + 1, true, 1757000000UL,
                                               7.23f, 650.4f, 24.6f, 7.18f, 648.6f,
                                               true, 7, 0);
  TEST_ASSERT_EQUAL_size_t(fullLen, n);
}

void test_point_null_ou_cap_zero_retourne_zero(void) {
  char out[8];
  TEST_ASSERT_EQUAL_size_t(0, formatPersistedHistoryPoint(out, 0, true, 1UL, NAN, NAN,
                                                          NAN, NAN, NAN, false, 0, 0));
  TEST_ASSERT_EQUAL_size_t(0, formatPersistedHistoryPoint(nullptr, 8, true, 1UL, NAN, NAN,
                                                          NAN, NAN, NAN, false, 0, 0));
}

// =============================================================================
// (d) + E4 — document complet, comparaison OCTET À OCTET avec l'ancien format
// =============================================================================
// Jeu de points IDENTIQUE à tools/gen_history_reference_json.cpp (kPoints) —
// toute modification de l'un doit se répercuter sur l'autre, sinon ce test ne
// compare plus rien de significatif. La fixture test/fixtures/history_reference.json
// a été produite par ce générateur avec le code de HistoryManager::saveToFile()
// TEL QU'IL ÉTAIT AVANT feature-131 (JsonDocument + serialized(String(x,n))) —
// c'est le critère d'acceptation n°2 de la spec.

struct TestPoint {
  unsigned long ts;
  float ph, orp, temp, phFiltered, orpFiltered;
  bool filtration;
  uint8_t doseMask;
  uint8_t granularity;
};

static const TestPoint kPoints[] = {
    {1757000000UL, 7.23f, 650.4f, 24.6f, 7.18f, 648.6f, true, 0, 0},
    {1757000300UL, NAN, 651.0f, 24.7f, NAN, 649.0f, true, 1, 0},
    {1757000600UL, 7.20f, NAN, 24.5f, 7.19f, NAN, true, 2, 0},
    {1757000900UL, 7.21f, 652.0f, NAN, 7.20f, 650.0f, false, 4, 0},
    {1757001200UL, 7.22f, 649.5f, 24.4f, NAN, NAN, true, 3, 0},
    {1757001500UL, 7.19f, 650.9f, 24.3f, 7.17f, 649.1f, true, 7, 0},
    {1757000000UL, 7.21f, 651.3f, 24.4f, 7.99f, 999.0f, true, 1, 1},
    {1756900000UL, 7.25f, 649.8f, 23.9f, 7.11f, 111.0f, false, 0, 2},
    {1756800000UL, 6.98f, -12.3f, -1.25f, 6.95f, -10.0f, false, 5, 0},
};
static const size_t kPointsCount = sizeof(kPoints) / sizeof(kPoints[0]);

// Reproduit la structure de HistoryManager::emitPersistedJson() (prefixe +
// points + suffixe, drapeau `first`) — la boucle elle-même n'est PAS
// natively testable (méthode de HistoryManager, qui tire LittleFS/FreeRTOS),
// mais elle est SI SIMPLE (cf. history.cpp) que la reproduire ici teste
// réellement les fonctions PURES sous-jacentes (kHistoryPersistedPrefix/
// Suffix, formatPersistedHistoryPoint) dans leur contexte d'usage réel.
static size_t emitDocument(char* out, size_t cap) {
  size_t pos = 0;
  auto append = [&](const char* s, size_t n) -> bool {
    if (pos + n + 1 > cap) return false;
    memcpy(out + pos, s, n);
    pos += n;
    return true;
  };
  if (!append(kHistoryPersistedPrefix, sizeof(kHistoryPersistedPrefix) - 1)) return 0;
  bool first = true;
  for (size_t i = 0; i < kPointsCount; ++i) {
    const TestPoint& p = kPoints[i];
    char buf[kHistoryPersistPointMaxBytes];
    const size_t n = formatPersistedHistoryPoint(buf, sizeof(buf), first, p.ts, p.ph,
                                                 p.orp, p.temp, p.phFiltered, p.orpFiltered,
                                                 p.filtration, p.doseMask, p.granularity);
    if (n == 0) continue;
    if (!append(buf, n)) return 0;
    first = false;
  }
  if (!append(kHistoryPersistedSuffix, sizeof(kHistoryPersistedSuffix) - 1)) return 0;
  out[pos] = '\0';
  return pos;
}

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  const char* prefixes[4] = {"", "../", "../../", "../../../"};
  for (int i = 0; i < 4; ++i) {
    snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4000000L) { fclose(fp); continue; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(fp); continue; }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    if (outLen) *outLen = (long)got;
    return buf;
  }
  return NULL;
}

void test_e4_document_complet_octet_a_octet_identique_a_l_ancien_format(void) {
  char out[4096];
  const size_t n = emitDocument(out, sizeof(out));
  TEST_ASSERT_TRUE_MESSAGE(n > 0, "emitDocument a echoue (buffer trop petit ?)");

  long refLen = 0;
  char* ref = readProjectFile("test/fixtures/history_reference.json", &refLen);
  TEST_ASSERT_NOT_NULL_MESSAGE(ref,
      "test/fixtures/history_reference.json introuvable — regenerer avec "
      "tools/gen_history_reference_json.cpp");

  TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)refLen, n,
      "longueur differente entre le nouveau format (flux) et l'ancien (JsonDocument) "
      "— divergence bloquante, critere d'acceptation n2 de feature-131");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(ref, out,
      "contenu different octet a octet entre le nouveau format et l'ancien "
      "(2.52.2, JsonDocument + String(x,n)) — divergence bloquante");
  free(ref);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // (a) oracle dtostrf
  RUN_TEST(test_oracle_matches_port_ph_sweep_0_a_14_pas_1_4096);
  RUN_TEST(test_oracle_matches_port_temp_sweep_moins55_a_125_pas_0_0625);
  RUN_TEST(test_oracle_matches_port_orp_sweep_pm2000_pas_1_32);
  RUN_TEST(test_bascule_moins_54_75_temperature_prec1);
  RUN_TEST(test_bascule_24_25_temperature_prec1);
  RUN_TEST(test_bascule_0_125_ph_prec2);
  RUN_TEST(test_divergence_dtostrf_vs_printf_sweep_temperature_est_174_sur_2881);
  RUN_TEST(test_fixed_cap_exact_minus_one_returns_zero);
  RUN_TEST(test_fixed_prec_over_max_returns_zero);
  RUN_TEST(test_fixed_null_or_zero_cap_returns_zero);

  // (b) golden par point
  RUN_TEST(test_point_raw_tout_fini_premier_point);
  RUN_TEST(test_point_non_premier_a_virgule_de_tete);
  RUN_TEST(test_point_omission_ph_non_fini);
  RUN_TEST(test_point_omission_orp_non_fini);
  RUN_TEST(test_point_omission_temp_non_finie);
  RUN_TEST(test_point_omission_pf_non_finie);
  RUN_TEST(test_point_omission_of_non_finie);
  RUN_TEST(test_point_pf_of_absents_sur_hourly_meme_si_finis);
  RUN_TEST(test_point_pf_of_absents_sur_daily_meme_si_finis);
  RUN_TEST(test_point_doseMask_sweep_0_a_7);
  RUN_TEST(test_point_filtration_true_false);
  RUN_TEST(test_point_ordre_des_cles_t_p_o_T_pf_of_f_d_g);

  // (c) dépassement de capacité
  RUN_TEST(test_point_cap_trop_petit_retourne_zero_jamais_lu);
  RUN_TEST(test_point_null_ou_cap_zero_retourne_zero);

  // (d) + E4 — document complet
  RUN_TEST(test_e4_document_complet_octet_a_octet_identique_a_l_ancien_format);

  return UNITY_END();
}
