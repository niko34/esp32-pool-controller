// =============================================================================
// Tests unitaires natifs — Détecteur de stabilité des bruts pH/ORP
// (purge_stability_logic, feature-091 v2.34.0)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino. Module PUR
// (ADR-0017) : l'appelant fournit les lectures, l'état vit dans une struct.
//
// Les PARAMÈTRES viennent de constants.h (kPurge*) et non de valeurs locales :
// une mutation des constantes de production doit être VUE par cette suite.
// Décision pool-chemistry (04/09/2026) : fenêtre W = 10 min, plancher 10 min.
//
// ORGANISATION
//   A — Médiane (pair/impair, NaN exclus à l'accumulation)
//   B — Cinématique de la fenêtre : jamais stable ni instable avant fenêtre
//       pleine (mutant « unstable sur fenêtre partielle »)
//   C — Signaux synthétiques : plat bruité, dérives (limite connue 0,017 pH/min),
//       hystérésis 1×/2×, pointes isolées (mutant « médiane → moyenne »)
//   D — Agrégation des voies : ORP seul instable, ORP jamais lu, ORP partiel,
//       pentes ORP (limite connue 1,6 mV/min)
//   E — Rejeux des courbes RÉELLES (04/09 et 02/09), à 5 s, bruit ±0,18
//       déterministe (LCG à graine fixe) ; démonstration figée W=8 ; 5 graines
//
// LIMITE PHYSIQUE DU DÉTECTEUR (à garder en tête pour lire les seuils) :
//   étendue de W médianes-minute ≤ seuil. Une dérive MONOTONE de pente s donne
//   une étendue s·(W−1) → détectable si s > seuil/(W−1) = 0,15/9 ≈ 0,017 pH/min.
//   Un FOND DE V (descente puis remontée) n'occupe que la moitié de la fenêtre
//   de chaque côté : étendue ≈ s·(W−1)/2 → pente détectable = seuil/((W−1)/2)
//   ≈ 0,033 pH/min. C'est pourquoi W=4 et W=8 ouvraient dans le fond du
//   plongeon du 04/09 (pente ~0,045 pH/min de chaque côté du fond) et W=10 non.
//
// Les rejeux encodent les EXIGENCES de la spec feature-091 (source de vérité :
// « la porte doit rester fermée pour TOUS les termes tant que la mesure
// bouge », « le critère de stabilité doit tenir sur ~50 min de rinçage »),
// pas le comportement observé du module. Un rejeu rouge est une information,
// pas un test à retoucher.
// =============================================================================

#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "purge_stability_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Décor commun
// -----------------------------------------------------------------------------
static PurgeStabilityParams prodParams() {
  PurgeStabilityParams p;
  p.samplesPerMin = kPurgeSamplesPerMin;
  p.windowMin = kPurgeWindowMin;
  p.phStableRange = kPurgeStablePhRange;
  p.orpStableRange = kPurgeStableOrpRange;
  p.phRecloseRange = kPurgeRecloseRangePh;
  p.orpRecloseRange = kPurgeRecloseRangeOrp;
  return p;
}

static const int kSpm = kPurgeSamplesPerMin;  // 12 lectures / minute (5 s)
static const int kW = kPurgeWindowMin;        // 10 minutes

// Bruit RÉEL des lectures : ±0,18 sur le brut pH (cf. mémoire « dispersion
// ~0,36 ») ; ±5 mV sur l'ORP (EZO ORP, quelques mV crête). Un bruit ORP de
// ±18 mV (essayé) est irréaliste et fait clignoter la voie ORP à elle seule :
// σ(médiane de 12) ≈ 5,2 mV → étendue de 10 médianes ≈ 16 mV > 15.
static const float kPhNoise = 0.18f;
static const float kOrpNoise = 5.0f;

// Bruit pseudo-aléatoire DÉTERMINISTE (LCG numérique de Numerical Recipes),
// uniforme sur [−amp ; +amp]. Graine remise à chaque test : les rejeux sont
// reproductibles au bit près d'une exécution à l'autre.
static uint32_t g_seed = 0;
static void seed(uint32_t s) { g_seed = s; }
static float noise(float amp) {
  g_seed = g_seed * 1664525u + 1013904223u;
  return amp * (((g_seed >> 8) & 0xFFFFu) / 32767.5f - 1.0f);
}

static float lerp(float a, float b, float f) { return a + (b - a) * f; }

// Injecte `minutes` minutes complètes d'un pH constant (ORP NaN par défaut).
static void feedFlat(PurgeStabilityState& st, const PurgeStabilityParams& p,
                     int minutes, float ph, float orp = NAN, float noiseAmp = 0.0f,
                     float orpNoiseAmp = 0.0f) {
  for (int s = 0; s < minutes * kSpm; ++s) {
    const float o = isnan(orp) ? NAN : orp + noise(orpNoiseAmp);
    purgeStabilityAddSample(st, p, ph + noise(noiseAmp), o);
  }
}

// =============================================================================
// A — MÉDIANE
// =============================================================================
void test_A_mediane_impaire_et_paire(void) {
  const float impair[5] = {7.3f, 6.9f, 7.0f, 9.5f, 7.1f};  // trié : 6.9 7.0 7.1 7.3 9.5
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.1f, purgeStabilityMedian(impair, 5));
  const float pair[4] = {7.4f, 7.0f, 7.2f, 6.8f};  // trié : 6.8 7.0 7.2 7.4 → (7.0+7.2)/2
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.1f, purgeStabilityMedian(pair, 4));
  const float un[1] = {6.5f};
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.5f, purgeStabilityMedian(un, 1));
  TEST_ASSERT_TRUE(isnan(purgeStabilityMedian(un, 0)));
  // L'entrée n'est pas modifiée par le tri.
  TEST_ASSERT_EQUAL_FLOAT(7.3f, impair[0]);
  TEST_ASSERT_EQUAL_FLOAT(9.5f, impair[3]);
}

// Une lecture NaN (stale, erreur I²C) n'entre PAS dans la minute : la minute est
// définie en NOMBRE de lectures valides. 12 soumissions dont 3 NaN = 9 valides
// → aucune médiane ; 3 valides de plus → la première médiane tombe.
void test_A_nan_exclus_de_la_minute(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  for (int s = 0; s < kSpm; ++s) {
    purgeStabilityAddSample(st, p, (s % 4 == 0) ? NAN : 7.0f, NAN);
  }
  TEST_ASSERT_EQUAL_UINT8(0, purgeChannelMinutes(st.ph));
  for (int s = 0; s < 3; ++s) purgeStabilityAddSample(st, p, 7.0f, NAN);
  TEST_ASSERT_EQUAL_UINT8(1, purgeChannelMinutes(st.ph));
  // Le min/max des bruts ignore aussi les NaN.
  TEST_ASSERT_EQUAL_FLOAT(7.0f, purgeChannelRawMin(st.ph));
  TEST_ASSERT_EQUAL_FLOAT(7.0f, purgeChannelRawMax(st.ph));
}

// La médiane-minute est ROBUSTE à une pointe isolée : 11 lectures à 7,00 et une
// à 9,80 (glitch EZO) donnent 7,00, pas 7,23. C'est le test qui distingue la
// médiane d'une moyenne.
void test_A_mediane_minute_insensible_a_une_pointe(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  for (int s = 0; s < kSpm; ++s) purgeStabilityAddSample(st, p, (s == 5) ? 9.80f : 7.00f, NAN);
  TEST_ASSERT_EQUAL_UINT8(1, purgeChannelMinutes(st.ph));
  // Étendue sur 1 médiane = 0 ; la médiane elle-même vaut 7,00 (visible via une
  // 2e minute propre : l'étendue reste nulle si la 1re médiane valait 7,00).
  for (int s = 0; s < kSpm; ++s) purgeStabilityAddSample(st, p, 7.00f, NAN);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, purgeChannelRange(st.ph));
  // Le min/max BRUT, lui, voit la pointe (journal de fin de purge).
  TEST_ASSERT_EQUAL_FLOAT(9.80f, purgeChannelRawMax(st.ph));
}

// =============================================================================
// B — CINÉMATIQUE DE LA FENÊTRE
// =============================================================================

// Jamais stable avant la fenêtre pleine : W−1 minutes d'un signal parfaitement
// plat ne suffisent pas, la W-ième ouvre. `anyFull` suit la même frontière.
void test_B_stable_exactement_a_fenetre_pleine(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  for (int m = 1; m <= kW; ++m) {
    feedFlat(st, p, 1, 7.10f);
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[64];
    snprintf(msg, sizeof(msg), "minute %d", m);
    if (m < kW) {
      TEST_ASSERT_FALSE_MESSAGE(v.stable, msg);
      TEST_ASSERT_FALSE_MESSAGE(v.anyFull, msg);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(v.stable, msg);
      TEST_ASSERT_TRUE_MESSAGE(v.anyFull, msg);
    }
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
  }
}

