// =============================================================================
// Tests unitaires natifs — PORTE DE SESSION pH (feature-094, v2.36.0)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
// Cible : `src/ph_session_logic.h` (module PUR, patron ADR-0017) — appelé
// DIRECTEMENT, jamais recopié.
//
// CE QUE CETTE SUITE PROTÈGE
// --------------------------
//   A — phDecisionTick : le signal lent (amorçage, maturation de la fenêtre,
//       rejet des dt aberrants, repliement de millis(), ré-amorçage) ;
//   B — phSessionGate : l'hystérésis sur le signal lent, la prolongation par la
//       durée minimale MURALE, la défense en profondeur pompe-ON ;
//   C — L'INVARIANT ANTI-v2.26.0 : porte et bande morte sortent du MÊME appel et
//       ne peuvent plus diverger (bloc le plus important pour la non-régression) ;
//   D — le plancher de sous-dépassement, qui PRIME sur la durée minimale ;
//   E — le régime de débit (C7) exigé pendant la prolongation ;
//   F — le RÉ-AMORÇAGE après reprise de filtration (aucune décision pendant 10 min) ;
//   G — les GARDES : aucune n'est subordonnée à la durée minimale de session ;
//   H — la comptabilisation de cycle au premier débit réel, et le quota /jour ;
//   J — le REJEU de la série réelle du 03/09/2026 15:00-19:00 (anti-chatter).
//
// ═════════════════════════════════════════════════════════════════════════════
// ⚠️ CE QUI RESTE HORS D'ATTEINTE DU NATIF — À LIRE AVANT DE FAIRE CONFIANCE
// ═════════════════════════════════════════════════════════════════════════════
// Le module pur ne décide QUE de la porte, de la bande morte et du régime. Tout
// le reste est du CÂBLAGE dans `PumpControllerClass::update()`, qui ne compile
// pas en natif (sensors, mqttCfg, esp_task_wdt, LittleFS). Ne sont donc PAS
// couverts ici, et une mutation de ces lignes ne tuerait AUCUN test :
//
//   1. L'ORDRE DE PRODUCTION « gardes AVANT porte ». Le bloc G prouve que chaque
//      garde refuse, et que le verdict de porte est INDIFFÉRENT à cette garde
//      (aucun champ de garde dans PhGateIn, aucun champ de durée dans
//      DoseInputs — les deux décisions sont structurellement disjointes). Il ne
//      prouve PAS que la coquille les compose bien dans cet ordre :
//      `pump_controller.cpp:3132-3159` reste à relire à l'œil.
//   2. L'APPLICATION du plafond de débit C7 (`if (reason == kPhGateHoldCapped)
//      flow = min(flow, phFfFlow)`, pump_controller.cpp:3322-3326). Le bloc E
//      vérifie que le RÉGIME est correctement classé ; le clamp lui-même est une
//      ligne de la coquille.
//   3. Le plafond de session (kPhSessionMaxOnMs / kPhSessionMaxWallMs) et la
//      pause de mélange, évalués APRÈS la porte dans la coquille.
//   4. `tickDosingCycleDay()` : méthode de PumpControllerClass. Le bloc H prouve
//      la CONSÉQUENCE de son absence (evaluateDose ferme tout au quota) et teste
//      le prédicat de rollover 24 h via son jumeau pur `shouldRolloverByMillis`,
//      mais la ligne `state.cyclesToday = 0` reste non couverte.
//   5. La cadence réelle de tick (~5 s) : ici elle est un paramètre.
//
// Le rejeu (bloc J) a sa propre limite, capitale : la série est un enregistrement
// en BOUCLE OUVERTE. Voir l'en-tête de `fixture_ph_20260903.h`.
// =============================================================================

#include <unity.h>
#include <math.h>   // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdio.h>  // snprintf : diagnostic des rejeux et balayages

#include "ph_session_logic.h"
#include "dosing_logic.h"
#include "ph_guard_logic.h"
#include "constants.h"
#include "fixture_ph_20260903.h"

void setUp(void) {}
void tearDown(void) {}

// Valeurs de configuration de l'appareil le 03/09/2026 (journal système) —
// utilisées partout pour que les tests parlent de la MÊME piscine que la fixture.
static const float    kStart   = kPhStartThresholdDefault;   // 0,05
static const float    kStop    = kPhStopThreshold;           // 0,01
static const uint32_t kTauMs   = kPhDecisionWindowMs;        // 600 000 (10 min)
static const uint32_t kMinWall = (uint32_t)kPhSessionMinWallMs;  // 1 800 000 (30 min)
static const uint32_t kTickMs  = 5000;                       // cadence de régulation

// Entrées de porte « session ouverte, tout nominal », à dériver champ par champ.
static PhGateIn openSession(float slowError) {
  PhGateIn in;
  in.wasOpen       = true;
  in.liveError     = slowError;
  in.decisionError = slowError;
  in.decisionReady = true;
  in.startThr      = kStart;
  in.stopThr       = kStop;
  in.sessionOnMs   = 0;
  in.sessionWallMs = 1000;  // session ouverte depuis 1 SECONDE
  in.minSessionMs  = kMinWall;
  return in;
}

static PhGateIn closedSession(float slowError) {
  PhGateIn in = openSession(slowError);
  in.wasOpen       = false;
  in.sessionWallMs = 0;
  return in;
}

// =============================================================================
// A — phDecisionTick : le signal lent
// =============================================================================

void test_A_le_premier_echantillon_arme_sans_autoriser_de_decision(void) {
  PhDecisionFilter st;
  phDecisionTick(st, 7.40f, 1000, kTauMs);
  TEST_ASSERT_EQUAL_FLOAT(7.40f, st.value);
  // La valeur est là, mais la fenêtre est VIDE : décider maintenant reviendrait à
  // décider sur un échantillon unique, c'est-à-dire sur le bruit.
  TEST_ASSERT_FALSE(st.primed);
}

void test_A_la_fenetre_devient_pleine_exactement_a_tau(void) {
  PhDecisionFilter st;
  uint32_t now = 100000;
  phDecisionTick(st, 7.30f, now, kTauMs);
  // Amorçage : warmupMs repart de 0, il faut tau de PLUS.
  const uint32_t ticks = kTauMs / kTickMs;
  for (uint32_t i = 1; i < ticks; ++i) {
    now += kTickMs;
    phDecisionTick(st, 7.30f, now, kTauMs);
    TEST_ASSERT_FALSE(st.primed);  // strictement avant tau : jamais prêt
  }
  now += kTickMs;
  phDecisionTick(st, 7.30f, now, kTauMs);
  TEST_ASSERT_TRUE(st.primed);
}

void test_A_reponse_a_un_echelon_63_pct_a_une_constante_de_temps(void) {
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.00f, now, kTauMs);
  for (uint32_t i = 0; i < kTauMs / kTickMs; ++i) {
    now += kTickMs;
    phDecisionTick(st, 8.00f, now, kTauMs);
  }
  // Premier ordre : 1 - exp(-1) = 0,632. Tolérance large (discrétisation).
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 7.632f, st.value);
}

void test_A_le_bruit_alternatif_est_attenue_d_un_ordre_de_grandeur(void) {
  // Bruit carré ±0,15 (crête-à-crête 15 min mesurée le 03/09) à 5 s : la sortie
  // ne doit plus traverser la bande d'hystérésis. C'est TOUTE la raison d'être
  // du signal lent.
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.30f, now, kTauMs);
  float maxV = -1e9f, minV = 1e9f;
  for (uint32_t i = 0; i < 4000; ++i) {
    now += kTickMs;
    phDecisionTick(st, (i % 2 == 0) ? 7.45f : 7.15f, now, kTauMs);
    if (st.primed) {
      if (st.value > maxV) maxV = st.value;
      if (st.value < minV) minV = st.value;
    }
  }
  char msg[128];
  snprintf(msg, sizeof(msg), "amplitude du signal lent = %.4f (entree 0,300)",
           (double)(maxV - minV));
  TEST_ASSERT_TRUE_MESSAGE((maxV - minV) < 0.01f, msg);
}

