// =============================================================================
// test_native_temp_mode — Chloration de secours + mode ORP EFFECTIF (feature-140)
// =============================================================================
// Couvre src/temp_mode_logic.{h,cpp} : module PUR (ADR-0017), aucun millis()
// Arduino, aucune NVS. Les constantes de référence (préfixe kRef*) recopient
// les valeurs FIGÉES par le gate pool-chemistry (constants.h) — recopiées, pas
// incluses, pour ne pas tirer les dépendances lourdes de constants.h (même
// patron que test_native_dosing/test_dosing_decision.cpp, section Boost).
//
// D0 — LE TEST QUI COMPTE LE PLUS : pH NaN (représenté ici par le fait que
// isOrpInhibitedByPh() n'intervient PAS dans ce module — hors périmètre, cf.
// pump_controller.cpp) + NVS "automatic" + secours armé => le mode EFFECTIF
// DOIT devenir Scheduled, ce qui ouvre la branche `scheduled` existante. Et le
// volume EFFECTIF doit ouvrir la branche même si orpDailyTargetMl NVS = 0.
// =============================================================================

#include <unity.h>
#include "temp_mode_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Constantes de référence (figées par le gate pool-chemistry, feature-140,
// gate 1) — recopiées depuis constants.h, PAS incluses.
// ---------------------------------------------------------------------------
static const time_t kRefRescueDurationS     = 172800;  // 48h
static const float  kRefRescueDailyMl       = 1500.0f;
static const float  kRefRescueHardCapMl     = 2000.0f;
static const float  kRefBoostFactor         = 1.5f;
static const time_t kRefMinValidEpoch       = 1700000000;  // 14 nov. 2023
static const uint32_t kRefVisibleMs         = 90000UL;     // 90s

// =============================================================================
// orpModeFromString — conversion NVS -> enum, fail-closed
// =============================================================================

void test_orp_mode_from_string_automatic(void) {
  TEST_ASSERT_TRUE(orpModeFromString("automatic") == OrpMode::Automatic);
}

void test_orp_mode_from_string_scheduled(void) {
  TEST_ASSERT_TRUE(orpModeFromString("scheduled") == OrpMode::Scheduled);
}

void test_orp_mode_from_string_manual(void) {
  TEST_ASSERT_TRUE(orpModeFromString("manual") == OrpMode::Manual);
}

void test_orp_mode_from_string_unknown_fails_closed_to_manual(void) {
  // NVS corrompue ou chaîne inattendue : JAMAIS lue comme une autorisation
  // d'automatisme ou de dosage programmé.
  TEST_ASSERT_TRUE(orpModeFromString("garbage") == OrpMode::Manual);
  TEST_ASSERT_TRUE(orpModeFromString("") == OrpMode::Manual);
}

void test_orp_mode_from_string_null_fails_closed_to_manual(void) {
  TEST_ASSERT_TRUE(orpModeFromString(nullptr) == OrpMode::Manual);
}

// =============================================================================
// effectiveOrpModePure — D0 (le test qui compte le plus)
// =============================================================================

void test_D0_rescue_opens_scheduled_branch_from_automatic_with_unknown_ph(void) {
  // Scénario D0 : NVS "automatic" (l'utilisateur n'a PAS changé sa config) +
  // secours armé. Sans cette bascule, requireKnownPh resterait vrai côté
  // pump_controller.cpp (dérivé de la NVS brute "automatic") et un pH NaN
  // poserait l'inhibition ORP -> zéro injection. Le mode EFFECTIF DOIT devenir
  // Scheduled pour ouvrir la branche `scheduled` existante, aveugle à l'ORP.
  OrpMode nvs = orpModeFromString("automatic");
  OrpMode effective = effectiveOrpModePure(nvs, /*rescueActive=*/true);
  TEST_ASSERT_TRUE(effective == OrpMode::Scheduled);
  TEST_ASSERT_FALSE(effective == OrpMode::Automatic);  // negation explicite : la feature est morte sinon
}

