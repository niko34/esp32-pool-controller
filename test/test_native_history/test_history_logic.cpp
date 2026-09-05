// =============================================================================
// Tests unitaires natifs — history_logic (feature-041)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// On teste le COMPORTEMENT observable du module pur d'agrégation d'historique :
//   - bucketTimestamp (AC1, frontières strictes + garde bucketSeconds==0)
//   - isOlderThan     (AC2, frontière stricte + wrap uint32)
//   - finalizeMean    (AC3, count==0 → NaN)
//   - isMajority      (AC4, division entière stricte)
//   - anyTrue         (AC4)
//   - historySaveAdmit (AC5, bug-nospc-partition-history : garde d'espace,
//     frontière large, anti-overflow SIZE_MAX)
// via l'API publique, pas l'implémentation interne.
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdint.h>  // SIZE_MAX
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constants.h"  // kHistorySaveSpaceMarginBytes, kMax*DataPoints
#include "history_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// AC1 — bucketTimestamp
// -----------------------------------------------------------------------------
void test_bucketTimestamp_hourly_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(7200, bucketTimestamp(7200, 3600));  // frontière exacte
}
void test_bucketTimestamp_hourly_above_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(7200, bucketTimestamp(7201, 3600));
}
void test_bucketTimestamp_hourly_below_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(3600, bucketTimestamp(7199, 3600));
}
void test_bucketTimestamp_hourly_zero_ts(void) {
  TEST_ASSERT_EQUAL_UINT32(0, bucketTimestamp(0, 3600));
}
void test_bucketTimestamp_daily_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(86400, bucketTimestamp(86400, 86400));
}
void test_bucketTimestamp_daily_above_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(86400, bucketTimestamp(90000, 86400));
}
void test_bucketTimestamp_daily_below_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(0, bucketTimestamp(86399, 86400));
}
void test_bucketTimestamp_zero_bucket_guard(void) {
  TEST_ASSERT_EQUAL_UINT32(12345, bucketTimestamp(12345, 0));  // garde /0 → ts
}

// -----------------------------------------------------------------------------
// AC2 — isOlderThan (frontière STRICTE + wrap uint32)
// -----------------------------------------------------------------------------
void test_isOlderThan_age_zero(void) {
  TEST_ASSERT_FALSE(isOlderThan(10000, 10000, 3600));  // age 0
}
void test_isOlderThan_age_equals_max(void) {
  TEST_ASSERT_FALSE(isOlderThan(13600, 10000, 3600));  // 3600 > 3600 faux
}
void test_isOlderThan_age_max_plus_one(void) {
  TEST_ASSERT_TRUE(isOlderThan(13601, 10000, 3600));   // 3601 > 3600 vrai
}
void test_isOlderThan_uint32_wrap_within(void) {
  // (now - ts) = 50 - 0xFFFFFFF0 wrap = 66 ≤ 100 → false
  TEST_ASSERT_FALSE(isOlderThan(50, 0xFFFFFFF0u, 100));
}
void test_isOlderThan_uint32_wrap_exceeds(void) {
  // 66 > 10 → true
  TEST_ASSERT_TRUE(isOlderThan(50, 0xFFFFFFF0u, 10));
}

// -----------------------------------------------------------------------------
// AC3 — finalizeMean (count==0 → NaN)
// -----------------------------------------------------------------------------
void test_finalizeMean_zero_count_is_nan(void) {
  TEST_ASSERT_TRUE(isnan(finalizeMean(42.0f, 0)));
}
void test_finalizeMean_integer_result(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.0f, finalizeMean(21.0f, 3));
}
void test_finalizeMean_fractional_result(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.35f, finalizeMean(14.7f, 2));
}

// -----------------------------------------------------------------------------
// AC4 — isMajority (division ENTIÈRE stricte) + anyTrue
// -----------------------------------------------------------------------------
void test_isMajority_2_of_4_false(void) {
  TEST_ASSERT_FALSE(isMajority(2, 4));  // 2 > 2 faux
}
void test_isMajority_3_of_4_true(void) {
  TEST_ASSERT_TRUE(isMajority(3, 4));
}
void test_isMajority_3_of_5_true(void) {
  TEST_ASSERT_TRUE(isMajority(3, 5));   // 3 > 2
}
void test_isMajority_0_of_0_false(void) {
  TEST_ASSERT_FALSE(isMajority(0, 0));
}
void test_isMajority_1_of_1_true(void) {
  TEST_ASSERT_TRUE(isMajority(1, 1));   // 1 > 0
}
void test_isMajority_1_of_2_false(void) {
  TEST_ASSERT_FALSE(isMajority(1, 2));  // 1 > 1 faux
}
void test_anyTrue_zero_false(void) {
  TEST_ASSERT_FALSE(anyTrue(0));
}
void test_anyTrue_one_true(void) {
  TEST_ASSERT_TRUE(anyTrue(1));
}
void test_anyTrue_five_true(void) {
  TEST_ASSERT_TRUE(anyTrue(5));
}