void test_A_une_mesure_non_finie_rearme_le_signal(void) {
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.30f, now, kTauMs);
  for (uint32_t i = 0; i < 200; ++i) { now += kTickMs; phDecisionTick(st, 7.30f, now, kTauMs); }
  TEST_ASSERT_TRUE(st.primed);
  phDecisionTick(st, NAN, now + kTickMs, kTauMs);
  TEST_ASSERT_FALSE(st.primed);
  TEST_ASSERT_TRUE(isnan(st.value));
  TEST_ASSERT_EQUAL_UINT32(0, st.warmupMs);
}

void test_A_un_dt_aberrant_rearme_au_lieu_de_reduire_la_moyenne(void) {
  // Un trou d'évaluation donnerait alpha = 1 : la « moyenne » se réduirait au
  // dernier échantillon, `primed` restant vrai — exactement la décision sur bruit
  // que le module supprime.
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.30f, now, kTauMs);
  for (uint32_t i = 0; i < 200; ++i) { now += kTickMs; phDecisionTick(st, 7.30f, now, kTauMs); }
  TEST_ASSERT_TRUE(st.primed);
  phDecisionTick(st, 7.90f, now + kPhIntegralMaxTickDtMs + 1, kTauMs);
  TEST_ASSERT_FALSE(st.primed);
  TEST_ASSERT_TRUE(isnan(st.value));
}

void test_A_un_dt_nul_ne_change_rien(void) {
  PhDecisionFilter st;
  phDecisionTick(st, 7.30f, 50000, kTauMs);
  phDecisionTick(st, 7.30f, 50000 + kTickMs, kTauMs);
  const float v = st.value;
  const uint32_t w = st.warmupMs;
  phDecisionTick(st, 9.00f, 50000 + kTickMs, kTauMs);  // même ms
  TEST_ASSERT_EQUAL_FLOAT(v, st.value);
  TEST_ASSERT_EQUAL_UINT32(w, st.warmupMs);
}

void test_A_tau_nul_rearme_fail_closed(void) {
  PhDecisionFilter st;
  phDecisionTick(st, 7.30f, 1000, 0);
  TEST_ASSERT_FALSE(st.primed);
  TEST_ASSERT_TRUE(isnan(st.value));
}

void test_A_le_repliement_de_millis_ne_rearme_pas(void) {
  // millis() se replie à 24,8 j. Un dt calculé en signé produirait ici un dt
  // négatif → énorme en non signé → ré-amorçage intempestif toutes les 24,8 j.
  PhDecisionFilter st;
  uint32_t now = 0xFFFFFF00u;
  phDecisionTick(st, 7.30f, now, kTauMs);
  for (uint32_t i = 0; i < 200; ++i) { now += kTickMs; phDecisionTick(st, 7.30f, now, kTauMs); }
  TEST_ASSERT_TRUE(st.primed);      // la fenêtre a traversé le repliement
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.30f, st.value);
}

void test_A_la_maturation_sature_sans_replier(void) {
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.30f, now, kTauMs);
  for (uint32_t i = 0; i < 5000; ++i) { now += kTickMs; phDecisionTick(st, 7.30f, now, kTauMs); }
  TEST_ASSERT_TRUE(st.primed);
  TEST_ASSERT_EQUAL_UINT32(kTauMs, st.warmupMs);  // saturé, jamais replié à 0
}

// =============================================================================
// B — phSessionGate : hystérésis sur le signal lent
// =============================================================================

void test_B_verrou_numerique_des_constantes_de_porte(void) {
  // VERROU DE VALEURS. Les autres tests dérivent leurs bornes des constantes :
  // ils vérifient la MÉCANIQUE, pas les nombres. Sans ce test, ramener la durée
  // minimale à 5 min ou la fenêtre lente à 1 min laisserait toute la suite verte.
  // Les valeurs sont celles arbitrées par le gate pool-chemistry (feature-094).
  TEST_ASSERT_EQUAL_UINT32(1800000UL, (uint32_t)kPhSessionMinWallMs);  // 30 min
  TEST_ASSERT_EQUAL_UINT32(600000UL, kPhDecisionWindowMs);             // 10 min
  TEST_ASSERT_EQUAL_FLOAT(0.03f, kPhDoorCloseMarginPh);
  TEST_ASSERT_EQUAL_FLOAT(0.05f, kPhSessionUndershootFloorPh);
  TEST_ASSERT_EQUAL_FLOAT(0.05f, kPhStartThresholdDefault);
  TEST_ASSERT_EQUAL_FLOAT(0.01f, kPhStopThreshold);
}

void test_B_porte_fermee_ouvre_au_dessus_du_seuil_de_demarrage(void) {
  PhGateOut out = phSessionGate(closedSession(kStart + 0.001f));
  TEST_ASSERT_TRUE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateOpened, out.reason);
}

void test_B_porte_fermee_le_seuil_de_demarrage_est_strict(void) {
  PhGateOut out = phSessionGate(closedSession(kStart));
  TEST_ASSERT_FALSE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateClosedIdle, out.reason);
}

void test_B_porte_ouverte_se_maintient_au_dessus_du_point_de_fermeture(void) {
  // Point de fermeture = stopThr - marge = 0,01 - 0,03 = -0,02.
  PhGateOut out = phSessionGate(openSession(kStop - kPhDoorCloseMarginPh + 0.001f));
  TEST_ASSERT_TRUE(out.open);
}

void test_B_le_point_de_fermeture_vaut_bien_moins_002(void) {
  // Verrou numérique : si la marge changeait, ce test le dirait AVANT le terrain.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.02f, kStop - kPhDoorCloseMarginPh);
}

void test_B_sous_le_point_de_fermeture_une_session_jeune_est_prolongee(void) {
  PhGateIn in = openSession(kStop - kPhDoorCloseMarginPh - 0.001f);
  in.sessionWallMs = 1000;  // 1 s
  PhGateOut out = phSessionGate(in);
  TEST_ASSERT_TRUE(out.open);
}

void test_B_sous_le_point_de_fermeture_une_session_mure_se_ferme(void) {
  PhGateIn in = openSession(kStop - kPhDoorCloseMarginPh - 0.001f);
  in.sessionWallMs = kMinWall;
  PhGateOut out = phSessionGate(in);
  TEST_ASSERT_FALSE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateClosedResolved, out.reason);
}

void test_B_la_bascule_de_prolongation_est_exactement_la_duree_minimale(void) {
  PhGateIn in = openSession(kStop - kPhDoorCloseMarginPh - 0.001f);
  in.sessionWallMs = kMinWall - 1;
  TEST_ASSERT_TRUE(phSessionGate(in).open);
  in.sessionWallMs = kMinWall;
  TEST_ASSERT_FALSE(phSessionGate(in).open);
}

void test_B_la_duree_minimale_est_MURALE_pas_pompe_on(void) {
  // Au rapport cyclique mesuré (3-5 %), 30 min de pompe-ON dureraient ~10 h
  // murales : compter en pompe-ON transformerait « 30 min » en « une demi-journée ».
  PhGateIn in = openSession(kStop - kPhDoorCloseMarginPh - 0.001f);
  in.sessionWallMs = 29UL * 60000UL;  // 29 min murales
  in.sessionOnMs   = 0;               // quasi rien de pompe-ON
  TEST_ASSERT_TRUE(phSessionGate(in).open);
  in.sessionWallMs = 31UL * 60000UL;
  TEST_ASSERT_FALSE(phSessionGate(in).open);
}

void test_B_defense_en_profondeur_base_murale_perdue_le_pompe_on_ferme(void) {
  // sessionWallMs = 0 (base de temps inconnue) : la prolongation reste bornée par
  // le temps POMPE-ON, sinon une session sans base murale serait éternelle.
  PhGateIn in = openSession(kStop - kPhDoorCloseMarginPh - 0.001f);
  in.sessionWallMs = 0;
  in.sessionOnMs   = kMinWall;
  TEST_ASSERT_FALSE(phSessionGate(in).open);
}

void test_B_signal_non_pret_ferme_meme_une_session_jeune(void) {
  PhGateIn in = openSession(0.20f);  // erreur ÉNORME
  in.decisionReady = false;
  PhGateOut out = phSessionGate(in);
  TEST_ASSERT_FALSE(out.open);  // fail-closed : sans signal, on ne dose pas
  TEST_ASSERT_EQUAL_UINT8(kPhGateClosedNotReady, out.reason);
}