// MUTANT 12 — `unstable` ne doit JAMAIS être calculé sur une fenêtre partielle :
// une marche de 1,0 pH entre la 1re et la 2e minute donne une étendue de 1,0
// (> 0,30) dès 2 médianes — et pourtant ni stable ni instable tant que la
// fenêtre n'est pas pleine (W−1 médianes). Sinon une re-purge fraîchement
// démarrée se « refermerait » sur elle-même dès la 2e minute.
void test_B_jamais_instable_sur_fenetre_partielle(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  feedFlat(st, p, 1, 7.00f);
  for (int m = 2; m < kW; ++m) {
    feedFlat(st, p, 1, 6.00f);
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[64];
    snprintf(msg, sizeof(msg), "%d medianes (partiel)", m);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f, v.phRange, msg);
    TEST_ASSERT_FALSE_MESSAGE(v.anyFull, msg);
    TEST_ASSERT_FALSE_MESSAGE(v.stable, msg);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
  }
  feedFlat(st, p, 1, 6.00f);  // W médianes : pleine → la marche est vue
  const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_TRUE(v.anyFull);
  TEST_ASSERT_TRUE(v.unstable);
  TEST_ASSERT_FALSE(v.stable);
}

// La fenêtre GLISSE : la marche sort de l'anneau après windowMin minutes de plat
// et la voie redevient stable — chaque purge est jugée sur ses dernières minutes.
void test_B_la_marche_sort_de_la_fenetre(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  feedFlat(st, p, 1, 7.00f);
  feedFlat(st, p, kW - 1, 6.00f);
  TEST_ASSERT_TRUE(purgeStabilityEvaluate(st, p).unstable);
  feedFlat(st, p, 1, 6.00f);  // la minute à 7,00 est évincée
  const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.unstable);
  TEST_ASSERT_TRUE(v.stable);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, v.phRange);
}

// reset() ramène à « jamais lu » : aucune voie ne participe, ni stable ni
// instable (la porte attendra le plafond, fail-safe).
void test_B_reset_ramene_a_jamais_lu(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  feedFlat(st, p, kW, 7.00f, 650.0f);
  TEST_ASSERT_TRUE(purgeStabilityEvaluate(st, p).stable);
  purgeStabilityReset(st);
  const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);
  TEST_ASSERT_FALSE(v.anyFull);
  TEST_ASSERT_TRUE(isnan(v.phRange));
  TEST_ASSERT_TRUE(isnan(v.orpRange));
  TEST_ASSERT_TRUE(isnan(purgeChannelRawMin(st.ph)));
  TEST_ASSERT_EQUAL_UINT8(0, purgeChannelMinutes(st.ph));
}

// =============================================================================
// C — SIGNAUX SYNTHÉTIQUES
// =============================================================================

// Plat pur avec le bruit RÉEL des lectures (±0,18 sur le brut) : jamais
// instable sur 60 min, jamais stable avant la fenêtre pleine, et stable une
// bonne part du temps ensuite.
//
// Pourquoi « une bonne part » et non « toujours » : la médiane de 12 lectures
// uniformes ±0,18 a σ ≈ 0,052 ; l'étendue de W=10 médianes vaut en moyenne
// ≈ 3,1 σ ≈ 0,16, soit LE SEUIL : elle dépasse 0,15 environ une minute sur
// deux. Mesuré sur 21 graines (60 min) : 35 % à 90 % des minutes stables,
// jamais `unstable`. Ce clignotement est SANS conséquence une fois la porte
// ouverte : l'ouverture est un événement PONCTUEL — une seule minute jugée
// stable après le plancher suffit, la porte ne se referme ensuite que sur
// `unstable` (2×), jamais atteint ici.
//
// LIMITE CONNUE (à rapporter à pool-chemistry) : comme la fenêtre glisse d'une
// médiane à la fois, les minutes « > 0,15 » viennent en SÉRIES. Sur une eau
// parfaitement plate, la PREMIÈRE minute stable tombe à t+10 pour 12 graines
// sur 21, mais à t+13-19 pour 5, à t+22-29 pour 4 (pire cas t+29 : la porte
// s'ouvre 19 min après le plancher). Graine 20260904 : t+19, 29/51 (57 %).
// Bornes de non-régression : ≥ 40 % des minutes stables et une première minute
// stable au plus tard W+20.
void test_C_plat_bruite_jamais_instable_et_souvent_stable(void) {
  seed(20260904u);
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  int stables = 0, firstStable = -1;
  for (int m = 1; m <= 60; ++m) {
    feedFlat(st, p, 1, 6.80f, NAN, kPhNoise);
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[64];
    snprintf(msg, sizeof(msg), "t+%d : etendue %.3f", m, (double)v.phRange);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    if (m < kW) TEST_ASSERT_FALSE_MESSAGE(v.stable, msg);
    if (v.stable) {
      stables++;
      if (firstStable < 0) firstStable = m;
    }
  }
  const int evaluated = 60 - kW + 1;
  char msg[128];
  snprintf(msg, sizeof(msg), "plat bruite : %d/%d minutes stables (attendu >= 40 %%), 1re a t+%d",
           stables, evaluated, firstStable);
  TEST_ASSERT_TRUE_MESSAGE(stables * 10 >= evaluated * 4, msg);
  TEST_ASSERT_TRUE_MESSAGE(firstStable >= kW && firstStable <= kW + 20, msg);
}

// EXIGENCE SPEC (brief T1) : une dérive monotone franche n'est JAMAIS stable.
// Seuil de détection théorique : 0,15/(W−1) ≈ 0,017 pH/min. À 0,03 pH/min
// (0,3 pH / 10 min, l'ordre de grandeur d'une remontée de rinçage) l'étendue
// pleine vaut 0,27 : jamais stable ; à 0,05 pH/min (0,45) non plus.
void test_C_derive_monotone_jamais_stable(void) {
  const float slopes[2] = {0.03f, 0.05f};
  for (int k = 0; k < 2; ++k) {
    PurgeStabilityState st;
    purgeStabilityReset(st);
    const PurgeStabilityParams p = prodParams();
    const float slopePerSample = slopes[k] / kSpm;
    float ph = 6.00f;
    for (int m = 1; m <= 30; ++m) {
      for (int s = 0; s < kSpm; ++s) {
        purgeStabilityAddSample(st, p, ph, NAN);
        ph += slopePerSample;
      }
      const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
      char msg[96];
      snprintf(msg, sizeof(msg), "t+%d : derive %.3f pH/min jugee stable (etendue %.3f)",
               m, (double)slopes[k], (double)v.phRange);
      TEST_ASSERT_FALSE_MESSAGE(v.stable, msg);
      if (m >= kW) {
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, slopes[k] * (kW - 1), v.phRange, msg);
      }
    }
  }
}

// LIMITE CONNUE, acceptée par pool-chemistry (consignée dans l'ADR) : une dérive
// LENTE de 0,015 pH/min (0,15 pH / 10 min) donne une étendue pleine de 0,135
// < 0,15 → elle EST jugée stable. Le détecteur ne voit pas les pentes
// ≤ 0,017 pH/min ; en contrepartie une telle eau est déjà « quasi rincée » (le
// terme intégral n'en souffre pas : 0,15 pH sur 10 min ≈ 1 % de son autorité).
// Ce test FIGE la limite : si elle bouge (seuil, fenêtre), on le saura.
void test_C_derive_lente_0015_est_jugee_stable_limite_connue(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  const float slope = 0.015f;
  float ph = 6.00f;
  for (int m = 1; m <= 30; ++m) {
    for (int s = 0; s < kSpm; ++s) {
      purgeStabilityAddSample(st, p, ph, NAN);
      ph += slope / kSpm;
    }
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[96];
    snprintf(msg, sizeof(msg), "t+%d : etendue %.3f", m, (double)v.phRange);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    if (m >= kW) {
      TEST_ASSERT_TRUE_MESSAGE(v.stable, msg);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 0.135f, v.phRange, msg);
    }
  }
}

// MUTANT 1 — HYSTÉRÉSIS 1×/2× : une étendue de 0,225 pH (entre le seuil
// d'ouverture 0,15 et le seuil de re-fermeture 0,30) n'est NI stable NI
// instable. Si la re-fermeture se faisait à 1× (0,15), un simple retour au
// bruit d'ouverture refermerait la porte.
void test_C_hysteresis_entre_1x_et_2x(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  const float mid = 0.5f * (kPurgeStablePhRange + kPurgeRecloseRangePh);  // 0,225
  feedFlat(st, p, kW - 1, 7.00f);
  feedFlat(st, p, 1, 7.00f + mid);
  PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, mid, v.phRange);
  TEST_ASSERT_TRUE(v.anyFull);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);

  // Frontières : juste sous 0,15 stable ; > 0,30 instable ; 0,30 pile : ni l'un
  // ni l'autre. (Marge 0,005 : 7,00f + 0,15f − 7,00f n'est pas exactement 0,15f.)
  purgeStabilityReset(st);
  feedFlat(st, p, kW - 1, 7.00f);
  feedFlat(st, p, 1, 7.00f + kPurgeStablePhRange - 0.005f);
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_TRUE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);

  purgeStabilityReset(st);
  feedFlat(st, p, kW - 1, 7.00f);
  feedFlat(st, p, 1, 7.00f + kPurgeRecloseRangePh - 0.005f);  // 0,295 : dans la bande morte
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);

  purgeStabilityReset(st);
  feedFlat(st, p, kW - 1, 7.00f);
  feedFlat(st, p, 1, 7.00f + kPurgeRecloseRangePh + 0.01f);
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_TRUE(v.unstable);

  // Même hystérésis sur l'ORP.
  purgeStabilityReset(st);
  feedFlat(st, p, kW - 1, 7.00f, 650.0f);
  feedFlat(st, p, 1, 7.00f, 650.0f + 0.5f * (kPurgeStableOrpRange + kPurgeRecloseRangeOrp));
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);
}