// -----------------------------------------------------------------------------
// AC5 — historySaveAdmit (bug-nospc-partition-history)
// Sémantique : free + old >= json + marge → admis (frontière LARGE).
// -----------------------------------------------------------------------------
void test_saveAdmit_exact_boundary_admitted(void) {
  // 10000 + 20000 == 22000 + 8000 → admis (égalité exacte)
  TEST_ASSERT_TRUE(historySaveAdmit(10000, 20000, 22000, 8000));
}
void test_saveAdmit_one_byte_short_refused(void) {
  // 10000 + 20000 < 22001 + 8000 → refusé à 1 octet près
  TEST_ASSERT_FALSE(historySaveAdmit(10000, 20000, 22001, 8000));
}
void test_saveAdmit_one_byte_spare_admitted(void) {
  // 10001 + 20000 > 22000 + 8000 → admis
  TEST_ASSERT_TRUE(historySaveAdmit(10001, 20000, 22000, 8000));
}
void test_saveAdmit_no_old_file_admitted(void) {
  // Ancien fichier absent (old=0) : le libre seul doit couvrir json + marge
  TEST_ASSERT_TRUE(historySaveAdmit(30000, 0, 22000, 8000));
}
void test_saveAdmit_no_old_file_refused(void) {
  TEST_ASSERT_FALSE(historySaveAdmit(29999, 0, 22000, 8000));
}
void test_saveAdmit_zero_margin(void) {
  // Marge 0 : json seul décide (frontière large)
  TEST_ASSERT_TRUE(historySaveAdmit(22000, 0, 22000, 0));
  TEST_ASSERT_FALSE(historySaveAdmit(21999, 0, 22000, 0));
}
void test_saveAdmit_all_zero_admitted(void) {
  // Rien à écrire, rien de requis → admis (0 + 0 >= 0 + 0)
  TEST_ASSERT_TRUE(historySaveAdmit(0, 0, 0, 0));
}
void test_saveAdmit_overflow_required_side(void) {
  // json + marge déborderait size_t en addition naïve : SIZE_MAX + SIZE_MAX.
  // Disponible nul → refus (une addition débordante donnerait requis ≈ 0 → admis à tort).
  TEST_ASSERT_FALSE(historySaveAdmit(0, 0, SIZE_MAX, SIZE_MAX));
}
void test_saveAdmit_overflow_available_side(void) {
  // free + old déborderait size_t : SIZE_MAX + SIZE_MAX couvre SIZE_MAX + SIZE_MAX → admis.
  TEST_ASSERT_TRUE(historySaveAdmit(SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX));
}
void test_saveAdmit_size_max_exact(void) {
  // Disponible SIZE_MAX exactement requis SIZE_MAX → admis ; +1 de marge → refusé.
  TEST_ASSERT_TRUE(historySaveAdmit(SIZE_MAX, 0, SIZE_MAX, 0));
  TEST_ASSERT_FALSE(historySaveAdmit(SIZE_MAX, 0, SIZE_MAX, 1));
}


// ---------------------------------------------------------------------------
// bug-history-doublons-troncature — historyBucketConsolidable
// ---------------------------------------------------------------------------
// RAW_MAX_AGE = 6h = 21600 s ; bucket horaire = 3600 s.

void test_bucketConsolidable_partial_hour_refused(void) {
  // now = 10:00:00 + 6h : le bucket 04:00 court jusqu'à 04:59:59 — sa dernière
  // seconde a EXACTEMENT 6h+1s-3600... cas parlant : bucket dont seule une
  // partie des points dépasse 6h. now tel que bucketStart+3599 a 6h pile →
  // frontière stricte : NON consolidable.
  uint32_t bucketStart = 100000;
  uint32_t now = bucketStart + 3599 + 21600;  // dernière seconde : âge == maxAge
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketStart, 3600, 21600));
}

void test_bucketConsolidable_complete_hour_admitted(void) {
  uint32_t bucketStart = 100000;
  uint32_t now = bucketStart + 3599 + 21601;  // dernière seconde : âge > maxAge
  TEST_ASSERT_TRUE(historyBucketConsolidable(now, bucketStart, 3600, 21600));
}

