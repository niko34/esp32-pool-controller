// =============================================================================
// Tests unitaires natifs — history_logic (feature-041)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS matériel ESP32.
// On teste le COMPORTEMENT observable du module pur d'agrégation d'historique :
//   - bucketTimestamp (AC1, frontières strictes + garde bucketSeconds==0)
//   - isOlderThan     (AC2, frontière stricte + wrap uint32)
//   - finalizeMean    (AC3, count==0 → NaN)
//   - isMajority      (AC4, division entière stricte)
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
// AC4 — isMajority (division ENTIÈRE stricte)
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
// bug-098 (2026-09-07) — anyTrue() a été RETIRÉE du module pur avec ses trois
// tests : l'union bit à bit des masques de dosage (`doseMaskUnion |=
// p.doseMask`) l'a remplacée dans les deux boucles de consolidateData(), voie
// par voie, et le dernier `anyTrue(phDosingCount)` a disparu avec les deux
// booléens de DataPoint. Des assertions vertes sur du code mort font croire à
// un invariant vivant et maintiennent une couverture de 100 % trompeuse.

// -----------------------------------------------------------------------------
// AC5 — historySaveAdmit (bug-nospc-partition-history, RÉVISÉ feature-131)
// -----------------------------------------------------------------------------
// Sémantique DEPUIS feature-131 (écriture atomique .tmp+rename) :
//   free >= json + marge → admis (frontière LARGE), PLUS de crédit de l'ancien
// fichier (le paramètre `oldFileBytes` a disparu de la signature). Avec
// .tmp+rename, l'ancien /history.json et le .tmp du nouveau COEXISTENT
// pendant toute l'écriture : son espace n'est récupéré qu'au rename final, et
// `freeBytes` (totalBytes()-usedBytes()) le compte déjà comme « occupé », donc
// non disponible — il n'y a plus rien à créditer explicitement.
// Ces cas remplacent AC5 (ancienne signature 4 arguments, ADR historique
// bug-nospc-partition-history) — voir aussi la section « feature-131 — plus de
// crédit de l'ancien fichier » plus bas pour la preuve du changement de
// sémantique sur un scénario chiffré.
void test_saveAdmit_exact_boundary_admitted(void) {
  // 22000 == 22000 + 0 → admis (égalité exacte)
  TEST_ASSERT_TRUE(historySaveAdmit(22000, 22000, 0));
}
void test_saveAdmit_one_byte_short_refused(void) {
  TEST_ASSERT_FALSE(historySaveAdmit(29999, 22000, 8000));
}
void test_saveAdmit_one_byte_spare_admitted(void) {
  TEST_ASSERT_TRUE(historySaveAdmit(30001, 22000, 8000));
}
void test_saveAdmit_zero_margin(void) {
  // Marge 0 : json seul décide (frontière large)
  TEST_ASSERT_TRUE(historySaveAdmit(22000, 22000, 0));
  TEST_ASSERT_FALSE(historySaveAdmit(21999, 22000, 0));
}
void test_saveAdmit_all_zero_admitted(void) {
  // Rien à écrire, rien de requis → admis (0 >= 0 + 0)
  TEST_ASSERT_TRUE(historySaveAdmit(0, 0, 0));
}
void test_saveAdmit_overflow_required_side(void) {
  // json + marge déborderait size_t en addition naïve : SIZE_MAX + SIZE_MAX.
  // Disponible nul → refus (une addition débordante donnerait requis ≈ 0 → admis à tort).
  TEST_ASSERT_FALSE(historySaveAdmit(0, SIZE_MAX, SIZE_MAX));
}
void test_saveAdmit_size_max_exact(void) {
  // Disponible SIZE_MAX exactement requis SIZE_MAX → admis ; +1 de marge → refusé.
  TEST_ASSERT_TRUE(historySaveAdmit(SIZE_MAX, SIZE_MAX, 0));
  TEST_ASSERT_FALSE(historySaveAdmit(SIZE_MAX, SIZE_MAX, 1));
}