// MUTANT 11 — MÉDIANE vs MOYENNE : plat 6,80 ± 0,18 avec UNE pointe isolée à
// +3,0 pH (glitch EZO, 9,80) dans une minute sur deux, 20 minutes. La
// médiane-minute ne bouge pas → stable dès la fenêtre pleine ; une moyenne
// décalerait les minutes impaires de +0,25 → étendue > 0,15 → jamais stable.
// (Graine choisie telle que le plat bruité seul soit stable sur ces minutes.)
void test_C_pointes_isolees_ne_cassent_pas_la_stabilite(void) {
  seed(777u);
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  int stables = 0;
  for (int m = 0; m < 20; ++m) {
    for (int s = 0; s < kSpm; ++s) {
      float ph = 6.80f + noise(kPhNoise);
      if ((m % 2 == 1) && s == 6) ph = 9.80f;  // pointe isolée
      purgeStabilityAddSample(st, p, ph, NAN);
    }
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[96];
    snprintf(msg, sizeof(msg), "t+%d : etendue %.3f", m + 1, (double)v.phRange);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    // Une moyenne donnerait une étendue ≥ 0,25 à chaque minute pleine.
    if (m + 1 >= kW) {
      TEST_ASSERT_TRUE_MESSAGE(v.phRange < 0.25f, msg);
      stables += v.stable ? 1 : 0;
    }
  }
  // Même exigence que le plat bruité : le glitch ne coûte pas de stabilité.
  const int evaluated = 20 - kW + 1;
  char msg[96];
  snprintf(msg, sizeof(msg), "pointes isolees : %d/%d minutes stables (attendu >= 40 %%)",
           stables, evaluated);
  TEST_ASSERT_TRUE_MESSAGE(stables * 10 >= evaluated * 4, msg);
}

// =============================================================================
// D — AGRÉGATION DES VOIES
// =============================================================================

// pH parfaitement plat, ORP qui alterne ±20 mV d'une minute à l'autre (étendue
// 40 mV > 15) : NON stable (une voie lue et agitée bloque) ET instable (40 > 30).
void test_D_orp_seul_instable_bloque(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  for (int m = 0; m < kW + 2; ++m) {
    feedFlat(st, p, 1, 7.00f, (m % 2 == 0) ? 630.0f : 670.0f);
  }
  const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, v.phRange);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 40.0f, v.orpRange);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_TRUE(v.unstable);

  // ORP agité mais SOUS 2× (20 mV) : bloque l'ouverture sans refermer.
  purgeStabilityReset(st);
  for (int m = 0; m < kW + 2; ++m) {
    feedFlat(st, p, 1, 7.00f, (m % 2 == 0) ? 640.0f : 660.0f);
  }
  const PurgeStabilityVerdict w = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(w.stable);
  TEST_ASSERT_FALSE(w.unstable);
}

// ORP JAMAIS lu (module absent, sonde débranchée : NaN permanent) : la voie est
// ignorée, le pH décide seul — stable à t+W.
void test_D_orp_jamais_lu_est_ignore(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  feedFlat(st, p, kW, 7.00f, NAN);
  const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_TRUE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);
  TEST_ASSERT_TRUE(isnan(v.orpRange));
  TEST_ASSERT_TRUE(isnan(purgeChannelRawMin(st.orp)));
  // Symétrique : pH jamais lu, ORP décide seul.
  purgeStabilityReset(st);
  feedFlat(st, p, kW, NAN, 650.0f);
  TEST_ASSERT_TRUE(purgeStabilityEvaluate(st, p).stable);
}

// ORP PARTIEL : lu la 1re minute puis muet (NaN) pendant W min → lu au moins une
// fois mais fenêtre incomplète → NON stable (pas d'ouverture sur une preuve
// partielle) ET non instable (une voie incomplète ne referme jamais). Quand la
// voie reprend et remplit sa fenêtre, la stabilité revient.
void test_D_orp_partiel_bloque_sans_refermer(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  feedFlat(st, p, 1, 7.00f, 650.0f);
  feedFlat(st, p, kW, 7.00f, NAN);  // pH : W+1 médianes ; ORP : 1
  PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_TRUE(v.anyFull);   // pH plein
  TEST_ASSERT_FALSE(v.stable);   // ORP incomplet bloque
  TEST_ASSERT_FALSE(v.unstable);
  TEST_ASSERT_EQUAL_UINT8(1, purgeChannelMinutes(st.orp));
  // Reprise ORP, valeur très différente de la 1re minute (700 vs 650) : quand
  // la fenêtre ORP est pleine, l'étendue 50 mV > 30 → instable.
  feedFlat(st, p, kW - 2, 7.00f, 700.0f);
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);  // W−1 médianes ORP : encore partiel
  feedFlat(st, p, 1, 7.00f, 700.0f);
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_TRUE(v.unstable);   // W médianes ORP : 650 puis 700… → 50 mV
  // Puis la marche sort de l'anneau ORP : stable sur les deux voies.
  feedFlat(st, p, 1, 7.00f, 700.0f);
  v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_TRUE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);
}

// Aucune voie jamais lue : ni stable ni instable, anyFull faux (la porte attend
// le plafond — fail-safe).
void test_D_aucune_voie_lue_ni_stable_ni_instable(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  for (int s = 0; s < 10 * kSpm; ++s) purgeStabilityAddSample(st, p, NAN, NAN);
  const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
  TEST_ASSERT_FALSE(v.stable);
  TEST_ASSERT_FALSE(v.unstable);
  TEST_ASSERT_FALSE(v.anyFull);
}

// Condition 3 pool-chemistry — la voie ORP SEULE doit tenir la porte fermée :
// pH parfaitement plat, ORP en pente 3 mV/min pendant 12 min → étendue pleine
// 27 mV > 15 → jamais stable (et 27 < 30 : pas instable non plus, bande morte).
void test_D_orp_en_pente_3mv_min_jamais_stable(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  const float slope = 3.0f;
  float orp = 600.0f;
  for (int m = 1; m <= 12; ++m) {
    for (int s = 0; s < kSpm; ++s) {
      purgeStabilityAddSample(st, p, 7.00f, orp);
      orp += slope / kSpm;
    }
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[96];
    snprintf(msg, sizeof(msg), "t+%d : etendue ORP %.1f mV", m, (double)v.orpRange);
    TEST_ASSERT_FALSE_MESSAGE(v.stable, msg);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    if (m >= kW) {
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, slope * (kW - 1), v.orpRange, msg);
      TEST_ASSERT_TRUE_MESSAGE(v.orpRange > kPurgeStableOrpRange, msg);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.0f, v.phRange, msg);
    }
  }
}

// LIMITE CONNUE (symétrique du pH) : l'ORP seul ne détecte pas les pentes
// ≤ 15/(W−1) ≈ 1,6 mV/min. À 1,5 mV/min (étendue 13,5 mV) la voie est jugée
// stable. Figé pour que tout déplacement de la limite soit visible.
void test_D_orp_en_pente_1_5mv_min_est_juge_stable_limite_connue(void) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  const float slope = 1.5f;
  float orp = 600.0f;
  for (int m = 1; m <= 12; ++m) {
    for (int s = 0; s < kSpm; ++s) {
      purgeStabilityAddSample(st, p, 7.00f, orp);
      orp += slope / kSpm;
    }
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[96];
    snprintf(msg, sizeof(msg), "t+%d : etendue ORP %.1f mV", m, (double)v.orpRange);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    if (m >= kW) {
      TEST_ASSERT_TRUE_MESSAGE(v.stable, msg);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 13.5f, v.orpRange, msg);
    }
  }
}

