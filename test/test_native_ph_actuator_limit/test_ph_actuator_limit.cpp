// =============================================================================
// Tests unitaires natifs — LIMITE D'ACTIONNEUR pH (feature-086, v2.29.0)
// =============================================================================
// Cible : src/ph_actuator_limit_logic.h, module PUR (ADR-0017) —
// phActuatorLimitStreak (série de journées saturées) et phActuatorUndersized
// (conjonction série ≥ N ET épisode C5 ouvert). Tournent sur PC (env:native,
// Unity), hors ESP32 / Arduino.
//
// L'ENJEU : ce module est du DIAGNOSTIC PUR — aucune de ses sorties n'est
// consommée par une décision de dosage (prouvé par grep, cf. rapport de test).
// Le risque n'est donc pas chimique mais de CRÉDIBILITÉ : une alerte de
// sous-dimensionnement fausse enverrait l'utilisateur changer sa pompe ou
// relever sa limite horaire (donc SON excursion pendant le retard de mélange)
// pour rien. Les deux conditions bloquantes du visa pool-chemistry :
//   1. jamais d'alerte sans épisode C5 ouvert — or anomalyHeldMs (source de
//      anomalyNow dans pump_controller.cpp) exige `errorUsable` (erreur finie)
//      au tick : anomalyNow=false ⇒ jamais vrai, quel que soit le streak ;
//   2. lecture seule stricte (hors de portée d'un test natif : grep).
//
// ORGANISATION
//   A — série nominale, ancrage hier/aujourd'hui, conjonction undersized
//   B — marge de remplissage 80 % (79 / 80 exact / 81)
//   C — ruptures : trou calendaire, jour non exploitable intercalé
//   D — ancrage : dernier jour trop ancien, futur, horloge invalide
//   E — anneau vide, débit soutenable inexploitable
//   F — réarmement par changement de limite (soutenable relevé / baissé)
//   G — saison basse : 90-119 min de circulation ⇒ jamais de série
//   H — arithmétique de dates : série à cheval sur un changement de mois
//   I — caractérisation du prédicat (condition bloquante n°1) + contrat figé
//
// CE QUI RESTE HORS D'ATTEINTE (dit sans détour) : le câblage de
// pump_controller.cpp (rafraîchissement de _phLimitStreakDays à la clôture de
// journée, fourniture de `_phIntegral.anomalyHeldMs > 0`, journalisation par
// transition) et la relecture NVS de l'anneau (config.cpp). Couvert par les
// scénarios d'intégration manuels du rapport.
// =============================================================================

#include <unity.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)

#include "constants.h"
#include "ph_actuator_limit_logic.h"
#include "ph_persist_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Décor commun. Débit soutenable 2,5 mL/min et journées de 400 minutes de
// circulation : le maximum délivrable d'une journée vaut 1000 mL EXACTS en
// float, et le seuil de 80 % tombe sur 800 mL exacts — les frontières se
// testent à l'unité de mL près, sans arrondi parasite.
// -----------------------------------------------------------------------------
static const float    kSust    = 2.5f;
static const uint16_t kMinutes = 400;
static const uint16_t kFullMl  = 1000;  // 100 % du max délivrable (rate = kSust)
static const uint32_t kToday   = 20260830u;

static PhLoadDaySample sample(uint32_t date, uint16_t ml,
                              uint16_t minutes = kMinutes) {
  PhLoadDaySample d;
  d.date = date;
  d.ml = ml;
  d.minutes = minutes;
  return d;
}

// Construit l'anneau par phLoadHistoryAppend (du plus ANCIEN au plus RÉCENT) :
// on teste ainsi contre l'invariant réel « days[N-1] = journée la plus
// récente », pas contre une disposition posée à la main.
static PhLoadHistory ringOf(const PhLoadDaySample* days, uint8_t count) {
  PhLoadHistory h;
  for (uint8_t i = 0; i < count; ++i) phLoadHistoryAppend(h, days[i]);
  return h;
}

// Trois journées PLEINES (100 % du max) finissant la veille de kToday.
static PhLoadHistory ringThreeFullDays() {
  const PhLoadDaySample d[] = {
      sample(20260827u, kFullMl),
      sample(20260828u, kFullMl),
      sample(20260829u, kFullMl),
  };
  return ringOf(d, 3);
}