void test_B_erreur_non_finie_ferme_la_porte(void) {
  PhGateIn in = openSession(NAN);
  TEST_ASSERT_FALSE(phSessionGate(in).open);
  in.decisionError = INFINITY;
  // +inf est fini ? non : isfinite(+inf) == 0 → fermeture.
  TEST_ASSERT_FALSE(phSessionGate(in).open);
}

void test_B_le_seuil_de_demarrage_est_toujours_au_dessus_du_point_de_fermeture(void) {
  // Hystérésis STRICTE : il ne doit exister aucune valeur d'erreur qui ouvre ET
  // ferme. Sans cet écart, la porte oscillerait à chaque échantillon.
  TEST_ASSERT_TRUE(kStart > (kStop - kPhDoorCloseMarginPh));
}

// =============================================================================
// C — INVARIANT ANTI-v2.26.0 : porte et bande morte, un seul point de vérité
// =============================================================================
// Le défaut v2.26.0 (« la session ne se referme jamais ») est né de la DIVERGENCE
// entre la décision de porte et le choix de bande morte, écrits à deux endroits
// de pump_controller.cpp. phSessionGate rend les deux indissociables ; ce bloc
// vérifie que la propriété tient sur TOUT le domaine, pas sur trois cas choisis.

void test_C_la_bande_morte_suit_toujours_l_etat_de_session(void) {
  for (int wo = 0; wo <= 1; ++wo) {
    for (int k = -30; k <= 40; ++k) {
      const float e = 0.01f * (float)k;
      for (int ready = 0; ready <= 1; ++ready) {
        for (int wall = 0; wall <= 1; ++wall) {
          PhGateIn in = openSession(e);
          in.wasOpen       = (wo == 1);
          in.decisionReady = (ready == 1);
          in.sessionWallMs = wall ? kMinWall + 1 : 1000;
          PhGateOut out = phSessionGate(in);
          const float expected = in.wasOpen ? in.stopThr : in.startThr;
          char msg[160];
          snprintf(msg, sizeof(msg),
                   "wasOpen=%d e=%.3f ready=%d wall=%u -> deadband=%.4f attendu %.4f",
                   wo, (double)e, ready, in.sessionWallMs,
                   (double)out.deadband, (double)expected);
          TEST_ASSERT_EQUAL_FLOAT_MESSAGE(expected, out.deadband, msg);
        }
      }
    }
  }
}

void test_C_la_bande_morte_de_session_ouverte_est_la_plus_serree(void) {
  // Une session OUVERTE doit laisser le PI agir PLUS PRÈS de la consigne qu'une
  // session fermée. Si les deux bandes étaient inversées, le PI se tairait à
  // 0,05 alors que la porte ne ferme qu'à −0,02 : session ouverte, PI muet,
  // fermeture suspendue au seul plafond de session — le défaut v2.26.0 en
  // personne, remonté d'un cran.
  const float bandeFermee  = phSessionGate(closedSession(0.0f)).deadband;
  const float bandeOuverte = phSessionGate(openSession(0.0f)).deadband;
  char msg[128];
  snprintf(msg, sizeof(msg), "bande fermee=%.3f bande ouverte=%.3f",
           (double)bandeFermee, (double)bandeOuverte);
  TEST_ASSERT_TRUE_MESSAGE(bandeOuverte < bandeFermee, msg);
}

void test_C_une_ouverture_a_toujours_lieu_HORS_de_la_bande_morte_publiee(void) {
  // Si la porte pouvait ouvrir à une erreur INFÉRIEURE à la bande morte qu'elle
  // publie, le PI serait muet dès l'ouverture : session ouverte, débit nul, la
  // porte ne se refermerait que sur le plafond de session. C'est la moitié
  // « ouverture » du défaut v2.26.0.
  for (int k = -30; k <= 40; ++k) {
    const float e = 0.01f * (float)k;
    PhGateOut out = phSessionGate(closedSession(e));
    if (out.reason == kPhGateOpened) {
      char msg[128];
      snprintf(msg, sizeof(msg), "ouverture a e=%.3f avec bande morte %.3f",
               (double)e, (double)out.deadband);
      TEST_ASSERT_TRUE_MESSAGE(e > out.deadband, msg);
    }
  }
}

void test_C_une_fermeture_resolue_a_toujours_lieu_DANS_la_bande_morte_publiee(void) {
  // Miroir : la porte ne se ferme jamais alors que le PI est encore commandé à
  // agir. L'écart est la marge kPhDoorCloseMarginPh, qui garantit que la
  // fermeture n'est pas un fil du rasoir sur le point de parcage du PI — c'est
  // EXACTEMENT le mécanisme du défaut v2.26.0.
  for (int k = -30; k <= 40; ++k) {
    const float e = 0.01f * (float)k;
    PhGateIn in = openSession(e);
    in.sessionWallMs = kMinWall + 1;  // durée minimale écoulée
    PhGateOut out = phSessionGate(in);
    if (out.reason == kPhGateClosedResolved) {
      char msg[128];
      snprintf(msg, sizeof(msg), "fermeture a e=%.3f, bande morte %.3f, marge %.3f",
               (double)e, (double)out.deadband, (double)kPhDoorCloseMarginPh);
      TEST_ASSERT_TRUE_MESSAGE(e <= (out.deadband - kPhDoorCloseMarginPh), msg);
    }
  }
}

void test_C_le_point_de_parcage_du_PI_n_est_jamais_un_point_de_bascule(void) {
  // Le PI se parque sur SA bande morte (erreur == stopThr en session ouverte).
  // À ce point exact, la porte doit avoir un verdict FRANC (maintien), pas une
  // décision au fil du rasoir : c'est là que v2.26.0 s'était bloqué.
  PhGateIn in = openSession(kStop);
  in.sessionWallMs = kMinWall + 1;
  PhGateOut out = phSessionGate(in);
  TEST_ASSERT_TRUE(out.open);
  // ... mais le débit y est PLAFONNÉ au feedforward (C7) : la session reste
  // ouverte sans continuer d'acidifier au plein débit.
  TEST_ASSERT_EQUAL_UINT8(kPhGateHoldCapped, out.reason);
}

void test_C_le_motif_et_la_porte_ne_se_contredisent_jamais(void) {
  // Contrat d'énumération : chaque motif implique une valeur de `open`.
  for (int wo = 0; wo <= 1; ++wo) {
    for (int k = -30; k <= 40; ++k) {
      for (int ready = 0; ready <= 1; ++ready) {
        for (int wall = 0; wall <= 1; ++wall) {
          PhGateIn in = openSession(0.01f * (float)k);
          in.wasOpen       = (wo == 1);
          in.decisionReady = (ready == 1);
          in.sessionWallMs = wall ? kMinWall + 1 : 1000;
          PhGateOut out = phSessionGate(in);
          const bool motifOuvrant =
              (out.reason == kPhGateOpened || out.reason == kPhGateHoldFull ||
               out.reason == kPhGateHoldCapped || out.reason == kPhGateHoldZero);
          char msg[160];
          snprintf(msg, sizeof(msg), "wasOpen=%d e=%.3f ready=%d wall=%d motif=%u open=%d",
                   wo, (double)(0.01f * (float)k), ready, wall, out.reason, (int)out.open);
          TEST_ASSERT_EQUAL_MESSAGE((int)motifOuvrant, (int)out.open, msg);
        }
      }
    }
  }
}

// =============================================================================
// D — Plancher de sous-dépassement (C6) : prime sur la durée minimale
// =============================================================================

void test_D_un_sous_depassement_franc_ferme_une_session_d_une_seconde(void) {
  PhGateIn in = openSession(-kPhSessionUndershootFloorPh - 0.001f);
  in.sessionWallMs = 1000;  // 1 s
  PhGateOut out = phSessionGate(in);
  TEST_ASSERT_FALSE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateClosedUndershoot, out.reason);
}