void test_D0_second_chemin_effective_volume_opens_branch_even_if_nvs_target_zero(void) {
  // Second chemin de mort (D0) : NVS "automatic" avec orpDailyTargetMl = 0.
  // La garde `> 0` de pump_controller.cpp (lignes 2645/4366/4395/4470) doit
  // porter sur le volume EFFECTIF, pas sur la valeur NVS brute : avec secours
  // actif, le volume effectif doit valoir kRefRescueDailyMl (1500), PAS 0.
  float effectiveMl = effectiveOrpDailyTargetMlPure(
      /*nvsDailyTargetMl=*/0.0f, /*rescueActive=*/true, /*boostActive=*/false,
      kRefRescueDailyMl, kRefBoostFactor, /*maxChlorineMlPerDay=*/0.0f,
      kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1500.0f, effectiveMl);
  TEST_ASSERT_TRUE(effectiveMl > 0.0f);  // la branche `> 0` s'ouvre réellement
}

// =============================================================================
// effectiveOrpModePure — D1 : idempotence, non-destruction, non-exclusivité
// =============================================================================

void test_manual_nvs_always_manual_regardless_of_rescue(void) {
  // Condition #5 du gate : fail-closed explicite même si rescueActive était
  // vrai par erreur (ne devrait structurellement jamais arriver : le secours
  // est refusé à l'activation en mode Manuel).
  TEST_ASSERT_TRUE(effectiveOrpModePure(OrpMode::Manual, true) == OrpMode::Manual);
  TEST_ASSERT_TRUE(effectiveOrpModePure(OrpMode::Manual, false) == OrpMode::Manual);
}

void test_idempotence_rescue_active_on_nvs_already_scheduled(void) {
  // Cas réel de l'utilisateur AUJOURD'HUI (2026-10-05) : NVS déjà "scheduled"
  // (1500 mL/j manuel). Armer le secours ne doit RIEN casser : effectif reste
  // Scheduled, comme avant activation — idempotence.
  OrpMode nvs = orpModeFromString("scheduled");
  TEST_ASSERT_TRUE(effectiveOrpModePure(nvs, true) == OrpMode::Scheduled);
  TEST_ASSERT_TRUE(effectiveOrpModePure(nvs, false) == OrpMode::Scheduled);
}

void test_no_rescue_leaves_nvs_mode_unchanged_D3_D4(void) {
  // D3/D4 : non-régression — sans secours, le mode effectif est TOUJOURS le
  // mode NVS tel quel, quel qu'il soit (non-destructivité, D4).
  TEST_ASSERT_TRUE(effectiveOrpModePure(OrpMode::Automatic, false) == OrpMode::Automatic);
  TEST_ASSERT_TRUE(effectiveOrpModePure(OrpMode::Scheduled, false) == OrpMode::Scheduled);
}

void test_full_cycle_returns_to_scheduled_never_forced_to_automatic(void) {
  // Cycle complet avec le cas réel utilisateur : NVS = "scheduled" avant, Pendant
  // et après le secours — ce module ne réécrit JAMAIS la NVS (il n'a même pas
  // de moyen de le faire, c'est une fonction pure), donc le paramètre nvsMode
  // reste la SEULE source de vérité. Avant activation, pendant, après
  // expiration (rescueActive repasse à false) : toujours Scheduled, jamais
  // Automatic.
  OrpMode nvsRealValue = orpModeFromString("scheduled");
  OrpMode beforeActivation = effectiveOrpModePure(nvsRealValue, false);
  OrpMode duringRescue      = effectiveOrpModePure(nvsRealValue, true);
  OrpMode afterExpiry       = effectiveOrpModePure(nvsRealValue, false);
  TEST_ASSERT_TRUE(beforeActivation == OrpMode::Scheduled);
  TEST_ASSERT_TRUE(duringRescue == OrpMode::Scheduled);
  TEST_ASSERT_TRUE(afterExpiry == OrpMode::Scheduled);
  TEST_ASSERT_FALSE(afterExpiry == OrpMode::Automatic);
}

void test_full_cycle_with_automatic_nvs_returns_to_automatic_never_stuck_scheduled(void) {
  // Même preuve côté "automatic" : après expiration, l'effectif retourne à
  // Automatic (la NVS n'a jamais bougé) — le secours ne "colle" pas.
  OrpMode nvsRealValue = orpModeFromString("automatic");
  OrpMode duringRescue = effectiveOrpModePure(nvsRealValue, true);
  OrpMode afterExpiry  = effectiveOrpModePure(nvsRealValue, false);
  TEST_ASSERT_TRUE(duringRescue == OrpMode::Scheduled);
  TEST_ASSERT_TRUE(afterExpiry == OrpMode::Automatic);
}

// =============================================================================
// rescueActivePure — D1 : expiration, fail-closed au boot, redémarrage
// =============================================================================