// =============================================================================
// E — REJEUX DES COURBES RÉELLES
// =============================================================================
// Courbe du 04/09/2026 (journal + enregistrement /data à 60 s, brut pH) :
//   06:55-06:57  6,63 plat (eau stagnante — DÉJÀ stable à t0)
//   06:58 (t+1,5 → t+3)  plongeon 6,63 → 5,82
//   07:04 (t+9)          fond 5,54
//   07:13 (t+18) 5,93 → 07:22 (t+27) 6,35 → 07:34 (t+39) 6,86
//   ensuite plat 6,85-6,91
// ORP : −60 mV synchrone du plongeon, retour progressif jusqu'à t+39.
//
// FOND DU V (t+3 → t+18) : pentes −0,047 pH/min puis +0,043 pH/min. La fenêtre
// centrée sur le fond voit ≈ s·(W−1)/2 de chaque côté : W=4 → 0,07 (stable à
// tort à t+7), W=8 → 0,145 (stable à tort à t+13, cf. test dédié), W=10 →
// 0,19-0,21 > 0,15 → jamais stable dans le fond. La courbe est FIGÉE : elle
// est la référence de calibrage de W, ne pas la retoucher.
static float ph0409(float m) {
  if (m < 1.5f) return 6.63f;
  if (m < 3.0f) return lerp(6.63f, 5.82f, (m - 1.5f) / 1.5f);
  if (m < 9.0f) return lerp(5.82f, 5.54f, (m - 3.0f) / 6.0f);
  if (m < 18.0f) return lerp(5.54f, 5.93f, (m - 9.0f) / 9.0f);
  if (m < 27.0f) return lerp(5.93f, 6.35f, (m - 18.0f) / 9.0f);
  if (m < 39.0f) return lerp(6.35f, 6.86f, (m - 27.0f) / 12.0f);
  return 6.88f + 0.03f * sinf(m);  // plat 6,85-6,91
}
static float orp0409(float m) {
  if (m < 1.5f) return 650.0f;
  if (m < 3.0f) return lerp(650.0f, 590.0f, (m - 1.5f) / 1.5f);
  if (m < 18.0f) return 590.0f;
  if (m < 39.0f) return lerp(590.0f, 650.0f, (m - 18.0f) / 21.0f);
  return 650.0f;
}

// Rejoue la courbe du 04/09 pendant `minutes` avec les paramètres `p` et le
// bruit demandé (ORP NaN si orpNoise < 0). Retourne la 1re minute stable (−1 si
// aucune) et, si elle précède t+40 (fausse ouverture dans le V), la 1re minute
// `unstable` qui suit (re-fermeture C1), −1 si aucune.
struct Replay0409 { int firstStable; int recloseAfter; };
static Replay0409 replay0409(const PurgeStabilityParams& p, int minutes, float phNoise,
                             float orpNoise) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  Replay0409 r = {-1, -1};
  for (int s = 0; s < minutes * kSpm; ++s) {
    const float m = s / (float)kSpm;
    const float orp = (orpNoise < 0.0f) ? NAN : orp0409(m) + noise(orpNoise);
    purgeStabilityAddSample(st, p, ph0409(m) + noise(phNoise), orp);
    if ((s + 1) % kSpm != 0) continue;
    const int minute = (s + 1) / kSpm;
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    if (v.stable && r.firstStable < 0) r.firstStable = minute;
    if (r.firstStable > 0 && r.firstStable < 40 && v.unstable && r.recloseAfter < 0) {
      r.recloseAfter = minute;
    }
  }
  return r;
}

// EXIGENCE SPEC feature-091 (critère d'acceptation « la lecture stable de l'eau
// stagnante à t+0 n'ouvre pas la porte » + note du 04/09 « la porte doit rester
// fermée tant que la mesure bouge », « tenir sur ~50 min de rinçage ») :
//   - jamais stable avant t+W (fenêtre pleine) ;
//   - stable FAUX pendant tout le plongeon ET la remontée (t ≤ 39) ;
//   - stable VRAI une fois le plat atteint (première minute stable ≥ 40 ;
//     mesuré t+47-53 selon la graine, jamais au-delà du plafond 90 min).
// La première minute jugée stable est rapportée dans le message d'échec : c'est
// l'instant où la porte s'ouvrirait (plancher 10 min déjà écoulé).
void test_E_rejeu_0409_pas_stable_avant_la_fin_du_rincage(void) {
  seed(20260904u);
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  int firstStable = -1;
  float phAtFirstStable = NAN;
  for (int s = 0; s < 90 * kSpm; ++s) {
    const float m = s / (float)kSpm;
    purgeStabilityAddSample(st, p, ph0409(m) + noise(kPhNoise), orp0409(m) + noise(kOrpNoise));
    if ((s + 1) % kSpm != 0) continue;
    const int minute = (s + 1) / kSpm;
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    if (minute < kW) {
      TEST_ASSERT_FALSE(v.stable);
      TEST_ASSERT_FALSE(v.unstable);
    }
    if (v.stable && firstStable < 0) {
      firstStable = minute;
      phAtFirstStable = ph0409(m);
    }
  }
  char msg[160];
  snprintf(msg, sizeof(msg),
           "premiere minute stable : t+%d (pH brut %.2f) — attendu dans [40 ; 90], "
           "apres la fin de la remontee",
           firstStable, (double)phAtFirstStable);
  TEST_ASSERT_TRUE_MESSAGE(firstStable >= 40, msg);
  TEST_ASSERT_TRUE_MESSAGE(firstStable <= 90, msg);
}

// Le même rejeu SANS bruit : montre que la conclusion ne dépend pas du tirage.
// Le plongeon (t+1,5 → t+9) et la remontée (t+9 → t+39) sont des mouvements de
// 0,28 à 0,81 pH ; aucune de ces minutes ne doit être jugée stable. Mesuré :
// t+47 (pH 6,88), sans aucune re-fermeture ensuite.
void test_E_rejeu_0409_sans_bruit_meme_conclusion(void) {
  const Replay0409 r = replay0409(prodParams(), 60, 0.0f, 0.0f);
  char msg[128];
  snprintf(msg, sizeof(msg), "sans bruit : premiere minute stable t+%d (pH %.2f)",
           r.firstStable, (double)ph0409((float)r.firstStable));
  TEST_ASSERT_TRUE_MESSAGE(r.firstStable >= 40, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.firstStable <= 60, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, r.recloseAfter, msg);
}

// Condition 6 pool-chemistry — DÉMONSTRATION FIGÉE : avec une fenêtre de 8 min
// (paramètres LOCAUX, pas ceux de production), la même courbe sans bruit ouvre
// À TORT dans le fond du V à t+13 (pH brut 5,71, étendue 0,145 ≤ 0,15) puis se
// referme à t+17 (0,303 > 0,30). C'est la raison du passage W=8 → W=10 : ce
// test échouera si quelqu'un « améliore » la courbe ou le calcul de l'étendue
// au point de masquer le phénomène.
void test_E_demonstration_w8_ouvre_a_tort_dans_le_fond_du_v(void) {
  PurgeStabilityParams p8 = prodParams();
  p8.windowMin = 8;
  const Replay0409 r = replay0409(p8, 60, 0.0f, 0.0f);
  char msg[128];
  snprintf(msg, sizeof(msg), "W=8 : premiere minute stable t+%d (pH %.2f), refermeture t+%d",
           r.firstStable, (double)ph0409((float)r.firstStable), r.recloseAfter);
  TEST_ASSERT_EQUAL_INT_MESSAGE(13, r.firstStable, msg);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 5.71f, ph0409(13.0f), msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(17, r.recloseAfter, msg);
  // Et W=10 (production) sur la même courbe : rien avant t+40.
  const Replay0409 r10 = replay0409(prodParams(), 60, 0.0f, 0.0f);
  TEST_ASSERT_TRUE(r10.firstStable >= 40);
}

// Condition 8 pool-chemistry — 5 graines, bruit réel (pH ±0,18, ORP ±5 mV),
// courbe du 04/09 prolongée sur 90 min = kPurgeCapMs. Pour chaque graine :
//   - soit la porte s'ouvre par stabilité (1re minute stable ≤ 90) ;
//   - soit le plafond prend le relais (kPurgeCapMs) — toléré pour ≤ 1 graine.
// Une ouverture AVANT t+40 serait une fausse ouverture dans le V : elle doit
// alors être refermée par C1 (unstable) dans les 6 minutes. Mesuré (graines
// 1..5 × 7919 + 20260904) : t+48, 53, 48, 48, 56 — aucune fausse ouverture.
// Sur 20 graines : 0 sans ouverture, 0 fausse ouverture avec ORP ; avec le pH
// SEUL (ORP NaN), 1 graine sur 20 ouvre à tort à t+14 (cf. test suivant).
void test_E_rejeu_0409_5_graines_ouvrent_ou_plafond(void) {
  TEST_ASSERT_EQUAL_UINT32(90UL * 60000UL, kPurgeCapMs);
  int sansOuverture = 0;
  char dist[160] = {0};
  for (uint32_t k = 1; k <= 5; ++k) {
    seed(k * 7919u + 20260904u);
    const Replay0409 r = replay0409(prodParams(), 90, kPhNoise, kOrpNoise);
    char part[24];
    snprintf(part, sizeof(part), " g%u:t+%d", (unsigned)k, r.firstStable);
    strncat(dist, part, sizeof(dist) - strlen(dist) - 1);
    if (r.firstStable < 0) {
      sansOuverture++;
      continue;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "graine %u : ouverture a t+%d, refermeture t+%d",
             (unsigned)k, r.firstStable, r.recloseAfter);
    TEST_ASSERT_TRUE_MESSAGE(r.firstStable >= kW, msg);
    if (r.firstStable < 40) {
      // Fausse ouverture dans le V : C1 doit la rattraper vite.
      TEST_ASSERT_TRUE_MESSAGE(r.recloseAfter > 0, msg);
      TEST_ASSERT_TRUE_MESSAGE(r.recloseAfter - r.firstStable <= 6, msg);
    }
  }
  char msg[200];
  snprintf(msg, sizeof(msg), "graines sans ouverture en 90 min : %d (tolere <= 1) —%s",
           sansOuverture, dist);
  TEST_ASSERT_TRUE_MESSAGE(sansOuverture <= 1, msg);
  printf("  [info] distribution W=10 bruite (1re minute stable) :%s\n", dist);
}