void test_D_le_plancher_est_strict_a_moins_005_pile(void) {
  PhGateIn in = openSession(-kPhSessionUndershootFloorPh);
  in.sessionWallMs = 1000;
  PhGateOut out = phSessionGate(in);
  // À -0,05 PILE, on est encore dans la prolongation (comparaison stricte).
  TEST_ASSERT_TRUE(out.open);
  TEST_ASSERT_NOT_EQUAL(kPhGateClosedUndershoot, out.reason);
}

void test_D_le_plancher_ferme_a_toute_duree_de_session(void) {
  const uint32_t durees[] = {0, 1000, 60000, kMinWall - 1, kMinWall, 3600000UL};
  for (unsigned i = 0; i < sizeof(durees) / sizeof(durees[0]); ++i) {
    PhGateIn in = openSession(-0.20f);
    in.sessionWallMs = durees[i];
    char msg[96];
    snprintf(msg, sizeof(msg), "wall=%u ms", durees[i]);
    TEST_ASSERT_FALSE_MESSAGE(phSessionGate(in).open, msg);
  }
}

void test_D_le_plancher_partage_la_valeur_du_latch_defavorable_du_feedforward(void) {
  // Deux organes qui constatent le même événement physique (« le bassin est passé
  // de l'autre côté ») ne doivent pas le constater à deux valeurs différentes.
  TEST_ASSERT_EQUAL_FLOAT(kPhFfAdverseErrorPh, kPhSessionUndershootFloorPh);
}

// =============================================================================
// E — Régime de débit pendant la prolongation (C7)
// =============================================================================
// Sans lui, `flow += phFfFlow` servirait le feedforward AVEUGLÉMENT pendant 30 min
// sur un bassin déjà revenu à la consigne : le seul vrai danger de la durée
// minimale de session.

void test_E_au_dessus_du_seuil_d_arret_le_debit_est_plein(void) {
  PhGateOut out = phSessionGate(openSession(kStop + 0.001f));
  TEST_ASSERT_TRUE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateHoldFull, out.reason);
}

void test_E_sous_le_seuil_d_arret_le_debit_est_plafonne_au_feedforward(void) {
  PhGateOut out = phSessionGate(openSession(kStop));       // frontière incluse
  TEST_ASSERT_EQUAL_UINT8(kPhGateHoldCapped, out.reason);
  out = phSessionGate(openSession(0.001f));
  TEST_ASSERT_EQUAL_UINT8(kPhGateHoldCapped, out.reason);
}

void test_E_sous_la_consigne_le_debit_est_nul_et_la_porte_reste_ouverte(void) {
  PhGateOut out = phSessionGate(openSession(0.0f));        // frontière incluse
  TEST_ASSERT_TRUE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateHoldZero, out.reason);
  out = phSessionGate(openSession(-0.03f));
  TEST_ASSERT_TRUE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateHoldZero, out.reason);
}

void test_E_les_trois_regimes_sont_monotones_en_erreur(void) {
  // Balayage : plein → plafonné → nul → fermeture, dans cet ordre et sans retour.
  int lastRank = -1;  // 3 = plein, 2 = plafonné, 1 = nul, 0 = fermé
  for (int k = 40; k >= -30; --k) {
    PhGateIn in = openSession(0.01f * (float)k);
    in.sessionWallMs = 1000;
    PhGateOut out = phSessionGate(in);
    int rank;
    switch (out.reason) {
      case kPhGateHoldFull:   rank = 3; break;
      case kPhGateHoldCapped: rank = 2; break;
      case kPhGateHoldZero:   rank = 1; break;
      default:                rank = 0; break;
    }
    if (lastRank >= 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "e=%.3f rang %d apres %d", (double)(0.01f * (float)k),
               rank, lastRank);
      TEST_ASSERT_TRUE_MESSAGE(rank <= lastRank, msg);
    }
    lastRank = rank;
  }
  TEST_ASSERT_EQUAL(0, lastRank);  // on finit bien fermé (plancher)
}

// =============================================================================
// F — Ré-amorçage après reprise de filtration (T7)
// =============================================================================

void test_F_apres_rearmement_aucune_decision_avant_dix_minutes(void) {
  PhDecisionFilter st;
  uint32_t now = 1000000;
  // Régime établi, fenêtre pleine.
  phDecisionTick(st, 7.30f, now, kTauMs);
  for (uint32_t i = 0; i < 200; ++i) { now += kTickMs; phDecisionTick(st, 7.30f, now, kTauMs); }
  TEST_ASSERT_TRUE(st.primed);

  phDecisionRearm(st);  // reprise de filtration

  // TRANSITOIRE DE REPRISE, cas DANGEREUX : l'eau stagnante du circuit lit HAUT
  // (7,90 — erreur apparente +0,60, douze fois le seuil d'ouverture) pendant que
  // la boucle se remplit. C'est ce cas-là, et non la montée douce du 04/09, qui
  // discrimine : une porte aveugle au ré-amorçage ouvrirait ici dès le 2e tick et
  // doserait sur une mesure non représentative. La montée réelle 6,254 → 7,35 du
  // 04/09 culminait juste AU seuil : elle aurait innocenté la porte par chance.
  int ouvertures = 0;
  for (uint32_t i = 0; i < (kTauMs / kTickMs); ++i) {
    now += kTickMs;
    phDecisionTick(st, 7.90f, now, kTauMs);
    PhGateIn in = closedSession(st.value - 7.30f);
    in.decisionReady = st.primed;
    if (phSessionGate(in).open) ++ouvertures;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, ouvertures,
      "la porte a decide pendant le remplissage de la fenetre de 10 min");
  TEST_ASSERT_FALSE(st.primed);
  // Un tick de plus et la fenêtre est pleine : le blocage est temporaire, pas un
  // verrou définitif — et la décision reprend alors normalement.
  now += kTickMs;
  phDecisionTick(st, 7.90f, now, kTauMs);
  TEST_ASSERT_TRUE(st.primed);
  PhGateIn apres = closedSession(st.value - 7.30f);
  apres.decisionReady = st.primed;
  TEST_ASSERT_TRUE(phSessionGate(apres).open);
}

void test_F_la_montee_reelle_du_04_09_ne_declenche_rien_non_plus(void) {
  // Le transitoire RÉEL enregistré le 04/09 (6,254 → 7,35 entre 07:12 et 07:52),
  // rejoué tel quel. Il ne prouve pas grand-chose seul (il culmine tout juste au
  // seuil), mais il documente le cas de terrain d'origine.
  PhDecisionFilter st;
  phDecisionRearm(st);
  uint32_t now = 0;
  int ouvertures = 0;
  const uint32_t ticks = 40UL * 60UL * 1000UL / kTickMs;  // 40 min
  for (uint32_t i = 0; i < ticks; ++i) {
    now += kTickMs;
    const float ph = 6.254f + 1.096f * ((float)i / (float)ticks);
    phDecisionTick(st, ph, now, kTauMs);
    PhGateIn in = closedSession(st.value - 7.30f);
    in.decisionReady = st.primed;
    if (phSessionGate(in).open) ++ouvertures;
  }
  TEST_ASSERT_EQUAL_INT(0, ouvertures);
}

void test_F_le_rearmement_ferme_une_session_deja_ouverte(void) {
  PhDecisionFilter st;
  phDecisionRearm(st);
  PhGateIn in = openSession(0.30f);
  in.decisionReady = st.primed;  // faux
  in.sessionWallMs = 1000;       // 1 s : la durée minimale ne s'y oppose pas
  PhGateOut out = phSessionGate(in);
  TEST_ASSERT_FALSE(out.open);
  TEST_ASSERT_EQUAL_UINT8(kPhGateClosedNotReady, out.reason);
}

void test_F_rearmement_remet_tous_les_champs_a_zero(void) {
  PhDecisionFilter st;
  st.value = 7.4f; st.lastMs = 12345; st.warmupMs = 999; st.primed = true;
  phDecisionRearm(st);
  TEST_ASSERT_TRUE(isnan(st.value));
  TEST_ASSERT_EQUAL_UINT32(0, st.lastMs);
  TEST_ASSERT_EQUAL_UINT32(0, st.warmupMs);
  TEST_ASSERT_FALSE(st.primed);
}