void test_bucketConsolidable_first_second_old_but_bucket_partial(void) {
  // Le PREMIER point de l'heure a plus de 6h, mais pas la fin de l'heure →
  // l'ancien prédicat par point aurait consolidé (créant l'agrégat partiel),
  // le nouveau refuse — c'est LE cas du bug des doublons.
  uint32_t bucketStart = 100000;
  uint32_t now = bucketStart + 21601;  // début : 6h+1s ; fin : 6h+1s-3599 → trop jeune
  TEST_ASSERT_TRUE(isOlderThan(now, bucketStart, 21600));          // ancien : oui
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketStart, 3600, 21600));  // nouveau : non
}

void test_bucketConsolidable_zero_len_falls_back_to_start(void) {
  uint32_t bucketStart = 100000;
  TEST_ASSERT_TRUE(historyBucketConsolidable(bucketStart + 21601, bucketStart, 0, 21600));
  TEST_ASSERT_FALSE(historyBucketConsolidable(bucketStart + 21600, bucketStart, 0, 21600));
}

void test_bucketConsolidable_daily_bucket(void) {
  // HOURLY_MAX_AGE = 15 j = 1296000 s ; bucket jour = 86400 s.
  uint32_t bucketStart = 1000000;
  uint32_t lastSecond = bucketStart + 86399;
  TEST_ASSERT_FALSE(historyBucketConsolidable(lastSecond + 1296000, bucketStart, 86400, 1296000));
  TEST_ASSERT_TRUE(historyBucketConsolidable(lastSecond + 1296001, bucketStart, 86400, 1296000));
}

// ---------------------------------------------------------------------------
// bug-history-doublons-troncature — historyMergeMeanFinite
// ---------------------------------------------------------------------------

void test_mergeMean_both_finite_average(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.15f, historyMergeMeanFinite(7.1f, 7.2f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 565.5f, historyMergeMeanFinite(550.0f, 581.0f));
}

void test_mergeMean_one_finite_keeps_it(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.1f, historyMergeMeanFinite(7.1f, NAN));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.2f, historyMergeMeanFinite(NAN, 7.2f));
}

void test_mergeMean_none_finite_nan(void) {
  TEST_ASSERT_TRUE(isnan(historyMergeMeanFinite(NAN, NAN)));
  TEST_ASSERT_TRUE(isnan(historyMergeMeanFinite(INFINITY, NAN)));
}


// ---------------------------------------------------------------------------
// bug-points-futurs — historyIsExpired (future-safe) + bucket futur
// ---------------------------------------------------------------------------

void test_isExpired_past_old_true(void) {
  TEST_ASSERT_TRUE(historyIsExpired(1000000, 100000, 100));
}

void test_isExpired_past_recent_false(void) {
  TEST_ASSERT_FALSE(historyIsExpired(1000000, 999990, 100));
}

void test_isExpired_future_false(void) {
  // LE cas du bug : ts > now → le wrap uint32 rendait le point « très ancien ».
  // isOlderThan (invariant conservé) dit true ; historyIsExpired doit dire false.
  uint32_t now = 1786300000, tsFutur = now + 7200;
  TEST_ASSERT_TRUE(isOlderThan(now, tsFutur, 7776000));       // wrap : « vieux »
  TEST_ASSERT_FALSE(historyIsExpired(now, tsFutur, 7776000)); // garde : non expiré
}

void test_isExpired_boundary_now_equals_ts(void) {
  TEST_ASSERT_FALSE(historyIsExpired(1000000, 1000000, 100)); // âge 0 ≤ maxAge
}

void test_bucketConsolidable_future_bucket_refused(void) {
  // Bucket horaire dont la fin est dans le futur → jamais consolidable,
  // même si le wrap le ferait paraître ancien.
  uint32_t now = 1786300000;
  uint32_t bucketFutur = now + 3600;
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketFutur, 3600, 21600));
  // Bucket en cours (commencé, pas fini) : refusé aussi.
  uint32_t bucketEnCours = (now / 3600) * 3600;
  TEST_ASSERT_FALSE(historyBucketConsolidable(now, bucketEnCours, 3600, 21600));
}

// =============================================================================
// feature-095 — BUDGET DE LA PARTITION history (64 Ko)
// =============================================================================
// La décision du lot : la mesure LISSÉE n'est persistée QUE sur les points RAW.
// Ces tests la CHIFFRENT, en confrontant une charge réaliste de 339 points
// (96 RAW + 168 HOURLY + 75 DAILY = les plafonds de constants.h) à la vraie
// fonction d'admission historySaveAdmit() et à la vraie marge
// kHistorySaveSpaceMarginBytes.
//
// La taille du JSON est MESURÉE, pas recopiée : historyPointPersistedLen()
// reproduit octet pour octet ce qu'écrit HistoryManager::saveToFile() via
// ArduinoJson en mode compact (clés courtes t/p/o/T/pf/of/f/d/g, valeurs
// produites par String(x, 2) et String(x, 1)). Une clé ajoutée au format
// persisté fait bouger ces tests — c'est le but.
//
// Mode de défaillance gardé : si historySaveAdmit() refuse, /history.json cesse
// d'être réécrit (WARNING throttlé 1 h, retenté 5 min plus tard) — l'historique
// se fige sans que l'utilisateur le voie sur ses courbes.