// =============================================================================
// A — SÉRIE NOMINALE ET CONJONCTION
// =============================================================================

static void test_A_trois_jours_pleins_finissant_hier_streak_3(void) {
  const PhLoadHistory h = ringThreeFullDays();
  TEST_ASSERT_EQUAL_UINT8(3, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_A_undersized_exige_serie_ET_anomalie(void) {
  const PhLoadHistory h = ringThreeFullDays();
  const uint8_t streak = phActuatorLimitStreak(h, kToday, kSust);
  TEST_ASSERT_EQUAL_UINT8(3, streak);
  // Conjonction (livrable A3) : la série seule ne suffit JAMAIS — trois
  // journées pleines avec un pH à la consigne décrivent une installation
  // JUSTE dimensionnée, pas sous-dimensionnée.
  TEST_ASSERT_TRUE(phActuatorUndersized(streak, true));
  TEST_ASSERT_FALSE(phActuatorUndersized(streak, false));
}

static void test_A_ancrage_sur_aujourd_hui_accepte(void) {
  // Journée close datée d'AUJOURD'HUI (recul d'horloge à cheval sur minuit) :
  // l'ancrage l'accepte au même titre qu'hier.
  const PhLoadDaySample d[] = {
      sample(20260828u, kFullMl),
      sample(20260829u, kFullMl),
      sample(20260830u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(3, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_A_sept_jours_pleins_streak_7(void) {
  // Anneau plein : 7 journées consécutives finissant hier (âges 1 à 7, toutes
  // dans la fenêtre d'observation). Le maximum rendu est bien 7.
  const PhLoadDaySample d[] = {
      sample(20260823u, kFullMl), sample(20260824u, kFullMl),
      sample(20260825u, kFullMl), sample(20260826u, kFullMl),
      sample(20260827u, kFullMl), sample(20260828u, kFullMl),
      sample(20260829u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 7);
  TEST_ASSERT_EQUAL_UINT8(7, phActuatorLimitStreak(h, kToday, kSust));
  TEST_ASSERT_TRUE(phActuatorUndersized(7, true));
}

static void test_A_streak_2_jamais_undersized_meme_avec_anomalie(void) {
  const PhLoadDaySample d[] = {
      sample(20260828u, kFullMl),
      sample(20260829u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 2);
  const uint8_t streak = phActuatorLimitStreak(h, kToday, kSust);
  TEST_ASSERT_EQUAL_UINT8(2, streak);
  // N−1 jours : PAS d'alerte, même avec l'épisode C5 ouvert.
  TEST_ASSERT_FALSE(phActuatorUndersized(streak, true));
  TEST_ASSERT_FALSE(phActuatorUndersized(streak, false));
}

// =============================================================================
// B — MARGE DE REMPLISSAGE (kPhActuatorLimitDayFillRatio = 0,80)
// =============================================================================
// Max délivrable d'une journée du décor : 2,5 × 400 = 1000 mL ; seuil = 800 mL.

static void test_B_jour_a_79_pourcent_casse_la_serie(void) {
  // 790 mL = 79 % : la journée la plus ANCIENNE des trois ne compte pas.
  const PhLoadDaySample d[] = {
      sample(20260827u, 790),
      sample(20260828u, kFullMl),
      sample(20260829u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(2, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_B_jour_recent_a_79_pourcent_streak_0(void) {
  // Le même 79 % sur la journée la plus RÉCENTE : la série ne démarre pas.
  const PhLoadDaySample d[] = {
      sample(20260827u, kFullMl),
      sample(20260828u, kFullMl),
      sample(20260829u, 790),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_B_jours_a_81_pourcent_comptent(void) {
  const PhLoadDaySample d[] = {
      sample(20260827u, 810),
      sample(20260828u, 810),
      sample(20260829u, 810),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(3, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_B_frontiere_exacte_80_pourcent_compte(void) {
  // COMPORTEMENT DOCUMENTÉ À L'ÉGALITÉ : la comparaison du module est
  // `ml < ratio × max ⇒ rupture`, donc ml == 800,0 mL EXACTS (0,80 × 1000)
  // PROLONGE la série — « délivré AU niveau du maximum » inclut la borne.
  const PhLoadDaySample d[] = {
      sample(20260827u, 800),
      sample(20260828u, 800),
      sample(20260829u, 800),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(3, phActuatorLimitStreak(h, kToday, kSust));
}

// =============================================================================
// C — RUPTURES : TROU CALENDAIRE, JOUR NON EXPLOITABLE INTERCALÉ
// =============================================================================

static void test_C_trou_calendaire_casse_la_serie(void) {
  // 25, 26, [27 manquant], 28, 29 : la série ne se recolle pas par-dessus le
  // trou — deux journées seulement, même si 25 et 26 sont pleines.
  const PhLoadDaySample d[] = {
      sample(20260825u, kFullMl),
      sample(20260826u, kFullMl),
      sample(20260828u, kFullMl),
      sample(20260829u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 4);
  TEST_ASSERT_EQUAL_UINT8(2, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_C_jour_sous_120_minutes_casse_la_serie(void) {
  // Journée intercalée à 119 min < kPhLoadDayMinFiltrationMin : NON exploitable
  // (elle CASSE la série, elle ne la traverse pas), même si son volume colle à
  // 100 % de SON maximum (2,5 × 119 ≈ 297 mL ⇒ 238 mL = 80 % pile dépassé).
  const PhLoadDaySample d[] = {
      sample(20260827u, kFullMl),
      sample(20260828u, 297, 119),
      sample(20260829u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(1, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_C_debit_implausible_casse_la_serie(void) {
  // 600 mL en 200 min = 3,0 mL/min > 1,10 × 2,5 = 2,75 : la journée annonce
  // plus que l'actionneur ne peut délivrer (dénominateur tronqué) — écartée par
  // le filet phLoadDayRatePlausible, donc rupture, bien que 600 ≥ 80 % de son
  // max nominal (0,80 × 2,5 × 200 = 400 mL) : c'est LA PLAUSIBILITÉ qui tue.
  const PhLoadDaySample d[] = {
      sample(20260827u, kFullMl),
      sample(20260828u, 600, 200),
      sample(20260829u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(1, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_C_jour_hors_fenetre_7j_non_exploitable(void) {
  // Caractérisation du critère de fenêtre (âge 8 > kPhIntegralRestoreHistory-
  // WindowDays = 7 ⇒ non exploitable). NOTE D'ARCHITECTURE : dans la série
  // elle-même ce cas est INATTEIGNABLE par construction — l'anneau n'a que
  // 7 cases et l'ancrage exige hier/aujourd'hui, donc une série consécutive va
  // au plus jusqu'à l'âge 7, dernière borne DANS la fenêtre (cf. test des
  // 7 jours pleins). Le critère reste testé ici car phLoadDayUsable est bien
  // dans le chemin de phActuatorLimitStreak (mutation n°3 du rapport).
  TEST_ASSERT_FALSE(phLoadDayUsable(sample(20260822u, kFullMl), kToday, kSust));
  TEST_ASSERT_TRUE(phLoadDayUsable(sample(20260823u, kFullMl), kToday, kSust));
}

// =============================================================================
// D — ANCRAGE : LE JOUR ARCHIVÉ LE PLUS RÉCENT DOIT DATER D'HIER OU D'AUJOURD'HUI
// =============================================================================

static void test_D_dernier_jour_archive_trop_ancien_streak_0(void) {
  // Série parfaite 26-27-28, mais vue du 30 : le plus récent a 2 jours d'âge.
  // Un appareil resté éteint ne décrit plus le régime courant — série 0.
  const PhLoadDaySample d[] = {
      sample(20260826u, kFullMl),
      sample(20260827u, kFullMl),
      sample(20260828u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_D_jour_archive_dans_le_futur_streak_0(void) {
  // Horloge reculée : la journée la plus récente est datée de DEMAIN.
  const PhLoadDaySample d[] = {
      sample(20260830u, kFullMl),
      sample(20260831u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 2);
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, 20260829u, kSust));
}

static void test_D_horloge_inexploitable_streak_0(void) {
  const PhLoadHistory h = ringThreeFullDays();
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, 0u, kSust));
  // Date illisible (mois 13) : phLoadDayNumber rend 0 ⇒ même fail-closed.
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, 20261301u, kSust));
}

// =============================================================================
// E — ANNEAU VIDE, DÉBIT SOUTENABLE INEXPLOITABLE
// =============================================================================

static void test_E_anneau_vide_streak_0(void) {
  const PhLoadHistory h;  // toutes cases à date = 0
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, kSust));
}

static void test_E_soutenable_inexploitable_streak_0(void) {
  // Pompe non caractérisée ou config non chargée : on n'invente pas un
  // maximum, donc pas de série — quel que soit le contenu de l'anneau.
  const PhLoadHistory h = ringThreeFullDays();
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, 0.0f));
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, -1.0f));
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, NAN));
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, INFINITY));
}

// =============================================================================
// F — RÉARMEMENT PAR CHANGEMENT DE LIMITE (mêmes journées archivées)
// =============================================================================
// L'alerte doit se réarmer d'elle-même quand l'utilisateur change la limite
// horaire ou recalibre la pompe : le soutenable COURANT est un paramètre, les
// journées archivées ne bougent pas.

static void test_F_soutenable_releve_les_ratios_retombent_streak_0(void) {
  // Journées à 1000 mL = 100 % de l'ANCIEN max (2,5 mL/min). Limite relevée à
  // 3,5 mL/min ⇒ max du jour 1400 mL, seuil 1120 mL > 1000 ⇒ plus aucune
  // journée saturée (le débit 2,5 reste plausible : 2,5 ≤ 1,10 × 3,5 = 3,85 —
  // c'est bien la MARGE qui retombe, pas le filet de plausibilité).
  const PhLoadHistory h = ringThreeFullDays();
  TEST_ASSERT_EQUAL_UINT8(3, phActuatorLimitStreak(h, kToday, kSust));
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, 3.5f));
}

static void test_F_soutenable_baisse_jours_implausibles_streak_0(void) {
  // Limite BAISSÉE à 2,0 mL/min : les journées archivées à 2,5 mL/min dépassent
  // le nouveau plafond de vraisemblance (1,10 × 2,0 = 2,2 < 2,5) — le filet
  // phLoadDayRatePlausible les écarte, la série est cassée dès la première.
  const PhLoadHistory h = ringThreeFullDays();
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, 2.0f));
}

// =============================================================================
// G — SAISON BASSE : CIRCULATION SOUS LE MINIMUM D'EXPLOITABILITÉ
// =============================================================================

static void test_G_saison_basse_90_a_119_minutes_streak_0(void) {
  // Journées d'hiver à 90-119 min de circulation, volume collé au max de leurs
  // minutes : TOUTES sous kPhLoadDayMinFiltrationMin (120) ⇒ aucune n'est
  // exploitable ⇒ jamais de série, donc jamais d'alerte hors saison.
  const PhLoadDaySample d[] = {
      sample(20260827u, 225, 90),   // 2,5 × 90 = 225 mL (100 %)
      sample(20260828u, 262, 105),  // 2,5 × 105 = 262,5 mL
      sample(20260829u, 297, 119),  // 2,5 × 119 = 297,5 mL
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(0, phActuatorLimitStreak(h, kToday, kSust));
  // Frontière : 120 min exactement redevient exploitable (≥, pas >).
  const PhLoadDaySample ok[] = {
      sample(20260829u, 300, 120),  // 2,5 × 120 = 300 mL (100 %)
  };
  const PhLoadHistory h2 = ringOf(ok, 1);
  TEST_ASSERT_EQUAL_UINT8(1, phActuatorLimitStreak(h2, kToday, kSust));
}

// =============================================================================
// H — ARITHMÉTIQUE DE DATES : CONSÉCUTIVITÉ À CHEVAL SUR UN CHANGEMENT DE MOIS
// =============================================================================

static void test_H_serie_a_cheval_sur_le_changement_de_mois(void) {
  // 30/08 → 31/08 → 01/09 : la consécutivité passe par phLoadDayNumber
  // (arithmétique civile exacte), pas par la différence des entiers AAAAMMJJ
  // (20260901 − 20260831 = 70 en entier, 1 en jours).
  const PhLoadDaySample d[] = {
      sample(20260830u, kFullMl),
      sample(20260831u, kFullMl),
      sample(20260901u, kFullMl),
  };
  const PhLoadHistory h = ringOf(d, 3);
  TEST_ASSERT_EQUAL_UINT8(3, phActuatorLimitStreak(h, 20260902u, kSust));
}

// =============================================================================
// I — CARACTÉRISATION DU PRÉDICAT (condition bloquante n°1) + CONTRAT FIGÉ
// =============================================================================

static void test_I_anomalie_absente_jamais_undersized_quel_que_soit_streak(void) {
  // CONDITION BLOQUANTE n°1 du visa pool-chemistry : sans épisode C5 ouvert,
  // l'alerte est IMPOSSIBLE, quel que soit le compte de journées. Côté
  // coquille (pump_controller.cpp), anomalyNow = `_phIntegral.anomalyHeldMs
  // > 0`, et anomalyHeldMs n'est chargé par phIntegralTick QUE si
  // `errorUsable` (erreur pH finie) — une mesure inexploitable ne peut donc
  // pas OUVRIR l'anomalie. Balayage exhaustif du domaine utile (0..7 = anneau)
  // plus la borne du type.
  for (unsigned s = 0; s <= 7; ++s) {
    TEST_ASSERT_FALSE(phActuatorUndersized((uint8_t)s, false));
  }
  TEST_ASSERT_FALSE(phActuatorUndersized(255, false));
  // Et symétriquement : l'anomalie seule, sans la série, ne suffit pas.
  TEST_ASSERT_FALSE(phActuatorUndersized(0, true));
  TEST_ASSERT_FALSE(phActuatorUndersized(2, true));
  TEST_ASSERT_TRUE(phActuatorUndersized(3, true));
  TEST_ASSERT_TRUE(phActuatorUndersized(255, true));
}

static void test_I_contrat_numerique_des_constantes(void) {
  // Contrat FIGÉ par la spec (livrable A3) : si l'un de ces seuils bouge, le
  // test doit casser et forcer la relecture du visa pool-chemistry.
  TEST_ASSERT_EQUAL_UINT8(3, kPhActuatorLimitConsecutiveDays);
  TEST_ASSERT_EQUAL_FLOAT(0.80f, kPhActuatorLimitDayFillRatio);
  TEST_ASSERT_EQUAL_UINT8(7, kPhIntegralRestoreHistoryWindowDays);
  TEST_ASSERT_EQUAL_UINT16(120, kPhLoadDayMinFiltrationMin);
}

// =============================================================================

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  // A — série nominale et conjonction
  RUN_TEST(test_A_trois_jours_pleins_finissant_hier_streak_3);
  RUN_TEST(test_A_undersized_exige_serie_ET_anomalie);
  RUN_TEST(test_A_ancrage_sur_aujourd_hui_accepte);
  RUN_TEST(test_A_sept_jours_pleins_streak_7);
  RUN_TEST(test_A_streak_2_jamais_undersized_meme_avec_anomalie);
  // B — marge 80 %
  RUN_TEST(test_B_jour_a_79_pourcent_casse_la_serie);
  RUN_TEST(test_B_jour_recent_a_79_pourcent_streak_0);
  RUN_TEST(test_B_jours_a_81_pourcent_comptent);
  RUN_TEST(test_B_frontiere_exacte_80_pourcent_compte);
  // C — ruptures
  RUN_TEST(test_C_trou_calendaire_casse_la_serie);
  RUN_TEST(test_C_jour_sous_120_minutes_casse_la_serie);
  RUN_TEST(test_C_debit_implausible_casse_la_serie);
  RUN_TEST(test_C_jour_hors_fenetre_7j_non_exploitable);
  // D — ancrage
  RUN_TEST(test_D_dernier_jour_archive_trop_ancien_streak_0);
  RUN_TEST(test_D_jour_archive_dans_le_futur_streak_0);
  RUN_TEST(test_D_horloge_inexploitable_streak_0);
  // E — anneau vide / soutenable inexploitable
  RUN_TEST(test_E_anneau_vide_streak_0);
  RUN_TEST(test_E_soutenable_inexploitable_streak_0);
  // F — réarmement par changement de limite
  RUN_TEST(test_F_soutenable_releve_les_ratios_retombent_streak_0);
  RUN_TEST(test_F_soutenable_baisse_jours_implausibles_streak_0);
  // G — saison basse
  RUN_TEST(test_G_saison_basse_90_a_119_minutes_streak_0);
  // H — dates
  RUN_TEST(test_H_serie_a_cheval_sur_le_changement_de_mois);
  // I — prédicat et contrat
  RUN_TEST(test_I_anomalie_absente_jamais_undersized_quel_que_soit_streak);
  RUN_TEST(test_I_contrat_numerique_des_constantes);
  return UNITY_END();
}