// =============================================================================
// G — GARDES : aucune n'est subordonnée à la durée minimale de session
// =============================================================================
// Contrat C1 du gate. La démonstration native tient en deux temps :
//   (a) la garde REFUSE, quelle que soit la durée de session — et elle ne PEUT
//       pas en dépendre : `DoseInputs` ne contient AUCUN champ de durée de
//       session, `PhGateIn` aucun champ de garde. Les deux décisions sont
//       structurellement disjointes ;
//   (b) la COMPOSITION de production (garde ET porte) est fermée pour les trois
//       âges de session qui comptent : 1 s, 29 min, 31 min.
// La composition est modélisée ici en UNE ligne (`allowed && gate.open`) ; l'ordre
// réel est dans pump_controller.cpp et n'est PAS couvert (cf. en-tête).

static DoseInputs nominalPhInputs() {
  DoseInputs in;
  in.watchdogActive = true;
  in.waterPresent = true;
  in.reading = 7.35f;
  in.filterReady = true;
  in.filterState = DoseFilterState::Ready;
  in.filterUnstable = false;
  in.calPoints = 2;
  in.requiredPoints = 2;
  in.stabilizationActive = false;
  in.mixingActive = false;
  in.modeAutomatic = true;
  in.dailyInjectedMl = 100.0f;
  in.maxDailyMl = 2000.0f;
  in.usedMs = 60000UL;
  in.hourlyLimitMs = 600000UL;
  in.cyclesToday = 3;
  in.maxCyclesPerDay = 48;
  in.cyclesLastMin = 0;
  in.maxCyclesPerMin = kMaxDosingCyclesPerMinute;
  in.cyclesLast15Min = 0;
  in.maxCyclesPer15Min = kMaxDosingCyclesPer15Min;
  return in;
}

// La porte dit « ouverte » dans les trois cas testés (erreur lente franche).
static bool compositionOuvre(const DoseInputs& guards, uint32_t sessionWallMs) {
  const DoseDecision d = evaluateDose(guards);
  PhGateIn gin = openSession(0.20f);  // erreur ÉNORME : la porte veut doser
  gin.sessionWallMs = sessionWallMs;
  return d.allowed && phSessionGate(gin).open;
}

static void gardeFermeAToutAge(const DoseInputs& casse, DoseRefusal attendue,
                               const char* nom) {
  const DoseDecision d = evaluateDose(casse);
  char msg[160];
  snprintf(msg, sizeof(msg), "%s : cause %u attendue %u", nom, (unsigned)d.cause,
           (unsigned)attendue);
  TEST_ASSERT_FALSE_MESSAGE(d.allowed, msg);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)attendue, (uint8_t)d.cause, msg);
  // 1 s, 29 min, 31 min : la durée minimale ne DIFFÈRE jamais la fermeture.
  const uint32_t ages[] = {1000, 29UL * 60000UL, 31UL * 60000UL};
  for (unsigned i = 0; i < 3; ++i) {
    snprintf(msg, sizeof(msg), "%s : session de %u ms encore ouverte", nom, ages[i]);
    TEST_ASSERT_FALSE_MESSAGE(compositionOuvre(casse, ages[i]), msg);
  }
  // Contrôle négatif : sans la garde, la composition OUVRE bien — sinon le test
  // passerait pour une raison étrangère à la garde.
  TEST_ASSERT_TRUE(compositionOuvre(nominalPhInputs(), 1000));
}

void test_G_garde_filtration_absente(void) {
  DoseInputs in = nominalPhInputs();
  in.waterPresent = false;
  gardeFermeAToutAge(in, DoseRefusal::FiltrationOff, "filtration absente");
}

void test_G_garde_capteur_instable(void) {
  DoseInputs in = nominalPhInputs();
  in.filterUnstable = true;
  gardeFermeAToutAge(in, DoseRefusal::FilterUnstable, "capteur instable");
}

void test_G_garde_limite_horaire(void) {
  DoseInputs in = nominalPhInputs();
  in.usedMs = in.hourlyLimitMs;  // frontière atteinte
  gardeFermeAToutAge(in, DoseRefusal::HourlyLimit, "limite horaire");
}

void test_G_garde_quota_journalier(void) {
  DoseInputs in = nominalPhInputs();
  in.dailyInjectedMl = in.maxDailyMl;
  gardeFermeAToutAge(in, DoseRefusal::DailyLimit, "quota journalier");
}

void test_G_garde_quota_de_cycles_par_jour(void) {
  DoseInputs in = nominalPhInputs();
  in.cyclesToday = in.maxCyclesPerDay;
  gardeFermeAToutAge(in, DoseRefusal::CyclesPerDay, "cycles/jour");
}

void test_G_garde_watchdog_inactif(void) {
  DoseInputs in = nominalPhInputs();
  in.watchdogActive = false;
  gardeFermeAToutAge(in, DoseRefusal::WatchdogInactive, "watchdog inactif");
}

void test_G_garde_plancher_acide_ferme_a_toute_duree(void) {
  // Plancher acide : `phGuardBlocksBelow` sur le pH CORRIGÉ. La garde ne connaît
  // ni la durée de session ni la porte — elle ne peut donc pas en dépendre.
  const float sousPlancher = kPhFloorCorrected - 0.01f;
  TEST_ASSERT_TRUE(phGuardBlocksBelow(sousPlancher, kPhFloorCorrected, false));
  const uint32_t ages[] = {1000, 29UL * 60000UL, 31UL * 60000UL};
  for (unsigned i = 0; i < 3; ++i) {
    PhGateIn gin = openSession(0.20f);
    gin.sessionWallMs = ages[i];
    const bool bloque = phGuardBlocksBelow(sousPlancher, kPhFloorCorrected, false);
    TEST_ASSERT_FALSE(bloque && phSessionGate(gin).open && !bloque);
    TEST_ASSERT_TRUE(bloque);  // la garde reste armée quel que soit l'âge
  }
  // Hystérésis de relâche : une fois armée, elle ne lâche pas au premier
  // frémissement — donc a fortiori pas sur une session jeune.
  TEST_ASSERT_TRUE(phGuardBlocksBelow(kPhFloorCorrected + 0.01f, kPhFloorCorrected, true));
}

void test_G_garde_butee_brute_ferme_a_toute_duree(void) {
  const float auDessus = kPhCeilingRawHard + 0.01f;
  TEST_ASSERT_TRUE(phGuardBlocksAbove(auDessus, kPhCeilingRawHard, false));
  TEST_ASSERT_TRUE(phGuardBlocksAbove(kPhCeilingRawHard - 0.01f, kPhCeilingRawHard, true));
}

void test_G_la_porte_ignore_totalement_les_gardes(void) {
  // Formulation positive de la disjonction structurelle : à entrées de porte
  // identiques, le verdict est le MÊME que les gardes soient passantes ou non —
  // c'est pour cela que la coquille DOIT les composer, et que la porte ne peut
  // jamais « couvrir » une garde.
  PhGateIn gin = openSession(0.20f);
  gin.sessionWallMs = 1000;
  const PhGateOut a = phSessionGate(gin);
  const PhGateOut b = phSessionGate(gin);
  TEST_ASSERT_EQUAL(a.open, b.open);
  TEST_ASSERT_TRUE(a.open);  // la porte veut doser : seule la garde peut l'arrêter
}

// =============================================================================
// H — Comptabilisation de cycle et quota journalier (T11)
// =============================================================================

// ⚠️ AVERTISSEMENT DE PORTÉE (mis à jour après la revue de code)
// La règle de comptabilisation du pH N'EST PLUS une fonction pure : la revue a
// remplacé l'appel dégénéré `evaluateCycleStartAccounting(_phCycleCounted,
// !_phCycleCounted, phFlowActive)` par son équivalent littéral
// `if (!_phCycleCounted && phFlowActive)` (pump_controller.cpp:3521). Ce bloc en
// teste donc un MODÈLE, pas le code de production : une mutation de cette ligne
// ne tuerait aucun test ici. Ce qui est verrouillé, c'est le COMPORTEMENT attendu
// (un cycle par session, au premier débit réel) — et, ci-dessous, la
// non-régression de `evaluateCycleStartAccounting` que l'ORP continue d'appeler.
struct ComptaSessionPh {
  bool cycleCompte = false;  // _phCycleCounted
  int  cycles      = 0;      // phDosingState.cyclesToday (incréments)