// Longueur d'un point tel que PERSISTÉ (pas tel qu'exporté sur /get-history).
static size_t historyPointPersistedLen(unsigned long ts, float ph, float orp,
                                       float temp, int withFiltered, float phF,
                                       float orpF, int filtration, int dosing,
                                       int granularity) {
  char b[256];
  int n = snprintf(b, sizeof(b), "{\"t\":%lu,\"p\":%.2f,\"o\":%.1f,\"T\":%.1f",
                   ts, (double)ph, (double)orp, (double)temp);
  if (withFiltered) {
    n += snprintf(b + n, sizeof(b) - (size_t)n, ",\"pf\":%.2f,\"of\":%.1f",
                  (double)phF, (double)orpF);
  }
  n += snprintf(b + n, sizeof(b) - (size_t)n, ",\"f\":%s,\"d\":%s,\"g\":%d}",
                filtration ? "true" : "false", dosing ? "true" : "false",
                granularity);
  return (size_t)n;
}

// mode : 0 = aucune filtrée (≤ 2.37), 1 = filtrée sur RAW seul (2.38, la
// décision), 2 = filtrée sur TOUTES les granularités (l'alternative écartée).
static size_t historyFileBytes(int mode) {
  size_t total = strlen("{\"data\":[") + strlen("]}");
  size_t n = 0;
  unsigned long ts = 1757000000UL;  // epoch à 10 chiffres (cas courant jusqu'en 2033)
  for (size_t i = 0; i < kMaxRawDataPoints; ++i) {
    total += historyPointPersistedLen(ts + i * 300, 7.23f, 650.4f, 24.6f,
                                      mode >= 1, 7.18f, 648.6f, 1, 0, 0);
    n++;
  }
  for (size_t i = 0; i < kMaxHourlyDataPoints; ++i) {
    total += historyPointPersistedLen(ts - 3600UL * i, 7.21f, 651.3f, 24.4f,
                                      mode == 2, 7.19f, 650.1f, 1, 1, 1);
    n++;
  }
  for (size_t i = 0; i < kMaxDailyDataPoints; ++i) {
    total += historyPointPersistedLen(ts - 86400UL * i, 7.25f, 649.8f, 23.9f,
                                      mode == 2, 7.24f, 649.0f, 1, 1, 2);
    n++;
  }
  return total + (n - 1);  // virgules de séparation
}

// Blocs LittleFS consommés par un fichier de `bytes` octets. Le pointeur de la
// liste CTZ vit DANS le bloc de données (4 à 8 o par bloc) : on retient 8, le
// majorant — se tromper dans l'autre sens ferait passer un budget qui casse.
static size_t historyBlocksFor(size_t bytes) {
  const size_t kUsable = 4096 - 8;
  return (bytes + kUsable - 1) / kUsable;
}

void test_budget_taille_du_fichier_339_points(void) {
  const size_t avant = historyFileBytes(0);
  const size_t raw   = historyFileBytes(1);
  const size_t tout  = historyFileBytes(2);
  // Repères mesurés (v2.38.0) : 23 497 / 25 513 / 30 616 o.
  TEST_ASSERT_EQUAL_size_t(23497, avant);
  TEST_ASSERT_EQUAL_size_t(25513, raw);
  TEST_ASSERT_EQUAL_size_t(30616, tout);
  // Le surcoût de la décision retenue : +2 016 o (21 o × 96 points RAW).
  TEST_ASSERT_EQUAL_size_t(2016, raw - avant);
  // Celui de l'alternative écartée : +7 119 o, soit 3,5 fois plus.
  TEST_ASSERT_EQUAL_size_t(7119, tout - avant);
  // Et un bloc LittleFS de plus (7 contre 8) — c'est là que tout se joue.
  TEST_ASSERT_EQUAL_size_t(7, historyBlocksFor(raw));
  TEST_ASSERT_EQUAL_size_t(8, historyBlocksFor(tout));
}