void test_rescue_inactive_if_not_armed(void) {
  TEST_ASSERT_FALSE(rescueActivePure(false, 1800000000, 1800100000, kRefMinValidEpoch));
}

void test_rescue_active_within_window(void) {
  time_t now = 1800000000;
  time_t until = now + kRefRescueDurationS;
  TEST_ASSERT_TRUE(rescueActivePure(true, now, until, kRefMinValidEpoch));
}

void test_rescue_expired_at_expiration_boundary(void) {
  // now == untilEpoch : strictement expiré (condition `now < untilEpoch`).
  time_t until = 1800100000;
  TEST_ASSERT_FALSE(rescueActivePure(true, until, until, kRefMinValidEpoch));
}

void test_rescue_expired_after_48h(void) {
  time_t now = 1800000000;
  time_t until = now + kRefRescueDurationS;
  TEST_ASSERT_FALSE(rescueActivePure(true, until + 1, until, kRefMinValidEpoch));
}

void test_boot_with_epoch_below_min_valid_is_fail_closed(void) {
  // Condition #6 du gate : boot avant synchro NTP (epoch < kMinValidEpoch) =>
  // mode INACTIF, quel que soit l'état persisté (active=true, échéance future
  // non encore atteinte si l'horloge était bonne). JAMAIS actif-pour-toujours.
  time_t now = 100;  // epoch minuscule, type "1er janvier 1970 + 100s"
  time_t until = kRefMinValidEpoch + kRefRescueDurationS;  // persistée, future si l'heure était juste
  TEST_ASSERT_FALSE(rescueActivePure(true, now, until, kRefMinValidEpoch));
}

void test_boot_restart_mid_rescue_survives_if_epoch_valid_and_not_expired(void) {
  // Redémarrage PENDANT le mode, heure déjà synchronisée, échéance pas encore
  // atteinte : l'état persisté (active=true, untilEpoch) suffit à reprendre
  // sans perte — c'est tout ce qu'un redémarrage doit faire ici (pas de
  // compteur à remettre à zéro, condition #7, hors périmètre de ce module pur
  // mais cohérent avec lui : rescueActivePure ne réinitialise jamais rien).
  time_t bootEpoch = kRefMinValidEpoch + 100000;  // heure valide après NTP
  time_t until = bootEpoch + 3600;  // encore 1h à courir
  TEST_ASSERT_TRUE(rescueActivePure(true, bootEpoch, until, kRefMinValidEpoch));
}

void test_boot_exactly_at_min_valid_epoch_is_not_fail_closed(void) {
  // Borne : now == kMinValidEpoch n'est PAS "< minValidEpoch", donc pas de
  // fail-closed à la limite exacte (évite un off-by-one qui retarderait d'1s
  // la toute première évaluation après synchro).
  time_t until = kRefMinValidEpoch + 10;
  TEST_ASSERT_TRUE(rescueActivePure(true, kRefMinValidEpoch, until, kRefMinValidEpoch));
}

// =============================================================================
// rescueDeadlinePure — D1 : expiration à 48h (PAS de test de repliement ici :
// l'échéance est epoch, il n'y a rien à replier — cf. note de la spec).
// =============================================================================

void test_rescue_deadline_is_now_plus_48h(void) {
  time_t now = 1800000000;
  time_t until = rescueDeadlinePure(now, kRefRescueDurationS);
  TEST_ASSERT_EQUAL_INT64((int64_t)(now + kRefRescueDurationS), (int64_t)until);
  // 48h, littéralement — pas 24h (minuit) ni 72h (écartés par le gate).
  TEST_ASSERT_EQUAL_INT64(172800, (int64_t)(until - now));
}

// =============================================================================
// effectiveOrpDailyTargetMlPure — D1/D2 : effets combinés Boost + secours,
// plafond dur, limite utilisateur souveraine
// =============================================================================

void test_no_rescue_returns_nvs_target_unchanged_D3(void) {
  // D3 (Boost non-régression) : secours INACTIF => le Boost n'a JAMAIS touché
  // le volume scheduled/manual avant cette feature, et ça ne doit pas changer
  // ici non plus, qu'il soit actif ou non.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 800.0f,
      effectiveOrpDailyTargetMlPure(800.0f, false, true, kRefRescueDailyMl,
                                    kRefBoostFactor, 0.0f, kRefRescueHardCapMl));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 800.0f,
      effectiveOrpDailyTargetMlPure(800.0f, false, false, kRefRescueDailyMl,
                                    kRefBoostFactor, 0.0f, kRefRescueHardCapMl));
  // Même avec nvsDailyTargetMl = 0 et secours inactif : pas touché.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
      effectiveOrpDailyTargetMlPure(0.0f, false, true, kRefRescueDailyMl,
                                    kRefBoostFactor, 0.0f, kRefRescueHardCapMl));
}