  void tour(bool debitReel) {                    // pump_controller.cpp:3521
    if (!cycleCompte && debitReel) { cycleCompte = true; ++cycles; }
  }
  void fermetureDeSession() { cycleCompte = false; }  // closePhDosingSession /
                                                      // bloc `if (!phActive)`
};

static int cyclesComptesSurSession(const bool* debitParTour, int nbTours) {
  ComptaSessionPh c;
  for (int i = 0; i < nbTours; ++i) c.tour(debitParTour[i]);
  return c.cycles;
}

void test_H_une_session_sans_aucun_debit_ne_compte_aucun_cycle(void) {
  bool debit[200];
  for (int i = 0; i < 200; ++i) debit[i] = false;
  TEST_ASSERT_EQUAL_INT(0, cyclesComptesSurSession(debit, 200));
}

void test_H_une_session_avec_debit_compte_exactement_un_cycle(void) {
  // La porte peut ouvrir la session DES TOURS avant que le PI ne produise un
  // débit (pouls minimal, reconstruction de dt). Le cycle est compté au premier
  // débit RÉEL, une seule fois.
  bool debit[200];
  for (int i = 0; i < 200; ++i) debit[i] = (i >= 12);  // premier débit au 13e tour
  TEST_ASSERT_EQUAL_INT(1, cyclesComptesSurSession(debit, 200));
}

void test_H_un_debit_intermittent_ne_compte_toujours_qu_un_cycle(void) {
  // Le proportionnel de temps alterne pouls et pauses DANS une même session :
  // chaque reprise de pouls ne doit pas être comptée comme un nouveau cycle.
  bool debit[300];
  for (int i = 0; i < 300; ++i) debit[i] = ((i / 10) % 2 == 0) && (i >= 20);
  TEST_ASSERT_EQUAL_INT(1, cyclesComptesSurSession(debit, 300));
}

void test_H_le_temoin_remis_a_zero_autorise_le_cycle_suivant(void) {
  // closePhDosingSession() remet `_phCycleCounted` à false : la session suivante
  // compte SON premier débit.
  ComptaSessionPh c;
  for (int session = 0; session < 3; ++session) {
    c.fermetureDeSession();
    for (int i = 0; i < 20; ++i) c.tour(i >= 5);
  }
  TEST_ASSERT_EQUAL_INT(3, c.cycles);
}

void test_H_bascule_auto_vers_programmee_puis_retour_compte_bien_le_cycle(void) {
  // CHEMIN DE FUITE TROUVÉ EN REVUE DE CODE (bloquant n°1 de feature-094) : une
  // session automatique dont le cycle était DÉJÀ compté, basculée en
  // « Programmée » puis refermée par le bloc `if (!phActive)` — chemin qui
  // n'appelle PAS closePhDosingSession() — laissait `_phCycleCounted` à vrai. La
  // session automatique SUIVANTE ne comptait alors AUCUN cycle : ni
  // `cyclesToday++`, ni `recordDosingCycleStart` (ring anti-rafale), ni log de
  // démarrage. Toute la comptabilité de sécurité devenait aveugle, en silence.
  // Correctif : `_phCycleCounted = false` dans le bloc `if (!phActive)`, qui voit
  // TOUTES les fermetures.
  ComptaSessionPh c;
  for (int i = 0; i < 20; ++i) c.tour(i >= 5);   // session auto : 1 cycle compté
  TEST_ASSERT_EQUAL_INT(1, c.cycles);
  TEST_ASSERT_TRUE(c.cycleCompte);

  c.fermetureDeSession();  // bascule « Programmée » -> bloc `if (!phActive)`
  TEST_ASSERT_FALSE_MESSAGE(c.cycleCompte,
      "temoin non remis a zero : la prochaine session auto ne comptera aucun cycle");

  for (int i = 0; i < 20; ++i) c.tour(i >= 5);   // retour en automatique
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, c.cycles,
      "le cycle de la session automatique suivante n'a pas ete comptabilise");
}

void test_H_evaluateCycleStartAccounting_reste_inchangee_pour_l_ORP(void) {
  // Non-régression : la fonction pure n'est plus appelée par le pH, mais l'ORP
  // continue de l'utiliser (pump_controller.cpp:3991). Sa sémantique — compter
  // ssi (pas déjà actif) ET (démarrage décidé) ET (débit réellement commandé) —
  // ne doit pas avoir bougé.
  TEST_ASSERT_TRUE(evaluateCycleStartAccounting(false, true, true).countCycleStart);
  TEST_ASSERT_FALSE(evaluateCycleStartAccounting(false, true, false).countCycleStart);
  TEST_ASSERT_FALSE(evaluateCycleStartAccounting(false, false, true).countCycleStart);
  TEST_ASSERT_FALSE(evaluateCycleStartAccounting(true, true, true).countCycleStart);
}

void test_H_sans_remise_a_zero_journaliere_le_quota_coupe_la_regulation(void) {
  // CONSÉQUENCE du défaut signalé par le développeur : `cyclesToday` n'était remis
  // à zéro que dans shouldStartDosing, que le pH n'appelle plus. Une fois le quota
  // atteint, evaluateDose refuse TOUT — la régulation pH s'arrête définitivement.
  DoseInputs in = nominalPhInputs();
  in.cyclesToday = in.maxCyclesPerDay;
  TEST_ASSERT_FALSE(evaluateDose(in).allowed);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)DoseRefusal::CyclesPerDay, (uint8_t)evaluateDose(in).cause);
  // Après remise à zéro (ce que fait tickDosingCycleDay), le dosage repart.
  in.cyclesToday = 0;
  TEST_ASSERT_TRUE(evaluateDose(in).allowed);
}

void test_H_le_predicat_de_passage_de_jour_bascule_a_24h(void) {
  // NOTE D'HONNÊTETÉ : `tickDosingCycleDay()` est une méthode de
  // PumpControllerClass et RÉ-ÉCRIT ce prédicat en ligne
  // (`cyclesDayStart == 0 || now - cyclesDayStart >= 86400000UL`). On teste ici
  // son jumeau PUR `shouldRolloverByMillis`, de frontière identique : une
  // divergence future entre les deux ne serait PAS détectée par ce test.
  TEST_ASSERT_FALSE(shouldRolloverByMillis(1000, 1000 + 86399999UL));
  TEST_ASSERT_TRUE(shouldRolloverByMillis(1000, 1000 + 86400000UL));
  // Repliement de millis() : l'arithmétique non signée garde la frontière juste.
  TEST_ASSERT_TRUE(shouldRolloverByMillis(0xFFFF0000u, 0xFFFF0000u + 86400000UL));
}

// =============================================================================
// J — REJEU de la série réelle du 03/09/2026, 15:00 → 19:00
// =============================================================================
// ⚠️ CE QUE CE BLOC DÉMONTRE, ET RIEN DE PLUS : l'ANTI-CHATTER DE LA PORTE.
// La série est un enregistrement en BOUCLE OUVERTE (elle contient déjà l'acide
// réellement injecté ce jour-là). Elle ne peut donc PAS servir à prouver une
// trajectoire de bassin, un temps de descente, ni un taux de service du
// feedforward — trois critères de la spec invalidés pour cette raison.
//
// Cadence 60 s pour un tour de régulation à ~5 s : le rejeu SOUS-ESTIME le
// chatter. Les nombres ci-dessous sont une BORNE OPTIMISTE.

struct RejeuResultat {
  int      ouvertures;
  uint32_t msPorteOuverte;
  uint32_t msTotal;
  uint32_t plusCourteSessionMs;
};

// Blocage d'ordre zéro sur la fixture (les trous de relevé tiennent la dernière
// valeur connue, cf. en-tête de la fixture).
static float echantillonA(uint32_t offsetSec) {
  float v = kPhReplay20260903[0].phFiltered;
  for (int i = 0; i < kPhReplayCount; ++i) {
    if (kPhReplay20260903[i].offsetSec <= offsetSec) v = kPhReplay20260903[i].phFiltered;
    else break;
  }
  return v;
}