// LIMITE CONNUE avec bruit : le pH SEUL (ORP absent), bruit ±0,18, graine 6 →
// le tirage aplatit le fond du V juste assez pour une fausse ouverture à t+14
// (pH brut 5,76). La re-fermeture C1 (`unstable`, 2×) la rattrape à t+19 :
// c'est exactement le filet de sécurité prévu par la spec pour ce cas. Figé
// pour que la fenêtre d'exposition (5 min) reste visible et bornée.
void test_E_rejeu_0409_bruit_fausse_ouverture_refermee_par_c1(void) {
  seed(6u * 7919u + 20260904u);
  const Replay0409 r = replay0409(prodParams(), 60, kPhNoise, -1.0f);
  char msg[128];
  snprintf(msg, sizeof(msg), "pH seul, graine 6 : ouverture t+%d (pH %.2f), refermeture t+%d",
           r.firstStable, (double)ph0409((float)r.firstStable), r.recloseAfter);
  TEST_ASSERT_TRUE_MESSAGE(r.firstStable >= kW && r.firstStable < 40, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.recloseAfter > r.firstStable, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.recloseAfter - r.firstStable <= 6, msg);
}

// Courbe du 02/09 (plongeon TARDIF) : brut plat 6,80 ± 0,18 pendant 11 min
// (≥ W+1 : la fenêtre est pleine et propre à t+W), puis chute à 5,60 en 1,5 min
// (comme le 04/09). C'est le test de C1 (re-fermeture) :
//   - stable VRAI à t+W (fenêtre pleine, plat) → la porte s'ouvre au plancher ;
//   - `unstable` FAUX tant que la chute n'a pas commencé (t ≤ 11) ;
//   - `unstable` VRAI au plus tard 2 min après le début de la chute (t+13).
void test_E_rejeu_0209_plongeon_tardif_declenche_unstable(void) {
  seed(1u);
  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  const float chuteDebut = 11.0f;
  int firstUnstable = -1;
  for (int s = 0; s < 20 * kSpm; ++s) {
    const float m = s / (float)kSpm;
    float ph;
    if (m < chuteDebut) ph = 6.80f;
    else if (m < chuteDebut + 1.5f) ph = lerp(6.80f, 5.60f, (m - chuteDebut) / 1.5f);
    else ph = 5.60f;
    purgeStabilityAddSample(st, p, ph + noise(kPhNoise), NAN);
    if ((s + 1) % kSpm != 0) continue;
    const int minute = (s + 1) / kSpm;
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    char msg[64];
    snprintf(msg, sizeof(msg), "t+%d : etendue %.3f", minute, (double)v.phRange);
    if (minute == kW) TEST_ASSERT_TRUE_MESSAGE(v.stable, msg);
    if (minute <= 11) TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    if (v.unstable && firstUnstable < 0) firstUnstable = minute;
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "unstable vu pour la 1re fois a t+%d (attendu <= 13)", firstUnstable);
  TEST_ASSERT_TRUE_MESSAGE(firstUnstable > 0, msg);
  TEST_ASSERT_TRUE_MESSAGE(firstUnstable <= 13, msg);
}

// Les paramètres de production tiennent dans les tampons statiques (sans quoi
// le module les bornerait silencieusement et la fenêtre réelle ne serait pas
// celle documentée). W = 10 : décision pool-chemistry du 04/09/2026.
void test_E_parametres_de_production_dans_les_bornes(void) {
  TEST_ASSERT_TRUE(kPurgeSamplesPerMin <= kPurgeStabilityMaxSamplesPerMin);
  TEST_ASSERT_TRUE(kPurgeWindowMin <= kPurgeStabilityMaxWindowMin);
  TEST_ASSERT_EQUAL_UINT8(10, kPurgeWindowMin);
  TEST_ASSERT_EQUAL_UINT8(12, kPurgeSamplesPerMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.15f, kPurgeStablePhRange);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 15.0f, kPurgeStableOrpRange);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f * kPurgeStablePhRange, kPurgeRecloseRangePh);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f * kPurgeStableOrpRange, kPurgeRecloseRangeOrp);
}

// =============================================================================
// F — FEATURE-097 : MOYENNE INTERQUARTILE (nouvel estimateur de bloc)
// =============================================================================
// Chiffres validés le 2026-09-20 en réexécutant l'algorithme RÉEL (pas une
// réimplémentation Python) : voir scratch de vérification en tête de rapport
// test-engineer. Toute valeur ci-dessous est mesurée, pas recopiée de la spec.

// --- Chargement de la fixture réelle -----------------------------------------
// test/fixtures/f097_purge_noise_20260920.csv — 300 échantillons uniques à 5 s,
// bruit réel pH/ORP sur eau établie (2026-09-20). `pio test -e native` exécute
// depuis la racine du dépôt, mais on tolère des préfixes de repli comme le fait
// déjà test_native_history_persist_format (readProjectFile).
struct NoiseFixture {
  float ph[300];
  float orp[300];
  int n;
};

static bool loadNoiseFixture(NoiseFixture& fx) {
  const char* prefixes[4] = {"", "../", "../../", "../../../"};
  char path[256];
  FILE* fp = NULL;
  for (int i = 0; i < 4 && fp == NULL; ++i) {
    snprintf(path, sizeof(path), "%stest/fixtures/f097_purge_noise_20260920.csv", prefixes[i]);
    fp = fopen(path, "rb");
  }
  if (fp == NULL) return false;
  fx.n = 0;
  char line[256];
  while (fgets(line, sizeof(line), fp) != NULL && fx.n < 300) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (strncmp(line, "t_ms", 4) == 0) continue;
    long t;
    float ph, orp, phf, orpf, temp;
    if (sscanf(line, "%ld,%f,%f,%f,%f,%f", &t, &ph, &orp, &phf, &orpf, &temp) == 6) {
      fx.ph[fx.n] = ph;
      fx.orp[fx.n] = orp;
      fx.n++;
    }
  }
  fclose(fp);
  return fx.n == 300;
}

// --- Canal "ombre" : rejoue la meme mecanique d'anneau que channelAdd() de
// purgeStabilityLogic.cpp, mais avec une fonction de statistique de bloc
// INJECTEE (median ou IQM) — sert a comparer les DEUX estimateurs sur le
// MEME flux, sans dupliquer l'etat de production. Utilise par F6 (C2b/c).
struct Shadow {
  float buf[kPurgeStabilityMaxSamplesPerMin];
  int cnt = 0;
  float blocks[kPurgeStabilityMaxWindowMin];
  int head = 0, count = 0;
};
static void shadowAdd(Shadow& ch, float v, int spm, int windowMin,
                      float (*stat)(const float*, uint8_t)) {
  if (isnan(v)) return;
  ch.buf[ch.cnt++] = v;
  if (ch.cnt >= spm) {
    const float s = stat(ch.buf, (uint8_t)ch.cnt);
    ch.cnt = 0;
    ch.blocks[ch.head] = s;
    ch.head = (ch.head + 1) % windowMin;
    if (ch.count < windowMin) ch.count++;
  }
}
static float shadowRange(const Shadow& ch) {
  if (ch.count == 0) return NAN;
  float lo = ch.blocks[0], hi = ch.blocks[0];
  for (int i = 1; i < ch.count; ++i) {
    if (ch.blocks[i] < lo) lo = ch.blocks[i];
    if (ch.blocks[i] > hi) hi = ch.blocks[i];
  }
  return hi - lo;
}

// -----------------------------------------------------------------------------
// F1 — C1 pool-chemistry : trim = max(1, n/4), jamais n/4 nu, pour n = 1..60.
// -----------------------------------------------------------------------------
// Conception : v[0] = 0.0 (aberrant bas), v[n-1] = 100.0 (aberrant haut),
// milieu constant à 7.0. Avec trim >= 1 (n >= 3), les DEUX aberrants sont
// systématiquement écartés (un par côté suffit) et le résultat reste 7.0,
// QUELLE QUE SOIT la valeur exacte de trim au-delà de 1 (les valeurs du milieu
// sont toutes égales) — la revue de code l'a prouvé algébriquement
// (n − 2·trim >= ⌈n/2⌉ >= 1 sur tout l'intervalle). Le cas n = 3 est LE cas
// critique : n/4 = 0 sans le max(1, ...), ce qui donnerait la MOYENNE SIMPLE
// (0 + 7 + 100)/3 = 35,67 — à des kilomètres de 7,0. n = 1, 2 retombent sur la
// médiane (déjà couverte en section A) : vérifiés ici pour la continuité de
// la boucle paramétrée. Balayage jusqu'à 60 (= kPurgeStabilityMaxSamplesPerMin,
// la borne autorisée par constants.h pour kPurgeSamplesPerMin), et pas
// seulement jusqu'à 12 : preuve par construction sur TOUT l'intervalle
// exploitable, pas seulement la valeur de production actuelle.
void test_F1_trim_max1_n_sur_4_pour_n_1_a_60(void) {
  for (int n = 1; n <= kPurgeStabilityMaxSamplesPerMin; ++n) {
    float v[kPurgeStabilityMaxSamplesPerMin];
    char msg[48];
    snprintf(msg, sizeof(msg), "n=%d", n);
    if (n <= 2) {
      for (int i = 0; i < n; ++i) v[i] = 7.0f + (float)i;
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, purgeStabilityMedian(v, (uint8_t)n),
                                       purgeStabilityBlockStat(v, (uint8_t)n), msg);
      continue;
    }
    v[0] = 0.0f;
    v[n - 1] = 100.0f;
    for (int i = 1; i < n - 1; ++i) v[i] = 7.0f;
    const float bs = purgeStabilityBlockStat(v, (uint8_t)n);
    // MUTANT « garde C1 » (trim = n/4 nu, sans max(1,.)) : a n=3, bs deviendrait
    // 35,67. A ne pas confondre avec le mutant (b) de la spec, qui est le
    // passage a la moyenne simple.
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 7.0f, bs, msg);
  }
}