void test_rescue_alone_uses_rescue_daily_ml(void) {
  float v = effectiveOrpDailyTargetMlPure(800.0f, true, false, kRefRescueDailyMl,
                                           kRefBoostFactor, 0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1500.0f, v);
}

void test_rescue_plus_boost_combined_effect_clamped_to_hard_cap(void) {
  // Condition #4 du gate : 1500 x 1,5 = 2250 > plafond dur 2000 => DOIT être
  // clampé à 2000, PAS 2250. C'est le chemin de contournement explicitement
  // fermé par l'ORDRE des opérations (amplification puis clamp).
  float v = effectiveOrpDailyTargetMlPure(800.0f, true, true, kRefRescueDailyMl,
                                          kRefBoostFactor, 0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, v);
  TEST_ASSERT_TRUE(v <= kRefRescueHardCapMl);
}

void test_user_limit_sovereign_if_lower_than_rescue_volume(void) {
  // Condition #3 du gate : la limite utilisateur reste souveraine si elle est
  // PLUS BASSE que le volume de secours (1200 < 1500).
  float v = effectiveOrpDailyTargetMlPure(800.0f, true, false, kRefRescueDailyMl,
                                          kRefBoostFactor, /*maxChlorineMlPerDay=*/1200.0f,
                                          kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1200.0f, v);
}

void test_user_limit_lower_than_boosted_rescue_also_sovereign(void) {
  // Boost + secours = 2250 avant clamp ; limite utilisateur 1800 < 2250 =>
  // clampé à 1800 par la limite utilisateur, PAS par le plafond dur (2000).
  float v = effectiveOrpDailyTargetMlPure(800.0f, true, true, kRefRescueDailyMl,
                                          kRefBoostFactor, /*maxChlorineMlPerDay=*/1800.0f,
                                          kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1800.0f, v);
}

void test_generous_user_limit_does_not_bypass_hard_cap(void) {
  // Limite utilisateur généreuse (2500, > plafond dur) : le plafond dur
  // s'applique quand même en DERNIER (2250 -> pas clampé par 2500 -> clampé
  // par le plafond dur à 2000).
  float v = effectiveOrpDailyTargetMlPure(800.0f, true, true, kRefRescueDailyMl,
                                          kRefBoostFactor, /*maxChlorineMlPerDay=*/2500.0f,
                                          kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, v);
}

void test_rescue_hard_cap_never_exceeded_across_combinations(void) {
  // Balayage de combinaisons : le plafond dur n'est JAMAIS dépassé.
  const float userLimits[] = {0.0f, 500.0f, 1200.0f, 1800.0f, 2000.0f, 2500.0f, 5000.0f};
  for (int boost = 0; boost <= 1; ++boost) {
    for (size_t i = 0; i < sizeof(userLimits) / sizeof(userLimits[0]); ++i) {
      float v = effectiveOrpDailyTargetMlPure(0.0f, true, boost == 1, kRefRescueDailyMl,
                                              kRefBoostFactor, userLimits[i],
                                              kRefRescueHardCapMl);
      TEST_ASSERT_TRUE(v <= kRefRescueHardCapMl);
    }
  }
}

// =============================================================================
// rescueLiftReasonVisiblePure — D1 : quatre valeurs (0/1/2/3), fenêtre 90s,
// repliement de millis() (CELUI-CI en a un, contrairement à l'échéance 48h)
// =============================================================================

void test_rescue_lift_reason_none_always_zero(void) {
  // reasonCode = 0 (None) : toujours 0, quel que soit le temps écoulé.
  TEST_ASSERT_EQUAL_UINT8(0, rescueLiftReasonVisiblePure(0, 0, 0, kRefVisibleMs));
  TEST_ASSERT_EQUAL_UINT8(0, rescueLiftReasonVisiblePure(0, 1000000, 999000, kRefVisibleMs));
}