// mode 0 = logique v2.33.0 (décision sur la filtrée INSTANTANÉE, hystérésis
//          0,05 / 0,01, aucune durée minimale) — modèle de 3 lignes, VALIDÉ
//          contre la colonne `session` réellement enregistrée (test J1) ;
// mode 1 = feature-094 (phDecisionTick + phSessionGate, code de PRODUCTION).
static RejeuResultat rejeu(int mode) {
  const uint32_t fin = kPhReplay20260903[kPhReplayCount - 1].offsetSec;
  const uint32_t mixMs = (uint32_t)kPhMixingDelayMs;

  PhDecisionFilter st;
  bool ouverte = false;
  uint32_t mixJusquA = 0;      // ms
  uint32_t debutSession = 0;   // ms
  RejeuResultat r = {0, 0, 0, 0xFFFFFFFFu};

  for (uint32_t s = 0; s <= fin; s += (kTickMs / 1000)) {
    const uint32_t nowMs = s * 1000UL;
    const float ph = echantillonA(s);
    phDecisionTick(st, ph, nowMs, kTauMs);
    r.msTotal += kTickMs;

    bool veutOuvrir;
    if (nowMs < mixJusquA) {
      veutOuvrir = false;  // pause de mélange : garde amont, la porte n'est pas consultée
    } else if (mode == 0) {
      const float e = ph - kPhReplayTargetPh;
      veutOuvrir = ouverte ? (e > kStop) : (e > kStart);
    } else {
      PhGateIn in;
      in.wasOpen       = ouverte;
      in.liveError     = ph - kPhReplayTargetPh;
      in.decisionError = st.value - kPhReplayTargetPh;
      in.decisionReady = st.primed;
      in.startThr      = kStart;
      in.stopThr       = kStop;
      // Boucle ouverte : le temps POMPE-ON réel est inconnu (la fixture ne le
      // porte pas). 0 = choix CONSERVATEUR — la prolongation n'est alors bornée
      // que par la durée murale, jamais raccourcie par le second terme.
      in.sessionOnMs   = 0;
      in.sessionWallMs = ouverte ? (nowMs - debutSession) : 0;
      in.minSessionMs  = kMinWall;
      veutOuvrir = phSessionGate(in).open;
    }

    if (veutOuvrir && !ouverte) { ++r.ouvertures; debutSession = nowMs; }
    if (!veutOuvrir && ouverte) {
      const uint32_t duree = nowMs - debutSession;
      if (duree < r.plusCourteSessionMs) r.plusCourteSessionMs = duree;
      mixJusquA = nowMs + mixMs;  // fermeture ⇒ pause de mélange (production)
    }
    ouverte = veutOuvrir;
    if (ouverte) r.msPorteOuverte += kTickMs;
  }
  return r;
}

void test_J1_le_banc_de_rejeu_reproduit_les_7_ouvertures_enregistrees(void) {
  // VALIDATION DU BANC lui-même : rejouer la logique v2.33.0 sur la série doit
  // retrouver le nombre d'ouvertures que l'appareil a RÉELLEMENT effectuées
  // (colonne `session`). Sans ce contrôle, le « après » ne serait comparé à rien.
  //
  // PORTÉE EXACTE DE LA VALIDATION : le NOMBRE d'ouvertures, et lui seul. Le banc
  // ne reproduit PAS le temps d'ouverture cumulé (11,7 % rejoué contre 24,1 %
  // enregistré) : la production force en plus un temps minimum d'INJECTION en
  // pompe-ON (shouldContinueDosingPure) et fait tourner l'ordonnanceur
  // d'impulsions, deux mécanismes hors du périmètre de ce module. Le banc mesure
  // donc le CHATTER, pas le rapport cyclique.
  const RejeuResultat avant = rejeu(0);
  char msg[160];
  snprintf(msg, sizeof(msg), "AVANT (logique v2.33.0 rejouee) = %d ouvertures ; "
           "enregistrees sur l'appareil = %d", avant.ouvertures, kPhReplayRecordedOpenings);
  TEST_ASSERT_EQUAL_INT_MESSAGE(kPhReplayRecordedOpenings, avant.ouvertures, msg);
}

void test_J2_apres_feature_094_au_plus_trois_ouvertures(void) {
  const RejeuResultat avant  = rejeu(0);
  const RejeuResultat apres  = rejeu(1);
  char msg[200];
  snprintf(msg, sizeof(msg),
           "ouvertures de porte sur 03/09 15:00-19:00 : AVANT=%d  APRES=%d  (critere <= 3)",
           avant.ouvertures, apres.ouvertures);
  TEST_MESSAGE(msg);  // publié même au vert : c'est LE chiffre du lot
  TEST_ASSERT_TRUE_MESSAGE(apres.ouvertures <= 3, msg);
  TEST_ASSERT_TRUE_MESSAGE(apres.ouvertures < avant.ouvertures, msg);
}

void test_J3_taux_d_ouverture_mesure_et_publie(void) {
  // ⚠️ CRITÈRE NON DÉMONTRABLE EN BOUCLE OUVERTE — mesuré et publié, PAS asserté
  // à 80 %. La série contient déjà l'acide injecté par les 7 sessions réelles :
  // le signal lent y siège À la consigne (moyenne 7,2999 pour une cible de 7,30),
  // donc la porte de feature-094 n'a aucune raison d'ouvrir. En boucle FERMÉE, la
  // charge du bassin ferait remonter le pH et la porte s'ouvrirait pour une
  // LONGUE session — c'est précisément ce que le rejeu ne peut pas simuler.
  const RejeuResultat avant = rejeu(0);
  const RejeuResultat apres = rejeu(1);
  char msg[220];
  snprintf(msg, sizeof(msg),
           "taux de porte ouverte : enregistre=24,1%% rejeu-AVANT=%.1f%% rejeu-APRES=%.1f%% "
           "(critere terrain >= 80%% NON prouvable en boucle ouverte)",
           100.0 * (double)avant.msPorteOuverte / (double)avant.msTotal,
           100.0 * (double)apres.msPorteOuverte / (double)apres.msTotal);
  TEST_MESSAGE(msg);
  // Ce qui EST vérifiable : le rejeu couvre bien les 4 h annoncées.
  TEST_ASSERT_TRUE(apres.msTotal >= 4UL * 3600UL * 1000UL - 60000UL);
}

void test_J4_aucune_session_hachee_par_le_bruit(void) {
  // Anti-chatter, formulation directe : si la porte s'ouvre, elle ne peut plus se
  // refermer avant la durée minimale, sauf sous-dépassement franc. Le 03/09,
  // l'appareil produisait des sessions de 6 à 10 min ; ici, aucune session courte
  // ne doit exister.
  const RejeuResultat apres = rejeu(1);
  if (apres.plusCourteSessionMs != 0xFFFFFFFFu) {
    char msg[160];
    snprintf(msg, sizeof(msg), "plus courte session apres = %u ms (minimum %u)",
             apres.plusCourteSessionMs, kMinWall);
    TEST_ASSERT_TRUE_MESSAGE(apres.plusCourteSessionMs >= kMinWall, msg);
  } else {
    TEST_ASSERT_EQUAL_INT(0, apres.ouvertures);  // aucune session : cohérent
  }
}

void test_J5_echelon_synthetique_de_030_ouvre_en_moins_de_cinq_minutes(void) {
  // Remplace le critère « temps de descente du matin inchangé ±10 % », invalidé :
  // le 04/09 ne contient AUCUNE descente d'erreur >= 0,2 (la montée 6,254 → 7,35
  // de 07:12 à 07:52 est le transitoire de MESURE de reprise de filtration, pas le
  // bassin). Ici : signal lent déjà mûr à la consigne, puis échelon +0,30 SANS
  // bruit — la porte doit ouvrir en moins de 5 min.
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.30f, now, kTauMs);
  for (uint32_t i = 0; i < 2UL * (kTauMs / kTickMs); ++i) {
    now += kTickMs;
    phDecisionTick(st, 7.30f, now, kTauMs);
  }
  TEST_ASSERT_TRUE(st.primed);

  const uint32_t t0 = now;
  uint32_t ouvertureMs = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < 120; ++i) {  // 10 min de marge
    now += kTickMs;
    phDecisionTick(st, 7.60f, now, kTauMs);  // échelon +0,30
    PhGateIn in = closedSession(st.value - 7.30f);
    in.decisionReady = st.primed;
    if (phSessionGate(in).open) { ouvertureMs = now - t0; break; }
  }
  char msg[128];
  snprintf(msg, sizeof(msg), "ouverture apres %u ms (critere <= 300000)", ouvertureMs);
  TEST_ASSERT_TRUE_MESSAGE(ouvertureMs <= 300000UL, msg);
}