void test_budget_raw_seul_admis_en_regime_etabli(void) {
  // Régime établi, pas de rotation de journal en cours :
  //   métadonnées LittleFS 2 blocs + /system.log 8 Ko (3 blocs) + fichier 7 blocs
  //   = 12 blocs sur 16 → 4 blocs (16 384 o) libres.
  const size_t json = historyFileBytes(1);
  const size_t used = (2 + historyBlocksFor(8192) + historyBlocksFor(json)) * 4096;
  const size_t freeBytes = 65536 - used;
  const size_t oldSize = json - 90;  // le buffer vient de gagner un point RAW
  TEST_ASSERT_TRUE_MESSAGE(
      historySaveAdmit(freeBytes, oldSize, json, kHistorySaveSpaceMarginBytes),
      "sauvegarde refusee en regime etabli : l'historique cesserait d'etre persiste");
  // Résiduel : ce qui resterait après coup. Doit valoir au moins un bloc.
  const size_t dispo = freeBytes + oldSize;
  const size_t requis = json + kHistorySaveSpaceMarginBytes;
  TEST_ASSERT_TRUE(dispo >= requis + 4096);
  TEST_ASSERT_EQUAL_size_t(8102, dispo - requis);  // 2 blocs moins un point
}

void test_budget_toutes_granularites_aurait_saute_le_dernier_bloc(void) {
  // L'alternative écartée dans le MÊME régime : 13 blocs sur 16 occupés, le
  // résiduel tombe à un demi-bloc. C'est la mesure du risque évité.
  const size_t json = historyFileBytes(2);
  const size_t used = (2 + historyBlocksFor(8192) + historyBlocksFor(json)) * 4096;
  const size_t freeBytes = 65536 - used;
  const size_t dispo = freeBytes + (json - 90);
  const size_t requis = json + kHistorySaveSpaceMarginBytes;
  TEST_ASSERT_EQUAL_size_t(13, 2 + historyBlocksFor(8192) + historyBlocksFor(json));
  TEST_ASSERT_TRUE(dispo >= requis);           // encore admis…
  TEST_ASSERT_TRUE(dispo - requis < 4096);     // …mais plus un bloc de reste
  TEST_ASSERT_EQUAL_size_t(4006, dispo - requis);
}

void test_budget_pire_cas_tmp_de_rotation_orphelin(void) {
  // PIRE CAS : un /system.log.tmp de 6 Ko survit à un reset survenu pendant la
  // rotation (logger.cpp ne le nettoie qu'à l'effacement explicite des logs) —
  // 2 blocs de plus, définitivement.
  //   2 (méta) + 3 (/system.log 8 Ko) + 2 (.tmp 6 Ko) = 7 blocs hors historique.
  const size_t horsHisto = (2 + historyBlocksFor(8192) + historyBlocksFor(6144)) * 4096;

  const size_t avant = historyFileBytes(0);
  const size_t freeAvant = 65536 - (horsHisto + historyBlocksFor(avant) * 4096);
  TEST_ASSERT_TRUE_MESSAGE(
      historySaveAdmit(freeAvant, avant - 68, avant, kHistorySaveSpaceMarginBytes),
      "≤ 2.37 : le pire cas passait");

  // 2.38 (RAW seul) : le 7e bloc du fichier consomme le dernier bloc de jeu ;
  // un fichier qui GRANDIT d'un point (90 o) n'est plus admis dans cette
  // fenêtre. Refus TRANSITOIRE (retenté 5 min plus tard, WARNING throttlé),
  // qui devient permanent tant que le .tmp orphelin n'est pas effacé.
  const size_t raw = historyFileBytes(1);
  const size_t freeRaw = 65536 - (horsHisto + historyBlocksFor(raw) * 4096);
  TEST_ASSERT_EQUAL_size_t(8192, freeRaw);  // 2 blocs
  TEST_ASSERT_FALSE(historySaveAdmit(freeRaw, raw - 90, raw, kHistorySaveSpaceMarginBytes));
  // Fichier stable (pas de croissance) : encore admis, à l'octet près.
  TEST_ASSERT_TRUE(historySaveAdmit(freeRaw, raw, raw, kHistorySaveSpaceMarginBytes));
  TEST_ASSERT_FALSE(historySaveAdmit(freeRaw, raw, raw + 1, kHistorySaveSpaceMarginBytes));

  // L'alternative écartée, elle, échouait de 4 Ko dans le même pire cas.
  const size_t tout = historyFileBytes(2);
  const size_t freeTout = 65536 - (horsHisto + historyBlocksFor(tout) * 4096);
  TEST_ASSERT_FALSE(historySaveAdmit(freeTout, tout - 90, tout, kHistorySaveSpaceMarginBytes));
  TEST_ASSERT_FALSE(historySaveAdmit(freeTout, tout, tout, kHistorySaveSpaceMarginBytes));
}