void test_rescue_lift_reason_visible_within_window_all_four_values(void) {
  // Les quatre codes RescueLiftReason (0=None, 1=AutoLift, 2=Expired48h,
  // 3=Manual) : 1/2/3 doivent rester visibles DANS la fenêtre (ici 10s après
  // la transition, bien sous les 90s).
  uint32_t setAt = 100000;
  uint32_t now   = setAt + 10000;  // 10s plus tard
  TEST_ASSERT_EQUAL_UINT8(1, rescueLiftReasonVisiblePure(1, now, setAt, kRefVisibleMs));
  TEST_ASSERT_EQUAL_UINT8(2, rescueLiftReasonVisiblePure(2, now, setAt, kRefVisibleMs));
  TEST_ASSERT_EQUAL_UINT8(3, rescueLiftReasonVisiblePure(3, now, setAt, kRefVisibleMs));
}

void test_rescue_lift_reason_retombe_a_zero_juste_avant_90s(void) {
  uint32_t setAt = 100000;
  uint32_t now   = setAt + kRefVisibleMs - 1;  // 1ms avant la frontière
  TEST_ASSERT_EQUAL_UINT8(1, rescueLiftReasonVisiblePure(1, now, setAt, kRefVisibleMs));
}

void test_rescue_lift_reason_zero_at_exact_90s_boundary(void) {
  uint32_t setAt = 100000;
  uint32_t now   = setAt + kRefVisibleMs;  // exactement 90s : `>=` => 0
  TEST_ASSERT_EQUAL_UINT8(0, rescueLiftReasonVisiblePure(2, now, setAt, kRefVisibleMs));
}

void test_rescue_lift_reason_zero_well_after_window(void) {
  uint32_t setAt = 100000;
  uint32_t now   = setAt + kRefVisibleMs + 60000;  // 60s après la fenêtre
  TEST_ASSERT_EQUAL_UINT8(0, rescueLiftReasonVisiblePure(3, now, setAt, kRefVisibleMs));
}

void test_rescue_lift_reason_millis_rollover_still_visible(void) {
  // REPLIEMENT DE millis() (celui-ci EN A un, contrairement à l'échéance 48h
  // qui est epoch) : transition posée juste AVANT le repliement de l'uint32_t
  // (2^32 - 1), `now` juste APRÈS, dans la NOUVELLE ère. Écart réel = 10000ms
  // (10s), bien sous les 90s : DOIT rester visible malgré le repliement.
  uint32_t setAt = 4294967295u - 5000u;  // 5s avant 2^32
  uint32_t now   = 5000u;                // 5s après le repliement (nouvelle ère)
  // Écart réel : 5000 (avant repliement) + 5000 (après) = 10000 ms.
  TEST_ASSERT_EQUAL_UINT8(1, rescueLiftReasonVisiblePure(1, now, setAt, kRefVisibleMs));
}

void test_rescue_lift_reason_millis_rollover_expired_past_window(void) {
  // Même repliement, mais l'écart réel (95s) DÉPASSE la fenêtre de 90s : doit
  // bien retomber à 0 malgré le repliement (pas de faux-positif de visibilité
  // permanente après un wrap).
  uint32_t setAt = 4294967295u - 5000u;   // 5s avant 2^32
  uint32_t now   = 90000u;                // 90s après le repliement
  // Écart réel : 5000 + 90000 = 95000 ms > 90000 => 0.
  TEST_ASSERT_EQUAL_UINT8(0, rescueLiftReasonVisiblePure(1, now, setAt, kRefVisibleMs));
}

// =============================================================================
// feature-141 — clampRescueDailyMlPure : plancher 100, plafond souverain
// =============================================================================

static const float kRefRescueDailyMinMl = 100.0f;

void test_clamp_floor_100_applied_when_below(void) {
  // Saisie 10 mL, pas de limite utilisateur (0 = désactivée) : remontée à 100.
  float v = clampRescueDailyMlPure(10.0f, /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, v);
}

void test_clamp_ceiling_is_user_limit_when_below_hard_cap(void) {
  // Limite utilisateur 1800 < plafond dur 2000 : la limite utilisateur gouverne.
  float v = clampRescueDailyMlPure(2500.0f, /*userDailyLimitMl=*/1800.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1800.0f, v);
}

void test_clamp_ceiling_is_hard_cap_when_no_user_limit(void) {
  // userDailyLimitMl = 0 (désactivée) : seul le plafond dur borne au-dessus.
  float v = clampRescueDailyMlPure(2500.0f, /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, v);
}