// -----------------------------------------------------------------------------
// F2 — Bloc alterné RÉEL de la minute 22 (trace du 20/09) : la bascule vs la
// moyenne, à l'octet.
// -----------------------------------------------------------------------------
// Valeur IQM mesurée en réexécutant purgeStabilityBlockStat() sur ces 12
// valeurs exactes : 6,9673 (médiane : 7,0595 ≈ 7,059). Ce test fige la valeur
// RÉELLE du code — c'est elle qui fait foi pour toute non-régression future.
void test_F2_bloc_alterne_reel_minute_22(void) {
  const float bloc[12] = {7.051f, 6.747f, 7.083f, 6.752f, 7.083f, 6.743f,
                          7.085f, 6.775f, 7.079f, 6.749f, 7.079f, 7.068f};
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0595f, purgeStabilityMedian(bloc, 12));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.9673f, purgeStabilityBlockStat(bloc, 12));
  // La bascule de la médiane VS le lissage de l'IQM, sur la même donnée :
  // écart de 0,092, très supérieur à la précision flottante — c'est bien
  // l'estimateur qui change de comportement, pas un artefact d'arrondi.
  TEST_ASSERT_TRUE(purgeStabilityMedian(bloc, 12) - purgeStabilityBlockStat(bloc, 12) > 0.05f);
}

// -----------------------------------------------------------------------------
// F3 — Rejeu du PLATEAU RÉEL (fixture 20/09) à travers le pipeline complet
// (purgeStabilityAddSample + purgeStabilityEvaluate, comme en production).
// -----------------------------------------------------------------------------
// MUTANT (a) IQM → médiane — MUTANT CRITIQUE : sans lui, rien ne protège le
// correctif. Avec l'IQM, 16/16 fenêtres ouvrent ; avec la médiane, 13/16
// seulement (mesuré indépendamment en Python et vérifié par une seconde
// implémentation ici). Ce test bascule de vert à rouge si channelAdd()
// revient à purgeStabilityMedian().
// MUTANT (c) trim = n/4 → n/6 : ne change PAS le compte 16/16 sur cette
// fixture (le bruit y est trop loin du seuil pour que ça bascule un booléen),
// mais RESSERRE l'étendue max mesurée de 0,089 à 0,074 — d'où le verrou sur
// la valeur numérique, pas seulement sur le comptage.
void test_F3_rejeu_plateau_reel_16_sur_16(void) {
  NoiseFixture fx;
  TEST_ASSERT_TRUE_MESSAGE(loadNoiseFixture(fx),
      "fixture test/fixtures/f097_purge_noise_20260920.csv introuvable ou incomplete (300 attendus)");

  PurgeStabilityState st;
  purgeStabilityReset(st);
  const PurgeStabilityParams p = prodParams();
  int stableCount = 0, evaluated = 0;
  float maxPhRange = 0.0f, maxOrpRange = 0.0f;
  for (int i = 0; i < fx.n; ++i) {
    purgeStabilityAddSample(st, p, fx.ph[i], fx.orp[i]);
    if ((i + 1) % kSpm != 0) continue;
    const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
    if (!v.anyFull) continue;
    evaluated++;
    if (v.stable) stableCount++;
    if (v.phRange > maxPhRange) maxPhRange = v.phRange;
    if (!isnan(v.orpRange) && v.orpRange > maxOrpRange) maxOrpRange = v.orpRange;
    char msg[64];
    snprintf(msg, sizeof(msg), "fenetre %d : eau etablie ne doit jamais refermer", evaluated);
    TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
  }
  char msg[160];
  snprintf(msg, sizeof(msg), "%d/%d fenetres stables (etendue pH max %.4f, ORP max %.2f mV)",
           stableCount, evaluated, (double)maxPhRange, (double)maxOrpRange);
  TEST_ASSERT_EQUAL_INT_MESSAGE(16, evaluated, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(16, stableCount, msg);
  // Verrou numerique (tolerance large pour le passage float32, assez fine pour
  // distinguer du mutant n/6 [0,074] et de la mediane [0,192]).
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.004f, 0.0893f, maxPhRange, msg);
  TEST_ASSERT_TRUE_MESSAGE(maxOrpRange <= 15.0f, msg);
}

// -----------------------------------------------------------------------------
// F4 — C3 pool-chemistry : 4 lectures aberrantes sur 12 -> jamais stable,
// dans les deux directions (14,0 et 0,0). 0 a 3 -> verdict inchange.
// -----------------------------------------------------------------------------
// Conception : baseline plate 7,00 sur 9 blocs (fenetre pleine), PUIS un seul
// bloc contamine par k lectures a la valeur aberrante (les 12 - k autres a
// 7,00). Avec trim = max(1, 12/4) = 3 : k <= 3 aberrantes sont TOUTES
// absorbees par le trim (au plus 3 ecartees d'un cote) -> bloc inchange a
// 7,00 pile, verdict INCHANGE (stable). A k = 4, une aberrante deborde du
// trim et contamine la moyenne du milieu -> le bloc derive de +/-1,17 pH,
// bien au-dela du seuil de re-fermeture (0,30) -> unstable. C'est exactement
// le "decrochage a 4/12" du gate pool-chemistry, mesure sur le code reel :
// k=0..3 -> phRange=0 (stable) ; k=4 -> phRange=1,1667 (unstable).
// MUTANT (b) IQM -> moyenne simple : a k=1 deja, une moyenne deplacerait le
// bloc de (14,0-7,0)/12 = 0,58 -> phRange >> 0,30 -> unstable des k=1, alors
// que le comportement attendu est "verdict inchange jusqu'a k=3". Ce test
// tue donc directement le mutant (b).
static PurgeStabilityVerdict replayContaminatedBlock(const PurgeStabilityParams& p,
                                                     float aberrant, int k) {
  PurgeStabilityState st;
  purgeStabilityReset(st);
  for (int m = 0; m < kW - 1; ++m) feedFlat(st, p, 1, 7.00f);
  for (int s = 0; s < kSpm; ++s) {
    const float v = (s < k) ? aberrant : 7.00f;
    purgeStabilityAddSample(st, p, v, NAN);
  }
  return purgeStabilityEvaluate(st, p);
}
void test_F4_aberrantes_0_a_3_inchange_4_decroche_deux_directions(void) {
  const float directions[2] = {14.0f, 0.0f};
  const PurgeStabilityParams p = prodParams();
  for (int d = 0; d < 2; ++d) {
    for (int k = 0; k <= 3; ++k) {
      const PurgeStabilityVerdict v = replayContaminatedBlock(p, directions[d], k);
      char msg[96];
      snprintf(msg, sizeof(msg), "direction %.1f, k=%d : verdict devrait etre INCHANGE (stable)",
               (double)directions[d], k);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f, v.phRange, msg);
      TEST_ASSERT_TRUE_MESSAGE(v.stable, msg);
      TEST_ASSERT_FALSE_MESSAGE(v.unstable, msg);
    }
    const PurgeStabilityVerdict v4 = replayContaminatedBlock(p, directions[d], 4);
    char msg[96];
    snprintf(msg, sizeof(msg), "direction %.1f, k=4 : DOIT decrocher (jamais stable)",
             (double)directions[d]);
    TEST_ASSERT_FALSE_MESSAGE(v4.stable, msg);
    TEST_ASSERT_TRUE_MESSAGE(v4.phRange > 1.0f, msg);
  }
}