void test_J6_un_bruit_de_meme_amplitude_que_l_echelon_n_ouvre_PAS(void) {
  // Contre-épreuve indispensable de J5 : sans elle, un signal lent réduit à un
  // seul échantillon (mutation M3) passerait J5 haut la main. Ici, un bruit
  // alternatif ±0,30 (donc de crêtes IDENTIQUES à l'échelon de J5) de moyenne
  // nulle ne doit produire AUCUNE ouverture.
  PhDecisionFilter st;
  uint32_t now = 0;
  phDecisionTick(st, 7.30f, now, kTauMs);
  int ouvertures = 0;
  bool ouverte = false;
  for (uint32_t i = 0; i < 3000; ++i) {
    now += kTickMs;
    phDecisionTick(st, (i % 2 == 0) ? 7.60f : 7.00f, now, kTauMs);
    PhGateIn in = openSession(st.value - 7.30f);
    in.wasOpen       = ouverte;
    in.decisionReady = st.primed;
    in.sessionWallMs = 0;
    const bool o = phSessionGate(in).open;
    if (o && !ouverte) ++ouvertures;
    ouverte = o;
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "%d ouvertures sur un bruit de moyenne nulle", ouvertures);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, ouvertures, msg);
}

// =============================================================================

int main(int, char**) {
  UNITY_BEGIN();
  // A — signal lent
  RUN_TEST(test_A_le_premier_echantillon_arme_sans_autoriser_de_decision);
  RUN_TEST(test_A_la_fenetre_devient_pleine_exactement_a_tau);
  RUN_TEST(test_A_reponse_a_un_echelon_63_pct_a_une_constante_de_temps);
  RUN_TEST(test_A_le_bruit_alternatif_est_attenue_d_un_ordre_de_grandeur);
  RUN_TEST(test_A_une_mesure_non_finie_rearme_le_signal);
  RUN_TEST(test_A_un_dt_aberrant_rearme_au_lieu_de_reduire_la_moyenne);
  RUN_TEST(test_A_un_dt_nul_ne_change_rien);
  RUN_TEST(test_A_tau_nul_rearme_fail_closed);
  RUN_TEST(test_A_le_repliement_de_millis_ne_rearme_pas);
  RUN_TEST(test_A_la_maturation_sature_sans_replier);
  // B — hystérésis de porte
  RUN_TEST(test_B_verrou_numerique_des_constantes_de_porte);
  RUN_TEST(test_B_porte_fermee_ouvre_au_dessus_du_seuil_de_demarrage);
  RUN_TEST(test_B_porte_fermee_le_seuil_de_demarrage_est_strict);
  RUN_TEST(test_B_porte_ouverte_se_maintient_au_dessus_du_point_de_fermeture);
  RUN_TEST(test_B_le_point_de_fermeture_vaut_bien_moins_002);
  RUN_TEST(test_B_sous_le_point_de_fermeture_une_session_jeune_est_prolongee);
  RUN_TEST(test_B_sous_le_point_de_fermeture_une_session_mure_se_ferme);
  RUN_TEST(test_B_la_bascule_de_prolongation_est_exactement_la_duree_minimale);
  RUN_TEST(test_B_la_duree_minimale_est_MURALE_pas_pompe_on);
  RUN_TEST(test_B_defense_en_profondeur_base_murale_perdue_le_pompe_on_ferme);
  RUN_TEST(test_B_signal_non_pret_ferme_meme_une_session_jeune);
  RUN_TEST(test_B_erreur_non_finie_ferme_la_porte);
  RUN_TEST(test_B_le_seuil_de_demarrage_est_toujours_au_dessus_du_point_de_fermeture);
  // C — invariant anti-v2.26.0
  RUN_TEST(test_C_la_bande_morte_suit_toujours_l_etat_de_session);
  RUN_TEST(test_C_la_bande_morte_de_session_ouverte_est_la_plus_serree);
  RUN_TEST(test_C_une_ouverture_a_toujours_lieu_HORS_de_la_bande_morte_publiee);
  RUN_TEST(test_C_une_fermeture_resolue_a_toujours_lieu_DANS_la_bande_morte_publiee);
  RUN_TEST(test_C_le_point_de_parcage_du_PI_n_est_jamais_un_point_de_bascule);
  RUN_TEST(test_C_le_motif_et_la_porte_ne_se_contredisent_jamais);
  // D — plancher de sous-dépassement
  RUN_TEST(test_D_un_sous_depassement_franc_ferme_une_session_d_une_seconde);
  RUN_TEST(test_D_le_plancher_est_strict_a_moins_005_pile);
  RUN_TEST(test_D_le_plancher_ferme_a_toute_duree_de_session);
  RUN_TEST(test_D_le_plancher_partage_la_valeur_du_latch_defavorable_du_feedforward);
  // E — régime de débit C7
  RUN_TEST(test_E_au_dessus_du_seuil_d_arret_le_debit_est_plein);
  RUN_TEST(test_E_sous_le_seuil_d_arret_le_debit_est_plafonne_au_feedforward);
  RUN_TEST(test_E_sous_la_consigne_le_debit_est_nul_et_la_porte_reste_ouverte);
  RUN_TEST(test_E_les_trois_regimes_sont_monotones_en_erreur);
  // F — ré-amorçage
  RUN_TEST(test_F_apres_rearmement_aucune_decision_avant_dix_minutes);
  RUN_TEST(test_F_la_montee_reelle_du_04_09_ne_declenche_rien_non_plus);
  RUN_TEST(test_F_le_rearmement_ferme_une_session_deja_ouverte);
  RUN_TEST(test_F_rearmement_remet_tous_les_champs_a_zero);
  // G — gardes
  RUN_TEST(test_G_garde_filtration_absente);
  RUN_TEST(test_G_garde_capteur_instable);
  RUN_TEST(test_G_garde_limite_horaire);
  RUN_TEST(test_G_garde_quota_journalier);
  RUN_TEST(test_G_garde_quota_de_cycles_par_jour);
  RUN_TEST(test_G_garde_watchdog_inactif);
  RUN_TEST(test_G_garde_plancher_acide_ferme_a_toute_duree);
  RUN_TEST(test_G_garde_butee_brute_ferme_a_toute_duree);
  RUN_TEST(test_G_la_porte_ignore_totalement_les_gardes);
  // H — comptabilisation de cycle
  RUN_TEST(test_H_une_session_sans_aucun_debit_ne_compte_aucun_cycle);
  RUN_TEST(test_H_une_session_avec_debit_compte_exactement_un_cycle);
  RUN_TEST(test_H_un_debit_intermittent_ne_compte_toujours_qu_un_cycle);
  RUN_TEST(test_H_le_temoin_remis_a_zero_autorise_le_cycle_suivant);
  RUN_TEST(test_H_bascule_auto_vers_programmee_puis_retour_compte_bien_le_cycle);
  RUN_TEST(test_H_evaluateCycleStartAccounting_reste_inchangee_pour_l_ORP);
  RUN_TEST(test_H_sans_remise_a_zero_journaliere_le_quota_coupe_la_regulation);
  RUN_TEST(test_H_le_predicat_de_passage_de_jour_bascule_a_24h);
  // J — rejeu de la série réelle
  RUN_TEST(test_J1_le_banc_de_rejeu_reproduit_les_7_ouvertures_enregistrees);
  RUN_TEST(test_J2_apres_feature_094_au_plus_trois_ouvertures);
  RUN_TEST(test_J3_taux_d_ouverture_mesure_et_publie);
  RUN_TEST(test_J4_aucune_session_hachee_par_le_bruit);
  RUN_TEST(test_J5_echelon_synthetique_de_030_ouvre_en_moins_de_cinq_minutes);
  RUN_TEST(test_J6_un_bruit_de_meme_amplitude_que_l_echelon_n_ouvre_PAS);
  return UNITY_END();
}