void test_budget_formule_de_reference_de_la_partition(void) {
  // Formule de référence : 64 Ko − /system.log (8 Ko) − .tmp (6 Ko) − marge
  // d'admission (8 192 o) = 43 008 o utilisables par /history.json, avant
  // arrondi bloc. Le fichier de 2.38 en occupe 7 blocs = 28 672 o.
  const size_t dispo = 65536 - 8192 - 6144 - kHistorySaveSpaceMarginBytes;
  TEST_ASSERT_EQUAL_size_t(43008, dispo);
  const size_t occupe = historyBlocksFor(historyFileBytes(1)) * 4096;
  TEST_ASSERT_EQUAL_size_t(28672, occupe);
  TEST_ASSERT_TRUE_MESSAGE(occupe <= dispo, "budget de partition depasse");
  TEST_ASSERT_EQUAL_size_t(14336, dispo - occupe);  // 3,5 blocs de reste
}

// =============================================================================
// feature-095 — ASSERTIONS SUR LES SOURCES
// =============================================================================
// Ces tests portent sur le TEXTE de src/history.h, src/history.cpp et
// src/web_routes_data.cpp. C'est délibéré, et c'est le SEUL moyen disponible :
// HistoryManager n'est pas compilable en natif (LittleFS, FreeRTOS, sensors,
// pump_controller, ArduinoJson) et l'env `native` ne compile que les modules
// purs (platformio.ini, build_src_filter). Les invariants gardés ici sont
// STRUCTURELS — quelle garde entoure quelle écriture, quel champ entre dans
// quelle somme — et aucun test de comportement natif ne peut les voir.

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  const char* candidates[2];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_history/");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  candidates[0] = rooted[0] ? rooted : NULL;
  candidates[1] = rel;

  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i >= 2) {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    } else {
      if (candidates[i] == NULL) continue;
      snprintf(path, sizeof(path), "%s", candidates[i]);
    }
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
    *outLen = (long)got;
    return buf;
  }
  return NULL;
}

// Extrait le corps d'une fonction : du marqueur `from` jusqu'au marqueur `to`
// (exclu). Retourne un pointeur DANS `src` et écrit la longueur dans outLen.
static const char* slice(const char* src, const char* from, const char* to,
                         size_t* outLen) {
  const char* a = strstr(src, from);
  if (a == NULL) return NULL;
  const char* b = strstr(a + strlen(from), to);
  *outLen = (b != NULL) ? (size_t)(b - a) : strlen(a);
  return a;
}

// Nombre d'occurrences de `needle` dans les `len` premiers octets de `hay`.
static int countIn(const char* hay, size_t len, const char* needle) {
  int c = 0;
  const size_t n = strlen(needle);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) c++;
  }
  return c;
}

static int containsIn(const char* hay, size_t len, const char* needle) {
  return countIn(hay, len, needle) > 0;
}

void test_source_serialisation_de_la_filtree_gardee_par_granularity_raw(void) {
  // LE rempart du budget de partition (cf. tests ci-dessus) : sans la garde
  // `granularity == RAW`, le fichier passe de 7 à 8 blocs et la sauvegarde est
  // refusée dans le pire cas — l'historique cesse d'être persisté en silence.
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/history.cpp introuvable");

  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::saveToFile()",
                           "void HistoryManager::loadFromFile()", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);

  const char* guard = "if (point.granularity == RAW) {";
  const char* pf = "obj[\"pf\"]";
  const char* of = "obj[\"of\"]";
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, guard),
      "garde granularity == RAW absente de saveToFile : budget de partition non tenu");
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, pf));
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, of));

  // Les deux écritures doivent suivre la garde, et de près (même bloc).
  const size_t iGuard = (size_t)(strstr(body, guard) - body);
  const size_t iPf = (size_t)(strstr(body, pf) - body);
  const size_t iOf = (size_t)(strstr(body, of) - body);
  TEST_ASSERT_TRUE_MESSAGE(iPf > iGuard && iPf - iGuard < 200,
      "obj[\"pf\"] hors du bloc garde par granularity == RAW");
  TEST_ASSERT_TRUE_MESSAGE(iOf > iGuard && iOf - iGuard < 300,
      "obj[\"of\"] hors du bloc garde par granularity == RAW");
  // Et elles restent conditionnées à isfinite (jamais de NaN sérialisé).
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "isfinite(point.phFiltered)"));
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "isfinite(point.orpFiltered)"));
  free(src);
}

void test_source_datapoint_initialise_la_filtree_a_nan(void) {
  // DataPoint est construit SANS initialisation à plusieurs endroits
  // (/import-history, avgPoint de consolidateData). Sans initialiseur de
  // membre, phFiltered/orpFiltered valent de la mémoire recyclée — persistée
  // puis affichée comme une mesure réelle. Non détectable en natif autrement :
  // history.h tire <vector> et LittleFS.h, indisponibles sur l'hôte.
  long len = 0;
  char* src = readProjectFile("src/history.h", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/history.h introuvable");
  size_t bodyLen = 0;
  const char* body = slice(src, "struct DataPoint {", "};", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, "float phFiltered = NAN;"),
      "phFiltered sans initialiseur : memoire non initialisee persistee");
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, "float orpFiltered = NAN;"),
      "orpFiltered sans initialiseur : memoire non initialisee persistee");
  free(src);
}