void test_clamp_hard_cap_wins_over_generous_user_limit(void) {
  // Limite utilisateur 2500 > plafond dur 2000 : le plafond dur reste ultime
  // (même garde que effectiveOrpDailyTargetMlPure, ici sur la saisie directe).
  float v = clampRescueDailyMlPure(2500.0f, /*userDailyLimitMl=*/2500.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, v);
}

void test_clamp_user_limit_sovereign_even_below_floor_100(void) {
  // ⚠️ Cas signalé par la spec feature-141 : limite utilisateur 60 (< 100).
  // Le plancher 100 est appliqué D'ABORD (volume -> 100), PUIS le plafond
  // (min(60, 2000) = 60) s'applique : 100 > 60 => résultat 60, PAS 100.
  // Comportement voulu par pool-chemistry : « la limite utilisateur reste
  // SOUVERAINE même sous le plancher » — une limite journalière de sécurité
  // configurée à 60 mL/j doit rester respectée même par le secours.
  float v = clampRescueDailyMlPure(10.0f, /*userDailyLimitMl=*/60.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, v);
}

// =============================================================================
// feature-141 — resolveRescueDailyMlPure : storedMl absent/hors bornes =>
// chaîne, JAMAIS 0 (condition #2 du gate)
// =============================================================================

void test_resolve_stored_zero_falls_back_to_chain_never_zero(void) {
  // storedMl = 0 (sentinelle NVS absente) : hors bornes (< 100) => chaîne.
  float v = resolveRescueDailyMlPure(/*storedMl=*/0.0f, /*orpDailyTargetMl=*/700.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 700.0f, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

void test_resolve_stored_negative_falls_back_to_chain_never_zero(void) {
  // storedMl = -1 (corruption NVS plausible, uint16 ne le permet pas mais la
  // fonction pure doit rester défensive) : hors bornes => chaîne.
  float v = resolveRescueDailyMlPure(/*storedMl=*/-1.0f, /*orpDailyTargetMl=*/700.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 700.0f, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

void test_resolve_stored_below_floor_falls_back_to_chain_never_zero(void) {
  // storedMl = 50 : sous le plancher 100 => hors bornes => chaîne (PAS
  // simplement remonté à 100 — une valeur SOUS bornes est REMPLACÉE, pas
  // seulement bornée, cf. commentaire de temp_mode_logic.h).
  float v = resolveRescueDailyMlPure(/*storedMl=*/50.0f, /*orpDailyTargetMl=*/700.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 700.0f, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

void test_resolve_stored_above_hard_cap_falls_back_to_chain_never_zero(void) {
  // storedMl = 5000 (au-delà du plafond dur 2000) : hors bornes => chaîne.
  float v = resolveRescueDailyMlPure(/*storedMl=*/5000.0f, /*orpDailyTargetMl=*/700.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 700.0f, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

void test_resolve_chain_branch1_programmed_target_wins(void) {
  // Étage 1 : orpDailyTargetMl > 0 l'emporte sur les étages 2 et 3.
  float v = resolveRescueDailyMlPure(/*storedMl=*/0.0f, /*orpDailyTargetMl=*/700.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 700.0f, v);
}

void test_resolve_chain_branch2_size_default_when_no_target(void) {
  // Étage 2 : orpDailyTargetMl = 0 (non renseigné) => défaut par taille.
  float v = resolveRescueDailyMlPure(/*storedMl=*/0.0f, /*orpDailyTargetMl=*/0.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1000.0f, v);
}

void test_resolve_chain_branch3_fallback_when_no_target_no_size(void) {
  // Étage 3 : ni cible programmée ni défaut taille => repli final, JAMAIS 0.
  float v = resolveRescueDailyMlPure(/*storedMl=*/0.0f, /*orpDailyTargetMl=*/0.0f,
                                      /*sizeDefaultMl=*/0.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kRefRescueDailyMl, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

void test_resolve_size_stage_skipped_on_mqtt_path_invariant_feature_090(void) {
  // Chemin MQTT (startRescueChlorine, config.cpp) : sizeDefaultMl = 0.0f
  // PASSÉ EXPLICITEMENT par l'appelant pour ne JAMAIS consulter PoolSize sans
  // intervention humaine visible (invariant feature-090). orpDailyTargetMl = 0
  // (non renseigné) => l'étage taille est SAUTÉ (pas remplacé par 0) => repli
  // direct sur kRescueChlorineDailyMl, comme config.cpp:1399-1402.
  float v = resolveRescueDailyMlPure(/*storedMl=*/0.0f, /*orpDailyTargetMl=*/0.0f,
                                      /*sizeDefaultMl=*/0.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kRefRescueDailyMl, v);
}

void test_resolve_nominal_first_boot_v2_56_0_rescue_ml_absent_from_nvs(void) {
  // ⚠️ CAS NOMINAL, pas un cas limite : au tout premier boot de la 2.56.0 (ou
  // après un `nvs_flash_erase`), la clé `rescue_ml` N'EXISTE PAS encore dans
  // la NVS -> Preferences::getUShort("rescue_ml", 0) renvoie 0 (la sentinelle
  // par défaut), EXACTEMENT comme loadRescueState() (config.cpp:1299). C'est
  // le chemin de restauration générique (étage taille INCLUS, PAS le chemin
  // MQTT) : rescueDailyMlForDisplay() passe le vrai sizeDefault. Ici,
  // orp_daily_target_ml non renseigné (0, install neuve) et taille bassin
  // Medium (1000) => le volume affiché/armé DOIT être 1000, jamais 0.
  float v = resolveRescueDailyMlPure(/*storedMl=*/0.0f, /*orpDailyTargetMl=*/0.0f,
                                      /*sizeDefaultMl=*/1000.0f, /*fallbackMl=*/kRefRescueDailyMl,
                                      /*userDailyLimitMl=*/0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1000.0f, v);
  TEST_ASSERT_TRUE(v > 0.0f);
}

// =============================================================================
// D3 — LE TEST QUI COMPTE LE PLUS (dette de feature-140, condition #4 du gate
// feature-141) : 1900 mL SAISIS (donc déjà dans les bornes, inchangés par
// clampRescueDailyMlPure à l'activation) + Boost actif => volume effectif
// 2000, JAMAIS 1900 x 1,5 = 2850. Seule une valeur-limite proche du plafond
// dur peut prouver que le clamp C4 s'applique bien APRÈS l'amplification —
// une relecture du code ne le prouve pas.
// =============================================================================

void test_D3_rescue_1900_plus_boost_clamped_to_2000_never_2850(void) {
  float v = effectiveOrpDailyTargetMlPure(/*nvsDailyTargetMl=*/0.0f, /*rescueActive=*/true,
                                           /*boostActive=*/true, /*rescueDailyMl=*/1900.0f,
                                           kRefBoostFactor, /*maxChlorineMlPerDay=*/0.0f,
                                           kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, v);
  TEST_ASSERT_FALSE(v > 2000.001f);
  // Négation explicite : la valeur non clampée (1900 x 1,5) ne doit JAMAIS
  // apparaître en sortie — c'est précisément le chemin de contournement que
  // la condition #4 du gate ferme par l'ORDRE des opérations.
  TEST_ASSERT_FALSE((v > 2849.0f) && (v < 2851.0f));
}

void test_D3_just_below_boost_threshold_not_clamped(void) {
  // Juste SOUS le seuil où l'amplification franchirait le plafond dur
  // (2000 / 1,5 = 1333,33...) : 1333 x 1,5 = 1999,5 < 2000, PAS clampé —
  // prouve que le clamp n'intervient QUE quand c'est nécessaire, pas en
  // permanence (sinon le test D3 ne prouverait rien de l'ORDRE des
  // opérations, juste un clamp systématique à 2000).
  float v = effectiveOrpDailyTargetMlPure(0.0f, true, true, /*rescueDailyMl=*/1333.0f,
                                           kRefBoostFactor, 0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1999.5f, v);
  TEST_ASSERT_TRUE(v < 2000.0f);
}

void test_D3_just_above_boost_threshold_clamped_to_2000(void) {
  // Juste AU-DESSUS du même seuil : 1334 x 1,5 = 2001 > 2000 => clampé à 2000.
  float v = effectiveOrpDailyTargetMlPure(0.0f, true, true, /*rescueDailyMl=*/1334.0f,
                                           kRefBoostFactor, 0.0f, kRefRescueHardCapMl);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, v);
}

// =============================================================================
int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_orp_mode_from_string_automatic);
  RUN_TEST(test_orp_mode_from_string_scheduled);
  RUN_TEST(test_orp_mode_from_string_manual);
  RUN_TEST(test_orp_mode_from_string_unknown_fails_closed_to_manual);
  RUN_TEST(test_orp_mode_from_string_null_fails_closed_to_manual);

  RUN_TEST(test_D0_rescue_opens_scheduled_branch_from_automatic_with_unknown_ph);
  RUN_TEST(test_D0_second_chemin_effective_volume_opens_branch_even_if_nvs_target_zero);

  RUN_TEST(test_manual_nvs_always_manual_regardless_of_rescue);
  RUN_TEST(test_idempotence_rescue_active_on_nvs_already_scheduled);
  RUN_TEST(test_no_rescue_leaves_nvs_mode_unchanged_D3_D4);
  RUN_TEST(test_full_cycle_returns_to_scheduled_never_forced_to_automatic);
  RUN_TEST(test_full_cycle_with_automatic_nvs_returns_to_automatic_never_stuck_scheduled);

  RUN_TEST(test_rescue_inactive_if_not_armed);
  RUN_TEST(test_rescue_active_within_window);
  RUN_TEST(test_rescue_expired_at_expiration_boundary);
  RUN_TEST(test_rescue_expired_after_48h);
  RUN_TEST(test_boot_with_epoch_below_min_valid_is_fail_closed);
  RUN_TEST(test_boot_restart_mid_rescue_survives_if_epoch_valid_and_not_expired);
  RUN_TEST(test_boot_exactly_at_min_valid_epoch_is_not_fail_closed);

  RUN_TEST(test_rescue_deadline_is_now_plus_48h);

  RUN_TEST(test_no_rescue_returns_nvs_target_unchanged_D3);
  RUN_TEST(test_rescue_alone_uses_rescue_daily_ml);
  RUN_TEST(test_rescue_plus_boost_combined_effect_clamped_to_hard_cap);
  RUN_TEST(test_user_limit_sovereign_if_lower_than_rescue_volume);
  RUN_TEST(test_user_limit_lower_than_boosted_rescue_also_sovereign);
  RUN_TEST(test_generous_user_limit_does_not_bypass_hard_cap);
  RUN_TEST(test_rescue_hard_cap_never_exceeded_across_combinations);

  RUN_TEST(test_rescue_lift_reason_none_always_zero);
  RUN_TEST(test_rescue_lift_reason_visible_within_window_all_four_values);
  RUN_TEST(test_rescue_lift_reason_retombe_a_zero_juste_avant_90s);
  RUN_TEST(test_rescue_lift_reason_zero_at_exact_90s_boundary);
  RUN_TEST(test_rescue_lift_reason_zero_well_after_window);
  RUN_TEST(test_rescue_lift_reason_millis_rollover_still_visible);
  RUN_TEST(test_rescue_lift_reason_millis_rollover_expired_past_window);

  RUN_TEST(test_clamp_floor_100_applied_when_below);
  RUN_TEST(test_clamp_ceiling_is_user_limit_when_below_hard_cap);
  RUN_TEST(test_clamp_ceiling_is_hard_cap_when_no_user_limit);
  RUN_TEST(test_clamp_hard_cap_wins_over_generous_user_limit);
  RUN_TEST(test_clamp_user_limit_sovereign_even_below_floor_100);

  RUN_TEST(test_resolve_stored_zero_falls_back_to_chain_never_zero);
  RUN_TEST(test_resolve_stored_negative_falls_back_to_chain_never_zero);
  RUN_TEST(test_resolve_stored_below_floor_falls_back_to_chain_never_zero);
  RUN_TEST(test_resolve_stored_above_hard_cap_falls_back_to_chain_never_zero);
  RUN_TEST(test_resolve_chain_branch1_programmed_target_wins);
  RUN_TEST(test_resolve_chain_branch2_size_default_when_no_target);
  RUN_TEST(test_resolve_chain_branch3_fallback_when_no_target_no_size);
  RUN_TEST(test_resolve_size_stage_skipped_on_mqtt_path_invariant_feature_090);
  RUN_TEST(test_resolve_nominal_first_boot_v2_56_0_rescue_ml_absent_from_nvs);

  RUN_TEST(test_D3_rescue_1900_plus_boost_clamped_to_2000_never_2850);
  RUN_TEST(test_D3_just_below_boost_threshold_not_clamped);
  RUN_TEST(test_D3_just_above_boost_threshold_clamped_to_2000);

  return UNITY_END();
}