// -----------------------------------------------------------------------------
// F5 — C4 pool-chemistry : re-fermeture, 0,035 pH/min referme, 0,030 ne
// referme pas.
// -----------------------------------------------------------------------------
// Etablit d'abord une fenetre STABLE (plat 10 min), puis applique une derive
// monotone continue et cherche le premier instant `unstable`. Mesure sur le
// code reel : 0,030 -> jamais unstable en 15 min (etendue plafonne a 0,27,
// sous le seuil 0,30) ; 0,035 -> unstable des la 1re fenetre pleine suivant le
// debut de la derive (etendue 0,315 > 0,30), conforme au static_assert de
// constants.h (kPurgeMinDetectedDriftPhPerMin = 0,035).
// A NOTER (acte, pas corrige) : la re-fermeture REELLE observee sur le
// terrain se declenche vers 0,0333 pH/min (§B des releves terrain, ecarts
// 0,300 a 0,382 juste au-dessus du seuil) — plus bas que la limite theorique
// 0,035 exigee par C4. Cet ecart est du au BRUIT qui s'ajoute a la derive
// reelle et "assiste" le franchissement du seuil ; sur une derive VRAIE et
// PARFAITEMENT LISSE (ce test), la sensibilite mesuree est exactement celle
// du static_assert, pas celle, plus fine, vue sur le terrain bruite. Les
// declenchements "assistes par le bruit" sous 0,0333 restent donc PERDUS par
// une derive lisse en dessous de 0,035 — comportement voulu, pas un defaut.
void test_F5_reclose_0035_referme_0030_ne_referme_pas(void) {
  const PurgeStabilityParams p = prodParams();
  const float slopes[2] = {0.030f, 0.035f};
  const bool expectUnstable[2] = {false, true};
  for (int k = 0; k < 2; ++k) {
    PurgeStabilityState st;
    purgeStabilityReset(st);
    feedFlat(st, p, kW, 7.00f);
    TEST_ASSERT_TRUE(purgeStabilityEvaluate(st, p).stable);
    float ph = 7.00f;
    bool unstableSeen = false;
    int minuteUnstable = -1;
    for (int m = 1; m <= 15; ++m) {
      for (int s = 0; s < kSpm; ++s) {
        purgeStabilityAddSample(st, p, ph, NAN);
        ph += slopes[k] / kSpm;
      }
      const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
      if (v.unstable && !unstableSeen) { unstableSeen = true; minuteUnstable = m; }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "pente %.3f pH/min : unstable=%d (attendu %d), vu a t+%d",
             (double)slopes[k], unstableSeen, expectUnstable[k], minuteUnstable);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expectUnstable[k] ? 1 : 0, unstableSeen ? 1 : 0, msg);
  }
}