void test_source_relecture_toleante_a_l_absence_des_cles(void) {
  // Un /history.json écrit par ≤ 2.37 n'a ni "pf" ni "of" ; tous les agrégats,
  // même écrits par 2.38, n'en ont pas non plus. La relecture doit retomber sur
  // NaN — un `.as<float>()` nu rendrait 0, c'est-à-dire un pH de 0 affiché.
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::loadFromFile()",
                           "void HistoryManager::migrateLegacyHistory", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, "point[\"pf\"] | NAN"),
      "relecture de pf non tolerante a l'absence de la cle");
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, "point[\"of\"] | NAN"),
      "relecture de of non tolerante a l'absence de la cle");
  free(src);
}

void test_source_consolidation_n_agrege_jamais_la_filtree(void) {
  // Invariant « characterization » gelé : le validCount partagé ne compte QUE
  // le pH brut. Y ajouter la filtrée fausserait du même coup les moyennes
  // d'ORP et de température (elles divisent par ce même compteur).
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::consolidateData()",
                           "void HistoryManager::dedupAggregatedPoints", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);

  // Les deux boucles d'agrégation (HOURLY et DAILY) incrémentent le compteur
  // partagé exactement une fois chacune, dans la garde !isnan(p.ph).
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(body, bodyLen, "validCount++"),
      "nombre d'increments de validCount modifie : moyennes faussees");
  TEST_ASSERT_EQUAL_INT(2, countIn(body, bodyLen, "if (!isnan(p.ph)) {"));

  // La filtrée n'apparaît QUE sous la forme d'une remise à NaN de l'agrégat :
  // 2 affectations phFiltered + 2 orpFiltered, et aucun autre usage (pas de
  // somme, pas de p.phFiltered lu).
  TEST_ASSERT_EQUAL_INT(2, countIn(body, bodyLen, "avgPoint.phFiltered = NAN;"));
  TEST_ASSERT_EQUAL_INT(2, countIn(body, bodyLen, "avgPoint.orpFiltered = NAN;"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, bodyLen, "p.phFiltered"),
      "la filtree d'un point RAW est lue dans la consolidation : elle y est agregee");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, bodyLen, "p.orpFiltered"),
      "la filtree d'un point RAW est lue dans la consolidation : elle y est agregee");
  TEST_ASSERT_EQUAL_INT(0, countIn(body, bodyLen, "avgPoint.phFiltered +="));
  TEST_ASSERT_EQUAL_INT(0, countIn(body, bodyLen, "avgPoint.orpFiltered +="));
  free(src);
}

void test_source_enregistrement_sans_repli_sur_le_brut(void) {
  // « Pas de filtrée » doit rester observable : aucun repli sur la mesure brute,
  // et la porte de mesure (measuresValid) invalide la filtrée comme le brut.
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::recordDataPoint()",
                           "void HistoryManager::saveToFile()", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "point.phFiltered = sensors.getPhFiltered();"));
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "point.orpFiltered = sensors.getOrpFiltered();"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, bodyLen, "point.phFiltered = point.ph"),
      "repli de la filtree sur le brut : une filtree inexistante serait inventee");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, bodyLen, "point.orpFiltered = point.orp;"),
      "repli de la filtree sur le brut : une filtree inexistante serait inventee");
  // Mesures non représentatives → la filtrée tombe aussi.
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "point.phFiltered = NAN;"));
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "point.orpFiltered = NAN;"));
  free(src);
}