// ---------------------------------------------------------------------------
// feature-131 — PLUS DE CRÉDIT DE L'ANCIEN FICHIER (mutant F3 n°6)
// ---------------------------------------------------------------------------
// Preuve chiffrée du changement de sémantique, pas seulement de signature.
// Scénario : un fichier ~25 Ko occupe déjà la partition (7 blocs LittleFS),
// et un nouveau JSON, à peine plus gros, doit être écrit à côté (.tmp) avant
// le rename. `freeBytes` ci-dessous est délibérément calculé COMME SI
// l'ancien fichier restait en place pendant toute l'écriture (c'est
// exactement le cas depuis feature-131) : il n'inclut PAS son espace.
void test_saveAdmit_ancien_ne_credite_plus_l_espace(void) {
  const size_t oldFileBytes = 28672;   // 7 blocs LittleFS (4096 o), ancien /history.json
  const size_t jsonBytes = 25603;      // nouveau JSON, légèrement plus gros
  const size_t marginBytes = 8192;     // kHistorySaveSpaceMarginBytes
  const size_t freeBytes = 30000;      // libre réel, ANCIEN FICHIER DÉJÀ DÉDUIT

  // Sous l'ANCIENNE sémantique (crédit de l'ancien, formule reconstituée ici
  // SANS appeler la fonction supprimée — la signature n'existe plus) :
  //   free + old = 30000 + 28672 = 58672 >= 25603 + 8192 = 33795 → ADMIS.
  const size_t oldSemanticsAvailable = freeBytes + oldFileBytes;
  const size_t required = jsonBytes + marginBytes;
  TEST_ASSERT_TRUE_MESSAGE(oldSemanticsAvailable >= required,
      "le scenario doit etre choisi pour etre admis sous l'ancienne semantique");

  // Sous la NOUVELLE sémantique (celle réellement implémentée) : le même
  // `freeBytes` ne suffit plus, parce que l'ancien fichier COEXISTE avec le
  // .tmp — son espace n'est jamais rendu disponible pendant l'écriture.
  // Si un mutant réintroduisait un crédit (même sans le paramètre, par ex. en
  // gonflant freeBytes en interne), ce test basculerait à tort en succès.
  TEST_ASSERT_FALSE_MESSAGE(historySaveAdmit(freeBytes, jsonBytes, marginBytes),
      "regression : l'espace de l'ancien fichier est de nouveau credite alors "
      "qu'il coexiste avec le .tmp jusqu'au rename final (feature-131)");
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

// ⚠️ MODÈLE PÉRIMÉ SUR DEUX POINTS, relevé en bug-098 (2026-09-07) — signalé à
// la revue, NON corrigé ici parce que la correction propage sur une dizaine de
// constantes asservies (23497 / 25513 / 30616 / 2016 / 7119 / 8102 / 4006 /
// 43008) et sur tout le récit de budget :
//   1. la clé « d » n'est plus un BOOLÉEN mais un masque ENTIER (bug-098). Le
//      modèle ci-dessous SURESTIME donc le fichier — de 1 113 o sur la charge
//      de 339 points (chiffre mesuré par test_native_dose_mask, section 6). Il
//      se trompe du côté SÛR (il annonce plus gros que la réalité), c'est
//      pourquoi ces tests restent verts et pertinents comme MAJORANT ;
//   2. la partition `history` fait 256 Ko depuis le layout v5 (ADR-0068), pas
//      les 64 Ko sur lesquels raisonnent les tests d'admission qui suivent.
// Tant que ce n'est pas repris, lire ces chiffres comme une BORNE HAUTE
// historique, pas comme l'état du format persisté d'aujourd'hui.
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

// ---------------------------------------------------------------------------
// feature-131 — SCÉNARIOS REPENSÉS (pas adaptés mécaniquement)
// ---------------------------------------------------------------------------
// Les trois tests ci-dessous encodaient la sémantique de CRÉDIT de l'ancien
// fichier (remove() avant réécriture, partition 64 Ko, ADR-0033). Cette
// sémantique n'existe plus : depuis feature-131 (.tmp + rename), l'ancien
// /history.json et le .tmp coexistent PENDANT TOUTE L'ÉCRITURE — son espace
// n'est jamais crédité, et la partition `history` fait 256 Ko depuis
// ADR-0068 (pas 64 Ko). Les scénarios sont donc reconstruits sur ces deux
// bases nouvelles, pas simplement rejoués avec un paramètre en moins.
//
// kHistoryPartitionV5Bytes : 256 Ko (0x40000), layout v5 (ADR-0068) — la
// partition `history` héberge /history.json ET /system.log (+ leurs .tmp).
// Constante LOCALE au test (pas de constante globale équivalente dans
// constants.h à ce jour) : si elle diverge un jour de la vraie partition,
// c'est `partitions.csv`/`build_fs.sh` qui font foi, pas ce fichier.
static const size_t kHistoryPartitionV5Bytes = 262144;

void test_budget_regime_etabli_256ko_admis_sans_credit(void) {
  // Régime établi : l'ancien /history.json (mode RAW seul, 7 blocs) reste EN
  // PLACE pendant que le .tmp du nouveau s'écrit à côté — occupation réelle :
  // méta (2 blocs) + /system.log 8 Ko (2 blocs) + ancien fichier (7 blocs).
  const size_t json = historyFileBytes(1);  // nouveau JSON, même taille de régime
  const size_t oldFileBlocks = historyBlocksFor(json);
  const size_t usedHorsNouveau =
      (2 + historyBlocksFor(8192) + oldFileBlocks) * 4096;
  const size_t freeBytes = kHistoryPartitionV5Bytes - usedHorsNouveau;

  // AUCUN crédit de l'ancien fichier : seul `freeBytes` (qui l'exclut déjà,
  // puisqu'il reste occupé) est comparé à json + marge.
  TEST_ASSERT_TRUE_MESSAGE(
      historySaveAdmit(freeBytes, json, kHistorySaveSpaceMarginBytes),
      "sauvegarde refusee en regime etabli (256 Ko, sans credit) : "
      "l'historique cesserait d'etre persiste");
  const size_t requis = json + kHistorySaveSpaceMarginBytes;
  TEST_ASSERT_TRUE(freeBytes >= requis);
  // ADR-0068 dissout le motif de resserrement de l'ancienne partition 64 Ko :
  // le résiduel se compte ici en dizaines de blocs, pas en fraction de bloc.
  TEST_ASSERT_TRUE_MESSAGE(freeBytes - requis > 10 * 4096,
      "marge resultante anormalement faible pour une partition de 256 Ko");
}

void test_budget_pire_cas_256ko_avec_debris_reste_admis(void) {
  // PIRE CAS chargé de tout le débris possible : ancien fichier au format le
  // plus coûteux (toutes granularités, mode=2, écarté par la décision
  // feature-095 mais modélisé ici en pire cas), /system.log plein (8 Ko), ET
  // un .tmp de rotation du journal orphelin (6 Ko, cf. logger.cpp/ADR-0076)
  // qui survit tant que les logs ne sont pas effacés explicitement.
  const size_t tout = historyFileBytes(2);
  const size_t oldFileBlocks = historyBlocksFor(tout);
  const size_t usedHorsNouveau =
      (2 + historyBlocksFor(8192) + historyBlocksFor(6144) + oldFileBlocks) * 4096;
  const size_t freeBytes = kHistoryPartitionV5Bytes - usedHorsNouveau;

  // Sous 64 Ko (ADR-0033), ce même pire cas ÉCHOUAIT (cf. l'ancien
  // test_budget_pire_cas_tmp_de_rotation_orphelin, retiré). Sous 256 Ko et
  // sans crédit, il passe LARGEMENT : c'est précisément le motif documenté
  // dans constants.h pour dissoudre ADR-0033 sur ce point.
  TEST_ASSERT_TRUE_MESSAGE(
      historySaveAdmit(freeBytes, tout, kHistorySaveSpaceMarginBytes),
      "le pire cas de debris ne devrait plus refuser sous la partition 256 Ko (ADR-0068)");
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

// test_source_serialisation_de_la_filtree_gardee_par_granularity_raw a été
// RETIRÉ ici (feature-131) : il grattait le texte de
// `HistoryManager::saveToFile()` pour vérifier que `obj["pf"]`/`obj["of"]`
// restaient sous la garde `granularity == RAW`. Depuis feature-131, ce code
// n'existe plus dans history.cpp — il vit dans le module PUR
// `formatPersistedHistoryPoint()` (src/history_persist_format.h), directement
// TESTABLE EN COMPORTEMENT (pas seulement en texte source) : voir
// test/test_native_history_persist_format/, section (b) « golden par point »,
// qui vérifie l'ABSENCE effective de "pf"/"of" sur un point HOURLY/DAILY en
// comparant la sortie réelle de la fonction, pas son code. Un test de
// comportement direct est strictement plus fort qu'un test de source : garder
// les deux aurait dupliqué la preuve sans rien vérifier de plus.

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

  // AC5 — historySaveAdmit
  RUN_TEST(test_saveAdmit_exact_boundary_admitted);
  RUN_TEST(test_saveAdmit_one_byte_short_refused);
  RUN_TEST(test_saveAdmit_one_byte_spare_admitted);
  RUN_TEST(test_saveAdmit_zero_margin);
  RUN_TEST(test_saveAdmit_all_zero_admitted);
  RUN_TEST(test_saveAdmit_overflow_required_side);
  RUN_TEST(test_saveAdmit_size_max_exact);
  RUN_TEST(test_saveAdmit_ancien_ne_credite_plus_l_espace);

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
  RUN_TEST(test_budget_regime_etabli_256ko_admis_sans_credit);
  RUN_TEST(test_budget_pire_cas_256ko_avec_debris_reste_admis);
  RUN_TEST(test_budget_formule_de_reference_de_la_partition);

  // feature-095 — invariants structurels (sources)
  RUN_TEST(test_source_datapoint_initialise_la_filtree_a_nan);
  RUN_TEST(test_source_relecture_toleante_a_l_absence_des_cles);
  RUN_TEST(test_source_consolidation_n_agrege_jamais_la_filtree);
  RUN_TEST(test_source_enregistrement_sans_repli_sur_le_brut);
  RUN_TEST(test_source_import_history_relit_les_cles_longues);

  return UNITY_END();
}