// -----------------------------------------------------------------------------
// F6 — C2 pool-chemistry (AMENDE le 2026-09-20 apres revue) : rejeu du fond en
// V du 04/09 AVEC LE BRUIT REEL DE LA FIXTURE.
// -----------------------------------------------------------------------------
// HISTORIQUE DE L'AMENDEMENT (a lire avant de retoucher ce test) :
//   La version initiale de C2(b) exigeait "etendue IQM >= etendue mediane au
//   fond du V" (dominance d'etendue). Mesuree a 8/30 (27%), PAS a 30/30 —
//   remontee a pool-chemistry, qui a reconnu l'erreur de formulation : un
//   bruit plus FORT gonfle l'etendue et masque la deflation dangereuse ; la
//   mediane ne "dominait" que par accident (plus bruyante), pas par securite.
//   La grandeur de surete est DETERMINISTE et INDEPENDANTE de l'estimateur :
//   la marge du V (0,2025 contre le seuil 0,15, soit 0,0525) ne se comprime
//   QUE si le bruit ampute cette marge — d'autant plus rare que sigma est
//   petit (IQM : 0,0263 ; mediane : 0,0458). D'ou le nouveau C2(b) : comparer
//   le TAUX DE FAUSSE OUVERTURE, pas l'etendue.
// C2(a) — ACCEPTE NON BLOQUANT : parite avec l'existant (voir mesure
//   ci-dessous), et la fenetre exposee est le fond du V a pH BAS ou l'acide
//   est de toute facon refuse par la butee 6,20 (fail-closed) ; seul l'ORP
//   peut doser, deja borne par ses limites par injection et horaires.
// C2(b) NOUVEAU — taux de fausse ouverture, balayage EXHAUSTIF des 300
//   dephasages possibles (pas un echantillon de 30) : IQM <= mediane.
//   Mesure : IQM 2/300 (dephasages 169, 170 — meme evenement physique a 5 s
//   pres), mediane (canal "ombre", meme pipeline pH+ORP, calcule en parallele
//   sur le meme flux bruite) 2/300 (dephasages 85, 87) -> EGALITE, donc
//   IQM <= mediane est VRAIE (au pire a egalite, jamais pire).
// C2(c) NOUVEAU — exposition maximale d'une fausse ouverture bornee par
//   kPurgeWindowMin (10 min) : mesuree a 5 min (dephasage 170). Sentinelle de
//   derive de la borne, pas seulement du fait qu'elle soit "rattrapee".
// Dominance d'etendue (8/30) CONSERVEE en sentinelle diagnostique (pas un
// critere d'acceptation) : un saut a 30/30 signale toujours le mutant (a)
// IQM -> mediane (les deux canaux deviennent identiques).
static bool shadowMedianVerdict(Shadow& chPh, Shadow& chOrp, const PurgeStabilityParams& p,
                                float phv, float orpv, bool* outUnstable) {
  shadowAdd(chPh, phv, kSpm, kW, purgeStabilityMedian);
  shadowAdd(chOrp, orpv, kSpm, kW, purgeStabilityMedian);
  const bool phFull = chPh.count >= kW, orpFull = chOrp.count >= kW;
  const float phRange = shadowRange(chPh), orpRange = shadowRange(chOrp);
  const bool stable = phFull && orpFull && phRange <= p.phStableRange && orpRange <= p.orpStableRange;
  *outUnstable = (phFull && phRange > p.phRecloseRange) || (orpFull && orpRange > p.orpRecloseRange);
  return stable;
}
void test_F6_c2_taux_fausse_ouverture_et_exposition_max(void) {
  NoiseFixture fx;
  TEST_ASSERT_TRUE_MESSAGE(loadNoiseFixture(fx),
      "fixture test/fixtures/f097_purge_noise_20260920.csv introuvable ou incomplete (300 attendus)");
  double sumPh = 0.0, sumOrp = 0.0;
  for (int i = 0; i < fx.n; ++i) { sumPh += fx.ph[i]; sumOrp += fx.orp[i]; }
  const float meanPh = (float)(sumPh / fx.n), meanOrp = (float)(sumOrp / fx.n);
  float resPh[300], resOrp[300];
  for (int i = 0; i < fx.n; ++i) { resPh[i] = fx.ph[i] - meanPh; resOrp[i] = fx.orp[i] - meanOrp; }

  const PurgeStabilityParams p = prodParams();

  // --- C2(b) : balayage EXHAUSTIF des 300 dephasages, IQM (production) vs
  // mediane (canal ombre pH+ORP), meme flux bruite. --------------------------
  int falseOpensIqm = 0, falseOpensMed = 0, maxExposureIqm = -1;
  char worstMsg[160] = {0};
  for (int phase = 0; phase < fx.n; ++phase) {
    // IQM, via le pipeline de PRODUCTION reel (integration, pas une copie).
    {
      PurgeStabilityState st;
      purgeStabilityReset(st);
      int idx = phase;
      int firstStable = -1, recloseAfter = -1;
      for (int s = 0; s < 45 * kSpm; ++s) {
        const float m = (float)s / kSpm;
        const float phv = ph0409(m) + resPh[idx % fx.n];
        const float orpv = orp0409(m) + resOrp[idx % fx.n];
        idx++;
        purgeStabilityAddSample(st, p, phv, orpv);
        if ((s + 1) % kSpm != 0) continue;
        const int minute = (s + 1) / kSpm;
        const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
        if (v.stable && firstStable < 0) firstStable = minute;
        if (firstStable > 0 && firstStable < 40 && v.unstable && recloseAfter < 0) {
          recloseAfter = minute;
        }
      }
      if (firstStable > 0 && firstStable < 40) {
        falseOpensIqm++;
        char msg[128];
        snprintf(msg, sizeof(msg), "dephasage %d : IQM ouvre t+%d, referme t+%d (exposition %d min)",
                 phase, firstStable, recloseAfter, recloseAfter - firstStable);
        // C2(c) : l'exposition ne doit JAMAIS deborder kPurgeWindowMin.
        TEST_ASSERT_TRUE_MESSAGE(recloseAfter > 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(recloseAfter - firstStable <= (int)kW, msg);
        if (recloseAfter - firstStable > maxExposureIqm) maxExposureIqm = recloseAfter - firstStable;
        strncpy(worstMsg, msg, sizeof(worstMsg) - 1);
      }
    }
    // Mediane, canal ombre (meme regles d'agregation pH+ORP que la production).
    {
      Shadow chPh, chOrp;
      int idx = phase;
      int firstStable = -1;
      for (int s = 0; s < 45 * kSpm; ++s) {
        const float m = (float)s / kSpm;
        const float phv = ph0409(m) + resPh[idx % fx.n];
        const float orpv = orp0409(m) + resOrp[idx % fx.n];
        idx++;
        bool unstable = false;
        const bool stable = shadowMedianVerdict(chPh, chOrp, p, phv, orpv, &unstable);
        if ((s + 1) % kSpm != 0) continue;
        const int minute = (s + 1) / kSpm;
        if (stable && firstStable < 0) firstStable = minute;
        (void)unstable;
      }
      if (firstStable > 0 && firstStable < 40) falseOpensMed++;
    }
  }
  char msgB[220];
  snprintf(msgB, sizeof(msgB),
           "C2(b) taux de fausse ouverture sur %d dephasages : IQM %d, mediane %d (exigence IQM<=mediane) — %s",
           fx.n, falseOpensIqm, falseOpensMed, worstMsg);
  TEST_ASSERT_TRUE_MESSAGE(falseOpensIqm <= falseOpensMed, msgB);
  printf("  [info] F6(b) : %s\n", msgB);
  char msgC[160];
  snprintf(msgC, sizeof(msgC), "C2(c) exposition max mesuree %d min (doit rester <= kPurgeWindowMin=%d)",
           maxExposureIqm, (int)kW);
  TEST_ASSERT_TRUE_MESSAGE(maxExposureIqm <= (int)kW, msgC);
  printf("  [info] F6(c) : %s\n", msgC);

  // --- Sentinelle diagnostique (PAS un critere d'acceptation depuis
  // l'amendement) : dominance d'etendue au fond du V, echantillonnee sur 30
  // dephasages regulierement espaces. Un saut a 30/30 signale le mutant (a).
  int dominanceCount = 0, shifts = 0;
  for (int phase = 0; phase < 300; phase += 10) {
    shifts++;
    PurgeStabilityState st;
    purgeStabilityReset(st);
    int idx = phase;
    float medBlocks[kPurgeStabilityMaxWindowMin];
    int medHead = 0, medCount = 0;
    float minuteBuf[kPurgeStabilityMaxSamplesPerMin];
    int minuteCount = 0;
    float medMinRangeInV = 1e9f, iqmMinRangeInV = 1e9f;
    for (int s = 0; s < 20 * kSpm; ++s) {
      const float m = (float)s / kSpm;
      const float phv = ph0409(m) + resPh[idx % fx.n];
      const float orpv = orp0409(m) + resOrp[idx % fx.n];
      idx++;
      purgeStabilityAddSample(st, p, phv, orpv);
      minuteBuf[minuteCount++] = phv;
      if (minuteCount >= kSpm) {
        const float med = purgeStabilityMedian(minuteBuf, (uint8_t)minuteCount);
        minuteCount = 0;
        medBlocks[medHead] = med;
        medHead = (medHead + 1) % kW;
        if (medCount < kW) medCount++;
      }
      if ((s + 1) % kSpm != 0) continue;
      const int minute = (s + 1) / kSpm;
      if (medCount >= kW && minute >= 10 && minute <= 20) {
        float lo = medBlocks[0], hi = medBlocks[0];
        for (int kk = 1; kk < medCount; ++kk) {
          if (medBlocks[kk] < lo) lo = medBlocks[kk];
          if (medBlocks[kk] > hi) hi = medBlocks[kk];
        }
        const float medRange = hi - lo;
        const float iqmRange = purgeChannelRange(st.ph);
        if (medRange < medMinRangeInV) medMinRangeInV = medRange;
        if (iqmRange < iqmMinRangeInV) iqmMinRangeInV = iqmRange;
      }
    }
    if (iqmMinRangeInV >= medMinRangeInV) dominanceCount++;
  }
  char msgD[128];
  snprintf(msgD, sizeof(msgD), "sentinelle dominance (diagnostique) : %d/%d", dominanceCount, shifts);
  TEST_ASSERT_TRUE_MESSAGE(dominanceCount >= 6 && dominanceCount <= 10, msgD);
  printf("  [info] F6(sentinelle) : %s\n", msgD);
}

// -----------------------------------------------------------------------------
// F7 — Plateau POST-OUVERTURE : 0 re-fermeture sur >= 30 dephasages.
// -----------------------------------------------------------------------------
// Distinct de F6 (qui rejoue le fond du V, AVANT plateau) : ici l'eau est deja
// ETABLIE (fixture seule, pas de synthetique 0409), rejouee 60 min (cycle de
// la fixture 300 x 5 s = 25 min, boucle sur elle-meme) a 30 dephasages
// differents. Le bruit REEL, meme repete en boucle, ne doit jamais depasser
// le seuil de re-fermeture (0,30 pH / 30 mV) : mesure sur le code reel,
// 0/30 dephasages refermes.
void test_F7_plateau_post_ouverture_0_refermeture_30_dephasages(void) {
  NoiseFixture fx;
  TEST_ASSERT_TRUE_MESSAGE(loadNoiseFixture(fx),
      "fixture test/fixtures/f097_purge_noise_20260920.csv introuvable ou incomplete (300 attendus)");
  const PurgeStabilityParams p = prodParams();
  int reclosed = 0;
  int shifts = 0;
  char worstMsg[128] = {0};
  for (int phase = 0; phase < 300; phase += 10) {
    shifts++;
    PurgeStabilityState st;
    purgeStabilityReset(st);
    int idx = phase;
    bool sawUnstable = false;
    int firstUnstableMinute = -1;
    for (int s = 0; s < 60 * kSpm; ++s) {
      const float phv = fx.ph[idx % fx.n];
      const float orpv = fx.orp[idx % fx.n];
      idx++;
      purgeStabilityAddSample(st, p, phv, orpv);
      if ((s + 1) % kSpm != 0) continue;
      const int minute = (s + 1) / kSpm;
      const PurgeStabilityVerdict v = purgeStabilityEvaluate(st, p);
      if (v.unstable && !sawUnstable) { sawUnstable = true; firstUnstableMinute = minute; }
    }
    if (sawUnstable) {
      reclosed++;
      snprintf(worstMsg, sizeof(worstMsg), "dephasage %d : instable a t+%d", phase, firstUnstableMinute);
    }
  }
  char msg[192];
  snprintf(msg, sizeof(msg), "%d/%d dephasages refermes sur eau etablie (attendu 0) — %s",
           reclosed, shifts, worstMsg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(30, shifts, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, reclosed, msg);
}

// =============================================================================
// Runner
// =============================================================================
int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  // A — médiane
  RUN_TEST(test_A_mediane_impaire_et_paire);
  RUN_TEST(test_A_nan_exclus_de_la_minute);
  RUN_TEST(test_A_mediane_minute_insensible_a_une_pointe);
  // B — cinématique de la fenêtre
  RUN_TEST(test_B_stable_exactement_a_fenetre_pleine);
  RUN_TEST(test_B_jamais_instable_sur_fenetre_partielle);
  RUN_TEST(test_B_la_marche_sort_de_la_fenetre);
  RUN_TEST(test_B_reset_ramene_a_jamais_lu);
  // C — signaux synthétiques
  RUN_TEST(test_C_plat_bruite_jamais_instable_et_souvent_stable);
  RUN_TEST(test_C_derive_monotone_jamais_stable);
  RUN_TEST(test_C_derive_lente_0015_est_jugee_stable_limite_connue);
  RUN_TEST(test_C_hysteresis_entre_1x_et_2x);
  RUN_TEST(test_C_pointes_isolees_ne_cassent_pas_la_stabilite);
  // D — agrégation des voies
  RUN_TEST(test_D_orp_seul_instable_bloque);
  RUN_TEST(test_D_orp_jamais_lu_est_ignore);
  RUN_TEST(test_D_orp_partiel_bloque_sans_refermer);
  RUN_TEST(test_D_aucune_voie_lue_ni_stable_ni_instable);
  RUN_TEST(test_D_orp_en_pente_3mv_min_jamais_stable);
  RUN_TEST(test_D_orp_en_pente_1_5mv_min_est_juge_stable_limite_connue);
  // E — rejeux réels
  RUN_TEST(test_E_rejeu_0409_pas_stable_avant_la_fin_du_rincage);
  RUN_TEST(test_E_rejeu_0409_sans_bruit_meme_conclusion);
  RUN_TEST(test_E_demonstration_w8_ouvre_a_tort_dans_le_fond_du_v);
  RUN_TEST(test_E_rejeu_0409_5_graines_ouvrent_ou_plafond);
  RUN_TEST(test_E_rejeu_0409_bruit_fausse_ouverture_refermee_par_c1);
  RUN_TEST(test_E_rejeu_0209_plongeon_tardif_declenche_unstable);
  RUN_TEST(test_E_parametres_de_production_dans_les_bornes);
  // F — feature-097 : moyenne interquartile
  RUN_TEST(test_F1_trim_max1_n_sur_4_pour_n_1_a_60);
  RUN_TEST(test_F2_bloc_alterne_reel_minute_22);
  RUN_TEST(test_F3_rejeu_plateau_reel_16_sur_16);
  RUN_TEST(test_F4_aberrantes_0_a_3_inchange_4_decroche_deux_directions);
  RUN_TEST(test_F5_reclose_0035_referme_0030_ne_referme_pas);
  RUN_TEST(test_F6_c2_taux_fausse_ouverture_et_exposition_max);
  RUN_TEST(test_F7_plateau_post_ouverture_0_refermeture_30_dephasages);
  return UNITY_END();
}