void test_source_import_history_relit_les_cles_longues(void) {
  // /import-history rejoue le format EXPORTÉ (clés longues), pas le format
  // persisté : restaurer une sauvegarde ne doit pas perdre la filtrée. Absent
  // ou null → NaN, jamais 0.
  long len = 0;
  char* src = readProjectFile("src/web_routes_data.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/web_routes_data.cpp introuvable");
  TEST_ASSERT_TRUE(containsIn(src, (size_t)len,
      "dp.phFiltered = item[\"phFiltered\"].isNull() ? NAN : item[\"phFiltered\"].as<float>();"));
  TEST_ASSERT_TRUE(containsIn(src, (size_t)len,
      "dp.orpFiltered = item[\"orpFiltered\"].isNull() ? NAN : item[\"orpFiltered\"].as<float>();"));
  // Les points omis de /get-history sont comptés et signalés une fois.
  TEST_ASSERT_TRUE_MESSAGE(containsIn(src, (size_t)len, "st->skipped++"),
      "points omis non comptes : trou de courbe indiscernable d'une panne de sonde");
  TEST_ASSERT_TRUE(containsIn(src, (size_t)len, "kHistorySkipWarnThrottleMs"));
  free(src);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // AC1
  RUN_TEST(test_bucketTimestamp_hourly_boundary);
  RUN_TEST(test_bucketTimestamp_hourly_above_boundary);
  RUN_TEST(test_bucketTimestamp_hourly_below_boundary);
  RUN_TEST(test_bucketTimestamp_hourly_zero_ts);
  RUN_TEST(test_bucketTimestamp_daily_boundary);
  RUN_TEST(test_bucketTimestamp_daily_above_boundary);
  RUN_TEST(test_bucketTimestamp_daily_below_boundary);
  RUN_TEST(test_bucketTimestamp_zero_bucket_guard);

  // AC2
  RUN_TEST(test_isOlderThan_age_zero);
  RUN_TEST(test_isOlderThan_age_equals_max);
  RUN_TEST(test_isOlderThan_age_max_plus_one);
  RUN_TEST(test_isOlderThan_uint32_wrap_within);
  RUN_TEST(test_isOlderThan_uint32_wrap_exceeds);

  // AC3
  RUN_TEST(test_finalizeMean_zero_count_is_nan);
  RUN_TEST(test_finalizeMean_integer_result);
  RUN_TEST(test_finalizeMean_fractional_result);

  // AC4
  RUN_TEST(test_isMajority_2_of_4_false);
  RUN_TEST(test_isMajority_3_of_4_true);
  RUN_TEST(test_isMajority_3_of_5_true);
  RUN_TEST(test_isMajority_0_of_0_false);
  RUN_TEST(test_isMajority_1_of_1_true);
  RUN_TEST(test_isMajority_1_of_2_false);
  RUN_TEST(test_anyTrue_zero_false);
  RUN_TEST(test_anyTrue_one_true);
  RUN_TEST(test_anyTrue_five_true);

  // AC5 — historySaveAdmit
  RUN_TEST(test_saveAdmit_exact_boundary_admitted);
  RUN_TEST(test_saveAdmit_one_byte_short_refused);
  RUN_TEST(test_saveAdmit_one_byte_spare_admitted);
  RUN_TEST(test_saveAdmit_no_old_file_admitted);
  RUN_TEST(test_saveAdmit_no_old_file_refused);
  RUN_TEST(test_saveAdmit_zero_margin);
  RUN_TEST(test_saveAdmit_all_zero_admitted);
  RUN_TEST(test_saveAdmit_overflow_required_side);
  RUN_TEST(test_saveAdmit_overflow_available_side);
  RUN_TEST(test_saveAdmit_size_max_exact);

  // bug-history-doublons-troncature
  RUN_TEST(test_bucketConsolidable_partial_hour_refused);
  RUN_TEST(test_bucketConsolidable_complete_hour_admitted);
  RUN_TEST(test_bucketConsolidable_first_second_old_but_bucket_partial);
  RUN_TEST(test_bucketConsolidable_zero_len_falls_back_to_start);
  RUN_TEST(test_bucketConsolidable_daily_bucket);
  RUN_TEST(test_mergeMean_both_finite_average);
  RUN_TEST(test_mergeMean_one_finite_keeps_it);
  RUN_TEST(test_mergeMean_none_finite_nan);

  // bug-points-futurs
  RUN_TEST(test_isExpired_past_old_true);
  RUN_TEST(test_isExpired_past_recent_false);
  RUN_TEST(test_isExpired_future_false);
  RUN_TEST(test_isExpired_boundary_now_equals_ts);
  RUN_TEST(test_bucketConsolidable_future_bucket_refused);

  // feature-095 — budget de la partition history
  RUN_TEST(test_budget_taille_du_fichier_339_points);
  RUN_TEST(test_budget_raw_seul_admis_en_regime_etabli);
  RUN_TEST(test_budget_toutes_granularites_aurait_saute_le_dernier_bloc);
  RUN_TEST(test_budget_pire_cas_tmp_de_rotation_orphelin);
  RUN_TEST(test_budget_formule_de_reference_de_la_partition);

  // feature-095 — invariants structurels (sources)
  RUN_TEST(test_source_serialisation_de_la_filtree_gardee_par_granularity_raw);
  RUN_TEST(test_source_datapoint_initialise_la_filtree_a_nan);
  RUN_TEST(test_source_relecture_toleante_a_l_absence_des_cles);
  RUN_TEST(test_source_consolidation_n_agrege_jamais_la_filtree);
  RUN_TEST(test_source_enregistrement_sans_repli_sur_le_brut);
  RUN_TEST(test_source_import_history_relit_les_cles_longues);

  return UNITY_END();
}
